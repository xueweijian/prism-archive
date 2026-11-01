// PrismArchive -- PrismWin
// C29: PIXEL-level tests for the sidebar painter (GalleryRenderer::DrawSidebar).
//
// C12 produced a SidebarLayout of 13 command kinds, C13 landed the ordering the
// window uses, and until this card nothing consumed the sidebar commands: the
// model had geometry, the screen had no pixels. This suite renders the REAL
// renderer into a headless WIC target -- no window, no GPU, no font file -- and
// asserts the pixels the user is supposed to see:
//
//   * the column paints its surface INSIDE the panel box (and only there);
//   * the keyboard focus pill is a real colour difference inside the focused
//     row, and the highlight is LOCAL (the rest of the column is identical);
//   * the width fold is pinned exactly at the threshold, and a folded column
//     leaves no panel pixels behind;
//   * a collapsed column gives the list its width back (measured from the
//     selection pill, in the same frame size);
//   * the scroll window CLIPS: a row straddling the content bottom is cut at
//     the edge (positive control: the same label fully inside is visible);
//   * the telemetry capsule's corners are SQUIRCLES (C15 SquircleSdf): the
//     corner pixel is background while the edge pixel is the capsule.
//
// The stdout marker is part of the verdict (CONTRACTS D1): a crash after the
// checks or a silent early return cannot count as a pass. The mutation targets
// registered with WILL_FAIL compile the SAME source with one rule broken and
// must go red (CONTRACTS D3) -- each prints the active mutation first, so the
// log proves the injection was really compiled in.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;

namespace {

using prism::ArchiveItem;
using prism::sidebar::Encryption;
using prism::sidebar::FocusTarget;
using prism::sidebar::Section;
using prism::win::sidebar::Box;
using prism::win::sidebar::Command;
using prism::win::sidebar::Layout;
using prism::win::sidebar::Metrics;
using prism::win::sidebar::PrimKind;

constexpr UINT32 kWidth = 1400;
constexpr UINT32 kHeight = 900;
// The list body starts at the layout's content top (88 + 16 padding). The
// header band and row pitch below come from WindowLayout, not literals.
constexpr int kListContentTop = 104;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major, opaque backdrop underneath

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

// Mean per-channel absolute difference over a box (0 when the box is empty).
double MeanDiff(const Frame& a, const Frame& b, const Box& box) {
  double sum = 0.0;
  int count = 0;
  for (int y = static_cast<int>(box.top); y < static_cast<int>(box.bottom); ++y) {
    for (int x = static_cast<int>(box.left); x < static_cast<int>(box.right); ++x) {
      if (!a.In(x, y) || !b.In(x, y)) continue;
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs((static_cast<int>((a.At(x, y) >> shift) & 0xFFu)) -
                        (static_cast<int>((b.At(x, y) >> shift) & 0xFFu)));
      }
      ++count;
    }
  }
  return count > 0 ? sum / (count * 3.0) : 0.0;
}

int CountDiff(const Frame& a, const Frame& b, const Box& box, int threshold) {
  int count = 0;
  for (int y = static_cast<int>(box.top); y < static_cast<int>(box.bottom); ++y) {
    for (int x = static_cast<int>(box.left); x < static_cast<int>(box.right); ++x) {
      if (!a.In(x, y) || !b.In(x, y)) continue;
      if (ColorDistance(a.At(x, y), b.At(x, y)) >= threshold) ++count;
    }
  }
  return count;
}

// Renders the real GalleryRenderer into a WIC bitmap and reads the pixels back.
bool RenderScene(IWICImagingFactory* wic, const prism::win::gallery::GalleryRenderer::Scene& scene,
                 UINT32 width, UINT32 height, Frame* frame, int* painted, int* clipped,
                 int* collapsed) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  if (painted) *painted = renderer.dbgSidebarPainted;
  if (clipped) *clipped = renderer.dbgSidebarClipped;
  if (collapsed) *collapsed = renderer.dbgSidebarCollapsed;
  return true;
}

// ------------------------------------------------------------------- fixtures
ArchiveItem File(const char* path, std::uint32_t index, std::uint64_t size,
                 std::uint64_t packed) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  item.packSize = packed;
  return item;
}

ArchiveItem Dir(const char* path, std::uint32_t index) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.isDir = true;
  return item;
}

std::vector<ArchiveItem> SampleCatalog() {
  return {
      Dir("Design_Assets", 1),
      Dir("Design_Assets/01_Brand", 2),
      File("Design_Assets/01_Brand/logo.svg", 3, 1200, 400),
      File("Design_Assets/02_UI_Kit/Hero.webp", 4, 18400000, 14200000),
      File("Design_Assets/03_Raw/photo.jpg", 5, 9000000, 8800000),
      File("root.png", 6, 500, 300),
  };
}

// Builds the C12 command list the way GalleryWindow::EnsureSidebar does, with
// the focus the caller wants (the pill is emitted only for the focused row).
Layout BuildLayout(const std::vector<ArchiveItem>& items, float viewportHeight, float scrollY,
                   FocusTarget focus) {
  prism::sidebar::Model model(&items);
  model.SetArchiveName("Design_Assets_v4.7z");
  model.SetFavorites({
      prism::sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"},
      prism::sidebar::Favorite{"Downloads", "C:/Users/designer/Downloads", "download"},
  });
  model.PushRecent("D:/Work/Design_Assets.7z", 500);
  prism::sidebar::ArchiveInfo info;
  info.method = "LZMA2";
  info.dictionaryBytes = 64ull << 20;
  info.encryption = Encryption::Aes256;
  model.SetArchiveInfo(info);
  model.SetFocus(focus);

  Metrics metrics;
  // V1: the rail's column width is DESIGN.md's 220 -- the SAME constant the
  // renderer's layout reserves. A literal here would be a second source.
  metrics.width = prism::win::gallery::GalleryRenderer::kSidebarColumnWidth;
  metrics.viewportTop = 0.0f;
  metrics.viewportBottom = viewportHeight;
  metrics.scrollY = scrollY;
  return prism::win::sidebar::Build(model, metrics);
}

// The empty-archive layout: no Selection command at all, which keeps the list
// measurements below free of sidebar highlights.
Layout BuildEmptyLayout(float viewportHeight) {
  prism::sidebar::Model model;
  Metrics metrics;
  metrics.width = prism::win::gallery::GalleryRenderer::kSidebarColumnWidth;
  metrics.viewportTop = 0.0f;
  metrics.viewportBottom = viewportHeight;
  return prism::win::sidebar::Build(model, metrics);
}

std::vector<prism::list::SortItem> ListRows() {
  std::vector<prism::list::SortItem> rows;
  rows.push_back({"01_Brand_Guidelines", 0, 0, true, false, prism::PreviewKind::Directory});
  rows.push_back({"02_UI_Kit_iOS26", 0, 0, true, false, prism::PreviewKind::Directory});
  rows.push_back({"Hero.webp", 18400000, 14200000, false, false, prism::PreviewKind::Image});
  rows.push_back({"client_w9_encrypted.pdf", 4200000, 1100000, false, true,
                  prism::PreviewKind::Pdf});
  rows.push_back({"street_09.png", 3300000, 2900000, false, false, prism::PreviewKind::Image});
  return rows;
}

const Command* FindKind(const Layout& layout, PrimKind kind) {
  for (const Command& command : layout.commands) {
    if (command.kind == kind) return &command;
  }
  return nullptr;
}

Box FullBox(const Frame& frame) {
  return Box{0.0f, 0.0f, static_cast<float>(frame.width), static_cast<float>(frame.height)};
}

// Horizontal extent of the selection pill inside a row band: a column counts
// when ANY pixel in the band is blue-dominant. Scanning the band (not one scan
// line) keeps the label glyphs -- which paint light text ON the pill -- from
// punching holes into the measurement.
bool AccentSpan(const Frame& frame, int yTop, int yBottom, int minX, int* left,
                int* right) {
  int first = -1;
  int last = -1;
  for (int x = minX; x < static_cast<int>(frame.width); ++x) {
    bool selected = false;
    for (int y = yTop; y < yBottom && !selected; ++y) {
      if (!frame.In(x, y)) continue;
      selected = (frame.Channel(x, y, 0) - frame.Channel(x, y, 16)) >= 40;
    }
    if (!selected) continue;
    if (first < 0) first = x;
    last = x;
  }
  if (first < 0) return false;
  *left = first;
  *right = last + 1;
  return true;
}

IWICImagingFactory* gWic = nullptr;

}  // namespace

// --------------------------------------------------------------------- cases
PRISM_TEST("sidebar column paints its surface inside the panel") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout = BuildLayout(items, static_cast<float>(kHeight), 0.0f,
                                    FocusTarget{Section::Tree, 1});

  prism::win::gallery::GalleryRenderer::Scene with;
  with.listMode = true;  // the sidebar is list-mode chrome (renderer contract)
  with.sidebar = &layout;
  prism::win::gallery::GalleryRenderer::Scene without;
  without.listMode = true;

  Frame a, b;
  int painted = 0, paintedNone = 0, collapsed = 0, clipped = 0;
  PRISM_CHECK(RenderScene(gWic, with, kWidth, kHeight, &a, &painted, &clipped, &collapsed));
  PRISM_CHECK(RenderScene(gWic, without, kWidth, kHeight, &b, &paintedNone, nullptr, nullptr));
  PRISM_CHECK(painted > 20);  // the commands really reached the device
  PRISM_CHECK_EQ(paintedNone, 0);
  PRISM_CHECK_EQ(collapsed, 0);

  // x = 6 is left of every command box (paddingX = 16): pure panel surface.
  const int probeYs[3] = {40, 320, 700};
  for (const int y : probeYs) {
    const double diff = MeanDiff(a, b, Box{4.0f, static_cast<float>(y), 10.0f,
                                           static_cast<float>(y + 6)});
    PRISM_CHECK(diff > 10.0);
  }
  // ... and the surface is brighter than the canvas backdrop it sits on.
  PRISM_CHECK(a.Luminance(6, 40) > b.Luminance(6, 40) + 10);

  // Nothing leaks to the right of the column: the list area is untouched.
  PRISM_CHECK(MeanDiff(a, b, Box{300.0f, 0.0f, 1200.0f, 800.0f}) < 0.5);
  // The column is exactly kSidebarColumnWidth wide.
  const float right = layout.panel.right;
  PRISM_CHECK(right == prism::win::gallery::GalleryRenderer::kSidebarColumnWidth);
}

PRISM_TEST("focused row gets a local highlight pill") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout focused = BuildLayout(items, static_cast<float>(kHeight), 0.0f,
                                     FocusTarget{Section::Tree, 1});
  const Layout other = BuildLayout(items, static_cast<float>(kHeight), 0.0f,
                                   FocusTarget{Section::Tree, 4});
  const Command* pill = FindKind(focused, PrimKind::Selection);
  PRISM_CHECK(pill != nullptr);
  if (!pill) return;

  prism::win::gallery::GalleryRenderer::Scene sa;
  sa.listMode = true;
  sa.sidebar = &focused;
  prism::win::gallery::GalleryRenderer::Scene sb;
  sb.listMode = true;
  sb.sidebar = &other;

  Frame a, b;
  PRISM_CHECK(RenderScene(gWic, sa, kWidth, kHeight, &a, nullptr, nullptr, nullptr));
  PRISM_CHECK(RenderScene(gWic, sb, kWidth, kHeight, &b, nullptr, nullptr, nullptr));

  // Inside the focused row: a filled pill, not a hairline.
  PRISM_CHECK(CountDiff(a, b, pill->box, 12) > 400);
  PRISM_CHECK(MeanDiff(a, b, pill->box) > 4.0);

  // The pill sits inside the panel and does not touch the content column.
  PRISM_CHECK(pill->box.right <= prism::win::gallery::GalleryRenderer::kSidebarColumnWidth);

  // The highlight is LOCAL: one row below the pill the two frames are equal.
  const Box below{pill->box.left, pill->box.bottom + 2.0f, pill->box.right,
                  pill->box.bottom + 30.0f};
  PRISM_CHECK(MeanDiff(a, b, below) < 0.5);
  // ... and far away in the list column nothing changed either.
  PRISM_CHECK(MeanDiff(a, b, Box{400.0f, 100.0f, 1300.0f, 700.0f}) < 0.5);
}

PRISM_TEST("width fold threshold is pinned and folds to nothing") {
  using prism::win::gallery::GalleryRenderer;
  PRISM_CHECK(GalleryRenderer::SidebarFits(1400.0f));
  PRISM_CHECK(GalleryRenderer::SidebarFits(960.0f));   // exactly at the threshold: shown
  PRISM_CHECK(!GalleryRenderer::SidebarFits(959.0f));  // one pixel narrower: folded
  PRISM_CHECK(!GalleryRenderer::SidebarFits(720.0f));
  PRISM_CHECK(GalleryRenderer::SidebarInset(1400.0f) == GalleryRenderer::kSidebarColumnWidth);
  PRISM_CHECK(GalleryRenderer::SidebarInset(800.0f) == 0.0f);

  // The width fold is the WINDOW's policy (list mode + the rail toggle + this
  // predicate), so the renderer-level proof is the explicit fold: a folded rail
  // paints nothing at all, exactly like an absent one.
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout = BuildLayout(items, 700.0f, 0.0f, FocusTarget{Section::Tree, 1});

  prism::win::gallery::GalleryRenderer::Scene foldedScene;
  foldedScene.sidebar = &layout;
  foldedScene.sidebarCollapsed = true;
  prism::win::gallery::GalleryRenderer::Scene none;
  Frame a, b;
  int paintedFolded = 0, collapsedFolded = 0, clipped = 0;
  PRISM_CHECK(RenderScene(gWic, foldedScene, kWidth, kHeight, &a, &paintedFolded, &clipped,
                          &collapsedFolded));
  int paintedNone = 0;
  PRISM_CHECK(RenderScene(gWic, none, kWidth, kHeight, &b, &paintedNone, nullptr, nullptr));
  PRISM_CHECK_EQ(paintedFolded, 0);    // nothing was painted
  PRISM_CHECK_EQ(collapsedFolded, 1);  // and the fold was taken
  PRISM_CHECK_EQ(paintedNone, 0);
  PRISM_CHECK(MeanDiff(a, b, FullBox(a)) < 0.5);

  // The same layout UNfolded does paint: the zero above is the fold, not an
  // empty command list (a vacuous "no pixels" assertion would pass on a bug).
  prism::win::gallery::GalleryRenderer::Scene open;
  open.sidebar = &layout;
  int paintedOpen = 0;
  PRISM_CHECK(RenderScene(gWic, open, kWidth, kHeight, &b, &paintedOpen, nullptr, nullptr));
  PRISM_CHECK(paintedOpen > 5);
  PRISM_CHECK(MeanDiff(a, b, Box{4.0f, 40.0f, 232.0f, 700.0f}) > 10.0);
}

PRISM_TEST("collapsed column gives the list its width back") {
  const Layout sidebar = BuildEmptyLayout(static_cast<float>(kHeight));
  std::vector<prism::list::SortItem> rows = ListRows();

  prism::win::gallery::GalleryRenderer::Scene expanded;
  expanded.listMode = true;
  expanded.listItems = &rows;
  expanded.selectedRow = 2;
  expanded.sidebar = &sidebar;

  prism::win::gallery::GalleryRenderer::Scene folded = expanded;
  folded.sidebarCollapsed = true;

  Frame a, b;
  int painted = 0, collapsed = 0;
  PRISM_CHECK(RenderScene(gWic, expanded, kWidth, kHeight, &a, &painted, nullptr, &collapsed));
  PRISM_CHECK(painted >= 5);  // panel + empty state + capsule lines
  PRISM_CHECK(RenderScene(gWic, folded, kWidth, kHeight, &b, nullptr, nullptr, &collapsed));
  PRISM_CHECK_EQ(collapsed, 1);  // the user's fold is honoured

  // V1: 32 px rows. The probe band must cover the SELECTED row only -- with a
  // 36 px pitch the neighbouring folder glyph bled into the measurement.
  const int rowTop = kListContentTop + prism::layout::kListHeaderHeight +
                     2 * prism::layout::kListRowHeight;
  const int rowBottom = rowTop + prism::layout::kListRowHeight;
  int expandedLeft = 0, expandedRight = 0, foldedLeft = 0, foldedRight = 0;
  PRISM_CHECK(AccentSpan(a, rowTop + 2, rowBottom - 2, 0, &expandedLeft, &expandedRight));
  PRISM_CHECK(AccentSpan(b, rowTop + 2, rowBottom - 2, 0, &foldedLeft, &foldedRight));

  // Expanded: the pill starts after the 220 px column (16 px inset + column).
  PRISM_CHECK(expandedLeft >= 236);
  PRISM_CHECK(expandedLeft <= 260);
  // Folded: it starts at the plain 16 px inset...
  PRISM_CHECK(foldedLeft <= 24);
  // ... and is a whole column wider.
  const int expandedWidth = expandedRight - expandedLeft;
  const int foldedWidth = foldedRight - foldedLeft;
  PRISM_CHECK(foldedWidth - expandedWidth > 200);
  PRISM_CHECK(foldedWidth - expandedWidth <= 240);
}

PRISM_TEST("scroll window clips rows at the content edge") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const FocusTarget focus{Section::Tree, 1};
  const Layout unscrolled = BuildLayout(items, static_cast<float>(kHeight), 0.0f, focus);
  const Layout scrolled = BuildLayout(items, static_cast<float>(kHeight), 600.0f, focus);
  // C12 culls what left the viewport entirely (the scrollbar must not jump), so
  // the renderer receives FEWER commands -- evidence the scroll window is live.
  PRISM_CHECK(scrolled.commands.size() < unscrolled.commands.size());

  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.sidebar = &scrolled;
  Frame a;
  int painted = 0;
  PRISM_CHECK(RenderScene(gWic, scene, kWidth, kHeight, &a, &painted, nullptr, nullptr));
  PRISM_CHECK(painted >= 5);

  // The renderer's own clip: a label straddling the content bottom is CUT at
  // that edge. The band between the content window and the capsule stays
  // panel-coloured, i.e. the row cannot bleed over the anchored telemetry pill.
  Layout probe = scrolled;
  probe.commands.clear();
  Command panel;
  panel.kind = PrimKind::Panel;
  panel.box = probe.panel;
  probe.commands.push_back(panel);
  Command straddle;
  straddle.kind = PrimKind::Label;
  straddle.box = Box{24.0f, probe.contentBottom - 10.0f, 220.0f, probe.contentBottom + 18.0f};
  straddle.text = "STRADDLE-ROW-THAT-MUST-BE-CUT";
  straddle.color = prism::win::sidebar::ColorToken::PrimaryText;
  probe.commands.push_back(straddle);
  prism::win::gallery::GalleryRenderer::Scene probeScene;
  probeScene.listMode = true;
  probeScene.sidebar = &probe;
  Frame p;
  PRISM_CHECK(RenderScene(gWic, probeScene, kWidth, kHeight, &p, nullptr, nullptr, nullptr));
  int brightBelow = 0;
  for (int y = static_cast<int>(probe.contentBottom) + 1;
       y < static_cast<int>(probe.contentBottom) + 10; ++y) {
    for (int x = 24; x < 220; ++x) {
      if (p.Luminance(x, y) > 90) ++brightBelow;
    }
  }
  PRISM_CHECK_EQ(brightBelow, 0);

  // ... and the same at the TOP of the content window (the panel's padding band
  // must stay clean, otherwise text slides under the section headers).
  Layout top = probe;
  top.commands[1].box =
      Box{24.0f, top.contentTop - 12.0f, 220.0f, top.contentTop + 16.0f};
  prism::win::gallery::GalleryRenderer::Scene topScene;
  topScene.listMode = true;
  topScene.sidebar = &top;
  Frame t;
  PRISM_CHECK(RenderScene(gWic, topScene, kWidth, kHeight, &t, nullptr, nullptr, nullptr));
  int brightAbove = 0;
  for (int y = static_cast<int>(top.contentTop) - 11;
       y < static_cast<int>(top.contentTop) - 1; ++y) {
    for (int x = 24; x < 220; ++x) {
      if (t.Luminance(x, y) > 90) ++brightAbove;
    }
  }
  PRISM_CHECK_EQ(brightAbove, 0);

  // Positive control for the SAME measurement: the identical label fully inside
  // the content window is visibly brighter than the panel, so the zeroes above
  // are a clip and not a detector that cannot see text.
  Layout inside = probe;
  inside.commands[1].box =
      Box{24.0f, probe.contentBottom - 36.0f, 220.0f, probe.contentBottom - 4.0f};
  prism::win::gallery::GalleryRenderer::Scene insideScene;
  insideScene.listMode = true;
  insideScene.sidebar = &inside;
  Frame q;
  PRISM_CHECK(RenderScene(gWic, insideScene, kWidth, kHeight, &q, nullptr, nullptr, nullptr));
  int visible = 0;
  for (int y = static_cast<int>(inside.commands[1].box.top);
       y < static_cast<int>(inside.commands[1].box.bottom); ++y) {
    for (int x = 24; x < 220; ++x) {
      if (q.Luminance(x, y) > 90) ++visible;
    }
  }
  PRISM_CHECK(visible > 40);
}

PRISM_TEST("telemetry capsule corners are squircles") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout = BuildLayout(items, static_cast<float>(kHeight), 0.0f,
                                    FocusTarget{Section::Tree, 1});
  PRISM_CHECK(!layout.pill.Empty());
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.sidebar = &layout;
  Frame f;
  PRISM_CHECK(RenderScene(gWic, scene, kWidth, kHeight, &f, nullptr, nullptr, nullptr));

  const int cy = static_cast<int>((layout.pill.top + layout.pill.bottom) * 0.5f);
  // Sample the capsule on its right-hand margin: every text run is left
  // aligned, so this pixel is pure capsule surface.
  const UINT32 capsule = f.At(static_cast<int>(layout.pill.right) - 4,
                               static_cast<int>(layout.pill.top) + 10);
  const UINT32 panel = f.At(3, cy);

  // The capsule is a distinct surface (recessed canvas pane)...
  PRISM_CHECK(ColorDistance(capsule, panel) > 12);
  // ... its straight left edge is filled...
  const int pillLeft = static_cast<int>(layout.pill.left);
  const int pillTop = static_cast<int>(layout.pill.top);
  PRISM_CHECK(ColorDistance(f.At(pillLeft + 3, cy), capsule) < 10);
  // ... and the corner is neither square NOR circular. For a radius-14 corner
  // the top edge at x = left+3 sits at y = top+1.1 for the C15 squircle
  // (n = 4.2) but at y = top+4.7 for a circular arc, while a square corner has
  // fill all the way to (left, top). Sampling both sides pins the superellipse:
  const UINT32 outside = f.At(pillLeft + 1, pillTop + 1);  // outside even n=4.2
  const UINT32 bulge = f.At(pillLeft + 3, pillTop + 2);    // inside n=4.2, outside r=14
  PRISM_CHECK(ColorDistance(outside, panel) < 12);
  PRISM_CHECK(ColorDistance(outside, capsule) > 12);
  PRISM_CHECK(ColorDistance(bulge, capsule) < 12);
  PRISM_CHECK(ColorDistance(bulge, panel) > 12);
}

PRISM_TEST("icons are painted as vector glyphs") {
  const std::vector<ArchiveItem> items = SampleCatalog();
  const Layout layout = BuildLayout(items, static_cast<float>(kHeight), 0.0f,
                                    FocusTarget{Section::Tree, 1});
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.sidebar = &layout;
  Frame f;
  PRISM_CHECK(RenderScene(gWic, scene, kWidth, kHeight, &f, nullptr, nullptr, nullptr));

  int glyphs = 0;
  int bright = 0;
  for (const Command& command : layout.commands) {
    if (command.kind != PrimKind::Glyph && command.kind != PrimKind::Chevron &&
        command.kind != PrimKind::StatusIcon) {
      continue;
    }
    ++glyphs;
    for (int y = static_cast<int>(command.box.top); y < static_cast<int>(command.box.bottom);
         ++y) {
      for (int x = static_cast<int>(command.box.left); x < static_cast<int>(command.box.right);
           ++x) {
        if (!f.In(x, y)) continue;
        if (f.Luminance(x, y) > 90) ++bright;
      }
    }
  }
  PRISM_CHECK(glyphs >= 6);
  PRISM_CHECK(bright > 60);
}

int main(int argc, char** argv) {
  const std::string_view mutation = prism::win::gallery::GalleryRenderer::ActiveMutation();
  if (!mutation.empty()) {
    std::printf("C29 MUTANT ACTIVE: %.*s\n", static_cast<int>(mutation.size()),
                mutation.data());
  } else {
    std::puts("C29 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the painter cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the painter cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("SIDEBAR-PAINT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL SIDEBAR-PAINT CHECKS PASSED");
  return 0;
}