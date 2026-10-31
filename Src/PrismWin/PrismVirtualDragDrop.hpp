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

#include "../../Src/PrismCore/DragDrop.hpp"

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
// M4 seek completion: Explorer treats the handed-out IStream as seekable. The
// stream plans every seek through prism::dnd::PlanSeek and serves it three
// ways -- CursorOnly (lazy skip), LookbackHit (recent-bytes ring), or an
// honest RestartDecode that rewinds the decoder and re-decodes the solid
// prefix. A restart KEEPS the 7z.dll session: the producer re-runs Extract on
// the same IInArchive instead of re-opening the file. Every restart and every
// wasted byte lands in the diagnostics string; the stream never reports a
// position it did not actually reach.
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

  // Decoder pull granularity. Small enough that the cursor never trails the
  // frontier by more than one chunk, so the lookback ring (default 4 MiB)
  // always covers everything between the cursor and the frontier.
  static constexpr size_t kPullChunkBytes = 64 * 1024;

  HRESULT RuntimeClassInitialize(ProducerPtr producer, uint64_t totalSize,
                                 prism::dnd::SeekPolicy policy = {}) {
    if (!producer) return E_INVALIDARG;
    producer_ = std::move(producer);
    totalSize_ = totalSize;
    policy_ = policy;
    policy_.restartsUsed = 0;  // the stream owns this counter; seek refreshes it
    return S_OK;
  }

  IFACEMETHODIMP Read(void* buffer, ULONG count, ULONG* read) override {
    if (!buffer) return E_POINTER;
    if (read) *read = 0;
    if (failed_) return STG_E_READFAULT;
    if (count == 0 || position_ >= totalSize_) return S_FALSE;
    auto* out = static_cast<uint8_t*>(buffer);
    size_t served = 0;
    // (1) Serve what the cursor still covers from the recent-bytes ring.
    // Invariant between calls: position_ >= ringStart_. A violation means a
    // seek mis-planned; fail loudly rather than serve shifted bytes.
    while (served < count && position_ < frontier_) {
      if (position_ < ringStart_) {
        LatchFailure("lookback ring gap at offset " + std::to_string(position_));
        break;
      }
      const size_t offset = static_cast<size_t>(position_ - ringStart_);
      const size_t avail = static_cast<size_t>(frontier_ - position_);
      const size_t n = avail < (count - served) ? avail : (count - served);
      memcpy(out + served, ring_.data() + offset, n);
      served += n;
      position_ += n;
    }
    // (2) Align the decoder: a previous seek may have parked the cursor ahead
    // of the frontier (forward skip) or rewound the decoder to zero (restart
    // prefix). Drop exactly (position_ - frontier_) decoded bytes -- recomputed
    // from absolute positions every read, so no seek can desynchronise it.
    int64_t toDrop = static_cast<int64_t>(position_) - static_cast<int64_t>(frontier_);
    while (served < count && !failed_ && toDrop > 0) {
      const size_t want =
          static_cast<size_t>(toDrop) < kPullChunkBytes ? static_cast<size_t>(toDrop) : kPullChunkBytes;
      const std::string chunk = producer_->Next(want);
      if (chunk.empty()) {
        LatchFailure("decoder ended while skipping to offset " + std::to_string(position_));
        break;
      }
      frontier_ += chunk.size();
      if (droppingForRestart_) {
        wastedBytes_ += chunk.size();  // re-decoded purely to reach the target
      } else {
        skippedBytes_ += chunk.size();  // forward-skip cost of a solid block
      }
      toDrop -= static_cast<int64_t>(chunk.size());
    }
    if (toDrop <= 0) droppingForRestart_ = false;
    // (3) Pull fresh bytes and keep a copy in the ring for small back-steps.
    while (served < count && !failed_ && position_ < totalSize_) {
      const size_t want = (count - served) < kPullChunkBytes ? (count - served) : kPullChunkBytes;
      const std::string chunk = producer_->Next(want);
      if (chunk.empty()) {
        LatchFailure("decoder ended early at frontier " + std::to_string(frontier_));
        break;
      }
      AppendRing(chunk);
      frontier_ += chunk.size();
      const size_t n = chunk.size() < (count - served) ? chunk.size() : (count - served);
      memcpy(out + served, chunk.data(), n);
      served += n;
      position_ += n;
    }
    if (read) *read = static_cast<ULONG>(served);
    if (failed_) return STG_E_READFAULT;
    if (served == 0 && position_ >= totalSize_) return S_FALSE;
    return S_OK;
  }
  IFACEMETHODIMP Write(const void*, ULONG, ULONG*) override { return STG_E_ACCESSDENIED; }
  IFACEMETHODIMP Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* newPos) override {
    if (failed_) return STG_E_READFAULT;
    const int64_t current = static_cast<int64_t>(position_);
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
    policy_.restartsUsed = restarts_;
    const prism::dnd::SeekPlan plan =
        prism::dnd::PlanSeek(current, target, static_cast<int64_t>(totalSize_), policy_);
    if (plan.refused) {
      // Budget refusals surface here with the planner's reason (which names
      // the right-click-extract escape hatch); the cursor stays untouched.
      diagnostics_ += plan.reason + "; ";
      return STG_E_INVALIDFUNCTION;
    }
    switch (plan.kind) {
      case prism::dnd::SeekPlan::CursorOnly:
        // No decoder action now: Read() lazily drops (position_ - frontier_)
        // bytes on the next demand, so seeking across an unexplored solid
        // block stays O(1) until bytes are actually pulled.
        position_ = static_cast<uint64_t>(target);
        break;
      case prism::dnd::SeekPlan::LookbackHit:
        if (target >= static_cast<int64_t>(ringStart_) &&
            target <= static_cast<int64_t>(frontier_)) {
          position_ = static_cast<uint64_t>(target);  // served from the ring
        } else {
          // The planner's window is cursor-relative, the ring is
          // frontier-relative; when the planned window is not resident
          // (back-step after a back-step), pay the honest restart instead of
          // guessing at bytes that left the ring. The fallback is still a
          // restart: it must honour the budget, never over-spend silently.
          policy_.restartsUsed = restarts_;
          if (policy_.restartsUsed >= policy_.maxRestarts) {
            diagnostics_ +=
                "seek refused: lookback window not resident and the solid-decoder "
                "restart budget is exhausted -- extract this file with the "
                "right-click menu instead; ";
            return STG_E_INVALIDFUNCTION;
          }
          if (!DoRestartDecode()) return STG_E_READFAULT;
          position_ = static_cast<uint64_t>(target);
        }
        break;
      case prism::dnd::SeekPlan::RestartDecode:
        if (!DoRestartDecode()) return STG_E_READFAULT;
        position_ = static_cast<uint64_t>(target);
        break;
    }
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
    *stat = STATSTG{};
    stat->type = STGTY_STREAM;
    // cbSize must be filled regardless of STATFLAG_NONAME: the flag only
    // suppresses the name, and receivers (Explorer's copy engine) size their
    // progress UI from it.
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
  uint64_t restarts() const { return restarts_; }
  uint64_t wastedBytes() const { return wastedBytes_; }
  uint64_t skippedBytes() const { return skippedBytes_; }
  const std::string& diagnostics() const { return diagnostics_; }

 private:
  void AppendRing(const std::string& chunk) {
    ring_.append(chunk);
    const size_t cap = static_cast<size_t>(policy_.lookbackBytes);
    if (ring_.size() > cap) {
      const size_t excess = ring_.size() - cap;
      ringStart_ += excess;
      ring_.erase(0, excess);
    }
  }

  // Rewind the producer to the stream start on the SAME session. On failure
  // the stream latches failed_: every later Read returns STG_E_READFAULT
  // instead of bytes from an undefined decoder state.
  bool DoRestartDecode() {
    if (!producer_->Restart()) {
      LatchFailure("decoder restart failed");
      return false;
    }
    ++restarts_;
    ring_.clear();
    ringStart_ = 0;
    frontier_ = 0;
    droppingForRestart_ = true;
    diagnostics_ += "restart#" + std::to_string(restarts_) + "; ";
    return true;
  }

  void LatchFailure(const std::string& what) {
    if (failed_) return;
    failed_ = true;
    diagnostics_ += what + "; ";
  }

  ProducerPtr producer_;
  uint64_t totalSize_ = 0;
  uint64_t position_ = 0;    // the caller's cursor -- never advanced speculatively
  uint64_t frontier_ = 0;    // absolute offset of the decoder's next byte
  prism::dnd::SeekPolicy policy_;
  std::string ring_;         // decoded bytes [ringStart_, frontier_)
  uint64_t ringStart_ = 0;
  bool droppingForRestart_ = false;
  uint64_t restarts_ = 0;
  uint64_t wastedBytes_ = 0;
  uint64_t skippedBytes_ = 0;
  bool failed_ = false;
  std::string diagnostics_;
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
