// PrismArchive -- PrismCore
// C83: the WINDOW SHELL's palette -- title bar, menu bar, menu dropdowns.
//
// THE MEASURED DEFECT (UI-AUDIT A4). The top 64 px of the window were the
// system's white caption plus a light Win32 menu bar, and directly below them
// the canvas was #0B0E13. The shell and the content read as two applications.
//
// WHAT THIS FILE IS. One pure function from a theme Variant to the eight
// colours the shell paints, and the two numeric gates the card asks for:
//   * menu text contrast >= 4.5:1 (WCAG body text) on BOTH the resting and the
//     selected row;
//   * the shell's luma is within 12 of the canvas (0..255 scale), which is the
//     "title bar brightness < canvas brightness + 12" rule expressed as code so
//     the Windows pixel test and the Linux lane cannot disagree about it.
//
// WHY A PALETTE AND NOT FOUR CALL SITES. The title bar (DWM caption colour), the
// menu bar background brush, the dropdown background and the selected-row wash
// are four different Win32 surfaces. If each resolved its own token, a theme
// switch could leave one of them behind -- exactly the drift ThemeTokens exists
// to prevent. Every surface asks PaletteFor(variant).
//
// The colours are TOKENS, never literals: the menu surface is
// --surface-backdrop-reference (the same opaque reference the glass panels
// composite over, and the colour the canvas sits on), the text is
// --text-primary, the selected row is --selection-tint over that surface.
#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>

#include "ThemeTokens.hpp"

namespace prism::chrome {

using theme::Rgba;
using theme::Variant;

// The card's continuity rule, in one place: a shell pixel is "the same surface"
// as the canvas when its perceptual luma is less than the canvas luma plus this
// many levels. The Windows suite samples real pixels against it.
inline constexpr double kMaxShellCanvasLumaDelta = 12.0;

struct Palette {
  Rgba background{};    // menu bar + dropdown plate (opaque)
  Rgba text{};          // menu item text
  Rgba textDisabled{};  // a greyed item (WCAG exempts it, but it must be legible)
  Rgba highlight{};     // selected / hovered item plate (opaque)
  Rgba highlightText{}; // text on that plate
  Rgba separator{};     // 1 px rule between item groups
  Rgba rim{};           // --border-specular-rim over the background
};

// ---------------------------------------------------------------------------
// C91: THE ONE THEME ENTRY.
//
// THE MEASURED DEFECT. A 1,544,424 px whole-window diff of day -> night moved
// 122,969 px (7.96 %), all of it in y=0..62 (caption + menu bar). The content
// area -- 92 % of the window -- was pixel-identical in all three themes, because
// the canvas was a literal (#0B0E13) and the renderer's variant was never set.
//
// WHY THESE THREE FUNCTIONS AND NOT A TOKEN LOOKUP AT EACH CALL SITE. Every
// surface asks the same chain: token -> composite over THIS theme's canvas. If a
// painter resolved its own combination, the canvas and the surface painted on it
// could disagree by the alpha of one layer -- the "two authorities" drift the
// token table exists to prevent. A call site that wants a colour from the theme
// calls ThemeSurfaceColor(); it never composites by hand.
//
// THE CHAIN, in order:
//   CanvasColor(v)          --surface-canvas-base over --surface-backdrop-reference.
//                              This is the colour the target is CLEARED to.
//   CanvasPresentedColor(v) the same, washed with --surface-backdrop-veil: the
//                              pixel a resting canvas region actually shows once
//                              the C77 backdrop layer has run (it is ON by
//                              default, and its veil covers the whole frame).
//   ThemeSurfaceColor(id,v) the token composited over CanvasPresentedColor(v) --
//                              what a surface painted straight onto the canvas
//                              resolves to (island, tab plate, panel, ...).
//
// The numbers this reproduces, measured by the pixel suites:
//   canvas      day (247,249,252)  night (13,14,18)  high-contrast (0,0,0)
//   island      day (252,252,254)  night (26,29,37)
//   sidebar     day (255,255,255)  high-contrast (26,26,28)
//   swim lane   day (251,254,255)  high-contrast (4,5,6)
// ---------------------------------------------------------------------------

#if defined(PRISM_C91_MUTANT_CORE_MIDNIGHT)
// NEGATIVE CONTROL (CONTRACTS D3): prove the injected branch ran, so a red that
// came from somewhere else cannot print the CAUGHT verdict.
inline int& MutantCoreHits() {
  static int hits = 0;
  return hits;
}
#endif

// The flat canvas tile: the two tokens composed, no backdrop layer involved.
inline Rgba CanvasColor(Variant variant) {
#if defined(PRISM_C91_MUTANT_CORE_MIDNIGHT)
  // NEGATIVE CONTROL (CONTRACTS D3): every theme resolves to Midnight, so a Day
  // assertion that expects a light canvas MUST go red.
  ++MutantCoreHits();
  variant = Variant::Midnight;
#endif
  const Rgba base = theme::Lookup(theme::TokenId::SurfaceCanvasBase, variant).color;
  const Rgba reference =
      theme::Lookup(theme::TokenId::SurfaceBackdropReference, variant).color;
  return theme::CompositeOver(base, reference);
}

// The canvas pixel at rest, with the C77 backdrop layer's veil on top. Empty
// regions of a frame show exactly this; the renderer's band fills must use it
// too, or the action band would be a visible rectangle on the canvas.
inline Rgba CanvasPresentedColor(Variant variant) {
  const Rgba veil = theme::Lookup(theme::TokenId::SurfaceBackdropVeil, variant).color;
  return theme::CompositeOver(veil, CanvasColor(variant));
}

// A token as it lands on this theme's canvas. Surfaces that are drawn straight
// onto the canvas (the floating island, the tab plate, the rail panel) resolve
// here; the result is OPAQUE, because "what colour is that pixel" is the only
// question the pixel suites ask, and a translucent answer is not a colour.
//
// The three-argument form takes the canvas a caller ALREADY has (the renderer
// knows exactly which colour it cleared to this frame, veil or no veil). There
// is one implementation; the two-argument form is a convenience, not a second
// rule.
inline Rgba ThemeSurfaceColor(theme::TokenId id, Variant variant, const Rgba& canvas) {
  if (id == theme::TokenId::SurfaceCanvasBase) return canvas;
  const Rgba top = id == theme::TokenId::SurfaceFloatingIsland
                       ? theme::EffectiveGlassSurface(variant)
                       : theme::Lookup(id, variant).color;
  return theme::CompositeOver(top, canvas);
}

inline Rgba ThemeSurfaceColor(theme::TokenId id, Variant variant) {
  return ThemeSurfaceColor(id, variant, CanvasPresentedColor(variant));
}

inline Rgba Flatten(const Rgba& top, const Rgba& bottom) {
  return theme::CompositeOver(top, bottom);
}

// One palette per Variant. Every field comes from ThemeTokens::Lookup, so a
// missing row falls back through the documented chain instead of inventing a
// colour; a token that resolves to the magenta sentinel would fail the contrast
// gate loudly rather than paint a plausible-looking wrong shell.
inline Palette PaletteFor(Variant variant) {
  Palette palette;
  const Rgba backdrop =
      theme::Lookup(theme::TokenId::SurfaceBackdropReference, variant).color;
#if defined(PRISM_C83_MUTANT_MENU_SYSTEM_LIGHT)
  // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix shell -- the system's light
  // menu plate while the ink stays the theme's. White on white: 1.0:1.
  palette.background = Rgba(1.0f, 1.0f, 1.0f, 1.0f);
#else
  palette.background = backdrop;
#endif
  palette.text = theme::Lookup(theme::TokenId::TextPrimary, variant).color;
  palette.textDisabled = theme::Lookup(theme::TokenId::TextSecondary, variant).color;
  palette.highlight = Flatten(
      theme::Lookup(theme::TokenId::SelectionTint, variant).color, backdrop);
  palette.highlightText = palette.text;
  palette.separator =
      Flatten(theme::Lookup(theme::TokenId::DividerSubtle, variant).color, backdrop);
  palette.rim = Flatten(
      theme::Lookup(theme::TokenId::BorderSpecularRim, variant).gradient.from, backdrop);
  return palette;
}

// C106: THE ONE COLOUR THE SHELL PAINTS, for both halves of it.
//
// The caption is composited by DWM from a COLORREF and the menu plate is a GDI
// brush; they are different APIs, and before this card they were two separate
// PaletteFor(variant).background lookups in two files. Same value, yes -- but
// "the two are equal" was a coincidence of two call sites, not a property the
// code could be read as holding. Both halves now ask THIS function, so the
// equality the pixel gate measures is the equality the source expresses.
inline Rgba ShellSurface(Variant variant) { return PaletteFor(variant).background; }

// Perceptual luma on the 0..255 scale the pixel test samples. sRGB weights, not
// the WCAG linearised luminance: this is the number a screenshot reports.
inline double Luma255(const Rgba& color) {
  return 255.0 * (0.2126 * static_cast<double>(color.r) +
                  0.7152 * static_cast<double>(color.g) +
                  0.0722 * static_cast<double>(color.b));
}

// The card's title-bar rule, as a predicate both lanes call.
inline bool ShellMatchesCanvas(double shellLuma, double canvasLuma) {
  return shellLuma < canvasLuma + kMaxShellCanvasLumaDelta;
}

// True when the shell is the dark one (Midnight / HighContrast). The Windows
// layer uses this to decide whether the DWM caption must be dark.
inline bool IsDarkSurface(const Rgba& surface) {
  return theme::RelativeLuminance(surface) < 0.2;
}

inline double TextContrast(const Palette& palette) {
  return theme::ContrastRatio(palette.text, palette.background);
}

inline double HighlightContrast(const Palette& palette) {
  return theme::ContrastRatio(palette.highlightText, palette.highlight);
}

inline bool MeetsBodyText(const Palette& palette) {
  return TextContrast(palette) >= theme::kWcagBodyTextMinRatio &&
         HighlightContrast(palette) >= theme::kWcagBodyTextMinRatio;
}

// C83: the mnemonic a menu label carries ('&' + one character), matched the
// way the menu loop would. An owner-drawn item has no OS string, so the system
// cannot do this lookup for us -- the window matches the SAME painted text.
// "&&" is a literal ampersand, not a mnemonic.
inline bool MnemonicMatches(std::wstring_view text, wchar_t letter) {
  if (letter == 0) return false;
  const auto lower = [](wchar_t c) -> wchar_t {
    return (c >= L'A' && c <= L'Z') ? static_cast<wchar_t>(c - L'A' + L'a') : c;
  };
  const wchar_t wanted = lower(letter);
  for (std::size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] != L'&') continue;
    if (text[i + 1] == L'&') {
      ++i;
      continue;
    }
    return lower(text[i + 1]) == wanted;
  }
  return false;
}

// C125: the OS's own light/dark answer, as far as PrismCore may know it.
//
// THE MEASURED DEFECT. "system" (the schema default) resolved to Midnight. On a
// light Windows desktop (this machine: AppsUseLightTheme=1) the window was black
// while every other app was white -- and Tests/test_chrome_theme.cpp pinned that
// deliberate bug, so a correct implementation could not pass.
//
// WHY THE VALUE IS PASSED IN. PrismCore is header-only and may not include
// <windows.h> (preflight check_arch_no_windows), so the registry read lives one
// layer up (PrismWin/SystemTheme.hpp) and the DECISION stays here, where it can
// be tested on the Linux lane without a hive.
//
// Unknown is a real state, not a synonym for dark: no readable value means no
// evidence, and guessing "light" would paint a white shell on a dark desktop.
// The honest answer is the previous behaviour, which is what Unknown returns.
enum class SystemTheme : uint8_t { Unknown, Dark, Light };

inline std::ostream& operator<<(std::ostream& os, SystemTheme theme) {
  switch (theme) {
    case SystemTheme::Unknown: return os << "Unknown";
    case SystemTheme::Dark: return os << "Dark";
    case SystemTheme::Light: return os << "Light";
  }
  return os << "?";
}

// Which shell a saved appearance document asks for.
//
// "light" and "dark" are the user's explicit choices and win over the OS.
// "system" follows SystemTheme: the SAME variant drives the shell AND the canvas
// (GalleryWindow::SetChromeVariant -> Scene::theme), so "system" can no longer
// paint a white bar over a black canvas -- the two halves move together.
inline Variant ShellVariantFor(std::string_view themeKey, bool highContrast,
                               SystemTheme systemTheme) {
  if (highContrast) return Variant::HighContrast;
  if (themeKey == "light") return Variant::Daylight;
  if (themeKey == "dark") return Variant::Midnight;
#if defined(PRISM_C125_MUTANT_SYSTEM_ALWAYS_MIDNIGHT)
  // NEGATIVE CONTROL (CONTRACTS D3): the pre-card rule -- "system" is Midnight
  // whatever the desktop says. S3 (a light OS must be Daylight) MUST go red.
  return Variant::Midnight;
#else
  if (themeKey == "system") {
    return systemTheme == SystemTheme::Light ? Variant::Daylight : Variant::Midnight;
  }
  return Variant::Midnight;  // an unknown key keeps the previous shell
#endif
}

}  // namespace prism::chrome
