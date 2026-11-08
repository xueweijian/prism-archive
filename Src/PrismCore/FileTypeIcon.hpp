// PrismArchive -- PrismCore
// C124: THE FILE-TYPE GLYPH -- four kinds, one 16 x 16 box, no grey bar.
//
// THE MEASURED DEFECT (UI-AUDIT 12.6 item 2). `readme.md` and `data.bin` were
// drawn as the SAME 10 x 25 grey bar, pixel for pixel: the list had a file icon
// whose shape carried no information at all, while Bandizip's `.bin` is a disk
// and its `.md` is a page (UI-AUDIT 3). The list mode had no file glyph at all.
//
// WHAT THIS FILE IS. The CLASSIFIER (extension -> kind, pure text, testable on
// the Linux lane) and the GEOMETRY (each kind's silhouette as a handful of
// shapes in a 16 DIP box). The renderer paints the shapes; the suite compares
// the pixels of two kinds, so "they differ" is a measurement of the frame and
// not a claim about this table.
//
// WHY SHAPES AND NOT BITMAPS. C100's rule: 16 px icons are vector, so they stay
// crisp at every DPI (C120 made the process PerMonitorV2 -- a bitmap icon would
// be resampled). Stroke width is the card's 16 px tier: 1.25 DIP
// (DESIGN-PROPOSAL 4.3), a single number for every kind.
//
// The folder is NOT classified here: "is a directory" is the archive's fact
// (ArchiveItem::isDir), and the folder glyph is the gold material (C124's
// FolderHighlight/Body/Shadow tokens), not a file-type glyph.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string_view>

namespace prism::icons {

// DESIGN-PROPOSAL 4.3: two icon sizes in the product (20 toolbar / 16 list and
// tree) and one stroke per size. A third size is a design violation, not a
// preference, so the box is a constant rather than a parameter.
inline constexpr float kIconSizeDip = 16.0f;
inline constexpr float kIconStrokeDip = 1.25f;

enum class FileKind : uint8_t { Document = 0, Image, Code, Binary, Unknown };

inline constexpr std::size_t kFileKindCount = 5;

inline constexpr FileKind kFileKinds[kFileKindCount] = {
    FileKind::Document, FileKind::Image, FileKind::Code, FileKind::Binary,
    FileKind::Unknown};

inline constexpr std::string_view FileKindName(FileKind kind) {
  switch (kind) {
    case FileKind::Document: return "document";
    case FileKind::Image: return "image";
    case FileKind::Code: return "code";
    case FileKind::Binary: return "binary";
    case FileKind::Unknown: return "unknown";
  }
  return "?";
}

// D2/K: a failed assertion prints the value it got, so every enum here has a
// stream operator. Without one the harness's CheckEq does not compile, and a
// "the test would not build" is not the same as "the assertion can fail".
inline std::ostream& operator<<(std::ostream& os, FileKind kind) {
  return os << FileKindName(kind);
}

// ---------------------------------------------------------------------------
// Classification. The extension table is deliberately short: it names the
// formats a file manager is expected to recognise, and everything else is
// Unknown -- a wrong guess is worse than an honest page shape.
// ---------------------------------------------------------------------------
namespace detail {

inline constexpr std::string_view kDocumentExt[] = {"md",   "txt", "rtf",  "log",
                                                   "csv",  "pdf", "doc",  "docx",
                                                   "odt",  "xls", "xlsx", "ppt",
                                                   "pptx", "epub"};
inline constexpr std::string_view kImageExt[] = {"png", "jpg",  "jpeg", "gif",
                                                "bmp", "webp", "tif",  "tiff",
                                                "heic", "ico", "svg",  "avif"};
inline constexpr std::string_view kCodeExt[] = {"cpp",  "hpp", "h",    "c",   "cc",
                                               "cs",   "js",  "ts",   "jsx", "tsx",
                                               "py",   "rs",  "go",   "java", "json",
                                               "xml",  "yml", "yaml", "sh",  "ps1",
                                               "html", "css", "cmake", "toml", "ini"};
inline constexpr std::string_view kBinaryExt[] = {"bin",  "exe", "dll", "so",  "obj",
                                                 "lib",  "dat", "7z",  "zip", "rar",
                                                 "tar",  "gz",  "xz",  "zst", "bz2",
                                                 "iso",  "msi", "cab", "class"};

template <std::size_t N>
inline bool Contains(const std::string_view (&table)[N], std::string_view ext) {
  for (std::size_t i = 0; i < N; ++i) {
    if (table[i] == ext) return true;
  }
  return false;
}

// Lower-case ASCII only: an archive may hold "README.MD", and the glyph must not
// depend on the author's shift key.
inline std::string_view ExtensionOf(std::string_view path) {
  const std::size_t slash = path.find_last_of("/\\");
  const std::size_t dot = path.find_last_of('.');
  if (dot == std::string_view::npos) return {};
  if (slash != std::string_view::npos && dot < slash) return {};
  std::string_view ext = path.substr(dot + 1);
  if (ext.size() > 8) return {};  // not an extension, a dot inside a long name
  return ext;
}

inline bool EqualsLower(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    char c = a[i];
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c != b[i]) return false;
  }
  return true;
}

}  // namespace detail

inline FileKind KindFromPath(std::string_view path) {
  const std::string_view ext = detail::ExtensionOf(path);
  if (ext.empty()) return FileKind::Unknown;
  const auto matches = [ext](const auto& table) {
    for (const std::string_view candidate : table) {
      if (detail::EqualsLower(ext, candidate)) return true;
    }
    return false;
  };
  if (matches(detail::kDocumentExt)) return FileKind::Document;
  if (matches(detail::kImageExt)) return FileKind::Image;
  if (matches(detail::kCodeExt)) return FileKind::Code;
  if (matches(detail::kBinaryExt)) return FileKind::Binary;
  return FileKind::Unknown;
}

// ---------------------------------------------------------------------------
// Geometry. Coordinates are DIP inside the 16 x 16 box: x grows right, y grows
// down, and (0,0) is the box's top-left corner. Every kind has a DIFFERENT
// silhouette -- that is the whole point of the card -- and every kind is built
// from the same five primitives, so the painter has one loop.
// ---------------------------------------------------------------------------
enum class ShapeType : uint8_t { StrokeRect, FillRect, Line, Ellipse, FillEllipse };

struct Shape {
  ShapeType type = ShapeType::StrokeRect;
  float x0 = 0.0f;
  float y0 = 0.0f;
  float x1 = 0.0f;
  float y1 = 0.0f;
  float stroke = kIconStrokeDip;
};

struct Geometry {
  const Shape* shapes = nullptr;
  std::size_t count = 0;
};

namespace detail {

// A page with three text rows: the document shape.
inline constexpr Shape kDocumentShapes[] = {
    {ShapeType::StrokeRect, 2.5f, 1.5f, 13.5f, 14.5f, kIconStrokeDip},
    {ShapeType::Line, 5.0f, 5.0f, 11.0f, 5.0f, kIconStrokeDip},
    {ShapeType::Line, 5.0f, 8.0f, 11.0f, 8.0f, kIconStrokeDip},
    {ShapeType::Line, 5.0f, 11.0f, 9.0f, 11.0f, kIconStrokeDip},
};

// A page with a horizon and a sun: the image shape. Two solid marks, so the
// silhouette survives at 16 px even with the page outline 1 px wide.
inline constexpr Shape kImageShapes[] = {
    {ShapeType::StrokeRect, 2.5f, 1.5f, 13.5f, 14.5f, kIconStrokeDip},
    {ShapeType::Line, 3.5f, 12.0f, 7.5f, 6.5f, kIconStrokeDip},
    {ShapeType::Line, 7.5f, 6.5f, 11.0f, 12.0f, kIconStrokeDip},
    {ShapeType::FillEllipse, 10.0f, 4.5f, 11.6f, 6.1f, 0.0f},
};

// Two chevrons and a slash: the code shape. This is the one silhouette that is
// unmistakable when it is 10 px wide.
inline constexpr Shape kCodeShapes[] = {
    {ShapeType::Line, 7.0f, 3.5f, 3.0f, 8.0f, kIconStrokeDip},
    {ShapeType::Line, 3.0f, 8.0f, 7.0f, 12.5f, kIconStrokeDip},
    {ShapeType::Line, 9.0f, 3.5f, 13.0f, 8.0f, kIconStrokeDip},
    {ShapeType::Line, 13.0f, 8.0f, 9.0f, 12.5f, kIconStrokeDip},
    {ShapeType::Line, 9.5f, 3.0f, 6.5f, 13.0f, kIconStrokeDip},
};

// A disk (lid ellipse, two walls, one band): the binary shape -- Bandizip's own
// vocabulary for a .bin (UI-AUDIT 3).
inline constexpr Shape kBinaryShapes[] = {
    {ShapeType::Ellipse, 3.0f, 1.5f, 13.0f, 5.5f, kIconStrokeDip},
    {ShapeType::Line, 3.0f, 3.5f, 3.0f, 11.5f, kIconStrokeDip},
    {ShapeType::Line, 13.0f, 3.5f, 13.0f, 11.5f, kIconStrokeDip},
    {ShapeType::Ellipse, 3.0f, 9.5f, 13.0f, 13.5f, kIconStrokeDip},
    {ShapeType::Line, 3.0f, 8.0f, 13.0f, 8.0f, kIconStrokeDip},
};

// A page with a question stub: the honest "I do not know" shape.
inline constexpr Shape kUnknownShapes[] = {
    {ShapeType::StrokeRect, 2.5f, 1.5f, 13.5f, 14.5f, kIconStrokeDip},
    {ShapeType::Line, 6.0f, 6.0f, 10.0f, 6.0f, kIconStrokeDip},
    {ShapeType::Line, 8.0f, 6.0f, 8.0f, 9.5f, kIconStrokeDip},
    {ShapeType::FillEllipse, 7.4f, 11.0f, 8.6f, 12.2f, 0.0f},
};

template <std::size_t N>
inline constexpr Geometry Make(const Shape (&shapes)[N]) {
  return Geometry{shapes, N};
}

}  // namespace detail

inline constexpr Geometry GeometryOf(FileKind kind) {
  switch (kind) {
    case FileKind::Document: return detail::Make(detail::kDocumentShapes);
    case FileKind::Image: return detail::Make(detail::kImageShapes);
    case FileKind::Code: return detail::Make(detail::kCodeShapes);
    case FileKind::Binary: return detail::Make(detail::kBinaryShapes);
    case FileKind::Unknown: return detail::Make(detail::kUnknownShapes);
  }
  return detail::Make(detail::kUnknownShapes);
}

// Number of primitives a kind needs. Exposed because "two kinds differ" is
// worth asserting on the table too (the Windows suite asserts it on pixels):
// an edit that made two kinds share a silhouette would change this number.
inline constexpr std::size_t ShapeCount(FileKind kind) { return GeometryOf(kind).count; }

}  // namespace prism::icons
