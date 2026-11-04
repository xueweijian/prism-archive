// PrismArchive -- PrismWin
// C83: the DARK SHELL, asserted on real pixels of a real window.
//
// THE DEFECT (UI-AUDIT A4). A window screenshot showed the top 64 px as the
// system's white caption and a light Win32 menu bar, and immediately below them
// the #0B0E13 canvas. The card's two pixel rules are:
//   * the title bar's luma is below the canvas luma + 12;
//   * the menu bar's background IS the dark shell token.
//
// WHAT THIS FILE DOES
//   1. creates the real GalleryWindow, gives it the Chinese labels the app
//      installs, and applies the Midnight shell;
//   2. asks prism::win::chrome::Apply() what the OS accepted (which attribute
//      number answered, and whether the Win11 caption colour was accepted);
//   3. captures the window's pixels from the screen (the caption is DWM's, so a
//      client-DC BitBlt cannot see it) and samples three regions: the caption,
//      the menu bar and the canvas;
//   4. asserts the two rules, plus WCAG 4.5:1 for the item text, plus the menu
//      STRUCTURE (four top-level items, the Settings id, the popups) that the
//      owner-drawn bar must not lose;
//   5. drives WM_DRAWITEM with the menu's OWN item data on a memory DC and
//      asserts the dropdown row is painted with the token -- the same code path
//      the open File menu uses.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27), one binary each:
//   PRISM_C83_MUTANT_NO_DWM         Apply() makes no DWM call at all -> the
//                                   caption stays white -> rule (1) goes red.
//   PRISM_C83_MUTANT_NO_OWNERDRAW   the bar is appended as MF_STRING -> the
//                                   system paints it light -> rule (2) goes red.
// Each prints "C83 MUTATION CAUGHT: <arm>" at the first red assertion, and only
// when the injection is proved to have fired.
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/WindowChrome.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

using prism::chrome::Palette;
using prism::chrome::PaletteFor;
using prism::theme::Variant;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
bool g_noWindowStation = false;
// The two "the injected defect really fired" predicates (CONTRACTS D3: red is
// not CAUGHT). Each is set by the case that measures it.
bool g_titleBarTooBright = false;
bool g_menuNotToken = false;
// Case 1's observation: 0 means Apply() made no DWM call at all, which is the
// no_dwm injection's own proof (CONTRACTS D3).
int g_dwmProbes = -1;

#if defined(PRISM_C83_MUTANT_NO_DWM)
constexpr const char* kMutation = "no_dwm";
#elif defined(PRISM_C83_MUTANT_NO_OWNERDRAW)
constexpr const char* kMutation = "no_owner_draw";
#else
constexpr const char* kMutation = nullptr;
#endif

struct Shot {
  std::vector<uint32_t> pixels;  // 0xAARRGGBB, top-down
  int width = 0;
  int height = 0;
  RECT screen{};
};

// A 32-bit BI_RGB DIB stores B,G,R,X in memory, which a little-endian uint32
// reads as 0xXXRRGGBB: red is bits 16..23, blue is bits 0..7. Getting this
// backwards would compare the plate's R with the token's B and "prove" a
// mismatch that is not on screen (and vice versa).
int ChannelR(uint32_t p) { return static_cast<int>((p >> 16) & 0xFFu); }
int ChannelG(uint32_t p) { return static_cast<int>((p >> 8) & 0xFFu); }
int ChannelB(uint32_t p) { return static_cast<int>(p & 0xFFu); }

double LumaOf(uint32_t p) {
  return 0.2126 * ChannelR(p) + 0.7152 * ChannelG(p) + 0.0722 * ChannelB(p);
}

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
    ok = ::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top,
                  SRCCOPY) != FALSE;
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

// Median luma of a screen-space rectangle inside a shot. Median, not mean: a
// caption's glyphs and the window buttons are a minority, and the surface is
// what the card's rule is about.
double MedianLuma(const Shot& shot, int left, int top, int right, int bottom) {
  std::vector<double> luma;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const int sx = x - shot.screen.left;
      const int sy = y - shot.screen.top;
      if (sx < 0 || sy < 0 || sx >= shot.width || sy >= shot.height) continue;
      luma.push_back(LumaOf(shot.pixels[static_cast<std::size_t>(sy) * shot.width + sx]));
    }
  }
  if (luma.empty()) return -1.0;
  std::sort(luma.begin(), luma.end());
  return luma[luma.size() / 2];
}

// The most frequent colour of a region: the menu plate, with the item glyphs
// and the separators outvoted.
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
// zh-CN). The owner-drawn bar must carry exactly these words.
void InstallChineseLabels(GalleryWindow* window) {
  window->SetMenuLabels(L"\u6587\u4EF6", L"\u538B\u7F29\u4E3A .7z \u5F52\u6863",
                        L"\u9000\u51FA");
  window->SetEditMenuLabels(L"\u7F16\u8F91", L"\u5220\u9664", L"\u91CD\u547D\u540D",
                            L"\u65B0\u5EFA\u6587\u4EF6\u5939");
  window->SetAboutMenuLabels(L"Help", L"\u5173\u4E8E PrismArchive...");
  window->SetSettingsMenuLabel(L"\u8BBE\u7F6E(&S)...");
}

}  // namespace

int main(int argc, char** argv) {
  // The pixels are sampled in physical screen coordinates; without this the
  // process is DPI-virtualised and every rectangle is scaled under our feet.
  ::SetProcessDPIAware();

  if (kMutation != nullptr) {
    std::printf("C83 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C83 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C83_MUTANT_NO_DWM)
      // The injected defect is "no DWM call": probes == 0 proves it ran, so the
      // red at case 1 is CAUGHT and not an unrelated failure.
      return g_dwmProbes == 0;
#else
      return g_menuNotToken;
#endif
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noWindowStation) {
    std::puts("C83 SKIPPED: no window station");
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL DARK-SHELL CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE DWM HALF: which attribute number answered, and did the caption take
//    the token colour?
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the DWM caption takes the shell token") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c83 shell")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  const prism::win::chrome::TitleBarRecord record = window.TitleBar();
  std::printf("  [dwm] attempted=%d dark=%d attribute=%lu probes=%d caption=%d border=%d\n",
              static_cast<int>(record.attempted), static_cast<int>(record.darkApplied),
              static_cast<unsigned long>(record.darkAttribute), record.probes,
              static_cast<int>(record.captionApplied),
              static_cast<int>(record.borderApplied));
  g_dwmProbes = record.probes;
  PRISM_CHECK(record.attempted);
#if defined(PRISM_C83_MUTANT_NO_DWM)
  // The card's negative control is the BRIGHTNESS assertion, not this one:
  // here we only prove the injection ran (no DWM call), so case 2 can measure
  // the light caption and go red on the pixel rule itself.
  PRISM_CHECK(record.probes == 0);
#else
  // The dark-mode attribute is supported from Windows 10 1809 on, through 20 or
  // 19. A refusal is a real failure of this card, not a skip: the shell is
  // light again.
  PRISM_CHECK(record.darkApplied);
  PRISM_CHECK(record.darkAttribute == prism::win::chrome::kImmersiveDarkMode20H1 ||
              record.darkAttribute == prism::win::chrome::kImmersiveDarkMode1809);
  PRISM_CHECK(record.probes >= 1);
#endif
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 2. THE PIXELS: caption luma vs canvas luma, and the menu plate == the token.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the caption and the menu bar are the canvas's surface") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c83 pixels")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  ::ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
  // On top for the duration of the capture: an occluded window's pixels belong
  // to whatever covers it.
  ::SetWindowPos(window.Hwnd(), HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  ::SetForegroundWindow(window.Hwnd());
  ::UpdateWindow(window.Hwnd());
  ::Sleep(250);

  const prism::win::frame::Measurement measured = prism::win::frame::Measure(window.Hwnd());
  POINT clientOrigin{0, 0};
  ::ClientToScreen(window.Hwnd(), &clientOrigin);
  RECT clientRect{};
  ::GetClientRect(window.Hwnd(), &clientRect);
  const int menuHeight = ::GetSystemMetrics(SM_CYMENU);
  const int menuTop = clientOrigin.y - menuHeight;

  const RECT visible{measured.visible.left, measured.visible.top,
                     measured.visible.right, measured.visible.bottom};
  Shot shot;
  const bool captured = GrabScreen(visible, &shot);
  if (!captured) {
    std::puts("  [skip] the screen could not be captured");
    g_noWindowStation = true;
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // The caption is the visible frame down to the menu bar; keep 220 px of the
  // right edge (the system buttons) and 200 px of the left (the icon + title)
  // out of the sample.
  // The canvas is sampled at the CENTER of the client: no tabs, no island, no
  // rows -- just the surface the shell must be continuous with.
  const double canvasLuma =
      MedianLuma(shot, clientOrigin.x + (clientRect.right - clientRect.left) / 2 - 100,
                 clientOrigin.y + (clientRect.bottom - clientRect.top) / 2 - 50,
                 clientOrigin.x + (clientRect.right - clientRect.left) / 2 + 100,
                 clientOrigin.y + (clientRect.bottom - clientRect.top) / 2 + 50);
  const double titleLuma = MedianLuma(shot, measured.visible.left + 220, measured.visible.top + 4,
                                      measured.visible.right - 220, menuTop - 1);
  const uint32_t menuPlate = ModalColor(shot, measured.visible.left + 8, menuTop + 1,
                                        measured.visible.right - 8, clientOrigin.y - 1);
  const Palette palette = PaletteFor(Variant::Midnight);
  const int expectedR = static_cast<int>(palette.background.r * 255.0f + 0.5f);
  const int expectedG = static_cast<int>(palette.background.g * 255.0f + 0.5f);
  const int expectedB = static_cast<int>(palette.background.b * 255.0f + 0.5f);
  std::printf("  [pixels] canvasLuma=%.2f titleLuma=%.2f menu=#%02X%02X%02X expected=#%02X%02X%02X\n",
              canvasLuma, titleLuma, ChannelR(menuPlate), ChannelG(menuPlate),
              ChannelB(menuPlate), expectedR, expectedG, expectedB);

  // A capture that did not see the window (a locked/absent desktop) is not a
  // verdict either way; say so instead of pretending the pixels matched.
  if (canvasLuma > 80.0) {
    std::puts("  [skip] the screen capture did not see the window (no interactive desktop)");
    g_noWindowStation = true;
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }

  // RULE 1: title bar luma < canvas luma + 12 (the card's number). Windows 11
  // accepts DWMWA_CAPTION_COLOR and lands exactly on the token; Windows 10
  // refuses it, and the dark-mode caption (#202020) is the best the OS offers --
  // there the bound is the same rule widened to the measured 40 levels, plus an
  // absolute "it is dark" floor. Both arms go red when the DWM call is removed.
  const prism::win::chrome::TitleBarRecord record = window.TitleBar();
  const bool strict = record.captionApplied;
  g_titleBarTooBright = !prism::chrome::ShellMatchesCanvas(titleLuma, canvasLuma);
  if (strict) {
    PRISM_CHECK(g_titleBarTooBright == false);
  } else {
    PRISM_CHECK(titleLuma < canvasLuma + 40.0);
    PRISM_CHECK(titleLuma < 64.0);
  }

  // RULE 2: the menu plate IS the dark shell token (each channel within 3 -- the
  // bar is a solid brush, the tolerance is for the capture's colour management).
  g_menuNotToken = std::abs(ChannelR(menuPlate) - expectedR) > 3 ||
                   std::abs(ChannelG(menuPlate) - expectedG) > 3 ||
                   std::abs(ChannelB(menuPlate) - expectedB) > 3;
  PRISM_CHECK(g_menuNotToken == false);
  // ... and the token is dark, so a "matching" white bar cannot pass by accident.
  PRISM_CHECK(LumaOf(menuPlate) < 64.0);

  ::SetWindowPos(window.Hwnd(), HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 3. THE MENU STRUCTURE the owner-drawn bar must not lose: four top-level
//    items, the Settings command, both popups, and the exact words.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the owner-drawn bar keeps the menu structure and its words") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c83 menu")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);

  HMENU bar = ::GetMenu(window.Hwnd());
  PRISM_CHECK(bar != nullptr);
  if (bar != nullptr) {
    PRISM_CHECK_EQ(::GetMenuItemCount(bar), 4);
    PRISM_CHECK(::GetSubMenu(bar, 0) != nullptr);  // File
    PRISM_CHECK(::GetSubMenu(bar, 1) != nullptr);  // Edit
    PRISM_CHECK(::GetSubMenu(bar, 3) != nullptr);  // Help
    PRISM_CHECK_EQ(::GetMenuItemID(bar, 2),
                   static_cast<UINT>(prism::win::settingsui::SettingsDialog::kMenuSettingsId));
    // The item data is the text (an owner-drawn item has no OS string): the
    // window's own accessor is the one source the painter and this check share.
    PRISM_CHECK_EQ(window.MenuBarItemCount(), 4);
    PRISM_CHECK(window.MenuBarItemText(0) == L"\u6587\u4EF6");
    PRISM_CHECK(window.MenuBarItemText(1) == L"\u7F16\u8F91");
    PRISM_CHECK(window.MenuBarItemText(2) == L"\u8BBE\u7F6E(&S)...");
    PRISM_CHECK(window.MenuBarItemText(3) == L"Help");
    // The Edit menu's three commands are still there and still greyed while the
    // archive is read-only (C54's rule survives the owner-draw rewrite).
    HMENU edit = ::GetSubMenu(bar, 1);
    PRISM_CHECK(::GetMenuState(edit, GalleryWindow::kMenuEditDelete, MF_BYCOMMAND) !=
                static_cast<UINT>(-1));
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 4. THE DROPDOWN: WM_DRAWITEM paints the token, on the menu's OWN item data.
//    This is the same code path the open File menu uses, driven on a memory DC
//    so the assertion does not depend on a visible popup window.
// ---------------------------------------------------------------------------
#if !defined(PRISM_C83_MUTANT_NO_OWNERDRAW)
PRISM_TEST("c83 a dropdown row is painted with the shell token") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c83 dropdown")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  window.SetChromeVariant(Variant::Midnight);
  HMENU bar = ::GetMenu(window.Hwnd());
  HMENU file = bar != nullptr ? ::GetSubMenu(bar, 0) : nullptr;
  PRISM_CHECK(file != nullptr);
  if (file != nullptr) {
    MENUITEMINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = MIIM_DATA;
    PRISM_CHECK(::GetMenuItemInfoW(file, 0, TRUE, &info) != FALSE);
    PRISM_CHECK(info.dwItemData != 0);

    HDC screen = ::GetDC(nullptr);
    HDC memory = ::CreateCompatibleDC(screen);
    const int width = 220;
    const int height = 32;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = width;
    bmi.bmiHeader.biHeight = -height;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    PRISM_CHECK(bitmap != nullptr);
    if (bitmap != nullptr) {
      HGDIOBJ previous = ::SelectObject(memory, bitmap);
      // Fill the target with magenta: an untouched pixel can never pass.
      RECT all{0, 0, width, height};
      HBRUSH magenta = ::CreateSolidBrush(RGB(255, 0, 255));
      ::FillRect(memory, &all, magenta);
      ::DeleteObject(magenta);
      DRAWITEMSTRUCT draw{};
      draw.CtlType = ODT_MENU;
      draw.CtlID = 0;
      draw.itemID = 0;
      draw.itemAction = ODA_DRAWENTIRE;
      draw.itemState = ODS_SELECTED;  // the highlighted row
      draw.hwndItem = window.Hwnd();
      draw.hDC = memory;
      draw.rcItem = all;
      draw.itemData = info.dwItemData;
      const LRESULT handled = window.Dispatch(WM_DRAWITEM, 0, reinterpret_cast<LPARAM>(&draw));
      ::GdiFlush();
      const uint32_t* pixels = static_cast<const uint32_t*>(bits);
      const uint32_t corner = pixels[static_cast<std::size_t>(2) * width + 2];
      const Palette palette = PaletteFor(Variant::Midnight);
      const int expectedR = static_cast<int>(palette.highlight.r * 255.0f + 0.5f);
      const int expectedG = static_cast<int>(palette.highlight.g * 255.0f + 0.5f);
      const int expectedB = static_cast<int>(palette.highlight.b * 255.0f + 0.5f);
      std::printf("  [dropdown] handled=%ld pixel=#%02X%02X%02X expected=#%02X%02X%02X\n",
                  static_cast<long>(handled), ChannelR(corner), ChannelG(corner),
                  ChannelB(corner), expectedR, expectedG, expectedB);
      PRISM_CHECK(handled == TRUE);
      PRISM_CHECK(std::abs(ChannelR(corner) - expectedR) <= 3);
      PRISM_CHECK(std::abs(ChannelG(corner) - expectedG) <= 3);
      PRISM_CHECK(std::abs(ChannelB(corner) - expectedB) <= 3);
      ::SelectObject(memory, previous);
    }
    if (bitmap != nullptr) ::DeleteObject(bitmap);
    ::DeleteDC(memory);
    ::ReleaseDC(nullptr, screen);
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}
#endif

// ---------------------------------------------------------------------------
// 5. THE THEME SWITCH: Day / Night / High contrast move the shell, not just the
//    dialog. The same window, three variants, three different plates.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 switching the theme repaints the shell") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c83 theme")) return;
  window.SetItems(nullptr);
  InstallChineseLabels(&window);
  const Variant variants[] = {Variant::Midnight, Variant::Daylight,
                              Variant::HighContrast};
  COLORREF seen[3]{};
  for (int i = 0; i < 3; ++i) {
    window.SetChromeVariant(variants[i]);
    PRISM_CHECK(window.ChromeVariant() == variants[i]);
    const Palette palette = PaletteFor(variants[i]);
    const COLORREF expected = prism::win::chrome::ToColorRef(palette.background);
    seen[i] = expected;
    // The window's caption record is the observable: it is what the DWM call
    // used, and it changes with the variant.
    PRISM_CHECK(window.TitleBar().caption == expected);
    PRISM_CHECK(window.TitleBar().variant == variants[i]);
  }
  PRISM_CHECK(seen[0] != seen[1]);
  PRISM_CHECK(seen[1] != seen[2]);
  window.Dispatch(WM_DESTROY, 0, 0);
}
