// PrismArchive -- PrismWin
// The gallery window (blueprint ch. 4.3 + ch. 5): a Direct2D host that owns
// the HWND, the swap of user events into PrismCore decisions, and NOTHING
// else. Every pixel is painted by GalleryRenderer from a Scene snapshot --
// the window stopped drawing tiles itself when the renderer was adopted
// (batch A): two paint paths for one model is how drift starts.
//
// Threading contract for thumbnails: the sweep worker builds its OWN
// ThumbStore and hands it over via SetThumbs (pointer swap under a mutex).
// Only the UI thread ever calls Lookup (it mutates LRU recency), and it only
// ever touches the swapped-in store -- the worker never writes a store the
// UI can see.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h must not own min/max: PrismCore uses std::min/std::max
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wrl/client.h>

#include <memory>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/SpringAnimator.hpp"
#include "../PrismCore/ThumbStore.hpp"
#include "GalleryRenderer.hpp"

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

class GalleryWindow {
 public:
  // Fixed scroll animation: the blueprint's "panel smooth transition" preset.
  static constexpr SpringPreset kScrollSpring = kSpringFluidSheet;
  // Posted by the sweep worker when a fresh ThumbStore has been swapped in.
  static constexpr UINT kThumbsChanged = WM_APP + 0x1;

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
    RebuildListItems();
    RebuildLayout();
  }

  // Cache-policy handle kept for the compile contract (m3_compile_check); the
  // live pixel store arrives via SetThumbs.
  prism::gallery::ThumbnailLru& Thumbnails() { return thumbnails_; }
  bool ListMode() const { return listMode_; }  // Ctrl+1/2 toggles (ch. 9.1)
  prism::gallery::ViewportWindow& Viewport() { return viewport_; }
  HWND Hwnd() const { return host_.hwnd; }

  // Ownership handoff from the sweep worker (see threading contract above).
  void SetThumbs(std::shared_ptr<prism::gallery::ThumbStore> store) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    thumbs_ = std::move(store);
  }

  void SetBreadcrumb(const std::wstring& text) {
    breadcrumb_ = text;
    Invalidate();
  }

  // ---------- interaction (batch A increment 2) ----------
  // Double-click activates an item; the app decides what that means (v1:
  // smart-extract the whole archive). The window only maps pixels -> index.
  void SetActivateHandler(std::function<void(int)> handler) {
    onActivate_ = std::move(handler);
  }
  void SetSelection(int index) {
    if (selection_ == index) return;
    selection_ = index;
    Invalidate();
  }
  int Selection() const { return selection_; }
  // Progress: the worker posts kProgressMessage (wparam=done, lparam=total
  // bytes); the UI thread feeds the HUD -- single-threaded by rule.
  static constexpr UINT kProgressMessage = WM_APP + 2;
  prism::ProgressHud& Hud() { return hud_; }  // worker drives phases; UI renders

  void Show() {
    if (host_.hwnd) ShowWindow(host_.hwnd, SW_SHOW);
  }

  void Run() {
    Show();
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
    return self ? self->Proc(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  LRESULT Proc(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_SIZE: {
        if (target_) {
          target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
        }
        viewport_.SetViewportSize(static_cast<double>(LOWORD(lparam)),
                                  static_cast<double>(HIWORD(lparam)));
        Invalidate();
        return 0;
      }
      case WM_KEYDOWN: {
        // Blueprint ch. 9.1: Ctrl+1 gallery grid, Ctrl+2 file list.
        if (LOWORD(wparam) == '1' && (GetKeyState(VK_CONTROL) & 0x8000)) {
          listMode_ = false;
          Invalidate();
        } else if (LOWORD(wparam) == '2' && (GetKeyState(VK_CONTROL) & 0x8000)) {
          listMode_ = true;
          Invalidate();
        }
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
      case kThumbsChanged:
        Invalidate();
        return 0;
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
      case WM_LBUTTONDBLCLK: {
        const int idx = ItemAtPoint(lparam);
        if (idx >= 0 && onActivate_) {
          selection_ = idx;
          onActivate_(idx);
          return 0;
        }
        break;
      }
      case WM_LBUTTONDOWN: {
        SetSelection(ItemAtPoint(lparam));  // -1 clears: click on empty space
        return 0;
      }
      case kProgressMessage: {
        hud_.OnProgress(NowMs(), static_cast<uint64_t>(wparam),
                        static_cast<uint64_t>(lparam));
        Invalidate();
        return 0;
      }
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

  int ItemAtPoint(LPARAM lparam) const {
    const double x = static_cast<short>(LOWORD(lparam));
    const double y = static_cast<short>(HIWORD(lparam));
    if (listMode_) {
      const uint32_t count =
          static_cast<uint32_t>(host_.items ? host_.items->size() : 0);
      if (count == 0) return -1;
      const uint32_t row = prism::list::RowAtClamped(
          static_cast<uint32_t>(y), static_cast<uint32_t>(viewport_.Scroll()),
          count);
      const bool inRow = y >= 0.0 && x >= 0.0;
      return inRow ? static_cast<int>(row) : -1;
    }
    return viewport_.IndexAt(x, y);  // -1 outside any tile
  }

  static uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }

  void Render() {
    if (!EnsureDeviceResources()) return;
    if (!rendererReady_) {
      rendererReady_ = SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
      if (!rendererReady_) return;
    }
    target_->BeginDraw();
    GalleryRenderer::Scene scene;
    scene.items = host_.items;
    scene.viewport = &viewport_;
    scene.listMode = listMode_;
    scene.listItems = listMode_ ? &listItems_ : nullptr;
    scene.breadcrumb = breadcrumb_;
    {
      std::lock_guard<std::mutex> lock(thumbMutex_);
      scene.thumbs = thumbs_.get();
    scene.selectedItem = selection_;
    scene.selectedRow = listMode_ ? selection_ : -1;
    scene.hud = hud_.Render(NowMs());
    }
    renderer_.Draw(target_.Get(), scene);
    target_->EndDraw();
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
                                                target_.ReleaseAndGetAddressOf()))) {
    // note: CreateHwndRenderTarget takes a bare **out -- IID_PPV_ARGS would
    // expand to two arguments and break the arity.
      return false;
    }
    return true;
  }

  void RebuildListItems() {
    listItems_.clear();
    if (!host_.items) return;
    listItems_.reserve(host_.items->size());
    for (const auto& item : *host_.items) {
      prism::list::SortItem row;
      row.name = item.path;
      row.size = item.size;
      row.packedSize = item.packSize;
      row.isDir = item.isDir;
      row.encrypted = item.encrypted;
      row.kind = prism::QuickLookPlanner::KindByName(item.path);
      listItems_.push_back(row);
    }
    prism::list::FolderFirstOrder(listItems_);
  }

  void RebuildLayout() { viewport_ = prism::gallery::ViewportWindow(layout_, ItemsCount()); }

  int ItemsCount() const { return host_.items ? static_cast<int>(host_.items->size()) : 0; }

  void Invalidate() { if (host_.hwnd) InvalidateRect(host_.hwnd, nullptr, FALSE); }

  static double NowSeconds() {
    LARGE_INTEGER freq{}, counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
  }

  // ---------------------------------------------------------------- state
  static constexpr int kOverscanRows = 2;
  static constexpr double kWheelScrollPixels = 120.0 * 3.0;

  Host host_;
  prism::gallery::GridLayout layout_{};
  prism::gallery::ViewportWindow viewport_{layout_, 0};
  bool listMode_ = false;  // Ctrl+2: the four-column file list (ch. 3.2)
  prism::gallery::ThumbnailLru thumbnails_{256ull * 1024 * 1024};  // blueprint budget
  std::vector<prism::list::SortItem> listItems_;  // list-mode rows (built from catalog)
  std::wstring breadcrumb_;
  SpringAnimator scrollSpring_{kScrollSpring, 0.0};
  double scrollTarget_ = 0.0;

  GalleryRenderer renderer_;
  bool rendererReady_ = false;
  std::mutex thumbMutex_;
  std::shared_ptr<prism::gallery::ThumbStore> thumbs_;
  std::function<void(int)> onActivate_;
  int selection_ = -1;
  prism::ProgressHud hud_;

  ComPtr<ID2D1Factory> factory_;
  ComPtr<ID2D1HwndRenderTarget> target_;
};

}  // namespace prism::win::gallery
