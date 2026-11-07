// PrismArchive -- PrismWin
// C125: THE TRUST SIGNALS -- F5, Ctrl+E, and "follow the system".
//
// Three defects, all measured against Bandizip 6.29 and the Windows desktop:
//   1. F5 opened nothing. Bandizip binds 设置(&S) to F5; a migrated user pressing
//      it here got silence. The dangerous "fix" is to bind F5 to extraction, so
//      case 1 asserts BOTH halves: the settings launcher ran AND no extraction
//      handler was reached.
//   2. Ctrl+E wrote the whole archive to disk with no prompt. It now raises the
//      extract dialog (case 1) and the silent writer moved to Ctrl+Alt+E, which
//      case 1 also proves still reaches the app.
//   3. "system" was hard-wired to Midnight, so a light desktop got a black
//      window. Case 3 reads the REAL registry switch out of a temporary HKCU key
//      (1 -> Daylight, 0 -> Midnight, missing -> keep the previous shell), and
//      case 2 proves WM_SETTINGCHANGE("ImmersiveColorSet") is the event that
//      re-applies it -- without waiting for an OS theme change.
//
// Case 4 is the dialog itself: the three facts (path / file count / bytes) are
// on screen, the path is editable, a cancel accepts NOTHING, and a blank path
// is refused rather than handing the app "".
//
// The suite never touches the user's real Personalize key: it creates
// HKCU\Software\PrismArchive\Tests\c125-<pid>, reads through the same
// production function, and removes it again.
#include <windows.h>

#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismWin/ExtractDialog.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SystemTheme.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

using prism::chrome::ShellVariantFor;
using prism::chrome::SystemTheme;
using prism::keys::Command;
using prism::theme::Variant;
using prism::win::extractdlg::ExtractDialog;
using prism::win::extractdlg::ExtractDialogModel;
using prism::win::gallery::GalleryWindow;
using prism::win::systemtheme::FromSample;
using prism::win::systemtheme::ReadSample;

constexpr int kSkipReturnCode = 77;
bool g_noWindowStation = false;

void BuildCatalog(std::vector<prism::ArchiveItem>* catalog) {
  const char* paths[] = {"docs", "docs/a.txt", "notes.txt"};
  for (const char* path : paths) {
    prism::ArchiveItem item;
    item.index = static_cast<std::uint32_t>(catalog->size());
    item.path = path;
    item.isDir = std::string(path).find('.') == std::string::npos;
    item.size = 10;
    catalog->push_back(item);
  }
}

bool MakeWindow(GalleryWindow* window) {
  if (window->Create(L"PrismArchive c125 signals", GetModuleHandleW(nullptr))) return true;
  std::puts("  [skip] no window station");
  g_noWindowStation = true;
  return false;
}

// ---- registry fixtures (never the user's real Personalize key) ------------
void TestSubKey(wchar_t* out, std::size_t count) {
  std::swprintf(out, count, L"Software\\PrismArchive\\Tests\\c125-%lu",
                static_cast<unsigned long>(::GetCurrentProcessId()));
}

bool WriteValue(const wchar_t* subkey, DWORD type, const void* data, DWORD bytes) {
  HKEY key = nullptr;
  if (::RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr,
                        &key, nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status = ::RegSetValueExW(
      key, prism::win::systemtheme::kAppsUseLightThemeValue, 0, type,
      static_cast<const BYTE*>(data), bytes);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool WriteDword(const wchar_t* subkey, DWORD value) {
  return WriteValue(subkey, REG_DWORD, &value, sizeof(value));
}

bool WriteString(const wchar_t* subkey) {
  const wchar_t text[] = L"1";
  return WriteValue(subkey, REG_SZ, text, static_cast<DWORD>(sizeof(text)));
}

bool DeleteValue(const wchar_t* subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS status =
      ::RegDeleteValueW(key, prism::win::systemtheme::kAppsUseLightThemeValue);
  ::RegCloseKey(key);
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void RemoveTestKey(const wchar_t* subkey) {
  DeleteValue(subkey);
  ::RegDeleteKeyW(HKEY_CURRENT_USER, subkey);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. THE TWO CHORDS, on a real window: F5 opens settings (never extracts),
//    Ctrl+E asks, and Ctrl+Alt+E is the silent path that still works.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 F5 opens settings, Ctrl+E asks, Ctrl+Alt+E extracts") {
  GalleryWindow window;
  if (!MakeWindow(&window)) return;
  std::vector<prism::ArchiveItem> catalog;
  BuildCatalog(&catalog);
  window.SetItems(&catalog);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1000, 700));
  window.SetSelection(0);

  int settingsOpened = 0;
  int asked = 0;
  int activated = 0;
  window.SetSettingsLauncher([&settingsOpened]() { ++settingsOpened; });
  window.SetExtractDialogHandler([&asked]() { ++asked; });
  window.SetActivateHandler([&activated](int) { ++activated; });

  // F5: the settings door, and NOT the extractor. Both halves are asserted,
  // because "F5 did something" is exactly the wrong green.
  window.DispatchKeyStroke({prism::keys::kKeyF5, false, false, false});
  PRISM_CHECK_EQ(settingsOpened, 1);
  PRISM_CHECK_EQ(activated, 0);
  PRISM_CHECK_EQ(asked, 0);
  // The modal gate closed with the launcher (a stuck gate would freeze Ctrl+E).
  PRISM_CHECK(!window.SettingsModalOpen());

  // Ctrl+E: the ask-before-writing dialog, never the writer.
  window.DispatchKeyStroke({static_cast<std::uint32_t>('E'), true, false, false});
  PRISM_CHECK_EQ(asked, 1);
  PRISM_CHECK_EQ(activated, 0);

  // Ctrl+Alt+E: the silent smart extract still reaches the app.
  window.DispatchKeyStroke({static_cast<std::uint32_t>('E'), true, false, true});
  PRISM_CHECK_EQ(activated, 1);
  PRISM_CHECK_EQ(asked, 1);

  // The decision table and the window agree about the chord (no second map).
  PRISM_CHECK_EQ(prism::keys::Resolve({prism::keys::kKeyF5, false, false, false}),
                 Command::OpenSettings);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 2. "跟随系统" must FOLLOW: the OS switch is WM_SETTINGCHANGE("ImmersiveColorSet")
//    and nothing else may re-theme the window.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 only ImmersiveColorSet re-themes the window") {
  GalleryWindow window;
  if (!MakeWindow(&window)) return;
  int themed = 0;
  window.SetSystemThemeHandler([&themed]() { ++themed; });

  const wchar_t area[] = L"ImmersiveColorSet";
  window.Dispatch(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(area));
  PRISM_CHECK_EQ(themed, 1);
  PRISM_CHECK_EQ(window.SystemThemeNotifications(), 1);

  // A metrics/font change is not a light/dark change: no repaint decision.
  window.Dispatch(WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"WindowMetrics"));
  PRISM_CHECK_EQ(themed, 1);
  PRISM_CHECK_EQ(window.SystemThemeNotifications(), 1);
  // Neither is a null lParam (some senders broadcast the bare message).
  window.Dispatch(WM_SETTINGCHANGE, 0, 0);
  PRISM_CHECK_EQ(themed, 1);

  // The comparison is case-insensitive, the way the OS spells the area. A
  // strict compare would silently stop following on a differently-cased sender.
  PRISM_CHECK(prism::win::systemtheme::IsImmersiveColorSetChange(
      reinterpret_cast<LPARAM>(L"immersivecolorset")));
  PRISM_CHECK(!prism::win::systemtheme::IsImmersiveColorSetChange(0));
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 3. THE REGISTRY, injected: AppsUseLightTheme = 1 -> Daylight, 0 -> Midnight.
//    The key is a temporary one we create, so the assertion does not depend on
//    (or disturb) the desktop the suite happens to run on.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 the AppsUseLightTheme DWORD decides the system variant") {
  wchar_t subkey[160];
  TestSubKey(subkey, 160);
  RemoveTestKey(subkey);  // a crashed previous run must not leave a value behind

  // 1 = light (this machine's real value): Daylight, NOT Midnight.
  if (!PRISM_CHECK(WriteDword(subkey, 1))) return;
  {
    const prism::win::systemtheme::Sample sample = ReadSample(HKEY_CURRENT_USER, subkey);
    PRISM_CHECK(sample.present);
    PRISM_CHECK_EQ(sample.value, 1u);
    const SystemTheme theme = FromSample(sample);
    PRISM_CHECK_EQ(theme, SystemTheme::Light);
    PRISM_CHECK_EQ(ShellVariantFor("system", false, theme), Variant::Daylight);
  }

  // 0 = dark: Midnight.
  if (!PRISM_CHECK(WriteDword(subkey, 0))) { RemoveTestKey(subkey); return; }
  {
    const prism::win::systemtheme::Sample sample = ReadSample(HKEY_CURRENT_USER, subkey);
    PRISM_CHECK(sample.present);
    PRISM_CHECK_EQ(sample.value, 0u);
    const SystemTheme theme = FromSample(sample);
    PRISM_CHECK_EQ(theme, SystemTheme::Dark);
    PRISM_CHECK_EQ(ShellVariantFor("system", false, theme), Variant::Midnight);
  }

  // No value = no evidence. Unknown keeps the previous shell; it must NOT be
  // read as "0" (a missing REG_DWORD is not a dark desktop).
  DeleteValue(subkey);
  {
    const prism::win::systemtheme::Sample sample = ReadSample(HKEY_CURRENT_USER, subkey);
    PRISM_CHECK(!sample.present);
    const SystemTheme theme = FromSample(sample);
    PRISM_CHECK_EQ(theme, SystemTheme::Unknown);
    PRISM_CHECK_EQ(ShellVariantFor("system", false, theme), Variant::Midnight);
  }

  // A REG_SZ "1" is not the switch Windows honours: wrong type = no answer.
  if (PRISM_CHECK(WriteString(subkey))) {
    const prism::win::systemtheme::Sample sample = ReadSample(HKEY_CURRENT_USER, subkey);
    PRISM_CHECK(!sample.present);
    PRISM_CHECK_EQ(FromSample(sample), SystemTheme::Unknown);
  }
  RemoveTestKey(subkey);
  // ... and the key is really gone (the next run starts clean).
  PRISM_CHECK(!ReadSample(HKEY_CURRENT_USER, subkey).present);
}

// ---------------------------------------------------------------------------
// 4. THE DIALOG: path + file count + bytes on screen, cancel accepts nothing.
// ---------------------------------------------------------------------------
PRISM_TEST("c125 the extract dialog shows the three facts and a cancel writes nothing") {
  ExtractDialogModel model;
  model.targetDir = L"C:\\Users\\tester\\Downloads\\sample";
  model.summary = prism::win::extractdlg::BuildSummary(12, 2ull * 1024ull * 1024ull);

  ExtractDialog dialog;
  dialog.SetModel(model);
  if (!dialog.Create(GetModuleHandleW(nullptr))) {
    std::puts("  [skip] no window station");
    g_noWindowStation = true;
    return;
  }
  // The three facts the card demands, read off the CONTROLS (not the model).
  // (Compared byte-for-byte; PRISM_CHECK_EQ streams its operands and a
  // std::wstring has no narrow operator<< -- so equality is asserted directly.)
  PRISM_CHECK(dialog.ControlText(prism::win::extractdlg::ids::kTarget) == model.targetDir);
  const std::wstring summary =
      dialog.ControlText(prism::win::extractdlg::ids::kSummary);
  PRISM_CHECK(summary.find(L"12") != std::wstring::npos);        // file count
  PRISM_CHECK(summary.find(L"2.0 MB") != std::wstring::npos);    // bytes
  PRISM_CHECK(summary.find(L"\u4E2A\u6587\u4EF6") != std::wstring::npos);  // 个文件
  // The path field is editable: it is not a label.
  PRISM_CHECK((::GetWindowLongPtrW(dialog.Control(prism::win::extractdlg::ids::kTarget),
                                   GWL_STYLE) & ES_READONLY) == 0);

  // Cancel: the answer is "nothing", and the app is handed no target at all.
  dialog.Cancel();
  PRISM_CHECK(!dialog.Accepted());
  PRISM_CHECK(dialog.TargetDir().empty());

  // Esc through the real message path cancels too (a second dialog, same model).
  ExtractDialog escaped;
  escaped.SetModel(model);
  if (escaped.Create(GetModuleHandleW(nullptr))) {
    escaped.Dispatch(WM_KEYDOWN, VK_ESCAPE, 0);
    PRISM_CHECK(!escaped.Accepted());
    PRISM_CHECK(escaped.TargetDir().empty());
  }

  // Accept: the typed path is trimmed and returned; a blank path is refused
  // (the dialog stays up with its hint) so the app never receives "".
  ExtractDialog accepted;
  accepted.SetModel(model);
  if (accepted.Create(GetModuleHandleW(nullptr))) {
    ::SetWindowTextW(accepted.Control(prism::win::extractdlg::ids::kTarget), L"  D:\\out  ");
    accepted.Accept();
    PRISM_CHECK(accepted.Accepted());
    PRISM_CHECK(accepted.TargetDir() == std::wstring(L"D:\\out"));
  }
  ExtractDialog blank;
  blank.SetModel(model);
  if (blank.Create(GetModuleHandleW(nullptr))) {
    ::SetWindowTextW(blank.Control(prism::win::extractdlg::ids::kTarget), L"   ");
    blank.Accept();
    PRISM_CHECK(!blank.Accepted());            // refused, not accepted-as-empty
    PRISM_CHECK(blank.TargetDir().empty());
    PRISM_CHECK(blank.ControlText(prism::win::extractdlg::ids::kHint) ==
                model.emptyTargetHint);
    blank.Cancel();
  }
}

int main(int argc, char** argv) {
  // Real screen/registry facts, so the process must not be DPI-virtualised or
  // com-initialised by someone else.
  ::SetProcessDPIAware();
  ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noWindowStation) {
    std::puts("C125 SKIPPED: no window station");
    ::CoUninitialize();
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL C125 SIGNAL CHECKS PASSED");
  ::CoUninitialize();
  return failures;
}
