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

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "../PrismCore/DragDrop.hpp"  // SeekPolicy / PlanSeek / DelayedSeekCursor

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
//
// M4 seek completion: Explorer treats the handed-out IStream as seekable and
// seeks freely (its copy engine probes the tail; format sniffers re-read from
// 0 after a prefix). The stream delegates every decision to
// prism::dnd::DelayedSeekCursor -- the pure state machine that the Linux suite
// exercises with 115 assertions (byte-exactness across SET/CUR/END seeks, ring
// residency, restart budgets, refusal semantics). This class is only the COM
// shell: it resolves the seek origin, forwards bytes, and maps a refusal to the
// HRESULT Explorer expects. That split exists because the Windows TU cannot be
// compiled locally (CONTRACTS D8) while the logic underneath already is.
//
// All the solid-decoder facts the cursor encodes come from 7-Zip itself:
// 7zExtract.cpp:325 rewinds to FolderStartFileIndex and decodes forward, and
// 7zFolderInStream's GetSubStreamSize only ever answers about the stream it is
// currently reading -- there is no random-access promise to inherit, so an
// honest restart (on the SAME open session) is the only correct answer.
class DelayedExtractStream final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IStream> {
 public:
  // Restartable pull-producer over one sequential decoder (the solid block).
  class Producer {
   public:
    virtual ~Producer() = default;
    // Next sequential chunk of the decoder frontier (<= maxBytes). An empty
    // return means the decoder ended or failed before the declared stream
    // size; the stream latches that loudly instead of faking a short file.
    virtual std::string Next(size_t maxBytes) = 0;
    // Rewind the decoder to the stream start, KEEPING the underlying session.
    // false = the stream refuses further service (failing loudly) rather than
    // ever serving bytes from an undefined decoder state.
    virtual bool Restart() = 0;
  };
  using ProducerPtr = std::shared_ptr<Producer>;

  HRESULT RuntimeClassInitialize(ProducerPtr producer, uint64_t totalSize,
                                 prism::dnd::SeekPolicy policy = {}) {
    if (!producer) return E_INVALIDARG;
    producer_ = std::move(producer);
    totalSize_ = totalSize;
    cursor_ = prism::dnd::DelayedSeekCursor(
        totalSize,
        [this](size_t n) { return producer_ ? producer_->Next(n) : std::string(); },
        [this] { return producer_ ? producer_->Restart() : false; }, policy);
    return S_OK;
  }

  IFACEMETHODIMP Read(void* buffer, ULONG count, ULONG* read) override {
    if (!buffer) return E_POINTER;
    if (read) *read = 0;
    if (count == 0) return S_FALSE;
    const auto outcome = cursor_.Read(static_cast<uint8_t*>(buffer), count);
    if (read) *read = static_cast<ULONG>(outcome.served);
    if (outcome.failed) return STG_E_READFAULT;
    if (outcome.served == 0 && outcome.eof) return S_FALSE;
    return S_OK;
  }
  IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
    if (cursor_.failed()) return STG_E_READFAULT;
    const int64_t current = static_cast<int64_t>(cursor_.position());
    int64_t target = 0;
    if (origin == STREAM_SEEK_SET) {
      target = move.QuadPart;
    } else if (origin == STREAM_SEEK_CUR) {
      target = current + move.QuadPart;
    } else if (origin == STREAM_SEEK_END) {
      target = static_cast<int64_t>(totalSize_) + move.QuadPart;
    } else {
      return STG_E_INVALIDFUNCTION;
    }
    // A refused plan (range, restart count, or re-decode byte budget) leaves
    // the cursor where it was and says why in diagnostics(); STG_E_INVALIDFUNCTION
    // is the documented error for a seek the stream cannot honour.
    if (!cursor_.Seek(target, nullptr)) return STG_E_INVALIDFUNCTION;
    if (newPos) newPos->QuadPart = cursor_.position();
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
    *stat = STATSTG{};
    stat->type = STGTY_STREAM;
    // cbSize must be filled regardless of STATFLAG_NONAME: the flag only
    // suppresses the name, and receivers (Explorer's copy engine) size their
    // progress UI from it. The old skeleton returned before setting cbSize,
    // so every dragged file looked zero-length until it was actually read.
    stat->cbSize.QuadPart = totalSize_;
    if (flags != STATFLAG_NONAME) {
      // Name omitted on purpose: the virtual name lives in the
      // FILEDESCRIPTORW, not in the stream, and a null pwcsName is legal.
    }
    return S_OK;
  }
  IFACEMETHODIMP Clone(IStream** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }

  // Diagnostics (numbers and planner reasons only -- no paths, no names).
  uint64_t restarts() const { return cursor_.restarts(); }
  uint64_t restartBytesUsed() const { return cursor_.restartBytesUsed(); }
  uint64_t wastedBytes() const { return cursor_.wastedBytes(); }
  uint64_t skippedBytes() const { return cursor_.skippedBytes(); }
  uint64_t position() const { return cursor_.position(); }
  const prism::dnd::DelayedSeekCursor& cursor() const { return cursor_; }
  const std::string& diagnostics() const { return cursor_.diagnostics(); }

 private:
  ProducerPtr producer_;
  uint64_t totalSize_ = 0;
  prism::dnd::DelayedSeekCursor cursor_;
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
