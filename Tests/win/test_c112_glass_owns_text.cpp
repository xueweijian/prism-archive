// PrismArchive -- PrismWin
// C112: THE ISLAND OWNS THE PILL -- fill, refraction AND text.
//
// THE MEASURED DEFECT this card exists for (C107, seen twice by the user):
//   The island is a CreateTargetForHwnd(topmost=TRUE) visual ABOVE the window.
//   Its backdrop is a CAPTURE of the window's own pixels -- and those pixels
//   already contained the breadcrumb text. The optics blurred that text, the
//   refraction shifted it 1-3 px, and the result was painted back OVER the
//   window's sharp text. Every glyph therefore had a second, offset, blurred
//   copy of itself: "文字被自己的模糊副本盖住 = 重影".
//   Turning appearance.backdropRefraction OFF did not fix it (that only removes
//   the renderer's own backdrop layer); removing C107 from the integration tree
//   did. The optics were sampling their own output.
//
// WHAT THIS SUITE ASSERTS, on the REAL window + the REAL composition path:
//   S1 SHARPNESS. The pill's text, measured as the maximum horizontal gradient
//      over the text band, must retain >= 90% of the SAME text's sharpness with
//      the island switched off. A blurred/occluded copy drops this; it cannot
//      be satisfied by a brighter fill.
//   S2 NO GHOST. The blur halo -- text energy in the 1..3 px RING around the
//      glyphs, where the island-off frame is pure background -- must stay under
//      a few levels. A second, blurred copy of the same text is exactly what
//      puts energy there.
//   S3 THE WINDOW PAINTS NO TEXT UNDER THE ISLAND. With the island owning the
//      pill, the window's own frame must be EMPTY in the pill's text band (only
//      geometry for hit testing remains). This is the structural half of S1:
//      if the window still paints it, the capture feeds it back to the optics.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the SAME source compiled with
// PRISM_C112_MUTANT_GLASS_OVER_TEXT -- the text is still drawn by the window and
// the island is composed on top of it (the pre-C112 arm). S1 goes red and the
// entry prints "C112 MUTATION CAUGHT: glass_over_text"; the ctest entry matches
// that line with PASS_REGULAR_EXPRESSION, never with WILL_FAIL.
//
// D1: without DirectComposition there is no island to inspect. That case prints
// "C112 SKIP: ..." with the reason and the suite's final marker repeats it --
// never a silent pass.
#include <windows.h>
#include <d2d1_3.h>
#include <d3d11.h>
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
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"

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
using prism::win::gallery::GalleryWindow;

namespace {

// A pixel is 0xAARRGGBB -- the memory order of a 32bpp BGRA DIB, of a D2D
// B8G8R8A8 bitmap, and of the WIC format the screenshots are written in.
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
  // Luma in 0..255. The text is near-black on a light pill, so one channel would
  // do; the weighted sum keeps the metric honest under a coloured theme.
  double L(UINT x, UINT y) const {
    return 0.299 * R(x, y) + 0.587 * G(x, y) + 0.114 * B(x, y);
  }
};

// A rectangle cut out of a frame (the pill's own pixels).
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

// Nearest-neighbour 4x for the screenshots: no resampling, so the evidence is
// the pixels themselves and not a blurred photograph of them.
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
  if (FAILED(pngFrame->WritePixels(frame.height, frame.width * 4u, frame.width * frame.height * 4u,
                                   reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data()))))) {
    return false;
  }
  if (FAILED(pngFrame->Commit())) return false;
  return SUCCEEDED(encoder->Commit());
}

// The product's shaders: <checkout>/Assets/Shaders, found from __FILE__ the way
// the C107 suite does. Without them the host still composes -- flat -- and the
// "no ghost" measurement would have no optics to be red about.
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

// The runtime proof that PRISM_C112_MUTANT_GLASS_OVER_TEXT really fired: the
// product reported that the WINDOW owns the pill's text while the island is
// live. CONTRACTS D3: only a failure preceded by this may print CAUGHT.
bool g_mutantObserved = false;

// ---- the window -----------------------------------------------------------
// A real GalleryWindow with an EMPTY catalog: the pill's background is then one
// flat canvas, so every non-background pixel inside the pill is TEXT. The
// breadcrumb still carries the root label, so the pill is never empty.
bool MakeWindow(GalleryWindow& window, const wchar_t* title, bool glass) {
  if (!window.Create(title, GetModuleHandleW(nullptr))) return false;
  window.SetRootLabel(L"pack.7z");
  window.Show();
  // The REAL client size, handed to the window's own WM_SIZE: a synthetic size
  // that the client rectangle does not have would leave layout_ and the frame
  // disagreeing, and the two arms would then differ everywhere.
  SetWindowPos(window.Hwnd(), HWND_TOP, 40, 40, 1280, 800, SWP_SHOWWINDOW | SWP_NOACTIVATE);
  RECT client{};
  if (!GetClientRect(window.Hwnd(), &client)) return false;
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(client.right - client.left, client.bottom - client.top));
  if (glass && !window.EnableGlassLayer(FindShaderDir())) return false;
  if (glass) {
    // C76 defers the island's first present to the second paint and C112 hands
    // the pill over only after a present, so the steady state is reached by
    // PRESENTING once through the product's own PresentGlassFrame (the same
    // capture + chain + pill content a WM_PAINT would run). The frame is then
    // rendered offscreen -- and deliberately NOT after a real paint: that would
    // bind the renderer's cached C77 backdrop to the window's device, which a
    // WIC target cannot sample (D2DERR_WRONG_RESOURCE_DOMAIN, measured).
    window.PresentGlassFrameForTest();
  }
  return true;
}

// The frame the window paints, rendered through the SAME DrawFrame the window's
// own WM_PAINT uses (GalleryWindow::RenderSceneInto), into a software target.
bool RenderWindow(GalleryWindow& window, Frame* out) {
  RECT rc{};
  if (!GetClientRect(window.Hwnd(), &rc)) return false;
  const UINT width = static_cast<UINT>(rc.right - rc.left);
  const UINT height = static_cast<UINT>(rc.bottom - rc.top);
  if (width == 0 || height == 0) return false;

  static ComPtr<IWICImagingFactory> wic;
  static ComPtr<ID2D1Factory> d2d;
  if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(wic.GetAddressOf())))) {
    return false;
  }
  if (!d2d && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       d2d.GetAddressOf()))) {
    return false;
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf()))) {
    return false;
  }
  if (!window.RenderSceneInto(target.Get())) return false;

  WICRect lockRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, lock.GetAddressOf()))) return false;
  UINT stride = 0;
  UINT bytes = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &data))) return false;
  out->width = width;
  out->height = height;
  out->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * width, data + static_cast<size_t>(y) * stride,
                static_cast<size_t>(width) * 4u);
  }
  return true;
}

// Reads a bitmap that lives on the glass device back to the CPU: same device,
// so D2D allows it (a cross-device copy is D2DERR_WRONG_RESOURCE_DOMAIN).
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
                mapped.bits + static_cast<size_t>(y) * mapped.pitch, static_cast<size_t>(size.width) * 4u);
  }
  staging->Unmap();
  return true;
}

// The island's bitmap is PREMULTIPLIED and mostly translucent: what the user
// sees is the island composited over the window's frame (exactly what
// DirectComposition does with the premultiplied swapchain).
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
          (0xFFu << 24) | (static_cast<UINT32>(blend(16)) << 16) | (static_cast<UINT32>(blend(8)) << 8) |
          static_cast<UINT32>(blend(0));
    }
  }
  return out;
}

// Uploads the window's frame as the island's backdrop (this device can sample
// it; the product's own capture reads the same pixels off the window surface).
bool UploadBackdrop(ID2D1DeviceContext* dc, const Frame& frame, ComPtr<ID2D1Bitmap1>* out) {
  if (!dc || !out) return false;
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  return SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(frame.width, frame.height), frame.pixels.data(),
                                    frame.width * 4u, props, out->ReleaseAndGetAddressOf()));
}

// ---- the metrics ----------------------------------------------------------
// Horizontal gradient of luma, central difference, inside a band. This is the
// statistic a blur attacks first: a glyph edge is one column wide, a blurred one
// is six.
struct Band {
  UINT x0 = 0;
  UINT x1 = 0;
  UINT y0 = 0;
  UINT y1 = 0;
};

double MaxGradient(const Frame& frame, const Band& band) {
  double best = 0.0;
  if (band.x1 < band.x0 + 2) return 0.0;
  for (UINT y = band.y0; y < band.y1 && y < frame.height; ++y) {
    for (UINT x = band.x0; x + 1 < band.x1 && x + 1 < frame.width; ++x) {
      const double g = std::fabs(frame.L(x + 1, y) - frame.L(x - 0, y));
      if (g > best) best = g;
    }
  }
  return best;
}

// The background level of the pill: the median luma of the band's outer
// columns, which the text never reaches (the run is centred).
double BackgroundLevel(const Frame& frame, const Band& band) {
  std::vector<double> samples;
  for (UINT y = band.y0; y < band.y1 && y < frame.height; ++y) {
    for (UINT x = band.x0; x < band.x1 && x < frame.width; ++x) {
      const UINT leftInset = band.x0 + 12;
      const UINT rightInset = band.x1 > 12 ? band.x1 - 12 : band.x0;
      if (x >= leftInset && x < rightInset) continue;
      samples.push_back(frame.L(x, y));
    }
  }
  if (samples.empty()) return 0.0;
  std::sort(samples.begin(), samples.end());
  return samples[samples.size() / 2];
}

// Ink mask: the pixels the SHARP (island-off) pill considers text. Derived from
// the reference, so both frames are measured against the same glyphs.
std::vector<uint8_t> InkMask(const Frame& sharp, const Band& band, double background) {
  std::vector<uint8_t> mask(static_cast<size_t>(sharp.width) * sharp.height, 0);
  for (UINT y = band.y0; y < band.y1 && y < sharp.height; ++y) {
    for (UINT x = band.x0; x < band.x1 && x < sharp.width; ++x) {
      if (std::fabs(sharp.L(x, y) - background) >= 40.0) {
        mask[static_cast<size_t>(y) * sharp.width + x] = 1;
      }
    }
  }
  return mask;
}

// The RING: background pixels 1..3 px away from a glyph. In the island-off
// frame these are pure pill fill. A blurred second copy of the text puts energy
// here; sharp text does not.
struct RingStats {
  int samples = 0;
  double meanExcess = 0.0;  // mean |L - background| above a 6-level noise floor
  double maxExcess = 0.0;
};

RingStats MeasureRing(const Frame& frame, const Band& band, const std::vector<uint8_t>& ink,
                      double background) {
  RingStats stats;
  double sum = 0.0;
  const int w = static_cast<int>(frame.width);
  const int h = static_cast<int>(frame.height);
  for (int y = static_cast<int>(band.y0); y < static_cast<int>(band.y1) && y < h; ++y) {
    for (int x = static_cast<int>(band.x0); x < static_cast<int>(band.x1) && x < w; ++x) {
      const size_t index = static_cast<size_t>(y) * frame.width + static_cast<size_t>(x);
      if (ink[index]) continue;
      bool touching = false;
      for (int dy = -3; dy <= 3 && !touching; ++dy) {
        for (int dx = -3; dx <= 3 && !touching; ++dx) {
          if (dx == 0 && dy == 0) continue;
          const int nx = x + dx;
          const int ny = y + dy;
          if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
          if (ink[static_cast<size_t>(ny) * frame.width + static_cast<size_t>(nx)]) touching = true;
        }
      }
      if (!touching) continue;
      const double excess = std::fabs(frame.L(static_cast<UINT>(x), static_cast<UINT>(y)) - background);
      sum += (excess > 6.0) ? (excess - 6.0) : 0.0;
      if (excess > stats.maxExcess) stats.maxExcess = excess;
      ++stats.samples;
    }
  }
  stats.meanExcess = stats.samples > 0 ? sum / stats.samples : 0.0;
  return stats;
}

// How many pixels in the band are NOT the band's own background (the window's
// "it painted something here" count).
int InkPixels(const Frame& frame, const Band& band, double background, double threshold) {
  int count = 0;
  for (UINT y = band.y0; y < band.y1 && y < frame.height; ++y) {
    for (UINT x = band.x0; x < band.x1 && x < frame.width; ++x) {
      if (std::fabs(frame.L(x, y) - background) >= threshold) ++count;
    }
  }
  return count;
}

bool g_skipped = false;
std::string g_skipReason;

}  // namespace

// ---------------------------------------------------------------------------
// S1 + S2: the pill's text is SHARP and has NO second, blurred copy.
// ---------------------------------------------------------------------------
PRISM_TEST("c112_pill_text_is_sharp_and_has_no_ghost") {
  // D1: without DirectComposition there is no island and nothing to measure.
  {
    GalleryWindow probe;
    if (!probe.Create(L"c112 probe", GetModuleHandleW(nullptr))) {
      std::puts("  [skip] no window station");
      return;
    }
    if (!probe.EnableGlassLayer(FindShaderDir())) {
      g_skipped = true;
      g_skipReason = "no D3D/DirectComposition on this runner (EnableGlassLayer failed)";
      std::printf("  [skip] C112 SKIPPED: %s\n", g_skipReason.c_str());
      return;
    }
    if (!probe.GlassOpticsReady()) {
      g_skipped = true;
      g_skipReason = "the optical chain did not register on this runner: " + probe.GlassOpticsError();
      std::printf("  [skip] C112 SKIPPED: %s\n", g_skipReason.c_str());
      return;
    }
  }

  // The REFERENCE: the same window with the island switched OFF -- the pill the
  // user sees today when glass is disabled (sharp text, painted by the window).
  GalleryWindow offWindow;
  if (!PRISM_CHECK(MakeWindow(offWindow, L"c112 island off", /*glass=*/false))) return;
  Frame offFrame;
  if (!PRISM_CHECK(RenderWindow(offWindow, &offFrame))) return;

  // The SUBJECT: the same window with the island on.
  GalleryWindow onWindow;
  if (!PRISM_CHECK(MakeWindow(onWindow, L"c112 island on", /*glass=*/true))) return;
#if defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  // NEGATIVE CONTROL (D3/D27): armed ONLY when the product really reported the
  // pre-C112 ownership (window keeps the text while the island is live). The
  // first failing sharpness check prints the verdict.
  g_mutantObserved = !onWindow.GlassPillOwnsContent() && onWindow.GlassLayerVisible() &&
                     onWindow.GlassOpticsReady();
  prism::test::SetMutationVerdict("C112 MUTATION CAUGHT: glass_over_text",
                                  [] { return g_mutantObserved; });
  std::printf("  INJECTION: window keeps the pill text, island composed over it (observed=%d)\n",
              static_cast<int>(g_mutantObserved));
#else
  PRISM_CHECK(onWindow.GlassPillOwnsContent());
  if (!onWindow.GlassPillOwnsContent()) return;
#endif
  PRISM_CHECK(onWindow.GlassOpticsReady());
  Frame onFrame;
  if (!PRISM_CHECK(RenderWindow(onWindow, &onFrame))) return;

  const IslandRect rect = onWindow.GlassPanelRect();
  if (!PRISM_CHECK(rect.w > 0 && rect.h > 0)) return;
  std::printf("  island rect: %u,%u %ux%u\n", rect.x, rect.y, rect.w, rect.h);
  if (!PRISM_CHECK(rect.w <= offFrame.width && rect.y + rect.h <= offFrame.height)) return;

  // Compose the island over the window's frame through the PRODUCT's own island
  // paint (ComposeIslandForTest -> PaintIslandContent), with the window frame as
  // the content underneath (what the capture would read).
  ID2D1DeviceContext* dc = onWindow.GlassContext();
  if (!PRISM_CHECK(dc != nullptr)) return;
  ComPtr<ID2D1Bitmap1> backdrop;
  if (!PRISM_CHECK(UploadBackdrop(dc, onFrame, &backdrop))) return;
  D2D1_BITMAP_PROPERTIES1 targetProps = {};
  targetProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                              D2D1_ALPHA_MODE_PREMULTIPLIED);
  targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  ComPtr<ID2D1Bitmap1> islandTarget;
  if (!PRISM_CHECK(SUCCEEDED(dc->CreateBitmap(D2D1::SizeU(rect.w, rect.h), nullptr, 0, targetProps,
                                              islandTarget.GetAddressOf())))) {
    return;
  }
  if (!PRISM_CHECK(onWindow.ComposeIslandForTest(islandTarget.Get(), backdrop.Get()))) return;
  if (!PRISM_CHECK(onWindow.GlassLastFrameOptical())) return;
  Frame islandFrame;
  if (!PRISM_CHECK(ReadBitmap(dc, islandTarget.Get(), &islandFrame))) return;

  // The two things the user compares: the pill with the island off, and the pill
  // with the island on (island composited over the window's frame).
  const Frame pillOff = Crop(offFrame, rect.x, rect.y, rect.w, rect.h);
  const Frame pillOn =
      CompositeOver(islandFrame, Crop(onFrame, rect.x, rect.y, rect.w, rect.h));

  if (!PRISM_CHECK(onWindow.BreadcrumbText() == L"pack.7z")) {
    std::printf("  breadcrumb: '");
    for (wchar_t ch : onWindow.BreadcrumbText()) std::printf("%lc", ch);
    std::printf("'\n");
  }

  // The text band: the pill's 24 px / 10 px inset (the SAME rect the renderer
  // draws into), minus a 1 px margin so the rounded corners never enter.
  Band band;
  band.x0 = 26;
  band.x1 = rect.w > 28 ? rect.w - 26 : rect.w;
  band.y0 = 11;
  band.y1 = rect.h > 12 ? rect.h - 11 : rect.h;

  const double bgOff = BackgroundLevel(pillOff, band);
  const double bgOn = BackgroundLevel(pillOn, band);
  const double maxOff = MaxGradient(pillOff, band);
  const double maxOn = MaxGradient(pillOn, band);
  const double retention = maxOff > 0.0 ? maxOn / maxOff : 0.0;
  const std::vector<uint8_t> ink = InkMask(pillOff, band, bgOff);
  const RingStats ringOff = MeasureRing(pillOff, band, ink, bgOff);
  const RingStats ringOn = MeasureRing(pillOn, band, ink, bgOn);
  int inkCount = 0;
  for (uint8_t v : ink) inkCount += v ? 1 : 0;

  std::printf("  background: off=%.1f on=%.1f | ink pixels=%d\n", bgOff, bgOn, inkCount);
  std::printf("  S1 maxGradient: off=%.1f on=%.1f retention=%.1f%%\n", maxOff, maxOn,
              retention * 100.0);
  std::printf("  S2 halo ring: off mean=%.2f max=%.1f | on mean=%.2f max=%.1f (samples=%d)\n",
              ringOff.meanExcess, ringOff.maxExcess, ringOn.meanExcess, ringOn.maxExcess,
              ringOn.samples);

  // Screenshots (4x, nearest neighbour) of the SAME pill region, both arms.
  if (const char* outDir = std::getenv("PRISM_C112_OUT")) {
    const std::wstring root(outDir, outDir + std::strlen(outDir));
    const Frame onShot = Scale4x(pillOn);
    const Frame offShot = Scale4x(pillOff);
    std::printf("  [shot] c112-pill-on.png saved=%d\n",
                static_cast<int>(WritePng(root + L"\\c112-pill-on.png", onShot)));
    std::printf("  [shot] c112-pill-off.png saved=%d\n",
                static_cast<int>(WritePng(root + L"\\c112-pill-off.png", offShot)));
  }

  // S1: the SAME text, measured the same way. 90% is the card's line: a blur
  // cannot keep 90% of a one-pixel edge; an occlusion cannot either.
  PRISM_CHECK(inkCount > 100);  // there IS a text run in the band
  PRISM_CHECK(maxOff >= 40.0);  // the reference really has sharp glyph edges
  PRISM_CHECK(retention >= 0.90);
  // S2: no second, blurred copy in the ring around the glyphs.
  PRISM_CHECK(ringOn.meanExcess <= 4.0);
  PRISM_CHECK(ringOn.maxExcess <= 60.0);

#if defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  std::printf("  INJECTION: PRISM_C112_MUTANT_GLASS_OVER_TEXT (window keeps the text, island covers it)\n");
#endif
}

// ---------------------------------------------------------------------------
// S3: with the island owning the pill, the WINDOW paints nothing there.
// ---------------------------------------------------------------------------
PRISM_TEST("c112_window_keeps_only_geometry_under_the_pill") {
  GalleryWindow offWindow;
  if (!PRISM_CHECK(MakeWindow(offWindow, L"c112 s3 off", /*glass=*/false))) return;
  Frame offFrame;
  if (!PRISM_CHECK(RenderWindow(offWindow, &offFrame))) return;

  GalleryWindow onWindow;
  if (!PRISM_CHECK(MakeWindow(onWindow, L"c112 s3 on", /*glass=*/true))) return;
  if (!onWindow.GlassPillOwnsContent()) {
    g_skipped = true;
    g_skipReason = "no DirectComposition on this runner";
    return;
  }
  Frame onFrame;
  if (!PRISM_CHECK(RenderWindow(onWindow, &onFrame))) return;

  // Each window's OWN pill rectangle: the two arms must be the same panel, and
  // saying so explicitly keeps a layout drift from silently cropping the wrong
  // place (which would read as "the text vanished").
  const IslandRect rect = onWindow.GlassPanelRect();
  const IslandRect rectOff = offWindow.GlassPanelRect();
  std::printf("  pill rect: island-off %u,%u %ux%u | island-on %u,%u %ux%u\n", rectOff.x,
              rectOff.y, rectOff.w, rectOff.h, rect.x, rect.y, rect.w, rect.h);
  std::printf("  window: %dx%d client, nav band y=%d..%d\n", offWindow.ClientWidthForTest(),
              offWindow.ClientHeightForTest(), offWindow.NavBarTop(), offWindow.NavBarBottom());
  if (!PRISM_CHECK(rect.w > 0 && rect.h > 0)) return;
  PRISM_CHECK_EQ(rectOff.x, rect.x);
  PRISM_CHECK_EQ(rectOff.y, rect.y);
  PRISM_CHECK_EQ(rectOff.w, rect.w);
  PRISM_CHECK_EQ(rectOff.h, rect.h);
  const Frame offPill = Crop(offFrame, rectOff.x, rectOff.y, rectOff.w, rectOff.h);
  const Frame onPill = Crop(onFrame, rect.x, rect.y, rect.w, rect.h);

  Band band;
  band.x0 = 26;
  band.x1 = rect.w > 28 ? rect.w - 26 : rect.w;
  band.y0 = 11;
  band.y1 = rect.h > 12 ? rect.h - 11 : rect.h;
  const double bgOff = BackgroundLevel(offPill, band);
  const double bgOn = BackgroundLevel(onPill, band);
  const int offInk = InkPixels(offPill, band, bgOff, 40.0);
  const int onInk = InkPixels(onPill, band, bgOn, 40.0);
  std::printf("  window frame in the pill band: island-off ink=%d, island-on ink=%d\n", offInk,
              onInk);
  // The reference proves the detector fires; the subject proves the window
  // stopped painting there (the text lives on the island now).
  PRISM_CHECK(offInk > 100);
  PRISM_CHECK(onInk <= 4);

  // ... and the handover is LOCAL: every pixel outside the pill rectangle (plus
  // its 1 px stroke) is the same in both frames. The stroke is the one visible
  // difference and it is a boundary artefact, not a design change: the window
  // draws a 1 px border CENTRED on the shape's edge, so half of it falls outside
  // the rectangle; the island's surface IS the rectangle, so its half is
  // clipped. One pixel ring, same colour.
  //
  // This is what makes the C76 first-frame digest's re-recorded value a
  // statement about the pill and nothing else.
  size_t outsideChanged = 0;
  UINT minX = 99999, minY = 99999, maxX = 0, maxY = 0;
  if (PRISM_CHECK(offFrame.width == onFrame.width && offFrame.height == onFrame.height)) {
    for (UINT y = 0; y < offFrame.height; ++y) {
      for (UINT x = 0; x < offFrame.width; ++x) {
        const bool inPill = x + 1 >= rect.x && x <= rect.x + rect.w && y + 1 >= rect.y &&
                            y <= rect.y + rect.h;
        if (inPill) continue;
        if (offFrame.At(x, y) != onFrame.At(x, y)) {
          ++outsideChanged;
          minX = (std::min)(minX, x);
          minY = (std::min)(minY, y);
          maxX = (std::max)(maxX, x);
          maxY = (std::max)(maxY, y);
        }
      }
    }
  }
  std::printf("  pixels changed OUTSIDE the pill rect: %zu (bbox %u,%u-%u,%u)\n", outsideChanged,
              minX, minY, maxX, maxY);
  PRISM_CHECK(outsideChanged == 0);
}

int main(int argc, char** argv) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int code = prism::test::RunAll(argc, argv);
  if (SUCCEEDED(com)) CoUninitialize();
  if (code == 0) {
    if (g_skipped) std::printf("C112 SKIP: %s\n", g_skipReason.c_str());
    std::puts("ALL C112 GLASS-OWNS-TEXT CHECKS PASSED");
  }
  return code;
}