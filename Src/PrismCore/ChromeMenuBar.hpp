// PrismArchive -- PrismCore
// C106: THE MENU BAR'S TWO RULES -- it is the SAME SURFACE as the title bar, and
// it can GET OUT OF THE WAY.
//
// THE MEASURED DEFECT. The top of the window is two different systems' pixels: a
// DWM caption (32 px, one COLORREF) and a Win32 menu bar (SM_CYMENU px, a GDI
// brush, owner-drawn items). C83 made both read PaletteFor(variant).background,
// so they matched -- but the equality was two call sites agreeing by accident,
// and nothing in the tree asserted it: c83 compares each surface against the
// TOKEN, never against each other. A one-line change to either side (the
// PRISM_C106_MUTANT_WHITE_BAR injection is exactly that) produces the white bar
// the plan's section 4 describes, and every existing gate stays green.
//
// AND THE BAR ALWAYS OCCUPIED SPACE. There was no way to hide it: the top 20 px
// were lost to four words the user reads once a session.
//
// WHAT THIS FILE IS: the numbers, with no HWND in sight, so the Linux lane can
// gate the same rules the Windows pixel suite measures:
//   * SameShellSurface   the two shell pixels are one surface within 2 levels;
//   * MenuBarState       visible / autoHide, and the Alt toggle that flips it;
//   * AltTracker         "bare Alt" vs "Alt+F" -- a chord must keep its mnemonic;
//   * ContentTopScreen   the geometry the card's ">= 20 px" is measured on.
//
// The Windows half (GalleryWindow) calls THESE functions; it does not restate
// them. A rule that lived in both places could disagree with itself.
#pragma once

#include "ChromeTheme.hpp"

namespace prism::chrome {

// The card's pixel tolerance. The caption is composited by DWM (which may round
// one level) and the plate is a solid GDI brush; two levels is the allowance,
// never more. A bar one shade off is exactly the defect.
inline constexpr int kShellSurfaceTolerance = 2;

// The card's geometry floor: hiding the bar must move the content by at least
// this many pixels. SM_CYMENU is 20 at 96 DPI, so a bar that "hides" without
// giving the space back cannot satisfy this.
inline constexpr int kMinHiddenShift = 20;

// 0..255 quantisation, the SAME rounding as prism::win::chrome::ToColorRef. Kept
// here so the pure gate and the GDI brush cannot disagree about 0.5.
inline int Channel255(float value) {
  if (!(value > 0.0f)) return 0;
  if (value >= 1.0f) return 255;
  return static_cast<int>(value * 255.0f + 0.5f);
}

inline int ChannelDistance(int a, int b) { return a > b ? a - b : b - a; }

// Two 0..255 triples are the same surface within `tolerance` on EVERY channel.
// Per-channel, not luma: a bar that is the right brightness but the wrong hue is
// still a different surface, and luma alone cannot see that.
inline bool SameShellSurface(int menuR, int menuG, int menuB, int titleR, int titleG,
                             int titleB, int tolerance = kShellSurfaceTolerance) {
  return ChannelDistance(menuR, titleR) <= tolerance &&
         ChannelDistance(menuG, titleG) <= tolerance &&
         ChannelDistance(menuB, titleB) <= tolerance;
}

// The same predicate for the colours the code RESOLVES (as opposed to the pixels
// a screenshot returns). Both halves of the gate are one function.
inline bool SameShellSurface(const Rgba& menu, const Rgba& title,
                             int tolerance = kShellSurfaceTolerance) {
  return SameShellSurface(Channel255(menu.r), Channel255(menu.g), Channel255(menu.b),
                          Channel255(title.r), Channel255(title.g), Channel255(title.b),
                          tolerance);
}

// ---------------------------------------------------------------------------
// The auto-hide state.
//
// `autoHide` is the SETTING (the user asked for the bar to start hidden); the
// window applies it once, at creation, through StartHiddenIfAutoHide(). `visible`
// is the live state, which the Alt toggle flips. Keeping the two apart is what
// makes "temporarily shown with Alt" possible: showing the bar does not rewrite
// the preference, so the next start hides it again.
// ---------------------------------------------------------------------------
struct MenuBarState {
  bool visible = true;
  bool autoHide = false;

  bool operator==(const MenuBarState&) const = default;
};

// The preference is applied once, at start: auto-hide means the bar is not on
// screen until asked for.
inline MenuBarState StartHiddenIfAutoHide(MenuBarState state) {
  if (state.autoHide) state.visible = false;
  return state;
}

// One Alt press, one flip -- in BOTH directions. A toggle that only ever shows
// would be a one-way door: the user could not get the pixels back.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C106_MUTANT_ALT_NOOP returns the state
// unchanged -- the pre-card behaviour, where Alt did nothing at all. The window
// calls THIS function (GalleryWindow::ToggleMenuBar), so the mutation reaches the
// product path and not just the suite.
inline MenuBarState Toggle(MenuBarState state) {
#if defined(PRISM_C106_MUTANT_ALT_NOOP)
  return state;  // no-op: the bar never moves
#else
  state.visible = !state.visible;
  return state;
#endif
}

// "Bare Alt" detection. Windows sends WM_SYSKEYDOWN(VK_MENU) for the Alt press
// itself, so toggling on the press would fire on Alt+F as well and swallow the
// File mnemonic. The window records every OTHER sys key seen while Alt is down
// here, and toggles on the RELEASE only when the tracker stayed clean.
struct AltTracker {
  bool chordUsed = false;

  bool operator==(const AltTracker&) const = default;
};

inline void NoteAltChordKey(AltTracker* tracker) {
  if (tracker != nullptr) tracker->chordUsed = true;
}

inline bool ShouldToggleOnAltRelease(const AltTracker& tracker) {
  return !tracker.chordUsed;
}

// ---------------------------------------------------------------------------
// The geometry. All three numbers are SCREEN pixels, which is what a screenshot
// and a mouse position are in; the client-area layout is relative and cannot
// show the bar's absence on its own.
// ---------------------------------------------------------------------------
// How many pixels the content gains at the top when the bar is hidden.
inline int HiddenShift(const MenuBarState& state, int menuHeight) {
  if (menuHeight <= 0) return 0;
  return state.visible ? 0 : menuHeight;
}

// The top of the content area (the first client pixel) in screen coordinates.
// `frameTop` is the visible frame's top; the caption sits under it; the menu bar
// under the caption, and only while it is visible.
inline int ContentTopScreen(int frameTop, int captionHeight, const MenuBarState& state,
                            int menuHeight) {
  const int caption = captionHeight > 0 ? captionHeight : 0;
  const int menu = menuHeight > 0 ? menuHeight : 0;
  // The bar OCCUPIES its height while it is visible; HiddenShift is what it
  // GIVES BACK when it is not. Adding HiddenShift here (instead of the occupancy)
  // is the off-by-one-the-other-way bug the core suite caught on its first run:
  // the "shown" top came out 20 px too high.
  return frameTop + caption + (state.visible ? menu : 0);
}

// The client area's height once the bar is gone: the window did not grow, the
// menu simply stopped reserving non-client space, so the client takes the pixels.
inline int ClientHeightAfter(const MenuBarState& state, int clientHeight, int menuHeight) {
  return (clientHeight > 0 ? clientHeight : 0) + HiddenShift(state, menuHeight);
}

// True when the bar's absence is OBSERVABLE on screen: the card's ">= 20 px".
inline bool ShiftIsObservable(int shift) { return shift >= kMinHiddenShift; }

}  // namespace prism::chrome
