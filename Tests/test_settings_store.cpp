// PrismArchive -- PrismCore settings store suite (card C14).
//
// What is proven here, in the order a real upgrade walks through it:
//   * defaults      -- a fresh store is the documented schema, not an empty map
//   * round trip    -- load -> save -> load is byte-stable, unknown keys and all
//   * unknown keys  -- a build that knows less must not destroy what it cannot name
//   * missing keys   -- absent fields are defaulted and REPORTED (not silently)
//   * migration     -- v1 -> v3 in one load, percent -> fraction, newer key wins
//   * corruption    -- invalid JSON is rejected with a byte offset, no data leaks
//   * too new       -- a future schema is readable but NEVER writable
//   * validation    -- type/range/allow-list/length repairs, recorded per path
//   * parser        -- escapes, surrogate pairs, number lexemes, depth bound
//
// CONTRACTS: D1 (the success marker below is part of the verdict), D2 (values
// are asserted, not existence), D3 (three negative controls: the same suite is
// rebuilt with one rule compiled out, and WILL_FAIL is the verdict -- a mutation
// that silently stops applying makes the suite pass and turns the mutant red).
// B6: LoadStatus is compared through C14_CHECK_STATUS instead of a top-level
// PRISM_CHECK_EQ, so no operator<< is needed for it.
#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

#include "SettingsStore.hpp"
#include "mini_test.hpp"

namespace {

using prism::settings::JsonParseError;
using prism::settings::JsonValue;
using prism::settings::LoadStatus;
using prism::settings::SettingsStore;

bool CheckStatus(const SettingsStore& store, LoadStatus want, const char* file, int line) {
  if (store.report().status == want) return true;
  prism::test::ReportFailure(
      file, line, std::string("load status: expected ") + prism::settings::LoadStatusName(want) +
                      ", got " + prism::settings::LoadStatusName(store.report().status));
  return false;
}

#define C14_CHECK_STATUS(store, want) CheckStatus((store), (want), __FILE__, __LINE__)

bool HasRepair(const SettingsStore& store, std::string_view path, std::string_view kind) {
  for (const prism::settings::Repair& repair : store.repairs()) {
    if (repair.path == path && repair.kind == kind) return true;
  }
  return false;
}

bool HasDefaulted(const SettingsStore& store, std::string_view path) {
  for (const std::string& defaulted : store.defaultedPaths()) {
    if (defaulted == path) return true;
  }
  return false;
}

bool HasMigration(const SettingsStore& store, std::string_view fragment) {
  for (const std::string& migration : store.appliedMigrations()) {
    if (migration.find(fragment) != std::string::npos) return true;
  }
  return false;
}

// A document that names every live key, so a load of it must need no fill-ups
// and no repairs. Used by the round-trip case.
const char* const kCurrentDocument = R"json({
  "schemaVersion": 3,
  "appearance": {"theme": "dark", "glassIntensity": 0.5, "accentColor": "#112233"},
  "archive": {"recentCount": 7},
  "extract": {"overwrite": "replace", "closeWhenDone": true, "verifyAfterExtract": false},
  "language": {"tag": "ja-JP"},
  "paths": {"lastExtractDir": "D:/work/out"},
  "window": {"width": 1600, "height": 1000}
})json";

}  // namespace

PRISM_TEST("defaults: a fresh store carries every documented default") {
  SettingsStore store;
  C14_CHECK_STATUS(store, LoadStatus::Defaults);
  PRISM_CHECK_EQ(store.schemaVersion(), SettingsStore::kSchemaVersion);
  PRISM_CHECK(store.writable());
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1200);
  PRISM_CHECK_EQ(store.GetInt("window.height", 0), 800);
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("system"));
  PRISM_CHECK(std::fabs(store.GetNumber("appearance.glassIntensity", 0.0) - 0.72) < 1e-12);
  PRISM_CHECK_EQ(store.GetString("appearance.accentColor"), std::string("#4F8CFF"));
  PRISM_CHECK_EQ(store.GetInt("archive.recentCount", 0), 12);
  PRISM_CHECK_EQ(store.GetString("extract.overwrite"), std::string("ask"));
  PRISM_CHECK(!store.GetBool("extract.closeWhenDone", true));
  PRISM_CHECK(store.GetBool("extract.verifyAfterExtract", false));
  PRISM_CHECK_EQ(store.GetString("language.tag"), std::string("auto"));
  PRISM_CHECK_EQ(store.GetString("paths.lastExtractDir"), std::string(""));
  // The deprecated v1 spelling is not part of the current document.
  PRISM_CHECK(!store.Has("ui.language"));
}

PRISM_TEST("round trip: load -> save -> load is byte-stable and value-equal") {
  SettingsStore store;
  const prism::settings::LoadReport report = store.Load(kCurrentDocument);
  PRISM_CHECK(report.readable);
  C14_CHECK_STATUS(store, LoadStatus::Ok);
  PRISM_CHECK(store.repairs().empty());
  PRISM_CHECK(store.defaultedPaths().empty());
  PRISM_CHECK(store.appliedMigrations().empty());
  PRISM_CHECK_EQ(report.sourceVersion, 3);
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("dark"));
  PRISM_CHECK_EQ(store.GetString("appearance.accentColor"), std::string("#112233"));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1600);
  PRISM_CHECK_EQ(store.GetInt("archive.recentCount", 0), 7);
  PRISM_CHECK(!store.GetBool("extract.verifyAfterExtract", true));

  const std::string first = store.ToJson();
  PRISM_CHECK(first.find("\"schemaVersion\": 3") != std::string::npos);

  SettingsStore reloaded;
  PRISM_CHECK(reloaded.Load(first).readable);
  C14_CHECK_STATUS(reloaded, LoadStatus::Ok);
  PRISM_CHECK_EQ(reloaded.ToJson(), first);

  // The tree itself, not just its text, is identical (D2: compare content).
  JsonValue left;
  JsonValue right;
  PRISM_CHECK(prism::settings::ParseJson(first, left));
  PRISM_CHECK(prism::settings::ParseJson(reloaded.ToJson(), right));
  PRISM_CHECK(prism::settings::JsonEquals(left, right));
}

PRISM_TEST("unknown keys: a build that knows less keeps what a newer build wrote") {
  SettingsStore store;
  const prism::settings::LoadReport report = store.Load(R"json({
  "schemaVersion": 3,
  "appearance": {"theme": "purple", "futureTint": "#101010"},
  "extract": {"closeWhenDone": false},
  "future.telemetry": {"enabled": true},
  "futureSplitView": {"pane": 2, "items": [1, 2, 3]}
})json");
  PRISM_CHECK(report.readable);
  // Unknown values survive both spellings: a nested subtree, and a literal
  // key that happens to contain a dot (which is a member name, not a path).
  PRISM_CHECK(store.Has("appearance.futureTint"));
  PRISM_CHECK_EQ(store.GetString("appearance.futureTint"), std::string("#101010"));
  PRISM_CHECK_EQ(store.GetInt("futureSplitView.pane", 0), 2);
  const JsonValue* telemetry = store.mutableRoot().Find("future.telemetry");
  PRISM_CHECK(telemetry != nullptr && telemetry->IsObject());
  const JsonValue* enabled =
      (telemetry != nullptr && telemetry->IsObject()) ? telemetry->Find("enabled") : nullptr;
  PRISM_CHECK(enabled != nullptr);
  PRISM_CHECK(enabled != nullptr && enabled->boolean);
  // A dotted key is not addressable as a path, so a read through one is a
  // miss -- it must return the caller's fallback, never a guessed value.
  PRISM_CHECK(store.GetBool("future.telemetry", true));
  // ...a known-but-invalid sibling is repaired without touching them...
  C14_CHECK_STATUS(store, LoadStatus::Repaired);
  PRISM_CHECK(HasRepair(store, "appearance.theme", "not-allowed"));
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("system"));

  // ...and they survive a save/reload cycle byte for byte.
  const std::string saved = store.ToJson();
  PRISM_CHECK(saved.find("\"future.telemetry\"") != std::string::npos);
  PRISM_CHECK(saved.find("\"futureTint\"") != std::string::npos);
  PRISM_CHECK(saved.find("\"futureSplitView\"") != std::string::npos);
  SettingsStore reloaded;
  reloaded.Load(saved);
  PRISM_CHECK_EQ(reloaded.GetString("appearance.futureTint"), std::string("#101010"));
  PRISM_CHECK_EQ(reloaded.GetInt("futureSplitView.pane", 0), 2);
  PRISM_CHECK_EQ(reloaded.GetInt("futureSplitView.items", 0), 0);  // arrays are not paths
  const JsonValue* splitView = reloaded.mutableRoot().Find("futureSplitView");
  PRISM_CHECK(splitView != nullptr && splitView->IsObject());
  const JsonValue* items =
      (splitView != nullptr && splitView->IsObject()) ? splitView->Find("items") : nullptr;
  PRISM_CHECK(items != nullptr && items->items.size() == static_cast<std::size_t>(3));
}

PRISM_TEST("missing fields: absent keys are defaulted, reported, and kept") {
  SettingsStore store;
  const prism::settings::LoadReport report =
      store.Load(R"json({"schemaVersion": 3, "appearance": {"theme": "dark"}})json");
  PRISM_CHECK(report.readable);
  C14_CHECK_STATUS(store, LoadStatus::FilledDefaults);
  PRISM_CHECK(store.repairs().empty());
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("dark"));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1200);
  PRISM_CHECK(HasDefaulted(store, "window.width"));
  PRISM_CHECK(HasDefaulted(store, "paths.lastExtractDir"));
  PRISM_CHECK(!HasDefaulted(store, "appearance.theme"));
  // A second load of the saved document needs no fill-ups any more.
  SettingsStore reloaded;
  reloaded.Load(store.ToJson());
  C14_CHECK_STATUS(reloaded, LoadStatus::Ok);
  PRISM_CHECK(reloaded.defaultedPaths().empty());
}

PRISM_TEST("migration: a v1 document becomes v3 and carries the language rename") {
  SettingsStore store;
  const prism::settings::LoadReport report = store.Load(R"json({
  "ui": {"language": "en-US"},
  "window": {"width": 1440, "height": 900},
  "extract": {"overwrite": "replace", "closeWhenDone": true}
})json");
  PRISM_CHECK(report.readable);
  C14_CHECK_STATUS(store, LoadStatus::Migrated);
  PRISM_CHECK_EQ(report.sourceVersion, 0);  // no schemaVersion field at all
  PRISM_CHECK_EQ(store.schemaVersion(), SettingsStore::kSchemaVersion);
  PRISM_CHECK_EQ(store.GetString("language.tag"), std::string("en-US"));
  PRISM_CHECK(!store.Has("ui.language"));
  PRISM_CHECK(HasMigration(store, "1->2"));
  PRISM_CHECK(HasMigration(store, "2->3"));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1440);
  // A key introduced in v3 is defaulted by the same load.
  PRISM_CHECK(store.GetBool("extract.verifyAfterExtract", false));
  PRISM_CHECK(HasDefaulted(store, "extract.verifyAfterExtract"));
  // The migrated document is stable: no second migration, no repairs.
  SettingsStore reloaded;
  reloaded.Load(store.ToJson());
  C14_CHECK_STATUS(reloaded, LoadStatus::Ok);
  PRISM_CHECK(reloaded.appliedMigrations().empty());
}

PRISM_TEST("migration: a v2 percent glass intensity becomes a v3 fraction") {
  SettingsStore store;
  store.Load(R"json({"schemaVersion": 2, "appearance": {"glassIntensity": 72}})json");
  C14_CHECK_STATUS(store, LoadStatus::Migrated);
  PRISM_CHECK_EQ(store.report().sourceVersion, 2);
  PRISM_CHECK(std::fabs(store.GetNumber("appearance.glassIntensity", 0.0) - 0.72) < 1e-9);
  PRISM_CHECK(store.ToJson().find("\"glassIntensity\": 0.72") != std::string::npos);

  // 1 was already a fraction in v2 (100% was written as 1): it must not be
  // divided again. The v3 range check is what tells the two apart.
  SettingsStore fraction;
  fraction.Load(R"json({"schemaVersion": 2, "appearance": {"glassIntensity": 1}})json");
  PRISM_CHECK(std::fabs(fraction.GetNumber("appearance.glassIntensity", 0.0) - 1.0) < 1e-9);
  PRISM_CHECK(fraction.repairs().empty());
}

PRISM_TEST("migration: a newer key wins over the legacy spelling it renames") {
  SettingsStore store;
  store.Load(R"json({"ui": {"language": "en-US"}, "language": {"tag": "zh-CN"}})json");
  C14_CHECK_STATUS(store, LoadStatus::Migrated);
  PRISM_CHECK_EQ(store.GetString("language.tag"), std::string("zh-CN"));
  PRISM_CHECK(!store.Has("ui.language"));
}

PRISM_TEST("corruption: invalid JSON is rejected with a byte offset and leaks nothing") {
  SettingsStore store;
  const prism::settings::LoadReport report =
      store.Load(R"json({"schemaVersion": 3, "future": {"telemetry": true}, "appearance": {)json");
  PRISM_CHECK(!report.readable);
  C14_CHECK_STATUS(store, LoadStatus::Corrupt);
  PRISM_CHECK_EQ(report.sourceVersion, 0);
  PRISM_CHECK(report.error.find("invalid JSON at byte ") != std::string::npos);
  // The store is usable again, at defaults...
  PRISM_CHECK(store.writable());
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("system"));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1200);
  // ...and keys from the broken document do not leak into them.
  PRISM_CHECK(!store.Has("future"));

  // Whitespace-only and non-object documents are corrupt too, not "empty".
  SettingsStore blank;
  PRISM_CHECK(!blank.Load("   ").readable);
  C14_CHECK_STATUS(blank, LoadStatus::Corrupt);
  SettingsStore array;
  const prism::settings::LoadReport arrayReport = array.Load("[1, 2, 3]");
  PRISM_CHECK(!arrayReport.readable);
  PRISM_CHECK(arrayReport.error.find("must be a JSON object") != std::string::npos);
  C14_CHECK_STATUS(array, LoadStatus::Corrupt);
  // A schemaVersion that is not a whole positive number is corrupt as well.
  SettingsStore badVersion;
  PRISM_CHECK(!badVersion.Load(R"json({"schemaVersion": "3"})json").readable);
  PRISM_CHECK(!badVersion.Load(R"json({"schemaVersion": 0})json").readable);
}

PRISM_TEST("too new: a future schema is readable but never writable") {
  SettingsStore store;
  const prism::settings::LoadReport report = store.Load(R"json({
  "schemaVersion": 99,
  "appearance": {"theme": "dark", "futureTint": "#101010"},
  "window": {"width": 999999}
})json");
  PRISM_CHECK(report.readable);
  C14_CHECK_STATUS(store, LoadStatus::TooNew);
  PRISM_CHECK_EQ(report.sourceVersion, 99);
  PRISM_CHECK(!store.writable());
  // Read as written: no repair, no migration, no reordering.
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 999999);
  PRISM_CHECK_EQ(store.GetString("appearance.futureTint"), std::string("#101010"));
  PRISM_CHECK(store.repairs().empty());
  PRISM_CHECK(store.appliedMigrations().empty());
  PRISM_CHECK(store.defaultedPaths().empty());
  // Writes are refused...
  PRISM_CHECK(!store.SetString("appearance.theme", "light"));
  PRISM_CHECK(!store.Erase("appearance.futureTint"));
  // ...and the serialized form keeps the future version instead of downgrading.
  const std::string saved = store.ToJson();
  PRISM_CHECK(saved.find("\"schemaVersion\": 99") != std::string::npos);
  PRISM_CHECK(saved.find("\"futureTint\"") != std::string::npos);
  SettingsStore reloaded;
  reloaded.Load(saved);
  C14_CHECK_STATUS(reloaded, LoadStatus::TooNew);
  PRISM_CHECK_EQ(reloaded.GetString("appearance.theme"), std::string("dark"));
}

PRISM_TEST("validation: bad types, ranges and enum values are repaired per path") {
  SettingsStore store;
  store.Load(R"json({
  "schemaVersion": 3,
  "appearance": {"theme": "magenta", "glassIntensity": 3.5, "accentColor": "#123456"},
  "archive": {"recentCount": -5},
  "extract": {"overwrite": 42, "closeWhenDone": "yes", "verifyAfterExtract": true},
  "language": {"tag": "klingon"},
  "paths": {"lastExtractDir": "D:/out"},
  "window": {"width": 999999, "height": "tall"}
})json");
  C14_CHECK_STATUS(store, LoadStatus::Repaired);
  PRISM_CHECK(HasRepair(store, "appearance.theme", "not-allowed"));
  PRISM_CHECK(HasRepair(store, "appearance.glassIntensity", "range"));
  PRISM_CHECK(HasRepair(store, "archive.recentCount", "range"));
  PRISM_CHECK(HasRepair(store, "extract.overwrite", "type"));
  PRISM_CHECK(HasRepair(store, "extract.closeWhenDone", "type"));
  PRISM_CHECK(HasRepair(store, "language.tag", "not-allowed"));
  PRISM_CHECK(HasRepair(store, "window.width", "range"));
  PRISM_CHECK(HasRepair(store, "window.height", "type"));
  PRISM_CHECK_EQ(store.GetString("appearance.theme"), std::string("system"));
  PRISM_CHECK(std::fabs(store.GetNumber("appearance.glassIntensity", 0.0) - 1.0) < 1e-9);
  PRISM_CHECK_EQ(store.GetInt("archive.recentCount", -1), 0);
  PRISM_CHECK_EQ(store.GetString("extract.overwrite"), std::string("ask"));
  PRISM_CHECK(!store.GetBool("extract.closeWhenDone", true));
  PRISM_CHECK_EQ(store.GetString("language.tag"), std::string("auto"));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 8192);
  PRISM_CHECK_EQ(store.GetInt("window.height", 0), 800);

  // A repaired document is clean on the next load: repairs never repeat.
  SettingsStore reloaded;
  reloaded.Load(store.ToJson());
  C14_CHECK_STATUS(reloaded, LoadStatus::Ok);

  // A non-object subtree is rebuilt and reported as a structure repair.
  SettingsStore shaped;
  shaped.Load(R"json({"schemaVersion": 3, "appearance": 7})json");
  C14_CHECK_STATUS(shaped, LoadStatus::Repaired);
  PRISM_CHECK(HasRepair(shaped, "appearance", "structure"));
  PRISM_CHECK_EQ(shaped.GetString("appearance.theme"), std::string("system"));

  // A fractional value in an integer slot is truncated, with a type repair.
  SettingsStore fractional;
  fractional.Load(R"json({"schemaVersion": 3, "window": {"width": 1200.75}})json");
  PRISM_CHECK(HasRepair(fractional, "window.width", "type"));
  PRISM_CHECK_EQ(fractional.GetInt("window.width", 0), 1200);
}

PRISM_TEST("validation: an over-long path is truncated on a UTF-8 boundary") {
  const std::string prefix(258, 'a');
  // 258 ASCII bytes + 3-byte CJK full stop, i.e. a 260-byte cap lands INSIDE
  // the multi-byte character -- the truncation must back off to the boundary.
  const std::string value = prefix + "\xE3\x80\x82" + "/tail";
  std::string document = "{\"schemaVersion\": 3, \"paths\": {\"lastExtractDir\": \"";
  document += value;
  document += "\"}}";

  SettingsStore store;
  store.Load(document);
  C14_CHECK_STATUS(store, LoadStatus::Repaired);
  PRISM_CHECK(HasRepair(store, "paths.lastExtractDir", "truncated"));
  const std::string trimmed = store.GetString("paths.lastExtractDir");
  PRISM_CHECK_EQ(trimmed.size(), prefix.size());
  PRISM_CHECK_EQ(trimmed, prefix);
  // The truncated document is valid UTF-8 and reloads without another repair.
  SettingsStore reloaded;
  reloaded.Load(store.ToJson());
  C14_CHECK_STATUS(reloaded, LoadStatus::Ok);
}

PRISM_TEST("parser: escapes, surrogate pairs and number lexemes are exact") {
  JsonValue value;
  PRISM_CHECK(prism::settings::ParseJson("\"\\u4e2d\\u6587\"", value));
  PRISM_CHECK_EQ(value.text, std::string("\xE4\xB8\xAD\xE6\x96\x87"));
  PRISM_CHECK(prism::settings::ParseJson("\"\\ud83d\\ude00\"", value));
  PRISM_CHECK_EQ(value.text, std::string("\xF0\x9F\x98\x80"));

  // A number we did not touch keeps its source lexeme...
  PRISM_CHECK(prism::settings::ParseJson("1.50", value));
  PRISM_CHECK_EQ(prism::settings::SerializeJson(value, {false, 0}), std::string("1.50"));
  PRISM_CHECK(prism::settings::ParseJson("1e2", value));
  PRISM_CHECK_EQ(prism::settings::SerializeJson(value, {false, 0}), std::string("1e2"));
  // ...while a value the store synthesizes stays readable.
  PRISM_CHECK_EQ(prism::settings::FormatJsonNumber(0.72), std::string("0.72"));
  PRISM_CHECK_EQ(prism::settings::FormatJsonNumber(8192.0), std::string("8192"));

  // Rejections that matter for a file on disk.
  PRISM_CHECK(!prism::settings::ParseJson("\"\\q\"", value));
  PRISM_CHECK(!prism::settings::ParseJson("{} trailing", value));
  PRISM_CHECK(!prism::settings::ParseJson("\"\t\"", value));
  PRISM_CHECK(!prism::settings::ParseJson("\"\\ud800\"", value));
  PRISM_CHECK(!prism::settings::ParseJson("\"\\udc00\"", value));
  PRISM_CHECK(!prism::settings::ParseJson("01", value));
  PRISM_CHECK(!prism::settings::ParseJson("1.", value));
  PRISM_CHECK(!prism::settings::ParseJson("", value));
  PRISM_CHECK(!prism::settings::ParseJson("\"unterminated", value));
}

PRISM_TEST("parser: nesting is bounded so a hostile file cannot exhaust the stack") {
  std::string deep(200, '[');
  deep += std::string(200, ']');
  JsonValue value;
  JsonParseError error;
  PRISM_CHECK(!prism::settings::ParseJson(deep, value, &error));
  PRISM_CHECK(!error.ok);
  PRISM_CHECK(error.message.find("nesting") != std::string::npos);
  PRISM_CHECK(prism::settings::ParseJson("[[[[[[[[[[1]]]]]]]]]]", value));
  PRISM_CHECK(value.type == prism::settings::JsonType::Array);
}

PRISM_TEST("api: dotted-path setters, erase, and refused malformed paths") {
  SettingsStore store;
  PRISM_CHECK(store.SetBool("extract.closeWhenDone", true));
  PRISM_CHECK(store.GetBool("extract.closeWhenDone", false));
  PRISM_CHECK(store.SetInt("window.width", 1440));
  PRISM_CHECK_EQ(store.GetInt("window.width", 0), 1440);
  PRISM_CHECK(store.SetNumber("appearance.glassIntensity", 0.4));
  PRISM_CHECK(std::fabs(store.GetNumber("appearance.glassIntensity", 0.0) - 0.4) < 1e-12);
  PRISM_CHECK(store.SetString("language.tag", "zh-CN"));
  PRISM_CHECK_EQ(store.GetString("language.tag"), std::string("zh-CN"));
  PRISM_CHECK(store.Erase("paths.lastExtractDir"));
  PRISM_CHECK(!store.Has("paths.lastExtractDir"));
  PRISM_CHECK_EQ(store.GetString("paths.lastExtractDir", "fallback"), std::string("fallback"));

  // A malformed path is a refused write, not a crash and not a silent success.
  PRISM_CHECK(!store.SetBool("", true));
  PRISM_CHECK(!store.SetBool("window..width", true));
  PRISM_CHECK(!store.SetBool(".window", true));
  PRISM_CHECK(!store.SetBool("window.", true));

  // The erased key comes back from the schema on the next load.
  SettingsStore reloaded;
  reloaded.Load(store.ToJson());
  PRISM_CHECK_EQ(reloaded.GetString("paths.lastExtractDir"), std::string(""));
  PRISM_CHECK(reloaded.GetBool("extract.closeWhenDone", false));
  PRISM_CHECK_EQ(reloaded.GetInt("window.width", 0), 1440);
}

int main(int argc, char** argv) {
  std::cout << "=== settings store (schema v" << SettingsStore::kSchemaVersion << ") ==="
            << std::endl;
#if defined(PRISM_C14_MUTANT_SKIP_MIGRATION)
  std::cout << "C14 MUTANT ACTIVE: SKIP_MIGRATION -- the migration cases MUST fail" << std::endl;
#endif
#if defined(PRISM_C14_MUTANT_DROP_UNKNOWN_KEYS)
  std::cout << "C14 MUTANT ACTIVE: DROP_UNKNOWN_KEYS -- the unknown-key cases MUST fail"
            << std::endl;
#endif
#if defined(PRISM_C14_MUTANT_IGNORE_VALIDATION)
  std::cout << "C14 MUTANT ACTIVE: IGNORE_VALIDATION -- the validation cases MUST fail"
            << std::endl;
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc == 0) std::cout << "ALL SETTINGS-STORE CHECKS PASSED" << std::endl;
  return rc;
}
