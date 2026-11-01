// PrismArchive -- PrismCore
// SidebarModel: the decision layer behind the sidebar of blueprint 3.2
// (FAVORITES / RECENT / ARCHIVE TREE / the bottom telemetry pill).
//
// The window draws; this file decides. Everything here is pure C++20 with no
// Windows, no Direct2D and no filesystem access, so the whole sidebar can be
// driven from a Linux CI lane in milliseconds:
//
//   * FAVORITES  -- user-defined shortcuts (label + path + icon id), order kept
//                   exactly as configured;
//   * RECENT     -- MRU list with case/separator-insensitive de-duplication,
//                   a hard capacity and newest-first time order;
//   * ARCHIVE TREE -- the folder tree of the open archive, with the SAME path
//                   semantics as tree::Navigator (PrismCore-normalized '/'
//                   separated paths, no leading '/', pseudo entries dropped,
//                   digit-run natural ordering -- see ArchiveTreeView.hpp). It
//                   is verified by a differential test that walks this tree and
//                   compares every level against tree::Build. Files are NOT in
//                   the tree: the blueprint's tree lists folders, the content
//                   list owns files.
//   * STATUS     -- encryption / method / dictionary / compression ratio. The
//                   method and dictionary are engine facts (7z properties), so
//                   they are INJECTED; only the totals and the ratio are
//                   computed here.
//   * FOCUS      -- a single keyboard focus over the flattened order
//                   (favorites, then recent, then visible tree rows): Home/End,
//                   Up/Down cycling, Left/Right collapse-expand (the blueprint's
//                   tree interaction), Enter activation.
//
// Persistence goes through an injected PORT (SidebarStore). Card C14 owns the
// real settings store; this header never includes or links it -- the model must
// stay testable with an in-memory fake, and the app decides when to save.
//
// Negative controls (CONTRACTS D3): the compile-time switches below exist so a
// mutation build can turn a rule OFF and prove the suite goes red. They are
// never defined in a shipping build.
#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "ArchiveTreeView.hpp"
#include "PathSafety.hpp"

namespace prism::sidebar {

// ----------------------------------------------------------------- mutations
// One switch per rule the suite claims to guard. PRISM_SIDEBAR_MUTANT_* is set
// only by the mutation.sidebar.* CMake targets (WILL_FAIL), never by app code.
#if defined(PRISM_SIDEBAR_MUTANT_NO_RECENT_DEDUPE)
inline constexpr bool kRecentDedupeEnabled = false;
#else
inline constexpr bool kRecentDedupeEnabled = true;
#endif
#if defined(PRISM_SIDEBAR_MUTANT_NO_RECENT_CAP)
inline constexpr bool kRecentCapEnabled = false;
#else
inline constexpr bool kRecentCapEnabled = true;
#endif
#if defined(PRISM_SIDEBAR_MUTANT_NO_FOCUS_WRAP)
inline constexpr bool kFocusWrapEnabled = false;
#else
inline constexpr bool kFocusWrapEnabled = true;
#endif

// The MRU list is a capsule of "where was I", not a history: eight entries fit
// the sidebar without scrolling and each is one click away.
inline constexpr std::size_t kRecentCapacity = 8;

// Hostile archives can contain a folder chain thousands of levels deep. The
// tree is a navigation aid, not a stack; 64 levels is far beyond any real
// archive and bounds both the flatten recursion and a hostile input.
inline constexpr int kMaxTreeDepth = 64;

inline constexpr std::string_view kSettingsKey = "sidebar.state.v1";
inline constexpr std::string_view kSerializationMagic = "prism-sidebar 1";

// -------------------------------------------------------------------- enums
enum class Section : std::uint8_t { None, Favorites, Recent, Tree };

inline std::ostream& operator<<(std::ostream& os, Section section) {
  switch (section) {
    case Section::None: return os << "None";
    case Section::Favorites: return os << "Favorites";
    case Section::Recent: return os << "Recent";
    case Section::Tree: return os << "Tree";
  }
  return os << "?";
}

// 7-Zip reports "encrypted" as a property; the cipher name is archive metadata
// the engine does not always answer. "Unknown" is therefore a real state: items
// are encrypted but we cannot name the cipher -- never pretend AES-256.
enum class Encryption : std::uint8_t { None, Aes256, Unknown };

inline std::ostream& operator<<(std::ostream& os, Encryption encryption) {
  switch (encryption) {
    case Encryption::None: return os << "None";
    case Encryption::Aes256: return os << "Aes256";
    case Encryption::Unknown: return os << "Unknown";
  }
  return os << "?";
}

enum class FocusAction : std::uint8_t { None, Moved, Toggled, Activated };

inline std::ostream& operator<<(std::ostream& os, FocusAction action) {
  switch (action) {
    case FocusAction::None: return os << "None";
    case FocusAction::Moved: return os << "Moved";
    case FocusAction::Toggled: return os << "Toggled";
    case FocusAction::Activated: return os << "Activated";
  }
  return os << "?";
}

// ------------------------------------------------------------------- records
struct Favorite {
  std::string label;  // display name; "" -> the renderer uses the path's leaf
  std::string path;   // filesystem path, opaque to PrismCore
  std::string icon;   // icon id ("home", "download", "work", ...); renderer maps it
};

struct RecentEntry {
  std::string path;
  std::int64_t openedAt = 0;  // caller-supplied ticks; ordering is all this uses
};

// Engine facts the sidebar cannot derive from the catalog alone.
struct ArchiveInfo {
  std::string method;                        // "LZMA2", "Deflate", ...
  std::uint64_t dictionaryBytes = 0;          // 0 -> the pill shows "--"
  Encryption encryption = Encryption::None;
};

struct TreeRow {
  std::string path;  // PrismCore-normalized archive path ("" = archive root)
  std::string name;  // display label (last segment; the root uses the archive name)
  int depth = 0;
  bool isRoot = false;
  bool expandable = false;   // has child folders
  bool expanded = false;     // expandable AND not collapsed
  bool explicitDir = false;  // the archive stored this folder explicitly
  std::uint32_t catalogIndex = prism::tree::kSyntheticIndex;
};

struct FocusTarget {
  Section section = Section::None;
  int index = -1;  // index inside that section's list (visible rows for the tree)

  bool Valid() const { return section != Section::None && index >= 0; }
};

inline bool operator==(const FocusTarget& a, const FocusTarget& b) {
  return a.section == b.section && a.index == b.index;
}

struct Status {
  bool hasArchive = false;
  bool encrypted = false;
  Encryption encryption = Encryption::None;
  std::string method;
  std::uint64_t dictionaryBytes = 0;
  std::uint64_t totalUncompressed = 0;
  std::uint64_t totalPacked = 0;
  std::uint32_t ratioPermille = 0;  // packed/unpacked * 1000 (blueprint 42.8% -> 428)
  std::size_t fileCount = 0;
  std::size_t folderCount = 0;
};

// ------------------------------------------------------- persistence port
// Implemented by card C14's settings store, or by the app's own adapter. The
// model only needs string get/set: no window handles, no registry, no INI.
class SidebarStore {
 public:
  virtual ~SidebarStore() = default;
  virtual std::string Load(std::string_view key) const = 0;
  virtual void Store(std::string_view key, std::string_view value) = 0;
};

namespace detail {

inline char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// De-duplication identity: '/' and '\' are the same separator, runs collapse,
// a trailing separator is noise, and case is ignored (Windows paths are
// case-insensitive). The spelling the user first opened is what we display.
inline std::string RecentKey(std::string_view path) {
  std::string key;
  key.reserve(path.size());
  bool prevSlash = false;
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c == '/') {
      if (prevSlash) continue;
      prevSlash = true;
    } else {
      prevSlash = false;
    }
    key.push_back(LowerAscii(c));
  }
  while (!key.empty() && key.back() == '/') key.pop_back();
  return key;
}

inline std::string NormalizeDisplayPath(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  bool prevSlash = false;
  for (char c : path) {
    if (c == '\\') c = '/';
    if (c == '/') {
      if (prevSlash) continue;
      prevSlash = true;
    } else {
      prevSlash = false;
    }
    out.push_back(c);
  }
  while (!out.empty() && out.back() == '/') out.pop_back();
  return out;
}

inline bool IsSameOrDescendant(std::string_view path, std::string_view folder) {
  if (folder.empty()) return true;  // the root owns everything
  if (path == folder) return true;
  if (path.size() <= folder.size()) return false;
  if (path.compare(0, folder.size(), folder) != 0) return false;
  return path[folder.size()] == '/';
}

inline std::string EscapeField(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (char c : text) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

inline std::string UnescapeField(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\\' || i + 1 >= text.size()) {
      out.push_back(text[i]);
      continue;
    }
    const char next = text[++i];
    switch (next) {
      case '\\': out.push_back('\\'); break;
      case 't': out.push_back('\t'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      default:
        out.push_back('\\');
        out.push_back(next);
    }
  }
  return out;
}

inline std::vector<std::string> SplitTabs(std::string_view line) {
  std::vector<std::string> parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t tab = line.find('\t', start);
    if (tab == std::string_view::npos) {
      parts.emplace_back(line.substr(start));
      break;
    }
    parts.emplace_back(line.substr(start, tab - start));
    start = tab + 1;
  }
  return parts;
}

inline bool ParseInt64(std::string_view text, std::int64_t* out) {
  if (text.empty()) return false;
  std::int64_t value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc() || result.ptr != text.data() + text.size()) return false;
  *out = value;
  return true;
}

}  // namespace detail

// -------------------------------------------------------------------- model
class Model {
 public:
  Model() = default;
  explicit Model(const std::vector<ArchiveItem>* catalog) { AttachCatalog(catalog); }

  // --- archive catalog (borrowed; the app owns the snapshot) ----------------
  void AttachCatalog(const std::vector<ArchiveItem>* catalog) {
    catalog_ = catalog;
    BuildFolderIndex();
    rowsDirty_ = true;
    Tree();  // materialise now so focus clamping sees the real row list
    ClampFocus();
  }

  void SetArchiveName(std::string name) {
    if (name == archiveName_) return;
    archiveName_ = std::move(name);
    rowsDirty_ = true;  // the root row shows the archive name: repaint it now
    Tree();
  }
  const std::string& ArchiveName() const { return archiveName_; }

  void SetArchiveInfo(ArchiveInfo info) { info_ = std::move(info); }
  const ArchiveInfo& Info() const { return info_; }

  Status ComputeStatus() const {
    Status status;
    status.hasArchive = catalog_ != nullptr;
    status.method = info_.method;
    status.dictionaryBytes = info_.dictionaryBytes;
    bool anyEncrypted = false;
    if (catalog_ != nullptr) {
      for (const ArchiveItem& item : *catalog_) {
        if (item.isDir) {
          ++status.folderCount;
          continue;
        }
        ++status.fileCount;
        status.totalUncompressed += item.size;
        status.totalPacked += item.packSize;
        if (item.encrypted) anyEncrypted = true;
      }
    }
    status.encryption = info_.encryption;
    if (status.encryption == Encryption::None && anyEncrypted) {
      status.encryption = Encryption::Unknown;  // items are locked, cipher unnamed
    }
    status.encrypted = anyEncrypted || info_.encryption != Encryption::None;
    if (status.totalUncompressed > 0) {
      const long double ratio = static_cast<long double>(status.totalPacked) * 1000.0L /
                                static_cast<long double>(status.totalUncompressed);
      status.ratioPermille = static_cast<std::uint32_t>(std::llround(ratio));
    }
    return status;
  }

  // --- FAVORITES -----------------------------------------------------------
  void SetFavorites(std::vector<Favorite> favorites) {
    const FocusSnapshot snapshot = Snapshot();
    favorites_ = std::move(favorites);
    RestoreFocus(snapshot);
  }
  const std::vector<Favorite>& Favorites() const { return favorites_; }

  // --- RECENT (de-duplicated, capped, newest first) ------------------------
  void PushRecent(std::string_view path, std::int64_t openedAt) {
    const FocusSnapshot snapshot = Snapshot();
    std::string normalized = detail::NormalizeDisplayPath(path);
    if (normalized.empty()) return;
    if (kRecentDedupeEnabled) {
      const std::string key = detail::RecentKey(normalized);
      // Re-opening an entry must never make it look OLDER than it was: a
      // caller with a coarse or non-monotonic clock would otherwise reorder the
      // capsule. Keep the maximum stamp of the duplicate pair.
      std::int64_t newest = openedAt;
      for (const RecentEntry& entry : recent_) {
        if (detail::RecentKey(entry.path) == key && entry.openedAt > newest) {
          newest = entry.openedAt;
        }
      }
      openedAt = newest;
      recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
                                   [&key](const RecentEntry& entry) {
                                     return detail::RecentKey(entry.path) == key;
                                   }),
                    recent_.end());
    }
    recent_.insert(recent_.begin(), RecentEntry{std::move(normalized), openedAt});
    SortAndCapRecent();
    RestoreFocus(snapshot);
  }

  void SetRecent(std::vector<RecentEntry> entries) {
    const FocusSnapshot snapshot = Snapshot();
    std::vector<RecentEntry> merged;
    merged.reserve(entries.size());
    for (const RecentEntry& entry : entries) {
      std::string normalized = detail::NormalizeDisplayPath(entry.path);
      if (normalized.empty()) continue;
      if (kRecentDedupeEnabled) {
        const std::string key = detail::RecentKey(normalized);
        bool duplicate = false;
        for (RecentEntry& kept : merged) {
          if (detail::RecentKey(kept.path) != key) continue;
          if (entry.openedAt > kept.openedAt) kept.openedAt = entry.openedAt;
          duplicate = true;
          break;
        }
        if (duplicate) continue;
      }
      merged.push_back(RecentEntry{std::move(normalized), entry.openedAt});
    }
    recent_ = std::move(merged);
    SortAndCapRecent();
    RestoreFocus(snapshot);
  }

  void ClearRecent() {
    const FocusSnapshot snapshot = Snapshot();
    recent_.clear();
    RestoreFocus(snapshot);
  }

  const std::vector<RecentEntry>& Recent() const { return recent_; }

  // --- ARCHIVE TREE -------------------------------------------------------
  // Visible rows in paint order: the archive root first, then its children
  // depth-first, skipping the subtrees of collapsed folders.
  const std::vector<TreeRow>& Tree() const {
    if (rowsDirty_) {
      Flatten();
      rowsDirty_ = false;
    }
    return rows_;
  }

  bool IsCollapsed(std::string_view folder) const {
    return collapsed_.find(prism::NormalizeArchivePath(folder)) != collapsed_.end();
  }

  void SetCollapsed(std::string_view folder, bool collapse) {
    const std::string normalized = prism::NormalizeArchivePath(folder);
    if (folders_.find(normalized) == folders_.end()) return;  // not a folder we show
    if (collapse == IsCollapsed(normalized)) return;

    FocusSnapshot snapshot = Snapshot();
    bool focusWasHidden = false;
    if (collapse && snapshot.section == Section::Tree &&
        detail::IsSameOrDescendant(snapshot.path, normalized)) {
      // The row the user was looking at is about to disappear. Land on the
      // folder that closed (7-Zip keeps the parent focused: PanelKey.cpp's
      // OpenParentFolder contract) instead of pointing at a hidden index.
      focusWasHidden = true;
      snapshot.path = normalized;
      snapshot.index = -1;
    }
    if (collapse) {
      collapsed_.insert(normalized);
    } else {
      collapsed_.erase(normalized);
    }
    rowsDirty_ = true;
    Tree();
    if (focusWasHidden) {
      const int row = RowIndexOf(normalized);
      if (row >= 0) snapshot.index = row;
    }
    RestoreFocus(snapshot);
  }

  void ToggleCollapsed(std::string_view folder) {
    SetCollapsed(folder, !IsCollapsed(folder));
  }

  // --- FOCUS --------------------------------------------------------------
  std::vector<FocusTarget> FocusOrder() const {
    std::vector<FocusTarget> order;
    order.reserve(favorites_.size() + recent_.size() + Tree().size());
    for (std::size_t i = 0; i < favorites_.size(); ++i) {
      order.push_back(FocusTarget{Section::Favorites, static_cast<int>(i)});
    }
    for (std::size_t i = 0; i < recent_.size(); ++i) {
      order.push_back(FocusTarget{Section::Recent, static_cast<int>(i)});
    }
    const std::vector<TreeRow>& rows = Tree();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      order.push_back(FocusTarget{Section::Tree, static_cast<int>(i)});
    }
    return order;
  }

  FocusTarget Focus() const {
    const std::vector<FocusTarget> order = FocusOrder();
    if (order.empty()) return FocusTarget{};
    const int index = ClampOrdinal(focus_, static_cast<int>(order.size()));
    return order[static_cast<std::size_t>(index)];
  }

  bool SetFocus(FocusTarget target) {
    const std::vector<FocusTarget> order = FocusOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == target) {
        focus_ = static_cast<int>(i);
        return true;
      }
    }
    return false;
  }

  FocusAction Home() { return JumpToEdge(true); }
  FocusAction End() { return JumpToEdge(false); }

  FocusAction MoveDown() { return MoveBy(1); }
  FocusAction MoveUp() { return MoveBy(-1); }

  // Left: collapse an open folder, else walk to the parent row. Right: expand a
  // closed folder, else step into its first child. Favorites/recent are leaves.
  FocusAction MoveLeft() {
    const FocusTarget target = Focus();
    if (!target.Valid() || target.section != Section::Tree) return FocusAction::None;
    const std::vector<TreeRow>& rows = Tree();
    const TreeRow& row = rows[static_cast<std::size_t>(target.index)];
    if (row.expandable && !IsCollapsed(row.path)) {
      SetCollapsed(row.path, true);
      return FocusAction::Toggled;
    }
    if (row.depth == 0) return FocusAction::None;  // the archive root has no parent
    const std::string parent = prism::tree::UpOne(row.path);
    const int parentRow = RowIndexOf(parent);
    if (parentRow < 0) return FocusAction::None;
    focus_ = OrdinalOf(FocusTarget{Section::Tree, parentRow});
    return FocusAction::Moved;
  }

  FocusAction MoveRight() {
    const FocusTarget target = Focus();
    if (!target.Valid() || target.section != Section::Tree) return FocusAction::None;
    const std::vector<TreeRow>& rows = Tree();
    const TreeRow& row = rows[static_cast<std::size_t>(target.index)];
    if (!row.expandable) return FocusAction::None;
    if (IsCollapsed(row.path)) {
      SetCollapsed(row.path, false);
      return FocusAction::Toggled;
    }
    const std::size_t child = static_cast<std::size_t>(target.index) + 1;
    if (child < rows.size() && rows[child].depth == row.depth + 1) {
      focus_ = OrdinalOf(FocusTarget{Section::Tree, static_cast<int>(child)});
      return FocusAction::Moved;
    }
    return FocusAction::None;
  }

  // Enter / double-click on the focused row.
  FocusAction Activate() {
    const FocusTarget target = Focus();
    if (!target.Valid()) return FocusAction::None;
    activatedPath_ = PathOf(target);
    return FocusAction::Activated;
  }

  std::string FocusedPath() const { return PathOf(Focus()); }

  const std::string& ActivatedPath() const { return activatedPath_; }

  // --- persistence (injected port; C14 owns the implementation) -----------
  std::string Serialize() const {
    std::string out;
    out += kSerializationMagic;
    out += '\n';
    for (const Favorite& favorite : favorites_) {
      out += "F\t";
      out += detail::EscapeField(favorite.icon);
      out += '\t';
      out += detail::EscapeField(favorite.label);
      out += '\t';
      out += detail::EscapeField(favorite.path);
      out += '\n';
    }
    for (const RecentEntry& entry : recent_) {
      out += "R\t";
      out += std::to_string(entry.openedAt);
      out += '\t';
      out += detail::EscapeField(entry.path);
      out += '\n';
    }
    for (const std::string& folder : CollapsedFolders()) {
      out += "C\t";
      out += folder.empty() ? "-" : detail::EscapeField(folder);
      out += '\n';
    }
    return out;
  }

  // Tolerant by design: a settings blob written by a future/older build must
  // never wipe the user's sidebar. Only the magic line is fatal.
  bool Deserialize(std::string_view text) {
    bool magicSeen = false;
    bool first = true;
    std::vector<Favorite> favorites;
    std::vector<RecentEntry> recent;
    std::unordered_set<std::string> collapsed;
    std::size_t pos = 0;
    while (pos <= text.size()) {
      const std::size_t newline = text.find('\n', pos);
      std::string_view line = (newline == std::string_view::npos)
                                  ? text.substr(pos)
                                  : text.substr(pos, newline - pos);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      if (first) {
        first = false;
        if (line != kSerializationMagic) return false;
        magicSeen = true;
      } else if (!line.empty()) {
        const std::vector<std::string> parts = detail::SplitTabs(line);
        if (parts[0] == "F" && parts.size() >= 4) {
          favorites.push_back(Favorite{detail::UnescapeField(parts[2]),
                                       detail::UnescapeField(parts[3]),
                                       detail::UnescapeField(parts[1])});
        } else if (parts[0] == "R" && parts.size() >= 3) {
          std::int64_t openedAt = 0;
          if (detail::ParseInt64(parts[1], &openedAt)) {
            recent.push_back(RecentEntry{detail::UnescapeField(parts[2]), openedAt});
          }
        } else if (parts[0] == "C" && parts.size() >= 2) {
          const std::string field = detail::UnescapeField(parts[1]);
          collapsed.insert(field == "-" ? std::string()
                                        : prism::NormalizeArchivePath(field));
        }
        // Unknown record types are skipped on purpose (forward compatibility).
      }
      if (newline == std::string_view::npos) break;
      pos = newline + 1;
    }
    if (!magicSeen) return false;

    const FocusSnapshot snapshot = Snapshot();
    favorites_ = std::move(favorites);
    recent_ = std::move(recent);
    SortAndCapRecent();
    collapsed_.clear();
    for (const std::string& folder : collapsed) {
      if (folders_.find(folder) != folders_.end()) collapsed_.insert(folder);
    }
    rowsDirty_ = true;
    Tree();
    RestoreFocus(snapshot);
    return true;
  }

  void Save(SidebarStore* store) const {
    if (store == nullptr) return;
    store->Store(kSettingsKey, Serialize());
  }

  bool Restore(const SidebarStore* store) {
    if (store == nullptr) return false;
    const std::string blob = store->Load(kSettingsKey);
    if (blob.empty()) return false;
    return Deserialize(blob);
  }

 private:
  struct FolderIndex {
    std::string path;
    std::uint32_t index = prism::tree::kSyntheticIndex;  // catalog index when explicit
    bool explicitDir = false;
    std::vector<std::string> children;  // child FOLDER paths, sorted after the build
  };

  struct FocusSnapshot {
    Section section = Section::None;
    std::string path;
    int index = -1;
  };

  static int ClampOrdinal(int ordinal, int count) {
    if (count <= 0) return 0;
    if (ordinal < 0) return 0;
    if (ordinal >= count) return count - 1;
    return ordinal;
  }

  void SortAndCapRecent() {
    std::stable_sort(recent_.begin(), recent_.end(),
                     [](const RecentEntry& a, const RecentEntry& b) {
                       return a.openedAt > b.openedAt;
                     });
    if (kRecentCapEnabled && recent_.size() > kRecentCapacity) {
      recent_.resize(kRecentCapacity);
    }
  }

  // One pass over the catalog builds the folder adjacency. Paths are reduced
  // exactly like tree::Build does: NormalizeArchivePath, pseudo segments
  // ('.'/'..') terminate the item and every prefix before them stays a folder
  // -- which is what a Navigator walk shows for the same hostile entry.
  void BuildFolderIndex() {
    folders_.clear();
    folders_.reserve(catalog_ == nullptr ? 1 : (catalog_->size() / 4) + 1);
    FolderIndex root;
    root.path.clear();
    folders_.emplace(std::string(), std::move(root));
    if (catalog_ != nullptr) {
      for (const ArchiveItem& item : *catalog_) {
        const std::string path = prism::NormalizeArchivePath(item.path);
        if (path.empty()) continue;
        const std::vector<std::string> segments = prism::SplitPath(path);
        std::string prefix;
        for (std::size_t i = 0; i < segments.size(); ++i) {
          const std::string& segment = segments[i];
          if (segment == "." || segment == "..") break;  // pseudo entry: drop the rest
          const bool leaf = (i + 1 == segments.size());
          const std::string childPath =
              prefix.empty() ? segment : prefix + "/" + segment;
          if (leaf && !item.isDir) break;  // a file only contributes its ancestors
          EnsureFolder(prefix, childPath,
                       leaf ? item.index : prism::tree::kSyntheticIndex, leaf);
          prefix = childPath;
        }
      }
    }
    for (auto& entry : folders_) {
      std::vector<std::string>& children = entry.second.children;
      std::sort(children.begin(), children.end(),
                [this](const std::string& a, const std::string& b) {
                  // The SAME comparator tree::Build sorts with, so the sidebar
                  // and the content list can never disagree on sibling order.
                  const int cmp = prism::tree::detail::CompareNatural(a, b);
                  if (cmp != 0) return cmp < 0;
                  return FolderIndexOf(a) < FolderIndexOf(b);
                });
    }
  }

  void EnsureFolder(const std::string& parent, const std::string& path,
                    std::uint32_t index, bool explicitDir) {
    auto it = folders_.find(path);
    if (it == folders_.end()) {
      FolderIndex node;
      node.path = path;
      it = folders_.emplace(path, std::move(node)).first;
      auto parentIt = folders_.find(parent);
      if (parentIt != folders_.end()) parentIt->second.children.push_back(path);
    }
    if (explicitDir && !it->second.explicitDir) {
      it->second.explicitDir = true;  // first explicit node wins (tree::Build too)
      it->second.index = index;
    }
  }

  std::uint32_t FolderIndexOf(std::string_view path) const {
    const auto it = folders_.find(std::string(path));
    return it == folders_.end() ? prism::tree::kSyntheticIndex : it->second.index;
  }

  void Flatten() const {
    rows_.clear();
    rowIndex_.clear();
    if (catalog_ == nullptr) return;
    TreeRow root;
    root.path.clear();
    root.name = archiveName_.empty() ? std::string("ARCHIVE") : archiveName_;
    root.depth = 0;
    root.isRoot = true;
    const auto rootIt = folders_.find(std::string());
    root.expandable = rootIt != folders_.end() && !rootIt->second.children.empty();
    root.expanded = root.expandable && !IsCollapsed(std::string());
    AppendRow(root);
    if (root.expanded) AppendChildren(std::string(), 1);
  }

  void AppendChildren(const std::string& parent, int depth) const {
    if (depth > kMaxTreeDepth) return;
    const auto parentIt = folders_.find(parent);
    if (parentIt == folders_.end()) return;
    for (const std::string& childPath : parentIt->second.children) {
      const auto childIt = folders_.find(childPath);
      if (childIt == folders_.end()) continue;
      TreeRow row;
      row.path = childPath;
      row.name = std::string(prism::FileNameOf(childPath));
      row.depth = depth;
      row.catalogIndex = childIt->second.index;
      row.explicitDir = childIt->second.explicitDir;
      row.expandable = !childIt->second.children.empty();
      row.expanded = row.expandable && !IsCollapsed(childPath);
      AppendRow(row);
      if (row.expanded) AppendChildren(childPath, depth + 1);
    }
  }

  void AppendRow(const TreeRow& row) const {
    rowIndex_.emplace(row.path, static_cast<int>(rows_.size()));
    rows_.push_back(row);
  }

  int RowIndexOf(std::string_view path) const {
    Tree();
    const auto it = rowIndex_.find(std::string(path));
    return it == rowIndex_.end() ? -1 : it->second;
  }

  int OrdinalOf(FocusTarget target) const {
    const std::vector<FocusTarget> order = FocusOrder();
    for (std::size_t i = 0; i < order.size(); ++i) {
      if (order[i] == target) return static_cast<int>(i);
    }
    return -1;
  }

  void ClampFocus() {
    const std::vector<FocusTarget> order = FocusOrder();
    focus_ = ClampOrdinal(focus_, static_cast<int>(order.size()));
  }

  FocusSnapshot Snapshot() const {
    const FocusTarget target = Focus();
    FocusSnapshot snapshot;
    snapshot.section = target.section;
    snapshot.index = target.index;
    snapshot.path = PathOf(target);
    return snapshot;
  }

  void RestoreFocus(const FocusSnapshot& snapshot) {
    const std::vector<FocusTarget> order = FocusOrder();
    if (order.empty()) {
      focus_ = 0;
      return;
    }
    int ordinal = -1;
    if (snapshot.section == Section::Favorites) {
      int index = -1;
      for (std::size_t i = 0; i < favorites_.size(); ++i) {
        if (detail::RecentKey(favorites_[i].path) == detail::RecentKey(snapshot.path)) {
          index = static_cast<int>(i);
          break;
        }
      }
      if (index < 0 && !favorites_.empty()) {
        index = ClampOrdinal(snapshot.index, static_cast<int>(favorites_.size()));
      }
      if (index >= 0) ordinal = index;
    } else if (snapshot.section == Section::Recent) {
      int index = -1;
      for (std::size_t i = 0; i < recent_.size(); ++i) {
        if (detail::RecentKey(recent_[i].path) == detail::RecentKey(snapshot.path)) {
          index = static_cast<int>(i);
          break;
        }
      }
      if (index < 0 && !recent_.empty()) {
        index = ClampOrdinal(snapshot.index, static_cast<int>(recent_.size()));
      }
      if (index >= 0) ordinal = static_cast<int>(favorites_.size()) + index;
    } else if (snapshot.section == Section::Tree) {
      const int row = RowIndexOf(snapshot.path);
      if (row >= 0) {
        ordinal = static_cast<int>(favorites_.size() + recent_.size()) + row;
      }
    }
    if (ordinal < 0) ordinal = focus_;
    focus_ = ClampOrdinal(ordinal, static_cast<int>(order.size()));
  }

  FocusAction JumpToEdge(bool first) {
    const std::vector<FocusTarget> order = FocusOrder();
    if (order.empty()) return FocusAction::None;
    const int next = first ? 0 : static_cast<int>(order.size()) - 1;
    if (next == focus_) return FocusAction::None;
    focus_ = next;
    return FocusAction::Moved;
  }

  FocusAction MoveBy(int delta) {
    const std::vector<FocusTarget> order = FocusOrder();
    if (order.empty()) return FocusAction::None;
    const int count = static_cast<int>(order.size());
    int next = focus_ + delta;
    if (kFocusWrapEnabled) {
      next = ((next % count) + count) % count;
    } else {
      next = ClampOrdinal(next, count);
    }
    if (next == focus_) return FocusAction::None;
    focus_ = next;
    return FocusAction::Moved;
  }

  std::string PathOf(const FocusTarget& target) const {
    if (!target.Valid()) return std::string();
    switch (target.section) {
      case Section::Favorites:
        return target.index < static_cast<int>(favorites_.size())
                   ? favorites_[static_cast<std::size_t>(target.index)].path
                   : std::string();
      case Section::Recent:
        return target.index < static_cast<int>(recent_.size())
                   ? recent_[static_cast<std::size_t>(target.index)].path
                   : std::string();
      case Section::Tree: {
        const std::vector<TreeRow>& rows = Tree();
        return target.index < static_cast<int>(rows.size())
                   ? rows[static_cast<std::size_t>(target.index)].path
                   : std::string();
      }
      case Section::None:
        break;
    }
    return std::string();
  }

  std::vector<std::string> CollapsedFolders() const {
    std::vector<std::string> out(collapsed_.begin(), collapsed_.end());
    std::sort(out.begin(), out.end());  // deterministic blob
    return out;
  }

  const std::vector<ArchiveItem>* catalog_ = nullptr;
  std::string archiveName_ = "ARCHIVE";
  ArchiveInfo info_;
  std::vector<Favorite> favorites_;
  std::vector<RecentEntry> recent_;
  std::unordered_set<std::string> collapsed_;
  std::unordered_map<std::string, FolderIndex> folders_;
  mutable std::vector<TreeRow> rows_;
  mutable std::unordered_map<std::string, int> rowIndex_;
  mutable bool rowsDirty_ = true;
  int focus_ = 0;
  std::string activatedPath_;
};

}  // namespace prism::sidebar
