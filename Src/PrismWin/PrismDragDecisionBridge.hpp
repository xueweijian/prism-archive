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

#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "../../Src/PrismCore/DragDrop.hpp"
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

  IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override {
    // Explorer's copy engine uses GetData; a caller-managed destination
    // IStream is not supported by this skeleton on purpose (no silent
    // half-copies), so we refuse loudly instead.
    return E_NOTIMPL;
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

}  // namespace prism::win::dragdrop
