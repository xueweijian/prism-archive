// PrismArchive -- PrismCore
// The solid-block sweep session (blueprint ch. 4.3): the scheduler that turns
// ONE linear 7z Extract call over a solid block into a full set of gallery
// thumbnails -- the Bandizip 禁区突破.
//
// The session sits between the Windows extractor (which owns the byte stream
// and the scratch buffer) and the thumbnail cache. It never copies payload
// bytes: the scratch buffer's pointer travels through OnItemEnd, so peak
// memory is one scratch item plus thumbnails -- never the block.
//
// Every decision is unit-tested here without Windows:
//   decode now / skip over-limit / skip failed decode / abandon-and-drain.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include "Gallery.hpp"  // ThumbnailLru (the only thumbnail cache)

namespace prism::gallery {

struct DecodedThumb {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> pixels;  // 32bpp BGRA, row-major, top-down
};

class SolidSweepSession {
 public:
  struct IDecoder {
    virtual ~IDecoder() = default;
    // Decode one item's bytes to a thumbnail. nullopt = not an image /
    // corrupt; the pass must survive it and keep harvesting.
    virtual std::optional<DecodedThumb> Decode(const uint8_t* data, size_t bytes) = 0;
  };

  // `sizes` are the UNCOMPRESSED sizes parallel to `indices` (from the
  // catalog); the limit decision is made from them BEFORE bytes arrive, so a
  // truncated or lying payload can never talk us into a huge decode.
  SolidSweepSession(const std::vector<uint32_t>& indices, const std::vector<uint64_t>& sizes,
                    uint64_t decodeLimitBytes, IDecoder& decoder, ThumbnailLru& lru)
      : indices_(indices),
        sizes_(sizes),
        decodeLimit_(decodeLimitBytes),
        decoder_(decoder),
        lru_(lru) {
    total_ = static_cast<int>(indices_.size());
  }

  // The extractor entered item `index` (extraction order == call order).
  void OnItemStart(uint32_t index) {
    current_ = index;
    const auto it = std::find(indices_.begin(), indices_.end(), index);
    const size_t slot = it != indices_.end() ? static_cast<size_t>(it - indices_.begin())
                                             : SIZE_MAX;
    currentOverLimit_ = slot == SIZE_MAX || sizes_[slot] > decodeLimit_;
  }

  // Bookkeeping only: the scratch buffer lives in the extractor; the session
  // counts drained bytes (the abandonment metric).
  void OnItemBytes(size_t bytes) { drainedBytes += bytes; }

  // The item finished: `data` points at the caller's scratch buffer holding
  // exactly `bytes` of this item's uncompressed content.
  void OnItemEnd(const uint8_t* data, size_t bytes) {
    if (!abandoned_ && !currentOverLimit_ && data != nullptr && bytes > 0) {
      if (auto thumb = decoder_.Decode(data, bytes)) {
        lru_.Insert(current_, thumb->pixels.size());
        lru_.Touch(current_);
        ++decodedCount;
      } else {
        ++skippedCount;
      }
    } else {
      ++skippedCount;
    }
    ++finished_;
  }

  // The viewport moved on: keep consuming the stream (the extractor's
  // callback contract requires every byte to be consumed) but decode nothing
  // further -- CPU is not spent on pixels nobody will see.
  void Abandon() { abandoned_ = true; }
  bool abandoned() const { return abandoned_; }

  int decodedCount = 0;
  int skippedCount = 0;
  uint64_t drainedBytes = 0;

  // Progress through THIS block's items, in [0,1], monotonic.
  double Fraction() const {
    if (total_ <= 0) return 1.0;
    return static_cast<double>(finished_) / static_cast<double>(total_);
  }

 private:
  std::vector<uint32_t> indices_;
  std::vector<uint64_t> sizes_;
  uint64_t decodeLimit_ = 0;
  IDecoder& decoder_;
  ThumbnailLru& lru_;
  uint32_t current_ = 0;
  bool currentOverLimit_ = false;
  bool abandoned_ = false;
  int total_ = 0;
  int finished_ = 0;
};

}  // namespace prism::gallery
