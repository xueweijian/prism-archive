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
//
// Package identity (not registry) is what puts us in the Win11 menu: the CLSID
// below must equal com:Class/@Id and desktop5:Verb/@Clsid in
// Assets/Package/AppxManifest.xml -- both sides read the constant from
// SparsePackageRegistrar.hpp, and Tests/win/test_sparse_registrar.cpp asserts
// the manifest text against it.
#include <shlwapi.h>  // SHStrDupW
#include <shobjidl_core.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "../PrismCore/ContextMenuModel.hpp"
#include "PrismEngineBridge.hpp"      // WideToUtf8 / Utf8ToWide
#include "SparsePackageRegistrar.hpp"  // CLSID + verb table + launcher

using namespace Microsoft::WRL;

namespace prism::win::shell {

namespace {

constexpr wchar_t kPreviewWakeEvent[] = L"Local\\PrismLiquidPreviewWakeEvent";
constexpr wchar_t kPreviewTargetMapping[] = L"Local\\PrismLiquidPreviewTarget";

// Blueprint ch. 4.1: the companion Liquid Glass flyout is woken through a named
// event, so explorer.exe never blocks on us.
void NotifyCompanionPreviewFlyout(const std::string& path) noexcept {
  if (path.empty()) return;
  const std::wstring wide = Utf8ToWide(path);
  const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
  HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(bytes), kPreviewTargetMapping);
  if (mapping) {
    if (void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, bytes)) {
      std::memcpy(view, wide.c_str(), bytes);
      UnmapViewOfFile(view);
    }
    CloseHandle(mapping);
  }
  HANDLE event = OpenEventW(EVENT_MODIFY_STATE, FALSE, kPreviewWakeEvent);
  if (!event) return;
  SetEvent(event);
  CloseHandle(event);
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
                                 std::vector<std::wstring> selection) {
    verb_ = command.verb;
    title_ = Utf8ToWide(command.title);
    selection_ = std::move(selection);
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
    if (!pCmdState) return E_POINTER;
    *pCmdState = ECS_ENABLED;
    return S_OK;
  }
  IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
    if (!shell_launch::LaunchCompanion(verb_, selection_)) {
      return HRESULT_FROM_WIN32(::GetLastError());
    }
    return S_OK;
  }
  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    *pFlags = ECF_DEFAULT;
    return S_OK;
  }
  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;
    return E_NOTIMPL;
  }

 private:
  std::string verb_;
  std::wstring title_;
  std::vector<std::wstring> selection_;
};

// Renders prism::ContextMenuModel::Build(selection) as an Explorer enum.
class SubCommandEnumerator final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
 public:
  // Takes the already-decided model (GetState cached it); this class only
  // renders. Keeping the decision out of the renderer is what lets the Win10
  // layer and the in-app menu show byte-identical commands.
  HRESULT RuntimeClassInitialize(const std::vector<prism::MenuCommand>& model,
                                 std::vector<std::wstring> selection) {
    if (model.empty() || selection.empty()) return E_INVALIDARG;
    model_ = model;
    selection_ = std::move(selection);
    for (const auto& command : model) {
      ComPtr<SubCommand> cmd;
      if (SUCCEEDED(MakeAndInitialize<SubCommand>(&cmd, command, selection_))) {
        items_.push_back(cmd);
      }
    }
    return items_.empty() ? E_FAIL : S_OK;
  }

  IFACEMETHODIMP Next(ULONG celt, IExplorerCommand** rgelt, ULONG* pceltFetched) override {
    if (!rgelt) return E_POINTER;
    if (pceltFetched) *pceltFetched = 0;
    ULONG fetched = 0;
    while (fetched < celt && pos_ < items_.size()) {
      // A QI failure must not leave a null slot behind: the caller walks
      // exactly "fetched" entries and would dereference it.
      if (FAILED(items_[pos_]->QueryInterface(IID_PPV_ARGS(&rgelt[fetched])))) break;
      ++pos_;
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
  // Explorer clones enumerators when a submenu is reopened. E_NOTIMPL made the
  // second open render nothing at all.
  IFACEMETHODIMP Clone(IEnumExplorerCommand** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;
    ComPtr<SubCommandEnumerator> clone;
    HRESULT hr = MakeAndInitialize<SubCommandEnumerator>(&clone, model_, selection_);
    if (FAILED(hr)) return hr;
    hr = clone->Skip(static_cast<ULONG>(pos_));
    if (FAILED(hr)) return hr;
    return clone.CopyTo(IID_PPV_ARGS(ppEnum));
  }

 private:
  std::vector<ComPtr<SubCommand>> items_;
  std::vector<prism::MenuCommand> model_;
  std::vector<std::wstring> selection_;
  size_t pos_ = 0;
};

// The top-level entry that Windows 11 shows in the primary context menu.
// Requires package identity (MSIX sparse package) -- see AppxManifest.xml.
class PrismRootCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(std::wstring) { return S_OK; }  // selection per query

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

  // Windows queries GetState while building the menu -- that is our cue to wake
  // the Liquid Glass inline preview flyout (< 12 ms budget, blueprint ch. 7.2).
  IFACEMETHODIMP GetState(IShellItemArray* selection, BOOL, EXPCMDSTATE* pCmdState) override {
    if (!pCmdState) return E_POINTER;
    *pCmdState = ECS_ENABLED;
    const auto selected = paths::FromSelection(selection);
    cachedCommands_ = prism::ContextMenuModel::Build(selected);
    selection_.clear();
    selection_.reserve(selected.size());
    for (const std::string& path : selected) selection_.push_back(Utf8ToWide(path));
    if (!selected.empty()) NotifyCompanionPreviewFlyout(selected.front());
    // An empty model means "nothing here" -- hide the command entirely.
    if (cachedCommands_.empty()) *pCmdState = ECS_HIDDEN;
    return S_OK;
  }

  IFACEMETHODIMP Invoke(IShellItemArray* selection, IBindCtx*) override {
    const auto selected = paths::FromSelection(selection);
    if (selected.empty()) return E_INVALIDARG;
    std::vector<std::wstring> paths;
    paths.reserve(selected.size());
    for (const std::string& path : selected) paths.push_back(Utf8ToWide(path));
    if (!shell_launch::LaunchCompanion("open.in.prism", paths)) {
      return HRESULT_FROM_WIN32(::GetLastError());
    }
    return S_OK;
  }

  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    if (!pFlags) return E_POINTER;
    *pFlags = ECF_HASSUBCOMMANDS;
    return S_OK;
  }

  // IExplorerCommand::EnumSubCommands takes no selection: Windows passes the
  // items to GetState() first, so that is where we build and cache the model
  // for the invocation. EnumSubCommands then renders the cached decision.
  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;
    if (cachedCommands_.empty() || selection_.empty()) return E_NOTIMPL;
    ComPtr<SubCommandEnumerator> enumerator;
    if (FAILED(MakeAndInitialize<SubCommandEnumerator>(&enumerator, cachedCommands_,
                                                      selection_))) {
      return E_FAIL;
    }
    return enumerator->QueryInterface(IID_PPV_ARGS(ppEnum));
  }

 private:
  std::vector<prism::MenuCommand> cachedCommands_;
  std::vector<std::wstring> selection_;
};

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
  if (rclsid != prism::win::shell_ids::kExplorerCommandClsid) {
    return CLASS_E_CLASSNOTAVAILABLE;
  }
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
