// Windows integration test: FOLDER NAVIGATION on the REAL window class.
//
// What this proves (L2: the object under test is not a mock):
//   * the real GalleryWindow owns a real HWND and routes real
//     WM_LBUTTONDBLCLK / WM_KEYDOWN messages through its real switch into the
//     PrismCore navigator and back out into a changed row list;
//   * double-click on a FOLDER drills in; on a FILE the batch-A smart-extract
//     handler fires and NOTHING navigates;
//   * Backspace goes up one level with the child row re-selected, and at the
//     archive root it is a no-op (no empty window, no crash, viewport intact);
//   * a breadcrumb click jumps straight to that level;
//   * the pre-existing keys (arrows / Home / End / Enter / Space / Esc /
//     Ctrl+1,2) still behave;
//   * thumbnails are keyed by CATALOG id, so a folder switch needs no rescan:
//     the same id is still a store hit after navigating (and the id is the one
//     the whole-archive sweep stored).
//
// The stdout marker is part of the verdict (CONTRACTS D1/D4): a silent SKIP
// cannot pass -- this file returns non-zero on skip AND Tests/CMakeLists.txt
// registers PASS_REGULAR_EXPRESSION on the marker.
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ThumbStore.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using prism::ArchiveItem;
using prism::win::gallery::GalleryWindow;

namespace {

// A small but STRUCTURED archive: two levels of folders, a sibling whose name
// shares a prefix with another ("pack" vs "pack2" -- the naive starts_with
// trap), files at both levels, and images so thumbnails can be planted under
// catalog ids.
std::vector<ArchiveItem> BuildCatalog() {
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = dir;
    item.size = size;
    item.block = 0;
    items.push_back(item);
  };
  add("docs", true, 0);              // 0
  add("pack", true, 0);              // 1
  add("pack2", true, 0);             // 2
  add("root.png", false, 500);       // 3
  add("docs/manual.pdf", false, 400);  // 4
  add("pack/img1.png", false, 120);  // 5
  add("pack/note.txt", false, 30);   // 6
  add("pack/sub", true, 0);          // 7
  add("pack/sub/deep.png", false, 60);  // 8
  add("pack2/decoy.txt", false, 10);    // 9
  return items;
}

}  // namespace

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: COM init failed");
    return 1;  // D1: a skip is a FAILURE, never a pass
  }

  std::vector<ArchiveItem> items = BuildCatalog();
  GalleryWindow window;
  if (!window.Create(L"PrismArchive navigation test", GetModuleHandleW(nullptr))) {
    std::puts("SKIP: window creation failed (no interactive window station)");
    return 1;
  }
  if (!PRISM_CHECK(window.Hwnd() != nullptr)) return 1;

  window.SetItems(&items);
  window.SetRootLabel(L"nav.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  // ---------------------------------------------------------------- helpers
  auto rowOf = [&window](const char* path) {
    const auto& rows = window.Items();
    for (size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].path == path) return static_cast<int>(i);
    }
    return -1;
  };
  auto rowPathAt = [&window](int row) {
    const auto& rows = window.Items();
    return (row >= 0 && row < static_cast<int>(rows.size())) ? rows[row].path
                                                             : std::string("<none>");
  };
  // Row -> the pixel its tile centre is painted at (same geometry the renderer
  // uses: columns and cell height from the viewport, scroll added back).
  auto rowCentre = [&window](int row, int* px, int* py) {
    const int cols = window.Viewport().Columns();
    if (cols <= 0) return false;
    const double cellW = window.Viewport().ColumnWidth();
    const double cellH = window.Viewport().CellHeight();
    if (cellW <= 0.0 || cellH <= 0.0) return false;
    *px = static_cast<int>((row % cols) * cellW + cellW / 2.0);
    *py = static_cast<int>((row / cols) * cellH + cellH / 2.0);
    return true;
  };
  auto doubleClickRow = [&](int row) {
    int x = 0, y = 0;
    if (!rowCentre(row, &x, &y)) return false;
    window.Dispatch(WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
    return true;
  };
  auto key = [&window](int vk) { window.Dispatch(WM_KEYDOWN, static_cast<WPARAM>(vk), 0); };
  auto clickBar = [&](int x) {
    window.Dispatch(WM_LBUTTONDOWN, 0,
                    MAKELPARAM(x, (window.NavBarTop() + window.NavBarBottom()) / 2));
  };

  int activations = 0;
  int lastActivated = -1;
  window.SetActivateHandler([&](int catalogIndex) {
    ++activations;
    lastActivated = catalogIndex;
  });

  // ------------------------------------------------------------ 1. root view
  // docs, pack, pack2 (folders first, 7-Zip ordering), then root.png.
  if (!PRISM_CHECK_EQ(window.ItemCount(), 4)) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(0), std::string("docs"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(1), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(2), std::string("pack2"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(3), std::string("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.CrumbLevelCount(), 0)) return 1;

  // ---------------------------------------- 2. double-click a FOLDER drills in
  if (!doubleClickRow(rowOf("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;  // sub, img1, note
  // Folders first -- the SAME rule as the root view above, which this case
  // originally contradicted (it expected img1/note/sub). The window lane
  // caught it: a suite cannot hold two different orderings and call both
  // correct. The ordering itself is intentional (ArchiveTreeView.hpp: folders
  // first, then 7-Zip natural name order), and pinning it in BOTH views is
  // what stops it drifting.
  if (!PRISM_CHECK_EQ(rowPathAt(0), std::string("pack/sub"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(1), std::string("pack/img1.png"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(2), std::string("pack/note.txt"))) return 1;
  // "pack2/decoy.txt" is NOT here: a folder owns only its separator-delimited
  // subtree (the naive prefix bug would show it).
  if (!PRISM_CHECK_EQ(rowOf("pack2/decoy.txt"), -1)) return 1;
  if (!PRISM_CHECK_EQ(window.CrumbLevelCount(), 1)) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), -1)) return 1;  // entering clears
  if (!PRISM_CHECK_EQ(window.ScrollOffset(), 0.0)) return 1;
  if (!PRISM_CHECK_EQ(activations, 0)) return 1;  // a folder never extracts

  // ------------------------------------------------- 3. Enter drills deeper
  // Real arrow path, not a direct SetSelection. "pack/sub" is row 0 (folders
  // first), so Home lands ON the target -- the previous version pressed RIGHT
  // after Home and expected to still be on row 0, which only made sense under
  // the old files-first ordering. Derive the target from rowOf() so a future
  // ordering change cannot make this silently wrong again.
  const int subRow = rowOf("pack/sub");
  if (!PRISM_CHECK_EQ(subRow, 0)) return 1;  // folders first, stated once
  key(VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), subRow)) return 1;
  // Prove the arrows really move. NOT VK_DOWN: pack/ holds 3 items and the
  // grid is 3 columns wide, so it is a SINGLE row and a downward move has
  // nowhere to go -- a no-op is the correct behaviour, and asserting movement
  // there was my error, not a bug. VK_RIGHT is the grid's in-row step
  // (GalleryWindow: VK_RIGHT -> +1 when not in list mode), so it moves here.
  key(VK_RIGHT);
  if (!PRISM_CHECK_EQ(window.Selection(), subRow + 1)) return 1;
  key(VK_LEFT);
  if (!PRISM_CHECK_EQ(window.Selection(), subRow)) return 1;
  key(VK_RETURN);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack/sub"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 1)) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(0), std::string("pack/sub/deep.png"))) return 1;
  if (!PRISM_CHECK_EQ(window.CrumbLevelCount(), 2)) return 1;

  // ------------------------------------ 4. Backspace restores the child row
  key(VK_BACK);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), rowOf("pack/sub"))) return 1;
  if (!PRISM_CHECK_EQ(window.ScrollOffset(), 0.0)) return 1;

  // --------------------------------- 5. double-click a FILE keeps extract
  if (!doubleClickRow(rowOf("pack/note.txt"))) return 1;
  if (!PRISM_CHECK_EQ(activations, 1)) return 1;
  if (!PRISM_CHECK_EQ(lastActivated, 6)) return 1;  // the CATALOG index, not the row
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;  // no navigation
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;

  // ---------------------------------------- 6. breadcrumb click jumps levels
  // Crumbs now: [nav.7z] > [pack]; level 0 == the archive root.
  if (!PRISM_CHECK_EQ(window.CrumbLevelCount(), 1)) return 1;
  // Diagnostic first: print the geometry the test is about to aim at, so a red
  // run reports the numbers instead of only the consequence. (Six rounds of
  // "breadcrumb did not navigate" told me nothing about WHERE the pixels were.)
  std::printf("    [nav] textLeft=%d textRight=%d barTop=%d barBottom=%d "
              "client=%dx%d\n",
              window.NavBarTextLeft(), window.NavBarTextRight(),
              window.NavBarTop(), window.NavBarBottom(),
              window.ClientWidthForTest(), window.ClientHeightForTest());
  {
    const int px = window.NavBarTextLeft() + 4;
    const int py = (window.NavBarTop() + window.NavBarBottom()) / 2;
    std::printf("    [nav] hitTest(%d,%d) -> level %d  (kNoLevel=%d)\n", px, py,
                window.CrumbLevelAt(px, py), prism::tree::kNoLevel);
    std::printf("    [nav] crumb text width for 'nav.7z' = %d px\n",
                window.MeasureForTest("nav.7z"));
    std::fflush(stdout);
  }
  std::fflush(stdout);
  // Aim at the ROOT CRUMB'S TEXT, not at the island's left inset. The run is
  // centred inside the island (LayoutCenteredBreadcrumb -- the renderer paints
  // centre-aligned), so textLeft_ is up to hundreds of pixels away from where
  // the crumb actually begins. Clicking the inset lands on empty pill.
  // Level -2 is kRootLevel, NOT 0: levels index the crumb VECTOR, so 0 is the
  // first child folder ("pack") -- clicking that correctly stays put. Picking
  // the root by its real constant instead of by "0 looks like the first one".
  int crumbL = 0, crumbR = 0;
  if (!PRISM_CHECK(window.CrumbSpanForTest(prism::tree::kRootLevel, &crumbL, &crumbR))) return 1;
  if (!PRISM_CHECK(crumbR > crumbL)) return 1;
  std::printf("    [nav] root crumb spans [%d,%d) -- clicking its centre\n",
              crumbL, crumbR);
  std::fflush(stdout);
  clickBar((crumbL + crumbR) / 2);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 4)) return 1;
  {
    std::printf("    [nav] root rows after crumb click:");
    for (int i = 0; i < window.ItemCount(); ++i)
      std::printf(" [%d]%s", i, rowPathAt(i).c_str());
    std::printf("  selection=%d\n", window.Selection());
    std::fflush(stdout);
  }
  if (!PRISM_CHECK_EQ(window.Selection(), rowOf("pack"))) return 1;  // came out of pack
  // Clicking OUTSIDE the crumb run (right end of the island) is a dead pixel:
  // it must not navigate, and must not fall through to the grid behind it.
  const int beforeFolderLen = static_cast<int>(window.Folder().size());
  {
    // A DEAD pixel: just right of the crumb run but still inside the pill.
    // (Clicking the last crumb here would be a second root click -- it would
    // navigate, which is the opposite of what this case is about.)
    int l = 0, r = 0;
    if (!PRISM_CHECK(window.CrumbSpanForTest(prism::tree::kRootLevel, &l, &r))) return 1;
    // Click INSIDE the pill but LEFT of the centred run -- guaranteed dead.
    // (r + 2 landed in the next crumb's span, which navigated to "pack": the
    // rows then collapsed to pack's 3 children, so the later rowOf("pack")
    // resolved against a different list and returned 2. Proven by the dump.)
    const int selBefore = window.Selection();
    clickBar(window.NavBarTextLeft() + 2);
    // Miss the crumbs by a few pixels and BOTH must hold: no navigation, and
    // the selection survives. The old code routed the miss into
    // SetSelection(ItemAtPoint(...)) = -1, which silently deselected.
    if (!PRISM_CHECK_EQ(window.Selection(), selBefore)) return 1;
  }
  if (!PRISM_CHECK_EQ(static_cast<int>(window.Folder().size()), beforeFolderLen)) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), rowOf("pack"))) return 1;

  // --------------------------- 7. Backspace at the root is a no-op, not a wipe
  const int rootCount = window.ItemCount();
  const int rootSelection = window.Selection();
  key(VK_BACK);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), rootCount)) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), rootSelection)) return 1;
  key(VK_BACK);  // twice: still nothing
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;

  // ------------------------------------------- 8. pre-existing keys regress
  key(VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), 0)) return 1;
  key(VK_END);
  if (!PRISM_CHECK_EQ(window.Selection(), rootCount - 1)) return 1;
  key(VK_LEFT);   // grid mode: column step of one
  if (!PRISM_CHECK_EQ(window.Selection(), rootCount - 2)) return 1;
  key(VK_END);
  key(VK_RIGHT);  // past the last item: clamped, selection unchanged
  if (!PRISM_CHECK_EQ(window.Selection(), rootCount - 1)) return 1;
  key(VK_UP);     // a full row up from the last row lands out of range: no move
  if (!PRISM_CHECK_EQ(window.Selection(), rootCount - 1)) return 1;
  // Space opens Quick Look on a previewable row, Esc closes it. (Double-click
  // on the FILE first: it activates without navigating, which also re-proves
  // that a file activation leaves the folder alone.)
  const int beforeFileActivations = activations;
  if (!doubleClickRow(rowOf("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(activations, beforeFileActivations + 1)) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  key(VK_SPACE);
  if (!PRISM_CHECK(window.QuickLookOpen())) return 1;
  key(VK_ESCAPE);
  if (!PRISM_CHECK(!window.QuickLookOpen())) return 1;
  // Ctrl+1/2 only fire with the modifier held; without it they must not switch.
  window.Dispatch(WM_KEYDOWN, '2', 0);
  if (!PRISM_CHECK(!window.ListMode())) return 1;  // GetKeyState has no Ctrl here
  window.SetListMode(true);
  if (!PRISM_CHECK(window.ListMode())) return 1;
  window.SetListMode(false);
  if (!PRISM_CHECK(!window.ListMode())) return 1;

  // ------------------------------- 9. thumbnails survive a folder switch
  // The whole-archive sweep stores by CATALOG id (Main.cpp does exactly this).
  auto store = std::make_shared<prism::gallery::ThumbStore>(4u << 20);
  prism::gallery::DecodedThumb thumb;
  thumb.width = 2;
  thumb.height = 2;
  thumb.pixels.assign(16, 0x80);
  if (!PRISM_CHECK(store->OnThumb(5, thumb))) return 1;  // "pack/img1.png"
  const uint64_t versionBefore = store->Version();
  window.SetThumbs(store);
  if (!doubleClickRow(rowOf("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;
  // Navigating never touches the pixel store: no re-decode, no eviction.
  if (!PRISM_CHECK_EQ(store->Version(), versionBefore)) return 1;
  if (!PRISM_CHECK(store->Contains(5))) return 1;
  const auto& rows = window.Items();
  bool idPreserved = false;
  for (const auto& row : rows) {
    if (row.path == "pack/img1.png") idPreserved = row.index == 5;
  }
  if (!PRISM_CHECK(idPreserved)) return 1;  // the id the store knows

  window.Dispatch(WM_DESTROY, 0, 0);
  CoUninitialize();
  std::puts("ALL NAVIGATION CHECKS PASSED");
  return prism::test::FailureCount() == 0 ? 0 : 1;
}
