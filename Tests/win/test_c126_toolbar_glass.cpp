// PrismArchive -- PrismWin
// C126: THE GLASS MOVES TO THE TOOLBAR -- THE ISLAND OWNS ITS CONTENT.
//
// THE DEFECT THIS CARD EXISTS FOR. C123 deleted the breadcrumb pill, so
// GalleryWindow::GlassPanelRect() returned 0x0 and three things lost their
// consumer: the composition island itself, the refraction ghost C121 had just
// fixed, and the refraction half of appearance.backdropRefraction. C123 measured
// the residue with win.c123_window: with NO island, flipping the setting still
// moved 222,472 pixels of the frame and 1,236 of them inside the toolbar band --
// the band's own panel chrome, not refraction.
//
// THE FIX IS OWNERSHIP, NOT LAYERING (C112's lesson). "Put the island on top of
// the toolbar" leaves the window's icons and words underneath; the island's
// backdrop capture then contains them, the optics blur that capture and paint
// the blur back over the same glyphs -- the C112 "重影" with a different
// rectangle. So the island OWNS the band: GalleryRenderer::DrawActionBar
// returns without painting anything, and the island's own device context paints
// the fill, the group seams, the hover capsule, the eight icons and the eight
// labels from prism::actionbar::Compute (the ONE authority the hit test also
// reads). The window keeps the dropdown panel (it hangs below the island) and
// the click geometry.
//
// WHAT THIS SUITE ASSERTS, on the REAL window + the REAL composition path:
//   G1 OWNERSHIP   -- with the island live, the WINDOW's button counter advances
//                     by 0 and the ISLAND's by 8 in exactly 1 island frame;
//   G2 NO GHOST    -- the toolbar's text keeps the window-drawn arm's sharpness
//                     (within 1.05x) AND the island-owned window frame carries
//                     no ink at all in the band (the optical ghost's only
//                     possible source);
//   G3 CLEAN GHOST -- the refraction ghost is the page background: every pixel
//                     of the toolbar band in the island-owned frame is the
//                     canvas colour (within 2 levels), i.e. NO toolbar ink is in
//                     the source the optics blur;
//   G4 REFRACTION  -- flipping appearance.backdropRefraction moves MORE than
//                     C123's recorded 1,236 in-band pixels (the number the card
//                     exists to grow);
//   G5 HIT GEOMETRY-- the eight hit rectangles are C123's, to the DIP: the band,
//                     the pitch, the per-button rectangle, the OPEN button's
//                     centre, and ActionBarButtonAt resolving each centre to its
//                     own index. Glass must not move the interaction geometry;
//   G6 THEME       -- a theme switch reaches the island's pixels AND the ghost
//                     (two captures of one theme agree, the other theme does not).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27), each a separate ctest entry that
// decides with PASS_REGULAR_EXPRESSION and never with WILL_FAIL:
//   PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR -> GlassPillOwnsContent() is false,
//     the window keeps painting the band over/under the island. G1 and G2 MUST
//     go red; the verdict prints "C126 MUTATION CAUGHT: window_paints_toolbar".
//   PRISM_C126_MUTANT_TEXT_IN_GHOST -> the band's ink is re-drawn into the
//     refraction source (the C112 defect). G3 MUST go red; the verdict prints
//     "C126 MUTATION CAUGHT: text_in_ghost".
//
// D1: no DirectComposition / no optics on this runner is a NAMED skip (the final
// marker repeats the reason), never a silent pass.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4 clean.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::glass::IslandRect;
using prism::win::gallery::GalleryWindow;

namespace {

constexpr int kSkipReturnCode = 77;

// ---------------------------------------------------------------------------
// A captured frame. 0xAARRGGBB -- the memory order of a 32bpp BGRA DIB, of a D2D
// B8G8R8A8 bitmap and of the WIC format the captures come back in.
// ---------------------------------------------------------------------------
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
  double L(UINT x, UINT y) const {
    return 0.299 * R(x, y) + 0.587 * G(x, y) + 0.114 * B(x, y);
  }
};

Frame Crop(const Frame& source, UINT x0, UINT y0, UINT w, UINT h) {
  Frame out;
  out.width = w;
  out.height = h;
  out.pixels.assign(static_cast<size_t>(w) * h, 0u);
  for (UINT y = 0; y < h; ++y) {
    for (UINT x = 0; x < w; ++x) {
      out.pixels[static_cast<size_t>(y) * w + x] = source.At(x0 + x, y0 + y);
    }
  }
  return out;
}

Frame Scale4x(const Frame& source) {
  Frame out;
  out.width = source.width * 4;
  out.height = source.height * 4;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height, 0u);
  for (UINT y = 0; y < out.height; ++y) {
    for (UINT x = 0; x < out.width; ++x) {
      out.pixels[static_cast<size_t>(y) * out.width + x] = source.At(x / 4, y / 4);
    }
  }
  return out;
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

int ChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, d < 0 ? -d : d);
  }
  return delta;
}

// The product's shaders: <checkout>/Assets/Shaders found from __FILE__, the way
// the C107/C112 suites do. Both .hlsl and .cso live there, so a build tree is
// not required for the composition to have optics.
std::wstring FindShaderDir() {
  wchar_t env[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_SHADER_DIR", env, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return env;
  wchar_t module[MAX_PATH] = {};
  const DWORD moduleLength = GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (moduleLength > 0 && moduleLength < MAX_PATH) {
    std::wstring beside(module, moduleLength);
    const size_t slash = beside.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      beside.resize(slash);
      if (GetFileAttributesW((beside + L"\\Assets\\Shaders").c_str()) != INVALID_FILE_ATTRIBUTES) {
        return beside + L"\\Assets\\Shaders";
      }
    }
  }
  namespace fs = std::filesystem;
  fs::path probe = fs::absolute(fs::path(__FILE__)).parent_path();
  for (int hop = 0; hop < 8 && !probe.empty(); ++hop) {
    const fs::path candidate = probe / "Assets" / "Shaders";
    if (fs::exists(candidate / "LiquidGlassEffect.hlsl")) return candidate.wstring();
    probe = probe.parent_path();
  }
  return L"Assets\\Shaders";
}

bool g_skipped = false;
std::string g_skipReason;

void Skip(const char* reason) {
  if (!g_skipped) {
    g_skipped = true;
    g_skipReason = reason;
  }
}

// D3: the runtime proof the injected defect REALLY fired, per arm. Only a
// failure preceded by "the injection ran" may print CAUGHT.
bool g_windowPaintsObserved = false;
bool g_textInGhostObserved = false;

// ---------------------------------------------------------------------------
// The window. 1280x800 outer, which the custom chrome turns into a 1264 px
// client -- the geometry C123's own suite measured (OPEN's centre at x = 52).
// ---------------------------------------------------------------------------
std::vector<prism::ArchiveItem> g_catalog;

void BuildCatalog() {
  g_catalog.clear();
  const char* names[] = {"docs", "readme.md", "photos", "a.png", "b.png", "c.png"};
  for (size_t i = 0; i < 6; ++i) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = names[i];
    item.isDir = (i == 0 || i == 2);
    item.size = 1024u + i;
    g_catalog.push_back(std::move(item));
  }
}

bool MakeWindow(GalleryWindow& window, const wchar_t* title, bool glass) {
  if (!window.Create(title, GetModuleHandleW(nullptr))) return false;
  window.SetItems(&g_catalog);
  window.SetRootLabel(L"fixture.zip");
  window.SetStatus(L"6 items");
  window.Show();
  SetWindowPos(window.Hwnd(), HWND_TOP, 40, 40, 1280, 800, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  RECT client{};
  if (!GetClientRect(window.Hwnd(), &client)) return false;
  window.Dispatch(WM_SIZE, 0,
                  MAKELPARAM(client.right - client.left, client.bottom - client.top));
  if (!glass) return true;
  if (!window.EnableGlassLayer(FindShaderDir())) return false;
  // C76 defers the first present to the second paint; every assertion below is
  // about the STEADY state (the island composited and owning the band), so the
  // handover frame is crossed here through the product's own path.
  window.PresentGlassFrameForTest();
  return true;
}

// TWO windows for the whole run, one per ROLE. The suite asks six questions
// about the same two configurations, and every glass window costs a D3D11
// device + a D2D factory + two HLSL effect registrations -- enough of them in
// one process and the fifth Initialize fails inside the effect chain (measured
// here: GlassChain::Initialize 0x80070490 on the sixth window). Reusing one
// window per role keeps the measurements about the TOOLBAR and not about how
// many devices the runner will hand out.
GalleryWindow* WindowOnly() {
  static GalleryWindow window;
  static bool attempted = false;
  static bool ready = false;
  if (!attempted) {
    attempted = true;
    ready = MakeWindow(window, L"c126 window only", /*glass=*/false);
  }
  return ready ? &window : nullptr;
}

GalleryWindow* IslandWindow() {
  static GalleryWindow window;
  static bool attempted = false;
  static bool ready = false;
  if (!attempted) {
    attempted = true;
    // Only COMPOSITION is required here: the island paints the band whether or
    // not the two HLSL effects registered, so G1/G2/G3/G5/G6 are meaningful on a
    // runner without shaders. G4 (the refraction delta) is the one case that
    // needs the chain, and it says so itself.
    if (!MakeWindow(window, L"c126 island", /*glass=*/true)) {
      Skip("the composition host could not be initialized (EnableGlassLayer failed)");
    } else {
      ready = true;
    }
  }
  return ready ? &window : nullptr;
}

// C113's capture seam: the renderer's device objects are rebuilt on the WIC
// target, so the picture is the scene and not a frame D2D rejected.
//
// The target is kept OPEN because the refraction source can only be read on the
// device that BUILT it (a second WIC target is a second device and D2D rejects
// the copy with D2DERR_WRONG_RESOURCE_DOMAIN). G3 uses it to pull the ghost's
// own 1/8 bitmap out of the same device that just built it.
struct CaptureSurface {
  ComPtr<IWICImagingFactory> wic;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;
  UINT width = 0;
  UINT height = 0;

  // 'ghostInTopLeft' asks the window to blit its refraction source into this
  // target's corner after the frame (C126's measurement seam). Only the source
  // region is meaningful in that mode -- the frame's own corner is overwritten.
  bool Open(GalleryWindow& window, bool ghostInTopLeft = false) {
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.GetAddressOf())))) {
      return false;
    }
    RECT rc{};
    if (!GetClientRect(window.Hwnd(), &rc)) return false;
    width = static_cast<UINT>(rc.right - rc.left);
    height = static_cast<UINT>(rc.bottom - rc.top);
    if (width == 0 || height == 0) return false;
    if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnDemand, bitmap.GetAddressOf()))) {
      return false;
    }
    ComPtr<ID2D1Factory> factory;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf()))) {
      return false;
    }
    return window.RenderSceneForCapture(target.Get(), ghostInTopLeft);
  }

  bool Read(UINT x0, UINT y0, UINT w, UINT h, Frame* out) const {
    WICRect lockRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, lock.GetAddressOf()))) return false;
    UINT stride = 0;
    UINT bytes = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &data))) {
      return false;
    }
    out->width = w;
    out->height = h;
    out->pixels.assign(static_cast<size_t>(w) * h, 0u);
    for (UINT y = 0; y < h; ++y) {
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * w,
                  data + static_cast<size_t>(y0 + y) * stride + static_cast<size_t>(x0) * 4u,
                  static_cast<size_t>(w) * 4u);
    }
    return true;
  }
};

bool CaptureFrame(GalleryWindow& window, Frame* out) {
  CaptureSurface surface;
  if (!surface.Open(window)) return false;
  return surface.Read(0, 0, surface.width, surface.height, out);
}

bool ReadBitmap(ID2D1DeviceContext* dc, ID2D1Bitmap* source, Frame* out) {
  if (!dc || !source || !out) return false;
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

// The island is PREMULTIPLIED and mostly translucent: what the user sees is it
// composited over the window's frame (exactly what DirectComposition does).
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
        const int src = static_cast<int>((s >> shift) & 0xFFu);  // premultiplied
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

bool UploadBackdrop(ID2D1DeviceContext* dc, const Frame& frame, ComPtr<ID2D1Bitmap1>* out) {
  if (!dc || !out) return false;
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  return SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(frame.width, frame.height), frame.pixels.data(),
                                    frame.width * 4u, props, out->ReleaseAndGetAddressOf()));
}

// THE TOOLBAR AS THE USER SEES IT: the island's own content (the shipping
// PresentGlassFrame -> PaintIslandContent path, through the same
// PaintToolbarOnIsland) composited over the window's frame at the band's
// rectangle. Both arms of every comparison below go through this, so "island
// off" and "island on" differ by WHO paints the band and by nothing else.
bool ComposeToolbar(GalleryWindow& window, const Frame& windowFrame, Frame* out) {
  const IslandRect rect = window.GlassPanelRect();
  if (rect.w == 0 || rect.h == 0 || out == nullptr) return false;
  ID2D1DeviceContext* dc = window.GlassContext();
  if (dc == nullptr) return false;
  const Frame backdropCrop = Crop(windowFrame, rect.x, rect.y, rect.w, rect.h);
  ComPtr<ID2D1Bitmap1> backdrop;
  if (!UploadBackdrop(dc, backdropCrop, &backdrop)) return false;
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(rect.w, rect.h), nullptr, 0, props,
                              target.GetAddressOf()))) {
    return false;
  }
  if (!window.ComposeIslandForTest(target.Get(), backdrop.Get())) return false;
  Frame island;
  if (!ReadBitmap(dc, target.Get(), &island)) return false;
  *out = CompositeOver(island, backdropCrop);
  return true;
}

// ---- the metrics ----------------------------------------------------------
// Horizontal gradient of luma (central difference), inside a rectangle. This is
// the statistic a blur attacks first: a glyph edge is one column wide, a blurred
// one is six.
double MaxGradient(const Frame& frame, UINT x0, UINT y0, UINT x1, UINT y1) {
  double best = 0.0;
  if (x1 < x0 + 2) return 0.0;
  for (UINT y = y0; y < y1 && y < frame.height; ++y) {
    for (UINT x = x0; x + 1 < x1 && x + 1 < frame.width; ++x) {
      const double g = std::fabs(frame.L(x + 1, y) - frame.L(x, y));
      if (g > best) best = g;
    }
  }
  return best;
}

// The band's own background level: the median luma of its outer 8 columns, which
// no button or label reaches (the toolbar's padding). Text is then "anything 40
// levels away from that".
double BackgroundLevel(const Frame& frame, UINT x0, UINT y0, UINT x1, UINT y1, UINT inset) {
  std::vector<double> samples;
  for (UINT y = y0; y < y1 && y < frame.height; ++y) {
    for (UINT x = x0; x < x1 && x < frame.width; ++x) {
      if (x >= x0 + inset && x < (x1 > inset ? x1 - inset : x0)) continue;
      samples.push_back(frame.L(x, y));
    }
  }
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

int InkPixels(const Frame& frame, UINT x0, UINT y0, UINT x1, UINT y1, double background,
              double threshold) {
  int count = 0;
  for (UINT y = y0; y < y1 && y < frame.height; ++y) {
    for (UINT x = x0; x < x1 && x < frame.width; ++x) {
      if (std::fabs(frame.L(x, y) - background) >= threshold) ++count;
    }
  }
  return count;
}

int MaxDeltaFrom(const Frame& frame, UINT x0, UINT y0, UINT x1, UINT y1, UINT32 reference) {
  int worst = 0;
  for (UINT y = y0; y < y1 && y < frame.height; ++y) {
    for (UINT x = x0; x < x1 && x < frame.width; ++x) {
      worst = (std::max)(worst, ChannelDelta(frame.At(x, y), reference));
    }
  }
  return worst;
}

bool At96Dpi(GalleryWindow& window, const char* testName) {
  if (window.DpiForTest() == 96) return true;
  // D1: a named skip, repeated on the suite's final line. The layout is DIPs and
  // these samples are pixels, so on a HiDPI runner the band's columns are
  // scaled and every rectangle below would measure a different row.
  char reason[160];
  std::snprintf(reason, sizeof(reason),
                "%s runs at %d DPI, not 96: the pixel samples are DIP-only", testName,
                window.DpiForTest());
  Skip(reason);
  std::printf("  [skip] %s\n", reason);
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// G1: OWNERSHIP. Two counters, two devices.
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g1 ownership: the island paints the band, the window paints nothing") {
  GalleryWindow* referenceWindow = WindowOnly();
  if (!PRISM_CHECK(referenceWindow != nullptr)) return;
  GalleryWindow& reference = *referenceWindow;
  if (!PRISM_CHECK(At96Dpi(reference, "g1"))) return;
  Frame referenceFrame;
  if (!PRISM_CHECK(CaptureFrame(reference, &referenceFrame))) return;
  const GalleryWindow::ToolbarOwnershipCounters refCounters = reference.ToolbarOwnership();
  std::printf("C126 G1 reference (no island): window buttons=%d island paints=%d\n",
              refCounters.windowButtons, refCounters.islandPaints);
  // The band really reaches the device in the window-drawn arm: without this the
  // "0 buttons" below could be the zero of a band that was never painted.
  PRISM_CHECK(refCounters.windowButtons >= prism::actionbar::kActionCount);
  PRISM_CHECK_EQ(refCounters.islandPaints, 0);

  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  PRISM_CHECK(on.GlassPillOwnsContent());
  const GalleryWindow::ToolbarOwnershipCounters before = on.ToolbarOwnership();
  const bool presented = on.PresentGlassFrameForTest();
  const GalleryWindow::ToolbarOwnershipCounters island = on.ToolbarOwnership();
  const int islandPaints = island.islandPaints - before.islandPaints;
  const int islandButtons = island.islandButtons - before.islandButtons;
  std::printf("C126 G1 island: presented=%d island frames=%d buttons=%d icons=%d labels=%d\n",
              static_cast<int>(presented), islandPaints, islandButtons,
              island.islandIcons - before.islandIcons, island.islandLabels - before.islandLabels);
  PRISM_CHECK(presented);
  // ONE island frame, eight buttons, eight icons, eight words -- from the island.
  PRISM_CHECK_EQ(islandPaints, 1);
  PRISM_CHECK_EQ(islandButtons, static_cast<int>(prism::actionbar::kActionCount));
  PRISM_CHECK_EQ(island.islandIcons - before.islandIcons,
                 static_cast<int>(prism::actionbar::kActionCount));
  PRISM_CHECK_EQ(island.islandLabels - before.islandLabels,
                 static_cast<int>(prism::actionbar::kActionCount));

  // ...and the WINDOW paints none of them. The delta is measured around one
  // capture, so the handover frame (frame 1, before the island presented) cannot
  // contribute: ownership begins at the present above.
  const int windowBefore = on.ToolbarOwnership().windowButtons;
  const int skipsBefore = on.ToolbarOwnership().windowSkips;
  Frame onFrame;
  if (!PRISM_CHECK(CaptureFrame(on, &onFrame))) return;
  const GalleryWindow::ToolbarOwnershipCounters window = on.ToolbarOwnership();
  const int windowButtons = window.windowButtons - windowBefore;
  const int windowSkips = window.windowSkips - skipsBefore;
  std::printf("C126 G1 window: buttons=%d ownership skips=%d\n", windowButtons, windowSkips);
#if defined(PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR) || defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  // CONTRACTS D3: the injection is active when the WINDOW really painted the
  // band while the island was live. Only then may the failure print CAUGHT.
  g_windowPaintsObserved = windowButtons > 0;
#endif
  PRISM_CHECK_EQ(windowButtons, 0);
  PRISM_CHECK(windowSkips >= 1);
}

// ---------------------------------------------------------------------------
// G2: NO GHOST. Sharpness parity + the structural half (the window frame under
// the band is EMPTY, so the island's backdrop capture cannot contain any glyph).
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g2 no ghost: the island's toolbar text is as sharp as the window's") {
  GalleryWindow* referenceWindow = WindowOnly();
  if (!PRISM_CHECK(referenceWindow != nullptr)) return;
  GalleryWindow& reference = *referenceWindow;
  if (!PRISM_CHECK(At96Dpi(reference, "g2"))) return;
  Frame referenceFrame;
  if (!PRISM_CHECK(CaptureFrame(reference, &referenceFrame))) return;
  const GalleryWindow::ToolbarOwnershipCounters refCounters = reference.ToolbarOwnership();
  PRISM_CHECK(refCounters.windowButtons >= prism::actionbar::kActionCount);

  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  Frame onFrame;
  if (!PRISM_CHECK(CaptureFrame(on, &onFrame))) return;
  Frame composed;
  if (!PRISM_CHECK(ComposeToolbar(on, onFrame, &composed))) return;

  const prism::layout::Layout layout = on.LayoutForTest();
  const prism::actionbar::Layout band = on.ActionBarLayout();
  const prism::layout::Rect bandRect = prism::layout::ToolbarBandRect(layout);
  if (!PRISM_CHECK(!bandRect.Empty() && !band.buttons.empty())) return;
  const UINT left = static_cast<UINT>(bandRect.left);
  const UINT top = static_cast<UINT>(bandRect.top);
  const UINT right = static_cast<UINT>(bandRect.right);
  const UINT bottom = static_cast<UINT>(bandRect.bottom);

  // The LABEL BAND of the eight buttons: where the words are, so the statistic
  // cannot be dominated by the icons, the seam or the window's edge.
  const prism::layout::Rect firstLabel = prism::actionbar::LabelBoxRect(band.buttons.front().rect);
  const prism::layout::Rect lastLabel = prism::actionbar::LabelBoxRect(band.buttons.back().rect);
  const UINT labelTop = static_cast<UINT>(firstLabel.top);
  const UINT labelBottom = static_cast<UINT>(firstLabel.bottom);

  const double sharpReference =
      MaxGradient(referenceFrame, static_cast<UINT>(firstLabel.left), labelTop,
                  static_cast<UINT>(lastLabel.right), labelBottom);
  // The composed frame is ISLAND-LOCAL: subtract the band's origin.
  const double sharpIsland =
      MaxGradient(composed, static_cast<UINT>(firstLabel.left) - left, labelTop - top,
                  static_cast<UINT>(lastLabel.right) - left, labelBottom - top);
  std::printf("C126 G2 sharpness: window=%.1f island=%.1f ratio=%.3f\n", sharpReference,
              sharpIsland, sharpReference > 0.0 ? sharpIsland / sharpReference : 0.0);
  PRISM_CHECK(sharpReference > 20.0);
  // The card's band: the island's text may not be SHARPER than the window's
  // (a second, offset copy makes a two-step edge) ...
  PRISM_CHECK(sharpIsland <= sharpReference * 1.05);
  // ... and it may not be BLUNTER either (that is the C112 blur, measured the
  // other way round; both bounds are needed, one alone is satisfiable by the
  // defect on the other side).
  PRISM_CHECK(sharpIsland >= sharpReference * 0.90);

  // STRUCTURAL: the island-owned window frame carries no ink in the band, so the
  // capture the optics blur cannot contain a single glyph. This is the only
  // source of a ghost, and it is a different fact from the counter in G1.
  const double backgroundRef =
      BackgroundLevel(referenceFrame, left, top, right, bottom, 8);
  const int inkReference = InkPixels(referenceFrame, left, top, right, bottom, backgroundRef, 40.0);
  const int inkOwned = InkPixels(onFrame, left, top, right, bottom, backgroundRef, 40.0);
  std::printf("C126 G2 window-frame ink in the band: reference=%d island-owned=%d\n",
              inkReference, inkOwned);
  PRISM_CHECK(inkReference > 200);
#if defined(PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR) || defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  g_windowPaintsObserved = g_windowPaintsObserved || inkOwned > 0;
#endif
  PRISM_CHECK_EQ(inkOwned, 0);

  if (const char* outDir = std::getenv("PRISM_C126_OUT")) {
    const std::wstring root(outDir, outDir + std::strlen(outDir));
    std::printf("  [shot] c126-toolbar.png saved=%d\n",
                static_cast<int>(WritePng(root + L"\\c126-toolbar.png", Scale4x(composed))));
  }
}

// ---------------------------------------------------------------------------
// G3: THE GHOST IS CLEAN. The refraction source C121 fixed is the PAGE
// background; the toolbar's ink must not be in it.
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g3 clean ghost: no toolbar ink in the refraction source") {
  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  if (!PRISM_CHECK(At96Dpi(on, "g3"))) return;
  if (!PRISM_CHECK(prism::theme::BackdropRefractionEnabled())) return;  // the ghost is on
  const prism::layout::Layout layout = on.LayoutForTest();
  const prism::layout::Rect bandRect = prism::layout::ToolbarBandRect(layout);
  if (!PRISM_CHECK(!bandRect.Empty())) return;

  // THE SOURCE, NOT THE FRAME. DrawBands re-fills the action band with the
  // canvas colour AFTER DrawBackdrop, so the frame's band pixels are the band
  // fill whether the ghost is clean or not -- sampling them would be a green
  // that cannot go red. The object the optics are actually handed is the 1/8
  // bitmap, and the window's measurement seam blits it into the capture target
  // on the device that built it (C113's rule).
  CaptureSurface surface;
  if (!PRISM_CHECK(surface.Open(on, /*ghostInTopLeft=*/true))) return;
  // The size of the ghost that pass built (the renderer remembers it across the
  // device-resource release, because the read-back region has to be known).
  const UINT ghostW = on.GhostSourceWidth();
  const UINT ghostH = on.GhostSourceHeight();
  if (!PRISM_CHECK(ghostW > 0 && ghostH > 0)) return;
  Frame ghost;
  if (!PRISM_CHECK(surface.Read(0, 0, ghostW, ghostH, &ghost))) return;
  // The canvas reference comes from the SOURCE itself: its top-left is the title
  // band, which is reserved space painted with the canvas colour -- no colour
  // literal, and no dependence on the frame's own (overwritten) corner.
  const UINT32 ghostCanvas = ghost.At(1, 1);
  // The band's rectangle in the SOURCE's coordinates: the ghost is 1/8 of the
  // frame, and the page pass draws with that scale.
  const UINT gx0 = static_cast<UINT>(bandRect.left) / 8u;
  const UINT gy0 = static_cast<UINT>(bandRect.top) / 8u;
  const UINT gx1 = (std::min)(ghostW, static_cast<UINT>(bandRect.right) / 8u);
  const UINT gy1 = (std::min)(ghostH, static_cast<UINT>(bandRect.bottom) / 8u);
  if (!PRISM_CHECK(gx1 > gx0 && gy1 > gy0)) return;
  const int ghostBandWorst = MaxDeltaFrom(ghost, gx0, gy0, gx1, gy1, ghostCanvas);
  // The presented canvas, from a NORMAL capture: the ghost's own canvas has to be
  // the colour the window shows, or "clean" would be clean against the wrong
  // reference.
  Frame frame;
  const bool captured = CaptureFrame(on, &frame);
  const UINT32 presentedCanvas =
      captured ? frame.At(1, static_cast<UINT>(layout.titleBar.top) + 4) : 0u;
  std::printf("C126 G3 presented canvas=%08X ghost canvas=%08X source=%ux%u "
              "band in source=(%u,%u..%u,%u)\n",
              presentedCanvas, ghostCanvas, ghostW, ghostH, gx0, gy0, gx1, gy1);
  std::printf("C126 G3 refraction source deviation from the canvas: band=%d levels; "
              "text-in-ghost arm runs=%d\n",
              ghostBandWorst, on.TextInGhostArmRuns());
#if defined(PRISM_C126_MUTANT_TEXT_IN_GHOST)
  g_textInGhostObserved = on.TextInGhostArmRuns() > 0;
#endif
  // ±2 levels on the SOURCE: the ghost of a uniform canvas IS that canvas, so
  // anything the toolbar painted into it shows up here immediately and is not
  // attenuated by the veil the presented frame puts on top.
  PRISM_CHECK(ghostBandWorst <= 2);
  if (captured) PRISM_CHECK(ChannelDelta(ghostCanvas, presentedCanvas) <= 2);
}

// ---------------------------------------------------------------------------
// G4: THE REFRACTION SETTING REACHES THE BAND. C123 recorded 1,236 in-band
// pixels for a build with NO island; this card must grow that number.
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g4 refraction: the setting moves more than C123's 1,236 in-band pixels") {
  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  if (!PRISM_CHECK(At96Dpi(on, "g4"))) return;
  if (!on.GlassOpticsReady()) {
    // Without the effect chain the two arms compose the same flat band, so the
    // delta would be 0 for a reason that has nothing to do with the setting.
    Skip("the optical chain did not register on this runner: no refraction to observe");
    return;
  }
  const prism::layout::Layout layout = on.LayoutForTest();
  const prism::layout::Rect bandRect = prism::layout::ToolbarBandRect(layout);
  if (!PRISM_CHECK(!bandRect.Empty())) return;

  const bool was = prism::theme::BackdropRefractionEnabled();
  prism::theme::live::SetBackdropRefraction(true);
  Frame onFrame;
  Frame composedOn;
  const bool capturedOn =
      CaptureFrame(on, &onFrame) && ComposeToolbar(on, onFrame, &composedOn);
  prism::theme::live::SetBackdropRefraction(false);
  Frame offFrame;
  Frame composedOff;
  const bool capturedOff =
      CaptureFrame(on, &offFrame) && ComposeToolbar(on, offFrame, &composedOff);
  prism::theme::live::SetBackdropRefraction(was);
  if (!PRISM_CHECK(capturedOn && capturedOff)) {
    Skip("the device path could not capture/compose both refraction arms");
    return;
  }

  int changed = 0;
  int maxDelta = 0;
  for (UINT y = 0; y < composedOn.height; ++y) {
    for (UINT x = 0; x < composedOn.width; ++x) {
      const int d = ChannelDelta(composedOn.At(x, y), composedOff.At(x, y));
      if (d == 0) continue;
      ++changed;
      maxDelta = (std::max)(maxDelta, d);
    }
  }
  std::printf("C126 G4 refraction: %d of %u band pixels differ (max channel delta %d); "
              "C123 recorded 1236 with no island\n",
              changed, composedOn.width * composedOn.height, maxDelta);
  // The number C123 measured is the FLOOR, not the target: with the island
  // owning the band the setting now gates the optics on the one surface this
  // card creates, and the whole strip moves.
  PRISM_CHECK(changed > 1236);
  PRISM_CHECK(maxDelta > 2);
}

// ---------------------------------------------------------------------------
// G5: HIT GEOMETRY IS C123'S. Glass may change who paints a button; it may not
// change where a button is.
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g5 hit geometry: the eight rectangles are C123's, to the DIP") {
  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  if (!PRISM_CHECK(At96Dpi(on, "g5"))) return;
  const prism::layout::Layout layout = on.LayoutForTest();
  const prism::layout::Rect toolbar = prism::layout::ToolbarBandRect(layout);
  const prism::actionbar::Layout band = on.ActionBarLayout();
  std::printf("C126 G5 band=(%d,%d..%d,%d) buttons=%zu\n", band.band.left, band.band.top,
              band.band.right, band.band.bottom, band.buttons.size());
  PRISM_CHECK_EQ(band.band.left, toolbar.left);
  PRISM_CHECK_EQ(band.band.top, toolbar.top);
  PRISM_CHECK_EQ(band.band.right, toolbar.right);
  PRISM_CHECK_EQ(band.band.bottom, toolbar.bottom);
  PRISM_CHECK_EQ(band.band.left, layout.contentBand.left);
  PRISM_CHECK_EQ(band.band.right, layout.contentBand.right);
  if (!PRISM_CHECK_EQ(static_cast<int>(band.buttons.size()),
                      prism::actionbar::kActionCount)) {
    return;
  }

  // The pitch, re-derived from C123's own published constants (kIslandPad,
  // kButtonMin/MaxWidth, GapBefore) instead of from actionbar::Compute, so the
  // assertion is an independent statement of the geometry and a glass-side
  // inset would show up as a moved rectangle.
  const int gaps = prism::actionbar::TotalGaps();
  int width = (toolbar.Width() - 2 * prism::actionbar::kIslandPad - gaps) /
              prism::actionbar::kActionCount;
  width = (std::max)(prism::actionbar::kButtonMinWidth,
                     (std::min)(width, prism::actionbar::kButtonMaxWidth));
  int x = toolbar.left + prism::actionbar::kIslandPad;
  int mismatches = 0;
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    const prism::layout::Rect expected{x, toolbar.top, x + width, toolbar.bottom};
    const prism::actionbar::Button& button = band.buttons[static_cast<size_t>(i)];
    if (!(button.rect == expected)) {
      ++mismatches;
      std::printf("  button %d: painted (%d,%d..%d,%d) expected (%d,%d..%d,%d)\n", i,
                  button.rect.left, button.rect.top, button.rect.right, button.rect.bottom,
                  expected.left, expected.top, expected.right, expected.bottom);
    }
    // The click path resolves the rectangle's own centre to this button.
    const int cx = (button.rect.left + button.rect.right) / 2;
    const int cy = (button.rect.top + button.rect.bottom) / 2;
    PRISM_CHECK_EQ(on.ActionBarButtonAt(cx, cy), i);
    x += width + prism::actionbar::GapBefore(i + 1);
  }
  PRISM_CHECK_EQ(mismatches, 0);

  // The OPEN button's centre is C123's measured number (the audit's red line is
  // x <= 60; the suite printed 52 at this client width).
  const prism::actionbar::Button& open = band.buttons.front();
  const int openCentreX = (open.rect.left + open.rect.right) / 2;
  std::printf("C126 G5 OPEN centre=%d,%d\n", openCentreX,
              (open.rect.top + open.rect.bottom) / 2);
  PRISM_CHECK_EQ(openCentreX, 52);

  // ...and the ISLAND is that same rectangle: turning the glass on did not move
  // the toolbar by a pixel.
  const IslandRect panel = on.GlassPanelRect();
  std::printf("C126 G5 island rect=%u,%u %ux%u\n", panel.x, panel.y, panel.w, panel.h);
  PRISM_CHECK_EQ(static_cast<int>(panel.x), toolbar.left);
  PRISM_CHECK_EQ(static_cast<int>(panel.y), toolbar.top);
  PRISM_CHECK_EQ(static_cast<int>(panel.w), toolbar.Width());
  PRISM_CHECK_EQ(static_cast<int>(panel.h), toolbar.Height());
}

// ---------------------------------------------------------------------------
// G6: THE THEME REACHES THE ISLAND -- and the ghost follows it.
// ---------------------------------------------------------------------------
PRISM_TEST("c126.g6 theme: a theme switch reaches the island and the ghost") {
  GalleryWindow* islandWindow = IslandWindow();
  if (!PRISM_CHECK(islandWindow != nullptr)) return;
  GalleryWindow& on = *islandWindow;
  if (!PRISM_CHECK(At96Dpi(on, "g6"))) return;
  const prism::layout::Layout layout = on.LayoutForTest();
  const prism::layout::Rect bandRect = prism::layout::ToolbarBandRect(layout);
  if (!PRISM_CHECK(!bandRect.Empty())) return;

  const auto composeWith = [&](Frame* composed, Frame* windowFrame) {
    if (!CaptureFrame(on, windowFrame)) return false;
    return ComposeToolbar(on, *windowFrame, composed);
  };
  on.SetChromeVariant(prism::theme::Variant::Midnight);
  Frame midnightWindow;
  Frame midnight;   // island band, midnight
  if (!PRISM_CHECK(composeWith(&midnight, &midnightWindow))) return;
  const int rebuildsBefore = on.BackdropRebuilds();
  Frame midnightAgainWindow;
  Frame midnightAgain;
  if (!PRISM_CHECK(composeWith(&midnightAgain, &midnightAgainWindow))) return;
  on.SetChromeVariant(prism::theme::Variant::Daylight);
  Frame dayWindow;
  Frame day;
  if (!PRISM_CHECK(composeWith(&day, &dayWindow))) return;
  const int rebuildsAfter = on.BackdropRebuilds();
  on.SetChromeVariant(prism::theme::Variant::Midnight);

  // The island's own FILL: the far right of the band, clear of the eighth button
  // (the strip is filled edge to edge by ActionBarPainter::DrawBand).
  const UINT probeX = bandRect.Width() > 8 ? static_cast<UINT>(bandRect.Width()) - 4 : 0;
  const UINT probeY = static_cast<UINT>(bandRect.Height() / 2);
  const int fillDelta = ChannelDelta(day.At(probeX, probeY), midnight.At(probeX, probeY));
  // The ghost's own canvas: the title band, through the window frame.
  const UINT32 canvasMidnight = midnightWindow.At(1, static_cast<UINT>(layout.titleBar.top) + 4);
  const UINT32 canvasDay = dayWindow.At(1, static_cast<UINT>(layout.titleBar.top) + 4);
  const int canvasDelta = ChannelDelta(canvasDay, canvasMidnight);
  // Two captures of ONE theme must agree; the other theme must not. That is the
  // ghost's identity (its BackdropKey carries the theme), stated in pixels.
  int sameThemeWorst = 0;
  for (UINT y = 0; y < midnight.height; ++y) {
    for (UINT x = 0; x < midnight.width; ++x) {
      sameThemeWorst = (std::max)(sameThemeWorst,
                                  ChannelDelta(midnight.At(x, y), midnightAgain.At(x, y)));
    }
  }
  std::printf("C126 G6 theme: island fill delta=%d, canvas delta=%d, "
              "same-theme delta=%d, backdrop rebuilds %d -> %d\n",
              fillDelta, canvasDelta, sameThemeWorst, rebuildsBefore, rebuildsAfter);
  PRISM_CHECK(fillDelta > 8);       // the island followed the theme
  PRISM_CHECK(canvasDelta > 2);     // and so did the ghost
  PRISM_CHECK_EQ(sameThemeWorst, 0);// deterministic, so the two above mean something
  PRISM_CHECK(rebuildsAfter > rebuildsBefore);
  if (const char* outDir = std::getenv("PRISM_C126_OUT")) {
    const std::wstring root(outDir, outDir + std::strlen(outDir));
    std::printf("  [shot] c126-theme-day.png saved=%d\n",
                static_cast<int>(WritePng(root + L"\\c126-theme-day.png", Scale4x(day))));
  }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("SKIP: CoInitializeEx failed (this window station has no COM)\n");
    return kSkipReturnCode;
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  BuildCatalog();
  // D1: without a window station every case below would return before its first
  // assertion and the marker would be a lie. Probe ONCE, up front, and leave with
  // the project's visible Skip code.
  {
    GalleryWindow probe;
    if (!probe.Create(L"PrismArchive c126 probe", GetModuleHandleW(nullptr))) {
      std::puts("SKIP: window creation failed (this window station cannot make an HWND)");
      return kSkipReturnCode;
    }
  }
#if defined(PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR)
  std::puts("C126 MUTANT ACTIVE: window_paints_toolbar");
  prism::test::SetMutationVerdict("C126 MUTATION CAUGHT: window_paints_toolbar",
                                  [] { return g_windowPaintsObserved; });
#elif defined(PRISM_C126_MUTANT_TEXT_IN_GHOST)
  std::puts("C126 MUTANT ACTIVE: text_in_ghost");
  prism::test::SetMutationVerdict("C126 MUTATION CAUGHT: text_in_ghost",
                                  [] { return g_textInGhostObserved; });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  if (g_skipped) std::printf("C126 SKIP: %s\n", g_skipReason.c_str());
  std::printf("ALL C126 TOOLBAR-GLASS CHECKS PASSED (%d checks)\n", prism::test::CheckCount());
  ::CoUninitialize();
  return 0;
}
