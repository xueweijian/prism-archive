// PrismArchive -- PrismCore
// Design tokens, blueprint ch. 3.3 -- THE WHOLE TABLE, as code.
//
// The blueprint pins seven rows by name/value, once for Daylight Glass and once
// for Midnight Obsidian Glass. They are constants here, not stringly-typed
// lookups scattered through the renderer, because a token that only exists in
// a design doc drifts away from the pixels at the first refactor.
//
//   --surface-canvas-base        rgba(241,241,244,.72) | #0B0D12 opaque
//   --surface-floating-island    rgba(255,255,255,.82) | rgba(43,49,62,.75)
//   --border-specular-rim        top/bottom alpha .85->.18 | .28->.04
//   --accent-liquid-primary      #0A84FF flat @.88 | #0A84FF flat @.82
//   --font-stack-system          SF Pro Display/Inter + PingFang/HarmonyOS
//   --spring-snappy-interactive  m=1 k=420 c=28
//   --spring-fluid-sheet         m=1 k=280 c=34
//
// C124 replaced the eight ad-hoc surface tones those first two rows used to
// imply with an explicit FIVE-STEP LADDER (SurfaceTier below: canvas / panel /
// card / floating / border). The table above lists the rows the blueprint names;
// the ladder is the vocabulary the renderer paints, and it is where "which
// surface is this" is decided (UI-AUDIT 12.2 counted eleven tones and the
// hierarchy was backwards).
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
  // C124: THE LADDER'S THREE MISSING TIERS. Before this card the rail, the tab
  // plate, the Quick Look sheet and the tiles each invented a tone; the audit
  // counted ELEVEN. The five tiers below (canvas / panel / card / floating /
  // border) are the whole vocabulary -- see SurfaceTier.
  SurfacePanel,
  SurfaceCard,
  SurfaceBorder,
  // C124: THE FOLDER IS A MATERIAL, NOT A TINT. It used to be the ACCENT mixed
  // toward the panel, i.e. the fourth blue in a product that should have one
  // (UI-AUDIT 2.2: #2480DF, 210-214 degrees, the same hue as the selection bar
  // AND the tab underline). Gold is what makes "folder" readable without
  // competing with "selected" (DESIGN-PROPOSAL 12.5: 1.61:1 on white).
  FolderHighlight,
  FolderBody,
  FolderShadow,
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
    "--surface-panel",
    "--surface-card",
    "--surface-border",
    "--folder-highlight",
    "--folder-body",
    "--folder-shadow",
};
static_assert(sizeof(kTokenNames) / sizeof(kTokenNames[0]) == kTokenCount,
              "every TokenId needs exactly one CSS name");

inline constexpr Kind kTokenKinds[] = {
    // One row per TokenId, in enum order (18 tokens: 12 base + C77's three
    // panel numbers + C79's five chrome surfaces).
    Kind::Color, Kind::Color, Kind::Gradient, Kind::Gradient,  // canvas, island, rim, accent
    Kind::Font, Kind::Spring, Kind::Spring,                    // font stack, two springs
    Kind::Color, Kind::Color, Kind::Color,                     // text primary/secondary, backdrop ref
    Kind::Color, Kind::Color, Kind::Color,                     // C77: ghost, veil, panel shadow
    Kind::Color, Kind::Color, Kind::Color,                     // C79: selection, tab active, tab hover
    Kind::Color, Kind::Color,                                  // C79: divider, ambient shadow
    Kind::Color, Kind::Color, Kind::Color,                     // C124: panel, card, border
    Kind::Color, Kind::Color, Kind::Color,                     // C124: folder highlight/body/shadow
};
static_assert(sizeof(kTokenKinds) / sizeof(kTokenKinds[0]) == kTokenCount,
              "every TokenId needs exactly one kind");

// ---------------------------------------------------------------------------
// C124: THE SURFACE LADDER -- five tiers instead of eleven tones.
//
// THE MEASURED DEFECT (UI-AUDIT 12.2 / DESIGN-PROPOSAL 12.7). A sample of one
// Midnight window read ELEVEN distinct surface tones, and the ordering was
// backwards: the action island (the primary control surface, the thing that
// should read as "in front") measured 1.15:1 against the canvas while the side
// rail -- a passive column -- measured 1.32:1. The card's fix is a five-step
// ladder with a fixed relationship to the canvas:
//
//   tier       Midnight   vs canvas CR   dE to the tier below
//   canvas     #0B0D12    --             --
//   panel      #12151C    1.06           4.1
//   card       #1A1E26    1.16           4.5
//   floating   #232833    1.32           5.2
//   border     #2E3340    1.54           5.3
//
// WHY THE PIXELS AND NOT THE TOKENS ARE THE SPEC. A tier is a MATERIAL: the
// floating tier is translucent (the glass), so its token is not its pixel. The
// ladder above is what the frame MEASURES after compositing, which is why the
// numbers are asserted through prism::chrome::SurfaceTierPresented() and not
// against the raw rows. TierOf() is the structural half: every token that IS a
// surface must belong to a tier, so a sixth "just this once" surface cannot be
// added without the suite going red.
// ---------------------------------------------------------------------------
enum class SurfaceTier : uint8_t { None = 0, Canvas, Panel, Card, Floating, Border };

inline constexpr std::size_t kSurfaceTierCount = 5;

inline constexpr SurfaceTier kSurfaceTiers[kSurfaceTierCount] = {
    SurfaceTier::Canvas, SurfaceTier::Panel, SurfaceTier::Card,
    SurfaceTier::Floating, SurfaceTier::Border};

inline constexpr std::string_view SurfaceTierName(SurfaceTier tier) {
  switch (tier) {
    case SurfaceTier::None: return "none";
    case SurfaceTier::Canvas: return "canvas";
    case SurfaceTier::Panel: return "panel";
    case SurfaceTier::Card: return "card";
    case SurfaceTier::Floating: return "floating";
    case SurfaceTier::Border: return "border";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, SurfaceTier tier) {
  return os << SurfaceTierName(tier);
}

inline constexpr TokenId SurfaceTierToken(SurfaceTier tier) {
  switch (tier) {
    case SurfaceTier::Canvas: return TokenId::SurfaceCanvasBase;
    case SurfaceTier::Panel: return TokenId::SurfacePanel;
    case SurfaceTier::Card: return TokenId::SurfaceCard;
    case SurfaceTier::Floating: return TokenId::SurfaceFloatingIsland;
    case SurfaceTier::Border: return TokenId::SurfaceBorder;
    case SurfaceTier::None: break;
  }
  return TokenId::Count;
}

// Which tier a surface token sits on. SurfaceTabActive is the CARD tier: a
// raised tab plate is a card, not a sixth surface (C79 shipped it as its own
// tone, which is one of the eleven). Text, washes, shadows and the backdrop
// rows are NOT surfaces and answer None.
inline constexpr SurfaceTier TierOf(TokenId id) {
  switch (id) {
    case TokenId::SurfaceCanvasBase: return SurfaceTier::Canvas;
    case TokenId::SurfacePanel: return SurfaceTier::Panel;
    case TokenId::SurfaceCard: return SurfaceTier::Card;
    case TokenId::SurfaceFloatingIsland: return SurfaceTier::Floating;
    case TokenId::SurfaceBorder: return SurfaceTier::Border;
    case TokenId::SurfaceTabActive: return SurfaceTier::Card;
    default: return SurfaceTier::None;
  }
}

// The accent line's thickness, in DIP (C120: geometry is DIP, never "pixels").
// "THE SEAM" (DESIGN-PROPOSAL 16.3): one 2 DIP accent line, in four places --
// the active tab's bottom edge, the current crumb's bottom edge, the selected
// row's left edge, the progress fill's leading edge. One element, four
// positions, so the user follows a line instead of learning a legend.
inline constexpr float kSeamThicknessDip = 2.0f;

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

// ---------------------------------------------------------------------------
// C124: the card's numbers, in ONE place, with the pre-card palette compiled in
// as the negative control (CONTRACTS D3).
//
// PRISM_C124_MUTANT_OLD_PALETTE restores the palette this card replaces: the
// 11-tone Midnight surfaces, the two-blue accent (Daylight #007AFF vs Midnight
// #0A84FF) and a folder with no gold at all. It is a COMPILE-TIME switch, never
// a text edit, so the mutation cannot drift from the code it mutates. V1 (one
// accent) and V2 (the five-step ladder) MUST go red on that arm.
// ---------------------------------------------------------------------------
#if defined(PRISM_C124_MUTANT_OLD_PALETTE)
inline constexpr bool kOldPalette = true;
#else
inline constexpr bool kOldPalette = false;
#endif

struct Tone {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  float a = 1.0f;
};

inline constexpr Rgba Paint(const Tone& tone) {
  return Rgba(ChannelFromByte(tone.r), ChannelFromByte(tone.g),
              ChannelFromByte(tone.b), tone.a);
}

// THE LADDER (DESIGN-PROPOSAL 12.7). Midnight is the theme the audit measured,
// so it is the theme the five values are pinned in. The floating tier is a
// MATERIAL: (43,49,62) at 75 % composites to exactly #232833 on the #0B0D12
// canvas -- 0.75*43 + 0.25*11 = 35, 0.75*49 + 0.25*13 = 40, 0.75*62 + 0.25*18 =
// 51 -- so the tier's MEASURED pixel is the table's hex while the glass stays
// translucent (appearance.glassIntensity must still be able to move it).
inline constexpr Tone kMidCanvas = kOldPalette ? Tone{18, 20, 26, 0.68f} : Tone{11, 13, 18, 1.0f};
inline constexpr Tone kMidPanel = kOldPalette ? Tone{38, 42, 54, 0.94f} : Tone{18, 21, 28, 1.0f};
inline constexpr Tone kMidCard = kOldPalette ? Tone{58, 64, 80, 0.88f} : Tone{26, 30, 38, 1.0f};
inline constexpr Tone kMidFloating =
    kOldPalette ? Tone{38, 42, 54, 0.52f} : Tone{43, 49, 62, 0.75f};
inline constexpr Tone kMidBorder =
    kOldPalette ? Tone{255, 255, 255, 0.16f} : Tone{46, 51, 64, 1.0f};
inline constexpr Tone kMidReference = kOldPalette ? Tone{11, 12, 16, 1.0f} : Tone{11, 13, 18, 1.0f};
inline constexpr Tone kMidVeil = kOldPalette ? Tone{16, 17, 23, 0.65f} : Tone{11, 13, 18, 0.65f};
inline constexpr Tone kMidSelection =
    kOldPalette ? Tone{10, 132, 255, 0.14f} : Tone{10, 132, 255, 0.14f};

// THE ACCENT: one value in both design themes (DESIGN-PROPOSAL 16.1 / 16.3).
// HighContrast keeps its own raised-contrast pair -- an accessibility override,
// which is the one documented reason for a second accent value.
inline constexpr Tone kDayAccent =
    kOldPalette ? Tone{0, 122, 255, 0.88f} : Tone{10, 132, 255, 0.88f};
inline constexpr Tone kDayAccentEnd =
    kOldPalette ? Tone{0, 85, 212, 0.88f} : Tone{10, 132, 255, 0.88f};
inline constexpr Tone kMidAccent =
    kOldPalette ? Tone{10, 132, 255, 0.82f} : Tone{10, 132, 255, 0.82f};
inline constexpr Tone kMidAccentEnd =
    kOldPalette ? Tone{0, 96, 223, 0.82f} : Tone{10, 132, 255, 0.82f};

// THE FOLDER (DESIGN-PROPOSAL 2.1 / 12.5): highlight, body, shadow. The same
// three tones in every theme -- a folder is a MATERIAL, and its job is to be
// recognisable at 1.61:1 on white, not to shout. Pre-card this was the accent
// mixed toward the panel, i.e. a fourth blue.
inline constexpr Tone kFolderHighlight =
    kOldPalette ? Tone{10, 132, 255, 1.0f} : Tone{255, 217, 138, 1.0f};
inline constexpr Tone kFolderBody =
    kOldPalette ? Tone{10, 132, 255, 1.0f} : Tone{255, 182, 102, 1.0f};
inline constexpr Tone kFolderShadow =
    kOldPalette ? Tone{10, 132, 255, 1.0f} : Tone{219, 144, 58, 1.0f};

// --surface-canvas-base, --surface-floating-island, the two EXT text roles and
// the EXT backdrop reference.
inline constexpr ColorRow kColorRows[] = {
    // Daylight: the plan's layered material (ui-redesign-plan 2.1) -- canvas
    // #F5F5F7, content plates #FFFFFF, floating island rgba(255,255,255,.82).
    //
    // WHY THE BASE ROW IS (241,241,244) AND NOT (245,245,247). The canvas is a
    // 72 % sheet over the opaque --surface-backdrop-reference (white), and C77's
    // veil then washes it again; the PIXEL a resting canvas region shows is the
    // presented composite, not this row. (241,241,244)@.72 over white IS (245,245,247),
    // and the veil+ghost below are the same #F5F5F7, so the presented pixel lands
    // on #F5F5F7 exactly and the flat/presented pair cannot disagree.
    // The old row, (246,248,252)@.72, presented as (247,249,252): 2..5 levels
    // lighter and blue-tinted, which is the "hospital white" the plan names.
    {TokenId::SurfaceCanvasBase, Variant::Daylight,
     Rgba(ChannelFromByte(241), ChannelFromByte(241), ChannelFromByte(244), 0.72f)},
    // #FFFFFF at .82 (plan 2.1), up from .58. At .58 the island resolved to
    // (252,252,254) against a (247,249,252) canvas -- 5 levels, i.e. no body at
    // all: only the rim and the shadow were visible ("a plastic pill on a
    // washed-out white"). .82 puts the island 8 levels off the canvas, which is
    // the card's floor, while staying a translucent material (the rim still
    // reads over the 18 % of canvas that shows through).
    {TokenId::SurfaceFloatingIsland, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 0.82f)},
    // C124: the ladder's remaining Daylight tiers. The plan (2.1) gives Daylight
    // three materials -- paper #F5F5F7, plates #FFFFFF, floating rgba(255,255,
    // 255,.82) -- so panel and card legitimately share #FFFFFF here; the DARK
    // theme is where the audit counted eleven tones, and it is where the five
    // steps are pinned.
    {TokenId::SurfacePanel, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 1.0f)},
    {TokenId::SurfaceCard, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 1.0f)},
    {TokenId::SurfaceBorder, Variant::Daylight,
     Rgba(ChannelFromByte(230), ChannelFromByte(232), ChannelFromByte(236), 1.0f)},
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
    // C102: the ghost and the veil are the canvas colour (#F5F5F7), so the
    // backdrop layer cannot tint an empty region away from the canvas the
    // renderer cleared to. They used to be (246,248,252) -- the blue-white that
    // made the veil (65 % of the pixel!) the authority on the canvas colour
    // while --surface-canvas-base was nominally something else.
    {TokenId::SurfaceBackdropGhost, Variant::Daylight,
     Rgba(ChannelFromByte(245), ChannelFromByte(245), ChannelFromByte(247), 1.0f)},
    {TokenId::SurfaceBackdropVeil, Variant::Daylight,
     Rgba(ChannelFromByte(245), ChannelFromByte(245), ChannelFromByte(247), 0.65f)},
    {TokenId::ShadowFloatingPanel, Variant::Daylight,
     Rgba(0.0f, 0.0f, 0.0f, 0.18f)},
    // Midnight: THE FIVE-STEP LADDER (C124). The canvas is OPAQUE #0B0D12 -- the
    // ladder's own canvas, and the value the audit measured (#0C0D11 +-1). It
    // used to be an 68 % sheet over #0B0C10, which C121's veil then presented as
    // (16,17,23); against THAT canvas the panel tier measures CR 1.029, i.e.
    // below the card's 1.03 floor, so the ladder cannot be built on it. The
    // veil follows the canvas (C121's invariant: the wash is the colour it
    // washes), so a resting canvas region and the refraction-on canvas agree.
    {TokenId::SurfaceCanvasBase, Variant::Midnight, Paint(kMidCanvas)},
    {TokenId::SurfacePanel, Variant::Midnight, Paint(kMidPanel)},
    {TokenId::SurfaceCard, Variant::Midnight, Paint(kMidCard)},
    {TokenId::SurfaceFloatingIsland, Variant::Midnight, Paint(kMidFloating)},
    {TokenId::SurfaceBorder, Variant::Midnight, Paint(kMidBorder)},
    {TokenId::TextPrimary, Variant::Midnight,
     Rgba(ChannelFromByte(245), ChannelFromByte(245), ChannelFromByte(247), 1.0f)},
    {TokenId::TextSecondary, Variant::Midnight,
     Rgba(ChannelFromByte(161), ChannelFromByte(161), ChannelFromByte(166), 1.0f)},
    // C124: the opaque wallpaper reference is the ladder's canvas too. The shell
    // (title bar + menu plate) composites nothing over it, so the shell pixel is
    // this row: making it the canvas is what keeps C83's "the shell and the
    // canvas are one surface" rule true by construction instead of by 2 levels.
    {TokenId::SurfaceBackdropReference, Variant::Midnight, Paint(kMidReference)},
    {TokenId::SurfaceBackdropGhost, Variant::Midnight,
     Rgba(ChannelFromByte(11), ChannelFromByte(12), ChannelFromByte(16), 1.0f)},
    // C121: THE VEIL IS THE CANVAS COLOUR, in every theme (C102 established the
    // rule for Daylight; Midnight was left on --surface-backdrop-reference and
    // broke it). The veil covers the whole window, so a veil that is DARKER than
    // the canvas darkens the canvas itself: the frame measured (16,17,23) with
    // the layer off and (13,14,18) with it on, dE 2.4 on a region the layer does
    // not even paint. That is "the glass is not chroma-neutral", and no amount
    // of source scoping fixes it -- the wash has to land on the colour it is
    // washing. (16,17,23) IS --surface-canvas-base @.72 over the reference, so
    // veil-over-canvas is the canvas, exactly like Daylight's #F5F5F7 and High
    // Contrast's black. The C121 suite pins the result: H2, toggling refraction
    // on the far canvas, dE <= 1.
    {TokenId::SurfaceBackdropVeil, Variant::Midnight, Paint(kMidVeil)},
    {TokenId::ShadowFloatingPanel, Variant::Midnight,
     Rgba(0.0f, 0.0f, 0.0f, 0.18f)},
    // High contrast (EXT a11y variant: opaque, maximum separation)
    {TokenId::SurfaceCanvasBase, Variant::HighContrast,
     Rgba(ChannelFromByte(0), ChannelFromByte(0), ChannelFromByte(0), 1.0f)},
    // C124: HighContrast keeps the ladder -- five opaque steps on black. It is
    // exempt from V3's 1.03..1.10 band (an accessibility surface must be MORE
    // separated, not less), which is the same exemption SelectionTint documents.
    {TokenId::SurfacePanel, Variant::HighContrast,
     Rgba(ChannelFromByte(14), ChannelFromByte(14), ChannelFromByte(16), 1.0f)},
    {TokenId::SurfaceCard, Variant::HighContrast,
     Rgba(ChannelFromByte(20), ChannelFromByte(20), ChannelFromByte(22), 1.0f)},
    {TokenId::SurfaceFloatingIsland, Variant::HighContrast,
     Rgba(ChannelFromByte(28), ChannelFromByte(28), ChannelFromByte(30), 1.0f)},
    {TokenId::SurfaceBorder, Variant::HighContrast,
     Rgba(ChannelFromByte(56), ChannelFromByte(56), ChannelFromByte(62), 1.0f)},
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
    // C104: Midnight used to sit at 16%. The pill is a capsule now, and the wash
    // has to stay under 15% in BOTH design themes -- above that the rounded row
    // reads as a filled plate again, which is the XP report this card answers.
    // HighContrast is deliberately NOT in that band: on pure black a 14% wash is
    // invisible, so it keeps the accessibility lift (asserted as its own floor).
    {TokenId::SelectionTint, Variant::Daylight,
     Rgba(ChannelFromByte(10), ChannelFromByte(132), ChannelFromByte(255), 0.14f)},
    {TokenId::SelectionTint, Variant::Midnight, Paint(kMidSelection)},
    {TokenId::SelectionTint, Variant::HighContrast,
     Rgba(ChannelFromByte(102), ChannelFromByte(179), ChannelFromByte(255), 0.24f)},
    // Raised tab plate: brighter than the canvas, still a translucent material
    // so the backdrop's colour shows through (Daylight/Midnight).
    // C124: the raised tab plate IS the card tier (TierOf says so), so the three
    // rows below must equal the card rows exactly -- the suite asserts it, which
    // is why a "just this once" sixth tone cannot be introduced here. C79 had
    // this plate as its own value in all three themes; that was one of the
    // eleven the audit counted, and the 8 px corner + specular rim already carry
    // "raised" without a bespoke fill.
    {TokenId::SurfaceTabActive, Variant::Daylight,
     Rgba(ChannelFromByte(255), ChannelFromByte(255), ChannelFromByte(255), 1.0f)},
    {TokenId::SurfaceTabActive, Variant::Midnight, Paint(kMidCard)},
    {TokenId::SurfaceTabActive, Variant::HighContrast,
     Rgba(ChannelFromByte(20), ChannelFromByte(20), ChannelFromByte(22), 1.0f)},
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

    // C124: THE FOLDER'S THREE TONES. A material colour, identical in every
    // theme: (255,217,138) / (255,182,102) / (219,144,58), the audit's own
    // sample of Bandizip's folder (DESIGN-PROPOSAL 2.1, 12.5). Relative
    // luminance decreases highlight -> body -> shadow (the suite asserts the
    // ORDER, not just the values), which is what makes the glyph read as a
    // solid object with light from above at 16 px.
    {TokenId::FolderHighlight, Variant::Daylight, Paint(kFolderHighlight)},
    {TokenId::FolderBody, Variant::Daylight, Paint(kFolderBody)},
    {TokenId::FolderShadow, Variant::Daylight, Paint(kFolderShadow)},
    {TokenId::FolderHighlight, Variant::Midnight, Paint(kFolderHighlight)},
    {TokenId::FolderBody, Variant::Midnight, Paint(kFolderBody)},
    {TokenId::FolderShadow, Variant::Midnight, Paint(kFolderShadow)},
    {TokenId::FolderHighlight, Variant::HighContrast, Paint(kFolderHighlight)},
    {TokenId::FolderBody, Variant::HighContrast, Paint(kFolderBody)},
    {TokenId::FolderShadow, Variant::HighContrast, Paint(kFolderShadow)},
};

// --border-specular-rim and --accent-liquid-primary.
inline constexpr GradientRow kGradientRows[] = {
    {TokenId::BorderSpecularRim, Variant::Daylight,
     Gradient(Rgba(1.0f, 1.0f, 1.0f, 0.85f), Rgba(1.0f, 1.0f, 1.0f, 0.18f))},
    // C124: ONE ACCENT. #0A84FF in Daylight and Midnight, FLAT (from == to):
    // a two-stop accent is a second accent value on every gradient-capable
    // surface, which is how the product ended up with three blues. HighContrast
    // below keeps its raised-contrast pair -- the documented a11y override.
    {TokenId::AccentLiquidPrimary, Variant::Daylight,
     Gradient(Paint(kDayAccent), Paint(kDayAccentEnd))},
    {TokenId::BorderSpecularRim, Variant::Midnight,
     Gradient(Rgba(1.0f, 1.0f, 1.0f, 0.28f), Rgba(1.0f, 1.0f, 1.0f, 0.04f))},
    {TokenId::AccentLiquidPrimary, Variant::Midnight,
     Gradient(Paint(kMidAccent), Paint(kMidAccentEnd))},
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

// C124: a tier's row. This is the TOKEN, not the pixel -- the floating tier is
// translucent, so what a frame measures is prism::chrome::SurfaceTierPresented.
inline Rgba SurfaceTierColor(SurfaceTier tier, Variant variant) {
  const TokenId id = SurfaceTierToken(tier);
  if (id == TokenId::Count) return kUnknownFallbackColor;
  return Lookup(id, variant).color;
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

#if defined(PRISM_C102_MUTANT_FLAT)
// NEGATIVE CONTROL (CONTRACTS D3 / D26): put the Daylight island's alpha back
// to the pre-C102 .58 on the ONE path every painter of "glass" uses, so the
// "island stands off the canvas" assertion has nothing left to measure. The
// counter proves the injected branch ran (D27): a red that came from anywhere
// else cannot print the CAUGHT verdict.
inline int& MutantFlatHits() {
  static int hits = 0;
  return hits;
}
#endif

// The floating-island surface with the saved glass intensity folded into its
// alpha. Every consumer that paints "glass" must come through here; a literal
// .52/.58 at a call site is the defect this function exists to prevent.
inline Rgba EffectiveGlassSurface(Variant variant) {
  Rgba surface = Lookup(TokenId::SurfaceFloatingIsland, variant).color;
#if defined(PRISM_C102_MUTANT_FLAT)
  if (variant == Variant::Daylight) {
    ++MutantFlatHits();
    surface.a = 0.58f;
  }
#endif
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

