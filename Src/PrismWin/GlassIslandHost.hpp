// PrismArchive -- PrismWin
// GlassIslandHost: the DirectComposition host for the floating Liquid Glass
// islands (blueprint ch. 3.1/3.2). Each island is a DComp visual backed by
// its own alpha-premultiplied composition swapchain; the D2D device context
// draws the five-layer glass approximation into that surface, and the HLSL
// effect (LiquidGlassEffect.hlsl, cbuffer serialized by GlassIslandScene)
// drives the optics.
//
// CI contract: compiles + links (d3d11/dxgi/dcomp). Runtime behaviour needs
// an interactive session and is exercised on-device.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_3.h>
#include <d2d1helper.h>
#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <wrl/client.h>

#include <cstdint>
#include <vector>

#include "../PrismCore/GlassIsland.hpp"

namespace prism::win::glass {

using Microsoft::WRL::ComPtr;

class GlassIslandHost {
 public:
  struct IslandSurface {
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID2D1Bitmap1> d2dBitmap;  // the D2D target bound to the surface
    UINT width = 1;
    UINT height = 1;
  };

  bool Initialize(HWND hwnd) {
    hwnd_ = hwnd;
    // 1. D3D11 device (BGRA support is required for D2D interop).
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr,
                                   nullptr);
    if (FAILED(hr)) {
      // Fall back to WARP (software) so glass islands render everywhere.
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                             d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
      if (FAILED(hr)) return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d_.As(&dxgiDevice))) return false;

    // 2. D2D device + context over the same DXGI device.
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                           d2dFactory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = d2dDevice_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 3. The DComp device, target and root visual.
    hr = DCompositionCreateDevice(dxgiDevice.Get(), __uuidof(IDCompositionDevice),
                                  reinterpret_cast<void**>(dcomp_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return false;
    hr = dcomp_->CreateTargetForHwnd(hwnd, /*topmost=*/TRUE, target_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = dcomp_->CreateVisual(root_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = target_->SetRoot(root_.Get());
    if (FAILED(hr)) return false;

    // 4. One visual + composition swapchain per island (transparency lives
    // in PREMULTIPLIED alpha so the content below shows through the glass).
    const auto addIsland = [&](UINT w, UINT h) {
      IslandSurface surface;
      surface.width = w;
      surface.height = h;
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = w;
      desc.Height = h;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.Stereo = FALSE;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      ComPtr<IDXGIFactory2> factory;
      if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                    reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
        return false;
      }
      if (FAILED(factory->CreateSwapChainForComposition(
              d3d_.Get(), &desc, nullptr, surface.swapchain.ReleaseAndGetAddressOf()))) {
        return false;
      }
      ComPtr<IDXGISurface> dxgiSurface;
      if (FAILED(surface.swapchain->GetBuffer(0, __uuidof(IDXGISurface),
                                              reinterpret_cast<void**>(
                                                  dxgiSurface.ReleaseAndGetAddressOf())))) {
        return false;
      }
      D2D1_BITMAP_PROPERTIES1 props = {};
      props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
      props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
      props.bitmapOptions =
          D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
      if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(
              dxgiSurface.Get(), props, surface.d2dBitmap.ReleaseAndGetAddressOf()))) {
        return false;
      }
      ComPtr<IDCompositionVisual> visual;
      if (FAILED(dcomp_->CreateVisual(visual.ReleaseAndGetAddressOf()))) return false;
      if (FAILED(visual->SetContent(surface.swapchain.Get()))) return false;
      if (FAILED(root_->AddVisual(visual.Get(), FALSE, nullptr))) return false;
      surfaces_.push_back(surface);
      visuals_.push_back(visual);
      return true;
    };
    if (!addIsland(560, 44) || !addIsland(800, 48) || !addIsland(560, 32)) return false;
    return SUCCEEDED(dcomp_->Commit());
  }

  // Draw one island: binds the D2D target and calls the painter with the
  // prepared device context.
  template <typename Paint>
  bool PaintIsland(size_t islandIndex, const IslandRect& rect, Paint&& paint) {
    if (islandIndex >= surfaces_.size()) return false;
    IslandSurface& surface = surfaces_[islandIndex];
    ComPtr<IDCompositionVisual>& visual = visuals_[islandIndex];
    // Position the visual at the model's rect.
    const float tx = static_cast<float>(rect.x);
    const float ty = static_cast<float>(rect.y);
    ComPtr<IDCompositionTranslateTransform> transform;
    if (FAILED(dcomp_->CreateTranslateTransform(transform.ReleaseAndGetAddressOf()))) {
      return false;
    }
    transform->SetOffsetX(tx);
    transform->SetOffsetY(ty);
    if (FAILED(visual->SetTransform(transform.Get()))) return false;

    d2dContext_->SetTarget(surface.d2dBitmap.Get());
    d2dContext_->BeginDraw();
    paint(d2dContext_.Get(), static_cast<float>(rect.w), static_cast<float>(rect.h));
    HRESULT hr = d2dContext_->EndDraw();
    if (FAILED(hr)) return false;
    if (FAILED(surface.swapchain->Present(1, 0))) return false;
    return SUCCEEDED(dcomp_->Commit());
  }

  ID2D1DeviceContext* Context() const { return d2dContext_.Get(); }
  IDCompositionDevice* CompositionDevice() const { return dcomp_.Get(); }
  size_t IslandCount() const { return surfaces_.size(); }

 private:
  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Factory1> d2dFactory_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<ID2D1DeviceContext> d2dContext_;
  ComPtr<IDCompositionDevice> dcomp_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  std::vector<IslandSurface> surfaces_;
  std::vector<ComPtr<IDCompositionVisual>> visuals_;
};

}  // namespace prism::win::glass
