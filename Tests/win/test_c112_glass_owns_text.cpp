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
#include "BenchFrameCapture.hpp"
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
  int outerW = 1280, outerH = 800;
  if (const char* size = std::getenv("PRISM_C112_WINDOW")) {
    std::sscanf(size, "%dx%d", &outerW, &outerH);
  }
  SetWindowPos(window.Hwnd(), HWND_TOP, 40, 40, outerW, outerH, SWP_SHOWWINDOW | SWP_NOACTIVATE);
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

// The frame the window paints, through C113's capture seam
// (GalleryWindow::RenderSceneForCapture): the renderer's DEVICE objects are
// rebuilt on the WIC target, so the picture is the scene and not a frame D2D
// threw away. The reader also reports `painted` -- a canvas-only frame scores 0 --
// which every test below asserts BEFORE it compares a single pixel.
prism::win::benchframe::Frame CaptureWindow(GalleryWindow& window, const char* label, Frame* out) {
  using namespace prism::win::benchframe;
  // NOT static: an ASan run on CI caught the static factories being released
  // after D2D's own teardown (access violation in
  // ComPtr<ID2D1RenderTarget>::InternalRelease, at exit). Four captures pay four
  // factory creations; a process that crashes on the way out pays for every
  // future run.
  ComPtr<IWICImagingFactory> wic;
  ComPtr<ID2D1Factory> d2d;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.GetAddressOf())))) {
    return prism::win::benchframe::Frame{};
  }
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))) {
    return prism::win::benchframe::Frame{};
  }
  RECT rc{};
  if (!GetClientRect(window.Hwnd(), &rc)) return prism::win::benchframe::Frame{};
  const uint32_t width = static_cast<uint32_t>(rc.right - rc.left);
  const uint32_t height = static_cast<uint32_t>(rc.bottom - rc.top);
  const prism::win::benchframe::Frame captured =
      CaptureOffscreen(window, wic.Get(), d2d.Get(), width, height, label, Seam::kProduct);
  std::printf("  frame[%s]: rendered=%d hr=0x%08lX painted=%zu/%zu digest=%s\n", label,
              static_cast<int>(captured.rendered), static_cast<unsigned long>(captured.hr),
              captured.painted, captured.total, Hex(captured.digest).c_str());
  if (!captured.rendered || out == nullptr) return captured;
  out->width = captured.width;
  out->height = captured.height;
  out->pixels.assign(captured.total, 0u);
  for (size_t i = 0; i < captured.total; ++i) {
    const uint8_t* px = &captured.bgra[i * 4u];
    out->pixels[i] = (0xFFu << 24) | (static_cast<UINT32>(px[2]) << 16) |
                     (static_cast<UINT32>(px[1]) << 8) | static_cast<UINT32>(px[0]);
  }
  return captured;
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

// The pill as the USER sees it: the island's own content (ComposeIslandForTest
// -> PaintIslandContent, the shipping PresentGlassFrame body) composited over
// the window's frame at the pill rectangle. Both arms of every comparison below
// go through this, so "island off" and "island on" differ by WHO paints the
// pill and by nothing else.
bool ComposePillOn(GalleryWindow& window, const Frame& windowFrame, Frame* pill) {
  const IslandRect rect = window.GlassPanelRect();
  if (rect.w == 0 || rect.h == 0 || pill == nullptr) return false;
  ID2D1DeviceContext* dc = window.GlassContext();
  if (dc == nullptr) return false;
  ComPtr<ID2D1Bitmap1> backdrop;
  if (!UploadBackdrop(dc, windowFrame, &backdrop)) return false;
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(rect.w, rect.h), nullptr, 0, props, target.GetAddressOf()))) {
    return false;
  }
  if (!window.ComposeIslandForTest(target.Get(), backdrop.Get())) return false;
  Frame island;
  if (!ReadBitmap(dc, target.Get(), &island)) return false;
  *pill = CompositeOver(island, Crop(windowFrame, rect.x, rect.y, rect.w, rect.h));
  return true;
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

  // ---------------------------------------------------------------- C123
  // THIS SUITE WAS REWRITTEN BY CARD C123, and every assertion below says why.
  //
  // C112 measured the PILL's island: S1/S2 asked whether the composed pill kept
  // the glyph edges (>= 90 % retention), S3 asked whether the window had stopped
  // painting the text the island owned. C123 removes the pill: the crumb is a
  // 28 px PATH BAR painted into the page, one row under a flush toolbar, so
  // there is no surface that could double-paint it and no handover to measure.
  //
  // What survives is the part that can still fail: the layer must publish NO
  // island rectangle, and the WINDOW must really paint the path (ink inside the
  // layout's own text box). A resurrected pill island -- exactly the pre-card
  // defect, and what PRISM_C112_MUTANT_GLASS_OVER_TEXT arms -- turns the first
  // assertion red through its runtime predicate (D3/D27).
  GalleryWindow offWindow;
  if (!PRISM_CHECK(MakeWindow(offWindow, L"c112 crumb off", /*glass=*/false))) return;
  Frame offFrame;
  const prism::win::benchframe::Frame offCapture = CaptureWindow(offWindow, "crumb-off", &offFrame);
  if (!PRISM_CHECK(offCapture.rendered)) return;
  PRISM_CHECK(offCapture.painted * 20 >= offCapture.total);  // >= 5% painted

  GalleryWindow onWindow;
  if (!PRISM_CHECK(MakeWindow(onWindow, L"c112 crumb on", /*glass=*/true))) return;
#if defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  // NEGATIVE CONTROL (D3/D27): armed when the pre-C112 pill island is really
  // back over the band. The first failing "no island" check prints the verdict.
  g_mutantObserved = onWindow.GlassPanelRect().w != 0;
  prism::test::SetMutationVerdict("C112 MUTATION CAUGHT: glass_over_text",
                                  [] { return g_mutantObserved; });
  std::printf("  INJECTION: the pill island is back over the band (observed=%d)\n",
              static_cast<int>(g_mutantObserved));
#else
  PRISM_CHECK(!onWindow.GlassPillOwnsContent());
#endif
  PRISM_CHECK(onWindow.GlassOpticsReady());
  const IslandRect rect = onWindow.GlassPanelRect();
  std::printf("  island rect: %u,%u %ux%u (C123: none)\n", rect.x, rect.y, rect.w, rect.h);
  PRISM_CHECK_EQ(rect.w, 0u);
  PRISM_CHECK_EQ(rect.h, 0u);

  Frame onFrame;
  const prism::win::benchframe::Frame onCapture = CaptureWindow(onWindow, "crumb-on", &onFrame);
  if (!PRISM_CHECK(onCapture.rendered)) return;
  PRISM_CHECK(onCapture.painted * 20 >= onCapture.total);

  if (!PRISM_CHECK(onWindow.BreadcrumbText() == L"pack.7z")) return;
  // The text box is the LAYOUT's (contentBand.left + 16 .. - 16, 3 px inside the
  // 28 px row) -- the same rectangle the window hit-tests and the renderer's
  // LEADING run draws into. Sampling anywhere else would be measuring a guess.
  const prism::layout::Layout layout = onWindow.LayoutForTest();
  const prism::layout::Rect box = prism::layout::CrumbTextRect(layout);
  std::printf("  crumb bar: %d,%d %dx%d | text box: %d,%d %dx%d\n", layout.crumbBar.left,
              layout.crumbBar.top, layout.crumbBar.Width(), layout.crumbBar.Height(), box.left,
              box.top, box.Width(), box.Height());
  if (!PRISM_CHECK(!box.Empty() && !layout.crumbBar.Empty())) return;
  Band band;
  band.x0 = static_cast<UINT>(box.left);
  band.x1 = static_cast<UINT>(box.right);
  band.y0 = static_cast<UINT>(box.top);
  band.y1 = static_cast<UINT>(box.bottom);
  const double background = BackgroundLevel(onFrame, band);
  const int ink = InkPixels(onFrame, band, background, 40.0);
  std::printf("  path ink: %d pixels inside the text box (background %.1f)\n", ink, background);
  // The window really paints the path (a blank bar would score 0)...
  PRISM_CHECK(ink > 50);
  // ...and the same ink is present in the arm with the composition layer off,
  // i.e. nothing above the window is contributing it.
  const int offInk = InkPixels(offFrame, band, background, 40.0);
  std::printf("  path ink with the layer off: %d\n", offInk);
  PRISM_CHECK(offInk > 50);

  if (const char* outDir = std::getenv("PRISM_C112_OUT")) {
    const std::wstring root(outDir, outDir + std::strlen(outDir));
    std::printf("  [shot] c112-path-bar.png saved=%d\n",
                static_cast<int>(WritePng(root + L"\\c112-path-bar.png", Scale4x(onFrame))));
  }

#if defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
  std::printf("  INJECTION: PRISM_C112_MUTANT_GLASS_OVER_TEXT (a pill island over the band)\n");
#endif
}

// ---------------------------------------------------------------------------
// S3: the WINDOW paints the path, in BOTH arms (C123's replacement for "the
// window stopped painting what the island owned": with no island, every pixel
// of the crumb comes from the window, so the two arms must agree there).
// ---------------------------------------------------------------------------
PRISM_TEST("c112_no_island_over_the_crumb: both arms paint the same path") {
  GalleryWindow offWindow;
  if (!PRISM_CHECK(MakeWindow(offWindow, L"c112 s3 off", /*glass=*/false))) return;
  Frame offFrame;
  const prism::win::benchframe::Frame offCapture = CaptureWindow(offWindow, "s3-off", &offFrame);
  if (!PRISM_CHECK(offCapture.rendered)) return;
  PRISM_CHECK(offCapture.painted * 20 >= offCapture.total);

  GalleryWindow onWindow;
  if (!PRISM_CHECK(MakeWindow(onWindow, L"c112 s3 on", /*glass=*/true))) return;
  if (!onWindow.GlassOpticsReady()) {
    g_skipped = true;
    g_skipReason = "the optical chain did not register on this runner: " +
                   onWindow.GlassOpticsError();
    std::printf("  [skip] C112 SKIPPED: %s\n", g_skipReason.c_str());
    return;
  }
  Frame onFrame;
  const prism::win::benchframe::Frame onCapture = CaptureWindow(onWindow, "s3-on", &onFrame);
  if (!PRISM_CHECK(onCapture.rendered)) return;
  PRISM_CHECK(onCapture.painted * 20 >= onCapture.total);

  const IslandRect rect = onWindow.GlassPanelRect();
  const IslandRect rectOff = offWindow.GlassPanelRect();
  std::printf("  island rect: off %u,%u %ux%u | on %u,%u %ux%u (C123: both none)\n", rectOff.x,
              rectOff.y, rectOff.w, rectOff.h, rect.x, rect.y, rect.w, rect.h);
  PRISM_CHECK_EQ(rect.w, 0u);
  PRISM_CHECK_EQ(rect.h, 0u);
  PRISM_CHECK_EQ(rectOff.w, 0u);
  PRISM_CHECK_EQ(rectOff.h, 0u);

  const prism::layout::Layout layout = onWindow.LayoutForTest();
  const prism::layout::Rect box = prism::layout::CrumbTextRect(layout);
  Band band;
  band.x0 = static_cast<UINT>(box.left);
  band.x1 = static_cast<UINT>(box.right);
  band.y0 = static_cast<UINT>(box.top);
  band.y1 = static_cast<UINT>(box.bottom);
  if (!PRISM_CHECK(!box.Empty())) return;
  const double bgOn = BackgroundLevel(onFrame, band);
  const double bgOff = BackgroundLevel(offFrame, band);
  const std::vector<uint8_t> glyphs = InkMask(onFrame, band, bgOn);
  int glyphPixels = 0;
  int glyphsOff = 0;
  for (UINT y = band.y0; y < band.y1 && y < onFrame.height; ++y) {
    for (UINT x = band.x0; x < band.x1 && x < onFrame.width; ++x) {
      if (!glyphs[static_cast<size_t>(y) * onFrame.width + x]) continue;
      ++glyphPixels;
      if (std::fabs(offFrame.L(x, y) - bgOff) >= 40.0) ++glyphsOff;
    }
  }
  std::printf("  glyph pixels in the path band: %d, of which the layer-off arm paints %d\n",
              glyphPixels, glyphsOff);
  PRISM_CHECK(glyphPixels > 50);
  // Every glyph the window paints with the layer on is painted with it off too:
  // the composition layer contributes no crumb ink (it has no surface to own).
  PRISM_CHECK(glyphsOff * 100 >= glyphPixels * 90);
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