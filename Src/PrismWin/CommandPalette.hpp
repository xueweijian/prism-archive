// PrismArchive -- PrismWin
// Command palette SHELL (blueprint ch. 9.1 row 3). Structure only: geometry,
// the text buffer, the highlight and the key routing. It draws nothing and
// owns no HWND -- the renderer will paint Frame()/RowRect() later, and the
// window will forward WM_CHAR/WM_KEYDOWN into OnChar/OnKey.
//
// The RANKING is not here: it lives in PrismCore/CommandPalette.hpp, tested on
// Linux in milliseconds. This file only holds the state the ranker cannot know
// (is the palette open, what has the user typed, which row is highlighted) and
// the geometry the hit test and the renderer must agree on.
//
// Geometry rule of one source: Frame() is the single authority for the card,
// and RowRect()/RowAt() are derived from it. A renderer that paints the rows
// from the same functions cannot drift from the click targets -- the exact bug
// class the breadcrumb hit test burned six rounds on (batch A).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/CommandPalette.hpp"
#include "../PrismCore/Shortcuts.hpp"

namespace prism::win::palette {

struct Rect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int Width() const { return right - left; }
  int Height() const { return bottom - top; }
  bool Contains(int x, int y) const {
    return x >= left && x < right && y >= top && y < bottom;
  }
};

class PaletteShell {
 public:
  // Layout constants (px). The card is centred, the query line is a header, and
  // every result row is the same height so RowAt() is a division, not a search.
  static constexpr int kHeaderHeight = 56;
  static constexpr int kRowHeight = 40;
  static constexpr int kHorizontalInset = 12;
  static constexpr int kMinCardWidth = 480;
  static constexpr int kMaxCardWidth = 720;
  static constexpr int kMaxCardHeight = 520;

  void SetCommands(const std::vector<prism::palette::CommandSpec>& commands) {
    model_.SetCommands(commands);
    Refresh();
  }
  void SetCatalog(const std::vector<ArchiveItem>& catalog) {
    model_.SetCatalog(catalog);
    Refresh();
  }
  const prism::palette::Model& Model() const { return model_; }

  bool IsOpen() const { return open_; }

  // Opening always starts from an empty query: the palette is a fresh search,
  // not a resumed one (a stale query would hide the command list the user
  // expects to see first).
  bool Open() {
    open_ = true;
    query_.clear();
    Refresh();
    return true;
  }
  bool Close() {
    if (!open_) return false;
    open_ = false;
    query_.clear();
    results_.clear();
    highlight_ = -1;
    return true;
  }
  bool Toggle() { return open_ ? Close() : Open(); }

  const std::string& Query() const { return query_; }

  // Re-rank after the buffer changes and keep the highlight on a REAL row.
  void SetQuery(std::string query) {
    query_ = std::move(query);
    Refresh();
  }

  const std::vector<prism::palette::Match>& Results() const { return results_; }
  int RowCount() const { return static_cast<int>(results_.size()); }
  int Highlight() const { return highlight_; }

  // Clamped, not wrapped: a click or a programmatic set can never point past
  // the last row (-1 means "no highlight").
  void SetHighlight(int row) {
    if (results_.empty()) {
      highlight_ = -1;
      return;
    }
    if (row < 0) row = 0;
    if (row >= RowCount()) row = RowCount() - 1;
    highlight_ = row;
  }

  // Arrow keys WRAP (macOS Spotlight behaviour): Down on the last row returns
  // to the first, so a short list never traps the selection at the end.
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

  const prism::palette::Entry* Highlighted() const {
    if (highlight_ < 0 || highlight_ >= RowCount()) return nullptr;
    const size_t entry = results_[static_cast<size_t>(highlight_)].entry;
    if (entry >= model_.entries().size()) return nullptr;
    return &model_.entries()[entry];
  }

  // ---------------------------------------------------------------- geometry
  Rect Frame(int clientW, int clientH) const {
    int width = (clientW * 55) / 100;
    if (width < kMinCardWidth) width = kMinCardWidth;
    if (width > kMaxCardWidth) width = kMaxCardWidth;
    if (clientW > 0 && width > clientW) width = clientW;
    const int rows = RowCount() > 0 ? RowCount() : 1;
    int height = kHeaderHeight + rows * kRowHeight;
    if (height > kMaxCardHeight) height = kMaxCardHeight;
    const int limit = (clientH * 70) / 100;
    if (limit > 0 && height > limit) height = limit;
    Rect frame;
    frame.left = (clientW - width) / 2;
    frame.top = (clientH - height) / 2;
    frame.right = frame.left + width;
    frame.bottom = frame.top + height;
    return frame;
  }

  Rect RowRect(int clientW, int clientH, int row) const {
    const Rect frame = Frame(clientW, clientH);
    Rect rect;
    rect.left = frame.left + kHorizontalInset;
    rect.right = frame.right - kHorizontalInset;
    rect.top = frame.top + kHeaderHeight + row * kRowHeight;
    rect.bottom = rect.top + kRowHeight;
    return rect;
  }

  // Which row is under the pixel? -1 for the header, the card's padding, rows
  // past the end, and everything outside the card.
  int RowAt(int clientW, int clientH, int x, int y) const {
    const Rect frame = Frame(clientW, clientH);
    if (!frame.Contains(x, y)) return -1;
    const int local = y - (frame.top + kHeaderHeight);
    if (local < 0) return -1;
    const int row = local / kRowHeight;
    if (row < 0 || row >= RowCount()) return -1;
    if (!RowRect(clientW, clientH, row).Contains(x, y)) return -1;
    return row;
  }

  // ------------------------------------------------------------------- input
  struct KeyOutcome {
    bool consumed = false;
    // The command the WINDOW should execute. ActivateSelection means "read
    // Highlighted() now": the shell never executes anything itself.
    prism::keys::Command command = prism::keys::Command::None;
  };

  KeyOutcome OnKey(const prism::keys::Stroke& stroke) {
    KeyOutcome outcome;
    if (!open_) return outcome;
    const prism::keys::Command command =
        prism::keys::Resolve(stroke, prism::keys::Scope::Palette);
    switch (command) {
      case prism::keys::Command::ClosePalette:
        Close();
        outcome.consumed = true;
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
        outcome.command = command;
        return outcome;
      default:
        break;
    }
    if (stroke.ctrl || stroke.alt || stroke.shift) return outcome;  // not ours
    if (stroke.key == prism::keys::kKeyBack) {  // Backspace edits the query
      BackspaceQuery();
      outcome.consumed = true;
      return outcome;
    }
    // Printable VK codes are text (Space included): the palette's field owns
    // them, which is exactly what Resolve() promised by returning None.
    if (stroke.key >= 0x20 && stroke.key < 0x7F) {
      query_.push_back(static_cast<char>(stroke.key));
      Refresh();
      outcome.consumed = true;
    }
    return outcome;
  }

  // WM_CHAR entry point: the shell stores UTF-8, so a codepoint is encoded
  // here and a Backspace removes a whole character (not one byte, which would
  // leave a broken UTF-8 tail that the ranker would happily search for).
  bool OnChar(uint32_t codepoint) {
    if (!open_) return false;
    if (codepoint < 0x20 || codepoint == 0x7F) return false;
    if (codepoint < 0x80) {
      query_.push_back(static_cast<char>(codepoint));
    } else if (codepoint < 0x800) {
      query_.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
      query_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint < 0x10000) {
      query_.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
      query_.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      query_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else {
      query_.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
      query_.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
      query_.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
      query_.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    Refresh();
    return true;
  }

 private:
  // One press removes one CHARACTER. Popping a single byte of a multi-byte
  // UTF-8 sequence would leave a broken tail that the byte-based ranker still
  // searches, so the continuation bytes go with it.
  void BackspaceQuery() {
    while (!query_.empty()) {
      const unsigned char last = static_cast<unsigned char>(query_.back());
      query_.pop_back();
      if ((last & 0xC0) != 0x80) break;  // reached the lead byte
    }
    Refresh();
  }

  void Refresh() {
    results_ = model_.Query(query_);
    if (results_.empty()) {
      highlight_ = -1;
      return;
    }
    if (highlight_ < 0 || highlight_ >= RowCount()) highlight_ = 0;
  }

  prism::palette::Model model_;
  bool open_ = false;
  std::string query_;
  std::vector<prism::palette::Match> results_;
  int highlight_ = -1;
};

}  // namespace prism::win::palette
