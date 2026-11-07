// PrismArchive -- PrismWin
// C120, WINDOW LEVEL: WM_DPICHANGED really runs (D3).
//
// The pre-card window had NO WM_DPICHANGED branch at all -- and could not have
// needed one, because a DPI-UNAWARE process is never sent that message. The same
// is true of the handlers the dialogs already carry: they were dead code. This
// suite proves the branch is LIVE and does the two things a per-monitor window
// owes Windows:
//
//   (a) it APPLIES lParam's suggested rect through SetWindowPos -- the four
//       numbers in the message, not a re-derived guess (a window that answers
//       with its own geometry jumps and resizes wrongly as it crosses monitors);
//   (b) it reflows the LAYOUT at the new scale. Layout tokens are DIPs (96 ==
//       100%), so a 1200 px client area at 144 DPI is 800 DIPs wide: the same
//       numbers paint at 1.5x instead of the frame being bitmap-scaled.
//
// WHY SYNTHESISE THE MESSAGE. A CI runner has one monitor at 96 DPI, so no real
// per-monitor move can be produced there; Dispatch() is the same Proc the
// message loop calls, so what is exercised is the production branch, not a copy.
// The alternative -- asserting on GetDpiForWindow -- cannot work on a 96 DPI
// machine: Windows would answer 96 whether the process is aware or not, which is
// exactly the vacuous test this card must not ship.
//
// NEGATIVE CONTROL (CONTRACTS D3 + D27): PRISM_C120_MUTANT_NO_DPICHANGED makes
// the branch return immediately -- the pre-card "dead code" state -- and this
// same source must go red, printing "C120 DPICHANGED MUTATION CAUGHT: ..." at the
// first failing assertion. CTest's PASS_REGULAR_EXPRESSION is the verdict; there
// is no WILL_FAIL.
#include <windows.h>

#include <cstdio>

#include "../mini_test.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

using GalleryWindow = prism::win::gallery::GalleryWindow;

namespace {

constexpr UINT kNewDpi = 144;
constexpr int kSuggestedLeft = 100;
constexpr int kSuggestedTop = 120;
constexpr int kSuggestedWidth = 1200;
constexpr int kSuggestedHeight = 900;

}  // namespace

PRISM_TEST("c120.d3: WM_DPICHANGED applies lParam's rect and reflows the layout in DIPs") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C120 DPI", GetModuleHandleW(nullptr)))) return;
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 720));

  const int authoredDpi = window.DpiForTest();
  std::printf("c120 d3: at rest dpi=%d client=%dx%d\n", authoredDpi,
              window.ClientWidthForTest(), window.ClientHeightForTest());
  // 96 is the answer Windows gives an unaware process, and the scale every token
  // in this window was authored at. (A per-monitor process on a 120 DPI monitor
  // answers 120 here instead; the assertion below holds either way because the
  // window converts its own WM_SIZE with the same number.)
  PRISM_CHECK(authoredDpi >= 96);
  PRISM_CHECK(window.ClientWidthForTest() == ::MulDiv(1280, 96, authoredDpi));

  // A monitor move: the OS sends the new DPI in wParam and a suggested rect in
  // lParam. Windows' own rule is that the app resizes to EXACTLY those numbers.
  RECT suggested{kSuggestedLeft, kSuggestedTop, kSuggestedLeft + kSuggestedWidth,
                 kSuggestedTop + kSuggestedHeight};
  window.Dispatch(WM_DPICHANGED, MAKEWPARAM(kNewDpi, kNewDpi),
                  reinterpret_cast<LPARAM>(&suggested));

  std::printf("c120 d3: after WM_DPICHANGED dpi=%d client=%dx%d\n", window.DpiForTest(),
              window.ClientWidthForTest(), window.ClientHeightForTest());

  // (a) the window is now laid out at the message's DPI.
  PRISM_CHECK(window.DpiForTest() == static_cast<int>(kNewDpi));

  // (b) SetWindowPos really ran, with lParam's rect -- all four edges.
  RECT frame{};
  ::GetWindowRect(window.Hwnd(), &frame);
  PRISM_CHECK(frame.left == kSuggestedLeft);
  PRISM_CHECK(frame.top == kSuggestedTop);
  PRISM_CHECK(frame.right - frame.left == kSuggestedWidth);
  PRISM_CHECK(frame.bottom - frame.top == kSuggestedHeight);

  // (c) the LAYOUT is in DIPs: the client area the tokens see is the physical one
  // 96/144. A window that kept pixel math here would report the raw pixel width
  // and paint the 96 DPI design into a 1.5x larger frame.
  RECT client{};
  ::GetClientRect(window.Hwnd(), &client);
  PRISM_CHECK(client.right > 0);
  PRISM_CHECK(client.bottom > 0);
  PRISM_CHECK(window.ClientWidthForTest() ==
              ::MulDiv(static_cast<int>(client.right), 96, static_cast<int>(kNewDpi)));
  PRISM_CHECK(window.ClientHeightForTest() ==
              ::MulDiv(static_cast<int>(client.bottom), 96, static_cast<int>(kNewDpi)));
  // ... and it is a DIFFERENT number from the pixel width (1.5x smaller), which is
  // what makes case (c) a real assertion rather than an identity.
  PRISM_CHECK(window.ClientWidthForTest() < static_cast<int>(client.right));
}

int main(int argc, char** argv) {
#if defined(PRISM_C120_MUTANT_NO_DPICHANGED)
  ::prism::test::SetMutationVerdict("C120 DPICHANGED MUTATION CAUGHT: dead_branch");
#endif
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C120 DPICHANGED CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C120 DPICHANGED CHECKS PASSED");
  return 0;
}
