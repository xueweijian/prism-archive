// PrismArchive -- PrismCore
// ListSortModel: the archive list's order, its row type table and its column
// model (blueprint ch. 3.2 "列表": 名称 / 原始大小 / 压缩后 / 压缩率, folders
// first, Bandizip-grade multi-key sorting). Pure decisions -- the Windows
// renderer paints what this module computes; nothing here touches a window, a
// thread or 7z.dll.
//
// Three rules below are contracts, not taste:
//   * folders group first in BOTH directions: a descending size sort must not
//     bury the folders under the files, and a list that drops its folders on
//     the second header click reads as a bug;
//   * names compare NATURALLY and by CODE POINT: file2 < file10, and UTF-8
//     bytes are compared UNSIGNED (a signed char would invert every CJK name
//     on MSVC while the Linux lane stayed green);
//   * a tie is a real tie. The comparator refuses to invent an order for equal
//     rows, so stable_sort keeps the catalog order and the selection does not
//     jump between two rows whose keys happen to match.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace prism::list {

// ---------------------------------------------------------------------------
// Rows
// ---------------------------------------------------------------------------

// One visible list row. catalogIndex is the ONLY identity: sorting and
// filtering move rows around, so anything that remembers "the user selected
// this" must remember the catalog index, never a row position.
struct Row {
  uint32_t catalogIndex = 0;
  std::string name;       // UTF-8 display name (may contain '/')
  std::string typeTag;    // lowercase extension without the dot, "" for folders
  uint64_t size = 0;      // uncompressed bytes
  uint64_t packSize = 0;  // compressed bytes
  int64_t mtime = 0;      // platform ticks (FILETIME on Windows)
  bool isDir = false;
};

// ---------------------------------------------------------------------------
// UTF-8 / ASCII helpers (shared with FilterModel)
// ---------------------------------------------------------------------------

namespace detail {

constexpr bool IsAsciiDigit(unsigned char c) { return c >= '0' && c <= '9'; }

// ASCII-only case folding. Full Unicode folding needs ICU, which this project
// deliberately does not link; CJK has no case, so byte equality is the honest
// answer for it.
constexpr unsigned char FoldAscii(unsigned char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<unsigned char>(c + 32) : c;
}

inline size_t DigitRunEnd(std::string_view s, size_t at) {
  size_t end = at;
  while (end < s.size() && IsAsciiDigit(static_cast<unsigned char>(s[end]))) ++end;
  return end;
}

}  // namespace detail

// Byte length of the UTF-8 sequence starting at s[at] (1..4). A malformed or
// truncated sequence consumes exactly one byte, so a matcher can never stall or
// read past the end of the buffer.
inline size_t Utf8SequenceLength(std::string_view s, size_t at) {
  if (at >= s.size()) return 0;
  const unsigned char lead = static_cast<unsigned char>(s[at]);
  size_t need = 0;
  if ((lead & 0x80u) == 0x00u) {
    need = 1;
  } else if ((lead & 0xE0u) == 0xC0u) {
    need = 2;
  } else if ((lead & 0xF0u) == 0xE0u) {
    need = 3;
  } else if ((lead & 0xF8u) == 0xF0u) {
    need = 4;
  } else {
    return 1;  // stray continuation or 5/6-byte form: consume one byte
  }
  if (at + need > s.size()) return 1;
  for (size_t i = 1; i < need; ++i) {
    if ((static_cast<unsigned char>(s[at + i]) & 0xC0u) != 0x80u) return 1;
  }
  return need;
}

// Natural, code-point-ordered, case-insensitive-first name comparison:
//   file2 < file10            digit runs compare by NUMERIC value
//   x7   < x007               numerically equal -> fewer leading zeros first
//   File2 < file2             ASCII case breaks an otherwise perfect tie (upper first)
//   z.txt < 中文.txt          UTF-8 bytes compared UNSIGNED == code point order
// Returns <0, 0, >0.
inline int NaturalCompare(std::string_view a, std::string_view b) {
#if defined(PRISM_C13_MUTANT_LEXICOGRAPHIC_NAMES)
  // MUTATION (Tests/suites/c13-listmodel.cmake, WILL_FAIL): plain byte
  // lexicographic order, so file10 sorts before file2.
  const size_t common = (std::min)(a.size(), b.size());
  const int cmp = std::char_traits<char>::compare(a.data(), b.data(), common);
  if (cmp != 0) return cmp < 0 ? -1 : 1;
  if (a.size() == b.size()) return 0;
  return a.size() < b.size() ? -1 : 1;
#else
  size_t i = 0;
  size_t j = 0;
  int caseTie = 0;  // first ASCII case difference, used only if everything else ties

  while (i < a.size() && j < b.size()) {
    const unsigned char ca = static_cast<unsigned char>(a[i]);
    const unsigned char cb = static_cast<unsigned char>(b[j]);

    if (detail::IsAsciiDigit(ca) && detail::IsAsciiDigit(cb)) {
      const size_t ai = detail::DigitRunEnd(a, i);
      const size_t bj = detail::DigitRunEnd(b, j);
      size_t as = i;
      size_t bs = j;
      while (as < ai && a[as] == '0') ++as;
      while (bs < bj && b[bs] == '0') ++bs;
      const size_t alen = ai - as;
      const size_t blen = bj - bs;
      // More significant digits means a bigger number; no integer parsing, so a
      // 40-digit run cannot overflow into a false tie.
      if (alen != blen) return alen < blen ? -1 : 1;
      for (size_t k = 0; k < alen; ++k) {
        if (a[as + k] != b[bs + k]) return a[as + k] < b[bs + k] ? -1 : 1;
      }
      const size_t az = as - i;
      const size_t bz = bs - j;
      if (az != bz) return az < bz ? -1 : 1;  // "7" before "007"
      i = ai;
      j = bj;
      continue;
    }

    const unsigned char fa = detail::FoldAscii(ca);
    const unsigned char fb = detail::FoldAscii(cb);
    if (fa != fb) return fa < fb ? -1 : 1;
    if (ca != cb && caseTie == 0) caseTie = ca < cb ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return caseTie;
#endif
}

// ---------------------------------------------------------------------------
// Sort keys
// ---------------------------------------------------------------------------

enum class SortKey : uint8_t {
  kName = 0,
  kOriginalSize,
  kPackedSize,
  kRatio,
  kMtime,
  kType,
};

inline const char* SortKeyName(SortKey key) {
  switch (key) {
    case SortKey::kName: return "Name";
    case SortKey::kOriginalSize: return "OriginalSize";
    case SortKey::kPackedSize: return "PackedSize";
    case SortKey::kRatio: return "Ratio";
    case SortKey::kMtime: return "Mtime";
    case SortKey::kType: return "Type";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, SortKey key) { return os << SortKeyName(key); }

enum class SortDirection : uint8_t { kAscending = 0, kDescending };

inline std::ostream& operator<<(std::ostream& os, SortDirection direction) {
  return os << (direction == SortDirection::kAscending ? "Ascending" : "Descending");
}

struct SortSpec {
  SortKey key = SortKey::kName;
  SortDirection direction = SortDirection::kAscending;
};

// Multi-key: the keys are compared in order, the first non-tie decides.
struct MultiSortSpec {
  std::vector<SortSpec> keys;
  bool dirsFirst = true;
};

// ---------------------------------------------------------------------------
// Ratio: exact, not rounded
// ---------------------------------------------------------------------------

// 64x64 -> 128-bit product. MSVC has no __int128, and a ratio comparison must
// not overflow into "every big archive tie".
struct UInt128 {
  uint64_t hi = 0;
  uint64_t lo = 0;
};

inline UInt128 MultiplyU64(uint64_t a, uint64_t b) {
  const uint64_t aLo = a & 0xFFFFFFFFull;
  const uint64_t aHi = a >> 32;
  const uint64_t bLo = b & 0xFFFFFFFFull;
  const uint64_t bHi = b >> 32;
  const uint64_t loLo = aLo * bLo;
  const uint64_t loHi = aLo * bHi;
  const uint64_t hiLo = aHi * bLo;
  const uint64_t hiHi = aHi * bHi;
  const uint64_t mid = (loLo >> 32) + (loHi & 0xFFFFFFFFull) + (hiLo & 0xFFFFFFFFull);
  UInt128 out;
  out.lo = (loLo & 0xFFFFFFFFull) | (mid << 32);
  out.hi = hiHi + (loHi >> 32) + (hiLo >> 32) + (mid >> 32);
  return out;
}

inline UInt128 SubtractU64(UInt128 value, uint64_t sub) {
  const uint64_t oldLo = value.lo;
  value.lo -= sub;
  if (oldLo < sub) value.hi -= 1;
  return value;
}

// Cross-multiplied ratio compare: a.pack / a.size  vs  b.pack / b.size.
// size == 0 (a folder) has no ratio and sorts as the lowest one.
inline int CompareRatio(const Row& a, const Row& b) {
  if (a.size == 0 && b.size == 0) return 0;
  if (a.size == 0) return -1;
  if (b.size == 0) return 1;
  const UInt128 x = MultiplyU64(a.packSize, b.size);
  const UInt128 y = MultiplyU64(b.packSize, a.size);
  if (x.hi != y.hi) return x.hi < y.hi ? -1 : 1;
  if (x.lo != y.lo) return x.lo < y.lo ? -1 : 1;
  return 0;
}

// Display percent 0..100, rounded to nearest. Exact integer math: the quotient
// is bounded by 100 (packed <= size), so repeated subtraction is a short loop
// and no floating point (whose rounding would disagree with CompareRatio).
inline uint32_t RatioPercent(const Row& row) {
  if (row.size == 0) return 0;
  if (row.packSize >= row.size) return 100;
  UInt128 remainder = MultiplyU64(row.packSize, 100);
  uint32_t percent = 0;
  while (remainder.hi != 0 || remainder.lo >= row.size) {
    remainder = SubtractU64(remainder, row.size);
    ++percent;
    if (percent >= 100) return 100;  // saturation guard, cannot spin
  }
  if (remainder.lo >= row.size - remainder.lo) ++percent;  // round half up
  return percent;
}

// ---------------------------------------------------------------------------
// Type table (the 类型 sort key and the 类型 filter share it)
// ---------------------------------------------------------------------------

enum class TypeClass : uint8_t {
  kFolders = 0,
  kDocuments,
  kImages,
  kMedia,
  kArchives,
  kCode,
  kOther,
};

inline std::ostream& operator<<(std::ostream& os, TypeClass type) {
  switch (type) {
    case TypeClass::kFolders: return os << "Folders";
    case TypeClass::kDocuments: return os << "Documents";
    case TypeClass::kImages: return os << "Images";
    case TypeClass::kMedia: return os << "Media";
    case TypeClass::kArchives: return os << "Archives";
    case TypeClass::kCode: return os << "Code";
    case TypeClass::kOther: return os << "Other";
  }
  return os << "?";
}

// Lowercased extension without the dot. "dir/.gitignore" and "name." have none
// (Explorer's rule: a dot at the start of the FILE NAME is not an extension).
inline std::string TypeTagOf(std::string_view name) {
  const size_t slash = name.find_last_of('/');
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos) return std::string();
  const size_t base = (slash == std::string_view::npos) ? 0 : slash + 1;
  if (dot == base) return std::string();
  if (dot + 1 >= name.size()) return std::string();
  std::string tag;
  tag.reserve(name.size() - dot - 1);
  for (size_t i = dot + 1; i < name.size(); ++i) {
    tag.push_back(static_cast<char>(detail::FoldAscii(static_cast<unsigned char>(name[i]))));
  }
  return tag;
}

inline TypeClass ClassifyType(std::string_view name, bool isDir) {
  if (isDir) return TypeClass::kFolders;
  const std::string tag = TypeTagOf(name);
  if (tag.empty()) return TypeClass::kOther;
  constexpr std::string_view kDocuments[] = {"txt", "md",  "pdf",  "doc",  "docx", "rtf",
                                             "odt", "xls", "xlsx", "ppt",  "pptx", "csv"};
  constexpr std::string_view kImages[] = {"png", "jpg",  "jpeg", "gif", "webp", "bmp",
                                          "tif", "tiff", "svg",  "heic", "avif", "ico"};
  constexpr std::string_view kMedia[] = {"mp3", "flac", "wav", "m4a", "aac", "ogg",
                                         "mp4", "mkv",  "avi", "mov", "wmv", "webm"};
  constexpr std::string_view kArchives[] = {"7z", "zip", "rar", "tar", "gz",
                                            "bz2", "xz", "zst", "iso"};
  constexpr std::string_view kCode[] = {"c",   "cc",   "cpp",  "h",    "hpp", "rs",
                                        "go",  "py",   "js",   "ts",   "json", "xml",
                                        "yml", "yaml", "cs",   "java", "sh",  "ps1",
                                        "cmake"};
  for (std::string_view t : kDocuments) {
    if (tag == t) return TypeClass::kDocuments;
  }
  for (std::string_view t : kImages) {
    if (tag == t) return TypeClass::kImages;
  }
  for (std::string_view t : kMedia) {
    if (tag == t) return TypeClass::kMedia;
  }
  for (std::string_view t : kArchives) {
    if (tag == t) return TypeClass::kArchives;
  }
  for (std::string_view t : kCode) {
    if (tag == t) return TypeClass::kCode;
  }
  return TypeClass::kOther;
}

// ---------------------------------------------------------------------------
// Comparison
// ---------------------------------------------------------------------------

// One-key comparison, direction-independent (the caller applies the direction).
inline int CompareByKey(const Row& a, const Row& b, SortKey key) {
  switch (key) {
    case SortKey::kName:
      return NaturalCompare(a.name, b.name);
    case SortKey::kOriginalSize:
      return a.size < b.size ? -1 : (a.size > b.size ? 1 : 0);
    case SortKey::kPackedSize:
      return a.packSize < b.packSize ? -1 : (a.packSize > b.packSize ? 1 : 0);
    case SortKey::kRatio:
      return CompareRatio(a, b);
    case SortKey::kMtime:
      return a.mtime < b.mtime ? -1 : (a.mtime > b.mtime ? 1 : 0);
    case SortKey::kType:
      // Within one type the names still have to be in order: a type-only sort
      // that leaves the rows in catalog order looks broken to a user.
      if (a.typeTag != b.typeTag) return a.typeTag < b.typeTag ? -1 : 1;
      return NaturalCompare(a.name, b.name);
  }
  return 0;
}

// The list comparator. Two rows with equal keys are NOT ordered: returning
// false both ways is what lets stable_sort keep the catalog order.
inline bool RowLess(const Row& a, const Row& b, const MultiSortSpec& spec) {
#if !defined(PRISM_C13_MUTANT_DIRS_INTERLEAVED)
  if (spec.dirsFirst && a.isDir != b.isDir) return a.isDir;
#endif
  for (const SortSpec& key : spec.keys) {
    const int cmp = CompareByKey(a, b, key.key);
    if (cmp == 0) continue;
    return key.direction == SortDirection::kAscending ? cmp < 0 : cmp > 0;
  }
  return false;
}

// Stable by construction. Rows that tie stay in the order the catalog handed
// them over, which is what keeps a selection attached to its row.
inline void SortRows(std::vector<Row>& rows, const MultiSortSpec& spec) {
  const auto less = [&spec](const Row& a, const Row& b) { return RowLess(a, b, spec); };
#if defined(PRISM_C13_MUTANT_UNSTABLE_SORT)
  // MUTATION (WILL_FAIL): ordering among equal keys is no longer preserved.
  std::sort(rows.begin(), rows.end(), less);
#else
  std::stable_sort(rows.begin(), rows.end(), less);
#endif
}

// ---------------------------------------------------------------------------
// Column model
// ---------------------------------------------------------------------------

enum class ColumnId : uint8_t {
  kName = 0,
  kOriginalSize,
  kPackedSize,
  kRatio,
  kMtime,
  kType,
};

inline const char* ColumnIdName(ColumnId id) {
  switch (id) {
    case ColumnId::kName: return "Name";
    case ColumnId::kOriginalSize: return "OriginalSize";
    case ColumnId::kPackedSize: return "PackedSize";
    case ColumnId::kRatio: return "Ratio";
    case ColumnId::kMtime: return "Mtime";
    case ColumnId::kType: return "Type";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, ColumnId id) { return os << ColumnIdName(id); }

inline constexpr uint32_t kMaxColumnWidth = 2000;
inline constexpr uint32_t kMinColumnFloor = 24;

struct ColumnSpec {
  ColumnId id = ColumnId::kName;
  bool visible = true;
  uint32_t width = 0;
  uint32_t minWidth = 0;
};

// The header contract: a width is always inside [minWidth, kMaxColumnWidth]
// (a clamp the model KEEPS, not a hint it records), the name column is never
// hidden, and at least one column always stays visible.
class ColumnModel {
 public:
  static ColumnModel Default() {
    ColumnModel model;
    model.columns_ = {
        {ColumnId::kName, true, 260, 120},
        {ColumnId::kOriginalSize, true, 110, 64},
        {ColumnId::kPackedSize, true, 110, 64},
        {ColumnId::kRatio, true, 90, 64},
        {ColumnId::kMtime, true, 150, 96},
        // 类型 is opt-in: the blueprint's default list is 名称/原始/压缩后/压缩率
        // plus the modification time, and a six-column header on day one is noise.
        {ColumnId::kType, false, 90, 64},
    };
    return model;
  }

  const std::vector<ColumnSpec>& Columns() const { return columns_; }

  const ColumnSpec* Find(ColumnId id) const {
    for (const ColumnSpec& column : columns_) {
      if (column.id == id) return &column;
    }
    return nullptr;
  }

  bool Visible(ColumnId id) const {
    const ColumnSpec* column = Find(id);
    return column != nullptr && column->visible;
  }

  uint32_t Width(ColumnId id) const {
    const ColumnSpec* column = Find(id);
    return column == nullptr ? 0 : column->width;
  }

  uint32_t MinWidth(ColumnId id) const {
    const ColumnSpec* column = Find(id);
    return column == nullptr ? 0 : column->minWidth;
  }

  size_t VisibleCount() const {
    size_t count = 0;
    for (const ColumnSpec& column : columns_) {
      if (column.visible) ++count;
    }
    return count;
  }

  std::vector<ColumnId> VisibleColumns() const {
    std::vector<ColumnId> out;
    for (const ColumnSpec& column : columns_) {
      if (column.visible) out.push_back(column.id);
    }
    return out;
  }

  // Total header width of the visible columns; the name column flexes to the
  // remaining canvas, so this is the floor the window has to be able to show.
  uint32_t TotalWidth() const {
    uint32_t total = 0;
    for (const ColumnSpec& column : columns_) {
      if (column.visible) total += column.width;
    }
    return total;
  }

  bool SetVisible(ColumnId id, bool visible) {
    ColumnSpec* column = Mutable(id);
    if (column == nullptr) return false;
    if (column->visible == visible) return true;
    if (!visible) {
      if (id == ColumnId::kName) return false;  // the name IS the list
      if (VisibleCount() <= 1) return false;    // never an empty header
    }
    column->visible = visible;
    return true;
  }

  // The clamp is applied here, so a caller that asks for 10 px gets the minimum
  // back (readable through Width()) instead of silently breaking the header.
  bool SetWidth(ColumnId id, uint32_t width) {
    ColumnSpec* column = Mutable(id);
    if (column == nullptr) return false;
#if defined(PRISM_C13_MUTANT_WIDTH_BYPASSES_MIN)
    column->width = width;  // MUTATION: the minimum width is only a hint
#else
    column->width = (std::max)(column->minWidth, (std::min)(width, kMaxColumnWidth));
#endif
    return true;
  }

  // Raising the minimum can widen the column (the promise is kept immediately);
  // lowering it never shrinks the current width.
  bool SetMinWidth(ColumnId id, uint32_t minWidth) {
    ColumnSpec* column = Mutable(id);
    if (column == nullptr) return false;
    const uint32_t clamped =
        (std::max)(kMinColumnFloor, (std::min)(minWidth, kMaxColumnWidth));
    column->minWidth = clamped;
    if (column->width < clamped) column->width = clamped;
    return true;
  }

 private:
  ColumnSpec* Mutable(ColumnId id) {
    for (ColumnSpec& column : columns_) {
      if (column.id == id) return &column;
    }
    return nullptr;
  }

  std::vector<ColumnSpec> columns_;
};

}  // namespace prism::list
