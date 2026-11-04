// PrismArchive -- PrismWin
// PasswordDialog: the Win32 shell of the password ring UI (card C35), redesigned
// in C88 to the V3 glass language and translated through the shipped tables.
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
// C88: WHAT CHANGED AND WHAT DID NOT
//   * LANGUAGE. Every visible string now comes from the language tables
//     (password.*), including the two halves of the error line and the nine
//     status sentences. The model still produces the English sentence in
//     UiView::message -- that is the fallback for callers without a table (the
//     C35 suite) -- but it also publishes a UiMessage CODE, so this file never
//     parses English prose to translate it.
//   * PIXELS. The client is a floating glass card: GlassPanel's three
//     primitives (shadow / fill / rim, radius 14) over the canvas token, in the
//     Midnight variant the other V3 dialogs use. Buttons, the checkbox and the
//     two radios are owner-drawn from ThemeTokens; the password field and the
//     two lists get token-coloured frames, and the field draws an accent focus
//     ring while it owns the keyboard. No literal colour exists in this file:
//     every channel comes from prism::theme.
//   * WHAT DID NOT CHANGE. Every control keeps its id and its text; the dialog
//     still creates, centers and shows itself in Create(); the typed plaintext
//     is still read once on OK, scrubbed, and never displayed.
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
// CONTRACTS
//   B1  every std::min/max is parenthesised (a consumer TU may include
//       windows.h without NOMINMAX).
//   B2  this header includes <cstdint> for the counters it uses.
//   B9  Windows-only code; nothing here runs on a worker thread.
//   D3  two compile-time mutants: PRISM_C88_MUTANT_SYSTEM_GREY paints the
//       system face colour instead of the tokens, PRISM_C88_MUTANT_HARDCODED_
//       ENGLISH puts the English prompt back; both bump MutationCount() so the
//       suite can prove the injection actually ran.
//   D26 the mutants are PASS_REGULAR_EXPRESSION-only (see the C88 suite).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <commctrl.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/I18n.hpp"
#include "../PrismCore/PasswordUiModel.hpp"
#include "../PrismCore/ThemeTokens.hpp"
#include "GlassPanel.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")

// The two DWM attributes the card uses to keep the OS-drawn caption on the same
// palette as the client. Spelled locally because older SDK headers stop at 19.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_BORDER_COLOR
#define DWMWA_BORDER_COLOR 34
#endif
#ifndef DWMWA_CAPTION_COLOR
#define DWMWA_CAPTION_COLOR 35
#endif
#ifndef DWMWA_TEXT_COLOR
#define DWMWA_TEXT_COLOR 36
#endif

#if defined(_WIN32)

namespace prism::win::pwd_ui {

// pwd_ui is a sibling of prism::pwd, not a child, so unqualified lookup does not
// reach the model's names. These aliases keep the dialog readable without
// sprinkling prism::pwd:: over every signature.
using prism::pwd::PasswordUiConfig;
using prism::pwd::PasswordUiModel;
using prism::pwd::RememberChoice;
using prism::pwd::UiMessage;
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
// C88: the four captions used to be id 0 (unaddressable). They have ids now so
// the language switch can retitle them and the UIA test can name them.
constexpr int kCaptionPassword = 3120;
constexpr int kCaptionCurrent = 3121;
constexpr int kCaptionRing = 3122;
constexpr int kCaptionVault = 3123;
}  // namespace ids

// ---------------------------------------------------------------------------
// D3: deliberate-defect bookkeeping. The counter is shared by every inline copy
// through ODR, so the suite's main() can ask whether the injection ever ran.
// ---------------------------------------------------------------------------
#if defined(PRISM_C88_MUTANT_SYSTEM_GREY)
inline constexpr bool kMutantSystemGrey = true;
#else
inline constexpr bool kMutantSystemGrey = false;
#endif
#if defined(PRISM_C88_MUTANT_HARDCODED_ENGLISH)
inline constexpr bool kMutantHardcodedEnglish = true;
#else
inline constexpr bool kMutantHardcodedEnglish = false;
#endif

inline int& MutationCount() {
  static int count = 0;
  return count;
}
inline bool MutationFired() { return MutationCount() != 0; }
inline void ResetMutation() { MutationCount() = 0; }

// ---------------------------------------------------------------------------
// Geometry. One table for the painter, the child controls and the pixel suite:
// a test that had to re-derive these numbers could not tell a layout bug from a
// different (wrong) expectation.
// ---------------------------------------------------------------------------
namespace layout {
constexpr int kCardInset = 10;  // canvas ring around the floating card
constexpr int kRadius = 14;     // DESIGN.md: the floating card's corner
constexpr int kFieldRadius = 10;

inline RECT Rect(int left, int top, int right, int bottom) {
  RECT r{};
  r.left = left;
  r.top = top;
  r.right = right;
  r.bottom = bottom;
  return r;
}
inline RECT Prompt() { return Rect(26, 22, 534, 46); }
inline RECT PasswordCaption() { return Rect(26, 58, 106, 80); }
inline RECT PasswordFrame() { return Rect(112, 54, 390, 86); }
inline RECT PasswordEdit() { return Rect(114, 56, 388, 84); }
inline RECT ShowPassword() { return Rect(400, 56, 534, 86); }
inline RECT CurrentCaption() { return Rect(26, 96, 106, 116); }
inline RECT CurrentValue() { return Rect(112, 96, 390, 116); }
inline RECT Remember() { return Rect(112, 122, 306, 146); }
inline RECT Once() { return Rect(306, 122, 500, 146); }
inline RECT Error() { return Rect(26, 152, 534, 172); }
inline RECT Status() { return Rect(26, 176, 534, 216); }
inline RECT Progress() { return Rect(26, 224, 360, 240); }
inline RECT ProgressLabel() { return Rect(368, 222, 500, 242); }
inline RECT RingCaption() { return Rect(26, 248, 272, 268); }
inline RECT VaultCaption() { return Rect(300, 248, 534, 268); }
inline RECT CandidatesFrame() { return Rect(26, 272, 272, 372); }
inline RECT CandidatesList() { return Rect(27, 273, 271, 371); }
inline RECT VaultFrame() { return Rect(300, 272, 534, 372); }
inline RECT VaultList() { return Rect(301, 273, 533, 371); }
inline RECT UseVault() { return Rect(300, 380, 420, 406); }
inline RECT Ok() { return Rect(346, 418, 434, 448); }
inline RECT Cancel() { return Rect(442, 418, 534, 448); }
}  // namespace layout

// ---------------------------------------------------------------------------
// Colour: every channel is a C15 token (CONTRACTS A6). The helpers compose the
// translucent tokens into the opaque pixels GDI controls and the pixel suite
// both need, and nowhere else is a colour written down.
// ---------------------------------------------------------------------------
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;

inline constexpr Variant kCardVariant = Variant::Midnight;

inline Rgba TokenColor(TokenId id) {
  return prism::theme::Lookup(id, kCardVariant).color;
}
inline Rgba TokenFrom(TokenId id) {
  return prism::theme::Lookup(id, kCardVariant).gradient.from;
}
inline Rgba Over(const Rgba& top, const Rgba& bottom) {
  return prism::theme::CompositeOver(top, bottom);
}
inline Rgba Fade(const Rgba& color, float alpha) {
  return Rgba(color.r, color.g, color.b, alpha);
}

inline Rgba BackdropPixel() { return TokenColor(TokenId::SurfaceBackdropReference); }
inline Rgba CanvasPixel() {
  return Over(TokenColor(TokenId::SurfaceCanvasBase), BackdropPixel());
}
inline Rgba CardPixel() {
  return Over(prism::theme::EffectiveGlassSurface(kCardVariant), CanvasPixel());
}
// The card behind the child controls is opaque, so a control's background can be
// exactly this colour and the seam disappears.
inline Rgba AccentPixel() {
  return Over(prism::theme::EffectiveAccent(kCardVariant), CardPixel());
}
inline Rgba FieldPixel() {
  return Over(TokenColor(TokenId::SurfaceCanvasBase), CardPixel());
}
inline Rgba FieldBorderPixel() {
  return Over(Fade(TokenFrom(TokenId::BorderSpecularRim), 0.16f), FieldPixel());
}
inline Rgba ButtonHoverPixel() {
  return Over(TokenColor(TokenId::SurfaceTabHover), CardPixel());
}
inline Rgba ButtonPressedPixel() {
  return Over(TokenColor(TokenId::SelectionTint), CardPixel());
}
inline Rgba TextPixel() { return TokenColor(TokenId::TextPrimary); }
inline Rgba SecondaryTextPixel() { return TokenColor(TokenId::TextSecondary); }
inline Rgba DisabledFillPixel() {
  return Over(Fade(SecondaryTextPixel(), 0.10f), CardPixel());
}
inline Rgba DisabledTextPixel(const Rgba& fill) {
  return Over(Fade(SecondaryTextPixel(), 0.55f), fill);
}
inline Rgba SystemGreyPixel() {
  const COLORREF face = GetSysColor(COLOR_BTNFACE);
  const auto channel = [](BYTE v) { return static_cast<float>(v) / 255.0f; };
  return Rgba(channel(GetRValue(face)), channel(GetGValue(face)),
              channel(GetBValue(face)), 1.0f);
}
inline COLORREF ToColorRef(const Rgba& c) {
  const auto channel = [](float value) {
    const float clamped = (std::max)(0.0f, (std::min)(1.0f, value));
    return static_cast<BYTE>(clamped * 255.0f + 0.5f);
  };
  return RGB(channel(c.r), channel(c.g), channel(c.b));
}
inline D2D1_COLOR_F ToD2D(const Rgba& c) {
  return D2D1::ColorF(c.r, c.g, c.b, c.a);
}

// ---------------------------------------------------------------------------
// The English fallback. The product always installs a translator (Main.cpp);
// this table exists so the C35 suite -- which constructs the dialog with no
// language files on disk -- and any headless caller keep the previous English
// behaviour. c88's first case compares every fallback to en-US.json, so the two
// copies cannot drift silently.
// ---------------------------------------------------------------------------
struct EnglishRow {
  const char* key;
  const char* pattern;
};

inline const std::vector<EnglishRow>& EnglishRows() {
  static const std::vector<EnglishRow> rows = {
      {"password.title", "Password required"},
      {"password.prompt", "This archive needs a password."},
      {"password.label", "Password"},
      {"password.show", "Show password"},
      {"password.current", "Current"},
      {"password.none", "(none)"},
      {"password.remember", "Remember in vault"},
      {"password.once", "This time only"},
      {"password.ring", "Ring (masked)"},
      {"password.vault", "Stored passwords (masked)"},
      {"password.useSelected", "Use selected"},
      {"password.open", "Open"},
      {"password.cancel", "Cancel"},
      {"password.progress", "matching {done}/{total}"},
      {"password.error.wrong", "Wrong attempts: {n} / {budget}"},
      {"password.error.left", "{left} left"},
      {"password.error.exhausted", "budget exhausted, no retries left"},
      {"password.status.start",
       "This archive is password protected. No candidate password is available; "
       "type the password."},
      {"password.status.missed",
       "tried {tried}/{total} candidates, none matched. Type the password."},
      {"password.status.wrong", "wrong password (attempt {n}), {left} tries left."},
      {"password.status.exhausted",
       "wrong password (attempt {n}). retry budget exhausted; no more attempts."},
      {"password.status.enter", "enter a password."},
      {"password.status.probing", "a probe is already running."},
      {"password.status.idle", "the password flow has not started."},
      {"password.status.cancelled", "cancelled; no more attempts."},
      {"password.status.opened", "the password already matched."},
      {"password.status.budget", "retry budget exhausted; no more attempts."},
  };
  return rows;
}

inline const char* EnglishPattern(const char* key) {
  for (const EnglishRow& row : EnglishRows()) {
    if (std::string_view(row.key) == key) return row.pattern;
  }
  return key;  // an unknown key renders as itself, like Translator::Translate
}

// The i18n key for a model message code (C88). None means "the model's own
// sentence is the answer" and is only reachable without a table.
inline const char* MessageKey(UiMessage code) {
  switch (code) {
    case UiMessage::StartNoCandidates: return "password.status.start";
    case UiMessage::CandidatesMissed: return "password.status.missed";
    case UiMessage::WrongPassword: return "password.status.wrong";
    case UiMessage::BudgetExhausted: return "password.status.exhausted";
    case UiMessage::EnterPassword: return "password.status.enter";
    case UiMessage::ProbeRunning: return "password.status.probing";
    case UiMessage::NotStarted: return "password.status.idle";
    case UiMessage::Cancelled: return "password.status.cancelled";
    case UiMessage::AlreadyMatched: return "password.status.opened";
    case UiMessage::RetryBudgetExhausted: return "password.status.budget";
    case UiMessage::None: break;
  }
  return nullptr;
}

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

  // C88: the language table. The product installs it before Create(); a caller
  // without one keeps the English fallback (the C35 suite's shape). Safe to call
  // before or after Create -- an open dialog is retitled in place.
  void SetTranslator(const prism::i18n::Translator* translator) {
    translator_ = translator;
    if (hwnd_ == nullptr) return;
    RefreshStaticTexts();
    SyncControls();
    ApplyFrameColors();
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // C84: the password gate is the worst case of the invisible-dialog defect --
  // the app looked hung for as long as the dialog was up (measured: process
  // alive, zero visible windows). Showing and centering are part of Create, so
  // the product path (RunPasswordGate) and every test get them for free.
  bool Create(HINSTANCE instance, const std::wstring& title = L"Password required",
              HWND owner = nullptr) {
    instance_ = instance;
    owner_ = owner;
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
    if (hwnd_ == nullptr) {
      lastError_ = GetLastError();
      return false;
    }
    ShowCreated();
    return true;
  }

  // C84: the modal gate calls this after Create -- a visible dialog that never
  // takes the foreground/keyboard still reads as a frozen app. Public so the
  // product path and the Windows suite run the same code.
  void FocusInput() {
    if (hwnd_ == nullptr) return;
    SetForegroundWindow(hwnd_);
    SetFocus(passwordEdit_);
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    ReleaseResources();
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
    InvalidateRect(showPasswordCheck_, nullptr, TRUE);
    InvalidateRect(rememberRadio_, nullptr, TRUE);
    InvalidateRect(onceRadio_, nullptr, TRUE);

    SendMessageW(progressBar_, PBM_SETRANGE32, 0,
                 view.total > 0 ? static_cast<LPARAM>(view.total) : 1);
    SendMessageW(progressBar_, PBM_SETPOS, static_cast<WPARAM>(view.done), 0);
    SetTextIfDifferent(progressLabel_, ProgressText(view));
    SetTextIfDifferent(currentLabel_,
                       view.masked.empty() ? Text("password.none") : WideFromUtf8(view.masked));
    SetTextIfDifferent(errorLabel_, ErrorLine(view));
    SetTextIfDifferent(statusLabel_, StatusText(view));

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

  // The card rectangle in client pixels, for the pixel suite and for PaintCard.
  static RECT CardRect() {
    return layout::Rect(layout::kCardInset, layout::kCardInset,
                        kClientWidth - layout::kCardInset,
                        kClientHeight - layout::kCardInset);
  }

  // C88: what the language table says for a key, with the English fallback when
  // no table is installed. NEVER returns a key name (a menu reading
  // "password.prompt" would be worse than an English sentence).
  std::wstring Text(const char* key,
                    const std::vector<prism::i18n::Substitution>& subs = {}) const {
    if (translator_ != nullptr && translator_->Has(key)) {
      return WideFromUtf8(translator_->Translate(key, subs));
    }
    const std::string pattern(EnglishPattern(key));
    return WideFromUtf8(prism::i18n::FormatPattern(pattern, subs));
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
      case WM_PAINT:
        PaintCard();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_SIZE:
        if (target_) {
          target_->Resize(D2D1::SizeU(static_cast<UINT32>(LOWORD(lparam)),
                                      static_cast<UINT32>(HIWORD(lparam))));
        }
        return 0;
      case WM_CTLCOLORSTATIC:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORLISTBOX:
        return ColorControl(reinterpret_cast<HDC>(wparam), reinterpret_cast<HWND>(lparam));
      case WM_DRAWITEM:
        if (lparam != 0) {
          DrawItem(*reinterpret_cast<const DRAWITEMSTRUCT*>(lparam));
          return TRUE;
        }
        break;
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
        ReleaseResources();
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  HWND MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, RECT box) {
    const HWND control = CreateWindowExW(
        0, klass, text, WS_CHILD | WS_VISIBLE | style, box.left, box.top,
        box.right - box.left, box.bottom - box.top, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (control == nullptr) {
      missingControlId_ = id;
      lastError_ = GetLastError();
      return nullptr;
    }
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(bodyFont_ != nullptr ? bodyFont_
                                                               : GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
    return control;
  }

  HWND MakeLabel(const wchar_t* text, int id, RECT box, HFONT font = nullptr) {
    const HWND label = MakeControl(L"STATIC", text, SS_LEFT, id, box);
    if (label != nullptr && font != nullptr) {
      SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    return label;
  }

  bool CreateChildren() {
    // One font family, three sizes from DESIGN.md's type scale (11/13/15/20).
    bodyFont_ = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    promptFont_ = CreateFontW(-15, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (bodyFont_ == nullptr) bodyFont_ = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    if (promptFont_ == nullptr) promptFont_ = bodyFont_;

    promptLabel_ = MakeLabel(Text("password.prompt").c_str(), ids::kPrompt, layout::Prompt(),
                             promptFont_);
    passwordCaption_ = MakeLabel(Text("password.label").c_str(), ids::kCaptionPassword,
                                 layout::PasswordCaption());
    const RECT editBox = layout::PasswordEdit();
    passwordEdit_ = MakeControl(L"EDIT", L"",
                                ES_PASSWORD | ES_AUTOHSCROLL | WS_TABSTOP,
                                ids::kPassword, editBox);
    // C88: the checkbox and the two radios keep their REAL button styles
    // (BS_AUTOCHECKBOX / BS_AUTORADIOBUTTON), because the accessibility role a
    // screen reader sees is derived from that style -- an owner-draw button is
    // published as a push button. They paint themselves through a subclass, so
    // the art is ours and the semantics stay the OS's.
    showPasswordCheck_ = MakeControl(L"BUTTON", Text("password.show").c_str(),
                                     BS_AUTOCHECKBOX | WS_TABSTOP, ids::kShowPassword,
                                     layout::ShowPassword());
    currentCaption_ = MakeLabel(Text("password.current").c_str(), ids::kCaptionCurrent,
                                layout::CurrentCaption());
    currentLabel_ = MakeLabel(Text("password.none").c_str(), ids::kCurrent,
                              layout::CurrentValue());

    rememberRadio_ = MakeControl(L"BUTTON", Text("password.remember").c_str(),
                                 BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP,
                                 ids::kRemember, layout::Remember());
    onceRadio_ = MakeControl(L"BUTTON", Text("password.once").c_str(),
                             BS_AUTORADIOBUTTON | WS_TABSTOP, ids::kOnce,
                             layout::Once());
    SendMessageW(onceRadio_, BM_SETCHECK, BST_CHECKED, 0);

    errorLabel_ = MakeLabel(L"", ids::kError, layout::Error());
    statusLabel_ = MakeLabel(L"", ids::kStatus, layout::Status());
    const RECT progressBox = layout::Progress();
    progressBar_ = MakeControl(L"msctls_progress32", L"", PBS_SMOOTH, ids::kProgress,
                               progressBox);
    if (progressBar_ != nullptr) {
      // The themed progress control ignores PBM_*COLOR under visual styles on
      // some builds; setting them is still the only token hook it has, and the
      // bar is only on screen while the probe runs.
      SendMessageW(progressBar_, PBM_SETBARCOLOR, 0, ToColorRef(AccentPixel()));
      SendMessageW(progressBar_, PBM_SETBKCOLOR, 0, ToColorRef(FieldPixel()));
    }
    progressLabel_ = MakeLabel(L"", ids::kProgressLabel, layout::ProgressLabel());

    ringCaption_ = MakeLabel(Text("password.ring").c_str(), ids::kCaptionRing,
                             layout::RingCaption());
    const RECT candidateBox = layout::CandidatesList();
    candidatesList_ = MakeControl(L"LISTBOX", L"",
                                  LBS_NOINTEGRALHEIGHT | WS_VSCROLL,
                                  ids::kCandidates, candidateBox);
    vaultCaption_ = MakeLabel(Text("password.vault").c_str(), ids::kCaptionVault,
                              layout::VaultCaption());
    const RECT vaultBox = layout::VaultList();
    vaultList_ = MakeControl(L"LISTBOX", L"",
                             LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | WS_VSCROLL, ids::kVault,
                             vaultBox);
    useVaultButton_ = MakeControl(L"BUTTON", Text("password.useSelected").c_str(),
                                  BS_OWNERDRAW | WS_TABSTOP, ids::kUseVault,
                                  layout::UseVault());
    okButton_ = MakeControl(L"BUTTON", Text("password.open").c_str(), BS_OWNERDRAW,
                            ids::kOk, layout::Ok());
    cancelButton_ = MakeControl(L"BUTTON", Text("password.cancel").c_str(),
                                BS_OWNERDRAW | WS_TABSTOP, ids::kCancel, layout::Cancel());

    if (promptLabel_ == nullptr || passwordEdit_ == nullptr ||
        showPasswordCheck_ == nullptr || currentLabel_ == nullptr ||
        rememberRadio_ == nullptr || onceRadio_ == nullptr || errorLabel_ == nullptr ||
        statusLabel_ == nullptr || progressBar_ == nullptr || progressLabel_ == nullptr ||
        candidatesList_ == nullptr || vaultList_ == nullptr || useVaultButton_ == nullptr ||
        okButton_ == nullptr || cancelButton_ == nullptr) {
      return false;
    }

    // The three toggles are painted by ToggleSubclass, not by the button proc.
    SetWindowSubclass(showPasswordCheck_, &PasswordDialog::ToggleSubclass,
                      static_cast<UINT_PTR>(ids::kShowPassword),
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(rememberRadio_, &PasswordDialog::ToggleSubclass,
                      static_cast<UINT_PTR>(ids::kRemember),
                      reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(onceRadio_, &PasswordDialog::ToggleSubclass,
                      static_cast<UINT_PTR>(ids::kOnce),
                      reinterpret_cast<DWORD_PTR>(this));

    cardBrush_ = CreateSolidBrush(ToColorRef(CardPixel()));
    fieldBrush_ = CreateSolidBrush(ToColorRef(FieldPixel()));
    RefreshStaticTexts();
    ApplyFrameColors();
    PopulateLists();
    SyncControls();
    return true;
  }

  // The OS-drawn caption is kept (the dialog is not one of the borderless V3
  // cards) but it is asked to use the card's palette. Win11 honours the colour
  // attributes; older builds fall back to the dark flag alone, which is still
  // "not the light-grey system frame" the card removed.
  void ApplyFrameColors() {
    if (hwnd_ == nullptr) return;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    const COLORREF caption = ToColorRef(CardPixel());
    DwmSetWindowAttribute(hwnd_, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    const COLORREF text = ToColorRef(TextPixel());
    DwmSetWindowAttribute(hwnd_, DWMWA_TEXT_COLOR, &text, sizeof(text));
    const COLORREF border = ToColorRef(CardPixel());
    DwmSetWindowAttribute(hwnd_, DWMWA_BORDER_COLOR, &border, sizeof(border));
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

  void RefreshStaticTexts() {
    if (hwnd_ == nullptr) return;
    if (kMutantHardcodedEnglish) ++MutationCount();
    const std::wstring prompt = kMutantHardcodedEnglish
                                    ? std::wstring(L"This archive needs a password.")
                                    : Text("password.prompt");
    SetWindowTextW(promptLabel_, prompt.c_str());
    if (passwordCaption_ != nullptr) {
      SetWindowTextW(passwordCaption_, Text("password.label").c_str());
    }
    if (currentCaption_ != nullptr) {
      SetWindowTextW(currentCaption_, Text("password.current").c_str());
    }
    if (ringCaption_ != nullptr) SetWindowTextW(ringCaption_, Text("password.ring").c_str());
    if (vaultCaption_ != nullptr) SetWindowTextW(vaultCaption_, Text("password.vault").c_str());
    if (showPasswordCheck_ != nullptr) {
      SetWindowTextW(showPasswordCheck_, Text("password.show").c_str());
    }
    if (rememberRadio_ != nullptr) {
      SetWindowTextW(rememberRadio_, Text("password.remember").c_str());
    }
    if (onceRadio_ != nullptr) SetWindowTextW(onceRadio_, Text("password.once").c_str());
    if (useVaultButton_ != nullptr) {
      SetWindowTextW(useVaultButton_, Text("password.useSelected").c_str());
    }
    if (okButton_ != nullptr) SetWindowTextW(okButton_, Text("password.open").c_str());
    if (cancelButton_ != nullptr) {
      SetWindowTextW(cancelButton_, Text("password.cancel").c_str());
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
  }

  void OnCommand(int id, int code, HWND control) {
    if (control == nullptr) return;
    switch (id) {
      case ids::kShowPassword:
        if (code != BN_CLICKED) return;
        // The BUTTON's own auto-toggle already ran (that is the point of keeping
        // BS_AUTOCHECKBOX); this reads the state the user now sees.
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
        if (code == EN_SETFOCUS || code == EN_KILLFOCUS) {
          // The field's ring is painted by the card, not by the edit. The flag
          // (not GetFocus() at paint time) makes the ring assertable without a
          // foreground window: the notification IS what the edit sends.
          fieldFocused_ = code == EN_SETFOCUS;
          InvalidateRect(hwnd_, nullptr, FALSE);
          return;
        }
        if (code != EN_CHANGE) return;
        // Do not run a full sync on every keystroke: the only control whose
        // state depends on the text is OK, and re-setting text would move the
        // caret. The plaintext stays in the edit (that is where the user typed
        // it) and is masked by EM_SETPASSWORDCHAR.
        EnableWindow(okButton_, (model_.view().canSubmit &&
                                 !ControlTextOf(passwordEdit_).empty())
                                    ? TRUE
                                    : FALSE);
        InvalidateRect(okButton_, nullptr, TRUE);
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

  std::wstring ProgressText(const prism::pwd::UiView& view) const {
    if (translator_ == nullptr) return WideFromUtf8(view.progressLabel);
    return Text("password.progress", {{"done", std::to_string(view.done)},
                                      {"total", std::to_string(view.total)}});
  }

  // C88: the error line is two keys, not one sentence assembled in code. The
  // "wrong attempts" half and the "left / exhausted" half live in the table.
  std::wstring ErrorLine(const prism::pwd::UiView& view) const {
    if (view.wrongCount == 0 && view.state != prism::pwd::UiState::Exhausted) {
      return std::wstring();
    }
    const std::wstring prefix =
        Text("password.error.wrong", {{"n", std::to_string(view.wrongCount)},
                                      {"budget", std::to_string(view.budget)}});
    const bool exhausted = view.state == prism::pwd::UiState::Exhausted;
    const std::wstring suffix =
        exhausted
            ? Text("password.error.exhausted")
            : Text("password.error.left", {{"left", std::to_string(view.attemptsLeft)}});
    return prefix + L" (" + suffix + L")";
  }

  // C88: the status sentence comes from the model's message CODE, so this file
  // never parses English prose. Without a table the model's own sentence is
  // shown verbatim (the C35 behaviour).
  std::wstring StatusText(const prism::pwd::UiView& view) const {
    if (translator_ == nullptr) return WideFromUtf8(view.message);
    const char* key = MessageKey(view.messageCode);
    std::wstring text;
    if (key == nullptr) {
      text = WideFromUtf8(view.message);
    } else {
      text = Text(key, {{"n", std::to_string(view.wrongCount)},
                        {"left", std::to_string(view.attemptsLeft)},
                        {"tried", std::to_string(view.done)},
                        {"total", std::to_string(view.total)},
                        {"budget", std::to_string(view.budget)}});
    }
    // The engine trace is ENGLISH text this dialog does not own; a localized
    // card shows the localized sentence and leaves the trace to the caller/log.
    // The no-table path keeps it (the C35 suite and a debug build read it), so
    // nothing is lost where it is actionable.
    if (translator_ == nullptr && !view.detail.empty()) {
      text += L" " + WideFromUtf8(view.detail);
    }
    return text;
  }

  // -------------------------------------------------------------------------
  // Painting
  // -------------------------------------------------------------------------
  bool EnsureTarget() {
    if (target_) return true;
    if (!d2d_) {
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   d2d_.ReleaseAndGetAddressOf()))) {
        return false;
      }
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>((std::max)(1L, client.right - client.left)),
        static_cast<UINT32>((std::max)(1L, client.bottom - client.top)));
    const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f,
        96.0f);
    const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
        D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_NONE);
    if (FAILED(d2d_->CreateHwndRenderTarget(properties, hwndProperties,
                                            target_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    target_->SetDpi(96.0f, 96.0f);
    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f),
                                              brush_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    panel_.Bind(target_.Get(), brush_.Get());
    return true;
  }

  static D2D1_RECT_F RectFOf(const RECT& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
  }

  void FillRounded(const D2D1_RECT_F& rect, float radius, const Rgba& color) {
    brush_->SetColor(ToD2D(color));
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (SUCCEEDED(prism::win::glass::CreateSquirclePath(d2d_.Get(), rect, radius,
                                                        path.ReleaseAndGetAddressOf()))) {
      target_->FillGeometry(path.Get(), brush_.Get());
    } else {
      target_->FillRectangle(rect, brush_.Get());
    }
  }

  void StrokeRounded(const D2D1_RECT_F& rect, float radius, const Rgba& color,
                     float width) {
    brush_->SetColor(ToD2D(color));
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
    if (SUCCEEDED(prism::win::glass::CreateSquirclePath(d2d_.Get(), rect, radius,
                                                        path.ReleaseAndGetAddressOf()))) {
      target_->DrawGeometry(path.Get(), brush_.Get(), width);
    } else {
      target_->DrawRectangle(rect, brush_.Get(), width);
    }
  }

  // A text field's plate: the token fill plus a quiet border, promoted to the
  // accent ring while the field owns the keyboard.
  void PaintFieldFrame(const RECT& box, bool focused, bool accentFill) {
    const D2D1_RECT_F rect = RectFOf(box);
    FillRounded(rect, static_cast<float>(layout::kFieldRadius), FieldPixel());
    if (focused) {
      StrokeRounded(rect, static_cast<float>(layout::kFieldRadius), AccentPixel(), 2.0f);
    } else if (accentFill) {
      StrokeRounded(rect, static_cast<float>(layout::kFieldRadius), AccentPixel(), 1.0f);
    } else {
      StrokeRounded(rect, static_cast<float>(layout::kFieldRadius), FieldBorderPixel(),
                    1.0f);
    }
  }

  void PaintCard() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    bool painted = false;
    if (kMutantSystemGrey) ++MutationCount();
    if (EnsureTarget()) {
      target_->BeginDraw();
      target_->SetTransform(D2D1::Matrix3x2F::Identity());
      const Rgba canvas = kMutantSystemGrey ? SystemGreyPixel() : CanvasPixel();
      target_->Clear(ToD2D(canvas));
      const D2D1_RECT_F card = RectFOf(CardRect());
      // Bind on EVERY frame: Unbind at the end of the previous one dropped the
      // borrowed brush/factory, and the next frame painted only the Clear().
      panel_.Bind(target_.Get(), brush_.Get());
      prism::win::glass::Parts parts;
      if (kMutantSystemGrey) {
        // NEGATIVE CONTROL: the card is the system face colour, not the glass
        // token -- the pixel assertion that the background equals the token MUST
        // go red, and MutationCount() proves this branch ran.
        parts.hasExplicitFill = true;
        parts.explicitFill = SystemGreyPixel();
        parts.shadow = false;
      }
      panel_.Paint(target_.Get(), card, kCardVariant, parts);

      PaintFieldFrame(layout::PasswordFrame(), fieldFocused_, false);
      PaintFieldFrame(layout::CandidatesFrame(), false, false);
      PaintFieldFrame(layout::VaultFrame(), false, false);

      panel_.Unbind();
      if (SUCCEEDED(target_->EndDraw())) painted = true;
    }
    if (!painted) {
      // A headless/remote session can fail to create a D2D target; the card
      // must still read as a card, not as a white rectangle.
      RECT client{};
      GetClientRect(hwnd_, &client);
      const Rgba canvas = kMutantSystemGrey ? SystemGreyPixel() : CanvasPixel();
      HBRUSH canvasBrush = CreateSolidBrush(ToColorRef(canvas));
      if (canvasBrush != nullptr) {
        FillRect(dc, &client, canvasBrush);
        DeleteObject(canvasBrush);
      }
      HBRUSH cardBrush = CreateSolidBrush(ToColorRef(CardPixel()));
      if (cardBrush != nullptr) {
        const RECT card = CardRect();
        FillRect(dc, &card, cardBrush);
        DeleteObject(cardBrush);
      }
    }
    EndPaint(hwnd_, &paint);
  }

  // WM_CTLCOLOR*: the child controls keep their real Win32 class (UIA and BM_*
  // depend on it) but paint on the card's palette. The brush is cached because
  // the OS asks once per control per frame.
  LRESULT ColorControl(HDC dc, HWND control) {
    if (dc == nullptr) return reinterpret_cast<LRESULT>(cardBrush_);
    if (control == passwordEdit_ || control == candidatesList_ || control == vaultList_) {
      SetTextColor(dc, ToColorRef(IsWindowEnabled(control) ? TextPixel()
                                                           : DisabledTextPixel(FieldPixel())));
      SetBkColor(dc, ToColorRef(FieldPixel()));
      SetBkMode(dc, OPAQUE);
      return reinterpret_cast<LRESULT>(fieldBrush_);
    }
    const int id = control != nullptr ? GetDlgCtrlID(control) : 0;
    const Rgba text = (id == ids::kStatus || id == 0 || id == ids::kCaptionPassword ||
                       id == ids::kCaptionCurrent || id == ids::kCaptionRing ||
                       id == ids::kCaptionVault)
                          ? SecondaryTextPixel()
                          : TextPixel();
    SetTextColor(dc, ToColorRef(text));
    SetBkColor(dc, ToColorRef(CardPixel()));
    SetBkMode(dc, OPAQUE);
    return reinterpret_cast<LRESULT>(cardBrush_);
  }

  // -------------------------------------------------------------------------
  // Owner-drawn controls. Buttons, the checkbox and the radios are painted from
  // tokens instead of the system theme; their WINDOW TEXT is still the i18n
  // string, which is what MSAA/UIA publishes as the accessible name.
  // -------------------------------------------------------------------------
  void DrawItem(const DRAWITEMSTRUCT& item) {
    if (item.CtlType != ODT_BUTTON || item.hDC == nullptr) return;
    const bool enabled = (item.itemState & ODS_DISABLED) == 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const int id = static_cast<int>(item.CtlID);
    const std::wstring text = ControlTextOf(item.hwndItem);
    DrawPushButton(item, text, id == ids::kOk, enabled, pressed, focused);
  }

  // The three toggles paint themselves; the default button proc still owns the
  // click/space/arrow semantics and the BM_* state.
  static LRESULT CALLBACK ToggleSubclass(HWND hwnd, UINT message, WPARAM wparam,
                                         LPARAM lparam, UINT_PTR, DWORD_PTR ref) {
    auto* self = reinterpret_cast<PasswordDialog*>(ref);
    if (self == nullptr) return DefSubclassProc(hwnd, message, wparam, lparam);
    switch (message) {
      case WM_PAINT:
        self->PaintToggle(hwnd);
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        InvalidateRect(hwnd, nullptr, TRUE);
        break;
      default:
        break;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
  }

  void PaintToggle(HWND control) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(control, &paint);
    if (dc != nullptr) {
      RECT box{};
      GetClientRect(control, &box);
      DrawToggleBody(dc, box, ControlTextOf(control),
                     GetDlgCtrlID(control) != ids::kShowPassword,
                     SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED,
                     IsWindowEnabled(control) != FALSE, GetFocus() == control);
    }
    EndPaint(control, &paint);
  }

  void DrawPushButton(const DRAWITEMSTRUCT& item, const std::wstring& text, bool primary,
                      bool enabled, bool pressed, bool focused) {
    HDC dc = item.hDC;
    const RECT box = item.rcItem;
    HBRUSH background = CreateSolidBrush(ToColorRef(CardPixel()));
    FillRect(dc, &box, background);
    DeleteObject(background);

    Rgba fill = primary ? AccentPixel() : ButtonHoverPixel();
    if (pressed) fill = primary ? Over(Fade(TokenColor(TokenId::SurfaceBackdropReference),
                                            0.18f), AccentPixel())
                                : ButtonPressedPixel();
    if (!enabled) fill = DisabledFillPixel();
    Rgba textColor = TextPixel();
    if (!enabled) textColor = DisabledTextPixel(fill);

    SetBkMode(dc, TRANSPARENT);
    HBRUSH brush = CreateSolidBrush(ToColorRef(fill));
    HPEN pen = CreatePen(PS_SOLID, 1, ToColorRef(primary ? fill : FieldBorderPixel()));
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    RoundRect(dc, box.left, box.top, box.right, box.bottom, layout::kFieldRadius * 2,
              layout::kFieldRadius * 2);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(brush);
    DeleteObject(pen);

    if (focused) {
      const Rgba ring = primary ? TextPixel() : AccentPixel();
      HPEN focusPen = CreatePen(PS_SOLID, 2, ToColorRef(ring));
      HGDIOBJ old = SelectObject(dc, focusPen);
      HGDIOBJ oldNull = SelectObject(dc, GetStockObject(NULL_BRUSH));
      RoundRect(dc, box.left + 3, box.top + 3, box.right - 3, box.bottom - 3,
                layout::kFieldRadius * 2 - 4, layout::kFieldRadius * 2 - 4);
      SelectObject(dc, oldNull);
      SelectObject(dc, old);
      DeleteObject(focusPen);
    }

    SetTextColor(dc, ToColorRef(textColor));
    HGDIOBJ oldFont = SelectObject(dc, bodyFont_);
    RECT textBox = box;
    DrawTextW(dc, text.c_str(), -1, &textBox,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
  }

  void DrawToggleBody(HDC dc, RECT box, const std::wstring& text, bool radio,
                      bool checked, bool enabled, bool focused) {
    HBRUSH background = CreateSolidBrush(ToColorRef(CardPixel()));
    FillRect(dc, &box, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);

    const int glyph = 18;
    const int top = box.top + ((box.bottom - box.top) - glyph) / 2;
    const RECT glyphBox{box.left + 2, top, box.left + 2 + glyph, top + glyph};
    const Rgba frame = focused ? AccentPixel() : FieldBorderPixel();
    const Rgba plate = checked ? AccentPixel() : FieldPixel();

    HBRUSH fillBrush = CreateSolidBrush(ToColorRef(enabled ? plate : DisabledFillPixel()));
    HPEN framePen = CreatePen(PS_SOLID, focused ? 2 : 1, ToColorRef(frame));
    HGDIOBJ oldBrush = SelectObject(dc, fillBrush);
    HGDIOBJ oldPen = SelectObject(dc, framePen);
    if (radio) {
      Ellipse(dc, glyphBox.left, glyphBox.top, glyphBox.right, glyphBox.bottom);
    } else {
      RoundRect(dc, glyphBox.left, glyphBox.top, glyphBox.right, glyphBox.bottom, 8, 8);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(fillBrush);
    DeleteObject(framePen);

    if (checked) {
      HPEN markPen = CreatePen(PS_SOLID, 2, ToColorRef(TextPixel()));
      HGDIOBJ old = SelectObject(dc, markPen);
      if (radio) {
        const RECT dot{glyphBox.left + 5, glyphBox.top + 5, glyphBox.right - 5,
                       glyphBox.bottom - 5};
        HBRUSH dotBrush = CreateSolidBrush(ToColorRef(TextPixel()));
        HGDIOBJ oldDot = SelectObject(dc, dotBrush);
        Ellipse(dc, dot.left, dot.top, dot.right, dot.bottom);
        SelectObject(dc, oldDot);
        DeleteObject(dotBrush);
      } else {
        MoveToEx(dc, glyphBox.left + 4, glyphBox.top + 9, nullptr);
        LineTo(dc, glyphBox.left + 8, glyphBox.top + 13);
        LineTo(dc, glyphBox.left + 14, glyphBox.top + 5);
      }
      SelectObject(dc, old);
      DeleteObject(markPen);
    }

    SetTextColor(dc, ToColorRef(enabled ? TextPixel() : DisabledTextPixel(CardPixel())));
    HGDIOBJ oldFont = SelectObject(dc, bodyFont_);
    RECT textBox = box;
    textBox.left = glyphBox.right + 8;
    DrawTextW(dc, text.c_str(), -1, &textBox,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
  }

  void ReleaseResources() {
    panel_.Unbind();
    brush_.Reset();
    target_.Reset();
    d2d_.Reset();
    if (cardBrush_ != nullptr) {
      DeleteObject(cardBrush_);
      cardBrush_ = nullptr;
    }
    if (fieldBrush_ != nullptr) {
      DeleteObject(fieldBrush_);
      fieldBrush_ = nullptr;
    }
    HGDIOBJ stockFont = GetStockObject(DEFAULT_GUI_FONT);
    if (promptFont_ != nullptr && promptFont_ != bodyFont_ && promptFont_ != stockFont) {
      DeleteObject(promptFont_);
    }
    promptFont_ = nullptr;
    if (bodyFont_ != nullptr && bodyFont_ != stockFont) DeleteObject(bodyFont_);
    bodyFont_ = nullptr;
  }

  void ShowCreated();  // C84: center + ShowWindow + UpdateWindow, one place
  void CenterOnOwner();

  const prism::i18n::Translator* translator_ = nullptr;

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;  // C84: center over it when the app has a window
  DWORD lastError_ = 0;
  int missingControlId_ = 0;
  bool syncing_ = false;
  bool fieldFocused_ = false;
  prism::pwd::PasswordUiModel model_;

  HWND promptLabel_ = nullptr;
  HWND passwordCaption_ = nullptr;
  HWND passwordEdit_ = nullptr;
  HWND showPasswordCheck_ = nullptr;
  HWND currentCaption_ = nullptr;
  HWND currentLabel_ = nullptr;
  HWND rememberRadio_ = nullptr;
  HWND onceRadio_ = nullptr;
  HWND errorLabel_ = nullptr;
  HWND statusLabel_ = nullptr;
  HWND progressBar_ = nullptr;
  HWND progressLabel_ = nullptr;
  HWND ringCaption_ = nullptr;
  HWND candidatesList_ = nullptr;
  HWND vaultCaption_ = nullptr;
  HWND vaultList_ = nullptr;
  HWND useVaultButton_ = nullptr;
  HWND okButton_ = nullptr;
  HWND cancelButton_ = nullptr;

  HFONT bodyFont_ = nullptr;
  HFONT promptFont_ = nullptr;
  HBRUSH cardBrush_ = nullptr;
  HBRUSH fieldBrush_ = nullptr;

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush_;
  prism::win::glass::PanelPainter panel_;

  SubmitHandler onSubmit_;
  OpenedHandler onOpened_;
  CancelHandler onCancel_;
};

// C84: one place that turns a created window into a visible one. The negative
// control PRISM_C84_MUTANT_NO_SHOW compiles the ShowWindow calls out, so the
// visibility assertion in c84-dialog-visible.cmake goes red (D26/D27).
inline void PasswordDialog::ShowCreated() {
  CenterOnOwner();
  if (cardBrush_ == nullptr) {
    cardBrush_ = CreateSolidBrush(ToColorRef(CardPixel()));
  }
  if (fieldBrush_ == nullptr) {
    fieldBrush_ = CreateSolidBrush(ToColorRef(FieldPixel()));
  }
#if !defined(PRISM_C84_MUTANT_NO_SHOW)
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);
#endif
}

// Centered over the owner when the app has one, otherwise over the primary
// work area: the startup gate runs before the gallery exists (measured: the
// dialog used to open at CW_USEDEFAULT and, with no ShowWindow, not at all).
inline void PasswordDialog::CenterOnOwner() {
  if (hwnd_ == nullptr) return;
  RECT selfRect{};
  if (GetWindowRect(hwnd_, &selfRect) == FALSE) return;
  const int width = selfRect.right - selfRect.left;
  const int height = selfRect.bottom - selfRect.top;
  // The CENTER comes from the owner when there is one; the CLAMP always comes
  // from the monitor work area. Mixing the two (clamping to the owner rectangle)
  // puts the card off-screen when the owner itself overlaps the taskbar -- the
  // CI runner's 1024x768 desktop caught exactly that, locally the owner was
  // small enough that both rules agreed.
  RECT base{};
  if (owner_ != nullptr && GetWindowRect(owner_, &base) != FALSE && base.right > base.left) {
    // centered over the owner
  } else if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &base, 0) == FALSE) {
    return;
  }
  int x = base.left + ((base.right - base.left) - width) / 2;
  int y = base.top + ((base.bottom - base.top) - height) / 2;
  MONITORINFO monitor{};
  monitor.cbSize = sizeof(monitor);
  if (GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE) {
    if (x + width > monitor.rcWork.right) x = monitor.rcWork.right - width;
    if (y + height > monitor.rcWork.bottom) y = monitor.rcWork.bottom - height;
    if (x < monitor.rcWork.left) x = monitor.rcWork.left;
    if (y < monitor.rcWork.top) y = monitor.rcWork.top;
  }
  SetWindowPos(hwnd_, HWND_TOP, x, y, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

}  // namespace prism::win::pwd_ui

#endif  // _WIN32
