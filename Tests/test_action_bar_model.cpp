// PrismArchive -- PrismCore tests
// V2: the action band's DECISION layer (table, rules, groups, mnemonics,
// keyboard state machine, geometry). Pure integer/enum assertions, so the whole
// contract is pinned on Linux in milliseconds -- the Windows lane only has to
// prove that these numbers reach pixels and HWND messages.
//
// Negative controls (CONTRACTS D3), registered WILL_FAIL in v2-actions.cmake:
//   PRISM_V2_MUTANT_ALL_ENABLED        the enable matrix must go red
//   PRISM_V2_MUTANT_DUPLICATE_MNEMONIC the uniqueness case must go red
// Each mutant build also prints the active mutation, so a macro that stopped
// matching can never leave the pair green (D3's double control).
#include <algorithm>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "ActionBarModel.hpp"
#include "WindowLayout.hpp"

using namespace prism::actionbar;

namespace {

// The band is a FUNCTION of V1's window layout: these helpers make that
// visible in every assertion below instead of testing a second geometry.
prism::layout::Layout Window(int w, int h) {
  return prism::layout::Compute(w, h, /*sidebarVisible=*/false, /*listMode=*/false);
}
Layout Band(int w, int h) { return Compute(Window(w, h)); }

}  // namespace

namespace {

const Context kClosed{};                            // no archive at all
const Context kOpenNoSel{true, false, false};       // archive open, nothing selected
const Context kOpenSel{true, true, false};          // archive open, a row selected
const Context kReadOnlySel{true, true, true};       // read-only archive, row selected
const Context kReadOnlyNoSel{true, false, true};

// The matrix is written as EXPECTED VALUES, not recomputed from IsEnabled:
// a test that asks the implementation what it thinks proves nothing (D2).
struct Row {
  Context context;
  bool expected[static_cast<int>(ActionId::Count)];
};

// Order of the columns below == ActionId order.
const Row kMatrix[] = {
    //                Open  Extract Compress Add   Delete Test  View  Search
    {kClosed,         {true, false, false, false, false, false, false, false}},
    {kOpenNoSel,      {true, true,  true,  false, false, true,  false, true}},
    {kOpenSel,        {true, true,  true,  true,  true,  true,  true,  true}},
    {kReadOnlySel,    {true, true,  true,  false, false, true,  true,  true}},
    {kReadOnlyNoSel,  {true, true,  true,  false, false, true,  false, true}},
};

}  // namespace

PRISM_TEST("the table is eight actions in Bandizip's order") {
  const ActionId expected[] = {ActionId::Open,   ActionId::ExtractTo, ActionId::Compress,
                               ActionId::Add,    ActionId::Delete,    ActionId::Test,
                               ActionId::QuickLook, ActionId::Search};
  PRISM_CHECK_EQ(kActionCount, 8);
  for (int i = 0; i < kActionCount; ++i) {
    PRISM_CHECK_EQ(kActions[static_cast<std::size_t>(i)].id, expected[i]);
    PRISM_CHECK_EQ(IndexOf(expected[i]), i);
    PRISM_CHECK(!kActions[static_cast<std::size_t>(i)].label.empty());
    PRISM_CHECK(!kActions[static_cast<std::size_t>(i)].icon.empty());
  }
  // Open is FIRST: it is the only action legal with no archive, so it is the
  // one the hand must find without reading.
  PRISM_CHECK_EQ(kActions[0].id, ActionId::Open);
}

PRISM_TEST("groups are contiguous and ordered Archive -> Modify -> Inspect -> Find") {
  int last = -1;
  std::set<int> seen;
  for (int i = 0; i < kActionCount; ++i) {
    const int group = static_cast<int>(kActions[static_cast<std::size_t>(i)].group);
    PRISM_CHECK(group >= last);  // non-decreasing: a new block may not interleave
    last = group;
    seen.insert(group);
  }
  // ... and the band really uses every block (a single-group band would pass a
  // non-decreasing check vacuously).
  PRISM_CHECK_EQ(static_cast<int>(seen.size()), 4);
  PRISM_CHECK(seen.count(static_cast<int>(Group::Archive)) == 1);
  PRISM_CHECK(seen.count(static_cast<int>(Group::Modify)) == 1);
  PRISM_CHECK(seen.count(static_cast<int>(Group::Inspect)) == 1);
  PRISM_CHECK(seen.count(static_cast<int>(Group::Find)) == 1);
  // The two destructive actions are adjacent and in the Modify block.
  PRISM_CHECK_EQ(kActions[static_cast<std::size_t>(IndexOf(ActionId::Delete))].group,
                 Group::Modify);
  PRISM_CHECK_EQ(IndexOf(ActionId::Add) + 1, IndexOf(ActionId::Delete));
}

PRISM_TEST("the enable matrix is exactly the prose rules") {
  for (const Row& row : kMatrix) {
    for (int i = 0; i < kActionCount; ++i) {
      const ActionId id = kActions[static_cast<std::size_t>(i)].id;
      PRISM_CHECK_EQ(IsEnabled(id, row.context), row.expected[i]);
    }
  }
  // The three consequences written out, because they are the reason the band
  // exists at all (DESIGN.md §6 "工具栏六键"):
  PRISM_CHECK(!IsEnabled(ActionId::ExtractTo, kClosed));   // nothing to extract
  PRISM_CHECK(!IsEnabled(ActionId::Test, kClosed));        // nothing to test
  PRISM_CHECK(!IsEnabled(ActionId::QuickLook, kClosed));   // nothing to view
  PRISM_CHECK(!IsEnabled(ActionId::Delete, kOpenNoSel));   // nothing selected
  PRISM_CHECK(!IsEnabled(ActionId::Delete, kReadOnlySel)); // read-only archive
  PRISM_CHECK(!IsEnabled(ActionId::Add, kReadOnlySel));    // read-only archive
  PRISM_CHECK(IsEnabled(ActionId::Open, kClosed));         // always available
}

PRISM_TEST("mnemonics are unique, case-insensitive and round-trip") {
  std::set<char> seen;
  for (int i = 0; i < kActionCount; ++i) {
    const char mnemonic = kActions[static_cast<std::size_t>(i)].mnemonic;
    PRISM_CHECK(mnemonic >= 'A' && mnemonic <= 'Z');  // an Alt chord is a letter
    PRISM_CHECK(seen.insert(mnemonic).second);        // no duplicates
    // Both cases resolve to the same action (WM_SYSKEYDOWN sends uppercase, a
    // translation layer or a test may send lowercase).
    PRISM_CHECK_EQ(FromMnemonic(mnemonic), kActions[static_cast<std::size_t>(i)].id);
    const char lower = static_cast<char>(mnemonic - 'A' + 'a');
    PRISM_CHECK_EQ(FromMnemonic(lower), kActions[static_cast<std::size_t>(i)].id);
  }
  PRISM_CHECK_EQ(FromMnemonic('O'), ActionId::Open);
  PRISM_CHECK_EQ(FromMnemonic('E'), ActionId::ExtractTo);
  PRISM_CHECK_EQ(FromMnemonic('T'), ActionId::Test);
  PRISM_CHECK_EQ(FromMnemonic('Q'), ActionId::Count);  // unclaimed letter
  PRISM_CHECK_EQ(FromMnemonic('0'), ActionId::Count);
  PRISM_CHECK_EQ(FromMnemonic(0), ActionId::Count);
}

PRISM_TEST("a clean build carries no mutation") {
  // D3's other half: the negative-control builds print their name, and the
  // normal build must NOT (a stray define would make every WILL_FAIL green).
  PRISM_CHECK(ActiveMutation().empty());
}

PRISM_TEST("Tab walks the ring, wraps, and Enter runs the focused action") {
  Model model;
  model.SetContext(kOpenSel);
  PRISM_CHECK_EQ(model.FocusIndex(), -1);  // the grid keeps the keyboard at rest

  Effect effect = model.PressTab(true);
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::MoveFocus);
  PRISM_CHECK_EQ(model.FocusIndex(), 0);
  PRISM_CHECK_EQ(effect.action, ActionId::Open);
  for (int i = 0; i < kActionCount; ++i) model.PressTab(true);
  PRISM_CHECK_EQ(model.FocusIndex(), 0);  // wrapped
  model.PressTab(false);
  PRISM_CHECK_EQ(model.FocusIndex(), kActionCount - 1);

  // Enter on a plain action invokes it directly.
  model.SetFocusIndex(IndexOf(ActionId::Open));
  effect = model.PressEnter();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::Invoke);
  PRISM_CHECK_EQ(effect.action, ActionId::Open);
  PRISM_CHECK_EQ(effect.item, -1);
  PRISM_CHECK(effect.enabled);
}

PRISM_TEST("a disabled action consumes Enter but runs nothing") {
  Model model;
  model.SetContext(kClosed);
  const Effect effect = model.PressMnemonic('E');
  PRISM_CHECK_EQ(effect.action, ActionId::ExtractTo);  // the chord was claimed
  PRISM_CHECK(effect.Dropped());                       // ... and nothing ran
  PRISM_CHECK(!effect.enabled);
  PRISM_CHECK_EQ(model.FocusIndex(), IndexOf(ActionId::ExtractTo));  // ring still moves

  model.SetContext(kOpenSel);
  const Effect now = model.PressMnemonic('e');  // lowercase still resolves
  PRISM_CHECK_EQ(now.kind, Effect::Kind::Invoke);
  PRISM_CHECK_EQ(now.action, ActionId::ExtractTo);
}

PRISM_TEST("arrows open a dropdown and walk its rows; Esc closes it") {
  Model model;
  model.SetContext(kOpenSel);
  model.SetFocusIndex(IndexOf(ActionId::ExtractTo));
  PRISM_CHECK(!model.DropdownOpen());

  Effect effect = model.PressDown();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::OpenDropdown);
  PRISM_CHECK(model.DropdownOpen());
  PRISM_CHECK_EQ(model.DropdownOwner(), ActionId::ExtractTo);
  PRISM_CHECK_EQ(model.DropdownSize(), 3);

  effect = model.PressDown();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::MoveDropdown);
  PRISM_CHECK_EQ(model.DropdownIndex(), 1);
  model.PressDown();
  PRISM_CHECK_EQ(model.DropdownIndex(), 2);
  model.PressDown();
  PRISM_CHECK_EQ(model.DropdownIndex(), 0);  // wraps
  model.PressUp();
  PRISM_CHECK_EQ(model.DropdownIndex(), 2);  // wraps backwards

  // Enter picks the highlighted ROW, then closes the menu.
  model.PressDown();  // index 0
  effect = model.PressEnter();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::Invoke);
  PRISM_CHECK_EQ(effect.action, ActionId::ExtractTo);
  PRISM_CHECK_EQ(effect.item, 0);  // the highlighted row, captured before the close
  PRISM_CHECK(!model.DropdownOpen());

  // Esc closes without invoking; a second Esc drops the ring.
  model.SetFocusIndex(IndexOf(ActionId::Compress));
  model.PressDown();
  PRISM_CHECK(model.DropdownOpen());
  effect = model.PressEscape();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::CloseDropdown);
  PRISM_CHECK(!model.DropdownOpen());
  effect = model.PressEscape();
  PRISM_CHECK_EQ(effect.kind, Effect::Kind::MoveFocus);
  PRISM_CHECK_EQ(model.FocusIndex(), -1);
  PRISM_CHECK(model.PressEscape().Dropped());
}

PRISM_TEST("a plain action has no menu: Down declines the key") {
  Model model;
  model.SetContext(kOpenSel);
  model.SetFocusIndex(IndexOf(ActionId::Open));
  PRISM_CHECK(model.PressDown().Dropped());
  PRISM_CHECK(!model.DropdownOpen());
  // Enter still runs it -- declining the MENU must not decline the ACTION.
  PRISM_CHECK_EQ(model.PressEnter().kind, Effect::Kind::Invoke);
  // A non-dropdown action cannot open a menu even when asked directly.
  PRISM_CHECK(model.OpenDropdownFor(ActionId::Search).Dropped());
  PRISM_CHECK(!model.DropdownOpen());
}

PRISM_TEST("only ExtractTo and Compress own a dropdown") {
  for (int i = 0; i < kActionCount; ++i) {
    const Spec& spec = kActions[static_cast<std::size_t>(i)];
    const bool owns = !DropdownItems(spec.id).empty();
    PRISM_CHECK_EQ(owns, spec.hasDropdown);
  }
  // size() is size_t: comparing against a literal int trips -Werror=sign-compare
  // on the GCC lane (MSVC does not warn). Cast rather than lowering the warning.
  PRISM_CHECK_EQ(DropdownItems(ActionId::ExtractTo).size(), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(DropdownItems(ActionId::Compress).size(), static_cast<std::size_t>(3));
  // No empty labels (an empty row is an invisible click target).
  for (const DropdownItem& item : DropdownItems(ActionId::Compress)) {
    PRISM_CHECK(!item.label.empty());
  }
}

PRISM_TEST("the band IS V1's actionIsland and the pill is V1's NavPillRect") {
  const prism::layout::Layout window = Window(1280, 800);
  const Layout layout = Compute(window);
  // The band is not re-derived: it is the layout's rectangle, by reference.
  PRISM_CHECK_EQ(layout.band, window.actionIsland);
  PRISM_CHECK_EQ(layout.band, (prism::layout::Rect{0, 32, 1280, 88}));
  PRISM_CHECK_EQ(layout.band.Height(), prism::layout::kActionIslandHeight);
  PRISM_CHECK_EQ(layout.band.Height(), 56);
  PRISM_CHECK_EQ(layout.band.top, prism::layout::kTitleBarHeight);
  PRISM_CHECK_EQ(layout.band.bottom, prism::layout::kTopBandHeight);
  // The pill starts from V1's own rectangle: same left edge, same height.
  const prism::layout::Rect v1Pill = prism::layout::NavPillRect(window);
  PRISM_CHECK_EQ(layout.navPill.left, v1Pill.left);
  PRISM_CHECK_EQ(layout.navPill.top, v1Pill.top);
  PRISM_CHECK_EQ(layout.navPill.bottom, v1Pill.bottom);
  PRISM_CHECK(layout.navPill.right <= v1Pill.right);  // the toolbar may take width back
  PRISM_CHECK(layout.navPill.right > layout.navPill.left);

  // Eight buttons, each inside the band, left to right, no overlap.
  PRISM_CHECK_EQ(static_cast<int>(layout.buttons.size()), 8);
  PRISM_CHECK_EQ(layout.dropped, 0);
  PRISM_CHECK(layout.band.Contains(layout.island));
  int previousRight = layout.island.left;
  for (const Button& button : layout.buttons) {
    PRISM_CHECK(layout.band.Contains(button.rect));
    PRISM_CHECK(layout.island.Contains(button.rect));
    PRISM_CHECK(button.rect.left >= previousRight);
    PRISM_CHECK(button.rect.top == layout.band.top);
    PRISM_CHECK(button.rect.bottom == layout.band.bottom);
    previousRight = button.rect.right;
  }
  // The path pill shares the band and never overlaps the buttons (DESIGN.md §2:
  // siblings do not intersect, tolerance 0). The pill is on the LEFT (V1 put it
  // there); the actions take the row to its right.
  PRISM_CHECK(!layout.navPill.Empty());
  PRISM_CHECK(layout.band.Contains(layout.navPill));
  PRISM_CHECK(!layout.navPill.Intersects(layout.island));
  PRISM_CHECK(layout.island.left > layout.navPill.right);
  PRISM_CHECK_EQ(layout.navPill.Height(), prism::layout::kNavPillHeight);
  // ... and it is centred in the band, not glued to the top.
  PRISM_CHECK_EQ(layout.navPill.top - layout.band.top,
                 layout.band.bottom - layout.navPill.bottom);
  // Its text run is the one the window hit-tests: inside the pill, 24 px in.
  const Rect text = NavTextRect(layout);
  PRISM_CHECK(layout.navPill.Contains(text));
  PRISM_CHECK_EQ(text.left, layout.navPill.left + prism::layout::kNavTextInset);
  PRISM_CHECK_EQ(text.right, layout.navPill.right - prism::layout::kNavTextInset);
}

PRISM_TEST("all eight actions fit from 640 px up, inside layout.action") {
  // 640 is the floor the SPLIT policy guarantees: the pill gives width back
  // until the buttons reach kButtonMinWidth, and only below that does the tail
  // drop. Above 1250 the pill keeps V1's full 560 px and the buttons grow.
  const int widths[] = {640, 800, 900, 1024, 1280, 1600, 1920};
  for (const int width : widths) {
    const Layout layout = Band(width, 800);
    const prism::layout::Layout window = Window(width, 800);
    PRISM_CHECK_EQ(static_cast<int>(layout.buttons.size()), 8);
    PRISM_CHECK_EQ(layout.dropped, 0);
    for (const Button& button : layout.buttons) {
      PRISM_CHECK(layout.band.Contains(button.rect));  // inside layout.action
      PRISM_CHECK(button.rect.Width() >= kButtonMinWidth);
      PRISM_CHECK(button.rect.Width() <= kButtonMaxWidth);
    }
    PRISM_CHECK(!layout.navPill.Intersects(layout.island));
    PRISM_CHECK(layout.band.Contains(layout.navPill));
    // The island has real padding around its buttons (not a hairline).
    PRISM_CHECK(layout.island.left < layout.buttons.front().rect.left);
    PRISM_CHECK(layout.island.right > layout.buttons.back().rect.right);
    // The band split never grows the pill beyond V1's own rectangle.
    PRISM_CHECK(layout.navPill.right <= prism::layout::NavPillRect(window).right);
    PRISM_CHECK_EQ(layout.navPill.left, prism::layout::NavPillRect(window).left);
  }
  // Below the floor the tail is DROPPED, never squeezed into a smudge, and what
  // remains is still inside the band.
  const Layout narrow = Band(520, 800);
  PRISM_CHECK_EQ(narrow.dropped, kActionCount);
  PRISM_CHECK(narrow.buttons.empty());
  PRISM_CHECK(narrow.island.Empty());
  // The pill keeps V1's rect when the toolbar is not there at all.
  PRISM_CHECK_EQ(narrow.navPill, prism::layout::NavPillRect(Window(520, 800)));
  // Degenerate heights: a 30 px client has no action band to speak of, and the
  // model invents nothing (the layout's title band already eats the client).
  const Layout tiny = Band(800, 30);
  PRISM_CHECK(tiny.band.Empty());
  PRISM_CHECK(tiny.buttons.empty());
  // Zero-size: nothing is invented either.
  const Layout none = Band(0, 0);
  PRISM_CHECK(none.buttons.empty());
  PRISM_CHECK(none.island.Empty());
}

PRISM_TEST("hit testing agrees with the painted rects") {
  const Layout layout = Band(1280, 800);
  for (std::size_t i = 0; i < layout.buttons.size(); ++i) {
    const Rect& r = layout.buttons[i].rect;
    PRISM_CHECK_EQ(layout.Hit((r.left + r.right) / 2, (r.top + r.bottom) / 2),
                   static_cast<int>(i));
    PRISM_CHECK_EQ(layout.Hit(r.left, r.top), static_cast<int>(i));
    PRISM_CHECK_EQ(layout.Hit(r.right - 1, r.bottom - 1), static_cast<int>(i));
    PRISM_CHECK_EQ(layout.Hit(r.right, r.top), -1);  // one past: a gap
  }
  PRISM_CHECK_EQ(layout.Hit(10, 10), -1);   // the title bar
  PRISM_CHECK_EQ(layout.Hit(10, 200), -1);  // the content
  PRISM_CHECK_EQ(layout.Hit(layout.band.right - 2, 50), -1);  // the outer padding
}

PRISM_TEST("the dropdown panel hangs below its button") {
  const Layout layout = Band(1280, 800);
  const int extract = IndexOf(ActionId::ExtractTo);
  const Rect panel = DropdownRect(layout, extract);
  PRISM_CHECK(!panel.Empty());
  PRISM_CHECK(panel.top > layout.buttons[static_cast<std::size_t>(extract)].rect.bottom);
  PRISM_CHECK_EQ(panel.Width(), (std::max)(kDropdownMinWidth,
                                          layout.buttons[static_cast<std::size_t>(extract)]
                                              .rect.Width()));
  PRISM_CHECK_EQ(panel.Height(), 3 * kDropdownRowHeight + 2 * kDropdownPad);
  // Row mapping: the first row is inside the panel's top pad offset.
  PRISM_CHECK_EQ(DropdownRowAt(panel, panel.left + 4, panel.top + kDropdownPad + 1), 0);
  PRISM_CHECK_EQ(DropdownRowAt(panel, panel.left + 4,
                              panel.top + kDropdownPad + kDropdownRowHeight + 1),
                 1);
  PRISM_CHECK_EQ(DropdownRowAt(panel, panel.left + 4, panel.top + 1), -1);  // the pad
  PRISM_CHECK_EQ(DropdownRowAt(panel, panel.right + 4, panel.top + 20), -1);
  // A plain action has no panel at all.
  PRISM_CHECK(DropdownRect(layout, IndexOf(ActionId::Open)).Empty());
}

PRISM_TEST("contexts compare by value (the window hands one in)") {
  const Context expected{true, true, false};
  PRISM_CHECK(kOpenSel == expected);
  PRISM_CHECK(!(kOpenSel == kReadOnlySel));
  PRISM_CHECK_EQ(static_cast<int>(IsEnabled(ActionId::Open, Context{})), 1);
}

int main() {
  const int result = prism::test::RunAll(0, nullptr);
  if (result == 0) {
    std::puts("ALL ACTION-BAR MODEL CHECKS PASSED");
  } else {
    std::puts("ACTION-BAR MODEL CHECKS FAILED");
  }
  return result;
}
