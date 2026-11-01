// PrismArchive -- PrismCore
// EditPlan: the DECISION half of "rename / delete / new folder / move INSIDE an
// archive". Blueprint ch.2 promises copy-on-write editing ("增删改 without a full
// extraction"); C9 taught the product to CREATE an archive from nothing, C11 gave
// it a pool of readers, and this header is the piece in between: given a catalog
// and a list of edits, what is the archive going to look like, what does each
// edit cost, and which edits are ILLEGAL.
//
// Layer rule: no windows.h, no 7-Zip headers, no I/O -- everything here is a pure
// function of its arguments, so the Linux lane proves the rules in milliseconds
// and the Windows executor only marshals the result (same split as
// CompressionPlan vs ArchiveWriter).
//
// THE RULES LIVE HERE, NOT IN THE CALLER (they are what the tests assert):
//   * a new name must not collide with an existing entry (case-insensitive: this
//     is a Windows-first product and "Report.txt"/"report.txt" cannot coexist on
//     the filesystem an extraction lands on);
//   * paths must be relative, '/'-separated, and free of '..' / '.' / empty
//     segments / drive letters / Windows-illegal characters (path traversal is a
//     refusal, never a silent normalization);
//   * deleting a folder that still holds entries requires recurse=true -- a
//     silent "-r" is how a user loses a subtree;
//   * a read-only archive, and a multi-volume set, are refused with a reason
//     (the rewrite has to replace the original file, and a volume set cannot be
//     replaced by one file);
//   * an entry whose NAME is empty or "." (a malformed catalog no writer should
//     ever have produced) refuses the whole edit instead of being copied into a
//     path that cannot be handed back to the filesystem.
//
// WHAT IT COSTS (honest, ADR-002): 7-Zip's out engine never patches an archive in
// place; it streams every kept entry into a brand-new container. So every edit is
// one full rewrite of the kept payload, and in a SOLID archive the whole solid
// block is re-encoded. EditPlan::solidRewrite and EditPlan::estimatedWriteBytes
// say exactly that, rather than advertising an "instant" delete that the engine
// cannot deliver.
#pragma once

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ArchiveCatalog.hpp"

namespace prism {

// ---------------------------------------------------------------------------
// Operations and their verdicts (operator<< is required: PRISM_CHECK_EQ prints
// enum values and CONTRACTS B6 makes a missing one a preflight failure).
// ---------------------------------------------------------------------------
enum class EditOpKind { Rename, Delete, NewFolder, Move };

inline std::string_view ToString(EditOpKind kind) {
  switch (kind) {
    case EditOpKind::Rename: return "rename";
    case EditOpKind::Delete: return "delete";
    case EditOpKind::NewFolder: return "new-folder";
    case EditOpKind::Move: return "move";
  }
  return "?";
}

// Why an edit was refused. 'None' means "legal".
enum class EditRefusal {
  None,
  EmptyOperationList,
  ReadOnlyArchive,
  MultiVolumeArchive,
  MalformedEntryName,
  UnsafePath,
  MissingEntry,
  DuplicateName,
  NonEmptyFolder,
  NotAFolder,
  UnsupportedFormat,
};

inline std::string_view ToString(EditRefusal refusal) {
  switch (refusal) {
    case EditRefusal::None: return "ok";
    case EditRefusal::EmptyOperationList: return "no-operations";
    case EditRefusal::ReadOnlyArchive: return "read-only";
    case EditRefusal::MultiVolumeArchive: return "multi-volume";
    case EditRefusal::MalformedEntryName: return "malformed-name";
    case EditRefusal::UnsafePath: return "unsafe-path";
    case EditRefusal::MissingEntry: return "missing-entry";
    case EditRefusal::DuplicateName: return "duplicate-name";
    case EditRefusal::NonEmptyFolder: return "non-empty-folder";
    case EditRefusal::NotAFolder: return "not-a-folder";
    case EditRefusal::UnsupportedFormat: return "unsupported-format";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, EditOpKind kind) { return os << ToString(kind); }
inline std::ostream& operator<<(std::ostream& os, EditRefusal refusal) {
  return os << ToString(refusal);
}

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
struct EditOp {
  EditOpKind kind = EditOpKind::Delete;
  // Subject of the operation: the existing entry's path (Rename/Delete/Move) or
  // the folder to create (NewFolder). UTF-8, '/' separated.
  std::string path;
  // Rename: the complete new path. Move: the DESTINATION FOLDER ("" = the
  // archive root). Ignored by Delete/NewFolder.
  std::string target;
  // Delete: allow removing a folder that still holds entries.
  bool recurse = false;
};

// What the executor observed about the container. The model never touches the
// file, so the caller must tell it these five facts; every one of them is a rule.
struct EditArchiveFacts {
  bool readOnly = false;         // FILE_ATTRIBUTE_READONLY on the archive file
  bool multiVolume = false;      // part of a .001/.002 set
  bool solid = false;            // at least one entry lives in a solid block
  bool encrypted = false;        // payload encryption is on
  bool headerEncrypted = false;  // names are hidden (7z header encryption)
};

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
// One op's verdict + blast radius. "影响哪些条目、是否固实重写、预估代价".
struct EditOpImpact {
  EditOpKind kind = EditOpKind::Delete;
  bool legal = false;
  EditRefusal refusal = EditRefusal::None;
  std::string reason;  // always set; the human sentence for refusal OR the note
  // Entry paths this op touches, in archive order (old names for a rename/move,
  // the subtree for a delete, the materialized folders for a new folder).
  std::vector<std::string> affected;
  uint64_t bytesAffected = 0;
  // True when this edit forces the whole solid block through the compressor.
  bool forcesSolidRewrite = false;
};

struct EditPlanItem {
  std::string path;        // final path in the new archive
  std::string sourcePath;  // original path in the old archive ("" = created here)
  uint32_t sourceIndex = 0;  // old catalog index; 0xFFFFFFFF for a created folder
  bool isDir = false;
  uint64_t size = 0;
  // FILETIME ticks carried over from the old catalog (0 = the container has no
  // timestamp for it). The payload is copied, so the entry is still the same
  // file as far as the user -- and the rewritten archive -- is concerned.
  uint64_t mtime100ns = 0;
  bool added = false;    // this folder does not exist in the old archive
  bool renamed = false;  // this entry keeps its bytes but changes path
};

struct EditPlan {
  bool ok = false;
  EditRefusal refusal = EditRefusal::None;
  std::string error;  // first refusal, in one sentence

  std::vector<EditOpImpact> ops;  // one entry per requested op, in request order
  // Final archive content, sorted by path. Populated only when 'ok' -- a refused
  // plan must never be handed to a writer as "what to write".
  std::vector<EditPlanItem> items;
  std::vector<std::string> removed;  // paths the edit drops (old names)

  uint64_t keptFiles = 0;
  uint64_t keptBytes = 0;
  uint64_t entriesAffected = 0;  // distinct catalog entries touched by any op

  // The whole container is rewritten: this is the solid-block re-encode signal.
  bool solidRewrite = false;
  uint64_t estimatedWriteBytes = 0;  // kept payload that crosses the compressor
  std::string costNote;
};

namespace edit_detail {

struct Node {
  std::string path;        // current (edited) path
  std::string sourcePath;  // original path
  uint32_t sourceIndex = 0;
  bool isDir = false;
  uint64_t size = 0;
  uint64_t mtime100ns = 0;
  bool added = false;
  bool renamed = false;
};

inline char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Windows-first comparison: ASCII folds, every other byte (UTF-8 continuation,
// CJK) compares exactly. Two names that differ only by ASCII case are the SAME
// name, because they cannot coexist once extracted.
inline bool SamePath(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (LowerAscii(a[i]) != LowerAscii(b[i])) return false;
  }
  return true;
}

inline bool StartsWithPath(const std::string& path, const std::string& prefix) {
  if (path.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (LowerAscii(path[i]) != LowerAscii(prefix[i])) return false;
  }
  return true;
}

// 'path' strictly inside 'dir' (dir itself is not "under" dir).
inline bool IsUnder(const std::string& path, const std::string& dir) {
  if (path.size() <= dir.size() + 1) return false;
  if (!StartsWithPath(path, dir)) return false;
  return path[dir.size()] == '/';
}

inline std::string LeafName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string DirName(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

// The caller's path is repaired only in ways that cannot hide a traversal:
// backslashes become '/', a trailing '/' is dropped, and a leading "./" is
// removed. '..' and empty segments are NEVER repaired -- they are refused.
inline std::string NormalizeEditPath(std::string path) {
  std::string out;
  out.reserve(path.size());
  for (char c : path) out.push_back(c == '\\' ? '/' : c);
  while (out.size() > 1 && out.back() == '/') out.pop_back();
  while (out.rfind("./", 0) == 0 && out.size() > 2) out.erase(0, 2);
  return out;
}

// Validate an archive-relative path. Returns EditRefusal::None when usable.
inline EditRefusal ValidateArchivePath(const std::string& path, std::string* why) {
  auto refuse = [why](EditRefusal refusal, const std::string& reason) {
    if (why != nullptr) *why = reason;
    return refusal;
  };
  if (path.empty()) return refuse(EditRefusal::MalformedEntryName, "the name is empty");
  if (path.front() == '/') {
    return refuse(EditRefusal::UnsafePath, "an absolute path would escape the archive");
  }
  if (path.size() >= 2 && path[1] == ':') {
    return refuse(EditRefusal::UnsafePath, "a drive-qualified path would escape the archive");
  }
  if (path.back() == '/') {
    return refuse(EditRefusal::MalformedEntryName, "a trailing separator leaves an empty segment");
  }
  size_t start = 0;
  while (start <= path.size()) {
    size_t end = path.find('/', start);
    if (end == std::string::npos) end = path.size();
    const std::string segment = path.substr(start, end - start);
    if (segment.empty()) {
      return refuse(EditRefusal::MalformedEntryName, "an empty path segment ('//') is not a name");
    }
    if (segment == ".") {
      return refuse(EditRefusal::MalformedEntryName,
                    "a '.' path segment is a malformed archive entry");
    }
    if (segment == "..") {
      return refuse(EditRefusal::UnsafePath, "'..' would traverse outside the archive root");
    }
    for (char c : segment) {
      const unsigned char u = static_cast<unsigned char>(c);
      if (u < 0x20 || c == '\\') {
        return refuse(EditRefusal::MalformedEntryName,
                      "a control character or a backslash cannot be part of a name");
      }
      if (c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*') {
        return refuse(EditRefusal::UnsafePath,
                      std::string("'") + c + "' cannot exist in a Windows file name");
      }
    }
    if (end == path.size()) break;
    start = end + 1;
  }
  return EditRefusal::None;
}

}  // namespace edit_detail

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
inline EditPlan BuildEditPlan(const std::vector<ArchiveItem>& catalog,
                              const std::vector<EditOp>& ops,
                              const EditArchiveFacts& facts) {
  using namespace edit_detail;  // NOLINT: a decision function, not a library
  EditPlan plan;
  plan.solidRewrite = facts.solid;
  plan.costNote = facts.solid
                      ? "solid archive: one edit re-encodes the whole solid block, so the "
                        "rewrite costs every kept byte"
                      : "non-solid archive: kept entries are copied one by one, but the "
                        "container is still rewritten in full";

  // Every op still gets a verdict even when the plan is refused as a whole, so a
  // caller can show "these four edits, none of them possible, because ...".
  plan.ops.resize(ops.size());
  for (size_t i = 0; i < ops.size(); ++i) {
    plan.ops[i].kind = ops[i].kind;
    plan.ops[i].forcesSolidRewrite = facts.solid;
  }

  auto refuseAll = [&plan, &ops](EditRefusal refusal, std::string error) {
    plan.ok = false;
    plan.refusal = refusal;
    plan.error = std::move(error);
    for (size_t i = 0; i < ops.size(); ++i) {
      plan.ops[i].legal = false;
      plan.ops[i].refusal = refusal;
      plan.ops[i].reason = plan.error;
    }
    plan.items.clear();
    plan.removed.clear();
    return plan;
  };

  if (ops.empty()) {
    return refuseAll(EditRefusal::EmptyOperationList, "no edit was requested");
  }
  // A rewrite has to REPLACE the original file, so a read-only archive cannot be
  // edited at all: refusing here is the difference between "nothing happened" and
  // a half-written temp file next to a file the user believes is untouched.
  if (facts.readOnly) {
    return refuseAll(EditRefusal::ReadOnlyArchive,
                     "the archive is read-only: editing replaces the file, so the read-only "
                     "attribute must be cleared first");
  }
  if (facts.multiVolume) {
    return refuseAll(EditRefusal::MultiVolumeArchive,
                     "the archive is part of a multi-volume set (.001/.002/...): the rewrite "
                     "produces a single file, so edit a single-volume archive instead");
  }

  std::vector<Node> nodes;
  nodes.reserve(catalog.size());
  for (const ArchiveItem& item : catalog) {
    Node node;
    node.path = item.path;
    node.sourcePath = item.path;
    node.sourceIndex = item.index;
    node.isDir = item.isDir;
    node.size = item.isDir ? 0 : item.size;
    node.mtime100ns = item.mtime > 0 ? static_cast<uint64_t>(item.mtime) : 0;
    nodes.push_back(std::move(node));
  }

  // A catalog entry made of "." or of nothing is not a name this product can
  // round-trip. Copying it through would put a file called "." into the new
  // archive and every extraction path would then have to special-case it, so the
  // edit is refused with the entry named (it is never silently dropped: that
  // would be data loss the user did not ask for).
  for (const Node& node : nodes) {
    std::string why;
    const EditRefusal refusal = ValidateArchivePath(node.path, &why);
    if (refusal != EditRefusal::None) {
      return refuseAll(refusal, "the catalog holds an entry named '" + node.path + "' which " +
                                    std::string(ToString(refusal)) + " (" + why +
                                    "): refusing to rewrite this archive");
    }
  }

  auto findNode = [&nodes](const std::string& path) -> long long {
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (SamePath(nodes[i].path, path)) return static_cast<long long>(i);
    }
    return -1;
  };
  auto subtreeOf = [&nodes](size_t root, std::vector<size_t>* out) {
    out->clear();
    out->push_back(root);
    const std::string prefix = nodes[root].path;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i != root && IsUnder(nodes[i].path, prefix)) out->push_back(i);
    }
  };
  // Re-root 'root' and every descendant from 'from' to 'to'.
  auto reroot = [&nodes](size_t root, const std::string& to) {
    const std::string from = nodes[root].path;
    nodes[root].path = to;
    nodes[root].renamed = true;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i == root || !IsUnder(nodes[i].path, from)) continue;
      nodes[i].path = to + nodes[i].path.substr(from.size());
      nodes[i].renamed = true;
    }
  };

  std::vector<std::string> touched;  // distinct entry paths any op touched

  for (size_t opIndex = 0; opIndex < ops.size(); ++opIndex) {
    const EditOp& op = ops[opIndex];
    EditOpImpact& impact = plan.ops[opIndex];
    const std::string path = NormalizeEditPath(op.path);
    std::string why;
    const EditRefusal pathRefusal = ValidateArchivePath(path, &why);
    if (pathRefusal != EditRefusal::None) {
      impact.legal = false;
      impact.refusal = pathRefusal;
      impact.reason = "the requested path '" + path + "' is refused: " + why;
      if (plan.error.empty()) {
        plan.error = impact.reason;
        plan.refusal = pathRefusal;
      }
      continue;
    }

    if (op.kind == EditOpKind::NewFolder) {
      const long long existing = findNode(path);
      if (existing >= 0) {
        impact.legal = false;
        impact.refusal = EditRefusal::DuplicateName;
        impact.reason = "'" + path + "' already exists in the archive";
        if (plan.error.empty()) {
          plan.error = impact.reason;
          plan.refusal = impact.refusal;
        }
        continue;
      }
      // Materialize every missing ancestor, so the result is a real directory
      // tree and not one entry whose parent does not exist.
      std::string prefix;
      size_t start = 0;
      bool blockedByFile = false;
      while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos) end = path.size();
        prefix = prefix.empty() ? path.substr(0, end) : prefix + "/" + path.substr(start, end - start);
        const long long found = findNode(prefix);
        if (found >= 0 && !nodes[static_cast<size_t>(found)].isDir) {
          blockedByFile = true;
          impact.reason = "'" + prefix + "' is a file, so '" + path + "' cannot live under it";
          break;
        }
        if (found < 0) {
          Node node;
          node.path = prefix;
          node.sourcePath.clear();
          node.sourceIndex = 0xFFFFFFFFu;
          node.isDir = true;
          node.added = true;
          nodes.push_back(std::move(node));
          impact.affected.push_back(prefix);
          touched.push_back(prefix);
        }
        if (end == path.size()) break;
        start = end + 1;
      }
      if (blockedByFile) {
        impact.legal = false;
        impact.refusal = EditRefusal::NotAFolder;
        if (plan.error.empty()) {
          plan.error = impact.reason;
          plan.refusal = impact.refusal;
        }
        continue;
      }
      impact.legal = true;
      impact.reason = impact.affected.empty()
                          ? "'" + path + "' already existed as a folder"
                          : "created " + std::to_string(impact.affected.size()) +
                                " folder(s); no payload is touched";
      continue;
    }

    const long long subject = findNode(path);
    if (subject < 0) {
      impact.legal = false;
      impact.refusal = EditRefusal::MissingEntry;
      impact.reason = "no entry named '" + path + "' in the archive";
      if (plan.error.empty()) {
        plan.error = impact.reason;
        plan.refusal = impact.refusal;
      }
      continue;
    }
    const size_t root = static_cast<size_t>(subject);
    std::vector<size_t> subtree;
    subtreeOf(root, &subtree);
    uint64_t bytes = 0;
    for (size_t i : subtree) {
      if (!nodes[i].isDir) bytes += nodes[i].size;
      impact.affected.push_back(nodes[i].path);
    }
    impact.bytesAffected = bytes;

    if (op.kind == EditOpKind::Delete) {
      if (nodes[root].isDir && subtree.size() > 1 && !op.recurse) {
        impact.legal = false;
        impact.refusal = EditRefusal::NonEmptyFolder;
        impact.reason = "'" + path + "' is a folder holding " +
                        std::to_string(subtree.size() - 1) +
                        " entr(ies): pass recurse=true to delete the subtree on purpose";
        if (plan.error.empty()) {
          plan.error = impact.reason;
          plan.refusal = impact.refusal;
        }
        continue;
      }
      // Erase descendants first so the indices stay valid.
      for (size_t k = subtree.size(); k-- > 0;) {
        const size_t at = subtree[k];
        plan.removed.push_back(nodes[at].path);
        touched.push_back(nodes[at].path);
        nodes.erase(nodes.begin() + static_cast<long long>(at));
      }
      impact.legal = true;
      impact.reason = "removed " + std::to_string(subtree.size()) + " entr(ies), " +
                      std::to_string(bytes) + " byte(s) of payload never re-read";
      continue;
    }

    // Rename and Move both end in "this entry (and its subtree) gets a new path".
    std::string destination;  // the new full path
    if (op.kind == EditOpKind::Rename) {
      destination = NormalizeEditPath(op.target);
      std::string targetWhy;
      const EditRefusal targetRefusal = ValidateArchivePath(destination, &targetWhy);
      if (targetRefusal != EditRefusal::None) {
        impact.legal = false;
        impact.refusal = targetRefusal;
        impact.reason = "the new name '" + destination + "' is refused: " + targetWhy;
        if (plan.error.empty()) {
          plan.error = impact.reason;
          plan.refusal = impact.refusal;
        }
        continue;
      }
      if (nodes[root].isDir && (SamePath(destination, nodes[root].path) ||
                                IsUnder(destination, nodes[root].path))) {
        impact.legal = false;
        impact.refusal = EditRefusal::UnsafePath;
        impact.reason = "a folder cannot be renamed into itself ('" + destination + "')";
        if (plan.error.empty()) {
          plan.error = impact.reason;
          plan.refusal = impact.refusal;
        }
        continue;
      }
      // A rename renames; it does not conjure a directory tree (NewFolder exists
      // for that), so the destination's parent must already be a folder. Without
      // this rule "a/b.txt" would land under a folder the user never created and
      // the extraction would materialize a tree the archive does not describe.
      const std::string parent = DirName(destination);
      if (!parent.empty()) {
        const long long parentNode = findNode(parent);
        if (parentNode < 0) {
          impact.legal = false;
          impact.refusal = EditRefusal::MissingEntry;
          impact.reason = "the folder '" + parent + "' does not exist in the archive";
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
        if (!nodes[static_cast<size_t>(parentNode)].isDir) {
          impact.legal = false;
          impact.refusal = EditRefusal::NotAFolder;
          impact.reason = "'" + parent + "' is a file, not a folder";
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
      }
    } else {  // Move
      destination = NormalizeEditPath(op.target);
      if (destination.empty()) {
        destination = LeafName(nodes[root].path);
      } else {
        std::string targetWhy;
        const EditRefusal targetRefusal = ValidateArchivePath(destination, &targetWhy);
        if (targetRefusal != EditRefusal::None) {
          impact.legal = false;
          impact.refusal = targetRefusal;
          impact.reason = "the destination '" + destination + "' is refused: " + targetWhy;
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
        const long long destNode = findNode(destination);
        if (destNode < 0) {
          impact.legal = false;
          impact.refusal = EditRefusal::MissingEntry;
          impact.reason = "the destination folder '" + destination + "' does not exist";
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
        if (!nodes[static_cast<size_t>(destNode)].isDir) {
          impact.legal = false;
          impact.refusal = EditRefusal::NotAFolder;
          impact.reason = "'" + destination + "' is a file, not a folder";
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
        if (SamePath(destination, nodes[root].path) || IsUnder(destination, nodes[root].path)) {
          impact.legal = false;
          impact.refusal = EditRefusal::UnsafePath;
          impact.reason = "a folder cannot be moved into itself ('" + destination + "')";
          if (plan.error.empty()) {
            plan.error = impact.reason;
            plan.refusal = impact.refusal;
          }
          continue;
        }
        destination += "/" + LeafName(nodes[root].path);
      }
    }

    if (SamePath(destination, nodes[root].path)) {
      impact.legal = true;
      impact.affected.clear();
      impact.bytesAffected = 0;
      impact.reason = "no change: '" + destination + "' is already this entry's path";
      continue;
    }
    // Collision: anything already at the destination that is NOT part of the
    // moving subtree (a subtree cannot collide with itself).
    bool collision = false;
    for (size_t i = 0; i < nodes.size(); ++i) {
      if (i == root || IsUnder(nodes[i].path, nodes[root].path)) continue;
      if (SamePath(nodes[i].path, destination)) {
        collision = true;
        break;
      }
    }
    if (collision) {
      impact.legal = false;
      impact.refusal = EditRefusal::DuplicateName;
      impact.reason = "'" + destination + "' already exists in the archive";
      if (plan.error.empty()) {
        plan.error = impact.reason;
        plan.refusal = impact.refusal;
      }
      continue;
    }
    reroot(root, destination);
    for (size_t i : subtree) {
      touched.push_back(nodes[i].path);
    }
    impact.legal = true;
    impact.reason = "moved " + std::to_string(subtree.size()) + " entr(ies); the payload is "
                    "copied, never decoded for the user";
  }

  plan.ok = plan.error.empty();
  if (!plan.ok && plan.refusal == EditRefusal::None) {
    plan.refusal = EditRefusal::UnsafePath;  // defensive: never a silent refusal
  }

  // The final content. Sorted by path so two runs of the same request hand the
  // writer the same list (dirs sort before their children for free).
  std::vector<EditPlanItem> items;
  items.reserve(nodes.size());
  for (const Node& node : nodes) {
    EditPlanItem item;
    item.path = node.path;
    item.sourcePath = node.sourcePath;
    item.sourceIndex = node.sourceIndex;
    item.isDir = node.isDir;
    item.size = node.size;
    item.mtime100ns = node.mtime100ns;
    item.added = node.added;
    item.renamed = node.renamed;
    if (!node.isDir) {
      ++plan.keptFiles;
      plan.keptBytes += node.size;
    }
    items.push_back(std::move(item));
  }
  std::sort(items.begin(), items.end(),
            [](const EditPlanItem& a, const EditPlanItem& b) { return a.path < b.path; });
  std::sort(plan.removed.begin(), plan.removed.end());

  // Distinct entries touched, for "how much of the archive did this edit see".
  std::vector<std::string> distinct;
  for (const std::string& path : touched) {
    bool seen = false;
    for (const std::string& existing : distinct) {
      if (SamePath(existing, path)) {
        seen = true;
        break;
      }
    }
    if (!seen) distinct.push_back(path);
  }
  plan.entriesAffected = distinct.size();
  plan.estimatedWriteBytes = plan.keptBytes;
  if (plan.ok) plan.items = std::move(items);
  return plan;
}

}  // namespace prism
