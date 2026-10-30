// PrismArchive -- PrismWin
// Registration-free 7z.dll bridge (blueprint ch. 2.2 / ch. 6.1).
//
// Loads 7z.dll with LoadLibraryW (no registry, no side-by-side manifest),
// drives IInArchive through the raw vtables declared in SevenZipAbi.hpp and
// exposes two sinks for the extraction stream:
//   1. MemoryOutStream -- thumbnails / inline preview, zero disk I/O
//   2. DiskOutStream   -- final physical file, NT long-path aware
// A third sink (IStream pipe for zero-copy drag & drop) lives in
// PrismVirtualDragDrop.hpp.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../PrismCore/PrismCore.hpp"
#include "SevenZipAbi.hpp"

namespace prism::win {

// ---------------------------------------------------------------------------
// Text conversion helpers
// ---------------------------------------------------------------------------
inline std::wstring Utf8ToWide(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int needed =
      MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), needed);
  return out;
}

inline std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

// ---------------------------------------------------------------------------
// 7z.dll loader
// ---------------------------------------------------------------------------
class SevenZipLibrary {
 public:
  SevenZipLibrary() = default;
  ~SevenZipLibrary() {
    if (module_) FreeLibrary(module_);
  }
  SevenZipLibrary(const SevenZipLibrary&) = delete;
  SevenZipLibrary& operator=(const SevenZipLibrary&) = delete;

  bool Load(const std::wstring& dllPath) {
    module_ = LoadLibraryW(dllPath.c_str());
    if (!module_) return false;
    createObject_ =
        reinterpret_cast<abi::FnCreateObject>(GetProcAddress(module_, "CreateObject"));
    getNumberOfFormats_ = reinterpret_cast<abi::FnGetNumberOfFormats>(
        GetProcAddress(module_, "GetNumberOfFormats"));
    getHandlerProperty2_ = reinterpret_cast<abi::FnGetHandlerProperty2>(
        GetProcAddress(module_, "GetHandlerProperty2"));
    setLargePageMode_ =
        reinterpret_cast<abi::FnSetLargePageMode>(GetProcAddress(module_, "SetLargePageMode"));
    if (createObject_) path_ = dllPath;
    return createObject_ != nullptr;
  }

  bool loaded() const { return module_ != nullptr && createObject_ != nullptr; }
  const std::wstring& path() const { return path_; }

  // Blueprint ch. 7.2: 2 MB large pages cut TLB misses on big LZMA2 dictionaries.
  void EnableLargePages() {
    if (setLargePageMode_) setLargePageMode_();
  }

  HRESULT CreateObject(const GUID& clsid, const GUID& iid, void** out) const {
    if (!createObject_) return E_NOTIMPL;
    return createObject_(&clsid, &iid, out);
  }

  std::optional<uint32_t> FormatCount() const {
    if (!getNumberOfFormats_) return std::nullopt;
    abi::UInt32 count = 0;
    if (FAILED(getNumberOfFormats_(&count))) return std::nullopt;
    return count;
  }

 private:
  HMODULE module_ = nullptr;
  std::wstring path_;
  abi::FnCreateObject createObject_ = nullptr;
  abi::FnGetNumberOfFormats getNumberOfFormats_ = nullptr;
  abi::FnGetHandlerProperty2 getHandlerProperty2_ = nullptr;
  abi::FnSetLargePageMode setLargePageMode_ = nullptr;
};

// ---------------------------------------------------------------------------
// Input stream over a real file (IInStream + IStreamGetSize)
// ---------------------------------------------------------------------------
class InFileStream final : public abi::IInStream, public abi::IStreamGetSize {
 public:
  explicit InFileStream(const std::wstring& path) {
    handle_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size{};
      if (GetFileSizeEx(handle_, &size)) size_ = static_cast<uint64_t>(size.QuadPart);
    }
  }
  ~InFileStream() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  uint64_t size() const { return size_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream || iid == abi::IID_IInStream) {
      *out = static_cast<abi::IInStream*>(this);
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

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processed) override {
    DWORD read = 0;
    if (!ReadFile(handle_, data, size, &read, nullptr)) {
      if (processed) *processed = 0;
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (processed) *processed = read;
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 origin, abi::UInt64* newPos) override {
    LARGE_INTEGER move{};
    move.QuadPart = offset;
    DWORD method = FILE_BEGIN;
    if (origin == 1) method = FILE_CURRENT;
    if (origin == 2) method = FILE_END;
    if (!SetFilePointerEx(handle_, move, &move, method)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (newPos) *newPos = static_cast<abi::UInt64>(move.QuadPart);
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
// Sink 1: bounded in-memory sink (thumbnails / inline preview)
// ---------------------------------------------------------------------------
class MemoryOutStream final : public abi::ISequentialOutStream {
 public:
  explicit MemoryOutStream(size_t capacity) : capacity_(capacity) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream) {
      *out = static_cast<abi::ISequentialOutStream*>(this);
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

  // Zip-bomb containment: refuse to grow past the caller's budget instead of
  // letting a 42.zip exhaust the address space (blueprint ch. 7.1).
  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    if (bytes_.size() + size > capacity_) {
      tripped_ = true;
      return E_OUTOFMEMORY;
    }
    const auto* p = static_cast<const uint8_t*>(data);
    bytes_.insert(bytes_.end(), p, p + size);
    if (processed) *processed = size;
    return S_OK;
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  bool tripped() const { return tripped_; }

 private:
  size_t capacity_;
  std::vector<uint8_t> bytes_;
  bool tripped_ = false;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Sink 2: final physical file (NT long-path aware, \\?\ prefix)
// ---------------------------------------------------------------------------
class DiskOutStream final : public abi::ISequentialOutStream {
 public:
  explicit DiskOutStream(std::wstring finalPath) : path_(std::move(finalPath)) {}

  bool Open() {
    // Blueprint ch. 7.2: \\?\ lifts the MAX_PATH ceiling and allows the Linux
    // reserved names (aux.c, con.txt) that 7-Zip otherwise chokes on.
    const std::wstring ntPath = L"\\\\?\\" + path_;
    handle_ = CreateFileW(ntPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return handle_ != INVALID_HANDLE_VALUE;
  }

  uint64_t written() const { return written_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream) {
      *out = static_cast<abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) {
      if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
      delete this;
    }
    return r;
  }

  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (handle_ == INVALID_HANDLE_VALUE) return E_FAIL;
    DWORD done = 0;
    if (!WriteFile(handle_, data, size, &done, nullptr)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    written_ += done;
    if (processed) *processed = done;
    return S_OK;
  }

 private:
  std::wstring path_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  uint64_t written_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Open callback (also doubles as the password provider)
// ---------------------------------------------------------------------------
class OpenCallback final : public abi::IArchiveOpenCallback, public abi::ICryptoGetTextPassword {
 public:
  explicit OpenCallback(std::wstring password = {}) : password_(std::move(password)) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || IsOpenCallbackIid(iid)) {
      *out = static_cast<abi::IArchiveOpenCallback*>(this);
    } else if (iid == abi::IID_ICryptoGetTextPassword) {
      *out = static_cast<abi::ICryptoGetTextPassword*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }

  // 7-Zip families its IIDs as {23170F69-40C1-278A-0000-00KK00MM0000}. We accept
  // the whole 0x05 (callback) family minus the extract-callback and password
  // slots, so a single wrong IID constant can never break Open() silently.
  static bool IsOpenCallbackIid(REFIID iid) {
    if (iid == abi::IID_IArchiveOpenCallback) return true;
    if (iid.Data1 != 0x23170F69 || iid.Data2 != 0x40C1 || iid.Data3 != 0x278A) return false;
    return iid.Data4[3] == 0x05 && iid.Data4[5] != 0x06 && iid.Data4[5] != 0x10;
  }

  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  // IProgress
  STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }
  // IArchiveOpenCallback (two-pointer variants: separate vtable slots)
  STDMETHODIMP SetTotal(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }

  STDMETHODIMP CryptoGetTextPassword(BSTR* password) override {
    if (!password) return E_POINTER;
    if (password_.empty()) return E_ABORT;  // unknown -> let the UI prompt
    *password = SysAllocString(password_.c_str());
    return *password ? S_OK : E_OUTOFMEMORY;
  }

 private:
  std::wstring password_;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// Extract callback: smart path rewriting + Zip Slip guard + bomb fuse
// ---------------------------------------------------------------------------
class SmartExtractCallback final : public abi::IArchiveExtractCallback {
 public:
  struct Result {
    uint64_t filesWritten = 0;
    uint64_t bytesWritten = 0;
    bool guardTripped = false;
    bool fuseTripped = false;
    std::string guardReason;
    std::string fuseReason;
    std::vector<std::string> writtenPaths;
  };

  SmartExtractCallback(const std::vector<ArchiveItem>& catalog, const SmartPlan& plan,
                       const std::string& targetRoot, BombFuse* fuse = nullptr)
      : catalog_(catalog), plan_(plan), guard_(targetRoot), fuse_(fuse) {}

  const Result& result() const { return result_; }
  void SetFreeDiskBytes(uint64_t freeBytes) { freeDiskBytes_ = freeBytes; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
      *out = static_cast<abi::IArchiveExtractCallback*>(this);
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

  STDMETHODIMP SetTotal(abi::UInt64 total) override {
    total_ = total;
    return S_OK;
  }

  STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
    if (completeValue && fuse_) {
      // The fuse is a sliding-window monitor, so it must be fed deltas.
      const uint64_t produced = *completeValue;
      const uint64_t deltaPacked = packedSoFar_ - fedPacked_;
      const uint64_t deltaProduced = produced > fedProduced_ ? produced - fedProduced_ : 0;
      fedPacked_ = packedSoFar_;
      fedProduced_ = produced;
      if (!fuse_->Feed(deltaPacked, deltaProduced, freeDiskBytes_)) {
        result_.fuseTripped = true;
        result_.fuseReason = fuse_->reason();
        return E_ABORT;
      }
    }
    return S_OK;
  }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialOutStream** outStream,
                         abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != abi::kExtract) return S_OK;
    if (index >= catalog_.size()) return E_INVALIDARG;

    const ArchiveItem& item = catalog_[index];
    if (item.isDir) {
      currentPath_.clear();
      return S_OK;
    }
    if (plan_.ignoredIndices.count(item.index)) return S_OK;  // Mac junk / promo

    // PrismCore decides the final path; the Zip Slip guard re-checks it after
    // rewriting (blueprint ch. 7.1 defence #2, two-stage validation).
    const std::string finalPath = SmartExtractionPlanner::TransformItemPath(plan_, item.path);
    if (!guard_.Approve(finalPath)) {
      result_.guardTripped = true;
      result_.guardReason = "blocked out-of-root write: " + finalPath;
      return E_ABORT;
    }

    packedSoFar_ += item.packSize ? item.packSize : 1;
    const std::wstring wide = Utf8ToWide(finalPath);
    CreateParentDirectories(wide);
    auto* sink = new DiskOutStream(wide);
    if (!sink->Open()) {
      delete sink;
      result_.guardTripped = true;
      result_.guardReason = "cannot create " + finalPath;
      return E_ABORT;
    }
    currentPath_ = finalPath;
    currentSink_ = sink;
    *outStream = sink;
    return S_OK;
  }

  STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }

  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
    if (operationResult == 0 && !currentPath_.empty()) {
      ++result_.filesWritten;
      result_.writtenPaths.push_back(currentPath_);
      if (currentSink_) result_.bytesWritten += currentSink_->written();
    }
    currentSink_ = nullptr;
    currentPath_.clear();
    return S_OK;
  }

 private:
  static void CreateParentDirectories(const std::wstring& fullPath) {
    for (size_t i = 0; i < fullPath.size(); ++i) {
      if (fullPath[i] != L'\\' && fullPath[i] != L'/') continue;
      if (i < 3) continue;  // skip "C:\"
      const std::wstring part = fullPath.substr(0, i);
      CreateDirectoryW((L"\\\\?\\" + part).c_str(), nullptr);
    }
  }

  const std::vector<ArchiveItem>& catalog_;
  SmartPlan plan_;
  WriteGuard guard_;
  BombFuse* fuse_ = nullptr;
  Result result_;
  std::string currentPath_;
  DiskOutStream* currentSink_ = nullptr;
  uint64_t packedSoFar_ = 0;
  uint64_t fedPacked_ = 0;
  uint64_t fedProduced_ = 0;
  uint64_t freeDiskBytes_ = 0;
  abi::UInt64 total_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// ArchiveReader: central-directory snapshot + the two extraction entry points
// ---------------------------------------------------------------------------
class ArchiveReader {
 public:
  ArchiveReader(SevenZipLibrary& library, ITranscoder* transcoder = nullptr)
      : library_(library), transcoder_(transcoder) {}

  ~ArchiveReader() {
    if (archive_) {
      archive_->Close();
      archive_->Release();
    }
    if (stream_) stream_->Release();
  }

  bool Open(const std::wstring& archivePath, const std::wstring& password = {}) {
    stream_ = new InFileStream(archivePath);
    if (!stream_->valid()) return false;

    // Try candidate handlers in order: no registry, no sniffing table needed --
    // the handler itself rejects anything it cannot parse.
    for (const GUID* clsid : {&abi::CLSID_CFormat7z, &abi::CLSID_CFormatZip}) {
      abi::IInArchive* candidate = nullptr;
      if (FAILED(library_.CreateObject(*clsid, abi::IID_IInArchive,
                                       reinterpret_cast<void**>(&candidate))) ||
          !candidate) {
        continue;
      }
      auto* callback = new OpenCallback(password);
      const HRESULT hr = candidate->Open(stream_, nullptr, callback);
      callback->Release();
      if (SUCCEEDED(hr)) {
        archive_ = candidate;
        DiscoverPropertyIds();
        return true;
      }
      candidate->Release();
    }
    return false;
  }

  bool opened() const { return archive_ != nullptr; }

  // Blueprint ch. 2.2: a 50 000-entry directory tree in about a millisecond,
  // with zero payload decompressed.
  std::vector<ArchiveItem> ReadCatalog() const {
    std::vector<ArchiveItem> items;
    if (!archive_) return items;
    abi::UInt32 count = 0;
    if (FAILED(archive_->GetNumberOfItems(&count))) return items;

    const PROPID pidPath = PropertyId("Path", abi::kpidPath);
    const PROPID pidIsDir = PropertyId("IsDir", abi::kpidIsDir);
    const PROPID pidSize = PropertyId("Size", abi::kpidSize);
    const PROPID pidPack = PropertyId("PackSize", abi::kpidPackSize);
    const PROPID pidMTime = PropertyId("MTime", abi::kpidMTime);
    const PROPID pidEnc = PropertyId("Encrypted", abi::kpidEncrypted);
    const PROPID pidBlock = PropertyId("Block", 0);

    items.reserve(count);
    for (abi::UInt32 i = 0; i < count; ++i) {
      ArchiveItem item;
      item.index = i;
      item.path = GetString(i, pidPath);
      item.isDir = GetBool(i, pidIsDir);
      item.size = GetUInt64(i, pidSize);
      item.packSize = GetUInt64(i, pidPack);
      item.mtime = static_cast<int64_t>(GetUInt64(i, pidMTime));
      item.encrypted = GetBool(i, pidEnc);
      if (pidBlock != 0) item.block = static_cast<uint32_t>(GetUInt64(i, pidBlock));
      if (transcoder_) {
        // Legacy CJK names are repaired here, at the single place where raw
        // archive bytes enter the product (blueprint ch. 7.1 defence #4).
        const auto repaired = RepairArchiveName(item.path, false, transcoder_);
        item.path = repaired.name;
      }
      items.push_back(std::move(item));
    }
    return items;
  }

  // Sink 1: thumbnail / preview extraction under a hard memory budget.
  std::optional<std::vector<uint8_t>> ExtractToMemory(
      uint32_t index, size_t maxBytes = 16ull * 1024 * 1024) const {
    if (!archive_) return std::nullopt;
    auto* sink = new MemoryOutStream(maxBytes);
    auto* callback = new MemoryExtractCallback(sink);
    const abi::UInt32 idx = index;
    const HRESULT hr = archive_->Extract(&idx, 1, 0, callback);
    const bool tripped = sink->tripped();
    std::vector<uint8_t> out = sink->bytes();
    callback->Release();
    sink->Release();
    if (FAILED(hr) || tripped) return std::nullopt;
    return out;
  }

  // Sink 2: full smart extraction straight onto the final paths -- no %TEMP%
  // detour, no post-extraction MoveFile (blueprint ch. 4.2).
  bool ExtractWithPlan(const std::vector<ArchiveItem>& catalog, const SmartPlan& plan,
                       const std::string& targetRoot, BombFuse* fuse,
                       SmartExtractCallback::Result* resultOut) const {
    if (!archive_) return false;
    std::vector<abi::UInt32> indices;
    for (const auto& item : catalog) {
      if (item.isDir) continue;
      if (plan.ignoredIndices.count(item.index)) continue;
      indices.push_back(item.index);
    }
    if (indices.empty()) return true;
    auto* callback = new SmartExtractCallback(catalog, plan, targetRoot, fuse);
    const HRESULT hr =
        archive_->Extract(indices.data(), static_cast<abi::UInt32>(indices.size()), 0, callback);
    if (resultOut) *resultOut = callback->result();
    const bool ok = SUCCEEDED(hr) && !callback->result().guardTripped &&
                    !callback->result().fuseTripped;
    callback->Release();
    return ok;
  }

  // Property ids are discovered by name (authoritative, read out of the DLL
  // itself) and only fall back to the hard-coded enum -- guessing PROPIDs is
  // exactly how ABI drift starts.
  PROPID PropertyId(const std::string& name, PROPID fallback) const {
    const auto it = propIds_.find(name);
    if (it != propIds_.end()) return it->second;
    return fallback;
  }

  const std::map<std::string, PROPID>& discoveredProperties() const { return propIds_; }

 private:
  class MemoryExtractCallback final : public abi::IArchiveExtractCallback {
   public:
    explicit MemoryExtractCallback(MemoryOutStream* sink) : sink_(sink) { sink_->AddRef(); }
    ~MemoryExtractCallback() {
      if (sink_) sink_->Release();
    }
    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
      if (!out) return E_POINTER;
      if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
        *out = static_cast<abi::IArchiveExtractCallback*>(this);
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
                           abi::Int32 mode) override {
      if (!out) return E_POINTER;
      *out = nullptr;
      if (mode == abi::kExtract) {
        *out = sink_;
        sink_->AddRef();
      }
      return S_OK;
    }
    STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }
    STDMETHODIMP SetOperationResult(abi::Int32) override { return S_OK; }

   private:
    MemoryOutStream* sink_ = nullptr;
    LONG refs_ = 1;
  };

  void DiscoverPropertyIds() {
    abi::UInt32 count = 0;
    if (!archive_ || FAILED(archive_->GetNumberOfProperties(&count))) return;
    for (abi::UInt32 i = 0; i < count; ++i) {
      BSTR name = nullptr;
      PROPID propId = 0;
      VARTYPE vt = VT_EMPTY;
      if (SUCCEEDED(archive_->GetPropertyInfo(i, &name, &propId, &vt)) && name) {
        propIds_[WideToUtf8(name)] = propId;
        SysFreeString(name);
      }
    }
  }

  std::string GetString(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return {};
    PROPVARIANT value{};
    PropVariantInit(&value);
    std::string out;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      if (value.vt == VT_BSTR && value.bstrVal) {
        out = WideToUtf8(value.bstrVal);
      } else if (value.vt == VT_LPWSTR && value.pwszVal) {
        out = WideToUtf8(value.pwszVal);
      }
    }
    PropVariantClear(&value);
    return out;
  }

  uint64_t GetUInt64(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return 0;
    PROPVARIANT value{};
    PropVariantInit(&value);
    uint64_t out = 0;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      switch (value.vt) {
        case VT_UI8: out = value.uhVal.QuadPart; break;
        case VT_UI4: out = value.ulVal; break;
        case VT_UI2: out = value.uiVal; break;
        case VT_I8: out = static_cast<uint64_t>(value.hVal.QuadPart); break;
        case VT_I4: out = static_cast<uint64_t>(value.lVal); break;
        case VT_FILETIME:
          out = (static_cast<uint64_t>(value.filetime.dwHighDateTime) << 32) |
                value.filetime.dwLowDateTime;
          break;
        default: break;
      }
    }
    PropVariantClear(&value);
    return out;
  }

  bool GetBool(abi::UInt32 index, PROPID pid) const {
    if (!archive_ || pid == 0) return false;
    PROPVARIANT value{};
    PropVariantInit(&value);
    bool out = false;
    if (SUCCEEDED(archive_->GetProperty(index, pid, &value))) {
      if (value.vt == VT_BOOL) out = value.boolVal != VARIANT_FALSE;
    }
    PropVariantClear(&value);
    return out;
  }

  SevenZipLibrary& library_;
  ITranscoder* transcoder_ = nullptr;
  InFileStream* stream_ = nullptr;
  abi::IInArchive* archive_ = nullptr;
  std::map<std::string, PROPID> propIds_;
};

}  // namespace prism::win
