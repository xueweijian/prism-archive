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

// The canvas the window actually clears to (GalleryRenderer::kBackdrop,
// #0B0E13). It is a literal there for historical reasons; naming it here lets
// the shell test say "the shell is continuous with THAT pixel" without importing
// the renderer into PrismCore.
inline constexpr Rgba kCanvasReference{0.043f, 0.055f, 0.075f, 1.0f};

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

// Which shell a saved appearance document asks for.
//
// "light" and "dark" are the user's explicit choices. "system" (the schema
// default) resolves to the window's OWN palette -- Midnight -- because the
// canvas this shell must be continuous with is #0B0E13 today: following a light
// OS desktop would paint a white bar over a black canvas, which is the defect
// this card closes, not a fix for it. When the canvas learns to follow the OS
// (a later card), this is the one function that changes.
inline Variant ShellVariantFor(std::string_view themeKey, bool highContrast) {
  if (highContrast) return Variant::HighContrast;
  if (themeKey == "light") return Variant::Daylight;
  return Variant::Midnight;
}

}  // namespace prism::chrome
