// PrismArchive -- PrismWin
// Mark-of-the-Web inheritance, Win32 side (blueprint 7.1 #1).
//
// The policy -- whether to stamp at all, and what a failure means -- lives in
// PrismCore/SecurityGuard.hpp (PlanMotwInheritance / MotwStampFailureAction).
// This header only does the system calls, and it deliberately does more than the
// old MoTWPropagator it replaces:
//
//   * long paths: every path goes through the \\?\ prefix (CONTRACTS B4), so an
//     archive extracted below a >260-character directory is still stamped;
//   * read-back verification: after writing the target ADS the stream is opened
//     again and compared byte-for-byte with the source. "WriteFile returned true"
//     is not evidence that SmartScreen will see the mark;
//   * absence vs. unreadable: a missing stream is Absent (nothing to inherit),
//     while a sharing lock / I/O error is Unreadable -- the wiring must warn, not
//     silently pretend the archive was clean.
//
// Contract: the copied blob is VERBATIM. ZoneId=3, HostUrl and ReferrerUrl keep
// their original bytes and order; the stream is never re-serialised.
#pragma once

#include <windows.h>

#include <cstdint>
#include <ostream>
#include <string>

#include "../PrismCore/SecurityGuard.hpp"

namespace prism::win::motw {

// The Zone.Identifier stream is a few hundred bytes. Anything larger is not a
// download mark we understand; refuse to clone it into every extracted file.
inline constexpr uint64_t kMaxZoneBlobBytes = 64ull * 1024;

enum class SourceState : uint8_t { Absent, Present, Empty, Unreadable };

inline const char* ToString(SourceState state) {
  switch (state) {
    case SourceState::Absent: return "Absent";
    case SourceState::Present: return "Present";
    case SourceState::Empty: return "Empty";
    case SourceState::Unreadable: return "Unreadable";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, SourceState state) {
  return os << ToString(state);
}

enum class StampResult : uint8_t {
  Stamped,           // written AND read back byte-identical
  NoSourceMark,      // archive had no Zone.Identifier: nothing to inherit
  EmptySourceMark,   // stream exists but is empty: not a usable mark
  SourceUnreadable,  // archive stream could not be read
  WriteFailed,       // CreateFileW/WriteFile on the target ADS failed
  VerifyFailed       // write returned success but the read-back differs
};

inline const char* ToString(StampResult result) {
  switch (result) {
    case StampResult::Stamped: return "Stamped";
    case StampResult::NoSourceMark: return "NoSourceMark";
    case StampResult::EmptySourceMark: return "EmptySourceMark";
    case StampResult::SourceUnreadable: return "SourceUnreadable";
    case StampResult::WriteFailed: return "WriteFailed";
    case StampResult::VerifyFailed: return "VerifyFailed";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, StampResult result) {
  return os << ToString(result);
}

struct StampReport {
  StampResult result = StampResult::NoSourceMark;
  std::string detail;  // e.g. "CreateFileW(target ADS) failed, Win32 error 5"
};

namespace detail {

inline std::string Win32Detail(const char* what, DWORD error) {
  return std::string(what) + " failed, Win32 error " + std::to_string(static_cast<unsigned long>(error));
}

// CONTRACTS B4: Win32 needs backslashes and the \\?\ prefix (which disables path
// normalisation, so names that Windows would otherwise "fix" stay exact).
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
  if (n == 0 || n >= 32768) return p;  // fall back: short paths still work
  const std::wstring absolute(buffer, n);
  if (absolute.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + absolute.substr(2);
  return L"\\\\?\\" + absolute;
}

inline bool ReadStream(const std::wstring& streamPath, std::string* out, DWORD* error) {
  HANDLE file = CreateFileW(streamPath.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
      static_cast<uint64_t>(size.QuadPart) > kMaxZoneBlobBytes) {
    if (error) *error = ERROR_FILE_TOO_LARGE;
    CloseHandle(file);
    return false;
  }
  out->assign(static_cast<size_t>(size.QuadPart), '\0');
  size_t total = 0;
  while (total < out->size()) {
    DWORD got = 0;
    const DWORD want = static_cast<DWORD>(out->size() - total);
    if (!ReadFile(file, out->data() + total, want, &got, nullptr) || got == 0) {
      if (error) *error = GetLastError();
      CloseHandle(file);
      return false;
    }
    total += got;
  }
  CloseHandle(file);
  out->resize(total);
  return true;
}

}  // namespace detail

// Reads "<archive>:Zone.Identifier" once, then stamps every written file. Create
// one SourceMark per extraction and reuse it: re-opening the source stream per
// extracted file is 80k extra opens on a node_modules-sized archive.
class SourceMark {
 public:
  explicit SourceMark(std::wstring archivePath) : archivePath_(std::move(archivePath)) { Load(); }

  SourceState state() const { return state_; }
  bool hasMoTW() const { return state_ == SourceState::Present; }
  const std::string& blob() const { return blob_; }
  const MotwFields& fields() const { return fields_; }
  const std::string& detail() const { return detail_; }
  const std::wstring& archivePath() const { return archivePath_; }

  // Core policy input (SecurityGuard.hpp).
  MotwSource policySource() const {
    switch (state_) {
      case SourceState::Present: return MotwSource::Present;
      case SourceState::Absent: return MotwSource::Absent;
      case SourceState::Empty:
      case SourceState::Unreadable: return MotwSource::Unreadable;
    }
    return MotwSource::Unreadable;
  }

  // Legacy-shaped convenience: true only when the ADS was written AND read back.
  bool Stamp(const std::wstring& writtenFilePath) const {
    return StampDetailed(writtenFilePath).result == StampResult::Stamped;
  }

  StampReport StampDetailed(const std::wstring& writtenFilePath) const {
    switch (state_) {
      case SourceState::Absent:
        return {StampResult::NoSourceMark, "source archive has no Zone.Identifier"};
      case SourceState::Empty:
        return {StampResult::EmptySourceMark, "source Zone.Identifier is empty"};
      case SourceState::Unreadable:
        return {StampResult::SourceUnreadable, detail_};
      case SourceState::Present:
        break;
    }

    const std::wstring stream =
        detail::ToExtendedPath(writtenFilePath) + L":Zone.Identifier";
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      return {StampResult::WriteFailed, detail::Win32Detail("CreateFileW(target ADS)", GetLastError())};
    }

    size_t total = 0;
    while (total < blob_.size()) {
      DWORD written = 0;
      const DWORD chunk = static_cast<DWORD>(blob_.size() - total);
      if (!WriteFile(file, blob_.data() + total, chunk, &written, nullptr) || written == 0) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        return {StampResult::WriteFailed, detail::Win32Detail("WriteFile(target ADS)", error)};
      }
      total += written;
    }
    FlushFileBuffers(file);
    CloseHandle(file);

    std::string readBack;
    DWORD error = 0;
    if (!detail::ReadStream(stream, &readBack, &error)) {
      return {StampResult::VerifyFailed, detail::Win32Detail("read-back of target ADS", error)};
    }
    if (readBack != blob_) {
      return {StampResult::VerifyFailed,
              "read-back differs from source (" + std::to_string(readBack.size()) + " vs " +
                  std::to_string(blob_.size()) + " bytes)"};
    }
    return {StampResult::Stamped, {}};
  }

 private:
  void Load() {
    const std::wstring stream = detail::ToExtendedPath(archivePath_) + L":Zone.Identifier";
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      const DWORD error = GetLastError();
      if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        state_ = SourceState::Absent;
      } else {
        state_ = SourceState::Unreadable;
        detail_ = detail::Win32Detail("CreateFileW(source ADS)", error);
      }
      return;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        static_cast<uint64_t>(size.QuadPart) > kMaxZoneBlobBytes) {
      state_ = SourceState::Unreadable;
      detail_ = "source Zone.Identifier is missing a usable size (or exceeds " +
                std::to_string(kMaxZoneBlobBytes) + " bytes)";
      CloseHandle(file);
      return;
    }
    if (size.QuadPart == 0) {
      state_ = SourceState::Empty;
      detail_ = "source Zone.Identifier is empty";
      CloseHandle(file);
      return;
    }

    std::string blob(static_cast<size_t>(size.QuadPart), '\0');
    size_t total = 0;
    while (total < blob.size()) {
      DWORD got = 0;
      const DWORD want = static_cast<DWORD>(blob.size() - total);
      if (!ReadFile(file, blob.data() + total, want, &got, nullptr) || got == 0) {
        state_ = SourceState::Unreadable;
        detail_ = detail::Win32Detail("ReadFile(source ADS)", GetLastError());
        CloseHandle(file);
        return;
      }
      total += got;
    }
    CloseHandle(file);

    blob.resize(total);
    fields_ = ParseMotwFields(blob);
    if (!fields_.present) {
      // Garbage that is not a Zone.Identifier must not be cloned into every
      // extracted file, and it must not be reported as "no mark" either.
      state_ = SourceState::Unreadable;
      detail_ = "source Zone.Identifier does not parse as a download mark";
      return;
    }
    blob_ = std::move(blob);
    state_ = SourceState::Present;
  }

  std::wstring archivePath_;
  std::string blob_;
  MotwFields fields_;
  SourceState state_ = SourceState::Absent;
  std::string detail_;
};

// One-shot API for the assembly layer (card C5's requested signature). For a
// batch of extracted files construct SourceMark once and call Stamp per file.
inline bool StampFile(const std::wstring& archive, const std::wstring& written) {
  return SourceMark(archive).Stamp(written);
}

}  // namespace prism::win::motw
