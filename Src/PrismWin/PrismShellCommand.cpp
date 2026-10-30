// PrismArchive -- PrismWin
// Windows 11 modern context menu: IExplorerCommand + IEnumExplorerCommand
// (blueprint ch. 4.1 / ch. 6.3).
//
// Deliberately NOT MFT_OWNERDRAW: Bandizip's ownerdraw menu is the reason it
// breaks on Win11 dark mode and mixed-DPI setups. We render the native
// cascading menu and pop a *separate* non-activating DirectComposition flyout
// (see PrismLiquidGlass, milestone M2) for the inline archive preview.
#include <shobjidl_core.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <string>
#include <vector>

using namespace Microsoft::WRL;

namespace prism::win::shell {

namespace {

constexpr wchar_t kPreviewWakeEvent[] = L"Local\\PrismLiquidPreviewWakeEvent";

// Blueprint ch. 4.1: the companion Liquid Glass flyout is woken through shared
// memory + a named event, so explorer.exe never blocks on us.
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
  if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &si, &pi)) {
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
}

}  // namespace

enum class Verb {
  SmartExtractHere,
  ExtractHere,
  ExtractToSubfolder,
  ExtractToCustomPath,
  TestIntegrity,
  OpenInPrism
};

class SubCommand final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(Verb verb, std::wstring title, std::wstring archivePath,
                                 bool separatorAfter = false) {
    verb_ = verb;
    title_ = std::move(title);
    archive_ = std::move(archivePath);
    separatorAfter_ = separatorAfter;
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
    *pguidCommandName = GUID_NULL;
    return S_OK;
  }
  IFACEMETHODIMP GetState(IShellItemArray*, BOOL, EXPCMDSTATE* pCmdState) override {
    *pCmdState = ECS_ENABLED;
    return S_OK;
  }
  IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
    std::wstring args = L"\"" + archive_ + L"\"";
    switch (verb_) {
      case Verb::SmartExtractHere: args = L"--smart-extract " + args; break;
      case Verb::ExtractHere: args = L"--extract-here " + args; break;
      case Verb::ExtractToSubfolder: args = L"--extract-sub " + args; break;
      case Verb::ExtractToCustomPath: args = L"--extract-to " + args; break;
      case Verb::TestIntegrity: args = L"--test-integrity " + args; break;
      case Verb::OpenInPrism: break;
    }
    LaunchWorker(args);
    return S_OK;
  }
  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    *pFlags = separatorAfter_ ? (ECF_DEFAULT | ECF_SEPARATORAFTER) : ECF_DEFAULT;
    return S_OK;
  }
  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
    *ppEnum = nullptr;
    return E_NOTIMPL;
  }

 private:
  Verb verb_ = Verb::OpenInPrism;
  std::wstring title_;
  std::wstring archive_;
  bool separatorAfter_ = false;
};

class SubCommandEnumerator final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IEnumExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(const std::wstring& archivePath) {
    const std::pair<Verb, const wchar_t*> defs[] = {
        {Verb::SmartExtractHere, L"Smart extract here"},
        {Verb::ExtractHere, L"Extract here"},
        {Verb::ExtractToSubfolder, L"Extract to \"<archive>\""},
        {Verb::ExtractToCustomPath, L"Extract to..."},
        {Verb::TestIntegrity, L"Test archive integrity"}};
    for (const auto& def : defs) {
      ComPtr<SubCommand> cmd;
      if (SUCCEEDED(MakeAndInitialize<SubCommand>(&cmd, def.first, std::wstring(def.second),
                                                  archivePath))) {
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
  size_t pos_ = 0;
};

// The top-level entry that Windows 11 shows in the primary context menu.
// Requires package identity (MSIX sparse package) -- see AppxManifest.xml.
class PrismRootCommand final
    : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IExplorerCommand> {
 public:
  HRESULT RuntimeClassInitialize(std::wstring archivePath) {
    archive_ = std::move(archivePath);
    return S_OK;
  }

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
    if (selection) NotifyCompanionPreviewFlyout();
    return S_OK;
  }

  IFACEMETHODIMP Invoke(IShellItemArray*, IBindCtx*) override {
    LaunchWorker(L"\"" + archive_ + L"\"");
    return S_OK;
  }

  IFACEMETHODIMP GetFlags(EXPCMDFLAGS* pFlags) override {
    *pFlags = ECF_HASSUBCOMMANDS;
    return S_OK;
  }

  IFACEMETHODIMP EnumSubCommands(IEnumExplorerCommand** ppEnum) override {
    if (!ppEnum) return E_POINTER;
    ComPtr<SubCommandEnumerator> enumerator;
    if (FAILED(MakeAndInitialize<SubCommandEnumerator>(&enumerator, archive_))) return E_FAIL;
    return enumerator->QueryInterface(IID_PPV_ARGS(ppEnum));
  }

 private:
  std::wstring archive_;
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

extern "C" {

__declspec(dllexport) HRESULT WINAPI DllGetClassObject(REFCLSID rclsid, REFIID riid,
                                                       LPVOID* ppv) {
  using namespace prism::win::shell;
  if (ppv == nullptr) return E_POINTER;
  *ppv = nullptr;
  if (rclsid != CLSID_PrismShellCommand) return CLASS_E_CLASSNOTAVAILABLE;
  ComPtr<PrismShellClassFactory> factory;
  HRESULT hr = MakeAndInitialize<PrismShellClassFactory>(&factory);
  if (FAILED(hr)) return hr;
  return factory->QueryInterface(riid, ppv);
}

__declspec(dllexport) HRESULT WINAPI DllCanUnloadNow() {
  return prism::win::shell::PrismShellClassFactory::g_lockCount == 0 ? S_OK : S_FALSE;
}

__declspec(dllexport) BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
  return reason == DLL_PROCESS_ATTACH || reason == DLL_PROCESS_DETACH ? TRUE : TRUE;
}

}  // extern "C"
