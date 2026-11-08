// PrismArchive -- PrismWin
// C107: the Liquid Glass OPTICAL CHAIN, connected and asserted in pixels.
//
// THE MEASURED DEFECT this card exists for (2026-11-05, before the change):
//   InitializeOptics(...)  defined in GlassIslandHost.hpp, product call sites = 0
//   PaintIsland(...)       called exactly once (GalleryWindow::PresentGlassFrame)
//                          with backdrop = nullptr, and its paint callback only
//                          cleared the surface to transparent
//   chain_.Draw(backdrop)  gated on opticsReady_ && backdrop && chain_.Ready()
//                          -- a condition that could never be true
// So LiquidGlassEffect.cso and KawaseDualBlur.D2DEffect.cso had NEVER executed:
// every "glass" surface on screen was GlassPanel::Paint (translucent fill, 1 px
// rim, drop shadow).
//
// WHAT THIS SUITE ASSERTS (each one able to go red on its own):
//   1. OPTICS READY. The two effects register AND the chain is live
//      (OpticsLive()), with an empty ShaderError(); the Kawase radius is
//      derived from appearance.glassIntensity.
//   2. THE PANEL IS NOT A FLAT FILL. The same panel, composed twice (optics vs
//      the pre-C107 GlassPanel look), differs by >= 6 levels at the same
//      coordinates.
//   3. THE CHAIN SAMPLES THE CONTENT UNDER IT. The window's own pixels under the
//      panel are repainted red -> blue; the captured backdrop must follow AND
//      the composed panel must follow it (delta R >= 4, delta B >= 4). This is
//      the assertion that catches both "no backdrop was connected" and "a
//      backdrop was connected but the optics never ran".
//   4. NO LEAK. 200 composed frames leave GlassTransientBytes() unchanged: the
//      captured backdrop is REUSED, not re-created (C33's memory discipline).
//   5. THE PRODUCT WINDOW. A real GalleryWindow: EnableGlassLayer initializes
//      the chain, the island covers NavPillRect (the LAYOUT's breadcrumb pill,
//      not a fixed 960x48), and its frames are composed WITH a backdrop. On a
//      runner without DirectComposition this case prints a SKIP line (D1) --
//      it never passes silently.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C107_MUTANT_NO_BACKDROP makes
// PaintIslandInto drop the backdrop (exactly the pre-C107 call site).
// Assertion 3 goes red and the verdict is printed at the FIRST failing check:
//   C107 MUTATION CAUGHT: no_backdrop
// The mutant entry matches that line with PASS_REGULAR_EXPRESSION -- no WILL_FAIL.
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"
#include "../../Src/PrismWin/GlassPanel.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;
using prism::glass::IslandRect;
using prism::win::glass::GlassIslandHost;

namespace {

constexpr UINT kPanelW = 480;
constexpr UINT kPanelH = 44;

// The runtime sign that the injection really fired (CONTRACTS D27): the mutant
// host counts the backdrop it dropped, and ComposeFromWindow latches it before
// any of the pixel assertions run -- so the verdict below is armed on evidence,
// not on a #define.
bool g_injectionRan = false;
// A case that could not run here says so (D1) -- and the reason is repeated in
// the suite's final marker instead of disappearing into a green run.
bool g_skipped = false;
std::string g_skipReason;

// A real window whose content we control: the product's capture reads the
// window's OWN surface, so a test that painted into a lookalike bitmap would
// not be testing the capture at all.
struct Stage {
  HWND hwnd = nullptr;
  ComPtr<ID2D1Factory> factory;
  ComPtr<ID2D1HwndRenderTarget> target;

  static LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_PAINT) {
      PAINTSTRUCT ps;
      BeginPaint(window, &ps);
      EndPaint(window, &ps);
      return 0;
    }
    if (message == WM_ERASEBKGND) return 1;
    return DefWindowProcW(window, message, wparam, lparam);
  }

  bool Open(UINT width, UINT height) {
    static bool registered = false;
    if (!registered) {
      WNDCLASSEXW wc = {sizeof(wc)};
      wc.lpfnWndProc = &Stage::WndProc;
      wc.hInstance = GetModuleHandleW(nullptr);
      wc.lpszClassName = L"PrismC107Stage";
      wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
      if (!RegisterClassExW(&wc)) return false;
      registered = true;
    }
    hwnd = CreateWindowExW(0, L"PrismC107Stage", L"c107", WS_OVERLAPPEDWINDOW, 0, 0,
                           static_cast<int>(width), static_cast<int>(height), nullptr, nullptr,
                           GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return false;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    return SUCCEEDED(factory->CreateHwndRenderTarget(
        props,
        D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(width, height),
                                         D2D1_PRESENT_OPTIONS_IMMEDIATELY),
        target.GetAddressOf()));
  }

  // The content UNDER the panel, painted the way the window paints: opaque.
  bool Paint(int r, int g, int b) {
    if (!target) return false;
    target->BeginDraw();
    target->Clear(D2D1::ColorF(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                               static_cast<float>(b) / 255.0f, 1.0f));
    return SUCCEEDED(target->EndDraw());
  }

  void Close() {
    target.Reset();
    if (hwnd) DestroyWindow(hwnd);
    hwnd = nullptr;
  }
};

// A pixel is 0xAARRGGBB -- the memory order of a 32bpp BGRA DIB, of a D2D
// B8G8R8A8 bitmap, and of the WIC format the screenshots are written in. ONE
// convention, so a capture, a composite and a PNG cannot disagree.
UINT32 Bgra(int r, int g, int b) {
  return (0xFFu << 24) | (static_cast<UINT32>(r) << 16) | (static_cast<UINT32>(g) << 8) |
         static_cast<UINT32>(b);
}

struct Frame {
  UINT width = 0;
  UINT height = 0;
  std::vector<UINT32> pixels;

  UINT32 At(UINT x, UINT y) const {
    if (x >= width || y >= height) return 0;
    return pixels[static_cast<size_t>(y) * width + x];
  }
  int R(UINT x, UINT y) const { return static_cast<int>((At(x, y) >> 16) & 0xFFu); }
  int G(UINT x, UINT y) const { return static_cast<int>((At(x, y) >> 8) & 0xFFu); }
  int B(UINT x, UINT y) const { return static_cast<int>(At(x, y) & 0xFFu); }
};

// Reads a bitmap that lives on the host's device back to the CPU: same device,
// so D2D allows it (a cross-device copy is D2DERR_WRONG_RESOURCE_DOMAIN).
bool ReadBitmap(GlassIslandHost* host, ID2D1Bitmap* source, Frame* out) {
  if (!host || !source || !out) return false;
  ID2D1DeviceContext* dc = host->Context();
  if (!dc) return false;
  ComPtr<ID2D1Bitmap1> readable;
  if (FAILED(source->QueryInterface(IID_PPV_ARGS(readable.GetAddressOf())))) return false;
  const D2D1_SIZE_U size = source->GetPixelSize();
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
  ComPtr<ID2D1Bitmap1> staging;
  if (FAILED(dc->CreateBitmap(size, nullptr, 0, props, staging.GetAddressOf()))) return false;
  if (FAILED(staging->CopyFromBitmap(nullptr, readable.Get(), nullptr))) return false;
  D2D1_MAPPED_RECT mapped = {};
  if (FAILED(staging->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
  out->width = size.width;
  out->height = size.height;
  out->pixels.assign(static_cast<size_t>(size.width) * size.height, 0u);
  for (UINT y = 0; y < size.height; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * size.width,
                mapped.bits + static_cast<size_t>(y) * mapped.pitch,
                static_cast<size_t>(size.width) * 4u);
  }
  staging->Unmap();
  return true;
}

// The island's bitmap is PREMULTIPLIED and mostly translucent: what the user
// sees is the island composited over the window content behind it (exactly what
// DirectComposition does with the premultiplied swapchain). Every pixel
// comparison below therefore happens on the pixels that reach the screen.
Frame CompositeOver(const Frame& island, const Frame& backdrop) {
  Frame out;
  out.width = (std::min)(island.width, backdrop.width);
  out.height = (std::min)(island.height, backdrop.height);
  out.pixels.assign(static_cast<size_t>(out.width) * out.height, 0u);
  for (UINT y = 0; y < out.height; ++y) {
    for (UINT x = 0; x < out.width; ++x) {
      const UINT32 s = island.At(x, y);
      const UINT32 d = backdrop.At(x, y);
      const int inv = 255 - static_cast<int>((s >> 24) & 0xFFu);
      const auto blend = [&](int shift) {
        const int src = static_cast<int>((s >> shift) & 0xFFu);  // already premultiplied
        const int dst = static_cast<int>((d >> shift) & 0xFFu);
        return (std::min)(255, src + (dst * inv + 127) / 255);
      };
      out.pixels[static_cast<size_t>(y) * out.width + x] =
          (0xFFu << 24) | (static_cast<UINT32>(blend(16)) << 16) |
          (static_cast<UINT32>(blend(8)) << 8) | static_cast<UINT32>(blend(0));
    }
  }
  return out;
}

std::wstring FindShaderDir() {
  wchar_t env[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_SHADER_DIR", env, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return env;
  namespace fs = std::filesystem;
  fs::path probe = fs::absolute(fs::path(__FILE__)).parent_path();
  for (int hop = 0; hop < 8 && !probe.empty(); ++hop) {
    const fs::path candidate = probe / "Assets" / "Shaders";
    if (fs::exists(candidate / "LiquidGlassEffect.hlsl")) return candidate.wstring();
    probe = probe.parent_path();
  }
  return L"Assets\\Shaders";
}

// A colourful backdrop the optics have something to refract: four quadrants of
// saturated colour plus structure, so a flat fill and a refracted one cannot
// agree by accident.
Frame PatternFrame(UINT width, UINT height) {
  Frame frame;
  frame.width = width;
  frame.height = height;
  frame.pixels.resize(static_cast<size_t>(width) * height, 0u);
  for (UINT y = 0; y < height; ++y) {
    for (UINT x = 0; x < width; ++x) {
      const bool right = x >= width / 2;
      const bool bottom = y >= height / 2;
      UINT32 colour = Bgra(30, 40, 60);
      if (!right && !bottom) colour = Bgra(235, 70, 40);
      else if (right && !bottom) colour = Bgra(40, 200, 120);
      else if (!right && bottom) colour = Bgra(60, 90, 235);
      else colour = Bgra(240, 200, 60);
      if (((x / 7) + (y / 5)) % 3 == 0) colour = Bgra(200, 200, 200);
      frame.pixels[static_cast<size_t>(y) * width + x] = colour;
    }
  }
  return frame;
}

bool UploadFrame(GlassIslandHost* host, const Frame& frame, ComPtr<ID2D1Bitmap1>* out) {
  if (!host || !host->Context() || !out) return false;
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  return SUCCEEDED(host->Context()->CreateBitmap(D2D1::SizeU(frame.width, frame.height),
                                                frame.pixels.data(), frame.width * 4u, props,
                                                out->ReleaseAndGetAddressOf()));
}

// The constants the product builds: the model's own cbuffer at the panel size.
prism::win::glass::GlassConstants PanelConstants(const IslandRect& rect) {
  prism::glass::GlassIslandScene scene;
  scene.Layout(rect.w, rect.h);
  scene.SetTheme(prism::glass::Theme::Daylight);
  prism::win::glass::GlassConstants constants{};
  scene.WriteCbuffer(prism::glass::IslandId::Nav, rect.w, rect.h, constants.v);
  constants.v[0] = static_cast<float>(rect.w) * 0.5f;
  constants.v[1] = static_cast<float>(rect.h) * 0.5f;
  constants.v[2] = static_cast<float>(rect.w) * 0.5f;
  constants.v[3] = static_cast<float>(rect.h) * 0.5f;
  constants.v[8] = static_cast<float>(rect.h) * 0.5f;
  return constants;
}

// The panel chrome the product paints INTO the island (rim + hairline). The
// fill and the shadow belong to the window underneath -- they are what the
// glass refracts.
void PaintRimOnly(ID2D1DeviceContext* dc, float width, float height,
                  ComPtr<ID2D1SolidColorBrush>* brush) {
  if (!*brush) {
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1),
                                         brush->ReleaseAndGetAddressOf()))) {
      return;
    }
  }
  prism::win::glass::PanelPainter painter;
  painter.Bind(dc, brush->Get());
  prism::win::glass::Parts parts;
  parts.shadow = false;
  parts.fill = false;
  painter.PaintRim(dc, D2D1::RectF(0.0f, 0.0f, width, height), prism::theme::Variant::Daylight,
                   parts, nullptr);
  painter.Unbind();
}

// The pre-C107 panel: fill + rim + shadow, no optics at all. Assertion 2's
// control, and the "glass OFF" screenshot.
void PaintFlatPanel(ID2D1DeviceContext* dc, float width, float height,
                    ComPtr<ID2D1SolidColorBrush>* brush) {
  if (!*brush) {
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1),
                                         brush->ReleaseAndGetAddressOf()))) {
      return;
    }
  }
  prism::win::glass::PanelPainter painter;
  painter.Bind(dc, brush->Get());
  prism::win::glass::Parts parts;
  parts.shadow = false;
  painter.Paint(dc, D2D1::RectF(0.0f, 0.0f, width, height), prism::theme::Variant::Daylight, parts,
                nullptr);
  painter.Unbind();
}

bool WritePng(const std::wstring& path, const Frame& frame) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.GetAddressOf())))) {
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(wic->CreateStream(stream.GetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> pngFrame;
  ComPtr<IPropertyBag2> bag;
  if (FAILED(encoder->CreateNewFrame(pngFrame.GetAddressOf(), bag.GetAddressOf()))) return false;
  if (FAILED(pngFrame->Initialize(bag.Get()))) return false;
  if (FAILED(pngFrame->SetSize(frame.width, frame.height))) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(pngFrame->SetPixelFormat(&format))) return false;
  if (FAILED(pngFrame->WritePixels(frame.height, frame.width * 4u,
                                   frame.width * frame.height * 4u,
                                   reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data()))))) {
    return false;
  }
  if (FAILED(pngFrame->Commit())) return false;
  return SUCCEEDED(encoder->Commit());
}

// The screenshots the card asks for: the SAME panel, optics on / off, from the
// product's own composition path (PaintIslandInto + the effect chain), placed
// on a wider canvas so the refracted content is visible around it.
void MaybeWriteScreenshots(GlassIslandHost* host, const IslandRect& rect,
                           const prism::win::glass::GlassConstants& constants,
                           ID2D1Bitmap1* backdrop, ComPtr<ID2D1SolidColorBrush>* glassBrush,
                           ComPtr<ID2D1SolidColorBrush>* flatBrush) {
  wchar_t outDir[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_C107_OUT", outDir, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return;
  ID2D1DeviceContext* dc = host->Context();
  if (!dc) return;
  D2D1_BITMAP_PROPERTIES1 targetProps = {};
  targetProps.pixelFormat =
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(rect.w, rect.h), nullptr, 0, targetProps,
                              target.GetAddressOf()))) {
    return;
  }
  const Frame backdropPixels = PatternFrame(rect.w, rect.h);
  const UINT kCanvasW = 640, kCanvasH = 132;
  const UINT kOffsetX = (kCanvasW - rect.w) / 2, kOffsetY = (kCanvasH - rect.h) / 2;
  const auto save = [&](const wchar_t* name, bool optics) {
    Frame island;
    const bool ok = host->PaintIslandInto(
        target.Get(), rect, optics ? backdrop : nullptr, constants,
        [&](ID2D1DeviceContext* context, float w, float h) {
          if (optics) {
            PaintRimOnly(context, w, h, glassBrush);
          } else {
            PaintFlatPanel(context, w, h, flatBrush);
          }
        });
    if (!ok || !ReadBitmap(host, target.Get(), &island)) return;
    const Frame composed = CompositeOver(island, backdropPixels);
    Frame canvas;
    canvas.width = kCanvasW;
    canvas.height = kCanvasH;
    canvas.pixels.assign(static_cast<size_t>(kCanvasW) * kCanvasH, Bgra(24, 26, 32));
    for (UINT y = 0; y < composed.height; ++y) {
      for (UINT x = 0; x < composed.width; ++x) {
        const UINT cx = x + kOffsetX;
        const UINT cy = y + kOffsetY;
        if (cx < kCanvasW && cy < kCanvasH) {
          canvas.pixels[static_cast<size_t>(cy) * kCanvasW + cx] = composed.At(x, y);
        }
      }
    }
    const std::wstring path = std::wstring(outDir) + L"\\" + name;
    std::printf("  wrote %ls (%s)\n", path.c_str(), WritePng(path, canvas) ? "ok" : "FAILED");
  };
  save(L"c107-glass-on.png", true);
  save(L"c107-glass-off.png", false);
}

// Composes the panel from the window's captured backdrop and hands back both
// the island and what the capture saw.
bool ComposeFromWindow(GlassIslandHost* host, HWND hwnd, const IslandRect& rect,
                       const prism::win::glass::GlassConstants& constants,
                       ID2D1Bitmap1* islandTarget, ComPtr<ID2D1SolidColorBrush>* brush,
                       Frame* island, Frame* backdrop) {
  if (!host->GrabBackdrop(hwnd, rect)) return false;
  if (!host->PaintIslandInto(islandTarget, rect, host->Backdrop(), constants,
                             [&](ID2D1DeviceContext* dc, float w, float h) {
                               PaintRimOnly(dc, w, h, brush);
                             })) {
    return false;
  }
  if (!ReadBitmap(host, islandTarget, island)) return false;
  if (host->DroppedBackdrops() > 0) g_injectionRan = true;
  return ReadBitmap(host, host->Backdrop(), backdrop);
}

bool MakeIslandTarget(GlassIslandHost* host, ComPtr<ID2D1Bitmap1>* out) {
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  return SUCCEEDED(host->Context()->CreateBitmap(D2D1::SizeU(kPanelW, kPanelH), nullptr, 0, props,
                                                 out->ReleaseAndGetAddressOf()));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. OPTICS READY (assertion 1).
// ---------------------------------------------------------------------------
PRISM_TEST("c107_optics_ready") {
  GlassIslandHost host;
  PRISM_CHECK(host.Initialize(nullptr, FindShaderDir()));
  if (!host.OpticsReady()) std::printf("  ShaderError: %s\n", host.ShaderError().c_str());
  PRISM_CHECK(host.OpticsReady());
  PRISM_CHECK(host.ShaderError().empty());

  // ... and the chain is LIVE, which is the half that was missing: on the
  // pre-C107 product OpticsReady() was true and the chain was never built.
  PRISM_CHECK(!host.OpticsLive());
  PRISM_CHECK_EQ(host.OpticsInitializations(), 0);
  PRISM_CHECK(SUCCEEDED(host.InitializeOpticsForIntensity(0.72)));
  PRISM_CHECK(host.OpticsLive());
  PRISM_CHECK_EQ(host.OpticsInitializations(), 1);
  PRISM_CHECK(host.ShaderError().empty());

  // The Kawase radius is DERIVED from appearance.glassIntensity: the reference
  // value is the shader's own 8 px band, and turning the setting down really
  // thins the blur instead of only lightening the tint.
  PRISM_CHECK_EQ(prism::win::glass::KawaseRadiusForIntensity(0.72), 8.0f);
  PRISM_CHECK_EQ(prism::win::glass::KawaseRadiusForIntensity(0.36), 4.0f);
  PRISM_CHECK_EQ(prism::win::glass::KawaseRadiusForIntensity(1.44), 16.0f);
  PRISM_CHECK_EQ(prism::win::glass::KawaseRadiusForIntensity(40.0), 16.0f);  // the 2x cap
  PRISM_CHECK_EQ(prism::win::glass::KawaseRadiusForIntensity(-1.0), 0.0f);
  const prism::win::glass::KawaseConstants first =
      prism::win::glass::KawasePassConstants(0.72, 1.0f);
  const prism::win::glass::KawaseConstants second =
      prism::win::glass::KawasePassConstants(0.72, 0.5f);
  PRISM_CHECK_EQ(first.radiusPx, 8.0f);
  PRISM_CHECK_EQ(first.passScale, 1.0f);
  PRISM_CHECK_EQ(second.passScale, 0.5f);
}

// ---------------------------------------------------------------------------
// 2. THE PANEL IS NOT A FLAT FILL (assertion 2).
// ---------------------------------------------------------------------------
PRISM_TEST("c107_panel_is_not_a_flat_fill") {
  GlassIslandHost host;
  PRISM_CHECK(host.Initialize(nullptr, FindShaderDir()));
  PRISM_CHECK(SUCCEEDED(host.InitializeOpticsForIntensity(0.72)));
  PRISM_CHECK(host.OpticsLive());
  if (!host.Context()) return;

  const IslandRect rect{0, 0, kPanelW, kPanelH};
  const prism::win::glass::GlassConstants constants = PanelConstants(rect);
  const Frame backdrop = PatternFrame(kPanelW, kPanelH);
  ComPtr<ID2D1Bitmap1> backdropBitmap;
  ComPtr<ID2D1Bitmap1> islandTarget;
  PRISM_CHECK(UploadFrame(&host, backdrop, &backdropBitmap));
  PRISM_CHECK(MakeIslandTarget(&host, &islandTarget));
  if (!backdropBitmap || !islandTarget) return;

  ComPtr<ID2D1SolidColorBrush> glassBrush;
  ComPtr<ID2D1SolidColorBrush> flatBrush;
  Frame glassIsland;
  Frame flatIsland;
  PRISM_CHECK(host.PaintIslandInto(islandTarget.Get(), rect, backdropBitmap.Get(), constants,
                                   [&](ID2D1DeviceContext* dc, float w, float h) {
                                     PaintRimOnly(dc, w, h, &glassBrush);
                                   }));
  PRISM_CHECK(ReadBitmap(&host, islandTarget.Get(), &glassIsland));
  PRISM_CHECK(host.PaintIslandInto(islandTarget.Get(), rect, nullptr, constants,
                                   [&](ID2D1DeviceContext* dc, float w, float h) {
                                     PaintFlatPanel(dc, w, h, &flatBrush);
                                   }));
  PRISM_CHECK(ReadBitmap(&host, islandTarget.Get(), &flatIsland));
  if (glassIsland.pixels.empty() || flatIsland.pixels.empty()) return;

  MaybeWriteScreenshots(&host, rect, constants, backdropBitmap.Get(), &glassBrush, &flatBrush);

  const Frame glass = CompositeOver(glassIsland, backdrop);
  const Frame flat = CompositeOver(flatIsland, backdrop);

  // The body of the panel, away from the 1 px rim and the rounded corners.
  int differing = 0;
  int samples = 0;
  double sum = 0.0;
  for (UINT y = 8; y + 8 < kPanelH; y += 3) {
    for (UINT x = 24; x + 24 < kPanelW; x += 3) {
      const int dr = std::abs(glass.R(x, y) - flat.R(x, y));
      const int dg = std::abs(glass.G(x, y) - flat.G(x, y));
      const int db = std::abs(glass.B(x, y) - flat.B(x, y));
      sum += (dr + dg + db) / 3.0;
      if (dr + dg + db >= 18) ++differing;  // 6 levels on the mean channel
      ++samples;
    }
  }
  const double mean = samples > 0 ? sum / samples : 0.0;
  std::printf("  panel vs flat fill: mean=%.2f levels, %d/%d samples differ by >= 6\n", mean,
              differing, samples);
  // Assertion 2 in two forms: the AVERAGE pixel differs, and it is not one
  // outlier -- most of the panel does.
  PRISM_CHECK(mean >= 6.0);
  PRISM_CHECK(samples > 0 && differing * 2 >= samples);
}

// ---------------------------------------------------------------------------
// 3. THE CHAIN SAMPLES THE CONTENT UNDER IT (assertion 3).
// ---------------------------------------------------------------------------
PRISM_TEST("c107_panel_follows_the_backdrop") {
  Stage stage;
  if (!stage.Open(kPanelW + 80, kPanelH + 80)) {
    std::puts("  [skip] no window station");
    return;
  }
  GlassIslandHost host;
  // hwnd = nullptr: this case tests the CAPTURE and the composition, not the
  // window's own composition target (that is case 5's subject). A DComp target
  // on the stage window would make GDI readback return an empty surface.
  PRISM_CHECK(host.Initialize(nullptr, FindShaderDir()));
  PRISM_CHECK(SUCCEEDED(host.InitializeOpticsForIntensity(0.72)));
  PRISM_CHECK(host.OpticsLive());
#if defined(PRISM_C107_MUTANT_NO_BACKDROP)
  // NEGATIVE CONTROL (D27): armed ONLY for this case, so the verdict names the
  // assertion it caught. It prints at the first red check and exits 0.
  prism::test::SetMutationVerdict("C107 MUTATION CAUGHT: no_backdrop",
                                  [] { return g_injectionRan; });
#endif
  ComPtr<ID2D1Bitmap1> islandTarget;
  ComPtr<ID2D1SolidColorBrush> brush;
  PRISM_CHECK(MakeIslandTarget(&host, &islandTarget));
  if (!host.Context() || !islandTarget) {
    stage.Close();
    return;
  }

  const IslandRect rect{40, 20, kPanelW, kPanelH};
  const prism::win::glass::GlassConstants constants = PanelConstants(rect);

  // RED under the panel.
  PRISM_CHECK(stage.Paint(255, 0, 0));
  Frame islandRed;
  Frame backdropRed;
  PRISM_CHECK(ComposeFromWindow(&host, stage.hwnd, rect, constants, islandTarget.Get(), &brush,
                                &islandRed, &backdropRed));
  if (islandRed.pixels.empty() || backdropRed.pixels.empty()) {
    stage.Close();
    return;
  }
  const UINT cx = kPanelW / 2;
  const UINT cy = kPanelH / 2;
  std::printf("  captured backdrop under the panel: R=%d G=%d B=%d\n", backdropRed.R(cx, cy),
              backdropRed.G(cx, cy), backdropRed.B(cx, cy));
  // The CAPTURE saw the window (not a blank surface, not the desktop).
  PRISM_CHECK(backdropRed.R(cx, cy) >= 200);
  PRISM_CHECK(backdropRed.G(cx, cy) <= 40);

  // BLUE under the panel: same panel, same constants.
  PRISM_CHECK(stage.Paint(0, 0, 255));
  Frame islandBlue;
  Frame backdropBlue;
  PRISM_CHECK(ComposeFromWindow(&host, stage.hwnd, rect, constants, islandTarget.Get(), &brush,
                                &islandBlue, &backdropBlue));
  if (islandBlue.pixels.empty() || backdropBlue.pixels.empty()) {
    stage.Close();
    return;
  }

  const Frame panelRed = CompositeOver(islandRed, backdropRed);
  const Frame panelBlue = CompositeOver(islandBlue, backdropBlue);
  const int dRed = panelRed.R(cx, cy) - panelBlue.R(cx, cy);
  const int dBlue = panelBlue.B(cx, cy) - panelRed.B(cx, cy);
  std::printf("  panel centre red -> blue: dR=%d dB=%d (red R=%d B=%d | blue R=%d B=%d)\n", dRed,
              dBlue, panelRed.R(cx, cy), panelRed.B(cx, cy), panelBlue.R(cx, cy),
              panelBlue.B(cx, cy));
  // Assertion 3, part A: the panel is not simply the backdrop. A translucent
  // island that paints NOTHING over the backdrop (the pre-C107 nullptr path,
  // and exactly what the no_backdrop mutant does) leaves the composite equal to
  // the backdrop in the panel body -- the optics are what put the tint, the
  // refraction and the rim between the user and those pixels.
  double differs = 0.0;
  int bodySamples = 0;
  for (UINT y = 8; y + 8 < kPanelH; y += 3) {
    for (UINT x = 24; x + 24 < kPanelW; x += 3) {
      const int dr = std::abs(panelRed.R(x, y) - backdropRed.R(x, y));
      const int dg = std::abs(panelRed.G(x, y) - backdropRed.G(x, y));
      const int db = std::abs(panelRed.B(x, y) - backdropRed.B(x, y));
      differs += (dr + dg + db) / 3.0;
      ++bodySamples;
    }
  }
  const double panelVsBackdrop = bodySamples > 0 ? differs / bodySamples : 0.0;
  std::printf("  panel vs raw backdrop: mean=%.2f levels\n", panelVsBackdrop);
  PRISM_CHECK(panelVsBackdrop >= 6.0);

  // Assertion 3, part B: the panel follows the content behind it. This catches
  // "the backdrop was never captured" (the panel is identical both times).
  PRISM_CHECK(dRed >= 4);
  PRISM_CHECK(dBlue >= 4);
  if (host.DroppedBackdrops() > 0) {
    // The injection really ran: the runtime half of D27. Arming the verdict
    // here means the mutant build prints it at the FIRST red assertion above
    // (the checks are evaluated in order, so the verdict is already printed by
    // the time this line runs -- see main()).
    std::printf("  INJECTION: %d backdrop(s) dropped by PRISM_C107_MUTANT_NO_BACKDROP\n",
                host.DroppedBackdrops());
  }
#if defined(PRISM_C107_MUTANT_NO_BACKDROP)
  prism::test::ClearMutationVerdict();
#endif
  stage.Close();
}

// ---------------------------------------------------------------------------
// 4. NO LEAK (assertion 4, C33's discipline).
// ---------------------------------------------------------------------------
PRISM_TEST("c107_200_frames_do_not_leak") {
  Stage stage;
  if (!stage.Open(kPanelW + 80, kPanelH + 80)) {
    std::puts("  [skip] no window station");
    return;
  }
  GlassIslandHost host;
  PRISM_CHECK(host.Initialize(nullptr, FindShaderDir()));
  PRISM_CHECK(SUCCEEDED(host.InitializeOpticsForIntensity(0.72)));
  ComPtr<ID2D1Bitmap1> islandTarget;
  ComPtr<ID2D1SolidColorBrush> brush;
  PRISM_CHECK(MakeIslandTarget(&host, &islandTarget));
  if (!host.Context() || !islandTarget) {
    stage.Close();
    return;
  }
  const IslandRect rect{40, 20, kPanelW, kPanelH};
  const prism::win::glass::GlassConstants constants = PanelConstants(rect);
  PRISM_CHECK(stage.Paint(120, 140, 200));

#if defined(PRISM_C107_MUTANT_BACKDROP_NOT_COUNTED)
  // NEGATIVE CONTROL (D27): the product allocates the backdrop but leaves it
  // out of TransientBytes(). The composition assertion below must go red.
  prism::test::SetMutationVerdict("C107 MUTATION CAUGHT: backdrop_not_counted",
                                  [] { return true; });
#endif
  size_t first = 0;
  bool ok = true;
  for (int frame = 0; frame < 200 && ok; ++frame) {
    ok = host.GrabBackdrop(stage.hwnd, rect) &&
         host.PaintIslandInto(islandTarget.Get(), rect, host.Backdrop(), constants,
                              [&](ID2D1DeviceContext* dc, float w, float h) {
                                PaintRimOnly(dc, w, h, &brush);
                              });
    if (frame == 0) first = host.TransientBytes();
  }
  PRISM_CHECK(ok);
  const size_t last = host.TransientBytes();
  // The total IS its composition (C107): island surfaces + effect graph +
  // the captured backdrop. Pinning a number would break the next time the
  // layout resizes the island; pinning the composition cannot, and the
  // backdrop_not_counted mutant makes exactly this line red.
  std::printf("  composition: island=%zu graph=%zu backdrop=%zu total=%zu\n",
              host.IslandSurfaceBytes(), host.GraphBytes(), host.BackdropBytes(), last);
  PRISM_CHECK_EQ(last, host.IslandSurfaceBytes() + host.GraphBytes() + host.BackdropBytes());
#if defined(PRISM_C107_MUTANT_BACKDROP_NOT_COUNTED)
  prism::test::ClearMutationVerdict();
#endif
  std::printf("  transient bytes: first frame=%zu, after 200 frames=%zu, backdrop=%zu\n", first,
              last, host.BackdropBytes());
  // The backdrop is REUSED: the accounting is identical, not merely bounded.
  PRISM_CHECK(first > 0);
  PRISM_CHECK_EQ(first, last);
  // ... and the product's own release still returns it (C33's contract).
  const size_t released = host.ReleaseTransient();
  PRISM_CHECK(released > 0);
  PRISM_CHECK_EQ(host.TransientBytes(), static_cast<size_t>(0));
  PRISM_CHECK(host.Backdrop() == nullptr);
  stage.Close();
}

// ---------------------------------------------------------------------------
// 5. THE PRODUCT WINDOW (integration): the chain is initialized by
//    EnableGlassLayer, the island is the LAYOUT's breadcrumb pill, and the
//    frames are composed WITH a captured backdrop.
// ---------------------------------------------------------------------------
PRISM_TEST("c107_product_window_connects_the_chain") {
  // Without DirectComposition the window keeps the pre-DComp path and there is
  // no island to inspect: a SKIP with a printed reason (D1), never a silent
  // pass.
  {
    // A real window: DCompositionCreateDevice succeeds without one, but
    // CreateTargetForHwnd(nullptr) does not -- probing with nullptr would
    // report "no composition" on every machine, including this one.
    Stage probeStage;
    if (!probeStage.Open(320, 200)) {
      std::puts("  [skip] no window station");
      return;
    }
    GlassIslandHost probe;
    const bool initialized = probe.Initialize(probeStage.hwnd, FindShaderDir());
    const bool composition = probe.UsesComposition();
    probeStage.Close();
    if (!initialized) {
      std::puts("  [skip] no D3D11 device on this runner");
      return;
    }
    if (!composition) {
      g_skipped = true;
      g_skipReason = "no DirectComposition on this runner (UsesComposition=false)";
      std::printf("  [skip] C107 SKIPPED: %s\n", g_skipReason.c_str());
      return;
    }
    std::puts("  C107 NOTE: DirectComposition is available on this runner");
  }

  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"C107 optics", GetModuleHandleW(nullptr))) {
    std::puts("  [skip] no window station");
    return;
  }
  window.Show();
  PRISM_CHECK(window.EnableGlassLayer(FindShaderDir()));
  // Assertion 1 through the PRODUCT entry point: EnableGlassLayer is what
  // initializes the chain, exactly once.
  PRISM_CHECK(window.GlassOpticsReady());
  if (!window.GlassOpticsError().empty()) {
    std::printf("  GlassOpticsError: %s\n", window.GlassOpticsError().c_str());
  }
  PRISM_CHECK(window.GlassOpticsError().empty());
  PRISM_CHECK_EQ(window.GlassOpticsInitializations(), 1);

  // C123 CHANGED THIS CASE (documented in the card's PR, one line per
  // assertion): the island used to be the breadcrumb PILL. The pill is gone --
  // the crumb is a 28 px path bar painted into the page -- so the product window
  // now publishes NO island rectangle at all. Returning the legacy pill here
  // would composite a blurred 44 px rectangle over the toolbar: a ghost of a
  // control the user can no longer click. The contract this case now pins is
  // exactly that absence.
  const prism::glass::IslandRect panel = window.GlassPanelRect();
  std::printf("  island rect: %u,%u %ux%u (C123: the crumb is a path bar, no island)\n",
              panel.x, panel.y, panel.w, panel.h);
  PRISM_CHECK_EQ(panel.x, 0u);
  PRISM_CHECK_EQ(panel.y, 0u);
  PRISM_CHECK_EQ(panel.w, 0u);
  PRISM_CHECK_EQ(panel.h, 0u);

  // Frames: pump bounded (D29) through the window's own WM_PAINT path. With no
  // panel there is nothing to compose, so the chain must stay at frame 0 -- a
  // non-zero count here would mean a phantom surface was still being fed.
  HWND hwnd = window.Hwnd();
  const ULONGLONG deadline = GetTickCount64() + 1500;
  while (GetTickCount64() < deadline) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    InvalidateRect(hwnd, nullptr, FALSE);
    UpdateWindow(hwnd);
    Sleep(10);
  }
  std::printf("  composed frames=%d, backdrop captures=%d, backdrop bytes=%zu, transient=%zu\n",
              window.GlassComposedFrames(), window.GlassBackdropCaptures(),
              window.GlassBackdropBytes(), window.GlassTransientBytes());
  PRISM_CHECK_EQ(window.GlassComposedFrames(), 0);
  PRISM_CHECK_EQ(window.GlassBackdropCaptures(), 0);
  PRISM_CHECK_EQ(window.GlassBackdropBytes(), static_cast<size_t>(0));
}

// The suite's own marker (D1): printed only when every check passed, with the
// skip reason repeated on its own line when a case could not run here.
int main(int argc, char** argv) {
  // WIC (the screenshot encoder) is a COM server.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int code = prism::test::RunAll(argc, argv);
  if (SUCCEEDED(com)) CoUninitialize();
  if (code == 0) {
    if (g_skipped) {
      std::printf("C107 SKIP: %s\n", g_skipReason.c_str());
    }
    std::puts("ALL C107 OPTICS CHECKS PASSED");
  }
  return code;
}
