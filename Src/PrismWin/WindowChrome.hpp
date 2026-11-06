// PrismArchive -- PrismWin
// C83: the DARK TITLE BAR -- the OS half of the shell (the pure half is
// prism::chrome::PaletteFor in PrismCore/ChromeTheme.hpp).
//
// THE DEFECT. The caption and the menu bar were the system's light chrome; the
// client area below them is #0B0E13. This header turns the caption into the
// shell surface, and the window's owner-drawn menu bar (GalleryWindow) paints
// the SAME token, so the two are one continuous surface.
//
// WHAT IS SET, AND WHY EACH ONE
//   1. DWMWA_USE_IMMERSIVE_DARK_MODE. The attribute NUMBER is not stable:
//      Windows 10 1809..1909 answer 19, Windows 10 20H1+ and Windows 11 answer
//      20. Both are tried, 20 first; a refusal is a value (HRESULT + which
//      number answered), never a crash and never a silent no-op.
//   2. DWMWA_CAPTION_COLOR (Windows 11 22000+). Dark mode alone paints the
//      caption #202020, which is still ~20 levels above the canvas; the card's
//      rule is "title bar luma < canvas luma + 12", and only the token colour
//      satisfies it. On Windows 10 the call fails with E_INVALIDARG and is
//      reported as captionApplied=false -- the test then applies the dark-mode
//      bound instead of pretending the caption matched.
//   3. DWMWA_BORDER_COLOR (Windows 11 22000+). The 1 px --border-specular-rim
//      rim joins the shell to the canvas. Optional by design: a failure is
//      reported, not retried.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C83_MUTANT_NO_DWM compiles the three
// DwmSetWindowAttribute calls out. The suite's title-bar brightness assertion
// MUST go red, and the mutation verdict is armed on the record proving the
// injection ran (see Tests/win/test_c83_chrome.cpp).
#pragma once

#include <windows.h>

#include <dwmapi.h>

#include "../PrismCore/ChromeTheme.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace prism::win::chrome {

// The two documented spellings of "dark caption" across Windows 10 builds.
inline constexpr DWORD kImmersiveDarkMode20H1 = 20;
inline constexpr DWORD kImmersiveDarkMode1809 = 19;
// Windows 11 22000+ attributes (ignored with a failure code elsewhere).
inline constexpr DWORD kBorderColorAttribute = 34;
inline constexpr DWORD kCaptionColorAttribute = 35;

// What the OS actually accepted. Kept whole because "the title bar is dark" is
// not a boolean: the suite asserts WHICH attribute answered, and the Windows 10
// lane has to know that the caption colour was refused.
struct TitleBarRecord {
  bool attempted = false;       // an HWND was given and Apply() ran
  bool darkApplied = false;     // a dark-mode attribute was accepted
  DWORD darkAttribute = 0;      // 20 or 19 (0: none)
  HRESULT darkResult = E_FAIL;  // last failure, for the log
  int probes = 0;               // how many attribute numbers were tried
  bool captionApplied = false;  // DWMWA_CAPTION_COLOR accepted (Win11)
  bool borderApplied = false;   // DWMWA_BORDER_COLOR accepted (Win11)
  COLORREF caption = 0;
  COLORREF border = 0;
  prism::theme::Variant variant = prism::theme::Variant::Midnight;
};

inline COLORREF ToColorRef(const prism::theme::Rgba& color) {
  const auto channel = [](float value) -> int {
    if (!(value > 0.0f)) return 0;
    if (value >= 1.0f) return 255;
    return static_cast<int>(value * 255.0f + 0.5f);
  };
  return RGB(channel(color.r), channel(color.g), channel(color.b));
}

// 20 first, then 19. The first accepted number is recorded; a total refusal
// leaves darkApplied=false and the title bar simply keeps the system look.
inline bool SetDarkModeAttribute(HWND hwnd, bool dark, TitleBarRecord* record) {
  const BOOL value = dark ? TRUE : FALSE;
  const DWORD candidates[] = {kImmersiveDarkMode20H1, kImmersiveDarkMode1809};
  for (const DWORD attribute : candidates) {
    ++record->probes;
    const HRESULT hr =
        ::DwmSetWindowAttribute(hwnd, attribute, &value, sizeof(value));
    if (SUCCEEDED(hr)) {
      record->darkApplied = true;
      record->darkAttribute = attribute;
      return true;
    }
    record->darkResult = hr;
  }
  return false;
}

// Apply the shell to one window. Never throws, never assumes a Windows version:
// every unsupported attribute is a recorded failure.
inline TitleBarRecord Apply(HWND hwnd, prism::theme::Variant variant) {
  TitleBarRecord record;
  record.variant = variant;
  if (hwnd == nullptr || ::IsWindow(hwnd) == FALSE) return record;
  record.attempted = true;

  // C106: the caption colour is prism::chrome::ShellSurface -- the SAME call the
  // owner-drawn menu plate makes (GalleryWindow::RebuildMenu). Two APIs, one
  // colour, one function: the pixel gate that compares the two surfaces is
  // measuring a property of the source, not a coincidence of two lookups.
  const prism::chrome::Rgba surface = prism::chrome::ShellSurface(variant);
  const prism::chrome::Palette palette = prism::chrome::PaletteFor(variant);
  const bool dark = prism::chrome::IsDarkSurface(surface);

#if defined(PRISM_C83_MUTANT_NO_DWM)
  // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix state in one line -- no DWM
  // call at all, so the caption keeps the system's light chrome. The suite's
  // brightness assertion must go red; 'probes' stays 0, which is the "the
  // injection really ran" predicate the mutation verdict is armed on.
  record.caption = ToColorRef(surface);
  record.border = ToColorRef(palette.rim);
  (void)dark;
  return record;
#else
  SetDarkModeAttribute(hwnd, dark, &record);
  record.caption = ToColorRef(surface);
  record.border = ToColorRef(palette.rim);
  const HRESULT captionResult = ::DwmSetWindowAttribute(
      hwnd, kCaptionColorAttribute, &record.caption, sizeof(record.caption));
  record.captionApplied = SUCCEEDED(captionResult);
  const HRESULT borderResult = ::DwmSetWindowAttribute(
      hwnd, kBorderColorAttribute, &record.border, sizeof(record.border));
  record.borderApplied = SUCCEEDED(borderResult);
  return record;
#endif
}

}  // namespace prism::win::chrome
