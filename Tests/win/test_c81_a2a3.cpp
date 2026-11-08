// PrismArchive -- card C81: A2 (the list header rendered as a selected row)
// and A3 (the compression ratio contradicting itself in three places).
//
// A2's pixel contract (docs/UI-AUDIT.md A2), measured on RENDERED FRAMES:
//   1. no accent-dominant ("blue-led") pixel anywhere in the header band;
//   2. the header's background IS the canvas (flat, no panel, no ghost);
//   3. a 1 px separator sits on the header's last row (>= 3 levels vs canvas).
//      The ghost half of A2 is asserted differentially: the band renders
//      IDENTICALLY with the C77 backdrop layer ON and OFF (the blurred still
//      can no longer bleed header text under the crisp labels).
//
// A3's consistency contract: for the SAME byte pair the surfaces that carry a
// ratio -- the list cell and the sidebar info card -- must carry the SAME string
// from prism::CompressionRatioView. The painters render these strings VERBATIM
// through DrawText (no re-formatting on the way to pixels), so the primitive
// read-back below IS the rendered text byte for byte.
//
// C128 moved the ratio OFF the status band (the band is three numbers: the
// current folder's files/folders and the archive's packed size), so the third
// surface this card audited no longer exists and the arm that made the band lie
// about the ratio (PRISM_C81_MUTANT_STATUS_HARDCODED_UNKNOWN) retired with it.
// The band's own negative control is C128's (mutation.c128.old_status), which
// restores the six-slot band -- the shape, not a string.
//
// Negative controls (CONTRACTS D3 + D26 + D27; same source, compile-time arms):
//   PRISM_C81_MUTANT_HEADER_SELECTED         the header gets the selected-row
//                                            vocabulary -> A2 assertions red
// Each mutant is registered under PASS_REGULAR_EXPRESSION (never WILL_FAIL,
// D26); the CAUGHT marker prints at the FIRST failed assertion (D27).
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
#include "../../Src/PrismCore/CompressionRatio.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

#if defined(PRISM_C81_MUTANT_HEADER_SELECTED)
inline constexpr const char* kMutantBanner = "C81 MUTANT ACTIVE: header_selected";
inline constexpr const char* kMutantMarker = "C81 MUTATION CAUGHT: header_selected";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

namespace {

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;

using Microsoft::WRL::ComPtr;
using prism::list::SortItem;

IWICImagingFactory* gWic = nullptr;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) &&
           y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
};

struct Box {
  int left = 0, top = 0, right = 0, bottom = 0;
};

// The canvas colour as the render target quantizes it to bytes. C91: it is the
// theme's canvas composite, not the #0B0E13 literal the renderer used to clear
// to. "presented" asks for the pixel a resting area shows when the C77 backdrop
// layer is ON (the canvas is washed by the veil then) -- the layer is part of
// the canvas recipe, so a frame that turns it on moves every canvas pixel.
UINT32 CanvasPixel(bool presented) {
  const prism::theme::Rgba token =
      presented ? prism::chrome::CanvasPresentedColor(prism::theme::Variant::Midnight)
                : prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  const D2D1_COLOR_F c = D2D1::ColorF(token.r, token.g, token.b, token.a);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(c.a) << 24) | (q(c.r) << 16) | (q(c.g) << 8) | q(c.b);
}

bool NearColor(UINT32 a, UINT32 b, int tolerance) {
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) -
                  static_cast<int>((b >> shift) & 0xFFu);
    if (d < -tolerance || d > tolerance) return false;
  }
  return true;
}

// "Blue-led": the accent token (#4F8CFF family) dominates the blue channel.
bool AccentDominant(UINT32 color) {
  const int r = static_cast<int>((color >> 16) & 0xFFu);
  const int g = static_cast<int>((color >> 8) & 0xFFu);
  const int b = static_cast<int>(color & 0xFFu);
  return b - (std::max)(r, g) > 30;
}

struct Render {
  bool ok = false;
  Frame frame;
};

SortItem File(const char* name, std::uint64_t size, std::uint64_t packed) {
  SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  return item;
}

prism::win::gallery::GalleryRenderer::Scene ListScene(
    std::vector<SortItem>* rows, const prism::layout::StatusInfo& status) {
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.listItems = rows;
  scene.status = status;
  scene.breadcrumb = L"one.7z";
  return scene;
}

bool RenderScene(const prism::win::gallery::GalleryRenderer::Scene& scene, Render* out) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand,
                                bitmap.ReleaseAndGetAddressOf()))) {
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
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
    return false;
  }
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
  out->ok = true;
  return true;
}

// The layout the renderer itself computes for this scene shape (no rail: the
// test scenes carry no sidebar), so every rectangle below is THE rectangle.
prism::layout::Layout TestLayout() {
  return prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                /*railColumn=*/false, /*listMode=*/true);
}

// The floating breadcrumb pill (C77 glass) hangs ABOVE the band and its drop
// shadow falls onto the band's first rows: measured on this exact frame the
// shadow's ink reaches +5 rows and +13 px past the pill (band rows 0..4,
// x 16..589 against a pill of 16..576). Those pixels are CHROME, not the
// header surface A2 audits. The ghost half of A2 is asserted differentially
// between two SAME-CHROME frames (glass panels both, backdrop layer toggled),
// so this exclusion cannot hide a backdrop regression.
prism::layout::Rect ChromeShadowBox(const prism::layout::Layout& layout,
                                    const prism::layout::Rect& header) {
  const prism::layout::Rect pill = prism::layout::NavPillRect(layout);
  constexpr int kRowReach = 8;
  constexpr int kXReach = 20;
  prism::layout::Rect box;
  box.left = (std::max)(header.left, pill.left - 2);
  box.right = (std::min)(header.right, pill.right + kXReach);
  box.top = header.top;
  box.bottom = (std::min)(header.bottom, header.top + kRowReach);
  return box;
}

bool InChrome(const prism::layout::Rect& box, int x, int y) {
  return x >= box.left && x < box.right && y >= box.top && y < box.bottom;
}

// A2, assertions 1-3, on one frame. chrome == nullptr: no floating chrome in
// the frame, the whole band is audited strictly.
void AssertHeaderPixels(const Render& painted, const char* tag,
                        const prism::layout::Rect* chrome, bool presented) {
  const prism::layout::Layout layout = TestLayout();
  const prism::layout::Rect header = prism::layout::ListHeaderRect(layout);
  const UINT32 canvas = CanvasPixel(presented);

  // 1. no accent-dominant pixel in the band (no ring, no accent panel). The
  //    chrome shadow is neutral grey, so the full band stays under this one.
  int accentPixels = 0;
  for (int y = header.top; y < header.bottom; ++y) {
    for (int x = header.left; x < header.right; ++x) {
      if (AccentDominant(painted.frame.At(x, y))) ++accentPixels;
    }
  }
  std::printf("  [c81] %s accent-led pixels in header=%d\n", tag, accentPixels);
  PRISM_CHECK_EQ(accentPixels, 0);

  // 2. the band's background IS the canvas: two glyph-free strips (above the
  //    labels and between them and the rule), across the full width minus the
  //    floating chrome's shadow box when one is present.
  int offCanvas = 0;
  int sampled = 0;
  const int strips[2] = {header.top + 1, header.top + 2};
  for (int y : strips) {
    for (int x = header.left + 1; x < header.right - 1; ++x) {
      if (chrome != nullptr && InChrome(*chrome, x, y)) continue;
      ++sampled;
      if (!NearColor(painted.frame.At(x, y), canvas, 1)) ++offCanvas;
    }
  }
  std::printf("  [c81] %s off-canvas bg samples=%d/%d\n", tag, offCanvas, sampled);
  PRISM_CHECK_EQ(offCanvas, 0);

  // 3. the 1 px separator on the header's LAST row: >= 3 levels from the
  //    canvas on most of the row; the row just above it stays bare canvas.
  int rulePixels = 0;
  int ruleRun = 0;
  const int ruleY = header.bottom - 1;
  for (int x = header.left; x < header.right; ++x) {
    UINT32 c = painted.frame.At(x, ruleY);
    int maxDelta = 0;
    for (int shift = 0; shift <= 16; shift += 8) {
      maxDelta = (std::max)(maxDelta,
                            std::abs(static_cast<int>((c >> shift) & 0xFFu) -
                                     static_cast<int>((canvas >> shift) & 0xFFu)));
    }
    if (maxDelta >= 3) ++rulePixels;
  }
  const int width = header.right - header.left;
  int aboveRule = 0;
  for (int x = header.left; x < header.right; ++x) {
    if (!NearColor(painted.frame.At(x, header.bottom - 2), canvas, 1)) ++aboveRule;
  }
  ruleRun = rulePixels;
  std::printf("  [c81] %s separator run=%d/%d above-rule ink=%d\n", tag, ruleRun, width,
              aboveRule);
  PRISM_CHECK(ruleRun >= (width * 4) / 5);
  PRISM_CHECK_EQ(aboveRule, 0);
}

// A2's ghost half: the backdrop layer must not change the header band beyond
// the host's OWN run-to-run rasterization noise. The floor is MEASURED, never
// assumed: the same glass scene rendered twice through two fresh renderer
// instances bounds what this environment's ASan/font-cache stack may change
// between two identical frames (the C79 standard). On a deterministic host
// the floor is 0 and the original byte-identical standard applies verbatim --
// a relative criterion, not a relaxation.
// The mean per-channel change between two frames over one rectangle: the
// magnitude counterpart of BandDifferingPixels, for pairs where the whole area
// legitimately moves (the C91 veil) and a pixel COUNT cannot discriminate.
double MeanChannelDelta(const Render& a, const Render& b, const prism::layout::Rect& rect) {
  double sum = 0.0;
  int count = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      if (!a.frame.In(x, y) || !b.frame.In(x, y)) continue;
      const UINT32 pa = a.frame.At(x, y);
      const UINT32 pb = b.frame.At(x, y);
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs(static_cast<int>((pa >> shift) & 0xFFu) -
                        static_cast<int>((pb >> shift) & 0xFFu));
      }
      ++count;
    }
  }
  return count == 0 ? 0.0 : sum / (3.0 * static_cast<double>(count));
}

int BandDifferingPixels(const Render& a, const Render& b) {
  const prism::layout::Layout layout = TestLayout();
  const prism::layout::Rect header = prism::layout::ListHeaderRect(layout);
  int differing = 0;
  for (int y = header.top; y < header.bottom; ++y) {
    for (int x = header.left; x < header.right; ++x) {
      if (a.frame.At(x, y) != b.frame.At(x, y)) ++differing;
    }
  }
  return differing;
}

// The audit's replica: one 15 B file packed into 19 bytes.
std::vector<SortItem> AuditRows() {
  return {File("one.txt", 15, 19)};
}

const std::string kGrowToken = prism::CompressionRatioView(15, 19);  // ">100% (126.7%)"

// The band's facts. C128: three numbers (the ratio is the CARD's now).
const prism::layout::StatusInfo GrowStatus() {
  prism::layout::StatusInfo info;
  info.fileCount = 1;
  info.folderCount = 0;
  info.archiveBytes = 19;
  return info;
}

// The sidebar info card's RATIO VALUE, as the view builds it.
//
// C105 (plan 3.2): the card is a label/value stack, so the ratio is no longer
// one formatted line ("RATIO >100% (126.7%)") but a "RATIO" label with the
// canonical string as the value directly under it. This helper reads that
// value, which is a STRICTER check than the old one: the card's text must now
// BE the canonical token, with no prefix to hide a mismatch behind.
std::string CardRatioLine(prism::sidebar::Model* model) {
  // C128: the card is folded by default; this reads the EXPANDED card's RATIO
  // value (a StatusText under the "RATIO" label), so it asks for that state.
  prism::win::sidebar::Metrics metrics;
  metrics.infoCardExpanded = true;
  const prism::win::sidebar::Layout card = prism::win::sidebar::Build(*model, metrics);
  for (const prism::win::sidebar::Command& label : card.commands) {
    if (label.kind != prism::win::sidebar::PrimKind::StatusLabel) continue;
    if (label.text != "RATIO") continue;
    for (const prism::win::sidebar::Command& value : card.commands) {
      if (value.kind != prism::win::sidebar::PrimKind::StatusText) continue;
      if (value.box.top >= label.box.bottom - 0.01f &&
          std::fabs(value.box.left - label.box.left) < 0.01f) {
        return value.text;
      }
    }
  }
  return {};
}

// C128: the band has NO ratio slot any more (the ratio lives on the info card).
// The helper stays so "the band carries no ratio" is an assertion with a name
// instead of a loop a reader has to re-argue.
const prism::layout::StatusSegment* RatioSegment(
    const std::vector<prism::layout::StatusSegment>& segments) {
  for (const auto& segment : segments) {
    if (segment.id == prism::layout::StatusId::Ratio) return &segment;
  }
  return nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// A2: the header is flat text + a 1 px rule
// ---------------------------------------------------------------------------
PRISM_TEST("c81.a2.header_flat_on_canvas") {
  std::vector<SortItem> rows = AuditRows();
  Render plain;
  if (!PRISM_CHECK(RenderScene(ListScene(&rows, GrowStatus()), &plain))) return;
  AssertHeaderPixels(plain, "plain", nullptr, /*presented=*/false);
}

PRISM_TEST("c81.a2.header_with_glass_backdrop") {
  std::vector<SortItem> rows = AuditRows();
  // The differential pair holds the CHROME constant (glass panels on in both)
  // and toggles ONLY the backdrop layer, so any band pixel that changes is the
  // blurred still bleeding under the header -- the audit's ghost -- and
  // nothing else.
  prism::win::gallery::GalleryRenderer::Scene chromeScene = ListScene(&rows, GrowStatus());
  chromeScene.glassPanels = true;
  Render chromeOnly;
  if (!PRISM_CHECK(RenderScene(chromeScene, &chromeOnly))) return;

  prism::win::gallery::GalleryRenderer::Scene glassScene = chromeScene;
  glassScene.backdropRefraction = true;
  Render glass;
  if (!PRISM_CHECK(RenderScene(glassScene, &glass))) return;

  const prism::layout::Layout layout = TestLayout();
  const prism::layout::Rect chromeBox =
      ChromeShadowBox(layout, prism::layout::ListHeaderRect(layout));
  AssertHeaderPixels(glass, "glass", &chromeBox, /*presented=*/true);

  Render glassAgain;
  if (!PRISM_CHECK(RenderScene(glassScene, &glassAgain))) return;
  const int noise = BandDifferingPixels(glass, glassAgain);
  const int changed = BandDifferingPixels(glass, chromeOnly);
  // C91: toggling the refraction layer also toggles the VEIL, which is part of
  // the canvas recipe -- so the whole band now moves by a couple of levels and a
  // pixel COUNT saturates. The audit's actual question is "does the blurred
  // still bleed UNDER the header", i.e. "does the band move MORE than bare
  // canvas does". So the comparison is the MEAN channel delta of the band
  // against the same-sized strip of empty canvas below it, in the same pair of
  // frames.
  const prism::layout::Rect header = prism::layout::ListHeaderRect(TestLayout());
  prism::layout::Rect reference = header;
  reference.top = header.bottom + 200;
  reference.bottom = reference.top + (header.bottom - header.top);
  const double bandDelta = MeanChannelDelta(glass, chromeOnly, header);
  const double refDelta = MeanChannelDelta(glass, chromeOnly, reference);
  std::printf("  [c81] header band: backdrop changed=%d px, same-scene noise floor=%d px, "
              "bandDelta=%.2f canvasDelta=%.2f\n",
              changed, noise, bandDelta, refDelta);
  PRISM_CHECK(bandDelta <= refDelta + 1.0);
}

// ---------------------------------------------------------------------------
// A3: one string, three surfaces (primitive read-back == rendered text)
// ---------------------------------------------------------------------------
PRISM_TEST("c81.a3.growth_consistency") {
  // 1. the canonical view itself carries the honest direction AND the value.
  PRISM_CHECK_EQ(kGrowToken, std::string(">100% (126.7%)"));
  PRISM_CHECK(kGrowToken.find(">100%") != std::string::npos);

  // 2. the list cell.
  std::vector<SortItem> rows = AuditRows();
  const std::string cell = prism::list::RatioCellText(rows[0]);
  PRISM_CHECK_EQ(cell, kGrowToken);

  // 3. the info card (its totals ARE the single file's bytes here).
  std::vector<prism::ArchiveItem> catalog;
  {
    prism::ArchiveItem item;
    item.path = "one.txt";
    item.index = 0;
    item.size = 15;
    item.packSize = 19;
    catalog.push_back(item);
  }
  prism::sidebar::Model model(&catalog);
  const prism::sidebar::Status sidebarStatus = model.ComputeStatus();
  PRISM_CHECK_EQ(sidebarStatus.ratioView, kGrowToken);
  const std::string cardLine = CardRatioLine(&model);
  PRISM_CHECK_EQ(cardLine, kGrowToken);

  // 4. the status band (C128): the ratio is NOT on the band any more. It is the
  //    info card's, and only the card's -- the band answers "what am I looking
  //    at" with three numbers (files / folders / archive size). The old
  //    assertion here (the band's ratio == the canonical token) is exactly what
  //    C128 removed, so it is replaced by the ABSENCE, which is the property the
  //    removal is supposed to have. The segment list is OWNED in a local: the
  //    pointer INTO the vector must not outlive it (C86's ASan lesson).
  const prism::layout::Layout layout = TestLayout();
  const prism::layout::StatusInfo info = GrowStatus();
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(layout, info);
  PRISM_CHECK(RatioSegment(segments) == nullptr);

  // 5. byte-identical across the surfaces that still carry it (the audit's ①).
  //    C105: the card's value line IS the token now (the "RATIO " prefix that
  //    used to be stripped here is a label of its own), so the comparison is
  //    exact instead of "equal after six characters".
  PRISM_CHECK_EQ(cell, sidebarStatus.ratioView);
  PRISM_CHECK_EQ(cardLine, sidebarStatus.ratioView);
}

PRISM_TEST("c81.a3.zero_bytes_unknown_everywhere") {
  std::vector<SortItem> rows = {File("empty.txt", 0, 0)};
  const std::string cell = prism::list::RatioCellText(rows[0]);
  PRISM_CHECK_EQ(cell, std::string("未知"));

  std::vector<prism::ArchiveItem> catalog;
  {
    prism::ArchiveItem item;
    item.path = "empty.txt";
    item.index = 0;
    catalog.push_back(item);
  }
  prism::sidebar::Model model(&catalog);
  const prism::sidebar::Status sidebarStatus = model.ComputeStatus();
  PRISM_CHECK_EQ(sidebarStatus.ratioView, std::string("未知"));
  PRISM_CHECK_EQ(CardRatioLine(&model), std::string("未知"));

  // 4. ... and the status band does not carry a ratio slot at all (C128), so it
  //    cannot disagree with the two surfaces above about a number it never shows.
  const prism::layout::Layout layout = TestLayout();
  const prism::layout::StatusInfo info = GrowStatus();
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(layout, info);  // owned: see growth_consistency
  PRISM_CHECK(RatioSegment(segments) == nullptr);
}

PRISM_TEST("c81.a3.shrink_consistency") {
  std::vector<SortItem> rows = {File("docs.zip", 10000, 4280)};
  const std::string token = prism::CompressionRatioView(10000, 4280);
  PRISM_CHECK_EQ(token, std::string("42.8%"));

  std::vector<prism::ArchiveItem> catalog;
  {
    prism::ArchiveItem item;
    item.path = "docs.zip";
    item.index = 0;
    item.size = 10000;
    item.packSize = 4280;
    catalog.push_back(item);
  }
  prism::sidebar::Model model(&catalog);
  PRISM_CHECK_EQ(model.ComputeStatus().ratioView, token);
  PRISM_CHECK_EQ(CardRatioLine(&model), token);
  PRISM_CHECK_EQ(prism::list::RatioCellText(rows[0]), token);

  // 4. the band no longer shows a ratio (C128): the two surfaces above are the
  //    whole claim now.
  const prism::layout::Layout layout = TestLayout();
  const prism::layout::StatusInfo info = GrowStatus();
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(layout, info);  // owned: see growth_consistency
  PRISM_CHECK(RatioSegment(segments) == nullptr);
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) std::printf("%s\n", kMutantBanner);
  if (kMutantMarker != nullptr) prism::test::SetMutationVerdict(kMutantMarker);

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed");
    return 1;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,
                              reinterpret_cast<void**>(&gWic)))) {
    std::puts("FAIL: WIC factory");
    return 1;
  }
  const int result = prism::test::RunAll(argc, argv);
  if (result != 0) {
    std::puts("C81 A2A3 CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C81 A2A3 CHECKS PASSED");
  return 0;
}
