
// PrismArchive -- Win32 path normalisation for long paths and reserved names
// (blueprint ch. 7.2: "\?\ extended prefix, 100% compatibility with deep
// repositories and Linux/macOS archive entries such as aux.c / con.txt / nul").
//
// Why this is a separate header: the rule is subtle and has to be unit-testable
// without touching a disk. The extended prefix '\?\ ' is not cosmetic -- it
// tells the Win32 path parser "do no normalisation at all". That has two
// consequences this header owns:
//
//   1. Relative paths and '.' / '..' segments MUST be resolved BEFORE the
//      prefix is added. '\?\C:\a\..\b' is a literal filename named '..' to the
//      filesystem, not the parent directory. GetFullPathNameW() does that
//      resolution correctly even past MAX_PATH (it is one of the few APIs that
//      does, so it is the right tool -- not GetFullPathNameA).
//   2. The DOS device-name parser is bypassed, which is exactly what makes
//      'aux.c', 'con.txt' and 'nul' legal file names (they are otherwise
//      interpreted as devices and CreateFileW fails or, worse, writes to a
//      device).
//
// Round trip: Normalize() is idempotent, and ToDisplay() undoes it:
//     Normalize(ToDisplay(Normalize(p))) == Normalize(p)
// The benchmark/test in Tests/win/test_write_backend.cpp asserts that plus a
// real >260-character file round trip (contract D2: content, not existence).
//
// Negative control (contract D3): PRISM_LONGPATH_MUTANT_STRIP_PREFIX makes
// Normalize() return the absolute path with the prefix cut off, incrementing
// InjectionCount() in the same branch. The mutant executable prints
// "LONGPATH MUTANT CAUGHT" only when the prefix assertion went red AND the
// counter proves the branch actually ran -- a comment-out that no longer
// compiles in can never fake that.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace prism::win::longpath {

// --- injection bookkeeping (tests only; always compiled, cheap) -------------
inline int& InjectionCount() {
  static int count = 0;
  return count;
}

inline bool StartsWith(std::wstring_view text, std::wstring_view prefix) {
  return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

inline bool IsExtended(std::wstring_view path) { return StartsWith(path, L"\\\\?\\"); }
inline bool IsDeviceNs(std::wstring_view path) { return StartsWith(path, L"\\\\.\\"); }
inline bool IsUnc(std::wstring_view path) {
  return StartsWith(path, L"\\\\") && !IsExtended(path) && !IsDeviceNs(path);
}

inline wchar_t UpperAscii(wchar_t c) {
  return (c >= L'a' && c <= L'z') ? static_cast<wchar_t>(c - L'a' + L'A') : c;
}

inline bool EqualsIgnoreCaseAscii(std::wstring_view a, std::wstring_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (UpperAscii(a[i]) != UpperAscii(b[i])) return false;
  }
  return true;
}

// Last component of a path. Works on both separators and tolerates a trailing
// separator ("C:\dir\" -> "dir").
inline std::wstring_view BaseName(std::wstring_view path) {
  size_t end = path.size();
  while (end > 0 && (path[end - 1] == L'\\' || path[end - 1] == L'/')) --end;
  const size_t slash = path.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
  const size_t begin = (slash == std::wstring_view::npos) ? 0 : slash + 1;
  if (end < begin) end = begin;
  return path.substr(begin, end - begin);
}

// True for the DOS device names Windows resolves BEFORE the filesystem:
// CON, PRN, AUX, NUL, COM1..COM9, LPT1..LPT9 -- extension and trailing dots are
// ignored by that parser, so "con.txt" and "aux.c" are devices too.
inline bool IsReservedBaseName(std::wstring_view base) {
  std::wstring_view stem = base;
  const size_t dot = stem.find(L'.');
  if (dot != std::wstring_view::npos) stem = stem.substr(0, dot);
  while (!stem.empty() && (stem.back() == L' ' || stem.back() == L'.')) stem.remove_suffix(1);
  if (stem.size() == 3) {
    if (EqualsIgnoreCaseAscii(stem, L"CON") || EqualsIgnoreCaseAscii(stem, L"PRN") ||
        EqualsIgnoreCaseAscii(stem, L"AUX") || EqualsIgnoreCaseAscii(stem, L"NUL")) {
      return true;
    }
  }
  if (stem.size() == 4) {
    const std::wstring_view prefix = stem.substr(0, 3);
    const wchar_t digit = stem[3];
    if (EqualsIgnoreCaseAscii(prefix, L"COM") || EqualsIgnoreCaseAscii(prefix, L"LPT")) {
      if (digit >= L'1' && digit <= L'9') return true;
    }
  }
  return false;
}

// Whether a path NEEDS the extended prefix to behave correctly. Normalize()
// adds it to every absolute path anyway (uniform, and it is what makes
// reserved names and trailing dots work), but the predicate is kept because it
// states the contract and is asserted directly by the unit tests.
inline bool NeedsExtendedPrefix(std::wstring_view path) {
  if (IsExtended(path) || IsDeviceNs(path)) return false;
  if (IsUnc(path)) return true;                       // \\server\share -> \\?\UNC\...
  if (path.size() >= MAX_PATH) return true;           // cannot be expressed otherwise
  if (IsReservedBaseName(BaseName(path))) return true;  // device-name parser must be bypassed
  // A component ending in '.' or ' ' is silently trimmed by the Win32 parser.
  if (!path.empty() && (path.back() == L'.' || path.back() == L' ')) return true;
  return false;
}

namespace detail {

inline std::wstring CurrentDirectory() {
  const DWORD needed = ::GetCurrentDirectoryW(0, nullptr);
  if (needed == 0) return {};
  std::vector<wchar_t> buffer(needed + 1, L'\0');
  const DWORD written = ::GetCurrentDirectoryW(needed + 1, buffer.data());
  if (written == 0) return {};
  return std::wstring(buffer.data(), written);
}

inline bool IsDriveAbsolute(std::wstring_view path) {
  return path.size() >= 3 && path[1] == L':' &&
         ((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) &&
         (path[2] == L'\\' || path[2] == L'/');
}

inline bool IsDriveRelative(std::wstring_view path) {
  return path.size() >= 2 && path[1] == L':' && !IsDriveAbsolute(path);
}

// Turns a slash-normalised path into an ABSOLUTE one without asking the DOS
// device parser (GetFullPathNameW("...\nul") returns "\\.\nul" -- the device).
// Only drive-relative paths ("C:foo") fall back to GetFullPathNameW.
inline std::wstring MakeAbsolute(const std::wstring& path) {
  if (IsDriveAbsolute(path) || IsUnc(path)) return path;
  if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
      (path[1] == L'\\' || path[1] == L'/')) {
    return path;  // UNC: \\server\share\...
  }
  if (IsDriveRelative(path)) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD written = ::GetFullPathNameW(path.c_str(), 32768, buffer.data(), nullptr);
    if (written > 0 && written < 32768) return std::wstring(buffer.data(), written);
    return path;
  }
  if (!path.empty() && (path[0] == L'\\' || path[0] == L'/')) {
    // Root-relative: attach the cwd's drive (or its UNC share).
    const std::wstring cwd = CurrentDirectory();
    if (cwd.size() >= 2 && cwd[1] == L':') return cwd.substr(0, 2) + path;
    if (IsUnc(cwd)) {
      const size_t slash = cwd.find(L'\\', 2);
      if (slash != std::wstring::npos) {
        const size_t second = cwd.find(L'\\', slash + 1);
        if (second != std::wstring::npos) return cwd.substr(0, second) + path;
      }
    }
    return path;
  }
  const std::wstring cwd = CurrentDirectory();
  if (cwd.empty()) return path;
  std::wstring joined = cwd;
  if (!joined.empty() && joined.back() != L'\\') joined.push_back(L'\\');
  return joined + path;
}

// '.'/'..' collapse on an absolute, slash-normalised path. UNC paths keep
// server + share as their floor; drive paths keep "C:".
inline std::wstring CollapseComponents(std::wstring_view absolute, bool unc) {
  std::vector<std::wstring> parts;
  size_t i = unc ? 2 : 0;
  while (i < absolute.size()) {
    while (i < absolute.size() && (absolute[i] == L'\\' || absolute[i] == L'/')) ++i;
    const size_t start = i;
    while (i < absolute.size() && absolute[i] != L'\\' && absolute[i] != L'/') ++i;
    if (i == start) break;
    std::wstring part(absolute.substr(start, i - start));
    if (part == L".") continue;
    if (part == L"..") {
      const size_t floor = unc ? 2u : 1u;
      if (parts.size() > floor) parts.pop_back();
      continue;
    }
    parts.push_back(std::move(part));
  }
  std::wstring out = unc ? L"\\\\?\\UNC\\" : L"\\\\?\\";
  for (size_t k = 0; k < parts.size(); ++k) {
    if (k != 0) out.push_back(L'\\');
    out += parts[k];
  }
  if (!unc && parts.size() == 1) out.push_back(L'\\');
  return out;
}

}  // namespace detail

// The single entry point. Every destination path in WriteBackend goes through
// it: the Win32 API call itself never sees a relative path or a '.' segment.
inline std::wstring Normalize(std::wstring_view path) {
  if (path.empty()) return {};
  std::wstring slashed(path);
  for (wchar_t& c : slashed) {
    if (c == L'/') c = L'\\';
  }

  // Already in a namespace that must not be re-parsed.
  if (IsExtended(slashed) || IsDeviceNs(slashed)) return slashed;

  // Resolve relative paths and '.'/'..' BEFORE the prefix (see the header
  // comment). Resolution is deterministic string work on purpose: the obvious
  // GetFullPathNameW runs the DOS device parser and turns "...\nul" into the
  // NUL *device* -- the exact opposite of what this header promises.
  std::wstring absolute = detail::MakeAbsolute(slashed);
  // A cwd that is itself extended (SetCurrentDirectoryW with '\?\...') makes
  // the joined path extended; strip the prefix, collapse in plain space, and
  // let CollapseComponents put the correct prefix back. Leaving it on would
  // make '..' a literal component (the prefix disables normalisation).
  bool unc = IsUnc(absolute);
  if (IsExtended(absolute)) {
    unc = StartsWith(absolute, L"\\\\?\\UNC\\");
    absolute = unc ? (std::wstring(L"\\\\") + absolute.substr(8)) : absolute.substr(4);
  }
  if (IsDeviceNs(absolute)) return absolute;

#ifdef PRISM_LONGPATH_MUTANT_STRIP_PREFIX
  // Negative control: the prefix is the guard. Report the injection truthfully
  // (D3) so the mutant can prove the mutation fired.
  ++InjectionCount();
  (void)unc;
  return absolute;
#else
  return detail::CollapseComponents(absolute, unc);
#endif
}

// Inverse for display / for handing a path back to APIs that reject '\?\'
// (the shell, the UI, CLI arguments). Not intended to be fed to CreateFileW.
inline std::wstring ToDisplay(std::wstring_view path) {
  if (StartsWith(path, L"\\\\?\\UNC\\")) {
    return std::wstring(L"\\\\") + std::wstring(path.substr(8));
  }
  if (StartsWith(path, L"\\\\?\\")) return std::wstring(path.substr(4));
  return std::wstring(path);
}

// True when the normalized form is longer than MAX_PATH characters (excluding
// the terminating NUL) -- the cell the round-trip test uses to prove the long
// path actually needed the prefix.
inline bool ExceedsMaxPath(std::wstring_view normalized) {
  return ToDisplay(normalized).size() >= MAX_PATH;
}

}  // namespace prism::win::longpath
