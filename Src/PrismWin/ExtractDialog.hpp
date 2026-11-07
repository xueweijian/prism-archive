// PrismArchive -- PrismWin
// ExtractDialog: Ctrl+E ASKS before it writes (card C125).
//
// THE MEASURED DEFECT. Ctrl+E resolved to prism::keys::Command::SmartExtract,
// i.e. "derive a target folder and write the whole archive now". Bandizip binds
// the same chord (菜单原文 解压文件(&E)  Ctrl+E) to a dialog the user can cancel.
// One finger gesture, two consequence CLASSES: "asks" vs "writes unasked". Only
// the second is irreversible, so only the second may not be a shortcut.
//
// WHAT THIS FILE OWNS: the window and its three facts. The dialog shows the
// target path (editable, prefilled by the app), the number of files and the
// uncompressed bytes it is about to write, and it returns TWO answers:
// Accepted() plus the path. It never touches the filesystem and never starts an
// extraction -- the plan and the engine stay in the app, which is what makes
// "cancel wrote nothing" a property of the wiring rather than a promise in a
// comment.
//
// WHY THE ELEMENTS ARE PASSED IN AS TEXT. The counts and the size are the app's
// (it has the catalog and the plan); the dialog is not a second authority on
// what will be written. The suite asserts the three elements are ON SCREEN by
// reading the controls, not by trusting this header.
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

#include "../PrismCore/ListLayout.hpp"  // prism::list::FormatBytes (one byte formatter)

namespace prism::win::extractdlg {

// Local transcoder on purpose: PrismWin has several identically named inline
// helpers in unrelated headers, and importing two into one TU is an ODR hazard.
inline std::wstring Widen(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

// "将写入 12 个文件 / 2.0 MB" -- the three facts the card demands are ON SCREEN
// before a byte lands. Built here, from the same plan numbers the extraction
// will use, so the suite asserts the product's string and not a second copy.
inline std::wstring BuildSummary(std::uint64_t fileCount, std::uint64_t bytes) {
  return L"\u5C06\u5199\u5165 " + std::to_wstring(fileCount) +
         L" \u4E2A\u6587\u4EF6 / " + Widen(prism::list::FormatBytes(bytes));
}

struct ExtractDialogModel {
  std::wstring title = L"\u89E3\u538B";  // 解压
  std::wstring targetLabel = L"\u89E3\u538B\u5230\uFF1A";  // 解压到：
  // The destination, EDITABLE. C125 requires it visible: "remember last time and
  // do not show it" is the behaviour this dialog exists to stop.
  std::wstring targetDir;
  // "将写入 12 个文件 / 2.0 MB" -- built by the app from the catalog it owns.
  std::wstring summary;
  std::wstring acceptLabel = L"\u89E3\u538B";   // 解压
  std::wstring cancelLabel = L"\u53D6\u6D88";   // 取消
  std::wstring emptyTargetHint =
      L"\u76EE\u6807\u8DEF\u5F84\u4E0D\u80FD\u4E3A\u7A7A";  // 目标路径不能为空
};

namespace ids {
constexpr int kTargetLabel = 9501;
constexpr int kTarget = 9502;
constexpr int kSummary = 9503;
constexpr int kHint = 9504;
constexpr int kAccept = 9510;
constexpr int kCancel = 9511;
}  // namespace ids

class ExtractDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveExtractDialog";
  static constexpr int kClientWidth = 620;
  static constexpr int kClientHeight = 196;

  // Real HWNDs this process has created. The wiring suite pins "Ctrl+E raises
  // exactly one dialog"; the counter is what makes that a number, not an opinion.
  static std::atomic<std::uint64_t>& Creations() {
    static std::atomic<std::uint64_t> creations{0};
    return creations;
  }

  ExtractDialog() = default;
  ~ExtractDialog() { Destroy(); }
  ExtractDialog(const ExtractDialog&) = delete;
  ExtractDialog& operator=(const ExtractDialog&) = delete;

  void SetModel(ExtractDialogModel model) {
    model_ = std::move(model);
    if (hwnd_ != nullptr) SyncControls();
  }
  const ExtractDialogModel& Model() const { return model_; }

  bool Create(HINSTANCE instance, HWND owner = nullptr) {
    instance_ = instance;
    owner_ = owner;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &ExtractDialog::ProcThunk;
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
    // Centered over the owner when there is one (the app always passes it).
    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) {
      RECT ownerRect{};
      if (GetWindowRect(owner_, &ownerRect) != FALSE) {
        const int width = frame.right - frame.left;
        const int height = frame.bottom - frame.top;
        x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
        y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;
      }
    }
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, model_.title.c_str(),
                            WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_CLIPCHILDREN, x, y,
                            frame.right - frame.left, frame.bottom - frame.top, owner_,
                            nullptr, instance, this);
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

  void Show() {
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    UpdateWindow(hwnd_);
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }
  DWORD lastError() const { return lastError_; }

  bool Finished() const { return finished_; }
  bool Accepted() const { return accepted_; }

  // What the user agreed to. Whitespace is trimmed and a blank path is refused
  // (FinishAccepted keeps the dialog open), so the app can never be handed "".
  std::wstring TargetDir() const { return target_; }

  std::wstring ControlText(int id) const {
    const HWND control = Control(id);
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
  }

  // Test seams: the SAME Finish() a click reaches, so no assertion can pass
  // through a path the product does not have.
  void Accept() { FinishAccepted(); }
  void Cancel() { FinishCancelled(); }
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  // Modal driver: one line at the call site. The owner is disabled for exactly
  // the lifetime of the loop and re-enabled even when the dialog fails to open.
  bool OpenModal(HINSTANCE instance, HWND owner) {
    if (!Create(instance, owner)) return false;
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) EnableWindow(owner_, FALSE);
    Show();
    SetForegroundWindow(hwnd_);
    SetFocus(Control(ids::kAccept));
    MSG msg{};
    while (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
      const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
      if (got <= 0) break;  // WM_QUIT belongs to the app loop, not to this dialog
      if (IsDialogMessageW(hwnd_, &msg) == FALSE) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) EnableWindow(owner_, TRUE);
    return accepted_;
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self =
        reinterpret_cast<ExtractDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<ExtractDialog*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(hwnd, message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  void SyncControls() {
    if (hwnd_ == nullptr) return;
    SetWindowTextW(hwnd_, model_.title.c_str());
    SetWindowTextW(Control(ids::kTargetLabel), model_.targetLabel.c_str());
    SetWindowTextW(Control(ids::kTarget), model_.targetDir.c_str());
    SetWindowTextW(Control(ids::kSummary), model_.summary.c_str());
    SetWindowTextW(Control(ids::kHint), L"");
    SetWindowTextW(Control(ids::kAccept), model_.acceptLabel.c_str());
    SetWindowTextW(Control(ids::kCancel), model_.cancelLabel.c_str());
    InvalidateRect(hwnd_, nullptr, TRUE);
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
    add(L"STATIC", model_.targetLabel.c_str(), staticStyle, 20, 18, 120, 22,
        ids::kTargetLabel);
    add(L"EDIT", model_.targetDir.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_BORDER, 20, 44,
        kClientWidth - 40, 26, ids::kTarget);
    add(L"STATIC", model_.summary.c_str(), staticStyle, 20, 80, kClientWidth - 40, 24,
        ids::kSummary);
    // The one in-place complaint this dialog can raise ("目标路径不能为空").
    add(L"STATIC", L"", staticStyle, 20, 110, kClientWidth - 40, 20, ids::kHint);
    add(L"BUTTON", model_.acceptLabel.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 330, 142, 130, 34,
        ids::kAccept);
    add(L"BUTTON", model_.cancelLabel.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 470, 142, 130, 34,
        ids::kCancel);
  }

  static std::wstring Trim(std::wstring text) {
    const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r'; };
    while (!text.empty() && space(text.back())) text.pop_back();
    std::size_t begin = 0;
    while (begin < text.size() && space(text[begin])) ++begin;
    return text.substr(begin);
  }

  void FinishAccepted() {
    if (finished_) return;
    const std::wstring typed = Trim(
        hwnd_ != nullptr ? ControlText(ids::kTarget) : model_.targetDir);
    if (typed.empty()) {
      // A blank destination is not an answer: keep the dialog up and say why,
      // rather than extracting to "" (which the app would have to refuse).
      SetWindowTextW(Control(ids::kHint), model_.emptyTargetHint.c_str());
      SetFocus(Control(ids::kTarget));
      return;
    }
    finished_ = true;
    accepted_ = true;
    target_ = typed;
    Destroy();
  }

  void FinishCancelled() {
    if (finished_) return;
    finished_ = true;
    accepted_ = false;
    target_.clear();
    Destroy();
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
      case WM_COMMAND: {
        const int id = LOWORD(wparam);
        // IDOK / IDCANCEL are what IsDialogMessage sends for Enter / Esc (and
        // for the BS_DEFPUSHBUTTON); the button ids are the direct-click path.
        // Both are answered here, so neither route can be the only one that works.
        if (id == ids::kAccept || id == IDOK) {
          FinishAccepted();
          return 0;
        }
        if (id == ids::kCancel || id == IDCANCEL) {
          FinishCancelled();
          return 0;
        }
        return 0;
      }
      case WM_KEYDOWN:
        // Fallback for the paths that do not run through IsDialogMessage (the
        // suite's Dispatch, a keystroke delivered straight to this window).
        // Esc cancels -- the promise is that a cancel writes NOTHING -- and
        // Enter takes the visible path as the user's answer.
        if (wparam == VK_ESCAPE) {
          FinishCancelled();
          return 0;
        }
        if (wparam == VK_RETURN) {
          FinishAccepted();
          return 0;
        }
        return 0;
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
  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  DWORD lastError_ = 0;
  bool finished_ = false;
  bool accepted_ = false;
  std::wstring target_;
  ExtractDialogModel model_;
};

}  // namespace prism::win::extractdlg
