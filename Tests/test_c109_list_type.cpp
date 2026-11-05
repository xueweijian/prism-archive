// PrismArchive -- PrismCore
// C109: the LIST's vertical rhythm and column geometry (the parts that are pure
// decisions and therefore run on every platform, including the Linux lane).
//
// What this pins:
//   * kListRowHeight is 36 everywhere it is consumed -- paint (ListRowRect),
//     virtualization (VisibleListRows) and the window hit test (ListRowAt) --
//     and the flow/grid fallback band is deliberately still 32 (decoupled, so
//     C109's density change cannot silently reflow V4's grid).
//   * a click on the THIRD row maps to row index 2, first pixel and last
//     pixel, with and without a scroll offset.
//   * adjacent numeric columns are separated by at least 16 px and each column
//     has ONE right-edge baseline (the renderer aligns trailing text to it).
//
// The Windows arm (Tests/win/test_c109_list_type.cpp) measures the advances and
// the rendered right edges; this file cannot, because it must stay free of
// <windows.h> (preflight check_arch_no_windows).
#include <cstdint>
#include <string>

#include "mini_test.hpp"
#include "../Src/PrismCore/ListLayout.hpp"
#include "../Src/PrismCore/WindowLayout.hpp"

using prism::layout::Layout;
using prism::layout::Rect;

PRISM_TEST("c109.row_height: 36 px, and paint/virtualization/hit test share it") {
  PRISM_CHECK_EQ(prism::layout::kListRowHeight, 36);
  PRISM_CHECK_EQ(prism::list::kRowHeight, 36u);
  // The GRID's fallback band is a different surface: C109 must not move it.
  PRISM_CHECK_EQ(prism::layout::kFlowRowHeight, 32);

  const Layout layout = prism::layout::Compute(1200, 800, /*sidebarVisible=*/true,
                                               /*listMode=*/true);
  const Rect body = prism::layout::ListBodyRect(layout);
  PRISM_CHECK(body.Height() > 4 * 36);
  for (int i = 0; i < 4; ++i) {
    const Rect row = prism::layout::ListRowRect(layout, i, 0);
    PRISM_CHECK_EQ(row.Height(), 36);
    PRISM_CHECK_EQ(row.top, body.top + i * 36);
    PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, row.left + 4, row.top, 0), i);
    PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, row.left + 4, row.top + 35, 0), i);
  }

  // The card's third-row case, stated as a click: the 36 px pitch puts the
  // third row at body.top + 2*36. A 32 px pitch (or an off-by-one) lands on 2
  // only for a few pixels; these four points span the whole row.
  const int x = body.left + 40;
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * 36 + 1, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * 36 + 17, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * 36 + 35, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 3 * 36 + 1, 0), 3);

  // A scroll of exactly two rows moves row 2 to the top pixel of the body.
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 1, 2 * 36), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 1, 2 * 36 + 36), 3);

  // Virtualization uses the same pitch: the last visible index is derived from
  // body.Height()/36, and its row rect still fits inside the body.
  const prism::layout::RowRange window = prism::layout::VisibleListRows(layout, 200, 0);
  PRISM_CHECK_EQ(window.first, 0);
  const int expectedLast = body.Height() / 36 + 1;  // + kOverscan(2) - 1
  PRISM_CHECK_EQ(window.last, expectedLast);
  PRISM_CHECK(prism::layout::ListRowRect(layout, window.last, 0).bottom >= body.bottom);
}

PRISM_TEST("c109.columns: >=16 px gutters and one baseline per column") {
  PRISM_CHECK(prism::list::kColumnGutter >= 16);
  const std::uint32_t w = 1200;
  const prism::list::ColumnLayout cols;

  // The gutters are BETWEEN box edges: each Left() to the previous Right().
  PRISM_CHECK_EQ(cols.sizeLeft(w) - cols.nameRight(w), prism::list::kColumnGutter);
  PRISM_CHECK_EQ(cols.packedLeft(w) - cols.sizeRight(w), prism::list::kColumnGutter);
  PRISM_CHECK_EQ(cols.ratioLeft(w) - cols.packedRight(w), prism::list::kColumnGutter);

  // One baseline per column, ordered left to right, all inside the canvas.
  PRISM_CHECK(cols.nameRight(w) < cols.sizeRight(w));
  PRISM_CHECK(cols.sizeRight(w) < cols.packedRight(w));
  PRISM_CHECK(cols.packedRight(w) < cols.ratioRight(w));
  PRISM_CHECK(cols.ratioRight(w) <= w);
  PRISM_CHECK_EQ(cols.ratioRight(w), w - prism::list::kColumnRightMargin);

  // A persisted C27 geometry with narrower columns keeps the same gutters.
  prism::list::ColumnLayout custom;
  custom.sizeW = 88;
  custom.packedW = 88;
  custom.ratioW = 110;
  PRISM_CHECK_EQ(custom.sizeLeft(1184) - custom.nameRight(1184), 16u);
  PRISM_CHECK_EQ(custom.packedLeft(1184) - custom.sizeRight(1184), 16u);
  PRISM_CHECK_EQ(custom.ratioLeft(1184) - custom.packedRight(1184), 16u);

  // The three strings assertion (2) renders really do differ in length, so a
  // right-edge comparison is not comparing identical strings.
  PRISM_CHECK_EQ(prism::list::FormatBytes(6451), std::string("6.3 KB"));
  PRISM_CHECK_EQ(prism::list::FormatBytes(11469), std::string("11.2 KB"));
  PRISM_CHECK_EQ(prism::list::FormatBytes(129394278), std::string("123.4 MB"));
}

PRISM_TEST_MAIN()
