// PrismArchive -- C127 i18n honesty suite (sidebar card labels + window title).
//
// THE TWO REPORTED DEFECTS
//   1. The info card's four FIELD LABELS were English ("ENCRYPTION / METHOD /
//      DICTIONARY / RATIO") while their values were Chinese -- two languages in
//      one card. C105 had already put the labels in the tables under
//      "sidebar.card.crypto", but the name did not say "encryption" and the
//      Chinese values were longer paraphrases ("加密状态 / 压缩方法"). This card
//      makes the key name match the word, shortens the values to the requested
//      words, and adds a mutation switch that restores the English defaults so
//      the assertion below can actually go red.
//   2. The window title was "PrismArchive -- fixture.zip"; Windows convention
//      (Bandizip included) is "fixture.zip - PrismArchive". The rule lives in
//      ONE pure function (WindowTitle.hpp) and Main.cpp's two setters call it.
//
// WHAT IS ASSERTED
//   I1  the four labels of the SHIPPING sidebar layout are 加密/方法/字典/压缩率
//       under zh-CN, contain CJK text, and carry no run of >= 4 uppercase ASCII
//       characters (the exact shape of the old defect);
//   I1b the labels really come from the translator: the source asks for
//       "sidebar.card.encryption", and the mutant build (below) proves that
//       turning the lookup off makes I1 red instead of silently passing;
//   I2  WindowTitle(filename) is "^.+ - PrismArchive$" and WindowTitle("") is
//       exactly "PrismArchive"; Main.cpp's TWO title setters use it and the old
//       "PrismArchive -- " literal is gone;
//   I3  RequiredKeys() and both shipped tables carry the four card keys exactly
//       once, the census pin matches the real size (149: C105 already shipped
//       three of the four keys -- there are NOT four new keys, see the report),
//       and the retired "sidebar.card.crypto" name is gone.
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C127_MUTANT_HARDCODED_LABEL
// makes LabelsFrom keep the compiled-in English defaults. The mutant CTest entry
// requires the "C127 MUTATION CAUGHT: hardcoded_label" marker; the marker is
// printed at the FIRST failing assertion only when the injected branch is
// active, so an injection that stopped firing leaves the entry RED. No WILL_FAIL
// is stacked on the regex.
#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "I18n.hpp"
#include "SidebarModel.hpp"
#include "SidebarView.hpp"
#include "WindowTitle.hpp"
#include "mini_test.hpp"

#if !defined(PRISM_C127_LANG_DIR)
#error "PRISM_C127_LANG_DIR must name Assets/Lang (see Tests/suites/c127-i18n.cmake)"
#endif
#if !defined(PRISM_C127_SOURCE_DIR)
#error "PRISM_C127_SOURCE_DIR must name the repository root (see Tests/suites/c127-i18n.cmake)"
#endif

namespace {

using prism::i18n::Catalog;
using prism::i18n::Translator;

// UTF-8 byte escapes, not literal CJK: the compiler's source charset must not
// be able to turn a missing translation into mojibake that still compares equal.
const std::string kZhEncryption = "\xE5\x8A\xA0\xE5\xAF\x86";              // 加密
const std::string kZhMethod = "\xE6\x96\xB9\xE6\xB3\x95";                  // 方法
const std::string kZhDictionary = "\xE5\xAD\x97\xE5\x85\xB8";              // 字典
const std::string kZhRatio = "\xE5\x8E\x8B\xE7\xBC\xA9\xE7\x8E\x87";       // 压缩率

std::string LangPath(std::string_view file) {
  return std::string(PRISM_C127_LANG_DIR) + "/" + std::string(file);
}

bool LoadShipped(std::string_view tag, Catalog* out) {
  std::string error;
  const std::string path = LangPath(std::string(tag) + ".json");
  if (!out->LoadFromFile(path, &error)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "cannot load " + path + ": " + error);
    return false;
  }
  return true;
}

Translator MakeTranslator(const Catalog& catalog) {
  Translator translator;
  translator.AddCatalog(catalog);
  translator.SetLanguage("zh-CN");
  return translator;
}

// A run of >= 4 characters that are ASCII uppercase letters or spaces, with at
// least one letter. This is the exact shape of "ENCRYPTION"/"NO ARCHIVE OPEN";
// it deliberately does not fire on "AES-256" (a token with a digit/hyphen) or on
// acronyms shorter than four.
bool HasAllCapsRun(std::string_view text, std::size_t minRun = 4) {
  std::size_t run = 0;
  bool hasLetter = false;
  for (const char c : text) {
    const bool upper = c >= 'A' && c <= 'Z';
    const bool space = c == ' ';
    if (upper || space) {
      ++run;
      hasLetter = hasLetter || upper;
    } else {
      if (run >= minRun && hasLetter) return true;
      run = 0;
      hasLetter = false;
    }
  }
  return run >= minRun && hasLetter;
}

bool HasNonAscii(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80u) return true;
  }
  return false;
}

struct CardFixture {
  prism::sidebar::Model model;
  std::vector<prism::ArchiveItem> catalog;

  CardFixture() {
    prism::ArchiveItem file;
    file.index = 0;
    file.path = "report.txt";
    file.size = 1000;
    file.packSize = 988;
    catalog.push_back(file);
    model.AttachCatalog(&catalog);
    model.SetArchiveName("fixture.zip");
    prism::sidebar::ArchiveInfo info;
    info.method = "LZMA2";
    info.dictionaryBytes = 64u * 1024u * 1024u;
    info.encryption = prism::sidebar::Encryption::None;
    model.SetArchiveInfo(info);
  }
};

std::string StatusLabelAt(const prism::win::sidebar::Layout& layout, int index) {
  for (const prism::win::sidebar::Command& command : layout.commands) {
    if (command.kind == prism::win::sidebar::PrimKind::StatusLabel &&
        command.payload == index) {
      return command.text;
    }
  }
  return std::string();
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

std::string SourceFile(std::string_view relative) {
  return std::string(PRISM_C127_SOURCE_DIR) + "/" + std::string(relative);
}

std::size_t CountIn(const std::string& haystack, std::string_view needle) {
  std::size_t count = 0;
  for (std::size_t at = haystack.find(needle); at != std::string::npos;
       at = haystack.find(needle, at + needle.size())) {
    ++count;
  }
  return count;
}

prism::win::sidebar::Layout BuildShippingSidebar(const Translator& translator) {
  CardFixture fixture;
  const prism::win::sidebar::Metrics metrics;
  return prism::win::sidebar::Build(fixture.model, metrics,
                                    prism::win::sidebar::LabelsFrom(&translator));
}

}  // namespace

// ---------------------------------------------------------------- I1 labels
PRISM_TEST("c127 the info card's four labels are Chinese words, not English caps") {
  Catalog zh;
  if (!LoadShipped("zh-CN", &zh)) return;
  const Translator translator = MakeTranslator(zh);
  const prism::win::sidebar::Layout layout = BuildShippingSidebar(translator);

  const std::string labels[4] = {
      StatusLabelAt(layout, 0), StatusLabelAt(layout, 1),
      StatusLabelAt(layout, 2), StatusLabelAt(layout, 3)};
  const std::string wanted[4] = {kZhEncryption, kZhMethod, kZhDictionary, kZhRatio};
  const char* names[4] = {"encryption", "method", "dictionary", "ratio"};

  for (int i = 0; i < 4; ++i) {
    std::printf("  [c127] card label %-10s = \"%s\"\n", names[i], labels[i].c_str());
    PRISM_CHECK(!labels[i].empty());
    PRISM_CHECK(HasNonAscii(labels[i]));
    PRISM_CHECK_EQ(labels[i], wanted[i]);
    // The reported defect: an all-caps ASCII word beside a Chinese value.
    if (HasAllCapsRun(labels[i])) {
      prism::test::ReportFailure(__FILE__, __LINE__,
                                 std::string("card label is ALL-CAPS ASCII: ") + labels[i]);
    }
  }
  // Four DIFFERENT words: a lookup that accidentally returns one label four
  // times would otherwise still be "Chinese and lowercase".
  for (int i = 0; i < 4; ++i) {
    for (int j = i + 1; j < 4; ++j) PRISM_CHECK(labels[i] != labels[j]);
  }
}

// I1b -- the labels are reached THROUGH the translator, not hardcoded.
PRISM_TEST("c127 the card labels ask for sidebar.card.encryption/method/dictionary/ratio") {
  const std::string view = ReadWholeFile(SourceFile("Src/PrismWin/SidebarView.hpp"));
  PRISM_CHECK(!view.empty());
  PRISM_CHECK_EQ(CountIn(view, "pick(\"sidebar.card.encryption\""), std::size_t{1});
  PRISM_CHECK_EQ(CountIn(view, "pick(\"sidebar.card.method\""), std::size_t{1});
  PRISM_CHECK_EQ(CountIn(view, "pick(\"sidebar.card.dictionary\""), std::size_t{1});
  PRISM_CHECK_EQ(CountIn(view, "pick(\"sidebar.card.ratio\""), std::size_t{1});
  // The old name is retired: a table that still answers it is a second truth.
  PRISM_CHECK_EQ(CountIn(view, "sidebar.card.crypto"), std::size_t{0});
}

// ------------------------------------------------------------------ I2 title
PRISM_TEST("c127 the window title is '<file> - PrismArchive', or just the app name") {
  const std::wstring withFile = prism::ui::WindowTitle(L"fixture.zip");
  const std::wstring withoutFile = prism::ui::WindowTitle(L"");
  std::printf("  [c127] title with file = \"%ls\"\n", withFile.c_str());
  std::printf("  [c127] title no archive = \"%ls\"\n", withoutFile.c_str());

  PRISM_CHECK(withFile == std::wstring(L"fixture.zip - PrismArchive"));
  PRISM_CHECK(withoutFile == std::wstring(L"PrismArchive"));

  // The contract shape from the card, asserted as a regex rather than only as
  // one literal: a file name may be anything, the suffix may not move.
  const std::wregex withFileShape(L"^.+ - PrismArchive$");
  const std::wregex withoutFileShape(L"^PrismArchive$");
  PRISM_CHECK(std::regex_match(withFile, withFileShape));
  PRISM_CHECK(std::regex_match(withoutFile, withoutFileShape));

  // Chinese file names are DATA, not labels: the suffix is the one fixed part.
  const std::wstring zhName = prism::ui::WindowTitle(L"\u6d4b\u8bd5.zip");
  PRISM_CHECK(std::regex_match(zhName, withFileShape));
}

PRISM_TEST("c127 Main.cpp has no second title truth (both setters call WindowTitle)") {
  const std::string main = ReadWholeFile(SourceFile("Src/App/Main.cpp"));
  PRISM_CHECK(!main.empty());
  PRISM_CHECK_EQ(CountIn(main, "window.Create(prism::ui::WindowTitle("), std::size_t{1});
  PRISM_CHECK_EQ(CountIn(main, "prism::ui::WindowTitle("), std::size_t{2});
  PRISM_CHECK(CountIn(main, "SetWindowTextW(window.Hwnd(),") == 1);
  // The reversed form must be gone from the product source as a LITERAL: a
  // future copy/paste of the old line would reintroduce the defect. (The
  // header comment may still NAME the old shape.)
  PRISM_CHECK_EQ(CountIn(main, "L\"PrismArchive -- \""), std::size_t{0});
}

// ---------------------------------------------------------------- I3 census
PRISM_TEST("c127 the census and both tables carry the four card keys exactly once") {
  const std::vector<std::string>& required = prism::i18n::RequiredKeys();
  const char* keys[4] = {"sidebar.card.encryption", "sidebar.card.method",
                         "sidebar.card.dictionary", "sidebar.card.ratio"};

  for (const char* key : keys) {
    const auto count = std::count(required.begin(), required.end(), std::string(key));
    if (count != 1) {
      prism::test::ReportFailure(__FILE__, __LINE__,
                                 std::string("i18n key ") + key + " appears " +
                                     std::to_string(count) + " times in RequiredKeys()");
    }
  }
  // The retired name must not linger in the contract.
  PRISM_CHECK_EQ(std::count(required.begin(), required.end(),
                            std::string("sidebar.card.crypto")),
                 std::ptrdiff_t{0});

  Catalog en;
  Catalog zh;
  if (!LoadShipped("en-US", &en) || !LoadShipped("zh-CN", &zh)) return;
  const std::string zhValues[4] = {kZhEncryption, kZhMethod, kZhDictionary, kZhRatio};
  const std::string enValues[4] = {"ENCRYPTION", "METHOD", "DICTIONARY", "RATIO"};
  for (int i = 0; i < 4; ++i) {
    const std::string* enValue = en.Find(keys[i]);
    const std::string* zhValue = zh.Find(keys[i]);
    if (enValue == nullptr || zhValue == nullptr) {
      prism::test::ReportFailure(__FILE__, __LINE__,
                                 std::string("missing card key in a shipped table: ") + keys[i]);
      continue;
    }
    PRISM_CHECK_EQ(*enValue, enValues[i]);
    PRISM_CHECK_EQ(*zhValue, zhValues[i]);
    PRISM_CHECK_EQ(en.Find(std::string(keys[i])), enValue);
  }
  // A table that kept the old key would be a second truth for the same word.
  PRISM_CHECK(en.Find("sidebar.card.crypto") == nullptr);
  PRISM_CHECK(zh.Find("sidebar.card.crypto") == nullptr);

  const std::size_t census = required.size();
  std::printf("  [c127] census=%zu card_keys=4\n", census);
  // C105 shipped three of the four keys (method/dictionary/ratio) and the fourth
  // under the name "crypto". Renaming that one keeps the census at 149; the
  // card's "153" assumed all four were new (see the report's evidence).
  PRISM_CHECK_EQ(census, std::size_t{149});
}

// A table from an OLDER install: everything except the four card keys. The
// screenshot that opened this card is exactly this file: menu/values Chinese,
// card labels the compiled English defaults. The defect is not the label lookup
// (it did ask); it is that NOTHING SAID the table was incomplete.
PRISM_TEST("c127 a stale table is REPORTED and explains the mixed-language card") {
  const std::string document = ReadWholeFile(LangPath("zh-CN.json"));
  PRISM_CHECK(!document.empty());
  std::string stale;
  stale.reserve(document.size());
  std::istringstream lines(document);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("\"sidebar.card.") != std::string::npos) continue;
    stale += line;
    stale += '\n';
  }
  Catalog catalog;
  std::string error;
  if (!PRISM_CHECK(catalog.LoadFromJson(stale, &error))) {
    std::printf("  [c127] stale table rejected: %s\n", error.c_str());
    return;
  }
  Translator staleTranslator;
  staleTranslator.AddCatalog(catalog);
  staleTranslator.SetLanguage("zh-CN");

  const std::vector<std::string> missing =
      prism::i18n::MissingRequiredKeys(staleTranslator);
  std::printf("  [c127] stale table missing required keys: %zu\n", missing.size());
  PRISM_CHECK_EQ(missing.size(), std::size_t{4});
  for (const char* key : {"sidebar.card.encryption", "sidebar.card.method",
                          "sidebar.card.dictionary", "sidebar.card.ratio"}) {
    const auto at = std::find(missing.begin(), missing.end(), std::string(key));
    if (at == missing.end()) {
      prism::test::ReportFailure(__FILE__, __LINE__,
                                 std::string("stale table did not report ") + key);
    }
  }

  // And this IS how the screenshot happened: the lookup asks, the table cannot
  // answer, the compiled default is English next to a Chinese value.
  CardFixture fixture;
  const prism::win::sidebar::Metrics metrics;
  const prism::win::sidebar::Layout staleLayout =
      prism::win::sidebar::Build(fixture.model, metrics,
                                 prism::win::sidebar::LabelsFrom(&staleTranslator));
  const std::string staleLabel = StatusLabelAt(staleLayout, 0);
  std::printf("  [c127] stale-table first label = \"%s\"\n", staleLabel.c_str());
  PRISM_CHECK_EQ(staleLabel, std::string("ENCRYPTION"));
  PRISM_CHECK(HasAllCapsRun(staleLabel));
}

int main(int argc, char** argv) {
#if defined(PRISM_C127_MUTANT_HARDCODED_LABEL)
  // The injection is "on" exactly when the shipping lookup is off; only then may
  // the failure below print CAUGHT.
  prism::test::SetMutationVerdict("C127 MUTATION CAUGHT: hardcoded_label",
                                  [] { return !prism::win::sidebar::kCardLabelsLocalized; });
#endif
  const int code = prism::test::RunAll(argc, argv);
  if (code == 0) std::puts("ALL C127 I18N CHECKS PASSED");
  return code;
}
