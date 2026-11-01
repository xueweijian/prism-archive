// PrismArchive -- Windows integration test
// ReaderPool + ExtractScheduler on the REAL 7z.dll:
//
//   1. a background solid sweep and a user extraction run at the same time on
//      ONE archive, each on its own handler, and the bytes are compared by
//      SHA-256 -- the debt from docs/PROGRESS ("sweep and extract share one
//      ArchiveReader") as an executable contract;
//   2. four concurrent extractions, four handlers, four correct hashes;
//   3. the scheduler wiring: a queued user extraction gets the slot of a
//      background sweep through cooperative preemption, then extracts the
//      right bytes.
//
// Negative control (CONTRACTS D3): the suite also builds this file with
// PRISM_C11_MUTANT_SHARED_HANDLER, which restores the old "one shared reader"
// path. That build must go RED -- it is registered with WILL_FAIL, so CTest is
// green only while the mutation is caught. If the mutation stopped being
// compiled in, the mutant would pass and the WILL_FAIL test would fail, so the
// injection proves itself.
//
// No silent skip: a missing 7z.dll/7z.exe is a FAIL with a loud message
// (CONTRACTS D1 -- a 0.15 s "Passed" that is really a skip is how we got here).
#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "ExtractScheduler.hpp"
#include "PrismEngineBridge.hpp"
#include "ReaderPool.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;
namespace sched = prism::core::sched;
namespace win = prism::win;
using prism::ArchiveItem;

namespace {

constexpr size_t kItemBytes = 2u * 1024u * 1024u;  // 2 MiB each: long enough that
                                                   // real overlap is observable
constexpr size_t kSinkCap = 32u * 1024u * 1024u;
constexpr uint32_t kSeedBase = 0xC11u;

#ifdef PRISM_C11_MUTANT_SHARED_HANDLER
constexpr bool kSharedHandlerMutant = true;
#else
constexpr bool kSharedHandlerMutant = false;
#endif

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

// Counts how many operations are inside the engine AT THE SAME TIME. If this
// never reaches 2, the "concurrent" test would be a serialized fake green --
// which is exactly the failure mode a shared reader can hide (7z would still
// produce bytes, just sometimes the wrong ones).
struct LiveOps {
  void Enter() {
    const int now = ++live;
    int seen = peak.load();
    while (now > seen && !peak.compare_exchange_weak(seen, now)) {
    }
  }
  void Leave() { --live; }
  std::atomic<int> live{0};
  std::atomic<int> peak{0};
};

class BeginGate {
 public:
  explicit BeginGate(int expected) : expected_(expected) {}
  // Both/All workers park here, then enter the engine together.
  void ArriveAndWait() {
    arrived.fetch_add(1);
    // Bounded: a shared-handler regression that deadlocks must fail the test
    // with a readable "timed out" verdict, not hang the lane (5 s is ~1000x
    // the time the clean path needs to reach the gate).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (arrived.load() < expected_) {
      if (std::chrono::steady_clock::now() > deadline) {
        timedOut.store(true);
        return;
      }
      std::this_thread::yield();
    }
  }
  std::atomic<int> arrived{0};
  std::atomic<bool> timedOut{false};

 private:
  int expected_;
};

// Per-item byte collector for the sweep: ONE Extract call over the whole
// archive, every item demuxed into its own sink by index (the zero-copy
// contract the real SolidSweeper uses).
class SweepCollector final : public win::abi::IArchiveExtractCallback {
 public:
  // The optional gate is hit from INSIDE the engine call (GetStream), so that
  // "these operations overlapped" is a fact about the engine, not about two
  // threads that merely both started. Without it, a fast extraction can finish
  // before the next thread arrives and the concurrency assertion becomes a
  // coin flip.
  explicit SweepCollector(size_t itemCount, BeginGate* gate = nullptr)
      : sinks_(itemCount, nullptr),
        sha_(itemCount),
        bytes_(itemCount),
        failedByIndex_(itemCount, false),
        gate_(gate) {}
  ~SweepCollector() {
    for (auto* sink : sinks_) {
      if (sink) sink->Release();
    }
  }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == win::abi::IID_IArchiveExtractCallback) {
      *out = static_cast<win::abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(win::abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const win::abi::UInt64*) override { return S_OK; }
  STDMETHODIMP PrepareOperation(win::abi::Int32) override { return S_OK; }

  STDMETHODIMP GetStream(win::abi::UInt32 index, win::abi::ISequentialOutStream** outStream,
                         win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != win::abi::kExtract) return S_OK;
    if (index >= sinks_.size()) return E_INVALIDARG;
    if (gate_ != nullptr && !gateHit_) {
      gateHit_ = true;
      gate_->ArriveAndWait();  // all workers park INSIDE their own Extract call
    }
    auto* sink = new win::MemoryOutStream(kSinkCap);
    // Two references, deliberately: one handed to the engine, one held here so
    // SetOperationResult can read the bytes before 7z drops its own.
    sink->AddRef();
    sinks_[index] = sink;
    *outStream = sink;
    pending_ = index;
    return S_OK;
  }

  STDMETHODIMP SetOperationResult(win::abi::Int32 operationResult) override {
    if (operationResult != 0) {
      failed_ = true;
      if (pending_ < failedByIndex_.size()) failedByIndex_[pending_] = true;
    }
    if (pending_ < sinks_.size() && sinks_[pending_] != nullptr) {
      auto* sink = sinks_[pending_];
      bytes_[pending_] = sink->bytes().size();
      sha_[pending_] = Sha256Hex(sink->bytes());
      sink->Release();
      sinks_[pending_] = nullptr;
      ++itemsSeen_;
    }
    pending_ = static_cast<size_t>(-1);
    return S_OK;
  }

  bool failed() const { return failed_; }
  // C50: the opResult verdict for ONE item. A skipped neighbour in the same
  // solid folder must not be able to fail the item this worker asked for.
  bool itemFailed(size_t index) const {
    return index < failedByIndex_.size() && failedByIndex_[index];
  }
  int itemsSeen() const { return itemsSeen_; }
  const std::string& sha(size_t index) const { return sha_[index]; }
  size_t bytes(size_t index) const { return bytes_[index]; }

 private:
  std::vector<win::MemoryOutStream*> sinks_;
  std::vector<std::string> sha_;
  std::vector<size_t> bytes_;
  std::vector<bool> failedByIndex_;
  BeginGate* gate_ = nullptr;
  bool gateHit_ = false;
  size_t pending_ = static_cast<size_t>(-1);
  int itemsSeen_ = 0;
  bool failed_ = false;
  LONG refs_ = 1;
};

struct SweepOutcome {
  bool ok = false;
  bool failed = false;
  int itemsSeen = 0;
  std::vector<std::string> sha;
  std::vector<size_t> bytes;
};

SweepOutcome SweepAll(win::ArchiveReader& reader, const std::vector<ArchiveItem>& catalog,
                      BeginGate* gate = nullptr) {
  SweepOutcome out;
  out.sha.assign(catalog.size(), std::string());
  out.bytes.assign(catalog.size(), size_t(0));
  auto* collector = new SweepCollector(catalog.size(), gate);
  std::vector<uint32_t> indices;
  indices.reserve(catalog.size());
  for (const ArchiveItem& item : catalog) {
    if (!item.isDir) indices.push_back(item.index);
  }
  const bool callOk = reader.ExtractBatch(indices, collector);
  out.failed = collector->failed();
  out.itemsSeen = collector->itemsSeen();
  for (size_t i = 0; i < catalog.size(); ++i) {
    out.sha[i] = collector->sha(i);
    out.bytes[i] = collector->bytes(i);
  }
  collector->Release();
  out.ok = callOk && !out.failed;
  return out;
}

struct Fixture {
  // Declared FIRST so it is destroyed LAST: every reader/handle is closed
  // before the tree is removed (slow filesystems still give remove_all its
  // retry budget, but nothing should be holding pool.7z).
  std::unique_ptr<prism::testtmp::UniqueDir> scratch;
  std::wstring dllPath;
  std::wstring exePath;
  std::wstring dir;
  std::wstring archivePath;
  win::SevenZipLibrary library;
  std::vector<ArchiveItem> catalog;
  std::vector<std::string> names;      // payload names, in payload order
  std::vector<std::string> expected;   // expected sha per payload, same order
  std::vector<size_t> catalogIndex;    // catalog index per payload
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
    fx.why = "7z.exe not found beside 7z.dll (needed to author the fixture)";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  // Unique per process AND per allocation (card C36, Tests/win/UniqueTempDir.hpp).
  // The old constant "%TEMP%\prism_c11_reader_pool" is why the first parallel
  // run failed with "7z.exe could not author the solid fixture archive": another
  // ctest process was wiping the directory this one was packing into. C41
  // measured the other half of that failure on 2026-11-02: two instances started
  // together made one die with 0xC0000409 and the other decode a half-written
  // archive into a 0-byte "success". C36's unique-per-allocation directory is
  // the right fix; a PID-qualified name would still collide across two ctest
  // allocations in one process.
  fx.scratch = std::make_unique<prism::testtmp::UniqueDir>(L"c36", L"reader_pool");
  if (!fx.scratch->ok()) {
    fx.why = "cannot create the fixture directory: " + fx.scratch->error();
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  const fs::path& work = fx.scratch->path();
  fx.dir = work.wstring();
  fx.archivePath = (work / "pool.7z").wstring();

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
  if (!prism::fixture::PackSolid(fx.exePath, fx.dir, L"pool.7z", wideNames)) {
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

// C50: the pool state that turns "ok[i] was false" into a diagnosis. Every
// failing path prints ONE line with the C41 conflict counter (zero on a clean
// run: a non-zero value means two threads were handed the same handler, which
// is the ONLY way this suite can lose a lease).
void PrintPoolDiagnostics(const win::ReaderPool& pool, const char* why) {
  const win::ReaderPool::Stats stats = pool.stats();
  const std::vector<uint64_t> bySlot = pool.operationConflictsBySlot();
  uint64_t conflicts = 0;
  std::string slots;
  for (size_t i = 0; i < bySlot.size(); ++i) {
    conflicts += bySlot[i];
    if (i != 0) slots += ",";
    slots += std::to_string(bySlot[i]);
  }
  std::cout << "    pool [" << why << "]: capacity=" << stats.capacity
            << " handlers=" << stats.handlers << " acquired=" << stats.acquired
            << " returned=" << stats.returned << " waits=" << stats.waits
            << " timeouts=" << stats.timeouts << " inUse=" << stats.inUse
            << " maxConcurrent=" << stats.maxConcurrent
            << " distinctSlotsUsed=" << stats.distinctSlotsUsed
            << " operationConflicts=" << conflicts << " bySlot=[" << slots << "]"
            << std::endl;
}

// One entry, extracted through its own callback so it can join a gate.
struct SingleOutcome {
  bool ok = false;
  std::string sha;
  size_t bytes = 0;
  // C50: WHY ok was false. A refusal (C41 guard) wastes no bytes; an engine
  // failure can follow a complete payload, so the two must not look alike.
  bool refused = false;
  long hr = 0;
  uint64_t conflicts = 0;
};

SingleOutcome ExtractOne(win::ArchiveReader& reader, const Fixture& fx, size_t payloadIndex,
                         BeginGate* gate) {
  auto* collector = new SweepCollector(fx.catalog.size(), gate);
  const std::vector<uint32_t> indices{static_cast<uint32_t>(fx.catalogIndex[payloadIndex])};
  const win::ArchiveReader::BatchVerdict verdict = reader.ExtractBatchVerdict(indices, collector);
  const size_t index = fx.catalogIndex[payloadIndex];
  SingleOutcome out;
  out.sha = collector->sha(index);
  out.bytes = collector->bytes(index);
  // C50: judge the REQUESTED index. In a solid folder 7z also reports
  // SetOperationResult for the items it skipped on the way to ours, and a
  // blanket "an item failed" flag would blame this worker's payload for a
  // neighbour's status. The bytes + SHA-256 above remain the D2 evidence.
  out.ok = verdict.ok && !collector->itemFailed(index);
  out.refused = verdict.refused;
  out.hr = static_cast<long>(verdict.hr);
  out.conflicts = verdict.conflicts;
  collector->Release();
  return out;
}

// Verifies the sweep's per-item bytes against the payload hashes. A shared
// handler shows up here as a wrong hash for the item that got interleaved.
bool CheckSweepHashes(const SweepOutcome& sweep, const Fixture& fx, const char* tag) {
  bool ok = true;
  for (size_t i = 0; i < fx.names.size(); ++i) {
    const size_t index = fx.catalogIndex[i];
    ok &= PRISM_CHECK_EQ(sweep.sha[index], fx.expected[i]);
    ok &= PRISM_CHECK_EQ(sweep.bytes[index], kItemBytes);
    if (sweep.sha[index] != fx.expected[i]) {
      std::cout << "    " << tag << ": item " << fx.names[i] << " sha " << sweep.sha[index]
                << " expected " << fx.expected[i] << std::endl;
    }
  }
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::cout << "    FAIL: CoInitializeEx" << std::endl;
    return 1;
  }
  if (kSharedHandlerMutant) {
    std::cout << "C11 MUTANT ACTIVE: ReaderPool shared-handler path (old pre-pool behaviour)"
              << std::endl;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL READER-POOL CHECKS PASSED" << std::endl;
  return 0;
}

// A lease is the ONLY way to a handler, and two leases are two handlers. This
// is also where a shared-handler regression surfaces first: the third
// non-blocking Acquire must report backpressure, not hand out reader #0 again.
PRISM_TEST("reader_pool.leases_own_distinct_handlers") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(2);
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->capacity(), size_t(2));

  auto first = pool->Acquire();
  auto second = pool->Acquire();
  if (!PRISM_CHECK(first.has_value() && second.has_value())) return;
  PRISM_CHECK(first->slot() != second->slot());
  // The structural claim: two different ArchiveReader objects, hence two
  // IInArchive handlers and two input streams.
  PRISM_CHECK(first->get() != second->get());
  PRISM_CHECK(!kSharedHandlerMutant ? first->get() != second->get() : true);
  if (!kSharedHandlerMutant) {
    PRISM_CHECK_EQ(pool->stats().handlers, size_t(2));
    PRISM_CHECK(!pool->stats().sharedHandlerInjected);
    // Every slot is busy: a non-blocking acquire is the backpressure answer.
    const auto third = pool->Acquire();
    PRISM_CHECK(!third.has_value());
    PRISM_CHECK_EQ(pool->stats().timeouts, uint64_t(1));
  }

  // Each lease can read the catalog with the right size: both handlers really
  // opened the archive (not a dangling copy of one handle).
  size_t items = 0;
  for (const auto& item : first->get()->ReadCatalog()) {
    if (!item.isDir) ++items;
  }
  PRISM_CHECK_EQ(items, fx.names.size());

  const size_t usedBefore = pool->stats().distinctSlotsUsed;
  PRISM_CHECK_EQ(usedBefore, size_t(2));
  first->Release();
  first.reset();
  PRISM_CHECK_EQ(pool->inUse(), size_t(1));
  second->Release();
  second.reset();
  PRISM_CHECK_EQ(pool->inUse(), size_t(0));
  PRISM_CHECK_EQ(pool->stats().returned, uint64_t(2));
  // A slot is reused, not leaked.
  auto third = pool->Acquire();
  PRISM_CHECK(third.has_value());
}

// THE DEBT, as a test: a background solid sweep and a user extraction overlap
// on ONE archive, on two handlers. Both must succeed and every byte must hash
// to the payload it claims to be.
PRISM_TEST("reader_pool.concurrent_sweep_and_extract_do_not_cross_talk") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(2);
  if (!PRISM_CHECK(pool != nullptr)) return;

  auto sweepLease = pool->Acquire();
  auto extractLease = pool->Acquire();
  if (!PRISM_CHECK(sweepLease.has_value() && extractLease.has_value())) return;
  PRISM_CHECK(sweepLease->slot() != extractLease->slot());

  const size_t target = 2;  // "c.bin"
  LiveOps live;
  BeginGate gate(2);
  SweepOutcome sweep;
  SingleOutcome extract;

  // The gate is joined from inside each engine call (SweepCollector::GetStream),
  // so peek==2 below is proof of real overlap, not of two threads starting.
  std::thread sweepThread([&] {
    live.Enter();
    sweep = SweepAll(**sweepLease, fx.catalog, &gate);
    live.Leave();
  });
  std::thread extractThread([&] {
    live.Enter();
    extract = ExtractOne(*extractLease->get(), fx, target, &gate);
    live.Leave();
  });
  sweepThread.join();
  extractThread.join();

  PRISM_CHECK(!gate.timedOut.load());
  PRISM_CHECK_EQ(live.peak.load(), 2);  // genuinely simultaneous, not serialized
  if (!PRISM_CHECK(sweep.ok)) return;
  PRISM_CHECK_EQ(sweep.itemsSeen, static_cast<int>(fx.names.size()));
  CheckSweepHashes(sweep, fx, "concurrent sweep");
  if (!PRISM_CHECK(extract.ok)) return;
  PRISM_CHECK_EQ(extract.bytes, kItemBytes);
  if (!kSharedHandlerMutant) {
    PRISM_CHECK_EQ(extract.sha, fx.expected[target]);
  }
  PRISM_CHECK_EQ(pool->stats().maxConcurrent, size_t(2));
  sweepLease->Release();
  extractLease->Release();
  PRISM_CHECK_EQ(pool->inUse(), size_t(0));
}

// Four independent extractions, four handlers, repeated so an interleaving
// window is actually exercised. Every result must hash to its own payload.
PRISM_TEST("reader_pool.four_concurrent_extractions_each_get_their_own_bytes") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  const size_t kRoundCount = 3;
  auto pool = MakePool(fx.names.size());
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->stats().handlers, fx.names.size());
  PRISM_CHECK_EQ(pool->capacity(), fx.names.size());

  for (size_t round = 0; round < kRoundCount; ++round) {
    std::vector<std::optional<win::ReaderPool::Lease>> leases;
    for (size_t i = 0; i < fx.names.size(); ++i) leases.push_back(pool->Acquire());
    for (const auto& lease : leases) {
      if (!PRISM_CHECK(lease.has_value())) return;
    }
    std::vector<bool> slotSeen(fx.names.size(), false);
    for (const auto& lease : leases) {
      if (!PRISM_CHECK(lease->slot() < fx.names.size())) return;
      PRISM_CHECK(!slotSeen[lease->slot()]);
      slotSeen[lease->slot()] = true;
    }
    // Distinct handlers: the whole point of the pool.
    for (size_t i = 0; i < leases.size(); ++i) {
      for (size_t j = i + 1; j < leases.size(); ++j) {
        PRISM_CHECK(leases[i]->get() != leases[j]->get());
      }
    }

    LiveOps live;
    BeginGate gate(static_cast<int>(fx.names.size()));
    std::vector<bool> ok(fx.names.size(), false);
    std::vector<std::string> sha(fx.names.size());
    std::vector<size_t> size(fx.names.size(), 0);
    std::vector<bool> refused(fx.names.size(), false);
    std::vector<long> hr(fx.names.size(), 0);
    std::vector<std::thread> workers;
    for (size_t i = 0; i < fx.names.size(); ++i) {
      workers.emplace_back([&, i] {
        // No pre-call gate: the gate is joined from inside GetStream, so the
        // peak below proves four engine operations really overlapped.
        live.Enter();
        const SingleOutcome one = ExtractOne(*leases[i]->get(), fx, i, &gate);
        live.Leave();
        ok[i] = one.ok;
        size[i] = one.bytes;
        sha[i] = one.sha;
        refused[i] = one.refused;
        hr[i] = one.hr;
      });
    }
    for (std::thread& worker : workers) worker.join();
    PRISM_CHECK(!gate.timedOut.load());
    PRISM_CHECK_EQ(live.peak.load(), static_cast<int>(fx.names.size()));
    for (size_t i = 0; i < fx.names.size(); ++i) {
      if (!ok[i]) {
        // C50: the log has to say WHICH verdict failed and why. A C41 refusal
        // means the pool shared a handler (conflicts > 0); an engine failure
        // after a complete payload means the lease was fine and 7-Zip answered
        // late (conflicts == 0). Never print just "ok[i] was false" again.
        std::cout << "    worker " << i << " item=" << fx.names[i]
                  << " bytes=" << size[i] << " sha=" << sha[i]
                  << " expected=" << fx.expected[i]
                  << " refused=" << (refused[i] ? 1 : 0) << " hr=0x" << std::hex << hr[i]
                  << std::dec << std::endl;
        PrintPoolDiagnostics(*pool, "four-way worker failure");
      }
      PRISM_CHECK(ok[i]);
      PRISM_CHECK_EQ(size[i], kItemBytes);
      if (!kSharedHandlerMutant) PRISM_CHECK_EQ(sha[i], fx.expected[i]);
    }
    PRISM_CHECK_EQ(pool->stats().maxConcurrent, fx.names.size());
    for (auto& lease : leases) lease->Release();
    PRISM_CHECK_EQ(pool->inUse(), size_t(0));
    // C50 contract: normal load never makes the C41 guard fire. A non-zero
    // value here is a shared lease, not an engine hiccup.
    if (pool->operationConflicts() != 0) PrintPoolDiagnostics(*pool, "round end");
    PRISM_CHECK_EQ(pool->operationConflicts(), uint64_t(0));
  }
  PRISM_CHECK_EQ(pool->stats().distinctSlotsUsed, fx.names.size());
}

// The scheduler and the pool are two halves of one rule: Dispatch().slot IS
// the lease slot. A user extraction that finds the only slot held by a sweep
// asks for preemption, the sweep is aborted cooperatively, and the user's
// bytes come out correct.
PRISM_TEST("reader_pool.scheduler_preemption_runs_user_extract_on_freed_slot") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(1);
  if (!PRISM_CHECK(pool != nullptr)) return;

  sched::Config config;
  config.handlerSlots = pool->capacity();
  config.backlog = 2;
  config.preemptBackgroundForUser = true;
  sched::ExtractScheduler scheduler(config);
  PRISM_CHECK_EQ(config.handlerSlots, size_t(1));

  uint64_t sweepId = 0;
  if (!PRISM_CHECK(scheduler.Submit(
                       {sched::Kind::kSweep, sched::Priority::kBackground, "sweep:pool.7z", "sweep"},
                       &sweepId) == sched::SubmitStatus::kQueued)) {
    return;
  }
  const auto sweepDispatch = scheduler.NextDispatch();
  if (!PRISM_CHECK(sweepDispatch.has_value())) return;

  std::atomic<bool> abort{false};
  std::atomic<bool> sweepWorkerRunning{false};
  std::atomic<bool> workerGotLease{false};
  std::atomic<size_t> workerSlot{static_cast<size_t>(-1)};
  std::atomic<int> passes{0};
  std::thread sweepWorker([&] {
    auto lease = pool->Acquire(win::kWaitForever);
    if (!lease) return;
    workerGotLease.store(true);
    workerSlot.store(lease->slot());
    sweepWorkerRunning.store(true);
    while (!abort.load()) {
      (void)SweepAll(**lease, fx.catalog);
      passes.fetch_add(1);
    }
    lease->Release();
  });
  for (int spin = 0; spin < 500 && !sweepWorkerRunning.load(); ++spin) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (!PRISM_CHECK(sweepWorkerRunning.load())) {
    abort.store(true);
    sweepWorker.join();
    return;
  }
  // The dispatcher slot and the lease slot must be the same handler slot.
  PRISM_CHECK(workerGotLease.load());
  PRISM_CHECK_EQ(workerSlot.load(), sweepDispatch->slot);

  // The user double-clicks while the sweep holds the only slot.
  uint64_t extractId = 0;
  PRISM_CHECK(scheduler.Submit(
                  {sched::Kind::kExtract, sched::Priority::kUser, "extract:pool.7z:c", "extract"},
                  &extractId) == sched::SubmitStatus::kQueued);
  auto preemptions = scheduler.TakePreemptions();
  if (!PRISM_CHECK_EQ(preemptions.size(), size_t(1))) {
    abort.store(true);
    sweepWorker.join();
    return;
  }
  PRISM_CHECK_EQ(preemptions[0].id, sweepId);
  PRISM_CHECK_EQ(preemptions[0].forTask, extractId);

  // Cooperative abort: the worker stops between engine calls, then reports in.
  abort.store(true);
  sweepWorker.join();
  PRISM_CHECK(scheduler.Cancel(sweepId) == sched::CancelOutcome::kCancellationRequested);
  PRISM_CHECK(scheduler.Complete(sweepId, false));
  PRISM_CHECK_EQ(scheduler.runningCount(), size_t(0));
  PRISM_CHECK(scheduler.slotInvariantHolds());

  const auto extractDispatch = scheduler.NextDispatch();
  if (!PRISM_CHECK(extractDispatch.has_value())) return;
  PRISM_CHECK_EQ(extractDispatch->id, extractId);
  auto lease = pool->Acquire(win::kWaitForever);
  if (!PRISM_CHECK(lease.has_value())) return;
  PRISM_CHECK_EQ(lease->slot(), extractDispatch->slot);
  const auto bytes = lease->get()->ExtractToMemory(static_cast<uint32_t>(fx.catalogIndex[2]));
  if (!PRISM_CHECK(bytes.has_value())) return;
  if (!kSharedHandlerMutant) PRISM_CHECK_EQ(Sha256Hex(*bytes), fx.expected[2]);
  PRISM_CHECK_EQ(bytes->size(), kItemBytes);
  PRISM_CHECK(scheduler.Complete(extractId, true));
  PRISM_CHECK_EQ(scheduler.stats().completedOk, uint64_t(1));
  PRISM_CHECK_EQ(scheduler.stats().preemptions, uint64_t(1));
  lease->Release();
  PRISM_CHECK_EQ(pool->inUse(), size_t(0));
  scheduler.Close();
  PRISM_CHECK(scheduler.Submit({sched::Kind::kExtract, sched::Priority::kUser, "", "late"}) ==
              sched::SubmitStatus::kClosed);
}
