// PrismArchive -- PrismCore
// C85: the log's DAY and the log's CLOCK, unit-tested where they are pure.
//
// The Windows suite (win_c85_errors) proves the FILE appears and carries a real
// year; this one proves the two rules that decide WHAT appears, so a change in
// the date arithmetic or in the clock seam is caught on the Linux lane in
// milliseconds instead of on a Windows runner.
//
//   1. prism::log::FormatUtcDateCompact -- the "YYYYMMDD" a log file is named
//      after, checked at the epoch, at a known instant, and at a leap day (a
//      date rule that is off by one shows up exactly there);
//   2. Logger::SetClock -- a record left at the default timestamp takes the
//      clock's value (that is the 1970 fix), while an EXPLICIT timestamp still
//      wins (that is what keeps FormatRecord's tests honest).
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include "mini_test.hpp"
#include "../Src/PrismCore/Log.hpp"

namespace {

namespace plog = prism::log;

// Unique per process, so two concurrent builds of this suite never share a
// directory (the same rule Tests/test_log.cpp follows).
std::filesystem::path TempDir(const char* name) {
  static int anchor = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::string tag = std::to_string(static_cast<long long>(now)) + "_" +
                          std::to_string(reinterpret_cast<std::uintptr_t>(&anchor));
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / (std::string("prism_c85_") + name + "_" + tag);
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

// 2026-11-05T00:00:00Z: 1793836800 s since the epoch (checked by hand from
// 2026-01-01T00:00:00Z = 1767225600 + 308 days).
constexpr int64_t kNov5th2026 = 1793836800LL * 1000LL;

}  // namespace

PRISM_TEST("c85 the log file's day is the UTC day of the record") {
  PRISM_CHECK_EQ(plog::FormatUtcDateCompact(0), std::string("19700101"));
  PRISM_CHECK_EQ(plog::FormatUtcDateCompact(kNov5th2026), std::string("20261105"));
  // The last millisecond of a day stays on that day...
  PRISM_CHECK_EQ(plog::FormatUtcDateCompact(kNov5th2026 + 86399999LL), std::string("20261105"));
  // ...and the next one rolls over. A sink that got this wrong would append the
  // new day's lines to yesterday's file.
  PRISM_CHECK_EQ(plog::FormatUtcDateCompact(kNov5th2026 + 86400000LL), std::string("20261106"));
  // 2024-02-29: the leap day the civil-date algorithm has to get right.
  // 2024-01-01T00:00:00Z = 1704067200; + 59 days = 2024-02-29.
  PRISM_CHECK_EQ(plog::FormatUtcDateCompact((1704067200LL + 59LL * 86400LL) * 1000LL),
                 std::string("20240229"));
}

PRISM_TEST("c85 a record without a timestamp takes the REAL clock") {
  plog::Logger logger(plog::Level::kInfo, 8);
  // Before the clock is installed the record formats as the epoch -- the exact
  // line the user reported ("1970-01-01T00:00:00.000Z").
  PRISM_CHECK(!logger.hasClock());
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "startup", "app started"));
  PRISM_CHECK(logger.ring().Last(1).front().rfind("1970-01-01", 0) == 0);

  logger.SetClock([] { return kNov5th2026; });
  PRISM_CHECK(logger.hasClock());
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "startup", "app started"));
  const std::string stamped = logger.ring().Last(1).front();
  PRISM_CHECK_EQ(stamped.substr(0, 4), std::string("2026"));
  PRISM_CHECK_EQ(stamped.substr(0, 19), std::string("2026-11-05T00:00:00"));

  // An EXPLICIT timestamp is not overridden: the clock is a default, not a
  // policy.
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "startup", "explicit", {}, 86400000LL));
  PRISM_CHECK_EQ(logger.ring().Last(1).front().substr(0, 10), std::string("1970-01-02"));
}

PRISM_TEST("c85 the daily-sink rule is a NAME, and the name is the day") {
  // The exact suffix a Windows sink builds: prism-<day>.log. Asserted here so
  // the naming rule is pinned on every platform; the file itself is proven on
  // the Windows lane (win_c85_errors).
  const std::string day = plog::FormatUtcDateCompact(kNov5th2026);
  const std::string name = "prism-" + day + ".log";
  PRISM_CHECK_EQ(name, std::string("prism-20261105.log"));
  // The rotation suffix RollingFileSink inserts goes BEFORE the extension, so a
  // day's rotations stay attributable to that day.
  const std::filesystem::path dir = TempDir("name");
  plog::RollingFileSink sink((dir / name).string(), 1024, 2);
  PRISM_CHECK(sink.ok());
  PRISM_CHECK_EQ(sink.PathFor(0), (dir / "prism-20261105.log").string());
  PRISM_CHECK_EQ(sink.PathFor(1), (dir / "prism-20261105.1.log").string());
  sink.Close();
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  // D1: the stdout marker is a CONJUNCTION with exit 0 -- a suite that printed
  // nothing must never read as green.
  std::puts("ALL C85 LOG CHECKS PASSED");
  return 0;
}
