// PrismArchive -- card V1 layout invariants (DESIGN.md §2/§3).
//
// The main window's geometry is a pure function (PrismCore/WindowLayout.hpp),
// so the three P0 rules are assertable on any machine, with no window station:
//
//   * sibling rectangles never intersect (tolerance 0);
//   * every item rectangle that is fully visible sits inside the content area;
//   * a collapsed sidebar leaves NO ghost: the content is exactly the window
//     minus the 32 px of padding, and the reclaimed band is usable;
//   * grid columns == floor((contentInnerWidth + gap) / (minTile + gap)) >= 1,
//     computed for 800 / 1000 / 1200 / 1600 px wide.
//
// P0-2 (no skeleton cards): CellKindFor() is the single classification rule --
// directory, or thumbnail-capable extension WITH a decoded thumbnail. The
// suite pins both halves of that predicate.
//
// Negative controls (CONTRACTS D3, registered WILL_FAIL in
// Tests/suites/v1-layout.cmake):
// PRISM_V1_MUTANT_NO_THUMB_FALLBACK -> every non-folder item tiles; must be red
// PRISM_V1_MUTANT_IGNORE_SIDEBAR -> the rail reserves no width; must be red
#include <cstdio>
#include <string>
#include <vector>

#include "../Src/PrismCore/WindowLayout.hpp"
#include "mini_test.hpp"

#if defined(PRISM_V1_MUTANT_NO_THUMB_FALLBACK)
static const char* kMutantBanner = "V1 MUTANT ACTIVE: NO_THUMB_FALLBACK (every item tiles)";
#elif defined(PRISM_V1_MUTANT_IGNORE_SIDEBAR)
static const char* kMutantBanner = "V1 MUTANT ACTIVE: IGNORE_SIDEBAR (rail reserves 0)";
#else
static const char* kMutantBanner = nullptr;
#endif

namespace {

using prism::layout::Cell;
using prism::layout::CellKind;
using prism::layout::Layout;
using prism::layout::Rect;

constexpr int kWidths[] = {800, 1000, 1200, 1600};
constexpr int kHeights[] = {600, 700, 800, 1000};

bool Disjoint(const Rect& a, const Rect& b) { return !a.Intersects(b); }

int FullyVisibleCells(const Layout& layout, int itemCount, int scroll) {
  int fullyVisible = 0;
  const std::vector<Cell> cells = prism::layout::VisibleCells(layout, itemCount, scroll);
  for (const Cell& cell : cells) {
    if (cell.rect.top >= layout.contentInner.top &&
        cell.rect.bottom <= layout.contentInner.bottom) {
      ++fullyVisible;
    }
  }
  return fullyVisible;
}

}  // namespace
PRISM_TEST("v1.layout: the five bands tile the window and never overlap") {
  for (const int w : kWidths) {
    for (const int h : kHeights) {
      for (const bool sidebar : {false, true}) {
        for (const bool listMode : {false, true}) {
          const Layout l = prism::layout::Compute(w, h, sidebar, listMode);
          // Siblings are pairwise disjoint, tolerance 0.
          PRISM_CHECK(Disjoint(l.titleBar, l.actionIsland));
          PRISM_CHECK(Disjoint(l.titleBar, l.sidebar));
          PRISM_CHECK(Disjoint(l.titleBar, l.content));
          PRISM_CHECK(Disjoint(l.titleBar, l.statusBar));
          PRISM_CHECK(Disjoint(l.actionIsland, l.sidebar));
          PRISM_CHECK(Disjoint(l.actionIsland, l.content));
          PRISM_CHECK(Disjoint(l.actionIsland, l.statusBar));
          PRISM_CHECK(Disjoint(l.sidebar, l.content));
          PRISM_CHECK(Disjoint(l.sidebar, l.statusBar));
          PRISM_CHECK(Disjoint(l.content, l.statusBar));
          // ... and together they tile the client area with no gap.
          PRISM_CHECK_EQ(l.titleBar.top, 0);
          PRISM_CHECK_EQ(l.titleBar.bottom, 32);
          PRISM_CHECK_EQ(l.actionIsland.top, 32);
          PRISM_CHECK_EQ(l.actionIsland.bottom, 88);
          PRISM_CHECK_EQ(l.content.top, 88);
          PRISM_CHECK_EQ(l.statusBar.top, h - 28);
          PRISM_CHECK_EQ(l.statusBar.bottom, h);
          PRISM_CHECK_EQ(l.content.bottom, h - 28);
          PRISM_CHECK_EQ(l.content.right, w);
          PRISM_CHECK_EQ(l.statusBar.right, w);
          PRISM_CHECK_EQ(l.actionIsland.right, w);
          // The content begins exactly where the sidebar ends.
          if (sidebar) {
            PRISM_CHECK_EQ(l.sidebar.left, 0);
            PRISM_CHECK_EQ(l.sidebar.right, 220);
            PRISM_CHECK_EQ(l.content.left, l.sidebar.right);
          } else {
            PRISM_CHECK(l.sidebar.Empty());
            PRISM_CHECK_EQ(l.content.left, 0);
          }
        }
      }
    }
  }
}

PRISM_TEST("v1.layout: content padding is 16 on every side") {
  const Layout l = prism::layout::Compute(1600, 1000, true, false);
  PRISM_CHECK_EQ(l.contentInner.left, l.content.left + 16);
  PRISM_CHECK_EQ(l.contentInner.top, l.content.top + 16);
  PRISM_CHECK_EQ(l.contentInner.right, l.content.right - 16);
  PRISM_CHECK_EQ(l.contentInner.bottom, l.content.bottom - 16);
  PRISM_CHECK_EQ(l.contentInner.Width(), 1348);
  PRISM_CHECK_EQ(l.contentInner.Height(), 852);
}

PRISM_TEST("v1.layout: collapsing the rail leaves no ghost at x=220") {
  const Layout open = prism::layout::Compute(1600, 1000, true, false);
  const Layout shut = prism::layout::Compute(1600, 1000, false, false);

  // 1. The rail is gone, not merely transparent.
  PRISM_CHECK(shut.sidebar.Empty());
  PRISM_CHECK_EQ(shut.content.left, 0);
  PRISM_CHECK_EQ(shut.contentInner.left, 16);
  // 2. Content width is exactly window - 32 (the two 16 px paddings).
  PRISM_CHECK_EQ(shut.contentInner.Width(), 1600 - 32);
  PRISM_CHECK_EQ(open.contentInner.Width(), 1600 - 220 - 32);

  // 3. The reclaimed band is USABLE: tiles start at the padded left edge and
  //    the first row reaches past x=220 (the band the rail used to own).
  const Rect first = prism::layout::TileRect(shut, 0, 0);
  const Rect second = prism::layout::TileRect(shut, 1, 0);
  PRISM_CHECK_EQ(first.left, shut.contentInner.left);
  PRISM_CHECK(first.left < prism::layout::kSidebarWidth);
  PRISM_CHECK(second.right > prism::layout::kSidebarWidth);
  // ... and the window actually reflows to more columns without the rail.
  PRISM_CHECK(shut.columns > open.columns);

  // 4. The only difference is the rail band: the top bands, the status bar and
  //    the content's right/bottom edges are identical.
  PRISM_CHECK(open.titleBar == shut.titleBar);
  PRISM_CHECK(open.actionIsland == shut.actionIsland);
  PRISM_CHECK(open.statusBar == shut.statusBar);
  PRISM_CHECK_EQ(open.content.top, shut.content.top);
  PRISM_CHECK_EQ(open.content.bottom, shut.content.bottom);
  PRISM_CHECK_EQ(open.content.right, shut.content.right);
  PRISM_CHECK_EQ(open.content.left - shut.content.left, 220);
  PRISM_CHECK_EQ(open.contentInner.left - shut.contentInner.left, 220);
  PRISM_CHECK_EQ(open.contentInner.Width() + 220, shut.contentInner.Width());
}
PRISM_TEST("v1.layout: every fully visible item rect sits inside the content") {
  for (const int w : kWidths) {
    for (const int h : kHeights) {
      for (const bool sidebar : {false, true}) {
        const Layout l = prism::layout::Compute(w, h, sidebar, false);
        for (const int count : {1, 5, 17, 60}) {
          for (const int scroll : {0, 220, 5000}) {
            const std::vector<Cell> cells = prism::layout::VisibleCells(l, count, scroll);
            for (const Cell& cell : cells) {
              PRISM_CHECK(cell.index >= 0 && cell.index < count);
              // Anything emitted at least touches the content viewport...
              PRISM_CHECK(cell.rect.Intersects(l.contentInner));
              // ... and a fully visible cell is entirely inside it.
              if (cell.rect.top >= l.contentInner.top &&
                  cell.rect.bottom <= l.contentInner.bottom) {
                PRISM_CHECK(l.contentInner.Contains(cell.rect));
              }
              // Cells never overlap each other (tolerance 0).
              for (const Cell& other : cells) {
                if (&other == &cell) continue;
                if (other.index <= cell.index) continue;
                PRISM_CHECK(!cell.rect.Intersects(other.rect));
              }
            }
            // The path is reachable: at scroll 0 there is always a visible cell.
            if (scroll == 0) {
              PRISM_CHECK(!cells.empty());
              PRISM_CHECK(FullyVisibleCells(l, count, 0) > 0);
            }
          }
        }
      }
    }
  }
}

PRISM_TEST("v1.layout: columns follow floor((W+gap)/(minTile+gap)) at four widths") {
  struct Expect {
    int windowW;
    int sidebarVisibleColumns;
    int collapsedColumns;
  };
  // Hand-computed from the DESIGN.md formula with gap 12 / minTile 120.
  const Expect expected[] = {
      {800, 4, 5},     // inner 548 -> 560/132 = 4 ; 768 -> 780/132 = 5
      {1000, 5, 7},    // inner 748 -> 760/132 = 5 ; 968 -> 980/132 = 7
      {1200, 7, 8},    // inner 948 -> 960/132 = 7 ; 1168 -> 1180/132 = 8
      {1600, 10, 11},  // inner 1348 -> 1360/132 = 10 ; 1568 -> 1580/132 = 11
  };
  for (const Expect& e : expected) {
    const Layout open = prism::layout::Compute(e.windowW, 1000, true, false);
    const Layout shut = prism::layout::Compute(e.windowW, 1000, false, false);
    PRISM_CHECK_EQ(open.columns, e.sidebarVisibleColumns);
    PRISM_CHECK_EQ(shut.columns, e.collapsedColumns);
    // The formula itself, re-applied to the reported inner width.
    PRISM_CHECK_EQ(open.columns, prism::layout::ColumnsFor(open.contentInner.Width()));
    PRISM_CHECK_EQ(shut.columns, prism::layout::ColumnsFor(shut.contentInner.Width()));
    PRISM_CHECK(shut.columns >= 1);
  }
  // 1600 must reflow to MORE columns than 1200 (the P0-3 acceptance).
  const Layout wide = prism::layout::Compute(1600, 1000, false, false);
  const Layout narrow = prism::layout::Compute(1200, 1000, false, false);
  PRISM_CHECK(wide.columns > narrow.columns);
  // Degenerate widths still yield one column (never a divide by zero).
  for (int w = 0; w <= 300; ++w) {
    PRISM_CHECK(prism::layout::ColumnsFor(w) >= 1);
    PRISM_CHECK(prism::layout::Compute(w, 200, true, false).columns >= 1);
  }
}

PRISM_TEST("v1.layout: tile geometry stays inside the DESIGN tile bounds") {
  for (const int w : kWidths) {
    for (const bool sidebar : {false, true}) {
      const Layout l = prism::layout::Compute(w, 1000, sidebar, false);
      PRISM_CHECK(l.tileWidth >= prism::layout::kMinTileWidth);
      PRISM_CHECK(l.tileWidth <= prism::layout::kMaxTileWidth);
      PRISM_CHECK(l.cellHeight >= prism::layout::kMinCellHeight);
      PRISM_CHECK(l.cellHeight <=
                  prism::layout::kMaxTileWidth + prism::layout::kTileLabelHeight +
                      prism::layout::kIslandGap);
      // The last column does not overflow the padded content.
      const Rect last = prism::layout::TileRect(l, l.columns - 1, 0);
      PRISM_CHECK(last.right <= l.contentInner.right);
      PRISM_CHECK_EQ(last.right - last.left, l.tileWidth);
      // Adjacent columns are separated by exactly gap.
      if (l.columns >= 2) {
        const Rect a = prism::layout::TileRect(l, 0, 0);
        const Rect b = prism::layout::TileRect(l, 1, 0);
        PRISM_CHECK_EQ(b.left - a.right, prism::layout::kIslandGap);
      }
      // Row pitch == cellHeight.
      const Rect row0 = prism::layout::TileRect(l, 0, 0);
      const Rect row1 = prism::layout::TileRect(l, l.columns, 0);
      PRISM_CHECK_EQ(row1.top - row0.top, l.cellHeight);
    }
  }
}
PRISM_TEST("v1.layout: no thumbnail means a 32px row, never a skeleton tile") {
  using prism::PreviewKind;
  // Directories paint their folder tile (not a loading state).
  PRISM_CHECK_EQ(prism::layout::CellKindFor(true, PreviewKind::Directory, false),
                 CellKind::Tile);
  // Text/document types never tile, whether or not a thumb happens to exist.
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Text, false),
                 CellKind::Row);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Text, true),
                 CellKind::Row);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Pdf, true),
                 CellKind::Row);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::None, false),
                 CellKind::Row);
  // A thumbnail-capable image tiles ONLY once ThumbStore holds the pixels.
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Image, false),
                 CellKind::Row);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Image, true),
                 CellKind::Tile);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Media, false),
                 CellKind::Row);
  PRISM_CHECK_EQ(prism::layout::CellKindFor(false, PreviewKind::Media, true),
                 CellKind::Tile);
  // The fallback row is exactly 32 px and lives inside its cell.
  const Layout l = prism::layout::Compute(1600, 1000, true, false);
  const Rect cell = prism::layout::TileRect(l, 0, 0);
  const Rect row = prism::layout::FallbackRowRect(cell);
  PRISM_CHECK_EQ(row.Height(), 32);
  PRISM_CHECK_EQ(row.Width(), cell.Width());
  PRISM_CHECK(cell.Contains(row));
}

PRISM_TEST("v1.layout: list mode rows are 32px and stay inside the content") {
  const Layout l = prism::layout::Compute(1200, 800, true, true);
  PRISM_CHECK_EQ(l.columns, 1);
  const Rect header = prism::layout::ListHeaderRect(l);
  const Rect body = prism::layout::ListBodyRect(l);
  PRISM_CHECK_EQ(header.Height(), 32);
  PRISM_CHECK_EQ(header.top, l.contentInner.top);
  PRISM_CHECK_EQ(body.top, header.bottom);
  PRISM_CHECK_EQ(body.bottom, l.contentInner.bottom);
  PRISM_CHECK(l.contentInner.Contains(header));
  PRISM_CHECK(l.contentInner.Contains(body));
  PRISM_CHECK(!header.Intersects(body));
  // Rows: 32 px pitch, anchored to the body, never over the status bar.
  const Rect r0 = prism::layout::ListRowRect(l, 0, 0);
  const Rect r1 = prism::layout::ListRowRect(l, 1, 0);
  PRISM_CHECK_EQ(r0.Height(), 32);
  PRISM_CHECK_EQ(r1.top - r0.top, 32);
  PRISM_CHECK_EQ(r0.left, l.contentInner.left);
  PRISM_CHECK_EQ(r0.right, l.contentInner.right);
  PRISM_CHECK(!r0.Intersects(l.statusBar));
  // Hit test round-trips through the same geometry.
  const int mid = (r1.top + r1.bottom) / 2;
  PRISM_CHECK_EQ(prism::layout::ListRowAt(l, r1.left + 4, mid, 0), 1);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(l, l.contentInner.left - 4, mid, 0), -1);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(l, r1.left + 4, header.top + 2, 0), -1);
  // The visible window is never empty for a non-empty list.
  const prism::layout::RowRange range = prism::layout::VisibleListRows(l, 40, 0);
  PRISM_CHECK(!range.Empty());
  PRISM_CHECK_EQ(range.first, 0);
}

PRISM_TEST("v1.layout: floating chrome lives inside the bands it belongs to") {
  for (const int w : kWidths) {
    for (const bool sidebar : {false, true}) {
      const Layout l = prism::layout::Compute(w, 1000, sidebar, false);
      const Rect nav = prism::layout::NavPillRect(l);
      const Rect text = prism::layout::NavTextRect(l);
      PRISM_CHECK(l.actionIsland.Contains(nav));
      PRISM_CHECK(nav.Contains(text));
      PRISM_CHECK_EQ(nav.Height(), 44);
      PRISM_CHECK(!nav.Intersects(l.titleBar));
      PRISM_CHECK(!nav.Intersects(l.statusBar));

      const Rect card = prism::layout::QuickLookCardRect(l);
      const Rect capsule = prism::layout::ProgressCapsuleRect(l);
      PRISM_CHECK(l.contentInner.Contains(card));
      PRISM_CHECK(l.contentInner.Contains(capsule));
      PRISM_CHECK(!capsule.Intersects(l.statusBar));
      PRISM_CHECK(!card.Intersects(capsule));
      PRISM_CHECK(!card.Intersects(nav));
    }
  }
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);  // D3: the injection must be visible
  }
  const int result = prism::test::RunAll(argc, argv);
  if (result == 0) {
    std::puts("ALL LAYOUT-INVARIANTS CHECKS PASSED");  // D1: marker + exit 0
  }
  return result;
}
