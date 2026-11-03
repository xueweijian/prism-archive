// PrismArchive -- PrismCore tests (card C74)
// The action band's labels are KEYS in the table (actionbar.open, ...) and the
// caller injects the active language's eight words. This suite proves the
// contract that made a language switch possible:
//
//   * the table carries keys, and every key is in RequiredKeys() -- a table that
//     drops one fails the i18n census, not the screen;
//   * an injected ENGLISH table paints eight English words;
//   * an injected CHINESE table paints eight Chinese words;
//   * NO table falls back to the built-in words and never crashes (the band must
//     never paint an empty button, and PrismCore cannot include the translator);
//   * a wrong-sized table is rejected instead of half-applied.
//
// NEGATIVE CONTROL (CONTRACTS D3 + D26): the same source is compiled with
// PRISM_C74_MUTANT_FIXED_LABELS, where SetLabels() is ignored and every lookup
// returns the built-in word (the pre-C74 band). The two injected-table cases
// MUST go red; the verdict is the "C74 MUTATION CAUGHT: fixed_labels" marker,
// printed at the first red assertion and only when the injection is proved
// active (D27).
#include <array>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "ActionBarModel.hpp"
#include "I18n.hpp"
#include "mini_test.hpp"

using namespace prism::actionbar;

namespace {

constexpr std::size_t kCount = static_cast<std::size_t>(kActionCount);

std::array<std::string, kCount> EnglishLabels() {
  return {"Open", "Extract to...", "Compress", "Add",
          "Delete", "Test", "View", "Search"};
}

std::array<std::string, kCount> ChineseLabels() {
  // The exact built-in words, as UTF-8 byte escapes so a compiler that reads
  // this file as ANSI cannot turn them into mojibake and still pass.
  return {"\xE6\x89\x93\xE5\xBC\x80",                                      // 打开
          "\xE8\xA7\xA3\xE5\x8E\x8B\xE5\x88\xB0\xE2\x80\xA6",              // 解压到…
          "\xE5\x8E\x8B\xE7\xBC\xA9",                                      // 压缩
          "\xE6\xB7\xBB\xE5\x8A\xA0",                                      // 添加
          "\xE5\x88\xA0\xE9\x99\xA4",                                      // 删除
          "\xE6\xB5\x8B\xE8\xAF\x95",                                      // 测试
          "\xE6\x9F\xA5\xE7\x9C\x8B",                                      // 查看
          "\xE6\x90\x9C\xE7\xB4\xA2"};                                     // 搜索
}

}  // namespace

PRISM_TEST("labels: the table carries resource keys, not compiled-in words") {
  std::set<std::string> seen;
  const std::vector<std::string>& required = prism::i18n::RequiredKeys();
  for (int i = 0; i < kActionCount; ++i) {
    const std::string key(kActions[static_cast<std::size_t>(i)].key);
    PRISM_CHECK(!key.empty());
    PRISM_CHECK(key.rfind("actionbar.", 0) == 0);  // the band's own namespace
    PRISM_CHECK(seen.insert(key).second);          // one key per action
    bool required_key = false;
    for (const std::string& candidate : required) {
      if (candidate == key) required_key = true;
    }
    PRISM_CHECK(required_key);
  }
  PRISM_CHECK_EQ(seen.size(), kCount);
  // The built-in words are the fallback, never the key: a band that painted
  // "actionbar.open" would be worse than one that painted a stale word.
  for (std::size_t i = 0; i < kCount; ++i) {
    PRISM_CHECK(!DefaultLabels()[i].empty());
    PRISM_CHECK(DefaultLabels()[i] != std::string(kActions[i].key));
  }
}

PRISM_TEST("labels: an injected English table paints all eight English words") {
  const std::array<std::string, kCount> english = EnglishLabels();
  Model model;
  model.SetLabels(english);
  PRISM_CHECK(model.HasInjectedLabels());
  for (int i = 0; i < kActionCount; ++i) {
    PRISM_CHECK_EQ(model.Label(i), english[static_cast<std::size_t>(i)]);
    PRISM_CHECK_EQ(model.LabelOf(kActions[static_cast<std::size_t>(i)].id),
                   english[static_cast<std::size_t>(i)]);
  }
}

PRISM_TEST("labels: an injected Chinese table paints all eight Chinese words") {
  const std::array<std::string, kCount> chinese = ChineseLabels();
  Model model;
  model.SetLabels(chinese);
  for (int i = 0; i < kActionCount; ++i) {
    PRISM_CHECK_EQ(model.Label(i), chinese[static_cast<std::size_t>(i)]);
  }
  // The two tables really differ (otherwise "the word changed" would be a lie).
  const std::array<std::string, kCount> english = EnglishLabels();
  for (std::size_t i = 0; i < kCount; ++i) {
    PRISM_CHECK(english[i] != chinese[i]);
  }
}

PRISM_TEST("labels: no table falls back to the built-in words and never crashes") {
  Model model;
  PRISM_CHECK(!model.HasInjectedLabels());
  for (int i = 0; i < kActionCount; ++i) {
    PRISM_CHECK(!model.Label(i).empty());
    PRISM_CHECK_EQ(model.Label(i), DefaultLabels()[static_cast<std::size_t>(i)]);
  }
  // Out-of-range indices are answered, not dereferenced past the table.
  PRISM_CHECK(!model.Label(-1).empty());
  PRISM_CHECK(!model.Label(kActionCount).empty());
  PRISM_CHECK(!model.Label(1000).empty());

  // A wrong-sized injection is REJECTED: seven translated words plus one stale
  // one is worse than the built-in default.
  const std::vector<std::string> shortTable = {"Open", "Extract", "Compress"};
  model.SetLabels(shortTable);
  PRISM_CHECK(!model.HasInjectedLabels());
  PRISM_CHECK_EQ(model.Label(0), DefaultLabels()[0]);

  // Clearing goes back to the default too.
  model.SetLabels(EnglishLabels());
  PRISM_CHECK(model.HasInjectedLabels());
  model.ClearLabels();
  PRISM_CHECK(!model.HasInjectedLabels());
  PRISM_CHECK_EQ(model.Label(0), DefaultLabels()[0]);
}

int main() {
#if defined(PRISM_C74_MUTANT_FIXED_LABELS)
  prism::test::SetMutationVerdict(
      "C74 MUTATION CAUGHT: fixed_labels",
      [] { return !prism::actionbar::kLabelInjectionEnabled; });
#endif
  const int failed = prism::test::RunAll(0, nullptr);
  if (failed == 0) std::puts("ALL C74 ACTIONBAR-LABEL CHECKS PASSED");
  return failed;
}
