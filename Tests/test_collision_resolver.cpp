// TDD: extraction-time collision policy (blueprint ch. 4.2 / 7.1).
//
// First principles:
//  (a) never silently overwrite a user file,
//  (b) behave deterministically when the ARCHIVE ITSELF contains entries that
//      map to the same target -- names must be unique even though nothing has
//      touched the disk yet (the plan set),
//  (c) directory collisions MERGE, files RENAME -- the caller says which is which,
//  (d) case-insensitivity of the *plan* is ours; the disk's own case rules are
//      the filesystem's job (Windows/macOS exists() already folds case),
//  (e) give up loudly after a bounded number of renames instead of spinning
//      "name (37).txt" chains.
#include "PathSafety.hpp"
#include "mini_test.hpp"

#include <set>
#include <string>

namespace {

std::set<std::string> MakeSet(std::initializer_list<std::string> items) {
  return std::set<std::string>(items);
}

prism::CollisionResolver MakeResolver(const std::set<std::string>& onDisk) {
  // capture by value: the lambda outlives this frame inside the returned resolver
  return prism::CollisionResolver(
      [onDisk](const std::string& p) { return onDisk.count(p) > 0; });
}

PRISM_TEST("no conflict passes through unchanged and reserves the name") {
  auto r = MakeResolver({});
  PRISM_CHECK_EQ(r.Resolve("C:/out/logo.png", false), std::string("C:/out/logo.png"));
  // The reservation must stick: a second identical archive entry may NOT reuse it.
  PRISM_CHECK_EQ(r.Resolve("C:/out/logo.png", false), std::string("C:/out/logo (2).png"));
}

PRISM_TEST("existing file gets ' (n)' inserted before the extension") {
  auto r = MakeResolver({"/out/readme.txt"});
  PRISM_CHECK_EQ(r.Resolve("/out/readme.txt", false), std::string("/out/readme (2).txt"));
  PRISM_CHECK_EQ(r.Resolve("/out/readme.txt", false), std::string("/out/readme (3).txt"));
}

PRISM_TEST("stem without extension also collides safely") {
  auto r = MakeResolver({"/out/LICENSE"});
  PRISM_CHECK_EQ(r.Resolve("/out/LICENSE", false), std::string("/out/LICENSE (2)"));
}

PRISM_TEST("planned names collide case-insensitively by default") {
  // Real-world case: one archive ships both "Readme.TXT" and "readme.txt".
  auto r = MakeResolver({});
  PRISM_CHECK_EQ(r.Resolve("/out/Readme.TXT", false), std::string("/out/Readme.TXT"));
  PRISM_CHECK_EQ(r.Resolve("/out/readme.txt", false), std::string("/out/readme (2).txt"));
}

PRISM_TEST("case-sensitive mode treats casing as distinct") {
  prism::CollisionPolicy policy;
  policy.caseInsensitive = false;
  auto r = prism::CollisionResolver([](const std::string&) { return false; }, policy);
  PRISM_CHECK_EQ(r.Resolve("/out/Readme.TXT", false), std::string("/out/Readme.TXT"));
  PRISM_CHECK_EQ(r.Resolve("/out/readme.txt", false), std::string("/out/readme.txt"));
}

PRISM_TEST("directory collisions merge instead of renaming") {
  auto r = MakeResolver({"/out/Assets"});
  PRISM_CHECK_EQ(r.Resolve("/out/Assets", true), std::string("/out/Assets"));
  // ...and a file under the merged directory keeps its own resolution.
  PRISM_CHECK_EQ(r.Resolve("/out/Assets/logo.png", false), std::string("/out/Assets/logo.png"));
}

PRISM_TEST("planned siblings do not leak across directories") {
  auto r = MakeResolver({});
  PRISM_CHECK_EQ(r.Resolve("/out/a/config.ini", false), std::string("/out/a/config.ini"));
  PRISM_CHECK_EQ(r.Resolve("/out/b/config.ini", false), std::string("/out/b/config.ini"));
}

PRISM_TEST("gives up loudly when every candidate is taken") {
  prism::CollisionPolicy policy;
  policy.maxAttempts = 2;  // suffixes (2)..(3) are the ceiling for one entry
  auto r = prism::CollisionResolver([](const std::string&) { return true; }, policy);
  PRISM_CHECK(r.Resolve("/out/x.bin", false).empty());
  PRISM_CHECK(r.error());
}

PRISM_TEST("suffix chain respects the attempt ceiling") {
  prism::CollisionPolicy policy;
  policy.maxAttempts = 2;  // allowed suffixes: (2), (3) -- nothing beyond
  const auto onDisk = MakeSet(
      {"/out/x.bin", "/out/x (2).bin", "/out/x (3).bin", "/out/x (4).bin"});
  auto r = prism::CollisionResolver(
      [&onDisk](const std::string& p) { return onDisk.count(p) > 0; }, policy);
  PRISM_CHECK_EQ(r.Resolve("/out/x.bin", false), std::string(""));
  PRISM_CHECK(r.error());
}

PRISM_TEST("archived duplicates all get distinct names before any write") {
  auto r = MakeResolver({});
  std::set<std::string> seen;
  for (int i = 0; i < 4; ++i) {
    const std::string p = r.Resolve("/out/photo.jpg", false);
    PRISM_CHECK(!p.empty());
    PRISM_CHECK(seen.insert(p).second);  // every name unique
  }
}

}  // namespace

PRISM_TEST_MAIN()
