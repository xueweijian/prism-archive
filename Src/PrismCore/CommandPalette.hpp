// PrismArchive -- PrismCore
// Command palette BRAIN (blueprint ch. 9.1 row 3): fuzzy search over the
// in-archive paths and the command set, with ranking decided here and only
// here. The Win32 shell in PrismWin/CommandPalette.hpp owns pixels, geometry
// and the text buffer; it never invents an order.
//
// Why the model lives in PrismCore: ranking is arithmetic, and this project
// iterates arithmetic in milliseconds on Linux (HANDOVER ch.4). A ranker that
// lives in WndProc can only be tested by a Windows CI round.
//
// Determinism is a contract (CONTRACTS C4): the same input yields the same
// order, bit for bit. Ties are resolved by an EXPLICIT chain -- score, then
// kind (commands before files), then shorter title, then byte-wise title, then
// catalog order -- so no sort implementation detail can leak into the UI.
//
// Scoring (documented because the tests assert exact orders):
//   * case-insensitive ASCII subsequence match (UTF-8 bytes pass through, so
//     CJK text matches when its byte sequence matches -- highlighting is
//     byte-based and the UI converts to text ranges);
//   * every matched character is worth kBase;
//   * +kBoundary when it sits at the start or right after a separator
//     (/ \ space . - _) -- "png" prefers "root.png" over "xapngy.bin";
//   * +kConsecutive when it directly follows the previous matched character;
//   * -kRunRestart when the match resumes after a gap;
//   * +kPrefixBonus when the whole title starts with the query.
// The best alignment is computed by an O(query x target) DP, so a 50k-entry
// archive does not turn a keystroke into a scan nightmare.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"
#include "Shortcuts.hpp"

namespace prism::palette {

inline constexpr uint32_t kNoIndex = 0xFFFFFFFFu;

enum class Kind : uint8_t { Command, File };

inline std::ostream& operator<<(std::ostream& os, Kind kind) {
  switch (kind) {
    case Kind::Command: return os << "Command";
    case Kind::File: return os << "File";
  }
  return os << "?";
}

struct Entry {
  Kind kind = Kind::File;
  std::string title;          // archive-relative path, or the command's label
  std::string subtitle;       // hint (shortcut) for commands, size/kind for files
  uint32_t index = kNoIndex;  // catalog index (files); kNoIndex for commands
  prism::keys::Command command = prism::keys::Command::None;  // commands only
};

// The command set the palette can execute. Labels are the searchable text.
struct CommandSpec {
  prism::keys::Command command = prism::keys::Command::None;
  const char* label = "";
  const char* hint = "";
};

inline const std::vector<CommandSpec>& DefaultCommands() {
  static const std::vector<CommandSpec> commands = {
      {prism::keys::Command::SmartExtract, "Extract Smart", "Ctrl+E"},
      {prism::keys::Command::NewFolder, "New Folder", "Ctrl+Shift+N"},
      {prism::keys::Command::ToggleGrid, "Grid View", "Ctrl+1"},
      {prism::keys::Command::ToggleList, "List View", "Ctrl+2"},
      {prism::keys::Command::ZoomNext, "Zoom Next", "Ctrl+Shift+P"},
      {prism::keys::Command::HistoryBack, "Back", "Alt+Left"},
      {prism::keys::Command::HistoryForward, "Forward", "Alt+Right"},
      {prism::keys::Command::QuickLookToggle, "Quick Look", "Space"},
  };
  return commands;
}

namespace detail {

inline constexpr int kNoMatch = -1;
inline constexpr int kBase = 10;
inline constexpr int kBoundary = 20;
inline constexpr int kConsecutive = 30;
inline constexpr int kRunRestart = 8;
inline constexpr int kPrefixBonus = 40;

inline char FoldAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline bool IsSeparator(char c) {
  return c == '/' || c == '\\' || c == ' ' || c == '.' || c == '-' || c == '_';
}

inline int BoundaryBonus(std::string_view target, size_t j) {
#ifdef PRISM_PALETTE_MUTANT_NO_BOUNDARY
  (void)target;
  (void)j;
  return 0;
#else
  return (j == 0 || IsSeparator(target[j - 1])) ? kBoundary : 0;
#endif
}

inline bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (FoldAscii(text[i]) != FoldAscii(prefix[i])) return false;
  }
  return true;
}

// Trim ASCII spaces/tabs (a palette query of spaces is an EMPTY query, which
// lists commands first -- see Model::Query).
inline std::string_view TrimAscii(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && (text[begin] == ' ' || text[begin] == '\t')) ++begin;
  while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
  return text.substr(begin, end - begin);
}

// One alignment: score, plus the matched byte offsets when kTrace is set.
// A single implementation serves both callers (Score for ranking every entry,
// AlignMatch for the visible page) so the two can never disagree.
template <bool kTrace>
int AlignImpl(std::string_view target, std::string_view query,
              std::vector<size_t>* outPositions) {
  if (outPositions) outPositions->clear();
  const size_t n = target.size();
  const size_t q = query.size();
  if (q == 0) return 0;
  if (q > n) return kNoMatch;

  std::vector<int> prev(n, kNoMatch);
  std::vector<int> cur(n, kNoMatch);
  std::vector<int> prefixMax(n, kNoMatch);
  std::vector<int> parent;
  if constexpr (kTrace) parent.assign(q * n, -1);

  // Row 0: every position where the first query character appears.
  const char q0 = FoldAscii(query[0]);
  for (size_t j = 0; j < n; ++j) {
    if (FoldAscii(target[j]) != q0) continue;
    prev[j] = kBase + BoundaryBonus(target, j);
  }

  for (size_t i = 1; i < q; ++i) {
    int running = kNoMatch;
    for (size_t k = 0; k < n; ++k) {
      running = (std::max)(running, prev[k]);
      prefixMax[k] = running;
    }
    std::fill(cur.begin(), cur.end(), kNoMatch);
    const char qc = FoldAscii(query[i]);
    for (size_t j = i; j < n; ++j) {
      if (FoldAscii(target[j]) != qc) continue;
      const int bonus = kBase + BoundaryBonus(target, j);
      int best = kNoMatch;
      // Only the tracing instantiation reads this; [[maybe_unused]] keeps the
      // ranking instantiation warning-free under gcc -Wall -Werror (the Linux
      // lane builds these headers with -Werror; MSVC has no such warning, so a
      // "passes locally" here would be exactly the CONTRACTS B3 trap).
      [[maybe_unused]] int parentPos = -1;
      if (prev[j - 1] != kNoMatch) {  // extend the run: strongest signal
        best = prev[j - 1] + bonus + kConsecutive;
        parentPos = static_cast<int>(j - 1);
      }
      if (j >= 2 && prefixMax[j - 2] != kNoMatch) {  // resume after a gap
        const int candidate = prefixMax[j - 2] + bonus - kRunRestart;
        if (candidate > best) {
          best = candidate;
          if constexpr (kTrace) {
            // Recover the position that produced the prefix max (needed for
            // the trace). O(n) only for entries whose trace we need.
            size_t k2 = j - 1;
            while (k2-- > 0) {
              if (prev[k2] == prefixMax[j - 2]) break;
            }
            parentPos = static_cast<int>(k2);
          }
        }
      }
      cur[j] = best;
      if constexpr (kTrace) parent[i * n + j] = parentPos;
    }
    prev.swap(cur);
  }

  int bestScore = kNoMatch;
  size_t bestEnd = 0;
  for (size_t j = 0; j < n; ++j) {
    if (prev[j] > bestScore) {  // strict: ties keep the EARLIEST end
      bestScore = prev[j];
      bestEnd = j;
    }
  }
  if (bestScore == kNoMatch) return kNoMatch;
  if (StartsWithIgnoreCase(target, query)) bestScore += kPrefixBonus;

  if constexpr (kTrace) {
    if (outPositions) {
      outPositions->assign(q, 0);
      size_t j = bestEnd;
      for (size_t i = q; i-- > 0;) {
        (*outPositions)[i] = j;
        if (i == 0) break;
        const int p = parent[i * n + j];
        if (p < 0) {  // defensive: never hand back a half alignment
          outPositions->clear();
          return bestScore;
        }
        j = static_cast<size_t>(p);
      }
    }
  }
  return bestScore;
}

inline std::string FormatBytes(uint64_t bytes) {
  static const char* kUnits[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = static_cast<double>(bytes);
  size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < 5) {
    value /= 1024.0;
    ++unit;
  }
  char buf[32];
  if (unit == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
  }
  return std::string(buf);
}

}  // namespace detail

// The ranked hit. Positions are byte-based and only filled for the entries the
// shell is about to draw (ranking uses Score, which allocates nothing).
struct Match {
  size_t entry = 0;
  int score = 0;
  std::vector<size_t> positions;
};

class Model {
 public:
  static constexpr size_t kDefaultLimit = 50;

  void SetCommands(const std::vector<CommandSpec>& commands) {
    commands_.clear();
    commands_.reserve(commands.size());
    for (const auto& spec : commands) {
      Entry entry;
      entry.kind = Kind::Command;
      entry.title = spec.label;
      entry.subtitle = spec.hint;
      entry.command = spec.command;
      commands_.push_back(std::move(entry));
    }
    Rebuild();
  }

  // Every catalog entry participates (blueprint: search DEEP files), with its
  // normalized archive path as the searchable text.
  void SetCatalog(const std::vector<ArchiveItem>& catalog) {
    files_.clear();
    files_.reserve(catalog.size());
    for (const auto& item : catalog) {
      const std::string path = prism::NormalizeArchivePath(item.path);
      if (path.empty()) continue;
      Entry entry;
      entry.kind = Kind::File;
      entry.title = path;
      entry.subtitle = item.isDir ? "folder" : detail::FormatBytes(item.size);
      entry.index = item.index;
      files_.push_back(std::move(entry));
    }
    Rebuild();
  }

  // Commands first (declaration order), then files (catalog order).
  const std::vector<Entry>& entries() const { return entries_; }
  const std::vector<Entry>& commands() const { return commands_; }
  const std::vector<Entry>& files() const { return files_; }

  // Empty (or all-space) query: the flat entry order, truncated to the limit.
  // Commands come first because that is what a user who opened the palette
  // without typing is most likely to want.
  std::vector<Match> Query(std::string_view query, size_t limit = kDefaultLimit) const {
    const std::string_view trimmed = detail::TrimAscii(query);
    std::vector<Match> results;
    if (trimmed.empty()) {
      const size_t count = (std::min)(limit, entries_.size());
      results.reserve(count);
      for (size_t i = 0; i < count; ++i) results.push_back(Match{i, 0, {}});
      return results;
    }
    struct Candidate {
      size_t entry = 0;
      int score = 0;
    };
    std::vector<Candidate> candidates;
    candidates.reserve(entries_.size());
    for (size_t i = 0; i < entries_.size(); ++i) {
      const int score = Score(entries_[i].title, trimmed);
      if (score == detail::kNoMatch) continue;
      candidates.push_back({i, score});
    }
    const auto better = [this](const Candidate& a, const Candidate& b) {
      if (a.score != b.score) return a.score > b.score;
      const Entry& ea = entries_[a.entry];
      const Entry& eb = entries_[b.entry];
      if (ea.kind != eb.kind) return ea.kind == Kind::Command;
#ifdef PRISM_PALETTE_MUTANT_LONGER_FIRST
      if (ea.title.size() != eb.title.size()) return ea.title.size() > eb.title.size();
#else
      if (ea.title.size() != eb.title.size()) return ea.title.size() < eb.title.size();
#endif
      if (ea.title != eb.title) return ea.title < eb.title;
      return a.entry < b.entry;  // total order: same input, same output (C4)
    };
    std::sort(candidates.begin(), candidates.end(), better);
    const size_t count = (std::min)(limit, candidates.size());
    results.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      Match match;
      match.entry = candidates[i].entry;
      match.score = candidates[i].score;
      // Positions are for DRAWING, so only the visible page pays for them.
      detail::AlignImpl<true>(entries_[match.entry].title, trimmed, &match.positions);
      results.push_back(std::move(match));
    }
    return results;
  }

  // Ranking score (no allocation). kNoMatch when the query is not a subsequence.
  static int Score(std::string_view target, std::string_view query) {
    return detail::AlignImpl<false>(target, detail::TrimAscii(query), nullptr);
  }

  // Same score plus the matched byte offsets (used by the shell for highlights).
  static Match AlignMatch(std::string_view target, std::string_view query) {
    Match match;
    const std::string_view trimmed = detail::TrimAscii(query);
    match.score = detail::AlignImpl<true>(target, trimmed, &match.positions);
    return match;
  }

  static bool Matches(std::string_view target, std::string_view query) {
    return Score(target, query) != detail::kNoMatch;
  }

 private:
  void Rebuild() {
    entries_.clear();
    entries_.reserve(commands_.size() + files_.size());
    for (const auto& command : commands_) entries_.push_back(command);
    for (const auto& file : files_) entries_.push_back(file);
  }

  std::vector<Entry> commands_;
  std::vector<Entry> files_;
  std::vector<Entry> entries_;
};

}  // namespace prism::palette
