// PrismArchive -- PrismCore
// Preview brain (blueprint ch. 4.1 inline preview + ch. 4.3 solid-block gallery):
//   * QuickLookPlanner      -- what kind of preview, and is it safe/cheap?
//   * SolidBlockPrefetcher  -- what to decode next so the gallery feels instant.
#pragma once

#include <cstdint>
#include <ostream>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"

namespace prism {

enum class PreviewKind { None, Directory, Image, Text, Pdf, Media };

inline std::ostream& operator<<(std::ostream& os, PreviewKind kind) {
  switch (kind) {
    case PreviewKind::None: return os << "None";
    case PreviewKind::Directory: return os << "Directory";
    case PreviewKind::Image: return os << "Image";
    case PreviewKind::Text: return os << "Text";
    case PreviewKind::Pdf: return os << "Pdf";
    case PreviewKind::Media: return os << "Media";
  }
  return os << "?";
}

class QuickLookPlanner {
 public:
  static constexpr uint64_t kDefaultByteLimit = 32ull << 20;

  static PreviewKind KindByName(std::string_view fileName) {
    // ExtensionOf returns ".png" (dot included, already lowercased).
    std::string ext = ExtensionOf(fileName);
    if (!ext.empty()) ext.erase(0, 1);
    // Blueprint ch. 4.3: the gallery decodes JPEG, PNG, WebP, AVIF, HEIC,
    // PSD, RAW and SVG via WIC/Direct2D -- Quick Look types must match.
    if (ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "gif" || ext == "webp" ||
        ext == "bmp" || ext == "avif" || ext == "heic" || ext == "heif" || ext == "svg" ||
        ext == "psd" || ext == "tif" || ext == "tiff" || ext == "cr2" || ext == "nef" ||
        ext == "arw" || ext == "dng") {
      return PreviewKind::Image;
    }
    if (ext == "pdf") return PreviewKind::Pdf;
    if (ext == "txt" || ext == "md" || ext == "cpp" || ext == "h" || ext == "hpp" ||
        ext == "c" || ext == "cc" || ext == "py" || ext == "java" || ext == "js" ||
        ext == "ts" || ext == "json" || ext == "xml" || ext == "yaml" || ext == "yml" ||
        ext == "ini" || ext == "cfg" || ext == "log" || ext == "csv" || ext == "html" ||
        ext == "css" || ext == "url") {
      return PreviewKind::Text;
    }
    if (ext == "mp4" || ext == "mov" || ext == "mkv" || ext == "avi" || ext == "webm" ||
        ext == "mp3" || ext == "wav" || ext == "flac" || ext == "ogg" || ext == "m4a") {
      return PreviewKind::Media;
    }
    return PreviewKind::None;
  }

  struct Decision {
    PreviewKind kind = PreviewKind::None;
    bool exceedsLimit = false;  // decodable, but the UI must warn (bomb hygiene)
  };

  static Decision Decide(const ArchiveItem& item, uint64_t limit = kDefaultByteLimit) {
    Decision d;
    if (item.isDir) {
      d.kind = PreviewKind::Directory;
      return d;
    }
    d.kind = KindByName(FileNameOf(item.path));
    d.exceedsLimit = item.size > limit;
    return d;
  }
};

class SolidBlockPrefetcher {
 public:
  // Focus first, then its in-block successors in index order, then the block
  // prefix (wrap). Directories are skipped; the first item that would blow the
  // byte budget stops the plan (no holes, deterministic order).
  static std::vector<uint32_t> Plan(const ArchiveCatalog& catalog, uint32_t focus,
                                    uint64_t budget) {
    std::vector<uint32_t> plan;
    const ArchiveItem* focusItem = catalog.FindByIndex(focus);
    if (!focusItem || focusItem->isDir) return plan;
    const uint32_t block = focusItem->block;
    uint64_t spent = 0;
    auto tryAdd = [&](uint32_t index) {
      const ArchiveItem* item = catalog.FindByIndex(index);
      if (!item || item->isDir || item->block != block) return;
      if (spent + item->size > budget) return;
      spent += item->size;
      plan.push_back(index);
    };
    tryAdd(focus);
    const uint32_t last = static_cast<uint32_t>(catalog.Size());
    for (uint32_t i = focus + 1; i < last; ++i) tryAdd(i);
    for (uint32_t i = 0; i < focus; ++i) tryAdd(i);
    return plan;
  }
};

}  // namespace prism
