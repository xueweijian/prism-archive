// PrismArchive -- PrismCore
// C106: THE MENU BAR'S TWO RULES AS NUMBERS (the pure half).
//
// The Windows suite (win/test_c106_chrome.cpp) samples REAL pixels of a REAL
// window: a DWM caption over an owner-drawn menu bar. This suite gates the same
// rules where they can be checked on every lane, including the Linux one, and
// with numbers instead of a screenshot:
//
//   1. the two shell surfaces are ONE surface within 2 levels per channel, and
//      the palette that produces them is a single function (ShellSurface);
//   2. the auto-hide state machine: a bare Alt flips it, a chord does not, and
//      hiding gives the content back EXACTLY the bar's height (>= 20 px at
//      96 DPI, which is the card's floor).
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C106_MUTANT_ALT_NOOP makes
// Toggle() return its argument unchanged -- the pre-card state, where Alt did
// nothing at all. Case 2's "one Alt press flips the bar" assertions MUST go red,
// and the verdict is armed on the mutation being compiled in, never on a red.
#include <cstdio>
#include <string>

#include "mini_test.hpp"

#include "../Src/PrismCore/ChromeMenuBar.hpp"
#include "../Src/PrismCore/ChromeTheme.hpp"

namespace {

using prism::chrome::AltTracker;
using prism::chrome::MenuBarState;
using prism::theme::Variant;

#if defined(PRISM_C106_MUTANT_ALT_NOOP)
constexpr const char* kMutation = "alt_noop";
#else
constexpr const char* kMutation = nullptr;
#endif

// The state a window is in on a 96 DPI screen: SM_CYMENU is 20 there, which is
// also the card's ">= 20 px" floor. Both numbers are passed in, so the test does
// not depend on the machine it runs on -- the WINDOWS suite reads the real
// SM_CYMENU and this one uses the documented value.
constexpr int kMenuHeight = 20;
constexpr int kCaptionHeight = 23;  // SM_CYCAPTION at 96 DPI on Windows 11
constexpr int kFrameTop = 100;

const Variant kVariants[3] = {Variant::Daylight, Variant::Midnight,
                              Variant::HighContrast};

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C106 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C106 MUTATION CAUGHT: ") + kMutation;
    // D3: the CAUGHT verdict is armed on the injection being in this binary AND
    // the rule it breaks being the one that went red. Toggle() is the rule.
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
      MenuBarState state;
      state.visible = true;
      return prism::chrome::Toggle(state).visible == state.visible;  // no-op proof
    });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL MENU-BAR CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. ONE SURFACE: the plate and the caption resolve the same colour, in every
//    theme, through one function.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 the shell surface is one colour in all three themes") {
  for (const Variant variant : kVariants) {
    const prism::chrome::Rgba surface = prism::chrome::ShellSurface(variant);
    const prism::chrome::Rgba plate = prism::chrome::PaletteFor(variant).background;
    // The caption (WindowChrome::Apply) and the menu plate (GalleryWindow::
    // RebuildMenu) both call ShellSurface. If a future card gives either half its
    // own lookup, this is where the drift shows up first.
    PRISM_CHECK(prism::chrome::SameShellSurface(surface, plate));
    const int r = prism::chrome::Channel255(surface.r);
    const int g = prism::chrome::Channel255(surface.g);
    const int b = prism::chrome::Channel255(surface.b);
    const std::string themeName(prism::theme::VariantName(variant));
    std::printf("  [%s] shell=#%02X%02X%02X\n", themeName.c_str(), r, g, b);
    // The three themes really are three different surfaces: a "gate" that passes
    // because every theme paints the same colour would prove nothing.
    PRISM_CHECK(!(prism::chrome::SameShellSurface(
        surface, prism::chrome::ShellSurface(kVariants[0])) &&
        variant != kVariants[0]));
  }
  // The two shell themes are DARK and the day theme is LIGHT: the gate has to
  // hold across that boundary, not inside one half of it.
  PRISM_CHECK(prism::chrome::IsDarkSurface(prism::chrome::ShellSurface(Variant::Midnight)));
  PRISM_CHECK(!prism::chrome::IsDarkSurface(prism::chrome::ShellSurface(Variant::Daylight)));
}

// ---------------------------------------------------------------------------
// 2. THE PIXEL PREDICATE ITSELF: 2 levels pass, 3 do not, and the pre-fix white
//    bar is caught in the two dark themes.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 the two-level gate rejects the white bar") {
  // Identical pixels.
  PRISM_CHECK(prism::chrome::SameShellSurface(11, 12, 16, 11, 12, 16));
  // Two levels of capture/compositor rounding: still one surface.
  PRISM_CHECK(prism::chrome::SameShellSurface(11, 12, 16, 13, 14, 18));
  // Three levels: a different surface. The tolerance is 2, not "about right".
  PRISM_CHECK(!prism::chrome::SameShellSurface(11, 12, 16, 14, 12, 16));
  // One channel off is enough -- a bar with the right brightness and the wrong
  // hue is still the wrong bar.
  PRISM_CHECK(!prism::chrome::SameShellSurface(11, 12, 16, 11, 12, 20));
  // The plan's white bar over the two dark themes: 255 vs the dark plate.
  for (const Variant variant : kVariants) {
    const prism::chrome::Rgba surface = prism::chrome::ShellSurface(variant);
    if (!prism::chrome::IsDarkSurface(surface)) continue;
    PRISM_CHECK(!prism::chrome::SameShellSurface(255, 255, 255,
                                                 prism::chrome::Channel255(surface.r),
                                                 prism::chrome::Channel255(surface.g),
                                                 prism::chrome::Channel255(surface.b)));
  }
}

// ---------------------------------------------------------------------------
// 3. THE STATE MACHINE: one bare Alt press flips, twice restores, the preference
//    seeds the start, and a chord is not a bare Alt.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 a bare Alt flips the bar and a second Alt puts it back") {
  MenuBarState state;
  PRISM_CHECK(state.visible);       // a fresh window shows its bar
  PRISM_CHECK(!state.autoHide);

  // One press: down.
  state = prism::chrome::Toggle(state);
  PRISM_CHECK(!state.visible);
  // The pixels it gives back are the bar's height, exactly.
  PRISM_CHECK_EQ(prism::chrome::HiddenShift(state, kMenuHeight), kMenuHeight);
  PRISM_CHECK(prism::chrome::ShiftIsObservable(kMenuHeight));
  // The card's floor is 20: a menu bar one pixel shorter could not satisfy it,
  // and the predicate says so instead of rounding.
  PRISM_CHECK(!prism::chrome::ShiftIsObservable(kMenuHeight - 1));

  // Second press: back up, and nothing is reserved.
  state = prism::chrome::Toggle(state);
  PRISM_CHECK(state.visible);
  PRISM_CHECK_EQ(prism::chrome::HiddenShift(state, kMenuHeight), 0);

  // The preference seeds the start: auto-hide means the bar is not on screen.
  MenuBarState autoHide;
  autoHide.autoHide = true;
  autoHide = prism::chrome::StartHiddenIfAutoHide(autoHide);
  PRISM_CHECK(!autoHide.visible);
  PRISM_CHECK(autoHide.autoHide);  // the preference itself is NOT rewritten
  // ...and a manual show does not rewrite it either: the next start hides again.
  autoHide = prism::chrome::Toggle(autoHide);
  PRISM_CHECK(autoHide.visible);
  PRISM_CHECK(autoHide.autoHide);
}

PRISM_TEST("c106 a chord is not a bare Alt") {
  AltTracker bare;
  PRISM_CHECK(prism::chrome::ShouldToggleOnAltRelease(bare));

  // Alt+F: the F arrives as WM_SYSKEYDOWN while Alt is down.
  AltTracker chord;
  prism::chrome::NoteAltChordKey(&chord);
  PRISM_CHECK(!prism::chrome::ShouldToggleOnAltRelease(chord));

  // The window resets the tracker on every Alt press, so the NEXT bare Alt works
  // again (a tracker that stayed dirty would make Alt a one-shot).
  chord = AltTracker{};
  PRISM_CHECK(prism::chrome::ShouldToggleOnAltRelease(chord));
}

// ---------------------------------------------------------------------------
// 4. THE GEOMETRY: the content top moves up by exactly the bar's height, and the
//    window itself does not move.
// ---------------------------------------------------------------------------
PRISM_TEST("c106 hiding the bar moves the content up by the bar height") {
  MenuBarState shown;
  MenuBarState hidden = prism::chrome::Toggle(shown);

  const int shownTop = prism::chrome::ContentTopScreen(kFrameTop, kCaptionHeight, shown,
                                                      kMenuHeight);
  const int hiddenTop = prism::chrome::ContentTopScreen(kFrameTop, kCaptionHeight, hidden,
                                                       kMenuHeight);
  std::printf("  [geometry] shown=%d hidden=%d delta=%d\n", shownTop, hiddenTop,
              shownTop - hiddenTop);
  // frameTop + caption + bar while it is up; frameTop + caption while it is down.
  PRISM_CHECK_EQ(shownTop, kFrameTop + kCaptionHeight + kMenuHeight);
  PRISM_CHECK_EQ(hiddenTop, kFrameTop + kCaptionHeight);
  PRISM_CHECK_EQ(shownTop - hiddenTop, kMenuHeight);
  // The card's floor, on the number the user sees move.
  PRISM_CHECK(shownTop - hiddenTop >= prism::chrome::kMinHiddenShift);

  // The client area gains the same pixels at its TOP: the window did not grow.
  PRISM_CHECK_EQ(prism::chrome::ClientHeightAfter(hidden, 800, kMenuHeight), 800 + kMenuHeight);
  PRISM_CHECK_EQ(prism::chrome::ClientHeightAfter(shown, 800, kMenuHeight), 800);

  // Degenerate inputs are values, not crashes: no bar, no shift.
  PRISM_CHECK_EQ(prism::chrome::HiddenShift(hidden, 0), 0);
  PRISM_CHECK_EQ(prism::chrome::HiddenShift(hidden, -7), 0);
}
