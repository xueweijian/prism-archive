// Security-chain policy tests (blueprint ch. 7.1 #1 and #3):
//   * MoTW inheritance decision table + verbatim Zone.Identifier fields
//   * AMSI Verdict mapping / "Unavailable is never Allow" / target selection /
//     chunk plan (the Windows half lives in Tests/win/test_motw_stamp.cpp)
//   * Zip-bomb fuse: per-entry ratio, disk proximity, and the two-condition
//     "critical" latch the blueprint names.
//
// Five mutation builds (Tests/CMakeLists.txt) invert one rule each and MUST fail
// this suite; see the PR for the injection evidence (CONTRACTS D3).
#include "SecurityGuard.hpp"
#include "mini_test.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

using prism::AmsiVerdict;
using prism::MotwSource;
using prism::SecurityAction;

// ---------------------------------------------------------------------------
// AMSI
// ---------------------------------------------------------------------------

PRISM_TEST("amsi: AMSI_RESULT classification follows the documented contract") {
  // learn.microsoft.com/windows/win32/api/amsi/ne-amsi-amsi_result
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0), AmsiVerdict::Allow);        // CLEAN
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(1), AmsiVerdict::Allow);        // NOT_DETECTED
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0x4000), AmsiVerdict::Block);   // admin block start
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0x4FFF), AmsiVerdict::Block);   // admin block end
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0x8000), AmsiVerdict::Block);   // DETECTED
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0x9000), AmsiVerdict::Block);   // beyond DETECTED
  // A provider risk estimate (2..32767, outside the admin range) is not a
  // detection, but it is not clean either: no definitive verdict -> Unavailable.
  // 0x2710 = 10000 is the documented "estimated risk level" range, i.e. outside
  // the admin-blocked window [0x4000, 0x4FFF]; 20000 would be inside it.
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(2), AmsiVerdict::Unavailable);
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(10000), AmsiVerdict::Unavailable);
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(0x5000), AmsiVerdict::Unavailable);
  PRISM_CHECK_EQ(prism::ClassifyAmsiResult(-1), AmsiVerdict::Unavailable);
}

PRISM_TEST("amsi: Unavailable is NEVER silently Allow") {
  // The blueprint requires an alert when the pre-scan did not happen.
  PRISM_CHECK_EQ(prism::DecideAmsiAction(AmsiVerdict::Unavailable), SecurityAction::Warn);
  PRISM_CHECK(prism::DecideAmsiAction(AmsiVerdict::Unavailable) != SecurityAction::Allow);

  // Even a caller that configures Allow must be clamped back to Warn: this is a
  // guard, not a preference.
  prism::AmsiPolicy permissive;
  permissive.onUnavailable = SecurityAction::Allow;
  PRISM_CHECK_EQ(prism::DecideAmsiAction(AmsiVerdict::Unavailable, permissive),
                 SecurityAction::Warn);

  // Escalation is allowed (a stricter product policy may abort instead).
  prism::AmsiPolicy strict;
  strict.onUnavailable = SecurityAction::Block;
  PRISM_CHECK_EQ(prism::DecideAmsiAction(AmsiVerdict::Unavailable, strict),
                 SecurityAction::Block);

  PRISM_CHECK_EQ(prism::DecideAmsiAction(AmsiVerdict::Allow), SecurityAction::Allow);
  PRISM_CHECK_EQ(prism::DecideAmsiAction(AmsiVerdict::Block), SecurityAction::Block);
}

PRISM_TEST("amsi: target selection is extension-based, not has-a-dot") {
  PRISM_CHECK(prism::IsAmsiTarget("C:/out/setup.exe"));
  PRISM_CHECK(prism::IsAmsiTarget("C:/out/payload.EXE"));       // case-insensitive
  PRISM_CHECK(prism::IsAmsiTarget("C:\\out\\script.Ps1"));
  PRISM_CHECK(prism::IsAmsiTarget("/tmp/invoice.docm"));
  PRISM_CHECK(prism::IsAmsiTarget("run.bat"));
  PRISM_CHECK(prism::IsAmsiTarget("macro.XLSM"));

  // Not payloads: documents, media, archives, and extensions with no dot.
  PRISM_CHECK(!prism::IsAmsiTarget("photo.jpg"));
  PRISM_CHECK(!prism::IsAmsiTarget("notes.txt"));
  PRISM_CHECK(!prism::IsAmsiTarget("pack.7z"));
  PRISM_CHECK(!prism::IsAmsiTarget("pack.tar.gz"));
  PRISM_CHECK(!prism::IsAmsiTarget("C:/out.v2/setup"));         // dot only in a folder
  PRISM_CHECK(!prism::IsAmsiTarget("C:/out/no_extension"));
  PRISM_CHECK(!prism::IsAmsiTarget(""));
}

PRISM_TEST("amsi: chunk plan covers every byte and overlaps by exactly the overlap") {
  constexpr uint64_t kMiB = 1024ull * 1024ull;
  const auto plan = prism::PlanAmsiChunks(10 * kMiB, 4 * kMiB, 64 * 1024);
  PRISM_CHECK_EQ(plan.size(), size_t(3));
  if (plan.size() != 3) return;
  PRISM_CHECK_EQ(plan[0].offset, uint64_t(0));
  PRISM_CHECK_EQ(plan[0].length, uint64_t(4 * kMiB));
  PRISM_CHECK_EQ(plan[1].offset, uint64_t(4 * kMiB - 64 * 1024));
  PRISM_CHECK_EQ(plan[1].length, uint64_t(4 * kMiB));
  PRISM_CHECK_EQ(plan[2].offset, uint64_t(8 * kMiB - 128 * 1024));
  PRISM_CHECK_EQ(plan[2].offset + plan[2].length, uint64_t(10 * kMiB));

  // Structural properties, so the assertion is not just a re-listing of the
  // arithmetic: no gap, every chunk non-empty, and overlap never below the plan.
  for (size_t i = 1; i < plan.size(); ++i) {
    const uint64_t previousEnd = plan[i - 1].offset + plan[i - 1].length;
    PRISM_CHECK(plan[i].offset < previousEnd);                    // no gap
    PRISM_CHECK(previousEnd - plan[i].offset == 64 * 1024);       // exact overlap
  }

  const auto empty = prism::PlanAmsiChunks(0);
  PRISM_CHECK(empty.empty());
  const auto tiny = prism::PlanAmsiChunks(100, 4 * kMiB, 64 * 1024);
  PRISM_CHECK_EQ(tiny.size(), size_t(1));
  if (tiny.size() == 1) PRISM_CHECK_EQ(tiny[0].length, uint64_t(100));
}

// ---------------------------------------------------------------------------
// Mark-of-the-Web
// ---------------------------------------------------------------------------

PRISM_TEST("motw: decision table -- absent allows, present stamps, unreadable warns") {
  const prism::MotwPlan absent = prism::PlanMotwInheritance(MotwSource::Absent);
  PRISM_CHECK(!absent.stamp);
  PRISM_CHECK_EQ(absent.action, SecurityAction::Allow);

  const prism::MotwPlan present = prism::PlanMotwInheritance(MotwSource::Present);
  PRISM_CHECK(present.stamp);
  PRISM_CHECK_EQ(present.action, SecurityAction::Allow);

  // We could not read the stream: do not claim the archive had no mark.
  const prism::MotwPlan unreadable = prism::PlanMotwInheritance(MotwSource::Unreadable);
  PRISM_CHECK(!unreadable.stamp);
  PRISM_CHECK_EQ(unreadable.action, SecurityAction::Warn);

  // A failed stamp is not success: default warns, and Block is configurable.
  PRISM_CHECK_EQ(prism::MotwStampFailureAction(), SecurityAction::Warn);
  prism::MotwPolicy strict;
  strict.onStampFailure = SecurityAction::Block;
  PRISM_CHECK_EQ(prism::MotwStampFailureAction(strict), SecurityAction::Block);
}

PRISM_TEST("motw: verbatim Zone.Identifier keeps ZoneId=3 and both URLs") {
  const std::string source =
      "[ZoneTransfer]\r\n"
      "ZoneId=3\r\n"
      "HostUrl=https://example.com/download/Invoice.zip\r\n"
      "ReferrerUrl=https://example.com/inbox\r\n";
  const prism::MotwFields fields = prism::ParseMotwFields(source);
  PRISM_CHECK(fields.present);
  PRISM_CHECK_EQ(fields.zoneId, 3);
  PRISM_CHECK_EQ(fields.hostUrl, std::string("https://example.com/download/Invoice.zip"));
  PRISM_CHECK_EQ(fields.referrerUrl, std::string("https://example.com/inbox"));

  // The stream is cloned byte-for-byte; parsing must not be used to round-trip it
  // (a re-serialisation could drop unknown fields). This is the assertion that
  // the Windows test mirrors against the real ADS.
  PRISM_CHECK(prism::ParseMotwFields(source).zoneId == fields.zoneId);

  // Garbage is not a mark.
  const prism::MotwFields garbage = prism::ParseMotwFields("this is not a zone map\n");
  PRISM_CHECK(!garbage.present);
  PRISM_CHECK_EQ(garbage.zoneId, -1);

  // A malformed ZoneId is rejected rather than truncated or wrapped.
  const prism::MotwFields badZone = prism::ParseMotwFields("[ZoneTransfer]\nZoneId=12x\n");
  PRISM_CHECK_EQ(badZone.zoneId, -1);
  const prism::MotwFields overflow = prism::ParseMotwFields("ZoneId=99999999999999999999\n");
  PRISM_CHECK_EQ(overflow.zoneId, -1);

  // No = at all -> nothing parsed.
  PRISM_CHECK(!prism::ParseMotwFields("[ZoneTransfer]").present);
}

// ---------------------------------------------------------------------------
// Zip-bomb fuse
// ---------------------------------------------------------------------------

PRISM_TEST("bomb fuse: per-entry ratio catches a bomb the aggregate window cannot arm for") {
  constexpr uint64_t kMiB = 1024ull * 1024ull;
  prism::BombFuse fuse;  // defaults: 4 MiB window, 1000:1, 85% of free, 64 MiB floor

  // One 1 MiB entry that expands to 1 GiB (1024:1), fed with its packed size up
  // front (kpidPackSize), exactly how the extract callback sees it. It is fed in
  // 64 MiB produced-chunks, and the *packed* input never exceeds 1 MiB, so the
  // 4 MiB window detector is not even armed while the bomb runs.
  fuse.BeginFile(1 * kMiB);
  bool alive = true;
  for (int i = 0; i < 16 && alive; ++i) alive = fuse.Feed(64 * 1024, 64 * kMiB, 100ull << 30);
  // Snapshot the verdict state before the next BeginFile resets the per-entry
  // counters (a BeginFile after a trip is a no-op for Feed but does reset them).
  const bool windowArmedAtTrip = fuse.windowArmed();
  const uint64_t windowCompressedAtTrip = fuse.windowCompressed();
  const double fileRatioAtTrip = fuse.currentFileRatio();

  // Incompressible neighbours arrive after the bomb entry. With the per-entry
  // detector mutated away, this is where the aggregate window gets its chance --
  // and it misses: 4 MiB packed / ~1 GiB produced is ~257:1, below the limit.
  // (Feed is a no-op once tripped, which is why the clean build ignores it.)
  fuse.BeginFile(3 * kMiB);
  fuse.Feed(3 * kMiB, 3 * kMiB, 100ull << 30);

  PRISM_CHECK(!alive);
  PRISM_CHECK(fuse.tripped());
  PRISM_CHECK(!windowArmedAtTrip);                    // the window guard could not fire
  PRISM_CHECK(windowCompressedAtTrip < 4 * kMiB);
  PRISM_CHECK(fileRatioAtTrip > 1000.0);              // the entry really is a bomb
  PRISM_CHECK(!fuse.critical());                      // disk has room: not the dual case
  PRISM_CHECK(fuse.reason().find("single entry") != std::string::npos);
  std::cout << "    fuse reason: " << fuse.reason() << std::endl;
}

PRISM_TEST("bomb fuse: both blueprint conditions in one sample latch critical()") {
  constexpr uint64_t kMiB = 1024ull * 1024ull;
  prism::BombFuse fuse;
  // Free space 1 GiB -> the 85% line is 870 MiB; the entry produces 1 GiB at
  // 1024:1. Ratio AND disk proximity are both true in this single sample.
  fuse.BeginFile(1 * kMiB);
  const bool alive = fuse.Feed(64 * 1024, 1024 * kMiB, 1ull << 30);
  PRISM_CHECK(!alive);
  PRISM_CHECK(fuse.tripped());
  PRISM_CHECK(fuse.critical());
  PRISM_CHECK(fuse.reason().find("AND") != std::string::npos);
  std::cout << "    critical reason: " << fuse.reason() << std::endl;
}

PRISM_TEST("bomb fuse: one condition alone is not the blueprint's critical case") {
  constexpr uint64_t kMiB = 1024ull * 1024ull;
  // Ratio only, plenty of disk.
  prism::BombFuse ratioOnly;
  ratioOnly.BeginFile(1 * kMiB);
  for (int i = 0; i < 16 && !ratioOnly.tripped(); ++i) {
    ratioOnly.Feed(64 * 1024, 64 * kMiB, 100ull << 30);
  }
  PRISM_CHECK(ratioOnly.tripped());
  PRISM_CHECK(!ratioOnly.critical());

  // Disk only, benign 1:1 data.
  prism::BombFuse diskOnly;
  for (int i = 0; i < 20 && !diskOnly.tripped(); ++i) {
    diskOnly.Feed(1 * kMiB, 1 * kMiB, 16 * kMiB);
  }
  PRISM_CHECK(diskOnly.tripped());
  PRISM_CHECK(!diskOnly.critical());
  PRISM_CHECK(diskOnly.reason().find("free space") != std::string::npos);
}

PRISM_TEST("bomb fuse: extraction-start free space is latched, not re-read per sample") {
  constexpr uint64_t kMiB = 1024ull * 1024ull;
  prism::BombFuse fuse;
  fuse.BeginFile(1 * kMiB);
  // First sample latches 1 GiB free (85% = 870 MiB).
  PRISM_CHECK(fuse.Feed(0, 1 * kMiB, 1ull << 30));
  PRISM_CHECK_EQ(fuse.extractionStartFreeBytes(), uint64_t(1ull << 30));
  // The caller now passes the *current* free space, which has dropped to 10 MiB.
  // If the threshold chased it, 100 MiB of output would already trip; latched it
  // must not.
  PRISM_CHECK(fuse.Feed(0, 99 * kMiB, 10 * kMiB));
  PRISM_CHECK(!fuse.tripped());
  // Cross the latched 870 MiB line: trips as disk proximity, no ratio (1000:1).
  PRISM_CHECK(!fuse.Feed(0, 780 * kMiB, 10 * kMiB));
  PRISM_CHECK(fuse.tripped());
  PRISM_CHECK(!fuse.critical());
}

}  // namespace

PRISM_TEST_MAIN()
