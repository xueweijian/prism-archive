// PrismArchive -- PrismCore
// Bandizip-style "Smart Extraction" zero-payload topology decision tree
// (blueprint ch. 4.2 + ch. 6.2).
//
// The whole decision is taken from the central directory alone -- no byte of
// payload is decompressed -- and is materialized as a *path rewrite rule* that
// the extract callback applies while streaming, so the output lands directly at
// its final physical path with zero post-extraction MoveFile traffic.
#pragma once

#include <functional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"

namespace prism {

enum class SmartStrategy {
  EmptyArchive,                 // nothing to extract
  FlatDirectly,                 // branch A/B: single root, lay it down as-is
  StripRedundantNestedRoot,     // branch B+: A/A/* nesting doll, peel one level
  WrapIntoNewNamedSubfolder,    // branch D: scattered roots -> ./<ArchiveName>/
  WrapAndFilterPromoNoise       // branch E: main dir + ad junk -> drop the junk
};

inline const char* ToString(SmartStrategy s) {
  switch (s) {
    case SmartStrategy::EmptyArchive: return "EmptyArchive";
    case SmartStrategy::FlatDirectly: return "FlatDirectly";
    case SmartStrategy::StripRedundantNestedRoot: return "StripRedundantNestedRoot";
    case SmartStrategy::WrapIntoNewNamedSubfolder: return "WrapIntoNewNamedSubfolder";
    case SmartStrategy::WrapAndFilterPromoNoise: return "WrapAndFilterPromoNoise";
  }
  return "Unknown";
}

struct SmartPlan {
  SmartStrategy strategy = SmartStrategy::WrapIntoNewNamedSubfolder;
  std::string baseTargetDir;           // every rewritten path is joined to this
  std::string stripPrefix;             // prefix removed from each entry path
  std::set<uint32_t> ignoredIndices;   // __MACOSX / .DS_Store / promo junk
  uint64_t fileCount = 0;
  uint64_t totalBytes = 0;
  std::string reason;                  // surfaced in the UI + asserted in tests
};

struct PlannerOptions {
  // Branch E: flatten the main directory into the target dir (Bandizip-like) or
  // keep it and merely delete the ad files.
  bool flattenPromoMainDir = true;
  // Promo filtering is a heuristic; callers may disable it (e.g. "extract all").
  bool filterPromoNoise = true;
};

class SmartExtractionPlanner {
 public:
  // Injected so the planner stays filesystem-free (and therefore testable).
  using ExistsFn = std::function<bool(const std::string&)>;

  static SmartPlan Build(const std::string& archivePath, const std::string& outputDir,
                         const std::vector<ArchiveItem>& items, ExistsFn exists,
                         const PlannerOptions& options = {}) {
    SmartPlan plan;
    std::set<std::string> topEntries;   // unique first path components
    std::set<std::string> topDirs;      // subset that are directories
    std::set<std::string> secondLevel;  // components under the first level
    std::vector<uint32_t> promoIndices;
    uint64_t promoFiles = 0;

    for (const auto& item : items) {
      const std::string p = NormalizeArchivePath(item.path);
      if (p.empty()) {
        plan.ignoredIndices.insert(item.index);
        continue;
      }
      if (IsNoiseEntry(p)) {
        plan.ignoredIndices.insert(item.index);
        continue;
      }
      // Promo filtering is top-level only: a nested "docs/readme.txt" is real
      // content, an ad file sitting next to the main folder is not.
      const bool isPromo =
          options.filterPromoNoise && IsPromoNoise(p) && !item.isDir && p.find('/') == std::string::npos;
      if (isPromo) {
        promoIndices.push_back(item.index);
        ++promoFiles;
      }
      if (!item.isDir && !isPromo) {
        ++plan.fileCount;
        plan.totalBytes += item.size;
      }

      const size_t slash = p.find('/');
      const std::string root = std::string(RootSegment(p));
      topEntries.insert(root);
      if (item.isDir || slash != std::string::npos) topDirs.insert(root);
      if (slash != std::string::npos && slash + 1 < p.size()) {
        const std::string rest = p.substr(slash + 1);
        const size_t s2 = rest.find('/');
        secondLevel.insert((s2 == std::string::npos) ? rest : rest.substr(0, s2));
      }
    }

    if (topEntries.empty()) {
      plan.strategy = SmartStrategy::EmptyArchive;
      plan.baseTargetDir = outputDir;
      plan.reason = "archive contains no extractable entries";
      return plan;
    }

    const std::string stem = ArchiveStem(archivePath);

    if (topEntries.size() == 1) {
      const std::string soleRoot = *topEntries.begin();

      // Branch B+: A/A/* nesting doll -> peel exactly one redundant prefix.
      if (topDirs.size() == 1 && secondLevel.size() == 1 &&
          EqualsIgnoreCase(soleRoot, *secondLevel.begin())) {
        plan.strategy = SmartStrategy::StripRedundantNestedRoot;
        // Peel the whole doll ("Photo/Photo/"), not just one level: the base
        // directory already carries the folder name, so stripping a single
        // level would recreate "Photo/Photo/1.jpg" inside it.
        plan.stripPrefix = soleRoot + "/" + *secondLevel.begin() + "/";
        plan.baseTargetDir = ResolveCollisionFreeDir(outputDir, soleRoot, exists);
        plan.reason = "nested doll archive: '" + soleRoot + "/" + soleRoot +
                      "' collapsed to a single level";
        ApplyPromoFilter(plan, promoIndices);
        return plan;
      }

      const std::string candidate = JoinPath(outputDir, soleRoot);
      if (!exists(candidate)) {
        // Branch A/B: nothing collides -- lay it down flat, no wrapping.
        plan.strategy = SmartStrategy::FlatDirectly;
        plan.baseTargetDir = outputDir;
        plan.reason = "single root '" + soleRoot + "' with no name collision";
      } else if (!topDirs.count(soleRoot)) {
        // Collision on a LOOSE FILE: never rename it into a directory named
        // after itself ("pack.txt (2)/pack.txt") -- that buries the file in a
        // folder the user never asked for. Keep the output dir and let the
        // per-item collision resolver rename the file beside its siblings.
        plan.strategy = SmartStrategy::FlatDirectly;
        plan.baseTargetDir = outputDir;
        plan.reason = "target '" + soleRoot + "' exists -> file renamed in place";
      } else {
        // Collision on a real DIRECTORY: never silently merge/overwrite.
        // Wrap into "<name> (2)" so the existing folder survives intact.
        plan.strategy = SmartStrategy::StripRedundantNestedRoot;
        plan.stripPrefix = (topDirs.size() == 1) ? (soleRoot + "/") : std::string();
        plan.baseTargetDir = ResolveCollisionFreeDir(outputDir, soleRoot, exists);
        plan.reason = "target '" + soleRoot + "' already exists -> collision-safe folder";
      }
      ApplyPromoFilter(plan, promoIndices);
      return plan;
    }

    // Branch E: exactly one real folder plus a couple of ad files.
    const uint64_t looseFiles = topEntries.size() - topDirs.size();
    if (topDirs.size() == 1 && promoFiles > 0 && looseFiles <= promoFiles &&
        looseFiles <= 2) {
      const std::string mainDir = *topDirs.begin();
      for (uint32_t idx : promoIndices) plan.ignoredIndices.insert(idx);
      plan.strategy = SmartStrategy::WrapAndFilterPromoNoise;
      if (options.flattenPromoMainDir) {
        plan.stripPrefix = mainDir + "/";
        plan.baseTargetDir = outputDir;
        plan.reason = "promo junk removed and '" + mainDir + "' flattened into target";
      } else {
        plan.baseTargetDir = outputDir;
        plan.reason = "promo junk removed, '" + mainDir + "' kept as a folder";
      }
      return plan;
    }

    // Branch D: multiple scattered roots -> build a safety capsule folder.
    plan.strategy = SmartStrategy::WrapIntoNewNamedSubfolder;
    plan.baseTargetDir = ResolveCollisionFreeDir(outputDir, stem, exists);
    const std::string wrapName = std::string(FileNameOf(plan.baseTargetDir));
    plan.reason = std::to_string(topEntries.size()) +
                  " scattered top-level roots -> wrapped into '" +
                  (wrapName.empty() ? stem : wrapName) + "'";
    ApplyPromoFilter(plan, promoIndices);
    return plan;
  }

  // Called per item inside IArchiveExtractCallback::GetStream -- O(1) and
  // side-effect free. Returns the final on-disk path for this entry.
  static std::string TransformItemPath(const SmartPlan& plan, std::string_view rawArchivePath) {
    std::string rel = SanitizeRelativePath(rawArchivePath);
    if (!plan.stripPrefix.empty()) {
      const std::string prefix = NormalizeArchivePath(plan.stripPrefix);
      // The wrapper folder entry itself collapses onto the base directory --
      // otherwise it would be recreated one level too deep (Photo/Photo).
      if (rel == RootSegment(prefix)) return plan.baseTargetDir;
      if (rel.size() >= prefix.size() && rel.compare(0, prefix.size(), prefix) == 0) {
        rel.erase(0, prefix.size());
        while (!rel.empty() && rel.front() == '/') rel.erase(rel.begin());
      }
    }
    if (rel.empty()) return plan.baseTargetDir;  // the root folder entry itself
    return JoinPath(plan.baseTargetDir, rel);
  }

  static std::string ResolveCollisionFreeDir(const std::string& parentDir,
                                             const std::string& desiredName, ExistsFn exists) {
    std::string candidate = JoinPath(parentDir, desiredName);
    if (!exists(candidate)) return candidate;
    for (int suffix = 2; suffix < 100000; ++suffix) {
      std::string numbered =
          JoinPath(parentDir, desiredName + " (" + std::to_string(suffix) + ")");
      if (!exists(numbered)) return numbered;
    }
    return candidate;
  }

 private:
  static void ApplyPromoFilter(SmartPlan& plan, const std::vector<uint32_t>& promo) {
    // Never let a heuristic delete the entire archive: if nothing but "promo"
    // files survived, they are the payload and must be extracted.
    if (plan.fileCount == 0) return;
    for (uint32_t idx : promo) plan.ignoredIndices.insert(idx);
  }
};

}  // namespace prism
