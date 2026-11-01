// PrismArchive -- PrismCore
// Tests for SidebarModel (card C12, blueprint 3.2): FAVORITES / RECENT /
// ARCHIVE TREE / status pill / keyboard focus.
//
// The assertions are about CONTENT, never about "something happened":
// exact MRU order after de-duplication and eviction, the exact folder rows the
// blueprint's tree would paint, the exact focus target after Home/End/Left/Right,
// the exact permille ratio the pill prints.
//
// The tree's path semantics are not asserted against a hand-written expectation
// alone: a differential case walks every folder of the model and compares its
// children with tree::Build -- the authority the content list uses (CONTRACTS
// D13, "compare against the authority, not against my own model").
#include "mini_test.hpp"

#include "../Src/PrismCore/ArchiveTreeView.hpp"
#include "../Src/PrismCore/SidebarModel.hpp"

#include <cstdint>
#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace prism;
using prism::sidebar::Encryption;
using prism::sidebar::FocusAction;
using prism::sidebar::FocusTarget;
using prism::sidebar::Section;

namespace {

ArchiveItem File(const char* path, std::uint32_t index, std::uint64_t size,
                 std::uint64_t packed, bool encrypted = false) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  item.packSize = packed;
  item.encrypted = encrypted;
  return item;
}

ArchiveItem Dir(const char* path, std::uint32_t index) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.isDir = true;
  return item;
}

// Shaped like the blueprint's screenshot: three top folders, one of them only
// implied by its children, one nested level, and "v2"/"v10" so the natural
// (digit-run) comparator is actually exercised.
std::vector<ArchiveItem> SampleCatalog() {
  return {
      Dir("Design_Assets", 1),
      Dir("Design_Assets/01_Brand", 2),
      File("Design_Assets/01_Brand/logo.svg", 3, 1200, 400),
      File("Design_Assets/01_Brand/guidelines.pdf", 4, 4200, 1100),
      File("Design_Assets/02_UI_Kit/Hero_Banner_4K.webp", 5, 18400000, 14200000),
      File("Design_Assets/02_UI_Kit/sub/deep.txt", 6, 640, 200),
      Dir("Design_Assets/03_Raw_Photos", 7),
      File("Design_Assets/03_Raw_Photos/DSC_0001.jpg", 8, 9000000, 8800000),
      File("v2/readme.md", 9, 50, 40),
      File("v10/readme.md", 10, 60, 45),
      File("root.png", 11, 500, 300),
  };
}

// --------------------------------------------------------------- favourites
PRISM_TEST("TestFavoritesKeepConfiguredOrderAndIcons") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetFavorites({
      sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"},
      sidebar::Favorite{"Downloads", "C:/Users/designer/Downloads", "download"},
      sidebar::Favorite{"Work", "D:/Work/Archives", "work"},
  });
  const auto& favorites = model.Favorites();
  PRISM_CHECK_EQ(favorites.size(), std::size_t{3});
  PRISM_CHECK_EQ(favorites[0].label, std::string("Desktop"));
  PRISM_CHECK_EQ(favorites[2].path, std::string("D:/Work/Archives"));
  PRISM_CHECK_EQ(favorites[1].icon, std::string("download"));
}

// ------------------------------------------------------------------- recent
PRISM_TEST("TestRecentDeduplicatesAcrossSeparatorsAndCase") {
  sidebar::Model model;
  model.PushRecent("C:\\Work\\a.7z", 100);
  model.PushRecent("c:/work/A.7Z", 200);  // same file, other spelling
  PRISM_CHECK_EQ(model.Recent().size(), std::size_t{1});
  PRISM_CHECK_EQ(model.Recent()[0].openedAt, std::int64_t{200});
  // Display spelling is normalized to '/', and the newest spelling wins.
  PRISM_CHECK_EQ(model.Recent()[0].path, std::string("c:/work/A.7Z"));
}

PRISM_TEST("TestRecentDuplicateKeepsNewestTimestamp") {
  sidebar::Model model;
  model.PushRecent("D:/a.zip", 500);
  model.PushRecent("E:/b.zip", 700);
  model.PushRecent("D:/a.zip", 300);  // older re-open must not lower the stamp
  PRISM_CHECK_EQ(model.Recent().size(), std::size_t{2});
  PRISM_CHECK_EQ(model.Recent()[0].openedAt, std::int64_t{700});
  std::int64_t aStamp = -1;
  for (const auto& entry : model.Recent()) {
    if (entry.path == "D:/a.zip") aStamp = entry.openedAt;
  }
  PRISM_CHECK_EQ(aStamp, std::int64_t{500});
}

PRISM_TEST("TestRecentIsNewestFirst") {
  sidebar::Model model;
  model.PushRecent("D:/old.7z", 100);
  model.PushRecent("D:/new.7z", 900);
  model.PushRecent("D:/mid.7z", 500);
  PRISM_CHECK_EQ(model.Recent().size(), std::size_t{3});
  PRISM_CHECK_EQ(model.Recent()[0].path, std::string("D:/new.7z"));
  PRISM_CHECK_EQ(model.Recent()[1].path, std::string("D:/mid.7z"));
  PRISM_CHECK_EQ(model.Recent()[2].path, std::string("D:/old.7z"));
}

PRISM_TEST("TestRecentCapacityEvictsTheOldest") {
  sidebar::Model model;
  for (int i = 0; i < 12; ++i) {
    model.PushRecent("D:/archive" + std::to_string(i) + ".7z", i * 10);
  }
  PRISM_CHECK_EQ(model.Recent().size(), sidebar::kRecentCapacity);
  // The eight newest survive: archives 4..11, newest first.
  PRISM_CHECK_EQ(model.Recent().front().path, std::string("D:/archive11.7z"));
  PRISM_CHECK_EQ(model.Recent().back().path, std::string("D:/archive4.7z"));
}

PRISM_TEST("TestRecentIgnoresEmptyPaths") {
  sidebar::Model model;
  model.PushRecent("", 10);
  model.PushRecent("   ", 20);  // whitespace is a path, not emptiness
  PRISM_CHECK_EQ(model.Recent().size(), std::size_t{1});
  PRISM_CHECK_EQ(model.Recent()[0].path, std::string("   "));
}

PRISM_TEST("TestSetRecentNormalizesStoredList") {
  sidebar::Model model;
  model.SetRecent({
      sidebar::RecentEntry{"D:/b.7z", 5},
      sidebar::RecentEntry{"D:/a.7z", 10},
      sidebar::RecentEntry{"d:\\B.7z", 9},  // duplicate of the first
      sidebar::RecentEntry{"", 30},         // dropped
      sidebar::RecentEntry{"D:/c.7z", 1},
  });
  PRISM_CHECK_EQ(model.Recent().size(), std::size_t{3});
  PRISM_CHECK_EQ(model.Recent()[0].path, std::string("D:/a.7z"));
  std::int64_t bStamp = -1;
  for (const auto& entry : model.Recent()) {
    if (entry.path == "D:/b.7z") bStamp = entry.openedAt;
  }
  PRISM_CHECK_EQ(bStamp, std::int64_t{9});  // max stamp of the duplicates
}

// --------------------------------------------------------------------- tree
PRISM_TEST("TestTreeRootAndChildren") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetArchiveName("Design_Assets_v4.7z");
  const auto& rows = model.Tree();
  PRISM_CHECK(rows.size() >= 5);
  PRISM_CHECK_EQ(rows[0].path, std::string(""));
  PRISM_CHECK(rows[0].isRoot);
  PRISM_CHECK_EQ(rows[0].name, std::string("Design_Assets_v4.7z"));
  PRISM_CHECK_EQ(rows[0].depth, 0);
  PRISM_CHECK(rows[0].expanded);
  // Top level folders, natural order: Design_Assets, v2, v10.
  PRISM_CHECK_EQ(rows[1].path, std::string("Design_Assets"));
  PRISM_CHECK_EQ(rows[1].depth, 1);
  PRISM_CHECK_EQ(rows[1].name, std::string("Design_Assets"));
  PRISM_CHECK(rows[1].expandable);
  PRISM_CHECK(rows[1].explicitDir);
  PRISM_CHECK_EQ(rows[1].catalogIndex, std::uint32_t{1});
  // Its first child is 01_Brand, which is explicit.
  PRISM_CHECK_EQ(rows[2].path, std::string("Design_Assets/01_Brand"));
  PRISM_CHECK_EQ(rows[2].depth, 2);
  PRISM_CHECK_EQ(rows[2].catalogIndex, std::uint32_t{2});
  // 02_UI_Kit exists only because of its children -> synthetic index.
  bool sawUiKit = false;
  bool sawDeep = false;
  for (const auto& row : rows) {
    if (row.path == "Design_Assets/02_UI_Kit") {
      sawUiKit = true;
      PRISM_CHECK(!row.explicitDir);
      PRISM_CHECK_EQ(row.catalogIndex, tree::kSyntheticIndex);
      PRISM_CHECK_EQ(row.depth, 2);
      PRISM_CHECK(row.expandable);  // has a sub folder
    }
    if (row.path == "Design_Assets/02_UI_Kit/sub") {
      sawDeep = true;
      PRISM_CHECK_EQ(row.depth, 3);
      PRISM_CHECK(!row.expandable);
    }
  }
  PRISM_CHECK(sawUiKit);
  PRISM_CHECK(sawDeep);
  // Files never appear in the tree.
  for (const auto& row : rows) {
    PRISM_CHECK(row.path.find(".svg") == std::string::npos);
    PRISM_CHECK(row.path.find(".webp") == std::string::npos);
  }
}

// Differential authority check (CONTRACTS D13): every expanded folder's child
// list must equal what tree::Build says it is, path for path and index for
// index. This is what makes "same path semantics as Navigator" a fact instead
// of a claim.
PRISM_TEST("TestTreeMatchesNavigatorAuthority") {
  auto items = SampleCatalog();
  // Hostile / odd entries: a pseudo parent, redundant separators, an implied
  // top-level folder and an explicit folder that also has children.
  items.push_back(File("../evil/payload.txt", 20, 10, 5));
  items.push_back(File("x/./y/note.txt", 21, 10, 5));
  items.push_back(File("/leading//double/slash/file.txt", 22, 10, 5));
  items.push_back(Dir("explicit_and_implied", 23));
  items.push_back(File("explicit_and_implied/child.bin", 24, 10, 5));

  sidebar::Model model(&items);
  const auto& rows = model.Tree();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    const auto& row = rows[i];
    const auto view = tree::Build(items, row.path);
    std::vector<tree::Entry> expected;
    for (const auto& entry : view.entries) {
      if (entry.isDir) expected.push_back(entry);
    }
    std::vector<const sidebar::TreeRow*> actual;
    for (std::size_t j = i + 1; j < rows.size(); ++j) {
      if (rows[j].depth <= row.depth) break;
      if (rows[j].depth == row.depth + 1) actual.push_back(&rows[j]);
    }
    if (!PRISM_CHECK_EQ(actual.size(), expected.size())) continue;
    for (std::size_t k = 0; k < expected.size(); ++k) {
      PRISM_CHECK_EQ(actual[k]->path, expected[k].path);
      PRISM_CHECK_EQ(actual[k]->catalogIndex, expected[k].index);
      PRISM_CHECK_EQ(actual[k]->explicitDir,
                     expected[k].index != tree::kSyntheticIndex);
    }
  }
  // At the root no pseudo folder may appear; the prefix before a pseudo
  // segment does ("x"), exactly as a Navigator walk shows it.
  bool sawX = false;
  bool sawDotDot = false;
  for (const auto& row : rows) {
    if (row.path == "x") sawX = true;
    if (row.path == ".." || row.path.find("../") == 0) sawDotDot = true;
  }
  PRISM_CHECK(sawX);
  PRISM_CHECK(!sawDotDot);
}

PRISM_TEST("TestTreeCollapseHidesSubtreeAndExpandRestoresIt") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  const std::size_t expandedRowCount = model.Tree().size();
  model.SetCollapsed("Design_Assets", true);
  PRISM_CHECK(model.IsCollapsed("Design_Assets"));
  PRISM_CHECK(model.Tree().size() < expandedRowCount);
  for (const auto& row : model.Tree()) {
    PRISM_CHECK(row.path != "Design_Assets/01_Brand");
  }
  // The collapsed folder row itself is still there, now drawn closed.
  bool foundClosed = false;
  for (const auto& row : model.Tree()) {
    if (row.path != "Design_Assets") continue;
    foundClosed = true;
    PRISM_CHECK(!row.expanded);
  }
  PRISM_CHECK(foundClosed);
  model.SetCollapsed("Design_Assets", false);
  PRISM_CHECK_EQ(model.Tree().size(), expandedRowCount);
}

PRISM_TEST("TestCollapseRootHidesEverythingAndSurvivesNormalization") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetCollapsed("\\", true);  // separator-only spelling of the root
  PRISM_CHECK(model.IsCollapsed(""));
  PRISM_CHECK_EQ(model.Tree().size(), std::size_t{1});
  PRISM_CHECK(model.Tree()[0].isRoot);
  PRISM_CHECK(!model.Tree()[0].expanded);
  model.ToggleCollapsed("");
  PRISM_CHECK(!model.IsCollapsed(""));
  PRISM_CHECK(model.Tree().size() > 1);
}

PRISM_TEST("TestCollapseUnknownFolderIsANoOp") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  const std::size_t before = model.Tree().size();
  model.SetCollapsed("does/not/exist", true);
  PRISM_CHECK(!model.IsCollapsed("does/not/exist"));
  PRISM_CHECK_EQ(model.Tree().size(), before);
}

// -------------------------------------------------------------------- focus
PRISM_TEST("TestFocusTraversesSectionsAndCycles") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetFavorites({sidebar::Favorite{"Desktop", "C:/Desktop", "home"}});
  model.PushRecent("D:/a.7z", 10);
  const int treeRows = static_cast<int>(model.Tree().size());

  PRISM_CHECK_EQ(model.Home(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Favorites);
  PRISM_CHECK_EQ(model.Focus().index, 0);

  PRISM_CHECK_EQ(model.MoveDown(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Recent);
  PRISM_CHECK_EQ(model.MoveDown(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Tree);
  PRISM_CHECK_EQ(model.Focus().index, 0);

  PRISM_CHECK_EQ(model.End(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Tree);
  PRISM_CHECK_EQ(model.Focus().index, treeRows - 1);
  // The focus cycles: Down at the last target wraps to the first.
  PRISM_CHECK_EQ(model.MoveDown(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Favorites);
  // And Up at the first wraps to the last.
  PRISM_CHECK_EQ(model.MoveUp(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Focus().section, Section::Tree);
  PRISM_CHECK_EQ(model.Focus().index, treeRows - 1);
}

PRISM_TEST("TestFocusLeftCollapsesThenWalksUp") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  int uiKit = -1;
  int brand = -1;
  const auto& rows = model.Tree();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == "Design_Assets/02_UI_Kit") uiKit = static_cast<int>(i);
    if (rows[i].path == "Design_Assets/01_Brand") brand = static_cast<int>(i);
  }
  PRISM_CHECK(uiKit >= 0);
  PRISM_CHECK(brand >= 0);

  // 01_Brand has files only -> not expandable, so Left walks to its parent.
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, brand}));
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets"));

  // An expanded folder collapses in place...
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, uiKit}));
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets/02_UI_Kit"));
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Toggled);
  PRISM_CHECK(model.IsCollapsed("Design_Assets/02_UI_Kit"));
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets/02_UI_Kit"));
  // ...and a collapsed one walks to its parent.
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets"));
  // Collapsing the root is the root row's own toggle; it has no parent.
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Toggled);
  PRISM_CHECK(model.IsCollapsed("Design_Assets"));
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.FocusedPath(), std::string(""));
  // The root row is expandable too: Left closes it, and only then is there
  // nothing above to walk to.
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Toggled);
  PRISM_CHECK(model.IsCollapsed(""));
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::None);
}

PRISM_TEST("TestFocusRightExpandsThenEntersFirstChild") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, 1}));  // Design_Assets
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets"));
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::Toggled);  // collapse
  PRISM_CHECK(model.IsCollapsed("Design_Assets"));
  PRISM_CHECK_EQ(model.MoveRight(), FocusAction::Toggled);  // expand
  PRISM_CHECK(!model.IsCollapsed("Design_Assets"));
  PRISM_CHECK_EQ(model.MoveRight(), FocusAction::Moved);  // first child
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets/01_Brand"));
  // A leaf folder cannot expand or descend.
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, 2}));  // 01_Brand
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets/01_Brand"));
  PRISM_CHECK_EQ(model.MoveRight(), FocusAction::None);
}

PRISM_TEST("TestCollapsingAncestorRescuesHiddenFocus") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  int deep = -1;
  const auto& rows = model.Tree();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == "Design_Assets/02_UI_Kit/sub") deep = static_cast<int>(i);
  }
  PRISM_CHECK(deep >= 0);
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, deep}));
  model.SetCollapsed("Design_Assets/02_UI_Kit", true);
  PRISM_CHECK_EQ(model.FocusedPath(), std::string("Design_Assets/02_UI_Kit"));
}

PRISM_TEST("TestFocusSurvivesRecentAndFavoriteInsertion") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, 1}));
  const std::string before = model.FocusedPath();
  model.PushRecent("D:/newest.7z", 999);
  PRISM_CHECK_EQ(model.FocusedPath(), before);
  PRISM_CHECK_EQ(model.Focus().section, Section::Tree);
  // Favorites inserted in front keep the tree focus too.
  model.SetFavorites({sidebar::Favorite{"A", "C:/A", "home"},
                      sidebar::Favorite{"B", "C:/B", "work"}});
  PRISM_CHECK_EQ(model.FocusedPath(), before);
}

PRISM_TEST("TestEmptyModelFocusIsInert") {
  sidebar::Model model;  // no catalog at all
  PRISM_CHECK(!model.Focus().Valid());
  PRISM_CHECK_EQ(model.Home(), FocusAction::None);
  PRISM_CHECK_EQ(model.End(), FocusAction::None);
  PRISM_CHECK_EQ(model.MoveDown(), FocusAction::None);
  PRISM_CHECK_EQ(model.MoveUp(), FocusAction::None);
  PRISM_CHECK_EQ(model.MoveLeft(), FocusAction::None);
  PRISM_CHECK_EQ(model.MoveRight(), FocusAction::None);
  PRISM_CHECK_EQ(model.Activate(), FocusAction::None);
  PRISM_CHECK_EQ(model.FocusedPath(), std::string(""));
  PRISM_CHECK(!model.SetFocus(FocusTarget{Section::Tree, 0}));
  PRISM_CHECK(model.Tree().empty());
}

PRISM_TEST("TestActivateReportsTheFocusedTarget") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetFavorites({sidebar::Favorite{"Desktop", "C:/Desktop", "home"}});
  PRISM_CHECK_EQ(model.Home(), FocusAction::Moved);
  PRISM_CHECK_EQ(model.Activate(), FocusAction::Activated);
  PRISM_CHECK_EQ(model.ActivatedPath(), std::string("C:/Desktop"));
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, 1}));
  PRISM_CHECK_EQ(model.Activate(), FocusAction::Activated);
  PRISM_CHECK_EQ(model.ActivatedPath(), std::string("Design_Assets"));
}

// ------------------------------------------------------------------- status
PRISM_TEST("TestStatusComputesTotalsAndRatio") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetArchiveInfo(
      sidebar::ArchiveInfo{"LZMA2", 64ull * 1024 * 1024, Encryption::Aes256});
  const sidebar::Status status = model.ComputeStatus();
  PRISM_CHECK(status.hasArchive);
  PRISM_CHECK(status.encrypted);
  PRISM_CHECK_EQ(status.encryption, Encryption::Aes256);
  PRISM_CHECK_EQ(status.method, std::string("LZMA2"));
  PRISM_CHECK_EQ(status.dictionaryBytes, 64ull * 1024 * 1024);
  PRISM_CHECK_EQ(status.fileCount, std::size_t{8});    // dirs are not files
  PRISM_CHECK_EQ(status.folderCount, std::size_t{3});  // explicit dir entries
  std::uint64_t expectedUnpacked = 0;
  std::uint64_t expectedPacked = 0;
  for (const auto& item : items) {
    if (item.isDir) continue;
    expectedUnpacked += item.size;
    expectedPacked += item.packSize;
  }
  PRISM_CHECK_EQ(status.totalUncompressed, expectedUnpacked);
  PRISM_CHECK_EQ(status.totalPacked, expectedPacked);
}

PRISM_TEST("TestStatusRatioMatchesBlueprintPercentage") {
  // Blueprint 3.2 prints "42.8%": 608 MB packed of 1.42 GB unpacked.
  std::vector<ArchiveItem> items = {File("payload.bin", 0, 1420, 608)};
  sidebar::Model model(&items);
  PRISM_CHECK_EQ(model.ComputeStatus().ratioPermille, std::uint32_t{428});
  // Zero-size archive: ratio 0, never a division by zero.
  std::vector<ArchiveItem> emptyArchive;
  sidebar::Model zero(&emptyArchive);
  PRISM_CHECK_EQ(zero.ComputeStatus().ratioPermille, std::uint32_t{0});
  PRISM_CHECK(zero.ComputeStatus().hasArchive);
  PRISM_CHECK_EQ(zero.ComputeStatus().fileCount, std::size_t{0});
}

PRISM_TEST("TestStatusPromotesUnnamedEncryptionToUnknown") {
  std::vector<ArchiveItem> items = {File("secret.txt", 0, 100, 90, true)};
  sidebar::Model model(&items);
  PRISM_CHECK_EQ(model.ComputeStatus().encryption, Encryption::Unknown);
  PRISM_CHECK(model.ComputeStatus().encrypted);
  // An explicit engine answer wins over the heuristic.
  model.SetArchiveInfo(sidebar::ArchiveInfo{"AES", 0, Encryption::Aes256});
  PRISM_CHECK_EQ(model.ComputeStatus().encryption, Encryption::Aes256);
  // No catalog -> nothing to report.
  sidebar::Model none;
  PRISM_CHECK(!none.ComputeStatus().hasArchive);
  PRISM_CHECK_EQ(none.ComputeStatus().encryption, Encryption::None);
}

// -------------------------------------------------------------- persistence
class FakeStore : public sidebar::SidebarStore {
 public:
  std::string Load(std::string_view key) const override {
    const auto it = values.find(std::string(key));
    return it == values.end() ? std::string() : it->second;
  }
  void Store(std::string_view key, std::string_view value) override {
    values[std::string(key)] = std::string(value);
  }
  std::map<std::string, std::string> values;
};

PRISM_TEST("TestPersistenceRoundTripsThroughInjectedPort") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetFavorites({sidebar::Favorite{"Work\tArchives", "D:/Work/Archives", "work"}});
  model.PushRecent("D:/a.zip", 30);
  model.PushRecent("E:/b.7z", 20);
  model.SetCollapsed("Design_Assets/01_Brand", true);
  model.SetCollapsed("", true);

  FakeStore store;
  model.Save(&store);
  PRISM_CHECK_EQ(store.values.size(), std::size_t{1});
  PRISM_CHECK(!store.values.begin()->second.empty());

  sidebar::Model restored(&items);
  PRISM_CHECK(restored.Restore(&store));
  PRISM_CHECK_EQ(restored.Favorites().size(), std::size_t{1});
  PRISM_CHECK_EQ(restored.Favorites()[0].label, std::string("Work\tArchives"));
  PRISM_CHECK_EQ(restored.Favorites()[0].path, std::string("D:/Work/Archives"));
  PRISM_CHECK_EQ(restored.Favorites()[0].icon, std::string("work"));
  PRISM_CHECK_EQ(restored.Recent().size(), std::size_t{2});
  PRISM_CHECK_EQ(restored.Recent()[0].path, std::string("D:/a.zip"));
  PRISM_CHECK_EQ(restored.Recent()[1].openedAt, std::int64_t{20});
  PRISM_CHECK(restored.IsCollapsed("Design_Assets/01_Brand"));
  PRISM_CHECK(restored.IsCollapsed(""));
  // Identical blobs: serialization is deterministic.
  PRISM_CHECK_EQ(restored.Serialize(), model.Serialize());
}

PRISM_TEST("TestPersistenceRejectsForeignBlobsButNeverWipesState") {
  auto items = SampleCatalog();
  sidebar::Model model(&items);
  model.SetFavorites({sidebar::Favorite{"Keep", "C:/Keep", "home"}});
  PRISM_CHECK(!model.Deserialize("not-a-prism-blob\nF\tx\ty\tz"));
  PRISM_CHECK_EQ(model.Favorites().size(), std::size_t{1});

  // Forward compatibility: unknown records and a malformed number are skipped,
  // the valid records around them still land.
  const std::string blob = std::string(sidebar::kSerializationMagic) +
                           "\nZ\tfuture-record\n"
                           "R\tnot-a-number\tD:/bad.7z\n"
                           "R\t40\tD:/good.7z\n"
                           "C\t- \n"
                           "C\tDesign_Assets\n";
  sidebar::Model tolerant(&items);
  PRISM_CHECK(tolerant.Deserialize(blob));
  PRISM_CHECK_EQ(tolerant.Recent().size(), std::size_t{1});
  PRISM_CHECK_EQ(tolerant.Recent()[0].path, std::string("D:/good.7z"));
  PRISM_CHECK(tolerant.IsCollapsed("Design_Assets"));
  PRISM_CHECK(!tolerant.IsCollapsed(""));  // "C\t- " is not the root marker

  // A null store is a no-op, not a crash.
  PRISM_CHECK(!model.Restore(nullptr));
  model.Save(nullptr);
}

PRISM_TEST("TestEmptyArchiveStillHasARootRow") {
  std::vector<ArchiveItem> emptyArchive;
  sidebar::Model model(&emptyArchive);
  PRISM_CHECK(model.ComputeStatus().hasArchive);
  PRISM_CHECK_EQ(model.Tree().size(), std::size_t{1});
  PRISM_CHECK(model.Tree()[0].isRoot);
  PRISM_CHECK(!model.Tree()[0].expandable);
}

// The CONTRACTS D3 switchboard: these three are true in every shipping build.
// A mutation target (WILL_FAIL) flips exactly one of them to false, so the
// target can only stay green while the injection is really compiled in.
PRISM_TEST("TestMutationSwitchesAreOnInTheCleanBuild") {
  PRISM_CHECK(sidebar::kRecentDedupeEnabled);
  PRISM_CHECK(sidebar::kRecentCapEnabled);
  PRISM_CHECK(sidebar::kFocusWrapEnabled);
}

}  // namespace

#if defined(PRISM_SIDEBAR_MUTANT_NO_RECENT_DEDUPE)
#define PRISM_SIDEBAR_MUTANT_ACTIVE "no-recent-dedupe"
#elif defined(PRISM_SIDEBAR_MUTANT_NO_RECENT_CAP)
#define PRISM_SIDEBAR_MUTANT_ACTIVE "no-recent-cap"
#elif defined(PRISM_SIDEBAR_MUTANT_NO_FOCUS_WRAP)
#define PRISM_SIDEBAR_MUTANT_ACTIVE "no-focus-wrap"
#endif

int main() {
  std::cout << "=== sidebar model (card C12) ===" << std::endl;
#ifdef PRISM_SIDEBAR_MUTANT_ACTIVE
  // CONTRACTS D3: prove the injection reached THIS translation unit. The
  // mutation target is registered WILL_FAIL, so a dropped compile definition
  // turns the target green and CTest red.
  std::cout << "MUTANT ACTIVE: " << PRISM_SIDEBAR_MUTANT_ACTIVE
            << " (the guarded assertion above must fail)" << std::endl;
#endif
  const int rc = prism::test::RunAll(0, nullptr);
  // D1 in the other direction: a suite that registered no cases (a forgotten
  // PRISM_TEST, a renamed main) prints "0 checks, 0 failures" and exits 0 --
  // which would make the stdout marker a fake green. Refuse to print it unless
  // the work really happened.
  if (prism::test::CheckCount() < 100 || prism::test::Registry().size() < 20) {
    std::cout << "SUITE DID NOT RUN: " << prism::test::Registry().size() << " cases, "
              << prism::test::CheckCount() << " checks" << std::endl;
    return 1;
  }
  if (rc == 0) std::cout << "ALL SIDEBAR-MODEL CHECKS PASSED" << std::endl;
  return rc;
}