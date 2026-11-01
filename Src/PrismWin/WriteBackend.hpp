// PrismArchive -- small-file write backends (blueprint ch. 7.2).
//
// The blueprint's ch.7.2 row: "80 000 x 2 KB small files, 42 s -> 6.8 s
// (IORING + directory-tree pre-creation pool + 4-thread sharded async write)".
// This header is the executable core of that row:
//
//   * ONE three-method interface -- CreateFile / Write / Close -- so a fake
//     backend can be injected in a unit test and the driver's error handling is
//     testable without touching a disk (contract C6: assert the layer that owns
//     the responsibility, and keep that layer small).
//   * THREE implementations: SequentialBackend (the honest baseline: per-file
//     CreateDirectoryW + CreateFileW), PrecreateBackend (the directory tree is
//     created once, and the one handle Win32 actually lets us reuse -- a
//     repeated destination path -- is kept in a one-slot cache), and
//     IoRingBackend (Windows 11 BuildIoRing write submission, dynamically loaded
//     with GetProcAddress and degrading to PrecreateBackend with a REASON when
//     the API or the WRITE opcode is missing -- never a fake success).
//   * A large-page probe (LargePageStatus / ProbeLargePages): 7z.dll's own
//     SetLargePageMode is called through card C1's SevenZipLibrary entry point
//     by the caller, and this probe reports whether the OS would actually honour
//     it (SeLockMemoryPrivilege held AND a real MEM_LARGE_PAGES VirtualAlloc
//     succeeded). "Unavailable" is reported as degraded with the failing step,
//     because a probe that always says yes is worse than no probe.
//
// What is NOT claimed here: Win32 has no "create file relative to a directory
// handle" API (that is NtCreateFile + RootDirectory), so per-file handles can
// NOT be reused across distinct paths. The only legal reuse is for a destination
// path that appears more than once in the plan, and that is exactly what the
// one-slot cache implements; the benchmark reports handles_reused so the claim
// is visible as a number, not as prose.
//
// Negative controls (contract D3) live in this header on purpose:
//   PRISM_WRITE_MUTANT_NO_PREDIR
//       PrecreateBackend skips the directory pre-creation pass. Every write then
//       hits a missing parent and fails; the mutant executable prints
//       "WRITE-BACKEND MUTANT CAUGHT" only when that failure was observed AND
//       the injection counter proves the branch compiled in.
//   (the long-path control lives in LongPath.hpp: PRISM_LONGPATH_MUTANT_STRIP_PREFIX)
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if __has_include(<ioringapi.h>)
#include <ioringapi.h>
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "LongPath.hpp"

#if defined(NTDDI_WIN10_CO) && (NTDDI_VERSION >= NTDDI_WIN10_CO) && defined(NTDDI_WIN10_NI) && \
    (NTDDI_VERSION >= NTDDI_WIN10_NI)
#define PRISM_WRITEBACK_HAS_IORING 1
#else
#define PRISM_WRITEBACK_HAS_IORING 0
#endif

namespace prism::win::writeback {

// --- the three-method contract ---------------------------------------------
using FileHandle = void*;
inline constexpr FileHandle kInvalidFile = nullptr;

// Why the handle is opaque to callers: the whole point of the interface is that
// the driver never knows whether it is a Win32 HANDLE, a ring queue slot, or a
// fake object in a unit test.
class WriteBackend {
 public:
  WriteBackend() = default;
  WriteBackend(const WriteBackend&) = delete;
  WriteBackend& operator=(const WriteBackend&) = delete;
  virtual ~WriteBackend() = default;

  // Creates (and truncates) one destination file. Returns kInvalidFile and
  // fills *why on failure. The backend owns path normalisation: callers pass
  // the path they mean, relative or not, and never add the extended prefix
  // themselves.
  virtual FileHandle CreateFile(const std::wstring& path, std::wstring* why) = 0;

  // Writes the full buffer or fails. size is bytes, never a partial contract:
  // a short write is a failure, not a number the caller must inspect.
  virtual bool Write(FileHandle file, const void* data, size_t size, std::wstring* why) = 0;

  // Finalises the file. Returns false if any bytes of this file failed. The
  // backend may complete the last submission lazily (IoRing) or keep one handle
  // in a reuse cache (Precreate); both guarantee completeness before the
  // backend is destroyed, which is why the tests validate after the backend
  // goes out of scope (documented, not incidental).
  virtual bool Close(FileHandle file, std::wstring* why) = 0;
};

// --- driver ----------------------------------------------------------------
struct WriteItem {
  std::wstring path;
  const void* data = nullptr;
  size_t size = 0;
};

struct WriteStats {
  bool ok = true;
  uint64_t files = 0;   // files whose bytes were fully written
  uint64_t bytes = 0;   // bytes accepted by the backend
  std::wstring firstError;
};

// Drives a backend over a plan. pipelineWindow > 1 opens/writes a window of
// files before closing them -- the shape a real extraction pipeline uses and
// the only way the IoRing backend can batch submission through a three-method
// interface (Close is per file, so the window is what lets several SQEs sit in
// the queue at once). window == 1 is strictly file-at-a-time.
inline WriteStats WriteAll(WriteBackend& backend, const std::vector<WriteItem>& items,
                           size_t pipelineWindow = 1) {
  WriteStats stats;
  const size_t window = (std::max)(size_t{1}, pipelineWindow);
  size_t i = 0;
  while (i < items.size()) {
    const size_t end = (std::min)(items.size(), i + window);
    struct OpenFile {
      FileHandle handle;
      const WriteItem* item;
    };
    std::vector<OpenFile> opened;
    opened.reserve(end - i);
    bool failed = false;

    for (size_t j = i; j < end; ++j) {
      std::wstring why;
      FileHandle handle = backend.CreateFile(items[j].path, &why);
      if (handle == kInvalidFile) {
        stats.firstError = items[j].path + L": " + why;
        failed = true;
        break;
      }
      opened.push_back({handle, &items[j]});
      if (!backend.Write(handle, items[j].data, items[j].size, &why)) {
        stats.firstError = items[j].path + L": " + why;
        failed = true;
        break;
      }
      ++stats.files;
      stats.bytes += items[j].size;
    }

    for (auto it = opened.rbegin(); it != opened.rend(); ++it) {
      std::wstring why;
      if (!backend.Close(it->handle, &why)) {
        if (stats.firstError.empty()) stats.firstError = it->item->path + L": " + why;
        failed = true;
      }
    }
    if (failed) {
      stats.ok = false;
      break;
    }
    i = end;
  }
  return stats;
}

// --- shared Win32 helpers ---------------------------------------------------
namespace detail {

inline std::wstring Hex32(uint32_t value) {
  std::wostringstream os;
  os << L"0x" << std::hex << value;
  return os.str();
}

inline std::wstring Win32Why(const wchar_t* what, DWORD code) {
  return std::wstring(what) + L" failed, GetLastError=" + std::to_wstring(code) + L" (" +
         Hex32(code) + L")";
}

inline std::wstring HResultWhy(const wchar_t* what, HRESULT hr) {
  return std::wstring(what) + L" failed, hr=" + Hex32(static_cast<uint32_t>(hr));
}

// Parent directory of a NORMALISED (extended-prefix) path. The root forms
// '\?\C:\' and '\?\UNC\server\share\' have no parent.
inline std::wstring ParentDirectory(const std::wstring& normalized) {
  std::wstring_view view(normalized);
  size_t end = view.size();
  while (end > 0 && (view[end - 1] == L'\\' || view[end - 1] == L'/')) --end;
  const size_t slash = view.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
  if (slash == std::wstring_view::npos) return {};
  std::wstring parent(view.substr(0, slash));
  if (parent == L"\\\\?\\" || parent == L"\\\\?\\UNC") return {};
  return parent;
}

// A component whose name needs FILE_FLAG_POSIX_SEMANTICS to be preserved
// verbatim (trailing dot/space, or a DOS device name that the extended prefix
// already protects but POSIX semantics keeps consistent across APIs).
inline bool NeedsPosixSemantics(std::wstring_view base) {
  if (longpath::IsReservedBaseName(base)) return true;
  return !base.empty() && (base.back() == L'.' || base.back() == L' ');
}

inline bool IsRootDirectory(const std::wstring& dir) {
  return dir == L"\\\\?\\" || dir == L"\\\\?\\UNC";
}

// Creates dir and any missing ancestor, one level at a time. Returns false with
// *why set on a real failure; *created (optional) counts the directories that
// did not exist before.
inline bool CreateDirectoryChain(const std::wstring& dir, std::wstring* why, size_t* created) {
  if (dir.empty() || IsRootDirectory(dir) || dir.size() <= 4) return true;
  const DWORD attributes = ::GetFileAttributesW(dir.c_str());
  if (attributes != INVALID_FILE_ATTRIBUTES) {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return true;
    if (why) *why = L"a file occupies the directory path: " + dir;
    return false;
  }
  const DWORD attrError = ::GetLastError();
  if (attrError != ERROR_FILE_NOT_FOUND && attrError != ERROR_PATH_NOT_FOUND) {
    if (why) *why = Win32Why(L"GetFileAttributesW", attrError);
    return false;
  }
  const std::wstring parent = ParentDirectory(dir);
  if (!parent.empty() && parent != dir) {
    if (!CreateDirectoryChain(parent, why, created)) return false;
  }
  if (::CreateDirectoryW(dir.c_str(), nullptr)) {
    if (created) ++*created;
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS) return true;
  if (why) *why = Win32Why(L"CreateDirectoryW", error);
  return false;
}

// Opens a destination for overwrite. FILE_FLAG_POSIX_SEMANTICS is requested only
// where the name needs it (reserved name / trailing dot) and retried without it
// on volumes that reject the flag, so a FAT/exFAT destination degrades instead
// of failing -- never assume NTFS.
inline HANDLE OpenDestination(const std::wstring& normalized, DWORD extraFlags,
                              std::wstring* why) {
  const std::wstring_view base = longpath::BaseName(normalized);
  const DWORD baseFlags = FILE_ATTRIBUTE_NORMAL | extraFlags;
  DWORD flags = baseFlags;
  const bool posix = NeedsPosixSemantics(base);
  if (posix) flags |= FILE_FLAG_POSIX_SEMANTICS;
  HANDLE handle =
      ::CreateFileW(normalized.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, flags, nullptr);
  if (handle == INVALID_HANDLE_VALUE && posix && ::GetLastError() == ERROR_INVALID_PARAMETER) {
    handle = ::CreateFileW(normalized.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, baseFlags,
                           nullptr);
  }
  if (handle == INVALID_HANDLE_VALUE && why) {
    *why = Win32Why(L"CreateFileW", ::GetLastError());
  }
  return handle;
}

inline bool WriteWholeFile(HANDLE file, const void* data, size_t size, std::wstring* why) {
  const uint8_t* cursor = static_cast<const uint8_t*>(data);
  size_t remaining = size;
  while (remaining > 0) {
    const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(1u << 30)));
    DWORD written = 0;
    if (!::WriteFile(file, cursor, chunk, &written, nullptr) || written != chunk) {
      if (why) *why = Win32Why(L"WriteFile", ::GetLastError());
      return false;
    }
    cursor += chunk;
    remaining -= chunk;
  }
  return true;
}

inline bool CloseWholeFile(HANDLE file, std::wstring* why) {
  if (file == INVALID_HANDLE_VALUE || file == nullptr) return true;
  if (!::CloseHandle(file)) {
    if (why) *why = Win32Why(L"CloseHandle", ::GetLastError());
    return false;
  }
  return true;
}

}  // namespace detail

// --- backend 1: the honest sequential baseline ------------------------------
//
// One CreateFileW per file, and because the baseline has no pre-created tree it
// must also make the parent chain per file. That is what the blueprint row calls
// "serial CreateFileW -> WriteFile -> CloseHandle"; keeping the directory work
// in this backend is what makes the comparison against PrecreateBackend a
// comparison of strategies instead of a comparison of fixtures.
class SequentialBackend final : public WriteBackend {
 public:
  SequentialBackend() = default;

  FileHandle CreateFile(const std::wstring& path, std::wstring* why) override {
    ++createCalls_;
    const std::wstring normalized = longpath::Normalize(path);
    const std::wstring parent = detail::ParentDirectory(normalized);
    if (!parent.empty() && !detail::CreateDirectoryChain(parent, why, &directoriesCreated_)) {
      return kInvalidFile;
    }
    const HANDLE handle = detail::OpenDestination(normalized, 0, why);
    if (handle == INVALID_HANDLE_VALUE) return kInvalidFile;
    return handle;
  }

  bool Write(FileHandle file, const void* data, size_t size, std::wstring* why) override {
    ++writeCalls_;
    return detail::WriteWholeFile(static_cast<HANDLE>(file), data, size, why);
  }

  bool Close(FileHandle file, std::wstring* why) override {
    ++closeCalls_;
    return detail::CloseWholeFile(static_cast<HANDLE>(file), why);
  }

  uint64_t createCalls() const { return createCalls_; }
  uint64_t writeCalls() const { return writeCalls_; }
  uint64_t closeCalls() const { return closeCalls_; }
  uint64_t directoriesCreated() const { return directoriesCreated_; }

 private:
  uint64_t createCalls_ = 0;
  uint64_t writeCalls_ = 0;
  uint64_t closeCalls_ = 0;
  uint64_t directoriesCreated_ = 0;
};

// --- backend 2: directory-tree pre-creation + one-slot handle reuse ---------
//
// The constructor walks the plan, deduplicates every parent directory and
// creates the tree once, before the first file. The per-file path then costs one
// CreateFileW and nothing else. The directory work is therefore INSIDE the
// measured cell (the benchmark times backend construction + writes), which is
// the only fair way to compare it with the baseline.
class PrecreateBackend final : public WriteBackend {
 public:
  explicit PrecreateBackend(const std::vector<std::wstring>& destinations) {
    std::map<std::wstring, int> counts;
    std::vector<std::wstring> parents;
    parents.reserve(destinations.size());
    for (const std::wstring& destination : destinations) {
      const std::wstring normalized = longpath::Normalize(destination);
      parents.push_back(detail::ParentDirectory(normalized));
      ++counts[normalized];
    }
    for (const auto& entry : counts) {
      if (entry.second > 1) duplicateCounts_[entry.first] = entry.second;
    }

    std::sort(parents.begin(), parents.end());
    parents.erase(std::unique(parents.begin(), parents.end()), parents.end());
#ifdef PRISM_WRITE_MUTANT_NO_PREDIR
    // Negative control: no tree, therefore no parent directory per file.
    // InjectionCount() is the proof the branch actually ran (D3).
    ++longpath::InjectionCount();
    precreateError_ = L"PRISM_WRITE_MUTANT_NO_PREDIR: directory pre-creation disabled";
    precreateFailed_ = true;
#else
    for (const std::wstring& parent : parents) {
      if (parent.empty()) continue;
      if (!detail::CreateDirectoryChain(parent, &precreateError_, &directoriesCreated_)) {
        precreateFailed_ = true;
        break;
      }
    }
#endif
  }

  ~PrecreateBackend() override {
    if (reuseHandle_ != kInvalidFile) {
      detail::CloseWholeFile(static_cast<HANDLE>(reuseHandle_), nullptr);
    }
  }

  FileHandle CreateFile(const std::wstring& path, std::wstring* why) override {
    ++createCalls_;
    const std::wstring normalized = longpath::Normalize(path);
    if (precreateFailed_ && why) *why = precreateError_;

    // One-slot reuse: legal only because this exact path appears again in the
    // plan (an OS handle is per path; distinct paths cannot share one).
    if (reuseHandle_ != kInvalidFile && reusePath_ == normalized) {
      HANDLE reused = static_cast<HANDLE>(reuseHandle_);
      LARGE_INTEGER zero{};
      if (::SetFilePointerEx(reused, zero, nullptr, FILE_BEGIN) && ::SetEndOfFile(reused)) {
        reuseHandle_ = kInvalidFile;
        reusePath_.clear();
        ++handlesReused_;
        return reused;
      }
      detail::CloseWholeFile(reused, nullptr);
      reuseHandle_ = kInvalidFile;
      reusePath_.clear();
    }

    const HANDLE handle = detail::OpenDestination(normalized, FILE_FLAG_SEQUENTIAL_SCAN, why);
    if (handle == INVALID_HANDLE_VALUE) return kInvalidFile;
    pathOf_[handle] = normalized;
    return handle;
  }

  bool Write(FileHandle file, const void* data, size_t size, std::wstring* why) override {
    ++writeCalls_;
    return detail::WriteWholeFile(static_cast<HANDLE>(file), data, size, why);
  }

  bool Close(FileHandle file, std::wstring* why) override {
    ++closeCalls_;
    HANDLE handle = static_cast<HANDLE>(file);
    auto it = pathOf_.find(handle);
    const bool reusable = it != pathOf_.end() && duplicateCounts_.count(it->second) > 0;
    if (reusable && reuseHandle_ == kInvalidFile) {
      reuseHandle_ = handle;
      reusePath_ = it->second;
      pathOf_.erase(it);
      return true;  // bytes are complete; the handle is finalised on reuse/destruction
    }
    if (it != pathOf_.end()) pathOf_.erase(it);
    return detail::CloseWholeFile(handle, why);
  }

  uint64_t createCalls() const { return createCalls_; }
  uint64_t writeCalls() const { return writeCalls_; }
  uint64_t closeCalls() const { return closeCalls_; }
  uint64_t directoriesCreated() const { return directoriesCreated_; }
  uint64_t handlesReused() const { return handlesReused_; }
  bool precreateFailed() const { return precreateFailed_; }
  const std::wstring& precreateError() const { return precreateError_; }

 private:
  std::unordered_map<void*, std::wstring> pathOf_;
  std::map<std::wstring, int> duplicateCounts_;
  FileHandle reuseHandle_ = kInvalidFile;
  std::wstring reusePath_;
  uint64_t createCalls_ = 0;
  uint64_t writeCalls_ = 0;
  uint64_t closeCalls_ = 0;
  uint64_t directoriesCreated_ = 0;
  uint64_t handlesReused_ = 0;
  bool precreateFailed_ = false;
  std::wstring precreateError_;
};

// --- backend kinds (printed by the benchmark/test failure messages) ---------
enum class BackendKind { Sequential = 0, Precreate = 1, IoRing = 2 };

inline const char* BackendKindId(BackendKind kind) {
  switch (kind) {
    case BackendKind::Sequential:
      return "sequential";
    case BackendKind::Precreate:
      return "precreate";
    case BackendKind::IoRing:
      return "ioring";
  }
  return "unknown";
}

inline std::ostream& operator<<(std::ostream& os, BackendKind kind) {
  return os << BackendKindId(kind);
}

// --- backend 3: Windows 11 IoRing (BuildIoRingWriteFile) --------------------
//
// Dynamically loaded on purpose: the application must start on Windows 10 (no
// IoRing at all) and on Windows 11 builds where the WRITE opcode is missing.
// GetProcAddress failures are data, not a crash, and the backend then delegates
// to PrecreateBackend while KEEPING the reason -- the caller can report
// "degraded: ..." instead of pretending the fast path ran.
#if PRISM_WRITEBACK_HAS_IORING
struct IoRingApi {
  using FnCreateIoRing = HRESULT(WINAPI*)(IORING_VERSION, IORING_CREATE_FLAGS, UINT32, UINT32,
                                          HIORING*);
  using FnCloseIoRing = HRESULT(WINAPI*)(HIORING);
  using FnSubmitIoRing = HRESULT(WINAPI*)(HIORING, UINT32, UINT32, UINT32*);
  using FnPopIoRingCompletion = HRESULT(WINAPI*)(HIORING, IORING_CQE*);
  using FnIsIoRingOpSupported = BOOL(WINAPI*)(HIORING, IORING_OP_CODE);
  using FnBuildIoRingWriteFile = HRESULT(WINAPI*)(HIORING, IORING_HANDLE_REF, IORING_BUFFER_REF,
                                                  UINT32, UINT64, FILE_WRITE_FLAGS, UINT_PTR,
                                                  IORING_SQE_FLAGS);

  FnCreateIoRing create = nullptr;
  FnCloseIoRing close = nullptr;
  FnSubmitIoRing submit = nullptr;
  FnPopIoRingCompletion pop = nullptr;
  FnIsIoRingOpSupported isOpSupported = nullptr;
  FnBuildIoRingWriteFile buildWrite = nullptr;
  bool available = false;
  std::wstring reason;
};

inline const IoRingApi& LoadIoRingApi() {
  static const IoRingApi api = [] {
    IoRingApi loaded;
    const wchar_t* modules[] = {L"kernel32.dll", L"api-ms-win-core-ioring-l1-1-1.dll",
                                L"api-ms-win-core-ioring-l1-1-0.dll"};
    auto resolve = [&modules](const char* name) -> FARPROC {
      for (const wchar_t* moduleName : modules) {
        HMODULE module = ::GetModuleHandleW(moduleName);
        if (module == nullptr) module = ::LoadLibraryW(moduleName);
        if (module == nullptr) continue;
        FARPROC proc = ::GetProcAddress(module, name);
        if (proc != nullptr) return proc;
      }
      return nullptr;
    };
    loaded.create = reinterpret_cast<IoRingApi::FnCreateIoRing>(resolve("CreateIoRing"));
    loaded.close = reinterpret_cast<IoRingApi::FnCloseIoRing>(resolve("CloseIoRing"));
    loaded.submit = reinterpret_cast<IoRingApi::FnSubmitIoRing>(resolve("SubmitIoRing"));
    loaded.pop =
        reinterpret_cast<IoRingApi::FnPopIoRingCompletion>(resolve("PopIoRingCompletion"));
    loaded.isOpSupported =
        reinterpret_cast<IoRingApi::FnIsIoRingOpSupported>(resolve("IsIoRingOpSupported"));
    loaded.buildWrite =
        reinterpret_cast<IoRingApi::FnBuildIoRingWriteFile>(resolve("BuildIoRingWriteFile"));
    const bool complete = loaded.create && loaded.close && loaded.submit && loaded.pop &&
                          loaded.isOpSupported && loaded.buildWrite;
    loaded.available = complete;
    if (!complete) {
      loaded.reason =
          L"Windows 11 IoRing exports not found (CreateIoRing / BuildIoRingWriteFile)";
    }
    return loaded;
  }();
  return api;
}
#endif  // PRISM_WRITEBACK_HAS_IORING

class IoRingBackend final : public WriteBackend {
 public:
  explicit IoRingBackend(const std::vector<std::wstring>& destinations,
                         uint32_t batchSize = 32)
      : precreate_(std::make_unique<PrecreateBackend>(destinations)) {
    batchSize_ = (std::max)(uint32_t{1}, (std::min)(batchSize, uint32_t{256}));
#if PRISM_WRITEBACK_HAS_IORING
    const IoRingApi& api = LoadIoRingApi();
    if (!api.available) {
      degradedReason_ = api.reason;
      return;
    }
    IORING_CREATE_FLAGS flags{};
    flags.Required = IORING_CREATE_REQUIRED_FLAGS_NONE;
    flags.Advisory = IORING_CREATE_ADVISORY_FLAGS_NONE;
    HIORING ring = nullptr;
    // Version 3 is the first version that carries WRITE; the numeric literal
    // keeps this header compiling on SDKs where the enum name is gated.
    const HRESULT created = api.create(static_cast<IORING_VERSION>(300), flags, 256, 256, &ring);
    if (FAILED(created)) {
      degradedReason_ = detail::HResultWhy(L"CreateIoRing", created);
      return;
    }
    if (!api.isOpSupported(ring, IORING_OP_WRITE)) {
      api.close(ring);
      degradedReason_ = L"the IoRing exists but does not support IORING_OP_WRITE";
      return;
    }
    api_ = &api;
    ring_ = ring;
    pending_.reserve(batchSize_);
    available_ = true;
#else
    degradedReason_ = L"the Windows SDK does not declare IoRing (NTDDI < WIN10_CO/NI)";
#endif
  }

  ~IoRingBackend() override {
    Flush();
#if PRISM_WRITEBACK_HAS_IORING
    if (ring_ != nullptr && api_ != nullptr) api_->close(ring_);
#endif
  }

  FileHandle CreateFile(const std::wstring& path, std::wstring* why) override {
    if (!available_) return precreate_->CreateFile(path, why);
    ++createCalls_;
    const std::wstring normalized = longpath::Normalize(path);
    HANDLE handle = ::CreateFileW(normalized.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
      if (why) *why = detail::Win32Why(L"CreateFileW(FILE_FLAG_OVERLAPPED)", ::GetLastError());
      return kInvalidFile;
    }
    return handle;
  }

  bool Write(FileHandle file, const void* data, size_t size, std::wstring* why) override {
    if (!available_) return precreate_->Write(file, data, size, why);
    ++writeCalls_;
    if (size > 0xFFFFFFFFull) {
      if (why) *why = L"one IoRing submission is limited to 4 GiB";
      return false;
    }
    Pending entry;
    entry.handle = static_cast<HANDLE>(file);
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    entry.bytes.assign(bytes, bytes + size);
    entry.userData = nextUserData_++;
    pending_.push_back(std::move(entry));
    Pending& queued = pending_.back();
    const HRESULT built = api_->buildWrite(
        ring_, IORING_HANDLE_REF(queued.handle), IORING_BUFFER_REF(queued.bytes.data()),
        static_cast<UINT32>(queued.bytes.size()), 0, FILE_WRITE_FLAGS_NONE,
        static_cast<UINT_PTR>(queued.userData), IOSQE_FLAGS_NONE);
    if (FAILED(built)) {
      std::wstring reason = detail::HResultWhy(L"BuildIoRingWriteFile", built);
      failures_[queued.handle] = reason;
      pending_.pop_back();
      if (why) *why = std::move(reason);
      return false;
    }
    if (pending_.size() >= batchSize_) Flush();
    return true;  // submission errors surface at Close (that is the batch's trade)
  }

  bool Close(FileHandle file, std::wstring* why) override {
    if (!available_) return precreate_->Close(file, why);
    ++closeCalls_;
    Flush();
    HANDLE handle = static_cast<HANDLE>(file);
    bool ok = true;
    auto failure = failures_.find(handle);
    if (failure != failures_.end()) {
      if (why) *why = failure->second;
      failures_.erase(failure);
      ok = false;
    }
    if (!detail::CloseWholeFile(handle, why)) ok = false;
    return ok;
  }

  // Not part of the three-method contract: the benchmark and tests call it to
  // verify that a full batch is submitted in one SubmitIoRing call.
  void Flush() {
    if (!available_) {
      return;
    }
#if PRISM_WRITEBACK_HAS_IORING
    if (pending_.empty()) return;
    const uint32_t count = static_cast<uint32_t>(pending_.size());
    UINT32 submitted = 0;
    const HRESULT submittedHr = api_->submit(ring_, count, 30000, &submitted);
    ++submissions_;
    entriesSubmitted_ += submitted;
    uint32_t completed = 0;
    const ULONGLONG deadline = ::GetTickCount64() + 60000;
    while (completed < count && ::GetTickCount64() < deadline) {
      IORING_CQE cqe{};
      const HRESULT popped = api_->pop(ring_, &cqe);
      if (popped == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) {
        if (FAILED(api_->submit(ring_, 1, 1000, nullptr))) break;
        continue;
      }
      if (FAILED(popped)) break;
      ++completed;
      Pending* done = FindPending(cqe.UserData);
      if (done == nullptr) continue;
      if (FAILED(cqe.ResultCode)) {
        failures_[done->handle] = detail::HResultWhy(L"IoRing completion", cqe.ResultCode);
      } else if (cqe.Information != done->bytes.size()) {
        failures_[done->handle] = L"IoRing short write: " + std::to_wstring(cqe.Information) +
                                  L" of " + std::to_wstring(done->bytes.size()) + L" bytes";
      }
    }
    if (completed < count) {
      const std::wstring reason =
          FAILED(submittedHr)
              ? detail::HResultWhy(L"SubmitIoRing", submittedHr)
              : L"IoRing submission did not complete before the 60 s deadline";
      for (const Pending& entry : pending_) {
        if (failures_.count(entry.handle) == 0) failures_[entry.handle] = reason;
      }
    }
    pending_.clear();
#endif
  }

  bool available() const { return available_; }
  const std::wstring& degradedReason() const { return degradedReason_; }
  uint32_t batchSize() const { return batchSize_; }
  uint64_t submissions() const { return submissions_; }
  uint64_t entriesSubmitted() const { return entriesSubmitted_; }
  uint64_t createCalls() const { return createCalls_; }
  uint64_t writeCalls() const { return writeCalls_; }
  uint64_t closeCalls() const { return closeCalls_; }
  const PrecreateBackend& precreate() const { return *precreate_; }

 private:
  struct Pending {
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::vector<uint8_t> bytes;
    uint64_t userData = 0;
  };

  Pending* FindPending(uint64_t userData) {
    for (Pending& entry : pending_) {
      if (entry.userData == userData) return &entry;
    }
    return nullptr;
  }

  std::unique_ptr<PrecreateBackend> precreate_;
  uint32_t batchSize_ = 32;
  bool available_ = false;
  std::wstring degradedReason_;
#if PRISM_WRITEBACK_HAS_IORING
  const IoRingApi* api_ = nullptr;
  HIORING ring_ = nullptr;
#endif
  std::vector<Pending> pending_;
  std::unordered_map<void*, std::wstring> failures_;
  uint64_t nextUserData_ = 1;
  uint64_t createCalls_ = 0;
  uint64_t writeCalls_ = 0;
  uint64_t closeCalls_ = 0;
  uint64_t submissions_ = 0;
  uint64_t entriesSubmitted_ = 0;
};

// --- large pages: call the engine's switch, then probe whether it can work ---
//
// 7z.dll exposes SetLargePageMode (card C1 already wraps it in
// SevenZipLibrary::EnableLargePages). That call is a REQUEST, not a guarantee:
// it only helps when the process holds SeLockMemoryPrivilege and the OS can hand
// out 2 MB pages. The probe below answers "did it actually become possible",
// step by step, and ProbeLargePages() reports the first missing step as
// "degraded: ..." -- the one thing this must never do is claim success because
// the export exists.
struct LargePageStatus {
  bool sevenZipExportPresent = false;  // 7z.dll is loaded and exports SetLargePageMode
  bool privilegeHeld = false;          // SeLockMemoryPrivilege is ENABLED in our token
  bool allocationWorks = false;        // a real MEM_LARGE_PAGES VirtualAlloc succeeded
  uint32_t largePageMinimum = 0;       // GetLargePageMinimum(), bytes
  uint32_t lastError = 0;
  std::wstring detail;

  // "Effective" means all three links of the chain held. Two out of three is a
  // request the OS will ignore, i.e. degraded.
  bool effective() const { return sevenZipExportPresent && privilegeHeld && allocationWorks; }
};

namespace detail {

// Enables SeLockMemoryPrivilege for this process and verifies it in the token's
// privilege set (AdjustTokenPrivileges can succeed while enabling nothing when
// the privilege is absent -- GetLastError() == ERROR_NOT_ALL_ASSIGNED).
inline bool EnableLockMemoryPrivilege(uint32_t* lastError) {
  HANDLE token = nullptr;
  if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_ADJUST_PRIVILEGES, &token)) {
    if (lastError) *lastError = ::GetLastError();
    return false;
  }
  bool enabled = false;
  uint32_t error = ERROR_SUCCESS;
  LUID luid{};
  // Explicit wide literal: SE_LOCK_MEMORY_NAME is a TEXT() macro and this
  // project builds non-UNICODE, so the macro would hand LookupPrivilegeValueW
  // a narrow string (the B5 failure class, one API over).
  if (!::LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &luid)) {
    error = ::GetLastError();
  } else {
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    ::SetLastError(ERROR_SUCCESS);
    if (!::AdjustTokenPrivileges(token, FALSE, &privileges, 0, nullptr, nullptr)) {
      error = ::GetLastError();
    } else {
      const uint32_t adjustError = ::GetLastError();
      if (adjustError != ERROR_SUCCESS) {
        error = adjustError;  // ERROR_NOT_ALL_ASSIGNED: the privilege is not ours
      } else {
        DWORD needed = 0;
        ::GetTokenInformation(token, TokenPrivileges, nullptr, 0, &needed);
        std::vector<uint8_t> buffer(needed == 0 ? 1 : needed, 0);
        if (::GetTokenInformation(token, TokenPrivileges, buffer.data(),
                                  static_cast<DWORD>(buffer.size()), &needed)) {
          const TOKEN_PRIVILEGES* held = reinterpret_cast<const TOKEN_PRIVILEGES*>(buffer.data());
          for (DWORD i = 0; i < held->PrivilegeCount; ++i) {
            if (held->Privileges[i].Luid.LowPart == luid.LowPart &&
                held->Privileges[i].Luid.HighPart == luid.HighPart &&
                (held->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) != 0) {
              enabled = true;
              break;
            }
          }
          if (!enabled) error = ERROR_PRIVILEGE_NOT_HELD;
        } else {
          error = ::GetLastError();
        }
      }
    }
  }
  ::CloseHandle(token);
  if (lastError) *lastError = error;
  return enabled;
}

inline std::wstring JoinReasons(const std::vector<std::wstring>& reasons) {
  std::wstring joined;
  for (size_t i = 0; i < reasons.size(); ++i) {
    if (i != 0) joined += L"; ";
    joined += reasons[i];
  }
  return joined;
}

}  // namespace detail

// Call AFTER SevenZipLibrary::Load() + EnableLargePages(): the export check uses
// GetModuleHandleW, i.e. it reports the module the process actually has loaded,
// not whether a file on disk would have the export.
inline LargePageStatus ProbeLargePages(const std::wstring& sevenZipDllPath) {
  LargePageStatus status;
  if (!sevenZipDllPath.empty()) {
    HMODULE module = ::GetModuleHandleW(sevenZipDllPath.c_str());
    if (module == nullptr) {
      const size_t slash = sevenZipDllPath.find_last_of(L"\\/");
      const std::wstring base =
          (slash == std::wstring::npos) ? sevenZipDllPath : sevenZipDllPath.substr(slash + 1);
      module = ::GetModuleHandleW(base.c_str());
    }
    status.sevenZipExportPresent =
        module != nullptr && ::GetProcAddress(module, "SetLargePageMode") != nullptr;
  }
  status.largePageMinimum = static_cast<uint32_t>(::GetLargePageMinimum());

  uint32_t privilegeError = ERROR_SUCCESS;
  status.privilegeHeld = detail::EnableLockMemoryPrivilege(&privilegeError);

  if (status.privilegeHeld && status.largePageMinimum > 0) {
    void* block = ::VirtualAlloc(nullptr, status.largePageMinimum,
                                 MEM_RESERVE | MEM_COMMIT | MEM_LARGE_PAGES, PAGE_READWRITE);
    status.allocationWorks = block != nullptr;
    status.lastError = block != nullptr ? ERROR_SUCCESS : ::GetLastError();
    if (block != nullptr) ::VirtualFree(block, 0, MEM_RELEASE);
  } else {
    status.lastError = status.privilegeHeld ? ERROR_NOT_ENOUGH_MEMORY : privilegeError;
  }

  if (status.effective()) {
    status.detail = L"enabled: SetLargePageMode export present, SeLockMemoryPrivilege held, " +
                    std::to_wstring(status.largePageMinimum) + L"-byte VirtualAlloc succeeded";
    return status;
  }

  std::vector<std::wstring> reasons;
  if (!status.sevenZipExportPresent) {
    reasons.push_back(L"7z.dll is not loaded or does not export SetLargePageMode");
  }
  if (!status.privilegeHeld) {
    reasons.push_back(L"SeLockMemoryPrivilege is not assigned to this token (GetLastError=" +
                      std::to_wstring(privilegeError) + L")");
  }
  if (status.largePageMinimum == 0) {
    reasons.push_back(L"GetLargePageMinimum() reports 0 (large pages unsupported here)");
  } else if (status.privilegeHeld && !status.allocationWorks) {
    reasons.push_back(L"MEM_LARGE_PAGES VirtualAlloc failed (GetLastError=" +
                      std::to_wstring(status.lastError) + L")");
  }
  status.detail = L"degraded: " + detail::JoinReasons(reasons);
  return status;
}

}  // namespace prism::win::writeback
