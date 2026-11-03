// PrismArchive -- C69: the Settings dialog must actually DO something.
//
// MEASURED BEFORE (the defect this card closes). The V3 shell (C31) compiled the
// Day / Night / High contrast combo and the language combo, and neither changed
// a pixel or a word:
//   * SettingsDialog::BuildModel() hardcoded prism::theme::Variant::Midnight, so
//     every theme painted the same frame (identical RGB at the same coordinate);
//   * the dialog's rows were literal L"Language" ... with ids of 0, and the app
//     never rebuilt its menu bar after saving a new language.tag -- the bar kept
//     the words it started with until the next launch.
//
// WHAT THIS SUITE PROVES (pixels and HWND strings, never comments):
//   1. theme: a REAL dialog HWND is shown, painted, and the RGB at one fixed
//      coordinate is sampled after each of the three combo positions. The sample
//      is the primary button (its fill IS the theme's accent token), so the
//      expected colour is derived from ThemeTokens, not typed in twice;
//   2. language: a REAL GalleryWindow menu bar plus a REAL dialog -- picking
//      zh-CN in the dialog's combo must change GetMenuStringW on the window and
//      the row labels the user reads;
//   3. the checklist: what each named setting observably does (and, for the four
//      with no runtime consumer, the boundary recorded in docs/USAGE.md).
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). Two mutant binaries are compiled
// from THIS source:
//   PRISM_C69_MUTANT_NO_THEME_INVALIDATE  the theme branch stops invalidating =>
//        the sampled RGB after the switch still equals the previous theme's.
//   PRISM_C69_MUTANT_NO_MENU_REBUILD      ApplyMenuLabels() becomes a no-op =>
//        the bar still says "Compress to a .7z archive" after picking zh-CN.
// Each prints "C69 MUTATION CAUGHT: <arm>" at the FIRST red assertion through
// mini_test's verdict, and only when the injection is proved active.
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/DialogChrome.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SettingsDialog.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace su = prism::win::settingsui;
namespace chrome = prism::win::chrome;
namespace th = prism::theme;

namespace {

#ifndef PRISM_C69_LANG_DIR
#define PRISM_C69_LANG_DIR "Assets\\Lang"
#endif

bool g_skip = false;

bool WindowStationUsable() {
  static const bool usable = [] {
    const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (probe == nullptr) return false;
    DestroyWindow(probe);
    return true;
  }();
  return usable;
}

bool RequireStation() {
  if (!WindowStationUsable()) {
    g_skip = true;
    std::printf("    [skip] no interactive window station\n");
    return false;
  }
  return true;
}

std::wstring TempRoot() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, buffer);
  std::wstring root(length > 0 ? std::wstring(buffer, length) : std::wstring(L".\\"));
  if (!root.empty() && root.back() == L'\\') root.pop_back();
  return root + L"\\prism-c69-settings-" + std::to_wstring(GetCurrentProcessId());
}

prism::win::settingsfile::Paths FreshPaths(const wchar_t* name) {
  const std::wstring root = TempRoot() + L"\\" + name;
  prism::win::settingsfile::EnsureDirectory(root);
  return prism::win::settingsfile::PathsUnder(root);
}

// One translator per language, loaded from the SAME tables the app ships.
prism::i18n::Translator& TranslatorFor(const wchar_t* tag) {
  static prism::i18n::Translator en;
  static prism::i18n::Translator zh;
  static bool loaded = false;
  if (!loaded) {
    std::string why;
    const std::string dir(PRISM_C69_LANG_DIR);
    en.LoadCatalogFile(dir + "\\en-US.json", &why);
    zh.LoadCatalogFile(dir + "\\en-US.json", &why);  // fallback chain, like the app
    zh.LoadCatalogFile(dir + "\\zh-CN.json", &why);
    en.SetLanguage("en-US");
    zh.SetLanguage("zh-CN");
    loaded = true;
  }
  return tag == std::wstring(L"zh-CN") ? zh : en;
}

// A real user click on the theme combo: set the selection, then dispatch the
// notification the control itself sends. The pixels are what we judge.
void SelectCombo(su::SettingsDialog& dialog, int id, int index) {
  const HWND combo = dialog.Control(id);
  SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
}

// WHAT THE SCREEN SAYS, read the way a screenshot reads it: the window's own DC
// after the window has painted itself. UpdateWindow() only sends WM_PAINT when
// the window asked for one, so a theme change that forgot Invalidate() keeps the
// previous frame on screen and this returns the OLD colour.
COLORREF CapturePixel(HWND hwnd, int x, int y) {
  HDC win = GetDC(hwnd);
  if (win == nullptr) return CLR_INVALID;
  HDC mem = CreateCompatibleDC(win);
  if (mem == nullptr) {
    ReleaseDC(hwnd, win);
    return CLR_INVALID;
  }
  RECT rc{};
  GetClientRect(hwnd, &rc);
  HBITMAP bmp = CreateCompatibleBitmap(win, rc.right - rc.left, rc.bottom - rc.top);
  HGDIOBJ old = SelectObject(mem, bmp);
  const BOOL blitted =
      BitBlt(mem, 0, 0, rc.right - rc.left, rc.bottom - rc.top, win, 0, 0, SRCCOPY);
  const COLORREF color = blitted != FALSE ? GetPixel(mem, x, y) : CLR_INVALID;
  SelectObject(mem, old);
  DeleteObject(bmp);
  DeleteDC(mem);
  ReleaseDC(hwnd, win);
  return color;
}

int ChannelDelta(COLORREF a, COLORREF b) {
  const int dr = std::abs(static_cast<int>(GetRValue(a)) - static_cast<int>(GetRValue(b)));
  const int dg = std::abs(static_cast<int>(GetGValue(a)) - static_cast<int>(GetGValue(b)));
  const int db = std::abs(static_cast<int>(GetBValue(a)) - static_cast<int>(GetBValue(b)));
  return dr + dg + db;
}

COLORREF ToColorRef(const th::Rgba& c) {
  const auto byte = [](float v) {
    const int i = static_cast<int>(std::lround(v * 255.0f));
    return static_cast<BYTE>(i < 0 ? 0 : (i > 255 ? 255 : i));
  };
  return RGB(byte(c.r), byte(c.g), byte(c.b));
}

// The primary button's fill, as the chrome painter composes it: the theme's
// accent over the theme's card. Same maths DialogChrome uses (Composite/Accent/
// Card are exported for exactly this reason).
COLORREF ExpectedButtonPixel(th::Variant variant) {
  return ToColorRef(chrome::Composite(chrome::Accent(variant), chrome::Card(variant)));
}

bool NearColor(COLORREF measured, COLORREF expected, int tolerance, const char* what) {
  const int dr = std::abs(static_cast<int>(GetRValue(measured)) - static_cast<int>(GetRValue(expected)));
  const int dg = std::abs(static_cast<int>(GetGValue(measured)) - static_cast<int>(GetGValue(expected)));
  const int db = std::abs(static_cast<int>(GetBValue(measured)) - static_cast<int>(GetBValue(expected)));
  const bool ok = dr <= tolerance && dg <= tolerance && db <= tolerance;
  if (!ok) {
    std::printf("    [pixel] %s measured=(%u,%u,%u) expected=(%u,%u,%u)\n", what,
                GetRValue(measured), GetGValue(measured), GetBValue(measured),
                GetRValue(expected), GetGValue(expected), GetBValue(expected));
  }
  return ok;
}

}  // namespace

// ===========================================================================
// 1. The mapping itself: pure, so every lane pins it (CONTRACTS D7).
// ===========================================================================

PRISM_TEST("core: the draft's theme IS the token variant the dialog paints") {
  PRISM_CHECK(su::ThemeVariantOf(su::Theme::Daylight) == th::Variant::Daylight);
  PRISM_CHECK(su::ThemeVariantOf(su::Theme::Midnight) == th::Variant::Midnight);
  PRISM_CHECK(su::ThemeVariantOf(su::Theme::HighContrast) == th::Variant::HighContrast);
  // The three tokens the pixel test relies on must be three DIFFERENT colours,
  // otherwise "the pixel changed" would prove nothing.
  const COLORREF day = ExpectedButtonPixel(th::Variant::Daylight);
  const COLORREF night = ExpectedButtonPixel(th::Variant::Midnight);
  const COLORREF high = ExpectedButtonPixel(th::Variant::HighContrast);
  PRISM_CHECK(ChannelDelta(day, night) > 30);
  PRISM_CHECK(ChannelDelta(night, high) > 30);
  PRISM_CHECK(ChannelDelta(day, high) > 30);
}

// ===========================================================================
// 2. THE THEME, ON SCREEN. Real HWND, real combo, real pixels.
// ===========================================================================

PRISM_TEST("win: the theme combo repaints the window -- same point, different RGB") {
  if (!RequireStation()) return;
  su::SettingsDialog dialog;
  if (!dialog.Create(GetModuleHandleW(nullptr), L"C69 theme")) {
    std::printf("    [dialog] Create failed: err=%lu id=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    PRISM_CHECK(false);
    return;
  }
  std::string error;
  dialog.SetPaths(FreshPaths(L"theme"));
  PRISM_CHECK(dialog.LoadFrom(dialog.paths(), &error));
  ::ShowWindow(dialog.Hwnd(), SW_SHOWNOACTIVATE);
  ::UpdateWindow(dialog.Hwnd());
  ::Sleep(60);

  // The sample point: the middle of the primary button (Save), scaled with the
  // window's DPI the same way the chrome model is. Its fill IS Accent(variant).
  RECT client{};
  ::GetClientRect(dialog.Hwnd(), &client);
  const int sampleX = static_cast<int>(std::lround(452.0 * (client.right - client.left) / 640.0));
  const int sampleY = static_cast<int>(std::lround(520.0 * (client.bottom - client.top) / 552.0));

  const COLORREF day = CapturePixel(dialog.Hwnd(), sampleX, sampleY);
  PRISM_CHECK(day != CLR_INVALID);
  PRISM_CHECK(NearColor(day, ExpectedButtonPixel(th::Variant::Daylight), 6, "daylight"));

  // Night, through the control a user touches.
  SelectCombo(dialog, su::ids::kTheme, 1);
  PRISM_CHECK(dialog.State().theme() == su::Theme::Midnight);
  ::UpdateWindow(dialog.Hwnd());  // paints only because the dialog invalidated
  ::Sleep(40);
  const COLORREF night = CapturePixel(dialog.Hwnd(), sampleX, sampleY);
  PRISM_CHECK(NearColor(night, ExpectedButtonPixel(th::Variant::Midnight), 6, "midnight"));
  PRISM_CHECK(ChannelDelta(day, night) > 20);  // the user-visible difference

  // High contrast.
  SelectCombo(dialog, su::ids::kTheme, 2);
  PRISM_CHECK(dialog.State().theme() == su::Theme::HighContrast);
  ::UpdateWindow(dialog.Hwnd());
  ::Sleep(40);
  const COLORREF high = CapturePixel(dialog.Hwnd(), sampleX, sampleY);
  PRISM_CHECK(NearColor(high, ExpectedButtonPixel(th::Variant::HighContrast), 6, "high-contrast"));
  PRISM_CHECK(ChannelDelta(high, night) > 20);
  PRISM_CHECK(ChannelDelta(high, day) > 20);

  std::printf("    [pixels] day=%06lX night=%06lX high=%06lX\n",
              static_cast<unsigned long>(day), static_cast<unsigned long>(night),
              static_cast<unsigned long>(high));
  ::DestroyWindow(dialog.Hwnd());
}

// ===========================================================================
// 3. THE LANGUAGE, ON SCREEN. Real window menu + real dialog rows.
// ===========================================================================

PRISM_TEST("win: picking zh-CN retitles the menu bar and the dialog's rows") {
  if (!RequireStation()) return;
  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"C69 language", GetModuleHandleW(nullptr))) {
    PRISM_CHECK(false);
    return;
  }
  prism::app::Tr en{&TranslatorFor(L"en-US")};
  // The STARTUP bar is applied directly, not through ApplyMenuLabels: the
  // negative control removes the switch-time rebuild only, so this first paint
  // must stay identical in the clean and the mutant build (otherwise the mutant
  // would go red on the setup line and prove nothing about the switch).
  {
    const prism::app::MenuLabels start = prism::app::BuildMenuLabels(en, L"en-US");
    window.SetMenuLabels(start.file, start.compress, start.exit);
    window.SetEditMenuLabels(start.edit, start.deleteItem, start.rename, start.newFolder);
    window.SetSettingsMenuLabel(start.settings);
  }
  const std::wstring englishCompress = window.MenuItemText(
      prism::win::gallery::GalleryWindow::kMenuCompress);
  PRISM_CHECK(englishCompress == L"Compress to a .7z archive");
  PRISM_CHECK(window.MenuTitle() == L"&File");

  su::SettingsDialog dialog;
  if (!dialog.Create(GetModuleHandleW(nullptr), L"C69 language dialog")) {
    PRISM_CHECK(false);
    return;
  }
  std::string error;
  dialog.SetPaths(FreshPaths(L"language"));
  PRISM_CHECK(dialog.LoadFrom(dialog.paths(), &error));
  dialog.SetTranslator(&TranslatorFor(L"en-US"));
  PRISM_CHECK(dialog.ControlTextUtf8(su::ids::kLabelLanguage) == std::string("Language"));

  // The same wiring Main.cpp installs: the app hears the tag, rebuilds the bar
  // with that language's table, and hands the dialog the translator it should
  // read its own rows from.
  dialog.SetLanguageHandler([&window, &dialog](const std::string& tag) {
    const std::wstring wide = prism::win::Utf8ToWide(tag);
    prism::app::Tr live{&TranslatorFor(wide.c_str())};
    prism::app::ApplyMenuLabels(window, prism::app::BuildMenuLabels(live, wide));
    dialog.SetTranslator(live.translator);
  });

  SelectCombo(dialog, su::ids::kLanguage, 1);  // the shipped list is auto, zh-CN, en-US
  PRISM_CHECK(dialog.State().languageTag() == std::string("zh-CN"));

  const std::wstring chineseCompress =
      window.MenuItemText(prism::win::gallery::GalleryWindow::kMenuCompress);
  PRISM_CHECK(chineseCompress != englishCompress);
  PRISM_CHECK(chineseCompress == L"\u538B\u7F29\u4E3A .7z \u5F52\u6863");  // 压缩为 .7z 归档
  PRISM_CHECK(window.MenuTitle() == L"\u6587\u4EF6");                       // 文件
  PRISM_CHECK(dialog.ControlTextUtf8(su::ids::kLabelLanguage) == std::string("\u8BED\u8A00"));

  // ... and back, so the switch is not a one-way trip.
  SelectCombo(dialog, su::ids::kLanguage, 2);
  PRISM_CHECK(window.MenuItemText(prism::win::gallery::GalleryWindow::kMenuCompress) ==
              englishCompress);
  PRISM_CHECK(dialog.ControlTextUtf8(su::ids::kLabelLanguage) == std::string("Language"));

  ::DestroyWindow(dialog.Hwnd());
  ::DestroyWindow(window.Hwnd());
}

// ===========================================================================
// 4. THE CHECKLIST: what each named setting observably does.
//
// The four that have NO runtime consumer are named in docs/USAGE.md; asserting
// here that they are inert would pin the absence of a feature, so they are
// recorded as a boundary instead. The two that ARE wired are driven through the
// product objects (SettingsStore + GalleryWindow).
// ===========================================================================

PRISM_TEST("checklist: every schema key the card names survives a save/load") {
  prism::settings::SettingsStore store;
  store.SetString("appearance.theme", "dark");
  store.SetNumber("appearance.glassIntensity", 0.35);
  store.SetString("appearance.accentColor", "#FF8800");
  store.SetString("extract.overwrite", "replace");
  store.SetBool("extract.verifyAfterExtract", false);
  store.SetBool("window.listMode", true);
  store.SetString("window.sort", "size:desc");

  prism::settings::SettingsStore reloaded;
  const prism::settings::LoadReport report = reloaded.Load(store.ToJson());
  PRISM_CHECK(report.status != prism::settings::LoadStatus::Corrupt);
  PRISM_CHECK(reloaded.GetString("appearance.theme") == std::string("dark"));
  PRISM_CHECK(std::fabs(reloaded.GetNumber("appearance.glassIntensity", 0.0) - 0.35) < 1e-9);
  PRISM_CHECK(reloaded.GetString("appearance.accentColor") == std::string("#FF8800"));
  PRISM_CHECK(reloaded.GetString("extract.overwrite") == std::string("replace"));
  PRISM_CHECK(reloaded.GetBool("extract.verifyAfterExtract", true) == false);
  PRISM_CHECK(reloaded.GetBool("window.listMode", false) == true);
  PRISM_CHECK(reloaded.GetString("window.sort") == std::string("size:desc"));
}

PRISM_TEST("checklist: list mode and sort are observable on the window's own model") {
  if (!RequireStation()) return;
  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"C69 checklist", GetModuleHandleW(nullptr))) {
    PRISM_CHECK(false);
    return;
  }
  std::vector<prism::ArchiveItem> items;
  items.push_back(prism::ArchiveItem{1, "a.txt", 100, 90, 0, 0, false, false});
  items.push_back(prism::ArchiveItem{2, "b.txt", 900, 800, 0, 0, false, false});
  window.SetItems(&items);

  // listMode: the grid is the default; setting it flips the model the renderer
  // reads (scene.listMode) and the layout follows it.
  PRISM_CHECK(!window.ListMode());
  window.SetListMode(true);
  PRISM_CHECK(window.ListMode());

  // sort: the app's sort spec reaches the window and comes back unchanged.
  prism::list::MultiSortSpec spec;
  prism::list::SortSpec key;
  key.key = prism::list::SortKey::kOriginalSize;
  key.direction = prism::list::SortDirection::kDescending;
  spec.keys.push_back(key);
  window.SetSortSpec(spec);
  PRISM_CHECK_EQ(window.SortSpec().keys.size(), size_t{1});
  PRISM_CHECK(window.SortSpec().keys[0].key == prism::list::SortKey::kOriginalSize);
  PRISM_CHECK(window.SortSpec().keys[0].direction ==
              prism::list::SortDirection::kDescending);
  ::DestroyWindow(window.Hwnd());
}

int main(int argc, char** argv) {
#if defined(PRISM_C69_MUTANT_NO_THEME_INVALIDATE)
  prism::test::SetMutationVerdict("C69 MUTATION CAUGHT: no_theme_invalidate",
                                  [] { return !su::kThemeRepaintOnChange; });
#elif defined(PRISM_C69_MUTANT_NO_MENU_REBUILD)
  prism::test::SetMutationVerdict("C69 MUTATION CAUGHT: no_menu_rebuild",
                                  [] { return !prism::app::kRebuildMenuOnLanguageChange; });
#endif
  const int failures = prism::test::RunAll(argc, argv);
  if (g_skip) {
    std::printf("SKIP: no interactive window station\n");
    return 77;
  }
  if (failures != 0) return 1;
  std::puts("ALL C69 SETTINGS-EFFECT CHECKS PASSED");
  return 0;
}
