// PrismArchive -- PrismCore
// ListColumns: the four columns the default (list) view shows, and the words
// that fill them -- 名称 / 压缩后大小 / 原始大小 / 类型, Bandizip's order.
//
// This header owns the COLUMN MODEL (order, semantic role, header words, the
// type-column vocabulary and the header's sort state). It deliberately owns no
// geometry: every rectangle comes from WindowLayout (the ONE geometry
// authority), so a test can assert "the columns do not overlap" against the
// same numbers the renderer paints with.
//
// WHY THE ORDER IS name / packed / original / type AND NOT name / original /
// packed / ratio (the pre-C122 list): the migration target is Bandizip, whose
// header reads 名称 | 压缩后大小 | 原始大小 | 类型 (docs/UI-AUDIT-BANDIZIP.md
// §3 and the design proposal's red line #5). Two of the four cells are sizes
// and both are RIGHT-aligned; the type cell is a WORD, not a number.
//
// THE TYPE COLUMN (design proposal §5.3): five Chinese class names --
// 文件夹 / 压缩文件 / 图片 / 文本 / 二进制. An extension this table does not
// know renders the EMPTY STRING, never "未知": the audit's anti-pattern #7
// ("unknown fields are not rendered; '未知' is not a value") is the rule here,
// and C122's L4 asserts it with .xyz.
#pragma once

#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>

#include "ListSortModel.hpp"  // TypeTagOf: the ONE extension parser (dot rules)

namespace prism::list {

// --------------------------------------------------------------- the columns
// The enum VALUE is the column ORDER, left to right. WindowLayout reads the
// same order when it lays the header out, so "the renderer painted the columns
// in a different order than the model" is not a state this code can reach.
enum class Column : std::uint8_t {
  Name = 0,
  PackedSize,
  OriginalSize,
  Type,
};

inline constexpr int kColumnCount = 4;

inline constexpr Column kColumnOrder[kColumnCount] = {
    Column::Name, Column::PackedSize, Column::OriginalSize, Column::Type};

inline const char* ColumnName(Column column) {
  switch (column) {
    case Column::Name: return "Name";
    case Column::PackedSize: return "PackedSize";
    case Column::OriginalSize: return "OriginalSize";
    case Column::Type: return "Type";
  }
  return "?";
}

// The header words, verbatim from Bandizip's 6.29 header (UI-AUDIT §3). UTF-8,
// like every other user-visible literal in PrismCore.
inline std::string_view ColumnTitle(Column column) {
  switch (column) {
    case Column::Name: return "名称";
    case Column::PackedSize: return "压缩后大小";
    case Column::OriginalSize: return "原始大小";
    case Column::Type: return "类型";
  }
  return {};
}

// The two size columns are NUMBERS: monospaced digits, right-aligned to the
// column baseline. The name and type cells are words, left-aligned.
inline constexpr bool ColumnIsNumeric(Column column) {
  return column == Column::PackedSize || column == Column::OriginalSize;
}

// ------------------------------------------------------------ the type column
// D3 NEGATIVE CONTROL: PRISM_C122_MUTANT_NO_TYPE turns every type cell into the
// empty string -- the "we never wired the column" build. L4 MUST go red; the
// verdict is PASS_REGULAR_EXPRESSION on the marker (CONTRACTS D26/D27), never
// WILL_FAIL.
#if defined(PRISM_C122_MUTANT_NO_TYPE)
inline constexpr bool kTypeLabelsEnabled = false;
#else
inline constexpr bool kTypeLabelsEnabled = true;
#endif

// Runtime proof the injected arm really compiled in (CONTRACTS D3): the mutant
// suite prints its CAUGHT marker only when this answers false.
inline constexpr bool TypeLabelsEnabled() { return kTypeLabelsEnabled; }

// A folder is not an extension question.
inline constexpr std::string_view kFolderLabel = "文件夹";
inline constexpr std::string_view kArchiveLabel = "压缩文件";
inline constexpr std::string_view kImageLabel = "图片";
inline constexpr std::string_view kTextLabel = "文本";
inline constexpr std::string_view kBinaryLabel = "二进制";

namespace detail {

inline bool InList(std::string_view tag, const std::string_view* list, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (tag == list[i]) return true;
  }
  return false;
}

}  // namespace detail

// Five classes, or the EMPTY string when the extension is not one we can name.
// The four extension classes are explicit tables: "everything not in a table is
// 二进制" would turn .xyz into 二进制, and the card's L4 says an unknown
// extension is NOTHING (the type cell stays blank).
inline std::string_view TypeLabelOf(std::string_view name, bool isDir) {
#if !defined(PRISM_C122_MUTANT_NO_TYPE)
  if (isDir) return kFolderLabel;
  const std::string tag = TypeTagOf(name);
  if (tag.empty()) return {};
  const std::string_view view(tag);
  constexpr std::string_view kArchives[] = {
      "7z",  "zip", "rar",  "tar", "gz",  "tgz", "bz2", "tbz2", "xz",  "txz",
      "zst", "lz",  "lzma", "cab", "arj", "ace", "iso", "jar",  "apk", "deb",
      "rpm", "z",   "cpio"};
  constexpr std::string_view kImages[] = {
      "png",  "jpg",  "jpeg", "gif",  "webp", "bmp",  "tif", "tiff",
      "svg",  "heic", "heif", "avif", "ico",  "psd",  "tga", "raw",
      "cr2",  "nef",  "arw",  "dng",  "jfif", "jpe"};
  constexpr std::string_view kTexts[] = {
      "txt",  "md",   "markdown", "log",  "csv",  "tsv",  "json", "xml",
      "yaml", "yml",  "ini",      "cfg",  "conf", "toml", "html", "htm",
      "css",  "scss", "less",     "js",   "mjs",  "cjs",  "ts",   "tsx",
      "jsx",  "c",    "cc",       "cpp",  "cxx",  "h",    "hpp",  "hxx",
      "cs",   "java", "kt",       "py",   "rb",   "go",   "rs",   "php",
      "sh",   "bash", "zsh",      "ps1",  "bat",  "cmd",  "sql",  "cmake",
      "make", "mk",   "rtf",      "srt",  "vtt",  "tex",  "diff", "patch",
      // Documents share the 文本 class: with five buckets (文件夹 / 压缩文件 /
      // 图片 / 文本 / 二进制) a PDF or a Word file IS a text document, and a
      // blank 类型 cell on the first screen reads as a bug.
      "pdf",  "doc",  "docx",     "xls",  "xlsx", "ppt",  "pptx", "odt",
      "ods",  "odp",  "epub",     "mobi"};
  constexpr std::string_view kBinaries[] = {
      "bin", "exe", "dll", "sys",  "dat",   "o",    "obj",  "lib", "so",
      "dylib", "a", "class", "wasm", "pdb", "msi",   "dmp",  "com", "scr",
      "drv", "ocx", "cpl", "efi"};
  if (detail::InList(view, kArchives, std::size(kArchives))) return kArchiveLabel;
  if (detail::InList(view, kImages, std::size(kImages))) return kImageLabel;
  if (detail::InList(view, kTexts, std::size(kTexts))) return kTextLabel;
  if (detail::InList(view, kBinaries, std::size(kBinaries))) return kBinaryLabel;
#else
  (void)name;
  (void)isDir;
#endif
  return {};
}

// ------------------------------------------------------- header sort (C122 §3)
// "Clickable header, UI state only": the window applies the click to its OWN
// sort spec (which re-orders the VIEW -- the archive's engine order is never
// touched), and this value decides which column shows the arrow and which way
// it points. A fresh list starts on 名称 ascending, like Bandizip.
struct HeaderSort {
  Column column = Column::Name;
  bool ascending = true;

  constexpr bool operator==(const HeaderSort&) const = default;
};

// First click on a column selects it ascending; a second click on the SAME
// column flips the direction. Clicking a different column starts over at
// ascending, so the state cannot drift into "descending because the previous
// column was".
inline constexpr HeaderSort NextSort(HeaderSort current, Column clicked) {
  if (current.column == clicked) {
    current.ascending = !current.ascending;
    return current;
  }
  current.column = clicked;
  current.ascending = true;
  return current;
}

}  // namespace prism::list
