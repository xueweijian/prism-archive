// PrismArchive -- PrismCore
// C109: the LIST's vertical rhythm and column geometry (the parts that are pure
// decisions and therefore run on every platform, including the Linux lane).
//
// What this pins:
//   * kListRowHeight is ONE number everywhere it is consumed -- paint
//     (ListRowRect), virtualization (VisibleListRows) and the window hit test
//     (ListRowAt) -- and the flow/grid fallback band is deliberately still 32
//     (decoupled, so a list-density change cannot silently reflow V4's grid).
//     C122 moved the number to 26 DIP (with a 22 DIP compact tier under 700 DIP
//     of window height); the INVARIANT this file pins -- all four consumers
//     share the pitch -- is what the number change must not break.
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

PRISM_TEST("c109.row_height: one pitch for paint/virtualization/hit test") {
  // C122 set the number (26 DIP; 22 under a 700 DIP window). This test's subject
  // is the INVARIANT: every consumer reads the same function.
  const int pitch = prism::layout::kListRowHeight;
  PRISM_CHECK_EQ(pitch, 26);
  PRISM_CHECK_EQ(prism::layout::kListRowHeightCompact, 22);
  // The GRID's fallback band is a different surface: a list-density change must
  // not move it.
  PRISM_CHECK_EQ(prism::layout::kFlowRowHeight, 32);

  const Layout layout = prism::layout::Compute(1200, 800, /*sidebarVisible=*/true,
                                               /*listMode=*/true);
  PRISM_CHECK_EQ(prism::layout::ListRowHeightFor(layout), pitch);
  const Rect body = prism::layout::ListBodyRect(layout);
  PRISM_CHECK(body.Height() > 4 * pitch);
  for (int i = 0; i < 4; ++i) {
    const Rect row = prism::layout::ListRowRect(layout, i, 0);
    PRISM_CHECK_EQ(row.Height(), pitch);
    PRISM_CHECK_EQ(row.top, body.top + i * pitch);
    PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, row.left + 4, row.top, 0), i);
    PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, row.left + 4, row.top + pitch - 1, 0), i);
  }

  // The card's third-row case, stated as a click: the pitch puts the third row
  // at body.top + 2*pitch. A different pitch (or an off-by-one) lands on 2 only
  // for a few pixels; these four points span the whole row.
  const int x = body.left + 40;
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * pitch + 1, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * pitch + 17, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 2 * pitch + pitch - 1, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 3 * pitch + 1, 0), 3);

  // A scroll of exactly two rows moves row 2 to the top pixel of the body.
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 1, 2 * pitch), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, x, body.top + 1, 2 * pitch + pitch), 3);

  // Virtualization uses the same pitch: the last visible index is derived from
  // body.Height()/pitch, and its row rect still fits inside the body.
  const prism::layout::RowRange window = prism::layout::VisibleListRows(layout, 200, 0);
  PRISM_CHECK_EQ(window.first, 0);
  const int expectedLast = body.Height() / pitch + 1;  // + kOverscan(2) - 1
  PRISM_CHECK_EQ(window.last, expectedLast);
  PRISM_CHECK(prism::layout::ListRowRect(layout, window.last, 0).bottom >= body.bottom);
}

PRISM_TEST("c109.columns: >=16 px gutters and one baseline per numeric column") {
  // C122: the list paints FOUR columns whose geometry lives in WindowLayout
  // (ListColumnRects), so the gutter/baseline contract is asserted on THAT
  // authority. The pre-C122 prism::list::ColumnLayout is now only the persisted
  // C27 value -- it no longer describes a painted cell -- and
  // core.test_list_layout still covers its own arithmetic.
  const Layout layout = prism::layout::Compute(1200, 800, /*sidebarVisible=*/true,
                                               /*listMode=*/true);
  const prism::layout::ListColumnLayout cols = prism::layout::ListColumnRects(layout);
  PRISM_CHECK(prism::layout::kListColumnGutter >= 16);

  // The gutters are BETWEEN box edges: each Left() to the previous Right().
  PRISM_CHECK_EQ(cols.packed.left - cols.name.right, prism::layout::kListColumnGutter);
  PRISM_CHECK_EQ(cols.original.left - cols.packed.right, prism::layout::kListColumnGutter);
  PRISM_CHECK_EQ(cols.type.left - cols.original.right, prism::layout::kListColumnGutter);

  // One baseline per numeric column, ordered left to right, all inside the
  // canvas. The last cell ends on the 20 DIP right padding, never on the clip
  // edge (C122's "2.0 MB shipped as 2.0 M" defect).
  PRISM_CHECK(cols.name.right < cols.packed.right);
  PRISM_CHECK(cols.packed.right < cols.original.right);
  PRISM_CHECK(cols.original.right < cols.type.right);
  PRISM_CHECK(cols.type.right <= layout.contentInner.right);
  PRISM_CHECK_EQ(cols.type.right, layout.contentInner.right - prism::layout::kListRightPadding);

  // The three strings assertion (2) renders really do differ in length, so a
  // right-edge comparison is not comparing identical strings.
  PRISM_CHECK_EQ(prism::list::FormatBytes(6451), std::string("6.3 KB"));
  PRISM_CHECK_EQ(prism::list::FormatBytes(11469), std::string("11.2 KB"));
  PRISM_CHECK_EQ(prism::list::FormatBytes(129394278), std::string("123.4 MB"));
}

PRISM_TEST_MAIN()
