// PrismArchive -- card C100: the action band's PURGE (no mnemonic letters) and
// its eight CURVE icons.
//
// Two defects, one file:
//
//   1. "单字母牛皮癣": every button painted a 10 px O/E/C/A/D/T/V/S in its
//      top-right corner (ActionBar.hpp:558-572) from a second DWrite format.
//      The letters were decoration, not the shortcut -- the window claims an
//      Alt chord in WM_SYSKEYDOWN and asks the MODEL (Model::PressMnemonic),
//      and the UIA node already publishes shortcut = "Alt+O". So they go, and
//      the painted band must report ZERO of them.
//   2. The silhouettes were 23 DrawLine calls and 0 curves; the compress glyph
//      was four straight lines from the corners to the middle, i.e. a big X.
//      All eight icons are now ID2D1PathGeometry + AddBezier/AddArc stroked
//      through ONE round-cap ID2D1StrokeStyle.
//
// The assertions are on the DEVICE: the painter's per-frame counters, the
// stroke style's own cap/join properties, and rendered pixels from the real
// GalleryRenderer -> ActionBarPainter path (never a mock, never a mock-up).
//
// Negative controls (CONTRACTS D3 + D26/D27, same source, compile-time arms):
//   PRISM_C100_MUTANT_MNEMONIC      the corner letter comes back  -> test 1 red
//   PRISM_C100_MUTANT_LINE_COMPRESS the four-line X comes back    -> test 2 red
// Both are registered under PASS_REGULAR_EXPRESSION (never WILL_FAIL) and the
// marker is printed at the FIRST failed assertion.
//
// /WX- is the project's Windows-test convention (SDK headers are not /W4).
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
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
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;
using prism::actionbar::ActionId;
using prism::actionbar::Context;
using prism::actionbar::Rect;

#if defined(PRISM_C100_MUTANT_MNEMONIC)
inline constexpr const char* kMutantBanner = "C100 MUTANT ACTIVE: mnemonic";
inline constexpr const char* kMutantMarker = "C100 MUTATION CAUGHT: mnemonic";
#elif defined(PRISM_C100_MUTANT_LINE_COMPRESS)
inline constexpr const char* kMutantBanner = "C100 MUTANT ACTIVE: line_compress";
inline constexpr const char* kMutantMarker = "C100 MUTATION CAUGHT: line_compress";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

namespace {

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;
// CONTRACTS D1: a suite that cannot run (no window station, no WIC) says so and
// leaves with the SKIP code; its CTest entry carries a PASS_REGULAR_EXPRESSION.
constexpr int kSkipReturnCode = 77;

IWICImagingFactory* gWic = nullptr;
const char* gOutDir = nullptr;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

// Everything one real paint of the band left behind: the pixels, the geometry
// the band was given, and the painter's device-state readbacks.
struct Painted {
  bool ok = false;
  Frame frame;
  prism::actionbar::Layout layout;
  int mnemonicGlyphs = 0;
  int textFormats = 0;
  int curveCount[prism::actionbar::kActionCount] = {};
  bool strokeBound = false;
  D2D1_CAP_STYLE startCap = D2D1_CAP_STYLE_FLAT;
  D2D1_CAP_STYLE endCap = D2D1_CAP_STYLE_FLAT;
  D2D1_CAP_STYLE dashCap = D2D1_CAP_STYLE_FLAT;
  D2D1_LINE_JOIN join = D2D1_LINE_JOIN_MITER;
};

prism::actionbar::Layout Band1280() {
  return prism::actionbar::Compute(
      prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                             /*sidebarVisible=*/false, /*listMode=*/false));
}

bool PaintBand(const prism::actionbar::Model& model, Painted* out) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.actions = &model;
  scene.actionHover = -1;
  scene.actionPressed = -1;
  scene.theme = prism::theme::Variant::Midnight;
  scene.breadcrumb = L"Design_Assets_v4.7z";

  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kWidth), static_cast<INT>(kHeight)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  out->frame.width = kWidth;
  out->frame.height = kHeight;
  out->frame.pixels.resize(static_cast<size_t>(kWidth) * kHeight);
  for (UINT32 y = 0; y < kHeight; ++y) {
    std::memcpy(out->frame.pixels.data() + static_cast<size_t>(y) * kWidth,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(kWidth) * 4);
  }

  // Read the DEVICE state back while the renderer (and its painter) live.
  const prism::win::actionbar::ActionBarPainter& painter = renderer.ActionBand();
  out->mnemonicGlyphs = painter.MnemonicGlyphsDrawn();
  out->textFormats = painter.TextFormatCount();
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    out->curveCount[i] = painter.IconCurveCount(i);
  }
  out->strokeBound = painter.StrokeStyleBound();
  out->startCap = painter.StrokeStartCap();
  out->endCap = painter.StrokeEndCap();
  out->dashCap = painter.StrokeDashCap();
  out->join = painter.StrokeJoin();
  out->layout = Band1280();
  out->ok = true;
  return true;
}

bool WritePng(const wchar_t* path, UINT32 width, UINT32 height,
              const std::vector<UINT32>& pixels) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
          static_cast<UINT>(pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(pixels.data())),
          bitmap.GetAddressOf()))) {
    return false;
  }
  return SUCCEEDED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                       encoder.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                           bag.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(frame->Initialize(bag.Get())) && SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

// The band strip (y = 20..120), the same crop the C78 assets use.
void SaveBand(const wchar_t* name, const Frame& frame) {
  if (gOutDir == nullptr) return;
  const int top = 20, bottom = 120;
  std::vector<UINT32> crop(static_cast<size_t>(kWidth) * (bottom - top));
  for (int y = top; y < bottom; ++y) {
    std::memcpy(crop.data() + static_cast<size_t>(y - top) * kWidth,
                frame.pixels.data() + static_cast<size_t>(y) * kWidth,
                static_cast<size_t>(kWidth) * 4);
  }
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%ls", gOutDir, name);
  WritePng(path, kWidth, static_cast<UINT32>(bottom - top), crop);
}

// The eight 24 px icon boxes, magnified 4x with a nearest-neighbour copy: the
// pixels are the renderer's own, only bigger, so the curve evidence can be read
// in the PR without a zooming tool. No drawing, no re-synthesis.
void SaveIconRow(const wchar_t* name, const Frame& frame,
                 const prism::actionbar::Layout& layout) {
  if (gOutDir == nullptr || layout.buttons.size() != prism::actionbar::kActionCount) return;
  constexpr int kZoom = 4;
  constexpr int kGap = 6;
  constexpr int kPad = 8;
  constexpr int kCell = prism::actionbar::kIconBox;
  const int cell = kCell * kZoom;
  const int width = kPad * 2 + static_cast<int>(layout.buttons.size()) * cell +
                    (static_cast<int>(layout.buttons.size()) - 1) * kGap;
  const int height = kPad * 2 + cell;
  std::vector<UINT32> pixels(static_cast<size_t>(width) * height, 0xFF000000u);
  for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
    const Rect icon = prism::actionbar::IconBoxRect(layout.buttons[i].rect);
    const int ox = kPad + static_cast<int>(i) * (cell + kGap);
    for (int y = 0; y < cell; ++y) {
      for (int x = 0; x < cell; ++x) {
        const int sx = icon.left + x / kZoom;
        const int sy = icon.top + y / kZoom;
        if (!frame.In(sx, sy)) continue;
        pixels[static_cast<size_t>(kPad + y) * width +
               static_cast<size_t>(ox + x)] = frame.At(sx, sy);
      }
    }
  }
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%ls", gOutDir, name);
  WritePng(path, static_cast<UINT32>(width), static_cast<UINT32>(height), pixels);
}

// The island fill, sampled inside its own padding and away from every button.
UINT32 IslandFill(const Painted& painted) {
  const Rect& island = painted.layout.island;
  return painted.frame.At(island.left + 3, (island.top + island.bottom) / 2);
}

int InkInBox(const Frame& frame, int left, int top, int right, int bottom, UINT32 fill,
             int threshold) {
  int count = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      if (ColorDistance(frame.At(x, y), fill) > threshold) ++count;
    }
  }
  return count;
}

// The longest run of consecutive ink samples along one ray out of the icon
// centre. A stroke that CROSSES the ray gives 2-3 samples; a stroke that RUNS
// ALONG it (the old four-line X) gives a long run. That difference is the whole
// "this is a cross, not a parcel" measurement.
int LongestInkRun(const Frame& frame, float cx, float cy, double degrees, UINT32 fill,
                  int threshold) {
  const double radians = degrees * 3.14159265358979323846 / 180.0;
  const double dx = std::cos(radians);
  const double dy = -std::sin(radians);  // screen y grows downward
  int best = 0;
  int run = 0;
  for (double r = 2.0; r <= 7.0; r += 1.0) {
    const int x = static_cast<int>(std::lround(cx + dx * r));
    const int y = static_cast<int>(std::lround(cy + dy * r));
    if (!frame.In(x, y)) break;
    if (ColorDistance(frame.At(x, y), fill) > threshold) {
      ++run;
      best = (std::max)(best, run);
    } else {
      run = 0;
    }
  }
  return best;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. the mnemonic letters are gone from the DEVICE, not just from the source
// ---------------------------------------------------------------------------
PRISM_TEST("c100 the band paints zero mnemonic letters") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  Painted painted;
  if (!PRISM_CHECK(PaintBand(model, &painted))) return;
  std::printf("  [c100] mnemonic glyphs=%d text formats=%d\n", painted.mnemonicGlyphs,
              painted.textFormats);

  PRISM_CHECK_EQ(painted.mnemonicGlyphs, 0);
  // One text format left: the label. A second one was the corner letter's.
  PRISM_CHECK_EQ(painted.textFormats, 1);

  // ... and the pixels agree. The old letter box was
  // (right-18 .. right-5, top+5 .. top+17); after the purge it is bare island on
  // every one of the eight buttons, so "the letter moved" cannot pass.
  const UINT32 fill = IslandFill(painted);
  int worst = 0;
  int dirtyButtons = 0;
  for (const prism::actionbar::Button& button : painted.layout.buttons) {
    const int ink = InkInBox(painted.frame, button.rect.right - 18, button.rect.top + 5,
                             button.rect.right - 5, button.rect.top + 17, fill, 25);
    worst = (std::max)(worst, ink);
    if (ink != 0) ++dirtyButtons;
  }
  std::printf("  [c100] mnemonic corner ink: worst button=%d dirty buttons=%d\n", worst,
              dirtyButtons);
  PRISM_CHECK_EQ(dirtyButtons, 0);

  SaveBand(L"c100-actionbar.png", painted.frame);
  SaveIconRow(L"c100-icons.png", painted.frame, painted.layout);
}

// ---------------------------------------------------------------------------
// 2. the compress icon is a packed box, NOT a cross (rendered pixels)
// ---------------------------------------------------------------------------
PRISM_TEST("c100 the compress icon is a box with straps, not a cross") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  Painted painted;
  if (!PRISM_CHECK(PaintBand(model, &painted))) return;

  const int index = prism::actionbar::IndexOf(ActionId::Compress);
  if (!PRISM_CHECK(index >= 0)) return;
  const Rect icon = prism::actionbar::IconBoxRect(
      painted.layout.buttons[static_cast<std::size_t>(index)].rect);
  const int cx = icon.left + icon.Width() / 2;
  const int cy = icon.top + icon.Height() / 2;
  const UINT32 fill = IslandFill(painted);

  // (a) the exact centre pixel is background: the four lines used to meet
  //     within a pixel and a half of it.
  const int centre = ColorDistance(painted.frame.At(cx, cy), fill);
  std::printf("  [c100] compress centre distance=%d\n", centre);
  PRISM_CHECK(centre <= 6);

  // (b) nor does any neighbour: a 3 x 3 window around the centre is paper.
  int window = 0;
  for (int y = cy - 1; y <= cy + 1; ++y) {
    for (int x = cx - 1; x <= cx + 1; ++x) {
      if (!painted.frame.In(x, y)) continue;
      window = (std::max)(window, ColorDistance(painted.frame.At(x, y), fill));
    }
  }
  std::printf("  [c100] compress centre window max distance=%d\n", window);
  PRISM_CHECK(window <= 12);

  // (c) no stroke RUNS toward a diagonal from the centre. A crossing edge is a
  //     2-3 sample blip; the old X put a near-radial line in all four quadrants.
  int radialRuns = 0;
  int worstRun = 0;
  for (int quadrant = 0; quadrant < 4; ++quadrant) {
    const double base = quadrant == 0   ? 45.0
                        : quadrant == 1 ? 135.0
                        : quadrant == 2 ? 225.0
                                        : 315.0;
    for (double offset = -15.0; offset <= 15.0; offset += 5.0) {
      const int run = LongestInkRun(painted.frame, static_cast<float>(cx),
                                    static_cast<float>(cy), base + offset, fill, 40);
      worstRun = (std::max)(worstRun, run);
      if (run >= 5) ++radialRuns;
    }
  }
  std::printf("  [c100] compress radial runs: rays with a stroke=%d worst run=%d px\n",
              radialRuns, worstRun);
  PRISM_CHECK_EQ(radialRuns, 0);

  // (d) the icon really paints (a bare box would satisfy (a)-(c) trivially).
  const int ink = InkInBox(painted.frame, icon.left, icon.top, icon.right, icon.bottom, fill,
                           40);
  std::printf("  [c100] compress ink pixels=%d\n", ink);
  PRISM_CHECK(ink >= 25);
}

// ---------------------------------------------------------------------------
// 3. every icon is a CURVE drawing: >= 2 bezier/arc primitives each
// ---------------------------------------------------------------------------
PRISM_TEST("c100 every icon is built from at least two curve primitives") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  Painted painted;
  if (!PRISM_CHECK(PaintBand(model, &painted))) return;

  const UINT32 fill = IslandFill(painted);
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(i);
    const Rect icon = prism::actionbar::IconBoxRect(
        painted.layout.buttons[static_cast<std::size_t>(i)].rect);
    const int ink = InkInBox(painted.frame, icon.left, icon.top, icon.right, icon.bottom, fill,
                             40);
    std::printf("  [c100] %-8s curves=%d ink=%d\n", std::string(spec.icon).c_str(),
                painted.curveCount[i], ink);
    // At least two curve primitives (AddBezier / AddArc), per the card.
    PRISM_CHECK(painted.curveCount[i] >= 2);
    // ... and the glyph is actually on the device.
    PRISM_CHECK(ink >= 12);
  }
  // One ink width for the whole family, inside the blueprint's 1.8-2.0 band.
  const float width = prism::win::actionbar::ActionBarPainter::GlyphStrokeWidth();
  std::printf("  [c100] glyph stroke width=%.2f px\n", static_cast<double>(width));
  PRISM_CHECK(width >= 1.8f);
  PRISM_CHECK(width <= 2.0f);
}

// ---------------------------------------------------------------------------
// 4. round caps: read back off the DEVICE stroke style, not a comment
// ---------------------------------------------------------------------------
PRISM_TEST("c100 the glyph stroke style has round caps and round joins") {
  prism::actionbar::Model model;
  model.SetContext(Context{true, true, false});
  Painted painted;
  if (!PRISM_CHECK(PaintBand(model, &painted))) return;

  std::printf("  [c100] stroke style bound=%d start=%d end=%d dash=%d join=%d\n",
              painted.strokeBound ? 1 : 0, static_cast<int>(painted.startCap),
              static_cast<int>(painted.endCap), static_cast<int>(painted.dashCap),
              static_cast<int>(painted.join));
  PRISM_CHECK(painted.strokeBound);
  PRISM_CHECK_EQ(static_cast<int>(D2D1_CAP_STYLE_ROUND), static_cast<int>(painted.startCap));
  PRISM_CHECK_EQ(static_cast<int>(D2D1_CAP_STYLE_ROUND), static_cast<int>(painted.endCap));
  PRISM_CHECK_EQ(static_cast<int>(D2D1_CAP_STYLE_ROUND), static_cast<int>(painted.dashCap));
  PRISM_CHECK_EQ(static_cast<int>(D2D1_LINE_JOIN_ROUND), static_cast<int>(painted.join));
}

int main(int argc, char** argv) {
  gOutDir = argc > 1 ? argv[1] : nullptr;
  if (gOutDir != nullptr) std::filesystem::create_directories(gOutDir);
  if (kMutantBanner != nullptr) std::printf("%s\n", kMutantBanner);
  if (kMutantMarker != nullptr) prism::test::SetMutationVerdict(kMutantMarker);

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("C100 SKIP: CoInitializeEx failed (no COM on this window station)");
    return kSkipReturnCode;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,
                              reinterpret_cast<void**>(&gWic)))) {
    std::puts("C100 SKIP: no WIC imaging factory");
    return kSkipReturnCode;
  }
  const int result = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (result != 0) {
    std::puts("C100 ICON / MNEMONIC CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C100 ICON AND MNEMONIC CHECKS PASSED");
  return 0;
}
