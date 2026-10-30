// TDD: ThumbStore -- the pixel-holding cache behind the gallery tiles. The
// sweep session harvests DecodedThumbs through IThumbSink; this store keeps
// the actual BGRA payload under a byte budget with true LRU eviction, plus a
// version counter so the renderer can invalidate its device bitmaps.
#include "SolidSweep.hpp"
#include "ThumbStore.hpp"
#include "mini_test.hpp"

#include <cstring>

namespace {

using prism::gallery::DecodedThumb;
using prism::gallery::ThumbStore;

DecodedThumb MakeThumb(uint32_t w, uint32_t h, uint8_t fill) {
  DecodedThumb t;
  t.width = w;
  t.height = h;
  t.pixels.assign(static_cast<size_t>(w) * h * 4, fill);
  return t;
}

PRISM_TEST("stored thumbs are retrievable with payload intact") {
  ThumbStore store{1 << 20};
  const DecodedThumb thumb = MakeThumb(64, 64, 0xAB);
  PRISM_CHECK(store.OnThumb(7, thumb));
  const DecodedThumb* got = store.Lookup(7);
  if (!PRISM_CHECK(got != nullptr)) return;
  PRISM_CHECK_EQ(got->width, 64u);
  PRISM_CHECK_EQ(got->height, 64u);
  PRISM_CHECK_EQ(got->pixels.size(), size_t(64 * 64 * 4));
  PRISM_CHECK(got->pixels[0] == 0xAB);
}

PRISM_TEST("budget pressure evicts the least recently used thumb") {
  ThumbStore store{3 * 256};  // three 256-byte thumbs
  store.OnThumb(1, MakeThumb(8, 8, 1));  // 8*8*4 = 256 bytes each
  store.OnThumb(2, MakeThumb(8, 8, 2));
  store.OnThumb(3, MakeThumb(8, 8, 3));
  PRISM_CHECK(store.Lookup(1) != nullptr);  // touch 1: now 2 is the oldest
  PRISM_CHECK(store.OnThumb(4, MakeThumb(8, 8, 4)));
  PRISM_CHECK(store.Lookup(2) == nullptr);  // evicted
  PRISM_CHECK(store.Lookup(1) != nullptr);
  PRISM_CHECK(store.Lookup(3) != nullptr);
  PRISM_CHECK(store.Lookup(4) != nullptr);
  PRISM_CHECK_EQ(store.Count(), 3u);
  PRISM_CHECK(store.SizeBytes() <= store.BudgetBytes());
}

PRISM_TEST("a single thumb larger than the whole budget is refused loudly") {
  ThumbStore store{100};
  PRISM_CHECK(!store.OnThumb(9, MakeThumb(64, 64, 0)));  // 16 KB > 100 B
  PRISM_CHECK_EQ(store.Count(), 0u);
  PRISM_CHECK(store.Lookup(9) == nullptr);
}

PRISM_TEST("re-inserting the same key replaces, never double-counts") {
  ThumbStore store{1 << 20};
  store.OnThumb(5, MakeThumb(32, 32, 1));
  store.OnThumb(5, MakeThumb(16, 16, 2));
  PRISM_CHECK_EQ(store.Count(), 1u);
  const DecodedThumb* got = store.Lookup(5);
  if (!PRISM_CHECK(got != nullptr)) return;
  PRISM_CHECK_EQ(got->width, 16u);  // the replacement won
  PRISM_CHECK_EQ(store.SizeBytes(), size_t(16 * 16 * 4));
}

PRISM_TEST("version bumps on mutation, not on lookup") {
  ThumbStore store{1 << 20};
  const uint64_t v0 = store.Version();
  store.Lookup(3);  // miss: must not bump
  PRISM_CHECK_EQ(store.Version(), v0);
  store.OnThumb(3, MakeThumb(8, 8, 1));
  const uint64_t v1 = store.Version();
  PRISM_CHECK(v1 != v0);
  store.Lookup(3);  // hit: must not bump either
  PRISM_CHECK_EQ(store.Version(), v1);
  store.OnThumb(4, MakeThumb(8, 8, 1));
  PRISM_CHECK(store.Version() != v1);
}

PRISM_TEST("eviction under a sweep keeps the newest harvest") {
  ThumbStore store{4 * 256};
  for (uint32_t i = 0; i < 8; ++i) {
    PRISM_CHECK(store.OnThumb(i, MakeThumb(8, 8, static_cast<uint8_t>(i))));
  }
  PRISM_CHECK_EQ(store.Count(), 4u);
  PRISM_CHECK(store.Lookup(7) != nullptr);  // newest kept
  PRISM_CHECK(store.Lookup(0) == nullptr);  // oldest gone
  PRISM_CHECK(store.SizeBytes() <= store.BudgetBytes());
}

}  // namespace

PRISM_TEST_MAIN()
