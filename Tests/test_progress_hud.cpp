// TDD: the three-phase fluid progress HUD (blueprint ch. 3.4).
//
// First principles:
//  (a) a progress bar that flashes for 200 ms is noise -- stay Hidden until the
//      operation proves it will take real time (400 ms),
//  (b) under 5 s a compact capsule is enough; past that the user needs the
//      headline (file name) and ETA -- expand,
//  (c) percent must never move backwards (7z reports per-item totals that jitter),
//  (d) done is a state, not an event: show success for 1 s, then dismiss,
//  (e) all of it is a pure function of (start time, now, samples) -- so the
//      clock is injected and the whole thing tests without a UI.
#include "ProgressHud.hpp"
#include "mini_test.hpp"

#include <string>

namespace {

using prism::HudPhase;

PRISM_TEST("stays hidden for the first 400 ms") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting UI_Kit/Components.fig", /*nowMs=*/0);
  PRISM_CHECK_EQ(hud.Render(100).phase, HudPhase::Hidden);
  PRISM_CHECK_EQ(hud.Render(399).phase, HudPhase::Hidden);
}

PRISM_TEST("compact capsule between 400 ms and 5 s") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting pack", 0);
  hud.OnProgress(1'000, 250, 1'000);  // 25 % after one second of work
  const auto view = hud.Render(1'500);
  PRISM_CHECK_EQ(view.phase, HudPhase::Compact);
  PRISM_CHECK_EQ(view.percent, 25);
  PRISM_CHECK_EQ(view.headline, std::string("Extracting pack"));
}

PRISM_TEST("expands past 5 s with ETA") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting pack", 0);
  hud.OnProgress(1'000, 1'000, 4'000);  // 25% after 1 s of work
  const auto view = hud.Render(5'200);
  PRISM_CHECK_EQ(view.phase, HudPhase::Expanded);
  // 25% took ~1.0 s of elapsed work -> remaining 75% ~= 3 s (hysteresis-free math)
  PRISM_CHECK(view.etaSeconds > 2.0 && view.etaSeconds < 4.0);
}

PRISM_TEST("percent never moves backwards") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting pack", 0);
  hud.OnProgress(100, 800, 1000);
  hud.OnProgress(100, 400, 1000);  // a lying later sample
  PRISM_CHECK_EQ(hud.Render(2000).percent, 80);
}

PRISM_TEST("unknown total renders indeterminate, not 0%") {
  prism::ProgressHud hud;
  hud.OnStart("Scanning archive", 0);
  hud.OnProgress(0, 0, 0);
  const auto view = hud.Render(2000);
  PRISM_CHECK_EQ(view.phase, HudPhase::Compact);
  PRISM_CHECK_EQ(view.percent, -1);
}

PRISM_TEST("done shows success for one second, then dismisses itself") {
  prism::ProgressHud hud;
  hud.OnStart("Extracting pack", 0);
  hud.OnProgress(100, 500, 1000);
  hud.OnDone(6000);
  const auto done = hud.Render(6500);
  PRISM_CHECK_EQ(done.phase, HudPhase::Done);
  PRISM_CHECK_EQ(done.percent, 100);
  PRISM_CHECK_EQ(hud.Render(7100).phase, HudPhase::Hidden);
}

PRISM_TEST("a new operation resets the timeline") {
  prism::ProgressHud hud;
  hud.OnStart("first", 0);
  hud.OnDone(10'000);
  hud.OnStart("second", 10'500);
  PRISM_CHECK_EQ(hud.Render(10'600).phase, HudPhase::Hidden);  // fresh 400 ms grace
  hud.OnProgress(11'000, 100, 100);  // completes right away
  const auto view = hud.Render(11'100);
  PRISM_CHECK_EQ(view.phase, HudPhase::Compact);
  PRISM_CHECK_EQ(view.percent, 100);
}

}  // namespace

PRISM_TEST_MAIN()
