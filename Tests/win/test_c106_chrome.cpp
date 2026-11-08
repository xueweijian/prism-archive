// PrismArchive -- PrismWin
// C106: THE MENU BAR IS THE TITLE BAR, AND IT CAN GET OUT OF THE WAY.
//
// THE DEFECT (plan section 4). The top of the window is a DWM caption over a
// Win32 menu bar -- two systems' pixels. C83 made both read the same token, but
// nothing compared them WITH EACH OTHER, so a bar pinned to white (the plan's
// "突兀白条") left every existing gate green. And the bar always occupied 20 px:
// there was no way to put it away.
//
// WHAT THIS FILE MEASURES, on a real window, from real screen pixels:
//   1. THREE THEMES: menu plate == caption plate within 2 levels per channel
//      (the card's pixel gate). The caption is DWM's, so this is a screen
//      capture, not a client-DC BitBlt.
//   2. Alt: the bar leaves the non-client area (GetMenu() == null -- a Win32
//      menu bar has no HWND, so that IS its visibility) and the content moves up
//      by exactly SM_CYMENU, which is >= 20 px. A second Alt restores both.
//   3. The chords do not depend on the bar: Ctrl+O and Ctrl+E still fire while
//      it is down.
//   4. The menu is still there for assistive technology: the OBJID_MENU object
//      enumerates the four items while hidden, and the WM_COMMAND door its
//      Invoke uses still runs the app's handler.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C106_MUTANT_WHITE_BAR pins the
// menu plate to white in GalleryWindow::RebuildMenu (the product's own brush).
// Case 1 MUST go red in the two dark themes, and the CAUGHT verdict is armed on
// the observable proof that the injection ran: a WHITE plate sampled in a theme
// whose ShellSurface is dark.
//
// SCREENSHOTS: `win_c106_chrome_test.exe <out-dir>` writes
// c106-chrome-{day,night,hc}.png -- the real caption + menu bar + a slice of the
// content, captured from the screen. Without the argument the suite only asserts
// (which is what CI runs).
#include <windows.h>
#include <oleacc.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismCore/ChromeMenuBar.hpp"
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

using Microsoft::WRL::ComPtr;
using prism::chrome::MenuBarState;
using prism::chrome::PaletteFor;
using prism::theme::Variant;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
bool g_noWindowStation = false;

#if defined(PRISM_C106_MUTANT_WHITE_BAR)
constexpr const char* kMutation = "white_bar";
#else
constexpr const char* kMutation = nullptr;
#endif

// The two "the injected defect really fired" predicates (CONTRACTS D3: a red is
// not CAUGHT). Case 1 sets the first when it samples a white plate in a DARK
// theme -- something only the injection can produce.
bool g_whitePlateInDarkTheme = false;

const char* g_outDir = nullptr;

struct Shot {
  std::vector<uint32_t> pixels;  // 0xAARRGGBB, top-down
  int width = 0;
  int height = 0;
  RECT screen{};
};

int ChannelR(uint32_t p) { return static_cast<int>((p >> 16) & 0xFFu); }
int ChannelG(uint32_t p) { return static_cast<int>((p >> 8) & 0xFFu); }
int ChannelB(uint32_t p) { return static_cast<int>(p & 0xFFu); }

bool GrabScreen(const RECT& rect, Shot* out) {
  if (out == nullptr) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) return false;
  HDC memory = ::CreateCompatibleDC(screen);
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;  // top-down
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bitmap = ::CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  bool ok = false;
  if (bitmap != nullptr && bits != nullptr) {
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    ok = ::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top, SRCCOPY) != FALSE;
    ::GdiFlush();
    if (ok) {
      out->width = width;
      out->height = height;
      out->screen = rect;
      const uint32_t* source = static_cast<const uint32_t*>(bits);
      out->pixels.assign(source, source + static_cast<std::size_t>(width) * height);
    }
    ::SelectObject(memory, previous);
  }
  if (bitmap != nullptr) ::DeleteObject(bitmap);
  ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  return ok;
}

// The most frequent colour of a screen-space region: the plate, with the item
// glyphs and the title text outvoted. A mean would be pulled by the glyphs; a
// single pixel would be a coin toss.
uint32_t ModalColor(const Shot& shot, int left, int top, int right, int bottom) {
  std::vector<uint32_t> colors;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const int sx = x - shot.screen.left;
      const int sy = y - shot.screen.top;
      if (sx < 0 || sy < 0 || sx >= shot.width || sy >= shot.height) continue;
      colors.push_back(shot.pixels[static_cast<std::size_t>(sy) * shot.width + sx] & 0x00FFFFFFu);
    }
  }
  if (colors.empty()) return 0xFFFFFFFFu;
  std::sort(colors.begin(), colors.end());
  uint32_t best = colors.front();
  std::size_t bestCount = 0;
  std::size_t runStart = 0;
  for (std::size_t i = 1; i <= colors.size(); ++i) {
    if (i == colors.size() || colors[i] != colors[runStart]) {
      if (i - runStart > bestCount) {
        bestCount = i - runStart;
        best = colors[runStart];
      }
      runStart = i;
    }
  }
  return best;
}

bool MakeWindow(GalleryWindow* window, const wchar_t* title) {
  if (window->Create(title, GetModuleHandleW(nullptr))) return true;
  std::puts("  [skip] no window station");
  g_noWindowStation = true;
  return false;
}

// The Chinese labels the shipped app installs (AppWiring::BuildMenuLabels for
// zh-CN), so the plate is sampled with real glyphs on it.
void InstallChineseLabels(GalleryWindow* window) {
  window->SetMenuLabels(L"\u6587\u4EF6", L"\u538B\u7F29\u4E3A .7z \u5F52\u6863",
                        L"\u9000\u51FA");
  window->SetEditMenuLabels(L"\u7F16\u8F91", L"\u5220\u9664", L"\u91CD\u547D\u540D",
                            L"\u65B0\u5EFA\u6587\u4EF6\u5939");
  window->SetAboutMenuLabels(L"Help", L"\u5173\u4E8E PrismArchive...");
  window->SetSettingsMenuLabel(L"\u8BBE\u7F6E(&S)...");
}

void Present(GalleryWindow* window) {
  ::ShowWindow(window->Hwnd(), SW_SHOWNORMAL);
  ::SetWindowPos(window->Hwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ::SetForegroundWindow(window->Hwnd());
  ::UpdateWindow(window->Hwnd());
  ::Sleep(250);
}

void Dismiss(GalleryWindow* window) {
  ::SetWindowPos(window->Hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  window->Dispatch(WM_DESTROY, 0, 0);
}

// The strip the card asks for: the caption, the menu bar, and 48 px of content
// under them, at full window width.
bool WriteStripPng(const Shot& shot, int height, const char* fileName) {
  if (g_outDir == nullptr) return true;  // CI: assert only, write nothing
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.GetAddressOf())))) {
    return false;
  }
  const int width = shot.width;
  if (height > shot.height) height = shot.height;
  std::vector<uint32_t> strip(static_cast<std::size_t>(width) * height);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      // BitBlt leaves the alpha byte at 0; a PNG written with it would be fully
      // transparent, so every pixel becomes opaque here.
      strip[static_cast<std::size_t>(y) * width + x] =
          shot.pixels[static_cast<std::size_t>(y) * width + x] | 0xFF000000u;
    }
  }
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%hs", g_outDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmapFromMemory(
          static_cast<UINT>(width), static_cast<UINT>(height), GUID_WICPixelFormat32bppBGRA,
          static_cast<UINT>(width * 4), static_cast<UINT>(strip.size() * 4),
          reinterpret_cast<BYTE*>(strip.data()), bitmap.GetAddressOf()))) {
    return false;
  }
  return SUCCEEDED(wic->CreateStream(stream.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                           bag.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(frame->Initialize(bag.Get())) &&
         SUCCEEDED(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height))) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) &&
         SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

// Ctrl for the REAL WM_KEYDOWN path: the window reads GetKeyState, so the
// THREAD's keyboard state is what has to hold the chord (the seam test_c85_keys
// and test_keys use, proven to work on the CI window station).
bool SetCtrl(bool down) {
  BYTE state[256] = {};
  if (!::GetKeyboardState(state)) return false;
  state[VK_CONTROL] = down ? 0x80 : 0x00;
  state[VK_SHIFT] = 0x00;
  state[VK_MENU] = 0x00;
  return ::SetKeyboardState(state) != FALSE;
}

void PressCtrl(GalleryWindow* window, UINT vk) {
  (void)SetCtrl(true);
  window->Dispatch(WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
  (void)SetCtrl(false);
}

// One bare Alt press: Windows delivers the press as WM_SYSKEYDOWN(VK_MENU) and
// the decision on WM_SYSKEYUP. Both are dispatched, in that order.
void PressAlt(GalleryWindow* window) {
  window->Dispatch(WM_SYSKEYDOWN, VK_MENU, 0);
  window->Dispatch(WM_SYSKEYUP, VK_MENU, 0);
}

// The visible frame + the client origin, in screen pixels.
struct Frame {
  RECT visible{};
  POINT clientOrigin{};
  int clientHeight = 0;
  int menuHeight = 0;
  int menuTop = 0;  // the first row of the menu bar (== the last of the caption + 1)
};

Frame MeasureFrame(GalleryWindow* window) {
  Frame frame;
  const prism::win::frame::Measurement measured =
      prism::win::frame::Measure(window->Hwnd());
  frame.visible = RECT{measured.visible.left, measured.visible.top, measured.visible.right,
                       measured.visible.bottom};
  ::ClientToScreen(window->Hwnd(), &frame.clientOrigin);
  RECT client{};
  ::GetClientRect(window->Hwnd(), &client);
  frame.clientHeight = client.bottom - client.top;
  frame.menuHeight = ::GetSystemMetrics(SM_CYMENU);
  frame.menuTop = frame.clientOrigin.y - frame.menuHeight;
  return frame;
}

void BuildCatalog(std::vector<prism::ArchiveItem>* catalog) {
  const char* paths[] = {"docs", "docs/a.txt", "photos", "photos/p1.png", "notes.txt"};
  for (const char* path : paths) {
    prism::ArchiveItem item;
    item.index = static_cast<std::uint32_t>(catalog->size());
    item.path = path;
    item.isDir = std::string(path).find('.') == std::string::npos;
    item.size = 10;
    catalog->push_back(item);
  }
}

const char* ThemeFile(Variant variant) {
  switch (variant) {
    case Variant::Daylight: return "c106-chrome-day.png";
    case Variant::HighContrast: return "c106-chrome-hc.png";
    default: return "c106-chrome-night.png";
  }
}

}  // namespace

int main(int argc, char** argv) {
  // The pixels are sampled in physical screen coordinates; without this the
  // process is DPI-virtualised and every rectangle is scaled under our feet.
  ::SetProcessDPIAware();
  if (argc > 1) g_outDir = argv[1];
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  if (kMutation != nullptr) {
    std::printf("C106 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C106 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
      // D3: CAUGHT requires the injection to have reached the PIXELS. A white
      // plate in a dark theme cannot come from the theme.
      return g_whitePlateInDarkTheme;
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noWindowStation) {
    std::puts("C106 SKIPPED: no window station");
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL C106 CHROME CHECKS PASSED");
  ::CoUninitialize();
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE CARD'S PIXEL GATE: menu plate == caption plate, in all three themes.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 the menu bar is the title bar in day, night and high contrast") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c106 shell")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  Present(&window);

  const Variant variants[3] = {Variant::Daylight, Variant::Midnight, Variant::HighContrast};
  bool sawAnyTheme = false;
  int sameSurface = 0;
  for (const Variant variant : variants) {
    window.SetChromeVariant(variant);
    ::UpdateWindow(window.Hwnd());
    ::Sleep(300);

    const Frame frame = MeasureFrame(&window);
    Shot shot;
    if (!GrabScreen(frame.visible, &shot)) {
      std::puts("  [skip] the screen could not be captured");
      g_noWindowStation = true;
      Dismiss(&window);
      return;
    }
    // The caption: everything under the visible frame's top edge down to the
    // menu bar. The title text and the system buttons are kept out of the sample
    // (220 px on each side) -- the SURFACE is what the card is about.
    const uint32_t titlePlate =
        ModalColor(shot, frame.visible.left + 220, frame.visible.top + 4,
                   frame.visible.right - 220, frame.menuTop - 1);
    // The menu bar: from its first row to the last client-exclusive row, the
    // window's full width minus a margin (the glyphs are at the left).
    const uint32_t menuPlate = ModalColor(shot, frame.visible.left + 8, frame.menuTop + 1,
                                          frame.visible.right - 8, frame.clientOrigin.y - 1);
    const bool same = prism::chrome::SameShellSurface(
        ChannelR(menuPlate), ChannelG(menuPlate), ChannelB(menuPlate), ChannelR(titlePlate),
        ChannelG(titlePlate), ChannelB(titlePlate));
    if (same) ++sameSurface;
    // A white plate in a DARK theme is the injection's signature.
    const prism::chrome::Rgba surface = prism::chrome::ShellSurface(variant);
    if (prism::chrome::IsDarkSurface(surface) && ChannelR(menuPlate) >= 250 &&
        ChannelG(menuPlate) >= 250 && ChannelB(menuPlate) >= 250) {
      g_whitePlateInDarkTheme = true;
    }
    // "Did the capture see THIS theme at all?" -- a locked or absent desktop
    // returns whatever was on screen, and that is a skip, not a verdict.
    const int wantR = prism::chrome::Channel255(surface.r);
    const int wantG = prism::chrome::Channel255(surface.g);
    const int wantB = prism::chrome::Channel255(surface.b);
    const bool themeVisible =
        prism::chrome::SameShellSurface(ChannelR(menuPlate), ChannelG(menuPlate),
                                       ChannelB(menuPlate), wantR, wantG, wantB, 6);
    sawAnyTheme = sawAnyTheme || themeVisible;
    const std::string themeName(prism::theme::VariantName(variant));
    std::printf("  [%s] caption=#%02X%02X%02X menu=#%02X%02X%02X token=#%02X%02X%02X same=%d\n",
                themeName.c_str(), ChannelR(titlePlate), ChannelG(titlePlate),
                ChannelB(titlePlate), ChannelR(menuPlate), ChannelG(menuPlate),
                ChannelB(menuPlate), wantR, wantG, wantB, static_cast<int>(same));
    // THE CARD'S ASSERTION 1 FIRST: the plate is the SAME surface as the caption,
    // within 2 levels per channel. This is the assertion the white_bar injection
    // must turn red, so it is the one that reports the CAUGHT line.
    PRISM_CHECK(same);
    // ... and the capture really saw this theme (a locked desktop is a skip).
    PRISM_CHECK(themeVisible);

    if (g_outDir != nullptr) {
      const int stripHeight = frame.clientOrigin.y - frame.visible.top + 48;
      if (!WriteStripPng(shot, stripHeight, ThemeFile(variant))) {
        std::printf("  [png] could not write %s\n", ThemeFile(variant));
      } else {
        std::printf("  [png] %s %dx%d\n", ThemeFile(variant), shot.width, stripHeight);
      }
    }
  }
  if (!sawAnyTheme) {
    std::puts("  [skip] the capture saw none of the three themes");
    g_noWindowStation = true;
    Dismiss(&window);
    return;
  }
  PRISM_CHECK_EQ(sameSurface, 3);
  Dismiss(&window);
}

// ---------------------------------------------------------------------------
// 2. ALT: the bar leaves the non-client area and the content takes the pixels.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 Alt hides the bar and the content moves up by the menu height") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c106 alt")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  Present(&window);

  const Frame before = MeasureFrame(&window);
  RECT windowRectBefore{};
  ::GetWindowRect(window.Hwnd(), &windowRectBefore);
  // Up: the bar is installed and reserves non-client space.
  PRISM_CHECK(::GetMenu(window.Hwnd()) != nullptr);
  PRISM_CHECK(window.MenuBarVisible());
  PRISM_CHECK(window.ParkedMenu() == nullptr);
  PRISM_CHECK(window.MenuBarHeight() >= prism::chrome::kMinHiddenShift);

  PressAlt(&window);
  ::UpdateWindow(window.Hwnd());
  ::Sleep(150);

  const Frame after = MeasureFrame(&window);
  // A Win32 menu bar has no HWND, so GetMenu() IS its visibility: null means the
  // window is not wearing it any more.
  PRISM_CHECK(::GetMenu(window.Hwnd()) == nullptr);
  PRISM_CHECK(!window.MenuBarVisible());
  // ... and it was PARKED, not destroyed: Alt brings back the same bar.
  PRISM_CHECK(window.ParkedMenu() != nullptr);
  const int shift = before.clientOrigin.y - after.clientOrigin.y;
  std::printf("  [alt] menuHeight=%d clientTop %ld -> %ld (shift=%d) clientHeight %d -> %d\n",
              before.menuHeight, static_cast<long>(before.clientOrigin.y),
              static_cast<long>(after.clientOrigin.y), shift, before.clientHeight,
              after.clientHeight);
  // The card's numbers: exactly the menu height, and at least 20 px.
  PRISM_CHECK_EQ(shift, before.menuHeight);
  PRISM_CHECK(shift >= prism::chrome::kMinHiddenShift);
  PRISM_CHECK(prism::chrome::ShiftIsObservable(shift));
  // The window did NOT move: the content moved, the frame did not.
  RECT windowRectAfter{};
  ::GetWindowRect(window.Hwnd(), &windowRectAfter);
  PRISM_CHECK_EQ(windowRectAfter.top, windowRectBefore.top);
  PRISM_CHECK_EQ(windowRectAfter.left, windowRectBefore.left);
  // The client area grew by exactly the pixels the bar gave back.
  PRISM_CHECK_EQ(after.clientHeight, before.clientHeight + before.menuHeight);

  // A second Alt restores the bar AND the geometry: it is a toggle, not a door.
  PressAlt(&window);
  ::UpdateWindow(window.Hwnd());
  ::Sleep(150);
  const Frame restored = MeasureFrame(&window);
  PRISM_CHECK(::GetMenu(window.Hwnd()) != nullptr);
  PRISM_CHECK(window.MenuBarVisible());
  PRISM_CHECK(window.ParkedMenu() == nullptr);
  PRISM_CHECK_EQ(restored.clientOrigin.y, before.clientOrigin.y);
  PRISM_CHECK_EQ(restored.clientHeight, before.clientHeight);

  Dismiss(&window);
}

// ---------------------------------------------------------------------------
// 3. THE CHORDS do not depend on the bar being on screen.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 Ctrl+O and Ctrl+E still fire while the bar is hidden") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c106 chords")) return;
  std::vector<prism::ArchiveItem> catalog;
  BuildCatalog(&catalog);
  window.SetItems(&catalog);
  window.SetSelection(0);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  Present(&window);

  int opened = 0;
  int activated = 0;
  int asked = 0;  // C125: Ctrl+E raises the extract dialog, it does not write
  window.SetOpenArchiveHandler([&opened]() { ++opened; });
  window.SetActivateHandler([&activated](int) { ++activated; });
  window.SetExtractDialogHandler([&asked]() { ++asked; });

  // The bar goes away FIRST: a chord that only worked with a menu would fail
  // here, and that is the whole assertion.
  window.SetMenuBarVisible(false);
  PRISM_CHECK(::GetMenu(window.Hwnd()) == nullptr);

  PressCtrl(&window, 'O');
  PRISM_CHECK_EQ(opened, 1);
  // C125: Ctrl+E asks (dialog seam), it never reaches the extract door directly.
  PressCtrl(&window, 'E');
  PRISM_CHECK_EQ(asked, 1);
  PRISM_CHECK_EQ(activated, 0);

  // Both chords were claimed by the key table, not by a menu loop: the bar is
  // still down afterwards, and nothing was posted to it.
  PRISM_CHECK(::GetMenu(window.Hwnd()) == nullptr);
  PRISM_CHECK(!window.MenuBarVisible());

  // The same two chords with the bar UP behave identically (a chord is not a
  // menu accelerator -- it is resolved by PrismCore's table either way).
  window.SetMenuBarVisible(true);
  PRISM_CHECK(::GetMenu(window.Hwnd()) != nullptr);
  PressCtrl(&window, 'O');
  PRISM_CHECK_EQ(opened, 2);
  PressCtrl(&window, 'E');
  PRISM_CHECK_EQ(asked, 2);
  PRISM_CHECK_EQ(activated, 0);

  Dismiss(&window);
}

// ---------------------------------------------------------------------------
// 4. ACCESSIBILITY: the menu object still enumerates and still operates.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 the menu is still in the UIA tree while the bar is hidden") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c106 uia")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  window.SetMenuBarVisible(false);  // hidden from the first message on
  PRISM_CHECK(::GetMenu(window.Hwnd()) == nullptr);

  IAccessible* menu = nullptr;
  const HRESULT hr = ::AccessibleObjectFromWindow(window.Hwnd(), OBJID_MENU, IID_IAccessible,
                                                  reinterpret_cast<void**>(&menu));
  PRISM_CHECK(SUCCEEDED(hr) && menu != nullptr);
  if (menu != nullptr) {
    long children = -1;
    PRISM_CHECK(SUCCEEDED(menu->get_accChildCount(&children)));
    std::printf("  [uia] menu children=%ld (bar hidden)\n", children);
    // The five items are still THERE: the bar was parked, not dismantled.
    // (C122 added 视图 between 设置 and 帮助; the count moved 4 -> 5.)
    PRISM_CHECK_EQ(children, 5);
    VARIANT child;
    ::VariantInit(&child);
    child.vt = VT_I4;
    child.lVal = 1;  // CHILDID_SELF is 1, so the first item is 2
    BSTR name = nullptr;
    if (SUCCEEDED(menu->get_accName(child, &name)) && name != nullptr) {
      std::wprintf(L"  [uia] child[1] name=%ls\n", name);
      ::SysFreeString(name);
    } else {
      PRISM_CHECK(false);
    }
    menu->Release();
  }

  // The door the a11y Invoke uses is a WM_COMMAND post; it must still run the
  // app's handler while the bar is down.
  int commands = 0;
  UINT lastCommand = 0;
  window.SetMenuCommandHandler([&commands, &lastCommand](UINT id) {
    ++commands;
    lastCommand = id;
  });
  window.Dispatch(WM_COMMAND, GalleryWindow::kMenuCompress, 0);
  PRISM_CHECK_EQ(commands, 1);
  PRISM_CHECK_EQ(lastCommand, static_cast<UINT>(GalleryWindow::kMenuCompress));

  Dismiss(&window);
}
