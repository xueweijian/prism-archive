// PrismArchive -- PrismCore
// The selection a file list owns: an ORDERED set of rows plus the anchor the
// next Shift+click measures from (card C67).
//
// Why this is core and not window code: Ctrl+click, Shift+click and Ctrl+A are
// DECISIONS ("this click REPLACES / TOGGLES / EXTENDS"), and the window may only
// map events -> decisions -> calls. Keeping them here also means the rules are
// testable in milliseconds on the Linux lane, where no HWND exists.
//
// Contract, stated as data (see Tests/test_selection_model.cpp):
//   * Selected() is ascending, duplicate-free and inside [0, Count()) -- the
//     order the app walks when it batches an action over "what is selected".
//   * a plain click REPLACES the selection; Ctrl+click TOGGLES one row and moves
//     the anchor there; Shift+click REPLACES the selection with the contiguous
//     range [anchor, row] (anchor unchanged, exactly like Explorer).
//   * a click on empty space (-1) clears -- unless it carries Ctrl, where the
//     user is mid-cherry-pick and losing the selection would be a surprise.
//   * Size() == 0 is the ONE input of the action band's hasSelection rule.
#pragma once

#include <algorithm>
#include <cstddef>
#include <ostream>
#include <vector>

namespace prism::sel {

// What a single click carried. Shift wins over Ctrl when both are held (Win32
// reports both for Ctrl+Shift+click; Explorer treats that as an extension).
struct ClickModifiers {
  bool ctrl = false;
  bool shift = false;
};

inline std::ostream& operator<<(std::ostream& os, const ClickModifiers& mods) {
  return os << "ctrl=" << (mods.ctrl ? 1 : 0) << " shift=" << (mods.shift ? 1 : 0);
}

class SelectionModel {
 public:
  // The list changed shape (folder navigation, archive switch). Rows that no
  // longer exist are dropped, never remapped: a stale row number would select
  // whatever now happens to sit at that position.
  void SetCount(int count) {
    count_ = count < 0 ? 0 : count;
    std::vector<int> kept;
    kept.reserve(selected_.size());
    for (int row : selected_) {
      if (row >= 0 && row < count_) kept.push_back(row);
    }
    selected_ = std::move(kept);
    if (focus_ < 0 || focus_ >= count_) focus_ = selected_.empty() ? -1 : selected_.back();
    if (anchor_ < 0 || anchor_ >= count_) anchor_ = focus_;
  }

  int Count() const { return count_; }
  bool Empty() const { return selected_.empty(); }
  std::size_t Size() const { return selected_.size(); }
  const std::vector<int>& Selected() const { return selected_; }
  int Focus() const { return focus_; }
  int Anchor() const { return anchor_; }

  bool Contains(int row) const {
    return std::binary_search(selected_.begin(), selected_.end(), row);
  }

  void Clear() {
    selected_.clear();
    focus_ = -1;
    anchor_ = -1;
  }

  void SelectAll() {
    selected_.clear();
    selected_.reserve(static_cast<std::size_t>(count_));
    for (int row = 0; row < count_; ++row) selected_.push_back(row);
    anchor_ = count_ > 0 ? 0 : -1;
    focus_ = count_ > 0 ? count_ - 1 : -1;
  }

  // A programmatic selection (arrow keys, palette jump, restore). Always a
  // single row and always the new anchor -- the keyboard is not cherry-picking.
  void SetSingle(int row) {
    if (row < 0 || row >= count_) {
      Clear();
      return;
    }
    selected_.assign(1, row);
    focus_ = row;
    anchor_ = row;
  }

  void Click(int row, ClickModifiers mods = {}) {
    if (row < 0) {
      if (!mods.ctrl && !mods.shift) Clear();
      return;
    }
    if (row >= count_) return;
    if (mods.shift && anchor_ >= 0) {
      const int first = anchor_ < row ? anchor_ : row;
      const int last = anchor_ < row ? row : anchor_;
      selected_.clear();
      selected_.reserve(static_cast<std::size_t>(last - first + 1));
      for (int i = first; i <= last; ++i) selected_.push_back(i);
      focus_ = row;  // the anchor stays where the range started
      return;
    }
    if (mods.ctrl) {
      const auto it = std::lower_bound(selected_.begin(), selected_.end(), row);
      if (it != selected_.end() && *it == row) {
        selected_.erase(it);
      } else {
        selected_.insert(it, row);
      }
      focus_ = row;
      anchor_ = row;
      return;
    }
    SetSingle(row);
  }

  // Deterministic replacement (restore, test setup): sorted, unique, in range.
  void SetSelected(std::vector<int> rows) {
    std::vector<int> normalized;
    normalized.reserve(rows.size());
    for (int row : rows) {
      if (row >= 0 && row < count_) normalized.push_back(row);
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    selected_ = std::move(normalized);
    focus_ = selected_.empty() ? -1 : selected_.back();
    anchor_ = selected_.empty() ? -1 : selected_.front();
  }

 private:
  int count_ = 0;
  int focus_ = -1;
  int anchor_ = -1;
  std::vector<int> selected_;  // ascending, unique
};

}  // namespace prism::sel
