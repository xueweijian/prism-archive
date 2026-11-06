// PrismArchive -- PrismCore
// C91: THE THEME SURFACE ARITHMETIC -- the numbers the pixels must land on.
//
// THE MEASURED DEFECT. A whole-window day -> night diff of 1,544,424 px moved
// 122,969 px (7.96 %), all of them in the title bar and the DWM border. The
// content -- 92 % of the window -- was pixel-identical in all three themes,
// because the canvas was the literal #0B0E13 and the renderer's variant was
// never set.
//
// WHAT THIS FILE PINS. prism::chrome::CanvasColor / CanvasPresentedColor /
// ThemeSurfaceColor are the ONE entry every surface goes through. Here they are
// asked for all three variants and compared with the values the Windows pixel
// suite samples from a real rendered frame:
//
//   canvas      day (245,245,247)  night (13,14,18)  high-contrast (0,0,0)
//   island      day (253,253,254)  night (26,29,37)
//   rail panel  day (255,255,255)  high-contrast (26,26,28)
//   tab plate   day (254,254,255)  high-contrast (40,40,44)
//   selected    day (219,236,255)
//   info card   day (242,242,245)
//   status strip day (249,250,253) high-contrast (4,5,6)
//
// NEGATIVE CONTROL (CONTRACTS D3, one binary):
//   PRISM_C91_MUTANT_CORE_MIDNIGHT  every variant resolves to Midnight -> the
//                                   Day assertions MUST go red, and the verdict
//                                   is armed on the injection counter so an
//                                   unrelated failure cannot print CAUGHT.
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

constexpr int kCanvasTolerance = 2;

#if defined(PRISM_C91_MUTANT_CORE_MIDNIGHT)
constexpr const char* kMutation = "core_midnight";
#else
constexpr const char* kMutation = nullptr;
#endif

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

// A named expectation: prints the measured triple next to the wanted one, so a
// red run says WHICH surface drifted and by how much instead of "false".
bool Near(const char* label, const Pixel& got, const Pixel& want, int tolerance) {
  const int dr = std::abs(got.r - want.r);
  const int dg = std::abs(got.g - want.g);
  const int db = std::abs(got.b - want.b);
  const bool ok = dr <= tolerance && dg <= tolerance && db <= tolerance;
  std::printf("  [c91] %-34s got=(%3d,%3d,%3d) want=(%3d,%3d,%3d) +-%d %s\n", label, got.r,
              got.g, got.b, want.r, want.g, want.b, tolerance, ok ? "ok" : "RED");
  return ok;
}

std::string Name(Variant v) { return std::string(prism::theme::VariantName(v)); }

// The rail's panel: the floating-island token at the near-opaque alpha the
// sidebar resolves (GalleryRenderer::TokenColor(Panel)), over the canvas.
Rgba PanelColor(Variant variant) {
  Rgba panel = prism::theme::Lookup(TokenId::SurfaceFloatingIsland, variant).color;
  panel.a = 0.94f;
  return prism::theme::CompositeOver(panel, ch::CanvasPresentedColor(variant));
}

// The status strip: the canvas plus the card's documented lift. The renderer
// applies the same lift to the same canvas (GalleryRenderer::DrawBands).
Rgba StatusStripColor(Variant variant) {
  Rgba strip = ch::CanvasPresentedColor(variant);
  strip.r += 0.016f;
  strip.g += 0.018f;
  strip.b += 0.022f;
  return strip;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C91 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C91 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C91_MUTANT_CORE_MIDNIGHT)
      // The injected branch really ran: the counter is only touched inside it.
      return prism::chrome::MutantCoreHits() > 0;
#else
      return false;
#endif
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL THEME-SURFACE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE CANVAS: three themes, three colours, and none of them a literal.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the canvas is the theme's composite") {
  // The card's three numbers, verbatim. High contrast is EXACT: an opaque black
  // canvas has no arithmetic left in it, so a tolerance there would only hide a
  // fallback to a Midnight row.
  // C102 moved the Daylight canvas to the plan's #F5F5F7; the assertion and
  // its +-2 tolerance are unchanged, only the expected triple is.
  const Pixel wantDay{245, 245, 247};
  const Pixel wantNight{13, 14, 18};
  const Pixel wantHighContrast{0, 0, 0};

  const Pixel day = ToPixel(ch::CanvasPresentedColor(Variant::Daylight));
  const Pixel night = ToPixel(ch::CanvasPresentedColor(Variant::Midnight));
  const Pixel highContrast = ToPixel(ch::CanvasPresentedColor(Variant::HighContrast));
  std::printf("  [c91] canvas day=(%d,%d,%d) night=(%d,%d,%d) hc=(%d,%d,%d)\n", day.r, day.g,
              day.b, night.r, night.g, night.b, highContrast.r, highContrast.g,
              highContrast.b);

  PRISM_CHECK(Near("canvas daylight", day, wantDay, kCanvasTolerance));
  PRISM_CHECK(Near("canvas midnight", night, wantNight, kCanvasTolerance));
  PRISM_CHECK(Near("canvas high contrast", highContrast, wantHighContrast, 0));

  // ... and the three are pairwise different: "the theme reached the canvas" is
  // exactly the statement that the old build failed (all three were #0B0E13).
  PRISM_CHECK(day.r != night.r || day.g != night.g || day.b != night.b);
  PRISM_CHECK(night.r != highContrast.r || night.g != highContrast.g ||
              night.b != highContrast.b);
  PRISM_CHECK(day.r != highContrast.r || day.g != highContrast.g ||
              day.b != highContrast.b);

  // The presented canvas is the flat one washed by the veil; the veil must not
  // move it by more than the card's tolerance or the Clear() and the band fills
  // would disagree with the sampled pixel.
  const Pixel flatDay = ToPixel(ch::CanvasColor(Variant::Daylight));
  PRISM_CHECK(Near("flat canvas daylight", flatDay, wantDay, 3));
  std::printf("  [c91] veil shift: day (%d,%d,%d) -> (%d,%d,%d)\n", flatDay.r, flatDay.g,
              flatDay.b, day.r, day.g, day.b);
}

// ---------------------------------------------------------------------------
// 2. THE CONTENT SURFACES ride that canvas -- island, panel, tab, selection,
//    info card, status strip. All three variants.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 every content surface resolves from the theme") {
  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    std::printf("  [c91] --- %s ---\n", Name(variant).c_str());
    const Rgba canvas = ch::CanvasPresentedColor(variant);
    const Pixel panel = ToPixel(PanelColor(variant));

    // The floating island (breadcrumb pill, progress capsule, action band).
    const Pixel island =
        ToPixel(ch::ThemeSurfaceColor(TokenId::SurfaceFloatingIsland, variant));
    // The raised tab plate.
    const Pixel tab = ToPixel(ch::ThemeSurfaceColor(TokenId::SurfaceTabActive, variant));
    // The sidebar's selected row: --selection-tint over the rail's panel.
    const Pixel selected = ToPixel(prism::theme::CompositeOver(
        prism::theme::Lookup(TokenId::SelectionTint, variant).color, PanelColor(variant)));
    // The telemetry capsule: the canvas pane at .95 over the panel.
    Rgba pane = prism::theme::Lookup(TokenId::SurfaceCanvasBase, variant).color;
    pane.a = 0.95f;
    const Pixel infoCard = ToPixel(prism::theme::CompositeOver(pane, PanelColor(variant)));
    const Pixel strip = ToPixel(StatusStripColor(variant));

    if (variant == Variant::Daylight) {
      PRISM_CHECK(Near("island (day)", island, Pixel{253, 253, 254}, 2));
      PRISM_CHECK(Near("rail panel (day)", panel, Pixel{255, 255, 255}, 2));
      PRISM_CHECK(Near("tab plate (day)", tab, Pixel{254, 254, 255}, 2));
      PRISM_CHECK(Near("selected row (day)", selected, Pixel{219, 236, 255}, 2));
      PRISM_CHECK(Near("info card (day)", infoCard, Pixel{242, 242, 245}, 3));
      PRISM_CHECK(Near("status strip (day)", strip, Pixel{249, 250, 253}, 3));
    } else if (variant == Variant::Midnight) {
      PRISM_CHECK(Near("island (night)", island, Pixel{26, 29, 37}, 2));
    } else {
      PRISM_CHECK(Near("tab plate (hc)", tab, Pixel{40, 40, 44}, 2));
      PRISM_CHECK(Near("rail panel (hc)", panel, Pixel{26, 26, 28}, 2));
      PRISM_CHECK(Near("status strip (hc)", strip, Pixel{4, 5, 6}, 3));
      // The card's item 7 quotes (26,26,28) for the HC info card, but its own
      // formula -- canvas-base @.95 over the panel -- cannot produce it: an
      // opaque black canvas pane leaves (1,1,2). The number in the card is the
      // PANEL's colour (item 4 quotes it too), so this asserts the formula and
      // prints the measured value instead of pretending the two agree.
      std::printf("  [c91] info card (hc) measured=(%d,%d,%d); the card's (26,26,28) is\n"
                  "        the rail panel, not the .95 pane over it\n",
                  infoCard.r, infoCard.g, infoCard.b);
      PRISM_CHECK(infoCard.r < 8 && infoCard.g < 8 && infoCard.b < 8);
    }

    // The text roles are tokens too: the list/draw code recolours its brushes
    // from these every frame.
    const Pixel primary =
        ToPixel(ch::ThemeSurfaceColor(TokenId::TextPrimary, variant));
    const Pixel wantPrimary =
        ToPixel(prism::theme::Lookup(TokenId::TextPrimary, variant).color);
    PRISM_CHECK(Near("text primary", primary, wantPrimary, 1));
    // Ink on that surface must stay legible (WCAG 4.5:1) in every variant --
    // the point of theming is not to trade contrast for colour.
    const double contrast = prism::theme::ContrastRatio(
        prism::theme::Lookup(TokenId::TextPrimary, variant).color, canvas);
    std::printf("  [c91] %s text/canvas contrast=%.2f\n", Name(variant).c_str(),
              contrast);
    PRISM_CHECK(contrast >= prism::theme::kWcagBodyTextMinRatio);
  }
}

// ---------------------------------------------------------------------------
// 3. THE SHELL GATE, in pure form: the caption is continuous with the canvas
//    in EVERY variant. This is C83's rule applied to the themed canvas -- in
//    Day it used to be false (shell 255 vs canvas 12.9) and only Midnight was
//    ever tested.
// ---------------------------------------------------------------------------
PRISM_TEST("c91 the shell stays continuous with every theme's canvas") {
  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    const double shell = ch::Luma255(ch::PaletteFor(variant).background);
    const double canvas = ch::Luma255(ch::CanvasPresentedColor(variant));
    std::printf("  [c91] %s shell luma=%.2f canvas luma=%.2f delta=%.2f\n",
                Name(variant).c_str(), shell, canvas, shell - canvas);
    PRISM_CHECK(ch::ShellMatchesCanvas(shell, canvas));
  }
  // The predicate is still a gate, not a tautology: a white caption over the
  // MIDNIGHT canvas must fail, which is the pre-C83 defect.
  PRISM_CHECK(!ch::ShellMatchesCanvas(
      255.0, ch::Luma255(ch::CanvasPresentedColor(Variant::Midnight))));
}
