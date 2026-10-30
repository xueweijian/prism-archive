// Path normalization + Zip Slip regression suite.
#include "PathSafety.hpp"
#include "mini_test.hpp"

namespace {

std::string HexDump(const std::string& s) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  for (const char c : s) {
    const auto v = static_cast<unsigned char>(c);
    out.push_back(kDigits[v >> 4]);
    out.push_back(kDigits[v & 0xF]);
  }
  return out;
}

PRISM_TEST("normalize: backslashes, duplicate and trailing separators") {
  PRISM_CHECK_EQ(prism::NormalizeArchivePath("a\\b\\c.txt"), std::string("a/b/c.txt"));
  PRISM_CHECK_EQ(prism::NormalizeArchivePath("/abs/path/"), std::string("abs/path"));
  PRISM_CHECK_EQ(prism::NormalizeArchivePath("a//b///c"), std::string("a/b/c"));
  PRISM_CHECK_EQ(prism::NormalizeArchivePath(""), std::string(""));
}

PRISM_TEST("sanitize: Zip Slip traversal is neutralized") {
  // Classic Zip Slip payloads -- every one must lose its escape segments.
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("../../Windows/System32/evil.dll"),
                 std::string("Windows/System32/evil.dll"));
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("..\\..\\evil.exe"), std::string("evil.exe"));
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("a/../../b.txt"), std::string("b.txt"));
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("/etc/passwd"), std::string("etc/passwd"));
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("C:\\Windows\\evil.dll"),
                 std::string("Windows/evil.dll"));
  PRISM_CHECK_EQ(prism::SanitizeRelativePath("a/./b/../c.txt"), std::string("a/c.txt"));
  // "..." is NOT a traversal segment -- it must be preserved verbatim.
  PRISM_CHECK_EQ(prism::SanitizeRelativePath(".../.../x"), std::string(".../.../x"));
}

// Negative control: if the sanitizer silently stopped dropping ".." the checks
// above would still pass only because the expected strings were written by hand,
// so pin the property that matters directly -- no surviving ".." segment.
PRISM_TEST("sanitize property: no '..' segment survives any input") {
  const char* payloads[] = {"..", "../..", "a/../..", "../../../a/b", "a\\..\\..\\..\\c"};
  for (const char* p : payloads) {
    const std::string clean = prism::SanitizeRelativePath(p);
    for (const std::string& part : prism::SplitPath(clean)) {
      PRISM_CHECK(part != "..");
      PRISM_CHECK(part != ".");
    }
  }
}

PRISM_TEST("containment guard accepts children and rejects escapes") {
  PRISM_CHECK(prism::IsContainedIn("/out/root", "/out/root/a/b.txt"));
  PRISM_CHECK(prism::IsContainedIn("/out/root", "/out/root"));
  PRISM_CHECK(!prism::IsContainedIn("/out/root", "/out/root2/a.txt"));  // prefix trap
  PRISM_CHECK(!prism::IsContainedIn("/out/root", "/out"));
  PRISM_CHECK(!prism::IsContainedIn("/out/root", "/other/a.txt"));
}

PRISM_TEST("archive stem handles compound extensions") {
  PRISM_CHECK_EQ(prism::ArchiveStem("Design_Assets_v4.tar.gz"), std::string("Design_Assets_v4"));
  PRISM_CHECK_EQ(prism::ArchiveStem("Backup.tar.zst"), std::string("Backup"));
  PRISM_CHECK_EQ(prism::ArchiveStem("plain.zip"), std::string("plain"));
  PRISM_CHECK_EQ(prism::ArchiveStem("D:\\dl\\Project.7z"), std::string("Project"));
  PRISM_CHECK_EQ(prism::ArchiveStem("no_extension"), std::string("no_extension"));
}

PRISM_TEST("noise + promo classification") {
  PRISM_CHECK(prism::IsNoiseEntry("__MACOSX/._icon"));
  PRISM_CHECK(prism::IsNoiseEntry("photos/.DS_Store"));
  PRISM_CHECK(prism::IsNoiseEntry("Thumbs.db"));
  PRISM_CHECK(!prism::IsNoiseEntry("real/file.txt"));
  PRISM_CHECK(prism::IsPromoNoise("Visit_Our_Sponsor.url"));
  PRISM_CHECK(prism::IsPromoNoise("Download_More_FreeStuff.htm"));
  // Real content must never be classified as promo -- a README or a setup.exe
  // is the payload in most archives, and dropping it silently destroys data.
  PRISM_CHECK(!prism::IsPromoNoise("readme.txt"));
  PRISM_CHECK(!prism::IsPromoNoise("setup.exe"));
  PRISM_CHECK(!prism::IsPromoNoise("main.cpp"));
  PRISM_CHECK(!prism::IsPromoNoise("photo_0001.jpg"));
}

PRISM_TEST("utf-8 payload bytes survive untouched") {
  const std::string name = "\xe6\x96\x87\xe6\xa1\xa3.txt";  // 文档.txt
  PRISM_CHECK_EQ(HexDump(prism::SanitizeRelativePath(name)), HexDump(name));
  PRISM_CHECK_EQ(prism::FileNameOf("dir/\xe6\x96\x87.txt"), std::string("\xe6\x96\x87.txt"));
}

}  // namespace

PRISM_TEST_MAIN()
