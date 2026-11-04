// PrismArchive -- PrismCore
// A11yTree: the accessibility tree the WINDOW PUBLISHES (card C80).
//
// THE DEFECT IT CLOSES. Every pixel of the client area is ours (D2D): the eight
// action buttons, the tab strip, the rows, the sidebar and the status line are
// painted, not created as child HWNDs. A screen reader therefore sees NOTHING
// below the menu bar -- measured with a UIA client against the shipped window:
// nine elements, all of them the OS title bar and menu bar. A user who cannot
// see the pixels cannot use the application at all, and an automation suite can
// only assert pixels, which is slow and brittle.
//
// This header is the DATA layer that fixes it, and it is deliberately PURE:
//   * no <windows.h>, no COM, no IAccessible -- Src/PrismWin/Accessible.hpp is
//     the bridge that turns these nodes into MSAA objects (and therefore into
//     UIA elements through the MSAA proxy);
//   * UTF-8 strings and integer rectangles, so the whole tree is buildable and
//     assertable on the Linux lane in milliseconds.
//
// The split matters for testability (CONTRACTS D1/D3): a bug in "which children
// does the window expose" is caught by core.a11y_tree without a window station,
// and the Windows suite only has to prove that the SAME tree reaches a real
// IAccessible through WM_GETOBJECT.
//
// ROLES. The values are OUR enum, not the ROLE_SYSTEM_* constants, for the same
// reason the band's geometry is not D2D types: PrismCore may not include
// Windows. PrismWin maps them one-to-one (RoleToMsaa in Accessible.hpp), and
// the Windows test asserts the mapping by reading accRole back.
//
// NEGATIVE CONTROL (CONTRACTS D3): PRISM_C80_MUTANT_NO_GETOBJECT lives in
// PrismWin/Accessible.hpp where WM_GETOBJECT is answered; a tree that is built
// but never handed to a client is the exact pre-fix state, and the suite must
// go red on "childCount == 0".
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "WindowLayout.hpp"  // prism::layout::Rect -- ONE rectangle type (B1)

namespace prism::a11y {

// What an element IS, in MSAA terms (the Win layer maps; see the header note).
enum class Role : std::uint8_t {
  Client,      // ROLE_SYSTEM_CLIENT      -- the window's client area
  PushButton,  // ROLE_SYSTEM_PUSHBUTTON  -- one of the eight actions
  PageTab,     // ROLE_SYSTEM_PAGETAB     -- one open archive
  List,        // ROLE_SYSTEM_LIST        -- the rows of the current folder
  ListItem,    // ROLE_SYSTEM_LISTITEM    -- one file/folder row
  Outline,     // ROLE_SYSTEM_OUTLINE     -- the sidebar's archive tree
  OutlineItem, // ROLE_SYSTEM_OUTLINEITEM -- one folder in it
  StatusBar,   // ROLE_SYSTEM_STATUSBAR   -- the status line
  Text,        // ROLE_SYSTEM_TEXT        -- the search field's visible text
  Group,       // ROLE_SYSTEM_GROUPING    -- a named container
  // C87: the live progress capsule. A screen reader must hear the operation and
  // its percent WHILE it runs; the element exists only while the HUD is on
  // screen, so "finished" and "no element" are the same fact.
  ProgressBar, // ROLE_SYSTEM_PROGRESSBAR -- the HUD capsule
  // C83: the menu bar is owner-drawn now, and an owner-drawn item has no OS
  // string -- so the window publishes the bar itself (see GalleryWindow's
  // OBJID_MENU answer). Without these two roles the four items would reach a
  // screen reader as unnamed menu items, which is the regression the card
  // forbids.
  MenuBar,     // ROLE_SYSTEM_MENUBAR
  MenuItem,    // ROLE_SYSTEM_MENUITEM
  // C85: the x on a tab. It is a BUTTON to the client (ROLE_SYSTEM_PUSHBUTTON,
  // so UIA exposes InvokePattern), but it is NOT one of the eight band actions:
  // its index names a TAB, and its default action closes that tab. Folding it
  // into PushButton would make accDoDefaultAction press the wrong thing.
  CloseButton,
};

// Stable machine spelling (stderr, test output, the snapshot script). Same
// shape as prism::actionbar::ActionName: an enum without a printed name is a
// debugging tax nobody pays twice.
inline const char* RoleName(Role role) {
  switch (role) {
    case Role::Client: return "client";
    case Role::PushButton: return "pushbutton";
    case Role::PageTab: return "pagetab";
    case Role::List: return "list";
    case Role::ListItem: return "listitem";
    case Role::Outline: return "outline";
    case Role::OutlineItem: return "outlineitem";
    case Role::StatusBar: return "statusbar";
    case Role::Text: return "text";
    case Role::Group: return "group";
    case Role::ProgressBar: return "progressbar";
    case Role::MenuBar: return "menubar";
    case Role::MenuItem: return "menuitem";
    case Role::CloseButton: return "closebutton";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Role role) {
  return os << RoleName(role);
}

// One published element. Everything an IAccessible has to answer EXCEPT the
// live handles lives here, so a test can assert exactly what a screen reader
// would be told.
struct Node {
  Role role = Role::Group;
  std::string name;         // accName: the words a screen reader reads
  std::string value;        // accValue (status text, counters); "" == none
  std::string description; // accDescription: state a role cannot express
  std::string action;      // accDefaultAction verb ("" == not invokable)
  std::string shortcut;    // accKeyboardShortcut, e.g. "Alt+S"
  std::string key;         // opaque payload for the handler (folder path)
  prism::layout::Rect bounds{};  // CLIENT-relative; empty == not on screen

  // Which thing this element stands for, per role:
  //   PushButton  -> index into prism::actionbar::kActions
  //   PageTab     -> tab index;  ListItem -> folder row; OutlineItem -> row
  //   CloseButton -> tab index (its default action closes that tab, C85)
  int index = -1;

  bool enabled = true;
  bool selected = false;
  bool focused = false;
  bool expandable = false;
  bool expanded = false;
  int level = 0;  // OutlineItem nesting depth

  std::vector<Node> children;

  const Node* Child(int oneBased) const {
    if (oneBased < 1 || static_cast<std::size_t>(oneBased) > children.size()) return nullptr;
    return &children[static_cast<std::size_t>(oneBased - 1)];
  }
};

// ------------------------------------------------------------------ queries
// Every helper takes the node to answer FROM, so the same function serves the
// window root and any element a client navigated into.

// Total elements in the subtree (root included). This is the number the card's
// "at least 20 elements" assertion counts, and it is the number the MSAA client
// can reach by walking get_accChildCount/get_accChild.
inline int CountNodes(const Node& root) {
  int count = 1;
  for (const Node& child : root.children) count += CountNodes(child);
  return count;
}

// Depth-first, document order. The order is the order the tree is built in --
// NOT sorted -- because "the button order is the band order" is a product rule.
inline void CollectPreorder(const Node& node, std::vector<const Node*>* out) {
  if (out == nullptr) return;
  out->push_back(&node);
  for (const Node& child : node.children) CollectPreorder(child, out);
}

inline std::vector<const Node*> Preorder(const Node& root) {
  std::vector<const Node*> out;
  out.reserve(static_cast<std::size_t>(CountNodes(root)));
  CollectPreorder(root, &out);
  return out;
}

inline const Node* FindByName(const Node& root, std::string_view name) {
  if (root.name == name) return &root;
  for (const Node& child : root.children) {
    if (const Node* hit = FindByName(child, name)) return hit;
  }
  return nullptr;
}

inline const Node* FindByRole(const Node& root, Role role) {
  if (root.role == role) return &root;
  for (const Node& child : root.children) {
    if (const Node* hit = FindByRole(child, role)) return hit;
  }
  return nullptr;
}

inline int CountRole(const Node& root, Role role) {
  int count = root.role == role ? 1 : 0;
  for (const Node& child : root.children) count += CountRole(child, role);
  return count;
}

// The element at a CLIENT-relative point (accHitTest's whole job): the deepest
// node whose bounds contain it. An empty (dropped) rectangle never matches --
// a button the band could not fit is reachable from the keyboard, not from a
// pixel that does not exist.
inline bool HitTest(const Node& node, int x, int y, const Node** out,
                    std::vector<int>* path) {
  if (node.bounds.Empty()) return false;
  if (x < node.bounds.left || x >= node.bounds.right || y < node.bounds.top ||
      y >= node.bounds.bottom) {
    return false;
  }
  if (out != nullptr) *out = &node;
  if (path != nullptr) path->clear();
  // Children are drawn (and live) above their parent: the deepest match wins.
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    std::vector<int> childPath;
    const Node* hit = nullptr;
    if (HitTest(node.children[i], x, y, &hit, &childPath)) {
      if (out != nullptr) *out = hit;
      if (path != nullptr) {
        path->clear();
        path->push_back(static_cast<int>(i) + 1);  // MSAA child ids are 1-based
        path->insert(path->end(), childPath.begin(), childPath.end());
      }
      return true;
    }
  }
  return true;
}

// The node a child-id path selects. 1-based ids, exactly the ids MSAA clients
// pass in accChild/accName: {1} is the first child of the root. An out-of-range
// step answers nullptr instead of throwing, because a stale client must get an
// empty answer, never a crash.
inline const Node* NodeAtPath(const Node& root, const std::vector<int>& path) {
  const Node* node = &root;
  for (const int step : path) {
    const Node* next = node->Child(step);
    if (next == nullptr) return nullptr;
    node = next;
  }
  return node;
}

// The focused element in this subtree, as a path RELATIVE to 'node' (empty
// when 'node' itself is the focused one). MSAA's accFocus must name one element
// or answer "nothing"; a tree where two elements claim focus is a lie the
// client cannot resolve.
inline const Node* FindFocused(const Node& node, std::vector<int>* path) {
  if (node.focused) {
    if (path != nullptr) path->clear();
    return &node;
  }
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    std::vector<int> sub;
    const Node* hit = FindFocused(node.children[i], &sub);
    if (hit == nullptr) continue;
    if (path != nullptr) {
      path->clear();
      path->push_back(static_cast<int>(i) + 1);  // MSAA child ids are 1-based
      path->insert(path->end(), sub.begin(), sub.end());
    }
    return hit;
  }
  return nullptr;
}

// A child-id path rendered for diagnostics ("1.3.2"); the snapshot script and
// the Windows suite print it so a wrong element is identifiable from a log.
inline std::string PathText(const std::vector<int>& path) {
  if (path.empty()) return "self";
  std::string out;
  for (const int step : path) {
    if (!out.empty()) out.push_back('.');
    out += std::to_string(step);
  }
  return out;
}

}  // namespace prism::a11y
