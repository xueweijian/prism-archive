// TDD (M4, contract-first): the delayed-extract stream must plan every seek as
// CursorOnly / LookbackHit / RestartDecode and REFUSE the plans it cannot pay
// for, with a reason the user can act on.
//
// The Windows stream consumes exactly these plans, so a wrong plan means wrong
// bytes served to Explorer -- here the math is checked on Linux in seconds.
//
// Three mutation builds must all go red (Tests/CMakeLists.txt WILL_FAIL gates):
//   PRISM_DND_MUTANT_SEEK_FORWARD_ONLY     every seek planned as CursorOnly
//   PRISM_DND_MUTANT_SEEK_IGNORE_BUDGET    restarts planned past the byte budget
//   PRISM_DND_MUTANT_SEEK_TRUST_RESIDENCY  ... (planner has no residency rule;
//                                          kept as the stream-side re-plan test)
//
// The last section is the MEASUREMENT that chose the two default parameters.
// It is not decoration: the numbers in kDefaultSeekLookbackBytes /
// kDefaultRestartDecodeBudgetBytes are asserted here, so changing a default
// without redoing the measurement fails the suite (L1 + L5 evidence).
#include "../Src/PrismCore/DragDrop.hpp"
#include "mini_test.hpp"

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

using prism::dnd::kDefaultRestartDecodeBudgetBytes;
using prism::dnd::kDefaultSeekLookbackBytes;
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
  policy.restartBytesUsed = 0;
  policy.restartDecodeBudget = kDefaultRestartDecodeBudgetBytes;
  return policy;
}
}  // namespace

// ---------------------------------------------------------------------------
// Shape of the plan
// ---------------------------------------------------------------------------
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
  // Inclusive boundary: a back-step of exactly the ring size is still resident.
  const auto plan = PlanSeek(4096, 0, kSize, Policy());
  PRISM_CHECK_EQ(plan.kind, SeekPlan::LookbackHit);
  // ...and one byte past it is not.
  const auto past = PlanSeek(8193, 8193 - 4097, kSize, Policy());
  PRISM_CHECK_EQ(past.kind, SeekPlan::RestartDecode);
  PRISM_CHECK_EQ(past.discardBytes, int64_t(8193 - 4097));
}

PRISM_TEST("lookback disabled sends every backward seek to the decoder restart") {
  // This is exactly the plan the stream re-derives when its ring turns out not
  // to cover the target (PRISM_DND_MUTANT_SEEK_TRUST_RESIDENCY removes the
  // re-plan, and the Windows suite's non-resident case goes red).
  SeekPolicy policy = Policy();
  policy.useLookback = false;
  const auto plan = PlanSeek(4096, 4000, kSize, policy);
  PRISM_CHECK_EQ(plan.kind, SeekPlan::RestartDecode);
  PRISM_CHECK_EQ(plan.discardBytes, int64_t(4000));
}

// ---------------------------------------------------------------------------
// The two budgets. Both must refuse, and the reasons must name different
// escapes -- a caller that only knows "refused" cannot tell the user what to do.
// ---------------------------------------------------------------------------
PRISM_TEST("restart COUNT budget exhaustion refuses with a user-actionable reason") {
  SeekPolicy policy = Policy();
  policy.maxRestarts = 2;
  policy.restartsUsed = 2;  // both restarts already spent on this stream
  const auto plan = PlanSeek(5000, 10, kSize, policy);
  PRISM_CHECK(plan.refused);
  PRISM_CHECK(plan.reason.find("restart budget exhausted") != std::string::npos);
  // The refusal must name the escape hatch, not just complain.
  PRISM_CHECK(plan.reason.find("right-click") != std::string::npos);
}

PRISM_TEST("restart BYTE budget refuses a re-decode it cannot pay for in time") {
  // A count budget alone cannot bound latency: two restarts into a 256 MiB
  // solid block are four seconds of decode. The byte budget is the one that
  // bounds wall-clock, so it must bite on the FIRST oversized restart even
  // though no restart has happened yet.
  SeekPolicy policy = Policy();
  policy.maxRestarts = 8;
  policy.restartsUsed = 0;
  policy.restartDecodeBudget = 4 * 1024 * 1024;
  const auto ok = PlanSeek(5 * 1024 * 1024, 1 * 1024 * 1024, 10 * 1024 * 1024, policy);
  PRISM_CHECK_EQ(ok.kind, SeekPlan::RestartDecode);
  PRISM_CHECK(!ok.refused);
  const auto tooFar = PlanSeek(9 * 1024 * 1024, 6 * 1024 * 1024, 10 * 1024 * 1024, policy);
  PRISM_CHECK(tooFar.refused);
  PRISM_CHECK(tooFar.reason.find("re-decode budget exhausted") != std::string::npos);
  PRISM_CHECK(tooFar.reason.find("right-click") != std::string::npos);
  // The reason must quote the numbers, otherwise a support log is useless.
  PRISM_CHECK(tooFar.reason.find("4194304") != std::string::npos);
}

PRISM_TEST("spent restart bytes accumulate across restarts and then refuse") {
  SeekPolicy policy = Policy();
  policy.restartDecodeBudget = 8 * 1024 * 1024;
  policy.restartBytesUsed = 6 * 1024 * 1024;
  // 1 MiB more fits (6 + 1 <= 8).
  PRISM_CHECK_EQ(PlanSeek(4 * 1024 * 1024, 1 * 1024 * 1024, kSize, policy).kind,
                 SeekPlan::RestartDecode);
  // 3 MiB more does not (6 + 3 > 8) -> refused, and NOT silently downgraded.
  const auto over = PlanSeek(4 * 1024 * 1024, 3 * 1024 * 1024, kSize, policy);
  PRISM_CHECK(over.refused);
  // Seeking to the block start is always affordable: the prefix is zero bytes.
  PRISM_CHECK_EQ(PlanSeek(4 * 1024 * 1024, 0, kSize, policy).kind, SeekPlan::RestartDecode);
}

PRISM_TEST("lookback hits stay available after both budgets are gone") {
  SeekPolicy policy = Policy();
  policy.maxRestarts = 2;
  policy.restartsUsed = 2;
  policy.restartDecodeBudget = 0;
  policy.restartBytesUsed = 0;
  const auto ring = PlanSeek(4096, 4096 - 4096, kSize, policy);
  PRISM_CHECK_EQ(ring.kind, SeekPlan::LookbackHit);
  PRISM_CHECK(!ring.refused);
  const auto forward = PlanSeek(4096, 5000, kSize, policy);
  PRISM_CHECK_EQ(forward.kind, SeekPlan::CursorOnly);
  PRISM_CHECK_EQ(forward.discardBytes, int64_t(904));
}

// ---------------------------------------------------------------------------
// Range refusals: distinct reasons, and EOF itself is legal.
// ---------------------------------------------------------------------------
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
  // A refused plan must never carry a discard: the stream would execute it.
  PRISM_CHECK_EQ(negative.discardBytes, int64_t(0));
  PRISM_CHECK_EQ(beyond.discardBytes, int64_t(0));
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

PRISM_TEST("plan determinism: same inputs, same plan, no hidden state") {
  const SeekPolicy policy = Policy();
  const auto a = PlanSeek(8192, 0, kSize, policy);
  const auto b = PlanSeek(8192, 0, kSize, policy);
  PRISM_CHECK_EQ(a.kind, b.kind);
  PRISM_CHECK_EQ(a.discardBytes, b.discardBytes);
  PRISM_CHECK_EQ(a.refused, b.refused);
  PRISM_CHECK(a.reason == b.reason);
}

// ---------------------------------------------------------------------------
// The measurement that chose the defaults.
//
// Source: the 7-Zip CLI on this machine (7-Zip 24.08, `-m0=lzma2 -mx=5 -ms=on`),
// timing `7z e -so archive <files 0..k>`, which performs exactly the prefix
// decode 7z.dll performs for a solid-block seek (see 7zExtract.cpp:319-348).
// Three corpora, 16 x 1 MiB, 5 repetitions each, min taken:
//
//   corpus   packed   marginal decode   ms per MiB of discarded prefix
//   text      2.70x        60.5 MiB/s          16.54
//   mixed     1.77x        83.2 MiB/s          12.02
//   random    1.00x       713.6 MiB/s           1.40
//   RAM memcpy (what the ring costs): 15 797 MiB/s = 0.063 ms per MiB
//
// Two conclusions, both asserted below:
//   1. The ring is worth its RAM: serving a back-step from it is 22x-260x
//      cheaper per byte than re-decoding the same bytes (text worst case).
//   2. A COUNT budget cannot bound latency. 2 restarts into a 256 MiB text
//      block = 2 x 4.2 s. A BYTE budget can: 16 MiB of discarded prefix is
//      264 ms worst case / 22 ms best case, which is why
//      kDefaultRestartDecodeBudgetBytes is 16 MiB and not "2 restarts".
// ---------------------------------------------------------------------------
namespace {
constexpr double kMeasuredMsPerMiBText = 16.54;
constexpr double kMeasuredMsPerMiBRandom = 1.40;
constexpr double kMeasuredMemcpyMsPerMiB = 0.063;
}  // namespace

PRISM_TEST("measured: the ring is far cheaper per byte than the restart it replaces") {
  const double ringPerMiB = kMeasuredMemcpyMsPerMiB;
  const double textPerMiB = kMeasuredMsPerMiBText;
  const double ratio = textPerMiB / ringPerMiB;
  std::cout << "    ring service " << ringPerMiB << " ms/MiB vs text re-decode "
            << textPerMiB << " ms/MiB -> " << ratio << "x cheaper" << std::endl;
  PRISM_CHECK(ratio > 20.0);  // measured 262x; 20x is the conservative floor
  // The ring must at least cover a 4 MiB back-step for less than one restart of
  // the same size: this is the inequality that makes 4 MiB a sane default.
  const double ringCost4MiB = 4.0 * ringPerMiB;
  const double restartCost4MiB = 4.0 * textPerMiB;
  PRISM_CHECK(ringCost4MiB < restartCost4MiB / 100.0);
}

PRISM_TEST("measured: the 16 MiB byte budget bounds added latency; a count cannot") {
  SeekPolicy policy;  // the shipped defaults
  PRISM_CHECK_EQ(policy.lookbackBytes, kDefaultSeekLookbackBytes);
  PRISM_CHECK_EQ(policy.restartDecodeBudget, kDefaultRestartDecodeBudgetBytes);
  const double budgetMiB = static_cast<double>(kDefaultRestartDecodeBudgetBytes) / (1024.0 * 1024.0);
  const double worst = budgetMiB * kMeasuredMsPerMiBText;
  const double best = budgetMiB * kMeasuredMsPerMiBRandom;
  std::cout << "    default budget " << budgetMiB << " MiB -> worst-case added latency "
            << worst << " ms (text), " << best << " ms (stored)" << std::endl;
  // A quarter second is the ceiling we are willing to make a drop target wait
  // before it gets an answer at all; anything longer must be refused instead.
  PRISM_CHECK(worst <= 300.0);
  PRISM_CHECK(best <= 40.0);
  // A 256 MiB text-like solid block would cost 2 x 4.2 s under a 2-restart
  // count budget -- 30x over the ceiling, i.e. the count budget is unbounded.
  const double twoRestarts256MiB = 2.0 * 256.0 * kMeasuredMsPerMiBText;
  PRISM_CHECK(twoRestarts256MiB > 4000.0);
  // ...and the default byte budget refuses that same seek up front.
  SeekPolicy big = policy;
  const auto refused = PlanSeek(200 * 1024 * 1024, 128 * 1024 * 1024, 256 * 1024 * 1024, big);
  PRISM_CHECK(refused.refused);
  PRISM_CHECK(refused.reason.find("right-click") != std::string::npos);
}

PRISM_TEST("measured: a 4 MiB ring covers the back-steps real readers issue") {
  // Explorer's copy engine and most format parsers read a prefix, seek back to
  // 0, then read the whole file. Under the default ring NONE of those back-steps
  // may cost a restart as long as the prefix stayed inside the ring.
  const SeekPolicy shipped;
  for (const int64_t prefix : {int64_t(64) * 1024, int64_t(256) * 1024,
                               int64_t(1024) * 1024, int64_t(4) * 1024 * 1024}) {
    const auto plan = PlanSeek(prefix, 0, 256 * 1024 * 1024, shipped);
    PRISM_CHECK_EQ(plan.kind, SeekPlan::LookbackHit);
  }
  // One byte past the ring has to restart -- and at offset 0 that restart is
  // free, which is why the ring only has to cover the *read-ahead* prefix.
  const auto past = PlanSeek(int64_t(4) * 1024 * 1024 + 1, 0, 256 * 1024 * 1024, shipped);
  PRISM_CHECK_EQ(past.kind, SeekPlan::RestartDecode);
  PRISM_CHECK_EQ(past.discardBytes, int64_t(0));  // block start: nothing to discard
}



// ---------------------------------------------------------------------------
// DelayedSeekCursor: the same state machine the Windows IStream drives.
//
// A synthetic solid block stands in for the decoder: `pull` yields bytes from a
// contiguous buffer, `restart` rewinds it to 0. That is exactly the contract
// ArchiveItemStreamProducer offers (Next/Restart over one 7z.dll Extract), so
// every invariant below is the one the Windows suite asserts against the real
// DLL -- only without the CI round.
// ---------------------------------------------------------------------------
namespace {

// Offset-sensitive bytes: a wrong-offset read can never alias the expected
// content (a repeating pattern could pass at period multiples).
std::string MakeSolidBlock(size_t bytes, uint32_t seed) {
  std::string out(bytes, '\0');
  uint32_t state = seed;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    const uint32_t mix = static_cast<uint32_t>((i * 2654435761u) >> 13);
    out[i] = static_cast<char>(((state >> 16) ^ mix) & 0xFF);
  }
  return out;
}

struct FakeDecoder {
  std::string block;
  size_t cursor = 0;
  uint64_t pulledBytes = 0;

  std::string Pull(size_t maxBytes) {
    if (cursor >= block.size()) return {};
    const size_t n = (block.size() - cursor) < maxBytes ? (block.size() - cursor) : maxBytes;
    std::string out = block.substr(cursor, n);
    cursor += n;
    pulledBytes += n;
    return out;
  }
  bool Restart() {
    cursor = 0;
    return true;
  }
};

std::string ReadN(prism::dnd::DelayedSeekCursor& cursor, size_t n) {
  std::string out;
  out.resize(n);
  size_t done = 0;
  while (done < n) {
    const auto r = cursor.Read(reinterpret_cast<uint8_t*>(out.data()) + done, n - done);
    if (r.failed || r.served == 0) break;
    done += r.served;
  }
  out.resize(done);
  return out;
}

}  // namespace

PRISM_TEST("cursor: sequential read of a solid block is byte-exact and looks back for free") {
  const std::string block = MakeSolidBlock(3 * 1024 * 1024 + 12345, 99);
  FakeDecoder decoder{block, 0, 0};
  prism::dnd::SeekPolicy policy;
  policy.lookbackBytes = 512 * 1024;
  prism::dnd::DelayedSeekCursor cursor(
      block.size(), [&](size_t n) { return decoder.Pull(n); }, [&] { return decoder.Restart(); },
      policy);

  const std::string got = ReadN(cursor, block.size());
  PRISM_CHECK_EQ(got.size(), block.size());
  PRISM_CHECK(got == block);  // D2: content, not merely size
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(0));
  // A back-step inside the ring must be served from it: no new decoder bytes.
  const uint64_t pulledBefore = decoder.pulledBytes;
  std::string reason;
  PRISM_CHECK(cursor.Seek(static_cast<int64_t>(block.size()) - 1000, &reason));
  PRISM_CHECK(reason.empty());
  const std::string tail = ReadN(cursor, 1000);
  PRISM_CHECK(tail == block.substr(block.size() - 1000));
  PRISM_CHECK_EQ(decoder.pulledBytes, pulledBefore);  // zero decoder work
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(0));
}

PRISM_TEST("cursor: a restart re-decodes and still yields byte-exact bytes") {
  const std::string block = MakeSolidBlock(2 * 1024 * 1024 + 7, 5);
  FakeDecoder decoder{block, 0, 0};
  prism::dnd::SeekPolicy policy;
  policy.lookbackBytes = 64 * 1024;  // small: force a restart
  prism::dnd::DelayedSeekCursor cursor(
      block.size(), [&](size_t n) { return decoder.Pull(n); }, [&] { return decoder.Restart(); },
      policy);

  PRISM_CHECK(ReadN(cursor, 1000 * 1000).size() == 1000u * 1000u);
  std::string reason;
  // Back to 100: 1000000 bytes back is far outside the 64 KiB ring.
  PRISM_CHECK(cursor.Seek(100, &reason));
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(1));
  PRISM_CHECK_EQ(cursor.restartBytesUsed(), uint64_t(100));
  const std::string got = ReadN(cursor, 4096);
  PRISM_CHECK(got == block.substr(100, 4096));
  // The wasted work is booked: 100 bytes re-decoded and dropped.
  PRISM_CHECK_EQ(cursor.wastedBytes(), uint64_t(100));
  PRISM_CHECK(cursor.diagnostics().find("restart#1(100B)") != std::string::npos);
}

PRISM_TEST("cursor: a forward skip lands on the exact offset without retaining the gap") {
  const std::string block = MakeSolidBlock(1024 * 1024, 3);
  FakeDecoder decoder{block, 0, 0};
  prism::dnd::SeekPolicy policy;
  policy.lookbackBytes = 256 * 1024;
  prism::dnd::DelayedSeekCursor cursor(
      block.size(), [&](size_t n) { return decoder.Pull(n); }, [&] { return decoder.Restart(); },
      policy);
  // Seek far forward BEFORE any read: nothing is decoded yet (O(1) seek).
  std::string reason;
  PRISM_CHECK(cursor.Seek(900 * 1024, &reason));
  PRISM_CHECK_EQ(decoder.pulledBytes, uint64_t(0));
  const std::string got = ReadN(cursor, 50);
  PRISM_CHECK(got == block.substr(900 * 1024, 50));
  // The skipped 900 KiB is charged as skip cost, not as restart waste.
  PRISM_CHECK(cursor.skippedBytes() >= 900 * 1024 - prism::dnd::DelayedSeekCursor::kPullChunkBytes);
  PRISM_CHECK_EQ(cursor.wastedBytes(), uint64_t(0));
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(0));
  // THE INVARIANT: the 900 KiB gap was skipped, never retained, so the ring
  // must not claim it. PR8's Read() advanced the frontier over the skipped
  // chunks but left the ring anchored at its old start, so the window
  // advertised a range it did not hold -- the next in-window back-step then
  // served the wrong bytes (or indexed past the buffer). Asserted directly:
  // a purely functional test can miss it when the wrongly-claimed offset
  // happens to land on the "not resident -> restart" branch (D5).
  PRISM_CHECK_EQ(cursor.ring().end(), cursor.frontier());
  // The base is the frontier minus the freshly retained tail (nothing over budget).
  PRISM_CHECK(cursor.ring().residentBytes() <= cursor.ring().capacity());
  PRISM_CHECK(cursor.frontier() - cursor.ring().base() == cursor.ring().residentBytes());
  PRISM_CHECK(cursor.ring().base() >= 900 * 1024);  // no skipped byte is claimed
  const std::string before = ReadN(cursor, 10);
  PRISM_CHECK(before == block.substr(900 * 1024 + 50, 10));
  // ...and functionally: read deep into the block, then back-step INSIDE the
  // ring window. Those bytes must be the real ones, not the skipped range.
  const std::string bulk = ReadN(cursor, 200 * 1024);
  PRISM_CHECK(bulk == block.substr(900 * 1024 + 60, 200 * 1024));
  PRISM_CHECK(cursor.Seek(1000 * 1024, &reason));
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(0));  // in-window: no restart
  const std::string inside = ReadN(cursor, 128);
  PRISM_CHECK(inside == block.substr(1000 * 1024, 128));
  // A back-step over the gap itself is NOT resident: restart, never guess.
  PRISM_CHECK(cursor.Seek(800 * 1024, &reason));
  PRISM_CHECK_EQ(cursor.restarts(), uint64_t(1));
  const std::string back = ReadN(cursor, 64);
  PRISM_CHECK(back == block.substr(800 * 1024, 64));  // byte-exact after the restart
}

PRISM_TEST("cursor: a spent byte budget refuses and leaves the cursor untouched") {
  const std::string block = MakeSolidBlock(4 * 1024 * 1024, 11);
  FakeDecoder decoder{block, 0, 0};
  prism::dnd::SeekPolicy policy;
  policy.lookbackBytes = 4096;             // ring can never help here
  policy.restartDecodeBudget = 64 * 1024;  // far below a 3 MiB restart
  prism::dnd::DelayedSeekCursor cursor(
      block.size(), [&](size_t n) { return decoder.Pull(n); }, [&] { return decoder.Restart(); },
      policy);

  PRISM_CHECK(ReadN(cursor, 4 * 1024 * 1024).size() == 4u * 1024 * 1024);
  const uint64_t posBefore = cursor.position();
  const uint64_t restartsBefore = cursor.restarts();
  const uint64_t pulledBefore = decoder.pulledBytes;
  std::string reason;
  PRISM_CHECK(!cursor.Seek(3 * 1024 * 1024, &reason));
  PRISM_CHECK(reason.find("right-click") != std::string::npos);
  PRISM_CHECK_EQ(cursor.position(), posBefore);
  PRISM_CHECK_EQ(cursor.restarts(), restartsBefore);
  PRISM_CHECK_EQ(decoder.pulledBytes, pulledBefore);  // no decode was started
  // The stream stays honest: reading at EOF is still EOF, not a failure.
  uint8_t probe[8];
  const auto r = cursor.Read(probe, sizeof(probe));
  PRISM_CHECK(!r.failed);
  PRISM_CHECK(r.eof);
}

PRISM_TEST("ring: the window promises its FULL capacity, not capacity minus a chunk") {
  // The bug this locks: the ring used to evict whole chunks only, so a 4 MiB
  // window on a 64 KiB pull stream actually retained 4 MiB - 64 KiB. The
  // planner, however, grants a LookbackHit for a rewind of exactly
  // lookbackBytes, so the boundary rewind succeeded and then the read hit a
  // residency gap -- a FAILED stream where an honest restart was expected.
  // A window that advertises more than it holds is the defect; assert the
  // endpoints the planner actually trusts, not just "<= capacity".
  constexpr size_t kCap = 256 * 1024;
  constexpr size_t kChunk = 64 * 1024;
  prism::dnd::RecentByteRing ring(kCap);
  const std::string chunk(kChunk, 'q');
  for (size_t i = 0; i < 8; ++i) ring.Append(chunk);
  PRISM_CHECK_EQ(ring.capacity(), kCap);
  PRISM_CHECK_EQ(ring.end(), uint64_t(kChunk * 8));
  // The whole window is resident: exactly capacity, not capacity - chunk.
  PRISM_CHECK_EQ(ring.residentBytes(), kCap);
  PRISM_CHECK_EQ(ring.end() - ring.base(), uint64_t(kCap));
  // ...and it is byte-exact at BOTH edges: the oldest retained byte and the
  // newest are both readable, so the boundary rewind really is payable.
  std::string edge(1, '\0');
  PRISM_CHECK_EQ(ring.Copy(ring.base(), reinterpret_cast<uint8_t*>(edge.data()), 1),
                 size_t(1));
  PRISM_CHECK_EQ(ring.Copy(ring.end() - 1, reinterpret_cast<uint8_t*>(edge.data()), 1),
                 size_t(1));
  // One byte before the window is honestly NOT resident (never a guess).
  PRISM_CHECK_EQ(ring.Copy(ring.base() - 1, reinterpret_cast<uint8_t*>(edge.data()), 1),
                 size_t(0));
  // A multi-chunk copy spanning the whole window succeeds in one go -- the
  // trimmed head chunk must be traversed correctly, not skipped.
  std::string whole(kCap, '\0');
  PRISM_CHECK_EQ(ring.Copy(ring.base(), reinterpret_cast<uint8_t*>(whole.data()), kCap),
                 kCap);
  PRISM_CHECK(whole == std::string(kCap, 'q'));
  // An odd-sized append must still land on exactly capacity (trim is byte
  // exact, not chunk exact).
  ring.Append(std::string(1234, 'r'));
  PRISM_CHECK_EQ(ring.residentBytes(), kCap);
  PRISM_CHECK_EQ(ring.end() - ring.base(), uint64_t(kCap));
  std::string tail(1234, '\0');
  PRISM_CHECK_EQ(ring.Copy(ring.end() - 1234, reinterpret_cast<uint8_t*>(tail.data()), 1234),
                 size_t(1234));
  PRISM_CHECK(tail == std::string(1234, 'r'));
}

PRISM_TEST("ring: chunked ring is O(1) per append; the erase-front form is not") {
  // PR8 kept the ring as a std::string and called erase(0, excess) on every
  // append past capacity. That is a memmove of the whole capacity per chunk.
  // Measure both at the shipped 4 MiB capacity, 64 KiB chunks, 256 chunks
  // (= 16 MiB streamed, i.e. one solid block's worth of read traffic).
  constexpr size_t kCap = prism::dnd::kDefaultSeekLookbackBytes;
  constexpr size_t kChunk = 64 * 1024;
  constexpr size_t kChunks = 256;
  const std::string chunk(kChunk, 'x');

  const auto timeIt = [&](bool useChunked) {
    double best = 1e18;
    for (int rep = 0; rep < 5; ++rep) {
      const auto t0 = std::chrono::steady_clock::now();
      if (useChunked) {
        prism::dnd::RecentByteRing ring(kCap);
        for (size_t i = 0; i < kChunks; ++i) ring.Append(chunk);
        if (ring.residentBytes() > kCap) return -1.0;  // never silently over budget
      } else {
        std::string flat;
        for (size_t i = 0; i < kChunks; ++i) {
          flat.append(chunk);
          if (flat.size() > kCap) flat.erase(0, flat.size() - kCap);
        }
      }
      const double dt =
          std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
      if (dt < best) best = dt;
    }
    return best;
  };

  const double chunked = timeIt(true);
  const double flat = timeIt(false);
  PRISM_CHECK(chunked > 0.0);
  std::cout << "    ring @4 MiB x 16 MiB streamed: chunked " << chunked * 1e3
            << " ms vs erase-front " << flat * 1e3 << " ms (" << flat / chunked
            << "x)" << std::endl;
  // This is a MICRO-BENCHMARK inside a correctness suite. The ring's advantage
  // at 4 MiB is real and large developer-side (~30-60x), but the ratio depends
  // on the allocator and on how much of the working set survives in cache --
  // on a shared CI runner it is not a stable pass/fail signal, and a flaky
  // gate on a performance number trains people to ignore red.
  //
  // So: the DIRECTION is asserted (the ring must not be slower), the MAGNITUDE
  // is reported. The magnitude lives in the run log and in docs/PERF.md.
  PRISM_CHECK(chunked < flat);
}

PRISM_TEST("ring: capacity, residency and eviction are exact") {
  prism::dnd::RecentByteRing ring(1024);
  ring.Append(std::string(256, 'a'));
  ring.Append(std::string(256, 'b'));
  PRISM_CHECK_EQ(ring.base(), uint64_t(0));
  PRISM_CHECK_EQ(ring.end(), uint64_t(512));
  PRISM_CHECK_EQ(ring.residentBytes(), size_t(512));
  // Grow past capacity: the oldest chunks fall out, base advances exactly.
  ring.Append(std::string(256, 'c'));
  ring.Append(std::string(256, 'd'));
  ring.Append(std::string(256, 'e'));
  PRISM_CHECK(ring.residentBytes() <= 1024);
  PRISM_CHECK_EQ(ring.end(), uint64_t(1280));
  PRISM_CHECK(ring.base() > 0);
  // Residency is honest at both edges.
  PRISM_CHECK(ring.Holds(ring.base()));
  PRISM_CHECK(ring.Holds(ring.end()));
  PRISM_CHECK(!ring.Holds(ring.base() - 1));
  uint8_t probe[8]{};
  PRISM_CHECK_EQ(ring.Copy(ring.base() - 1, probe, 1), size_t(0));
  // The retained tail copies exactly.
  std::string tail(8, '\0');
  PRISM_CHECK_EQ(ring.Copy(1272, reinterpret_cast<uint8_t*>(tail.data()), 8), size_t(8));
  PRISM_CHECK(tail == std::string(8, 'e'));
}

PRISM_TEST_MAIN()
