// PrismArchive -- PrismWin
// Pixel-level tests for SidebarView (card C12, blueprint 3.2).
//
// SidebarView emits drawing COMMANDS, not pixels, and that is exactly why this
// suite can assert the layout to the pixel without a window, a GPU or a font:
// every rectangle below is an exact function of the design tokens. The test
// pins:
//   * the column and capsule geometry (panel, anchored status pill);
//   * the row stack of FAVORITES / RECENT / ARCHIVE TREE with exact row boxes;
//   * the tree indentation and chevron states;
//   * the keyboard-focus Selection rectangle;
//   * the scroll clip: rows fully above the viewport or under the pill are
//     culled, while contentHeight/maxScroll stay scroll-invariant;
//   * the telemetry capsule's three lines and the ratio bar;
//   * the empty state (no archive open).
//
// The stdout marker is part of the verdict (CONTRACTS D1): a silent skip can
// never count as a pass.
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using prism::ArchiveItem;
using prism::sidebar::Encryption;
using prism::sidebar::FocusTarget;
using prism::sidebar::Section;
using prism::win::sidebar::Box;
using prism::win::sidebar::Command;
using prism::win::sidebar::Layout;
using prism::win::sidebar::Metrics;
using prism::win::sidebar::PrimKind;

namespace {

ArchiveItem File(const char* path, std::uint32_t index, std::uint64_t size,
                 std::uint64_t packed) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  item.packSize = packed;
  return item;
}

ArchiveItem Dir(const char* path, std::uint32_t index) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.isDir = true;
  return item;
}

std::vector<ArchiveItem> SampleCatalog() {
  return {
      Dir("Design_Assets", 1),
      Dir("Design_Assets/01_Brand", 2),
      File("Design_Assets/01_Brand/logo.svg", 3, 1200, 400),
      File("Design_Assets/02_UI_Kit/Hero.webp", 4, 18400000, 14200000),
      File("Design_Assets/03_Raw/photo.jpg", 5, 9000000, 8800000),
      File("root.png", 6, 500, 300),
  };
}

// A model that fills every section, so the vertical stack is fully exercised.
prism::sidebar::Model FullModel(std::vector<ArchiveItem>* items) {
  prism::sidebar::Model model(items);
  model.SetArchiveName("Design_Assets_v4.7z");
  model.SetFavorites({
      prism::sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"},
      prism::sidebar::Favorite{"Downloads", "C:/Users/designer/Downloads", "download"},
  });
  model.PushRecent("D:/Work/Design_Assets.7z", 500);
  return model;
}

const Command* FindLabel(const Layout& layout, std::string_view text) {
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::Label && command.text == text) return &command;
  }
  return nullptr;
}

const Command* FindSection(const Layout& layout, std::string_view text) {
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::SectionLabel && command.text == text) return &command;
  }
  return nullptr;
}

const Command* FindStatus(const Layout& layout, std::string_view text) {
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::StatusText && command.text == text) return &command;
  }
  return nullptr;
}

std::vector<const Command*> OfKind(const Layout& layout, PrimKind kind) {
  std::vector<const Command*> out;
  for (const Command& command : layout.commands) {
    if (command.kind == kind) out.push_back(&command);
  }
  return out;
}

bool Near(double a, double b, double epsilon = 0.01) { return std::fabs(a - b) < epsilon; }

bool SameBox(const Box& a, const Box& b) { return a == b; }

bool BoxInsideViewport(const Box& box, float top, float bottom) {
  return box.bottom > top && box.top < bottom;
}

// ------------------------------------------------------------------ geometry
PRISM_TEST("TestColumnAndPillGeometry") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  const Metrics metrics;
  const Layout layout = prism::win::sidebar::Build(model, metrics);

  PRISM_CHECK(SameBox(layout.panel, Box{0.0f, 0.0f, 236.0f, 1080.0f}));
  // The capsule is anchored: its bottom edge sits paddingBottom above the
  // viewport bottom, regardless of how much content is scrolled.
  PRISM_CHECK(SameBox(layout.pill, Box{16.0f, 982.0f, 220.0f, 1068.0f}));
  PRISM_CHECK(Near(layout.contentTop, 12.0f));
  PRISM_CHECK(Near(layout.contentBottom, 970.0f));
  PRISM_CHECK(layout.contentHeight > 0.0f);
  // This fixture fits: no scroll needed, and the clamp is explicit.
  PRISM_CHECK(Near(layout.maxScroll, 0.0f));
  // The capsule sits over the content area, so it reduces the scroll window.
  PRISM_CHECK(Near(layout.contentBottom - layout.contentTop, 958.0f));
  // First command is the panel background: paint order is part of the contract.
  PRISM_CHECK_EQ(layout.commands.front().kind, PrimKind::Panel);
}

PRISM_TEST("TestSectionStackAndRowBoxes") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});

  // FAVORITES: header at y=12 (h=18), rows at 30 and 58.
  const Command* favoritesHeader = FindSection(layout, "FAVORITES");
  PRISM_CHECK(favoritesHeader != nullptr);
  if (favoritesHeader != nullptr) {
    PRISM_CHECK(SameBox(favoritesHeader->box, Box{16.0f, 12.0f, 220.0f, 30.0f}));
  }
  const Command* desktop = FindLabel(layout, "Desktop");
  PRISM_CHECK(desktop != nullptr);
  if (desktop != nullptr) {
    PRISM_CHECK(SameBox(desktop->box, Box{40.0f, 30.0f, 220.0f, 58.0f}));
    PRISM_CHECK_EQ(desktop->section, Section::Favorites);
    PRISM_CHECK_EQ(desktop->payload, 0);
  }
  const Command* downloads = FindLabel(layout, "Downloads");
  PRISM_CHECK(downloads != nullptr);
  if (downloads != nullptr) {
    PRISM_CHECK(SameBox(downloads->box, Box{40.0f, 58.0f, 220.0f, 86.0f}));
  }

  // RECENT starts after the favorites block plus the section gap.
  PRISM_CHECK(FindSection(layout, "RECENT") != nullptr);
  const Command* recent = FindLabel(layout, "Design_Assets.7z");
  PRISM_CHECK(recent != nullptr);
  if (recent != nullptr) {
    PRISM_CHECK_EQ(recent->section, Section::Recent);
    PRISM_CHECK(SameBox(recent->box, Box{40.0f, 118.0f, 220.0f, 146.0f}));
  }

  // ARCHIVE TREE: header 160..178, root row 178..206, children below.
  const Command* treeHeader = FindSection(layout, "ARCHIVE TREE");
  PRISM_CHECK(treeHeader != nullptr);
  if (treeHeader != nullptr) {
    PRISM_CHECK(SameBox(treeHeader->box, Box{16.0f, 160.0f, 220.0f, 178.0f}));
  }
  const Command* root = FindLabel(layout, "Design_Assets_v4.7z");
  PRISM_CHECK(root != nullptr);
  if (root != nullptr) {
    PRISM_CHECK(SameBox(root->box, Box{56.0f, 178.0f, 220.0f, 206.0f}));
    PRISM_CHECK_EQ(root->section, Section::Tree);
    PRISM_CHECK_EQ(root->payload, 0);
  }
  const Command* design = FindLabel(layout, "Design_Assets");
  PRISM_CHECK(design != nullptr);
  if (design != nullptr) {
    // depth 1: left = paddingX + indent
    PRISM_CHECK(SameBox(design->box, Box{70.0f, 206.0f, 220.0f, 234.0f}));
    PRISM_CHECK_EQ(design->payload, 1);
  }
}

PRISM_TEST("TestTreeIndentAndChevrons") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});

  const auto labels = OfKind(layout, PrimKind::Label);
  const Command* brand = nullptr;
  for (const Command* label : labels) {
    if (label->text == "01_Brand") brand = label;
  }
  PRISM_CHECK(brand != nullptr);
  if (brand != nullptr) {
    // depth 2: left = 16 + 2 * 14 = 44, text at +16 chevron +16 icon +8 gap
    PRISM_CHECK(Near(brand->box.left, 44.0f + 16.0f + 8.0f + 16.0f));
  }

  // Open folder -> chevron-down on its row; closed -> chevron-right.
  bool sawDown = false;
  for (const Command* chevron : OfKind(layout, PrimKind::Chevron)) {
    if (chevron->icon == "chevron-down") sawDown = true;
  }
  PRISM_CHECK(sawDown);
  model.SetCollapsed("Design_Assets", true);
  const Layout collapsed = prism::win::sidebar::Build(model, Metrics{});
  bool sawRight = false;
  for (const Command* chevron : OfKind(collapsed, PrimKind::Chevron)) {
    if (chevron->icon == "chevron-right") sawRight = true;
  }
  PRISM_CHECK(sawRight);
  // 01_Brand is gone from the paint list once its parent is closed.
  PRISM_CHECK(FindLabel(collapsed, "01_Brand") == nullptr);
  // Non-expandable folders still reserve the chevron slot: their label starts
  // at the same x as their expandable siblings (no 16 px jitter).
  model.SetCollapsed("Design_Assets", false);
  const Layout expanded = prism::win::sidebar::Build(model, Metrics{});
  const Command* raw = FindLabel(expanded, "03_Raw");
  PRISM_CHECK(raw != nullptr);
  if (raw != nullptr) {
    PRISM_CHECK_EQ(static_cast<int>(raw->box.left), 44 + 16 + 8 + 16);
  }
}

PRISM_TEST("TestSelectionFollowsKeyboardFocus") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  PRISM_CHECK(model.SetFocus(FocusTarget{Section::Tree, 1}));
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});
  const auto selection = OfKind(layout, PrimKind::Selection);
  PRISM_CHECK_EQ(selection.size(), std::size_t{1});
  if (!selection.empty()) {
    PRISM_CHECK(SameBox(selection[0]->box, Box{30.0f, 206.0f, 220.0f, 234.0f}));
    PRISM_CHECK_EQ(selection[0]->section, Section::Tree);
    PRISM_CHECK_EQ(selection[0]->payload, 1);
  }
  // Moving the focus moves the highlight: no stale selection is left behind.
  PRISM_CHECK_EQ(model.MoveDown(), prism::sidebar::FocusAction::Moved);
  const Layout moved = prism::win::sidebar::Build(model, Metrics{});
  const auto movedSelection = OfKind(moved, PrimKind::Selection);
  PRISM_CHECK_EQ(movedSelection.size(), std::size_t{1});
  if (!movedSelection.empty()) {
    PRISM_CHECK_EQ(movedSelection[0]->payload, 2);
  }
}

PRISM_TEST("TestScrollCullsRowsButKeepsContentHeight") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  Metrics scrolled;
  scrolled.scrollY = 320.0f;
  const Layout top = prism::win::sidebar::Build(model, Metrics{});
  const Layout down = prism::win::sidebar::Build(model, scrolled);

  // Scroll-invariant: the scrollbar length must not jump as rows are culled.
  PRISM_CHECK(Near(top.contentHeight, down.contentHeight));
  PRISM_CHECK(Near(top.maxScroll, down.maxScroll));
  PRISM_CHECK(FindLabel(top, "Desktop") != nullptr);
  PRISM_CHECK(FindLabel(down, "Desktop") == nullptr);
  // Nothing that survived the cull may be entirely outside the viewport or
  // hidden under the status capsule.
  for (const Command& command : down.commands) {
    if (command.kind == PrimKind::Panel || command.kind == PrimKind::StatusPill ||
        command.kind == PrimKind::Separator || command.kind == PrimKind::RatioBar ||
        command.kind == PrimKind::StatusIcon || command.kind == PrimKind::StatusText) {
      continue;
    }
    PRISM_CHECK(BoxInsideViewport(command.box, 0.0f, down.contentBottom));
  }
}

PRISM_TEST("TestStatusCapsuleLinesAndRatioBar") {
  std::vector<ArchiveItem> items = {File("payload.bin", 0, 1420, 608)};
  prism::sidebar::Model model(&items);
  model.SetArchiveInfo(
      prism::sidebar::ArchiveInfo{"LZMA2", 64ull * 1024 * 1024, Encryption::Aes256});
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});

  const Command* encrypted = FindStatus(layout, "AES-256 ENCRYPTED");
  const Command* dictionary = FindStatus(layout, "LZMA2 | DICT 64.0 MB");
  const Command* ratio = FindStatus(layout, "RATIO 42.8%");
  PRISM_CHECK(encrypted != nullptr);
  PRISM_CHECK(dictionary != nullptr);
  PRISM_CHECK(ratio != nullptr);
  if (encrypted != nullptr) {
    // Pill inner box: left 16+10, text after 16 px icon + 8 px gap.
    PRISM_CHECK(SameBox(encrypted->box, Box{50.0f, 992.0f, 210.0f, 1014.0f}));
  }
  if (dictionary != nullptr) {
    PRISM_CHECK(SameBox(dictionary->box, Box{50.0f, 1014.0f, 210.0f, 1036.0f}));
  }
  if (ratio != nullptr) {
    PRISM_CHECK(SameBox(ratio->box, Box{50.0f, 1036.0f, 210.0f, 1058.0f}));
  }
  const auto bars = OfKind(layout, PrimKind::RatioBar);
  PRISM_CHECK_EQ(bars.size(), std::size_t{1});
  if (!bars.empty()) {
    // 42.8% of the 160 px inner width -> 68.48 px.
    PRISM_CHECK(Near(bars[0]->box.left, 50.0));
    PRISM_CHECK(Near(bars[0]->box.right, 50.0 + 160.0 * 0.428));
  }
  // The capsule is drawn after the content: it is never overdrawn by rows.
  std::size_t pillIndex = 0;
  std::size_t lastRowIndex = 0;
  for (std::size_t i = 0; i < layout.commands.size(); ++i) {
    if (layout.commands[i].kind == PrimKind::StatusPill) pillIndex = i;
    if (layout.commands[i].kind == PrimKind::RowHit) lastRowIndex = i;
  }
  PRISM_CHECK(pillIndex > lastRowIndex);
}

PRISM_TEST("TestEmptyStateWithoutArchive") {
  prism::sidebar::Model model;  // no catalog
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});
  bool emptyState = false;
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::EmptyState && command.text == "NO ARCHIVE OPEN") {
      emptyState = true;
      PRISM_CHECK_EQ(command.icon, std::string("empty"));
      PRISM_CHECK(Near(command.box.top, 30.0));  // tree header 12..30
    }
  }
  PRISM_CHECK(emptyState);
  PRISM_CHECK(FindSection(layout, "FAVORITES") == nullptr);
  PRISM_CHECK(FindSection(layout, "RECENT") == nullptr);
  PRISM_CHECK(FindSection(layout, "ARCHIVE TREE") != nullptr);
  PRISM_CHECK(FindStatus(layout, "NO ARCHIVE") != nullptr);
  PRISM_CHECK(OfKind(layout, PrimKind::RatioBar).empty());
  // The capsule is dimmed, not missing.
  bool dimmedPill = false;
  for (const Command* pill : OfKind(layout, PrimKind::StatusPill)) {
    dimmedPill = Near(pill->opacity, 0.6);
  }
  PRISM_CHECK(dimmedPill);
}

PRISM_TEST("TestRowPayloadsCoverEverySection") {
  auto items = SampleCatalog();
  prism::sidebar::Model model = FullModel(&items);
  const Layout layout = prism::win::sidebar::Build(model, Metrics{});
  const auto hits = OfKind(layout, PrimKind::RowHit);
  PRISM_CHECK_EQ(hits.size(), model.Favorites().size() + model.Recent().size() +
                                  model.Tree().size());
  std::size_t favorites = 0;
  std::size_t recent = 0;
  std::size_t tree = 0;
  for (const Command* hit : hits) {
    if (hit->section == Section::Favorites) ++favorites;
    if (hit->section == Section::Recent) ++recent;
    if (hit->section == Section::Tree) ++tree;
    PRISM_CHECK_EQ(hit->opacity, 0.0f);  // a hit region paints nothing
  }
  PRISM_CHECK_EQ(favorites, model.Favorites().size());
  PRISM_CHECK_EQ(recent, model.Recent().size());
  PRISM_CHECK_EQ(tree, model.Tree().size());
}

PRISM_TEST("TestFormattingHelpers") {
  PRISM_CHECK_EQ(prism::win::sidebar::FormatRatio(428), std::string("42.8%"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatRatio(0), std::string("0.0%"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatRatio(1000), std::string("100.0%"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatBytes(0), std::string("0 B"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatBytes(512), std::string("512 B"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatBytes(64ull * 1024 * 1024),
                 std::string("64.0 MB"));
  PRISM_CHECK_EQ(prism::win::sidebar::FormatBytes(1420000000ull),
                 std::string("1.3 GB"));
  PRISM_CHECK_EQ(prism::win::sidebar::EncryptionLabel(Encryption::Aes256),
                 std::string("AES-256 ENCRYPTED"));
  PRISM_CHECK_EQ(prism::win::sidebar::EncryptionLabel(Encryption::Unknown),
                 std::string("ENCRYPTED"));
  PRISM_CHECK_EQ(prism::win::sidebar::EncryptionLabel(Encryption::None),
                 std::string("NOT ENCRYPTED"));
  // A favorite with no label falls back to the path's leaf.
  PRISM_CHECK_EQ(prism::win::sidebar::RowLabel("", "D:/Work/Archives"),
                 std::string("Archives"));
}

// The CONTRACTS D3 switchboard: true in every shipping build, flipped to false
// by exactly one mutation target (WILL_FAIL).
PRISM_TEST("TestMutationSwitchesAreOnInTheCleanBuild") {
  PRISM_CHECK(prism::win::sidebar::kIndentEnabled);
  PRISM_CHECK(prism::win::sidebar::kCullEnabled);
}

}  // namespace

#if defined(PRISM_SIDEBAR_MUTANT_VIEW_NO_INDENT)
#define PRISM_SIDEBAR_VIEW_MUTANT_ACTIVE "no-indent"
#elif defined(PRISM_SIDEBAR_MUTANT_VIEW_NO_CULL)
#define PRISM_SIDEBAR_VIEW_MUTANT_ACTIVE "no-cull"
#endif

int main() {
  std::cout << "=== sidebar view (card C12) ===" << std::endl;
#ifdef PRISM_SIDEBAR_VIEW_MUTANT_ACTIVE
  // CONTRACTS D3: prove the injection reached THIS translation unit. The
  // mutation target is registered WILL_FAIL, so a dropped compile definition
  // turns the target green and CTest red.
  std::cout << "MUTANT ACTIVE: " << PRISM_SIDEBAR_VIEW_MUTANT_ACTIVE
            << " (the guarded geometry below must fail)" << std::endl;
#endif
  const int rc = prism::test::RunAll(0, nullptr);
  // Same anti-fake-green guard as the model suite: the marker is only printed
  // when the cases actually ran.
  if (prism::test::CheckCount() < 40 || prism::test::Registry().size() < 10) {
    std::cout << "SUITE DID NOT RUN: " << prism::test::Registry().size() << " cases, "
              << prism::test::CheckCount() << " checks" << std::endl;
    return 1;
  }
  if (rc == 0) std::cout << "ALL SIDEBAR-VIEW CHECKS PASSED" << std::endl;
  return rc;
}