// PrismArchive -- PrismWin
// OverwriteDialog: the ONE conflict dialog of an extraction (card C94).
//
// BEFORE THIS CARD
// Every conflicting file raised its own MB_YESNOCANCEL box
// ("是=覆盖，否=跳过，取消=另存为 (1)"): a 14-file extraction over 14 existing
// targets asked 14 times and offered no way to answer once.
//
// WHAT THIS FILE OWNS
//   1. THE WINDOW. One programmatic dialog (same shape as C18/C35/C54: no .rc
//      template, so every assertion targets a real control) whose buttons are
//      the whole scope decision: 全部覆盖 / 全部跳过 / 全部重命名, and
//      仅这一个：覆盖 / 跳过 / 另存为, plus 本操作内记住我的选择.
//   2. THE SAFE DISMISSAL. Esc / close = "取消": nothing is written for any
//      conflict (Answer::SkipAll), which is the same promise the old cancel had
//      in the only sense that matters -- the user's files stay untouched.
//   3. THE CREATION COUNTER. Creations() counts real HWNDs this class created.
//      The C94 suite pins it to 1 for a 14-conflict extraction; the
//      PRISM_C94_MUTANT_PER_FILE build (core Session) makes it 14.
//
// The dialog decides NOTHING about the filesystem: it returns a scope answer,
// and prism::overwrite::Session turns that into per-file decisions.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <utility>

#include "../PrismCore/OverwriteSession.hpp"

namespace prism::win::owdlg {

// Local transcoder on purpose: PrismWin has several identically named inline
// helpers in unrelated headers, and importing two into one TU is an ODR hazard.
inline std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

struct ConflictModel {
  std::wstring title = L"\u76EE\u6807\u5DF2\u5B58\u5728";  // 目标已存在
  std::wstring body;                                            // set by BuildConflictModel
  std::wstring hint =
      L"\u9009\u62E9\u5E94\u7528\u5230\u672C\u6B21\u89E3\u538B\u7684"
      L"\u5168\u90E8\u51B2\u7A81\u6587\u4EF6\uFF1B"
      L"\u6309 Esc \u53D6\u6D88 = \u4E00\u5F8B\u4E0D\u8986\u76D6"
      L"\uFF08\u4E0D\u5199\u76D8\uFF09\u3002";  // 选择…全部冲突文件；按 Esc 取消…
  // Button labels, kept as data so the suite can assert the dialog's OWN text.
  std::wstring replaceAll = L"\u5168\u90E8\u8986\u76D6";    // 全部覆盖
  std::wstring skipAll = L"\u5168\u90E8\u8DF3\u8FC7";       // 全部跳过
  std::wstring renameAll = L"\u5168\u90E8\u91CD\u547D\u540D";  // 全部重命名
  std::wstring oneLabel = L"\u4EC5\u8FD9\u4E00\u4E2A\uFF1A";   // 仅这一个：
  std::wstring replaceOne = L"\u8986\u76D6";                  // 覆盖
  std::wstring skipOne = L"\u8DF3\u8FC7";                     // 跳过
  std::wstring renameOne = L"\u53E6\u5B58\u4E3A";            // 另存为
  std::wstring rememberLabel =
      L"\u672C\u64CD\u4F5C\u5185\u8BB0\u4F4F\u6211\u7684\u9009\u62E9";  // 本操作内记住我的选择
};

inline ConflictModel BuildConflictModel(const std::string& utf8Path) {
  ConflictModel model;
  model.body = L"\u76EE\u6807\u5DF2\u5B58\u5728\uFF1A\n" + Widen(utf8Path);  // 目标已存在：
  return model;
}

struct ConflictResult {
  prism::overwrite::Answer answer = prism::overwrite::Answer::SkipAll;
  bool remember = false;   // checkbox state; an "all" answer implies true
  bool cancelled = false;  // Esc / close: no conflict is written
};

namespace ids {
constexpr int kTitle = 9401;
constexpr int kBody = 9402;
constexpr int kHint = 9403;
constexpr int kReplaceAll = 9410;
constexpr int kSkipAll = 9411;
constexpr int kRenameAll = 9412;
constexpr int kOneLabel = 9413;
constexpr int kReplaceOne = 9414;
constexpr int kSkipOne = 9415;
constexpr int kRenameOne = 9416;
constexpr int kRemember = 9417;
}  // namespace ids

class OverwriteDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveOverwriteDialog";
  static constexpr int kClientWidth = 640;
  static constexpr int kClientHeight = 254;

  // How many real conflict windows this process has created. The product path
  // increments it once per dialog, so "14 conflicts -> 1 dialog" is a count of
  // windows, not a count of log lines.
  static std::atomic<std::uint64_t>& Creations() {
    static std::atomic<std::uint64_t> creations{0};
    return creations;
  }

  OverwriteDialog() = default;
  ~OverwriteDialog() { Destroy(); }
  OverwriteDialog(const OverwriteDialog&) = delete;
  OverwriteDialog& operator=(const OverwriteDialog&) = delete;

  bool Create(HINSTANCE instance, const std::wstring& windowTitle = {}) {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &OverwriteDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      lastError_ = GetLastError();
      return false;
    }
    RECT frame{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED, FALSE,
                       WS_EX_DLGMODALFRAME);
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName,
                            windowTitle.empty() ? model_.title.c_str() : windowTitle.c_str(),
                            WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                            frame.bottom - frame.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) {
      lastError_ = GetLastError();
      return false;
    }
    Creations().fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }
  DWORD lastError() const { return lastError_; }

  void SetModel(ConflictModel model) {
    model_ = std::move(model);
    if (hwnd_ == nullptr) return;
    SetWindowTextW(hwnd_, model_.title.c_str());
    SetWindowTextW(Control(ids::kTitle), model_.title.c_str());
    SetWindowTextW(Control(ids::kBody), model_.body.c_str());
    SetWindowTextW(Control(ids::kHint), model_.hint.c_str());
    SetWindowTextW(Control(ids::kReplaceAll), model_.replaceAll.c_str());
    SetWindowTextW(Control(ids::kSkipAll), model_.skipAll.c_str());
    SetWindowTextW(Control(ids::kRenameAll), model_.renameAll.c_str());
    SetWindowTextW(Control(ids::kOneLabel), model_.oneLabel.c_str());
    SetWindowTextW(Control(ids::kReplaceOne), model_.replaceOne.c_str());
    SetWindowTextW(Control(ids::kSkipOne), model_.skipOne.c_str());
    SetWindowTextW(Control(ids::kRenameOne), model_.renameOne.c_str());
    SetWindowTextW(Control(ids::kRemember), model_.rememberLabel.c_str());
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  const ConflictModel& Model() const { return model_; }

  void Show() {
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
  }

  bool Finished() const { return finished_; }
  const ConflictResult& Result() const { return result_; }

  // The message entry point, so a suite can drive the REAL handler chain.
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  bool RememberChecked() const {
    const HWND box = Control(ids::kRemember);
    return box != nullptr &&
           (SendMessageW(box, BM_GETCHECK, 0, 0) & BST_CHECKED) != 0;
  }
  void SetRememberChecked(bool checked) {
    const HWND box = Control(ids::kRemember);
    if (box != nullptr) SendMessageW(box, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
  }

  // Test seam: the SAME Finish() a click reaches, so no case can pass through
  // a path the product does not have.
  void Choose(prism::overwrite::Answer answer) { Finish(answer); }
  void Cancel() { FinishCancelled(); }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self =
        reinterpret_cast<OverwriteDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<OverwriteDialog*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(hwnd, message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void CreateControls() {
    const auto font = reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT));
    auto add = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y,
                   int width, int height, int id) {
      HWND control = CreateWindowExW(0, klass, text, style, x, y, width, height, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     instance_, nullptr);
      if (control != nullptr) SendMessageW(control, WM_SETFONT, font, TRUE);
      return control;
    };
    const DWORD staticStyle = WS_CHILD | WS_VISIBLE | SS_LEFT;
    const DWORD buttonStyle = WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP;
    // No separate headline: the caption is the title, and the body's first line
    // already says 目标已存在 followed by the exact path.
    add(L"STATIC", model_.body.c_str(), staticStyle, 20, 16, kClientWidth - 40, 46,
        ids::kBody);
    add(L"STATIC", model_.hint.c_str(), staticStyle, 20, 64, kClientWidth - 40, 36,
        ids::kHint);
    add(L"BUTTON", model_.replaceAll.c_str(), buttonStyle, 20, 108, 190, 34,
        ids::kReplaceAll);
    add(L"BUTTON", model_.skipAll.c_str(), buttonStyle, 225, 108, 190, 34, ids::kSkipAll);
    add(L"BUTTON", model_.renameAll.c_str(), buttonStyle, 430, 108, 190, 34,
        ids::kRenameAll);
    add(L"STATIC", model_.oneLabel.c_str(), staticStyle, 20, 158, 96, 24,
        ids::kOneLabel);
    add(L"BUTTON", model_.replaceOne.c_str(), buttonStyle, 120, 154, 140, 32,
        ids::kReplaceOne);
    add(L"BUTTON", model_.skipOne.c_str(), buttonStyle, 270, 154, 140, 32,
        ids::kSkipOne);
    add(L"BUTTON", model_.renameOne.c_str(), buttonStyle, 420, 154, 140, 32,
        ids::kRenameOne);
    add(L"BUTTON", model_.rememberLabel.c_str(),
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP, 20, 204, 440, 26,
        ids::kRemember);
    add(L"STATIC", L"Esc = \u53D6\u6D88", staticStyle, 470, 208, 150, 22, -1);
    SetFocus(Control(ids::kReplaceAll));
  }

  void Finish(prism::overwrite::Answer answer) {
    if (finished_) return;
    finished_ = true;
    result_.answer = answer;
    result_.remember = RememberChecked();
    result_.cancelled = false;
    Destroy();
  }

  void FinishCancelled() {
    if (finished_) return;
    finished_ = true;
    result_.answer = prism::overwrite::Answer::SkipAll;
    result_.remember = false;
    result_.cancelled = true;
    Destroy();
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
      case WM_COMMAND: {
        const int id = LOWORD(wparam);
        switch (id) {
          case ids::kReplaceAll: Finish(prism::overwrite::Answer::ReplaceAll); return 0;
          case ids::kSkipAll: Finish(prism::overwrite::Answer::SkipAll); return 0;
          case ids::kRenameAll: Finish(prism::overwrite::Answer::RenameAll); return 0;
          case ids::kReplaceOne: Finish(prism::overwrite::Answer::ReplaceOne); return 0;
          case ids::kSkipOne: Finish(prism::overwrite::Answer::SkipOne); return 0;
          case ids::kRenameOne: Finish(prism::overwrite::Answer::RenameOne); return 0;
          default: return 0;
        }
      }
      case WM_CLOSE:
        FinishCancelled();
        return 0;
      case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  DWORD lastError_ = 0;
  bool finished_ = false;
  ConflictModel model_;
  ConflictResult result_;
};

// The product entry point used by the extraction worker: create, show, pump the
// dialog's own queue until it closes, report. Returns false when no window could
// be created -- the caller then answers Skip (never a silent overwrite).
inline bool AskConflict(const std::string& utf8Path, ConflictResult* out) {
  OverwriteDialog dialog;
  dialog.SetModel(BuildConflictModel(utf8Path));
  if (!dialog.Create(GetModuleHandleW(nullptr))) return false;
  dialog.Show();
  MSG msg{};
  while (!dialog.Finished()) {
    const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
    if (got == 0) {  // WM_QUIT: hand it back to this thread's loop and stop.
      PostQuitMessage(static_cast<int>(msg.wParam));
      break;
    }
    if (got == static_cast<BOOL>(-1)) break;
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (out != nullptr) *out = dialog.Result();
  dialog.Destroy();
  return true;
}

}  // namespace prism::win::owdlg
