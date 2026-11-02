// PrismArchive -- PrismWin
// EditConfirmDialog: the UI half of "edit inside an archive" (card C54).
//
// C51 landed the DECISION (prism::EditPlan) and the EXECUTION
// (prism::win::edit::ApplyEdits), but until this card the product had no door
// to it: the window could list, extract and re-pack, and the user still could
// not delete one entry.
//
// WHAT THIS FILE OWNS
// -------------------
//   1. THE MODEL. BuildEditDialogModel() turns an EditPlan into the exact text a
//      human reads before a rewrite: it must SAY "copy-on-write" and "rewrite
//      the whole archive", and it must show the affected-entry count and the
//      estimated write bytes C51 already computed. No number is re-derived here.
//   2. THE RULE that a cancelled dialog means "nothing was written"
//      (EditMayProceed). The negative control PRISM_C54_MUTANT_CANCEL_STILL_WRITES
//      compiles that rule out, and the suite's "cancel leaves the archive
//      byte-identical" case must go red -- a cancel path that still writes is
//      the worst defect this card could ship.
//   3. THE HWND. A small programmatic dialog (same shape as C18/C35: no .rc
//      template, so every assertion targets a real control) with a title, the
//      warning body, an optional name field (rename / new folder) and OK/Cancel.
//      Delete is the only action without a text field, because it has nothing
//      to type -- it has only the consequence to read.
//
// The dialog decides NOTHING about legality: a refused plan is never shown as a
// confirmable action. The window's entry points and the app's gate come first.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/EditPlan.hpp"
#include "../PrismCore/PathSafety.hpp"

namespace prism::win::editui {

// ---------------------------------------------------------------- transcoders
// Local, exactly like pwd_ui's: PrismWin has several identically named inline
// helpers in unrelated headers, and importing two into one TU is an ODR hazard.
inline std::wstring WideFromUtf8(std::string_view utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

inline std::string Utf8FromWide(std::wstring_view wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
  if (needed <= 0) return std::string();
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                      needed, nullptr, nullptr);
  return utf8;
}

// ---------------------------------------------------------------- the intent
// What the window hands to the app when the user asks for an edit. The window
// knows the archive-relative paths (it owns the catalog and the selection); the
// app owns the confirmation, the engine and the progress HUD.
enum class EditAction : std::uint8_t { Delete, Rename, NewFolder };

inline std::string_view ToString(EditAction action) {
  switch (action) {
    case EditAction::Delete: return "delete";
    case EditAction::Rename: return "rename";
    case EditAction::NewFolder: return "new-folder";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, EditAction action) {
  return os << ToString(action);
}

struct EditIntent {
  EditAction action = EditAction::Delete;
  std::string path;       // the existing entry (delete/rename)
  std::string directory;  // its parent ("" = archive root)
  std::string leaf;       // its current name (rename) / default name (new folder)
  bool isDir = false;
  // Delete: remove the subtree on purpose. The window asks for delete on a
  // folder, and C51 refuses a non-empty folder unless the caller says so.
  bool recurse = false;
};

// ---------------------------------------------------------------- the model
struct EditDialogModel {
  bool ok = false;          // the plan itself is legal
  bool dangerous = false;   // delete / rename: the archive is replaced
  bool needsInput = false;  // rename / new folder
  std::wstring title;
  std::wstring body;
  std::wstring inputLabel;
  std::wstring inputText;
  std::wstring acceptLabel;
  std::wstring cancelLabel = L"\u53D6\u6D88";  // 取消
  uint64_t entriesAffected = 0;
  uint64_t estimatedWriteBytes = 0;
  uint64_t keptFiles = 0;
  bool solidRewrite = false;
  std::wstring archiveName;
};

// Build the sentence the user must be able to read BEFORE the rewrite. The
// numbers are C51's (plan.entriesAffected / plan.estimatedWriteBytes), never a
// second estimate that could disagree with what the engine is about to do.
inline EditDialogModel BuildEditDialogModel(EditAction action, const std::string& subject,
                                            const prism::EditPlan& plan,
                                            const std::wstring& archiveName,
                                            const std::wstring& suggestedInput = {}) {
  EditDialogModel model;
  model.ok = plan.ok;
  model.dangerous = action != EditAction::NewFolder;
  model.needsInput = action != EditAction::Delete;
  model.entriesAffected = plan.entriesAffected;
  model.estimatedWriteBytes = plan.estimatedWriteBytes;
  model.keptFiles = plan.keptFiles;
  model.solidRewrite = plan.solidRewrite;
  model.archiveName = archiveName;

  const std::wstring subjectWide = WideFromUtf8(subject);
  switch (action) {
    case EditAction::Delete:
      model.title = L"\u5220\u9664\u5F52\u6863\u6761\u76EE";  // 删除归档条目
      model.acceptLabel = L"\u5220\u9664";                    // 删除
      break;
    case EditAction::Rename:
      model.title = L"\u91CD\u547D\u540D\u5F52\u6863\u6761\u76EE";  // 重命名归档条目
      model.acceptLabel = L"\u91CD\u547D\u540D";                    // 重命名
      model.inputLabel = L"\u65B0\u540D\u79F0";                     // 新名称
      model.inputText = suggestedInput.empty()
                            ? WideFromUtf8(std::string(prism::FileNameOf(subject)))
                            : suggestedInput;
      break;
    case EditAction::NewFolder:
      model.title = L"\u65B0\u5EFA\u5F52\u6863\u76EE\u5F55";  // 新建归档目录
      model.acceptLabel = L"\u521B\u5EFA";                    // 创建
      model.inputLabel = L"\u76EE\u5F55\u540D\u79F0";         // 目录名称
      model.inputText = suggestedInput.empty()
                            ? std::wstring(L"\u65B0\u5EFA\u6587\u4EF6\u5939")
                            : suggestedInput;
      break;
  }

  // The exact sentence the card requires. The literal "copy-on-write" is kept
  // in ASCII so an audit of the binary finds the promise in the product, too.
  model.body = L"\u8FD9\u4F1A\u7528 copy-on-write \u91CD\u5199\u6574\u4E2A\u5F52\u6863"
               L"\uFF08\u539F\u6587\u4EF6\u5728\u65B0\u5F52\u6863\u6821\u9A8C\u901A\u8FC7"
               L"\u524D\u4E0D\u4F1A\u88AB\u66FF\u6362\uFF09\u3002\n\n";
  model.body += L"\u53D7\u5F71\u54CD\u6761\u76EE\uFF1A" +
                std::to_wstring(plan.entriesAffected) + L"\n";
  model.body += L"\u9884\u4F30\u5199\u5165\u91CF\uFF1A" +
                std::to_wstring(plan.estimatedWriteBytes) + L" \u5B57\u8282\uFF08\u4FDD\u7559 " +
                std::to_wstring(plan.keptFiles) + L" \u4E2A\u6587\u4EF6\uFF09\n";
  if (plan.solidRewrite) {
    model.body += L"\u56FA\u5B9E\u5F52\u6863\uFF1A\u4E00\u6B21\u7F16\u8F91\u4F1A\u91CD\u65B0"
                  L"\u538B\u7F29\u6574\u4E2A\u56FA\u5B9E\u5757\u3002\n";
  }
  model.body += L"\u5F52\u6863\uFF1A" + archiveName + L"\n\n";
  model.body += L"\u5BF9\u8C61\uFF1A" + subjectWide + L"\n";
  if (!plan.ok && !plan.error.empty()) {
    model.body += L"\u62D2\u7EDD\u539F\u56E0\uFF1A" + WideFromUtf8(plan.error) + L"\n";
  }
  return model;
}

// Build the operations the user asked for. EMPTY input is refused by returning
// an empty list (the caller shows "a name is required" instead of submitting a
// malformed edit that C51 would refuse for the wrong reason).
//
// DefaultEditOps is the FIRST plan (the dialog's estimate): rename/new-folder
// start from the current/default name, which is also what the text field shows.
inline std::vector<prism::EditOp> BuildEditOps(const EditIntent& intent,
                                               const std::wstring& input = {}) {
  std::vector<prism::EditOp> ops;
  prism::EditOp op;
  switch (intent.action) {
    case EditAction::Delete:
      op.kind = prism::EditOpKind::Delete;
      op.path = intent.path;
      op.recurse = intent.recurse;
      ops.push_back(std::move(op));
      return ops;
    case EditAction::Rename:
    case EditAction::NewFolder: {
      const std::string name = Utf8FromWide(input);
      if (name.empty()) return ops;
      const std::string joined =
          intent.directory.empty() ? name : intent.directory + "/" + name;
      if (intent.action == EditAction::Rename) {
        op.kind = prism::EditOpKind::Rename;
        op.path = intent.path;
        op.target = joined;
      } else {
        op.kind = prism::EditOpKind::NewFolder;
        op.path = joined;
      }
      ops.push_back(std::move(op));
      return ops;
    }
  }
  return ops;
}

inline std::vector<prism::EditOp> DefaultEditOps(const EditIntent& intent) {
  if (intent.action == EditAction::Delete) return BuildEditOps(intent);
  return BuildEditOps(intent, WideFromUtf8(intent.leaf));
}

// ---------------------------------------------------------------- cancellation
struct EditDialogResult {
  bool accepted = false;
  std::wstring input;
};

// THE RULE: a cancelled dialog writes nothing. The negative control below makes
// this return true for a cancel, which is exactly the defect the suite's
// "cancel leaves the archive byte-identical" assertion exists to catch.
#if defined(PRISM_C54_MUTANT_CANCEL_STILL_WRITES)
// C54 MUTANT ACTIVE: cancel_writes -- a cancelled dialog is treated as accepted,
// so the caller performs the rewrite the user refused.
inline bool EditMayProceed(const EditDialogResult&) { return true; }
#else
inline bool EditMayProceed(const EditDialogResult& result) { return result.accepted; }
#endif

// The single-operation gate the window uses to refuse a second edit, extract or
// preview while one is running (requirement 4). The negative control removes the
// refusal, so the suite's "the second operation is rejected" case goes red.
#if defined(PRISM_C54_MUTANT_ALLOW_CONCURRENT)
// C54 MUTANT ACTIVE: allow_concurrent -- the busy gate no longer refuses.
inline bool ConcurrencyGateActive() { return false; }
#else
inline bool ConcurrencyGateActive() { return true; }
#endif

// ---------------------------------------------------------------- the HWND
namespace ids {
constexpr int kTitle = 5401;
constexpr int kBody = 5402;
constexpr int kInputLabel = 5403;
constexpr int kInput = 5404;
constexpr int kOk = 1;      // == IDOK
constexpr int kCancel = 2;  // == IDCANCEL
}  // namespace ids

class EditConfirmDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveEditConfirmDialog";
  static constexpr int kClientWidth = 660;
  static constexpr int kClientHeight = 400;

  using ResultHandler = std::function<void(const EditDialogResult&)>;

  EditConfirmDialog() = default;
  ~EditConfirmDialog() { Destroy(); }
  EditConfirmDialog(const EditConfirmDialog&) = delete;
  EditConfirmDialog& operator=(const EditConfirmDialog&) = delete;

  bool Create(HINSTANCE instance, const std::wstring& windowTitle = L"\u7F16\u8F91\u5F52\u6863") {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &EditConfirmDialog::ProcThunk;
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
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, windowTitle.c_str(),
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
  DWORD lastError() const { return lastError_; }

  void SetResultHandler(ResultHandler handler) { onResult_ = std::move(handler); }

  // Applies the model to the controls. Safe before Create() (the controls are
  // filled in WM_CREATE from the same model).
  void SetModel(EditDialogModel model) {
    model_ = std::move(model);
    if (hwnd_ == nullptr) return;
    SetWindowTextW(hwnd_, model_.title.c_str());
    SetWindowTextW(Control(ids::kTitle), model_.title.c_str());
    SetWindowTextW(Control(ids::kBody), model_.body.c_str());
    SetWindowTextW(Control(ids::kInputLabel), model_.inputLabel.c_str());
    SetWindowTextW(Control(ids::kInput), model_.inputText.c_str());
    SetWindowTextW(Control(ids::kOk), model_.acceptLabel.c_str());
    SetWindowTextW(Control(ids::kCancel), model_.cancelLabel.c_str());
    const int show = model_.needsInput ? SW_SHOW : SW_HIDE;
    ShowWindow(Control(ids::kInputLabel), show);
    ShowWindow(Control(ids::kInput), show);
    if (model_.needsInput) {
      SetFocus(Control(ids::kInput));
      SendMessageW(Control(ids::kInput), EM_SETSEL, 0, -1);
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  const EditDialogModel& Model() const { return model_; }

  void Show() {
    if (hwnd_ == nullptr) return;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
  }

  // The message entry point. WndProc calls it and the Windows suite calls it
  // too, so a case drives the REAL handler chain without a message pump (the
  // pattern C35's dialog established).
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  // Test seams for the two answers. They call the SAME Finish() the buttons do.
  void PressAccept() { Finish(true); }
  void PressCancel() { Finish(false); }

  std::wstring InputText() const { return ControlText(Control(ids::kInput)); }
  void SetInputText(const std::wstring& text) {
    if (Control(ids::kInput) != nullptr) SetWindowTextW(Control(ids::kInput), text.c_str());
  }
  bool InputVisible() const {
    const HWND edit = Control(ids::kInput);
    return edit != nullptr &&
           (GetWindowLongW(edit, GWL_STYLE) & static_cast<LONG>(WS_VISIBLE)) != 0;
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<EditConfirmDialog*>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<EditConfirmDialog*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->hwnd_ = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->HandleMessage(hwnd, message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  static std::wstring ControlText(HWND control) {
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    if (length <= 0) return std::wstring();
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    const int copied = GetWindowTextW(control, text.data(), length + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
  }

  void CreateControls() {
    const DWORD staticStyle = WS_CHILD | WS_VISIBLE | SS_LEFT;
    const auto font = reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT));
    auto add = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int x, int y,
                   int width, int height, int id) {
      HWND control = CreateWindowExW(0, klass, text, style, x, y, width, height, hwnd_,
                                     reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     instance_, nullptr);
      if (control != nullptr) SendMessageW(control, WM_SETFONT, font, TRUE);
      return control;
    };
    add(L"STATIC", model_.title.c_str(), staticStyle | SS_LEFT, 20, 16, kClientWidth - 40, 28,
        ids::kTitle);
    add(L"STATIC", model_.body.c_str(), staticStyle, 20, 50, kClientWidth - 40, 230,
        ids::kBody);
    add(L"STATIC", model_.inputLabel.c_str(), staticStyle, 20, 292, 110, 22,
        ids::kInputLabel);
    add(L"EDIT", model_.inputText.c_str(),
        WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | ES_LEFT, 135, 290,
        kClientWidth - 175, 24, ids::kInput);
    add(L"BUTTON", model_.acceptLabel.c_str(),
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP, kClientWidth - 250, 344, 105,
        32, ids::kOk);
    add(L"BUTTON", model_.cancelLabel.c_str(),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP, kClientWidth - 130, 344, 105,
        32, ids::kCancel);
    const int show = model_.needsInput ? SW_SHOW : SW_HIDE;
    ShowWindow(Control(ids::kInputLabel), show);
    ShowWindow(Control(ids::kInput), show);
  }

  void Finish(bool accepted) {
    if (finished_) return;
    finished_ = true;
    EditDialogResult result;
    result.accepted = accepted;
    result.input = accepted ? InputText() : std::wstring();
    // The callback may destroy/replace this object; capture the handler first.
    ResultHandler handler = onResult_;
    Destroy();
    if (handler) handler(result);
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        CreateControls();
        return 0;
      case WM_COMMAND: {
        const int id = LOWORD(wparam);
        if (id == ids::kOk) {
          Finish(true);
          return 0;
        }
        if (id == ids::kCancel) {
          Finish(false);
          return 0;
        }
        return 0;
      }
      case WM_CLOSE:
        Finish(false);
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
  EditDialogModel model_;
  ResultHandler onResult_;
};

}  // namespace prism::win::editui
