// PrismArchive -- PrismWin
// C80: THE CLIENT AREA MADE VISIBLE -- WM_GETOBJECT -> IAccessible (MSAA).
//
// THE MEASURED DEFECT this suite closes: a UIA client against the shipped
// window saw NINE elements -- the OS title bar and the menu bar. The eight
// action buttons, the tab strip, every row, the sidebar tree and the status
// line were invisible because they are D2D paint, not child HWNDs, and the
// window had no WM_GETOBJECT handler.
//
// WHAT IS ASSERTED (each one is a claim about what a screen reader reads):
//   1. AccessibleObjectFromWindow(OBJID_CLIENT) -- i.e. the REAL WM_GETOBJECT
//      path -- returns an IAccessible, and walking it (get_accChildCount ->
//      get_accChild -> ...) reaches at least 20 elements, the same number the
//      window itself publishes;
//   2. the eight action buttons carry the ACTIVE language's words, the MSAA
//      push-button role, and the band's enable matrix as UNAVAILABLE/available;
//   3. the open archive is a PAGE TAB named after the file, SELECTED;
//   4. the list is a LIST whose rows are named after the entries, and
//      accSelect(TAKESELECTION) changes the WINDOW's selection (not a copy);
//   5. accDoDefaultAction on 搜索 runs the same path a click runs, and the
//      search field appears in the tree;
//   6. accFocus and accHitTest name elements the client can find again;
//   7. a window with NO archive publishes an empty list and does not crash;
//   8. after the window is destroyed, a held IAccessible answers an empty tree
//      and accLocation fails -- no use-after-free.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C80_MUTANT_NO_GETOBJECT makes
// WM_GETOBJECT answer 0, which is the pre-fix behaviour; the element count
// assertion MUST go red. The binary arms mini_test's mutation verdict with a
// RUNTIME predicate that proves both that the macro compiled in AND that the
// window really answers nothing, prints "C80 MUTATION CAUGHT: no_getobject" at
// the FIRST failed assertion and exits 0. No WILL_FAIL anywhere (D26).
//
// Verdict shape (D1): the stdout marker is a conjunction with exit 0; a machine
// without a window station prints why and exits 77 (SKIP_RETURN_CODE).
#include <windows.h>

#include <oleacc.h>
#include <oleauto.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/A11yTree.hpp"
#include "../../Src/PrismWin/Accessible.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using prism::win::gallery::GalleryWindow;

namespace {

// ---------------------------------------------------------------- mutants
#if defined(PRISM_C80_MUTANT_NO_GETOBJECT)
// C80 MUTANT ACTIVE: no_getobject -- WM_GETOBJECT answers 0, so the client area
// is unreachable again. The banner sits inside the guard (CONTRACTS D28).
constexpr const char* kMutation = "no_getobject";
constexpr const char* kMutationBanner = "C80 MUTANT ACTIVE: no_getobject";
#elif defined(PRISM_C80_MUTANT_READONLY_SEARCH)
// C80 MUTANT ACTIVE: readonly_search -- the field is visible and focusable but
// put_accValue answers E_NOTIMPL, which is exactly the state a UIA driver
// reports as "POLICY_DENIED: supported_action".
constexpr const char* kMutation = "readonly_search";
constexpr const char* kMutationBanner = "C80 MUTANT ACTIVE: readonly_search";
#else
constexpr const char* kMutation = nullptr;
constexpr const char* kMutationBanner = nullptr;
#endif

constexpr int kSkipReturnCode = 77;

GalleryWindow* g_window = nullptr;
std::vector<prism::ArchiveItem> g_catalog;

// ---------------------------------------------------------------- fixtures
void Add(const char* path, bool dir, std::uint64_t size) {
  prism::ArchiveItem item;
  item.index = static_cast<std::uint32_t>(g_catalog.size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  g_catalog.push_back(item);
}

// 13 rows in the archive root (one folder + twelve files) and two sidebar tree
// rows, so "at least 20 elements" is a statement about the TREE, not about a
// single convenient child.
void BuildCatalog() {
  Add("docs", true, 0);
  Add("docs/readme.txt", false, 10);
  Add("docs/guide.txt", false, 20);
  for (int i = 1; i <= 12; ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "item%02d.txt", i);
    Add(name, false, static_cast<std::uint64_t>(100 + i));
  }
}

// ---------------------------------------------------------------- MSAA glue
std::wstring ToWide(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(),
                        needed);
  return out;
}

std::string ToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(),
                        needed, nullptr, nullptr);
  return out;
}

VARIANT ChildId(long id) {
  VARIANT v;
  ::VariantInit(&v);
  v.vt = VT_I4;
  v.lVal = id;
  return v;
}

// The window's accessible object through the REAL message: this is exactly what
// UIA's MSAA proxy does, so the test exercises the shipped path (D1's object
// under test is the product, not a mock).
IAccessible* ClientAccessible(HWND hwnd) {
  if (hwnd == nullptr) return nullptr;
  IAccessible* acc = nullptr;
  const HRESULT hr =
      ::AccessibleObjectFromWindow(hwnd, OBJID_CLIENT, IID_IAccessible, (void**)&acc);
  if (FAILED(hr)) return nullptr;
  return acc;
}

long ChildCount(IAccessible* acc) {
  long count = -1;
  if (acc == nullptr || FAILED(acc->get_accChildCount(&count))) return -1;
  return count;
}

std::wstring NameOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  BSTR text = nullptr;
  if (acc == nullptr || FAILED(acc->get_accName(child, &text)) || text == nullptr) return {};
  std::wstring out(text, ::SysStringLen(text));
  ::SysFreeString(text);
  return out;
}

long RoleOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  VARIANT role;
  ::VariantInit(&role);
  if (acc == nullptr || FAILED(acc->get_accRole(child, &role))) return -1;
  return role.lVal;
}

long StateOf(IAccessible* acc, long id) {
  VARIANT child = ChildId(id);
  VARIANT state;
  ::VariantInit(&state);
  if (acc == nullptr || FAILED(acc->get_accState(child, &state))) return -1;
  return state.lVal;
}

// A child as its own IAccessible (refcount transferred to the caller).
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

// Every element the client can reach. This is the number the card's
// "at least 20" assertion is about: the tree a screen reader walks.
int CountElements(IAccessible* acc) {
  const long children = ChildCount(acc);
  int total = 1;
  for (long i = 1; i <= children; ++i) {
    if (IAccessible* child = ChildObject(acc, i)) {
      total += CountElements(child);
      child->Release();
    } else {
      ++total;  // the parent answers for this child id
    }
  }
  return total;
}

// Find (parent, childId) for a name, optionally filtered by role. Returns a
// reference on the parent so the caller can invoke on the id it found.
bool FindNamed(IAccessible* acc, const std::wstring& name, long wantRole,
               IAccessible** parentOut, long* idOut) {
  const long children = ChildCount(acc);
  for (long i = 1; i <= children; ++i) {
    if (NameOf(acc, i) != name) continue;
    if (wantRole >= 0 && RoleOf(acc, i) != wantRole) continue;
    acc->AddRef();
    *parentOut = acc;
    *idOut = i;
    return true;
  }
  for (long i = 1; i <= children; ++i) {
    IAccessible* child = ChildObject(acc, i);
    if (child == nullptr) continue;
    if (FindNamed(child, name, wantRole, parentOut, idOut)) {
      child->Release();
      return true;
    }
    child->Release();
  }
  return false;
}

// The child-id-bearing pair, released on scope exit.
struct Located {
  IAccessible* parent = nullptr;
  long id = 0;
  ~Located() {
    if (parent != nullptr) parent->Release();
  }
  bool ok() const { return parent != nullptr; }
};

Located Locate(IAccessible* root, const std::wstring& name, long role = -1) {
  Located out;
  FindNamed(root, name, role, &out.parent, &out.id);
  return out;
}

// D3 proof for the read-only arm: the field is there, and the write is refused.
// Only a build that really answers E_NOTIMPL may print the CAUGHT marker.
[[maybe_unused]] bool SearchValueWriteRefused() {
  if (g_window == nullptr || !g_window->SearchOpen()) return false;
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  if (acc == nullptr) return false;
  bool refused = false;
  for (long i = 1; i <= ChildCount(acc); ++i) {
    if (RoleOf(acc, i) != ROLE_SYSTEM_TEXT) continue;
    VARIANT child = ChildId(i);
    BSTR probe = ::SysAllocString(L"probe");
    refused = acc->put_accValue(child, probe) == E_NOTIMPL;
    ::SysFreeString(probe);
    break;
  }
  acc->Release();
  return refused;
}

// The focused element's name, through accFocus (VT_DISPATCH) or "-self" for
// CHILDID_SELF, or "" when nothing has focus.
std::wstring FocusedName(IAccessible* acc) {
  VARIANT focus;
  ::VariantInit(&focus);
  if (acc == nullptr || FAILED(acc->get_accFocus(&focus))) return {};
  if (focus.vt == VT_I4) return L"-self";
  if (focus.vt != VT_DISPATCH || focus.pdispVal == nullptr) return {};
  IAccessible* focused = nullptr;
  focus.pdispVal->QueryInterface(IID_IAccessible, (void**)&focused);
  focus.pdispVal->Release();
  if (focused == nullptr) return {};
  std::wstring name = NameOf(focused, CHILDID_SELF);
  focused->Release();
  return name;
}

// The row index of the folder page that shows 'leaf' -- so "accSelect changed
// the WINDOW's selection" is asserted against the window's own model instead of
// a number copied into the test.
int FolderRowOf(GalleryWindow* window, const std::wstring& leaf) {
  const std::vector<prism::ArchiveItem>& items = window->Items();
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::string& path = items[i].path;
    const std::size_t slash = path.find_last_of('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (ToWide(name) == leaf) return static_cast<int>(i);
  }
  return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
PRISM_TEST("c80.client_accessible_reaches_the_whole_client_area") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  // In the mutant build this is the FIRST red: WM_GETOBJECT answered 0.
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  const int elements = CountElements(acc);
  std::cout << "  accessible elements=" << elements
            << " window elements=" << g_window->AccessibleElementCount() << std::endl;
  // THE CARD'S NUMBER: nine before the fix (title bar + menu bar), at least
  // twenty now.
  PRISM_CHECK(elements >= 20);
  // ...and the client must reach every element the window itself publishes.
  PRISM_CHECK_EQ(elements, g_window->AccessibleElementCount());
  // 8 buttons + 1 tab + 1 list + 1 outline + 1 status, in band/tab/list order.
  PRISM_CHECK_EQ(ChildCount(acc), 5L + 7L);  // 12
  acc->Release();
}

PRISM_TEST("c80.action_buttons_are_named_in_the_active_language") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  const Located extract = Locate(acc, L"\u89E3\u538B\u5230\u2026", ROLE_SYSTEM_PUSHBUTTON);
  const Located open = Locate(acc, L"\u6253\u5F00", ROLE_SYSTEM_PUSHBUTTON);
  const Located search = Locate(acc, L"\u641C\u7D22", ROLE_SYSTEM_PUSHBUTTON);
  PRISM_CHECK(extract.ok());
  PRISM_CHECK(open.ok());
  PRISM_CHECK(search.ok());

  // The enable matrix reaches the client: with nothing selected 添加/删除 are
  // UNAVAILABLE, and 打开 is not.
  const Located add = Locate(acc, L"\u6DFB\u52A0", ROLE_SYSTEM_PUSHBUTTON);
  PRISM_CHECK(add.ok());
  if (add.ok()) {
    PRISM_CHECK((StateOf(add.parent, add.id) & STATE_SYSTEM_UNAVAILABLE) != 0);
    PRISM_CHECK_EQ(g_window->ActionContext().hasSelection, false);
  }
  if (open.ok()) {
    PRISM_CHECK((StateOf(open.parent, open.id) & STATE_SYSTEM_UNAVAILABLE) == 0);
    // The default action a screen reader announces exists and is a verb.
    VARIANT child = ChildId(open.id);
    BSTR action = nullptr;
    PRISM_CHECK(SUCCEEDED(open.parent->get_accDefaultAction(child, &action)));
    PRISM_CHECK(action != nullptr && ::SysStringLen(action) > 0);
    ::SysFreeString(action);
  }
  acc->Release();
}

PRISM_TEST("c80.the_open_archive_is_a_selected_page_tab") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  const Located tab = Locate(acc, L"fixture.7z", ROLE_SYSTEM_PAGETAB);
  PRISM_CHECK(tab.ok());
  if (tab.ok()) {
    PRISM_CHECK((StateOf(tab.parent, tab.id) & STATE_SYSTEM_SELECTED) != 0);
    PRISM_CHECK((StateOf(tab.parent, tab.id) & STATE_SYSTEM_SELECTABLE) != 0);
  }
  acc->Release();
}

PRISM_TEST("c80.list_rows_are_named_and_accSelect_moves_the_window_selection") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  // The list's name is the breadcrumb (which the layout could reword), so find
  // it by ROLE instead of by a string a future change might edit.
  long listId = 0;
  for (long i = 1; i <= ChildCount(acc); ++i) {
    if (RoleOf(acc, i) == ROLE_SYSTEM_LIST) {
      listId = i;
      break;
    }
  }
  PRISM_CHECK(listId > 0);
  long rows = -1;
  if (listId > 0) {
    IAccessible* list = ChildObject(acc, listId);
    PRISM_CHECK(list != nullptr);
    if (list != nullptr) {
      rows = ChildCount(list);
      list->Release();
    }
  }
  // 13 rows: one folder + twelve files.
  PRISM_CHECK_EQ(rows, 13L);
  const Located item = Locate(acc, L"item03.txt", ROLE_SYSTEM_LISTITEM);
  PRISM_CHECK(item.ok());
  if (item.ok()) {
    PRISM_CHECK_EQ(FolderRowOf(g_window, L"item03.txt"), 3);  // docs, item01, item02, item03
    VARIANT child = ChildId(item.id);
    PRISM_CHECK(SUCCEEDED(item.parent->accSelect(SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION,
                                                  child)));
    // The WINDOW's state moved -- not a copy inside the provider.
    PRISM_CHECK_EQ(g_window->Selection(), 3);
    PRISM_CHECK_EQ(g_window->SelectionCount(), 1);
    // ...and the band's legality followed the selection (the same rule a click
    // feeds): 添加 is now available.
    PRISM_CHECK_EQ(g_window->ActionContext().hasSelection, true);
    // The selected row is where accFocus points while no other surface owns the
    // keyboard -- the same element the window paints its focus ring around.
    PRISM_CHECK_EQ(ToUtf8(FocusedName(acc)), std::string("item03.txt"));
  }
  acc->Release();
}

PRISM_TEST("c80.default_action_presses_search_and_the_field_appears") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  PRISM_CHECK(!g_window->SearchOpen());
  const Located search = Locate(acc, L"\u641C\u7D22", ROLE_SYSTEM_PUSHBUTTON);
  PRISM_CHECK(search.ok());
  if (search.ok()) {
    VARIANT child = ChildId(search.id);
    PRISM_CHECK(SUCCEEDED(search.parent->accDoDefaultAction(child)));
  }
  // The SAME path a click takes ended in the window's own search state.
  PRISM_CHECK(g_window->SearchOpen());
  // ...and a client can now READ the field.
  const Located field = Locate(acc, L"\u641C\u7D22\u6846", ROLE_SYSTEM_TEXT);
  PRISM_CHECK(field.ok());
  acc->Release();
}

PRISM_TEST("c80.search_field_accepts_a_written_value_like_a_typed_one") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  PRISM_CHECK(g_window->SearchOpen());  // the previous case opened it through the UI
  const Located field = Locate(acc, L"\u641C\u7D22\u6846", ROLE_SYSTEM_TEXT);
  PRISM_CHECK(field.ok());
  if (!field.ok()) {
    acc->Release();
    return;
  }
  // What a UIA client reads before it types: focusable, and NOT read-only (the
  // proxy derives IsReadOnly from STATE_SYSTEM_READONLY, and the field accepts a
  // value -- a read-only bit here is "visible but unusable").
  const long state = StateOf(field.parent, field.id);
  PRISM_CHECK((state & STATE_SYSTEM_FOCUSABLE) != 0);
  PRISM_CHECK((state & STATE_SYSTEM_READONLY) == 0);
  PRISM_CHECK_EQ(g_window->ListRowCount(), 13);

  VARIANT child = ChildId(field.id);
  BSTR one = ::SysAllocString(L"item01");
  // In the read-only mutant build this is the FIRST red (E_NOTIMPL).
  PRISM_CHECK_EQ(field.parent->put_accValue(child, one), S_OK);
  ::SysFreeString(one);

  // (1) the QUERY changed -- through the same model the keyboard edits.
  PRISM_CHECK_EQ(g_window->SearchModel().Query(), std::string("item01"));
  // (2) the list is filtered by the same ApplySearchFilter a typed character
  //     runs, while the CATALOG is untouched (filtering is a view).
  PRISM_CHECK_EQ(g_window->ListRowCount(), 1);
  PRISM_CHECK_EQ(g_window->Items().size(), static_cast<std::size_t>(13));
  // ...and a client reads the new text back.
  {
    BSTR read = nullptr;
    PRISM_CHECK(SUCCEEDED(field.parent->get_accValue(child, &read)));
    PRISM_CHECK(read != nullptr && std::wstring(read) == L"item01");
    ::SysFreeString(read);
  }

  // (3) an empty write restores every row.
  BSTR empty = ::SysAllocString(L"");
  PRISM_CHECK_EQ(field.parent->put_accValue(child, empty), S_OK);
  ::SysFreeString(empty);
  PRISM_CHECK_EQ(g_window->SearchModel().Query(), std::string(""));
  PRISM_CHECK_EQ(g_window->ListRowCount(), 13);

  // The default action on the field puts the keyboard in it (UIA Focus).
  PRISM_CHECK(SUCCEEDED(field.parent->accDoDefaultAction(child)));
  PRISM_CHECK_EQ(ToUtf8(FocusedName(acc)), std::string("\u641C\u7D22\u6846"));
  acc->Release();
}

PRISM_TEST("c80.focus_and_hit_test_agree_with_the_window") {
  IAccessible* acc = ClientAccessible(g_window->Hwnd());
  PRISM_CHECK(acc != nullptr);
  if (acc == nullptr) return;
  // The open search field owns the keyboard, so accFocus names it.
  PRISM_CHECK_EQ(ToUtf8(FocusedName(acc)), std::string("\u641C\u7D22\u6846"));

  // accHitTest at the centre of the 搜索 button's OWN accLocation must come
  // back as that button: the published rectangle and the hit test cannot drift.
  const Located search = Locate(acc, L"\u641C\u7D22", ROLE_SYSTEM_PUSHBUTTON);
  PRISM_CHECK(search.ok());
  if (search.ok()) {
    VARIANT child = ChildId(search.id);
    long left = 0, top = 0, width = 0, height = 0;
    PRISM_CHECK(SUCCEEDED(
        search.parent->accLocation(&left, &top, &width, &height, child)));
    std::cout << "  search rect=" << left << "," << top << " " << width << "x" << height
              << std::endl;
    PRISM_CHECK(width > 0);
    PRISM_CHECK(height > 0);
    VARIANT hit;
    ::VariantInit(&hit);
    PRISM_CHECK(SUCCEEDED(acc->accHitTest(left + width / 2, top + height / 2, &hit)));
    std::wstring hitName;
    if (hit.vt == VT_DISPATCH && hit.pdispVal != nullptr) {
      IAccessible* hitAcc = nullptr;
      hit.pdispVal->QueryInterface(IID_IAccessible, (void**)&hitAcc);
      hit.pdispVal->Release();
      if (hitAcc != nullptr) {
        hitName = NameOf(hitAcc, CHILDID_SELF);
        hitAcc->Release();
      }
    }
    PRISM_CHECK_EQ(ToUtf8(hitName), std::string("\u641C\u7D22"));
  }

  // Closing the search hands the keyboard back to the band ring -- pressing the
  // button put the ring on 搜索 (ActionBarClick does that), and the ring is what
  // the window paints. accFocus must follow, or a screen reader would announce a
  // field that no longer owns the keys.
  g_window->CloseSearch();
  PRISM_CHECK_EQ(ToUtf8(FocusedName(acc)), std::string("\u641C\u7D22"));
  acc->Release();
}

PRISM_TEST("c80.an_empty_window_publishes_an_empty_list_and_survives") {
  IAccessible* acc = nullptr;
  {
    auto empty = std::make_unique<GalleryWindow>();
    if (!empty->Create(L"PrismArchive a11y empty", ::GetModuleHandleW(nullptr))) {
      std::cout << "  (second window could not be created; the window station is shared)"
                << std::endl;
      return;
    }
    acc = ClientAccessible(empty->Hwnd());
    PRISM_CHECK(acc != nullptr);
    if (acc != nullptr) {
      const long children = ChildCount(acc);
      // No archive: eight buttons + one (empty) list + one outline + one status.
      PRISM_CHECK_EQ(children, 11L);
      bool sawList = false;
      for (long i = 1; i <= children; ++i) {
        if (RoleOf(acc, i) != ROLE_SYSTEM_LIST) continue;
        sawList = true;
        IAccessible* list = ChildObject(acc, i);
        PRISM_CHECK(list != nullptr);
        if (list != nullptr) {
          // Empty, NOT missing, and no crash: the card's negative-state rule.
          PRISM_CHECK_EQ(ChildCount(list), 0L);
          list->Release();
        }
      }
      PRISM_CHECK(sawList);
    }
  }  // the window (and its HWND) die here
  // A client that kept the object across the close must get an ANSWER, not a
  // use-after-free: the tree is detached, so no screen location can be named.
  if (acc != nullptr) {
    VARIANT child = ChildId(1);
    long left = 0, top = 0, width = 0, height = 0;
    PRISM_CHECK_EQ(acc->accLocation(&left, &top, &width, &height, child), E_FAIL);
    acc->Release();
  }
}

// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return kSkipReturnCode;
  }
  BuildCatalog();

  static GalleryWindow window;
  if (!window.Create(L"PrismArchive accessibility", ::GetModuleHandleW(nullptr))) {
    std::cout << "SKIP: window creation failed (no interactive window station)" << std::endl;
    return kSkipReturnCode;
  }
  g_window = &window;
  // The shape the app publishes: one archive with a folder and twelve files, a
  // tab naming it, and a status line. The window is not a mock -- these are the
  // SAME setters Main.cpp calls.
  window.SetRootLabel(L"fixture.7z");
  window.SetItems(&g_catalog);
  window.SetStatus(L"\u5171 13 \u9879");
  (void)window.OpenTab("C:/fixtures/fixture.7z", true);
  // The app's side of the band (Main.cpp installs the same handler): pressing a
  // button through MSAA must run the SAME wiring a click runs, so 搜索 has to
  // open the real search field -- not a test-only stub.
  window.SetActionHandler([](prism::actionbar::ActionId id, int) {
    if (id == prism::actionbar::ActionId::Search) g_window->OpenSearch();
  });
  window.Show();

  if (kMutation != nullptr) {
    std::cout << kMutationBanner << std::endl;
    // CONTRACTS D27: armed BEFORE the first assertion. The predicate proves the
    // injected defect really fired (the window answers WM_GETOBJECT with 0), so
    // a red for an unrelated reason can never print CAUGHT (D3).
    // The predicate asks the WINDOW, not the macro only: WM_GETOBJECT must really
    // answer 0. (AccessibleObjectFromWindow still hands back the OS's default
    // window object in that state, so the message itself is the proof.)
    if (std::strcmp(kMutation, "readonly_search") == 0) {
      // The write must really be refused before a red may claim CAUGHT (D3).
      prism::test::SetMutationVerdict("C80 MUTATION CAUGHT: readonly_search", [] {
        return prism::win::a11y::ActiveMutation() == "readonly_search" &&
               SearchValueWriteRefused();
      });
    } else {
      prism::test::SetMutationVerdict("C80 MUTATION CAUGHT: no_getobject", [] {
        return prism::win::a11y::ActiveMutation() == "no_getobject" && g_window != nullptr &&
               ::SendMessageW(g_window->Hwnd(), WM_GETOBJECT, 0, OBJID_CLIENT) == 0;
      });
    }
  }

  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    // Reaching this line means the suite stayed green (stale injection) or went
    // red for a reason the predicate did not prove: both must stay RED.
    if (rc == 0) {
      std::cout << "C80 MUTATION NOT CAUGHT: the suite stayed green with the accessibility "
                   "bridge disarmed -- the injection is stale"
                << std::endl;
    }
    return 1;
  }
  if (rc != 0) return rc;
  std::cout << "ALL A11Y CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  ::CoUninitialize();
  return 0;
}
