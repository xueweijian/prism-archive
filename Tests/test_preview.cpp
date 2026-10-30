// TDD: Quick Look strategy + solid-block gallery prefetch (blueprint ch. 4.1/4.3).
//
// First principles:
//  * a preview's value is "instant visual"; its cost is decompressed bytes and
//    (in solid archives) the whole block prefix, and its risk is a bomb wearing
//    an image extension. So the planner must answer three questions and refuse
//    loudly on the third.
//  * a solid block is read as one unit: after the user opens item N, the next
//    bytes we already have paid for are the REST OF N'S BLOCK -- never a
//    random other block. Prefetch order therefore follows block layout, focus
//    first, capped by an explicit byte budget.
#include "ArchiveCatalog.hpp"
#include "Preview.hpp"
#include "mini_test.hpp"

#include <string>
#include <vector>

namespace {

prism::ArchiveItem MakeItem(uint32_t index, const std::string& path, uint64_t size,
                            uint32_t block = 0, bool isDir = false) {
  prism::ArchiveItem item;
  item.index = index;
  item.path = path;
  item.size = size;
  item.block = block;
  item.isDir = isDir;
  return item;
}

prism::ArchiveCatalog GalleryCatalog() {
  prism::ArchiveCatalog catalog;
  catalog.Add(MakeItem(0, "Scans/aa.png", 10, 1));
  catalog.Add(MakeItem(1, "Scans/bb.png", 20, 1));
  catalog.Add(MakeItem(2, "Scans/cc.png", 30, 1));
  catalog.Add(MakeItem(3, "Docs/readme.txt", 5, 2));
  return catalog;
}

PRISM_TEST("kind table maps common extensions") {
  using K = prism::PreviewKind;
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("photo.PNG") == K::Image);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("hero.webp") == K::Image);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("doc.pdf") == K::Pdf);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("main.cpp") == K::Text);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("notes.txt") == K::Text);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("clip.mp4") == K::Media);
  PRISM_CHECK(prism::QuickLookPlanner::KindByName("_makefile") == K::None);
}

PRISM_TEST("decide flags oversized previews instead of refusing silently") {
  const auto small = prism::QuickLookPlanner::Decide(MakeItem(0, "a/b.png", 800 * 1024));
  PRISM_CHECK(small.kind == prism::PreviewKind::Image);
  PRISM_CHECK(!small.exceedsLimit);

  const auto huge = prism::QuickLookPlanner::Decide(MakeItem(1, "a/huge.jpg", 900ull << 20));
  PRISM_CHECK(huge.kind == prism::PreviewKind::Image);      // still previewable
  PRISM_CHECK(huge.exceedsLimit);                           // but loudly flagged
}

PRISM_TEST("directories preview as themselves") {
  const auto d = prism::QuickLookPlanner::Decide(MakeItem(9, "Assets/", 0, 0, true));
  PRISM_CHECK(d.kind == prism::PreviewKind::Directory);
}

PRISM_TEST("prefetch follows the focused block: focus first, then successors, then wrap") {
  const auto plan = prism::SolidBlockPrefetcher::Plan(GalleryCatalog(), 1, 100);
  PRISM_CHECK_EQ(plan.size(), size_t(3));
  PRISM_CHECK_EQ(plan[0], uint32_t(1));  // the item the user clicked
  PRISM_CHECK_EQ(plan[1], uint32_t(2));  // its in-block successor
  PRISM_CHECK_EQ(plan[2], uint32_t(0));  // then wrap around to the prefix
}

PRISM_TEST("prefetch skips an item that busts the budget but keeps filling") {
  // bb(20) + cc(30) > 45, so cc is skipped; aa(10) still fits and is useful
  // (it is the item right before the focus). No holes by design is worth less
  // here than a full budget -- deterministic order is kept either way.
  const auto plan = prism::SolidBlockPrefetcher::Plan(GalleryCatalog(), 1, 45);
  PRISM_CHECK_EQ(plan.size(), size_t(2));
  PRISM_CHECK_EQ(plan[0], uint32_t(1));
  PRISM_CHECK_EQ(plan[1], uint32_t(0));
}

PRISM_TEST("prefetch never leaks across blocks") {
  const auto plan = prism::SolidBlockPrefetcher::Plan(GalleryCatalog(), 3, 100);
  PRISM_CHECK_EQ(plan.size(), size_t(1));
  PRISM_CHECK_EQ(plan[0], uint32_t(3));
}

PRISM_TEST("directories are never prefetched as payload") {
  prism::ArchiveCatalog catalog;
  catalog.Add(MakeItem(0, "D/", 0, 1, true));
  catalog.Add(MakeItem(1, "D/x.png", 10, 1));
  const auto plan = prism::SolidBlockPrefetcher::Plan(catalog, 1, 100);
  PRISM_CHECK_EQ(plan.size(), size_t(1));
  PRISM_CHECK_EQ(plan[0], uint32_t(1));
}

}  // namespace

PRISM_TEST_MAIN()
