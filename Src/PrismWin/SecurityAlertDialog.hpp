// PrismArchive -- PrismWin
// SecurityAlertDialog: red Liquid Glass modal alert card (blueprint ch. 7.1,
// card C137).
//
// Surfaced whenever a Zip-bomb fuse trips (BombFuse) or AMSI detects a threat
// (AmsiVerdict::Block) or WriteGuard blocks an escape write. Unlike a transient
// status-bar append, this raises an unmissable red glass modal card over the
// owner window (with an offscreen Draw() seam for pixel assertions) and pairs
// with SecurityAlertSink so worker threads can post alerts safely to the UI
// thread (CONTRACTS B9).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "../PrismCore/ThemeTokens.hpp"
#include "GlassPanel.hpp"
#include "Typography.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace prism::win::securityui {

enum class SecurityAlertKind : std::uint8_t { BombFuse, AmsiThreat, WriteGuard };

struct SecurityAlertModel {
  SecurityAlertKind kind = SecurityAlertKind::BombFuse;
  bool critical = false;
  std::wstring title;
  std::wstring headline;
  std::wstring detail;
  std::wstring archiveName;
  std::wstring actionLabel = L"\u786E\u8BA4";  // 确认
  prism::theme::Variant variant = prism::theme::Variant::Midnight;
};

inline std::wstring WidenUtf8(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

inline SecurityAlertModel BuildSecurityAlertModel(SecurityAlertKind kind,
                                                  const std::wstring& archivePath,
                                                  std::string_view detailUtf8,
                                                  bool critical = false,
                                                  prism::theme::Variant variant =
                                                      prism::theme::Variant::Midnight) {
  SecurityAlertModel model;
  model.kind = kind;
  model.critical = critical;
  model.variant = variant;
  const std::size_t slash = archivePath.find_last_of(L"/\\");
  model.archiveName = slash == std::wstring::npos ? archivePath : archivePath.substr(slash + 1);
  const std::wstring detailWide = WidenUtf8(detailUtf8);

  switch (kind) {
    case SecurityAlertKind::BombFuse:
      model.title = L"\u5B89\u5168\u8B66\u62A5 \u2014 \u538B\u7F29\u70B8\u5F39\u7194\u65AD";
      model.headline = critical
          ? L"\u5DF2\u7D27\u6025\u4E2D\u6B62\u89E3\u538B\uFF1A\u68C0\u6D4B\u5230\u9AD8\u5371\u81A8\u80C0\u7387\u4E14\u903C\u8FD1\u78C1\u76D8\u4E0A\u9650"
          : L"\u5DF2\u4E2D\u6B62\u89E3\u538B\uFF1A\u89E6\u53D1\u538B\u7F29\u70B8\u5F39\u7194\u65AD\u4FDD\u62A4";
      break;
    case SecurityAlertKind::AmsiThreat:
      model.title = L"\u5B89\u5168\u8B66\u62A5 \u2014 AMSI \u68C0\u6D4B\u5230\u5A01\u80C1";
      model.headline = L"\u5DF2\u62E6\u622A\u5E76\u6E05\u9664\u53EF\u7591\u53EF\u6267\u884C\u8F7D\u8377";
      break;
    case SecurityAlertKind::WriteGuard:
      model.title = L"\u5B89\u5168\u8B66\u62A5 \u2014 \u8DEF\u5F84\u8D8A\u754C\u62E6\u622A";
      model.headline = L"\u5DF2\u963B\u6B62\u5411\u76EE\u6807\u76EE\u5F55\u4E4B\u5916\u5199\u5165\u6587\u4EF6";
      break;
  }

  std::wstring body;
  if (!model.archiveName.empty()) {
    body += L"\u5F52\u6863\uFF1A" + model.archiveName + L"\n";  // 归档：
  }
  if (!detailWide.empty()) {
    body += L"\u8BE6\u60C5\uFF1A" + detailWide;  // 详情：
  } else {
    body += L"\u8BE6\u60C5\uFF1A\u5DF2\u89E6\u53D1\u5B89\u5168\u9632\u62A4\u7B56\u7565\u5E76\u963B\u6B62\u64CD\u4F5C\u3002";
  }
  model.detail = std::move(body);
  return model;
}

namespace ids {
constexpr int kTitle = 9601;
constexpr int kHeadline = 9602;
constexpr int kDetail = 9603;
constexpr int kConfirm = 9610;
}  // namespace ids

class SecurityAlertDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveSecurityAlertDialog";
  static constexpr int kClientWidth = 540;
  static constexpr int kClientHeight = 248;

  // Semantic danger red tokens (crimson Liquid Glass alert).
  static constexpr prism::theme::Rgba kDangerAccent{1.0f, 0.231f, 0.188f, 1.0f};      // #FF3B30
  static constexpr prism::theme::Rgba kDangerGlassWash{0.72f, 0.09f, 0.11f, 0.34f};
  static constexpr prism::theme::Rgba kDangerRim{1.0f, 0.38f, 0.34f, 0.90f};
  static constexpr prism::theme::Rgba kDangerButtonHover{1.0f, 0.32f, 0.28f, 0.95f};

  static std::atomic<std::uint64_t>& Creations() {
    static std::atomic<std::uint64_t> creations{0};
    return creations;
  }

  static SecurityAlertModel& LastPostedModel() {
    static SecurityAlertModel last;
    return last;
  }

  SecurityAlertDialog() = default;
  ~SecurityAlertDialog() { Destroy(); }
  SecurityAlertDialog(const SecurityAlertDialog&) = delete;
  SecurityAlertDialog& operator=(const SecurityAlertDialog&) = delete;

  void SetModel(SecurityAlertModel model) {
    model_ = std::move(model);
    if (hwnd_ != nullptr) {
      SetWindowTextW(hwnd_, model_.title.c_str());
      if (HWND h = Control(ids::kTitle)) SetWindowTextW(h, model_.title.c_str());
      if (HWND h = Control(ids::kHeadline)) SetWindowTextW(h, model_.headline.c_str());
      if (HWND h = Control(ids::kDetail)) SetWindowTextW(h, model_.detail.c_str());
      if (HWND h = Control(ids::kConfirm)) SetWindowTextW(h, model_.actionLabel.c_str());
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }

  const SecurityAlertModel& Model() const { return model_; }

  bool Create(HINSTANCE instance, HWND owner = nullptr) {
    instance_ = instance;
    owner_ = owner;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SecurityAlertDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      lastError_ = GetLastError();
      return false;
    }

    RECT frame{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED, FALSE,
                       WS_EX_DLGMODALFRAME | WS_EX_TOPMOST);
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
    const std::wstring caption =
        model_.title.empty() ? std::wstring(L"\u5B89\u5168\u8B66\u62A5") : model_.title;
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_TOPMOST, kClassName, caption.c_str(),
                            WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_CLIPCHILDREN,
                            x, y, frame.right - frame.left, frame.bottom - frame.top,
                            owner_, nullptr, instance, this);
    if (hwnd_ == nullptr) {
      lastError_ = GetLastError();
      return false;
    }
    Creations().fetch_add(1, std::memory_order_relaxed);
    return true;
  }

  void Destroy() {
    target_.Reset();
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }
  bool Acknowledged() const { return acknowledged_; }
  DWORD lastError() const { return lastError_; }

  bool OpenModal(HINSTANCE instance, HWND owner = nullptr) {
    if (!Create(instance, owner)) return false;
    ShowWindow(hwnd_, SW_SHOWNORMAL);
    SetForegroundWindow(hwnd_);
    UpdateWindow(hwnd_);
    const bool hadOwner = owner != nullptr && IsWindow(owner) != FALSE && IsWindowEnabled(owner) != FALSE;
    if (hadOwner) EnableWindow(owner, FALSE);
    MSG msg{};
    while (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE && !done_) {
      const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
      if (got <= 0) break;
      if (msg.message == WM_KEYDOWN && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
        acknowledged_ = true;
        done_ = true;
        Destroy();
        break;
      }
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    if (hadOwner && IsWindow(owner) != FALSE) {
      EnableWindow(owner, TRUE);
      SetForegroundWindow(owner);
    }
    return acknowledged_;
  }

  // Draws the red Liquid Glass alert card onto any D2D render target (window or
  // offscreen WIC bitmap for deterministic pixel assertions).
  HRESULT Draw(ID2D1RenderTarget* target, float width, float height) {
    if (!target || width <= 0.0f || height <= 0.0f) return E_INVALIDARG;
    if (FAILED(EnsureFactories())) return E_FAIL;

    const prism::theme::Variant v = model_.variant;
    const prism::theme::Rgba canvas =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceCanvasBase, v).color;
    const prism::theme::Rgba island =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceFloatingIsland, v).color;
    const prism::theme::Rgba textPrimary =
        prism::theme::Lookup(prism::theme::TokenId::TextPrimary, v).color;
    const prism::theme::Rgba textSecondary =
        prism::theme::Lookup(prism::theme::TokenId::TextSecondary, v).color;

    target->Clear(ToD2D(canvas));

    const D2D1_RECT_F cardRect = D2D1::RectF(14.0f, 14.0f, width - 14.0f, height - 14.0f);
    Microsoft::WRL::ComPtr<ID2D1Factory> targetFactory;
    target->GetFactory(targetFactory.GetAddressOf());
    if (!targetFactory) targetFactory = d2dFactory_;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> cardPath;
    if (FAILED(prism::win::glass::CreateSquirclePath(targetFactory.Get(), cardRect, 18.0f,
                                                     cardPath.GetAddressOf()))) {
      return E_FAIL;
    }

    // Translucent glass base + crimson danger tint + specular red rim.
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> baseBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> redWashBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> rimBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> accentBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> titleBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> bodyBrush;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> btnTextBrush;

    const prism::theme::Rgba baseFill(island.r, island.g, island.b, 0.84f);
    if (FAILED(target->CreateSolidColorBrush(ToD2D(baseFill), baseBrush.GetAddressOf()))) return E_FAIL;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(kDangerGlassWash), redWashBrush.GetAddressOf()))) return E_FAIL;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(kDangerRim), rimBrush.GetAddressOf()))) return E_FAIL;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(buttonHovered_ ? kDangerButtonHover : kDangerAccent),
                                             accentBrush.GetAddressOf()))) {
      return E_FAIL;
    }
    if (FAILED(target->CreateSolidColorBrush(ToD2D(textPrimary), titleBrush.GetAddressOf()))) return E_FAIL;
    if (FAILED(target->CreateSolidColorBrush(ToD2D(textSecondary), bodyBrush.GetAddressOf()))) return E_FAIL;
    if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                                             btnTextBrush.GetAddressOf()))) {
      return E_FAIL;
    }

    target->FillGeometry(cardPath.Get(), baseBrush.Get());
    target->FillGeometry(cardPath.Get(), redWashBrush.Get());
    target->DrawGeometry(cardPath.Get(), rimBrush.Get(), 1.5f);

    // Top crimson specular highlight seam.
    target->DrawLine(D2D1::Point2F(cardRect.left + 18.0f, cardRect.top + 1.0f),
                     D2D1::Point2F(cardRect.right - 18.0f, cardRect.top + 1.0f),
                     rimBrush.Get(), 1.5f);

    // Red circular warning badge on the top-left of the card.
    const D2D1_ELLIPSE badge = D2D1::Ellipse(D2D1::Point2F(cardRect.left + 32.0f, cardRect.top + 36.0f),
                                             14.0f, 14.0f);
    target->FillEllipse(badge, accentBrush.Get());
    if (titleFormat_) {
      const D2D1_RECT_F bangRect = D2D1::RectF(cardRect.left + 18.0f, cardRect.top + 22.0f,
                                               cardRect.left + 46.0f, cardRect.top + 50.0f);
      titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      target->DrawText(L"!", 1, titleFormat_.Get(), bangRect, btnTextBrush.Get());
      titleFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }

    // Headline + title + detail.
    const float textLeft = cardRect.left + 58.0f;
    const float textRight = cardRect.right - 20.0f;
    if (titleFormat_) {
      const D2D1_RECT_F titleRect = D2D1::RectF(textLeft, cardRect.top + 18.0f, textRight,
                                                cardRect.top + 44.0f);
      const std::wstring& titleText =
          model_.title.empty() ? std::wstring(L"\u5B89\u5168\u8B66\u62A5") : model_.title;
      target->DrawText(titleText.c_str(), static_cast<UINT32>(titleText.size()),
                       titleFormat_.Get(), titleRect, titleBrush.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    if (bodyFormat_) {
      const D2D1_RECT_F headRect = D2D1::RectF(textLeft, cardRect.top + 48.0f, textRight,
                                               cardRect.top + 92.0f);
      target->DrawText(model_.headline.c_str(), static_cast<UINT32>(model_.headline.size()),
                       bodyFormat_.Get(), headRect, titleBrush.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);

      const D2D1_RECT_F detailRect = D2D1::RectF(textLeft, cardRect.top + 94.0f, textRight,
                                                 cardRect.bottom - 56.0f);
      target->DrawText(model_.detail.c_str(), static_cast<UINT32>(model_.detail.size()),
                       bodyFormat_.Get(), detailRect, bodyBrush.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // Red glass "确认" action button.
    buttonRect_ = D2D1::RectF(cardRect.right - 120.0f, cardRect.bottom - 48.0f,
                               cardRect.right - 20.0f, cardRect.bottom - 14.0f);
    const D2D1_ROUNDED_RECT btnShape = D2D1::RoundedRect(buttonRect_, 10.0f, 10.0f);
    target->FillRoundedRectangle(btnShape, accentBrush.Get());
    if (bodyFormat_) {
      bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
      const std::wstring& label =
          model_.actionLabel.empty() ? std::wstring(L"\u786E\u8BA4") : model_.actionLabel;
      target->DrawText(label.c_str(), static_cast<UINT32>(label.size()),
                       bodyFormat_.Get(), buttonRect_, btnTextBrush.Get());
      bodyFormat_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      bodyFormat_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    return S_OK;
  }

 private:
  static D2D1_COLOR_F ToD2D(const prism::theme::Rgba& c) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a);
  }

  HRESULT EnsureFactories() {
    if (!d2dFactory_) {
      const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                           d2dFactory_.GetAddressOf());
      if (FAILED(hr)) return hr;
    }
    if (!dwriteFactory_) {
      const HRESULT hr = DWriteCreateFactory(
          DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
          reinterpret_cast<IUnknown**>(dwriteFactory_.GetAddressOf()));
      if (FAILED(hr)) return hr;
    }
    if (!titleFormat_ && dwriteFactory_) {
      prism::win::typography::CreateTextFormat(dwriteFactory_.Get(), 15.0f,
                                               DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                               DWRITE_FONT_STYLE_NORMAL, L"en-US",
                                               titleFormat_.GetAddressOf());
    }
    if (!bodyFormat_ && dwriteFactory_) {
      prism::win::typography::CreateTextFormat(dwriteFactory_.Get(), 13.0f,
                                               DWRITE_FONT_WEIGHT_NORMAL,
                                               DWRITE_FONT_STYLE_NORMAL, L"en-US",
                                               bodyFormat_.GetAddressOf());
    }
    return S_OK;
  }

  void CreateHiddenAdapterControls(HWND hwnd) {
    // Hidden accessible adapter controls so UIA/Win32 inspection can read exact
    // strings even while Direct2D paints the glass surface.
    CreateWindowExW(0, L"STATIC", model_.title.c_str(), WS_CHILD, 0, 0, 1, 1, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kTitle)), instance_, nullptr);
    CreateWindowExW(0, L"STATIC", model_.headline.c_str(), WS_CHILD, 0, 0, 1, 1, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kHeadline)), instance_, nullptr);
    CreateWindowExW(0, L"STATIC", model_.detail.c_str(), WS_CHILD, 0, 0, 1, 1, hwnd,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kDetail)), instance_, nullptr);
    CreateWindowExW(0, L"BUTTON", model_.actionLabel.c_str(), WS_CHILD | BS_DEFPUSHBUTTON,
                    0, 0, 1, 1, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kConfirm)),
                    instance_, nullptr);
  }

  void PaintWindow() {
    PAINTSTRUCT ps{};
    BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
    const UINT h = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));
    if (SUCCEEDED(EnsureFactories())) {
      if (!target_) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
            D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(w, h));
        d2dFactory_->CreateHwndRenderTarget(props, hwndProps, target_.GetAddressOf());
      } else {
        target_->Resize(D2D1::SizeU(w, h));
      }
      if (target_) {
        target_->BeginDraw();
        Draw(target_.Get(), static_cast<float>(w), static_cast<float>(h));
        if (target_->EndDraw() == D2DERR_RECREATE_TARGET) {
          target_.Reset();
        }
      }
    }
    EndPaint(hwnd_, &ps);
  }

  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SecurityAlertDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<SecurityAlertDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      if (self != nullptr) self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
  }

  LRESULT Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        CreateHiddenAdapterControls(hwnd);
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_PAINT:
        PaintWindow();
        return 0;
      case WM_MOUSEMOVE: {
        const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
        const bool inside = x >= buttonRect_.left && x <= buttonRect_.right &&
                            y >= buttonRect_.top && y <= buttonRect_.bottom;
        if (inside != buttonHovered_) {
          buttonHovered_ = inside;
          InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
      }
      case WM_LBUTTONDOWN: {
        const float x = static_cast<float>(static_cast<short>(LOWORD(lparam)));
        const float y = static_cast<float>(static_cast<short>(HIWORD(lparam)));
        if (x >= buttonRect_.left && x <= buttonRect_.right &&
            y >= buttonRect_.top && y <= buttonRect_.bottom) {
          acknowledged_ = true;
          done_ = true;
          Destroy();
          return 0;
        }
        return 0;
      }
      case WM_COMMAND:
        if (LOWORD(wparam) == ids::kConfirm) {
          acknowledged_ = true;
          done_ = true;
          Destroy();
          return 0;
        }
        break;
      case WM_CLOSE:
        acknowledged_ = true;
        done_ = true;
        Destroy();
        return 0;
      case WM_DESTROY:
        target_.Reset();
        hwnd_ = nullptr;
        done_ = true;
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  DWORD lastError_ = 0;
  bool acknowledged_ = false;
  bool done_ = false;
  bool buttonHovered_ = false;
  SecurityAlertModel model_;
  D2D1_RECT_F buttonRect_{};
  Microsoft::WRL::ComPtr<ID2D1Factory> d2dFactory_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> titleFormat_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> bodyFormat_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
};

// Message-only sink owned by the UI thread so background extraction workers
// can post red glass security alerts without touching UI objects (CONTRACTS B9).
class SecurityAlertSink {
 public:
  static constexpr UINT kAlertMessage = WM_APP + 0x41;
  using Handler = std::function<void(const SecurityAlertModel&)>;

  SecurityAlertSink() = default;
  ~SecurityAlertSink() { Destroy(); }
  SecurityAlertSink(const SecurityAlertSink&) = delete;
  SecurityAlertSink& operator=(const SecurityAlertSink&) = delete;

  bool Create(HINSTANCE instance, HWND owner, Handler handler = {}) {
    instance_ = instance;
    owner_ = owner;
    handler_ = std::move(handler);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &SecurityAlertSink::ProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = L"PrismSecurityAlertSink";
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }
    hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, instance, this);
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  int alertsDelivered() const { return delivered_.load(std::memory_order_relaxed); }

  bool Post(const SecurityAlertModel& model) const {
    if (hwnd_ == nullptr) return false;
    auto* heap = new SecurityAlertModel(model);
    if (PostMessageW(hwnd_, kAlertMessage, 0, reinterpret_cast<LPARAM>(heap)) == FALSE) {
      delete heap;
      return false;
    }
    return true;
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SecurityAlertSink*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<SecurityAlertSink*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      if (self != nullptr) self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->Handle(hwnd, message, wparam, lparam);
  }

  LRESULT Handle(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == kAlertMessage) {
      std::unique_ptr<SecurityAlertModel> payload(
          reinterpret_cast<SecurityAlertModel*>(lparam));
      if (payload) {
        delivered_.fetch_add(1, std::memory_order_relaxed);
        SecurityAlertDialog::LastPostedModel() = *payload;
        if (handler_) {
          handler_(*payload);
        } else {
          SecurityAlertDialog dialog;
          dialog.SetModel(*payload);
          dialog.OpenModal(instance_, owner_);
        }
      }
      return 0;
    }
    if (message == WM_DESTROY) {
      hwnd_ = nullptr;
      return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
  }

  HINSTANCE instance_ = nullptr;
  HWND owner_ = nullptr;
  HWND hwnd_ = nullptr;
  Handler handler_;
  std::atomic<int> delivered_{0};
};

}  // namespace prism::win::securityui
