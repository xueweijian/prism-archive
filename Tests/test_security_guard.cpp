// Bomb fuse, write guard and password ring policy (blueprint ch. 7.1 / ch. 8.2).
#include "PasswordVault.hpp"
#include "SecurityGuard.hpp"
#include "mini_test.hpp"

#include <string>

namespace {

PRISM_TEST("bomb fuse: normal ratios never trip") {
  prism::BombFuse fuse;
  // A healthy LZMA2 stream: ~3:1 over 200 MiB.
  for (int i = 0; i < 64; ++i) {
    PRISM_CHECK(fuse.Feed(1ull << 20, 3ull << 20, 100ull << 30));
  }
  PRISM_CHECK(!fuse.tripped());
  PRISM_CHECK(fuse.currentRatio() < 10.0);
}

PRISM_TEST("bomb fuse: 42.zip class payload trips") {
  prism::BombFuse fuse;
  // 4 MiB of compressed input expanding to 4 GiB (1000+:1).
  bool stillOk = true;
  for (int i = 0; i < 64 && stillOk; ++i) {
    stillOk = fuse.Feed(64ull * 1024, 64ull * 1024 * 1024, 500ull << 30);
  }
  PRISM_CHECK(!stillOk);
  PRISM_CHECK(fuse.tripped());
  PRISM_CHECK(!fuse.reason().empty());
  std::cout << "    fuse reason: " << fuse.reason() << std::endl;
}

PRISM_TEST("bomb fuse: disk-full guard trips before the volume dies") {
  prism::BombLimits limits;
  limits.maxRatio = 1e9;  // disable the ratio rule, keep the space rule
  prism::BombFuse fuse(limits);
  bool stillOk = true;
  for (int i = 0; i < 20 && stillOk; ++i) {
    stillOk = fuse.Feed(1ull << 20, 1ull << 20, 16ull << 20);  // 16 MiB free
  }
  PRISM_CHECK(!stillOk);
  PRISM_CHECK(fuse.tripped());
}

PRISM_TEST("bomb fuse: hard ceiling is honoured") {
  prism::BombLimits limits;
  limits.hardCeilingBytes = 8ull * 1024 * 1024;
  prism::BombFuse fuse(limits);
  bool stillOk = true;
  for (int i = 0; i < 32 && stillOk; ++i) stillOk = fuse.Feed(1024, 1024 * 1024, 1ull << 30);
  PRISM_CHECK(!stillOk);
}

PRISM_TEST("write guard: only paths under the root are approved") {
  prism::WriteGuard guard("C:/out/root");
  std::string norm;
  PRISM_CHECK(guard.Approve("C:/out/root/a/b.txt", &norm));
  PRISM_CHECK_EQ(norm, std::string("C:/out/root/a/b.txt"));
  PRISM_CHECK(!guard.Approve("C:/out/other/a.txt"));
  PRISM_CHECK(!guard.Approve("C:/out"));
  PRISM_CHECK(!guard.Approve("../../evil.txt"));

  // Negative control: a guard that accepted everything would make this pass
  // trivially, so assert the reject path with the exact escape used by Zip Slip.
  PRISM_CHECK(!guard.Approve("C:/out/root/../../Windows/evil.dll"));
}

PRISM_TEST("write guard: symlinks are deferred to a post-pass") {
  prism::WriteGuard guard("C:/out/root");
  guard.DeferLink(7, "link");
  PRISM_CHECK_EQ(guard.deferredLinks().size(), size_t(1));
  PRISM_CHECK_EQ(guard.deferredLinks()[0].first, uint32_t(7));
}

PRISM_TEST("password sniffing: filename + comment + sibling hints") {
  const auto c = prism::ExtractPasswordCandidates("D:/dl/素材包_密码123456.7z");
  bool found = false;
  for (const std::string& s : c) {
    if (s == "123456") found = true;
  }
  PRISM_CHECK(found);

  const auto labelled = prism::ExtractPasswordCandidates("pack.zip", {}, {"解压码：prism2026"});
  bool foundLabelled = false;
  for (const std::string& s : labelled) {
    if (s == "prism2026") foundLabelled = true;
  }
  PRISM_CHECK(foundLabelled);

  const auto english =
      prism::ExtractPasswordCandidates("x.zip", "password: S3cret!");
  bool foundEnglish = false;
  for (const std::string& s : english) {
    if (s == "S3cret!") foundEnglish = true;
  }
  PRISM_CHECK(foundEnglish);
}

PRISM_TEST("password sniffing: no false positives on clean names") {
  const auto c = prism::ExtractPasswordCandidates("D:/dl/Design_Assets_v4.zip");
  // The archive stem is always a candidate -- anything else is a false positive.
  for (const std::string& s : c) {
    PRISM_CHECK(s == "Design_Assets_v4");
  }
}

PRISM_TEST("silent attempt budget is format-aware: AES key derivation gets a short list") {
  // Review item 3a: 7zAES key derivation costs 2^19 SHA-256 rounds per guess
  // (~15-40 ms), so silently trying 64 passwords is 0.5-2.5 s of stall. Only
  // ZipCrypto is cheap enough to sweep the whole vault.
  PRISM_CHECK_EQ(prism::SilentAttemptBudget(prism::PasswordKdfClass::ZipCrypto), size_t(64));
  PRISM_CHECK_EQ(prism::SilentAttemptBudget(prism::PasswordKdfClass::AesKeyDerivation),
                 size_t(12));
  PRISM_CHECK(prism::KdfClassOf("a.zip") == prism::PasswordKdfClass::ZipCrypto);
  PRISM_CHECK(prism::KdfClassOf("a.7z") == prism::PasswordKdfClass::AesKeyDerivation);
  PRISM_CHECK(prism::KdfClassOf("a.rar") == prism::PasswordKdfClass::AesKeyDerivation);

  // A vault deeper than the budget must be truncated, not merely reordered.
  prism::MemoryVault vault;
  for (int i = 0; i < 40; ++i) vault.Add("pw" + std::to_string(i));
  const auto aes = prism::BuildCandidateList(vault, "secret.7z");
  PRISM_CHECK(aes.size() <= 12);
  const auto zip = prism::BuildCandidateList(vault, "secret.zip");
  PRISM_CHECK(zip.size() > aes.size());  // ZipCrypto may take the whole list
  // An explicit number always wins over the format default.
  PRISM_CHECK_EQ(prism::BuildCandidateList(vault, "secret.7z", {}, {}, 3).size(), size_t(3));
}

PRISM_TEST("candidate list: context guesses rank above vault entries") {
  prism::MemoryVault vault;
  vault.Add("vault-first");
  vault.Add("123456");
  const auto list = prism::BuildCandidateList(vault, "素材包_密码123456.7z");
  PRISM_CHECK_EQ(list.front(), std::string("123456"));
  bool sawVault = false;
  for (const std::string& s : list) {
    if (s == "vault-first") sawVault = true;
  }
  PRISM_CHECK(sawVault);
  // No duplicates even when the vault repeats a context guess.
  size_t count123456 = 0;
  for (const std::string& s : list) {
    if (s == "123456") ++count123456;
  }
  PRISM_CHECK_EQ(count123456, size_t(1));
}

}  // namespace

PRISM_TEST_MAIN()
