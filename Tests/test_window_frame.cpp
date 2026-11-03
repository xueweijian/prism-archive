// PrismArchive -- PrismCore
// C66: the persisted window frame policy, asserted as PURE integer math.
//
// The user-facing defect: a 1536x816 work area, a window written back as
// 1552x832 -- the work area plus the DWM invisible border on both axes -- and
// the last three action buttons off the right edge of the screen. The Win32
// half (DWM extended bounds, MonitorFromRect, SetWindowPos) is proved by
// win.c66_window; THIS file proves the rules that half obeys, so a Linux lane
// catches a regression in the clamp before a Windows runner does.
//
// Every case below is a RECTANGLE IN, RECTANGLE OUT statement. Nothing here
// waits, samples pixels or reads the clock (CONTRACTS R2).
#include <cstdint>
#include <cstdio>

#include "mini_test.hpp"

#include "../Src/PrismCore/WindowLayout.hpp"

namespace {

using prism::layout::ClampFrameToWorkArea;
using prism::layout::FrameBorders;
using prism::layout::FrameFitsWorkArea;
using prism::layout::FrameToPersist;
using prism::layout::Rect;
using prism::layout::VisibleFrameForWindowRect;
using prism::layout::WindowRectForVisibleFrame;

#if defined(PRISM_C66_MUTANT_NO_WORKAREA_CLAMP)
constexpr const char* kMutation = "no_workarea_clamp";
#else
constexpr const char* kMutation = nullptr;
#endif

// The user's machine, to the pixel: 1536 x 816 of usable screen.
constexpr Rect kWork{0, 0, 1536, 816};

bool Inside(const Rect& inner, const Rect& outer) { return outer.Contains(inner); }

}  // namespace

int main(int argc, char** argv) {
  // CONTRACTS D27: the verdict is armed BEFORE the first case and printed at the
  // first red assertion. 'kMutation' is the compile-time proof the injection is
  // in this binary; a build without it has no prefix and cannot claim CAUGHT.
  if (kMutation != nullptr) {
    std::printf("C66 MUTANT ACTIVE: %s\n", kMutation);
    ::prism::test::SetMutationVerdict(
        "C66 MUTATION CAUGHT: no_workarea_clamp",
        [] { return prism::layout::ClampFrameToWorkArea(Rect{0, 0, 2000, 2000}, kWork) ==
                    Rect{0, 0, 2000, 2000}; });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  // CONTRACTS D1: the marker is part of the verdict, so a silent run can never
  // be mistaken for a pass.
  if (failures == 0) std::puts("ALL WINDOW-FRAME CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// The reported defect, as one assertion: 1552x832 on a 1536x816 screen.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 an oversized saved frame is clamped into the work area") {
  const Rect saved{0, 0, 1552, 832};
  PRISM_CHECK(!FrameFitsWorkArea(saved, kWork));  // the defect, stated
  const Rect out = ClampFrameToWorkArea(saved, kWork);
  PRISM_CHECK(Inside(out, kWork));
  PRISM_CHECK_EQ(out.Width(), 1536);
  PRISM_CHECK_EQ(out.Height(), 816);
  PRISM_CHECK_EQ(out.left, 0);
  PRISM_CHECK_EQ(out.top, 0);
}

PRISM_TEST("c66 a frame that already fits is returned unchanged (a fixed point)") {
  const Rect saved{100, 50, 1300, 750};
  PRISM_CHECK(FrameFitsWorkArea(saved, kWork));
  const Rect out = ClampFrameToWorkArea(saved, kWork);
  PRISM_CHECK(out == saved);
  // Applying the clamp twice must not move it: a restore that is not idempotent
  // is the drift the five-cycle test exists to catch.
  PRISM_CHECK(ClampFrameToWorkArea(out, kWork) == out);
}

PRISM_TEST("c66 a frame saved from a monitor that is gone lands on screen") {
  // Negative coordinates are legal (a second monitor to the left). The clamp
  // must bring the LEFT edge in, not just the right one.
  const Rect saved{-2400, -300, -400, 500};
  const Rect out = ClampFrameToWorkArea(saved, kWork);
  PRISM_CHECK(Inside(out, kWork));
  PRISM_CHECK(out.left >= kWork.left);
  PRISM_CHECK(out.top >= kWork.top);
}

PRISM_TEST("c66 a frame hanging off the right/bottom edge is pulled back") {
  const Rect saved{1200, 700, 2400, 1500};
  const Rect out = ClampFrameToWorkArea(saved, kWork);
  PRISM_CHECK(Inside(out, kWork));
  PRISM_CHECK(out.right <= kWork.right);
  PRISM_CHECK(out.bottom <= kWork.bottom);
  PRISM_CHECK_EQ(out.Width(), 1200);  // only moved, not shrunk: it fits
  PRISM_CHECK_EQ(out.Height(), 800);
}

PRISM_TEST("c66 a frame below the minimum grows to the documented minimum") {
  const Rect saved{10, 10, 110, 110};  // 100 x 100: too small to lay out
  const Rect out = ClampFrameToWorkArea(saved, kWork);
  PRISM_CHECK_EQ(out.Width(), prism::layout::kMinWindowWidth);
  PRISM_CHECK_EQ(out.Height(), prism::layout::kMinWindowHeight);
  PRISM_CHECK(Inside(out, kWork));
}

PRISM_TEST("c66 a work area smaller than the minimum wins over the minimum") {
  // A 320 px screen must not produce a 640 px window hanging off it: rule (2)
  // is "the minimum, but never more than the work area can hold".
  const Rect tiny{0, 0, 320, 240};
  const Rect out = ClampFrameToWorkArea(Rect{0, 0, 1200, 800}, tiny);
  PRISM_CHECK(Inside(out, tiny));
  PRISM_CHECK_EQ(out.Width(), 320);
  PRISM_CHECK_EQ(out.Height(), 240);
}

PRISM_TEST("c66 a work area on a second monitor is honoured verbatim") {
  const Rect right{1920, 0, 3456, 1080};
  const Rect out = ClampFrameToWorkArea(Rect{3000, 900, 3800, 1400}, right);
  PRISM_CHECK(Inside(out, right));
  PRISM_CHECK(out.left >= right.left);
}

// ---------------------------------------------------------------------------
// The shadow border: window rect <-> visible frame, in both directions.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 the window rect is the visible frame plus the invisible border") {
  const FrameBorders borders{8, 8, 8, 8};  // the measured 16 px per axis
  const Rect visible{200, 100, 1400, 900};
  const Rect windowRect = WindowRectForVisibleFrame(visible, borders);
  PRISM_CHECK(windowRect == (Rect{192, 92, 1408, 908}));
  // ...and the inverse is the identity, which is what makes save/restore a
  // fixed point rather than "+8 px every close".
  PRISM_CHECK(VisibleFrameForWindowRect(windowRect, borders) == visible);
  PRISM_CHECK(FrameToPersist(windowRect, borders) == visible);
}

PRISM_TEST("c66 persisting the visible frame keeps a maximized window's number sane") {
  // A maximized window on the user's screen: GetWindowRect is the work area
  // inflated by the border, the visible frame is the work area itself.
  const FrameBorders borders{8, 8, 8, 8};
  const Rect maximizedWindow{-8, -8, 1544, 824};
  const Rect persisted = FrameToPersist(maximizedWindow, borders);
  PRISM_CHECK_EQ(persisted.Width(), 1536);
  PRISM_CHECK_EQ(persisted.Height(), 816);
  PRISM_CHECK(FrameFitsWorkArea(persisted, kWork));
  // The OLD rule (persist GetWindowRect) produced exactly the reported number.
  PRISM_CHECK_EQ(maximizedWindow.Width(), 1552);
  PRISM_CHECK_EQ(maximizedWindow.Height(), 832);
}
