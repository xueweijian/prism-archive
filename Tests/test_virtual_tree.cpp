// Copy-on-Write virtual archive tree (blueprint ch. 4.5).
#include "VirtualTree.hpp"
#include "mini_test.hpp"

#include <string>

namespace {

std::vector<prism::ArchiveItem> Sample() {
  std::vector<prism::ArchiveItem> items;
  uint32_t index = 0;
  for (const char* path : {"Assets/", "Assets/logo.png", "Assets/old_name.txt", "README.md"}) {
    prism::ArchiveItem item;
    item.index = index++;
    item.path = path;
    item.isDir = path[0] != '\0' && path[std::string(path).size() - 1] == '/';
    items.push_back(item);
  }
  return items;
}

PRISM_TEST("tree is built from the catalog and is clean") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(!tree.dirty());
  PRISM_CHECK(tree.Find("Assets/logo.png") != nullptr);
  PRISM_CHECK(tree.Find("nope") == nullptr);
}

PRISM_TEST("rename rewrites the whole subtree as metadata-only updates") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(tree.Rename("Assets", "Media"));
  PRISM_CHECK(tree.dirty());
  const auto plan = tree.MakeUpdatePlan();
  PRISM_CHECK_EQ(plan.size(), size_t(3));  // Media/ + Media/logo.png + Media/old_name.txt
  for (const auto& item : plan) {
    PRISM_CHECK(!item.newData);   // payload must not be recompressed
    PRISM_CHECK(item.newProps);
    PRISM_CHECK(!item.removed);
  }
  bool sawLogo = false;
  for (const auto& item : plan) {
    if (item.newPath == "Media/logo.png") {
      sawLogo = true;
      PRISM_CHECK_EQ(item.index, int64_t(1));
    }
  }
  PRISM_CHECK(sawLogo);
  PRISM_CHECK(tree.Find("Media/logo.png") != nullptr);
  PRISM_CHECK(tree.Find("Assets/logo.png") == nullptr);
}

PRISM_TEST("rename refuses to overwrite an existing node") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(!tree.Rename("Assets", "README.md"));
  PRISM_CHECK(!tree.dirty());
}

PRISM_TEST("mkdir creates intermediate folders with index -1") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(tree.Mkdir("Assets/2026/Icons"));
  PRISM_CHECK(tree.dirty());
  const auto plan = tree.MakeUpdatePlan();
  bool sawNew = false;
  for (const auto& item : plan) {
    if (item.newPath == "Assets/2026") {
      sawNew = true;
      PRISM_CHECK_EQ(item.index, int64_t(-1));
      PRISM_CHECK(!item.newData);
    }
  }
  PRISM_CHECK(sawNew);
}

PRISM_TEST("move relocates a subtree") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(tree.Mkdir("Docs"));
  PRISM_CHECK(tree.Move("Assets/old_name.txt", "Docs"));
  PRISM_CHECK(tree.Find("Docs/old_name.txt") != nullptr);
  PRISM_CHECK(tree.Find("Assets/old_name.txt") == nullptr);
  // Moving into the folder it already lives in is a no-op, not a data loss.
  PRISM_CHECK(!tree.Move("Docs/old_name.txt", "Docs"));
  const auto plan = tree.MakeUpdatePlan();
  bool sawMoved = false;
  for (const auto& item : plan) {
    if (item.newPath == "Docs/old_name.txt") sawMoved = true;
  }
  PRISM_CHECK(sawMoved);
}

PRISM_TEST("erase collects indices for removal") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(tree.Erase("Assets"));
  PRISM_CHECK_EQ(tree.erasedIndices().size(), size_t(3));  // dir + 2 files
  const auto plan = tree.MakeUpdatePlan();
  PRISM_CHECK_EQ(plan.size(), size_t(3));
  for (const auto& item : plan) PRISM_CHECK(item.removed);
}

PRISM_TEST("root cannot be renamed or erased") {
  auto tree = prism::VirtualArchiveTree::FromCatalog(Sample());
  PRISM_CHECK(!tree.Rename("", "x"));
  PRISM_CHECK(!tree.Erase(""));
}

}  // namespace

PRISM_TEST_MAIN()
