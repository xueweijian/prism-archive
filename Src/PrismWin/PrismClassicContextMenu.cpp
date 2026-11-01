// PrismArchive -- PrismWin
// Win10 classic shell extension: IShellExtInit + IContextMenu (blueprint ch. 4.1,
// M2 compat layer).
//
// Win11 uses IExplorerCommand + the MSIX sparse package (PrismShellCommand.cpp);
// Win10 still needs the old IContextMenu / IShellExtInit COM contract. Both
// render the SAME PrismCore ContextMenuModel -- this file is a view, not a brain.
//
// Registration for this DLL is per-user HKCU and needs no package identity:
// tools/msix/register-classic.ps1 writes the CLSID + the ContextMenuHandlers
// entry c3's installer calls. Clients/installer must use the CLSID declared in
// SparsePackageRegistrar.hpp -- that header is the single source of truth and
// Tests/win/test_sparse_registrar.cpp cross-checks it against the manifest.
//
// Contract notes that cost real debugging time:
//  * QueryContextMenu returns the number of menu IDs consumed, and must never
//    reserve an ID >= idCmdLast (the shell reuses that range for "Open with").
//  * InvokeCommand's lpVerb is an OFFSET from idCmdFirst, not our verb string.
//  * The selection lives in an STGMEDIUM that WE own: it must be released, and
//    it must not be a process-wide static (two Explorer windows would share it).
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>  // RuntimeClass / ClassicCom / MakeAndInitialize

#include <string>
#include <vector>

#include "../PrismCore/ContextMenuModel.hpp"
#include "../PrismCore/I18n.hpp"       // C40: the menu text is the app's tables
#include "CompanionPreviewBridge.hpp"  // C30 inline-preview IPC (fire-and-forget)
#include "PrismEngineBridge.hpp"       // WideToUtf8 / Utf8ToWide
#include "SettingsFile.hpp"            // C40: the user's language.tag
#include "SparsePackageRegistrar.hpp"

using namespace Microsoft::WRL;

namespace prism::win::classic {

using shell_ids::kClassicMenuClsid;

namespace {

// Verb ids as UTF-16 for GetCommandString(GCS_VERBW); the shell uses these for
// "always do this" / telemetry, and our own tests use them to prove that the
// offset -> verb mapping is right WITHOUT launching a process.
std::wstring VerbWide(const std::string& verb) { return Utf8ToWide(verb); }

// Blueprint ch. 4.1 trigger: the moment the menu opens, hand the first selected
// path plus the click position to the companion flyout over the C30 channel.
//
// Two rules decide the shape of this function:
//   * it runs on explorer.exe's UI thread inside QueryContextMenu, so it must be
//     non-blocking -- ackTimeoutMs=0 means "write, signal, return" with no wait;
//   * it must not create shell state when the companion is absent.
//     TryPublishPreview only OPENS an existing section, so "no companion" costs
//     two failed NtOpenSection calls and the menu opens exactly as before.
// The shell does NOT enumerate the archive: listing entries belongs to the
// companion process (PrismCore), never to explorer's UI thread. entries stays
// empty and kFlagNeedsCatalogScan asks the companion to fill the first N itself.
// C40 -- THE MENU SPEAKS THE USER'S LANGUAGE.
//
// Two facts decide the shape of this block:
//   * explorer.exe owns the process, so "the executable's directory" is
//     C:\Windows. The tables live next to the DLL that renders the menu, and the
//     install layout puts that DLL in the same folder as PrismArchive.exe.
//   * the language is the user's, not the process's: it comes from the same
//     settings document the app reads (language.tag), resolved ONCE per DLL
//     lifetime -- a context menu must not parse JSON on every right-click.
// Any failure (no tables, no settings, an unreadable file) leaves the translator
// null, and ContextMenuModel::Build then keeps its English defaults: a menu with
// English labels is usable, a menu with "menu.extract.here" is not.
std::wstring ModuleDirectory() {
  HMODULE module = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModuleDirectory), &module) == FALSE) {
    return std::wstring();
  }
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetModuleFileNameW(module, buffer, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) return std::wstring();
  const std::wstring path(buffer, static_cast<size_t>(length));
  const size_t slash = path.find_last_of(L"\\/");
  return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::string SystemLocaleUtf8() {
  wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
  const int length = ::GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  return length > 1
             ? WideToUtf8(std::wstring(locale, static_cast<size_t>(length - 1)))
             : std::string();
}

std::string RequestedLanguage() {
  prism::settings::SettingsStore store;
  const prism::win::settingsfile::LoadReport report =
      prism::win::settingsfile::LoadStore(store, prism::win::settingsfile::DefaultPaths());
  static_cast<void>(report);  // a missing/corrupt document means "auto"
  return store.GetString("language.tag", "auto");
}

const prism::i18n::Translator* ShellTranslator() {
  struct State {
    prism::i18n::Translator translator;
    bool ready = false;
  };
  // Function-local static: thread-safe initialisation (C++11), one parse per
  // DLL lifetime, no allocation that outlives the module.
  static const State state = [] {
    State loaded;
    const std::wstring directory =
        prism::win::Utf8ToWide(prism::ContextMenuModel::LanguageDirectoryForModuleDir(
            WideToUtf8(ModuleDirectory())));
    if (directory.empty()) return loaded;
    for (const std::string& tag : prism::i18n::SupportedTags()) {
      const std::wstring file = directory + L"\\" + Utf8ToWide(tag) + L".json";
      if (loaded.translator.LoadCatalogFile(WideToUtf8(file))) loaded.ready = true;
    }
    if (!loaded.ready) return loaded;
    const prism::i18n::LanguageChoice choice =
        prism::i18n::ChooseLanguage(RequestedLanguage(), SystemLocaleUtf8());
    loaded.translator.SetLanguage(choice.tag);
    return loaded;
  }();
  return state.ready ? &state.translator : nullptr;
}

void PublishInlinePreview(const std::vector<std::string>& selected) noexcept {
  if (selected.empty()) return;
  POINT cursor{};
  if (!::GetCursorPos(&cursor)) return;  // no anchor: the flyout would be guessed
  const auto packet =
      prism::win::companion::MakeShellPacket(Utf8ToWide(selected.front()), cursor);
  (void)prism::win::companion::TryPublishPreview(packet, /*ackTimeoutMs=*/0);
}

}  // namespace

class ClassicMenu final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellExtInit, IContextMenu> {
 public:
  ClassicMenu() = default;
  ~ClassicMenu() override {
    if (medium_.hGlobal) {
      ::ReleaseStgMedium(&medium_);
      medium_ = STGMEDIUM{};
    }
  }

  // IShellExtInit: pull CF_HDROP out of the data object.
  STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* dataObject, HKEY) override {
    if (!dataObject) return E_INVALIDARG;
    FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (medium_.hGlobal) {
      ::ReleaseStgMedium(&medium_);
      medium_ = STGMEDIUM{};
    }
    if (FAILED(dataObject->GetData(&format, &medium_))) return E_FAIL;
    const UINT count =
        ::DragQueryFileW(static_cast<HDROP>(medium_.hGlobal), 0xFFFFFFFF, nullptr, 0);
    selected_.clear();
    for (UINT i = 0; i < count; ++i) {
      const UINT needed =
          ::DragQueryFileW(static_cast<HDROP>(medium_.hGlobal), i, nullptr, 0);
      if (needed == 0) continue;
      std::wstring path(static_cast<size_t>(needed) + 1, L'\0');
      const UINT written = ::DragQueryFileW(static_cast<HDROP>(medium_.hGlobal), i,
                                            path.data(), needed + 1);
      if (written == 0) continue;
      path.resize(written);
      selected_.push_back(WideToUtf8(path));
    }
    return selected_.empty() ? E_FAIL : S_OK;
  }

  // IContextMenu: render the PrismCore model into a popup submenu.
  STDMETHODIMP QueryContextMenu(HMENU menu, UINT indexMenu, UINT idCmdFirst,
                                UINT idCmdLast, UINT flags) override {
    if (!menu) return E_INVALIDARG;
    if (idCmdLast <= idCmdFirst) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    // CMF_DEFAULTONLY: the shell only wants the default verb; offering a
    // submenu here would hijack double-click.
    if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    model_ = prism::ContextMenuModel::Build(selected_, ShellTranslator());
    if (model_.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    HMENU popup = ::CreatePopupMenu();
    if (!popup) return E_OUTOFMEMORY;

    const UINT available = idCmdLast - idCmdFirst;  // exclusive upper bound
    UINT used = 0;
    UINT defaultOffset = static_cast<UINT>(-1);
    for (const prism::MenuCommand& command : model_) {
      if (used >= available) break;  // never steal an ID the shell reserved
      const std::wstring title = Utf8ToWide(command.title);
      ::InsertMenuW(popup, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                    idCmdFirst + used, title.c_str());
      if (command.isDefault) defaultOffset = used;
      ++used;
    }
    // Truncating the model would silently drop verbs the user can see in the
    // Win11 menu, so the popup is only inserted when the whole model fits.
    if (used != model_.size()) {
      ::DestroyMenu(popup);
      model_.clear();
      return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    }
    if (defaultOffset != static_cast<UINT>(-1)) {
      ::SetMenuDefaultItem(popup, idCmdFirst + defaultOffset, FALSE);
    }
    ::InsertMenuW(menu, indexMenu, MF_BYPOSITION | MF_POPUP,
                  reinterpret_cast<UINT_PTR>(popup), L"PrismArchive");
    // The menu is on screen from here on: wake the inline preview. This is the
    // ONLY thing C30 adds to this file -- no state, no window, no blocking call.
    PublishInlinePreview(selected_);
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, used);
  }

  STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
    if (!info) return E_INVALIDARG;
    if (model_.empty()) model_ = prism::ContextMenuModel::Build(selected_, ShellTranslator());
    if (model_.empty()) return E_UNEXPECTED;

    std::string verb;
    if (IS_INTRESOURCE(info->lpVerb)) {
      const size_t offset = static_cast<size_t>(reinterpret_cast<UINT_PTR>(info->lpVerb));
      if (offset >= model_.size()) return E_INVALIDARG;
      verb = model_[offset].verb;
    } else if (info->lpVerb) {
      // CMINVOKECOMMANDINFO::lpVerb is ANSI even in the W build (only
      // CMINVOKECOMMANDINFOEX carries lpVerbW). Our verb ids are ASCII, so the
      // narrow form is exactly right -- but it is matched against the model, so
      // an unknown string can never turn into a launch.
      const std::string requested(info->lpVerb);
      for (const prism::MenuCommand& command : model_) {
        if (command.verb == requested) {
          verb = command.verb;
          break;
        }
      }
      if (verb.empty()) return E_INVALIDARG;
    } else {
      return E_INVALIDARG;
    }

    std::vector<std::wstring> paths;
    paths.reserve(selected_.size());
    for (const std::string& path : selected_) paths.push_back(Utf8ToWide(path));
    if (!shell_launch::LaunchCompanion(verb, paths)) {
      return HRESULT_FROM_WIN32(::GetLastError());
    }
    return S_OK;
  }

  STDMETHODIMP GetCommandString(UINT_PTR idCmd, UINT type, UINT*, CHAR* buffer,
                                UINT count) override {
    if (model_.empty()) model_ = prism::ContextMenuModel::Build(selected_, ShellTranslator());
    const size_t offset = static_cast<size_t>(idCmd);
    if (type == GCS_VALIDATEA || type == GCS_VALIDATEW) {
      return offset < model_.size() ? S_OK : S_FALSE;
    }
    if (offset >= model_.size()) return E_INVALIDARG;
    if (type == GCS_VERBW) {
      if (!buffer || count == 0) return E_INVALIDARG;
      const std::wstring verb = VerbWide(model_[offset].verb);
      wchar_t* out = reinterpret_cast<wchar_t*>(buffer);
      const size_t room = count / sizeof(wchar_t);
      const size_t copy = (std::min)(verb.size(), room - 1);
      std::wmemcpy(out, verb.c_str(), copy);
      out[copy] = L'\0';
      return S_OK;
    }
    if (type == GCS_HELPTEXTW) {
      if (!buffer || count == 0) return E_INVALIDARG;
      const std::wstring help = Utf8ToWide(model_[offset].title);
      wchar_t* out = reinterpret_cast<wchar_t*>(buffer);
      const size_t room = count / sizeof(wchar_t);
      const size_t copy = (std::min)(help.size(), room - 1);
      std::wmemcpy(out, help.c_str(), copy);
      out[copy] = L'\0';
      return S_OK;
    }
    return E_NOTIMPL;  // ANSI variants: the shell must ask by ID, not by string
  }

 private:
  STGMEDIUM medium_{};
  std::vector<std::string> selected_;
  std::vector<prism::MenuCommand> model_;
};

class ClassicMenuFactory final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
 public:
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    ComPtr<ClassicMenu> menu;
    const HRESULT hr = MakeAndInitialize<ClassicMenu>(&menu);
    if (FAILED(hr)) return hr;
    // QI through the object, not a static_cast: the shell asks for
    // IContextMenu first and IShellExtInit second (contract B8/D10).
    return menu.CopyTo(iid, out);
  }
  STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};

}  // namespace prism::win::classic

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  if (rclsid != prism::win::classic::kClassicMenuClsid) return CLASS_E_CLASSNOTAVAILABLE;
  ComPtr<IClassFactory> factory;
  const HRESULT hr = MakeAndInitialize<prism::win::classic::ClassicMenuFactory>(&factory);
  if (FAILED(hr)) return hr;
  return factory.CopyTo(riid, ppv);
}

STDAPI DllCanUnloadNow() {
  return S_FALSE;  // WRL module lifetime: stay resident while Explorer hosts us
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  switch (reason) {
    case DLL_PROCESS_ATTACH:
    case DLL_PROCESS_DETACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
      break;
    default:
      break;
  }
  return TRUE;
}
