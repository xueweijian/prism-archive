// PrismArchive -- Windows integration test (cards C50 + C114)
// ONE LEASE == ONE HANDLER. Two threads must never hold the same ArchiveReader.
//
// WHY THIS FILE EXISTS
//   win.reader_pool.four_concurrent_extractions_each_get_their_own_bytes has
//   failed 4 times on CI across different PRs. C41 added a deterministic guard
//   inside ArchiveReader (a second concurrent operation on one handler is
//   REFUSED and counted), which turned a silent data-loss shape into a clean
//   rejection -- and made the counter operationConflicts() a signal: if the
//   pool really hands one handler to two threads, that counter must rise.
//
//   So this suite attacks the LEASE itself, from many threads at once, which is
//   the one thing the existing suite never did (it acquires all leases from the
//   main thread, sequentially):
//
//     1. forty_rounds_four_threads_never_share_a_handler: 40 rounds x 4
//        threads x 4 acquire/release cycles. Immediately after every Acquire
//        the worker claims a per-slot atomic brain flag; a flag that is already
//        taken is a DUPLICATE LEASE (the bug this card is about), reported with
//        the pool state. Two of every four cycles also run a REAL engine
//        operation on the leased handler (D2: assert the bytes and their
//        SHA-256, never just "it returned something") and then assert
//        operationConflicts() == 0 on that handler, so a shared lease is proven
//        by the engine's own counter, not only by our bookkeeping.
//     2. blocked_waiters_are_served_one_at_a_time: capacity 1, three threads
//        blocking in Acquire(kWaitForever). This is the Release()/Acquire()
//        window the card names as hypothesis 2: a slot flag overwritten between
//        the returning lease and the waiting thread must let two waiters in,
//        and the brain flag catches it.
//
//   Zero operationConflicts() is a CONTRACT on a clean run, not a diagnostic:
//   every round asserts it, so a lease regression fails here first.
//
// C114 -- WHY THOSE TWO ASSERTIONS WERE REPLACED (verdict: the TEST was wrong,
// not the pool)
//   This file used to end every round of case 1 with
//       PRISM_CHECK_EQ(st.distinctSlotsUsed, kWorkers);
//   and case 2 with
//       PRISM_CHECK(st.waits > 0);
//   Both went red on CI under concurrent load (twice: "expected
//   (st.distinctSlotsUsed) == (kWorkers) got '3' vs '4'") and green locally.
//   Reproduced on the unmodified code -- 10 runs under CPU contention, 10 red,
//   including the exact '3' vs '4' and a "expected true: st.waits > 0" the
//   report never mentioned.
//
//   Neither number is a property of ReaderPool. Read the implementation
//   (Src/PrismWin/ReaderPool.hpp):
//     * Pick() returns the LOWEST free slot. distinctSlotsUsed counts slots that
//       have EVER been handed out (State::used, never cleared, State::RefreshDerived).
//       It reaches capacity only if the OS scheduler happens to keep all
//       kWorkers leases alive at one instant -- a thread that is late by a
//       single timeslice never touches the highest slot and the counter reads 3
//       forever. That is the CI number.
//     * stats().waits counts requests that FOUND EVERY SLOT BUSY. Three threads
//       on a capacity-1 pool can be scheduled so strictly one at a time that no
//       request ever finds the slot busy; the counter is then 0 even though the
//       pool is perfect and every lease was exclusive.
//   The pool's contract is "at most capacity concurrent leases, every lease is
//   its own handler". It never promised "the scheduler will overlap kWorkers
//   threads", and no implementation of a first-free picker can promise it.
//   So the assertion was measuring the scheduler, not the pool. What replaced
//   them FORCES the state it asserts on:
//     * capacity_is_a_hard_ceiling_and_slots_are_exclusive holds
//       kWorkers-1 leases across a rendezvous, asserts distinctSlotsUsed ==
//       kWorkers-1 right there (the retired assertion's "failure" is a legal,
//       byte-verified state), then adds the kWorkers-th lease, asserts
//       inUse == capacity and kWorkers DISTINCT handler pointers, and proves the
//       next request is refused: Acquire(0) returns nothing (timeout counted)
//       and a blocking Acquire(500ms) gets nothing while every slot is held.
//       Finally it proves a release WAKES the blocked request and hands it the
//       freed slot (recycling notifies).
//     * blocked_waiters_are_served_one_at_a_time now holds the only slot itself
//       and waits until stats().waits proves all three waiters reached the pool
//       and were refused, instead of hoping three threads trip over each other.
//   Nothing here sleeps its way to green: every wait is a bounded rendezvous
//   (R3/D29), and every assertion is on a stale-proof count or pointer identity.
//
// Negative controls (CONTRACTS D3 + D27):
//   * PRISM_C50_MUTANT_SHARED_LEASE removes the lease mutual exclusion in
//     ReaderPool::Acquire (no slot is marked busy, everybody is handed slot 0).
//     That build must go RED and print "C50 MUTATION CAUGHT: shared_lease".
//   * PRISM_C114_MUTANT_OVERSELL removes the CEILING: a request that finds every
//     slot busy is handed slot 0 instead of blocking, so inUse climbs past
//     capacity. That build must go RED and print
//     "C114 MUTATION CAUGHT: oversell". The marker is gated on the D3 predicate
//     "a lease was observed that took inUse past capacity", so a run that goes
//     red for an unrelated reason can never print it. The marker is the CTest
//     verdict (D26: no WILL_FAIL stacked on a regex).
//
// No silent skip: a missing 7z.dll/7z.exe is a FAIL with a loud message (D1).
#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "PrismEngineBridge.hpp"
#include "ReaderPool.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
namespace win = prism::win;
using prism::ArchiveItem;

namespace {

constexpr size_t kItemBytes = 1u * 1024u * 1024u;  // per payload: big enough that
                                                   // two engine calls really overlap
constexpr size_t kWorkers = 4;
constexpr size_t kRounds = 40;        // the card's 40-round budget
constexpr size_t kCyclesPerRound = 4; // acquire/release cycles per worker per round
constexpr uint32_t kSeedBase = 0xC50u;

// Every rendezvous below is bounded (R3/D29): a hang in CI is worse than a red.
constexpr std::chrono::milliseconds kRendezvousBudget = std::chrono::milliseconds(60000);
constexpr std::chrono::milliseconds kCounterBudget = std::chrono::milliseconds(30000);
// How long the (capacity+1)-th request is allowed to sit inside Acquire before
// we insist it came back empty-handed. It cannot be served: every slot is held
// by a thread that is parked on a permit we own.
constexpr std::chrono::milliseconds kRefusedBudget = std::chrono::milliseconds(500);

// The D27 predicate's input: the freshest operationConflicts() total we have
// observed. Set BEFORE every assertion that could report, so ReportFailure can
// decide "the injected shared lease really fired" instead of guessing.
std::atomic<uint64_t> g_conflictsSeen{0};

// The C114 predicate: set the moment a lease is observed that took inUse PAST
// capacity -- the only state a correct pool can never produce, and the exact
// state the PRISM_C114_MUTANT_OVERSELL injection creates. D3: a red for any
// other reason leaves this false and must not print CAUGHT.
std::atomic<bool> g_ceilingViolated{false};

std::string Sha256Hex(const std::vector<uint8_t>& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  BCRYPT_HASH_HANDLE hash = nullptr;
  std::string hex;
  if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
    unsigned char digest[32] = {};
    if (BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) >=
            0 &&
        BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0) {
      static const char* kHex = "0123456789abcdef";
      hex.reserve(64);
      for (const unsigned char byte : digest) {
        hex.push_back(kHex[byte >> 4]);
        hex.push_back(kHex[byte & 0x0F]);
      }
    }
    BCryptDestroyHash(hash);
  }
  BCryptCloseAlgorithmProvider(alg, 0);
  return hex;
}

std::vector<uint8_t> MakePayload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

// Keeps the D27 predicate in step with the pool. Cheap (one atomic load per
// slot); called before any check that could fail, so a CAUGHT verdict is only
// ever printed when the counter really is non-zero.
uint64_t SyncConflicts(const win::ReaderPool& pool) {
  const uint64_t total = pool.operationConflicts();
  g_conflictsSeen.store(total);
  return total;
}

// C114: the ceiling observation. Called after EVERY Acquire in this file, before
// any assertion that acquire feeds, so a CAUGHT verdict can only be printed on a
// run where the pool really did hand out more leases than it has handlers.
void NoteCeiling(const win::ReaderPool& pool) {
  const win::ReaderPool::Stats s = pool.stats();
  if (s.inUse > s.capacity) g_ceilingViolated.store(true);
}

// One compact, greppable block. Every failure path prints this, so the next CI
// red says WHICH handler conflicted instead of only "ok[i] was false".
void DumpPool(const win::ReaderPool& pool, const std::string& why) {
  const win::ReaderPool::Stats s = pool.stats();
  const std::vector<uint64_t> bySlot = pool.operationConflictsBySlot();
  if (s.inUse > s.capacity) g_ceilingViolated.store(true);
  uint64_t total = 0;
  std::string slots;
  for (size_t i = 0; i < bySlot.size(); ++i) {
    total += bySlot[i];
    if (i != 0) slots += ",";
    slots += std::to_string(bySlot[i]);
  }
  g_conflictsSeen.store(total);
  // One line, assembled first and written under a lock: four worker threads can
  // fail at once, and an interleaved diagnostic is the very thing this card is
  // removing.
  std::ostringstream line;
  line << "    POOL-LEASE [" << why << "] capacity=" << s.capacity
       << " handlers=" << s.handlers << " acquired=" << s.acquired
       << " returned=" << s.returned << " waits=" << s.waits
       << " timeouts=" << s.timeouts << " inUse=" << s.inUse
       << " maxConcurrent=" << s.maxConcurrent
       << " distinctSlotsUsed=" << s.distinctSlotsUsed
       << " operationConflicts=" << total << " bySlot=[" << slots << "]";
  static std::mutex dumpMutex;
  std::lock_guard<std::mutex> guard(dumpMutex);
  std::cout << line.str() << std::endl;
  std::cout.flush();
}

// C114: a rendezvous, not a sleep. This is the difference between "the pool let
// four threads hold four handlers" (a fact the test creates) and "the scheduler
// happened to run four threads at once" (what used to be asserted).
class Gate {
 public:
  explicit Gate(size_t parties) : parties_(parties) {}

  bool ArriveAndWait(std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    ++arrived_;
    if (arrived_ >= parties_) {
      open_ = true;
      cv_.notify_all();
      return true;
    }
    return cv_.wait_for(lock, budget, [this] { return open_; });
  }

  bool WaitForArrivals(size_t want, std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, budget, [this, want] { return arrived_ >= want; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  size_t parties_;
  size_t arrived_ = 0;
  bool open_ = false;
};

// One-shot "you may release the lease now" permit, bounded.
class Permit {
 public:
  void Open() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    cv_.notify_all();
  }
  bool Wait(std::chrono::milliseconds budget) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, budget, [this] { return open_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool open_ = false;
};

// Bounded wait for a POOL COUNTER to reach a state (e.g. "all three waiters are
// inside Acquire and were refused"). The counter, not the clock, is the
// assertion; the deadline only keeps a broken pool from hanging the lane.
template <class Pred>
bool WaitForPool(win::ReaderPool& pool, Pred pred, std::chrono::milliseconds budget) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred(pool)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return pred(pool);
}

struct Fixture {
  // Declared FIRST so it is destroyed LAST: every reader/handle is closed
  // before the tree is removed (C36: unique per process AND per allocation).
  std::unique_ptr<prism::testtmp::UniqueDir> scratch;
  std::wstring dllPath;
  std::wstring exePath;
  std::wstring dir;
  std::wstring archivePath;
  win::SevenZipLibrary library;
  std::vector<ArchiveItem> catalog;
  std::vector<std::string> names;
  std::vector<std::string> expected;
  std::vector<size_t> catalogIndex;
  bool ready = false;
  std::string why;
};

Fixture& World() {
  static Fixture fixture;
  return fixture;
}

bool SetupFixture() {
  Fixture& fx = World();
  if (fx.ready) return true;
  if (!fx.why.empty()) {
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.dllPath = prism::fixture::DllPath();
  fx.exePath = prism::fixture::ExePath();
  if (fx.dllPath.empty()) {
    fx.why = "PRISM_7Z_DLL is not set (windows-latest ships 7-Zip)";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  if (fx.exePath.empty()) {
    fx.why = prism::fixture::ExePathWhy();
    if (fx.why.empty()) fx.why = "7z.exe not found beside 7z.dll (needed to author the fixture)";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  // Fixture directory policy from C36: unique per process AND per allocation.
  fx.scratch = std::make_unique<prism::testtmp::UniqueDir>(L"c36", L"pool_lease");
  if (!fx.scratch->ok()) {
    fx.why = "cannot create the fixture directory: " + fx.scratch->error();
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  const fs::path& work = fx.scratch->path();
  fx.dir = work.wstring();
  fx.archivePath = (work / "pool_lease.7z").wstring();

  fx.names = {"a.bin", "b.bin", "c.bin", "d.bin"};
  for (size_t i = 0; i < fx.names.size(); ++i) {
    const std::vector<uint8_t> payload = MakePayload(kSeedBase + static_cast<uint32_t>(i), kItemBytes);
    std::ofstream file(work / fx.names[i], std::ios::binary);
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    if (!file.good()) {
      fx.why = "cannot write fixture payload " + fx.names[i];
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
    fx.expected.push_back(Sha256Hex(payload));
  }
  std::vector<std::wstring> wideNames;
  for (const std::string& name : fx.names) wideNames.push_back(std::wstring(name.begin(), name.end()));
  if (!prism::fixture::PackSolid(fx.exePath, fx.dir, L"pool_lease.7z", wideNames)) {
    fx.why = "7z.exe could not author the solid fixture archive";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  if (!fx.library.Load(fx.dllPath)) {
    fx.why = "LoadLibrary(7z.dll) failed";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  win::ArchiveReader probe(fx.library);
  if (!probe.Open(fx.archivePath)) {
    fx.why = "probe reader could not open the fixture: " + probe.openTrace();
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.catalog = probe.ReadCatalog();
  if (fx.catalog.size() != fx.names.size()) {
    fx.why = "fixture catalog size mismatch";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.catalogIndex.assign(fx.names.size(), static_cast<size_t>(-1));
  for (size_t i = 0; i < fx.names.size(); ++i) {
    for (const ArchiveItem& item : fx.catalog) {
      if (item.path == fx.names[i]) {
        fx.catalogIndex[i] = item.index;
        break;
      }
    }
    if (fx.catalogIndex[i] == static_cast<size_t>(-1)) {
      fx.why = "catalog entry missing for " + fx.names[i];
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
  }
  fx.ready = true;
  return true;
}

std::shared_ptr<win::ReaderPool> MakePool(size_t capacity) {
  win::ReaderPool::Config config;
  config.archivePath = World().archivePath;
  config.capacity = capacity;
  std::string error;
  auto pool = win::ReaderPool::Create(World().library, config, &error);
  if (!pool) std::cout << "    ReaderPool::Create failed: " << error << std::endl;
  return pool;
}

// One real engine operation on a leased handler. The bytes and their SHA-256
// are the evidence (D2); operationConflicts() is the C41 signal that two
// threads were on this handler at once.
void RunEngineOp(win::ReaderPool& pool, win::ArchiveReader* reader, size_t payloadIndex,
                 const char* tag) {
  if (!PRISM_CHECK(reader != nullptr)) return;
  const auto bytes = reader->ExtractToMemory(
      static_cast<uint32_t>(World().catalogIndex[payloadIndex]));
  SyncConflicts(pool);
  if (!bytes.has_value()) DumpPool(pool, std::string(tag) + ": engine call returned nothing");
  if (!PRISM_CHECK(bytes.has_value())) return;
  PRISM_CHECK_EQ(bytes->size(), kItemBytes);
  if (bytes->size() == kItemBytes) PRISM_CHECK_EQ(Sha256Hex(*bytes), World().expected[payloadIndex]);
  SyncConflicts(pool);
  if (reader->operationConflicts() != 0) DumpPool(pool, std::string(tag) + ": handler conflict");
  PRISM_CHECK_EQ(reader->operationConflicts(), uint64_t(0));
}

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::cout << "    FAIL: CoInitializeEx" << std::endl;
    return 1;
  }
#if defined(PRISM_C50_MUTANT_SHARED_LEASE)
  std::cout << "C50 MUTANT ACTIVE: ReaderPool lease mutual exclusion removed" << std::endl;
  // D27 + D3: the marker is printed at the FIRST failure and only while the
  // predicate says the injected defect really fired. Exit 0 is the harness's
  // (CTest decides with PASS_REGULAR_EXPRESSION, D26).
  prism::test::SetMutationVerdict("C50 MUTATION CAUGHT: shared_lease",
                                  [] { return g_conflictsSeen.load() > 0; });
#endif
#if defined(PRISM_C114_MUTANT_OVERSELL)
  std::cout << "C114 MUTANT ACTIVE: ReaderPool::Acquire no longer refuses a request while every "
               "slot is busy"
            << std::endl;
  // D27 + D3: only a lease that took inUse PAST capacity proves this injection
  // fired. A run that goes red without ever observing that stays red WITHOUT
  // the marker (CTest then fails on the missing regex -- that is the point).
  prism::test::SetMutationVerdict("C114 MUTATION CAUGHT: oversell",
                                  [] { return g_ceilingViolated.load(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL POOL-LEASE CHECKS PASSED" << std::endl;
  return 0;
}

// THE CARD, as an executable contract: kWorkers-1 threads are INSIDE their
// leases at the same time (they park on a rendezvous), so the pool state below
// is created by the test instead of sampled from a race. Then:
//   * distinctSlotsUsed is exactly kWorkers-1 -- the number the retired
//     assertion called a failure, produced while every lease is exclusive and
//     every extraction returns the right bytes;
//   * the kWorkers-th lease lands on its own handler: capacity really is
//     usable, not just declared;
//   * the (kWorkers+1)-th request is REFUSED twice: non-blocking Acquire(0)
//     returns nothing (and is counted as a timeout), and a blocking
//     Acquire(500ms) comes back empty because every slot is held;
//   * releasing one slot WAKES that blocked request and hands it exactly the
//     freed handler (a pool that recycles without notifying hangs here).
PRISM_TEST("pool_lease.capacity_is_a_hard_ceiling_and_slots_are_exclusive") {
  if (!PRISM_CHECK(SetupFixture())) return;
  auto pool = MakePool(kWorkers);
  if (!PRISM_CHECK(pool != nullptr)) return;
  if (!PRISM_CHECK_EQ(pool->capacity(), kWorkers)) return;
  // Create() promised one independent handler per slot; a pool that opened
  // fewer would hand one handler to two threads (the C50 bug).
  if (!PRISM_CHECK_EQ(pool->stats().handlers, kWorkers)) return;

  constexpr size_t kHeld = kWorkers - 1;  // the "only 3 of 4" CI state, on purpose
  struct Holder {
    bool got = false;
    size_t slot = static_cast<size_t>(-1);
    win::ArchiveReader* reader = nullptr;
  };
  Holder holders[kHeld];
  std::mutex holdersMutex;
  Gate allHeld(kHeld);
  Permit letGo[kHeld];

  std::vector<std::thread> threads;
  for (size_t w = 0; w < kHeld; ++w) {
    threads.emplace_back([&, w] {
      auto lease = pool->Acquire(win::kWaitForever);
      NoteCeiling(*pool);
      {
        std::lock_guard<std::mutex> lock(holdersMutex);
        holders[w].got = lease.has_value();
        if (lease.has_value()) {
          holders[w].slot = lease->slot();
          holders[w].reader = lease->get();
        }
      }
      // A real extraction while the OTHER holders are also inside their leases:
      // exclusivity is proven by the bytes (D2), not by the pool's bookkeeping.
      if (lease.has_value()) RunEngineOp(*pool, lease->get(), w % kWorkers, "held engine op");
      // Even a failed Acquire arrives here, so main never waits out the whole
      // budget on a thread that bailed out early.
      if (!allHeld.ArriveAndWait(kRendezvousBudget)) PRISM_CHECK(false);
      if (!lease.has_value()) return;
      if (!letGo[w].Wait(kRendezvousBudget)) PRISM_CHECK(false);
      lease->Release();
    });
  }

  const bool allArrived = allHeld.WaitForArrivals(kHeld, kRendezvousBudget);
  if (!allArrived) DumpPool(*pool, "holders never rendezvoused");
  if (!PRISM_CHECK(allArrived)) {
    for (auto& permit : letGo) permit.Open();
    for (std::thread& thread : threads) thread.join();
    return;
  }

  // --- the state the retired assertion called a failure ---------------------
  const win::ReaderPool::Stats three = pool->stats();
  if (three.distinctSlotsUsed != kHeld || three.inUse != kHeld) {
    DumpPool(*pool, "three simultaneous leases");
  }
  std::cout << "    C114 EVIDENCE: " << three.inUse << " simultaneous leases on a capacity-"
            << three.capacity << " pool -> distinctSlotsUsed=" << three.distinctSlotsUsed
            << " (this is a legal, exclusive, byte-verified state)" << std::endl;
  PRISM_CHECK_EQ(three.distinctSlotsUsed, kHeld);
  PRISM_CHECK_EQ(three.inUse, kHeld);
  PRISM_CHECK_EQ(three.maxConcurrent, kHeld);
  {
    std::lock_guard<std::mutex> lock(holdersMutex);
    for (size_t i = 0; i < kHeld; ++i) {
      if (!PRISM_CHECK(holders[i].got)) continue;
      for (size_t j = i + 1; j < kHeld; ++j) {
        if (!holders[j].got) continue;
        // Pointer identity IS the C50 contract: one lease, one handler.
        PRISM_CHECK(holders[i].reader != holders[j].reader);
        PRISM_CHECK(holders[i].slot != holders[j].slot);
      }
    }
  }

  // --- capacity is real: the last slot is usable ----------------------------
  auto extra = pool->Acquire(std::chrono::milliseconds(0));
  NoteCeiling(*pool);
  if (!PRISM_CHECK(extra.has_value())) {
    DumpPool(*pool, "the last free slot was refused");
    for (auto& permit : letGo) permit.Open();
    for (std::thread& thread : threads) thread.join();
    return;
  }
  PRISM_CHECK_EQ(pool->inUse(), kWorkers);
  PRISM_CHECK_EQ(pool->stats().distinctSlotsUsed, kWorkers);
  PRISM_CHECK_EQ(pool->stats().maxConcurrent, kWorkers);
  {
    std::lock_guard<std::mutex> lock(holdersMutex);
    for (const Holder& holder : holders) {
      if (holder.got) PRISM_CHECK(holder.reader != extra->get());
    }
    PRISM_CHECK(extra->slot() < kWorkers);
  }

  // --- the ceiling: request kWorkers+1 must NOT be served -------------------
  const uint64_t timeoutsBefore = pool->stats().timeouts;
  auto refused = pool->Acquire(std::chrono::milliseconds(0));
  NoteCeiling(*pool);
  const bool oversold = refused.has_value();
  if (oversold) {
    // Only reachable in the mutation build (or a real regression): the pool
    // handed out a lease while every slot was held. Release it so the rest of
    // this case can still run and report the whole picture.
    refused->Release();
    DumpPool(*pool, "a full pool granted a non-blocking lease");
  }
  PRISM_CHECK(!oversold);
  PRISM_CHECK_EQ(pool->stats().timeouts, timeoutsBefore + 1);

  // Blocking path: it cannot be served either. The counter proves it reached
  // the pool and found every slot busy; the lease must NOT arrive.
  const uint64_t waitsBefore = pool->stats().waits;
  const uint64_t timeoutsBeforeWait = pool->stats().timeouts;
  std::atomic<bool> blockedWaiterGranted{false};
  std::thread blockedWaiter([&] {
    auto lease = pool->Acquire(kRefusedBudget);
    NoteCeiling(*pool);
    if (lease.has_value()) {
      blockedWaiterGranted.store(true);
      lease->Release();
    }
  });
  const bool waiterQueued = WaitForPool(
      *pool, [waitsBefore](win::ReaderPool& p) { return p.stats().waits >= waitsBefore + 1; },
      kCounterBudget);
  if (!waiterQueued) DumpPool(*pool, "the rejected request never reached the wait path");
  PRISM_CHECK(waiterQueued);
  PRISM_CHECK_EQ(pool->inUse(), kWorkers);
  blockedWaiter.join();
  PRISM_CHECK(!blockedWaiterGranted.load());
  PRISM_CHECK_EQ(pool->stats().timeouts, timeoutsBeforeWait + 1);
  PRISM_CHECK_EQ(pool->inUse(), kWorkers);

  // --- recycling notifies: the blocked request gets the freed slot ----------
  const size_t freedSlot = extra->slot();
  win::ArchiveReader* freedReader = extra->get();
  std::atomic<bool> woken{false};
  std::atomic<size_t> wokenSlot{static_cast<size_t>(-1)};
  std::atomic<win::ArchiveReader*> wokenReader{nullptr};
  const uint64_t waitsBeforeWake = pool->stats().waits;
  std::thread waker([&] {
    auto lease = pool->Acquire(win::kWaitForever);
    NoteCeiling(*pool);
    if (!lease.has_value()) {
      PRISM_CHECK(lease.has_value());
      return;
    }
    wokenSlot.store(lease->slot());
    wokenReader.store(lease->get());
    woken.store(true);
    lease->Release();
  });
  const bool wakerQueued = WaitForPool(
      *pool, [waitsBeforeWake](win::ReaderPool& p) { return p.stats().waits >= waitsBeforeWake + 1; },
      kCounterBudget);
  if (!wakerQueued) DumpPool(*pool, "the wake-up request never blocked");
  PRISM_CHECK(wakerQueued);
  PRISM_CHECK_EQ(pool->inUse(), kWorkers);
  extra->Release();  // no notify at all here would hang the waker forever
  extra.reset();
  const bool wokenUp = WaitForPool(*pool, [&woken](win::ReaderPool&) { return woken.load(); },
                                   kCounterBudget);
  if (!wokenUp) DumpPool(*pool, "release did not wake the blocked request");
  PRISM_CHECK(wokenUp);
  PRISM_CHECK_EQ(wokenSlot.load(), freedSlot);
  PRISM_CHECK(wokenReader.load() == freedReader);
  waker.join();

  // --- teardown: every lease returned, every counter consistent -------------
  for (auto& permit : letGo) permit.Open();
  for (std::thread& thread : threads) thread.join();
  NoteCeiling(*pool);
  const win::ReaderPool::Stats final = pool->stats();
  if (final.inUse != 0 || final.acquired != final.returned) DumpPool(*pool, "teardown");
  PRISM_CHECK_EQ(final.inUse, size_t(0));
  PRISM_CHECK_EQ(final.acquired, final.returned);
  PRISM_CHECK_EQ(final.distinctSlotsUsed, kWorkers);
  SyncConflicts(*pool);
  PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
}

// Hypothesis 2 from the card, made executable -- and, since C114, made
// DETERMINISTIC: the main thread holds the pool's only slot, so a waiter that
// increments stats().waits provably found it busy and was refused. Waiting for
// that counter (bounded) replaces the old hope that three racing threads would
// happen to collide: under CPU load they ran strictly one at a time and the
// retired assertion "st.waits > 0" failed on a perfectly healthy pool.
PRISM_TEST("pool_lease.blocked_waiters_are_served_one_at_a_time") {
  if (!PRISM_CHECK(SetupFixture())) return;
  auto pool = MakePool(1);
  if (!PRISM_CHECK(pool != nullptr)) return;
  if (!PRISM_CHECK_EQ(pool->capacity(), size_t(1))) return;

  constexpr size_t kWaiters = 3;
  constexpr size_t kCycles = 12;
  std::atomic<bool> held{false};
  std::atomic<size_t> duplicateLeases{0};

  for (size_t cycle = 0; cycle < kCycles; ++cycle) {
    // The main thread IS the holder: capacity 1, so every waiter below must be
    // refused until we let go. Nothing about that depends on the scheduler.
    auto owner = pool->Acquire(std::chrono::milliseconds(0));
    NoteCeiling(*pool);
    if (!PRISM_CHECK(owner.has_value())) {
      DumpPool(*pool, "the single slot was not free at cycle start");
      return;
    }
    // NOTE: 'held' tracks the WAITERS only. The main thread's own lease is not a
    // duplicate when a waiter later inherits the slot.
    const uint64_t waitsBefore = pool->stats().waits;

    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (size_t w = 0; w < kWaiters; ++w) {
      waiters.emplace_back([&, w] {
        auto lease = pool->Acquire(win::kWaitForever);
        NoteCeiling(*pool);
        if (!PRISM_CHECK(lease.has_value())) {
          DumpPool(*pool, "waiter got no lease");
          return;
        }
        PRISM_CHECK_EQ(lease->slot(), size_t(0));
        const bool alreadyHeld = held.exchange(true);
        if (alreadyHeld) {
          duplicateLeases.fetch_add(1);
          std::cout << "    DUPLICATE WAITER cycle=" << cycle << " waiter=" << w << std::endl;
          DumpPool(*pool, "duplicate waiter");
        }
        PRISM_CHECK(!alreadyHeld);
        if (cycle % 4 == 0) RunEngineOp(*pool, lease->get(), w % kWorkers, "waiter engine op");
        held.store(false);
        lease->Release();
      });
    }

    // Proof that all three are queued AND refused: stats().waits only rises for
    // a request that found every slot busy, and we are still holding the slot,
    // so each of the three must have reached Acquire and been turned away.
    const bool allBlocked = WaitForPool(
        *pool, [waitsBefore](win::ReaderPool& p) { return p.stats().waits >= waitsBefore + kWaiters; },
        kCounterBudget);
    if (!allBlocked) DumpPool(*pool, "waiters were not all queued behind the held slot");
    PRISM_CHECK(allBlocked);
    PRISM_CHECK_EQ(pool->inUse(), size_t(1));

    owner->Release();
    owner.reset();
    for (std::thread& waiter : waiters) waiter.join();
    SyncConflicts(*pool);
    PRISM_CHECK_EQ(pool->inUse(), size_t(0));
    PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
  }

  PRISM_CHECK_EQ(duplicateLeases.load(), size_t(0));
  const win::ReaderPool::Stats st = pool->stats();
  PRISM_CHECK_EQ(st.acquired, st.returned);
  // Deterministic, not "> 0": this EXACT value is guaranteed by the rendezvous,
  // never by probability. In every cycle the main thread holds the pool's only
  // slot and does not release it until stats().waits has risen by exactly
  // kWaiters -- and waits only rises for a request that found EVERY slot busy.
  // So each of the kWaiters must reach Acquire() while the slot is taken and be
  // refused there; no waiter can slip through, because the slot is never free
  // while the counter is being observed. Hence waits == kCycles * kWaiters on
  // every schedule: a pool that stopped refusing scores lower, a pool that
  // double-counted scores higher.
  PRISM_CHECK_EQ(st.waits, uint64_t(kCycles * kWaiters));
  SyncConflicts(*pool);
  PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
}

// THE CARD, as an executable contract: 40 rounds x 4 threads, every Acquire
// followed by "no other thread is on this handler" and, on half the cycles, by
// a real extraction whose bytes decide. Nothing here trusts the pool's own
// counters for the ownership claim -- the brain flag is per slot and only the
// thread holding that lease can set it.
PRISM_TEST("pool_lease.forty_rounds_four_threads_never_share_a_handler") {
  if (!PRISM_CHECK(SetupFixture())) return;
  auto pool = MakePool(kWorkers);
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->capacity(), kWorkers);

  std::atomic<bool> held[kWorkers];
  for (auto& flag : held) flag.store(false);
  std::atomic<size_t> duplicateLeases{0};
  std::atomic<size_t> engineOps{0};

  for (size_t round = 0; round < kRounds; ++round) {
    std::vector<std::thread> workers;
    workers.reserve(kWorkers);
    for (size_t w = 0; w < kWorkers; ++w) {
      workers.emplace_back([&, w] {
        for (size_t cycle = 0; cycle < kCyclesPerRound; ++cycle) {
          auto lease = pool->Acquire(win::kWaitForever);
          NoteCeiling(*pool);
          if (!lease.has_value()) {
            DumpPool(*pool, "Acquire(kWaitForever) returned nothing");
            PRISM_CHECK(lease.has_value());
            return;
          }
          const size_t slot = lease->slot();
          if (!PRISM_CHECK(slot < kWorkers)) {
            lease->Release();
            return;
          }
          // THE ASSERTION THE CARD ASKS FOR: claim this handler for this
          // thread. A flag that was already taken means the pool handed one
          // ArchiveReader to two leases.
          const bool alreadyHeld = held[slot].exchange(true);
          if (alreadyHeld) {
            duplicateLeases.fetch_add(1);
            std::cout << "    DUPLICATE LEASE round=" << round << " worker=" << w
                      << " slot=" << slot << " cycle=" << cycle << std::endl;
            DumpPool(*pool, "duplicate lease");
          }
          PRISM_CHECK(!alreadyHeld);
          if (((round + cycle) % 2) == 0) {
            RunEngineOp(*pool, lease->get(), w, "round engine op");
            engineOps.fetch_add(1);
          }
          held[slot].store(false);
          lease->Release();
        }
      });
    }
    for (std::thread& worker : workers) worker.join();

    // Round invariants: the pool is fully returned, nobody conflicted, and the
    // statistics agree with the leases that really existed.
    SyncConflicts(*pool);
    if (pool->inUse() != 0 || pool->operationConflicts() != 0) {
      DumpPool(*pool, "round invariants");
    }
    PRISM_CHECK_EQ(pool->inUse(), size_t(0));
    PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
    const win::ReaderPool::Stats st = pool->stats();
    PRISM_CHECK_EQ(st.acquired, st.returned);
    PRISM_CHECK(st.maxConcurrent <= kWorkers);
    PRISM_CHECK(st.maxConcurrent >= 1);
    // C114: this used to be PRISM_CHECK_EQ(st.distinctSlotsUsed, kWorkers), which
    // is a claim about the OS scheduler, not about the pool: distinctSlotsUsed
    // counts slots EVER handed out, and Pick() always takes the lowest free one,
    // so a worker that is late by one timeslice leaves the highest slot untouched
    // (CI: got '3' vs '4'). The claim that every slot is usable is proven
    // deterministically, with all of them held at once, in
    // pool_lease.capacity_is_a_hard_ceiling_and_slots_are_exclusive.
    PRISM_CHECK(st.distinctSlotsUsed <= kWorkers);
    PRISM_CHECK_EQ(st.handlers, kWorkers);
    if (st.distinctSlotsUsed > kWorkers || st.maxConcurrent > kWorkers) {
      DumpPool(*pool, "round stats");
    }
  }

  PRISM_CHECK_EQ(duplicateLeases.load(), size_t(0));
  PRISM_CHECK(engineOps.load() > 0);
  // The counter is the observable contract C50 adds: zero under normal load.
  SyncConflicts(*pool);
  PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
  const std::vector<uint64_t> bySlot = pool->operationConflictsBySlot();
  PRISM_CHECK_EQ(bySlot.size(), kWorkers);
  for (const uint64_t conflicts : bySlot) PRISM_CHECK_EQ(conflicts, uint64_t(0));
}
