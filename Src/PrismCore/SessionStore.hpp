#pragma once
// PrismArchive -- SessionStore (card C46): the last session, and what it may
// recover.
//
// C14/C27 already persist the user's PREFERENCES (favourites, recents, window
// size, sort key, column widths). Preferences answer "how do I like it"; they
// never answer "what was I doing". Closing the app and reopening it therefore
// loses the archive the user had open and the folder inside it -- the exact
// thing Bandizip remembers for you.
//
// This header is the decision layer and nothing else:
//   * the document model: up to kMaxEntries archives, each with the folder the
//     user was in and the view mode (list / grid), plus which entry was ACTIVE;
//   * a schema version with a real migration (v1 = paths only, v2 = folders and
//     view modes) and a policy for a version this build does not know: DISCARD
//     and warn once. Never crash, never half-read a stranger's document;
//   * C14's JSON scanner/serializer, reused -- a second parser in the tree would
//     be a second set of escaping bugs (the C14 suite already owns that one);
//   * the CRASH GUARD: a session document is only restorable when the previous
//     process marked a CLEAN shutdown. A killed process leaves the dirty mark
//     behind, so the next start offers nothing instead of reopening stale tabs.
//     That policy lives here (RestorableEntries) so the pure suite and the exe
//     share ONE implementation -- a policy duplicated in Main.cpp is a policy
//     the negative control cannot reach.
//
// NOT here, on purpose: any file path, any Win32 call, any message box. The file
// layer (atomic temp + ReplaceFileW, %LOCALAPPDATA% placement) is
// prism::win::settingsfile (C14) driven from Src/App/AppWiring.hpp; PrismCore
// stays compilable on Linux, which is what lets the schema/migration/crash rules
// be tested in milliseconds instead of on a window station.
//
// Contracts touched: D1 (stdout marker is part of the verdict), D3 (negative
// controls must be able to go red), B1/B2 (header hygiene), D26 (mutant marker
// is a conjunction, never PASS_REGULAR_EXPRESSION + WILL_FAIL).

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SettingsStore.hpp"  // C14: JSON parse/serialize (one parser in the tree)

namespace prism::session {

// ---------------------------------------------------------------------------
// 1. Document shape
// ---------------------------------------------------------------------------

// v1: {"schemaVersion":1,"cleanShutdown":true,"archives":["C:/a.7z", ...]}
//     (paths only -- what a "remember the last files" prototype would write)
// v2: entries carry the folder inside the archive and the view mode.
inline constexpr int kSchemaVersion = 2;
inline constexpr int kLegacySchemaVersion = 1;

// The card's ceiling. A ninth open archive is not "a session", it is a browser.
inline constexpr std::size_t kMaxEntries = 8;

// Bounds that keep a hostile/truncated document from growing a window title or
// a breadcrumb walk without limit.
inline constexpr std::size_t kMaxPathChars = 4096;
inline constexpr std::size_t kMaxFolderChars = 1024;

struct SessionEntry {
  std::string path;      // archive path, '/' separators (PrismCore convention)
  std::string folder;    // current folder INSIDE the archive ("" == root)
  bool listMode = false;  // false == grid view
};

enum class SessionLoadStatus : std::uint8_t {
  kMissing = 0,            // no document: first run, or one was never written
  kLoaded = 1,             // a v2 document was read
  kMigrated = 2,           // a v1 document was read and upgraded
  kCorrupt = 3,            // not JSON / not an object: DISCARDED
  kUnsupportedVersion = 4, // newer than this build knows: DISCARDED
};

inline const char* SessionLoadStatusName(SessionLoadStatus status) {
  switch (status) {
    case SessionLoadStatus::kMissing: return "missing";
    case SessionLoadStatus::kLoaded: return "loaded";
    case SessionLoadStatus::kMigrated: return "migrated";
    case SessionLoadStatus::kCorrupt: return "corrupt";
    case SessionLoadStatus::kUnsupportedVersion: return "unsupported-version";
  }
  return "?";
}

inline const char* ViewModeName(bool listMode) { return listMode ? "list" : "grid"; }

inline bool ParseViewMode(std::string_view text, bool* listMode) {
  if (text == "list") {
    if (listMode != nullptr) *listMode = true;
    return true;
  }
  if (text == "grid") {
    if (listMode != nullptr) *listMode = false;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// 2. Path/folder normalization (pure, no filesystem)
// ---------------------------------------------------------------------------

// '/' is PrismCore's separator (CONTRACTS B4); the Win32 layer converts before
// it hands a path to the OS. Trims surrounding blanks, maps '\' to '/', collapses
// duplicate separators, drops a trailing separator, and clips over-long input
// instead of rejecting it (a path too long to remember is still a path).
inline std::string NormalizePath(std::string_view raw) {
  std::string text;
  text.reserve(raw.size());
  for (const char c : raw) text.push_back(c == '\\' ? '/' : c);
  std::size_t begin = 0;
  std::size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  std::string collapsed;
  collapsed.reserve(end - begin);
  for (std::size_t i = begin; i < end; ++i) {
    if (text[i] == '/' && !collapsed.empty() && collapsed.back() == '/') continue;
    collapsed.push_back(text[i]);
  }
  // "C:/" must survive: a drive root is one separator, not a trailing one.
  const bool driveRoot = collapsed.size() == 3 && collapsed[1] == ':' && collapsed[2] == '/';
  while (!driveRoot && !collapsed.empty() && collapsed.back() == '/') collapsed.pop_back();
  if (collapsed.size() > kMaxPathChars) collapsed.resize(kMaxPathChars);
  return collapsed;
}

// Folder names inside an archive: no leading '/' (the root is ""), no trailing
// '/', no "//", no "." segments. ".." is refused (clamped to the root) because a
// session file is DATA, and data must not be able to walk out of the archive.
inline std::string NormalizeFolder(std::string_view raw) {
  std::string text;
  text.reserve(raw.size());
  for (const char c : raw) text.push_back(c == '\\' ? '/' : c);
  std::string out;
  out.reserve(text.size());
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t slash = text.find('/', start);
    if (slash == std::string::npos) slash = text.size();
    const std::string_view segment(text.data() + start, slash - start);
    if (!segment.empty() && segment != ".") {
      if (segment == "..") {
        out.clear();
      } else {
        if (!out.empty()) out.push_back('/');
        out.append(segment);
      }
    }
    if (slash == text.size()) break;
    start = slash + 1;
  }
  if (out.size() > kMaxFolderChars) out.resize(kMaxFolderChars);
  // Clipping may have cut mid-segment; never leave a dangling separator.
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

inline std::vector<std::string> FolderSegments(std::string_view folder) {
  std::vector<std::string> segments;
  const std::string clean = NormalizeFolder(folder);
  std::size_t start = 0;
  while (start < clean.size()) {
    std::size_t slash = clean.find('/', start);
    if (slash == std::string::npos) slash = clean.size();
    segments.emplace_back(clean.substr(start, slash - start));
    start = slash + 1;
  }
  return segments;
}

// ---------------------------------------------------------------------------
// 3. The store
// ---------------------------------------------------------------------------

struct SessionLoadReport {
  SessionLoadStatus status = SessionLoadStatus::kMissing;
  bool hadDocument = false;        // bytes existed on disk
  bool cleanShutdown = false;      // the previous process closed normally
  std::size_t discardedEntries = 0;  // malformed entries dropped while reading
  std::vector<std::string> warnings;  // one line each, for the C16 logger
};

class SessionStore {
 public:
  SessionStore() = default;

  // Reads a document. Never throws, never half-applies: on a corrupt or
  // newer-than-known document the store ends up EMPTY with a warning, which is
  // the "discard, do not crash" contract the card asks for.
  SessionLoadReport Load(std::string_view document) {
    SessionLoadReport report;
    entries_.clear();
    active_ = -1;
    clean_ = false;

    std::size_t begin = 0;
    std::size_t end = document.size();
    while (begin < end && IsBlank(document[begin])) ++begin;
    while (end > begin && IsBlank(document[end - 1])) --end;
    if (begin == end) return report;  // kMissing: nothing was ever written

    report.hadDocument = true;
    const std::string_view text = document.substr(begin, end - begin);
    prism::settings::JsonValue root;
    prism::settings::JsonParseError parseError;
#if defined(PRISM_C46_MUTANT_KEEP_BAD_JSON)
    // NEGATIVE CONTROL: the discard branch is compiled out. The suite's corrupt
    // -document case must go red; a stale injection leaves it green.
    prism::settings::ParseJson(text, root, &parseError);
#else
    if (!prism::settings::ParseJson(text, root, &parseError)) {
      report.status = SessionLoadStatus::kCorrupt;
      report.warnings.push_back("session document is not JSON (" + parseError.message +
                                " at byte " + std::to_string(parseError.offset) +
                                "); discarded");
      return report;
    }
#endif
    if (!root.IsObject()) {
#if !defined(PRISM_C46_MUTANT_KEEP_BAD_JSON)
      report.status = SessionLoadStatus::kCorrupt;
      report.warnings.push_back("session document is not a JSON object; discarded");
      return report;
#endif
    }

    int version = kLegacySchemaVersion;  // absent: the v1 prototype wrote no version
    if (const prism::settings::JsonValue* node = root.Find("schemaVersion")) {
      if (!node->IsNumber()) {
#if !defined(PRISM_C46_MUTANT_KEEP_BAD_JSON)
        report.status = SessionLoadStatus::kCorrupt;
        report.warnings.push_back("session schemaVersion is not a number; discarded");
        return report;
#endif
      } else {
        version = static_cast<int>(node->number);
      }
    }
#if !defined(PRISM_C46_MUTANT_IGNORE_SCHEMA_VERSION)
    if (version > kSchemaVersion) {
      report.status = SessionLoadStatus::kUnsupportedVersion;
      report.warnings.push_back(
          "session schema version " + std::to_string(version) + " is newer than " +
          std::to_string(kSchemaVersion) + "; the session was discarded (it will be rewritten)");
      return report;
    }
    if (version < kLegacySchemaVersion) {
      report.status = SessionLoadStatus::kCorrupt;
      report.warnings.push_back("session schema version " + std::to_string(version) +
                                " is not one this build knows; discarded");
      return report;
    }
#endif

    if (const prism::settings::JsonValue* node = root.Find("cleanShutdown")) {
      if (node->type == prism::settings::JsonType::Bool) {
        clean_ = node->boolean;
        report.cleanShutdown = clean_;
      }
    }

    const prism::settings::JsonValue* list =
        root.Find(version >= kSchemaVersion ? "entries" : "archives");
    if (list != nullptr && list->type == prism::settings::JsonType::Array) {
      for (const prism::settings::JsonValue& item : list->items) {
        SessionEntry entry;
        if (version >= kSchemaVersion) {
          if (!item.IsObject()) {
            ++report.discardedEntries;
            continue;
          }
          const prism::settings::JsonValue* path = item.Find("path");
          if (path == nullptr || !path->IsString() || NormalizePath(path->text).empty()) {
            ++report.discardedEntries;
            continue;
          }
          entry.path = NormalizePath(path->text);
          if (const prism::settings::JsonValue* folder = item.Find("folder")) {
            if (folder->IsString()) entry.folder = NormalizeFolder(folder->text);
          }
          if (const prism::settings::JsonValue* view = item.Find("view")) {
            if (view->IsString() && !ParseViewMode(view->text, &entry.listMode)) {
              // An unknown view mode is repaired to the grid, not fatal: the
              // user's place in the archive matters more than the layout name.
              report.warnings.push_back("session view mode '" + view->text +
                                        "' is unknown; using the grid for " + entry.path);
            }
          }
        } else {
          // v1 stored bare strings.
          if (!item.IsString() || NormalizePath(item.text).empty()) {
            ++report.discardedEntries;
            continue;
          }
          entry.path = NormalizePath(item.text);
        }
        if (entries_.size() >= kMaxEntries) {  // defensive: a huge document
          ++report.discardedEntries;
          continue;
        }
        entries_.push_back(std::move(entry));
      }
    }

    if (const prism::settings::JsonValue* node = root.Find("active")) {
      if (node->IsNumber()) {
        const int requested = static_cast<int>(node->number);
        if (requested >= 0 && requested < static_cast<int>(entries_.size())) active_ = requested;
      }
    }
    if (active_ < 0 && !entries_.empty()) active_ = 0;

    if (version < kSchemaVersion) {
      report.status = SessionLoadStatus::kMigrated;
      report.warnings.push_back("session schema v" + std::to_string(version) + " migrated to v" +
                                std::to_string(kSchemaVersion) + " (folders and view modes default" +
                                " to the archive root / grid)");
    } else {
      report.status = SessionLoadStatus::kLoaded;
    }
    return report;
  }

  void Reset() {
    entries_.clear();
    active_ = -1;
    clean_ = false;
  }

  const std::vector<SessionEntry>& entries() const { return entries_; }
  int activeIndex() const { return active_; }
  bool empty() const { return entries_.empty(); }

  const SessionEntry* activeEntry() const {
    if (active_ < 0 || static_cast<std::size_t>(active_) >= entries_.size()) return nullptr;
    return &entries_[static_cast<std::size_t>(active_)];
  }

  bool cleanShutdown() const { return clean_; }

  // Written by the CURRENT process: "in use" at startup, "clean" at exit. A
  // crash between the two leaves false on disk, which is the whole guard.
  void MarkCleanShutdown(bool clean) { clean_ = clean; }

  int IndexOf(std::string_view path) const {
    const std::string wanted = NormalizePath(path);
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].path == wanted) return static_cast<int>(i);
    }
    return -1;
  }

  // What the next start is allowed to reopen. THE crash guard:
  // an unclean shutdown returns nothing, whatever the document says.
  std::vector<SessionEntry> RestorableEntries() const {
#if defined(PRISM_C46_MUTANT_IGNORE_CLEAN_SHUTDOWN)
    // NEGATIVE CONTROL: the dirty mark no longer suppresses a restore, so the
    // killed-process case reopens the dead session. The suite MUST go red.
    return entries_;
#else
    if (!clean_) return {};
    return entries_;
#endif
  }

  // MRU: the just-opened archive moves to the front, the list is de-duplicated
  // (one archive = one tab) and clipped to kMaxEntries from the tail.
  bool RecordOpen(std::string_view path, std::string_view folder, bool listMode) {
    const std::string normalized = NormalizePath(path);
    if (normalized.empty()) return false;
    SessionEntry entry;
    entry.path = normalized;
    entry.folder = NormalizeFolder(folder);
    entry.listMode = listMode;
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].path != normalized) continue;
      entry.folder = entry.folder.empty() ? entries_[i].folder : entry.folder;
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
      break;
    }
    entries_.insert(entries_.begin(), std::move(entry));
    if (entries_.size() > kMaxEntries) {
      entries_.resize(kMaxEntries);
    }
    active_ = 0;
    return true;
  }

  // The active archive's place, updated when the user navigates or switches the
  // view. A path that is no longer in the session is ignored (nothing to write).
  bool UpdatePosition(std::string_view path, std::string_view folder, bool listMode) {
    const int index = IndexOf(path);
    if (index < 0) return false;
    SessionEntry& entry = entries_[static_cast<std::size_t>(index)];
    entry.folder = NormalizeFolder(folder);
    entry.listMode = listMode;
    return true;
  }

  bool Remove(std::string_view path) {
    const int index = IndexOf(path);
    if (index < 0) return false;
    entries_.erase(entries_.begin() + index);
    if (entries_.empty()) {
      active_ = -1;
    } else if (active_ >= static_cast<int>(entries_.size())) {
      active_ = static_cast<int>(entries_.size()) - 1;
    }
    return true;
  }

  // Replaces the whole list (the "arguments win" path). Invalid entries are
  // dropped, duplicates keep their first occurrence, the tail is clipped.
  void Replace(std::vector<SessionEntry> entries, int activeIndex = 0) {
    entries_.clear();
    for (SessionEntry& entry : entries) {
      const std::string path = NormalizePath(entry.path);
      if (path.empty()) continue;
      if (IndexOf(path) >= 0) continue;
      if (entries_.size() >= kMaxEntries) break;
      entry.path = path;
      entry.folder = NormalizeFolder(entry.folder);
      entries_.push_back(std::move(entry));
    }
    active_ = 0;
    if (activeIndex > 0 && activeIndex < static_cast<int>(entries_.size())) active_ = activeIndex;
    if (entries_.empty()) active_ = -1;
  }

  // Stable serialization: schema version first, then the crash guard, then the
  // list. Reads better than it parses, and the parser accepts any order.
  std::string ToJson() const {
    prism::settings::JsonValue root = prism::settings::MakeObject();
    root.SetMember("schemaVersion", prism::settings::MakeNumber(kSchemaVersion));
    root.SetMember("cleanShutdown", prism::settings::MakeBool(clean_));
    root.SetMember("active", prism::settings::MakeNumber(active_));
    prism::settings::JsonValue list;
    list.type = prism::settings::JsonType::Array;
    for (const SessionEntry& entry : entries_) {
      prism::settings::JsonValue node = prism::settings::MakeObject();
      node.SetMember("path", prism::settings::MakeString(entry.path));
      node.SetMember("folder", prism::settings::MakeString(entry.folder));
      node.SetMember("view", prism::settings::MakeString(ViewModeName(entry.listMode)));
      list.items.push_back(std::move(node));
    }
    root.SetMember("entries", std::move(list));
    prism::settings::JsonWriteOptions options;
    options.pretty = true;
    options.indent = 2;
    return prism::settings::SerializeJson(root, options);
  }

 private:
  static bool IsBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
  }

  std::vector<SessionEntry> entries_;
  int active_ = -1;
  bool clean_ = false;
};

}  // namespace prism::session
