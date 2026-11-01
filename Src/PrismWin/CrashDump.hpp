// PrismArchive -- PrismWin
// CrashDump: the last line of defence. SetUnhandledExceptionFilter +
// MiniDumpWriteDump into %LOCALAPPDATA%\PrismArchive\crash\, with the last N
// formatted log records attached next to the dump.
//
// WHY A MINIDUMP *AND* A TEXT REPORT
//   A .dmp is only useful to someone with a debugger and the matching binaries.
//   The text report next to it is readable by the user, by a bug report, and by
//   grep in CI -- and it is where the log tail lives. Both files are named from
//   the same stamp, so a pair can never be confused for two different crashes.
//
// THE CONTRACT THAT MAKES THE REPORT SAFE TO SHARE (CONTRACTS B7)
//   The log lines written here were already redacted by prism::log::Logger
//   BEFORE they were formatted (Log.hpp: redaction happens before formatting, so
//   no sink can receive a secret). The dump is therefore credential-free by
//   construction; the Windows suite proves it by crashing a child process with a
//   password-bearing record in its ring and asserting the password is absent.
//
// HONEST LIMITS (stated so nobody trusts this further than it goes)
//   * The writer takes no locks and allocates. A crash inside the heap or inside
//     the loader lock can therefore make this fail or hang; the filter then
//     returns EXCEPTION_CONTINUE_SEARCH and Windows does what it always did.
//   * dbghelp.dll is loaded at crash time with LoadLibraryW/GetProcAddress, NOT
//     linked: a missing or wrong-architecture dbghelp must degrade to "text
//     report only", never to a startup failure of the application.
//   * A failed dump deletes its partial file and records
//     "dump-status: failed win32=<n>" in the text report, because a truncated
//     .dmp that looks like evidence is worse than no evidence (same rule as
//     CONTRACTS D2: assert CONTENT, not existence).
#pragma once

#include <windows.h>

#include <dbghelp.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "../PrismCore/Log.hpp"

namespace prism::win::crash {

inline constexpr unsigned kReportVersion = 1;
inline constexpr size_t kDefaultLogTail = 64;
inline constexpr const wchar_t* kCrashSubdirectory = L"PrismArchive\\crash";

// The report is UTF-8 and one record per line; path separators stay native
// because the report is also opened by humans in Explorer.
struct Options {
  std::wstring directory;  // empty => DefaultCrashDirectory()
  const prism::log::RingBuffer* ring = nullptr;
  size_t logTail = kDefaultLogTail;
  MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal);
  // Tests pin these so a run cannot be confused with another one; production
  // leaves them empty/0 and the handler stamps the crash time.
  std::wstring fileNameStamp;
  unsigned long sequence = 0;
};

struct Paths {
  std::wstring directory;
  std::wstring dumpPath;
  std::wstring logPath;
};

struct Outcome {
  bool logWritten = false;
  bool dumpWritten = false;
  DWORD dumpError = 0;
  std::wstring directory;
  std::wstring dumpPath;
  std::wstring logPath;
  size_t logTailCount = 0;
};

namespace detail {

inline std::string Utf8FromWide(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

inline std::wstring WideFromUtf8(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
  return out;
}

inline std::wstring EnvValue(const wchar_t* name) {
  const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) return std::wstring();
  std::wstring value(static_cast<size_t>(needed), L'\0');
  const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
  if (written == 0 || written >= needed) return std::wstring();
  value.resize(static_cast<size_t>(written));
  return value;
}

inline std::wstring TrimTrailingSeparators(std::wstring path) {
  while (path.size() > 1) {
    const wchar_t last = path.back();
    if (last != L'\\' && last != L'/') break;
    // Keep "C:\" intact.
    if (path.size() == 3 && path[1] == L':') break;
    path.pop_back();
  }
  return path;
}

inline std::wstring JoinPath(const std::wstring& directory, const std::wstring& name) {
  if (directory.empty()) return name;
  std::wstring out = TrimTrailingSeparators(directory);
  if (!out.empty() && out.back() != L'\\') out.push_back(L'\\');
  out += name;
  return out;
}

inline std::string Hex32(unsigned long value) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), "0x%08lX", value);
  return std::string(buffer);
}

inline std::string HexPointer(std::uintptr_t value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "0x%llX", static_cast<unsigned long long>(value));
  return std::string(buffer);
}

inline int64_t NowEpochMillis() {
  FILETIME fileTime;
  GetSystemTimeAsFileTime(&fileTime);
  ULARGE_INTEGER ticks;
  ticks.LowPart = fileTime.dwLowDateTime;
  ticks.HighPart = fileTime.dwHighDateTime;
  // 100-ns ticks since 1601 -> ms since 1970.
  const std::uint64_t epochTicks = 116444736000000000ull;
  if (ticks.QuadPart <= epochTicks) return 0;
  return static_cast<int64_t>((ticks.QuadPart - epochTicks) / 10000ull);
}

inline std::wstring FormatFileStamp(const SYSTEMTIME& utc) {
  wchar_t buffer[32];
  std::swprintf(buffer, sizeof(buffer) / sizeof(buffer[0]), L"%04u%02u%02u-%02u%02u%02u",
                static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth),
                static_cast<unsigned>(utc.wDay), static_cast<unsigned>(utc.wHour),
                static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond));
  return std::wstring(buffer);
}

// One directory component at a time: CREATE_NEW on "%LOCALAPPDATA%\PrismArchive"
// must not fail when it already exists, and this must work under a TEMP that has
// never seen the product.
inline bool EnsureDirectory(const std::wstring& directory) {
  const std::wstring trimmed = TrimTrailingSeparators(directory);
  if (trimmed.empty()) return false;
  std::wstring partial;
  size_t index = 0;
  // Preserve a drive prefix ("C:") and a UNC prefix ("\\server\share").
  if (trimmed.size() >= 3 && trimmed[1] == L':') {
    partial = trimmed.substr(0, 3);
    index = 3;
  } else if (trimmed.rfind(L"\\\\", 0) == 0) {
    const size_t share = trimmed.find(L'\\', 2);
    if (share == std::wstring::npos) return false;
    const size_t next = trimmed.find(L'\\', share + 1);
    if (next == std::wstring::npos) return true;  // the share itself is the leaf
    partial = trimmed.substr(0, next + 1);
    index = next + 1;
  }
  while (index < trimmed.size()) {
    while (index < trimmed.size() && trimmed[index] == L'\\') ++index;
    if (index >= trimmed.size()) break;
    const size_t next = trimmed.find(L'\\', index);
    const size_t end = next == std::wstring::npos ? trimmed.size() : next;
    partial += trimmed.substr(index, end - index);
    if (!CreateDirectoryW(partial.c_str(), nullptr) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
      return false;
    }
    if (end < trimmed.size()) partial.push_back(L'\\');
    index = end;
  }
  return true;
}

inline bool WriteWholeFile(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  size_t written = 0;
  bool ok = true;
  while (written < bytes.size()) {
    DWORD chunk = 0;
    const DWORD want = static_cast<DWORD>((std::min)(bytes.size() - written,
                                                     static_cast<size_t>(1) << 20));
    if (!WriteFile(file, bytes.data() + written, want, &chunk, nullptr) || chunk == 0) {
      ok = false;
      break;
    }
    written += chunk;
  }
  CloseHandle(file);
  if (!ok) DeleteFileW(path.c_str());
  return ok;
}

inline bool AppendToFile(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                            nullptr) != FALSE &&
                  written == bytes.size();
  CloseHandle(file);
  return ok;
}

// dbghelp is resolved at crash time on purpose: a missing/incorrect dbghelp must
// cost the dump, not the application.
using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                                          PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

inline MiniDumpWriteDumpFn ResolveMiniDumpWriteDump() {
  static bool attempted = false;
  static MiniDumpWriteDumpFn resolved = nullptr;
  if (!attempted) {
    attempted = true;
    HMODULE module = LoadLibraryW(L"dbghelp.dll");
    if (module != nullptr) {
      const FARPROC proc = GetProcAddress(module, "MiniDumpWriteDump");
      void* raw = nullptr;
      static_assert(sizeof(raw) == sizeof(proc), "function pointer / FARPROC size mismatch");
      std::memcpy(&raw, &proc, sizeof(raw));
      std::memcpy(&resolved, &raw, sizeof(resolved));
    }
  }
  return resolved;
}

}  // namespace detail

// %LOCALAPPDATA%\PrismArchive\crash, then %TEMP%\PrismArchive\crash, then a
// relative fallback. Never empty: the caller always has a directory to try.
inline std::wstring DefaultCrashDirectory() {
  const std::wstring localAppData = detail::EnvValue(L"LOCALAPPDATA");
  if (!localAppData.empty()) {
    return detail::JoinPath(localAppData, kCrashSubdirectory);
  }
  const std::wstring temp = detail::EnvValue(L"TEMP");
  if (!temp.empty()) return detail::JoinPath(temp, kCrashSubdirectory);
  return L"crash";
}

inline Paths MakePaths(const std::wstring& directory, const std::wstring& stamp,
                       unsigned long processId, unsigned long sequence) {
  Paths paths;
  paths.directory = directory;
  std::wstring name = L"crash-" + stamp + L"-" + std::to_wstring(processId);
  if (sequence > 0) name += L"-" + std::to_wstring(sequence);
  paths.dumpPath = detail::JoinPath(directory, name + L".dmp");
  paths.logPath = detail::JoinPath(directory, name + L".txt");
  return paths;
}

// Writes the text report (always, first) and then the minidump (best effort).
// Callable with a fabricated EXCEPTION_POINTERS, which is how the suite proves
// the writer works without crashing anything.
inline Outcome WriteCrashReport(const EXCEPTION_POINTERS* info, const Options& options) {
  Outcome outcome;
  const std::wstring directory =
      options.directory.empty() ? DefaultCrashDirectory() : options.directory;
  outcome.directory = directory;
  if (!detail::EnsureDirectory(directory)) return outcome;

  SYSTEMTIME utc;
  GetSystemTime(&utc);
  const std::wstring stamp =
      options.fileNameStamp.empty() ? detail::FormatFileStamp(utc) : options.fileNameStamp;
  const Paths paths = MakePaths(directory, stamp, static_cast<unsigned long>(GetCurrentProcessId()),
                                options.sequence);
  outcome.dumpPath = paths.dumpPath;
  outcome.logPath = paths.logPath;

  std::string report;
  report += "PrismArchive crash report\n";
  report += "report-version: " + std::to_string(kReportVersion) + "\n";
  report += "time: " + prism::log::FormatUtcTimestamp(detail::NowEpochMillis()) + "\n";
  report += "process-id: " + std::to_string(static_cast<unsigned long>(GetCurrentProcessId())) + "\n";

  DWORD exceptionCode = 0;
  DWORD exceptionFlags = 0;
  std::uintptr_t exceptionAddress = 0;
  if (info != nullptr && info->ExceptionRecord != nullptr) {
    exceptionCode = info->ExceptionRecord->ExceptionCode;
    exceptionFlags = info->ExceptionRecord->ExceptionFlags;
    exceptionAddress = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  }
  report += "exception-code: " + detail::Hex32(static_cast<unsigned long>(exceptionCode)) + "\n";
  report += "exception-flags: " + detail::Hex32(static_cast<unsigned long>(exceptionFlags)) + "\n";
  report += "exception-address: " + detail::HexPointer(exceptionAddress) + "\n";
  report += "dump: " + detail::Utf8FromWide(paths.dumpPath) + "\n";

  std::vector<std::string> tail;
  if (options.ring != nullptr && options.logTail > 0) {
    tail = options.ring->Last(options.logTail);
  }
  outcome.logTailCount = tail.size();
  report += "log-tail-count: " + std::to_string(tail.size()) + "\n";
  if (options.ring != nullptr) {
    report += "log-ring-size: " + std::to_string(options.ring->size()) + "/" +
              std::to_string(options.ring->capacity()) + "\n";
  }
  report += "--- crash-time log tail (oldest first) ---\n";
  for (const std::string& line : tail) {
    report += line;
    report += '\n';
  }

  outcome.logWritten = detail::WriteWholeFile(paths.logPath, report);

  // The dump: self-dump of the crashing process.
  bool dumpOk = false;
  DWORD dumpError = 0;
  if (const detail::MiniDumpWriteDumpFn write = detail::ResolveMiniDumpWriteDump()) {
    HANDLE file = CreateFileW(paths.dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      dumpError = GetLastError();
    } else {
      MINIDUMP_EXCEPTION_INFORMATION exceptionInfo{};
      exceptionInfo.ThreadId = GetCurrentThreadId();
      exceptionInfo.ExceptionPointers = const_cast<EXCEPTION_POINTERS*>(info);
      exceptionInfo.ClientPointers = FALSE;
      const BOOL ok = write(GetCurrentProcess(), GetCurrentProcessId(), file, options.dumpType,
                            info != nullptr ? &exceptionInfo : nullptr, nullptr, nullptr);
      if (!ok) dumpError = GetLastError();
      CloseHandle(file);
      dumpOk = ok != FALSE;
      if (!dumpOk) {
        // A truncated .dmp that looks like evidence is worse than no evidence.
        DeleteFileW(paths.dumpPath.c_str());
      }
    }
  } else {
    dumpError = ERROR_MOD_NOT_FOUND;
  }
  outcome.dumpWritten = dumpOk;
  outcome.dumpError = dumpOk ? 0 : (dumpError == 0 ? ERROR_GEN_FAILURE : dumpError);

  // The verdict is appended AFTER the attempt, so the pair is self-describing:
  // "dump-status: ok" and a real MDMP file, or the reason it is not there.
  std::string status = "dump-status: ";
  if (dumpOk) {
    status += "ok\n";
  } else {
    status += "failed win32=" + std::to_string(static_cast<unsigned long>(outcome.dumpError)) +
              "\n";
  }
  if (outcome.logWritten) detail::AppendToFile(paths.logPath, status);
  return outcome;
}

namespace detail {

// Installed state lives in the module, not on a caller's stack: the process may
// crash long after Install() returned.
inline Options*& InstalledOptions() {
  static Options* options = nullptr;
  return options;
}

inline LONG& HandlerDepth() {
  static LONG depth = 0;
  return depth;
}

inline LONG WINAPI UnhandledExceptionFilterHandler(EXCEPTION_POINTERS* info) {
  // Re-entrancy: a crash while writing the report must not recurse.
  if (InterlockedIncrement(&HandlerDepth()) > 1) return EXCEPTION_CONTINUE_SEARCH;
  const Options* options = InstalledOptions();
  if (options == nullptr) return EXCEPTION_CONTINUE_SEARCH;
  WriteCrashReport(info, *options);
  return EXCEPTION_EXECUTE_HANDLER;
}

}  // namespace detail

// Installs the filter. Returns false only when the OS refuses the install.
inline bool Install(const Options& options) {
  static Options stored;
  stored = options;
  detail::InstalledOptions() = &stored;
  // No modal system box on a machine with no interactive console (CI, kiosk),
  // and no "insert disk" prompts from the crash path.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  return SetUnhandledExceptionFilter(&detail::UnhandledExceptionFilterHandler) != nullptr ||
         GetLastError() == ERROR_SUCCESS;
}

inline void Uninstall() {
  detail::InstalledOptions() = nullptr;
  SetUnhandledExceptionFilter(nullptr);
}

inline bool IsInstalled() { return detail::InstalledOptions() != nullptr; }

}  // namespace prism::win::crash
