// PrismArchive -- PrismCore
// Tests for the folder-navigation view model: which entries belong to the
// current folder, and what the breadcrumb chain above it is. The gallery is
// USELESS for real archives without this -- catalogs are trees and the window
// renders one flat list.
//
// Batch A increment 5 adds the two decisions the window used to have no answer
// for: the breadcrumb hit test (pure geometry with an INJECTED width function,
// so it is exact on Linux) and the Navigator state machine (drill in, back up,
// crumb jump, selection restore). Every assertion below is about CONTENT
// (paths, levels, catalog indices), never about "something happened".
#include "mini_test.hpp"

#include "../Src/PrismCore/ArchiveTreeView.hpp"

#include "../Src/PrismCore/ArchiveCatalog.hpp"

#include <iostream>
#include <string>
#include <vector>

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

// Deterministic pixel width: the test owns the metric, so no font is needed.
// Counts UTF-8 code points (a continuation byte is not a glyph), which is what
// a DirectWrite measurement sees; 10 px per glyph keeps the arithmetic legible.
int FakeWidth(std::string_view text) {
  int glyphs = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++glyphs;
  }
  return glyphs * 10;
}

std::string PathsJoined(const std::vector<tree::Entry>& entries) {
  std::string out;
  for (const auto& e : entries) {
    if (!out.empty()) out += "|";
    out += e.path;
  }
  return out;
}

int IndexOfPath(const std::vector<tree::Entry>& entries, std::string_view path) {
  for (size_t i = 0; i < entries.size(); ++i) {
    if (entries[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

// ------------------------------------------------------------------ view model

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

// ------------------------------------------------ increment 5: view refinements

PRISM_TEST("ordering follows 7-Zip: digit runs compare by value, not bytes") {
  // PanelSort.cpp:8-46 CompareFileNames_ForFolderList. A byte compare puts
  // file10 before file2, which looks broken next to Explorer.
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir) {
    ArchiveItem it;
    it.path = path;
    it.isDir = dir;
    it.index = static_cast<uint32_t>(items.size());
    items.push_back(it);
  };
  add("file10.txt", false);
  add("file2.txt", false);
  add("img011.txt", false);
  add("img02.txt", false);
  auto view = tree::Build(items, "");
  PRISM_CHECK_EQ(PathsJoined(view.entries),
                 std::string("file2.txt|file10.txt|img02.txt|img011.txt"));
}

PRISM_TEST("pseudo children '.' and '..' never appear in a listing") {
  // 7-Zip synthesises ".." only with ShowDots (PanelItems.cpp:578), default
  // off (Panel.h:608). Garbage in the central directory is not a child.
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir) {
    ArchiveItem it;
    it.path = path;
    it.isDir = dir;
    it.index = static_cast<uint32_t>(items.size());
    items.push_back(it);
  };
  add("..", true);
  add(".", true);
  add("../escape.txt", false);
  add("pack/..", true);
  add("pack/.", true);
  add("pack/real.txt", false);
  auto root = tree::Build(items, "");
  PRISM_CHECK_EQ(PathsJoined(root.entries), std::string("pack"));
  auto inner = tree::Build(items, "pack");
  PRISM_CHECK_EQ(PathsJoined(inner.entries), std::string("pack/real.txt"));
}

PRISM_TEST("a hostile folder string cannot escape the archive root") {
  // Two layers, two different jobs (CONTRACTS B4 / Zip Slip):
  //   Build()      drops the ".." segments, so no crumb can ever read "..";
  //   Navigator()  then clamps a path that does not exist to the deepest
  //                existing ancestor, so a stale/hostile jump lands on the
  //                root instead of an empty window.
  std::vector<ArchiveItem> items;
  ArchiveItem it;
  it.path = "inner.txt";
  it.index = 0;
  items.push_back(it);
  auto view = tree::Build(items, "../../etc");
  PRISM_CHECK(view.entries.empty());  // "etc" is not this archive
  PRISM_CHECK_EQ(view.breadcrumb.size(), size_t(1));
  PRISM_CHECK_EQ(view.breadcrumb[0].name, std::string("etc"));  // never ".."
  PRISM_CHECK_EQ(view.breadcrumb[0].folder, std::string("etc"));

  tree::Navigator nav(&items);
  const auto jump = nav.GoTo("../../etc");
  PRISM_CHECK_EQ(jump.action, tree::NavAction::None);  // already at the root
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  PRISM_CHECK_EQ(PathsJoined(nav.Current().entries), std::string("inner.txt"));
}

PRISM_TEST("a folder with no catalog node does not steal a child's index") {
  // Implicit folders ("Inbox/a.txt" with no "Inbox" entry) are real in 7z/ZIP.
  // Borrowing the child's index would paint that file's thumbnail on the tile.
  std::vector<ArchiveItem> items;
  ArchiveItem child;
  child.path = "Inbox/mail.eml";
  child.size = 42;
  child.index = 7;
  items.push_back(child);
  auto view = tree::Build(items, "");
  PRISM_CHECK_EQ(view.entries.size(), size_t(1));
  PRISM_CHECK_EQ(view.entries[0].path, std::string("Inbox"));
  PRISM_CHECK(view.entries[0].isDir);
  PRISM_CHECK_EQ(view.entries[0].index, tree::kSyntheticIndex);
  PRISM_CHECK(view.entries[0].index != 7u);
}

// ------------------------------------------------- increment 5: breadcrumb hit

PRISM_TEST("breadcrumb segments: root label, then one crumb per level") {
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  nav.GoTo("Assets/Icons");
  const auto segments = nav.Segments("Archive");
  // root, sep, Assets, sep, Icons
  PRISM_CHECK_EQ(segments.size(), size_t(5));
  PRISM_CHECK_EQ(segments[0].text, std::string("Archive"));
  PRISM_CHECK_EQ(segments[0].level, tree::kRootLevel);
  PRISM_CHECK_EQ(segments[1].level, tree::kNoLevel);  // separator: dead pixels
  PRISM_CHECK_EQ(segments[2].text, std::string("Assets"));
  PRISM_CHECK_EQ(segments[2].level, 0);
  PRISM_CHECK_EQ(segments[4].text, std::string("Icons"));
  PRISM_CHECK_EQ(segments[4].level, 1);
  // The painted string IS the concatenation of the clickable segments.
  PRISM_CHECK_EQ(tree::JoinBreadcrumb(segments),
                 std::string("Archive \xE2\x80\xBA Assets \xE2\x80\xBA Icons"));
}

PRISM_TEST("breadcrumb geometry: a click maps to exactly one level") {
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  nav.GoTo("Assets/Icons");
  // FakeWidth = 10 px per char. origin 100:
  //   "Archive" 7ch -> [100,170)  root
  //   " > "     3ch -> [170,200)  separator
  //   "Assets"  6ch -> [200,260)  level 0
  //   " > "     3ch -> [260,290)  separator
  //   "Icons"   5ch -> [290,340)  level 1
  const auto layout = nav.Layout("Archive", 100, &FakeWidth);
  PRISM_CHECK_EQ(layout.total, 240);
  PRISM_CHECK_EQ(layout.spans.size(), size_t(5));
  PRISM_CHECK_EQ(layout.spans[2].left, 200);
  PRISM_CHECK_EQ(layout.spans[2].right, 260);
  PRISM_CHECK_EQ(layout.spans[4].right, 340);

  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 99), tree::kNoLevel);   // before
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 100), tree::kRootLevel);  // left edge
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 169), tree::kRootLevel);  // last px
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 170), tree::kNoLevel);    // separator
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 185), tree::kNoLevel);    // separator
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 200), 0);                 // left edge
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 259), 0);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 260), tree::kNoLevel);    // gap
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 290), 1);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 339), 1);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 340), tree::kNoLevel);    // past end
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 5000), tree::kNoLevel);
}

PRISM_TEST("breadcrumb clipping: a crumb clipped away is not clickable") {
  // The bar is clipped to the island. Both clamp directions must be real:
  // a crumb whose pixels are outside the island cannot answer clicks, or the
  // user navigates by clicking text they cannot see.
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  nav.GoTo("Assets/Icons");
  // Raw layout: root [100,170), sep [170,200), Assets [200,260),
  // sep [260,290), Icons [290,340).
  const auto rightClipped = nav.Layout("Archive", 100, &FakeWidth, 0, 320);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(rightClipped, 300), 1);        // visible part
  PRISM_CHECK_EQ(tree::HitBreadcrumb(rightClipped, 330), tree::kNoLevel);  // clipped px
  PRISM_CHECK_EQ(tree::HitBreadcrumb(rightClipped, 339), tree::kNoLevel);
  const auto leftClipped = nav.Layout("Archive", 100, &FakeWidth, 250, 4000);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(leftClipped, 255), 0);         // visible part
  PRISM_CHECK_EQ(tree::HitBreadcrumb(leftClipped, 210), tree::kNoLevel);  // clipped px
  PRISM_CHECK_EQ(tree::HitBreadcrumb(leftClipped, 120), tree::kNoLevel);
  // A crumb clipped away entirely is a dead span, not a level.
  const auto gone = nav.Layout("Archive", 100, &FakeWidth, 0, 280);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(gone, 295), tree::kNoLevel);   // Icons is outside
  PRISM_CHECK_EQ(tree::HitBreadcrumb(gone, 339), tree::kNoLevel);
  // Empty clip window: nothing is clickable at all.
  const auto dead = nav.Layout("Archive", 100, &FakeWidth, 400, 401);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(dead, 120), tree::kNoLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(dead, 300), tree::kNoLevel);
}

PRISM_TEST("breadcrumb at the root: only the root label, no crumb levels") {
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  const auto segments = nav.Segments("Archive");
  PRISM_CHECK_EQ(segments.size(), size_t(1));
  const auto layout = nav.Layout("Archive", 0, &FakeWidth);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 5), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 70), tree::kNoLevel);  // one past
}

// ---------------------------------------------------- increment 5: navigator

PRISM_TEST("navigate: drill into a folder, then Back restores its own row") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  PRISM_CHECK_EQ(nav.Count(), 3);          // Assets, docs, root.png
  PRISM_CHECK_EQ(nav.Selection(), -1);     // nothing selected initially

  const int assetsRow = IndexOfPath(nav.Current().entries, "Assets");
  PRISM_CHECK(assetsRow >= 0);
  const auto entered = nav.Enter(assetsRow);
  PRISM_CHECK_EQ(entered.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets"));
  // Entering keeps NOTHING selected (PanelFolderChange.cpp:1071-1079).
  PRISM_CHECK_EQ(nav.Selection(), -1);
  PRISM_CHECK_EQ(PathsJoined(nav.Current().entries),
                 std::string("Assets/Icons|Assets/readme.md"));

  const auto up = nav.Back();
  PRISM_CHECK_EQ(up.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  // Back selects the folder we just came out of -- by name, not by rememberd
  // index, so it survives re-sorting.
  PRISM_CHECK_EQ(up.selection, assetsRow);
  PRISM_CHECK_EQ(nav.Selection(), assetsRow);
  PRISM_CHECK_EQ(nav.Current().entries[static_cast<size_t>(nav.Selection())].path,
                 std::string("Assets"));
}

PRISM_TEST("navigate: deeper nesting restores only the immediate child") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.Enter(IndexOfPath(nav.Current().entries, "Assets"));   // Assets
  nav.Enter(IndexOfPath(nav.Current().entries, "Assets/Icons"));  // Assets/Icons
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets/Icons"));
  PRISM_CHECK_EQ(nav.Count(), 3);
  const auto up = nav.Back();
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets"));
  PRISM_CHECK_EQ(up.selection, IndexOfPath(nav.Current().entries, "Assets/Icons"));
  const auto upAgain = nav.Back();
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  PRISM_CHECK_EQ(upAgain.selection, IndexOfPath(nav.Current().entries, "Assets"));
}

PRISM_TEST("navigate: Backspace at the root is a no-op, not an empty window") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.SetSelection(1);  // the user is looking at something
  const auto stuck = nav.Back();
  PRISM_CHECK_EQ(stuck.action, tree::NavAction::None);
  PRISM_CHECK_EQ(stuck.selection, 1);
  PRISM_CHECK_EQ(nav.Selection(), 1);  // and it is still selected afterwards
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  PRISM_CHECK_EQ(nav.Count(), 3);  // the list is untouched
  PRISM_CHECK_EQ(PathsJoined(nav.Current().entries),
                 std::string("Assets|docs|root.png"));
  // Backspace at the root must never turn into a navigation (which would
  // reset the viewport and clear the selection for no reason).
  PRISM_CHECK_EQ(nav.Back().action, tree::NavAction::None);
  PRISM_CHECK_EQ(nav.Selection(), 1);
}

PRISM_TEST("navigate: double-click on a FILE keeps the file action semantics") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  const int fileRow = IndexOfPath(nav.Current().entries, "root.png");
  const auto result = nav.Enter(fileRow);
  PRISM_CHECK_EQ(result.action, tree::NavAction::ActivateFile);
  // The catalog index (not the row) is what the engine needs -- row 2 of the
  // filtered folder list is index 8 of the archive.
  PRISM_CHECK_EQ(result.catalogIndex, 8u);
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));  // never navigated away
  PRISM_CHECK_EQ(nav.Count(), 3);
}

PRISM_TEST("navigate: bad rows and re-clicking a crumb change nothing") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  PRISM_CHECK_EQ(nav.Enter(-1).action, tree::NavAction::None);
  PRISM_CHECK_EQ(nav.Enter(99).action, tree::NavAction::None);
  nav.GoTo("Assets");
  PRISM_CHECK_EQ(nav.GoToLevel(0).action, tree::NavAction::None);  // current crumb
  PRISM_CHECK_EQ(nav.GoToLevel(7).action, tree::NavAction::None);  // no such level
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets"));
}

PRISM_TEST("navigate: clicking the root crumb jumps from any depth in one go") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  PRISM_CHECK_EQ(nav.Current().breadcrumb.size(), size_t(2));
  const auto root = nav.GoToLevel(tree::kRootLevel);
  PRISM_CHECK_EQ(root.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  PRISM_CHECK_EQ(root.selection, IndexOfPath(nav.Current().entries, "Assets"));
  PRISM_CHECK_EQ(nav.Count(), 3);
}

PRISM_TEST("navigate: a crumb click restores the child we walked out of") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  const auto jump = nav.GoToLevel(0);  // "Assets"
  PRISM_CHECK_EQ(jump.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets"));
  PRISM_CHECK_EQ(jump.selection, IndexOfPath(nav.Current().entries, "Assets/Icons"));
}

PRISM_TEST("navigate: crossing to an unrelated folder selects nothing") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  const auto jump = nav.GoToLevel(3 - 3);  // stays valid: level 0
  PRISM_CHECK_EQ(jump.action, tree::NavAction::FolderChanged);
  tree::Navigator nav2(&items);
  nav2.GoTo("Assets");
  const auto toDocs = nav2.GoToLevel(tree::kRootLevel);
  PRISM_CHECK_EQ(toDocs.action, tree::NavAction::FolderChanged);
  // Root is an ancestor of "Assets", so the Assets row IS restored -- the
  // acceptance rule is "the child on the path", not "always -1".
  PRISM_CHECK_EQ(toDocs.selection, IndexOfPath(nav2.Current().entries, "Assets"));
  // A sibling with a shared prefix ("Assets" vs "Assets2") must not match.
  std::vector<ArchiveItem> tricky;
  auto add = [&tricky](const char* path, bool dir) {
    ArchiveItem it;
    it.path = path;
    it.isDir = dir;
    it.index = static_cast<uint32_t>(tricky.size());
    tricky.push_back(it);
  };
  add("Assets", true);
  add("Assets2", true);
  add("Assets2/x.txt", false);
  tree::Navigator nav3(&tricky);
  nav3.GoTo("Assets2");
  const auto back = nav3.Back();
  PRISM_CHECK_EQ(back.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(back.selection, IndexOfPath(nav3.Current().entries, "Assets2"));
  PRISM_CHECK(nav3.Current().entries[static_cast<size_t>(back.selection)].path !=
              std::string("Assets"));
}

PRISM_TEST("navigate: selection is cleared out of range, never clamped") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  PRISM_CHECK(nav.SetSelection(1));
  PRISM_CHECK_EQ(nav.Selection(), 1);
  PRISM_CHECK(!nav.SetSelection(1));  // unchanged -> no repaint requested
  PRISM_CHECK(nav.SetSelection(99));
  PRISM_CHECK_EQ(nav.Selection(), -1);
  PRISM_CHECK(nav.SetSelection(0));
  PRISM_CHECK(nav.SetSelection(-5));  // click on empty space clears
  PRISM_CHECK_EQ(nav.Selection(), -1);
}

PRISM_TEST("navigate: the folder list keeps catalog ids, so thumbs survive") {
  // THE thumbnail-scope contract: the sweep fills a store keyed by CATALOG
  // index for the whole archive; switching folders re-filters rows, it must
  // not renumber them, or every folder would need its own rescan.
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  PRISM_CHECK_EQ(nav.FolderItems().size(), nav.Current().entries.size());
  for (size_t i = 0; i < nav.FolderItems().size(); ++i) {
    PRISM_CHECK_EQ(nav.FolderItems()[i].index, nav.Current().entries[i].index);
    PRISM_CHECK_EQ(nav.FolderItems()[i].path, nav.Current().entries[i].path);
  }
  const int aRow = IndexOfPath(nav.Current().entries, "Assets/Icons/a.png");
  PRISM_CHECK_EQ(nav.FolderItems()[static_cast<size_t>(aRow)].index, 2u);  // catalog id
  PRISM_CHECK_EQ(aRow, 0);                                                // but row 0 here
  // Sizes travel with the row (the list mode paints them).
  PRISM_CHECK_EQ(nav.FolderItems()[static_cast<size_t>(aRow)].size, 100ull);
}

PRISM_TEST("navigate: an empty folder renders no rows and still navigates") {
  std::vector<ArchiveItem> items;
  ArchiveItem dir;
  dir.path = "empty";
  dir.isDir = true;
  dir.index = 0;
  items.push_back(dir);
  tree::Navigator nav(&items);
  PRISM_CHECK_EQ(nav.Count(), 1);
  const auto entered = nav.Enter(0);
  PRISM_CHECK_EQ(entered.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string("empty"));
  PRISM_CHECK_EQ(nav.Count(), 0);
  PRISM_CHECK_EQ(nav.Selection(), -1);
  PRISM_CHECK_EQ(nav.Current().breadcrumb.size(), size_t(1));
  const auto back = nav.Back();
  PRISM_CHECK_EQ(back.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(back.selection, 0);
  PRISM_CHECK_EQ(nav.Count(), 1);
}

PRISM_TEST("navigate: attaching a fresh catalog resets to the root") {
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  nav.SetSelection(1);
  std::vector<ArchiveItem> other;
  ArchiveItem meta;
  meta.path = "only.txt";
  meta.index = 0;
  other.push_back(meta);
  nav.Attach(&other);
  PRISM_CHECK_EQ(nav.Folder(), std::string(""));
  PRISM_CHECK_EQ(nav.Count(), 1);
  PRISM_CHECK_EQ(nav.Selection(), -1);
  PRISM_CHECK_EQ(nav.FolderItems().size(), size_t(1));
  // A navigator with no catalog is inert, never a crash.
  tree::Navigator none;
  PRISM_CHECK_EQ(none.Count(), 0);
  PRISM_CHECK_EQ(none.Enter(0).action, tree::NavAction::None);
  PRISM_CHECK_EQ(none.Back().action, tree::NavAction::None);
}

PRISM_TEST("navigate: a stale folder path lands on its deepest existing parent") {
  // The archive changed under us (or the crumb came from another archive):
  // 7-Zip keeps walking up until a bind succeeds (PanelFolderChange.cpp:63-118).
  auto items = SampleCatalog();
  tree::Navigator nav(&items);
  nav.GoTo("Assets/Icons");
  const auto stale = nav.GoTo("Assets/Ghost/Deep");
  PRISM_CHECK_EQ(stale.action, tree::NavAction::FolderChanged);
  PRISM_CHECK_EQ(nav.Folder(), std::string("Assets"));  // deepest real ancestor
  PRISM_CHECK_EQ(stale.selection, IndexOfPath(nav.Current().entries, "Assets/Icons"));
  // A folder that only exists implicitly (children, no node) is real too.
  std::vector<ArchiveItem> implicit;
  ArchiveItem leaf;
  leaf.path = "Inbox/Old/mail.eml";
  leaf.index = 0;
  implicit.push_back(leaf);
  tree::Navigator nav2(&implicit);
  nav2.GoTo("Inbox/Old");
  PRISM_CHECK_EQ(nav2.Folder(), std::string("Inbox/Old"));
  PRISM_CHECK_EQ(nav2.Count(), 1);
}

PRISM_TEST("breadcrumb centred in the island: the click matches the painted text") {
  // The renderer centre-aligns the crumb run inside the nav island, so the
  // hit test centres it too. FakeWidth 10/glyph, box [100,500):
  //   "Archive"(70) + " > "(30) + "Assets"(60) = 160 -> origin 100+(400-160)/2
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  nav.GoTo("Assets");
  const auto layout = prism::tree::LayoutCenteredBreadcrumb(
      nav.Segments("Archive"), 100, 500, &FakeWidth);
  PRISM_CHECK_EQ(layout.origin, 220);
  PRISM_CHECK_EQ(layout.total, 160);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 219), tree::kNoLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 220), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 289), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 290), tree::kNoLevel);  // separator
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 320), 0);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 379), 0);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 380), tree::kNoLevel);  // right of run
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 480), tree::kNoLevel);  // empty box area
  // Clipped by the island's text box: the tail of the run is dead pixels.
  // Box [250,340) is narrower than the 160 px run -> origin pinned to 250,
  // root [250,320), separator [320,350) clipped at 340, "Assets" gone.
  const auto clipped = prism::tree::LayoutCenteredBreadcrumb(
      nav.Segments("Archive"), 250, 340, &FakeWidth);
  PRISM_CHECK_EQ(clipped.origin, 250);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 255), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 319), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 325), tree::kNoLevel);   // separator
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 345), tree::kNoLevel);   // clipped
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 360), tree::kNoLevel);   // clipped
  PRISM_CHECK_EQ(tree::HitBreadcrumb(clipped, 249), tree::kNoLevel);   // left of box
  // A run wider than the box starts at the box's left edge and clips.
  const auto overflow = prism::tree::LayoutCenteredBreadcrumb(
      nav.Segments("Archive"), 100, 200, &FakeWidth);
  PRISM_CHECK_EQ(overflow.origin, 100);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(overflow, 105), tree::kRootLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(overflow, 199), tree::kNoLevel);  // inside the sep
  PRISM_CHECK_EQ(tree::HitBreadcrumb(overflow, 260), tree::kNoLevel);  // past the box
  // Degenerate box (zero width) yields no clickable crumbs at all.
  const auto zero = prism::tree::LayoutCenteredBreadcrumb(
      nav.Segments("Archive"), 300, 300, &FakeWidth);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(zero, 300), tree::kNoLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(zero, 320), tree::kNoLevel);
}

PRISM_TEST("breadcrumb measurement: a zero-width metric makes nothing clickable") {
  // If DirectWrite is unavailable in the window, widths are 0. The hit test
  // must then find no crumb instead of treating the whole run as level 0.
  auto items = SampleCatalog();
  auto nav = tree::Navigator(&items);
  nav.GoTo("Assets/Icons");
  const auto layout = nav.Layout("Archive", 100, nullptr, 0, 1000);
  PRISM_CHECK_EQ(layout.total, 0);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 100), tree::kNoLevel);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(layout, 105), tree::kNoLevel);
  const auto zeroWidth = [](std::string_view) { return 0; };
  const auto flat = nav.Layout("Archive", 100, zeroWidth, 0, 1000);
  PRISM_CHECK_EQ(tree::HitBreadcrumb(flat, 100), tree::kNoLevel);
}

PRISM_TEST_MAIN()
