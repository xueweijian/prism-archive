// PrismArchive -- PrismCore
// C102: DAYLIGHT LAYERED MATERIAL -- the canvas, the island and the layers
// between them, asserted as NUMBERS (and in the Windows suite as pixels).
//
// THE MEASURED DEFECT (plan 2.1 / the user's report). The Daylight canvas
// presented as (247,249,252) and the floating island as (252,252,254): five
// levels apart, so the island had no body at all -- what the user saw was "a
// plastic pill on a washed-out white page", rim and shadow only. The plan asks
// for three visibly different layers:
//
//   canvas   #F5F5F7   (the paper)
//   cards    #FFFFFF   (the content plate, kept)
//   islands  rgba(255,255,255,.82)  (the glass that floats above both)
//
// WHAT THIS FILE PINS, all through prism::chrome's ONE theme entry:
//   1. the Daylight canvas presents as (245,245,247) +-2;
//   2. the island stands at least 8 levels off that canvas (max channel
//      difference, the metric the card quotes: today it is 5);
//   3. Midnight (16,17,23) and HighContrast (0,0,0) are UNCHANGED -- the
//      regression arm: their rows are compared verbatim, not just re-composited,
//      because "the dark themes did not move" is a statement about the table;
//   4. HighContrast text still clears WCAG 4.5:1 (the C83/C91 contrast gate);
//   5. the content plate stays #FFFFFF.
//
// NEGATIVE CONTROL (CONTRACTS D3 / D26 / D27), one binary:
//   PRISM_C102_MUTANT_FLAT   the Daylight island alpha goes back to the pre-C102
//                            .58 on EffectiveGlassSurface() -- the one path every
//                            painter uses. Assertion 2 MUST go red and the
//                            verdict is armed on the injection counter, so an
//                            unrelated failure can never print CAUGHT.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#include "mini_test.hpp"

#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/ThemeTokens.hpp"

namespace {

namespace ch = prism::chrome;
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;

#if defined(PRISM_C102_MUTANT_FLAT)
constexpr const char* kMutation = "flat";
#else
constexpr const char* kMutation = nullptr;
#endif

// The card's own rounding: a presented colour is reported in bytes, so the
// assertions are about what a screenshot would show.
int ByteOf(float v) {
  const double scaled = static_cast<double>(v) * 255.0 + 0.5;
  if (scaled < 0.0) return 0;
  if (scaled > 255.0) return 255;
  return static_cast<int>(scaled);
}

struct Pixel {
  int r = 0;
  int g = 0;
  int b = 0;
};

Pixel ToPixel(const Rgba& c) { return Pixel{ByteOf(c.r), ByteOf(c.g), ByteOf(c.b)}; }

// The card's difference metric: the largest single-channel gap. (247,249,252)
// vs (252,252,254) is 5 -- the number in the report.
int MaxChannelDiff(const Pixel& a, const Pixel& b) {
  return (std::max)({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

// A named expectation: the measured triple is always printed, so a red run says
// WHICH channel drifted and by how much instead of just "false".
bool Near(const char* label, const Pixel& got, const Pixel& want, int tolerance) {
  const bool ok = MaxChannelDiff(got, want) <= tolerance;
  std::printf("  [c102] %-34s got=(%3d,%3d,%3d) want=(%3d,%3d,%3d) +-%d %s\n", label, got.r,
              got.g, got.b, want.r, want.g, want.b, tolerance, ok ? "ok" : "RED");
  return ok;
}

std::string Name(Variant v) { return std::string(prism::theme::VariantName(v)); }

// A raw table row, compared field by field. This is the regression arm: it asks
// the TABLE whether the dark themes moved, before any compositing can hide it.
bool RowIs(const char* label, TokenId id, Variant variant, int r, int g, int b, float a) {
  const Rgba row = prism::theme::Lookup(id, variant).color;
  const Pixel got = ToPixel(row);
  const bool ok = got.r == r && got.g == g && got.b == b &&
                  std::abs(static_cast<double>(row.a) - static_cast<double>(a)) < 0.005;
  std::printf("  [c102] %-34s got=(%3d,%3d,%3d,%.2f) want=(%3d,%3d,%3d,%.2f) %s\n", label,
              got.r, got.g, got.b, static_cast<double>(row.a), r, g, b,
              static_cast<double>(a), ok ? "ok" : "RED");
  return ok;
}

// The rail's / card's plate: the floating-island surface at the near-opaque
// alpha the sidebar resolves (GalleryRenderer::TokenColor(Panel)).
Rgba PanelColor(Variant variant) {
  Rgba panel = prism::theme::Lookup(TokenId::SurfaceFloatingIsland, variant).color;
  panel.a = 0.94f;
  return prism::theme::CompositeOver(panel, ch::CanvasPresentedColor(variant));
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C102 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C102 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C102_MUTANT_FLAT)
      return prism::theme::MutantFlatHits() > 0;
#else
      return false;
#endif
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL C102 DAYLIGHT CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE CANVAS: what a resting Daylight region presents as.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 the daylight canvas presents as #F5F5F7") {
  const Pixel presented = ToPixel(ch::CanvasPresentedColor(Variant::Daylight));
  const Pixel flat = ToPixel(ch::CanvasColor(Variant::Daylight));
  std::printf("  [c102] canvas daylight flat=(%d,%d,%d) presented=(%d,%d,%d)\n", flat.r,
              flat.g, flat.b, presented.r, presented.g, presented.b);
  PRISM_CHECK(Near("canvas presented (day)", presented, Pixel{245, 245, 247}, 2));
  // The veil must not be the authority on the canvas colour: the flat tile (the
  // Clear() colour with the backdrop layer off) has to agree with the presented
  // one, or a band fill and the target's clear would be two different greys.
  PRISM_CHECK(Near("canvas flat (day)", flat, presented, 2));
}

// ---------------------------------------------------------------------------
// 2. THE ISLAND vs THE CANVAS: the "body" the card is about. THIS is the
//    assertion the flat mutant kills.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 the island stands at least 8 levels off the canvas") {
  const Pixel canvas = ToPixel(ch::CanvasPresentedColor(Variant::Daylight));
  const Pixel island =
      ToPixel(ch::ThemeSurfaceColor(TokenId::SurfaceFloatingIsland, Variant::Daylight));
  const int levels = MaxChannelDiff(island, canvas);
  std::printf("  [c102] island=(%d,%d,%d) canvas=(%d,%d,%d) levels=%d (floor 8)\n",
              island.r, island.g, island.b, canvas.r, canvas.g, canvas.b, levels);
  PRISM_CHECK(levels >= 8);

  // ... and the material is still glass: .82, not an opaque plate. An opaque
  // island would have passed the line above by destroying the layer, so the
  // alpha is asserted separately.
  const Rgba surface = prism::theme::EffectiveGlassSurface(Variant::Daylight);
  std::printf("  [c102] island alpha=%.3f (plan 2.1 asks .82)\n",
              static_cast<double>(surface.a));
  PRISM_CHECK(std::abs(static_cast<double>(surface.a) - 0.82) < 0.005);
}

// ---------------------------------------------------------------------------
// 3. THE REGRESSION ARM: Midnight and HighContrast did not move -- not by one
//    channel, not by one alpha thousandth.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 night and high-contrast rows are untouched") {
  // Midnight: canvas, island, the two backdrop rows, the text roles.
  // C124 SUPERSEDES THE "night did not move" HALF OF THIS ARM. The card builds
  // the FIFVE-step surface ladder on the audit's own canvas (#0B0D12), so the
  // Midnight canvas row is opaque #0B0D12 instead of an 68 % sheet over
  // #0B0C10, and the island is the ladder's floating tier. The rows below are
  // the table's NEW values, still compared verbatim -- the point of the arm (a
  // dark row may not drift silently) is unchanged.
  PRISM_CHECK(RowIs("midnight canvas", TokenId::SurfaceCanvasBase, Variant::Midnight, 11, 13,
                    18, 1.0f));
  PRISM_CHECK(RowIs("midnight island", TokenId::SurfaceFloatingIsland, Variant::Midnight, 43,
                    49, 62, 0.75f));
  PRISM_CHECK(RowIs("midnight ghost", TokenId::SurfaceBackdropGhost, Variant::Midnight, 11, 12,
                    16, 1.0f));
  // C121: the veil row moved from --surface-backdrop-reference (11,12,16, the
  // colour that DARKENED the canvas by 3 levels when refraction was on) to the
  // canvas colour itself, which is C102's own rule for Daylight applied to
  // Midnight. The ghost row above is untouched: the ghost is only ever consumed
  // for its ALPHA.
  // C124: the veil is still THE CANVAS COLOUR (C121's rule); the canvas moved to
  // the ladder's #0B0D12, so the veil moved with it. Toggling refraction still
  // cannot move a resting pixel -- c121 H2 measures dE 0.00.
  PRISM_CHECK(RowIs("midnight veil", TokenId::SurfaceBackdropVeil, Variant::Midnight, 11, 13,
                    18, 0.65f));
  PRISM_CHECK(RowIs("midnight text primary", TokenId::TextPrimary, Variant::Midnight, 245, 245,
                    247, 1.0f));

  // HighContrast: opaque, maximum separation.
  PRISM_CHECK(RowIs("hc canvas", TokenId::SurfaceCanvasBase, Variant::HighContrast, 0, 0, 0,
                    1.0f));
  PRISM_CHECK(RowIs("hc island", TokenId::SurfaceFloatingIsland, Variant::HighContrast, 28, 28,
                    30, 1.0f));
  PRISM_CHECK(RowIs("hc text primary", TokenId::TextPrimary, Variant::HighContrast, 255, 255,
                    255, 1.0f));

  // ... and the presented pixels the C91 suite pins.
  // C121: the veil is the canvas colour in every theme now; Midnight's
  // presented pixel is its plain canvas composite. C124: #0B0D12 -- the
  // ladder's canvas and the audit's measured #0C0D11 +-1 (the panel tier only
  // reaches 1.064:1 from THIS canvas; from the brighter (16,17,23) it measures
  // 1.029, below the card's 1.03 floor).
  PRISM_CHECK(Near("canvas presented (night)",
                   ToPixel(ch::CanvasPresentedColor(Variant::Midnight)), Pixel{11, 13, 18}, 2));
  PRISM_CHECK(Near("canvas presented (hc)",
                   ToPixel(ch::CanvasPresentedColor(Variant::HighContrast)), Pixel{0, 0, 0},
                   0));
}

// ---------------------------------------------------------------------------
// 4. THE ACCESSIBILITY GATE: HighContrast text still clears WCAG 4.5:1, and so
//    does the Daylight ink on the NEW canvas.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 high-contrast text keeps its 4.5:1") {
  for (const Variant variant : {Variant::Daylight, Variant::HighContrast}) {
    const ch::Palette palette = ch::PaletteFor(variant);
    const double text = ch::TextContrast(palette);
    const double highlight = ch::HighlightContrast(palette);
    std::printf("  [c102] %s text contrast=%.2f highlight=%.2f\n", Name(variant).c_str(),
                text, highlight);
    PRISM_CHECK(text >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(highlight >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(ch::MeetsBodyText(palette));
  }
}

// ---------------------------------------------------------------------------
// 5. THE CONTENT PLATE: #FFFFFF, kept. The island got heavier; the cards did
//    not get greyer.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 the content plate stays white") {
  // Daylight: the panel is #FFFFFF at .94 over the new canvas -- still white to
  // within two levels, i.e. the cards did NOT follow the canvas down.
  const Pixel day = ToPixel(PanelColor(Variant::Daylight));
  std::printf("  [c102] daylight content plate=(%d,%d,%d)\n", day.r, day.g, day.b);
  PRISM_CHECK(Near("content plate (day)", day, Pixel{255, 255, 255}, 2));

  // HighContrast: the plate is the opaque (28,28,30) island, not white. Pinned
  // so "the cards stay white" cannot be read as "the dark theme went light".
  const Pixel hc = ToPixel(PanelColor(Variant::HighContrast));
  std::printf("  [c102] hc content plate=(%d,%d,%d)\n", hc.r, hc.g, hc.b);
  PRISM_CHECK(Near("content plate (hc)", hc, Pixel{26, 26, 28}, 2));
}
