// PrismArchive -- PrismWin
// C123: THE TOP SEAM through the REAL window.
//
// The pure suite (Tests/test_c123_layout.cpp) pins the geometry on every
// platform. This one proves the WINDOW composes that geometry -- the same
// rectangles its hit test resolves and its renderer paints:
//
//   1. tabs on demand: no strip with one archive (0 px of chrome), a 32 px
//      strip from the second one on, and the toolbar follows it down;
//   2. the toolbar the window hit-tests is the FLUSH band: its left and right
//      edges are the content band's, and the OPEN button's centre is inside the
//      proposal's red line (x <= 60, y <= 110);
//   3. the crumb is a 28 px path bar under the toolbar, its text box starting
//      at contentBand.left + 16, with every clickable span inside that box;
//   4. PIXELS: a captured frame really paints the strip edge to edge -- the
//      band's first and last columns carry the island material, not the canvas.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4 clean.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"  // C123: the refraction setting
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;

namespace {

constexpr int kSkipReturnCode = 77;
GalleryWindow* g_window = nullptr;
std::vector<prism::ArchiveItem> g_catalog;

#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
// Runtime proof the injected pill island really came back (CONTRACTS D3): it is
// only set where the product reports a non-empty panel, and only then may the
// first failing "no island" check print the CAUGHT marker.
bool g_pillIslandObserved = false;
#endif

void Add(const char* path, bool dir, uint64_t size) {
  prism::ArchiveItem item;
  item.index = static_cast<uint32_t>(g_catalog.size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  g_catalog.push_back(item);
}

void BuildCatalog() {
  Add("docs", true, 0);
  Add("docs/readme.md", false, 53);
  Add("photos", true, 0);
}

void ResetTabs() {
  while (!g_window->Tabs().empty()) {
    (void)g_window->CloseTab(g_window->Tabs().activeIndex(), /*confirmed=*/true);
  }
}

struct OpenButton {
  int x = -1;
  int y = -1;
  prism::layout::Rect rect;
  bool found = false;
};

OpenButton FindOpen(const prism::actionbar::Layout& band) {
  OpenButton out;
  for (const prism::actionbar::Button& button : band.buttons) {
    if (button.id != prism::actionbar::ActionId::Open) continue;
    out.rect = button.rect;
    out.x = (button.rect.left + button.rect.right) / 2;
    out.y = (button.rect.top + button.rect.bottom) / 2;
    out.found = true;
    break;
  }
  return out;
}

int ChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, d < 0 ? -d : d);
  }
  return delta;
}

// Renders the window's OWN frame (RenderSceneForCapture) into an offscreen WIC
// target and hands back the pixels. Returns false when the device path is not
// available -- the caller reports a skip, never a pass.
bool CaptureWindow(UINT32 width, UINT32 height, std::vector<UINT32>* pixels) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return false;
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1Factory> factory;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               factory.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                  target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (!g_window->RenderSceneForCapture(target.Get())) return false;
  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
    return false;
  }
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  pixels->resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(pixels->data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Tabs on demand, through the window's own layout.
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window t1: one archive leaves no tab band, two do") {
  ResetTabs();
  g_window->SetItems(&g_catalog);
  g_window->SetRootLabel(L"fixture.zip");
  const prism::layout::Layout none = g_window->LayoutForTest();
  PRISM_CHECK(none.tabBar.Empty());
  PRISM_CHECK_EQ(none.toolbar.top, prism::layout::kTitleBarHeight);

  const prism::tabs::OpenOutcome first = g_window->OpenTab("C:\\work\\fixture.zip");
  PRISM_CHECK_EQ(first.verdict, prism::tabs::TabVerdict::kOk);
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(1));
  const prism::layout::Layout one = g_window->LayoutForTest();
  PRISM_CHECK(one.tabBar.Empty());
  PRISM_CHECK_EQ(one.tabBar.Height(), 0);
  PRISM_CHECK_EQ(one.toolbar.top, prism::layout::kTitleBarHeight);

  const prism::tabs::OpenOutcome second = g_window->OpenTab("C:/work/second.zip");
  PRISM_CHECK_EQ(second.verdict, prism::tabs::TabVerdict::kOk);
  const prism::layout::Layout two = g_window->LayoutForTest();
  PRISM_CHECK(!two.tabBar.Empty());
  PRISM_CHECK_EQ(two.tabBar.Height(), 32);
  PRISM_CHECK_EQ(two.toolbar.top, two.tabBar.bottom);
  std::printf("C123 window T1: 1 archive toolbar.top=%d, 2 archives toolbar.top=%d\n",
              one.toolbar.top, two.toolbar.top);
}

// ---------------------------------------------------------------------------
// 2. T2/T3 through the window: the band the hit test resolves is flush, and
//    the OPEN button sits at the corner.
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window t2/t3: flush toolbar, OPEN at the corner") {
  for (const int tabs : {1, 2}) {
    ResetTabs();
    for (int i = 0; i < tabs; ++i) {
      (void)g_window->OpenTab(i == 0 ? "C:\\work\\fixture.zip" : "C:/work/second.zip");
    }
    const prism::layout::Layout layout = g_window->LayoutForTest();
    const prism::actionbar::Layout band = g_window->ActionBarLayout();
    std::printf("C123 window T2: tabs=%d band=(%d..%d) contentBand=(%d..%d)\n", tabs,
                band.band.left, band.band.right, layout.contentBand.left,
                layout.contentBand.right);
    PRISM_CHECK(prism::actionbar::BandIsFlush(band));
    PRISM_CHECK_EQ(band.band.left, layout.contentBand.left);
    PRISM_CHECK_EQ(band.band.right, layout.contentBand.right);
    PRISM_CHECK_EQ(band.band.right, layout.windowW);
    PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()), prism::actionbar::kActionCount);

    const OpenButton open = FindOpen(band);
    PRISM_CHECK(open.found);
    std::printf("C123 window T2: open centre=(%d,%d)\n", open.x, open.y);
    PRISM_CHECK(open.x >= 0 && open.x <= 60);
    PRISM_CHECK(open.y >= 0 && open.y <= 110);
    // The hit test resolves the SAME pixel to the OPEN button.
    PRISM_CHECK_EQ(g_window->ActionBarButtonAt(open.x, open.y), 0);
  }
}

// ---------------------------------------------------------------------------
// 3. T4/T5 through the window: the crumb is a 28 px path bar, left-aligned.
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window t4/t5: the crumb path bar is left-aligned and clear of the bands") {
  ResetTabs();
  g_window->SetItems(&g_catalog);
  g_window->SetRootLabel(L"fixture.zip");
  (void)g_window->OpenTab("C:\\work\\fixture.zip");
  const prism::layout::Layout layout = g_window->LayoutForTest();
  const prism::actionbar::Layout band = g_window->ActionBarLayout();
  const prism::layout::Rect bar = layout.crumbBar;
  std::printf("C123 window T4: crumbBar=(%d,%d..%d,%d) textLeft=%d\n", bar.left, bar.top,
              bar.right, bar.bottom, g_window->NavBarTextLeft());
  PRISM_CHECK(!bar.Empty());
  PRISM_CHECK_EQ(bar.Height(), 28);
  PRISM_CHECK_EQ(band.band.Intersects(bar) ? 1 : 0, 0);           // T5: no toolbar overlap
  PRISM_CHECK_EQ(bar.Intersects(layout.contentBand) ? 1 : 0, 0);  // T5: no content overlap
  // The window's own crumb geometry is the layout's (not a second copy).
  PRISM_CHECK_EQ(g_window->NavBarTop(), bar.top);
  PRISM_CHECK_EQ(g_window->NavBarBottom(), bar.bottom);
  PRISM_CHECK_EQ(g_window->NavBarTextLeft(), layout.contentBand.left + 16);

  // Every clickable span is inside the text box, and the root crumb is really
  // where the left edge says -- a CENTRED run would start (box - run) / 2 px in.
  int left = 0;
  int right = 0;
  PRISM_CHECK(g_window->CrumbSpanForTest(prism::tree::kRootLevel, &left, &right));
  std::printf("C123 window T4: root span=(%d..%d) textBox.left=%d\n", left, right,
              g_window->NavBarTextLeft());
  PRISM_CHECK_EQ(left, g_window->NavBarTextLeft());
  PRISM_CHECK(right > left);
  PRISM_CHECK_EQ(g_window->CrumbLevelAt((left + right) / 2, bar.top + bar.Height() / 2),
                 prism::tree::kRootLevel);
}

// ---------------------------------------------------------------------------
// 4. PIXELS: the strip really reaches both window edges.
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window pixels: the toolbar is painted edge to edge") {
  ResetTabs();
  (void)g_window->OpenTab("C:\\work\\fixture.zip");
  const prism::layout::Layout layout = g_window->LayoutForTest();
  if (layout.windowW <= 0 || layout.windowH <= 0) {
    std::puts("  [skip] the window has no client size yet");
    return;
  }
  if (g_window->DpiForTest() != 96) {
    // The frame is rendered in PIXELS and the layout is DIPs; on a HiDPI runner
    // the band's columns are scaled and this sampling would measure a different
    // row. Named, never silent (D1).
    std::printf("  [skip] DPI is %d, not 96: the pixel sample is DIP-only\n",
                g_window->DpiForTest());
    return;
  }
  const UINT32 width = static_cast<UINT32>(layout.windowW);
  const UINT32 height = static_cast<UINT32>(layout.windowH);
  std::vector<UINT32> pixels;
  if (!CaptureWindow(width, height, &pixels)) {
    std::puts("  [skip] the device path could not capture the frame");
    return;
  }
  const auto at = [&](int x, int y) {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  };
  const int bandY = (layout.toolbar.top + layout.toolbar.bottom) / 2;
  // The painter's own counter: the band really reached the device. Without it
  // the three colour samples below could agree because nothing was painted --
  // the same "a canvas-only frame passes a colour test" trap C113 records.
  const GalleryWindow::ActionPaintCounters painted = g_window->PaintedActionCounters();
  PRISM_CHECK(painted.islands >= 1);
  PRISM_CHECK_EQ(painted.buttons, prism::actionbar::kActionCount);

  const UINT32 farLeft = at(1, bandY);
  const UINT32 farRight = at(static_cast<int>(width) - 2, bandY);
  // A reference column INSIDE the strip but to the RIGHT of the last button:
  // the material the band paints where no button can reach. The eight buttons
  // end at layout.toolbar.left + 8 + 8 * kButtonMaxWidth + gaps << width, so a
  // 40 px inset is always clear of them.
  const UINT32 reference = at(static_cast<int>(width) - 40, bandY);
  const UINT32 canvas = at(1, layout.titleBar.top + 4);
  std::printf("C123 window pixels: left=%08X right=%08X reference=%08X canvas=%08X\n", farLeft,
              farRight, reference, canvas);
  // Both extremes carry the SAME material as the empty part of the strip: the
  // toolbar is one rectangle from the window's left edge to its right, not a
  // floating island that starts at 589.
  PRISM_CHECK(ChannelDelta(farLeft, farRight) <= 3);
  PRISM_CHECK(ChannelDelta(farLeft, reference) <= 3);
  PRISM_CHECK(ChannelDelta(farRight, reference) <= 3);
  // ...and the strip is not the canvas, so "flush" cannot be green because the
  // band was never painted at all (the island counter above already rules that
  // out; this keeps the two facts independent).
  PRISM_CHECK(ChannelDelta(farLeft, canvas) > 2);
}

// ---------------------------------------------------------------------------
// 5. THE ISLAND'S RECTANGLE. C123 rewrote this case to pin "no island"; C126
//    gives the island a HOST, so the case is rewritten AGAIN -- and every
//    changed assertion is listed here, one line each, because an assertion that
//    is quietly edited is an assertion nobody can trust (CONTRACTS D3):
//
//      file:line (pre-C126)  old assertion                 why it is a DEFECT now
//      --------------------  ----------------------------  -------------------------------
//      test_c123_window.cpp:327  PRISM_CHECK_EQ(panel.x, 0u)   C126's whole point is that the
//      test_c123_window.cpp:328  PRISM_CHECK_EQ(panel.y, 0u)   island covers the toolbar band;
//      test_c123_window.cpp:329  PRISM_CHECK_EQ(panel.w, 0u)   "0x0" was a CORRECT intermediate
//      test_c123_window.cpp:330  PRISM_CHECK_EQ(panel.h, 0u)   state, not a contract.
//
//    NEW assertion: the panel IS prism::layout::ToolbarBandRect -- not a second
//    rectangle, not the deleted pill, not 0x0 -- and the 28 px crumb row it must
//    never cover still exists. WHAT MAKES IT RED: the registered D3 arm
//    (PRISM_C123_MUTANT_PILL_ISLAND) returns the deleted pill (16,38 560x44), so
//    the band equality fails and the entry prints
//    "C123 MUTATION CAUGHT: pill_island".
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window island: the composition surface IS the toolbar band") {
  ResetTabs();
  (void)g_window->OpenTab("C:\\work\\fixture.zip");
  const prism::layout::Layout layout = g_window->LayoutForTest();
  const prism::layout::Rect toolbar = prism::layout::ToolbarBandRect(layout);
  const prism::glass::IslandRect panel = g_window->GlassPanelRect();
  std::printf("C123 window island: %u,%u %ux%u (toolbar band %d,%d %dx%d)\n", panel.x,
              panel.y, panel.w, panel.h, toolbar.left, toolbar.top, toolbar.Width(),
              toolbar.Height());
#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
  // CONTRACTS D3: the injected pill is really back when the product publishes a
  // rectangle that is NOT the band (a 44 px pill at x = 16 instead of 0..W).
  g_pillIslandObserved = !toolbar.Empty() &&
                         (static_cast<int>(panel.x) != toolbar.left ||
                          static_cast<int>(panel.y) != toolbar.top ||
                          static_cast<int>(panel.w) != toolbar.Width() ||
                          static_cast<int>(panel.h) != toolbar.Height());
#endif
  PRISM_CHECK(!toolbar.Empty());
  PRISM_CHECK_EQ(static_cast<int>(panel.x), toolbar.left);
  PRISM_CHECK_EQ(static_cast<int>(panel.y), toolbar.top);
  PRISM_CHECK_EQ(static_cast<int>(panel.w), toolbar.Width());
  PRISM_CHECK_EQ(static_cast<int>(panel.h), toolbar.Height());
  // ... and the row it must never cover still exists (so this cannot be green
  // because the crumb band disappeared too), and the two do not intersect.
  PRISM_CHECK_EQ(layout.crumbBar.Height(), 28);
  PRISM_CHECK_EQ(toolbar.Intersects(layout.crumbBar) ? 1 : 0, 0);
}

// ---------------------------------------------------------------------------
// 6. appearance.backdropRefraction must not silently become a no-op.
//
// C123 recorded this because the island had no host: the setting still gated the
// panel language (rail, cards, band chrome), so it moved pixels -- but only
// 1,236 of them inside the toolbar band, and none of them by REFRACTION.
//
// C126 OWNS THAT NUMBER NOW. This case is kept as the floor it always was
// ("turning the material off changes the frame"), with NO glass layer in this
// window: it measures the window-only path. The requirement that the in-band
// count GROW -- the reason this card exists -- is c126 G4, on the real
// composition path (> 1,236 in-band pixels with the island live). Splitting the
// two keeps each assertion honest: the floor lives where the window is, the
// growth lives where the island is, and neither can be satisfied by the other.
// ---------------------------------------------------------------------------
PRISM_TEST("c123.window refraction: the appearance setting still reaches pixels") {
  ResetTabs();
  (void)g_window->OpenTab("C:\\work\\fixture.zip");
  const prism::layout::Layout layout = g_window->LayoutForTest();
  if (layout.windowW <= 0 || layout.windowH <= 0) {
    std::puts("  [skip] the window has no client size yet");
    return;
  }
  if (g_window->DpiForTest() != 96) {
    std::printf("  [skip] DPI is %d, not 96: the pixel sample is DIP-only\n",
                g_window->DpiForTest());
    return;
  }
  const UINT32 width = static_cast<UINT32>(layout.windowW);
  const UINT32 height = static_cast<UINT32>(layout.windowH);
  const bool was = prism::theme::BackdropRefractionEnabled();
  prism::theme::live::SetBackdropRefraction(true);
  std::vector<UINT32> on;
  const bool capturedOn = CaptureWindow(width, height, &on);
  prism::theme::live::SetBackdropRefraction(false);
  std::vector<UINT32> off;
  const bool capturedOff = CaptureWindow(width, height, &off);
  prism::theme::live::SetBackdropRefraction(was);
  if (!capturedOn || !capturedOff) {
    std::puts("  [skip] the device path could not capture both frames");
    return;
  }
  int changed = 0;
  int changedInBand = 0;
  int maxDelta = 0;
  for (int y = 0; y < static_cast<int>(height); ++y) {
    for (int x = 0; x < static_cast<int>(width); ++x) {
      const size_t i = static_cast<size_t>(y) * width + static_cast<size_t>(x);
      const int d = ChannelDelta(on[i], off[i]);
      if (d == 0) continue;
      ++changed;
      maxDelta = (std::max)(maxDelta, d);
      if (y >= layout.toolbar.top && y < layout.toolbar.bottom) ++changedInBand;
    }
  }
  std::printf("C123 refraction: %d pixels differ (max channel delta %d), %d of them inside "
              "the toolbar band of %ux%u\n",
              changed, maxDelta, changedInBand, width, height);
  // The setting is NOT a no-op: it still gates the panel material (the rail, the
  // action strip, the cards), which is why turning it off changes pixels. C126
  // landed the other half (the refraction the island applies to the toolbar
  // band); its growth is asserted by c126 G4, and the floor asserted here stays
  // "never zero" so a future card cannot make the setting decorative.
  PRISM_CHECK(changed > 0);
  PRISM_CHECK(maxDelta > 2);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("SKIP: CoInitializeEx failed (this window station has no COM)\n");
    return kSkipReturnCode;
  }
  BuildCatalog();
  static GalleryWindow window;
  if (!window.Create(L"PrismArchive c123 seam", GetModuleHandleW(nullptr))) {
    std::printf("SKIP: window creation failed (no interactive window station)\n");
    return kSkipReturnCode;
  }
  g_window = &window;
#if defined(PRISM_C123_MUTANT_PILL_ISLAND)
  std::printf("C123 MUTANT ACTIVE: pill_island\n");
  prism::test::SetMutationVerdict("C123 MUTATION CAUGHT: pill_island",
                                  [] { return g_pillIslandObserved; });
#endif

  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::printf("ALL C123 WINDOW CHECKS PASSED (%d checks)\n", prism::test::CheckCount());
  ::CoUninitialize();
  return 0;
}
