// PrismArchive -- Windows integration test (card C50)
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
// Negative control (CONTRACTS D3 + D27): the same source is built with
// PRISM_C50_MUTANT_SHARED_LEASE, which removes the lease mutual exclusion in
// ReaderPool::Acquire (no slot is marked busy, everybody is handed slot 0).
// That build must go RED and print "C50 MUTATION CAUGHT: shared_lease" -- and
// the verdict is gated on the D3 predicate (operationConflicts > 0), so a run
// that goes red for an unrelated reason can never print the marker. The marker
// is the CTest verdict (D26: no WILL_FAIL stacked on a regex).
//
// No silent skip: a missing 7z.dll/7z.exe is a FAIL with a loud message (D1).
#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
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

// The D27 predicate's input: the freshest operationConflicts() total we have
// observed. Set BEFORE every assertion that could report, so ReportFailure can
// decide "the injected shared lease really fired" instead of guessing.
std::atomic<uint64_t> g_conflictsSeen{0};

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

// One compact, greppable block. Every failure path prints this, so the next CI
// red says WHICH handler conflicted instead of only "ok[i] was false".
void DumpPool(const win::ReaderPool& pool, const std::string& why) {
  const win::ReaderPool::Stats s = pool.stats();
  const std::vector<uint64_t> bySlot = pool.operationConflictsBySlot();
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
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL POOL-LEASE CHECKS PASSED" << std::endl;
  return 0;
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
    PRISM_CHECK_EQ(st.distinctSlotsUsed, kWorkers);
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

// Hypothesis 2 from the card, made executable: the window between a lease
// being released and a blocked Acquire waking up. One slot, three waiters --
// if a slot flag can be overwritten in that window, two waiters hold it.
PRISM_TEST("pool_lease.blocked_waiters_are_served_one_at_a_time") {
  if (!PRISM_CHECK(SetupFixture())) return;
  auto pool = MakePool(1);
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->capacity(), size_t(1));

  constexpr size_t kWaiters = 3;
  constexpr size_t kCycles = 12;
  std::atomic<bool> held{false};
  std::atomic<size_t> duplicateLeases{0};

  for (size_t cycle = 0; cycle < kCycles; ++cycle) {
    std::vector<std::thread> waiters;
    waiters.reserve(kWaiters);
    for (size_t w = 0; w < kWaiters; ++w) {
      waiters.emplace_back([&, w] {
        auto lease = pool->Acquire(win::kWaitForever);
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
    for (std::thread& waiter : waiters) waiter.join();
    SyncConflicts(*pool);
    PRISM_CHECK_EQ(pool->inUse(), size_t(0));
    PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
  }

  PRISM_CHECK_EQ(duplicateLeases.load(), size_t(0));
  const win::ReaderPool::Stats st = pool->stats();
  PRISM_CHECK_EQ(st.acquired, st.returned);
  PRISM_CHECK(st.waits > 0);  // the blocking path really was exercised
  SyncConflicts(*pool);
  PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
}
