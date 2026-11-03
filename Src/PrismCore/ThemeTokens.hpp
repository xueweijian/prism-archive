// PrismArchive -- PrismCore
// Design tokens, blueprint ch. 3.3 -- THE WHOLE TABLE, as code.
//
// The blueprint pins seven rows by name/value, once for Daylight Glass and once
// for Midnight Obsidian Glass. They are constants here, not stringly-typed
// lookups scattered through the renderer, because a token that only exists in
// a design doc drifts away from the pixels at the first refactor.
//
//   --surface-canvas-base        rgba(246,248,252,.72) | rgba(18,20,26,.68)
//   --surface-floating-island    rgba(255,255,255,.58) | rgba(38,42,54,.52)
//   --border-specular-rim        top/bottom alpha .85->.18 | .28->.04
//   --accent-liquid-primary      #007AFF->#0055D4 @.88 | #0A84FF->#0060DF @.82
//   --font-stack-system          SF Pro Display/Inter + PingFang/HarmonyOS
//   --spring-snappy-interactive  m=1 k=420 c=28
//   --spring-fluid-sheet         m=1 k=280 c=34
//
// Two things the table itself does not spell out, marked EXT below:
//   * --text-primary / --text-secondary -- the two foreground roles the WCAG
//     gate needs to pin (body >= 4.5, secondary >= 3.0);
//   * --surface-backdrop-reference -- the opaque wallpaper reference a
//     translucent rgba() surface is composited over before its contrast is
//     measured. Contrast on a raw alpha channel is not a number.
//
// LOOKUP AND THE FALLBACK CHAIN
// Lookup is by token NAME (the CSS custom property spelling, with or without
// the leading "--"). Resolution walks: requested variant -> Midnight (the dark
// sibling) -> Daylight base. HighContrast is therefore ADDITIVE: it overrides
// the surfaces/text it must and inherits the rest instead of maintaining a
// second, silently divergent copy. Every result reports the variant it actually
// came from and whether it fell back, so a missing row can never masquerade as
// a rendered value. An unknown name resolves to an unmissable magenta sentinel
// with known=false -- never black-on-black.
//
// GLASS CONTRACT (CONTRACTS A6): the two glass tokens are the *same numbers*
// GlassIslandScene::WriteCbuffer writes into reg3 (tint) / reg4 (specular rim).
// There is exactly ONE cbuffer layout -- 5 registers, 20 floats, 80 bytes --
// and Tests/test_theme_tokens.cpp pins both themes against it. No second
// layout is invented here.
//
// Negative-control hooks (compile-time, NEVER a text edit -- CONTRACTS D3):
//   PRISM_C15_MUTANT_SWAP_THEME       swap Daylight <-> Midnight resolution
//   PRISM_C15_MUTANT_NO_FALLBACK      stop after the requested variant
//   PRISM_C15_MUTANT_LINEAR_RAW       luminance without the sRGB de-gamma
//   PRISM_C15_MUTANT_CIRCLE_EXPONENT  squircle n=2.0 instead of 4.2
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace prism::theme {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------
enum class Variant : uint8_t { Daylight, Midnight, HighContrast };
enum class Kind : uint8_t { Color, Gradient, Spring, Font };
enum class TokenId : uint8_t {
  SurfaceCanvasBase,
  SurfaceFloatingIsland,
  BorderSpecularRim,
  AccentLiquidPrimary,
  FontStackSystem,
  SpringSnappyInteractive,
  SpringFluidSheet,
  TextPrimary,               // EXT: WCAG foreground role
  TextSecondary,             // EXT: WCAG secondary role
  SurfaceBackdropReference,  // EXT: opaque compositing reference
  // C77 EXT: the three numbers a glass PANEL is made of. They are tokens, not
  // literals at the call site, for the same reason the surfaces are: a second
  // colour source is how the chrome drifts away from the design table.
  SurfaceBackdropGhost,      // refracted content: colour = the neutral it is
                             // mixed toward, alpha = the refraction STRENGTH
  SurfaceBackdropVeil,       // desaturation wash drawn over the ghost
  ShadowFloatingPanel,       // drop shadow: colour + peak alpha
  // C79 chrome tokens. The sidebar/tab-bar/status-bar surfaces used to be
  // resolved by hardcoded alphas inside GalleryRenderer; each one is now a row
  // in this table, in all three variants, so HighContrast cannot inherit a
  // wash that is invisible on black.
  SelectionTint,      // sidebar selected-row wash (accent @ ~14%)
  SurfaceTabActive,   // raised tab plate (brighter than the canvas)
  SurfaceTabHover,    // 4%-lift hover wash for an inactive tab
  DividerSubtle,      // 1 px status-bar divider (alpha .08)
  ShadowAmbient,      // large-radius low-alpha depth under a raised surface
  Count
};

inline constexpr std::size_t kTokenCount = static_cast<std::size_t>(TokenId::Count);

inline constexpr std::string_view kTokenNames[] = {
    "--surface-canvas-base",
    "--surface-floating-island",
    "--border-specular-rim",
    "--accent-liquid-primary",
    "--font-stack-system",
    "--spring-snappy-interactive",
    "--spring-fluid-sheet",
    "--text-primary",
    "--text-secondary",
    "--surface-backdrop-reference",
    "--surface-backdrop-ghost",
    "--surface-backdrop-veil",
    "--shadow-floating-panel",
    "--selection-tint",
    "--surface-tab-active",
    "--surface-tab-hover",
    "--divider-subtle",
    "--shadow-ambient",
};
static_assert(sizeof(kTokenNames) / sizeof(kTokenNames[0]) == kTokenCount,
              "every TokenId needs exactly one CSS name");

inline constexpr Kind kTokenKinds[] = {
    Kind::Color,    Kind::Color, Kind::Gradient, Kind::Gradient,
    Kind::Font,     Kind::Spring, Kind::Spring,  Kind::Color,
    Kind::Color,    Kind::Color, Kind::Color,    Kind::Color,
    Kind::Color,
    Kind::Color,    Kind::Color,  Kind::Gradient, Kind::Gradient,
    Kind::Font,     Kind::Spring, Kind::Spring,   Kind::Color,
    Kind::Color,    Kind::Color,  Kind::Color,    Kind::Color,
    Kind::Color,    Kind::Color,  Kind::Color,
};
static_assert(sizeof(kTokenKinds) / sizeof(kTokenKinds[0]) == kTokenCount,
              "every TokenId needs exactly one kind");

inline constexpr std::string_view VariantName(Variant v) {
  switch (v) {
    case Variant::Daylight: return "daylight";
    case Variant::Midnight: return "midnight";
    case Variant::HighContrast: return "high-contrast";
  }
  return "unknown";
}

inline constexpr std::string_view TokenName(TokenId id) {
  return id == TokenId::Count ? std::string_view{}
                              : kTokenNames[static_cast<std::size_t>(id)];
}

inline constexpr Kind KindOf(TokenId id) {
  return id == TokenId::Count ? Kind::Color
                              : kTokenKinds[static_cast<std::size_t>(id)];
}

inline std::ostream& operator<<(std::ostream& os, Variant v) {
  return os << VariantName(v);
}
inline std::ostream& operator<<(std::ostream& os, TokenId id) {
  return os << TokenName(id);
}

// ---------------------------------------------------------------------------
// Value shapes
// ---------------------------------------------------------------------------
struct Rgba {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float a = 1.0f;

  constexpr Rgba() = default;
  constexpr Rgba(float rr, float gg, float bb, float aa)
      : r(rr), g(gg), b(bb), a(aa) {}
};

// Two-stop gradient. For --border-specular-rim, from = top edge, to = bottom
// edge; for --accent-liquid-primary, from/to are the linear gradient ends.
struct Gradient {
  Rgba from{};
  Rgba to{};

  constexpr Gradient() = default;
  constexpr Gradient(Rgba f, Rgba t) : from(f), to(t) {}
};

struct SpringSpec {
  double mass = 1.0;
  double stiffness = 1.0;
  double damping = 1.0;

  constexpr SpringSpec() = default;
  constexpr SpringSpec(double m, double k, double c)
      : mass(m), stiffness(k), damping(c) {}

  // zeta = c / (2 * sqrt(k * m)); < 1 underdamped, >= 1 overdamped.
  double DampingRatio() const {
    return damping / (2.0 * std::sqrt(stiffness * mass));
  }
};

struct FontStack {
  std::string_view latin{};  // Western family list, in priority order
  std::string_view cjk{};    // CJK family list, in priority order
  int weightDelta = 0;       // Dark Mode: -20, stops glyph glow bloom
  bool tabularNumbers = false;  // OpenType tnum for size/ratio/offset columns

  constexpr FontStack() = default;
  constexpr FontStack(std::string_view l, std::string_view c, int w, bool t)
      : latin(l), cjk(c), weightDelta(w), tabularNumbers(t) {}
};

// One resolved token. Exactly one of color/gradient/spring/font is meaningful,
// selected by kind. The source field is the variant whose row actually answered
// and fellBack is true when that is not the requested variant.
struct TokenValue {
  TokenId id = TokenId::Count;
  Kind kind = Kind::Color;
  bool known = false;
  bool fellBack = true;
  Variant source = Variant::Daylight;
  Rgba color{};
  Gradient gradient{};
  SpringSpec spring{};
  FontStack font{};
};

inline constexpr float ChannelFromByte(uint8_t v) {
  return static_cast<float>(v) / 255.0f;
}

// Missing-token sentinel: magenta is impossible in this palette on purpose.
inline constexpr Rgba kUnknownFallbackColor{1.0f, 0.0f, 1.0f, 1.0f};

// ---------------------------------------------------------------------------
// Compile-time negative-control switches (CONTRACTS D3). Each one is compiled
// into a separate test binary registered with WILL_FAIL, so a mutation that
// stopped being observable turns CTest red instead of looking sharp.
// ---------------------------------------------------------------------------
namespace mutation {
#if defined(PRISM_C15_MUTANT_SWAP_THEME)
inline constexpr bool kSwapTheme = true;
#else
inline constexpr bool kSwapTheme = false;
#endif
#if defined(PRISM_C15_MUTANT_NO_FALLBACK)
inline constexpr bool kNoFallback = true;
#else
inline constexpr bool kNoFallback = false;
#endif
#if defined(PRISM_C15_MUTANT_LINEAR_RAW)
inline constexpr bool kRawSrgbLuminance = true;
#else
inline constexpr bool kRawSrgbLuminance = false;
#endif
#if defined(PRISM_C15_MUTANT_CIRCLE_EXPONENT)
inline constexpr bool kCircleExponent = true;
#else
inline constexpr bool kCircleExponent = false;
#endif

inline constexpr std::string_view ActiveMutation() {
  if (kSwapTheme) return "swap-theme";
  if (kNoFallback) return "no-fallback";
  if (kRawSrgbLuminance) return "linear-raw";
  if (kCircleExponent) return "circle-exponent";
  return {};
}
}  // namespace mutation

// ---------------------------------------------------------------------------
// The table. Daylight rows are the base; Midnight/HighContrast rows override.
// Values are the blueprint's rgba()/hex numbers normalised to 0..1.
// ---------------------------------------------------------------------------
namespace detail {

struct ColorRow {
  TokenId id;
  Variant variant;
  Rgba value;
};
struct GradientRow {
  TokenId id;
  Variant variant;
  Gradient value;
};
struct SpringRow {
  TokenId id;
  Variant variant;
  SpringSpec value;
};
struct FontRow {
  TokenId id;
  Variant variant;
  FontStack value;
};

// --surface-canvas-base, --surface-floating-island, the two EXT text roles and
// the EXT backdrop reference.
inline constexpr ColorRow kColorRows[] = {
    // Daylight (blueprint 3.3 verbatim)
    {TokenId::SurfaceCanvasBase, Variant::Daylight,
     Rgba(ChannelFromByte(246), ChannelFromByte(248), ChannelFromByte(252), 0.72f)},
    {TokenId::SurfaceFloatingIsland, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.58f)},
    {TokenId::TextPrimary, Variant::Daylight,
     Rgba(ChannelFromByte(29), ChannelFromByte(29), ChannelFromByte(31), 1.0f)},
    {TokenId::TextSecondary, Variant::Daylight,
     Rgba(ChannelFromByte(110), ChannelFromByte(110), ChannelFromByte(115), 1.0f)},
    {TokenId::SurfaceBackdropReference, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 1.0f)},
    // C77: the backdrop ghost is the CANVAS colour (the neutral the refracted
    // content is washed toward) at the refraction STRENGTH the design asks for.
    // The ghost is laid down whole (alpha 1.0) and the VEIL is what decides how
    // much of it survives: .65 leaves 35 % of the chroma -- "desaturate to
    // ~0.35", and it is the veil (not a second alpha) that the pixel suite
    // measures, because a veil is a colour and an alpha is not.
    {TokenId::SurfaceBackdropGhost, Variant::Daylight,
     Rgba(ChannelFromByte(246), ChannelFromByte(248), ChannelFromByte(252), 1.0f)},
    {TokenId::SurfaceBackdropVeil, Variant::Daylight,
     Rgba(ChannelFromByte(246), ChannelFromByte(248), ChannelFromByte(252), 0.65f)},
    {TokenId::ShadowFloatingPanel, Variant::Daylight,
     Rgba(0.0f, 0.0f, 0.0f, 0.18f)},
    // Midnight (blueprint 3.3 verbatim)
    {TokenId::SurfaceCanvasBase, Variant::Midnight,
     Rgba(ChannelFromByte(18), ChannelFromByte(20), ChannelFromByte(26), 0.68f)},
    {TokenId::SurfaceFloatingIsland, Variant::Midnight,
     Rgba(ChannelFromByte(38), ChannelFromByte(42), ChannelFromByte(54), 0.52f)},
    {TokenId::TextPrimary, Variant::Midnight,
     Rgba(ChannelFromByte(245), ChannelFromByte(245), ChannelFromByte(247), 1.0f)},
    {TokenId::TextSecondary, Variant::Midnight,
     Rgba(ChannelFromByte(161), ChannelFromByte(161), ChannelFromByte(166), 1.0f)},
    {TokenId::SurfaceBackdropReference, Variant::Midnight,
     Rgba(ChannelFromByte(11), ChannelFromByte(12), ChannelFromByte(16), 1.0f)},
    {TokenId::SurfaceBackdropGhost, Variant::Midnight,
     Rgba(ChannelFromByte(11), ChannelFromByte(12), ChannelFromByte(16), 1.0f)},
    {TokenId::SurfaceBackdropVeil, Variant::Midnight,
     Rgba(ChannelFromByte(11), ChannelFromByte(12), ChannelFromByte(16), 0.65f)},
    {TokenId::ShadowFloatingPanel, Variant::Midnight,
     Rgba(0.0f, 0.0f, 0.0f, 0.18f)},
    // High contrast (EXT a11y variant: opaque, maximum separation)
    {TokenId::SurfaceCanvasBase, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 1.0f)},
    {TokenId::SurfaceFloatingIsland, Variant::HighContrast,
     Rgba(ChannelFromByte(28), ChannelFromByte(28), ChannelFromByte(30), 1.0f)},
    {TokenId::TextPrimary, Variant::HighContrast,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 1.0f)},
    {TokenId::TextSecondary, Variant::HighContrast,
     Rgba(ChannelFromByte(229), ChannelFromByte(229), ChannelFromByte(234), 1.0f)},
    {TokenId::SurfaceBackdropReference, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 1.0f)},
    // High contrast: the refraction is still THERE (turning it off would make
    // the setting a lie) but the veil is nearly opaque, so the ghost can never
    // compete with a glyph. The shadow is pure black and full strength.
    {TokenId::SurfaceBackdropGhost, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 0.55f)},
    {TokenId::SurfaceBackdropVeil, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 0.82f)},
    {TokenId::ShadowFloatingPanel, Variant::HighContrast,
     Rgba(0.0f, 0.0f, 0.0f, 0.30f)},

    // C79 chrome roles -- one row per variant, no inherited wash. Daylight and
    // Midnight keep the Liquid Glass materials (semi-opaque accent/surface);
    // HighContrast is opaque enough to stay legible on pure black.
    // Sidebar selected row: the accent at 14% (blueprint 3.2's wash).
    {TokenId::SelectionTint, Variant::Daylight,
     Rgba(ChannelFromByte(0), ChannelFromByte(122), ChannelFromByte(255), 0.14f)},
    {TokenId::SelectionTint, Variant::Midnight,
     Rgba(ChannelFromByte(10), ChannelFromByte(132), ChannelFromByte(255), 0.16f)},
    {TokenId::SelectionTint, Variant::HighContrast,
     Rgba(ChannelFromByte(102), ChannelFromByte(179), ChannelFromByte(255), 0.24f)},
    // Raised tab plate: brighter than the canvas, still a translucent material
    // so the backdrop's colour shows through (Daylight/Midnight).
    {TokenId::SurfaceTabActive, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.85f)},
    {TokenId::SurfaceTabActive, Variant::Midnight,
     Rgba(ChannelFromByte(58), ChannelFromByte(64), ChannelFromByte(80), 0.88f)},
    {TokenId::SurfaceTabActive, Variant::HighContrast,
     Rgba(ChannelFromByte(40), ChannelFromByte(40), ChannelFromByte(44), 1.0f)},
    // Inactive-tab hover: a ~4% lift, never a second plate.
    {TokenId::SurfaceTabHover, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.32f)},
    {TokenId::SurfaceTabHover, Variant::Midnight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.08f)},
    {TokenId::SurfaceTabHover, Variant::HighContrast,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.20f)},
    // 1 px status divider: alpha .08 (HighContrast raises it so the rule is not
    // lost between two opaque clusters).
    {TokenId::DividerSubtle, Variant::Daylight,
     Rgba(ChannelFromByte(29), ChannelFromByte(29), ChannelFromByte(31), 0.08f)},
    {TokenId::DividerSubtle, Variant::Midnight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.08f)},
    {TokenId::DividerSubtle, Variant::HighContrast,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.40f)},
    // Depth: a large-radius, low-alpha shadow token (no literal black).
    {TokenId::ShadowAmbient, Variant::Daylight,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 0.10f)},
    {TokenId::ShadowAmbient, Variant::Midnight,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 0.40f)},
    {TokenId::ShadowAmbient, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 0.70f)},
};

// --border-specular-rim and --accent-liquid-primary.
inline constexpr GradientRow kGradientRows[] = {
    {TokenId::BorderSpecularRim, Variant::Daylight,
     Gradient(Rgba(1.0f, 1.0f, 1.0f, 0.85f), Rgba(1.0f, 1.0f, 1.0f, 0.18f))},
    {TokenId::AccentLiquidPrimary, Variant::Daylight,
     Gradient(Rgba(ChannelFromByte(0), ChannelFromByte(122), ChannelFromByte(255), 0.88f),
              Rgba(ChannelFromByte(0), ChannelFromByte(85), ChannelFromByte(212), 0.88f))},
    {TokenId::BorderSpecularRim, Variant::Midnight,
     Gradient(Rgba(1.0f, 1.0f, 1.0f, 0.28f), Rgba(1.0f, 1.0f, 1.0f, 0.04f))},
    {TokenId::AccentLiquidPrimary, Variant::Midnight,
     Gradient(Rgba(ChannelFromByte(10), ChannelFromByte(132), ChannelFromByte(255), 0.82f),
              Rgba(ChannelFromByte(0), ChannelFromByte(96), ChannelFromByte(223), 0.82f))},
    {TokenId::BorderSpecularRim, Variant::HighContrast,
     Gradient(Rgba(1.0f, 1.0f, 1.0f, 0.95f), Rgba(1.0f, 1.0f, 1.0f, 0.45f))},
    {TokenId::AccentLiquidPrimary, Variant::HighContrast,
     Gradient(Rgba(ChannelFromByte(102), ChannelFromByte(179), ChannelFromByte(255), 1.0f),
              Rgba(ChannelFromByte(51), ChannelFromByte(136), ChannelFromByte(255), 1.0f))},
};

// Physics is variant-independent: the blueprint carries the same m/k/c in both
// columns, so Midnight/HighContrast legitimately inherit these rows.
inline constexpr SpringRow kSpringRows[] = {
    {TokenId::SpringSnappyInteractive, Variant::Daylight, SpringSpec(1.0, 420.0, 28.0)},
    {TokenId::SpringFluidSheet, Variant::Daylight, SpringSpec(1.0, 280.0, 34.0)},
};

// --font-stack-system: same families both themes; Dark Mode shifts weight -20.
// HighContrast deliberately has no row and inherits Midnight's (a legible
// fallback chain, not a second copy).
inline constexpr FontStack kSystemFontDaylight{
    "SF Pro Display, Inter", "PingFang SC, HarmonyOS Sans SC", 0, true};
inline constexpr FontStack kSystemFontMidnight{
    "SF Pro Display, Inter", "PingFang SC, HarmonyOS Sans SC", -20, true};
inline constexpr FontRow kFontRows[] = {
    {TokenId::FontStackSystem, Variant::Daylight, kSystemFontDaylight},
    {TokenId::FontStackSystem, Variant::Midnight, kSystemFontMidnight},
};

template <class Row, std::size_t N>
inline const Row* FindRow(const Row (&rows)[N], TokenId id, Variant variant) {
  for (std::size_t i = 0; i < N; ++i) {
    if (rows[i].id == id && rows[i].variant == variant) return &rows[i];
  }
  return nullptr;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// Fallback chain: requested variant -> Midnight (dark sibling) -> Daylight.
// ---------------------------------------------------------------------------
inline constexpr Variant kChainDaylight[] = {Variant::Daylight};
inline constexpr Variant kChainMidnight[] = {Variant::Midnight, Variant::Daylight};
inline constexpr Variant kChainHighContrast[] = {Variant::HighContrast, Variant::Midnight,
                                                 Variant::Daylight};

inline const Variant* FallbackChain(Variant v, std::size_t& length) {
  switch (v) {
    case Variant::Daylight:
      length = 1;
      return kChainDaylight;
    case Variant::Midnight:
      length = 2;
      return kChainMidnight;
    case Variant::HighContrast:
      length = 3;
      return kChainHighContrast;
  }
  length = 1;
  return kChainDaylight;
}

inline TokenValue Lookup(TokenId id, Variant variant) {
  TokenValue out;
  out.id = id;
  out.kind = KindOf(id);
  out.known = id != TokenId::Count;

  Variant requested = variant;
  if (mutation::kSwapTheme) {
    if (requested == Variant::Daylight) {
      requested = Variant::Midnight;
    } else if (requested == Variant::Midnight) {
      requested = Variant::Daylight;
    }
  }

  std::size_t chainLength = 0;
  const Variant* chain = FallbackChain(requested, chainLength);
  if (mutation::kNoFallback && chainLength > 1) chainLength = 1;

  if (out.known) {
    for (std::size_t i = 0; i < chainLength; ++i) {
      const Variant candidate = chain[i];
      switch (out.kind) {
        case Kind::Color: {
          const detail::ColorRow* row =
              detail::FindRow(detail::kColorRows, id, candidate);
          if (row != nullptr) {
            out.color = row->value;
            out.source = candidate;
            out.fellBack = candidate != requested;
            return out;
          }
          break;
        }
        case Kind::Gradient: {
          const detail::GradientRow* row =
              detail::FindRow(detail::kGradientRows, id, candidate);
          if (row != nullptr) {
            out.gradient = row->value;
            out.source = candidate;
            out.fellBack = candidate != requested;
            return out;
          }
          break;
        }
        case Kind::Spring: {
          const detail::SpringRow* row =
              detail::FindRow(detail::kSpringRows, id, candidate);
          if (row != nullptr) {
            out.spring = row->value;
            out.source = candidate;
            out.fellBack = candidate != requested;
            return out;
          }
          break;
        }
        case Kind::Font: {
          const detail::FontRow* row =
              detail::FindRow(detail::kFontRows, id, candidate);
          if (row != nullptr) {
            out.font = row->value;
            out.source = candidate;
            out.fellBack = candidate != requested;
            return out;
          }
          break;
        }
      }
    }
  }

  // No row answered (or the id itself is unknown): visible sentinel, never a
  // guessed value. known=false is the signal the caller must not render it.
  out.known = false;
  out.fellBack = true;
  out.source = Variant::Daylight;
  out.color = kUnknownFallbackColor;
  return out;
}

// CSS spelling with or without the leading "--" is accepted.
inline std::string_view NormalizeTokenName(std::string_view name) {
  while (!name.empty() && name.front() == '-') name.remove_prefix(1);
  return name;
}

inline TokenId FindToken(std::string_view name) {
  const std::string_view normalized = NormalizeTokenName(name);
  for (std::size_t i = 0; i < kTokenCount; ++i) {
    if (NormalizeTokenName(kTokenNames[i]) == normalized) {
      return static_cast<TokenId>(i);
    }
  }
  return TokenId::Count;
}

inline TokenValue Lookup(std::string_view name, Variant variant) {
  // Unknown names go through the same sentinel path as TokenId::Count, so an
  // unknown token can never masquerade as a rendered (e.g. black) colour.
  return Lookup(FindToken(name), variant);
}

// ---------------------------------------------------------------------------
// WCAG 2.x contrast. Surfaces here are translucent, so measure AFTER
// compositing over the backdrop reference -- alpha is a material, not a colour.
// ---------------------------------------------------------------------------
inline constexpr double kWcagBodyTextMinRatio = 4.5;
inline constexpr double kWcagSecondaryTextMinRatio = 3.0;

inline double SrgbToLinear(double channel) {
  if (mutation::kRawSrgbLuminance) return channel;
  return channel <= 0.04045 ? channel / 12.92
                            : std::pow((channel + 0.055) / 1.055, 2.4);
}

inline double RelativeLuminance(const Rgba& color) {
  const double r = SrgbToLinear(static_cast<double>(color.r));
  const double g = SrgbToLinear(static_cast<double>(color.g));
  const double b = SrgbToLinear(static_cast<double>(color.b));
  return 0.2126 * r + 0.7152 * g + 0.0722 * b;
}

inline Rgba CompositeOver(const Rgba& top, const Rgba& bottom) {
  const double ta = static_cast<double>(top.a);
  const double ba = static_cast<double>(bottom.a);
  const double outA = ta + ba * (1.0 - ta);
  if (!(outA > 0.0)) return Rgba(0.0f, 0.0f, 0.0f, 0.0f);
  const auto channel = [ta, ba, outA](float t, float b) {
    return static_cast<float>((static_cast<double>(t) * ta +
                               static_cast<double>(b) * ba * (1.0 - ta)) /
                              outA);
  };
  return Rgba(channel(top.r, bottom.r), channel(top.g, bottom.g),
              channel(top.b, bottom.b), static_cast<float>(outA));
}

inline double ContrastRatio(const Rgba& a, const Rgba& b) {
  const double la = RelativeLuminance(a);
  const double lb = RelativeLuminance(b);
  const double hi = (std::max)(la, lb);
  const double lo = (std::min)(la, lb);
  return (hi + 0.05) / (lo + 0.05);
}

// ---------------------------------------------------------------------------
// Squircle / G2 continuous corner (blueprint 3.3 item 2): |x/a|^n + |y/b|^n = 1
// with n = 4.2. Field is the normalised implicit q-1 (negative inside, zero on
// the boundary); Sdf is the first-order signed distance (q-1)/|grad q|, which is
// exact along the axes and the reference for anti-aliasing / rim banding.
// ---------------------------------------------------------------------------
inline constexpr double kSquircleExponent = mutation::kCircleExponent ? 2.0 : 4.2;

inline double SquircleField(double x, double y, double halfW, double halfH,
                            double exponent = kSquircleExponent) {
  if (!(halfW > 0.0) || !(halfH > 0.0)) return std::hypot(x, y);
  const double ax = std::abs(x) / halfW;
  const double ay = std::abs(y) / halfH;
  const double q =
      std::pow(std::pow(ax, exponent) + std::pow(ay, exponent), 1.0 / exponent);
  return q - 1.0;
}

inline double SquircleSdf(double x, double y, double halfW, double halfH,
                          double exponent = kSquircleExponent) {
  if (!(halfW > 0.0) || !(halfH > 0.0)) return std::hypot(x, y);
  const double ax = std::abs(x) / halfW;
  const double ay = std::abs(y) / halfH;
  const double sx = std::pow(ax, exponent);
  const double sy = std::pow(ay, exponent);
  const double s = sx + sy;
  if (!(s > 0.0)) return -(std::min)(halfW, halfH);
  const double q = std::pow(s, 1.0 / exponent);
  // dq/dx = |x|^(n-1) * sign(x) / a^n * s^(1/n - 1)  (same for y).
  const double common = std::pow(s, 1.0 / exponent - 1.0);
  const double gx = (x < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(x), exponent - 1.0) /
                    std::pow(halfW, exponent) * common;
  const double gy = (y < 0.0 ? -1.0 : 1.0) * std::pow(std::abs(y), exponent - 1.0) /
                    std::pow(halfH, exponent) * common;
  const double gradient = std::hypot(gx, gy);
  if (!(gradient > 0.0)) return -(std::min)(halfW, halfH);
  return (q - 1.0) / gradient;
}

// ---------------------------------------------------------------------------
// The one LiquidGlassConstants layout (CONTRACTS A6). Registers:
//   reg0 center.xy | halfSize.xy   reg1 pointer.xy | texel.xy
//   reg2 corner | bevel | ior | sat  reg3 tint.rgba  reg4 specular.rgba
// GlassIslandScene::WriteCbuffer fills all 20 floats; these tokens feed reg3 and
// reg4, and the test pins the equality for both themes.
// ---------------------------------------------------------------------------
inline constexpr std::size_t kLiquidGlassCbufferFloats = 20;
inline constexpr std::size_t kLiquidGlassCbufferBytes =
    sizeof(float) * kLiquidGlassCbufferFloats;
static_assert(kLiquidGlassCbufferBytes == 80,
              "LiquidGlassConstants is 5 registers / 20 floats / 80 bytes (A6)");

struct LiquidGlassTokenRegisters {
  Rgba tint;      // reg3 tint.rgba
  Rgba specular;  // reg4 specular.rgba (rim top-edge alpha)
};

// HighContrast is a UI overlay role; GlassIslandScene models Daylight/Midnight
// only. Mapping all three keeps the token API total, but the cbuffer authority
// remains WriteCbuffer (which cannot express HighContrast yet).
inline LiquidGlassTokenRegisters GlassRegisters(Variant variant) {
  return {Lookup(TokenId::SurfaceFloatingIsland, variant).color,
          Lookup(TokenId::BorderSpecularRim, variant).gradient.from};
}

// ---------------------------------------------------------------------------
// C71: LIVE appearance overrides.
//
// appearance.accentColor and appearance.glassIntensity used to be schema-only
// keys: the store kept them, no renderer ever read them. They are applied here
// (the ONE place a pixel colour is resolved) and consumed by ActionBar,
// GalleryRenderer and GlassIslandScene::WriteCbuffer. The app calls
// prism::appearance::Apply(store) once at startup and after a settings save.
//
// WHY THE GLASS INTENSITY IS RELATIVE TO 0.72
// The schema's documented default is 0.72, and before this card the renderer
// used the token alpha verbatim. Treating 0.72 as 1.0 (a REFERENCE, not a
// multiplier) is what keeps an upgraded install pixel-identical at its default;
// a raw multiplier would have silently redrawn every window on first launch.
// So the effective alpha is token.a * (intensity / 0.72), clamped to 1.
// ---------------------------------------------------------------------------
namespace live {

inline constexpr double kReferenceGlassIntensity = 0.72;

struct Overrides {
  bool hasAccent = false;
  Rgba accent{};
  double glassIntensity = kReferenceGlassIntensity;
  // C77: appearance.backdropRefraction. ON by default, because the backdrop
  // layer is the thing that makes a translucent panel read as glass at all --
  // a switch that shipped off would have shipped the pre-C77 look.
  bool backdropRefraction = true;
};

inline Overrides& Current() {
  static Overrides overrides;
  return overrides;
}

inline void SetAccent(Rgba color) {
  Current().hasAccent = true;
  Current().accent = color;
}

inline void ClearAccent() { Current().hasAccent = false; }

inline void SetGlassIntensity(double intensity) {
  // The schema already clamps to [0,1]; this is the belt for a TooNew document
  // this build must read but not validate.
  if (!(intensity >= 0.0)) intensity = 0.0;
  if (intensity > 1.0) intensity = 1.0;
  Current().glassIntensity = intensity;
}

inline void SetBackdropRefraction(bool enabled) { Current().backdropRefraction = enabled; }

inline void Reset() { Current() = Overrides{}; }

#if defined(PRISM_C71_MUTANT_IGNORE_ACCENT)
// NEGATIVE CONTROL (CONTRACTS D3): the override is compiled out, so a saved
// accent never reaches a pixel. The counter proves the injected branch ran; the
// suite's verdict is armed on it (D27).
inline int& MutantAccentHits() {
  static int hits = 0;
  return hits;
}
#endif
#if defined(PRISM_C71_MUTANT_IGNORE_GLASS)
// NEGATIVE CONTROL (CONTRACTS D3): glass intensity is ignored.
inline int& MutantGlassHits() {
  static int hits = 0;
  return hits;
}
#endif

}  // namespace live

// The accent a renderer must use: the user's saved colour when one was applied,
// otherwise the theme token (exactly the pre-C71 behaviour).
inline Rgba EffectiveAccent(Variant variant) {
#if defined(PRISM_C71_MUTANT_IGNORE_ACCENT)
  ++live::MutantAccentHits();
  return Lookup(TokenId::AccentLiquidPrimary, variant).gradient.from;
#else
  const live::Overrides& overrides = live::Current();
  if (overrides.hasAccent) return overrides.accent;
  return Lookup(TokenId::AccentLiquidPrimary, variant).gradient.from;
#endif
}

// The multiplier appearance.glassIntensity represents, relative to the schema's
// 0.72 reference (1.0 == "do not change anything").
inline double GlassIntensityScale() {
  return live::Current().glassIntensity / live::kReferenceGlassIntensity;
}

// C77: the refraction layer. Renderers ask THIS, never a local bool, so the
// setting and the pixels cannot disagree.
inline bool BackdropRefractionEnabled() { return live::Current().backdropRefraction; }

// The floating-island surface with the saved glass intensity folded into its
// alpha. Every consumer that paints "glass" must come through here; a literal
// .52/.58 at a call site is the defect this function exists to prevent.
inline Rgba EffectiveGlassSurface(Variant variant) {
  Rgba surface = Lookup(TokenId::SurfaceFloatingIsland, variant).color;
#if defined(PRISM_C71_MUTANT_IGNORE_GLASS)
  ++live::MutantGlassHits();
#else
  double alpha = static_cast<double>(surface.a) * GlassIntensityScale();
  if (alpha < 0.0) alpha = 0.0;
  if (alpha > 1.0) alpha = 1.0;
  surface.a = static_cast<float>(alpha);
#endif
  return surface;
}

}  // namespace prism::theme

