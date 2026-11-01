// PrismArchive -- PrismWin
// ShellTranslator: the ONE place the shell extensions find their string tables
// (card C47). Header-only and dependency-free by design: both context-menu DLLs
// and the test that drives them include this file and link nothing extra.
//
// WHY IT EXISTS
// C40 taught the Win10 classic menu (PrismClassicContextMenu.cpp) to speak the
// user's language, but left the resolution inline in that TU, so the Win11
// IExplorerCommand handler (PrismShellCommand.cpp) kept answering English. A
// second copy of "find the tables" would have drifted the moment either side
// changed; this header is that rule, once, for both DLLs.
//
// THE FOUR RULES IT ENCODES (each one cost debugging time to learn)
//   1. The tables live next to the DLL THAT RENDERS THE MENU, never next to the
//      host executable: inside explorer.exe the "executable's directory" is
//      C:\Windows. The path is <dll dir>\Assets\Lang -- the same flat layout
//      "cmake --install" produces.
//   2. The language is the USER's, not the process's: language.tag from the
//      settings document the app writes, resolved through
//      prism::i18n::ChooseLanguage, so "auto" follows the system locale and an
//      unsupported tag lands on en-US instead of a half-translated menu.
//   3. It is resolved ONCE per module lifetime. A right-click must not parse
//      JSON; the function-local static below is C++11-guaranteed to run its
//      initialiser exactly once, racing threads included.
//   4. Every failure -- no Assets\Lang, an unreadable settings document, a table
//      the parser rejects -- leaves the translator NULL, and the model keeps its
//      English literals. A menu with English labels is usable; a menu showing
//      "menu.extract.here" is not.
//
// THREADING (B8 -- the shell makes no apartment or UI-thread promises)
// The shell may call IExplorerCommand from whichever thread it likes, so this
// header assumes NOTHING: no COM initialisation, no window, no message loop, no
// apartment. Pure Win32 file reads plus string work.
//   * the catalog tables and the chosen language are immutable after Load();
//   * the one mutable thing behind the pointer is PrismCore's MISS BOOKKEEPING
//     (Translator::Translate records a miss for an absent key through mutable
//     members), so BuildModel() takes a short mutex around the model build.
//     That is why callers never touch the raw Translator: handing it out would
//     publish an unsynchronised mutation, and "it is single-threaded in
//     Explorer today" is exactly the assumption this card forbids.
//
// CONTRACTS: D2 (tests compare the table's BYTES, so ResolveCount() reports a
// COUNT and no case measures time), D3 (the C47 suite's negative control removes
// the translation from PrismShellCommand.cpp and MUST go red), B2 (fixed-width
// integers arrive through <cstdint>, pulled by I18n.hpp), B8 (no apartment/UI
// assumption anywhere in this file), B1 (no unparenthesized std::min/max here).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "../PrismCore/ContextMenuModel.hpp"  // LanguageDirectoryForModuleDir + Build
#include "../PrismCore/I18n.hpp"              // Translator / ChooseLanguage
#include "SettingsFile.hpp"  // language.tag, DefaultPaths, UTF-8 <-> UTF-16 helpers

namespace prism::win::shelli18n {

// ---------------------------------------------------------------------------
// 1. Diagnostics: how often the resolver ran.
//
// One counter per MODULE (the accessor's function-local static lives in the
// binary that compiled this header), which is exactly the scope of the rule
// "once per DLL lifetime". It exists so the suite can assert the count instead
// of waiting on a stopwatch (CONTRACTS D2).
// ---------------------------------------------------------------------------
inline std::atomic<unsigned>& ResolveCount() {
  static std::atomic<unsigned> count{0};
  return count;
}

// ---------------------------------------------------------------------------
// 2. Which module is asking, and therefore where its tables are.
//
// FROM_ADDRESS + UNCHANGED_REFCOUNT: the handle is resolved from a code address
// INSIDE the calling module, so the answer is that module's directory even when
// the host is explorer.exe. No reference is taken, so nothing has to be freed.
// ---------------------------------------------------------------------------
inline std::wstring ModuleDirectoryOf(const void* addressInModule) {
  HMODULE module = nullptr;
  if (addressInModule == nullptr ||
      ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(addressInModule), &module) == FALSE) {
    return std::wstring();
  }
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(module, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return std::wstring();
  const std::wstring path(buffer, static_cast<std::size_t>(length));
  const std::size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

// The directory of the module this header was compiled into. An inline function
// is COMDAT-merged only WITHIN one binary, so each DLL (and the test exe) gets
// its own address and its own answer.
inline std::wstring ThisModuleDirectory() {
  return ModuleDirectoryOf(reinterpret_cast<const void*>(&ThisModuleDirectory));
}

// ---------------------------------------------------------------------------
// 3. The two language inputs.
// ---------------------------------------------------------------------------

// The system locale is an INPUT to PrismCore's choice rule (PrismCore must not
// include <windows.h>): the shell layer reads it and passes it in.
inline std::string SystemLocaleUtf8() {
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  const int length = ::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  return length > 1
             ? prism::win::settingsfile::WideToUtf8(
                   std::wstring(locale, static_cast<std::size_t>(length - 1)))
             : std::string();
}

// language.tag, read through the SAME store the app writes. A missing or
// unreadable document is not an error here: it means "auto", which is also what
// LoadStore reports as FreshDefaults/RecoveredFromCorrupt.
inline std::string RequestedLanguageTag() {
  prism::settings::SettingsStore store;
  const prism::win::settingsfile::LoadReport report =
      prism::win::settingsfile::LoadStore(store, prism::win::settingsfile::DefaultPaths());
  static_cast<void>(report);  // the menu only needs the value, not why it is that value
  return store.GetString("language.tag", "auto");
}

// ---------------------------------------------------------------------------
// 4. One resolution request. All three fields are explicit so the rule is
//    assertable without a shell, a DLL, or the user's real settings.
// ---------------------------------------------------------------------------
struct Source {
  std::string moduleDirectory;  // UTF-8 directory of the DLL that renders the menu
  std::string requestedTag;     // language.tag value ("auto" follows systemLocale)
  std::string systemLocale;     // UTF-8 locale name; "" means "not provided"
};

// ---------------------------------------------------------------------------
// 5. The resolved tables + the chosen language. Immutable after Load().
// ---------------------------------------------------------------------------
class ShellTranslator {
 public:
  ShellTranslator() = default;
  ShellTranslator(const ShellTranslator&) = delete;
  ShellTranslator& operator=(const ShellTranslator&) = delete;

  // Reads <moduleDirectory>\Assets\Lang\<tag>.json for every supported tag and
  // chooses the active language. Returns nullptr when no table loaded -- the
  // caller then renders the model's English defaults (rule 4 above).
  static std::shared_ptr<const ShellTranslator> Load(const Source& source) {
    const std::string directory =
        prism::ContextMenuModel::LanguageDirectoryForModuleDir(source.moduleDirectory);
    if (directory.empty()) return nullptr;

    std::shared_ptr<ShellTranslator> loaded = std::make_shared<ShellTranslator>();
    loaded->directory_ = directory;
    for (const std::string& tag : prism::i18n::SupportedTags()) {
      const std::string file = directory + "\\" + tag + ".json";
      if (loaded->translator_.LoadCatalogFile(file)) {
        ++loaded->catalogs_;
        loaded->keys_ += loaded->translator_.KeyCount(tag);
      }
    }
    if (loaded->catalogs_ == 0) return nullptr;

    const prism::i18n::LanguageChoice choice =
        prism::i18n::ChooseLanguage(source.requestedTag, source.systemLocale);
    loaded->translator_.SetLanguage(choice.tag);
    loaded->language_ = loaded->translator_.language();
    loaded->ready_ = true;
    return loaded;
  }

  bool ready() const { return ready_; }
  const std::string& language() const { return language_; }  // canonical, supported tag
  const std::string& languageDirectory() const { return directory_; }
  std::size_t catalogCount() const { return catalogs_; }
  std::size_t keyCount() const { return keys_; }

  // The ONLY way a caller gets menu text. Thread-safe by construction: the
  // tables are read-only and the lock serialises PrismCore's miss counter.
  std::vector<prism::MenuCommand> BuildModel(
      const std::vector<std::string>& selectedPaths) const {
    if (!ready_) return prism::ContextMenuModel::BuildDefault(selectedPaths);
    const std::lock_guard<std::mutex> guard(mutex_);
    return prism::ContextMenuModel::Build(selectedPaths, &translator_);
  }

  // The table's own bytes for a key (no English fallback): the suite uses this
  // to compare what the shell rendered against what the shipping table says.
  std::string TableText(std::string_view key) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return translator_.Translate(key);
  }

  std::size_t KeyCount(std::string_view tag) const {
    const std::lock_guard<std::mutex> guard(mutex_);
    return translator_.KeyCount(tag);
  }

 private:
  prism::i18n::Translator translator_;
  std::string directory_;
  std::string language_;
  std::size_t catalogs_ = 0;
  std::size_t keys_ = 0;
  bool ready_ = false;
  mutable std::mutex mutex_;  // guards Translator's miss bookkeeping only
};

// ---------------------------------------------------------------------------
// 6. This module's translator: resolved on first use, never re-read.
// ---------------------------------------------------------------------------
inline const ShellTranslator* Cached() {
  // Function-local static: C++11 guarantees one initialisation even when two
  // Explorer threads ask simultaneously, and no thread can observe a half-built
  // translator. A later language change is picked up when the DLL reloads -- the
  // shell caches the menu text per DLL lifetime by design (rule 3).
  static const std::shared_ptr<const ShellTranslator> cached = [] {
    ResolveCount().fetch_add(1, std::memory_order_relaxed);
    Source source;
    source.moduleDirectory = prism::win::settingsfile::WideToUtf8(ThisModuleDirectory());
    source.requestedTag = RequestedLanguageTag();
    source.systemLocale = SystemLocaleUtf8();
    return ShellTranslator::Load(source);
  }();
  return cached.get();
}

// The single call both shell DLLs make. Null-translator is not an error: the
// model falls back to its English literals.
inline std::vector<prism::MenuCommand> BuildMenuModel(
    const std::vector<std::string>& selectedPaths) {
  const ShellTranslator* translator = Cached();
  if (translator == nullptr) return prism::ContextMenuModel::BuildDefault(selectedPaths);
  return translator->BuildModel(selectedPaths);
}

}  // namespace prism::win::shelli18n
