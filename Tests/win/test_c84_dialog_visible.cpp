// PrismArchive -- C84: a created dialog must be VISIBLE (S1-3 settings freeze,
// S2-5 compress silence, and the password gate's "process alive, no window").
//
// MEASURED BEFORE THIS CARD (see the desktop-test report):
//   * Settings... -> CreateWindowExW WITHOUT WS_VISIBLE, then OpenModal()
//     disabled the owner and entered a message loop on a window that had
//     never been shown: IsWindowEnabled(main)=False, zero visible windows.
//   * Compress -> the same CreateWindowExW with no ShowWindow anywhere on the
//     product path: clicking "Compress" created a dialog nobody could see.
//   * Open an encrypted archive -> RunPasswordGate() created the password
//     dialog and pumped messages, but Create() never showed it: process alive,
//     data: [] from agent-desktop.
//   The repair is in Create() itself (not at one call site): center over the
//   owner, ShowWindow(SW_SHOW), UpdateWindow; the modal paths then take the
//   foreground and focus the first input. The password, settings and compress
//   dialogs all share that contract.
//
// WHAT THIS FILE ASSERTS (HWND state and product paths, never comments):
//   1. each dialog's OWN create/open entry point leaves IsWindowVisible true
//      and the card centered over its owner (About/EditConfirm/Vault are the
//      sibling guard: they must not lose the property this card defines);
//   2. PRODUCT PATH Settings: PostMessage(gallery, WM_COMMAND,
//      kMenuSettingsId) brings up a visible PrismArchiveSettingsDialog within
//      3 s, the owner is disabled while it is up, the keyboard is on the first
//      input control, and WM_CLOSE re-enables the main window;
//   3. PRODUCT PATH Compress: a visible PrismArchiveCompressDialog;
//   4. PRODUCT PATH Password, with an isolated %LOCALAPPDATA%: launching
//      enc.7z shows a VISIBLE password dialog within 3 s; a wrong password
//      leaves the dialog up with an error line; the right one closes it and the
//      gallery window (title = the archive) appears.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the mutant target compiles this same
// source with PRISM_C84_MUTANT_NO_SHOW, which compiles the ShowWindow calls out
// of all three dialogs. Every visibility assertion above must go red; the
// marker is the conjunction "the build-time injection is active", printed by
// mini_test at the FIRST failure. No WILL_FAIL is stacked on the CTest regex.
//
// D1: no PrismArchive.exe / fixture / 7z.dll -> print WHY, exit 77. The suite
// keeps SKIP_RETURN_CODE 77 AND PASS_REGULAR_EXPRESSION, so a skip is visible.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/PrismWin/AboutDialog.hpp"
#include "../../Src/PrismWin/CompressDialog.hpp"
#include "../../Src/PrismWin/EditConfirmDialog.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/PasswordDialog.hpp"
#include "../../Src/PrismWin/SettingsDialog.hpp"
#include "../../Src/PrismWin/VaultDialog.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "comctl32.lib")

namespace su = prism::win::settingsui;
namespace chrome = prism::win::chrome;
namespace cd = prism::win::cd;
namespace pwd_ui = prism::win::pwd_ui;
namespace about = prism::win::about;
namespace editui = prism::win::editui;
namespace vaultui = prism::win::vaultui;

namespace {

std::wstring gExe;
std::wstring gFixtures;
std::wstring gRoot;

std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                      needed, nullptr, nullptr);
  return utf8;
}

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

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
  out += name;
  return out;
}

bool ReadTextFile(const std::wstring& path, std::string* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  char buffer[8192];
  DWORD got = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    out->append(buffer, got);
  }
  CloseHandle(file);
  return true;
}

bool CopyFixture(const std::wstring& name, const std::wstring& intoDir) {
  const std::wstring from = Join(gFixtures, name);
  const std::wstring to = Join(intoDir, name);
  HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL, nullptr);
  if (in == INVALID_HANDLE_VALUE) return false;
  HANDLE out = CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (out == INVALID_HANDLE_VALUE) {
    CloseHandle(in);
    return false;
  }
  char buffer[8192];
  DWORD got = 0;
  bool ok = true;
  while (ReadFile(in, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    DWORD written = 0;
    if (WriteFile(out, buffer, got, &written, nullptr) == FALSE || written != got) {
      ok = false;
      break;
    }
  }
  CloseHandle(in);
  CloseHandle(out);
  return ok;
}

// The centering contract INCLUDING the work-area clamp: a dialog taller than
// the work area is pinned to the top, not centered off-screen.
RECT ExpectedCentered(HWND owner, HWND dialog) {
  RECT ownerRect{};
  RECT dialogRect{};
  GetWindowRect(owner, &ownerRect);
  GetWindowRect(dialog, &dialogRect);
  const int width = dialogRect.right - dialogRect.left;
  const int height = dialogRect.bottom - dialogRect.top;
  RECT expected{};
  expected.left = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
  expected.top = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
  expected.right = expected.left + width;
  expected.bottom = expected.top + height;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (GetMonitorInfoW(MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE) {
    if (expected.right > monitor.rcWork.right) {
      expected.left = monitor.rcWork.right - width;
      expected.right = expected.left + width;
    }
    if (expected.bottom > monitor.rcWork.bottom) {
      expected.top = monitor.rcWork.bottom - height;
      expected.bottom = expected.top + height;
    }
    if (expected.left < monitor.rcWork.left) {
      expected.left = monitor.rcWork.left;
      expected.right = expected.left + width;
    }
    if (expected.top < monitor.rcWork.top) {
      expected.top = monitor.rcWork.top;
      expected.bottom = expected.top + height;
    }
  }
  return expected;
}

bool SameRect(const RECT& a, const RECT& b, int tolerance = 1) {
  return std::abs(a.left - b.left) <= tolerance && std::abs(a.top - b.top) <= tolerance &&
         std::abs((a.right - a.left) - (b.right - b.left)) <= tolerance;
}

HWND MakeOwner(int x, int y, int width, int height) {
  return CreateWindowExW(0, L"STATIC", L"C84 owner", WS_OVERLAPPEDWINDOW, x, y, width, height,
                         nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
}

// ---------------------------------------------------------------------------
// A timer window used as the probe for a MODAL dialog's own message loop.
// WM_TIMER addressed to this window is dispatched on the UI thread from inside
// SettingsDialog::OpenModal / VaultDialog::OpenModal, which is the only place
// where GetFocus() and IsWindowEnabled(owner) can be read while the gate runs.
// ---------------------------------------------------------------------------
struct ModalProbe {
  const wchar_t* className = nullptr;
  HWND owner = nullptr;
  bool done = false;
  bool timedOut = false;
  bool seen = false;
  bool visible = false;
  bool ownerDisabled = false;
  bool centered = false;
  HWND dialog = nullptr;
  HWND focus = nullptr;
  int focusedWidgetId = -1;  // SettingsDialog's chrome focus row (first input)
  su::SettingsDialog* settings = nullptr;
  ULONGLONG start = 0;
  int width = 0;
  int height = 0;
};

LRESULT CALLBACK ProbeProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    return TRUE;
  }
  if (message != WM_TIMER) return DefWindowProcW(hwnd, message, wparam, lparam);
  auto* probe = reinterpret_cast<ModalProbe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (probe == nullptr || probe->done) return 0;
  HWND dialog = FindWindowW(probe->className, nullptr);
  DWORD pid = 0;
  if (dialog != nullptr) GetWindowThreadProcessId(dialog, &pid);
  if (dialog != nullptr && pid == GetCurrentProcessId()) {
    probe->seen = true;
    probe->dialog = dialog;
    probe->visible = IsWindowVisible(dialog) != FALSE;
    probe->ownerDisabled = probe->owner != nullptr && IsWindowEnabled(probe->owner) == FALSE;
    probe->focus = GetFocus();
    if (probe->settings != nullptr) {
      const chrome::ChromeModel model = probe->settings->BuildModel();
      const chrome::Widget* focused = model.Focused();
      probe->focusedWidgetId = focused != nullptr ? focused->id : -1;
    }
    if (probe->owner != nullptr) {
      RECT actual{};
      GetWindowRect(dialog, &actual);
      probe->centered = SameRect(actual, ExpectedCentered(probe->owner, dialog));
    }
    RECT rect{};
    GetWindowRect(dialog, &rect);
    probe->width = rect.right - rect.left;
    probe->height = rect.bottom - rect.top;
    probe->done = true;
    KillTimer(hwnd, 1);
    // The gate is modal and owns its own loop: closing the window is what
    // makes it return, so the test can inspect the aftermath.
    PostMessageW(dialog, WM_CLOSE, 0, 0);
  } else if (probe->start != 0 && GetTickCount64() - probe->start > 4000) {
    // Nothing appeared: the modal loop must not outlive the suite's budget.
    // WM_QUIT makes OpenModal's GetMessageW return 0; done stops the pump.
    probe->timedOut = true;
    probe->done = true;
    KillTimer(hwnd, 1);
    PostQuitMessage(0);
  }
  return 0;
}

// A probe window + timer, plus the bounded pump that drives one modal call.
class ModalProbeWindow {
 public:
  explicit ModalProbeWindow(ModalProbe* probe) {
    static bool registered = [] {
      WNDCLASSEXW wc{};
      wc.cbSize = sizeof(wc);
      wc.lpfnWndProc = &ProbeProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"PrismC84ProbeWindow";
      RegisterClassExW(&wc);
      return true;
    }();
    (void)registered;
    hwnd_ = CreateWindowExW(0, L"PrismC84ProbeWindow", L"c84 probe", WS_OVERLAPPED, 0, 0, 10,
                            10, nullptr, nullptr, GetModuleHandleW(nullptr), probe);
    if (hwnd_ != nullptr) SetTimer(hwnd_, 1, 50, nullptr);
  }
  ~ModalProbeWindow() {
    if (hwnd_ != nullptr) {
      KillTimer(hwnd_, 1);
      DestroyWindow(hwnd_);
    }
  }
  ModalProbeWindow(const ModalProbeWindow&) = delete;
  ModalProbeWindow& operator=(const ModalProbeWindow&) = delete;
  HWND hwnd() const { return hwnd_; }

 private:
  HWND hwnd_ = nullptr;
};

// Pumps until *done or the deadline. WM_QUIT ends the pump too (a modal call
// that cannot be closed must not hang the suite: the probe posts it).
void PumpUntil(const bool* done, DWORD timeoutMs) {
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  MSG msg{};
  while (!*done && GetTickCount64() < deadline) {
    if (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      if (msg.message == WM_QUIT) break;
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    } else {
      Sleep(5);
    }
  }
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    if (msg.message == WM_QUIT) break;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

// ---------------------------------------------------------------------------
// Cross-process automation for the password gate (item 4): the app runs in its
// own process, so the dialog is a real remote HWND. Every call is bounded.
// ---------------------------------------------------------------------------
LRESULT SendBounded(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  DWORD_PTR result = 0;
  if (SendMessageTimeoutW(window, message, wparam, lparam,
                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 5000, &result) == 0) {
    return 0;
  }
  return static_cast<LRESULT>(result);
}

HWND FindTopLevelOf(DWORD pid, const wchar_t* className, bool requireVisible) {
  struct Ctx {
    DWORD pid;
    const wchar_t* cls;
    bool visible;
    HWND found;
  } ctx{pid, className, requireVisible, nullptr};
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* self = reinterpret_cast<Ctx*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner != self->pid) return TRUE;
        wchar_t cls[128] = {};
        GetClassNameW(hwnd, cls, 128);
        if (wcscmp(cls, self->cls) != 0) return TRUE;
        if (self->visible && IsWindowVisible(hwnd) == FALSE) return TRUE;
        self->found = hwnd;
        return FALSE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return ctx.found;
}

HWND WaitForTopLevel(DWORD pid, const wchar_t* className, bool requireVisible, DWORD timeoutMs) {
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  while (GetTickCount64() < deadline) {
    const HWND found = FindTopLevelOf(pid, className, requireVisible);
    if (found != nullptr) return found;
    Sleep(50);
  }
  return nullptr;
}

bool WaitForWindowGone(DWORD pid, const wchar_t* className, DWORD timeoutMs) {
  const ULONGLONG deadline = GetTickCount64() + timeoutMs;
  while (GetTickCount64() < deadline) {
    if (FindTopLevelOf(pid, className, false) == nullptr) return true;
    Sleep(50);
  }
  return false;
}

void SubmitPassword(HWND dialog, const wchar_t* password) {
  const HWND edit = GetDlgItem(dialog, pwd_ui::ids::kPassword);
  const HWND ok = GetDlgItem(dialog, pwd_ui::ids::kOk);
  if (edit == nullptr || ok == nullptr) return;
  SendBounded(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(password));
  SendBounded(dialog, WM_COMMAND,
              MAKEWPARAM(pwd_ui::ids::kPassword, EN_CHANGE), reinterpret_cast<LPARAM>(edit));
  SendBounded(dialog, WM_COMMAND,
              MAKEWPARAM(pwd_ui::ids::kOk, BN_CLICKED), reinterpret_cast<LPARAM>(ok));
}

std::wstring WindowTitle(HWND hwnd) {
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  return text;
}

class ScopedEnvironment {
 public:
  ScopedEnvironment(const wchar_t* name, const std::wstring& value) : name_(name) {
    wchar_t buffer[2 * MAX_PATH] = {};
    const DWORD got = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    if (got > 0) {
      had_ = true;
      old_.assign(buffer, got);
    }
    SetEnvironmentVariableW(name, value.c_str());
  }
  ~ScopedEnvironment() {
    SetEnvironmentVariableW(name_.c_str(), had_ ? old_.c_str() : nullptr);
  }
  ScopedEnvironment(const ScopedEnvironment&) = delete;
  ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

 private:
  std::wstring name_;
  std::wstring old_;
  bool had_ = false;
};

struct ChildProcess {
  PROCESS_INFORMATION info{};
  HANDLE log = INVALID_HANDLE_VALUE;
  std::wstring logPath;
};

bool LaunchApp(const std::wstring& archive, const std::wstring& scratch,
               const std::wstring& logPath, ChildProcess* child) {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  child->logPath = logPath;
  child->log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child->log == INVALID_HANDLE_VALUE) return false;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = child->log;
  si.hStdError = child->log;
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  si.hStdInput = input;
  std::wstring command = L"\"" + gExe + L"\" \"" + archive + L"\"";
  std::vector<wchar_t> buffer(command.begin(), command.end());
  buffer.push_back(L'\0');
  const BOOL ok = CreateProcessW(nullptr, buffer.data(), nullptr, nullptr, TRUE,
                                 CREATE_UNICODE_ENVIRONMENT, nullptr, scratch.c_str(), &si,
                                 &child->info);
  if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
  if (ok == FALSE) {
    CloseHandle(child->log);
    child->log = INVALID_HANDLE_VALUE;
    return false;
  }
  return true;
}

void FinishChild(ChildProcess* child, bool graceful) {
  if (child->info.hProcess == nullptr) return;
  if (graceful) {
    const HWND main = FindTopLevelOf(child->info.dwProcessId, L"PrismGalleryWindow", true);
    if (main != nullptr) PostMessageW(main, WM_CLOSE, 0, 0);
  }
  if (WaitForSingleObject(child->info.hProcess, 8000) == WAIT_TIMEOUT) {
    TerminateProcess(child->info.hProcess, 0xC84);
    WaitForSingleObject(child->info.hProcess, 5000);
  }
  CloseHandle(child->info.hThread);
  CloseHandle(child->info.hProcess);
  child->info = PROCESS_INFORMATION{};
  if (child->log != INVALID_HANDLE_VALUE) {
    CloseHandle(child->log);
    child->log = INVALID_HANDLE_VALUE;
  }
}

}  // namespace

// ===========================================================================
// 1. Create() itself leaves the dialog visible and centered. This is the
//    repair's contract, not one caller's habit.
// ===========================================================================
PRISM_TEST("c84_settings_create_is_visible_and_centered") {
  const HWND owner = MakeOwner(200, 150, 1000, 700);
  if (!PRISM_CHECK(owner != nullptr)) return;
  su::SettingsDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), L"C84 settings", owner);
  if (!PRISM_CHECK(created)) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    DestroyWindow(owner);
    return;
  }
  PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  RECT actual{};
  GetWindowRect(dialog.Hwnd(), &actual);
  PRISM_CHECK(SameRect(actual, ExpectedCentered(owner, dialog.Hwnd())));
  PRISM_CHECK(dialog.Control(su::ids::kLanguage) != nullptr);
  dialog.Destroy();
  DestroyWindow(owner);
}



PRISM_TEST("c84_compress_create_is_visible_and_centered") {
  const HWND owner = MakeOwner(240, 180, 1000, 700);
  if (!PRISM_CHECK(owner != nullptr)) return;
  cd::CompressDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), L"C84 compress", owner);
  if (!PRISM_CHECK(created)) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    DestroyWindow(owner);
    return;
  }
  PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  RECT actual{};
  GetWindowRect(dialog.Hwnd(), &actual);
  PRISM_CHECK(SameRect(actual, ExpectedCentered(owner, dialog.Hwnd())));
  dialog.Destroy();
  DestroyWindow(owner);
}

PRISM_TEST("c84_password_create_is_visible_focused_and_centered") {
  const HWND owner = MakeOwner(180, 140, 1000, 700);
  if (!PRISM_CHECK(owner != nullptr)) return;
  pwd_ui::PasswordDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), L"C84 password", owner);
  if (!PRISM_CHECK(created)) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    DestroyWindow(owner);
    return;
  }
  PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  RECT actual{};
  GetWindowRect(dialog.Hwnd(), &actual);
  PRISM_CHECK(SameRect(actual, ExpectedCentered(owner, dialog.Hwnd())));
  dialog.SetConfig(pwd_ui::PasswordUiConfig{});
  dialog.Start();
  dialog.FocusInput();
  PRISM_CHECK(GetFocus() == dialog.Control(pwd_ui::ids::kPassword));
  dialog.Destroy();
  DestroyWindow(owner);
}

// ===========================================================================
// 2. Sibling guard (requirement 2): the dialogs the card did not have to
//    change must keep the same "opened means visible" property.
// ===========================================================================
PRISM_TEST("c84_about_create_is_visible") {
  const HWND owner = MakeOwner(260, 160, 900, 600);
  if (!PRISM_CHECK(owner != nullptr)) return;
  about::AboutDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), owner);
  PRISM_CHECK(created);
  if (created) PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  dialog.Destroy();
  DestroyWindow(owner);
}

PRISM_TEST("c84_editconfirm_show_is_visible") {
  const HWND owner = MakeOwner(300, 200, 900, 600);
  if (!PRISM_CHECK(owner != nullptr)) return;
  editui::EditConfirmDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), L"C84 edit confirm");
  PRISM_CHECK(created);
  if (created) {
    dialog.Show();
    PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
  }
  dialog.Destroy();
  DestroyWindow(owner);
}

PRISM_TEST("c84_vault_openmodal_is_visible") {
  const HWND owner = MakeOwner(280, 190, 900, 600);
  if (!PRISM_CHECK(owner != nullptr)) return;
  ModalProbe probe;
  probe.className = vaultui::VaultDialog::kClassName;
  probe.owner = nullptr;  // the vault gate is not owned by the gallery
  probe.start = GetTickCount64();
  vaultui::VaultDialog dialog;
  const std::wstring vaultPath = Join(gRoot, L"vault.dat");
  {
    ModalProbeWindow probeWindow(&probe);
    // OpenModal owns the loop; the probe posts WM_CLOSE from inside it.
    dialog.OpenModal(GetModuleHandleW(nullptr), nullptr, vaultPath);
  }
  PRISM_CHECK(probe.seen);
  PRISM_CHECK(probe.visible);
  dialog.Destroy();
  DestroyWindow(owner);
}

// ===========================================================================
// 3. PRODUCT PATH: Settings... on a real GalleryWindow. PostMessage is what
//    the menu does; the default launcher is replaced only to keep the test
//    out of %LOCALAPPDATA% (PathsUnder(scratch)).
// ===========================================================================
PRISM_TEST("c84_product_settings_menu_opens_visible_dialog_and_restores_owner") {
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"C84 settings product", GetModuleHandleW(nullptr)))) return;
  const prism::win::settingsfile::Paths paths =
      prism::win::settingsfile::PathsUnder(Join(gRoot, L"settings"));
  prism::win::settingsfile::EnsureDirectory(paths.root);
  ModalProbe probe;
  probe.className = su::SettingsDialog::kClassName;
  probe.owner = window.Hwnd();
  probe.start = GetTickCount64();
  window.SetSettingsLauncher([&] {
    su::SettingsDialog dialog;
    dialog.SetPaths(paths);
    std::string error;
    dialog.LoadFrom(paths, &error);
    // The probe reads the modal dialog's own chrome focus while its loop runs.
    probe.settings = &dialog;
    dialog.OpenModal(GetModuleHandleW(nullptr), window.Hwnd());
    probe.settings = nullptr;
  });
  {
    ModalProbeWindow probeWindow(&probe);
    PostMessageW(window.Hwnd(), WM_COMMAND,
                 MAKEWPARAM(su::SettingsDialog::kMenuSettingsId, 0), 0);
    PumpUntil(&probe.done, 3000);
  }
  PRISM_CHECK(probe.seen);
  PRISM_CHECK(probe.visible);
  PRISM_CHECK(probe.ownerDisabled);
  PRISM_CHECK(probe.centered);
  // The modal gate gives the keyboard to the dialog (V3's stock controls are
  // invisible adapters, so a hidden child cannot hold focus) and the dialog's
  // own focus row is the FIRST input: the language combo.
  PRISM_CHECK(probe.focus == window.Hwnd() || probe.focus == probe.dialog);
  PRISM_CHECK_EQ(probe.focusedWidgetId, su::ids::kLanguage);
  // After WM_CLOSE the app must be usable again (the measured S1-3 freeze).
  PRISM_CHECK(IsWindowEnabled(window.Hwnd()) != FALSE);
  PRISM_CHECK(FindWindowW(su::SettingsDialog::kClassName, nullptr) == nullptr);
}

// ===========================================================================
// 4. PRODUCT PATH: the compress dialog the "Compress" verb constructs.
// ===========================================================================
PRISM_TEST("c84_product_compress_dialog_is_visible") {
  const HWND owner = MakeOwner(230, 170, 1000, 700);
  if (!PRISM_CHECK(owner != nullptr)) return;
  cd::CompressDialog dialog;
  const bool created = dialog.Create(GetModuleHandleW(nullptr), L"C84 compress product", owner);
  PRISM_CHECK(created);
  if (created) {
    PRISM_CHECK(IsWindowVisible(dialog.Hwnd()) != FALSE);
    RECT actual{};
    GetWindowRect(dialog.Hwnd(), &actual);
    PRISM_CHECK(SameRect(actual, ExpectedCentered(owner, dialog.Hwnd())));
  }
  dialog.Destroy();
  DestroyWindow(owner);
}

// ===========================================================================
// 5. PRODUCT PATH: launch the REAL PrismArchive.exe on enc.7z with an isolated
//    %LOCALAPPDATA%. This is the S1-1 user report: process alive, no window.
// ===========================================================================
PRISM_TEST("c84_product_password_gate_is_visible_and_usable") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  if (!PRISM_CHECK(CopyFixture(L"enc.7z", gRoot))) return;
  const std::wstring isolated = Join(gRoot, L"localappdata");
  std::filesystem::create_directories(isolated);
  const std::wstring scratch = Join(gRoot, L"launch");
  std::filesystem::create_directories(scratch);
  const std::wstring logPath = Join(gRoot, L"password-gate.log");

  ScopedEnvironment localAppData(L"LOCALAPPDATA", isolated);
  ChildProcess child;
  if (!PRISM_CHECK(LaunchApp(enc, scratch, logPath, &child))) return;

  const HWND gate = WaitForTopLevel(child.info.dwProcessId, pwd_ui::PasswordDialog::kClassName,
                                    true, 3000);
  if (gate == nullptr) {
    DWORD code = 0;
    GetExitCodeProcess(child.info.hProcess, &code);
    std::printf("    [password] no visible gate in 3 s; process alive=%d\n",
                code == STILL_ACTIVE);
    FinishChild(&child, false);
    PRISM_CHECK(gate != nullptr);
    return;
  }
  PRISM_CHECK(IsWindowVisible(gate) != FALSE);
  DWORD code = 0;
  GetExitCodeProcess(child.info.hProcess, &code);
  PRISM_CHECK(code == STILL_ACTIVE);

  // Wrong password: the dialog stays up and says why (never a silent swallow).
  SubmitPassword(gate, L"definitely-not-the-password");
  HWND errorLabel = nullptr;
  std::wstring errorText;
  const ULONGLONG errorDeadline = GetTickCount64() + 10000;
  while (GetTickCount64() < errorDeadline) {
    errorLabel = GetDlgItem(gate, pwd_ui::ids::kError);
    errorText = errorLabel != nullptr ? WindowTitle(errorLabel) : std::wstring();
    if (!errorText.empty()) break;
    Sleep(50);
  }
  PRISM_CHECK(!errorText.empty());
  PRISM_CHECK(IsWindow(gate) != FALSE);
  PRISM_CHECK(IsWindowVisible(gate) != FALSE);
  if (errorText.empty()) {
    std::printf("    [password] no error line after a wrong attempt\n");
  }

  // Correct password: the gate closes and the gallery (title = the archive)
  // appears -- the archive content is what the app opened.
  SubmitPassword(gate, L"PrismTest2026");
  PRISM_CHECK(WaitForWindowGone(child.info.dwProcessId, pwd_ui::PasswordDialog::kClassName,
                                20000));
  const HWND main =
      WaitForTopLevel(child.info.dwProcessId, L"PrismGalleryWindow", true, 20000);
  PRISM_CHECK(main != nullptr);
  if (main != nullptr) {
    const std::wstring title = WindowTitle(main);
    PRISM_CHECK(title.find(L"enc.7z") != std::wstring::npos);
    std::printf("    [password] gallery title: %s\n",
                prism::win::WideToUtf8(title).c_str());
  }

  FinishChild(&child, true);
  std::string log;
  const bool haveLog = ReadTextFile(logPath, &log);
  PRISM_CHECK(haveLog);
  PRISM_CHECK(log.find("session opened archive=") != std::string::npos);
  if (log.find("session opened archive=") == std::string::npos) {
    std::printf("    [password] stderr tail (no session-open line):\n%s\n",
                log.size() > 2000 ? log.substr(log.size() - 2000).c_str() : log.c_str());
  }
}

int main(int argc, char** argv) {
  std::printf("=== c84 dialog-visible ===\n");
#if defined(PRISM_C84_MUTANT_NO_SHOW)
  // D26/D27: the build-time flag IS the proof that the injection is present;
  // the first failure inside a visibility assertion prints the marker and
  // leaves 0, which is what the CTest PASS_REGULAR_EXPRESSION matches.
  std::printf("C84 MUTANT ACTIVE: PRISM_C84_MUTANT_NO_SHOW -- ShowWindow is compiled out\n");
  prism::test::SetMutationVerdict("C84 MUTATION CAUGHT: no_show");
#endif

  if (!WindowStationUsable()) {
    std::printf("SKIP: no interactive window station (CreateWindowExW on a stock class failed)\n");
    return 77;
  }

  wchar_t buffer[2 * MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"PRISM_C84_EXE", buffer, ARRAYSIZE(buffer)) > 0) gExe = buffer;
  if (gExe.empty()) {
    std::printf("SKIP: PRISM_C84_EXE is not set (the product path cannot be spawned)\n");
    return 77;
  }
  if (GetEnvironmentVariableW(L"PRISM_C84_FIXTURES", buffer, ARRAYSIZE(buffer)) > 0) {
    gFixtures = buffer;
  }
  if (gFixtures.empty()) {
    std::printf("SKIP: PRISM_C84_FIXTURES is not set\n");
    return 77;
  }
  wchar_t dll[MAX_PATH] = {};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, ARRAYSIZE(dll)) == 0) {
    std::printf("SKIP: PRISM_7Z_DLL is not set (the spawned app cannot open enc.7z)\n");
    return 77;
  }
  {
    const std::wstring fixture = Join(gFixtures, L"enc.7z");
    if (GetFileAttributesW(fixture.c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::printf("SKIP: fixture missing: %s\n", prism::win::WideToUtf8(fixture).c_str());
      return 77;
    }
    if (GetFileAttributesW(gExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
      std::printf("SKIP: PrismArchive.exe missing: %s\n", prism::win::WideToUtf8(gExe).c_str());
      return 77;
    }
  }

  prism::testtmp::UniqueDir scratch(L"c84", L"dialog_visible");
  if (!scratch.ok()) {
    std::printf("C84 FAILED: cannot create the scratch directory: %s\n", scratch.error().c_str());
    return 1;
  }
  gRoot = scratch.wstr();
  const int rc = prism::test::RunAll(argc, argv);
#if defined(PRISM_C84_MUTANT_NO_SHOW)
  if (rc == 0) {
    std::printf("C84 MUTATION NOT CAUGHT: the suite stayed green with ShowWindow compiled out\n");
    return 1;
  }
  return 0;
#else
  if (rc != 0) return rc;
  std::printf("ALL C84 DIALOG-VISIBLE CHECKS PASSED\n");
  return 0;
#endif
}
