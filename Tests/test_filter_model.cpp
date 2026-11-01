// PrismArchive -- PrismCore test
// FilterModel: substring / glob / type / size-range filters and the selection
// bookkeeping that keeps a selection attached to its files while the list is
// being narrowed.
//
// The selection contract is the sharp edge: the caller's selection is a list of
// CATALOG INDICES, and rows are only removed, never renumbered. Negative
// controls (CONTRACTS D3, built by Tests/suites/c13-listmodel.cmake with
// WILL_FAIL):
//   PRISM_C13_MUTANT_GLOB_STAR_STOPS_AT_SLASH -> '*' crossing a separator goes red
//   PRISM_C13_MUTANT_SELECTION_BY_POSITION    -> selection-by-index goes red
// A mutation macro that stopped matching would make its mutant PASS and the
// WILL_FAIL registration go red, so the injection is verified, not assumed.
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#include "mini_test.hpp"

#include "FilterModel.hpp"

namespace {

using prism::list::ApplyFilter;
using prism::list::ClassifyType;
using prism::list::ContainsFold;
using prism::list::FilterResult;
using prism::list::FilterSpec;
using prism::list::MatchesFilter;
using prism::list::MatchesGlob;
using prism::list::Row;
using prism::list::TypeClass;
using prism::list::TypeTagOf;

Row MakeRow(std::string name, uint32_t index, uint64_t size = 0, bool isDir = false) {
  Row row;
  row.catalogIndex = index;
  row.name = std::move(name);
  row.typeTag = TypeTagOf(row.name);
  row.size = size;
  row.packSize = size / 2;
  row.isDir = isDir;
  if (isDir) {
    row.size = 0;
    row.typeTag.clear();
  }
  return row;
}

std::vector<std::string> NamesOf(const std::vector<Row>& rows,
                                 const std::vector<uint32_t>& indices) {
  std::vector<std::string> out;
  for (uint32_t index : indices) {
    for (const Row& row : rows) {
      if (row.catalogIndex == index) {
        out.push_back(row.name);
        break;
      }
    }
  }
  return out;
}

std::string Join(const std::vector<std::string>& parts) {
  std::string out;
  for (const std::string& part : parts) {
    if (!out.empty()) out += '|';
    out += part;
  }
  return out;
}

std::vector<std::string> Joined(const std::vector<Row>& rows,
                                const std::vector<uint32_t>& indices) {
  return NamesOf(rows, indices);
}

// The fixture used by most cases: five files with distinct types and sizes.
std::vector<Row> Fixture() {
  return {
      MakeRow("a.txt", 0, 100),
      MakeRow("b.png", 1, 2048),
      MakeRow("c.7z", 2, 5000),
      MakeRow("年度报告.txt", 3, 300),
      MakeRow("dir", 4, 0, true),
  };
}

}  // namespace

// --------------------------------------------------------------------------
// Substring
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.substring_is_ascii_case_insensitive") {
  PRISM_CHECK(ContainsFold("Annual Report.TXT", "report"));
  PRISM_CHECK(ContainsFold("Annual Report.TXT", "ANNUAL"));
  PRISM_CHECK(ContainsFold("abc", ""));
  PRISM_CHECK(!ContainsFold("abc", "abcd"));
  PRISM_CHECK(!ContainsFold("abc", "bcd"));
  // CJK has no case: byte equality, and the match may start mid-name.
  PRISM_CHECK(ContainsFold("年度报告.txt", "报告"));
  PRISM_CHECK(ContainsFold("年度报告.txt", "年度"));
  PRISM_CHECK(!ContainsFold("年度报告.txt", "季度"));

  FilterSpec spec;
  spec.substring = "报告";
  const std::vector<Row> rows = Fixture();
  PRISM_CHECK(MatchesFilter(rows[3], spec));
  PRISM_CHECK(!MatchesFilter(rows[0], spec));
}

// --------------------------------------------------------------------------
// Glob
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.glob_question_matches_one_code_point") {
  PRISM_CHECK(MatchesGlob("*.txt", "a.txt"));
  PRISM_CHECK(!MatchesGlob("*.txt", "a.txt.bak"));
  PRISM_CHECK(MatchesGlob("report?.txt", "report1.txt"));
  PRISM_CHECK(!MatchesGlob("report?.txt", "report10.txt"));
  // '?' is one CODE POINT, not one byte: 报 is 3 UTF-8 bytes.
  PRISM_CHECK(MatchesGlob("报?.txt", "报告.txt"));
  PRISM_CHECK(!MatchesGlob("报?.txt", "报告会.txt"));
  PRISM_CHECK(MatchesGlob("*", "anything at all"));
  PRISM_CHECK(MatchesGlob("", ""));
  PRISM_CHECK(!MatchesGlob("", "x"));
}

// '*' spans the path separator: the list shows archive-internal paths, so
// "*.txt" has to find "docs/notes/a.txt" too.
PRISM_TEST("filter_model.glob_star_crosses_separators") {
  PRISM_CHECK(MatchesGlob("*.txt", "docs/notes/a.txt"));
  PRISM_CHECK(MatchesGlob("docs/*", "docs/a/b.txt"));
  PRISM_CHECK(MatchesGlob("*a/b*", "x/a/b/y"));
  PRISM_CHECK(!MatchesGlob("docs/*", "src/a.txt"));
  PRISM_CHECK(MatchesGlob("a/*/c", "a/b/c"));
  PRISM_CHECK(!MatchesGlob("a/*/c", "a/b/d"));
}

// --------------------------------------------------------------------------
// Type
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.type_classification_table") {
  PRISM_CHECK_EQ(TypeTagOf("Report.TXT"), std::string("txt"));
  PRISM_CHECK_EQ(TypeTagOf("docs/.gitignore"), std::string(""));  // dotfile, not an extension
  PRISM_CHECK_EQ(TypeTagOf("name."), std::string(""));
  PRISM_CHECK_EQ(TypeTagOf("noext"), std::string(""));

  PRISM_CHECK_EQ(ClassifyType("a.txt", false), TypeClass::kDocuments);
  PRISM_CHECK_EQ(ClassifyType("a.pdf", false), TypeClass::kDocuments);
  PRISM_CHECK_EQ(ClassifyType("a.PNG", false), TypeClass::kImages);
  PRISM_CHECK_EQ(ClassifyType("a.flac", false), TypeClass::kMedia);
  PRISM_CHECK_EQ(ClassifyType("a.7z", false), TypeClass::kArchives);
  PRISM_CHECK_EQ(ClassifyType("a.hpp", false), TypeClass::kCode);
  PRISM_CHECK_EQ(ClassifyType("a.bin", false), TypeClass::kOther);
  PRISM_CHECK_EQ(ClassifyType("a.txt", true), TypeClass::kFolders);
}

PRISM_TEST("filter_model.type_filter_excludes_everything_else") {
  const std::vector<Row> rows = Fixture();
  FilterSpec spec;
  spec.type = TypeClass::kImages;
  const FilterResult result = ApplyFilter(rows, {}, spec);
  PRISM_CHECK_EQ(result.visible.size(), size_t(1));
  PRISM_CHECK_EQ(Join(Joined(rows, result.visible)), std::string("b.png"));

  FilterSpec folders;
  folders.type = TypeClass::kFolders;
  const FilterResult dirs = ApplyFilter(rows, {}, folders);
  PRISM_CHECK_EQ(Join(Joined(rows, dirs.visible)), std::string("dir"));
}

// --------------------------------------------------------------------------
// Size range
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.size_range_is_inclusive_and_bites_folders") {
  const std::vector<Row> rows = Fixture();
  FilterSpec spec;
  spec.minSize = uint64_t(300);
  spec.maxSize = uint64_t(2048);
  const FilterResult result = ApplyFilter(rows, {}, spec);
  // Boundaries are inclusive: 300 and 2048 are both in. The folder carries
  // size 0 in the catalog, so a non-zero minimum hides it (deliberate).
  PRISM_CHECK_EQ(Join(Joined(rows, result.visible)), std::string("b.png|年度报告.txt"));

  FilterSpec openMin;
  openMin.minSize = uint64_t(0);
  PRISM_CHECK_EQ(ApplyFilter(rows, {}, openMin).visible.size(), size_t(5));

  FilterSpec emptyRange;
  emptyRange.minSize = uint64_t(6000);
  PRISM_CHECK_EQ(ApplyFilter(rows, {}, emptyRange).visible.size(), size_t(0));
}

// --------------------------------------------------------------------------
// Combination + stability
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.constraints_combine_with_and") {
  const std::vector<Row> rows = Fixture();
  FilterSpec spec;
  spec.substring = "report";   // nothing matches (the CJK name is 报告, not report)
  spec.type = TypeClass::kDocuments;
  PRISM_CHECK_EQ(ApplyFilter(rows, {}, spec).visible.size(), size_t(0));

  FilterSpec both;
  both.glob = "*.txt";
  both.type = TypeClass::kDocuments;
  both.minSize = uint64_t(1);
  const FilterResult result = ApplyFilter(rows, {}, both);
  PRISM_CHECK_EQ(Join(Joined(rows, result.visible)), std::string("a.txt|年度报告.txt"));
}

PRISM_TEST("filter_model.filter_is_a_stable_projection") {
  const std::vector<Row> rows = Fixture();
  FilterSpec spec;
  spec.glob = "*.*";
  const FilterResult first = ApplyFilter(rows, {}, spec);
  const FilterResult second = ApplyFilter(rows, {}, spec);
  PRISM_CHECK_EQ(Join(Joined(rows, first.visible)), std::string("a.txt|b.png|c.7z|年度报告.txt"));
  PRISM_CHECK(first.visible == second.visible);  // idempotent

  const FilterResult empty = ApplyFilter(rows, {}, FilterSpec{});
  PRISM_CHECK_EQ(empty.visible.size(), size_t(5));
  PRISM_CHECK_EQ(empty.hiddenSelection, size_t(0));
}

// --------------------------------------------------------------------------
// Selection: catalog indices, not row positions
// --------------------------------------------------------------------------

PRISM_TEST("filter_model.selection_is_kept_by_catalog_index") {
  // Catalog indices deliberately unrelated to row positions: reading the
  // selection as a position is the mutant this case has to catch.
  std::vector<Row> rows = {
      MakeRow("keep.txt", 1000, 10),
      MakeRow("hide.bin", 1001, 10),
      MakeRow("keep2.txt", 1002, 10),
      MakeRow("keep3.txt", 1003, 10),
  };
  const std::vector<uint32_t> selection = {1000, 1001, 1003};
  FilterSpec spec;
  spec.glob = "*.txt";
  const FilterResult result = ApplyFilter(rows, selection, spec);
  PRISM_CHECK_EQ(Join(Joined(rows, result.visible)), std::string("keep.txt|keep2.txt|keep3.txt"));
  // The hidden-by-filter member is dropped from the selection, and counted so
  // the status line can say so -- never silently forgotten.
  if (!PRISM_CHECK_EQ(result.selection.size(), size_t(2))) return;  // no OOB on a mutation
  PRISM_CHECK_EQ(result.selection[0], 1000u);
  PRISM_CHECK_EQ(result.selection[1], 1003u);
  PRISM_CHECK_EQ(result.hiddenSelection, size_t(1));
  PRISM_CHECK(result.IsVisible(1000));
  PRISM_CHECK(!result.IsVisible(1001));
}

PRISM_TEST("filter_model.selection_entries_are_not_row_positions") {
  // Small selection values (1, 2) that HAPPEN to be valid row positions but
  // are not the rows the user selected. Under the position mutant this case
  // returns keep2.bin / keep3.txt instead of catching the small indices.
  std::vector<Row> rows = {
      MakeRow("first.txt", 1, 10),
      MakeRow("second.bin", 2, 10),
      MakeRow("third.txt", 3, 10),
  };
  const std::vector<uint32_t> selection = {2, 3};  // catalog indices, all visible
  const FilterResult result = ApplyFilter(rows, selection, FilterSpec{});
  PRISM_CHECK_EQ(result.visible.size(), size_t(3));
  if (!PRISM_CHECK_EQ(result.selection.size(), size_t(2))) return;  // no OOB on a mutation
  PRISM_CHECK_EQ(result.selection[0], 2u);
  PRISM_CHECK_EQ(result.selection[1], 3u);
  PRISM_CHECK_EQ(result.hiddenSelection, size_t(0));
}

PRISM_TEST("filter_model.selection_survives_returning_to_the_full_list") {
  std::vector<Row> rows = {
      MakeRow("a.txt", 40, 10), MakeRow("b.png", 41, 10), MakeRow("c.txt", 42, 10),
  };
  const std::vector<uint32_t> selection = {40, 42};

  FilterSpec narrowed;
  narrowed.type = TypeClass::kImages;
  const FilterResult hidden = ApplyFilter(rows, selection, narrowed);
  PRISM_CHECK_EQ(hidden.visible.size(), size_t(1));
  PRISM_CHECK_EQ(hidden.selection.size(), size_t(0));
  PRISM_CHECK_EQ(hidden.hiddenSelection, size_t(2));

  // Clearing the filter restores the rows; the caller still holds 40 and 42,
  // which is the whole point of an index-based selection.
  const FilterResult restored = ApplyFilter(rows, selection, FilterSpec{});
  PRISM_CHECK_EQ(restored.selection.size(), size_t(2));
  PRISM_CHECK(restored.IsVisible(40));
  PRISM_CHECK(restored.IsVisible(42));
}

namespace {

void PrintMutantBanner() {
#if defined(PRISM_C13_MUTANT_GLOB_STAR_STOPS_AT_SLASH)
  std::cout << "C13 MUTANT ACTIVE: GLOB_STAR_STOPS_AT_SLASH ('*' cannot cross '/')"
            << std::endl;
#endif
#if defined(PRISM_C13_MUTANT_SELECTION_BY_POSITION)
  std::cout << "C13 MUTANT ACTIVE: SELECTION_BY_POSITION (selection read as row indices)"
            << std::endl;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  PrintMutantBanner();
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL FILTER-MODEL CHECKS PASSED" << std::endl;
  return 0;
}
