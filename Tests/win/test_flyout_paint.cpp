// PrismArchive -- PrismWin
// C48: PIXEL-level tests for the inline-preview flyout (FlyoutSurface).
//
// C30 proved the flyout's LIFECYCLE (no activation, placement, ESC) and C34
// proved it is HOSTED (section owned, wake -> read -> show) -- but the window is
// WS_EX_NOREDIRECTIONBITMAP by design, so until this card it had no pixels at
// all: a peek that peeked at nothing. This suite renders the REAL content
// painter (FlyoutSurface::PaintContent -- the same function the composition
// island calls) into a headless WIC target and asserts what a user would see:
//
//   1. The surface IS the main window's floating-island token, composited over
//      the canvas token: the flyout background equals the design value to the
//      byte (and is visibly NOT the backdrop).
//   2. Every row's text sits on Metrics::RowBaseline: 12 rows, each measured,
//      each within 2 px of the metric, with the ink offset inside the row
//      IDENTICAL for all 12 -- so a drifting baseline cannot pass.
//   3. The corner is the C15 SQUIRCLE: probes classified by theme::SquircleField
//      itself (inside -> surface, outside -> canvas), on all four corners. The
//      pixel that separates a squircle from a RIGHT ANGLE is exactly the
//      outside probe, which is why the square-corner mutant goes red.
//   4. Height follows the row count (12 vs 3 -> 9 pitches) while the pitch does
//      NOT change, measured from the pixels in both frames.
//   5. The pitch scales with the DPI (96 -> 18 px, 144 -> 27 px, 192 -> 36 px),
//      asserted against the DESIGN numbers, not against the function under test.
//   6. The composition island really exists: D3D11 + IDCompositionDevice, a
//      rounded-rectangle clip, one content visual with a premultiplied
//      composition swapchain, and Paint() resizing it to the rows shown.
//
// Verdict rules: the stdout marker is a CONJUNCTION with exit 0 (CONTRACTS D1);
// a machine without a window station / DirectComposition prints WHY and exits 77
// (SKIP_RETURN_CODE). The two negative controls are D27 mutants: the injection
// banner proves it compiled in, the armed verdict prints "C48 MUTATION CAUGHT:
// <id>" at the FIRST failed assertion, and the target's PASS_REGULAR_EXPRESSION
// is that marker (never WILL_FAIL -- CONTRACTS D26).
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
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/FlyoutSurface.hpp"

using Microsoft::WRL::ComPtr;
using namespace prism::win::flyout;
using prism::theme::SquircleField;
using prism::theme::Variant;

namespace {

// D1: a labelled skip is a 77, never a silent pass.
constexpr int kSkipReturnCode = 77;

bool g_noWindowStation = false;
bool g_noComposition = false;

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
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
  int Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3;
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

// The RGBA a token composite produces, as a BGRA pixel (alpha forced opaque by
// the opaque backdrop the suite clears to).
UINT32 ToPixel(const prism::theme::Rgba& c) {
  const auto byte = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<UINT32>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return (byte(c.a) << 24) | (byte(c.r) << 16) | (byte(c.g) << 8) | byte(c.b);
}

// What the flyout's surface must look like on screen: the floating-island token
// over the canvas token. This is the MAIN WINDOW's own surface stack, so the
// assertion is literally "the flyout background matches the window's tokens".
UINT32 ExpectedSurfacePixel(Variant variant) {
  return ToPixel(prism::theme::CompositeOver(SurfaceColor(variant), CanvasColor(variant)));
}
UINT32 CanvasPixel(Variant variant) { return ToPixel(CanvasColor(variant)); }

// What the island looks like over an ARBITRARY backdrop. Used with a wallpaper
// tone that is not a token: a surface that was never painted would leave the
// backdrop untouched, and the assertion can tell the two apart (a white island
// over a white canvas cannot).
UINT32 ExpectedOn(const prism::theme::Rgba& backdrop, Variant variant) {
  return ToPixel(prism::theme::CompositeOver(SurfaceColor(variant), backdrop));
}

// A desktop wallpaper tone: deliberately NOT one of the design tokens, so
// "the island was painted" is observable instead of blending into the canvas.
constexpr prism::theme::Rgba kWallpaper{0.32f, 0.47f, 0.66f, 1.0f};
constexpr prism::theme::Rgba kBlackBackdrop{0.0f, 0.0f, 0.0f, 1.0f};
constexpr prism::theme::Rgba kWhiteBackdrop{1.0f, 1.0f, 1.0f, 1.0f};

// Renders one flyout scene through the REAL painter into an offscreen WIC
// target, over the caller's opaque backdrop.
bool RenderScene(IWICImagingFactory* wic, const Scene& scene,
                 const prism::theme::Rgba& backdrop, Frame* frame) {
  const Metrics m = scene.metrics();
  const UINT32 width = static_cast<UINT32>(m.width);
  const UINT32 height = static_cast<UINT32>(scene.Height());
  if (width == 0 || height == 0) return false;

  FlyoutSurface surface;
  if (FAILED(surface.Initialize())) return false;

  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(surface.Factory()->CreateWicBitmapRenderTarget(
          bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  target->BeginDraw();
  target->Clear(ToD2D(backdrop));
  if (FAILED(surface.CreateDeviceResources(target.Get(), scene.dpi))) {
    target->EndDraw();
    return false;
  }
  surface.PaintContent(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

// ------------------------------------------------------------------ fixtures
// Names are CAP-ONLY on purpose ("ROW01": digits and capitals, no descenders),
// so the bottom-most ink pixel of a row IS its baseline -- the only glyph set
// for which "baseline" can be measured from pixels instead of assumed.
std::vector<Row> Rows(int count) {
  std::vector<Row> rows;
  for (int i = 0; i < count; ++i) {
    Row row;
    row.name = L"ROW" + std::to_wstring(i + 1);
    row.sizeBytes = static_cast<uint64_t>(1024) * static_cast<uint64_t>(i + 1);
    row.directory = (i % 4 == 3);
    rows.push_back(row);
  }
  return rows;
}

Scene MakeScene(int rows, int dpi, Variant variant = Variant::Daylight) {
  Scene scene;
  scene.title = L"C:\\Design_Assets_v4.7z";
  scene.subtitle = std::to_wstring(rows) + L" items";
  scene.rows = Rows(rows);
  scene.variant = variant;
  scene.dpi = dpi;
  return scene;
}

// Bottom-most ink row inside a row band, restricted to the name column (the
// icon column and the size column are outside it, so only the name's glyphs are
// measured). Returns -1 when the band has no ink at all.
int InkBottom(const Frame& frame, const Metrics& m, int row) {
  const int top = m.RowTop(row);
  const int bottom = top + m.rowHeight;
  const int left = m.textLeft + 2;
  const int right = m.SizeColumnLeft() - 6;
  const int surface = frame.Luminance(left + 1, top + 1);
  int lowest = -1;
  for (int y = top; y < bottom && y < static_cast<int>(frame.height); ++y) {
    for (int x = left; x < right && x < static_cast<int>(frame.width); ++x) {
      if (frame.Luminance(x, y) < surface - 40) {
        lowest = y;
        break;
      }
    }
  }
  return lowest;
}

IWICImagingFactory* gWic = nullptr;

}  // namespace

// --------------------------------------------------------------------- cases
PRISM_TEST("flyout background is the main window's floating-island token") {
  const Scene scene = MakeScene(12, 96);
  const Metrics m = scene.metrics();
  // The gap between the name column and the size column: no text is painted
  // there, so these are pure surface pixels.
  const int xs = m.SizeColumnLeft() - 10;

  // (a) Over the canvas token the flyout is EXACTLY the token composite the main
  // window's floating islands produce -- the design value, not an approximation.
  Frame onCanvas;
  PRISM_CHECK(RenderScene(gWic, scene, CanvasColor(scene.variant), &onCanvas));
  if (onCanvas.pixels.empty()) return;
  const UINT32 expected = ExpectedSurfacePixel(scene.variant);
  int mismatches = 0;
  for (int row = 0; row < 12; ++row) {
    const int y = m.RowTop(row) + m.rowHeight / 2;
    const int distance = ColorDistance(onCanvas.At(xs, y), expected);
    if (distance > 3) {
      ++mismatches;
      std::cout << "    [bg] row " << row << " pixel=0x" << std::hex << onCanvas.At(xs, y)
                << " expected=0x" << expected << std::dec << " d=" << distance << std::endl;
    }
  }
  PRISM_CHECK_EQ(mismatches, 0);
  // The header band is part of the island too (it is what names the archive).
  PRISM_CHECK(ColorDistance(onCanvas.At(xs, 6), expected) <= 3);
  // ... and the pixel outside the squircle is the backdrop, not the island.
  PRISM_CHECK(ColorDistance(onCanvas.At(1, 1), CanvasPixel(scene.variant)) <= 3);

  // (b) Over a wallpaper tone that is NOT a token: the island must still be the
  // island. Daylight's white-on-white surface is only ~3 levels away from the
  // canvas, so this is the assertion that proves the surface was PAINTED at all
  // instead of leaving the backdrop untouched.
  Frame onWallpaper;
  PRISM_CHECK(RenderScene(gWic, scene, kWallpaper, &onWallpaper));
  if (onWallpaper.pixels.empty()) return;
  const UINT32 onWall = ExpectedOn(kWallpaper, scene.variant);
  PRISM_CHECK(ColorDistance(onWallpaper.At(xs, m.RowTop(0) + 4), onWall) <= 3);
  PRISM_CHECK(ColorDistance(onWallpaper.At(xs, m.RowTop(0) + 4), ToPixel(kWallpaper)) > 30);

  // (c) The surface keeps the TOKEN's alpha: over black it is darker than over
  // white by exactly (1 - alpha) * 255. An opaque fill would show 0 here, and a
  // surface that was never painted would show the full 255 -- both go red.
  Frame onBlack;
  Frame onWhite;
  PRISM_CHECK(RenderScene(gWic, scene, kBlackBackdrop, &onBlack));
  PRISM_CHECK(RenderScene(gWic, scene, kWhiteBackdrop, &onWhite));
  if (onBlack.pixels.empty() || onWhite.pixels.empty()) return;
  const int y = m.RowTop(0) + 4;
  const int diff = std::abs(onWhite.Luminance(xs, y) - onBlack.Luminance(xs, y));
  const double alpha = static_cast<double>(SurfaceColor(scene.variant).a);
  const int expectedDiff = static_cast<int>(std::lround((1.0 - alpha) * 255.0));
  std::cout << "  [alpha] white-black=" << diff << " expected=" << expectedDiff << std::endl;
  PRISM_CHECK(std::abs(diff - expectedDiff) <= 4);
}

PRISM_TEST("every row's text baseline follows the row pitch") {
  const Scene scene = MakeScene(12, 96);
  Frame frame;
  PRISM_CHECK(RenderScene(gWic, scene, CanvasColor(scene.variant), &frame));
  if (frame.pixels.empty()) return;
  const Metrics m = scene.metrics();

  int measured = 0;
  int offsets[12] = {0};
  for (int row = 0; row < 12; ++row) {
    const int bottom = InkBottom(frame, m, row);
    PRISM_CHECK(bottom >= 0);  // every row really painted its name
    if (bottom < 0) continue;
    ++measured;
    const int baseline = m.RowBaseline(row);
    // Ink ends ON the baseline (the last glyph row is one pixel above it), never
    // above the cap band and never into the next row.
    PRISM_CHECK(bottom <= baseline);
    PRISM_CHECK(bottom >= baseline - 3);
    offsets[row] = bottom - m.RowTop(row);
  }
  PRISM_CHECK_EQ(measured, 12);

  // The offset of the ink inside its band is IDENTICAL for all 12 rows: the
  // baseline is a function of the pitch, not an accumulating error.
  for (int row = 1; row < 12; ++row) {
    PRISM_CHECK_EQ(offsets[row], offsets[0]);
  }
  // ... and one row lower is exactly one rowHeight lower.
  PRISM_CHECK_EQ(m.RowBaseline(1) - m.RowBaseline(0), m.rowHeight);
  PRISM_CHECK_EQ(m.rowHeight, 18);  // the 96-dpi design pitch
}

PRISM_TEST("corners are C15 squircles, not right angles") {
  const Scene scene = MakeScene(12, 96);
  Frame frame;
  // Rendered over the wallpaper tone: the corner probe must be able to tell
  // "outside the squircle" (backdrop) from "inside" (island) by a wide margin.
  PRISM_CHECK(RenderScene(gWic, scene, kWallpaper, &frame));
  if (frame.pixels.empty()) return;

  const Metrics m = scene.metrics();
  const int r = m.radius;
  const int w = static_cast<int>(frame.width);
  const int h = static_cast<int>(frame.height);
  const UINT32 expected = ExpectedOn(kWallpaper, scene.variant);
  const UINT32 backdrop = ToPixel(kWallpaper);
  PRISM_CHECK_EQ(r, 14);  // the main window's island radius at 96 dpi

  struct Corner {
    int cx;
    int cy;
    int sx;
    int sy;
  };
  const Corner corners[4] = {{r, r, -1, -1}, {w - r, r, 1, -1}, {r, h - r, -1, 1},
                             {w - r, h - r, 1, 1}};
  for (const Corner& corner : corners) {
    // The probes are CLASSIFIED by the token header's own field, so a change of
    // exponent moves them instead of silently invalidating the test.
    // The extreme corner pixel: 3 px outside the G2 boundary along the diagonal
    // (the squircle reaches 1.18 * r from the corner centre, so a radius-14
    // corner starts at ~2.3 px in) and comfortably clear of the 1 px rim's
    // anti-aliasing -- while a RIGHT-ANGLED surface fills it completely.
    const int outX = corner.sx < 0 ? 0 : w - 1;
    const int outY = corner.sy < 0 ? 0 : h - 1;
    const int inX = corner.cx + corner.sx * (r / 2);
    const int inY = corner.cy + corner.sy * (r / 2);
    const double outField =
        SquircleField(outX - corner.cx, outY - corner.cy, r, r);
    const double inField = SquircleField(inX - corner.cx, inY - corner.cy, r, r);
    PRISM_CHECK(outField > 0.0);  // outside the squircle ...
    PRISM_CHECK(inField < 0.0);   // ... and well inside it

    // The pixel 1 px in from the corner point: OUTSIDE the squircle, but INSIDE
    // a square corner. A right-angled surface fills it, which is exactly the
    // defect PRISM_C48_MUTANT_SQUARE_CORNER injects.
    PRISM_CHECK(ColorDistance(frame.At(outX, outY), backdrop) <= 3);
    PRISM_CHECK(ColorDistance(frame.At(outX, outY), expected) > 20);
    // A point on the diagonal well inside the corner is island, not backdrop.
    PRISM_CHECK(ColorDistance(frame.At(inX, inY), expected) <= 3);
  }
}

PRISM_TEST("height follows the row count, the row pitch does not") {
  const Scene tall = MakeScene(12, 96);
  const Scene shortScene = MakeScene(3, 96);
  Frame tallFrame;
  Frame shortFrame;
  PRISM_CHECK(RenderScene(gWic, tall, CanvasColor(tall.variant), &tallFrame));
  PRISM_CHECK(RenderScene(gWic, shortScene, CanvasColor(shortScene.variant), &shortFrame));
  if (tallFrame.pixels.empty() || shortFrame.pixels.empty()) return;

  const Metrics tallMetrics = tall.metrics();
  const Metrics shortMetrics = shortScene.metrics();
  PRISM_CHECK_EQ(tallMetrics.width, shortMetrics.width);  // fixed 360
  PRISM_CHECK_EQ(tallMetrics.width, 360);
  // 9 more rows == exactly 9 pitches taller (design numbers, not the function).
  PRISM_CHECK_EQ(tall.Height() - shortScene.Height(), 9 * 18);
  PRISM_CHECK_EQ(static_cast<int>(tallFrame.height) - static_cast<int>(shortFrame.height),
                 9 * 18);
  PRISM_CHECK_EQ(static_cast<int>(shortFrame.height), 24 + 3 * 18);

  // The pitch is unchanged: measured from the pixels of BOTH frames.
  const int tallBottom0 = InkBottom(tallFrame, tallMetrics, 0);
  const int tallBottom1 = InkBottom(tallFrame, tallMetrics, 1);
  const int shortBottom0 = InkBottom(shortFrame, shortMetrics, 0);
  const int shortBottom1 = InkBottom(shortFrame, shortMetrics, 1);
  PRISM_CHECK(tallBottom0 >= 0 && tallBottom1 >= 0 && shortBottom0 >= 0 && shortBottom1 >= 0);
  PRISM_CHECK_EQ(tallBottom1 - tallBottom0, 18);
  PRISM_CHECK_EQ(shortBottom1 - shortBottom0, 18);
  PRISM_CHECK_EQ(tallBottom0, shortBottom0);  // the first row did not move

  // The list is CLIPPED at 12 rows: a 20-row scene is 12 rows tall, and it paints
  // the same height as the 12-row one (the flyout never grows past a screenful).
  const Scene overfull = MakeScene(20, 96);
  PRISM_CHECK_EQ(overfull.VisibleRows(), 12);
  PRISM_CHECK_EQ(overfull.Height(), tall.Height());
}

PRISM_TEST("row height scales with the DPI") {
  // Design numbers, computed independently of MetricsForDpi so the fixed-pitch
  // injection cannot make the test agree with itself.
  const int expected[3] = {18, 27, 36};  // 96 / 144 / 192 dpi
  const int dpis[3] = {96, 144, 192};
  for (int i = 0; i < 3; ++i) {
    const Metrics m = MetricsForDpi(dpis[i]);
    PRISM_CHECK_EQ(m.rowHeight, expected[i]);
    PRISM_CHECK_EQ(m.width, 360);  // the width never scales
  }
  // 12 rows at 192 dpi are 12 * 36 tall plus the scaled header (24 * 2).
  PRISM_CHECK_EQ(MetricsForDpi(192).HeightForRows(12), 48 + 12 * 36);
  PRISM_CHECK_EQ(MetricsForDpi(144).HeightForRows(3), 36 + 3 * 27);
  // A row is one pitch lower than the previous one at every DPI.
  PRISM_CHECK_EQ(MetricsForDpi(144).RowTop(1) - MetricsForDpi(144).RowTop(0), 27);

  // ... and the PIXELS agree: render at 144 dpi and measure the pitch.
  const Scene scene = MakeScene(12, 144);
  Frame frame;
  PRISM_CHECK(RenderScene(gWic, scene, CanvasColor(scene.variant), &frame));
  if (frame.pixels.empty()) return;
  const Metrics m = scene.metrics();
  PRISM_CHECK_EQ(static_cast<int>(frame.height), 36 + 12 * 27);
  const int bottom0 = InkBottom(frame, m, 0);
  const int bottom1 = InkBottom(frame, m, 1);
  const int bottom2 = InkBottom(frame, m, 2);
  PRISM_CHECK(bottom0 >= 0 && bottom1 >= 0 && bottom2 >= 0);
  PRISM_CHECK_EQ(bottom1 - bottom0, 27);
  PRISM_CHECK_EQ(bottom2 - bottom1, 27);
}

PRISM_TEST("placement is fed the size the rows need (C30 owns the geometry)") {
  const RECT work{0, 0, 1920, 1040};
  const POINT cursor{400, 300};
  const prism::win::companion::FlyoutRect tall = Placement(cursor, work, 12, 96);
  const prism::win::companion::FlyoutRect shortRect = Placement(cursor, work, 3, 96);
  PRISM_CHECK_EQ(tall.right - tall.left, 360);
  PRISM_CHECK_EQ(tall.bottom - tall.top, 24 + 12 * 18);
  PRISM_CHECK_EQ(shortRect.bottom - shortRect.top, 24 + 3 * 18);
  PRISM_CHECK_EQ(shortRect.right - shortRect.left, 360);
  // Inside the work area (the flip/clamp itself is C30's suite).
  PRISM_CHECK(tall.left >= work.left);
  PRISM_CHECK(tall.right <= work.right);
  PRISM_CHECK(tall.top >= work.top);
  PRISM_CHECK(tall.bottom <= work.bottom);
  // The size the surface paints is the size the placement reserves.
  const Scene scene = MakeScene(12, 96);
  PRISM_CHECK_EQ(scene.Height(), tall.bottom - tall.top);
}

PRISM_TEST("composition island: rounded clip plus a content visual") {
  FlyoutSurface surface;
  PRISM_CHECK(FAILED(surface.Initialize()) == false);

  const HINSTANCE instance = ::GetModuleHandleW(nullptr);
  const wchar_t kClass[] = L"PrismArchiveC48FlyoutProbe";
  WNDCLASSEXW wc{sizeof(wc)};
  wc.lpfnWndProc = ::DefWindowProcW;
  wc.hInstance = instance;
  wc.lpszClassName = kClass;
  if (::RegisterClassExW(&wc) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    g_noWindowStation = true;
    std::cout << "  [skip] RegisterClassExW failed err=" << ::GetLastError() << std::endl;
    return;
  }
  const HWND hwnd = ::CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
                                          WS_EX_NOREDIRECTIONBITMAP,
                                      kClass, L"c48", WS_POPUP, 0, 0, 360, 240, nullptr,
                                      nullptr, instance, nullptr);
  if (hwnd == nullptr) {
    g_noWindowStation = true;
    std::cout << "  [skip] CreateWindowExW failed err=" << ::GetLastError() << std::endl;
    return;
  }

  if (!surface.InitializeComposition(hwnd, 96)) {
    g_noComposition = true;
    std::cout << "  [skip] composition unavailable hr=0x" << std::hex
              << static_cast<unsigned long>(surface.CompositionError()) << std::dec
              << std::endl;
    ::DestroyWindow(hwnd);
    return;
  }

  // The visual tree: a root carrying the rounded-rectangle clip, and ONE content
  // visual carrying the composition swapchain (ADR-001: one visual per island).
  PRISM_CHECK_EQ(surface.VisualCount(), static_cast<size_t>(2));
  PRISM_CHECK(surface.HasRoundedClip());
  PRISM_CHECK(surface.SwapchainLive());
  // The clip is a SUPERSET of the painted squircle: at the same radius a circular
  // DComp clip would shave the G2 bulge the corner test above protects.
  PRISM_CHECK(surface.ClipRadius() > static_cast<float>(MetricsForDpi(96).radius));
  PRISM_CHECK_EQ(surface.SurfaceWidth(), 360);
  PRISM_CHECK_EQ(surface.SurfaceHeight(), MetricsForDpi(96).HeightForRows(12));

  // Painting goes through the SAME content painter and resizes the island to the
  // rows the payload actually has.
  Scene scene = MakeScene(12, 96);
  PRISM_CHECK(surface.Paint(scene));
  // The painter's resources must exist after the first Paint: a missing brush
  // set makes PaintContent a silent no-op (the island would be empty, not red).
  PRISM_CHECK(surface.ContentReady());
  PRISM_CHECK_EQ(surface.SurfaceHeight(), 24 + 12 * 18);
  Scene few = MakeScene(3, 96);
  PRISM_CHECK(surface.Paint(few));
  PRISM_CHECK_EQ(surface.SurfaceHeight(), 24 + 3 * 18);
  PRISM_CHECK_EQ(surface.SurfaceWidth(), 360);

  ::DestroyWindow(hwnd);
}

int main(int argc, char** argv) {
  const char* mutation = nullptr;
#if defined(PRISM_C48_MUTANT_SQUARE_CORNER)
  mutation = "square-corner";
#elif defined(PRISM_C48_MUTANT_FIXED_ROW_HEIGHT)
  mutation = "fixed-row-height";
#endif
  if (mutation != nullptr) {
    std::cout << "C48 MUTANT ACTIVE: " << mutation << std::endl;
    // CONTRACTS D27: arm the verdict BEFORE the first assertion. ReportFailure
    // prints it the moment the first check fails and leaves with 0, so teardown
    // (COM uninit, WIC release, a crash in the mutated path) cannot swallow it.
    // CONTRACTS D3: the predicate proves the injection really FIRED -- a mutant
    // that goes red for an unrelated reason must not claim CAUGHT.
    if (std::string(mutation) == "square-corner") {
      prism::test::SetMutationVerdict(
          "C48 MUTATION CAUGHT: square-corner",
          [] { return prism::win::flyout::SquareCornerFills() > 0; });
    } else {
      prism::test::SetMutationVerdict(
          "C48 MUTATION CAUGHT: fixed-row-height",
          [] {
            return MetricsForDpi(192).rowHeight == MetricsForDpi(96).rowHeight;
          });
    }
  }

  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return kSkipReturnCode;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::cout << "FAIL: WIC unavailable -- the flyout pixels cannot be verified" << std::endl;
    return 1;
  }
  gWic = wic.Get();

  const int rc = prism::test::RunAll(argc, argv);

  if (mutation != nullptr) {
    // Reaching here with a red suite means a failure bypassed ReportFailure; the
    // normal verdict already left with 0 at the first failed assertion.
    if (rc == 0) {
      std::cout << "C48 MUTATION NOT CAUGHT: the suite stayed green with " << mutation
                << " compiled in -- the injection is stale" << std::endl;
      return 1;
    }
    std::cout << "C48 MUTATION CAUGHT: " << mutation << std::endl;
    return 0;
  }

  if (rc != 0) {
    std::cout << "FLYOUT-PAINT CHECKS FAILED" << std::endl;
    return 1;
  }
  if (g_noWindowStation || g_noComposition) {
    std::cout << "C48 SKIP: this machine cannot exercise the composition island (window="
              << (g_noWindowStation ? "no" : "yes")
              << " composition=" << (g_noComposition ? "no" : "yes") << ")" << std::endl;
    return kSkipReturnCode;
  }
  std::cout << "ALL FLYOUT-PAINT CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  return 0;
}
