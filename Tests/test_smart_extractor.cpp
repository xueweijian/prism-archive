// Five-branch Smart Extraction decision tree (blueprint ch. 4.2).
#include "SmartExtractor.hpp"
#include "mini_test.hpp"

#include <set>
#include <string>

namespace {

using prism::ArchiveItem;
using prism::SmartStrategy;

std::vector<ArchiveItem> MakeCatalog(const std::vector<std::pair<std::string, bool>>& entries) {
  std::vector<ArchiveItem> out;
  uint32_t index = 0;
  for (const auto& e : entries) {
    ArchiveItem item;
    item.index = index++;
    item.path = e.first;
    item.isDir = e.second;
    item.size = e.second ? 0 : 1024;
    out.push_back(item);
  }
  return out;
}

// Fake filesystem: the planner never touches the real disk.
struct FakeFs {
  std::set<std::string> existing;
  prism::SmartExtractionPlanner::ExistsFn Fn() {
    return [this](const std::string& p) { return existing.count(p) > 0; };
  }
};

PRISM_TEST("branch A: single loose file lays flat") {
  FakeFs fs;
  const auto catalog = MakeCatalog({{"readme.md", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/pack.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)), std::string("FlatDirectly"));
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl"));
  PRISM_CHECK(plan.stripPrefix.empty());
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "readme.md"),
                 std::string("C:/dl/readme.md"));
}

PRISM_TEST("branch B: single root folder keeps its own name") {
  FakeFs fs;
  const auto catalog = MakeCatalog({{"Assets/", true}, {"Assets/a.png", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/pack.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)), std::string("FlatDirectly"));
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl"));
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Assets/a.png"),
                 std::string("C:/dl/Assets/a.png"));
}

PRISM_TEST("branch B+: A/A nesting doll is peeled exactly one level") {
  FakeFs fs;
  const auto catalog =
      MakeCatalog({{"Photo/", true}, {"Photo/Photo/", true}, {"Photo/Photo/1.jpg", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Photo.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("StripRedundantNestedRoot"));
  // The doll prefix is peeled in full: the base dir already carries "Photo".
  PRISM_CHECK_EQ(plan.stripPrefix, std::string("Photo/Photo/"));
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl/Photo"));
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Photo/Photo/1.jpg"),
                 std::string("C:/dl/Photo/1.jpg"));
  // The wrapper folder entry itself maps onto the base dir.
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Photo/"),
                 std::string("C:/dl/Photo"));
}

PRISM_TEST("branch A collision: a loose FILE never becomes a directory name") {
  // Review item 5.3: when the single root is a file that already exists, the
  // collision helper was handed the file name and produced
  // "C:/dl/pack.txt (2)/pack.txt" -- a directory named after the file, with
  // the file inside it. That is never what the user wants; the file must be
  // renamed next to its sibling instead.
  FakeFs fs;
  fs.existing.insert("C:/dl/pack.txt");
  const auto catalog = MakeCatalog({{"pack.txt", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/pack.zip", "C:/dl", catalog,
                                                         fs.Fn());
  const std::string target =
      prism::SmartExtractionPlanner::TransformItemPath(plan, "pack.txt");
  // The plan lays the file flat in the output dir and does NOT invent a
  // directory for it; renaming to "pack (2).txt" is the CollisionResolver's
  // job one layer down (it is the component that touches the filesystem).
  if (!PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl"))) return;
  if (!PRISM_CHECK_EQ(target, std::string("C:/dl/pack.txt"))) return;
  // ...and the resolver, given that same visible path, does rename it.
  prism::CollisionPolicy policy;
  prism::CollisionResolver resolver(
      [&fs](const std::string& p) { return fs.existing.count(p) > 0; }, policy);
  if (!PRISM_CHECK_EQ(resolver.Resolve("C:/dl/pack.txt", false),
                      std::string("C:/dl/pack (2).txt"))) return;
}

PRISM_TEST("branch B collision: a real DIRECTORY still gets the (2) suffix") {
  FakeFs fs;
  fs.existing.insert("C:/dl/Assets");
  const auto catalog = MakeCatalog({{"Assets/", true}, {"Assets/a.png", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/pack.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl/Assets (2)"));
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Assets/a.png"),
                 std::string("C:/dl/Assets (2)/a.png"));
}

PRISM_TEST("branch D: scattered roots are wrapped into an archive-named capsule") {
  FakeFs fs;
  const auto catalog = MakeCatalog({{"src/main.cpp", false}, {"README.md", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Project.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("WrapIntoNewNamedSubfolder"));
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl/Project"));
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "src/main.cpp"),
                 std::string("C:/dl/Project/src/main.cpp"));
}

PRISM_TEST("branch D: collision produces Folder (2) instead of overwriting") {
  FakeFs fs;
  fs.existing.insert("C:/dl/Project");
  const auto catalog = MakeCatalog({{"src/main.cpp", false}, {"README.md", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Project.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl/Project (2)"));
  // Negative control: a naive `out/stem` would have silently merged two
  // unrelated archives into one folder.
  PRISM_CHECK(plan.baseTargetDir != "C:/dl/Project");
}

PRISM_TEST("branch E: promo junk is dropped, main folder survives") {
  FakeFs fs;
  const auto catalog = MakeCatalog(
      {{"Crack/", true}, {"Crack/setup.exe", false}, {"Visit_Sponsor.url", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Crack.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("WrapAndFilterPromoNoise"));
  PRISM_CHECK_EQ(plan.ignoredIndices.size(), size_t(1));  // only the ad .url
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl"));
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Crack/setup.exe"),
                 std::string("C:/dl/setup.exe"));
}

PRISM_TEST("branch E can be configured to keep the main folder") {
  FakeFs fs;
  prism::PlannerOptions options;
  options.flattenPromoMainDir = false;
  const auto catalog = MakeCatalog(
      {{"Crack/", true}, {"Crack/setup.exe", false}, {"Visit_Sponsor.url", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Crack.zip", "C:/dl", catalog,
                                                         fs.Fn(), options);
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)),
                 std::string("WrapAndFilterPromoNoise"));
  PRISM_CHECK(plan.stripPrefix.empty());
  PRISM_CHECK_EQ(prism::SmartExtractionPlanner::TransformItemPath(plan, "Crack/setup.exe"),
                 std::string("C:/dl/Crack/setup.exe"));
}

PRISM_TEST("mac droppings never count as top-level roots") {
  FakeFs fs;
  // A real-world zip: one real folder + __MACOSX. Without noise filtering this
  // would be mis-detected as "two scattered roots" and needlessly wrapped.
  const auto catalog = MakeCatalog({{"Docs/", true},
                                    {"Docs/a.txt", false},
                                    {"__MACOSX/", true},
                                    {"__MACOSX/._a.txt", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Docs.zip", "C:/dl", catalog,
                                                         fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(plan.strategy)), std::string("FlatDirectly"));
  PRISM_CHECK_EQ(plan.ignoredIndices.size(), size_t(2));
  PRISM_CHECK_EQ(plan.fileCount, uint64_t(1));
}

PRISM_TEST("zip slip payload cannot escape the target root") {
  FakeFs fs;
  const auto catalog = MakeCatalog({{"../../evil.exe", false}, {"ok.txt", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/x.zip", "C:/dl", catalog,
                                                         fs.Fn());
  // Two top-level roots (".." and "ok.txt") -> wrapped into C:/dl/x first,
  // then every entry is sanitized, so the payload lands *inside* the capsule.
  const std::string target =
      prism::SmartExtractionPlanner::TransformItemPath(plan, "../../evil.exe");
  PRISM_CHECK(prism::IsContainedIn(plan.baseTargetDir, target));
  PRISM_CHECK_EQ(target, std::string("C:/dl/x/evil.exe"));
  PRISM_CHECK(!prism::IsContainedIn("C:/dl", "C:/dl/../evil.txt") ||
              !prism::IsContainedIn(plan.baseTargetDir, "../evil.txt"));
}

PRISM_TEST("empty and all-noise archives degrade gracefully") {
  FakeFs fs;
  const auto empty =
      prism::SmartExtractionPlanner::Build("C:/dl/x.zip", "C:/dl", {}, fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(empty.strategy)), std::string("EmptyArchive"));
  PRISM_CHECK_EQ(empty.baseTargetDir, std::string("C:/dl"));

  const auto catalog = MakeCatalog({{"__MACOSX/._x", false}});
  const auto noise =
      prism::SmartExtractionPlanner::Build("C:/dl/x.zip", "C:/dl", catalog, fs.Fn());
  PRISM_CHECK_EQ(std::string(prism::ToString(noise.strategy)), std::string("EmptyArchive"));
}

PRISM_TEST("compound extension uses the outer stem for the capsule name") {
  FakeFs fs;
  const auto catalog = MakeCatalog({{"a.bin", false}, {"b.bin", false}});
  const auto plan = prism::SmartExtractionPlanner::Build("C:/dl/Assets.tar.gz", "C:/dl",
                                                         catalog, fs.Fn());
  PRISM_CHECK_EQ(plan.baseTargetDir, std::string("C:/dl/Assets"));
}

}  // namespace

PRISM_TEST_MAIN()
