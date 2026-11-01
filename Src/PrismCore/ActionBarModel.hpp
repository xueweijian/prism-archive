// PrismArchive -- PrismCore
// ActionBarModel: the action band's ONE decision layer (DESIGN.md §2/§3/§6).
//
// Why this file exists: the design spec gives the band 56 px under the 32 px
// title bar and then lists eight actions ("打开/解压/压缩/添加/删除/测试/查看/
// 搜索"). The band was empty because "which actions" and "when is this action
// legal" lived nowhere -- every call site would have re-decided it, and the
// Bandizip muscle memory would have drifted into a different set of buttons on
// every card.
//
// So this header owns, as pure integer/enum data with no Win32:
//   * the ACTION TABLE (id, group, icon id, label, Alt mnemonic, dropdown);
//   * the ENABLE RULES, one predicate per action (see RawEnabled below);
//   * the GROUPS and their order (Archive -> Modify -> Inspect -> Find);
//   * the DROPDOWN items of the two actions that have a menu;
//   * the KEYBOARD state machine (Tab / Enter / arrows / Esc / Alt mnemonic);
//   * the GEOMETRY of the band, its buttons and its dropdown panel.
//
// "Interaction like Bandizip" is a contract about ORDER and POSITION: the same
// eight actions in the same order, left-anchored under the title bar, each with
// a letter mnemonic. "Design like the blueprint" is a contract about PIXELS,
// which this header deliberately does NOT touch -- ActionBar.hpp paints it from
// the C15 design tokens (no Bandizip blue, no Bandizip icon pixels).
//
// The band shares its 56 px with the breadcrumb pill (V1's WindowLayout puts
// the nav pill inside layout.actionIsland), so the layout function also places
// that pill: actions on the left, path on the right, one gap between them and
// no overlap.
//
// B1: every min/max is parenthesised -- a consumer TU may include windows.h
//     without NOMINMAX.
// B2: this header includes what it uses (<algorithm>, <array>, <cstdint>,
//     <span>, <string_view>, <vector>).
//
// Negative controls (CONTRACTS D3; compile-time, never a text edit):
//   PRISM_V2_MUTANT_ALL_ENABLED        every action is always enabled
//   PRISM_V2_MUTANT_DUPLICATE_MNEMONIC Alt+E is claimed twice
// Each prints through ActiveMutation() so a suite can PROVE the injection was
// compiled in -- a macro that silently stopped matching would leave the mutant
// green and its WILL_FAIL registration red forever.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <span>
#include <string_view>
#include <vector>

#include "WindowLayout.hpp"  // V1: the ONE geometry authority (bands, pill, padding)

namespace prism::actionbar {

// --------------------------------------------------------------- mutations
#if defined(PRISM_V2_MUTANT_ALL_ENABLED)
inline constexpr bool kEnableRulesArmed = false;
#else
inline constexpr bool kEnableRulesArmed = true;
#endif

#if defined(PRISM_V2_MUTANT_DUPLICATE_MNEMONIC)
inline constexpr char kTestMnemonic = 'E';  // collides with ExtractTo on purpose
#else
inline constexpr char kTestMnemonic = 'T';
#endif

// Empty in a clean build. The paint suite asserts this, so a stray -D in the
// normal target can never be mistaken for a clean render.
inline constexpr std::string_view ActiveMutation() {
  if (!kEnableRulesArmed) return "all-enabled";
  if (kTestMnemonic != 'T') return "duplicate-mnemonic";
  return {};
}

// ------------------------------------------------------------------- table
enum class ActionId : uint8_t {
  Open,
  ExtractTo,
  Compress,
  Add,
  Delete,
  Test,
  QuickLook,
  Search,
  Count,
};

inline std::ostream& operator<<(std::ostream& os, ActionId id) {
  switch (id) {
    case ActionId::Open: return os << "Open";
    case ActionId::ExtractTo: return os << "ExtractTo";
    case ActionId::Compress: return os << "Compress";
    case ActionId::Add: return os << "Add";
    case ActionId::Delete: return os << "Delete";
    case ActionId::Test: return os << "Test";
    case ActionId::QuickLook: return os << "QuickLook";
    case ActionId::Search: return os << "Search";
    case ActionId::Count: return os << "Count";
  }
  return os << "?";
}

// The groups are the visual blocks of the band: everything that talks to the
// ARCHIVE, then what MODIFIES it, then what INSPECTS it, then FIND. The table
// below is sorted by this order and the suite asserts that the order is
// non-decreasing -- a new action inserted in the wrong block is a design
// regression, not a compile error.
enum class Group : uint8_t { Archive, Modify, Inspect, Find, Count };

inline std::ostream& operator<<(std::ostream& os, Group group) {
  switch (group) {
    case Group::Archive: return os << "Archive";
    case Group::Modify: return os << "Modify";
    case Group::Inspect: return os << "Inspect";
    case Group::Find: return os << "Find";
    case Group::Count: return os << "Count";
  }
  return os << "?";
}

struct Spec {
  ActionId id = ActionId::Count;
  Group group = Group::Count;
  std::string_view icon;   // vector-glyph id consumed by ActionBar.hpp
  std::string_view label;  // UTF-8, painted by DirectWrite
  char mnemonic = 0;       // Alt+<letter>
  bool hasDropdown = false;
};

// Eight actions, Bandizip's order. The labels are OURS (the blueprint's
// language), never copied from Bandizip's resource file.
inline constexpr std::array<Spec, 8> kActions{{
    {ActionId::Open, Group::Archive, "open", "打开", 'O', false},
    {ActionId::ExtractTo, Group::Archive, "extract", "解压到…", 'E', true},
    {ActionId::Compress, Group::Archive, "compress", "压缩", 'C', true},
    {ActionId::Add, Group::Modify, "add", "添加", 'A', false},
    {ActionId::Delete, Group::Modify, "delete", "删除", 'D', false},
    {ActionId::Test, Group::Inspect, "test", "测试", kTestMnemonic, false},
    {ActionId::QuickLook, Group::Inspect, "view", "查看", 'V', false},
    {ActionId::Search, Group::Find, "search", "搜索", 'S', false},
}};

inline constexpr int kActionCount = static_cast<int>(kActions.size());

inline constexpr int IndexOf(ActionId id) {
  for (int i = 0; i < kActionCount; ++i) {
    if (kActions[static_cast<std::size_t>(i)].id == id) return i;
  }
  return -1;
}

inline constexpr const Spec& SpecOf(int index) { return kActions[static_cast<std::size_t>(index)]; }

inline constexpr char ToUpperAscii(char c) {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
}

// Case-insensitive (WM_SYSKEYDOWN delivers the uppercase code; a test may not).
inline constexpr ActionId FromMnemonic(char ch) {
  const char upper = ToUpperAscii(ch);
  for (int i = 0; i < kActionCount; ++i) {
    const Spec& spec = kActions[static_cast<std::size_t>(i)];
    if (upper != 0 && spec.mnemonic == upper) return spec.id;
  }
  return ActionId::Count;
}

// ------------------------------------------------------------- enable rules
// One context, three booleans -- the whole state the band needs. It is derived
// by the window from the catalog + the navigator's selection, never passed in
// from a widget.
struct Context {
  bool archiveOpen = false;
  bool hasSelection = false;
  bool readOnly = false;

  bool operator==(const Context&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const Context& c) {
  return os << "{open=" << c.archiveOpen << " sel=" << c.hasSelection
            << " ro=" << c.readOnly << "}";
}

// THE RULES, one line of prose above each so the intent survives a refactor:
//   Open      always: a user must be able to open an archive from any state.
//   ExtractTo needs an open archive -- there is nothing to extract from else.
//   Compress  needs an open archive (it packs what the catalog shows).
//   Add       needs an open archive, a selection and a WRITABLE archive.
//   Delete    same as Add: a read-only archive must not offer a destructive
//             action that can only fail after the user confirmed it.
//   Test      needs an open archive.
//   QuickLook needs an open archive AND a selected row (it previews THAT row).
//   Search    needs an open archive: it searches the catalog, not the disk.
inline constexpr bool RawEnabled(ActionId id, const Context& c) {
  switch (id) {
    case ActionId::Open: return true;
    case ActionId::ExtractTo:
    case ActionId::Compress:
    case ActionId::Test:
    case ActionId::Search:
      return c.archiveOpen;
    case ActionId::Add:
    case ActionId::Delete:
      return c.archiveOpen && c.hasSelection && !c.readOnly;
    case ActionId::QuickLook:
      return c.archiveOpen && c.hasSelection;
    case ActionId::Count: return false;
  }
  return false;
}

// The negative control turns the RULES off, not the buttons: the suite's
// "disabled matrix" must then go red on its own (CONTRACTS D3).
inline constexpr bool IsEnabled(ActionId id, const Context& c) {
  return kEnableRulesArmed ? RawEnabled(id, c) : true;
}

// ---------------------------------------------------------------- dropdowns
struct DropdownItem {
  std::string_view label;
};

inline constexpr DropdownItem kExtractItems[] = {
    {"解压到当前文件夹"},
    {"解压到 <归档名>\\"},
    {"解压到指定文件夹…"},
};

inline constexpr DropdownItem kCompressItems[] = {
    {"压缩为 ZIP"},
    {"压缩为 7Z"},
    {"压缩到指定位置…"},
};

inline std::span<const DropdownItem> DropdownItems(ActionId id) {
  switch (id) {
    case ActionId::ExtractTo: return kExtractItems;
    case ActionId::Compress: return kCompressItems;
    default: return {};
  }
}

// ----------------------------------------------------------------- keyboard
// The model returns what the WINDOW should do; it never calls the app itself.
struct Effect {
  enum class Kind : uint8_t { None, Invoke, OpenDropdown, CloseDropdown, MoveFocus, MoveDropdown };
  Kind kind = Kind::None;
  ActionId action = ActionId::Count;
  int item = -1;         // Invoke: dropdown item (-1 == the default action)
                         // MoveFocus/MoveDropdown: the new index
  bool enabled = false;  // Invoke only: false means "the button is disabled"

  bool Dropped() const { return kind == Kind::None; }
};

inline std::ostream& operator<<(std::ostream& os, Effect::Kind kind) {
  switch (kind) {
    case Effect::Kind::None: return os << "None";
    case Effect::Kind::Invoke: return os << "Invoke";
    case Effect::Kind::OpenDropdown: return os << "OpenDropdown";
    case Effect::Kind::CloseDropdown: return os << "CloseDropdown";
    case Effect::Kind::MoveFocus: return os << "MoveFocus";
    case Effect::Kind::MoveDropdown: return os << "MoveDropdown";
  }
  return os << "?";
}

class Model {
 public:
  void SetContext(const Context& context) { context_ = context; }
  const Context& GetContext() const { return context_; }
  bool Enabled(ActionId id) const { return IsEnabled(id, context_); }

  // Focus is an index into kActions, -1 for "no focus ring" (the default: the
  // band must not steal the keyboard from the grid when the window opens).
  int FocusIndex() const { return focus_; }
  void SetFocusIndex(int index) {
    focus_ = (index >= 0 && index < kActionCount) ? index : -1;
  }
  ActionId Focused() const {
    return focus_ >= 0 ? kActions[static_cast<std::size_t>(focus_)].id : ActionId::Count;
  }
  bool FocusedEnabled() const {
    return focus_ >= 0 && Enabled(kActions[static_cast<std::size_t>(focus_)].id);
  }

  bool DropdownOpen() const { return dropdownOwner_ != ActionId::Count; }
  ActionId DropdownOwner() const { return dropdownOwner_; }
  int DropdownIndex() const { return dropdownIndex_; }
  int DropdownSize() const {
    return DropdownOpen() ? static_cast<int>(DropdownItems(dropdownOwner_).size()) : 0;
  }
  void SetDropdownIndex(int index) {
    const int size = DropdownSize();
    dropdownIndex_ = size > 0 ? ((index % size) + size) % size : 0;
  }

  // Tab cycles the focus ring (wrapping). A dropdown closes first: leaving a
  // menu floating while the ring moves elsewhere is a lie about what Enter does.
  Effect PressTab(bool forward) {
    if (DropdownOpen()) CloseDropdownInternal();
    const int next =
        focus_ < 0
            ? (forward ? 0 : kActionCount - 1)
            : ((focus_ + (forward ? 1 : -1)) % kActionCount + kActionCount) % kActionCount;
    focus_ = next;
    return Effect{Effect::Kind::MoveFocus, kActions[static_cast<std::size_t>(next)].id, next,
                  Enabled(kActions[static_cast<std::size_t>(next)].id)};
  }

  // Enter: a menu picks its highlighted row; otherwise the focused action runs.
  // A dropdown button opens its menu on the FIRST Enter (Bandizip) and runs the
  // default variant on the second -- so the arrow never hides the action.
  Effect PressEnter() {
    if (DropdownOpen()) {
      const ActionId owner = dropdownOwner_;
      const int item = dropdownIndex_;
      const bool ok = Enabled(owner);
      CloseDropdownInternal();
      return Effect{Effect::Kind::Invoke, owner, item, ok};
    }
    if (focus_ < 0) return {};
    const Spec& spec = kActions[static_cast<std::size_t>(focus_)];
    if (!Enabled(spec.id)) return Effect{Effect::Kind::None, spec.id, -1, false};
    if (spec.hasDropdown) return OpenDropdown();  // first Enter opens the menu
    return Effect{Effect::Kind::Invoke, spec.id, -1, true};
  }

  // Down: inside a menu it walks the rows; on a dropdown button it OPENS the
  // menu (requirement: "方向键展开"); anywhere else it is not ours.
  Effect PressDown() {
    if (DropdownOpen()) {
      const int size = DropdownSize();
      dropdownIndex_ = size > 0 ? (dropdownIndex_ + 1) % size : 0;
      return Effect{Effect::Kind::MoveDropdown, dropdownOwner_, dropdownIndex_, true};
    }
    if (focus_ < 0) return {};
    const Spec& spec = kActions[static_cast<std::size_t>(focus_)];
    if (!spec.hasDropdown || !Enabled(spec.id)) return {};
    return OpenDropdown();
  }

  Effect PressUp() {
    if (!DropdownOpen()) return {};
    const int size = DropdownSize();
    dropdownIndex_ = size > 0 ? (dropdownIndex_ - 1 + size) % size : 0;
    return Effect{Effect::Kind::MoveDropdown, dropdownOwner_, dropdownIndex_, true};
  }

  Effect PressEscape() {
    if (DropdownOpen()) {
      const ActionId owner = dropdownOwner_;
      CloseDropdownInternal();
      return Effect{Effect::Kind::CloseDropdown, owner, -1, true};
    }
    if (focus_ < 0) return {};
    const ActionId owner = Focused();
    focus_ = -1;
    return Effect{Effect::Kind::MoveFocus, owner, -1, true};
  }

  // Pressing an action directly (a click on its button): focus it and run its
  // DEFAULT variant -- the menu is still one Down away. A disabled button takes
  // the focus ring but runs nothing.
  Effect PressAction(ActionId id) {
    const int index = IndexOf(id);
    if (index < 0) return {};
    focus_ = index;
    if (!Enabled(id)) return Effect{Effect::Kind::None, id, -1, false};
    if (DropdownOpen() && dropdownOwner_ != id) CloseDropdownInternal();
    return Effect{Effect::Kind::Invoke, id, -1, true};
  }

  // An Alt mnemonic: the same door as a click, reached from the keyboard.
  Effect PressMnemonic(char ch) {
    const ActionId id = FromMnemonic(ch);
    if (id == ActionId::Count) return {};
    return PressAction(id);
  }

  // A click on the arrow / a menu request for a given action.
  Effect OpenDropdown() {
    if (focus_ < 0) return {};
    const Spec& spec = kActions[static_cast<std::size_t>(focus_)];
    if (!spec.hasDropdown || !Enabled(spec.id)) return {};
    dropdownOwner_ = spec.id;
    dropdownIndex_ = 0;
    return Effect{Effect::Kind::OpenDropdown, spec.id, 0, true};
  }

  Effect OpenDropdownFor(ActionId id) {
    const int index = IndexOf(id);
    if (index < 0 || !kActions[static_cast<std::size_t>(index)].hasDropdown) return {};
    focus_ = index;
    return OpenDropdown();
  }

  Effect CloseDropdown() {
    if (!DropdownOpen()) return {};
    const ActionId owner = dropdownOwner_;
    CloseDropdownInternal();
    return Effect{Effect::Kind::CloseDropdown, owner, -1, true};
  }

 private:
  void CloseDropdownInternal() {
    dropdownOwner_ = ActionId::Count;
    dropdownIndex_ = 0;
  }

  Context context_;
  int focus_ = -1;
  ActionId dropdownOwner_ = ActionId::Count;
  int dropdownIndex_ = 0;
};

// ----------------------------------------------------------------- geometry
// Integer rects, no D2D types: the same numbers are asserted on Linux (pure)
// and used on Windows (paint + hit test), so a paint bug and a hit-test bug
// cannot disagree about where a button is.
// ONE rect type for the whole product: prism::layout::Rect (WindowLayout.hpp).
// A second struct here would be a second source of "what a rectangle is", and
// the paint layer, the hit test and V1's bands would drift apart one rounding
// at a time.
using Rect = prism::layout::Rect;

// DESIGN.md §2: title 32 | action island 56. These are the SAME numbers V1's
// WindowLayout publishes as kTitleBarHeight / kActionIslandHeight; when that
// header lands the band rectangle can be handed in instead of computed, and
// every other rule here keeps working unchanged.
// Geometry is NOT re-declared here. The band (title 32 / action 56), the 16 px
// content padding and the breadcrumb pill all come from prism::layout
// (WindowLayout.hpp) -- the ONE geometry authority, asserted by V1's suite.
// What this file owns is the INTERNAL rhythm of the action island (button
// width, gaps, icon box) and the dropdown panel: things that cannot shadow a
// band.
inline constexpr int kButtonGap = 4;  // spacing scale: 4
inline constexpr int kIslandPad = 8;
inline constexpr int kButtonMinWidth = 64;
inline constexpr int kButtonMaxWidth = 88;
inline constexpr int kIconBox = 20;
inline constexpr int kNavGap = 12;  // air between the path pill and the actions
inline constexpr int kDropdownGap = 4;
inline constexpr int kDropdownRowHeight = 28;
inline constexpr int kDropdownPad = 6;
inline constexpr int kDropdownMinWidth = 180;

struct Button {
  ActionId id = ActionId::Count;
  Rect rect;
  int group = 0;
};

struct Layout {
  Rect band;                    // == prism::layout::Layout::actionIsland
  Rect island;                  // the floating glass pill the buttons live in
  std::vector<Button> buttons;  // left to right; the "dropped" ones did not fit
  int dropped = 0;
  Rect navPill;                 // == prism::layout::NavPillRect (V1's pill)

  // Index into buttons, or -1.
  int Hit(int x, int y) const {
    for (std::size_t i = 0; i < buttons.size(); ++i) {
      const Rect& r = buttons[i].rect;
      if (x >= r.left && x < r.right && y >= r.top && y < r.bottom) return static_cast<int>(i);
    }
    return -1;
  }
};

// The band comes IN as V1's layout: this function decides only how the 56 px
// row is SHARED, never where the row is. The breadcrumb pill keeps V1's own
// rectangle (NavPillRect) -- it is painted there by the renderer and hit-tested
// there by the window -- and the actions take the space to its right, left to
// right in table order.
//
// Consequences that are asserted, not hoped for:
//   * every button is inside layout.actionIsland (tolerance 0);
//   * the island never intersects the pill;
//   * a band that cannot hold all eight buttons DROPS the tail (the alternative
//     -- squeezing below kButtonMinWidth -- clips the label mid-glyph).
inline Layout Compute(const prism::layout::Layout& window) {
  Layout out;
  out.band = window.actionIsland;
  out.navPill = prism::layout::NavPillRect(window);
  if (out.band.Empty()) return out;

  const int inset = prism::layout::kContentPadding;
  const int limit = out.band.right - inset;
  const int gaps = (kActionCount - 1) * kButtonGap;
  // The eight buttons at their floor, plus the island's own padding: what the
  // band MUST leave free if the toolbar is to be complete.
  const int actionFloor = 2 * kIslandPad + kActionCount * kButtonMinWidth + gaps;

  // Split the band. The pill is V1's rectangle until the actions need the room;
  // then the pill gives width back (its right edge moves left, its left edge and
  // its height stay V1's). A path display truncates gracefully; a toolbar that
  // silently loses its last three actions does not.
  if (!out.navPill.Empty()) {
    const int maxPillRight = limit - actionFloor - kNavGap;
    if (maxPillRight >= out.navPill.left) {
      out.navPill.right = (std::min)(out.navPill.right, maxPillRight);
    }
  }

  int left = out.band.left + inset;
  if (!out.navPill.Empty()) left = out.navPill.right + kNavGap;
  const int available = limit - left;
  if (available < actionFloor) {
    out.dropped = kActionCount;  // the band cannot hold a usable toolbar at all
    out.island = Rect{};
    return out;
  }

  int buttonWidth = (available - 2 * kIslandPad - gaps) / kActionCount;
  buttonWidth = (std::max)(kButtonMinWidth, (std::min)(buttonWidth, kButtonMaxWidth));

  int x = left + kIslandPad;
  for (int i = 0; i < kActionCount; ++i) {
    const Rect r{x, out.band.top, x + buttonWidth, out.band.bottom};
    if (r.right > limit) {
      ++out.dropped;
    } else {
      out.buttons.push_back(
          Button{kActions[static_cast<std::size_t>(i)].id, r,
                 static_cast<int>(kActions[static_cast<std::size_t>(i)].group)});
    }
    x += buttonWidth + kButtonGap;
  }
  if (out.buttons.empty()) return out;

  out.island =
      Rect{left, out.band.top, out.buttons.back().rect.right + kIslandPad, out.band.bottom};
  return out;
}

// The dropdown panel of one button: below it, at least kDropdownMinWidth wide.
inline Rect DropdownRect(const Layout& layout, int buttonIndex) {
  if (buttonIndex < 0 || buttonIndex >= static_cast<int>(layout.buttons.size())) return {};
  const Button& button = layout.buttons[static_cast<std::size_t>(buttonIndex)];
  const int rows = static_cast<int>(DropdownItems(button.id).size());
  if (rows <= 0) return {};
  const int width = (std::max)(button.rect.Width(), kDropdownMinWidth);
  const int height = rows * kDropdownRowHeight + 2 * kDropdownPad;
  return Rect{button.rect.left, button.rect.bottom + kDropdownGap, button.rect.left + width,
              button.rect.bottom + kDropdownGap + height};
}

// The text run inside the SPLIT pill (the pill may be narrower than V1's, so
// its text rect cannot come from layout::NavTextRect -- that one reads
// NavPillRect). Same 24 px inset and same 10 px vertical inset as V1.
inline Rect NavTextRect(const Layout& bar) {
  if (bar.navPill.Empty()) return Rect{};
  return Rect{bar.navPill.left + prism::layout::kNavTextInset, bar.navPill.top + 10,
              bar.navPill.right - prism::layout::kNavTextInset, bar.navPill.bottom - 10};
}

inline int DropdownRowAt(const Rect& panel, int x, int y) {
  if (panel.Empty()) return -1;
  if (x < panel.left || x >= panel.right || y < panel.top || y >= panel.bottom) return -1;
  const int local = y - panel.top - kDropdownPad;
  if (local < 0) return -1;
  return local / kDropdownRowHeight;
}

}  // namespace prism::actionbar
