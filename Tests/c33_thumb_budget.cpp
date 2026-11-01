// C33: the ThumbStore budget as a CAP and as an IDLE RETURN (CONTRACTS D17).
//
// D17 says a bounded container's advertised capacity must be exactly what it
// can serve. ThumbStore's 64 MiB budget is advertised to the app and printed in
// every memory table, so two things have to be true, and neither was asserted
// before this card:
//
//   1. the cap is REACHABLE -- after saturation one more thumb does not fit
//      (a store that stops 300 KiB short is promising memory it never serves);
//   2. resident stays <= the cap after EVERY op, including under Trim, which
//      is the only path that moves resident down without pressure;
//   3. Trim really returns the pixels (bytes returned == bytes that left), so
//      "release when the gallery is hidden" is not a comment.
//
// The Windows lane's negative control (PRISM_THUMB_MUTANT_NO_TRIM) turns the
// same assertions red; this suite is the always-on arithmetic half.
#include "SolidSweep.hpp"
#include "ThumbStore.hpp"
#include "mini_test.hpp"

#include <cstdint>

namespace {

using prism::gallery::DecodedThumb;
using prism::gallery::ThumbStore;

DecodedThumb MakeThumb(uint32_t side, uint8_t fill) {
  DecodedThumb thumb;
  thumb.width = side;
  thumb.height = side;
  thumb.pixels.assign(static_cast<size_t>(side) * side * 4, fill);
  return thumb;
}

// 64 KiB per thumb: small enough to saturate a 1 MiB store in 16 inserts.
constexpr uint32_t kSide = 128;
constexpr size_t kThumbBytes = static_cast<size_t>(kSide) * kSide * 4;

// What the store actually holds, counted through the public lookup path.
size_t ResidentByLookup(ThumbStore& store) {
  size_t sum = 0;
  for (uint32_t i = 0; i < 32; ++i) {
    const DecodedThumb* thumb = store.Lookup(i);
    if (thumb != nullptr) sum += thumb->pixels.size();
  }
  return sum;
}

PRISM_TEST("the advertised budget is reachable, not merely not exceeded") {
  ThumbStore store{1 << 20};
  for (uint32_t i = 0; i < 64; ++i) store.OnThumb(i, MakeThumb(kSide, static_cast<uint8_t>(i)));
  const size_t resident = store.SizeBytes();
  PRISM_CHECK(resident <= store.BudgetBytes());
  // 1 MiB / 64 KiB = 16 whole thumbs; the cap must be full, not 15/16 of it.
  PRISM_CHECK_EQ(resident, (1u << 20));
  PRISM_CHECK_EQ(store.Count(), 16u);
}

PRISM_TEST("idle trim returns exactly the bytes it drops, and the version moves") {
  ThumbStore store{1 << 20};
  for (uint32_t i = 0; i < 16; ++i) store.OnThumb(i, MakeThumb(kSide, 1));
  const size_t before = store.SizeBytes();
  const uint64_t versionBefore = store.Version();
  const size_t returned = store.Trim(before / 4);
  PRISM_CHECK_EQ(returned, before - store.SizeBytes());
  PRISM_CHECK(store.SizeBytes() <= before / 4);
  PRISM_CHECK(returned > 0);
  PRISM_CHECK(store.Version() != versionBefore);  // renderer cache must invalidate
  // Trimming to something already satisfied is a no-op, not a silent eviction.
  const uint64_t versionAfter = store.Version();
  PRISM_CHECK_EQ(store.Trim(store.BudgetBytes()), size_t(0));
  PRISM_CHECK_EQ(store.Version(), versionAfter);
}

PRISM_TEST("release-all drops every pixel (the hidden-gallery state)") {
  ThumbStore store{1 << 20};
  for (uint32_t i = 0; i < 8; ++i) store.OnThumb(i, MakeThumb(kSide, 2));
  PRISM_CHECK(store.SizeBytes() > 0);
  const size_t returned = store.ReleaseAll();
  PRISM_CHECK_EQ(returned, size_t(8) * kThumbBytes);
  PRISM_CHECK_EQ(store.SizeBytes(), size_t(0));
  PRISM_CHECK_EQ(store.Count(), size_t(0));
  PRISM_CHECK(store.Lookup(0) == nullptr);
}

PRISM_TEST("resident <= budget after every op of a seeded sequence (D17 invariant)") {
  ThumbStore store{512 * 1024};  // 8 thumbs
  uint32_t state = 0x9E3779B9u;
  for (int step = 0; step < 1000; ++step) {
    state = state * 1664525u + 1013904223u;
    const uint32_t op = state % 4;
    const uint32_t key = (state >> 8) % 32;
    if (op == 0) {
      store.OnThumb(key, MakeThumb(kSide, static_cast<uint8_t>(step)));
    } else if (op == 1) {
      store.Lookup(key);
    } else if (op == 2) {
      store.Trim(static_cast<size_t>(state >> 12) % (store.BudgetBytes() + 1));
    } else {
      store.OnThumb(key, MakeThumb(kSide / 2, 7));  // smaller replacement
    }
    PRISM_CHECK(store.SizeBytes() <= store.BudgetBytes());
    // The ledger must equal the payload it holds: a trim that forgets to settle
    // the accounting would still pass the cap check above.
    PRISM_CHECK_EQ(store.SizeBytes(), ResidentByLookup(store));
    if (prism::test::FailureCount() > 0) break;  // one red is enough to verdict
  }
}

}  // namespace

PRISM_TEST_MAIN()
