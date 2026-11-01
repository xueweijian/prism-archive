// PrismArchive -- PrismWin
// Registration-free 7z.dll bridge (blueprint ch. 2.2 / ch. 6.1).
//
// Loads 7z.dll with LoadLibraryW (no registry, no side-by-side manifest),
// drives IInArchive through the raw vtables declared in SevenZipAbi.hpp and
// exposes two sinks for the extraction stream:
//   1. MemoryOutStream -- thumbnails / inline preview, zero disk I/O
//   2. DiskOutStream   -- final physical file, NT long-path aware
// A third sink (IStream pipe for zero-copy drag & drop) lives in
// PrismVirtualDragDrop.hpp.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/PrismCore.hpp"
#include "FormatSniffer.hpp"
#include "SevenZipAbi.hpp"

namespace prism::win {

// ---------------------------------------------------------------------------
// Text conversion helpers
// ---------------------------------------------------------------------------
inline std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

inline std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------------------
// Engine format table (card C26: extension -> CLSID routing)
// ---------------------------------------------------------------------------
// 7z.dll publishes its own handler table through GetHandlerProperty2(): name,
// class id, and the extensions each handler claims. Reading it back is the only
// authoritative way to learn the CLSID of a format that is not in the official
// Guid.txt -- zstd (7-Zip 25.x) and fork-only lz4/brotli handlers live there,
// and a guessed GUID fails CreateObject with no useful diagnostic (CONTRACTS A4).
struct EngineFormatInfo {
  std::string name;                     // as the DLL spells it: "7z", "zstd"
  GUID clsid{};                         // handler class id
  bool hasClsid = false;                // the DLL answered kClassID
  std::vector<std::string> extensions;  // lowercase, no dot: {"zst", "tzst"}
};

// CPP/7zip/Archive/IArchive.h -- NArchive::NHandlerPropID.
enum : PROPID {
  kHandlerPropName = 0,
  kHandlerPropClassId = 1,
  kHandlerPropExt = 2,
  kHandlerPropAddExt = 3,
};

inline std::string LowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// "zst tzst" / "* .tar" / "(.tar)" -> {"zst","tzst","tar"}. Anything that is
// not a plain extension (signature fragments, "~.swf") is dropped: routing on
// a bogus token would hand the wrong handler to a file we can identify.
inline std::vector<std::string> ParseExtensionList(std::string_view raw) {
  std::vector<std::string> out;
  std::string token;
  auto flush = [&out, &token]() {
    std::string ext;
    for (char c : token) {
      if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ext.push_back(c);
    }
    token.clear();
    if (ext.empty() || ext.size() > 10) return;
    for (const std::string& existing : out) {
      if (existing == ext) return;
    }
    out.push_back(ext);
  };
  for (char raw_c : raw) {
    char c = raw_c;
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == ' ' || c == '\t') {
      if (!token.empty()) flush();
    } else {
      token.push_back(c);
    }
  }
  if (!token.empty()) flush();
  return out;
}

// ---------------------------------------------------------------------------
// 7z.dll discovery (card C1: app lifecycle / startup; card C37: portable)
// ---------------------------------------------------------------------------
// The order below is a product decision, not a guess. Two suites assert it --
// Tests/win/test_app_lifecycle.cpp (C1: env wins, probes are last) and
// Tests/win/test_portable_lookup.cpp (C37: the whole ladder against a fake
// PATH) -- so moving a rung here must move it in both:
//   1. PRISM_7Z_DLL            -- explicit override, wins over everything;
//   2. <exe dir>\7z.dll        -- a copy shipped beside the app (portable install);
//   3. HKLM / HKLM(WOW64) / HKCU  SOFTWARE\7-Zip "InstallDir" -- the machine's
//      own answer to "where am I installed", so 7-Zip on D: or E: is found;
//   4. every directory on PATH, in PATH order (C37) -- the "green" 7-Zip that
//      was unpacked into E:\DevTools\7-Zip and never installed. It writes no
//      registry key, so before C37 the only way to reach it was to hand-set
//      PRISM_7Z_DLL; the probes in (6) can never find it (measured 2026-11-01:
//      exit code 0, no artifact, and a box listing three dead ends);
//   5. common per-user install dirs (C37): %LOCALAPPDATA%\Programs\7-Zip (the
//      installer's non-admin layout) and %USERPROFILE%\scoop\apps\7zip\current
//      (scoop's app directory);
//   6. C:\Program Files\7-Zip / C:\Program Files (x86)\7-Zip -- last-resort
//      probes for a registry-less (or copied) install.
// The whole ladder is ENUMERATED (it no longer stops at the first hit) and the
// highest-priority existing file wins. That is what makes the failure report
// complete -- every path tried, with its source -- and what lets a test assert
// the ORDER without depending on what this machine happens to have installed.
// Discovery reports EVERY path it tried: a "7z.dll not found" box without the
// candidate list is undiagnosable, and the machine this shipped from keeps
// 7-Zip on E: (D12: the absence of evidence has to name what was expected).
struct SevenZipDllLookup {
  std::wstring path;    // first candidate that exists (empty: none)
  std::wstring source;  // where 'path' came from: "env" | "app-dir" |
                        // "registry:HKLM" | "registry:HKLM32" | "registry:HKCU" |
                        // "path" | "common:LOCALAPPDATA" | "common:scoop" | "probe"
  std::vector<std::wstring> candidates;        // every path tried, in probe order
  std::vector<std::wstring> candidateSources;  // parallel to 'candidates'
};

inline bool ExistsAsFile(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// <dir>\<name> without std::filesystem so this header stays usable from TUs
// that only link the Win32 side.
inline std::wstring JoinFsPath(const std::wstring& dir, const wchar_t* name) {
  if (dir.empty()) return {};
  const wchar_t last = dir.back();
  std::wstring out = dir;
  if (last != L'\\' && last != L'/') out.push_back(L'\\');
  out += name;
  return out;
}

// The directory the running image lives in. Empty when the query fails (then
// the "copy beside the exe" candidate is skipped rather than guessed).
inline std::wstring ExecutableDirectory() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD written =
        GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0) return {};
    if (written < buffer.size()) {  // not truncated
      buffer.resize(written);
      const size_t slash = buffer.find_last_of(L"\\/");
      return slash == std::wstring::npos ? std::wstring() : buffer.substr(0, slash);
    }
    buffer.resize(buffer.size() * 2);  // a long path: grow and ask again
  }
}

// 7-Zip's own "where am I" value. InstallDir is a REG_SZ that ends with '\'.
inline bool RegistryInstallDir(HKEY root, const wchar_t* subkey, std::wstring* out) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(root, subkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  wchar_t value[MAX_PATH * 2]{};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG rc = RegQueryValueExW(key, L"InstallDir", nullptr, &type,
                                   reinterpret_cast<LPBYTE>(value), &bytes);
  RegCloseKey(key);
  if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return false;
  value[(sizeof(value) / sizeof(value[0])) - 1] = L'\0';
  if (value[0] == L'\0') return false;
  *out = value;
  return true;
}

// Win32 splits PATH on ';' only. Empty entries mean "the current directory" to
// the loader -- never a place to import a DLL from -- so they are dropped, and
// the quotes/stray spaces an "Edit environment variables" box leaves behind are
// trimmed.
inline std::vector<std::wstring> SplitSearchPath(const std::wstring& value) {
  std::vector<std::wstring> out;
  std::size_t start = 0;
  while (start < value.size()) {
    std::size_t end = value.find(L';', start);
    if (end == std::wstring::npos) end = value.size();
    const std::wstring piece = value.substr(start, end - start);
    const std::size_t first = piece.find_first_not_of(L" \t\"");
    if (first != std::wstring::npos) {
      const std::size_t last = piece.find_last_not_of(L" \t\"");
      out.push_back(piece.substr(first, last - first + 1));
    }
    start = end + 1;
  }
  return out;
}

// GetEnvironmentVariableW needs the real size: PATH is allowed to be far longer
// than MAX_PATH. "" means unset (or set to nothing).
inline std::wstring ReadEnvironment(const wchar_t* name) {
  const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) return {};
  std::wstring value(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
  if (written == 0 || written >= needed) return {};
  value.resize(static_cast<std::size_t>(written));
  return value;
}

// Everything Resolve7zDll() reads from the machine, as data. Split out so the
// ORDER can be asserted against a CONTROLLED environment: on a dev box the
// registry answers first, so a suite that could only call Resolve7zDll() could
// never prove that the PATH layer (C37) finds anything at all.
struct SevenZipDllEnvironment {
  std::wstring envOverride;  // PRISM_7Z_DLL, "" = unset
  std::wstring exeDir;       // <exe dir>
  std::vector<std::pair<std::wstring, std::wstring>> registryDirs;  // (InstallDir, source)
  std::wstring pathValue;    // PATH, "" = unset
  std::wstring localAppData; // LOCALAPPDATA
  std::wstring userProfile;  // USERPROFILE
  bool includeProgramFilesProbes = true;
};

inline SevenZipDllEnvironment CurrentSevenZipDllEnvironment() {
  SevenZipDllEnvironment env;
  env.envOverride = ReadEnvironment(L"PRISM_7Z_DLL");
  env.exeDir = ExecutableDirectory();
  // Registry before the Program Files probes: this is the only source that
  // knows about a non-default install drive.
  struct RegProbe {
    HKEY root;
    const wchar_t* subkey;
    const wchar_t* source;
  };
  const RegProbe probes[] = {
      {HKEY_LOCAL_MACHINE, L"SOFTWARE\\7-Zip", L"registry:HKLM"},
      {HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\7-Zip", L"registry:HKLM32"},
      {HKEY_CURRENT_USER, L"SOFTWARE\\7-Zip", L"registry:HKCU"},
  };
  for (const RegProbe& probe : probes) {
    std::wstring installDir;
    if (!RegistryInstallDir(probe.root, probe.subkey, &installDir)) continue;
    env.registryDirs.emplace_back(installDir, probe.source);
  }
  env.pathValue = ReadEnvironment(L"PATH");
  env.localAppData = ReadEnvironment(L"LOCALAPPDATA");
  env.userProfile = ReadEnvironment(L"USERPROFILE");
  return env;
}

// The documented ladder (see the block above), as a pure function of 'env'.
inline SevenZipDllLookup Resolve7zDllFrom(const SevenZipDllEnvironment& env) {
  SevenZipDllLookup lookup;
  auto alreadyListed = [&lookup](const std::wstring& path) {
    // The same file spelled two ways is still one line in the report.
    for (const std::wstring& seen : lookup.candidates) {
      if (CompareStringOrdinal(seen.c_str(), -1, path.c_str(), -1, TRUE) == CSTR_EQUAL) return true;
    }
    return false;
  };
  bool dedupe = true;
  auto accept = [&lookup, &alreadyListed, &dedupe](const std::wstring& path,
                                                   const wchar_t* source) {
    if (path.empty()) return;
    if (dedupe && alreadyListed(path)) return;
    lookup.candidates.push_back(path);
    lookup.candidateSources.push_back(source);
    if (lookup.path.empty() && ExistsAsFile(path)) {
      lookup.path = path;
      lookup.source = source;
    }
  };

  accept(env.envOverride, L"env");                        // 1: explicit override
  accept(JoinFsPath(env.exeDir, L"7z.dll"), L"app-dir");  // 2: shipped beside the app
  for (const auto& entry : env.registryDirs) {            // 3: 7-Zip's own answer
    accept(JoinFsPath(entry.first, L"7z.dll"), entry.second.c_str());
  }
#ifndef PRISM_C37_MUTANT_NO_PATH_SCAN
  for (const std::wstring& dir : SplitSearchPath(env.pathValue)) {  // 4: PATH (C37)
    accept(JoinFsPath(dir, L"7z.dll"), L"path");
  }
#endif
  // 5: common per-user layouts (C37). JoinFsPath rejects an empty parent, so a
  // machine without LOCALAPPDATA/USERPROFILE simply skips the rung.
  accept(JoinFsPath(JoinFsPath(JoinFsPath(env.localAppData, L"Programs"), L"7-Zip"), L"7z.dll"),
         L"common:LOCALAPPDATA");
  const std::wstring scoopApp =
      JoinFsPath(JoinFsPath(JoinFsPath(env.userProfile, L"scoop"), L"apps"), L"7zip");
  accept(JoinFsPath(JoinFsPath(scoopApp, L"current"), L"7z.dll"), L"common:scoop");

  // 6: the last-resort probes are ALWAYS the tail, even when PATH already named
  // the same folder -- the two lines then differ by SOURCE ("path" vs "probe"),
  // which is the honest report of one directory reachable two ways, and C1's
  // suite asserts that the tail is exactly these two.
  dedupe = false;
  if (env.includeProgramFilesProbes) {
    accept(L"C:\\Program Files\\7-Zip\\7z.dll", L"probe");
    accept(L"C:\\Program Files (x86)\\7-Zip\\7z.dll", L"probe");
  }
  return lookup;
}

inline SevenZipDllLookup Resolve7zDll() {
  return Resolve7zDllFrom(CurrentSevenZipDllEnvironment());
}

// ---------------------------------------------------------------------------
// 7z.dll loader
// ---------------------------------------------------------------------------
class SevenZipLibrary {
 public:
  SevenZipLibrary() = default;
  ~SevenZipLibrary() {
    // Deliberately NOT FreeLibrary'd. 7z.dll keeps process-wide state
    // (g_ExternalCodecs in DllExports2.cpp, filled by its SetCodecs export),
    // and handing the module back frees that state while stale pointers
    // survive: load/free/load cycles inside one process make the next
    // SetCodecs read freed memory. AddressSanitizer reports it as
    // heap-use-after-free inside SetCodecs, which looks like our bug and is
    // not. A real archive manager loads the engine once and keeps it; the OS
    // reclaims the image at process exit, so leaking one HMODULE here is the
    // correct trade. (Repeated cross-DLL unload/reload is a 7-Zip hazard, not
    // a supported pattern.)
  }
  SevenZipLibrary(const SevenZipLibrary&) = delete;
  SevenZipLibrary& operator=(const SevenZipLibrary&) = delete;

  bool Load(const std::wstring& dllPath) {
    module_ = LoadLibraryW(dllPath.c_str());
    if (!module_) return false;
    createObject_ =
        reinterpret_cast<abi::FnCreateObject>(GetProcAddress(module_, "CreateObject"));
    getNumberOfFormats_ = reinterpret_cast<abi::FnGetNumberOfFormats>(
        GetProcAddress(module_, "GetNumberOfFormats"));
    getHandlerProperty2_ = reinterpret_cast<abi::FnGetHandlerProperty2>(
        GetProcAddress(module_, "GetHandlerProperty2"));
    setLargePageMode_ =
        reinterpret_cast<abi::FnSetLargePageMode>(GetProcAddress(module_, "SetLargePageMode"));
    if (createObject_) path_ = dllPath;
    // Scan the handler table HERE, not lazily on first Open(): Load() is a
    // single-threaded startup step, while Open() may race with the reader pool
    // (D24). The table is what routes an extension to its CLSID.
    if (createObject_) ScanFormats();
    return createObject_ != nullptr;
  }

  bool loaded() const { return module_ != nullptr && createObject_ != nullptr; }
  const std::wstring& path() const { return path_; }

  // Blueprint ch. 7.2: 2 MB large pages cut TLB misses on big LZMA2 dictionaries.
  void EnableLargePages() {
    if (setLargePageMode_) setLargePageMode_();
  }

  HRESULT CreateObject(const GUID& clsid, const GUID& iid, void** out) const {
    if (!createObject_) return E_NOTIMPL;
    return createObject_(&clsid, &iid, out);
  }

  std::optional<uint32_t> FormatCount() const {
    if (!getNumberOfFormats_) return std::nullopt;
    abi::UInt32 count = 0;
    if (FAILED(getNumberOfFormats_(&count))) return std::nullopt;
    return count;
  }

  // The engine's own handler table (card C26). Scanned once and cached: the
  // table is immutable for the module's lifetime and Open() runs per file.
  // An empty result means the DLL would not answer -- callers fall back to the
  // pinned CLSIDs instead of treating "unknown" as "unsupported".
  const std::vector<EngineFormatInfo>& formats() {
    if (!formatsScanned_) ScanFormats();
    return formats_;
  }

  // Every handler CLSID that claims `extension` (lowercase, no dot), in the
  // DLL's own order. ".rar" legitimately answers twice (Rar and Rar5), and the
  // Rar handler declines a Rar5 archive -- both must be tried.
  std::vector<GUID> ClsidsForExtension(std::string_view extension) {
    std::vector<GUID> out;
    for (const EngineFormatInfo& format : formats()) {
      if (!format.hasClsid) continue;
      for (const std::string& ext : format.extensions) {
        if (ext == extension) {
          out.push_back(format.clsid);
          break;
        }
      }
    }
    return out;
  }

  const EngineFormatInfo* FormatNamed(std::string_view name) {
    const std::string wanted = LowerAscii(std::string(name));
    for (const EngineFormatInfo& format : formats()) {
      if (LowerAscii(format.name) == wanted) return &format;
    }
    return nullptr;
  }

 private:
  std::string HandlerPropString(abi::UInt32 index, PROPID propId) const {
    if (!getHandlerProperty2_) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(getHandlerProperty2_(index, propId, &value)) && value.vt == VT_BSTR &&
        value.bstrVal) {
      out = WideToUtf8(value.bstrVal);
    }
    PropVariantClear(&value);
    return out;
  }

  void ScanFormats() {
    formatsScanned_ = true;
    if (!getNumberOfFormats_ || !getHandlerProperty2_) return;
    abi::UInt32 count = 0;
    if (FAILED(getNumberOfFormats_(&count))) return;
    formats_.reserve(count);
    for (abi::UInt32 i = 0; i < count; ++i) {
      EngineFormatInfo info;
      info.name = HandlerPropString(i, kHandlerPropName);
      if (info.name.empty()) continue;
      PROPVARIANT value{};
      PropVariantInit(&value);
      if (SUCCEEDED(getHandlerProperty2_(i, kHandlerPropClassId, &value)) &&
          value.vt == VT_BSTR && value.bstrVal) {
        // 7-Zip hands the CLSID back as a BSTR whose eight wchar_t ARE the 16
        // GUID bytes -- NOT the "{...}" text form (measured against 7z.dll
        // 25.01, card C26: first bytes 69 0F 17 23 ...). The copy is validated
        // by the 7-Zip GUID prefix instead of SysStringLen(), which keeps this
        // header compilable by the SDK-vocabulary gate (tools/win_stub).
        GUID parsed{};
        std::memcpy(&parsed, value.bstrVal, sizeof(GUID));
        if (parsed.Data1 == 0x23170F69 && parsed.Data2 == 0x40C1 && parsed.Data3 == 0x278A) {
          info.clsid = parsed;
          info.hasClsid = true;
        }
      }
      PropVariantClear(&value);
      info.extensions = ParseExtensionList(HandlerPropString(i, kHandlerPropExt));
      for (const std::string& added :
           ParseExtensionList(HandlerPropString(i, kHandlerPropAddExt))) {
        bool seen = false;
        for (const std::string& ext : info.extensions) {
          if (ext == added) seen = true;
        }
        if (!seen) info.extensions.push_back(added);
      }
      formats_.push_back(std::move(info));
    }
  }

  HMODULE module_ = nullptr;
  std::wstring path_;
  abi::FnCreateObject createObject_ = nullptr;
  abi::FnGetNumberOfFormats getNumberOfFormats_ = nullptr;
  abi::FnGetHandlerProperty2 getHandlerProperty2_ = nullptr;
  abi::FnSetLargePageMode setLargePageMode_ = nullptr;
  std::vector<EngineFormatInfo> formats_;
  bool formatsScanned_ = false;
};

// ---------------------------------------------------------------------------
// C28: open-path I/O accounting
// ---------------------------------------------------------------------------
// The open benchmark asserts BYTES, not seconds (Tests/win/test_open_perf.cpp):
// a wall-clock gate on a shared runner measures the machine, while a byte count
// is reproducible and says exactly what the code did. The counter is
// process-wide because Open() builds its own stream internally -- the test
// snapshots it around one Open() call.
struct OpenIoCounters {
  std::atomic<uint64_t> bytesRead{0};      // every byte the engine pulled through us
  std::atomic<uint64_t> reads{0};          // Read() calls
  std::atomic<uint64_t> sniffBytes{0};     // the part spent identifying the file
  std::atomic<uint64_t> mutantProbeAll{0}; // injection counter (CONTRACTS D3)
  std::atomic<uint64_t> mutantIsoSniffOff{0};  // C43: iso rule deleted (D3)
};

inline OpenIoCounters& IoCounters() {
  static OpenIoCounters counters;
  return counters;
}

inline void ResetIoCounters() {
  IoCounters().bytesRead.store(0, std::memory_order_relaxed);
  IoCounters().reads.store(0, std::memory_order_relaxed);
  IoCounters().sniffBytes.store(0, std::memory_order_relaxed);
  // mutantProbeAll is NOT reset here: it is the D3 injection counter and must
  // accumulate across every Open() the run performed.
}

// ---------------------------------------------------------------------------
// Input stream over a real file (IInStream + IStreamGetSize)
// ---------------------------------------------------------------------------
class InFileStream final : public abi::IInStream, public abi::IStreamGetSize {
 public:
  explicit InFileStream(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size{};
      if (GetFileSizeEx(handle_, &size)) size_ = static_cast<uint64_t>(size.QuadPart);
    }
  }
  ~InFileStream() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  uint64_t size() const { return size_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream || iid == abi::IID_IInStream) {
      *out = static_cast<abi::IInStream*>(this);
    } else if (iid == abi::IID_IStreamGetSize) {
      *out = static_cast<abi::IStreamGetSize*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processed) override {
    DWORD read = 0;
    if (!ReadFile(handle_, data, size, &read, nullptr)) {
      if (processed) *processed = 0;
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (processed) *processed = read;
    IoCounters().bytesRead.fetch_add(read, std::memory_order_relaxed);
    IoCounters().reads.fetch_add(1, std::memory_order_relaxed);
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 origin, abi::UInt64* newPos) override {
    LARGE_INTEGER move{};
    move.QuadPart = offset;
    DWORD method = FILE_BEGIN;
    if (origin == 1) method = FILE_CURRENT;
    if (origin == 2) method = FILE_END;
    if (!SetFilePointerEx(handle_, move, &move, method)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (newPos) *newPos = static_cast<abi::UInt64>(move.QuadPart);
    return S_OK;
  }

  STDMETHODIMP GetSize(abi::UInt64* size) override {
    if (!size) return E_POINTER;
    *size = size_;
    return S_OK;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// C28: the sniff window -- 64 head bytes + the last 1 KiB, read through the
// SAME stream Open() will hand to the engine
// ---------------------------------------------------------------------------
// Reading it here (rather than with a second CreateFileW) keeps the accounting
// honest -- the bytes ARE part of Open's cost -- and guarantees the handler sees
// an untouched file position: every candidate Open is preceded by Seek(0).
// 512, not 64: tar keeps its magic ("ustar") at offset 257 of the first block,
// so a 64-byte head could never see it. One 512-byte block is the natural unit
// and still a rounding error next to any real archive.
constexpr std::size_t kSniffHeadBytes = 512;
constexpr std::size_t kSniffTailBytes = 1024;
// Card C43: ISO9660 keeps its primary volume descriptor at sector 16 -- offset
// 0x8000, with the "CD001" identifier at 0x8001 -- 32 KiB into the file, so a
// 512-byte head can never see it. Eight bytes at 0x8000 are all the rule needs,
// and they are the ONLY extra I/O the sniff adds to a large file (the byte gate
// in Tests/win/test_format_coverage.cpp counts them).
constexpr abi::UInt64 kSniffSector16Offset = 0x8000;
constexpr std::size_t kSniffSector16Bytes = 8;

inline bool ReadSniffWindow(InFileStream& stream, sniff::Window* window,
                            std::vector<uint8_t>* head, std::vector<uint8_t>* tail,
                            std::vector<uint8_t>* sector16) {
  if (!window || !head || !tail || !sector16) return false;
  abi::UInt64 pos = 0;
  if (FAILED(stream.Seek(0, /*STREAM_SEEK_SET=*/0, &pos))) return false;
  head->assign(kSniffHeadBytes, 0);
  abi::UInt32 got = 0;
  if (FAILED(stream.Read(head->data(), static_cast<abi::UInt32>(head->size()), &got))) {
    return false;
  }
  head->resize(got);
  IoCounters().sniffBytes.fetch_add(got, std::memory_order_relaxed);

  // Sector 16 (ISO9660/UDF volume recognition area). Skipped for a file that
  // cannot hold it -- never a short read, never a guess.
  sector16->clear();
  const uint64_t size = stream.size();
  if (size >= kSniffSector16Offset + kSniffSector16Bytes) {
    pos = 0;
    if (FAILED(stream.Seek(static_cast<abi::Int64>(kSniffSector16Offset),
                           /*STREAM_SEEK_SET=*/0, &pos))) {
      return false;
    }
    sector16->assign(kSniffSector16Bytes, 0);
    abi::UInt32 sectorGot = 0;
    if (FAILED(stream.Read(sector16->data(), static_cast<abi::UInt32>(sector16->size()),
                           &sectorGot))) {
      return false;
    }
    sector16->resize(sectorGot);
    IoCounters().sniffBytes.fetch_add(sectorGot, std::memory_order_relaxed);
  }
  if (size <= kSniffHeadBytes) {
    // Tiny file: the head IS the whole file, so the tail is the same bytes.
    *tail = *head;
  } else {
    const uint64_t tailBytes =
        size < kSniffTailBytes ? size : static_cast<uint64_t>(kSniffTailBytes);
    tail->assign(static_cast<std::size_t>(tailBytes), 0);
    pos = 0;
    if (FAILED(stream.Seek(static_cast<abi::Int64>(size - tailBytes), /*SET=*/0, &pos))) {
      return false;
    }
    abi::UInt32 tailGot = 0;
    if (FAILED(stream.Read(tail->data(), static_cast<abi::UInt32>(tail->size()), &tailGot))) {
      return false;
    }
    tail->resize(tailGot);
    IoCounters().sniffBytes.fetch_add(tailGot, std::memory_order_relaxed);
  }

  window->head = head->data();
  window->headSize = head->size();
  window->tail = tail->data();
  window->tailSize = tail->size();
  window->sector16 = sector16->empty() ? nullptr : sector16->data();
  window->sector16Size = sector16->size();
  window->fileSize = size;
  return true;
}

// ---------------------------------------------------------------------------
// Sink 1: bounded in-memory sink (thumbnails / inline preview)
// ---------------------------------------------------------------------------
class MemoryOutStream final : public abi::ISequentialOutStream {
 public:
  explicit MemoryOutStream(size_t capacity) : capacity_(capacity) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream) {
      *out = static_cast<abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    // (No IID_IOutStream here: MemoryOutStream is the in-memory extraction
    // sink; only DiskOutStream -- the physical file -- is seekable/sizable.)
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  // Zip-bomb containment: refuse to grow past the caller's budget instead of
  // letting a 42.zip exhaust the address space (blueprint ch. 7.1).
  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    if (bytes_.size() + size > capacity_) {
      tripped_ = true;
      return E_OUTOFMEMORY;
    }
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
    if (processed) *processed = size;
    return S_OK;
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  bool tripped() const { return tripped_; }

 private:
  size_t capacity_;
  std::vector<uint8_t> bytes_;
  bool tripped_ = false;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Sink 2: final physical file (NT long-path aware, \\?\ prefix)
// ---------------------------------------------------------------------------
// '\'/' -> '\': \?\-prefixed paths get no Win32 normalization.
inline std::wstring ToBackslashes(std::wstring p) {
  for (wchar_t& c : p) {
    if (c == L'/') c = L'\\';
  }
  return p;
}

// Full IOutStream: the 7z UPDATE engine QIs the out stream for IOutStream
// and returns E_NOTIMPL without it (7zUpdate.cpp ~L1927, newer engines);
// Seek/SetSize ride SetFilePointerEx/SetEndOfFile (STREAM_SEEK_* values are
// identical to FILE_*).
class DiskOutStream final : public abi::IOutStream {
 public:
  explicit DiskOutStream(std::wstring finalPath) : path_(std::move(finalPath)) {}

  bool Open() {
    // Blueprint ch. 7.2: \\?\ lifts the MAX_PATH ceiling and allows the Linux
    // reserved names (aux.c, con.txt) that 7-Zip otherwise chokes on.
    // CAVEAT: the \\?\ prefix DISABLES Win32 path normalization, so '/'
    // segments are no longer converted to '\' and CreateFileW fails. Convert
    // separators ourselves (PrismCore's contract is '/' everywhere).
    std::wstring ntPath = ToBackslashes(path_);
    if (ntPath.rfind(L"\\\\?\\", 0) != 0) ntPath.insert(0, L"\\\\?\\");
    handle_ = CreateFileW(ntPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
  }

  uint64_t written() const { return written_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == abi::IID_IOutStream) {
      *out = static_cast<abi::IOutStream*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream) {
      *out = static_cast<abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
      if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
      delete this;
    }
    return r;
  }

  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (handle_ == INVALID_HANDLE_VALUE) return E_FAIL;
    DWORD done = 0;
    if (!WriteFile(handle_, data, size, &done, nullptr)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    written_ += done;
    if (processed) *processed = done;
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 seekOrigin,
                    abi::UInt64* newPosition) override {
    if (handle_ == INVALID_HANDLE_VALUE) return E_FAIL;
    if (seekOrigin > STREAM_SEEK_END) return STG_E_INVALIDFUNCTION;
    LARGE_INTEGER move{};
    move.QuadPart = offset;
    LARGE_INTEGER newPos{};
    if (!SetFilePointerEx(handle_, move, &newPos,
                          static_cast<DWORD>(seekOrigin))) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (newPosition) *newPosition = static_cast<abi::UInt64>(newPos.QuadPart);
    return S_OK;
  }

  STDMETHODIMP SetSize(abi::UInt64 newSize) override {
    if (handle_ == INVALID_HANDLE_VALUE) return E_FAIL;
    LARGE_INTEGER saved{};
    if (!SetFilePointerEx(handle_, LARGE_INTEGER{}, &saved, FILE_CURRENT)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    LARGE_INTEGER end{};
    end.QuadPart = static_cast<LONGLONG>(newSize);
    if (!SetFilePointerEx(handle_, end, nullptr, FILE_BEGIN) || !SetEndOfFile(handle_)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    LARGE_INTEGER restore{};
    restore.QuadPart = saved.QuadPart;
    if (!SetFilePointerEx(handle_, restore, nullptr, FILE_BEGIN)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    return S_OK;
  }

 private:
  std::wstring path_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  uint64_t written_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Open callback (also doubles as the password provider)
// ---------------------------------------------------------------------------
class OpenCallback final : public abi::IArchiveOpenCallback, public abi::ICryptoGetTextPassword {
  // (IArchiveOpenCallback is IUnknown-based; ICryptoGetTextPassword is queried
  //  separately when a password is needed.)
 public:
  explicit OpenCallback(std::wstring password = {}) : password_(std::move(password)) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || IsOpenCallbackIid(iid)) {
      *out = static_cast<abi::IArchiveOpenCallback*>(this);
    } else if (iid == abi::IID_ICryptoGetTextPassword) {
      *out = static_cast<abi::ICryptoGetTextPassword*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // 7-Zip families its IIDs as {23170F69-40C1-278A-0000-00KK00MM0000}. We accept
  // the whole 0x05 (callback) family minus the extract-callback and password
  // slots, so a single wrong IID constant can never break Open() silently.
  static bool IsOpenCallbackIid(REFIID iid) {
    if (iid == abi::IID_IArchiveOpenCallback) return true;
    if (iid.Data1 != 0x23170F69 || iid.Data2 != 0x40C1 || iid.Data3 != 0x278A) return false;
    // Exact per Guid.txt: IArchiveOpenCallback = family 06 member 10,
    // IProgress = family 00 member 05.
    const BYTE fam = iid.Data4[3], member = iid.Data4[5];
    return (fam == 0x06 && member == 0x10) || (fam == 0x00 && member == 0x05);
  }

  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  // IArchiveOpenCallback only -- five vtable slots in total. The one-pointer
  // IProgress overloads must NOT be declared here: the real interface does not
  // derive IProgress (IDecl.h), so they would shift every later slot.
  STDMETHODIMP SetTotal(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }

  STDMETHODIMP CryptoGetTextPassword(BSTR* password) override {
    if (!password) return E_POINTER;
    if (password_.empty()) return E_ABORT;  // unknown -> let the UI prompt
    *password = SysAllocString(password_.c_str());
    return *password ? S_OK : E_OUTOFMEMORY;
  }

 private:
  std::wstring password_;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Per-item extraction result (7-Zip's NArchive::NExtract::NOperationResult)
// ---------------------------------------------------------------------------
// IInArchive::Extract() answers S_OK even when an item could not be decoded:
// the verdict arrives out of band, in SetOperationResult(opResult). The pre-C26
// code looked only at the HRESULT, so a .7z compressed with a method this
// 7z.dll does not implement (zstd) was reported as a successful extraction of
// zero bytes -- the file on disk was empty and nothing said why
// (2026-11-01, card C17 / C26).
//
// The numeric values are 7-Zip's (CPP/7zip/Archive/IArchive.h):
enum class OpResult : abi::Int32 {
  kOk = 0,
  kUnsupportedMethod = 1,
  kDataError = 2,
  kCrcError = 3,
  kUnavailable = 4,
  kUnexpectedEnd = 5,
  kDataAfterEnd = 6,
  kIsNotArc = 7,
  kHeadersError = 8,
  kWrongPassword = 9,
  kUnknown = -1,
};

inline const char* ToString(OpResult result) {
  switch (result) {
    case OpResult::kOk: return "kOk";
    case OpResult::kUnsupportedMethod: return "kUnsupportedMethod";
    case OpResult::kDataError: return "kDataError";
    case OpResult::kCrcError: return "kCrcError";
    case OpResult::kUnavailable: return "kUnavailable";
    case OpResult::kUnexpectedEnd: return "kUnexpectedEnd";
    case OpResult::kDataAfterEnd: return "kDataAfterEnd";
    case OpResult::kIsNotArc: return "kIsNotArc";
    case OpResult::kHeadersError: return "kHeadersError";
    case OpResult::kWrongPassword: return "kWrongPassword";
    case OpResult::kUnknown: break;
  }
  return "kUnknown";
}

inline std::ostream& operator<<(std::ostream& os, OpResult result) {
  return os << ToString(result);
}

// Negative control (CONTRACTS D3): a build that reproduces the pre-C26 defect
// on purpose -- the engine's per-item verdict is read and thrown away, so an
// unsupported method looks exactly like a 0-byte success. The hit counter is
// what proves the injection actually fired: a mutation whose source no longer
// matches must not be able to look "sharp" by staying green.
#ifdef PRISM_C26_MUTANT_IGNORE_OPRESULT
inline int& MutantIgnoredOpResultHits() {
  static int hits = 0;
  return hits;
}
#endif

// C41 negative control (CONTRACTS D3): a build with the one-operation-per-handler
// guard compiled out. The suite must see the second operation ACCEPTED on a
// handler that is already busy, and only then print "C41 MUTANT CAUGHT" -- the
// counter is the proof the injection fired, not just that the checks went red.
#ifdef PRISM_C41_MUTANT_NO_OPERATION_GUARD
inline std::atomic<uint64_t>& MutantOperationGuardBypasses() {
  static std::atomic<uint64_t> bypasses{0};
  return bypasses;
}
#endif

// ---------------------------------------------------------------------------
// Extract callback: smart path rewriting + Zip Slip guard + bomb fuse
// ---------------------------------------------------------------------------
class SmartExtractCallback final : public abi::IArchiveExtractCallback {
 public:
  struct Result {
    uint64_t filesWritten = 0;
    uint64_t bytesWritten = 0;
    bool guardTripped = false;
    bool fuseTripped = false;
    std::string guardReason;
    std::string fuseReason;
    std::vector<std::string> writtenPaths;
    // C26: per-item engine verdicts. Extract() returns S_OK while items fail,
    // so "0 bytes written, no error" used to be indistinguishable from an empty
    // archive. failedItems is the fact; failureReason is the actionable text.
    uint64_t failedItems = 0;
    OpResult firstFailure = OpResult::kOk;  // engine code of the FIRST failure
    std::vector<uint32_t> failedIndices;
    std::vector<std::string> failedPaths;
    std::string failureReason;  // empty when nothing failed
    // C41: a second operation was attempted on this handler while one was
    // already running. Refused, not allowed to corrupt the first one.
    bool operationConflict = false;
    // C41: the engine answered S_OK but handed out no stream at all, so the
    // operation would have "succeeded" by writing 0 bytes. That is the silent
    // data-loss shape, so ExtractWithPlan reports it as a failure.
    bool emptyResult = false;
    bool failed() const { return failedItems != 0; }
  };

  // The engine reports a failure as a bare number; naming the compression
  // method needs kpidMethod from the archive, which only ArchiveReader holds.
  // Called at most once per FAILED item -- never on the success path.
  using MethodNameLookup = std::function<std::string(uint32_t index)>;

  SmartExtractCallback(const std::vector<ArchiveItem>& catalog, const SmartPlan& plan,
                       const std::string& targetRoot, BombFuse* fuse = nullptr,
                       MethodNameLookup methodLookup = {},
                       std::vector<uint32_t> requestedIndices = {})
      : catalog_(catalog),
        plan_(plan),
        guard_(targetRoot),
        fuse_(fuse),
        methodLookup_(std::move(methodLookup)),
        requestedIndices_(std::move(requestedIndices)) {}

  const Result& result() const { return result_; }
  void SetFreeDiskBytes(uint64_t freeBytes) { freeDiskBytes_ = freeBytes; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
      *out = static_cast<abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP SetTotal(abi::UInt64 total) override {
    total_ = total;
    return S_OK;
  }

  STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
    if (completeValue && fuse_) {
      // The fuse is a sliding-window monitor, so it must be fed deltas.
      const uint64_t produced = *completeValue;
      const uint64_t deltaPacked = packedSoFar_ - fedPacked_;
      const uint64_t deltaProduced = produced > fedProduced_ ? produced - fedProduced_ : 0;
      fedPacked_ = packedSoFar_;
      fedProduced_ = produced;
      if (!fuse_->Feed(deltaPacked, deltaProduced, freeDiskBytes_)) {
        result_.fuseTripped = true;
        result_.fuseReason = fuse_->reason();
        return E_ABORT;
      }
    }
    return S_OK;
  }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialOutStream** outStream,
                         abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != abi::kExtract) return S_OK;
    if (index >= catalog_.size()) return E_INVALIDARG;

    const ArchiveItem& item = catalog_[index];
    if (item.isDir) {
      currentPath_.clear();
      return S_OK;
    }
    if (plan_.ignoredIndices.count(item.index)) return S_OK;  // Mac junk / promo

    // PrismCore decides the final path; the Zip Slip guard re-checks it after
    // rewriting (blueprint ch. 7.1 defence #2, two-stage validation).
    const std::string finalPath = SmartExtractionPlanner::TransformItemPath(plan_, item.path);
    if (!guard_.Approve(finalPath)) {
      result_.guardTripped = true;
      result_.guardReason = "blocked out-of-root write: " + finalPath;
      return E_ABORT;
    }

    packedSoFar_ += item.packSize ? item.packSize : 1;
    const std::wstring wide = Utf8ToWide(finalPath);
    CreateParentDirectories(wide);
    auto* sink = new DiskOutStream(wide);
    if (!sink->Open()) {
      delete sink;
      result_.guardTripped = true;
      result_.guardReason = "cannot create " + finalPath;
      return E_ABORT;
    }
    currentPath_ = finalPath;
    currentIndex_ = item.index;
    // Self-healing cursor: whatever PrepareOperation guessed, a real GetStream
    // tells us the exact index, so the next unattributed failure starts after
    // this one.
    while (nextRequested_ < requestedIndices_.size() &&
           requestedIndices_[nextRequested_] != item.index) {
      ++nextRequested_;
    }
    if (nextRequested_ < requestedIndices_.size()) ++nextRequested_;
    // TWO references are needed and they are different things:
    //   * outStream  -- handed to 7z, which OWNS it and will Release it once
    //                   the item is written;
    //   * currentSink_ -- ours, purely so SetOperationResult can read
    //                   written() before the engine drops the stream.
    // Handing the same single reference to both (sink refs=1, out param, no
    // AddRef) meant 7z freed the object and currentSink_ dangled:
    // AddressSanitizer reported heap-use-after-free on exactly this path.
    sink->AddRef();          // ours
    currentSink_ = sink;
    *outStream = sink;       // engine's (the one created by operator new)
    return S_OK;
  }

  // An item whose method the engine cannot create a coder for NEVER reaches
  // GetStream -- 7-Zip reports it straight through PrepareOperation +
  // SetOperationResult(kUnsupportedMethod). Without this cursor the failure
  // would be attributed to whatever item was decoded last (measured on 7z.dll
  // 25.01, card C26), so the actionable message would name the wrong file.
  STDMETHODIMP PrepareOperation(abi::Int32) override {
    if (currentPath_.empty() && nextRequested_ < requestedIndices_.size()) {
      currentIndex_ = requestedIndices_[nextRequested_];
      ++nextRequested_;
    }
    return S_OK;
  }

  // THE contract this card exists for: opResult is the engine's per-item
  // verdict and it is NOT part of the Extract() HRESULT. A non-zero value means
  // "this item did not decode" -- typically kUnsupportedMethod for an archive
  // compressed with a method the loaded 7z.dll does not implement.
  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
#ifdef PRISM_C26_MUTANT_IGNORE_OPRESULT
    ++MutantIgnoredOpResultHits();
    operationResult = 0;  // the pre-C26 defect, injected on purpose
#endif
    if (operationResult == 0) {
      if (!currentPath_.empty()) {
        ++result_.filesWritten;
        result_.writtenPaths.push_back(currentPath_);
        if (currentSink_) result_.bytesWritten += currentSink_->written();
      }
    } else {
      RecordFailure(operationResult);
    }
    // Drop OUR reference (see GetStream); the engine drops its own when it is
    // done with the stream. Without this the AddRef above would leak.
    if (currentSink_) currentSink_->Release();
    currentSink_ = nullptr;
    currentPath_.clear();
    currentIndex_ = 0;
    return S_OK;
  }

 private:
  // Naming an unknown coder: 7-Zip reports it by its hex method id. 04F71101 is
  // zstd (7-Zip Zstandard), the method that produced this card's fixture.
  static std::string FriendlyMethodName(const std::string& raw) {
    if (raw == "04F71101" || raw == "04f71101") return "zstd (04F71101)";
    return raw;
  }

  static std::string DescribeFailure(OpResult code, const std::string& path,
                                     const std::string& methodRaw) {
    const std::string item = path.empty() ? std::string("<unknown item>") : path;
    const std::string method = FriendlyMethodName(methodRaw);
    std::string head;
    switch (code) {
      case OpResult::kUnsupportedMethod:
        head = "unsupported compression method";
        if (!method.empty()) head += " '" + method + "'";
        head +=
            ": this 7z.dll cannot decode it -- install 7-Zip ZS (zstd/lz4/brotli)"
            " or update the engine";
        break;
      case OpResult::kWrongPassword:
        head = "wrong password (or an encrypted header this engine cannot read)";
        break;
      case OpResult::kDataError:
        head = "the archive data is corrupt";
        break;
      case OpResult::kCrcError:
        head = "CRC mismatch -- the extracted data is not what the archive claims";
        break;
      case OpResult::kUnexpectedEnd:
        head = "the archive ends before the item does (truncated download?)";
        break;
      case OpResult::kUnavailable:
        head = "the engine reports this item as unavailable";
        break;
      case OpResult::kDataAfterEnd:
        head = "unexpected data after the end of the archive";
        break;
      case OpResult::kIsNotArc:
        head = "the engine no longer recognises this file as an archive";
        break;
      case OpResult::kHeadersError:
        head = "the archive header is damaged";
        break;
      default:
        head = "engine reported per-item result " +
               std::to_string(static_cast<int>(code)) + " (" + ToString(code) + ")";
        break;
    }
    return head + " [item '" + item + "']";
  }

  // The item an unattributed failure belongs to. When the engine skipped
  // GetStream there is no final on-disk path yet, but the catalog entry still
  // names the file the user asked for -- which is what the message must say.
  const std::string& FailedItemLabel() const {
    if (!currentPath_.empty()) return currentPath_;
    static const std::string kUnknown = "<unknown item>";
    if (currentIndex_ < catalog_.size()) return catalog_[currentIndex_].path;
    return kUnknown;
  }

  void RecordFailure(abi::Int32 operationResult) {
    ++result_.failedItems;
    const OpResult code = static_cast<OpResult>(operationResult);
    if (result_.failedItems == 1) {  // the first failure is the headline
      result_.firstFailure = code;
      result_.failureReason = DescribeFailure(
          code, FailedItemLabel(),
          methodLookup_ ? methodLookup_(currentIndex_) : std::string());
    }
    result_.failedIndices.push_back(currentIndex_);
    result_.failedPaths.push_back(FailedItemLabel());
  }

  static void CreateParentDirectories(const std::wstring& fullPathRaw) {
    const std::wstring fullPath = ToBackslashes(fullPathRaw);
    for (size_t i = 0; i < fullPath.size(); ++i) {
      if (fullPath[i] != L'\\' && fullPath[i] != L'/') continue;
      if (i < 3) continue;  // skip "C:\"
      const std::wstring part = fullPath.substr(0, i);
      CreateDirectoryW((L"\\\\?\\" + part).c_str(), nullptr);
    }
  }

  const std::vector<ArchiveItem>& catalog_;
  SmartPlan plan_;
  WriteGuard guard_;
  BombFuse* fuse_ = nullptr;
  Result result_;
  std::string currentPath_;
  uint32_t currentIndex_ = 0;  // catalog index of the item being decoded
  MethodNameLookup methodLookup_;
  // Indices handed to Extract(), in the engine's reporting order. PrepareOperation
  // consumes this cursor when GetStream never runs (unsupported method).
  std::vector<uint32_t> requestedIndices_;
  size_t nextRequested_ = 0;
  DiskOutStream* currentSink_ = nullptr;
  uint64_t packedSoFar_ = 0;
  uint64_t fedPacked_ = 0;
  uint64_t fedProduced_ = 0;
  uint64_t freeDiskBytes_ = 0;
  abi::UInt64 total_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Extension -> handler routing (card C26)
// ---------------------------------------------------------------------------
// Before C26, Open() brute-forced two handlers (Zip, 7z) and nothing else, so a
// bare .zst / .tar / .gz could never open even on an engine that supports them
// (7-Zip 25.x decodes .zst natively -- a capability we got for free the moment
// the extension was routed instead of probed). The table below is the contract:
// these extensions are routed. The CLSID itself comes from the engine's own
// handler table when it claims the extension (authoritative, and the only way
// to learn fork-only GUIDs); pinnedClsid is the fallback for formats whose
// class ids are hard-coded in SevenZipAbi.hpp. Anything else -- unknown
// extension, or a routed handler that declines -- falls back to the pre-C26
// "try the candidates in turn" loop.
//
// Pinned handler CLSIDs, pattern {23170F69-40C1-278A-1000-000110xx0000}
// (7-Zip Guid.txt). Tests/win/test_extract_failure.cpp cross-checks every one
// of these against the engine's kClassID table, so a typo here is a red test,
// not a silent degradation to the probe path.
constexpr GUID kClsidBzip2 = {0x23170F69, 0x40C1, 0x278A,
                              {0x10, 0x00, 0x00, 0x01, 0x10, 0x02, 0x00, 0x00}};
constexpr GUID kClsidRar = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0x03, 0x00, 0x00}};
constexpr GUID kClsidXz = {0x23170F69, 0x40C1, 0x278A,
                           {0x10, 0x00, 0x00, 0x01, 0x10, 0x0C, 0x00, 0x00}};
constexpr GUID kClsidTar = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0xEE, 0x00, 0x00}};
constexpr GUID kClsidGzip = {0x23170F69, 0x40C1, 0x278A,
                             {0x10, 0x00, 0x00, 0x01, 0x10, 0xEF, 0x00, 0x00}};
// Card C43. Every GUID below was READ BACK from this machine's 7z.dll through
// GetNumberOfFormats/GetHandlerProperty2 (60 handlers, 2026-11-02), never
// recalled: handler index 03 Arj, 04 nothing, 06 Lzh, 08 Cab, E4 Dmg, E5
// Compound, E6 wim, E7 Iso, E9 Chm, ED Cpio. The full dump is quoted in
// Tests/win/test_format_coverage.cpp.
constexpr GUID kClsidArj = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0x04, 0x00, 0x00}};
constexpr GUID kClsidLzh = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0x06, 0x00, 0x00}};
constexpr GUID kClsidCab = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0x08, 0x00, 0x00}};
constexpr GUID kClsidDmg = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0xE4, 0x00, 0x00}};
constexpr GUID kClsidCompound = {0x23170F69, 0x40C1, 0x278A,
                                 {0x10, 0x00, 0x00, 0x01, 0x10, 0xE5, 0x00, 0x00}};
constexpr GUID kClsidWim = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0xE6, 0x00, 0x00}};
constexpr GUID kClsidIso = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0xE7, 0x00, 0x00}};
constexpr GUID kClsidChm = {0x23170F69, 0x40C1, 0x278A,
                            {0x10, 0x00, 0x00, 0x01, 0x10, 0xE9, 0x00, 0x00}};
constexpr GUID kClsidCpio = {0x23170F69, 0x40C1, 0x278A,
                             {0x10, 0x00, 0x00, 0x01, 0x10, 0xED, 0x00, 0x00}};

struct FormatRoute {
  const wchar_t* extension;  // lowercase, with the dot: L".7z"
  const char* handler;       // engine handler name: diagnostics + intent
  const GUID* pinnedClsid;   // nullptr: engine-only (fork formats: zstd/lz4/br)
};

// ".ZST" -> L".zst"; a dot in a directory name is not an extension.
inline std::wstring PathExtensionLower(std::wstring_view path) {
  const size_t dot = path.find_last_of(L'.');
  if (dot == std::wstring_view::npos) return {};
  const size_t separator = path.find_last_of(L"\\/");
  if (separator != std::wstring_view::npos && dot < separator) return {};
  std::wstring extension(path.substr(dot));
  for (wchar_t& c : extension) {
    if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
  }
  return extension;
}

inline const FormatRoute* FindFormatRoute(std::wstring_view path) {
  static const FormatRoute kRoutes[] = {
      {L".7z", "7z", &abi::CLSID_CFormat7z},
      {L".zip", "zip", &abi::CLSID_CFormatZip},
      {L".rar", "rar", &kClsidRar},
      {L".tar", "tar", &kClsidTar},
      {L".gz", "gzip", &kClsidGzip},
      {L".tgz", "gzip", &kClsidGzip},
      {L".bz2", "bzip2", &kClsidBzip2},
      {L".tbz2", "bzip2", &kClsidBzip2},
      {L".xz", "xz", &kClsidXz},
      {L".txz", "xz", &kClsidXz},
      {L".zst", "zstd", nullptr},
      {L".tzst", "zstd", nullptr},
      {L".lz4", "lz4", nullptr},
      {L".br", "br", nullptr},
      // Card C43: the "common formats" the C26 table did not cover. The handler
      // names are the engine's own (see the dump in test_format_coverage.cpp),
      // and the CLSIDs are read back from it, not recalled (CONTRACTS A4).
      {L".iso", "Iso", &kClsidIso},
      {L".cab", "Cab", &kClsidCab},
      {L".wim", "wim", &kClsidWim},
      {L".swm", "wim", &kClsidWim},
      {L".esd", "wim", &kClsidWim},
      {L".ppkg", "wim", &kClsidWim},
      {L".msi", "Compound", &kClsidCompound},
      {L".msp", "Compound", &kClsidCompound},
      {L".chm", "Chm", &kClsidChm},
      {L".chi", "Chm", &kClsidChm},
      {L".chq", "Chm", &kClsidChm},
      {L".chw", "Chm", &kClsidChm},
      {L".cpio", "Cpio", &kClsidCpio},
      {L".arj", "Arj", &kClsidArj},
      {L".lzh", "Lzh", &kClsidLzh},
      {L".lha", "Lzh", &kClsidLzh},
      {L".dmg", "Dmg", &kClsidDmg},
  };
  const std::wstring extension = PathExtensionLower(path);
  if (extension.empty()) return nullptr;
  for (const FormatRoute& route : kRoutes) {
    if (extension == route.extension) return &route;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// ArchiveReader: central-directory snapshot + the two extraction entry points
// ---------------------------------------------------------------------------
class ArchiveReader {
 public:
  ArchiveReader(SevenZipLibrary& library, ITranscoder* transcoder = nullptr)
      : library_(library), transcoder_(transcoder) {}

  ~ArchiveReader() {
    if (archive_) {
      archive_->Close();
      archive_->Release();
    }
    if (stream_) stream_->Release();
  }

  bool Open(const std::wstring& archivePath, const std::wstring& password = {}) {
    auto hr = [this](const char* what, HRESULT code) {
      char buf[80];
      std::snprintf(buf, sizeof(buf), "%s hr=0x%08lX; ", what,
                    static_cast<unsigned long>(code));
      openTrace_ += buf;
      return code;
    };
    stream_ = new InFileStream(archivePath);
    if (!stream_->valid()) {
      openTrace_ = "CreateFileW failed; ";
      return false;
    }

    // C26: route by extension first. The extension decides WHICH handlers are
    // worth asking; the engine's own table decides their CLSIDs. The old
    // two-handler probe survives as the fallback below, so a mislabeled file
    // (a .zip named .7z, an archive with no extension at all) still opens.
    //
    // C28: the FILE now speaks before the name does. A 512-byte head, the last
    // 1 KiB and (C43) 8 bytes at sector 16 identify the container, and a format
    // proven by its own magic gets exactly ONE handler -- no candidate walks a
    // 4 GiB archive to discover it is the wrong one.
    // C43 tightens that: for a PROVEN format the extension route and the legacy
    // probe are no longer queued behind the sniffed handler. They used to be,
    // for a "what if the sniff guessed wrong" that a signature cannot express --
    // and the price was measured: the [Zip, 7z] probe reads an entire 8 MiB file
    // before declining one whose bytes already said "cab". A proven format now
    // fails by NAME instead (failureReason()/unsupportedFormat()). kHint and
    // kNone keep the full route + probe list, so a mislabeled or prefix-wrapped
    // file still opens.
    const std::wstring extension = PathExtensionLower(archivePath);
    const FormatRoute* route = FindFormatRoute(archivePath);
    openTrace_ += "ext=" + (extension.empty() ? std::string("<none>") : WideToUtf8(extension));
    // The C26 trace token stays verbatim ("route=<handler>" / "route=probe"):
    // win.extract_failure asserts on it, and a diagnostic format is a contract
    // for every test that reads it. C28's own line is appended separately below.
    openTrace_ += route ? (" route=" + std::string(route->handler) + "; ")
                        : std::string(" route=probe; ");

    struct Candidate {
      GUID clsid;
      const char* label;
    };
    std::vector<Candidate> candidates;
    auto addCandidate = [&candidates](const GUID& clsid, const char* label) {
      for (const Candidate& existing : candidates) {
        // memcmp, not IsEqualGUID: this header must stay compilable by the
        // SDK-vocabulary gate (tools/check_windows_tu.sh), which does not
        // declare every guiddef.h helper.
        if (std::memcmp(&existing.clsid, &clsid, sizeof(GUID)) == 0) return;
      }
      candidates.push_back({clsid, label});
    };

    failureReason_.clear();
    unsupportedFormat_.clear();

    sniff::Window sniffWindow;
    std::vector<uint8_t> sniffHead;
    std::vector<uint8_t> sniffTail;
    std::vector<uint8_t> sniffSector16;
    sniff::Finding finding;
    if (ReadSniffWindow(*stream_, &sniffWindow, &sniffHead, &sniffTail, &sniffSector16)) {
      sniffWindow.extensionHint = WideToUtf8(extension.substr(1));
      finding = sniff::Sniff(sniffWindow);
    }
#ifdef PRISM_C43_MUTANT_NO_ISO_SNIFF
    // CONTRACTS D3: the injection counter. It rises ONLY for a file that really
    // carries the ISO9660 descriptor -- i.e. exactly when the deleted rule would
    // have fired. A mutation that never runs cannot look sharp by staying green.
    if (sniffWindow.sector16 &&
        sniff::BytesAre(sniffWindow.sector16, sniffWindow.sector16Size, 1,
                        {'C', 'D', '0', '0', '1'})) {
      IoCounters().mutantIsoSniffOff.fetch_add(1, std::memory_order_relaxed);
    }
#endif
#ifdef PRISM_C28_MUTANT_PROBE_ALL
    // The pre-C28 defect, injected on purpose (CONTRACTS D3): throw the sniff
    // away, so the extension route and the legacy [Zip, 7z] probe decide again
    // -- exactly what a file cost before this card. The counter is the proof
    // the injection FIRED: a mutation that never runs would make its red
    // verdict meaningless.
    IoCounters().mutantProbeAll.fetch_add(1, std::memory_order_relaxed);
    finding = sniff::Finding{};
    finding.evidence = "mutant: sniff ignored";
#endif
    openTrace_ += sniff::DescribeRoute(finding) + " [" + finding.evidence + "]; ";

    // C43: a format proven by its own magic gets ONE answer, not a search.
    // When the sniff says kMagic the container's bytes are the fact; asking any
    // other handler afterwards can only re-read the file to learn the same
    // thing -- measured on this engine, the legacy [Zip, 7z] probe walks a whole
    // 8 MiB file before declining (Tests/win/test_format_coverage.cpp). So the
    // extension route and the probe stay in the list only for kHint/kNone, and
    // a proven format that cannot be opened fails EXPLICITLY, by name.
    bool sniffIsFinal = false;
    if (finding.singleHandler()) {
      sniffIsFinal = true;
      const char* formatExt = sniff::FormatExtension(finding.format);
      const char* handler = sniff::FormatName(finding.format);
      const std::vector<GUID> fromEngine =
          formatExt ? library_.ClsidsForExtension(formatExt) : std::vector<GUID>{};
      for (const GUID& clsid : fromEngine) addCandidate(clsid, handler);
      if (fromEngine.empty() && formatExt) {
        const FormatRoute* sniffRoute = FindFormatRoute(std::wstring(1, L'.') +
                                                        Utf8ToWide(formatExt));
        if (sniffRoute && sniffRoute->pinnedClsid) addCandidate(*sniffRoute->pinnedClsid, handler);
      }
      if (candidates.empty()) {
        // The bytes name a format this 7z.dll has no handler for (a fork-only
        // codec, or an engine without the format). Say it, name it, and do not
        // probe: "no handler" is the answer, not a reason to read the file again.
        unsupportedFormat_ = handler;
        failureReason_ = std::string("unsupported format '") + handler +
                         "': this 7z.dll has no handler for it";
        openTrace_ += std::string("unsupported=") + handler + " reason=no-handler; ";
        return false;
      }
    }

    if (!sniffIsFinal && route) {
      const std::vector<GUID> fromEngine = library_.ClsidsForExtension(WideToUtf8(extension.substr(1)));
      for (const GUID& clsid : fromEngine) addCandidate(clsid, route->handler);
      if (fromEngine.empty()) {
        if (route->pinnedClsid) {
          addCandidate(*route->pinnedClsid, route->handler);
        } else {
          openTrace_ += "(engine claims no handler for this extension) ";
        }
      }
    }
    // The pre-C26 candidate list, always last: unknown extensions land here by
    // definition, and so does a routed handler that declined the file. A
    // sniff-proven format never reaches it (C43).
    if (!sniffIsFinal) {
      addCandidate(abi::CLSID_CFormatZip, "Zip");
      addCandidate(abi::CLSID_CFormat7z, "7z");
    }

    for (const Candidate& slot : candidates) {
      abi::IInArchive* candidate = nullptr;
      const char* name = slot.label;
      if (FAILED(hr(name, library_.CreateObject(slot.clsid, abi::IID_IInArchive,
                                                reinterpret_cast<void**>(&candidate)))) ||
          !candidate) {
        openTrace_ += "(no object) ";
        continue;
      }
      auto* callback = new OpenCallback(password);
      // A failed Open may leave the stream mid-file (the Zip handler reads
      // the header before rejecting): rewind so the next candidate sees the
      // real magic bytes, not the leftovers.
      abi::UInt64 newPos = 0;
      stream_->Seek(0, /*STREAM_SEEK_SET=*/0, &newPos);
      const HRESULT result = candidate->Open(stream_, nullptr, callback);
      callback->Release();
      std::string tag = std::string(name) + ".Open";
      // 7-Zip convention: S_OK = mine, S_FALSE = "not my format, try next".
      // SUCCEEDED(S_FALSE) is TRUE -- treating it as success silently bound
      // the Zip handler to .7z files and yielded empty catalogs. Only S_OK
      // accepts a handler.
      hr(tag.c_str(), result);
      if (result == S_OK) {
        archive_ = candidate;
        openTrace_ += "OK ";
        DiscoverPropertyIds();
        return true;
      }
      candidate->Release();
    }
    if (sniffIsFinal) {
      // Proven format, handler refused the file (truncated, corrupt, or an
      // image this handler cannot parse). The probe would read the whole file
      // to reach the same verdict, so the failure names the format instead.
      const char* handler = sniff::FormatName(finding.format);
      failureReason_ = std::string("the '") + handler + "' handler rejected this file: its bytes " +
                       "identify it as " + handler + ", but this 7z.dll cannot read this one";
      openTrace_ += std::string("declined=") + handler + " reason=handler-rejected; ";
    }
    return false;
  }

  // Diagnostics for the last Open() attempt (printed by the integration test).
  const std::string& openTrace() const { return openTrace_; }
  // C43: the EXPLICIT verdict for a file whose own bytes prove a format this
  // build cannot open. 'unsupportedFormat' is set only when the engine has no
  // handler at all; 'failureReason' explains both that case and a handler that
  // rejected the file. Empty after a successful Open().
  const std::string& unsupportedFormat() const { return unsupportedFormat_; }
  const std::string& failureReason() const { return failureReason_; }
  const std::string& propTrace() const { return propTrace_; }

  // Release the archive and the file handle *before* the caller deletes the
  // temp directory: Windows refuses to unlink a file with an open handle, and
  // std::filesystem::remove_all would then throw.
  void Close() {
    if (archive_) {
      archive_->Close();
      archive_->Release();
      archive_ = nullptr;
    }
    if (stream_) {
      stream_->Release();
      stream_ = nullptr;
    }
    openTrace_.clear();
    propTrace_.clear();
    failureReason_.clear();
    unsupportedFormat_.clear();
  }

  bool opened() const { return archive_ != nullptr; }

  // Blueprint ch. 2.2: a 50 000-entry directory tree in about a millisecond,
  // with zero payload decompressed.
  std::vector<ArchiveItem> ReadCatalog() const {
    std::vector<ArchiveItem> items;
    if (!archive_) return items;
    abi::UInt32 count = 0;
    if (FAILED(archive_->GetNumberOfItems(&count))) return items;

    const PROPID pidPath = PropertyId("Path", abi::kpidPath);
    const PROPID pidIsDir = PropertyId("IsDir", abi::kpidIsDir);
    const PROPID pidSize = PropertyId("Size", abi::kpidSize);
    const PROPID pidPack = PropertyId("PackSize", abi::kpidPackSize);
    const PROPID pidMTime = PropertyId("MTime", abi::kpidMTime);
    const PROPID pidEnc = PropertyId("Encrypted", abi::kpidEncrypted);
    // kpidBlock (27) is how solid blocks are identified -- the whole
    // single-pass sweep keys off it. Passing 0 as the fallback made this
    // ALWAYS return 0, so every item looked like block 0 and block grouping
    // silently collapsed. Standard properties carry name = NULL in
    // GetPropertyInfo, so the fallback must be the authoritative ordinal.
    const PROPID pidBlock = PropertyId("Block", abi::kpidBlock);

    items.reserve(count);
    for (abi::UInt32 i = 0; i < count; ++i) {
      ArchiveItem item;
      item.index = i;
      item.path = GetString(i, pidPath);
      item.isDir = GetBool(i, pidIsDir);
      item.size = GetUInt64(i, pidSize);
      item.packSize = GetUInt64(i, pidPack);
      item.mtime = static_cast<int64_t>(GetUInt64(i, pidMTime));
      item.encrypted = GetBool(i, pidEnc);
      if (pidBlock != 0) item.block = static_cast<uint32_t>(GetUInt64(i, pidBlock));
      if (transcoder_) {
        // Legacy CJK names are repaired here, at the single place where raw
        // archive bytes enter the product (blueprint ch. 7.1 defence #4).
        const auto repaired = RepairArchiveName(item.path, false, transcoder_);
        item.path = repaired.name;
      }
      // 7-Zip reports kpidPath with native separators on Windows; PrismCore's
      // contract is UTF-8 + '/' everywhere.
      item.path = NormalizeArchivePath(item.path);
      items.push_back(std::move(item));
    }
    return items;
  }

  // Evidence probe: the RAW property truth per entry, VT type included --
  // ReadCatalog's interpretation (GetBool/GetString) is one hypothesis this
  // can contradict. One line per entry: i|path|pid6(vt,val)|pid9|pid7.
  std::string RawProbe() const {
    std::string out;
    if (!archive_) return "no archive";
    abi::UInt32 count = 0;
    if (FAILED(archive_->GetNumberOfItems(&count))) return "no count";
    for (abi::UInt32 i = 0; i < count; ++i) {
      char line[256];
      std::snprintf(line, sizeof(line), "probe i=%u ", static_cast<unsigned>(i));
      out += line;
      const PROPID pids[] = {abi::kpidPath, abi::kpidIsDir, abi::kpidAttrib, abi::kpidSize};
      for (PROPID pid : pids) {
        PROPVARIANT v{};
        PropVariantInit(&v);
        std::string rendered;
        if (SUCCEEDED(archive_->GetProperty(i, pid, &v))) {
          switch (v.vt) {
            case VT_BOOL: rendered = v.boolVal ? "true" : "false"; break;
            case VT_UI4: rendered = std::to_string(v.ulVal); break;
            case VT_UI8: rendered = std::to_string(v.uhVal.QuadPart); break;
            case VT_BSTR: {
              std::wstring w(v.bstrVal ? v.bstrVal : L"");
              rendered.assign(w.begin(), w.end());
              break;
            }
            case VT_FILETIME: rendered = "filetime"; break;
            case VT_EMPTY: rendered = "EMPTY"; break;
            default: rendered = "vt=" + std::to_string(v.vt); break;
          }
        } else {
          rendered = "HR_FAIL";
        }
        out += " pid" + std::to_string(pid) + "=" + rendered;
        PropVariantClear(&v);
      }
      out += "\n";
    }
    return out;
  }

  // -------------------------------------------------------------------------
  // C41: ONE ENGINE OPERATION PER HANDLER, ENFORCED (D24's invariant).
  //
  // The pool already promises "one lease == one handler == one operation".
  // A promise that only lives in the caller turns a broken invariant into
  // SILENT DATA LOSS: two IInArchive::Extract calls on one handler share its
  // decoder and its input stream, so one of them "succeeds" with 0 bytes (the
  // SHA-256 of the empty string, e3b0c442...) or with the other item's bytes.
  // Measured 2026-11-02 on the shared-handler mutation: 185 of 200 concurrent
  // extractions returned ok=false or an empty payload, and 44 returned the
  // WRONG payload.
  //
  // So the last line of defence is here: a second concurrent operation on the
  // same handler is REFUSED and counted (operationConflicts()), never allowed
  // to run. The negative control compiles this guard away
  // (PRISM_C41_MUTANT_NO_OPERATION_GUARD) and the suite then observes the
  // second operation being accepted -- the exact pre-fix shape.
  // -------------------------------------------------------------------------
  class OperationScope {
   public:
    explicit OperationScope(const ArchiveReader& reader)
        : reader_(const_cast<ArchiveReader&>(reader)) {}
    OperationScope(const OperationScope&) = delete;
    OperationScope& operator=(const OperationScope&) = delete;
    ~OperationScope() {
      if (entered_) reader_.EndOperation();
    }
    bool Enter() {
      entered_ = reader_.BeginOperation();
      return entered_;
    }

   private:
    ArchiveReader& reader_;
    bool entered_ = false;
  };

  bool inOperation() const { return operationInFlight_.load(std::memory_order_acquire); }
  uint64_t operationConflicts() const {
    return operationConflicts_.load(std::memory_order_relaxed);
  }

  // Sink 1: thumbnail / preview extraction under a hard memory budget.
  std::optional<std::vector<uint8_t>> ExtractToMemory(
      uint32_t index, size_t maxBytes = 16ull * 1024 * 1024) const {
    if (!archive_) return std::nullopt;
    OperationScope operation(*this);
    if (!operation.Enter()) return std::nullopt;  // C41: one handler, one operation
    auto* sink = new MemoryOutStream(maxBytes);
    auto* callback = new MemoryExtractCallback(sink);
    const abi::UInt32 idx = index;
    const HRESULT hr = archive_->Extract(&idx, 1, 0, callback);
    const bool tripped = sink->tripped();
    std::vector<uint8_t> out = sink->bytes();
    callback->Release();
    sink->Release();
    if (FAILED(hr) || tripped) return std::nullopt;
    return out;
  }

  // Sink 2: full smart extraction straight onto the final paths -- no %TEMP%
  // detour, no post-extraction MoveFile (blueprint ch. 4.2).
  bool ExtractWithPlan(const std::vector<ArchiveItem>& catalog, const SmartPlan& plan,
                       const std::string& targetRoot, BombFuse* fuse,
                       SmartExtractCallback::Result* resultOut) const {
    if (!archive_) return false;
    OperationScope operation(*this);
    if (!operation.Enter()) {
      // C41: refusing is the fix. Running anyway is how a handler's decoder and
      // input stream get shared and one operation silently writes 0 bytes.
      if (resultOut) {
        SmartExtractCallback::Result refused;
        refused.operationConflict = true;
        refused.failureReason =
            "refused: another engine operation is already running on this handler "
            "(one handler == one operation)";
        *resultOut = refused;
      }
      return false;
    }
    std::vector<abi::UInt32> indices;
    for (const auto& item : catalog) {
      if (item.isDir) continue;
      if (plan.ignoredIndices.count(item.index)) continue;
      indices.push_back(item.index);
    }
    if (indices.empty()) return true;
    // kpidMethod is what turns an opaque opResult=1 into "the archive uses
    // zstd". The lambda runs only for a FAILED item (see RecordFailure).
    auto* callback = new SmartExtractCallback(
        catalog, plan, targetRoot, fuse,
        [this](uint32_t index) { return GetString(index, abi::kpidMethod); },
        std::vector<uint32_t>(indices.begin(), indices.end()));
    const HRESULT hr =
        archive_->Extract(indices.data(), static_cast<abi::UInt32>(indices.size()), 0, callback);
    // C26: SUCCEEDED(hr) is NOT the verdict. The engine reports "this item did
    // not decode" through SetOperationResult while Extract() still answers
    // S_OK -- the exact shape that reported an unsupported method as a
    // successful 0-byte extraction.
    SmartExtractCallback::Result result = callback->result();
    callback->Release();
    bool ok = SUCCEEDED(hr) && !result.guardTripped && !result.fuseTripped && !result.failed();
    // C41: the OTHER half of the same contract. Every requested index is a file
    // (directories and ignored entries are skipped above), so a run that wrote
    // NO file at all did not extract anything -- and "ok with 0 bytes" is
    // exactly the silent-loss report this card exists for. Never let the
    // HRESULT alone call that a success.
    if (ok && result.filesWritten == 0) {
      ok = false;
      result.emptyResult = true;
      result.failureReason = "the engine reported success but wrote no files for " +
                             std::to_string(indices.size()) + " requested item(s)";
    }
    if (resultOut) *resultOut = result;
    return ok;
  }

  // Sink 3: the solid sweep (blueprint ch. 4.3) -- ONE Extract call over a
  // whole solid block; the callback demuxes per-item bytes as they stream by.
  //
  // Scan abort (card C1 lifecycle): a whole-archive sweep takes seconds on a
  // large archive, and closing the window must not wait for it. 7-Zip stops an
  // Extract as soon as the extract callback answers with a failure HRESULT, so
  // the sweep's callback is wrapped in BatchAbortProbe and every entry point it
  // has returns E_ABORT once RequestBatchAbort() has been called.
  //
  // The scope is deliberately ExtractBatch-only (the read-only thumbnail scan).
  // ExtractWithPlan is the extraction the USER started; cutting that in half on
  // window close would leave half-written files on disk, so it is never aborted
  // from here -- it is joined instead.
  void RequestBatchAbort() { batchAbort_.store(true, std::memory_order_release); }
  void ClearBatchAbort() { batchAbort_.store(false, std::memory_order_release); }
  bool BatchAbortRequested() const { return batchAbort_.load(std::memory_order_acquire); }

  // C50: WHY a batch extraction failed. ExtractBatch() collapsed three
  // different facts into one bool -- "the C41 guard refused a second operation",
  // "the engine answered a failing HRESULT" and "the engine reported a failed
  // item" -- so a red CI log said only "expected true: ok[i]". Measured on
  // 2026-11-01 (main, run 41332202296, win.reader_pool): the worker's payload
  // was COMPLETE and its SHA-256 matched, and the log still could not say
  // whether a lease had been shared or the engine had hiccuped. This struct
  // reports the three cases apart; the guard's semantics are untouched
  // (refusal stays a refusal, and still counts).
  struct BatchVerdict {
    bool ok = false;
    bool refused = false;    // C41 guard: another operation was in flight
    bool aborted = false;    // the batch-abort flag was set when the call began
    HRESULT hr = S_OK;       // engine answer (S_OK whenever ok)
    uint64_t conflicts = 0;  // this handler's C41 counter AFTER the call
    size_t requested = 0;
    bool invalidArgument = false;  // null handler/callback, or no indices
  };

  BatchVerdict ExtractBatchVerdict(const std::vector<uint32_t>& indices,
                                   abi::IArchiveExtractCallback* callback) const {
    BatchVerdict verdict;
    verdict.requested = indices.size();
    verdict.aborted = BatchAbortRequested();
    if (!archive_ || indices.empty() || !callback) {
      verdict.hr = E_INVALIDARG;
      verdict.invalidArgument = true;
      verdict.conflicts = operationConflicts();
      return verdict;
    }
    OperationScope operation(*this);
    if (!operation.Enter()) {
      // C41: one handler, one operation. Refusing is the fix -- the caller must
      // be able to tell this apart from an engine failure, because a refusal
      // wastes no bytes while an engine failure produced some.
      verdict.refused = true;
      verdict.hr = E_ABORT;
      verdict.conflicts = operationConflicts();
      return verdict;
    }
    BatchAbortProbe probe(callback, &batchAbort_);
    std::vector<abi::UInt32> idx(indices.begin(), indices.end());
    verdict.hr = archive_->Extract(idx.data(), static_cast<abi::UInt32>(idx.size()), 0, &probe);
    verdict.ok = SUCCEEDED(verdict.hr);
    verdict.conflicts = operationConflicts();
    return verdict;
  }

  bool ExtractBatch(const std::vector<uint32_t>& indices,
                    abi::IArchiveExtractCallback* callback) const {
    return ExtractBatchVerdict(indices, callback).ok;
  }

  // Property ids are discovered by name (authoritative, read out of the DLL
  // itself) and only fall back to the hard-coded enum -- guessing PROPIDs is
  // exactly how ABI drift starts.
  PROPID PropertyId(const std::string&, PROPID fallback) const {
    // GetPropertyInfo() reports standard properties with name = NULL and only
    // fills PROPID/VARTYPE, so discovery is a *set of PROPIDs*, not names.
    //
    // CRITICAL: the discovered set describes what the handler will ADMIT to,
    // not what GetProperty(i, pid, ...) answers. 7z's discovery table
    // (Archive/7z/7zProperties.cpp kPropMap -> _fileInfoPopIDs) has no entry
    // for kpidIsDir, yet CHandler::GetProperty answers kpidIsDir truthfully
    // (7zHandler.cpp: case kpidIsDir: PropVarEm_Set_Bool(value, item.IsDir)).
    // Gating on discovery therefore reported pid 0, GetBool returned false,
    // and EVERY DIRECTORY IN EVERY 7z ARCHIVE READ AS A FILE (2026-10-31).
    //
    // So: always trust the compile-time ordinal (pinned against
    // CPP/7zip/PropID.h); discovery is diagnostics only. A handler that does
    // not answer a property returns an empty PROPVARIANT, which every caller
    // already treats as absent -- the failure is local, not a silent default.
    return fallback;
  }

  // The set of PROPIDs this handler actually exposes.
  const std::set<uint32_t>& discoveredProperties() const { return propIdSet_; }

 private:
  // One flag, five entry points: whatever 7-Zip polls next, the answer is
  // E_ABORT. A stack object owned by ExtractBatch -- 7-Zip's AddRef/Release is
  // balanced around the call (the same contract SweepExtractCallback in
  // SolidSweeper.hpp depends on), so a zero count is a programming error here,
  // never a delete.
  class BatchAbortProbe final : public abi::IArchiveExtractCallback {
   public:
    BatchAbortProbe(abi::IArchiveExtractCallback* inner, const std::atomic<bool>* flag)
        : inner_(inner), flag_(flag) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
        *out = static_cast<abi::IArchiveExtractCallback*>(this);
        AddRef();
        return S_OK;
      }
      // Not ours: forward instead of hiding a capability the inner callback may
      // answer (D10 -- QueryInterface must not lie in either direction).
      return inner_->QueryInterface(iid, out);
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override { return InterlockedDecrement(&refs_); }

    STDMETHODIMP SetTotal(abi::UInt64 total) override {
      if (Aborted()) return E_ABORT;
      return inner_->SetTotal(total);
    }
    STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
      if (Aborted()) return E_ABORT;
      return inner_->SetCompleted(completeValue);
    }
    STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialOutStream** outStream,
                          abi::Int32 askExtractMode) override {
      if (Aborted()) return E_ABORT;
      return inner_->GetStream(index, outStream, askExtractMode);
    }
    STDMETHODIMP PrepareOperation(abi::Int32 askExtractMode) override {
      if (Aborted()) return E_ABORT;
      return inner_->PrepareOperation(askExtractMode);
    }
    STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
      if (Aborted()) return E_ABORT;
      return inner_->SetOperationResult(operationResult);
    }

   private:
    bool Aborted() const {
#ifdef PRISM_MUTANT_SCAN_ABORT_IGNORED
      // Negative control (D3): the flag exists and is set, the check is gone.
      // The lifecycle suite must go red on this build.
      return false;
#else
      return flag_ != nullptr && flag_->load(std::memory_order_acquire);
#endif
    }

    abi::IArchiveExtractCallback* inner_;
    const std::atomic<bool>* flag_;
    LONG refs_ = 1;
  };

  class MemoryExtractCallback final : public abi::IArchiveExtractCallback {
   public:
    explicit MemoryExtractCallback(MemoryOutStream* sink) : sink_(sink) { sink_->AddRef(); }
    ~MemoryExtractCallback() {
      if (sink_) sink_->Release();
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
        *out = static_cast<abi::IArchiveExtractCallback*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG r = InterlockedDecrement(&refs_);
      if (r == 0) delete this;
      return r;
    }
    STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
    STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }
    STDMETHODIMP GetStream(abi::UInt32, abi::ISequentialOutStream** out,
                           abi::Int32 mode) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (mode == abi::kExtract) {
        *out = sink_;
        sink_->AddRef();
      }
      return S_OK;
    }
    STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }
    STDMETHODIMP SetOperationResult(abi::Int32) override { return S_OK; }

   private:
    MemoryOutStream* sink_ = nullptr;
    LONG refs_ = 1;
  };

  void DiscoverPropertyIds() {
    char buf[96];
    if (!archive_) {
      propTrace_ = "no archive";
      return;
    }
    abi::UInt32 count = 0;
    const HRESULT hrCount = archive_->GetNumberOfProperties(&count);
    std::snprintf(buf, sizeof(buf), "GetNumberOfProperties hr=0x%08lX n=%u; ",
                  static_cast<unsigned long>(hrCount), static_cast<unsigned>(count));
    propTrace_ = buf;
    if (FAILED(hrCount)) return;
    for (abi::UInt32 i = 0; i < count; ++i) {
      BSTR name = nullptr;
      PROPID propId = 0;
      VARTYPE vt = VT_EMPTY;
      const HRESULT hr = archive_->GetPropertyInfo(i, &name, &propId, &vt);
      if (SUCCEEDED(hr)) {
        propIdSet_.insert(static_cast<uint32_t>(propId));
        if (name) SysFreeString(name);
      } else {
        std::snprintf(buf, sizeof(buf), "[%u] hr=0x%08lX ", static_cast<unsigned>(i),
                      static_cast<unsigned long>(hr));
        propTrace_ += buf;
      }
    }
  }


  std::string GetString(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      if (value.vt == VT_BSTR && value.bstrVal) {
        out = WideToUtf8(value.bstrVal);
      } else if (value.vt == VT_LPWSTR && value.pwszVal) {
        out = WideToUtf8(value.pwszVal);
      }
    }
    PropVariantClear(&value);
    return out;
  }

  uint64_t GetUInt64(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return 0;
    PROPVARIANT value{};
    PropVariantInit(&value);
    uint64_t out = 0;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      switch (value.vt) {
        case VT_UI8: out = value.uhVal.QuadPart; break;
        case VT_UI4: out = value.ulVal; break;
        case VT_UI2: out = value.uiVal; break;
        case VT_I8: out = static_cast<uint64_t>(value.hVal.QuadPart); break;
        case VT_I4: out = static_cast<uint64_t>(value.lVal); break;
        case VT_FILETIME:
          out = (static_cast<uint64_t>(value.filetime.dwHighDateTime) << 32) |
                value.filetime.dwLowDateTime;
          break;
        default: break;
      }
    }
    PropVariantClear(&value);
    return out;
  }

  bool GetBool(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return false;
    PROPVARIANT value{};
    PropVariantInit(&value);
    bool out = false;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      if (value.vt == VT_BOOL) out = value.boolVal != VARIANT_FALSE;
    }
    PropVariantClear(&value);
    return out;
  }

  SevenZipLibrary& library_;
  ITranscoder* transcoder_ = nullptr;
  InFileStream* stream_ = nullptr;
  abi::IInArchive* archive_ = nullptr;
  // C43: the explicit verdict when the bytes prove a format this build cannot
  // open. 'unsupportedFormat_' is set only when NO handler exists at all;
  // 'failureReason_' explains that case and a handler that rejected the file.
  std::string failureReason_;
  std::string unsupportedFormat_;
  std::string openTrace_;
  // Diagnostics only -- see PropertyId(): this set must never gate a
  // property read, because handlers answer properties they do not advertise.
  std::set<uint32_t> propIdSet_;
  std::string propTrace_;
  // C41 (D24): at most one engine operation may be inside this handler. Mutable
  // because the entry points are const and the state is exactly "is a call in
  // flight", not part of the archive's identity.
  bool BeginOperation() const {
#ifdef PRISM_C41_MUTANT_NO_OPERATION_GUARD
    // Negative control (D3): the guard is gone; the second operation is let in.
    // The counter proves the injection really fired.
    MutantOperationGuardBypasses().fetch_add(1, std::memory_order_relaxed);
    operationInFlight_.store(true, std::memory_order_release);
    return true;
#else
    bool expected = false;
    if (!operationInFlight_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      operationConflicts_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    return true;
#endif
  }
  void EndOperation() const { operationInFlight_.store(false, std::memory_order_release); }

  mutable std::atomic<bool> operationInFlight_{false};
  mutable std::atomic<uint64_t> operationConflicts_{0};
  // Sticky "stop the thumbnail scan" flag, read by BatchAbortProbe on the sweep
  // thread and written by the UI thread when the window closes (see
  // RequestBatchAbort). Atomic because those are different threads by design.
  std::atomic<bool> batchAbort_{false};
};

}  // namespace prism::win
