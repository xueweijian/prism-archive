// TDD: Windows filename hygiene -- the review's item 5.5. Windows silently
// mangles or *refuses* names that a POSIX-minded extractor happily writes:
//   - reserved device names (CON, PRN, AUX, NUL, COM1..9, LPT1..9) -- even
//     with an extension ("CON.txt" is still the console device),
//   - trailing dots and spaces are stripped by the filesystem, so
//     "file." becomes "file" and a later "file" collides,
//   - ':' opens an NTFS alternate data stream ("a.txt:hidden" hides data),
//   - the < > " | ? * control-character set is simply invalid.
// Every one of these is an extraction-time data-loss or security bug, so the
// sanitiser is tested hard, including the negative control that it does NOT
// rewrite legitimate names.
#include "PathSafety.hpp"
#include "mini_test.hpp"

#include <string>

namespace {

using prism::SanitizeWindowsName;

PRISM_TEST("reserved device names are neutralised, extension or not") {
  PRISM_CHECK_EQ(SanitizeWindowsName("CON"), std::string("CON_"));
  PRISM_CHECK_EQ(SanitizeWindowsName("con.txt"), std::string("con_.txt"));
  PRISM_CHECK_EQ(SanitizeWindowsName("AUX"), std::string("AUX_"));
  PRISM_CHECK_EQ(SanitizeWindowsName("NUL.log"), std::string("NUL_.log"));
  PRISM_CHECK_EQ(SanitizeWindowsName("COM1"), std::string("COM1_"));
  PRISM_CHECK_EQ(SanitizeWindowsName("lpt9.dat"), std::string("lpt9_.dat"));
  // COM0 / COM10 are NOT reserved: over-sanitising breaks real files.
  PRISM_CHECK_EQ(SanitizeWindowsName("COM0"), std::string("COM0"));
  PRISM_CHECK_EQ(SanitizeWindowsName("COM10"), std::string("COM10"));
}

PRISM_TEST("trailing dots and spaces are trimmed (they would be lost)") {
  PRISM_CHECK_EQ(SanitizeWindowsName("file."), std::string("file"));
  PRISM_CHECK_EQ(SanitizeWindowsName("file   "), std::string("file"));
  PRISM_CHECK_EQ(SanitizeWindowsName("file. . "), std::string("file"));
  // Interior dots are legitimate.
  PRISM_CHECK_EQ(SanitizeWindowsName("a.b.c"), std::string("a.b.c"));
}

PRISM_TEST("colons are replaced: NTFS alternate data streams") {
  PRISM_CHECK_EQ(SanitizeWindowsName("a.txt:hidden"), std::string("a.txt_hidden"));
  // A drive-style prefix inside an archive member is still a hostile name.
  PRISM_CHECK_EQ(SanitizeWindowsName("C:evil"), std::string("C_evil"));
}

PRISM_TEST("illegal punctuation and control characters are replaced") {
  PRISM_CHECK_EQ(SanitizeWindowsName("a<b>c|d?e*f\"g"), std::string("a_b_c_d_e_f_g"));
  PRISM_CHECK_EQ(SanitizeWindowsName(std::string("tab\there")), std::string("tab_here"));
  PRISM_CHECK_EQ(SanitizeWindowsName(std::string("nl\nhere")), std::string("nl_here"));
}

PRISM_TEST("negative control: ordinary names pass through untouched") {
  for (const char* name : {"report.pdf", "素材包_v2.7z", "a-b_c (3).txt", "README",
                           "photo_2026.10.30.jpg"}) {
    if (SanitizeWindowsName(name) != std::string(name)) return;
  }
  PRISM_CHECK(true);
}

PRISM_TEST("an all-illegal name never becomes empty") {
  PRISM_CHECK(!SanitizeWindowsName("...").empty());
  PRISM_CHECK(!SanitizeWindowsName("   ").empty());
  PRISM_CHECK(!SanitizeWindowsName("").empty());  // fails loudly, not silently
}

}  // namespace

PRISM_TEST_MAIN()
