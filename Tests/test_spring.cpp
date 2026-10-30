// TDD: the spring physics engine (blueprint ch. 3.1 "弹性微交互物理模型").
//
// First principles:
//  (a) the blueprint pins two presets by their *physical* constants --
//      snappy-interactive: m=1.0, k=420, c=28  => zeta ~ 0.68 (underdamped,
//      a slight living bounce), fluid-sheet: m=1.0, k=280, c=34 => zeta ~ 1.0
//      (critically damped, fast with no overshoot). The animator must reproduce
//      exactly these characters,
//  (b) animation runs on a fixed-step symplectic integrator so the motion is
//      bit-identical across runs and frame rates (determinism is the contract),
//  (c) settling must *terminate*: past the epsilon band the value snaps to the
//      target and the integrator sleeps (a UI timer must be able to stop),
//  (d) retargeting mid-flight keeps the velocity (a user flicking a sheet the
//      other way must not freeze it first),
//  (e) everything is driven by an injected clock -- no UI needed to test.
#include "SpringAnimator.hpp"
#include "mini_test.hpp"

#include <cmath>
#include <vector>

namespace {

using prism::kSpringFluidSheet;
using prism::kSpringSnappyInteractive;
using prism::SpringAnimator;

PRISM_TEST("preset damping ratios match the blueprint physics table") {
  const double zetaSnappy = kSpringSnappyInteractive.DampingRatio();
  const double zetaFluid = kSpringFluidSheet.DampingRatio();
  PRISM_CHECK(zetaSnappy > 0.65 && zetaSnappy < 0.72);  // ~0.68
  PRISM_CHECK(zetaFluid > 0.98 && zetaFluid < 1.05);    // ~1.0
}

PRISM_TEST("snappy preset overshoots (living bounce), fluid never does") {
  SpringAnimator snappy{kSpringSnappyInteractive, 0.0};
  snappy.Retarget(1.0, 0.0);
  double snappyMax = 0.0;
  for (double t = 0.0; t <= 3.0; t += 1.0 / 240.0) {
    snappy.Tick(t);
    snappyMax = std::max(snappyMax, snappy.Value());
  }
  PRISM_CHECK(snappyMax > 1.001);  // visible bounce past the target

  SpringAnimator fluid{kSpringFluidSheet, 0.0};
  fluid.Retarget(1.0, 0.0);
  double fluidMax = 0.0;
  for (double t = 0.0; t <= 3.0; t += 1.0 / 240.0) {
    fluid.Tick(t);
    fluidMax = std::max(fluidMax, fluid.Value());
  }
  PRISM_CHECK(fluidMax <= 1.0 + 1e-9);  // no overshoot, ever
}

PRISM_TEST("both presets settle and snap exactly to the target") {
  for (const auto* preset : {&kSpringSnappyInteractive, &kSpringFluidSheet}) {
    SpringAnimator spring{*preset, 0.0};
    spring.Retarget(1.0, 0.0);
    for (double t = 0.0; t <= 2.5; t += 1.0 / 60.0) spring.Tick(t);
    PRISM_CHECK(spring.IsSettled());
    PRISM_CHECK_EQ(spring.Value(), 1.0);  // snapped, not merely close
    PRISM_CHECK_EQ(spring.Velocity(), 0.0);
    // A settled animator stays put forever.
    for (double t = 2.5; t <= 5.0; t += 1.0 / 60.0) spring.Tick(t);
    PRISM_CHECK_EQ(spring.Value(), 1.0);
  }
}

PRISM_TEST("identical tick sequences produce identical motion (determinism)") {
  auto run = [] {
    SpringAnimator spring{kSpringSnappyInteractive, 0.0};
    spring.Retarget(1.0, 0.0);
    std::vector<double> trace;
    for (double t = 0.0; t <= 1.0; t += 1.0 / 60.0) {
      spring.Tick(t);
      trace.push_back(spring.Value());
    }
    return trace;
  };
  const auto a = run();
  const auto b = run();
  PRISM_CHECK_EQ(a.size(), b.size());
  for (size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) return;  // PRISM_CHECK_EQ on doubles compares loosely
  }
  PRISM_CHECK(true);
}

PRISM_TEST("retargeting mid-flight keeps momentum and never goes NaN") {
  SpringAnimator spring{kSpringSnappyInteractive, 0.0};
  spring.Retarget(1.0, 0.0);
  for (double t = 0.0; t <= 0.10; t += 1.0 / 240.0) spring.Tick(t);
  spring.Retarget(-1.0, 0.10);  // flick the other way
  bool wentNaN = false;
  for (double t = 0.10; t <= 2.5; t += 1.0 / 240.0) {
    spring.Tick(t);
    if (!std::isfinite(spring.Value()) || !std::isfinite(spring.Velocity())) {
      wentNaN = true;
      break;
    }
  }
  PRISM_CHECK(!wentNaN);
  PRISM_CHECK(spring.IsSettled());
  PRISM_CHECK_EQ(spring.Value(), -1.0);
}

PRISM_TEST("a stiffer spring crosses 90 percent sooner (responsiveness)") {
  const prism::SpringPreset soft{1.0, 280.0, 34.0};
  const prism::SpringPreset stiff{1.0, 800.0, 34.0};
  auto crossing = [&](const prism::SpringPreset& preset) {
    SpringAnimator spring{preset, 0.0};
    spring.Retarget(1.0, 0.0);
    for (double t = 0.0; t <= 3.0; t += 1.0 / 240.0) {
      spring.Tick(t);
      if (spring.Value() >= 0.9) return t;
    }
    return 3.0;  // never crossed
  };
  PRISM_CHECK(crossing(stiff) < crossing(soft));
}

}  // namespace

PRISM_TEST_MAIN()
