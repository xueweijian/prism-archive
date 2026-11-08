// PrismArchive -- PrismCore
// WindowLayout: the ONE geometry authority of the main window (DESIGN.md §2).
//
// Why this file exists: the two screenshots (bandizip.png / prism.png) showed
// islands painted at absolute coordinates that no other component knew about,
// so the grid slid under the sidebar and two cards overlapped. Every rectangle
// the main window paints now comes from here, and the rules are pure integer
// math -- a Linux CI job can assert them without a window station:
//
//   * bands:   title 32 | TAB BAR 32 (C60: only while an archive is open) |
//              action island 56 | sidebar 220 (0 when collapsed)
//              | content padding 16 | status bar 28
//   * siblings do not intersect (tolerance 0): the bands tile the window and
//     the content starts exactly where the sidebar ends;
//   * collapsed sidebar: sidebar rect is EMPTY, the content grows to the full
//     window width, so nothing is left behind at x = 220 ("no ghost");
//   * grid columns: floor((contentInnerWidth + gap) / (minTile + gap)), >= 1
//     (DESIGN.md §2); the same rule at 800/1000/1200/1600 px wide.
//
// P0-2 (DESIGN.md §3): a grid cell may only be a TILE when it can actually
// produce a thumbnail. The predicate is explicit -- directory, or a
// thumbnail-capable extension (Image/Media) WITH a decoded thumbnail in the
// ThumbStore. Everything else (txt/pdf/exe/... and images whose thumbnail has
// not landed) is a 32 px ROW (icon + name + size), never a skeleton card.
//
// B1: (std::min)/(std::max) are parenthesised -- a consumer TU may include
//     windows.h without NOMINMAX.
// B2: this header includes what it uses (<cstdint>, <algorithm>, <vector>).
#pragma once

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ListColumns.hpp"
#include "Preview.hpp"

namespace prism::layout {

// ------------------------------------------------------------- mutations (D3)
// Each switch turns exactly one rule off; the suite must go red in that build.
//   PRISM_V1_MUTANT_NO_THUMB_FALLBACK -> every non-folder item becomes a tile
//                                        (the old "skeleton card" behaviour).
//   PRISM_V1_MUTANT_IGNORE_SIDEBAR    -> the sidebar is painted but its width
//                                        is not reserved, so the content
//                                        overlaps the rail.
#if defined(PRISM_V1_MUTANT_NO_THUMB_FALLBACK)
inline constexpr bool kThumbFallbackEnabled = false;
#else
inline constexpr bool kThumbFallbackEnabled = true;
#endif
#if defined(PRISM_V1_MUTANT_IGNORE_SIDEBAR)
inline constexpr bool kSidebarReservesWidth = false;
#else
inline constexpr bool kSidebarReservesWidth = true;
#endif

// V4 negative controls (CONTRACTS D3/D27). Both are compile-time switches, so
// the same suite source proves the rule rather than a copy of it:
//   PRISM_V4_MUTANT_CELL_ROW        -> a tile-less item keeps its 220 px CELL
//                                      (V1's compromise: "row" inside the grid),
//                                      which is exactly the pill the screenshot
//                                      showed. The flow assertions go red.
//   PRISM_V4_MUTANT_GRID_NO_SIDEBAR -> the rail is list-mode chrome again, so a
//                                      grid frame reserves 0 px. The "the rail
//                                      is chrome, not a list column" assertions
//                                      go red.
#if defined(PRISM_V4_MUTANT_CELL_ROW)
inline constexpr bool kRowsSpanContent = false;
#else
inline constexpr bool kRowsSpanContent = true;
#endif
#if defined(PRISM_V4_MUTANT_GRID_NO_SIDEBAR)
inline constexpr bool kRailInGridMode = false;
#else
inline constexpr bool kRailInGridMode = true;
#endif
// C115 negative control: the breadcrumb pill ignores the action island and keeps
// V1's full 560 px at every width -- the pre-card behaviour, i.e. the 144 px of
// toolbar on top of the pill at 1028. See NavPillRect.
#if defined(PRISM_C115_MUTANT_IGNORE_NARROW)
inline constexpr bool kNavPillYieldsToBand = false;
#else
inline constexpr bool kNavPillYieldsToBand = true;
#endif

// ==================================================== C123: THE TOP SEAM
// THE DEFECT (measured at 125 %, DIP = device / 1.25; proposal 14.4/17.3):
//
//   | quantity                         | Bandizip | before C123 |
//   |----------------------------------|----------|-------------|
//   | centre of the OPEN button        | (32, 83) | (643, 138)  |
//   | toolbar's top edge               | 50       | 117         |
//   | toolbar's left edge              | flush    | 589 (a 964 wide floating island) |
//   | breadcrumb text                  | --       | centred: container 28..718, text 234..495 -> 62.2 % dead air |
//
// THE CAUSE IS STRUCTURAL, not artistic: the breadcrumb pill lives INSIDE the
// 56 px action band (NavPillRect), so the eight buttons must start to its
// right -- actionbar::Compute anchors the first button at pill.right + 12.
// A path display therefore owns the first 560 px of the row that the app's
// primary affordance ("Open") needs. Two more bands (the 32 px tab strip and
// the pill's own 56 px) pushed the row down to y = 117.
//
// THE FIX (three structural moves, no new panel, no new toolbar -- proposal
// 16.4's iron rule):
//   1. TABS ON DEMAND (proposal 17.2): TabBarShown() -- the 32 px strip exists
//      only while a SECOND archive is open. One archive is the migration
//      user's first state and the strip there is pure cost: 0 px of chrome.
//   2. THE CRUMB LEAVES THE BAND: it becomes a 28 px PATH BAR on its own row
//      under the toolbar (kBreadcrumbBarHeight), left-aligned at
//      contentBand.left + kContentPadding -- a path, not a second button group.
//   3. THE TOOLBAR GOES FLUSH AND FULL-WIDTH: its left and right edges ARE the
//      content band's (ToolbarBandRect), so the first button's centre lands at
//      52 rather than 643 and the row reads as the same solid strip Bandizip
//      has.
//
// WHY compute a SEPARATE layout function (ComputeC123) instead of moving
// Compute() itself: Compute() is the pinned geometry of twenty pre-C123
// suites (v1/v2/v4, C60, C66, C78, C92, C100, C115, C120...). Their scenes are
// floating-island compositions and stay byte-identical; the WINDOW migrates.
// The one shared truth is still this header: Layout::chrome says which
// composition a rectangle belongs to, and both the renderer and the action-bar
// model branch on it -- never on a copy of the numbers.
//
// C115 IS NOT TOUCHED. NavPillRect still yields width to the action island's
// floor, and the legacy composition still uses it. What changes is who ASKS:
// a C123 layout publishes no pill (Layout::crumbBar instead), so the narrow
// window can no longer put one on top of the toolbar at all. The C115 rule
// remains the law for a composition that still has a pill in the band.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27):
//   PRISM_C123_MUTANT_FLOATING_TOOLBAR -> ToolbarBandRect returns the PRE-CARD
//   floating island (pill.right + kNavActionGap .. band.right - kContentPadding)
//   instead of the band, i.e. the 589-start / 964-wide rectangle the audit
//   photographed. T2 (the Open button's centre) and T3 (flush edges) MUST go
//   red in that build; the arm prints "C123 MUTATION CAUGHT: floating_toolbar"
//   from the FIRST failing assertion and leaves 0, and the CTest entry decides
//   with PASS_REGULAR_EXPRESSION -- never WILL_FAIL.
#if defined(PRISM_C123_MUTANT_FLOATING_TOOLBAR)
inline constexpr bool kToolbarSpansBand = false;
#else
inline constexpr bool kToolbarSpansBand = true;
#endif

// The runtime proof the arm really compiled in (D3): a macro that stopped
// matching would leave the mutant green while its CTest entry stayed red.
inline constexpr std::string_view ActiveC123Mutation() {
  return kToolbarSpansBand ? std::string_view{} : std::string_view{"floating_toolbar"};
}


// The runtime proof each mutant is REALLY armed (D3: a macro that stopped
// matching would leave the mutant green and its WILL_FAIL entry red forever).
inline constexpr std::string_view ActiveMutation() {
  if (!kRowsSpanContent) return "cell-row";
  if (!kRailInGridMode) return "grid-no-sidebar";
  if (!kNavPillYieldsToBand) return "ignore-narrow";
  return {};
}

// ---------------------------------------------------------------- tokens
inline constexpr int kTitleBarHeight = 32;
inline constexpr int kActionIslandHeight = 56;
inline constexpr int kTopBandHeight = kTitleBarHeight + kActionIslandHeight;  // 88
// C60 (DESIGN.md §2.1): the tab strip sits BETWEEN the title and the action
// island, is 32 px tall and -- this is the part that is easy to get wrong --
// is HIDDEN, not empty, when no archive is open: with no tabs the rect is
// Empty() and the action island moves back up to y = 32, so the 32 px is not
// reserved for a strip that would paint nothing. A band that keeps its space
// while showing nothing is dead air above every archive-less window.
inline constexpr int kTabBarHeight = 32;
inline constexpr int kSidebarWidth = 220;
inline constexpr int kContentPadding = 16;
inline constexpr int kStatusBarHeight = 28;
inline constexpr int kIslandGap = 12;
inline constexpr int kListHeaderHeight = 32;
// C122: 26 DIP, not 36. The design proposal's §13.2 measured Bandizip at 19.2
// logical px and asked for 26: 35 % looser than Bandizip, 19 % denser than
// Win11 Explorer, and -- the number the user actually feels -- 21.6 visible
// rows instead of 15.6 in the same 1007x742 window.
//
// The whole list stack (ListRowRect / VisibleListRows / ListRowAt and the
// window's scroll pitch) reads ListRowHeightFor(layout), so one number moves
// paint and hit test together. 26 is the REGULAR tier; a window shorter than
// kListCompactWindowHeight switches to the 22 DIP compact tier (design proposal
// §17.1), which is a property of the WINDOW, not of a caller's mood.
inline constexpr int kListRowHeight = 26;
inline constexpr int kListRowHeightCompact = 22;
inline constexpr int kListCompactWindowHeight = 700;  // DIP

// C122 §2: the four columns. Name flexes; the two size columns are 120 DIP and
// the type column 100 DIP (design proposal §5.3). kListRightPadding is the
// right INNER padding: before this card the list's right inset was 0, so the
// size cell ended exactly on the clip edge and "2.0 MB" shipped as "2.0 M"
// (design proposal §12.6 defect 1). The last column's right edge -- and with
// it every numeric baseline in the row -- is the content edge minus this.
inline constexpr int kListColPackedWidth = 120;
inline constexpr int kListColOriginalWidth = 120;
inline constexpr int kListColTypeWidth = 100;
inline constexpr int kListColumnGutter = 16;
inline constexpr int kListRightPadding = 20;
inline constexpr int kListIndent = 20;
inline constexpr int kMinTileWidth = 120;  // DESIGN.md §2: minimum column width
inline constexpr int kMaxTileWidth = 192;
inline constexpr int kTileLabelHeight = 30;  // name plate under a photo tile
inline constexpr int kMinCellHeight = 220;
inline constexpr int kFallbackRowHeight = 32;  // P0-2: the row a tile-less item falls back to
inline constexpr int kNavPillHeight = 44;
inline constexpr int kNavPillMaxWidth = 560;
inline constexpr int kNavTextInset = 24;
// C123: the breadcrumb is a PATH BAR, not a pill (proposal 5.2/17.3). 28 is the
// type scale's own line height (4.2: "面包屑 13 / 400 / 28"), so the row is
// exactly one line of crumb text plus the 2 px current-segment rule (16.3's
// "一线") -- no pill, no 44 px island.
inline constexpr int kBreadcrumbBarHeight = 28;
// The path starts at the content band's left edge + 16 (proposal 17.3: "左对齐
// 到内容区 +16（不是居中）"), which is the same 16 the content's own padding
// uses -- one number, so the crumb's first glyph lines up with the list header
// below it.
inline constexpr int kBreadcrumbInset = kContentPadding;
// The 2 px rule under the crumb you are standing on. It is the "seam" token of
// 16.3 applied to a fifth semantic position (the other four are the selected
// row's left bar, the tab underline, the focus ring and the breadcrumb's own
// current segment).
inline constexpr int kBreadcrumbUnderlineHeight = 2;
// C123 (proposal 17.2): the strip appears only when a SECOND archive is open.
// One archive -- the state every migrating user starts in -- keeps 0 px.
inline constexpr int kTabBarMinArchives = 2;
// ------------------------------------------------- C115: the band's two tenants
// The 56 px band holds the breadcrumb pill (left) and the action island (right).
// They cannot both have their full width: 16 (inset) + 560 (pill) + 12 (gap) +
// 580 (island) + 16 (inset) = 1184 px. Below that the band must give something
// up, and BOTH sides have to agree about what -- the defect this constant
// exists to end is that two functions each answered "the pill is 560 px wide"
// and "the pill is 404 px wide" for the same 1028 px window (C112 measured the
// 144 px of toolbar ink that landed inside the authority's rectangle).
//
// WHAT GIVES: the pill. A path display truncates gracefully (it is a string
// with an ellipsis); a toolbar that silently loses 查看/搜索 does not, and C66
// already decided that a band which cannot show all eight shows none. So the
// pill yields width down to nothing before a single button is dropped.
//
// WHY NOT A FLOOR (kNavPillMinWidth): at the documented minimum window
// (kMinWindowWidth = 640) a 160 px floor would leave 640 - 16 - 160 - 12 - 16 =
// 436 px of band for a 580 px toolbar, i.e. the toolbar would have to disappear
// at the smallest window the product supports. That trades a readable crumb for
// the app's primary affordance at the exact size where the user has the least
// room. Measured at 640 the pill is a 16 px nub with all eight buttons intact;
// that is the existing (C78) contract and this card does not spend it.
//
// kActionIslandMinWidth is the island's own floor: 2 * kIslandPad (16) +
// kActionCount * kButtonMinWidth (8 * 64) + TotalGaps (52). The number lives
// here because it is CHROME geometry (how the band is split), while the table
// that makes it true lives in ActionBarModel -- which static_asserts the two
// against each other, so a ninth action or a new group cannot silently make the
// pill reserve the wrong width.
inline constexpr int kActionIslandMinWidth = 580;
// The air between the pill's right edge and the island's left edge. It lives
// here because the SPLIT is decided here (NavPillRect); actionbar::kNavGap is an
// alias so the two cannot drift apart.
inline constexpr int kNavActionGap = 12;
inline constexpr int kProgressCapsuleWidth = 260;
inline constexpr int kProgressCapsuleHeight = 46;
inline constexpr int kQuickLookWidth = 380;
inline constexpr int kQuickLookHeight = 500;
// V4: a fallback row is a BAND across the content, not a cell inside the grid.
// A 32 px band is what makes 19 rows fit in 800 px; the cell-sized row V1
// shipped was a 220 px pill with dead air to its right -- the exact defect the
// v2 screenshot showed.
// C109: the GRID's fallback band stays 32. Only the file LIST got the 36 px
// breathing room; aliasing this to kListRowHeight would silently reflow every
// tile-less grid row and invalidate V4's band assertions for no design reason.
inline constexpr int kFlowRowHeight = 32;
inline constexpr int kFlowTileGap = kIslandGap;  // alias: the grid's gap
// The hit test's row pitch. Off by one in the CONTROL build only: a click on
// the last pixel of row N-1 must then land on row N (the neighbour), which is
// what the polish suite's boundary assertions are written to catch.
#if defined(PRISM_V4_MUTANT_HIT_PITCH)
inline constexpr int kFlowHitPitch = kFlowRowHeight + 1;
#else
inline constexpr int kFlowHitPitch = kFlowRowHeight;
#endif

// ------------------------------------------------------------------ Rect
struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  constexpr int Width() const { return right - left; }
  constexpr int Height() const { return bottom - top; }
  constexpr bool Empty() const { return Width() <= 0 || Height() <= 0; }

  // Tolerance 0 on purpose: a one-pixel overlap is still an overlap.
  constexpr bool Contains(const Rect& other) const {
    return other.left >= left && other.top >= top && other.right <= right &&
           other.bottom <= bottom;
  }
  constexpr bool Intersects(const Rect& other) const {
    return left < other.right && other.left < right && top < other.bottom &&
           other.top < bottom;
  }
  bool operator==(const Rect&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const Rect& r) {
  return os << "[" << r.left << "," << r.top << " " << r.right << "x" << r.bottom << "]";
}

// Shrink by pad on every side; never produces a negative size (a 3-px window
// is a degenerate layout, not an inverted rectangle).
inline Rect Inset(const Rect& rect, int pad) {
  const int halfW = (std::min)(pad, (std::max)(0, rect.Width() / 2));
  const int halfH = (std::min)(pad, (std::max)(0, rect.Height() / 2));
  return Rect{rect.left + halfW, rect.top + halfH, rect.right - halfW, rect.bottom - halfH};
}

// ---------------------------------------------------------------- layout
enum class CellKind : std::uint8_t { Tile, Row };

inline std::ostream& operator<<(std::ostream& os, CellKind kind) {
  switch (kind) {
    case CellKind::Tile: return os << "Tile";
    case CellKind::Row: return os << "Row";
  }
  return os << "?";
}

// C123: WHICH composition a Layout describes. The rectangles mean the same
// thing in both (they come from the same function), but the CHROME differs:
//   V1   -- the breadcrumb pill lives in the action band (NavPillRect), the
//           toolbar is the eight buttons to its right. Every pre-C123 scene.
//   C123 -- no pill: the toolbar spans the band, and the crumb is a 28 px path
//           bar under it (crumbBar). The window composes THIS one.
// Consumers (the renderer, actionbar::Compute, GalleryWindow) branch on this
// field instead of keeping a second copy of the numbers.
enum class Chrome : std::uint8_t { V1, C123 };

inline std::ostream& operator<<(std::ostream& os, Chrome chrome) {
  switch (chrome) {
    case Chrome::V1: return os << "V1";
    case Chrome::C123: return os << "C123";
  }
  return os << "?";
}

struct Layout {
  int windowW = 0;
  int windowH = 0;
  bool sidebarVisible = false;
  bool listMode = false;
  Chrome chrome = Chrome::V1;

  // Siblings: these six tile the window and are pairwise disjoint.
  Rect titleBar;
  // C60: empty (and tabBarVisible false) when no archive is open.
  // C123: also empty until a SECOND archive is open (TabBarShown).
  Rect tabBar;
  bool tabBarVisible = false;
  Rect actionIsland;
  // C123: the toolbar proper. It is the actionIsland rectangle in both
  // compositions; the separate field exists because "the band the toolbar is
  // painted in" is what T3 measures, and a caller should not have to know that
  // the band predates the toolbar.
  Rect toolbar;
  // C123: the 28 px breadcrumb path bar (empty in V1, where the crumb is the
  // pill inside actionIsland). It sits BETWEEN the toolbar and the content.
  Rect crumbBar;
  Rect sidebar;      // empty when collapsed
  Rect content;      // starts at sidebar.right
  // C123: the FULL-WIDTH band that holds the rail and the list. It is the
  // window's 内容区: the toolbar and the crumb bar are measured against ITS left
  // and right edges (T3/T4), and it is what "通栏" means. In V1 it equals
  // content (the rail reserves width inside it), so a caller may read it
  // unconditionally.
  Rect contentBand;
  Rect statusBar;
  // Child of content (inset by kContentPadding).
  Rect contentInner;

  // Grid metrics (grid mode; in list mode columns == 1 and the tile metrics
  // are still filled in so callers can branch without a divide-by-zero).
  int columns = 1;
  int tileWidth = 0;
  int cellHeight = 0;
  int gap = kIslandGap;
};

// DESIGN.md §2, verbatim: floor((contentWidth + gap) / (minTile + gap)), >= 1.
inline int ColumnsFor(int contentInnerWidth) {
  if (contentInnerWidth <= 0) return 1;
  const int columns = (contentInnerWidth + kIslandGap) / (kMinTileWidth + kIslandGap);
  return (std::max)(1, columns);
}

// DESIGN.md §2 lists the rail as a BAND ("侧栏 220 | 可折叠"), not as list-mode
// chrome: it is constant in both views and only an explicit collapse (or a
// window too narrow to hold it) removes it. This predicate is the ONE place
// that decides, so the window's paint, its hit test and this header cannot
// disagree about whether the grid has a rail.
inline bool RailVisible(bool userWantsRail, bool listMode) {
  if (!userWantsRail) return false;
  return kRailInGridMode ? true : listMode;
}

// C123 (proposal 17.2): the tab strip is CHROME THAT MUST EARN ITS 32 px.
// One archive is the migration user's first and most common state; a strip that
// shows a single tab there costs 32 px and buys nothing ("1 个包时 0 像素
// chrome"). Two is the first count at which the strip answers a question the
// window cannot ("which of these am I looking at"). The predicate is a function
// -- not "`size() >= 2`` spelled at the call site -- so the paint, the hit test
// and the layout cannot disagree about when the band exists.
inline constexpr bool TabBarShown(int openArchives) {
  return openArchives >= kTabBarMinArchives;
}

namespace detail {

// ONE implementation for both compositions. 'c123' selects the C123 chrome
// (the crumb's own 28 px row, no pill); everything else -- the bands above the
// content, the rail's reservation, the grid metrics -- is the same arithmetic,
// so the two layouts can never drift apart on the parts that did not change.
inline Layout ComputeChrome(int windowW, int windowH, bool sidebarVisible, bool listMode,
                            bool tabBarVisible, bool c123, bool breadcrumbVisible) {
  Layout l;
  l.windowW = (std::max)(0, windowW);
  l.windowH = (std::max)(0, windowH);
  l.sidebarVisible = sidebarVisible;
  l.listMode = listMode;
  l.chrome = c123 ? Chrome::C123 : Chrome::V1;
  l.tabBarVisible = tabBarVisible;

  const int w = l.windowW;
  const int h = l.windowH;

  l.titleBar = Rect{0, 0, w, (std::min)(kTitleBarHeight, h)};
  // C60: the strip is inserted between the two bands that already existed, so
  // the action island (and everything under it) slides down by exactly 32 px
  // while tabs are open and returns to y = 32 when the last one closes. The
  // default (false) keeps every pre-C60 caller -- and every pre-C60 assertion
  // about title/action/content -- byte-for-byte identical.
  const int tabTop = l.titleBar.bottom;
  l.tabBar = tabBarVisible
                 ? Rect{0, tabTop, w, (std::min)(tabTop + kTabBarHeight, h)}
                 : Rect{};
  const int topBand = tabBarVisible ? kTopBandHeight + kTabBarHeight : kTopBandHeight;
  // The island starts where the band ABOVE it ends: the strip when it exists,
  // the title bar when it does not. (Reading l.tabBar.bottom unconditionally
  // made the hidden strip's empty rect -- top/right/bottom all zero -- drag the
  // action island up to y = 0 and give it the title band's 32 px as well.)
  const int islandTop = l.tabBar.Empty() ? l.titleBar.bottom : l.tabBar.bottom;
  l.actionIsland = Rect{0, islandTop, w, (std::min)(topBand, h)};
  l.toolbar = l.actionIsland;
  // C123: the crumb's own row, between the toolbar and the content. It is
  // HIDDEN, not empty, when there is no path to show (the same rule C60 gave
  // the strip): a band that keeps its 28 px while painting nothing is dead air
  // above every archive-less window.
  const int crumbTop = l.actionIsland.bottom;
  const int crumbHeight =
      (c123 && breadcrumbVisible)
          ? (std::min)(kBreadcrumbBarHeight, (std::max)(0, h - crumbTop))
          : 0;
  l.crumbBar = crumbHeight > 0 ? Rect{0, crumbTop, w, crumbTop + crumbHeight} : Rect{};
  const int contentTop = crumbTop + crumbHeight;
  const int statusTop = (std::max)(contentTop, h - kStatusBarHeight);

  // The rail is PAINTED whenever it is visible, but it only RESERVES width when
  // the rule is armed. The mutant therefore produces two intersecting siblings
  // instead of silently moving the content.
  const bool reserve = sidebarVisible && kSidebarReservesWidth;
  const int railWidth = sidebarVisible ? (std::min)(kSidebarWidth, w) : 0;
  l.sidebar = sidebarVisible ? Rect{0, contentTop, railWidth, statusTop} : Rect{};
  l.content = Rect{reserve ? railWidth : 0, contentTop, w, statusTop};
  // In V1 the content band IS the content column: the rail is its left edge, so
  // "the window's content area" and "the list's rectangle" are the same thing.
  // C123 separates them: the band spans the window (that is what the toolbar
  // and the crumb bar align to) and the rail is a child of it.
  l.contentBand = c123 ? Rect{0, contentTop, w, statusTop} : l.content;
  l.statusBar = Rect{0, statusTop, w, h};
  l.contentInner = Inset(l.content, kContentPadding);

  l.gap = kIslandGap;
  l.columns = listMode ? 1 : ColumnsFor(l.contentInner.Width());
  const int usable = l.contentInner.Width() - (l.columns - 1) * l.gap;
  int tile = l.columns > 0 ? usable / l.columns : 0;
  tile = (std::max)(1, tile);
  l.tileWidth = (std::min)(tile, kMaxTileWidth);
  l.cellHeight = (std::max)(kMinCellHeight, l.tileWidth + kTileLabelHeight + l.gap);
  return l;
}

}  // namespace detail

// The complete geometry of the main window for one client size (V1
// composition: the breadcrumb pill shares the 56 px band with the toolbar).
inline Layout Compute(int windowW, int windowH, bool sidebarVisible, bool listMode,
                      bool tabBarVisible = false) {
  return detail::ComputeChrome(windowW, windowH, sidebarVisible, listMode, tabBarVisible,
                               /*c123=*/false, /*breadcrumbVisible=*/false);
}

// C123: the SAME window, with the top seam fixed. The toolbar is a full-width
// band, the crumb is a 28 px path bar under it, and the tab strip appears only
// from the second archive on. 'openArchives' is the TabSet's size (not a bool):
// "is the strip shown" is a rule about a COUNT (TabBarShown), and a caller that
// hands in a count cannot disagree with the rule.
inline Layout ComputeC123(int windowW, int windowH, bool sidebarVisible, bool listMode,
                          int openArchives, bool breadcrumbVisible = true) {
  return detail::ComputeChrome(windowW, windowH, sidebarVisible, listMode,
                               TabBarShown(openArchives), /*c123=*/true, breadcrumbVisible);
}

// ------------------------------------------------------------ item geometry
// One grid cell (content space + scroll). The caller clips to contentInner;
// a fully visible cell is guaranteed to sit inside it.
inline Rect TileRect(const Layout& layout, int index, int scrollPx) {
  if (index < 0 || layout.columns <= 0) return Rect{};
  const int column = index % layout.columns;
  const int row = index / layout.columns;
  const int x = layout.contentInner.left + column * (layout.tileWidth + layout.gap);
  const int y = layout.contentInner.top + row * layout.cellHeight - scrollPx;
  return Rect{x, y, x + layout.tileWidth, y + layout.cellHeight};
}

struct Cell {
  int index = -1;
  Rect rect;
};

// The cells that intersect the content viewport at scrollPx. Partially visible
// cells keep their FULL rect (the renderer clips); a fully visible cell is
// inside contentInner by construction.
inline std::vector<Cell> VisibleCells(const Layout& layout, int itemCount, int scrollPx) {
  std::vector<Cell> cells;
  if (itemCount <= 0 || layout.columns <= 0 || layout.contentInner.Empty()) return cells;
  const int firstRow = (std::max)(0, scrollPx / layout.cellHeight - 1);
  const int lastRow = (std::min)((itemCount - 1) / layout.columns,
                                 (scrollPx + layout.contentInner.Height()) /
                                         layout.cellHeight +
                                     1);
  for (int row = firstRow; row <= lastRow; ++row) {
    for (int column = 0; column < layout.columns; ++column) {
      const int index = row * layout.columns + column;
      if (index >= itemCount) break;
      const Rect rect = TileRect(layout, index, scrollPx);
      if (rect.bottom <= layout.contentInner.top || rect.top >= layout.contentInner.bottom) {
        continue;
      }
      cells.push_back(Cell{index, rect});
    }
  }
  return cells;
}

// P0-2: the 32 px row a cell falls back to. It is anchored to the TOP of the
// cell, spans the cell width and is exactly kFallbackRowHeight tall.
inline Rect FallbackRowRect(const Rect& cell) {
  return Rect{cell.left, cell.top, cell.right,
              (std::min)(cell.bottom, cell.top + kFallbackRowHeight)};
}

// The classification rule, stated once (DESIGN.md §3 P0):
//   * a directory always paints its folder tile -- it is not a loading state;
//   * a thumbnail-capable extension (Image/Media) paints a tile ONLY when the
//     ThumbStore actually holds a decoded thumbnail for the item;
//   * everything else falls back to a 32 px row.
// "Looks like a picture" is never an input.
inline bool ThumbnailCapable(prism::PreviewKind kind) {
  return kind == prism::PreviewKind::Image || kind == prism::PreviewKind::Media;
}

inline CellKind CellKindFor(bool isDir, prism::PreviewKind kind, bool thumbnailAvailable) {
  if (isDir) return CellKind::Tile;
  if (!kThumbFallbackEnabled) return CellKind::Tile;  // negative control
  if (!ThumbnailCapable(kind)) return CellKind::Row;
  if (!thumbnailAvailable) return CellKind::Row;
  return CellKind::Tile;
}

// --------------------------------------------------------------- V4: the flow
// The grid used to be a RECTANGLE GRID for every item: a text file became a
// 220 x 220 cell with a 32 px pill at its top-left, which is the "small capsule
// with a wall of empty space to its right" in the v2 screenshot. DESIGN.md §3
// ("没有缩略图的条目不得画骨架卡；直接回退成一条 32 px 的行") asks for a ROW,
// and a row is a band, not a cell.
//
// So the grid is a FLOW: items are walked in catalog order and each one is
// either
//   * a TILE -- only a directory, or an image/media item whose thumbnail the
//     ThumbStore actually holds. Tiles fill a grid row left to right, at most
//     layout.columns of them, one layout.cellHeight tall;
//   * a ROW  -- everything else. It spans the ENTIRE content band
//     (left == content.left, right == content.right) and is kFlowRowHeight tall.
// Consecutive rows are vertically contiguous (row N's bottom IS row N+1's top),
// which is what makes the same 32 px pitch the list view uses.
//
// Both facts are asserted by Tests/win/test_visual_polish.cpp; the D3 control
// PRISM_V4_MUTANT_CELL_ROW puts the rows back inside their cells and the same
// assertions go red.
struct FlowItem {
  bool isDir = false;
  prism::PreviewKind kind = prism::PreviewKind::None;
  bool thumbnailAvailable = false;
};

inline CellKind FlowKindOf(const FlowItem& item) {
  return CellKindFor(item.isDir, item.kind, item.thumbnailAvailable);
}

struct FlowCell {
  int index = -1;
  CellKind kind = CellKind::Row;
  Rect rect;
  bool operator==(const FlowCell&) const = default;
};

// Every item's painted rectangle, in catalog order, at scroll 0.
inline std::vector<FlowCell> FlowCells(const Layout& layout, std::span<const FlowItem> items) {
  std::vector<FlowCell> out;
  const int count = static_cast<int>(items.size());
  if (count <= 0 || layout.columns <= 0 || layout.contentInner.Empty()) return out;
  out.reserve(static_cast<size_t>(count));

  if (!kRowsSpanContent) {
    // Negative control: V1's shape. Every item is a CELL; a tile-less item
    // paints its 32 px pill anchored to the top of its 220 px cell.
    for (int i = 0; i < count; ++i) {
      const Rect cell = TileRect(layout, i, 0);
      const CellKind kind = FlowKindOf(items[static_cast<size_t>(i)]);
      out.push_back(FlowCell{i, kind, kind == CellKind::Row ? FallbackRowRect(cell) : cell});
    }
    return out;
  }

  int y = layout.contentInner.top;
  int i = 0;
  while (i < count) {
    if (FlowKindOf(items[static_cast<size_t>(i)]) == CellKind::Row) {
      // The band: full content width, 32 px, immediately under the previous
      // band -- no gap, so N rows occupy exactly N * kFlowRowHeight.
      out.push_back(FlowCell{i, CellKind::Row,
                             Rect{layout.content.left, y, layout.content.right,
                                  y + kFlowRowHeight}});
      y += kFlowRowHeight;
      ++i;
      continue;
    }
    // A run of tiles fills ONE grid row; a row item ends the run early (a tile
    // never shares a band with a row).
    int column = 0;
    while (i < count && column < layout.columns &&
           FlowKindOf(items[static_cast<size_t>(i)]) == CellKind::Tile) {
      const int x = layout.contentInner.left + column * (layout.tileWidth + layout.gap);
      out.push_back(FlowCell{i, CellKind::Tile,
                             Rect{x, y, x + layout.tileWidth, y + layout.cellHeight}});
      ++column;
      ++i;
    }
    y += layout.cellHeight;
  }
  return out;
}

// The cells that intersect the content viewport at scrollPx (the neighbours
// just outside it are kept so a scrolling frame never pops a half-row in).
inline std::vector<FlowCell> VisibleFlowCells(const Layout& layout,
                                              std::span<const FlowItem> items,
                                              int scrollPx) {
  std::vector<FlowCell> cells = FlowCells(layout, items);
  std::vector<FlowCell> out;
  if (cells.empty() || layout.contentInner.Empty()) return out;
  const int scroll = (std::max)(0, scrollPx);
  const int top = layout.contentInner.top - kFlowRowHeight;
  const int bottom = layout.contentInner.bottom + layout.cellHeight;
  out.reserve(cells.size());
  for (FlowCell& cell : cells) {
    cell.rect.top -= scroll;
    cell.rect.bottom -= scroll;
    if (cell.rect.bottom <= top || cell.rect.top >= bottom) continue;
    out.push_back(cell);
  }
  return out;
}

inline int FlowContentHeight(const Layout& layout, std::span<const FlowItem> items) {
  const std::vector<FlowCell> cells = FlowCells(layout, items);
  if (cells.empty()) return 0;
  return (std::max)(0, cells.back().rect.bottom - layout.contentInner.top);
}

inline int FlowMaxScroll(const Layout& layout, std::span<const FlowItem> items) {
  return (std::max)(0, FlowContentHeight(layout, items) - layout.contentInner.Height());
}

// One item's painted rectangle (scroll 0). Returns an empty rect when the index
// is out of range, so a caller never divides by a phantom cell.
inline Rect FlowItemRect(const Layout& layout, std::span<const FlowItem> items, int index) {
  if (index < 0 || index >= static_cast<int>(items.size())) return Rect{};
  const std::vector<FlowCell> cells = FlowCells(layout, items);
  if (index >= static_cast<int>(cells.size())) return Rect{};
  return cells[static_cast<size_t>(index)].rect;
}

// Client point -> item index, through the SAME rectangles that were painted.
inline int FlowItemAt(const Layout& layout, std::span<const FlowItem> items, int x, int y,
                      int scrollPx) {
  if (x < layout.content.left || x >= layout.content.right) return -1;
  const int scroll = (std::max)(0, scrollPx);
  // kFlowHitPitch == kFlowRowHeight in every shipping build (the control adds
  // one pixel, so the boundaries move and the suite's boundary checks go red).
  const int local = y + scroll + (kFlowHitPitch - kFlowRowHeight);
  const std::vector<FlowCell> cells = FlowCells(layout, items);
  for (const FlowCell& cell : cells) {
    if (local < cell.rect.top || local >= cell.rect.bottom) continue;
    if (x < cell.rect.left || x >= cell.rect.right) continue;
    return cell.index;
  }
  return -1;
}

// --------------------------------------------------------- V4: the status bar
// DESIGN.md §2 gives the status band 28 px and §3 lists its four states
// (常规/加密/未知方法/压缩中). A 28 px band that paints nothing is the defect the
// screenshot showed; this block is the model behind the four segments, so the
// text and the rectangles are assertable without a device.
enum class CryptoState : std::uint8_t { Unencrypted, Encrypted, Unknown };

inline std::ostream& operator<<(std::ostream& os, CryptoState state) {
  switch (state) {
    case CryptoState::Unencrypted: return os << "Unencrypted";
    case CryptoState::Encrypted: return os << "Encrypted";
    case CryptoState::Unknown: return os << "Unknown";
  }
  return os << "?";
}

struct StatusInfo {
  CryptoState crypto = CryptoState::Unknown;
  std::string_view method;              // empty -> "方法未知"
  std::string_view dictionary;          // empty -> "字典未知"
  int ratioPercent = -1;                // < 0 -> unknown
  // C81/A3: the canonical ratio view (prism::CompressionRatioView of the
  // archive totals). EMPTY means unknown; non-empty it is the exact string
  // the list cell and the info card render -- one source, one string.
  std::string ratioView;
  bool compressing = false;             // wins over the ratio
  // C79: the RIGHT cluster's two counts. -1 means "not reported", which paints
  // the label with an explicit "--" rather than a fabricated 0.
  int itemCount = -1;
  int selectedCount = -1;
};

// C79: two clusters. The left one answers "what is this archive"; the right
// one answers "how much of it am I looking at". Divider is a real segment (a
// 1 px rule) so its rect is laid out with the same arithmetic as the text and
// the painter never invents an x coordinate.
enum class StatusId : std::uint8_t {
  Encryption,
  Method,
  Dictionary,
  Divider,
  ItemCount,
  SelectedCount,
  Ratio,
  Count
};

inline std::ostream& operator<<(std::ostream& os, StatusId id) {
  switch (id) {
    case StatusId::Encryption: return os << "Encryption";
    case StatusId::Method: return os << "Method";
    case StatusId::Dictionary: return os << "Dictionary";
    case StatusId::Divider: return os << "Divider";
    case StatusId::ItemCount: return os << "ItemCount";
    case StatusId::SelectedCount: return os << "SelectedCount";
    case StatusId::Ratio: return os << "Ratio";
    case StatusId::Count: return os << "Count";
  }
  return os << "?";
}

inline std::string_view CryptoText(const StatusInfo& info) {
  switch (info.crypto) {
    case CryptoState::Unencrypted: return "未加密";
    case CryptoState::Encrypted: return "已加密";
    case CryptoState::Unknown: return "加密状态未知";
  }
  return "加密状态未知";
}

inline std::string_view MethodText(const StatusInfo& info) {
  return info.method.empty() ? std::string_view{"方法未知"} : info.method;
}

inline std::string_view DictionaryText(const StatusInfo& info) {
  return info.dictionary.empty() ? std::string_view{"字典未知"} : info.dictionary;
}

// The label of the ratio segment; the NUMBER travels separately (value), so the
// painter can format "压缩率 87%" without this header owning a buffer.
inline std::string_view RatioText(const StatusInfo& info) {
  if (info.compressing) return "压缩中…";
  // C81/A3: known now means "the canonical view string is present". The
  // PRISM_C81_MUTANT_STATUS_HARDCODED_UNKNOWN arm (Tests/suites/c81-a2a3.cmake)
  // injects the OLD lie -- the band claiming 未知 while the row and the card
  // show the real value -- and the consistency suite MUST go red.
#if defined(PRISM_C81_MUTANT_STATUS_HARDCODED_UNKNOWN)
  return "压缩率未知";
#else
  return (!info.ratioView.empty() || info.ratioPercent >= 0) ? "压缩率" : "压缩率未知";
#endif
}

// C79: the right cluster's labels. The NUMBER travels in StatusSegment::value,
// exactly like Ratio's percentage, so this header owns no buffer.
inline std::string_view ItemCountText(const StatusInfo& info) {
  return info.itemCount >= 0 ? "条目" : "条目 --";
}

inline std::string_view SelectedCountText(const StatusInfo& info) {
  return info.selectedCount >= 0 ? "选中" : "选中 --";
}

struct StatusSegment {
  StatusId id = StatusId::Count;
  Rect rect;
  std::string_view text;
  int value = -1;  // Ratio only: the percentage, or -1 when unknown/compressing
  // C81/A3: the canonical ratio string. When non-empty the painter renders
  // text + ' ' + valueText (no integer append).
  //
  // C86 (ASan, CI): this USED to be a std::string_view -- "a view into
  // StatusInfo::ratioView, which the scene owns for the whole frame". That
  // contract is invisible at the call site and one ordinary spelling breaks it:
  //     for (auto& s : StatusSegments(layout, StatusForPublish())) { ... }
  // The StatusInfo is a TEMPORARY there, destroyed at the end of the range-init,
  // so every ratio segment kept a view into a dead std::string. It cost a
  // stack-use-after-scope in win.c80_a11y and in c86.window on the ASan runner.
  // Owning the bytes is a few characters per frame and makes the vector
  // self-contained, which is what a caller has every right to assume.
  std::string valueText;
};

inline constexpr int kStatusPad = 12;
// C105 (plan 3.2): the band separates its segments with AIR, not with rules.
// The gap is the 16-24 px the design asks for; the mutation arm squeezes it to
// 8 px so the "adjacent segments are at least 16 px apart" assertion MUST go
// red (CONTRACTS D3).
#if defined(PRISM_C105_MUTANT_STATUS_TIGHT_GAP)
inline constexpr int kStatusGap = 8;
#else
inline constexpr int kStatusGap = 24;
#endif
// ... and the 1 px vertical rule between the two clusters is GONE. The switch
// is false in every shipping build; the mutation arm restores the rule, which
// must turn the "the band carries no vertical rule" assertion red.
#if defined(PRISM_C105_MUTANT_STATUS_DIVIDER)
inline constexpr bool kStatusDividerEnabled = true;
#else
inline constexpr bool kStatusDividerEnabled = false;
#endif
inline constexpr int kStatusSegmentMinWidth = 96;

// C79: TWO clusters, laid out from the bar's two edges. The left cluster
// (encryption | method | dictionary) starts at the left padding; the right
// cluster (items | selected | ratio) is RIGHT-aligned, so the empty middle is
// the "40% blank" the design asks for and it grows with the window instead of
// leaving a ragged tail. A 1 px Divider sits at the midpoint of that gap.
inline std::vector<StatusSegment> StatusSegments(const Layout& layout, const StatusInfo& info) {
  std::vector<StatusSegment> out;
  if (layout.statusBar.Empty()) return out;
  const int top = layout.statusBar.top + 4;
  const int bottom = layout.statusBar.bottom - 4;
  if (bottom <= top) return out;

  // C79: tighter than the pre-C79 168/128/140 so the left cluster reads as one
  // group; the freed width becomes the blank middle (>= 40% at 1600 px).
  const int leftWidths[] = {150, 110, 120};
  const std::string_view leftTexts[] = {CryptoText(info), MethodText(info), DictionaryText(info)};
  int x = layout.statusBar.left + kContentPadding;
  int leftEnd = x;
  for (int i = 0; i < 3; ++i) {
    const int width = (std::max)(kStatusSegmentMinWidth, leftWidths[i]);
    const Rect rect{x, top, (std::min)(x + width, layout.statusBar.right - kContentPadding), bottom};
    if (rect.Empty()) break;
    out.push_back(StatusSegment{static_cast<StatusId>(i), rect, leftTexts[i], -1, {}});
    leftEnd = rect.right;
    x = rect.right + kStatusGap;
  }

  // The right cluster is measured from the RIGHT edge: items, selected, ratio.
  // Its width is fixed, so the ratio column never jumps when a count changes.
  const int rightWidths[] = {96, 96, 112};
  int rightWidth = 2 * kStatusGap;
  for (int i = 0; i < 3; ++i) rightWidth += (std::max)(kStatusSegmentMinWidth, rightWidths[i]);
  int rightStart = layout.statusBar.right - kContentPadding - rightWidth;
  if (rightStart < leftEnd + kStatusGap) {
    // Narrow window: keep the right cluster's right edge (the ratio stays
    // readable) and let the gap collapse. The divider is omitted rather than
    // drawn through a label.
    rightStart = leftEnd + kStatusGap;
  }
  const int gap = rightStart - leftEnd;
  // C105: NO DIVIDER. The band used to cut the two clusters apart with a 1 px
  // vertical rule -- the "mechanical grey bar split by six rules" the user
  // reported. The empty middle is now pure air; the clusters are told apart by
  // the gap alone (and by the label/value roles inside each segment).
  if (kStatusDividerEnabled && gap >= 2 * kStatusGap) {
    const int dividerX = leftEnd + gap / 2;
    out.push_back(StatusSegment{StatusId::Divider,
                                Rect{dividerX, top, dividerX + 1, bottom}, {}, -1, {}});
  }
  const std::string_view rightTexts[] = {ItemCountText(info), SelectedCountText(info),
                                         RatioText(info)};
  const int rightValues[] = {info.itemCount, info.selectedCount,
                             (info.compressing ? -1 : info.ratioPercent)};
  const std::string rightValueTexts[] = {{}, {},
      (info.compressing ? std::string() : info.ratioView)};
  int rx = rightStart;
  for (int i = 0; i < 3; ++i) {
    const int width = (std::max)(kStatusSegmentMinWidth, rightWidths[i]);
    const Rect rect{rx, top, (std::min)(rx + width, layout.statusBar.right - kContentPadding),
                    bottom};
    if (rect.Empty()) break;
    out.push_back(StatusSegment{static_cast<StatusId>(4 + i), rect, rightTexts[i],
                                rightValues[i], rightValueTexts[i]});
    rx = rect.right + kStatusGap;
  }
  return out;
}


// ------------------------------------------------------------- list mode
inline Rect ListHeaderRect(const Layout& layout) {
  return Rect{layout.contentInner.left, layout.contentInner.top, layout.contentInner.right,
              (std::min)(layout.contentInner.bottom,
                         layout.contentInner.top + kListHeaderHeight)};
}

inline Rect ListBodyRect(const Layout& layout) {
  const Rect header = ListHeaderRect(layout);
  return Rect{layout.contentInner.left, header.bottom, layout.contentInner.right,
              layout.contentInner.bottom};
}

// C122: the row pitch is a property of the WINDOW (its height, in DIP), not of
// the caller. A 1007x742 window gets the 26 DIP regular tier; a short one
// (< 700 DIP) gets 22. Every consumer -- paint, virtualization, hit test and the
// window's scroll viewport -- asks THIS function, so a frame can never paint one
// pitch and hit-test another. Tests at 96 DPI read 26 / 22 directly (DIP == px
// at 96 DPI).
inline int ListRowHeightFor(const Layout& layout) {
  return layout.windowH < kListCompactWindowHeight ? kListRowHeightCompact : kListRowHeight;
}

inline Rect ListRowRect(const Layout& layout, int row, int scrollPx) {
  const Rect body = ListBodyRect(layout);
  const int pitch = ListRowHeightFor(layout);
  const int y = body.top + row * pitch - scrollPx;
  return Rect{body.left, y, body.right, y + pitch};
}

// The first/last row that intersects the body, overscan rows on both sides.
struct RowRange {
  int first = 0;
  int last = -1;
  bool Empty() const { return first > last; }
};

inline RowRange VisibleListRows(const Layout& layout, int rowCount, int scrollPx) {
  RowRange range;
  if (rowCount <= 0) return range;
  const Rect body = ListBodyRect(layout);
  if (body.Empty()) return range;
  const int pitch = ListRowHeightFor(layout);
  constexpr int kOverscan = 2;
  range.first = (std::max)(0, scrollPx / pitch - kOverscan);
  range.last = (std::min)(rowCount - 1,
                          (scrollPx + body.Height()) / pitch + kOverscan - 1);
  if (range.last < range.first) range.first = range.last;
  return range;
}

// Hit test: a client point inside the body maps to a row, else -1.
inline int ListRowAt(const Layout& layout, int x, int y, int scrollPx) {
  const Rect body = ListBodyRect(layout);
  if (body.Empty() || x < body.left || x >= body.right) return -1;
  if (y < body.top || y >= body.bottom) return -1;
  const int pitch = ListRowHeightFor(layout);
  return (y - body.top + scrollPx) / pitch;
}

// ------------------------------------------------------- C122: the four cells
// The list's column rectangles, right-anchored: the TYPE column ends on the
// content edge minus kListRightPadding (so the last cell cannot be clipped),
// then original / packed / name march leftwards with one 16 DIP gutter between
// them. The name column takes whatever is left.
//
// One struct, one function: the header labels, the row cells, the sort hit test
// and the suite all read THIS, so "the header says 压缩后大小 but the number is
// under 原始大小" is not a state the code can reach.
//
// C27's persisted prism::list::ColumnLayout (window.columns.size/packed/ratio)
// is still SAVED and LOADED, but it does NOT participate in painting: the four
// widths above are fixed by the design (120/120/100), and changing this function
// is how a future card makes columns draggable -- do not grow a second column
// model beside it. Reading a stale ColumnLayout to position a cell would put
// the numbers under the wrong heading.
//
// RULE FOR EVERY LIST-GEOMETRY ASSERTION: derive the rectangle from THIS
// function (or from GalleryWindow::LayoutForTest(), which is the same object the
// frame painted) -- never hard-code a pixel value. A test that writes "x = 1388"
// or "row = 36" goes red for a layout change it was never about, and the usual
// "fix" (re-baselining the number) is noise that hides the real regression.
struct ListColumnLayout {
  Rect name;
  Rect packed;
  Rect original;
  Rect type;

  const Rect& For(prism::list::Column column) const {
    switch (column) {
      case prism::list::Column::Name: return name;
      case prism::list::Column::PackedSize: return packed;
      case prism::list::Column::OriginalSize: return original;
      case prism::list::Column::Type: return type;
    }
    return name;
  }
};

inline ListColumnLayout ListColumnRects(const Layout& layout) {
  ListColumnLayout columns;
  const Rect header = ListHeaderRect(layout);
  if (header.Empty()) return columns;
  // Degenerate widths clamp to the header's left edge instead of producing
  // rectangles that run backwards through their neighbour.
  const int right = (std::max)(header.left, header.right - kListRightPadding);
  const int typeLeft = (std::max)(header.left, right - kListColTypeWidth);
  columns.type = Rect{typeLeft, header.top, right, header.bottom};
  const int originalRight = (std::max)(header.left, typeLeft - kListColumnGutter);
  columns.original = Rect{(std::max)(header.left, originalRight - kListColOriginalWidth),
                          header.top, originalRight, header.bottom};
  const int packedRight = (std::max)(header.left, columns.original.left - kListColumnGutter);
  columns.packed = Rect{(std::max)(header.left, packedRight - kListColPackedWidth), header.top,
                        packedRight, header.bottom};
  const int nameLeft = header.left + kListIndent;
  const int nameRight = (std::max)(nameLeft, columns.packed.left - kListColumnGutter);
  columns.name = Rect{nameLeft, header.top, nameRight, header.bottom};
  return columns;
}

// Which header cell is under (x, y)? prism::list::Column::Name is column 0, so
// an unset point cannot be confused with a real hit -- use ListColumnHit below.
struct ListColumnHit {
  prism::list::Column column = prism::list::Column::Name;
  bool hit = false;
};

inline ListColumnHit ListColumnAt(const Layout& layout, int x, int y) {
  ListColumnHit result;
  const Rect header = ListHeaderRect(layout);
  if (header.Empty() || y < header.top || y >= header.bottom) return result;
  const ListColumnLayout columns = ListColumnRects(layout);
  for (const prism::list::Column column : prism::list::kColumnOrder) {
    const Rect rect = columns.For(column);
    if (rect.Empty() || x < rect.left || x >= rect.right) continue;
    result.column = column;
    result.hit = true;
    return result;
  }
  return result;
}

// ------------------------------------------------------------- floating chrome
// The breadcrumb pill lives INSIDE the action band (44 px, the blueprint's nav
// island height), so it can never overlap the title bar or the status bar.
//
// C115: it also shares that band with the action island, so its right edge is
// the FIRST thing the narrow window takes away. This function is the ONE place
// that decides how much -- actionbar::Compute passes its own measured floor in,
// and every other consumer takes the default (the same number). Before this
// card the pill here was ALWAYS 560 px while Compute quietly used a narrower
// one: the renderer painted the narrow pill, but the glass island panel (sized
// from THIS function, GalleryWindow::GlassPanelRect) stayed 560 px, so the
// acrylic island really did extend 144 px under the toolbar at 1028.
inline Rect NavPillRect(const Layout& layout, int actionReserve) {
  const int available = (std::max)(0, layout.actionIsland.Width() - 2 * kContentPadding);
  const int width = (std::min)(kNavPillMaxWidth, available);
  const int height = (std::min)(kNavPillHeight, layout.actionIsland.Height());
  const int top = layout.actionIsland.top + (layout.actionIsland.Height() - height) / 2;
  Rect pill{kContentPadding, top, kContentPadding + width, top + height};
  if (kNavPillYieldsToBand && actionReserve > 0 && !pill.Empty()) {
    // The right edge the island needs: band.right - inset - island - gap.
    const int maxRight =
        layout.actionIsland.right - kContentPadding - actionReserve - kNavActionGap;
    // '>=' (not '>'): when the island eats the last pixel the pill becomes
    // EMPTY rather than a 0-wide rounded rect with two borders on one column.
    if (maxRight >= pill.left) pill.right = (std::min)(pill.right, maxRight);
  }
  return pill;
}

inline Rect NavPillRect(const Layout& layout) {
  return NavPillRect(layout, kActionIslandMinWidth);
}

inline Rect NavTextRect(const Layout& layout) {
  const Rect pill = NavPillRect(layout);
  // C115: the 24 px inset must not invert the text box inside a pill that has
  // been squeezed below 48 px (640 px window). Halving the inset keeps the box
  // inside the pill and, at the floor, empty -- which reads as "no crumb text
  // to click", not as a box running backwards through the toolbar.
  const int inset = (std::min)(kNavTextInset, (std::max)(0, pill.Width() / 2));
  return Rect{pill.left + inset, pill.top + 10, pill.right - inset, pill.bottom - 10};
}

// ============================================== C123: the toolbar's rectangle
// WHERE the eight buttons live. In the V1 composition (and in the pre-C123
// window the audit photographed) the toolbar is the pill's right-hand
// neighbour: the first button starts at pill.right + 12, which is where the
// 643 px came from. Under C123 it is the band itself, edge to edge, so its left
// and right edges ARE the content band's -- "贴边通栏".
//
// The D3 control returns the pre-card floating island instead (the 589-start /
// 964-wide rectangle in 14.4), which is why T2 and T3 go red in that build.
inline Rect ToolbarBandRect(const Layout& layout) {
  const Rect band = layout.toolbar;
  if (band.Empty()) return Rect{};
  if (kToolbarSpansBand) return band;
  // MUTANT (floating_toolbar): the pill keeps its full width and the toolbar
  // is pushed to its right, inset 16 from the band's right edge.
  const Rect pill = NavPillRect(layout);
  const int left = (std::min)(band.right, pill.right + kNavActionGap);
  const int right = (std::max)(left, band.right - kContentPadding);
  return Rect{left, band.top, right, band.bottom};
}

// ============================================= C123: the breadcrumb path bar
// The text box of the path bar: left-aligned to the content band + 16 (proposal
// 17.3) and 18 px tall inside the 28 px row -- one line of 13 px type with the
// 2 px rule's room under it.
//
// This is the ONE answer to "where does the crumb start". The window's hit test
// and the renderer's text run both read it, so a click can never land on a
// glyph the user is not looking at. It replaces the CENTRED box
// (LayoutCenteredBreadcrumb): centring put the first glyph at 234 while the
// container started at 28 -- 62.2 % of the row was dead air and the left edge
// drifted with the path's length.
inline constexpr int kCrumbTextInsetY = 3;

inline Rect CrumbTextRect(const Layout& layout) {
  const Rect bar = layout.crumbBar;
  if (bar.Empty()) return Rect{};
  const int left = layout.contentBand.left + kBreadcrumbInset;
  // A window narrower than 2 * the inset would invert the box; the floor keeps
  // it a degenerate (empty) rectangle instead of a backwards one.
  const int right = (std::max)(left, layout.contentBand.right - kBreadcrumbInset);
  const int top = bar.top + kCrumbTextInsetY;
  const int bottom = (std::max)(top, bar.bottom - kCrumbTextInsetY);
  return Rect{left, top, right, bottom};
}

// The 2 px rule under the CURRENT segment: the "一线" token (16.3), painted on
// the crumb's own bottom edge and clamped to the bar. A segment that was
// clipped away (or an empty one) yields an empty rect -- never a 2 px dash at
// the start of the bar.
inline Rect CrumbUnderlineRect(const Layout& layout, int spanLeft, int spanRight) {
  const Rect bar = layout.crumbBar;
  if (bar.Empty() || spanRight <= spanLeft) return Rect{};
  const int left = (std::max)(bar.left, (std::min)(spanLeft, bar.right));
  const int right = (std::max)(left, (std::min)(spanRight, bar.right));
  if (right <= left) return Rect{};
  return Rect{left, bar.bottom - kBreadcrumbUnderlineHeight, right, bar.bottom};
}

// Quick Look: a card anchored to the content's top-right corner.
inline Rect QuickLookCardRect(const Layout& layout) {
  const int width = (std::min)(kQuickLookWidth,
                               (std::max)(0, layout.contentInner.Width() - 2 * kContentPadding));
  const int height = (std::min)(kQuickLookHeight,
                                (std::max)(0, layout.contentInner.Height() - 2 * kContentPadding));
  const int right = layout.contentInner.right - kContentPadding;
  const int top = layout.contentInner.top + kContentPadding;
  return Rect{right - width, top, right, top + height};
}

// The area inside the Quick Look card that holds the decoded image (C87). ONE
// definition, read by the painter AND by the suite: the card's inset and the
// title/meta strip are geometry, and a test that re-derived them could agree
// with a bug.
inline constexpr int kQuickLookInset = 14;
inline constexpr int kQuickLookFooterHeight = 110;

inline Rect QuickLookPreviewRect(const Layout& layout) {
  const Rect card = QuickLookCardRect(layout);
  return Rect{card.left + kQuickLookInset, card.top + kQuickLookInset,
              card.right - kQuickLookInset, card.top + card.Height() - kQuickLookFooterHeight};
}

// Contain-fit of a w x h image inside 'area', centred. The C87 suite samples the
// SAME rectangle the renderer draws into, so "the preview has the source's
// pixels" is measured where the pixels are.
inline Rect FitContain(const Rect& area, int contentW, int contentH) {
  if (area.Empty() || contentW <= 0 || contentH <= 0) return Rect{};
  const double sx = static_cast<double>(area.Width()) / static_cast<double>(contentW);
  const double sy = static_cast<double>(area.Height()) / static_cast<double>(contentH);
  const double scale = sx < sy ? sx : sy;
  const int width = (std::max)(1, static_cast<int>(contentW * scale + 0.5));
  const int height = (std::max)(1, static_cast<int>(contentH * scale + 0.5));
  const int left = area.left + (area.Width() - width) / 2;
  const int top = area.top + (area.Height() - height) / 2;
  return Rect{left, top, left + width, top + height};
}

// Progress capsule: bottom-centre of the CONTENT (it used to cross into the
// status bar, which is the same class of bug as the overlapping cards).
inline Rect ProgressCapsuleRect(const Layout& layout) {
  const int width = (std::min)(kProgressCapsuleWidth, layout.contentInner.Width());
  const int left = layout.contentInner.left + (layout.contentInner.Width() - width) / 2;
  const int bottom = layout.contentInner.bottom - kContentPadding;
  const int height = (std::min)(kProgressCapsuleHeight,
                                (std::max)(0, bottom - layout.contentInner.top));
  return Rect{left, bottom - height, left + width, bottom};
}

// =========================================================== C66: the frame
// THE DEFECT (measured, not assumed): a user with a 1536x816 work area had
// "window": {"width": 1552, "height": 832} written to settings.json -- exactly
// the work area PLUS the DWM invisible resize border (8 px per side on both
// axes). Two separate rules produced that number, and both have to go:
//
//   1. GetWindowRect() is NOT the frame the user sees. On a resizable window the
//      DWM extends the window rectangle by SM_CXSIZEFRAME + SM_CXPADDEDBORDER
//      past the visible edge on the left/right/top, and by the same amount at
//      the bottom. Persisting that rectangle and handing it back to
//      SetWindowPos() makes the NEXT window 16 px wider than the one the user
//      closed (the saved rect already contains a border the new window will add
//      again). Five cycles is enough to walk a 1200 px window off a 1536 px
//      screen -- and the tail of the action island (添加/删除/测试) is the first
//      thing that leaves the screen.
//   2. A MAXIMIZED window's GetWindowRect() is the monitor work area inflated by
//      that same border -- so a user who simply maximised and closed saved a
//      rectangle that can never fit any screen again.
//
// The policy below is pure integer math (no windows.h; the Linux lane asserts
// it): persist the VISIBLE frame, and CLAMP every restored frame into the
// monitor work area before it reaches SetWindowPos. The Win32 half (DWM
// extended bounds, MonitorFromRect, SetWindowPos) lives in
// Src/PrismWin/WindowFrame.hpp.
//
// CONTRACTS B1: every min/max is parenthesised.

// The floor a restored window may never go below. These are the SAME numbers
// SettingsStore publishes as window.width/window.height's documented minimum
// (640 x 480); they live here because they are geometry, and a second copy in
// the store would be a second answer to "how small may a window be".
inline constexpr int kMinWindowWidth = 640;
inline constexpr int kMinWindowHeight = 480;

// The four thicknesses of the DWM invisible border, in pixels, measured on the
// live window (GetWindowRect minus DWMWA_EXTENDED_FRAME_BOUNDS per side).
// Positive means "the window rect reaches this far past the visible frame".
struct FrameBorders {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  constexpr bool operator==(const FrameBorders&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const FrameBorders& b) {
  return os << "{" << b.left << "," << b.top << "," << b.right << "," << b.bottom << "}";
}

// Clamp a frame into the work area. Rules, in this order:
//   1. never wider/taller than the work area;
//   2. never smaller than the documented minimum -- UNLESS the work area itself
//      is smaller than the minimum (a 320 px wide screen must not produce a
//      640 px window that hangs off it);
//   3. the LEFT and TOP edges stay inside the work area, so a saved rect from a
//      monitor that is no longer attached (or a negative-coordinate layout)
//      lands on screen instead of at x = -1920.
// A frame that already fits is returned unchanged (byte-for-byte), which is
// what makes the save/restore cycle a fixed point rather than a slow drift.
inline Rect ClampFrameToWorkArea(const Rect& frame, const Rect& work,
                                 int minWidth = kMinWindowWidth,
                                 int minHeight = kMinWindowHeight) {
  // CONTRACTS D3 negative control: the whole clamp is compiled out, so the
  // "an oversized saved frame lands inside the work area" assertions go red.
#if defined(PRISM_C66_MUTANT_NO_WORKAREA_CLAMP)
  // CONTRACTS D31: every parameter is still referenced, or the mutant build
  // fails on -Werror=unused-parameter before the rule it is meant to break.
  (void)work;
  (void)minWidth;
  (void)minHeight;
  return frame;
#else
  if (work.Empty()) return frame;  // no work area known: nothing to clamp to
  Rect out = frame;
  out.left = frame.left;
  out.top = frame.top;

  const int workWidth = work.Width();
  const int workHeight = work.Height();
  int width = (std::min)(frame.Width(), workWidth);
  int height = (std::min)(frame.Height(), workHeight);
  if (width < 0) width = 0;
  if (height < 0) height = 0;
  // (2) the minimum, but never above what the work area can hold.
  width = (std::max)(width, (std::min)(minWidth, workWidth));
  height = (std::max)(height, (std::min)(minHeight, workHeight));

  // (3) both edges inside. A frame wider than the work area keeps its LEFT edge
  // at the work area's left, because a window whose right edge is off screen
  // loses the toolbar; one whose left edge is off screen loses the sidebar.
  out.left = (std::max)(work.left, (std::min)(frame.left, work.right - width));
  out.top = (std::max)(work.top, (std::min)(frame.top, work.bottom - height));
  out.right = out.left + width;
  out.bottom = out.top + height;
  return out;
#endif
}

// The window rectangle SetWindowPos must be given to produce the visible frame
// 'visible'. The invisible border is OUTSIDE the visible frame on every side.
inline Rect WindowRectForVisibleFrame(const Rect& visible, const FrameBorders& borders) {
  return Rect{visible.left - borders.left, visible.top - borders.top,
              visible.right + borders.right, visible.bottom + borders.bottom};
}

// The inverse: the frame the user actually sees, from GetWindowRect.
inline Rect VisibleFrameForWindowRect(const Rect& windowRect, const FrameBorders& borders) {
  return Rect{windowRect.left + borders.left, windowRect.top + borders.top,
              windowRect.right - borders.right, windowRect.bottom - borders.bottom};
}

// Does the whole frame sit inside the work area (tolerance 0)?
inline bool FrameFitsWorkArea(const Rect& frame, const Rect& work) {
  return work.Contains(frame);
}

// The rectangle the app should PERSIST for this window: the visible frame the
// user sees, not the shadow-inflated window rect. Kept here (pure) so the rule
// is assertable without a window station; the Win32 measurement feeds it.
inline Rect FrameToPersist(const Rect& windowRect, const FrameBorders& borders) {
  return VisibleFrameForWindowRect(windowRect, borders);
}


// ==================================================== C86: the SAVED window
// THE DEFECT (measured with the shipped exe, 2026-11-05): the settings document
// held
//     "window": { "width": 1536, "height": 480, "listMode": true, "sort": {...} }
// and NOTHING about where the window was. A user who dragged it to (300,150)
// got the CW_USEDEFAULT cascade position (98,80) back on the next start. C66
// fixed WHAT size is written (the visible frame, clamped into the work area)
// but only ever carried width/height across the restart: the origin was dropped
// on the way out -- GalleryWindow captured a SIZE, and the app's WindowState had
// no field for x/y -- so there was nothing to restore on the way in.
//
// THE POLICY, and why it is a struct rather than two more integers:
//   * a saved window is an ORIGIN and a SIZE, and both are the VISIBLE frame
//     C66 defines (DWMWA_EXTENDED_FRAME_BOUNDS, never GetWindowRect -- see the
//     C66 note above: GetWindowRect includes the DWM shadow border);
//   * `maximized` is a SEPARATE bit. The rectangle of a maximized window is the
//     work area, so persisting it as the normal frame is exactly the historical
//     trap (a 1552x832 normal frame on a 1536x816 screen). Restore the NORMAL
//     rectangle, THEN maximize; that is also what makes un-maximizing put the
//     window where the user left it;
//   * `hasPosition` is false for a document a pre-C86 build wrote. There is no
//     origin to honour there, so the window keeps the one it was created with
//     and only the size is applied (C66's behaviour, preserved);
//   * every restore is clamped into the work area (ClampFrameToWorkArea), so a
//     frame saved on a monitor that has since been unplugged -- or on a screen
//     that has since been made smaller -- lands on screen instead of at
//     x = -1920.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C86_MUTANT_NO_POSITION makes
// SavedWindowFor() drop the origin, which is the pre-fix document byte for byte
// (a size and nothing else). The suite's "the window comes back where the user
// left it" assertions MUST go red in that build.

#if defined(PRISM_C86_MUTANT_NO_POSITION)
inline constexpr bool kPersistWindowPosition = false;
#else
inline constexpr bool kPersistWindowPosition = true;
#endif

// The runtime proof the arm is REALLY armed (D3): a stale macro would leave the
// mutant green while its entry stayed red forever.
// A maximized window's rectangle is a MODE, not a frame: it is the work area, and
// writing it as the normal rectangle is the 1552x832 defect. This switch is the
// D3 control for that rule (the saved frame becomes the maximized rectangle).
#if defined(PRISM_C86_MUTANT_MAXIMIZED_AS_NORMAL)
inline constexpr bool kMaximizedIsASeparateBit = false;
#else
inline constexpr bool kMaximizedIsASeparateBit = true;
#endif

// The breadcrumb's count follows the folder (S3-4). The control restores the
// app-written archive total, which is the number the user reported seeing next
// to a two-entry folder name.
#if defined(PRISM_C86_MUTANT_ARCHIVE_COUNT)
inline constexpr bool kBreadcrumbCountsFolder = false;
#else
inline constexpr bool kBreadcrumbCountsFolder = true;
#endif

inline constexpr std::string_view ActiveWindowMutation() {
  if (!kPersistWindowPosition) return "no_position";
  if (!kMaximizedIsASeparateBit) return "maximized_as_normal";
  if (!kBreadcrumbCountsFolder) return "archive_count";
  return {};
}

struct SavedWindow {
  Rect visible{};
  bool maximized = false;
  // false == the document predates C86 and holds only a size.
  bool hasPosition = true;

  bool operator==(const SavedWindow&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const SavedWindow& saved) {
  return os << "[" << saved.visible.left << "," << saved.visible.top << " "
            << saved.visible.Width() << "x" << saved.visible.Height()
            << (saved.maximized ? " max" : "") << (saved.hasPosition ? "" : " size-only") << "]";
}

// What the app may write for a closing window. The origin survives only when the
// rule is armed; the maximized bit always does, because it is a MODE and not a
// rectangle.
inline SavedWindow SavedWindowFor(const Rect& visible, bool maximized) {
  SavedWindow saved;
  saved.visible = kPersistWindowPosition
                      ? visible
                      : Rect{0, 0, visible.Width(), visible.Height()};
  saved.maximized = maximized;
  saved.hasPosition = kPersistWindowPosition;
  return saved;
}

// The frame a restore may apply. The clamp is the C66 rule applied to the WHOLE
// rectangle: the SIZE is capped by the work area and the ORIGIN is pulled back
// inside it, so a saved (300,150) on a work area that shrank to 0..800 x 0..600
// lands at (300,150) still fully visible, and a saved frame from an unplugged
// second monitor lands on the monitor the user still has.
struct WindowRestore {
  Rect frame{};        // the VISIBLE normal frame to apply
  bool maximized = false;
  bool moved = false;  // the clamp had to move the saved origin

  bool operator==(const WindowRestore&) const = default;
};

inline WindowRestore PlanWindowRestore(const SavedWindow& saved, const Rect& work,
                                       int minWidth = kMinWindowWidth,
                                       int minHeight = kMinWindowHeight) {
  WindowRestore plan;
  plan.frame = ClampFrameToWorkArea(saved.visible, work, minWidth, minHeight);
  plan.maximized = saved.maximized;
  plan.moved = plan.frame.left != saved.visible.left || plan.frame.top != saved.visible.top;
  return plan;
}

// ======================================== C86: the status band's segment text
// THE DEFECT (measured through UIA against the shipped window): the status band
// published ONE element named "共 3 项" (the breadcrumb's transient status line).
// 未加密 / 方法未知 / 字典未知 / 条目 / 选中 / 压缩率 were painted and reachable by
// nobody: they are D2D text, not child HWNDs, and the C80 tree never split the
// band into its segments.
//
// The fix is that the SEGMENT LIST becomes the accessible tree's children, in
// the order the band paints them. This function is the ONE spelling of a
// segment's text -- the painter, the accessibility tree and the suite all call
// it, so a label can never read one way on screen and another way in the tree.
// C105: the VALUE half of a segment, spelled once. The painter draws it as its
// own TextPrimary run (plan 3.2: "the leading word in TextSecondary, the value
// in TextPrimary") while the accessibility tree keeps reading the whole line
// through StatusSegmentLabel below -- both go through THIS function, so the
// painted value and the announced one cannot drift apart.
inline std::string StatusSegmentValueText(const StatusSegment& segment) {
  if (segment.id == StatusId::Divider) return {};
  // C81/A3: the canonical ratio string travels verbatim (it IS what the info
  // card and the list cell render); the integer path stays for scenes that only
  // set value.
  if (!segment.valueText.empty()) return segment.valueText;
  if (segment.value < 0) return {};
  std::string text = std::to_string(segment.value);
  if (segment.id == StatusId::Ratio) text.push_back('%');
  return text;
}

inline std::string StatusSegmentLabel(const StatusSegment& segment) {
  if (segment.id == StatusId::Divider) return {};
  std::string text(segment.text);
  // C86: this is the ONE place a band label is spelled, so the paint and the
  // accessibility tree cannot disagree.
  const std::string value = StatusSegmentValueText(segment);
  if (!value.empty()) {
    text.push_back(' ');
    text.append(value);
  }
  return text;
}

// ====================================== C86: the breadcrumb's COUNT segment
// THE DEFECT (reported, then measured): after drilling into a subfolder the
// breadcrumb still read "共 16 项" -- the archive's TOTAL, which the app wrote
// into the transient status line once, at open -- while the status band said
// 条目 2, the number of rows actually on screen. Two numbers, one window, and
// the wrong one sat next to the folder name.
//
// The count now describes the CURRENT FOLDER, and when the archive holds more
// entries than that folder (nested ones), the total is given in parentheses so
// the information is not lost either.
inline std::string BreadcrumbCountText(int folderCount, int archiveTotal, bool atRoot) {
  // D3 control: the count is the ARCHIVE's total again, which is what the app
  // wrote once at open -- the number the user reported seeing inside a folder.
  if (!kBreadcrumbCountsFolder) {
    return archiveTotal > 0 ? "共 " + std::to_string(archiveTotal) + " 项" : std::string();
  }
  if (folderCount < 0) return {};
  const std::string here = std::to_string(folderCount);
  if (archiveTotal <= folderCount) return "共 " + here + " 项";
  const std::string total = std::to_string(archiveTotal);
  return atRoot ? "共 " + here + " 项（全档 " + total + " 项）"
                : "本目录 " + here + " 项（共 " + total + " 项）";
}

}  // namespace prism::layout
