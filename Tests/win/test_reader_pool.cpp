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
#include <cstdlib>
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

// C70: how many 4-way rounds this process runs. The shipped default stays 3 so
// ctest's budget does not move; PRISM_C70_STRESS_ROUNDS lets the SAME binary
// hammer the intermittent path locally (the red needs hundreds of rounds to
// show up, and a stress knob is not a weaker assertion -- the checks are the
// same on every round).
constexpr size_t kDefaultFourWayRounds = 3;

size_t FourWayRounds() {
  const char* raw = std::getenv("PRISM_C70_STRESS_ROUNDS");
  if (raw == nullptr || *raw == '\0') return kDefaultFourWayRounds;
  const long parsed = std::strtol(raw, nullptr, 10);
  if (parsed <= 0) return kDefaultFourWayRounds;
  return static_cast<size_t>(parsed);
}

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

// C70: ONE row of the engine's callback sequence. The failure this card chases
// ("payload correct, ok[i] false") can only come from SetOperationResult, so
// the row records the code the engine sent, which index the collector had
// pending at that moment, and how many bytes that index already held. A code
// that arrives with written==0 is a neighbour's verdict; a code that arrives
// with the full payload is the engine failing an item it fully decoded.
struct OpEvent {
  win::abi::Int32 code = 0;
  size_t index = static_cast<size_t>(-1);  // collector's pending index
  size_t written = 0;                      // bytes that index held right then
  bool hadSink = false;                    // false: the code belongs to nobody
  // The askExtractMode of the PrepareOperation that opened the item being
  // closed (0 = kExtract, 1 = kTest, 2 = kSkip). 7-Zip calls GetStream then
  // PrepareOperation per item, so this names HOW the engine opened it: a
  // kTest after a kDataError is 7-Zip's FlushCorrupted, not a normal item.
  win::abi::Int32 mode = -1;
};

inline const char* DescribeOpResult(win::abi::Int32 code) {
  switch (code) {
    case 0: return "kOK";
    case 1: return "kUnsupportedMethod";
    case 2: return "kDataError";
    case 3: return "kCRCError";
    case 4: return "kUnavailable";
    case 5: return "kUnexpectedEnd";
    case 6: return "kDataAfterEnd";
    case 7: return "kIsNotArc";
    case 8: return "kHeadersError";
    case 9: return "kWrongPassword";
    default: return "unknown";
  }
}

// C70: what the engine told ONE worker, in order. Printed only on a failure,
// and it answers the questions the old log could not: which SetOperationResult
// carried a non-zero code, which index was pending when it arrived, and whether
// that index already held the full payload.
struct CallbackTrace {
  std::vector<OpEvent> events;
  std::vector<uint32_t> skippedIndices;
  size_t extractStreams = 0;
  size_t skippedStreams = 0;
  size_t unattributedFailures = 0;

  void Print(size_t requested) const {
    std::cout << "      callback: extractStreams=" << extractStreams
              << " skippedStreams=" << skippedStreams << " resultCalls=" << events.size()
              << " unattributedFailures=" << unattributedFailures << " skippedIndices=[";
    for (size_t i = 0; i < skippedIndices.size(); ++i) {
      if (i != 0) std::cout << ",";
      std::cout << skippedIndices[i];
    }
    std::cout << "]" << std::endl;
    for (size_t i = 0; i < events.size(); ++i) {
      const OpEvent& e = events[i];
      std::cout << "      #" << i << " SetOperationResult(code=" << e.code << " "
                << DescribeOpResult(e.code) << ") askMode=" << e.mode << " pending=";
      if (e.hadSink) {
        std::cout << e.index << (e.index == requested ? " (THIS worker)" : " (other)")
                  << " written=" << e.written;
      } else {
        std::cout << "none written=0";
      }
      std::cout << std::endl;
    }
  }
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
  STDMETHODIMP PrepareOperation(win::abi::Int32 askExtractMode) override {
    lastMode_ = askExtractMode;
    return S_OK;
  }

  STDMETHODIMP GetStream(win::abi::UInt32 index, win::abi::ISequentialOutStream** outStream,
                         win::abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != win::abi::kExtract) {
      ++skippedStreams_;
      if (skippedIndices_.size() < 8) skippedIndices_.push_back(index);
      return S_OK;  // skip: no sink, and pending_ stays whatever it was
    }
    ++extractStreams_;
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
    const size_t attributed = pending_;
    const bool hadSink = pending_ < sinks_.size() && sinks_[pending_] != nullptr;
    if (events_.size() < 32) {
      OpEvent event;
      event.code = operationResult;
      event.index = hadSink ? attributed : static_cast<size_t>(-1);
      event.written = hadSink ? sinks_[attributed]->bytes().size() : 0;
      event.hadSink = hadSink;
      event.mode = lastMode_;
      events_.push_back(event);
    }
    if (operationResult != 0) {
      failed_ = true;
      if (!hadSink) ++unattributedFailures_;
    }
    if (hadSink) {
      auto* sink = sinks_[attributed];
      bytes_[attributed] = sink->bytes().size();
      sha_[attributed] = Sha256Hex(sink->bytes());
      sink->Release();
      sinks_[attributed] = nullptr;
      ++itemsSeen_;
    }
    pending_ = static_cast<size_t>(-1);
    return S_OK;
  }

  bool failed() const { return failed_; }
  // C70: the engine side of a worker, copied out before Release() so a failing
  // worker can print exactly what 7z told it.
  void Snapshot(CallbackTrace* out) const {
    out->events = events_;
    out->skippedIndices = skippedIndices_;
    out->extractStreams = extractStreams_;
    out->skippedStreams = skippedStreams_;
    out->unattributedFailures = unattributedFailures_;
  }
  // NOTE (C70): this collector no longer decides WHICH item a code belongs to.
  // It witnesses bytes (D2) and records the raw callback sequence; the verdict
  // is ArchiveReader::BatchVerdict::itemFailed(), attributed by the engine's own
  // GetStream/SetOperationResult pairing.
  int itemsSeen() const { return itemsSeen_; }
  const std::string& sha(size_t index) const { return sha_[index]; }
  size_t bytes(size_t index) const { return bytes_[index]; }

 private:
  std::vector<win::MemoryOutStream*> sinks_;
  std::vector<std::string> sha_;
  std::vector<size_t> bytes_;
  BeginGate* gate_ = nullptr;
  bool gateHit_ = false;
  size_t pending_ = static_cast<size_t>(-1);
  win::abi::Int32 lastMode_ = -1;
  int itemsSeen_ = 0;
  bool failed_ = false;
  LONG refs_ = 1;
  // C70 diagnostics (never consulted by the assertions themselves).
  std::vector<OpEvent> events_;
  std::vector<uint32_t> skippedIndices_;
  size_t extractStreams_ = 0;
  size_t skippedStreams_ = 0;
  size_t unattributedFailures_ = 0;
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
  // C70: the two conjuncts of ok, so a red log says WHICH one was false.
  bool verdictOk = false;
  bool itemFailed = false;
  bool aborted = false;
  bool invalidArgument = false;
  size_t requested = 0;
  size_t catalogIndex = static_cast<size_t>(-1);
  // C70: the engine's per-item verdicts, attributed by the product probe. The
  // failure log prints them so "ok=false" can never again hide WHICH item the
  // engine failed and with what code.
  std::vector<win::ArchiveReader::ItemVerdict> items;
  size_t resultsWithoutItem = 0;
  CallbackTrace trace;
};

// One catalog index through its own collector: the byte witness (D2) plus the
// product's per-item verdict. Catalog indices, not payload slots, so the
// corrupt-neighbour fixture can reuse it.
SingleOutcome ExtractIndex(win::ArchiveReader& reader, size_t itemCount, size_t index,
                           BeginGate* gate) {
  auto* collector = new SweepCollector(itemCount, gate);
  const std::vector<uint32_t> indices{static_cast<uint32_t>(index)};
  const win::ArchiveReader::BatchVerdict verdict = reader.ExtractBatchVerdict(indices, collector);
  SingleOutcome out;
  out.sha = collector->sha(index);
  out.bytes = collector->bytes(index);
  // C50: judge the REQUESTED index. In a solid folder 7z also reports
  // SetOperationResult for the items it skipped on the way to ours, and a
  // blanket "an item failed" flag would blame this worker's payload for a
  // neighbour's status. The bytes + SHA-256 above remain the D2 evidence.
  // C70: the DECIDABLE criterion -- did the engine mark THIS index as failed?
  // The product attributes every SetOperationResult to the item 7-Zip opened
  // for it, so a skipped neighbour's kCRCError is a different entry and cannot
  // make this true. The collector below stays the byte witness (D2) only.
  out.itemFailed = verdict.itemFailed(static_cast<uint32_t>(index));
  out.ok = verdict.ok && !out.itemFailed;
  out.refused = verdict.refused;
  out.hr = static_cast<long>(verdict.hr);
  out.conflicts = verdict.conflicts;
  out.verdictOk = verdict.ok;
  out.aborted = verdict.aborted;
  out.invalidArgument = verdict.invalidArgument;
  out.requested = verdict.requested;
  out.catalogIndex = index;
  out.items = verdict.items.results;
  out.resultsWithoutItem = verdict.items.resultsWithoutItem;
  collector->Snapshot(&out.trace);
  collector->Release();
  return out;
}

SingleOutcome ExtractOne(win::ArchiveReader& reader, const Fixture& fx, size_t payloadIndex,
                         BeginGate* gate) {
  return ExtractIndex(reader, fx.catalog.size(), fx.catalogIndex[payloadIndex], gate);
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

// C70: the deterministic engine scenario behind this card, built once.
//
// A solid 7z over four 2 MiB payloads, with ONE byte of item 1's packed data
// flipped. Item 1 then fails its CRC while item 2 -- which the engine must
// decode item 1 to reach -- is byte-identical to its payload. 7-Zip reports
// code=3 (kCRCError) for index 1 and code=0 for index 2 (measured on 7z.dll
// 25.01, 2026-11-03, Tests/win/probe). That is the shape a verdict must not
// confuse: ONE item's code must never be charged to another item.
struct NeighbourFixture {
  std::unique_ptr<prism::testtmp::UniqueDir> scratch;
  std::wstring dir;
  std::wstring archivePath;
  win::SevenZipLibrary library;
  std::vector<ArchiveItem> catalog;
  std::vector<std::string> names;
  std::vector<std::string> expected;      // SHA-256 per payload
  std::vector<size_t> catalogIndex;       // payload order -> catalog index
  size_t corruptPayload = 1;              // the item whose CRC fails
  size_t probePayload = 2;                // the item we extract through it
  bool ready = false;
  std::string why;
};

NeighbourFixture& Neighbour() {
  static NeighbourFixture fixture;
  return fixture;
}

bool SetupNeighbourFixture() {
  NeighbourFixture& fx = Neighbour();
  if (fx.ready) return true;
  if (!fx.why.empty()) {
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  if (!SetupFixture()) {  // shares the DLL/CLI discovery and its failure text
    fx.why = "the base fixture is unavailable";
    return false;
  }
  const Fixture& base = World();
  fx.scratch = std::make_unique<prism::testtmp::UniqueDir>(L"c70", L"neighbour");
  if (!fx.scratch->ok()) {
    fx.why = "cannot create the fixture directory: " + fx.scratch->error();
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.dir = fx.scratch->path().wstring();
  fx.archivePath = (fx.scratch->path() / "neighbour.7z").wstring();

  fx.names = {"n0.bin", "n1.bin", "n2.bin", "n3.bin"};
  std::vector<std::wstring> wideNames;
  for (size_t i = 0; i < fx.names.size(); ++i) {
    const std::vector<uint8_t> payload = MakePayload(kSeedBase + 0x40u + static_cast<uint32_t>(i),
                                                     kItemBytes);
    std::ofstream file(fx.scratch->path() / fx.names[i], std::ios::binary);
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    if (!file.good()) {
      fx.why = "cannot write fixture payload " + fx.names[i];
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
    fx.expected.push_back(Sha256Hex(payload));
    wideNames.push_back(std::wstring(fx.names[i].begin(), fx.names[i].end()));
  }
  if (!prism::fixture::PackSolid(base.exePath, fx.dir, L"neighbour.7z", wideNames)) {
    fx.why = "7z.exe could not author the neighbour fixture";
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  // Flip one byte 1 KiB into item 1's packed data. A .7z is [32-byte signature
  // header][packed streams][header], and these payloads are incompressible
  // (LCG noise), so item 0's packed data is its own size: the byte lands inside
  // item 1 for every 7-Zip version the fixtures support.
  {
    std::fstream archive(fx.archivePath, std::ios::in | std::ios::out | std::ios::binary);
    const std::streamoff offset = 32 + static_cast<std::streamoff>(kItemBytes) + 1024;
    archive.seekg(offset);
    char byte = 0;
    archive.read(&byte, 1);
    if (!archive.good()) {
      fx.why = "cannot read the packed data of the neighbour fixture";
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
    byte = static_cast<char>(byte ^ 0xFF);
    archive.seekp(offset);
    archive.write(&byte, 1);
    archive.flush();
    if (!archive.good()) {
      fx.why = "cannot corrupt the neighbour fixture";
      std::cout << "    FAIL: " << fx.why << std::endl;
      return false;
    }
  }
  if (!fx.library.Load(base.dllPath)) {
    fx.why = "LoadLibrary(7z.dll) failed";
    return false;
  }
  win::ArchiveReader probe(fx.library);
  if (!probe.Open(fx.archivePath)) {
    fx.why = "probe reader could not open the neighbour fixture: " + probe.openTrace();
    std::cout << "    FAIL: " << fx.why << std::endl;
    return false;
  }
  fx.catalog = probe.ReadCatalog();
  if (fx.catalog.size() != fx.names.size()) {
    fx.why = "neighbour fixture catalog size mismatch";
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
#if defined(PRISM_C70_MUTANT_BLAME_REQUESTED)
  // D28: the banner sits against the guard so the mutation matrix can prove the
  // injection was compiled in; D27: the verdict is armed with the runtime
  // predicate "the injection actually fired", so an unrelated red cannot print
  // CAUGHT (D3) and a dead injection cannot fake it.
  std::cout << "C70 MUTANT ACTIVE: every item verdict is charged to the requested index"
            << std::endl;
  prism::test::SetMutationVerdict("C70 MUTATION CAUGHT: blame_requested", [] {
    return win::ArchiveReader::MutantBlameHits() != 0;
  });
#endif
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
  const size_t kRoundCount = FourWayRounds();
  auto pool = MakePool(fx.names.size());
  if (!PRISM_CHECK(pool != nullptr)) return;
  PRISM_CHECK_EQ(pool->stats().handlers, fx.names.size());
  PRISM_CHECK_EQ(pool->capacity(), fx.names.size());

  if (kRoundCount > kDefaultFourWayRounds) {
    std::cout << "    stress: " << kRoundCount << " four-way rounds (PRISM_C70_STRESS_ROUNDS)"
              << std::endl;
  }
  for (size_t round = 0; round < kRoundCount; ++round) {
    if (kRoundCount > kDefaultFourWayRounds && round != 0 && round % 50 == 0) {
      std::cout << "    stress: round " << round << "/" << kRoundCount << " clean so far"
                << std::endl;
    }
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
    std::vector<SingleOutcome> outcomes(fx.names.size());
    std::vector<std::thread> workers;
    for (size_t i = 0; i < fx.names.size(); ++i) {
      workers.emplace_back([&, i] {
        // No pre-call gate: the gate is joined from inside GetStream, so the
        // peak below proves four engine operations really overlapped.
        live.Enter();
        outcomes[i] = ExtractOne(*leases[i]->get(), fx, i, &gate);
        live.Leave();
      });
    }
    for (std::thread& worker : workers) worker.join();
    PRISM_CHECK(!gate.timedOut.load());
    PRISM_CHECK_EQ(live.peak.load(), static_cast<int>(fx.names.size()));
    // C70: PRISM_C70_TRACE=1 dumps the engine's callback sequence for round 0
    // even when it is green -- the baseline shape has to be known before a
    // one-off red can be read.
    static const bool kTraceFirstRound = std::getenv("PRISM_C70_TRACE") != nullptr;
    if (kTraceFirstRound && round == 0) {
      for (size_t i = 0; i < outcomes.size(); ++i) {
        std::cout << "    trace worker " << i << " catalogIndex=" << outcomes[i].catalogIndex
                  << " ok=" << (outcomes[i].ok ? 1 : 0) << " bytes=" << outcomes[i].bytes
                  << std::endl;
        outcomes[i].trace.Print(outcomes[i].catalogIndex);
      }
    }
    for (size_t i = 0; i < fx.names.size(); ++i) {
      const SingleOutcome& one = outcomes[i];
      if (!one.ok) {
        // C70: the log has to say WHICH conjunct of ok was false and what the
        // engine actually reported. verdictOk=false is the engine's HRESULT;
        // itemFailed=true is a per-item code, and the callback trace below says
        // whether that code arrived on the requested index with the full
        // payload already written (our item) or with zero bytes (a neighbour).
        std::cout << "    worker " << i << " item=" << fx.names[i]
                  << " catalogIndex=" << one.catalogIndex << " bytes=" << one.bytes
                  << " sha=" << one.sha << " expected=" << fx.expected[i]
                  << " ok=" << (one.ok ? 1 : 0) << " verdictOk=" << (one.verdictOk ? 1 : 0)
                  << " itemFailed=" << (one.itemFailed ? 1 : 0)
                  << " refused=" << (one.refused ? 1 : 0)
                  << " aborted=" << (one.aborted ? 1 : 0)
                  << " invalidArgument=" << (one.invalidArgument ? 1 : 0)
                  << " requested=" << one.requested << " conflicts=" << one.conflicts
                  << " hr=0x" << std::hex << one.hr << std::dec << std::endl;
        one.trace.Print(one.catalogIndex);
        // C70: the product's attribution, one line per SetOperationResult the
        // engine sent. "code=0 (THIS worker)" with a matching SHA-256 is the
        // green shape; a non-zero code on the requested index is the engine
        // failing an item it fully decoded (a different bug, reported loudly).
        for (const auto& item : one.items) {
          std::cout << "      itemVerdict index=" << item.index << " code=" << item.code << " "
                    << DescribeOpResult(item.code) << " askMode=" << item.askMode
                    << " hadStream=" << (item.hadStream ? 1 : 0) << std::endl;
        }
        if (one.resultsWithoutItem != 0) {
          std::cout << "      resultsWithoutItem=" << one.resultsWithoutItem << std::endl;
        }
        PrintPoolDiagnostics(*pool, "four-way worker failure");
      }
      PRISM_CHECK(one.ok);
      PRISM_CHECK_EQ(one.bytes, kItemBytes);
      if (!kSharedHandlerMutant) PRISM_CHECK_EQ(one.sha, fx.expected[i]);
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

// C70: THE ENGINE'S CODE FOR A SKIPPED NEIGHBOUR MUST NOT CONVICT OUR ITEM.
//
// Extracting item 2 of a solid block forces 7-Zip to decode item 1 first. When
// item 1 is corrupt the engine reports SetOperationResult(kCRCError) for item 1
// and kOK for item 2 (measured, 7z.dll 25.01). The verdict must therefore name
// item 1, and item 2 must still be accepted with its full payload -- which is
// the decidable criterion this card ships: "the code belongs to the item the
// engine opened for it", not "the last item somebody asked for".
//
// Deterministic by construction: the fixture is one flipped byte at a fixed
// offset, no timing and no concurrency. The negative control is the mutant
// build PRISM_C70_MUTANT_BLAME_REQUESTED, which charges every code to the
// requested index; it must go RED here (mutation.reader_pool.blame_requested).
PRISM_TEST("reader_pool.skipped_neighbour_error_is_not_our_failure") {
  if (!PRISM_CHECK(SetupNeighbourFixture())) return;
  NeighbourFixture& fx = Neighbour();
  const size_t corruptIndex = fx.catalogIndex[fx.corruptPayload];
  const size_t probeIndex = fx.catalogIndex[fx.probePayload];
  PRISM_CHECK(corruptIndex != probeIndex);

  win::ReaderPool::Config config;
  config.archivePath = fx.archivePath;
  config.capacity = 1;
  std::string error;
  auto pool = win::ReaderPool::Create(fx.library, config, &error);
  if (!PRISM_CHECK(pool != nullptr)) {
    std::cout << "    ReaderPool::Create failed: " << error << std::endl;
    return;
  }
  auto lease = pool->Acquire();
  if (!PRISM_CHECK(lease.has_value())) return;

  const SingleOutcome one = ExtractIndex(*lease->get(), fx.catalog.size(), probeIndex, nullptr);
  // D2: the bytes are the evidence, and they are intact -- the corruption is in
  // the neighbour, not in what we asked for.
  PRISM_CHECK_EQ(one.bytes, kItemBytes);
  PRISM_CHECK_EQ(one.sha, fx.expected[fx.probePayload]);
  // D18: the neighbour's code has to be REAL for this test to mean anything.
  int32_t neighbourCode = -1;
  for (const auto& item : one.items) {
    if (item.index == static_cast<uint32_t>(corruptIndex)) neighbourCode = item.code;
  }
  if (!PRISM_CHECK(neighbourCode != -1)) {
    std::cout << "    the engine never reported the skipped neighbour; items=" << one.items.size()
              << std::endl;
    one.trace.Print(probeIndex);
  } else {
    PRISM_CHECK(neighbourCode != 0);
  }
  // THE CONTRACT: the neighbour's failure is not ours.
  if (one.itemFailed) {
    std::cout << "    worker item=" << fx.names[fx.probePayload] << " bytes=" << one.bytes
              << " sha=" << one.sha << " expected=" << fx.expected[fx.probePayload]
              << " verdictOk=" << (one.verdictOk ? 1 : 0)
              << " itemFailed=" << (one.itemFailed ? 1 : 0) << " catalogIndex=" << probeIndex
              << std::endl;
    for (const auto& item : one.items) {
      std::cout << "      itemVerdict index=" << item.index << " code=" << item.code << " "
                << DescribeOpResult(item.code) << " askMode=" << item.askMode
                << " hadStream=" << (item.hadStream ? 1 : 0) << std::endl;
    }
    one.trace.Print(probeIndex);
  }
  PRISM_CHECK(!one.itemFailed);
  PRISM_CHECK(one.ok);
  lease->Release();
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
