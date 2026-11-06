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
#include <string>
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

// The stable machine name (stderr/tests). Same spelling as operator<<.
inline const char* ActionName(ActionId id) {
  switch (id) {
    case ActionId::Open: return "Open";
    case ActionId::ExtractTo: return "ExtractTo";
    case ActionId::Compress: return "Compress";
    case ActionId::Add: return "Add";
    case ActionId::Delete: return "Delete";
    case ActionId::Test: return "Test";
    case ActionId::QuickLook: return "QuickLook";
    case ActionId::Search: return "Search";
    case ActionId::Count: return "Count";
  }
  return "?";
}

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
  std::string_view icon;  // vector-glyph id consumed by ActionBar.hpp
  // C74: the RESOURCE KEY, not the word. The table used to carry compiled-in
  // Chinese, so the band stayed Chinese after a language switch. The caller now
  // injects the active language's eight labels (SetLabels below); PrismCore may
  // not include the translator (ARCH rule) or Windows.
  std::string_view key;
  char mnemonic = 0;  // Alt+<letter>
  bool hasDropdown = false;
};

// Eight actions, Bandizip's order. The keys are OURS (the blueprint's
// language), never copied from Bandizip's resource file.
inline constexpr std::array<Spec, 8> kActions{{
    {ActionId::Open, Group::Archive, "open", "actionbar.open", 'O', false},
    {ActionId::ExtractTo, Group::Archive, "extract", "actionbar.extractTo", 'E', true},
    {ActionId::Compress, Group::Archive, "compress", "actionbar.compress", 'C', true},
    {ActionId::Add, Group::Modify, "add", "actionbar.add", 'A', false},
    {ActionId::Delete, Group::Modify, "delete", "actionbar.delete", 'D', false},
    {ActionId::Test, Group::Inspect, "test", "actionbar.test", kTestMnemonic, false},
    {ActionId::QuickLook, Group::Inspect, "view", "actionbar.quickLook", 'V', false},
    {ActionId::Search, Group::Find, "search", "actionbar.search", 'S', false},
}};

// The words the band shipped with before the keys existed. This is the LAST
// fallback, used only when the caller injects nothing (unit suites, the shell
// DLLs, a table-less build): an empty button is worse than a built-in word.
inline const std::array<std::string, 8>& DefaultLabels() {
  static const std::array<std::string, 8> labels = {"打开", "解压到…", "压缩",
                                                    "添加", "删除",   "测试",
                                                    "查看", "搜索"};
  return labels;
}

// C74 negative control (CONTRACTS D3). In this build SetLabels() is ignored and
// every lookup returns the built-in word, i.e. the pre-C74 band. The label suite
// MUST go red; Tests/suites/c74-i18nlast.cmake builds exactly that binary.
#if defined(PRISM_C74_MUTANT_FIXED_LABELS)
// MUTANT ACTIVE (C74 arm fixed_labels): the injected table never reaches a
// button, so switching the language leaves the band in Chinese.
inline constexpr bool kLabelInjectionEnabled = false;
#else
inline constexpr bool kLabelInjectionEnabled = true;
#endif

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

  // ------------------------------------------------------------- labels (C74)
  // The table carries KEYS; the caller injects the eight words of the active
  // language. A wrong-sized table is REJECTED (ignored) instead of half-applied:
  // a band with seven translated buttons and one stale word is worse than the
  // built-in default. No translator is included here by design.
  void SetLabels(std::span<const std::string> labels) {
    if (labels.size() != static_cast<std::size_t>(kActionCount)) {
      labels_.clear();
      return;
    }
    labels_.assign(labels.begin(), labels.end());
  }
  void ClearLabels() { labels_.clear(); }
  bool HasInjectedLabels() const { return !labels_.empty(); }

  // Never empty, never throws: index out of range falls back to the first
  // built-in word rather than reading past the table.
  const std::string& Label(int index) const {
    const std::size_t clamped =
        (index >= 0 && index < kActionCount) ? static_cast<std::size_t>(index) : 0;
    if (kLabelInjectionEnabled &&
        labels_.size() == static_cast<std::size_t>(kActionCount)) {
      return labels_[clamped];
    }
    return DefaultLabels()[clamped];
  }
  const std::string& LabelOf(ActionId id) const { return Label(IndexOf(id)); }

 private:
  void CloseDropdownInternal() {
    dropdownOwner_ = ActionId::Count;
    dropdownIndex_ = 0;
  }

  Context context_;
  int focus_ = -1;
  ActionId dropdownOwner_ = ActionId::Count;
  int dropdownIndex_ = 0;
  // Empty == "no table injected": Label() serves the built-in words.
  std::vector<std::string> labels_;
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
// C78: the band has a RHYTHM, not one uniform gap. Inside a Group the buttons
// sit 4 px apart; between two Groups the gap is 12 px and a 1 px hairline marks
// the seam. That measured strip is what makes "Archive | Modify | Inspect |
// Find" legible without drawing a box around each block.
inline constexpr int kButtonGap = 4;   // spacing scale: 4 (intra-group)
inline constexpr int kGroupGap = 12;   // spacing scale: 12 (inter-group)
// C78: the group separator hairline. 8% white is a seam, not a border -- it
// must be findable at 100% but invisible at a glance. The pixel assertion
// recovers the alpha from the rendered row and requires 0.06..0.10.
inline constexpr float kGroupSeparatorAlpha = 0.08f;
inline constexpr int kGroupSeparatorInset = 14;  // vertical air above/below
inline constexpr int kIslandPad = 8;
inline constexpr int kButtonMinWidth = 64;
inline constexpr int kButtonMaxWidth = 88;
// C78: 24 px glyph, 11 px label, 6 px above and below the content stack. The
// old 20 px box read as a 16 px icon after antialiasing inside a 56 px band.
inline constexpr int kIconBox = 24;
// C115: the gap is part of the SPLIT rule now and lives in WindowLayout with it;
// this alias keeps every existing call site (and the band's own arithmetic)
// reading the one number.
inline constexpr int kNavGap = prism::layout::kNavActionGap;  // 12 px air
inline constexpr int kDropdownGap = 4;
inline constexpr int kDropdownRowHeight = 28;
inline constexpr int kDropdownPad = 6;
inline constexpr int kDropdownMinWidth = 180;

// ------------------------------------------------------- V4: icon + label boxes
// The v2 screenshot showed the glyph sitting high and the label clipped mid
// glyph. The cause was that neither box existed: the painter placed the icon at
// "button.top + 9" and the text in "button.top + 32 .. button.bottom - 2", two
// hand-tuned constants with no relationship to each other.
//
// V4 gives each button two NAMED boxes and pins the relation:
//   * the icon box is kIconBox square, horizontally centred, kIconTop below the
//     button's top edge;
//   * the label box starts kLabelGap under it and is exactly kLabelHeight tall,
//     spanning the button's full width;
//   * therefore iconCentre == labelCentre - (0, kIconLabelOffset): one fixed
//     pixel offset, asserted by the paint suite instead of eyeballed.
// The label is kButtonMinWidth wide at its narrowest, which is the width the
// shortest usable label (64 px) needs; anything longer is ELLIPSIZED (DWrite
// trimming with U+2026), never clipped mid-glyph.
inline constexpr int kIconTop = 6;       // 6 px above the glyph
inline constexpr int kLabelGap = 6;      // 6 px between glyph and word
inline constexpr int kLabelHeight = 14;  // an 11 px run needs 14 px of box
inline constexpr int kLabelTop = kIconTop + kIconBox + kLabelGap;  // 36
// labelCentre.y - iconCentre.y, derived so a change to either box moves it.
inline constexpr int kIconLabelOffset =
    (kLabelTop + kLabelHeight / 2) - (kIconTop + kIconBox / 2);  // 25
// C78: the dropdown chevron lives BELOW the label, in the 6 px bottom margin:
// the mnemonic already owns the top-right corner and the label owns the middle.
// Only the two actions with Spec::hasDropdown ever paint it, and only while
// hovered / pressed / open (Apple-style restraint: no permanent affordance).
inline constexpr int kArrowWidth = 8;
inline Rect ArrowBoxRect(const Rect& button) {
  const int centre = (button.left + button.right) / 2;
  // Directly under the label box, inside the button's own 6 px bottom margin.
  return Rect{centre - kArrowWidth / 2, button.top + kLabelTop + kLabelHeight + 1,
              centre + kArrowWidth / 2, button.bottom - 1};
}
// The narrowest label area that must fit ("解压到…" is ~44 px at 11 px): the
// button's own floor, so every button in the band satisfies it by construction.
inline constexpr int kMinLabelWidth = kButtonMinWidth;
// U+2026 HORIZONTAL ELLIPSIS -- the only truncation the band may show.
inline constexpr char32_t kLabelEllipsis = 0x2026;

// The two boxes of one button, in the same coordinate space as Button::rect.
inline Rect IconBoxRect(const Rect& button) {
  const int centre = (button.left + button.right) / 2;
  return Rect{centre - kIconBox / 2, button.top + kIconTop, centre + kIconBox / 2,
              button.top + kIconTop + kIconBox};
}

inline Rect LabelBoxRect(const Rect& button) {
  return Rect{button.left, button.top + kLabelTop, button.right,
              button.top + kLabelTop + kLabelHeight};
}

struct Button {
  ActionId id = ActionId::Count;
  Rect rect;
  int group = 0;
};

// C78: one seam between two Groups. The rect is the 1 px hairline itself (a
// crisp integer column, so its rendered alpha can be recovered exactly instead
// of being smeared over two antialiased columns); before/after make the
// ordering bug ("the separator sits between Inspect and Find but groups say
// otherwise") impossible to write silently.
struct GroupSeparator {
  Rect rect;
  Group before = Group::Count;
  Group after = Group::Count;
};

struct Layout {
  Rect band;                    // == prism::layout::Layout::actionIsland
  Rect island;                  // the floating glass pill the buttons live in
  std::vector<Button> buttons;  // left to right; the "dropped" ones did not fit
  std::vector<GroupSeparator> separators;  // one per Group boundary, left to right
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
// The gap BEFORE action i (i >= 1): 12 px across a Group seam, 4 px inside one.
// The table is sorted by Group, so this is a pure function of the index and a
// new action inserted in the wrong block also changes the rhythm -- the reason
// the model suite asserts the order is non-decreasing.
inline constexpr int GapBefore(int index) {
  if (index <= 0 || index >= kActionCount) return 0;
  return kActions[static_cast<std::size_t>(index)].group ==
                 kActions[static_cast<std::size_t>(index - 1)].group
             ? kButtonGap
             : kGroupGap;
}

inline constexpr int TotalGaps() {
  int total = 0;
  for (int i = 1; i < kActionCount; ++i) total += GapBefore(i);
  return total;
}

// C115: the band split is no longer decided HERE. The pill yields width inside
// prism::layout::NavPillRect -- the authority the renderer, the hit test and the
// glass island all read -- and this file hands it the number only it knows: the
// narrowest island that can still show all eight buttons. That is what makes
// "the pill I paint" and "the pill the layout publishes" one rectangle instead
// of two that differ by 144 px at 1028. The static_assert keeps the two copies
// of that number from drifting: if a ninth action lands, this line fails to
// compile instead of the pill silently reserving the wrong width.
static_assert(prism::layout::kActionIslandMinWidth ==
                  2 * kIslandPad + kActionCount * kButtonMinWidth + TotalGaps(),
              "C115: WindowLayout's action-island reserve must equal the band's own "
              "floor (2*kIslandPad + kActionCount*kButtonMinWidth + TotalGaps())");

inline Layout Compute(const prism::layout::Layout& window) {
  Layout out;
  out.band = window.actionIsland;
  const int gaps = TotalGaps();
  // The eight buttons at their floor, plus the island's own padding: what the
  // band MUST leave free if the toolbar is to be complete.
  const int actionFloor = 2 * kIslandPad + kActionCount * kButtonMinWidth + gaps;
  // The pill is V1's rectangle until the actions need the room; then it gives
  // width back (its right edge moves left, its left edge and its height stay
  // V1's). A path display truncates gracefully; a toolbar that silently loses
  // its last three actions does not.
  out.navPill = prism::layout::NavPillRect(window, actionFloor);

  const int inset = prism::layout::kContentPadding;
  const int limit = out.band.right - inset;
  if (out.band.Empty()) return out;

#if defined(PRISM_C115_MUTANT_IGNORE_NARROW)
  // D3: the pre-C115 split, reproduced exactly. NavPillRect above already
  // returned V1's full 560 px (that is the injection); this is the SECOND,
  // stale copy the old code kept -- the band squeezes its own rectangle while
  // the authority (and the glass island that reads it) stays 560 px wide. The
  // result is the 144 px of toolbar on top of the pill at 1028, which is what
  // the suite's sweep must catch.
  if (!out.navPill.Empty()) {
    const int maxPillRight = limit - actionFloor - kNavGap;
    if (maxPillRight >= out.navPill.left) {
      out.navPill.right = (std::min)(out.navPill.right, maxPillRight);
    }
  }
#endif

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
    x += buttonWidth + GapBefore(i + 1);
  }
  // C66: "a dropped button means NO buttons". The tail-drop loop above can, in
  // principle, keep the first N buttons while reporting dropped > 0 -- and that
  // is exactly the half-painted toolbar the user photographed (six buttons on
  // screen, 添加/删除/测试 missing). A band that cannot show all eight shows
  // none: the island disappears and the app keeps the Alt chords. Asserted by
  // win.c66_window at 800/1280/1536 px.
  if (out.dropped > 0) {
    out.buttons.clear();
    out.separators.clear();
    out.island = Rect{};
    return out;
  }
  if (out.buttons.empty()) return out;

  out.island =
      Rect{left, out.band.top, out.buttons.back().rect.right + kIslandPad, out.band.bottom};

  // The seams: one integer column in the exact middle of each 12 px Group gap.
  // Positioned from the PAINTED button rects (not from x), so a 1 px rounding
  // in the width cannot detach a separator from the buttons it divides.
  for (std::size_t i = 1; i < out.buttons.size(); ++i) {
    const Group before = static_cast<Group>(out.buttons[i - 1].group);
    const Group after = static_cast<Group>(out.buttons[i].group);
    if (before == after) continue;
    const int seam = out.buttons[i - 1].rect.right + kGroupGap / 2;
    out.separators.push_back(GroupSeparator{
        Rect{seam, out.band.top + kGroupSeparatorInset, seam + 1,
             out.band.bottom - kGroupSeparatorInset},
        before, after});
  }
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
  // C115: same clamp as layout::NavTextRect. A pill squeezed below two insets
  // (the 640 px window) must yield an empty text box, not an inverted one.
  const int inset = (std::min)(prism::layout::kNavTextInset,
                               (std::max)(0, bar.navPill.Width() / 2));
  return Rect{bar.navPill.left + inset, bar.navPill.top + 10, bar.navPill.right - inset,
              bar.navPill.bottom - 10};
}

inline int DropdownRowAt(const Rect& panel, int x, int y) {
  if (panel.Empty()) return -1;
  if (x < panel.left || x >= panel.right || y < panel.top || y >= panel.bottom) return -1;
  const int local = y - panel.top - kDropdownPad;
  if (local < 0) return -1;
  return local / kDropdownRowHeight;
}

}  // namespace prism::actionbar
