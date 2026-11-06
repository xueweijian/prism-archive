// PrismArchive -- PrismWin
// C110: the GRID's refraction layer must not contain the tiles, and a tile must
// have a BODY in every theme.
//
// DEFECT 1 -- THE TILES ARE IN THE REFLECTION.
// C77 builds the backdrop by re-rendering the CONTENT into a 1/8-resolution
// target, blurring it and compositing it under everything (BuildBackdrop).
// DrawListMode has taken a "backdropPass" argument since C90, which is how the
// row glyphs and the selection pill left the refraction source. DrawGrid never
// had one: every tile -- its 6 % fill, its accent folder mark, its name plate
// and its decoded thumbnail -- was re-drawn into the ghost, then blurred over
// the whole window at 1/8. Measured on the integration tree (1482x991, only
// appearance.backdropRefraction switched):
//     px(480,400)  on=46,47,50   off=16,17,23
//     px(350,600)  on=30,31,35   off=16,17,23
//     px(800,530)  on=51,52,56   off=30,31,37
// -- a grey smear 30 levels above the canvas where there is nothing but a tile.
//
// DEFECT 2 -- THE TILE HAS NO BODY ON THE DAY CANVAS.
// The tile fill was the literal kTileFill = white 6 %. On the #F5F5F7 Day canvas
// that composites to 246 on 245: 0-1 level, i.e. no card at all -- the folder
// tiles "float in the air". A tile must be a MATERIAL: darker than the canvas on
// Day (black 5 %), lighter on Night (white 6 %) and on High Contrast (white 10 %).
//
// THE ASSERTIONS (real GalleryRenderer, headless WIC target, 1482x991, grid mode,
// folders -> tiles; every rectangle comes from WindowLayout, the same authority
// the renderer reads):
//   A1  BOTH themes, backdropRefraction OFF vs ON: the mean luminance inside
//       every tile rectangle may differ by at most 4 levels. On Midnight the
//       veil moves the WHOLE window ~4.3 levels, so the tile rectangle is
//       measured against the same frame's canvas-region movement -- that wash is
//       C77's compositing, not the tiles; the shipped code reads +38.9 after the
//       subtraction. On Daylight the veil IS the canvas colour (C102), so the
//       raw two-frame difference is asserted too (shipped: -7.46).
//   A2  DAYLIGHT, the same two frames: a CANVAS region (below the flow, where no
//       item can paint) may differ by at most 2 levels -- and the layer must
//       still have been composited (BackdropDraws() == 1, BackdropRebuilds() ==
//       1). "Fix A1 by deleting the layer" therefore fails this case, not the
//       pixel diff: on the Day canvas the veil is the canvas colour (C102), so
//       the wash is 0 by construction and switching the layer off would have
//       been invisible in pixels alone.
//   A3  DAYLIGHT: the tile's BODY patch (top-left, inside the fill, clear of the
//       corner radius, the folder mark and the name plate) is at least 6 levels
//       away from the canvas of the same frame. Shipped code reads 0.7.
//   A4  MIDNIGHT / HIGH CONTRAST: the same patch delta stays inside the band the
//       theme asks for (Night 8..24, Day 6..24, HC 12..40): a tile that flipped
//       to the Day material on the night canvas (or lost its fill entirely) must
//       go red, so "fix Day" cannot regress the other two themes.
//
// THE RULE THE FIX IMPLEMENTS (wider than the card's first wording, reviewed and
// approved): the ghost carries PIXELS THAT CAME FROM THE ARCHIVE -- a decoded
// thumbnail -- and never the chrome we drew around them (fill, folder mark,
// label, skeleton, selection ring). A folder grid therefore contributes nothing
// to the ghost at all, and A1 is measured on exactly that scene.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27):
//   PRISM_C110_MUTANT_TILES_IN_BACKDROP  the grid's backdrop pass draws the
//       tiles again (the shipped defect). A1 MUST go red and print
//       "C110 MUTATION CAUGHT: tiles-in-backdrop".
//   PRISM_C110_MUTANT_FLAT_TILE          TileFillFor() returns the old white 6 %
//       in every theme. A3 MUST go red and print
//       "C110 MUTATION CAUGHT: flat-tile".
// Both verdicts are armed on a counter that only the injected branch increments,
// so a red that came from something else stays red WITHOUT a false CAUGHT.
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
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;
using prism::ArchiveItem;
using prism::win::gallery::GalleryRenderer;

namespace {

// THE CARD'S FRAME: 1482x991 client area, grid mode, no rail (the paint this
// card was measured on). The probe rectangles are derived from the SAME layout
// and asserted against it below, so a layout change cannot silently move this
// suite onto empty canvas.
constexpr UINT32 kWidth = 1482;
constexpr UINT32 kHeight = 991;

// A1's limit (card): the tile rectangle may move 4 levels between the two
// frames. The veil alone moves a Midnight pixel ~3.5, which is why the limit is
// 4 and not 2.
constexpr double kTileRectLimit = 4.0;
// A2's limit (card): a canvas pixel may not move more than 2.
constexpr double kCanvasLimit = 2.0;
// A3's floor (card): the Day tile body must stand 6 levels off its canvas.
constexpr double kDayBodyFloor = 6.0;
// A4's bands: [low, high] for the body-vs-canvas delta per variant.
constexpr double kNightBodyLow = 8.0;
constexpr double kNightBodyHigh = 24.0;
constexpr double kDayBodyHigh = 24.0;
constexpr double kHighContrastBodyLow = 12.0;
constexpr double kHighContrastBodyHigh = 40.0;

#if defined(PRISM_C110_MUTANT_TILES_IN_BACKDROP)
// C110 MUTANT ACTIVE: tiles-in-backdrop -- DrawGrid's backdrop pass paints the
// tiles again, i.e. the code that shipped. A1 MUST go red.
inline constexpr const char* kMutantBanner = "C110 MUTANT ACTIVE: tiles-in-backdrop";
inline constexpr const char* kMutantMarker = "C110 MUTATION CAUGHT: tiles-in-backdrop";
#elif defined(PRISM_C110_MUTANT_FLAT_TILE)
// C110 MUTANT ACTIVE: flat-tile -- TileFillFor() is the pre-C110 white 6 % in
// every theme. A3 MUST go red.
inline constexpr const char* kMutantBanner = "C110 MUTANT ACTIVE: flat-tile";
inline constexpr const char* kMutantMarker = "C110 MUTATION CAUGHT: flat-tile";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

// D3: the injected branch really ran. Only the mutant arm of the renderer
// increments these, so a clean build reads 0 and a red for an unrelated reason
// can never print CAUGHT.
int gTilePasses = 0;
int gFlatTileHits = 0;

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
  // The audit metric this card was reported with: the plain mean of the three
  // channels (the card's numbers 46,47,50 are that mean, not Rec.709).
  double Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3.0;
  }
};

struct Box {
  int left = 0;
  int top = 0;
  int right = 0;   // exclusive
  int bottom = 0;  // exclusive
};

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

  bool Paint(const GalleryRenderer::Scene& scene, Frame* out) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    if (FAILED(target->EndDraw())) return false;
#if defined(PRISM_C110_MUTANT_TILES_IN_BACKDROP)
    gTilePasses += renderer.BackdropTilePasses();
#elif defined(PRISM_C110_MUTANT_FLAT_TILE)
    gFlatTileHits += renderer.FlatTileMutantHits();
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
  int BackdropDraws() const { return renderer.BackdropDraws(); }
  int BackdropRebuilds() const { return renderer.BackdropRebuilds(); }
};

// ---------------------------------------------------------------- fixture
// A demo.zip-shaped catalog: nine folders (tiles) followed by six text payloads
// (32 px rows). The tiles are what the refraction source must not carry.
struct Fixture {
  std::vector<ArchiveItem> catalog;
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  explicit Fixture(prism::theme::Variant theme) {
    const char* dirs[] = {"Photos/",      "Projects/",   "Music/",     "Videos/",
                          "Documents/",   "Downloads/",  "Backup/",    "Screenshots/",
                          "Archive/"};
    const char* files[] = {"README.txt",        "report_q4_final.txt", "notes_from_field.md",
                           "manifest.json",     "build_2026-11-06.log", "draft_copy.txt"};
    uint32_t index = 0;
    for (const char* path : dirs) {
      ArchiveItem item;
      item.path = path;
      item.index = index++;
      item.isDir = true;
      catalog.push_back(item);
    }
    for (const char* path : files) {
      ArchiveItem item;
      item.path = path;
      item.index = index++;
      item.size = 1'200'000u;
      item.packSize = 402'113u;
      catalog.push_back(item);
    }
    layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                    /*sidebarVisible=*/false, /*listMode=*/false);
    scene.theme = theme;
    scene.items = &catalog;
    scene.layout = &layout;
    scene.breadcrumb = L"demo.zip  |  15 items  |  solid block 1/2";
    scene.status.itemCount = static_cast<int>(catalog.size());
    scene.status.ratioView = prism::CompressionRatioView(7'200'000, 2'412'678);
  }

  // The item rectangles the RENDERER will paint, from the same layout + flow.
  std::vector<prism::layout::FlowCell> Cells() const {
    std::vector<prism::layout::FlowItem> flow;
    flow.reserve(catalog.size());
    for (const ArchiveItem& item : catalog) {
      prism::layout::FlowItem entry;
      entry.isDir = item.isDir;
      entry.kind = prism::QuickLookPlanner::KindByName(item.path);
      entry.thumbnailAvailable = false;  // no ThumbStore in this scene
      flow.push_back(entry);
    }
    return prism::layout::VisibleFlowCells(layout, flow, 0);
  }

  // Backdrop OFF and ON differ by exactly one flag.
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

bool Render(const GalleryRenderer::Scene& scene, Frame* out, int* draws = nullptr,
            int* rebuilds = nullptr) {
  Harness harness;
  if (!harness.Open(kWidth, kHeight)) return false;
  const bool ok = harness.Paint(scene, out);
  if (draws != nullptr) *draws = harness.BackdropDraws();
  if (rebuilds != nullptr) *rebuilds = harness.BackdropRebuilds();
  return ok;
}

// The canvas region this card measures: BELOW the flow, inside the content clip,
// where no cell can paint. Returned empty when the flow fills the viewport (the
// caller asserts it is non-empty, so the case can never pass vacuously).
Box CanvasProbe(const Fixture& fixture) {
  const prism::layout::Rect& inner = fixture.layout.contentInner;
  int lastBottom = inner.top;
  for (const prism::layout::FlowCell& cell : fixture.Cells()) {
    lastBottom = (std::max)(lastBottom, cell.rect.bottom);
  }
  const int top = lastBottom + 40;
  const int bottom = (std::min)(inner.bottom - 8, top + 180);
  if (bottom - top < 120) return Box{};
  return Box{inner.left + 40, top, inner.right - 40, bottom};
}

double MeanLuminance(const Frame& frame, const Box& box) {
  double sum = 0.0;
  long count = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      sum += frame.Luminance(x, y);
      ++count;
    }
  }
  return count > 0 ? sum / static_cast<double>(count) : 0.0;
}

Box InsetBox(const prism::layout::Rect& rect, int by) {
  return Box{rect.left + by, rect.top + by, rect.right - by, rect.bottom - by};
}

// The tile's BODY: top-left corner, inside the 16 px squircle, ABOVE the folder
// mark (which is centred) and clear of the 28 px name plate. Every pixel here is
// the tile's own fill, so its distance from the canvas is the card's body.
Box BodyPatch(const prism::layout::Rect& rect) {
  return Box{rect.left + 6, rect.top + 24, rect.left + 22, rect.top + 44};
}

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

}  // namespace

// ---------------------------------------------------------------------------
// A1: the grid's refraction layer carries no tiles
// ---------------------------------------------------------------------------
// The two frames differ by ONE flag. Everything the tile itself paints (fill,
// folder mark, name plate) is drawn ON TOP of the layer in both frames, so the
// only thing that can move a tile-rectangle pixel is what the ghost put under
// it. On the shipped code the ghost contained the tile, blurred: ~20-30 levels.
PRISM_TEST("c110.grid_tiles_stay_out_of_the_reflection") {
  // A1 runs in BOTH themes the defect shows up in. The card's numbers
  // (on=46,47,50 / off=16,17,23) are the Midnight frame; Day is where C102 made
  // "the veil IS the canvas colour", so there the layer's own wash is 0 and the
  // raw frame diff can be asserted on its own as well.
  struct Arm {
    prism::theme::Variant variant;
    const char* name;
    bool assertRaw;  // the veil is a no-op on this canvas (C102)
  };
  const Arm arms[] = {{prism::theme::Variant::Midnight, "midnight", false},
                      {prism::theme::Variant::Daylight, "daylight", true}};
  for (const Arm& arm : arms) {
    Fixture fixture(arm.variant);
    const std::vector<prism::layout::FlowCell> cells = fixture.Cells();
    int tileCount = 0;
    for (const auto& cell : cells) {
      if (cell.kind != prism::layout::CellKind::Tile) continue;
      if (cell.rect.top < fixture.layout.contentInner.top) continue;
      if (cell.rect.bottom > fixture.layout.contentInner.bottom) continue;
      ++tileCount;
    }
    // The scene must actually carry tiles, otherwise "no delta" is a statement
    // about an empty grid.
    PRISM_CHECK(tileCount >= 6);
    std::printf("C110 %s grid: %d tile cells of %d cells at %ux%u (tile %dx%d, %d columns)\n",
                arm.name, tileCount, static_cast<int>(cells.size()), kWidth, kHeight,
                fixture.layout.tileWidth, fixture.layout.cellHeight, fixture.layout.columns);

    const Box canvasBox = CanvasProbe(fixture);
    PRISM_CHECK(canvasBox.right > canvasBox.left);

    Frame reference;
    Frame lit;
    int draws = 0;
    int rebuilds = 0;
    PRISM_CHECK(Render(fixture.Off(), &reference));
    PRISM_CHECK(Render(fixture.On(), &lit, &draws, &rebuilds));
    if (reference.pixels.empty() || lit.pixels.empty()) return;
    // A1 only says something when the layer is actually built and composited:
    // otherwise "the tile rect did not move" would be "the layer is gone".
    PRISM_CHECK_EQ(draws, 1);
    PRISM_CHECK_EQ(rebuilds, 1);

    // The layer's OWN wash: the veil covers the whole window, so when the layer
    // comes on, every pixel -- tile or not -- moves by ~4.3 levels on the
    // Midnight canvas. That is C77's compositing, not the tiles, and A1 is about
    // the tiles: subtract the same-frame canvas movement. On the Day canvas the
    // veil IS the canvas colour (C102), so this number is 0 and the raw diff is
    // asserted there as well.
    const double canvasOff = MeanLuminance(reference, canvasBox);
    const double canvasOn = MeanLuminance(lit, canvasBox);
    const double wash = canvasOn - canvasOff;
    std::printf("C110 A1 %s layer wash (canvas region) = %+.2f levels\n", arm.name, wash);

    double worstRelative = 0.0;
    double worstRaw = 0.0;
    int worstIndex = -1;
    for (const auto& cell : cells) {
      if (cell.kind != prism::layout::CellKind::Tile) continue;
      if (cell.rect.top < fixture.layout.contentInner.top) continue;
      if (cell.rect.bottom > fixture.layout.contentInner.bottom) continue;
      const Box probe = InsetBox(cell.rect, 3);
      const double off = MeanLuminance(reference, probe);
      const double on = MeanLuminance(lit, probe);
      const double delta = on - off;
      std::printf("C110 A1 %s tile[%d] rect=(%d,%d)-(%d,%d) offLum=%.2f onLum=%.2f delta=%+.2f "
                  "relative=%+.2f\n",
                  arm.name, cell.index, cell.rect.left, cell.rect.top, cell.rect.right,
                  cell.rect.bottom, off, on, delta, delta - wash);
      if (std::fabs(delta - wash) > worstRelative) {
        worstRelative = std::fabs(delta - wash);
        worstIndex = cell.index;
      }
      if (std::fabs(delta) > worstRaw) worstRaw = std::fabs(delta);
    }
    std::printf("C110 A1 %s worst |delta - wash| = %.2f levels at tile[%d] (limit %.1f)\n",
                arm.name, worstRelative, worstIndex, kTileRectLimit);
    // RED on the shipped code: the ghost is a blurred copy of these very tiles.
    PRISM_CHECK(worstRelative <= kTileRectLimit);
    if (arm.assertRaw) {
      std::printf("C110 A1 %s worst raw |delta| = %.2f levels (limit %.1f)\n", arm.name,
                  worstRaw, kTileRectLimit);
      PRISM_CHECK(worstRaw <= kTileRectLimit);
    }
  }
}

// ---------------------------------------------------------------------------
// A2 + A3: Daylight -- the canvas is untouched, and the tile has a body
// ---------------------------------------------------------------------------
// A2 is measured on the DAY canvas because that is where C102 made the veil the
// canvas colour: the layer's own wash is 0 there by construction, so a non-zero
// delta can only come from something the layer PAINTED into the canvas region.
// "Fix A1 by deleting the layer" is caught by the two counters, not by the
// pixels -- and that is exactly why they are asserted in the same case.
PRISM_TEST("c110.day_canvas_is_clean_and_the_tile_has_a_body") {
  Fixture fixture(prism::theme::Variant::Daylight);
  const Box canvasBox = CanvasProbe(fixture);
  PRISM_CHECK(canvasBox.right > canvasBox.left);
  PRISM_CHECK(canvasBox.bottom > canvasBox.top);
  const std::vector<prism::layout::FlowCell> cells = fixture.Cells();

  Frame reference;
  Frame lit;
  int draws = 0;
  int rebuilds = 0;
  PRISM_CHECK(Render(fixture.Off(), &reference));
  PRISM_CHECK(Render(fixture.On(), &lit, &draws, &rebuilds));
  if (reference.pixels.empty() || lit.pixels.empty()) return;
  PRISM_CHECK_EQ(draws, 1);
  PRISM_CHECK_EQ(rebuilds, 1);
  if (!WritePng(L"c110-day.png", lit)) std::puts("  WARN: PNG failed");

  // A2 -- the canvas region moved by nothing the layer painted.
  const double canvasOff = MeanLuminance(reference, canvasBox);
  const double canvasOn = MeanLuminance(lit, canvasBox);
  std::printf("C110 A2 canvas=(%d,%d)-(%d,%d) offLum=%.2f onLum=%.2f delta=%+.2f (limit %.1f)\n",
              canvasBox.left, canvasBox.top, canvasBox.right, canvasBox.bottom, canvasOff,
              canvasOn, canvasOn - canvasOff, kCanvasLimit);
  PRISM_CHECK(std::fabs(canvasOn - canvasOff) <= kCanvasLimit);

  // A3 -- the tile body is darker than the Day canvas (black 5 %), not 0.7
  // levels of white 6 %.
  double worstBody = 1e9;
  int worstIndex = -1;
  int measured = 0;
  for (const auto& cell : cells) {
    if (cell.kind != prism::layout::CellKind::Tile) continue;
    if (cell.rect.bottom > fixture.layout.contentInner.bottom) continue;
    if (cell.rect.right > fixture.layout.contentInner.right) continue;
    const Box patch = BodyPatch(cell.rect);
    const double body = MeanLuminance(lit, patch);
    const double delta = body - canvasOn;  // signed: Day's body is DARKER
    std::printf("C110 A3 tile[%d] body=(%d,%d)-(%d,%d) lum=%.2f canvas=%.2f delta=%+.2f\n",
                cell.index, patch.left, patch.top, patch.right, patch.bottom, body, canvasOn,
                delta);
    ++measured;
    if (-delta < worstBody) {
      worstBody = -delta;
      worstIndex = cell.index;
    }
  }
  PRISM_CHECK(measured >= 6);
  std::printf("C110 A3 weakest body = %.2f levels at tile[%d] (floor %.1f)\n", worstBody,
              worstIndex, kDayBodyFloor);
  PRISM_CHECK(worstBody >= kDayBodyFloor);
  // A4's Day half: the body must not become a hole either (a 5 % black tile is
  // ~12 levels; 24 leaves room for a material change but not for a literal).
  PRISM_CHECK(worstBody <= kDayBodyHigh);
}

// ---------------------------------------------------------------------------
// A4: Night / High Contrast keep their own material
// ---------------------------------------------------------------------------
// The Day fix must not reach the other two themes: on the night canvas the tile
// is LIGHTER than the canvas (white 6 %), on High Contrast it is white 10 % on
// black. The bands are wide enough to allow a design change, narrow enough that
// the Day material (black 5 % -> ~1 level on black) is red on both.
PRISM_TEST("c110.night_and_high_contrast_keep_their_tile_material") {
  struct Arm {
    prism::theme::Variant variant;
    const wchar_t* png;
    double low;
    double high;
    const char* name;
  };
  const Arm arms[] = {{prism::theme::Variant::Midnight, L"c110-night.png", kNightBodyLow,
                       kNightBodyHigh, "midnight"},
                      {prism::theme::Variant::HighContrast, nullptr, kHighContrastBodyLow,
                       kHighContrastBodyHigh, "high-contrast"}};
  for (const Arm& arm : arms) {
    Fixture fixture(arm.variant);
    const Box canvasBox = CanvasProbe(fixture);
    PRISM_CHECK(canvasBox.right > canvasBox.left);
    Frame lit;
    int draws = 0;
    int rebuilds = 0;
    PRISM_CHECK(Render(fixture.On(), &lit, &draws, &rebuilds));
    if (lit.pixels.empty()) return;
    PRISM_CHECK_EQ(draws, 1);
    PRISM_CHECK_EQ(rebuilds, 1);
    if (arm.png != nullptr && !WritePng(arm.png, lit)) std::puts("  WARN: PNG failed");

    const double canvas = MeanLuminance(lit, canvasBox);
    const std::vector<prism::layout::FlowCell> cells = fixture.Cells();
    int measured = 0;
    double worstLow = 1e9;
    double worstHigh = -1e9;
    for (const auto& cell : cells) {
      if (cell.kind != prism::layout::CellKind::Tile) continue;
      if (cell.rect.bottom > fixture.layout.contentInner.bottom) continue;
      if (cell.rect.right > fixture.layout.contentInner.right) continue;
      const double body = MeanLuminance(lit, BodyPatch(cell.rect));
      const double delta = body - canvas;
      std::printf("C110 A4 %s tile[%d] bodyLum=%.2f canvasLum=%.2f delta=%+.2f\n", arm.name,
                  cell.index, body, canvas, delta);
      ++measured;
      worstLow = (std::min)(worstLow, delta);
      worstHigh = (std::max)(worstHigh, delta);
    }
    PRISM_CHECK(measured >= 6);
    std::printf("C110 A4 %s body delta range = [%.2f, %.2f] (want [%.1f, %.1f])\n", arm.name,
                worstLow, worstHigh, arm.low, arm.high);
    PRISM_CHECK(worstLow >= arm.low);
    PRISM_CHECK(worstHigh <= arm.high);
  }
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
#if defined(PRISM_C110_MUTANT_TILES_IN_BACKDROP)
    prism::test::SetMutationVerdict(kMutantMarker, []() { return gTilePasses > 0; });
#elif defined(PRISM_C110_MUTANT_FLAT_TILE)
    prism::test::SetMutationVerdict(kMutantMarker, []() { return gFlatTileHits > 0; });
#endif
  } else {
    std::puts("C110 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the grid backdrop cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the grid backdrop cannot be verified");
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
    std::puts("C110 GRID-BACKDROP CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C110 GRID-BACKDROP CHECKS PASSED");
  return 0;
}
