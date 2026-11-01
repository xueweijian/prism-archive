// PrismArchive -- compression plan (blueprint ch.2 create path).
//
// This suite is the cheap half of card C9: every decision the archive writer
// makes (format -> method, level -> dictionary, solidity, thread count,
// encryption, the exact ISetProperties payload) is asserted HERE, on Linux, in
// milliseconds. The Windows lane then proves the engine accepts those decisions
// (Tests/win/test_write_archive.cpp).
//
// Negative controls: the same source is compiled four more times with one rule
// deliberately broken each time (PRISM_CP_MUTANT_*), registered WILL_FAIL --
// a mutant that stops failing means this suite stopped being a guard
// (CONTRACTS D3).
#include <cstdint>
#include <string>
#include <vector>

#include "CompressionPlan.hpp"
#include "mini_test.hpp"

using namespace prism;

namespace {

std::vector<PlanItem> TwoFiles() {
  std::vector<PlanItem> items;
  items.push_back(PlanItem{"alpha.txt", 4096, false, 0});
  items.push_back(PlanItem{"sub/gamma.txt", 8192, false, 0});
  items.push_back(PlanItem{"sub", 0, true, 0});
  return items;
}

// The exact decimal byte counts the plan feeds to SetProperties("d").
uint64_t DictOf(int level, uint64_t inputBytes) {
  return ResolveDictionary(ArchiveFormat::SevenZip, ClampLevel(level), inputBytes);
}

// Joined with ',' because mini_test prints operands: std::vector has no
// operator<< and PRISM_CHECK_EQ would fail to compile on it.
std::string PropertyNames(const CompressionPlan& plan) {
  std::string names;
  for (const auto& p : plan.properties) {
    if (!names.empty()) names.push_back(',');
    names += p.name;
  }
  return names;
}

bool HasProperty(const CompressionPlan& plan, const std::string& name, const std::string& value) {
  for (const auto& p : plan.properties) {
    if (p.name == name && p.strValue == value) return true;
  }
  return false;
}

}  // namespace

// ---------------------------------------------------------------------------
PRISM_TEST("level is clamped, never passed through") {
  PRISM_CHECK_EQ(ClampLevel(0), 0);
  PRISM_CHECK_EQ(ClampLevel(7), 7);
  PRISM_CHECK_EQ(ClampLevel(9), 9);
  // A slider cannot produce these; a caller bug must not turn into a 512 MB
  // dictionary request.
  PRISM_CHECK_EQ(ClampLevel(42), 9);
  PRISM_CHECK_EQ(ClampLevel(-3), 5);
}

PRISM_TEST("dictionary never exceeds the payload it compresses") {
  // 12 KiB of input at level 9 ("512 MiB") must not reserve 512 MiB.
  const uint64_t small = DictOf(9, 12288);
  PRISM_CHECK_EQ(small, static_cast<uint64_t>(16384));
  PRISM_CHECK(small >= (4ull << 10));  // LZMA2 minimum window
  // Large input: the ladder wins.
  const uint64_t big = DictOf(6, 100ull << 20);
  PRISM_CHECK_EQ(big, static_cast<uint64_t>(64ull << 20));
  // Level 0 is Copy: no dictionary at all.
  PRISM_CHECK_EQ(DictOf(0, 100ull << 20), static_cast<uint64_t>(0));
  // Empty input still yields a legal dictionary, never 0.
  PRISM_CHECK(DictOf(9, 0) >= (4ull << 10));
  // A caller-supplied dictionary is honoured but capped at 1 GiB.
  CompressionOptions o;
  o.level = 9;
  o.dictionaryBytes = 4ull << 30;
  const auto capped = MakeCompressionPlan(TwoFiles(), o);
  PRISM_CHECK(capped.ok);
  PRISM_CHECK_EQ(capped.dictionaryBytes, static_cast<uint64_t>(1ull << 30));
}

PRISM_TEST("solid: 7z only, and the block fits both dictionary and payload") {
  // zip and tar cannot be solid, whatever the caller asks for.
  PRISM_CHECK(!ResolveSolid(ArchiveFormat::Zip, SolidMode::On));
  PRISM_CHECK(!ResolveSolid(ArchiveFormat::Tar, SolidMode::On));
  PRISM_CHECK(ResolveSolid(ArchiveFormat::SevenZip, SolidMode::Auto));
  PRISM_CHECK(!ResolveSolid(ArchiveFormat::SevenZip, SolidMode::Off));

  CompressionOptions o;
  o.level = 6;  // 64 MiB dictionary
  const auto plan = MakeCompressionPlan(TwoFiles(), o);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK(plan.solid);
  // 12 KiB of payload: the block may not exceed the rounded-up payload.
  PRISM_CHECK_EQ(plan.solidBlockBytes, static_cast<uint64_t>(16384));
  PRISM_CHECK(plan.solidBlockBytes <= plan.dictionaryBytes);

  // Asking for solid on zip is an honest, recorded downgrade -- not a lie and
  // not a hard failure (unlike the encryption cases below).
  CompressionOptions z;
  z.format = ArchiveFormat::Zip;
  z.solid = SolidMode::On;
  const auto zplan = MakeCompressionPlan(TwoFiles(), z);
  PRISM_CHECK(zplan.ok);
  PRISM_CHECK(!zplan.solid);
  PRISM_CHECK_EQ(zplan.solidBlockBytes, static_cast<uint64_t>(0));
  PRISM_CHECK_EQ(zplan.warnings.size(), static_cast<size_t>(1));
}

PRISM_TEST("threads: auto follows the machine, explicit is bounded") {
  PRISM_CHECK_EQ(ResolveThreads(0, 4), 4);
  PRISM_CHECK_EQ(ResolveThreads(0, 0), 8);      // unknown core count
  PRISM_CHECK_EQ(ResolveThreads(100, 200), 64); // engine ceiling, not 200
  PRISM_CHECK_EQ(ResolveThreads(999, 4), 4);    // never more workers than cores
  PRISM_CHECK_EQ(ResolveThreads(3, 8), 3);
  PRISM_CHECK_EQ(ResolveThreads(-2, 8), 8);
}

PRISM_TEST("property payload is the exact 7-Zip contract, in engine order") {
  CompressionOptions o;
  o.level = 9;
  o.cpuCores = 4;
  const auto plan = MakeCompressionPlan(TwoFiles(), o);
  PRISM_CHECK(plan.ok);
  // Order matters: "s" as BOOL initialises the solid state, the "<n>b" string
  // then sets the block size (COutHandler::SetSolidFromPROPVARIANT/String).
  PRISM_CHECK_EQ(PropertyNames(plan), std::string("x,mt,m,d,s,s"));
  PRISM_CHECK_EQ(plan.properties[0].uintValue, static_cast<uint32_t>(9));
  PRISM_CHECK_EQ(plan.properties[1].uintValue, static_cast<uint32_t>(4));
  PRISM_CHECK_EQ(plan.properties[2].strValue, std::string("LZMA2"));
  // The dictionary must ride as a size STRING with a unit suffix: the engine
  // refuses both VT_UI4 and a bare-digit BSTR here (measured 2026-11-01).
  PRISM_CHECK_EQ(plan.properties[3].strValue, std::string("16384b"));
  PRISM_CHECK(plan.properties[4].boolValue);
  PRISM_CHECK(HasProperty(plan, "s", "16384b"));

  // zip: level and threads only -- no dictionary, no method, no solid.
  CompressionOptions z;
  z.format = ArchiveFormat::Zip;
  z.level = 6;
  z.cpuCores = 2;
  const auto zplan = MakeCompressionPlan(TwoFiles(), z);
  PRISM_CHECK(zplan.ok);
  PRISM_CHECK_EQ(zplan.methodName, std::string("Deflate"));
  PRISM_CHECK_EQ(PropertyNames(zplan), std::string("x,mt"));

  // tar: no properties at all, level is forced to 0 and the request is logged.
  CompressionOptions t;
  t.format = ArchiveFormat::Tar;
  t.level = 7;
  const auto tplan = MakeCompressionPlan(TwoFiles(), t);
  PRISM_CHECK(tplan.ok);
  PRISM_CHECK_EQ(tplan.methodName, std::string("Store"));
  PRISM_CHECK_EQ(tplan.level, 0);
  PRISM_CHECK_EQ(tplan.properties.size(), static_cast<size_t>(0));
  PRISM_CHECK_EQ(tplan.warnings.size(), static_cast<size_t>(1));
}

PRISM_TEST("encryption: AES-256 where it exists, refusal where it does not") {
  // 7z: header encryption is a property, and it needs the password to exist.
  CompressionOptions o;
  o.encrypt = true;
  o.encryptFileNames = true;
  const auto plan = MakeCompressionPlan(TwoFiles(), o);
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.cipher, Cipher::Aes256);
  PRISM_CHECK(HasProperty(plan, "he", ""));
  for (const auto& p : plan.properties) {
    if (p.name == "he") PRISM_CHECK(p.boolValue);
  }

  // zip: AES-256 is a method choice ("em"), never header encryption.
  CompressionOptions z;
  z.format = ArchiveFormat::Zip;
  z.encrypt = true;
  const auto zplan = MakeCompressionPlan(TwoFiles(), z);
  PRISM_CHECK(zplan.ok);
  PRISM_CHECK(HasProperty(zplan, "em", "AES256"));
  PRISM_CHECK_EQ(zplan.properties.size(), static_cast<size_t>(3));  // x, mt, em

  // Header encryption on zip is REFUSED: claiming it would show the names.
  CompressionOptions zh;
  zh.format = ArchiveFormat::Zip;
  zh.encrypt = true;
  zh.encryptFileNames = true;
  const auto zhplan = MakeCompressionPlan(TwoFiles(), zh);
  PRISM_CHECK(!zhplan.ok);
  PRISM_CHECK(zhplan.error.find("encrypt file names") != std::string::npos);

  // tar has no cipher at all: silently dropping encryption would hand back an
  // unprotected archive the user asked to protect.
  CompressionOptions t;
  t.format = ArchiveFormat::Tar;
  t.encrypt = true;
  const auto tplan = MakeCompressionPlan(TwoFiles(), t);
  PRISM_CHECK(!tplan.ok);
  PRISM_CHECK(tplan.error.find("tar cannot encrypt") != std::string::npos);

  // Header encryption without a password is a contradiction.
  CompressionOptions n;
  n.encryptFileNames = true;
  const auto nplan = MakeCompressionPlan(TwoFiles(), n);
  PRISM_CHECK(!nplan.ok);
}

PRISM_TEST("cost model is honest about UpdateItems") {
  // level 0 stores: the archive cannot be smaller than the payload.
  CompressionOptions o;
  o.level = 0;
  const auto stored = MakeCompressionPlan(TwoFiles(), o);
  PRISM_CHECK(stored.ok);
  PRISM_CHECK_EQ(stored.inputBytes, static_cast<uint64_t>(4096 + 8192));
  PRISM_CHECK_EQ(stored.cost.lowerBoundBytes, stored.inputBytes);
  PRISM_CHECK_EQ(stored.cost.driver, CostDriver::StoreInputOnce);
  PRISM_CHECK_EQ(stored.cost.writePasses, static_cast<uint32_t>(1));
  PRISM_CHECK(stored.cost.note.find("verbatim") != std::string::npos);

  // level 9 compresses: no honest lower bound, but exactly one full write.
  CompressionOptions c;
  c.level = 9;
  const auto packed = MakeCompressionPlan(TwoFiles(), c);
  PRISM_CHECK(packed.ok);
  PRISM_CHECK_EQ(packed.cost.lowerBoundBytes, static_cast<uint64_t>(0));
  PRISM_CHECK_EQ(packed.cost.driver, CostDriver::CompressInputOnce);
  PRISM_CHECK_EQ(packed.cost.writePasses, static_cast<uint32_t>(1));
  // ADR-002 must be visible where the number is produced, not only in docs.
  PRISM_CHECK(packed.cost.note.find("ADR-002") != std::string::npos);
  PRISM_CHECK(packed.cost.note.find("compresses every input byte once") != std::string::npos);

  // tar: stored, one pass, and it says so.
  CompressionOptions t;
  t.format = ArchiveFormat::Tar;
  const auto tar = MakeCompressionPlan(TwoFiles(), t);
  PRISM_CHECK_EQ(tar.cost.driver, CostDriver::StoreInputOnce);
  PRISM_CHECK_EQ(tar.cost.lowerBoundBytes, tar.inputBytes);
}

PRISM_TEST("entry paths: sanitized or refused, never silently emptied") {
  std::string out;
  std::string why;
  const char kBs = static_cast<char>(92);

  PRISM_CHECK(SanitizeEntryPath(std::string("dir") + kBs + "file.txt", false, &out, &why));
  PRISM_CHECK_EQ(out, std::string("dir/file.txt"));

  PRISM_CHECK(SanitizeEntryPath("sub/", true, &out, &why));
  PRISM_CHECK_EQ(out, std::string("sub"));

  PRISM_CHECK(!SanitizeEntryPath("sub/", false, &out, &why));  // file with separator
  PRISM_CHECK(!SanitizeEntryPath("../escape.txt", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("a/../../b.txt", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("/etc/passwd", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("C:/Windows/evil.dll", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("a//b.txt", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("", false, &out, &why));
  PRISM_CHECK(!SanitizeEntryPath("./a.txt", false, &out, &why));
  // Every refusal carried a reason (CONTRACTS C5: a refusal states why).
  PRISM_CHECK(!why.empty());
}

PRISM_TEST("plan rejects what it cannot honestly promise") {
  const auto empty = MakeCompressionPlan({}, CompressionOptions{});
  PRISM_CHECK(!empty.ok);
  PRISM_CHECK(empty.error.find("nothing to archive") != std::string::npos);

  std::vector<PlanItem> bad{PlanItem{"../evil.txt", 1, false, 0}};
  const auto escaped = MakeCompressionPlan(bad, CompressionOptions{});
  PRISM_CHECK(!escaped.ok);
  PRISM_CHECK(escaped.error.find("path traversal") != std::string::npos);

  // Counts and totals are the plan's contract with the UI.
  const auto plan = MakeCompressionPlan(TwoFiles(), CompressionOptions{});
  PRISM_CHECK(plan.ok);
  PRISM_CHECK_EQ(plan.fileCount, static_cast<uint64_t>(2));
  PRISM_CHECK_EQ(plan.dirCount, static_cast<uint64_t>(1));
  PRISM_CHECK_EQ(plan.inputBytes, static_cast<uint64_t>(12288));
  // Deterministic ordering, twice.
  const auto again = MakeCompressionPlan(TwoFiles(), CompressionOptions{});
  PRISM_CHECK_EQ(plan.items.size(), again.items.size());
  for (size_t i = 0; i < plan.items.size(); ++i) {
    PRISM_CHECK_EQ(plan.items[i].path, again.items[i].path);
  }
  PRISM_CHECK_EQ(plan.items.front().path, std::string("alpha.txt"));
}

PRISM_TEST("archive file name: extension is added, never doubled") {
  PRISM_CHECK_EQ(DefaultArchiveFileName("docs", ArchiveFormat::SevenZip), std::string("docs.7z"));
  PRISM_CHECK_EQ(DefaultArchiveFileName("docs", ArchiveFormat::Zip), std::string("docs.zip"));
  PRISM_CHECK_EQ(DefaultArchiveFileName("docs.tar", ArchiveFormat::Tar), std::string("docs.tar"));
  PRISM_CHECK_EQ(DefaultArchiveFileName("a.b", ArchiveFormat::Zip), std::string("a.b.zip"));
  PRISM_CHECK_EQ(std::string(ExtensionFor(ArchiveFormat::Tar)), std::string(".tar"));
}

PRISM_TEST_MAIN()
