// Legacy CJK filename repair (blueprint ch. 7.1 defence #4).
#include "CharsetRepair.hpp"
#include "mini_test.hpp"

#include <iostream>
#include <string>

namespace {

std::string FromHex(const std::string& hex) {
  std::string out;
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    out.push_back(static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return out;
}

// Fixtures generated with Python codecs (see tools/gen_charset_fixtures.py).
const char* kGbkBytes = "cec4b5b5d7cac1cf2e646f6378";        // 文档资料.docx in GBK
const char* kGbkText = "\xe6\x96\x87\xe6\xa1\xa3\xe8\xb5\x84\xe6\x96\x99.docx";
const char* kGbkMojibake =
    "\xc3\x8e\xc3\x84\xc2\xb5\xc2\xb5\xc3\x97\xc3\x8a\xc3\x81\xc3\x8f.docx";  // read as Latin-1

const char* kBig5Bytes = "c0c9aed7b8eaaec62e646f6378";       // 檔案資料.docx
const char* kBig5Text = "\xe6\xaa\x94\xe6\xa1\x88\xe8\xb3\x87\xe6\x96\x99.docx";

const char* kSjisBytes = "8e9197bf83748348838b835f2e747874";  // 資料フォルダ.txt
const char* kSjisText =
    "\xe8\xb3\x87\xe6\x96\x99\xe3\x83\x95\xe3\x82\xa9\xe3\x83\xab\xe3\x83\x80.txt";

const char* kEucKrBytes = "b9aebcadc0dab7e12e747874";        // 문서자료.txt
const char* kEucKrText = "\xeb\xac\xb8\xec\x84\x9c\xec\x9e\x90\xeb\xa3\x8c.txt";

const char* kGbkDirBytes = "cfeec4bfb1b8b7dd2fcbb5c3f72e747874";  // 项目备份/说明.txt

PRISM_TEST("utf-8 validation is strict (overlong + truncated rejected)") {
  bool multi = false;
  PRISM_CHECK(prism::IsUtf8Valid("plain ascii", &multi));
  PRISM_CHECK(!multi);
  PRISM_CHECK(prism::IsUtf8Valid(kGbkText, &multi));
  PRISM_CHECK(multi);
  PRISM_CHECK(!prism::IsUtf8Valid(FromHex(kGbkBytes), &multi));  // GBK bytes are not UTF-8
  PRISM_CHECK(!prism::IsUtf8Valid(std::string("\xe6\x96")));  // truncated 3-byte seq
  PRISM_CHECK(!prism::IsUtf8Valid(std::string("\xc0\xaf")));      // overlong '/'
}

PRISM_TEST("detector picks GB18030 for simplified-chinese bytes") {
  const std::string raw = FromHex(kGbkBytes);
  const auto ranked = prism::DetectEncodings(raw);
  PRISM_CHECK(ranked.size() >= 2);
  PRISM_CHECK_EQ(std::string(prism::EncodingName(ranked.front().encoding)),
                 std::string("GB18030 (Simplified Chinese)"));
  PRISM_CHECK(ranked.front().score > ranked[1].score);
}

PRISM_TEST("detector picks Big5 for traditional-chinese bytes") {
  const auto ranked = prism::DetectEncodings(FromHex(kBig5Bytes));
  PRISM_CHECK_EQ(std::string(prism::EncodingName(ranked.front().encoding)),
                 std::string("Big5 (Traditional Chinese)"));
}

PRISM_TEST("detector picks Shift-JIS for japanese bytes") {
  const auto ranked = prism::DetectEncodings(FromHex(kSjisBytes));
  PRISM_CHECK_EQ(std::string(prism::EncodingName(ranked.front().encoding)),
                 std::string("Shift-JIS (Japanese)"));
}

PRISM_TEST("korean bytes: EUC-KR is at least a tied finalist") {
  // Known limitation: the GB2312 and KS X 1001 common bands overlap heavily, so
  // without frequency tables EUC-KR ties with GB18030. The contract we promise
  // is "EUC-KR offered in the switcher", not "EUC-KR always first".
  const auto ranked = prism::DetectEncodings(FromHex(kEucKrBytes));
  bool euckrOffered = false;
  for (size_t i = 0; i < ranked.size() && i < 3; ++i) {
    if (ranked[i].encoding == prism::Encoding::EucKr) euckrOffered = true;
  }
  PRISM_CHECK(euckrOffered);
  // And the UI must be told to offer a switcher, because the top scores tie.
  PRISM_CHECK(prism::IsAmbiguous(ranked));
}

PRISM_TEST("utf-8 flagged archives are trusted and left alone") {
  auto transcoder = prism::CreateSystemTranscoder();
  const auto result = prism::RepairArchiveName(kGbkText, /*utf8Flagged=*/true, transcoder.get());
  PRISM_CHECK(!result.repaired);
  PRISM_CHECK_EQ(result.name, std::string(kGbkText));
}

PRISM_TEST("mojibake detection recognizes Latin-1 misreads") {
  PRISM_CHECK(prism::LooksLikeMojibake(kGbkMojibake));
  PRISM_CHECK(!prism::LooksLikeMojibake(kGbkText));
  PRISM_CHECK(!prism::LooksLikeMojibake("plain name.txt"));
}

PRISM_TEST("mojibake -> raw bytes round-trips exactly") {
  PRISM_CHECK_EQ(prism::MojibakeToRawBytes(kGbkMojibake), FromHex(kGbkBytes));
}

PRISM_TEST("end-to-end repair with the system transcoder") {
  auto transcoder = prism::CreateSystemTranscoder();
  if (!transcoder) {
    std::cout << "    SKIP: no system transcoder compiled in (this platform has no iconv)"
              << std::endl;
    return;
  }
  const struct {
    const char* bytes;
    const char* expect;
  } cases[] = {
      {kGbkBytes, kGbkText},
      {kBig5Bytes, kBig5Text},
      {kSjisBytes, kSjisText},
      {kGbkDirBytes, "\xe9\xa1\xb9\xe7\x9b\xae\xe5\xa4\x87\xe4\xbb\xbd/\xe8\xaf\xb4\xe6\x98\x8e.txt"},
  };
  for (const auto& c : cases) {
    const auto result = prism::RepairArchiveName(FromHex(c.bytes), false, transcoder.get());
    PRISM_CHECK(result.repaired);
    PRISM_CHECK_EQ(result.name, std::string(c.expect));
  }

  // Latin-1 mojibake that reaches us as *valid UTF-8 text*.
  const auto mojibake = prism::RepairArchiveName(kGbkMojibake, false, transcoder.get());
  PRISM_CHECK(mojibake.repaired);
  PRISM_CHECK_EQ(mojibake.name, std::string(kGbkText));

  // Korean: the auto-picker may tie with GB18030 (see the ambiguity test), but
  // once the UI lets the user pick EUC-KR explicitly the decode must be exact.
  const auto korean = transcoder->ToUtf8(prism::Encoding::EucKr, FromHex(kEucKrBytes));
  PRISM_CHECK(korean.has_value());
  if (korean) PRISM_CHECK_EQ(*korean, std::string(kEucKrText));
}

}  // namespace

PRISM_TEST_MAIN()
