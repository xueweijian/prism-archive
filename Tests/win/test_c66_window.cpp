// PrismArchive -- PrismWin
// C66: THE WINDOW THAT WAS 16 px WIDER THAN THE SCREEN, plus the two patches
// this card wires (Help > About, Ctrl+F search).
//
// THE MEASURED DEFECT. %LOCALAPPDATA%\PrismArchive\settings.json held
// "window": {"width": 1552, "height": 832} on a machine whose work area is
// 1536x816. The last three action buttons (添加/删除/测试) were painted off the
// right edge. This suite proves the two rules AND their negative controls:
//
//   1. PERSIST THE VISIBLE FRAME. A maximized window's GetWindowRect is the
//      work area inflated by the DWM invisible border (8 px per side). Case 1
//      measures that on a REAL window and asserts the inflation is positive and
//      that the old number does not fit the work area -- the reported 1552x832
//      is not a coincidence, it is the maximized window rect.
//      CONTROL: PRISM_C66_MUTANT_NO_DWM restores the pre-fix reading and the
//      five-cycle stability assertions go red.
//   2. CLAMP ON RESTORE. Every restored frame is clamped into
//      MonitorFromRect(...).rcWork before SetWindowPos, so GetWindowRect itself
//      ends up on screen. Case 2 restores a frame LARGER than the work area.
//      CONTROL: PRISM_C66_MUTANT_NO_WORKAREA_CLAMP compiles the clamp out and
//      that case goes red.
//
// Both arms print "C66 MUTATION CAUGHT: <arm>" at the first red assertion
// through mini_test's verdict (CONTRACTS D26/D27), and only when the injection
// is proved active -- an unrelated red stays red without claiming CAUGHT (D3).
//
// The last two cases are the PATCH WIRING, driven on a real HWND and a real
// HMENU: the Help > About item is found on the menu bar the window actually
// installed, the dialog is opened through WM_COMMAND and closed by posting
// WM_CLOSE from a worker thread (the modal loop pumps, so this is the door the
// user has), and Ctrl+F is dispatched through the SAME router WM_KEYDOWN uses.
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ActionBarModel.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/AboutDialog.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/WindowFrame.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

using prism::layout::Rect;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
bool g_noWindowStation = false;
// Set by case 1: the frame the app would PERSIST did not fit the work area.
// It is both the failing assertion and the "the injected defect really fired"
// predicate for the mutation verdict (CONTRACTS D3: red is not CAUGHT).
bool g_persistedOverflowsWork = false;
// Set by the explicit persist case: PersistFrame() handed back a frame bigger
// than the work area it was given. That is the predicate for the persist-clamp
// arm -- and it is driven by an INPUT the test builds, so it never depends on
// the desktop size of the machine running it.
bool g_explicitPersistOverflowsWork = false;

#if defined(PRISM_C66_MUTANT_NO_DWM)
constexpr const char* kMutation = "no_dwm";
#elif defined(PRISM_C66_MUTANT_NO_PERSIST_CLAMP)
constexpr const char* kMutation = "no_persist_clamp";
#elif defined(PRISM_C66_MUTANT_NO_WORKAREA_CLAMP)
constexpr const char* kMutation = "no_workarea_clamp";
#else
constexpr const char* kMutation = nullptr;
#endif

bool MakeWindow(GalleryWindow* window, const wchar_t* title) {
  if (window->Create(title, GetModuleHandleW(nullptr))) return true;
  std::puts("  [skip] no window station");
  g_noWindowStation = true;
  return false;
}

// A catalog with distinct names, so a substring filter has something to hide.
struct Toy {
  std::vector<prism::ArchiveItem> items;
  Toy() {
    const char* names[] = {"alpha.txt", "alpine.txt", "beta.txt", "gamma.txt"};
    uint32_t index = 0;
    for (const char* name : names) {
      prism::ArchiveItem item;
      item.index = index++;
      item.path = name;
      item.size = 1024;
      items.push_back(item);
    }
  }
};

std::size_t ListRows(GalleryWindow* window) {
  return static_cast<std::size_t>(window->ListRowCount());
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C66 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C66 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C66_MUTANT_NO_DWM)
                                        // The injection fired when the frame
                                        // the app would persist is the raw
                                        // window rect -- i.e. it does not fit
                                        // the screen, which is case 1's failure.
                                        return g_persistedOverflowsWork;
#elif defined(PRISM_C66_MUTANT_NO_PERSIST_CLAMP)
                                        // The injection fired when an explicitly
                                        // oversized frame came back unclamped.
                                        return g_explicitPersistOverflowsWork;
#else
                                        const Rect work{0, 0, 1536, 816};
                                        return prism::layout::ClampFrameToWorkArea(
                                                   Rect{0, 0, 1552, 832}, work) ==
                                               Rect{0, 0, 1552, 832};
#endif
                                      });
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noWindowStation) {
    std::puts("C66 SKIPPED: no window station");
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL WINDOW-RECT CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. ROOT CAUSE: the maximized window rect is the work area plus the border.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 root cause: a maximized window's GetWindowRect does not fit the work area") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 maximize")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  ::ShowWindow(window.Hwnd(), SW_MAXIMIZE);
  ::UpdateWindow(window.Hwnd());

  const prism::win::frame::Measurement m = prism::win::frame::Measure(window.Hwnd());
  std::printf("  [max] work=%dx%d windowRect=%dx%d visible=%dx%d borders=%d,%d,%d,%d dwm=%d\n",
              work.Width(), work.Height(), m.window.Width(), m.window.Height(),
              m.visible.Width(), m.visible.Height(), m.borders.left, m.borders.top,
              m.borders.right, m.borders.bottom, static_cast<int>(m.dwm));
  // THE DEFECT, MEASURED: a maximized window's GetWindowRect does not fit the
  // work area. On this machine it is 1552x832 against 1536x816 -- byte for byte
  // the number the user's settings.json held. On a session where DWM answers,
  // the visible frame IS the work area and the difference is the shadow border;
  // where it does not (dwm=0 here), the inflation lives entirely in
  // GetWindowRect, which is the same rectangle and the same bug.
  PRISM_CHECK(!prism::layout::FrameFitsWorkArea(m.window, work));
  PRISM_CHECK(m.window.Width() >= work.Width());
  PRISM_CHECK(m.window.Height() >= work.Height());
  if (m.dwm) {
    PRISM_CHECK_EQ(m.visible.Width(), work.Width());
    PRISM_CHECK_EQ(m.visible.Height(), work.Height());
    PRISM_CHECK_EQ(m.window.Width() - m.visible.Width(), m.borders.left + m.borders.right);
    PRISM_CHECK_EQ(m.window.Height() - m.visible.Height(), m.borders.top + m.borders.bottom);
    PRISM_CHECK(m.borders.left > 0 || m.borders.top > 0);
  }
  // THE FIX: what gets persisted is the NORMAL frame (never the maximized
  // rectangle) AND it is clamped into the work area before it is written, so it
  // fits the screen it will be restored onto -- on any desktop, not just this
  // one (case 1b builds an oversized input on purpose).
  // What is WRITTEN to settings.json is a SIZE, so "fits" is a size test: the
  // position is the window's own and is clamped on restore (case 2).
  const Rect persisted = prism::win::frame::FrameToPersist(window.Hwnd());
  g_persistedOverflowsWork =
      persisted.Width() > work.Width() || persisted.Height() > work.Height();
  PRISM_CHECK(!g_persistedOverflowsWork);
  PRISM_CHECK(persisted.Width() <= work.Width());
  PRISM_CHECK(persisted.Height() <= work.Height());
  std::printf("  [persist] rect=%d,%d %dx%d (work %dx%d)\n", persisted.left, persisted.top,
              persisted.Width(), persisted.Height(), work.Width(), work.Height());
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 1b. PERSIST is clamped as well as RESTORE.
//
// WHY THIS CASE EXISTS: the first version of case 1 above trusted the window's
// own default size and went green on a 1536x816 desktop while the CI runner --
// whose desktop is smaller than the default window -- persisted a frame wider
// than its work area and went red. The input here is BUILT by the test (the
// screen plus 400 x 300), so the assertion says the same thing on every machine
// and the pre-fix behaviour cannot hide behind a convenient desktop.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 persisting an oversized frame writes one that fits the work area") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 persist")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const Rect oversized{work.left, work.top, work.left + work.Width() + 400,
                       work.top + work.Height() + 300};
  const Rect persisted = prism::win::frame::PersistFrame(window.Hwnd(), oversized);
  std::printf("  [persist-explicit] in=%dx%d out=%dx%d work=%dx%d\n", oversized.Width(),
              oversized.Height(), persisted.Width(), persisted.Height(), work.Width(),
              work.Height());
  g_explicitPersistOverflowsWork =
      persisted.Width() > work.Width() || persisted.Height() > work.Height();
  PRISM_CHECK(!g_explicitPersistOverflowsWork);
  PRISM_CHECK(persisted.Width() <= work.Width());
  PRISM_CHECK(persisted.Height() <= work.Height());
  // The schema's floor is never broken when the screen can hold it (the store
  // repairs window.width/height up to 640x480, so a persisted value below it
  // would come back as an overflow this card exists to prevent).
  if (work.Width() >= prism::layout::kMinWindowWidth) {
    PRISM_CHECK(persisted.Width() >= prism::layout::kMinWindowWidth);
  }
  if (work.Height() >= prism::layout::kMinWindowHeight) {
    PRISM_CHECK(persisted.Height() >= prism::layout::kMinWindowHeight);
  }
  // ...and the window's own answer obeys the same rule on every desktop.
  const Rect real = prism::win::frame::FrameToPersist(window.Hwnd());
  std::printf("  [persist-real] %dx%d work=%dx%d\n", real.Width(), real.Height(),
              work.Width(), work.Height());
  PRISM_CHECK(real.Width() <= work.Width());
  PRISM_CHECK(real.Height() <= work.Height());
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 2. An oversized saved frame is clamped on restore.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 restoring a frame larger than the work area lands inside it") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 oversize")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // Exactly the reported shape: the work area plus 16 px on both axes.
  prism::win::frame::Restore restored =
      window.RestoreFrame(work.Width() + 16, work.Height() + 16);
  std::printf("  [restore] work=%dx%d requested=%dx%d window=%dx%d visible=%dx%d ok=%d\n",
              restored.work.Width(), restored.work.Height(), restored.requested.Width(),
              restored.requested.Height(), restored.applied.Width(), restored.applied.Height(),
              restored.visible.Width(), restored.visible.Height(),
              static_cast<int>(restored.ok));
  if (!PRISM_CHECK(restored.ok)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // The card's assertion, literally: GetWindowRect itself is inside the screen.
  PRISM_CHECK(prism::layout::FrameFitsWorkArea(restored.applied, restored.work));
  PRISM_CHECK(prism::layout::FrameFitsWorkArea(restored.visible, restored.work));
  PRISM_CHECK(restored.applied.right <= restored.work.right);
  PRISM_CHECK(restored.applied.bottom <= restored.work.bottom);
  // ...and the whole client area of that window is on screen too, which is what
  // the action island's last button needs.
  RECT client{};
  PRISM_CHECK(::GetClientRect(window.Hwnd(), &client) != FALSE);
  POINT origin{0, 0};
  PRISM_CHECK(::ClientToScreen(window.Hwnd(), &origin) != FALSE);
  PRISM_CHECK(origin.x + client.right <= restored.work.right);
  PRISM_CHECK(origin.y + client.bottom <= restored.work.bottom);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 3. Five save -> restore cycles must NOT grow the window by the border.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 five save/restore cycles stay within 1 px (no +16 drift)") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 cycles")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const int startWidth = (std::min)(1200, work.Width() - 40);
  const int startHeight = (std::min)(760, work.Height() - 40);
  int width = startWidth;
  int height = startHeight;
  int firstWidth = 0;
  int firstHeight = 0;
  for (int cycle = 0; cycle < 5; ++cycle) {
    const prism::win::frame::Restore restored = window.RestoreFrame(width, height);
    if (!PRISM_CHECK(restored.ok)) break;
    // The app persists THIS: what the user sees.
    const Rect persisted = prism::win::frame::FrameToPersist(window.Hwnd());
    std::printf("  [cycle %d] saved=%dx%d restored visible=%dx%d persisted=%dx%d\n", cycle,
                width, height, restored.visible.Width(), restored.visible.Height(),
                persisted.Width(), persisted.Height());
    if (cycle == 0) {
      firstWidth = restored.visible.Width();
      firstHeight = restored.visible.Height();
      PRISM_CHECK(firstWidth > 0);
    } else {
      // A border-sized drift per cycle is 8 px; allow 1 px of rounding only.
      PRISM_CHECK(restored.visible.Width() - firstWidth <= 1);
      PRISM_CHECK(firstWidth - restored.visible.Width() <= 1);
      PRISM_CHECK(restored.visible.Height() - firstHeight <= 1);
      PRISM_CHECK(firstHeight - restored.visible.Height() <= 1);
    }
    // The next "launch" starts from what this close wrote.
    width = persisted.Width();
    height = persisted.Height();
    if (width <= 0 || height <= 0) break;
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 4. A stale saved rectangle (negative coordinates) still lands on a monitor.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 a frame saved at negative coordinates restores on a real monitor") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 negative")) return;
  window.SetItems(nullptr);
  const Rect saved{-2400, -600, -1200, 200};
  const prism::win::frame::Restore restored =
      prism::win::frame::RestoreVisibleFrame(window.Hwnd(), saved);
  std::printf("  [negative] work=%dx%d applied=%dx%d ok=%d\n", restored.work.Width(),
              restored.work.Height(), restored.applied.Width(), restored.applied.Height(),
              static_cast<int>(restored.ok));
  if (!PRISM_CHECK(restored.ok)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  PRISM_CHECK(prism::layout::FrameFitsWorkArea(restored.applied, restored.work));
  PRISM_CHECK(restored.applied.left >= restored.work.left);
  PRISM_CHECK(restored.applied.top >= restored.work.top);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 5. 800 / 1280 / 1536: the last action button, or NO buttons at all.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 at 800/1280/1536 the island is either complete or absent") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 widths")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const int heights[] = {800, 800, 816};
  const int widths[] = {800, 1280, 1536};
  for (int i = 0; i < 3; ++i) {
    const int width = (std::min)(widths[i], work.Width());
    const int height = (std::min)(heights[i], work.Height());
    const prism::win::frame::Restore restored = window.RestoreFrame(width, height);
    if (!PRISM_CHECK(restored.ok)) continue;
    RECT client{};
    PRISM_CHECK(::GetClientRect(window.Hwnd(), &client) != FALSE);
    POINT origin{0, 0};
    PRISM_CHECK(::ClientToScreen(window.Hwnd(), &origin) != FALSE);
    const prism::layout::Layout layout =
        prism::layout::Compute(client.right, client.bottom, /*sidebarVisible=*/true,
                               /*listMode=*/false, /*tabBarVisible=*/false);
    const prism::actionbar::Layout band = prism::actionbar::Compute(layout);
    std::printf("  [width %d] client=%dx%d buttons=%zu dropped=%d island=%d..%d\n", width,
                client.right, client.bottom, band.buttons.size(), band.dropped,
                band.island.left, band.island.right);
    if (band.dropped > 0) {
      // The rule this card adds: a dropped button means NO buttons. Painting the
      // first six and hiding the tail is the screenshot.
      PRISM_CHECK(band.buttons.empty());
      PRISM_CHECK(band.island.Empty());
      continue;
    }
    if (!PRISM_CHECK(!band.buttons.empty())) continue;
    const Rect last = band.buttons.back().rect;
    // In client space, inside the layout band...
    PRISM_CHECK(layout.actionIsland.Contains(last));
    // ...and in SCREEN space, inside the work area: the assertion the user
    // actually cares about.
    const Rect onScreen{origin.x + last.left, origin.y + last.top, origin.x + last.right,
                        origin.y + last.bottom};
    PRISM_CHECK(onScreen.right <= restored.work.right);
    PRISM_CHECK(onScreen.bottom <= restored.work.bottom);
    PRISM_CHECK(onScreen.left >= restored.work.left);
    PRISM_CHECK(onScreen.top >= restored.work.top);
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 6. Help > About: on the real menu bar, and it opens a real dialog.
// ---------------------------------------------------------------------------
namespace {

struct AboutProbe {
  DWORD pid = 0;
  HWND dialog = nullptr;
};

BOOL CALLBACK FindAboutProc(HWND hwnd, LPARAM param) {
  auto* probe = reinterpret_cast<AboutProbe*>(param);
  DWORD pid = 0;
  ::GetWindowThreadProcessId(hwnd, &pid);
  if (pid != probe->pid) return TRUE;
  wchar_t name[64]{};
  if (::GetClassNameW(hwnd, name, ARRAYSIZE(name)) == 0) return TRUE;
  if (::lstrcmpW(name, prism::win::about::AboutDialog::kClassName) != 0) return TRUE;
  probe->dialog = hwnd;
  return FALSE;
}

}  // namespace

PRISM_TEST("c66 Help > About is a menu item and WM_COMMAND opens the dialog") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 about")) return;
  window.SetItems(nullptr);
  HMENU bar = ::GetMenu(window.Hwnd());
  if (!PRISM_CHECK(bar != nullptr)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // Walk the bar the window actually installed: the item is found by its
  // COMMAND ID, not by a label the test guessed.
  const int count = ::GetMenuItemCount(bar);
  bool found = false;
  std::wstring helpTitle;
  for (int i = 0; i < count && !found; ++i) {
    HMENU popup = ::GetSubMenu(bar, i);
    if (popup == nullptr) continue;
    // C83: the bar is owner-drawn, so GetMenuStringW answers 0 for the top-level
    // item; the ITEM is found by the About command it contains (unchanged) and
    // its title comes from the window, which is the text WM_DRAWITEM paints.
    const int items = ::GetMenuItemCount(popup);
    for (int j = 0; j < items; ++j) {
      if (::GetMenuItemID(popup, j) ==
          static_cast<UINT>(prism::win::about::AboutDialog::kMenuAboutId)) {
        found = true;
        helpTitle = window.MenuBarItemText(i);
        break;
      }
    }
  }
  if (!PRISM_CHECK(found)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  std::printf("  [menu] Help popup = '%ls'\n", helpTitle.c_str());

  // Open it exactly as the menu does, and close it from ANOTHER thread: the
  // dialog is modal, so the owner's thread is inside OpenModal's loop and only a
  // posted message can reach the dialog (bounded: 5 s, CONTRACTS D29).
  AboutProbe probe;
  probe.pid = ::GetCurrentProcessId();
  const HWND mainWindow = window.Hwnd();
  std::thread closer([&probe, mainWindow] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      ::EnumWindows(FindAboutProc, reinterpret_cast<LPARAM>(&probe));
      if (probe.dialog != nullptr) break;
      ::Sleep(20);
    }
    if (probe.dialog != nullptr) {
      ::PostMessageW(probe.dialog, WM_CLOSE, 0, 0);
      return;
    }
    // Nothing appeared: unblock the owner so the suite fails on the assertion
    // instead of hanging (D29: every wait is bounded and also watches its exit).
    ::PostMessageW(mainWindow, WM_NULL, 0, 0);
  });
  window.Dispatch(WM_COMMAND,
                  MAKEWPARAM(prism::win::about::AboutDialog::kMenuAboutId, 0), 0);
  closer.join();
  PRISM_CHECK(probe.dialog != nullptr);
  PRISM_CHECK(::IsWindow(mainWindow) != FALSE);
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 7. Ctrl+F opens the field and the row count really changes.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 Ctrl+F opens the search field and filters the list") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c66 search")) return;
  Toy toy;
  window.SetItems(&toy.items);
  window.SetRootLabel(L"toy.7z");
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 800));
  window.SetListMode(true);
  const std::size_t all = ListRows(&window);
  std::printf("  [search] rows before=%zu\n", all);
  if (!PRISM_CHECK(all == static_cast<std::size_t>(4))) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  if (!PRISM_CHECK(!window.SearchOpen())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // The SAME router WM_KEYDOWN uses (Ctrl is read from the thread's key state
  // there, which a suite cannot set; the stroke is the router's real input).
  prism::keys::Stroke stroke;
  stroke.key = 'F';
  stroke.ctrl = true;
  window.DispatchKeyStroke(stroke);
  if (!PRISM_CHECK(window.SearchOpen())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // Typing goes through WM_CHAR (the only route that honours the layout/IME).
  for (const wchar_t unit : std::wstring(L"alp")) {
    window.Dispatch(WM_CHAR, static_cast<WPARAM>(unit), 0);
  }
  const std::size_t filtered = ListRows(&window);
  std::printf("  [search] query='%s' rows after=%zu\n", window.SearchModel().Query().c_str(),
              filtered);
  PRISM_CHECK_EQ(window.SearchModel().Query(), std::string("alp"));
  PRISM_CHECK(filtered < all);          // the list really shrank
  PRISM_CHECK(filtered == static_cast<std::size_t>(2));  // alpha.txt, alpine.txt

  // Esc clears, then closes (the model's three-stage rule); the rows come back.
  window.DispatchKeyStroke(prism::keys::Stroke{0x1B, false, false, false});
  window.DispatchKeyStroke(prism::keys::Stroke{0x1B, false, false, false});
  PRISM_CHECK(!window.SearchOpen());
  PRISM_CHECK_EQ(ListRows(&window), all);
  window.Dispatch(WM_DESTROY, 0, 0);
}
