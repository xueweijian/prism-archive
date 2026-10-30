// PrismArchive -- PrismWin
// Companion preview window (blueprint ch. 4.1 / milestone M2): the
// non-activating Liquid Glass flyout that shows the inline preview next to the
// Explorer context menu.
//
// Milestone scope: a real top-level message window that consumes the PrismCore
// models (QuickLookPlanner decides WHAT to show, ProgressHud decides the
// phase) and paints through GDI. The DirectComposition + Direct2D optics land
// in M3 -- the window, its lifecycle, the named-event wake protocol and the
// model wiring are the contract that survives that upgrade.
#pragma once

#include <windows.h>
#include <windowsx.h>  // IDC_ARROW etc.

#include <string>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"

namespace prism::win::companion {

// Shared-memory contract with the shell DLL (see PrismShellCommand.cpp):
//   1. the DLL writes the selected archive path into the section,
//   2. signals the wake event,
//   3. this window renders within the 12 ms budget (blueprint ch. 7.2).
constexpr wchar_t kSectionName[] = L"Local\\PrismLiquidPreviewSection";
constexpr wchar_t kWakeEventName[] = L"Local\\PrismLiquidPreviewWakeEvent";

class CompanionWindow {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveCompanionPreview";

  bool RegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, static_cast<LPCWSTR>(IDC_ARROW));
    // No activation on click: the flyout must never steal focus from Explorer.
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClassExW(&wc) != 0;
  }

  bool Create(HINSTANCE instance, int width, int height) {
    hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kClassName,
        L"PrismArchive Preview", WS_POPUP, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        nullptr, nullptr, instance, this);
    return hwnd_ != nullptr;
  }

  // Model inputs -------------------------------------------------------------
  void SetCatalogItem(const prism::ArchiveItem& item, uint64_t previewLimit) {
    const auto decision = prism::QuickLookPlanner::Decide(item, previewLimit);
    kindLine_ = KindLabel(decision.kind);
    if (decision.exceedsLimit) kindLine_ += L"  (large -- preview capped)";
  }

  void SetProgress(prism::ProgressHud& hud, uint64_t nowMs) {
    const auto view = hud.Render(nowMs);
    switch (view.phase) {
      case prism::HudPhase::Hidden:
        progressLine_.clear();
        break;
      case prism::HudPhase::Compact:
        progressLine_ = L"Progress " + std::to_wstring(view.percent) + L"%";
        break;
      case prism::HudPhase::Expanded:
        progressLine_ = WideFromUtf8(view.headline) + L"  " + std::to_wstring(view.percent) +
                        L"%  ETA " + std::to_wstring(static_cast<int>(view.etaSeconds)) + L"s";
        break;
      case prism::HudPhase::Done:
        progressLine_ = L"Done";
        break;
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  void Show() const {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  void Hide() const {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
  }
  HWND Hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CompanionWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE && lparam) {
      self = reinterpret_cast<CompanionWindow*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
      case WM_PAINT:
        self->OnPaint();
        return 0;
      case WM_DESTROY:
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
  }

  void OnPaint() const {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // M3 swaps this for the D2D Liquid Glass pipeline; the text contract stays.
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x20, 0x24, 0x2E));
    DrawTextW(dc, kindLine_.c_str(), -1, &rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
    if (!progressLine_.empty()) {
      RECT below = rc;
      below.top += 28;
      DrawTextW(dc, progressLine_.c_str(), -1, &below, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    EndPaint(hwnd_, &ps);
  }

  static const wchar_t* KindLabel(prism::PreviewKind kind) {
    switch (kind) {
      case prism::PreviewKind::Image: return L"Image preview";
      case prism::PreviewKind::Text: return L"Text preview";
      case prism::PreviewKind::Pdf: return L"PDF preview";
      case prism::PreviewKind::Media: return L"Media preview";
      case prism::PreviewKind::Directory: return L"Folder";
      case prism::PreviewKind::None: return L"No preview available";
    }
    return L"";
  }

  static std::wstring WideFromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(wide, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), wide);
    return out;
  }

  HWND hwnd_ = nullptr;
  std::wstring kindLine_;
  std::wstring progressLine_;
};

}  // namespace prism::win::companion
