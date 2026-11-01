// PrismArchive -- C40: the i18n wiring and the password gate, end to end.
//
// WHAT THIS SUITE IS FOR (and what it is not):
//   * it is NOT a second copy of C19's catalog tests or C35's model tests. It
//     drives the WIRING this card added: the sentences Main.cpp now builds, the
//     menu/sidebar labels the app now asks the table for, and the password flow
//     from "the engine said E_ABORT" to "the DPAPI vault has the secret".
//   * every i18n assertion compares BYTES read back from the shipped tables, not
//     a boolean: "the label is Chinese" means "the label equals the zh-CN value
//     of its key", and "the fallback is real text" means "no backslash anywhere
//     in the string".
//
// CONTRACTS: D1 (the marker at the end is part of the verdict; nothing here can
// take the no-op path -- the tables are a compile-time input), D2 (content
// comparisons), D3 + D26 (two mutations, and the CAUGHT marker is a CONJUNCTION
// this binary prints itself: red suite + active injection, never WILL_FAIL),
// B9 (the probe runs off the UI thread and only POSTS its result).
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/ContextMenuModel.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismWin/DpapiVault.hpp"
#include "../../Src/PrismWin/PasswordDialog.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

#if !defined(PRISM_C40_LANG_DIR)
// The asset directory is a COMPILE-TIME input (Tests/suites/c40-i18nwire.cmake).
// Without it the suite would read nothing and pass vacuously -- the D1 trap.
#error "PRISM_C40_LANG_DIR must name the Assets/Lang directory (see Tests/suites/c40-i18nwire.cmake)"
#endif

namespace fs = std::filesystem;

namespace {

using prism::i18n::FormatPattern;
using prism::i18n::Translator;
using prism::pwd::PasswordUiConfig;
using prism::pwd::RememberChoice;
using prism::pwd::UiState;

// ---------------------------------------------------------------------------
// Mutation switches (CONTRACTS D3/D26). Each one is compiled into its own
// executable and changes the behaviour the matching case asserts. The CAUGHT
// marker below is printed ONLY when the suite went red with the mutation
// compiled in; a stale injection leaves the suite green and the marker absent,
// so the CTest entry goes red instead of silently passing.
// ---------------------------------------------------------------------------
#if defined(PRISM_C40_MUTANT_ESCAPED_FALLBACK)
constexpr const char* kMutant = "escaped_fallback";
#elif defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES)
constexpr const char* kMutant = "unlimited_budget";
#else
constexpr const char* kMutant = nullptr;
#endif

std::string LangPath(std::string_view file) {
  return std::string(PRISM_C40_LANG_DIR) + "/" + std::string(file);
}

bool LoadTables(Translator* translator, const char* tag) {
  std::string why;
  if (!translator->LoadCatalogFile(LangPath("en-US.json"), &why)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "cannot load en-US.json: " + why);
    return false;
  }
  if (!translator->LoadCatalogFile(LangPath("zh-CN.json"), &why)) {
    prism::test::ReportFailure(__FILE__, __LINE__, "cannot load zh-CN.json: " + why);
    return false;
  }
  translator->SetLanguage(tag);
  return true;
}

// The table's own bytes for a key; empty when the key is absent (so a missing
// string can never be mistaken for a translated one).
std::string TableText(Translator& translator, const std::string& key) {
  return translator.Has(key) ? translator.Translate(key) : std::string();
}

bool HasNonAscii(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80u) return true;
  }
  return false;
}

bool HasBackslash(const std::wstring& text) { return text.find(L'\\') != std::wstring::npos; }

std::wstring Wide(const std::string& utf8) { return prism::win::Utf8ToWide(utf8); }

// Wide strings have no operator<< on the harness's narrow stream, so every wide
// comparison goes through UTF-8: the failure message then prints the actual bytes.
std::string U8(const std::wstring& text) { return prism::win::WideToUtf8(text); }

std::vector<char> ReadBytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<char>((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
}

// Unique per-process fixture directory: a second run of this binary must never
// inherit a vault (or a failure) from the previous one.
fs::path FreshDir() {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) /
                       ("c40_i18nwire_" + std::to_string(GetCurrentProcessId()));
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// Pumps the UI thread until the predicate holds or a bounded number of wake-ups
// passes. The bound is a COUNT, not a stopwatch: a hung probe fails the case, it
// does not flake.
bool PumpUntil(const std::function<bool()>& done) {
  MSG message{};
  for (int i = 0; i < 4000; ++i) {
    if (done()) return true;
    if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
      continue;
    }
    MsgWaitForMultipleObjectsEx(0, nullptr, 5, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
  }
  return done();
}

// ---------------------------------------------------------------------------
// The password gate under test: C35's dialog + C40's runner and sink, wired the
// same way Main.cpp wires them. The probe is a fake engine (a real 7z.dll Open
// would need an encrypted fixture and would test C5, not this card), but every
// other piece -- the dialog, the model, the worker, the post, the DPAPI vault --
// is the shipping one.
// ---------------------------------------------------------------------------
struct Gate {
  prism::app::PasswordProbeRunner runner;
  prism::app::PasswordResultWindow sink;
  prism::win::pwd_ui::PasswordDialog dialog;

  std::string correct = "correct-horse-battery";
  std::wstring vaultPath;
  std::string openedPassword;
  DWORD openedThread = 0;
  DWORD probeThread = 0;
  bool rememberWrite = false;
  bool rememberTried = false;

  ~Gate() {
    runner.Join();
    sink.Destroy();
    dialog.Destroy();
  }

  bool Create(HINSTANCE instance, PasswordUiConfig config) {
    if (!dialog.Create(instance, L"PrismArchive C40")) return false;
    dialog.SetConfig(std::move(config));
    if (!sink.Create(instance, [this](const prism::app::PasswordProbeResult& result) {
          dialog.FinishProbe(result.found, result.cancelled, result.password,
                             result.diagnostics);
        })) {
      return false;
    }
    dialog.SetSubmitHandler([this](const std::string& password) {
      runner.Start(
          password,
          [this](const std::string& candidate, std::string* why) {
            probeThread = GetCurrentThreadId();
            if (why != nullptr) *why = "fake engine trace";
            return candidate == correct;
          },
          [this](const prism::app::PasswordProbeResult& result) { sink.Post(result); });
    });
    dialog.SetOpenedHandler([this](const std::string& password, bool remember) {
      openedPassword = password;
      openedThread = GetCurrentThreadId();
      if (remember) {
        rememberTried = true;
        rememberWrite = prism::app::RememberPassword(vaultPath, password);
      }
    });
    dialog.Start();
    return true;
  }

  bool Submit(const std::wstring& text) {
    HWND edit = dialog.Control(prism::win::pwd_ui::ids::kPassword);
    HWND ok = dialog.Control(prism::win::pwd_ui::ids::kOk);
    if (edit == nullptr || ok == nullptr) return false;
    SetWindowTextW(edit, text.c_str());
    // "true" means the attempt actually STARTED (the model went Probing), which is
    // the same question the dialog's own OnAccept asks. A refused submit -- empty
    // input, an exhausted budget -- answers false.
    const UiState before = dialog.Model().state();
    dialog.Dispatch(WM_COMMAND, MAKEWPARAM(prism::win::pwd_ui::ids::kOk, BN_CLICKED),
                    reinterpret_cast<LPARAM>(ok));
    return before != UiState::Probing && dialog.Model().state() == UiState::Probing;
  }

  bool Click(int id) {
    HWND control = dialog.Control(id);
    if (control == nullptr) return false;
    dialog.Dispatch(WM_COMMAND, MAKEWPARAM(id, BN_CLICKED),
                    reinterpret_cast<LPARAM>(control));
    return true;
  }
};

}  // namespace

// ---------------------------------------------------------------------------
// 1. The three double-escaped fallbacks (C38's audit bugs #1-#3).
// ---------------------------------------------------------------------------
PRISM_TEST("c40.fallback: the open/refresh sentences are real text, never escapes") {
  const prism::app::Tr none{nullptr};

  const std::wstring refresh = prism::app::RefreshDeferredStatus(none);
  PRISM_CHECK(!refresh.empty());
  PRISM_CHECK(!HasBackslash(refresh));
  PRISM_CHECK_EQ(U8(refresh),
                 U8(std::wstring(L"\u5DF2\u5199\u5165\uFF1B\u5F53\u524D\u64CD\u4F5C"
                                 L"\u7ED3\u675F\u540E\u5237\u65B0")));

  const std::wstring open = prism::app::OpenFailedStatus(none, L"a.7z");
  PRISM_CHECK(!HasBackslash(open));
  PRISM_CHECK(open.find(L"a.7z") != std::wstring::npos);
  PRISM_CHECK_EQ(U8(open), U8(std::wstring(L"\u6253\u5F00\u5931\u8D25 a.7z")));

  const std::wstring noReason = prism::app::EngineNoReasonText(none);
  PRISM_CHECK(!HasBackslash(noReason));
  PRISM_CHECK_EQ(U8(noReason),
                 U8(std::wstring(L"\uFF08\u5F15\u64CE\u672A\u62A5\u544A"
                                 L"\u539F\u56E0\uFF09")));

  const std::wstring items = prism::app::ItemsCountStatus(none, 3);
  PRISM_CHECK(!HasBackslash(items));
  PRISM_CHECK_EQ(U8(items), U8(std::wstring(L"\u5171 3 \u9879")));

  // A worker never composes a sentence from the engine's silence: the caller
  // hands it the resolved text, and an empty error becomes exactly that.
  PRISM_CHECK_EQ(U8(prism::app::CompressFailureDetail(noReason, std::string())), U8(noReason));
  PRISM_CHECK_EQ(U8(prism::app::CompressFailureDetail(noReason, "kDataError")),
                 std::string("kDataError"));
}

PRISM_TEST("c40.fallback: with a table the key wins and {name} is filled") {
  Translator translator;
  if (!PRISM_CHECK(LoadTables(&translator, "zh-CN"))) return;
  const prism::app::Tr zh{&translator};

  const std::wstring open = prism::app::OpenFailedStatus(zh, L"a.7z");
  PRISM_CHECK(open.find(L"a.7z") != std::wstring::npos);
  PRISM_CHECK(open.find(L'{') == std::wstring::npos);  // no placeholder survived
  PRISM_CHECK(!HasBackslash(open));
  PRISM_CHECK(HasNonAscii(prism::win::WideToUtf8(open)));

  translator.SetLanguage("en-US");
  const std::wstring english = prism::app::OpenFailedStatus(zh, L"a.7z");
  PRISM_CHECK_EQ(U8(english), std::string("Failed to open a.7z"));
}

// ---------------------------------------------------------------------------
// 2. The twelve labels the tables always had and no call site asked for.
// ---------------------------------------------------------------------------
PRISM_TEST("c40.menu: all eight verbs render the zh-CN table's own text") {
  Translator translator;
  if (!PRISM_CHECK(LoadTables(&translator, "zh-CN"))) return;

  const std::vector<std::vector<std::string>> selections = {
      {"report.7z"},
      {"a.7z", "b.7z"},
      {"report.7z", "notes.txt"},
  };
  std::size_t seen = 0;
  for (const std::vector<std::string>& selection : selections) {
    const std::vector<prism::MenuCommand> english =
        prism::ContextMenuModel::BuildDefault(selection);
    const std::vector<prism::MenuCommand> zh =
        prism::ContextMenuModel::Build(selection, &translator);
    if (!PRISM_CHECK_EQ(zh.size(), english.size())) continue;
    for (std::size_t i = 0; i < zh.size(); ++i) {
      ++seen;
      const std::string key = "menu." + zh[i].verb;
      const std::string table = TableText(translator, key);
      PRISM_CHECK(!table.empty());
      PRISM_CHECK(HasNonAscii(table));
      // The rendered label IS the table's bytes, with the verb's own payload
      // substituted -- read back, not inferred from a flag.
      PRISM_CHECK_EQ(zh[i].title, FormatPattern(table, {{"name", zh[i].titleArgument}}));
      // ... and it is no longer the English default (the bug this card fixes).
      PRISM_CHECK(zh[i].title != english[i].title);
      PRISM_CHECK(!HasNonAscii(english[i].title));
    }
  }
  PRISM_CHECK_EQ(seen, std::size_t{15});  // 5 + 3 + 7 across the three selections

  // The English default is still byte-identical to en-US (the shell host without
  // tables depends on it, and C19's suite asserts the same equality).
  Translator en;
  if (!PRISM_CHECK(LoadTables(&en, "en-US"))) return;
  const std::vector<prism::MenuCommand> defaults =
      prism::ContextMenuModel::BuildDefault({"report.7z", "notes.txt"});
  for (const prism::MenuCommand& item : defaults) {
    PRISM_CHECK_EQ(item.title, FormatPattern(TableText(en, "menu." + item.verb),
                                             {{"name", item.titleArgument}}));
  }

  // A table that lacks a verb keeps the English default instead of "menu.x.y".
  Translator sparse;
  sparse.SetLanguage("zh-CN");
  const std::vector<prism::MenuCommand> fallback =
      prism::ContextMenuModel::Build({"a.7z"}, &sparse);
  PRISM_CHECK_EQ(fallback.size(), std::size_t{5});
  PRISM_CHECK_EQ(fallback[0].title, std::string("Smart Extract Here"));
}

PRISM_TEST("c40.sidebar: the four headings are the table's text under zh-CN") {
  Translator translator;
  if (!PRISM_CHECK(LoadTables(&translator, "zh-CN"))) return;

  prism::sidebar::Model model;
  model.SetFavorites({{"", "C:/Users/me/Documents", "folder"}});
  model.PushRecent("C:/archives/a.7z", 100);

  prism::win::sidebar::Metrics metrics;
  prism::win::sidebar::SetTranslator(&translator);
  const prism::win::sidebar::Layout layout = prism::win::sidebar::Build(model, metrics);
  prism::win::sidebar::SetTranslator(nullptr);  // never leak the slot into later cases

  const auto sectionLabel = [&layout](prism::sidebar::Section section) {
    for (const prism::win::sidebar::Command& command : layout.commands) {
      if (command.kind == prism::win::sidebar::PrimKind::SectionLabel &&
          command.section == section) {
        return command.text;
      }
    }
    return std::string();
  };
  const auto emptyState = [&layout]() {
    for (const prism::win::sidebar::Command& command : layout.commands) {
      if (command.kind == prism::win::sidebar::PrimKind::EmptyState) return command.text;
    }
    return std::string();
  };

  PRISM_CHECK_EQ(sectionLabel(prism::sidebar::Section::Favorites),
                 TableText(translator, "sidebar.section.favorites"));
  PRISM_CHECK_EQ(sectionLabel(prism::sidebar::Section::Recent),
                 TableText(translator, "sidebar.section.recent"));
  PRISM_CHECK_EQ(sectionLabel(prism::sidebar::Section::Tree),
                 TableText(translator, "sidebar.section.tree"));
  PRISM_CHECK_EQ(emptyState(), TableText(translator, "sidebar.empty.noArchive"));
  PRISM_CHECK(HasNonAscii(sectionLabel(prism::sidebar::Section::Favorites)));
  PRISM_CHECK(HasNonAscii(emptyState()));

  // Without a translator the four literals are unchanged (the unit suites and the
  // shell DLLs render these, and none of them has a table).
  const prism::win::sidebar::Layout plain = prism::win::sidebar::Build(model, metrics);
  PRISM_CHECK_EQ(plain.commands.size(), layout.commands.size());
  const auto plainLabel = [&plain](prism::sidebar::Section section) {
    for (const prism::win::sidebar::Command& command : plain.commands) {
      if (command.kind == prism::win::sidebar::PrimKind::SectionLabel &&
          command.section == section) {
        return command.text;
      }
    }
    return std::string();
  };
  PRISM_CHECK_EQ(plainLabel(prism::sidebar::Section::Favorites), std::string("FAVORITES"));
  PRISM_CHECK_EQ(plainLabel(prism::sidebar::Section::Tree), std::string("ARCHIVE TREE"));
}

PRISM_TEST("c40.shell: the tables are resolved from the MODULE, not the host exe") {
  PRISM_CHECK_EQ(
      prism::ContextMenuModel::LanguageDirectoryForModuleDir("C:\\Apps\\Prism"),
      std::string("C:\\Apps\\Prism\\Assets\\Lang"));
  PRISM_CHECK_EQ(
      prism::ContextMenuModel::LanguageDirectoryForModuleDir("C:\\Apps\\Prism\\"),
      std::string("C:\\Apps\\Prism\\Assets\\Lang"));
  PRISM_CHECK_EQ(prism::ContextMenuModel::LanguageDirectoryForModuleDir(""),
                 std::string());
  // The rule is about the MODULE: nothing here consults the process executable.
  PRISM_CHECK(prism::ContextMenuModel::LanguageDirectoryForModuleDir("C:\\Windows")
                  .find("explorer") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. The password gate.
// ---------------------------------------------------------------------------
PRISM_TEST("c40.open: only our own E_ABORT means 'ask for a password'") {
  PRISM_CHECK(prism::app::OpenNeedsPassword("ext=.7z route=7z; 7z.Open hr=0x80004004; "));
  PRISM_CHECK(!prism::app::OpenNeedsPassword(
      "ext=.7z route=7z; 7z.Open hr=0x00000001; Zip.Open hr=0x00000001; "));
  PRISM_CHECK(!prism::app::OpenNeedsPassword(""));
  char trace[64] = {};
  std::snprintf(trace, sizeof(trace), "hr=0x%08lX",
                static_cast<unsigned long>(prism::openflow::kOpenHrAbort));
  PRISM_CHECK(prism::app::OpenNeedsPassword(trace));
}

PRISM_TEST("c40.password: three wrong attempts exhaust the budget, on the UI thread") {
  const DWORD mainThread = GetCurrentThreadId();
  const fs::path dir = FreshDir();

  Gate gate;
  PasswordUiConfig config;
  config.archiveName = "locked.7z";
  gate.vaultPath = (dir / "passwords.vault").wstring();
  if (!PRISM_CHECK(gate.Create(GetModuleHandleW(nullptr), config))) return;

  int delivered = 0;
  for (int attempt = 1; attempt <= prism::pwd::kManualAttemptBudget; ++attempt) {
    PRISM_CHECK(gate.Submit(L"wrong-" + std::to_wstring(attempt)));
    PRISM_CHECK_EQ(gate.dialog.Model().state(), UiState::Probing);
    const bool arrived =
        PumpUntil([&gate, delivered] { return gate.sink.delivered() > delivered; });
    PRISM_CHECK(arrived);
    ++delivered;
    PRISM_CHECK_EQ(gate.dialog.Model().wrongCount(), attempt);
  }

  // Exhausted is terminal: no further submit may reach the engine.
  PRISM_CHECK_EQ(gate.dialog.Model().state(), UiState::Exhausted);
  PRISM_CHECK_EQ(gate.dialog.Model().attemptsLeft(), 0);
  PRISM_CHECK(gate.dialog.Model().view().exhausted);
  PRISM_CHECK(!gate.dialog.Model().view().canSubmit);
  PRISM_CHECK(!gate.dialog.ControlEnabled(prism::win::pwd_ui::ids::kOk));
  PRISM_CHECK(!gate.Submit(L"correct-horse-battery"));  // the OK path is closed
  PRISM_CHECK_EQ(gate.sink.delivered(), delivered);     // nothing new was posted
  PRISM_CHECK_EQ(gate.dialog.Model().state(), UiState::Exhausted);
  PRISM_CHECK_EQ(gate.dialog.Model().wrongCount(), prism::pwd::kManualAttemptBudget);

  // B9: the probe ran OFF this thread, its result arrived ON this thread.
  PRISM_CHECK(gate.probeThread != 0);
  PRISM_CHECK(gate.probeThread != mainThread);
  PRISM_CHECK_EQ(gate.sink.deliveredThread(), mainThread);
  PRISM_CHECK_EQ(gate.openedPassword, std::string());
}

PRISM_TEST("c40.password: the right password opens, and 'remember' writes the vault") {
  const DWORD mainThread = GetCurrentThreadId();
  const fs::path dir = FreshDir();
  const fs::path vault = dir / "passwords.vault";

  Gate gate;
  PasswordUiConfig config;
  config.archiveName = "locked.7z";
  gate.vaultPath = vault.wstring();
  if (!PRISM_CHECK(gate.Create(GetModuleHandleW(nullptr), config))) return;

  // "Remember in vault" is a radio button: drive the real handler.
  PRISM_CHECK(gate.Click(prism::win::pwd_ui::ids::kRemember));
  PRISM_CHECK(gate.dialog.Model().rememberChoice() == RememberChoice::Remember);

  PRISM_CHECK(gate.Submit(L"correct-horse-battery"));
  PRISM_CHECK(PumpUntil([&gate] { return gate.dialog.Model().state() == UiState::Opened; }));

  PRISM_CHECK_EQ(gate.dialog.Model().state(), UiState::Opened);
  PRISM_CHECK_EQ(gate.openedPassword, std::string("correct-horse-battery"));
  PRISM_CHECK(gate.dialog.Model().ShouldPersist());
  PRISM_CHECK(gate.rememberTried);
  PRISM_CHECK(gate.rememberWrite);

  // The vault is the DPAPI one, at the temp path this case owns: it exists, it
  // does not contain the plaintext, and a fresh reader gets the secret back.
  PRISM_CHECK(fs::exists(vault));
  const std::vector<char> raw = ReadBytes(vault);
  PRISM_CHECK(!raw.empty());
  const std::string needle = "correct-horse-battery";
  PRISM_CHECK(std::search(raw.begin(), raw.end(), needle.begin(), needle.end()) == raw.end());
  prism::win::DpapiVault reopened(vault.wstring());
  const std::vector<std::string> entries = reopened.List();
  PRISM_CHECK_EQ(entries.size(), std::size_t{1});
  if (entries.size() == 1) PRISM_CHECK_EQ(entries[0], needle);

  // B9 again, this time for the success path: the probe was a worker, the open
  // callback (and therefore the vault write) was the UI thread.
  PRISM_CHECK(gate.probeThread != 0);
  PRISM_CHECK(gate.probeThread != mainThread);
  PRISM_CHECK_EQ(gate.openedThread, mainThread);
  PRISM_CHECK_EQ(gate.sink.deliveredThread(), mainThread);
}

PRISM_TEST("c40.password: 'this time only' writes nothing") {
  const fs::path dir = FreshDir();
  const fs::path vault = dir / "passwords.vault";

  Gate gate;
  PasswordUiConfig config;
  config.archiveName = "locked.7z";
  gate.vaultPath = vault.wstring();
  if (!PRISM_CHECK(gate.Create(GetModuleHandleW(nullptr), config))) return;

  PRISM_CHECK(gate.Click(prism::win::pwd_ui::ids::kOnce));
  PRISM_CHECK(gate.dialog.Model().rememberChoice() == RememberChoice::Once);
  PRISM_CHECK(gate.Submit(L"correct-horse-battery"));
  PRISM_CHECK(PumpUntil([&gate] { return gate.dialog.Model().state() == UiState::Opened; }));
  PRISM_CHECK_EQ(gate.dialog.Model().state(), UiState::Opened);
  PRISM_CHECK(!gate.dialog.Model().ShouldPersist());
  PRISM_CHECK(!gate.rememberTried);
  PRISM_CHECK(!fs::exists(vault));
}

// ---------------------------------------------------------------------------
// 4. The budget rule itself (the mutation's target, asserted directly).
// ---------------------------------------------------------------------------
PRISM_TEST("c40.password: the retry budget is finite") {
  PRISM_CHECK(prism::pwd::UiBudgetAllows(0, 3));
  PRISM_CHECK(prism::pwd::UiBudgetAllows(2, 3));
  PRISM_CHECK(!prism::pwd::UiBudgetAllows(3, 3));
  PRISM_CHECK(!prism::pwd::UiBudgetAllows(4, 3));
}

int main(int argc, char** argv) {
  std::cout << "=== c40 i18n wire ===" << std::endl;
  if (kMutant != nullptr) {
    std::cout << "C40 MUTANT ACTIVE: " << kMutant
              << " (one rule compiled out; the matching case must go red)" << std::endl;
  }
  const int rc = prism::test::RunAll(argc, argv);

  // CONTRACTS D26: the mutant verdict is a CONJUNCTION this binary prints itself.
  if (kMutant != nullptr) {
    if (rc == 0) {
      std::cout << "C40 MUTATION NOT CAUGHT: the suite stayed green with " << kMutant
                << " compiled in -- the injection is stale" << std::endl;
      return 1;
    }
    std::cout << "C40 MUTATION CAUGHT: " << kMutant
              << " (see the FAIL lines above for the contract it broke)" << std::endl;
    return 0;
  }
  if (rc != 0) return rc;
  std::cout << "ALL C40 I18N-WIRE CHECKS PASSED" << std::endl;
  return 0;
}
