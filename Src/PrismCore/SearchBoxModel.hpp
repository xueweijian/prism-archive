// PrismArchive -- PrismCore
// SearchBoxModel: the archive list's SEARCH BOX as pure state (blueprint
// ch. 3.2 / Bandizip: a search field that narrows the visible rows as you type).
//
// This module owns every rule the painter and the (later) window must not
// re-decide:
//
//   * the query string is UTF-8; caret/selection are BYTE OFFSETS that are
//     always snapped to a code-point boundary, so Backspace/Left/Right can
//     never split a CJK character in half;
//   * an IME composition is a PREVIEW: it is shown (DisplayText) but it does
//     not touch Query(), does not filter, and does not move the committed
//     caret. Keys that belong to the IME while composing are reported as
//     ignored instead of editing text behind its back;
//   * Escape has three stages -- cancel the composition, then clear the query,
//     then close. The later UI card must not invent a fourth;
//   * filtering reuses C13's FilterModel: the query is the substring, rows are
//     only removed (never renumbered), so a selection keeps pointing at the
//     same catalog indices and hiding one is counted (hiddenSelection);
//   * an EMPTY query is "no filter", not "matches nothing". That is the rule
//     the negative control below deliberately breaks.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27), registered by
// Tests/suites/c62-searchui.cmake:
//   PRISM_C62_MUTANT_ESC_CLOSES_ON_FIRST     the first Escape closes the box
//                                            instead of clearing the query
//   PRISM_C62_MUTANT_EMPTY_QUERY_FILTERS     an empty query hides every row
// Both are compiled into the SAME test source; the test arms
// prism::test::SetMutationVerdict with a runtime predicate, prints the CAUGHT
// marker at the first red assertion and exits 0, so a stale injection can never
// read as "caught" (D3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "FilterModel.hpp"
#include "ListSortModel.hpp"

namespace prism::search {

// The two feedback strings are part of the card's contract, so they live here
// (not in the painter): a later i18n card replaces them in ONE place.
inline constexpr std::string_view kEmptyStateText = "无结果";
inline constexpr std::string_view kFilteringPrefix = "过滤中 ";
inline constexpr std::string_view kFilteringSuffix = " 条";
inline constexpr std::string_view kPlaceholderText = "搜索";

enum class Phase : uint8_t { Closed, Open };
enum class Status : uint8_t {
  Unfiltered,  // empty query: every row is visible
  Matches,     // the query keeps N > 0 rows
  NoResults,   // the query keeps no rows
};

inline std::ostream& operator<<(std::ostream& os, Phase phase) {
  switch (phase) {
    case Phase::Closed: return os << "Closed";
    case Phase::Open: return os << "Open";
  }
  return os << "?";
}

inline std::ostream& operator<<(std::ostream& os, Status status) {
  switch (status) {
    case Status::Unfiltered: return os << "Unfiltered";
    case Status::Matches: return os << "Matches";
    case Status::NoResults: return os << "NoResults";
  }
  return os << "?";
}

// The keyboard vocabulary the window maps WM_CHAR/WM_KEYDOWN onto. Character
// carries UTF-8 because that is what the model stores; the window encodes the
// WM_CHAR code unit (or the IME's committed string) once, at the boundary.
enum class Key : uint8_t {
  Character,
  Backspace,
  Delete,
  Left,
  Right,
  Home,
  End,
  Escape,
  Enter,
  SelectAll,
};

inline std::ostream& operator<<(std::ostream& os, Key key) {
  switch (key) {
    case Key::Character: return os << "Character";
    case Key::Backspace: return os << "Backspace";
    case Key::Delete: return os << "Delete";
    case Key::Left: return os << "Left";
    case Key::Right: return os << "Right";
    case Key::Home: return os << "Home";
    case Key::End: return os << "End";
    case Key::Escape: return os << "Escape";
    case Key::Enter: return os << "Enter";
    case Key::SelectAll: return os << "SelectAll";
  }
  return os << "?";
}

struct KeyEvent {
  Key key = Key::Character;
  std::string text;  // Key::Character only, UTF-8 (may be one or more code points)
  bool ctrl = false;
  bool shift = false;

  static KeyEvent Text(std::string utf8) {
    KeyEvent event;
    event.key = Key::Character;
    event.text = std::move(utf8);
    return event;
  }
  static KeyEvent Press(Key key, bool ctrl = false, bool shift = false) {
    KeyEvent event;
    event.key = key;
    event.ctrl = ctrl;
    event.shift = shift;
    return event;
  }
};

// What the WINDOW should do after a key. Edited = redraw only; FilterChanged =
// the query moved, so the list must be rebuilt from VisibleIndices(); Closed =
// the search session ended (and its filter is gone); Accepted = Enter asked for
// the selection to move to the list.
struct Effect {
  enum class Kind : uint8_t { None, Edited, FilterChanged, Closed, Accepted };

  Kind kind = Kind::None;
  bool consumed = false;
  bool queryChanged = false;
  bool closed = false;
  bool accepted = false;

  static Effect Make(Kind kind) {
    Effect effect;
    effect.kind = kind;
    effect.consumed = kind != Kind::None;
    effect.queryChanged = kind == Kind::FilterChanged;
    effect.closed = kind == Kind::Closed;
    effect.accepted = kind == Kind::Accepted;
    return effect;
  }
  // An ignored key must NOT be reported as consumed: the window still owns
  // Enter-to-open, Tab, and the main window's accelerators in that case.
  static Effect Ignored() { return Effect{}; }

  bool operator==(const Effect&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, Effect::Kind kind) {
  switch (kind) {
    case Effect::Kind::None: return os << "None";
    case Effect::Kind::Edited: return os << "Edited";
    case Effect::Kind::FilterChanged: return os << "FilterChanged";
    case Effect::Kind::Closed: return os << "Closed";
    case Effect::Kind::Accepted: return os << "Accepted";
  }
  return os << "?";
}

namespace detail {

inline std::size_t NextCodePointStart(std::string_view text, std::size_t pos) {
  if (pos >= text.size()) return text.size();
  std::size_t length = prism::list::Utf8SequenceLength(text, pos);
  if (length == 0) length = 1;
  return pos + length > text.size() ? text.size() : pos + length;
}

// The last boundary strictly before pos (0 when pos is 0/inside the first
// code point). Caret positions are always boundaries, so this is exact.
inline std::size_t PrevCodePointStart(std::string_view text, std::size_t pos) {
  if (pos == 0) return 0;
  if (pos > text.size()) pos = text.size();
  std::size_t at = 0;
  std::size_t previous = 0;
  while (at < pos) {
    previous = at;
    at = NextCodePointStart(text, at);
  }
  return previous;
}

// The start of the code point containing pos, so an offset handed in from a
// mouse hit can never land inside a multi-byte sequence.
inline std::size_t CodePointStartAtOrBefore(std::string_view text, std::size_t pos) {
  if (pos >= text.size()) return text.size();
  std::size_t at = 0;
  while (at < text.size()) {
    const std::size_t next = NextCodePointStart(text, at);
    if (next > pos) return at;
    at = next;
  }
  return text.size();
}

inline bool IsSpaceByte(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline std::size_t PrevWordStart(std::string_view text, std::size_t pos) {
  std::size_t at = CodePointStartAtOrBefore(text, pos);
  while (at > 0) {
    const std::size_t previous = PrevCodePointStart(text, at);
    if (!IsSpaceByte(static_cast<unsigned char>(text[previous]))) break;
    at = previous;
  }
  while (at > 0) {
    const std::size_t previous = PrevCodePointStart(text, at);
    if (IsSpaceByte(static_cast<unsigned char>(text[previous]))) break;
    at = previous;
  }
  return at;
}

inline std::size_t NextWordStart(std::string_view text, std::size_t pos) {
  std::size_t at = CodePointStartAtOrBefore(text, pos);
  while (at < text.size() && IsSpaceByte(static_cast<unsigned char>(text[at]))) {
    at = NextCodePointStart(text, at);
  }
  while (at < text.size() && !IsSpaceByte(static_cast<unsigned char>(text[at]))) {
    at = NextCodePointStart(text, at);
  }
  return at;
}

}  // namespace detail

class Model {
 public:
  // ------------------------------------------------------------------ session
  // Ctrl+F. Selecting the existing query is the Explorer/Bandizip behaviour:
  // typing replaces the old search instead of appending to it.
  void Open(bool selectAll = true) {
    phase_ = Phase::Open;
    focused_ = true;
    composing_ = false;
    composition_.clear();
    if (selectAll && !query_.empty()) {
      anchor_ = 0;
      caret_ = query_.size();
    } else {
      anchor_ = query_.size();
      caret_ = query_.size();
    }
  }

  // Closing the search ends the filtering too: the rows come back. That is one
  // rule, not two, so the two cannot drift apart.
  void Close() {
    phase_ = Phase::Closed;
    focused_ = false;
    query_.clear();
    composing_ = false;
    composition_.clear();
    anchor_ = 0;
    caret_ = 0;
    Recompute();
  }

  bool IsOpen() const { return phase_ == Phase::Open; }
  Phase GetPhase() const { return phase_; }
  void SetFocused(bool focused) { focused_ = focused; }
  bool Focused() const { return focused_ && phase_ == Phase::Open; }

  // -------------------------------------------------------------------- query
  const std::string& Query() const { return query_; }
  std::size_t Caret() const { return caret_; }
  bool HasSelection() const { return anchor_ != caret_; }
  std::size_t SelectionStart() const { return anchor_ < caret_ ? anchor_ : caret_; }
  std::size_t SelectionEnd() const { return anchor_ < caret_ ? caret_ : anchor_; }

  // The committed text with the IME preview spliced in. The painter draws this;
  // the filter NEVER does (see Composing()).
  std::string DisplayText() const {
    if (!composing_) return query_;
    std::string text = query_;
    text.replace(SelectionStart(), SelectionEnd() - SelectionStart(), composition_);
    return text;
  }

  // Byte offset of the caret inside DisplayText. During a composition the IME
  // caret sits after its preview, not at the committed position.
  std::size_t DisplayCaret() const {
    return composing_ ? SelectionStart() + composition_.size() : caret_;
  }

  bool Composing() const { return composing_; }
  const std::string& Composition() const { return composition_; }

  void SetQuery(std::string query, bool selectAll = false) {
    query_ = std::move(query);
    composing_ = false;
    composition_.clear();
    caret_ = query_.size();
    anchor_ = selectAll ? 0 : caret_;
    Recompute();
  }

  void SelectAll() {
    anchor_ = 0;
    caret_ = query_.size();
  }

  void ClearQuery() {
    query_.clear();
    composing_ = false;
    composition_.clear();
    anchor_ = 0;
    caret_ = 0;
    Recompute();
  }

  // Programmatic caret (mouse / focus restore). Snapped to a code point start.
  void SetCaret(std::size_t byteOffset, bool extend = false) {
    caret_ = detail::CodePointStartAtOrBefore(query_, byteOffset);
    if (!extend) anchor_ = caret_;
  }

  // ------------------------------------------------------------------ editing
  Effect InsertText(std::string_view utf8) {
    if (phase_ != Phase::Open || utf8.empty()) return Effect::Ignored();
    // A WM_CHAR outside the IME means the composition ended; keep the two
    // states consistent instead of leaving a preview that will never commit.
    if (composing_) CancelComposition();
    ReplaceSelection(utf8);
    Recompute();
    return Effect::Make(Effect::Kind::FilterChanged);
  }

  Effect HandleKey(const KeyEvent& event) {
    if (phase_ != Phase::Open) return Effect::Ignored();
    switch (event.key) {
      case Key::Character: return InsertText(event.text);
      case Key::Backspace: return Backspace(event.ctrl);
      case Key::Delete: return DeleteForward(event.ctrl);
      case Key::Left: return MoveCaret(true, event.shift, event.ctrl);
      case Key::Right: return MoveCaret(false, event.shift, event.ctrl);
      case Key::Home: return MoveToBoundary(true, event.shift);
      case Key::End: return MoveToBoundary(false, event.shift);
      case Key::Escape: return Escape();
      case Key::Enter: return Effect::Make(Effect::Kind::Accepted);
      case Key::SelectAll:
        SelectAll();
        return Effect::Make(Effect::Kind::Edited);
    }
    return Effect::Ignored();
  }

  Effect Press(Key key, bool ctrl = false, bool shift = false) {
    return HandleKey(KeyEvent::Press(key, ctrl, shift));
  }

  // Three stages, in this order:
  //   0. an IME composition is cancelled, query untouched;
  //   1. a non-empty query (or a selection) is cleared, box stays open;
  //   2. an already-empty box closes.
  Effect Escape() {
    if (composing_) {
      composing_ = false;
      composition_.clear();
      return Effect::Make(Effect::Kind::Edited);
    }
    if (!query_.empty() || HasSelection()) {
#if defined(PRISM_C62_MUTANT_ESC_CLOSES_ON_FIRST)
      // MUTATION: the first Escape ends the session instead of clearing the
      // query. The suite's two-stage case must go red on this build.
      Close();
      return Effect::Make(Effect::Kind::Closed);
#else
      query_.clear();
      anchor_ = 0;
      caret_ = 0;
      Recompute();
      return Effect::Make(Effect::Kind::FilterChanged);
#endif
    }
    Close();
    return Effect::Make(Effect::Kind::Closed);
  }

  // -------------------------------------------------------------- composition
  void BeginComposition() {
    if (phase_ != Phase::Open) return;
    composing_ = true;
    composition_.clear();
  }

  Effect UpdateComposition(std::string preview) {
    if (phase_ != Phase::Open) return Effect::Ignored();
    composing_ = true;
    composition_ = std::move(preview);
    return Effect::Make(Effect::Kind::Edited);
  }

  Effect CommitComposition(std::string committed) {
    if (phase_ != Phase::Open) return Effect::Ignored();
    composing_ = false;
    composition_.clear();
    if (committed.empty()) return Effect::Make(Effect::Kind::Edited);
    ReplaceSelection(committed);
    Recompute();
    return Effect::Make(Effect::Kind::FilterChanged);
  }

  Effect CancelComposition() {
    if (!composing_) return Effect::Ignored();
    composing_ = false;
    composition_.clear();
    return Effect::Make(Effect::Kind::Edited);
  }

  // ------------------------------------------------- rows / filter composition
  void SetRows(std::vector<list::Row> rows) {
    rows_ = std::move(rows);
    Recompute();
  }

  const std::vector<list::Row>& Rows() const { return rows_; }

  void SetSelection(std::vector<uint32_t> catalogIndices) {
    selection_ = std::move(catalogIndices);
    Recompute();
  }

  // The caller's entries that survived the filter, same order as the input.
  const std::vector<uint32_t>& Selection() const { return filter_.selection; }
  std::size_t HiddenSelection() const { return filter_.hiddenSelection; }

  const list::FilterResult& Filter() const { return filter_; }
  const std::vector<uint32_t>& VisibleIndices() const { return filter_.visible; }
  std::size_t MatchedCount() const { return matched_; }

  Status GetStatus() const {
    if (query_.empty()) return Status::Unfiltered;
    return matched_ > 0 ? Status::Matches : Status::NoResults;
  }

  // "过滤中 N 条" / "无结果" / "" -- exactly the strings the badge paints.
  std::string StatusText() const {
    switch (GetStatus()) {
      case Status::Unfiltered: return {};
      case Status::Matches:
        return std::string(kFilteringPrefix) + std::to_string(matched_) +
               std::string(kFilteringSuffix);
      case Status::NoResults: return std::string(kEmptyStateText);
    }
    return {};
  }

  // The list's empty-state line, empty unless the query matched nothing.
  std::string EmptyStateText() const {
    return GetStatus() == Status::NoResults ? std::string(kEmptyStateText) : std::string{};
  }

 private:
  void ReplaceSelection(std::string_view text) {
    const std::size_t start = SelectionStart();
    query_.replace(start, SelectionEnd() - start, text.data(), text.size());
    caret_ = start + text.size();
    anchor_ = caret_;
  }

  void EraseSelection() {
    const std::size_t start = SelectionStart();
    query_.erase(start, SelectionEnd() - start);
    caret_ = start;
    anchor_ = caret_;
  }

  Effect Backspace(bool byWord) {
    // While the IME owns the keyboard the model must not edit behind it.
    if (composing_) return Effect::Ignored();
    if (HasSelection()) {
      EraseSelection();
    } else if (caret_ == 0) {
      return Effect::Ignored();
    } else {
      const std::size_t from = byWord ? detail::PrevWordStart(query_, caret_)
                                      : detail::PrevCodePointStart(query_, caret_);
      query_.erase(from, caret_ - from);
      caret_ = from;
      anchor_ = from;
    }
    Recompute();
    return Effect::Make(Effect::Kind::FilterChanged);
  }

  Effect DeleteForward(bool byWord) {
    if (composing_) return Effect::Ignored();
    if (HasSelection()) {
      EraseSelection();
    } else if (caret_ >= query_.size()) {
      return Effect::Ignored();
    } else {
      const std::size_t to = byWord ? detail::NextWordStart(query_, caret_)
                                    : detail::NextCodePointStart(query_, caret_);
      query_.erase(caret_, to - caret_);
    }
    Recompute();
    return Effect::Make(Effect::Kind::FilterChanged);
  }

  Effect MoveCaret(bool left, bool extend, bool byWord) {
    if (composing_) return Effect::Ignored();
    if (!extend && HasSelection()) {
      caret_ = left ? SelectionStart() : SelectionEnd();
      anchor_ = caret_;
      return Effect::Make(Effect::Kind::Edited);
    }
    std::size_t next = caret_;
    if (left) {
      next = byWord ? detail::PrevWordStart(query_, caret_)
                    : detail::PrevCodePointStart(query_, caret_);
    } else {
      next = byWord ? detail::NextWordStart(query_, caret_)
                    : detail::NextCodePointStart(query_, caret_);
    }
    caret_ = next;
    if (!extend) anchor_ = caret_;
    return Effect::Make(Effect::Kind::Edited);
  }

  Effect MoveToBoundary(bool home, bool extend) {
    if (composing_) return Effect::Ignored();
    caret_ = home ? 0 : query_.size();
    if (!extend) anchor_ = caret_;
    return Effect::Make(Effect::Kind::Edited);
  }

  // One place computes "what is visible". An empty query is the NO-FILTER
  // branch (every row, in catalog order) -- not a filter that matches nothing.
  void Recompute() {
    filter_ = list::FilterResult{};
#if defined(PRISM_C62_MUTANT_EMPTY_QUERY_FILTERS)
    // MUTATION: the empty query is treated as "matches nothing", so clearing
    // the box hides the whole archive. Every assertion that expects the full
    // row count with an empty query must go red on this build.
    if (query_.empty()) {
      filter_.hiddenSelection = selection_.size();
      matched_ = 0;
      return;
    }
#endif
    if (query_.empty()) {
      filter_.visible.reserve(rows_.size());
      for (const list::Row& row : rows_) {
        filter_.visible.push_back(row.catalogIndex);
        filter_.visibleIndices.insert(row.catalogIndex);
      }
      filter_.selection = selection_;
      filter_.hiddenSelection = 0;
      matched_ = filter_.visible.size();
      return;
    }

    list::FilterSpec spec;
    spec.substring = query_;  // C13 owns the matching rules (ASCII fold, UTF-8 bytes)
    filter_ = list::ApplyFilter(rows_, selection_, spec);
    matched_ = filter_.visible.size();
  }

  Phase phase_ = Phase::Closed;
  bool focused_ = false;
  bool composing_ = false;
  std::string query_;
  std::string composition_;
  std::size_t anchor_ = 0;
  std::size_t caret_ = 0;
  std::vector<list::Row> rows_;
  std::vector<uint32_t> selection_;
  list::FilterResult filter_;
  std::size_t matched_ = 0;
};

}  // namespace prism::search
