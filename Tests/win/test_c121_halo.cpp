// PrismArchive -- PrismWin
// C121: THE HALO. The refraction layer must carry the PAGE, not the archive.
//
// THE DEFECT (docs/DESIGN-PROPOSAL-BANDIZIP.md 2.4, 12.1; docs/design/C #1).
// C77 re-renders the "content" into a 1/8 target and blurs it under everything.
// C110 kept the DECODED THUMBNAIL in that source "because it is archive content",
// so a photo becomes a solid 1/8 plate, the 28 px blur spreads it 46-48 logical
// px (4.4x the 13 px gap between two tiles) and the veil leaves 35 % of it:
//     0.35 x (210,60,50) + 0.65 x (12,13,17) = (81,29,29)   model
//     measured in the gap                    = (81,29,28)
// 21 % of the content area tinted, chroma 53 where the no-refraction frame reads
// 7. The same layer also moves the CANVAS itself, (16,17,23) -> (13,14,18),
// dE 2.4: a wash that is darker than the colour it washes is not a refraction.
//
// WHAT THIS SUITE ASSERTS (real GalleryRenderer, headless WIC target, grid with
// photo tiles -- the scene the defect was measured in):
//   H1  the CHROMA (max(R,G,B) - min(R,G,B)) in the gap between two adjacent
//       photo tiles is <= 10 on average (the audit's threshold; today 53). The
//       photos themselves are asserted to be chromatic, so a frame that simply
//       drew no tiles cannot pass.
//   H2  toggling appearance.backdropRefraction moves the FAR canvas (the audit's
//       probe: empty content below the flow, away from every panel) by dE <= 1
//       (today 2.4). The layer must be chroma-neutral.
//   H3  the CHROMA spread below a photo tile is <= 12 logical px: scan down from
//       the tile's bottom edge and stop where the pixel reaches the canvas
//       (today 46-48).
//   H4  the cache KEY carries the THEME: a theme switch rebuilds the source, so
//       the second frame's canvas is the second theme's canvas (the source is
//       OPAQUE, so a stale one paints the old canvas over the new theme). And
//       the scroll is NOT an input: scrolling 100 px must not rebuild or change
//       a single ghost pixel (C77-3's "a still of the page", with the reason).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): each arm is the shipped defect, in
// the SAME source, registered as its own target with PASS_REGULAR_EXPRESSION on
// the verdict -- never WILL_FAIL (D26 forbids combining them). The verdict is a
// CONJUNCTION: printed at the FIRST failing check and only when the injected
// branch actually ran (the renderer counts it), so a red for an unrelated reason
// can never print CAUGHT.
//   PRISM_C121_MUTANT_TILES_IN_GHOST  the decoded thumbnails go back into the
//       source -> H1 (and H3) MUST go red.
//   PRISM_C121_MUTANT_KEY_NO_THEME    the key drops the theme (the shipped key)
//       -> H4 MUST go red.
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
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;
using prism::ArchiveItem;
using prism::win::gallery::GalleryRenderer;

namespace {

constexpr UINT32 kWidth = 1600;
constexpr UINT32 kHeight = 1000;

// H1: the audit's threshold (DESIGN-PROPOSAL 17.7: the gap's chroma must be
// <= 10; the shipped layer reads 53 and the no-refraction frame reads 7).
constexpr int kGapChromaLimit = 10;
// H2: R2 of the material rules -- toggling refraction may move the canvas by at
// most dE 1 (the shipped layer moves it 2.4).
constexpr double kCanvasDeltaELimit = 1.0;
// H3: "any glow spreads <= 12 lp" (R3).
constexpr int kSpreadLimitPx = 12;
// A photo tile's own chroma, as a control: the fixture's tints are saturated, so
// a tile that reads below this means the scene painted no photo at all.
constexpr int kPhotoChromaFloor = 60;

#if defined(PRISM_C121_MUTANT_TILES_IN_GHOST)
inline constexpr const char* kMutantBanner = "C121 MUTANT ACTIVE: tiles-in-ghost";
inline constexpr const char* kMutantMarker = "C121 MUTATION CAUGHT: tiles-in-ghost";
inline constexpr bool kTilesInGhost = true;
#elif defined(PRISM_C121_MUTANT_KEY_NO_THEME)
inline constexpr const char* kMutantBanner = "C121 MUTANT ACTIVE: key-no-theme";
inline constexpr const char* kMutantMarker = "C121 MUTATION CAUGHT: key-no-theme";
inline constexpr bool kTilesInGhost = false;
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
inline constexpr bool kTilesInGhost = false;
#endif

// D3: the injected branch really ran. Published by Harness::Paint from the
// renderer's own counters, so the verdict predicate never reads a case's locals.
int gEntryPasses = 0;
int gKeyNoThemeHits = 0;

IWICImagingFactory* gWic = nullptr;

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
  // The audit's chroma: max(R,G,B) - min(R,G,B), on the 8-bit pixel.
  int Chroma(int x, int y) const {
    const int r = Channel(x, y, 16);
    const int g = Channel(x, y, 8);
    const int b = Channel(x, y, 0);
    return (std::max)(r, (std::max)(g, b)) - (std::min)(r, (std::min)(g, b));
  }
};

struct Box {
  int left = 0;
  int top = 0;
  int right = 0;   // exclusive
  int bottom = 0;  // exclusive
  bool Empty() const { return right <= left || bottom <= top; }
};

// dE76, the metric the audit reported its 2.4 / 30.5 in (docs/design/C 2.2.2).
// Kept here, not in the product: no shipping code has an opinion about colour
// difference. sRGB -> linear -> XYZ (D65) -> Lab.
struct Lab {
  double l = 0.0;
  double a = 0.0;
  double b = 0.0;
};

double SrgbToLinear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Lab ToLab(int r, int g, int b) {
  const double rl = SrgbToLinear(r / 255.0);
  const double gl = SrgbToLinear(g / 255.0);
  const double bl = SrgbToLinear(b / 255.0);
  const double x = (0.4124564 * rl + 0.3575761 * gl + 0.1804375 * bl) / 0.95047;
  const double y = 0.2126729 * rl + 0.7151522 * gl + 0.0721750 * bl;
  const double z = (0.0193339 * rl + 0.1191920 * gl + 0.9503041 * bl) / 1.08883;
  const auto f = [](double t) {
    return t > 0.008856451679 ? std::cbrt(t) : (7.787037037 * t + 16.0 / 116.0);
  };
  const double fx = f(x);
  const double fy = f(y);
  const double fz = f(z);
  Lab lab;
  lab.l = 116.0 * fy - 16.0;
  lab.a = 500.0 * (fx - fy);
  lab.b = 200.0 * (fy - fz);
  return lab;
}

double DeltaE(UINT32 pa, UINT32 pb) {
  const Lab a = ToLab(static_cast<int>((pa >> 16) & 0xFFu), static_cast<int>((pa >> 8) & 0xFFu),
                      static_cast<int>(pa & 0xFFu));
  const Lab b = ToLab(static_cast<int>((pb >> 16) & 0xFFu), static_cast<int>((pb >> 8) & 0xFFu),
                      static_cast<int>(pb & 0xFFu));
  const double dl = a.l - b.l;
  const double da = a.a - b.a;
  const double db = a.b - b.b;
  return std::sqrt(dl * dl + da * da + db * db);
}

double MeanDeltaE(const Frame& a, const Frame& b, const Box& box) {
  double sum = 0.0;
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!a.In(x, y) || !b.In(x, y)) continue;
      sum += DeltaE(a.At(x, y), b.At(x, y));
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

double MeanChroma(const Frame& frame, const Box& box) {
  double sum = 0.0;
  int count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      sum += frame.Chroma(x, y);
      ++count;
    }
  }
  return count > 0 ? sum / count : 0.0;
}

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

  // One frame. ghost=true paints the refraction SOURCE over the finished frame
  // as the last operation; the source is opaque, so the read-back IS the ghost.
  bool Paint(const GalleryRenderer::Scene& scene, Frame* out, bool ghost = false) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
#if defined(PRISM_C121_MUTANT_TILES_IN_GHOST)
    gEntryPasses += renderer.BackdropEntryPasses();
#elif defined(PRISM_C121_MUTANT_KEY_NO_THEME)
    gKeyNoThemeHits += renderer.KeyNoThemeMutantHits();
#endif
    if (ghost && !renderer.PaintRefractionSource(target.Get())) return false;
    if (FAILED(target->EndDraw())) return false;
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

UINT32 CanvasPixel(prism::theme::Variant variant) {
  const prism::theme::Rgba token = prism::chrome::CanvasPresentedColor(variant);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(token.a) << 24) | (q(token.r) << 16) | (q(token.g) << 8) | q(token.b);
}

ArchiveItem ImageItem(const char* path, std::uint32_t index, std::uint64_t size) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  return item;
}

std::vector<ArchiveItem> Catalog() {
  std::vector<ArchiveItem> items;
  items.push_back(ImageItem("Berlin_Trip_2026/", 0, 0));
  items.back().isDir = true;
  const char* names[] = {"gate_front.png", "gate_night.png", "quadriga.png",
                         "bode_morning.png", "pergamon.png",   "neues_dome.png",
                         "mural_01.png",    "mural_02.png",   "street_04.png",
                         "street_05.png",   "street_06.png",  "street_07.png"};
  for (int i = 0; i < 12; ++i) {
    char path[96];
    std::snprintf(path, sizeof(path), "Berlin_Trip_2026/%s", names[i]);
    items.push_back(ImageItem(path, static_cast<std::uint32_t>(i + 1),
                              3'100'000u + 977u * static_cast<std::uint64_t>(i)));
  }
  return items;
}

// The audit's photo colours: saturated, so a 1/8 plate of one is unmistakable.
void FillThumbs(const std::vector<ArchiveItem>& items, prism::gallery::ThumbStore* store) {
  const unsigned char tints[12][3] = {
      {210, 60, 50},   {40, 160, 210}, {200, 70, 140},  {90, 200, 120},
      {240, 190, 70},  {120, 90, 230}, {60, 200, 200},  {230, 100, 100},
      {150, 220, 90},  {210, 140, 220}, {80, 130, 240}, {250, 160, 90},
  };
  int slot = 0;
  for (const ArchiveItem& item : items) {
    if (item.isDir) continue;
    prism::gallery::DecodedThumb thumb;
    thumb.width = 8;
    thumb.height = 8;
    thumb.pixels.assign(8u * 8u * 4u, 0);
    for (size_t px = 0; px < 64; ++px) {
      thumb.pixels[px * 4 + 0] = tints[slot % 12][2];
      thumb.pixels[px * 4 + 1] = tints[slot % 12][1];
      thumb.pixels[px * 4 + 2] = tints[slot % 12][0];
      thumb.pixels[px * 4 + 3] = 0xFF;
    }
    store->OnThumb(item.index, thumb);
    ++slot;
  }
}

struct Fixture {
  std::vector<ArchiveItem> items;
  prism::gallery::ThumbStore thumbs{64u << 20};
  prism::gallery::ViewportWindow viewport{{.columns = 5, .cellHeight = 220.0}, 13};
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  Fixture() {
    items = Catalog();
    FillThumbs(items, &thumbs);
    viewport.SetViewportSize(static_cast<int>(kWidth), static_cast<int>(kHeight));
    viewport.SetScroll(0.0);  // row 0 at the top: the gap probe is deterministic
    layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                    /*sidebarVisible=*/false, /*listMode=*/false);
    scene.theme = prism::theme::Variant::Midnight;
    scene.items = &items;
    scene.viewport = &viewport;
    scene.thumbs = &thumbs;
    scene.layout = &layout;
    scene.breadcrumb = L"Berlin_Trip_2026.zip  |  13 items  |  5 solid blocks";
    scene.status.itemCount = static_cast<int>(items.size());
    scene.backdropRefraction = true;
  }

  // The cells the renderer will paint: the same flow WindowLayout hands it, so
  // the probes cannot drift onto empty canvas after a layout change.
  std::vector<prism::layout::FlowCell> Cells() const {
    std::vector<prism::layout::FlowItem> flow;
    flow.reserve(items.size());
    for (const ArchiveItem& item : items) {
      prism::layout::FlowItem entry;
      entry.isDir = item.isDir;
      entry.kind = prism::QuickLookPlanner::KindByName(item.path);
      entry.thumbnailAvailable = thumbs.Contains(item.index) &&
                                 prism::layout::ThumbnailCapable(entry.kind);
      flow.push_back(entry);
    }
    return prism::layout::VisibleFlowCells(layout, flow,
                                           static_cast<int>(viewport.Scroll()));
  }

  GalleryRenderer::Scene Off() const {
    GalleryRenderer::Scene s = scene;
    s.backdropRefraction = false;
    return s;
  }
};

struct GapProbe {
  Box gap;
  Box leftTile;
};

GapProbe FindGap(const Fixture& fixture) {
  GapProbe best;
  const std::vector<prism::layout::FlowCell> cells = fixture.Cells();
  for (size_t i = 0; i + 1 < cells.size(); ++i) {
    const prism::layout::FlowCell& a = cells[i];
    const prism::layout::FlowCell& b = cells[i + 1];
    if (a.kind != prism::layout::CellKind::Tile || b.kind != prism::layout::CellKind::Tile) {
      continue;
    }
    // PHOTO tiles on both sides: a folder tile carries no decoded pixels, so the
    // gap next to it would measure half the effect and the control patch (which
    // asserts the tile is saturated) would be measuring a folder glyph.
    if (fixture.items[static_cast<size_t>(a.index)].isDir ||
        fixture.items[static_cast<size_t>(b.index)].isDir) {
      continue;
    }
    if (a.rect.top != b.rect.top) continue;  // same row only
    const int gapWidth = b.rect.left - a.rect.right;
    if (gapWidth <= 0) continue;
    if (gapWidth <= best.gap.right - best.gap.left) continue;
    // The gap, over the photo part of the tiles (above the 28 px name plate).
    best.gap = Box{a.rect.right, a.rect.top + 40, b.rect.left, a.rect.bottom - 40};
    best.leftTile = Box{a.rect.left, a.rect.top, a.rect.right, a.rect.bottom};
  }
  return best;
}

// A tile on the LAST row of the flow: nothing is below it but canvas, so a
// downward scan measures the glow and never the next row's photo.
Box LastRowTile(const Fixture& fixture) {
  const std::vector<prism::layout::FlowCell> cells = fixture.Cells();
  Box best;
  int bestBottom = -1;
  for (const prism::layout::FlowCell& cell : cells) {
    if (cell.kind != prism::layout::CellKind::Tile) continue;
    if (fixture.items[static_cast<size_t>(cell.index)].isDir) continue;
    if (cell.rect.bottom > fixture.layout.contentInner.bottom) continue;
    if (cell.rect.bottom > bestBottom) {
      bestBottom = cell.rect.bottom;
      best = Box{cell.rect.left, cell.rect.top, cell.rect.right, cell.rect.bottom};
    }
  }
  return best;
}

// The audit's far-canvas probe: below the flow, inside the content, at least
// 40 px from every panel edge (a band's own blur is page background, not the
// halo this card removes).
Box FarCanvas(const Fixture& fixture) {
  const std::vector<prism::layout::FlowCell> cells = fixture.Cells();
  int lastBottom = fixture.layout.contentInner.top;
  for (const prism::layout::FlowCell& cell : cells) {
    lastBottom = (std::max)(lastBottom, cell.rect.bottom);
  }
  const int top = lastBottom + 48;
  int bottom = fixture.layout.statusBar.top - 40;
  if (bottom > fixture.layout.contentInner.bottom - 8) {
    bottom = fixture.layout.contentInner.bottom - 8;
  }
  const int left = fixture.layout.contentInner.left + 60;
  const int right = fixture.layout.contentInner.right - 60;
  if (bottom - top < 80) return Box{};
  return Box{left, top, right, bottom};
}

}  // namespace

// ---------------------------------------------------------------------------
// H1: the gap between two photo tiles is canvas, not a colour plate
// ---------------------------------------------------------------------------
// The reporter's sample (DESIGN-PROPOSAL 17.7): the whole gap between adjacent
// tiles. On the shipped layer it reads the tile's own colour at 35 % -- chroma
// 53 -- because the thumbnail IS the 1/8 source. With the source scoped to the
// page it reads the canvas.
PRISM_TEST("c121.h1_tile_gap_chroma") {
  Fixture fixture;
  const GapProbe probe = FindGap(fixture);
  PRISM_CHECK(!probe.gap.Empty());
  if (probe.gap.Empty()) return;
  PRISM_CHECK(probe.gap.right - probe.gap.left >= 6);

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame lit;
  PRISM_CHECK(harness.Paint(fixture.scene, &lit));
  if (lit.pixels.empty()) return;

  // The layer really ran: the gap is only interesting if the ghost was built
  // and composited (a frame that painted no layer must not pass this).
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 1);
  PRISM_CHECK_EQ(harness.renderer.BackdropDraws(), 1);

  // Control: the photos ARE in the frame and ARE saturated. Without this the
  // gap could be clean because nothing was drawn.
  const Box photo = Box{probe.leftTile.left + 20, probe.leftTile.top + 60,
                        probe.leftTile.right - 20, probe.leftTile.top + 160};
  const double photoChroma = MeanChroma(lit, photo);
  std::printf("C121 H1 photo chroma=%.2f (floor %d)\n", photoChroma, kPhotoChromaFloor);
  PRISM_CHECK(photoChroma >= kPhotoChromaFloor);

  const double gapChroma = MeanChroma(lit, probe.gap);
  std::printf("C121 H1 gap=(%d,%d)-(%d,%d) meanChroma=%.2f limit=%d\n", probe.gap.left,
              probe.gap.top, probe.gap.right, probe.gap.bottom, gapChroma, kGapChromaLimit);
  PRISM_CHECK(gapChroma <= kGapChromaLimit);
}

// ---------------------------------------------------------------------------
// H2: turning the layer on does not move the canvas (chroma neutrality)
// ---------------------------------------------------------------------------
PRISM_TEST("c121.h2_canvas_delta_e") {
  Fixture fixture;
  const Box canvas = FarCanvas(fixture);
  PRISM_CHECK(!canvas.Empty());
  if (canvas.Empty()) return;

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame reference;
  Frame lit;
  PRISM_CHECK(harness.Paint(fixture.Off(), &reference));
  PRISM_CHECK(harness.Paint(fixture.scene, &lit));
  if (reference.pixels.empty() || lit.pixels.empty()) return;
  PRISM_CHECK_EQ(harness.renderer.BackdropDraws(), 1);

  // The probe must really be canvas in the reference, or the dE would be
  // measuring a tile.
  const UINT32 canvasPixel = CanvasPixel(fixture.scene.theme);
  int offCanvas = 0;
  for (int y = canvas.top; y < canvas.bottom; y += 5) {
    for (int x = canvas.left; x < canvas.right; x += 5) {
      if (!reference.In(x, y)) continue;
      const int r = static_cast<int>((reference.At(x, y) >> 16) & 0xFFu);
      const int g = static_cast<int>((reference.At(x, y) >> 8) & 0xFFu);
      const int b = static_cast<int>(reference.At(x, y) & 0xFFu);
      if (std::abs(r - static_cast<int>((canvasPixel >> 16) & 0xFFu)) > 1 ||
          std::abs(g - static_cast<int>((canvasPixel >> 8) & 0xFFu)) > 1 ||
          std::abs(b - static_cast<int>(canvasPixel & 0xFFu)) > 1) {
        ++offCanvas;
      }
    }
  }
  std::printf("C121 H2 probe=(%d,%d)-(%d,%d) offCanvasSamples=%d\n", canvas.left, canvas.top,
              canvas.right, canvas.bottom, offCanvas);
  PRISM_CHECK_EQ(offCanvas, 0);

  const double meanDe = MeanDeltaE(reference, lit, canvas);
  double maxDe = 0.0;
  for (int y = canvas.top; y < canvas.bottom; ++y) {
    for (int x = canvas.left; x < canvas.right; ++x) {
      if (!reference.In(x, y) || !lit.In(x, y)) continue;
      maxDe = (std::max)(maxDe, DeltaE(reference.At(x, y), lit.At(x, y)));
    }
  }
  std::printf("C121 H2 meanDeltaE=%.3f maxDeltaE=%.3f limit=%.1f\n", meanDe, maxDe,
              kCanvasDeltaELimit);
  PRISM_CHECK(meanDe <= kCanvasDeltaELimit);
  PRISM_CHECK(maxDe <= 3.0);  // the wash was dE 2.4 on the canvas itself
}

// ---------------------------------------------------------------------------
// H3: the glow reaches at most one gap (12 lp) past a tile's edge
// ---------------------------------------------------------------------------
PRISM_TEST("c121.h3_halo_spread") {
  Fixture fixture;
  const Box tile = LastRowTile(fixture);
  PRISM_CHECK(!tile.Empty());
  if (tile.Empty()) return;

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame lit;
  PRISM_CHECK(harness.Paint(fixture.scene, &lit));
  if (lit.pixels.empty()) return;

  const int x = (tile.left + tile.right) / 2;
  // The photo's own bottom edge, found in the PIXELS: the last row inside the
  // tile that is still saturated. The audit measured the glow from the ink's
  // edge (its tile had no 30 px name plate under the photo), so the scan starts
  // where the photo stops -- not at the tile's rectangle, which would credit
  // the plate's 30 px to the glow.
  int photoBottom = -1;
  for (int y = tile.bottom - 1; y > tile.top + 40; --y) {
    if (!lit.In(x, y)) continue;
    if (lit.Chroma(x, y) >= kPhotoChromaFloor) {
      photoBottom = y;
      break;
    }
  }
  PRISM_CHECK(photoBottom > tile.top + 80);
  if (photoBottom < 0) return;
  std::printf("C121 H3 tile=(%d,%d)-(%d,%d) x=%d photoBottom=%d\n", tile.left, tile.top,
              tile.right, tile.bottom, x, photoBottom);

  // The canvas level: the lowest chroma well below the tile (the audit's
  // reference probe).
  int canvasChroma = 255;
  for (int y = photoBottom + 140; y < photoBottom + 180; ++y) {
    if (!lit.In(x, y)) continue;
    canvasChroma = (std::min)(canvasChroma, lit.Chroma(x, y));
  }
  std::printf("C121 H3 canvasChroma=%d\n", canvasChroma);

  int spread = -1;
  for (int d = 1; d <= 160; ++d) {
    if (!lit.In(x, photoBottom + d)) break;
    if (lit.Chroma(x, photoBottom + d) <= canvasChroma + 2) {
      spread = d;
      break;
    }
  }
  std::printf("C121 H3 spread=%d px limit=%d\n", spread, kSpreadLimitPx);
  PRISM_CHECK(spread > 0);
  PRISM_CHECK(spread <= kSpreadLimitPx);
}

// ---------------------------------------------------------------------------
// H4: the key carries the THEME, and the scroll is not an input
// ---------------------------------------------------------------------------
PRISM_TEST("c121.h4_key_theme_and_scroll") {
  Fixture fixture;
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;

  GalleryRenderer::Scene day = fixture.scene;
  day.theme = prism::theme::Variant::Daylight;
  GalleryRenderer::Scene night = fixture.scene;
  night.theme = prism::theme::Variant::Midnight;

  Frame dayFrame;
  PRISM_CHECK(harness.Paint(day, &dayFrame));
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 1);
  if (dayFrame.pixels.empty()) return;
  const Box canvas = FarCanvas(fixture);
  PRISM_CHECK(!canvas.Empty());
  if (canvas.Empty()) return;
  const int probeX = (canvas.left + canvas.right) / 2;
  const int probeY = (canvas.top + canvas.bottom) / 2;

  const auto matches = [](UINT32 pixel, UINT32 expected) {
    for (int shift = 0; shift <= 16; shift += 8) {
      if (std::abs(static_cast<int>((pixel >> shift) & 0xFFu) -
                   static_cast<int>((expected >> shift) & 0xFFu)) > 2) {
        return false;
      }
    }
    return true;
  };
  std::printf("C121 H4 day canvas=%08X expected=%08X\n", dayFrame.At(probeX, probeY),
              CanvasPixel(prism::theme::Variant::Daylight));
  PRISM_CHECK(matches(dayFrame.At(probeX, probeY),
                      CanvasPixel(prism::theme::Variant::Daylight)));

  // THE THEME IS IN THE KEY: the second frame must be the SECOND theme. A stale
  // opaque source would paint the daylight canvas over the midnight frame.
  Frame nightFrame;
  PRISM_CHECK(harness.Paint(night, &nightFrame));
  if (nightFrame.pixels.empty()) return;
  std::printf("C121 H4 theme switch: rebuilds=%d nightCanvas=%08X expected=%08X\n",
              harness.renderer.BackdropRebuilds(), nightFrame.At(probeX, probeY),
              CanvasPixel(prism::theme::Variant::Midnight));
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 2);
  PRISM_CHECK(
      matches(nightFrame.At(probeX, probeY), CanvasPixel(prism::theme::Variant::Midnight)));

  // THE SCROLL IS NOT AN INPUT: 100 px later the source is the same bitmap and
  // the ghost has not moved a pixel.
  Frame ghostBefore;
  PRISM_CHECK(harness.Paint(night, &ghostBefore, /*ghost=*/true));
  if (ghostBefore.pixels.empty()) return;
  fixture.viewport.SetScroll(100.0);
  GalleryRenderer::Scene scrolled = night;
  scrolled.viewport = &fixture.viewport;
  Frame ghostAfter;
  PRISM_CHECK(harness.Paint(scrolled, &ghostAfter, /*ghost=*/true));
  if (ghostAfter.pixels.empty()) return;
  std::printf("C121 H4 scroll: rebuilds=%d\n", harness.renderer.BackdropRebuilds());
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 2);
  int ghostMoved = 0;
  for (int y = canvas.top; y < canvas.bottom; y += 3) {
    for (int x = canvas.left; x < canvas.right; x += 3) {
      if (!ghostBefore.In(x, y) || !ghostAfter.In(x, y)) continue;
      if (ghostBefore.At(x, y) != ghostAfter.At(x, y)) ++ghostMoved;
    }
  }
  std::printf("C121 H4 ghost pixels moved by scrolling=%d\n", ghostMoved);
  PRISM_CHECK_EQ(ghostMoved, 0);
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    if (kTilesInGhost) {
      // D3: armed on the renderer's own counter -- H1 may only print CAUGHT when
      // the injected branch really drew the thumbnails into the ghost.
      prism::test::SetMutationVerdict(kMutantMarker, []() { return gEntryPasses > 0; });
    } else {
      prism::test::SetMutationVerdict(kMutantMarker, []() { return gKeyNoThemeHits > 0; });
    }
  } else {
    std::puts("C121 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the halo cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the halo cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C121 HALO CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C121 HALO CHECKS PASSED");
  return 0;
}
