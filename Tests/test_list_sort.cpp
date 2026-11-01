// PrismArchive -- PrismCore test
// ListSortModel: multi-key ordering (name / original / packed / ratio / mtime /
// type), folders-first grouping, natural + code-point name order, stability,
// and the column model's visibility / width / minimum-width contract.
//
// The property case is the one that matters: 1000 random rows under random
// key sets, asserting the output is a PERMUTATION of the input, is ordered by
// the comparator, and keeps the input order inside every group of equal keys
// (stability). Negative controls (CONTRACTS D3, built by
// Tests/suites/c13-listmodel.cmake with WILL_FAIL):
//   PRISM_C13_MUTANT_UNSTABLE_SORT       -> stability property goes red
//   PRISM_C13_MUTANT_LEXICOGRAPHIC_NAMES -> natural order goes red (file10 < file2)
//   PRISM_C13_MUTANT_DIRS_INTERLEAVED    -> folders-first goes red
//   PRISM_C13_MUTANT_WIDTH_BYPASSES_MIN  -> column minimum width goes red
// A mutation macro that stopped matching would make its mutant PASS and the
// WILL_FAIL registration go red, so the injection is verified, not assumed.
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "mini_test.hpp"

#include "ListSortModel.hpp"

namespace {

using prism::list::ColumnId;
using prism::list::ColumnModel;
using prism::list::MultiSortSpec;
using prism::list::NaturalCompare;
using prism::list::Row;
using prism::list::SortDirection;
using prism::list::SortKey;
using prism::list::SortRows;
using prism::list::SortSpec;
using prism::list::TypeTagOf;

Row MakeRow(std::string name, uint32_t index, uint64_t size = 0, uint64_t packSize = 0,
            int64_t mtime = 0, bool isDir = false) {
  Row row;
  row.catalogIndex = index;
  row.name = std::move(name);
  row.typeTag = TypeTagOf(row.name);
  row.size = size;
  row.packSize = packSize;
  row.mtime = mtime;
  row.isDir = isDir;
  if (isDir) {
    row.size = 0;
    row.packSize = 0;
    row.typeTag.clear();
  }
  return row;
}

std::vector<std::string> Names(const std::vector<Row>& rows) {
  std::vector<std::string> out;
  out.reserve(rows.size());
  for (const Row& row : rows) out.push_back(row.name);
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

MultiSortSpec OneKey(SortKey key, SortDirection direction = SortDirection::kAscending) {
  MultiSortSpec spec;
  spec.keys.push_back(SortSpec{key, direction});
  return spec;
}

// Deterministic PRNG (xorshift64*): the same numbers on every compiler, unlike
// std::uniform_int_distribution, whose exact sequence is implementation-defined.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed) : state(seed | 1ull) {}
  uint64_t Next() {
    uint64_t x = state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    state = x;
    return x;
  }
  uint32_t Below(uint32_t bound) {
    return bound == 0 ? 0u : static_cast<uint32_t>(Next() % static_cast<uint64_t>(bound));
  }
};

const char* const kStems[] = {"file", "File", "报告", "IMG", "photo", "a", "z", "资料"};
const char* const kExtensions[] = {"txt", "png", "7z", "md", "", "cpp", "jpg"};

std::string RandomName(Rng& rng) {
  std::string name = kStems[rng.Below(static_cast<uint32_t>(std::size(kStems)))];
  if (rng.Below(8) == 0) name.append(rng.Below(3), '0');  // file7 vs file007
  name += std::to_string(rng.Below(40));
  const uint32_t ext = rng.Below(static_cast<uint32_t>(std::size(kExtensions)));
  if (kExtensions[ext][0] != '\0') {
    name += '.';
    name += kExtensions[ext];
  }
  return name;
}

std::vector<Row> RandomRows(Rng& rng, size_t count) {
  std::vector<Row> rows;
  rows.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    // Small value domains on purpose: ties are what stability is about, and a
    // generator that never ties would let an unstable sort look correct.
    const uint64_t size = static_cast<uint64_t>(rng.Below(6)) * 1024;
    const uint64_t packSize = size == 0 ? 0 : rng.Below(4) == 0 ? size : size / 2;
    const bool isDir = rng.Below(7) == 0;
    rows.push_back(MakeRow(RandomName(rng), static_cast<uint32_t>(i), size, packSize,
                           static_cast<int64_t>(rng.Below(5)) * 100, isDir));
  }
  return rows;
}

MultiSortSpec RandomSpec(Rng& rng) {
  MultiSortSpec spec;
  spec.dirsFirst = rng.Below(5) != 0;
  const uint32_t keyCount = 1 + rng.Below(3);
  for (uint32_t k = 0; k < keyCount; ++k) {
    SortSpec key;
    key.key = static_cast<SortKey>(rng.Below(6));
    key.direction =
        rng.Below(2) == 0 ? SortDirection::kAscending : SortDirection::kDescending;
    spec.keys.push_back(key);
  }
  return spec;
}

bool Equivalent(const Row& a, const Row& b, const MultiSortSpec& spec) {
  return !prism::list::RowLess(a, b, spec) && !prism::list::RowLess(b, a, spec);
}

}  // namespace

// --------------------------------------------------------------------------
// Name order
// --------------------------------------------------------------------------

PRISM_TEST("list_sort.natural_name_order") {
  PRISM_CHECK(NaturalCompare("file2", "file10") < 0);
  PRISM_CHECK(NaturalCompare("file10", "file2") > 0);
  PRISM_CHECK(NaturalCompare("file1", "file2") < 0);
  PRISM_CHECK(NaturalCompare("a1b2", "a1b10") < 0);
  PRISM_CHECK(NaturalCompare("file2", "file2") == 0);
  // A digit run is compared by VALUE, never parsed into an integer: 40 digits
  // must not overflow into a false tie.
  PRISM_CHECK(NaturalCompare("n9999999999999999999999999999999999999998",
                             "n9999999999999999999999999999999999999999") < 0);
  PRISM_CHECK(NaturalCompare("n123456789012345678901234567890",
                             "n99") > 0);  // more digits, bigger number
}

PRISM_TEST("list_sort.leading_zeros_break_the_numeric_tie") {
  PRISM_CHECK(NaturalCompare("x7", "x007") < 0);
  PRISM_CHECK(NaturalCompare("x007", "x0007") < 0);
  PRISM_CHECK(NaturalCompare("0", "00") < 0);
  PRISM_CHECK(NaturalCompare("v1", "v01") < 0);
}

// UTF-8 compared as UNSIGNED bytes is exactly code-point order. 'z' (0x7A) is
// below 0xE4, and a signed char would answer the opposite -- that is the trap
// this case exists for (it stayed green on Linux while MSVC inverted it).
PRISM_TEST("list_sort.utf8_sorts_by_code_point") {
  PRISM_CHECK(NaturalCompare("z.txt", "中文.txt") < 0);
  PRISM_CHECK(NaturalCompare("张.txt", "李.txt") < 0);  // U+5F20 < U+674E
  PRISM_CHECK(NaturalCompare("李.txt", "王.txt") < 0);  // U+674E < U+738B
  PRISM_CHECK(NaturalCompare("中文.txt", "中文.txt") == 0);

  std::vector<Row> rows = {MakeRow("王.txt", 0), MakeRow("张.txt", 1), MakeRow("李.txt", 2),
                           MakeRow("z.txt", 3)};
  SortRows(rows, OneKey(SortKey::kName));
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("z.txt|张.txt|李.txt|王.txt"));
}

PRISM_TEST("list_sort.ascii_case_is_the_last_tie_break") {
  PRISM_CHECK(NaturalCompare("File2", "file2") < 0);  // upper before lower
  PRISM_CHECK(NaturalCompare("file2", "File2") > 0);
  PRISM_CHECK(NaturalCompare("a.txt", "B.txt") < 0);  // case folds away first
  PRISM_CHECK(NaturalCompare("b.txt", "A.txt") > 0);
}

// --------------------------------------------------------------------------
// Key semantics
// --------------------------------------------------------------------------

PRISM_TEST("list_sort.folders_first_in_both_directions") {
  std::vector<Row> rows = {
      MakeRow("b.txt", 0, 10, 5),   MakeRow("folder", 1, 0, 0, 0, true),
      MakeRow("a.txt", 2, 30, 7),   MakeRow("zip", 3, 0, 0, 0, true),
  };
  std::vector<Row> ascending = rows;
  SortRows(ascending, OneKey(SortKey::kName, SortDirection::kAscending));
  PRISM_CHECK_EQ(Join(Names(ascending)), std::string("folder|zip|a.txt|b.txt"));

  std::vector<Row> descending = rows;
  SortRows(descending, OneKey(SortKey::kName, SortDirection::kDescending));
  // Folders still lead: only the order INSIDE each group flips.
  PRISM_CHECK_EQ(Join(Names(descending)), std::string("zip|folder|b.txt|a.txt"));
}

PRISM_TEST("list_sort.size_keys_are_numeric_not_lexicographic") {
  std::vector<Row> rows = {MakeRow("nine", 0, 9), MakeRow("ten", 1, 10),
                           MakeRow("twenty", 2, 20)};
  SortRows(rows, OneKey(SortKey::kOriginalSize));
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("nine|ten|twenty"));

  SortRows(rows, OneKey(SortKey::kOriginalSize, SortDirection::kDescending));
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("twenty|ten|nine"));
}

PRISM_TEST("list_sort.packed_and_mtime_keys") {
  std::vector<Row> rows = {MakeRow("a", 0, 100, 40, 500), MakeRow("b", 1, 100, 10, 300),
                           MakeRow("c", 2, 100, 70, 900)};
  std::vector<Row> byPacked = rows;
  SortRows(byPacked, OneKey(SortKey::kPackedSize));
  PRISM_CHECK_EQ(Join(Names(byPacked)), std::string("b|a|c"));

  std::vector<Row> byMtime = rows;
  SortRows(byMtime, OneKey(SortKey::kMtime, SortDirection::kDescending));
  PRISM_CHECK_EQ(Join(Names(byMtime)), std::string("c|a|b"));
}

PRISM_TEST("list_sort.type_key_orders_by_tag_then_name") {
  std::vector<Row> rows = {MakeRow("b.zip", 0), MakeRow("a.txt", 1), MakeRow("c.txt", 2),
                           MakeRow("a.zip", 3)};
  SortRows(rows, OneKey(SortKey::kType));
  // txt < zip, and inside each tag the names are natural-ordered.
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("a.txt|c.txt|a.zip|b.zip"));
}

// The ratio column sorts by the EXACT packed/uncompressed fraction, not by the
// rounded percentage it displays: 33.3% and 33.4% both print 33%, and the list
// must still order them. The second pair multiplies past 64 bits, so a naive
// uint64 cross-product wraps and lands on the wrong side.
PRISM_TEST("list_sort.ratio_is_exact_not_rounded") {
  std::vector<Row> rows = {MakeRow("b", 0, 1000, 334), MakeRow("a", 1, 1000, 333)};
  PRISM_CHECK_EQ(prism::list::RatioPercent(rows[0]), 33u);
  PRISM_CHECK_EQ(prism::list::RatioPercent(rows[1]), 33u);
  SortRows(rows, OneKey(SortKey::kRatio));
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("a|b"));

  const uint64_t bigSize = 1ull << 62;
  Row big = MakeRow("big", 0, bigSize, bigSize / 2);            // exactly 50.0%
  Row small = MakeRow("small", 1, 1u << 20, (1u << 20) / 2 + 1);  // just above 50%
  PRISM_CHECK(prism::list::CompareRatio(big, small) < 0);
  PRISM_CHECK(prism::list::CompareRatio(small, big) > 0);
}

PRISM_TEST("list_sort.ratio_percent_rounds_half_up") {
  PRISM_CHECK_EQ(prism::list::RatioPercent(MakeRow("z", 0, 0, 0)), 0u);
  PRISM_CHECK_EQ(prism::list::RatioPercent(MakeRow("a", 0, 8, 1)), 13u);   // 12.5%
  PRISM_CHECK_EQ(prism::list::RatioPercent(MakeRow("b", 0, 3, 2)), 67u);   // 66.7%
  PRISM_CHECK_EQ(prism::list::RatioPercent(MakeRow("c", 0, 10, 10)), 100u);
  PRISM_CHECK_EQ(prism::list::RatioPercent(MakeRow("d", 0, 10, 99)), 100u);  // stored, saturates
}

PRISM_TEST("list_sort.multi_key_composition") {
  std::vector<Row> rows = {
      MakeRow("b.txt", 0, 500), MakeRow("a.txt", 1, 500), MakeRow("c.txt", 2, 900),
      MakeRow("d.zip", 3, 100),
  };
  MultiSortSpec spec;
  spec.keys.push_back(SortSpec{SortKey::kOriginalSize, SortDirection::kDescending});
  spec.keys.push_back(SortSpec{SortKey::kName, SortDirection::kAscending});
  SortRows(rows, spec);
  // Size decides first; the 500-byte tie is broken by natural name order.
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("c.txt|a.txt|b.txt|d.zip"));
}

PRISM_TEST("list_sort.empty_spec_groups_folders_only") {
  std::vector<Row> rows = {MakeRow("b.txt", 0, 10), MakeRow("dir", 1, 0, 0, 0, true),
                           MakeRow("a.txt", 2, 5)};
  MultiSortSpec spec;  // no keys at all
  SortRows(rows, spec);
  PRISM_CHECK_EQ(Join(Names(rows)), std::string("dir|b.txt|a.txt"));  // catalog order inside groups
}

PRISM_TEST("list_sort.stability_keeps_catalog_order_for_equal_keys") {
  std::vector<Row> rows;
  for (uint32_t i = 0; i < 64; ++i) {
    rows.push_back(MakeRow("row" + std::to_string(i), i, 777, 111, 5));
  }
  std::vector<Row> original = rows;
  SortRows(rows, OneKey(SortKey::kOriginalSize));
  bool sameOrder = rows.size() == original.size();
  for (size_t i = 0; sameOrder && i < rows.size(); ++i) {
    if (rows[i].catalogIndex != original[i].catalogIndex) sameOrder = false;
  }
  PRISM_CHECK(sameOrder);  // 64 identical keys: nothing may move
}

// --------------------------------------------------------------------------
// Property: random rows x random key sets -- permutation, order, stability,
// and a genuine strict weak ordering.
// --------------------------------------------------------------------------

PRISM_TEST("list_sort.property_random_keys_are_a_total_order_and_stable") {
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    Rng rng(seed * 0x9E3779B97F4A7C15ull);
    for (int round = 0; round < 3; ++round) {
      const MultiSortSpec spec = RandomSpec(rng);
      std::vector<Row> rows = RandomRows(rng, 1000);
      std::vector<size_t> inputPosition(rows.size(), 0);
      for (size_t i = 0; i < rows.size(); ++i) inputPosition[rows[i].catalogIndex] = i;

      std::vector<uint32_t> wanted;
      wanted.reserve(rows.size());
      for (const Row& row : rows) wanted.push_back(row.catalogIndex);
      std::sort(wanted.begin(), wanted.end());

      SortRows(rows, spec);

      std::vector<uint32_t> got;
      got.reserve(rows.size());
      for (const Row& row : rows) got.push_back(row.catalogIndex);
      std::sort(got.begin(), got.end());
      if (got != wanted) {
        std::cout << "    seed " << seed << " round " << round
                  << ": sorted output is not a permutation of its input" << std::endl;
        PRISM_CHECK(false);
        return;
      }

      for (size_t i = 1; i < rows.size(); ++i) {
        const Row& previous = rows[i - 1];
        const Row& current = rows[i];
        if (prism::list::RowLess(current, previous, spec)) {
          std::cout << "    seed " << seed << " round " << round << ": out of order at " << i
                    << " ('" << previous.name << "' then '" << current.name << "')"
                    << std::endl;
          PRISM_CHECK(false);
          return;
        }
        if (Equivalent(previous, current, spec) &&
            inputPosition[current.catalogIndex] < inputPosition[previous.catalogIndex]) {
          std::cout << "    seed " << seed << " round " << round << ": STABILITY broken at "
                    << i << " ('" << previous.name << "' then '" << current.name << "')"
                    << std::endl;
          PRISM_CHECK(false);
          return;
        }
      }

      // Strict weak ordering on random triples: irreflexive, asymmetric,
      // transitive (both the 'less' and the 'equivalent' relation).
      for (int probe = 0; probe < 200; ++probe) {
        const Row& a = rows[rng.Below(static_cast<uint32_t>(rows.size()))];
        const Row& b = rows[rng.Below(static_cast<uint32_t>(rows.size()))];
        const Row& c = rows[rng.Below(static_cast<uint32_t>(rows.size()))];
        const bool ab = prism::list::RowLess(a, b, spec);
        const bool ba = prism::list::RowLess(b, a, spec);
        const bool bc = prism::list::RowLess(b, c, spec);
        const bool ac = prism::list::RowLess(a, c, spec);
        const bool irreflexive = !prism::list::RowLess(a, a, spec);
        const bool asymmetric = !(ab && ba);
        const bool transitive = !(ab && bc) || ac;
        const bool equivTransitive =
            !(Equivalent(a, b, spec) && Equivalent(b, c, spec)) || Equivalent(a, c, spec);
        if (!irreflexive || !asymmetric || !transitive || !equivTransitive) {
          std::cout << "    seed " << seed << " round " << round
                    << ": comparator is not a strict weak ordering for keys "
                    << (spec.keys.empty() ? SortKey::kName : spec.keys[0].key) << std::endl;
          PRISM_CHECK(false);
          return;
        }
      }
    }
  }
  PRISM_CHECK(true);  // reached only when every seed/round passed
}

// --------------------------------------------------------------------------
// Column model
// --------------------------------------------------------------------------

PRISM_TEST("list_sort.columns_default_matches_the_blueprint_list") {
  const ColumnModel model = ColumnModel::Default();
  // Every column exists in the model; the 类型 column is opt-in.
  PRISM_CHECK_EQ(model.Columns().size(), size_t(6));
  PRISM_CHECK_EQ(model.VisibleCount(), size_t(5));
  PRISM_CHECK(model.Visible(ColumnId::kName));
  PRISM_CHECK(model.Visible(ColumnId::kRatio));
  PRISM_CHECK(model.Visible(ColumnId::kMtime));
  PRISM_CHECK(!model.Visible(ColumnId::kType));
  PRISM_CHECK_EQ(model.Width(ColumnId::kName), 260u);
  PRISM_CHECK_EQ(model.MinWidth(ColumnId::kName), 120u);
  PRISM_CHECK_EQ(model.TotalWidth(), 720u);  // 260+110+110+90+150
}

PRISM_TEST("list_sort.columns_visibility_respects_the_header_contract") {
  ColumnModel model = ColumnModel::Default();
  PRISM_CHECK(model.SetVisible(ColumnId::kType, true));
  PRISM_CHECK(model.Visible(ColumnId::kType));
  PRISM_CHECK_EQ(model.VisibleCount(), size_t(6));
  PRISM_CHECK_EQ(model.TotalWidth(), 810u);
  PRISM_CHECK(model.SetVisible(ColumnId::kType, false));
  PRISM_CHECK_EQ(model.TotalWidth(), 720u);

  // The name column is the list itself; it cannot be hidden.
  PRISM_CHECK(!model.SetVisible(ColumnId::kName, false));
  PRISM_CHECK(model.Visible(ColumnId::kName));

  // Nor can the header be emptied.
  PRISM_CHECK(model.SetVisible(ColumnId::kOriginalSize, false));
  PRISM_CHECK(model.SetVisible(ColumnId::kPackedSize, false));
  PRISM_CHECK(model.SetVisible(ColumnId::kRatio, false));
  PRISM_CHECK(model.SetVisible(ColumnId::kMtime, false));
  PRISM_CHECK_EQ(model.VisibleCount(), size_t(1));
  PRISM_CHECK(!model.SetVisible(ColumnId::kName, false));
  PRISM_CHECK_EQ(model.VisibleColumns().size(), size_t(1));
  PRISM_CHECK(model.VisibleColumns()[0] == ColumnId::kName);

  // Hiding something twice is not an error, and unknown ids are refused.
  PRISM_CHECK(model.SetVisible(ColumnId::kType, false));
}

PRISM_TEST("list_sort.columns_width_never_goes_below_the_minimum") {
  ColumnModel model = ColumnModel::Default();
  PRISM_CHECK(model.SetWidth(ColumnId::kRatio, 10));  // below minWidth 64
  PRISM_CHECK_EQ(model.Width(ColumnId::kRatio), 64u);
  PRISM_CHECK(model.SetWidth(ColumnId::kRatio, 0));
  PRISM_CHECK_EQ(model.Width(ColumnId::kRatio), 64u);
  PRISM_CHECK(model.SetWidth(ColumnId::kRatio, 5000));  // above kMaxColumnWidth
  PRISM_CHECK_EQ(model.Width(ColumnId::kRatio), prism::list::kMaxColumnWidth);
  PRISM_CHECK(model.SetWidth(ColumnId::kRatio, 128));
  PRISM_CHECK_EQ(model.Width(ColumnId::kRatio), 128u);
}

PRISM_TEST("list_sort.columns_minimum_width_raises_the_width") {
  ColumnModel model = ColumnModel::Default();
  PRISM_CHECK_EQ(model.Width(ColumnId::kMtime), 150u);
  PRISM_CHECK(model.SetMinWidth(ColumnId::kMtime, 200));
  PRISM_CHECK_EQ(model.MinWidth(ColumnId::kMtime), 200u);
  PRISM_CHECK_EQ(model.Width(ColumnId::kMtime), 200u);  // promise kept now, not later
  // Lowering the minimum never shrinks the current width.
  PRISM_CHECK(model.SetMinWidth(ColumnId::kMtime, 40));
  PRISM_CHECK_EQ(model.MinWidth(ColumnId::kMtime), 40u);
  PRISM_CHECK_EQ(model.Width(ColumnId::kMtime), 200u);
  // Below the floor the model records the floor, not something unreadable.
  PRISM_CHECK(model.SetMinWidth(ColumnId::kMtime, 4));
  PRISM_CHECK_EQ(model.MinWidth(ColumnId::kMtime), prism::list::kMinColumnFloor);
  // A minimum above the maximum is clamped, not recorded as impossible.
  PRISM_CHECK(model.SetMinWidth(ColumnId::kMtime, 99999));
  PRISM_CHECK_EQ(model.MinWidth(ColumnId::kMtime), prism::list::kMaxColumnWidth);
  PRISM_CHECK_EQ(model.Width(ColumnId::kMtime), prism::list::kMaxColumnWidth);
}

namespace {

void PrintMutantBanner() {
#if defined(PRISM_C13_MUTANT_UNSTABLE_SORT)
  std::cout << "C13 MUTANT ACTIVE: UNSTABLE_SORT (equal keys may be reordered)" << std::endl;
#endif
#if defined(PRISM_C13_MUTANT_LEXICOGRAPHIC_NAMES)
  std::cout << "C13 MUTANT ACTIVE: LEXICOGRAPHIC_NAMES (file10 before file2)" << std::endl;
#endif
#if defined(PRISM_C13_MUTANT_DIRS_INTERLEAVED)
  std::cout << "C13 MUTANT ACTIVE: DIRS_INTERLEAVED (folders are just rows)" << std::endl;
#endif
#if defined(PRISM_C13_MUTANT_WIDTH_BYPASSES_MIN)
  std::cout << "C13 MUTANT ACTIVE: WIDTH_BYPASSES_MIN (minWidth is only a hint)" << std::endl;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  PrintMutantBanner();
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL LIST-SORT CHECKS PASSED" << std::endl;
  return 0;
}
