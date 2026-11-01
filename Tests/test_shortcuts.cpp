// PrismArchive -- PrismCore test
// Blueprint ch. 9.1 keyboard table + the two decisions it drives that PrismCore
// owns: the thumbnail zoom ladder and Ctrl+Shift+N's virtual-mkdir policy.
//
// The window keeps the EXECUTION (selection arithmetic, navigation); this file
// locks the DECISIONS, so a future edit in WndProc cannot silently redefine a
// chord. The Windows suite (Tests/win/test_keys.cpp) then asserts that the real
// window behaviour agrees with this table.
#include <cstddef>
#include <string>
#include <vector>

#include "../Src/PrismCore/NewFolder.hpp"
#include "../Src/PrismCore/Shortcuts.hpp"
#include "../Src/PrismCore/VirtualTree.hpp"
#include "mini_test.hpp"

using prism::ArchiveItem;
using prism::keys::Command;
using prism::keys::PointerAction;
using prism::keys::PointerStroke;
using prism::keys::Scope;
using prism::keys::Stroke;
using prism::tree::MkdirResult;
using prism::tree::MkdirStatus;
using prism::VirtualArchiveTree;

namespace {

Command Key(uint32_t key, bool ctrl = false, bool shift = false, bool alt = false,
            Scope scope = Scope::Gallery) {
  return prism::keys::Resolve(Stroke{key, ctrl, shift, alt}, scope);
}

void Add(std::vector<ArchiveItem>* items, const char* path, bool dir, uint64_t size) {
  ArchiveItem item;
  item.index = static_cast<uint32_t>(items->size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  items->push_back(item);
}

std::vector<ArchiveItem> BuildCatalog() {
  std::vector<ArchiveItem> items;
  Add(&items, "docs", true, 0);                 // 0
  Add(&items, "pack", true, 0);                 // 1
  Add(&items, "pack/img1.png", false, 120);     // 2
  Add(&items, "pack/sub", true, 0);             // 3
  Add(&items, "pack/sub/deep.png", false, 60);  // 4
  Add(&items, "root.png", false, 500);          // 5
  Add(&items, "implied", true, 0);              // 6
  Add(&items, "implied/child.txt", false, 5);   // 7
  return items;
}

bool PlanHas(const std::vector<prism::UpdateItem>& plan, const std::string& path) {
  for (const auto& item : plan) {
    if (item.index < 0 && item.newPath == path) return true;
  }
  return false;
}

}  // namespace

PRISM_TEST("keys: the six blueprint 9.1 rows resolve to their commands") {
  // 9.1 row 3: Ctrl+K -> command palette.
  if (!PRISM_CHECK_EQ(Key('K', true), Command::OpenPalette)) return;
  // 9.1 row 4: Ctrl+E -> smart extract without a dialog.
  if (!PRISM_CHECK_EQ(Key('E', true), Command::SmartExtract)) return;
  // 9.1 row 5: Ctrl+Shift+N -> folder inside the archive (CoW transaction).
  if (!PRISM_CHECK_EQ(Key('N', true, true), Command::NewFolder)) return;
  // 9.1 row 2: Ctrl+Shift+P -> thumbnail ladder (64..512 px).
  if (!PRISM_CHECK_EQ(Key('P', true, true), Command::ZoomNext)) return;
  // 9.1 row 6: Alt+Left / Alt+Right -> history.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyLeft, false, false, true), Command::HistoryBack)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyRight, false, false, true), Command::HistoryForward)) return;
  // Batch-A keys restated in the same table.
  if (!PRISM_CHECK_EQ(Key('1', true), Command::ToggleGrid)) return;
  if (!PRISM_CHECK_EQ(Key('2', true), Command::ToggleList)) return;
  // 9.1 row 1: Space -> Quick Look; Esc -> close (gallery scope: the overlay).
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeySpace), Command::QuickLookToggle)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyEscape), Command::QuickLookClose)) return;
  // Navigation keys the window already implements.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyReturn), Command::ActivateSelection)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyBack), Command::GoUp)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyHome), Command::SelectFirst)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyEnd), Command::SelectLast)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyLeft), Command::SelectLeft)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyRight), Command::SelectRight)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyUp), Command::SelectUp)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyDown), Command::SelectDown)) return;
  // The VK values this header names must be the documented Win32 ones: the
  // window passes wParam through, so a typo here would silently remap a key.
  if (!PRISM_CHECK_EQ(prism::keys::kKeyBack, 0x08u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyReturn, 0x0Du)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyEscape, 0x1Bu)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeySpace, 0x20u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyEnd, 0x23u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyHome, 0x24u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyLeft, 0x25u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyUp, 0x26u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyRight, 0x27u)) return;
  if (!PRISM_CHECK_EQ(prism::keys::kKeyDown, 0x28u)) return;
}

PRISM_TEST("keys: modifiers are strict, so near-miss chords stay inert") {
  // AltGr arrives as Ctrl+Alt on Windows: it must not toggle the view.
  if (!PRISM_CHECK_EQ(Key('1', true, false, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('2', true, false, true), Command::None)) return;
  // Ctrl+Shift+1 is not Ctrl+1: silent fall-through here would be a surprise.
  if (!PRISM_CHECK_EQ(Key('1', true, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('K', true, true), Command::None)) return;   // Ctrl+Shift+K
  if (!PRISM_CHECK_EQ(Key('E', true, false, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('N', true, false, true), Command::None)) return;
  // A bare digit/letter is not a command (it may be a type-ahead elsewhere).
  if (!PRISM_CHECK_EQ(Key('1'), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('2'), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key('K'), Command::None)) return;
  // Alt+Left is history; Ctrl+Left / Shift+Left are not.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyLeft, true), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyLeft, false, true), Command::None)) return;
  // Shift+Space is not Quick Look (some IMEs use it to switch input mode).
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeySpace, false, true), Command::None)) return;
}

PRISM_TEST("keys: Escape closes exactly the surface on top") {
  // Gallery scope: the Quick Look overlay (the window owns the close guard).
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyEscape, false, false, false, Scope::Gallery),
                      Command::QuickLookClose)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyEscape, false, false, false, Scope::QuickLook),
                      Command::QuickLookClose)) return;
  // Palette scope: the palette, not the preview underneath it.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyEscape, false, false, false, Scope::Palette),
                      Command::ClosePalette)) return;
  // Ctrl+K toggles: the same chord opens and closes the palette.
  if (!PRISM_CHECK_EQ(Key('K', true, false, false, Scope::Palette), Command::ClosePalette)) return;
}

PRISM_TEST("keys: the palette's text field owns printable input") {
  // Typing "e" must never smart-extract, and Space must never open Quick Look.
  if (!PRISM_CHECK_EQ(Key('e', false, false, false, Scope::Palette), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeySpace, false, false, false, Scope::Palette),
                      Command::None)) return;
  // Global chords are suspended while the user is typing.
  if (!PRISM_CHECK_EQ(Key('E', true, false, false, Scope::Palette), Command::None)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyLeft, false, false, true, Scope::Palette),
                      Command::None)) return;
  // Only navigation / confirm survive.
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyDown, false, false, false, Scope::Palette),
                      Command::SelectDown)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyUp, false, false, false, Scope::Palette),
                      Command::SelectUp)) return;
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyReturn, false, false, false, Scope::Palette),
                      Command::ActivateSelection)) return;
  // Quick Look scope still navigates: closing the preview is not required to
  // arrow to the next file (the window closes the card as it moves).
  if (!PRISM_CHECK_EQ(Key(prism::keys::kKeyRight, false, false, false, Scope::QuickLook),
                      Command::SelectRight)) return;
}

PRISM_TEST("keys: Alt+double-click is smart extract, plain double-click activates") {
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{2, true}),
                      PointerAction::SmartExtract)) return;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{2, false}),
                      PointerAction::Activate)) return;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{1, false}),
                      PointerAction::Select)) return;
  // Alt+click is a shell gesture, not a selection or a disk write.
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{1, true}),
                      PointerAction::None)) return;
  // Ctrl/Shift + double-click are other gestures: never start an extraction.
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{2, true, true}),
                      PointerAction::Activate)) return;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(PointerStroke{2, true, false, true}),
                      PointerAction::Activate)) return;
  PointerStroke right;
  right.clickCount = 2;
  right.alt = true;
  right.secondaryButton = true;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(right), PointerAction::None)) return;
}

PRISM_TEST("zoom: Ctrl+Shift+P cycles the 64..512 ladder and wraps") {
  using namespace prism::keys::zoom;
  if (!PRISM_CHECK_EQ(kMinPx, 64)) return;
  if (!PRISM_CHECK_EQ(kMaxPx, 512)) return;
  if (!PRISM_CHECK_EQ(kPresetCount, size_t{7})) return;
  // Walking up from the smallest stop reaches every stop exactly once.
  int size = kMinPx;
  const int expected[] = {96, 128, 192, 256, 384, 512, 64};
  for (const int next : expected) {
    size = NextPreset(size);
    if (!PRISM_CHECK_EQ(size, next)) return;
  }
  // A size produced by pinch lands on the next stop ABOVE it.
  if (!PRISM_CHECK_EQ(NextPreset(200), 256)) return;
  if (!PRISM_CHECK_EQ(NextPreset(511), 512)) return;
  if (!PRISM_CHECK_EQ(NextPreset(0), 64)) return;
  if (!PRISM_CHECK_EQ(PrevPreset(200), 192)) return;
  if (!PRISM_CHECK_EQ(PrevPreset(512), 384)) return;
  if (!PRISM_CHECK_EQ(PrevPreset(64), 512)) return;  // wraps at the bottom
  // The hard window is clamped in both directions (blueprint: 64 px .. 512 px).
  if (!PRISM_CHECK_EQ(ClampPx(10), 64)) return;
  if (!PRISM_CHECK_EQ(ClampPx(999), 512)) return;
  if (!PRISM_CHECK_EQ(ClampPx(128), 128)) return;
  if (!PRISM_CHECK_EQ(ClampPx(-40), 64)) return;
}

PRISM_TEST("zoom: pinch is continuous but obeys the same hard window") {
  using namespace prism::keys::zoom;
  if (!PRISM_CHECK_EQ(ApplyPinch(128, 2.0), 256)) return;
  if (!PRISM_CHECK_EQ(ApplyPinch(128, 0.5), 64)) return;
  // No snapping on pinch: the renderer interpolates between the presets.
  if (!PRISM_CHECK_EQ(ApplyPinch(300, 1.0), 300)) return;
  if (!PRISM_CHECK_EQ(ApplyPinch(100, 0.01), 64)) return;
  if (!PRISM_CHECK_EQ(ApplyPinch(500, 10.0), 512)) return;
  // round-half-up: 129 * 0.5 = 64.5 -> 65 (a truncating cast would give 64).
  if (!PRISM_CHECK_EQ(ApplyPinch(129, 0.5), 65)) return;
  // A degenerate factor is a no-op, not a division by zero or a jump to 0.
  if (!PRISM_CHECK_EQ(ApplyPinch(128, 0.0), 128)) return;
  if (!PRISM_CHECK_EQ(ApplyPinch(128, -3.0), 128)) return;
}

PRISM_TEST("mkdir: Ctrl+Shift+N consumes the VirtualTree copy-on-write plan") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  if (!PRISM_CHECK(!tree.dirty())) return;
  if (!PRISM_CHECK(tree.MakeUpdatePlan().empty())) return;  // nothing to commit yet

  const MkdirResult created = prism::tree::PlanNewFolder(tree, "pack", "Inbox");
  if (!PRISM_CHECK_EQ(created.status, MkdirStatus::Ok)) return;
  if (!PRISM_CHECK(created.created)) return;
  if (!PRISM_CHECK_EQ(created.folder, std::string("pack"))) return;
  if (!PRISM_CHECK_EQ(created.name, std::string("Inbox"))) return;
  if (!PRISM_CHECK_EQ(created.path, std::string("pack/Inbox"))) return;
  if (!PRISM_CHECK(tree.dirty())) return;
  // The node exists in the tree and is a directory, not a zero-length file.
  const VirtualArchiveTree::Node* node = tree.Find("pack/Inbox");
  if (!PRISM_CHECK(node != nullptr)) return;
  if (!PRISM_CHECK(node->isDir)) return;
  if (!PRISM_CHECK_EQ(node->index, static_cast<int64_t>(-1))) return;  // brand new
  // ... and the transaction the executor will run contains EXACTLY that node:
  // newData = 0 / newProps = 1 (VirtualTree's honest cost model).
  const std::vector<prism::UpdateItem> plan = tree.MakeUpdatePlan();
  if (!PRISM_CHECK_EQ(plan.size(), size_t{1})) return;
  if (!PRISM_CHECK(PlanHas(plan, "pack/Inbox"))) return;
  if (!PRISM_CHECK_EQ(plan[0].index, static_cast<int64_t>(-1))) return;
  if (!PRISM_CHECK(!plan[0].newData)) return;
  if (!PRISM_CHECK(plan[0].newProps)) return;
  if (!PRISM_CHECK(!plan[0].removed)) return;

  // A second folder appends one more node, it does not rewrite the first.
  const MkdirResult second = prism::tree::PlanNewFolder(tree, "pack", "Outbox");
  if (!PRISM_CHECK_EQ(second.status, MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(tree.MakeUpdatePlan().size(), size_t{2})) return;
}

PRISM_TEST("mkdir: duplicates are refused, including case-only differences") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "Inbox").status,
                      MkdirStatus::Ok)) return;
  // Same name twice.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "Inbox").status,
                      MkdirStatus::AlreadyExists)) return;
  // Archive paths are case-sensitive; a Windows destination is NOT. Creating
  // "inbox" next to "Inbox" would extract onto the same file.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "inbox").status,
                      MkdirStatus::AlreadyExists)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "INBOX").status,
                      MkdirStatus::AlreadyExists)) return;
  // A FILE with that name is a collision too.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "img1.png").status,
                      MkdirStatus::AlreadyExists)) return;
  // A sibling in a different folder is not a collision.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "docs", "Inbox").status,
                      MkdirStatus::Ok)) return;
  // Nothing was silently added by the refused attempts.
  if (!PRISM_CHECK_EQ(tree.MakeUpdatePlan().size(), size_t{2})) return;
}

PRISM_TEST("mkdir: names that Windows would reinterpret are refused, not mangled") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  const char* invalid[] = {"CON", "con.txt", "aux", "NUL", "COM1", "a:b", "a<b",
                           "a>b", "a|b", "a?b", "a*b", "a\"b", "a.", "a .", "..",
                           ".", "a/b", "a\\b"};
  for (const char* name : invalid) {
    if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", name).status,
                        MkdirStatus::InvalidName)) return;
  }
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "").status,
                      MkdirStatus::EmptyName)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "   ").status,
                      MkdirStatus::EmptyName)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "\t  ").status,
                      MkdirStatus::EmptyName)) return;
  // Nothing was created by any of the refusals.
  if (!PRISM_CHECK(!tree.dirty())) return;
  if (!PRISM_CHECK(tree.MakeUpdatePlan().empty())) return;
  // Internal spaces and dots are perfectly legal file names.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "pack", "my folder 2.0").status,
                      MkdirStatus::Ok)) return;
  // Edge spaces are a typing artefact and are trimmed, not rejected: the
  // accepted name is the trimmed one (asserted, not assumed).
  const MkdirResult trimmed = prism::tree::PlanNewFolder(tree, "pack", "  spaced  ");
  if (!PRISM_CHECK_EQ(trimmed.status, MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(trimmed.name, std::string("spaced"))) return;
  if (!PRISM_CHECK_EQ(trimmed.path, std::string("pack/spaced"))) return;
}

PRISM_TEST("mkdir: the parent must exist and be a folder, not a file") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  // A stale palette row / replayed key can point at a folder that is gone: the
  // decision refuses instead of letting Mkdir() invent a chain of parents.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "nope", "x").status,
                      MkdirStatus::NoSuchFolder)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "root.png", "x").status,
                      MkdirStatus::NoSuchFolder)) return;
  if (!PRISM_CHECK(!tree.dirty())) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "implied", "New").status,
                      MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "implied/New", "Nested").status,
                      MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(tree.MakeUpdatePlan().size(), size_t{2})) return;
  // The archive root is a folder too.
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "", "TopLevel").status,
                      MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "/", "TopLevel").status,
                      MkdirStatus::AlreadyExists)) return;
}

PRISM_TEST("mkdir: an archive-implied folder still accepts a child") {
  // "ghost" has no directory node in the catalog: only "ghost/child.txt"
  // exists, so VirtualTree synthesises the node (isDir == false, index == -1).
  // Navigation treats it as a folder, and so must Ctrl+Shift+N.
  std::vector<ArchiveItem> catalog;
  Add(&catalog, "ghost/child.txt", false, 5);
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  const VirtualArchiveTree::Node* ghost = tree.Find("ghost");
  if (!PRISM_CHECK(ghost != nullptr)) return;
  if (!PRISM_CHECK(!ghost->isDir)) return;  // implied, not an explicit entry
  // VirtualTree already plans implied nodes as "new" (they carry index -1):
  // that plan is the tree's business, and pinning it here means a future
  // change to that rule has to update this test deliberately.
  if (!PRISM_CHECK_EQ(tree.MakeUpdatePlan().size(), size_t{1})) return;
  if (!PRISM_CHECK_EQ(prism::tree::PlanNewFolder(tree, "ghost", "New").status,
                      MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(tree.MakeUpdatePlan().size(), size_t{2})) return;
}

PRISM_TEST("mkdir: the folder argument is normalized before it is used") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  VirtualArchiveTree tree = VirtualArchiveTree::FromCatalog(catalog);
  const MkdirResult result = prism::tree::PlanNewFolder(tree, "/pack//sub/", "Deep2");
  if (!PRISM_CHECK_EQ(result.status, MkdirStatus::Ok)) return;
  if (!PRISM_CHECK_EQ(result.folder, std::string("pack/sub"))) return;
  if (!PRISM_CHECK_EQ(result.path, std::string("pack/sub/Deep2"))) return;
  if (!PRISM_CHECK(PlanHas(tree.MakeUpdatePlan(), "pack/sub/Deep2"))) return;
}

PRISM_TEST("mkdir: the catalog helper agrees with the tree's collision rule") {
  std::vector<ArchiveItem> catalog = BuildCatalog();
  if (!PRISM_CHECK(prism::tree::FolderNameTaken(catalog, "pack", "img1.png"))) return;
  if (!PRISM_CHECK(prism::tree::FolderNameTaken(catalog, "pack", "IMG1.PNG"))) return;
  if (!PRISM_CHECK(prism::tree::FolderNameTaken(catalog, "pack", "sub"))) return;
  if (!PRISM_CHECK(prism::tree::FolderNameTaken(catalog, "", "docs"))) return;
  if (!PRISM_CHECK(prism::tree::FolderNameTaken(catalog, "pack/sub", "deep.png"))) return;
  if (!PRISM_CHECK(!prism::tree::FolderNameTaken(catalog, "pack", "note.txt"))) return;
  if (!PRISM_CHECK(!prism::tree::FolderNameTaken(catalog, "docs", "img1.png"))) return;
}

PRISM_TEST_MAIN()
