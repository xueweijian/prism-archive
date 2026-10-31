// PrismArchive -- PrismWin
// Bridge between the platform-neutral drag decision core
// (Src/PrismCore/DragDrop.hpp) and the Win32/Shell drag protocol.
//
// Why this exists as a separate header: the decision core must stay free of
// <windows.h> so its rules stay testable on Linux in seconds, but the Shell
// side still needs one place that (a) feeds Explorer observations (resolved
// folder, volume free space) into DecideDragChannel and (b) publishes the
// channel-B payload as a real IDataObject carrying CFSTR_FILEDESCRIPTORW +
// CFSTR_FILECONTENTS. Descriptor names are taken ONLY from the sanitised
// GroupDescriptor -- raw archive paths never reach the clipboard.
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../../Src/PrismCore/DragDrop.hpp"
#include "PrismEngineBridge.hpp"
#include "PrismVirtualDragDrop.hpp"

namespace prism::win::dragdrop {

// ---------------------------------------------------------------------------
// UTF-8 <-> UTF-16 via CP_UTF8. The CRT locale must not decide how an archive
// name is decoded, so we never use the A-suffixed APIs or mbstowcs here.
// ---------------------------------------------------------------------------
inline std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty() || utf8.size() > 0x7FFFFFFFull) return {};
  const int srcBytes = static_cast<int>(utf8.size());
  const int units = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcBytes, nullptr, 0);
  if (units <= 0) return {};
  std::wstring wide(static_cast<size_t>(units), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), srcBytes, wide.data(), units);
  return wide;
}

inline std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty() || wide.size() > 0x7FFFFFFFull) return {};
  const int srcUnits = static_cast<int>(wide.size());
  const int bytes =
      WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcUnits, nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string utf8(static_cast<size_t>(bytes), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), srcUnits, utf8.data(), bytes, nullptr, nullptr);
  return utf8;
}

// PrismCore ticks are FILETIME units (100 ns since 1601-01-01) on Windows
// (see ArchiveCatalog.hpp), so no epoch conversion -- just a split.
inline FILETIME FileTimeFromTicks(int64_t ticks) {
  const uint64_t raw = static_cast<uint64_t>(ticks);
  FILETIME ft{};
  ft.dwLowDateTime = static_cast<DWORD>(raw & 0xFFFFFFFFull);
  ft.dwHighDateTime = static_cast<DWORD>(raw >> 32);
  return ft;
}

// ---------------------------------------------------------------------------
// Channel A support: observations about the Explorer window under the cursor,
// collected conservatively and handed to the decision core. Every failure is
// expressed as a missing/flag-false field -- never as a guessed default,
// because the core refuses on "unknown" instead of gambling.
// ---------------------------------------------------------------------------
inline prism::dnd::DragTargetDescription DescribeExplorerTargetAtCursor(
    POINT screenPt, uint64_t archiveTotalBytes, bool encrypted, bool passwordResolved,
    bool solidBlock, uint64_t solidBlockBytes) {
  prism::dnd::DragTargetDescription target;
  target.archiveTotalBytes = archiveTotalBytes;
  target.encrypted = encrypted;
  target.passwordResolved = passwordResolved;
  target.solidBlock = solidBlock;
  target.solidBlockBytes = solidBlockBytes;

  const std::wstring folder = ExplorerDropTargetResolver::TryResolveExplorerPathAtCursor(screenPt);
  if (folder.empty()) return target;  // not a resolvable folder view: core stays in fallback
  target.isFileManager = true;

  // The Shell hands out backslash paths; the core's grammar is the gatekeeper.
  const prism::dnd::DropTargetPath parsed = prism::dnd::DropTargetPath::Parse(WideToUtf8(folder));
  if (!parsed.trusted) {
    // Keep physicalPath empty: the core names the refusal from its own enum,
    // we do not pre-empt it with a Win-side verdict.
    return target;
  }
  target.physicalPath = parsed.value;

  ULARGE_INTEGER freeToCaller{};
  ULARGE_INTEGER totalBytes{};
  ULARGE_INTEGER totalFree{};
  // Why the full folder path (not a drive root): GetDiskFreeSpaceExW accepts a
  // directory and handles mounted/per-volume quirks better than a root guess.
  if (GetDiskFreeSpaceExW(folder.c_str(), &freeToCaller, &totalBytes, &totalFree)) {
    target.freeBytes = static_cast<uint64_t>(freeToCaller.QuadPart);
  }
  return target;
}

// ZeroCopyDropSource must never stream into a folder merely because it LOOKS
// like a directory (UNC shares and device namespaces pass is_directory today).
// The decision core's path grammar is the single gatekeeper for direct writes.
inline bool IsTrustedDirectDropTarget(const std::wstring& widePath) {
  return prism::dnd::DropTargetPath::Parse(WideToUtf8(widePath)).trusted;
}

// ---------------------------------------------------------------------------
// Two-format enumerator: OLE drag-drop targets enumerate before they read, so
// without this the object would advertise nothing and every drop degrades.
// Defined before VirtualFileDataObject because WRL::Make needs the full type.
// ---------------------------------------------------------------------------
class FixedFormatEnumerator final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<
                                              Microsoft::WRL::ClassicCom>,
                                          IEnumFORMATETC> {
 public:
  HRESULT RuntimeClassInitialize(const FORMATETC* formats, size_t count) {
    formats_.assign(formats, formats + count);
    cursor_ = 0;
    return S_OK;
  }

  IFACEMETHODIMP Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
    if (!out || (count > 0 && !fetched)) return E_POINTER;
    if (fetched) *fetched = 0;
    ULONG taken = 0;
    while (taken < count && cursor_ < formats_.size()) {
      out[taken] = formats_[cursor_];
      ++cursor_;
      ++taken;
    }
    if (fetched) *fetched = taken;
    return (taken == count) ? S_OK : S_FALSE;
  }

  IFACEMETHODIMP Skip(ULONG count) override {
    cursor_ += count;
    if (cursor_ > formats_.size()) {
      cursor_ = formats_.size();
      return S_FALSE;
    }
    return S_OK;
  }

  IFACEMETHODIMP Reset() override {
    cursor_ = 0;
    return S_OK;
  }

  IFACEMETHODIMP Clone(IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    Microsoft::WRL::ComPtr<FixedFormatEnumerator> clone =
        Microsoft::WRL::Make<FixedFormatEnumerator>();
    if (!clone) return E_OUTOFMEMORY;
    HRESULT hr = clone->RuntimeClassInitialize(formats_.data(), formats_.size());
    if (FAILED(hr)) return hr;
    clone->cursor_ = cursor_;
    *out = clone.Detach();
    return S_OK;
  }

 private:
  std::vector<FORMATETC> formats_;
  size_t cursor_ = 0;
};

// ---------------------------------------------------------------------------
// Channel B payload: one IDataObject advertising
//   CFSTR_FILEDESCRIPTORW  (TYMED_HGLOBAL, lindex -1, names from the sanitised
//                          GroupDescriptor only)
//   CFSTR_FILECONTENTS     (TYMED_ISTREAM, one lazy stream per lindex)
// The FILEGROUPDESCRIPTORW blob is built once up-front so GetData can hand out
// a fresh HGLOBAL copy per call: receivers are allowed to GlobalFree the
// medium, so sharing our single allocation would be a use-after-free.
// ---------------------------------------------------------------------------
class VirtualFileDataObject final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<
                                              Microsoft::WRL::ClassicCom>,
                                          IDataObject> {
 public:
  // streamFor(lindex) is invoked only when the drop target actually reads
  // bytes; returning null fails that item instead of writing zeros.
  using StreamFactory = std::function<Microsoft::WRL::ComPtr<IStream>(size_t)>;

  HRESULT RuntimeClassInitialize(const prism::dnd::GroupDescriptor& group,
                                 StreamFactory streamFor) {
    if (group.error != prism::dnd::DenyReason::None) return E_INVALIDARG;
    // An empty descriptor set would be a silent no-op drop for the target;
    // callers must fall back to CF_HDROP / TempCopyFallback instead.
    if (group.files.empty()) return E_INVALIDARG;
    // Explicit wide literals: CFSTR_FILEDESCRIPTORW is a TEXT() macro and the
    // project is not built with UNICODE defined, so the macro would expand to
    // a narrow string and fail RegisterClipboardFormatW.
    cfDescriptor_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
    cfContents_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
    if (cfDescriptor_ == 0 || cfContents_ == 0) return E_FAIL;

    const size_t count = group.files.size();
    const SIZE_T blobBytes = sizeof(FILEGROUPDESCRIPTORW) + (count - 1) * sizeof(FILEDESCRIPTORW);
    descriptorBlob_ = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, blobBytes);
    if (!descriptorBlob_) return E_OUTOFMEMORY;
    auto* header = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(descriptorBlob_));
    if (!header) {
      GlobalFree(descriptorBlob_);
      descriptorBlob_ = nullptr;
      return E_FAIL;
    }
    header->cItems = static_cast<UINT>(count);
    for (size_t i = 0; i < count; ++i) {
      const prism::dnd::VirtualFileDescriptor& file = group.files[i];
      files_.push_back(file);
      FILEDESCRIPTORW& entry = header->fgd[i];
      entry.dwFlags = FD_PROGRESSUI | FD_ATTRIBUTES;
      if (file.isDirectory) {
        entry.dwFileAttributes = FILE_ATTRIBUTE_DIRECTORY;
      } else {
        entry.dwFlags |= FD_FILESIZE | FD_WRITESTIME;
        entry.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
        ULARGE_INTEGER size{};
        size.QuadPart = file.size;
        entry.nFileSizeLow = size.LowPart;
        entry.nFileSizeHigh = size.HighPart;
        const FILETIME ft = FileTimeFromTicks(file.modifiedTime);
        entry.ftCreationTime = ft;
        entry.ftLastWriteTime = ft;
      }
      // Names are already sanitised and <= 259 UTF-16 units by the decision
      // core; lstrcpynW is the last-resort clamp, never the policy.
      const std::wstring wideName = Utf8ToWide(file.name);
      lstrcpynW(entry.cFileName, wideName.c_str(), MAX_PATH);
    }
    GlobalUnlock(descriptorBlob_);
    streamFor_ = std::move(streamFor);
    return S_OK;
  }

  // No `override` here: whether the WRL base declares a virtual destructor is
  // an implementation detail; Release() deletes via the concrete type, so this
  // destructor always runs for objects made through MakeAndInitialize.
  ~VirtualFileDataObject() {
    if (descriptorBlob_) GlobalFree(descriptorBlob_);
  }

  IFACEMETHODIMP GetData(FORMATETC* etc, STGMEDIUM* medium) override {
    if (!etc || !medium) return E_POINTER;
    if (etc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (etc->cfFormat == cfDescriptor_) {
      if (!(etc->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
      if (etc->lindex != -1) return DV_E_LINDEX;  // descriptors are one group blob
      HGLOBAL copy = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, GlobalSize(descriptorBlob_));
      if (!copy) return STG_E_MEDIUMFULL;
      const void* src = GlobalLock(descriptorBlob_);
      void* dst = GlobalLock(copy);
      if (!src || !dst) {
        if (dst) GlobalUnlock(copy);
        if (src) GlobalUnlock(descriptorBlob_);
        GlobalFree(copy);
        return E_FAIL;
      }
      memcpy(dst, src, GlobalSize(descriptorBlob_));
      GlobalUnlock(descriptorBlob_);
      GlobalUnlock(copy);
      medium->tymed = TYMED_HGLOBAL;
      medium->hGlobal = copy;
      medium->pUnkForRelease = nullptr;  // the receiver owns (and frees) the copy
      return S_OK;
    }
    if (etc->cfFormat == cfContents_) {
      if (!(etc->tymed & TYMED_ISTREAM)) return DV_E_TYMED;
      if (etc->lindex < 0 || static_cast<size_t>(etc->lindex) >= files_.size()) {
        return DV_E_LINDEX;
      }
      if (files_[static_cast<size_t>(etc->lindex)].isDirectory) {
        return E_INVALIDARG;  // a directory has no contents stream
      }
      if (!streamFor_) return E_NOTIMPL;
      Microsoft::WRL::ComPtr<IStream> stream = streamFor_(static_cast<size_t>(etc->lindex));
      if (!stream) return E_FAIL;  // the target sees a failure, never silent zeros
      medium->tymed = TYMED_ISTREAM;
      medium->pstm = stream.Detach();  // ownership moves to the receiver
      medium->pUnkForRelease = nullptr;
      return S_OK;
    }
    return DV_E_FORMATETC;
  }

  // Caller-managed destination medium. Where the old skeleton refused this
  // wholesale, the seek-capable stream now lets us copy INTO the caller's
  // medium: FILEDESCRIPTORW into the caller's HGLOBAL, FILECONTENTS pumped
  // into the caller's IStream. Every failure mode carries its specific
  // HRESULT -- a blanket E_NOTIMPL would hide which side broke.
  IFACEMETHODIMP GetDataHere(FORMATETC* etc, STGMEDIUM* medium) override {
    if (!etc || !medium) return E_POINTER;
    if (etc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (etc->cfFormat == cfDescriptor_) {
      if (!(etc->tymed & TYMED_HGLOBAL) || !medium->hGlobal) return DV_E_TYMED;
      const SIZE_T needed = GlobalSize(descriptorBlob_);
      if (GlobalSize(medium->hGlobal) < needed) return STG_E_MEDIUMFULL;
      const void* src = GlobalLock(descriptorBlob_);
      void* dst = GlobalLock(medium->hGlobal);
      if (!src || !dst) {
        if (dst) GlobalUnlock(medium->hGlobal);
        if (src) GlobalUnlock(descriptorBlob_);
        return E_FAIL;
      }
      memcpy(dst, src, needed);
      GlobalUnlock(medium->hGlobal);
      GlobalUnlock(descriptorBlob_);
      return S_OK;
    }
    if (etc->cfFormat == cfContents_) {
      if (!(etc->tymed & TYMED_ISTREAM) || !medium->pstm) return DV_E_TYMED;
      if (etc->lindex < 0 || static_cast<size_t>(etc->lindex) >= files_.size()) {
        return DV_E_LINDEX;
      }
      const size_t index = static_cast<size_t>(etc->lindex);
      if (files_[index].isDirectory) return E_INVALIDARG;
      if (!streamFor_) return E_NOTIMPL;
      Microsoft::WRL::ComPtr<IStream> source = streamFor_(index);
      if (!source) return E_FAIL;  // the target sees a failure, never silent zeros
      // Write at the caller's stream position; the medium's pstm stays
      // caller-owned (we neither AddRef nor Release it).
      return PumpStreamTo(source.Get(), files_[index].size, medium->pstm);
    }
    return DV_E_FORMATETC;
  }

  IFACEMETHODIMP QueryGetData(FORMATETC* etc) override {
    if (!etc) return E_POINTER;
    if (etc->dwAspect != DVASPECT_CONTENT) return DV_E_DVASPECT;
    if (etc->cfFormat == cfDescriptor_ && (etc->tymed & TYMED_HGLOBAL)) return S_OK;
    if (etc->cfFormat == cfContents_ && (etc->tymed & TYMED_ISTREAM)) return S_OK;
    return DV_E_FORMATETC;
  }

  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
    if (!out) return E_POINTER;
    if (in) *out = *in;
    return DATA_S_SAMEFORMATETC;
  }

  IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }

  IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;  // we are a pure data source
    FORMATETC formats[2]{};
    formats[0].cfFormat = cfDescriptor_;
    formats[0].dwAspect = DVASPECT_CONTENT;
    formats[0].lindex = -1;
    formats[0].tymed = TYMED_HGLOBAL;
    formats[1].cfFormat = cfContents_;
    formats[1].dwAspect = DVASPECT_CONTENT;
    formats[1].lindex = -1;
    formats[1].tymed = TYMED_ISTREAM;
    Microsoft::WRL::ComPtr<FixedFormatEnumerator> enumerator =
        Microsoft::WRL::Make<FixedFormatEnumerator>();
    if (!enumerator) return E_OUTOFMEMORY;
    HRESULT hr = enumerator->RuntimeClassInitialize(formats, 2);
    if (FAILED(hr)) return hr;
    *out = enumerator.Detach();
    return S_OK;
  }

  IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  // Pump a fresh source stream into the caller's IStream. A short source is
  // an STG_E_READFAULT -- handing out a silently truncated file would corrupt
  // the user's copy while "succeeding".
  static HRESULT PumpStreamTo(IStream* source, uint64_t bytes, IStream* dest) {
    if (!source || !dest) return E_POINTER;
    uint8_t buffer[16 * 1024];
    uint64_t remaining = bytes;
    while (remaining > 0) {
      const ULONG want = remaining < sizeof(buffer) ? static_cast<ULONG>(remaining)
                                                    : static_cast<ULONG>(sizeof(buffer));
      ULONG got = 0;
      if (FAILED(source->Read(buffer, want, &got)) || got == 0) return STG_E_READFAULT;
      ULONG written = 0;
      const HRESULT hrWrite = dest->Write(buffer, got, &written);
      if (FAILED(hrWrite)) return hrWrite;
      if (written != got) return STG_E_WRITEFAULT;
      remaining -= written;
    }
    return S_OK;
  }

  CLIPFORMAT cfDescriptor_ = 0;
  CLIPFORMAT cfContents_ = 0;
  HGLOBAL descriptorBlob_ = nullptr;
  std::vector<prism::dnd::VirtualFileDescriptor> files_;
  StreamFactory streamFor_;
};

inline HRESULT CreateVirtualFileDataObject(const prism::dnd::GroupDescriptor& group,
                                           VirtualFileDataObject::StreamFactory streamFor,
                                           IDataObject** out) {
  if (!out) return E_POINTER;
  *out = nullptr;
  Microsoft::WRL::ComPtr<VirtualFileDataObject> object;
  HRESULT hr =
      Microsoft::WRL::MakeAndInitialize<VirtualFileDataObject>(&object, group, std::move(streamFor));
  if (FAILED(hr)) return hr;
  *out = object.Detach();
  return S_OK;
}

// ---------------------------------------------------------------------------
// Restartable pull-producer over one archive item (channel B payload).
//
// 7z.dll PUSHES decoded bytes; DelayedExtractStream PULLS on demand. This
// adapter bridges the two with a bounded pipe: a worker thread drives ONE
// Extract call on the reader's IInArchive, the pipe sink parks chunks until
// Next() drains them, and back-pressure makes the sink wait (with an abort
// escape so a restart can always unblock it). Restart() aborts the running
// extraction and starts a fresh Extract on the SAME session -- no re-Open,
// no catalog re-parse; the solid dictionary cost is paid in decode time only.
//
// Lifetime: the reader must stay Open and untouched for the producer's whole
// life; the producer is the only thing allowed to call Extract on it.
// ---------------------------------------------------------------------------
class ArchiveItemStreamProducer final : public DelayedExtractStream::Producer {
 public:
  ArchiveItemStreamProducer(prism::win::ArchiveReader* reader, uint32_t itemIndex,
                            size_t pipeHighWaterBytes = 4u * 1024 * 1024)
      : reader_(reader), index_(itemIndex), highWater_(pipeHighWaterBytes) {}

  ~ArchiveItemStreamProducer() override { StopWorker(); }

  ArchiveItemStreamProducer(const ArchiveItemStreamProducer&) = delete;
  ArchiveItemStreamProducer& operator=(const ArchiveItemStreamProducer&) = delete;

  std::string Next(size_t maxBytes) override {
    if (maxBytes == 0) return {};
    EnsureWorker();
    std::unique_lock<std::mutex> lock(mutex_);
    waitReadable_.wait(lock, [this] { return !chunks_.empty() || eof_ || failed_; });
    if (chunks_.empty()) return {};  // eof_ or failed_: the stream latches loudly
    if (pending_.empty()) {
      pending_ = std::move(chunks_.front());
      chunks_.pop_front();
    }
    std::string out = pending_.substr(0, maxBytes);
    pending_.erase(0, maxBytes < pending_.size() ? maxBytes : pending_.size());
    waitWritable_.notify_all();  // drained: the writer may push again
    return out;
  }

  bool Restart() override {
    StopWorker();  // abort + join the current Extract (same IInArchive kept)
    {
      std::lock_guard<std::mutex> lock(mutex_);
      chunks_.clear();
      pending_.clear();
      eof_ = false;
      failed_ = false;
      started_ = false;
      ++restarts_;
    }
    EnsureWorker();  // fresh Extract on the kept-open session
    return true;
  }

  uint64_t restarts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return restarts_;
  }

 private:
  // Bounded pipe sink: parks chunks for Next(), waits under back-pressure,
  // and returns E_ABORT the moment a restart asks the extraction to stop.
  class PipeSink final : public prism::win::abi::ISequentialOutStream {
   public:
    explicit PipeSink(ArchiveItemStreamProducer* owner) : owner_(owner) {}

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == IID_IUnknown || iid == prism::win::abi::IID_ISequentialOutStream) {
        *out = static_cast<prism::win::abi::ISequentialOutStream*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG r = InterlockedDecrement(&refs_);
      if (r == 0) delete this;
      return r;
    }

    STDMETHODIMP Write(const void* data, prism::win::abi::UInt32 size,
                       prism::win::abi::UInt32* processed) override {
      if (processed) *processed = 0;
      if (!data && size != 0) return E_POINTER;
      if (size == 0) return S_OK;  // a zero-length write must not build a null-range string
      {
        std::unique_lock<std::mutex> lock(owner_->mutex_);
        // Bounded wait with an abort escape: a blocked writer must always be
        // woken by StopWorker, otherwise a restart would deadlock here.
        owner_->waitWritable_.wait_for(lock, std::chrono::milliseconds(50),
                                       [this] { return owner_->abort_ || owner_->queuedBytes_ < owner_->highWater_; });
        if (owner_->abort_) return E_ABORT;
        owner_->chunks_.emplace_back(static_cast<const char*>(data),
                                     static_cast<const char*>(data) + size);
        owner_->queuedBytes_ += size;
        owner_->producedBytes_ += size;
      }
      owner_->waitReadable_.notify_all();
      if (processed) *processed = size;
      return S_OK;
    }

   private:
    ArchiveItemStreamProducer* owner_;
    LONG refs_ = 1;
  };

  // Single-index extract callback: hands the dll the pipe sink, mirrors the
  // MemoryExtractCallback vtable shape from the engine bridge.
  class SingleItemCallback final : public prism::win::abi::IArchiveExtractCallback {
   public:
    explicit SingleItemCallback(prism::win::abi::ISequentialOutStream* sink) : sink_(sink) {
      sink_->AddRef();
    }
    // No `override` on the destructor: the raw 7z ABI structs declare no
    // virtual destructor (same rule as the engine bridge's callbacks).
    ~SingleItemCallback() { sink_->Release(); }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == IID_IUnknown || iid == prism::win::abi::IID_IArchiveExtractCallback) {
        *out = static_cast<prism::win::abi::IArchiveExtractCallback*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
    STDMETHODIMP_(ULONG) Release() override {
      const ULONG r = InterlockedDecrement(&refs_);
      if (r == 0) delete this;
      return r;
    }
    STDMETHODIMP SetTotal(prism::win::abi::UInt64) override { return S_OK; }
    STDMETHODIMP SetCompleted(const prism::win::abi::UInt64*) override { return S_OK; }
    STDMETHODIMP GetStream(prism::win::abi::UInt32, prism::win::abi::ISequentialOutStream** out,
                           prism::win::abi::Int32 mode) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (mode == prism::win::abi::kExtract) {
        *out = sink_;
        sink_->AddRef();
      }
      return S_OK;
    }
    STDMETHODIMP PrepareOperation(prism::win::abi::Int32) override { return S_OK; }
    STDMETHODIMP SetOperationResult(prism::win::abi::Int32) override { return S_OK; }

   private:
    prism::win::abi::ISequentialOutStream* sink_;
    LONG refs_ = 1;
  };

  void EnsureWorker() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    started_ = true;
    abort_ = false;
    worker_ = std::thread([this] { RunExtract(); });
  }

  void RunExtract() {
    PipeSink* sink = new PipeSink(this);
    SingleItemCallback callback(sink);
    const bool ok = reader_->ExtractBatch({index_}, &callback);
    std::lock_guard<std::mutex> lock(mutex_);
    eof_ = true;
    // A restart abort is a normal way to die; anything else is a failure the
    // stream must surface instead of serving a silently short file.
    failed_ = !ok && !abort_;
  }

  void StopWorker() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      abort_ = true;
    }
    waitWritable_.notify_all();
    waitReadable_.notify_all();
    if (worker_.joinable()) worker_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    queuedBytes_ = 0;
  }

  // Evidence accessors: a red CI run must say WHICH side starved.
  bool workerFailed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failed_;
  }
  uint64_t producedBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return producedBytes_;
  }

  prism::win::ArchiveReader* reader_;
  uint32_t index_;
  size_t highWater_;
  std::thread worker_;
  mutable std::mutex mutex_;
  std::condition_variable waitReadable_;
  std::condition_variable waitWritable_;
  std::deque<std::string> chunks_;
  std::string pending_;
  size_t queuedBytes_ = 0;
  bool started_ = false;
  bool eof_ = false;
  uint64_t producedBytes_ = 0;
  bool failed_ = false;
  bool abort_ = false;
  uint64_t restarts_ = 0;
};

// Factory: one restartable stream for one archive item, wired through the
// seek planner with the caller's policy. This is the production path for
// CFSTR_FILECONTENTS lindex->IStream.
inline Microsoft::WRL::ComPtr<IStream> CreateDelayedExtractStream(
    prism::win::ArchiveReader* reader, uint32_t itemIndex, uint64_t totalSize,
    const prism::dnd::SeekPolicy& policy) {
  auto producer = std::make_shared<ArchiveItemStreamProducer>(reader, itemIndex);
  Microsoft::WRL::ComPtr<DelayedExtractStream> stream;
  if (FAILED(Microsoft::WRL::MakeAndInitialize<DelayedExtractStream>(
          &stream, producer, totalSize, policy))) {
    return nullptr;
  }
  return stream;
}

}  // namespace prism::win::dragdrop
