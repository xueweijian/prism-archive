// PrismArchive -- PrismWin
// C86 (S3-2 + S3-11 + S3-4): the saved window, the status band a client can
// read, and the number next to a folder name.
//
// THE THREE MEASURED DEFECTS this file pins down, each on a REAL window:
//   1. S3-2. %LOCALAPPDATA%\PrismArchive\settings.json held
//      "window": {"width": 1536, "height": 480, "listMode": true, ...} and no
//      origin at all: a user who dragged the window to (300,150) got the
//      CW_USEDEFAULT cascade back. C66 fixed which SIZE is written (the visible
//      frame, clamped); the position never left the process.
//   2. S3-11. A UIA client saw ONE element for the whole status band --
//      `status | 共 3 项` -- so 未加密 / 方法未知 / 字典未知 / 条目 / 选中 /
//      压缩率 were painted and unreachable.
//   3. S3-4. Inside a two-entry folder the breadcrumb still read 共 16 项 (the
//      archive total, written once by the app) while the band said 条目 2.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27 + D28). The SAME source is built
// for each arm; each binary arms mini_test's verdict with a predicate that
// proves the injection really fired, prints "C86 MUTATION CAUGHT: <arm>" at the
// FIRST red assertion and exits 0. No WILL_FAIL anywhere.
//   no_position           -- the app writes width/height only (the pre-fix file)
//   no_status_segments    -- the band is one element again
//   maximized_as_normal   -- a maximized window's rectangle is saved as normal
//   archive_count         -- the breadcrumb shows the archive total again
//
// D29: every wait is bounded and also watches the thing it waits for.
#include <windows.h>

#include <oleacc.h>
#include <oleauto.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cwchar>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/A11yTree.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/Accessible.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/WindowFrame.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

using prism::layout::Rect;
using prism::layout::SavedWindow;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
bool g_noWindowStation = false;

// What each arm's predicate watches: set by the case that is supposed to fail.
bool g_documentLostPosition = false;   // no_position
bool g_bandPublishedNoSegments = false;  // no_status_segments
bool g_maximizedSavedAsNormal = false;   // maximized_as_normal
bool g_breadcrumbDisagreed = false;      // archive_count

#if defined(PRISM_C86_MUTANT_NO_POSITION)
constexpr const char* kMutation = "no_position";
#elif defined(PRISM_C86_MUTANT_NO_STATUS_SEGMENTS)
constexpr const char* kMutation = "no_status_segments";
#elif defined(PRISM_C86_MUTANT_MAXIMIZED_AS_NORMAL)
constexpr const char* kMutation = "maximized_as_normal";
#elif defined(PRISM_C86_MUTANT_ARCHIVE_COUNT)
constexpr const char* kMutation = "archive_count";
#else
constexpr const char* kMutation = nullptr;
#endif

bool MakeWindow(GalleryWindow* window, const wchar_t* title) {
  if (window->Create(title, GetModuleHandleW(nullptr))) return true;
  std::puts("  [skip] no window station");
  g_noWindowStation = true;
  return false;
}

// 16 catalog entries: a two-entry folder plus thirteen files at the root
// (14 root rows). The numbers are the card's: "共 16 项" for the archive,
// "2" for the folder the user is in.
std::vector<prism::ArchiveItem> g_catalog;

void Add(const char* path, bool dir, std::uint64_t size) {
  prism::ArchiveItem item;
  item.index = static_cast<std::uint32_t>(g_catalog.size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  g_catalog.push_back(item);
}

void BuildCatalog() {
  Add("docs", true, 0);
  for (int i = 1; i <= 13; ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "item%02d.txt", i);
    Add(name, false, static_cast<std::uint64_t>(100 + i));
  }
  Add("docs/readme.txt", false, 10);
  Add("docs/guide.txt", false, 20);
}

// ------------------------------------------------------------- text helpers
std::string ToUtf8(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0,
                                           nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        out.data(), needed, nullptr, nullptr);
  return out;
}

// --------------------------------------------------------------- MSAA glue
VARIANT ChildId(long id) {
  VARIANT v;
  ::VariantInit(&v);
  v.vt = VT_I4;
  v.lVal = id;
  return v;
}

IAccessible* ClientAccessible(HWND hwnd) {
  if (hwnd == nullptr) return nullptr;
  IAccessible* acc = nullptr;
  if (FAILED(::AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible,
                                          (void**)&acc))) {
    return nullptr;
  }
  return acc;
}

long ChildCount(IAccessible* acc) {
  long count = -1;
  if (acc == nullptr || FAILED(acc->get_accChildCount(&count))) return -1;
  return count;
}

long RoleOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  VARIANT role;
  ::VariantInit(&role);
  if (acc == nullptr || FAILED(acc->get_accRole(child, &role))) return -1;
  return role.lVal;
}

std::wstring NameOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  BSTR text = nullptr;
  if (acc == nullptr || FAILED(acc->get_accName(child, &text)) || text == nullptr) return {};
  std::wstring out(text, ::SysStringLen(text));
  ::SysFreeString(text);
  return out;
}

IAccessible* ChildObject(IAccessible* parent, long id) {
  if (parent == nullptr) return nullptr;
  VARIANT child = ChildId(id);
  IDispatch* dispatch = nullptr;
  if (FAILED(parent->get_accChild(child, &dispatch)) || dispatch == nullptr) return nullptr;
  IAccessible* childAcc = nullptr;
  dispatch->QueryInterface(IID_IAccessible, (void**)&childAcc);
  dispatch->Release();
  return childAcc;
}

// The status band's element, by ROLE: its name is a transient sentence and a
// future wording change must not turn this suite red for the wrong reason.
IAccessible* StatusBarObject(IAccessible* root) {
  for (long i = 1; i <= ChildCount(root); ++i) {
    if (RoleOf(root, i) != ROLE_SYSTEM_STATUSBAR) continue;
    return ChildObject(root, i);
  }
  return nullptr;
}

// Every child a client can read out of the band, in tree order (= visual order).
std::vector<std::wstring> StatusChildNames(IAccessible* status) {
  std::vector<std::wstring> out;
  for (long i = 1; i <= ChildCount(status); ++i) out.push_back(NameOf(status, i));
  return out;
}

// The 条目 / 选中 / 压缩率 texts, whichever arm is asking.
std::wstring StatusChildStartingWith(IAccessible* status, const wchar_t* prefix) {
  const std::size_t length = std::wcslen(prefix);
  for (const std::wstring& name : StatusChildNames(status)) {
    if (name.size() >= length && name.compare(0, length, prefix) == 0) return name;
  }
  return {};
}

int TrailingNumber(const std::wstring& text) {
  int value = 0;
  bool any = false;
  for (const wchar_t unit : text) {
    if (unit >= L'0' && unit <= L'9') {
      value = value * 10 + static_cast<int>(unit - L'0');
      any = true;
    } else if (any) {
      break;
    }
  }
  return any ? value : -1;
}

// A rectangle the runner's desktop can actually hold, so the position assertions
// never depend on the size of the screen the test happens to run on (D21).
Rect FitFrame(const Rect& work, int wantWidth, int wantHeight) {
  const int width = (std::min)(wantWidth, (std::max)(prism::layout::kMinWindowWidth,
                                                     work.Width() - 40));
  const int height = (std::min)(wantHeight, (std::max)(prism::layout::kMinWindowHeight,
                                                       work.Height() - 40));
  const int left = work.left + (std::max)(0, (work.Width() - width) / 2);
  const int top = work.top + (std::max)(0, (work.Height() - height) / 2);
  return Rect{left, top, left + width, top + height};
}

int Distance(int a, int b) { return a > b ? a - b : b - a; }

// D29: a bounded wait for a state the system changes on its own (a shown window
// becoming zoomed). 20 ms steps and a hard deadline -- never a fixed Sleep, and
// the caller still asserts on the result.
template <class Predicate>
bool WaitUntil(Predicate done, int timeoutMs = 2000) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  for (;;) {
    if (done()) return true;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("SKIP: CoInitializeEx failed (this window station has no COM)");
    return kSkipReturnCode;
  }
  BuildCatalog();

  if (kMutation != nullptr) {
    std::printf("C86 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C86 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
      if (std::strcmp(kMutation, "no_position") == 0) {
        return prism::win::gallery::ActiveGalleryMutation() == "no_position" &&
               g_documentLostPosition;
      }
      if (std::strcmp(kMutation, "no_status_segments") == 0) {
        return prism::win::gallery::ActiveGalleryMutation() == "no_status_segments" &&
               g_bandPublishedNoSegments;
      }
      if (std::strcmp(kMutation, "maximized_as_normal") == 0) {
        return prism::win::gallery::ActiveGalleryMutation() == "maximized_as_normal" &&
               g_maximizedSavedAsNormal;
      }
      return prism::win::gallery::ActiveGalleryMutation() == "archive_count" &&
             g_breadcrumbDisagreed;
    });
  }

  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noWindowStation) {
    std::puts("C86 SKIPPED: no window station");
    ::CoUninitialize();
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL WINDOW-STATE CHECKS PASSED");
  ::CoUninitialize();
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE DOCUMENT. The app's own save/load pair, through a real settings
//    document: an origin is written and read back, and a pre-C86 file (a size
//    only) is still honoured as a size only.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 the settings document carries the window origin and its mode") {
  prism::settings::SettingsStore store;
  prism::app::WindowState state;
  state.width = 1200;
  state.height = 750;
  state.x = 300;
  state.y = 150;
  state.hasPosition = true;
  state.maximized = false;
  prism::app::SaveWindowState(store, state);

  const std::string document = store.ToJson();
  std::printf("  [document] %s\n", document.c_str());
  // The reported file held width/height and no origin: this is that assertion,
  // stated against the bytes the app writes.
  g_documentLostPosition = document.find("\"x\"") == std::string::npos ||
                           document.find("\"y\"") == std::string::npos;
  PRISM_CHECK(!g_documentLostPosition);
  PRISM_CHECK(document.find("\"width\": 1200") != std::string::npos);
  PRISM_CHECK(document.find("\"height\": 750") != std::string::npos);

  prism::settings::SettingsStore reloaded;
  reloaded.Load(document);
  const prism::app::WindowState back = prism::app::LoadWindowState(reloaded);
  PRISM_CHECK(back.hasPosition);
  PRISM_CHECK_EQ(back.x, 300);
  PRISM_CHECK_EQ(back.y, 150);
  PRISM_CHECK_EQ(back.width, 1200);
  PRISM_CHECK_EQ(back.height, 750);
  PRISM_CHECK(!back.maximized);

  // A maximized CLOSE stores the normal frame plus the mode, never the mode's
  // rectangle as a frame.
  prism::app::WindowState zoomed;
  zoomed.width = 1000;
  zoomed.height = 700;
  zoomed.x = 40;
  zoomed.y = 40;
  zoomed.hasPosition = true;
  zoomed.maximized = true;
  prism::app::SaveWindowState(store, zoomed);
  prism::settings::SettingsStore reloadedZoom;
  reloadedZoom.Load(store.ToJson());
  const prism::app::WindowState zoomBack = prism::app::LoadWindowState(reloadedZoom);
  PRISM_CHECK(zoomBack.maximized);
  PRISM_CHECK_EQ(zoomBack.width, 1000);
  PRISM_CHECK_EQ(zoomBack.height, 700);
}

PRISM_TEST("c86 a pre-C86 document (size only) keeps the window's own place") {
  prism::settings::SettingsStore store;
  store.Load(R"json({"schemaVersion": 3, "window": {"width": 900, "height": 600}})json");
  const prism::app::WindowState back = prism::app::LoadWindowState(store);
  PRISM_CHECK_EQ(back.width, 900);
  PRISM_CHECK_EQ(back.height, 600);
  // No origin in the file means "no opinion", NOT (0,0): defaulting to the
  // corner would move every existing user's window on the first launch.
  PRISM_CHECK(!back.hasPosition);
}

// ---------------------------------------------------------------------------
// 2. MOVE -> CLOSE -> OPEN on a real HWND: <= 2 px.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 a moved window closes and reopens where the user left it (<= 2 px)") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c86 move")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const Rect target = FitFrame(work, 1200, 750);
  SavedWindow placed;
  placed.visible = target;
  placed.hasPosition = true;
  const prism::win::frame::Restore first = window.RestoreFrame(placed);
  if (!PRISM_CHECK(first.ok)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  // The frame the policy PREDICTS for this saved window on THIS desktop, clamp
  // included. The assertion is relative to that plan, never to a pixel that only
  // exists on a 1536x816 screen (D21): a 1024x720 CI runner and this desktop must
  // both be able to say "the origin was honoured".
  const Rect expected = prism::layout::PlanWindowRestore(placed, work).frame;
  std::printf("  [moved] target=%d,%d %dx%d expected=%d,%d %dx%d visible=%d,%d %dx%d\n",
              target.left, target.top, target.Width(), target.Height(), expected.left,
              expected.top, expected.Width(), expected.Height(), first.visible.left,
              first.visible.top, first.visible.Width(), first.visible.Height());
  // The window really is where the policy says (otherwise the round trip below
  // would prove nothing about the origin).
  PRISM_CHECK(Distance(first.visible.left, expected.left) <= 2);
  PRISM_CHECK(Distance(first.visible.top, expected.top) <= 2);
  PRISM_CHECK(Distance(first.visible.Width(), expected.Width()) <= 2);

  // CLOSE: the only moment the frame still exists.
  window.Dispatch(WM_CLOSE, 0, 0);
  const SavedWindow closed = window.ClosedWindow();
  std::printf("  [closed] %d,%d %dx%d max=%d hasPos=%d\n", closed.visible.left,
              closed.visible.top, closed.visible.Width(), closed.visible.Height(),
              static_cast<int>(closed.maximized), static_cast<int>(closed.hasPosition));
  PRISM_CHECK(closed.hasPosition);
  PRISM_CHECK(Distance(closed.visible.left, first.visible.left) <= 2);
  PRISM_CHECK(Distance(closed.visible.top, first.visible.top) <= 2);
  PRISM_CHECK(Distance(closed.visible.Width(), first.visible.Width()) <= 2);
  PRISM_CHECK(Distance(closed.visible.Height(), first.visible.Height()) <= 2);

  // OPEN: a NEW window, restored from exactly what the close produced.
  GalleryWindow reopened;
  if (!MakeWindow(&reopened, L"PrismArchive c86 reopen")) return;
  reopened.SetItems(nullptr);
  const prism::win::frame::Restore again = reopened.RestoreFrame(closed);
  if (!PRISM_CHECK(again.ok)) {
    reopened.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  std::printf("  [reopened] %d,%d %dx%d\n", again.visible.left, again.visible.top,
              again.visible.Width(), again.visible.Height());
  // THE CARD'S NUMBER: <= 2 px on both axes.
  PRISM_CHECK(Distance(again.visible.left, closed.visible.left) <= 2);
  PRISM_CHECK(Distance(again.visible.top, closed.visible.top) <= 2);
  PRISM_CHECK(Distance(again.visible.Width(), closed.visible.Width()) <= 2);
  PRISM_CHECK(Distance(again.visible.Height(), closed.visible.Height()) <= 2);
  reopened.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 3. MAXIMIZED: the saved rectangle is the NORMAL one, and the mode rides
//    beside it. The historical trap is a 1552x832 "normal" frame.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 a maximized window saves its normal frame plus a mode, not the work area") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c86 maximize")) return;
  window.SetItems(nullptr);
  const Rect work = prism::win::frame::WorkAreaOfWindow(window.Hwnd());
  if (!PRISM_CHECK(!work.Empty())) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const Rect normal = FitFrame(work, 1000, 700);
  SavedWindow placed;
  placed.visible = normal;
  placed.hasPosition = true;
  const prism::win::frame::Restore first = window.RestoreFrame(placed);
  if (!PRISM_CHECK(first.ok)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const Rect normalVisible = first.visible;
  ::ShowWindow(window.Hwnd(), SW_MAXIMIZE);
  ::UpdateWindow(window.Hwnd());
  // D29: maximizing is the system's message to deliver; poll for the state with
  // a hard deadline instead of assuming one turn of the queue is enough.
  if (!PRISM_CHECK(WaitUntil([&] { return ::IsZoomed(window.Hwnd()) != FALSE; }))) {
    std::printf("  [max] the window never became zoomed (IsZoomed=0 after 2 s)\n");
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  window.Dispatch(WM_CLOSE, 0, 0);
  const SavedWindow closed = window.ClosedWindow();
  std::printf("  [max] normal=%d,%d %dx%d closed=%d,%d %dx%d max=%d work=%dx%d\n",
              normalVisible.left, normalVisible.top, normalVisible.Width(),
              normalVisible.Height(), closed.visible.left, closed.visible.top,
              closed.visible.Width(), closed.visible.Height(),
              static_cast<int>(closed.maximized), work.Width(), work.Height());
  // (a) the mode is recorded at all,
  PRISM_CHECK(closed.maximized);
  // (b) the frame written is the NORMAL rectangle -- the one un-maximizing
  //     returns to -- so it is not the work area and it fits the screen,
  PRISM_CHECK(prism::layout::FrameFitsWorkArea(closed.visible, work));
  PRISM_CHECK(Distance(closed.visible.Width(), normalVisible.Width()) <= 2);
  PRISM_CHECK(Distance(closed.visible.Height(), normalVisible.Height()) <= 2);
  PRISM_CHECK(Distance(closed.visible.left, normalVisible.left) <= 2);
  g_maximizedSavedAsNormal = !closed.maximized ||
                             Distance(closed.visible.Width(), work.Width()) <= 2;
  PRISM_CHECK(!g_maximizedSavedAsNormal);

  // (c) reopening re-applies the mode, and un-maximizing lands on the saved
  //     normal frame rather than on a default.
  GalleryWindow reopened;
  if (!MakeWindow(&reopened, L"PrismArchive c86 remax")) return;
  reopened.SetItems(nullptr);
  const prism::win::frame::Restore again = reopened.RestoreFrame(closed);
  if (!PRISM_CHECK(again.ok)) {
    reopened.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  PRISM_CHECK(again.maximized);
  PRISM_CHECK(WaitUntil([&] { return ::IsZoomed(reopened.Hwnd()) != FALSE; }));
  ::ShowWindow(reopened.Hwnd(), SW_RESTORE);
  ::UpdateWindow(reopened.Hwnd());
  // D29: wait (bounded) for the restore to settle rather than sampling one frame.
  WaitUntil([&] { return ::IsZoomed(reopened.Hwnd()) == FALSE; }, 1000);
  const Rect restoredFrame = prism::win::frame::Measure(reopened.Hwnd()).visible;
  std::printf("  [unmax] %d,%d %dx%d\n", restoredFrame.left, restoredFrame.top,
              restoredFrame.Width(), restoredFrame.Height());
  PRISM_CHECK(Distance(restoredFrame.left, closed.visible.left) <= 2);
  PRISM_CHECK(Distance(restoredFrame.top, closed.visible.top) <= 2);
  PRISM_CHECK(Distance(restoredFrame.Width(), closed.visible.Width()) <= 2);
  PRISM_CHECK(Distance(restoredFrame.Height(), closed.visible.Height()) <= 2);
  reopened.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 4. THE STATUS BAND, through the SAME MSAA surface a UIA client walks
//    (S3-11). Every painted segment is its own element, in the band's order.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 the status band publishes every segment in visual order") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c86 band")) return;
  window.SetRootLabel(L"c86.7z");
  window.SetItems(&g_catalog);
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1600, 900));
  // The app's own facts for an ordinary archive: no encryption, no method, no
  // dictionary. Anything else would be a different claim than the card's.
  prism::sidebar::ArchiveInfo info;
  info.encryption = prism::sidebar::Encryption::None;
  window.SetArchiveInfo(info);

  IAccessible* root = ClientAccessible(window.Hwnd());
  if (!PRISM_CHECK(root != nullptr)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  IAccessible* status = StatusBarObject(root);
  if (!PRISM_CHECK(status != nullptr)) {
    root->Release();
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const std::vector<std::wstring> names = StatusChildNames(status);
  g_bandPublishedNoSegments = names.empty();
  std::printf("  [band] %zu segments:\n", names.size());
  for (const std::wstring& name : names) std::printf("    - %s\n", ToUtf8(name).c_str());
  // THE BAND'S LIST, in the order it paints it. C128: three NUMBERS -- the
  // current folder's files and folders and the archive's packed size. (The six
  // words this case used to read -- 未加密 / 方法未知 / 字典未知 / 条目 / 选中 /
  // 压缩率 -- are on the info card now.)
  if (PRISM_CHECK_EQ(names.size(), static_cast<std::size_t>(3))) {
    PRISM_CHECK(names[0] == std::wstring(L"文件: 13"));   // the ROOT rows
    PRISM_CHECK(names[1] == std::wstring(L"文件夹: 1"));
    // This fixture never sets packSize, so the catalog cannot answer the size:
    // the band says "--" instead of inventing "0 B".
    PRISM_CHECK(names[2] == std::wstring(L"压缩包大小: --"));
  }
  // The published rectangles are the painted ones, left to right.
  long previousRight = 0;
  bool haveRect = false;
  for (long i = 1; i <= ChildCount(status); ++i) {
    VARIANT child = ChildId(i);
    long left = 0, top = 0, width = 0, height = 0;
    if (FAILED(status->accLocation(&left, &top, &width, &height, child))) continue;
    std::printf("    [rect] %ld,%ld %ldx%ld\n", left, top, width, height);
    PRISM_CHECK(width > 0);
    PRISM_CHECK(height > 0);
    if (haveRect) PRISM_CHECK(left >= previousRight);
    previousRight = left + width;
    haveRect = true;
  }
  status->Release();
  root->Release();
  window.Dispatch(WM_DESTROY, 0, 0);
}

PRISM_TEST("c86 the band's numbers are the window's facts (a selection does not move them)") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c86 select")) return;
  window.SetRootLabel(L"c86.7z");
  window.SetItems(&g_catalog);
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1600, 900));

  // C128: the band publishes 文件 / 文件夹 / 压缩包大小 -- the 选中 slot this case
  // used to read is gone (a status bar counts the folder, not the cursor). The
  // property it existed to prove survives in the shape the band has now: what a
  // client hears is the WINDOW's own count, and selecting a row -- a fact about
  // the cursor -- does not move any of the three numbers.
  IAccessible* root = ClientAccessible(window.Hwnd());
  if (!PRISM_CHECK(root != nullptr)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  IAccessible* status = StatusBarObject(root);
  if (PRISM_CHECK(status != nullptr)) {
    const std::wstring filesBefore = StatusChildStartingWith(status, L"文件");
    const std::wstring foldersBefore = StatusChildStartingWith(status, L"文件夹");
    PRISM_CHECK(filesBefore == std::wstring(L"文件: 13"));  // the root's files
    PRISM_CHECK(foldersBefore == std::wstring(L"文件夹: 1"));
    status->Release();
  }
  root->Release();

  window.SetSelection(3);
  root = ClientAccessible(window.Hwnd());
  if (PRISM_CHECK(root != nullptr)) {
    if (IAccessible* after = StatusBarObject(root)) {
      PRISM_CHECK(StatusChildStartingWith(after, L"文件") == std::wstring(L"文件: 13"));
      PRISM_CHECK(StatusChildStartingWith(after, L"文件夹") == std::wstring(L"文件夹: 1"));
      after->Release();
    }
    root->Release();
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// 5. THE BREADCRUMB COUNT (S3-4). Inside a two-entry folder the crumb and the
//    band must name the same number.
// ---------------------------------------------------------------------------
PRISM_TEST("c86 inside a two-entry folder the breadcrumb and the band agree") {
  GalleryWindow window;
  if (!MakeWindow(&window, L"PrismArchive c86 crumb")) return;
  window.SetRootLabel(L"c86.7z");
  window.SetItems(&g_catalog);
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1600, 900));
  // Drill in the way the USER does (Enter on the folder row), through the same
  // public door a double-click uses.
  int docsRow = -1;
  for (std::size_t i = 0; i < window.Items().size(); ++i) {
    if (window.Items()[i].path == "docs") docsRow = static_cast<int>(i);
  }
  if (!PRISM_CHECK(docsRow >= 0)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  if (!PRISM_CHECK(window.ActivateRow(docsRow))) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  const std::wstring crumb = window.BreadcrumbText();
  std::printf("  [crumb] %s (folder rows=%d, archive=%zu)\n", ToUtf8(crumb).c_str(),
              window.ItemCount(), g_catalog.size());
  PRISM_CHECK_EQ(window.ItemCount(), 2);
  // THE DEFECT: "共 16 项" next to a folder that holds two rows.
  g_breadcrumbDisagreed = crumb.find(L"共 16 项（") == std::wstring::npos &&
                          crumb.find(L"本目录 2 项") == std::wstring::npos;
  PRISM_CHECK(crumb.find(L"本目录 2 项") != std::wstring::npos);
  PRISM_CHECK(crumb.find(L"共 16 项") != std::wstring::npos);  // the total survives

  // The SAME number, read out of the band's FILE slot (C128): the band counts
  // the folder's rows -- 2 files, 0 folders here -- and the crumb says how many
  // the folder holds. One number on two surfaces, which is the whole point.
  IAccessible* root = ClientAccessible(window.Hwnd());
  if (!PRISM_CHECK(root != nullptr)) {
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  if (IAccessible* status = StatusBarObject(root)) {
    const std::wstring files = StatusChildStartingWith(status, L"文件");
    const std::wstring folders = StatusChildStartingWith(status, L"文件夹");
    std::printf("  [band] %s | %s\n", ToUtf8(files).c_str(), ToUtf8(folders).c_str());
    PRISM_CHECK(files == std::wstring(L"文件: 2"));
    PRISM_CHECK(folders == std::wstring(L"文件夹: 0"));
    PRISM_CHECK_EQ(TrailingNumber(files), window.ItemCount());
    status->Release();
  } else {
    PRISM_CHECK(false);
  }
  root->Release();
  window.Dispatch(WM_DESTROY, 0, 0);
}
