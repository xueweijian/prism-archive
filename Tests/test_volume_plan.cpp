// PrismArchive -- PrismCore unit tests for VolumePlan (card C10).
//
// Every rule here is a guard with a demonstrated negative control: the same
// source compiles three times with a deliberate mutation (PRISM_VOLUME_MUTANT_*)
// and the mutant binaries are registered WILL_FAIL in Tests/suites/c10-volumes.cmake
// (CONTRACTS D3). A guard that stays green under its own mutant is not a guard.
#include <string>
#include <vector>

#include "VolumePlan.hpp"
#include "mini_test.hpp"

using prism::FamilyOfPath;
using prism::MakeVolumePlan;
using prism::PlanFromExplicitOrder;
using prism::VolumeEntry;
using prism::VolumeFamily;
using prism::VolumePlan;

namespace {

std::vector<std::string> Names(std::initializer_list<const char*> names) {
  std::vector<std::string> out;
  for (const char* n : names) out.emplace_back(n);
  return out;
}

std::string Joined(const VolumePlan& plan) {
  std::string out;
  for (const auto& v : plan.volumes) {
    if (!out.empty()) out += "|";
    out += v.name;
  }
  return out;
}

}  // namespace

PRISM_TEST("family detection follows the 7-Zip naming families") {
  PRISM_CHECK(FamilyOfPath("C:/dl/photos.7z.001") == VolumeFamily::NumericSplit);
  PRISM_CHECK(FamilyOfPath("photos.zip.001") == VolumeFamily::NumericSplit);
  PRISM_CHECK(FamilyOfPath("photos.z01") == VolumeFamily::SplitZip);
  PRISM_CHECK(FamilyOfPath("photos.part1.rar") == VolumeFamily::RarNew);
  PRISM_CHECK(FamilyOfPath("photos.r00") == VolumeFamily::RarOld);
  // A bare .zip/.rar is not provably a volume set from its name alone.
  PRISM_CHECK(FamilyOfPath("photos.zip") == VolumeFamily::None);
  PRISM_CHECK(FamilyOfPath("photos.rar") == VolumeFamily::None);
  PRISM_CHECK(FamilyOfPath("notes.txt") == VolumeFamily::None);
  // ".001" must be at least three digits; two digits is an ordinary extension.
  PRISM_CHECK(FamilyOfPath("photos.7z.01") == VolumeFamily::None);
}

PRISM_TEST("numeric split: any member resolves to the whole ordered set") {
  const auto siblings = Names({"photos.7z.001", "photos.7z.002", "photos.7z.003",
                               "photos.7z.sha256", "notes.txt"});
  for (const char* opened : {"photos.7z.001", "photos.7z.002", "photos.7z.003",
                             "C:/dl/photos.7z.003"}) {
    const VolumePlan plan = MakeVolumePlan(opened, siblings);
    if (!PRISM_CHECK(plan.ok())) {
      std::cout << "    error: " << plan.error << std::endl;
      continue;
    }
    PRISM_CHECK(plan.family == VolumeFamily::NumericSplit);
    // The stem keeps the archive extension: 7-Zip writes "<name>.7z.001".
    PRISM_CHECK_EQ(plan.stem, std::string("photos.7z"));
    PRISM_CHECK_EQ(plan.volumes.size(), size_t(3));
    PRISM_CHECK_EQ(Joined(plan), std::string("photos.7z.001|photos.7z.002|photos.7z.003"));
    PRISM_CHECK(plan.missing.empty());
  }
}

PRISM_TEST("numeric split: a missing MIDDLE volume is rejected, not skipped") {
  // This is the exact failure mode that must never read half an archive and
  // report success: .001 + .003 concatenates into a stream whose byte offsets
  // are wrong from the start of .002 onwards.
  const VolumePlan plan =
      MakeVolumePlan("photos.7z.001", Names({"photos.7z.001", "photos.7z.003"}));
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK_EQ(plan.missing.size(), size_t(1));
  if (!plan.missing.empty()) PRISM_CHECK_EQ(plan.missing.front(), std::uint64_t(2));
  PRISM_CHECK(plan.error.find("photos.7z.002") != std::string::npos);
  std::cout << "    rejected with: " << plan.error << std::endl;
}

PRISM_TEST("numeric split: a truncated TAIL is a name-level blind spot (documented)") {
  // Names cannot prove .002 is the end of a three-volume set; the engine's
  // 7z start-header check is the authority there (see win.volumes). This case
  // is asserted so the limitation is explicit rather than silently assumed.
  const VolumePlan plan =
      MakeVolumePlan("photos.7z.001", Names({"photos.7z.001", "photos.7z.002"}));
  PRISM_CHECK(plan.ok());
  PRISM_CHECK_EQ(plan.volumes.size(), size_t(2));
}

PRISM_TEST("numeric split: a set that does not start at .001 is rejected") {
  const VolumePlan plan =
      MakeVolumePlan("photos.7z.002", Names({"photos.7z.002", "photos.7z.003"}));
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK(plan.error.find("must start at 1") != std::string::npos);
}

PRISM_TEST("split ZIP: z01..zNN plus the .zip finale is one ordered set") {
  const auto siblings = Names({"pack.zip", "pack.z01", "pack.z02", "unrelated.zip"});
  for (const char* opened : {"pack.z01", "pack.z02", "pack.zip"}) {
    const VolumePlan plan = MakeVolumePlan(opened, siblings);
    if (!PRISM_CHECK(plan.ok())) {
      std::cout << "    error: " << plan.error << std::endl;
      continue;
    }
    PRISM_CHECK(plan.family == VolumeFamily::SplitZip);
    PRISM_CHECK_EQ(plan.stem, std::string("pack"));
    // The central directory lives in .zip, so it MUST be the last volume.
    PRISM_CHECK_EQ(Joined(plan), std::string("pack.z01|pack.z02|pack.zip"));
  }
}

PRISM_TEST("split ZIP: a missing .zip finale is rejected") {
  // Without the central directory the archive is not readable at all; reading
  // z01+z02 and calling it success is the failure this guard exists for.
  const VolumePlan plan = MakeVolumePlan("pack.z01", Names({"pack.z01", "pack.z02"}));
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK(plan.error.find("pack.zip") != std::string::npos);
  std::cout << "    rejected with: " << plan.error << std::endl;
}

PRISM_TEST("split ZIP: a missing middle part is rejected") {
  const VolumePlan plan =
      MakeVolumePlan("pack.zip", Names({"pack.z01", "pack.z03", "pack.zip"}));
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK_EQ(plan.missing.size(), size_t(1));
  if (!plan.missing.empty()) PRISM_CHECK_EQ(plan.missing.front(), std::uint64_t(2));
  PRISM_CHECK(plan.error.find("pack.z02") != std::string::npos);
}

PRISM_TEST("RAR new style: part1..partN are ordered numerically, not lexically") {
  // Lexical order would put part10 before part2. A ten-part set with only
  // parts 1, 2 and 10 present is INCOMPLETE, so ordering and the gap list are
  // asserted on a plan that is correctly rejected.
  const auto siblings = Names({"movie.part1.rar", "movie.part2.rar", "movie.part10.rar"});
  const VolumePlan plan = MakeVolumePlan("movie.part10.rar", siblings);
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK(plan.family == VolumeFamily::RarNew);
  PRISM_CHECK_EQ(plan.volumes.size(), size_t(3));
  PRISM_CHECK_EQ(plan.volumes.front().name, std::string("movie.part1.rar"));
  PRISM_CHECK_EQ(plan.volumes.back().name, std::string("movie.part10.rar"));
  PRISM_CHECK_EQ(plan.missing.size(), size_t(7));  // 3..9
  PRISM_CHECK(plan.error.find("movie.part3.rar") != std::string::npos);
}

PRISM_TEST("RAR old style: r00 starts at ordinal 0, the .rar is last") {
  const VolumePlan plan =
      MakeVolumePlan("movie.rar", Names({"movie.rar", "movie.r00", "movie.r01"}));
  if (!PRISM_CHECK(plan.ok())) {
    std::cout << "    error: " << plan.error << std::endl;
    return;
  }
  PRISM_CHECK(plan.family == VolumeFamily::RarOld);
  PRISM_CHECK_EQ(Joined(plan), std::string("movie.r00|movie.r01|movie.rar"));
  PRISM_CHECK_EQ(plan.volumes.front().ordinal, std::uint64_t(0));
  PRISM_CHECK_EQ(plan.volumes.back().ordinal, std::uint64_t(2));
}

PRISM_TEST("explicit order: ascending is accepted, scrambled order is rejected") {
  std::vector<VolumeEntry> ascending = {{"a.7z.001", 1, 100}, {"a.7z.002", 2, 100},
                                        {"a.7z.003", 3, 20}};
  const VolumePlan good =
      PlanFromExplicitOrder(VolumeFamily::NumericSplit, "a.7z", ascending);
  PRISM_CHECK(good.ok());
  PRISM_CHECK_EQ(Joined(good), std::string("a.7z.001|a.7z.002|a.7z.003"));

  std::vector<VolumeEntry> scrambled = {{"a.7z.002", 2, 100}, {"a.7z.001", 1, 100}};
  const VolumePlan bad = PlanFromExplicitOrder(VolumeFamily::NumericSplit, "a.7z", scrambled);
  PRISM_CHECK(!bad.ok());
  PRISM_CHECK(bad.error.find("not ascending") != std::string::npos);
  std::cout << "    rejected with: " << bad.error << std::endl;

  std::vector<VolumeEntry> duplicate = {{"a.7z.001", 1, 100}, {"a.7z.001", 1, 100}};
  PRISM_CHECK(!PlanFromExplicitOrder(VolumeFamily::NumericSplit, "a.7z", duplicate).ok());
}

PRISM_TEST("explicit order: a final volume that is not last is rejected") {
  std::vector<VolumeEntry> zipNotLast = {{"pack.z01", 1, 100}, {"pack.zip", 2, 100},
                                         {"pack.z02", 3, 100}};
  const VolumePlan plan = PlanFromExplicitOrder(VolumeFamily::SplitZip, "pack", zipNotLast);
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK(plan.error.find("final volume must be last") != std::string::npos);
}

PRISM_TEST("a plain archive beside unrelated files is not misread as volumes") {
  const auto siblings = Names({"photos.zip", "notes.txt", "readme.md"});
  const VolumePlan plan = MakeVolumePlan("photos.zip", siblings);
  PRISM_CHECK(!plan.ok());
  PRISM_CHECK(plan.error.find("single-volume") != std::string::npos);
}

PRISM_TEST("uppercase volume names are accepted case-insensitively") {
  const VolumePlan plan =
      MakeVolumePlan("PHOTOS.7Z.001", Names({"PHOTOS.7Z.001", "PHOTOS.7Z.002"}));
  if (!PRISM_CHECK(plan.ok())) {
    std::cout << "    error: " << plan.error << std::endl;
    return;
  }
  PRISM_CHECK_EQ(plan.volumes.size(), size_t(2));
  PRISM_CHECK_EQ(plan.volumes.front().name, std::string("PHOTOS.7Z.001"));
}

PRISM_TEST("sizes sum to the concatenated stream length") {
  VolumePlan plan;
  plan.volumes = {{"a.7z.001", 1, 4096}, {"a.7z.002", 2, 4096}, {"a.7z.003", 3, 512}};
  PRISM_CHECK_EQ(plan.totalSize(), std::uint64_t(8704));
}

PRISM_TEST_MAIN()
