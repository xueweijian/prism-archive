// Card C93 -- the breadcrumb MODEL and the tree-click MODEL, on every lane
// (PrismCore only).
//
// WHAT IS PROVEN HERE, AND WHY AT THIS LAYER
//   The window maps pixels to decisions; the decisions are PrismCore's. Three
//   facts are cheap to pin without a desktop, so a Linux lane also proves them:
//     * a crumb's hit span is EXACTLY the measured text ([left, right), half
//       open), separators and gaps answer kNoLevel, and a crumb clipped out of
//       the island's text box answers nothing;
//     * GoToLevel(kRootLevel) returns to the EMPTY path -- the archive root --
//       and the row count returns to the root's;
//     * a click on the crumb the user is already on is Unchanged and does NOT
//       bump NavigationCount (assertion 3's model half: "no navigation event"
//       is a counter, not an impression);
//     * a tree row click plans a navigation, and the ROOT row -- whose folder
//       is the empty path, the very value that used to mean "do nothing" --
//       plans "navigate to the root".
//
// The Windows suite (Tests/win/test_c93_navigation.cpp) drives the same code
// through the real window class and the real mouse path.
//
// NEGATIVE CONTROLS (D3): the mutants live behind PRISM_C93_MUTANT_* in
// ArchiveTreeView.hpp / SidebarModel.hpp and are compiled into SEPARATE test
// executables by Tests/suites/c93-navigation.cmake. This file is the clean arm.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveTreeView.hpp"
#include "SidebarModel.hpp"
#include "mini_test.hpp"

namespace {

using prism::ArchiveItem;
using prism::tree::BreadcrumbLayout;
using prism::tree::HitBreadcrumb;
using prism::tree::kNoLevel;
using prism::tree::kRootLevel;
using prism::tree::LayoutBreadcrumb;
using prism::tree::LayoutCenteredBreadcrumb;
using prism::tree::Navigator;
using prism::tree::NavAction;
using prism::tree::Segment;

// The catalog the field report described: a two-level folder chain plus files at
// two depths, so "the row count changed" can never be explained by "there was
// only one row anyway".
std::vector<ArchiveItem> Catalog() {
  std::vector<ArchiveItem> items;
  auto add = [&items](std::uint32_t index, const char* path, bool isDir) {
    ArchiveItem item;
    item.index = index;
    item.path = path;
    item.isDir = isDir;
    item.size = 10 * (index + 1);
    item.packSize = 5 * (index + 1);
    items.push_back(std::move(item));
  };
  add(0, "docs", true);
  add(1, "docs/sub", true);
  add(2, "docs/sub/a.txt", false);
  add(3, "docs/b.txt", false);
  add(4, "root.txt", false);
  return items;
}

// Deterministic pixel widths, keyed by the EXACT strings the layout measures.
// The numbers are deliberately non-uniform: a hit test that used the segment
// index or a fixed cell width would disagree with every one of them.
int FakeWidth(std::string_view text) {
  if (text == "pack.7z") return 70;
  if (text == "docs") return 32;
  if (text == "sub") return 24;
  if (text == "\x20\xE2\x80\xBA\x20") return 14;  // " > " with U+203A
  return static_cast<int>(text.size()) * 8;
}

std::vector<std::string> PathsOf(const std::vector<prism::tree::Entry>& entries) {
  std::vector<std::string> out;
  out.reserve(entries.size());
  for (const prism::tree::Entry& entry : entries) out.push_back(entry.path);
  return out;
}

}  // namespace

PRISM_TEST("c93.crumb_span_is_the_painted_text") {
  std::vector<ArchiveItem> catalog = Catalog();
  Navigator nav(&catalog);
  PRISM_CHECK_EQ(nav.GoTo("docs/sub").action, NavAction::FolderChanged);

  const std::vector<Segment> segments = nav.Segments("pack.7z");
  PRISM_CHECK_EQ(segments.size(), static_cast<std::size_t>(5));
  const BreadcrumbLayout layout = LayoutBreadcrumb(segments, 100, &FakeWidth);

  // Root / separator / "docs" / separator / "sub", laid left to right at
  // exactly the measured widths. Half-open: [left, right).
  struct Cell {
    int left;
    int right;
    int level;
  };
  const Cell expected[] = {
      {100, 170, kRootLevel},  // "pack.7z"  70 px -- the ROOT crumb
      {170, 184, kNoLevel},    // " > "      14 px -- decorative
      {184, 216, 0},           // "docs"     32 px
      {216, 230, kNoLevel},    // " > "
      {230, 254, 1},           // "sub"      24 px
  };
  for (std::size_t i = 0; i < layout.spans.size() && i < 5; ++i) {
    PRISM_CHECK_EQ(layout.spans[i].left, expected[i].left);
    PRISM_CHECK_EQ(layout.spans[i].right, expected[i].right);
    PRISM_CHECK_EQ(layout.spans[i].level, expected[i].level);
  }
  PRISM_CHECK_EQ(layout.total, 154);  // 70+14+32+14+24: no hidden padding

  // The boundary rule, one pixel on each side of every edge. A "clickable bar"
  // (the defect) would answer a level at 170 and at 254; a text-width hit test
  // answers kNoLevel there.
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 100), kRootLevel);  // left edge inclusive
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 169), kRootLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 170), kNoLevel);    // separator
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 183), kNoLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 184), 0);           // "docs" left edge
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 215), 0);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 216), kNoLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 229), kNoLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 230), 1);           // "sub" left edge
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 253), 1);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 254), kNoLevel);    // right edge exclusive
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 99), kNoLevel);     // before the run
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 1000), kNoLevel);   // past the run
}

PRISM_TEST("c93.clipped_crumb_is_not_clickable") {
  std::vector<ArchiveItem> catalog = Catalog();
  Navigator nav(&catalog);
  nav.GoTo("docs/sub");
  const std::vector<Segment> segments = nav.Segments("pack.7z");

  // A 100 px text box is narrower than the 154 px run: the leading root crumb
  // and part of the "docs" crumb are visible, "sub" is entirely outside.
  const BreadcrumbLayout layout = LayoutCenteredBreadcrumb(segments, 200, 300, &FakeWidth);
  PRISM_CHECK_EQ(layout.origin, 200);  // clamped: never starts left of the box
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 205), kRootLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 285), 0);       // "docs" starts at 284
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 299), 0);       // clipped at the box edge
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 300), kNoLevel);
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 320), kNoLevel);  // "sub" is off-box
  // The pixel just left of the box must not hit either: the pill, not the text.
  PRISM_CHECK_EQ(HitBreadcrumb(layout, 199), kNoLevel);
}

PRISM_TEST("c93.clicking_root_crumb_restores_the_root_rows") {
  std::vector<ArchiveItem> catalog = Catalog();
  Navigator nav(&catalog);
  const int rootRows = nav.Count();
  PRISM_CHECK_EQ(rootRows, 2);  // docs/ + root.txt
  PRISM_CHECK_EQ(nav.Folder(), std::string());
  PRISM_CHECK_EQ(nav.NavigationCount(), 0);

  PRISM_CHECK_EQ(nav.GoTo("docs/sub").action, NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string("docs/sub"));
  PRISM_CHECK_EQ(nav.Count(), 1);  // docs/sub/a.txt
  PRISM_CHECK_EQ(nav.NavigationCount(), 1);

  // Segment 0 of the bar is the ROOT crumb (kRootLevel): one click, archive
  // root, and the row list is the root's again -- both halves asserted.
  PRISM_CHECK_EQ(nav.GoToLevel(kRootLevel).action, NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string());
  PRISM_CHECK_EQ(nav.Count(), rootRows);
  if (PRISM_CHECK_EQ(nav.Current().entries.size(), static_cast<std::size_t>(2))) {
    PRISM_CHECK_EQ(nav.Current().entries[0].path, std::string("docs"));
    PRISM_CHECK_EQ(nav.Current().entries[1].path, std::string("root.txt"));
  }
  PRISM_CHECK_EQ(nav.NavigationCount(), 2);

  // Clicking the root crumb AT the root changes nothing (the empty path is the
  // root, so this is the "same target" case, not a second navigation).
  PRISM_CHECK_EQ(nav.GoToLevel(kRootLevel).action, NavAction::None);
  PRISM_CHECK_EQ(nav.NavigationCount(), 2);
}

PRISM_TEST("c93.clicking_the_current_crumb_is_not_a_navigation") {
  std::vector<ArchiveItem> catalog = Catalog();
  Navigator nav(&catalog);
  nav.GoTo("docs");
  PRISM_CHECK_EQ(nav.Folder(), std::string("docs"));
  const int before = nav.NavigationCount();
  const std::vector<std::string> rows = PathsOf(nav.Current().entries);
  const std::size_t rowCount = rows.size();

  // "docs" is crumb level 0 while we are in it: pressing it must be Unchanged
  // and must NOT bump the counter (which would push history, reset the viewport
  // and flash the list on every re-press).
  const auto result = nav.GoToLevel(0);
  PRISM_CHECK_EQ(result.action, NavAction::None);
  PRISM_CHECK_EQ(nav.Folder(), std::string("docs"));
  PRISM_CHECK_EQ(nav.NavigationCount(), before);
  const std::vector<std::string> after = PathsOf(nav.Current().entries);
  if (PRISM_CHECK_EQ(after.size(), rowCount)) {
    for (std::size_t i = 0; i < after.size(); ++i) {
      PRISM_CHECK_EQ(after[i], rows[i]);
    }
  }

  // Same claim through the path door the window uses for the tree.
  PRISM_CHECK_EQ(nav.GoTo("docs").action, NavAction::None);
  PRISM_CHECK_EQ(nav.NavigationCount(), before);
}

PRISM_TEST("c93.tree_root_row_plans_a_navigation_to_the_root") {
  std::vector<ArchiveItem> catalog = Catalog();
  prism::sidebar::Model sidebar(&catalog);
  sidebar.SetArchiveName("pack.7z");
  const std::vector<prism::sidebar::TreeRow>& rows = sidebar.Tree();
  if (!PRISM_CHECK(rows.size() >= 3)) return;
  PRISM_CHECK(rows[0].isRoot);
  PRISM_CHECK_EQ(rows[0].path, std::string());

  // The ROOT row: navigate=true with the EMPTY folder. A bool-free design (a
  // TreeRow* or a "non-empty path" check) is exactly what shipped the dead row,
  // because "" is both "the root" and the sentinel for "nothing".
  const prism::sidebar::TreeClickTarget root = prism::sidebar::PlanTreeClick(rows, 0);
  PRISM_CHECK(root.navigate);
  PRISM_CHECK_EQ(root.folder, std::string());

  int docsRow = -1;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == "docs") docsRow = static_cast<int>(i);
  }
  if (!PRISM_CHECK(docsRow > 0)) return;
  const prism::sidebar::TreeClickTarget docs =
      prism::sidebar::PlanTreeClick(rows, docsRow);
  PRISM_CHECK(docs.navigate);
  PRISM_CHECK_EQ(docs.folder, std::string("docs"));

  // Out of range: no plan, no crash.
  PRISM_CHECK(!prism::sidebar::PlanTreeClick(rows, -1).navigate);
  PRISM_CHECK(!prism::sidebar::PlanTreeClick(rows, static_cast<int>(rows.size())).navigate);
}

int main() {
  const int failures = prism::test::RunAll(0, nullptr);
  if (failures != 0) return 1;
  std::puts("ALL C93 NAVIGATION CHECKS PASSED");
  return 0;
}
