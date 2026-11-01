// PrismArchive -- card C36 self-check: the scratch-directory policy the Windows
// suites share (Tests/win/UniqueTempDir.hpp).
//
// The parallel lane is only as safe as the naming rule, so the rule is asserted
// instead of assumed:
//
//   1. the leaf carries card + pid + counter, and the directory really exists;
//   2. two allocations in one process differ;
//   3. 8 threads x 24 allocations produce 192 DISTINCT directories -- this is
//      the exact hazard that broke "ctest -j 8" (eight suites at one instant in
//      one shared %TEMP% path);
//   4. the pid field is this process, so a leftover from a crashed run can
//      never be adopted by a later one (no stale-green);
//   5. cleanup is RAII: normal exit, early return and an exception all remove
//      the tree, including a read-only payload;
//   6. stale sweeping deletes a dead process's debris and NEVER touches a live
//      process's directory (that would re-create the race this card removes).
//
// Negative control (CONTRACTS D3): this file is also built with
// PRISM_C36_MUTANT_SHARED_DIR, which makes UniquePath ignore pid + counter.
// That build must go RED (check 2/3 fail) and it prints "C36 MUTANT ACTIVE" so
// the injection proves it was really compiled in, not just declared.
#include <windows.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"

namespace fs = std::filesystem;
namespace tt = prism::testtmp;

namespace {

constexpr wchar_t kCard[] = L"c36selftest";
constexpr wchar_t kSweepCard[] = L"c36sweep";

bool WriteSmallFile(const fs::path& path) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out << "fixture payload";
  return static_cast<bool>(out);
}

}  // namespace

PRISM_TEST("c36.unique_tempdir: leaf carries card + pid + counter") {
  // The clean control states the absence of the D3 injection out loud; the
  // mutant binary (PRISM_C36_MUTANT_SHARED_DIR) fails here first and prints its
  // "C36 MUTANT ACTIVE" banner, so neither build can be mistaken for the other.
  PRISM_CHECK(!tt::InjectionArmed());
  const tt::UniqueDir dir(kCard, L"shape");
  if (!PRISM_CHECK(dir.ok())) {
    std::cout << "    why: " << dir.error() << std::endl;
    return;
  }
  PRISM_CHECK(dir.Exists());
  const std::wstring leaf = dir.path().filename().wstring();
  // Card prefix: debris in %TEMP% stays attributable.
  PRISM_CHECK(leaf.rfind(std::wstring(L"prism_") + kCard, 0) == 0);
  // Pid field: this process, and this process only.
  const std::wstring pidField =
      L"_p" + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L"_";
  if (!PRISM_CHECK(leaf.find(pidField) != std::wstring::npos)) {
    std::wcout << L"    leaf: " << leaf << std::endl;
  }
  // Counter field: the leaf ends with _<digits>.
  PRISM_CHECK(!leaf.empty() && leaf.back() >= L'0' && leaf.back() <= L'9');
  // It is a real, writable directory -- not just a string.
  PRISM_CHECK(::WriteSmallFile(dir.path() / L"probe.bin"));
}

PRISM_TEST("c36.unique_tempdir: two allocations in one process differ") {
  const tt::UniqueDir first(kCard, L"pair");
  const tt::UniqueDir second(kCard, L"pair");
  if (!PRISM_CHECK(first.ok() && second.ok())) return;
  PRISM_CHECK(first.path() != second.path());
  PRISM_CHECK(first.Exists() && second.Exists());
}

// THE hazard: eight suites (here: eight threads) allocating at one instant.
PRISM_TEST("c36.unique_tempdir: 8 threads x 24 allocations are 192 distinct dirs") {
  constexpr int kThreads = 8;
  constexpr int kPerThread = 24;
  std::mutex guard;
  std::set<std::wstring> leaves;
  std::atomic<int> created{0};
  std::atomic<int> failed{0};

  std::vector<std::thread> workers;
  workers.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    workers.emplace_back([&] {
      for (int i = 0; i < kPerThread; ++i) {
        const tt::UniqueDir dir(kCard, L"burst");
        if (!dir.ok()) {
          ++failed;
          continue;
        }
        {
          std::lock_guard<std::mutex> lock(guard);
          leaves.insert(dir.path().wstring());
        }
        ++created;
      }
    });
  }
  for (std::thread& worker : workers) worker.join();

  PRISM_CHECK_EQ(failed.load(), 0);
  PRISM_CHECK_EQ(created.load(), kThreads * kPerThread);
  if (!PRISM_CHECK_EQ(leaves.size(), static_cast<size_t>(kThreads * kPerThread))) {
    std::cout << "    two allocations shared a directory: the naming rule lost the pid or the "
                 "counter (this is the pre-C36 state that broke ctest -j 8)"
              << std::endl;
  }
}

PRISM_TEST("c36.unique_tempdir: RAII removes on normal exit, early return and throw") {
  // (a) normal scope exit
  fs::path normal;
  {
    const tt::UniqueDir dir(kCard, L"normal");
    if (!PRISM_CHECK(dir.ok())) return;
    normal = dir.path();
    PRISM_CHECK(::WriteSmallFile(normal / L"payload.bin"));
  }
  PRISM_CHECK(!fs::exists(normal));

  // (b) the error path: the case bails out with the fixture half written
  fs::path early;
  const auto failingCase = [&]() -> bool {
    const tt::UniqueDir dir(kCard, L"early");
    if (!dir.ok()) return true;
    early = dir.path();
    if (!::WriteSmallFile(early / L"half-authored.bin")) return true;
    return false;  // the assertion that failed
  };
  PRISM_CHECK(!failingCase());
  PRISM_CHECK(!fs::exists(early));

  // (c) an exception while the fixture is being authored
  fs::path thrown;
  bool caught = false;
  try {
    const tt::UniqueDir dir(kCard, L"thrown");
    thrown = dir.path();
    if (!::WriteSmallFile(thrown / L"in-flight.bin")) {
      throw std::runtime_error("fixture write failed");
    }
    throw std::runtime_error("fixture exploded mid-authoring");
  } catch (const std::exception& ex) {
    caught = true;
    PRISM_CHECK(std::string(ex.what()).find("exploded") != std::string::npos);
  }
  PRISM_CHECK(caught);
  PRISM_CHECK(!fs::exists(thrown));

  // (d) an explicit RemoveNow is idempotent and the destructor stays a no-op
  {
    tt::UniqueDir dir(kCard, L"explicit");
    if (!PRISM_CHECK(dir.ok())) return;
    const fs::path path = dir.path();
    PRISM_CHECK(dir.RemoveNow());
    PRISM_CHECK(!fs::exists(path));
    PRISM_CHECK(!dir.Exists());
  }
}

// A read-only payload makes remove_all stop halfway on Windows; the header has
// to clear the attribute or every run leaves a fixture behind.
PRISM_TEST("c36.unique_tempdir: a read-only payload does not survive cleanup") {
  fs::path path;
  {
    const tt::UniqueDir dir(kCard, L"readonly");
    if (!PRISM_CHECK(dir.ok())) return;
    path = dir.path();
    const fs::path file = path / L"locked.bin";
    PRISM_CHECK(::WriteSmallFile(file));
    if (!PRISM_CHECK(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY) != 0)) return;
  }
  PRISM_CHECK(!fs::exists(path));
}

PRISM_TEST("c36.unique_tempdir: stale sweep spares a live process's directory") {
  const fs::path root = tt::ScratchRoot();
  std::error_code ec;
  // A pid that cannot be alive: OpenProcess fails for it, so its debris is
  // exactly what a crashed run leaves behind.
  const std::wstring deadLeaf =
      std::wstring(L"prism_") + kSweepCard + L"_leak_p4294967280_1";
  const std::wstring liveLeaf =
      std::wstring(L"prism_") + kSweepCard + L"_leak_p" +
      std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())) + L"_1";
  const fs::path dead = root / deadLeaf;
  const fs::path live = root / liveLeaf;
  fs::create_directories(dead / L"sub", ec);
  fs::create_directories(live / L"sub", ec);
  if (!PRISM_CHECK(!ec)) return;
  PRISM_CHECK(::WriteSmallFile(dead / L"sub" / L"orphan.bin"));
  PRISM_CHECK(::WriteSmallFile(live / L"sub" / L"live.bin"));

  const size_t removed = tt::SweepStale(kSweepCard);
  PRISM_CHECK_EQ(removed, static_cast<size_t>(1));
  PRISM_CHECK(!fs::exists(dead));
  // The live pid's directory belongs to a concurrent run: touching it would
  // re-create exactly the race this card exists to remove.
  PRISM_CHECK(fs::exists(live / L"sub" / L"live.bin"));

  std::error_code ignored;
  fs::remove_all(live, ignored);
  PRISM_CHECK(!fs::exists(live));
}

int main(int argc, char** argv) {
#if defined(PRISM_C36_MUTANT_SHARED_DIR)
  std::printf("C36 MUTANT ACTIVE: SHARED DIR -- UniquePath ignores pid+counter\n");
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) {
    std::printf("C36 TEMP-DIR CHECKS FAILED\n");
    return rc;
  }
  // The marker is a CONJUNCTION with the exit code (CONTRACTS D1): it can only
  // be printed by a green run, so a crash after printing still fails.
  std::printf("ALL C36 TEMP-DIR CHECKS PASSED\n");
  return 0;
}
