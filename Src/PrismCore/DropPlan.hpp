// PrismArchive -- PrismCore
// Card C52 -- "the user drags files INTO the window".
//
// C20 shipped the other direction (drag OUT of the archive, zero %TEMP% files)
// and C4 shipped "extract here" from the context menu. Dropping content ONTO
// the window had no model at all: Explorer would refuse the drop, or -- worse,
// once a WM_DROPFILES handler existed -- the window would have to guess whether
// a .7z meant "add this file to the open archive" or "open this file".
//
// This header is that decision, as PURE DATA IN / DATA OUT:
//
//   in  : the dragged tree (paths + which ones are folders), whether an archive
//         is open, and whether that archive is read-only
//   out : ONE primary action (OpenArchive / ExtractHere / AddToArchive /
//         TestArchive / Refuse + a named reason), the alternatives a hint can
//         offer next to it, and the text the overlay paints
//
// No <windows.h>, no 7-Zip headers, no filesystem access: the whole rule set is
// asserted on Linux in milliseconds (Tests/test_drop_plan.cpp), including the
// two D3 negative controls declared below. The Win32 layer's only job is
// HDROP -> DropEntry (a bounded FindFirstFileW walk) and pixels.
//
// WHY THE RULES RE-DERIVE "contains a directory" FROM THE TREE
// ------------------------------------------------------------
// Explorer hands the window only the TOP-LEVEL items, and a caller can hand
// this model a stale hint ("yes, a folder is in there") after a failed scan.
// The verdict therefore comes from the entries themselves; the containsDirectory
// flag is kept as the caller claim and any disagreement with the tree is
// surfaced as DropPlan::declaredDirectoryMismatch -- reported, never trusted.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "PathSafety.hpp"
#include "WindowLayout.hpp"

namespace prism::drop {

// ---------------------------------------------------------------------------
// D3 negative-control switches. Each is compiled into its own binary and
// registered with a "MUTATION CAUGHT" marker (Tests/suites/c52-dropin.cmake):
//
//   PRISM_C52_MUTANT_READONLY_ALLOWS_ADD      read-only archives accept content
//                                            (the "just write anyway" shortcut)
//   PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION    a dropped folder is handed over
//                                            as ONE opaque member: the recursive
//                                            walk that makes "add this folder"
//                                            mean "add what is inside it" is off
//
// A mutation that stops matching its assertion must turn CTest red, so the
// suites arm SetMutationVerdict with a RUNTIME predicate that proves the
// injected defect actually fired (CONTRACTS D3/D27).
// ---------------------------------------------------------------------------
namespace mutation {
#if defined(PRISM_C52_MUTANT_READONLY_ALLOWS_ADD)
inline constexpr bool kReadOnlyAllowsAdd = true;
#else
inline constexpr bool kReadOnlyAllowsAdd = false;
#endif
#if defined(PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION)
inline constexpr bool kFlatDirectories = true;
#else
inline constexpr bool kFlatDirectories = false;
#endif
}  // namespace mutation

// ---------------------------------------------------------------------------
// Actions. The window never invents one: what a drop MEANS is decided here.
// ---------------------------------------------------------------------------
enum class DropAction : std::uint8_t {
  OpenArchive,   // browse the dropped archive in this window
  ExtractHere,   // unpack the dropped archive next to the drop target
  AddToArchive,  // write the dropped content INTO the open archive
  TestArchive,   // verify the dropped archive without replacing the view
  Refuse         // nothing happens; the plan names why
};

inline const char* ToString(DropAction action) {
  switch (action) {
    case DropAction::OpenArchive: return "OpenArchive";
    case DropAction::ExtractHere: return "ExtractHere";
    case DropAction::AddToArchive: return "AddToArchive";
    case DropAction::TestArchive: return "TestArchive";
    case DropAction::Refuse: return "Refuse";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, DropAction action) {
  return os << ToString(action);
}

// A refusal always carries a name. A silent "nothing happened" is undebuggable
// from a crash report and indistinguishable from a broken drop target (the same
// rule C5 pinned for the drag-OUT transport).
enum class DropReason : std::uint8_t {
  None = 0,
  EmptySelection,   // nothing to deliver
  NotAnArchive,     // non-archive content with no open archive to add it to
  NoArchiveOpen,    // a folder was dropped but no archive can receive it
  ReadOnlyArchive,  // the open archive cannot be written
  TooManyMembers    // the drop exceeds the flattening bound
};

inline const char* ToString(DropReason reason) {
  switch (reason) {
    case DropReason::None: return "None";
    case DropReason::EmptySelection: return "EmptySelection";
    case DropReason::NotAnArchive: return "NotAnArchive";
    case DropReason::NoArchiveOpen: return "NoArchiveOpen";
    case DropReason::ReadOnlyArchive: return "ReadOnlyArchive";
    case DropReason::TooManyMembers: return "TooManyMembers";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, DropReason reason) {
  return os << ToString(reason);
}

// ---------------------------------------------------------------------------
// What kind of archive a name looks like.
//
// NotArchive is the everyday case (a .txt, a photo). Container archives have an
// internal tree a gallery can browse. Single-stream ones (.gz, .zst, ...) hold
// exactly one payload with no name of its own: opening one shows a one-row
// archive, so the model prefers extracting them.
// ---------------------------------------------------------------------------
enum class ArchiveKind : std::uint8_t { NotArchive, Container, SingleStream };

inline const char* ToString(ArchiveKind kind) {
  switch (kind) {
    case ArchiveKind::NotArchive: return "NotArchive";
    case ArchiveKind::Container: return "Container";
    case ArchiveKind::SingleStream: return "SingleStream";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, ArchiveKind kind) {
  return os << ToString(kind);
}

inline bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// PrismCore keeps '/' separators; PathSafety only touches the last segment.
inline std::string DropBaseNameLower(std::string_view path) {
  return prism::ToLower(std::string(prism::FileNameOf(prism::NormalizeArchivePath(path))));
}

inline ArchiveKind Classify(std::string_view path) {
  const std::string name = DropBaseNameLower(path);
  if (name.empty()) return ArchiveKind::NotArchive;
  // Multi-part suffixes FIRST: ExtensionOf() only ever answers the last dot
  // component, so "movie.tar.gz" would otherwise be read as a bare ".gz" stream.
  if (EndsWith(name, ".tar.gz") || EndsWith(name, ".tar.zst") || EndsWith(name, ".tar.bz2") ||
      EndsWith(name, ".tar.xz") || EndsWith(name, ".tar.lz4") || EndsWith(name, ".tar.br")) {
    return ArchiveKind::Container;
  }
  if (EndsWith(name, ".tgz") || EndsWith(name, ".tbz2") || EndsWith(name, ".txz")) {
    return ArchiveKind::Container;
  }
  const std::string ext = prism::ExtensionOf(path);
  if (ext.size() < 2 || ext == ".") return ArchiveKind::NotArchive;
  // Random-access containers (an internal tree the gallery can browse).
  if (ext == ".7z" || ext == ".zip" || ext == ".rar" || ext == ".tar" || ext == ".cab" ||
      ext == ".iso") {
    return ArchiveKind::Container;
  }
  // Single-payload streams.
  if (ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".zst" || ext == ".lz4" ||
      ext == ".br" || ext == ".lzma" || ext == ".z") {
    return ArchiveKind::SingleStream;
  }
  return ArchiveKind::NotArchive;
}


// ---------------------------------------------------------------------------
// The dragged tree
// ---------------------------------------------------------------------------
// A directory drag names only the top-level items; children is what the Win32
// layer found BELOW them (bounded walk). Keeping the shape (instead of a flat
// path list) is what lets the recursion rule live here and be mutated here.
struct DropEntry {
  std::string path;                 // UTF-8, as Explorer spelled it
  bool isDirectory = false;
  std::vector<DropEntry> children;  // only meaningful when isDirectory
};

struct DropInput {
  std::vector<DropEntry> entries;
  // The caller claim ("the drop contains a folder"). Reported when it
  // disagrees with the tree; NEVER used to override the tree (see the header
  // comment and DropPlan::declaredDirectoryMismatch).
  bool containsDirectory = false;
  bool archiveOpen = false;
  bool archiveReadOnly = false;
  // Bound on the flattened member list: a hostile folder must not turn a drop
  // into an unbounded allocation (same reasoning as DescriptorOptions::maxFiles).
  std::size_t maxMembers = 4096;
};

struct DropStep {
  DropAction action = DropAction::Refuse;
  std::string label;   // short UI label
  std::string detail;  // stable ASCII explanation ("OpenArchive: ...")
};

struct FlattenStats {
  std::size_t files = 0;
  std::size_t directories = 0;
  std::size_t containers = 0;
  std::size_t singleStreams = 0;
  bool overflow = false;  // the maxMembers bound was hit
};

// Depth bound for the recursion. A tree deeper than this is refused instead of
// silently truncated (a truncated add would lose data), and the bound keeps a
// pathological tree from exhausting the stack.
inline constexpr int kMaxDepth = 64;

inline void FlattenEntries(const std::vector<DropEntry>& entries, int depth,
                           std::vector<std::string>* members, FlattenStats* stats,
                           std::size_t maxMembers) {
  for (const DropEntry& entry : entries) {
    if (entry.path.empty()) continue;
    if (entry.isDirectory) {
      ++stats->directories;
      if (mutation::kFlatDirectories) {
        // D3: the folder is delivered as ONE member instead of its contents.
        if (members->size() < maxMembers) {
          members->push_back(entry.path);
          ++stats->files;
        } else {
          stats->overflow = true;
        }
        continue;
      }
      if (depth >= kMaxDepth) {
        stats->overflow = true;
        continue;
      }
      FlattenEntries(entry.children, depth + 1, members, stats, maxMembers);
      continue;
    }
    if (members->size() >= maxMembers) {
      stats->overflow = true;
      continue;
    }
    members->push_back(entry.path);
    ++stats->files;
    switch (Classify(entry.path)) {
      case ArchiveKind::Container: ++stats->containers; break;
      case ArchiveKind::SingleStream: ++stats->singleStreams; break;
      case ArchiveKind::NotArchive: break;
    }
  }
}

// How many top-level folders the drop names (the walk below them is
// FlattenEntries' job). Used by the Win32 layer to decide whether it must scan
// the filesystem at all.
inline bool TreeContainsDirectory(const std::vector<DropEntry>& entries) {
  for (const DropEntry& entry : entries) {
    if (entry.isDirectory) return true;
  }
  return false;
}

struct DropPlan {
  DropAction primary = DropAction::Refuse;
  DropReason reason = DropReason::None;
  std::vector<DropStep> alternatives;
  // True when the two candidate readings are both defensible and the UI should
  // ASK instead of acting on the default (a .7z dropped onto an open archive:
  // "add it" vs "open it" are both what a user might have meant).
  bool asksUser = false;
  std::size_t fileCount = 0;        // flattened members
  std::size_t archiveCount = 0;     // members that look like archives
  std::size_t directoryCount = 0;   // folders seen while flattening
  std::size_t containerCount = 0;
  std::size_t singleStreamCount = 0;
  bool recursiveExpansion = false;  // the folder contents, not the folder, are members
  bool declaredDirectoryMismatch = false;
  std::vector<std::string> members;  // flattened, drag order preserved
  std::string headline;              // one line: the hint title
  std::string detail;                // the hint second line; refusals name the reason

  bool accepted() const { return primary != DropAction::Refuse; }
  // C5 shape: a refusal that cannot name its reason is a bug, so the model
  // exposes the invariant instead of leaving callers to infer it.
  bool HasExplicitReason() const {
    return primary != DropAction::Refuse || reason != DropReason::None;
  }
};

inline std::string ActionLabel(DropAction action) {
  switch (action) {
    case DropAction::OpenArchive: return "打开归档";
    case DropAction::ExtractHere: return "解压到此处";
    case DropAction::AddToArchive: return "添加到归档";
    case DropAction::TestArchive: return "测试归档";
    case DropAction::Refuse: return "拒绝";
  }
  return "?";
}

inline DropStep MakeStep(DropAction action, std::string detail) {
  DropStep step;
  step.action = action;
  step.label = ActionLabel(action);
  step.detail = std::move(detail);
  return step;
}

inline std::string Quoted(std::string_view path) {
  return std::string("\xE2\x80\x9C") +
         std::string(prism::FileNameOf(prism::NormalizeArchivePath(path))) +
         std::string("\xE2\x80\x9D");
}

inline void RefuseWith(DropPlan* plan, DropReason reason, std::string headline,
                       std::string detail) {
  plan->primary = DropAction::Refuse;
  plan->reason = reason;
  plan->alternatives.clear();
  plan->asksUser = false;
  plan->headline = std::move(headline);
  plan->detail = std::string("拒绝 (") + ToString(reason) + ")：" + std::move(detail);
}


// ---------------------------------------------------------------------------
// The rule set (one function, because a split rule set is two rule sets)
// ---------------------------------------------------------------------------
inline DropPlan PlanDrop(const DropInput& input) {
  DropPlan plan;
  plan.declaredDirectoryMismatch =
      input.containsDirectory != TreeContainsDirectory(input.entries);

  FlattenStats stats;
  FlattenEntries(input.entries, 0, &plan.members, &stats, input.maxMembers);
  plan.fileCount = plan.members.size();
  plan.directoryCount = stats.directories;
  plan.archiveCount = stats.containers + stats.singleStreams;
  plan.containerCount = stats.containers;
  plan.singleStreamCount = stats.singleStreams;
  plan.recursiveExpansion = stats.directories > 0 && !mutation::kFlatDirectories;

  if (stats.overflow) {
    RefuseWith(&plan, DropReason::TooManyMembers, "无法添加：项目太多",
               "拖入的内容超过 " + std::to_string(input.maxMembers) + " 项上限，文件夹未展开");
    return plan;
  }

  // The first non-archive member names itself in the refusal: "something here
  // is not an archive" is useless without telling the user WHICH file.
  std::string firstNonArchive;
  for (const std::string& member : plan.members) {
    if (Classify(member) == ArchiveKind::NotArchive) {
      firstNonArchive = member;
      break;
    }
  }
  const bool hasDirectory = plan.directoryCount > 0;

  if (hasDirectory) {
    // A folder has no "open" reading: it can only be ADDED to an open archive.
    if (!input.archiveOpen) {
      RefuseWith(&plan, DropReason::NoArchiveOpen, "无法添加：没有打开的归档",
                 "拖入了文件夹，但当前没有打开的归档可以接收它");
      return plan;
    }
    if (input.archiveReadOnly && !mutation::kReadOnlyAllowsAdd) {
      RefuseWith(&plan, DropReason::ReadOnlyArchive, "无法添加：归档为只读",
                 "当前归档是只读的，无法写入 " + std::to_string(plan.fileCount) + " 个文件");
      return plan;
    }
    if (plan.fileCount == 0) {
      RefuseWith(&plan, DropReason::EmptySelection, "没有可投放的文件", "拖入的文件夹里没有文件");
      return plan;
    }
    plan.primary = DropAction::AddToArchive;
    plan.headline = "添加到归档：" + std::to_string(plan.fileCount) + " 项";
    plan.detail = "AddToArchive: " + std::to_string(plan.fileCount) + " 个文件将写入当前归档";
    if (plan.recursiveExpansion) plan.detail += "（文件夹已递归展开）";
    return plan;
  }

  if (plan.fileCount == 0) {
    RefuseWith(&plan, DropReason::EmptySelection, "没有可投放的文件", "拖入的列表是空的");
    return plan;
  }

  const bool allArchives = plan.archiveCount == plan.fileCount;

  if (!input.archiveOpen) {
    // Nothing to add to: the drop must BE an archive, or it is refused.
    if (!allArchives) {
      RefuseWith(&plan, DropReason::NotAnArchive, "无法打开：不是归档文件",
                 Quoted(firstNonArchive.empty() ? plan.members.front() : firstNonArchive) +
                     " 不是归档，且当前没有打开的归档可供添加");
      return plan;
    }
    const std::string& first = plan.members.front();
    if (plan.singleStreamCount == plan.fileCount) {
      // A .gz/.zst payload has no tree to browse: extracting IS the useful
      // action, opening it is the alternative.
      plan.primary = DropAction::ExtractHere;
      plan.headline = "解压到此处：" +
                      std::string(prism::FileNameOf(prism::NormalizeArchivePath(first)));
      plan.detail = "ExtractHere: 单文件压缩流，解压出内容而不是浏览它";
      plan.alternatives.push_back(MakeStep(DropAction::OpenArchive, "OpenArchive: 仍然在窗口中打开"));
      if (plan.fileCount > 1) plan.asksUser = true;
      return plan;
    }
    plan.primary = DropAction::OpenArchive;
    plan.headline = "打开归档：" +
                    std::string(prism::FileNameOf(prism::NormalizeArchivePath(first)));
    plan.detail = "OpenArchive: 在窗口中打开 " + first;
    plan.alternatives.push_back(MakeStep(DropAction::ExtractHere, "ExtractHere: 直接解压，不打开"));
    // Several archives and no window to show them in: which one opens is the
    // user call, so the hint asks instead of silently picking one.
    if (plan.fileCount > 1) {
      plan.asksUser = true;
      plan.detail += "（拖入了 " + std::to_string(plan.fileCount) + " 个归档）";
    }
    return plan;
  }

  // An archive is open.
  if (allArchives) {
    if (input.archiveReadOnly && !mutation::kReadOnlyAllowsAdd) {
      // Adding is impossible, but the dropped archive can still be verified
      // without touching the read-only one -- that is the default, and opening
      // it (which replaces the view) stays available as the alternative.
      const std::string& first = plan.members.front();
      plan.primary = DropAction::TestArchive;
      plan.headline = "测试归档：" +
                      std::string(prism::FileNameOf(prism::NormalizeArchivePath(first)));
      plan.detail = "TestArchive: 当前归档只读，无法添加；只做完整性校验";
      plan.alternatives.push_back(
          MakeStep(DropAction::OpenArchive, "OpenArchive: 用拖入的归档替换当前视图"));
      plan.alternatives.push_back(MakeStep(DropAction::ExtractHere, "ExtractHere: 解压到此处"));
      plan.asksUser = true;
      return plan;
    }
    // Both readings are legitimate: add the dropped archive as a member, or
    // open it. The default is "add" (the drop landed ON an archive), the hint
    // asks, and OpenArchive/TestArchive are one click away.
    plan.primary = DropAction::AddToArchive;
    plan.headline = "添加到归档：" + std::to_string(plan.fileCount) + " 项";
    plan.detail = "AddToArchive: 拖入的归档将作为成员写入当前归档";
    plan.alternatives.push_back(
        MakeStep(DropAction::OpenArchive, "OpenArchive: 改为打开拖入的归档"));
    plan.alternatives.push_back(
        MakeStep(DropAction::TestArchive, "TestArchive: 只校验，不写入也不打开"));
    plan.asksUser = true;
    return plan;
  }

  // Mixed content (or plain files): the intent is unambiguous -- add them.
  if (input.archiveReadOnly && !mutation::kReadOnlyAllowsAdd) {
    RefuseWith(&plan, DropReason::ReadOnlyArchive, "无法添加：归档为只读",
               "当前归档是只读的，无法写入 " + std::to_string(plan.fileCount) + " 个文件");
    return plan;
  }
  plan.primary = DropAction::AddToArchive;
  plan.headline = "添加到归档：" + std::to_string(plan.fileCount) + " 项";
  plan.detail = "AddToArchive: " + std::to_string(plan.fileCount) + " 个文件将写入当前归档";
  return plan;
}


// ---------------------------------------------------------------------------
// Overlay geometry
//
// The hint is a centred band inside the CONTENT area, so it can never invite a
// click on chrome (the action band, the status bar, a rail) that it does not
// cover, and a tiny window degrades to "no hint" instead of an inverted rect.
// Pure integer geometry: the Windows suite asserts the produced rect is inside
// the same prism::layout::Rect the renderer paints.
// ---------------------------------------------------------------------------
inline constexpr int kHintPadding = 24;
inline constexpr int kHintMaxHeight = 120;

inline prism::layout::Rect HintRect(const prism::layout::Rect& contentInner) {
  const prism::layout::Rect inner = prism::layout::Inset(contentInner, kHintPadding);
  if (inner.Empty()) return prism::layout::Rect{};
  const int height = (std::min)(kHintMaxHeight, inner.Height());
  const int top = inner.top + (inner.Height() - height) / 2;
  return prism::layout::Rect{inner.left, top, inner.right, top + height};
}

}  // namespace prism::drop
