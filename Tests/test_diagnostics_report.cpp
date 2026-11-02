// PrismArchive -- PrismCore unit suite, card C57.
//
// The diagnostics report is the artifact a user pastes into a support thread.
// Two properties decide whether that is safe and whether it is useful:
//   1. EVERY documented field is there (a report that silently drops the 7z.dll
//      path explains nothing about the bug it was built for);
//   2. NOTHING identifying is there -- no user name, no full path, no vault
//      content, no password. This file asserts both on the OUTPUT TEXT, line by
//      line, because that text is exactly what leaves the machine.
//
// The negative control (CONTRACTS D3/D26/D27) is a second build of this same
// suite with PRISM_C57_MUTANT_NO_REDACTION defined: the redactor then hands the
// full path through, so the "no full path in any line" assertions MUST go red.
// The verdict is armed BEFORE the first case with SetMutationVerdict(prefix,
// predicate): the CAUGHT marker is printed at the FIRST failure, and only when
// RedactionMutantFired() proves the injection actually ran (D3: red is not
// CAUGHT). The mutant's ctest entry requires that marker and nothing else --
// no WILL_FAIL is stacked on it (D26). A stale injection keeps this suite green,
// which turns the mutant entry red -- the guard proves itself. Arm names avoid
// clean/unarmed/control (D28).
//
// CONTRACTS D31: this TU is also compiled with g++ -Wall -Wextra -Wpedantic
// -Werror on the Linux lane, so every size_t comparison is explicit and no
// helper is left unused.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "../Src/PrismCore/DiagnosticsReport.hpp"
#include "mini_test.hpp"

namespace diag = prism::diagnostics;

namespace {

// The user this report belongs to. It is a NEEDLE, never a value -- the point of
// the test is that it cannot appear anywhere in the output.
constexpr char kUserName[] = "alice";

std::vector<std::string> SplitLines(const std::string& report) {
  std::vector<std::string> lines;
  std::string current;
  for (const char ch : report) {
    if (ch == '\n') {
      lines.push_back(current);
      current.clear();
      continue;
    }
    current.push_back(ch);
  }
  if (!current.empty()) lines.push_back(current);
  return lines;
}

bool HasLine(const std::vector<std::string>& lines, const std::string& wanted) {
  for (const std::string& line : lines) {
    if (line == wanted) return true;
  }
  return false;
}

constexpr const char kMissing[] = "<missing>";

std::string ValueOf(const std::vector<std::string>& lines, std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  for (const std::string& line : lines) {
    if (line.compare(0, prefix.size(), prefix) == 0) return line.substr(prefix.size());
  }
  return std::string(kMissing);
}

// A path that must never survive: profile folder, per-user AppData, a document
// name that carries the user's name.
constexpr char kEnginePath[] =
    "C:\\Users\\alice\\AppData\\Local\\Programs\\PrismArchive\\7z.dll";
constexpr char kRecentOne[] = "C:\\Users\\alice\\Documents\\divorce-2019\\tax.7z";
constexpr char kRecentTwo[] = "D:\\backup\\alice-private\\notes.zip";
constexpr char kRecentThree[] = "\\\\server\\share\\alice\\budget.rar";

diag::Snapshot FullSnapshot() {
  diag::Snapshot snapshot;
  snapshot.version = "0.1.0.0";
  snapshot.buildType = "ASan";
  snapshot.osVersion = "10.0.22631";
  snapshot.cpuArch = "x64";
  snapshot.enginePath = kEnginePath;
  snapshot.engineVersion = "25.01";
  snapshot.engineFormatCount = 42;
  snapshot.installMode = diag::InstallMode::kInstalled;
  snapshot.progIdRegistered = true;
  snapshot.menuHandlerRegistered = false;
  snapshot.language = "zh-CN";
  snapshot.theme = "night";
  snapshot.recentFiles = {kRecentOne, kRecentTwo, kRecentThree};
  snapshot.userName = kUserName;
  snapshot.generatedAtMs = 1762042000000;  // 2025-11-02T02:46:40Z
  return snapshot;
}

std::string LogLine(const char* level, const char* component, const char* message) {
  // C16's exact wire form: "<ts> <LEVEL padded to 5> <component>: <message>".
  std::string name(level);
  while (name.size() < 5) name.push_back(' ');
  return std::string("2026-11-02T10:00:00.000Z ") + name + " " + component + ": " + message;
}

// The invariant that makes the whole design checkable: any drive path that
// survives must be "<letter>:" + separator + the ellipsis. Anything else is a
// full path (or a fragment of one) that escaped.
bool NoFullPathInvariant(const std::vector<std::string>& lines, std::string* offender) {
  for (const std::string& line : lines) {
    for (std::size_t i = 0; i + 2 < line.size(); ++i) {
      const char a = line[i];
      const bool letter = (a >= 'A' && a <= 'Z') || (a >= 'a' && a <= 'z');
      if (!letter || line[i + 1] != ':') continue;
      const char sep = line[i + 2];
      if (sep != '\\' && sep != '/') continue;
      const std::string rest = line.substr(i + 3);
      if (rest.compare(0, sizeof(diag::kEllipsis) - 1, diag::kEllipsis) != 0) {
        *offender = line;
        return false;
      }
    }
  }
  return true;
}

// One assertion shape used by several cases: nothing identifying survives, and
// the offender is printed so a red run says WHICH line leaked.
void ExpectNothingIdentifying(const std::vector<std::string>& lines) {
  std::string offender;
  const bool clean = NoFullPathInvariant(lines, &offender);
  if (!clean) std::printf("    [leaked line] %s\n", offender.c_str());
  PRISM_CHECK(clean);
  const std::string whole = [&lines] {
    std::string joined;
    for (const std::string& line : lines) {
      joined += line;
      joined.push_back('\n');
    }
    return joined;
  }();
  PRISM_CHECK(whole.find(kUserName) == std::string::npos);
  PRISM_CHECK(whole.find("Users") == std::string::npos);
  PRISM_CHECK(whole.find("divorce") == std::string::npos);
  PRISM_CHECK(whole.find("private") == std::string::npos);
}

}  // namespace

// ===========================================================================
// 1. The shape: one field, one line, always the same set.
// ===========================================================================
PRISM_TEST("report: every documented field is present, once, in one line") {
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(FullSnapshot()));
  PRISM_CHECK_EQ(lines.size(), static_cast<std::size_t>(28));  // 26 fields + 2 markers
  PRISM_CHECK_EQ(lines.front(), std::string("# PrismArchive diagnostics report"));
  PRISM_CHECK_EQ(lines.back(), std::string("# end of report"));

  const char* const keys[] = {
      "generated",  "version",      "build",         "os",           "arch",
      "engine.path", "engine.version", "engine.formats", "install.mode", "registry.progid",
      "registry.menu", "ui.language", "ui.theme",     "recent.count", "recent.1",
      "recent.2",   "recent.3",     "log.window",    "log.sampled",  "log.trace",
      "log.debug",  "log.info",     "log.warn",      "log.error",    "log.fatal",
      "log.other"};
  for (const char* key : keys) {
    const std::string value = ValueOf(lines, key);
    if (value == kMissing) std::printf("    [missing field] %s\n", key);
    PRISM_CHECK(value != kMissing);
  }
  // The values, not just their presence (CONTRACTS D2).
  PRISM_CHECK_EQ(ValueOf(lines, "version"), std::string("0.1.0.0"));
  PRISM_CHECK_EQ(ValueOf(lines, "build"), std::string("ASan"));
  PRISM_CHECK_EQ(ValueOf(lines, "os"), std::string("10.0.22631"));
  PRISM_CHECK_EQ(ValueOf(lines, "arch"), std::string("x64"));
  PRISM_CHECK_EQ(ValueOf(lines, "engine.version"), std::string("25.01"));
  PRISM_CHECK_EQ(ValueOf(lines, "engine.formats"), std::string("42"));
  PRISM_CHECK_EQ(ValueOf(lines, "install.mode"), std::string("installed"));
  PRISM_CHECK_EQ(ValueOf(lines, "registry.progid"), std::string("present"));
  PRISM_CHECK_EQ(ValueOf(lines, "registry.menu"), std::string("absent"));
  PRISM_CHECK_EQ(ValueOf(lines, "ui.language"), std::string("zh-CN"));
  PRISM_CHECK_EQ(ValueOf(lines, "ui.theme"), std::string("night"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.count"), std::string("3"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.window"), std::string("20"));
  // The timestamp is the snapshot's, not "now": the report is reproducible.
  PRISM_CHECK(ValueOf(lines, "generated").find("2025-11-02") == 0);
}

// ===========================================================================
// 2. Redaction -- the property the report exists to be safe under.
// ===========================================================================
PRISM_TEST("report: the 7z.dll path collapses to drive + ellipsis + name") {
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(FullSnapshot()));
  const std::string want = std::string("C:\\") + diag::kEllipsis + "\\7z.dll";
  PRISM_CHECK_EQ(ValueOf(lines, "engine.path"), want);  // the NAME still names the engine
  ExpectNothingIdentifying(lines);
}

PRISM_TEST("report: recent files are NAME ONLY, and the name is scrubbed too") {
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(FullSnapshot()));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.1"), std::string("tax.7z"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.2"), std::string("notes.zip"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.3"), std::string("budget.rar"));

  diag::Snapshot named = FullSnapshot();
  named.recentFiles = {R"(C:\Users\alice\Documents\alice-tax.7z)"};
  const std::vector<std::string> scrubbed = SplitLines(diag::BuildReport(named));
  PRISM_CHECK_EQ(ValueOf(scrubbed, "recent.1"), std::string("[user]-tax.7z"));
  ExpectNothingIdentifying(scrubbed);
}

PRISM_TEST("report: the caps are promises -- a long name keeps its extension") {
  diag::Snapshot snapshot = FullSnapshot();
  std::string longName = "C:\\Users\\alice\\Documents\\";
  longName.append(300, 'x');
  longName += ".7z";
  snapshot.recentFiles = {longName};
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(snapshot));
  const std::string name = ValueOf(lines, "recent.1");
  PRISM_CHECK(name.size() <= diag::kMaxNameChars);  // D17: bounded by construction
  PRISM_CHECK(name.size() > 3);
  PRISM_CHECK_EQ(name.substr(name.size() - 3), std::string(".7z"));
  PRISM_CHECK(name.find(diag::kEllipsis) != std::string::npos);
  ExpectNothingIdentifying(lines);
}

PRISM_TEST("report: more than 16 recent files are counted, not printed") {
  diag::Snapshot snapshot = FullSnapshot();
  snapshot.recentFiles.clear();
  for (int i = 0; i < 20; ++i) {
    snapshot.recentFiles.push_back("C:\\Users\\alice\\a" + std::to_string(i) + ".7z");
  }
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(snapshot));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.count"), std::string("16"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.truncated"), std::string("4"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.16"), std::string("a15.7z"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.17"), std::string(kMissing));
  ExpectNothingIdentifying(lines);
}

PRISM_TEST("report: a value can never forge a second line") {
  diag::Snapshot snapshot = FullSnapshot();
  // A "value" that tries to become two fields. StripControl turns the newlines
  // into spaces, so the report keeps its shape (the C16 framing rule).
  snapshot.language = "en-US\nversion = 9.9\r\nengine.path = C:\\Users\\alice\\x";
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(snapshot));
  PRISM_CHECK(!HasLine(lines, "version = 9.9"));
  PRISM_CHECK_EQ(ValueOf(lines, "version"), std::string("0.1.0.0"));
  PRISM_CHECK_EQ(lines.size(), static_cast<std::size_t>(28));
  ExpectNothingIdentifying(lines);  // even a smuggled path is redacted
}

// ===========================================================================
// 3. D1: nothing is omitted. A value the probe could not answer is "unknown".
// ===========================================================================
PRISM_TEST("report: a missing value is printed as unknown, never dropped") {
  diag::Snapshot empty;
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(empty));
  // 2 markers + 13 fixed fields + recent.count + 9 log fields = 25; there is no
  // recent.N line because there is no entry (the COUNT is the field, the rows
  // are data -- the shape is stable in its fields, not in its row count).
  PRISM_CHECK_EQ(lines.size(), static_cast<std::size_t>(25));
  PRISM_CHECK_EQ(ValueOf(lines, "generated"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "version"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "engine.path"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "engine.version"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "engine.formats"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "os"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "arch"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "install.mode"), std::string("unknown"));
  PRISM_CHECK_EQ(ValueOf(lines, "recent.count"), std::string("0"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.sampled"), std::string("0"));
  // The build flavour is a compile-time fact, so it is never "unknown".
  PRISM_CHECK_EQ(ValueOf(lines, "build"),
                 std::string(diag::BuildFlavorName(diag::CompileTimeBuildFlavor())));
}

// ===========================================================================
// 4. The log section is COUNTS ONLY, over the last 20 records (D25).
// ===========================================================================
PRISM_TEST("log counts: the window is the LAST 20 records, and messages never leak") {
  diag::Snapshot snapshot = FullSnapshot();
  snapshot.logLines.clear();
  for (int i = 0; i < 5; ++i) snapshot.logLines.push_back(LogLine("TRACE", "ui", "early"));
  for (int i = 0; i < 20; ++i) snapshot.logLines.push_back(LogLine("INFO", "ui", "late"));
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(snapshot));
  PRISM_CHECK_EQ(ValueOf(lines, "log.sampled"), std::string("20"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.trace"), std::string("0"));  // dropped by the window
  PRISM_CHECK_EQ(ValueOf(lines, "log.info"), std::string("20"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.warn"), std::string("0"));
}

PRISM_TEST("log counts: a line the wire format does not explain is counted, not fatal") {
  diag::Snapshot snapshot = FullSnapshot();
  snapshot.logLines = {"garbage-without-a-level", LogLine("WARN", "shell", "hm"),
                       LogLine("ERR", "shell", "not a level")};
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(snapshot));
  PRISM_CHECK_EQ(ValueOf(lines, "log.sampled"), std::string("3"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.warn"), std::string("1"));
  PRISM_CHECK_EQ(ValueOf(lines, "log.other"), std::string("2"));
}

// ===========================================================================
// 5. The rules that live in this header and are cheap to pin here.
// ===========================================================================
PRISM_TEST("install mode: the installed/portable rule is a pure function") {
  const std::string local = "C:\\Users\\bob\\AppData\\Local";
  PRISM_CHECK(diag::DetectInstallMode(
                  "C:\\Users\\bob\\AppData\\Local\\Programs\\PrismArchive\\PrismArchive.exe", local,
                  false) == diag::InstallMode::kInstalled);
  PRISM_CHECK(diag::DetectInstallMode("C:\\Program Files\\PrismArchive\\PrismArchive.exe", "",
                                      false) == diag::InstallMode::kInstalled);
  PRISM_CHECK(diag::DetectInstallMode("C:\\Program Files (x86)\\PrismArchive\\PrismArchive.exe",
                                      "", false) == diag::InstallMode::kInstalled);
  PRISM_CHECK(diag::DetectInstallMode("D:\\tools\\PrismArchive\\PrismArchive.exe", local, false) ==
              diag::InstallMode::kPortable);
  // Forward slashes and case are the same folder (B4: the rule, not the spelling).
  PRISM_CHECK(diag::DetectInstallMode("c:/program files/prismarchive/PrismArchive.exe", "",
                                      false) == diag::InstallMode::kInstalled);
  // The marker wins over the path: a portable copy inside Programs is still portable.
  PRISM_CHECK(diag::DetectInstallMode("C:\\Program Files\\PrismArchive\\PrismArchive.exe", "",
                                      true) == diag::InstallMode::kPortable);
  PRISM_CHECK(diag::DetectInstallMode("", local, false) == diag::InstallMode::kUnknown);
}

PRISM_TEST("build flavour: the compile-time fact matches the compiler's own macro") {
  const diag::BuildFlavor flavor = diag::CompileTimeBuildFlavor();
  PRISM_CHECK(std::string(diag::BuildFlavorName(flavor)) != std::string("unknown"));
#if defined(__SANITIZE_ADDRESS__) || defined(PRISM_ASAN) || defined(_SANITIZE_ADDRESS_)
  PRISM_CHECK(flavor == diag::BuildFlavor::kAddressSanitizer);
#endif
}

#if !defined(PRISM_C57_MUTANT_NO_REDACTION)
PRISM_TEST("redaction: the redactor is live in this build (D3 self-check)") {
  // The mutant build compiles this case out; the clean build proves the counter
  // is ZERO, so the mutant entry's CAUGHT marker can only come from the
  // injection and not from a stale global.
  diag::ResetRedactionMutant();
  PRISM_CHECK(!diag::RedactionMutantFired());
  const std::vector<std::string> lines = SplitLines(diag::BuildReport(FullSnapshot()));
  PRISM_CHECK(!diag::RedactionMutantFired());
  PRISM_CHECK_EQ(ValueOf(lines, "engine.path"),
                 std::string("C:\\") + diag::kEllipsis + "\\7z.dll");
}
#endif

int main(int argc, char** argv) {
#if defined(PRISM_C57_MUTANT_NO_REDACTION)
  // The matrix's injection evidence (D28): the binary says out loud that it is
  // the mutant, so "the macro was on the command line" is not the only proof.
  std::puts("C57 MUTANT ACTIVE: no_redaction (full paths and the user name survive)");
  // CONTRACTS D27: arm the verdict BEFORE the first case. The marker is a
  // conjunction -- this run went red AND the injection provably ran -- and the
  // harness prints it at the FIRST failure, so a teardown crash cannot swallow
  // it. The predicate is what keeps an unrelated red from being reported as
  // CAUGHT (D3).
  ::prism::test::SetMutationVerdict("C57 MUTATION CAUGHT: no_redaction",
                                    [] { return diag::RedactionMutantFired(); });
#endif
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) return 1;
  std::puts("ALL DIAGNOSTICS-REPORT CHECKS PASSED");
  return 0;
}
