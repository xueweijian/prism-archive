// PrismArchive -- PrismWin
// Settings file layer: where the settings document lives and how it reaches
// disk without ever existing in a half-written state (blueprint ch. 8.4).
//
// THE THREE INVARIANTS
//   1. One location: %LOCALAPPDATA%\PrismArchive\settings.json. The environment
//      variable is read first because that is the only hook a test (or a
//      portable install) has to redirect the folder; SHGetKnownFolderPath is the
//      fallback for a process started without a shell environment.
//   2. Atomic replacement: bytes are written to a sibling temp file, flushed, and
//      then swapped in with ReplaceFileW, so a reader sees either the previous
//      document or the new one, never a truncated one. A fresh file, where there
//      is nothing to replace, is a single rename. If the platform refuses
//      ReplaceFileW a rename-over is used instead and reported, rather than
//      failing the save the user asked for.
//      NOTE: ReplaceFileW moves the old document to the backup name before it
//      moves the replacement in, so for a few microseconds the PATH can be
//      absent (its content is never torn). LoadStore therefore retries a missing
//      file once before concluding "first run" -- otherwise a second instance
//      saving at that instant could make this one write defaults over it.
//   3. Recovery leaves evidence: a document the parser rejects is MOVED aside to
//      settings.json.corrupt-<time>.bak byte-for-byte before defaults are
//      written. "The app forgot my settings" must always be diagnosable from
//      the disk, and a copy (rather than a move) would leave the broken file in
//      place for the next start.
//
// The policy -- schema, defaults, migration, validation, unknown-key
// preservation -- lives in PrismCore/SettingsStore.hpp and is unit-tested on
// every lane. This header is only the system calls.
//
// CONTRACTS: B4 (Win32 paths are built with backslashes; no '\\?\' prefix is
// needed because the target is a normal per-user path), D1 (nothing here skips
// silently -- an unresolvable folder is a failure), D2 (recovery asserts the
// backup's BYTES, not that a file appeared).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <combaseapi.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "SettingsStore.hpp"

namespace prism::win::settingsfile {

inline constexpr wchar_t kAppFolder[] = L"PrismArchive";
inline constexpr wchar_t kSettingsFile[] = L"settings.json";

struct Paths {
  std::wstring root;  // %LOCALAPPDATA%\PrismArchive
  std::wstring file;  // root\settings.json

  bool valid() const { return !root.empty() && !file.empty(); }
};

inline std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  if (text.size() > 0x7FFFFFF0u) return {};
  const int length = static_cast<int>(text.size());
  const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr,
                                         nullptr);
  if (needed <= 0) return {};
  std::string out(static_cast<std::size_t>(needed), '\0');
  if (WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), needed, nullptr, nullptr) !=
      needed) {
    return {};
  }
  return out;
}

inline std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  if (text.size() > 0x7FFFFFF0u) return {};
  const int length = static_cast<int>(text.size());
  const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
  if (needed <= 0) return {};
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  if (MultiByteToWideChar(CP_UTF8, 0, text.data(), length, out.data(), needed) != needed) {
    return {};
  }
  return out;
}

inline std::wstring DirectoryOf(const std::wstring& path) {
  const std::size_t slash = path.find_last_of(L"\\/");
  if (slash == std::wstring::npos) return {};
  return path.substr(0, slash);
}

inline std::wstring TrimTrailingSeparators(std::wstring value) {
  // Keep the root ("C:\\"); everything below it loses trailing separators.
  while (value.size() > 3 && (value.back() == L'\\' || value.back() == L'/')) value.pop_back();
  if (value.size() == 3 && value[1] == L':' && value[2] == L'\\') value.pop_back();
  return value;
}

// Creates every missing component of a drive-rooted path. UNC paths are not
// supported on purpose: %LOCALAPPDATA% is always a local drive path, and
// half-supporting UNC here would hide that assumption.
inline bool EnsureDirectory(const std::wstring& directory) {
  if (directory.empty()) return false;
  if (GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  std::wstring current;
  std::size_t index = 0;
  if (directory.size() >= 2 && directory[1] == L':') {
    current = directory.substr(0, 2);
    index = 2;
  }
  while (index < directory.size()) {
    while (index < directory.size() && (directory[index] == L'\\' || directory[index] == L'/')) {
      ++index;
    }
    const std::size_t start = index;
    while (index < directory.size() && directory[index] != L'\\' && directory[index] != L'/') {
      ++index;
    }
    if (start == index) break;
    current += L"\\";
    current += directory.substr(start, index - start);
    if (GetFileAttributesW(current.c_str()) == INVALID_FILE_ATTRIBUTES) {
      if (!CreateDirectoryW(current.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
      }
    }
  }
  return GetFileAttributesW(directory.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline std::wstring LocalAppDataDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", buffer, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return TrimTrailingSeparators(std::wstring(buffer, length));
  PWSTR raw = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw)) &&
      raw != nullptr) {
    std::wstring value(raw);
    CoTaskMemFree(raw);
    return TrimTrailingSeparators(value);
  }
  return {};
}

inline Paths PathsUnder(std::wstring_view localAppData) {
  Paths paths;
  const std::wstring base = TrimTrailingSeparators(std::wstring(localAppData));
  if (base.empty()) return paths;
  paths.root = base + L"\\" + kAppFolder;
  paths.file = paths.root + L"\\" + kSettingsFile;
  return paths;
}

inline Paths DefaultPaths() { return PathsUnder(LocalAppDataDirectory()); }

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

// Reads the whole file. A missing file is reported as a false return with
// ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND, which the store loader treats as
// "first run" rather than as an error.
inline bool ReadAllBytes(const std::wstring& path, std::string& bytes, DWORD* error) {
  bytes.clear();
  HANDLE handle =
      CreateFileW(path.c_str(), GENERIC_READ,
                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  char buffer[64 * 1024];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(sizeof buffer), &read, nullptr)) {
      if (error) *error = GetLastError();
      CloseHandle(handle);
      return false;
    }
    if (read == 0) break;
    bytes.append(buffer, static_cast<std::size_t>(read));
  }
  CloseHandle(handle);
  if (error) *error = 0;
  return true;
}

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

enum class SaveMechanism {
  None,            // nothing was written
  MoveFile,        // fresh file, or the rename fallback (see SaveResult::fellBack)
  ReplaceFile,     // the contract: temp file + ReplaceFileW
  InPlaceMutant,   // negative-control build only: truncated and streamed in place
};

inline const char* SaveMechanismName(SaveMechanism mechanism) {
  switch (mechanism) {
    case SaveMechanism::None: return "none";
    case SaveMechanism::MoveFile: return "move-file";
    case SaveMechanism::ReplaceFile: return "replace-file";
    case SaveMechanism::InPlaceMutant: return "in-place-mutant";
  }
  return "?";
}

struct SaveResult {
  bool ok = false;
  SaveMechanism mechanism = SaveMechanism::None;
  DWORD error = 0;
  std::wstring tempPath;  // the sibling temp file used (empty when none was needed)
  bool fellBack = false;  // ReplaceFileW refused; a rename-over was used instead
};

inline std::wstring UniqueTempPath(const std::wstring& file) {
  static std::atomic<unsigned long> counter{0};
  const unsigned long serial = counter.fetch_add(1) + 1;
  return file + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
         std::to_wstring(serial);
}

inline bool WriteChunk(HANDLE handle, std::string_view bytes, DWORD* error) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const std::size_t remaining = bytes.size() - offset;
    const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<std::size_t>(1u << 20)));
    DWORD written = 0;
    if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) || written == 0) {
      if (error) *error = GetLastError();
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

// A rename that transiently fails because a reader holds the target is retried:
// losing that race would surface as "settings could not be saved", and the whole
// point of writing a temp file first is that the retry costs nothing.
inline bool MoveIntoPlace(const std::wstring& from, const std::wstring& to, DWORD flags,
                          DWORD* error) {
  DWORD last = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (MoveFileExW(from.c_str(), to.c_str(), flags)) {
      if (error) *error = 0;
      return true;
    }
    last = GetLastError();
    if (last != ERROR_SHARING_VIOLATION && last != ERROR_ACCESS_DENIED &&
        last != ERROR_LOCK_VIOLATION) {
      break;
    }
    Sleep(5);
  }
  if (error) *error = last;
  return false;
}

inline bool WriteAndFlush(const std::wstring& path, std::string_view bytes, DWORD* error) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  bool ok = WriteChunk(handle, bytes, error);
  if (ok && !FlushFileBuffers(handle)) {
    if (error) *error = GetLastError();
    ok = false;
  }
  if (!CloseHandle(handle) && ok) {
    if (error) *error = GetLastError();
    ok = false;
  }
  return ok;
}

#if defined(PRISM_C14_MUTANT_NONATOMIC_SAVE)
// NEGATIVE CONTROL (CONTRACTS D3/D5): the save path every naive implementation
// starts with -- truncate the live file and stream the payload into it. The
// 2 ms gap between the halves is the observability amplifier: an unobservable
// mutation would prove nothing about the observer. The concurrent-reader case
// in Tests/win/test_settings_file.cpp MUST catch the torn window; the CTest
// entry is WILL_FAIL, so red there is green, and a mutation that stopped
// applying would invert the verdict.
inline bool WriteInPlaceTorn(const std::wstring& file, std::string_view bytes, DWORD* error) {
  HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  const std::size_t half = bytes.size() / 2;
  bool ok = WriteChunk(handle, bytes.substr(0, half), error);
  if (ok) {
    Sleep(2);
    ok = WriteChunk(handle, bytes.substr(half), error);
  }
  if (ok && !FlushFileBuffers(handle)) {
    if (error) *error = GetLastError();
    ok = false;
  }
  CloseHandle(handle);
  return ok;
}
#endif

// Writes the document so that a concurrent reader observes either the previous
// content or the new content -- never a truncated one.
inline bool SaveFileAtomic(const std::wstring& file, std::string_view bytes, SaveResult* result) {
  SaveResult local;
  SaveResult* out = result != nullptr ? result : &local;
  *out = SaveResult{};
  if (file.empty()) {
    out->error = ERROR_INVALID_PARAMETER;
    return false;
  }
  const std::wstring directory = DirectoryOf(file);
  if (!EnsureDirectory(directory)) {
    out->error = GetLastError();
    return false;
  }

#if defined(PRISM_C14_MUTANT_NONATOMIC_SAVE)
  out->mechanism = SaveMechanism::InPlaceMutant;
  DWORD mutantError = 0;
  if (!WriteInPlaceTorn(file, bytes, &mutantError)) {
    out->error = mutantError;
    return false;
  }
  out->ok = true;
  return true;
#else
  const std::wstring temp = UniqueTempPath(file);
  DWORD writeError = 0;
  if (!WriteAndFlush(temp, bytes, &writeError)) {
    DeleteFileW(temp.c_str());
    out->error = writeError;
    return false;
  }
  out->tempPath = temp;

  // Fresh file: a rename is the only step, so nothing can be observed halfway.
  if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) {
    DWORD moveError = 0;
    bool moved = MoveIntoPlace(temp, file, MOVEFILE_WRITE_THROUGH, &moveError);
    if (!moved && moveError == ERROR_ALREADY_EXISTS) {
      // Another instance created the file inside that window. A replace rename
      // is still one atomic step, and losing the save here would be a failure
      // the user asked for nothing to do with.
      moved = MoveIntoPlace(temp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH,
                            &moveError);
      out->fellBack = moved;
    }
    if (!moved) {
      out->error = moveError;
      DeleteFileW(temp.c_str());
      return false;
    }
    out->mechanism = SaveMechanism::MoveFile;
    out->ok = true;
    return true;
  }

  // ReplaceFileW keeps the target's identity/ACLs and is the documented
  // "replace a document" primitive. No backup name is passed: two instances
  // saving at once would race for that one name (ERROR_ALREADY_EXISTS), and the
  // recovery path already owns the "keep the bad bytes" job.
  DWORD replaceError = 0;
  for (int attempt = 0; attempt < 20; ++attempt) {
    if (ReplaceFileW(file.c_str(), temp.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr,
                     nullptr)) {
      out->mechanism = SaveMechanism::ReplaceFile;
      out->ok = true;
      return true;
    }
    replaceError = GetLastError();
    if (replaceError != ERROR_SHARING_VIOLATION && replaceError != ERROR_ACCESS_DENIED &&
        replaceError != ERROR_LOCK_VIOLATION && replaceError != ERROR_ALREADY_EXISTS) {
      break;
    }
    Sleep(5);
  }

  // A single rename over the target is still atomic on NTFS. Falling back keeps
  // the user's save, and SaveResult::fellBack carries the fact that the
  // preferred mechanism was refused (the file-layer suite asserts the preferred
  // one on a normal machine, so a silent regression here goes red).
  DWORD moveError = 0;
  if (MoveIntoPlace(temp, file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH, &moveError)) {
    out->mechanism = SaveMechanism::MoveFile;
    out->fellBack = true;
    out->ok = true;
    return true;
  }
  out->error = replaceError != 0 ? replaceError : moveError;
  DeleteFileW(temp.c_str());
  return false;
#endif
}

inline bool SaveStore(const prism::settings::SettingsStore& store, const Paths& paths,
                      SaveResult* result) {
  SaveResult local;
  SaveResult* out = result != nullptr ? result : &local;
  if (!paths.valid()) {
    *out = SaveResult{};
    out->error = ERROR_INVALID_PARAMETER;
    return false;
  }
  if (!store.writable()) {
    // A document from a newer build: writing our view of it would delete every
    // field we cannot name. Refusing is the whole point of the TooNew status.
    *out = SaveResult{};
    out->error = ERROR_WRITE_PROTECT;
    return false;
  }
  const std::string bytes = store.ToJson();
  return SaveFileAtomic(paths.file, bytes, out);
}

// ---------------------------------------------------------------------------
// Load + recovery
// ---------------------------------------------------------------------------

enum class LoadOutcome {
  Loaded,                // an existing document was read (its LoadStatus says how)
  FreshDefaults,         // no document yet: defaults computed and written
  RecoveredFromCorrupt,  // bad document moved to a backup, defaults written
  Failed,                // I/O refused; the on-disk state was left untouched
};

inline const char* LoadOutcomeName(LoadOutcome outcome) {
  switch (outcome) {
    case LoadOutcome::Loaded: return "loaded";
    case LoadOutcome::FreshDefaults: return "fresh-defaults";
    case LoadOutcome::RecoveredFromCorrupt: return "recovered-from-corrupt";
    case LoadOutcome::Failed: return "failed";
  }
  return "?";
}

struct LoadReport {
  LoadOutcome outcome = LoadOutcome::Failed;
  prism::settings::LoadStatus storeStatus = prism::settings::LoadStatus::Ok;
  std::wstring path;
  std::wstring backupPath;  // set when RecoveredFromCorrupt
  std::string detail;
  DWORD error = 0;
};

inline std::wstring NextBackupPath(const std::wstring& file) {
  FILETIME now = {};
  GetSystemTimeAsFileTime(&now);
  const unsigned long long stamp =
      (static_cast<unsigned long long>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
  std::wstring candidate = file + L".corrupt-" + std::to_wstring(stamp) + L".bak";
  for (int attempt = 1; attempt < 1000; ++attempt) {
    if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) break;
    candidate = file + L".corrupt-" + std::to_wstring(stamp) + L"-" + std::to_wstring(attempt) +
                L".bak";
  }
  return candidate;
}

// Loads the settings document, recovering from corruption by MOVING the bad
// bytes aside first. The backup is the only copy of what the user had -- it is
// never deleted, and it is never truncated.
inline LoadReport LoadStore(prism::settings::SettingsStore& store, const Paths& paths) {
  LoadReport report;
  report.path = paths.file;
  if (!paths.valid()) {
    report.outcome = LoadOutcome::Failed;
    report.error = ERROR_INVALID_PARAMETER;
    report.detail = "the settings location could not be resolved";
    return report;
  }

  std::string bytes;
  DWORD readError = 0;
  bool read = ReadAllBytes(paths.file, bytes, &readError);
  if (!read && (readError == ERROR_FILE_NOT_FOUND || readError == ERROR_PATH_NOT_FOUND)) {
    // See the header note: ReplaceFileW can leave the path absent for a few
    // microseconds. Writing defaults here would hand a second instance's save a
    // "first run" and overwrite it, so a missing path is retried once.
    Sleep(2);
    read = ReadAllBytes(paths.file, bytes, &readError);
  }
  if (!read) {
    if (readError == ERROR_FILE_NOT_FOUND || readError == ERROR_PATH_NOT_FOUND) {
      store.ResetToDefaults();
      SaveResult saved;
      if (!SaveStore(store, paths, &saved)) {
        report.outcome = LoadOutcome::Failed;
        report.error = saved.error;
        report.detail = "no settings file yet, and the defaults could not be written";
        report.storeStatus = store.report().status;
        return report;
      }
      report.outcome = LoadOutcome::FreshDefaults;
      report.storeStatus = store.report().status;
      return report;
    }
    report.outcome = LoadOutcome::Failed;
    report.error = readError;
    report.detail = "the settings file could not be read";
    return report;
  }

  const prism::settings::LoadReport loaded = store.Load(bytes);
  report.storeStatus = loaded.status;
  if (loaded.status != prism::settings::LoadStatus::Corrupt) {
    report.outcome = LoadOutcome::Loaded;
    return report;
  }

  const std::wstring backup = NextBackupPath(paths.file);
  if (!MoveFileExW(paths.file.c_str(), backup.c_str(), MOVEFILE_WRITE_THROUGH)) {
    const DWORD moveError = GetLastError();
    // Cross-device / ACL fallback: copy then drop the original. If neither
    // works, leave the file alone and report it -- never overwrite the only
    // copy of the user's data.
    if (!CopyFileW(paths.file.c_str(), backup.c_str(), FALSE) ||
        !DeleteFileW(paths.file.c_str())) {
      report.outcome = LoadOutcome::Failed;
      report.error = moveError;
      report.detail = "the corrupt settings file could not be backed up, so it was left untouched";
      return report;
    }
  }
  report.backupPath = backup;

  store.ResetToDefaults();
  SaveResult saved;
  if (!SaveStore(store, paths, &saved)) {
    report.outcome = LoadOutcome::Failed;
    report.error = saved.error;
    report.detail = "the corrupt document was backed up, but the defaults could not be written";
    // storeStatus keeps saying Corrupt: that is what the document was. The
    // store itself is at defaults now; ask it, do not overwrite the evidence.
    return report;
  }
  report.outcome = LoadOutcome::RecoveredFromCorrupt;
  return report;
}

}  // namespace prism::win::settingsfile
