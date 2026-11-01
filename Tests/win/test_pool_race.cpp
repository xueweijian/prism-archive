// PrismArchive -- Windows integration test (card C41)
// CONCURRENT EXTRACTION MUST NEVER RETURN A "SUCCESSFUL" EMPTY ITEM.
//
// The observed CI failures on win.reader_pool were all shapes of the same
// silent data loss: one of four workers reports ok=false, or -- worse -- it
// reports SUCCESS and the payload hashes to the SHA-256 of the EMPTY STRING
// (e3b0c442...), i.e. a 0-byte file that nobody flagged.
//
// This suite drives the REAL product path for four concurrent extractions:
//   ReaderPool lease -> ArchiveReader::ExtractWithPlan -> SmartExtractCallback
// Four handlers, four single-item SmartPlans, the SAME solid 7z archive, 50
// rounds. Every round asserts, per worker, BYTES and SHA-256:
//   1. ExtractWithPlan() returned true;
//   2. the callback recorded exactly one written file and kItemBytes bytes;
//   3. the file on disk exists, is kItemBytes long and hashes to its payload.
// A run that loses data cannot pass (D2: content, not existence).
//
// Overlap is counted around the engine call (D24 needs the operations to be
// simultaneous; the exact in-callback rendezvous lives in win.reader_pool,
// which gates inside GetStream).
//
// Negative control (D3): this file is ALSO built with
// PRISM_C41_MUTANT_NO_OPERATION_GUARD, which compiles the one-handler-one-
// operation guard out of ArchiveReader. The "second operation is refused" case
// must go red -- the mutant accepts it on a busy handler -- and the binary
// prints C41 MUTANT CAUGHT only when the injection provably fired.
//
// No silent skip: a missing 7z.dll/7z.exe is a FAIL with a loud message (D1);
// a genuinely absent engine exits 77 -> visible Skip.
#include <windows.h>

#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "PrismEngineBridge.hpp"
#include "ReaderPool.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;
namespace win = prism::win;
using prism::ArchiveItem;

namespace {

constexpr size_t kItemBytes = 1u * 1024u * 1024u;  // per payload
constexpr int kRounds = 50;
constexpr size_t kWorkers = 4;

#ifdef PRISM_C41_MUTANT_NO_OPERATION_GUARD
constexpr bool kNoGuardMutant = true;
std::atomic<bool> g_mutantSawBypass{false};
#else
constexpr bool kNoGuardMutant = false;
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

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>* out) {
  std::ifstream file(fs::path(path), std::ios::binary);
  if (!file.good()) return false;
  out->assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  return true;
}

// Distinct fixture directory PER PROCESS (CONTRACTS: test fixtures must be
// isolated). The old fixed name let two suite binaries delete and re-pack the
// same archive under each other -- measured on 2026-11-02: the loser of that
// race died with 0xC0000409 or decoded a half-written archive. A path that no
// other process can name is the only concurrency-safe fixture.
fs::path UniqueWorkDir() {
  return fs::temp_directory_path() /
         ("prism_c41_pool_race_" + std::to_string(GetCurrentProcessId()));
}

struct Fixture {
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
  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty()) {
    fx.why = "PRISM_7Z_DLL is not set (windows-latest ships 7-Zip)";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  if (exePath.empty()) {
    fx.why = "7z.exe not found beside 7z.dll (needed to author the fixture)";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  const fs::path work = UniqueWorkDir();
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  fx.dir = work.wstring();
  fx.archivePath = (work / "race.7z").wstring();

  for (size_t i = 0; i < kWorkers; ++i) {
    const std::string name = "race_" + std::to_string(i) + ".bin";
    const std::vector<uint8_t> payload = MakePayload(0xC41u + static_cast<uint32_t>(i), kItemBytes);
    std::ofstream file(work / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    if (!file.good()) {
      fx.why = "cannot write fixture payload " + name;
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
    fx.names.push_back(name);
    fx.expected.push_back(Sha256Hex(payload));
  }
  std::vector<std::wstring> wideNames;
  for (const std::string& name : fx.names) {
    wideNames.push_back(std::wstring(name.begin(), name.end()));
  }
  if (!prism::fixture::PackSolid(exePath, fx.dir, L"race.7z", wideNames)) {
    fx.why = "7z.exe could not author the solid fixture archive";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  if (!fx.library.Load(dllPath)) {
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

// A plan that extracts EXACTLY ONE catalog entry: everything else is ignored.
// This is the product path -- ExtractWithPlan -> SmartExtractCallback -- with
// four handlers running one single-item operation each at the same time.
prism::SmartPlan SingleItemPlan(const Fixture& fx, size_t worker, const std::string& outputDir) {
  prism::SmartPlan plan = prism::SmartExtractionPlanner::Build(
      "race.7z", outputDir, fx.catalog, [](const std::string&) { return false; });
  for (size_t i = 0; i < fx.names.size(); ++i) {
    if (i != worker) plan.ignoredIndices.insert(static_cast<uint32_t>(fx.catalogIndex[i]));
  }
  return plan;
}

struct Outcome {
  bool ok = false;
  uint64_t filesWritten = 0;
  uint64_t bytesWritten = 0;
  uint64_t failedItems = 0;
  bool guardTripped = false;
  bool fuseTripped = false;
  std::string error;
};

// The engine call, with a live-operation counter around it so the suite can
// prove the four extractions really overlapped.
class LiveOps {
 public:
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

// Parks the FIRST operation inside the engine's GetStream until the main
// thread says "go". That makes "two operations wanted this handler at the same
// time" a fact about the engine callback (D24), not a timing guess.
class BlockingCollector final : public win::abi::IArchiveExtractCallback {
 public:
  explicit BlockingCollector(size_t capacity) { sink_ = new win::MemoryOutStream(capacity); }
  ~BlockingCollector() {
    if (sink_) sink_->Release();
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
  STDMETHODIMP GetStream(win::abi::UInt32, win::abi::ISequentialOutStream** outStream,
                         win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != win::abi::kExtract) return S_OK;
    arrived.store(true);
    // Bounded: a regression must fail loud, never hang the lane.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!release.load() && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    sink_->AddRef();
    *outStream = sink_;
    return S_OK;
  }
  STDMETHODIMP SetOperationResult(win::abi::Int32 operationResult) override {
    failed_ = failed_ || operationResult != 0;
    bytes_.store(sink_ ? sink_->bytes().size() : 0);
    return S_OK;
  }

  size_t bytes() const { return bytes_.load(); }
  bool failed() const { return failed_; }

  std::atomic<bool> arrived{false};
  std::atomic<bool> release{false};

 private:
  win::MemoryOutStream* sink_ = nullptr;
  std::atomic<size_t> bytes_{0};
  bool failed_ = false;
  LONG refs_ = 1;
};

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::cout << "    FAIL: CoInitializeEx" << std::endl;
    return 1;
  }
  if (kNoGuardMutant) {
    std::cout << "C41 MUTANT ACTIVE: one-operation-per-handler guard COMPILED OUT" << std::endl;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc != 0) {
    std::cout << "POOL-RACE CHECKS FAILED" << std::endl;
#ifdef PRISM_C41_MUTANT_NO_OPERATION_GUARD
    // CONTRACTS D3/D26: the marker is a CONJUNCTION -- the checks went red AND
    // the injection provably fired (the guard was bypassed while a handler was
    // busy). A mutation that stopped compiling in prints nothing.
    if (g_mutantSawBypass.load() && prism::win::MutantOperationGuardBypasses().load() > 0) {
      std::cout << "C41 MUTANT CAUGHT (bypasses="
                << prism::win::MutantOperationGuardBypasses().load() << ")" << std::endl;
    }
#endif
    return rc;
  }
  std::cout << "ALL POOL-RACE CHECKS PASSED" << std::endl;
  return 0;
}

// THE stress case: 4 leased handlers, same solid archive, 4 different entries,
// 50 rounds. A single empty/mismatched payload is a failure.
PRISM_TEST("pool_race.four_concurrent_extracts_never_return_empty_bytes") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(kWorkers);
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->stats().handlers, kWorkers);

  std::vector<win::ArchiveReader*> readers;
  LiveOps live;
  std::atomic<int> empties{0};
  std::atomic<int> wrongHash{0};
  std::atomic<int> notOk{0};
  std::atomic<int> badCounts{0};

  for (int round = 0; round < kRounds; ++round) {
    std::vector<std::optional<win::ReaderPool::Lease>> leases;
    for (size_t i = 0; i < kWorkers; ++i) {
      leases.push_back(pool->Acquire(win::kWaitForever));
    }
    for (const auto& lease : leases) {
      if (!PRISM_CHECK(lease.has_value())) return;
    }
    readers.clear();
    for (const auto& lease : leases) readers.push_back(lease->get());
    // Structural proof first (two leases can NEVER be one handler), but do not
    // short-circuit: a shared-handler build must still run the extractions so
    // the byte assertions catch the DATA it loses, not just the shape.
    for (size_t i = 0; i < readers.size(); ++i) {
      for (size_t j = i + 1; j < readers.size(); ++j) {
        PRISM_CHECK(readers[i] != readers[j]);
      }
    }

    std::vector<Outcome> outcome(kWorkers);
    std::vector<std::vector<uint8_t>> bytes(kWorkers);
    std::vector<std::string> sha(kWorkers);
    std::vector<std::string> outputDir(kWorkers);
    std::vector<prism::SmartPlan> plans;
    plans.reserve(kWorkers);
    for (size_t i = 0; i < kWorkers; ++i) {
      outputDir[i] = (fs::path(fx.dir) / ("out_r" + std::to_string(round) + "_w" +
                                          std::to_string(i)))
                         .string();
      std::error_code ec;
      fs::create_directories(outputDir[i], ec);
      plans.push_back(SingleItemPlan(fx, i, outputDir[i]));
    }

    std::vector<std::thread> workers;
    for (size_t i = 0; i < kWorkers; ++i) {
      workers.emplace_back([&, i] {
        live.Enter();
        prism::win::SmartExtractCallback::Result result;
        const bool ok = readers[i]->ExtractWithPlan(fx.catalog, plans[i], plans[i].baseTargetDir,
                                                     nullptr, &result);
        live.Leave();
        Outcome out;
        out.ok = ok;
        out.filesWritten = result.filesWritten;
        out.bytesWritten = result.bytesWritten;
        out.failedItems = result.failedItems;
        out.guardTripped = result.guardTripped;
        out.fuseTripped = result.fuseTripped;
        out.error = result.failureReason.empty() ? result.guardReason : result.failureReason;
        outcome[i] = out;
        // Read the payload the product actually wrote, by its own path.
        const std::string finalPath =
            prism::SmartExtractionPlanner::TransformItemPath(plans[i], fx.names[i]);
        if (!ReadFileBytes(prism::win::Utf8ToWide(finalPath), &bytes[i])) bytes[i].clear();
        sha[i] = bytes[i].empty() ? std::string() : Sha256Hex(bytes[i]);
      });
    }
    for (std::thread& worker : workers) worker.join();

    for (size_t i = 0; i < kWorkers; ++i) {
      const bool ok = outcome[i].ok;
      const bool empty = bytes[i].empty();
      if (!ok) ++notOk;
      if (empty) ++empties;
      if (!empty && sha[i] != fx.expected[i]) ++wrongHash;
      if (outcome[i].filesWritten != 1 || outcome[i].bytesWritten != kItemBytes) ++badCounts;

      if (!ok || empty || (sha[i] != fx.expected[i]) ||
          outcome[i].filesWritten != 1 || outcome[i].bytesWritten != kItemBytes) {
        std::cout << "    round " << round << " worker " << i << " FAILED: ok=" << ok
                  << " filesWritten=" << outcome[i].filesWritten
                  << " bytesWritten=" << outcome[i].bytesWritten
                  << " failedItems=" << outcome[i].failedItems
                  << " guard=" << outcome[i].guardTripped << " fuse=" << outcome[i].fuseTripped
                  << " diskBytes=" << bytes[i].size() << " sha="
                  << (sha[i].empty() ? std::string("<empty>") : sha[i])
                  << " expected=" << fx.expected[i]
                  << (outcome[i].error.empty() ? std::string() : " reason=" + outcome[i].error)
                  << std::endl;
      }
      PRISM_CHECK(ok);
      PRISM_CHECK(!empty);
      PRISM_CHECK_EQ(sha[i], fx.expected[i]);
      PRISM_CHECK_EQ(outcome[i].filesWritten, uint64_t(1));
      PRISM_CHECK_EQ(outcome[i].bytesWritten, uint64_t(kItemBytes));
      PRISM_CHECK_EQ(bytes[i].size(), kItemBytes);
    }

    for (auto& lease : leases) lease->Release();
    PRISM_CHECK_EQ(pool->inUse(), size_t(0));
  }

  std::cout << "    stress summary: " << kRounds << " rounds x " << kWorkers
            << " workers, peak concurrency " << live.peak.load() << ", empty=" << empties.load()
            << " wrongHash=" << wrongHash.load() << " notOk=" << notOk.load()
            << " badCounts=" << badCounts.load() << std::endl;
  // The four extractions must genuinely overlap; a serialized run would prove
  // nothing about concurrency (D24).
  PRISM_CHECK(live.peak.load() >= 2);
  PRISM_CHECK_EQ(empties.load(), 0);
  PRISM_CHECK_EQ(wrongHash.load(), 0);
  PRISM_CHECK_EQ(notOk.load(), 0);
  PRISM_CHECK_EQ(badCounts.load(), 0);
  PRISM_CHECK_EQ(pool->stats().maxConcurrent, kWorkers);
  PRISM_CHECK_EQ(pool->stats().distinctSlotsUsed, kWorkers);
}

// Direct proof that the callback owns the sink for exactly the item it handed
// out: a single-handler, single-item extraction repeated many times must stay
// byte-identical (the "self-healing cursor" must not leak into the next call).
PRISM_TEST("pool_race.single_handler_repeats_are_byte_identical") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(1);
  if (!PRISM_CHECK(pool != nullptr)) return;
  for (int round = 0; round < 20; ++round) {
    auto lease = pool->Acquire(win::kWaitForever);
    if (!PRISM_CHECK(lease.has_value())) return;
    const size_t worker = static_cast<size_t>(round) % kWorkers;
    const std::string outputDir =
        (fs::path(fx.dir) / ("single_" + std::to_string(round))).string();
    std::error_code ec;
    fs::create_directories(outputDir, ec);
    const prism::SmartPlan plan = SingleItemPlan(fx, worker, outputDir);
    prism::win::SmartExtractCallback::Result result;
    const bool ok =
        lease->get()->ExtractWithPlan(fx.catalog, plan, plan.baseTargetDir, nullptr, &result);
    PRISM_CHECK(ok);
    PRISM_CHECK_EQ(result.filesWritten, uint64_t(1));
    PRISM_CHECK_EQ(result.bytesWritten, uint64_t(kItemBytes));
    std::vector<uint8_t> bytes;
    const std::string finalPath =
        prism::SmartExtractionPlanner::TransformItemPath(plan, fx.names[worker]);
    PRISM_CHECK(ReadFileBytes(prism::win::Utf8ToWide(finalPath), &bytes));
    PRISM_CHECK_EQ(bytes.size(), kItemBytes);
    PRISM_CHECK_EQ(Sha256Hex(bytes), fx.expected[worker]);
  }
}

// THE INVARIANT, ENFORCED: a second engine operation on a handler that is
// already inside 7z is REFUSED. The first operation is parked inside its own
// GetStream, so the overlap is a fact about the engine callback (D24), not a
// timing guess -- which is what makes this a deterministic negative control:
// compile the guard out (PRISM_C41_MUTANT_NO_OPERATION_GUARD) and the second
// operation is accepted on the busy handler, which is exactly the shape that
// produced ok=false / empty payloads / wrong bytes before C41.
PRISM_TEST("pool_race.second_operation_on_one_handler_is_refused") {
  if (!PRISM_CHECK(SetupFixture())) return;
  const Fixture& fx = World();
  auto pool = MakePool(1);
  if (!PRISM_CHECK(pool != nullptr)) return;
  auto lease = pool->Acquire(win::kWaitForever);
  if (!PRISM_CHECK(lease.has_value())) return;
  win::ArchiveReader* reader = lease->get();

  auto* collector = new BlockingCollector(32ull * 1024 * 1024);
  std::atomic<bool> firstOk{false};
  std::thread first([&] {
    firstOk.store(reader->ExtractBatch(
        {static_cast<uint32_t>(fx.catalogIndex[0])}, collector));
  });
  // Wait until operation #1 is parked INSIDE 7z's GetStream.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!collector->arrived.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  PRISM_CHECK(collector->arrived.load());
  PRISM_CHECK(reader->inOperation());

  // Operation #2 on the SAME handler, while #1 is still inside the engine.
  const auto second = reader->ExtractToMemory(static_cast<uint32_t>(fx.catalogIndex[1]));
  const bool refused = !second.has_value() && reader->operationConflicts() == 1;
#ifdef PRISM_C41_MUTANT_NO_OPERATION_GUARD
  if (!refused) g_mutantSawBypass.store(true);
#endif
  PRISM_CHECK(!second.has_value());  // the guard refused it
  PRISM_CHECK_EQ(reader->operationConflicts(), uint64_t(1));

  collector->release.store(true);
  first.join();
  PRISM_CHECK(firstOk.load());
  PRISM_CHECK(!collector->failed());
  // The refused second call must not have damaged operation #1.
  PRISM_CHECK_EQ(collector->bytes(), kItemBytes);
  PRISM_CHECK(!reader->inOperation());  // the guard was released, not leaked
  collector->Release();

  // And the handler is usable again afterwards.
  const auto again = reader->ExtractToMemory(static_cast<uint32_t>(fx.catalogIndex[1]));
  if (PRISM_CHECK(again.has_value())) {
    PRISM_CHECK_EQ(again->size(), kItemBytes);
    PRISM_CHECK_EQ(Sha256Hex(*again), fx.expected[1]);
  }
  PRISM_CHECK_EQ(reader->operationConflicts(), uint64_t(1));
  lease->Release();
}
