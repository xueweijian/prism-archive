// PrismArchive -- Windows integration test
// UI JOURNEYS (blueprint ch. 9.1): a real HWND, a real WndProc, real posted
// messages -- and the PrismCore decisions the missing chords still need.
//
// Why posted messages instead of Dispatch(): Dispatch() is a seam that calls
// the same switch, but a user's key arrives through the queue. Posting proves
// the window class, the message pump and the handler cooperate -- and that no
// handler depends on being called synchronously.
//
// The journeys that are not wired into the window yet (Ctrl+K, Ctrl+Shift+P,
// Alt+Left/Right, Ctrl+Shift+N, Alt+double-click) are driven through the
// PrismCore decision + the PrismWin shells, i.e. exactly the layer the window
// patch will call. Tests/CMakeLists pins the stdout marker, so a silent SKIP
// (no window station / no 7z.dll) can never count as a pass (CONTRACTS D1).
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ArchiveTreeView.hpp"
#include "../../Src/PrismCore/HistoryStack.hpp"
#include "../../Src/PrismCore/NewFolder.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismCore/VirtualTree.hpp"
#include "../../Src/PrismWin/CommandPalette.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/QuickLookOverlay.hpp"

using prism::ArchiveItem;
using prism::win::gallery::GalleryWindow;

namespace {

std::vector<ArchiveItem> BuildCatalog() {
  std::vector<ArchiveItem> items;
  auto add = [&items](const char* path, bool dir, uint64_t size) {
    ArchiveItem item;
    item.index = static_cast<uint32_t>(items.size());
    item.path = path;
    item.isDir = dir;
    item.size = size;
    items.push_back(item);
  };
  add("docs", true, 0);                 // 0
  add("pack", true, 0);                 // 1
  add("pack/img1.png", false, 120);     // 2
  add("pack/note.txt", false, 30);      // 3
  add("pack/sub", true, 0);             // 4
  add("pack/sub/deep.png", false, 60);  // 5
  add("root.png", false, 500);          // 6
  return items;
}

// Row of a path inside a Navigator's current folder (the history journey
// drives the navigator directly, so it cannot borrow the window's lambda).
int RowIndexIn(const prism::tree::Navigator& nav, const char* path) {
  const auto& rows = nav.FolderItems();
  for (size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

// Run the real message loop until the queue is empty. PeekMessage (not
// GetMessage) on purpose: it never blocks and never synthesises WM_PAINT, so a
// headless run cannot stall on D2D presentation.
void Pump() {
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
}

void PostKey(HWND hwnd, UINT vk) {
  PostMessageW(hwnd, WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
  Pump();
}

// Ctrl has to be held in the THREAD's keyboard state for the window's
// GetKeyState(VK_CONTROL) to see it; SetInputState is the only way to do that
// without a physical keyboard, and the first journey asserts it works instead
// of assuming it (a failure here is loud, never a skip).
bool SetCtrlDown(bool down) {
  BYTE state[256] = {};
  if (!GetKeyboardState(state)) return false;
  state[VK_CONTROL] = down ? 0x80 : 0x00;
  state[VK_SHIFT] = 0x00;
  state[VK_MENU] = 0x00;
  return SetKeyboardState(state) != FALSE;
}

}  // namespace

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: COM init failed");
    return 1;  // D1: a skip is a failure
  }

  std::vector<ArchiveItem> items = BuildCatalog();
  GalleryWindow window;
  if (!window.Create(L"PrismArchive UI journey", GetModuleHandleW(nullptr))) {
    std::puts("SKIP: window creation failed (no interactive window station)");
    return 1;
  }
  if (!PRISM_CHECK(window.Hwnd() != nullptr)) return 1;
  window.SetItems(&items);
  window.SetRootLabel(L"journey.7z");

  int activations = 0;
  int lastActivated = -1;
  window.SetActivateHandler([&](int catalogIndex) {
    ++activations;
    lastActivated = catalogIndex;
  });

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
  // V4: the grid is a FLOW, so the click point is computed HERE, from the
  // definition (DESIGN.md §2/§3): a directory is a tile of tileWidth x
  // cellHeight inside the padded content; anything without a thumbnail is a
  // 32 px band spanning the content. The window's rectangles are the INPUTS
  // (content inner, tile width, columns, gaps) -- never the answer -- so a
  // layout bug cannot make the expectation and the hit test agree by accident.
  auto tileCentre = [&window](int row, int* px, int* py) {
    const prism::layout::Layout layout = window.LayoutForTest();
    const auto& items = window.Items();
    if (row < 0 || row >= static_cast<int>(items.size())) return false;
    int bandTop = layout.contentInner.top;
    int column = 0;
    for (int i = 0; i <= row; ++i) {
      if (items[static_cast<size_t>(i)].isDir) {  // a tile
        if (i == row) {
          *px = layout.contentInner.left + column * (layout.tileWidth + layout.gap) +
                layout.tileWidth / 2;
          *py = bandTop + layout.cellHeight / 2;
          return true;
        }
        if (++column == layout.columns) {
          bandTop += layout.cellHeight;
          column = 0;
        }
      } else {  // a 32 px band, the pitch DESIGN.md §2 gives the list
        // A tile run always occupies a full cell row, even when it ends early:
        // the band that follows starts UNDER it (definition, not implementation).
        if (column > 0) {
          bandTop += layout.cellHeight;
          column = 0;
        }
        if (i == row) {
          *px = (layout.content.left + layout.content.right) / 2;
          *py = bandTop + 16;
          return true;
        }
        bandTop += 32;
      }
    }
    return false;
  };
  auto postDoubleClick = [&window, &tileCentre](int row) {
    int x = 0;
    int y = 0;
    if (!tileCentre(row, &x, &y)) return false;
    // The point comes from the definition; the window must resolve it to the
    // SAME item (independent expectation vs. the product's hit test).
    if (!PRISM_CHECK_EQ(window.HitTestForTest(x, y), row)) return false;
    PostMessageW(window.Hwnd(), WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
    Pump();
    return true;
  };

  // ------------------------------------------------ journey 1: open the archive
  // The first message a real window ever gets is a size; post it, do not call.
  PostMessageW(window.Hwnd(), WM_SIZE, 0, MAKELPARAM(1280, 800));
  Pump();
  if (!PRISM_CHECK_EQ(window.ClientWidthForTest(), 1280)) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(0), std::string("docs"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(1), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(2), std::string("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;

  // -------------------------------------- journey 2: Ctrl+2 / Ctrl+1 view modes
  // Ctrl is a modifier the window reads from the thread keyboard state; prove
  // the test can actually hold it before asserting on what it toggles.
  if (!PRISM_CHECK(SetCtrlDown(true))) return 1;
  if (!PRISM_CHECK((GetKeyState(VK_CONTROL) & 0x8000) != 0)) {
    std::puts("FAIL: SetKeyboardState cannot hold Ctrl on this station");
    return 1;
  }
  PostKey(window.Hwnd(), '2');
  if (!PRISM_CHECK(window.ListMode())) return 1;
  PostKey(window.Hwnd(), '1');
  if (!PRISM_CHECK(!window.ListMode())) return 1;
  if (!PRISM_CHECK(SetCtrlDown(false))) return 1;
  // And a bare '2' must not switch (the chord needs the modifier).
  PostKey(window.Hwnd(), '2');
  if (!PRISM_CHECK(!window.ListMode())) return 1;
  PostKey(window.Hwnd(), '1');
  if (!PRISM_CHECK(!window.ListMode())) return 1;
  // List mode rows start below the header: DOWN moves one row, LEFT/RIGHT are
  // inert there. This is the window's rule, not the keymap's.
  window.SetListMode(true);
  PostKey(window.Hwnd(), VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), 0)) return 1;
  PostKey(window.Hwnd(), VK_DOWN);
  if (!PRISM_CHECK_EQ(window.Selection(), 1)) return 1;
  PostKey(window.Hwnd(), VK_RIGHT);
  if (!PRISM_CHECK_EQ(window.Selection(), 1)) return 1;
  window.SetListMode(false);

  // ------------------------------------------ journey 3: arrows / Home / End
  PostKey(window.Hwnd(), VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), 0)) return 1;
  PostKey(window.Hwnd(), VK_RIGHT);
  if (!PRISM_CHECK_EQ(window.Selection(), 1)) return 1;
  PostKey(window.Hwnd(), VK_RIGHT);
  if (!PRISM_CHECK_EQ(window.Selection(), 2)) return 1;
  PostKey(window.Hwnd(), VK_RIGHT);  // past the last item: clamped, no wrap
  if (!PRISM_CHECK_EQ(window.Selection(), 2)) return 1;
  PostKey(window.Hwnd(), VK_LEFT);
  if (!PRISM_CHECK_EQ(window.Selection(), 1)) return 1;
  PostKey(window.Hwnd(), VK_END);
  if (!PRISM_CHECK_EQ(window.Selection(), 2)) return 1;
  PostKey(window.Hwnd(), VK_HOME);
  if (!PRISM_CHECK_EQ(window.Selection(), 0)) return 1;

  // -------------------------------------- journey 4: Enter opens a directory
  window.SetSelection(rowOf("pack"));
  PostKey(window.Hwnd(), VK_RETURN);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;
  if (!PRISM_CHECK_EQ(rowPathAt(0), std::string("pack/sub"))) return 1;
  if (!PRISM_CHECK_EQ(window.CrumbLevelCount(), 1)) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), -1)) return 1;  // entering clears

  // ------------------------------- journey 5: Backspace goes up, remembers child
  PostKey(window.Hwnd(), VK_BACK);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), rowOf("pack"))) return 1;  // came out of it
  PostKey(window.Hwnd(), VK_BACK);  // at the root: a no-op, not a wipe
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;
  if (!PRISM_CHECK_EQ(window.Selection(), rowOf("pack"))) return 1;

  // ------------------------------ journey 6: double-click a folder drills in
  if (!postDoubleClick(rowOf("docs"))) return 1;
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("docs"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 0)) return 1;  // empty folder: empty list
  PostKey(window.Hwnd(), VK_BACK);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;

  // ------------------ journey 7: double-click a FILE activates, never navigates
  const int activationsBefore = activations;
  if (!postDoubleClick(rowOf("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(activations, activationsBefore + 1)) return 1;
  if (!PRISM_CHECK_EQ(lastActivated, 6)) return 1;  // CATALOG index, not the row
  if (!PRISM_CHECK_EQ(window.Folder(), std::string(""))) return 1;

  // ------------------------------- journey 8: Space opens Quick Look, Esc closes
  window.SetSelection(rowOf("root.png"));
  PostKey(window.Hwnd(), VK_SPACE);
  if (!PRISM_CHECK(window.QuickLookOpen())) return 1;
  // A second Space closes it (toggle), exactly like a preview panel.
  PostKey(window.Hwnd(), VK_SPACE);
  if (!PRISM_CHECK(!window.QuickLookOpen())) return 1;
  PostKey(window.Hwnd(), VK_SPACE);
  if (!PRISM_CHECK(window.QuickLookOpen())) return 1;
  PostKey(window.Hwnd(), VK_ESCAPE);
  if (!PRISM_CHECK(!window.QuickLookOpen())) return 1;

  // ------------------------------------- journey 9: Ctrl+K command palette
  prism::win::palette::PaletteShell palette;
  palette.SetCommands(prism::palette::DefaultCommands());
  palette.SetCatalog(items);
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(prism::keys::Stroke{'K', true, false, false}),
                      prism::keys::Command::OpenPalette)) return 1;
  if (!PRISM_CHECK(palette.Open())) return 1;
  if (!PRISM_CHECK_EQ(palette.RowCount(),
                      static_cast<int>(prism::palette::DefaultCommands().size() +
                                       items.size()))) return 1;
  if (!PRISM_CHECK_EQ(palette.Highlight(), 0)) return 1;
  const auto commandRow = palette.Highlighted();
  if (!PRISM_CHECK(commandRow != nullptr)) return 1;
  if (!PRISM_CHECK_EQ(commandRow->kind, prism::palette::Kind::Command)) return 1;
  // Typing narrows the list; the ranker (PrismCore) decides the order.
  palette.OnChar('p');
  palette.OnChar('n');
  palette.OnChar('g');
  if (!PRISM_CHECK_EQ(palette.Query(), std::string("png"))) return 1;
  if (!PRISM_CHECK_EQ(palette.RowCount(), 3)) return 1;  // root/img1/deep
  if (!PRISM_CHECK_EQ(palette.Highlighted()->title, std::string("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(palette.Highlighted()->index, 6u)) return 1;
  // Down moves and wraps; Enter hands the highlighted row back to the window.
  auto down = palette.OnKey(prism::keys::Stroke{prism::keys::kKeyDown, false, false, false});
  if (!PRISM_CHECK(down.consumed)) return 1;
  if (!PRISM_CHECK_EQ(palette.Highlighted()->title, std::string("pack/img1.png"))) return 1;
  palette.OnKey(prism::keys::Stroke{prism::keys::kKeyDown, false, false, false});
  palette.OnKey(prism::keys::Stroke{prism::keys::kKeyDown, false, false, false});  // wraps
  if (!PRISM_CHECK_EQ(palette.Highlight(), 0)) return 1;
  // Backspace edits the query and re-ranks.
  palette.OnKey(prism::keys::Stroke{prism::keys::kKeyBack, false, false, false});
  if (!PRISM_CHECK_EQ(palette.Query(), std::string("pn"))) return 1;
  // Geometry and hit test come from one source.
  const int clientW = window.ClientWidthForTest();
  const int clientH = window.ClientHeightForTest();
  const auto frame = palette.Frame(clientW, clientH);
  if (!PRISM_CHECK_EQ(frame.left, (clientW - frame.Width()) / 2)) return 1;
  const auto rowRect = palette.RowRect(clientW, clientH, 1);
  if (!PRISM_CHECK_EQ(palette.RowAt(clientW, clientH, rowRect.left + 4,
                                    (rowRect.top + rowRect.bottom) / 2), 1)) return 1;
  if (!PRISM_CHECK_EQ(palette.RowAt(clientW, clientH, frame.left + 2, frame.top + 2), -1)) return 1;
  if (!PRISM_CHECK_EQ(palette.RowAt(clientW, clientH, -5, -5), -1)) return 1;
  // Esc closes the palette through the same keymap the window will use.
  palette.SetQuery("deep");
  const auto enter = palette.OnKey(prism::keys::Stroke{prism::keys::kKeyReturn, false, false, false});
  if (!PRISM_CHECK(enter.consumed)) return 1;
  if (!PRISM_CHECK_EQ(enter.command, prism::keys::Command::ActivateSelection)) return 1;
  if (!PRISM_CHECK_EQ(palette.Highlighted()->title, std::string("pack/sub/deep.png"))) return 1;
  const auto escape = palette.OnKey(prism::keys::Stroke{prism::keys::kKeyEscape, false, false, false});
  if (!PRISM_CHECK(escape.consumed)) return 1;
  if (!PRISM_CHECK_EQ(escape.command, prism::keys::Command::ClosePalette)) return 1;
  if (!PRISM_CHECK(!palette.IsOpen())) return 1;

  // ------------------------- journey 10: Ctrl+Shift+P walks the zoom ladder
  const auto zoomChord = prism::keys::Resolve(prism::keys::Stroke{'P', true, true, false});
  if (!PRISM_CHECK_EQ(zoomChord, prism::keys::Command::ZoomNext)) return 1;
  int tileSize = 128;
  const int ladder[] = {192, 256, 384, 512, 64};  // ...and wraps to the smallest
  for (const int expected : ladder) {
    tileSize = prism::keys::zoom::NextPreset(tileSize);
    if (!PRISM_CHECK_EQ(tileSize, expected)) return 1;
  }

  // ------------------------------ journey 11: Alt+Left/Right replays history
  prism::tree::Navigator nav(&items);
  prism::history::Stack history;
  history.Visit(nav.Folder());
  if (!PRISM_CHECK_EQ(nav.Enter(RowIndexIn(nav, "pack")).action,
                      prism::tree::NavAction::FolderChanged)) return 1;
  history.Visit(nav.Folder());
  if (!PRISM_CHECK_EQ(nav.Enter(RowIndexIn(nav, "pack/sub")).action,
                      prism::tree::NavAction::FolderChanged)) return 1;
  history.Visit(nav.Folder());
  if (!PRISM_CHECK_EQ(nav.Folder(), std::string("pack/sub"))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(prism::keys::Stroke{prism::keys::kKeyLeft, false, false, true}),
                      prism::keys::Command::HistoryBack)) return 1;
  std::string target;
  if (!PRISM_CHECK(history.Back(&target))) return 1;
  if (!PRISM_CHECK_EQ(nav.GoTo(target).action, prism::tree::NavAction::FolderChanged)) return 1;
  if (!PRISM_CHECK_EQ(nav.Folder(), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(prism::keys::Stroke{prism::keys::kKeyRight, false, false, true}),
                      prism::keys::Command::HistoryForward)) return 1;
  if (!PRISM_CHECK(history.Forward(&target))) return 1;
  if (!PRISM_CHECK_EQ(nav.GoTo(target).action, prism::tree::NavAction::FolderChanged)) return 1;
  if (!PRISM_CHECK_EQ(nav.Folder(), std::string("pack/sub"))) return 1;

  // -------------------- journey 12: Ctrl+Shift+N stages a CoW folder creation
  if (!PRISM_CHECK_EQ(prism::keys::Resolve(prism::keys::Stroke{'N', true, true, false}),
                      prism::keys::Command::NewFolder)) return 1;
  prism::VirtualArchiveTree tree = prism::VirtualArchiveTree::FromCatalog(items);
  const auto made = prism::tree::PlanNewFolder(tree, "pack", "Inbox");
  if (!PRISM_CHECK_EQ(made.status, prism::tree::MkdirStatus::Ok)) return 1;
  if (!PRISM_CHECK_EQ(made.path, std::string("pack/Inbox"))) return 1;
  const auto plan = tree.MakeUpdatePlan();
  bool staged = false;
  for (const auto& update : plan) {
    if (update.index < 0 && update.newPath == "pack/Inbox" && !update.newData && update.newProps) {
      staged = true;
    }
  }
  if (!PRISM_CHECK(staged)) return 1;
  // Uncommitted by design: the gallery still shows the catalog, so the folder
  // appears only after the executor's UpdateItems() commits the plan.
  window.SetSelection(rowOf("pack"));
  PostKey(window.Hwnd(), VK_RETURN);
  if (!PRISM_CHECK_EQ(window.Folder(), std::string("pack"))) return 1;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return 1;  // Inbox not committed yet
  PostKey(window.Hwnd(), VK_BACK);

  // ------------------- journey 13: Alt+double-click extracts; overlay content
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(prism::keys::PointerStroke{2, true}),
                      prism::keys::PointerAction::SmartExtract)) return 1;
  if (!PRISM_CHECK_EQ(prism::keys::ResolvePointer(prism::keys::PointerStroke{2, false}),
                      prism::keys::PointerAction::Activate)) return 1;
  prism::win::preview::QuickLookOverlay overlay;
  overlay.SetContent(items[6], true);  // root.png, still streaming in
  overlay.Open();
  if (!PRISM_CHECK(overlay.IsOpen())) return 1;
  if (!PRISM_CHECK_EQ(overlay.Title(), std::string("root.png"))) return 1;
  if (!PRISM_CHECK_EQ(overlay.Kind(), prism::PreviewKind::Image)) return 1;
  if (!PRISM_CHECK_EQ(overlay.Lines().size(), size_t{3})) return 1;  // size/kind/stream
  if (!PRISM_CHECK_EQ(overlay.Lines()[0].text, std::string("500 B"))) return 1;
  if (!PRISM_CHECK_EQ(overlay.Lines()[1].text, std::string("Image"))) return 1;
  if (!PRISM_CHECK_EQ(overlay.Lines()[2].text, std::string("Streaming"))) return 1;
  const auto card = overlay.Frame(clientW, clientH);
  if (!PRISM_CHECK_EQ(card.left, (clientW - card.Width()) / 2)) return 1;
  const auto content = overlay.ContentRect(clientW, clientH);
  if (!PRISM_CHECK(content.top > card.top)) return 1;  // the title strip is real
  if (!PRISM_CHECK(content.right < card.right)) return 1;

  window.Dispatch(WM_DESTROY, 0, 0);
  Pump();
  CoUninitialize();
  std::puts("ALL UI-JOURNEY CHECKS PASSED");
  return prism::test::FailureCount() == 0 ? 0 : 1;
}
