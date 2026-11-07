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

#include <algorithm>  // (std::min): a parenthesised call, B1
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
// What we WANT to suggest. If the desktop is smaller, the test shrinks it (see
// the precondition in the case) -- the assertion is always against the numbers
// actually put in lParam, never against these.
constexpr int kSuggestedWidth = 1200;
constexpr int kSuggestedHeight = 900;
// Headroom under ptMaxTrackSize: the bound is enforced by DefWindowProc while
// the window is sized, so the request must sit strictly inside it rather than on
// the edge (a one-pixel disagreement would read as "we ignored lParam").
constexpr int kTrackMargin = 64;

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
  //
  // PRECONDITION (learned from a red CI round): the suggested size must fit the
  // desktop. DefWindowProc clamps a sizing request to the window's MAXIMUM
  // tracking size (WM_GETMINMAXINFO -> ptMaxTrackSize, SM_CXMAXTRACK by
  // default), and a headless runner desktop is small -- the first version of
  // this test asked for 1200x900 on a 1024x768 session and the OS answered
  // 1044x760, which looked like "we ignored lParam" but was Windows refusing to
  // make a window bigger than the desktop. Asking the window itself for the
  // bound is the faithful source: it is the very number SetWindowPos will
  // enforce. The margin keeps the request strictly inside it.
  MINMAXINFO limit{};
  ::SendMessageW(window.Hwnd(), WM_GETMINMAXINFO, 0, reinterpret_cast<LPARAM>(&limit));
  int maxTrackW = limit.ptMaxTrackSize.x;
  int maxTrackH = limit.ptMaxTrackSize.y;
  if (maxTrackW <= 0) maxTrackW = ::GetSystemMetrics(SM_CXMAXTRACK);
  if (maxTrackH <= 0) maxTrackH = ::GetSystemMetrics(SM_CYMAXTRACK);
  const int width = (std::min)(kSuggestedWidth, maxTrackW - kTrackMargin);
  const int height = (std::min)(kSuggestedHeight, maxTrackH - kTrackMargin);
  std::printf("c120 d3: maxTrack=%dx%d -> suggesting %dx%d outer\n", maxTrackW, maxTrackH,
              width, height);
  PRISM_CHECK(width > 0);
  PRISM_CHECK(height > 0);

  RECT suggested{kSuggestedLeft, kSuggestedTop, kSuggestedLeft + width,
                 kSuggestedTop + height};
  window.Dispatch(WM_DPICHANGED, MAKEWPARAM(kNewDpi, kNewDpi),
                  reinterpret_cast<LPARAM>(&suggested));

  RECT applied{};
  ::GetWindowRect(window.Hwnd(), &applied);
  std::printf("c120 d3: after WM_DPICHANGED dpi=%d client=%dx%d outer=%ld,%ld %ldx%ld\n",
              window.DpiForTest(), window.ClientWidthForTest(), window.ClientHeightForTest(),
              applied.left, applied.top, applied.right - applied.left,
              applied.bottom - applied.top);

  // (a) the window is now laid out at the message's DPI.
  PRISM_CHECK(window.DpiForTest() == static_cast<int>(kNewDpi));

  // (b) SetWindowPos really ran, with lParam's rect -- all four edges, against
  // the numbers this case actually suggested.
  PRISM_CHECK(applied.left == kSuggestedLeft);
  PRISM_CHECK(applied.top == kSuggestedTop);
  PRISM_CHECK(applied.right - applied.left == width);
  PRISM_CHECK(applied.bottom - applied.top == height);

  // (c) the LAYOUT is in DIPs: the client area the tokens see is the physical one
  // 96/144. A window that kept pixel math here would report the raw pixel width
  // and paint the 96 DPI design into a 1.5x larger frame.
  RECT client{};
  ::GetClientRect(window.Hwnd(), &client);
  PRISM_CHECK(client.right > 0);
  PRISM_CHECK(client.bottom > 0);
  const int dpi = static_cast<int>(kNewDpi);
  const int expectedW = ::MulDiv(static_cast<int>(client.right), 96, dpi);
  const int expectedH = ::MulDiv(static_cast<int>(client.bottom), 96, dpi);
  // MulDiv ROUNDS to the nearest DIP; a cast truncates. The two disagree by one
  // whenever the client size is not a whole multiple of the scale -- CI's first
  // geometry (964 px at 144 DPI = 642.67 DIP) was exactly such a case and this
  // assertion caught the product truncating. The line below says whether THIS
  // run's geometry is one of those, so a green is not mistaken for a proof.
  const int truncatedW = static_cast<int>(static_cast<long long>(client.right) * 96 / dpi);
  const int truncatedH = static_cast<int>(static_cast<long long>(client.bottom) * 96 / dpi);
  std::printf("c120 d3: client px=%ldx%ld -> dip=%dx%d (truncating would say %dx%d)\n",
              client.right, client.bottom, expectedW, expectedH, truncatedW, truncatedH);
  PRISM_CHECK(window.ClientWidthForTest() == expectedW);
  PRISM_CHECK(window.ClientHeightForTest() == expectedH);
  if (expectedW != truncatedW || expectedH != truncatedH) {
    std::puts("c120 d3: this geometry is a ROUNDING case (round != truncate)");
  }
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