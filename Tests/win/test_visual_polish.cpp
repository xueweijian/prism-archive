// PrismArchive -- card V4: the five things the v2 screenshot still got wrong.
//
// The screenshot (E:\Temp\ui\prism_v2.png, 1200x800, alpha.7z) was measured, not
// eyeballed, and each defect became an assertion here:
//
//   1. TEXT ITEMS IN GRID CELLS. V1 kept the 220 px cell so the old click test
//      would not move; the result was a small capsule with dead air to its
//      right. The grid is now a FLOW (WindowLayout::FlowCells): a tile-less
//      item is a 32 px BAND spanning content.left .. content.right, contiguous
//      with its neighbours. The grid holds ONLY directories and images whose
//      thumbnail the store really has.
//   2. ICON/LABEL MISALIGNMENT. Both boxes now come from the model
//      (ActionBarModel::IconBoxRect / LabelBoxRect) and are pinned to one fixed
//      offset; the label is ellipsized (U+2026), never clipped mid-glyph.
//   3. SIDEBAR MISSING IN GRID MODE. DESIGN.md §2 lists the rail as a BAND, not
//      list-mode chrome. Grid layout reserves 220 px; an explicit collapse gives
//      it back (0 px, content moves left by exactly 220).
//   4. EMPTY STATUS BAR. Four segments (加密/方法/字典/压缩率) with the four
//      states DESIGN.md §3 lists: 未加密 / 已加密 / 未知方法 / 压缩中.
//   5. DENSITY. 1200x800 list mode must show >= 18 rows at the 32 px pitch
//      (the layout yields 19); three text items occupy 96 px, not 800.
//
// Negative controls (CONTRACTS D3 + D27, same source, compile-time switch):
//   PRISM_V4_MUTANT_CELL_ROW         rows go back INSIDE their cells (V1 shape)
//   PRISM_V4_MUTANT_GRID_NO_SIDEBAR  the rail becomes list-mode chrome again
// Each arms SetMutationVerdict(prefix, predicate): the marker is printed at the
// FIRST failed assertion and the process exits 0, so the CTest entry is green
// only while the suite really catches the injected defect (D26: no WILL_FAIL).
//
// /WX- is the project's Windows-test convention (B9): the SDK headers are not
// /W4 clean.
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
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::layout::CellKind;
using prism::layout::FlowCell;
using prism::layout::FlowItem;
using prism::layout::Layout;
using prism::layout::Rect;

namespace {

constexpr UINT32 kWidth = 1200;
constexpr UINT32 kHeight = 800;

IWICImagingFactory* gWic = nullptr;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
  int Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3;
  }
};

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

struct Counters {
  int statusSegments = 0;
  int sidebarPanels = 0;
  int rowCells = 0;
  int tileCells = 0;
  int actionLabels = 0;
  int actionClipped = 0;
};

struct Render {
  bool ok = false;
  Frame frame;
  Counters counters;
  ComPtr<ID2D1RenderTarget> target;
};

prism::win::gallery::GalleryRenderer::Scene GridScene(std::vector<prism::ArchiveItem>* items,
                                                      prism::gallery::ThumbStore* thumbs) {
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.items = items;
  scene.thumbs = thumbs;
  scene.breadcrumb = L"alpha.7z  |  3 items";
  return scene;
}

bool Render64(IWICImagingFactory* wic, const prism::win::gallery::GalleryRenderer::Scene& scene,
              Render* out) {
  auto renderer = std::make_unique<prism::win::gallery::GalleryRenderer>();
  if (FAILED(renderer->Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer->Factory()->CreateWicBitmapRenderTarget(
          bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer->CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer->Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  out->frame.width = kWidth;
  out->frame.height = kHeight;
  out->frame.pixels.resize(static_cast<size_t>(kWidth) * kHeight);
  for (UINT32 y = 0; y < kHeight; ++y) {
    std::memcpy(out->frame.pixels.data() + static_cast<size_t>(y) * kWidth,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(kWidth) * 4);
  }
  out->counters.statusSegments = renderer->dbgStatusSegments;
  out->counters.sidebarPanels = renderer->dbgSidebarPanels;
  out->counters.rowCells = renderer->dbgRowCells;
  out->counters.tileCells = renderer->dbgTileCells;
  out->counters.actionLabels = renderer->ActionLabelsPainted();
  out->counters.actionClipped = renderer->ActionLabelsClipped();
  out->target = target;
  out->ok = true;
  return true;
}

// UTF-8 -> UTF-16, the same conversion the painter's Utf8() performs.
std::wstring Wide(std::string_view text) {
  if (text.empty()) return {};
  const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                       nullptr, 0);
  std::wstring out(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), wide);
  return out;
}

// Ink = pixels that differ from the band's own background.
int InkPixels(const Frame& frame, const Rect& rect, UINT32 fill) {
  int ink = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (ColorDistance(frame.At(x, y), fill) > 24) ++ink;
    }
  }
  return ink;
}

struct InkBox {
  int left = 1 << 20, top = 1 << 20, right = -1, bottom = -1;
  int count = 0;
  bool Empty() const { return count == 0; }
  int CentreX() const { return (left + right) / 2; }
  int CentreY() const { return (top + bottom) / 2; }
};

InkBox InkBoundingBox(const Frame& frame, const Rect& box) {
  InkBox out;
  const UINT32 fill = frame.At((std::max)(0, box.left - 3), box.top);
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (ColorDistance(frame.At(x, y), fill) <= 24) continue;
      out.left = (std::min)(out.left, x);
      out.top = (std::min)(out.top, y);
      out.right = (std::max)(out.right, x);
      out.bottom = (std::max)(out.bottom, y);
      ++out.count;
    }
  }
  return out;
}

std::vector<prism::ArchiveItem> TextCatalog() {
  std::vector<prism::ArchiveItem> items;
  const char* names[] = {"alpha.txt", "beta.txt", "gamma.txt"};
  for (int i = 0; i < 3; ++i) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = names[i];
    item.size = 1000 + static_cast<uint64_t>(i);
    item.block = 0;
    item.isDir = false;
    items.push_back(item);
  }
  return items;
}

std::vector<prism::ArchiveItem> MixedCatalog() {
  std::vector<prism::ArchiveItem> items;
  auto add = [&items](const char* path, bool dir) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.size = 4096;
    item.block = 0;
    item.isDir = dir;
    items.push_back(item);
  };
  add("photos", true);        // 0: directory -> tile
  add("notes.txt", false);    // 1: text -> row
  add("manual.pdf", false);   // 2: pdf -> row
  add("raw.png", false);      // 3: image WITHOUT a thumb -> row
  add("shot.png", false);     // 4: image WITH a thumb -> tile
  return items;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. the flow: a tile-less item is a full-width BAND
// ---------------------------------------------------------------------------
PRISM_TEST("v4.flow: a tile-less item spans the whole content band") {
  const Layout layout = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), true, false);
  const std::vector<prism::ArchiveItem> archive = TextCatalog();
  std::vector<FlowItem> flow;
  for (const auto& item : archive) {
    flow.push_back(FlowItem{item.isDir, prism::QuickLookPlanner::KindByName(item.path), false});
  }
  const std::vector<FlowCell> cells = prism::layout::FlowCells(layout, flow);
  PRISM_CHECK_EQ(static_cast<int>(cells.size()), 3);
  for (int i = 0; i < static_cast<int>(cells.size()); ++i) {
    const FlowCell& cell = cells[static_cast<size_t>(i)];
    PRISM_CHECK_EQ(cell.kind, CellKind::Row);
    // The band, not the cell: it starts and ends with the CONTENT.
    PRISM_CHECK_EQ(cell.rect.left, layout.content.left);
    PRISM_CHECK_EQ(cell.rect.right, layout.content.right);
    PRISM_CHECK_EQ(cell.rect.Width(), layout.content.Width());
    PRISM_CHECK_EQ(cell.rect.Height(), prism::layout::kFlowRowHeight);
    PRISM_CHECK_EQ(cell.rect.Height(), 32);
    PRISM_CHECK(cell.rect.Width() > layout.tileWidth);  // the old 220 px pill
    // Contiguous: the next band starts exactly where this one ends.
    if (i + 1 < static_cast<int>(cells.size())) {
      PRISM_CHECK_EQ(cells[static_cast<size_t>(i + 1)].rect.top, cell.rect.bottom);
    }
  }
  // The first band starts at the padded top; the flow is anchored like the list.
  PRISM_CHECK_EQ(cells.front().rect.top, layout.contentInner.top);
  // Three text items occupy 96 px, not a 800 px grid: the screenshot's "3 items
  // fill the window" cannot come back.
  PRISM_CHECK_EQ(cells.back().rect.bottom - cells.front().rect.top, 3 * 32);
}

PRISM_TEST("v4.flow: the grid holds only directories and real thumbnails") {
  const Layout layout = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), true, false);
  std::vector<prism::ArchiveItem> archive = MixedCatalog();
  prism::gallery::ThumbStore store{4u << 20};
  prism::gallery::DecodedThumb thumb;
  thumb.width = 2;
  thumb.height = 2;
  thumb.pixels.assign(16, 0x80);
  PRISM_CHECK(store.OnThumb(4, thumb));  // only "shot.png" has pixels

  std::vector<FlowItem> flow;
  for (const auto& item : archive) {
    const prism::PreviewKind kind = prism::QuickLookPlanner::KindByName(item.path);
    flow.push_back(FlowItem{item.isDir, kind, store.Contains(item.index)});
  }
  const std::vector<FlowCell> cells = prism::layout::FlowCells(layout, flow);
  PRISM_CHECK_EQ(static_cast<int>(cells.size()), 5);
  int tiles = 0;
  for (const FlowCell& cell : cells) {
    if (cell.kind == CellKind::Tile) {
      ++tiles;
      const FlowItem& item = flow[static_cast<size_t>(cell.index)];
      // ONLY these two may tile: a directory, or an image/media item whose
      // pixels the store really holds.
      PRISM_CHECK(item.isDir || (prism::layout::ThumbnailCapable(item.kind) &&
                                 item.thumbnailAvailable));
      PRISM_CHECK(layout.contentInner.Contains(cell.rect));
      PRISM_CHECK_EQ(cell.rect.Width(), layout.tileWidth);
    } else {
      PRISM_CHECK_EQ(cell.rect.left, layout.content.left);
      PRISM_CHECK_EQ(cell.rect.right, layout.content.right);
    }
  }
  PRISM_CHECK_EQ(tiles, 2);  // "photos" + "shot.png"
  PRISM_CHECK_EQ(prism::layout::FlowKindOf(flow[1]), CellKind::Row);  // notes.txt
  PRISM_CHECK_EQ(prism::layout::FlowKindOf(flow[2]), CellKind::Row);  // manual.pdf
  PRISM_CHECK_EQ(prism::layout::FlowKindOf(flow[3]), CellKind::Row);  // raw.png, no thumb
  PRISM_CHECK_EQ(prism::layout::FlowKindOf(flow[4]), CellKind::Tile); // shot.png
  // Hit testing uses the same rectangles the paint used.
  const FlowCell& notes = cells[1];
  PRISM_CHECK_EQ(prism::layout::FlowItemAt(layout, flow, notes.rect.left + 4,
                                          notes.rect.top + 4, 0),
                 1);
  PRISM_CHECK_EQ(prism::layout::FlowItemAt(layout, flow, layout.content.left - 2,
                                          notes.rect.top + 4, 0),
                 -1);
}

// ---------------------------------------------------------------------------
// 5. density
// ---------------------------------------------------------------------------
PRISM_TEST("v4.density: 1200x800 shows at least 18 list rows") {
  const Layout layout = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), false, true);
  PRISM_CHECK_EQ(prism::layout::kListRowHeight, 32);
  const Rect body = prism::layout::ListBodyRect(layout);
  const int rows = body.Height() / prism::layout::kListRowHeight;
  PRISM_CHECK_EQ(rows, 19);  // (800 - 88 - 28 - 32 - 32) / 32
  PRISM_CHECK(rows >= 18);
  // Every one of those rows is anchored to the same 32 px pitch, and the hit
  // test agrees with the first and the last of them.
  for (int i = 0; i < rows; ++i) {
    const Rect row = prism::layout::ListRowRect(layout, i, 0);
    PRISM_CHECK_EQ(row.Height(), 32);
    PRISM_CHECK_EQ(row.top, body.top + i * 32);
    PRISM_CHECK_EQ(prism::layout::ListRowAt(layout, row.left + 4, row.top + 8, 0), i);
  }
  PRISM_CHECK(prism::layout::ListRowRect(layout, rows - 1, 0).bottom <= body.bottom);
}

// ---------------------------------------------------------------------------
// 2. action island: one fixed icon/label offset, no clipped label
// ---------------------------------------------------------------------------
PRISM_TEST("v4.actionbar: icon box centre is the label centre minus one offset") {
  const Layout window = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), false, false);
  const prism::actionbar::Layout band = prism::actionbar::Compute(window);
  PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()), 8);
  for (const prism::actionbar::Button& button : band.buttons) {
    const Rect icon = prism::actionbar::IconBoxRect(button.rect);
    const Rect label = prism::actionbar::LabelBoxRect(button.rect);
    // Both boxes live inside the button they belong to.
    PRISM_CHECK(button.rect.Contains(icon));
    PRISM_CHECK(button.rect.Contains(label));
    PRISM_CHECK(!icon.Intersects(label));
    // THE alignment rule, in pixels: same centre X, and the icon centre sits
    // exactly kIconLabelOffset above the label centre.
    PRISM_CHECK_EQ(icon.left + icon.Width() / 2, label.left + label.Width() / 2);
    const int iconCentreY = icon.top + icon.Height() / 2;
    const int labelCentreY = label.top + label.Height() / 2;
    PRISM_CHECK_EQ(iconCentreY, labelCentreY - prism::actionbar::kIconLabelOffset);
    PRISM_CHECK_EQ(prism::actionbar::kIconLabelOffset, 22);
    // The label box is wide enough for the shortest usable label.
    PRISM_CHECK(label.Width() >= prism::actionbar::kMinLabelWidth);
    PRISM_CHECK_EQ(prism::actionbar::kMinLabelWidth, 64);
  }
  // ... and at the band's own floor (640 px client) the guarantee still holds.
  const Layout narrow = prism::layout::Compute(640, 800, false, false);
  const prism::actionbar::Layout narrowBand = prism::actionbar::Compute(narrow);
  PRISM_CHECK_EQ(static_cast<int>(narrowBand.buttons.size()), 8);
  for (const prism::actionbar::Button& button : narrowBand.buttons) {
    PRISM_CHECK(prism::actionbar::LabelBoxRect(button.rect).Width() >=
                prism::actionbar::kMinLabelWidth);
  }
}

PRISM_TEST("v4.actionbar: every label fits its box (no truncation)") {
  IWICImagingFactory* wic = gWic;
  if (!PRISM_CHECK(wic != nullptr)) return;
  ComPtr<IDWriteFactory> write;
  if (!PRISM_CHECK(SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                                 __uuidof(IDWriteFactory),
                                                 reinterpret_cast<IUnknown**>(
                                                     write.ReleaseAndGetAddressOf()))))) {
    return;
  }
  ComPtr<IDWriteTextFormat> format;
  if (!PRISM_CHECK(SUCCEEDED(write->CreateTextFormat(
          L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
          DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"en-US",
          format.ReleaseAndGetAddressOf())))) {
    return;
  }
  // The narrowest band the policy still fills completely: 640 px.
  const Layout window = prism::layout::Compute(640, 800, false, false);
  const prism::actionbar::Layout band = prism::actionbar::Compute(window);
  PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()), 8);
  for (const prism::actionbar::Button& button : band.buttons) {
    const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(
        prism::actionbar::IndexOf(button.id));
    const std::wstring wide = Wide(std::string(spec.label));
    ComPtr<IDWriteTextLayout> layout;
    if (!PRISM_CHECK(SUCCEEDED(write->CreateTextLayout(
            wide.c_str(), static_cast<UINT32>(wide.size()), format.Get(), 1000.0f, 32.0f,
            layout.ReleaseAndGetAddressOf())))) {
      return;
    }
    DWRITE_TEXT_METRICS metrics{};
    PRISM_CHECK(SUCCEEDED(layout->GetMetrics(&metrics)));
    const int available = prism::actionbar::LabelBoxRect(button.rect).Width();
    PRISM_CHECK(metrics.widthIncludingTrailingWhitespace <= static_cast<float>(available));
  }
}

PRISM_TEST("v4.actionbar: the rendered icon and label really line up") {
  IWICImagingFactory* wic = gWic;
  if (!PRISM_CHECK(wic != nullptr)) return;
  prism::actionbar::Model model;
  model.SetContext(prism::actionbar::Context{true, true, false});
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.actions = &model;
  scene.breadcrumb = L"alpha.7z";
  Render render;
  if (!PRISM_CHECK(Render64(wic, scene, &render))) return;
  PRISM_CHECK_EQ(render.counters.actionLabels, 8);
  PRISM_CHECK(render.counters.actionClipped >= 0);
  PRISM_CHECK_EQ(render.counters.actionClipped, 0);  // no label needed the ellipsis

  const Layout window = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), false, false);
  const prism::actionbar::Layout band = prism::actionbar::Compute(window);
  // Probe the ExtractTo button: it is the longest label in the table.
  const int index = prism::actionbar::IndexOf(prism::actionbar::ActionId::ExtractTo);
  const prism::actionbar::Button& button = band.buttons[static_cast<size_t>(index)];
  const Rect iconBox = prism::actionbar::IconBoxRect(button.rect);
  const Rect labelBox = prism::actionbar::LabelBoxRect(button.rect);
  const InkBox iconInk = InkBoundingBox(render.frame, iconBox);
  const InkBox labelInk = InkBoundingBox(render.frame, labelBox);
  PRISM_CHECK(!iconInk.Empty());
  PRISM_CHECK(!labelInk.Empty());
  // The icon paints inside its box only, and the label inside its own.
  PRISM_CHECK(iconInk.left >= iconBox.left && iconInk.right < iconBox.right);
  PRISM_CHECK(iconInk.top >= iconBox.top && iconInk.bottom < iconBox.bottom);
  PRISM_CHECK(labelInk.left >= labelBox.left && labelInk.right < labelBox.right);
  PRISM_CHECK(labelInk.top >= labelBox.top && labelInk.bottom < labelBox.bottom);
  // Alignment, measured: the two ink clouds share the button's centre line.
  PRISM_CHECK(std::abs(iconInk.CentreX() - labelInk.CentreX()) <= 2);
  PRISM_CHECK(std::abs((iconInk.CentreY() + prism::actionbar::kIconLabelOffset) -
                       labelInk.CentreY()) <= 3);
  // The label is not clipped: its ink leaves a margin on both sides.
  PRISM_CHECK(labelInk.left - labelBox.left >= 2);
  PRISM_CHECK(labelBox.right - 1 - labelInk.right >= 2);

  // The ellipsis is wired into the format (query the device state, D3-style).
  prism::win::actionbar::ActionBarPainter painter;
  if (!PRISM_CHECK(SUCCEEDED(painter.CreateDeviceResources(render.target.Get())))) return;
  PRISM_CHECK(painter.LabelEllipsisConfigured());
}

// ---------------------------------------------------------------------------
// 1b. the hit test follows the SAME 32 px pitch, boundary by boundary
// ---------------------------------------------------------------------------
PRISM_TEST("v4.hit: a click on the 32 px pitch lands on the band it names") {
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive v4 hit", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> archive = TextCatalog();
  window.SetItems(&archive);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(kWidth, kHeight));
  const Layout layout = window.LayoutForTest();
  PRISM_CHECK_EQ(window.ItemCount(), 3);
  // EXPECTED values come from the definition, not from the window: three
  // tile-less items are three contiguous 32 px bands starting at the padded
  // content top (DESIGN.md §2). The literal 32 is the pitch the spec names.
  const int firstTop = layout.contentInner.top;
  const int x = layout.content.left + 40;
  for (int i = 0; i < 3; ++i) {
    const int bandTop = firstTop + i * 32;
    PRISM_CHECK_EQ(window.HitTestForTest(x, bandTop), i);          // first pixel
    PRISM_CHECK_EQ(window.HitTestForTest(x, bandTop + 31), i);     // last pixel
    if (i > 0) {
      // One pixel ABOVE the boundary still belongs to the previous row: this is
      // the assertion that a pitch that is off by one cannot survive.
      PRISM_CHECK_EQ(window.HitTestForTest(x, bandTop - 1), i - 1);
    }
  }
  // Above the first band there is no item, and the rail is not the grid.
  PRISM_CHECK_EQ(window.HitTestForTest(x, firstTop - 1), -1);
  PRISM_CHECK_EQ(window.HitTestForTest(x, firstTop + 3 * 32), -1);
  PRISM_CHECK_EQ(window.HitTestForTest(layout.content.left - 40, firstTop + 4), -1);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 3. the sidebar is chrome: present in grid mode, 220 px, collapse -> 0
// ---------------------------------------------------------------------------
PRISM_TEST("v4.sidebar: the grid reserves the rail, collapse gives it back") {
  // Policy: the rail is a band (DESIGN.md §2), not list-mode chrome.
  PRISM_CHECK(prism::layout::RailVisible(true, false));
  PRISM_CHECK(prism::layout::RailVisible(true, true));
  PRISM_CHECK(!prism::layout::RailVisible(false, false));

  const Layout grid = prism::layout::Compute(static_cast<int>(kWidth),
                                             static_cast<int>(kHeight), true, false);
  PRISM_CHECK_EQ(grid.sidebar.Width(), 220);
  PRISM_CHECK_EQ(grid.content.left, 220);
  const Layout collapsed = prism::layout::Compute(static_cast<int>(kWidth),
                                                  static_cast<int>(kHeight), false, false);
  PRISM_CHECK(collapsed.sidebar.Empty());
  PRISM_CHECK_EQ(collapsed.content.left, 0);
  PRISM_CHECK_EQ(grid.content.left - collapsed.content.left, 220);
  PRISM_CHECK_EQ(collapsed.contentInner.Width() - grid.contentInner.Width(), 220);

  // ... and the REAL window follows the same policy in grid mode.
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive v4 sidebar", GetModuleHandleW(nullptr)))) {
    return;
  }
  std::vector<prism::ArchiveItem> archive = TextCatalog();
  window.SetItems(&archive);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(kWidth, kHeight));
  PRISM_CHECK(!window.ListMode());
  PRISM_CHECK_EQ(window.LayoutForTest().sidebar.Width(), 220);
  PRISM_CHECK_EQ(window.LayoutForTest().content.left, 220);
  // The rail's own command list is what the renderer paints; it must exist in
  // grid mode too (it used to be list-only).
  PRISM_CHECK(!window.SidebarLayout().commands.empty());

  window.SetSidebarRailVisible(false);
  PRISM_CHECK(window.LayoutForTest().sidebar.Empty());
  PRISM_CHECK_EQ(window.LayoutForTest().content.left, 0);
  window.SetSidebarRailVisible(true);
  PRISM_CHECK_EQ(window.LayoutForTest().sidebar.Width(), 220);

  // Pixels: a grid frame with the rail really paints the left column.
  std::vector<prism::ArchiveItem> catalog = MixedCatalog();
  prism::sidebar::Model model(&catalog);
  prism::win::sidebar::Metrics metrics;
  metrics.width = static_cast<float>(prism::layout::kSidebarWidth);
  metrics.viewportTop = static_cast<float>(grid.content.top);
  metrics.viewportBottom = static_cast<float>(grid.content.bottom);
  const prism::win::sidebar::Layout rail = prism::win::sidebar::Build(model, metrics);

  prism::win::gallery::GalleryRenderer::Scene withRail = GridScene(&catalog, nullptr);
  withRail.sidebar = &rail;
  prism::win::gallery::GalleryRenderer::Scene withoutRail = withRail;
  withoutRail.sidebar = nullptr;
  Render a, b;
  if (!PRISM_CHECK(Render64(gWic, withRail, &a))) return;
  if (!PRISM_CHECK(Render64(gWic, withoutRail, &b))) return;
  PRISM_CHECK(a.counters.sidebarPanels > 0);
  int changed = 0;
  for (int y = 100; y < 700; y += 7) {
    for (int x = 8; x < 200; x += 7) {
      if (a.frame.At(x, y) != b.frame.At(x, y)) ++changed;
    }
  }
  PRISM_CHECK(changed > 100);  // the rail's column is not the grid's
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 4. the status bar: four segments, four states
// ---------------------------------------------------------------------------
PRISM_TEST("v4.status: four segments with the four DESIGN states") {
  const Layout layout = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), true, false);
  // State 1: not encrypted.
  prism::layout::StatusInfo clear;
  clear.crypto = prism::layout::CryptoState::Unencrypted;
  clear.method = "LZMA2";
  clear.dictionary = "字典 64 MB";
  clear.ratioPercent = 87;
  PRISM_CHECK_EQ(prism::layout::CryptoText(clear), std::string_view("未加密"));
  auto segments = prism::layout::StatusSegments(layout, clear);
  PRISM_CHECK_EQ(static_cast<int>(segments.size()), 4);
  PRISM_CHECK_EQ(segments[0].id, prism::layout::StatusId::Encryption);
  PRISM_CHECK_EQ(segments[1].id, prism::layout::StatusId::Method);
  PRISM_CHECK_EQ(segments[2].id, prism::layout::StatusId::Dictionary);
  PRISM_CHECK_EQ(segments[3].id, prism::layout::StatusId::Ratio);
  for (const auto& segment : segments) {
    PRISM_CHECK(layout.statusBar.Contains(segment.rect));
    PRISM_CHECK(segment.rect.Width() >= prism::layout::kStatusSegmentMinWidth);
    PRISM_CHECK(!segment.text.empty());
  }
  for (size_t i = 0; i + 1 < segments.size(); ++i) {
    PRISM_CHECK(!segments[i].rect.Intersects(segments[i + 1].rect));
    PRISM_CHECK(segments[i].rect.right < segments[i + 1].rect.left);
  }
  PRISM_CHECK_EQ(segments[1].text, std::string_view("LZMA2"));
  PRISM_CHECK_EQ(segments[2].text, std::string_view("字典 64 MB"));
  PRISM_CHECK_EQ(segments[3].text, std::string_view("压缩率"));
  PRISM_CHECK_EQ(segments[3].value, 87);

  // State 2: encrypted.
  prism::layout::StatusInfo encrypted = clear;
  encrypted.crypto = prism::layout::CryptoState::Encrypted;
  PRISM_CHECK_EQ(prism::layout::CryptoText(encrypted), std::string_view("已加密"));

  // State 3: the method is unknown (an engine that answered nothing).
  prism::layout::StatusInfo unknown = clear;
  unknown.method = {};
  unknown.dictionary = {};
  unknown.ratioPercent = -1;
  auto unknownSegments = prism::layout::StatusSegments(layout, unknown);
  PRISM_CHECK_EQ(unknownSegments[1].text, std::string_view("方法未知"));
  PRISM_CHECK_EQ(unknownSegments[2].text, std::string_view("字典未知"));
  PRISM_CHECK_EQ(unknownSegments[3].value, -1);

  // State 4: compressing.
  prism::layout::StatusInfo compressing = clear;
  compressing.compressing = true;
  auto compressingSegments = prism::layout::StatusSegments(layout, compressing);
  PRISM_CHECK_EQ(compressingSegments[3].text, std::string_view("压缩中…"));
  PRISM_CHECK_EQ(compressingSegments[3].value, -1);
}

PRISM_TEST("v4.status: the band is no longer empty at the device") {
  IWICImagingFactory* wic = gWic;
  if (!PRISM_CHECK(wic != nullptr)) return;
  const Layout layout = prism::layout::Compute(static_cast<int>(kWidth),
                                               static_cast<int>(kHeight), true, false);

  prism::layout::StatusInfo encrypted;
  encrypted.crypto = prism::layout::CryptoState::Encrypted;
  encrypted.method = "LZMA2";
  encrypted.dictionary = "字典 64 MB";
  encrypted.ratioPercent = 87;

  std::vector<prism::ArchiveItem> archive = TextCatalog();
  prism::win::gallery::GalleryRenderer::Scene scene = GridScene(&archive, nullptr);
  scene.status = encrypted;
  Render painted;
  if (!PRISM_CHECK(Render64(wic, scene, &painted))) return;
  PRISM_CHECK_EQ(painted.counters.statusSegments, 4);

  const auto segments = prism::layout::StatusSegments(layout, encrypted);
  // The band's own fill, sampled left of the first segment (never on a glyph).
  const UINT32 bandFill = painted.frame.At(layout.statusBar.left + 4,
                                           layout.statusBar.top + 14);
  for (const auto& segment : segments) {
    PRISM_CHECK(InkPixels(painted.frame, segment.rect, bandFill) > 8);
  }

  // 未加密 must look different from 已加密 in the encryption segment (the text
  // and the state dot both change).
  prism::layout::StatusInfo clear = encrypted;
  clear.crypto = prism::layout::CryptoState::Unencrypted;
  prism::win::gallery::GalleryRenderer::Scene clearScene = scene;
  clearScene.status = clear;
  Render clearFrame;
  if (!PRISM_CHECK(Render64(wic, clearScene, &clearFrame))) return;
  int differing = 0;
  const Rect box = segments[0].rect;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (painted.frame.At(x, y) != clearFrame.frame.At(x, y)) ++differing;
    }
  }
  PRISM_CHECK(differing > 6);
}

int main(int argc, char** argv) {
#if defined(PRISM_V4_MUTANT_CELL_ROW)
  std::printf("V4 MUTANT ACTIVE: cell-row (tile-less items stay inside their cells)\n");
  prism::test::SetMutationVerdict("V4 MUTATION CAUGHT: cell-row",
                                  [] { return !prism::layout::kRowsSpanContent; });
#elif defined(PRISM_V4_MUTANT_GRID_NO_SIDEBAR)
  std::printf("V4 MUTANT ACTIVE: grid-no-sidebar (the rail is list-mode chrome)\n");
  prism::test::SetMutationVerdict("V4 MUTATION CAUGHT: grid-no-sidebar",
                                  [] { return !prism::layout::kRailInGridMode; });
#elif defined(PRISM_V4_MUTANT_HIT_PITCH)
  std::printf("V4 MUTANT ACTIVE: hit-pitch (the hit test's row pitch is off by one)\n");
  prism::test::SetMutationVerdict(
      "V4 MUTATION CAUGHT: hit-pitch",
      [] { return prism::layout::kFlowHitPitch != prism::layout::kFlowRowHeight; });
#endif
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: COM init failed");
    return 77;  // D1: a skip is never a pass
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&gWic)))) {
    std::puts("SKIP: WIC unavailable");
    return 77;
  }
  const int result = prism::test::RunAll(argc, argv);
  gWic->Release();
  gWic = nullptr;
  if (result == 0) std::puts("ALL VISUAL-POLISH CHECKS PASSED");
  return result;
}
