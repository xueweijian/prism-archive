// PrismArchive -- the password dialog driven through REAL HWNDs (card C35).
//
// The rules live in Src/PrismCore/PasswordUiModel.hpp and are proved headless by
// Tests/test_password_ui_model.cpp. This file proves the WIRING:
//   * the dialog is built from code (no .rc): every control is a real HWND;
//   * the password edit really is a password field (EM_GETPASSWORDCHAR) and the
//     Show-password toggle flips it;
//   * the typed plaintext is scrubbed from the edit on submit, and NO control
//     text or window title ever contains the plaintext or a >= 4-char fragment
//     of it (read back with GetWindowTextW, CONTRACTS D2);
//   * the vault list displays masked labels only and picking one submits the
//     secret without putting it in any control;
//   * the retry budget is visible in the error line and locks the edit at 0;
//   * the progress bar is wired to C5: ProgressHandler() is passed to
//     prism::pwd::Probe, worker threads only PostMessage, and the UI thread
//     receives "matching 4/12".
//
// SKIP policy (CONTRACTS D1): a window station that refuses CreateWindowExW is a
// printed SKIP with exit 77; the suite registers SKIP_RETURN_CODE 77 AND
// PASS_REGULAR_EXPRESSION, so a silent skip can never count as a pass.
//
// Negative controls (CONTRACTS D3, wired in Tests/suites/c35-pwd.cmake):
//   PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT    the model's mask returns plaintext.
//   PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES the model accepts a fourth attempt.
// main() arms PASSWORD-UI MUTANT CAUGHT BEFORE the first assertion (D27);
// mini_test prints it at the first failed check and leaves with 0, so the
// dialog teardown cannot swallow the verdict. A dead injection never fails and
// the marker never appears -- the suite still uses the marker as a conjunction
// (D26: no WILL_FAIL stacked with PASS_REGULAR_EXPRESSION).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

#include <commctrl.h>
#endif

#include "../mini_test.hpp"
#include "../../Src/PrismCore/OpenFlow.hpp"
#include "../../Src/PrismWin/PasswordDialog.hpp"

namespace {

using prism::pwd::Candidate;
using prism::pwd::PasswordUiConfig;
using prism::pwd::RememberChoice;
using prism::pwd::Source;
using prism::pwd::UiState;
namespace of = prism::openflow;
namespace ui = prism::win::pwd_ui;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Bullets(size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) out += "\xE2\x80\xA2";
  return out;
}

// Content-level leak check (CONTRACTS D2/D5). The last two characters are the
// documented mask preview, so only the full secret and >= 4-char fragments count.
bool Leaks(const std::string& haystack, const std::string& secret) {
  if (secret.empty()) return false;
  if (Contains(haystack, secret)) return true;
  if (secret.size() < 4) return false;
  for (size_t len = 4; len <= secret.size(); ++len) {
    for (size_t i = 0; i + len <= secret.size(); ++i) {
      if (Contains(haystack, secret.substr(i, len))) return true;
    }
  }
  return false;
}

Candidate C(const std::string& text, Source source) {
  Candidate candidate;
  candidate.text = text;
  candidate.source = source;
  return candidate;
}

std::vector<Candidate> Ring12() {
  std::vector<Candidate> ring;
  for (int i = 1; i <= 12; ++i) {
    const std::string n = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
    ring.push_back(C("Cand" + n + "#zz", Source::VaultHistory));
  }
  return ring;
}

#if defined(_WIN32)

std::string gSkipReason;

std::wstring WindowTextOf(HWND hwnd) {
  if (hwnd == nullptr) return std::wstring();
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<size_t>(length));
  return text;
}

std::wstring ControlTextOf(HWND control) { return WindowTextOf(control); }

struct LeakScan {
  const std::string* secret = nullptr;
  int exceptId = 0;
  bool leaked = false;
};

BOOL CALLBACK LeakScanProc(HWND child, LPARAM param) {
  auto* scan = reinterpret_cast<LeakScan*>(param);
  if (scan->exceptId != 0 && GetDlgCtrlID(child) == scan->exceptId) return TRUE;
  const std::string text = ui::Utf8FromWide(ControlTextOf(child));
  if (Leaks(text, *scan->secret)) {
    wchar_t klass[64] = L"?";
    GetClassNameW(child, klass, 64);
    std::printf("    [leak] ctrl id=%d class=%ls text=%s\n", GetDlgCtrlID(child),
                klass, text.c_str());
    scan->leaked = true;
    return FALSE;
  }
  return TRUE;
}

// Reads every child control back with GetWindowTextW, plus the window title.
// exceptId lets the one legitimate holder (the password edit while the user is
// typing) be excluded; after submit the edit is empty and exceptId is 0, so the
// whole window is checked.
bool LeaksInWindow(HWND hwnd, const std::string& secret, int exceptId) {
  if (hwnd == nullptr) return false;
  if (Leaks(ui::Utf8FromWide(WindowTextOf(hwnd)), secret)) return true;
  LeakScan scan;
  scan.secret = &secret;
  scan.exceptId = exceptId;
  EnumChildWindows(hwnd, LeakScanProc, reinterpret_cast<LPARAM>(&scan));
  return scan.leaked;
}

bool WindowStationUsable() {
  static const bool usable = [] {
    const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10,
                                       10, nullptr, nullptr, GetModuleHandleW(nullptr),
                                       nullptr);
    if (probe == nullptr) return false;
    DestroyWindow(probe);
    return true;
  }();
  return usable;
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

void ClickButton(ui::PasswordDialog& dialog, int id) {
  SendMessageW(dialog.Control(id), BM_CLICK, 0, 0);
}

void TypeInto(ui::PasswordDialog& dialog, const std::wstring& text) {
  const HWND edit = dialog.Control(ui::ids::kPassword);
  SetWindowTextW(edit, text.c_str());
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kPassword, EN_CHANGE),
                  reinterpret_cast<LPARAM>(edit));
}

std::wstring ListItem(HWND list, int index) {
  const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
  if (length == LB_ERR) return std::wstring();
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index),
               reinterpret_cast<LPARAM>(text.data()));
  text.resize(static_cast<size_t>(length));
  return text;
}

int ListCount(HWND list) {
  const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
  return count == LB_ERR ? 0 : static_cast<int>(count);
}

int ProgressPosition(HWND progress) {
  return static_cast<int>(SendMessageW(progress, PBM_GETPOS, 0, 0));
}

bool OpenDialog(ui::PasswordDialog& dialog, std::vector<Candidate> candidates,
                std::vector<std::string> vault = {}, int budget = 3,
                bool canRemember = true) {
  if (!gSkipReason.empty()) return false;
  if (!WindowStationUsable()) {
    gSkipReason = "no interactive window station (CreateWindowExW on a stock class failed)";
    return false;
  }
  if (!dialog.Create(GetModuleHandleW(nullptr), L"Password required - fixture.7z")) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    PRISM_CHECK(false);  // our bug, not the environment: this must be RED
    return false;
  }
  PasswordUiConfig config;
  config.archiveName = "fixture.7z";
  config.candidates = std::move(candidates);
  config.vaultEntries = std::move(vault);
  config.manualBudget = budget;
  config.canRemember = canRemember;
  dialog.SetConfig(std::move(config));
  dialog.Start();
  return true;
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// 1. Programmatic construction + the password field contract
// ===========================================================================

PRISM_TEST("win: the dialog is built from code and every control is a real HWND") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;

  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  PRISM_CHECK(GetClassInfoExW(GetModuleHandleW(nullptr), ui::PasswordDialog::kClassName,
                              &info) != FALSE);

  const int controls[] = {ui::ids::kPrompt,      ui::ids::kPassword,
                          ui::ids::kShowPassword, ui::ids::kRemember,
                          ui::ids::kOnce,        ui::ids::kCurrent,
                          ui::ids::kError,       ui::ids::kStatus,
                          ui::ids::kProgress,    ui::ids::kProgressLabel,
                          ui::ids::kCandidates,  ui::ids::kVault,
                          ui::ids::kUseVault,    ui::ids::kOk,
                          ui::ids::kCancel};
  for (int id : controls) {
    if (!PRISM_CHECK(dialog.Control(id) != nullptr)) {
      std::printf("    [dialog] missing control id %d\n", id);
      return;
    }
  }

  const HWND edit = dialog.Control(ui::ids::kPassword);
  PRISM_CHECK((GetWindowLongW(edit, GWL_STYLE) & ES_PASSWORD) != 0);
  PRISM_CHECK(static_cast<WPARAM>(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0)) != 0);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::NeedPassword);
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kPassword));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kOk));  // nothing typed yet
  PRISM_CHECK(!dialog.ControlVisible(ui::ids::kProgress));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kCurrent), std::string("(none)"));
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(ui::ids::kOnce),
                                                BM_GETCHECK, 0, 0)),
                 static_cast<int>(BST_CHECKED));  // privacy-preserving default
  PRISM_CHECK_EQ(static_cast<int>(SendMessageW(dialog.Control(ui::ids::kRemember),
                                                BM_GETCHECK, 0, 0)),
                 static_cast<int>(BST_UNCHECKED));
}

PRISM_TEST("win: show-password flips EM_SETPASSWORDCHAR and nothing else leaks") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;

  const std::string secret = "hunter2SuperSecret";
  const HWND edit = dialog.Control(ui::ids::kPassword);
  TypeInto(dialog, ui::WideFromUtf8(secret));

  // The edit holds what the user typed (that is where typing goes) but its
  // DISPLAY is masked -- the test proves the mask char, not the buffer.
  PRISM_CHECK_EQ(ui::Utf8FromWide(ControlTextOf(edit)), secret);
  PRISM_CHECK(static_cast<WPARAM>(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0)) != 0);
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kOk));
  // Every OTHER control and the title are clean while the user is typing.
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret, ui::ids::kPassword));

  ClickButton(dialog, ui::ids::kShowPassword);
  PRISM_CHECK(dialog.Model().showPassword());
  PRISM_CHECK_EQ(static_cast<WPARAM>(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0)),
                 static_cast<WPARAM>(0));
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret, ui::ids::kPassword));

  ClickButton(dialog, ui::ids::kShowPassword);
  PRISM_CHECK(!dialog.Model().showPassword());
  PRISM_CHECK(static_cast<WPARAM>(SendMessageW(edit, EM_GETPASSWORDCHAR, 0, 0)) != 0);
}

// ===========================================================================
// 2. Submit scrubs the edit and hands the secret to the engine seam
// ===========================================================================

PRISM_TEST("win: OK submits once, clears the edit, and leaves no plaintext behind") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;

  bool called = false;
  std::string captured;
  dialog.SetSubmitHandler([&](const std::string& password) {
    called = true;
    captured = password;
  });

  const std::string secret = "TopSecret42";
  TypeInto(dialog, ui::WideFromUtf8(secret));
  ClickButton(dialog, ui::ids::kOk);

  PRISM_CHECK(called);
  PRISM_CHECK_EQ(captured, secret);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Probing);
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kPassword).empty());
  // Now the edit is empty, so the WHOLE window (including the edit) is checked.
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret, 0));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kCurrent), Bullets(9) + "42");
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kPassword));
  PRISM_CHECK(dialog.ControlVisible(ui::ids::kProgress));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kProgressLabel),
                 std::string("matching 0/1"));
}

PRISM_TEST("win: an empty OK is refused with a reason and hands nothing over") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;
  bool called = false;
  dialog.SetSubmitHandler([&](const std::string&) { called = true; });

  // The real UI contract: OK is disabled while the edit is empty, so a click is
  // a no-op. The model's own "enter a password" refusal is pinned headless.
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kOk));
  ClickButton(dialog, ui::ids::kOk);
  PRISM_CHECK(!called);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::NeedPassword);
}

// ===========================================================================
// 3. Retry budget: error line, lock-out, and the D3 control
// ===========================================================================

PRISM_TEST("win: wrong attempts count down and the fourth OK is refused") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;

  int submissions = 0;
  dialog.SetSubmitHandler([&](const std::string&) { ++submissions; });

  TypeInto(dialog, L"Xy9#Qp2L");
  ClickButton(dialog, ui::ids::kOk);
  PRISM_CHECK_EQ(submissions, 1);
  dialog.FinishProbe(false, false);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Wrong);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kError), "Wrong attempts: 1 / 3"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kError), "2 left"));
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kPassword));
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kCurrent) == std::string("(none)"));
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Xy9#Qp2L", ui::ids::kPassword));

  TypeInto(dialog, L"mN4$Rt8v");
  ClickButton(dialog, ui::ids::kOk);
  dialog.FinishProbe(false, false);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kError), "1 left"));

  TypeInto(dialog, L"kL6!Wq3z");
  ClickButton(dialog, ui::ids::kOk);
  dialog.FinishProbe(false, false);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Exhausted);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kError), "budget exhausted"));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kPassword));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kOk));

  // The mutation accepts this fourth attempt: submissions becomes 4 and the
  // state becomes Probing, so the checks above/below go red and main() prints
  // the CAUGHT marker (D3 + D26).
  TypeInto(dialog, L"neverTried99");
  ClickButton(dialog, ui::ids::kOk);
  PRISM_CHECK_EQ(submissions, 3);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Exhausted);
  PRISM_CHECK_EQ(dialog.Model().attemptsUsed(), 3);
}

// ===========================================================================
// 4. C5 progress wiring: ProgressHandler -> PostMessage -> model -> bar
// ===========================================================================

PRISM_TEST("win: the progress bar follows PostProbeProgress and is monotone") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, Ring12())) return;

  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Probing);
  PRISM_CHECK(dialog.ControlVisible(ui::ids::kProgress));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kProgressLabel),
                 std::string("matching 0/12"));
  PRISM_CHECK_EQ(ProgressPosition(dialog.Control(ui::ids::kProgress)), 0);

  dialog.PostProbeProgress(3, 12);
  PumpMessages();
  PRISM_CHECK_EQ(dialog.Model().view().done, 3);
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kProgressLabel),
                 std::string("matching 3/12"));
  PRISM_CHECK_EQ(ProgressPosition(dialog.Control(ui::ids::kProgress)), 3);

  // A stale worker must not rewind the bar or the label.
  dialog.PostProbeProgress(2, 12);
  PumpMessages();
  PRISM_CHECK_EQ(dialog.Model().view().done, 3);
  PRISM_CHECK_EQ(ProgressPosition(dialog.Control(ui::ids::kProgress)), 3);
}

PRISM_TEST("win: a real pwd::Probe drives the dialog through ProgressHandler") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, Ring12())) return;

  of::FlowConfig flowConfig;
  flowConfig.candidates = Ring12();
  flowConfig.kdf = prism::PasswordKdfClass::ZipCrypto;
  flowConfig.threads = 1;  // deterministic counts
  flowConfig.archiveName = "fixture.7z";
  of::OpenFlow flow(flowConfig);
  flow.Begin();
  // Header-encrypted + no password offered: the C5 truth table says the ring
  // starts probing. We only use flow.plan(); the dialog owns the UI.
  flow.OnOpenResult(of::OpenAttempt{of::kOpenHrAbort, of::kErrFlagsUndefined, false, false});

  bool openedCalled = false;
  std::string openedPassword;
  bool openedRemember = true;
  dialog.SetOpenedHandler([&](const std::string& password, bool remember) {
    openedCalled = true;
    openedPassword = password;
    openedRemember = remember;
  });

  std::atomic<int> calls{0};
  const prism::pwd::ProbeOutcome outcome = prism::pwd::Probe(
      flow.plan(),
      [&](const std::string& password) {
        ++calls;
        return password == "Cand04#zz";
      },
      dialog.ProgressHandler(),
      [&]() { return flow.cancelled(); });
  PumpMessages();

  PRISM_CHECK(outcome.found);
  PRISM_CHECK_EQ(outcome.attemptsMade, 4);
  PRISM_CHECK_EQ(calls.load(), 4);
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kProgressLabel),
                 std::string("matching 4/12"));
  PRISM_CHECK_EQ(ProgressPosition(dialog.Control(ui::ids::kProgress)), 4);

  const HWND window = dialog.Hwnd();
  dialog.FinishProbe(outcome.found, outcome.cancelled, outcome.password, outcome.diagnostics);
  PRISM_CHECK(openedCalled);
  PRISM_CHECK_EQ(openedPassword, std::string("Cand04#zz"));
  PRISM_CHECK(!openedRemember);  // an automatic ring hit never silently persists
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Opened);
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Cand04#zz", 0));  // hwnd is null; title gone
}

// ===========================================================================
// 5. Vault list: masked labels only, pick -> submit, remember semantics
// ===========================================================================

PRISM_TEST("win: the vault list shows masks, never the DPAPI plaintext") {
  ui::PasswordDialog dialog;
  const std::vector<std::string> vault = {"Qz8#Lm2Vt99", "Wp5$Rn7Kx77"};
  if (!OpenDialog(dialog, {}, vault)) return;

  const HWND list = dialog.Control(ui::ids::kVault);
  PRISM_CHECK_EQ(ListCount(list), 2);
  const std::wstring first = ListItem(list, 0);
  const std::wstring second = ListItem(list, 1);
  PRISM_CHECK(!Leaks(ui::Utf8FromWide(first), "Qz8#Lm2Vt99"));
  PRISM_CHECK(!Leaks(ui::Utf8FromWide(second), "Wp5$Rn7Kx77"));
  PRISM_CHECK(ui::Utf8FromWide(first).size() > 2);
  PRISM_CHECK(ui::Utf8FromWide(first).find("99") != std::string::npos);
  PRISM_CHECK(ui::Utf8FromWide(second).find("77") != std::string::npos);

  // The candidate list follows the same rule (empty here, so prove the vault one
  // through the whole-window scan as well).
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Qz8#Lm2Vt99", 0));
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Wp5$Rn7Kx77", 0));
}

PRISM_TEST("win: picking a vault entry submits it without putting it in a control") {
  ui::PasswordDialog dialog;
  const std::vector<std::string> vault = {"Qz8#Lm2Vt99", "Wp5$Rn7Kx77"};
  if (!OpenDialog(dialog, {}, vault)) return;

  bool called = false;
  std::string captured;
  dialog.SetSubmitHandler([&](const std::string& password) {
    called = true;
    captured = password;
  });

  const HWND list = dialog.Control(ui::ids::kVault);
  SendMessageW(list, LB_SETCURSEL, 1, 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kVault, LBN_SELCHANGE),
                  reinterpret_cast<LPARAM>(list));
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kUseVault));

  ClickButton(dialog, ui::ids::kUseVault);
  PRISM_CHECK(called);
  PRISM_CHECK_EQ(captured, std::string("Wp5$Rn7Kx77"));
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Probing);
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kPassword).empty());
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Wp5$Rn7Kx77", 0));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kCurrent), Bullets(9) + "77");
}

PRISM_TEST("win: a remembered vault pick is reported once and then closed") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {}, {"Qz8#Lm2Vt99"})) return;

  bool openedCalled = false;
  std::string openedPassword;
  bool openedRemember = false;
  dialog.SetOpenedHandler([&](const std::string& password, bool remember) {
    openedCalled = true;
    openedPassword = password;
    openedRemember = remember;
  });

  ClickButton(dialog, ui::ids::kRemember);
  PRISM_CHECK(dialog.Model().rememberChoice() == RememberChoice::Remember);

  const HWND list = dialog.Control(ui::ids::kVault);
  SendMessageW(list, LB_SETCURSEL, 0, 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kVault, LBN_SELCHANGE),
                  reinterpret_cast<LPARAM>(list));
  ClickButton(dialog, ui::ids::kUseVault);
  const HWND window = dialog.Hwnd();
  dialog.FinishProbe(true, false);

  PRISM_CHECK(openedCalled);
  PRISM_CHECK(openedRemember);
  PRISM_CHECK_EQ(openedPassword, std::string("Qz8#Lm2Vt99"));
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK_EQ(dialog.Model().TakeVaultPassword(), std::string("Qz8#Lm2Vt99"));
  PRISM_CHECK(dialog.Model().TakeVaultPassword().empty());
}

// ===========================================================================
// 6. Cancel / close
// ===========================================================================

PRISM_TEST("win: Cancel cancels the model, fires once, and destroys the window") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;

  int cancels = 0;
  int submissions = 0;
  dialog.SetCancelHandler([&]() { ++cancels; });
  dialog.SetSubmitHandler([&](const std::string&) { ++submissions; });

  const std::string secret = "Zc7#Kp2L";
  TypeInto(dialog, ui::WideFromUtf8(secret));
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret, ui::ids::kPassword));

  const HWND window = dialog.Hwnd();
  ClickButton(dialog, ui::ids::kCancel);
  PRISM_CHECK_EQ(cancels, 1);
  PRISM_CHECK_EQ(submissions, 0);
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Cancelled);
}

PRISM_TEST("win: WM_CLOSE is a cancel too, not a hidden window") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;
  int cancels = 0;
  dialog.SetCancelHandler([&]() { ++cancels; });
  const HWND window = dialog.Hwnd();
  dialog.Dispatch(WM_CLOSE, 0, 0);
  PRISM_CHECK_EQ(cancels, 1);
  PRISM_CHECK_EQ(IsWindow(window), FALSE);
  PRISM_CHECK_EQ(dialog.Model().state(), UiState::Cancelled);
}

// ===========================================================================
// 7. The candidate list is masked too
// ===========================================================================

PRISM_TEST("win: the candidate ring list shows source labels and masks") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, Ring12())) return;

  const HWND list = dialog.Control(ui::ids::kCandidates);
  PRISM_CHECK_EQ(ListCount(list), 12);
  for (int i = 0; i < 12; ++i) {
    const std::string item = ui::Utf8FromWide(ListItem(list, i));
    PRISM_CHECK(!Leaks(item, "Cand04#zz"));
    PRISM_CHECK(item.find("#zz") == std::string::npos);
  }
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Cand01#zz", 0));
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "Cand04#zz", 0));
}

// ===========================================================================
// 8. Show-password is not a leak when combined with a submit
// ===========================================================================

PRISM_TEST("win: revealed -> submitted leaves the whole window clean") {
  ui::PasswordDialog dialog;
  if (!OpenDialog(dialog, {})) return;
  const std::string secret = "RevealThenSubmit88";
  TypeInto(dialog, ui::WideFromUtf8(secret));
  ClickButton(dialog, ui::ids::kShowPassword);
  PRISM_CHECK_EQ(static_cast<WPARAM>(SendMessageW(dialog.Control(ui::ids::kPassword),
                                                  EM_GETPASSWORDCHAR, 0, 0)),
                 static_cast<WPARAM>(0));
  ClickButton(dialog, ui::ids::kOk);
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kPassword).empty());
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret, 0));
}

#endif  // _WIN32

int main(int argc, char** argv) {
#if defined(_WIN32) && \
    (defined(PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT) || \
     defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES))
  // CONTRACTS D3 + D26 + D27: the marker is a CONJUNCTION -- the run went red
  // AND the deliberate defect fired. It is armed BEFORE the first assertion:
  // ReportFailure prints it at the first failed check and leaves with 0, so the
  // dialog teardown cannot swallow it (the failure mode this card hit on
  // 2026-11-02). A dead mutation never fails and the marker never appears, so
  // PASS_REGULAR_EXPRESSION misses and the mutation test is RED.
  prism::test::SetMutationVerdict("PASSWORD-UI MUTANT CAUGHT");
#endif
  const int failures = prism::test::RunAll(argc, argv);

#if defined(_WIN32)
#if defined(PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT) || \
    defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES)
  // Reaching here means no failed check went through ReportFailure, so no
  // verdict was printed. Stay RED (exit 1) instead of printing a late marker:
  // a run-then-print verdict is exactly what D27 forbids.
  std::printf("mutant build: no first-failure verdict (checks=%d failures=%d mutation hits=%d)\n",
              prism::test::CheckCount(), failures, prism::pwd::UiMutationCount());
  return 1;
#else
  if (!gSkipReason.empty()) {
    // CONTRACTS D1: a skip is PRINTED and it is never exit 0.
    std::printf("SKIP: %s\n", gSkipReason.c_str());
    return 77;  // SKIP_RETURN_CODE 77 (Tests/suites/c35-pwd.cmake)
  }
  if (failures != 0) return 1;
  std::puts("ALL PASSWORD-UI CHECKS PASSED");
  return 0;
#endif
#else
  return failures;
#endif
}
