// PrismArchive -- PrismCore
// ArchiveTreeView: the folder-navigation view model over a flat catalog.
//
// Catalogs are TREES; the gallery renders ONE list. This module answers the
// two questions every navigation interaction needs: "what belongs to the
// folder I am looking at" and "where am I" (the breadcrumb chain). It is a
// pure function of (catalog, currentFolder) -- no mutation, no state, so the
// window can rebuild it on every navigation for free.
//
// Design notes:
//  * Paths are PrismCore-normalized ('/' separators, no leading slash).
//  * Prefix matching requires the separator (or end): "Assets" must not own
//    "Assets2/x" -- the classic naive-starts_with bug.
//  * Folders sort before files, then by path; order is deterministic so the
//    window's selection can be reproduced across navigation.
#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"  // NormalizeArchivePath

namespace prism::tree {

struct Entry {
  std::string path;
  bool isDir = false;
  uint64_t size = 0;
  uint32_t index = 0;  // catalog index, for extract/preview dispatch
};

struct Crumb {
  std::string name;      // display label ("Icons")
  std::string folder;    // normalized folder path ("Assets/Icons"); "" = root
};

struct View {
  std::vector<Entry> entries;
  std::vector<Crumb> breadcrumb;  // empty at the root

  // One level up; "" means already at the root.
  std::string UpFrom(std::string_view folder) const {
    const std::string f = prism::NormalizeArchivePath(folder);
    const size_t slash = f.rfind('/');
    if (slash == std::string::npos) return "";
    return f.substr(0, slash);
  }
};

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
  const std::string f = prism::NormalizeArchivePath(folder);
  View view;
  std::string prefix;
  if (!f.empty()) {
    prefix = f + "/";
    auto [_, crumbs] = SplitBreadcrumb(f);
    view.breadcrumb = std::move(crumbs);
  }
  for (const auto& item : catalog) {
    const std::string p = prism::NormalizeArchivePath(item.path);
    if (p.empty()) continue;
    if (!p.starts_with(prefix)) continue;
    const std::string rest = p.substr(prefix.size());
    if (rest.empty()) continue;  // the folder node itself
    // A folder prefix must own only its separator-delimited subtree.
    if (rest.find('/') != std::string::npos && !item.isDir) continue;
    // Files deeper than one level belong to a subfolder, not here. A DIR
    // entry deeper down is an ancestor folder -- also not a direct child.
    const size_t slash = rest.find('/');
    if (slash != std::string::npos) {
      // Direct child FOLDER may appear as "Sub/" (trailing slash form) or as
      // an implicit ancestor of a deeper file: surface it once, by name.
      const std::string childName = rest.substr(0, slash);
      const std::string childPath = prefix + childName;
      bool known = false;
      for (const auto& e : view.entries) {
        if (e.path == childPath) {
          known = true;
          break;
        }
      }
      if (!known) {
        view.entries.push_back({childPath, true, 0, item.index});
      }
      continue;
    }
    view.entries.push_back({p, item.isDir, item.size, item.index});
  }
  // Folders first, then by path -- deterministic for selection reproducibility.
  std::sort(view.entries.begin(), view.entries.end(),
            [](const Entry& a, const Entry& b) {
              if (a.isDir != b.isDir) return a.isDir;
              return a.path < b.path;
            });
  return view;
}

}  // namespace prism::tree
