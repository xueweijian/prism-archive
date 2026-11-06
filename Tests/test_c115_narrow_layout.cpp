// PrismArchive -- PrismCore tests
// C115: the breadcrumb pill and the action island share the 56 px band, and at
// narrow widths they OVERLAPPED -- the toolbar sat on the breadcrumb.
//
// THE DEFECT, measured before this card (same inputs the card quotes:
// prism::layout::Compute(w, 729, sidebar=true, listMode=false, tabs=true) and
// actionbar::Compute):
//
//   w=1028  NavPillRect=(16..576)   island=(432..1012)   overlap 144 px
//   w=1264  NavPillRect=(16..576)   island=(588..1248)   overlap 0
//   w=1482  NavPillRect=(16..576)   island=(588..1360)   overlap 0
//
// THE BOUNDARY (measured, 900 -> 1400, one pixel at a time): the overlap is NOT
// first introduced at 1028. It exists at the FIRST sample (900) and at every
// width up to and including 1171; 1172 is the first clean width (there the
// island's left edge lands exactly on 1172 - 596 = 576, the V1 pill's right).
// The overlap is exactly (1172 - w) px, so 1028 is simply where C112
// photographed it: 1172 - 1028 = 144, the number in the card.
// The pill only keeps its full 560 px from 1184 up (1184 = 16+560+12+580+16);
// 1172..1183 squeeze it by 1..11 px while the island still clears it.
//
// WHY: two different rectangles called themselves "the breadcrumb pill".
// WindowLayout::NavPillRect (the geometry authority) always returned V1's 560 px
// pill, while actionbar::Compute squeezed its own copy so the toolbar could
// fit. The renderer painted the squeezed one; the glass island (the composition
// panel that the window positions from NavPillRect) and every probe that asked
// the authority were 144 px too wide at 1028 -- i.e. the acrylic island really
// was under the first toolbar button.
//
// THE FIX (option 2 of the card): ONE split rule. NavPillRect itself yields
// width to the action island's floor (kActionIslandMinWidth), and
// actionbar::Compute consumes that SAME function, so the authority, the paint,
// the hit test and the glass panel cannot disagree.
//
// WHAT IS ASSERTED:
//   1. 900..1920 step 4 (the card's acceptance): pill and island never
//      intersect; the authority's pill IS the band's pill; the island keeps
//      kNavGap of air to its right; every button is inside the band.
//   2. 1264 / 1482 are byte-for-byte the pre-card geometry (the baseline the
//      rest of the redesign wave was measured on).
//   3. The crumb text rect never leaves the pill and is never inverted, and the
//      window's crumb spans (LayoutCenteredBreadcrumb) stay inside it.
//   4. Degenerate widths: the split never invents a negative or inverted rect.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the SAME source compiled with
// PRISM_C115_MUTANT_IGNORE_NARROW restores "the pill never yields". Assertion 1
// must go red and the suite must print
//   C115 MUTATION CAUGHT: ignore_narrow
// from the FIRST failure (exit 0), registered with PASS_REGULAR_EXPRESSION --
// never WILL_FAIL.
#include <cstdio>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "ActionBarModel.hpp"
#include "ArchiveTreeView.hpp"
#include "WindowLayout.hpp"

using prism::layout::Rect;

namespace {

constexpr int kSweepFirst = 900;
constexpr int kSweepLast = 1920;
constexpr int kSweepStep = 4;
constexpr int kBandHeight = 729;  // the card's client height
// 1172 is the measured boundary (see the header comment): the first width at
// which the pill keeps all 560 px and the island clears it. Every narrower
// width overlaps by exactly (1172 - w) px before the fix.
constexpr int kCleanFrom = 1172;   // first width with no overlap
constexpr int kFullPillFrom = 1184;  // first width where the pill keeps 560 px

// Only the mutant build declares these: a clean build that kept them would trip
// -Wunused-const-variable on the GCC/Clang lane (CONTRACTS D31).
#if defined(PRISM_C115_MUTANT_IGNORE_NARROW)
const char* const kMutantMarker = "C115 MUTATION CAUGHT: ignore_narrow";
const char* const kMutantBanner = "C115 MUTANT ACTIVE: ignore_narrow";
bool gNarrowAdjustmentObserved = false;
#endif

prism::layout::Layout Window(int w) {
  return prism::layout::Compute(w, kBandHeight, /*sidebarVisible=*/true,
                                /*listMode=*/false, /*tabs=*/true);
}

// The band and the authority's pill for one width, in one place, so every
// assertion below is about the SAME pair of rectangles the product uses.
struct Split {
  prism::layout::Layout window;
  prism::actionbar::Layout band;
  Rect pill;   // prism::layout::NavPillRect(window) -- the authority
  Rect text;   // prism::layout::NavTextRect(window)
};

Split At(int w) {
  Split s;
  s.window = Window(w);
  s.band = prism::actionbar::Compute(s.window);
  s.pill = prism::layout::NavPillRect(s.window);
  s.text = prism::layout::NavTextRect(s.window);
  return s;
}

const std::vector<int>& SweepWidths() {
  static const std::vector<int> widths = [] {
    std::vector<int> out;
    for (int w = kSweepFirst; w <= kSweepLast; w += kSweepStep) out.push_back(w);
    return out;
  }();
  return widths;
}

}  // namespace

PRISM_TEST("c115.sweep: pill and action island never intersect, 900..1920 step 4") {
  // Aggregated on purpose: a per-width PRISM_CHECK would print 4 failures x 256
  // widths on the pre-fix build and bury the one number that matters. One count
  // per rule still goes red on a SINGLE bad width, and the 1028 line below is
  // the card's own measurement.
  int overlaps = 0;
  int firstOverlap = 0;
  int lastOverlap = 0;
  int widestOverlap = 0;
  int pillMismatches = 0;      // authority's pill != band's pill
  int pillOnIsland = 0;        // the band's own pill touching its island
  int missingAir = 0;          // island.left != pill.right + kNavGap
  int buttonsOnPill = 0;       // a button overlapping the pill
  int outsideBand = 0;         // a button outside actionIsland
  int wrongCount = 0;          // buttons.size() != 8 or dropped != 0
  int widths = 0;
  for (const int w : SweepWidths()) {
    const Split s = At(w);
    ++widths;
    // The two occupants of the band are disjoint -- tolerance 0 (Rect::Intersects
    // is half-open, so touching edges are NOT an overlap).
    if (s.pill.Intersects(s.band.island)) {
#if defined(PRISM_C115_MUTANT_IGNORE_NARROW)
      gNarrowAdjustmentObserved = true;  // the injected overlap really happened
#endif
      const int px = (std::min)(s.pill.right, s.band.island.right) -
                     (std::max)(s.pill.left, s.band.island.left);
      if (overlaps == 0) firstOverlap = w;
      lastOverlap = w;
      widestOverlap = (std::max)(widestOverlap, px);
      ++overlaps;
      if (w == 1028) {
        std::printf("  C115 overlap at w=1028: pill=(%d..%d) island=(%d..%d) = %d px\n",
                    s.pill.left, s.pill.right, s.band.island.left, s.band.island.right, px);
      }
    }
    if (!(s.pill == s.band.navPill)) ++pillMismatches;
    if (s.band.navPill.Intersects(s.band.island)) ++pillOnIsland;
    // "Adjacent" is stronger than "disjoint": exactly kNavGap of air.
    if (!s.band.island.Empty() && !s.pill.Empty() &&
        s.band.island.left != s.pill.right + prism::actionbar::kNavGap) {
      ++missingAir;
    }
    for (const auto& button : s.band.buttons) {
      if (button.rect.Intersects(s.pill)) ++buttonsOnPill;
      if (!s.window.actionIsland.Contains(button.rect)) ++outsideBand;
    }
    if (static_cast<int>(s.band.buttons.size()) != prism::actionbar::kActionCount ||
        s.band.dropped != 0) {
      ++wrongCount;
    }
  }
  std::printf("C115 sweep: %d widths %d..%d step %d, overlaps=%d (first=%d last=%d max=%d px)\n",
              widths, kSweepFirst, kSweepLast, kSweepStep, overlaps, firstOverlap, lastOverlap,
              widestOverlap);
  PRISM_CHECK_EQ(widths, (kSweepLast - kSweepFirst) / kSweepStep + 1);
  PRISM_CHECK_EQ(overlaps, 0);
  PRISM_CHECK_EQ(pillMismatches, 0);
  PRISM_CHECK_EQ(pillOnIsland, 0);
  PRISM_CHECK_EQ(missingAir, 0);
  PRISM_CHECK_EQ(buttonsOnPill, 0);
  PRISM_CHECK_EQ(outsideBand, 0);
  PRISM_CHECK_EQ(wrongCount, 0);
}

PRISM_TEST("c115.boundary: 1028 is (1172 - w) px of overlap, 1172 is clean") {
  // The invariant the boundary is made of: whatever the width, the island keeps
  // exactly kNavGap of air to the pill's right and the pill never exceeds V1's
  // 560 px. Pre-fix, every width below 1172 broke the first half by (1172 - w)
  // px -- 144 at 1028 -- even though the band's own copy kept the gap.
  for (const int w : SweepWidths()) {
    const Split s = At(w);
    const int reach = s.band.island.left - s.pill.right;  // air between them
    PRISM_CHECK_EQ(reach, prism::actionbar::kNavGap);  // never negative, never more
    PRISM_CHECK_EQ(s.pill.left, prism::layout::kContentPadding);
    if (w >= kFullPillFrom) {
      PRISM_CHECK_EQ(s.pill.right,
                     prism::layout::kContentPadding + prism::layout::kNavPillMaxWidth);
    } else {
      PRISM_CHECK(s.pill.right <= prism::layout::kContentPadding +
                                      prism::layout::kNavPillMaxWidth);
    }
  }
  // The boundary itself: at 1172 the island's left edge lands exactly on the V1
  // pill's right edge (576) -- the last pixel of the defect, and the first width
  // where there is nothing left to overlap.
  const Split atBoundary = At(kCleanFrom);
  PRISM_CHECK_EQ(atBoundary.band.island.left,
                 prism::layout::kContentPadding + prism::layout::kNavPillMaxWidth);
  PRISM_CHECK(!atBoundary.pill.Intersects(atBoundary.band.island));
  const Split at1028 = At(1028);
  std::printf("C115 boundary: 1172 is the first clean width; at 1028 pill=(%d..%d) "
              "island=(%d..%d)\n",
              at1028.pill.left, at1028.pill.right, at1028.band.island.left,
              at1028.band.island.right);
  PRISM_CHECK(at1028.pill.right < 576);
}

PRISM_TEST("c115.baseline: 1264 and 1482 are the pre-card geometry, to the pixel") {
  // The numbers the rest of the redesign wave was measured on. A narrow-window
  // fix that moved these would invalidate C91/C93/C102/C107/C110's baselines.
  struct Baseline {
    int width;
    Rect pill;
    Rect island;
    Rect text;
  };
  const Baseline kBaselines[] = {
      {1264, Rect{16, 70, 576, 114}, Rect{588, 64, 1248, 120}, Rect{40, 80, 552, 104}},
      {1482, Rect{16, 70, 576, 114}, Rect{588, 64, 1360, 120}, Rect{40, 80, 552, 104}},
  };
  for (const Baseline& expected : kBaselines) {
    const Split s = At(expected.width);
    std::printf("C115 baseline w=%d: pill=(%d..%d) island=(%d..%d) text=(%d..%d)\n",
                expected.width, s.pill.left, s.pill.right, s.band.island.left,
                s.band.island.right, s.text.left, s.text.right);
    PRISM_CHECK_EQ(s.pill, expected.pill);
    PRISM_CHECK_EQ(s.band.island, expected.island);
    PRISM_CHECK_EQ(s.text, expected.text);
    PRISM_CHECK_EQ(s.pill.Width(), prism::layout::kNavPillMaxWidth);
  }
}

PRISM_TEST("c115.text: the crumb run never leaves the (shortened) pill") {
  // A crumb run that is too long must be truncated INSIDE the pill, not drawn
  // under the toolbar. The window's own layout clamps to the text box; here we
  // pin the box itself: inside the pill, never inverted, at every width.
  const std::vector<prism::tree::Segment> segments = [] {
    std::vector<prism::tree::Segment> out;
    out.push_back(prism::tree::Segment{std::string("PrismArchive.7z"),
                                       prism::tree::kRootLevel});
    const char* names[] = {"payload_text", "2026-11", "quarterly_reports", "final"};
    for (int i = 0; i < 4; ++i) {
      out.push_back(prism::tree::Segment{std::string(prism::tree::kBreadcrumbSeparator),
                                         prism::tree::kNoLevel});
      out.push_back(prism::tree::Segment{std::string(names[i]), i});
    }
    return out;
  }();
  // 9 px per character: a deliberately wide measure, so the run OVERFLOWS at the
  // narrow widths and the clamp has to do real work.
  const auto measure = [](std::string_view text) { return static_cast<int>(text.size()) * 9; };

  for (const int w : SweepWidths()) {
    const Split s = At(w);
    PRISM_CHECK(s.pill.Contains(s.text));
    PRISM_CHECK(s.text.Height() > 0);
    PRISM_CHECK(s.text.Width() > 0);
    // The hit test and the paint share this layout: every clickable crumb is
    // inside the text box, and therefore inside the pill.
    const prism::tree::BreadcrumbLayout crumbs = prism::tree::LayoutCenteredBreadcrumb(
        segments, s.text.left, s.text.right, measure);
    PRISM_CHECK(!crumbs.spans.empty());
    for (const auto& span : crumbs.spans) {
      PRISM_CHECK(span.left >= s.text.left);
      PRISM_CHECK(span.right <= s.text.right);
      PRISM_CHECK(s.pill.Contains(Rect{span.left, s.text.top, span.right, s.text.bottom}));
    }
    // And the run really is wider than the box at the narrow end -- otherwise
    // the assertion above would be vacuous.
    if (w == kSweepFirst) PRISM_CHECK(crumbs.total > s.text.Width());
  }
}

PRISM_TEST("c115.degenerate: no invented rectangles at tiny or zero widths") {
  // 640 is the documented minimum window (kMinWindowWidth). The pill is
  // ALLOWED to become a nub there -- the split policy deliberately spends the
  // pill before it spends a button (see the header comment) -- but the two
  // rects must still be disjoint and the text box must not invert.
  const int widths[] = {0, 1, 320, 520, 640, 783, 784, 900};
  for (const int w : widths) {
    const Split s = At(w);
    PRISM_CHECK(!s.pill.Intersects(s.band.island));
    PRISM_CHECK(s.pill.left >= 0);
    PRISM_CHECK(s.pill.right >= s.pill.left);
    PRISM_CHECK(s.text.right >= s.text.left);
    PRISM_CHECK(s.pill.Contains(s.text));
    if (!s.pill.Empty()) PRISM_CHECK(s.window.actionIsland.Contains(s.pill));
  }
  // 640: the window is at its documented minimum and the toolbar is still
  // complete -- the behaviour that made the pill expendable in the first place.
  const Split min = At(640);
  std::printf("C115 degenerate: w=640 pill=(%d..%d) island=(%d..%d) buttons=%zu dropped=%d\n",
              min.pill.left, min.pill.right, min.band.island.left, min.band.island.right,
              min.band.buttons.size(), min.band.dropped);
  PRISM_CHECK_EQ(static_cast<int>(min.band.buttons.size()),
                 prism::actionbar::kActionCount);
  PRISM_CHECK_EQ(min.band.dropped, 0);
  // 520: no room for a complete toolbar, so it is ABSENT (C66's rule), and an
  // absent island cannot overlap anything.
  const Split absent = At(520);
  PRISM_CHECK(absent.band.island.Empty());
  PRISM_CHECK(absent.band.buttons.empty());
}

int main(int argc, char** argv) {
#if defined(PRISM_C115_MUTANT_IGNORE_NARROW)
  std::printf("%s\n", kMutantBanner);
  // D3: CAUGHT is printed only when the injected defect really fired (the sweep
  // saw an actual pill/island overlap), never merely because the macro exists.
  prism::test::SetMutationVerdict(kMutantMarker, [] { return gNarrowAdjustmentObserved; });
#else
  std::puts("C115 MUTANT: none (clean build)");
#endif
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C115 NARROW-LAYOUT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C115 NARROW-LAYOUT CHECKS PASSED");
  return 0;
}
