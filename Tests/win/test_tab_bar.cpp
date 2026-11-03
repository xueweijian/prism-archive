// PrismArchive -- PrismWin
// C60: THE TAB STRIP on a real HWND -- "[C59] may open several archives, but the
// window still shows one at a time" is what this suite closes.
//
// C59 proved the MODEL (TabSet: 16 tabs, one tab per normalized path, close
// activates the right neighbour, dirty tabs refuse, session round-trip). This
// card draws and drives it, so the seven cases below assert the things that
// only exist once there is a strip:
//
//   1. two opened archives -> TWO tabs, the second one active, the strip
//      occupying DESIGN.md §2.1's 32 px band between the title and the action
//      island -- asserted on the window's own layout, then on PIXELS (the
//      active plate is the SurfaceFloatingIsland token, an inactive one is the
//      canvas token);
//   2. clicking the first tab switches the active tab AND asks the app for that
//      archive (the same call C34's SwitchArchive path makes), so the content
//      really follows;
//   3. clicking the x of the ACTIVE tab removes it and activates the RIGHT
//      neighbour (C59's rule, now visible);
//   4. a dirty tab's x asks C54's confirmation box; "cancel" keeps the tab --
//      and the model the product shows is C54's EditDialogModel, driven here
//      through C54's own EditConfirmDialog;
//   5. Ctrl+Tab / Ctrl+Shift+Tab cycle and wrap;
//   6. a drag reorder changes the ORDER while every tab keeps its path (and the
//      active tab stays the same archive);
//   7. with no archive open the strip is NOT rendered: no tab surface and no
//      accent reaches the band, and the strip reserves no space at all.
//
// Verdict rules: the stdout marker is a CONJUNCTION with exit 0 (CONTRACTS D1);
// a machine without a window station prints WHY and exits 77 (SKIP_RETURN_CODE).
// The two negative controls are D27 mutants: the injection banner proves the
// macro compiled in, the armed verdict prints "C60 MUTATION CAUGHT: <id>" at the
// FIRST failed assertion, and the target's PASS_REGULAR_EXPRESSION is that
// marker (never WILL_FAIL -- CONTRACTS D26).
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/EditConfirmDialog.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/TabBar.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryRenderer;
using prism::win::gallery::GalleryWindow;

namespace {

// ---------------------------------------------------------------- mutants
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR)
// C60 MUTANT ACTIVE: close_first_tab -- C59's injected "closing activates tab 0"
// rule is compiled in, so case 3 (right neighbour) MUST go red.
constexpr const char* kMutation = "close_first_tab";
#elif defined(PRISM_C60_MUTANT_DIRTY_NO_CONFIRM)
// C60 MUTANT ACTIVE: dirty_no_confirm -- a dirty tab closes without asking, so
// case 4 (cancel keeps the tab) MUST go red.
constexpr const char* kMutation = "dirty_no_confirm";
#else
constexpr const char* kMutation = nullptr;
#endif

// D3: the injection must PROVE it fired; a stale macro would let the mutant
// target pass while claiming to test something. These read the same constexpr
// the product code branches on, so they cannot drift from it.
[[maybe_unused]] bool CloseFirstTabInjectionFired() {
  return prism::win::tabbar::ActiveMutation() == "close_first_tab";
}
[[maybe_unused]] bool DirtyNoConfirmInjectionFired() {
  return prism::win::tabbar::ActiveMutation() == "dirty_no_confirm";
}

constexpr int kSkipReturnCode = 77;

GalleryWindow* g_window = nullptr;

std::vector<prism::ArchiveItem> g_catalogAlpha;
std::vector<prism::ArchiveItem> g_catalogBeta;

std::string g_requested;   // the path the window asked the app to show
int g_requests = 0;

std::string ToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                      needed, nullptr, nullptr);
  return utf8;
}

// ------------------------------------------------------- pixel fixtures
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

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

UINT32 ToPixel(const prism::theme::Rgba& c) {
  const auto byte = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<UINT32>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return (byte(c.a) << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

prism::theme::Rgba Token(prism::theme::TokenId id) {
  return prism::theme::Lookup(id, prism::theme::Variant::Midnight).color;
}

// The renderer clears to kBackdrop (opaque) before any band is painted, so every
// semi-transparent token lands ON that colour. Computing the expectation the
// same way D2D composites it is what makes the assertion "the token reached the
// pixel" instead of "some dark colour is there".
constexpr prism::theme::Rgba kBackdropPixel{0.043f, 0.055f, 0.075f, 1.0f};

UINT32 Backdrop() { return ToPixel(kBackdropPixel); }
UINT32 CanvasOverBackdrop() {
  return ToPixel(prism::theme::CompositeOver(Token(prism::theme::TokenId::SurfaceCanvasBase),
                                            kBackdropPixel));
}
UINT32 IslandOverCanvas() {
  return ToPixel(prism::theme::CompositeOver(
      Token(prism::theme::TokenId::SurfaceFloatingIsland),
      prism::theme::CompositeOver(Token(prism::theme::TokenId::SurfaceCanvasBase),
                                  kBackdropPixel)));
}
UINT32 AccentPixel() {
  return ToPixel(prism::theme::Lookup(prism::theme::TokenId::AccentLiquidPrimary,
                                      prism::theme::Variant::Midnight)
                     .gradient.from);
}

// Renders one scene through the REAL renderer into an offscreen WIC target.
bool RenderTabs(const prism::win::tabbar::View& view, const prism::layout::Layout& layout,
                Frame* frame, int* tabsPainted) {
  if (layout.windowW <= 0 || layout.windowH <= 0) return false;
  const UINT32 width = static_cast<UINT32>(layout.windowW);
  const UINT32 height = static_cast<UINT32>(layout.windowH);

  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return false;
  }
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
          bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;

  GalleryRenderer::Scene scene;
  scene.layout = &layout;
  scene.items = nullptr;                     // the strip is what is under test
  scene.tabs = view.items.empty() ? nullptr : &view;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  if (tabsPainted != nullptr) *tabsPainted = renderer.TabBarTabsPainted();
  return true;
}

// ------------------------------------------------------------- fixtures
void Add(std::vector<prism::ArchiveItem>* items, const char* path, bool dir, uint64_t size) {
  prism::ArchiveItem item;
  item.index = static_cast<uint32_t>(items->size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  items->push_back(item);
}

// Two catalogs that cannot be confused: the app-side switch is proven by the
// ROW COUNT, not by a label both archives could share.
void BuildCatalogs() {
  Add(&g_catalogAlpha, "alpha", true, 0);
  Add(&g_catalogAlpha, "alpha/one.txt", false, 10);
  Add(&g_catalogBeta, "beta", true, 0);
  Add(&g_catalogBeta, "beta/two.txt", false, 20);
  Add(&g_catalogBeta, "beta/three.bin", false, 30);
}

// The app's side of the contract, in the shape Main.cpp uses it: "show this
// archive" means attach ITS catalog and label. No engine here -- the strip is
// what is under test, and case 2 asserts this call was made with the right path.
void ApplySwitch(const std::string& path) {
  if (path == "C:/work/alpha.7z") {
    g_window->SetItems(&g_catalogAlpha);
    g_window->SetRootLabel(L"alpha.7z");
  } else if (path == "C:/work/beta.7z") {
    g_window->SetItems(&g_catalogBeta);
    g_window->SetRootLabel(L"beta.7z");
  }
}

void ResetTabs() {
  while (!g_window->Tabs().empty()) {
    (void)g_window->CloseTab(g_window->Tabs().activeIndex(), /*confirmed=*/true);
  }
  // Reset AFTER the closes: every close of the active tab notifies the app, and
  // a counter cleared before them would count the reset itself.
  g_requested.clear();
  g_requests = 0;
}

// The pointer coordinates of the MIDDLE of tab 'index' (away from the x), from
// the rectangles the window itself painted.
POINT TabCentre(int index) {
  const std::vector<prism::win::tabbar::TabGeometry> rects = g_window->TabBarRects();
  if (index < 0 || index >= static_cast<int>(rects.size())) return POINT{-1, -1};
  const prism::layout::Rect r = rects[static_cast<size_t>(index)].rect;
  return POINT{r.left + r.Width() / 4, r.top + r.Height() / 2};
}

POINT CloseCentre(int index) {
  const std::vector<prism::win::tabbar::TabGeometry> rects = g_window->TabBarRects();
  if (index < 0 || index >= static_cast<int>(rects.size())) return POINT{-1, -1};
  const prism::layout::Rect r = rects[static_cast<size_t>(index)].close;
  return POINT{(r.left + r.right) / 2, (r.top + r.bottom) / 2};
}

LRESULT Click(POINT p) {
  const LPARAM packed =
      static_cast<LPARAM>(MAKELONG(static_cast<short>(p.x), static_cast<short>(p.y)));
  return g_window->Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, packed);
}

std::string PathAt(int index) {
  const prism::tabs::Tab* tab = g_window->Tabs().At(index);
  return tab != nullptr ? tab->path : std::string("<none>");
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Two archives -> two tabs, the second active, the strip painted.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 open two archives: two tabs, second active, strip painted") {
  ResetTabs();
  const prism::tabs::OpenOutcome first = g_window->OpenTab("C:\\work\\alpha.7z");
  const prism::tabs::OpenOutcome second = g_window->OpenTab("C:/work/beta.7z");
  PRISM_CHECK_EQ(first.verdict, prism::tabs::TabVerdict::kOk);
  PRISM_CHECK_EQ(second.verdict, prism::tabs::TabVerdict::kOk);
  PRISM_CHECK(second.created);
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(g_window->Tabs().At(0)->displayName, std::string("alpha.7z"));
  PRISM_CHECK_EQ(g_window->Tabs().At(1)->displayName, std::string("beta.7z"));
  PRISM_CHECK(g_window->TabBarVisible());

  // DESIGN.md §2.1: 32 px, between the title and the action island.
  const prism::layout::Layout& layout = g_window->LayoutForTest();
  PRISM_CHECK_EQ(layout.tabBar.top, prism::layout::kTitleBarHeight);
  PRISM_CHECK_EQ(layout.tabBar.Height(), prism::layout::kTabBarHeight);
  PRISM_CHECK_EQ(layout.actionIsland.top, 32 + 32);
  PRISM_CHECK_EQ(layout.tabBar.bottom, layout.actionIsland.top);
  PRISM_CHECK_EQ(layout.tabBar.Width(), layout.windowW);

  const std::vector<prism::win::tabbar::TabGeometry> rects = g_window->TabBarRects();
  PRISM_CHECK_EQ(rects.size(), static_cast<std::size_t>(2));
  // The three hit tests agree with the painted rectangles.
  const POINT firstTab = TabCentre(0);
  PRISM_CHECK_EQ(g_window->TabBarTabAt(firstTab.x, firstTab.y), 0);
  PRISM_CHECK_EQ(g_window->TabBarTabAt(TabCentre(1).x, TabCentre(1).y), 1);
  const POINT firstClose = CloseCentre(0);
  PRISM_CHECK_EQ(g_window->TabBarCloseAt(firstClose.x, firstClose.y), 0);
  PRISM_CHECK_EQ(g_window->TabBarTabAt(firstClose.x, firstClose.y), 0);  // x is inside the tab
  // ... and the x's hit box is INSIDE its tab (a click on it must still be a
  // close, never a switch).
  PRISM_CHECK(rects[0].rect.Contains(rects[0].close));

  // PIXELS: the active plate is the floating-island token, the inactive one the
  // canvas token, and both reached the device.
  const prism::win::tabbar::View view = g_window->TabBarView();
  Frame frame;
  int painted = 0;
  PRISM_CHECK(RenderTabs(view, layout, &frame, &painted));
  PRISM_CHECK_EQ(painted, 2);
  const auto plateProbe = [&](int index) {
    const prism::layout::Rect r = rects[static_cast<size_t>(index)].rect;
    // 5 px inside the plate: right of the rim, left of the title's 12 px inset,
    // so nothing but the plate's own fill can be at that pixel.
    return frame.At(r.left + 5, r.top + r.Height() / 2);
  };
  PRISM_CHECK(ColorDistance(plateProbe(1), IslandOverCanvas()) <= 2);
  PRISM_CHECK(ColorDistance(plateProbe(0), CanvasOverBackdrop()) <= 2);
  PRISM_CHECK(ColorDistance(IslandOverCanvas(), CanvasOverBackdrop()) > 8);
  // The strip's own background (between the two tabs) is the canvas token.
  const int gapX = rects[1].rect.left - 1;
  PRISM_CHECK(ColorDistance(frame.At(gapX, rects[1].rect.top + 5), CanvasOverBackdrop()) <= 2);
}

// ---------------------------------------------------------------------------
// 2. Click the first tab -> active switches AND the app is asked for it.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 click the first tab: active switches and the content follows") {
  ResetTabs();
  g_window->OpenTab("C:\\work\\alpha.7z");
  g_window->OpenTab("C:/work/beta.7z");
  ApplySwitch("C:/work/beta.7z");
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(g_window->ItemCount(), static_cast<int>(g_catalogBeta.size()));

  const POINT firstTab = TabCentre(0);
  PRISM_CHECK_EQ(g_window->TabBarTabAt(firstTab.x, firstTab.y), 0);
  Click(firstTab);

  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);
  // The window asked for the archive it now shows -- normalized to '/' (C59's
  // path identity), which is what C34's SwitchArchive receives.
  PRISM_CHECK_EQ(g_requests, 1);
  PRISM_CHECK_EQ(g_requested, std::string("C:/work/alpha.7z"));
  // The app answered that call; the rows are the OTHER archive's now.
  PRISM_CHECK_EQ(g_window->ItemCount(), static_cast<int>(g_catalogAlpha.size()));
  PRISM_CHECK_EQ(ToUtf8(g_window->BreadcrumbText()).substr(0, 5), std::string("alpha"));
}

// ---------------------------------------------------------------------------
// 3. Close the ACTIVE tab -> the right neighbour becomes active.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 close the active tab: the right neighbour takes its place") {
  ResetTabs();
  g_window->OpenTab("C:/t/one.7z");
  g_window->OpenTab("C:/t/two.7z");
  g_window->OpenTab("C:/t/three.7z");
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(3));
  PRISM_CHECK(g_window->ActivateTab(1));  // the MIDDLE tab is active
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(PathAt(1), std::string("C:/t/two.7z"));

  const POINT close = CloseCentre(1);
  PRISM_CHECK_EQ(g_window->TabBarCloseAt(close.x, close.y), 1);
  Click(close);  // the x of the active tab

  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(PathAt(0), std::string("C:/t/one.7z"));
  PRISM_CHECK_EQ(PathAt(1), std::string("C:/t/three.7z"));
  // C59's rule, now visible: the tab that SLID INTO the closed slot is active.
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(g_window->Tabs().active()->displayName, std::string("three.7z"));
  // Closing a tab is not a switch of an archive that is already shown... unless
  // the active tab changed, which it did: the app was told.
  PRISM_CHECK_EQ(g_requested, std::string("C:/t/three.7z"));

  // Closing the last tab is an EMPTY STATE, not an exit.
  (void)g_window->CloseTab(1, true);
  const prism::tabs::CloseOutcome last = g_window->CloseTab(0, true);
  PRISM_CHECK(last.emptyState);
  PRISM_CHECK(g_window->Tabs().empty());
  PRISM_CHECK(!g_window->TabBarVisible());
  PRISM_CHECK_EQ(g_window->ItemCount(), 0);
  PRISM_CHECK(IsWindow(g_window->Hwnd()) != FALSE);
  PRISM_CHECK(g_window->LayoutForTest().tabBar.Empty());
}

// ---------------------------------------------------------------------------
// 4. A dirty tab's x asks C54's box; "cancel" keeps the tab.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 dirty tab: the close asks first, cancel keeps the tab") {
  ResetTabs();
  g_window->OpenTab("C:/d/one.7z");
  g_window->OpenTab("C:/d/two.7z");
  PRISM_CHECK(g_window->Tabs().SetDirty(1, true));
  PRISM_CHECK_EQ(g_window->Tabs().DirtyCount(), static_cast<std::size_t>(1));

  // The injected confirmer IS the user: "no".
  int asked = 0;
  std::wstring askedName;
  g_window->SetTabCloseConfirmer([&](const std::wstring& name) {
    ++asked;
    askedName = name;
    return false;
  });

  const POINT close = CloseCentre(1);
  Click(close);
  PRISM_CHECK_EQ(asked, 1);
  PRISM_CHECK(askedName == L"two.7z");
  PRISM_CHECK_EQ(g_window->TabClosePrompts(), 1);
  PRISM_CHECK_EQ(g_window->TabCloseRefusals(), 1);
  // CANCEL means nothing happened: the tab is still there, still dirty, still
  // showing the same archive.
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK(g_window->Tabs().At(1)->dirty);

  // The model the product shows is C54's, with the archive's name in it.
  const prism::win::editui::EditDialogModel model = GalleryWindow::TabCloseModel(L"two.7z");
  PRISM_CHECK(!model.title.empty());
  PRISM_CHECK(model.body.find(L"two.7z") != std::wstring::npos);
  PRISM_CHECK(model.dangerous);
  PRISM_CHECK(!model.needsInput);

  // ...and C54's OWN dialog accepts that model and answers "cancel" through the
  // same EditMayProceed() the product branches on.
  prism::win::editui::EditConfirmDialog dialog;
  PRISM_CHECK(dialog.Create(GetModuleHandleW(nullptr), L"PrismArchive"));
  dialog.SetModel(model);
  prism::win::editui::EditDialogResult answer;
  bool answered = false;
  dialog.SetResultHandler([&](const prism::win::editui::EditDialogResult& result) {
    answer = result;
    answered = true;
  });
  dialog.PressCancel();
  PRISM_CHECK(answered);
  PRISM_CHECK(!prism::win::editui::EditMayProceed(answer));
  dialog.Destroy();

  // "Yes" closes it -- and the tab really leaves the strip.
  g_window->SetTabCloseConfirmer([&](const std::wstring&) { return true; });
  const std::optional<prism::tabs::CloseOutcome> closed = g_window->RequestCloseTab(1);
  PRISM_CHECK(closed.has_value());
  PRISM_CHECK(closed->closed);
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(1));
}

// ---------------------------------------------------------------------------
// 5. Ctrl+Tab / Ctrl+Shift+Tab cycle (and wrap).
// ---------------------------------------------------------------------------
PRISM_TEST("c60 ctrl+tab cycles the strip in both directions") {
  ResetTabs();
  g_window->OpenTab("C:/k/a.7z");
  g_window->OpenTab("C:/k/b.7z");
  g_window->OpenTab("C:/k/c.7z");
  g_window->ActivateTab(0);

  prism::keys::Stroke next;
  next.key = 0x09;  // VK_TAB
  next.ctrl = true;
  prism::keys::Stroke previous = next;
  previous.shift = true;

  g_window->DispatchKeyStroke(next);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  g_window->DispatchKeyStroke(next);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 2);
  g_window->DispatchKeyStroke(next);  // wraps to the first tab
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);
  g_window->DispatchKeyStroke(previous);  // wraps backwards to the last tab
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 2);
  PRISM_CHECK_EQ(g_window->Tabs().active()->path, std::string("C:/k/c.7z"));
  // Each hop told the app which archive to show.
  PRISM_CHECK_EQ(g_requested, std::string("C:/k/c.7z"));

  // Ctrl+W closes the CURRENT tab (no dirty flag here: no question asked).
  prism::keys::Stroke closeKey;
  closeKey.key = 'W';
  closeKey.ctrl = true;
  g_window->DispatchKeyStroke(closeKey);
  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);  // the right neighbour
}

// ---------------------------------------------------------------------------
// 6. Drag reorder: the order changes, every path survives.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 dragging a tab reorders the strip without losing a path") {
  ResetTabs();
  g_window->OpenTab("C:/r/first.7z");
  g_window->OpenTab("C:/r/second.7z");
  g_window->OpenTab("C:/r/third.7z");
  g_window->ActivateTab(0);

  const std::vector<std::string> before = {PathAt(0), PathAt(1), PathAt(2)};
  const std::vector<prism::win::tabbar::TabGeometry> rects = g_window->TabBarRects();
  PRISM_CHECK_EQ(rects.size(), static_cast<std::size_t>(3));

  const POINT from = TabCentre(0);
  const POINT to{rects[2].rect.left + rects[2].rect.Width() * 3 / 4,
                 rects[2].rect.top + rects[2].rect.Height() / 2};
  const LPARAM fromPacked =
      static_cast<LPARAM>(MAKELONG(static_cast<short>(from.x), static_cast<short>(from.y)));
  const LPARAM toPacked =
      static_cast<LPARAM>(MAKELONG(static_cast<short>(to.x), static_cast<short>(to.y)));
  g_window->Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, fromPacked);
  g_window->Dispatch(WM_MOUSEMOVE, MK_LBUTTON, toPacked);
  // The insertion marker is armed and lands on the slot after the last tab.
  PRISM_CHECK(g_window->TabDragActive());
  PRISM_CHECK_EQ(g_window->TabDragSlot(), 3);
  PRISM_CHECK_EQ(g_window->TabBarView().dragFrom, 0);
  g_window->Dispatch(WM_LBUTTONUP, 0, toPacked);

  PRISM_CHECK_EQ(g_window->Tabs().size(), static_cast<std::size_t>(3));
  // The order changed...
  PRISM_CHECK_EQ(PathAt(0), before[1]);
  PRISM_CHECK_EQ(PathAt(1), before[2]);
  PRISM_CHECK_EQ(PathAt(2), before[0]);
  // ...and every path is still exactly one tab (nothing renamed, nothing lost).
  int matches = 0;
  for (const std::string& path : before) {
    for (int i = 0; i < 3; ++i) {
      if (PathAt(i) == path) ++matches;
    }
  }
  PRISM_CHECK_EQ(matches, 3);
  // The active tab is the SAME archive: the drag moved where it sits, not what
  // the user is looking at (C59's Move contract).
  PRISM_CHECK_EQ(g_window->Tabs().active()->path, std::string("C:/r/first.7z"));
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 2);
  // A click without movement is NOT a reorder.
  const POINT stay = TabCentre(1);
  const LPARAM stayPacked =
      static_cast<LPARAM>(MAKELONG(static_cast<short>(stay.x), static_cast<short>(stay.y)));
  g_window->Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, stayPacked);
  g_window->Dispatch(WM_MOUSEMOVE, MK_LBUTTON, stayPacked);
  PRISM_CHECK(!g_window->TabDragActive());
  g_window->Dispatch(WM_LBUTTONUP, 0, stayPacked);
  PRISM_CHECK_EQ(PathAt(0), before[1]);
}

// ---------------------------------------------------------------------------
// 7. No archive -> the strip is NOT rendered (pixels) and reserves no space.
// ---------------------------------------------------------------------------
PRISM_TEST("c60 no archive: the strip is not rendered and reserves nothing") {
  ResetTabs();
  PRISM_CHECK(g_window->Tabs().empty());
  PRISM_CHECK(!g_window->TabBarVisible());

  const prism::layout::Layout hidden =
      prism::layout::Compute(900, 600, false, false, /*tabBarVisible=*/false);
  PRISM_CHECK(hidden.tabBar.Empty());
  PRISM_CHECK_EQ(hidden.tabBar.Height(), 0);
  PRISM_CHECK_EQ(hidden.actionIsland.top, prism::layout::kTitleBarHeight);
  PRISM_CHECK_EQ(hidden.actionIsland.Height(), prism::layout::kActionIslandHeight);
  PRISM_CHECK_EQ(hidden.content.top, prism::layout::kTopBandHeight);

  // Frame B: nothing open. Nothing of the strip may reach the band.
  const prism::win::tabbar::View empty;
  Frame bare;
  int painted = -1;
  PRISM_CHECK(RenderTabs(empty, hidden, &bare, &painted));
  PRISM_CHECK_EQ(painted, 0);
  // The band is the ACTION ISLAND's tint here -- one uniform band, and not the
  // strip's canvas plate: a strip that painted itself would show its own
  // SurfaceCanvasBase composite instead (the assertion below compares the band
  // against the two colours, so "painted nothing" is measured, not assumed).
  // x = 850 is past the breadcrumb pill, so both probes are in the band's own
  // fill: one uniform band with no seam where the strip would have been.
  const UINT32 bandPixel = bare.At(850, 48);
  PRISM_CHECK(ColorDistance(bandPixel, bare.At(850, 80)) == 0);
  PRISM_CHECK(ColorDistance(bandPixel, CanvasOverBackdrop()) > 2);
  // No accent (the dirty dot / insertion marker) anywhere in the band.
  int accentPixels = 0;
  for (int y = hidden.titleBar.bottom; y < hidden.actionIsland.bottom; ++y) {
    for (int x = 0; x < hidden.windowW; ++x) {
      if (ColorDistance(bare.At(x, y), AccentPixel()) < 24) ++accentPixels;
    }
  }
  PRISM_CHECK_EQ(accentPixels, 0);

  // Frame A: two tabs, for contrast -- the SAME band shows tab plates there, so
  // the measurement above cannot be green because "the renderer draws nothing".
  g_window->OpenTab("C:/n/one.7z");
  g_window->OpenTab("C:/n/two.7z");
  const prism::layout::Layout shown =
      prism::layout::Compute(900, 600, false, false, /*tabBarVisible=*/true);
  PRISM_CHECK_EQ(shown.tabBar.Height(), prism::layout::kTabBarHeight);
  PRISM_CHECK_EQ(shown.actionIsland.top, prism::layout::kTabBarHeight + prism::layout::kTitleBarHeight);
  const prism::win::tabbar::View view = g_window->TabBarView();
  Frame strip;
  int stripPainted = 0;
  PRISM_CHECK(RenderTabs(view, shown, &strip, &stripPainted));
  PRISM_CHECK_EQ(stripPainted, 2);
  const std::vector<prism::win::tabbar::TabGeometry> rects =
      prism::win::tabbar::TabRects(shown.tabBar, view);
  PRISM_CHECK_EQ(rects.size(), static_cast<std::size_t>(2));
  const prism::layout::Rect active = rects[1].rect;
  PRISM_CHECK(ColorDistance(strip.At(active.left + 5, active.top + active.Height() / 2),
                            IslandOverCanvas()) <= 2);
  // The plate exists at y = 48 in THIS frame and not in the empty one.
  PRISM_CHECK(ColorDistance(strip.At(active.left + 5, 48), IslandOverCanvas()) <= 2);
  PRISM_CHECK(ColorDistance(bare.At(active.left + 5, 48), IslandOverCanvas()) > 8);
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return kSkipReturnCode;
  }
  BuildCatalogs();

  static GalleryWindow window;
  if (!window.Create(L"PrismArchive tab bar", GetModuleHandleW(nullptr))) {
    std::cout << "SKIP: window creation failed (no interactive window station)" << std::endl;
    return kSkipReturnCode;
  }
  g_window = &window;
  // The app's side of the contract (Main.cpp installs the same handler): the
  // strip asks, the app shows. It also records what was asked for, which is
  // what makes case 2 a wiring assertion instead of a model one.
  window.SetTabHandler([](const std::string& path) {
    ++g_requests;
    g_requested = path;
    if (!path.empty()) ApplySwitch(path);
  });

  if (kMutation != nullptr) {
    std::cout << "C60 MUTANT ACTIVE: " << kMutation << std::endl;
    // CONTRACTS D27: arm BEFORE the first assertion; the first failure prints
    // the marker and leaves with 0, and only while the injected defect fired.
    if (std::strcmp(kMutation, "close_first_tab") == 0) {
      prism::test::SetMutationVerdict("C60 MUTATION CAUGHT: close_first_tab",
                                      [] { return CloseFirstTabInjectionFired(); });
    } else {
      prism::test::SetMutationVerdict("C60 MUTATION CAUGHT: dirty_no_confirm",
                                      [] { return DirtyNoConfirmInjectionFired(); });
    }
  }

  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::cout << "C60 MUTATION NOT CAUGHT: the suite stayed green with the regression compiled "
                   "in -- the injection is stale"
                << std::endl;
      return 1;
    }
    std::cout << "C60 MUTATION CAUGHT: " << kMutation << " (suite red, see FAIL lines)"
              << std::endl;
    return 0;
  }
  if (rc != 0) return rc;
  std::cout << "ALL TAB-BAR CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  ::CoUninitialize();
  return 0;
}
