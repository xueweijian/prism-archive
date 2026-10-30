// PrismArchive -- PrismWin
// Zero-temp-copy drag & drop out of an archive (blueprint ch. 4.4 / ch. 8.1).
//
// Channel A: the drop lands on a real Explorer window -> we sniff the target
// folder through IShellWindows and stream 7z.dll output directly onto it, so a
// 4 GB file never touches %TEMP% (the original 7-Zip behaviour writes it twice).
// Channel B: the target is an application (WeChat, Photoshop, browser upload) ->
// CFSTR_FILEDESCRIPTORW + CFSTR_FILECONTENTS with a lazy IStream that pulls
// from the decompressor on demand.
#pragma once

#include <exdisp.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <windows.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <filesystem>
#include <functional>
#include <string>

using namespace Microsoft::WRL;

namespace prism::win::dragdrop {

// Reverse-resolves the physical folder shown by the Explorer window under the
// cursor (IShellWindows -> IWebBrowserApp -> IShellBrowser -> IFolderView).
class ExplorerDropTargetResolver {
 public:
  static std::wstring TryResolveExplorerPathAtCursor(POINT screenPt) noexcept {
    HWND hwndUnderCursor = WindowFromPoint(screenPt);
    if (!hwndUnderCursor) return {};
    HWND hwndTopLevel = GetAncestor(hwndUnderCursor, GA_ROOT);
    if (!hwndTopLevel) return {};

    wchar_t className[64]{};
    GetClassNameW(hwndTopLevel, className, 64);
    if (wcscmp(className, L"Progman") == 0 || wcscmp(className, L"WorkerW") == 0) {
      PWSTR desktop = nullptr;
      if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)) && desktop) {
        std::wstring result(desktop);
        CoTaskMemFree(desktop);
        return result;
      }
    }

    ComPtr<IShellWindows> shellWindows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_LOCAL_SERVER,
                                IID_PPV_ARGS(&shellWindows)))) {
      return {};
    }
    long count = 0;
    shellWindows->get_Count(&count);
    for (long i = 0; i < count; ++i) {
      VARIANT index{};
      index.vt = VT_I4;
      index.lVal = i;
      ComPtr<IDispatch> dispatch;
      if (FAILED(shellWindows->Item(index, &dispatch)) || !dispatch) continue;
      ComPtr<IWebBrowserApp> browser;
      if (FAILED(dispatch.As(&browser))) continue;
      SHANDLE_PTR hwndBrowser = 0;
      if (FAILED(browser->get_HWND(&hwndBrowser))) continue;
      if (reinterpret_cast<HWND>(hwndBrowser) != hwndTopLevel) continue;

      ComPtr<IServiceProvider> services;
      if (FAILED(browser.As(&services))) continue;
      ComPtr<IShellBrowser> shellBrowser;
      if (FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shellBrowser))))
        continue;
      ComPtr<IShellView> shellView;
      if (FAILED(shellBrowser->QueryActiveShellView(&shellView))) continue;
      ComPtr<IFolderView> folderView;
      if (FAILED(shellView.As(&folderView))) continue;
      ComPtr<IPersistFolder2> persist;
      if (FAILED(folderView->GetFolder(IID_PPV_ARGS(&persist)))) continue;
      PIDLIST_ABSOLUTE pidl = nullptr;
      if (SUCCEEDED(persist->GetCurFolder(&pidl)) && pidl) {
        wchar_t path[MAX_PATH * 4]{};
        if (SHGetPathFromIDListEx(pidl, path, ARRAYSIZE(path), GPFIDL_DEFAULT)) {
          CoTaskMemFree(pidl);
          return std::wstring(path);
        }
        CoTaskMemFree(pidl);
      }
    }
    return {};
  }
};

// Channel B: delayed-render virtual file stream. The decompressor only runs
// when the drop target actually asks for bytes.
class DelayedExtractStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IStream> {
 public:
  // producer(bytesRequested) returns the next decompressed chunk.
  using Producer = std::function<std::string(size_t)>;

  HRESULT RuntimeClassInitialize(Producer producer, uint64_t totalSize) {
    producer_ = std::move(producer);
    totalSize_ = totalSize;
    return S_OK;
  }

  IFACEMETHODIMP Read(void* buffer, ULONG count, ULONG* read) override {
    if (!buffer) return E_POINTER;
    if (read) *read = 0;
    if (position_ >= totalSize_ || count == 0) return S_FALSE;
    const std::string chunk = producer_ ? producer_(count) : std::string();
    if (chunk.empty()) return S_FALSE;
    const size_t bytes = min(static_cast<size_t>(count), chunk.size());
    memcpy(buffer, chunk.data(), bytes);
    position_ += bytes;
    if (read) *read = static_cast<ULONG>(bytes);
    return S_OK;
  }
  IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
    uint64_t target = position_;
    if (origin == STREAM_SEEK_SET) {
      target = static_cast<uint64_t>(move.QuadPart);
    } else if (origin == STREAM_SEEK_CUR) {
      target = position_ + static_cast<uint64_t>(move.QuadPart);
    } else if (origin == STREAM_SEEK_END) {
      target = totalSize_ + static_cast<uint64_t>(move.QuadPart);
    }
    position_ = target;
    if (newPos) newPos->QuadPart = position_;
    return S_OK;
  }
  IFACEMETHODIMP SetSize(ULARGE_INTEGER) override { return E_NOTIMPL; }
  IFACEMETHODIMP CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override {
    return E_NOTIMPL;
  }
  IFACEMETHODIMP Commit(DWORD) override { return S_OK; }
  IFACEMETHODIMP Revert() override { return S_OK; }
  IFACEMETHODIMP LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  IFACEMETHODIMP UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return E_NOTIMPL; }
  IFACEMETHODIMP Stat(STATSTG* stat, DWORD flags) override {
    if (!stat) return E_POINTER;
    if (flags == STATFLAG_NONAME) return S_OK;
    stat->cbSize.QuadPart = totalSize_;
    return S_OK;
  }
  IFACEMETHODIMP Clone(IStream** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }

 private:
  Producer producer_;
  uint64_t totalSize_ = 0;
  uint64_t position_ = 0;
};

// Channel A drop source: on mouse-up we try to short-circuit the Shell copy and
// stream straight into the resolved folder.
class ZeroCopyDropSource final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDropSource> {
 public:
  using DirectDropFn = std::function<bool(const std::wstring& targetDir)>;

  HRESULT RuntimeClassInitialize(DirectDropFn onDirectDrop) {
    onDirectDrop_ = std::move(onDirectDrop);
    return S_OK;
  }

  IFACEMETHODIMP QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
    if (escapePressed || (keyState & MK_RBUTTON)) return DRAGDROP_S_CANCEL;
    if (!(keyState & MK_LBUTTON)) {
      POINT cursor{};
      GetCursorPos(&cursor);
      const std::wstring target =
          ExplorerDropTargetResolver::TryResolveExplorerPathAtCursor(cursor);
      if (!target.empty() && std::filesystem::is_directory(target) && onDirectDrop_ &&
          onDirectDrop_(target)) {
        // PrismArchive owns the write now: cancel the Shell's copy protocol.
        return DRAGDROP_S_CANCEL;
      }
      return DRAGDROP_S_DROP;  // fall back to the virtual IStream channel
    }
    return S_OK;
  }

  IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }

 private:
  DirectDropFn onDirectDrop_;
};

}  // namespace prism::win::dragdrop
