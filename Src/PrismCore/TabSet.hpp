// PrismArchive -- PrismCore (card C59)
// THE TAB SET: several archives open at once.
//
// Today the window holds ONE archive. C46's session remembers up to eight
// archives, but only the ACTIVE one is restored, so "reopen what I was doing"
// means one tab, not the ten the user had. The blueprint wants the Bandizip
// shape: one window, a strip of tabs, every tab its own place inside its own
// archive.
//
// This header is the MODEL layer and nothing else -- no window, no pixels, no
// file system. The card that draws the strip must not have to decide ANY of the
// rules below; it only paints what this object already answered:
//
//   * which archives are open, in tab-bar order, and which one is active;
//   * one tab per PATH, compared on the NORMALIZED path, case-insensitively
//     (Windows: "C:\a.7Z" and "c:/A.7z" are one archive, one tab);
//   * open/close/activate/reorder, each returning a VERDICT with a reason --
//     nothing is silently dropped, nothing is silently ignored;
//   * closing the ACTIVE tab activates its RIGHT neighbour (the tab that slides
//     into its place), or the LEFT one when there is no right neighbour; closing
//     the last tab is an EMPTY STATE, never an exit;
//   * a DIRTY tab answers "needs confirmation" instead of closing itself;
//   * the projection onto C46's session document, round-tripping exactly, with
//     the MRU order the session has always used (active archive first).
//
// WHY A SEPARATE MRU LIST: the tab bar is reorderable (the user drags tabs) and
// the session is a recency list. Those are two different orders over the same
// tabs, so both are stored: tabs_ is the bar, mru_ is the recency. The active
// tab is always at the front of mru_, which is what makes the projection's
// "active first" a property of the model instead of a rule the UI must remember.
//
// WHAT THIS LAYER DOES NOT KNOW: whether the file is on disk. PrismCore never
// touches the disk (it must compile and be tested on Linux), so OpenRequest
// carries the caller's answer as data -- one bool, injected, so the refusal is
// assertable without a filesystem. The FORMAT question, by contrast, is pure and
// is answered here by reusing C52's classifier (drop::Classify): a second
// extension table in the tree would drift from the drop path and from the
// context menu, and then one of them would advertise a tab the other refuses.
//
// Contracts touched: D1 (stdout marker is part of the verdict), D3 (negative
// controls must be able to go red for the INJECTED reason), B1/B2 (header
// hygiene: no unparenthesized std::min/max, <cstdint> included here), D26/D27
// (mutant marker is a conjunction, printed at the first failure), D28 (arm names
// avoid clean/unarmed/control), D31 (g++ -Werror clean).
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "DropPlan.hpp"      // C52: the ONE archive classifier (no third table)
#include "SessionStore.hpp"  // C46: the session document this projects onto

namespace prism::tabs {

// ---------------------------------------------------------------------------
// 0. The two D3 negative controls
// ---------------------------------------------------------------------------
// Each is compiled into its own binary from the same source (see
// Tests/suites/c59-tabs.cmake), and each turns ONE rule off:
//
//   PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR  closing the active tab always
//                                          activates tab 0 ("just pick the first")
//   PRISM_C59_MUTANT_DEDUP_OPEN            reopening an open archive appends a
//                                          SECOND tab for the same path
//
// They are constexpr VALUES rather than #ifdef islands inside the algorithms so
// that the clean build still type-checks the mutated statement -- an injected
// branch that no longer compiles proves nothing about the test.
namespace mutation {
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR)
inline constexpr bool kCloseActivatesFirstTab = true;
#else
inline constexpr bool kCloseActivatesFirstTab = false;
#endif
#if defined(PRISM_C59_MUTANT_DEDUP_OPEN)
inline constexpr bool kDuplicateOpensNewTab = true;
#else
inline constexpr bool kDuplicateOpensNewTab = false;
#endif
}  // namespace mutation

// ---------------------------------------------------------------------------
// 1. Limits and the tab document
// ---------------------------------------------------------------------------

// The card's ceiling. A seventeenth archive is not a window, it is a browser.
inline constexpr std::size_t kMaxTabs = 16;

// What a tab remembers about "where the user is". It is owned by the tab, so the
// model can answer "what would the session say" without the UI being present.
struct ViewState {
  std::string folder;      // folder INSIDE the archive ("" == root, C46 convention)
  bool listMode = false;   // false == grid view
};

struct Tab {
  std::string path;         // normalized, '/' separators, original case kept
  std::string displayName;  // tab-strip text (the file name)
  ViewState view;           // where the user is inside this archive
  bool dirty = false;       // unsaved changes: closing asks first
  // Stable identity of this tab for the UI's own per-tab state (scroll offset,
  // selection, thumbnails). A reorder keeps it; closing and reopening mints a
  // NEW id, so a stale binding can never be served for a fresh archive.
  std::uint64_t viewId = 0;
};

// ---------------------------------------------------------------------------
// 2. Verdicts: every refusal has a name and a reason
// ---------------------------------------------------------------------------
// A silent "nothing happened" is undebuggable and indistinguishable from a
// broken menu item (the rule C5 pinned for the drag-out transport).
enum class TabVerdict : std::uint8_t {
  kOk = 0,
  kEmptyPath,          // nothing to open
  kMissingFile,        // the caller says the path does not resolve
  kUnsupportedFormat,  // not an archive this product opens
  kLimitReached,       // kMaxTabs already open (refused, never silently dropped)
  kNoSuchTab,          // index out of range / already empty
  kNeedsConfirm,       // the tab is dirty: ask the user, then retry confirmed
};

inline const char* TabVerdictName(TabVerdict verdict) {
  switch (verdict) {
    case TabVerdict::kOk: return "ok";
    case TabVerdict::kEmptyPath: return "empty-path";
    case TabVerdict::kMissingFile: return "missing-file";
    case TabVerdict::kUnsupportedFormat: return "unsupported-format";
    case TabVerdict::kLimitReached: return "limit-reached";
    case TabVerdict::kNoSuchTab: return "no-such-tab";
    case TabVerdict::kNeedsConfirm: return "needs-confirm";
  }
  return "?";
}

// preflight B6: an enum compared at the top level of PRISM_CHECK_EQ is printed
// on failure, so it needs a stream operator next to its name.
inline std::ostream& operator<<(std::ostream& os, TabVerdict verdict) {
  return os << TabVerdictName(verdict);
}

// ---------------------------------------------------------------------------
// 3. Path identity: ONE archive == ONE tab
// ---------------------------------------------------------------------------
// Normalization is C46's (SessionStore::NormalizePath): '\' -> '/', collapse
// duplicate separators, drop a trailing separator (a drive root survives), trim
// blanks. Reusing it is deliberate -- the tab's identity and the session's
// de-duplication must agree, or the same archive becomes two tabs after a
// restart.

inline std::string ToLowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

inline std::string CanonicalPath(std::string_view raw) {
  return session::NormalizePath(raw);
}

// The equality key. UTF-8 bytes above ASCII pass through untouched: lowercasing
// them would corrupt the path (the same rule C46's path handling follows).
inline std::string PathKey(std::string_view raw) {
  return ToLowerAscii(CanonicalPath(raw));
}

// Format support is C52's classifier, not a new table. A path the drop path
// would not treat as an archive cannot become a tab either.
inline bool IsSupportedArchivePath(std::string_view path) {
  return drop::Classify(path) != drop::ArchiveKind::NotArchive;
}

inline std::string DisplayNameFor(std::string_view normalizedPath) {
  const std::string_view name = FileNameOf(normalizedPath);
  return name.empty() ? std::string(normalizedPath) : std::string(name);
}

// ---------------------------------------------------------------------------
// 4. Requests and outcomes
// ---------------------------------------------------------------------------

struct OpenRequest {
  std::string path;         // any separators/case; normalized here
  // The caller's answer to "does this resolve?" -- PrismCore cannot stat a file.
  // Default true so a UI that already resolved the path does not repeat itself.
  bool exists = true;
  std::string displayName;  // optional override; the file name when empty
  ViewState view;           // where to land (the caller's starting folder)
};

struct OpenOutcome {
  TabVerdict verdict = TabVerdict::kOk;
  int index = -1;                 // where the tab is (the EXISTING one when deduped)
  bool created = false;           // a new tab was appended
  bool activatedExisting = false; // the path was already open: activated, not added
  std::string path;               // the normalized path the verdict is about
  std::string reason;             // human-readable; never empty on a refusal
};

struct CloseOutcome {
  TabVerdict verdict = TabVerdict::kOk;
  bool closed = false;
  bool needsConfirm = false;   // dirty tab: nothing changed, ask the user
  int index = -1;              // the tab that was closed (-1 when refused)
  int activeIndex = -1;        // active tab AFTER the close; -1 == empty state
  bool emptyState = false;     // the last tab closed: window stays open, empty
  std::string reason;
};

// What C46's session can hold of this tab set. Entries are MRU-ordered with the
// active archive FIRST, which is the order SessionStore::RecordOpen() produces;
// the tabs that do not fit the session ceiling are COUNTED, never silently lost.
struct SessionProjection {
  std::vector<session::SessionEntry> entries;  // MRU, active first
  int active = -1;                             // 0 when non-empty
  std::size_t dropped = 0;                     // beyond session::kMaxEntries
  bool lossy() const { return dropped != 0; }
};

// What a restore had to throw away, and why. Restoring a session document is
// data-in/data-out: a hostile or hand-edited document loses entries, and the
// loss is reported instead of becoming a mystery missing tab.
struct RestoreReport {
  std::size_t restored = 0;
  std::size_t droppedBlank = 0;        // empty path
  std::size_t droppedDuplicate = 0;    // same archive twice (one tab per path)
  std::size_t droppedUnsupported = 0;  // not an archive this product opens
  std::size_t droppedOverLimit = 0;    // beyond kMaxTabs
  std::vector<std::string> warnings;   // one line each, for the C16 logger
};

// ---------------------------------------------------------------------------
// 5. The tab set
// ---------------------------------------------------------------------------

class TabSet {
 public:
  TabSet() = default;

  // --- read-only surface the strip paints from ---------------------------
  std::size_t size() const { return tabs_.size(); }
  bool empty() const { return tabs_.empty(); }
  const std::vector<Tab>& tabs() const { return tabs_; }

  int activeIndex() const { return active_; }  // -1 == empty state
  const Tab* active() const { return At(active_); }
  const Tab* At(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return nullptr;
    return &tabs_[static_cast<std::size_t>(index)];
  }

  // Case-insensitive, separator-insensitive; -1 when no tab owns that path.
  int IndexOf(std::string_view path) const {
    const std::string key = PathKey(path);
    if (key.empty()) return -1;
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
      if (PathKey(tabs_[i].path) == key) return static_cast<int>(i);
    }
    return -1;
  }

  // --- open ---------------------------------------------------------------
  OpenOutcome Open(const OpenRequest& request) {
    OpenOutcome outcome;
    const std::string path = CanonicalPath(request.path);
    outcome.path = path;
    if (path.empty()) {
      outcome.verdict = TabVerdict::kEmptyPath;
      outcome.reason = "the archive path is empty";
      return outcome;
    }

    // De-duplication FIRST: "open" on an archive that is already open must not
    // cost the user their place in it, so the existing tab is activated and its
    // view state is left exactly where it was.
    const int existing = IndexOf(path);
    if (existing >= 0 && !mutation::kDuplicateOpensNewTab) {
      Activate(existing);
      outcome.index = existing;
      outcome.activatedExisting = true;
      outcome.reason = "already open; activated the existing tab";
      return outcome;
    }

    if (!request.exists) {
      outcome.verdict = TabVerdict::kMissingFile;
      outcome.reason = "no such file: " + path;
      return outcome;
    }
    if (!IsSupportedArchivePath(path)) {
      const std::string ext = ExtensionOf(path);
      outcome.verdict = TabVerdict::kUnsupportedFormat;
      outcome.reason = "unsupported archive format '" + (ext.empty() ? path : ext) +
                       "' -- only archives the engine can list get a tab";
      return outcome;
    }
    if (tabs_.size() >= kMaxTabs) {
      // Refused, with the reason naming the ceiling: silently dropping the
      // seventeenth archive is how a user loses a file they thought was open.
      outcome.verdict = TabVerdict::kLimitReached;
      outcome.reason = "all " + std::to_string(kMaxTabs) +
                       " tabs are in use; close one before opening " + DisplayNameFor(path);
      return outcome;
    }

    Tab tab;
    tab.path = path;
    tab.displayName = request.displayName.empty() ? DisplayNameFor(path) : request.displayName;
    tab.view.folder = session::NormalizeFolder(request.view.folder);
    tab.view.listMode = request.view.listMode;
    tab.viewId = nextViewId_++;
    tabs_.push_back(std::move(tab));
    const int index = static_cast<int>(tabs_.size()) - 1;
    active_ = index;
    Touch(tabs_[static_cast<std::size_t>(index)].viewId);

    outcome.index = index;
    outcome.created = true;
    outcome.reason = "opened " + tabs_[static_cast<std::size_t>(index)].displayName;
    return outcome;
  }

  OpenOutcome Open(std::string_view path) {
    OpenRequest request;
    request.path = std::string(path);
    return Open(request);
  }

  // --- close --------------------------------------------------------------
  // confirmed == false is the default: a dirty tab never disappears because a
  // stray middle-click asked. The caller shows the prompt and retries with true.
  CloseOutcome Close(int index, bool confirmed = false) {
    CloseOutcome outcome;
    outcome.index = index;
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) {
      outcome.verdict = TabVerdict::kNoSuchTab;
      outcome.index = -1;
      outcome.activeIndex = active_;
      outcome.reason = "no tab at index " + std::to_string(index);
      return outcome;
    }
    const Tab& closing = tabs_[static_cast<std::size_t>(index)];
    if (closing.dirty && !confirmed) {
      outcome.verdict = TabVerdict::kNeedsConfirm;
      outcome.needsConfirm = true;
      outcome.activeIndex = active_;
      outcome.reason = "'" + closing.displayName + "' has unsaved changes";
      return outcome;
    }

    const std::uint64_t closingId = closing.viewId;
    const bool closingActive = (index == active_);
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(index));
    ForgetFromMru(closingId);
    outcome.closed = true;

    if (tabs_.empty()) {
      // "No tabs" is a STATE, not a shutdown: the window keeps its frame and the
      // empty-state page (C46 restores from here on the next launch).
      active_ = -1;
      outcome.emptyState = true;
      outcome.activeIndex = -1;
      outcome.reason = "the last tab was closed; the window is empty";
      return outcome;
    }

    if (closingActive) {
      // The right neighbour slides into the closed tab's place -- so it IS the
      // tab now at 'index'. With no right neighbour, the left one is last.
      const int lastAfter = static_cast<int>(tabs_.size()) - 1;
      const int neighbour = (index <= lastAfter) ? index : lastAfter;
      active_ = mutation::kCloseActivatesFirstTab ? 0 : neighbour;
      Touch(tabs_[static_cast<std::size_t>(active_)].viewId);
    } else if (index < active_) {
      --active_;  // the active tab moved left by one; it is still the same tab
    }
    outcome.activeIndex = active_;
    outcome.reason = "closed tab " + std::to_string(index);
    return outcome;
  }

  CloseOutcome CloseActive(bool confirmed = false) { return Close(active_, confirmed); }

  // --- activate / reorder -------------------------------------------------
  bool Activate(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return false;
    active_ = index;
    Touch(tabs_[static_cast<std::size_t>(index)].viewId);
    return true;
  }

  bool ActivatePath(std::string_view path) {
    const int index = IndexOf(path);
    return index >= 0 && Activate(index);
  }

  // Tab-bar reorder (drag and drop). The ACTIVE tab stays the same tab, its
  // viewId -- and therefore the UI's binding to it -- survives, and the MRU
  // order does not change: where a tab SITS is not when it was last used.
  bool Move(int from, int to) {
    if (from < 0 || to < 0) return false;
    if (static_cast<std::size_t>(from) >= tabs_.size()) return false;
    if (static_cast<std::size_t>(to) >= tabs_.size()) return false;
    if (from == to) return false;
    Tab moved = tabs_[static_cast<std::size_t>(from)];
    tabs_.erase(tabs_.begin() + static_cast<std::ptrdiff_t>(from));
    tabs_.insert(tabs_.begin() + static_cast<std::ptrdiff_t>(to), std::move(moved));
    if (active_ == from) {
      active_ = to;
    } else if (from < active_ && to >= active_) {
      --active_;
    } else if (from > active_ && to <= active_) {
      ++active_;
    }
    return true;
  }

  // --- per-tab state ------------------------------------------------------
  bool SetViewState(int index, std::string_view folder, bool listMode) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return false;
    Tab& tab = tabs_[static_cast<std::size_t>(index)];
    tab.view.folder = session::NormalizeFolder(folder);
    tab.view.listMode = listMode;
    return true;
  }

  bool SetDirty(int index, bool dirty) {
    if (index < 0 || static_cast<std::size_t>(index) >= tabs_.size()) return false;
    tabs_[static_cast<std::size_t>(index)].dirty = dirty;
    return true;
  }

  std::size_t DirtyCount() const {
    std::size_t count = 0;
    for (const Tab& tab : tabs_) {
      if (tab.dirty) ++count;
    }
    return count;
  }

  void Clear() {
    tabs_.clear();
    mru_.clear();
    active_ = -1;
  }

  // The recency order, active first. Exposed because the session projection is
  // built from it and the UI shows it in its "reopen" menu.
  std::vector<std::string> MruPaths() const {
    std::vector<std::string> paths;
    paths.reserve(mru_.size());
    for (const std::uint64_t id : mru_) {
      const int index = IndexOfViewId(id);
      if (index >= 0) paths.push_back(tabs_[static_cast<std::size_t>(index)].path);
    }
    return paths;
  }

  // --- C46 session projection --------------------------------------------
  // Order: MRU, active archive first -- exactly what SessionStore::RecordOpen()
  // writes. The projection is clipped to the SESSION's ceiling (8), and the tabs
  // that do not fit are counted in 'dropped': the active tab is first, so the
  // archive the user was looking at always survives the clip.
  SessionProjection ToSession() const {
    SessionProjection projection;
    std::vector<std::uint64_t> order;
    order.reserve(tabs_.size());
    for (const std::uint64_t id : mru_) {
      if (IndexOfViewId(id) >= 0) order.push_back(id);
    }
    // Defensive: a tab added by any future path that forgets to Touch() still
    // reaches the document -- appended in bar order instead of vanishing.
    for (const Tab& tab : tabs_) {
      bool listed = false;
      for (const std::uint64_t id : order) {
        if (id == tab.viewId) {
          listed = true;
          break;
        }
      }
      if (!listed) order.push_back(tab.viewId);
    }

    for (const std::uint64_t id : order) {
      const int index = IndexOfViewId(id);
      if (index < 0) continue;
      const Tab& tab = tabs_[static_cast<std::size_t>(index)];
      if (projection.entries.size() >= session::kMaxEntries) {
        ++projection.dropped;
        continue;
      }
      session::SessionEntry entry;
      entry.path = tab.path;
      entry.folder = session::NormalizeFolder(tab.view.folder);
      entry.listMode = tab.view.listMode;
      projection.entries.push_back(std::move(entry));
    }
    projection.active = projection.entries.empty() ? -1 : 0;
    return projection;
  }

  // Rebuild a tab set from a C46 document. The document is DATA: blank paths,
  // duplicates, non-archives and overflow are dropped and COUNTED
  // (RestoreReport), never half-applied and never a crash. 'activeIndex' is the
  // session's active entry; the resulting set honours it, and its MRU order is
  // that active entry followed by the rest in document order -- which is what
  // makes the round trip ToSession(FromSession(p)) == p hold for a canonical
  // projection.
  static TabSet FromSession(const std::vector<session::SessionEntry>& entries,
                            int activeIndex, RestoreReport* report = nullptr) {
    TabSet set;
    RestoreReport local;
    RestoreReport& out = (report != nullptr) ? *report : local;
    int wantedActive = -1;

    for (std::size_t i = 0; i < entries.size(); ++i) {
      const std::string path = CanonicalPath(entries[i].path);
      if (path.empty()) {
        ++out.droppedBlank;
        out.warnings.push_back("session entry " + std::to_string(i) +
                               " has an empty path; no tab restored");
        continue;
      }
      const int existing = set.IndexOf(path);
      if (existing >= 0) {
        ++out.droppedDuplicate;
        if (static_cast<int>(i) == activeIndex) wantedActive = existing;
        out.warnings.push_back("session lists " + path +
                               " twice; one tab (the first) was restored");
        continue;
      }
      if (!IsSupportedArchivePath(path)) {
        ++out.droppedUnsupported;
        out.warnings.push_back("session entry '" + path +
                               "' is not an archive this build opens; no tab restored");
        continue;
      }
      if (set.tabs_.size() >= kMaxTabs) {
        ++out.droppedOverLimit;
        out.warnings.push_back("session lists more than " + std::to_string(kMaxTabs) +
                               " archives; '" + path + "' was not restored");
        continue;
      }
      Tab tab;
      tab.path = path;
      tab.displayName = DisplayNameFor(path);
      tab.view.folder = session::NormalizeFolder(entries[i].folder);
      tab.view.listMode = entries[i].listMode;
      tab.viewId = set.nextViewId_++;
      if (static_cast<int>(i) == activeIndex) wantedActive = static_cast<int>(set.tabs_.size());
      set.tabs_.push_back(std::move(tab));
    }

    out.restored = set.tabs_.size();
    if (set.tabs_.empty()) {
      set.active_ = -1;
      return set;
    }
    set.active_ = (wantedActive >= 0) ? wantedActive : 0;
    set.Touch(set.tabs_[static_cast<std::size_t>(set.active_)].viewId);
    for (std::size_t i = 0; i < set.tabs_.size(); ++i) {
      if (static_cast<int>(i) != set.active_) set.mru_.push_back(set.tabs_[i].viewId);
    }
    return set;
  }

 private:
  int IndexOfViewId(std::uint64_t id) const {
    for (std::size_t i = 0; i < tabs_.size(); ++i) {
      if (tabs_[i].viewId == id) return static_cast<int>(i);
    }
    return -1;
  }

  // Most-recently-used bookkeeping: the touched tab moves to the front. Every
  // mutation keeps the invariant "the active tab is mru_.front()".
  void Touch(std::uint64_t id) {
    ForgetFromMru(id);
    mru_.insert(mru_.begin(), id);
  }

  void ForgetFromMru(std::uint64_t id) {
    for (std::size_t i = 0; i < mru_.size(); ++i) {
      if (mru_[i] == id) {
        mru_.erase(mru_.begin() + static_cast<std::ptrdiff_t>(i));
        return;
      }
    }
  }

  std::vector<Tab> tabs_;            // tab-bar order
  std::vector<std::uint64_t> mru_;   // viewIds, most recent first
  int active_ = -1;                  // index into tabs_, -1 == empty state
  std::uint64_t nextViewId_ = 1;     // 0 is reserved for "no tab"
};

}  // namespace prism::tabs
