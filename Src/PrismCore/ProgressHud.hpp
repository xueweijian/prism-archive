// PrismArchive -- PrismCore
// Three-phase fluid progress HUD (blueprint ch. 3.4): a pure, clock-injected
// state machine. The Win11 capsule, the Win10 taskbar progress and the in-app
// HUD all render from this one model.
//
//   Hidden   : < 400 ms   -- fast ops must not flash a bar
//   Compact  : 400ms..5s  -- percent capsule
//   Expanded : > 5 s      -- headline + ETA
//   Done     : success shown for 1 s, then Hidden
#pragma once

#include <cstdint>
#include <ostream>
#include <string>

namespace prism {

enum class HudPhase { Hidden, Compact, Expanded, Done };

// How an operation ENDED (card C87). Success/Failed/Cancelled all dismiss the
// HUD at once: the capsule is a live indicator, not a notification centre, and
// a capsule left on screen after the work finished is the "frozen at N%" defect
// the card closes. The 1 s Done phase stays available for surfaces that WANT a
// completion beat (OnDone); the window's operation lifecycle uses OnFinish.
enum class HudOutcome { Success, Failed, Cancelled };

inline std::ostream& operator<<(std::ostream& os, HudPhase phase) {
  switch (phase) {
    case HudPhase::Hidden: return os << "Hidden";
    case HudPhase::Compact: return os << "Compact";
    case HudPhase::Expanded: return os << "Expanded";
    case HudPhase::Done: return os << "Done";
  }
  return os << "?";
}

class ProgressHud {
 public:
  // doneBytes/totalBytes are cumulative uncompressed byte counts; nowMs is the
  // injected clock (any monotonic millisecond timebase).
  void OnStart(std::string headline, uint64_t nowMs) {
    started_ = true;
    headline_ = std::move(headline);
    startMs_ = nowMs;
    lastSampleMs_ = nowMs;
    doneMs_ = 0;
    doneBytes_ = 0;
    totalBytes_ = 0;
    lastPercent_ = 0;
    finished_ = false;
    dismissed_ = false;
    outcome_ = HudOutcome::Success;
  }

  // The operation ENDED (success, failure or cancel). The HUD is gone on the
  // NEXT Render -- no frozen percent, no lingering Done capsule. OnDone below
  // keeps the 1 s completion beat for callers that ask for it explicitly.
  void OnFinish(uint64_t nowMs, HudOutcome outcome) {
    started_ = true;
    finished_ = true;
    dismissed_ = true;
    outcome_ = outcome;
    doneMs_ = nowMs;
    if (outcome == HudOutcome::Success) doneBytes_ = totalBytes_;
  }

  // Forget everything: a new archive/tab must not inherit the last operation's
  // frozen percent (C87 requirement 4).
  void Reset() {
    headline_.clear();
    startMs_ = 0;
    lastSampleMs_ = 0;
    doneMs_ = 0;
    doneBytes_ = 0;
    totalBytes_ = 0;
    lastPercent_ = 0;
    started_ = false;
    finished_ = false;
    dismissed_ = false;
    outcome_ = HudOutcome::Success;
  }

  bool started() const { return started_; }
  bool finished() const { return finished_; }
  bool dismissed() const { return dismissed_; }
  HudOutcome outcome() const { return outcome_; }

  void OnProgress(uint64_t nowMs, uint64_t doneBytes, uint64_t totalBytes) {
    if (finished_) return;
    // Clamp monotonicity: later samples may cover a smaller window.
    if (totalBytes > 0 && doneBytes > totalBytes) doneBytes = totalBytes;
    if (doneBytes < doneBytes_) return;  // ignore regressions
    doneBytes_ = doneBytes;
    totalBytes_ = totalBytes;
    lastSampleMs_ = nowMs;
  }

  void OnDone(uint64_t nowMs) {
    finished_ = true;
    doneBytes_ = totalBytes_;  // success means everything landed
    doneMs_ = nowMs;
  }

  struct View {
    HudPhase phase = HudPhase::Hidden;
    int percent = 0;       // -1 == indeterminate
    std::string headline;
    double etaSeconds = 0.0;
  };

  View Render(uint64_t nowMs) const {
    if (finished_) {
      // OnFinish: dismissed at once. OnDone: the legacy 1 s beat.
      if (dismissed_) return {HudPhase::Hidden, 0, "", 0.0};
      if (nowMs - doneMs_ <= kDoneShowMs) return {HudPhase::Done, 100, headline_, 0.0};
      return {HudPhase::Hidden, 0, "", 0.0};
    }
    if (!started_) return {HudPhase::Hidden, 0, "", 0.0};

    View view;
    view.headline = headline_;
    const uint64_t elapsed = nowMs > startMs_ ? nowMs - startMs_ : 0;

    if (totalBytes_ == 0) {
      view.percent = -1;  // indeterminate
    } else {
      const double frac = static_cast<double>(doneBytes_) / static_cast<double>(totalBytes_);
      int percent = static_cast<int>(frac * 100.0 + 0.5);
      if (percent > 100) percent = 100;
      if (percent < lastPercent_) percent = lastPercent_;  // never backwards
      lastPercent_ = percent;
      view.percent = percent;
      // ETA from the work rate actually observed up to the latest sample
      // (not wall-clock now: a stalled sample must not inflate the ETA).
      if (doneBytes_ > 0 && frac > 0.0 && frac < 1.0) {
        const double observedSec =
            lastSampleMs_ > startMs_ ? (lastSampleMs_ - startMs_) / 1000.0 : 0.0;
        view.etaSeconds = observedSec * (1.0 - frac) / frac;
      }
    }

    if (elapsed < kGraceMs) return {HudPhase::Hidden, view.percent, "", 0.0};
    view.phase = elapsed >= kExpandMs ? HudPhase::Expanded : HudPhase::Compact;
    return view;
  }

  static constexpr uint64_t kGraceMs = 400;
  static constexpr uint64_t kExpandMs = 5'000;
  static constexpr uint64_t kDoneShowMs = 1'000;

 private:
  std::string headline_;
  uint64_t startMs_ = 0;
  uint64_t lastSampleMs_ = 0;
  uint64_t doneMs_ = 0;
  uint64_t doneBytes_ = 0;
  uint64_t totalBytes_ = 0;
  mutable int lastPercent_ = 0;
  bool started_ = false;
  bool finished_ = false;
  bool dismissed_ = false;  // OnFinish: no Done beat, Hidden immediately
  HudOutcome outcome_ = HudOutcome::Success;
};

}  // namespace prism
