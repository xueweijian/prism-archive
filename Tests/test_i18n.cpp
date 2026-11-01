// PrismArchive -- PrismCore i18n suite (card C19).
//
// What is proven here, in the order the UI hits it:
//   * both shipped tables parse through card C14's strict JSON DOM, and the two
//     key sets are IDENTICAL -- a half-translated table cannot land;
//   * every required surface resolves: the right-click menu's five items (driven
//     by ContextMenuModel's real verbs, not by a copy of its strings), the
//     sidebar headings, the telemetry capsule, the buttons, the errors and the
//     SKIP prompts;
//   * the fallback chain zh-CN -> zh -> en-US, each level asserted by CONTENT,
//     and the chain is capped at three levels;
//   * a missing key returns the key itself and is counted (unique list + hit
//     count), so "no untranslated string shipped" can be a real gate;
//   * "{n}" placeholders: filled once, unknown names preserved, values never
//     re-scanned, plural .one/.other selection;
//   * language choice: settings value, auto -> system locale, invalid value ->
//     en-US with exactly ONE recorded validation failure;
//   * a rejected resource file is loud and leaves the loaded tables untouched.
//
// CONTRACTS: D1 (the success marker below is part of the verdict, and there is
// no way to SKIP: the assets are a compile-time input), D2 (every assertion is
// about content -- exact bytes for Chinese, exact verbs for the menu), D3 (three
// negative controls: the same suite rebuilt with one rule compiled out, WILL_FAIL
// is the verdict, and a separate --mutation-report entry pins the marker so an
// injection that never landed is RED instead of a silent no-op).
//
// B6 note: no enum is compared at the top level of PRISM_CHECK_EQ here, so no
// operator<< is required from the header.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "ContextMenuModel.hpp"
#include "I18n.hpp"
#include "SettingsStore.hpp"
#include "mini_test.hpp"

#if !defined(PRISM_C19_LANG_DIR)
// The asset directory is a COMPILE-TIME input (Tests/suites/c19-i18n.cmake).
// Without it the suite would read nothing and pass vacuously -- the D1 trap.
#error "PRISM_C19_LANG_DIR must name the Assets/Lang directory (see Tests/suites/c19-i18n.cmake)"
#endif

namespace {

using prism::i18n::Catalog;
using prism::i18n::LanguageChoice;
using prism::i18n::Substitution;
using prism::i18n::Translator;

// The Chinese values asserted below are written as UTF-8 byte escapes on
// purpose: a compiler that reads this file as ANSI must not be able to turn
// "解压" into mojibake and still pass. (MSVC gets /utf-8 globally, gcc/clang
// default to UTF-8; the escapes make it irrelevant.)
const std::string kZhExtract = "\xE8\xA7\xA3\xE5\x8E\x8B";                                    // 解压
const std::string kZhExtractHere =                                                            // 解压到此处
    "\xE8\xA7\xA3\xE5\x8E\x8B\xE5\x88\xB0\xE6\xAD\xA4\xE5\xA4\x84";
const std::string kZhFiles = "{n} \xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6";                      // {n} 个文件
const std::string kZhSkipSummary = "\xE8\xB7\xB3\xE8\xBF\x87 {n} \xE9\xA1\xB9";               // 跳过 {n} 项
const std::string kZhFailed = "\xE8\xA7\xA3\xE5\x8E\x8B\xE5\xA4\xB1\xE8\xB4\xA5";             // 解压失败
const std::string kZhFavorites = "\xE6\x94\xB6\xE8\x97\x8F";                                  // 收藏
const std::string kZhArchiveOpen = "\xE6\x9C\xAA\xE6\x89\x93\xE5\xBC\x80\xE5\xBD\x92"         // 未打开归档
                                   "\xE6\xA1\xA3";

std::string LangPath(std::string_view file) {
  return std::string(PRISM_C19_LANG_DIR) + "/" + std::string(file);
}

bool IsValidUtf8(std::string_view text) {
  std::size_t i = 0;
  while (i < text.size()) {
    const unsigned char c = static_cast<unsigned char>(text[i]);
    std::size_t continuation = 0;
    if (c < 0x80u) {
      ++i;
      continue;
    }
    if ((c & 0xE0u) == 0xC0u) {
      continuation = 1;
    } else if ((c & 0xF0u) == 0xE0u) {
      continuation = 2;
    } else if ((c & 0xF8u) == 0xF0u) {
      continuation = 3;
    } else {
      return false;
    }
    if (i + continuation >= text.size()) return false;
    for (std::size_t k = 1; k <= continuation; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0u) != 0x80u) return false;
    }
    i += continuation + 1;
  }
  return true;
}

bool HasNonAscii(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80u) return true;
  }
  return false;
}

bool HasControlChars(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) < 0x20u) return true;
  }
  return false;
}

// Loads a shipped table; the bool is the CHECK's subject so a missing asset file
// is a red case, never a skip.
bool LoadShippedCatalog(std::string_view tag, Catalog* out) {
  std::string error;
  const std::string path = LangPath(std::string(tag) + ".json");
  if (!out->LoadFromFile(path, &error)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "cannot load " + path + ": " + error);
    return false;
  }
  return prism::i18n::SameTag(out->language(), tag);
}

std::vector<std::string> SortedKeys(const Catalog& catalog) {
  std::vector<std::string> keys;
  keys.reserve(catalog.size());
  for (const prism::i18n::StringEntry& entry : catalog.entries()) keys.push_back(entry.key);
  std::sort(keys.begin(), keys.end());
  return keys;
}

std::string Join(const std::vector<std::string>& parts) {
  std::string joined;
  for (const std::string& part : parts) {
    if (!joined.empty()) joined.push_back(',');
    joined += part;
  }
  return joined;
}

std::string JoinChain(const std::vector<std::string>& parts) { return Join(parts); }

// A translator carrying the two shipped tables, at the given language.
Translator MakeTranslator(std::string_view language) {
  Translator translator;
  Catalog en;
  Catalog zh;
  if (!LoadShippedCatalog("en-US", &en) || !LoadShippedCatalog("zh-CN", &zh)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "shipped tables could not be loaded");
  }
  std::string error;
  if (!translator.AddCatalog(std::move(en), &error)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "en-US catalog was rejected: " + error);
  }
  if (!translator.AddCatalog(std::move(zh), &error)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "zh-CN catalog was rejected: " + error);
  }
  translator.SetLanguage(language);
  return translator;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. The shipped tables.
// ---------------------------------------------------------------------------
PRISM_TEST("catalog: the en-US table parses through the C14 JSON DOM") {
  Catalog en;
  PRISM_CHECK(LoadShippedCatalog("en-US", &en));
  PRISM_CHECK_EQ(en.language(), std::string("en-US"));
  PRISM_CHECK(en.size() >= 50);  // the documented minimum surface (C38: 39 -> 50)
  PRISM_CHECK_EQ(en.size(), prism::i18n::RequiredKeys().size());

  const std::string* extract = en.Find("menu.extract.here");
  PRISM_CHECK(extract != nullptr);
  if (extract != nullptr) PRISM_CHECK_EQ(*extract, std::string("Extract Here"));

  // A placeholder survives parsing byte-for-byte: no unescaping accident.
  const std::string* toSubfolder = en.Find("menu.extract.to.subfolder");
  PRISM_CHECK(toSubfolder != nullptr);
  if (toSubfolder != nullptr) {
    PRISM_CHECK_EQ(*toSubfolder, std::string("Extract to \"{name}\""));
  }
  const std::string* files = en.Find("status.files.other");
  PRISM_CHECK(files != nullptr);
  if (files != nullptr) PRISM_CHECK_EQ(*files, std::string("{n} FILES"));

  // Every English value is printable ASCII or at least valid UTF-8, never a
  // control character (a "\n" smuggled into a label breaks layout silently).
  for (const prism::i18n::StringEntry& entry : en.entries()) {
    PRISM_CHECK(!entry.value.empty());
    PRISM_CHECK(IsValidUtf8(entry.value));
    PRISM_CHECK(!HasControlChars(entry.value));
  }
}

PRISM_TEST("catalog: the zh-CN table is complete, real Chinese, and key-identical") {
  Catalog en;
  Catalog zh;
  PRISM_CHECK(LoadShippedCatalog("en-US", &en));
  PRISM_CHECK(LoadShippedCatalog("zh-CN", &zh));
  PRISM_CHECK_EQ(zh.language(), std::string("zh-CN"));
  PRISM_CHECK_EQ(zh.size(), en.size());

  // Parity in BOTH directions: a key added to one table only must fail here.
  const std::vector<std::string> enKeys = SortedKeys(en);
  const std::vector<std::string> zhKeys = SortedKeys(zh);
  PRISM_CHECK_EQ(Join(zhKeys), Join(enKeys));

  // Exact bytes: "解压到此处", not a mojibake byte soup that happens to be valid
  // UTF-8. D2 -- content, not existence.
  const std::string* extractHere = zh.Find("menu.extract.here");
  PRISM_CHECK(extractHere != nullptr);
  if (extractHere != nullptr) PRISM_CHECK_EQ(*extractHere, kZhExtractHere);
  const std::string* extract = zh.Find("action.extract");
  PRISM_CHECK(extract != nullptr);
  if (extract != nullptr) PRISM_CHECK_EQ(*extract, kZhExtract);
  const std::string* favorites = zh.Find("sidebar.section.favorites");
  PRISM_CHECK(favorites != nullptr);
  if (favorites != nullptr) PRISM_CHECK_EQ(*favorites, kZhFavorites);
  const std::string* files = zh.Find("status.files.other");
  PRISM_CHECK(files != nullptr);
  if (files != nullptr) PRISM_CHECK_EQ(*files, kZhFiles);
  const std::string* summary = zh.Find("skip.summary");
  PRISM_CHECK(summary != nullptr);
  if (summary != nullptr) PRISM_CHECK_EQ(*summary, kZhSkipSummary);
  const std::string* failed = zh.Find("progress.failed");
  PRISM_CHECK(failed != nullptr);
  if (failed != nullptr) PRISM_CHECK_EQ(*failed, kZhFailed);
  const std::string* noArchive = zh.Find("sidebar.empty.noArchive");
  PRISM_CHECK(noArchive != nullptr);
  if (noArchive != nullptr) PRISM_CHECK_EQ(*noArchive, kZhArchiveOpen);

  int translated = 0;
  for (const prism::i18n::StringEntry& entry : zh.entries()) {
    PRISM_CHECK(IsValidUtf8(entry.value));
    PRISM_CHECK(!HasControlChars(entry.value));
    if (HasNonAscii(entry.value)) ++translated;
  }
  // Chinese tables keep ASCII tokens (placeholders, "7z.dll") on purpose; the
  // majority of values must still carry CJK text or this is an English copy.
  PRISM_CHECK(translated >= 30);
}

PRISM_TEST("catalog: every required surface is present in both tables") {
  Catalog en;
  Catalog zh;
  PRISM_CHECK(LoadShippedCatalog("en-US", &en));
  PRISM_CHECK(LoadShippedCatalog("zh-CN", &zh));
  for (const std::string& key : prism::i18n::RequiredKeys()) {
    const std::string* enValue = en.Find(key);
    const std::string* zhValue = zh.Find(key);
    if (enValue == nullptr || zhValue == nullptr) {
      prism::test::ReportFailure(__FILE__, __LINE__, "missing required key " + key);
      continue;
    }
    PRISM_CHECK(!enValue->empty());
    PRISM_CHECK(!zhValue->empty());
    PRISM_CHECK(zhValue->size() != 0);
  }
  // The surface list is part of the contract, so its size is asserted, not
  // inferred: 9 menu keys, 4 sidebar, 10 capsule, 7 actions, 11 errors, 4 skip,
  // 5 progress = 50. Card C38 added the eleven keys the exe was already asking
  // for through Tr::Or() code defaults (menu.file, action.exit, the compression
  // progress three, status.refreshDeferred, and the five CLI/verification
  // errors); that is why this number moved from 39 -- and it moves only when
  // RequiredKeys() moves, which is the point of the census.
  PRISM_CHECK_EQ(prism::i18n::RequiredKeys().size(), std::size_t{51});
}

PRISM_TEST("catalog: the right-click menu's REAL verbs all resolve") {
  Catalog en;
  Catalog zh;
  PRISM_CHECK(LoadShippedCatalog("en-US", &en));
  PRISM_CHECK(LoadShippedCatalog("zh-CN", &zh));

  // Three selections reach all eight verbs of ContextMenuModel: one archive,
  // two archives, and a mixed selection (archive + plain file).
  const std::vector<std::vector<std::string>> selections = {
      {"report.7z"},
      {"a.7z", "b.7z"},
      {"report.7z", "notes.txt"},
  };
  std::size_t seen = 0;
  for (const std::vector<std::string>& selection : selections) {
    const std::vector<prism::MenuCommand> items = prism::ContextMenuModel::Build(selection);
    PRISM_CHECK(!items.empty());
    for (const prism::MenuCommand& item : items) {
      ++seen;
      const std::string key = "menu." + item.verb;
      const std::string* enValue = en.Find(key);
      PRISM_CHECK(enValue != nullptr);
      if (enValue == nullptr) {
        prism::test::ReportFailure(__FILE__, __LINE__, "no string for verb " + item.verb);
        continue;
      }
      // The rendered label must reproduce the model's own title, with the model's
      // titleArgument filling {name}. That is the wiring contract: the verb id is
      // the key, and the payload is the only substitution.
      const std::string rendered =
          prism::i18n::FormatPattern(*enValue, {{"name", item.titleArgument}});
      PRISM_CHECK_EQ(rendered, item.title);
      PRISM_CHECK(zh.Find(key) != nullptr);
      const std::string zhRendered =
          prism::i18n::FormatPattern(*zh.Find(key), {{"name", item.titleArgument}});
      PRISM_CHECK(!zhRendered.empty());
      if (item.verb == "extract.to.subfolder") {
        PRISM_CHECK(zhRendered.find("{name}") == std::string::npos);
        PRISM_CHECK(zhRendered.find(item.titleArgument) != std::string::npos);
      }
    }
  }
  PRISM_CHECK_EQ(seen, std::size_t{15});  // 5 + 3 + 7 items across the selections
  // The five single-archive items are the documented minimum.
  const std::vector<prism::MenuCommand> single = prism::ContextMenuModel::Build({"report.7z"});
  PRISM_CHECK_EQ(single.size(), std::size_t{5});
  PRISM_CHECK_EQ(single[0].verb, std::string("smart.extract.here"));
  PRISM_CHECK(single[0].isDefault);
  PRISM_CHECK_EQ(single[4].verb, std::string("open.in.prism"));
}

// ---------------------------------------------------------------------------
// 2. Lookup and fallback.
// ---------------------------------------------------------------------------
PRISM_TEST("lookup: exact key wins in the active language") {
  Translator translator = MakeTranslator("zh-CN");
  PRISM_CHECK_EQ(translator.Translate("menu.extract.here"), kZhExtractHere);
  PRISM_CHECK_EQ(translator.Translate("skip.summary", {{"n", "4"}}), std::string("\xE8\xB7\xB3\xE8\xBF\x87 4 \xE9\xA1\xB9"));
  PRISM_CHECK(translator.MissingKeys().empty());

  translator.SetLanguage("en-US");
  PRISM_CHECK_EQ(translator.Translate("menu.extract.here"), std::string("Extract Here"));
  PRISM_CHECK(translator.MissingKeys().empty());
}

PRISM_TEST("lookup: zh-CN -> zh -> en-US, each level asserted by content") {
  // A deliberately partial regional table, exactly what the chain exists for.
  Catalog zhCN;
  Catalog zh;
  Catalog en;
  std::string error;
  PRISM_CHECK(zhCN.LoadFromJson(
      R"({"language":"zh-CN","strings":{"action.extract":"\u89E3\u538B"}})", &error));
  PRISM_CHECK(zh.LoadFromJson(
      R"({"language":"zh","strings":{"action.cancel":"\u53D6\u6D88","status.ratio":"\u538B\u7F29\u6BD4 {percent}"}})",
      &error));
  PRISM_CHECK(LoadShippedCatalog("en-US", &en));

  Translator translator;
  PRISM_CHECK(translator.AddCatalog(std::move(zhCN), &error));
  PRISM_CHECK(translator.AddCatalog(std::move(zh), &error));
  PRISM_CHECK(translator.AddCatalog(std::move(en), &error));
  translator.SetLanguage("zh-CN");

  PRISM_CHECK_EQ(JoinChain(translator.chain()), std::string("zh-CN,zh,en-US"));
  PRISM_CHECK_EQ(translator.Translate("action.extract"), kZhExtract);          // zh-CN
  PRISM_CHECK_EQ(translator.Translate("action.cancel"), std::string("\xE5\x8F\x96\xE6\xB6\x88"));  // zh
  PRISM_CHECK_EQ(translator.Translate("menu.extract.here"), std::string("Extract Here"));  // en-US
  PRISM_CHECK(translator.MissingKeys().empty());  // a fallback hit is NOT a miss

  // The chain is capped at three levels and always ends on en-US.
  PRISM_CHECK_EQ(JoinChain(prism::i18n::ChainFor("zh-CN")), std::string("zh-CN,zh,en-US"));
  PRISM_CHECK_EQ(JoinChain(prism::i18n::ChainFor("en-US")), std::string("en-US"));
  PRISM_CHECK_EQ(JoinChain(prism::i18n::ChainFor("ja-JP")), std::string("ja-JP,ja,en-US"));
  PRISM_CHECK_EQ(JoinChain(prism::i18n::ChainFor("")), std::string("en-US"));
  PRISM_CHECK(prism::i18n::ChainFor("zh-Hans-CN").size() == 3);

  // Language spelling is canonicalized, not compared case-sensitively: a
  // hand-edited settings file says "zh_cn" and must still work.
  translator.SetLanguage("zh_cn");
  PRISM_CHECK_EQ(translator.language(), std::string("zh-CN"));
  PRISM_CHECK_EQ(JoinChain(translator.chain()), std::string("zh-CN,zh,en-US"));
}

PRISM_TEST("lookup: a missing key returns the key and is counted once per key") {
  Translator translator = MakeTranslator("zh-CN");
  translator.ClearMissing();
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{0});

  PRISM_CHECK_EQ(translator.Translate("menu.does.not.exist"),
                 std::string("menu.does.not.exist"));
  PRISM_CHECK_EQ(translator.Translate("menu.does.not.exist"),
                 std::string("menu.does.not.exist"));
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{2});      // every hit counted
  PRISM_CHECK_EQ(translator.MissingKeys().size(), std::size_t{1});  // unique list
  if (!translator.MissingKeys().empty()) {
    PRISM_CHECK_EQ(translator.MissingKeys()[0], std::string("menu.does.not.exist"));
  }

  // A plural key with no table entry at all is one miss on the BASE key, so the
  // gate reads "status.missing" instead of ".one"/".other" noise.
  PRISM_CHECK_EQ(translator.TranslateCount("status.missing", 2), std::string("status.missing"));
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{3});
  PRISM_CHECK_EQ(translator.MissingKeys().size(), std::size_t{2});

  // Has() answers without polluting the miss list.
  PRISM_CHECK(translator.Has("action.extract"));
  PRISM_CHECK(!translator.Has("nope.nope"));
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{3});

  // A successful lookup after failures changes nothing either.
  PRISM_CHECK_EQ(translator.Translate("action.cancel"), std::string("\xE5\x8F\x96\xE6\xB6\x88"));
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{3});

  translator.ClearMissing();
  PRISM_CHECK_EQ(translator.MissCount(), std::size_t{0});
  PRISM_CHECK(translator.MissingKeys().empty());
}

// ---------------------------------------------------------------------------
// 3. Placeholders and plurals.
// ---------------------------------------------------------------------------
PRISM_TEST("placeholders: filled once, unknown names preserved, no rescan") {
  Catalog probe;
  std::string error;
  PRISM_CHECK(probe.LoadFromJson(R"({"language":"en-US","strings":{
      "pair":"{a} of {b}",
      "numbers":"{n} / {total}",
      "braced":"{a}",
      "unterminated":"tail {a",
      "empty":"{}"}})",
                                 &error));
  Translator translator;
  PRISM_CHECK(translator.AddCatalog(std::move(probe), &error));
  translator.SetLanguage("en-US");

  PRISM_CHECK_EQ(translator.Translate("pair", {{"a", "1"}, {"b", "2"}}), std::string("1 of 2"));
  PRISM_CHECK_EQ(translator.Translate("numbers", {{"n", "3"}, {"total", "9"}}),
                 std::string("3 / 9"));
  // An unsupplied placeholder stays visible (deleting it would silently ship a
  // lie); the rest of the pattern is still substituted.
  PRISM_CHECK_EQ(translator.Translate("pair", {{"a", "1"}}), std::string("1 of {b}"));
  PRISM_CHECK_EQ(translator.Translate("pair"), std::string("{a} of {b}"));
  // Values are appended, never rescanned: a value that looks like a placeholder
  // must not be expanded again.
  PRISM_CHECK_EQ(translator.Translate("braced", {{"a", "{b}"}, {"b", "boom"}}),
                 std::string("{b}"));
  PRISM_CHECK_EQ(translator.Translate("unterminated", {{"a", "x"}}), std::string("tail {a"));
  PRISM_CHECK_EQ(translator.Translate("empty", {{"a", "x"}}), std::string("{}"));
  // Direct helper use is the same code path the Win layer will call.
  PRISM_CHECK_EQ(prism::i18n::FormatPattern("v{v} ({major}.{minor})",
                                            {{"v", "1"}, {"major", "2"}, {"minor", "3"}}),
                 std::string("v1 (2.3)"));
}

PRISM_TEST("placeholders: plural selection uses .one / .other and falls back") {
  Translator en = MakeTranslator("en-US");
  PRISM_CHECK_EQ(en.TranslateCount("status.files", 1), std::string("1 FILE"));
  PRISM_CHECK_EQ(en.TranslateCount("status.files", 2), std::string("2 FILES"));
  PRISM_CHECK_EQ(en.TranslateCount("status.files", 0), std::string("0 FILES"));
  PRISM_CHECK_EQ(en.TranslateCount("status.folders", 1), std::string("1 FOLDER"));
  PRISM_CHECK_EQ(en.TranslateCount("status.folders", 7), std::string("7 FOLDERS"));

  Translator zh = MakeTranslator("zh-CN");
  PRISM_CHECK_EQ(zh.TranslateCount("status.files", 1), std::string("1 ") + std::string("\xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6"));
  PRISM_CHECK_EQ(zh.TranslateCount("status.files", 12), std::string("12 ") + std::string("\xE4\xB8\xAA\xE6\x96\x87\xE4\xBB\xB6"));
  PRISM_CHECK_EQ(zh.TranslateCount("skip.summary", 3), std::string("\xE8\xB7\xB3\xE8\xBF\x87 3 \xE9\xA1\xB9"));
}

// ---------------------------------------------------------------------------
// 4. Language selection.
// ---------------------------------------------------------------------------
PRISM_TEST("language: settings value, auto -> system, invalid -> en-US once") {
  // Explicit, supported, and spelled sloppily.
  LanguageChoice choice = prism::i18n::ChooseLanguage("zh-CN", "en-US");
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  PRISM_CHECK_EQ(choice.source, std::string("settings"));
  PRISM_CHECK_EQ(choice.validationFailures, 0);
  PRISM_CHECK(!choice.followedSystem);

  choice = prism::i18n::ChooseLanguage("zh_cn", "en-US");
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  PRISM_CHECK_EQ(choice.validationFailures, 0);

  // auto / empty follow the system, including a locale Windows spells with a
  // script ("zh-Hans-CN" -> the zh-CN table).
  choice = prism::i18n::ChooseLanguage("auto", "zh-CN");
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  PRISM_CHECK_EQ(choice.source, std::string("system"));
  PRISM_CHECK(choice.followedSystem);
  PRISM_CHECK_EQ(choice.validationFailures, 0);

  choice = prism::i18n::ChooseLanguage("", "zh-Hans-CN");
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  choice = prism::i18n::ChooseLanguage("auto", "en-GB");
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));

  // An invalid value lands on en-US and records EXACTLY ONE failure, naming the
  // value the user must fix.
  choice = prism::i18n::ChooseLanguage("klingon", "zh-CN");
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));
  PRISM_CHECK_EQ(choice.source, std::string("fallback"));
  PRISM_CHECK_EQ(choice.validationFailures, 1);
  PRISM_CHECK(choice.detail.find("klingon") != std::string::npos);
  PRISM_CHECK(!choice.followedSystem);

  // A system locale this build has no table for is also one recorded failure --
  // never a silent half-English UI with a guessed tag.
  choice = prism::i18n::ChooseLanguage("auto", "fr-FR");
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));
  PRISM_CHECK_EQ(choice.validationFailures, 1);

  // ja-JP is legal for the settings schema but has no table: unsupported, and
  // reported, not silently accepted.
  choice = prism::i18n::ChooseLanguage("ja-JP", "zh-CN");
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));
  PRISM_CHECK_EQ(choice.validationFailures, 1);

  // The choice implies the chain the translator will use.
  PRISM_CHECK_EQ(JoinChain(prism::i18n::ChainForChoice(prism::i18n::ChooseLanguage("zh-CN", "en-US"))),
                 std::string("zh-CN,zh,en-US"));
}

PRISM_TEST("language: language.tag from the C14 store drives the choice") {
  prism::settings::SettingsStore store;
  PRISM_CHECK_EQ(store.GetString("language.tag", "auto"), std::string("auto"));

  // A document that names zh-CN: the store keeps it, the app renders Chinese.
  const std::string chinese = R"({"schemaVersion":3,"language":{"tag":"zh-CN"}})";
  PRISM_CHECK(store.Load(chinese).readable);
  PRISM_CHECK(store.report().error.empty());  // a valid document needs no complaint
  LanguageChoice choice = prism::i18n::ChooseLanguageFromSettings(store, "en-US");
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  PRISM_CHECK_EQ(choice.validationFailures, 0);

  // ja-JP is inside the store's allow-list (schema v3) but outside this build's
  // tables: the store accepts it, the language choice reports the failure.
  const std::string japanese = R"({"schemaVersion":3,"language":{"tag":"ja-JP"}})";
  PRISM_CHECK(store.Load(japanese).readable);
  PRISM_CHECK_EQ(store.GetString("language.tag", "auto"), std::string("ja-JP"));
  choice = prism::i18n::ChooseLanguageFromSettings(store, "zh-CN");
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));
  PRISM_CHECK_EQ(choice.validationFailures, 1);
  PRISM_CHECK(choice.detail.find("ja-JP") != std::string::npos);

  // auto + the injected system locale (in the app: GetUserDefaultLocaleName).
  const std::string automatic = R"({"schemaVersion":3,"language":{"tag":"auto"}})";
  PRISM_CHECK(store.Load(automatic).readable);
  prism::i18n::SetSystemLocaleOverride("zh-CN");
  PRISM_CHECK_EQ(prism::i18n::SystemLocaleName(), std::string("zh-CN"));
  choice = prism::i18n::ChooseLanguageFromSettings(store, prism::i18n::SystemLocaleName());
  PRISM_CHECK_EQ(choice.tag, std::string("zh-CN"));
  PRISM_CHECK(choice.followedSystem);
  PRISM_CHECK_EQ(choice.validationFailures, 0);

  prism::i18n::SetSystemLocaleOverride("pt_BR");
  PRISM_CHECK_EQ(prism::i18n::SystemLocaleName(), std::string("pt-BR"));
  choice = prism::i18n::ChooseLanguageFromSettings(store, prism::i18n::SystemLocaleName());
  PRISM_CHECK_EQ(choice.tag, std::string("en-US"));
  PRISM_CHECK_EQ(choice.validationFailures, 1);
  prism::i18n::SetSystemLocaleOverride("");
  PRISM_CHECK(prism::i18n::SystemLocaleName().empty());
}

// ---------------------------------------------------------------------------
// 5. Bad resource files.
// ---------------------------------------------------------------------------
PRISM_TEST("resources: a rejected table is loud and never half-loads") {
  Catalog probe;
  std::string error;
  PRISM_CHECK(!probe.LoadFromJson("{}", &error));
  PRISM_CHECK(error.find("language") != std::string::npos);

  PRISM_CHECK(!probe.LoadFromJson(R"({"language":"xx","strings":{}})", &error));
  PRISM_CHECK(!probe.LoadFromJson(R"({"language":"xx"})", &error));
  PRISM_CHECK(!probe.LoadFromJson(R"({"language":"xx","strings":{"a":1}})", &error));
  PRISM_CHECK(!probe.LoadFromJson(R"({"language":"xx","strings":{"":""}})", &error));
  PRISM_CHECK(!probe.LoadFromJson(R"({"language":"xx","schemaVersion":2,"strings":{"a":"b"}})",
                                  &error));
  PRISM_CHECK(!probe.LoadFromJson("{", &error));
  PRISM_CHECK(error.find("invalid JSON at byte") != std::string::npos);
  PRISM_CHECK(!probe.LoadFromJson("[]", &error));
  PRISM_CHECK(probe.size() == 0 && probe.language().empty());

  // Duplicate keys: the C14 parser's documented last-wins rule, inherited here.
  PRISM_CHECK(probe.LoadFromJson(R"({"language":"en-US","strings":{"a":"1","a":"2"}})", &error));
  PRISM_CHECK_EQ(probe.size(), std::size_t{1});
  const std::string* value = probe.Find("a");
  PRISM_CHECK(value != nullptr);
  if (value != nullptr) PRISM_CHECK_EQ(*value, std::string("2"));

  // A failed file load leaves the translator exactly as it was.
  Translator translator = MakeTranslator("en-US");
  const std::size_t before = translator.catalogs().size();
  PRISM_CHECK(!translator.LoadCatalogFile(LangPath("xx-YY.json"), &error));
  PRISM_CHECK_EQ(translator.catalogs().size(), before);
  PRISM_CHECK_EQ(translator.Translate("action.extract"), std::string("Extract"));
  PRISM_CHECK(translator.MissingKeys().empty());

  // Replacing a table in place keeps the active chain working (hot reload).
  Catalog reloaded;
  PRISM_CHECK(reloaded.LoadFromJson(
      R"({"language":"zh-CN","strings":{"action.extract":"\u89E3\u538B!"}})", &error));
  PRISM_CHECK(translator.AddCatalog(std::move(reloaded), &error));
  translator.SetLanguage("zh-CN");
  PRISM_CHECK_EQ(translator.Translate("action.extract"), std::string("\xE8\xA7\xA3\xE5\x8E\x8B!"));
  // zh-CN lost every other key in this replacement, so the chain serves them
  // from en-US -- and none of those fallbacks counts as a miss.
  PRISM_CHECK_EQ(translator.Translate("action.cancel"), std::string("Cancel"));
  PRISM_CHECK_EQ(translator.Translate("sidebar.empty.noArchive"), std::string("NO ARCHIVE OPEN"));
  PRISM_CHECK(translator.MissingKeys().empty());
}

// ---------------------------------------------------------------------------
// 6. The suite's own verdict.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  // D3 instrument: this mode proves the compile-time injection actually reached
  // the header. The CMake entry pins "C19 MUTANT ACTIVE: <id>" as its PASS
  // regex, so a mutation that never compiled in is RED, not a silent no-op.
  if (argc > 1 && std::string_view(argv[1]) == "--mutation-report") {
    const char* mutation = prism::i18n::ActiveMutation();
    if (mutation == nullptr) {
      std::cout << "C19 MUTANT ACTIVE: none -- no injection reached I18n.hpp" << std::endl;
      return 1;
    }
    std::cout << "C19 MUTANT ACTIVE: " << mutation << std::endl;
    return 0;
  }
  const int failed = prism::test::RunAll(argc, argv);
  if (failed == 0) std::cout << "ALL I18N CHECKS PASSED" << std::endl;
  return failed;
}
