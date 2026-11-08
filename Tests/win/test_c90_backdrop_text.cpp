// PrismArchive -- PrismWin
// C90: the list's white blurred plate -- the C77 refraction source must not
// contain the ROW GLYPHS.
//
// THE DEFECT (research A, E:\wg\r-a\RESEARCH-A-white-blocks.md). GalleryRenderer
// renders the CONTENT into a 1/8-resolution target and blurs it (BuildBackdrop).
// In list mode that pass used to re-draw every row label: name, 原始大小,
// 压缩后, 压缩率. At 1/8 a 13 px run of near-white text is ~1.6 px tall, so every
// glyph rasterizes into a FULL mini pixel; 4 mini px of row pitch plus sigma
// 1.75 fuses the rows into a solid mini band, and the veil leaves 35% of it.
// Measured on the audit's frame: the name column's real ink coverage is 0.092,
// a faithful blur would read lum 16.8, the frame read 78.7 -- a source coverage
// of 0.81, 8.8x the ink. That is the user's two white rectangles, and it is why
// the plate is white: TextPrimary @ Midnight (245,245,247) at 35% over the
// #0B0E13 canvas is +73 levels.
//
// THE FIX (this suite's contract): glyphs are not a refraction source. The row
// TEXT leaves the backdrop pass; the folder icon, the ratio capsule and the
// selection tint STAY (they are the content a refraction layer is supposed to
// carry, and they are what keeps C77-1's "the layer exists" measurable). The
// list HEADER was excluded for exactly this reason by A2; the rows were missed.
//
// THE ASSERTIONS, on rendered pixels (headless WIC target, real renderer):
//   1. NAME BAND (the audit's x 400..1400, inside the list body): over the
//      pixels that are canvas in the backdrop-OFF reference -- i.e. the empty
//      space between glyphs, which is what the audit measured -- the ON frame's
//      max luminance may not exceed canvas + 24. Today it reads ~91 vs a limit
//      of 38: RED before the fix, and the mutant arm below re-prints it.
//   2. POSITIVE CONTROL, re-based by C121: the refraction source carries the
//      PAGE -- the rail's panel inside the ghost reads >= 8 levels off the
//      canvas -- and the ratio column carries ZERO item pixels (the ghost there
//      is the canvas, within 4). This used to assert the opposite: "the capsule
//      bar is still in the refraction source, so ON and OFF differ by >= 8 on
//      >= 1500 px". That assertion was the twin of C77-1 and both were WRONG for
//      the same reason: they required ITEM pixels in the source, and the item
//      pixels are exactly the coloured halo C121 removes. The layer's presence
//      is still proven, by a PAGE surface instead of by the capsule.
//   3. NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the same source compiled with
//      PRISM_C77_MUTANT_TEXT_IN_BACKDROP -- drawGlyphs forced back to true --
//      MUST print "C77 MUTATION CAUGHT: text-in-backdrop" at the first failing
//      check. The verdict is armed on the injected branch having actually run
//      (the renderer counts the list-mode backdrop passes that drew glyphs), so
//      a red for an unrelated reason stays red WITHOUT a false CAUGHT.
//   4. REBUILD POLICY unchanged: the ghost is rebuilt on a content change and
//      NEVER on a scroll (C77's contract, still exactly one build).
//   5. THE SELECTED ROW LEAVES NO BAND ACROSS THE ROW. The follow-up's defect:
//      after opening an archive the window selects the first row, and its 32 px
//      accent pill is 4 mini px of solid accent in the 1/8 source -- a band that
//      reaches ~40 px above and below the row and, because BackdropKey never
//      carried selectedRow, outlives the selection. The scan is the reporter's:
//      x 400..1400 every 100 px, every y of the rows band, and at a pixel that
//      is canvas in the backdrop-OFF frame ON may differ by at most 4 levels.
//      PRISM_C77_MUTANT_STATE_IN_BACKDROP restores the pill and MUST print
//      "C77 MUTATION CAUGHT: state-in-backdrop".
//
// The suite covers LIST MODE with multiple rows on purpose: test_c77_glass.cpp
// only ever renders the GRID scene (it never sets listItems/listMode), which is
// why CI stayed green while every real list carried the plate.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4.
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
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;
using prism::ArchiveItem;
using prism::list::SortItem;
using prism::win::gallery::GalleryRenderer;

namespace {

// The audit's window (1920x1080 client at 125% DPI) with the rail shown, which
// is what puts the name column at x 256..1542 and the ratio capsule at
// x 1782..1848 -- the geometry the card's two probe boxes were measured in.
constexpr UINT32 kWidth = 1920;
constexpr UINT32 kHeight = 1080;

// The card's probe boxes, verbatim. They are ASSERTED against the layout below,
// so a layout change that moved the name column out from under them fails loudly
// instead of silently measuring a rectangle that no longer contains text.
constexpr int kNameBandLeft = 400;
constexpr int kNameBandRight = 1400;
constexpr int kRatioStripLeft = 1730;
constexpr int kRatioStripRight = 1830;

// A reference pixel is "canvas" when it is within this many levels of the flat
// canvas colour. Everything above it is crisp ink (a glyph, the icon, the bar,
// the floating chrome) and is NOT what this card measures: the plate lives in
// the space BETWEEN the glyphs.
constexpr int kCanvasTolerance = 4;
// The card's limit: maxLum(ON) <= lum(canvas) + 24.
constexpr int kPlateLimit = 24;
// C121: the positive control's floor -- a page panel must stand this far off the
// canvas in the refraction source (the rail's panel is +21/+24/+29 on Midnight,
// and the blur of a 220 px wide panel leaves its middle at full strength).
constexpr int kPagePanelMinDelta = 8;

#if defined(PRISM_C77_MUTANT_TEXT_IN_BACKDROP)
// C90 MUTANT ACTIVE: text-in-backdrop -- DrawListMode's drawGlyphs is forced
// back to true, i.e. the code that shipped. Assertion (1) MUST go red.
inline constexpr const char* kMutantBanner = "C77 MUTANT ACTIVE: text-in-backdrop";
inline constexpr const char* kMutantMarker = "C77 MUTATION CAUGHT: text-in-backdrop";
#elif defined(PRISM_C77_MUTANT_STATE_IN_BACKDROP)
// C90 MUTANT ACTIVE: state-in-backdrop -- the selected row's 32 px accent pill is
// back in the 1/8 source. Assertion (5), the full-row-band scan, MUST go red.
inline constexpr const char* kMutantBanner = "C77 MUTANT ACTIVE: state-in-backdrop";
inline constexpr const char* kMutantMarker = "C77 MUTATION CAUGHT: state-in-backdrop";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

// D3: the injected branch really ran. Counted only by the renderer's mutant
// arm (the counter is compiled out of the clean build), so a mutant that dies
// for an unrelated reason never prints the verdict.
int gGlyphPasses = 0;
int gStatePasses = 0;

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
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
  // The audit's own metric: the plain mean of the three channels (it reported
  // "lum 78.7" and "canvas 13.7" that way).
  int Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3;
  }
};

int MaxChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, std::abs(d));
  }
  return delta;
}

struct Box {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;
};

// One renderer, one target, many frames (the backdrop cache lives on the
// renderer, which is exactly what the rebuild-policy case measures).
struct Harness {
  GalleryRenderer renderer;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;

  bool Open(UINT32 width, UINT32 height) {
    if (FAILED(renderer.Initialize())) return false;
    if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand,
                                  bitmap.ReleaseAndGetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
            bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer.CreateDeviceResources(target.Get()));
  }

  bool Paint(const GalleryRenderer::Scene& scene, Frame* out, bool ghost = false) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    // C121: the refraction SOURCE alone: the seam clears the target, so the
    // read-back is the source itself (its alpha says what the page painted).
    if (ghost && !renderer.PaintRefractionSource(target.Get())) {
      target->EndDraw();
      return false;
    }
    if (FAILED(target->EndDraw())) return false;
#if defined(PRISM_C77_MUTANT_TEXT_IN_BACKDROP)
    gGlyphPasses += renderer.BackdropGlyphPasses();
#elif defined(PRISM_C77_MUTANT_STATE_IN_BACKDROP)
    gStatePasses += renderer.BackdropStatePasses();
#endif
    if (out == nullptr) return true;
    UINT bitmapWidth = 0;
    UINT bitmapHeight = 0;
    if (FAILED(bitmap->GetSize(&bitmapWidth, &bitmapHeight))) return false;
    WICRect rc{0, 0, static_cast<INT>(bitmapWidth), static_cast<INT>(bitmapHeight)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
      return false;
    }
    out->width = rc.Width;
    out->height = rc.Height;
    out->pixels.resize(static_cast<size_t>(out->width) * out->height);
    for (UINT32 y = 0; y < out->height; ++y) {
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * out->width,
                  data + static_cast<size_t>(y) * stride,
                  static_cast<size_t>(out->width) * 4);
    }
    return true;
  }
};

bool Render(const GalleryRenderer::Scene& scene, Frame* out) {
  Harness harness;
  return harness.Open(kWidth, kHeight) && harness.Paint(scene, out);
}

// The canvas colour as the render target quantizes it to bytes. C91: the canvas
// is the THEME's composite now (prism::chrome::CanvasColor) -- asking the same
// entry the renderer clears with, not a literal. The C90 scenes leave the C77
// refraction layer off, so the FLAT composite is the pixel on screen.
UINT32 CanvasPixel() {
  const prism::theme::Rgba token =
      prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  const D2D1_COLOR_F c = D2D1::ColorF(token.r, token.g, token.b, token.a);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

int CanvasLuminance() {
  const UINT32 c = CanvasPixel();
  return (static_cast<int>((c >> 16) & 0xFFu) + static_cast<int>((c >> 8) & 0xFFu) +
          static_cast<int>(c & 0xFFu)) /
         3;
}

// C91: the canvas is the THEME's composite, and the C77 veil is part of what a
// resting pixel shows. CanvasPresentedColor(v) IS that pixel -- the veil token
// composited over the canvas, which is the same composition DrawBackdrop runs.
//
// WHY THIS IS THE BASELINE FOR THE BAND SCAN. The first version compared the
// layer-ON frame against the layer-OFF frame and allowed 4 levels. On the night
// canvas that wash is (16,17,23) -> (13,14,18): the BLUE channel moves 5 levels,
// so the scan counted every empty pixel of the list as a violation and the
// Windows lane went red with no band in the frame at all (PR #114). The band
// this assertion exists for is +41..73, so measuring against the theme's own
// presented canvas keeps the assertion just as sharp and makes it independent of
// the theme, the DPI and the host's rounding.
UINT32 PresentedCanvasPixel() {
  const prism::theme::Rgba token =
      prism::chrome::CanvasPresentedColor(prism::theme::Variant::Midnight);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(token.a) << 24) | (q(token.r) << 16) | (q(token.g) << 8) | q(token.b);
}

// ------------------------------------------------------------------ fixture
SortItem Row(const char* name, std::uint64_t size, std::uint64_t packed) {
  SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  return item;
}

// A real archive listing: one folder, thirteen text payloads whose names run
// well past x = 400 in the name column (the audit's plate started where the
// text starts and ended ~17 px past the last glyph).
std::vector<SortItem> PayloadRows() {
  std::vector<SortItem> rows;
  SortItem dir;
  dir.name = "payload_text/";
  dir.isDir = true;
  rows.push_back(dir);
  rows.push_back(Row("payload_text/quarterly_report_q4_final.txt", 4'820'331, 900'112));
  rows.push_back(Row("payload_text/transcript_2026-11-04_standup.txt", 1'204'412, 210'388));
  rows.push_back(Row("payload_text/product_brief_localization_zh.txt", 998'221, 402'113));
  rows.push_back(Row("payload_text/error_log_2026-10-31_2300.txt", 3'301'122, 780'221));
  rows.push_back(Row("payload_text/notes_from_the_field_team.txt", 445'120, 99'001));
  rows.push_back(Row("payload_text/api_reference_archive_storage.txt", 1'820'333, 512'000));
  rows.push_back(Row("payload_text/customer_interview_digest.txt", 2'200'000, 812'345));
  rows.push_back(Row("payload_text/design_review_backdrop_layer.txt", 1'550'000, 400'000));
  rows.push_back(Row("payload_text/screenshot_captions_draft.txt", 660'000, 120'000));
  rows.push_back(Row("payload_text/changelog_before_after.txt", 300'022, 88'000));
  rows.push_back(Row("payload_text/renderer_glyph_audit.md", 120'000, 40'000));
  rows.push_back(Row("payload_text/list_mode_backdrop_report.md", 240'000, 60'000));
  rows.push_back(Row("payload_text/solid_block_manifest.txt", 88'000, 30'000));
  return rows;
}

struct Fixture {
  std::vector<ArchiveItem> catalog;   // the rail's telemetry capsule reads this
  std::vector<SortItem> rows;
  prism::sidebar::Model model;
  prism::win::sidebar::Layout rail;
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  Fixture() : rows(PayloadRows()), model(&catalog) {
    // The rail is CHROME and never part of the refraction source; it is here so
    // the frame is the real window (and so the name column sits where the audit
    // measured it: rail 220 + 16 px padding + 20 px indent).
    const char* paths[] = {"payload_text/quarterly_report_q4_final.txt",
                           "payload_text/transcript_2026-11-04_standup.txt",
                           "payload_text/error_log_2026-10-31_2300.txt"};
    for (std::size_t i = 0; i < 3; ++i) {
      ArchiveItem item;
      item.path = paths[i];
      item.index = static_cast<std::uint32_t>(i);
      item.size = 4'820'331u / (i + 1);
      item.packSize = 900'112u / (i + 1);
      catalog.push_back(item);
    }
    layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                    /*sidebarVisible=*/true, /*listMode=*/true);
    prism::win::sidebar::Metrics metrics;
    metrics.width = static_cast<float>(prism::layout::kSidebarWidth);
    metrics.viewportTop = static_cast<float>(layout.actionIsland.bottom);
    metrics.viewportBottom = static_cast<float>(layout.statusBar.top);
    rail = prism::win::sidebar::Build(model, metrics);

    scene.listMode = true;
    scene.listItems = &rows;
    scene.sidebar = &rail;
    scene.layout = &layout;  // the SAME geometry the renderer would compute
    scene.breadcrumb = L"payload.7z  |  14 items  |  solid block 3/5";
    scene.glassPanels = true;  // the shipping frame: floating breadcrumb island
    scene.status.itemCount = static_cast<int>(rows.size());
    scene.status.ratioView = prism::CompressionRatioView(21'737'891, 4'522'292);
    // selectedRow stays -1: a selection tint is CONTENT and would legitimately
    // brighten the band (it is kept in the backdrop on purpose); the audit's
    // frame measured the plate between the glyphs, not the selection.
  }

  // Backdrop OFF and ON differ by exactly one flag: the reference therefore
  // contains the same crisp content, the same chrome and the same canvas.
  GalleryRenderer::Scene Off() const {
    GalleryRenderer::Scene s = scene;
    s.backdropRefraction = false;
    return s;
  }
  GalleryRenderer::Scene On() const {
    GalleryRenderer::Scene s = scene;
    s.backdropRefraction = true;
    return s;
  }
};

bool WritePng(const wchar_t* fileName, const Frame& frame) {
  if (gOutDir == nullptr) return true;  // no artifact requested
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%s", gOutDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> encodeFrame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          frame.width, frame.height, GUID_WICPixelFormat32bppPBGRA, frame.width * 4,
          static_cast<UINT>(frame.pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data())),
          bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
      FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(encodeFrame.ReleaseAndGetAddressOf(),
                                     bag.ReleaseAndGetAddressOf())) ||
      FAILED(encodeFrame->Initialize(bag.Get())) ||
      FAILED(encodeFrame->SetSize(frame.width, frame.height))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (FAILED(encodeFrame->SetPixelFormat(&format)) ||
      FAILED(encodeFrame->WriteSource(bitmap.Get(), nullptr)) ||
      FAILED(encodeFrame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  std::printf("  wrote %ls\n", path);
  return true;
}

// ------------------------------------------------------------------ measure
struct BandStats {
  int mask = 0;       // pixels that are canvas in the reference (the "empty space")
  int ink = 0;        // pixels that carry crisp ink in the reference
  int maxLum = 0;     // the plate's worst pixel, over the mask
  int maxX = -1;
  int maxY = -1;
  int medianLum = 0;
  int over = 0;       // mask pixels above the limit: the plate's AREA
};

// The audit's measurement, made explicit: for every pixel of the band that is
// CANVAS in the backdrop-OFF reference -- the space between glyphs, which is
// where the plate was measured at 78.7 -- take the ON frame's luminance. The
// crisp glyphs themselves are drawn ON TOP of the layer and are not part of the
// defect (their luminance is the same in both frames).
BandStats MeasureBand(const Frame& on, const Frame& off, const Box& box, int canvasLum) {
  BandStats stats;
  std::vector<int> histogram(256, 0);
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!on.In(x, y) || !off.In(x, y)) continue;
      const int reference = off.Luminance(x, y);
      if (reference > canvasLum + kCanvasTolerance) {
        if (reference > canvasLum + kPlateLimit) ++stats.ink;
        continue;
      }
      ++stats.mask;
      const int lum = on.Luminance(x, y);
      ++histogram[static_cast<size_t>(lum)];
      if (lum > canvasLum + kPlateLimit) ++stats.over;
      if (lum > stats.maxLum) {
        stats.maxLum = lum;
        stats.maxX = x;
        stats.maxY = y;
      }
    }
  }
  int accumulated = 0;
  for (int lum = 0; lum < 256; ++lum) {
    accumulated += histogram[static_cast<size_t>(lum)];
    if (stats.mask > 0 && accumulated * 2 >= stats.mask) {
      stats.medianLum = lum;
      break;
    }
  }
  return stats;
}

// The band scan, shared by both states (no selection / first row selected).
// Reports the per-column profile the reporter asked for and returns the number
// of reference-canvas pixels that do not sit on the theme's presented canvas.
struct BandScan {
  int probes = 0;
  int violations = 0;
  int worst = 0;
  int worstX = -1;
  int worstY = -1;
};

BandScan ScanRowBand(const Frame& lit, const Frame& reference, const Box& band, int rowsBottom,
                     int canvasLum, int tolerance, const char* tag) {
  BandScan scan;
  const UINT32 washed = PresentedCanvasPixel();
  std::printf("  C90 %s baseline: presentedCanvas=(%d,%d,%d) tolerance=%d\n", tag,
              static_cast<int>((washed >> 16) & 0xFFu), static_cast<int>((washed >> 8) & 0xFFu),
              static_cast<int>(washed & 0xFFu), tolerance);
  for (int x = band.left; x <= band.right; x += 100) {
    int columnWorst = 0;
    for (int y = band.top; y < rowsBottom; ++y) {
      if (!reference.In(x, y) || !lit.In(x, y)) continue;
      // Only pixels that are CANVAS in the reference: crisp ink (the pill, a
      // glyph, the bar) is drawn over the layer in both frames and is not the
      // ghost's doing.
      if (reference.Luminance(x, y) > canvasLum + kCanvasTolerance) continue;
      ++scan.probes;
      const int delta = MaxChannelDelta(lit.At(x, y), washed);
      columnWorst = (std::max)(columnWorst, delta);
      if (delta > tolerance) ++scan.violations;
      if (delta > scan.worst) {
        scan.worst = delta;
        scan.worstX = x;
        scan.worstY = y;
      }
    }
    std::printf("  C90 %s x=%4d: maxChannelDelta=%2d\n", tag, x, columnWorst);
  }
  std::printf("  C90 %s: probes=%d violations=%d worst=%d at (%d,%d)\n", tag, scan.probes,
              scan.violations, scan.worst, scan.worstX, scan.worstY);
  return scan;
}

// The geometry the renderer itself will use for this scene shape, and the four
// numbers the card's probe boxes depend on. Asserting them is what keeps the
// boxes honest: they are the audit's coordinates, not derived ones.
struct Geometry {
  prism::layout::Rect body;
  prism::layout::Rect header;
  float nameLeft = 0.0f;
  float nameRight = 0.0f;
  float ratioColumn = 0.0f;
  float barLeft = 0.0f;
  float barRight = 0.0f;
  float contentRight = 0.0f;
};

Geometry TestGeometry(const prism::layout::Layout& layout) {
  const prism::list::ColumnLayout cols;  // the module default (no C27 columns here)
  Geometry g;
  g.body = prism::layout::ListBodyRect(layout);
  g.header = prism::layout::ListHeaderRect(layout);
  g.contentRight = static_cast<float>(g.body.right);
  g.nameLeft = static_cast<float>(g.body.left) + static_cast<float>(prism::layout::kListIndent);
  g.nameRight = g.contentRight - cols.sizeW - cols.packedW - cols.ratioW - 12.0f;
  g.ratioColumn = g.contentRight - cols.ratioW + 6.0f;
  g.barLeft = g.contentRight - cols.ratioW + 8.0f;
  g.barRight = g.barLeft + (cols.ratioW - 64.0f);
  return g;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1 + 2: the plate is gone, the layer is still there
// ---------------------------------------------------------------------------
PRISM_TEST("c90.list_name_band_is_not_a_white_plate") {
  Fixture fixture;
  const Geometry geometry = TestGeometry(fixture.layout);
  const int canvasLum = CanvasLuminance();
  std::printf("C90 geometry: body=(%d,%d)-(%d,%d) header=(%d,%d)-(%d,%d)\n", geometry.body.left,
              geometry.body.top, geometry.body.right, geometry.body.bottom, geometry.header.left,
              geometry.header.top, geometry.header.right, geometry.header.bottom);
  std::printf("C90 geometry: nameColumn=%.0f..%.0f ratioColumn=%.0f bar=%.0f..%.0f canvas=%d\n",
              geometry.nameLeft, geometry.nameRight, geometry.ratioColumn, geometry.barLeft,
              geometry.barRight, canvasLum);

  // The card's boxes, asserted against the layout: the name band must sit past
  // the folder icon and inside the name column; the ratio strip must sit right
  // of the name column and overlap the capsule bar.
  PRISM_CHECK(static_cast<float>(kNameBandLeft) >= geometry.nameLeft + 28.0f);
  PRISM_CHECK(static_cast<float>(kNameBandRight) <= geometry.nameRight);
  PRISM_CHECK(static_cast<float>(kRatioStripLeft) >= geometry.nameRight);
  PRISM_CHECK(static_cast<float>(kRatioStripRight) <= geometry.contentRight);
  PRISM_CHECK(static_cast<float>(kRatioStripLeft) < geometry.barRight);
  PRISM_CHECK(static_cast<float>(kRatioStripRight) > geometry.barLeft);

  // The audit's y range is the ROW BAND, not the whole body: below the last row
  // there is nothing but canvas, and averaging that in would dilute the very
  // number the audit reported (78.7 over the rows). The empty tail is still
  // audited by the ratio strip's pixel count, which only exists where rows are.
  const int rowsBottom =
      (std::min)(geometry.body.bottom,
                 geometry.body.top + static_cast<int>(fixture.rows.size()) *
                                         prism::layout::kListRowHeight);
  std::printf("C90 geometry: row band y=%d..%d (%d rows)\n", geometry.body.top, rowsBottom,
              static_cast<int>(fixture.rows.size()));
  const Box nameBand{kNameBandLeft, geometry.body.top, kNameBandRight, rowsBottom};
  const Box ratioStrip{kRatioStripLeft, geometry.body.top, kRatioStripRight, rowsBottom};

  Frame reference;
  Frame lit;
  PRISM_CHECK(Render(fixture.Off(), &reference));
  PRISM_CHECK(Render(fixture.On(), &lit));
  if (reference.pixels.empty() || lit.pixels.empty()) return;

  if (!WritePng(L"c90-list-backdrop-on.png", lit)) std::puts("  WARN: PNG failed");

  const BandStats band = MeasureBand(lit, reference, nameBand, canvasLum);
  std::printf(
      "C90 name band: mask=%d ink=%d (%.3f coverage) onMaxLum=%d at (%d,%d) onMedianLum=%d "
      "platePixels=%d canvas=%d limit=%d\n",
      band.mask, band.ink, band.mask > 0 ? static_cast<double>(band.ink) / band.mask : 0.0,
      band.maxLum, band.maxX, band.maxY, band.medianLum, band.over, canvasLum,
      canvasLum + kPlateLimit);
  // The band must be mostly empty space, otherwise "max over the mask" would be
  // a measurement of nothing (the audit's frame: ink coverage 0.092).
  PRISM_CHECK(band.mask > 20000);
  PRISM_CHECK(band.ink * 8 < band.mask);
  // THE CARD'S ASSERTION 1. On the shipped code this reads ~91 (the plate);
  // with the glyphs out of the refraction source it reads the canvas.
  PRISM_CHECK(band.maxLum <= canvasLum + kPlateLimit);

  // ASSERTION 2, the positive control, RE-BASED BY C121. The old form required
  // the ratio capsule to be inside the refraction source -- i.e. it required
  // ITEM pixels in the background, the same wrong premise C77-1 carried, and
  // the reason the ghost ever contained a thumbnail. The source is the PAGE
  // now, so the proof that the layer is real moves to a page surface: the rail.
  //   (a) the rail's panel is IN the source (>= 8 levels off the canvas);
  //   (b) the ratio column is NOT (the ghost there is the canvas, within 4) --
  //       the item-pixel half, which is what this suite is about.
  // Switching the layer off wholesale turns (a) red while assertion 1 stays
  // green, which is the job the old control had.
  Harness ghostHarness;
  PRISM_CHECK(ghostHarness.Open(kWidth, kHeight));
  if (ghostHarness.bitmap != nullptr) {
    Frame ghost;
    PRISM_CHECK(ghostHarness.Paint(fixture.On(), &ghost, /*ghost=*/true));
    if (!ghost.pixels.empty()) {
      const UINT32 canvasPixel = PresentedCanvasPixel();
      double panelDelta = 0.0;
      int panelSamples = 0;
      int panelMin = 1000;
      for (int y = static_cast<int>(fixture.rail.panel.top) + 40;
           y < static_cast<int>(fixture.rail.panel.bottom) - 40; y += 3) {
        for (int x = static_cast<int>(fixture.rail.panel.left) + 40;
             x < static_cast<int>(fixture.rail.panel.right) - 40; x += 3) {
          if (!ghost.In(x, y)) continue;
          const int delta = MaxChannelDelta(ghost.At(x, y), canvasPixel);
          panelDelta += delta;
          panelMin = (std::min)(panelMin, delta);
          ++panelSamples;
        }
      }
      PRISM_CHECK(panelSamples > 1000);
      const double meanPanelDelta = panelSamples > 0 ? panelDelta / panelSamples : 0.0;
      std::printf("C90 source: rail panel meanDelta=%.2f minDelta=%d samples=%d (floor %d)\n",
                  meanPanelDelta, panelMin, panelSamples, kPagePanelMinDelta);
      PRISM_CHECK(meanPanelDelta >= kPagePanelMinDelta);

      int worstItemDelta = 0;
      int itemSamples = 0;
      for (int y = ratioStrip.top; y < ratioStrip.bottom; ++y) {
        for (int x = ratioStrip.left; x < ratioStrip.right; ++x) {
          if (!ghost.In(x, y)) continue;
          worstItemDelta = (std::max)(worstItemDelta, MaxChannelDelta(ghost.At(x, y), canvasPixel));
          ++itemSamples;
        }
      }
      PRISM_CHECK(itemSamples > 1000);
      std::printf("C90 source: ratio column worstDelta=%d samples=%d (limit 4)\n",
                  worstItemDelta, itemSamples);
      PRISM_CHECK(worstItemDelta <= 4);   // ZERO item pixels in the reflection
    }
  }

  // ASSERTION 5 in the UNSELECTED state: the same wide scan (x 400..1400 every
  // 100 px) must also hold with no pill in the scene. The assertion has to be a
  // property of the layer, not of one selection state.
  const Box scanBand{kNameBandLeft, geometry.body.top, kNameBandRight, rowsBottom};
  const BandScan scan =
      ScanRowBand(lit, reference, scanBand, rowsBottom, canvasLum, 4, "unselected");
  PRISM_CHECK(scan.probes > 4000);
  PRISM_CHECK(scan.violations == 0);
}

// ---------------------------------------------------------------------------
// 5: the SELECTED ROW leaves no band anywhere across the row
// ---------------------------------------------------------------------------
// The audit's second red circle: after opening an archive the FIRST row is
// selected (GalleryWindow: scene.selectedRow = folderToListRow_[selection_]),
// and the 32 px accent pill went into the 1/8 source with the text. At 1/8 that
// pill is 4 mini px of solid accent, so the blur returns a full-width band that
// (a) is far wider than the row itself and (b) OUTLIVES the selection, because
// BackdropKey does not carry selectedRow -- which is exactly the "rounded panel
// smeared down the list" the parent measured at x 350..1200 on a row that no
// longer holds anything.
//
// The scan is the reporter's: x 400..1400 every 100 px, at every y of the rows'
// band. The criterion is theirs too -- at a pixel that is CANVAS in the
// backdrop-OFF frame, the ON frame may not sit more than 4 levels away from the
// canvas it should be showing. "The canvas it should be showing" is the THEME's
// presented canvas (C91's CanvasPresentedColor: the veil composited over the
// canvas), not the OFF frame: the veil's own wash is 5 levels on the night
// canvas's blue channel, and the first version of this scan charged that wash as
// a band at every empty pixel -- red on CI (PR #114) with no band in the frame.
PRISM_TEST("c90.selected_row_leaves_no_band_across_the_row") {
  Fixture fixture;
  fixture.scene.selectedRow = 0;  // the window's default after opening an archive
  const Geometry geometry = TestGeometry(fixture.layout);
  const int canvasLum = CanvasLuminance();
  const int rowsBottom =
      (std::min)(geometry.body.bottom,
                 geometry.body.top + static_cast<int>(fixture.rows.size()) *
                                         prism::layout::kListRowHeight);

  Frame reference;
  Frame lit;
  PRISM_CHECK(Render(fixture.Off(), &reference));
  PRISM_CHECK(Render(fixture.On(), &lit));
  if (reference.pixels.empty() || lit.pixels.empty()) return;

  // The frame this card's evidence uses: same scene, selection on.
  if (!WritePng(L"c90-list-backdrop-selected-on.png", lit)) std::puts("  WARN: PNG failed");

  constexpr int kBandTolerance = 4;  // "back to the canvas it sits on, +/- 4"
  const Box scanBand{kNameBandLeft, geometry.body.top, kNameBandRight, rowsBottom};
  const BandScan scan =
      ScanRowBand(lit, reference, scanBand, rowsBottom, canvasLum, kBandTolerance, "selected");
  // 11 columns x 448 rows of the row band, minus the selected pill's own crisp
  // pixels (~4.5k). Not vacuous, and the mutant arm proves it is not.
  PRISM_CHECK(scan.probes > 4000);
  // ASSERTION 5: no band of any width. This is what the name-column-only box
  // could not see: the pill is 1600 px wide.
  PRISM_CHECK(scan.violations == 0);
}

// ---------------------------------------------------------------------------
// 4: the ghost is a still -- rebuilt on a content change, never on a scroll
// ---------------------------------------------------------------------------
PRISM_TEST("c90.backdrop_rebuilds_on_content_change_not_on_scroll") {
  Fixture fixture;
  GalleryRenderer::Scene scene = fixture.On();
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;

  PRISM_CHECK(harness.Paint(scene, nullptr));
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 1);
  PRISM_CHECK_EQ(harness.renderer.BackdropDraws(), 1);

  // A scroll moves which rows are visible but not what the ghost is: it is a
  // still of the content, and a per-frame full-window blur is exactly what C77
  // forbade.
  scene.listScroll = 96;
  PRISM_CHECK(harness.Paint(scene, nullptr));
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 1);
  PRISM_CHECK_EQ(harness.renderer.BackdropDraws(), 2);

  // A content change (the caller's revision) DOES rebuild.
  scene.contentRevision = 7;
  PRISM_CHECK(harness.Paint(scene, nullptr));
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 2);
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    // D3/D27: the verdict prints at the FIRST failing check, and only when the
    // injected branch actually ran (the renderer counted the list-mode backdrop
    // passes that drew glyphs). A red for another reason stays red without it.
#if defined(PRISM_C77_MUTANT_STATE_IN_BACKDROP)
    prism::test::SetMutationVerdict(kMutantMarker, []() { return gStatePasses > 0; });
#else
    prism::test::SetMutationVerdict(kMutantMarker, []() { return gGlyphPasses > 0; });
#endif
  } else {
    std::puts("C77 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the list backdrop cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the list backdrop cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  if (argc > 1) {
    std::error_code error;
    std::filesystem::create_directories(argv[1], error);
    gOutDir = argv[1];
  }
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C90 BACKDROP-TEXT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C90 BACKDROP-TEXT CHECKS PASSED");
  return 0;
}
