// PrismArchive -- PrismWin
// Settings dialog (card C31, blueprint ch. 8.4 + ch. 9.1). The UI half of the
// settings story: C14 shipped SettingsStore (schema, defaults, migration,
// validation) and C27 landed it on disk through SettingsFile (temp file +
// ReplaceFileW). This file adds the two things that were missing -- a screen to
// edit the values and a gate that refuses to persist an illegal one.
//
// WHAT IS PURE AND WHAT IS NOT
//   DialogState is plain C++: no HWND, no windows.h, no disk. Every rule
//   (units, defaults, ranges, path shape, "an illegal value must not be
//   persisted") is proved on the Linux lane in milliseconds.
//   SettingsDialog is the programmatic Win32 shell, built from CreateWindowExW
//   like C18's CompressDialog -- no .rc, no dialog template, no CMake change.
//
// VALIDATION HAS ONE SOURCE (CONTRACTS D7)
//   The dialog does not re-implement the schema. It applies the draft to a COPY
//   of the settings store, serializes it, and loads it back through
//   SettingsStore::Load(). If the store would REPAIR anything the draft is
//   illegal -- the store's own repair list IS the error message. That is what
//   makes a language tag the assets ship but the schema does not know (say
//   "de-DE") fail HERE instead of being silently rewritten to "auto" on the next
//   start. The only rules duplicated are the ones no schema owns: the byte cap
//   on the extract path's SHAPE, the thumbnail cache's unit arithmetic, and the
//   read-only ("newer build wrote this file") refusal.
//
// KEYS THE SCHEMA DOES NOT NAME YET
//   The card asks for a thumbnail cache cap, a log level and the MoTW
//   inheritance switch; C14's schema has no entry for them. They are written as
//   ordinary (unknown) keys -- cache.thumbnailLimitMb, logging.level,
//   security.inheritMotw -- which SettingsStore PRESERVES verbatim by contract.
//   When a future schema adopts them nothing here changes: the store starts
//   validating them and a bad value surfaces through the same probe.
//   appearance.highContrast is the third theme (ThemeTokens::Variant) which the
//   schema's three-value appearance.theme cannot express; it rides beside the
//   schema value instead of overloading it.
//
// CONTRACTS:
//   B1  every std::min/max below is parenthesized (a consumer TU may include
//       windows.h without NOMINMAX).
//   B2  this header includes <cstdint> itself (it uses uint32_t/int64_t).
//   B4  the extract directory is normalized to '\' before it leaves the dialog,
//       and a '/' is reported as a shape problem rather than silently fixed in
//       the persisted value.
//   D1  nothing here skips: a settings file that cannot be read is a printed
//       failure, and SettingsDialog::Load() reports it.
//   D2  the save test asserts the FILE CONTENT, not that a file appeared.
//   D3  two field rules and the store probe each carry a compile-time mutant
//       branch that drops the complaint and bumps MutationCount(); the suites
//       are compiled from this same source.
//   D7  the schema is not duplicated; the store probe is the authority.
//   D26 no PASS_REGULAR_EXPRESSION is ever combined with WILL_FAIL: the mutant
//       marker (see the suite file) is a conjunction instead.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/SettingsStore.hpp"

namespace prism::win::settingsui {

// ---------------------------------------------------------------------------
// 1. Vocabulary
// ---------------------------------------------------------------------------

enum class Field : std::uint8_t {
  Language = 0,
  Theme,
  ExtractDir,
  ThumbnailLimit,
  LogLevel,
  Motw,
};

inline const char* FieldName(Field field) {
  switch (field) {
    case Field::Language: return "language";
    case Field::Theme: return "theme";
    case Field::ExtractDir: return "extract-dir";
    case Field::ThumbnailLimit: return "thumbnail-limit";
    case Field::LogLevel: return "log-level";
    case Field::Motw: return "motw";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Field field) { return os << FieldName(field); }

// The three themes the renderer can actually paint (ThemeTokens::Variant).
// "system" from the schema is not offered: the card asks for day/night/high
// contrast, and a fourth "follow the OS" row would be a mode the UI cannot
// preview. A stored "system" therefore loads as Daylight.
enum class Theme : std::uint8_t { Daylight = 0, Midnight, HighContrast };

inline const char* ThemeName(Theme theme) {
  switch (theme) {
    case Theme::Daylight: return "day";
    case Theme::Midnight: return "night";
    case Theme::HighContrast: return "high-contrast";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, Theme theme) { return os << ThemeName(theme); }

enum class SizeUnit : std::uint8_t { MB = 0, GB };

inline const char* SizeUnitName(SizeUnit unit) { return unit == SizeUnit::GB ? "GB" : "MB"; }

inline std::ostream& operator<<(std::ostream& os, SizeUnit unit) {
  return os << SizeUnitName(unit);
}

enum class LogLevel : std::uint8_t { Off = 0, Error, Warn, Info, Debug };

inline const char* LogLevelKeyOf(LogLevel level) {
  switch (level) {
    case LogLevel::Off: return "off";
    case LogLevel::Error: return "error";
    case LogLevel::Warn: return "warn";
    case LogLevel::Info: return "info";
    case LogLevel::Debug: return "debug";
  }
  return "info";
}

inline LogLevel LogLevelFromKey(std::string_view key) {
  if (key == "off") return LogLevel::Off;
  if (key == "error") return LogLevel::Error;
  if (key == "warn") return LogLevel::Warn;
  if (key == "debug") return LogLevel::Debug;
  return LogLevel::Info;  // "info" and anything unrecognised
}

inline const char* LogLevelLabel(LogLevel level) {
  switch (level) {
    case LogLevel::Off: return "Off";
    case LogLevel::Error: return "Error";
    case LogLevel::Warn: return "Warnings";
    case LogLevel::Info: return "Info";
    case LogLevel::Debug: return "Debug";
  }
  return "Info";
}

inline std::ostream& operator<<(std::ostream& os, LogLevel level) {
  return os << LogLevelKeyOf(level);
}

// ---------------------------------------------------------------------------
// 2. The keys this screen owns
// ---------------------------------------------------------------------------

inline constexpr char kLanguageKey[] = "language.tag";
inline constexpr char kThemeKey[] = "appearance.theme";
inline constexpr char kHighContrastKey[] = "appearance.highContrast";
inline constexpr char kExtractDirKey[] = "paths.lastExtractDir";
inline constexpr char kThumbnailKey[] = "cache.thumbnailLimitMb";
inline constexpr char kLogLevelKey[] = "logging.level";
inline constexpr char kMotwKey[] = "security.inheritMotw";

inline constexpr int kThumbnailMinMb = 32;
inline constexpr int kThumbnailMaxMb = 16 * 1024;  // 16 GiB
inline constexpr int kDefaultThumbnailMb = 512;
inline constexpr int kExtractDirMaxBytes = 260;   // the schema's byte cap
inline constexpr int kLanguageTagMaxBytes = 16;

struct Issue {
  Field field = Field::Language;
  std::string text;
};

// ---------------------------------------------------------------------------
// 3. DialogState -- every rule, no window
// ---------------------------------------------------------------------------

class DialogState {
 public:
  static constexpr int kThemeCount = 3;
  static constexpr int kLogLevelCount = 5;

  // A fresh state mirrors the store's documented defaults, so "open Settings on
  // a machine that never saved" shows 512 MB / info / inherit, not blanks.
  DialogState() { Adopt(prism::settings::SettingsStore{}); }

  // ---- language -----------------------------------------------------------
  // The list is what Assets/Lang actually ships (the Win32 layer reads the
  // directory); the SCHEMA's allow-list stays the store's business, and a tag
  // the assets ship but the schema rejects is caught by the store probe.
  void SetLanguages(std::vector<std::string> languages) {
    languages_ = std::move(languages);
    if (IndexIn(languages_, languageTag_) < 0) languages_.push_back(languageTag_);
    Validate();
  }
  const std::vector<std::string>& languages() const { return languages_; }
  const std::string& languageTag() const { return languageTag_; }
  int languageIndex() const { return IndexIn(languages_, languageTag_); }

  bool SetLanguageTag(std::string tag) {
    if (IndexIn(languages_, tag) < 0) return false;  // not a shipped language
    languageTag_ = std::move(tag);
    Validate();
    return true;
  }
  bool SetLanguageIndex(int index) {
    if (index < 0 || index >= static_cast<int>(languages_.size())) return false;
    languageTag_ = languages_[static_cast<std::size_t>(index)];
    Validate();
    return true;
  }

  // ---- theme --------------------------------------------------------------
  Theme theme() const { return theme_; }
  void SetTheme(Theme theme) {
    theme_ = theme;
    Validate();
  }
  static Theme ThemeAt(int index) {
    switch (index) {
      case 1: return Theme::Midnight;
      case 2: return Theme::HighContrast;
      default: return Theme::Daylight;
    }
  }
  static int ThemeIndex(Theme theme) { return static_cast<int>(theme); }

  // ---- default extraction directory --------------------------------------
  const std::string& extractDirText() const { return extractDirText_; }
  void SetExtractDirText(std::string text) {
    extractDirText_ = std::move(text);
    Validate();
  }
  // B4: '/' becomes '\' and trailing separators go away, but the root keeps its
  // separator ("C:\" is a folder; "C:" is a drive-relative path).
  static std::string NormalizeExtractDir(std::string_view text);
  std::string normalizedExtractDir() const { return NormalizeExtractDir(extractDirText_); }

  // ---- thumbnail cache cap -------------------------------------------------
  const std::string& thumbnailText() const { return thumbnailText_; }
  void SetThumbnailText(std::string text) {
    thumbnailText_ = std::move(text);
    Validate();
  }
  SizeUnit thumbnailUnit() const { return thumbnailUnit_; }
  void SetThumbnailUnit(SizeUnit unit) {
    thumbnailUnit_ = unit;
    Validate();
  }
  // -1 when the text is not a number at all (never a silent 0: "0 MB" would be
  // a cache the user did not ask for).
  int ThumbnailMb() const;
  // The value a valid draft would persist (defaults to the default when the
  // text is unparseable, so the control can show something honest).
  int ThumbnailMbOr(int fallback) const {
    const int mb = ThumbnailMb();
    return mb < 0 ? fallback : mb;
  }
  std::string ThumbnailLabel() const;  // "2 GB (2048 MB)" -- the unit echo

  // ---- log level -----------------------------------------------------------
  LogLevel logLevel() const { return logLevel_; }
  void SetLogLevel(LogLevel level) {
    logLevel_ = level;
    Validate();
  }
  static LogLevel LogLevelAt(int index) {
    switch (index) {
      case 0: return LogLevel::Off;
      case 1: return LogLevel::Error;
      case 2: return LogLevel::Warn;
      case 3: return LogLevel::Info;
      case 4: return LogLevel::Debug;
      default: return LogLevel::Info;
    }
  }
  static int LogLevelIndexOf(LogLevel level) { return static_cast<int>(level); }

  // ---- Mark-of-the-Web -----------------------------------------------------
  bool inheritMotw() const { return inheritMotw_; }
  void SetInheritMotw(bool inherit) {
    inheritMotw_ = inherit;
    Validate();
  }

  // ---- validation ----------------------------------------------------------
  // Recomputes issues(). Called by every setter and by ApplyTo(), so a caller
  // that never calls it explicitly cannot read a stale verdict.
  bool Validate() const;
  const std::vector<Issue>& issues() const { return issues_; }
  bool HasIssue(Field field) const;
  std::string IssueFor(Field field) const;
  bool Valid() const { return issues_.empty(); }
  // "Save" is enabled only when nothing is wrong. A dialog that let the user
  // press it and then refused would be a worse version of the same thing.
  bool SaveEnabled() const { return Valid(); }

  // ---- persistence ---------------------------------------------------------
  const prism::settings::SettingsStore& base() const { return base_; }

  // Reads every field out of a store (the dialog does this after LoadStore).
  // A stored language tag the shipped list does not know is APPENDED so the
  // value is visible instead of being replaced behind the user's back -- the
  // store probe then reports it if the schema disagrees.
  void Adopt(const prism::settings::SettingsStore& store);

  // Applies the draft to a COPY of "store"; an invalid draft changes nothing
  // and returns false (that is the "illegal value never reaches the disk"
  // guarantee, in one place).
  bool ApplyTo(prism::settings::SettingsStore& store) const;

  // ---- deliberate-defect bookkeeping (CONTRACTS D3) ------------------------
  // Each mutant branch below bumps this counter when the defect OBSERVABLY
  // fires (a bad value was let through). The suite's marker is the conjunction
  // "run went red AND this is non-zero", so a mutation that silently stopped
  // applying prints no marker and turns the mutant entry red (CONTRACTS D26).
  static int& MutationCount() {
    static int count = 0;
    return count;
  }
  static bool MutationFired() { return MutationCount() != 0; }

 private:
  static int IndexIn(const std::vector<std::string>& list, std::string_view value) {
    for (std::size_t i = 0; i < list.size(); ++i) {
      if (list[i] == value) return static_cast<int>(i);
    }
    return -1;
  }

  void AddIssue(Field field, std::string text) const {
    for (const Issue& existing : issues_) {
      if (existing.field == field) return;  // one message per field, first wins
    }
    issues_.push_back(Issue{field, std::move(text)});
  }

  static Field FieldForPath(std::string_view path) {
    if (path == kLanguageKey) return Field::Language;
    if (path == kThemeKey) return Field::Theme;
    if (path == kExtractDirKey) return Field::ExtractDir;
    if (path == kThumbnailKey) return Field::ThumbnailLimit;
    if (path == kLogLevelKey) return Field::LogLevel;
    if (path == kMotwKey) return Field::Motw;
    return Field::Theme;
  }

  // Applies every field without validating. Only Validate()'s probe and
  // ApplyTo() (after a clean Validate) call it.
  void ApplyFields(prism::settings::SettingsStore& store) const {
    store.SetString(kLanguageKey, languageTag_);
    store.SetString(kThemeKey, theme_ == Theme::Midnight ? "dark" : "light");
    store.SetBool(kHighContrastKey, theme_ == Theme::HighContrast);
    store.SetString(kExtractDirKey, normalizedExtractDir());
    store.SetInt(kThumbnailKey, ThumbnailMbOr(kDefaultThumbnailMb));
    store.SetString(kLogLevelKey, LogLevelKeyOf(logLevel_));
    store.SetBool(kMotwKey, inheritMotw_);
  }

  void CheckLanguageTag() const {
    if (languageTag_.empty()) {
      AddIssue(Field::Language, "Choose a language (Auto follows Windows).");
      return;
    }
    if (languageTag_.size() > static_cast<std::size_t>(kLanguageTagMaxBytes)) {
      AddIssue(Field::Language, "\"" + languageTag_ + "\" is too long to be a language tag.");
    }
  }

  void CheckExtractDir() const {
    const std::string problem = ExtractDirProblem(extractDirText_);
    if (problem.empty()) return;
#if defined(PRISM_C31_MUTANT_ACCEPT_BAD_DIR)
    // MUTANT (CONTRACTS D3): the complaint is dropped, so an illegal path is
    // saved. The suite asserts the file's content did not change -> it goes red.
    static_cast<void>(MutationCount()++);
    return;
#else
    AddIssue(Field::ExtractDir, problem);
#endif
  }

  void CheckThumbnail() const {
    int mb = 0;
    const std::string problem = ThumbnailProblem(thumbnailText_, thumbnailUnit_, &mb);
    if (problem.empty()) return;
#if defined(PRISM_C31_MUTANT_ACCEPT_BAD_THUMBNAIL)
    // MUTANT (CONTRACTS D3): an out-of-range / unparseable cap is accepted, so
    // the persisted document carries a value the UI promised not to write.
    static_cast<void>(MutationCount()++);
    return;
#else
    AddIssue(Field::ThumbnailLimit, problem);
#endif
  }

  // The store probe. THIS is the "validate through SettingsStore" requirement:
  // apply -> serialize -> Load() -> read the store's own repair list.
  void ProbeStore() const;

  static std::string ExtractDirProblem(std::string_view raw);
  static std::string ThumbnailProblem(std::string_view raw, SizeUnit unit, int* mb);

  std::vector<std::string> languages_{"auto", "zh-CN", "en-US"};
  std::string languageTag_{"auto"};
  Theme theme_ = Theme::Daylight;
  std::string extractDirText_;
  std::string thumbnailText_ = "512";
  SizeUnit thumbnailUnit_ = SizeUnit::MB;
  LogLevel logLevel_ = LogLevel::Info;
  bool inheritMotw_ = true;
  prism::settings::SettingsStore base_;
  mutable std::vector<Issue> issues_;
};

// ---------------------------------------------------------------------------
// 4. DialogState implementation (inline: header-only, like every Prism module)
// ---------------------------------------------------------------------------

inline std::string DialogState::NormalizeExtractDir(std::string_view text) {
  std::string forward;
  forward.reserve(text.size());
  for (const char ch : text) forward.push_back(ch == '/' ? '\\' : ch);
  // Collapse the doubled separators a paste from Explorer sometimes carries.
  std::string collapsed;
  collapsed.reserve(forward.size());
  for (const char ch : forward) {
    if (ch == '\\' && !collapsed.empty() && collapsed.back() == '\\') continue;
    collapsed.push_back(ch);
  }
  // Keep the root's separator: "C:\" is a folder, "C:" is not.
  while (collapsed.size() > 3 && collapsed.back() == '\\') collapsed.pop_back();
  return collapsed;
}

inline std::string DialogState::ExtractDirProblem(std::string_view raw) {
  if (raw.empty()) return std::string();  // empty = "ask every time", a real answer
  const std::string normalized = NormalizeExtractDir(raw);
  if (normalized.empty()) return std::string();
  if (normalized.size() > static_cast<std::size_t>(kExtractDirMaxBytes)) {
    return "The path is longer than the " + std::to_string(kExtractDirMaxBytes) +
           "-byte settings cap.";
  }
  const bool driveAbsolute = normalized.size() >= 3 &&
                             ((normalized[0] >= 'A' && normalized[0] <= 'Z') ||
                              (normalized[0] >= 'a' && normalized[0] <= 'z')) &&
                             normalized[1] == ':' && normalized[2] == '\\';
  if (!driveAbsolute) {
    return "Enter an absolute path such as C:\\Users\\you\\Extracted (or leave it empty to ask "
           "each time).";
  }
  for (std::size_t i = 2; i < normalized.size(); ++i) {
    const char ch = normalized[i];
    if (ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
      return std::string("The path contains '") + ch + "', which Windows does not allow.";
    }
  }
  // A ".." COMPONENT is refused; ".." inside a name ("a..b") is fine.
  std::size_t start = 3;
  while (start <= normalized.size()) {
    const std::size_t end = normalized.find('\\', start);
    const std::string_view component =
        std::string_view(normalized).substr(start, (end == std::string::npos ? normalized.size()
                                                                           : end) - start);
    if (component == "..") return std::string("The path must not contain a '..' component.");
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return std::string();
}

inline std::string DialogState::ThumbnailProblem(std::string_view raw, SizeUnit unit, int* mb) {
  if (mb != nullptr) *mb = -1;
  // Trim ASCII spaces only: a stray '\t' is a paste accident, not whitespace the
  // settings format documents.
  std::size_t begin = 0;
  std::size_t end = raw.size();
  while (begin < end && raw[begin] == ' ') ++begin;
  while (end > begin && raw[end - 1] == ' ') --end;
  const std::string_view text = raw.substr(begin, end - begin);
  if (text.empty()) return std::string("Enter the cache cap, for example 512 MB.");
  bool seenDigit = false;
  bool seenDot = false;
  for (const char ch : text) {
    if (ch >= '0' && ch <= '9') {
      seenDigit = true;
    } else if (ch == '.' && !seenDot) {
      seenDot = true;
    } else {
      return std::string("\"") + std::string(text) +
             "\" is not a number. Enter digits, optionally with a decimal point.";
    }
  }
  if (!seenDigit) return std::string("Enter a number of ") + SizeUnitName(unit) + ".";
  const double value = std::strtod(std::string(text).c_str(), nullptr);
  const double megabytes = value * (unit == SizeUnit::GB ? 1024.0 : 1.0);
  if (!std::isfinite(megabytes)) return std::string("That number is out of range.");
  const double rounded = std::floor(megabytes + 0.5);
  if (std::fabs(megabytes - rounded) > 1e-6) {
    return std::string("The cache cap must be a whole number of megabytes (") +
           std::to_string(static_cast<long long>(std::floor(megabytes))) +
           " MB is not one).";
  }
  if (rounded < static_cast<double>(kThumbnailMinMb)) {
    return "The cache cap must be at least " + std::to_string(kThumbnailMinMb) + " MB.";
  }
  if (rounded > static_cast<double>(kThumbnailMaxMb)) {
    return "The cache cap must be at most " + std::to_string(kThumbnailMaxMb) + " MB (16 GB).";
  }
  if (mb != nullptr) *mb = static_cast<int>(rounded);
  return std::string();
}

inline int DialogState::ThumbnailMb() const {
  int mb = -1;
  if (!ThumbnailProblem(thumbnailText_, thumbnailUnit_, &mb).empty()) return -1;
  return mb;
}

inline std::string DialogState::ThumbnailLabel() const {
  const int mb = ThumbnailMb();
  if (mb < 0) return std::string();
  if (mb >= 1024 && mb % 1024 == 0) {
    return std::to_string(mb / 1024) + " GB (" + std::to_string(mb) + " MB)";
  }
  return std::to_string(mb) + " MB";
}

inline void DialogState::ProbeStore() const {
  if (!base_.writable()) {
    AddIssue(Field::Theme,
             "This settings file was written by a newer build; it is read-only until that "
             "build changes it.");
    return;
  }
  prism::settings::SettingsStore candidate = base_;
  ApplyFields(candidate);
  prism::settings::SettingsStore roundTrip;
  const prism::settings::LoadReport report = roundTrip.Load(candidate.ToJson());
  const bool rejected = report.status == prism::settings::LoadStatus::Repaired ||
                        report.status == prism::settings::LoadStatus::Corrupt ||
                        !report.writable;
  if (!rejected) return;
#if defined(PRISM_C31_MUTANT_SKIP_STORE_PROBE)
  // MUTANT (CONTRACTS D3): the store's verdict is discarded, so a value the
  // schema forbids is persisted (and silently repaired on the next start).
  static_cast<void>(MutationCount()++);
  return;
#endif
  if (report.status == prism::settings::LoadStatus::Corrupt || roundTrip.repairs().empty()) {
    AddIssue(Field::Theme, "The settings store refused this document: " + report.error);
    return;
  }
  const prism::settings::Repair& repair = roundTrip.repairs().front();
  AddIssue(FieldForPath(repair.path),
           "The settings store would repair " + repair.path + ": " + repair.detail);
}

inline void DialogState::Adopt(const prism::settings::SettingsStore& store) {
  base_ = store;
  languageTag_ = store.GetString(kLanguageKey, "auto");
  if (languageTag_.empty()) languageTag_ = "auto";
  if (IndexIn(languages_, languageTag_) < 0) languages_.push_back(languageTag_);
  const std::string themeKey = store.GetString(kThemeKey, "light");
  const bool highContrast = store.GetBool(kHighContrastKey, false);
  if (highContrast || themeKey == "high-contrast") {
    theme_ = Theme::HighContrast;
  } else if (themeKey == "dark") {
    theme_ = Theme::Midnight;
  } else {
    theme_ = Theme::Daylight;  // "light", "system" and anything unknown
  }
  extractDirText_ = store.GetString(kExtractDirKey, "");
  const int storedMb = store.GetInt(kThumbnailKey, kDefaultThumbnailMb);
  thumbnailUnit_ = SizeUnit::MB;
  thumbnailText_ = std::to_string(storedMb > 0 ? storedMb : kDefaultThumbnailMb);
  logLevel_ = LogLevelFromKey(store.GetString(kLogLevelKey, "info"));
  inheritMotw_ = store.GetBool(kMotwKey, true);
  Validate();
}

inline bool DialogState::Validate() const {
  issues_.clear();
  CheckLanguageTag();
  CheckExtractDir();
  CheckThumbnail();
  // The probe only runs when the cheap rules are already happy: a draft with no
  // issues at all must also survive a round trip through the real store.
  if (issues_.empty()) ProbeStore();
  return issues_.empty();
}

inline bool DialogState::HasIssue(Field field) const {
  for (const Issue& issue : issues_) {
    if (issue.field == field) return true;
  }
  return false;
}

inline std::string DialogState::IssueFor(Field field) const {
  for (const Issue& issue : issues_) {
    if (issue.field == field) return issue.text;
  }
  return std::string();
}

inline bool DialogState::ApplyTo(prism::settings::SettingsStore& store) const {
  if (!Validate()) return false;  // the draft is illegal: nothing is touched
  prism::settings::SettingsStore candidate = store;
  ApplyFields(candidate);
  store = std::move(candidate);
  return true;
}

}  // namespace prism::win::settingsui

// ===========================================================================
// 5. SettingsDialog -- the programmatic Win32 shell (CONTRACTS D1: no skip)
// ===========================================================================
#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "DialogChrome.hpp"
#include "SettingsFile.hpp"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace prism::win::settingsui {

inline std::wstring WideFromUtf8(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (needed <= 0) return std::wstring();
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

inline std::string Utf8FromWide(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
  if (needed <= 0) return std::string();
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed,
                        nullptr, nullptr);
  return utf8;
}

namespace ids {
inline constexpr int kLanguage = 4001;
inline constexpr int kTheme = 4002;
inline constexpr int kExtractDir = 4003;
inline constexpr int kBrowse = 4004;
inline constexpr int kThumbnail = 4005;
inline constexpr int kUnit = 4006;
inline constexpr int kLogLevel = 4007;
inline constexpr int kMotw = 4008;
inline constexpr int kSummary = 4009;
inline constexpr int kSave = 4010;
inline constexpr int kCancel = 4011;
// One hint line per field: the in-place message ("非法值就地提示") lives here,
// directly under the control that owns the rule.
inline constexpr int kLanguageHint = 4021;
inline constexpr int kThemeHint = 4022;
inline constexpr int kExtractDirHint = 4023;
inline constexpr int kThumbnailHint = 4024;
inline constexpr int kLogLevelHint = 4025;
inline constexpr int kMotwHint = 4026;
}  // namespace ids

class SettingsDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveSettingsDialog";

  // Client geometry: one label column, one control column, a hint line under
  // every row. Fixed, like C18's dialog, because a resize handler would be more
  // code than the screen deserves.
  static constexpr int kClientWidth = 640;
  static constexpr int kClientHeight = 552;
  static constexpr UINT kMenuSettingsId = 0x4B31;  // the menu item patch A installs

  // The dialog hands the finished document (not a path) to the application, so
  // the app can decide where it lives (portable build, tests, a future profile).
  using SaveHandler =
      std::function<bool(const prism::settings::SettingsStore&, std::string* error)>;
  using BrowseHandler = std::function<std::wstring()>;

  SettingsDialog() = default;
  ~SettingsDialog() { Destroy(); }
  SettingsDialog(const SettingsDialog&) = delete;
  SettingsDialog& operator=(const SettingsDialog&) = delete;

  DWORD lastError() const { return lastError_; }
  int missingControlId() const { return missingControlId_; }

  bool Create(HINSTANCE instance, const std::wstring& title = L"Settings") {
    instance_ = instance;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &SettingsDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (wide on purpose)
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
      lastError_ = GetLastError();
      if (lastError_ != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    // V3: borderless card; DialogChrome paints the title bar (drag + close).
    dpi_ = chrome::CurrentDpi(nullptr);
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const int windowWidth = static_cast<int>(std::lround(kClientWidth * scale));
    const int windowHeight = static_cast<int>(std::lround(kClientHeight * scale));
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title.c_str(),
                            WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                            windowWidth, windowHeight, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) lastError_ = GetLastError();
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }

  DialogState& State() { return state_; }
  const DialogState& State() const { return state_; }

  const settingsfile::Paths& paths() const { return paths_; }
  void SetPaths(settingsfile::Paths paths) { paths_ = std::move(paths); }

  // Loads the document from disk. A failure is REPORTED (CONTRACTS D1): the
  // caller decides whether that is fatal -- the dialog itself still opens at
  // defaults so the user can repair a broken install, and the status line says
  // the file could not be read.
  bool LoadFrom(const settingsfile::Paths& paths, std::string* error) {
    SetPaths(paths);
    const settingsfile::LoadReport report = settingsfile::LoadStore(store_, paths_);
    if (report.outcome == settingsfile::LoadOutcome::Failed) {
      loadError_ = report.detail + " (error " + std::to_string(report.error) + ")";
      if (error != nullptr) *error = loadError_;
      state_.Adopt(store_);
      SyncControls();
      return false;
    }
    loadError_.clear();
    state_.Adopt(store_);
    SyncControls();
    return true;
  }

  // Validates, then persists ATOMICALLY through SettingsFile (temp file +
  // ReplaceFileW). An illegal draft never reaches SaveStore: Save() returns
  // false with the store's own complaint and the file on disk is untouched.
  bool Save(std::string* error) {
    if (!state_.Validate()) {
      const std::string why = state_.issues().empty() ? "the draft is not valid"
                                                      : state_.issues().front().text;
      if (error != nullptr) *error = why;
      SyncControls();
      FocusFirstIssue();
      return false;
    }
    prism::settings::SettingsStore next = store_;
    if (!state_.ApplyTo(next)) {  // defensive: ApplyTo revalidates on its own
      if (error != nullptr) *error = "the draft is not valid";
      return false;
    }
    if (onSave_) {
      if (!onSave_(next, error)) return false;
      store_ = next;
      state_.Adopt(next);
      saved_ = true;
      SyncControls();
      return true;
    }
    settingsfile::SaveResult result;
    if (!settingsfile::SaveStore(next, paths_, &result)) {
      if (error != nullptr) {
        *error = std::string("the settings file could not be written (") +
                 settingsfile::SaveMechanismName(result.mechanism) + ", error " +
                 std::to_string(result.error) + ")";
      }
      return false;
    }
    store_ = next;
    state_.Adopt(next);
    saved_ = true;
    SyncControls();
    return true;
  }

  bool saved() const { return saved_; }
  const std::string& loadError() const { return loadError_; }

  void SetSaveHandler(SaveHandler handler) { onSave_ = std::move(handler); }
  void SetBrowseHandler(BrowseHandler handler) { onBrowse_ = std::move(handler); }

  // state -> controls. Also the validation pump: every handler ends here, and
  // this is where the hint lines and the Save enablement are recomputed.
  void SyncControls();

  // Re-reads the language list into the combo. The shipped list is normally
  // known before Create(), but a build that drops a new Assets/Lang file must be
  // able to refresh without recreating the window -- and a test must be able to
  // drive that path without a fake control.
  void RefreshLanguageChoices() {
    if (languageBox_ == nullptr) return;
    SendMessageW(languageBox_, CB_RESETCONTENT, 0, 0);
    for (const std::string& tag : state_.languages()) {
      SendMessageW(languageBox_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(WideFromUtf8(tag).c_str()));
    }
    SyncControls();
  }

  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  std::wstring ControlText(int id) const {
    const HWND control = Control(id);
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
  }
  std::string ControlTextUtf8(int id) const { return Utf8FromWide(ControlText(id)); }

  bool ControlEnabled(int id) const {
    const HWND control = Control(id);
    return control != nullptr && IsWindowEnabled(control) != FALSE;
  }

  // The one-line summary above the buttons: either the first complaint or the
  // value the draft would persist (so the user can see "2 GB (2048 MB)" before
  // pressing Save).
  std::string SummaryLine() const;

  // Modal driver. The dialog owns its own message loop so the caller is one
  // line, and the owner window is disabled for the duration exactly once.
  bool OpenModal(HINSTANCE instance, HWND owner) {
    if (!Create(instance, L"PrismArchive settings")) return false;
    owner_ = owner;
    if (owner_ != nullptr) EnableWindow(owner_, FALSE);
    MSG msg{};
    while (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
      const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
      if (got <= 0) break;  // WM_QUIT: the app is going away, do not eat it
      if (IsDialogMessageW(hwnd_, &msg) == FALSE) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) EnableWindow(owner_, TRUE);
    return saved_;
  }

  // Patch A's other half: the menu bar with the one item this card adds. Main.cpp
  // calls this with the gallery HWND; GalleryWindow's WM_COMMAND forwards
  // kMenuSettingsId here.
  static void InstallMenu(HWND hwnd) {
    if (hwnd == nullptr) return;
    HMENU menu = CreateMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kMenuSettingsId, L"Settings...");
    SetMenu(hwnd, menu);
    DrawMenuBar(hwnd);
  }

  static std::wstring PickFolder(HWND owner) {
    BROWSEINFOW info{};
    info.hwndOwner = owner;
    info.lpszTitle = L"Choose the default extraction folder";
    info.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&info);
    if (id == nullptr) return std::wstring();
    wchar_t path[MAX_PATH] = {};
    const bool ok = SHGetPathFromIDListW(id, path) != FALSE;
    CoTaskMemFree(id);
    return ok ? std::wstring(path) : std::wstring();
  }


  // -------------------------------------------------------------------------
  // V3 chrome model. The stock controls stay as invisible adapters (C31's
  // assertions drive them by handle); the pixels are DialogChrome's.
  // -------------------------------------------------------------------------
  chrome::ChromeModel BuildModel() const {
    using chrome::WidgetKind;
    chrome::ChromeModel model;
    model.variant = prism::theme::Variant::Midnight;
    model.dpi = dpi_;
    model.width = static_cast<float>(kClientWidth);
    model.height = static_cast<float>(kClientHeight);
    model.title = L"Settings";

    auto label = [&model](const wchar_t* text, float x, float y, float w, float h) {
      chrome::Widget widget;
      widget.kind = WidgetKind::Label;
      widget.rect = D2D1::RectF(x, y, x + w, y + h);
      widget.label = text;
      model.widgets.push_back(std::move(widget));
    };
    auto hint = [&model](const std::wstring& text, float x, float y, float w, float h,
                         bool error) {
      chrome::Widget widget;
      widget.kind = WidgetKind::Hint;
      widget.rect = D2D1::RectF(x, y, x + w, y + h);
      widget.label = text;
      widget.error = error;
      model.widgets.push_back(std::move(widget));
    };
    auto visual = [this](int id, bool enabled, bool empty = false) {
      return chrome::ResolveState(enabled, hot_ == id, pressed_ == id, empty);
    };
    auto add = [&model](chrome::Widget widget) { model.widgets.push_back(std::move(widget)); };

    constexpr float kLabelX = 20.0f;
    constexpr float kControlX = 190.0f;
    constexpr float kRow = 62.0f;
    constexpr float kTop = 64.0f;
    const auto rowY = [](int row) { return kTop + kRow * row; };

    label(L"Language", kLabelX, rowY(0) + 8.0f, 160.0f, 22.0f);
    label(L"Theme", kLabelX, rowY(1) + 8.0f, 160.0f, 22.0f);
    label(L"Default extraction folder", kLabelX, rowY(2) + 8.0f, 160.0f, 22.0f);
    label(L"Thumbnail cache cap", kLabelX, rowY(3) + 8.0f, 160.0f, 22.0f);
    label(L"Log level", kLabelX, rowY(4) + 8.0f, 160.0f, 22.0f);
    label(L"Mark-of-the-Web", kLabelX, rowY(5) + 8.0f, 160.0f, 22.0f);

    chrome::Widget language;
    language.id = ids::kLanguage;
    language.kind = WidgetKind::Combo;
    language.rect = D2D1::RectF(kControlX, rowY(0), kControlX + 240.0f, rowY(0) + 30.0f);
    language.value = WideFromUtf8(state_.languageTag());
    language.enabled = true;
    language.state = visual(ids::kLanguage, true);
    add(language);

    chrome::Widget theme;
    theme.id = ids::kTheme;
    theme.kind = WidgetKind::Combo;
    theme.rect = D2D1::RectF(kControlX, rowY(1), kControlX + 240.0f, rowY(1) + 30.0f);
    theme.value = WideFromUtf8(ThemeName(state_.theme()));
    theme.enabled = true;
    theme.state = visual(ids::kTheme, true);
    add(theme);

    chrome::Widget extract;
    extract.id = ids::kExtractDir;
    extract.kind = WidgetKind::TextField;
    extract.rect = D2D1::RectF(kControlX, rowY(2), kControlX + 330.0f, rowY(2) + 30.0f);
    extract.label = L"C:\\Users\\you\\Extracted";
    extract.value = WideFromUtf8(state_.extractDirText());
    extract.enabled = true;
    extract.state = visual(ids::kExtractDir, true, state_.extractDirText().empty());
    add(extract);

    chrome::Widget browse;
    browse.id = ids::kBrowse;
    browse.kind = WidgetKind::Button;
    browse.rect = D2D1::RectF(kControlX + 340.0f, rowY(2), kControlX + 430.0f, rowY(2) + 30.0f);
    browse.label = L"Browse...";
    browse.enabled = true;
    browse.state = visual(ids::kBrowse, true);
    add(browse);

    chrome::Widget thumbnail;
    thumbnail.id = ids::kThumbnail;
    thumbnail.kind = WidgetKind::TextField;
    thumbnail.rect = D2D1::RectF(kControlX, rowY(3), kControlX + 110.0f, rowY(3) + 30.0f);
    thumbnail.label = L"512";
    thumbnail.value = WideFromUtf8(state_.thumbnailText());
    thumbnail.enabled = true;
    thumbnail.state = visual(ids::kThumbnail, true, state_.thumbnailText().empty());
    add(thumbnail);

    chrome::Widget unit;
    unit.id = ids::kUnit;
    unit.kind = WidgetKind::Combo;
    unit.rect = D2D1::RectF(kControlX + 122.0f, rowY(3), kControlX + 202.0f, rowY(3) + 30.0f);
    unit.value = WideFromUtf8(SizeUnitName(state_.thumbnailUnit()));
    unit.enabled = true;
    unit.state = visual(ids::kUnit, true);
    add(unit);

    chrome::Widget logLevel;
    logLevel.id = ids::kLogLevel;
    logLevel.kind = WidgetKind::Combo;
    logLevel.rect = D2D1::RectF(kControlX, rowY(4), kControlX + 240.0f, rowY(4) + 30.0f);
    logLevel.value = WideFromUtf8(LogLevelLabel(state_.logLevel()));
    logLevel.enabled = true;
    logLevel.state = visual(ids::kLogLevel, true);
    add(logLevel);

    chrome::Widget motw;
    motw.id = ids::kMotw;
    motw.kind = WidgetKind::CheckBox;
    motw.rect = D2D1::RectF(kControlX, rowY(5), kControlX + 420.0f, rowY(5) + 28.0f);
    motw.label = L"Inherit Mark-of-the-Web from the archive";
    motw.checked = state_.inheritMotw();
    motw.enabled = true;
    motw.state = visual(ids::kMotw, true);
    add(motw);

    const Field fields[] = {Field::Language, Field::Theme, Field::ExtractDir,
                            Field::ThumbnailLimit, Field::LogLevel, Field::Motw};
    for (int row = 0; row < 6; ++row) {
      const std::string issue = state_.IssueFor(fields[row]);
      if (!issue.empty()) {
        hint(WideFromUtf8(issue), kControlX, rowY(row) + 31.0f,
             static_cast<float>(kClientWidth) - kControlX - 20.0f, 18.0f, true);
      }
    }

    const std::string summary = SummaryLine();
    hint(WideFromUtf8(summary), kLabelX, rowY(6), static_cast<float>(kClientWidth) - 40.0f,
         36.0f, !state_.Valid() || !loadError_.empty());

    chrome::Widget save;
    save.id = ids::kSave;
    save.kind = WidgetKind::Button;
    save.rect = D2D1::RectF(static_cast<float>(kClientWidth) - 200.0f,
                           static_cast<float>(kClientHeight) - 52.0f,
                           static_cast<float>(kClientWidth) - 116.0f,
                           static_cast<float>(kClientHeight) - 22.0f);
    save.label = L"Save";
    save.primary = true;
    save.enabled = state_.SaveEnabled();
    save.state = visual(ids::kSave, save.enabled);
    add(save);

    chrome::Widget cancel;
    cancel.id = ids::kCancel;
    cancel.kind = WidgetKind::Button;
    cancel.rect = D2D1::RectF(static_cast<float>(kClientWidth) - 104.0f,
                             static_cast<float>(kClientHeight) - 52.0f,
                             static_cast<float>(kClientWidth) - 20.0f,
                             static_cast<float>(kClientHeight) - 22.0f);
    cancel.label = L"Cancel";
    cancel.enabled = true;
    cancel.state = visual(ids::kCancel, true);
    add(cancel);

    model.FocusIndex(focus_);
    return model;
  }

  int FocusIndexOfModel() const { return BuildModel().FocusIndex(); }
  int FocusedControlId() const { return BuildModel().FocusedId(); }
  int HotControlId() const { return hot_; }
  bool DispatchKey(UINT vk, bool shift) { return OnKeyDown(vk, shift); }
  void RefreshChrome() {
    if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SettingsDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<SettingsDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // WM_CREATE runs INSIDE CreateWindowExW, before Create() can assign the
      // handle: without this line every child control is created parentless.
      self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  bool CreateChildren();
  HWND MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, int x, int y,
                   int width, int height);
  void OnCommand(int id, int code, HWND control);
  void OnSaveClicked();
  void OnBrowse();
  void FocusFirstIssue();
  static void SetTextIfDifferent(HWND control, const std::wstring& text);

  // ---- V3 input / paint ------------------------------------------------
  int FocusableIndex(int id) const {
    const chrome::ChromeModel model = BuildModel();
    int index = 0;
    for (const chrome::Widget& widget : model.widgets) {
      if (!widget.Focusable()) continue;
      if (widget.id == id) return index;
      ++index;
    }
    return -1;
  }

  void SetFocusToId(int id) {
    const int index = FocusableIndex(id);
    if (index >= 0) {
      focus_ = index;
      RefreshChrome();
    }
  }

  void ActivateWidget(int id) {
    switch (id) {
      case ids::kLanguage:
      case ids::kTheme:
      case ids::kUnit:
      case ids::kLogLevel: {
        HWND box = nullptr;
        switch (id) {
          case ids::kLanguage: box = languageBox_; break;
          case ids::kTheme: box = themeBox_; break;
          case ids::kUnit: box = unitBox_; break;
          default: box = logBox_; break;
        }
        const int count = static_cast<int>(SendMessageW(box, CB_GETCOUNT, 0, 0));
        const int current = static_cast<int>(SendMessageW(box, CB_GETCURSEL, 0, 0));
        if (count <= 0) return;
        const int next = (current + 1) % count;
        SendMessageW(box, CB_SETCURSEL, static_cast<WPARAM>(next), 0);
        OnCommand(id, CBN_SELCHANGE, box);
        break;
      }
      case ids::kMotw:
        SendMessageW(motwCheck_, BM_CLICK, 0, 0);
        break;
      case ids::kBrowse:
        SendMessageW(browseButton_, BM_CLICK, 0, 0);
        break;
      case ids::kSave:
        SendMessageW(saveButton_, BM_CLICK, 0, 0);
        break;
      case ids::kCancel:
        SendMessageW(cancelButton_, BM_CLICK, 0, 0);
        break;
      case ids::kExtractDir:
      case ids::kThumbnail:
        SetFocusToId(id);
        break;
      default:
        break;
    }
  }

  bool OnKeyDown(UINT vk, bool shift) {
    const chrome::ChromeModel model = BuildModel();
    const chrome::Widget* focused = model.Focused();
    const bool combo = focused != nullptr && focused->kind == chrome::WidgetKind::Combo;
    const chrome::KeyCommand command = chrome::ClassifyKey(vk, shift, false, combo);
    switch (command) {
      case chrome::KeyCommand::Next:
        focus_ = chrome::NextFocus(focus_, model.FocusCount(), false);
        RefreshChrome();
        return true;
      case chrome::KeyCommand::Previous:
        focus_ = chrome::NextFocus(focus_, model.FocusCount(), true);
        RefreshChrome();
        return true;
      case chrome::KeyCommand::Cancel:
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return true;
      case chrome::KeyCommand::Activate:
        if (focused == nullptr) return false;
        if (focused->kind == chrome::WidgetKind::TextField) {
          if (state_.SaveEnabled()) SendMessageW(saveButton_, BM_CLICK, 0, 0);
          return true;
        }
        ActivateWidget(focused->id);
        return true;
      case chrome::KeyCommand::Increase:
      case chrome::KeyCommand::Decrease: {
        if (focused == nullptr || !combo) return false;
        HWND box = nullptr;
        switch (focused->id) {
          case ids::kLanguage: box = languageBox_; break;
          case ids::kTheme: box = themeBox_; break;
          case ids::kUnit: box = unitBox_; break;
          case ids::kLogLevel: box = logBox_; break;
          default: return false;
        }
        const int count = static_cast<int>(SendMessageW(box, CB_GETCOUNT, 0, 0));
        const int current = static_cast<int>(SendMessageW(box, CB_GETCURSEL, 0, 0));
        if (count <= 0) return true;
        int next = current + (command == chrome::KeyCommand::Increase ? 1 : -1);
        if (next < 0) next = count - 1;
        if (next >= count) next = 0;
        SendMessageW(box, CB_SETCURSEL, static_cast<WPARAM>(next), 0);
        OnCommand(focused->id, CBN_SELCHANGE, box);
        return true;
      }
      case chrome::KeyCommand::None:
        break;
    }
    return false;
  }

  bool OnCharTyped(wchar_t ch) {
    const chrome::ChromeModel model = BuildModel();
    const chrome::Widget* focused = model.Focused();
    if (focused == nullptr || focused->kind != chrome::WidgetKind::TextField) return false;
    HWND target = nullptr;
    switch (focused->id) {
      case ids::kExtractDir: target = extractEdit_; break;
      case ids::kThumbnail: target = thumbEdit_; break;
      default: return false;
    }
    std::wstring text = ControlText(focused->id);
    if (ch == L'\b') {
      if (text.empty()) return true;
      text.pop_back();
    } else if (ch >= 0x20 && ch != 0x7F) {
      text.push_back(ch);
    } else {
      return true;
    }
    SetWindowTextW(target, text.c_str());
    OnCommand(focused->id, EN_CHANGE, target);
    return true;
  }

  void PaintChrome() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    bool painted = false;
    if (SUCCEEDED(chromePainter_.Initialize())) {
      if (!chromeTarget_) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(client.right - client.left),
            static_cast<UINT32>(client.bottom - client.top));
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
            D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_NONE);
        if (SUCCEEDED(chromePainter_.Factory()->CreateHwndRenderTarget(
                properties, hwndProperties, chromeTarget_.ReleaseAndGetAddressOf()))) {
          chromeTarget_->SetDpi(96.0f, 96.0f);
        }
      }
      if (chromeTarget_) {
        chromeTarget_->BeginDraw();
        chromePainter_.Draw(chromeTarget_.Get(), BuildModel());
        if (SUCCEEDED(chromeTarget_->EndDraw())) painted = true;
      }
    }
    if (!painted) {
      RECT client{};
      GetClientRect(hwnd_, &client);
      const HBRUSH brush = CreateSolidBrush(RGB(11, 12, 16));
      FillRect(dc, &client, brush);
      DeleteObject(brush);
    }
    EndPaint(hwnd_, &paint);
  }

  void TrackMouseLeave() {
    TRACKMOUSEEVENT track{};
    track.cbSize = sizeof(track);
    track.dwFlags = TME_LEAVE;
    track.hwndTrack = hwnd_;
    TrackMouseEvent(&track);
  }

  LRESULT OnChromeMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_PAINT:
        PaintChrome();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
          SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        chromeTarget_.Reset();
        RefreshChrome();
        return 0;
      }
      case WM_SIZE:
        if (chromeTarget_) {
          chromeTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(LOWORD(lparam)),
                                            static_cast<UINT32>(HIWORD(lparam))));
        }
        RefreshChrome();
        return DefWindowProcW(hwnd_, message, wparam, lparam);
      case WM_MOUSEMOVE: {
        TrackMouseLeave();
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const chrome::ChromeModel model = BuildModel();
        const bool closeHover = chrome::HitCloseScreen(model, static_cast<float>(x),
                                                       static_cast<float>(y));
        const int hot = chrome::HitTestScreen(model, static_cast<float>(x), static_cast<float>(y));
        if (hot != hot_ || closeHover != closeHover_) {
          hot_ = hot;
          closeHover_ = closeHover;
          RefreshChrome();
        }
        return 0;
      }
      case WM_MOUSELEAVE:
        hot_ = -1;
        closeHover_ = false;
        pressed_ = -1;
        RefreshChrome();
        return 0;
      case WM_LBUTTONDOWN: {
        SetCapture(hwnd_);
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const chrome::ChromeModel model = BuildModel();
        if (chrome::HitCloseScreen(model, static_cast<float>(x), static_cast<float>(y))) {
          closePressed_ = true;
          pressed_ = -2;
          RefreshChrome();
          return 0;
        }
        if (chrome::HitTitleScreen(model, static_cast<float>(x), static_cast<float>(y))) {
          ReleaseCapture();
          SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
          return 0;
        }
        const int hit = chrome::HitTestScreen(model, static_cast<float>(x), static_cast<float>(y));
        if (hit >= 0) {
          pressed_ = hit;
          SetFocusToId(hit);
          RefreshChrome();
        }
        return 0;
      }
      case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const chrome::ChromeModel model = BuildModel();
        const bool wasClose = closePressed_;
        closePressed_ = false;
        if (GetCapture() == hwnd_) ReleaseCapture();
        if (wasClose && chrome::HitCloseScreen(model, static_cast<float>(x),
                                               static_cast<float>(y))) {
          pressed_ = -1;
          SendMessageW(hwnd_, WM_CLOSE, 0, 0);
          return 0;
        }
        const int hit = chrome::HitTestScreen(model, static_cast<float>(x), static_cast<float>(y));
        const int pressed = pressed_;
        pressed_ = -1;
        if (hit >= 0 && hit == pressed) ActivateWidget(hit);
        RefreshChrome();
        return 0;
      }
      case WM_KEYDOWN:
        if (!syncing_) {
          const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
          if (OnKeyDown(static_cast<UINT>(wparam), shift)) return 0;
        }
        break;
      case WM_CHAR:
        if (!syncing_ && OnCharTyped(static_cast<wchar_t>(wparam))) return 0;
        break;
      case WM_NCHITTEST: {
        if (hwnd_ != nullptr) {
          const LRESULT base = DefWindowProcW(hwnd_, message, wparam, lparam);
          POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
          ScreenToClient(hwnd_, &point);
          const chrome::ChromeModel model = BuildModel();
          if (chrome::HitTitleScreen(model, static_cast<float>(point.x),
                                     static_cast<float>(point.y))) {
            return HTCAPTION;
          }
          return base;
        }
        break;
      }
      default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
  }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND languageBox_ = nullptr;
  HWND themeBox_ = nullptr;
  HWND extractEdit_ = nullptr;
  HWND browseButton_ = nullptr;
  HWND thumbEdit_ = nullptr;
  HWND unitBox_ = nullptr;
  HWND logBox_ = nullptr;
  HWND motwCheck_ = nullptr;
  HWND summaryText_ = nullptr;
  HWND saveButton_ = nullptr;
  HWND cancelButton_ = nullptr;
  DWORD lastError_ = 0;
  int missingControlId_ = 0;
  bool syncing_ = false;
  bool saved_ = false;
  int dpi_ = 96;
  int focus_ = 0;
  int hot_ = -1;
  int pressed_ = -1;
  bool closeHover_ = false;
  bool closePressed_ = false;
  chrome::ChromePainter chromePainter_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> chromeTarget_;
  std::string loadError_;
  settingsfile::Paths paths_;
  prism::settings::SettingsStore store_;
  DialogState state_;
  SaveHandler onSave_;
  BrowseHandler onBrowse_;
};

}  // namespace prism::win::settingsui

#endif  // _WIN32

#if defined(_WIN32)

namespace prism::win::settingsui {

// Layout constants shared by CreateChildren() and the tests: one label column,
// one control column, one hint line per row.
namespace layout {
inline constexpr int kLabelX = 16;
inline constexpr int kControlX = 176;
inline constexpr int kControlWidth = 330;
inline constexpr int kRow = 62;
inline constexpr int kTop = 12;
inline int RowY(int row) { return kTop + row * kRow; }
inline int HintY(int row) { return RowY(row) + 30; }
}  // namespace layout

inline std::string SettingsDialog::SummaryLine() const {
  if (!loadError_.empty()) return "The settings file could not be loaded: " + loadError_;
  if (!state_.issues().empty()) {
    const Issue& first = state_.issues().front();
    return std::string(FieldName(first.field)) + ": " + first.text;
  }
  return std::string("Will save: ") + ThemeName(state_.theme()) + " theme, cache " +
         state_.ThumbnailLabel() + ", log " + LogLevelKeyOf(state_.logLevel()) +
         (state_.inheritMotw() ? ", MoTW inherited" : ", MoTW not inherited");
}

inline void SettingsDialog::SetTextIfDifferent(HWND control, const std::wstring& text) {
  if (control == nullptr) return;
  const int length = GetWindowTextLengthW(control);
  std::wstring current(static_cast<std::size_t>(length) + 1, L'\0');
  GetWindowTextW(control, current.data(), length + 1);
  current.resize(static_cast<std::size_t>(length));
  if (current == text) return;  // never reset the caret of a control being typed in
  SetWindowTextW(control, text.c_str());
}

inline void SettingsDialog::SyncControls() {
  if (hwnd_ == nullptr) return;
  // SetWindowTextW on an edit control notifies EN_CHANGE back to us; the guard
  // keeps a programmatic sync from being read as user input.
  syncing_ = true;
  SendMessageW(languageBox_, CB_SETCURSEL, static_cast<WPARAM>(state_.languageIndex()), 0);
  SendMessageW(themeBox_, CB_SETCURSEL,
               static_cast<WPARAM>(DialogState::ThemeIndex(state_.theme())), 0);
  SetTextIfDifferent(extractEdit_, WideFromUtf8(state_.extractDirText()));
  SetTextIfDifferent(thumbEdit_, WideFromUtf8(state_.thumbnailText()));
  SendMessageW(unitBox_, CB_SETCURSEL,
               static_cast<WPARAM>(state_.thumbnailUnit() == SizeUnit::GB ? 1 : 0), 0);
  SendMessageW(logBox_, CB_SETCURSEL,
               static_cast<WPARAM>(DialogState::LogLevelIndexOf(state_.logLevel())), 0);
  SendMessageW(motwCheck_, BM_SETCHECK,
               state_.inheritMotw() ? BST_CHECKED : BST_UNCHECKED, 0);

  const Field fields[] = {Field::Language,      Field::Theme,     Field::ExtractDir,
                          Field::ThumbnailLimit, Field::LogLevel, Field::Motw};
  const int hintIds[] = {ids::kLanguageHint, ids::kThemeHint,     ids::kExtractDirHint,
                         ids::kThumbnailHint, ids::kLogLevelHint, ids::kMotwHint};
  for (std::size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); ++i) {
    const HWND hint = Control(hintIds[i]);
    if (hint == nullptr) continue;
    SetWindowTextW(hint, WideFromUtf8(state_.IssueFor(fields[i])).c_str());
  }
  SetTextIfDifferent(summaryText_, WideFromUtf8(SummaryLine()));
  EnableWindow(saveButton_, state_.SaveEnabled() ? TRUE : FALSE);
  syncing_ = false;
}

inline HWND SettingsDialog::MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style,
                                        int id, int x, int y, int width, int height) {
  // V3: invisible adapters. C31 drives them by handle; the user sees the
  // DialogChrome pixels instead of the system combo/edit/button chrome.
  const HWND control = CreateWindowExW(
      0, klass, text, WS_CHILD | style, x, y, width, height, hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
  if (control == nullptr) {
    // "The dialog did not open" is not diagnosable; "control 4005 (edit) failed"
    // is. Same rule as C18.
    missingControlId_ = id;
    lastError_ = GetLastError();
    return nullptr;
  }
  SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
               TRUE);
  return control;
}

inline bool SettingsDialog::CreateChildren() {
  using namespace layout;
  const int kHintWidth = kClientWidth - kControlX - 24;

  languageBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                             ids::kLanguage, kControlX, RowY(0), 200, 200);
  themeBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                          ids::kTheme, kControlX, RowY(1), 200, 200);
  extractEdit_ = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                             ids::kExtractDir, kControlX, RowY(2), 250, 22);
  browseButton_ = MakeControl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, ids::kBrowse,
                              kControlX + 258, RowY(2) - 1, 84, 24);
  thumbEdit_ = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP | ES_NUMBER,
                           ids::kThumbnail, kControlX, RowY(3), 90, 22);
  unitBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ids::kUnit,
                         kControlX + 98, RowY(3), 80, 120);
  logBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                        ids::kLogLevel, kControlX, RowY(4), 200, 200);
  motwCheck_ = MakeControl(L"BUTTON", L"Inherit Mark-of-the-Web from the archive",
                           BS_AUTOCHECKBOX | WS_TABSTOP, ids::kMotw, kControlX, RowY(5), 430, 20);
  summaryText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kSummary, kLabelX, RowY(6),
                             kClientWidth - 2 * kLabelX, 44);
  saveButton_ = MakeControl(L"BUTTON", L"Save", BS_DEFPUSHBUTTON | WS_TABSTOP, ids::kSave,
                            kClientWidth - 200, kClientHeight - 44, 84, 28);
  cancelButton_ = MakeControl(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ids::kCancel,
                              kClientWidth - 104, kClientHeight - 44, 84, 28);

  const int hintIds[] = {ids::kLanguageHint, ids::kThemeHint,     ids::kExtractDirHint,
                         ids::kThumbnailHint, ids::kLogLevelHint, ids::kMotwHint};
  for (int row = 0; row < 6; ++row) {
    MakeControl(L"STATIC", L"", SS_LEFT, hintIds[row], kControlX, HintY(row), kHintWidth, 18);
  }

  const wchar_t* labels[] = {L"Language", L"Theme", L"Default extraction folder",
                             L"Thumbnail cache cap", L"Log level", L"Mark-of-the-Web"};
  for (int row = 0; row < 6; ++row) {
    MakeControl(L"STATIC", labels[row], SS_LEFT, 0, kLabelX, RowY(row) + 4, 150, 18);
  }

  if (languageBox_ == nullptr || themeBox_ == nullptr || extractEdit_ == nullptr ||
      browseButton_ == nullptr || thumbEdit_ == nullptr || unitBox_ == nullptr ||
      logBox_ == nullptr || motwCheck_ == nullptr || summaryText_ == nullptr ||
      saveButton_ == nullptr || cancelButton_ == nullptr) {
    return false;
  }

  for (const std::string& tag : state_.languages()) {
    SendMessageW(languageBox_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(WideFromUtf8(tag).c_str()));
  }
  const wchar_t* themes[] = {L"Day", L"Night", L"High contrast"};
  for (const wchar_t* theme : themes) {
    SendMessageW(themeBox_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(theme));
  }
  SendMessageW(unitBox_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"MB"));
  SendMessageW(unitBox_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GB"));
  const LogLevel levels[] = {LogLevel::Off, LogLevel::Error, LogLevel::Warn, LogLevel::Info,
                             LogLevel::Debug};
  for (const LogLevel level : levels) {
    SendMessageW(logBox_, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(WideFromUtf8(LogLevelLabel(level)).c_str()));
  }
  SyncControls();
  return true;
}

inline void SettingsDialog::OnBrowse() {
  std::wstring picked = onBrowse_ ? onBrowse_() : PickFolder(hwnd_);
  if (picked.empty()) return;  // the user cancelled: not an error, not a change
  state_.SetExtractDirText(Utf8FromWide(picked));
}

inline void SettingsDialog::FocusFirstIssue() {
  if (state_.HasIssue(Field::Language)) {
    SetFocus(languageBox_);
  } else if (state_.HasIssue(Field::Theme)) {
    SetFocus(themeBox_);
  } else if (state_.HasIssue(Field::ExtractDir)) {
    SetFocus(extractEdit_);
  } else if (state_.HasIssue(Field::ThumbnailLimit)) {
    SetFocus(thumbEdit_);
  } else if (state_.HasIssue(Field::LogLevel)) {
    SetFocus(logBox_);
  } else {
    SetFocus(motwCheck_);
  }
}

inline void SettingsDialog::OnSaveClicked() {
  std::string error;
  if (Save(&error)) {
    Destroy();  // the accepted save closes the dialog, nothing else
    return;
  }
  // The hint lines already say WHAT is wrong; the summary says that the disk was
  // deliberately not touched.
  SetTextIfDifferent(summaryText_, WideFromUtf8("Not saved: " + error));
}

inline void SettingsDialog::OnCommand(int id, int code, HWND control) {
  if (control == nullptr) return;
  switch (id) {
    case ids::kLanguage:
      if (code != CBN_SELCHANGE) return;
      state_.SetLanguageIndex(
          static_cast<int>(SendMessageW(languageBox_, CB_GETCURSEL, 0, 0)));
      break;
    case ids::kTheme:
      if (code != CBN_SELCHANGE) return;
      state_.SetTheme(
          DialogState::ThemeAt(static_cast<int>(SendMessageW(themeBox_, CB_GETCURSEL, 0, 0))));
      break;
    case ids::kExtractDir:
      if (code != EN_CHANGE) return;
      state_.SetExtractDirText(ControlTextUtf8(ids::kExtractDir));
      break;
    case ids::kThumbnail:
      if (code != EN_CHANGE) return;
      state_.SetThumbnailText(ControlTextUtf8(ids::kThumbnail));
      break;
    case ids::kUnit:
      if (code != CBN_SELCHANGE) return;
      state_.SetThumbnailUnit(SendMessageW(unitBox_, CB_GETCURSEL, 0, 0) == 1 ? SizeUnit::GB
                                                                             : SizeUnit::MB);
      break;
    case ids::kLogLevel:
      if (code != CBN_SELCHANGE) return;
      state_.SetLogLevel(DialogState::LogLevelAt(
          static_cast<int>(SendMessageW(logBox_, CB_GETCURSEL, 0, 0))));
      break;
    case ids::kMotw:
      if (code != BN_CLICKED) return;
      state_.SetInheritMotw(SendMessageW(motwCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
      break;
    case ids::kBrowse:
      if (code != BN_CLICKED) return;
      OnBrowse();
      break;
    case ids::kSave:
      if (code != BN_CLICKED) return;
      OnSaveClicked();
      return;  // Save()/SyncControls() already refreshed the surface
    case ids::kCancel:
      if (code != BN_CLICKED) return;
      SendMessageW(hwnd_, WM_CLOSE, 0, 0);
      return;
    default:
      return;
  }
  SyncControls();
}

inline LRESULT SettingsDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                             LPARAM lparam) {
  switch (message) {
    // V3 chrome owns these: the stock controls are adapters, never painted.
    case WM_PAINT:
    case WM_ERASEBKGND:
    case WM_DPICHANGED:
    case WM_SIZE:
    case WM_MOUSEMOVE:
    case WM_MOUSELEAVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_KEYDOWN:
    case WM_CHAR:
    case WM_NCHITTEST:
      return OnChromeMessage(message, wparam, lparam);
    case WM_CREATE:
      if (!CreateChildren()) return -1;
      focus_ = 0;
      return 0;
    case WM_COMMAND:
      if (!syncing_) OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
      return 0;
    case WM_CLOSE:
      // No PostQuitMessage: the dialog lives inside the app's loop and quitting
      // the process is the app's decision, not a dialog's.
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      chromeTarget_.Reset();
      chromePainter_.ResetDeviceResources();
      hwnd_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(hwnd, message, wparam, lparam);
  }
}

}  // namespace prism::win::settingsui

#endif  // _WIN32
