// PrismArchive -- PrismCore
// I18n: the en/zh resource tables, their lookup rules and the language choice.
//
// WHY THIS LIVES IN PrismCore
// The UI strings are decisions, not paint: which tag wins, what a missing key
// renders as, how "{n}" is filled. Keeping that here means the Linux CI lane
// proves the whole table (both languages, both fallback steps, every error and
// SKIP prompt) in milliseconds, without a window and without a runner that has
// a Chinese font installed. The Win layer only reads the file and asks.
//
// WHAT THE RESOURCE FILE LOOKS LIKE
//   {
//     "schemaVersion": 1,
//     "language": "en-US",
//     "strings": { "menu.extract.here": "Extract Here", ... }
//   }
// It is parsed by card C14's STRICT JSON reader (prism::settings::ParseJson),
// so the table inherits that parser's rules for free: escapes, surrogate pairs,
// duplicate keys (last wins), trailing garbage, 64-level nesting bound. No
// third-party JSON library -- that is a project rule, and reusing the DOM also
// means one parser to keep honest instead of two.
//
// THE LOOKUP CONTRACT
//   1. exact key in the active language;
//   2. the language fallback chain -- zh-CN -> zh -> en-US (a regional table may
//      be partial; the base-language table exists so a translator can land
//      "zh" first and "zh-CN" later);
//   3. the key itself, verbatim, and ONE recorded miss.
// A miss is never hidden: MissingKeys()/MissCount() are what the "no untranslated
// string shipped" gate reads. Returning the key is deliberate -- a UI that shows
// "menu.extract.here" is ugly but debuggable; an empty label is neither.
//
// PLACEHOLDERS
// Only "{name}" is a placeholder (name = [A-Za-z0-9_.]+). A name without a
// supplied value stays exactly as it was, including the braces: silently
// deleting it would quietly ship "3 items" where "3 items of 5" was meant.
// Values are appended, never re-scanned, so a translated value that itself
// contains "{n}" cannot recurse.
//
// CONTRACTS: B1 (parenthesized std::min/max -- no unparenthesized ones here),
// B2 (<cstdint> included by this header for the int64 count API), D1 (the suite's
// stdout marker is part of the verdict), D2 (values, not existence: the tests
// compare translated CONTENT), D3 (three compile-time negative controls below --
// each one is rebuilt as its own executable and WILL_FAIL is the verdict).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SettingsStore.hpp"

namespace prism::i18n {

// ---------------------------------------------------------------------------
// 0. Negative controls (CONTRACTS D3). Never defined in a shipping build.
//
// Each switch turns ONE rule off. The suite must go red; the CTest entry is
// WILL_FAIL, so red there is green. The inversion is the verdict: a mutation
// that silently stops applying leaves the suite green and turns THAT entry red
// (see Tests/suites/c19-i18n.cmake, which also builds a --mutation-report run
// that must print the marker -- that is the injection's own proof).
// ---------------------------------------------------------------------------
#if defined(PRISM_C19_MUTANT_NO_FALLBACK)
inline constexpr bool kFallbackEnabled = false;
#else
inline constexpr bool kFallbackEnabled = true;
#endif

#if defined(PRISM_C19_MUTANT_NO_PLACEHOLDER)
inline constexpr bool kPlaceholderEnabled = false;
#else
inline constexpr bool kPlaceholderEnabled = true;
#endif

#if defined(PRISM_C19_MUTANT_NO_MISSING_TRACKING)
inline constexpr bool kMissingTrackingEnabled = false;
#else
inline constexpr bool kMissingTrackingEnabled = true;
#endif

// The name of the injected mutation, or nullptr in a shipping build. The
// --mutation-report mode prints it; the CMake entry pins it as a PASS regex, so
// a macro that never reached this header is a RED entry, not a silent no-op.
inline const char* ActiveMutation() {
#if defined(PRISM_C19_MUTANT_NO_FALLBACK)
  return "no-fallback";
#elif defined(PRISM_C19_MUTANT_NO_PLACEHOLDER)
  return "no-placeholder";
#elif defined(PRISM_C19_MUTANT_NO_MISSING_TRACKING)
  return "no-missing-tracking";
#else
  return nullptr;
#endif
}

// ---------------------------------------------------------------------------
// 1. Language tags.
// ---------------------------------------------------------------------------

inline constexpr std::string_view kAutoTag = "auto";
inline constexpr std::string_view kFallbackTag = "en-US";
inline constexpr int kResourceSchemaVersion = 1;

inline std::string LowerAscii(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c);
  }
  return out;
}

// Canonical BCP-47-ish spelling: language lower, 4-letter script Title, 2-letter
// (or 3-digit) region upper, variants lower. Underscores become dashes because
// POSIX locales spell them that way and a hand-edited settings file will too.
inline std::string NormalizeTag(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  for (const char c : raw) {
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
    out.push_back(c == '_' ? '-' : c);
  }
  std::string canonical;
  canonical.reserve(out.size());
  std::size_t start = 0;
  int part = 0;
  for (;;) {
    const std::size_t dash = out.find('-', start);
    const std::size_t end = dash == std::string::npos ? out.size() : dash;
    std::string piece = out.substr(start, end - start);
    if (piece.empty()) break;  // malformed ("zh--CN"): stop at the empty piece
    if (part == 0) {
      piece = LowerAscii(piece);
    } else if (piece.size() == 4) {
      piece = LowerAscii(piece);
      piece[0] = static_cast<char>(piece[0] - 'a' + 'A');
      for (std::size_t i = 1; i < piece.size(); ++i) {
        if (piece[i] >= 'A' && piece[i] <= 'Z') piece[i] = static_cast<char>(piece[i] - 'A' + 'a');
      }
    } else if (piece.size() == 2) {
      for (char& ch : piece) {
        if (ch >= 'a' && ch <= 'z') ch = static_cast<char>(ch - 'a' + 'A');
      }
    } else {
      piece = LowerAscii(piece);
    }
    if (!canonical.empty()) canonical.push_back('-');
    canonical += piece;
    ++part;
    if (dash == std::string::npos) break;
    start = dash + 1;
  }
  return canonical;
}

inline bool SameTag(std::string_view a, std::string_view b) {
  return NormalizeTag(a) == NormalizeTag(b);
}

// The tags this build ships a table for. Anything else is a value the store may
// legally hold (its schema allow-list is wider) but the app cannot render, so
// the language choice reports a validation failure instead of guessing.
inline const std::vector<std::string>& SupportedTags() {
  static const std::vector<std::string> tags = {"en-US", "zh-CN"};
  return tags;
}

// Canonical spelling of a supported tag, or "" when the tag is not supported.
inline std::string CanonicalTag(std::string_view raw) {
  for (const std::string& tag : SupportedTags()) {
    if (SameTag(tag, raw)) return tag;
  }
  return std::string();
}

inline bool IsSupportedTag(std::string_view tag) { return !CanonicalTag(tag).empty(); }

// Base language of a tag: "zh-CN" -> "zh", "en-US" -> "en", "zh" -> "zh".
inline std::string BaseLanguage(std::string_view tag) {
  const std::string normalized = NormalizeTag(tag);
  const std::size_t dash = normalized.find('-');
  return dash == std::string::npos ? normalized : normalized.substr(0, dash);
}

// Best supported tag for an arbitrary locale name. Exact match first, then the
// base language ("zh-Hans-CN" -> "zh-CN", "en-GB" -> "en-US"). "" when this
// build has no table whose language matches.
inline std::string BestSupportedTag(std::string_view locale) {
  if (const std::string exact = CanonicalTag(locale); !exact.empty()) return exact;
  const std::string base = BaseLanguage(locale);
  if (base.empty()) return std::string();
  for (const std::string& tag : SupportedTags()) {
    if (BaseLanguage(tag) == base) return tag;
  }
  return std::string();
}

// The fallback chain for a tag, most specific first, kFallbackTag pinned last.
// zh-CN -> {zh-CN, zh, en-US}; en-US -> {en-US}; ja-JP -> {ja-JP, ja, en-US}
// (no Japanese table ships today, so every key lands on English -- deliberately
// better than a half-empty UI). Deduplicated, capped at three levels.
//
// The base-language level is skipped when it would only say "English" again:
// en-GB -> {en-GB, en-US}, not {en-GB, en, en-US}. The last level already IS
// English, so a separate "en" table would be a fourth copy of the same strings.
inline std::vector<std::string> ChainFor(std::string_view tag) {
  std::vector<std::string> chain;
  if (!kFallbackEnabled) {
    // NEGATIVE CONTROL: the chain collapses to the active tag, so a key that
    // only exists further down the chain resolves to nothing. The fallback
    // cases in the suite MUST go red.
    const std::string only = NormalizeTag(tag);
    if (!only.empty()) chain.push_back(only);
    return chain;
  }
  const std::string fallback(kFallbackTag);
  const std::string normalized = NormalizeTag(tag);
  if (!normalized.empty()) chain.push_back(normalized);
  const std::string base = BaseLanguage(normalized);
  const bool baseAddsALanguage =
      !base.empty() && base != normalized && base != BaseLanguage(fallback);
  if (baseAddsALanguage) chain.push_back(base);
  bool haveFallback = false;
  for (const std::string& candidate : chain) {
    if (SameTag(candidate, fallback)) haveFallback = true;
  }
  if (!haveFallback) chain.push_back(fallback);
  return chain;
}

// ---------------------------------------------------------------------------
// 2. One parsed resource table.
// ---------------------------------------------------------------------------

struct StringEntry {
  std::string key;
  std::string value;
};

// Declared before Catalog because Catalog::LoadFromFile calls it; the body is
// below so the table class reads first.
inline bool ReadTextFile(const std::string& path, std::string* out, std::string* error);

// A loaded table. LoadFromJson is strict on purpose: a half-readable table is a
// worse failure than a loud one, because the UI would silently mix languages.
class Catalog {
 public:
  bool LoadFromJson(std::string_view document, std::string* error = nullptr) {
    prism::settings::JsonValue root;
    prism::settings::JsonParseError parseError;
    if (!prism::settings::ParseJson(document, root, &parseError)) {
      return Fail(error, "invalid JSON at byte " + std::to_string(parseError.offset) + ": " +
                             parseError.message);
    }
    if (!root.IsObject()) {
      return Fail(error, std::string("the resource table must be a JSON object, got ") +
                             prism::settings::JsonTypeName(root.type));
    }
    if (const prism::settings::JsonValue* version = root.Find("schemaVersion")) {
      const bool whole = version->type == prism::settings::JsonType::Number &&
                         version->number == static_cast<double>(kResourceSchemaVersion);
      if (!whole) {
        return Fail(error, "schemaVersion must be " + std::to_string(kResourceSchemaVersion) +
                               ", got " + prism::settings::SerializeJson(
                                               *version, prism::settings::JsonWriteOptions{false, 0}));
      }
    }
    const prism::settings::JsonValue* language = root.Find("language");
    if (language == nullptr || !language->IsString() || language->text.empty()) {
      return Fail(error, "the resource table must name its language");
    }
    const prism::settings::JsonValue* strings = root.Find("strings");
    if (strings == nullptr || !strings->IsObject()) {
      return Fail(error, "the resource table must hold a \"strings\" object");
    }
    if (strings->members.empty()) return Fail(error, "the \"strings\" object is empty");

    // Build into locals and swap only on success: a rejected document must not
    // leave half of a table behind.
    std::string parsedLanguage = language->text;
    std::vector<StringEntry> parsed;
    parsed.reserve(strings->members.size());
    for (const prism::settings::JsonValue& member : strings->members) {
      if (member.key.empty()) return Fail(error, "a resource key is empty");
      if (!member.IsString()) {
        return Fail(error, "resource \"" + member.key + "\" must be a string, got " +
                               prism::settings::JsonTypeName(member.type));
      }
      parsed.push_back(StringEntry{member.key, member.text});
    }
    language_ = std::move(parsedLanguage);
    entries_ = std::move(parsed);
    return true;
  }

  bool LoadFromFile(const std::string& path, std::string* error = nullptr) {
    std::string text;
    if (!ReadTextFile(path, &text, error)) return false;
    return LoadFromJson(text, error);
  }

  const std::string& language() const { return language_; }
  const std::vector<StringEntry>& entries() const { return entries_; }
  std::size_t size() const { return entries_.size(); }

  const std::string* Find(std::string_view key) const {
    for (const StringEntry& entry : entries_) {
      if (entry.key == key) return &entry.value;
    }
    return nullptr;
  }

 private:
  static bool Fail(std::string* error, std::string message) {
    if (error != nullptr) *error = std::move(message);
    return false;
  }

  std::string language_;
  std::vector<StringEntry> entries_;
};

// Whole-file read for the asset loader. Standard C++ only: PrismCore never
// touches Win32 (preflight ARCH rule), and the Win layer passes the asset path.
inline bool ReadTextFile(const std::string& path, std::string* out, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    if (error != nullptr) *error = "cannot open " + path;
    return false;
  }
  std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
  if (!stream.good() && !stream.eof()) {
    if (error != nullptr) *error = "cannot read " + path;
    return false;
  }
  if (out != nullptr) *out = std::move(text);
  return true;
}

// ---------------------------------------------------------------------------
// 3. Placeholders.
// ---------------------------------------------------------------------------

struct Substitution {
  std::string_view name;
  std::string value;
};

inline bool IsPlaceholderChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
         c == '.';
}

inline const std::string* FindSubstitution(const std::vector<Substitution>& subs,
                                           std::string_view name) {
  for (const Substitution& sub : subs) {
    if (sub.name == name) return &sub.value;
  }
  return nullptr;
}

// Fills "{name}" from subs. Unknown names, empty "{}", nested "{a{b}}" and a
// value that itself contains braces are all handled without rescanning.
inline std::string FormatPattern(std::string_view pattern, const std::vector<Substitution>& subs) {
  if (!kPlaceholderEnabled) {
    // NEGATIVE CONTROL: the pattern is emitted verbatim, so every "{n}" stays
    // visible. The placeholder cases in the suite MUST go red.
    return std::string(pattern);
  }
  std::string out;
  out.reserve(pattern.size() + 16);
  std::size_t i = 0;
  while (i < pattern.size()) {
    if (pattern[i] != '{') {
      out.push_back(pattern[i++]);
      continue;
    }
    const std::size_t close = pattern.find('}', i + 1);
    if (close == std::string_view::npos) {  // unterminated: rest is literal
      out.append(pattern.substr(i));
      break;
    }
    const std::string_view name = pattern.substr(i + 1, close - i - 1);
    bool simple = !name.empty();
    for (const char c : name) {
      if (!IsPlaceholderChar(c)) {
        simple = false;
        break;
      }
    }
    const std::string* value = simple ? FindSubstitution(subs, name) : nullptr;
    if (value == nullptr) {
      out.append(pattern.substr(i, close - i + 1));  // leave "{name}" intact
    } else {
      out.append(*value);  // appended, never re-scanned
    }
    i = close + 1;
  }
  return out;
}

// ---------------------------------------------------------------------------
// 4. The translator: catalogs + active language + misses.
// ---------------------------------------------------------------------------

class Translator {
 public:
  // Registers (or replaces) a table under its own "language" value.
  bool AddCatalog(Catalog catalog, std::string* error = nullptr) {
    const std::string normalized = NormalizeTag(catalog.language());
    if (normalized.empty()) {
      if (error != nullptr) *error = "the resource table has no language tag";
      return false;
    }
    for (Catalog& existing : catalogs_) {
      if (NormalizeTag(existing.language()) == normalized) {
        existing = std::move(catalog);
        if (SameTag(normalized, language_)) RebuildChain();
        return true;
      }
    }
    catalogs_.push_back(std::move(catalog));
    if (SameTag(normalized, language_)) RebuildChain();
    return true;
  }

  bool LoadCatalogFile(const std::string& path, std::string* error = nullptr) {
    Catalog catalog;
    if (!catalog.LoadFromFile(path, error)) return false;
    return AddCatalog(std::move(catalog), error);
  }

  // Sets the active language and its fallback chain. The tag is canonicalized
  // (zh-cn -> zh-CN) so settings, assets and tests spell it the same way.
  void SetLanguage(std::string_view tag) {
    const std::string canonical = CanonicalTag(tag);
    language_ = canonical.empty() ? NormalizeTag(tag) : canonical;
    if (language_.empty()) language_ = std::string(kFallbackTag);
    RebuildChain();
  }

  const std::string& language() const { return language_; }
  const std::vector<std::string>& chain() const { return chain_; }
  const std::vector<Catalog>& catalogs() const { return catalogs_; }

  bool HasCatalog(std::string_view tag) const { return FindCatalog(tag) != nullptr; }

  std::size_t KeyCount(std::string_view tag) const {
    const Catalog* catalog = FindCatalog(tag);
    return catalog == nullptr ? 0 : catalog->size();
  }

  // True when some table in the chain defines the key. Does NOT record a miss.
  bool Has(std::string_view key) const {
    const std::string* text = nullptr;
    std::string source;
    return Resolve(key, &text, &source);
  }

  std::string Translate(std::string_view key) const { return Translate(key, {}); }

  std::string Translate(std::string_view key, const std::vector<Substitution>& subs) const {
    const std::string* text = nullptr;
    std::string source;
    if (!Resolve(key, &text, &source)) {
      RecordMiss(key);
      return std::string(key);  // the key itself: ugly, but debuggable
    }
    return FormatPattern(*text, subs);
  }

  // Plural form: "<key>.one" for n == 1, "<key>.other" otherwise, with {n}
  // filled in. A table that has no plural split falls back to the bare key, so
  // zh (no plural inflection) needs only one entry.
  std::string TranslateCount(std::string_view key, std::int64_t n) const {
    const std::string base(key);
    const std::string suffixed = base + (n == 1 ? ".one" : ".other");
    const std::string number = std::to_string(n);
    const std::vector<Substitution> subs = {{"n", number}};
    const std::string* text = nullptr;
    std::string source;
    if (Resolve(suffixed, &text, &source)) return FormatPattern(*text, subs);
    if (Resolve(base, &text, &source)) return FormatPattern(*text, subs);
    RecordMiss(base);
    return base;
  }

  // Unique keys that missed, in first-miss order. Duplicates collapse: a status
  // bar that misses one key 60 times a second must not grow a list without end.
  const std::vector<std::string>& MissingKeys() const { return missing_; }
  // Every miss, duplicates included -- the "how often did we fall through" count.
  std::size_t MissCount() const { return missCount_; }
  void ClearMissing() {
    missing_.clear();
    missCount_ = 0;
  }

 private:
  const Catalog* FindCatalog(std::string_view tag) const {
    const std::string normalized = NormalizeTag(tag);
    for (const Catalog& catalog : catalogs_) {
      if (NormalizeTag(catalog.language()) == normalized) return &catalog;
    }
    return nullptr;
  }

  bool Resolve(std::string_view key, const std::string** text, std::string* source) const {
    for (const std::string& tag : chain_) {
      const Catalog* catalog = FindCatalog(tag);
      if (catalog == nullptr) continue;
      if (const std::string* value = catalog->Find(key)) {
        *text = value;
        if (source != nullptr) *source = tag;
        return true;
      }
    }
    return false;
  }

  void RecordMiss(std::string_view key) const {
    if (!kMissingTrackingEnabled) {
      // NEGATIVE CONTROL: misses are not recorded, so the missing-statistics
      // cases in the suite MUST go red (the returned text stays the key).
      return;
    }
    ++missCount_;
    for (const std::string& known : missing_) {
      if (known == key) return;
    }
    missing_.emplace_back(key);
  }

  void RebuildChain() {
    chain_ = ChainFor(language_);
    if (chain_.empty()) chain_.push_back(std::string(kFallbackTag));
  }

  std::vector<Catalog> catalogs_;
  std::string language_ = std::string(kFallbackTag);
  std::vector<std::string> chain_ = ChainFor(kFallbackTag);
  mutable std::vector<std::string> missing_;
  mutable std::size_t missCount_ = 0;
};

// ---------------------------------------------------------------------------
// 5. Language selection: settings value -> a tag this build can render.
// ---------------------------------------------------------------------------

struct LanguageChoice {
  std::string requested;            // the raw settings value ("auto", "zh-CN", ...)
  std::string tag;                  // always a supported tag
  std::string source;               // "settings" | "system" | "fallback"
  bool followedSystem = false;      // the value asked for "auto"/nothing
  int validationFailures = 0;       // 0 or exactly 1 -- never a cascade
  std::string detail;               // machine-readable reason, for the log
};

// The system locale is an INPUT, not a dependency: PrismCore must not include
// <windows.h> (preflight ARCH rule), so the Windows layer calls
//   SetSystemLocaleOverride(utf8(GetUserDefaultLocaleName(...)))
// once at startup. Tests inject a fixed value; an empty override means "the app
// never told us", which falls back to en-US and is reported as a failure below.
inline std::string& SystemLocaleOverride() {
  static std::string value;
  return value;
}

inline void SetSystemLocaleOverride(std::string_view locale) {
  SystemLocaleOverride() = std::string(locale);
}

inline std::string SystemLocaleName() {
  const std::string& configured = SystemLocaleOverride();
  return configured.empty() ? std::string() : NormalizeTag(configured);
}

// Resolves one settings value against one system locale. "auto" and "" mean
// "follow the system"; anything this build has no table for is a validation
// failure that lands on en-US (never on a half-translated UI).
inline LanguageChoice ChooseLanguage(std::string_view requested, std::string_view systemLocale) {
  LanguageChoice choice;
  choice.requested = std::string(requested);
  const std::string trimmed = NormalizeTag(requested);

  const bool automatic = trimmed.empty() || SameTag(trimmed, kAutoTag);
  if (automatic) {
    choice.followedSystem = true;
    const std::string system = NormalizeTag(systemLocale);
    const std::string best = BestSupportedTag(system);
    if (!best.empty()) {
      choice.tag = best;
      choice.source = "system";
      choice.detail = "followed system locale " + (system.empty() ? std::string("(unset)") : system);
      return choice;
    }
    choice.tag = std::string(kFallbackTag);
    choice.source = "fallback";
    choice.validationFailures = 1;  // exactly one, whatever the cause
    choice.detail = system.empty() ? "no system locale was provided"
                                   : "system locale \"" + system + "\" has no resource table";
    return choice;
  }

  const std::string canonical = CanonicalTag(trimmed);
  if (!canonical.empty()) {
    choice.tag = canonical;
    choice.source = "settings";
    choice.detail = "language.tag = " + choice.requested;
    return choice;
  }

  choice.tag = std::string(kFallbackTag);
  choice.source = "fallback";
  choice.validationFailures = 1;
  choice.detail = "\"" + choice.requested + "\" is not a language this build ships";
  return choice;
}

// Reads language.tag from card C14's store. The store already repairs values its
// own allow-list rejects; this second check exists for tags the schema allows
// but no table implements (ja-JP today), and for hand-edited documents a newer
// build wrote. It never writes back: selecting a language is a read.
inline LanguageChoice ChooseLanguageFromSettings(const prism::settings::SettingsStore& store,
                                                std::string_view systemLocale) {
  return ChooseLanguage(store.GetString("language.tag", "auto"), systemLocale);
}

// Convenience: the chain a resolved choice implies (tests and the app both use
// it, so "which tables will be consulted" is never re-derived by hand).
inline std::vector<std::string> ChainForChoice(const LanguageChoice& choice) {
  return ChainFor(choice.tag);
}

// Keys every shipping table must define: the right-click menu (one key per
// ContextMenuModel verb, so a new verb without a string fails the suite), the
// sidebar section labels, the telemetry capsule, the primary buttons, and the
// error/SKIP prompts the user actually reads. Declared here rather than in the
// test so the Win layer can assert the same list at startup.
inline const std::vector<std::string>& RequiredKeys() {
  static const std::vector<std::string> keys = {
      // The menu BAR title (card C38). It was missed by C19/C27: the app asked
      // for it via Tr::Or() with a code default, so the miss was never recorded
      // and the English menu bar shipped in Chinese builds.
      "menu.file",
      // Right-click menu (5 items for a single archive + the group variants).
      "menu.smart.extract.here",
      "menu.extract.here",
      "menu.extract.to.subfolder",
      "menu.test.integrity",
      "menu.open.in.prism",
      "menu.smart.extract.each.to.own.folder",
      "menu.compress.to.7z",
      "menu.compress.to.zip",
      // Sidebar headings and the empty state.
      "sidebar.section.favorites",
      "sidebar.section.recent",
      "sidebar.section.tree",
      "sidebar.empty.noArchive",
      // Status capsule.
      "status.encrypted",
      "status.notEncrypted",
      "status.unknownMethod",
      "status.ratio",
      "status.dictionary",
      "status.files.one",
      "status.files.other",
      "status.folders.one",
      "status.folders.other",
      // Primary actions.
      "action.extract",
      "action.compress",
      "action.cancel",
      "action.retry",
      "action.skip",
      "action.overwrite",
      // The menu bar's Exit item (C38: same MissCount hole as menu.file).
      "action.exit",
      // Errors. The five below are the CLI/verb + verification surfaces the exe
      // already asks for (C27 wiring, C38 keys): before C38 they only existed as
      // code defaults inside Tr::Or(), so a user who hit them saw Chinese in an
      // English build and a translator never learned the key existed.
      "error.engine.missing",
      "error.engine.load",
      "error.open",
      "error.password",
      "error.disk.full",
      "error.access.denied",
      "error.engine.noReason",
      "error.verify.failed",
      "error.missingPath",
      "error.verb.unsupported",
      "error.verb.supported",
      // SKIP / progress prompts.
      "skip.file",
      "skip.summary",
      "skip.reason.exists",
      "skip.reason.encrypted",
      "progress.extracted",
      "progress.failed",
      // Compression progress (C27's menu -> dialog -> writer path, C38 keys).
      "progress.compressed",
      "progress.compressFailed",
      "progress.compress.busy",
      // Status line: a finished re-pack that cannot refresh yet (C27 wiring).
      "status.refreshDeferred",
  };
  return keys;
}

}  // namespace prism::i18n
