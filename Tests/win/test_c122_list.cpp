// PrismArchive -- PrismWin
// C122: THE DEFAULT VIEW IS THE LIST, with Bandizip's four columns.
//
// THE DEFECT this suite is armed against: opening an archive shipped a GRID of
// large folder cards and two loose file rows (the audit's "another category of
// product"), the list had TWO columns (name + size) at 36 px, and the size cell
// ended on the clip edge -- "2.0 MB" rendered as "2.0 M" (design proposal
// §12.6 defect 1).
//
// WHAT IS ASSERTED (L = the card's own name for each):
//   L1  Scene{}.listMode is TRUE, and ListModeActive() still demands rows.
//   L2  名称 / 压缩后大小 / 原始大小 / 类型: four non-empty, pairwise disjoint
//       header cells whose widths are 120/120/100 and whose RIGHT edge is the
//       content edge minus the 20 DIP right padding; the header hit test
//       answers the same cells.
//   L3  Row height 26 DIP at 96 DPI, 22 under a 700 DIP window; paint
//       (ListRowRect) and the hit test (ListRowAt) use the same pitch.
//   L4  类型: .zip/.png/.md/.bin -> 压缩文件/图片/文本/二进制, a folder ->
//       文件夹, and .xyz -> the EMPTY string (not "未知"); the empty cell is
//       also empty ON PIXELS.
//   L5  The two numeric columns are right-aligned on their own baselines:
//       different-length values end on the same x (<= 1 px) and the value is
//       not clipped ("2.0 M" would be shorter than its own advance width).
//   L6  The switch survives: clicking a header sorts the VIEW, and View >
//       网格 / 列表 carries the grid back and forth (WM_COMMAND, not a guess).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27), no WILL_FAIL anywhere:
//   PRISM_C122_MUTANT_GRID_DEFAULT  scene.listMode defaults to false -> L1 red.
//   PRISM_C122_MUTANT_NO_TYPE       every type cell is "" -> L4 red.
// Both print "C122 MUTATION CAUGHT: <arm>" at the FIRST failing check and leave
// 0; the CTest entry's PASS_REGULAR_EXPRESSION is the verdict.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ListColumns.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/ListType.hpp"
#include "../../Src/PrismWin/Typography.hpp"

using Microsoft::WRL::ComPtr;
using prism::list::Column;
using prism::list::SortItem;
using prism::layout::ListColumnLayout;
using prism::layout::Rect;
using prism::win::gallery::GalleryRenderer;
using prism::win::gallery::GalleryWindow;

namespace {

constexpr UINT32 kWidth = 1920;
constexpr UINT32 kHeight = 1080;

#if defined(PRISM_C122_MUTANT_GRID_DEFAULT)
inline constexpr const char* kMutantBanner = "C122 MUTANT ACTIVE: grid_default";
#elif defined(PRISM_C122_MUTANT_NO_TYPE)
inline constexpr const char* kMutantBanner = "C122 MUTANT ACTIVE: no_type";
#else
inline constexpr const char* kMutantBanner = nullptr;
#endif

IWICImagingFactory* gWic = nullptr;
IDWriteFactory* gDwrite = nullptr;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

int MaxChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, std::abs(d));
  }
  return delta;
}

std::wstring Widen(std::string_view text) {
  if (text.empty()) return {};
  const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), wide);
  return out;
}

UINT32 CanvasPixel() {
  const prism::theme::Rgba token =
      prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(token.a) << 24) | (q(token.r) << 16) | (q(token.g) << 8) | q(token.b);
}

struct Harness {
  GalleryRenderer renderer;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;

  bool Open(UINT32 width, UINT32 height) {
    if (FAILED(renderer.Initialize())) return false;
    if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand,
                                  bitmap.ReleaseAndGetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
            bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer.CreateDeviceResources(target.Get()));
  }

  bool Paint(const GalleryRenderer::Scene& scene, Frame* out) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    if (FAILED(target->EndDraw())) return false;
    UINT bitmapWidth = 0;
    UINT bitmapHeight = 0;
    if (FAILED(bitmap->GetSize(&bitmapWidth, &bitmapHeight))) return false;
    WICRect rc{0, 0, static_cast<INT>(bitmapWidth), static_cast<INT>(bitmapHeight)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
      return false;
    }
    out->width = rc.Width;
    out->height = rc.Height;
    out->pixels.resize(static_cast<size_t>(out->width) * out->height);
    for (UINT32 y = 0; y < out->height; ++y) {
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * out->width,
                  data + static_cast<size_t>(y) * stride,
                  static_cast<size_t>(out->width) * 4);
    }
    return true;
  }
};

// ---------------------------------------------------------------- fixtures
SortItem Row(const char* name, std::uint64_t size, std::uint64_t packed) {
  SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  return item;
}

prism::layout::Layout ListLayoutAt(int width, int height) {
  return prism::layout::Compute(width, height, /*sidebarVisible=*/true, /*listMode=*/true);
}

struct SceneFixture {
  std::vector<SortItem> rows;
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  explicit SceneFixture(std::vector<SortItem> items)
      : rows(std::move(items)),
        layout(ListLayoutAt(static_cast<int>(kWidth), static_cast<int>(kHeight))) {
    scene.listMode = true;
    scene.listItems = &rows;
    scene.layout = &layout;
    scene.breadcrumb = L"fixture.zip";
    // C128: the band's first number is the current folder's file count.
    scene.status.fileCount = static_cast<int>(rows.size());
  }
};

// ------------------------------------------------------------------- L1
PRISM_TEST("c122.l1_default_view_is_the_list") {
  // Stated first, so the grid_default mutant reds on THIS line (and its CAUGHT
  // marker is armed on the same observable: a default Scene asking for the grid).
  PRISM_CHECK(GalleryRenderer::DefaultViewIsList());
  GalleryRenderer::Scene scene;
  PRISM_CHECK(scene.listMode);
  // The request is not the whole answer: a scene without list rows paints the
  // grid (this is what keeps every pre-C122 grid scene working).
  PRISM_CHECK(!GalleryRenderer::ListModeActive(scene));
  std::vector<SortItem> rows{Row("a.txt", 1024, 512)};
  scene.listItems = &rows;
  PRISM_CHECK(GalleryRenderer::ListModeActive(scene));
  scene.listMode = false;
  PRISM_CHECK(!GalleryRenderer::ListModeActive(scene));
  std::printf("C122 L1: default listMode=%d active-with-rows=%d\n",
              GalleryRenderer::DefaultViewIsList() ? 1 : 0,
              GalleryRenderer::ListModeActive(scene) ? 1 : 0);
}

// ------------------------------------------------------------------- L2
void AssertFourColumns(int width, int height, const char* tag) {
  const prism::layout::Layout layout = ListLayoutAt(width, height);
  const Rect header = prism::layout::ListHeaderRect(layout);
  const ListColumnLayout cols = prism::layout::ListColumnRects(layout);
  const Rect* cells[] = {&cols.name, &cols.packed, &cols.original, &cols.type};
  std::printf("C122 L2 %s: name=[%d,%d] packed=[%d,%d] original=[%d,%d] type=[%d,%d] "
              "header=[%d,%d]\n",
              tag, cols.name.left, cols.name.right, cols.packed.left, cols.packed.right,
              cols.original.left, cols.original.right, cols.type.left, cols.type.right,
              header.left, header.right);
  for (const Rect* cell : cells) {
    PRISM_CHECK(!cell->Empty());
    PRISM_CHECK_EQ(cell->top, header.top);
    PRISM_CHECK_EQ(cell->bottom, header.bottom);
    PRISM_CHECK(cell->left >= header.left);
    PRISM_CHECK(cell->right <= header.right);
  }
  // Pairwise disjoint: a one-pixel overlap is still an overlap (Rect's rule).
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) PRISM_CHECK(!cells[i]->Intersects(*cells[j]));
  }
  // Bandizip's order AND the design's widths.
  PRISM_CHECK(cols.name.right <= cols.packed.left);
  PRISM_CHECK(cols.packed.right <= cols.original.left);
  PRISM_CHECK(cols.original.right <= cols.type.left);
  PRISM_CHECK_EQ(cols.packed.Width(), prism::layout::kListColPackedWidth);
  PRISM_CHECK_EQ(cols.original.Width(), prism::layout::kListColOriginalWidth);
  PRISM_CHECK_EQ(cols.type.Width(), prism::layout::kListColTypeWidth);
  PRISM_CHECK_EQ(cols.packed.Width(), 120);
  PRISM_CHECK_EQ(cols.original.Width(), 120);
  PRISM_CHECK_EQ(cols.type.Width(), 100);
  // The right edge the defect was about: content edge - 20, not the clip edge.
  PRISM_CHECK_EQ(cols.type.right, layout.contentInner.right - prism::layout::kListRightPadding);
  PRISM_CHECK_EQ(prism::layout::kListRightPadding, 20);
  // The header hit test answers the SAME cells (a click cannot sort a column
  // the painter did not draw).
  for (const Column column : prism::list::kColumnOrder) {
    const Rect& cell = cols.For(column);
    const int x = (cell.left + cell.right) / 2;
    const prism::layout::ListColumnHit hit =
        prism::layout::ListColumnAt(layout, x, header.top + 5);
    PRISM_CHECK(hit.hit);
    PRISM_CHECK(hit.column == column);
    PRISM_CHECK(prism::list::ColumnIsNumeric(column) ==
                (column == Column::PackedSize || column == Column::OriginalSize));
  }
  // A gutter is not a column, and the body below the band is not a header.
  PRISM_CHECK(!prism::layout::ListColumnAt(layout, cols.name.right + 8, header.top + 5).hit);
  PRISM_CHECK(!prism::layout::ListColumnAt(layout, cols.type.left + 2, header.bottom + 5).hit);
}

PRISM_TEST("c122.l2_four_columns") {
  // Bandizip's own window and a common desktop size; both must hold.
  AssertFourColumns(1007, 742, "1007x742");
  AssertFourColumns(1280, 900, "1280x900");
  // The header words, in order.
  PRISM_CHECK(prism::list::ColumnTitle(Column::Name) == std::string_view("名称"));
  PRISM_CHECK(prism::list::ColumnTitle(Column::PackedSize) == std::string_view("压缩后大小"));
  PRISM_CHECK(prism::list::ColumnTitle(Column::OriginalSize) == std::string_view("原始大小"));
  PRISM_CHECK(prism::list::ColumnTitle(Column::Type) == std::string_view("类型"));
  PRISM_CHECK(prism::list::kColumnOrder[0] == Column::Name);
  PRISM_CHECK(prism::list::kColumnOrder[1] == Column::PackedSize);
  PRISM_CHECK(prism::list::kColumnOrder[2] == Column::OriginalSize);
  PRISM_CHECK(prism::list::kColumnOrder[3] == Column::Type);
}

// ------------------------------------------------------------------- L3
PRISM_TEST("c122.l3_row_height_26_dip") {
  const prism::layout::Layout regular = ListLayoutAt(1280, 900);
  const prism::layout::Layout compact = ListLayoutAt(1280, 660);
  const prism::layout::Layout atThreshold = ListLayoutAt(1280, 700);
  const prism::layout::Layout belowThreshold = ListLayoutAt(1280, 699);
  PRISM_CHECK_EQ(prism::layout::ListRowHeightFor(regular), 26);
  PRISM_CHECK_EQ(prism::layout::ListRowHeightFor(compact), 22);
  PRISM_CHECK_EQ(prism::layout::ListRowHeightFor(atThreshold), 26);
  PRISM_CHECK_EQ(prism::layout::ListRowHeightFor(belowThreshold), 22);
  PRISM_CHECK_EQ(prism::layout::kListRowHeight, 26);
  PRISM_CHECK_EQ(prism::layout::kListRowHeightCompact, 22);
  const Rect first = prism::layout::ListRowRect(regular, 0, 0);
  const Rect second = prism::layout::ListRowRect(regular, 1, 0);
  PRISM_CHECK_EQ(first.Height(), 26);
  PRISM_CHECK_EQ(second.Height(), 26);
  PRISM_CHECK_EQ(second.top, first.bottom);  // contiguous: no gap, no overlap
  const Rect compactRow = prism::layout::ListRowRect(compact, 1, 0);
  PRISM_CHECK_EQ(compactRow.Height(), 22);
  // Paint and hit test agree, in both tiers.
  const Rect body = prism::layout::ListBodyRect(regular);
  const int x = body.left + 40;
  PRISM_CHECK_EQ(prism::layout::ListRowAt(regular, x, body.top + 2 * 26 + 5, 0), 2);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(regular, x, body.top + 3 * 26, 0), 3);
  const Rect compactBody = prism::layout::ListBodyRect(compact);
  PRISM_CHECK_EQ(prism::layout::ListRowAt(compact, x, compactBody.top + 2 * 22 + 5, 0), 2);
  std::printf("C122 L3: regular=%d compact=%d threshold(700)=%d\n",
              prism::layout::ListRowHeightFor(regular), prism::layout::ListRowHeightFor(compact),
              prism::layout::ListRowHeightFor(atThreshold));
}

// ------------------------------------------------------------------- L4
PRISM_TEST("c122.l4_type_column_vocabulary") {
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("fixture.zip", false)),
                 std::string("压缩文件"));
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("shot.png", false)),
                 std::string("图片"));
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("readme.md", false)),
                 std::string("文本"));
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("data.bin", false)),
                 std::string("二进制"));
  // A PDF is a text/document class here: with five buckets, leaving it blank
  // would put an empty 类型 cell on the first screen (the type column is meant
  // to be READ, not to be a puzzle).
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("report.pdf", false)),
                 std::string("文本"));
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("photos", true)),
                 std::string("文件夹"));
  // Unknown is NOTHING, never "未知" (the audit's anti-pattern #7).
  PRISM_CHECK(prism::list::TypeLabelOf("mystery.xyz", false).empty());
  PRISM_CHECK(prism::list::TypeLabelOf("no-extension", false).empty());
  // The extension parser is case-insensitive and the dot rule is TypeTagOf's.
  PRISM_CHECK_EQ(std::string(prism::list::TypeLabelOf("UPPER.ZIP", false)),
                 std::string("压缩文件"));
  PRISM_CHECK(prism::list::TypeLabelOf(".gitignore", false).empty());
}

// The type TEXT reaches pixels, and an unknown extension leaves the cell empty.
PRISM_TEST("c122.l4_type_cell_is_painted") {
  std::vector<SortItem> rows{Row("a.zip", 4096, 2048), Row("b.png", 4096, 4000),
                             Row("c.md", 4096, 1024), Row("d.bin", 4096, 3900),
                             Row("e.xyz", 4096, 2000)};
  SceneFixture fixture(std::move(rows));
  Harness harness;
  if (!PRISM_CHECK(harness.Open(kWidth, kHeight))) return;
  Frame frame;
  if (!PRISM_CHECK(harness.Paint(fixture.scene, &frame))) return;
  if (frame.pixels.empty()) return;
  const Rect body = prism::layout::ListBodyRect(fixture.layout);
  const ListColumnLayout cols = prism::layout::ListColumnRects(fixture.layout);
  const UINT32 canvas = CanvasPixel();
  const int x0 = cols.type.left + 2;
  const int x1 = cols.type.right - 2;
  for (int row = 0; row < 5; ++row) {
    const Rect rect = prism::layout::ListRowRect(fixture.layout, row, 0);
    int ink = 0;
    for (int y = rect.top + 2; y < (std::min)(rect.bottom - 2, rect.top + 20); ++y) {
      for (int x = x0; x < x1; ++x) {
        if (!frame.In(x, y)) continue;
        if (MaxChannelDelta(frame.At(x, y), canvas) >= 50) ++ink;
      }
    }
    std::printf("C122 L4 pixels: type cell row=%d ink=%d (row band %d..%d)\n", row, ink,
                rect.top, rect.bottom);
    if (row < 4) {
      PRISM_CHECK(ink > 10);   // 压缩文件 / 图片 / 文本 / 二进制 all draw glyphs
    } else {
      PRISM_CHECK_EQ(ink, 0);  // .xyz: the cell stays empty, not "未知"
    }
  }
  (void)body;
}

// ------------------------------------------------------------------- L5
// The rightmost ink pixel of one numeric cell, inside the row's TEXT band (the
// ratio capsule lives lower in the row on purpose, see L5's note).
int NumericInk(const Frame& frame, const Rect& cell, int rowTop, int rowBottom, int* leftmost) {
  const UINT32 canvas = CanvasPixel();
  int right = -1;
  int left = -1;
  for (int y = rowTop + 2; y < (std::min)(rowBottom - 2, rowTop + 20); ++y) {
    for (int x = cell.left; x < cell.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (MaxChannelDelta(frame.At(x, y), canvas) < 50) continue;
      if (left < 0) left = x;
      right = x;
    }
  }
  if (leftmost != nullptr) *leftmost = left;
  return right;
}

float NumericAdvance(const wchar_t* text) {
  IDWriteTextFormat* format = nullptr;
  if (FAILED(prism::win::listtype::CreateNumericFormat(gDwrite, &format))) return -1.0f;
  IDWriteTextLayout* layout = nullptr;
  float width = -1.0f;
  if (SUCCEEDED(prism::win::listtype::CreateNumericLayout(
          gDwrite, format, text, static_cast<UINT32>(std::wcslen(text)), 500.0f, 24.0f,
          &layout))) {
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(layout->GetMetrics(&metrics))) width = metrics.widthIncludingTrailingWhitespace;
    layout->Release();
  }
  format->Release();
  return width;
}

PRISM_TEST("c122.l5_numeric_columns_right_aligned") {
  // Three DIFFERENT lengths per column: 1.0 KB / 8.0 KB / 128.0 MB.
  std::vector<SortItem> rows{Row("one.txt", 1024, 1024), Row("two.txt", 8192, 8192),
                             Row("three.txt", 134217728, 134217728)};
  SceneFixture fixture(std::move(rows));
  Harness harness;
  if (!PRISM_CHECK(harness.Open(kWidth, kHeight))) return;
  Frame frame;
  if (!PRISM_CHECK(harness.Paint(fixture.scene, &frame))) return;
  if (frame.pixels.empty()) return;
  const ListColumnLayout cols = prism::layout::ListColumnRects(fixture.layout);
  const UINT32 canvas = CanvasPixel();
  (void)canvas;
  const char* names[] = {"packed", "original"};
  const Rect cells[2] = {cols.packed, cols.original};
  const uint64_t values[3][2] = {{1024, 1024}, {8192, 8192}, {134217728, 134217728}};
  for (int column = 0; column < 2; ++column) {
    int rights[3] = {0, 0, 0};
    int lefts[3] = {0, 0, 0};
    for (int row = 0; row < 3; ++row) {
      const Rect rect = prism::layout::ListRowRect(fixture.layout, row, 0);
      rights[row] = NumericInk(frame, cells[column], rect.top, rect.bottom, &lefts[row]);
      const std::wstring text = Widen(prism::list::FormatBytes(values[row][column]));
      const float advance = NumericAdvance(text.c_str());
      const int inkWidth = rights[row] - lefts[row] + 1;
      std::printf("C122 L5 %s row=%d ink=%d..%d (width %d, advance %.2f, cell %d..%d)\n",
                  names[column], row, lefts[row], rights[row], inkWidth,
                  static_cast<double>(advance), cells[column].left, cells[column].right);
      PRISM_CHECK(rights[row] > 0);
      PRISM_CHECK(lefts[row] >= cells[column].left);
      // Not clipped on the right: the whole value is inside its column, and the
      // ink really ends ON the baseline (the defect cut "2.0 MB" to "2.0 M").
      PRISM_CHECK(rights[row] <= cells[column].right);
      PRISM_CHECK(rights[row] >= cells[column].right - 8);
      // The glyph run was not shortened: ink width within 4 px of the advance.
      PRISM_CHECK(advance > 0.0f);
      PRISM_CHECK(static_cast<float>(inkWidth) >= advance - 4.0f);
    }
    // One baseline: different lengths end on the same x, within 1 px.
    const int spread = (std::max)({rights[0], rights[1], rights[2]}) -
                       (std::min)({rights[0], rights[1], rights[2]});
    std::printf("C122 L5 %s: right edges %d/%d/%d spread=%d\n", names[column], rights[0],
                rights[1], rights[2], spread);
    PRISM_CHECK(spread <= 1);
  }
}

// ------------------------------------------------------------------- L6
PRISM_TEST("c122.l6_header_sorts_and_view_menu_switches") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive c122 list", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items;
  const char* paths[] = {"big.bin", "small.txt", "mid.png"};
  const uint64_t sizes[] = {900, 100, 500};
  for (int i = 0; i < 3; ++i) {
    prism::ArchiveItem item;
    item.path = paths[i];
    item.index = static_cast<std::uint32_t>(i);
    item.size = sizes[i];
    item.packSize = sizes[i] / 2;
    items.push_back(item);
  }
  window.SetItems(&items);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1200, 800));
  window.SetListMode(true);
  PRISM_CHECK(window.ListMode());
  // The default sort is 名称 ascending and the header says so.
  PRISM_CHECK(window.ListSort().column == Column::Name);
  PRISM_CHECK(window.ListSort().ascending);

  const prism::layout::Layout layout = window.LayoutForTest();
  const Rect header = prism::layout::ListHeaderRect(layout);
  const ListColumnLayout cols = prism::layout::ListColumnRects(layout);
  const int packedX = (cols.packed.left + cols.packed.right) / 2;
  // First click on 压缩后大小: select it, ascending.
  window.Dispatch(WM_LBUTTONDOWN, 0, MAKELPARAM(packedX, header.top + 6));
  PRISM_CHECK_EQ(window.SortSpec().keys.size(), size_t{1});
  PRISM_CHECK(window.SortSpec().keys[0].key == prism::list::SortKey::kPackedSize);
  PRISM_CHECK(window.SortSpec().keys[0].direction == prism::list::SortDirection::kAscending);
  PRISM_CHECK(window.ListSort().column == Column::PackedSize);
  // Second click: same column, flipped.
  window.Dispatch(WM_LBUTTONDOWN, 0, MAKELPARAM(packedX, header.top + 6));
  PRISM_CHECK(window.SortSpec().keys[0].direction == prism::list::SortDirection::kDescending);
  PRISM_CHECK(!window.ListSort().ascending);
  // The rows really re-ordered by packed size, descending (900/2, 500/2, 100/2).
  PRISM_CHECK_EQ(window.ListRowCount(), 3);
  if (window.ListRowCount() == 3) {
    PRISM_CHECK_EQ(window.ListRows()[0].name, std::string("big.bin"));
    PRISM_CHECK_EQ(window.ListRows()[2].name, std::string("small.txt"));
  }
  // A press in the BODY is not a header click.
  const Rect body = prism::layout::ListBodyRect(layout);
  const int rowsBefore = window.ListRowCount();
  window.Dispatch(WM_LBUTTONDOWN, 0, MAKELPARAM(body.left + 60, body.top + 4));
  PRISM_CHECK(window.ListSort().column == Column::PackedSize);
  PRISM_CHECK_EQ(window.ListRowCount(), rowsBefore);

  // View > 网格 / 列表 through the SAME WM_COMMAND route the menu posts.
  window.SetViewMenuLabels(L"\u89C6\u56FE(&V)", L"\u5217\u8868(&L)", L"\u7F51\u683C(&G)");
  window.Dispatch(WM_COMMAND, MAKEWPARAM(GalleryWindow::kMenuViewGrid, 0), 0);
  PRISM_CHECK(!window.ListMode());
  window.Dispatch(WM_COMMAND, MAKEWPARAM(GalleryWindow::kMenuViewList, 0), 0);
  PRISM_CHECK(window.ListMode());
  // The bar carries the View menu between 设置 and 帮助, with two items.
  HMENU bar = ::GetMenu(window.Hwnd());
  PRISM_CHECK(bar != nullptr);
  if (bar != nullptr) {
    PRISM_CHECK_EQ(::GetMenuItemCount(bar), 5);
    HMENU view = ::GetSubMenu(bar, 3);
    PRISM_CHECK(view != nullptr);
    if (view != nullptr) {
      PRISM_CHECK_EQ(::GetMenuItemCount(view), 2);
      PRISM_CHECK_EQ(::GetMenuItemID(view, 0), static_cast<UINT>(GalleryWindow::kMenuViewList));
      PRISM_CHECK_EQ(::GetMenuItemID(view, 1), static_cast<UINT>(GalleryWindow::kMenuViewGrid));
    }
    PRISM_CHECK_EQ(window.MenuBarItemCount(), 5);
    PRISM_CHECK(window.MenuBarItemText(3) == std::wstring(L"\u89C6\u56FE(&V)"));
  }
  // The header's REAL format is 11 px (design proposal 4.2 / 5.3).
  Harness harness;
  if (PRISM_CHECK(harness.Open(640, 480))) {
    IDWriteTextFormat* format = harness.renderer.ColumnHeaderFormat();
    PRISM_CHECK(format != nullptr);
    if (format != nullptr) PRISM_CHECK_EQ(format->GetFontSize(), 11.0f);
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) std::printf("%s\n", kMutantBanner);
#if defined(PRISM_C122_MUTANT_GRID_DEFAULT)
  // Armed on the OBSERVABLE defect, not on the macro: a freshly built Scene
  // asks for the grid.
  prism::test::SetMutationVerdict("C122 MUTATION CAUGHT: grid_default", [] {
    GalleryRenderer::Scene probe;
    return !probe.listMode;
  });
#elif defined(PRISM_C122_MUTANT_NO_TYPE)
  // Armed on a known archive extension losing its label -- the injection firing.
  prism::test::SetMutationVerdict("C122 MUTATION CAUGHT: no_type", [] {
    return prism::list::TypeLabelOf("probe.zip", false).empty();
  });
#else
  std::puts("C122 MUTANT: none (clean build)");
#endif

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the list cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the list cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&gDwrite)))) {
    std::puts("FAIL: DWrite unavailable -- the list cannot be verified");
    return 1;
  }
  const int failures = prism::test::RunAll(argc, argv);
  gDwrite->Release();
  if (failures != 0) {
    std::puts("C122 LIST CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C122 LIST CHECKS PASSED");
  return 0;
}
