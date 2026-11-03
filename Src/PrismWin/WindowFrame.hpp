// PrismArchive -- PrismWin
// WindowFrame: the Win32 half of C66's frame policy (the pure half is
// prism::layout's ClampFrameToWorkArea / FrameToPersist).
//
// THE MEASURED DEFECT. A 1536x816 work area, a maximized window, a close: the
// persisted "window" block was 1552x832 -- the work area plus the DWM invisible
// resize border on both axes. Restoring that number with SetWindowPos makes the
// window 16 px wider than the screen, and the tail of the action island (添加 /
// 删除 / 测试) is painted off screen. Two rules are needed, and this header is
// where both meet the OS:
//
//   1. PERSIST the visible frame -- DwmGetWindowAttribute with
//      DWMWA_EXTENDED_FRAME_BOUNDS -- never GetWindowRect. For a MAXIMIZED
//      window even that is the work area, so the geometry that gets stored is
//      GetWindowPlacement's NORMAL rectangle: the size the user had BEFORE
//      maximizing, which is the only size that fits a screen again.
//   2. RESTORE through ClampFrameToWorkArea(MonitorFromRect(...)), then convert
//      the clamped VISIBLE frame back to a window rect by adding the border the
//      live window actually has. The window rect is finally clamped to the work
//      area as well, so GetWindowRect itself is inside the screen -- that is the
//      assertion the suite makes, because it is what the user can see.
//
// Both directions are measured on the SAME window, which is what makes the
// cycle a fixed point instead of a drift: saving a visible 1200x800 and
// restoring it produces a window whose visible frame is 1200x800 again.
//
// CONTRACTS B1: every min/max is parenthesised. B2: this header includes what
// it uses. D3: PRISM_C66_MUTANT_NO_WORKAREA_CLAMP and
// PRISM_C66_MUTANT_NO_DWM are the two negative controls, and BOTH are armed
// through the same functions the product calls.
#pragma once

#include <windows.h>

#include <dwmapi.h>

#include <algorithm>

#include "../PrismCore/WindowLayout.hpp"

#pragma comment(lib, "dwmapi.lib")

namespace prism::win::frame {

using prism::layout::FrameBorders;
using prism::layout::Rect;

// One window, measured once: what GetWindowRect says, what the user sees, and
// the difference. 'dwm' false means the extended frame was not available (a
// bare Win32 window, or a compositor that refused) and 'visible' fell back to
// the window rect with zero borders.
struct Measurement {
  Rect window;
  Rect visible;
  FrameBorders borders;
  bool dwm = false;

  bool operator==(const Measurement&) const = default;
};

inline Rect ReadWindowRect(HWND hwnd) {
  RECT rect{};
  if (hwnd == nullptr || ::GetWindowRect(hwnd, &rect) == FALSE) return Rect{};
  return Rect{rect.left, rect.top, rect.right, rect.bottom};
}

// The extended (visible) frame. DwmGetWindowAttribute returns a RECT in screen
// coordinates for the SAME window; the difference to GetWindowRect is the
// invisible border C66 is about.
inline bool ReadVisibleFrame(HWND hwnd, Rect* out) {
  if (hwnd == nullptr || out == nullptr) return false;
#if defined(PRISM_C66_MUTANT_NO_DWM)
  // NEGATIVE CONTROL (CONTRACTS D3): the shadow border is counted as if it were
  // part of the window, which is exactly the pre-fix behaviour. The five-cycle
  // stability assertion MUST go red; the marker is only printed when the
  // injection is proved active, so an unrelated red can never claim CAUGHT.
  (void)hwnd;
  *out = ReadWindowRect(hwnd);
  return false;
#else
  RECT rect{};
  const HRESULT hr = ::DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect,
                                             static_cast<DWORD>(sizeof(rect)));
  if (FAILED(hr)) return false;
  if (rect.right <= rect.left || rect.bottom <= rect.top) return false;
  *out = Rect{rect.left, rect.top, rect.right, rect.bottom};
  return true;
#endif
}

inline Measurement Measure(HWND hwnd) {
  Measurement m;
  m.window = ReadWindowRect(hwnd);
  Rect visible;
  if (ReadVisibleFrame(hwnd, &visible)) {
    m.visible = visible;
    m.dwm = true;
    m.borders = FrameBorders{visible.left - m.window.left, visible.top - m.window.top,
                             m.window.right - visible.right,
                             m.window.bottom - visible.bottom};
    // A negative border means the compositor answered with something that is
    // not this window's frame; fall back rather than move the window by it.
    if (m.borders.left < 0 || m.borders.top < 0 || m.borders.right < 0 ||
        m.borders.bottom < 0) {
      m.borders = FrameBorders{};
      m.visible = m.window;
      m.dwm = false;
    }
  } else {
    m.visible = m.window;
    m.borders = FrameBorders{};
  }
  return m;
}

// The work area of the monitor closest to 'rect'. MonitorFromRect never fails:
// MONITOR_DEFAULTTONEAREST answers the primary monitor when the point is on no
// monitor at all, which is what makes a stale saved rectangle from an unplugged
// second screen land on the screen the user still has.
inline Rect WorkAreaFor(const Rect& rect) {
  RECT probe{rect.left, rect.top, rect.right, rect.bottom};
  const HMONITOR monitor = ::MonitorFromRect(&probe, MONITOR_DEFAULTTONEAREST);
  if (monitor != nullptr) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
      return Rect{info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    }
  }
  return Rect{0, 0, ::GetSystemMetrics(SM_CXSCREEN), ::GetSystemMetrics(SM_CYSCREEN)};
}

inline Rect WorkAreaOfWindow(HWND hwnd) {
  const HMONITOR monitor = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
  if (monitor != nullptr) {
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(monitor, &info)) {
      return Rect{info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom};
    }
  }
  return WorkAreaFor(Rect{});
}

// The frame the app should WRITE for a given visible frame: clamped into the
// work area of THIS window's monitor.
//
// Clamping on the way OUT as well as on the way IN is not belt-and-braces -- it
// is what makes "the settings file never holds a frame the screen cannot show"
// true for EVERY path. Without it, a window whose normal rectangle is larger
// than the desktop (created at a default size on a small screen, or carried over
// from a monitor that was unplugged) writes that oversized number and is only
// corrected by the NEXT restore. The CI runner is exactly that machine: a
// default-sized window on a smaller desktop made FrameToPersist return a frame
// wider than rcWork, and win.c66_window caught it (CONTRACTS R1: the runner was
// right and the local desktop was the coincidence).
//
// The floor stays the schema's documented 640x480 (SettingsStore window.width /
// window.height): ClampFrameToWorkArea never returns less than the minimum
// unless the work area itself is smaller, which is the degenerate case the
// restore path also has to survive.
inline Rect PersistFrame(HWND hwnd, const Rect& visible) {
  if (hwnd == nullptr) return visible;
  const Rect work = WorkAreaOfWindow(hwnd);
#if defined(PRISM_C66_MUTANT_NO_PERSIST_CLAMP)
  // NEGATIVE CONTROL (CONTRACTS D3): write the frame as measured, unclamped.
  // The suite feeds an EXPLICIT frame bigger than the work area, so it goes red
  // on any desktop size -- the assertion never depends on the runner's screen.
  (void)work;
  return visible;
#else
  return prism::layout::ClampFrameToWorkArea(visible, work);
#endif
}

// What the app should write to settings.json for THIS window. A maximized (or
// minimized) window answers with its NORMAL rectangle: persisting the maximized
// window rect is rule (1) above, and it is what produced 1552x832. Either way
// the answer goes through PersistFrame, so it fits the work area.
inline Rect FrameToPersist(HWND hwnd) {
  if (hwnd == nullptr) return Rect{};
#if defined(PRISM_C66_MUTANT_NO_DWM)
  // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix rule in one line -- persist
  // GetWindowRect, which is the visible frame PLUS the shadow border, and is the
  // work area plus that border when the window is maximized (the user's
  // 1552x832 on a 1536x816 screen). Deliberately NOT clamped: the pre-fix code
  // did not clamp either, and the suite's "what gets persisted fits the work
  // area" assertion is what has to catch it.
  return ReadWindowRect(hwnd);
#else
  WINDOWPLACEMENT placement{};
  placement.length = sizeof(placement);
  const bool placed = ::GetWindowPlacement(hwnd, &placement) != FALSE;
  const bool zoomed = ::IsZoomed(hwnd) != FALSE;
  const Measurement m = Measure(hwnd);
  if (placed && (zoomed || ::IsIconic(hwnd) != FALSE)) {
    const RECT& normal = placement.rcNormalPosition;
    const Rect normalWindow{normal.left, normal.top, normal.right, normal.bottom};
    return PersistFrame(hwnd,
                        prism::layout::VisibleFrameForWindowRect(normalWindow, m.borders));
  }
  return PersistFrame(hwnd, m.visible);
#endif
}

// The outcome of one restore, kept whole so the caller (and the suite) can
// assert on what was ASKED for, what the work area was, and what the window
// ended up being -- a boolean would hide exactly the numbers that matter.
struct Restore {
  bool ok = false;
  bool moved = false;      // the position had to change to stay on screen
  Rect saved;              // the persisted VISIBLE frame
  Rect work;               // the work area it was clamped into
  Rect requested;          // the clamped VISIBLE frame
  Rect applied;            // GetWindowRect after SetWindowPos
  Rect visible;            // measured visible frame after SetWindowPos
  FrameBorders borders;
};

// Restore the persisted frame. Order matters and is the fix:
//   measure -> clamp the VISIBLE frame into the work area -> add the border of
//   THIS window -> clamp the resulting window rect into the work area -> move
//   and size -> measure.
inline Restore RestoreVisibleFrame(HWND hwnd, const Rect& saved,
                                   int minWidth = prism::layout::kMinWindowWidth,
                                   int minHeight = prism::layout::kMinWindowHeight) {
  Restore out;
  out.saved = saved;
  if (hwnd == nullptr) return out;
  const Measurement before = Measure(hwnd);
  out.borders = before.borders;

  out.work = WorkAreaFor(saved);
  if (out.work.Empty()) out.work = WorkAreaOfWindow(hwnd);
  if (out.work.Empty()) return out;

  // (2a) the user's frame, clamped.
  out.requested = prism::layout::ClampFrameToWorkArea(saved, out.work, minWidth, minHeight);
  // (2b) the window rectangle that produces that visible frame, clamped again so
  // GetWindowRect itself stays on screen (the card's assertion, and the only
  // way the right-hand buttons can never be cut off).
  const Rect wantedWindow =
      prism::layout::WindowRectForVisibleFrame(out.requested, out.borders);
  const Rect targetWindow = prism::layout::ClampFrameToWorkArea(
      wantedWindow, out.work, minWidth, minHeight);

  const bool haveWindowRect = !before.window.Empty();
  out.moved = !haveWindowRect || before.window.left != targetWindow.left ||
              before.window.top != targetWindow.top;
  const BOOL sized = ::SetWindowPos(hwnd, nullptr, targetWindow.left, targetWindow.top,
                                    targetWindow.Width(), targetWindow.Height(),
                                    SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
  if (sized == FALSE) return out;
  out.ok = true;
  out.applied = ReadWindowRect(hwnd);
  out.visible = Measure(hwnd).visible;
  return out;
}

}  // namespace prism::win::frame
