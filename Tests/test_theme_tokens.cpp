// Theme tokens (blueprint ch. 3.3) -- the whole table as constants, the WCAG
// contrast gate, the n=4.2 squircle SDF, and the LiquidGlassConstants hand-off.
//
// Why these assertions exist:
//   * a design token that is not pinned by value is a comment, not a contract;
//   * translucency means contrast must be measured AFTER compositing, so the
//     test composites the real rgba() tokens over the reference backdrop;
//   * the shader reads reg3/reg4 from the SAME numbers the UI does
//     (CONTRACTS A6: 5 registers / 20 floats / 80 bytes), checked against
//     GlassIslandScene::WriteCbuffer rather than a second invented layout.
//
// Negative controls (CONTRACTS D3): four compile-time mutations, each a
// separate WILL_FAIL binary from Tests/suites/c15-theme.cmake. This TU prints
// "C15 MUTANT ACTIVE: <name>" when one is compiled in, so the log proves the
// injection happened instead of silently failing to match.
#include "GlassIsland.hpp"
#include "ThemeTokens.hpp"
#include "mini_test.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace {

using prism::glass::GlassIslandScene;
using prism::glass::IslandId;
using prism::theme::ChannelFromByte;
using prism::theme::CompositeOver;
using prism::theme::ContrastRatio;
using prism::theme::Gradient;
using prism::theme::Kind;
using prism::theme::Lookup;
using prism::theme::Rgba;
using prism::theme::SquircleField;
using prism::theme::SquircleSdf;
using prism::theme::TokenId;
using prism::theme::TokenValue;
using prism::theme::Variant;

bool Near(double a, double b, double epsilon = 1e-6) {
  return std::abs(a - b) <= epsilon;
}

Rgba ColorOf(TokenId id, Variant variant) {
  return Lookup(id, variant).color;
}

Gradient GradientOf(TokenId id, Variant variant) {
  return Lookup(id, variant).gradient;
}

// WCAG relative luminance is defined on opaque colours; text roles are opaque
// by construction, but assert it so a future translucent text token cannot
// silently pass the ratio gate with a meaningless number.
bool IsOpaque(const Rgba& c) { return Near(c.a, 1.0, 1e-6); }

PRISM_TEST("3.3: every token resolves for Daylight, Midnight and HighContrast") {
  const Variant variants[] = {Variant::Daylight, Variant::Midnight,
                              Variant::HighContrast};
  for (std::size_t i = 0; i < prism::theme::kTokenCount; ++i) {
    const TokenId id = static_cast<TokenId>(i);
    PRISM_CHECK(!prism::theme::TokenName(id).empty());
    for (const Variant variant : variants) {
      const TokenValue value = Lookup(id, variant);
      PRISM_CHECK(value.known);
      PRISM_CHECK(value.id == id);
      PRISM_CHECK(value.kind == prism::theme::KindOf(id));
    }
  }
  // The seven blueprint rows, spelled exactly as the CSS property in 3.3.
  const std::string_view rows[] = {
      "--surface-canvas-base",        "--surface-floating-island",
      "--border-specular-rim",        "--accent-liquid-primary",
      "--font-stack-system",          "--spring-snappy-interactive",
      "--spring-fluid-sheet",
  };
  for (const std::string_view name : rows) {
    PRISM_CHECK(Lookup(name, Variant::Daylight).known);
    PRISM_CHECK(Lookup(name, Variant::HighContrast).known);
  }
}

PRISM_TEST("3.3: blueprint values are verbatim, not approximations") {
  // --surface-canvas-base
  const Rgba dayCanvas = ColorOf(TokenId::SurfaceCanvasBase, Variant::Daylight);
  PRISM_CHECK(Near(dayCanvas.r, ChannelFromByte(246)));
  PRISM_CHECK(Near(dayCanvas.g, ChannelFromByte(248)));
  PRISM_CHECK(Near(dayCanvas.b, ChannelFromByte(252)));
  PRISM_CHECK(Near(dayCanvas.a, 0.72));
  const Rgba nightCanvas = ColorOf(TokenId::SurfaceCanvasBase, Variant::Midnight);
  PRISM_CHECK(Near(nightCanvas.r, ChannelFromByte(18)));
  PRISM_CHECK(Near(nightCanvas.g, ChannelFromByte(20)));
  PRISM_CHECK(Near(nightCanvas.b, ChannelFromByte(26)));
  PRISM_CHECK(Near(nightCanvas.a, 0.68));

  // --surface-floating-island
  const Rgba dayIsland = ColorOf(TokenId::SurfaceFloatingIsland, Variant::Daylight);
  PRISM_CHECK(Near(dayIsland.r, 1.0) && Near(dayIsland.g, 1.0) &&
              Near(dayIsland.b, 1.0));
  PRISM_CHECK(Near(dayIsland.a, 0.58));
  const Rgba nightIsland =
      ColorOf(TokenId::SurfaceFloatingIsland, Variant::Midnight);
  PRISM_CHECK(Near(nightIsland.r, ChannelFromByte(38)));
  PRISM_CHECK(Near(nightIsland.g, ChannelFromByte(42)));
  PRISM_CHECK(Near(nightIsland.b, ChannelFromByte(54)));
  PRISM_CHECK(Near(nightIsland.a, 0.52));

  // --border-specular-rim: top .85 -> bottom .18 (day), .28 -> .04 (night)
  const Gradient dayRim = GradientOf(TokenId::BorderSpecularRim, Variant::Daylight);
  PRISM_CHECK(Near(dayRim.from.a, 0.85));
  PRISM_CHECK(Near(dayRim.to.a, 0.18));
  const Gradient nightRim =
      GradientOf(TokenId::BorderSpecularRim, Variant::Midnight);
  PRISM_CHECK(Near(nightRim.from.a, 0.28));
  PRISM_CHECK(Near(nightRim.to.a, 0.04));
  PRISM_CHECK(Near(dayRim.from.r, 1.0) && Near(nightRim.from.b, 1.0));

  // --accent-liquid-primary: #007AFF -> #0055D4 @.88; #0A84FF -> #0060DF @.82
  const Gradient dayAccent =
      GradientOf(TokenId::AccentLiquidPrimary, Variant::Daylight);
  PRISM_CHECK(Near(dayAccent.from.r, ChannelFromByte(0)));
  PRISM_CHECK(Near(dayAccent.from.g, ChannelFromByte(122)));
  PRISM_CHECK(Near(dayAccent.from.b, ChannelFromByte(255)));
  PRISM_CHECK(Near(dayAccent.from.a, 0.88));
  PRISM_CHECK(Near(dayAccent.to.r, ChannelFromByte(0)));
  PRISM_CHECK(Near(dayAccent.to.g, ChannelFromByte(85)));
  PRISM_CHECK(Near(dayAccent.to.b, ChannelFromByte(212)));
  const Gradient nightAccent =
      GradientOf(TokenId::AccentLiquidPrimary, Variant::Midnight);
  PRISM_CHECK(Near(nightAccent.from.r, ChannelFromByte(10)));
  PRISM_CHECK(Near(nightAccent.from.g, ChannelFromByte(132)));
  PRISM_CHECK(Near(nightAccent.from.b, ChannelFromByte(255)));
  PRISM_CHECK(Near(nightAccent.from.a, 0.82));
  PRISM_CHECK(Near(nightAccent.to.b, ChannelFromByte(223)));

  // --spring-snappy-interactive: m=1, k=420, c=28 (zeta ~ 0.68)
  const auto snappy =
      Lookup(TokenId::SpringSnappyInteractive, Variant::Daylight).spring;
  PRISM_CHECK(Near(snappy.mass, 1.0));
  PRISM_CHECK(Near(snappy.stiffness, 420.0));
  PRISM_CHECK(Near(snappy.damping, 28.0));
  PRISM_CHECK(Near(snappy.DampingRatio(), 0.68, 0.02));
  // --spring-fluid-sheet: m=1, k=280, c=34 (zeta ~ 1.0, no overshoot)
  const auto fluid =
      Lookup(TokenId::SpringFluidSheet, Variant::Daylight).spring;
  PRISM_CHECK(Near(fluid.mass, 1.0));
  PRISM_CHECK(Near(fluid.stiffness, 280.0));
  PRISM_CHECK(Near(fluid.damping, 34.0));
  PRISM_CHECK(Near(fluid.DampingRatio(), 1.0, 0.02));

  // --font-stack-system: Western SF Pro Display/Inter; CJK PingFang/HarmonyOS.
  const auto dayFont = Lookup(TokenId::FontStackSystem, Variant::Daylight).font;
  PRISM_CHECK(dayFont.latin.find("SF Pro Display") != std::string_view::npos);
  PRISM_CHECK(dayFont.latin.find("Inter") != std::string_view::npos);
  PRISM_CHECK(dayFont.cjk.find("PingFang SC") != std::string_view::npos);
  PRISM_CHECK(dayFont.cjk.find("HarmonyOS Sans SC") != std::string_view::npos);
  PRISM_CHECK(dayFont.tabularNumbers);
  PRISM_CHECK(dayFont.weightDelta == 0);
  const auto nightFont =
      Lookup(TokenId::FontStackSystem, Variant::Midnight).font;
  PRISM_CHECK(nightFont.weightDelta == -20);  // Dark Mode weight -20
}

PRISM_TEST("lookup falls back through Midnight to the Daylight base") {
  // HighContrast overrides the surfaces/text it must: no fallback.
  const TokenValue hcCanvas = Lookup(TokenId::SurfaceCanvasBase, Variant::HighContrast);
  PRISM_CHECK(hcCanvas.known);
  PRISM_CHECK(!hcCanvas.fellBack);
  PRISM_CHECK(hcCanvas.source == Variant::HighContrast);
  PRISM_CHECK(Near(hcCanvas.color.a, 1.0));  // HC is opaque

  // ... and INHERITS the rest instead of keeping a divergent copy. The font row
  // is deliberately absent for HighContrast, so Midnight answers.
  const TokenValue hcFont = Lookup(TokenId::FontStackSystem, Variant::HighContrast);
  PRISM_CHECK(hcFont.known);
  PRISM_CHECK(hcFont.fellBack);
  PRISM_CHECK(hcFont.source == Variant::Midnight);
  PRISM_CHECK(hcFont.font.weightDelta == -20);

  // Physics has no variant rows at all: both Midnight and HighContrast inherit
  // the Daylight base (the blueprint repeats the same m/k/c in both columns).
  const TokenValue nightSpring =
      Lookup(TokenId::SpringFluidSheet, Variant::Midnight);
  PRISM_CHECK(nightSpring.fellBack);
  PRISM_CHECK(nightSpring.source == Variant::Daylight);
  PRISM_CHECK(Near(nightSpring.spring.stiffness, 280.0));
  const TokenValue hcSpring =
      Lookup(TokenId::SpringSnappyInteractive, Variant::HighContrast);
  PRISM_CHECK(hcSpring.fellBack);
  PRISM_CHECK(hcSpring.source == Variant::Daylight);
  PRISM_CHECK(Near(hcSpring.spring.damping, 28.0));
}

PRISM_TEST("lookup accepts the CSS spelling and never invents a missing token") {
  for (std::size_t i = 0; i < prism::theme::kTokenCount; ++i) {
    const TokenId id = static_cast<TokenId>(i);
    PRISM_CHECK(prism::theme::FindToken(prism::theme::TokenName(id)) == id);
    PRISM_CHECK(prism::theme::FindToken(
                    prism::theme::TokenName(id).substr(2)) == id);
  }
  PRISM_CHECK(Lookup("--accent-liquid-primary", Variant::Midnight).kind ==
              Kind::Gradient);
  PRISM_CHECK(Lookup("--font-stack-system", Variant::HighContrast).known);

  const TokenValue missing = Lookup("--no-such-token", Variant::Daylight);
  PRISM_CHECK(!missing.known);
  PRISM_CHECK(missing.fellBack);
  PRISM_CHECK(Near(missing.color.r, 1.0) && Near(missing.color.g, 0.0) &&
              Near(missing.color.b, 1.0));  // magenta sentinel, never black
  PRISM_CHECK(Lookup(TokenId::Count, Variant::Daylight).known == false);
}

PRISM_TEST("WCAG: body >= 4.5 and secondary >= 3.0 on every variant") {
  const Variant variants[] = {Variant::Daylight, Variant::Midnight,
                              Variant::HighContrast};
  for (const Variant variant : variants) {
    const Rgba backdrop =
        ColorOf(TokenId::SurfaceBackdropReference, variant);
    const Rgba canvas =
        CompositeOver(ColorOf(TokenId::SurfaceCanvasBase, variant), backdrop);
    const Rgba island =
        CompositeOver(ColorOf(TokenId::SurfaceFloatingIsland, variant), canvas);
    const Rgba body = ColorOf(TokenId::TextPrimary, variant);
    const Rgba secondary = ColorOf(TokenId::TextSecondary, variant);
    PRISM_CHECK(IsOpaque(body));
    PRISM_CHECK(IsOpaque(secondary));
    PRISM_CHECK(IsOpaque(canvas));
    PRISM_CHECK(IsOpaque(island));

    const double bodyOnCanvas = ContrastRatio(body, canvas);
    const double secondaryOnCanvas = ContrastRatio(secondary, canvas);
    const double bodyOnIsland = ContrastRatio(body, island);
    const double secondaryOnIsland = ContrastRatio(secondary, island);
    std::cout << "  [WCAG] " << variant << " body/canvas=" << bodyOnCanvas
              << " secondary/canvas=" << secondaryOnCanvas
              << " body/island=" << bodyOnIsland
              << " secondary/island=" << secondaryOnIsland << std::endl;

    PRISM_CHECK(bodyOnCanvas >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(bodyOnIsland >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(secondaryOnCanvas >= prism::theme::kWcagSecondaryTextMinRatio);
    PRISM_CHECK(secondaryOnIsland >= prism::theme::kWcagSecondaryTextMinRatio);
  }
}

PRISM_TEST("WCAG math: calibrated against the known 21:1 and 4.54:1 pairs") {
  const Rgba white(1.0f, 1.0f, 1.0f, 1.0f);
  const Rgba black(0.0f, 0.0f, 0.0f, 1.0f);
  PRISM_CHECK(Near(ContrastRatio(black, white), 21.0, 1e-9));
  PRISM_CHECK(Near(ContrastRatio(white, black), 21.0, 1e-9));  // symmetric

  // #767676 on white is the canonical "just passes 4.5:1" grey.
  const Rgba grey(ChannelFromByte(118), ChannelFromByte(118),
                  ChannelFromByte(118), 1.0f);
  PRISM_CHECK(Near(ContrastRatio(grey, white), 4.54, 0.01));

  PRISM_CHECK(Near(prism::theme::SrgbToLinear(0.0), 0.0));
  PRISM_CHECK(Near(prism::theme::SrgbToLinear(1.0), 1.0));
  PRISM_CHECK(Near(prism::theme::SrgbToLinear(0.5), 0.214041, 1e-5));

  // 50% black over white composites to mid-grey; an opaque top wins outright.
  const Rgba halfBlack(0.0f, 0.0f, 0.0f, 0.5f);
  const Rgba greyResult = CompositeOver(halfBlack, white);
  PRISM_CHECK(Near(greyResult.r, 0.5));
  PRISM_CHECK(Near(greyResult.a, 1.0));
  PRISM_CHECK(Near(CompositeOver(white, black).r, 1.0));
}

PRISM_TEST("squircle: n=4.2 boundary, signed distance and aspect ratio") {
  PRISM_CHECK(Near(prism::theme::kSquircleExponent, 4.2));
  const double a = 100.0;
  const double b = 60.0;

  // Boundary: |x/a|^n + |y/b|^n = 1 -> SDF 0 on both axes.
  PRISM_CHECK(Near(SquircleSdf(a, 0.0, a, b), 0.0, 1e-9));
  PRISM_CHECK(Near(SquircleSdf(-a, 0.0, a, b), 0.0, 1e-9));
  PRISM_CHECK(Near(SquircleSdf(0.0, b, a, b), 0.0, 1e-9));
  PRISM_CHECK(Near(SquircleSdf(0.0, -b, a, b), 0.0, 1e-9));

  // Sign: inside negative, outside positive, centre at -min(a,b).
  PRISM_CHECK(SquircleSdf(0.0, 0.0, a, b) < 0.0);
  PRISM_CHECK(Near(SquircleSdf(0.0, 0.0, a, b), -b, 1e-9));
  PRISM_CHECK(SquircleSdf(1.2 * a, 0.0, a, b) > 0.0);

  // Along the axes the first-order SDF is exact: distance to the boundary.
  PRISM_CHECK(Near(SquircleSdf(0.7 * a, 0.0, a, b), -0.3 * a, 1e-6));
  PRISM_CHECK(Near(SquircleSdf(0.0, 0.5 * b, a, b), -0.5 * b, 1e-6));
  PRISM_CHECK(Near(SquircleSdf(a - 1.0, 0.0, a, b), -1.0, 1e-6));

  // 45-degree boundary point: each normalised term is 0.5.
  const double t = std::pow(0.5, 1.0 / prism::theme::kSquircleExponent);
  PRISM_CHECK(Near(SquircleSdf(t * a, t * b, a, b), 0.0, 1e-6));

  // Control case n=2 must behave like a circle (the helper is parameterised so
  // the exponent is testable, not hard-coded into the assertion).
  PRISM_CHECK(Near(SquircleSdf(0.6 * a, 0.8 * b, a, b, 2.0), 0.0, 1e-6));

  // Degenerate extents collapse to the origin, finite, never NaN.
  PRISM_CHECK(std::isfinite(SquircleSdf(3.0, 4.0, 0.0, 0.0)));
  PRISM_CHECK(Near(SquircleSdf(3.0, 4.0, 0.0, 0.0), 5.0, 1e-9));
}

PRISM_TEST("squircle: n=4.2 corners are fuller than an ellipse") {
  const double a = 100.0;
  // 0.8a, 0.8a: n=2 gives 2*0.64 = 1.28 (outside), n=4.2 gives ~0.784
  // (inside). This is the G2 "pebble" fullness the blueprint asks for.
  const double superellipse = SquircleField(0.8 * a, 0.8 * a, a, a, 4.2);
  const double ellipse = SquircleField(0.8 * a, 0.8 * a, a, a, 2.0);
  PRISM_CHECK(superellipse < 0.0);
  PRISM_CHECK(ellipse > 0.0);
  PRISM_CHECK(SquircleSdf(0.8 * a, 0.8 * a, a, a) < 0.0);
  // And the 45-degree corner is farther from the centre than a circle's.
  PRISM_CHECK(SquircleField(a, a, a, a, 4.2) > 0.0);
}

PRISM_TEST("LiquidGlass reg3/reg4 are the same token numbers WriteCbuffer emits") {
  static_assert(sizeof(float) * 20u == 80u,
                "LiquidGlassConstants must stay 5 registers / 80 bytes (A6)");
  static_assert(prism::theme::kLiquidGlassCbufferBytes == 80u,
                "token-side layout constant must agree with the shader cbuffer");

  struct Pair {
    prism::glass::Theme glass;
    Variant token;
  };
  const Pair pairs[] = {{prism::glass::Theme::Daylight, Variant::Daylight},
                        {prism::glass::Theme::Midnight, Variant::Midnight}};

  GlassIslandScene scene;
  scene.Layout(1600, 1000);
  float cb[20] = {};
  for (const Pair& pair : pairs) {
    scene.SetTheme(pair.glass);
    scene.WriteCbuffer(IslandId::Nav, 1600, 1000, cb);

    // reg3 tint.rgba == --surface-floating-island
    const Rgba tint = ColorOf(TokenId::SurfaceFloatingIsland, pair.token);
    PRISM_CHECK(Near(cb[12], tint.r, 1e-6));
    PRISM_CHECK(Near(cb[13], tint.g, 1e-6));
    PRISM_CHECK(Near(cb[14], tint.b, 1e-6));
    PRISM_CHECK(Near(cb[15], tint.a, 1e-6));

    // reg4 specular.rgba == --border-specular-rim top edge
    const Gradient rim = GradientOf(TokenId::BorderSpecularRim, pair.token);
    PRISM_CHECK(Near(cb[16], rim.from.r, 1e-6));
    PRISM_CHECK(Near(cb[17], rim.from.g, 1e-6));
    PRISM_CHECK(Near(cb[18], rim.from.b, 1e-6));
    PRISM_CHECK(Near(cb[19], rim.from.a, 1e-6));

    const auto registers = prism::theme::GlassRegisters(pair.token);
    PRISM_CHECK(Near(registers.tint.a, cb[15], 1e-6));
    PRISM_CHECK(Near(registers.specular.a, cb[19], 1e-6));

    for (int i = 0; i < 20; ++i) PRISM_CHECK(std::isfinite(cb[i]));
  }
}

PRISM_TEST("negative controls are inert in a clean build (D3 self-check)") {
  PRISM_CHECK(prism::theme::mutation::ActiveMutation().empty());
}

}  // namespace

int main(int argc, char** argv) {
  const std::string_view active = prism::theme::mutation::ActiveMutation();
  if (!active.empty()) {
    std::cout << "C15 MUTANT ACTIVE: " << active << std::endl;
  }
  const int result = prism::test::RunAll(argc, argv);
  if (result == 0) {
    std::cout << "ALL THEME-TOKEN CHECKS PASSED" << std::endl;
  }
  return result;
}
