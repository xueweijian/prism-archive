// PrismArchive -- PrismWin
// ArchiveWriter: the EXECUTION half of blueprint ch.2 -- "put these files into
// a real .7z / .zip / .tar". Until this header existed the product could open,
// list, thumbnail, extract, and even rewrite an archive's directory tree, but
// it could not CREATE one.
//
// Everything it needs is already proven elsewhere and is REUSED, not copied:
//   * SevenZipLibrary / InFileStream / DiskOutStream   (PrismEngineBridge.hpp)
//   * the create-from-nothing UpdateItems shape        (Tests/win/test_mkdir_matrix.cpp)
// The decision (which properties, which method, which dictionary, is it solid)
// comes from PrismCore's CompressionPlan -- this file only marshals.
//
// ABI provenance (CONTRACTS A4, pinned against upstream 7-Zip main on
// 2026-11-01, CPP/7zip/Guid.txt + CPP/7zip/Archive/IArchive.h):
//   IOutArchive             {23170F69-40C1-278A-0000-000600A00000}  member 0xA0
//   IArchiveUpdateCallback  {23170F69-40C1-278A-0000-000600800000}  member 0x80
//   ISetProperties          {23170F69-40C1-278A-0000-000600030000}  member 0x03
//   IProgress               {23170F69-40C1-278A-0000-000000050000}  member 0x05
//   ICryptoGetTextPassword2 {23170F69-40C1-278A-0000-000500110000}  member 0x11
//   Handler CLSIDs          {23170F69-40C1-278A-1000-000110xx0000}: Zip 01, 7z 07, Tar EE
// ISetProperties is a SEPARATE interface: IOutArchive does not derive it, the
// canonical caller QIs for it (UI/Common/SetProperties.cpp). IOutArchive is
// exactly QI + UpdateItems + GetFileTimeType (no extra slots before it).
//
// What the engine REQUIRES of the callback (7zHandlerOut.cpp, read line by
// line -- each of these is a hard E_INVALIDARG if answered wrong):
//   * kpidSize must be VT_UI8 for every new file, and must equal what
//     GetStream() actually serves (otherwise the entry is truncated or the
//     item is refused);
//   * kpidIsDir must be VT_BOOL when the node is a folder; VT_EMPTY falls back
//     to kpidAttrib's FILE_ATTRIBUTE_DIRECTORY and then to "it is a file";
//   * kpidAttrib is VT_UI4 (VT_EMPTY is legal), kpidPath is VT_BSTR;
//   * GetStream() returns S_OK + NULL only for directories, a real
//     ISequentialInStream for files, S_FALSE to skip.
// Both wrong answers above are exercised by the negative controls compiled into
// Tests/win/test_write_archive.cpp (PRISM_CW_MUTANT_DIR_AS_FILE /
// PRISM_CW_MUTANT_NO_SIZE), which is how this file's guards stay sharp.
//
// After UpdateItems the writer RE-OPENS the result and compares it against the
// plan: entry count, per-path kind, per-file size AND per-file payload bytes.
// A create that "succeeds" but wrote the wrong thing is the failure mode that
// matters here (7z silently writing a folder as a 0-byte file is exactly what
// the mkdir matrix caught), so the archive only replaces the target after the
// comparison passes -- same gate shape as VirtualTreeExecutor::CommitTree.
#pragma once

#include <windows.h>
#include <propidl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "../PrismCore/CompressionPlan.hpp"
#include "PrismEngineBridge.hpp"
#include "SevenZipAbi.hpp"

namespace prism::win::cw {

// ---- update-side ABI (provenance in the file header) -----------------------
constexpr GUID kIID_IProgress = {0x23170F69, 0x40C1, 0x278A,
                                 {0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00}};
constexpr GUID kIID_IOutArchive = {0x23170F69, 0x40C1, 0x278A,
                                   {0x00, 0x00, 0x00, 0x06, 0x00, 0xA0, 0x00, 0x00}};
constexpr GUID kIID_IArchiveUpdateCallback = {0x23170F69, 0x40C1, 0x278A,
                                              {0x00, 0x00, 0x00, 0x06, 0x00, 0x80, 0x00, 0x00}};
constexpr GUID kIID_ISetProperties = {0x23170F69, 0x40C1, 0x278A,
                                      {0x00, 0x00, 0x00, 0x06, 0x00, 0x03, 0x00, 0x00}};
constexpr GUID kIID_ICryptoGetTextPassword2 = {0x23170F69, 0x40C1, 0x278A,
                                               {0x00, 0x00, 0x00, 0x05, 0x00, 0x11, 0x00, 0x00}};
// kpidIsAnti = 21 (CPP/7zip/PropID.h): answering VT_BOOL FALSE is explicit;
// VT_EMPTY would also be legal but leaves the engine guessing.
constexpr PROPID kKpidIsAnti = 21;
constexpr GUID CLSID_CFormatTar = {0x23170F69, 0x40C1, 0x278A,
                                   {0x10, 0x00, 0x00, 0x01, 0x10, 0xEE, 0x00, 0x00}};

struct ISetProperties : public IUnknown {
  STDMETHOD(SetProperties)
  (const wchar_t* const* names, const PROPVARIANT* values, abi::UInt32 numProps) PURE;
};

// IProgress first, then the four update methods (Z7_IFACEM_IArchiveUpdateCallback).
struct IArchiveUpdateCallback : public abi::IProgress {
  STDMETHOD(GetUpdateItemInfo)
  (abi::UInt32 index, abi::Int32* newData, abi::Int32* newProps,
   abi::UInt32* indexInArchive) PURE;
  STDMETHOD(GetProperty)(abi::UInt32 index, PROPID propID, PROPVARIANT* value) PURE;
  STDMETHOD(GetStream)(abi::UInt32 index, abi::ISequentialInStream** inStream) PURE;
  STDMETHOD(SetOperationResult)(abi::Int32 operationResult) PURE;
};

struct IOutArchive : public IUnknown {
  STDMETHOD(UpdateItems)
  (abi::ISequentialOutStream* outStream, abi::UInt32 numItems,
   IArchiveUpdateCallback* updateCallback) PURE;
  STDMETHOD(GetFileTimeType)(abi::UInt32* type) PURE;
};

struct ICryptoGetTextPassword2 : public IUnknown {
  STDMETHOD(CryptoGetTextPassword2)(abi::Int32* passwordIsDefined, BSTR* password) PURE;
};


// ---------------------------------------------------------------------------
// Input stream: one source file, seekable size for IStreamGetSize
// ---------------------------------------------------------------------------
class SourceFileStream final : public abi::ISequentialInStream, public abi::IStreamGetSize {
 public:
  explicit SourceFileStream(std::wstring path) {
    std::wstring ntPath = ToBackslashes(std::move(path));
    if (ntPath.rfind(L"\\\\?\\", 0) != 0) ntPath.insert(0, L"\\\\?\\");
    handle_ = CreateFileW(ntPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size{};
      if (GetFileSizeEx(handle_, &size)) size_ = static_cast<uint64_t>(size.QuadPart);
    }
  }
  ~SourceFileStream() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  uint64_t size() const { return size_; }

  // CONTRACTS B8: inheriting an interface is not implementing it -- the engine
  // only ever reaches IStreamGetSize through QI.
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream) {
      *out = static_cast<abi::ISequentialInStream*>(this);
    } else if (iid == abi::IID_IStreamGetSize) {
      *out = static_cast<abi::IStreamGetSize*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processedSize) override {
    if (processedSize) *processedSize = 0;
    if (handle_ == INVALID_HANDLE_VALUE) return E_FAIL;
    DWORD read = 0;
    if (!ReadFile(handle_, data, size, &read, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    if (processedSize) *processedSize = read;
    return S_OK;
  }

  STDMETHODIMP GetSize(abi::UInt64* size) override {
    if (!size) return E_POINTER;
    *size = size_;
    return S_OK;
  }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  uint64_t size_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// The callback the engine drives: every answer comes from the plan
// ---------------------------------------------------------------------------
class CreateUpdateCallback final : public IArchiveUpdateCallback,
                                  public abi::ICryptoGetTextPassword,
                                  public ICryptoGetTextPassword2 {
 public:
  CreateUpdateCallback(std::vector<prism::PlanItem> items, std::wstring sourceRoot,
                       std::wstring password, std::function<bool(uint64_t)> progress)
      : items_(std::move(items)),
        sourceRoot_(std::move(sourceRoot)),
        password_(std::move(password)),
        progress_(std::move(progress)) {}

  // CONTRACTS B8: all four inherited interfaces are reachable, or the engine
  // gets E_NOINTERFACE and returns E_NOTIMPL (the defect that burned 18 CI
  // rounds on DiskOutStream).
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == kIID_IArchiveUpdateCallback) {
      *out = static_cast<IArchiveUpdateCallback*>(this);
    } else if (iid == kIID_IProgress) {
      *out = static_cast<abi::IProgress*>(this);
    } else if (iid == abi::IID_ICryptoGetTextPassword) {
      *out = static_cast<abi::ICryptoGetTextPassword*>(this);
    } else if (iid == kIID_ICryptoGetTextPassword2) {
      *out = static_cast<ICryptoGetTextPassword2*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP SetTotal(abi::UInt64 total) override {
    total_ = total;
    return S_OK;
  }

  // Honest progress: the engine reports COMPRESSED bytes written so far.
  STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
    if (!completeValue) return S_OK;
    ++completedCalls_;
    if (progress_ && !progress_(*completeValue)) {
      Fail("cancelled through the progress callback");
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHODIMP GetUpdateItemInfo(abi::UInt32 index, abi::Int32* newData, abi::Int32* newProps,
                                 abi::UInt32* indexInArchive) override {
    if (!newData || !newProps || !indexInArchive) return E_POINTER;
    if (index >= items_.size()) return E_INVALIDARG;
    // Create-from-nothing: every node is brand new, so it carries its own data
    // and its own properties, and indexInArchive is (UInt32)-1. This is the
    // shape proven by Tests/win/test_mkdir_matrix.cpp ("create0").
    *newData = 1;
    *newProps = 1;
    *indexInArchive = static_cast<abi::UInt32>(-1);
    return S_OK;
  }

  STDMETHODIMP GetProperty(abi::UInt32 index, PROPID propID, PROPVARIANT* value) override {
    if (!value) return E_POINTER;
    PropVariantInit(value);
    if (index >= items_.size()) return E_INVALIDARG;
    const prism::PlanItem& item = items_[index];
    const bool isDir = item.isDir;

    if (propID == abi::kpidPath) {
      // B4: Win32 layer speaks backslashes; the handler re-normalizes itself
      // (7zHandlerOut.cpp: NItemName::ReplaceSlashes_OsToUnix).
      value->vt = VT_BSTR;
      value->bstrVal = SysAllocString(ToBackslashes(Utf8ToWide(item.path)).c_str());
      return value->bstrVal ? S_OK : E_OUTOFMEMORY;
    }

    if (propID == abi::kpidSize) {
#if defined(PRISM_CW_MUTANT_NO_SIZE)
      // MUTANT (negative control, CONTRACTS D3): answer nothing for kpidSize.
      // 7zHandlerOut.cpp:673 then returns E_INVALIDARG and the whole create
      // must fail -- a writer that "works" without a size is lying about the
      // item it is about to stream.
      if (!isDir) {
        ++injectedMissingSize_;
        return S_OK;  // VT_EMPTY
      }
#endif
      // REQUIRED as VT_UI8 for every new file, and it must equal the bytes
      // GetStream() serves.
      value->vt = VT_UI8;
      value->uhVal.QuadPart = isDir ? 0 : item.size;
      return S_OK;
    }

    if (propID == abi::kpidIsDir) {
#if defined(PRISM_CW_MUTANT_DIR_AS_FILE)
      // MUTANT (negative control, CONTRACTS D3): tell the engine that folders
      // are files. 7z then writes every directory as a 0-byte file; only the
      // post-write comparison against the plan can catch it, which is exactly
      // the guard this mutation is proving.
      if (isDir) ++injectedDirAsFile_;
      value->vt = VT_BOOL;
      value->boolVal = VARIANT_FALSE;
      return S_OK;
#else
      value->vt = VT_BOOL;
      value->boolVal = isDir ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
#endif
    }

    if (propID == abi::kpidAttrib) {
      value->vt = VT_UI4;
      value->ulVal = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
      return S_OK;
    }

    if (propID == abi::kpidMTime) {
      // 7-Zip needs an MTime to sort entries; every filesystem-driven caller
      // has one. The plan carries it as FILETIME ticks.
      value->vt = VT_FILETIME;
      value->hVal.QuadPart = static_cast<LONGLONG>(item.mtime100ns);
      return S_OK;
    }

    if (propID == kKpidIsAnti) {
      value->vt = VT_BOOL;
      value->boolVal = VARIANT_FALSE;
      return S_OK;
    }
    return S_OK;  // VT_EMPTY: "no such property", which every caller accepts
  }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialInStream** inStream) override {
    if (!inStream) return E_POINTER;
    *inStream = nullptr;
    if (index >= items_.size()) return E_INVALIDARG;
    const prism::PlanItem& item = items_[index];
    if (item.isDir) return S_OK;  // S_OK + NULL is the documented folder answer
    // COM out-params transfer ONE reference; SourceFileStream starts at 1.
    auto* stream = new SourceFileStream(SourceRootPath(item.path));
    if (!stream->valid()) {
      Fail("cannot open source file for reading: " + item.path);
      stream->Release();
      return S_FALSE;  // "skip this file" -- loud, and the caller turns it red
    }
    if (stream->size() != item.size) {
      // The plan promised a size the disk does not have. Streaming it anyway
      // would write a truncated or padded entry, so refuse instead.
      char buf[128];
      std::snprintf(buf, sizeof(buf), "size changed since planning: plan=%llu disk=%llu",
                    static_cast<unsigned long long>(item.size),
                    static_cast<unsigned long long>(stream->size()));
      Fail(std::string("source file ") + item.path + " " + buf);
      stream->Release();
      return S_FALSE;
    }
    ++streamsServed_;
    *inStream = stream;
    return S_OK;
  }

  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
    if (operationResult != 0) {
      char buf[64];
      std::snprintf(buf, sizeof(buf), "SetOperationResult=%d", static_cast<int>(operationResult));
      Fail(buf);
    } else {
      ++itemsWritten_;
    }
    return S_OK;
  }

  STDMETHODIMP CryptoGetTextPassword(BSTR* password) override {
    if (!password) return E_POINTER;
    if (password_.empty()) {
      *password = nullptr;
      return S_OK;  // no password: legal for an unencrypted create
    }
    *password = SysAllocString(password_.c_str());
    return *password ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP CryptoGetTextPassword2(abi::Int32* passwordIsDefined, BSTR* password) override {
    if (!passwordIsDefined || !password) return E_POINTER;
    if (password_.empty()) {
      *passwordIsDefined = 0;
      *password = nullptr;
      return S_OK;
    }
    *passwordIsDefined = 1;
    *password = SysAllocString(password_.c_str());
    return *password ? S_OK : E_OUTOFMEMORY;
  }

  const std::string& reason() const { return reason_; }
  uint64_t itemsWritten() const { return itemsWritten_; }
  uint64_t streamsServed() const { return streamsServed_; }
  uint64_t completedCalls() const { return completedCalls_; }
  // CONTRACTS D3: proof that a deliberate defect actually reached the wire.
  int injectedDirAsFile() const { return injectedDirAsFile_; }
  int injectedMissingSize() const { return injectedMissingSize_; }
  std::string trace() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "items=%llu streams=%llu setCompleted=%llu total=%llu injected(dirAsFile=%d,"
                  "missingSize=%d)",
                  static_cast<unsigned long long>(items_.size()),
                  static_cast<unsigned long long>(streamsServed_),
                  static_cast<unsigned long long>(completedCalls_),
                  static_cast<unsigned long long>(total_), injectedDirAsFile_,
                  injectedMissingSize_);
    return buf;
  }

 private:
  std::wstring SourceRootPath(const std::string& relPath) const {
    std::wstring out = sourceRoot_;
    if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out.push_back(L'\\');
    out += Utf8ToWide(relPath);
    return out;
  }

  void Fail(const std::string& why) {
    if (reason_.empty()) reason_ = why;  // keep the FIRST reason, lose none
  }

  std::vector<prism::PlanItem> items_;
  std::wstring sourceRoot_;
  std::wstring password_;
  std::function<bool(uint64_t)> progress_;
  std::string reason_;
  abi::UInt64 total_ = 0;
  uint64_t itemsWritten_ = 0;
  uint64_t streamsServed_ = 0;
  uint64_t completedCalls_ = 0;
  int injectedDirAsFile_ = 0;
  int injectedMissingSize_ = 0;
  LONG refs_ = 1;
};


// ---------------------------------------------------------------------------
// Verification reader: re-open the freshly written archive and read it back
// ---------------------------------------------------------------------------
struct VerifyItem {
  std::string path;  // normalized UTF-8, '/' separated
  bool isDir = false;
  uint64_t size = 0;
};

inline std::string ReaderPropString(abi::IInArchive* archive, uint32_t index, PROPID propId) {
  PROPVARIANT value{};
  PropVariantInit(&value);
  std::string out;
  if (archive && SUCCEEDED(archive->GetProperty(index, propId, &value))) {
    if (value.vt == VT_BSTR && value.bstrVal) out = WideToUtf8(value.bstrVal);
    else if (value.vt == VT_LPWSTR && value.pwszVal) out = WideToUtf8(value.pwszVal);
  }
  PropVariantClear(&value);
  return out;
}

inline bool ReaderPropBool(abi::IInArchive* archive, uint32_t index, PROPID propId) {
  PROPVARIANT value{};
  PropVariantInit(&value);
  bool out = false;
  if (archive && SUCCEEDED(archive->GetProperty(index, propId, &value))) {
    if (value.vt == VT_BOOL) out = value.boolVal != VARIANT_FALSE;
  }
  PropVariantClear(&value);
  return out;
}

inline uint64_t ReaderPropUInt(abi::IInArchive* archive, uint32_t index, PROPID propId) {
  PROPVARIANT value{};
  PropVariantInit(&value);
  uint64_t out = 0;
  if (archive && SUCCEEDED(archive->GetProperty(index, propId, &value))) {
    if (value.vt == VT_UI8) out = static_cast<uint64_t>(value.uhVal.QuadPart);
    else if (value.vt == VT_UI4) out = value.ulVal;
  }
  PropVariantClear(&value);
  return out;
}

// One-item extract callback used by the verification pass.
class SingleItemSink final : public abi::IArchiveExtractCallback,
                            public abi::ICryptoGetTextPassword {
 public:
  SingleItemSink(abi::ISequentialOutStream* sink, std::wstring password)
      : sink_(sink), password_(std::move(password)) {}
  // CONTRACTS B8: the extract path QIs ICryptoGetTextPassword per item. Without
  // this answer an AES archive opens fine (the open callback had the password)
  // and then every payload decrypts to ZERO bytes -- measured 2026-11-01, and
  // the payload comparison in CreateArchive is exactly what surfaced it.
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
      *out = static_cast<abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == abi::IID_ICryptoGetTextPassword) {
      *out = static_cast<abi::ICryptoGetTextPassword*>(this);
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
  STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }
  STDMETHODIMP GetStream(abi::UInt32, abi::ISequentialOutStream** out,
                         abi::Int32 askExtractMode) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (askExtractMode == abi::kExtract) {
      *out = sink_;
      sink_->AddRef();
    }
    return S_OK;
  }
  STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }
  STDMETHODIMP SetOperationResult(abi::Int32) override { return S_OK; }

  STDMETHODIMP CryptoGetTextPassword(BSTR* password) override {
    if (!password) return E_POINTER;
    if (password_.empty()) {
      *password = nullptr;
      return S_OK;
    }
    *password = SysAllocString(password_.c_str());
    return *password ? S_OK : E_OUTOFMEMORY;
  }

 private:
  abi::ISequentialOutStream* sink_ = nullptr;
  std::wstring password_;
  LONG refs_ = 1;
};

class VerifyReader {
 public:
  ~VerifyReader() { Close(); }

  bool Open(SevenZipLibrary& library, const std::wstring& path, const std::wstring& password) {
    stream_ = new InFileStream(path);
    if (!stream_->valid()) {
      trace_ = "CreateFileW failed";
      return false;
    }
    for (const GUID* clsid : {&abi::CLSID_CFormat7z, &abi::CLSID_CFormatZip, &CLSID_CFormatTar}) {
      abi::IInArchive* candidate = nullptr;
      if (FAILED(library.CreateObject(*clsid, abi::IID_IInArchive,
                                      reinterpret_cast<void**>(&candidate))) ||
          !candidate) {
        continue;
      }
      // A rejected Open consumes stream position: rewind for the next handler.
      abi::UInt64 pos = 0;
      stream_->Seek(0, 0, &pos);
      auto* callback = new OpenCallback(password);
      const HRESULT hr = candidate->Open(stream_, nullptr, callback);
      callback->Release();
      if (hr == S_OK) {  // S_OK only: S_FALSE means "not my format" (CONTRACTS A1)
        handler_ = candidate;
        password_ = password;
        return true;
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "open hr=0x%08lX; ", static_cast<unsigned long>(hr));
      trace_ += buf;
      candidate->Release();
    }
    return false;
  }

  void Close() {
    if (handler_) {
      handler_->Close();
      handler_->Release();
      handler_ = nullptr;
    }
    if (stream_) {
      stream_->Release();
      stream_ = nullptr;
    }
  }

  const std::string& trace() const { return trace_; }

  std::vector<VerifyItem> Catalog() {
    std::vector<VerifyItem> out;
    if (!handler_) return out;
    abi::UInt32 count = 0;
    if (FAILED(handler_->GetNumberOfItems(&count))) return out;
    out.reserve(count);
    for (abi::UInt32 i = 0; i < count; ++i) {
      VerifyItem item;
      item.path = prism::NormalizeArchivePath(ReaderPropString(handler_, i, abi::kpidPath));
      item.isDir = ReaderPropBool(handler_, i, abi::kpidIsDir);
      item.size = ReaderPropUInt(handler_, i, abi::kpidSize);
      out.push_back(std::move(item));
    }
    return out;
  }

  bool ExtractTo(abi::UInt32 index, uint64_t expectedSize, std::vector<uint8_t>* out) {
    if (!handler_) return false;
    // Bounded sink: a corrupt entry cannot make the verification itself the
    // memory hog. expectedSize comes from the archive's own metadata.
    auto* sink = new MemoryOutStream(static_cast<size_t>(expectedSize) + 4096);
    auto* callback = new SingleItemSink(sink, password_);
    const HRESULT hr = handler_->Extract(&index, 1, 0, callback);
    callback->Release();
    const std::vector<uint8_t> bytes = sink->bytes();
    sink->Release();
    if (FAILED(hr)) return false;
    *out = bytes;
    return true;
  }

 private:
  InFileStream* stream_ = nullptr;
  abi::IInArchive* handler_ = nullptr;
  std::wstring password_;
  std::string trace_;
};

inline bool ReadWholeFile(const std::wstring& path, std::vector<uint8_t>* out) {
  std::wstring ntPath = ToBackslashes(path);
  if (ntPath.rfind(L"\\\\?\\", 0) != 0) ntPath.insert(0, L"\\\\?\\");
  HANDLE h = CreateFileW(ntPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(h, &size)) {
    CloseHandle(h);
    return false;
  }
  out->resize(static_cast<size_t>(size.QuadPart));
  size_t done = 0;
  while (done < out->size()) {
    DWORD read = 0;
    const DWORD want = static_cast<DWORD>((std::min)(size_t{1} << 20, out->size() - done));
    if (!ReadFile(h, out->data() + done, want, &read, nullptr) || read == 0) {
      CloseHandle(h);
      return false;
    }
    done += read;
  }
  CloseHandle(h);
  return true;
}


// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------
struct WriteRequest {
  std::wstring archivePath;  // final target: full path WITH extension
  std::wstring sourceRoot;   // directory the plan's relative paths live under
  prism::CompressionPlan plan;
  std::wstring password;     // empty = unencrypted
  std::function<bool(uint64_t)> progress;  // returns false to cancel
};

struct WriteResult {
  bool ok = false;
  std::string error;
  std::string trace;
  std::wstring archivePath;
  uint64_t bytesWritten = 0;   // bytes the engine wrote into the new archive
  uint64_t itemsWritten = 0;   // SetOperationResult(kOK) count
  uint64_t streamsServed = 0;  // GetStream() answers
  uint64_t verifiedItems = 0;  // entries compared against the plan
  bool verified = false;       // re-open + per-file payload comparison passed
  int64_t elapsedMs = 0;
  // CONTRACTS D3: non-zero only in a mutant build, and asserted by the test so
  // an injection that silently stopped firing can never look like a sharp test.
  int injectedDirAsFile = 0;
  int injectedMissingSize = 0;
};

inline const GUID* ClsidFor(prism::ArchiveFormat format) {
  switch (format) {
    case prism::ArchiveFormat::SevenZip: return &abi::CLSID_CFormat7z;
    case prism::ArchiveFormat::Zip: return &abi::CLSID_CFormatZip;
    case prism::ArchiveFormat::Tar: return &CLSID_CFormatTar;
  }
  return &abi::CLSID_CFormat7z;
}

inline std::wstring ResolveSevenZipDll() {
  wchar_t buf[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", buf, ARRAYSIZE(buf)) > 0) return buf;
  const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  if (n > 0 && n < MAX_PATH) {
    std::wstring exe(buf, n);
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
      const std::wstring candidate = exe.substr(0, slash + 1) + L"7z.dll";
      if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
    }
  }
  wchar_t pf[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"ProgramFiles", pf, ARRAYSIZE(pf)) > 0) {
    const std::wstring candidate = std::wstring(pf) + L"\\7-Zip\\7z.dll";
    if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) return candidate;
  }
  return {};
}

// ISetProperties is a SEPARATE interface (IArchive.h, member 0x03): the handler
// must be QI'd for it, and a failure is loud -- silently ignoring the user's
// level/encryption would produce an archive nobody asked for.
inline bool ApplyProperties(IOutArchive* out, const std::vector<prism::PropertySetting>& props,
                            std::string* error, std::string* trace) {
  if (props.empty()) return true;
  ISetProperties* setter = nullptr;
  const HRESULT hrQI =
      out->QueryInterface(kIID_ISetProperties, reinterpret_cast<void**>(&setter));
  if (FAILED(hrQI) || !setter) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "QueryInterface(IID_ISetProperties) hr=0x%08lX: the handler cannot be "
                  "configured, so level/format options would be silently ignored",
                  static_cast<unsigned long>(hrQI));
    *error = buf;
    return false;
  }
  std::vector<std::wstring> names;
  names.reserve(props.size());
  for (const auto& p : props) names.push_back(Utf8ToWide(p.name));
  std::vector<const wchar_t*> namePtrs;
  namePtrs.reserve(names.size());
  for (const auto& n : names) namePtrs.push_back(n.c_str());
  std::vector<PROPVARIANT> values(props.size());
  for (size_t i = 0; i < props.size(); ++i) {
    PropVariantInit(&values[i]);
    switch (props[i].kind) {
      case prism::PropKind::Bool:
        values[i].vt = VT_BOOL;
        values[i].boolVal = props[i].boolValue ? VARIANT_TRUE : VARIANT_FALSE;
        break;
      case prism::PropKind::UInt:
        values[i].vt = VT_UI4;
        values[i].ulVal = props[i].uintValue;
        break;
      case prism::PropKind::Str:
        values[i].vt = VT_BSTR;
        values[i].bstrVal = SysAllocString(Utf8ToWide(props[i].strValue).c_str());
        break;
    }
    if (trace) {
      *trace += props[i].name + "=" +
                (props[i].kind == prism::PropKind::Bool ? (props[i].boolValue ? "true" : "false")
                 : props[i].kind == prism::PropKind::UInt ? std::to_string(props[i].uintValue)
                                                          : props[i].strValue) +
                " ";
    }
  }
  const HRESULT hr =
      setter->SetProperties(namePtrs.data(), values.data(), static_cast<abi::UInt32>(props.size()));
  for (auto& v : values) PropVariantClear(&v);
  setter->Release();
  if (FAILED(hr)) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "SetProperties hr=0x%08lX (the handler refused one of: %s)",
                  static_cast<unsigned long>(hr), trace ? trace->c_str() : "");
    *error = buf;
    return false;
  }
  return true;
}

// Deletes the temp file on every path that is not a successful replace.
struct TempArchiveGuard {
  std::wstring path;
  bool armed = true;
  ~TempArchiveGuard() {
    if (armed && !path.empty()) DeleteFileW(ToBackslashes(path).c_str());
  }
};

inline WriteResult CreateArchive(const WriteRequest& req) {
  WriteResult out;
  out.archivePath = req.archivePath;
  const auto started = std::chrono::steady_clock::now();
  auto elapsedMs = [&started]() -> int64_t {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    return static_cast<int64_t>((us + 999) / 1000);
  };
  auto fail = [&out, &elapsedMs](std::string why) {
    out.ok = false;
    out.error = "CreateArchive: " + std::move(why);
    out.elapsedMs = elapsedMs();
    return out;
  };

  if (!req.plan.ok) return fail("plan is not usable (" + req.plan.error + ")");
  if (req.archivePath.empty()) return fail("target archive path is empty");
  if (req.archivePath.find_first_of(L"*?\"<>|") != std::wstring::npos) {
    return fail("target archive path contains wildcard characters");
  }
  if (req.plan.encryptFileNames && req.password.empty()) {
    return fail("header encryption was requested without a password");
  }

  // ---- pre-flight the source tree (before a single byte is written) ------
  // The plan came from a directory enumeration; if the disk no longer agrees,
  // failing now beats writing a temp archive that cannot be verified.
  std::vector<prism::PlanItem> items = req.plan.items;
  for (auto& item : items) {
    std::wstring path = req.sourceRoot;
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') path.push_back(L'\\');
    path += Utf8ToWide(item.path);
    std::wstring ntPath = ToBackslashes(path);
    if (ntPath.rfind(L"\\\\?\\", 0) != 0) ntPath.insert(0, L"\\\\?\\");
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ntPath.c_str(), GetFileExInfoStandard, &data)) {
      return fail("source missing: " + item.path);
    }
    const bool diskIsDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (diskIsDir != item.isDir) {
      return fail("plan/disk kind mismatch for " + item.path +
                  (item.isDir ? " (plan: directory, disk: file)" : " (plan: file, disk: directory)"));
    }
    if (!item.isDir) {
      const uint64_t diskSize = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
      if (diskSize != item.size) {
        return fail("plan/disk size mismatch for " + item.path + " (plan=" +
                    std::to_string(item.size) + " disk=" + std::to_string(diskSize) + ")");
      }
    }
    if (item.mtime100ns == 0) {
      ULARGE_INTEGER ticks{};
      ticks.LowPart = data.ftLastWriteTime.dwLowDateTime;
      ticks.HighPart = data.ftLastWriteTime.dwHighDateTime;
      item.mtime100ns = ticks.QuadPart;
    }
  }

  SevenZipLibrary library;
  const std::wstring dll = ResolveSevenZipDll();
  if (dll.empty()) return fail("7z.dll not found (PRISM_7Z_DLL / exe dir / Program Files)");
  if (!library.Load(dll)) return fail("cannot load 7z.dll at " + WideToUtf8(dll));

  // ---- temp target in the SAME directory (only a same-volume rename is atomic)
  const size_t lastSlash = req.archivePath.find_last_of(L"\\/");
  const std::wstring dir =
      (lastSlash == std::wstring::npos) ? L"." : req.archivePath.substr(0, lastSlash);
  const std::wstring name =
      (lastSlash == std::wstring::npos) ? req.archivePath : req.archivePath.substr(lastSlash + 1);
  if (name.empty()) return fail("target archive path has no file name");
  const std::wstring tempPath = dir + L"\\" + name + L".cw-" +
                                std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L".tmp";
  TempArchiveGuard guard;
  guard.path = tempPath;

  // ---- create the out handler for the requested format -------------------
  IOutArchive* outArchive = nullptr;
  const GUID* clsid = ClsidFor(req.plan.format);
  const HRESULT hrCreate =
      library.CreateObject(*clsid, kIID_IOutArchive, reinterpret_cast<void**>(&outArchive));
  if (FAILED(hrCreate) || !outArchive) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "CreateObject(IOutArchive) for %.*s hr=0x%08lX -- this 7z.dll cannot WRITE that "
                  "format (reading it is a different interface)",
                  static_cast<int>(prism::ToString(req.plan.format).size()),
                  prism::ToString(req.plan.format).data(), static_cast<unsigned long>(hrCreate));
    return fail(buf);
  }
  auto failWithHandler = [&](std::string why) {
    outArchive->Release();
    return fail(std::move(why));
  };

  std::string propTrace;
  std::string propError;
  if (!ApplyProperties(outArchive, req.plan.properties, &propError, &propTrace)) {
    return failWithHandler(propError);
  }
  out.trace = "format=" + std::string(prism::ToString(req.plan.format)) +
              " props=[" + propTrace + "] ";

  // ---- write -------------------------------------------------------------
  auto* outStream = new DiskOutStream(tempPath);
  if (!outStream->Open()) {
    const DWORD err = GetLastError();
    outStream->Release();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "cannot create temp archive err=%lu", static_cast<unsigned long>(err));
    return failWithHandler(buf);
  }
  auto* callback = new CreateUpdateCallback(items, req.sourceRoot, req.password, req.progress);
  const HRESULT hrUpdate =
      outArchive->UpdateItems(outStream, static_cast<abi::UInt32>(items.size()), callback);
  out.bytesWritten = outStream->written();
  out.itemsWritten = callback->itemsWritten();
  out.streamsServed = callback->streamsServed();
  out.injectedDirAsFile = callback->injectedDirAsFile();
  out.injectedMissingSize = callback->injectedMissingSize();
  const std::string cbReason = callback->reason();
  out.trace += "update hr=0x" + [](HRESULT hr) {
    char b[16];
    std::snprintf(b, sizeof(b), "%08lX", static_cast<unsigned long>(hr));
    return std::string(b);
  }(hrUpdate) + " " + callback->trace();
  callback->Release();
  outStream->Release();  // close the temp handle before verification re-opens it

  if (FAILED(hrUpdate)) {
    std::string why = "UpdateItems failed (see trace)";
    if (!cbReason.empty()) why += ": " + cbReason;
    return failWithHandler(why);
  }
  if (!cbReason.empty()) return failWithHandler("UpdateItems reported a problem: " + cbReason);
  if (out.bytesWritten <= 0) {
    return failWithHandler("UpdateItems wrote 0 bytes; a valid archive always has a header");
  }

  // ---- verify: re-open and compare against the plan ----------------------
  // This is the gate that makes a "successful" create trustworthy. A 7z commit
  // can return S_OK while a node landed as the wrong KIND (the mkdir matrix
  // caught exactly that), so kind, size and payload are all compared.
  {
    VerifyReader reader;
    if (!reader.Open(library, tempPath, req.password)) {
      return failWithHandler("temp archive failed re-open validation (" + reader.trace() + ")");
    }
    const std::vector<VerifyItem> produced = reader.Catalog();
    if (produced.size() != items.size()) {
      reader.Close();
      return failWithHandler("validation: archive holds " + std::to_string(produced.size()) +
                             " items, plan promised " + std::to_string(items.size()));
    }
    // Order is NOT part of the contract (CONTRACTS A7: 7z reorders solid
    // blocks), so compare as a set keyed by path.
    std::vector<VerifyItem> want;
    want.reserve(items.size());
    for (const auto& item : items) {
      VerifyItem v;
      v.path = item.path;
      v.isDir = item.isDir;
      v.size = item.size;
      want.push_back(std::move(v));
    }
    const auto less = [](const VerifyItem& x, const VerifyItem& y) { return x.path < y.path; };
    (std::sort)(want.begin(), want.end(), less);
    std::vector<VerifyItem> got = produced;
    (std::sort)(got.begin(), got.end(), less);
    for (size_t i = 0; i < got.size(); ++i) {
      if (got[i].path != want[i].path) {
        const std::string why = "validation: entry '" + got[i].path + "' where plan promised '" +
                                want[i].path + "'";
        reader.Close();
        return failWithHandler(why);
      }
      if (got[i].isDir != want[i].isDir) {
        const std::string why =
            "validation: '" + want[i].path + "' landed as a " +
            (got[i].isDir ? "directory" : "file") + " but the plan says " +
            (want[i].isDir ? "directory" : "file");
        reader.Close();
        return failWithHandler(why);
      }
      if (!want[i].isDir && got[i].size != want[i].size) {
        const std::string why = "validation: '" + want[i].path + "' holds " +
                                std::to_string(got[i].size) + " bytes, plan promised " +
                                std::to_string(want[i].size);
        reader.Close();
        return failWithHandler(why);
      }
      ++out.verifiedItems;
    }
    // Payload bytes: extract every file back out and compare with the source.
    // A size-equal but content-different entry is the silent corruption case.
    for (abi::UInt32 i = 0; i < produced.size(); ++i) {
      if (produced[i].isDir) continue;
      std::vector<uint8_t> archived;
      if (!reader.ExtractTo(i, produced[i].size, &archived)) {
        reader.Close();
        return failWithHandler("validation: cannot re-extract '" + produced[i].path + "'");
      }
      std::wstring sourcePath = req.sourceRoot;
      if (!sourcePath.empty() && sourcePath.back() != L'\\' && sourcePath.back() != L'/') {
        sourcePath.push_back(L'\\');
      }
      sourcePath += Utf8ToWide(produced[i].path);
      std::vector<uint8_t> onDisk;
      if (!ReadWholeFile(sourcePath, &onDisk)) {
        reader.Close();
        return failWithHandler("validation: cannot read source '" + produced[i].path + "'");
      }
      if (archived != onDisk) {
        char buf[192];
        std::snprintf(buf, sizeof(buf),
                      "validation: payload mismatch for '%s' (archive=%lluB disk=%lluB)",
                      produced[i].path.c_str(),
                      static_cast<unsigned long long>(archived.size()),
                      static_cast<unsigned long long>(onDisk.size()));
        reader.Close();
        return failWithHandler(buf);
      }
    }
    out.verified = true;
    reader.Close();
  }

  // ---- publish ------------------------------------------------------------
  outArchive->Release();
  const HRESULT moved = MoveFileExW(ToBackslashes(tempPath).c_str(),
                                   ToBackslashes(req.archivePath).c_str(),
                                   MOVEFILE_REPLACE_EXISTING);
  if (!moved) {
    const DWORD err = GetLastError();
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "MoveFileExW publish failed err=%lu -- temp removed, target untouched",
                  static_cast<unsigned long>(err));
    return fail(buf);
  }
  guard.armed = false;  // the temp BECAME the target

  out.ok = true;
  out.elapsedMs = elapsedMs();
  return out;
}

}  // namespace prism::win::cw
