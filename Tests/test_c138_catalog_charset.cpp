// PrismCore unit tests for C138: ArchiveItem/ArchiveCatalog charset ambiguity
// preservation and CharsetRepair Case-2 (Latin-1 mojibake) ambiguity reporting.
#include "ArchiveCatalog.hpp"
#include "CharsetRepair.hpp"
#include "mini_test.hpp"

#include <algorithm>
#include <string>
#include <vector>

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

// Encodes raw bytes 0x00..0xFF as Latin-1 -> UTF-8 (what 7z.dll cp=28591 emits).
std::string BytesToLatin1Utf8(const std::string& raw) {
  std::string utf8;
  for (unsigned char b : raw) {
    if (b < 0x80) {
      utf8.push_back(static_cast<char>(b));
    } else {
      utf8.push_back(static_cast<char>(0xC0u | (b >> 6)));
      utf8.push_back(static_cast<char>(0x80u | (b & 0x3Fu)));
    }
  }
  return utf8;
}

class StubTranscoder final : public prism::ITranscoder {
 public:
  std::optional<std::string> ToUtf8(prism::Encoding, std::string_view raw) override {
    return std::string("decoded:") + std::string(raw.size(), 'x');
  }
  const char* BackendName() const override { return "stub"; }
};

PRISM_TEST("ArchiveCatalog preserves pathAmbiguous and alternatives across items") {
  prism::ArchiveCatalog catalog;
  prism::ArchiveItem cleanItem;
  cleanItem.index = 0;
  cleanItem.path = "readme.txt";
  cleanItem.pathAmbiguous = false;
  cleanItem.pathEncoding = prism::Encoding::Utf8;
  catalog.Add(cleanItem);

  prism::ArchiveItem ambigItem;
  ambigItem.index = 1;
  ambigItem.path = "ambig.txt";
  ambigItem.pathAmbiguous = true;
  ambigItem.pathEncoding = prism::Encoding::Gb18030;
  ambigItem.pathAlternatives = {prism::Encoding::Gb18030, prism::Encoding::EucKr};
  catalog.Add(ambigItem);

  PRISM_CHECK(catalog.HasAmbiguousPaths());
  const std::vector<uint32_t> indices = catalog.AmbiguousIndices();
  PRISM_CHECK_EQ(indices.size(), static_cast<size_t>(1));
  PRISM_CHECK_EQ(indices[0], static_cast<uint32_t>(1));

  const std::vector<prism::Encoding> alts = catalog.AmbiguousAlternatives();
  PRISM_CHECK_EQ(alts.size(), static_cast<size_t>(2));
  PRISM_CHECK(std::find(alts.begin(), alts.end(), prism::Encoding::EucKr) != alts.end());
}

PRISM_TEST("CharsetRepair Case 2 preserves ambiguity and EUC-KR alternative on mojibake") {
  const std::string eucKrRaw = FromHex("b9aebcadc0dab7e12e747874");
  const std::string latin1Mojibake = BytesToLatin1Utf8(eucKrRaw);
  PRISM_CHECK(prism::LooksLikeMojibake(latin1Mojibake));

  StubTranscoder transcoder;
  const prism::RepairResult repaired =
      prism::RepairArchiveName(latin1Mojibake, /*utf8Flagged=*/false, &transcoder);
  PRISM_CHECK(repaired.repaired);
  PRISM_CHECK(repaired.ambiguous);
  PRISM_CHECK(std::find(repaired.alternatives.begin(), repaired.alternatives.end(),
                        prism::Encoding::EucKr) != repaired.alternatives.end());
}

}  // namespace

PRISM_TEST_MAIN()
