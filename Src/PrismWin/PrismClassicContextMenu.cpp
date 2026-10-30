// PrismArchive -- PrismWin
// Win10 classic shell extension (blueprint ch. 4.1, M2 compat layer).
//
// Win11 uses IExplorerCommand + the MSIX sparse package (PrismShellCommand.cpp);
// Win10 still needs the old IContextMenu / IShellExtInit COM contract. Both
// render the SAME PrismCore ContextMenuModel -- this file is a view, not a brain.
//
// Compile-checked on windows-latest CI; full behaviour needs a shell host, so
// the logic it renders is unit-tested in PrismCore instead.
#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>  // RuntimeClass / ClassicCom / MakeAndInitialize

#include <string>
#include <vector>

#include "../PrismCore/ContextMenuModel.hpp"
#include "PrismEngineBridge.hpp"  // WideToUtf8 / Utf8ToWide

using namespace Microsoft::WRL;

namespace prism::win::classic {

// {8C1B7E52-1F3A-4B7E-9C41-PRISM0000C100} -- classic menu CLSID (independent of
// the Win11 handler's CLSID_PrismShellCommand).
constexpr CLSID CLSID_PrismClassicMenu = {
    0x8C1B7E52, 0x1F3A, 0x4B7E, {0x9C, 0x41, 0x5B, 0x3C, 0x00, 0x00, 0xC1, 0x00}};

class ClassicMenu final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IShellExtInit, IContextMenu> {
 public:
  // IShellExtInit: pull CF_HDROP out of the data object.
  STDMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* dataObject, HKEY) override {
    if (!dataObject) return E_INVALIDARG;
    FORMATETC fmt = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    if (FAILED(dataObject->GetData(&fmt, &medium_))) return E_FAIL;
    const UINT count = DragQueryFileW(static_cast<HDROP>(medium_.hGlobal), 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
      wchar_t path[MAX_PATH * 2]{};
      if (DragQueryFileW(static_cast<HDROP>(medium_.hGlobal), i, path, ARRAYSIZE(path)) > 0) {
        selected_.push_back(WideToUtf8(path));
      }
    }
    return selected_.empty() ? E_FAIL : S_OK;
  }

  // IContextMenu: render the PrismCore model into a popup submenu.
  STDMETHODIMP QueryContextMenu(HMENU menu, UINT indexMenu, UINT idCmdFirst, UINT idCmdLast,
                                UINT) override {
    if (idCmdLast <= idCmdFirst) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
    const auto model = prism::ContextMenuModel::Build(selected_);
    if (model.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);

    HMENU popup = CreatePopupMenu();
    UINT id = idCmdFirst;
    for (const auto& command : model) {
      const std::wstring title = Utf8ToWide(command.title);
      InsertMenuW(popup, static_cast<UINT>(-1), MF_BYPOSITION | MF_STRING,
                  id++, title.c_str());
    }
    InsertMenuW(menu, indexMenu++, MF_BYPOSITION | MF_POPUP,
                reinterpret_cast<UINT_PTR>(popup), L"PrismArchive");
    const UINT used = id - idCmdFirst;
    return MAKE_HRESULT(SEVERITY_SUCCESS, 0, used);
  }

  STDMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* info) override {
    if (!info || IS_INTRESOURCE(info->lpVerb) == FALSE) return E_FAIL;
    const auto model = prism::ContextMenuModel::Build(selected_);
    const size_t offset = reinterpret_cast<size_t>(info->lpVerb);
    if (offset >= model.size()) return E_INVALIDARG;
    // The verb model is shared with the Win11 handler; the actual worker is
    // launched by the same companion process both menus talk to.
    LaunchWorker(model[offset].verb);
    return S_OK;
  }

  STDMETHODIMP GetCommandString(UINT_PTR, UINT type, UINT*, CHAR* buffer, UINT count) override {
    if (type != GCS_VERBW) return E_NOTIMPL;
    return E_NOTIMPL;  // verbs are exposed by the sparse-package manifest on Win11
  }

 private:
  // Hand the verb + selection to the companion process; the shell DLL never
  // links the extraction engine directly (process isolation, blueprint ch. 7).
  void LaunchWorker(const std::string& verb) {
    std::wstring args = Utf8ToWide("--shell-verb " + verb);
    for (const std::string& path : selected_) {
      args += L" \"" + Utf8ToWide(path) + L"\"";
    }
    // Resolve OUR module path (the DLL), not the host's (explorer.exe).
    // A free function inside this .obj is the address anchor -- member
    // function pointers do not convert to LPCWSTR.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleAnchor), &self);
    wchar_t exe[MAX_PATH * 2]{};
    GetModuleFileNameW(self, exe, ARRAYSIZE(exe));
    std::wstring cmdline = std::wstring(L"\"") + exe + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, 0, nullptr,
                       nullptr, &si, &pi)) {
      CloseHandle(pi.hProcess);
      CloseHandle(pi.hThread);
    }
  }

  static STGMEDIUM medium_;
  std::vector<std::string> selected_;
};

STGMEDIUM ClassicMenu::medium_{};

namespace {
// Address anchor for GetModuleHandleExW(FROM_ADDRESS): member function
// pointers cannot be reinterpreted as LPCWSTR, free function pointers can.
void ModuleAnchor() {}
}  // namespace

// Called by the module entry points below. The static_cast chain is required:
// ClassicMenu inherits two IUnknown paths, so an implicit conversion to
// IUnknown* would be ambiguous.
HRESULT CreateClassicMenuInstance(IUnknown** out) {
  if (!out) return E_POINTER;
  ComPtr<ClassicMenu> menu;
  const HRESULT hr = MakeAndInitialize<ClassicMenu>(&menu);
  if (FAILED(hr)) return hr;
  *out = static_cast<IShellExtInit*>(menu.Detach());
  return S_OK;
}

}  // namespace prism::win::classic


// ---------------------------------------------------------------------------
// COM module entry points (same STDAPI discipline as PrismShellCommand.cpp).
// ---------------------------------------------------------------------------
namespace prism::win::classic {
class ClassicMenuFactory final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IClassFactory> {
 public:
  STDMETHODIMP CreateInstance(IUnknown* outer, REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    ComPtr<IUnknown> instance;
    const HRESULT hr = CreateClassicMenuInstance(&instance);
    if (FAILED(hr)) return hr;
    return instance.CopyTo(iid, out);
  }
  STDMETHODIMP LockServer(BOOL) override { return S_OK; }
};
}  // namespace prism::win::classic

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv) {
  if (!ppv) return E_POINTER;
  *ppv = nullptr;
  if (rclsid != prism::win::classic::CLSID_PrismClassicMenu) return CLASS_E_CLASSNOTAVAILABLE;
  ComPtr<IClassFactory> factory;
  const HRESULT hr =
      MakeAndInitialize<prism::win::classic::ClassicMenuFactory>(&factory);
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
