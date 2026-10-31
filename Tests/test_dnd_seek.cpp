// TDD (M4, contract-first): the delayed-extract stream seek planner must
// split every seek into CursorOnly / LookbackHit / RestartDecode and refuse
// beyond-budget re-decodes with a reason the user can act on. The Windows
// stream consumes exactly these plans, so a wrong plan means wrong bytes
// served to Explorer -- here the math is checked on Linux in seconds.
// Negative control: the mutant build forces every seek through CursorOnly
// and must fail this suite (Tests/CMakeLists.txt WILL_FAIL gate).
#include "../Src/PrismCore/DragDrop.hpp"
#include "mini_test.hpp"

#include <string>

using prism::dnd::PlanSeek;
using prism::dnd::SeekPlan;
using prism::dnd::SeekPolicy;

namespace {
// 10 MiB stream, default-ish policy but a small lookback so the boundary is
// expressible in plain numbers.
constexpr int64_t kSize = 10 * 1024 * 1024;
SeekPolicy Policy() {
  SeekPolicy policy;
  policy.useLookback = true;
  policy.lookbackBytes = 4096;
  policy.maxRestarts = 2;
  policy.restartsUsed = 0;
  return policy;
}
}  // namespace

PRISM_TEST("forward seek plans CursorOnly with exact skip distance") {
  const auto plan = PlanSeek(1000, 4500, kSize, Policy());
  PRISM_CHECK_EQ(plan.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(3500));
  PRISM_CHECK(!plan.refused);
  PRISM_CHECK(plan.reason.empty());
}

PRISM_TEST("seek to the current position is a zero-cost CursorOnly no-op") {
  const auto plan = PlanSeek(4096, 4096, kSize, Policy());
  PRISM_CHECK_EQ(plan.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(0));
  PRISM_CHECK(!plan.refused);
}

PRISM_TEST("small backward seek inside the ring hits the lookback window") {
  const auto plan = PlanSeek(4096, 4096 - 1000, kSize, Policy());
  PRISM_CHECK_EQ(plan.kind, SeekPlan::LookbackHit);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(0));
}

PRISM_TEST("rewind of exactly lookbackBytes is still a lookback hit") {
  const auto plan = PlanSeek(4096, 0, kSize, Policy());  // back == 4096
  PRISM_CHECK_EQ(plan.kind, SeekPlan::LookbackHit);
}

PRISM_TEST("one byte beyond the lookback window costs a decoder restart") {
  SeekPolicy policy = Policy();
  policy.lookbackBytes = 4096;
  // back == 4097: outside the ring by exactly one byte.
  const auto plan = PlanSeek(8193, 8193 - 4097, kSize, policy);
  PRISM_CHECK_EQ(plan.kind, SeekPlan::RestartDecode);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(8193 - 4097));
}

PRISM_TEST("restart budget exhaustion refuses with a user-actionable reason") {
  SeekPolicy policy = Policy();
  policy.maxRestarts = 2;
  policy.restartsUsed = 2;  // both restarts already spent on this stream
  const auto plan = PlanSeek(5000, 10, kSize, policy);
  PRISM_CHECK(plan.refused);
  PRISM_CHECK(plan.reason.find("restart budget exhausted") != std::string::npos);
  // The refusal must name the escape hatch, not just complain.
  PRISM_CHECK(plan.reason.find("right-click") != std::string::npos);
}

PRISM_TEST("lookback hits stay available after the restart budget is gone") {
  SeekPolicy policy = Policy();
  policy.maxRestarts = 2;
  policy.restartsUsed = 2;
  const auto ring = PlanSeek(4096, 4096 - 4096, kSize, policy);
  PRISM_CHECK_EQ(ring.kind, SeekPlan::LookbackHit);
  PRISM_CHECK(!ring.refused);
  const auto forward = PlanSeek(4096, 5000, kSize, policy);
  PRISM_CHECK_EQ(forward.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(forward.discardBytes, int64_t(904));
}

PRISM_TEST("negative and beyond-size targets are refused with distinct reasons") {
  const auto negative = PlanSeek(100, -1, kSize, Policy());
  PRISM_CHECK(negative.refused);
  PRISM_CHECK(negative.reason.find("negative") != std::string::npos);
  const auto beyond = PlanSeek(100, kSize + 1, kSize, Policy());
  PRISM_CHECK(beyond.refused);
  PRISM_CHECK(beyond.reason.find("beyond the declared stream size") != std::string::npos);
  // Exactly at EOF is legal: reading there simply reports end-of-stream.
  const auto atEof = PlanSeek(0, kSize, kSize, Policy());
  PRISM_CHECK_EQ(atEof.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(atEof.discardBytes, kSize);
}

PRISM_TEST("seek from the known stream end lands via the caller's END math") {
  // STREAM_SEEK_END with delta -256 resolves to target = size - 256 BEFORE
  // planning; the planner itself must treat it like any other absolute seek.
  const int64_t target = kSize - 256;
  const auto fromStart = PlanSeek(0, target, kSize, Policy());
  PRISM_CHECK_EQ(fromStart.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(fromStart.discardBytes, target);
  // A jump from inside the stream back across the END-resolved position.
  const auto across = PlanSeek(target + 128, target, kSize, Policy());
  PRISM_CHECK_EQ(across.kind, SeekPlan::LookbackHit);
}

PRISM_TEST("lookback disabled sends every backward seek to the decoder restart") {
  SeekPolicy policy = Policy();
  policy.useLookback = false;
  const auto plan = PlanSeek(4096, 4000, kSize, policy);
  PRISM_CHECK_EQ(plan.kind, SeekPlan::RestartDecode);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(4000));
}

PRISM_TEST("plan determinism: same inputs, same plan, no hidden state") {
  const SeekPolicy policy = Policy();
  const auto a = PlanSeek(8192, 0, kSize, policy);
  const auto b = PlanSeek(8192, 0, kSize, policy);
  PRISM_CHECK_EQ(a.kind, b.kind);
  PRISM_CHECK_EQ(a.discardBytes, b.discardBytes);
  PRISM_CHECK_EQ(a.refused, b.refused);
}

PRISM_TEST_MAIN()