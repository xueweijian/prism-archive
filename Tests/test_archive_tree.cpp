// PrismArchive -- PrismCore
// Tests for the folder-navigation view model: which entries belong to the
// current folder, and what the breadcrumb chain above it is. The gallery is
// USELESS for real archives without this -- catalogs are trees and the window
// renders one flat list.
#include "mini_test.hpp"

#include "../Src/PrismCore/ArchiveTreeView.hpp"

#include "../Src/PrismCore/ArchiveCatalog.hpp"

#include <iostream>

using namespace prism;

namespace {

std::vector<ArchiveItem> SampleCatalog() {
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, uint64_t size) {
    ArchiveItem it;
    it.path = path;
    it.isDir = dir;
    it.size = size;
    it.index = static_cast<uint32_t>(items.size());
    items.push_back(it);
  };
  add("Assets", true, 0);
  add("Assets/Icons", true, 0);
  add("Assets/Icons/a.png", false, 100);
  add("Assets/Icons/b.png", false, 200);
  add("Assets/Icons/c.svg", false, 300);
  add("Assets/readme.md", false, 50);
  add("docs", true, 0);
  add("docs/manual.pdf", false, 400);
  add("root.png", false, 500);
  return items;
}

}  // namespace

PRISM_TEST("root view: only top-level entries, folders first") {
  auto items = SampleCatalog();
  auto view = tree::Build(items, "");
  PRISM_CHECK_EQ(view.entries.size(), size_t(3));  // Assets, docs, root.png
  // Folders first (Assets, docs), then root.png.
  PRISM_CHECK(view.entries[0].isDir && view.entries[1].isDir);
  PRISM_CHECK(!view.entries[2].isDir);
  PRISM_CHECK_EQ(view.entries[2].path, std::string("root.png"));
  PRISM_CHECK(view.breadcrumb.empty());
}

PRISM_TEST("nested view: children of Assets/Icons only") {
  auto items = SampleCatalog();
  auto view = tree::Build(items, "Assets/Icons");
  PRISM_CHECK_EQ(view.entries.size(), size_t(3));
  PRISM_CHECK_EQ(view.entries[0].path, std::string("Assets/Icons/a.png"));
  PRISM_CHECK_EQ(view.entries[2].path, std::string("Assets/Icons/c.svg"));
  PRISM_CHECK_EQ(view.breadcrumb.size(), size_t(2));
  PRISM_CHECK_EQ(view.breadcrumb[0].name, std::string("Assets"));
  PRISM_CHECK_EQ(view.breadcrumb[1].name, std::string("Icons"));
  PRISM_CHECK_EQ(view.breadcrumb[1].folder, std::string("Assets/Icons"));
}

PRISM_TEST("breadcrumb of the root is empty; up-level walks one step") {
  auto items = SampleCatalog();
  auto view = tree::Build(items, "Assets/Icons");
  PRISM_CHECK_EQ(view.breadcrumb.size(), size_t(2));
  const std::string parent = view.UpFrom("Assets/Icons");
  PRISM_CHECK_EQ(parent, std::string("Assets"));
  const std::string root = view.UpFrom("Assets");
  PRISM_CHECK_EQ(root, std::string(""));
}

PRISM_TEST("trailing slashes and backslashes normalize before matching") {
  std::vector<ArchiveItem> items;
  ArchiveItem it;
  it.path = "pack/inner.txt";
  it.index = 0;
  items.push_back(it);
  auto view = tree::Build(items, "pack/");
  PRISM_CHECK_EQ(view.entries.size(), size_t(1));
  auto view2 = tree::Build(items, "pack\\");
  PRISM_CHECK_EQ(view2.entries.size(), size_t(1));
}

PRISM_TEST("hidden noise: a folder prefix must not match a longer name") {
  // 'Assets2/x' must NOT appear under 'Assets' -- prefix matching without a
  // separator check is the classic bug.
  std::vector<ArchiveItem> items;
  ArchiveItem decoy;
  decoy.path = "Assets2/x.txt";
  decoy.index = 0;
  items.push_back(decoy);
  ArchiveItem real;
  real.path = "Assets/y.txt";
  real.index = 1;
  items.push_back(real);
  auto view = tree::Build(items, "Assets");
  PRISM_CHECK_EQ(view.entries.size(), size_t(1));
  PRISM_CHECK_EQ(view.entries[0].path, std::string("Assets/y.txt"));
}

PRISM_TEST("empty catalog yields an empty view, not a crash") {
  std::vector<ArchiveItem> empty;
  auto view = tree::Build(empty, "");
  PRISM_CHECK(view.entries.empty());
  PRISM_CHECK(view.breadcrumb.empty());
}

PRISM_TEST_MAIN()
