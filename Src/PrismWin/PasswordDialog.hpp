// PrismArchive -- PrismWin
// PasswordDialog: the Win32 shell of the password ring UI (card C35).
//
// WHY NO .rc RESOURCE: like C18's CompressDialog, the dialog is created at
// runtime with CreateWindowExW and child controls. A dialog template would move
// the control set into a format the Linux lane cannot read and would force a
// change in Src/PrismWin/CMakeLists.txt (card C3 owns it). Programmatic creation
// also means every test assertion targets a real HWND, not a template string.
//
// TWO HALVES, ONE CARD:
//   * Src/PrismCore/PasswordUiModel.hpp owns every rule (states, budget,
//     masking, remember semantics). It is pure and tested on every lane.
//   * This file owns the HWND and the message plumbing. It contains NO rules: a
//     handler that decided what is legal would belong in the model.
//
// SECRECY WIRING (the reason the HWND test reads every control back):
//   * the password edit is created with ES_PASSWORD and re-armed with
//     EM_SETPASSWORDCHAR on every sync; the Show-password toggle sets it to 0
//     and back. The test proves EM_GETPASSWORDCHAR, not a style flag.
//   * the typed plaintext is read ONCE on OK, handed to the submit callback,
//     then the edit is cleared before any further work. Nothing else in the
//     dialog ever receives it: the "current" label, the status line, the
//     candidate list and the vault list all display model masks only (last two
//     code points, or bullets for <= 2 characters).
//   * the vault list is built from VaultItemView.label -- the DPAPI plaintext is
//     never put into a listbox item.
//   * the window title comes from the caller and is never a secret.
//
// OPENFLOW PROGRESS (C5): ProgressHandler() returns the callable to pass as the
// progress argument of pwd::Probe / openflow::OpenFlow. It only PostMessage()s,
// so a worker thread never touches a control (the same discipline CONTRACTS B9
// enforces on the app's worker threads). The UI thread receives
// WM_APP + 0x35 and forwards the counts into the model, which keeps the label
// monotone and renders "matching 3/12".
//
// The dialog does not run the probe itself: the app's wiring patch owns the
// engine call (it needs the archive reader). This header exposes the seam --
// Submit callback, FinishProbe, ProgressHandler -- so the wiring is three lines.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
// File scope, never inside the namespace: including windows.h inside a namespace
// wraps every SDK declaration into it (the C18 lesson).
#include <windows.h>

#include <commctrl.h>
#endif

#include "../PrismCore/PasswordUiModel.hpp"

#if defined(_WIN32)

namespace prism::win::pwd_ui {

// pwd_ui is a sibling of prism::pwd, not a child, so unqualified lookup does not
// reach the model's names. These aliases keep the dialog readable without
// sprinkling prism::pwd:: over every signature.
using prism::pwd::PasswordUiConfig;
using prism::pwd::PasswordUiModel;
using prism::pwd::RememberChoice;
using prism::pwd::UiState;

// Local transcoders. Kept in this namespace because prism::win already has
// several identically named inline helpers in unrelated headers; importing two
// of them into one TU is an ODR hazard this dialog does not need.
inline std::wstring WideFromUtf8(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                      wide.data(), needed);
  return wide;
}

inline std::string Utf8FromWide(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0,
                                         nullptr, nullptr);
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                      utf8.data(), needed, nullptr, nullptr);
  return utf8;
}

// Control ids live here (not in the model) so the HWND test can name the same
// control the dialog created. kOk/kCancel keep IDOK/IDCANCEL for a C25 harness.
namespace ids {
constexpr int kPrompt = 3101;
constexpr int kPassword = 3102;
constexpr int kShowPassword = 3103;
constexpr int kRemember = 3104;
constexpr int kOnce = 3105;
constexpr int kCurrent = 3106;
constexpr int kError = 3107;
constexpr int kStatus = 3108;
constexpr int kProgress = 3109;
constexpr int kProgressLabel = 3110;
constexpr int kCandidates = 3111;
constexpr int kVault = 3112;
constexpr int kUseVault = 3113;
constexpr int kOk = 1;      // == IDOK
constexpr int kCancel = 2;  // == IDCANCEL
}  // namespace ids

class PasswordDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchivePasswordDialog";
  static constexpr int kClientWidth = 560;
  static constexpr int kClientHeight = 470;

  // The probe handoff. The app opens the archive with this password and then
  // reports back through FinishProbe(); the dialog never blocks on the engine.
  using SubmitHandler = std::function<void(const std::string& password)>;
  using OpenedHandler = std::function<void(const std::string& password, bool remember)>;
  using CancelHandler = std::function<void()>;

  PasswordDialog() = default;
  ~PasswordDialog() { Destroy(); }
  PasswordDialog(const PasswordDialog&) = delete;
  PasswordDialog& operator=(const PasswordDialog&) = delete;

  DWORD lastError() const { return lastError_; }
  int missingControlId() const { return missingControlId_; }

  bool Create(HINSTANCE instance, const std::wstring& title = L"Password required") {
    instance_ = instance;
    EnsureCommonControls();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &PasswordDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
      lastError_ = GetLastError();
      if (lastError_ != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    RECT frame{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED, FALSE,
                       WS_EX_DLGMODALFRAME);
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title.c_str(),
                            WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                            frame.bottom - frame.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) lastError_ = GetLastError();
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }

  PasswordUiModel& Model() { return model_; }
  const PasswordUiModel& Model() const { return model_; }

  void SetSubmitHandler(SubmitHandler handler) { onSubmit_ = std::move(handler); }
  void SetOpenedHandler(OpenedHandler handler) { onOpened_ = std::move(handler); }
  void SetCancelHandler(CancelHandler handler) { onCancel_ = std::move(handler); }

  // Re-arms for an archive. Safe before or after Create(); the list boxes are
  // repopulated from the model's masked labels only.
  void SetConfig(PasswordUiConfig config) {
    model_.Reset(std::move(config));
    if (hwnd_ != nullptr) {
      syncing_ = true;
      SetWindowTextW(passwordEdit_, L"");
      syncing_ = false;
      PopulateLists();
      SyncControls();
    }
  }

  void Start() {
    model_.Start();
    SyncControls();
  }

  // The message entry point. WndProc calls it; the Windows test calls it too, so
  // a case can drive the REAL handler chain without a message pump (the pattern
  // Tests/win/test_navigation.cpp established).
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  std::wstring ControlText(int id) const { return ControlTextOf(Control(id)); }
  std::string ControlTextUtf8(int id) const { return Utf8FromWide(ControlText(id)); }

  bool ControlEnabled(int id) const {
    const HWND control = Control(id);
    return control != nullptr && IsWindowEnabled(control) != FALSE;
  }

  // IsWindowVisible() is false for every child while the dialog itself is not
  // shown (the test creates it hidden on purpose), so the WS_VISIBLE style bit
  // is the honest question "would it be visible once the parent is shown?".
  bool ControlVisible(int id) const {
    const HWND control = Control(id);
    return control != nullptr &&
           (GetWindowLongW(control, GWL_STYLE) & static_cast<LONG>(WS_VISIBLE)) != 0;
  }

  // ---- C5 OpenFlow / pwd::Probe seam -------------------------------------
  // Pass this as the probe's progress callable. It only posts: a worker thread
  // must never call SetWindowTextW (CONTRACTS B9 discipline). The dialog must
  // outlive the probe; the wiring patch owns that lifetime.
  std::function<void(int, int)> ProgressHandler() {
    return [this](int done, int total) { PostProbeProgress(done, total); };
  }

  void PostProbeProgress(int done, int total) {
    if (hwnd_ == nullptr) return;
    PostMessageW(hwnd_, kProgressMessage, static_cast<WPARAM>(done),
                 static_cast<LPARAM>(total));
  }

  // Called on the UI thread for a progress message (also public so a test can
  // skip the pump when it is testing the model sync rather than the post).
  void OnProbeProgress(int done, int total) {
    model_.OnProgress(done, total);
    SyncControls();
  }

  // The probe result. On Opened the dialog hands the password to the opened
  // callback (the archive is open; the dialog's job is done) and closes. On a
  // cancel it closes. A wrong manual attempt re-enables the edit and shows the
  // remaining budget instead of closing.
  void FinishProbe(bool found, bool cancelled, const std::string& password = {},
                   const std::string& diagnostics = {}) {
    model_.OnProbeFinished(found, cancelled, password, diagnostics);
    const prism::pwd::UiView& view = model_.view();
    if (view.state == prism::pwd::UiState::Opened) {
      const bool remember = model_.ShouldPersist();
      if (onOpened_) onOpened_(model_.openedPassword(), remember);
      if (hwnd_ != nullptr) {
        SyncControls();
        Destroy();
      }
      return;
    }
    if (view.state == prism::pwd::UiState::Cancelled) {
      if (hwnd_ != nullptr) {
        if (onCancel_) onCancel_();
        SyncControls();
        Destroy();
      }
      return;
    }
    SyncControls();
  }

  void SyncControls() {
    if (hwnd_ == nullptr) return;
    const prism::pwd::UiView& view = model_.view();
    syncing_ = true;

    SendMessageW(showPasswordCheck_, BM_SETCHECK,
                 view.showPassword ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(rememberRadio_, BM_SETCHECK,
                 view.remember == prism::pwd::RememberChoice::Remember ? BST_CHECKED
                                                                      : BST_UNCHECKED,
                 0);
    SendMessageW(onceRadio_, BM_SETCHECK,
                 view.remember == prism::pwd::RememberChoice::Once ? BST_CHECKED
                                                                   : BST_UNCHECKED,
                 0);

    SendMessageW(progressBar_, PBM_SETRANGE32, 0,
                 view.total > 0 ? static_cast<LPARAM>(view.total) : 1);
    SendMessageW(progressBar_, PBM_SETPOS, static_cast<WPARAM>(view.done), 0);
    SetTextIfDifferent(progressLabel_, WideFromUtf8(view.progressLabel));
    SetTextIfDifferent(currentLabel_,
                       WideFromUtf8(view.masked.empty() ? std::string("(none)") : view.masked));
    SetTextIfDifferent(errorLabel_, WideFromUtf8(ErrorLine(view)));
    SetTextIfDifferent(statusLabel_, WideFromUtf8(view.message));

    ShowWindow(progressBar_, view.showProgress ? SW_SHOW : SW_HIDE);
    ShowWindow(progressLabel_, view.showProgress ? SW_SHOW : SW_HIDE);

    const BOOL editable = view.canSubmit ? TRUE : FALSE;
    EnableWindow(passwordEdit_, editable);
    EnableWindow(showPasswordCheck_, editable);
    EnableWindow(rememberRadio_, editable);
    EnableWindow(onceRadio_, editable);
    EnableWindow(vaultList_, editable);

    const bool hasVaultSelection =
        SendMessageW(vaultList_, LB_GETCURSEL, 0, 0) != static_cast<LRESULT>(LB_ERR);
    EnableWindow(useVaultButton_, (editable && hasVaultSelection) ? TRUE : FALSE);
    EnableWindow(okButton_, (view.canSubmit && !ControlTextOf(passwordEdit_).empty()) ? TRUE
                                                                                      : FALSE);
    EnableWindow(cancelButton_, TRUE);

    // Show-password: 0 reveals, the black circle masks. Always re-armed so a
    // model reset cannot leave a plaintext edit behind.
    SendMessageW(passwordEdit_, EM_SETPASSWORDCHAR,
                 view.showPassword ? 0 : kPasswordChar, 0);
    InvalidateRect(passwordEdit_, nullptr, TRUE);
    syncing_ = false;
  }

 private:
  static constexpr WPARAM kPasswordChar = 0x25CF;  // black circle
  static constexpr UINT kProgressMessage = WM_APP + 0x35;

  static void EnsureCommonControls() {
    // The progress bar is a common control; without ICC_PROGRESS_CLASS the
    // CreateWindowExW for it fails and the dialog silently loses its bar. Like
    // C18, the return value is not the gate (a v5 comctl32 can return FALSE with
    // GetLastError()==0); the control creation is.
    static const bool once = [] {
      INITCOMMONCONTROLSEX settings{};
      settings.dwSize = sizeof(settings);
      settings.dwICC = ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES;
      InitCommonControlsEx(&settings);
      return true;
    }();
    static_cast<void>(once);
  }

  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<PasswordDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<PasswordDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // CreateChildren runs inside CreateWindowExW, before the caller can assign
      // the returned handle; without this line the children get a null parent.
      self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        return CreateChildren() ? 0 : -1;
      case WM_COMMAND:
        if (!syncing_) OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
        return 0;
      case kProgressMessage:
        OnProbeProgress(static_cast<int>(wparam), static_cast<int>(lparam));
        return 0;
      case WM_CLOSE:
        CloseAsCancelled();
        return 0;
      case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }

  HWND MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, int x,
                   int y, int width, int height) {
    const HWND control = CreateWindowExW(
        0, klass, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (control == nullptr) {
      missingControlId_ = id;
      lastError_ = GetLastError();
      return nullptr;
    }
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
  }

  HWND MakeLabel(const wchar_t* text, int x, int y, int width, int height = 20) {
    return MakeControl(L"STATIC", text, SS_LEFT, 0, x, y, width, height);
  }

  bool CreateChildren() {
    promptLabel_ = MakeControl(L"STATIC", L"This archive needs a password.", SS_LEFT,
                               ids::kPrompt, 16, 14, kClientWidth - 32, 22);
    MakeLabel(L"Password", 16, 56, 100);
    passwordEdit_ = MakeControl(L"EDIT", L"",
                                ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                ids::kPassword, 120, 52, 300, 24);
    showPasswordCheck_ = MakeControl(L"BUTTON", L"Show password",
                                     BS_AUTOCHECKBOX | WS_TABSTOP, ids::kShowPassword,
                                     430, 52, 114, 22);
    MakeLabel(L"Current", 16, 84, 100);
    currentLabel_ = MakeControl(L"STATIC", L"(none)", SS_LEFT, ids::kCurrent, 120, 84,
                                300, 20);

    rememberRadio_ = MakeControl(L"BUTTON", L"Remember in vault",
                                 BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, ids::kRemember,
                                 120, 112, 190, 22);
    onceRadio_ = MakeControl(L"BUTTON", L"This time only",
                             BS_AUTORADIOBUTTON | WS_TABSTOP, ids::kOnce, 320, 112, 180, 22);
    SendMessageW(onceRadio_, BM_SETCHECK, BST_CHECKED, 0);

    errorLabel_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kError, 16, 142,
                              kClientWidth - 32, 20);
    statusLabel_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kStatus, 16, 166,
                               kClientWidth - 32, 38);
    progressBar_ = MakeControl(L"msctls_progress32", L"", PBS_SMOOTH, ids::kProgress, 16,
                               210, 400, 18);
    progressLabel_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kProgressLabel, 424, 210,
                                 120, 20);

    MakeLabel(L"Ring (masked)", 16, 240, 240);
    candidatesList_ = MakeControl(L"LISTBOX", L"",
                                  LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER,
                                  ids::kCandidates, 16, 262, 250, 100);
    MakeLabel(L"Stored passwords (masked)", 294, 240, 240);
    vaultList_ = MakeControl(L"LISTBOX", L"",
                             LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL | WS_BORDER,
                             ids::kVault, 294, 262, 250, 100);
    useVaultButton_ = MakeControl(L"BUTTON", L"Use selected",
                                  BS_PUSHBUTTON | WS_TABSTOP, ids::kUseVault, 294, 368,
                                  120, 26);
    okButton_ = MakeControl(L"BUTTON", L"Open", BS_DEFPUSHBUTTON | WS_TABSTOP, ids::kOk,
                            kClientWidth - 200, kClientHeight - 42, 84, 28);
    cancelButton_ = MakeControl(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP,
                                ids::kCancel, kClientWidth - 104, kClientHeight - 42, 84,
                                28);

    if (promptLabel_ == nullptr || passwordEdit_ == nullptr ||
        showPasswordCheck_ == nullptr || currentLabel_ == nullptr ||
        rememberRadio_ == nullptr || onceRadio_ == nullptr || errorLabel_ == nullptr ||
        statusLabel_ == nullptr || progressBar_ == nullptr || progressLabel_ == nullptr ||
        candidatesList_ == nullptr || vaultList_ == nullptr || useVaultButton_ == nullptr ||
        okButton_ == nullptr || cancelButton_ == nullptr) {
      return false;
    }

    PopulateLists();
    SyncControls();
    return true;
  }

  void PopulateLists() {
    if (hwnd_ == nullptr) return;
    SendMessageW(candidatesList_, LB_RESETCONTENT, 0, 0);
    for (const prism::pwd::CandidateView& candidate : model_.Candidates()) {
      const std::wstring label = WideFromUtf8(candidate.label);
      SendMessageW(candidatesList_, LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(vaultList_, LB_RESETCONTENT, 0, 0);
    for (const prism::pwd::VaultItemView& item : model_.VaultItems()) {
      const std::wstring label = WideFromUtf8(item.label);
      SendMessageW(vaultList_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    EnableWindow(useVaultButton_, FALSE);
  }

  void OnCommand(int id, int code, HWND control) {
    if (control == nullptr) return;
    switch (id) {
      case ids::kShowPassword:
        if (code != BN_CLICKED) return;
        model_.SetShowPassword(SendMessageW(showPasswordCheck_, BM_GETCHECK, 0, 0) ==
                               BST_CHECKED);
        SyncControls();
        return;
      case ids::kRemember:
        if (code != BN_CLICKED) return;
        model_.SetRememberChoice(prism::pwd::RememberChoice::Remember);
        SyncControls();
        return;
      case ids::kOnce:
        if (code != BN_CLICKED) return;
        model_.SetRememberChoice(prism::pwd::RememberChoice::Once);
        SyncControls();
        return;
      case ids::kPassword:
        if (code != EN_CHANGE) return;
        // Do not run a full sync on every keystroke: the only control whose
        // state depends on the text is OK, and re-setting text would move the
        // caret. The plaintext stays in the edit (that is where the user typed
        // it) and is masked by EM_SETPASSWORDCHAR.
        EnableWindow(okButton_, (model_.view().canSubmit &&
                                 !ControlTextOf(passwordEdit_).empty())
                                    ? TRUE
                                    : FALSE);
        return;
      case ids::kVault:
        if (code == LBN_SELCHANGE) {
          const bool hasSelection =
              SendMessageW(vaultList_, LB_GETCURSEL, 0, 0) != static_cast<LRESULT>(LB_ERR);
          EnableWindow(useVaultButton_,
                       (model_.view().canSubmit && hasSelection) ? TRUE : FALSE);
          return;
        }
        if (code == LBN_DBLCLK) {
          OnUseVault();
          return;
        }
        return;
      case ids::kUseVault:
        if (code != BN_CLICKED) return;
        OnUseVault();
        return;
      case ids::kOk:
        if (code != BN_CLICKED) return;
        OnAccept();
        return;
      case ids::kCancel:
        if (code != BN_CLICKED) return;
        CloseAsCancelled();
        return;
      default:
        return;
    }
  }

  // Reads the typed password exactly once, clears the control BEFORE invoking
  // the app callback, and hands the secret over as a copy the caller owns.
  void OnAccept() {
    if (!model_.view().canSubmit) {
      SyncControls();
      return;
    }
    std::wstring wide = ControlTextOf(passwordEdit_);
    std::string typed = Utf8FromWide(wide);
    if (!wide.empty()) SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));

    syncing_ = true;
    SetWindowTextW(passwordEdit_, L"");
    syncing_ = false;

    const prism::pwd::UiState before = model_.state();
    model_.Submit(typed, model_.rememberChoice());
    const bool started = before != prism::pwd::UiState::Probing &&
                         model_.state() == prism::pwd::UiState::Probing;
    if (started && onSubmit_) onSubmit_(typed);
    prism::pwd::PasswordRing::SecureZero(typed);
    SyncControls();
  }

  void OnUseVault() {
    const LRESULT selection = SendMessageW(vaultList_, LB_GETCURSEL, 0, 0);
    if (selection == static_cast<LRESULT>(LB_ERR)) return;
    if (!model_.view().canSubmit) return;

    // A vault pick replaces whatever was typed; the edit is cleared first so the
    // two secrets can never be combined or left behind.
    syncing_ = true;
    SetWindowTextW(passwordEdit_, L"");
    syncing_ = false;

    if (!model_.PickVault(static_cast<size_t>(selection), model_.rememberChoice())) {
      SyncControls();
      return;
    }
    if (onSubmit_) {
      std::string pending = model_.pendingPassword();
      onSubmit_(pending);
      prism::pwd::PasswordRing::SecureZero(pending);
    }
    SyncControls();
  }

  void CloseAsCancelled() {
    if (hwnd_ == nullptr) return;
    const prism::pwd::UiState before = model_.state();
    model_.Cancel();
    // A window close while the archive is already open is not a cancel.
    if (before != prism::pwd::UiState::Opened &&
        before != prism::pwd::UiState::Cancelled && onCancel_) {
      onCancel_();
    }
    Destroy();
  }

  static std::wstring ControlTextOf(HWND control) {
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
  }

  void SetTextIfDifferent(HWND control, const std::wstring& text) const {
    if (control == nullptr) return;
    if (ControlTextOf(control) == text) return;
    SetWindowTextW(control, text.c_str());
  }

  static std::string ErrorLine(const prism::pwd::UiView& view) {
    if (view.wrongCount == 0 && view.state != prism::pwd::UiState::Exhausted) {
      return std::string();
    }
    std::string line = "Wrong attempts: " + std::to_string(view.wrongCount) + " / " +
                       std::to_string(view.budget);
    if (view.state == prism::pwd::UiState::Exhausted) {
      line += " (budget exhausted, no retries left)";
    } else {
      line += " (" + std::to_string(view.attemptsLeft) + " left)";
    }
    return line;
  }

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  DWORD lastError_ = 0;
  int missingControlId_ = 0;
  bool syncing_ = false;
  prism::pwd::PasswordUiModel model_;

  HWND promptLabel_ = nullptr;
  HWND passwordEdit_ = nullptr;
  HWND showPasswordCheck_ = nullptr;
  HWND currentLabel_ = nullptr;
  HWND rememberRadio_ = nullptr;
  HWND onceRadio_ = nullptr;
  HWND errorLabel_ = nullptr;
  HWND statusLabel_ = nullptr;
  HWND progressBar_ = nullptr;
  HWND progressLabel_ = nullptr;
  HWND candidatesList_ = nullptr;
  HWND vaultList_ = nullptr;
  HWND useVaultButton_ = nullptr;
  HWND okButton_ = nullptr;
  HWND cancelButton_ = nullptr;

  SubmitHandler onSubmit_;
  OpenedHandler onOpened_;
  CancelHandler onCancel_;
};

}  // namespace prism::win::pwd_ui

#endif  // _WIN32
