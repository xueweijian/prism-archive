// PrismArchive -- PrismCore
// Ctrl+Shift+N: create a folder INSIDE the archive without extracting it
// (blueprint ch. 9.1 row 5 + ch. 4.5).
//
// The tree work already exists (VirtualTree.hpp): Mkdir() inserts the node and
// MakeUpdatePlan() emits the copy-on-write transaction (newData = 0 /
// newProps = 1). What was missing is the DECISION in front of it -- the part
// where a typed name can be illegal, reserved, already present, or a path
// masquerading as a name. Doing that check in the window would make it
// untestable on Linux and would let two call sites (palette and key chord)
// drift apart.
//
// Policy: REFUSE rather than silently mangle. SanitizeWindowsName() rewrites
// "a.txt:hidden" and trims trailing dots, which is right for extraction (the
// name already exists and we must not lose data) but wrong for a name the user
// is typing RIGHT NOW: they would get a folder that is not what they asked
// for. So a name is either accepted verbatim or rejected with a reason.
#pragma once

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"
#include "VirtualTree.hpp"

namespace prism::tree {

enum class MkdirStatus : uint8_t {
  Ok,             // created; the update plan now contains the new node
  EmptyName,      // nothing but spaces
  InvalidName,    // illegal/reserved on Windows, a separator, or "."/".."
  AlreadyExists,  // a file OR folder with that name (case-insensitive) is here
  NoSuchFolder,   // the parent folder is not in the archive (or is a file)
  TreeRejected,   // VirtualArchiveTree::Mkdir refused (defensive)
};

inline std::ostream& operator<<(std::ostream& os, MkdirStatus status) {
  switch (status) {
    case MkdirStatus::Ok: return os << "Ok";
    case MkdirStatus::EmptyName: return os << "EmptyName";
    case MkdirStatus::InvalidName: return os << "InvalidName";
    case MkdirStatus::AlreadyExists: return os << "AlreadyExists";
    case MkdirStatus::NoSuchFolder: return os << "NoSuchFolder";
    case MkdirStatus::TreeRejected: return os << "TreeRejected";
  }
  return os << "?";
}

struct MkdirResult {
  MkdirStatus status = MkdirStatus::EmptyName;
  std::string folder;  // normalized parent folder ("pack" / "")
  std::string name;    // the accepted name (verbatim)
  std::string path;    // folder + "/" + name, the node's archive path
  bool created = false;
};

namespace detail {

inline std::string_view TrimSpaces(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return text.substr(begin, end - begin);
}

// A single path segment must survive Windows extraction VERBATIM: the tree may
// later be committed and the archive extracted on any machine, so a name that
// the filesystem would reinterpret must not enter the tree in the first place.
inline bool IsAcceptableSegment(std::string_view name) {
#ifdef PRISM_NEWFOLDER_MUTANT_ALLOW_RESERVED
  return !name.empty();  // mutant: any non-empty string is accepted
#else
  if (name.empty()) return false;
  if (name == "." || name == "..") return false;
  if (name.find('/') != std::string_view::npos) return false;
  if (name.find('\\') != std::string_view::npos) return false;
  if (name.size() > 255) return false;  // NTFS component ceiling
  // SanitizeWindowsName is the authority on what "survives verbatim": if it
  // would change the name, the name is not storable as typed.
  return prism::SanitizeWindowsName(name) == name;
#endif
}

// Archive paths are case-SENSITIVE, Windows destinations are not: creating
// "Pack" next to "pack" makes two archive entries that extract onto one file.
// The tree cannot answer this (its map is exact-match by design), so the
// sibling scan lives here where BOTH call sites (key chord and palette) share
// it.
inline bool SiblingExistsIgnoringCase(VirtualArchiveTree& tree,
                                      std::string_view folder,
                                      std::string_view name) {
  VirtualArchiveTree::Node* parent = tree.Find(folder);
  if (!parent) return false;
  for (const auto& [childName, child] : parent->children) {
    (void)child;
    if (prism::EqualsIgnoreCase(childName, name)) return true;
  }
  return false;
}

// A folder is anything the tree can show as a container: an explicit directory
// node, or a node the archive only IMPLIED through its children (VirtualTree's
// Insert leaves those with isDir == false and no catalog index, yet the
// navigator happily enters them -- "mkdir inside it" must mean the same thing).
inline bool IsFolderNode(VirtualArchiveTree& tree, std::string_view folder) {
  if (prism::NormalizeArchivePath(folder).empty()) return true;  // the root
  VirtualArchiveTree::Node* node = tree.Find(folder);
  if (!node) return false;
  return node->isDir || !node->children.empty();
}

inline bool SameNameIgnoringCase(std::string_view a, std::string_view b) {
  return prism::EqualsIgnoreCase(a, b);
}

}  // namespace detail

// The tree must already be seeded from the catalog (VirtualArchiveTree::
// FromCatalog) and kept live across the session: created folders live only in
// the tree until the executor commits the plan.
inline MkdirResult PlanNewFolder(VirtualArchiveTree& tree, std::string_view folder,
                                 std::string_view rawName) {
  MkdirResult result;
  result.folder = prism::SanitizeRelativePath(folder);
  // Typed names come from an edit box: trimming accidental edge spaces is
  // expected behaviour, not silent mangling (a leading/trailing space is a
  // typo, whereas ':' or a reserved device name is a different name).
  const std::string_view name = detail::TrimSpaces(rawName);
  if (name.empty()) {
    result.status = MkdirStatus::EmptyName;
    return result;
  }
  result.name = std::string(name);
  if (!detail::IsAcceptableSegment(name)) {
    result.status = MkdirStatus::InvalidName;
    return result;
  }
  const std::string target = prism::JoinPath(result.folder, name);
  result.path = target;

  if (!detail::IsFolderNode(tree, result.folder)) {
    result.status = MkdirStatus::NoSuchFolder;
    return result;
  }
  if (tree.Find(target) != nullptr ||
      detail::SiblingExistsIgnoringCase(tree, result.folder, name)) {
    result.status = MkdirStatus::AlreadyExists;
    return result;
  }
  if (!tree.Mkdir(target)) {
    result.status = MkdirStatus::TreeRejected;
    return result;
  }
  result.status = MkdirStatus::Ok;
  result.created = true;
  return result;
}

// Convenience for callers that hold the flat catalog (palette row, app glue):
// checks the same case-insensitive rule the tree enforces by exact name, so
// "Pack" can never be created next to "pack" on a Windows destination.
inline bool FolderNameTaken(const std::vector<ArchiveItem>& catalog,
                            std::string_view folder, std::string_view name) {
  const std::string parent = prism::SanitizeRelativePath(folder);
  const std::string prefix = parent.empty() ? std::string() : parent + "/";
  for (const auto& item : catalog) {
    const std::string path = prism::NormalizeArchivePath(item.path);
    if (path.size() <= prefix.size()) continue;
    if (path.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string_view rest = std::string_view(path).substr(prefix.size());
    if (rest.find('/') != std::string_view::npos) continue;  // deeper, not a sibling
    if (detail::SameNameIgnoringCase(rest, name)) return true;
  }
  return false;
}

}  // namespace prism::tree
