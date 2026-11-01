// PrismArchive -- PrismCore
// ArchiveTreeView: the folder-navigation view model over a flat catalog.
//
// Catalogs are TREES; the gallery renders ONE list. This module answers the
// three questions every navigation interaction needs: "what belongs to the
// folder I am looking at", "where am I" (the breadcrumb chain) and "which
// crumb did that click land on". It is pure C++ -- the window maps pixels to
// decisions and executes them, nothing more (batch A increment 5).
//
// Design notes:
//  * Paths are PrismCore-normalized ('/' separators, no leading slash).
//  * Prefix matching requires the separator (or end): "Assets" must not own
//    "Assets2/x" -- the classic naive-starts_with bug.
//  * Ordering is 7-Zip's: folders first, then CompareFileNames_ForFolderList
//    (CPP/7zip/UI/FileManager/PanelSort.cpp:8-46) -- digit runs compare by
//    value, so "file2" precedes "file10". Deterministic, so the window can
//    reproduce a selection across navigation.
//  * Breadcrumb geometry is computed HERE, not in the window. Text widths are
//    injected (std::function) so the hit test is testable on Linux with a
//    deterministic fake width and exact on Windows via DirectWrite. The drawn
//    string and the clickable spans come from the SAME segment list, so they
//    cannot drift apart.
//
// Open-source reference (adopted, with the source lines in the comments):
//   7-Zip FileManager  CPP/7zip/UI/FileManager/
//     PanelKey.cpp:261                 VK_BACK -> OpenParentFolder()
//     PanelFolderChange.cpp:917-995    OpenParentFolder(): the folder being
//                                      left is remembered as FocusedName
//     PanelItems.cpp:684-697           restore matches that NAME while the new
//                                      list is built (not an index)
//     PanelFolderChange.cpp:1071-1079  OpenFolder(): entering a folder keeps
//                                      nothing selected ("17.02: fixed : now
//                                      we don't select first item")
//     PanelItems.cpp:578               ".." pseudo-entry only when ShowDots is
//                                      enabled, never at the root; the default
//                                      is off (Panel.h:608) -> we synthesise no
//                                      "."/".." children either.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"  // NormalizeArchivePath, SanitizeRelativePath, JoinPath

namespace prism::tree {

// A folder node that the archive never stored explicitly (only its children
// exist) has no catalog item; thumbnails and pack sizes must not borrow a
// child's index, or a folder tile would paint some unrelated file's preview.
inline constexpr uint32_t kSyntheticIndex = 0xFFFFFFFFu;

struct Entry {
  std::string path;
  bool isDir = false;
  uint64_t size = 0;
  uint32_t index = kSyntheticIndex;  // catalog index, for extract/preview dispatch
};

struct Crumb {
  std::string name;      // display label ("Icons")
  std::string folder;    // normalized folder path ("Assets/Icons"); "" = root
};

// One level up; "" means already at the root. Free function so the Navigator
// and the View share one definition (7-Zip's OpenParentFolder() does the same
// walk: PanelFolderChange.cpp:917-995).
inline std::string UpOne(std::string_view folder) {
  const std::string f = prism::NormalizeArchivePath(folder);
  const size_t slash = f.rfind('/');
  if (slash == std::string::npos) return "";
  return f.substr(0, slash);
}

struct View {
  std::vector<Entry> entries;
  std::vector<Crumb> breadcrumb;  // empty at the root

  std::string UpFrom(std::string_view folder) const { return UpOne(folder); }
};

namespace detail {

// "." / ".." are not children: they are either garbage from a hostile archive
// or a pseudo-entry some tools leak into the central directory.
inline bool IsPseudoName(std::string_view name) {
  return name == "." || name == "..";
}

inline char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// 7-Zip's CompareFileNames_ForFolderList (PanelSort.cpp:8-46) in miniature:
// case-insensitive, digit runs compared by value (leading zeros ignored, then
// the shorter run wins). Adopted because a naive byte compare orders
// "file10.txt" before "file2.txt", which looks broken to every user who has
// ever seen Explorer -- and because the engine's own UI does it this way.
inline int CompareNatural(std::string_view a, std::string_view b) {
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    const bool digitA = a[i] >= '0' && a[i] <= '9';
    const bool digitB = b[j] >= '0' && b[j] <= '9';
    if (digitA && digitB) {
      size_t startA = i, startB = j;
      while (startA < a.size() && a[startA] == '0') ++startA;
      while (startB < b.size() && b[startB] == '0') ++startB;
      size_t endA = startA, endB = startB;
      while (endA < a.size() && a[endA] >= '0' && a[endA] <= '9') ++endA;
      while (endB < b.size() && b[endB] >= '0' && b[endB] <= '9') ++endB;
      const size_t lenA = endA - startA, lenB = endB - startB;
      if (lenA != lenB) return lenA < lenB ? -1 : 1;
      for (size_t k = 0; k < lenA; ++k) {
        if (a[startA + k] != b[startB + k]) return a[startA + k] < b[startB + k] ? -1 : 1;
      }
      i = endA;
      j = endB;
      continue;
    }
    const char ca = LowerAscii(a[i]), cb = LowerAscii(b[j]);
    if (ca != cb) return ca < cb ? -1 : 1;
    ++i;
    ++j;
  }
  if (i < a.size()) return 1;
  if (j < b.size()) return -1;
  return 0;
}

}  // namespace detail

// A folder string from anywhere (user click, stored path, hostile archive) is
// reduced to a strictly relative, '/'-separated path with '..' clamped at the
// root -- the same rule PathSafety applies to extraction targets.
inline std::string SafeFolder(std::string_view folder) {
  return prism::SanitizeRelativePath(folder);
}

inline std::pair<std::string, std::vector<Crumb>> SplitBreadcrumb(
    std::string_view folder) {
  std::vector<Crumb> crumbs;
  std::string walked;
  size_t start = 0;
  while (start <= folder.size()) {
    const size_t slash = folder.find('/', start);
    const std::string name(folder.substr(start, slash == std::string_view::npos
                                                  ? std::string_view::npos
                                                  : slash - start));
    if (!name.empty()) {
      walked = walked.empty() ? name : walked + "/" + name;
      crumbs.push_back({name, walked});
    }
    if (slash == std::string_view::npos) break;
    start = slash + 1;
  }
  return {"", std::move(crumbs)};
}

inline View Build(const std::vector<ArchiveItem>& catalog,
                  std::string_view folder) {
  const std::string f = SafeFolder(folder);
  View view;
  std::string prefix;
  if (!f.empty()) {
    prefix = f + "/";
    auto [_, crumbs] = SplitBreadcrumb(f);
    view.breadcrumb = std::move(crumbs);
  }
  // Explicit directory nodes keep their own catalog index; a folder that the
  // archive only implies (children without a parent entry) gets a synthetic
  // one instead of stealing a child's index.
  std::unordered_map<std::string, uint32_t> explicitDirs;
  for (const auto& item : catalog) {
    if (!item.isDir) continue;
    const std::string p = prism::NormalizeArchivePath(item.path);
    if (!p.empty()) explicitDirs.emplace(p, item.index);  // first node wins
  }
  for (const auto& item : catalog) {
    const std::string p = prism::NormalizeArchivePath(item.path);
    if (p.empty()) continue;
    if (!p.starts_with(prefix)) continue;
    const std::string rest = p.substr(prefix.size());
    if (rest.empty()) continue;  // the folder node itself
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
      const std::string childName = rest.substr(0, slash);
      if (detail::IsPseudoName(childName)) continue;
      const std::string childPath = prefix + childName;
      bool known = false;
      for (const auto& e : view.entries) {
        if (e.path == childPath) {
          known = true;
          break;
        }
      }
      if (!known) {
        const auto it = explicitDirs.find(childPath);
        view.entries.push_back({childPath, true, 0,
                                it == explicitDirs.end() ? kSyntheticIndex
                                                         : it->second});
      }
      continue;
    }
    if (detail::IsPseudoName(rest)) continue;  // "." / ".." leaked into the CD
    view.entries.push_back({p, item.isDir, item.size, item.index});
  }
  // Folders first, then 7-Zip's natural name order -- deterministic, so the
  // window can reproduce a selection across navigation.
  std::sort(view.entries.begin(), view.entries.end(),
            [](const Entry& a, const Entry& b) {
              if (a.isDir != b.isDir) return a.isDir;
              const int cmp = detail::CompareNatural(a.path, b.path);
              if (cmp != 0) return cmp < 0;
              return a.index < b.index;  // total order: never a tie
            });
  return view;
}

// ------------------------------------------------------------ breadcrumb bar
//
// The bar is a flat list of text segments; separators carry no level, so a
// click on one hits nothing. Levels index the crumb vector; kRootLevel is the
// leading root element (folder ""), which is how a deep folder gets back to
// the archive root in one click (7-Zip exposes the same as
// "Root Folder" in its menu -- resource.rc:126 IDM_OPEN_ROOT_FOLDER).

inline constexpr int kNoLevel = -1;
inline constexpr int kRootLevel = -2;
inline constexpr std::string_view kBreadcrumbSeparator = " \xE2\x80\xBA ";  // " › "

struct Segment {
  std::string text;
  int level = kNoLevel;  // kNoLevel = decorative separator
};

struct Span {
  int left = 0;
  int right = 0;  // half-open: [left, right)
  int level = kNoLevel;

  bool Empty() const { return right <= left; }
};

struct BreadcrumbLayout {
  int origin = 0;  // pixel x of the first segment's left edge
  int total = 0;   // full run width before clipping
  std::vector<Span> spans;
};

using WidthFn = std::function<int(std::string_view)>;

inline std::vector<Segment> BreadcrumbSegments(
    const std::vector<Crumb>& crumbs, std::string_view rootLabel = {},
    std::string_view separator = kBreadcrumbSeparator) {
  std::vector<Segment> segments;
  segments.push_back({std::string(rootLabel), kRootLevel});
  for (size_t i = 0; i < crumbs.size(); ++i) {
    segments.push_back({std::string(separator), kNoLevel});
    segments.push_back({crumbs[i].name, static_cast<int>(i)});
  }
  return segments;
}

// The exact string the renderer paints. It is built from the SAME segments the
// hit test walks, so "what you see" and "what you can click" cannot diverge.
inline std::string JoinBreadcrumb(const std::vector<Segment>& segments) {
  std::string out;
  for (const auto& segment : segments) out += segment.text;
  return out;
}

// Lays the segments out left to right starting at originX. `width` measures a
// segment in pixels (DirectWrite in the window, a fake in the tests). Spans are
// clamped to [clipLeft, clipRight): a crumb that is drawn completely outside
// the island must not be clickable, otherwise invisible pixels would navigate.
inline BreadcrumbLayout LayoutBreadcrumb(
    const std::vector<Segment>& segments, int originX,
    const std::function<int(std::string_view)>& width,
    int clipLeft = (std::numeric_limits<int>::min)(),
    int clipRight = (std::numeric_limits<int>::max)()) {
  BreadcrumbLayout layout;
  layout.origin = originX;
  int x = originX;
  layout.spans.reserve(segments.size());
  for (const auto& segment : segments) {
    const int measured = width ? width(std::string_view(segment.text)) : 0;
    const int advance = measured > 0 ? measured : 0;
    layout.spans.push_back({x, x + advance, segment.level});
    x += advance;
  }
  layout.total = x - originX;
  for (auto& span : layout.spans) {
    span.left = (std::max)(span.left, clipLeft);
    span.right = (std::min)(span.right, clipRight);
    if (span.Empty()) span.level = kNoLevel;  // fully clipped: dead pixels
  }
  return layout;
}

// Which crumb contains x? kNoLevel when the click is in a gap, on a separator,
// before the first segment or past the last one. Half-open spans make the
// boundary rule explicit: the left edge belongs to the crumb, the right edge
// belongs to whatever comes next.
inline int HitBreadcrumb(const BreadcrumbLayout& layout, int x) {
  for (const auto& span : layout.spans) {
    if (span.level == kNoLevel) continue;
    if (x >= span.left && x < span.right) return span.level;
  }
  return kNoLevel;
}

// The window paints the crumb run CENTRED inside the nav island (the renderer's
// title format is centre-aligned), so the hit test must centre it too and clip
// to the island's text box. Overflow goes to the right of the box when the run
// is wider than the island (same visible result as D2D's clip), and the clipped
// pixels answer nothing.
inline BreadcrumbLayout LayoutCenteredBreadcrumb(
    const std::vector<Segment>& segments, int boxLeft, int boxRight,
    const std::function<int(std::string_view)>& width) {
  int total = 0;
  for (const auto& segment : segments) {
    const int measured = width ? width(std::string_view(segment.text)) : 0;
    total += measured > 0 ? measured : 0;
  }
  const int boxWidth = (std::max)(0, boxRight - boxLeft);
  int origin = boxLeft + (boxWidth - total) / 2;
  if (origin < boxLeft) origin = boxLeft;  // never start left of the box
  return LayoutBreadcrumb(segments, origin, width, boxLeft, (std::max)(boxLeft, boxRight));
}

// ---------------------------------------------------------------- navigator
//
// The window's whole navigation state: current folder, the filtered list, the
// breadcrumb, and the selection. Every transition returns a NavResult so the
// caller knows whether to reset the viewport and whether to fall through to
// the file action (smart extract). No pixels, no windows -- CI can drive it.

enum class NavAction : uint8_t {
  None,           // nothing happened (root Backspace, same folder, bad index)
  FolderChanged,  // the list must be rebuilt; reset the viewport to the top
  ActivateFile,   // the entry is a file: the app performs its file action
};

inline std::ostream& operator<<(std::ostream& os, NavAction action) {
  switch (action) {
    case NavAction::None: return os << "None";
    case NavAction::FolderChanged: return os << "FolderChanged";
    case NavAction::ActivateFile: return os << "ActivateFile";
  }
  return os << "?";
}

struct NavResult {
  NavAction action = NavAction::None;
  int selection = -1;                   // selection AFTER the transition
  uint32_t catalogIndex = kSyntheticIndex;  // ActivateFile: catalog id to act on
};

class Navigator {
 public:
  Navigator() = default;
  explicit Navigator(const std::vector<ArchiveItem>* catalog) { Attach(catalog); }

  // The catalog is borrowed (the app owns the snapshot; it outlives the
  // window). Attaching resets to the archive root.
  void Attach(const std::vector<ArchiveItem>* catalog) {
    catalog_ = catalog;
    folder_.clear();
    selection_ = -1;
    Rebuild();
  }

  // Re-filter after the catalog itself changed.
  void Rebuild() {
    static const std::vector<ArchiveItem> kEmpty;
    const std::vector<ArchiveItem>& source = catalog_ ? *catalog_ : kEmpty;
    view_ = Build(source, folder_);
    std::unordered_map<uint32_t, const ArchiveItem*> byIndex;
    byIndex.reserve(source.size());
    for (const auto& item : source) byIndex.emplace(item.index, &item);
    items_.clear();
    items_.reserve(view_.entries.size());
    for (const auto& entry : view_.entries) {
      ArchiveItem item;
      item.index = entry.index;
      item.path = entry.path;
      item.isDir = entry.isDir;
      item.size = entry.size;
      if (entry.index != kSyntheticIndex) {
        const auto it = byIndex.find(entry.index);
        if (it != byIndex.end()) {
          item.packSize = it->second->packSize;
          item.mtime = it->second->mtime;
          item.block = it->second->block;
          item.encrypted = it->second->encrypted;
        }
      }
      items_.push_back(std::move(item));
    }
    if (selection_ >= static_cast<int>(view_.entries.size())) selection_ = -1;
  }

  const View& Current() const { return view_; }
  // Parallel to Current().entries (same order): the array the renderer, the
  // list mode and the thumbnail lookup all consume. Entries carry the CATALOG
  // index, so a thumbnails store filled by the whole-archive sweep keeps
  // working after a folder switch -- no rescan.
  const std::vector<ArchiveItem>& FolderItems() const { return items_; }
  const std::string& Folder() const { return folder_; }
  int Selection() const { return selection_; }
  int Count() const { return static_cast<int>(view_.entries.size()); }

  // Out-of-range clears the selection instead of clamping to a neighbour: a
  // click on empty space must not select a file the user did not touch.
  bool SetSelection(int index) {
    const int clamped = (index >= 0 && index < Count()) ? index : -1;
    if (clamped == selection_) return false;
    selection_ = clamped;
    return true;
  }

  // Double-click / Enter. Folders drill in, files are the app's business (the
  // window must NOT guess: "smart extract" is a product decision).
  NavResult Enter(int entryIndex) {
    NavResult result;
    if (entryIndex < 0 || entryIndex >= Count()) return Unchanged();
    const Entry& entry = view_.entries[static_cast<size_t>(entryIndex)];
    if (!entry.isDir) {
      result.action = NavAction::ActivateFile;
      result.catalogIndex = entry.index;
      result.selection = selection_;
      return result;
    }
    return GoTo(entry.path);
  }

  // Backspace. At the root this is a no-op (NavAction::None) -- not a crash and
  // not an empty window (7-Zip PanelKey.cpp:261 -> OpenParentFolder()).
  NavResult Back() {
    if (folder_.empty()) return Unchanged();
    return GoTo(UpOne(folder_));
  }

  // Jump to any folder. A target that does not exist in this catalog (a stale
  // crumb, a hostile path, a folder the archive only implied) is clamped to
  // its deepest existing ancestor -- never to an empty window. When the target
  // is an ancestor of the current folder, the selection lands on the folder we
  // came out of -- by NAME (7-Zip PanelFolderChange.cpp:990-995 FocusedName ->
  // PanelItems.cpp:684-697 match while building), so sorting changes cannot
  // move the highlight.
  NavResult GoTo(std::string_view folder) {
    const std::string target = ClampToExisting(SafeFolder(folder));
    if (target == folder_) return Unchanged();  // clicking the current crumb
    NavResult result;
    result.action = NavAction::FolderChanged;
    const std::string childName = FirstSegmentAfter(target, folder_);
    folder_ = target;
    Rebuild();
    selection_ = -1;
    if (!childName.empty()) {
      const std::string childPath = prism::JoinPath(target, childName);
      for (size_t i = 0; i < view_.entries.size(); ++i) {
        if (view_.entries[i].path == childPath) {
          selection_ = static_cast<int>(i);
          break;
        }
      }
    }
    result.selection = selection_;
    return result;
  }

  NavResult GoToLevel(int level) {
    if (level == kRootLevel) return GoTo("");
    if (level < 0 || level >= static_cast<int>(view_.breadcrumb.size())) {
      return Unchanged();
    }
    return GoTo(view_.breadcrumb[static_cast<size_t>(level)].folder);
  }

  // --- breadcrumb plumbing (the window supplies measurement + island clip) --
  std::vector<Segment> Segments(std::string_view rootLabel) const {
    return BreadcrumbSegments(view_.breadcrumb, rootLabel);
  }

  BreadcrumbLayout Layout(std::string_view rootLabel, int originX,
                          const std::function<int(std::string_view)>& width,
                          int clipLeft = (std::numeric_limits<int>::min)(),
                          int clipRight = (std::numeric_limits<int>::max)()) const {
    return LayoutBreadcrumb(Segments(rootLabel), originX, width, clipLeft, clipRight);
  }

  int Hit(std::string_view rootLabel, int x, int originX,
          const std::function<int(std::string_view)>& width, int clipLeft,
          int clipRight) const {
    return HitBreadcrumb(Layout(rootLabel, originX, width, clipLeft, clipRight), x);
  }

 private:
  // "Nothing happened" still reports the live selection, so the window never
  // has to guess whether to leave the highlight alone.
  NavResult Unchanged() const {
    NavResult result;
    result.selection = selection_;
    return result;
  }

  // Deepest existing folder on the path to `target` (target itself when it
  // exists). A missing folder is not something to navigate into: 7-Zip's
  // BindToPath walks up until a bind succeeds (PanelFolderChange.cpp:63-118).
  std::string ClampToExisting(std::string_view target) const {
    std::string candidate(target);
    while (!candidate.empty()) {
      if (FolderExists(candidate)) return candidate;
      candidate = UpOne(candidate);
    }
    return "";
  }

  bool FolderExists(std::string_view folder) const {
    if (!catalog_) return folder.empty();
    for (const auto& item : *catalog_) {
      const std::string_view p = item.path;
      if (p == folder) return item.isDir;
      // An implicit folder is real if anything lives beneath it.
      if (p.size() > folder.size() && p.compare(0, folder.size(), folder) == 0 &&
          p[folder.size()] == '/') {
        return true;
      }
    }
    return false;
  }

  // First path segment of `from` below `target` -- i.e. the child folder on the
  // path from the target down to where we are. "" when target is not an
  // ancestor of from.
  static std::string FirstSegmentAfter(std::string_view target,
                                       std::string_view from) {
    if (from.empty()) return {};
    if (target.empty()) {
      const size_t slash = from.find('/');
      return std::string(slash == std::string_view::npos ? from : from.substr(0, slash));
    }
    if (from.size() <= target.size() + 1) return {};
    if (from.compare(0, target.size(), target) != 0) return {};
    if (from[target.size()] != '/') return {};  // "Assets" is not "Assets2"
    const std::string_view rest = from.substr(target.size() + 1);
    const size_t slash = rest.find('/');
    return std::string(slash == std::string_view::npos ? rest : rest.substr(0, slash));
  }

  const std::vector<ArchiveItem>* catalog_ = nullptr;
  std::string folder_;
  View view_;
  std::vector<ArchiveItem> items_;
  int selection_ = -1;
};

}  // namespace prism::tree
