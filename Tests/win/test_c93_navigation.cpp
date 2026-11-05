// Card C93 -- 面包屑可点 + 树根点击回根, proven through the REAL window class.
//
// WHAT IS PROVEN (each number is only produced by the production path):
//   1. After drilling into "docs/sub", a click on SEGMENT 0 of the breadcrumb
//      (the archive-root crumb) puts the window back at CurrentPath()=="" and
//      the row list back to the root's row count.
//   2. After drilling into "docs/sub", a click on the sidebar's ROOT ROW puts
//      the window back at CurrentPath()=="" and the tree highlight with it.
//   3. A click on the crumb the user is ALREADY on changes nothing: same path,
//      same rows, and NavigationEvents() unchanged (the "no navigation event"
//      half of requirement 1 is a counter, not an impression).
//   4. Hovering a crumb paints a feedback rect that is the crumb's own measured
//      span -- not the whole bar -- and leaving the window drops it.
//   5. Backspace still walks up (the pre-C93 semantics survived the new wiring).
//
// NEGATIVE CONTROLS (D3/D26/D27), each its own executable compiled from THIS
// source; the marker prints at the FIRST failure and the process leaves 0:
//   * PRISM_C93_MUTANT_DEAD_CRUMB   -- no crumb pixel resolves to a level
//        (the field report's "the bar is dead text"). Assertion 1 goes red.
//   * PRISM_C93_MUTANT_ROOT_NOOP    -- the tree's ROOT row answers nothing
//        while sub-folders still navigate. Assertion 2 goes red.
//   * PRISM_C93_MUTANT_CRUMB_RESETS -- pressing the current crumb counts as a
//        navigation (history + viewport reset). Assertion 3 goes red.
// CTest pins the marker as PASS_REGULAR_EXPRESSION; no WILL_FAIL is stacked on
// a regex anywhere in this file.
//
// NO SKIP, NO ENGINE: the catalog is a plain vector<ArchiveItem> handed through
// the production SetItems entry point, so this suite cannot skip silently on a
// machine without 7z.dll.
//
// SCREENSHOT MODE: "win_c93_navigation <out-dir>" drives the same window, sets
// the REAL pointer over a crumb, lets the real paint path run and photographs
// the screen (BitBlt) -- the renderer is the product's, not a mock.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ArchiveTreeView.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

namespace {

using prism::ArchiveItem;
using prism::tree::kNoLevel;
using prism::tree::kRootLevel;
using prism::win::gallery::GalleryWindow;

// The field report's archive: a two-level chain plus files at two depths.
struct Fixture {
  std::vector<ArchiveItem> items;

  Fixture() {
    Add(0, "docs", true);
    Add(1, "docs/sub", true);
    Add(2, "docs/sub/a.txt", false);
    Add(3, "docs/b.txt", false);
    Add(4, "root.txt", false);
  }

  void Add(std::uint32_t index, const char* path, bool isDir) {
    ArchiveItem item;
    item.index = index;
    item.path = path;
    item.isDir = isDir;
    item.size = 10 * (index + 1);
    item.packSize = 5 * (index + 1);
    items.push_back(std::move(item));
  }
};

bool PumpUntilPainted(int rounds) {
  bool any = false;
  for (int i = 0; i < rounds; ++i) {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
      any = true;
    }
    ::Sleep(10);
  }
  return any;
}

int RowOfPath(const GalleryWindow& window, const std::string& path) {
  const std::vector<ArchiveItem>& rows = window.Items();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

bool MakeWindow(GalleryWindow& window, Fixture& fixture, const wchar_t* name) {
  if (!window.Create(name, ::GetModuleHandleW(nullptr))) return false;
  window.SetItems(&fixture.items);
  window.SetRootLabel(L"pack.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  return true;
}

// Setup: enter a folder through the SAME row activation Enter uses.
bool EnterFolder(GalleryWindow& window, const std::string& path) {
  const int row = RowOfPath(window, path);
  if (row < 0) {
    std::printf("    [enter] row missing path=%s folder=%s rows=%d\n", path.c_str(),
                window.Folder().c_str(), window.ItemCount());
    return false;
  }
  window.ActivateRow(row);
  if (window.Folder() != path) {
    std::printf("    [enter] path=%s row=%d folder=%s rows=%d\n", path.c_str(), row,
                window.Folder().c_str(), window.ItemCount());
    return false;
  }
  return true;
}

bool CrumbCentre(GalleryWindow& window, int level, int* x, int* y) {
  int left = 0;
  int right = 0;
  if (!window.CrumbSpanForTest(level, &left, &right)) return false;
  if (right <= left) return false;
  *x = (left + right) / 2;
  *y = (window.NavBarTop() + window.NavBarBottom()) / 2;
  return true;
}

// ---------------------------------------------------------------------------
// Screenshot mode. Same production window, real DComp/D2D painting, real
// pointer position; the photograph is the desktop pixels (BitBlt), because a
// composition swap chain is not captured by PrintWindow.
// ---------------------------------------------------------------------------
bool SaveHBitmapPng(HBITMAP bitmap, const std::wstring& path) {
  Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(factory.GetAddressOf())))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmap> source;
  if (FAILED(factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha,
                                              source.GetAddressOf()))) {
    return false;
  }
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

bool CaptureWindowPng(HWND hwnd, const std::wstring& path, const std::string& label) {
  RECT rect{};
  if (!::GetWindowRect(hwnd, &rect)) return false;
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  if (width <= 0 || height <= 0) return false;
  HDC screen = ::GetDC(nullptr);
  if (screen == nullptr) return false;
  HDC memory = ::CreateCompatibleDC(screen);
  HBITMAP bitmap = ::CreateCompatibleBitmap(screen, width, height);
  bool saved = false;
  if (memory != nullptr && bitmap != nullptr) {
    HGDIOBJ previous = ::SelectObject(memory, bitmap);
    const BOOL blitted = ::BitBlt(memory, 0, 0, width, height, screen, rect.left, rect.top,
                                  SRCCOPY);
    ::SelectObject(memory, previous);
    if (blitted != FALSE) saved = SaveHBitmapPng(bitmap, path);
  }
  if (bitmap != nullptr) ::DeleteObject(bitmap);
  if (memory != nullptr) ::DeleteDC(memory);
  ::ReleaseDC(nullptr, screen);
  std::printf("    [shot] %s saved=%d\n", label.c_str(), static_cast<int>(saved));
  return saved;
}

// Put the REAL cursor over a client point and let the window's own WM_MOUSEMOVE
// handler (the same one a user's mouse drives) set the hover state.
void HoverClientPoint(GalleryWindow& window, int x, int y) {
  POINT screen{x, y};
  ::ClientToScreen(window.Hwnd(), &screen);
  ::SetCursorPos(screen.x, screen.y);
  window.Dispatch(WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
  PumpUntilPainted(20);
}

int CaptureNavigationScreens(const std::string& outDir) {
  Fixture fixture;
  GalleryWindow window;
  if (!window.Create(L"PrismArchive -- c93 breadcrumb", ::GetModuleHandleW(nullptr))) {
    std::puts("SKIP: no window station");
    return 77;
  }
  window.SetItems(&fixture.items);
  window.SetRootLabel(L"pack.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  ::SetWindowPos(window.Hwnd(), HWND_TOP, 120, 80, 1280, 800, SWP_SHOWWINDOW);
  window.Show();
  ::SetForegroundWindow(window.Hwnd());
  ::Sleep(500);
  PumpUntilPainted(20);

  const std::wstring root(outDir.begin(), outDir.end());
  ::CreateDirectoryW(root.c_str(), nullptr);

  if (!EnterFolder(window, "docs") || !EnterFolder(window, "docs/sub")) {
    std::puts("SKIP: could not enter the fixture folder");
    return 77;
  }
  PumpUntilPainted(10);

  int x = 0;
  int y = 0;
  if (!CrumbCentre(window, 0, &x, &y)) {
    std::puts("SKIP: no crumb span");
    return 77;
  }
  HoverClientPoint(window, x, y);
  ::Sleep(250);
  if (!CaptureWindowPng(window.Hwnd(), root + L"\\c93-breadcrumb-hover.png",
                        "c93-breadcrumb-hover")) {
    return 1;
  }

  // Click the ROOT crumb through the production path, hover it again so the
  // feedback is visible in the same frame the list came back.
  int rx = 0;
  int ry = 0;
  if (!CrumbCentre(window, kRootLevel, &rx, &ry)) {
    std::puts("SKIP: no root crumb span");
    return 77;
  }
  window.ClickAt(rx, ry);
  PumpUntilPainted(20);
  if (window.Folder() != std::string()) {
    std::puts("FAIL: clicking the root crumb did not return to the archive root");
    return 1;
  }
  ::Sleep(150);
  if (!CaptureWindowPng(window.Hwnd(), root + L"\\c93-root-after-click.png",
                        "c93-root-after-click")) {
    return 1;
  }

  std::printf("C93 SCREENSHOTS WRITTEN\n");
  return 0;
}

}  // namespace

// ---- ASSERTION 1 ----------------------------------------------------------
// Registered first on purpose: under PRISM_C93_MUTANT_DEAD_CRUMB the marker
// must name THIS case, because this is the assertion the defect must break.
PRISM_TEST("c93.clicking_segment_zero_returns_to_the_archive_root") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 crumb to root"))) return;
  const int rootRows = window.ItemCount();
  if (!PRISM_CHECK_EQ(rootRows, 2)) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs/sub"))) return;
  PRISM_CHECK_EQ(window.ItemCount(), 1);

  int x = 0;
  int y = 0;
  if (!PRISM_CHECK(CrumbCentre(window, kRootLevel, &x, &y))) return;
  std::printf("    [crumb] segment0 x=%d y=%d nav=%d\n", x, y, window.NavigationEvents());
  if (!PRISM_CHECK(window.ClickAt(x, y))) return;

  // Both halves of the acceptance: the path AND the list really came back.
  PRISM_CHECK_EQ(window.Folder(), std::string());
  PRISM_CHECK_EQ(window.ItemCount(), rootRows);
  PRISM_CHECK_EQ(window.NavigationEvents(), 3);  // docs, docs/sub, then root
}

// ---- requirement 1, general case: segment k navigates to ITS path ---------
PRISM_TEST("c93.clicking_a_middle_crumb_navigates_to_that_folder") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 middle crumb"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs/sub"))) return;

  int x = 0;
  int y = 0;
  if (!PRISM_CHECK(CrumbCentre(window, 0, &x, &y))) return;  // "docs"
  if (!PRISM_CHECK(window.ClickAt(x, y))) return;
  PRISM_CHECK_EQ(window.Folder(), std::string("docs"));
  PRISM_CHECK_EQ(window.ItemCount(), 2);  // sub/ + b.txt
}

// ---- ASSERTION 2 ----------------------------------------------------------
PRISM_TEST("c93.clicking_the_tree_root_returns_to_the_archive_root") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 tree root"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs/sub"))) return;
  if (!PRISM_CHECK_EQ(window.SidebarRowPathForTest(0), std::string())) return;  // root row
  if (!PRISM_CHECK_EQ(window.SidebarFocusedPathForTest(), std::string("docs/sub"))) return;

  int left = 0, top = 0, right = 0, bottom = 0;
  if (!PRISM_CHECK(window.SidebarRowRectForTest(0, &left, &top, &right, &bottom))) return;
  if (!PRISM_CHECK(right > left && bottom > top)) return;
  std::printf("    [tree] root row box=(%d,%d)-(%d,%d)\n", left, top, right, bottom);

  window.ClickAt((left + right) / 2, (top + bottom) / 2);
  PRISM_CHECK_EQ(window.Folder(), std::string());
  PRISM_CHECK_EQ(window.ItemCount(), 2);
  // The tree highlight IS the current folder (requirement 2, second half).
  PRISM_CHECK_EQ(window.SidebarFocusedPathForTest(), std::string());
}

// ---- ASSERTION 3 ----------------------------------------------------------
PRISM_TEST("c93.clicking_the_current_crumb_is_a_no_op") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 current crumb"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  const std::string folderBefore = window.Folder();
  const int rowsBefore = window.ItemCount();
  const int navBefore = window.NavigationEvents();

  int x = 0;
  int y = 0;
  if (!PRISM_CHECK(CrumbCentre(window, 0, &x, &y))) return;  // "docs" is crumb 0
  if (!PRISM_CHECK(window.ClickAt(x, y))) return;

  PRISM_CHECK_EQ(window.Folder(), folderBefore);
  PRISM_CHECK_EQ(window.ItemCount(), rowsBefore);
  PRISM_CHECK_EQ(window.NavigationEvents(), navBefore);  // NO navigation event
}

// ---- requirement 1, feedback half ----------------------------------------
PRISM_TEST("c93.hover_feedback_is_the_crumb_span") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 hover"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs/sub"))) return;

  int left = 0;
  int right = 0;
  if (!PRISM_CHECK(window.CrumbSpanForTest(0, &left, &right))) return;
  const int y = (window.NavBarTop() + window.NavBarBottom()) / 2;
  const int x = (left + right) / 2;
  window.Dispatch(WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
  PRISM_CHECK_EQ(window.CrumbHoverLevel(), 0);
  PRISM_CHECK(window.HoveringCrumb());
  const prism::layout::Rect hover = window.CrumbHoverRect();
  PRISM_CHECK_EQ(hover.left, left);
  PRISM_CHECK_EQ(hover.right, right);  // exactly the text, never the bar
  PRISM_CHECK(hover.top >= window.NavBarTop());
  PRISM_CHECK(hover.bottom <= window.NavBarBottom());

  // A point on the pill but BEFORE the first crumb must not answer a crumb --
  // the "hit area covers the whole bar" defect would answer kRootLevel here.
  const int barLeft = window.NavBarTextLeft();
  window.Dispatch(WM_MOUSEMOVE, 0, MAKELPARAM(barLeft - 1, y));
  PRISM_CHECK_EQ(window.CrumbHoverLevel(), kNoLevel);
  PRISM_CHECK_EQ(window.CrumbHoverRect().Width(), 0);

  // Leaving the window clears it (no frozen highlight).
  window.Dispatch(WM_MOUSELEAVE, 0, 0);
  PRISM_CHECK_EQ(window.CrumbHoverLevel(), kNoLevel);
}

// ---- Backspace semantics unchanged ---------------------------------------
PRISM_TEST("c93.backspace_still_walks_up") {
  Fixture fixture;
  GalleryWindow window;
  if (!PRISM_CHECK(MakeWindow(window, fixture, L"c93 backspace"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs"))) return;
  if (!PRISM_CHECK(EnterFolder(window, "docs/sub"))) return;
  PRISM_CHECK(window.Back());
  PRISM_CHECK_EQ(window.Folder(), std::string("docs"));
  PRISM_CHECK(window.Back());
  PRISM_CHECK_EQ(window.Folder(), std::string());
  PRISM_CHECK(!window.Back());  // at the root: nothing, not a redraw
  PRISM_CHECK_EQ(window.Folder(), std::string());
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: CoInitializeEx failed");
    return 77;
  }
  if (argc > 1 && argv[1] != nullptr && argv[1][0] != '\0') {
    const int shot = CaptureNavigationScreens(argv[1]);
    ::CoUninitialize();
    return shot;
  }
#if defined(PRISM_C93_MUTANT_DEAD_CRUMB)
  std::puts("C93 MUTANT ACTIVE: no crumb pixel resolves to a level");
  prism::test::SetMutationVerdict("C93 MUTATION CAUGHT: dead_crumb",
                                  [] { return !prism::tree::kCrumbClickEnabled; });
#endif
#if defined(PRISM_C93_MUTANT_ROOT_NOOP)
  std::puts("C93 MUTANT ACTIVE: the tree root row answers nothing");
  prism::test::SetMutationVerdict(
      "C93 MUTATION CAUGHT: root_noop",
      [] { return prism::win::gallery::ActiveC93Mutation() == "root_noop"; });
#endif
#if defined(PRISM_C93_MUTANT_CRUMB_RESETS)
  std::puts("C93 MUTANT ACTIVE: the current crumb counts as a navigation");
  prism::test::SetMutationVerdict("C93 MUTATION CAUGHT: crumb_resets", [] {
    std::vector<ArchiveItem> catalog;
    ArchiveItem item;
    item.index = 0;
    item.path = "a";
    item.isDir = true;
    catalog.push_back(item);
    prism::tree::Navigator nav(&catalog);
    nav.GoTo("");
    return nav.NavigationCount() != 0;
  });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();
  if (rc != 0) return rc;
  std::puts("ALL C93 NAVIGATION CHECKS PASSED");
  return 0;
}
