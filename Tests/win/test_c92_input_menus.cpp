// Card C92 -- the two interactions a first-day user tries and a shipped build
// answered with nothing: DOUBLE-CLICK and RIGHT-CLICK.
//
// WHAT IS PROVEN, AND AT WHICH LAYER
//   A. The window class carries CS_DBLCLKS. Without it Windows never posts
//      WM_LBUTTONDBLCLK and the handler at GalleryWindow's switch is dead code
//      (the field report's root cause). The assertion reads the REAL class
//      style: GetClassLongPtrW(hwnd, GCL_STYLE).
//   B. A POSTED double-click sequence (DOWN, UP, DBLCLK, UP) through the real
//      window procedure drills into a folder (the visible row list changes) and
//      opens a FILE through the production helper: the entry lands in a
//      %TEMP%\PrismArchive-Open\... directory and its bytes match. Enter is
//      dispatched too and must reach the SAME handler -- the card's "Enter and
//      double-click mean the same thing" rule.
//   C. WM_CONTEXTMENU builds three DIFFERENT menus (row / empty space / sidebar
//      node), each >= 5 entries, from PrismCore's model, with mutually disjoint
//      id sets. The probe receives the production HMENU, so GetMenuItemCount is
//      the menu the user would see, not a copy.
//   D. A disabled item explains itself: WM_MENUSELECT over its id puts the
//      model's reason on the status line.
//   E. The i18n words are the table's: with zh-CN installed, every row label
//      equals the table row (no English literal survives).
//
// NEGATIVE CONTROLS (D3/D26/D27), each its own executable compiled from THIS
// source; the marker prints at the FIRST failure and the process leaves 0:
//   * PRISM_C92_MUTANT_NO_DBLCLKS -- the class loses CS_DBLCLKS (arm no_dblclks).
//   * PRISM_C92_MUTANT_NO_CONTEXT -- WM_CONTEXTMENU opens nothing (arm
//     no_context).
// CTest pins the marker as PASS_REGULAR_EXPRESSION; no WILL_FAIL is stacked on a
// regex anywhere in this file.
//
// SKIP POLICY (D1): case B needs a real 7z.dll + 7z.exe to author and read an
// archive; when either is missing the binary exits 77 and the suite says so.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/InAppContextMenu.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

namespace {

namespace fs = std::filesystem;
using prism::ArchiveItem;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
std::wstring g_dllPath;
std::wstring g_exePath;

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void PumpMessages() {
  MSG message{};
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
}

bool RowCentre(GalleryWindow& window, int row, int* px, int* py) {
  const prism::layout::Layout layout = window.LayoutForTest();
  const auto& items = window.Items();
  if (row < 0 || row >= static_cast<int>(items.size())) return false;
  int bandTop = layout.contentInner.top;
  int column = 0;
  for (int i = 0; i <= row; ++i) {
    if (items[static_cast<std::size_t>(i)].isDir) {
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
    } else {
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
}

int RowOfPath(const GalleryWindow& window, const std::string& path) {
  const auto& rows = window.Items();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

// A real archive with one FOLDER and one FILE at the root: folder/inner.txt and
// alpha.txt. The folder proves "double-click enters"; the file proves "open
// writes the entry somewhere and hands it over".
struct Fixture {
  prism::testtmp::UniqueDir dir{L"c92", L"archive"};
  std::wstring archive;
  std::vector<ArchiveItem> catalog;
};

bool BuildFixture(Fixture& fixture) {
  if (!fixture.dir.ok()) return false;
  std::error_code ec;
  fs::create_directories(fixture.dir.path() / L"folder", ec);
  if (!WriteText(fixture.dir.path() / L"folder" / L"inner.txt", "inner-bytes")) return false;
  if (!WriteText(fixture.dir.path() / L"alpha.txt", "alpha-bytes")) return false;
  // A THIRD root row: the right-click preselection case needs a multi-selection
  // big enough to prove a plain re-select would destroy it.
  if (!WriteText(fixture.dir.path() / L"beta.txt", "beta-bytes")) return false;
  if (!prism::fixture::PackSolid(g_exePath, fixture.dir.wstr(), L"pack.7z",
                                 {L"folder", L"alpha.txt", L"beta.txt"})) {
    return false;
  }
  fixture.archive = (fixture.dir.path() / L"pack.7z").wstring();
  prism::win::SevenZipLibrary library;
  if (!library.Load(g_dllPath)) return false;
  prism::win::ArchiveReader reader(library);
  if (!reader.Open(fixture.archive)) return false;
  fixture.catalog = reader.ReadCatalog();
  reader.Close();
  return true;
}

// The production helpers, one per case, so a failed fixture is a SKIP and not a
// misleading red (D1).
struct Handles {
  prism::win::SevenZipLibrary library;
  std::shared_ptr<prism::app::OperationHandles> ops;
  bool ok = false;
};

bool OpenHandles(const Fixture& fixture, Handles& handles) {
  if (!handles.library.Load(g_dllPath)) return false;
  prism::app::OperationHandles::Config config;
  config.archivePath = fixture.archive;
  config.handlerSlots = 2;
  std::string why;
  handles.ops = prism::app::OperationHandles::Create(handles.library, config, &why);
  handles.ok = handles.ops != nullptr;
  if (!handles.ok) std::fprintf(stderr, "handles: %s\n", why.c_str());
  return handles.ok;
}

std::wstring LangDir() {
  wchar_t buffer[1024] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_C92_LANG_DIR", buffer, 1024);
  if (length == 0 || length >= 1024) return std::wstring();
  return std::wstring(buffer, length);
}

// One point per surface, computed from the window's own geometry, and refused if
// the window does NOT resolve it to that surface -- so a menu test cannot pass
// because every point fell into the same branch.
bool RowPoint(GalleryWindow& window, int* x, int* y) {
  const int row = RowOfPath(window, std::string("alpha.txt"));
  if (row < 0) return false;
  return RowCentre(window, row, x, y) && window.HitTestForTest(*x, *y) == row;
}

bool EmptyPoint(GalleryWindow& window, int* x, int* y) {
  const prism::layout::Layout layout = window.LayoutForTest();
  const int cx = static_cast<int>((layout.content.left + layout.content.right) / 2);
  for (int y2 = layout.contentInner.bottom - 4; y2 > layout.contentInner.top; y2 -= 24) {
    if (window.HitTestForTest(cx, y2) < 0) {
      *x = cx;
      *y = y2;
      return true;
    }
  }
  return false;
}

bool SidebarPoint(GalleryWindow& window, int* x, int* y) {
  const prism::win::sidebar::Layout& layout = window.SidebarLayout();
  if (layout.panel.Empty()) return false;
  *x = static_cast<int>((layout.panel.left + layout.panel.right) / 2);
  *y = static_cast<int>((layout.panel.top + layout.panel.bottom) / 2);
  return true;
}

struct SeenMenu {
  prism::ctxmenu::Surface surface = prism::ctxmenu::Surface::Empty;
  prism::ctxmenu::Menu model;
  std::vector<UINT> ids;
  int menuItemCount = 0;
  bool fromKeyboard = false;
};

// Dispatches WM_CONTEXTMENU exactly as the system would: SCREEN coordinates.
void RightClickAt(GalleryWindow& window, int clientX, int clientY) {
  POINT point{clientX, clientY};
  ClientToScreen(window.Hwnd(), &point);
  window.Dispatch(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window.Hwnd()),
                  MAKELPARAM(point.x, point.y));
}

bool SameSet(const std::vector<UINT>& a, const std::vector<UINT>& b) {
  std::vector<UINT> left = a;
  std::vector<UINT> right = b;
  std::sort(left.begin(), left.end());
  std::sort(right.begin(), right.end());
  return left == right;
}

bool Disjoint(const std::vector<UINT>& a, const std::vector<UINT>& b) {
  for (UINT id : a) {
    if (std::find(b.begin(), b.end(), id) != b.end()) return false;
  }
  return true;
}

}  // namespace

PRISM_TEST("c92.window_class_delivers_double_clicks") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c92 class", GetModuleHandleW(nullptr)))) return;
  const LONG_PTR style = ::GetClassLongPtrW(window.Hwnd(), GCL_STYLE);
  std::cout << "    [class] style=0x" << std::hex << style << std::dec
            << " CS_DBLCLKS=" << ((style & CS_DBLCLKS) != 0) << std::endl;
  PRISM_CHECK((style & CS_DBLCLKS) != 0);
  PRISM_CHECK(GalleryWindow::DoubleClicksDelivered());
  // And the class is the one the window really has: a class name lookup must
  // answer the same style (a second registration would answer 0 here).
  wchar_t name[256] = {};
  if (PRISM_CHECK(::GetClassNameW(window.Hwnd(), name, 255) > 0)) {
    WNDCLASSEXW info{};
    info.cbSize = sizeof(info);
    PRISM_CHECK(::GetClassInfoExW(GetModuleHandleW(nullptr), name, &info) != FALSE);
    PRISM_CHECK((info.style & CS_DBLCLKS) != 0);
  }
}

PRISM_TEST("c92.posted_double_click_enters_folder_then_opens_file") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  Handles handles;
  if (!PRISM_CHECK(OpenHandles(fixture, handles))) return;
  std::vector<ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c92 open", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.SetRootLabel(L"pack.7z");
  window.SetArchivePath(fixture.archive);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  if (!PRISM_CHECK_EQ(window.Folder(), std::string())) return;
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return;

  int openedIndex = -1;
  int openedCount = 0;
  bool openOk = false;
  std::wstring openedPath;
  window.SetOpenHandler([&](int catalogIndex) {
    ++openedCount;
    openedIndex = catalogIndex;
    const prism::app::OpenEntryOutcome outcome =
        prism::app::OpenArchiveEntry(*handles.ops, items, static_cast<uint32_t>(catalogIndex),
                                     fixture.archive);
    openOk = outcome.ok;
    openedPath = outcome.path;
    if (!outcome.ok) {
      std::fprintf(stderr, "open failed: %s\n", outcome.error.c_str());
    }
  });

  // ---- A POSTED double-click on the FOLDER: DOWN, UP, DBLCLK, UP ----------
  // PostMessage, not Dispatch: the sequence travels through the window's real
  // message queue and the real window procedure.
  const int folderRow = RowOfPath(window, "folder");
  if (!PRISM_CHECK(folderRow >= 0)) return;
  int fx = 0, fy = 0;
  if (!PRISM_CHECK(RowCentre(window, folderRow, &fx, &fy))) return;
  if (!PRISM_CHECK_EQ(window.HitTestForTest(fx, fy), folderRow)) return;
  ::PostMessageW(window.Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(fx, fy));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(fx, fy));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(fx, fy));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(fx, fy));
  PumpMessages();
  // "the listbox name changed": the folder is now the view, not the archive root.
  std::cout << "    [folder] now=" << window.Folder() << " rows=" << window.ItemCount()
            << std::endl;
  PRISM_CHECK_EQ(window.Folder(), std::string("folder"));
  if (!PRISM_CHECK_EQ(window.ItemCount(), 1)) return;
  PRISM_CHECK_EQ(window.Items()[0].path, std::string("folder/inner.txt"));

  // ---- Enter and double-click must reach the SAME open handler ------------
  if (!PRISM_CHECK(window.Back())) return;  // back to the archive root
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return;
  const int fileRow = RowOfPath(window, "alpha.txt");
  if (!PRISM_CHECK(fileRow >= 0)) return;
  int x = 0, y = 0;
  if (!PRISM_CHECK(RowCentre(window, fileRow, &x, &y))) return;
  if (!PRISM_CHECK_EQ(window.HitTestForTest(x, y), fileRow)) return;
  // Select it the way a click would, then Enter.
  window.Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
  window.Dispatch(WM_LBUTTONUP, 0, MAKELPARAM(x, y));
  const int beforeEnter = openedCount;
  window.Dispatch(WM_KEYDOWN, VK_RETURN, 0);
  PRISM_CHECK_EQ(openedCount, beforeEnter + 1);

  // ... and the double-click is the same door.
  ::PostMessageW(window.Hwnd(), WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONDBLCLK, MK_LBUTTON, MAKELPARAM(x, y));
  ::PostMessageW(window.Hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(x, y));
  PumpMessages();
  PRISM_CHECK_EQ(openedCount, beforeEnter + 2);
  PRISM_CHECK_EQ(openedIndex, static_cast<int>(items[static_cast<std::size_t>(fileRow)].index));

  // ---- the entry really landed in a temp directory, with its bytes ---------
  std::cout << "    [open] ok=" << openOk << " path=" << prism::win::WideToUtf8(openedPath)
            << std::endl;
  PRISM_CHECK(openOk);
  PRISM_CHECK(!openedPath.empty());
  PRISM_CHECK(fs::exists(openedPath));
  if (!openedPath.empty()) {
    const std::string body = ReadText(openedPath);
    PRISM_CHECK_EQ(body, std::string("alpha-bytes"));
    // The temp directory must be a NEW one under %TEMP%, never the archive's
    // own folder (extracting beside the archive is the OTHER verb's contract).
    const fs::path landed(openedPath);
    const fs::path tempRoot = prism::testtmp::ScratchRoot();
    const bool underTemp =
        landed.wstring().rfind(tempRoot.wstring(), 0) == 0 &&
        landed.wstring().find(L"PrismArchive-Open") != std::wstring::npos;
    std::cout << "    [open] tempRoot=" << prism::win::WideToUtf8(tempRoot.wstring())
              << " underTemp=" << underTemp << std::endl;
    PRISM_CHECK(underTemp);
  }
}

PRISM_TEST("c92.context_menu_has_three_surfaces_with_distinct_ids") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  std::vector<ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c92 menus", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.SetRootLabel(L"pack.7z");
  window.SetArchivePath(fixture.archive);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  std::vector<SeenMenu> seen;
  window.SetContextMenuProbe([&seen](const GalleryWindow::ContextMenuInvocation& invocation) {
    SeenMenu record;
    record.surface = invocation.surface;
    record.model = invocation.model;
    record.ids = invocation.ids;
    record.fromKeyboard = invocation.fromKeyboard;
    record.menuItemCount =
        invocation.menu != nullptr ? ::GetMenuItemCount(invocation.menu) : -1;
    seen.push_back(std::move(record));
  });

  // ---- (1) nothing selected: right-click a row SELECTS it ------------------
  // Explorer/Bandizip behaviour: the menu must describe the row under the
  // pointer, otherwise "Open" is greyed on the very file the user pointed at.
  int rx = 0, ry = 0;
  if (!PRISM_CHECK(RowPoint(window, &rx, &ry))) return;
  const int alphaRow = RowOfPath(window, "alpha.txt");
  if (!PRISM_CHECK(alphaRow >= 0)) return;
  PRISM_CHECK_EQ(window.SelectionCount(), 0);
  RightClickAt(window, rx, ry);
  std::cout << "    [preselect] selection=" << window.Selection()
            << " expected=" << alphaRow << " count=" << window.SelectionCount()
            << std::endl;
  PRISM_CHECK_EQ(window.Selection(), alphaRow);
  PRISM_CHECK_EQ(window.SelectionCount(), 1);
  if (!PRISM_CHECK_EQ(seen.size(), static_cast<std::size_t>(1))) return;
  {
    const prism::ctxmenu::Item* open =
        seen[0].model.Find(prism::ctxmenu::Command::Open);
    if (!PRISM_CHECK(open != nullptr)) return;
    // The preselection is exactly what makes this read enabled.
    PRISM_CHECK(open->enabled);
  }

  // ---- (3) empty space keeps the selection ---------------------------------
  int ex = 0, ey = 0;
  if (!PRISM_CHECK(EmptyPoint(window, &ex, &ey))) return;
  RightClickAt(window, ex, ey);
  PRISM_CHECK_EQ(window.Selection(), alphaRow);
  // ... and a sidebar node keeps it too.
  int sx = 0, sy = 0;
  if (PRISM_CHECK(SidebarPoint(window, &sx, &sy))) {
    RightClickAt(window, sx, sy);
  }
  if (!PRISM_CHECK_EQ(seen.size(), static_cast<std::size_t>(3))) return;

  PRISM_CHECK(seen[0].surface == prism::ctxmenu::Surface::Row);
  PRISM_CHECK(seen[1].surface == prism::ctxmenu::Surface::Empty);
  PRISM_CHECK(seen[2].surface == prism::ctxmenu::Surface::Sidebar);

  // Each menu is a real HMENU with >= 5 entries, and the model and the menu
  // agree (a model the user cannot see is not a menu).
  for (const SeenMenu& menu : seen) {
    std::cout << "    [menu] " << prism::ctxmenu::SurfaceName(menu.surface)
              << " items=" << menu.model.items.size() << " hmenu=" << menu.menuItemCount
              << std::endl;
    PRISM_CHECK(menu.model.items.size() >= 5);
    PRISM_CHECK_EQ(menu.menuItemCount, static_cast<int>(menu.model.items.size()));
    PRISM_CHECK_EQ(menu.ids.size(), menu.model.items.size());
    PRISM_CHECK(menu.model.ReasonsOnEveryDisabledItem());
    for (const prism::ctxmenu::Item& item : menu.model.items) {
      PRISM_CHECK(!item.label.empty());
    }
  }
  // ... and the three id sets are pairwise disjoint (a shared id would mean one
  // menu's command could be resolved as another's).
  PRISM_CHECK(Disjoint(seen[0].ids, seen[1].ids));
  PRISM_CHECK(Disjoint(seen[0].ids, seen[2].ids));
  PRISM_CHECK(Disjoint(seen[1].ids, seen[2].ids));
  PRISM_CHECK(!SameSet(seen[0].ids, seen[1].ids));
  PRISM_CHECK(!SameSet(seen[0].ids, seen[2].ids));
  PRISM_CHECK(!SameSet(seen[1].ids, seen[2].ids));

  // The card's required verbs, per surface.
  auto has = [](const SeenMenu& menu, prism::ctxmenu::Command command) {
    return menu.model.Find(command) != nullptr;
  };
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::Open));
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::ExtractSelected));
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::ExtractAll));
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::SmartExtractHere));
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::CopyPath));
  PRISM_CHECK(has(seen[0], prism::ctxmenu::Command::RevealArchive));
  PRISM_CHECK(has(seen[1], prism::ctxmenu::Command::ExtractAll));
  PRISM_CHECK(has(seen[1], prism::ctxmenu::Command::SelectAll));
  PRISM_CHECK(has(seen[1], prism::ctxmenu::Command::Refresh));
  PRISM_CHECK(has(seen[1], prism::ctxmenu::Command::RevealArchive));
  PRISM_CHECK(has(seen[2], prism::ctxmenu::Command::RevealArchive));
  PRISM_CHECK(has(seen[2], prism::ctxmenu::Command::CopyPath));

  // ---- (2) a multi-selection survives a right-click INSIDE it --------------
  // Ctrl+A selects all three root rows (folder + alpha.txt + beta.txt); a
  // right-click on one of them must not collapse the set to that row.
  prism::keys::Stroke selectAll;
  selectAll.key = 'A';
  selectAll.ctrl = true;
  window.DispatchKeyStroke(selectAll);
  if (!PRISM_CHECK_EQ(window.SelectionCount(), 3)) return;
  const std::vector<int> selectionBefore = window.SelectedRows();
  RightClickAt(window, rx, ry);  // alpha.txt is inside the set
  std::cout << "    [preselect] multi kept=" << (window.SelectedRows() == selectionBefore)
            << " count=" << window.SelectionCount() << std::endl;
  PRISM_CHECK(window.SelectedRows() == selectionBefore);
  PRISM_CHECK_EQ(window.SelectionCount(), 3);
  if (!PRISM_CHECK(seen.size() >= 4)) return;
  const SeenMenu multiRowMenu = seen.back();
  PRISM_CHECK(multiRowMenu.surface == prism::ctxmenu::Surface::Row);

  // ---- a disabled item explains itself on the status line ------------------
  // Only the archive verbs can be greyed while an archive is open, so close the
  // archive fact for this probe: the verbs stay VISIBLE with a reason.
  window.SetArchiveOpen(false);
  RightClickAt(window, ex, ey);
  const SeenMenu& greyed = seen.back();
  const prism::ctxmenu::Item* extractAll =
      greyed.model.Find(prism::ctxmenu::Command::ExtractAll);
  if (!PRISM_CHECK(extractAll != nullptr)) return;
  if (!PRISM_CHECK(!extractAll->enabled)) return;
  const std::size_t greyedSlot =
      static_cast<std::size_t>(extractAll - greyed.model.items.data());
  const UINT greyedId = greyed.ids[greyedSlot];
  window.Dispatch(WM_MENUSELECT, MAKEWPARAM(greyedId, MF_GRAYED), 0);
  std::cout << "    [reason] " << prism::win::WideToUtf8(window.StatusText()) << std::endl;
  PRISM_CHECK_EQ(prism::win::WideToUtf8(window.StatusText()), extractAll->disabledReason);
  window.SetArchiveOpen(true);

  // ---- the chosen id runs the verb (the same switch TrackPopupMenu reaches) --
  const prism::ctxmenu::Item* extract =
      multiRowMenu.model.Find(prism::ctxmenu::Command::ExtractSelected);
  if (!PRISM_CHECK(extract != nullptr)) return;
  PRISM_CHECK(extract->enabled);
  const std::size_t extractSlot =
      static_cast<std::size_t>(extract - multiRowMenu.model.items.data());
  const UINT extractId = multiRowMenu.ids[extractSlot];

  std::vector<prism::ctxmenu::Command> dispatched;
  window.SetContextCommandHandler([&dispatched](prism::ctxmenu::Command command) {
    dispatched.push_back(command);
  });
  // Resolve the id the way WM_COMMAND does, then run it.
  window.Dispatch(WM_COMMAND, MAKEWPARAM(extractId, 0), 0);
  if (!PRISM_CHECK_EQ(dispatched.size(), static_cast<std::size_t>(1))) return;
  PRISM_CHECK(dispatched[0] == prism::ctxmenu::Command::ExtractSelected);

  // ---- the keyboard opens the same menu (Shift+F10 / the Menu key) ----------
  const std::size_t beforeKeyboard = seen.size();
  window.Dispatch(WM_KEYDOWN, VK_APPS, 0);
  if (!PRISM_CHECK_EQ(seen.size(), beforeKeyboard + 1)) return;
  PRISM_CHECK(seen.back().fromKeyboard);
  // WM_CONTEXTMENU(-1,-1) is the exact message Shift+F10 produces.
  window.Dispatch(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window.Hwnd()),
                  static_cast<LPARAM>(static_cast<DWORD>(-1)));
  if (!PRISM_CHECK_EQ(seen.size(), beforeKeyboard + 2)) return;
  PRISM_CHECK(seen.back().fromKeyboard);
  std::cout << "    [kbd] surface=" << prism::ctxmenu::SurfaceName(seen.back().surface)
            << " items=" << seen.back().model.items.size() << std::endl;
}

PRISM_TEST("c92.context_menu_speaks_the_table_language") {
  const std::wstring dir = LangDir();
  if (!PRISM_CHECK(!dir.empty())) return;
  prism::i18n::Translator zh;
  std::string why;
  const std::string base = prism::win::WideToUtf8(dir);
  if (!PRISM_CHECK(zh.LoadCatalogFile(base + "/en-US.json", &why))) {
    std::fprintf(stderr, "en-US: %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(zh.LoadCatalogFile(base + "/zh-CN.json", &why))) {
    std::fprintf(stderr, "zh-CN: %s\n", why.c_str());
    return;
  }
  zh.SetLanguage("zh-CN");

  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  std::vector<ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c92 i18n", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.SetRootLabel(L"pack.7z");
  window.SetArchivePath(fixture.archive);
  window.ApplyLanguage(&zh);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  std::vector<prism::ctxmenu::Menu> menus;
  window.SetContextMenuProbe(
      [&menus](const GalleryWindow::ContextMenuInvocation& invocation) {
        menus.push_back(invocation.model);
      });
  int rx = 0, ry = 0;
  if (!PRISM_CHECK(RowPoint(window, &rx, &ry))) return;
  RightClickAt(window, rx, ry);
  if (!PRISM_CHECK_EQ(menus.size(), static_cast<std::size_t>(1))) return;

  // Every label equals the TABLE row, byte for byte -- not merely "not English".
  struct Expected {
    prism::ctxmenu::Command command;
    const char* key;
  };
  const Expected expected[] = {
      {prism::ctxmenu::Command::Open, "context.open"},
      {prism::ctxmenu::Command::ExtractSelected, "context.extract.selected"},
      {prism::ctxmenu::Command::ExtractAll, "context.extract.all"},
      {prism::ctxmenu::Command::SmartExtractHere, "context.smart.extract.here"},
      {prism::ctxmenu::Command::CopyPath, "context.copy.path"},
      {prism::ctxmenu::Command::RevealArchive, "context.reveal.archive"},
  };
  for (const Expected& row : expected) {
    const prism::ctxmenu::Item* item = menus[0].Find(row.command);
    if (!PRISM_CHECK(item != nullptr)) continue;
    std::cout << "    [i18n] " << row.key << " = " << item->label << std::endl;
    PRISM_CHECK_EQ(item->label, zh.Translate(row.key));
  }
  // The Chinese table really says Chinese (a table row could be English).
  const prism::ctxmenu::Item* open = menus[0].Find(prism::ctxmenu::Command::Open);
  if (PRISM_CHECK(open != nullptr)) {
    PRISM_CHECK_EQ(open->label, std::string("\u6253\u5F00"));  // 打开
  }
}


// ---------------------------------------------------------------------------
// Screenshot mode ("win_c92_input_menus <out-dir>"): the same production path
// (WM_CONTEXTMENU -> TrackPopupMenu) with a watcher thread that photographs the
// open popup and then closes it with WM_CANCELMODE + ESC. This is the only way
// to photograph a REAL system popup: the menu is owned by the shell, not by our
// renderer, so nothing in the window can draw it offscreen.
// ---------------------------------------------------------------------------
bool SaveHBitmapPng(HBITMAP bitmap, const std::wstring& path) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(factory.GetAddressOf()));
  if (FAILED(hr)) return false;
  Microsoft::WRL::ComPtr<IWICBitmap> source;
  hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha,
                                        source.GetAddressOf());
  if (FAILED(hr)) return false;
  Microsoft::WRL::ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.GetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.GetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  Microsoft::WRL::ComPtr<IPropertyBag2> bag;
  if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), bag.GetAddressOf()))) return false;
  if (FAILED(frame->Initialize(bag.Get()))) return false;
  if (FAILED(frame->WriteSource(source.Get(), nullptr))) return false;
  return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool CaptureOneMenu(GalleryWindow& window, int clientX, int clientY,
                    const std::wstring& path) {
  POINT point{clientX, clientY};
  ::ClientToScreen(window.Hwnd(), &point);
  const int width = 640;
  const int height = 360;
  std::thread watcher([hwnd = window.Hwnd(), sx = point.x, sy = point.y, width, height,
                       path]() {
    ::Sleep(900);
    // WIC is COM: this worker is its own apartment (CO_E_NOTINITIALIZED is what
    // the first capture attempt got).
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool saved = false;
    HDC screen = ::GetDC(nullptr);
    if (screen != nullptr) {
      HDC memory = ::CreateCompatibleDC(screen);
      HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
      if (memory != nullptr && bitmap != nullptr) {
        HGDIOBJ previous = ::SelectObject(memory, bitmap);
        const BOOL blitted =
            ::BitBlt(memory, 0, 0, width, height, screen, sx - 4, sy - 4, SRCCOPY);
        ::SelectObject(memory, previous);
        if (blitted != FALSE) saved = SaveHBitmapPng(bitmap, path);
      }
      if (bitmap != nullptr) ::DeleteObject(bitmap);
      if (memory != nullptr) ::DeleteDC(memory);
      ::ReleaseDC(nullptr, screen);
    }
    if (SUCCEEDED(com)) ::CoUninitialize();
    std::printf("    [shot] %s saved=%d\n", prism::win::WideToUtf8(path).c_str(),
                static_cast<int>(saved));
    // Close the popup the documented way. NOT with an injected ESC: keybd_event
    // is asynchronous, and the ESC of the PREVIOUS menu landed in the NEXT one
    // (that is why the third screenshot showed a closed menu).
    ::PostMessageW(hwnd, WM_CANCELMODE, 0, 0);
    ::Sleep(200);
  });
  window.Dispatch(WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window.Hwnd()),
                  MAKELPARAM(point.x, point.y));
  watcher.join();
  return true;
}

int CaptureMenus(const std::string& outDir) {
  Fixture fixture;
  if (!BuildFixture(fixture)) {
    std::printf("SKIP: fixture\n");
    return kSkipReturnCode;
  }
  std::vector<ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!window.Create(L"PrismArchive -- c92 menus", GetModuleHandleW(nullptr))) {
    std::printf("SKIP: no window station\n");
    return kSkipReturnCode;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"pack.7z");
  window.SetArchivePath(fixture.archive);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1100, 720));
  ::SetWindowPos(window.Hwnd(), HWND_TOP, 120, 80, 1100, 720, SWP_SHOWWINDOW);
  window.Show();
  ::SetForegroundWindow(window.Hwnd());
  ::Sleep(400);

  const fs::path root(outDir);
  std::error_code ec;
  fs::create_directories(root, ec);

  int rx = 0, ry = 0;
  if (!RowPoint(window, &rx, &ry)) return 1;
  CaptureOneMenu(window, rx, ry, (root / L"c92-context-file.png").wstring());

  int ex = 0, ey = 0;
  if (!EmptyPoint(window, &ex, &ey)) ex = 40, ey = 400;
  CaptureOneMenu(window, ex, ey, (root / L"c92-context-empty.png").wstring());
  ::Sleep(350);  // let the previous watcher's ESC land before opening the next

  int sx = 0, sy = 0;
  if (SidebarPoint(window, &sx, &sy)) {
    CaptureOneMenu(window, sx, sy, (root / L"c92-context-tree.png").wstring());
  }
  std::printf("C92 SCREENSHOTS WRITTEN\n");
  return 0;
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed" << std::endl;
    return kSkipReturnCode;
  }
  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    // Screenshot mode: the suite's assertions are skipped, the POPUPS are real.
    if (g_dllPath.empty() || g_exePath.empty()) {
      std::printf("SKIP: %s\n", prism::fixture::ExePathWhy().c_str());
      ::CoUninitialize();
      return kSkipReturnCode;
    }
    const int shot = CaptureMenus(argv[1]);
    ::CoUninitialize();
    return shot;
  }
  if (g_dllPath.empty() || g_exePath.empty()) {
    std::cout << "SKIP: " << prism::fixture::ExePathWhy() << std::endl;
    ::CoUninitialize();
    return kSkipReturnCode;
  }
#if defined(PRISM_C92_MUTANT_NO_DBLCLKS)
  std::cout << "C92 MUTANT ACTIVE: the window class is registered without CS_DBLCLKS"
            << std::endl;
  prism::test::SetMutationVerdict("C92 MUTATION CAUGHT: no_dblclks",
                                  [] { return !GalleryWindow::DoubleClicksDelivered(); });
#endif
#if defined(PRISM_C92_MUTANT_NO_CONTEXT)
  std::cout << "C92 MUTANT ACTIVE: WM_CONTEXTMENU opens nothing" << std::endl;
  prism::test::SetMutationVerdict("C92 MUTATION CAUGHT: no_context",
                                  [] { return !GalleryWindow::ContextMenusEnabled(); });
#endif
#if defined(PRISM_C92_MUTANT_NO_PRESELECT)
  std::cout << "C92 MUTANT ACTIVE: a right-click no longer selects the row under it"
            << std::endl;
  prism::test::SetMutationVerdict("C92 MUTATION CAUGHT: no_preselect",
                                  [] { return !GalleryWindow::ContextPreselectsRow(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL C92 INPUT-MENU CHECKS PASSED" << std::endl;
  return 0;
}
