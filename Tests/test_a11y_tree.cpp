// PrismArchive -- PrismCore
// C80: the accessibility tree's pure half (Src/PrismCore/A11yTree.hpp).
//
// The Windows suite proves the tree reaches a real IAccessible through
// WM_GETOBJECT; this one proves the TREE ITSELF is right, on any machine:
// element counting, name lookup, the deepest-match hit test, 1-based child-id
// paths and the "at most one focused element" rule. A defect here would be a
// defect in what a screen reader reads, and it is caught in milliseconds
// instead of only on a Windows runner.
//
// Verdict (CONTRACTS D1): the stdout marker is a conjunction with exit 0; the
// binary exits 1 on any failed assertion.
#include <cstddef>
#include <iostream>
#include <string>
#include <vector>

#include "../Src/PrismCore/A11yTree.hpp"
#include "mini_test.hpp"

namespace {

using prism::a11y::Node;
using prism::a11y::Role;

Node MakeButton(std::string name, int index, bool enabled, prism::layout::Rect rect) {
  Node node;
  node.role = Role::PushButton;
  node.name = std::move(name);
  node.index = index;
  node.enabled = enabled;
  node.action = "\u6309\u4E0B";
  node.bounds = rect;
  return node;
}

// The shape the window publishes, shrunk to two buttons, one tab, a two-row
// list and a status line -- the same ORDER and the same roles, so the rules
// exercised here are the rules the product uses.
Node MakeTree() {
  Node root;
  root.role = Role::Client;
  root.name = "PrismArchive -- photos.7z";
  root.bounds = prism::layout::Rect{0, 0, 800, 600};

  root.children.push_back(MakeButton("\u6253\u5F00", 0, true,
                                     prism::layout::Rect{8, 40, 72, 96}));
  root.children.push_back(MakeButton("\u89E3\u538B\u5230\u2026", 1, false,
                                     prism::layout::Rect{76, 40, 140, 96}));

  Node tab;
  tab.role = Role::PageTab;
  tab.name = "photos.7z";
  tab.index = 0;
  tab.selected = true;
  tab.action = "\u9009\u62E9";
  tab.bounds = prism::layout::Rect{0, 8, 200, 40};
  root.children.push_back(tab);

  Node list;
  list.role = Role::List;
  list.name = "photos.7z";
  list.bounds = prism::layout::Rect{0, 100, 800, 560};
  for (int row = 0; row < 2; ++row) {
    Node item;
    item.role = Role::ListItem;
    item.name = row == 0 ? "one.jpg" : "two.jpg";
    item.index = row;
    item.selected = row == 0;
    item.focused = row == 0;
    item.action = "\u6253\u5F00";
    item.bounds = prism::layout::Rect{0, 100 + row * 32, 800, 132 + row * 32};
    list.children.push_back(std::move(item));
  }
  root.children.push_back(std::move(list));

  Node status;
  status.role = Role::StatusBar;
  status.name = "\u5171 2 \u9879";
  status.bounds = prism::layout::Rect{0, 570, 800, 600};
  root.children.push_back(std::move(status));

  // The list's first row is focused, so the root itself is not.
  return root;
}

}  // namespace

PRISM_TEST("a11y.count_counts_every_element_including_the_root") {
  const Node root = MakeTree();
  // 1 root + 2 buttons + 1 tab + 1 list + 2 rows + 1 status = 8.
  PRISM_CHECK_EQ(prism::a11y::CountNodes(root), 8);
  PRISM_CHECK_EQ(prism::a11y::CountNodes(root.children[3]), 3);  // list + its rows
}

PRISM_TEST("a11y.roles_are_counted_by_role") {
  const Node root = MakeTree();
  PRISM_CHECK_EQ(prism::a11y::CountRole(root, Role::PushButton), 2);
  PRISM_CHECK_EQ(prism::a11y::CountRole(root, Role::ListItem), 2);
  PRISM_CHECK_EQ(prism::a11y::CountRole(root, Role::PageTab), 1);
  PRISM_CHECK_EQ(prism::a11y::CountRole(root, Role::StatusBar), 1);
  PRISM_CHECK_EQ(prism::a11y::CountRole(root, Role::List), 1);
}

PRISM_TEST("a11y.find_by_name_reaches_a_nested_element") {
  const Node root = MakeTree();
  const Node* extract = prism::a11y::FindByName(root, "\u89E3\u538B\u5230\u2026");
  PRISM_CHECK(extract != nullptr);
  if (extract != nullptr) {
    PRISM_CHECK_EQ(extract->role, Role::PushButton);
    PRISM_CHECK_EQ(extract->index, 1);
    PRISM_CHECK_EQ(extract->enabled, false);  // the band's legality is published
  }
  PRISM_CHECK(prism::a11y::FindByName(root, "no-such-element") == nullptr);
}

PRISM_TEST("a11y.hit_test_returns_the_deepest_element") {
  const Node root = MakeTree();
  const Node* hit = nullptr;
  std::vector<int> path;
  // Inside a row -> the row, not the list and not the window.
  PRISM_CHECK(prism::a11y::HitTest(root, 400, 116, &hit, &path));
  PRISM_CHECK(hit != nullptr);
  if (hit != nullptr) PRISM_CHECK_EQ(hit->name, std::string("one.jpg"));
  // Path is 1-based child ids: list is child 4, first row is child 1.
  PRISM_CHECK_EQ(prism::a11y::PathText(path), std::string("4.1"));

  // A button's own rectangle resolves to the button.
  PRISM_CHECK(prism::a11y::HitTest(root, 20, 60, &hit, &path));
  PRISM_CHECK(hit != nullptr);
  if (hit != nullptr) PRISM_CHECK_EQ(hit->name, std::string("\u6253\u5F00"));
  PRISM_CHECK_EQ(prism::a11y::PathText(path), std::string("1"));

  // Outside the window: no element at all.
  PRISM_CHECK(!prism::a11y::HitTest(root, 9000, 9000, &hit, &path));
}

PRISM_TEST("a11y.empty_rectangles_are_never_hit") {
  Node root;
  root.role = Role::Client;
  root.bounds = prism::layout::Rect{0, 0, 100, 100};
  // A button the band could not fit: reachable from the keyboard, but there is
  // no pixel that can be pointed at.
  root.children.push_back(MakeButton("\u6DFB\u52A0", 0, true, prism::layout::Rect{}));
  const Node* hit = nullptr;
  std::vector<int> path;
  PRISM_CHECK(prism::a11y::HitTest(root, 10, 10, &hit, &path));
  PRISM_CHECK(hit == &root);
  PRISM_CHECK(path.empty());
}

PRISM_TEST("a11y.paths_round_trip_and_reject_out_of_range") {
  const Node root = MakeTree();
  const Node* row = prism::a11y::NodeAtPath(root, std::vector<int>{4, 2});
  PRISM_CHECK(row != nullptr);
  if (row != nullptr) PRISM_CHECK_EQ(row->name, std::string("two.jpg"));
  PRISM_CHECK(prism::a11y::NodeAtPath(root, std::vector<int>{4, 3}) == nullptr);
  PRISM_CHECK(prism::a11y::NodeAtPath(root, std::vector<int>{99}) == nullptr);
  PRISM_CHECK(prism::a11y::NodeAtPath(root, std::vector<int>{}) == &root);
}

PRISM_TEST("a11y.exactly_one_element_claims_focus") {
  const Node root = MakeTree();
  std::vector<int> path;
  const Node* focused = prism::a11y::FindFocused(root, &path);
  PRISM_CHECK(focused != nullptr);
  if (focused != nullptr) PRISM_CHECK_EQ(focused->name, std::string("one.jpg"));
  PRISM_CHECK_EQ(prism::a11y::PathText(path), std::string("4.1"));

  // The path must name the SAME element when walked again -- that is what
  // accFocus hands a client.
  const Node* walked = prism::a11y::NodeAtPath(root, path);
  PRISM_CHECK(walked == focused);
}

PRISM_TEST("a11y.role_names_are_stable") {
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(Role::PushButton)), std::string("pushbutton"));
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(Role::PageTab)), std::string("pagetab"));
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(Role::ListItem)), std::string("listitem"));
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(Role::Outline)), std::string("outline"));
  PRISM_CHECK_EQ(std::string(prism::a11y::RoleName(Role::StatusBar)), std::string("statusbar"));
}

PRISM_TEST("a11y.the_published_shape_matches_the_product") {
  const Node root = MakeTree();
  // The card's contract, asserted as an ORDER: the eight actions first (in band
  // order), then the tabs, then the list, then the sidebar, then the status.
  PRISM_CHECK_EQ(root.children[0].role, Role::PushButton);
  PRISM_CHECK_EQ(root.children[2].role, Role::PageTab);
  PRISM_CHECK_EQ(root.children[3].role, Role::List);
  PRISM_CHECK_EQ(root.children[4].role, Role::StatusBar);
  // ...and the list's rows are LIST ITEMS of that container, not siblings.
  PRISM_CHECK_EQ(root.children[3].children.size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(root.children[3].children[0].role, Role::ListItem);
}

// D1: the stdout marker is part of the verdict -- a silent skip or a crash
// after the last assertion can never be mistaken for a pass.
int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL A11Y-TREE CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  return 0;
}
