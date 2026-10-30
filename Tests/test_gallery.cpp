// TDD: the streaming gallery core (blueprint ch. 4.3) -- viewport
// virtualization, the fixed-budget thumbnail cache, per-solid-block sweep
// planning, and the Quick Look session.
//
// First principles:
//  (a) a 50k-file archive can never materialize 50k widgets: the UI renders
//      exactly the viewport (+ overscan rows) and nothing else; the math must
//      clamp, stay monotonic under scroll, and hit-test deterministically,
//  (b) decoded thumbnails are memory, not geometry: a fixed byte budget with
//      true LRU eviction; an item larger than the whole budget is *rejected
//      loudly* (L3 -- silent dropping would corrupt the budget invariant),
//  (c) solid blocks: one linear pass harvests every thumbnail along the way
//      (<= 256 MB blocks); oversized blocks get an on-demand badge instead of
//      freezing the gallery -- the strategy is decided by data, not vibes,
//  (d) Quick Look is a session over previewable leaves only: space toggles,
//      Esc closes, navigation skips non-previewable entries, and over-limit
//      items open in a *marked* streaming state (bomb hygiene, from M1).
#include "Gallery.hpp"
#include "mini_test.hpp"

#include <algorithm>
#include <vector>

namespace {

using namespace prism::gallery;

prism::ArchiveItem Leaf(const std::string& path, uint64_t size, uint32_t block = 0) {
  prism::ArchiveItem item;
  item.path = path;
  item.size = size;
  item.block = block;
  return item;
}

// ---------------------------------------------------------------- viewport

PRISM_TEST("viewport renders exactly the visible window plus overscan") {
  ViewportWindow viewport{GridLayout{.columns = 4, .cellHeight = 150.0}, 1000};
  viewport.SetViewportSize(960.0, 600.0);
  viewport.SetScroll(0.0);
  const auto top = viewport.Visible(/*overscanRows=*/1);
  PRISM_CHECK_EQ(top.first, 0);
  PRISM_CHECK_EQ(top.last, 19);  // rows 0..4 (4 visible + 1 overscan) x 4 columns

  viewport.SetScroll(600.0);  // one full row down
  const auto mid = viewport.Visible(1);
  PRISM_CHECK_EQ(mid.first, 12);  // row 3 (overscan above)
  PRISM_CHECK_EQ(mid.last, 35);   // row 8 (overscan below)
}

PRISM_TEST("scroll clamps to content and the last items stay reachable") {
  ViewportWindow viewport{GridLayout{.columns = 4, .cellHeight = 150.0}, 1000};
  viewport.SetViewportSize(960.0, 600.0);
  viewport.SetScroll(-50.0);
  PRISM_CHECK_EQ(viewport.Scroll(), 0.0);
  viewport.SetScroll(1e9);
  PRISM_CHECK_EQ(viewport.Scroll(), viewport.MaxScroll());
  const auto bottom = viewport.Visible(1);
  PRISM_CHECK_EQ(bottom.last, 999);
  PRISM_CHECK(bottom.first > 0);
}

PRISM_TEST("scrolling down never moves the window backwards (monotonic)") {
  ViewportWindow viewport{GridLayout{.columns = 4, .cellHeight = 150.0}, 1000};
  viewport.SetViewportSize(960.0, 600.0);
  int previousFirst = 0;
  for (double scroll = 0.0; scroll <= viewport.MaxScroll(); scroll += 137.0) {
    viewport.SetScroll(scroll);
    const auto range = viewport.Visible(1);
    if (range.first < previousFirst) return;  // regression
    previousFirst = range.first;
  }
  PRISM_CHECK(true);
}

PRISM_TEST("hit testing maps a content point to exactly one item") {
  ViewportWindow viewport{GridLayout{.columns = 4, .cellHeight = 150.0}, 1000};
  viewport.SetViewportSize(960.0, 600.0);
  PRISM_CHECK_EQ(viewport.IndexAt(0.0, 10.0), 0);      // row 0, col 0
  PRISM_CHECK_EQ(viewport.IndexAt(500.0, 100.0), 2);   // 960/4 = 240 px columns
  PRISM_CHECK_EQ(viewport.IndexAt(730.0, 100.0), 3);   // row 0, col 3
  PRISM_CHECK_EQ(viewport.IndexAt(10.0, 610.0), 16);   // row 4 (600 + 10)
  PRISM_CHECK_EQ(viewport.IndexAt(-5.0, -5.0), -1);    // outside the grid
}

PRISM_TEST("empty archive renders nothing and never divides by zero") {
  ViewportWindow viewport{GridLayout{.columns = 4, .cellHeight = 150.0}, 0};
  viewport.SetViewportSize(960.0, 600.0);
  viewport.SetScroll(0.0);
  const auto range = viewport.Visible(1);
  PRISM_CHECK(range.first > range.last);  // empty range contract
  PRISM_CHECK_EQ(viewport.MaxScroll(), 0.0);
  PRISM_CHECK_EQ(viewport.IndexAt(10.0, 10.0), -1);
}

// ---------------------------------------------------------------- LRU cache

PRISM_TEST("inserting beyond the budget evicts least-recently-used first") {
  ThumbnailLru cache{1000};
  PRISM_CHECK(cache.Insert(1, 400));
  PRISM_CHECK(cache.Insert(2, 400));
  PRISM_CHECK_EQ(cache.SizeBytes(), 800u);
  PRISM_CHECK(cache.Insert(3, 300));  // must evict key 1
  PRISM_CHECK(!cache.Contains(1));
  PRISM_CHECK(cache.Contains(2));
  PRISM_CHECK(cache.Contains(3));
  PRISM_CHECK_EQ(cache.SizeBytes(), 700u);
}

PRISM_TEST("a lookup refreshes recency (true LRU, not FIFO)") {
  ThumbnailLru cache{1000};
  cache.Insert(1, 400);
  cache.Insert(2, 400);
  PRISM_CHECK(cache.Lookup(1));  // touch 1 -> 2 becomes the victim
  PRISM_CHECK(cache.Insert(3, 300));
  PRISM_CHECK(cache.Contains(1));
  PRISM_CHECK(!cache.Contains(2));
}

PRISM_TEST("an item larger than the whole budget is rejected loudly") {
  ThumbnailLru cache{1000};
  cache.Insert(1, 400);
  PRISM_CHECK(!cache.Insert(9, 5000));  // refused, nothing evicted
  PRISM_CHECK(cache.Contains(1));
  PRISM_CHECK_EQ(cache.SizeBytes(), 400u);
}

PRISM_TEST("hits and misses are counted by lookup") {
  ThumbnailLru cache{1000};
  cache.Insert(1, 100);
  PRISM_CHECK(cache.Lookup(1));
  PRISM_CHECK(!cache.Lookup(42));
  PRISM_CHECK_EQ(cache.Hits(), 1u);
  PRISM_CHECK_EQ(cache.Misses(), 1u);
}

PRISM_TEST("multi-eviction: inserts keep evicting until the item fits") {
  ThumbnailLru cache{1000};
  cache.Insert(1, 300);
  cache.Insert(2, 300);
  cache.Insert(3, 300);  // 900 used
  PRISM_CHECK(cache.Insert(4, 500));  // evicts 1, then 2 -> 600 + 500 fits
  PRISM_CHECK(!cache.Contains(1) && !cache.Contains(2));
  PRISM_CHECK(cache.Contains(3) && cache.Contains(4));
  PRISM_CHECK(cache.SizeBytes() <= 1000);
}

PRISM_TEST("re-inserting a live key updates size instead of double counting") {
  ThumbnailLru cache{1000};
  cache.Insert(1, 400);
  cache.Insert(1, 100);  // shrink
  PRISM_CHECK_EQ(cache.SizeBytes(), 100u);
  PRISM_CHECK_EQ(cache.Count(), 1u);
}

// ------------------------------------------------------- solid sweep plan

PRISM_TEST("block strategy: sweep small blocks, badge oversized ones") {
  PRISM_CHECK_EQ(PlanBlock({1, 200ull * 1024 * 1024, 5000}), BlockStrategy::SinglePassSweep);
  PRISM_CHECK_EQ(PlanBlock({2, 256ull * 1024 * 1024, 8000}), BlockStrategy::SinglePassSweep);  // boundary is inclusive
  PRISM_CHECK_EQ(PlanBlock({3, 256ull * 1024 * 1024 + 1, 9000}), BlockStrategy::OnDemandBadge);
  PRISM_CHECK_EQ(PlanBlock({4, 4ull * 1024 * 1024 * 1024, 40}), BlockStrategy::OnDemandBadge);
}

PRISM_TEST("sweep order puts the focused block first, then nearest") {
  const std::vector<SolidBlock> blocks{
      {0, 1ull << 20, 10}, {1, 1ull << 20, 10}, {2, 1ull << 20, 10}, {3, 1ull << 20, 10}};
  const auto order = SweepOrder(blocks, /*focusBlock=*/2);
  PRISM_CHECK_EQ(order.size(), 4u);
  PRISM_CHECK_EQ(order[0], 2);
  PRISM_CHECK_EQ(order[1], 1);  // tie between 1 and 3 -> lower id first
  PRISM_CHECK_EQ(order[2], 3);
  PRISM_CHECK_EQ(order[3], 0);
}

// ------------------------------------------------------------ quick look

PRISM_TEST("quick look opens on previewable leaves and reports the kind") {
  std::vector<prism::ArchiveItem> items{
      Leaf("photos/a.png", 800'000), Leaf("data.bin", 100), Leaf("photos/b.png", 900'000),
      Leaf("sub", 0, 0), Leaf("docs/c.pdf", 2'000'000)};
  items[3].isDir = true;

  QuickLookSession session{items};
  PRISM_CHECK_EQ(session.OpenAt(0), QuickLookSession::State::Open);
  PRISM_CHECK_EQ(session.kind(), prism::PreviewKind::Image);
  PRISM_CHECK_EQ(session.focus(), 0u);
  PRISM_CHECK(!session.streaming());

  PRISM_CHECK_EQ(session.OpenAt(4), QuickLookSession::State::Open);
  PRISM_CHECK_EQ(session.kind(), prism::PreviewKind::Pdf);
}

PRISM_TEST("space on the same item closes, on another item switches") {
  const std::vector<prism::ArchiveItem> items{Leaf("a.png", 10), Leaf("b.png", 10)};
  QuickLookSession session{items};
  session.OpenAt(0);
  PRISM_CHECK_EQ(session.ToggleAt(0), QuickLookSession::State::Closed);
  PRISM_CHECK_EQ(session.ToggleAt(1), QuickLookSession::State::Open);
  PRISM_CHECK_EQ(session.focus(), 1u);
}

PRISM_TEST("navigation skips non-previewable entries and clamps at the ends") {
  std::vector<prism::ArchiveItem> items{
      Leaf("a.png", 10), Leaf("blob.dat", 10), Leaf("b.png", 10), Leaf("sub", 10),
      Leaf("c.pdf", 10)};
  items[3].isDir = true;  // dat -> None kind, dir -> not a leaf

  QuickLookSession session{items};
  session.OpenAt(0);
  PRISM_CHECK(session.Next());
  PRISM_CHECK_EQ(session.focus(), 2u);  // skipped blob.dat
  PRISM_CHECK(session.Next());
  PRISM_CHECK_EQ(session.focus(), 4u);  // skipped sub/
  PRISM_CHECK(!session.Next());         // end of the world
  PRISM_CHECK(session.Prev());
  PRISM_CHECK_EQ(session.focus(), 2u);
}

PRISM_TEST("over-limit items open in the marked streaming state") {
  const std::vector<prism::ArchiveItem> items{Leaf("huge.psd", 10'000'000)};
  QuickLookSession session{items, /*byteLimit=*/8'000'000};
  PRISM_CHECK_EQ(session.OpenAt(0), QuickLookSession::State::Streaming);
  PRISM_CHECK_EQ(session.kind(), prism::PreviewKind::Image);  // still an image
  PRISM_CHECK(session.streaming());

  prism::QuickLookPlanner::Decision decision =
      prism::QuickLookPlanner::Decide(items[0], 8'000'000);
  PRISM_CHECK(decision.exceedsLimit);  // planner and session agree
}

PRISM_TEST("escape closes from any state; directories never open") {
  std::vector<prism::ArchiveItem> items{Leaf("sub", 10), Leaf("a.png", 10)};
  items[0].isDir = true;
  QuickLookSession session{items};
  PRISM_CHECK_EQ(session.OpenAt(0), QuickLookSession::State::Closed);  // dir no-op
  session.OpenAt(1);
  PRISM_CHECK_EQ(session.Close(), QuickLookSession::State::Closed);
  PRISM_CHECK_EQ(session.Close(), QuickLookSession::State::Closed);  // idempotent
}

}  // namespace

PRISM_TEST_MAIN()
