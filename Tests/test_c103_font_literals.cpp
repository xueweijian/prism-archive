// PrismArchive -- C103: the source scan half of the font-stack card.
//
// BLUEPRINT:170 names ONE stack (SF Pro Display / Inter / PingFang SC /
// HarmonyOS Sans SC, Segoe UI fallback). Before C103, 19 DirectWrite call
// sites wrote L"Segoe UI" as the first argument of CreateTextFormat and two
// GDI call sites passed the same literal to CreateFontW, so the token could
// never resolve to anything else. This suite is the mechanical guard:
//
//   assertion 1  "Segoe UI" appears ZERO times under Src/ (Typography.hpp is
//                the one file allowed to name the fallback);
//   assertion 2  no source line calls DWrite's CreateTextFormat directly --
//                every creation goes through typography::CreateTextFormat.
//
// NON-VACUOUS BY CONSTRUCTION: the scan also asserts that it actually walked
// >= 50 source files and that Typography.hpp was among them. Pointing the
// compile-time root at the wrong directory turns the suite RED instead of
// silently reporting zero hits.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26): the SAME source compiled with
// PRISM_C103_SCAN_FIXTURE=1 scans Tests/fixtures/c103, which contains the
// literal on purpose. The zero-hit assertion MUST go red and the D27 marker
// "C103 MUTATION CAUGHT: segoe_literal" is printed at that first failure.
// No WILL_FAIL is stacked on the regex (D26).
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mini_test.hpp"

#ifndef PRISM_C103_SRC_DIR
#error "c103: PRISM_C103_SRC_DIR must name the directory to scan"
#endif

namespace {

namespace fs = std::filesystem;

struct Offense {
  std::string path;
  int line = 0;
  std::string text;
};

struct ScanResult {
  std::size_t files = 0;
  bool typographySeen = false;
  std::vector<Offense> segoe;
  std::vector<Offense> directCalls;
};

// The mutation predicate reads this; one process, one scan, one global.
std::size_t gSegoHits = 0;

bool IsScanned(const fs::path& path) {
  const std::string ext = path.extension().string();
  return ext == ".hpp" || ext == ".h" || ext == ".cpp" || ext == ".cc" ||
         ext == ".cxx" || ext == ".inl" || ext == ".rc";
}

std::size_t CountDirectCreateTextFormat(const std::string& line) {
  const std::string needle = "CreateTextFormat";
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = line.find(needle, pos)) != std::string::npos) {
    std::size_t i = pos;
    while (i > 0 && (line[i - 1] == ' ' || line[i - 1] == '\t')) --i;
    if (i >= 2 && line[i - 1] == '>' && line[i - 2] == '-') {
      ++count;  // writeFactory->CreateTextFormat(...)
    } else if (i >= 1 && line[i - 1] == '.') {
      ++count;  // writeFactory.CreateTextFormat(...)
    }
    pos += needle.size();
  }
  return count;
}

ScanResult ScanTree(const fs::path& root) {
  ScanResult result;
  std::error_code error;
  if (!fs::exists(root, error) || error) return result;

  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                     error);
  const fs::recursive_directory_iterator end;
  for (; !error && it != end; it.increment(error)) {
    const fs::directory_entry& entry = *it;
    std::error_code entryError;
    if (!entry.is_regular_file(entryError) || entryError) continue;
    if (!IsScanned(entry.path())) continue;
    ++result.files;
    if (entry.path().filename() == "Typography.hpp") result.typographySeen = true;

    std::ifstream input(entry.path());
    std::string line;
    int number = 0;
    while (std::getline(input, line)) {
      ++number;
      if (line.find("Segoe UI") != std::string::npos &&
          entry.path().filename() != "Typography.hpp") {
        result.segoe.push_back({entry.path().string(), number, line});
      }
      if (CountDirectCreateTextFormat(line) != 0 &&
          entry.path().filename() != "Typography.hpp") {
        result.directCalls.push_back({entry.path().string(), number, line});
      }
    }
  }
  return result;
}

void PrintOffenses(const char* what, const std::vector<Offense>& offenses) {
  for (const Offense& offense : offenses) {
    std::printf("    %s  %s:%d  %s\n", what, offense.path.c_str(), offense.line,
                offense.text.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
#if defined(PRISM_C103_SCAN_FIXTURE)
  // The fixture deliberately contains the literal: the zero-hit assertion below
  // MUST go red, and the verdict proves the injected defect really reached it.
  ::prism::test::SetMutationVerdict("C103 MUTATION CAUGHT: segoe_literal",
                                    [] { return gSegoHits > 0; });
#endif
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) return failures;
  std::puts("ALL C103 FONT-LITERAL CHECKS PASSED");
  return 0;
}

PRISM_TEST("c103 Src carries no font literal and no direct DWrite TextFormat call") {
  const ScanResult scan = ScanTree(fs::path(PRISM_C103_SRC_DIR));
  std::printf("  scanned %zu files under %s\n", scan.files, PRISM_C103_SRC_DIR);

#if defined(PRISM_C103_SCAN_FIXTURE)
  // The fixture has exactly one file; the positive check only proves the
  // scanner opened something before the zero-hit assertion fires.
  PRISM_CHECK(scan.files >= 1);
#else
  // A misspelt root must never look like "zero hits". The worktree has well
  // over 50 headers/sources under Src/, and Typography.hpp is the file the
  // other assertion exempts -- if it was not visited the scan is not real.
  PRISM_CHECK(scan.files >= 50);
  PRISM_CHECK(scan.typographySeen);
#endif

  PrintOffenses("SEGOE", scan.segoe);
  PrintOffenses("DIRECT", scan.directCalls);
  gSegoHits = scan.segoe.size();

  // Assertion 1 (BLUEPRINT:170): the stack lives in ONE file.
  PRISM_CHECK_EQ(scan.segoe.size(), std::size_t{0});
  // Assertion 2: the ONE CreateTextFormat entry.
  PRISM_CHECK_EQ(scan.directCalls.size(), std::size_t{0});
}
