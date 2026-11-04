// PrismArchive -- PrismWin
// C64: THE TABS ARE LIVE, THE MENU IS TRANSLATED, THE ISLAND IS THE TOKEN.
//
// C59 built the TabSet and C60 drew the strip; both stopped at "the model says
// beta". This suite closes the user-visible half:
//
//   1. CLICKING A TAB LOADS THAT ARCHIVE. The row path changes (the two
//      fixtures each contribute exactly ONE root row, so ItemCount cannot tell
//      them apart -- the path can), through C34's SwitchArchive, the same
//      order Main.cpp uses (AppWiring::ApplyArchiveSwitch).
//   2. A SWITCH THAT CANNOT OPEN ITS TARGET KEEPS THE ORIGINAL TAB. The
//      archive is deleted after it was opened; clicking its tab must leave the
//      strip on the archive whose rows are still on screen, count the refusal
//      and show a readable reason -- never a blank window.
//   3. THE SETTINGS ITEM IS TRANSLATED. The HMENU read-back is the evidence:
//      the label the window actually installed is the one the translator
//      supplied (the i18n key audit lives in core.i18n_audit).
//   4. THE ACTION ISLAND IS PAINTED FROM ITS TOKEN. A render of the real
//      renderer is sampled at the island and at the canvas:
//        * the island pixel == SurfaceFloatingIsland (.52) over the canvas,
//        * island-vs-canvas luminance is INSIDE the token's band (not the
//          hand-lightened +.72 ribbon the user photographed),
//        * the button label and its Alt mnemonic are >= 4.5:1 against it.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26/D27): two compile-time injections, each
// lowering ONE rule, print "C64 MUTATION CAUGHT: <id>" at the first red
// assertion through mini_test's verdict and exit 0. CTest pins that marker.
//   stale_tab_content  -- the switch succeeds but the rows are not republished
//                         (PRISM_C64_MUTANT_STALE_TAB_CONTENT).
//   keep_failed_tab    -- a failed switch leaves its tab active
//                         (PRISM_C64_MUTANT_FAILED_SWITCH_KEEPS_TAB).
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../win/UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
using prism::ArchiveItem;
using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryRenderer;
using prism::win::gallery::GalleryWindow;

namespace {

#if defined(PRISM_C64_MUTANT_STALE_TAB_CONTENT)
constexpr const char* kMutation = "stale_tab_content";
constexpr const char* kMutationBanner = "C64 MUTANT ACTIVE: stale_tab_content";
#elif defined(PRISM_C64_MUTANT_FAILED_SWITCH_KEEPS_TAB)
constexpr const char* kMutation = "keep_failed_tab";
constexpr const char* kMutationBanner = "C64 MUTANT ACTIVE: keep_failed_tab";
#else
constexpr const char* kMutation = nullptr;
constexpr const char* kMutationBanner = nullptr;
#endif

constexpr int kSkipReturnCode = 77;
constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;

bool g_noWindowStation = false;

// ----------------------------------------------------------------- fixture
struct Fixture {
  prism::testtmp::UniqueDir dir{L"c64", L"fixture"};
  std::wstring dllPath;
  std::wstring exePath;
  std::wstring alpha;
  std::wstring beta;
  prism::win::SevenZipLibrary library;
  std::vector<ArchiveItem> alphaCatalog;
  std::vector<ArchiveItem> betaCatalog;
  bool ready = false;
  std::string why;
};

Fixture& World() {
  static Fixture fixture;
  return fixture;
}

bool WriteText(const fs::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring command = L"\"" + exe + L"\" " + args;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &startup, &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

bool ReadCatalog(prism::win::SevenZipLibrary& library, const std::wstring& path,
                 std::vector<ArchiveItem>* out, std::string* why) {
  prism::win::ArchiveReader probe(library);
  if (!probe.Open(path)) {
    *why = "cannot open " + prism::win::WideToUtf8(path) + ": " + probe.openTrace();
    return false;
  }
  *out = probe.ReadCatalog();
  return !out->empty();
}

bool SetupFixture() {
  Fixture& fx = World();
  if (fx.ready) return true;
  if (!fx.why.empty()) return false;
  if (!fx.dir.ok()) {
    fx.why = "fixture directory: " + fx.dir.error();
    return false;
  }
  wchar_t dll[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, ARRAYSIZE(dll)) == 0) {
    fx.why = "PRISM_7Z_DLL is not set";
    return false;
  }
  fx.dllPath = dll;
  const fs::path exe = fs::path(fx.dllPath).parent_path() / L"7z.exe";
  if (!fs::exists(exe)) {
    fx.why = "7z.exe is missing beside 7z.dll";
    return false;
  }
  fx.exePath = exe.wstring();
  fx.alpha = (fx.dir.path() / L"alpha.7z").wstring();
  fx.beta = (fx.dir.path() / L"beta.7z").wstring();
  if (!WriteText(fx.dir.path() / L"alpha-root.txt", "alpha payload") ||
      !WriteText(fx.dir.path() / L"beta-root.txt", "beta payload")) {
    fx.why = "cannot write the fixture payloads";
    return false;
  }
  const std::wstring cwd = fx.dir.wstr();
  if (!Run7z(fx.exePath, L"a -t7z -ms=off \"alpha.7z\" alpha-root.txt", cwd) ||
      !Run7z(fx.exePath, L"a -t7z -ms=off \"beta.7z\" beta-root.txt", cwd)) {
    fx.why = "7z.exe could not author the two archives";
    return false;
  }
  if (!fx.library.Load(fx.dllPath)) {
    fx.why = "LoadLibrary(7z.dll) failed";
    return false;
  }
  if (!ReadCatalog(fx.library, fx.alpha, &fx.alphaCatalog, &fx.why)) return false;
  if (!ReadCatalog(fx.library, fx.beta, &fx.betaCatalog, &fx.why)) return false;
  if (fx.alphaCatalog.size() != 1 || fx.betaCatalog.size() != 1) {
    fx.why = "each fixture must hold exactly one entry";
    return false;
  }
  fx.ready = true;
  return true;
}

// --------------------------------------------------------------- pixels
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

int Channel(UINT32 pixel, int shift) {
  return static_cast<int>((pixel >> shift) & 0xFFu);
}

double RelativeLuminance(UINT32 pixel) {
  const auto linear = [](int value) {
    const double v = static_cast<double>(value) / 255.0;
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  };
  return 0.2126 * linear(Channel(pixel, 16)) + 0.7152 * linear(Channel(pixel, 8)) +
         0.0722 * linear(Channel(pixel, 0));
}

double ContrastRatio(UINT32 a, UINT32 b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  const double hi = (la > lb) ? la : lb;
  const double lo = (la > lb) ? lb : la;
  return (hi + 0.05) / (lo + 0.05);
}

// The renderer clears to this opaque backdrop; the canvas token is composited
// over it wherever a surface paints (DESIGN.md §4).
constexpr prism::theme::Rgba kBackdropPixel{0.043f, 0.055f, 0.075f, 1.0f};

prism::theme::Rgba Token(prism::theme::TokenId id) {
  return prism::theme::Lookup(id, prism::theme::Variant::Midnight).color;
}

bool RenderActionScene(const prism::actionbar::Model& model, Frame* frame,
                       const prism::layout::Layout** layoutOut) {
  static prism::layout::Layout layout;
  layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                  /*sidebarVisible=*/false, /*listMode=*/false);
  *layoutOut = &layout;
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return false;
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;

  GalleryRenderer::Scene scene;
  scene.layout = &layout;
  scene.actions = &model;
  scene.breadcrumb = L"alpha.7z";
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;
  frame->width = kWidth;
  frame->height = kHeight;
  frame->pixels.resize(static_cast<size_t>(kWidth) * kHeight);
  for (UINT32 y = 0; y < kHeight; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * kWidth,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(kWidth) * 4);
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// A. A tab click really loads the archive; a failed switch keeps the tab.
// ---------------------------------------------------------------------------
PRISM_TEST("c64 click the second tab: the rows become the second archive (by path)") {
  Fixture& fx = World();
  if (!SetupFixture()) {
    std::printf("  [skip] fixture unavailable: %s\n", fx.why.c_str());
    g_noWindowStation = true;  // reuse the labelled-skip flag
    return;
  }

  std::vector<ArchiveItem> catalog = fx.alphaCatalog;
  std::vector<ArchiveItem> items = catalog;
  std::wstring archivePath = fx.alpha;
  app::OperationHandles::Config config;
  config.archivePath = archivePath;
  config.handlerSlots = 2;
  std::string poolError;
  std::shared_ptr<app::OperationHandles> ops =
      app::OperationHandles::Create(fx.library, config, &poolError);
  if (!PRISM_CHECK(ops != nullptr)) {
    std::printf("    pool: %s\n", poolError.c_str());
    return;
  }
  app::ArchiveSession session{&fx.library, &ops, &catalog, &items, &archivePath, 2};

  GalleryWindow window;
  if (!window.Create(L"PrismArchive C64 tabs", GetModuleHandleW(nullptr))) {
    // D1: a machine without a window station is a LABELLED skip (exit 77),
    // never a green run that asserted nothing.
    std::puts("  [skip] no window station");
    g_noWindowStation = true;
    ops->Close();
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"alpha.7z");
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(kWidth, kHeight));
  window.SetTabHandler([&window, &session](const std::string& utf8Path) {
    if (utf8Path.empty()) return;
    const bool fromTab = window.ConsumeTabSwitchIntent(utf8Path);
    const app::ArchiveSwitch switched =
        app::ApplyArchiveSwitch(window, session, prism::win::Utf8ToWide(utf8Path));
    if (!switched.ok) {
      if (fromTab) (void)window.RefuseTabSwitch(utf8Path);
      window.SetStatus(L"open failed: " + prism::win::Utf8ToWide(switched.error));
    }
  });
  window.OpenTab(prism::win::WideToUtf8(fx.alpha), true);
  const prism::tabs::OpenOutcome second =
      window.OpenTab(prism::win::WideToUtf8(fx.beta), true, /*activate=*/false);
  PRISM_CHECK_EQ(second.verdict, prism::tabs::TabVerdict::kOk);
  PRISM_CHECK_EQ(window.Tabs().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(window.Tabs().activeIndex(), 0);  // the first argument stays active
  PRISM_CHECK_EQ(window.Items().front().path, std::string("alpha-root.txt"));

  const std::vector<prism::win::tabbar::TabGeometry> rects = window.TabBarRects();
  if (!PRISM_CHECK(rects.size() == static_cast<std::size_t>(2))) {
    window.Dispatch(WM_DESTROY, 0, 0);
    ops->Close();
    return;
  }
  const prism::layout::Rect r = rects[1].rect;
  const LPARAM at = MAKELONG(static_cast<short>(r.left + r.Width() / 4),
                             static_cast<short>(r.top + r.Height() / 2));
  window.Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, at);

  PRISM_CHECK_EQ(window.Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(window.Items().front().path, std::string("beta-root.txt"));
  PRISM_CHECK_EQ(window.Items().front().path, std::string(fx.betaCatalog.front().path));
  // TabSet stores the NORMALIZED path ('/'), so compare by path identity, not
  // by the spelling the fixture happened to use.
  PRISM_CHECK(prism::tabs::PathKey(prism::win::WideToUtf8(archivePath)) ==
              prism::tabs::PathKey(prism::win::WideToUtf8(fx.beta)));
  PRISM_CHECK(window.BreadcrumbText().find(L"beta.7z") != std::wstring::npos);

  // The archive disappears while its tab is open: clicking its tab must NOT
  // blank the window or leave the strip on a file nobody can read.
  (void)window.ActivateTab(0);
  PRISM_CHECK_EQ(window.Items().front().path, std::string("alpha-root.txt"));
  PRISM_CHECK(::DeleteFileW(fx.beta.c_str()) != FALSE);
  window.Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, at);
  PRISM_CHECK_EQ(window.Tabs().activeIndex(), 0);
  PRISM_CHECK_EQ(window.Items().front().path, std::string("alpha-root.txt"));
  PRISM_CHECK_EQ(window.RefusedOperationCount(), 1);
  PRISM_CHECK(!window.StatusText().empty());
  PRISM_CHECK(window.StatusText().find(L"open failed") != std::wstring::npos);
  PRISM_CHECK(window.ArchiveOpen());

  window.Dispatch(WM_DESTROY, 0, 0);
  ops->Close();
}

// ---------------------------------------------------------------------------
// C. The Settings item the window installs is the translated string.
// ---------------------------------------------------------------------------
PRISM_TEST("c64 the Settings menu item is the translated label") {
  GalleryWindow window;
  if (!window.Create(L"PrismArchive C64 menu", GetModuleHandleW(nullptr))) {
    std::puts("  [skip] no window station");
    g_noWindowStation = true;
    return;
  }
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(kWidth, kHeight));
  const std::wstring translated = L"\u8BBE\u7F6E(&S)...";  // 设置(&S)...
  window.SetSettingsMenuLabel(translated);
  HMENU bar = GetMenu(window.Hwnd());
  if (!PRISM_CHECK(bar != nullptr)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // C83: the bar is owner-drawn, so the OS stores NO string for the item (the
  // item data IS the text). The assertion is unchanged -- the Settings item is
  // still the translated label -- but it reads the window's accessor, which
  // returns exactly the string WM_DRAWITEM paints. The COMMAND must still be on
  // the real HMENU, so the item is genuinely installed, not just remembered.
  PRISM_CHECK(GetMenuState(bar, prism::win::settingsui::SettingsDialog::kMenuSettingsId,
                           MF_BYCOMMAND) != static_cast<UINT>(-1));
  const std::wstring text =
      window.MenuItemText(prism::win::settingsui::SettingsDialog::kMenuSettingsId);
  PRISM_CHECK(!text.empty());
  PRISM_CHECK(text == translated);
  PRISM_CHECK(text != L"Settings...");
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// D. Pixels: island == token composite, contrast >= 4.5:1, not a lit ribbon.
// ---------------------------------------------------------------------------
PRISM_TEST("c64 action island pixels: token fill, bounded canvas contrast, 4.5:1 text") {
  prism::actionbar::Model model;
  model.SetContext(prism::actionbar::Context{true, true, false});
  Frame frame;
  const prism::layout::Layout* windowLayout = nullptr;
  if (!RenderActionScene(model, &frame, &windowLayout)) {
    std::puts("  [skip] renderer unavailable (no WIC/D2D)");
    g_noWindowStation = true;
    return;
  }
  if (!PRISM_CHECK(windowLayout != nullptr)) return;

  const prism::actionbar::Layout band = prism::actionbar::Compute(*windowLayout);
  if (!PRISM_CHECK(!band.island.Empty())) return;

  // The canvas: the content area, far from every island.
  const UINT32 canvasPixel = frame.At(static_cast<int>(kWidth) / 2, static_cast<int>(kHeight) / 2);
  PRISM_CHECK(ColorDistance(canvasPixel, ToPixel(kBackdropPixel)) <= 2);

  // The island's own fill (inside its padding, left of every button).
  const int islandX = band.island.left + 3;
  const int islandY = (band.island.top + band.island.bottom) / 2;
  const UINT32 islandPixel = frame.At(islandX, islandY);
  const prism::theme::Rgba expectedIsland =
      prism::theme::CompositeOver(Token(prism::theme::TokenId::SurfaceFloatingIsland),
                                  kBackdropPixel);
  PRISM_CHECK(ColorDistance(islandPixel, ToPixel(expectedIsland)) <= 2);

  // Luminance (0..255, Rec.709) of the island against the canvas. The token
  // lands around 14.7; the pre-fix hand-lightened ribbon measured ~22.
  const auto luma255 = [](UINT32 pixel) {
    return 0.2126 * Channel(pixel, 16) + 0.7152 * Channel(pixel, 8) + 0.0722 * Channel(pixel, 0);
  };
  const double islandLuma = luma255(islandPixel);
  const double canvasLuma = luma255(canvasPixel);
  const double delta = islandLuma - canvasLuma;
  std::printf("  [island] token=(%.1f,%.1f,%.1f) pixel=(%d,%d,%d) canvas=(%d,%d,%d) dLuma=%.2f\n",
              static_cast<double>(expectedIsland.r) * 255.0,
              static_cast<double>(expectedIsland.g) * 255.0,
              static_cast<double>(expectedIsland.b) * 255.0, Channel(islandPixel, 16),
              Channel(islandPixel, 8), Channel(islandPixel, 0), Channel(canvasPixel, 16),
              Channel(canvasPixel, 8), Channel(canvasPixel, 0), delta);
  PRISM_CHECK(delta >= 8.0);   // the island must be visible at all
  PRISM_CHECK(delta <= 18.0);  // ... and must not be the brighter ribbon

  // Button text: the brightest glyph pixel in the label box, against the island
  // it is painted on. WCAG AA for normal text.
  const prism::actionbar::Button& button =
      band.buttons[static_cast<std::size_t>(prism::actionbar::IndexOf(
          prism::actionbar::ActionId::ExtractTo))];
  const prism::actionbar::Rect labelRect = prism::actionbar::LabelBoxRect(button.rect);
  UINT32 labelInk = islandPixel;
  for (int y = labelRect.top; y < labelRect.bottom; ++y) {
    for (int x = labelRect.left; x < labelRect.right; ++x) {
      if (!frame.In(x, y)) continue;
      if (RelativeLuminance(frame.At(x, y)) > RelativeLuminance(labelInk)) labelInk = frame.At(x, y);
    }
  }
  const double labelContrast = ContrastRatio(labelInk, islandPixel);
  std::printf("  [label] ink=(%d,%d,%d) contrast=%.2f:1\n", Channel(labelInk, 16),
              Channel(labelInk, 8), Channel(labelInk, 0), labelContrast);
  PRISM_CHECK(labelContrast >= 4.5);

  // The Alt mnemonic (the "floating letter" in the screenshot): top-right of
  // the same button. 0.60 alpha used to measure ~3.2:1.
  const prism::actionbar::Rect& r = button.rect;
  UINT32 mnemonicInk = islandPixel;
  for (int y = r.top + 5; y < r.top + 17; ++y) {
    for (int x = r.right - 18; x < r.right - 5; ++x) {
      if (!frame.In(x, y)) continue;
      if (RelativeLuminance(frame.At(x, y)) > RelativeLuminance(mnemonicInk)) {
        mnemonicInk = frame.At(x, y);
      }
    }
  }
  const double mnemonicContrast = ContrastRatio(mnemonicInk, islandPixel);
  std::printf("  [mnemonic] ink=(%d,%d,%d) contrast=%.2f:1\n", Channel(mnemonicInk, 16),
              Channel(mnemonicInk, 8), Channel(mnemonicInk, 0), mnemonicContrast);
  PRISM_CHECK(mnemonicContrast >= 4.5);
}

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::puts(kMutationBanner);
    prism::test::SetMutationVerdict(
        (std::string("C64 MUTATION CAUGHT: ") + kMutation).c_str(),
        [mutation = kMutation] { return mutation != nullptr; });
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("C64 SKIP: CoInitializeEx failed (no COM on this window station)");
    return kSkipReturnCode;
  }
  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::puts("C64 MUTATION NOT CAUGHT: the suite stayed green with the regression compiled in");
      return 1;
    }
    return 0;  // ReportFailure already printed the CAUGHT marker
  }
  if (rc != 0) {
    CoUninitialize();
    return rc;
  }
  if (g_noWindowStation) {
    std::puts("C64 SKIP: this machine has no usable window station / renderer");
    CoUninitialize();
    return kSkipReturnCode;
  }
  std::puts("ALL C64 TAB CHECKS PASSED");
  CoUninitialize();
  return 0;
}
