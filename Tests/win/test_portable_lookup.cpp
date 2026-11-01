// Tests/win/test_portable_lookup.cpp -- card C37: PORTABLE 7-Zip discovery.
//
// The defect this card exists for (measured on the dev box, 2026-11-01): 7-Zip
// unpacked into E:\DevTools\7-Zip and NEVER installed. No registry key, nothing
// under Program Files, so C1's ladder (env -> exe dir -> registry -> the two
// Program Files probes) resolved nothing: PrismArchive.exe exited 0 with no
// artifact, and the "7z.dll not found" box listed three dead ends.
//
// Asserted here:
//   1. the documented ladder as DATA -- env -> app-dir -> registry -> PATH ->
//      common dirs -> Program Files, in that order, every path labelled with
//      its source (a report you cannot act on is not a diagnosis, D12);
//   2. a 7z.dll in a directory listed on PATH is FOUND, source == "path";
//   3. two PATH entries that both carry 7z.dll resolve to the FIRST in PATH;
//   4. the common per-user layouts (%LOCALAPPDATA%\Programs\7-Zip, scoop) are
//      searched after PATH and before the Program Files probes;
//   5. the real Resolve7zDll() really runs the PATH rung and ends with the two
//      probes;
//   6. a 7z.dll with no 7z.exe beside it says WHICH half is missing (the
//      integration fixtures AUTHOR their archives with the CLI).
//
// No SKIP: every case runs on a machine with no 7-Zip at all. The discovery
// layer only stats files, so a placeholder 7z.dll proves what a real one proves
// (a real one is copied whenever this machine has one).
//
// D3/D26 negative control: compiled a second time with
// PRISM_C37_MUTANT_NO_PATH_SCAN (the PATH rung removed). Cases 1-5 must go red,
// and the binary prints "C37 MUTATION CAUGHT: no_path_scan" ONLY when it also
// MEASURED zero PATH-sourced candidates -- proof the injection bit, not merely
// that it compiled. The marker is the target's PASS_REGULAR_EXPRESSION; there is
// no WILL_FAIL anywhere (D26: WILL_FAIL with a pass marker is always red, and
// WILL_FAIL alone turns a SKIP into a green).
#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "advapi32.lib")  // RegOpenKeyExW inside Resolve7zDll

namespace {

namespace fs = std::filesystem;

using prism::win::JoinFsPath;
using prism::win::Resolve7zDll;
using prism::win::Resolve7zDllFrom;
using prism::win::SevenZipDllEnvironment;
using prism::win::SevenZipDllLookup;

constexpr std::size_t kMissing = static_cast<std::size_t>(-1);
constexpr wchar_t kProbe64[] = L"C:\\Program Files\\7-Zip\\7z.dll";
constexpr wchar_t kProbe86[] = L"C:\\Program Files (x86)\\7-Zip\\7z.dll";

#ifdef PRISM_C37_MUTANT_NO_PATH_SCAN
constexpr bool kNoPathScanMutant = true;
#else
constexpr bool kNoPathScanMutant = false;
#endif

// Unique per PROCESS and per CARD (C37): a shared %TEMP% folder is how two cards
// running in parallel delete each other's fixtures.
fs::path UniqueRoot() {
  return fs::temp_directory_path() /
         (L"prism_c37_lookup_" + std::to_wstring(GetCurrentProcessId()));
}

struct TempTree {
  fs::path root = UniqueRoot();
  TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
  }
  ~TempTree() {
    std::error_code ec;
    fs::remove_all(root, ec);
  }
  TempTree(const TempTree&) = delete;
  TempTree& operator=(const TempTree&) = delete;
};

class ScopedEnv {
 public:
  explicit ScopedEnv(const wchar_t* name) : name_(name) {
    wchar_t buffer[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(name, buffer, ARRAYSIZE(buffer));
    if (length > 0 && length < ARRAYSIZE(buffer)) value_.assign(buffer);
  }
  ~ScopedEnv() { SetEnvironmentVariableW(name_, value_.empty() ? nullptr : value_.c_str()); }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

  void Clear() const { SetEnvironmentVariableW(name_, nullptr); }
  bool Set(const std::wstring& value) const {
    return SetEnvironmentVariableW(name_, value.c_str()) != 0;
  }

 private:
  const wchar_t* name_;
  std::wstring value_;
};

bool MakeDir(const fs::path& dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  return !ec;
}

bool WritePlaceholder(const std::wstring& path) {
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return false;
  const char bytes[4] = {'7', 'z', '!', '!'};
  std::fwrite(bytes, 1, sizeof(bytes), file);
  std::fclose(file);
  return true;
}

// A real 7z.dll when this machine has one -- that is the honest fixture, a
// portable 7-Zip really IS a dll sitting in a folder -- and a 4-byte placeholder
// when it does not: discovery only asks GetFileAttributes, it never loads.
bool Plant7zDll(const fs::path& dir) {
  if (!MakeDir(dir)) return false;
  const std::wstring target = JoinFsPath(dir.wstring(), L"7z.dll");
  std::error_code ec;
  for (const std::wstring& source : {prism::fixture::DllPath(), Resolve7zDll().path}) {
    if (source.empty() || !prism::win::ExistsAsFile(source)) continue;
    fs::copy_file(fs::path(source), fs::path(target), fs::copy_options::overwrite_existing, ec);
    if (!ec) return true;
    ec.clear();
  }
  return WritePlaceholder(target);
}

std::size_t IndexOf(const SevenZipDllLookup& lookup, const std::wstring& path) {
  for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
    if (lookup.candidates[i] == path) return i;
  }
  return kMissing;
}

// A failure here has to be diagnosable from the log alone: dump what the search
// actually saw, with sources, exactly as the message box would.
void DumpLookup(const char* why, const SevenZipDllLookup& lookup) {
  std::printf("    %s; %zu candidate(s) were tried:\n", why, lookup.candidates.size());
  for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
    const std::string source =
        i < lookup.candidateSources.size() ? prism::win::WideToUtf8(lookup.candidateSources[i])
                                           : std::string("?");
    const std::string path = prism::win::WideToUtf8(lookup.candidates[i]);
    std::printf("      [%s] %s\n", source.c_str(), path.c_str());
  }
}

// The controlled environment used by case 1: every rung filled with a path that
// cannot exist, so the ORDER is what is being measured, not this machine.
SevenZipDllEnvironment LadderEnvironment(const fs::path& root) {
  SevenZipDllEnvironment env;
  env.envOverride = JoinFsPath((root / L"override").wstring(), L"7z.dll");
  env.exeDir = (root / L"app").wstring();
  env.registryDirs.emplace_back((root / L"reg").wstring(), L"registry:HKLM");
  env.pathValue = (root / L"pathA").wstring() + L";" + (root / L"pathB").wstring();
  env.localAppData = (root / L"lapp").wstring();
  env.userProfile = (root / L"profile").wstring();
  env.includeProgramFilesProbes = true;
  return env;
}

std::wstring LadderDll(const fs::path& root, const wchar_t* leaf) {
  return JoinFsPath((root / leaf).wstring(), L"7z.dll");
}

// ---------------------------------------------------------------------------
// 1. the ladder itself
// ---------------------------------------------------------------------------
PRISM_TEST("C37: the ladder is env -> app-dir -> registry -> PATH -> common -> Program Files") {
  TempTree tree;
  const SevenZipDllEnvironment env = LadderEnvironment(tree.root);
  const SevenZipDllLookup lookup = Resolve7zDllFrom(env);
  if (!PRISM_CHECK_EQ(lookup.candidates.size(), lookup.candidateSources.size())) return;

  // Nothing under the fake root exists, so the only legal winner is a probe --
  // or nothing at all on a machine without a Program Files 7-Zip.
  if (!PRISM_CHECK(lookup.path.empty() || lookup.path == kProbe64 || lookup.path == kProbe86)) {
    DumpLookup("a rung that cannot exist won the search", lookup);
    return;
  }

  const std::wstring dllA = LadderDll(tree.root, L"pathA");
  const std::wstring dllB = LadderDll(tree.root, L"pathB");
  const std::wstring laps =
      JoinFsPath(JoinFsPath(JoinFsPath(env.localAppData, L"Programs"), L"7-Zip"), L"7z.dll");
  const std::wstring scoop = JoinFsPath(
      JoinFsPath(JoinFsPath(JoinFsPath(JoinFsPath(env.userProfile, L"scoop"), L"apps"), L"7zip"),
                 L"current"),
      L"7z.dll");

  const std::size_t iEnv = IndexOf(lookup, env.envOverride);
  const std::size_t iApp = IndexOf(lookup, JoinFsPath(env.exeDir, L"7z.dll"));
  const std::size_t iReg = IndexOf(lookup, JoinFsPath(env.registryDirs.front().first, L"7z.dll"));
  const std::size_t iPathA = IndexOf(lookup, dllA);
  const std::size_t iPathB = IndexOf(lookup, dllB);
  const std::size_t iLaps = IndexOf(lookup, laps);
  const std::size_t iScoop = IndexOf(lookup, scoop);
  const std::size_t iProbe64 = IndexOf(lookup, kProbe64);
  const std::size_t iProbe86 = IndexOf(lookup, kProbe86);
  if (!PRISM_CHECK(iEnv != kMissing && iApp != kMissing && iReg != kMissing &&
                   iPathA != kMissing && iPathB != kMissing && iLaps != kMissing &&
                   iScoop != kMissing && iProbe64 != kMissing && iProbe86 != kMissing)) {
    DumpLookup("a rung of the ladder is missing from the report", lookup);
    return;
  }

  // Strictly increasing = the documented order. One assertion, because the
  // contract IS the whole sequence.
  const bool ordered = iEnv < iApp && iApp < iReg && iReg < iPathA && iPathA < iPathB &&
                       iPathB < iLaps && iLaps < iScoop && iScoop < iProbe64 && iProbe64 < iProbe86;
  if (!PRISM_CHECK(ordered)) {
    DumpLookup("order broken: expected env, app-dir, registry, PATH, common dirs, probes", lookup);
    return;
  }

  // Every line says where it came from (C37, task 2).
  if (!PRISM_CHECK(lookup.candidateSources[iEnv] == L"env")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iApp] == L"app-dir")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iReg] == L"registry:HKLM")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iPathA] == L"path")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iPathB] == L"path")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iLaps] == L"common:LOCALAPPDATA")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iScoop] == L"common:scoop")) return;
  if (!PRISM_CHECK(lookup.candidateSources[iProbe86] == L"probe")) return;
  // The last-resort probes are the TAIL (C1's suite asserts the same on the
  // real resolver, from the other side).
  if (!PRISM_CHECK(lookup.candidates.back() == kProbe86)) return;
}

// ---------------------------------------------------------------------------
// 2. the defect: a portable 7-Zip that only PATH knows about
// ---------------------------------------------------------------------------
PRISM_TEST("C37: a 7z.dll in a PATH directory is found, and its source says path") {
  TempTree tree;
  const fs::path portable = tree.root / L"DevTools" / L"7-Zip";
  if (!PRISM_CHECK(Plant7zDll(portable))) return;
  const std::wstring found = JoinFsPath(portable.wstring(), L"7z.dll");

  SevenZipDllEnvironment env;
  env.pathValue = portable.wstring();
  env.includeProgramFilesProbes = true;  // PATH must beat the last resort
  const SevenZipDllLookup hit = Resolve7zDllFrom(env);
  if (!PRISM_CHECK(hit.path == found)) {
    DumpLookup("a portable 7z.dll listed on PATH was not picked", hit);
    return;
  }
  if (!PRISM_CHECK(hit.source == L"path")) return;

  // The hit really came from the PATH rung: with PATH cleared the file is not
  // merely unresolved, it is not even listed.
  env.pathValue.clear();
  const SevenZipDllLookup cleared = Resolve7zDllFrom(env);
  if (!PRISM_CHECK(cleared.path != found)) return;
  if (!PRISM_CHECK(IndexOf(cleared, found) == kMissing)) {
    DumpLookup("a directory that is on no PATH still appeared in the report", cleared);
    return;
  }
}

// ---------------------------------------------------------------------------
// 3. PATH order is the priority order
// ---------------------------------------------------------------------------
PRISM_TEST("C37: PATH order decides when two entries both carry 7z.dll") {
  TempTree tree;
  const fs::path first = tree.root / L"one";
  const fs::path second = tree.root / L"two";
  if (!PRISM_CHECK(Plant7zDll(first) && Plant7zDll(second))) return;

  SevenZipDllEnvironment env;
  env.pathValue = second.wstring() + L";" + first.wstring();  // "two" is first on PATH
  env.includeProgramFilesProbes = false;
  const SevenZipDllLookup hit = Resolve7zDllFrom(env);
  const std::wstring expected = JoinFsPath(second.wstring(), L"7z.dll");
  if (!PRISM_CHECK(hit.path == expected)) {
    DumpLookup("the FIRST directory on PATH did not win", hit);
    return;
  }
  if (!PRISM_CHECK(IndexOf(hit, expected) < IndexOf(hit, JoinFsPath(first.wstring(), L"7z.dll")))) {
    return;
  }
}

// ---------------------------------------------------------------------------
// 4. common per-user layouts, still after PATH
// ---------------------------------------------------------------------------
PRISM_TEST("C37: %LOCALAPPDATA% and scoop layouts are searched after PATH, before the probes") {
  TempTree tree;
  const fs::path localAppData = tree.root / L"lapp";
  if (!PRISM_CHECK(Plant7zDll(localAppData / L"Programs" / L"7-Zip"))) return;

  SevenZipDllEnvironment env;
  env.localAppData = localAppData.wstring();
  env.userProfile = (tree.root / L"profile").wstring();
  env.includeProgramFilesProbes = false;
  const SevenZipDllLookup hit = Resolve7zDllFrom(env);
  const std::wstring expected =
      JoinFsPath((localAppData / L"Programs" / L"7-Zip").wstring(), L"7z.dll");
  if (!PRISM_CHECK(hit.path == expected)) {
    DumpLookup("%LOCALAPPDATA%\\Programs\\7-Zip was not searched", hit);
    return;
  }
  if (!PRISM_CHECK(hit.source == L"common:LOCALAPPDATA")) return;

  // PATH outranks it when both have a dll.
  const fs::path portable = tree.root / L"portable";
  if (!PRISM_CHECK(Plant7zDll(portable))) return;
  env.pathValue = portable.wstring();
  const SevenZipDllLookup both = Resolve7zDllFrom(env);
  if (!PRISM_CHECK(both.path == JoinFsPath(portable.wstring(), L"7z.dll"))) {
    DumpLookup("PATH lost to the common dirs", both);
    return;
  }

  // The scoop layout: %USERPROFILE%\scoop\apps\7zip\current.
  const fs::path profile = tree.root / L"profile2";
  if (!PRISM_CHECK(Plant7zDll(profile / L"scoop" / L"apps" / L"7zip" / L"current"))) return;
  SevenZipDllEnvironment scoopEnv;
  scoopEnv.userProfile = profile.wstring();
  scoopEnv.includeProgramFilesProbes = false;
  const SevenZipDllLookup scoop = Resolve7zDllFrom(scoopEnv);
  if (!PRISM_CHECK(scoop.source == L"common:scoop")) {
    DumpLookup("the scoop layout was not searched", scoop);
    return;
  }
}

// ---------------------------------------------------------------------------
// 5. the real entry point really runs this ladder
// ---------------------------------------------------------------------------
PRISM_TEST("C37: the real Resolve7zDll() searches PATH and ends with the two probes") {
  ScopedEnv override(L"PRISM_7Z_DLL");
  override.Clear();  // an override would (correctly) short-circuit the report

  const SevenZipDllLookup lookup = Resolve7zDll();
  if (!PRISM_CHECK_EQ(lookup.candidates.size(), lookup.candidateSources.size())) return;
  std::size_t pathRung = 0;
  for (const std::wstring& source : lookup.candidateSources) pathRung += source == L"path" ? 1 : 0;
  if (!PRISM_CHECK(pathRung >= 1)) {
    DumpLookup("PATH is never empty on Windows, so the PATH rung ran zero directories", lookup);
    return;
  }
  if (!PRISM_CHECK(lookup.candidates.back() == kProbe86)) {
    DumpLookup("the last candidate is not the x86 last-resort probe", lookup);
    return;
  }
  if (!PRISM_CHECK(lookup.candidateSources.back() == L"probe")) return;
}

// ---------------------------------------------------------------------------
// 6. the hint when the engine is there and the CLI is not
// ---------------------------------------------------------------------------
PRISM_TEST("C37: a 7z.dll with no 7z.exe says which half is missing") {
  TempTree tree;
  const fs::path dllOnly = tree.root / L"dll-only";
  if (!PRISM_CHECK(Plant7zDll(dllOnly))) return;
  const std::wstring dll = JoinFsPath(dllOnly.wstring(), L"7z.dll");

  ScopedEnv override(L"PRISM_7Z_DLL");
  if (!PRISM_CHECK(override.Set(dll))) return;

  // Precondition: the scenario is real -- the engine is resolvable, the CLI is
  // absent.
  if (!PRISM_CHECK(prism::win::ExistsAsFile(dll))) return;
  if (!PRISM_CHECK(prism::fixture::ExePath().empty())) return;

  const std::string why = prism::fixture::ExePathWhy();
  if (!PRISM_CHECK(!why.empty())) return;
  if (!PRISM_CHECK(why.find("7z.exe") != std::string::npos)) return;
  if (!PRISM_CHECK(why.find("MISSING") != std::string::npos)) return;
  if (!PRISM_CHECK(why.find("integration") != std::string::npos)) return;
  if (!PRISM_CHECK(why.find(prism::win::WideToUtf8(dllOnly.wstring())) != std::string::npos)) {
    std::printf("    hint does not name the directory that lacks the CLI: %s\n", why.c_str());
    return;
  }

  // ... and with the CLI beside it the hint goes away instead of going stale.
  if (!PRISM_CHECK(WritePlaceholder(JoinFsPath(dllOnly.wstring(), L"7z.exe")))) return;
  if (!PRISM_CHECK(prism::fixture::ExePath() == JoinFsPath(dllOnly.wstring(), L"7z.exe"))) return;
  if (!PRISM_CHECK(prism::fixture::ExePathWhy().empty())) return;
}

// ---------------------------------------------------------------------------
// The D3 proof that does not trust the suite: a search whose ONLY rung is PATH
// must yield PATH candidates. A clean build sees two; the mutant must see none,
// otherwise the injection did not bite and "CAUGHT" would be a lie.
// ---------------------------------------------------------------------------
std::size_t PathRungCandidates() {
  SevenZipDllEnvironment env;
  env.pathValue = L"C:\\c37-probe\\one;C:\\c37-probe\\two";
  env.includeProgramFilesProbes = false;
  std::size_t count = 0;
  for (const std::wstring& source : Resolve7zDllFrom(env).candidateSources) {
    count += source == L"path" ? 1 : 0;
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  if (kNoPathScanMutant) {
    std::puts(
        "C37 MUTANT ACTIVE: PRISM_C37_MUTANT_NO_PATH_SCAN -- the PATH rung of Resolve7zDllFrom "
        "is compiled out (a portable 7-Zip is invisible again)");
  }
  const std::size_t pathRung = PathRungCandidates();
  const int rc = prism::test::RunAll(argc, argv);

  if (kNoPathScanMutant) {
    // CONTRACTS D26: the verdict is a CONJUNCTION printed here, never WILL_FAIL.
    if (rc == 0) {
      std::printf(
          "C37 MUTATION NOT CAUGHT: the suite stayed green with the PATH rung compiled out -- "
          "the injection is stale\n");
      return 1;
    }
    if (pathRung != 0) {
      std::printf(
          "C37 MUTATION NOT ARMED: %zu PATH candidate(s) still resolved -- the injection "
          "compiled but did not bite\n",
          pathRung);
      return 1;
    }
    std::printf(
        "C37 MUTATION CAUGHT: no_path_scan (suite red above; the PATH rung measured 0 "
        "candidates)\n");
    return 0;
  }

  if (pathRung == 0) {
    std::printf("FAIL: the clean build's PATH rung produced 0 candidates -- PATH discovery is "
                "gone\n");
    return 1;
  }
  if (rc != 0) return rc;
  std::printf("ALL PORTABLE-LOOKUP CHECKS PASSED\n");
  return 0;
}
