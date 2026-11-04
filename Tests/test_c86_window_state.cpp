// PrismArchive -- PrismCore
// C86 (S3-2 + S3-4): the SAVED WINDOW as pure integer math, and the two texts
// the window publishes about it.
//
// THE MEASURED DEFECT this file pins down:
//   * %LOCALAPPDATA%\PrismArchive\settings.json held
//         "window": { "width": 1536, "height": 480, "listMode": true, ... }
//     and nothing else: no x, no y. A user who dragged the window to (300,150)
//     got the CW_USEDEFAULT cascade position back on the next start. C66 fixed
//     WHAT is written (the visible frame, clamped) but only a SIZE ever made
//     the round trip.
//   * the breadcrumb's trailing count said "共 16 项" -- the archive total, typed
//     in once by the app at open -- while the status band said 条目 2 inside a
//     two-entry folder.
//
// Both policies live in WindowLayout.hpp (no windows.h, so this suite runs on
// every lane in milliseconds); the Win32 half is win.c86_window.
//
// NEGATIVE CONTROLS (CONTRACTS D3): the same source is built again with
//   PRISM_C86_MUTANT_NO_POSITION  -- SavedWindowFor() drops the origin (the
//       pre-fix document, byte for byte), so "the origin survived" goes red;
//   PRISM_C86_MUTANT_ARCHIVE_COUNT -- the count is the archive total again, so
//       "the count names the folder" goes red.
// The marker is printed at the FIRST red assertion and only when the predicate
// proves the injection really fired (D26/D27), so an unrelated red cannot claim
// CAUGHT.
#include <cstdint>
#include <cstdio>
#include <string>

#include "mini_test.hpp"

#include "../Src/PrismCore/WindowLayout.hpp"

namespace {

using prism::layout::BreadcrumbCountText;
using prism::layout::CryptoState;
using prism::layout::Layout;
using prism::layout::PlanWindowRestore;
using prism::layout::Rect;
using prism::layout::SavedWindow;
using prism::layout::SavedWindowFor;
using prism::layout::StatusId;
using prism::layout::StatusInfo;
using prism::layout::StatusSegment;
using prism::layout::StatusSegmentLabel;

// The user's machine, to the pixel: 1536 x 816 of usable screen (the C66 card's
// measurement, reused so the two cards talk about the same desktop).
constexpr Rect kWork{0, 0, 1536, 816};

bool Inside(const Rect& inner, const Rect& outer) { return outer.Contains(inner); }

// C86 (ASan, CI): build a band from a StatusInfo that does NOT outlive the call
// -- the exact spelling that cost a stack-use-after-scope in win.c80_a11y and
// c86.window on the ASan runner:
//     for (auto& s : StatusSegments(layout, StatusForPublish())) { ... }
// The range-init temporary dies before the first iteration, so every segment
// that keeps a VIEW into it is read out of dead stack. The segments own their
// text now; this function makes the pattern explicit so the ASan lane keeps us
// honest if a future "optimisation" turns a std::string back into a view.
std::vector<StatusSegment> SegmentsFromTemporary(const Layout& layout) {
  StatusInfo info;
  info.crypto = CryptoState::Unencrypted;
  info.ratioView = "87.5%";
  info.itemCount = 2;
  info.selectedCount = 0;
  return prism::layout::StatusSegments(layout, info);
}

#if defined(PRISM_C86_MUTANT_NO_POSITION)
constexpr const char* kMutation = "no_position";
constexpr const char* kCaught = "C86 MUTATION CAUGHT: no_position";
#elif defined(PRISM_C86_MUTANT_ARCHIVE_COUNT)
constexpr const char* kMutation = "archive_count";
constexpr const char* kCaught = "C86 MUTATION CAUGHT: archive_count";
#else
constexpr const char* kMutation = nullptr;
constexpr const char* kCaught = nullptr;
#endif

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C86 MUTANT ACTIVE: %s\n", kMutation);
    // D3: the predicate proves the injection is IN this binary and really
    // changed the answer -- a red for any other reason must stay red.
    if (std::string(kMutation) == "no_position") {
      ::prism::test::SetMutationVerdict(kCaught, [] {
        return prism::layout::ActiveWindowMutation() == "no_position" &&
               SavedWindowFor(Rect{300, 150, 1200, 750}, false).visible.left == 0;
      });
    } else {
      ::prism::test::SetMutationVerdict(kCaught, [] {
        return prism::layout::ActiveWindowMutation() == "archive_count" &&
               BreadcrumbCountText(2, 16, false) == "共 16 项";
      });
    }
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL WINDOW-STATE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. The document C86 writes: an ORIGIN and a SIZE, both of them the visible
//    frame, plus the maximized bit beside them.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 a saved window keeps its origin as well as its size") {
  // The reported case: dragged to (300,150) and closed at 1200 x 600.
  const SavedWindow saved = SavedWindowFor(Rect{300, 150, 1500, 750}, false);
  PRISM_CHECK_EQ(saved.visible.left, 300);
  PRISM_CHECK_EQ(saved.visible.top, 150);
  PRISM_CHECK_EQ(saved.visible.Width(), 1200);
  PRISM_CHECK_EQ(saved.visible.Height(), 600);
  PRISM_CHECK(saved.hasPosition);
  PRISM_CHECK(!saved.maximized);
}

PRISM_TEST("c86 a maximized window saves the NORMAL frame and the mode separately") {
  // The historical trap: the rectangle of a maximized window is the work area
  // (1552x832 on this screen before C66 counted the shadow border). If that
  // number is written as the normal frame, un-maximizing has nowhere to go and
  // the next restore is a frame no screen can hold.
  const Rect normal{300, 150, 1300, 750};  // 1000 x 600, where the user was
  const SavedWindow saved = SavedWindowFor(normal, /*maximized=*/true);
  PRISM_CHECK(saved.maximized);
  PRISM_CHECK(saved.visible == normal);
  // What a maximized window would have reported instead -- and the reason the
  // mode is a separate bit.
  const Rect maximizedRect = kWork;
  PRISM_CHECK(saved.visible != maximizedRect);
  const prism::layout::WindowRestore plan = PlanWindowRestore(saved, kWork);
  PRISM_CHECK(plan.maximized);
  PRISM_CHECK(plan.frame == normal);
  PRISM_CHECK(!plan.moved);
}

// ---------------------------------------------------------------------------
// 2. RESTORE clamps the whole rectangle: a work area that got SMALLER, or a
//    monitor that is no longer attached, must not put the window off screen.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 a saved window restored onto a smaller work area stays inside it") {
  const SavedWindow saved = SavedWindowFor(Rect{300, 150, 1100, 650}, false);  // 800 x 500
  // The same desktop after a resolution change: 1000 x 600 instead of 1536 x 816.
  const Rect smaller{0, 0, 1000, 600};
  const prism::layout::WindowRestore plan = PlanWindowRestore(saved, smaller);
  PRISM_CHECK(Inside(plan.frame, smaller));
  PRISM_CHECK_EQ(plan.frame.Width(), 800);   // the size still fits: kept
  PRISM_CHECK_EQ(plan.frame.Height(), 500);
  PRISM_CHECK_EQ(plan.frame.left, 200);      // 300..1100 was 100 px too far right
  PRISM_CHECK_EQ(plan.frame.top, 100);       // 150..650 was 50 px too low
  PRISM_CHECK(plan.moved);
}

PRISM_TEST("c86 a frame saved on an unplugged monitor lands on the screen that is left") {
  // A second monitor to the LEFT: negative coordinates are legal, so the clamp
  // must pull the left/top edges in, not only the right/bottom ones.
  const SavedWindow saved = SavedWindowFor(Rect{-2400, -300, -1400, 400}, false);
  const prism::layout::WindowRestore plan = PlanWindowRestore(saved, kWork);
  PRISM_CHECK(Inside(plan.frame, kWork));
  PRISM_CHECK(plan.frame.left >= kWork.left);
  PRISM_CHECK(plan.frame.top >= kWork.top);
  PRISM_CHECK(plan.moved);
}

PRISM_TEST("c86 a frame that still fits is restored byte for byte (a fixed point)") {
  const SavedWindow saved = SavedWindowFor(Rect{300, 150, 1500, 750}, false);
  const prism::layout::WindowRestore plan = PlanWindowRestore(saved, kWork);
  PRISM_CHECK(plan.frame == saved.visible);
  PRISM_CHECK(!plan.moved);
  // Twice is the same as once: a restore that drifts is the +16 px per close the
  // C66 suite exists to catch, one card later.
  const prism::layout::WindowRestore again = PlanWindowRestore(
      SavedWindowFor(plan.frame, false), kWork);
  PRISM_CHECK(again.frame == plan.frame);
}

PRISM_TEST("c86 a work area smaller than the minimum still wins over the minimum") {
  // A 320 px desktop must not produce a 640 px window hanging off it.
  const Rect tiny{0, 0, 320, 240};
  const prism::layout::WindowRestore plan =
      PlanWindowRestore(SavedWindowFor(Rect{100, 100, 1100, 700}, false), tiny);
  PRISM_CHECK(Inside(plan.frame, tiny));
  PRISM_CHECK_EQ(plan.frame.Width(), 320);
  PRISM_CHECK_EQ(plan.frame.Height(), 240);
}

// ---------------------------------------------------------------------------
// 3. The breadcrumb's count speaks about the FOLDER (S3-4).
// ---------------------------------------------------------------------------
PRISM_TEST("c86 the breadcrumb count names the folder and keeps the archive total") {
  // Inside a two-entry folder of a sixteen-entry archive: the number the status
  // band shows (条目 2) comes first, the total stays in the parentheses.
  PRISM_CHECK_EQ(BreadcrumbCountText(2, 16, /*atRoot=*/false),
                 std::string("本目录 2 项（共 16 项）"));
  // At the root there is no "current folder" to distinguish from the archive.
  PRISM_CHECK_EQ(BreadcrumbCountText(16, 16, /*atRoot=*/true), std::string("共 16 项"));
  // ...and a root that holds fewer rows than the archive (nested entries) still
  // names the rows the user is looking at first.
  PRISM_CHECK_EQ(BreadcrumbCountText(13, 16, /*atRoot=*/true),
                 std::string("共 13 项（全档 16 项）"));
  PRISM_CHECK_EQ(BreadcrumbCountText(0, 0, true), std::string("共 0 项"));
  PRISM_CHECK_EQ(BreadcrumbCountText(-1, 16, false), std::string());
}

// ---------------------------------------------------------------------------
// 4. Every status segment has exactly ONE readable label (S3-11), and the band
//    publishes them in the order it paints them.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 the status band's segments read in the band's order") {
  const Layout layout = prism::layout::Compute(1600, 800, /*sidebarVisible=*/false,
                                               /*listMode=*/true);
  StatusInfo info;
  info.crypto = CryptoState::Unencrypted;
  info.ratioPercent = 87;
  info.itemCount = 2;
  info.selectedCount = 0;
  const std::vector<StatusSegment> segments = prism::layout::StatusSegments(layout, info);
  std::string joined;
  for (const StatusSegment& segment : segments) {
    const std::string label = StatusSegmentLabel(segment);
    if (label.empty()) continue;
    if (!joined.empty()) joined += " | ";
    joined += label;
  }
  std::printf("  [status] %s\n", joined.c_str());
  PRISM_CHECK_EQ(joined, std::string("未加密 | 方法未知 | 字典未知 | 条目 2 | 选中 0 | 压缩率 87%"));
  // The divider is ink, not a reading: it must not become an element.
  for (const StatusSegment& segment : segments) {
    if (segment.id != StatusId::Divider) continue;
    PRISM_CHECK(StatusSegmentLabel(segment).empty());
  }
}

PRISM_TEST("c86 an unknown ratio and empty counts say so instead of inventing numbers") {
  const Layout layout = prism::layout::Compute(1600, 800, false, true);
  StatusInfo info;  // every field at its "unknown" value
  info.crypto = CryptoState::Unknown;
  info.itemCount = 3;
  info.selectedCount = 0;
  const std::vector<StatusSegment> segments = prism::layout::StatusSegments(layout, info);
  std::vector<std::string> labels;
  for (const StatusSegment& segment : segments) {
    const std::string label = StatusSegmentLabel(segment);
    if (!label.empty()) labels.push_back(label);
  }
  // -1 means "the engine did not say", and the band says that rather than "0%".
  PRISM_CHECK(!labels.empty());
  PRISM_CHECK_EQ(labels.back(), std::string("压缩率未知"));
  PRISM_CHECK_EQ(labels.front(), std::string("加密状态未知"));
  PRISM_CHECK_EQ(labels[3], std::string("条目 3"));
}

PRISM_TEST("c86 a band label owns its text (no view into the caller's StatusInfo)") {
  const Layout layout = prism::layout::Compute(1600, 800, false, true);
  const std::vector<StatusSegment> segments = SegmentsFromTemporary(layout);
  if (!PRISM_CHECK(!segments.empty())) return;
  // The ratio is the LAST label, and it is the one that used to hold a view into
  // StatusInfo::ratioView -- a std::string owned by the caller.
  PRISM_CHECK_EQ(StatusSegmentLabel(segments.back()), std::string("压缩率 87.5%"));
  // Reading every label here is what the accessibility tree and the painter do;
  // on the ASan lane a view-based implementation dies at this line.
  std::string all;
  for (const StatusSegment& segment : segments) {
    const std::string label = StatusSegmentLabel(segment);
    if (!label.empty()) all += label;
  }
  PRISM_CHECK(all.find("压缩率 87.5%") != std::string::npos);
}

PRISM_TEST("c86 a compressing archive shows progress instead of a ratio") {
  const Layout layout = prism::layout::Compute(1600, 800, false, true);
  StatusInfo info;
  info.crypto = CryptoState::Unencrypted;
  info.compressing = true;
  info.ratioPercent = 50;  // present, but the run is not finished
  info.itemCount = 0;
  info.selectedCount = 0;
  const std::vector<StatusSegment> segments = prism::layout::StatusSegments(layout, info);
  bool sawCompressing = false;
  for (const StatusSegment& segment : segments) {
    if (segment.id != StatusId::Ratio) continue;
    sawCompressing = StatusSegmentLabel(segment) == "压缩中…";
    // No half-baked percentage while the engine is still writing.
    PRISM_CHECK_EQ(segment.value, -1);
  }
  PRISM_CHECK(sawCompressing);
}
