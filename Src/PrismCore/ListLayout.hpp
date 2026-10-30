// PrismArchive -- PrismCore
// ListLayout: the main-window file list mode (blueprint ch. 3.2). Four
// columns (名称 ^ | 原始大小 | 压缩后 | 压缩率), 36 px rows, virtualization
// with overscan, semantic micro-capsule ratio bars, tnum-friendly number
// formatting, folders-first ordering. Pure decisions -- the Windows renderer
// only paints what this module computes.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "Preview.hpp"

namespace prism::list {

inline constexpr uint32_t kRowHeight = 36;

struct RowWindowResult {
  uint32_t first = 0;
  uint32_t last = 0;  // inclusive
};

// Virtualized row window for a viewport [scrollTop, scrollTop+viewportH).
inline RowWindowResult RowWindow(uint32_t scrollTop, uint32_t viewportH, uint32_t rowCount) {
  RowWindowResult w;
  if (rowCount == 0) {
    w.first = 0;
    w.last = 0;  // empty: the renderer draws only the header
    return w;
  }
  const uint32_t overscan = 3;
  w.first = scrollTop / kRowHeight;
  w.first = w.first > overscan ? w.first - overscan : 0;
  w.first = std::min<uint32_t>(w.first, rowCount - 1);
  const uint32_t visibleEnd = (scrollTop + viewportH + kRowHeight - 1) / kRowHeight;
  w.last = std::min<uint32_t>(rowCount - 1, visibleEnd + overscan - 1);
  if (w.last < w.first) w.last = w.first;
  return w;
}

// Hit-testing.
inline std::optional<uint32_t> RowAt(uint32_t y, uint32_t scrollTop) {
  const uint32_t local = scrollTop + y;
  return local / kRowHeight;
}
inline uint32_t RowAtClamped(uint32_t y, uint32_t scrollTop, uint32_t rowCount) {
  if (rowCount == 0) return 0;
  const uint32_t row = (scrollTop + y) / kRowHeight;
  return std::min<uint32_t>(row, rowCount - 1);
}

enum class BarColor : uint8_t { Teal, Blue, Green, Amber };

inline std::ostream& operator<<(std::ostream& os, BarColor color) {
  switch (color) {
    case BarColor::Teal: return os << "Teal";
    case BarColor::Blue: return os << "Blue";
    case BarColor::Green: return os << "Green";
    case BarColor::Amber: return os << "Amber";
  }
  return os << "?";
}

struct RatioBarResult {
  uint8_t percent = 0;  // packed / size, 0..100
  BarColor color = BarColor::Blue;
};

struct SortItem {
  std::string name;
  uint64_t size = 0;
  uint64_t packedSize = 0;
  bool isDir = false;
  bool encrypted = false;
  prism::PreviewKind kind = prism::PreviewKind::None;
};

// The semantic micro-capsule (blueprint ch. 3.2.3): 图像与媒体=流光青,
// 代码与文档=深空蓝, 高压缩比文本=翡翠绿, 加密受保护=琥珀金.
inline std::optional<RatioBarResult> RatioBar(const SortItem& item) {
  if (item.isDir || item.size == 0) return std::nullopt;
  RatioBarResult r;
  r.percent = static_cast<uint8_t>(
      std::min<uint64_t>(100, item.packedSize * 100 / item.size));
  if (item.encrypted) {
    r.color = BarColor::Amber;
  } else if (item.kind == prism::PreviewKind::Image ||
             item.kind == prism::PreviewKind::Media) {
    r.color = BarColor::Teal;
  } else if (item.kind == prism::PreviewKind::Pdf ||
             item.kind == prism::PreviewKind::Text) {
    r.color = BarColor::Blue;
  } else {
    r.color = BarColor::Green;
  }
  return r;
}

// tnum-friendly fixed-scale byte strings (OpenType tnum keeps digits equal
// width; the formatter keeps one decimal and stable units).
inline std::string FormatBytes(uint64_t bytes) {
  const char* units[] = {"B", "KB", "MB", "GB", "TB"};
  if (bytes == 0) return "0 B";
  double value = static_cast<double>(bytes);
  int unit = 0;
  while (value >= 1024.0 && unit < 4) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, units[unit]);
  }
  return buf;
}

// Folders first (name asc), then files (name asc).
inline void FolderFirstOrder(std::vector<SortItem>& items) {
  std::stable_sort(items.begin(), items.end(), [](const SortItem& a, const SortItem& b) {
    if (a.isDir != b.isDir) return a.isDir;  // dirs first
    return a.name < b.name;
  });
}

// Fixed column geometry: Size=110, Packed=110, Ratio=130 anchored right;
// Name flexes to fill the remainder.
struct ColumnLayout {
  uint32_t sizeW = 110;
  uint32_t packedW = 110;
  uint32_t ratioW = 130;

  static ColumnLayout Default(uint32_t) { return {}; }
  uint32_t sizeX(uint32_t canvasW) const { return canvasW - sizeW - packedW - ratioW; }
  uint32_t packedX(uint32_t canvasW) const { return canvasW - packedW - ratioW; }
  uint32_t ratioX(uint32_t canvasW) const { return canvasW - ratioW; }
};

}  // namespace prism::list
