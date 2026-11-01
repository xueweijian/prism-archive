// PrismArchive -- PrismWin
// AMSI memory-level pre-scan of extracted payloads (blueprint 7.1 #1).
//
// amsi.dll is NOT assumed to exist: it is loaded with LoadLibraryW, the three
// entry points are resolved with GetProcAddress, and every failure mode resolves
// to AmsiVerdict::Unavailable -- which the policy layer maps to a warning, never
// to a silent allow (SecurityGuard.hpp: DecideAmsiAction).
//
// The verdict mapping ("0/1 = no detection, admin block, >=32768 detected,
// everything else = no definitive verdict") lives in PrismCore so it is unit
// tested on Linux; this file only turns Win32/AMSI calls into that vocabulary.
#pragma once

#include <windows.h>

#include <amsi.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../PrismCore/SecurityGuard.hpp"

namespace prism::win::amsi {

enum class InitMode : uint8_t {
  LoadAmsi,  // production path: load amsi.dll and AmsiInitialize
  // Models "AMSI is not installed / disabled by policy". The wiring uses
  // LoadAmsi; Disabled exists so the Unavailable path (never a silent Allow) can
  // be exercised on a machine that DOES have amsi.dll -- and because a product
  // policy may legitimately disable scanning, in which case the user must still
  // be told the file was not scanned.
  Disabled
};

struct ScanResult {
  AmsiVerdict verdict = AmsiVerdict::Unavailable;
  HRESULT hr = E_FAIL;
  long raw = -1;  // AMSI_RESULT as returned by the provider
  std::string detail;
};

namespace detail {

// CONTRACTS B4: backslashes + \\?\ (see MotwGuard.hpp, same rule; duplicated
// here so the AMSI guard has no dependency on the MoTW header).
inline std::wstring ToExtendedPath(const std::wstring& path) {
  if (path.empty()) return path;
  std::wstring p = path;
  for (wchar_t& c : p) {
    if (c == L'/') c = L'\\';
  }
  if (p.rfind(L"\\\\?\\", 0) == 0 || p.rfind(L"\\\\.\\", 0) == 0) return p;
  if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
    return L"\\\\?\\UNC\\" + p.substr(2);
  }
  wchar_t buffer[32768];
  const DWORD n = GetFullPathNameW(p.c_str(), 32768, buffer, nullptr);
  if (n == 0 || n >= 32768) return p;
  const std::wstring absolute(buffer, n);
  if (absolute.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + absolute.substr(2);
  return L"\\\\?\\" + absolute;
}

inline std::string Win32Detail(const char* what, DWORD error) {
  return std::string(what) + " failed, Win32 error " +
         std::to_string(static_cast<unsigned long>(error));
}

template <typename Fn>
Fn Resolve(HMODULE module, const char* name) {
  const FARPROC proc = GetProcAddress(module, name);
  void* raw = nullptr;
  static_assert(sizeof(raw) == sizeof(proc), "function pointer / FARPROC size mismatch");
  std::memcpy(&raw, &proc, sizeof(raw));
  Fn fn = nullptr;
  std::memcpy(&fn, &raw, sizeof(fn));
  return fn;
}

}  // namespace detail

// One Scanner per worker thread. It owns its AMSI context; no thread-safety is
// claimed for concurrent use of a single instance.
class Scanner {
 public:
  explicit Scanner(InitMode mode = InitMode::LoadAmsi) { Init(mode); }

  ~Scanner() {
    if (uninitialize_ && context_) uninitialize_(context_);
    if (module_) FreeLibrary(module_);
  }

  Scanner(const Scanner&) = delete;
  Scanner& operator=(const Scanner&) = delete;

  bool available() const { return scan_ != nullptr && context_ != nullptr; }
  const std::string& unavailableReason() const { return unavailableReason_; }

  // Diagnostics for the live test and the status UI: how many AMSI calls the last
  // ScanFile made, and how many bytes were presented (which exceeds the file size
  // by the planned overlaps). Reset to 0 at the start of every ScanFile.
  size_t lastChunksScanned() const { return lastChunksScanned_; }
  uint64_t lastScanBytes() const { return lastScanBytes_; }

  // Scans one in-memory buffer. contentName is shown to the provider and should
  // name the extracted file (its extension drives macro/script heuristics).
  ScanResult ScanBuffer(const void* data, size_t size, const std::wstring& contentName) const {
    if (!available()) return {AmsiVerdict::Unavailable, E_FAIL, -1, unavailableReason_};
    if (size == 0) return {AmsiVerdict::Allow, S_OK, kAmsiResultClean, "empty buffer"};
    if (size > static_cast<size_t>(0xFFFFFFFFull)) {
      return {AmsiVerdict::Unavailable, E_INVALIDARG, -1,
              "buffer exceeds the ULONG length AmsiScanBuffer accepts"};
    }
    AMSI_RESULT amsiResult = static_cast<AMSI_RESULT>(-1);
    const HRESULT hr =
        scan_(context_, const_cast<void*>(data), static_cast<ULONG>(size), contentName.c_str(),
              nullptr, &amsiResult);
    if (FAILED(hr)) {
      return {AmsiVerdict::Unavailable, hr, -1,
              "AmsiScanBuffer failed, hr=0x" + Hex(static_cast<unsigned long>(hr))};
    }
    const long raw = static_cast<long>(amsiResult);
    return {ClassifyAmsiResult(raw), hr, raw, {}};
  }

  // Full-file scan: PlanAmsiChunks keeps memory bounded and overlaps consecutive
  // buffers so a signature straddling a chunk boundary is still whole somewhere.
  // Block outranks Unavailable; a later chunk's detection is never hidden by an
  // earlier chunk's failure.
  ScanResult ScanFile(const std::wstring& path) const {
    lastChunksScanned_ = 0;
    lastScanBytes_ = 0;
    if (!available()) return {AmsiVerdict::Unavailable, E_FAIL, -1, unavailableReason_};

    const std::wstring full = detail::ToExtendedPath(path);
    HANDLE file = CreateFileW(full.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return {AmsiVerdict::Unavailable, HRESULT_FROM_WIN32(GetLastError()), -1,
              detail::Win32Detail("CreateFileW(scan target)", GetLastError())};
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
      CloseHandle(file);
      return {AmsiVerdict::Unavailable, HRESULT_FROM_WIN32(GetLastError()), -1,
              "GetFileSizeEx(scan target) failed"};
    }
    const uint64_t total = static_cast<uint64_t>(size.QuadPart);
    if (total == 0) {
      CloseHandle(file);
      return {AmsiVerdict::Allow, S_OK, kAmsiResultClean, "empty file"};
    }

    const std::vector<AmsiChunk> plan = PlanAmsiChunks(total);
    size_t capacity = 0;
    for (const AmsiChunk& chunk : plan) {
      if (chunk.length > capacity) capacity = static_cast<size_t>(chunk.length);
    }
    std::vector<uint8_t> buffer(capacity);

    ScanResult unavailable{AmsiVerdict::Unavailable, E_FAIL, -1, {}};
    bool sawUnavailable = false;
    for (const AmsiChunk& chunk : plan) {
      LARGE_INTEGER offset{};
      offset.QuadPart = static_cast<LONGLONG>(chunk.offset);
      if (!SetFilePointerEx(file, offset, nullptr, FILE_BEGIN)) {
        unavailable = {AmsiVerdict::Unavailable, HRESULT_FROM_WIN32(GetLastError()), -1,
                       "SetFilePointerEx(scan target) failed"};
        sawUnavailable = true;
        continue;
      }
      size_t got = 0;
      const size_t want = static_cast<size_t>(chunk.length);
      bool readFailed = false;
      while (got < want) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data() + got, static_cast<DWORD>(want - got), &read, nullptr) ||
            read == 0) {
          readFailed = true;
          break;
        }
        got += read;
      }
      if (readFailed) {
        unavailable = {AmsiVerdict::Unavailable, HRESULT_FROM_WIN32(GetLastError()), -1,
                       detail::Win32Detail("ReadFile(scan target)", GetLastError())};
        sawUnavailable = true;
        continue;
      }
      const ScanResult result =
          ScanBuffer(buffer.data(), got, ContentNameFor(full, chunk.offset));
      ++lastChunksScanned_;
      lastScanBytes_ += got;
      if (result.verdict == AmsiVerdict::Block) {
        CloseHandle(file);
        return result;
      }
      if (result.verdict == AmsiVerdict::Unavailable) {
        unavailable = result;
        sawUnavailable = true;
      }
    }
    CloseHandle(file);
    if (sawUnavailable) return unavailable;
    return {AmsiVerdict::Allow, S_OK, kAmsiResultClean, {}};
  }

 private:
  using AmsiInitializeFn = HRESULT(WINAPI*)(LPCWSTR, HAMSICONTEXT*);
  using AmsiUninitializeFn = void(WINAPI*)(HAMSICONTEXT);
  using AmsiScanBufferFn =
      HRESULT(WINAPI*)(HAMSICONTEXT, PVOID, ULONG, LPCWSTR, HAMSISESSION, AMSI_RESULT*);

  static std::string Hex(unsigned long value) {
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%08lX", value);
    return std::string(buffer);
  }

  // The provider sees the extracted file's own name for every chunk. Do NOT
  // append a chunk marker after the extension ("payload.exe#chunk@..."): the
  // extension is what drives the provider's script/macro heuristics, and
  // corrupting it silently downgrades a scan (measured 2026-11-01: the same
  // buffer stopped being classified as an executable).
  static std::wstring ContentNameFor(const std::wstring& fullPath, uint64_t) {
    const size_t slash = fullPath.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? fullPath : fullPath.substr(slash + 1);
  }

  void Init(InitMode mode) {
    if (mode == InitMode::Disabled) {
      unavailableReason_ = "AMSI disabled by policy";
      return;
    }
    module_ = LoadLibraryW(L"amsi.dll");
    if (!module_) {
      unavailableReason_ =
          detail::Win32Detail("LoadLibraryW(amsi.dll)", GetLastError());
      return;
    }
    const AmsiInitializeFn init = detail::Resolve<AmsiInitializeFn>(module_, "AmsiInitialize");
    scan_ = detail::Resolve<AmsiScanBufferFn>(module_, "AmsiScanBuffer");
    uninitialize_ = detail::Resolve<AmsiUninitializeFn>(module_, "AmsiUninitialize");
    if (!init || !scan_) {
      unavailableReason_ = "amsi.dll is missing AmsiInitialize/AmsiScanBuffer exports";
      scan_ = nullptr;
      FreeLibrary(module_);
      module_ = nullptr;
      return;
    }
    const HRESULT hr = init(L"PrismArchive/0.1", &context_);
    if (FAILED(hr)) {
      unavailableReason_ =
          "AmsiInitialize failed, hr=0x" + Hex(static_cast<unsigned long>(hr));
      context_ = nullptr;
      scan_ = nullptr;
      FreeLibrary(module_);
      module_ = nullptr;
    }
  }

  HMODULE module_ = nullptr;
  HAMSICONTEXT context_ = nullptr;
  AmsiScanBufferFn scan_ = nullptr;
  AmsiUninitializeFn uninitialize_ = nullptr;
  std::string unavailableReason_;
  mutable size_t lastChunksScanned_ = 0;
  mutable uint64_t lastScanBytes_ = 0;
};

}  // namespace prism::win::amsi
