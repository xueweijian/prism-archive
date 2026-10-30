// PrismArchive -- PrismCore
// The streaming gallery core (blueprint ch. 4.3): viewport virtualization,
// fixed-budget thumbnail cache, per-solid-block sweep planning, and the
// Quick Look session. Pure decisions, zero Windows -- the D2D renderer in
// PrismWin consumes these and nothing else.
#pragma once

#include <algorithm>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "Preview.hpp"

#include <ostream>

namespace prism::gallery {

// ---------------------------------------------------------------- viewport

struct GridLayout {
  int columns = 4;
  double cellHeight = 150.0;
};

// Which items does a scroll position render? Pure math with hard clamps --
// a virtualized list that can produce a negative index or divide by zero is a
// crash, not a feature.
class ViewportWindow {
 public:
  ViewportWindow(GridLayout layout, int itemCount)
      : layout_(layout), itemCount_(itemCount) {}

  void SetViewportSize(double widthPixels, double heightPixels) {
    width_ = widthPixels;
    height_ = heightPixels;
  }

  void SetScroll(double offsetPixels) {
    scroll_ = std::clamp(offsetPixels, 0.0, MaxScroll());
  }

  double Scroll() const { return scroll_; }
  int Columns() const { return layout_.columns; }
  double CellHeight() const { return layout_.cellHeight; }
  double ColumnWidth() const { return layout_.columns > 0 ? width_ / layout_.columns : 0.0; }

  double ContentHeight() const {
    if (itemCount_ <= 0) return 0.0;
    return static_cast<double>(Rows()) * layout_.cellHeight;
  }

  double MaxScroll() const { return std::max(0.0, ContentHeight() - height_); }

  // Inclusive visible index range, overscan rows added on both sides.
  struct Range {
    int first = 0;
    int last = -1;  // empty by default (first > last)
    bool Empty() const { return first > last; }
  };

  Range Visible(int overscanRows) const {
    if (itemCount_ <= 0 || layout_.columns <= 0 || layout_.cellHeight <= 0.0) return {};
    const int firstRow = std::max(0, static_cast<int>(scroll_ / layout_.cellHeight) - overscanRows);
    const int lastVisibleRow =
        static_cast<int>((scroll_ + height_ - 1.0) / layout_.cellHeight);
    const int lastRow = std::min(Rows() - 1, lastVisibleRow + overscanRows);
    if (lastRow < firstRow) return {};
    const int first = firstRow * layout_.columns;
    const int last = std::min(itemCount_ - 1, (lastRow + 1) * layout_.columns - 1);
    if (last < first) return {};
    return {first, last};
  }

  // Content-space hit test; -1 when the point is outside the grid.
  int IndexAt(double x, double y) const {
    if (itemCount_ <= 0 || layout_.columns <= 0 || layout_.cellHeight <= 0.0) return -1;
    if (x < 0.0 || y < 0.0 || y >= ContentHeight()) return -1;
    const int col = static_cast<int>(x / (width_ / layout_.columns));
    if (col < 0 || col >= layout_.columns) return -1;
    const int row = static_cast<int>(y / layout_.cellHeight);
    const int index = row * layout_.columns + col;
    return index < itemCount_ ? index : -1;
  }

 private:
  int Rows() const {
    return (itemCount_ + layout_.columns - 1) / layout_.columns;  // ceil
  }

  GridLayout layout_;
  int itemCount_;
  double width_ = 0.0;
  double height_ = 0.0;
  double scroll_ = 0.0;
};

// ---------------------------------------------------------------- LRU cache

// Decoded thumbnails are memory, not geometry: a fixed byte budget, true LRU
// eviction, and a LOUD rejection for items larger than the whole budget
// (silently dropping them would corrupt the budget invariant).
class ThumbnailLru {
 public:
  explicit ThumbnailLru(size_t budgetBytes) : budget_(budgetBytes) {}

  bool Insert(uint64_t key, size_t bytes) {
    if (bytes > budget_) return false;  // L3: never silently absorb
    auto it = index_.find(key);
    if (it != index_.end()) {  // re-insert: adjust the accounting first
      size_ -= it->second.second;
      lru_.erase(it->second.first);
      index_.erase(it);
    }
    while (size_ + bytes > budget_) EvictOldest();
    lru_.push_back(key);
    index_[key] = {std::prev(lru_.end()), bytes};
    size_ += bytes;
    return true;
  }

  // Hit: refresh recency and count. Miss: count. Peek() exists for logic that
  // must not disturb recency.
  bool Lookup(uint64_t key) {
    if (!Contains(key)) {
      ++misses_;
      return false;
    }
    Touch(key);
    ++hits_;
    return true;
  }

  bool Peek(uint64_t key) const { return index_.find(key) != index_.end(); }
  bool Contains(uint64_t key) const { return Peek(key); }

  void Touch(uint64_t key) {
    auto it = index_.find(key);
    if (it == index_.end()) return;
    lru_.erase(it->second.first);
    lru_.push_back(key);
    it->second.first = std::prev(lru_.end());
  }

  size_t SizeBytes() const { return size_; }
  size_t Count() const { return index_.size(); }
  size_t BudgetBytes() const { return budget_; }
  uint64_t Hits() const { return hits_; }
  uint64_t Misses() const { return misses_; }

 private:
  void EvictOldest() {
    if (lru_.empty()) return;
    const uint64_t victim = lru_.front();
    lru_.pop_front();
    size_ -= index_[victim].second;
    index_.erase(victim);
  }

  size_t budget_;
  size_t size_ = 0;
  std::list<uint64_t> lru_;  // front = least recently used
  std::unordered_map<uint64_t, std::pair<std::list<uint64_t>::iterator, size_t>> index_;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
};

// ------------------------------------------------------- solid sweep plan

struct SolidBlock {
  int id = 0;
  uint64_t bytes = 0;
  int itemCount = 0;
};

enum class BlockStrategy { SinglePassSweep, OnDemandBadge };

inline std::ostream& operator<<(std::ostream& os, BlockStrategy strategy) {
  switch (strategy) {
    case BlockStrategy::SinglePassSweep: return os << "SinglePassSweep";
    case BlockStrategy::OnDemandBadge: return os << "OnDemandBadge";
  }
  return os << "?";
}

// Blueprint ch. 4.3: a solid block of <= 256 MB is harvested in ONE linear
// pass (thumbnails come out along the way); anything bigger would stall the
// gallery, so the card gets an explicit on-demand badge instead. The boundary
// is inclusive -- 256 MB exactly is still a sweep.
inline constexpr uint64_t kSweepThresholdBytes = 256ull * 1024 * 1024;

inline BlockStrategy PlanBlock(const SolidBlock& block) {
  return block.bytes <= kSweepThresholdBytes ? BlockStrategy::SinglePassSweep
                                             : BlockStrategy::OnDemandBadge;
}

// Decode order across blocks: the focused block first (what the user is
// looking at), then outward by distance; ties resolve to the lower block id
// so the order is deterministic.
inline std::vector<int> SweepOrder(const std::vector<SolidBlock>& blocks, int focusBlock) {
  std::vector<int> order;
  order.reserve(blocks.size());
  for (const auto& block : blocks) order.push_back(block.id);
  std::sort(order.begin(), order.end(), [&](int a, int b) {
    const int da = std::abs(a - focusBlock);
    const int db = std::abs(b - focusBlock);
    if (da != db) return da < db;
    return a < b;
  });
  return order;
}

// ------------------------------------------------------------ quick look

// Quick Look is a session over previewable leaves: space toggles, Esc closes,
// navigation skips non-previewable entries, and over-limit items open in a
// marked Streaming state (the HUD then shows decode progress -- bomb hygiene
// inherited from the M1 planner).
class QuickLookSession {
 public:
  enum class State { Closed, Open, Streaming };

  QuickLookSession(const std::vector<prism::ArchiveItem>& items,
                   uint64_t byteLimit = prism::QuickLookPlanner::kDefaultByteLimit)
      : items_(&items), byteLimit_(byteLimit) {}

  State OpenAt(uint32_t index) {
    focus_ = index;
    if (!IsPreviewable(index)) return Close();
    const auto decision = prism::QuickLookPlanner::Decide((*items_)[index], byteLimit_);
    kind_ = decision.kind;
    state_ = decision.exceedsLimit ? State::Streaming : State::Open;
    return state_;
  }

  State ToggleAt(uint32_t index) {
    if (state_ != State::Closed && focus_ == index) return Close();
    return OpenAt(index);
  }

  State Close() {
    state_ = State::Closed;
    kind_ = prism::PreviewKind::None;
    return state_;
  }

  bool Next() { return Step(+1); }
  bool Prev() { return Step(-1); }

  State state() const { return state_; }
  prism::PreviewKind kind() const { return kind_; }
  uint32_t focus() const { return focus_; }
  bool streaming() const { return state_ == State::Streaming; }

 private:
  bool IsPreviewable(uint32_t index) const {
    if (index >= items_->size()) return false;
    const auto& item = (*items_)[index];
    if (item.isDir) return false;
    return prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::None;
  }

  bool Step(int direction) {
    if (items_->empty()) return false;
    int candidate = static_cast<int>(focus_) + direction;
    while (candidate >= 0 && candidate < static_cast<int>(items_->size())) {
      if (IsPreviewable(static_cast<uint32_t>(candidate))) {
        OpenAt(static_cast<uint32_t>(candidate));
        return true;
      }
      candidate += direction;
    }
    return false;
  }

  const std::vector<prism::ArchiveItem>* items_;
  uint64_t byteLimit_;
  State state_ = State::Closed;
  prism::PreviewKind kind_ = prism::PreviewKind::None;
  uint32_t focus_ = 0;
};

inline std::ostream& operator<<(std::ostream& os, QuickLookSession::State state) {
  switch (state) {
    case QuickLookSession::State::Closed: return os << "Closed";
    case QuickLookSession::State::Open: return os << "Open";
    case QuickLookSession::State::Streaming: return os << "Streaming";
  }
  return os << "?";
}

}  // namespace prism::gallery
