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

  // Idle return (C33). The budget is a CAP, not an allocation: nothing is
  // reserved up front, but nothing is handed back either until pressure forces
  // eviction -- so a gallery that once filled 64 MiB keeps 64 MiB resident for
  // the rest of the session. This is the only path that moves resident DOWN
  // without pressure, and it is what a minimised / hidden gallery calls.
  //
  // Returns the bytes actually returned. Trimming to a target that is already
  // satisfied returns 0 and touches nothing (no version bump: the renderer's
  // device-bitmap cache stays valid).
  size_t Trim(size_t targetBytes) {
#ifdef PRISM_THUMB_MUTANT_NO_TRIM
    // Negative control (c33-memory.cmake): the "give the pixels back" step is
    // skipped, so the resident accounting stays where pressure left it and the
    // idle-return assertion must go red. The caller sees the injection at
    // runtime: the return value is 0 while resident exceeds the target.
    (void)targetBytes;
    return 0;
#else
    if (targetBytes >= size_) return 0;
    const size_t before = size_;
    while (size_ > targetBytes && !entries_.empty()) EvictOldest();
    return before - size_;
#endif
  }

  // Drop every pixel: the "no island visible / window minimised" state.
  size_t ReleaseAll() { return Trim(0); }

  size_t Count() const { return entries_.size(); }
  // Resident payload bytes (the D17 quantity: what the budget actually holds).
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
