// PrismArchive -- PrismCore
// ThumbStore: the pixel-holding thumbnail cache behind the gallery tiles.
//
// The sweep session (SolidSweep.hpp) harvests decoded thumbnails through
// IThumbSink; this store keeps the BGRA payload itself under a byte budget
// with true LRU eviction. A version counter lets the renderer invalidate its
// device-bitmap cache exactly when the store mutated -- never per frame.
#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <unordered_map>
#include <utility>

#include "SolidSweep.hpp"

namespace prism::gallery {

class ThumbStore final : public IThumbSink {
 public:
  explicit ThumbStore(size_t budgetBytes) : budget_(budgetBytes) {}

  // IThumbSink: the sweep hands over a finished thumbnail.
  bool OnThumb(uint32_t itemIndex, const DecodedThumb& thumb) override {
    const size_t bytes = thumb.pixels.size();
    if (bytes == 0 || bytes > budget_) return false;  // L3: never silently absorb
    auto it = index_.find(itemIndex);
    if (it != index_.end()) {  // replace: settle the old accounting first
      size_ -= it->second.second;
      entries_.erase(it->second.first);
      index_.erase(it);
    }
    while (size_ + bytes > budget_ && !entries_.empty()) EvictOldest();
    entries_.push_back({itemIndex, thumb});
    index_[itemIndex] = {std::prev(entries_.end()), bytes};
    size_ += bytes;
    ++version_;
    return true;
  }

  // Hit: refresh recency and return the pixels. Miss: nullptr.
  const DecodedThumb* Lookup(uint32_t itemIndex) {
    auto it = index_.find(itemIndex);
    if (it == index_.end()) return nullptr;
    entries_.splice(entries_.end(), entries_, it->second.first);
    it->second.first = std::prev(entries_.end());
    return &it->second.first->thumb;
  }

  // Recency-free probe (renderer cache warm-up checks).
  bool Contains(uint32_t itemIndex) const { return index_.find(itemIndex) != index_.end(); }

  size_t Count() const { return entries_.size(); }
  size_t SizeBytes() const { return size_; }
  size_t BudgetBytes() const { return budget_; }
  uint64_t Version() const { return version_; }

 private:
  void EvictOldest() {
    if (entries_.empty()) return;
    size_ -= index_[entries_.front().index].second;
    index_.erase(entries_.front().index);
    entries_.pop_front();
    ++version_;  // eviction mutates what the renderer may still hold
  }

  struct Entry {
    uint32_t index;
    DecodedThumb thumb;
  };
  std::list<Entry> entries_;  // front = oldest, back = newest
  std::unordered_map<uint32_t, std::pair<std::list<Entry>::iterator, size_t>> index_;
  size_t budget_;
  size_t size_ = 0;
  uint64_t version_ = 0;
};

}  // namespace prism::gallery
