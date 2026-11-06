// PrismArchive -- PrismWin
// C87: the progress HUD's lifecycle and the Quick Look card's REAL pixels.
//
// THE TWO REPORTED DEFECTS (user-measured, screenshot
// E:\Temp\uicheck\preview-state.png):
//   1. the capsule froze at the last percent and never disappeared, because the
//      operation had no terminal transition at all;
//   2. the Quick Look card painted a blue gradient where the image belongs --
//      the decoded pixels were in the ThumbStore and the card never read them.
//
// WHAT IS ASSERTED (deterministic: injected clock + injected progress, no
// sleeps, no timing):
//   * while an operation runs the MSAA tree carries a ROLE_SYSTEM_PROGRESSBAR
//     element whose name is the human-readable operation and whose value is the
//     percentage (requirement 1);
//   * after success / failure / cancel the capsule is GONE -- two consecutive
//     rendered frames have ZERO HUD pixels in the capsule rectangle, and the
//     MSAA tree has no progress element (requirement 2);
//   * a second operation restarts below 10 %, and a tab switch / archive switch
//     resets the HUD instead of carrying the frozen value (requirement 4);
//   * opening Quick Look on an image paints the DECODED pixels: the preview
//     region's luminance variance is high AND each quadrant of the contain-fit
//     rectangle carries the source thumbnail's colour (requirement 3).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27; same source, compile-time arms):
//   PRISM_C87_MUTANT_HUD_NO_DISMISS        finish does not dismiss -> residual
//                                          pixels + progress element stay
//   PRISM_C87_MUTANT_NO_PROGRESS_A11Y      the capsule is painted but not
//                                          published -> the a11y assertion reds
//   PRISM_C87_MUTANT_PREVIEW_PLACEHOLDER   the card keeps the gradient -> the
//                                          quadrant/variance assertion reds
// Each mutant is registered under PASS_REGULAR_EXPRESSION (never WILL_FAIL);
// the CAUGHT marker prints at the FIRST failed assertion, and only when a
// runtime predicate proves the injected defect really fired.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <oleacc.h>
#include <oleauto.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ThumbStore.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/Accessible.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;

namespace {

// ---------------------------------------------------------------- mutants
#if defined(PRISM_C87_MUTANT_HUD_NO_DISMISS)
constexpr const char* kMutation = "hud_no_dismiss";
constexpr const char* kMutationBanner = "C87 MUTANT ACTIVE: hud_no_dismiss";
#elif defined(PRISM_C87_MUTANT_NO_PROGRESS_A11Y)
constexpr const char* kMutation = "no_progress_a11y";
constexpr const char* kMutationBanner = "C87 MUTANT ACTIVE: no_progress_a11y";
#elif defined(PRISM_C87_MUTANT_PREVIEW_PLACEHOLDER)
constexpr const char* kMutation = "preview_placeholder";
constexpr const char* kMutationBanner = "C87 MUTANT ACTIVE: preview_placeholder";
#else
constexpr const char* kMutation = nullptr;
constexpr const char* kMutationBanner = nullptr;
#endif

constexpr int kSkipReturnCode = 77;

GalleryWindow* g_window = nullptr;
IWICImagingFactory* g_wic = nullptr;
ID2D1Factory* g_d2d = nullptr;
std::vector<prism::ArchiveItem> g_catalog;

// The runtime proofs the mutation verdict reads (CONTRACTS D3).
bool g_hudStuck = false;
bool g_progressMissing = false;
bool g_previewPlaceholder = false;

// ---------------------------------------------------------------- fixtures
void BuildCatalog() {
  prism::ArchiveItem image;
  image.index = 0;
  image.path = "shot.png";
  image.isDir = false;
  image.size = 14256;
  g_catalog.push_back(image);
}

// A 2x2 colour chart: the card must reproduce THIS, not a gradient. 80x40 so
// the contain-fit rectangle is wide, and the quadrant centres are far from the
// interpolated edges.
prism::gallery::DecodedThumb QuadrantThumb() {
  prism::gallery::DecodedThumb thumb;
  thumb.width = 80;
  thumb.height = 40;
  thumb.pixels.resize(static_cast<size_t>(thumb.width) * thumb.height * 4u);
  const uint8_t colours[4][3] = {
      {255, 0, 0},    // top-left: red
      {0, 255, 0},    // top-right: green
      {0, 0, 255},    // bottom-left: blue
      {255, 255, 255} // bottom-right: white
  };
  for (uint32_t y = 0; y < thumb.height; ++y) {
    for (uint32_t x = 0; x < thumb.width; ++x) {
      const int q = (y < thumb.height / 2 ? 0 : 2) + (x < thumb.width / 2 ? 0 : 1);
      uint8_t* p = &thumb.pixels[(static_cast<size_t>(y) * thumb.width + x) * 4u];
      p[0] = colours[q][2];  // B
      p[1] = colours[q][1];  // G
      p[2] = colours[q][0];  // R
      p[3] = 255;            // A
    }
  }
  return thumb;
}

// ---------------------------------------------------------------- frames
struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA

  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

bool RenderWindow(GalleryWindow& window, Frame* out) {
  RECT rc{};
  if (!::GetClientRect(window.Hwnd(), &rc)) return false;
  const UINT32 width = static_cast<UINT32>(rc.right - rc.left);
  const UINT32 height = static_cast<UINT32>(rc.bottom - rc.top);
  if (width == 0 || height == 0) return false;

  ComPtr<IWICBitmap> bitmap;
  if (FAILED(g_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                target.GetAddressOf()))) {
    return false;
  }
  // C113: the CAPTURE seam, not the raw one. The raw seam paints with the
  // renderer's cached device objects, so on a window whose first capture already
  // bound them to a WIC device the SECOND capture is rejected by D2D
  // (D2DERR_WRONG_RESOURCE_DOMAIN) -- this test renders eight frames. The
  // capture seam rebuilds them on the target it is given; the frames this file
  // measures are then the scene instead of a bare canvas.
  if (!window.RenderSceneForCapture(target.Get())) return false;

  WICRect lockRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, lock.GetAddressOf()))) return false;
  UINT stride = 0;
  UINT bytes = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &data))) {
    return false;
  }
  out->width = width;
  out->height = height;
  out->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
  }
  return true;
}

int DifferingPixels(const Frame& a, const Frame& b, const prism::layout::Rect& rect) {
  if (a.width != b.width || a.height != b.height) return -1;
  int differing = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      if (a.At(x, y) != b.At(x, y)) ++differing;
    }
  }
  return differing;
}

double LuminanceVariance(const Frame& frame, const prism::layout::Rect& rect) {
  double sum = 0.0;
  double sumSq = 0.0;
  int count = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      const UINT32 c = frame.At(x, y);
      const double b = static_cast<double>(c & 0xFFu);
      const double g = static_cast<double>((c >> 8) & 0xFFu);
      const double r = static_cast<double>((c >> 16) & 0xFFu);
      const double lum = 0.114 * b + 0.587 * g + 0.299 * r;
      sum += lum;
      sumSq += lum * lum;
      ++count;
    }
  }
  if (count == 0) return 0.0;
  const double mean = sum / count;
  return sumSq / count - mean * mean;
}

int Channel(const Frame& frame, int x, int y, int shift) {
  return static_cast<int>((frame.At(x, y) >> shift) & 0xFFu);
}

// ---------------------------------------------------------------- MSAA glue
VARIANT ChildId(long id) {
  VARIANT v;
  ::VariantInit(&v);
  v.vt = VT_I4;
  v.lVal = id;
  return v;
}

IAccessible* ClientAccessible() {
  if (g_window == nullptr || g_window->Hwnd() == nullptr) return nullptr;
  IAccessible* acc = nullptr;
  if (FAILED(::AccessibleObjectFromWindow(g_window->Hwnd(), OBJID_CLIENT, IID_IAccessible,
                                          reinterpret_cast<void**>(&acc)))) {
    return nullptr;
  }
  return acc;
}

long ChildCount(IAccessible* acc) {
  long count = -1;
  if (acc == nullptr || FAILED(acc->get_accChildCount(&count))) return -1;
  return count;
}

long RoleOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  VARIANT role;
  ::VariantInit(&role);
  if (acc == nullptr || FAILED(acc->get_accRole(child, &role))) return -1;
  return role.lVal;
}

std::wstring NameOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  BSTR text = nullptr;
  if (acc == nullptr || FAILED(acc->get_accName(child, &text)) || text == nullptr) return {};
  std::wstring out(text, ::SysStringLen(text));
  ::SysFreeString(text);
  return out;
}

std::wstring ValueOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  BSTR text = nullptr;
  if (acc == nullptr || FAILED(acc->get_accValue(child, &text)) || text == nullptr) return {};
  std::wstring out(text, ::SysStringLen(text));
  ::SysFreeString(text);
  return out;
}

// The FIRST direct child with the progress-bar role, or 0.
long FindProgress(IAccessible* acc) {
  for (long i = 1; i <= ChildCount(acc); ++i) {
    if (RoleOf(acc, i) == ROLE_SYSTEM_PROGRESSBAR) return i;
  }
  return 0;
}

std::wstring ToWide(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
  return out;
}

// ---------------------------------------------------------------- driving
constexpr const char* kExtractHeadline = "\u89E3\u538B";  // 解压

// The exact posted-message path the app's worker uses (kHudStart carries an
// owned std::string*; kProgressMessage carries done/total).
void PostHudStart(GalleryWindow& window, const char* headline) {
  window.Dispatch(GalleryWindow::kHudStart, 0,
                  reinterpret_cast<LPARAM>(new std::string(headline)));
}

void PostHudProgress(GalleryWindow& window, uint64_t done, uint64_t total) {
  window.Dispatch(GalleryWindow::kProgressMessage, static_cast<WPARAM>(done),
                  static_cast<LPARAM>(total));
}

void SelectFirstRow(GalleryWindow& window) {
  const prism::layout::Rect cell = window.ItemRect(0);
  window.ClickAt((cell.left + cell.right) / 2, (cell.top + cell.bottom) / 2);
}

}  // namespace

// ---------------------------------------------------------------------------
// Requirement 1: the HUD is AUDIBLE while it runs.
// ---------------------------------------------------------------------------
PRISM_TEST("c87.hud_is_published_to_uia_while_running") {
  // Deterministic time: the model is fed an injected clock, so the capsule is
  // already past its 400 ms grace on the very next Render -- no sleeping.
  g_window->Hud().OnStart(kExtractHeadline, 0);
  g_window->Hud().OnProgress(1'000, 40, 100);

  Frame withHud;
  if (!PRISM_CHECK(RenderWindow(*g_window, &withHud))) return;

  IAccessible* acc = ClientAccessible();
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  const long progressId = FindProgress(acc);
  g_progressMissing = progressId <= 0;
  PRISM_CHECK(progressId > 0);
  if (progressId > 0) {
    PRISM_CHECK(NameOf(acc, progressId) == std::wstring(L"\u89E3\u538B"));
    PRISM_CHECK(ValueOf(acc, progressId) == std::wstring(L"40%"));
  }
  acc->Release();
  // Leave the window clean for the lifecycle cases below.
  g_window->Hud().Reset();
}

// ---------------------------------------------------------------------------
// Requirement 2: success / failure / cancel destroy the capsule.
// ---------------------------------------------------------------------------
PRISM_TEST("c87.hud_is_destroyed_on_every_terminal_state") {
  const prism::layout::Rect capsule =
      prism::layout::ProgressCapsuleRect(g_window->LayoutForTest());

  // Baseline WITHOUT a HUD. Rendering it twice measures this host's raster
  // noise floor, so "no HUD pixels" is a relative criterion, not a guess.
  Frame baseline;
  Frame baselineAgain;
  if (!PRISM_CHECK(RenderWindow(*g_window, &baseline))) return;
  if (!PRISM_CHECK(RenderWindow(*g_window, &baselineAgain))) return;
  const int noise = DifferingPixels(baseline, baselineAgain, capsule);
  std::printf("  [c87] capsule rect=%d,%d %dx%d noise floor=%d px\n", capsule.left,
              capsule.top, capsule.Width(), capsule.Height(), noise);

  const prism::HudOutcome outcomes[] = {prism::HudOutcome::Success,
                                        prism::HudOutcome::Failed,
                                        prism::HudOutcome::Cancelled};
  for (const prism::HudOutcome outcome : outcomes) {
    // A live HUD, started through the SAME posted messages the worker uses.
    PostHudStart(*g_window, kExtractHeadline);
    PostHudProgress(*g_window, 40, 100);
    // The posted start stamped NowMs(); inject the sample's clock so the
    // percent is exactly 40 regardless of how fast the machine is.
    g_window->Hud().OnStart(kExtractHeadline, 0);
    g_window->Hud().OnProgress(1'000, 40, 100);

    Frame live;
    if (!PRISM_CHECK(RenderWindow(*g_window, &live))) return;
    const int painted = DifferingPixels(live, baseline, capsule);
    std::printf("  [c87] outcome=%d HUD pixels=%d\n", static_cast<int>(outcome), painted);
    PRISM_CHECK(painted > 200);  // the capsule really is on screen

    // The app's terminal transition (posted message, handled synchronously here
    // through the same WndProc case).
    g_window->Dispatch(GalleryWindow::kHudFinish, static_cast<WPARAM>(outcome), 0);

    // The two consecutive frames the card asks for.
    Frame after1;
    Frame after2;
    if (!PRISM_CHECK(RenderWindow(*g_window, &after1))) return;
    if (!PRISM_CHECK(RenderWindow(*g_window, &after2))) return;
    const int residual1 = DifferingPixels(after1, baseline, capsule);
    const int residual2 = DifferingPixels(after2, baseline, capsule);
    std::printf("  [c87] outcome=%d residual frames=%d,%d px (noise %d)\n",
                static_cast<int>(outcome), residual1, residual2, noise);
    g_hudStuck = g_window->Hud().Render(GalleryWindow::NowMs()).phase !=
                 prism::HudPhase::Hidden;
    PRISM_CHECK(!g_hudStuck);
    PRISM_CHECK(residual1 <= noise);
    PRISM_CHECK(residual2 <= noise);

    IAccessible* acc = ClientAccessible();
    PRISM_CHECK(acc != nullptr);
    if (acc != nullptr) {
      PRISM_CHECK_EQ(FindProgress(acc), 0L);  // no progress element at all
      acc->Release();
    }
  }
}

// ---------------------------------------------------------------------------
// Requirement 4: a new operation starts from 0; tabs/archives reset.
// ---------------------------------------------------------------------------
PRISM_TEST("c87.hud_restarts_from_zero_and_resets_across_tabs") {
  // First operation ends at a high percent...
  g_window->Hud().OnStart(kExtractHeadline, 0);
  g_window->Hud().OnProgress(1'000, 89, 100);
  g_window->Dispatch(GalleryWindow::kHudFinish,
                     static_cast<WPARAM>(prism::HudOutcome::Success), 0);

  // ...the second one starts at 5 %, not at 89 %.
  g_window->Hud().OnStart("\u538B\u7F29", 0);  // 压缩
  g_window->Hud().OnProgress(1'000, 5, 100);
  const prism::ProgressHud::View view = g_window->Hud().Render(GalleryWindow::NowMs());
  std::printf("  [c87] second operation percent=%d\n", view.percent);
  PRISM_CHECK(view.percent < 10);
  PRISM_CHECK_EQ(view.percent, 5);

  IAccessible* acc = ClientAccessible();
  PRISM_CHECK(acc != nullptr);
  if (acc != nullptr) {
    const long id = FindProgress(acc);
    PRISM_CHECK(id > 0);
    if (id > 0) PRISM_CHECK(ValueOf(acc, id) == std::wstring(L"5%"));
    acc->Release();
  }

  // A tab switch is a new session: the capsule must not survive it.
  g_window->OpenTab("C:/fixtures/second.7z", true, false);
  PRISM_CHECK(g_window->ActivateTab(1));
  PRISM_CHECK_EQ(g_window->Hud().Render(GalleryWindow::NowMs()).phase,
                 prism::HudPhase::Hidden);
  acc = ClientAccessible();
  if (acc != nullptr) {
    PRISM_CHECK_EQ(FindProgress(acc), 0L);
    acc->Release();
  }

  // ...and so is re-attaching a catalog (the archive switch path).
  g_window->Hud().OnStart(kExtractHeadline, 0);
  g_window->Hud().OnProgress(1'000, 70, 100);
  g_window->SetItems(&g_catalog);
  PRISM_CHECK_EQ(g_window->Hud().Render(GalleryWindow::NowMs()).phase,
                 prism::HudPhase::Hidden);
  acc = ClientAccessible();
  if (acc != nullptr) {
    PRISM_CHECK_EQ(FindProgress(acc), 0L);
    acc->Release();
  }
}

// ---------------------------------------------------------------------------
// Requirement 3: Quick Look paints the DECODED image, not a stand-in.
// ---------------------------------------------------------------------------
PRISM_TEST("c87.quicklook_preview_paints_decoded_pixels") {
  // The sweep's own hand-off shape: a ThumbStore holding the decoded BGRA.
  auto store = std::make_shared<prism::gallery::ThumbStore>(4ull << 20);
  const prism::gallery::DecodedThumb thumb = QuadrantThumb();
  PRISM_CHECK(store->OnThumb(0, thumb));
  g_window->SetThumbs(store);

  // Select the image row and open Quick Look through the real key path.
  SelectFirstRow(*g_window);
  PRISM_CHECK_EQ(g_window->Selection(), 0);
  g_window->Dispatch(WM_KEYDOWN, VK_SPACE, 0);
  PRISM_CHECK(g_window->QuickLookOpen());

  Frame frame;
  if (!PRISM_CHECK(RenderWindow(*g_window, &frame))) return;

  const prism::layout::Layout& layout = g_window->LayoutForTest();
  const prism::layout::Rect card = prism::layout::QuickLookCardRect(layout);
  const prism::layout::Rect preview = prism::layout::QuickLookPreviewRect(layout);
  std::printf("  [c87] card=%d,%d %dx%d preview=%d,%d %dx%d\n", card.left, card.top,
              card.Width(), card.Height(), preview.left, preview.top, preview.Width(),
              preview.Height());

  // (1) the renderer really took the decoded-pixels path.
  PRISM_CHECK(g_window->QuickLookThumbDraws() > 0);

  // (2) the region is NOT a single colour / a smooth gradient.
  const double variance = LuminanceVariance(frame, preview);
  std::printf("  [c87] preview luminance variance=%.1f\n", variance);

  // (3) and it carries the SOURCE's layout: the 2x2 colour chart, sampled at
  // the centres of the contain-fit rectangle. A gradient stand-in cannot pass.
  const prism::layout::Rect fit = prism::layout::FitContain(
      preview, static_cast<int>(thumb.width), static_cast<int>(thumb.height));
  const int cx = fit.left + fit.Width() / 4;
  const int cy = fit.top + fit.Height() / 4;
  const int tx = fit.left + (fit.Width() * 3) / 4;
  const int ty = fit.top + (fit.Height() * 3) / 4;
  const bool redTL = Channel(frame, cx, cy, 16) > 180 &&
                     Channel(frame, cx, cy, 8) < 100 && Channel(frame, cx, cy, 0) < 100;
  const bool greenTR = Channel(frame, tx, cy, 8) > 180 &&
                       Channel(frame, tx, cy, 16) < 100 && Channel(frame, tx, cy, 0) < 100;
  const bool blueBL = Channel(frame, cx, ty, 0) > 180 &&
                      Channel(frame, cx, ty, 16) < 100 && Channel(frame, cx, ty, 8) < 100;
  const bool whiteBR = Channel(frame, tx, ty, 0) > 180 &&
                       Channel(frame, tx, ty, 8) > 180 && Channel(frame, tx, ty, 16) > 180;
  std::printf("  [c87] quadrants TL(red)=%d TR(green)=%d BL(blue)=%d BR(white)=%d\n",
              static_cast<int>(redTL), static_cast<int>(greenTR), static_cast<int>(blueBL),
              static_cast<int>(whiteBR));

  g_previewPlaceholder = !(variance > 1500.0 && redTL && greenTR && blueBL && whiteBR);
  PRISM_CHECK(variance > 1500.0);
  PRISM_CHECK(redTL);
  PRISM_CHECK(greenTR);
  PRISM_CHECK(blueBL);
  PRISM_CHECK(whiteBR);

  g_window->Dispatch(WM_KEYDOWN, VK_ESCAPE, 0);
  PRISM_CHECK(!g_window->QuickLookOpen());
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return kSkipReturnCode;
  }
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory,
                                reinterpret_cast<void**>(&g_wic)))) {
    std::cout << "SKIP: WIC factory unavailable" << std::endl;
    return kSkipReturnCode;
  }
  if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
    std::cout << "SKIP: D2D factory unavailable" << std::endl;
    return kSkipReturnCode;
  }

  BuildCatalog();
  static GalleryWindow window;
  if (!window.Create(L"PrismArchive C87 HUD lifecycle", ::GetModuleHandleW(nullptr))) {
    std::cout << "SKIP: window creation failed (no interactive window station)" << std::endl;
    return kSkipReturnCode;
  }
  g_window = &window;
  window.SetRootLabel(L"img.7z");
  window.SetItems(&g_catalog);
  window.SetStatus(L"\u5171 1 \u9879");  // 共 1 项
  (void)window.OpenTab("C:/fixtures/img.7z", true);
  window.Show();

  if (kMutation != nullptr) {
    std::cout << kMutationBanner << std::endl;
    // CONTRACTS D27: armed BEFORE the first assertion, and only a predicate
    // that proves the injected defect fired may print CAUGHT.
    if (std::strcmp(kMutation, "hud_no_dismiss") == 0) {
      prism::test::SetMutationVerdict("C87 MUTATION CAUGHT: hud_no_dismiss", [] {
        return g_hudStuck &&
               g_window->Hud().Render(GalleryWindow::NowMs()).phase !=
                   prism::HudPhase::Hidden;
      });
    } else if (std::strcmp(kMutation, "no_progress_a11y") == 0) {
      prism::test::SetMutationVerdict("C87 MUTATION CAUGHT: no_progress_a11y", [] {
        return g_progressMissing;
      });
    } else {
      prism::test::SetMutationVerdict("C87 MUTATION CAUGHT: preview_placeholder", [] {
        return g_previewPlaceholder && g_window->QuickLookThumbDraws() == 0;
      });
    }
  }

  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::cout << "C87 MUTATION NOT CAUGHT: the suite stayed green with the injection "
                   "compiled in -- the negative control is stale"
                << std::endl;
    }
    return 1;
  }
  if (rc != 0) {
    std::cout << "C87 HUD LIFECYCLE CHECKS FAILED" << std::endl;
    return rc;
  }
  std::cout << "ALL C87 HUD LIFECYCLE CHECKS PASSED (" << prism::test::CheckCount()
            << " checks)" << std::endl;
  ::CoUninitialize();
  return 0;
}
