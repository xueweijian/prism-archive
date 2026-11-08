// PrismArchive -- PrismCore tests
// C123: THE TOP SEAM -- toolbar flush/full-width, breadcrumb as a path bar,
// tabs on demand.
//
// THE DEFECT (measured at 125 %, proposal 14.4 / 17.3):
//
//   | quantity                  | Bandizip | before C123                        |
//   |---------------------------|----------|------------------------------------|
//   | OPEN button centre        | (32, 83) | (643, 138) -- 611 px too far right |
//   | toolbar's top edge        | 50       | 117                                |
//   | toolbar's left edge       | flush    | 589 (a 964 wide floating island)   |
//   | breadcrumb text           | --       | centred: 28..718 vs text 234..495  |
//
// The three causes are structural, and each one is a test below:
//   T1 the 32 px tab strip exists even for a single archive (17.2: "1 个包时
//      0 像素 chrome");
//   T2/T3 the toolbar starts where the breadcrumb PILL ends, because the pill
//      shares the 56 px band with it, so "Open" -- the app's primary action --
//      is 611 px from the corner;
//   T4 the path is CENTRED in its pill, so its left edge drifts with the path's
//      length and 62.2 % of the row is dead air.
//
// WHAT IS ASSERTED HERE (the card's T1..T5, plus the invariants that keep the
// fix from being a different layout with the same name):
//   T1  one archive -> the tab band's rectangle is EMPTY (and no 32 px hole is
//       left behind: the toolbar starts at the title bar's bottom); two
//       archives -> non-empty and exactly kTabBarHeight tall.
//   T2  the OPEN button's centre is at x <= 60 and y <= 110, at every window
//       width the product supports (640..1920).
//   T3  the toolbar's left and right edges ARE the content band's -- flush and
//       full width, not a floating island.
//   T4  the crumb's text box starts at contentBand.left + 16 and its first
//       segment starts THERE (the run is left-aligned, not centred).
//   T5  the crumb bar intersects neither the toolbar nor the content band: the
//       three bands tile the window top to bottom in the documented order
//       (title -> tabs -> toolbar -> crumb -> content -> status).
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the SAME source compiled with
// PRISM_C123_MUTANT_FLOATING_TOOLBAR restores the pre-card floating island
// (pill.right + 12 .. band.right - 16). T2 and T3 MUST go red, the program
// prints "C123 MUTATION CAUGHT: floating_toolbar" at the FIRST failing
// assertion and exits 0, and the CTest entry decides with
// PASS_REGULAR_EXPRESSION -- never WILL_FAIL.
#include <cstdio>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "ActionBarModel.hpp"
#include "ArchiveTreeView.hpp"
#include "WindowLayout.hpp"

using prism::layout::Rect;

namespace {

constexpr int kHeight = 800;   // a comfortable client height (DIPs)
constexpr int kWidth = 1280;

// The compositions the product ships, one per state the tests care about.
prism::layout::Layout Plan(int w, int openArchives, bool breadcrumb = true) {
  return prism::layout::ComputeC123(w, kHeight, /*sidebarVisible=*/true,
                                    /*listMode=*/true, openArchives, breadcrumb);
}

// The centre of the OPEN button, from the SAME layout the window paints and
// hit-tests (actionbar::Compute reads Layout::chrome and the band rectangle --
// never a copy of the numbers).
struct OpenCentre {
  int x = -1;
  int y = -1;
  bool found = false;
};

OpenCentre OpenButtonCentre(const prism::layout::Layout& plan) {
  const prism::actionbar::Layout band = prism::actionbar::Compute(plan);
  OpenCentre centre;
  for (const prism::actionbar::Button& button : band.buttons) {
    if (button.id != prism::actionbar::ActionId::Open) continue;
    centre.x = (button.rect.left + button.rect.right) / 2;
    centre.y = (button.rect.top + button.rect.bottom) / 2;
    centre.found = true;
    break;
  }
  return centre;
}

// Only the mutant build declares the marker: a clean build that kept it would
// trip -Wunused-variable on the GCC/Clang lane (CONTRACTS D31).
#if defined(PRISM_C123_MUTANT_FLOATING_TOOLBAR)
const char* const kMutantMarker = "C123 MUTATION CAUGHT: floating_toolbar";
const char* const kMutantBanner = "C123 MUTANT ACTIVE: floating_toolbar";

// The runtime proof that the injected defect really fired (D3): the toolbar is
// NOT flush with the content band. Evaluated at the first failure, from the
// same composition the failing assertion just measured.
bool FloatingToolbarObserved() {
  const prism::layout::Layout plan = Plan(kWidth, 1);
  const prism::actionbar::Layout band = prism::actionbar::Compute(plan);
  return band.band.left != plan.contentBand.left ||
         band.band.right != plan.contentBand.right;
}
#endif

}  // namespace

// ---------------------------------------------------------------------- T1
PRISM_TEST("c123.t1: the tab strip exists only from the second archive on") {
  // Zero archives: nothing to show -- the band is hidden, not empty.
  const prism::layout::Layout empty = Plan(kWidth, 0);
  PRISM_CHECK(empty.tabBar.Empty());
  PRISM_CHECK(!empty.tabBarVisible);

  // ONE archive (the migration user's first state): 0 px of chrome. The
  // assertion is not only "the rect is empty" but "nothing was reserved for
  // it": the toolbar starts at the title bar's bottom, exactly as it would if
  // the strip did not exist.
  const prism::layout::Layout one = Plan(kWidth, 1);
  PRISM_CHECK(one.tabBar.Empty());
  PRISM_CHECK(!one.tabBarVisible);
  PRISM_CHECK_EQ(one.tabBar.Height(), 0);
  PRISM_CHECK_EQ(one.toolbar.top, prism::layout::kTitleBarHeight);
  // ... and the whole row it used to occupy is GONE from the stack: title +
  // toolbar + crumb is all that sits above the content.
  PRISM_CHECK_EQ(one.content.top, prism::layout::kTitleBarHeight +
                                      prism::layout::kActionIslandHeight +
                                      prism::layout::kBreadcrumbBarHeight);

  // TWO archives: the strip is back, 32 px, between the title and the toolbar.
  const prism::layout::Layout two = Plan(kWidth, 2);
  PRISM_CHECK(!two.tabBar.Empty());
  PRISM_CHECK(two.tabBarVisible);
  PRISM_CHECK_EQ(two.tabBar.Height(), prism::layout::kTabBarHeight);
  PRISM_CHECK_EQ(two.tabBar.Height(), 32);
  PRISM_CHECK_EQ(two.tabBar.top, prism::layout::kTitleBarHeight);
  PRISM_CHECK_EQ(two.toolbar.top, two.tabBar.bottom);

  std::printf("C123 T1: 1 archive tab=(%d..%d h=%d) toolbar.top=%d; "
              "2 archives tab h=%d toolbar.top=%d\n",
              one.tabBar.left, one.tabBar.right, one.tabBar.Height(), one.toolbar.top,
              two.tabBar.Height(), two.toolbar.top);
}

// ---------------------------------------------------------------------- T2
PRISM_TEST("c123.t2: the OPEN button is at the corner, x <= 60 and y <= 110") {
  // The reference point of the whole card: Bandizip's (32, 83). The bounds are
  // the proposal's own red line ("x <= 60 && y <= 110"), not a tolerance the
  // implementation chose for itself.
  const int widths[] = {640, 800, 1024, 1280, 1536, 1920};
  int worstX = -1;
  int worstY = -1;
  for (const int w : widths) {
    for (const int archives : {1, 2}) {
      const prism::layout::Layout plan = Plan(w, archives);
      const OpenCentre centre = OpenButtonCentre(plan);
      std::printf("C123 T2: w=%d archives=%d open centre=(%d,%d)\n", w, archives,
                  centre.x, centre.y);
      PRISM_CHECK(centre.found);
      PRISM_CHECK(centre.x >= 0 && centre.x <= 60);
      PRISM_CHECK(centre.y >= 0 && centre.y <= 110);
      worstX = (std::max)(worstX, centre.x);
      worstY = (std::max)(worstY, centre.y);
    }
  }
  // Not vacuous: the button really is near the corner in the WORST case too.
  std::printf("C123 T2: worst centre=(%d,%d) over %d widths x 2 states\n", worstX, worstY,
              static_cast<int>(sizeof(widths) / sizeof(widths[0])));
  PRISM_CHECK(worstX <= 60);
  PRISM_CHECK(worstY <= 110);
}

// ---------------------------------------------------------------------- T3
PRISM_TEST("c123.t3: the toolbar is flush -- its edges ARE the content band's") {
  // 640 is the documented minimum window; 1920 covers a maximised 1080p. Step 4
  // because the 4 px grid is the design's own scale and a rounding bug shows up
  // as a 1-3 px gap at SOME width, not only at the one the card was shot at.
  int mismatches = 0;
  int widestGap = 0;
  int samples = 0;
  for (int w = 640; w <= 1920; w += 4) {
    for (const int archives : {1, 2}) {
      const prism::layout::Layout plan = Plan(w, archives);
      const prism::actionbar::Layout band = prism::actionbar::Compute(plan);
      ++samples;
      if (band.band.left != plan.contentBand.left) {
        ++mismatches;
        widestGap = (std::max)(widestGap, band.band.left - plan.contentBand.left);
      }
      if (band.band.right != plan.contentBand.right) {
        ++mismatches;
        widestGap = (std::max)(widestGap, plan.contentBand.right - band.band.right);
      }
      // The eight buttons are INSIDE the band and the island is the band itself
      // (a floating squircle would be a second rectangle called "toolbar").
      PRISM_CHECK(static_cast<int>(band.buttons.size()) == prism::actionbar::kActionCount);
      PRISM_CHECK_EQ(band.dropped, 0);
      PRISM_CHECK(prism::actionbar::BandIsFlush(band));
      for (const prism::actionbar::Button& button : band.buttons) {
        PRISM_CHECK(plan.toolbar.Contains(button.rect));
      }
    }
  }
  std::printf("C123 T3: %d compositions, %d edge mismatches, widest gap %d px\n", samples,
              mismatches, widestGap);
  PRISM_CHECK_EQ(mismatches, 0);
  PRISM_CHECK_EQ(samples, ((1920 - 640) / 4 + 1) * 2);
}

// ---------------------------------------------------------------------- T4
PRISM_TEST("c123.t4: the crumb is a path, left-aligned at contentBand.left + 16") {
  const int widths[] = {640, 900, 1024, 1280, 1536, 1920};
  for (const int w : widths) {
    const prism::layout::Layout plan = Plan(w, 1);
    const Rect text = prism::layout::CrumbTextRect(plan);
    std::printf("C123 T4: w=%d crumbBar=(%d..%d) text=(%d..%d)\n", w, plan.crumbBar.left,
                plan.crumbBar.right, text.left, text.right);
    PRISM_CHECK_EQ(text.left, plan.contentBand.left + prism::layout::kBreadcrumbInset);
    PRISM_CHECK_EQ(text.left, plan.contentBand.left + 16);
    PRISM_CHECK_EQ(text.right, plan.contentBand.right - 16);
    PRISM_CHECK_EQ(plan.crumbBar.Height(), prism::layout::kBreadcrumbBarHeight);
    PRISM_CHECK_EQ(plan.crumbBar.Height(), 28);
    // The text box lives inside the bar and the bar spans the content band.
    PRISM_CHECK(plan.crumbBar.Contains(text));
    PRISM_CHECK_EQ(plan.crumbBar.left, plan.contentBand.left);
    PRISM_CHECK_EQ(plan.crumbBar.right, plan.contentBand.right);
  }

  // And the run really starts at that left edge: the first segment's span
  // begins exactly at the text box. A CENTRED run (the pre-card layout) would
  // start at text.left + (box - run) / 2 and move with the path's length.
  const std::vector<prism::tree::Segment> segments = [] {
    std::vector<prism::tree::Segment> out;
    out.push_back(prism::tree::Segment{"fixture.zip", prism::tree::kRootLevel});
    for (const char* name : {"photos", "2026"}) {
      out.push_back(prism::tree::Segment{std::string(prism::tree::kBreadcrumbSeparator),
                                         prism::tree::kNoLevel});
      out.push_back(prism::tree::Segment{name, static_cast<int>(out.size())});
    }
    return out;
  }();
  const auto measure = [](std::string_view text) { return static_cast<int>(text.size()) * 7; };
  for (const int w : widths) {
    const prism::layout::Layout plan = Plan(w, 1);
    const Rect text = prism::layout::CrumbTextRect(plan);
    const prism::tree::BreadcrumbLayout crumbs =
        prism::tree::LayoutLeftBreadcrumb(segments, text.left, text.right, measure);
    PRISM_CHECK(!crumbs.spans.empty());
    PRISM_CHECK_EQ(crumbs.spans.front().left, text.left);
    for (const prism::tree::Span& span : crumbs.spans) {
      PRISM_CHECK(span.left >= text.left);
      PRISM_CHECK(span.right <= text.right);
    }
    // The current (last) segment gets the 2 px rule on the bar's bottom edge.
    const prism::tree::Span current = crumbs.spans.back();
    const Rect rule = prism::layout::CrumbUnderlineRect(plan, current.left, current.right);
    PRISM_CHECK_EQ(rule.Height(), prism::layout::kBreadcrumbUnderlineHeight);
    PRISM_CHECK_EQ(rule.Height(), 2);
    PRISM_CHECK_EQ(rule.bottom, plan.crumbBar.bottom);
    PRISM_CHECK_EQ(rule.left, current.left);
    PRISM_CHECK_EQ(rule.right, current.right);
  }
}

// ---------------------------------------------------------------------- T5
PRISM_TEST("c123.t5: the crumb touches neither the toolbar nor the content") {
  int overlaps = 0;
  for (int w = 640; w <= 1920; w += 4) {
    for (const int archives : {1, 2}) {
      const prism::layout::Layout plan = Plan(w, archives);
      const prism::actionbar::Layout band = prism::actionbar::Compute(plan);
      if (plan.crumbBar.Intersects(band.band)) ++overlaps;
      if (plan.crumbBar.Intersects(plan.contentBand)) ++overlaps;
      if (plan.crumbBar.Intersects(plan.toolbar)) ++overlaps;
      // The bands TILE: touching edges, in the documented order.
      PRISM_CHECK_EQ(plan.titleBar.top, 0);
      PRISM_CHECK_EQ(plan.toolbar.top, plan.titleBar.bottom +
                                           (plan.tabBar.Empty() ? 0
                                                                : prism::layout::kTabBarHeight));
      PRISM_CHECK_EQ(plan.crumbBar.top, plan.toolbar.bottom);
      PRISM_CHECK_EQ(plan.contentBand.top, plan.crumbBar.bottom);
      PRISM_CHECK_EQ(plan.content.top, plan.crumbBar.bottom);
      PRISM_CHECK_EQ(plan.statusBar.top, plan.contentBand.bottom);
      PRISM_CHECK(plan.contentBand.Contains(plan.contentInner));
      // The rail is a CHILD of the content band under C123 (the band is the
      // window's content area; the rail no longer shortens the toolbar).
      PRISM_CHECK(plan.contentBand.Contains(plan.sidebar));
      PRISM_CHECK_EQ(plan.sidebar.left, 0);
      PRISM_CHECK_EQ(plan.sidebar.Width(), prism::layout::kSidebarWidth);
    }
  }
  std::printf("C123 T5: overlaps=%d over the 640..1920 sweep\n", overlaps);
  PRISM_CHECK_EQ(overlaps, 0);

  // No path (no archive open): the row collapses and the content starts at the
  // toolbar's bottom. A reserved 28 px of nothing is the defect T1 forbids for
  // the tab strip, applied one band lower.
  const prism::layout::Layout blank = Plan(kWidth, 0, /*breadcrumb=*/false);
  PRISM_CHECK(blank.crumbBar.Empty());
  PRISM_CHECK_EQ(blank.contentBand.top, blank.toolbar.bottom);
  PRISM_CHECK(prism::layout::CrumbTextRect(blank).Empty());
  PRISM_CHECK(prism::layout::CrumbUnderlineRect(blank, 0, 100).Empty());
}

int main(int argc, char** argv) {
#if defined(PRISM_C123_MUTANT_FLOATING_TOOLBAR)
  std::printf("%s\n", kMutantBanner);
  prism::test::SetMutationVerdict(kMutantMarker, [] { return FloatingToolbarObserved(); });
#else
  std::puts("C123 MUTANT: none (clean build)");
#endif
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C123 LAYOUT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C123 LAYOUT CHECKS PASSED");
  return 0;
}
