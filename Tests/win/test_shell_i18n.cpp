// PrismArchive -- C47: the Win11 context menu speaks the user's language.
//
// WHAT THIS SUITE PROVES (and how)
//   1. ShellTranslator.hpp -- the resolver C40 left inside the classic DLL -- is
//      now shared, and it obeys four rules: tables come from the RENDERING
//      module's directory, language.tag chooses the table ("auto" follows the
//      system locale), a missing/broken table falls back to the English
//      literals, and the parse happens ONCE per module lifetime.
//   2. The REAL shipping handler (Src/PrismWin/PrismShellCommand.cpp, built as
//      PrismShellExt.dll) renders the table's Chinese bytes for zh-CN and the
//      table's English bytes for en-US. The test does not ask the class directly:
//      it LoadLibrary()s a COPY of the DLL into a fixture of its own, calls
//      DllGetClassObject (the same entry point Explorer uses), drives the real
//      IExplorerCommand::GetState -> EnumSubCommands -> GetTitle path over a real
//      IShellItemArray, and compares UTF-8 STRINGS -- never a boolean.
//      Because the copy lives in the fixture and the fixture is the only place
//      that has Assets\Lang, a handler that looked at the host executable's
//      directory (C:\Windows inside explorer.exe) would answer English and fail.
//   3. The negative control: PrismShellCommand.cpp compiled with
//      PRISM_C47_MUTANT_VERB_TITLE returns the verb id ("smart.extract.here")
//      where the translated title belongs, exactly the pre-C47 shape. That build
//      is a SECOND DLL and a SECOND executable; the zh-CN case MUST go red.
//
// CONTRACTS
//   D1: the trailing marker is part of the verdict, and the asset directory is a
//       COMPILE-TIME input, so a worktree that lost Assets/Lang fails the
//       configure step instead of passing a suite that reads nothing.
//   D2: assertions compare the table's bytes and the number of verbs seen; no
//       case measures time.
//   D3 + D26: no WILL_FAIL. The marker "C47 MUTATION CAUGHT: verb_title" is a
//       CONJUNCTION the binary prints itself: suite red AND the injected string
//       observed (a title equal to a verb id is a string no table or model
//       literal can produce). A stale injection stays green, the marker is
//       absent and the CTest entry goes red. D27's first-failure helper
//       (SetMutationVerdict) is absent from main right now -- C44 is reverted,
//       its re-land is PR #67 -- so the verdict is decided once at the end; the
//       migration is the single arming call documented in main().
//   B8: nothing here assumes Explorer's thread, apartment or UI state -- the
//       handler is only ever called through its COM interface from this thread,
//       and the translator is stateless/immutable after loading.
#include <windows.h>
#include <shlobj.h>
#include <shobjidl_core.h>
#include <wrl/client.h>  // ComPtr: the classic case holds four COM objects

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ContextMenuModel.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismWin/ShellTranslator.hpp"
#include "../../Src/PrismWin/SparsePackageRegistrar.hpp"

#if !defined(PRISM_C47_LANG_DIR)
#error "PRISM_C47_LANG_DIR must name Assets/Lang (see Tests/suites/c47-shelli18n.cmake)"
#endif
#if !defined(PRISM_C47_SHELL_DLL)
#error "PRISM_C47_SHELL_DLL must name the built PrismShellExt.dll (see c47-shelli18n.cmake)"
#endif
#if !defined(PRISM_C47_CLASSIC_DLL)
#error "PRISM_C47_CLASSIC_DLL must name the built PrismShellExtClassic.dll"
#endif

namespace fs = std::filesystem;

namespace {

using prism::win::settingsfile::Utf8ToWide;
using prism::win::settingsfile::WideToUtf8;

constexpr const char* kZh = "zh-CN";
constexpr const char* kEn = "en-US";

// The mutation's own evidence: set the moment ANY command title equals its verb
// id. Nothing in a healthy build can produce that string, so this flag is the
// runtime proof CONTRACTS D27 asks for ("red is not CAUGHT").
std::atomic<bool> g_sawVerbId{false};

std::string U8(const std::wstring& text) { return WideToUtf8(text); }
std::wstring W(const std::string& text) { return Utf8ToWide(text); }

bool HasNonAscii(std::string_view text) {
  for (const char c : text) {
    if (static_cast<unsigned char>(c) >= 0x80u) return true;
  }
  return false;
}

// Unique per-process, per-case directory: a second run (or a second case) must
// never inherit tables, settings or a DLL copy from the previous one.
fs::path FreshDir(const std::string& name) {
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) /
                        ("c47_shelli18n_" + std::to_string(::GetCurrentProcessId()) + "_" +
                         name);
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  return root;
}

bool CopyTables(const fs::path& langDir, std::string* why) {
  std::error_code ec;
  fs::create_directories(langDir, ec);
  if (ec) {
    *why = "cannot create " + langDir.string();
    return false;
  }
  for (const std::string& tag : prism::i18n::SupportedTags()) {
    const fs::path from = fs::path(PRISM_C47_LANG_DIR) / (tag + ".json");
    fs::copy_file(from, langDir / (tag + ".json"), fs::copy_options::overwrite_existing, ec);
    if (ec) {
      *why = "cannot copy " + from.string();
      return false;
    }
  }
  return true;
}

// Writes the document the APP would write: %LOCALAPPDATA%\PrismArchive\
// settings.json with language.tag set. The fixture's profile directory becomes
// LOCALAPPDATA for the run, so the resolver under test reads it and the user's
// real settings are never touched.
bool WriteLanguageTag(const fs::path& profile, const char* tag, std::string* why) {
  prism::settings::SettingsStore store;
  store.ResetToDefaults();
  if (!store.SetString("language.tag", tag)) {
    *why = "the store rejected language.tag";
    return false;
  }
  const prism::win::settingsfile::Paths paths =
      prism::win::settingsfile::PathsUnder(profile.wstring());
  if (!paths.valid()) {
    *why = "the fixture profile path is not valid";
    return false;
  }
  prism::win::settingsfile::SaveResult saved;
  if (!prism::win::settingsfile::SaveStore(store, paths, &saved)) {
    *why = "cannot write " + WideToUtf8(paths.file) + " (error " +
           std::to_string(saved.error) + ")";
    return false;
  }
  return true;
}

// PROCESS-WIDE environment override with restore: the handler reads
// LOCALAPPDATA through the settings layer (SettingsFile.hpp reads the variable
// first), so pointing it at the fixture is the only hook this test needs -- no
// test-only switch leaks into the shipping resolver.
class ScopedEnv {
 public:
  ScopedEnv(const wchar_t* name, const std::wstring& value) : name_(name) {
    wchar_t buffer[32767] = {};
    const DWORD length = ::GetEnvironmentVariableW(name, buffer, 32767);
    had_ = length > 0 && length < 32767;
    if (had_) previous_.assign(buffer, static_cast<std::size_t>(length));
    ::SetEnvironmentVariableW(name, value.c_str());
  }
  ~ScopedEnv() { ::SetEnvironmentVariableW(name_, had_ ? previous_.c_str() : nullptr); }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const wchar_t* name_;
  std::wstring previous_;
  bool had_ = false;
};

// The shipped tables, read independently of the DLL (a second parse, so "the
// DLL rendered Chinese" cannot be confused with "the DLL rendered whatever the
// test's own translator said").
class Table {
 public:
  bool Load(const fs::path& langDir, const char* tag) {
    std::string why;
    for (const std::string& supported : prism::i18n::SupportedTags()) {
      const fs::path file = langDir / (supported + ".json");
      if (!translator_.LoadCatalogFile(file.string(), &why)) {
        prism::test::ReportFailure(__FILE__, __LINE__,
                                   "cannot load " + file.string() + ": " + why);
        return false;
      }
    }
    translator_.SetLanguage(tag);
    return true;
  }

  // The table's own bytes for a verb, with the verb's payload substituted. ""
  // when the key is absent, so a missing string can never look translated.
  std::string Title(std::string_view verb, const std::string& argument) const {
    const std::string key = "menu." + std::string(verb);
    if (!translator_.Has(key)) return std::string();
    return translator_.Translate(key, {{"name", argument}});
  }

  std::string Text(const std::string& key) const {
    return translator_.Has(key) ? translator_.Translate(key) : std::string();
  }

 private:
  prism::i18n::Translator translator_;
};

// ---------------------------------------------------------------------------
// The fixture: a directory that looks like an installed PrismArchive -- the DLL
// plus Assets\Lang beside it -- and a profile it can call its %LOCALAPPDATA%.
// ---------------------------------------------------------------------------
struct Fixture {
  fs::path root;
  fs::path profile;
  fs::path dll;
  fs::path classic;
  fs::path input;
};

bool BuildFixture(const std::string& name, const char* tag, Fixture* out) {
  std::string why;
  out->root = FreshDir(name);
  if (!CopyTables(out->root / "Assets" / "Lang", &why)) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return false;
  }
  out->profile = out->root / "profile";
  if (!WriteLanguageTag(out->profile, tag, &why)) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return false;
  }
  out->dll = out->root / "PrismShellExt.dll";
  out->classic = out->root / "PrismShellExtClassic.dll";
  std::error_code ec;
  fs::copy_file(fs::path(W(PRISM_C47_SHELL_DLL)), out->dll,
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    prism::test::ReportFailure(__FILE__, __LINE__,
                               "cannot copy the handler DLL: " + ec.message());
    return false;
  }
  // The Win10 DLL is copied too: after C47 extracted the resolver, BOTH DLLs must
  // still speak the user's language, and only an end-to-end case can say so.
  fs::copy_file(fs::path(W(PRISM_C47_CLASSIC_DLL)), out->classic,
                fs::copy_options::overwrite_existing, ec);
  if (ec) {
    prism::test::ReportFailure(__FILE__, __LINE__,
                               "cannot copy the classic DLL: " + ec.message());
    return false;
  }
  out->input = out->root / "input";
  fs::create_directories(out->input, ec);
  for (const char* name7z : {"report.7z", "second.7z", "notes.txt"}) {
    std::ofstream file(out->input / name7z, std::ios::binary);
    file << "c47 fixture";
  }
  return true;
}

// ---------------------------------------------------------------------------
// The real COM surface, exactly the way Explorer reaches it.
// ---------------------------------------------------------------------------
struct ShellHost {
  HMODULE module = nullptr;
  IClassFactory* factory = nullptr;
  IExplorerCommand* command = nullptr;

  ~ShellHost() {
    if (command != nullptr) command->Release();
    if (factory != nullptr) factory->Release();
    if (module != nullptr) ::FreeLibrary(module);
  }

  bool Open(const fs::path& dll, std::string* why) {
    module = ::LoadLibraryW(dll.c_str());
    if (module == nullptr) {
      *why = "LoadLibraryW(" + dll.string() + ") failed with " +
             std::to_string(::GetLastError());
      return false;
    }
    using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
    const auto getClassObject = reinterpret_cast<GetClassObjectFn>(
        ::GetProcAddress(module, "DllGetClassObject"));
    if (getClassObject == nullptr) {
      *why = "DllGetClassObject is not exported";
      return false;
    }
    const HRESULT hr = getClassObject(prism::win::shell_ids::kExplorerCommandClsid,
                                      IID_IClassFactory,
                                      reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || factory == nullptr) {
      *why = "DllGetClassObject failed (hr=" + std::to_string(hr) + ")";
      return false;
    }
    if (FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&command))) ||
        command == nullptr) {
      *why = "IClassFactory::CreateInstance(IExplorerCommand) failed";
      return false;
    }
    return true;
  }
};

struct Selection {
  IShellItemArray* array = nullptr;
  ~Selection() {
    if (array != nullptr) array->Release();
  }
};

bool MakeSelection(const std::vector<fs::path>& files, Selection* out, std::string* why) {
  why->clear();
  // Owned pidls and the const-qualified view the API declares
  // (LPCITEMIDLIST*): passing the owning vector would drop the const qualifier.
  std::vector<PIDLIST_ABSOLUTE> pidls;
  std::vector<PCIDLIST_ABSOLUTE> view;
  for (const fs::path& file : files) {
    IShellItem* item = nullptr;
    HRESULT hr = ::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || item == nullptr) {
      *why = "SHCreateItemFromParsingName(" + file.string() + ") failed";
      return false;
    }
    PIDLIST_ABSOLUTE pidl = nullptr;
    hr = ::SHGetIDListFromObject(item, &pidl);
    item->Release();
    if (FAILED(hr) || pidl == nullptr) {
      *why = "SHGetIDListFromObject failed";
      return false;
    }
    pidls.push_back(pidl);
    view.push_back(pidl);
  }
  const HRESULT hr = ::SHCreateShellItemArrayFromIDLists(
      static_cast<UINT>(view.size()), view.data(), &out->array);
  for (PIDLIST_ABSOLUTE pidl : pidls) ::CoTaskMemFree(pidl);
  if (FAILED(hr) || out->array == nullptr) {
    *why = "SHCreateShellItemArrayFromIDLists failed";
    return false;
  }
  return true;
}

// GetState -> EnumSubCommands -> GetTitle, UTF-8 out. This is the whole render
// path the user's right-click drives.
bool ReadTitles(IExplorerCommand* command, IShellItemArray* selection,
                std::vector<std::string>* titles, std::string* why) {
  why->clear();
  EXPCMDSTATE state = ECS_DISABLED;
  HRESULT hr = command->GetState(selection, FALSE, &state);
  if (FAILED(hr) || state != ECS_ENABLED) {
    *why = "GetState did not enable the command";
    return false;
  }
  IEnumExplorerCommand* enumerator = nullptr;
  hr = command->EnumSubCommands(&enumerator);
  if (FAILED(hr) || enumerator == nullptr) {
    *why = "EnumSubCommands failed";
    return false;
  }
  for (;;) {
    IExplorerCommand* sub = nullptr;
    ULONG fetched = 0;
    hr = enumerator->Next(1, &sub, &fetched);
    if (FAILED(hr)) {
      *why = "IEnumExplorerCommand::Next failed";
      break;
    }
    if (fetched == 0 || sub == nullptr) break;
    LPWSTR name = nullptr;
    hr = sub->GetTitle(nullptr, &name);
    if (FAILED(hr) || name == nullptr) {
      sub->Release();
      *why = "GetTitle failed";
      break;
    }
    titles->push_back(U8(std::wstring(name)));
    ::CoTaskMemFree(name);
    sub->Release();
  }
  enumerator->Release();
  return why->empty();
}

// CF_HDROP data object for one file: the shape IShellExtInit receives when the
// user right-clicks a file in Explorer.
bool MakeFileDataObject(const fs::path& file, IDataObject** out, std::string* why) {
  why->clear();
  Microsoft::WRL::ComPtr<IShellItem> item;
  HRESULT hr = ::SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&item));
  if (FAILED(hr) || item == nullptr) {
    *why = "SHCreateItemFromParsingName(" + file.string() + ") failed";
    return false;
  }
  PIDLIST_ABSOLUTE full = nullptr;
  hr = ::SHGetIDListFromObject(item.Get(), &full);
  if (FAILED(hr) || full == nullptr) {
    *why = "SHGetIDListFromObject failed";
    return false;
  }
  // Parent FOLDER + child item: the CIDA shape the shell builds for "these
  // selected items in this folder". Passing the file's own pidl as the "folder"
  // produces a folder data object, and IShellExtInit then finds no CF_HDROP.
  PIDLIST_ABSOLUTE parent = ::ILClone(full);
  ::ILRemoveLastID(parent);
  PCUITEMID_CHILD child = ::ILFindLastID(full);
  hr = ::SHCreateDataObject(parent, 1, &child, nullptr, IID_PPV_ARGS(out));
  ::CoTaskMemFree(parent);
  ::CoTaskMemFree(full);
  if (FAILED(hr) || *out == nullptr) {
    *why = "SHCreateDataObject failed";
    return false;
  }
  return true;
}

// Loads the Win10 classic DLL, hands it one file the way Explorer does
// (IShellExtInit + CF_HDROP), renders the popup through
// IContextMenu::QueryContextMenu and reads the labels back out of the HMENU.
//
// Deliberately NO FreeLibrary: the COM objects below hold vtables that live in
// this DLL, and Explorer keeps both shell extensions resident anyway
// (DllCanUnloadNow answers S_FALSE). Unload-then-release is a use-after-free
// this case ran into on the first run.
bool RenderClassicMenu(const fs::path& dll, const fs::path& file, std::string* popupTitle,
                       UINT* inserted, std::string* firstLabel, std::string* why) {
  why->clear();
  HMODULE module = ::LoadLibraryW(dll.c_str());
  if (module == nullptr) {
    *why = "LoadLibraryW(" + dll.string() + ") failed with " +
           std::to_string(::GetLastError());
    return false;
  }
  using GetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**);
  const auto getClassObject = reinterpret_cast<GetClassObjectFn>(
      ::GetProcAddress(module, "DllGetClassObject"));
  if (getClassObject == nullptr) {
    *why = "DllGetClassObject is not exported";
    return false;
  }
  Microsoft::WRL::ComPtr<IClassFactory> factory;
  HRESULT hr = getClassObject(prism::win::shell_ids::kClassicMenuClsid, IID_IClassFactory,
                              reinterpret_cast<void**>(factory.GetAddressOf()));
  if (FAILED(hr) || factory == nullptr) {
    *why = "classic DllGetClassObject failed";
    return false;
  }
  Microsoft::WRL::ComPtr<IShellExtInit> extension;
  hr = factory->CreateInstance(nullptr, IID_PPV_ARGS(&extension));
  if (FAILED(hr) || extension == nullptr) {
    *why = "CreateInstance(IShellExtInit) failed";
    return false;
  }
  // The shell asks for IContextMenu first and IShellExtInit second (D10), so QI
  // instead of assuming the object answers both.
  Microsoft::WRL::ComPtr<IContextMenu> menu;
  hr = extension.As(&menu);
  if (FAILED(hr) || menu == nullptr) {
    *why = "QueryInterface(IContextMenu) failed";
    return false;
  }
  Microsoft::WRL::ComPtr<IDataObject> data;
  if (!MakeFileDataObject(file, data.GetAddressOf(), why)) return false;
  hr = extension->Initialize(nullptr, data.Get(), nullptr);
  if (FAILED(hr)) {
    *why = "IShellExtInit::Initialize rejected the CF_HDROP selection";
    return false;
  }

  HMENU owner = ::CreatePopupMenu();
  if (owner == nullptr) {
    *why = "CreatePopupMenu failed";
    return false;
  }
  hr = menu->QueryContextMenu(owner, 0, 1, 0x7FFF, 0);
  if (FAILED(hr)) {
    ::DestroyMenu(owner);
    *why = "QueryContextMenu failed";
    return false;
  }
  *inserted = static_cast<UINT>(HRESULT_CODE(hr));

  wchar_t title[64] = {};
  ::GetMenuStringW(owner, 0, title, 64, MF_BYPOSITION);
  *popupTitle = U8(std::wstring(title));

  HMENU submenu = ::GetSubMenu(owner, 0);
  if (submenu == nullptr) {
    ::DestroyMenu(owner);
    *why = "the popup has no submenu";
    return false;
  }
  wchar_t label[256] = {};
  if (::GetMenuStringW(submenu, 0, label, 256, MF_BYPOSITION) == 0) {
    ::DestroyMenu(owner);
    *why = "the submenu has no first item";
    return false;
  }
  *firstLabel = U8(std::wstring(label));
  ::DestroyMenu(owner);
  return true;
}

// The verb ids the shipping model produces for each selection, in menu order.
std::vector<prism::MenuCommand> EnglishModel(const std::vector<std::string>& selection) {
  return prism::ContextMenuModel::BuildDefault(selection);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Resolved once per module (CONTRACTS D2: a COUNT, not a stopwatch).
//
// This case runs FIRST on purpose: no earlier case may warm the cache, so the
// count below is the number of resolutions this module has performed at all.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.once: the shared translator resolves once per module") {
  PRISM_CHECK_EQ(prism::win::shelli18n::ResolveCount().load(), 0u);

  const prism::win::shelli18n::ShellTranslator* first = prism::win::shelli18n::Cached();
  const prism::win::shelli18n::ShellTranslator* second = prism::win::shelli18n::Cached();
  PRISM_CHECK(first == second);
  PRISM_CHECK_EQ(prism::win::shelli18n::ResolveCount().load(), 1u);

  // USING it must not re-read the tables either: that is the rule that keeps a
  // right-click from parsing JSON.
  const std::vector<prism::MenuCommand> model =
      prism::win::shelli18n::BuildMenuModel({"a.7z"});
  PRISM_CHECK_EQ(model.size(), std::size_t{5});
  PRISM_CHECK_EQ(prism::win::shelli18n::ResolveCount().load(), 1u);
  if (first != nullptr) PRISM_CHECK(first->ready());
}

// ---------------------------------------------------------------------------
// 2. The tables follow the MODULE, never the host executable.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.dir: the language directory follows the rendering module") {
  const std::wstring directory = prism::win::shelli18n::ThisModuleDirectory();
  PRISM_CHECK(!directory.empty());

  wchar_t exe[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, exe, MAX_PATH);
  PRISM_CHECK(length > 0 && length < MAX_PATH);
  const std::wstring exePath(exe, static_cast<std::size_t>(length));
  const std::size_t slash = exePath.find_last_of(L"\\/");
  PRISM_CHECK(slash != std::wstring::npos);
  const std::wstring exeDir = exePath.substr(0, slash);

  // For THIS module (the test exe) the two answers coincide. The end-to-end case
  // is what proves they diverge for the handler: its DLL is copied into the
  // fixture, so a handler that used the host executable's directory -- the
  // pre-C47 shape, and C:\Windows inside explorer.exe -- would have no tables.
  PRISM_CHECK_EQ(U8(directory), U8(exeDir));
  PRISM_CHECK_EQ(prism::ContextMenuModel::LanguageDirectoryForModuleDir(U8(directory)),
                 U8(directory) + "\\Assets\\Lang");
  PRISM_CHECK(prism::ContextMenuModel::LanguageDirectoryForModuleDir("").empty());
}

// ---------------------------------------------------------------------------
// 3. Every failure falls back to the English literals, never to a key.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.fallback: no usable table means English, never 'menu.x.y'") {
  const fs::path root = FreshDir("no-tables");
  prism::win::shelli18n::Source source;
  source.moduleDirectory = root.string();
  source.requestedTag = kZh;
  source.systemLocale = kZh;

  // (a) no Assets directory at all.
  PRISM_CHECK(prism::win::shelli18n::ShellTranslator::Load(source) == nullptr);

  // (b) an Assets\Lang directory that holds no table.
  const fs::path langDir = root / "Assets" / "Lang";
  std::error_code ec;
  fs::create_directories(langDir, ec);
  PRISM_CHECK(prism::win::shelli18n::ShellTranslator::Load(source) == nullptr);

  // (c) a table the JSON reader rejects is not a table either: a half-readable
  // document must not produce a half-translated menu.
  {
    std::ofstream broken(langDir / "zh-CN.json", std::ios::binary);
    broken << "{ \"language\": \"zh-CN\", this is not json";
  }
  PRISM_CHECK(prism::win::shelli18n::ShellTranslator::Load(source) == nullptr);

  // The model without a translator keeps the literals C19 pins as en-US.
  const std::vector<prism::MenuCommand> defaults = EnglishModel({"a.7z"});
  PRISM_CHECK_EQ(defaults.size(), std::size_t{5});
  PRISM_CHECK_EQ(defaults[0].title, std::string("Smart Extract Here"));
  PRISM_CHECK(!HasNonAscii(defaults[0].title));
  PRISM_CHECK(defaults[0].title.rfind("menu.", 0) != 0);
}

// ---------------------------------------------------------------------------
// 4. language.tag -> table; "auto" -> system locale; unsupported -> en-US.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.language: language.tag selects the table, and the titles change") {
  const fs::path root = FreshDir("load");
  std::string why;
  if (!PRISM_CHECK(CopyTables(root / "Assets" / "Lang", &why))) return;

  prism::win::shelli18n::Source source;
  source.moduleDirectory = root.string();
  source.requestedTag = kZh;
  source.systemLocale = kEn;  // the SETTINGS value must win over the system one

  const std::shared_ptr<const prism::win::shelli18n::ShellTranslator> zh =
      prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(zh != nullptr)) return;
  PRISM_CHECK(zh->ready());
  PRISM_CHECK_EQ(zh->language(), std::string(kZh));
  PRISM_CHECK_EQ(zh->catalogCount(), std::size_t{2});
  PRISM_CHECK(zh->keyCount() > 0);
  PRISM_CHECK_EQ(zh->languageDirectory(), root.string() + "\\Assets\\Lang");

  Table table;
  if (!PRISM_CHECK(table.Load(root / "Assets" / "Lang", kZh))) return;
  const std::vector<prism::MenuCommand> english = EnglishModel({"a.7z"});
  const std::vector<prism::MenuCommand> translated = zh->BuildModel({"a.7z"});
  if (!PRISM_CHECK_EQ(translated.size(), english.size())) return;
  for (std::size_t i = 0; i < translated.size(); ++i) {
    const std::string expected = table.Title(english[i].verb, english[i].titleArgument);
    PRISM_CHECK(!expected.empty());
    PRISM_CHECK(HasNonAscii(expected));
    PRISM_CHECK(expected != english[i].title);  // the fixture really is Chinese
    PRISM_CHECK_EQ(translated[i].title, expected);
  }
  // The same object, switched through a fresh resolution: en-US is the table's
  // English, not a stale zh-CN string (this is the "language switch" rule -- a
  // new Source, a new snapshot).
  source.requestedTag = kEn;
  const auto en = prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(en != nullptr)) return;
  PRISM_CHECK_EQ(en->language(), std::string(kEn));
  const std::vector<prism::MenuCommand> englishRendered = en->BuildModel({"a.7z"});
  if (!PRISM_CHECK_EQ(englishRendered.size(), english.size())) return;
  for (std::size_t i = 0; i < english.size(); ++i) {
    PRISM_CHECK_EQ(englishRendered[i].title, english[i].title);
    PRISM_CHECK(!HasNonAscii(englishRendered[i].title));
  }
  PRISM_CHECK_EQ(en->TableText("menu.extract.here"), std::string("Extract Here"));
  PRISM_CHECK(HasNonAscii(zh->TableText("menu.extract.here")));
  PRISM_CHECK_EQ(zh->TableText("menu.extract.here"), std::string("\xE8\xA7\xA3\xE5\x8E\x8B"
                                                                  "\xE5\x88\xB0\xE6\xAD\xA4"
                                                                  "\xE5\xA4\x84"));

  // "auto" follows the system locale; an unsupported tag lands on en-US.
  source.requestedTag = "auto";
  source.systemLocale = kZh;
  const auto autoZh = prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(autoZh != nullptr)) return;
  PRISM_CHECK_EQ(autoZh->language(), std::string(kZh));

  source.systemLocale = "en-GB";
  const auto autoEnGb = prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(autoEnGb != nullptr)) return;
  PRISM_CHECK_EQ(autoEnGb->language(), std::string(kEn));

  source.requestedTag = "ja-JP";  // a legal settings value with no table here
  source.systemLocale = kZh;
  const auto ja = prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(ja != nullptr)) return;
  PRISM_CHECK_EQ(ja->language(), std::string(kEn));
}

// ---------------------------------------------------------------------------
// 5. The REAL handler: zh-CN titles through IExplorerCommand.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.shell: the real IExplorerCommand renders the zh-CN table") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture("zh", kZh, &fixture))) return;

  // The handler resolves its tables and its language on first use, so the
  // profile must be in place before the DLL starts answering.
  const ScopedEnv profile(L"LOCALAPPDATA", fixture.profile.wstring());
  ShellHost host;
  std::string why;
  if (!PRISM_CHECK(host.Open(fixture.dll, &why))) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return;
  }

  Table table;
  if (!PRISM_CHECK(table.Load(fixture.root / "Assets" / "Lang", kZh))) return;

  const std::vector<std::vector<std::string>> selections = {
      {"report.7z"},
      {"report.7z", "second.7z"},
      {"report.7z", "notes.txt"},
  };
  std::vector<std::string> verbsSeen;
  std::size_t rows = 0;
  for (const std::vector<std::string>& selection : selections) {
    const std::vector<prism::MenuCommand> expected = EnglishModel(selection);
    std::vector<fs::path> files;
    for (const std::string& name : selection) files.push_back(fixture.input / name);

    Selection items;
    if (!PRISM_CHECK(MakeSelection(files, &items, &why))) {
      prism::test::ReportFailure(__FILE__, __LINE__, why);
      continue;
    }
    std::vector<std::string> titles;
    if (!PRISM_CHECK(ReadTitles(host.command, items.array, &titles, &why))) {
      prism::test::ReportFailure(__FILE__, __LINE__, why);
      continue;
    }
    if (!PRISM_CHECK_EQ(titles.size(), expected.size())) continue;

    for (std::size_t i = 0; i < titles.size(); ++i) {
      const std::string zh = table.Title(expected[i].verb, expected[i].titleArgument);
      if (!zh.empty() && titles[i] == expected[i].verb) {
        g_sawVerbId.store(true, std::memory_order_relaxed);
      }
      PRISM_CHECK_EQ(titles[i], zh);
      PRISM_CHECK(!titles[i].empty());
      PRISM_CHECK(HasNonAscii(titles[i]));
      PRISM_CHECK(titles[i] != expected[i].title);  // English would be the old bug
      PRISM_CHECK(titles[i] != expected[i].verb);   // 'menu' ids are not labels
      verbsSeen.push_back(expected[i].verb);
      ++rows;
    }
  }

  // All EIGHT verbs the tables define were rendered (5 + 3 + 7 rows).
  std::sort(verbsSeen.begin(), verbsSeen.end());
  verbsSeen.erase(std::unique(verbsSeen.begin(), verbsSeen.end()), verbsSeen.end());
  PRISM_CHECK_EQ(verbsSeen.size(), std::size_t{8});
  PRISM_CHECK_EQ(rows, std::size_t{15});
}

// ---------------------------------------------------------------------------
// 6. The same handler under en-US: the table's English, byte for byte.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.shell: the same IExplorerCommand renders the en-US table") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture("en", kEn, &fixture))) return;

  const ScopedEnv profile(L"LOCALAPPDATA", fixture.profile.wstring());
  ShellHost host;
  std::string why;
  if (!PRISM_CHECK(host.Open(fixture.dll, &why))) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return;
  }

  Table table;
  if (!PRISM_CHECK(table.Load(fixture.root / "Assets" / "Lang", kEn))) return;

  const std::vector<std::string> selection = {"report.7z"};
  const std::vector<prism::MenuCommand> expected = EnglishModel(selection);
  std::vector<fs::path> files;
  for (const std::string& name : selection) files.push_back(fixture.input / name);

  Selection items;
  if (!PRISM_CHECK(MakeSelection(files, &items, &why))) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return;
  }
  std::vector<std::string> titles;
  if (!PRISM_CHECK(ReadTitles(host.command, items.array, &titles, &why))) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return;
  }
  if (!PRISM_CHECK_EQ(titles.size(), expected.size())) return;
  for (std::size_t i = 0; i < titles.size(); ++i) {
    const std::string english = table.Title(expected[i].verb, expected[i].titleArgument);
    PRISM_CHECK(!english.empty());
    PRISM_CHECK(!HasNonAscii(english));
    PRISM_CHECK_EQ(titles[i], english);
    PRISM_CHECK_EQ(titles[i], expected[i].title);
    if (titles[i] == expected[i].verb) g_sawVerbId.store(true, std::memory_order_relaxed);
    PRISM_CHECK(titles[i] != expected[i].verb);
  }
}


// ---------------------------------------------------------------------------
// 6b. The Win10 layer after the extraction: it must STILL speak Chinese.
//
// ShellTranslator.hpp replaced the resolver that used to live in
// PrismClassicContextMenu.cpp. A compile-only check cannot tell whether the
// replacement is wired, so this case loads the real classic DLL, hands it a
// CF_HDROP selection through IShellExtInit, renders the popup through
// IContextMenu::QueryContextMenu and reads the menu item text back out of the
// HMENU -- the Win10 twin of the IExplorerCommand case above.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.classic: the Win10 IContextMenu renders the same zh-CN text") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture("classic", kZh, &fixture))) return;
  const ScopedEnv profile(L"LOCALAPPDATA", fixture.profile.wstring());

  Table table;
  if (!PRISM_CHECK(table.Load(fixture.root / "Assets" / "Lang", kZh))) return;

  std::string popupTitle;
  std::string firstLabel;
  std::string why;
  UINT inserted = 0;
  if (!PRISM_CHECK(RenderClassicMenu(fixture.classic, fixture.input / "report.7z", &popupTitle,
                                     &inserted, &firstLabel, &why))) {
    prism::test::ReportFailure(__FILE__, __LINE__, why);
    return;
  }
  PRISM_CHECK_EQ(inserted, 5u);
  PRISM_CHECK_EQ(popupTitle, std::string("PrismArchive"));

  const std::string expected = table.Title("smart.extract.here", "");
  PRISM_CHECK(!expected.empty());
  PRISM_CHECK(HasNonAscii(expected));
  PRISM_CHECK_EQ(firstLabel, expected);
  PRISM_CHECK(HasNonAscii(firstLabel));
  PRISM_CHECK(firstLabel != std::string("Smart Extract Here"));
}

// ---------------------------------------------------------------------------
// 7. The resolver is thread-safe by construction (B8).
//
// Four threads hammer the ONE shared snapshot: the tables are read-only, and the
// only mutable state behind the pointer (PrismCore's miss counter) is behind
// BuildModel's lock. A data race here would show up as a corrupt string, and the
// case asserts every title still equals the table's bytes.
// ---------------------------------------------------------------------------
PRISM_TEST("c47.threads: concurrent GetTitle-class lookups read one snapshot") {
  const fs::path root = FreshDir("threads");
  std::string why;
  if (!PRISM_CHECK(CopyTables(root / "Assets" / "Lang", &why))) return;

  prism::win::shelli18n::Source source;
  source.moduleDirectory = root.string();
  source.requestedTag = kZh;
  source.systemLocale = kZh;
  const auto shared = prism::win::shelli18n::ShellTranslator::Load(source);
  if (!PRISM_CHECK(shared != nullptr)) return;

  // The expected bytes are resolved ONCE on this thread, so the workers only
  // read them: the case is about the SHARED translator, not about the test's own
  // table object (whose miss bookkeeping is not synchronised, by design).
  Table table;
  if (!PRISM_CHECK(table.Load(root / "Assets" / "Lang", kZh))) return;
  std::vector<std::pair<std::string, std::string>> expected;
  for (const prism::MenuCommand& item : EnglishModel({"a.7z"})) {
    expected.emplace_back(item.verb, table.Title(item.verb, item.titleArgument));
  }
  if (!PRISM_CHECK_EQ(expected.size(), std::size_t{5})) return;
  for (const std::pair<std::string, std::string>& row : expected) {
    if (!PRISM_CHECK(!row.second.empty())) return;
  }

  std::atomic<int> mismatches{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 4; ++worker) {
    workers.emplace_back([shared, &expected, &mismatches] {
      for (int round = 0; round < 200; ++round) {
        const std::vector<prism::MenuCommand> model = shared->BuildModel({"a.7z"});
        if (model.size() != expected.size()) {
          mismatches.fetch_add(1);
          continue;
        }
        for (std::size_t i = 0; i < model.size(); ++i) {
          if (model[i].verb != expected[i].first || model[i].title != expected[i].second) {
            mismatches.fetch_add(1);
          }
        }
      }
    });
  }
  for (std::thread& worker : workers) worker.join();
  PRISM_CHECK_EQ(mismatches.load(), 0);
}

// TODO(D27): after #67 lands, replace this block with SetMutationVerdict(prefix, predicate)
// -- semantics are already the same conjunction.
int main(int argc, char** argv) {
  std::cout << "=== c47 shell i18n ===" << std::endl;

#if defined(PRISM_C47_MUTANT_VERB_TITLE)
  std::cout << "C47 MUTANT ACTIVE: verb_title (GetTitle hands Explorer the verb id; the "
               "zh-CN case must go red)"
            << std::endl;
  // CONTRACTS D3/D26: the CAUGHT marker below is a CONJUNCTION this binary prints
  // itself -- the suite went red AND the injected shape was observed (a title
  // equal to its verb id cannot come from any table or model literal), so an
  // unrelated failure stays RED instead of printing a false CAUGHT.
  //
  // D27's SetMutationVerdict (verdict at the FIRST failure) is deliberately NOT
  // called: main carries a revert of card C44 (ce6cf26; re-land PR #67 still
  // open) that removed the helper from Tests/mini_test.hpp. When C44 re-lands,
  // arming it here with exactly the predicate asserted below is the one-line
  // change that turns this into a D27 verdict.
#endif

  // The user's real settings are never touched: the resolver reads
  // %LOCALAPPDATA%, so every case runs against a fixture of its own.
  const fs::path isolated = FreshDir("profile");
  {
    std::string why;
    if (!WriteLanguageTag(isolated, kEn, &why)) {
      std::cout << "FAIL: " << why << std::endl;
      return 1;
    }
  }
  const ScopedEnv profile(L"LOCALAPPDATA", isolated.wstring());

  // B8: this test drives a COM object from THIS thread, apartment included --
  // the shipping code makes no such assumption, the caller must.
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "FAIL: CoInitializeEx(APARTMENTTHREADED) failed" << std::endl;
    return 1;
  }

  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();

#if defined(PRISM_C47_MUTANT_VERB_TITLE)
  // CONTRACTS D26/D3: CAUGHT only when the suite went RED *and* the run-time
  // predicate saw the injected defect. Green means the injection is stale; red
  // without a verb-id title means the failure was unrelated. Both stay RED.
  // Exit 0 on CAUGHT because CTest decides with the PASS_REGULAR_EXPRESSION on
  // the marker, never with WILL_FAIL (D26).
  if (rc == 0) {
    std::cout << "C47 MUTATION NOT CAUGHT: the suite stayed green with verb_title compiled "
                 "in -- the injection is stale"
              << std::endl;
    return 1;
  }
  if (!g_sawVerbId.load(std::memory_order_relaxed)) {
    std::cout << "C47 MUTATION NOT CAUGHT: the suite went red, but no title was ever the verb "
                 "id, so the injected defect did not fire here"
              << std::endl;
    return 1;
  }
  std::cout << "C47 MUTATION CAUGHT: verb_title (suite red AND a command title was the verb "
               "id; see the FAIL lines above)"
            << std::endl;
  return 0;
#else
  if (rc != 0) return rc;
  std::cout << "ALL C47 SHELL-I18N CHECKS PASSED" << std::endl;
  return 0;
#endif
}