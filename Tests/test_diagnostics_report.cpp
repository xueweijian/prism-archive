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
// mini_test prints the CAUGHT marker at the FIRST failure and leaves with 0, and
// the mutant's ctest entry requires that marker (D26: no WILL_FAIL is stacked on
// it). A stale injection keeps this suite green, which turns the mutant entry
// red -- the guard proves itself. Arm names avoid clean/unarmed/control (D28).
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

std::string Line(std::string_view key, const std::string& value) {
  return std::string(key) + " = " + value;
}

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

std::string ValueOf(const std::vector<std::string>& lines, std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  for (const std::string& line : lines) {
    if (line.compare(0, prefix.size(), prefix) == 0) return line.substr(prefix.size());
  }
  return std::string("<missing>");
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

}  // namespace
