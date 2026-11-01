// PrismArchive -- PrismCore
// VolumePlan: platform-neutral reasoning about multi-volume archives
// (blueprint ch. 2.1 -- the volume-merge logic lifted out of CPP/7zip/UI/Common).
//
// WHAT IS AUTHORITATIVE (verified against 7-Zip upstream, not guessed):
//   * Writer naming: CPP/7zip/Common/MultiOutStream.cpp GetFilePath():
//       name.Add_UInt32(index + 1); while (name.Len() < 3) InsertAtFront('0')
//       -> "<prefix>.001", "<prefix>.002", ... (minimum three digits)
//     Volume splitting is NOT an IOutArchive/ISetProperties property: Update.cpp
//     wraps the output in a CMultiOutStream (IOutStream) and the handler never
//     sees a volume size. See Src/PrismWin/VolumeStream.hpp.
//   * Reader contract: IArchiveOpenVolumeCallback { GetProperty(kpidName),
//     GetStream(name) } - family 06, member 0x30 (CPP/7zip/Archive/IArchive.h).
//     kpidName = 4 (CPP/7zip/PropID.h, counted from kpidNoProperty = 0).
//   * Naming families handled here:
//       7-Zip / numeric split   : "<stem>.7z.001", ".002", ...  (also .zip.001)
//       split ZIP (WinZip)      : "<stem>.z01" .. "<stem>.zNN" + "<stem>.zip" last
//       RAR new style           : "<stem>.part1.rar", ".part2.rar" ...
//       RAR old style           : "<stem>.r00" .. "<stem>.rNN" + "<stem>.rar" last
//
// LAYERING: this header is pure C++20. It never touches the filesystem; the
// caller supplies the sibling names (from a directory enumeration) so the same
// rules are unit-testable on Linux in milliseconds.
//
// HONEST LIMIT (CONTRACTS D18): names alone cannot prove the LAST volume of a
// numeric split exists -- a.7z.001 + a.7z.002 may be a truncated three-volume
// set. That case is caught by the engine (missing 7z start header) when the
// concatenated stream is opened; VolumeStream surfaces the engine's failure and
// never reports success. Missing MIDDLE volumes, which names CAN prove, are
// rejected before the engine is touched.
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

namespace prism {

enum class VolumeFamily {
  None,
  NumericSplit,  // .7z.001 / .zip.001 / any ".<3+ digits>"
  SplitZip,      // .z01 .. .zip (last)
  RarNew,        // .partN.rar
  RarOld,        // .r00 .. .rar (last)
};

inline const char* ToString(VolumeFamily family) {
  switch (family) {
    case VolumeFamily::None: return "None";
    case VolumeFamily::NumericSplit: return "NumericSplit";
    case VolumeFamily::SplitZip: return "SplitZip";
    case VolumeFamily::RarNew: return "RarNew";
    case VolumeFamily::RarOld: return "RarOld";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, VolumeFamily family) {
  return os << ToString(family);
}

struct VolumeEntry {
  std::string name;          // basename exactly as it appears on disk
  std::uint64_t ordinal = 0; // 1-based for NumericSplit/SplitZip/RarNew, 0-based for RarOld
  std::uint64_t size = 0;    // bytes when known, else 0
};

struct VolumePlan {
  VolumeFamily family = VolumeFamily::None;
  std::string stem;  // shared prefix, e.g. "photos.7z" (no ordinal / part suffix)
  std::vector<VolumeEntry> volumes;  // ascending by ordinal; last volume last
  std::vector<std::uint64_t> missing;
  std::string error;  // non-empty => this plan must NOT be used

  bool ok() const { return error.empty() && !volumes.empty(); }

  std::uint64_t totalSize() const {
    std::uint64_t total = 0;
    for (const VolumeEntry& v : volumes) total += v.size;
    return total;
  }
};

namespace volume_detail {

inline std::string Lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

inline std::string_view BaseName(std::string_view path) {
  const size_t slash = path.find_last_of("/\\");
  return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

inline bool AllDigits(std::string_view text) {
  if (text.empty()) return false;
  for (const char c : text) {
    if (c < '0' || c > '9') return false;
  }
  return true;
}

inline std::uint64_t DigitsValue(std::string_view text) {
  std::uint64_t value = 0;
  for (const char c : text) {
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10) return UINT64_MAX;  // saturate, never wrap
    value = value * 10 + digit;
  }
  return value;
}

// 7-Zip pads to at least three digits; larger indices grow naturally.
inline std::string PadOrdinal(std::uint64_t ordinal, size_t width) {
  std::string digits = std::to_string(ordinal);
  while (digits.size() < width) digits.insert(digits.begin(), '0');
  return digits;
}

// Splits "a.b.c" into (stem, extension-without-dot). Returns false when the
// name has no dot or ends with one.
inline bool SplitLastDot(std::string_view name, std::string_view* stem, std::string_view* last) {
  const size_t dot = name.find_last_of('.');
  if (dot == std::string_view::npos || dot == 0 || dot + 1 >= name.size()) return false;
  *stem = name.substr(0, dot);
  *last = name.substr(dot + 1);
  return true;
}

}  // namespace volume_detail

// ---------------------------------------------------------------------------
// Name parsing
// ---------------------------------------------------------------------------
// Fills stem/ordinal when "name" is a numbered member of "family".
inline bool ParseVolumeName(std::string_view name, VolumeFamily family, std::string* stem,
                            std::uint64_t* ordinal, size_t* width = nullptr) {
  using volume_detail::AllDigits;
  using volume_detail::BaseName;
  using volume_detail::DigitsValue;
  using volume_detail::Lower;
  using volume_detail::SplitLastDot;

  const std::string_view base = BaseName(name);
  const std::string lower = Lower(base);
  std::string_view stemView;
  std::string_view last;
  if (!SplitLastDot(lower, &stemView, &last)) return false;

  auto setStem = [&](std::string_view loweredStem) {
    // Keep the on-disk case: slice the same length off the original name.
    *stem = std::string(base.substr(0, loweredStem.size()));
  };

  switch (family) {
    case VolumeFamily::NumericSplit: {
      // ".<3+ digits>" and the ordinal starts at 1 (7-Zip writes .001 first).
      if (last.size() < 3 || !AllDigits(last)) return false;
      const std::uint64_t value = DigitsValue(last);
      if (value == 0) return false;
      setStem(stemView);
      *ordinal = value;
      if (width) *width = last.size();
      return true;
    }
    case VolumeFamily::SplitZip: {
      if (last.size() < 3 || last[0] != 'z') return false;  // "z" + two or more digits
      const std::string_view digits = last.substr(1);
      if (!AllDigits(digits)) return false;
      const std::uint64_t value = DigitsValue(digits);
      if (value == 0) return false;
      setStem(stemView);
      *ordinal = value;
      if (width) *width = digits.size();
      return true;
    }
    case VolumeFamily::RarNew: {
      if (last != "rar") return false;
      const size_t part = stemView.rfind(".part");
      if (part == std::string_view::npos || part == 0) return false;
      const std::string_view digits = stemView.substr(part + 5);
      if (!AllDigits(digits)) return false;
      const std::uint64_t value = DigitsValue(digits);
      if (value == 0) return false;
      setStem(stemView.substr(0, part));
      *ordinal = value;
      if (width) *width = digits.size();
      return true;
    }
    case VolumeFamily::RarOld: {
      // ".r00" .. ".r99" (old WinRAR) -- the ordinal is 0-based.
      if (last.size() < 3 || last[0] != 'r') return false;
      const std::string_view digits = last.substr(1);
      if (!AllDigits(digits)) return false;
      setStem(stemView);
      *ordinal = DigitsValue(digits);
      if (width) *width = digits.size();
      return true;
    }
    case VolumeFamily::None:
      return false;
  }
  return false;
}

// Family implied by the path itself (None for a bare .zip/.rar: those are only
// multi-volume when a sibling set proves it, which MakeVolumePlan checks).
inline VolumeFamily FamilyOfPath(std::string_view path) {
  const std::string_view base = volume_detail::BaseName(path);
  const std::string lower = volume_detail::Lower(base);
  std::string_view stemView;
  std::string_view last;
  if (!volume_detail::SplitLastDot(lower, &stemView, &last)) return VolumeFamily::None;
  if (last.size() >= 3 && volume_detail::AllDigits(last) &&
      volume_detail::DigitsValue(last) > 0) {
    return VolumeFamily::NumericSplit;
  }
  if (last.size() >= 3 && last[0] == 'z' && volume_detail::AllDigits(last.substr(1)) &&
      volume_detail::DigitsValue(last.substr(1)) > 0) {
    return VolumeFamily::SplitZip;
  }
  if (last == "rar") {
    const size_t part = stemView.rfind(".part");
    if (part != std::string_view::npos &&
        volume_detail::AllDigits(stemView.substr(part + 5)) &&
        volume_detail::DigitsValue(stemView.substr(part + 5)) > 0) {
      return VolumeFamily::RarNew;
    }
    return VolumeFamily::None;  // plain .rar; may still be a set with .r00 siblings
  }
  if (last.size() >= 3 && last[0] == 'r' && volume_detail::AllDigits(last.substr(1))) {
    return VolumeFamily::RarOld;
  }
  return VolumeFamily::None;
}

inline bool LooksLikeVolumePath(std::string_view path) {
  return FamilyOfPath(path) != VolumeFamily::None;
}

namespace volume_detail {

inline std::string MissingName(VolumeFamily family, const std::string& stem,
                               std::uint64_t ordinal, size_t width) {
  switch (family) {
    case VolumeFamily::NumericSplit:
      return stem + "." + PadOrdinal(ordinal, width < 3 ? 3 : width);
    case VolumeFamily::SplitZip:
      return stem + ".z" + PadOrdinal(ordinal, width < 2 ? 2 : width);
    case VolumeFamily::RarNew:
      return stem + ".part" + PadOrdinal(ordinal, width < 1 ? 1 : width) + ".rar";
    case VolumeFamily::RarOld:
      return stem + ".r" + PadOrdinal(ordinal, width < 2 ? 2 : width);
    case VolumeFamily::None:
      break;
  }
  return stem + "." + std::to_string(ordinal);
}

inline std::string ErrorWithMissing(std::string prefix, const std::vector<std::string>& names) {
  for (size_t i = 0; i < names.size(); ++i) {
    prefix += (i == 0 ? " " : ", ");
    prefix += names[i];
  }
  return prefix;
}

}  // namespace volume_detail

// Validates a caller-supplied ordering (e.g. a multi-selection handed to the app
// in shell order). Scrambled order, duplicates, and a final volume that is not
// last are rejected -- reading volumes out of order yields a byte-correct but
// semantically scrambled stream, and silently "sorting it out" hides a real
// user error instead of reporting it.
inline VolumePlan PlanFromExplicitOrder(VolumeFamily family, std::string stem,
                                        std::vector<VolumeEntry> given) {
  VolumePlan plan;
  plan.family = family;
  plan.stem = std::move(stem);
  if (given.empty()) {
    plan.error = "no volumes";
    return plan;
  }
  for (size_t i = 1; i < given.size(); ++i) {
#ifdef PRISM_VOLUME_MUTANT_ACCEPT_UNSORTED
    (void)i;
#else
    if (given[i].ordinal <= given[i - 1].ordinal) {
      plan.error = "volume order is not ascending: '" + given[i - 1].name + "' before '" +
                   given[i].name + "'";
      plan.volumes = std::move(given);
      return plan;
    }
#endif
  }
#ifndef PRISM_VOLUME_MUTANT_LAST_VOLUME_OPTIONAL
  if (family == VolumeFamily::SplitZip || family == VolumeFamily::RarOld) {
    const std::string expected = family == VolumeFamily::SplitZip ? ".zip" : ".rar";
    const std::string lastLower = volume_detail::Lower(given.back().name);
    if (lastLower.size() < expected.size() ||
        lastLower.compare(lastLower.size() - expected.size(), expected.size(), expected) != 0) {
      plan.error = "final volume must be last, found '" + given.back().name + "'";
      plan.volumes = std::move(given);
      return plan;
    }
  }
#endif
  plan.volumes = std::move(given);
  return plan;
}

// Infers the full ordered volume set from a directory listing.
//
// requestedPath is the file the user opened (any member of the set); siblings
// are the basenames present in the same directory (unrelated files are filtered
// out by stem).
inline VolumePlan MakeVolumePlan(std::string_view requestedPath,
                                 const std::vector<std::string>& siblings) {
  using namespace volume_detail;

  VolumePlan plan;
  const std::string_view requestedBase = BaseName(requestedPath);
  VolumeFamily family = FamilyOfPath(requestedPath);
  std::string stem;

  if (family != VolumeFamily::None) {
    std::uint64_t ordinal = 0;
    if (!ParseVolumeName(requestedBase, family, &stem, &ordinal)) {
      plan.error = "cannot parse volume name: " + std::string(requestedBase);
      return plan;
    }
  } else {
    // A bare ".zip" / ".rar" is a volume set only when numbered siblings share
    // the same stem; otherwise it is an ordinary single-file archive.
    const std::string lower = Lower(requestedBase);
    std::string_view stemView;
    std::string_view last;
    if (!SplitLastDot(lower, &stemView, &last) || (last != "zip" && last != "rar")) {
      plan.error = "not a volume path: " + std::string(requestedBase);
      return plan;
    }
    const std::string prefix(requestedBase.substr(0, stemView.size()));
    for (const std::string& sibling : siblings) {
      for (const VolumeFamily candidate : {VolumeFamily::SplitZip, VolumeFamily::RarOld}) {
        std::string candidateStem;
        std::uint64_t ordinal = 0;
        if (ParseVolumeName(sibling, candidate, &candidateStem, &ordinal) &&
            Lower(candidateStem) == Lower(prefix)) {
          family = candidate;
          stem = prefix;
          break;
        }
      }
      if (family != VolumeFamily::None) break;
    }
    if (family == VolumeFamily::None) {
      plan.error = "single-volume archive (no split parts beside '" +
                   std::string(requestedBase) + "')";
      return plan;
    }
  }

  size_t width = 0;
  bool sawFinal = false;
  std::vector<VolumeEntry> entries;
  for (const std::string& sibling : siblings) {
    std::string siblingStem;
    std::uint64_t ordinal = 0;
    size_t siblingWidth = 0;
    if (ParseVolumeName(sibling, family, &siblingStem, &ordinal, &siblingWidth) &&
        Lower(siblingStem) == Lower(stem)) {
      entries.push_back({sibling, ordinal, 0});
      width = (std::max)(width, siblingWidth);
    }
  }

  const bool needsFinal =
      family == VolumeFamily::SplitZip || family == VolumeFamily::RarOld;
  if (needsFinal) {
    const std::string finalName = stem + (family == VolumeFamily::SplitZip ? ".zip" : ".rar");
    for (const std::string& sibling : siblings) {
      if (Lower(sibling) == Lower(finalName)) {
        sawFinal = true;
        break;
      }
    }
#ifndef PRISM_VOLUME_MUTANT_LAST_VOLUME_OPTIONAL
    if (!sawFinal) {
      plan.error = "missing final volume: " + finalName;
    }
#else
    (void)sawFinal;
#endif
    if (entries.empty()) {
      if (plan.error.empty()) plan.error = "no numbered parts beside " + std::string(requestedBase);
      plan.family = family;
      plan.stem = stem;
      return plan;
    }
    std::uint64_t lastOrdinal = 0;
    for (const VolumeEntry& entry : entries) {
      lastOrdinal = (std::max)(lastOrdinal, entry.ordinal);
    }
    // Record the final volume at (max part + 1) so the ordering is total.
    entries.push_back({finalName, lastOrdinal + 1, 0});
  }

  if (entries.empty()) {
    plan.error = "no volumes found beside " + std::string(requestedBase);
    plan.family = family;
    plan.stem = stem;
    return plan;
  }

  std::sort(entries.begin(), entries.end(),
            [](const VolumeEntry& a, const VolumeEntry& b) { return a.ordinal < b.ordinal; });

  for (size_t i = 1; i < entries.size(); ++i) {
    if (entries[i].ordinal == entries[i - 1].ordinal) {
      plan.error = "duplicate volume ordinal: '" + entries[i - 1].name + "' and '" +
                   entries[i].name + "'";
      plan.family = family;
      plan.stem = stem;
      plan.volumes = std::move(entries);
      return plan;
    }
  }

  // Contiguity: 1..N for numeric splits and split ZIP (whose final .zip sits at
  // N+1); 0..N for old-style RAR.
  const std::uint64_t firstOrdinal = entries.front().ordinal;
  if (family == VolumeFamily::RarOld) {
    if (firstOrdinal != 0) {
      plan.error = "RAR old-style set must start at .r00 (found ordinal " +
                   std::to_string(firstOrdinal) + ")";
      plan.family = family;
      plan.stem = stem;
      plan.volumes = std::move(entries);
      return plan;
    }
  } else if (firstOrdinal != 1) {
    plan.error = "volume set must start at 1 (found '" + entries.front().name + "')";
    plan.family = family;
    plan.stem = stem;
    plan.volumes = std::move(entries);
    return plan;
  }

#ifndef PRISM_VOLUME_MUTANT_IGNORE_GAPS
  std::vector<std::string> missingNames;
  std::uint64_t expect = entries.front().ordinal;
  for (const VolumeEntry& entry : entries) {
    while (expect < entry.ordinal) {
      plan.missing.push_back(expect);
      missingNames.push_back(MissingName(family, stem, expect, width));
      ++expect;
    }
    expect = entry.ordinal + 1;
  }
  if (!missingNames.empty()) {
    plan.error = ErrorWithMissing("missing volume(s):", missingNames);
  }
#endif

  plan.family = family;
  plan.stem = stem;
  plan.volumes = std::move(entries);
  return plan;
}

}  // namespace prism
