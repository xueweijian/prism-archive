// PrismArchive -- PrismWin
// Windows 11 modern context menu: IExplorerCommand + IEnumExplorerCommand
// (blueprint ch. 4.1 / ch. 6.3).
//
// This file is a VIEW. Every decision -- which verbs exist, their order, their
// default -- comes from prism::ContextMenuModel and is unit-tested in
// Tests/test_context_menu.cpp. The Win10 IContextMenu DLL
// (PrismClassicContextMenu.cpp) renders the same model.
//
// Deliberately NOT MFT_OWNERDRAW: Bandizip's ownerdraw menu is the reason it
// breaks on Win11 dark mode and mixed-DPI setups. We render the native
// cascading menu and pop a *separate* non-activating DirectComposition flyout
// (see PrismLiquidGlass, milestone M2) for the inline archive preview.
#include <shlwapi.h>   // SHStrDupW
#include <shobjidl_core.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <string>
#include <vector>

#include "../PrismCore/ContextMenuModel.hpp"
#include "PrismEngineBridge.hpp"  // WideToUtf8 / Utf8ToWide

using namespace Microsoft::WRL;

namespace prism::win::shell {

namespace {

constexpr wchar_t kPreviewWakeEvent[] = L"Local\\PrismLiquidPreviewWakeEvent";

// Blueprint ch. 4.1: the companion Liquid Glass flyout is woken through a named
// event, so explorer.exe never blocks on us.
void NotifyCompanionPreviewFlyout() noexcept {
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kPreviewWakeEvent);
  if (!event) return;
  SetEvent(event);
  CloseHandle(event);
}

void LaunchWorker(const std::wstring& args) noexcept {
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring cmd = L"PrismArchive.exe " + args;
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                     nullptr, nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

// Model verb -> companion CLI. The verb ids live in ContextMenuModel; this is
// the single place they become command lines.
std::wstring ArgsForVerb(const std::string& verb, const std::wstring& archive) {
  const std::wstring quoted = L"\"" + archive + L"\"";
  if (verb == "smart.extract.here") return L"--smart-extract " + quoted;
  if (verb == "extract.here") return L"--extract-here " + quoted;
  if (verb == "extract.to.subfolder") return L"--extract-sub " + quoted;
  if (verb == "test.integrity") return L"--test-integrity " + quoted;
  if (verb == "smart.extract.each.to.own.folder") return L"--smart-extract-each " + quoted;
  if (verb == "compress.to.7z") return L"--compress-7z " + quoted;
  if (verb == "compress.to.zip") return L"--compress-zip " + quoted;
  return quoted;  // open.in.prism
}

}  // namespace

namespace paths {

// IShellItemArray -> UTF-8 filesystem paths (PrismCore's contract).
std::vector<std::string> FromSelection(IShellItemArray* selection) {
  std::vector<std::string> out;
  if (!selection) return out;
  DWORD count = 0;
  if (FAILED(selection->GetCount(&count))) return out;
  for (DWORD i = 0; i < count; ++i) {
    ComPtr<IShellItem> item;
    if (FAILED(selection->GetItemAt(i, &item))) continue;
    PWSTR path = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
      out.push_back(WideToUtf8(path));
      CoTaskMemFree(path);
    }
  }
  return out;
}

}  // namespace paths

// One cascade entry. The model decides IF it exists; this class only renders
// and hands the verb to the companion process.
class SubCommand final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(const prism::MenuCommand& command,
                                 const std::wstring& archivePath) {
    verb_ = command.verb;
    title_ = Utf8ToWide(command.title);
    archive_ = archivePath;
    return S_OK;
  }

  IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* ppszName) override {
    return SHStrDupW(title_.c_str(), ppszName);
  }
  IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* ppszIcon) override {
    *ppszIcon = nullptr;
    return E_NOTIMPL;
  }
  IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* ppszInfotip) override {
    *ppszInfotip = nullptr;
    return E_NOTIMPL;
  }
  IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) override {
    *pguidCommandName = GUID_NULL;  // verb identity is carried by the CLI args
    return S_OK;
  }
  IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* pCmdState) override {
    *pCmdState = ECS_ENABLED;
    return S_OK;
  }
  IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
    LaunchWorker(ArgsForVerb(verb_, archive_));
    return S_OK;
  }
  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    *pFlags = ECF_DEFAULT;
    return S_OK;
  }
  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
    *ppEnum = nullptr;
    return E_NOTIMPL;
  }

 private:
  std::string verb_;
  std::wstring title_;
  std::wstring archive_;
};

// Renders prism::ContextMenuModel::Build(selection) as an Explorer enum.
class SubCommandEnumerator final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(const std::vector<std::string>& selectedPaths) {
    if (selectedPaths.empty()) return E_INVALIDARG;
    archive_ = Utf8ToWide(selectedPaths.front());
    const auto model = prism::ContextMenuModel::Build(selectedPaths);
    for (const auto& command : model) {
      ComPtr<SubCommand> cmd;
      if (SUCCEEDED(MakeAndInitialize<SubCommand>(&cmd, command, archive_))) {
        items_.push_back(cmd);
      }
    }
    return items_.empty() ? E_FAIL : S_OK;
  }

  IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** rgelt, ULONG* pceltFetched) override {
    if (!rgelt) return E_POINTER;
    ULONG fetched = 0;
    while (fetched < celt && pos_ < items_.size()) {
      items_[pos_++]->QueryInterface(IID_PPV_ARGS(&rgelt[fetched]));
      ++fetched;
    }
    if (pceltFetched) *pceltFetched = fetched;
    return fetched == celt ? S_OK : S_FALSE;
  }
  IFACEMETHODIMP Skip(ULONG celt) override {
    pos_ = (pos_ + celt > items_.size()) ? items_.size() : pos_ + celt;
    return S_OK;
  }
  IFACEMETHODIMP Reset() override {
    pos_ = 0;
    return S_OK;
  }
  IFACEMETHODIMP Clone(IEnumExplorerCommand** ppEnum) override {
    *ppEnum = nullptr;
    return E_NOTIMPL;
  }

 private:
  std::vector<ComPtr<SubCommand>> items_;
  std::wstring archive_;
  size_t pos_ = 0;
};

// The top-level entry that Windows 11 shows in the primary context menu.
// Requires package identity (MSIX sparse package) -- see AppxManifest.xml.
class PrismRootCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(std::wstring) { return S_OK; }  // selection comes per-query

  IFACEMETHODIMP GetTitle(IShellItemArray*, LPWSTR* ppszName) override {
    return SHStrDupW(L"PrismArchive", ppszName);
  }
  IFACEMETHODIMP GetIcon(IShellItemArray*, LPWSTR* ppszIcon) override {
    *ppszIcon = nullptr;
    return E_NOTIMPL;
  }
  IFACEMETHODIMP GetToolTip(IShellItemArray*, LPWSTR* ppszInfotip) override {
    *ppszInfotip = nullptr;
    return E_NOTIMPL;
  }
  IFACEMETHODIMP GetCanonicalName(GUID* pguidCommandName) override {
    *pguidCommandName = GUID_NULL;
    return S_OK;
  }

  // Windows queries GetState when the submenu opens -- that is our cue to wake
  // the Liquid Glass inline preview flyout (< 12 ms budget, blueprint ch. 7.2).
  IFACEMETHODIMP GetState(IShellItemArray* selection, BOOL, EXPCMDSTATE* pCmdState) override {
    *pCmdState = ECS_ENABLED;
    if (selection && !paths::FromSelection(selection).empty()) NotifyCompanionPreviewFlyout();
    return S_OK;
  }

  IFACEMETHODIMP Invoke(IShellItemArray* selection, IBindCtx*) override {
    const auto selected = paths::FromSelection(selection);
    if (selected.empty()) return E_INVALIDARG;
    LaunchWorker(L"\"" + Utf8ToWide(selected.front()) + L"\"");
    return S_OK;
  }

  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    *pFlags = ECF_HASSUBCOMMANDS;
    return S_OK;
  }

  // IExplorerCommand has no EnumSubCommands: subcommands are reached by
  // QueryInterface(IID_IEnumExplorerCommand) on the root command.
  IFACEMETHODIMP QueryInterfaceEnum(IShellItemArray* selection,
                                    IEnumExplorerCommand** ppEnum) {
    ComPtr<SubCommandEnumerator> enumerator;
    if (FAILED(MakeAndInitialize<SubCommandEnumerator>(&enumerator,
                                                      paths::FromSelection(selection)))) {
      return E_FAIL;
    }
    return enumerator->QueryInterface(IID_PPV_ARGS(ppEnum));
  }
};

// {6F6A5E2C-3E7B-4F8A-9C1D-0A1B2C3D4E5F}
constexpr CLSID CLSID_PrismShellCommand = {0x6F6A5E2C, 0x3E7B, 0x4F8A,
                                           {0x9C, 0x1D, 0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F}};

class PrismShellClassFactory final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
 public:
  IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    ComPtr<PrismRootCommand> command;
    HRESULT hr = MakeAndInitialize<PrismRootCommand>(&command, std::wstring());
    if (FAILED(hr)) return hr;
    return command->QueryInterface(riid, object);
  }
  IFACEMETHODIMP LockServer(BOOL lock) override {
    if (lock) {
      InterlockedIncrement(&g_lockCount);
    } else {
      InterlockedDecrement(&g_lockCount);
    }
    return S_OK;
  }

  static LONG g_lockCount;
};

LONG PrismShellClassFactory::g_lockCount = 0;

}  // namespace prism::win::shell

// COM module entry points.
//
// Written exactly the way the SDK declares them (STDAPI = EXTERN_C HRESULT
// __stdcall): a bare __declspec(dllexport) definition conflicts with that
// declaration (C2375 "different linkage"). Exports come from PrismShellExt.def.
STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  using namespace prism::win::shell;
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  if (rclsid != CLSID_PrismShellCommand) return CLASS_E_CLASSNOTAVAILABLE;
  ComPtr<PrismShellClassFactory> factory;
  HRESULT hr = MakeAndInitialize<PrismShellClassFactory>(&factory);
  if (FAILED(hr)) return hr;
  return factory->QueryInterface(riid, reinterpret_cast<void**>(ppv));
}

STDAPI DllCanUnloadNow() {
  return prism::win::shell::PrismShellClassFactory::g_lockCount == 0 ? S_OK : S_FALSE;
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
