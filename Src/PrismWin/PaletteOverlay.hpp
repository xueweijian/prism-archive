// PrismArchive -- PrismWin
// Command palette OVERLAY (card C31): the layer that turns C8's ranking + C8's
// PaletteShell geometry into a LIST OF DRAW COMMANDS. Like the palette shell it
// paints nothing and owns no HWND -- the renderer walks the command list.
//
// WHY A COMMAND LIST AND NOT A WM_PAINT
//   The palette has three consumers that must never disagree: the pixels, the
//   click targets and the tests. If the renderer computed geometry itself, the
//   row a user clicks and the row a test asserts would be two implementations of
//   the same arithmetic (the drift class this project already paid for with the
//   breadcrumb hit test). Here the row rectangles come from PaletteShell::
//   RowRect(), the hit test uses the SAME function, and the commands carry those
//   rectangles verbatim.
//
// SCOPE (the one thing the shell does not model)
//   C8's model ranks commands and files into one list. Tab narrows that list to
//   commands, then files, then back to all -- filtering happens HERE, on the
//   ranked result, so the ordering is still the model's decision and this file
//   never invents an order (blueprint ch. 9.1 row 3).
//
// HIGHLIGHT SPANS ARE BYTE RANGES
//   PrismCore's ranker reports matched byte offsets (documented: CJK matches by
//   byte sequence). This layer merges adjacent offsets into runs and hands the
//   renderer [begin,end) into the SAME string it draws, so a highlight can never
//   point at a character that is not in the text. The pixel-estimate helper
//   (TextSpanRect) is deliberately separate from the span itself: the renderer
//   substitutes DWrite measurement for the monospace guess without touching the
//   contract.
//
// CONTRACTS:
//   B2  includes <cstdint> for the enums' underlying type.
//   D2  the tests assert span CONTENT (text.substr(begin, end-begin) == "png"),
//       not just that a span exists.
//   D7  geometry has one source (PaletteShell); this file never recomputes a row
//       rectangle from constants.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/CommandPalette.hpp"
#include "../PrismCore/Shortcuts.hpp"
#include "CommandPalette.hpp"

namespace prism::win::palette {

// ---------------------------------------------------------------------------
// 1. Scope -- the Tab cycle
// ---------------------------------------------------------------------------

enum class Scope : std::uint8_t { All = 0, Commands, Files };

inline const char* ScopeLabel(Scope scope) {
  switch (scope) {
    case Scope::All: return "All";
    case Scope::Commands: return "Commands";
    case Scope::Files: return "Files";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Scope scope) { return os << ScopeLabel(scope); }

// +1 forward (Tab), -1 backward (Shift+Tab). Three values wrap, so a keyboard
// user can always return to where they started with one more Tab.
inline Scope NextScope(Scope scope, int delta) {
  int index = static_cast<int>(scope) + (delta >= 0 ? 1 : -1);
  if (index < 0) index += 3;
  if (index >= 3) index -= 3;
  return static_cast<Scope>(index);
}

// ---------------------------------------------------------------------------
// 2. Draw commands
// ---------------------------------------------------------------------------

enum class DrawKind : std::uint8_t {
  Card = 0,       // the floating card (rounded by the renderer)
  QueryText,      // the query line, or its placeholder
  ScopeBadge,     // "All" / "Commands" / "Files"
  Separator,      // the hairline under the header
  RowBackground,  // one row, normal state
  RowHighlight,   // the row the user is on (drawn after RowBackground)
  RowTitle,       // the entry's title (+ its matched spans)
  RowSubtitle,    // shortcut hint / size hint
  EmptyText,      // "No match in Commands"
  HintText,       // "Tab switches scope - Esc closes" (only when it fits)
};

inline const char* DrawKindName(DrawKind kind) {
  switch (kind) {
    case DrawKind::Card: return "card";
    case DrawKind::QueryText: return "query";
    case DrawKind::ScopeBadge: return "scope";
    case DrawKind::Separator: return "separator";
    case DrawKind::RowBackground: return "row-background";
    case DrawKind::RowHighlight: return "row-highlight";
    case DrawKind::RowTitle: return "row-title";
    case DrawKind::RowSubtitle: return "row-subtitle";
    case DrawKind::EmptyText: return "empty";
    case DrawKind::HintText: return "hint";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, DrawKind kind) {
  return os << DrawKindName(kind);
}

// A matched run inside the command's own text, in BYTES, [begin, end).
struct TextSpan {
  std::size_t begin = 0;
  std::size_t end = 0;
};

// One item the renderer must draw. Rectangles are px in client space, exactly
// as PaletteShell computed them.
struct DrawCommand {
  DrawKind kind = DrawKind::Card;
  Rect rect;
  std::string text;            // UTF-8; empty for the rect-only kinds
  int row = -1;                // palette row, -1 for chrome
  std::vector<TextSpan> spans; // only RowTitle fills this today
};

// The renderer measures text with DWrite; this is only the fallback the geometry
// math (and the tests) can use without a device context. Fixed-width on purpose:
// a proportional guess would be a lie that looks precise.
inline constexpr int kCharWidth = 7;

inline Rect TextSpanRect(const Rect& textRect, TextSpan span) {
  Rect rect;
  rect.left = textRect.left + static_cast<int>(span.begin) * kCharWidth;
  rect.right = textRect.left + static_cast<int>(span.end) * kCharWidth;
  rect.top = textRect.top;
  rect.bottom = textRect.bottom;
  if (rect.right > textRect.right) rect.right = textRect.right;
  if (rect.left > rect.right) rect.left = rect.right;
  return rect;
}

// ---------------------------------------------------------------------------
// 3. PaletteOverlay
// ---------------------------------------------------------------------------

class PaletteOverlay {
 public:
  static constexpr std::uint32_t kKeyTab = 0x09;  // VK_TAB, passed through WM_KEYDOWN

  // Attaching does not open anything: the window attaches once in CreateWindow
  // and Ctrl+K calls Open() later.
  void Attach(PaletteShell* shell) {
    shell_ = shell;
    if (!open_) return;
    Refresh();
  }

  bool Open() {
    open_ = true;
    scope_ = Scope::All;
    if (shell_ != nullptr) shell_->Open();  // one open state, not two
    Refresh();
    return true;
  }

  bool Close() {
    if (!open_) return false;
    open_ = false;
    rows_.clear();
    highlight_ = -1;
    if (shell_ != nullptr) shell_->Close();
    return true;
  }

  // One open state, not two: the shell owns the keyboard routing, so if it was
  // closed behind our back (Esc reaching the shell first) the overlay is closed
  // too and paints nothing. A duplicated flag here would paint a dead card.
  bool IsOpen() const { return open_ && (shell_ == nullptr || shell_->IsOpen()); }
  Scope scope() const { return scope_; }
  void SetScope(Scope scope) {
    scope_ = scope;
    Refresh();
  }

  int RowCount() const { return static_cast<int>(rows_.size()); }
  int Highlight() const { return highlight_; }

  void SetHighlight(int row) {
    if (rows_.empty()) {
      highlight_ = -1;
      return;
    }
    if (row < 0) row = 0;
    if (row >= RowCount()) row = RowCount() - 1;
    highlight_ = row;
  }

  // Arrows WRAP (Spotlight behaviour, and the same rule PaletteShell documents):
  // a short list never traps the selection at its end.
  void MoveHighlight(int delta) {
    const int count = RowCount();
    if (count <= 0) {
      highlight_ = -1;
      return;
    }
    int next = (highlight_ + delta) % count;
    if (next < 0) next += count;
    highlight_ = next;
  }

  // The model entry behind a visible row. nullptr when the row is out of range
  // (a renderer that asked for a row we do not have gets nothing to draw).
  const prism::palette::Entry* EntryAt(int row) const {
    if (shell_ == nullptr || row < 0 || row >= RowCount()) return nullptr;
    const std::size_t entry = rows_[static_cast<std::size_t>(row)].entry;
    if (entry >= shell_->Model().entries().size()) return nullptr;
    return &shell_->Model().entries()[entry];
  }

  const prism::palette::Match* MatchAt(int row) const {
    if (row < 0 || row >= RowCount()) return nullptr;
    return &rows_[static_cast<std::size_t>(row)];
  }

  const prism::palette::Entry* Highlighted() const { return EntryAt(highlight_); }

  // How many rows the card can show at this client size. The card's height comes
  // from the SHELL (which sizes it for the unfiltered result count), so a narrow
  // scope simply leaves the lower part of the card empty -- the card never
  // resizes while the user presses Tab, which would make the list jump.
  int VisibleRowCount(int clientW, int clientH) const {
    if (shell_ == nullptr) return 0;
    const Rect frame = shell_->Frame(clientW, clientH);
    const int space = frame.Height() - PaletteShell::kHeaderHeight;
    return space > 0 ? space / PaletteShell::kRowHeight : 0;
  }

  // Which row is under the pixel? Uses the SHELL's rectangle functions, so a
  // click and this answer cannot drift. -1 for the header, the padding, rows
  // past the visible count and anything outside the card.
  int RowAt(int clientW, int clientH, int x, int y) const {
    if (!IsOpen() || shell_ == nullptr) return -1;
    RefreshIfStale();
    const Rect frame = shell_->Frame(clientW, clientH);
    if (!frame.Contains(x, y)) return -1;
    const int local = y - (frame.top + PaletteShell::kHeaderHeight);
    if (local < 0) return -1;
    const int row = local / PaletteShell::kRowHeight;
    if (row < 0 || row >= RowCount() || row >= VisibleRowCount(clientW, clientH)) return -1;
    if (!shell_->RowRect(clientW, clientH, row).Contains(x, y)) return -1;
    return row;
  }

  struct Outcome {
    bool consumed = false;   // the overlay took the key (do not forward it)
    bool closed = false;     // Esc / Ctrl+K
    bool activate = false;   // Enter: the window runs Highlighted()
    bool scopeChanged = false;
    prism::keys::Command command = prism::keys::Command::None;
  };

  // Up/Down move, Tab switches scope, Esc (and Ctrl+K) closes. Everything else
  // is left to the shell's text field -- that split is exactly what
  // prism::keys::Resolve(stroke, Scope::Palette) already encodes.
  Outcome OnKey(const prism::keys::Stroke& stroke) {
    Outcome outcome;
    if (!IsOpen()) return outcome;
    if (stroke.key == kKeyTab) {
      SetScope(NextScope(scope_, stroke.shift ? -1 : 1));
      outcome.consumed = true;
      outcome.scopeChanged = true;
      return outcome;
    }
    const prism::keys::Command command =
        prism::keys::Resolve(stroke, prism::keys::Scope::Palette);
    switch (command) {
      case prism::keys::Command::ClosePalette:
        Close();
        outcome.consumed = true;
        outcome.closed = true;
        outcome.command = command;
        return outcome;
      case prism::keys::Command::SelectUp:
        MoveHighlight(-1);
        outcome.consumed = true;
        outcome.command = command;
        return outcome;
      case prism::keys::Command::SelectDown:
        MoveHighlight(1);
        outcome.consumed = true;
        outcome.command = command;
        return outcome;
      case prism::keys::Command::ActivateSelection:
        outcome.consumed = true;
        outcome.activate = true;
        outcome.command = command;
        return outcome;
      default:
        return outcome;  // letters / Backspace belong to the shell
    }
  }

  // Byte offsets -> merged runs. Adjacent bytes become ONE span, so "png" in
  // "root.png" is a single highlight instead of three. Out-of-range offsets are
  // dropped rather than clamped into a different character.
  static std::vector<TextSpan> MergePositions(const std::vector<std::size_t>& positions,
                                              std::size_t textSize) {
    std::vector<std::size_t> sorted;
    sorted.reserve(positions.size());
    for (const std::size_t position : positions) {
      if (position < textSize) sorted.push_back(position);
    }
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    std::vector<TextSpan> spans;
    std::size_t index = 0;
    while (index < sorted.size()) {
      const std::size_t begin = sorted[index];
      std::size_t end = begin + 1;
      ++index;
      while (index < sorted.size() && sorted[index] == end) {
        ++end;
        ++index;
      }
      spans.push_back(TextSpan{begin, end});
    }
    return spans;
  }

  // The command list the renderer walks. Empty when the overlay is closed.
  std::vector<DrawCommand> Build(int clientW, int clientH) const {
    std::vector<DrawCommand> commands;
    if (!IsOpen() || shell_ == nullptr) return commands;
    RefreshIfStale();  // the shell's text field owns the query; follow it
    const Rect frame = shell_->Frame(clientW, clientH);
    const int inset = PaletteShell::kHorizontalInset;
    commands.push_back(Make(DrawKind::Card, frame));

    const bool emptyQuery = shell_->Query().empty();
    Rect query;
    query.left = frame.left + inset;
    query.top = frame.top + 10;
    query.right = frame.right - inset - 110;
    query.bottom = frame.top + PaletteShell::kHeaderHeight - 12;
    commands.push_back(Make(
        DrawKind::QueryText, query,
        emptyQuery ? std::string("Type a command or a file name") : shell_->Query()));

    Rect badge;
    badge.left = frame.right - inset - 96;
    badge.top = frame.top + 12;
    badge.right = frame.right - inset;
    badge.bottom = frame.top + 40;
    commands.push_back(Make(DrawKind::ScopeBadge, badge, ScopeLabel(scope_)));

    Rect rule;
    rule.left = frame.left + inset;
    rule.top = frame.top + PaletteShell::kHeaderHeight - 2;
    rule.right = frame.right - inset;
    rule.bottom = rule.top + 1;
    commands.push_back(Make(DrawKind::Separator, rule));

    const int visible = VisibleRowCount(clientW, clientH);
    if (rows_.empty()) {
      const Rect empty = shell_->RowRect(clientW, clientH, 0);
      commands.push_back(Make(DrawKind::EmptyText, empty,
                              std::string("No match in ") + ScopeLabel(scope_)));
      return commands;
    }

    for (int row = 0; row < RowCount() && row < visible; ++row) {
      const Rect rect = shell_->RowRect(clientW, clientH, row);
      commands.push_back(Make(DrawKind::RowBackground, rect, std::string(), row));
      if (row == highlight_) {
        commands.push_back(Make(DrawKind::RowHighlight, rect, std::string(), row));
      }
      const prism::palette::Entry* entry = EntryAt(row);
      if (entry == nullptr) continue;
      Rect title;
      title.left = rect.left + 8;
      title.top = rect.top + 4;
      title.right = rect.right - 8;
      title.bottom = rect.top + 22;
      DrawCommand titleCommand = Make(DrawKind::RowTitle, title, entry->title, row);
      titleCommand.spans = MergePositions(rows_[static_cast<std::size_t>(row)].positions,
                                          entry->title.size());
      commands.push_back(std::move(titleCommand));
      Rect subtitle;
      subtitle.left = rect.left + 8;
      subtitle.top = rect.top + 22;
      subtitle.right = rect.right - 8;
      subtitle.bottom = rect.bottom - 2;
      commands.push_back(Make(DrawKind::RowSubtitle, subtitle, entry->subtitle, row));
    }

    // The footer only appears when the card has spare room; a hint drawn over a
    // row would be worse than no hint at all.
    const int used = visible < RowCount() ? visible : RowCount();
    // Free space appears when the card is CAP height-capped (the last row does
    // not fill it) -- never when the rows exactly fill the card.
    const int spare = frame.Height() - PaletteShell::kHeaderHeight - used * PaletteShell::kRowHeight;
    if (spare >= 18) {
      Rect hint;
      hint.left = frame.left + inset;
      hint.top = frame.top + PaletteShell::kHeaderHeight + used * PaletteShell::kRowHeight + 2;
      hint.right = frame.right - inset;
      hint.bottom = hint.top + 18;
      commands.push_back(Make(DrawKind::HintText, hint,
                              std::string("Tab: ") + ScopeLabel(NextScope(scope_, 1)) +
                                  "   Up/Down: move   Esc: close"));
    }
    return commands;
  }

 private:
  // One constructor keeps the aggregate field order from leaking into 20 call
  // sites (a reordered field would silently swap text and row).
  static DrawCommand Make(DrawKind kind, const Rect& rect, std::string text = std::string(),
                          int row = -1) {
    DrawCommand command;
    command.kind = kind;
    command.rect = rect;
    command.text = std::move(text);
    command.row = row;
    return command;
  }

  // Re-rank and re-filter. Highlight stays on a REAL row.
  //
  // const with a mutable cache on purpose: Build() is a const painting query,
  // but it is ALSO the last thing that runs after the shell's text field
  // changed. Requiring the caller to remember a Requery() would be a footgun of
  // exactly the kind this card exists to remove.
  void Refresh() const {
    rows_.clear();
    cachedQuery_ = shell_ != nullptr ? shell_->Query() : std::string();
    if (shell_ == nullptr) {
      highlight_ = -1;
      return;
    }
    const std::vector<prism::palette::Match> ranked = shell_->Model().Query(shell_->Query());
    const std::vector<prism::palette::Entry>& entries = shell_->Model().entries();
    for (const prism::palette::Match& match : ranked) {
      if (match.entry >= entries.size()) continue;
      const prism::palette::Kind kind = entries[match.entry].kind;
      if (scope_ == Scope::Commands && kind != prism::palette::Kind::Command) continue;
      if (scope_ == Scope::Files && kind != prism::palette::Kind::File) continue;
      rows_.push_back(match);
    }
    if (rows_.empty()) {
      highlight_ = -1;
      return;
    }
    if (highlight_ < 0 || highlight_ >= RowCount()) highlight_ = 0;
  }

  // Recomputes the rows when the shell's query moved since the last build.
  void RefreshIfStale() const {
    if (shell_ == nullptr) return;
    if (shell_->Query() != cachedQuery_) Refresh();
  }

  PaletteShell* shell_ = nullptr;
  Scope scope_ = Scope::All;
  bool open_ = false;
  mutable std::vector<prism::palette::Match> rows_;
  mutable int highlight_ = -1;
  mutable std::string cachedQuery_;
};

}  // namespace prism::win::palette
