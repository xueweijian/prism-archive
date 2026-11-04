// PrismArchive -- PrismWin
// Accessible: the MSAA bridge for a window that paints every pixel itself
// (card C80).
//
// THE MEASURED DEFECT. UIA against the shipped window returned NINE elements:
// the title bar's three buttons, the system menu and four menu items. The eight
// action buttons, the tab strip, every row, the sidebar and the status line were
// invisible, because they are D2D paint, not child HWNDs -- the window had no
// WM_GETOBJECT handler at all. A screen reader cannot use the product, and
// automation can only compare pixels.
//
// WHAT THIS FILE IS. A full MSAA object model: WM_GETOBJECT(OBJID_CLIENT) hands
// UIA an IAccessible (LresultFromObject), and the platform's MSAA proxy turns it
// into UIA elements -- ControlType, Name, BoundingRectangle, IsEnabled,
// InvokePattern (through accDoDefaultAction), SelectionItemPattern (through
// accSelect) -- without us writing a single IRawElementProviderSimple. That is
// the bridge the card asks for: the drawing stays ours, accessibility is added
// BESIDE it, never by replacing it with standard controls.
//
// WHY ONE OBJECT PER ELEMENT. MSAA clients walk the tree with
// get_accChildCount -> get_accChild(id) -> that child's own accName/accRole/
// accChildCount... A "simple" provider that answers every child id from the root
// cannot describe grandchildren (a list item inside the list), so each element is
// its own IAccessible holding its subtree snapshot. Child ids are 1-based indices
// into that snapshot, which is exactly what MSAA specifies.
//
// FRESHNESS. Every snapshot is rebuilt from the window's live state when the
// object is created, and accQuery calls that arrive later in the same walk reuse
// it. That is the same guarantee Explorer's list view gives: an element is a view
// of state at the moment it was asked for. All calls are marshalled onto the UI
// thread (LresultFromObject does that), so a snapshot cannot be mutated by
// another thread while it is being read.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27). PRISM_C80_MUTANT_NO_GETOBJECT makes
// WM_GETOBJECT answer 0 -- the exact pre-fix behaviour. The Windows suite's
// element-count assertion MUST go red and print "C80 MUTATION CAUGHT:
// no_getobject" at the FIRST failure. ActiveMutation() is the runtime proof that
// the macro compiled in, so a stale injection can never claim CAUGHT.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <oleacc.h>
#include <oleauto.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/A11yTree.hpp"

#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")

namespace prism::win::a11y {

// ------------------------------------------------------------------ mutation
#if defined(PRISM_C80_MUTANT_NO_GETOBJECT)
// MUTANT ACTIVE (C80 arm no_getobject): the window answers WM_GETOBJECT with 0,
// i.e. exactly the pre-fix state where the client area was unreachable. The
// element-count assertion in Tests/win/test_c80_a11y.cpp MUST go red.
inline constexpr bool kAccessibleBridgeArmed = false;
#else
inline constexpr bool kAccessibleBridgeArmed = true;
#endif

// C80's second arm. The search field is visible and focusable, but its VALUE is
// read-only: put_accValue answers E_NOTIMPL, which is exactly the state the UIA
// driver reported as "POLICY_DENIED: supported_action" -- a screen reader can
// find the field and cannot type into it.
#if defined(PRISM_C80_MUTANT_READONLY_SEARCH)
// MUTANT ACTIVE (C80 arm readonly_search): the writable-value bridge is gone.
inline constexpr bool kSearchValueWritable = false;
#else
inline constexpr bool kSearchValueWritable = true;
#endif

inline constexpr std::string_view ActiveMutation() {
  if (!kAccessibleBridgeArmed) return std::string_view("no_getobject");
  if (!kSearchValueWritable) return std::string_view("readonly_search");
  return std::string_view();
}

// --------------------------------------------------------------- conversions
inline std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        needed);
  return wide;
}

inline std::string WideToUtf8(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                           nullptr);
  if (needed <= 0) return std::string();
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                        needed, nullptr, nullptr);
  return utf8;
}

inline BSTR AllocBstr(const std::wstring& text) {
  return ::SysAllocStringLen(text.data(), static_cast<UINT>(text.size()));
}

// The ONE role mapping (PrismCore deliberately has no ROLE_SYSTEM_* constants).
inline long RoleToMsaa(prism::a11y::Role role) {
  switch (role) {
    case prism::a11y::Role::Client: return ROLE_SYSTEM_CLIENT;
    case prism::a11y::Role::PushButton: return ROLE_SYSTEM_PUSHBUTTON;
    case prism::a11y::Role::PageTab: return ROLE_SYSTEM_PAGETAB;
    case prism::a11y::Role::List: return ROLE_SYSTEM_LIST;
    case prism::a11y::Role::ListItem: return ROLE_SYSTEM_LISTITEM;
    case prism::a11y::Role::Outline: return ROLE_SYSTEM_OUTLINE;
    case prism::a11y::Role::OutlineItem: return ROLE_SYSTEM_OUTLINEITEM;
    case prism::a11y::Role::StatusBar: return ROLE_SYSTEM_STATUSBAR;
    case prism::a11y::Role::Text: return ROLE_SYSTEM_TEXT;
    case prism::a11y::Role::Group: return ROLE_SYSTEM_GROUPING;
    case prism::a11y::Role::ProgressBar: return ROLE_SYSTEM_PROGRESSBAR;
  }
  return ROLE_SYSTEM_CLIENT;
}

// The state word a client reads through get_accState. The three rules that
// matter for this product:
//   * a disabled action (the band's enable matrix) is UNAVAILABLE, so a screen
//     reader says "dimmed" and UIA's IsEnabled is false;
//   * a row / tab is SELECTABLE and SELECTED, which is what makes UIA expose
//     SelectionItemPattern and what accSelect changes;
//   * focus is answered from the same state the window paints its ring from.
inline long StateOf(const prism::a11y::Node& node) {
  long state = 0;
  if (node.role == prism::a11y::Role::PushButton ||
      node.role == prism::a11y::Role::PageTab ||
      node.role == prism::a11y::Role::ListItem ||
      node.role == prism::a11y::Role::OutlineItem ||
      node.role == prism::a11y::Role::List ||
      node.role == prism::a11y::Role::Text) {
    state |= STATE_SYSTEM_FOCUSABLE;
  }
  // NO STATE_SYSTEM_READONLY on the search field: the UIA proxy derives
  // IsReadOnly from that bit, and the field accepts put_accValue. Getting this
  // wrong is invisible in the tree and fatal in use (the driver reports
  // "editable pass / supported_action fail").
  if (!node.enabled) state |= STATE_SYSTEM_UNAVAILABLE;
  if (node.focused) state |= STATE_SYSTEM_FOCUSED;
  if (node.role == prism::a11y::Role::PageTab ||
      node.role == prism::a11y::Role::ListItem) {
    state |= STATE_SYSTEM_SELECTABLE;
  }
  if (node.selected) state |= STATE_SYSTEM_SELECTED;
  if (node.expandable) {
    state |= node.expanded ? STATE_SYSTEM_EXPANDED : STATE_SYSTEM_COLLAPSED;
  }
  return state;
}

// ---------------------------------------------------------------- the window
// What an accessible element needs from the window that owns it. The provider
// never includes GalleryWindow (circular include, and a 4k-line header in every
// client TU); GalleryWindow implements this in one small adapter and hands out a
// shared_ptr, so an Accessible that outlives the window degrades to "empty"
// instead of dereferencing a dead pointer.
class Host {
 public:
  virtual ~Host() = default;
  virtual HWND Window() const = 0;
  // A fresh snapshot of the whole client-area tree (root included).
  virtual prism::a11y::Node Tree() = 0;
  // The SAME paths a click and the keyboard take -- never a second implementation.
  virtual void InvokeAction(int index) = 0;
  virtual void FocusAction(int index) = 0;
  virtual void SelectRow(int row) = 0;
  virtual void ActivateRow(int row) = 0;
  virtual void ActivateTab(int index) = 0;
  virtual void ActivateFolder(const std::string& key) = 0;
  // The search field's two verbs (C80 follow-up): a VALUE write and "put the
  // keyboard in it". Both must reach the SAME model the keyboard uses.
  virtual void SetSearchValue(const std::string& utf8) = 0;
  virtual void FocusSearchField() = 0;
};

class Accessible final : public IAccessible {
 public:
  // Refcount 1: the caller owns the reference (COM out-parameter convention,
  // CONTRACTS D15 -- no extra AddRef on construction).
  static Accessible* Create(std::shared_ptr<Host> host, std::vector<int> path) {
    prism::a11y::Node tree;
    if (host) tree = host->Tree();
    const prism::a11y::Node* node = prism::a11y::NodeAtPath(tree, path);
    if (node == nullptr) return nullptr;
    return new (std::nothrow) Accessible(std::move(host), std::move(path), *node);
  }

  // ---- IUnknown ----------------------------------------------------------
  STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
    if (out == nullptr) return E_INVALIDARG;
    *out = nullptr;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDispatch) ||
        riid == __uuidof(IAccessible)) {
      *out = static_cast<IAccessible*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (left == 0) delete this;
    return left;
  }

  // ---- IDispatch (IAccessible's base) ------------------------------------
  STDMETHODIMP GetTypeInfoCount(UINT* count) override {
    if (count != nullptr) *count = 0;
    return S_OK;
  }
  STDMETHODIMP GetTypeInfo(UINT, LCID, ITypeInfo** info) override {
    if (info != nullptr) *info = nullptr;
    return E_NOTIMPL;
  }
  STDMETHODIMP GetIDsOfNames(REFIID, LPOLESTR*, UINT, LCID, DISPID*) override {
    return E_NOTIMPL;
  }
  STDMETHODIMP Invoke(DISPID, REFIID, LCID, WORD, DISPPARAMS*, VARIANT*,
                      EXCEPINFO*, UINT*) override {
    return DISP_E_MEMBERNOTFOUND;
  }

  // ---- IAccessible: identity ---------------------------------------------
  STDMETHODIMP get_accParent(IDispatch** parent) override {
    if (parent == nullptr) return E_INVALIDARG;
    *parent = nullptr;
    if (path_.empty() || !host_) return S_FALSE;  // the window has no MSAA parent
    std::vector<int> up = path_;
    up.pop_back();
    Accessible* acc = Create(host_, std::move(up));
    if (acc == nullptr) return S_FALSE;
    *parent = static_cast<IDispatch*>(acc);  // refcount 1 transferred (D15)
    return S_OK;
  }

  STDMETHODIMP get_accChildCount(long* count) override {
    if (count == nullptr) return E_INVALIDARG;
    Refresh();
    *count = static_cast<long>(node_.children.size());
    return S_OK;
  }

  STDMETHODIMP get_accChild(VARIANT varChild, IDispatch** child) override {
    if (child == nullptr) return E_INVALIDARG;
    *child = nullptr;
    if (varChild.vt != VT_I4) return E_INVALIDARG;
    if (varChild.lVal == CHILDID_SELF) return S_FALSE;
    Refresh();
    if (node_.Child(varChild.lVal) == nullptr) return E_INVALIDARG;
    Accessible* acc = Create(host_, OffsetPath(varChild.lVal));
    if (acc == nullptr) return S_FALSE;
    *child = static_cast<IDispatch*>(acc);
    return S_OK;
  }

  STDMETHODIMP get_accName(VARIANT varChild, BSTR* name) override {
    if (name == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    *name = AllocBstr(Utf8ToWide(node->name));
    return *name != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP get_accValue(VARIANT varChild, BSTR* value) override {
    if (value == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->value.empty()) {
      *value = nullptr;
      return S_FALSE;
    }
    *value = AllocBstr(Utf8ToWide(node->value));
    return *value != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP get_accDescription(VARIANT varChild, BSTR* text) override {
    if (text == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->description.empty()) {
      *text = nullptr;
      return S_FALSE;
    }
    *text = AllocBstr(Utf8ToWide(node->description));
    return *text != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP get_accRole(VARIANT varChild, VARIANT* role) override {
    if (role == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    ::VariantInit(role);
    role->vt = VT_I4;
    role->lVal = RoleToMsaa(node->role);
    return S_OK;
  }

  STDMETHODIMP get_accState(VARIANT varChild, VARIANT* state) override {
    if (state == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    ::VariantInit(state);
    state->vt = VT_I4;
    state->lVal = StateOf(*node);
    return S_OK;
  }

  STDMETHODIMP get_accHelp(VARIANT, BSTR* help) override {
    if (help != nullptr) *help = nullptr;
    return S_FALSE;
  }
  STDMETHODIMP get_accHelpTopic(BSTR*, VARIANT, long* topic) override {
    if (topic != nullptr) *topic = 0;
    return S_FALSE;
  }

  STDMETHODIMP get_accKeyboardShortcut(VARIANT varChild, BSTR* shortcut) override {
    if (shortcut == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->shortcut.empty()) {
      *shortcut = nullptr;
      return S_FALSE;
    }
    *shortcut = AllocBstr(Utf8ToWide(node->shortcut));
    return *shortcut != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  // accFocus must name ONE element. The tree is built with at most one focused
  // node (the band ring, else the selection row), so this is a lookup, not a
  // decision.
  STDMETHODIMP get_accFocus(VARIANT* focus) override {
    if (focus == nullptr) return E_INVALIDARG;
    ::VariantInit(focus);
    Refresh();
    std::vector<int> relative;
    const prism::a11y::Node* hit = prism::a11y::FindFocused(node_, &relative);
    if (hit == nullptr) return S_FALSE;  // VT_EMPTY == "no element has focus"
    if (relative.empty()) {
      focus->vt = VT_I4;
      focus->lVal = CHILDID_SELF;
      return S_OK;
    }
    Accessible* acc = Create(host_, AbsolutePath(relative));
    if (acc == nullptr) return S_FALSE;
    focus->vt = VT_DISPATCH;
    focus->pdispVal = static_cast<IDispatch*>(acc);
    return S_OK;
  }

  // A single selected element is what this product can show (the multi-select
  // set is exposed through the rows' SELECTED state). An enum of several is a
  // documented limitation, and answering S_FALSE is honest about it.
  STDMETHODIMP get_accSelection(VARIANT* selection) override {
    if (selection == nullptr) return E_INVALIDARG;
    ::VariantInit(selection);
    Refresh();
    for (std::size_t i = 0; i < node_.children.size(); ++i) {
      if (!node_.children[i].selected) continue;
      Accessible* acc = Create(host_, AbsolutePath({static_cast<int>(i)}));
      if (acc == nullptr) return S_FALSE;
      selection->vt = VT_DISPATCH;
      selection->pdispVal = static_cast<IDispatch*>(acc);
      return S_OK;
    }
    return S_FALSE;
  }

  STDMETHODIMP get_accDefaultAction(VARIANT varChild, BSTR* action) override {
    if (action == nullptr) return E_INVALIDARG;
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->action.empty()) {
      *action = nullptr;
      return S_FALSE;
    }
    *action = AllocBstr(Utf8ToWide(node->action));
    return *action != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  // ---- IAccessible: verbs ------------------------------------------------
  STDMETHODIMP accSelect(long flags, VARIANT varChild) override {
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (!node->enabled) return S_FALSE;
    const bool focus = (flags & (SELFLAG_TAKEFOCUS | SELFLAG_TAKESELECTION)) != 0;
    if (!focus) return S_FALSE;  // ADD/EXTEND: the set is not what this exposes
    switch (node->role) {
      case prism::a11y::Role::PushButton:
        if (host_) host_->FocusAction(node->index);
        return S_OK;
      case prism::a11y::Role::PageTab:
        if (host_) host_->ActivateTab(node->index);
        return S_OK;
      case prism::a11y::Role::ListItem:
        if (host_) host_->SelectRow(node->index);
        return S_OK;
      case prism::a11y::Role::OutlineItem:
        if (host_) host_->ActivateFolder(node->key);
        return S_OK;
      case prism::a11y::Role::Text:
        // TAKEFOCUS on the field is the same verb as its default action.
        if (host_) host_->FocusSearchField();
        return S_OK;
      case prism::a11y::Role::Client:
      case prism::a11y::Role::List:
      case prism::a11y::Role::Outline:
      case prism::a11y::Role::StatusBar:
      case prism::a11y::Role::Group:
      case prism::a11y::Role::ProgressBar:
        return S_FALSE;
    }
    return S_FALSE;
  }

  STDMETHODIMP accLocation(long* left, long* top, long* width, long* height,
                           VARIANT varChild) override {
    if (left == nullptr || top == nullptr || width == nullptr || height == nullptr) {
      return E_INVALIDARG;
    }
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    POINT origin{0, 0};
    if (!host_ || host_->Window() == nullptr ||
        ::ClientToScreen(host_->Window(), &origin) == FALSE) {
      return E_FAIL;
    }
    const prism::layout::Rect& bounds = node->bounds;
    *left = origin.x + bounds.left;
    *top = origin.y + bounds.top;
    *width = bounds.Width();
    *height = bounds.Height();
    return S_OK;
  }

  STDMETHODIMP accNavigate(long direction, VARIANT start, VARIANT* end) override {
    if (end == nullptr) return E_INVALIDARG;
    ::VariantInit(end);
    if (start.vt != VT_I4) return E_INVALIDARG;
    Refresh();
    if (start.lVal == CHILDID_SELF) {
      if (direction == NAVDIR_FIRSTCHILD || direction == NAVDIR_LASTCHILD) {
        if (node_.children.empty()) return S_FALSE;
        const int index = direction == NAVDIR_FIRSTCHILD
                              ? 1
                              : static_cast<int>(node_.children.size());
        Accessible* acc = Create(host_, AbsolutePath({index}));
        if (acc == nullptr) return S_FALSE;
        end->vt = VT_DISPATCH;
        end->pdispVal = static_cast<IDispatch*>(acc);
        return S_OK;
      }
      return S_FALSE;
    }
    if (node_.Child(start.lVal) == nullptr) return E_INVALIDARG;
    if (direction == NAVDIR_NEXT) {
      if (node_.Child(start.lVal + 1) == nullptr) return S_FALSE;
      Accessible* acc = Create(host_, AbsolutePath({start.lVal + 1}));
      if (acc == nullptr) return S_FALSE;
      end->vt = VT_DISPATCH;
      end->pdispVal = static_cast<IDispatch*>(acc);
      return S_OK;
    }
    if (direction == NAVDIR_PREVIOUS) {
      if (node_.Child(start.lVal - 1) == nullptr) return S_FALSE;
      Accessible* acc = Create(host_, AbsolutePath({start.lVal - 1}));
      if (acc == nullptr) return S_FALSE;
      end->vt = VT_DISPATCH;
      end->pdispVal = static_cast<IDispatch*>(acc);
      return S_OK;
    }
    return S_FALSE;
  }

  // accHitTest: SCREEN coordinates in, the deepest element at that point out.
  // This is what lets UIA answer "what is under the pointer" and what makes the
  // window's focus story verifiable from the outside.
  STDMETHODIMP accHitTest(long x, long y, VARIANT* child) override {
    if (child == nullptr) return E_INVALIDARG;
    ::VariantInit(child);
    if (!host_ || host_->Window() == nullptr) return S_FALSE;
    Refresh();
    POINT point{x, y};
    if (::ScreenToClient(host_->Window(), &point) == FALSE) return S_FALSE;
    const prism::a11y::Node* hit = nullptr;
    std::vector<int> relative;
    if (!prism::a11y::HitTest(node_, point.x, point.y, &hit, &relative)) {
      return S_FALSE;  // outside this element entirely
    }
    if (relative.empty()) {
      child->vt = VT_I4;
      child->lVal = CHILDID_SELF;
      return S_OK;
    }
    Accessible* acc = Create(host_, AbsolutePath(relative));
    if (acc == nullptr) return S_FALSE;
    child->vt = VT_DISPATCH;
    child->pdispVal = static_cast<IDispatch*>(acc);
    return S_OK;
  }

  // The ONE verb a screen reader / UIA InvokePattern ends at: the window's real
  // click path (GalleryWindow::A11yInvokeAction -> ActionBarClick), so a button
  // pressed by assistive technology can never behave differently from a click.
  STDMETHODIMP accDoDefaultAction(VARIANT varChild) override {
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->action.empty()) return DISP_E_MEMBERNOTFOUND;
    if (!node->enabled) return S_FALSE;
    switch (node->role) {
      case prism::a11y::Role::PushButton:
        if (host_) host_->InvokeAction(node->index);
        return S_OK;
      case prism::a11y::Role::PageTab:
        if (host_) host_->ActivateTab(node->index);
        return S_OK;
      case prism::a11y::Role::ListItem:
        if (host_) host_->ActivateRow(node->index);
        return S_OK;
      case prism::a11y::Role::OutlineItem:
        if (host_) host_->ActivateFolder(node->key);
        return S_OK;
      case prism::a11y::Role::Text:
        // "Press the field" means "put the keyboard in it" (UIA Focus).
        if (host_) host_->FocusSearchField();
        return S_OK;
      case prism::a11y::Role::Client:
      case prism::a11y::Role::List:
      case prism::a11y::Role::Outline:
      case prism::a11y::Role::StatusBar:
      case prism::a11y::Role::Group:
      case prism::a11y::Role::ProgressBar:
        return DISP_E_MEMBERNOTFOUND;
    }
    return DISP_E_MEMBERNOTFOUND;
  }

  STDMETHODIMP put_accName(VARIANT, BSTR) override { return E_NOTIMPL; }

  // WRITABLE VALUE (C80 follow-up). MSAA maps this to UIA's
  // ValuePattern.SetValue -- the call a screen reader or an automation client
  // uses to TYPE into the search field. Before this, the field was visible and
  // focusable but answered E_NOTIMPL here, which the UIA driver reports as
  // "POLICY_DENIED: supported_action": found, and impossible to use.
  //
  // It ends at the window's ONE search-text path (SetSearchValueFromAccessible
  // -> the model's SetQuery + ApplySearchFilter), the same code WM_CHAR runs for
  // a typed character. Only the field is editable: renaming a button through
  // MSAA is not a feature, so every other role still answers E_NOTIMPL.
  STDMETHODIMP put_accValue(VARIANT varChild, BSTR value) override {
    const prism::a11y::Node* node = Resolve(varChild);
    if (node == nullptr) return E_INVALIDARG;
    if (node->role != prism::a11y::Role::Text) return E_NOTIMPL;
    // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix read-only field.
    if (!kSearchValueWritable) return E_NOTIMPL;
    if (!host_) return E_FAIL;
    const std::wstring text =
        value != nullptr ? std::wstring(value, ::SysStringLen(value)) : std::wstring();
    host_->SetSearchValue(WideToUtf8(text));
    return S_OK;
  }

  // ---- read-back seams (the suite asserts the object it handed out) --------
  const prism::a11y::Node& Snapshot() const { return node_; }
  const std::vector<int>& Path() const { return path_; }

 private:
  Accessible(std::shared_ptr<Host> host, std::vector<int> path,
             const prism::a11y::Node& node)
      : host_(std::move(host)), path_(std::move(path)), node_(node) {}

  // LIVE view: every answer is computed from the window's CURRENT state, so an
  // object a client obtained once (exactly what a screen reader does) never
  // describes a UI that has moved on. The tree is tens of elements and this runs
  // on the UI thread, so rebuilding is cheaper than any cache-coherence bug.
  // An element that no longer exists keeps its last snapshot: a client pressing
  // an id it just read must get an answer (S_FALSE) rather than an empty object.
  void Refresh() const {
    // A detached host (the window is gone) must NOT clobber the last snapshot
    // with an empty tree: accLocation then answers "the window is gone" (E_FAIL)
    // instead of pretending the element never existed.
    if (!host_ || host_->Window() == nullptr) return;
    prism::a11y::Node tree = host_->Tree();
    const prism::a11y::Node* node = prism::a11y::NodeAtPath(tree, path_);
    if (node != nullptr) node_ = *node;
  }

  // The node a VARIANT names: CHILDID_SELF -> this element, an id -> its child.
  const prism::a11y::Node* Resolve(const VARIANT& varChild) const {
    if (varChild.vt != VT_I4) return nullptr;
    if (varChild.lVal == CHILDID_SELF) {
      Refresh();
      return &node_;
    }
    Refresh();
    return node_.Child(varChild.lVal);
  }

  // Paths are stored in MSAA's own id space: each step is a 1-based child id,
  // so a path can be printed, compared and walked without a second convention.
  std::vector<int> AbsolutePath(const std::vector<int>& relative) const {
    std::vector<int> result = path_;
    result.insert(result.end(), relative.begin(), relative.end());
    return result;
  }

  std::vector<int> OffsetPath(long oneBased) const {
    std::vector<int> result = path_;
    result.push_back(static_cast<int>(oneBased));
    return result;
  }

  std::atomic<ULONG> refs_{1};
  std::shared_ptr<Host> host_;
  std::vector<int> path_;
  // Mutable because every const query refreshes it: the tree is a VIEW, and a
  // view that cannot be re-read is a screenshot.
  mutable prism::a11y::Node node_;
};

// The window's WM_GETOBJECT answer. OBJID_CLIENT only: OBJID_WINDOW (the frame)
// stays with the OS's own accessible object, and OBJID_NATIVEOM belongs to
// applications that publish a native object model (we do not).
//
// Returns 0 when the message is not ours or the object cannot be created; an
// LRESULT of 0 is exactly what DefWindowProc answers, so a failure degrades to
// the pre-fix behaviour instead of crashing the client.
inline LRESULT HandleGetObject(const std::shared_ptr<Host>& host, WPARAM wparam,
                               LPARAM lparam) {
  if (!host) return 0;
  const LONG objectId = static_cast<LONG>(lparam);
  if (objectId != OBJID_CLIENT) return 0;
  Accessible* acc = Accessible::Create(host, {});
  if (acc == nullptr) return 0;
  const LRESULT result =
      ::LresultFromObject(__uuidof(IAccessible), wparam, static_cast<IAccessible*>(acc));
  acc->Release();  // LresultFromObject took its own reference
  return result;
}

}  // namespace prism::win::a11y
