// PrismArchive -- PrismWin
// LogFile: WHERE a log line lands on Windows (card C85).
//
// THE MEASURED DEFECT. After a full session %LOCALAPPDATA%\PrismArchive held no
// .log file at all, and every stderr line started with "1970-01-01T00:00:00.000Z":
// Main.cpp installed a sink that only wrote to stderr, and nothing ever supplied
// a timestamp, so the record default (0 ms) formatted as the epoch. A user
// reporting "it failed" had nothing to attach, and the developer had nothing to
// read.
//
// WHAT THIS FILE IS. The Windows half of the sink, kept out of PrismCore for the
// same reason SettingsFile is: %LOCALAPPDATA%, GetTempPathW and a WIDE
// filesystem path are OS facts. The DAILY name and the size policy are
// PrismCore's (prism::log::FormatUtcDateCompact + RollingFileSink), so the
// naming and the rotation have exactly one implementation.
//
//   <LOCALAPPDATA>\PrismArchive\logs\prism-YYYYMMDD.log   (UTC day)
//   ... rotated by size into prism-YYYYMMDD.1.log, .2.log ...
//   ... and the directory is pruned to Policy::maxFiles files, oldest first.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C85_MUTANT_NO_FILE_LOG makes
// Write() a no-op -- the shipped defect, one layer down. The suite's "the log
// file exists after one launch" assertion MUST go red and print
// "C85 MUTATION CAUGHT: no_file_log"; ActiveMutation() is the runtime proof the
// macro compiled in.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "../PrismCore/Log.hpp"
#include "SettingsFile.hpp"  // LocalAppDataDirectory(): the ONE %LOCALAPPDATA% reader

namespace prism::win::logfile {

// ---------------------------------------------------------------- mutation
#if defined(PRISM_C85_MUTANT_NO_FILE_LOG)
// MUTANT ACTIVE (C85 arm no_file_log): the file sink writes nothing, which is
// the state the card measured -- stderr only, nothing on disk.
inline constexpr bool kFileLogEnabled = false;
#else
inline constexpr bool kFileLogEnabled = true;
#endif

inline std::string_view ActiveMutation() {
  return kFileLogEnabled ? std::string_view() : std::string_view("no_file_log");
}

// The WALL CLOCK the logger is handed at startup. std::chrono::system_clock is
// the only source here on purpose: a monotonic clock would keep ticking at the
// right RATE and describe the wrong YEAR, which is precisely the 1970 bug.
inline int64_t SystemNowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// How much history a user's disk may hold. 4 MiB x 5 files is a bound a support
// session cannot exceed, which is the promise CONTRACTS D17 asks of a capacity.
struct Policy {
  size_t maxBytes = 4u * 1024u * 1024u;
  size_t maxFiles = 5;
  // Rotations inside one day: a crash loop can produce megabytes in minutes, and
  // RollingFileSink's per-day slot count is what keeps the newest evidence.
  size_t rotationsPerDay = 2;
};

inline std::wstring LogDirectory() {
  const std::wstring root = prism::win::settingsfile::LocalAppDataDirectory();
  if (!root.empty()) return root + L"\\PrismArchive\\logs";
  wchar_t temp[MAX_PATH]{};
  const DWORD length = ::GetTempPathW(MAX_PATH, temp);
  if (length > 0 && length < MAX_PATH) return std::wstring(temp) + L"PrismArchive\\logs";
  return L"logs";
}

// UTF-8 bytes of a path (the narrow form is what messages and name checks use).
inline std::string NarrowPath(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
}

// prism-YYYYMMDD.log for the UTC day 'epochMillis' falls in. UTC, not local time,
// because the RECORD timestamps are UTC (FormatUtcTimestamp): a file whose name
// and whose lines disagree about the day is confusing exactly when it matters.
inline std::filesystem::path DailyPath(const std::wstring& directory, int64_t epochMillis) {
  const std::string day = prism::log::FormatUtcDateCompact(epochMillis);
  std::wstring name = L"prism-";
  for (const char c : day) name.push_back(static_cast<wchar_t>(c));
  name += L".log";
  std::filesystem::path path(directory);
  path /= name;
  return path;
}

// Mid-session day change: the sink reopens on the new day instead of appending a
// new day's lines to yesterday's file. Pruning happens on every open, so a
// directory that grew while the process was down is bounded too.
class DailyFileSink {
 public:
  DailyFileSink(std::wstring directory, Policy policy, prism::log::Logger::Clock clock)
      : directory_(std::move(directory)),
        policy_(policy),
        clock_(clock ? std::move(clock) : prism::log::Logger::Clock(SystemNowMillis)) {}

  DailyFileSink(const DailyFileSink&) = delete;
  DailyFileSink& operator=(const DailyFileSink&) = delete;

  // One formatted line -> today's file. False means "not written", never
  // "silently dropped": error() says why.
  bool Write(const std::string& line) {
    if (!kFileLogEnabled) {
      ++suppressedByMutant_;
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);  // Emit() may be reached from a worker
    const int64_t now = clock_();
    const std::string day = prism::log::FormatUtcDateCompact(now);
    if (!sink_ || day != day_) {
      if (!Open(now, day)) return false;
    }
    return sink_->Write(line);
  }

  const std::filesystem::path& currentPath() const { return current_; }
  const std::string& error() const { return error_; }
  bool ok() const { return sink_ != nullptr && sink_->ok(); }
  int filesKept() const { return filesKept_; }
  size_t suppressedByMutant() const { return suppressedByMutant_; }

 private:
  bool Open(int64_t now, const std::string& day) {
    day_ = day;
    sink_.reset();
    current_ = DailyPath(directory_, now);
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    sink_ = std::make_unique<prism::log::RollingFileSink>(
        current_, policy_.maxBytes, policy_.rotationsPerDay);
    if (!sink_->ok()) {
      error_ = sink_->error();
      return false;
    }
    error_.clear();
    Prune();
    return true;
  }

  // Keep the newest Policy::maxFiles files in the directory. Ordered by write
  // time (a name sort would put "prism-20260101.1.log" before
  // "prism-20260101.log"), oldest deleted first, the file just opened never.
  void Prune() {
    std::error_code ec;
    std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory_, ec)) {
      if (ec) break;
      if (!entry.is_regular_file(ec) || ec) continue;
      const std::string name = NarrowPath(entry.path().filename());
      if (name.rfind("prism-", 0) != 0) continue;
      if (name.size() < 4 || name.compare(name.size() - 4, 4, ".log") != 0) continue;
      files.emplace_back(entry.last_write_time(ec), entry.path());
      ec.clear();
    }
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    filesKept_ = static_cast<int>(files.size());
    if (policy_.maxFiles == 0) return;
    while (files.size() > policy_.maxFiles) {
      if (files.front().second != current_) {
        std::filesystem::remove(files.front().second, ec);
        ec.clear();
      }
      files.erase(files.begin());
    }
    filesKept_ = static_cast<int>(files.size());
  }

  std::wstring directory_;
  Policy policy_;
  prism::log::Logger::Clock clock_;
  mutable std::mutex mutex_;
  std::unique_ptr<prism::log::RollingFileSink> sink_;
  std::filesystem::path current_;
  std::string day_;
  std::string error_;
  int filesKept_ = 0;
  size_t suppressedByMutant_ = 0;
};

}  // namespace prism::win::logfile
