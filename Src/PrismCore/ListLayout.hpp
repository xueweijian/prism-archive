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

#include "CompressionRatio.hpp"
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

// C81/A3: the text in the 压缩率 cell is THE canonical view string
// (prism::CompressionRatioView) -- never a second formatter. The capsule bar
// stays decorative and is drawn only for a real shrink; an expanded entry
// (">100% (...)") or an unknown one renders the text across the column.
inline std::string RatioCellText(const SortItem& item) {
  // A directory is not a byte stream: like its "--" in the size columns its
  // ratio cell carries the canonical unknown, never a fabricated number.
  if (item.isDir) return prism::CompressionRatioView(0, 0);
  return prism::CompressionRatioView(item.size, item.packedSize);
}

inline bool RatioBarVisible(const SortItem& item) {
  return !item.isDir && item.size > 0 && item.packedSize > 0 &&
         item.packedSize <= item.size;
}

// ---------------------------------------------------------------- C101 ratio
// The compression-ratio strip used to be a full-saturation rule across the WHOLE
// ratio column: 6 px tall, alpha 1.0, the file-kind colour (#007AFF for text).
// The user's report is "刺眼荧光绿厚条" and the re-design plan §2.2 turns the bar
// into a READOUT: 4 px, radius 2, at most 56 px wide, alpha 0.55, and its hue
// comes from the MEASURED RATIO -- <=50 % green, 51..80 % amber, >80 % red.
//
// Why the tint is not the raw palette value: alpha alone does not mute. At 0.55
// over the Midnight canvas the full-saturation hues composite to HSV S 0.70
// (#34C759) .. 0.90 (#FF9F0A), because the canvas is dark and the hue is not;
// the plan's own pixel ceiling (S <= 0.45, <= 60 levels above the canvas) is
// unreachable that way. So the palette below is the HUE SOURCE and the fill is
// that hue DESATURATED toward its own mean grey (keeping hue and mean, lowering
// S) and then capped in mean so the composite stays under the ceiling. The
// threshold still decides which hue you see; the tests assert the hue angle.
enum class RatioBand : uint8_t { Low, Medium, High };

inline RatioBand RatioBandOf(uint32_t percent) {
  if (percent > 80) return RatioBand::High;
  if (percent > 50) return RatioBand::Medium;
  return RatioBand::Low;
}

struct Rgb {
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
};

// The plan's palette: #34C759 / #FF9F0A / #FF3B30.
inline Rgb RatioHueOf(RatioBand band) {
  switch (band) {
    case RatioBand::Low: return {0.204f, 0.780f, 0.349f};
    case RatioBand::Medium: return {1.000f, 0.624f, 0.039f};
    case RatioBand::High: return {1.000f, 0.231f, 0.188f};
  }
  return {1.0f, 1.0f, 1.0f};
}

// The plan's geometry and alpha. The renderer and the suite read them here, so
// "4 px / 56 px / 0.55" can never become two numbers.
inline constexpr float kRatioBarAlpha = 0.55f;
inline constexpr float kRatioBarHeight = 4.0f;
inline constexpr float kRatioBarRadius = 2.0f;
inline constexpr float kRatioBarMaxWidth = 56.0f;

// Mute: pull the hue 60 % of the way to its own mean grey. Hue and mean are
// preserved (mixing toward the mean leaves the mean where it was), S drops.
inline constexpr float kRatioBarMute = 0.60f;
// The MEAN the fill may not exceed, derived from the plan's ceiling. Measured
// on the Midnight frame: the canvas composites to 18.7 levels and the groove
// under the fill (kTileBorder, white at 12 %) to 47, so a 0.55 fill lands at
// 0.55*m + 0.45*47 and the ceiling of canvas+60 = 78.7 allows m <= 92/255 =
// 0.361. Every band is held at or below it, which is why the three bands differ
// in HUE and not in brightness.
inline constexpr float kRatioBarMeanCeiling = 0.36f;

inline Rgb RatioBarTint(uint32_t percent) {
  const Rgb hue = RatioHueOf(RatioBandOf(percent));
  const float mean = (hue.r + hue.g + hue.b) / 3.0f;
  Rgb tint{hue.r + (mean - hue.r) * kRatioBarMute,
           hue.g + (mean - hue.g) * kRatioBarMute,
           hue.b + (mean - hue.b) * kRatioBarMute};
  const float tintMean = (tint.r + tint.g + tint.b) / 3.0f;
  if (tintMean > kRatioBarMeanCeiling) {
    const float scale = kRatioBarMeanCeiling / tintMean;
    tint.r *= scale;
    tint.g *= scale;
    tint.b *= scale;
  }
  return tint;
}

// The composite the target actually shows for a fill pixel: the tint at the
// bar's alpha over the surface underneath it (the groove). Exposed so the core
// suite can assert the plan's pixel ceilings without a window.
inline Rgb RatioBarComposite(uint32_t percent, const Rgb& underneath) {
  const Rgb tint = RatioBarTint(percent);
  return {tint.r * kRatioBarAlpha + underneath.r * (1.0f - kRatioBarAlpha),
          tint.g * kRatioBarAlpha + underneath.g * (1.0f - kRatioBarAlpha),
          tint.b * kRatioBarAlpha + underneath.b * (1.0f - kRatioBarAlpha)};
}

// HSV saturation of a colour (max == 0 -> 0). The plan's pixel ceiling is on
// this number, so it lives next to the palette instead of in a test.
inline float RgbSaturation(const Rgb& color) {
  const float high = (std::max)(color.r, (std::max)(color.g, color.b));
  const float low = (std::min)(color.r, (std::min)(color.g, color.b));
  if (high <= 0.0f) return 0.0f;
  return (high - low) / high;
}

inline float RgbMean(const Rgb& color) { return (color.r + color.g + color.b) / 3.0f; }

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

// Fixed column geometry, right-anchored to the body's right edge.
//
// C109: the three numeric columns are explicit BOXES with a >= 16 px gutter
// between them, and a cell's number is right-aligned to the box's right edge --
// the COLUMN BASELINE. Before this card the renderer derived three loose X
// positions with magic +6/-18 nudges, so the "columns" had no shared edge a test
// could name and a 6.3 KB / 11.2 KB pair was only as aligned as the font's
// digits happened to be. Name flexes to fill the remainder.
inline constexpr uint32_t kColumnGutter = 16;       // DESIGN: >= 16 px between columns
inline constexpr uint32_t kColumnRightMargin = 10;  // body edge -> ratio baseline

struct ColumnLayout {
  uint32_t sizeW = 110;
  uint32_t packedW = 110;
  uint32_t ratioW = 130;

  static ColumnLayout Default(uint32_t) { return {}; }

  // Right edges are the column baselines the renderer aligns trailing text to.
  // Every Left() is exactly one gutter away from its neighbour's Right(), which
  // is what keeps the columns from touching even when a persisted width is odd.
  uint32_t ratioRight(uint32_t canvasW) const {
    return canvasW > kColumnRightMargin ? canvasW - kColumnRightMargin : 0;
  }
  uint32_t ratioLeft(uint32_t canvasW) const {
    const uint32_t right = ratioRight(canvasW);
    return right > ratioW ? right - ratioW : 0;
  }
  uint32_t packedRight(uint32_t canvasW) const {
    const uint32_t left = ratioLeft(canvasW);
    return left > kColumnGutter ? left - kColumnGutter : 0;
  }
  uint32_t packedLeft(uint32_t canvasW) const {
    const uint32_t right = packedRight(canvasW);
    return right > packedW ? right - packedW : 0;
  }
  uint32_t sizeRight(uint32_t canvasW) const {
    const uint32_t left = packedLeft(canvasW);
    return left > kColumnGutter ? left - kColumnGutter : 0;
  }
  uint32_t sizeLeft(uint32_t canvasW) const {
    const uint32_t right = sizeRight(canvasW);
    return right > sizeW ? right - sizeW : 0;
  }
  uint32_t nameRight(uint32_t canvasW) const {
    const uint32_t left = sizeLeft(canvasW);
    return left > kColumnGutter ? left - kColumnGutter : 0;
  }

  // C27 persisted the three widths and older call sites asked for a column's
  // left edge by name; keep those names meaning "left edge of the box".
  uint32_t sizeX(uint32_t canvasW) const { return sizeLeft(canvasW); }
  uint32_t packedX(uint32_t canvasW) const { return packedLeft(canvasW); }
  uint32_t ratioX(uint32_t canvasW) const { return ratioLeft(canvasW); }
};

}  // namespace prism::list
