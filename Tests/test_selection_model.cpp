// Card C67 -- the multi-selection rules as DATA (no window, no Windows).
//
// What is pinned here (each line is an assertion below):
//   1. a plain click REPLACES the selection;
//   2. Ctrl+click TOGGLES one row and the set stays ascending/unique;
//   3. Shift+click selects the contiguous range from the ANCHOR, and the anchor
//      does not move (a second Shift+click extends from the same place);
//   4. Ctrl+A selects every row; a click on empty space clears, unless it is a
//      Ctrl+click (cherry-picking must not be undone by a miss);
//   5. a list that shrinks drops stale rows instead of remapping them;
//   6. Size() is the ONE input of the action band's hasSelection rule.
#include <cstddef>
#include <iostream>
#include <vector>

#include "../Src/PrismCore/ActionBarModel.hpp"
#include "../Src/PrismCore/SelectionModel.hpp"
#include "mini_test.hpp"

namespace {

using prism::sel::ClickModifiers;
using prism::sel::SelectionModel;

ClickModifiers Plain() { return ClickModifiers{}; }
ClickModifiers Ctrl() { return ClickModifiers{true, false}; }
ClickModifiers Shift() { return ClickModifiers{false, true}; }

std::vector<int> Rows(const SelectionModel& model) { return model.Selected(); }

}  // namespace

PRISM_TEST("selection.plain_click_replaces_and_empty_space_clears") {
  SelectionModel model;
  model.SetCount(5);
  model.Click(2, Plain());
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(model.Focus(), 2);
  PRISM_CHECK_EQ(model.Anchor(), 2);
  model.Click(4, Plain());
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(model.Focus(), 4);
  PRISM_CHECK(model.Contains(4));
  PRISM_CHECK(!model.Contains(2));
  model.Click(-1, Plain());
  PRISM_CHECK(model.Empty());
  PRISM_CHECK_EQ(model.Focus(), -1);
}

PRISM_TEST("selection.ctrl_click_toggles_and_keeps_the_set_ordered") {
  SelectionModel model;
  model.SetCount(6);
  model.Click(4, Plain());
  model.Click(1, Ctrl());
  model.Click(3, Ctrl());
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(3));
  const std::vector<int> expected{1, 3, 4};
  PRISM_CHECK(Rows(model) == expected);  // ORDER is part of the contract
  PRISM_CHECK_EQ(model.Focus(), 3);
  PRISM_CHECK_EQ(model.Anchor(), 3);
  model.Click(3, Ctrl());  // toggling the focused row removes it
  const std::vector<int> after{1, 4};
  PRISM_CHECK(Rows(model) == after);
  // A missed Ctrl+click on empty space must not throw the cherry-pick away.
  model.Click(-1, Ctrl());
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(2));
}

PRISM_TEST("selection.shift_click_extend_from_the_anchor") {
  SelectionModel model;
  model.SetCount(8);
  model.Click(5, Plain());
  model.Click(2, Shift());  // range [2..5], anchor stays at 5
  const std::vector<int> expected{2, 3, 4, 5};
  PRISM_CHECK(Rows(model) == expected);
  PRISM_CHECK_EQ(model.Anchor(), 5);
  PRISM_CHECK_EQ(model.Focus(), 2);
  // The anchor did NOT move: the next Shift+click still measures from 5.
  model.Click(7, Shift());
  const std::vector<int> wider{5, 6, 7};
  PRISM_CHECK(Rows(model) == wider);
  // Shift after a Ctrl+click anchor is still a range, not an addition.
  model.Click(1, Ctrl());
  PRISM_CHECK_EQ(model.Anchor(), 1);
  model.Click(3, Shift());
  const std::vector<int> range{1, 2, 3};
  PRISM_CHECK(Rows(model) == range);
}

PRISM_TEST("selection.select_all_and_cleared_shapes") {
  SelectionModel model;
  model.SetCount(4);
  model.SelectAll();
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(4));
  PRISM_CHECK_EQ(model.Focus(), 3);
  PRISM_CHECK_EQ(model.Anchor(), 0);
  PRISM_CHECK(model.Contains(0));
  PRISM_CHECK(model.Contains(3));
  // The action band's hasSelection is derived from the same Size().
  prism::actionbar::Context context;
  context.archiveOpen = true;
  context.hasSelection = !model.Empty();
  prism::actionbar::Model bar;
  bar.SetContext(context);
  const auto enabled = [&bar](prism::actionbar::ActionId id) {
    return prism::actionbar::IsEnabled(id, bar.GetContext());
  };
  // Delete/QuickLook are the selection-dependent rules (ExtractTo works on the
  // whole archive even with nothing selected, by design).
  PRISM_CHECK(enabled(prism::actionbar::ActionId::Delete));
  PRISM_CHECK(enabled(prism::actionbar::ActionId::QuickLook));
  model.Clear();
  context.hasSelection = !model.Empty();
  bar.SetContext(context);
  PRISM_CHECK(!enabled(prism::actionbar::ActionId::Delete));
  PRISM_CHECK(!enabled(prism::actionbar::ActionId::QuickLook));
  PRISM_CHECK(enabled(prism::actionbar::ActionId::ExtractTo));
  PRISM_CHECK_EQ(model.Size(), static_cast<std::size_t>(0));
}

PRISM_TEST("selection.shrinking_list_drops_stale_rows") {
  SelectionModel model;
  model.SetCount(5);
  model.SetSelected({1, 2, 4});
  model.SetCount(3);
  const std::vector<int> kept{1, 2};
  PRISM_CHECK(Rows(model) == kept);
  PRISM_CHECK_EQ(model.Focus(), 2);
  model.SetCount(0);
  PRISM_CHECK(model.Empty());
  PRISM_CHECK_EQ(model.Focus(), -1);
  model.SetSingle(3);  // out of range -> cleared, never clamped to a neighbour
  PRISM_CHECK(model.Empty());
}

PRISM_TEST_MAIN()
