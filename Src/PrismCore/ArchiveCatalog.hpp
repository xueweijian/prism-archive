// PrismArchive -- PrismCore
// Platform-neutral archive catalog model (UTF-8, '/'-separated).
// No Windows / 7-Zip headers may appear in this directory: PrismCore is the
// brain of the product and must build + test on Linux/macOS/Windows alike.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace prism {

// One entry of an archive central directory.
// Mirrors the data PrismWin's 7z.dll bridge lifts out of IInArchive::GetProperty.
struct ArchiveItem {
  uint32_t index = 0;
  std::string path;  // UTF-8, '/'-separated, relative, no leading '/'
  uint64_t size = 0;      // uncompressed
  uint64_t packSize = 0;  // compressed
  int64_t mtime = 0;      // platform ticks (FILETIME on Windows)
  uint32_t block = 0;     // solid block id (used by the gallery prefetch engine)
  bool isDir = false;
  bool encrypted = false;
};

// Immutable snapshot of an archive's central directory plus O(1) path lookup.
class ArchiveCatalog {
 public:
  void Add(ArchiveItem item) {
    const uint32_t idx = item.index;
    byPath_.emplace(item.path, idx);
    items_.push_back(std::move(item));
  }

  const std::vector<ArchiveItem>& Items() const { return items_; }
  size_t Size() const { return items_.size(); }
  bool Empty() const { return items_.empty(); }

  const ArchiveItem* FindByPath(std::string_view path) const {
    auto it = byPath_.find(std::string(path));
    if (it == byPath_.end()) return nullptr;
    return &items_[it->second];
  }

  std::vector<uint32_t> IndicesOfDir(std::string_view dirPrefix) const {
    std::vector<uint32_t> out;
    const std::string prefix = std::string(dirPrefix);
    for (const auto& item : items_) {
      if (item.path.size() > prefix.size() && item.path.compare(0, prefix.size(), prefix) == 0 &&
          item.path[prefix.size()] == '/') {
        out.push_back(item.index);
      }
    }
    return out;
  }

  // All item indices sharing a solid block (7z/RAR). Used by the adaptive
  // solid-block sliding-window thumbnail cache (blueprint ch. 4.3).
  std::vector<uint32_t> IndicesOfBlock(uint32_t block) const {
    std::vector<uint32_t> out;
    for (const auto& item : items_) {
      if (!item.isDir && item.block == block) out.push_back(item.index);
    }
    return out;
  }

  uint64_t TotalUncompressed() const {
    uint64_t total = 0;
    for (const auto& item : items_) {
      if (!item.isDir) total += item.size;
    }
    return total;
  }

 private:
  std::vector<ArchiveItem> items_;
  std::unordered_map<std::string, uint32_t> byPath_;
};

}  // namespace prism
