// Windows integration test (batch B3): the password probe executor against
// the REAL 7z.dll, on a genuine AES-256 encrypted-header archive authored by
// the 7-Zip CLI that ships beside 7z.dll on the runner.
//
// What only a real engine can prove here:
//   * -mhe=on locks the catalog behind the password => StrategyFor must be
//     FullOpen, and an empty-password Open must fail (fixture sanity);
//   * TryOpenWith7z answers true for EXACTLY the right password (AES key
//     derivation verdicts, not a mock);
//   * hit-at-position-4 and full-exhaustion counts with real KDF timing.
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "PasswordProbe.hpp"
#include "PasswordRing.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

namespace {

std::wstring g_dllPath;
std::wstring g_exePath;

// Per-test scratch dir under %TEMP%; removed by the caller when it still
// exists (an open 7z handle would make remove_all throw, so keep readers
// scoped tightly).
std::wstring MakeTempDir(const wchar_t* tag) {
  wchar_t temp[MAX_PATH];
  DWORD n = GetTempPathW(MAX_PATH, temp);
  if (n == 0 || n >= MAX_PATH) return {};
  std::wstring dir = std::wstring(temp) + L"prism_pwd_" + std::wstring(tag) +
                     L"_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

bool WriteSmallFile(const std::wstring& dir, const std::wstring& name,
                    const std::string& content) {
  std::ofstream out(std::filesystem::path(dir) / std::filesystem::path(name),
                    std::ios::binary);
  if (!out) return false;
  out << content;
  return static_cast<bool>(out);
}

bool RemoveDir(const std::wstring& dir) {
  std::error_code ec;
  std::filesystem::remove_all(std::filesystem::path(dir), ec);
  return !ec;
}

// The 12-candidate sweep list: the right password sits at position 4 in case
// 1 and is swapped out for a 12th decoy in case 2 (all wrong).
std::vector<std::string> CandidateWords(const std::string& hit) {
  std::vector<std::string> words = {
      "left01xx",  "left02xx",  "left03xx",  hit,         "left05xx",
      "left06xx",  "left07xx",  "left08xx",  "left09xx",  "left10xx",
      "left11xx",  "left12xx"};
  if (hit.empty()) words[3] = "left04xx";
  return words;
}

prism::pwd::PasswordRing::AttemptPlan PlanFromWords(const std::vector<std::string>& words,
                                                    int concurrency) {
  prism::pwd::PasswordRing::AttemptPlan plan;
  plan.concurrency = concurrency;
  for (const std::string& w : words) plan.ordered.push_back({w});
  return plan;
}

}  // namespace

int main() {
  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  // No SKIP: 7z.exe beside 7z.dll is part of the windows-latest contract, and
  // a missing engine must be a RED, not a silently green no-op (CONTRACTS D1).
  if (!PRISM_CHECK(!g_dllPath.empty())) return 1;
  if (!PRISM_CHECK(!g_exePath.empty())) return 1;

  const std::wstring kPassword = L"Right04#2024";   // CLI authoring (wide)
  const std::string kPasswordUtf8 = "Right04#2024"; // candidate list (narrow)

  const std::wstring dir = MakeTempDir(L"auth");
  if (!PRISM_CHECK(!dir.empty())) return 1;
  if (!PRISM_CHECK(WriteSmallFile(dir, L"alpha.txt", "alpha payload\n"))) return 1;
  if (!PRISM_CHECK(WriteSmallFile(dir, L"beta.txt", "beta payload 1984\n"))) return 1;
  if (!PRISM_CHECK(WriteSmallFile(dir, L"gamma.txt", "gamma\n"))) return 1;

  // Genuine AES-256 + encrypted-header 7z (-mhe=on): header encryption is the
  // format fact that forces the FullOpen strategy.
  const std::wstring archive = dir + L"\\secret.7z";
  const bool packed = prism::fixture::RunSevenZip(
      g_exePath, L"a -t7z -p" + std::wstring(kPassword) +
                     L" -mhe=on \"" + archive + L"\" \"" + dir + L"\\alpha.txt\" \"" +
                     dir + L"\\beta.txt\" \"" + dir + L"\\gamma.txt\"");
  if (!PRISM_CHECK(packed)) return 1;
  if (!PRISM_CHECK(std::filesystem::exists(std::filesystem::path(archive)))) return 1;

  // ---- Fixture sanity: header-encrypted => no password, no catalog --------
  {
    prism::win::SevenZipLibrary library;
    if (!PRISM_CHECK(library.Load(g_dllPath))) return 1;
    prism::win::ArchiveReader anonymous(library);
    // 7z refuses to even parse an encrypted header without the password, so
    // Open must fail -- this is what makes headerEncrypted=true observable.
    PRISM_CHECK(!anonymous.Open(archive));
    // Decision core: header-encrypted archives are FullOpen, never BodyProbe;
    // an empty candidate ring is Decline (loud, with a reason).
    PRISM_CHECK(prism::pwd::PasswordRing::StrategyFor(true, 12) ==
                prism::pwd::PasswordRing::ProbeStrategy::FullOpen);
    PRISM_CHECK(prism::pwd::PasswordRing::StrategyFor(true, 0) ==
                prism::pwd::PasswordRing::ProbeStrategy::Decline);
    // The 12-attempt budget the exhaustion case asserts is the AES one.
    // NOTE: SilentAttemptBudget/KdfClassOf live in `prism` (PasswordVault),
    // not `prism::pwd`.
    PRISM_CHECK(prism::SilentAttemptBudget(prism::KdfClassOf("secret.7z")) == 12);
  }

  // ---- Thin bridge: true only for the real password -----------------------
  PRISM_CHECK(prism::win::pwd::TryOpenWith7z(archive, kPasswordUtf8));
  PRISM_CHECK(!prism::win::pwd::TryOpenWith7z(archive, "left01xx"));
  PRISM_CHECK(!prism::win::pwd::TryOpenWith7z(archive, ""));

  auto tryOpen = [&archive](const std::string& pw) {
    return prism::win::pwd::TryOpenWith7z(archive, pw);
  };

  // ---- Case 1: hit at position 4, sequential plan ------------------------
  {
    const prism::pwd::PasswordRing::AttemptPlan plan = PlanFromWords(CandidateWords(kPasswordUtf8), 1);
    std::vector<int> events;
    const prism::pwd::ProbeOutcome out =
        prism::pwd::Probe(plan, tryOpen, [&](int done, int total) {
          events.push_back(done);
          if (done > total) PRISM_CHECK(false);  // progress stays in range
        });
    if (!PRISM_CHECK(out.found)) {
      std::cout << "    diagnostics: " << out.diagnostics << std::endl;
      RemoveDir(dir);
      return 1;
    }
    if (!PRISM_CHECK_EQ(out.password, kPasswordUtf8)) return 1;
    if (!PRISM_CHECK_EQ(out.attemptsMade, 4)) return 1;  // 3 KDF misses + hit
    if (!PRISM_CHECK_EQ(events.size(), size_t(4))) return 1;
    bool monotonic = true;
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i] != static_cast<int>(i) + 1) monotonic = false;
    }
    if (!PRISM_CHECK(monotonic)) return 1;
    if (!PRISM_CHECK_EQ(events.back(), 4)) return 1;
    if (!PRISM_CHECK(out.elapsedMs >= 0)) return 1;
    // Content-level redaction (D2): no candidate text reaches diagnostics.
    for (const std::string& w : CandidateWords(kPasswordUtf8)) {
      if (!PRISM_CHECK(out.diagnostics.find(w) == std::string::npos)) {
        std::cout << "    leaked candidate: " << w << std::endl;
        RemoveDir(dir);
        return 1;
      }
    }
    std::cout << "    hit@4: found in " << out.elapsedMs << " ms ("
              << out.attemptsMade << " real AES opens)" << std::endl;
  }

  // ---- Case 2: all wrong, concurrent plan, AES budget exhausted ----------
  {
    const prism::pwd::PasswordRing::AttemptPlan plan =
        PlanFromWords(CandidateWords(""), 4);  // 12 decoys, no hit among them
    std::vector<int> events;
    const auto t0 = std::chrono::steady_clock::now();
    const prism::pwd::ProbeOutcome out =
        prism::pwd::Probe(plan, tryOpen, [&](int done, int) {
          events.push_back(done);
        });
    const auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();

    if (!PRISM_CHECK(!out.found)) return 1;
    if (!PRISM_CHECK(out.password.empty())) return 1;
    if (!PRISM_CHECK_EQ(out.attemptsMade, 12)) return 1;  // AES budget: 12
    if (!PRISM_CHECK_EQ(events.size(), size_t(12))) return 1;
    bool monotonic = true;
    for (size_t i = 0; i < events.size(); ++i) {
      if (events[i] != static_cast<int>(i) + 1) monotonic = false;
    }
    if (!PRISM_CHECK(monotonic)) return 1;
    if (!PRISM_CHECK(out.elapsedMs > 0)) return 1;  // real KDF time, not 0
    if (!PRISM_CHECK(!out.diagnostics.empty())) return 1;
    // CONTENT-level absence of every candidate (CONTRACTS D2), one find() per
    // candidate -- a "[redacted]-somewhere" existence check would pass on a
    // partially leaked string.
    for (const std::string& w : CandidateWords("")) {
      if (!PRISM_CHECK(out.diagnostics.find(w) == std::string::npos)) return 1;
      if (w.size() >= 6 &&
          !PRISM_CHECK(out.diagnostics.find(w.substr(0, 5)) == std::string::npos)) {
        return 1;
      }
    }
    // The miss report must carry the numbers a user can act on.
    if (!PRISM_CHECK(out.diagnostics.find("12/12") != std::string::npos)) return 1;
    if (!PRISM_CHECK(out.diagnostics.find("未命中") != std::string::npos)) return 1;

    std::cout << "    exhaust: 12 AES candidates in " << out.elapsedMs
              << " ms executor / " << wallMs << " ms wall (4 workers), "
              << out.attemptsMade << " attempts" << std::endl;
    std::cout << "    diagnostics: " << out.diagnostics << std::endl;
  }

  RemoveDir(dir);
  std::cout << "    ALL PWD-PROBE CHECKS PASSED" << std::endl;
  return 0;
}

