// PrismArchive -- PrismCore
// C83: the SHELL palette (title bar / menu bar / dropdown), asserted as numbers.
//
// THE DEFECT (UI-AUDIT A4). The top 64 px of the window were the system's white
// caption and a light Win32 menu bar; the canvas below is #0B0E13. The shell and
// the content did not read as one application.
//
// WHAT THIS FILE PINS (Linux lane, no HWND):
//   1. the menu plate is a TOKEN, not a literal: PaletteFor(variant).background
//      IS --surface-backdrop-reference for that variant;
//   2. the item text meets WCAG body text on BOTH the resting and the selected
//      row (>= 4.5:1), which is the card's contrast assertion;
//   3. the dark shell is within the card's 12-level luma band of the canvas the
//      window actually clears to;
//   4. the appearance document's theme key maps to exactly one shell variant.
//
// The WINDOWS half (DWM attribute probing, real pixels) is win.c83_chrome.
#include <cmath>
#include <cstdio>

#include "mini_test.hpp"

#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/ThemeTokens.hpp"

namespace {

using prism::chrome::Palette;
using prism::chrome::PaletteFor;
using prism::theme::ContrastRatio;
using prism::theme::Lookup;
using prism::theme::TokenId;
using prism::theme::Variant;

#if defined(PRISM_C83_MUTANT_MENU_SYSTEM_LIGHT)
constexpr const char* kMutation = "menu_system_light";
#else
constexpr const char* kMutation = nullptr;
#endif

bool SameColor(const prism::theme::Rgba& a, const prism::theme::Rgba& b) {
  return std::fabs(static_cast<double>(a.r) - static_cast<double>(b.r)) < 1e-6 &&
         std::fabs(static_cast<double>(a.g) - static_cast<double>(b.g)) < 1e-6 &&
         std::fabs(static_cast<double>(a.b) - static_cast<double>(b.b)) < 1e-6 &&
         std::fabs(static_cast<double>(a.a) - static_cast<double>(b.a)) < 1e-6;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C83 MUTANT ACTIVE: %s\n", kMutation);
    // The injection fired when the Midnight menu text no longer clears 4.5:1 on
    // its own plate -- i.e. the assertion below is the thing that catches it.
    ::prism::test::SetMutationVerdict("C83 MUTATION CAUGHT: menu_system_light", [] {
      return prism::chrome::TextContrast(PaletteFor(Variant::Midnight)) <
             prism::theme::kWcagBodyTextMinRatio;
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL SHELL-PALETTE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE MENU PLATE IS THE TOKEN (the card's "background == dark token").
// ---------------------------------------------------------------------------
PRISM_TEST("c83 menu plate is --surface-backdrop-reference, per variant") {
  for (const Variant variant :
       {Variant::Daylight, Variant::Midnight, Variant::HighContrast}) {
    const Palette palette = PaletteFor(variant);
    const prism::theme::Rgba token =
        Lookup(TokenId::SurfaceBackdropReference, variant).color;
    std::printf("  [%s] plate=%.3f,%.3f,%.3f token=%.3f,%.3f,%.3f\n",
                std::string(prism::theme::VariantName(variant)).c_str(),
                static_cast<double>(palette.background.r),
                static_cast<double>(palette.background.g),
                static_cast<double>(palette.background.b),
                static_cast<double>(token.r), static_cast<double>(token.g),
                static_cast<double>(token.b));
    PRISM_CHECK(SameColor(palette.background, token));
    // ... and the text is the theme's primary ink, not a literal black/white.
    PRISM_CHECK(SameColor(palette.text, Lookup(TokenId::TextPrimary, variant).color));
  }
}

// ---------------------------------------------------------------------------
// 2. WCAG: menu text >= 4.5:1 on the resting plate AND on the selected row.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 menu text meets 4.5:1 on every variant and every row state") {
  for (const Variant variant :
       {Variant::Daylight, Variant::Midnight, Variant::HighContrast}) {
    const Palette palette = PaletteFor(variant);
    const double resting = prism::chrome::TextContrast(palette);
    const double selected = prism::chrome::HighlightContrast(palette);
    std::printf("  [%s] text/plate=%.2f selected=%.2f\n",
                std::string(prism::theme::VariantName(variant)).c_str(), resting,
                selected);
    PRISM_CHECK(resting >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(selected >= prism::theme::kWcagBodyTextMinRatio);
    PRISM_CHECK(prism::chrome::MeetsBodyText(palette));
  }
}

// ---------------------------------------------------------------------------
// 3. THE CARD'S LUMA RULE: the dark shell is within 12 levels of the canvas.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the dark shell is within 12 luma levels of the canvas") {
  const double canvas = prism::chrome::Luma255(prism::chrome::kCanvasReference);
  const double shell = prism::chrome::Luma255(PaletteFor(Variant::Midnight).background);
  std::printf("  canvas luma=%.2f shell luma=%.2f delta=%.2f\n", canvas, shell,
              shell - canvas);
  PRISM_CHECK(prism::chrome::IsDarkSurface(PaletteFor(Variant::Midnight).background));
  PRISM_CHECK(prism::chrome::IsDarkSurface(
      PaletteFor(Variant::HighContrast).background));
  PRISM_CHECK(prism::chrome::ShellMatchesCanvas(shell, canvas));
  // The predicate itself is a gate, not a tautology: a white caption (255) and
  // the Win11 dark-mode default (#202020, 32) must BOTH fail against this
  // canvas. That is exactly why the DWM caption COLOUR is set as well.
  PRISM_CHECK(!prism::chrome::ShellMatchesCanvas(255.0, canvas));
  PRISM_CHECK(!prism::chrome::ShellMatchesCanvas(32.0, canvas));
}

// ---------------------------------------------------------------------------
// 4. THE SELECTED ROW IS VISIBLE: a highlight identical to the plate would make
//    keyboard navigation invisible even though contrast passes.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the selected row is visibly lifted off the plate") {
  for (const Variant variant : {Variant::Daylight, Variant::Midnight,
                                Variant::HighContrast}) {
    const Palette palette = PaletteFor(variant);
    const double delta = std::fabs(prism::chrome::Luma255(palette.highlight) -
                                   prism::chrome::Luma255(palette.background));
    std::printf("  [%s] highlight delta=%.2f\n",
                std::string(prism::theme::VariantName(variant)).c_str(), delta);
    PRISM_CHECK(delta >= 4.0);
  }
}

// ---------------------------------------------------------------------------
// 4b. THE MNEMONIC. An owner-drawn item has no OS string, so the window matches
//     the painted label itself (WM_SYSCHAR). The rule is the menu loop's: '&x',
//     '&&' is a literal ampersand, and a label with no marker has no mnemonic.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 a mnemonic is read from the painted label") {
  PRISM_CHECK(prism::chrome::MnemonicMatches(L"&File", L'F'));
  PRISM_CHECK(prism::chrome::MnemonicMatches(L"&File", L'f'));
  PRISM_CHECK(!prism::chrome::MnemonicMatches(L"&File", L'E'));
  PRISM_CHECK(prism::chrome::MnemonicMatches(L"\u8BBE\u7F6E(&S)...", L's'));
  PRISM_CHECK(prism::chrome::MnemonicMatches(L"Rena&me", L'M'));
  PRISM_CHECK(!prism::chrome::MnemonicMatches(L"&&File", L'F'));
  PRISM_CHECK(!prism::chrome::MnemonicMatches(L"\u6587\u4EF6", L'F'));
  PRISM_CHECK(!prism::chrome::MnemonicMatches(L"", L'F'));
}

// ---------------------------------------------------------------------------
// 5. THE APPEARANCE DOCUMENT -> ONE SHELL VARIANT.
// ---------------------------------------------------------------------------
PRISM_TEST("c83 the saved theme key maps to exactly one shell variant") {
  PRISM_CHECK(prism::chrome::ShellVariantFor("dark", false) == Variant::Midnight);
  PRISM_CHECK(prism::chrome::ShellVariantFor("light", false) == Variant::Daylight);
  // "system" is the schema default. The canvas this shell must be continuous
  // with is Midnight today, so "system" resolves to Midnight -- a white bar over
  // a black canvas would be the A4 defect, not a fix.
  PRISM_CHECK(prism::chrome::ShellVariantFor("system", false) == Variant::Midnight);
  PRISM_CHECK(prism::chrome::ShellVariantFor("", false) == Variant::Midnight);
  PRISM_CHECK(prism::chrome::ShellVariantFor("nonsense", false) == Variant::Midnight);
  // High contrast wins over the light/dark choice: it is an accessibility mode.
  PRISM_CHECK(prism::chrome::ShellVariantFor("light", true) == Variant::HighContrast);
  PRISM_CHECK(prism::chrome::ShellVariantFor("dark", true) == Variant::HighContrast);
}
