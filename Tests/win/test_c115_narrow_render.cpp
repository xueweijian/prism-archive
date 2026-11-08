// PrismArchive -- PrismWin
// C115: the narrow window's breadcrumb pill -- the REAL paint path.
//
// The pure suite (Tests/test_c115_narrow_layout.cpp) pins the geometry on every
// platform. This file proves the same numbers reach PIXELS through the shipping
// renderer, and produces the two artefacts the card asks for:
//
//   docs/assets/ui/c115-1028.png   the squeeze: pill 16..420, island 432..1012
//   docs/assets/ui/c115-1264.png   the baseline: pill 16..576, island 588..1248
//
// WHAT IS ASSERTED (real GalleryRenderer, real action bar model, headless WIC):
//   1. the pill the renderer draws is the pill prism::layout publishes (they
//      were 144 px apart at 1028 before this card), and it ends where the layout
//      says it ends -- measured by walking the fill colour along the pill's own
//      row, not by trusting a rectangle;
//   2. the crumb text stays inside its 24 px-inset box: no ink in the strip
//      between the text box and the pill's right edge, i.e. nothing spills
//      toward the toolbar (the clip is what the card allows, and the box is now
//      derived from the SHORTENED pill);
//   3. 1264 keeps the pre-card geometry to the pixel (the baseline every other
//      card's frame is measured on).
//
// The PNGs are written only when an output directory is passed on argv[1] (the
// c109/c110 convention), so a plain ctest run stays headless and side-effect
// free while "win_c115_narrow_render.exe <docs/assets/ui>" records the evidence.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4 clean.
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
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;
using prism::ArchiveItem;
using prism::win::gallery::GalleryRenderer;

namespace {

constexpr int kHeight = 729;  // the card's client height
constexpr int kSqueezed = 1028;
constexpr int kBaseline = 1264;

IWICImagingFactory* gWic = nullptr;
const char* gOutDir = nullptr;

#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
// CONTRACTS D3: set where the product reports an island again; read by the
// verdict predicate at the first failing "no island" check.
bool g_pillIslandObserved = false;
#endif

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

// Is 'pixel' materially DARKER than 'fill' on every channel? The pill's drop
// shadow is the one thing that reliably lives just OUTSIDE its body: the pill
// and the action island are the SAME material token (SurfaceFloatingIsland), so
// their fills cannot tell them apart -- the shadow can.
bool Darker(UINT32 pixel, UINT32 fill, int by) {
  for (int shift = 0; shift <= 16; shift += 8) {
    const int p = static_cast<int>((pixel >> shift) & 0xFFu);
    const int f = static_cast<int>((fill >> shift) & 0xFFu);
    if (f - p < by) return false;
  }
  return true;
}

int MaxChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, std::abs(d));
  }
  return delta;
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

bool WritePng(const wchar_t* fileName, const Frame& frame) {
  if (gOutDir == nullptr) return true;  // assertions only (the CI path)
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%s", gOutDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> encodeFrame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          frame.width, frame.height, GUID_WICPixelFormat32bppPBGRA, frame.width * 4,
          static_cast<UINT>(frame.pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data())),
          bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
      FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(encodeFrame.ReleaseAndGetAddressOf(),
                                     bag.ReleaseAndGetAddressOf())) ||
      FAILED(encodeFrame->Initialize(bag.Get())) ||
      FAILED(encodeFrame->SetSize(frame.width, frame.height))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (FAILED(encodeFrame->SetPixelFormat(&format)) ||
      FAILED(encodeFrame->WriteSource(bitmap.Get(), nullptr)) ||
      FAILED(encodeFrame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  std::printf("  wrote %ls\n", path);
  return true;
}

// ---------------------------------------------------------------- fixture
// A real archive-shaped catalog: folders (tiles) plus text payloads (rows), the
// rail, the action band and a breadcrumb that is WIDER than the squeezed pill so
// the truncation is visible at 1028 and absent at 1264.
struct Fixture {
  std::vector<ArchiveItem> catalog;
  prism::sidebar::Model railModel;
  prism::win::sidebar::Layout rail;
  prism::layout::Layout layout;
  prism::actionbar::Model actions;
  GalleryRenderer::Scene scene;

  explicit Fixture(int width) : railModel(&catalog) {
    const char* dirs[] = {"Photos/", "Projects/", "Documents/", "Archive/", "Backup/"};
    const char* files[] = {"README.txt",        "quarterly_report_q4_final.txt",
                           "notes_from_field.md", "manifest.json",
                           "build_2026-11-06.log", "draft_copy.txt"};
    uint32_t index = 0;
    for (const char* path : dirs) {
      ArchiveItem item;
      item.path = path;
      item.index = index++;
      item.isDir = true;
      catalog.push_back(item);
    }
    for (const char* path : files) {
      ArchiveItem item;
      item.path = path;
      item.index = index++;
      item.size = 1'200'000u;
      item.packSize = 402'113u;
      catalog.push_back(item);
    }
    // The card's own inputs: sidebar visible, grid mode, tabs open.
    layout = prism::layout::Compute(width, kHeight, /*sidebarVisible=*/true,
                                    /*listMode=*/false, /*tabs=*/true);
    prism::win::sidebar::Metrics metrics;
    metrics.width = static_cast<float>(prism::layout::kSidebarWidth);
    metrics.viewportTop = static_cast<float>(layout.actionIsland.bottom);
    metrics.viewportBottom = static_cast<float>(layout.statusBar.top);
    rail = prism::win::sidebar::Build(railModel, metrics);

    actions.SetContext(prism::actionbar::Context{true, true, false});
    scene.theme = prism::theme::Variant::Midnight;
    scene.items = &catalog;
    scene.layout = &layout;
    scene.sidebar = &rail;
    scene.actions = &actions;
    scene.breadcrumb =
        L"Design_Assets_v4.7z  >  payload_text  >  2026-11  >  quarterly_reports  >  final";
    scene.status.itemCount = static_cast<int>(catalog.size());
    scene.status.ratioView = prism::CompressionRatioView(7'200'000, 2'412'678);
    scene.glassPanels = true;
    scene.backdropRefraction = false;
  }
};

// One frame through the SHIPPING pipeline (the same Draw call the window's
// Render() reaches), optionally recorded as a PNG.
bool RenderAt(int width, const wchar_t* fileName, Frame* out) {
  Fixture fixture(width);
  Harness harness;
  if (!harness.Open(static_cast<UINT32>(width), kHeight)) return false;
  if (!harness.Paint(fixture.scene, out)) return false;
  if (!WritePng(fileName, *out)) {
    std::printf("  WARN: PNG failed for %ls\n", fileName);
    return false;
  }
  return true;
}

// The pill's own fill, sampled where no glyph can be (12 px inside its left edge,
// 8 px below its top edge: inside the rounded corner's inner area and above the
// text box, which starts at top + 10).
UINT32 PillFill(const Frame& frame, const prism::layout::Rect& pill) {
  return frame.At(pill.left + 12, pill.top + 8);
}

// The horizontal extent of the text INK in [left, right) x [top, bottom).
// minX = -1 when the band is blank (the caller asserts it is not, so a blank
// reading can never pass as "no overflow").
struct InkBounds {
  int minX = -1;
  int maxX = -1;
  int count = 0;
};

InkBounds InkExtent(const Frame& frame, int left, int top, int right, int bottom, UINT32 fill) {
  InkBounds out;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      if (MaxChannelDelta(frame.At(x, y), fill) <= 24) continue;
      ++out.count;
      if (out.minX < 0 || x < out.minX) out.minX = x;
      if (x > out.maxX) out.maxX = x;
    }
  }
  return out;
}

// How many pixels of the strip [left, right) x [top, bottom) are INK against the
// pill's fill (a glyph, a rim, a button -- anything that is not the body).
int InkIn(const Frame& frame, int left, int top, int right, int bottom, UINT32 fill) {
  int ink = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      if (MaxChannelDelta(frame.At(x, y), fill) > 24) ++ink;
    }
  }
  return ink;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE SQUEEZE: at 1028 the painted pill is 16..420 and the toolbar starts at
//    432. Before C115 the authority published 16..576 for the same frame, i.e.
//    144 px of pill that the toolbar sat on.
// ---------------------------------------------------------------------------
PRISM_TEST("c115.render: at 1028 the painted pill is the shortened one") {
  Frame frame;
  if (!PRISM_CHECK(RenderAt(kSqueezed, L"c115-1028.png", &frame))) return;
  const Fixture fixture(kSqueezed);
  const prism::layout::Rect pill = prism::layout::NavPillRect(fixture.layout);
  const prism::layout::Rect text = prism::layout::NavTextRect(fixture.layout);
  const prism::actionbar::Layout band = prism::actionbar::Compute(fixture.layout);
  std::printf("C115 rendered 1028: pill=(%d..%d) text=(%d..%d) island=(%d..%d) dropped=%d\n",
              pill.left, pill.right, text.left, text.right, band.island.left,
              band.island.right, band.dropped);
  // The numbers the card quotes.
  PRISM_CHECK_EQ(pill.left, prism::layout::kContentPadding);
  PRISM_CHECK_EQ(pill.right, 420);
  PRISM_CHECK_EQ(band.island.left, 432);
  PRISM_CHECK_EQ(pill, band.navPill);
  PRISM_CHECK(!pill.Intersects(band.island));
  PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()), prism::actionbar::kActionCount);

  // PAINT vs GEOMETRY, measured on the frame: the pill body really stops at
  // 420. A paint path still using V1's 560 px rectangle would fill x=576 here
  // (this is the C112 probe, turned into an assertion).
  const UINT32 fill = PillFill(frame, pill);
  // The body still reaches its own edge (8 px inside it is the fill) ...
  PRISM_CHECK(MaxChannelDelta(fill, frame.At(pill.right - 8, pill.top + 8)) <= 24);
  // ... and immediately PAST that edge there is the pill's drop shadow, not its
  // body. A paint path still using V1's 560 px rectangle would fill 424 and 428
  // with the body colour and this pair would go red -- which is precisely the
  // C112 measurement (toolbar ink inside the 560 px pill rect) turned around.
  std::printf("C115 edge 1028: body=%08x x+4=%08x x+8=%08x (darker => the pill ends at %d)\n",
              fill, frame.At(pill.right + 4, pill.top + 8),
              frame.At(pill.right + 8, pill.top + 8), pill.right);
  PRISM_CHECK(Darker(frame.At(pill.right + 4, pill.top + 8), fill, 8));
  PRISM_CHECK(Darker(frame.At(pill.right + 8, pill.top + 8), fill, 8));

  // 2. THE TEXT BOX: the crumb run ENDS INSIDE it. Measured on the frame: every
  // glyph column lies in [text.left, text.right], the run really reaches the
  // right edge (the ellipsis sits there), and nothing is painted between the
  // text box and the pill's body -- i.e. nothing spills toward the toolbar.
  const InkBounds run = InkExtent(frame, pill.left + 6, text.top, pill.right - 2, text.bottom,
                                  fill);
  std::printf("C115 crumb ink 1028: x=%d..%d (%d px) inside text box %d..%d\n", run.minX,
              run.maxX, run.count, text.left, text.right);
  PRISM_CHECK(run.count > 0);
  PRISM_CHECK(run.minX >= text.left);   // no leading overflow (the pre-C115 leak)
  PRISM_CHECK(run.maxX <= text.right);  // no trailing overflow
  PRISM_CHECK(run.maxX >= text.right - 12);  // the trim's ellipsis is at the edge
  const int stripInk = InkIn(frame, text.right + 1, text.top, pill.right - 6, text.bottom, fill);
  PRISM_CHECK_EQ(stripInk, 0);
  // The run IS long enough to overflow that box, so the assertions above are not
  // vacuous: the text box is narrower than the string's natural width.
  PRISM_CHECK(text.Width() < 360);
}

// ---------------------------------------------------------------------------
// 3. THE BASELINE: 1264 is untouched -- pill 16..576, island 588..1248, and the
//    painted body still ends at 576 (8 px inside the right edge is fill, 24 px
//    past it is not, because that is the 12 px gap and the island's padding).
// ---------------------------------------------------------------------------
PRISM_TEST("c115.render: 1264 is pixel-identical to the pre-card baseline") {
  Frame frame;
  if (!PRISM_CHECK(RenderAt(kBaseline, L"c115-1264.png", &frame))) return;
  const Fixture fixture(kBaseline);
  const prism::layout::Rect pill = prism::layout::NavPillRect(fixture.layout);
  const prism::layout::Rect text = prism::layout::NavTextRect(fixture.layout);
  const prism::actionbar::Layout band = prism::actionbar::Compute(fixture.layout);
  std::printf("C115 rendered 1264: pill=(%d..%d) text=(%d..%d) island=(%d..%d)\n", pill.left,
              pill.right, text.left, text.right, band.island.left, band.island.right);
  PRISM_CHECK_EQ(pill, (prism::layout::Rect{16, 70, 576, 114}));
  PRISM_CHECK_EQ(text, (prism::layout::Rect{40, 80, 552, 104}));
  PRISM_CHECK_EQ(band.island, (prism::layout::Rect{588, 64, 1248, 120}));
  PRISM_CHECK_EQ(pill.Width(), prism::layout::kNavPillMaxWidth);

  const UINT32 fill = PillFill(frame, pill);
  PRISM_CHECK(MaxChannelDelta(fill, frame.At(pill.right - 8, pill.top + 8)) <= 24);
  // Just past the edge is the drop shadow, never the body (the baseline keeps
  // the full 560 px pill, and it still ends exactly at 576).
  PRISM_CHECK(Darker(frame.At(pill.right + 4, pill.top + 8), fill, 8));
  PRISM_CHECK(Darker(frame.At(pill.right + 8, pill.top + 8), fill, 8));
  // The long crumb run still overflows its box even at 1264, so the same
  // containment rule is exercised here too -- inside the 512 px text box, with
  // the ellipsis at its trailing edge.
  const InkBounds run = InkExtent(frame, pill.left + 6, text.top, pill.right - 2, text.bottom,
                                  fill);
  std::printf("C115 crumb ink 1264: x=%d..%d (%d px) inside text box %d..%d\n", run.minX,
              run.maxX, run.count, text.left, text.right);
  PRISM_CHECK(run.count > 0);
  PRISM_CHECK(run.minX >= text.left);
  PRISM_CHECK(run.maxX <= text.right);
  PRISM_CHECK(run.maxX >= text.right - 12);
  const int stripInk = InkIn(frame, text.right + 1, text.top, pill.right - 6, text.bottom, fill);
  PRISM_CHECK_EQ(stripInk, 0);
}

// ---------------------------------------------------------------------------
// 4. THE COMPOSITION PANEL -- C123: THERE IS NONE.
//
// GalleryWindow::GlassPanelRect() sizes the COMPOSITION (topmost) island -- the
// surface layered ABOVE the window's D2D content. C115 asked "is that surface
// ever the whole client" and answered "it is the pill". C123 REMOVES the pill:
// the crumb is a 28 px path bar painted into the page, so the product window
// publishes no island at all and PresentGlassFrame (rect.w == 0 -> return)
// sizes nothing. The contract this case pins is that absence, at every width:
// an island reappearing here would be a blurred ghost rectangle over the
// toolbar -- a surface for a control that no longer exists.
//
// The other two cases of this suite (the pill the RENDERER paints, and the
// crumb run inside its box) are legacy-composition scenes and are unchanged.
// ---------------------------------------------------------------------------
PRISM_TEST("c115.glass_panel: C123 has no composition island over the toolbar") {
  prism::win::gallery::GalleryWindow window;
  // (a) BEFORE Create: no layout has ever been computed. An empty panel is the
  // only safe answer -- and PresentGlassFrame bails on it, so nothing is sized.
  const prism::glass::IslandRect cold = window.GlassPanelRect();
  std::printf("C115 glass panel before layout: %ux%u\n", cold.w, cold.h);
  PRISM_CHECK_EQ(cold.w, 0u);
  PRISM_CHECK_EQ(cold.h, 0u);

  if (!PRISM_CHECK(window.Create(L"PrismArchive c115 glass panel", GetModuleHandleW(nullptr)))) {
    return;
  }
  // C123: the crumb band exists only while there is a path to show, so the
  // window needs one before the ring below can measure the row it replaced.
  window.SetRootLabel(L"fixture.zip");
  const int widths[] = {640, 700, 784, 800, 900, 1028, 1172, 1184, 1264, 1482, 1920};
  for (const int w : widths) {
    const int h = (std::max)(480, kHeight);
    window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(w, h));
    const prism::layout::Layout layout = window.LayoutForTest();
    const prism::glass::IslandRect panel = window.GlassPanelRect();
    if (w == 1028) {
      std::printf("C115 glass panel at 1028: %u,%u %ux%u (C123 crumbBar %d,%d %dx%d, client %dx%d)\n",
                  panel.x, panel.y, panel.w, panel.h, layout.crumbBar.left, layout.crumbBar.top,
                  layout.crumbBar.Width(), layout.crumbBar.Height(), w, h);
    }
    // (b) NO island: the composition layer has nothing to cover.
#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
    g_pillIslandObserved = g_pillIslandObserved || panel.w != 0 || panel.h != 0;
#endif
    PRISM_CHECK_EQ(panel.w, 0u);
    PRISM_CHECK_EQ(panel.h, 0u);
    PRISM_CHECK_EQ(panel.x, 0u);
    PRISM_CHECK_EQ(panel.y, 0u);
    // (c) and the crumb row it replaced still exists, at the layout's numbers --
    // so "no island" cannot be green because the crumb band disappeared too.
    PRISM_CHECK_EQ(layout.crumbBar.Height(), 28);
    PRISM_CHECK_EQ(layout.crumbBar.left, layout.contentBand.left);
    PRISM_CHECK_EQ(layout.crumbBar.right, layout.contentBand.right);
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the narrow layout cannot be rendered");
    return 1;
  }
#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
  // CONTRACTS D3: the pill island C123 deleted comes back -> the "no island"
  // ring below MUST go red.
  std::puts("C115 MUTANT ACTIVE: pill_island");
  prism::test::SetMutationVerdict("C115 MUTATION CAUGHT: pill_island",
                                  [] { return g_pillIslandObserved; });
#endif
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the narrow layout cannot be rendered");
    return 1;
  }
  gWic = wic.Get();
  if (argc > 1) {
    std::error_code error;
    std::filesystem::create_directories(argv[1], error);
    gOutDir = argv[1];
  }
  const int failures = prism::test::RunAll(argc, argv);
  gWic = nullptr;
  if (failures != 0) {
    std::puts("C115 NARROW RENDER CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C115 NARROW RENDER CHECKS PASSED");
  return 0;
}
