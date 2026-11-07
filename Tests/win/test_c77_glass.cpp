// PrismArchive -- PrismWin
// C77: the BACKDROP LAYER and the GLASS PANEL, asserted in pixels.
//
// The user's report was "the interface is nowhere near Liquid Glass". The two
// things that were missing are exactly the two things this suite measures:
//
//   1. NO BACKDROP LAYER. A translucent panel over a flat colour is grey, not
//      glass -- there is nothing behind it to refract. This suite renders the
//      SAME scene twice (layer off / layer on) and requires a pixel difference
//      at the same coordinate: proof that content really is being seen through.
//   2. NO PANEL LANGUAGE. The islands were a fill and a 1 px outline: no top
//      specular rim, no drop shadow, no continuous corner. Each of those is
//      measured separately below (brightness step, shadow band, corner pixel).
//
// Plus the two properties that make the layer shippable rather than pretty:
// the appearance setting really reaches the renderer (and defaults ON), and the
// layer is rebuilt on a size/content change but NEVER on a scroll.
//
// Negative controls (CONTRACTS D3/D26/D27): the same source compiled with one
// rule broken -- the layer never painted, or the shadow never painted -- must
// go RED, and the verdict is printed at the FIRST failing check, armed on the
// injected branch having actually run. The clean build prints no verdict, so a
// mutant entry whose injection stopped matching turns red instead of green.
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
#include "../../Src/PrismCore/Appearance.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;

namespace {

using prism::ArchiveItem;
using prism::win::gallery::GalleryRenderer;

constexpr UINT32 kWidth = 1600;
constexpr UINT32 kHeight = 1000;

#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
// C121 MUTANT ACTIVE: empty-ghost
// NEGATIVE CONTROL (CONTRACTS D3): the layer is built and composited, but the
// PAGE is not drawn into it -- the source is empty. C121's assertion (1) -- "the
// rail's panel is in the source" -- MUST go red, which is what stops "fix the
// halo by emptying the layer" from looking like a success.
inline constexpr bool kBackdropMutation = false;
inline constexpr bool kShadowMutation = false;
constexpr const char* kMutationName = "empty-ghost";
#elif defined(PRISM_C77_MUTANT_NO_BACKDROP)
// C77 MUTANT ACTIVE: no-backdrop
// NEGATIVE CONTROL (CONTRACTS D3): the refraction layer is never painted, so
// assertion (1) -- "the backdrop differs from the flat colour" -- MUST go red.
inline constexpr bool kBackdropMutation = true;
inline constexpr bool kShadowMutation = false;
constexpr const char* kMutationName = "no-backdrop";
#elif defined(PRISM_C77_MUTANT_NO_SHADOW)
// C77 MUTANT ACTIVE: no-shadow
// NEGATIVE CONTROL (CONTRACTS D3): the panel paints no drop shadow, so
// assertion (3) -- "the band below the panel is darker" -- MUST go red.
inline constexpr bool kBackdropMutation = false;
inline constexpr bool kShadowMutation = true;
constexpr const char* kMutationName = "no-shadow";
#else
inline constexpr bool kBackdropMutation = false;
inline constexpr bool kShadowMutation = false;
constexpr const char* kMutationName = "";
#endif

#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
constexpr const char* kVerdict = "C77 MUTATION CAUGHT: empty-ghost";
#elif defined(PRISM_C77_MUTANT_NO_BACKDROP)
constexpr const char* kVerdict = "C77 MUTATION CAUGHT: no-backdrop";
#elif defined(PRISM_C77_MUTANT_NO_SHADOW)
constexpr const char* kVerdict = "C77 MUTATION CAUGHT: no-shadow";
#else
constexpr const char* kVerdict = "";
#endif

// The "injection really ran" predicate D27 wants: the mutant constant is
// compiled in AND the layer it disables was actually reached at least once.
int& MutationReached() {
  static int reached = 0;
  return reached;
}

#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
// C121: proof that the EMPTY-GHOST arm really ran, published by the harness from
// the renderer's own counter (the increment is compiled into that arm only).
int gEmptyGhostHits = 0;
#endif

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
  int Luminance(int x, int y) const {
    return (Channel(x, y, 16) + Channel(x, y, 8) + Channel(x, y, 0)) / 3;
  }
};

// One renderer, one target, many frames: the backdrop cache lives on the
// renderer, which is exactly what the rebuild-policy case measures.
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

  bool Frame_(const GalleryRenderer::Scene& scene, Frame* out, bool ghost = false) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
    gEmptyGhostHits += renderer.EmptyGhostMutantHits();
#endif
    // C121: the refraction SOURCE alone. It is opaque, so painting it as the
    // last operation of the frame turns the read-back into the ghost.
    if (ghost && !renderer.PaintRefractionSource(target.Get())) {
      target->EndDraw();
      return false;
    }
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

bool Render(const GalleryRenderer::Scene& scene, UINT32 width, UINT32 height, Frame* out) {
  Harness harness;
  return harness.Open(width, height) && harness.Frame_(scene, out);
}

int ColorDistance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

// ------------------------------------------------------------------ fixtures
ArchiveItem ImageItem(const char* path, std::uint32_t index, std::uint64_t size) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  return item;
}

std::vector<ArchiveItem> Catalog() {
  std::vector<ArchiveItem> items;
  items.push_back(ImageItem("Brandenburg_Gate/", 0, 0));
  items.back().isDir = true;
  const char* names[] = {"gate_front.png",  "gate_night.png", "quadriga.png",
                         "bode_morning.png", "pergamon.png",   "neues_dome.png",
                         "mural_01.png",    "mural_02.png",   "street_04.png",
                         "street_05.png",   "street_06.png",  "street_07.png"};
  for (int i = 0; i < 12; ++i) {
    char path[64];
    std::snprintf(path, sizeof(path), "Brandenburg_Gate/%s", names[i]);
    items.push_back(ImageItem(path, static_cast<std::uint32_t>(i + 1),
                              3'100'000u + 977u * static_cast<std::uint64_t>(i)));
  }
  return items;
}

// Bright, saturated 8x8 thumbs: the ghost of a photo is what a refraction layer
// has to be visible against.
void FillThumbs(const std::vector<ArchiveItem>& items, prism::gallery::ThumbStore* store) {
  const unsigned char tints[12][3] = {
      {230, 120, 60},  {40, 160, 210}, {200, 70, 140}, {90, 200, 120},
      {240, 190, 70},  {120, 90, 230}, {60, 200, 200}, {230, 100, 100},
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
  GalleryRenderer::Scene scene;

  Fixture() {
    items = Catalog();
    FillThumbs(items, &thumbs);
    viewport.SetViewportSize(static_cast<int>(kWidth), static_cast<int>(kHeight));
    viewport.SetScroll(420.0);
    scene.items = &items;
    scene.viewport = &viewport;
    scene.thumbs = &thumbs;
    scene.breadcrumb = L"Berlin_Trip_2026.zip  |  13 items  |  5 solid blocks";
    scene.quickLookOpen = true;
    scene.quickLookTitle = L"Brandenburg_Gate/street_07.png";
    scene.quickLookMeta = L"PNG  |  3.3 MB  |  solid block 3/5";
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

// ------------------------------------------------------------------ the cases

PRISM_TEST("C77-1 the backdrop refracts the PAGE -- and carries no item pixels") {
  // C121 RE-DEFINES THIS CASE, and the old form was WRONG, not merely stale.
  //
  // It used to assert "the same coordinate differs by > 8 over the content area,
  // on more than 20000 pixels" -- i.e. THE LAYER MUST REFRACT THE ITEMS. That is
  // the assertion C110 obeyed when it kept the decoded thumbnails in the
  // refraction source, and it is why a photo tile grew a coloured halo: a 1/8
  // sample of the photo, blurred 28 px, is a plate, not a glow (measured
  // (81,29,28) in the 13 px gap between two tiles, chroma 53 against 7 with the
  // layer off, spread 46-48 lp = 4.4x the gap).
  //
  // The audit's rule is the opposite one: a refraction source carries the PAGE
  // -- canvas, static bands, rail -- and never the archive (docs/design/C 5.1
  // R1). So this case now asserts BOTH halves, and both can go red:
  //   (1) THE SOURCE CARRIES A PAGE PANEL. Inside the rail's rectangle the
  //       ghost reads >= 8 levels away from the canvas at the same coordinate:
  //       proof that the layer is refracting a page surface (the rail's panel is
  //       +21/+24/+29 on the midnight canvas, and the 1/8 blur of a 220 px wide
  //       panel leaves the middle at full strength). THIS IS THE HALF THAT KEEPS
  //       "fix it by deleting the layer" RED: an empty source reads 0.
  //   (2) THE SOURCE CARRIES ZERO ITEM PIXELS. Inside a photo tile's rectangle
  //       the ghost IS the canvas, within 2 levels -- the ghost-level twin of
  //       the C121 suite's H1/H3 pixel assertions.
  //
  // The ghost is read through GalleryRenderer::PaintRefractionSource, the seam
  // that paints the 1/8 source alone (opaque, so it replaces the frame): asking
  // the layer what it is made of instead of inferring it from the chrome that
  // was painted over it.
  Fixture fixture;
  std::vector<ArchiveItem> catalog = fixture.items;
  prism::sidebar::Model model(&catalog);
  const prism::layout::Layout layout =
      prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                             /*sidebarVisible=*/true, /*listMode=*/false);
  prism::win::sidebar::Metrics metrics;
  metrics.width = static_cast<float>(prism::layout::kSidebarWidth);
  metrics.viewportTop = static_cast<float>(layout.actionIsland.bottom);
  metrics.viewportBottom = static_cast<float>(layout.statusBar.top);
  const prism::win::sidebar::Layout rail = prism::win::sidebar::Build(model, metrics);
  PRISM_CHECK(rail.panel.Width() > 100.0f);

  GalleryRenderer::Scene glass = fixture.scene;
  glass.layout = &layout;   // the SAME geometry the renderer would compute
  glass.sidebar = &rail;
  glass.backdropRefraction = true;
  glass.glassPanels = false;  // this case measures the SOURCE, not the panel language
  glass.theme = prism::theme::Variant::Midnight;

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame frame;
  PRISM_CHECK(harness.Frame_(glass, &frame));
  // CONTRACTS D3: the no-backdrop arm returns before the layer is ever built, so
  // "the layer was requested and never composited" is the proof it ran. Armed
  // before the first assertion the arm is expected to break.
  if (kBackdropMutation && harness.renderer.BackdropDraws() == 0) ++MutationReached();
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 1);
  PRISM_CHECK_EQ(harness.renderer.BackdropDraws(), 1);

  const prism::theme::Rgba canvasRgba =
      prism::chrome::CanvasPresentedColor(prism::theme::Variant::Midnight);
  const auto byteOf = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<UINT32>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  const UINT32 ghostCanvas = 0xFF000000u | (byteOf(canvasRgba.r) << 16) |
                             (byteOf(canvasRgba.g) << 8) | byteOf(canvasRgba.b);
  const auto maxChannelDelta = [](UINT32 a, UINT32 b) {
    int delta = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
      delta = (std::max)(delta, std::abs(static_cast<int>((a >> shift) & 0xFFu) -
                                         static_cast<int>((b >> shift) & 0xFFu)));
    }
    return delta;
  };

  Frame ghost;
  PRISM_CHECK(harness.Frame_(glass, &ghost, /*ghost=*/true));
  if (ghost.pixels.empty()) return;
  // The two pictures this case is about: the frame, and the source it was built
  // from. The second one is the evidence a reviewer needs -- the frame alone can
  // no longer show what the layer carries (C121 made it a page).
  if (!WritePng(L"c77-1-frame.png", frame)) std::puts("  WARN: frame PNG failed");
  if (!WritePng(L"c77-1-refraction-source.png", ghost)) std::puts("  WARN: source PNG failed");
  if (kBackdropMutation) ++MutationReached();

  // THE SOURCE IS A PAGE, NOT A CUT-OUT. Every sample of it is opaque: the
  // refraction source covers the frame, the way a page background does. Before
  // C121 it was a transparent field with the items floating in it -- and this
  // check is also what makes the empty-ghost arm honest, because a suite that
  // painted nothing would otherwise read the FRAME's own rail through the hole
  // and call it "the page is in the source".
  int transparentSamples = 0;
  for (int y = 4; y < static_cast<int>(ghost.height); y += 7) {
    for (int x = 4; x < static_cast<int>(ghost.width); x += 7) {
      if (static_cast<int>((ghost.At(x, y) >> 24) & 0xFFu) != 255) ++transparentSamples;
    }
  }
  std::printf("C77 backdrop source: transparent samples=%d\n", transparentSamples);
  PRISM_CHECK_EQ(transparentSamples, 0);

  // (1) the rail's panel is IN the source.
  double panelDelta = 0.0;
  int panelSamples = 0;
  int panelMin = 1000;
  for (int y = static_cast<int>(rail.panel.top) + 40;
       y < static_cast<int>(rail.panel.bottom) - 40; y += 3) {
    for (int x = static_cast<int>(rail.panel.left) + 40;
         x < static_cast<int>(rail.panel.right) - 40; x += 3) {
      if (!ghost.In(x, y)) continue;
      const int delta = maxChannelDelta(ghost.At(x, y), ghostCanvas);
      panelDelta += delta;
      panelMin = (std::min)(panelMin, delta);
      ++panelSamples;
    }
  }
  PRISM_CHECK(panelSamples > 2000);
  const double meanPanelDelta = panelSamples > 0 ? panelDelta / panelSamples : 0.0;
  std::printf("C77 backdrop source: rail panel (%d,%d)-(%d,%d) meanDelta=%.2f minDelta=%d "
              "samples=%d\n",
              static_cast<int>(rail.panel.left), static_cast<int>(rail.panel.top),
              static_cast<int>(rail.panel.right), static_cast<int>(rail.panel.bottom),
              meanPanelDelta, panelMin, panelSamples);
  PRISM_CHECK(meanPanelDelta >= 8.0);   // the page is IN the reflection
  PRISM_CHECK(panelMin >= 4);           // ... across the whole panel, not one lucky pixel

  // (2) a photo tile is NOT. The flow is WindowLayout's, so the probe cannot
  // drift off the tile it names.
  std::vector<prism::layout::FlowItem> flow;
  flow.reserve(fixture.items.size());
  for (const ArchiveItem& item : fixture.items) {
    prism::layout::FlowItem entry;
    entry.isDir = item.isDir;
    entry.kind = prism::QuickLookPlanner::KindByName(item.path);
    entry.thumbnailAvailable = fixture.thumbs.Contains(item.index) &&
                               prism::layout::ThumbnailCapable(entry.kind);
    flow.push_back(entry);
  }
  const std::vector<prism::layout::FlowCell> cells = prism::layout::VisibleFlowCells(
      layout, flow, static_cast<int>(fixture.viewport.Scroll()));
  prism::layout::Rect tile;
  for (const prism::layout::FlowCell& cell : cells) {
    if (cell.kind != prism::layout::CellKind::Tile) continue;
    if (cell.rect.top < layout.contentInner.top || cell.rect.bottom > layout.contentInner.bottom) {
      continue;
    }
    tile = cell.rect;
    break;
  }
  PRISM_CHECK(!tile.Empty());
  if (tile.Empty()) return;
  int worstItemDelta = 0;
  int itemSamples = 0;
  for (int y = tile.top + 50; y < tile.top + 150; ++y) {
    for (int x = tile.left + 20; x < tile.right - 20; ++x) {
      if (!ghost.In(x, y)) continue;
      worstItemDelta = (std::max)(worstItemDelta, maxChannelDelta(ghost.At(x, y), ghostCanvas));
      ++itemSamples;
    }
  }
  PRISM_CHECK(itemSamples > 2000);
  std::printf("C77 backdrop source: photo tile (%d,%d)-(%d,%d) worstDelta=%d samples=%d\n",
              tile.left, tile.top, tile.right, tile.bottom, worstItemDelta, itemSamples);
  // ... and the tile really holds a saturated photo in the frame, so a clean
  // ghost here cannot be a clean ghost over an empty tile.
  int frameChroma = 0;
  for (int y = tile.top + 60; y < tile.top + 100; y += 2) {
    for (int x = tile.left + 30; x < tile.right - 30; x += 2) {
      if (!frame.In(x, y)) continue;
      const int r = static_cast<int>((frame.At(x, y) >> 16) & 0xFFu);
      const int g = static_cast<int>((frame.At(x, y) >> 8) & 0xFFu);
      const int b = static_cast<int>(frame.At(x, y) & 0xFFu);
      frameChroma = (std::max)(frameChroma,
                               (std::max)(r, (std::max)(g, b)) - (std::min)(r, (std::min)(g, b)));
    }
  }
  std::printf("C77 backdrop source: the same tile in the FRAME has chroma=%d\n", frameChroma);
  PRISM_CHECK(frameChroma >= 60);
  PRISM_CHECK(worstItemDelta <= 2);   // ZERO item pixels in the reflection
}

PRISM_TEST("C77-2 the glass panel has a rim, a shadow and a continuous corner") {
  Fixture fixture;
  GalleryRenderer::Scene glass = fixture.scene;
  glass.backdropRefraction = true;
  glass.glassPanels = true;
  glass.theme = prism::theme::Variant::Midnight;
  GalleryRenderer::Scene bare = glass;
  bare.glassPanels = false;  // the reference: same content, no panel at all

  Frame with;
  Frame without;
  PRISM_CHECK(Render(glass, kWidth, kHeight, &with));
  PRISM_CHECK(Render(bare, kWidth, kHeight, &without));
  if (with.pixels.empty() || without.pixels.empty()) return;

  const prism::layout::Layout layout =
      prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight), false, false);
  const prism::layout::Rect card = prism::layout::QuickLookCardRect(layout);
  PRISM_CHECK(!card.Empty());
  if (card.Empty()) return;
  const int cx = (card.left + card.right) / 2;
  const int top = card.top;
  const int bottom = card.bottom;
  std::printf("C77 panel: card=(%d,%d)-(%d,%d) window=%ux%u\n", card.left, card.top,
              card.right, card.bottom, kWidth, kHeight);

  // (2) the top 1 device pixel is the specular rim: >= 12 levels brighter than
  //     the panel body 8 px below it.
  const int rimLuma = with.Luminance(cx, top);
  const int bodyLuma = with.Luminance(cx, top + 8);
  std::printf("C77 panel: rimLuma=%d bodyLuma=%d delta=%d\n", rimLuma, bodyLuma,
              rimLuma - bodyLuma);
  PRISM_CHECK(rimLuma - bodyLuma >= 12);

  // (3) the drop shadow: 6..10 px BELOW the panel the frame is darker than the
  //     same pixels without the panel, by 3..10 levels. Outside that window it
  //     must be lighter than that (the shadow has to fall off, not flood).
  double bandSum = 0.0;
  double farSum = 0.0;
  int bandCount = 0;
  int farCount = 0;
  // The band is measured ABOVE the top edge: the panel is offset +6 px down, so
  // its halo reaches ~18 px up, and that is where the backdrop still carries the
  // refracted content -- a shadow measured on the bare canvas at luma 13 cannot
  // be 3 levels dark without being a black halo, and the card's own design
  // number is alpha .18.
  //
  // C110: THE FLOOR IS NOW MATERIAL-INDEPENDENT (see the ratio check below).
  // This band sits over a PHOTO tile's letterboxed fill area (the decoded 8x8
  // thumb covers only the middle 131 px of the 190 px plate, so y 104..133 is the
  // tile's 6 % white fill). In the 1/8 refraction source that fill is a solid
  // mini rectangle, and -- because it is drawn over a TRANSPARENT target, not
  // over the canvas -- it contributed ~+5 levels to this band. C110 took the
  // tile chrome out of the grid's ghost (that fill WAS the user's "large grey
  // smear": the tile rectangle moved 34.5 levels between layer OFF and ON), so
  // the band's backdrop fell from 38.4 to 28.1 and the same, unchanged shadow
  // (effective alpha .105 both before and after -- measured) now reads 2.96
  // instead of 4.17. An absolute floor therefore measures the BACKDROP, not the
  // shadow; the shadow is asserted as a ratio to the band it darkens.
  double referenceSum = 0.0;  // C110: the band's own backdrop, for the ratio
  for (int y = top - 10; y < top - 6; ++y) {
    for (int x = card.left + 60; x < card.right - 60; ++x) {
      if (!with.In(x, y) || !without.In(x, y)) continue;
      bandSum += without.Luminance(x, y) - with.Luminance(x, y);
      referenceSum += without.Luminance(x, y);
      ++bandCount;
    }
  }
  for (int y = top - 44; y < top - 40; ++y) {
    for (int x = card.left + 60; x < card.right - 60; ++x) {
      if (!with.In(x, y) || !without.In(x, y)) continue;
      farSum += without.Luminance(x, y) - with.Luminance(x, y);
      ++farCount;
    }
  }
  PRISM_CHECK(bandCount > 1000);
  const double bandDarkness = bandCount > 0 ? bandSum / bandCount : 0.0;
  const double farDarkness = farCount > 0 ? farSum / farCount : 0.0;
  const double referenceBand = bandCount > 0 ? referenceSum / bandCount : 0.0;
  // C110: the shadow's EFFECTIVE ALPHA over the band it lands on. This is the
  // material-independent form of "the panel casts a shadow": the absolute levels
  // move with whatever the backdrop is carrying, the ratio does not. Measured
  // .109 with the tile ghost in the source (band darkening 4.17 over a 38.37
  // backdrop) and .106 with it taken out (2.96 over 28.06): the shadow itself
  // did not change, which is exactly what this case is supposed to pin. A
  // missing shadow reads 0.
  const double shadowAlpha = referenceBand > 1.0 ? bandDarkness / referenceBand : 0.0;
  std::printf("C77 panel: shadowBand=%.2f referenceBand=%.2f shadowAlpha=%.3f farBand=%.2f "
              "samples=%d bgLuma=%d\n",
              bandDarkness, referenceBand, shadowAlpha, farDarkness, bandCount,
              without.Luminance(cx, bottom + 8));
  // The upper bound is a FLOOD guard: a shadow that blackens the band is as
  // wrong as one that is absent.
  PRISM_CHECK(bandDarkness <= 10.0);
  // The shadow is alpha .18 at its peak and this band sits ~6-10 px away from
  // the edge, so the visible alpha is ~.10. The window is wide enough for the
  // theme and the blur, narrow enough that a missing or a flooded shadow fails.
  PRISM_CHECK(shadowAlpha >= 0.06);
  PRISM_CHECK(shadowAlpha <= 0.30);
  // The shadow is a gradient: 40 px out it is gone.
  PRISM_CHECK(std::abs(farDarkness) < 1.5);
  if (kShadowMutation) ++MutationReached();

  // (4) the corner: a squircle radius 14 leaves the four corner pixels
  //     untouched, so they equal the same pixels without the panel -- and, on a
  //     flat backdrop, the exact canvas colour.
  // The CORNER pixels themselves. A squircle bulges toward its corner (that is
  // the point of the exponent), so the pixel at +1 is still inside the
  // antialiasing ramp; the corner pixel is the one that must be untouched.
  const int cornerX[4] = {card.left, card.right - 1, card.left, card.right - 1};
  const int cornerY[4] = {top, top, bottom - 1, bottom - 1};
  int paintedCorners = 0;
  for (int i = 0; i < 4; ++i) {
    if (!with.In(cornerX[i], cornerY[i]) || !without.In(cornerX[i], cornerY[i])) continue;
    if (with.At(cornerX[i], cornerY[i]) != without.At(cornerX[i], cornerY[i])) ++paintedCorners;
    std::printf("  corner[%d] (%d,%d) with=%08X without=%08X dist=%d\n", i, cornerX[i],
                cornerY[i], with.At(cornerX[i], cornerY[i]), without.At(cornerX[i], cornerY[i]),
                ColorDistance(with.At(cornerX[i], cornerY[i]), without.At(cornerX[i], cornerY[i])));
  }
  std::printf("C77 panel: paintedCorners=%d\n", paintedCorners);
  PRISM_CHECK_EQ(paintedCorners, 0);

  // ... and the fill really is a rounded shape: the edge midpoint 1 px inside
  // the left border is the panel, the corner is not.
  PRISM_CHECK(with.At(card.left + 1, (top + bottom) / 2) !=
              without.At(card.left + 1, (top + bottom) / 2));

  // The literal "corner == background colour" check: an EMPTY catalog, so the
  // only thing behind the card is the canvas itself (kBackdrop).
  GalleryRenderer::Scene empty;
  empty.glassPanels = true;
  empty.theme = prism::theme::Variant::Midnight;
  empty.quickLookOpen = true;
  empty.breadcrumb = L"empty.7z";
  Frame flatFrame;
  PRISM_CHECK(Render(empty, kWidth, kHeight, &flatFrame));
  if (!flatFrame.pixels.empty()) {
    // 32bppPBGRA read as a UINT32 is B,G,R,A in memory order. C91: the canvas is
    // the THEME's composite now, so the constant is built from the same entry the
    // renderer clears with -- not a second copy of #0B0E13.
    const prism::theme::Rgba canvasRgba =
        prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
    const auto byteOf = [](float v) {
      const double scaled = static_cast<double>(v) * 255.0 + 0.5;
      return static_cast<UINT32>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
    };
    const UINT32 canvas = 0xFF000000u | (byteOf(canvasRgba.r) << 16) |
                          (byteOf(canvasRgba.g) << 8) | byteOf(canvasRgba.b);
    int notBackground = 0;
    for (int i = 0; i < 4; ++i) {
      if (!flatFrame.In(cornerX[i], cornerY[i])) continue;
      if (ColorDistance(flatFrame.At(cornerX[i], cornerY[i]), canvas) > 1) ++notBackground;
      std::printf("  flatCorner[%d] (%d,%d) = %08X (canvas %08X)\n", i, cornerX[i], cornerY[i],
                  flatFrame.At(cornerX[i], cornerY[i]), canvas);
    }
    std::printf("C77 panel: cornersOffCanvas=%d\n", notBackground);
    PRISM_CHECK_EQ(notBackground, 0);
  }
}

PRISM_TEST("C77-3 the backdrop is rebuilt on size/content change, never on scroll") {
  Fixture fixture;
  GalleryRenderer::Scene scene = fixture.scene;
  scene.backdropRefraction = true;
  scene.glassPanels = true;

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;

  // Six frames, scroll only. The layer is a still of the content: it must be
  // built once and re-used, or every wheel notch would cost a full-window blur.
  for (int i = 0; i < 6; ++i) {
    fixture.viewport.SetScroll(100.0 + 40.0 * i);
    PRISM_CHECK(harness.Frame_(scene, nullptr));
  }
  const int afterScroll = harness.renderer.BackdropRebuilds();
  const int drawsAfterScroll = harness.renderer.BackdropDraws();
  std::printf("C77 perf: scrollFrames=6 rebuilds=%d draws=%d\n", afterScroll, drawsAfterScroll);
  PRISM_CHECK_EQ(afterScroll, 1);
  PRISM_CHECK_EQ(drawsAfterScroll, 6);

  // The same scene, one pixel wider: a size change DOES rebuild.
  scene.contentRevision = 1;
  PRISM_CHECK(harness.Frame_(scene, nullptr));
  const int afterRevision = harness.renderer.BackdropRebuilds();
  std::printf("C77 perf: afterContentRevision rebuilds=%d\n", afterRevision);
  PRISM_CHECK_EQ(afterRevision, 2);

  // A different catalog (a rename is enough) rebuilds without any bump.
  Fixture other;
  other.items[1].path = "Brandenburg_Gate/gate_front_renamed.png";
  GalleryRenderer::Scene renamed = scene;
  renamed.items = &other.items;
  renamed.viewport = &other.viewport;
  renamed.thumbs = &other.thumbs;
  PRISM_CHECK(harness.Frame_(renamed, nullptr));
  std::printf("C77 perf: afterRename rebuilds=%d\n", harness.renderer.BackdropRebuilds());
  PRISM_CHECK_EQ(harness.renderer.BackdropRebuilds(), 3);

  // The card's inequality, stated as a bound: rebuilds <= changes + 1 (the
  // initial build), and far below the number of frames drawn.
  PRISM_CHECK(harness.renderer.BackdropRebuilds() <= 3 + 1);
  PRISM_CHECK(harness.renderer.BackdropRebuilds() < harness.renderer.BackdropDraws());

  // The cached bitmap is 1/8 resolution, not a second full-size surface.
  const int pixels = harness.renderer.BackdropBitmapPixels();
  const int fullPixels = static_cast<int>(kWidth * kHeight);
  std::printf("C77 perf: backdropPixels=%d (%.2f%% of the window)\n", pixels,
              100.0 * pixels / fullPixels);
  PRISM_CHECK(pixels > 0);
  PRISM_CHECK(pixels * 40 < fullPixels);  // <= 1/40 of a full-size surface
}

PRISM_TEST("C77-4 the appearance setting reaches the renderer and defaults ON") {
  // A fresh document: the key is absent, so the answer is the schema default.
  prism::settings::SettingsStore fresh;
  fresh.Load(R"({"schemaVersion": 3})");
  const prism::appearance::Applied applied = prism::appearance::Apply(fresh);
  std::printf("C77 setting: fresh default backdrop=%d\n",
              applied.backdropRefraction ? 1 : 0);
  PRISM_CHECK(applied.backdropRefraction);
  PRISM_CHECK(prism::theme::BackdropRefractionEnabled());

  // ... and the stored value is what the renderer asks for.
  prism::settings::SettingsStore off;
  off.Load(R"({"schemaVersion": 3, "appearance": {"backdropRefraction": false}})");
  const prism::appearance::Applied disabled = prism::appearance::Apply(off);
  PRISM_CHECK(!disabled.backdropRefraction);
  PRISM_CHECK(!prism::theme::BackdropRefractionEnabled());

  prism::settings::SettingsStore on;
  on.Load(R"({"schemaVersion": 3, "appearance": {"backdropRefraction": true}})");
  PRISM_CHECK(prism::appearance::Apply(on).backdropRefraction);

  // Reset so a later case (and the process) sees the shipping default again.
  prism::appearance::Reset();
  PRISM_CHECK(prism::theme::BackdropRefractionEnabled());

  // The schema row exists, is a Bool, and its default is "true" -- the two
  // spellings (SettingsStore's table and Appearance.hpp) must agree.
  const prism::settings::SettingSpec* spec =
      prism::settings::FindSettingSpec(prism::appearance::kBackdropKey);
  PRISM_CHECK(spec != nullptr);
  if (spec != nullptr) {
    PRISM_CHECK(spec->type == prism::settings::SettingType::Bool);
    PRISM_CHECK(std::string(spec->defaultText) == "true");
    PRISM_CHECK(prism::appearance::kDefaultBackdropRefraction);
  }

  // The three C77 tokens exist in ALL THREE themes (the card's hard rule), and
  // the fill window is the card's .45-.55.
  const prism::theme::Variant variants[3] = {prism::theme::Variant::Daylight,
                                             prism::theme::Variant::Midnight,
                                             prism::theme::Variant::HighContrast};
  for (const prism::theme::Variant variant : variants) {
    const prism::theme::TokenValue ghost = prism::theme::Lookup(
        prism::theme::TokenId::SurfaceBackdropGhost, variant);
    const prism::theme::TokenValue veil =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceBackdropVeil, variant);
    const prism::theme::TokenValue shadow =
        prism::theme::Lookup(prism::theme::TokenId::ShadowFloatingPanel, variant);
    PRISM_CHECK(ghost.known);
    PRISM_CHECK(veil.known);
    PRISM_CHECK(shadow.known);
    PRISM_CHECK(!ghost.fellBack);
    PRISM_CHECK(!veil.fellBack);
    PRISM_CHECK(!shadow.fellBack);
    PRISM_CHECK(ghost.color.a > 0.0f);
    PRISM_CHECK(veil.color.a > 0.0f);
    PRISM_CHECK(shadow.color.a > 0.0f);
    // The PAINTED alpha, not the raw token: Daylight's island token is .58 and
    // HighContrast's is 1.0 on purpose, and the helper clamps both into the
    // card's .45-.55 window. Asserting the token itself would have been an
    // assertion about a number no pixel ever uses.
    const float painted =
        prism::win::glass::PanelPainter::ClampFillAlpha(
            prism::theme::EffectiveGlassSurface(variant).a);
    PRISM_CHECK(painted >= prism::win::glass::kFillAlphaMin);
    PRISM_CHECK(painted <= prism::win::glass::kFillAlphaMax);
  }
  // The clamp IS the card's window: a token that asked for .9 still paints .55.
  PRISM_CHECK_EQ(prism::win::glass::PanelPainter::ClampFillAlpha(0.9f),
                 prism::win::glass::kFillAlphaMax);
  PRISM_CHECK_EQ(prism::win::glass::PanelPainter::ClampFillAlpha(0.1f),
                 prism::win::glass::kFillAlphaMin);
}

PRISM_TEST("C77-5 the panel helper paints every part it is asked for") {
  Fixture fixture;
  GalleryRenderer::Scene scene = fixture.scene;
  scene.backdropRefraction = true;
  scene.glassPanels = true;
  scene.theme = prism::theme::Variant::Midnight;

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  harness.renderer.ResetGlassStats();
  PRISM_CHECK(harness.Frame_(scene, nullptr));
  const prism::win::glass::Stats stats = harness.renderer.GlassStats();
  std::printf("C77 helper: panels=%d fills=%d rims=%d shadows=%d\n", stats.panels, stats.fills,
              stats.rims, stats.shadows);
  // breadcrumb island (3) + Quick Look card (3) are full panels; the action
  // island contributes a shadow and a rim but NOT a fill (C64 owns that one),
  // and the rail contributes a shadow.
  // Exactly two panels in this fixture (the breadcrumb island and the Quick Look
  // card). The action island and the rail are absent, and their FILLS are not
  // the helper's business anyway -- that is what the exact equality says.
  PRISM_CHECK_EQ(stats.panels, 2);
  PRISM_CHECK_EQ(stats.fills, 2);
  PRISM_CHECK_EQ(stats.rims, 2);
  PRISM_CHECK_EQ(stats.shadows, 2);
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutationName[0] != '\0') {
    std::printf("C77 MUTANT ACTIVE: %s\n", kMutationName);
    // D27: the verdict is printed at the FIRST failing check, and only if the
    // injected branch actually ran -- red for an unrelated reason stays red.
    // C121's empty-ghost arm is armed on the renderer's counter (it drew no page
    // into the source); C77's own arms on the test-side proof that the layer
    // call was reached.
#if defined(PRISM_C121_MUTANT_EMPTY_GHOST)
    prism::test::SetMutationVerdict(kVerdict, []() { return gEmptyGhostHits > 0; });
#else
    prism::test::SetMutationVerdict(kVerdict, []() { return MutationReached() > 0; });
#endif
  } else {
    std::puts("C77 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the glass cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the glass cannot be verified");
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
    std::puts("C77 GLASS CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C77 GLASS CHECKS PASSED");
  return 0;
}
