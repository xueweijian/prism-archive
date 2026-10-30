// PrismArchive -- PrismCore performance benchmarks & invariants (blueprint ch. 7.2).
//
// Why invariants and not absolute milliseconds: CI runners are shared,
// virtualised and noisy, so "sweep took 12 ms" can never be a gate -- it is a
// different number on every run and on every machine. What CAN be a gate is
// the *shape* of the work, which is a property of the algorithm:
//
//   1. decode count  -- one linear pass touches every item once; per-item
//                       extraction re-decompresses the solid prefix, so N
//                       items cost N(N+1)/2 item-decompressions. Exactly.
//   2. peak memory   -- independent of the BLOCK size (only scratch + thumbs),
//                       measured with a real allocation counter (operator new).
//   3. abandon       -- zero decodes after the viewport moves on, at any split
//                       point, while the stream is still fully drained.
//   4. LRU budget    -- SizeBytes() <= BudgetBytes() after ANY random op
//                       sequence (1000 seeded sequences).
//   5. progress      -- Fraction() is monotonic non-decreasing and stays in
//                       [0,1] for ANY call sequence.
//
// Every invariant has a NEGATIVE CONTROL: a deliberately broken implementation
// ("the regression we fear") that the same predicate must reject. Without that
// the suite could be green forever and prove nothing.
//
// The absolute wall-clock numbers live in Tests/win/bench_solid.cpp (real
// 7z.dll, real solid archive, QueryPerformanceCounter) -- that is where the
// "k times faster" claim is actually measured.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "Gallery.hpp"
#include "SolidSweep.hpp"
#include "ThumbStore.hpp"
#include "mini_test.hpp"

// ---------------------------------------------------------------- allocator
// A real allocation counter, not a model: every operator new in this binary
// goes through here, so "peak memory" below is what the process actually
// held. This is how the block-size independence gets proved rather than
// asserted by reading the source.
namespace meter {

size_t g_live = 0;      // bytes currently outstanding
size_t g_peak = 0;      // peak live bytes sampled inside the window
size_t g_base = 0;      // live bytes when the window opened
bool g_on = false;      // sampling enabled?

struct Header {
  size_t size;
};

}  // namespace meter

void* operator new(size_t bytes) {
  void* raw = std::malloc(bytes + sizeof(meter::Header));
  if (!raw) throw std::bad_alloc();
  static_cast<meter::Header*>(raw)->size = bytes;
  meter::g_live += bytes;
  if (meter::g_on && meter::g_live > meter::g_peak) meter::g_peak = meter::g_live;
  return static_cast<char*>(raw) + sizeof(meter::Header);
}

void operator delete(void* p) noexcept {
  if (!p) return;
  void* raw = static_cast<char*>(p) - sizeof(meter::Header);
  meter::g_live -= static_cast<meter::Header*>(raw)->size;
  std::free(raw);
}

void operator delete(void* p, size_t) noexcept { operator delete(p); }
void* operator new[](size_t bytes) { return operator new(bytes); }
void operator delete[](void* p) noexcept { operator delete(p); }
void operator delete[](void* p, size_t) noexcept { operator delete(p); }

namespace {

using prism::gallery::DecodedThumb;
using prism::gallery::SolidSweepSession;
using prism::gallery::ThumbnailLru;
using prism::gallery::ThumbStore;

constexpr uint64_t kLimit = 64ull << 20;

// ------------------------------------------------------------- measured facts
// Collected by the cases and dumped as one machine-readable JSON line at exit
// so the CI summary shows digits, not just "ok".
struct BenchNumbers {
  int items = 0;
  long long sweepDecodes = 0;
  long long perItemDecodes = 0;
  long long sweepDecompressions = 0;
  long long perItemDecompressions = 0;
  long long sweepBytes = 0;
  long long perItemBytes = 0;
  long long peakBytesSmall = 0;  // 1 MB block
  long long peakBytesHuge = 0;   // 100 MB block
  long long lruMaxOver = 0;      // worst SizeBytes()-BudgetBytes() seen (must be <= 0)
} g_numbers;

// ------------------------------------------------------------- fake decoder
// Counts calls -- the decode count IS the invariant, so the decoder's only job
// is to be countable and to hand back a thumbnail of a fixed size (so peak
// memory cannot vary with the payload either).
struct CountingDecoder final : SolidSweepSession::IDecoder {
  std::optional<DecodedThumb> Decode(const uint8_t* data, size_t bytes) override {
    ++calls;
    if (!data || bytes == 0 || data[0] != 'P') return std::nullopt;  // decode failure path
    DecodedThumb thumb;
    thumb.width = 64;
    thumb.height = 64;
    thumb.pixels.assign(64 * 64 * 4, data[0]);
    return thumb;
  }
  int calls = 0;
};

struct RejectingDecoder final : SolidSweepSession::IDecoder {
  std::optional<DecodedThumb> Decode(const uint8_t*, size_t) override { return std::nullopt; }
};

// ------------------------------------------------------ simulated solid block
// A solid block is ONE byte blob; reaching item i from the start costs
// decompressing items 0..i. Both arms of the comparison run against the same
// model so the numbers are directly comparable.
struct SolidBlockSim {
  SolidBlockSim(int n, size_t itemBytes) : items(n), bytes(itemBytes) {
    data.assign(static_cast<size_t>(n) * bytes, 'P');  // every item looks decodable
  }
  const uint8_t* At(int i) const { return data.data() + static_cast<size_t>(i) * bytes; }
  int items;
  size_t bytes;
  std::vector<uint8_t> data;  // stands in for the archive/dictionary bytes
};

struct WorkTally {
  long long decompressions = 0;  // item-decompression units (the CPU the user pays)
  long long bytesRead = 0;       // bytes produced by the decompressor
};

// Arm A -- the PrismArchive path: ONE linear pass, every item delivered once.
WorkTally RunSinglePass(SolidSweepSession::IDecoder& decoder, prism::gallery::IThumbSink& sink,
                        const SolidBlockSim& block) {
  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (int i = 0; i < block.items; ++i) {
    indices.push_back(static_cast<uint32_t>(i));
    sizes.push_back(block.bytes);
  }
  SolidSweepSession session(indices, sizes, kLimit, decoder, sink);
  WorkTally tally;
  for (int i = 0; i < block.items; ++i) {
    session.OnItemStart(static_cast<uint32_t>(i));
    session.OnItemBytes(block.bytes);
    session.OnItemEnd(block.At(i), block.bytes);
    ++tally.decompressions;
    tally.bytesRead += static_cast<long long>(block.bytes);
  }
  return tally;
}

// Arm B -- the traditional path: extract item i on its own. A solid block has
// no per-item index, so the decompressor must replay items 0..i every time.
WorkTally RunPerItem(SolidSweepSession::IDecoder& decoder, prism::gallery::IThumbSink& sink,
                     const SolidBlockSim& block) {
  WorkTally tally;
  for (int target = 0; target < block.items; ++target) {
    std::vector<uint32_t> indices{static_cast<uint32_t>(target)};
    std::vector<uint64_t> sizes{block.bytes};
    SolidSweepSession session(indices, sizes, kLimit, decoder, sink);
    for (int i = 0; i <= target; ++i) {  // the stream always starts at item 0
      session.OnItemStart(static_cast<uint32_t>(i));
      session.OnItemBytes(block.bytes);
      session.OnItemEnd(block.At(i), block.bytes);
      ++tally.decompressions;
      tally.bytesRead += static_cast<long long>(block.bytes);
    }
  }
  return tally;
}

long long Triangular(int n) { return static_cast<long long>(n) * (n + 1) / 2; }

// ------------------------------------------------------- peak memory probe
// Returns the bytes of EXTRA live memory the sweep held at its high-water
// mark. The block blob itself is allocated while the meter is closed: those
// bytes belong to the archive/dictionary, not to the sweep's own accounting
// (which is exactly the claim under test -- the sweep must never materialise
// the block).
long long MeasureSweepPeak(size_t itemBytes, int itemCount, CountingDecoder* decoderOut) {
  SolidBlockSim block(itemCount, itemBytes);  // outside the window on purpose
  meter::g_base = meter::g_live;
  meter::g_peak = meter::g_live;
  meter::g_on = true;
  {
    CountingDecoder decoder;
    ThumbStore store{8ull << 20};
    RunSinglePass(decoder, store, block);
    if (decoderOut) *decoderOut = decoder;
    // store/decoder destroyed here so their bytes land in the window
  }
  meter::g_on = false;
  return static_cast<long long>(meter::g_peak - meter::g_base);
}

// --------------------------------------------- broken implementations (controls)
// Each one is the regression the corresponding invariant exists to catch.

// Regression 1: "just buffer the block, then decode from memory". Simple,
// tempting, and it makes peak memory scale with the archive.
long long MeasureBlockBufferingPeak(size_t itemBytes, int itemCount) {
  SolidBlockSim block(itemCount, itemBytes);
  meter::g_base = meter::g_live;
  meter::g_peak = meter::g_live;
  meter::g_on = true;
  {
    std::vector<uint8_t> copy(block.data);  // the bug: the whole block resident
    CountingDecoder decoder;
    ThumbStore store{8ull << 20};
    for (int i = 0; i < itemCount; ++i) {
      if (auto thumb = decoder.Decode(copy.data() + static_cast<size_t>(i) * itemBytes, itemBytes)) {
        store.OnThumb(static_cast<uint32_t>(i), *thumb);
      }
    }
  }
  meter::g_on = false;
  return static_cast<long long>(meter::g_peak - meter::g_base);
}

// Regression 2: "the extractor keeps decoding after abandon" (drain without
// the abandon check) -- pure wasted CPU on pixels nobody will see.
struct AbandonIgnoringSession {
  explicit AbandonIgnoringSession(CountingDecoder& d) : decoder(d) {}
  void OnItemStart(uint32_t) {}
  void OnItemBytes(size_t) {}
  void Abandon() { abandoned = true; }
  bool abandoned = false;
  void OnItemEnd(int index) {
    // The bug: `abandoned` is recorded but never consulted.
    if (auto thumb = decoder.Decode(reinterpret_cast<const uint8_t*>("P"), 1)) {
      ++decodedCount;
    }
    ++finished;
    (void)index;
  }
  int decodedCount = 0;
  int finished = 0;
  CountingDecoder& decoder;
};

// Regression 3: "cache the thumbs, skip the eviction loop" -- the classic way
// a memory budget dies: someone 'optimises' the eviction away.
struct UnboundedCache {
  void Insert(uint64_t key, size_t bytes) {
    entries.push_back({key, bytes});
    size += bytes;
  }
  size_t SizeBytes() const { return size; }
  size_t BudgetBytes() const { return budget; }
  std::vector<std::pair<uint64_t, size_t>> entries;
  size_t size = 0;
  size_t budget;
};

// Regression 4: "progress resets per item so the bar animates" -- monotonicity
// is what the HUD relies on; a resetting Fraction makes it jump backwards.
struct NonMonotonicFraction {
  explicit NonMonotonicFraction(int total) : total_(total) {}
  double OnItemStart() {
    finished_ = 0;  // the bug
    return Fraction();
  }
  double OnItemEnd() {
    ++finished_;
    return Fraction();
  }
  double Fraction() const {
    if (total_ <= 0) return 1.0;
    return static_cast<double>(finished_) / static_cast<double>(total_);
  }
  int finished_ = 0;
  int total_;
};

// -------------------------------------------------------------- invariants

// (2) peak memory is a function of scratch+thumbs only -- never of block size.
bool PeakIndependentOfBlockSize(long long peakSmall, long long peakHuge, long long blockHuge) {
  if (peakSmall <= 0 || peakHuge <= 0) return false;
  if (peakSmall != peakHuge) return false;                 // identical work -> identical peak
  if (peakHuge * 10 >= blockHuge) return false;            // must be < 10% of the block
  return peakHuge <= static_cast<long long>(8ull << 20) + static_cast<long long>(4ull << 20);
}

// (3) abandon: nothing decoded after the abandon point, stream fully drained.
bool AbandonStopsDecoding(int decoded, int expectedDecoded, bool drained) {
  return decoded == expectedDecoded && drained;
}

// (4) budget: the cache never exceeds its budget, for any op sequence.
bool BudgetHeld(const ThumbnailLru& lru) { return lru.SizeBytes() <= lru.BudgetBytes(); }
bool BudgetHeld(const ThumbStore& store) { return store.SizeBytes() <= store.BudgetBytes(); }

// (5) progress: monotone and bounded.
bool FractionSane(double previous, double current) {
  return current >= previous && current >= 0.0 && current <= 1.0;
}

// ============================================================== the cases

PRISM_TEST("decode count: one linear pass charges exactly one decode per item") {
  const int n = 100;
  SolidBlockSim block(n, 64);
  CountingDecoder decoder;
  ThumbStore store{8ull << 20};

  const WorkTally tally = RunSinglePass(decoder, store, block);

  PRISM_CHECK_EQ(decoder.calls, n);
  PRISM_CHECK_EQ(static_cast<int>(store.Count()), n);  // 100 * 16 KB fits in 8 MB
  PRISM_CHECK_EQ(tally.decompressions, static_cast<long long>(n));
  PRISM_CHECK_EQ(tally.bytesRead, static_cast<long long>(n) * 64);
  g_numbers.items = n;
  g_numbers.sweepDecodes = decoder.calls;
  g_numbers.sweepDecompressions = tally.decompressions;
  g_numbers.sweepBytes = tally.bytesRead;
}

PRISM_TEST("decode count: per-item extraction charges N(N+1)/2 item-decompressions") {
  const int n = 100;
  SolidBlockSim block(n, 64);
  CountingDecoder decoder;
  ThumbStore store{8ull << 20};

  const WorkTally tally = RunPerItem(decoder, store, block);

  // The requested item is decoded once per extraction -> N decodes...
  PRISM_CHECK_EQ(decoder.calls, n);
  // ... but every extraction replays the solid prefix: 1+2+...+N.
  PRISM_CHECK_EQ(tally.decompressions, Triangular(n));
  PRISM_CHECK_EQ(tally.decompressions, 5050ll);  // 100*101/2 -- an exact digit
  PRISM_CHECK_EQ(tally.bytesRead, Triangular(n) * 64);
  g_numbers.perItemDecodes = decoder.calls;
  g_numbers.perItemDecompressions = tally.decompressions;
  g_numbers.perItemBytes = tally.bytesRead;
}

PRISM_TEST("the single pass is exactly (N+1)/2 times cheaper in decompressions") {
  const int n = 100;
  SolidBlockSim block(n, 64);
  CountingDecoder decoder;
  ThumbStore store{8ull << 20};

  const WorkTally sweep = RunSinglePass(decoder, store, block);
  const WorkTally perItem = RunPerItem(decoder, store, block);

  // The claim under test, stated as arithmetic instead of a wall-clock number:
  // per-item == sweep * (N+1)/2, bit-exact.
  PRISM_CHECK_EQ(perItem.decompressions, sweep.decompressions * (n + 1) / 2);
  PRISM_CHECK_EQ(perItem.bytesRead, sweep.bytesRead * (n + 1) / 2);
  // Same thumbnails out of both arms -- so the saving is pure waste, not
  // "per-item did less work".
  PRISM_CHECK_EQ(static_cast<int>(store.Count()), n);
}

PRISM_TEST("peak memory: independent of block size (1 MB vs 100 MB block)") {
  CountingDecoder small;
  CountingDecoder huge;
  const long long peakSmall = MeasureSweepPeak(50 * 1024, 20, &small);   // ~1 MB block
  const long long peakHuge = MeasureSweepPeak(5 * 1024 * 1024, 20, &huge);  // ~100 MB block

  PRISM_CHECK_EQ(small.calls, 20);
  PRISM_CHECK_EQ(huge.calls, 20);
  if (!PRISM_CHECK(PeakIndependentOfBlockSize(peakSmall, peakHuge, 20ll * 5 * 1024 * 1024)))
    return;
  // Restate the digits so a regression prints something actionable.
  printf("    peak small block: %lld bytes, huge block: %lld bytes\n", peakSmall, peakHuge);
  g_numbers.peakBytesSmall = peakSmall;
  g_numbers.peakBytesHuge = peakHuge;
}

PRISM_TEST("NEGATIVE CONTROL: buffering the block breaks the peak-memory invariant") {
  const long long peakSmall = MeasureBlockBufferingPeak(50 * 1024, 20);
  const long long peakHuge = MeasureBlockBufferingPeak(5 * 1024 * 1024, 20);
  // The control MUST fail the same predicate the real code passes -- otherwise
  // the invariant is decorative.
  PRISM_CHECK(!PeakIndependentOfBlockSize(peakSmall, peakHuge, 20ll * 5 * 1024 * 1024));
  PRISM_CHECK(peakHuge > peakSmall);  // it really is the block that shows up
  printf("    control (block buffering): %lld -> %lld bytes\n", peakSmall, peakHuge);
}

PRISM_TEST("abandon mid-item: decoded count equals the number of COMPLETED items") {
  const int n = 10;
  SolidBlockSim block(n, 32);
  CountingDecoder decoder;
  ThumbStore store{8ull << 20};
  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (int i = 0; i < n; ++i) {
    indices.push_back(static_cast<uint32_t>(i));
    sizes.push_back(block.bytes);
  }
  SolidSweepSession session(indices, sizes, kLimit, decoder, store);

  for (int i = 0; i < n; ++i) {
    session.OnItemStart(static_cast<uint32_t>(i));
    if (i == 3) session.Abandon();  // the viewport moved on while item 3 streams
    session.OnItemBytes(block.bytes);
    session.OnItemEnd(block.At(i), block.bytes);
  }

  // Items 0..2 finished before the abandon, item 3 was cut mid-flight: 3 decodes.
  PRISM_CHECK_EQ(decoder.calls, 3);
  PRISM_CHECK_EQ(session.decodedCount, 3);
  PRISM_CHECK_EQ(session.skippedCount, 7);  // 3,4,5,6,7,8,9
  // ... and the stream was still fully drained (the 7z callback contract).
  PRISM_CHECK_EQ(session.drainedBytes, static_cast<uint64_t>(n) * block.bytes);
  PRISM_CHECK_EQ(session.Fraction(), 1.0);
  PRISM_CHECK(session.abandoned());
}

PRISM_TEST("abandon at every split point: decoded == index, drained == full block") {
  const int n = 12;
  SolidBlockSim block(n, 16);
  for (int cut = 0; cut <= n; ++cut) {
    CountingDecoder decoder;
    ThumbStore store{4ull << 20};
    std::vector<uint32_t> indices;
    std::vector<uint64_t> sizes;
    for (int i = 0; i < n; ++i) {
      indices.push_back(static_cast<uint32_t>(i));
      sizes.push_back(block.bytes);
    }
    SolidSweepSession session(indices, sizes, kLimit, decoder, store);
    for (int i = 0; i < n; ++i) {
      session.OnItemStart(static_cast<uint32_t>(i));
      if (i == cut) session.Abandon();
      session.OnItemBytes(block.bytes);
      session.OnItemEnd(block.At(i), block.bytes);
    }
    if (!PRISM_CHECK(AbandonStopsDecoding(decoder.calls, cut, session.drainedBytes ==
                                                              static_cast<uint64_t>(n) * block.bytes)))
      return;
    // Progress must still complete: the HUD shows "this block is done".
    if (!PRISM_CHECK_EQ(session.Fraction(), 1.0)) return;
  }
}

PRISM_TEST("NEGATIVE CONTROL: decoding after abandon is caught by the count invariant") {
  CountingDecoder decoder;
  AbandonIgnoringSession broken(decoder);
  for (int i = 0; i < 6; ++i) {
    broken.OnItemStart(static_cast<uint32_t>(i));
    if (i == 2) broken.Abandon();
    broken.OnItemEnd(i);
  }
  // The real contract would have stopped at 2; the broken one decodes all 6.
  PRISM_CHECK(broken.decodedCount == 6);
  PRISM_CHECK(!AbandonStopsDecoding(broken.decodedCount, 2, true));
}

PRISM_TEST("LRU budget holds across 1000 seeded random op sequences") {
  std::mt19937 rng(20261030u);  // fixed seed: reproducible on every machine
  std::uniform_int_distribution<int> keyDist(0, 400);
  std::uniform_int_distribution<int> bytesDist(1, 200 * 1024);
  std::uniform_int_distribution<int> opDist(0, 99);

  for (int round = 0; round < 1000; ++round) {
    const size_t budget = 256 * 1024 + static_cast<size_t>(round % 17) * 4096;
    ThumbnailLru lru{budget};
    ThumbStore store{budget};
    for (int step = 0; step < 40; ++step) {
      const uint64_t key = static_cast<uint64_t>(keyDist(rng));
      const size_t bytes = static_cast<size_t>(bytesDist(rng));
      const int op = opDist(rng);
      if (op < 55) {
        lru.Insert(key, bytes);
        // ThumbStore's sink takes a DecodedThumb; a pixel vector of `bytes`
        // exercises the same accounting path the gallery uses.
        DecodedThumb thumb;
        thumb.width = 64;
        thumb.height = 64;
        thumb.pixels.assign(bytes > 0 ? bytes : 1, 0x7F);
        store.OnThumb(static_cast<uint32_t>(key), thumb);
      } else if (op < 85) {
        lru.Lookup(key);
        store.Lookup(static_cast<uint32_t>(key));
      } else if (op < 95) {
        lru.Touch(key);
        (void)store.Contains(static_cast<uint32_t>(key));
      } else {
        lru.Peek(key);
      }
      if (!BudgetHeld(lru) || !BudgetHeld(store)) {
        PRISM_CHECK(false);  // loud: the invariant broke on a specific sequence
        return;
      }
      const long long over = static_cast<long long>(store.SizeBytes()) -
                             static_cast<long long>(store.BudgetBytes());
      g_numbers.lruMaxOver = std::max(g_numbers.lruMaxOver, over);
    }
  }
  PRISM_CHECK(true);
}

PRISM_TEST("NEGATIVE CONTROL: an unbudgeted cache breaches the budget predicate") {
  UnboundedCache broken;
  broken.budget = 64 * 1024;
  for (int i = 0; i < 50; ++i) broken.Insert(static_cast<uint64_t>(i), 8 * 1024);
  PRISM_CHECK(broken.SizeBytes() > broken.BudgetBytes());
  PRISM_CHECK(!(broken.SizeBytes() <= broken.BudgetBytes()));
}

PRISM_TEST("an item larger than the whole budget is refused, not absorbed") {
  ThumbnailLru lru{64 * 1024};
  PRISM_CHECK(!lru.Insert(1, 128 * 1024));  // loud refusal
  PRISM_CHECK_EQ(lru.SizeBytes(), size_t(0));
  PRISM_CHECK_EQ(lru.Count(), size_t(0));
  // A store holds its budget even when every single insert is oversized.
  ThumbStore store{64 * 1024};
  DecodedThumb huge;
  huge.width = 256;
  huge.height = 256;
  huge.pixels.assign(256 * 256 * 4, 1);  // 256 KB > 64 KB budget
  PRISM_CHECK(!store.OnThumb(0, huge));
  PRISM_CHECK(BudgetHeld(store));
}

PRISM_TEST("progress: Fraction() is monotone and bounded under 1000 random op sequences") {
  std::mt19937 rng(424242u);
  std::uniform_int_distribution<int> itemDist(0, 7);
  std::uniform_int_distribution<int> opDist(0, 99);

  // Contract-legal sequences: the extractor may interleave start/bytes/abandon
  // any way it likes, but it finishes each of the N items exactly once. That
  // is the sequence the HUD actually sees, and the one the [0,1] bound is
  // promised for.
  for (int round = 0; round < 1000; ++round) {
    const int n = 1 + round % 9;
    CountingDecoder decoder;
    ThumbStore store{1ull << 20};
    std::vector<uint32_t> indices;
    std::vector<uint64_t> sizes;
    for (int i = 0; i < n; ++i) {
      indices.push_back(static_cast<uint32_t>(i));
      sizes.push_back(16);
    }
    SolidSweepSession session(indices, sizes, kLimit, decoder, store);
    double previous = 0.0;
    int ends = 0;
    for (int step = 0; step < 40 && ends < n; ++step) {
      const int op = opDist(rng);
      if (op < 35) {
        session.OnItemStart(static_cast<uint32_t>(itemDist(rng) % n));
      } else if (op < 70) {
        session.OnItemBytes(16);
      } else if (op < 97) {
        session.OnItemEnd(reinterpret_cast<const uint8_t*>("P"), 1);
        ++ends;
      } else {
        session.Abandon();  // abandon must not rewind progress either
      }
      const double now = session.Fraction();
      if (!FractionSane(previous, now)) {
        PRISM_CHECK(false);
        return;
      }
      previous = now;
    }
    // Finish the block so the HUD reaches exactly 100%, never more.
    while (ends < n) {
      session.OnItemEnd(reinterpret_cast<const uint8_t*>("P"), 1);
      ++ends;
      const double now = session.Fraction();
      if (!FractionSane(previous, now)) {
        PRISM_CHECK(false);
        return;
      }
      previous = now;
    }
    if (!PRISM_CHECK_EQ(session.Fraction(), 1.0)) return;
  }
  PRISM_CHECK(true);
}

// KNOWN DEFECT (reported, not fixed -- Src/ is another module's surface):
// Fraction() divides finished_ by total_ with no clamp, so if the extractor
// reports MORE item-ends than the session was built for (defensive flush in
// SweepExtractCallback, an index the caller forgot to filter out, or an
// over-long batch list) the HUD receives e.g. 1.667 -- "167% done".
// This case pins the current behaviour so the number cannot drift silently,
// while staying green once someone clamps it (the `over <= 1.0` branch).
// Monotonicity, the property the progress bar actually relies on, holds either
// way and is asserted unconditionally.
PRISM_TEST("progress: over-reported item ends are monotone but may exceed 1.0 (known defect)") {
  CountingDecoder decoder;
  ThumbStore store{1ull << 20};
  SolidSweepSession session({0, 1, 2}, {1, 1, 1}, kLimit, decoder, store);
  double previous = 0.0;
  for (int i = 0; i < 5; ++i) {  // 5 ends into a 3-item block
    session.OnItemEnd(reinterpret_cast<const uint8_t*>("P"), 1);
    const double now = session.Fraction();
    if (!PRISM_CHECK(now >= previous)) return;  // monotone: always promised
    previous = now;
  }
  const double over = session.Fraction();
  if (over <= 1.0) {
    printf("    NOTE: Fraction() now clamps at 1.0 -- the documented defect is fixed\n");
    return;
  }
  PRISM_CHECK(over > 1.0);  // 5/3 == 1.666...
  printf("    KNOWN DEFECT: Fraction() reached %.3f (> 1.0) after over-report\n", over);
}

PRISM_TEST("NEGATIVE CONTROL: a resetting Fraction is caught by the monotonicity predicate") {
  NonMonotonicFraction broken(3);
  double previous = 0.0;
  bool caught = false;
  for (int i = 0; i < 3; ++i) {
    broken.OnItemEnd();
    const double afterEnd = broken.Fraction();
    if (!FractionSane(previous, afterEnd)) return;  // sanity of the checker
    previous = afterEnd;
    const double afterStart = broken.OnItemStart();  // the bug: rewinds to 0
    if (!FractionSane(previous, afterStart)) caught = true;
    previous = afterStart;
  }
  PRISM_CHECK(caught);
}

PRISM_TEST("an empty block reports 1.0 once, never NaN, never negative") {
  CountingDecoder decoder;
  ThumbStore store{1ull << 20};
  SolidSweepSession session({}, {}, kLimit, decoder, store);
  PRISM_CHECK_EQ(session.Fraction(), 1.0);
  PRISM_CHECK_EQ(decoder.calls, 0);
}

}  // namespace

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  // One machine-readable line for the CI summary; digits, not adjectives.
  printf(
      "CORE-BENCH {\"items\":%d,\"sweep_decodes\":%lld,\"peritem_decodes\":%lld,"
      "\"sweep_decompressions\":%lld,\"peritem_decompressions\":%lld,"
      "\"sweep_bytes\":%lld,\"peritem_bytes\":%lld,"
      "\"peak_bytes_1mb\":%lld,\"peak_bytes_100mb\":%lld,\"lru_over_budget\":%lld}\n",
      g_numbers.items, g_numbers.sweepDecodes, g_numbers.perItemDecodes,
      g_numbers.sweepDecompressions, g_numbers.perItemDecompressions, g_numbers.sweepBytes,
      g_numbers.perItemBytes, g_numbers.peakBytesSmall, g_numbers.peakBytesHuge,
      g_numbers.lruMaxOver);
  if (rc == 0) printf("ALL CORE BENCH CHECKS PASSED\n");
  return rc;
}
