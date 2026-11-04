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


// The runtime proof each mutant is REALLY armed (D3: a macro that stopped
// matching would leave the mutant green and its WILL_FAIL entry red forever).
inline constexpr std::string_view ActiveMutation() {
  if (!kRowsSpanContent) return "cell-row";
  if (!kRailInGridMode) return "grid-no-sidebar";
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
inline constexpr int kListRowHeight = 32;
inline constexpr int kListIndent = 20;
inline constexpr int kMinTileWidth = 120;  // DESIGN.md §2: minimum column width
inline constexpr int kMaxTileWidth = 192;
inline constexpr int kTileLabelHeight = 30;  // name plate under a photo tile
inline constexpr int kMinCellHeight = 220;
inline constexpr int kFallbackRowHeight = 32;  // P0-2: the row a tile-less item falls back to
inline constexpr int kNavPillHeight = 44;
inline constexpr int kNavPillMaxWidth = 560;
inline constexpr int kNavTextInset = 24;
inline constexpr int kProgressCapsuleWidth = 260;
inline constexpr int kProgressCapsuleHeight = 46;
inline constexpr int kQuickLookWidth = 380;
inline constexpr int kQuickLookHeight = 500;
// V4: a fallback row is a BAND across the content, not a cell inside the grid.
// A 32 px band is what makes 19 rows fit in 800 px (DESIGN.md §2 "列表：行高
// 32"); the cell-sized row V1 shipped was a 220 px pill with dead air to its
// right -- the exact defect the v2 screenshot showed.
inline constexpr int kFlowRowHeight = kListRowHeight;
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

struct Layout {
  int windowW = 0;
  int windowH = 0;
  bool sidebarVisible = false;
  bool listMode = false;

  // Siblings: these six tile the window and are pairwise disjoint.
  Rect titleBar;
  // C60: empty (and tabBarVisible false) when no archive is open.
  Rect tabBar;
  bool tabBarVisible = false;
  Rect actionIsland;
  Rect sidebar;      // empty when collapsed
  Rect content;      // starts at sidebar.right
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

// The complete geometry of the main window for one client size.
inline Layout Compute(int windowW, int windowH, bool sidebarVisible, bool listMode,
                      bool tabBarVisible = false) {
  Layout l;
  l.windowW = (std::max)(0, windowW);
  l.windowH = (std::max)(0, windowH);
  l.sidebarVisible = sidebarVisible;
  l.listMode = listMode;
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
  const int contentTop = l.actionIsland.bottom;
  const int statusTop = (std::max)(contentTop, h - kStatusBarHeight);

  // The rail is PAINTED whenever it is visible, but it only RESERVES width when
  // the rule is armed. The mutant therefore produces two intersecting siblings
  // instead of silently moving the content.
  const bool reserve = sidebarVisible && kSidebarReservesWidth;
  const int railWidth = sidebarVisible ? (std::min)(kSidebarWidth, w) : 0;
  l.sidebar = sidebarVisible ? Rect{0, contentTop, railWidth, statusTop} : Rect{};
  l.content = Rect{reserve ? railWidth : 0, contentTop, w, statusTop};
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
  // text + ' ' + valueText (no integer append); it travels as a view into
  // StatusInfo::ratioView, which the scene owns for the whole frame.
  std::string_view valueText;
};

inline constexpr int kStatusPad = 12;
inline constexpr int kStatusGap = 24;
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
  if (gap >= 2 * kStatusGap) {
    const int dividerX = leftEnd + gap / 2;
    out.push_back(StatusSegment{StatusId::Divider,
                                Rect{dividerX, top, dividerX + 1, bottom}, {}, -1, {}});
  }
  const std::string_view rightTexts[] = {ItemCountText(info), SelectedCountText(info),
                                         RatioText(info)};
  const int rightValues[] = {info.itemCount, info.selectedCount,
                             (info.compressing ? -1 : info.ratioPercent)};
  const std::string_view rightValueTexts[] = {{}, {},
      (info.compressing ? std::string_view{} : std::string_view{info.ratioView})};
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

inline Rect ListRowRect(const Layout& layout, int row, int scrollPx) {
  const Rect body = ListBodyRect(layout);
  const int y = body.top + row * kListRowHeight - scrollPx;
  return Rect{body.left, y, body.right, y + kListRowHeight};
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
  constexpr int kOverscan = 2;
  range.first = (std::max)(0, scrollPx / kListRowHeight - kOverscan);
  range.last = (std::min)(rowCount - 1,
                          (scrollPx + body.Height()) / kListRowHeight + kOverscan - 1);
  if (range.last < range.first) range.first = range.last;
  return range;
}

// Hit test: a client point inside the body maps to a row, else -1.
inline int ListRowAt(const Layout& layout, int x, int y, int scrollPx) {
  const Rect body = ListBodyRect(layout);
  if (body.Empty() || x < body.left || x >= body.right) return -1;
  if (y < body.top || y >= body.bottom) return -1;
  return (y - body.top + scrollPx) / kListRowHeight;
}

// ------------------------------------------------------------- floating chrome
// The breadcrumb pill lives INSIDE the action band (44 px, the blueprint's nav
// island height), so it can never overlap the title bar or the status bar.
inline Rect NavPillRect(const Layout& layout) {
  const int available = (std::max)(0, layout.actionIsland.Width() - 2 * kContentPadding);
  const int width = (std::min)(kNavPillMaxWidth, available);
  const int height = (std::min)(kNavPillHeight, layout.actionIsland.Height());
  const int top = layout.actionIsland.top + (layout.actionIsland.Height() - height) / 2;
  return Rect{kContentPadding, top, kContentPadding + width, top + height};
}

inline Rect NavTextRect(const Layout& layout) {
  const Rect pill = NavPillRect(layout);
  return Rect{pill.left + kNavTextInset, pill.top + 10, pill.right - kNavTextInset,
              pill.bottom - 10};
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

}  // namespace prism::layout
