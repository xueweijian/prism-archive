// Shared scratch-directory policy for the Windows integration suites (card C36).
//
// WHY THIS FILE EXISTS
// --------------------
// With "ctest -j 8" on the Windows lane, eight suites author fixtures at the
// same instant, and every one of them used to spell its scratch directory as a
// CONSTANT under %TEMP%: "prism_c11_reader_pool", "prism_solid_sweep",
// "prism_c25_wiring", "prism_write_archive", ... Different suites happened to
// use different constants, so they did not collide with each other -- until a
// second worktree, a second card, or simply two runs did. That is exactly how
// win.reader_pool started failing with "7z.exe could not author the solid
// fixture archive" the first time the lane went parallel: one process was
// wiping a directory another process had just packed into.
//
// A fixed name is also a false-green generator: a directory left behind by a
// crashed run is picked up by the next one, so a test can pass on somebody
// else's leftovers. Both defects disappear if the name is unique per process
// AND per allocation, and if the tree is removed by RAII even on the error
// path.
//
// NAMING CONTRACT
// ---------------
//   prism_<card>[_<tag>]_p<pid>_<counter>
//     card    the card id ("c36"), so debris in %TEMP% is attributable
//     pid     never shared by two live processes, so a leftover from a crashed
//             run can never be mistaken for a fresh fixture
//     counter monotonic in-process, so two cases in ONE process differ too
//
// LIFETIME CONTRACT
// -----------------
// RAII. The destructor removes the tree; "return 1", a failed PRISM_CHECK and
// an exception all run it. Removal retries with backoff and clears the
// read-only attribute, because 7z.exe/COM readers can hold a handle or a
// fixture can be marked read-only, and a half-deleted fixture is precisely the
// nondeterminism this header exists to kill.
//
// Nothing here is product code: it is test scaffolding and it is compiled with
// /WX- like every other Windows test target.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>

namespace prism::testtmp {

namespace fs = std::filesystem;

// True when this binary was built with the D3 negative control: the naming
// rule is deliberately broken so every allocation returns the SAME path. The
// mutant suite must go red; the clean suite asserts this stays false.
inline bool InjectionArmed() {
#if defined(PRISM_C36_MUTANT_SHARED_DIR)
  return true;
#else
  return false;
#endif
}

namespace detail {

// Clear FILE_ATTRIBUTE_READONLY on a tree. A read-only FILE makes remove_all
// stop halfway on Windows, and the leftover directory is exactly the debris
// this header exists to prevent.
inline void ClearReadOnly(const fs::path& dir) {
  std::error_code ec;
  fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
  const fs::recursive_directory_iterator end;
  for (; !ec && it != end; it.increment(ec)) {
    const DWORD attrs = GetFileAttributesW(it->path().c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
      SetFileAttributesW(it->path().c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
  }
  const DWORD attrs = GetFileAttributesW(dir.c_str());
  if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
    SetFileAttributesW(dir.c_str(), attrs & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
  }
}

// Remove a tree, retrying while handles drain. Returns true when the path is
// gone (a path that never existed counts as gone).
inline bool RemoveTree(const fs::path& dir) {
  std::error_code ec;
  for (int attempt = 0; attempt < 8; ++attempt) {
    if (!fs::exists(dir, ec)) return true;
    if (attempt > 0) ClearReadOnly(dir);  // a locked/read-only payload: retry armed
    fs::remove_all(dir, ec);
    if (!fs::exists(dir, ec)) return true;
    Sleep(static_cast<DWORD>(15 * (attempt + 1)));
  }
  return !fs::exists(dir, ec);
}

inline std::wstring CardTag(const wchar_t* card, const wchar_t* tag) {
  std::wstring name = L"prism_";
  name += (card != nullptr && *card != L'\0') ? std::wstring(card) : std::wstring(L"unknown");
  if (tag != nullptr && *tag != L'\0') {
    name += L"_";
    name += tag;
  }
  return name;
}

inline fs::path TempRoot() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD written = GetTempPathW(MAX_PATH, buffer);
  if (written != 0 && written < MAX_PATH) return fs::path(std::wstring(buffer, written));
  std::error_code ec;
  const fs::path fallback = fs::temp_directory_path(ec);
  return ec ? fs::path(L".") : fallback;
}

// Is a pid still running? OpenProcess fails for a pid that no longer exists,
// and a zombie handle reports a non-STILL_ACTIVE exit code.
inline bool ProcessAlive(unsigned long pid) {
  if (pid == 0) return false;
  HANDLE handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) return false;
  DWORD code = 0;
  const bool alive = GetExitCodeProcess(handle, &code) != 0 && code == STILL_ACTIVE;
  CloseHandle(handle);
  return alive;
}

// "prism_c36_tag_p1234_5" -> 1234. Returns 0 when the pid field is not there
// (a directory this header did not create -- never touched).
inline unsigned long PidFromName(const std::wstring& leaf) {
  const size_t at = leaf.rfind(L"_p");
  if (at == std::wstring::npos) return 0;
  size_t i = at + 2;
  unsigned long value = 0;
  bool digits = false;
  while (i < leaf.size() && leaf[i] >= L'0' && leaf[i] <= L'9') {
    value = value * 10 + static_cast<unsigned long>(leaf[i] - L'0');
    digits = true;
    ++i;
  }
  if (!digits || i >= leaf.size() || leaf[i] != L'_') return 0;
  return value;
}

// Delete this card's leftovers from processes that are gone. Best effort and
// deliberately narrow: a directory whose pid is still alive belongs to a
// concurrent run and is NEVER touched (that would recreate the very race this
// header removes), and a directory without our pid field is not ours.
inline std::size_t SweepStale(const wchar_t* card) {
  const std::wstring prefix = CardTag(card, nullptr) + L"_";
  std::size_t removed = 0;
  std::error_code ec;
  const fs::path root = TempRoot();
  for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if (!it->is_directory(entryEc)) continue;
    const std::wstring leaf = it->path().filename().wstring();
    if (leaf.rfind(prefix, 0) != 0) continue;
    const unsigned long pid = PidFromName(leaf);
    if (pid == 0 || pid == static_cast<unsigned long>(GetCurrentProcessId())) continue;
    if (ProcessAlive(pid)) continue;
    if (RemoveTree(it->path())) ++removed;
  }
  return removed;
}

}  // namespace detail

// The root every scratch directory is created under, i.e. %TEMP%.
inline fs::path ScratchRoot() { return detail::TempRoot(); }

// Best effort: remove this card's leftovers from processes that are gone.
// Exposed so the self-check can prove a live neighbour is never touched.
inline std::size_t SweepStale(const wchar_t* card) { return detail::SweepStale(card); }

// A unique scratch directory that removes itself.
class UniqueDir {
 public:
  explicit UniqueDir(const wchar_t* card, const wchar_t* tag = nullptr)
      : path_(Allocate(card, tag)) {
    std::error_code ec;
    fs::create_directories(path_, ec);
    if (ec || !fs::is_directory(path_, ec)) {
      ok_ = false;
      error_ = ec ? ec.message() : std::string("create_directories did not produce a directory");
    }
  }

  ~UniqueDir() { (void)detail::RemoveTree(path_); }

  UniqueDir(const UniqueDir&) = delete;
  UniqueDir& operator=(const UniqueDir&) = delete;
  UniqueDir(UniqueDir&&) = delete;
  UniqueDir& operator=(UniqueDir&&) = delete;

  bool ok() const noexcept { return ok_; }
  const std::string& error() const noexcept { return error_; }
  const fs::path& path() const noexcept { return path_; }
  std::wstring wstr() const { return path_.wstring(); }
  fs::path operator/(const wchar_t* leaf) const { return path_ / leaf; }

  bool Exists() const {
    std::error_code ec;
    return fs::exists(path_, ec);
  }

  // Empty the directory but keep the same (still unique) name.
  bool Reset() {
    if (!detail::RemoveTree(path_)) return false;
    std::error_code ec;
    fs::create_directories(path_, ec);
    return !ec;
  }

  // Remove now; the destructor stays a no-op afterwards.
  bool RemoveNow() { return detail::RemoveTree(path_); }

 private:
  static fs::path Allocate(const wchar_t* card, const wchar_t* tag) {
    // Hygiene on first use: debris from a crashed run of THIS card cannot
    // shadow a live fixture because the pid differs, but leaving it behind
    // would slowly fill %TEMP% on a build agent.
    static std::atomic<bool> swept{false};
    if (!swept.exchange(true)) (void)detail::SweepStale(card);
    static std::atomic<unsigned long> counter{0};
    std::wstring leaf = detail::CardTag(card, tag);
#if defined(PRISM_C36_MUTANT_SHARED_DIR)
    // D3 negative control: the naming rule is broken on purpose. Every caller
    // now shares one directory, which is exactly the pre-C36 behaviour.
    (void)counter;
#else
    leaf += L"_p";
    leaf += std::to_wstring(GetCurrentProcessId());
    leaf += L"_";
    leaf += std::to_wstring(++counter);
#endif
    return detail::TempRoot() / leaf;
  }

  fs::path path_;
  bool ok_ = true;
  std::string error_;
};

}  // namespace prism::testtmp
