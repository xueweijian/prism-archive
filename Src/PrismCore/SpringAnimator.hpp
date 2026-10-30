// PrismArchive -- PrismCore
// Spring physics for UI micro-interactions (blueprint ch. 3.1).
//
// First principles: a UI spring is a damped harmonic oscillator,
//     m * x'' = -k * (x - target) - c * x'.
// The blueprint pins two presets *by their physical constants*, so the
// character of the motion (bouncy vs. dead-critical) is a spec, not a taste:
//   - snappy-interactive: m=1, k=420, c=28  => zeta ~ 0.68 (living bounce)
//   - fluid-sheet:        m=1, k=280, c=34  => zeta ~ 1.0 (no overshoot)
// Integration is semi-implicit Euler at a FIXED substep with a carry
// remainder, which makes the trajectory a pure function of the tick sequence
// -- identical inputs give bit-identical motion at any frame rate.
#pragma once

#include <algorithm>
#include <cmath>

namespace prism {

struct SpringPreset {
  double mass = 1.0;
  double stiffness = 1.0;
  double damping = 1.0;

  // zeta = c / (2 * sqrt(k * m)); <1 underdamped (bounces), >=1 overdamped.
  double DampingRatio() const { return damping / (2.0 * std::sqrt(stiffness * mass)); }
};

inline constexpr SpringPreset kSpringSnappyInteractive{1.0, 420.0, 28.0};
inline constexpr SpringPreset kSpringFluidSheet{1.0, 280.0, 34.0};

class SpringAnimator {
 public:
  static constexpr double kSubstepSeconds = 1.0 / 240.0;
  static constexpr double kPositionEpsilon = 1e-3;
  static constexpr double kVelocityEpsilon = 1e-2;

  explicit SpringAnimator(SpringPreset preset, double value = 0.0)
      : preset_(preset), value_(value), target_(value) {}

  // New goal, preserving current velocity (a mid-flight retarget must not
  // freeze the motion first).
  void Retarget(double target, double nowSeconds) {
    target_ = target;
    settled_ = false;
    lastNow_ = nowSeconds;
    carry_ = 0.0;
  }

  void Tick(double nowSeconds) {
    if (nowSeconds <= lastNow_ || settled_) {
      lastNow_ = (std::max)(lastNow_, nowSeconds);
      return;
    }
    const double total = (nowSeconds - lastNow_) + carry_;
    const int steps = static_cast<int>(total / kSubstepSeconds);
    carry_ = total - steps * kSubstepSeconds;
    for (int i = 0; i < steps; ++i) {
      Substep();
      if (settled_) break;
    }
    lastNow_ = nowSeconds;
  }

  double Value() const { return value_; }
  double Velocity() const { return velocity_; }
  double Target() const { return target_; }
  bool IsSettled() const { return settled_; }

 private:
  void Substep() {
    const double acceleration =
        (-preset_.stiffness * (value_ - target_) - preset_.damping * velocity_) /
        preset_.mass;
    velocity_ += acceleration * kSubstepSeconds;  // semi-implicit: v first,
    value_ += velocity_ * kSubstepSeconds;        // then x with the new v
    if (std::abs(value_ - target_) < kPositionEpsilon &&
        std::abs(velocity_) < kVelocityEpsilon) {
      value_ = target_;  // snap: the UI timer is allowed to stop
      velocity_ = 0.0;
      settled_ = true;
    }
  }

  SpringPreset preset_;
  double value_;
  double velocity_ = 0.0;
  double target_;
  double lastNow_ = 0.0;
  double carry_ = 0.0;
  bool settled_ = false;
};

}  // namespace prism
