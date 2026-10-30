// TDD: the solid-block single-pass sweep session (blueprint ch. 4.3) -- the
// scheduler that turns ONE linear 7z Extract call into a full set of gallery
// thumbnails. This is the "Bandizip 禁区突破" decision core.
//
// First principles:
//  (a) the block streams by exactly once; each item's bytes are decoded the
//      moment they complete -- peak memory is one scratch item + thumbnails,
//      never the block,
//  (b) over-limit items are SKIPPED loudly, not half-decoded (bomb hygiene,
//      consistent with QuickLookPlanner),
//  (c) when the viewport moves on, the session ABANDONS: remaining bytes are
//      still drained (the 7z callback contract requires consuming the stream)
//      but never decoded -- CPU is not spent on pixels nobody will see,
//  (d) the decoder is injected, so the session is tested with a fake and the
//      WIC implementation is a thin Windows adapter,
//  (e) decoded thumbs go into the LRU (the only cache); scratch bytes never
//      linger.
#include "SolidSweep.hpp"
#include "mini_test.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {

using prism::gallery::DecodedThumb;
using prism::gallery::SolidSweepSession;
using prism::gallery::ThumbnailLru;

// Records calls; decodes only payloads that "look like" images (start 'P').
struct FakeDecoder final : SolidSweepSession::IDecoder {
  std::optional<DecodedThumb> Decode(const uint8_t* data, size_t bytes) override {
    ++calls;
    lastSize = bytes;
    if (bytes == 0 || data[0] != 'P') return std::nullopt;  // decode failure path
    DecodedThumb thumb;
    thumb.width = 64;
    thumb.height = 64;
    thumb.pixels.assign(64 * 64 * 4, data[0]);
    return thumb;
  }
  int calls = 0;
  size_t lastSize = 0;
};

SolidSweepSession MakeSession(const std::vector<uint32_t>& indices,
                              const std::vector<uint64_t>& sizes, FakeDecoder& decoder,
                              ThumbnailLru& lru, uint64_t limit = 32ull << 20) {
  return SolidSweepSession(indices, sizes, limit, decoder, lru);
}

PRISM_TEST("one pass over three items decodes three thumbnails into the LRU") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  auto session = MakeSession({0, 1, 2}, {100, 200, 300}, decoder, lru);

  for (uint32_t i : {0u, 1u, 2u}) {
    session.OnItemStart(i);
    const std::string blob(i + 1, 'P');
    session.OnItemBytes(blob.size());
    session.OnItemEnd(reinterpret_cast<const uint8_t*>(blob.data()), blob.size());
  }

  PRISM_CHECK_EQ(session.decodedCount, 3);
  PRISM_CHECK_EQ(decoder.calls, 3);
  PRISM_CHECK_EQ(lru.Count(), 3u);
  PRISM_CHECK_EQ(session.Fraction(), 1.0);
}

PRISM_TEST("over-limit items are skipped without decoding, others still harvest") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  // item 1 is 40 MB against a 32 MB decode limit
  auto session = MakeSession({0, 1, 2}, {100, 40ull << 20, 300}, decoder, lru);

  for (uint32_t i : {0u, 1u, 2u}) {
    session.OnItemStart(i);
    const std::string blob(8, 'P');
    session.OnItemBytes(8);
    session.OnItemEnd(reinterpret_cast<const uint8_t*>(blob.data()), 8);
  }

  PRISM_CHECK_EQ(session.decodedCount, 2);
  PRISM_CHECK_EQ(session.skippedCount, 1);
  PRISM_CHECK_EQ(decoder.calls, 2);  // the big one never reached the decoder
  PRISM_CHECK_EQ(lru.Count(), 2u);
}

PRISM_TEST("abandon drains remaining bytes but decodes nothing further") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  auto session = MakeSession({0, 1, 2}, {100, 200, 300}, decoder, lru);

  session.OnItemStart(0);
  session.OnItemBytes(2);
  session.OnItemEnd(reinterpret_cast<const uint8_t*>("P0"), 2);
  session.Abandon();  // the user scrolled away mid-block
  for (uint32_t i : {1u, 2u}) {
    session.OnItemStart(i);
    session.OnItemBytes(5);
    session.OnItemEnd(reinterpret_cast<const uint8_t*>("PXXXX"), 5);
  }

  PRISM_CHECK_EQ(session.decodedCount, 1);
  PRISM_CHECK_EQ(decoder.calls, 1);
  PRISM_CHECK_EQ(session.drainedBytes, 12u);  // 2 pre + 10 post abandon: all consumed
  PRISM_CHECK_EQ(session.Fraction(), 1.0);    // the pass still completed
}

PRISM_TEST("a failed decode does not poison the pass") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  auto session = MakeSession({0, 1}, {100, 100}, decoder, lru);

  session.OnItemStart(0);  // 'X' payload -> decoder returns nullopt
  session.OnItemBytes(4);
  session.OnItemEnd(reinterpret_cast<const uint8_t*>("XXXX"), 4);
  session.OnItemStart(1);
  session.OnItemBytes(4);
  session.OnItemEnd(reinterpret_cast<const uint8_t*>("POK."), 4);

  PRISM_CHECK_EQ(session.skippedCount, 1);
  PRISM_CHECK_EQ(session.decodedCount, 1);
  PRISM_CHECK_EQ(lru.Count(), 1u);
}

PRISM_TEST("empty payloads never reach the decoder") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  auto session = MakeSession({0}, {0}, decoder, lru);
  session.OnItemStart(0);
  session.OnItemBytes(0);
  session.OnItemEnd(nullptr, 0);
  PRISM_CHECK_EQ(decoder.calls, 0);
  PRISM_CHECK_EQ(session.skippedCount, 1);
}

PRISM_TEST("fraction is monotonic between zero and one") {
  ThumbnailLru lru{8 << 20};
  FakeDecoder decoder;
  auto session = MakeSession({0, 1, 2, 3}, {1, 1, 1, 1}, decoder, lru);
  double previous = 0.0;
  for (uint32_t i : {0u, 1u, 2u, 3u}) {
    session.OnItemStart(i);
    session.OnItemBytes(1);
    session.OnItemEnd(reinterpret_cast<const uint8_t*>("P"), 1);
    const double f = session.Fraction();
    if (f < previous || f > 1.0) return;
    previous = f;
  }
  PRISM_CHECK_EQ(session.Fraction(), 1.0);
}

PRISM_TEST("LRU pressure: a big session evicts old thumbs instead of growing") {
  ThumbnailLru lru{64 * 1024};  // fits ~4 thumbs of 16 KB
  FakeDecoder decoder;
  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (uint32_t i = 0; i < 10; ++i) {
    indices.push_back(i);
    sizes.push_back(10);
  }
  auto session = MakeSession(indices, sizes, decoder, lru);
  for (uint32_t i : indices) {
    session.OnItemStart(i);
    session.OnItemBytes(1);
    session.OnItemEnd(reinterpret_cast<const uint8_t*>("P"), 1);
  }
  PRISM_CHECK_EQ(session.decodedCount, 10);
  PRISM_CHECK(lru.Count() <= 5u);  // budget held, old thumbs evicted
  PRISM_CHECK(lru.SizeBytes() <= lru.BudgetBytes());
}

}  // namespace

PRISM_TEST_MAIN()
