// PrismArchive -- PrismWin
// The gallery window (blueprint ch. 4.3 + ch. 5): a Direct2D render loop that
// draws EXACTLY what prism::gallery tells it and animates scroll with the
// blueprint springs. All decisions live in PrismCore; this file is a renderer.
//
// M3 scope: grid of rounded-rect placeholder tiles + DirectWrite captions,
// wheel scrolling driven by SpringAnimator (fluid-sheet), DirectWrite glyph
// caching for the 120 fps target. Thumbnail textures arrive from the engine
// bridge (ring-buffer decode) in the next increment -- the cache policy is
// already the PrismCore ThumbnailLru, so the renderer never invents policy.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/SpringAnimator.hpp"

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

class GalleryWindow {
 public:
  // Fixed scroll animation: the blueprint's "panel smooth transition" preset.
  static constexpr SpringPreset kScrollSpring = kSpringFluidSheet;

  struct Host {
    HWND hwnd = nullptr;
    std::vector<prism::ArchiveItem>* items = nullptr;  // catalog snapshot (not owned)
  };

  bool Create(const std::wstring& title, HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &GalleryWindow::ProcThunk;
    wc.hInstance = instance;
    // IDC_ARROW expands to MAKEINTRESOURCEA in a non-UNICODE TU; name the wide
    // integer directly (32512) so the call is ASCII/Unicode-proof.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"PrismGalleryWindow";
    RegisterClassExW(&wc);

    host_.hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1280, 800, nullptr, nullptr, instance, this);
    return host_.hwnd != nullptr && EnsureDeviceResources();
  }

  void SetItems(std::vector<prism::ArchiveItem>* items) {
    host_.items = items;
    RebuildLayout();
  }

  // Cache-policy handle the decode pipeline shares with this window.
  prism::gallery::ThumbnailLru& Thumbnails() { return thumbnails_; }
  prism::gallery::ViewportWindow& Viewport() { return viewport_; }

  void Run() {
    if (!host_.hwnd) return;
    ShowWindow(host_.hwnd, SW_SHOW);
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<GalleryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam) {
      self = reinterpret_cast<GalleryWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->host_.hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Proc(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  LRESULT Proc(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_SIZE: {
        if (target_) {
          D2D1_SIZE_U size = D2D1::SizeU(LOWORD(lparam), HIWORD(lparam));
          target_->Resize(size);
        }
        viewport_.SetViewportSize(static_cast<double>(LOWORD(lparam)),
                                  static_cast<double>(HIWORD(lparam)));
        Invalidate();
        return 0;
      }
      case WM_MOUSEWHEEL: {
        // Target scroll follows the wheel; the spring makes the motion fluid.
        const double lines = static_cast<short>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                             static_cast<double>(WHEEL_DELTA);
        scrollTarget_ += lines * kWheelScrollPixels;
        viewport_.SetScroll(scrollTarget_);
        scrollSpring_.Retarget(viewport_.Scroll(), NowSeconds());
        Invalidate();
        return 0;
      }
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(host_.hwnd, &ps);
        AdvanceScrollSpring();
        Render();
        EndPaint(host_.hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;  // D2D paints the whole client area
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
    }
  }

  void AdvanceScrollSpring() {
    const double now = NowSeconds();
    scrollSpring_.Tick(now);
    if (!scrollSpring_.IsSettled()) {
      viewport_.SetScroll(scrollSpring_.Value());
      Invalidate();  // keep the loop alive until the motion settles
    }
  }

  void Render() {
    if (!EnsureDeviceResources()) return;
    target_->BeginDraw();
    target_->Clear(D2D1::ColorF(0x101418, 1.0f));  // deep glass backdrop
    const auto range = viewport_.Visible(kOverscanRows);
    if (range.Empty() || !host_.items) {
      target_->EndDraw();
      return;
    }
    for (int index = range.first; index <= range.last; ++index) {
      if (index >= static_cast<int>(host_.items->size())) break;
      DrawTile(index, (*host_.items)[index]);
    }
    target_->EndDraw();
  }

  void DrawTile(int index, const prism::ArchiveItem& item) {
    const int col = index % viewport_.Columns();
    const int row = index / viewport_.Columns();
    const float cellH = static_cast<float>(viewport_.CellHeight());
    const float left = static_cast<float>(col) * TileWidth() + kTileInset;
    const float top = static_cast<float>(row) * cellH - static_cast<float>(viewport_.Scroll()) +
                      kTileInset;
    const float right = static_cast<float>((col + 1) * TileWidth()) - kTileInset;
    const float bottom = top + cellH - 2.0f * kTileInset;
    const D2D1_ROUNDED_RECT tile = D2D1::RoundedRect(
        D2D1::RectF(left, top, right, bottom), kTileRadius, kTileRadius);
    target_->FillRoundedRectangle(tile, tileBrush_.Get());

    const std::wstring caption = CaptionOf(item.path);
    if (!caption.empty() && captionFormat_) {
      D2D1_RECT_F layout = D2D1::RectF(left + 8.0f, bottom - 28.0f, right - 8.0f, bottom - 4.0f);
      target_->DrawTextW(caption.c_str(), static_cast<UINT32>(caption.size()), captionFormat_.Get(),
                         layout, textBrush_.Get());
    }
  }

  static std::wstring CaptionOf(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), wide);
    return out;
  }

  bool EnsureDeviceResources() {
    if (target_) return true;
    if (!host_.hwnd) return false;
    RECT rc{};
    GetClientRect(host_.hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return false;
    if (!factory_) {
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   IID_PPV_ARGS(factory_.GetAddressOf())))) {
        return false;
      }
    }
    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory_->CreateHwndRenderTarget(props,
                                                D2D1::HwndRenderTargetProperties(
                                                    host_.hwnd,
                                                    D2D1::SizeU(static_cast<UINT32>(rc.right),
                                                                static_cast<UINT32>(rc.bottom))),
                                                IID_PPV_ARGS(target_.GetAddressOf())))) {
      return false;
    }
    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0x2A3138, 0.92f),
                                              tileBrush_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(target_->CreateSolidColorBrush(D2D1::ColorF(0xE8ECF1, 1.0f),
                                              textBrush_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (!writeFactory_) {
      if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown**>(writeFactory_.GetAddressOf())))) {
        return false;
      }
    }
    if (!captionFormat_) {
      writeFactory_->CreateTextFormat(L"Segoe UI Variable", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                      DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                      13.0f, L"en-US", captionFormat_.ReleaseAndGetAddressOf());
    }
    return true;
  }

  void RebuildLayout() { viewport_ = prism::gallery::ViewportWindow(CurrentLayout(), ItemsCount()); }

  int ItemsCount() const { return host_.items ? static_cast<int>(host_.items->size()) : 0; }

  prism::gallery::GridLayout CurrentLayout() const { return layout_; }

  float TileWidth() const {
    return static_cast<float>(viewport_.ColumnWidth());  // viewport width / columns
  }

  void Invalidate() { if (host_.hwnd) InvalidateRect(host_.hwnd, nullptr, FALSE); }

  static double NowSeconds() {
    LARGE_INTEGER freq{}, counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
  }

  // ---------------------------------------------------------------- state
  static constexpr int kOverscanRows = 2;
  static constexpr float kTileInset = 6.0f;
  static constexpr float kTileRadius = 14.0f;
  static constexpr double kWheelScrollPixels = 120.0 * 3.0;

  Host host_;
  prism::gallery::GridLayout layout_{};
  prism::gallery::ViewportWindow viewport_{layout_, 0};
  prism::gallery::ThumbnailLru thumbnails_{256ull * 1024 * 1024};  // blueprint cache budget
  SpringAnimator scrollSpring_{kScrollSpring, 0.0};
  double scrollTarget_ = 0.0;

  ComPtr<ID2D1Factory> factory_;
  ComPtr<ID2D1HwndRenderTarget> target_;
  ComPtr<ID2D1SolidColorBrush> tileBrush_;
  ComPtr<ID2D1SolidColorBrush> textBrush_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> captionFormat_;
};

}  // namespace prism::win::gallery
