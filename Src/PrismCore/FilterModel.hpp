// PrismArchive -- PrismCore
// FilterModel: the "显示过滤器" of the archive list (blueprint ch. 3.2 /
// Bandizip interaction): substring, glob, type class and size range, combined
// with AND, plus the selection bookkeeping that makes filtering safe.
//
// The selection rule is a contract: the caller's selection is a list of
// CATALOG INDICES. Sorting and filtering only move rows around, so a selection
// keeps pointing at the same files; what cannot stay visible is counted
// (hiddenSelection) so the status line can say how many are hidden instead of
// silently dropping them. Reading the selection as row POSITIONS is the classic
// bug this module exists to prevent -- and one of its negative controls.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "ListSortModel.hpp"

namespace prism::list {

// ASCII-case-insensitive substring search. UTF-8 bytes for non-ASCII are
// compared exactly (CJK has no case); the match can therefore start at any
// byte, which is what makes "报告" find "年度报告.txt".
inline bool ContainsFold(std::string_view haystack, std::string_view needle) {
  if (needle.empty()) return true;
  if (needle.size() > haystack.size()) return false;
  const size_t last = haystack.size() - needle.size();
  for (size_t start = 0; start <= last; ++start) {
    size_t k = 0;
    while (k < needle.size() &&
           detail::FoldAscii(static_cast<unsigned char>(haystack[start + k])) ==
               detail::FoldAscii(static_cast<unsigned char>(needle[k]))) {
      ++k;
    }
    if (k == needle.size()) return true;
  }
  return false;
}

// Glob: '*' matches any run of bytes INCLUDING '/', '?' matches exactly one
// UTF-8 CODE POINT (so "报?.txt" matches "报告.txt" but not "报告会.txt").
// Literal bytes must match exactly (no case folding: a glob is a pattern the
// user typed). Backtracking is the standard single-star loop.
inline bool MatchesGlob(std::string_view pattern, std::string_view text) {
  size_t p = 0;
  size_t t = 0;
  size_t star = std::string_view::npos;
  size_t starT = 0;

  while (t < text.size()) {
    if (p < pattern.size() && pattern[p] == '*') {
      star = p;
      ++p;
      starT = t;
      continue;
    }
    if (p < pattern.size()) {
      if (pattern[p] == '?') {
        t += Utf8SequenceLength(text, t);  // one CODE POINT, not one byte
        ++p;
        continue;
      }
      if (pattern[p] == text[t]) {
        ++p;
        ++t;
        continue;
      }
    }
    if (star == std::string_view::npos) return false;
#if defined(PRISM_C13_MUTANT_GLOB_STAR_STOPS_AT_SLASH)
    if (text[starT] == '/') return false;  // MUTATION: '*' cannot cross a separator
#endif
    ++starT;
    p = star + 1;
    t = starT;
  }
  while (p < pattern.size() && pattern[p] == '*') ++p;
  return p == pattern.size();
}

struct FilterSpec {
  std::string substring;              // "" = no constraint
  std::string glob;                   // "" = no constraint
  std::optional<TypeClass> type;      // nullopt = every type
  std::optional<uint64_t> minSize;    // inclusive, bytes
  std::optional<uint64_t> maxSize;    // inclusive, bytes
};

// Size bounds are applied to EVERY row as recorded -- folders carry size 0 in
// the catalog, so a minSize above 0 hides them. That is deliberate (the list is
// filtered by what it shows, not by what a folder would contain), and it is
// asserted in the suite so nobody has to guess.
inline bool MatchesFilter(const Row& row, const FilterSpec& spec) {
  if (!spec.substring.empty() && !ContainsFold(row.name, spec.substring)) return false;
  if (!spec.glob.empty() && !MatchesGlob(spec.glob, row.name)) return false;
  if (spec.type.has_value() && ClassifyType(row.name, row.isDir) != spec.type.value()) {
    return false;
  }
  if (spec.minSize.has_value() && row.size < spec.minSize.value()) return false;
  if (spec.maxSize.has_value() && row.size > spec.maxSize.value()) return false;
  return true;
}

struct FilterResult {
  std::vector<uint32_t> visible;  // catalog indices, in the input order
  std::vector<uint32_t> selection;  // the caller's entries that survived, same order
  size_t hiddenSelection = 0;       // selected rows the filter now hides

  bool IsVisible(uint32_t catalogIndex) const {
    return visibleIndices.count(catalogIndex) != 0;
  }

  std::unordered_set<uint32_t> visibleIndices;
};

// Applies the filter and reports what happened to the selection. Rows are
// visited in the input order, so the visible list is a stable projection of the
// catalog -- filtering never reorders the list, it only removes rows.
inline FilterResult ApplyFilter(const std::vector<Row>& rows,
                                const std::vector<uint32_t>& selection,
                                const FilterSpec& spec) {
  FilterResult out;
  out.visible.reserve(rows.size());
  for (const Row& row : rows) {
    if (!MatchesFilter(row, spec)) continue;
    out.visible.push_back(row.catalogIndex);
    out.visibleIndices.insert(row.catalogIndex);
  }

#if defined(PRISM_C13_MUTANT_SELECTION_BY_POSITION)
  // MUTATION (WILL_FAIL): the caller's selection is read as ROW POSITIONS, not
  // catalog indices -- exactly the confusion this module's contract forbids.
  for (uint32_t entry : selection) {
    const size_t position = static_cast<size_t>(entry);
    if (position < rows.size() && out.visibleIndices.count(rows[position].catalogIndex) != 0) {
      out.selection.push_back(rows[position].catalogIndex);
    } else {
      ++out.hiddenSelection;
    }
  }
#else
  out.selection.reserve(selection.size());
  for (uint32_t entry : selection) {
    if (out.visibleIndices.count(entry) != 0) {
      out.selection.push_back(entry);
    } else {
      ++out.hiddenSelection;
    }
  }
#endif
  return out;
}

}  // namespace prism::list
