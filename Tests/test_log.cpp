// PrismArchive -- PrismCore test
// Log: graded/structured/redacted/rate-limited logging + the ring buffer the
// crash report attaches its last 64 lines from, + the rolling file sink.
//
// Covered here (Linux lane, seconds):
//   * level filter, exact wire format, UTC timestamps incl. leap day,
//   * CONTRACTS B7 content-level redaction: sensitive KEY, free-text
//     "password=..." spans, URL userinfo, known secrets and their fragments,
//   * one record == one line (a message with a newline cannot forge a record),
//   * throttle: capacity per window, suppression COUNT on the next allowed
//     record, clock going backwards, config rejection,
//   * ring: newest-N semantics and eviction order,
//   * rolling file sink: rotation, retention, naming, unwritable path.
//
// Negative control (CONTRACTS D3, compiled by Tests/suites/c16-logging.cmake with
// WILL_FAIL): PRISM_C16_MUTANT_NO_REDACTION removes the redaction. The redaction
// cases MUST then go red; if the macro stopped matching they would pass, turning
// the WILL_FAIL registration red -- so the injection is verified, not assumed.
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "mini_test.hpp"

#include "Log.hpp"

// NOT `namespace log`: <cmath> already declares a global ::log, and the alias
// would collide (C2386).
namespace plog = prism::log;

namespace {

// Unique per process, so two builds of this suite can never share a directory.
std::string UniqueTag() {
  static int anchor = 0;
  const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::to_string(static_cast<long long>(now)) + "_" +
         std::to_string(reinterpret_cast<std::uintptr_t>(&anchor));
}

std::filesystem::path TempDir(const std::string& name) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() / ("prism_c16_" + name + "_" + UniqueTag());
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
  std::filesystem::create_directories(dir, ec);
  return dir;
}

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::vector<std::string> lines;
  std::ifstream in(path, std::ios::binary);
  std::string line;
  while (std::getline(in, line)) lines.push_back(line);
  return lines;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

size_t CountLines(const std::string& text) {
  size_t count = 0;
  for (const char c : text) {
    if (c == '\n') ++count;
  }
  return count;
}

// Captures what a sink would have written.
struct SinkCapture {
  std::vector<std::string> lines;
  plog::Logger::Sink Handler() {
    return [this](const std::string& line) { lines.push_back(line); };
  }
};

plog::Record SampleRecord() {
  plog::Record record;
  record.timestampMs = 1700000000123;  // 2023-11-14T22:13:20.123Z
  record.level = plog::Level::kWarn;
  record.component = "openflow";
  record.message = "password rejected";
  record.fields = {{"attempt", "3"}, {"password", "hunter2"}};
  return record;
}

}  // namespace

// The level vocabulary is what every other test and the crash report prints, so
// it is pinned by name and by ordering (a renumbered enum would silently move
// the default threshold).
PRISM_TEST("log.levels_have_stable_names_and_order") {
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kTrace)), std::string("TRACE"));
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kDebug)), std::string("DEBUG"));
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kInfo)), std::string("INFO"));
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kWarn)), std::string("WARN"));
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kError)), std::string("ERROR"));
  PRISM_CHECK_EQ(std::string(plog::LevelName(plog::Level::kFatal)), std::string("FATAL"));
  PRISM_CHECK(plog::Level::kTrace < plog::Level::kDebug);
  PRISM_CHECK(plog::Level::kInfo < plog::Level::kWarn);
  PRISM_CHECK(plog::Level::kWarn < plog::Level::kFatal);
  // B6: printable, and printing twice keeps the stream usable.
  std::ostringstream os;
  os << plog::Level::kError << "/" << plog::Level::kDebug;
  PRISM_CHECK_EQ(os.str(), std::string("ERROR/DEBUG"));
}

// Deterministic UTC formatting. The leap day and the year rollover are the two
// spots where a hand-rolled civil conversion goes wrong.
PRISM_TEST("log.timestamp_is_exact_utc") {
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(0), std::string("1970-01-01T00:00:00.000Z"));
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(1700000000123),
                 std::string("2023-11-14T22:13:20.123Z"));
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(1709164800000),
                 std::string("2024-02-29T00:00:00.000Z"));  // leap day
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(1798761600000),
                 std::string("2027-01-01T00:00:00.000Z"));  // year rollover
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(999), std::string("1970-01-01T00:00:00.999Z"));
  // Pre-epoch input is clamped, not rendered as 1969: a negative timestamp is a
  // caller bug and a deterministic line beats a wrong one.
  PRISM_CHECK_EQ(plog::FormatUtcTimestamp(-1), std::string("1970-01-01T00:00:00.000Z"));
}

// The wire format is byte-stable: level padded to 5 columns, component prefix,
// fields always quoted, sensitive field value replaced before formatting.
PRISM_TEST("log.wire_format_is_byte_stable_and_redacted") {
  SinkCapture capture;
  plog::Logger logger(plog::Level::kTrace, 8);
  logger.SetSink(capture.Handler());
  PRISM_CHECK(logger.Emit(SampleRecord()));
  if (!PRISM_CHECK_EQ(capture.lines.size(), size_t(1))) return;
  const std::string expected =
      "2023-11-14T22:13:20.123Z WARN  openflow: password rejected attempt=\"3\" "
      "password=\"[redacted]\"";
  PRISM_CHECK_EQ(capture.lines[0], expected);
  // The secret never reached the sink: content-level, not "redacted somewhere".
  PRISM_CHECK(!Contains(capture.lines[0], "hunter2"));
  PRISM_CHECK(Contains(capture.lines[0], plog::kRedacted));
  // Column layout: 24-char timestamp, one space, level padded to 5 columns, one
  // space, then the component. A shorter level must not shift the rest.
  if (capture.lines[0].size() > 31) {
    PRISM_CHECK_EQ(capture.lines[0][24], ' ');
    PRISM_CHECK_EQ(capture.lines[0].compare(25, 5, "WARN "), 0);
    PRISM_CHECK_EQ(capture.lines[0].compare(31, 10, "openflow: "), 0);
  }
}

// Log injection: a message carrying a newline must not become two records.
PRISM_TEST("log.control_characters_cannot_forge_a_record") {
  plog::Logger logger(plog::Level::kInfo, 8);
  const bool emitted = logger.Emit(plog::Level::kInfo, "sweep",
                                   std::string("first\n1970-01-01T00:00:00.000Z FATAL forged"),
                                   {{"note", "quote\"slash\\tab\tend"}});
  PRISM_CHECK(emitted);
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(1))) return;
  PRISM_CHECK_EQ(CountLines(lines[0]), size_t(0));  // no raw newline inside
  PRISM_CHECK(Contains(lines[0], "\\n"));
  PRISM_CHECK(Contains(lines[0], "note=\"quote\\\"slash\\\\tab\\tend\""));
  PRISM_CHECK(Contains(lines[0], "first\\n1970-01-01T00:00:00.000Z FATAL forged"));
  PRISM_CHECK(!Contains(lines[0], "forged\n"));
}

// B7, first half: the field KEY decides, so a caller cannot leak a credential by
// forgetting to declare it as a secret.
PRISM_TEST("log.sensitive_field_keys_are_never_printed") {
  const char* const kSensitiveKeys[] = {
      "password", "PASSWORD", "passwd", "pwd", "passphrase", "secret",
      "api_key",  "API-KEY",  "token", "db_password", "credentials", "SessionKey",
      "authorization", "privateKey", "salt",
  };
  plog::Logger logger(plog::Level::kInfo, 32);
  for (const char* key : kSensitiveKeys) {
    const std::string probe = std::string("probe-value-for-") + key;
    PRISM_CHECK(logger.Emit(plog::Level::kError, "vault", "attempt",
                            {{key, probe}, {"user", "alice"}}));
  }
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(sizeof(kSensitiveKeys) / sizeof(kSensitiveKeys[0])))) {
    return;
  }
  for (size_t i = 0; i < lines.size(); ++i) {
    PRISM_CHECK(!Contains(lines[i], "probe-value-for-"));
    PRISM_CHECK(Contains(lines[i], plog::kRedacted));
    PRISM_CHECK(Contains(lines[i], "user=\"alice\""));  // non-secret survives
  }
}

// B7, second half: the free text is scanned too -- credentials show up inside
// diagnostic strings composed elsewhere, not only in structured fields.
PRISM_TEST("log.free_text_credentials_and_url_userinfo_are_redacted") {
  plog::Logger logger(plog::Level::kInfo, 32);
  PRISM_CHECK(logger.Emit(plog::Level::kError, "openflow", "auth failed password=hunter2 for a.7z"));
  PRISM_CHECK(logger.Emit(plog::Level::kError, "openflow", "retry pwd: correct-horse-battery"));
  PRISM_CHECK(
      logger.Emit(plog::Level::kInfo, "proxy", "using http://user:s3cr3t@127.0.0.1:8080/x"));
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "engine", "token='abc123' secret=\"s1\""));
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(4))) return;
  const std::string all = lines[0] + lines[1] + lines[2] + lines[3];
  PRISM_CHECK(!Contains(all, "hunter2"));
  PRISM_CHECK(!Contains(all, "correct-horse-battery"));
  PRISM_CHECK(!Contains(all, "s3cr3t"));
  PRISM_CHECK(!Contains(all, "abc123"));
  PRISM_CHECK(Contains(lines[0], "password=[redacted]"));
  PRISM_CHECK(Contains(lines[2], "user:[redacted]@127.0.0.1:8080"));  // host survives
  PRISM_CHECK(Contains(lines[2], "http://"));
}

// Declared secrets are removed everywhere, including fragments: a half-leaked
// password is still a leak. Too-short secrets are ignored so no noise redaction.
PRISM_TEST("log.known_secrets_are_redacted_full_and_fragmented") {
  plog::Logger logger(plog::Level::kInfo, 16);
  logger.redactor().AddSecret("hunter2");
  logger.redactor().AddSecret("ab");  // below the safety floor
  PRISM_CHECK_EQ(logger.redactor().secretCount(), size_t(2));
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "vault",
                          "opening with hunter2, also nter2 and hunter", {{"note", "xhunter2x"}}));
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(1))) return;
  PRISM_CHECK(!Contains(lines[0], "hunter2"));
  PRISM_CHECK(!Contains(lines[0], "nter2"));
  PRISM_CHECK(!Contains(lines[0], "hunter"));  // 6-char fragment of hunter2
  PRISM_CHECK_EQ(CountLines(lines[0]), size_t(0));

  logger.redactor().ClearSecrets();
  PRISM_CHECK_EQ(logger.redactor().secretCount(), size_t(0));
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "vault", "opening with hunter2"));
  PRISM_CHECK(Contains(logger.ring().Snapshot().back(), "hunter2"));
}

// Throttling is what stops a failing solid sweep from writing the same line 400
// times: capacity per window, then a suppression COUNT carried on the next
// allowed record.
PRISM_TEST("log.throttle_caps_repeats_and_reports_the_count") {
  plog::ThrottleConfig config;
  config.capacity = 3;
  config.windowMs = 1000;
  plog::Throttle throttle(config);

  for (int i = 0; i < 10; ++i) {
    const plog::ThrottleVerdict verdict = throttle.Allow("sweep:fail", 0);
    PRISM_CHECK_EQ(verdict.allow, i < 3);
  }
  PRISM_CHECK_EQ(throttle.totalSuppressed(), size_t(7));
  // Same key, new window: allowed again, and it carries the count it swallowed.
  const plog::ThrottleVerdict next = throttle.Allow("sweep:fail", 1000);
  PRISM_CHECK(next.allow);
  PRISM_CHECK_EQ(next.suppressed, size_t(7));
  // The bucket refilled: two more allowed, then blocked again.
  PRISM_CHECK(throttle.Allow("sweep:fail", 1000).allow);
  PRISM_CHECK(throttle.Allow("sweep:fail", 1000).allow);
  PRISM_CHECK(!throttle.Allow("sweep:fail", 1000).allow);
  // Another key has its own budget.
  PRISM_CHECK(throttle.Allow("open:fail", 1000).allow);
  PRISM_CHECK_EQ(throttle.trackedKeys(), size_t(2));
  // A clock that jumped backwards must reset, not throttle for a day.
  PRISM_CHECK(throttle.Allow("sweep:fail", 5).allow);
  throttle.Reset();
  PRISM_CHECK_EQ(throttle.trackedKeys(), size_t(0));
  PRISM_CHECK_EQ(throttle.totalSuppressed(), size_t(0));
}

PRISM_TEST("log.throttle_rejects_impossible_config") {
  for (int which = 0; which < 3; ++which) {
    plog::ThrottleConfig config;
    if (which == 0) config.capacity = 0;
    if (which == 1) config.windowMs = 0;
    if (which == 2) config.maxKeys = 0;
    bool threw = false;
    try {
      plog::Throttle throttle(config);
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    PRISM_CHECK(threw);
  }
}

// The key table is bounded, and hitting the bound must never hide a first-seen
// failure: it is allowed untracked and counted (observable, not silent).
PRISM_TEST("log.throttle_key_table_is_bounded_without_hiding_new_failures") {
  plog::ThrottleConfig config;
  config.capacity = 1;
  config.windowMs = 100;
  config.maxKeys = 2;
  plog::Throttle throttle(config);
  PRISM_CHECK(throttle.Allow("a", 0).allow);
  PRISM_CHECK(!throttle.Allow("a", 0).allow);
  PRISM_CHECK(throttle.Allow("b", 0).allow);
  PRISM_CHECK_EQ(throttle.trackedKeys(), size_t(2));
  PRISM_CHECK(throttle.Allow("c", 0).allow);  // untracked, still emitted
  PRISM_CHECK(throttle.Allow("c", 0).allow);
  PRISM_CHECK_EQ(throttle.trackedKeys(), size_t(2));
  PRISM_CHECK_EQ(throttle.untrackedAllows(), size_t(2));
}

// Ring semantics: newest N, oldest evicted, and Last(n) is a suffix of Snapshot.
PRISM_TEST("log.ring_keeps_the_newest_lines_in_order") {
  plog::RingBuffer ring(4);
  PRISM_CHECK_EQ(ring.capacity(), size_t(4));
  for (const char* line : {"a", "b", "c", "d", "e", "f"}) ring.Push(line);
  PRISM_CHECK_EQ(ring.size(), size_t(4));
  const std::vector<std::string> snapshot = ring.Snapshot();
  if (!PRISM_CHECK_EQ(snapshot.size(), size_t(4))) return;
  PRISM_CHECK_EQ(snapshot[0], std::string("c"));  // a, b evicted
  PRISM_CHECK_EQ(snapshot[3], std::string("f"));
  const std::vector<std::string> last2 = ring.Last(2);
  if (!PRISM_CHECK_EQ(last2.size(), size_t(2))) return;
  PRISM_CHECK_EQ(last2[0], std::string("e"));
  PRISM_CHECK_EQ(last2[1], std::string("f"));
  PRISM_CHECK_EQ(ring.Last(0).size(), size_t(0));
  PRISM_CHECK_EQ(ring.Last(99).size(), size_t(4));  // never past what it holds
  PRISM_CHECK(ring.Last(99) == snapshot);  // Last(n) is exactly the newest n
  ring.Clear();
  PRISM_CHECK(ring.empty());
  PRISM_CHECK_EQ(ring.Last(64).size(), size_t(0));

  bool threw = false;
  try {
    plog::RingBuffer zero(0);
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  PRISM_CHECK(threw);  // D17: a capacity of 0 is not a mode
}

// The pipeline order is the contract: filter -> redact -> throttle -> format.
// A suppressed record must not consume ring space, and a filtered record must
// not consume a throttle token.
PRISM_TEST("log.pipeline_filters_throttles_and_redacts_before_the_sink") {
  SinkCapture capture;
  plog::ThrottleConfig config;
  config.capacity = 2;
  config.windowMs = 1000;
  plog::Logger logger(plog::Level::kInfo, 16, config);
  logger.SetSink(capture.Handler());

  PRISM_CHECK(!logger.Emit(plog::Level::kDebug, "sweep", "noise"));
  PRISM_CHECK_EQ(logger.filtered(), size_t(1));
  PRISM_CHECK_EQ(logger.ring().size(), size_t(0));

  for (int i = 0; i < 5; ++i) {
    logger.Emit(plog::Level::kError, "sweep", "block read failed",
                {{"password", "hunter2"}}, 0, "sweep:block");
  }
  PRISM_CHECK_EQ(logger.emitted(), size_t(2));
  PRISM_CHECK_EQ(logger.suppressed(), size_t(3));
  PRISM_CHECK_EQ(logger.ring().size(), size_t(2));
  PRISM_CHECK_EQ(capture.lines.size(), size_t(2));
  PRISM_CHECK(logger.ring().Snapshot() == capture.lines);  // ring and sink agree

  // The next window's first record carries the count -- and is still redacted.
  PRISM_CHECK(logger.Emit(plog::Level::kError, "sweep", "block read failed",
                          {{"password", "hunter2"}}, 1000, "sweep:block"));
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(3))) return;
  PRISM_CHECK(Contains(lines[2], "suppressed=\"3\""));
  PRISM_CHECK(Contains(lines[2], "password=\"[redacted]\""));
  PRISM_CHECK(!Contains(lines[2], "hunter2"));

  logger.SetMinLevel(plog::Level::kError);
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "sweep", "now below the floor") == false);
  PRISM_CHECK_EQ(logger.filtered(), size_t(2));

  logger.ClearSink();
  PRISM_CHECK(logger.Emit(plog::Level::kError, "sweep", "ring only", {}, 1000));
  PRISM_CHECK_EQ(capture.lines.size(), size_t(3));  // sink really detached
  PRISM_CHECK_EQ(logger.ring().size(), size_t(4));
}

// Rolling file: rotation happens, retention is bounded and ordered, no written
// line is lost while it is still inside the retention window.
PRISM_TEST("log.rolling_file_rotates_and_retains_a_suffix") {
  const std::filesystem::path dir = TempDir("roll");
  const std::string base = (dir / "prism.log").string();
  plog::RollingFileSink sink(base, 200, 3);
  if (!PRISM_CHECK(sink.ok())) {
    std::cout << "    sink error: " << sink.error() << std::endl;
    return;
  }
  PRISM_CHECK_EQ(sink.PathFor(0), base);
  PRISM_CHECK(sink.PathFor(1) != base);
  PRISM_CHECK(sink.PathFor(1).find("prism.1.log") != std::string::npos);
  PRISM_CHECK(sink.PathFor(2).find("prism.2.log") != std::string::npos);

  std::vector<std::string> written;
  for (int i = 0; i < 40; ++i) {
    const std::string line = "record " + std::to_string(100 + i) + " payload padding";
    PRISM_CHECK(sink.Write(line));
    written.push_back(line);
  }
  sink.Close();
  PRISM_CHECK(sink.rotations() > 0);
  PRISM_CHECK(sink.bytesWritten() > 0);

  // At most maxFiles files exist, and nothing older than that.
  std::error_code ec;
  PRISM_CHECK(std::filesystem::exists(sink.PathFor(0), ec));
  PRISM_CHECK(std::filesystem::exists(sink.PathFor(1), ec));
  PRISM_CHECK(!std::filesystem::exists(sink.PathFor(3), ec));

  // Oldest -> newest across the retained files.
  std::vector<std::string> retained;
  for (const std::string& line : ReadLines(sink.PathFor(2))) retained.push_back(line);
  for (const std::string& line : ReadLines(sink.PathFor(1))) retained.push_back(line);
  for (const std::string& line : ReadLines(sink.PathFor(0))) retained.push_back(line);
  // Three files of <= 200 bytes cannot hold all 40 records: eviction happened,
  // and what survived is still a real window (not a single line).
  PRISM_CHECK(retained.size() < written.size());
  if (!PRISM_CHECK(retained.size() >= 12)) return;

  // The retained lines are the TAIL of what was written: rotation must not
  // reorder, duplicate or drop a line that is still inside the window.
  const size_t offset = written.size() - retained.size();
  for (size_t i = 0; i < retained.size(); ++i) {
    if (retained[i] != written[offset + i]) {
      PRISM_CHECK_EQ(retained[i], written[offset + i]);
      break;
    }
  }
  PRISM_CHECK_EQ(retained.back(), written.back());
  // No file exceeds the cap except by the single record that crossed it.
  for (size_t index = 0; index < 3; ++index) {
    const auto size = std::filesystem::file_size(sink.PathFor(index), ec);
    if (ec) continue;
    PRISM_CHECK(size <= 200 + 64);
  }
  std::filesystem::remove_all(dir, ec);
}

// A logging sink must never throw and never take the process down: an unwritable
// path reports itself.
PRISM_TEST("log.rolling_sink_reports_an_unwritable_path") {
  const std::filesystem::path dir = TempDir("blocked");
  const std::filesystem::path blocker = dir / "blocker";
  {
    std::ofstream out(blocker, std::ios::binary);
    out << "not a directory";
  }
  plog::RollingFileSink sink((blocker / "prism.log").string(), 100, 2);
  PRISM_CHECK(!sink.ok());
  PRISM_CHECK(!sink.error().empty());
  PRISM_CHECK(!sink.Write("hello"));
  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

PRISM_TEST("log.rolling_sink_rejects_impossible_config") {
  for (int which = 0; which < 3; ++which) {
    bool threw = false;
    try {
      if (which == 0) {
        plog::RollingFileSink sink("", 100, 2);
        (void)sink;
      }
      if (which == 1) {
        plog::RollingFileSink sink("x.log", 0, 2);
        (void)sink;
      }
      if (which == 2) {
        plog::RollingFileSink sink("x.log", 100, 0);
        (void)sink;
      }
    } catch (const std::invalid_argument&) {
      threw = true;
    }
    PRISM_CHECK(threw);
  }
}

// UTF-8 payloads pass through byte-for-byte: the log is UTF-8 and the crash
// report is read by humans, so re-encoding here would corrupt Chinese paths.
PRISM_TEST("log.utf8_payloads_survive_escaping") {
  plog::Logger logger(plog::Level::kInfo, 4);
  const std::string chinese = "\xE6\x97\xA5\xE5\xBF\x97" ;  // "日志"
  PRISM_CHECK(logger.Emit(plog::Level::kInfo, "open", "a.7z \xE8\xA7\xA3\xE5\x8E\x8B",
                          {{"file", chinese}}));
  const std::vector<std::string> lines = logger.ring().Snapshot();
  if (!PRISM_CHECK_EQ(lines.size(), size_t(1))) return;
  PRISM_CHECK(Contains(lines[0], chinese));
  PRISM_CHECK(Contains(lines[0], "\xE8\xA7\xA3\xE5\x8E\x8B"));
  PRISM_CHECK_EQ(CountLines(lines[0]), size_t(0));
}

int main(int argc, char** argv) {
#ifdef PRISM_C16_LOG_MUTANT
  std::cout << "C16 MUTANT ACTIVE: NO_REDACTION (redaction compiled out; the "
               "redaction cases below MUST fail)"
            << std::endl;
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL LOG CHECKS PASSED" << std::endl;
  return 0;
}
