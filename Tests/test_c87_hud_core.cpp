// PrismArchive -- PrismCore
// C87: the progress HUD's terminal states and the accessibility role that makes
// it audible. The Windows suite proves the window publishes/paints it; this one
// proves the STATE MACHINE, on any machine, with an injected clock.
//
// What is asserted (card C87 requirements 1, 2 and 4):
//   1. OnFinish(Success/Failed/Cancelled) dismisses the capsule on the NEXT
//      Render -- success is NOT allowed to leave a frozen "100%" behind;
//   2. OnStart resets every timeline/percent field, so a second operation starts
//      from 0 (never from the previous run's high-water mark);
//   3. Reset() forgets an operation entirely (the cross-tab/cross-archive case);
//   4. Role::ProgressBar is a first-class a11y role with a stable spelling.
//
// The 1 s "Done" beat stays available through OnDone for surfaces that ask for
// it; the operation lifecycle uses OnFinish, which is the card's contract.
#include "A11yTree.hpp"
#include "ProgressHud.hpp"
#include "mini_test.hpp"

#include <string>

namespace {

using prism::HudOutcome;
using prism::HudPhase;

PRISM_TEST("c87.finish_dismisses_immediately_on_success") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting pack", 0);
  hud.OnProgress(1'000, 400, 1'000);
  PRISM_CHECK_EQ(hud.Render(2'000).phase, HudPhase::Compact);
  hud.OnFinish(2'000, HudOutcome::Success);
  // The NEXT frame has no capsule at all -- not a 1 s "Done" beat.
  PRISM_CHECK_EQ(hud.Render(2'000).phase, HudPhase::Hidden);
  PRISM_CHECK_EQ(hud.Render(2'001).phase, HudPhase::Hidden);
  PRISM_CHECK(hud.finished());
  PRISM_CHECK(hud.dismissed());
}

PRISM_TEST("c87.finish_dismisses_on_failure_and_cancel") {
  for (const HudOutcome outcome :
       {HudOutcome::Failed, HudOutcome::Cancelled}) {
    prism::ProgressHud hud;
    hud.OnStart("Compressing pack", 0);
    hud.OnProgress(1'000, 670, 1'000);
    hud.OnFinish(2'000, outcome);
    PRISM_CHECK_EQ(hud.Render(2'000).phase, HudPhase::Hidden);
    PRISM_CHECK_EQ(hud.Render(60'000).phase, HudPhase::Hidden);
    PRISM_CHECK(hud.dismissed());
    PRISM_CHECK_EQ(static_cast<int>(hud.outcome()), static_cast<int>(outcome));
  }
}

PRISM_TEST("c87.a_new_operation_starts_from_zero") {
  prism::ProgressHud hud;
  hud.OnStart("first", 0);
  hud.OnProgress(1'000, 900, 1'000);   // high-water mark 90 %
  hud.OnFinish(2'000, HudOutcome::Failed);
  PRISM_CHECK_EQ(hud.Render(2'000).phase, HudPhase::Hidden);

  hud.OnStart("second", 3'000);
  hud.OnProgress(3'100, 5, 100);       // 5 % of the NEW job
  const auto view = hud.Render(4'000);  // past the 400 ms grace
  PRISM_CHECK_EQ(view.phase, HudPhase::Compact);
  // The old 90 % must not leak: the percent is the new sample, not the maximum
  // of both runs.
  PRISM_CHECK_EQ(view.percent, 5);
  PRISM_CHECK(view.percent < 10);
  PRISM_CHECK_EQ(view.headline, std::string("second"));
}

PRISM_TEST("c87.reset_forgets_the_operation") {
  prism::ProgressHud hud;
  hud.OnStart("stale", 0);
  hud.OnProgress(1'000, 890, 1'000);
  hud.Reset();
  PRISM_CHECK(!hud.started());
  PRISM_CHECK(!hud.finished());
  PRISM_CHECK_EQ(hud.Render(2'000).phase, HudPhase::Hidden);
  // ...and a fresh start is a fresh timeline (Hidden through the grace window).
  hud.OnStart("fresh", 10'000);
  PRISM_CHECK_EQ(hud.Render(10'100).phase, HudPhase::Hidden);
  hud.OnProgress(10'500, 1, 100);
  PRISM_CHECK_EQ(hud.Render(11'000).percent, 1);
}

PRISM_TEST("c87.progress_role_is_stable") {
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(prism::a11y::Role::ProgressBar)),
                 std::string("progressbar"));
}

}  // namespace

PRISM_TEST_MAIN()
