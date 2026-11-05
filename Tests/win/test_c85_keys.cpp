// PrismArchive -- PrismWin
// C85: THE KEYBOARD AND THE ERROR SURFACE, measured with REAL keys and a REAL
// MSAA client (card C85, S3-6 + S3-3 + S3-6 后半).
//
// THE THREE DEFECTS THIS FILE PINS. All three were reproduced by hand against
// the shipped window before a line of this test existed:
//
//   1. Ctrl+Tab / Ctrl+Shift+Tab did nothing. Root cause: GalleryWindow's
//      HandleActionBarKey() claimed VK_TAB unconditionally ("return true"), so
//      HandleTabKey() -- which DOES implement both chords -- was unreachable.
//      The old tab suite (c60.tab_bar) missed it because it calls
//      DispatchKeyStroke(), i.e. it enters AFTER the band's refusal decision.
//      So this suite drives WM_KEYDOWN with a real thread key state, which is
//      the route a physical Ctrl+Tab takes.
//   2. Ctrl+W did nothing for the same reason's sibling: the close verb lives
//      in the same unreachable-or-unproven path. Asserted here through the
//      same real key route, and through the same RequestCloseTab the painted x
//      uses.
//   3. The tab strip published NO close entry to assistive technology: a UIA
//      walk stopped at "tab | Photos.7z", and the window's only "close" was the
//      title bar's button. Each tab now owns a child PUSHBUTTON whose default
//      action is CloseTab.
//
// WHAT IS ASSERTED, in the order a user meets it:
//   * Ctrl+Tab moves the ACTIVE TAB and the WINDOW TITLE follows (Photos.7z ->
//     Backup.7z); Ctrl+Shift+Tab moves back; a BARE Tab still walks the band's
//     ring (the band keeps the key it owns);
//   * Ctrl+W closes the active tab and the title falls back to the survivor;
//   * the MSAA tree publishes two close buttons in a two-tab window, and
//     accDoDefaultAction on one of them removes exactly that tab.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27), same source, one rule each:
//   PRISM_C85_MUTANT_BAND_EATS_CTRL_TAB  -> the band claims every Tab again
//                                          (arm: band_eats_ctrl_tab)
//   PRISM_C85_MUTANT_NO_CLOSE_NODE       -> the tab publishes no close entry
//                                          (arm: no_close_node)
// Both arms print their marker at the FIRST red assertion and exit 0, so the
// target's PASS_REGULAR_EXPRESSION is the verdict. No WILL_FAIL (D26); the arm
// names avoid clean/unarmed/control (D28).
#include <windows.h>

#include <oleacc.h>
#include <oleauto.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/A11yTree.hpp"
#include "../../Src/PrismWin/Accessible.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/TabBar.hpp"

using prism::win::gallery::GalleryWindow;

namespace {

constexpr int kSkipReturnCode = 77;

#if defined(PRISM_C85_MUTANT_BAND_EATS_CTRL_TAB)
constexpr const char* kMutation = "band_eats_ctrl_tab";
constexpr const char* kMutationBanner = "C85 MUTANT ACTIVE: band_eats_ctrl_tab";
#elif defined(PRISM_C85_MUTANT_NO_CLOSE_NODE)
constexpr const char* kMutation = "no_close_node";
constexpr const char* kMutationBanner = "C85 MUTANT ACTIVE: no_close_node";
#else
constexpr const char* kMutation = nullptr;
constexpr const char* kMutationBanner = nullptr;
#endif

// D3: the injection must PROVE it fired, by reading the same constant the
// product branches on (a stale macro leaves the mutant target RED, never green).
[[maybe_unused]] bool BandMutationFired() {
  // TRUE only when the band claims a Ctrl+Tab (the shipped defect). Read from
  // the SAME predicate the product branches on, so a stale macro stays RED
  // instead of letting the mutant claim CAUGHT from an unrelated failure.
  return !prism::win::gallery::GalleryWindow::TabIsChordForStripForTest();
}
[[maybe_unused]] bool CloseNodeMutationFired() {
  return prism::win::gallery::ActiveTabCloseMutation() == "no_close_node";
}

GalleryWindow* g_window = nullptr;
std::vector<prism::ArchiveItem> g_catalog;
std::string g_requested;  // the path the window asked the app to show

// ---------------------------------------------------------------- fixtures
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
  Add("docs/a.txt", false, 10);
  Add("photos", true, 0);
  Add("photos/p1.png", false, 20);
  Add("notes.txt", false, 30);
}

// Ctrl/Shift/Alt for the REAL WM_KEYDOWN path: the window reads GetKeyState, so
// the THREAD's keyboard state is what has to hold the chord (the same seam
// test_app_wiring uses; proven to work on the CI window station by test_keys).
bool SetModifiers(bool ctrl, bool shift) {
  BYTE state[256] = {};
  if (!GetKeyboardState(state)) return false;
  state[VK_CONTROL] = ctrl ? 0x80 : 0x00;
  state[VK_SHIFT] = shift ? 0x80 : 0x00;
  state[VK_MENU] = 0x00;
  return SetKeyboardState(state) != FALSE;
}

// One real key press: the thread key state, then the message WM_KEYDOWN.
void Press(UINT vk, bool ctrl, bool shift = false) {
  (void)SetModifiers(ctrl, shift);
  g_window->Dispatch(WM_KEYDOWN, static_cast<WPARAM>(vk), 0);
  (void)SetModifiers(false, false);
}

std::wstring Title() {
  wchar_t buffer[512]{};
  GetWindowTextW(g_window->Hwnd(), buffer, ARRAYSIZE(buffer));
  return std::wstring(buffer);
}

// The title follows the ACTIVE TAB, exactly as the app's tab handler does it in
// Main.cpp (SetWindowTextW after the archive switch). Asserting the window TITLE
// is asserting what the user reported, not an internal counter.
void FollowTitle(const std::string& path) {
  // The FILE NAME, exactly as Main.cpp's FileNameWide does it: the tab strips
  // the directory, so the title does too.
  std::string leaf = path;
  const size_t slash = leaf.find_last_of('/');
  if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
  std::wstring name;
  for (const char c : leaf) name.push_back(static_cast<wchar_t>(c));
  SetWindowTextW(g_window->Hwnd(), (L"PrismArchive -- " + name).c_str());
}

// ---------------------------------------------------------------- MSAA glue
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
  const HRESULT hr =
      ::AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, (void**)&acc);
  return FAILED(hr) ? nullptr : acc;
}

long ChildCount(IAccessible* acc) {
  long count = -1;
  if (acc == nullptr || FAILED(acc->get_accChildCount(&count))) return -1;
  return count;
}

std::wstring NameOf(IAccessible* acc, long id) {
  BSTR text = nullptr;
  if (acc == nullptr) return {};
  VARIANT child = ChildId(id);
  if (FAILED(acc->get_accName(child, &text)) || text == nullptr) return {};
  std::wstring out(text, ::SysStringLen(text));
  ::SysFreeString(text);
  return out;
}

long RoleOf(IAccessible* acc, long id) {
  VARIANT role;
  ::VariantInit(&role);
  VARIANT child = ChildId(id);
  if (acc == nullptr || FAILED(acc->get_accRole(child, &role))) return -1;
  return role.lVal;
}

IAccessible* ChildObject(IAccessible* parent, long id) {
  VARIANT child = ChildId(id);
  IDispatch* dispatch = nullptr;
  if (parent == nullptr) return nullptr;
  if (FAILED(parent->get_accChild(child, &dispatch)) || dispatch == nullptr) return nullptr;
  IAccessible* out = nullptr;
  dispatch->QueryInterface(IID_IAccessible, (void**)&out);
  dispatch->Release();
  return out;
}

// The tabs the client can see, with the close buttons under each of them.
struct TabView {
  long parentId = 0;            // id of the tab inside the WINDOW's accChildren
  std::wstring name;            // the tab's file name
  IAccessible* object = nullptr;  // the tab as its own object (owned)
  std::vector<long> closeIds;   // child ids that are buttons
  std::vector<std::wstring> closeNames;

  TabView() = default;
  // The object is OWNED. A raw pointer plus a destructor needs an explicit move
  // that nulls the source, or a vector reallocation releases it twice.
  TabView(const TabView&) = delete;
  TabView& operator=(const TabView&) = delete;
  TabView(TabView&& other) noexcept
      : parentId(other.parentId),
        name(std::move(other.name)),
        object(other.object),
        closeIds(std::move(other.closeIds)),
        closeNames(std::move(other.closeNames)) {
    other.object = nullptr;
  }
  TabView& operator=(TabView&& other) noexcept {
    if (this != &other) {
      if (object != nullptr) object->Release();
      parentId = other.parentId;
      name = std::move(other.name);
      object = other.object;
      closeIds = std::move(other.closeIds);
      closeNames = std::move(other.closeNames);
      other.object = nullptr;
    }
    return *this;
  }
  ~TabView() {
    if (object != nullptr) object->Release();
  }
};

std::vector<TabView> ReadTabs(IAccessible* root) {
  std::vector<TabView> tabs;
  const long children = ChildCount(root);
  for (long i = 1; i <= children; ++i) {
    if (RoleOf(root, i) != ROLE_SYSTEM_PAGETAB) continue;
    TabView tab;
    tab.parentId = i;
    tab.name = NameOf(root, i);
    tab.object = ChildObject(root, i);
    const long grandChildren = ChildCount(tab.object);
    for (long j = 1; j <= grandChildren; ++j) {
      if (RoleOf(tab.object, j) != ROLE_SYSTEM_PUSHBUTTON) continue;
      tab.closeIds.push_back(j);
      tab.closeNames.push_back(NameOf(tab.object, j));
    }
    tabs.push_back(std::move(tab));
  }
  return tabs;
}

std::wstring Wide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
  return out;
}

bool Contains(const std::wstring& haystack, const std::wstring& needle) {
  return haystack.find(needle) != std::wstring::npos;
}

// ---------------------------------------------------------------- the window
bool PrepareWindow(int tabCount) {
  g_catalog.clear();
  BuildCatalog();
  g_window = new GalleryWindow();
  if (!g_window->Create(L"PrismArchive -- Photos.7z", GetModuleHandleW(nullptr))) {
    return false;
  }
  g_window->SetItems(&g_catalog);
  g_window->SetRootLabel(L"Photos.7z");
  g_window->Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));
  g_window->SetTabHandler([](const std::string& path) {
    g_requested = path;
    FollowTitle(path);
  });
  if (tabCount >= 1) g_window->OpenTab("C:/c85/Photos.7z");
  if (tabCount >= 2) g_window->OpenTab("C:/c85/Backup.7z");
  if (tabCount >= 1) g_window->ActivateTab(0);
  g_requested.clear();
  return true;
}

void DestroyWindow() {
  if (g_window == nullptr) return;
  g_window->Dispatch(WM_DESTROY, 0, 0);
  delete g_window;
  g_window = nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Ctrl+Tab / Ctrl+Shift+Tab: the chords reach the STRIP, and the title
//    follows -- the exact sequence the card measured as dead.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 ctrl+tab cycles the tabs through the real WM_KEYDOWN route") {
  if (!PrepareWindow(2)) {
    PRISM_CHECK(false);
    return;
  }
  PRISM_CHECK_EQ(static_cast<int>(g_window->Tabs().size()), 2);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);
  PRISM_CHECK(Title() == std::wstring(L"PrismArchive -- Photos.7z"));

  Press(VK_TAB, /*ctrl=*/true);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);
  PRISM_CHECK_EQ(g_requested, std::string("C:/c85/Backup.7z"));
  PRISM_CHECK(Title() == std::wstring(L"PrismArchive -- Backup.7z"));

  Press(VK_TAB, /*ctrl=*/true, /*shift=*/true);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);
  PRISM_CHECK(Title() == std::wstring(L"PrismArchive -- Photos.7z"));

  // The BARE Tab is still the band's: the fix must not trade one dead key for
  // another. The ring moves off "no button focused", so the assertion is about
  // the band's own state, not about the tabs.
  const int beforeFocus = g_window->ActionBarFocus();
  Press(VK_TAB, /*ctrl=*/false);
  PRISM_CHECK(g_window->ActionBarFocus() != beforeFocus);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);  // the strip did NOT move

  DestroyWindow();
}

// ---------------------------------------------------------------------------
// 2. Ctrl+W closes the ACTIVE tab; the window falls back to the survivor.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 ctrl+w closes the active tab and the title falls back") {
  if (!PrepareWindow(2)) {
    PRISM_CHECK(false);
    return;
  }
  Press(VK_TAB, /*ctrl=*/true);  // Backup.7z is active now
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 1);

  Press('W', /*ctrl=*/true);
  PRISM_CHECK_EQ(static_cast<int>(g_window->Tabs().size()), 1);
  PRISM_CHECK_EQ(g_window->Tabs().activeIndex(), 0);
  PRISM_CHECK(Title() == std::wstring(L"PrismArchive -- Photos.7z"));
  PRISM_CHECK_EQ(g_window->Tabs().active()->path, std::string("C:/c85/Photos.7z"));

  DestroyWindow();
}

// ---------------------------------------------------------------------------
// 3. Every tab publishes a CLOSE button, and pressing it closes THAT tab.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 each tab exposes a close button a UIA client can invoke") {
  if (!PrepareWindow(2)) {
    PRISM_CHECK(false);
    return;
  }
  IAccessible* root = ClientAccessible(g_window->Hwnd());
  if (!PRISM_CHECK(root != nullptr)) {
    DestroyWindow();
    return;
  }
  std::vector<TabView> tabs = ReadTabs(root);
  PRISM_CHECK_EQ(static_cast<int>(tabs.size()), 2);
  if (tabs.size() == 2) {
    // Two tabs, two close entries -- the number the defect had at zero.
    int closeButtons = 0;
    for (const TabView& tab : tabs) closeButtons += static_cast<int>(tab.closeIds.size());
    PRISM_CHECK_EQ(closeButtons, 2);
    // The entry NAMES the tab it closes: "关闭 Photos.7z" / "关闭 Backup.7z".
    PRISM_CHECK(Contains(tabs[0].closeNames.empty() ? std::wstring() : tabs[0].closeNames[0],
                         L"Photos.7z"));
    PRISM_CHECK(Contains(tabs[1].closeNames.empty() ? std::wstring() : tabs[1].closeNames[0],
                         L"Backup.7z"));
    // Its default action is non-empty (a button with no verb is the "found but
    // unusable" shape C80 measured for the search field).
    if (!tabs[0].closeIds.empty()) {
      VARIANT child = ChildId(tabs[0].closeIds[0]);
      BSTR verb = nullptr;
      PRISM_CHECK(SUCCEEDED(tabs[0].object->get_accDefaultAction(child, &verb)));
      PRISM_CHECK(verb != nullptr && ::SysStringLen(verb) > 0);
      if (verb != nullptr) ::SysFreeString(verb);
      // Invoke it: the tab goes away and the OTHER one stays active.
      PRISM_CHECK(SUCCEEDED(tabs[0].object->accDoDefaultAction(child)));
      PRISM_CHECK_EQ(static_cast<int>(g_window->Tabs().size()), 1);
      PRISM_CHECK_EQ(g_window->Tabs().active()->path, std::string("C:/c85/Backup.7z"));
      PRISM_CHECK(Title() == std::wstring(L"PrismArchive -- Backup.7z"));
    }
  }
  root->Release();
  DestroyWindow();
}

int main(int argc, char** argv) {
  if (kMutationBanner != nullptr) std::printf("%s\n", kMutationBanner);
  if (kMutation != nullptr) {
    const std::string marker = std::string("C85 MUTATION CAUGHT: ") + kMutation;
    if (std::string(kMutation) == "band_eats_ctrl_tab") {
      prism::test::SetMutationVerdict(marker.c_str(), [] { return BandMutationFired(); });
    } else {
      prism::test::SetMutationVerdict(marker.c_str(), [] { return CloseNodeMutationFired(); });
    }
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("C85-KEYS SKIP: no COM on this window station");
    return kSkipReturnCode;
  }
  // A window station that cannot create the HWND is a visible SKIP, never a
  // fake green (the precedent C60/C80 set).
  GalleryWindow probe;
  if (!probe.Create(L"PrismArchive C85 probe", GetModuleHandleW(nullptr))) {
    std::puts("C85-KEYS SKIP: no interactive window station");
    CoUninitialize();
    return kSkipReturnCode;
  }
  probe.Dispatch(WM_DESTROY, 0, 0);

  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::printf("C85 MUTATION NOT CAUGHT: %s stayed green\n", kMutation);
      return 1;
    }
    return 0;
  }
  if (rc != 0) return rc;
  std::puts("ALL C85 KEY CHECKS PASSED");
  return 0;
}
