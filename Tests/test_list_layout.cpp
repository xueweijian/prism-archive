// TDD: ListLayout -- the main-window file list mode (blueprint ch. 3.2):
// virtualized rows, the four columns (名称/原始大小/压缩后/压缩率), semantic
// micro-capsule ratio bars, tnum-friendly byte formatting, folders-first
// ordering. Pure decisions, no Windows -- the renderer consumes them.
#include "ListLayout.hpp"
#include "mini_test.hpp"

#include <string>
#include <vector>

namespace {

using prism::list::ColumnLayout;
using prism::list::FolderFirstOrder;
using prism::list::FormatBytes;
using prism::list::RatioBar;
using prism::list::RowWindow;
using prism::list::SortItem;

PRISM_TEST("row virtualization returns the visible window with overscan") {
  const auto w = RowWindow(0, 720, 1000);
  PRISM_CHECK_EQ(w.first, 0u);
  PRISM_CHECK(w.last >= 720 / 36 - 1);  // at least the fully visible rows
  PRISM_CHECK(w.last - w.first <= 720 / 36 + 6);
}

PRISM_TEST("row window clamps at the end of the list") {
  const auto w = RowWindow(39'000, 720, 1000);  // scrolled past the end
  PRISM_CHECK_EQ(w.first, 999u);
  PRISM_CHECK_EQ(w.last, 999u);  // never past the last row
}

PRISM_TEST("ratio bar math: media files map to teal with exact percent") {
  SortItem item{};
  item.isDir = false;
  item.size = 1000;
  item.packedSize = 770;
  item.kind = prism::PreviewKind::Image;
  const auto bar = RatioBar(item);
  if (!PRISM_CHECK(bar.has_value())) return;
  PRISM_CHECK_EQ(bar->percent, 77);
  PRISM_CHECK_EQ(bar->color, prism::list::BarColor::Teal);
}

PRISM_TEST("folders and zero-size rows have no ratio bar; encryption wins") {
  SortItem dir{};
  dir.isDir = true;
  PRISM_CHECK(!RatioBar(dir).has_value());

  SortItem encrypted{};
  encrypted.size = 100;
  encrypted.packedSize = 90;
  encrypted.encrypted = true;
  encrypted.kind = prism::PreviewKind::Image;  // amber overrides teal
  const auto bar = RatioBar(encrypted);
  if (!PRISM_CHECK(bar.has_value())) return;
  PRISM_CHECK_EQ(bar->color, prism::list::BarColor::Amber);
}

PRISM_TEST("semantic colors: media teal, code/doc blue, dense text green") {
  SortItem media{};
  media.size = 100;
  media.packedSize = 97;
  media.kind = prism::PreviewKind::Media;
  PRISM_CHECK_EQ(RatioBar(media)->color, prism::list::BarColor::Teal);

  SortItem doc{};
  doc.size = 100;
  doc.packedSize = 26;
  doc.kind = prism::PreviewKind::Pdf;
  PRISM_CHECK_EQ(RatioBar(doc)->color, prism::list::BarColor::Blue);

  SortItem dense{};
  dense.size = 100;
  dense.packedSize = 20;
  dense.kind = prism::PreviewKind::None;
  PRISM_CHECK_EQ(RatioBar(dense)->color, prism::list::BarColor::Green);
}

PRISM_TEST("format bytes produces tnum-friendly fixed-scale strings") {
  PRISM_CHECK_EQ(FormatBytes(0), std::string("0 B"));
  PRISM_CHECK_EQ(FormatBytes(512), std::string("512 B"));
  PRISM_CHECK_EQ(FormatBytes(18 * 1024 * 1024), std::string("18.0 MB"));
  PRISM_CHECK_EQ(FormatBytes(142 * 1024 * 1024), std::string("142.0 MB"));
  PRISM_CHECK_EQ(FormatBytes(2ull * 1024 * 1024 * 1024), std::string("2.0 GB"));
  PRISM_CHECK_EQ(FormatBytes(4 * 1024 + 512), std::string("4.5 KB"));
}

PRISM_TEST("folders sort first, then names ascend; files after folders") {
  std::vector<SortItem> items(4);
  items[0].name = "zeta.txt";
  items[1].name = "02_UI_Kit";
  items[1].isDir = true;
  items[2].name = "01_Brand";
  items[2].isDir = true;
  items[3].name = "alpha.txt";
  FolderFirstOrder(items);
  PRISM_CHECK_EQ(items[0].name, std::string("01_Brand"));
  PRISM_CHECK_EQ(items[1].name, std::string("02_UI_Kit"));
  PRISM_CHECK_EQ(items[2].name, std::string("alpha.txt"));
  PRISM_CHECK_EQ(items[3].name, std::string("zeta.txt"));
}

PRISM_TEST("column layout: fixed x positions, name takes the flex space") {
  const ColumnLayout cols = ColumnLayout::Default(1200);
  PRISM_CHECK_EQ(cols.sizeX(1200), 1200u - (110u + 110u + 130u));
  PRISM_CHECK_EQ(cols.packedX(1200), 1200u - (110u + 130u));
  PRISM_CHECK_EQ(cols.ratioX(1200), 1200u - 130u);
}

PRISM_TEST("hit test maps a y coordinate to a row index") {
  PRISM_CHECK_EQ(prism::list::RowAt(0, 0).value_or(9999u), 0u);
  PRISM_CHECK_EQ(prism::list::RowAt(72, 0).value_or(9999u), 2u);
  PRISM_CHECK_EQ(prism::list::RowAt(72, 1080).value_or(9999u), 32u);  // 1080/36 + 2
  PRISM_CHECK_EQ(prism::list::RowAtClamped(100'000, 0, 500), 499u);
}

}  // namespace

PRISM_TEST_MAIN()
