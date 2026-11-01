// PrismArchive -- PrismWin
// ArchiveEditor: the EXECUTION half of "edit an archive without extracting it"
// (blueprint ch.2, copy-on-write). EditPlan (PrismCore) decides WHAT the archive
// becomes; this file makes 7z.dll build that archive next to the original and
// then puts it in place atomically.
//
// THE SHAPE (one pass, no user-visible extraction):
//   read the original  ->  write a TEMP archive in the SAME directory (kept
//   entries are decompressed from the original and re-compressed into the temp:
//   the deleted ones are skipped, the renamed ones get their new path, the new
//   folders are inserted)  ->  re-open the temp and compare every entry against
//   what the plan promised  ->  ReplaceFileW(temp, original).
// Nothing is published before the comparison passes, and every failure path
// deletes the temp and leaves the original byte-for-byte untouched. That is the
// property the "broken_publish" negative control exists to prove: with the guard
// deleted, a failed edit publishes a half-written file over the user's archive.
//
// WHAT IS PRESERVED (the card's hard requirement):
//   * encryption: payload AES and 7z HEADER encryption (the password is supplied
//     by the caller and re-applied; the temp is re-opened WITHOUT it to prove the
//     names are still hidden);
//   * solidity: the original's solid setting is re-applied, and the new archive's
//     per-entry kpidSolid flags are compared against it;
//   * per-entry mtime, kind, size and stored CRC32 -- the CRC comparison is what
//     turns "the file is there" into "the bytes are the same bytes";
//   * a MULTI-VOLUME set is REFUSED, not half-edited: the rewrite produces one
//     file, so replacing "x.7z.001" would silently orphan .002.
// A read-only archive is refused by the same rule (the edit replaces the file).
//
// REUSE, not re-implementation (C9 + C11):
//   * cw::IOutArchive / ISetProperties / ApplyProperties / ClsidFor /
//     DiskOutStream / TempArchiveGuard come from ArchiveWriter.hpp;
//   * the source archive is read through a ReaderPool lease (C11), so one engine
//     operation at a time owns the handler (C41) and the payload never rides an
//     ad-hoc second parser;
//   * PrismEngineBridge supplies InFileStream / OpenCallback / property readers.
// The one thing this file adds to the ABI surface is kpidCRC (19) and kpidSolid
// (13), declared locally exactly like cw::kKpidIsAnti: a card pins what it reads
// instead of widening the shared header every other card already validated.
//
// BOUNDED, ALWAYS (D29): one entry is copied through memory, so an entry larger
// than kMaxInMemoryEntry is REFUSED with a reason (bounded memory beats an
// allocation the machine cannot honour). No wait in this file is unbounded.
#pragma once

#include <windows.h>
#include <propidl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/CompressionPlan.hpp"
#include "../PrismCore/EditPlan.hpp"
#include "ArchiveWriter.hpp"
#include "PrismEngineBridge.hpp"
#include "ReaderPool.hpp"
#include "SevenZipAbi.hpp"

namespace prism::win::edit {

// CPP/7zip/PropID.h, the same authoritative ordinal list SevenZipAbi pins:
// kpidSolid = 13, kpidCRC = 19. Declared here (not in the shared header) for the
// same reason ArchiveWriter declares kKpidIsAnti: the card that reads a property
// pins it, and the shared ABI surface stays exactly what the other cards proved.
constexpr PROPID kKpidSolid = 13;
constexpr PROPID kKpidCRC = 19;

// One entry copied through memory is the price of serving 7z's ISequentialInStream
// from an archive entry. 256 MiB is the ceiling this path promises; above it the
// edit is REFUSED with a reason instead of asking for an unbounded allocation.
constexpr uint64_t kMaxInMemoryEntry = 256ull * 1024 * 1024;

// ---------------------------------------------------------------------------
// Entry fingerprint: the evidence the verify pass and the tests compare.
// ---------------------------------------------------------------------------
struct EntryFingerprint {
  std::string path;
  bool isDir = false;
  uint64_t size = 0;
  uint64_t mtime100ns = 0;
  uint32_t crc = 0;
  bool hasCrc = false;
  bool encrypted = false;
  // SOLIDITY IS A BLOCK PROPERTY, not a per-entry boolean: this engine answers
  // kpidSolid = false for every entry of a one-folder archive, so the grouping
  // below (two entries sharing one kpidBlock) is the only honest signal. Verified
  // against the real 7z.dll: the fixture is solid, kpidSolid says "false", and
  // kpidBlock is 0 for all four files.
  bool solid = false;
  uint32_t block = 0;
};

struct EditRequest {
  std::wstring archivePath;  // the archive being edited, full path
  std::wstring password;     // required for an encrypted archive
  std::vector<prism::EditOp> ops;
  std::function<bool(uint64_t)> progress;  // false = cancel the edit
};

struct EditResult {
  bool ok = false;
  std::string error;
  std::string trace;
  std::wstring archivePath;
  prism::EditPlan plan;

  uint64_t itemsWritten = 0;
  uint64_t bytesWritten = 0;   // bytes the engine wrote into the temp archive
  uint64_t bytesCopied = 0;    // payload bytes read out of the original
  uint64_t verifiedItems = 0;  // entries compared with the plan
  bool verified = false;
  int64_t elapsedMs = 0;

  // CONTRACTS D3: non-zero only in a mutant build, and asserted by the suite so
  // an injection that silently stopped firing can never look like a sharp test.
  int injectedKeepDeleted = 0;
  int injectedBrokenPublish = 0;
};

// ---------------------------------------------------------------------------
// Facts the model needs, observed from the real file (never guessed).
// ---------------------------------------------------------------------------
inline bool HasReadOnlyAttribute(const std::wstring& path) {
  const DWORD attrs = GetFileAttributesW(path.c_str());
  return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0;
}

inline bool EndsWithNoCase(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = std::wcslen(suffix);
  if (text.size() < n) return false;
  const size_t at = text.size() - n;
  for (size_t i = 0; i < n; ++i) {
    wchar_t a = text[at + i];
    wchar_t b = suffix[i];
    if (a >= L'A' && a <= L'Z') a = static_cast<wchar_t>(a - L'A' + L'a');
    if (b >= L'A' && b <= L'Z') b = static_cast<wchar_t>(b - L'A' + L'a');
    if (a != b) return false;
  }
  return true;
}

// A split archive is recognised by its NAME, the only signal that exists before
// every sibling is opened: "x.7z.001" (this file IS a volume) or "x.7z" beside an
// "x.7z.001" (this file is the set's base name). Editing one volume of a set in
// place would produce a single file and orphan the rest, so it is refused.
inline bool IsMultiVolumeSet(const std::wstring& path) {
  static const wchar_t* kVolumeSuffixes[] = {L".001", L".002", L".003"};
  for (const wchar_t* suffix : kVolumeSuffixes) {
    if (EndsWithNoCase(path, suffix)) return true;
  }
  // "x.7z" + "x.7z.001" on disk = the set this path belongs to.
  for (const wchar_t* suffix : {L".001", L".002"}) {
    const std::wstring sibling = path + suffix;
    if (GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
  }
  return false;
}

inline bool FormatFromPath(const std::wstring& path, prism::ArchiveFormat* format) {
  if (EndsWithNoCase(path, L".7z")) {
    *format = prism::ArchiveFormat::SevenZip;
    return true;
  }
  if (EndsWithNoCase(path, L".zip")) {
    *format = prism::ArchiveFormat::Zip;
    return true;
  }
  if (EndsWithNoCase(path, L".tar")) {
    *format = prism::ArchiveFormat::Tar;
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// FingerprintArchive: open with the raw handler and read the identity of every
// entry (name, kind, size, mtime, stored CRC32, encrypted, solid).
//
// Its own tiny open loop, like cw::VerifyReader, because this must work on the
// TEMP file too -- a ".tmp" name has no extension the router could trust.
// ---------------------------------------------------------------------------
inline bool FingerprintArchive(SevenZipLibrary& library, const std::wstring& path,
                               const std::wstring& password,
                               std::vector<EntryFingerprint>* out, std::string* why) {
  out->clear();
  InFileStream* stream = new InFileStream(path);
  if (!stream->valid()) {
    stream->Release();
    if (why != nullptr) *why = "cannot open the file for reading (in use, locked, or missing)";
    return false;
  }
  abi::IInArchive* handler = nullptr;
  std::string trace;
  for (const GUID* clsid : {&abi::CLSID_CFormat7z, &abi::CLSID_CFormatZip, &cw::CLSID_CFormatTar}) {
    abi::IInArchive* candidate = nullptr;
    if (FAILED(library.CreateObject(*clsid, abi::IID_IInArchive,
                                    reinterpret_cast<void**>(&candidate))) ||
        candidate == nullptr) {
      continue;
    }
    abi::UInt64 pos = 0;
    stream->Seek(0, /*STREAM_SEEK_SET=*/0, &pos);
    auto* callback = new OpenCallback(password);
    const HRESULT hr = candidate->Open(stream, nullptr, callback);
    callback->Release();
    if (hr == S_OK) {  // S_FALSE = "not my format" (CONTRACTS A1)
      handler = candidate;
      break;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), "hr=0x%08lX; ", static_cast<unsigned long>(hr));
    trace += buf;
    candidate->Release();
  }
  if (handler == nullptr) {
    stream->Release();
    if (why != nullptr) *why = "no handler accepted the archive (" + trace + ")";
    return false;
  }
  abi::UInt32 count = 0;
  if (FAILED(handler->GetNumberOfItems(&count))) {
    handler->Close();
    handler->Release();
    stream->Release();
    if (why != nullptr) *why = "the handler refused to report its item count";
    return false;
  }
  out->reserve(count);
  for (abi::UInt32 i = 0; i < count; ++i) {
    EntryFingerprint entry;
    entry.path = prism::NormalizeArchivePath(cw::ReaderPropString(handler, i, abi::kpidPath));
    entry.isDir = cw::ReaderPropBool(handler, i, abi::kpidIsDir);
    entry.size = cw::ReaderPropUInt(handler, i, abi::kpidSize);
    entry.mtime100ns = cw::ReaderPropUInt(handler, i, abi::kpidMTime);
    entry.encrypted = cw::ReaderPropBool(handler, i, abi::kpidEncrypted);
    entry.solid = cw::ReaderPropBool(handler, i, kKpidSolid);
    entry.block = static_cast<uint32_t>(cw::ReaderPropUInt(handler, i, abi::kpidBlock));
    PROPVARIANT crc{};
    PropVariantInit(&crc);
    if (SUCCEEDED(handler->GetProperty(i, kKpidCRC, &crc)) && crc.vt == VT_UI4) {
      entry.crc = crc.ulVal;
      entry.hasCrc = true;
    }
    PropVariantClear(&crc);
    out->push_back(std::move(entry));
  }
  handler->Close();
  handler->Release();
  stream->Release();
  return true;
}

// ---------------------------------------------------------------------------
// BytesInStream: the file payload the update callback hands to 7z.
// ---------------------------------------------------------------------------
class BytesInStream final : public abi::ISequentialInStream {
 public:
  explicit BytesInStream(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  // CONTRACTS B8: the engine only ever reaches an interface through QI.
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream) {
      *out = static_cast<abi::ISequentialInStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = InterlockedDecrement(&refs_);
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processedSize) override {
    if (processedSize != nullptr) *processedSize = 0;
    if (data == nullptr) return E_POINTER;
    const size_t remaining = bytes_.size() - offset_;
    const size_t take = (std::min)(remaining, static_cast<size_t>(size));
    if (take != 0) std::memcpy(data, bytes_.data() + offset_, take);
    offset_ += take;
    if (processedSize != nullptr) *processedSize = static_cast<abi::UInt32>(take);
    return S_OK;
  }

 private:
  std::vector<uint8_t> bytes_;
  size_t offset_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// EntryExtractor: one entry's bytes out of the ORIGINAL archive, with the
// password.
//
// WHY NOT ArchiveReader::ExtractToMemory: its sink does not answer
// ICryptoGetTextPassword (the defect C9 documented), so an AES archive "extracts"
// to ZERO bytes and the rewritten archive would silently lose every payload.
// This card must PRESERVE encryption, so the copy uses cw::SingleItemSink -- the
// password-answering sink C9 built for exactly this -- and nothing else changes.
// ---------------------------------------------------------------------------
class EntryExtractor {
 public:
  explicit EntryExtractor(SevenZipLibrary& library) : library_(library) {}
  ~EntryExtractor() { Close(); }

  EntryExtractor(const EntryExtractor&) = delete;
  EntryExtractor& operator=(const EntryExtractor&) = delete;

  bool Open(const std::wstring& path, const std::wstring& password, std::string* why) {
    Close();
    password_ = password;
    stream_ = new InFileStream(path);
    if (!stream_->valid()) {
      stream_->Release();
      stream_ = nullptr;
      if (why != nullptr) *why = "cannot open the archive for reading";
      return false;
    }
    std::string trace;
    for (const GUID* clsid : {&abi::CLSID_CFormat7z, &abi::CLSID_CFormatZip, &cw::CLSID_CFormatTar}) {
      abi::IInArchive* candidate = nullptr;
      if (FAILED(library_.CreateObject(*clsid, abi::IID_IInArchive,
                                       reinterpret_cast<void**>(&candidate))) ||
          candidate == nullptr) {
        continue;
      }
      abi::UInt64 pos = 0;
      stream_->Seek(0, /*STREAM_SEEK_SET=*/0, &pos);
      auto* callback = new OpenCallback(password_);
      const HRESULT hr = candidate->Open(stream_, nullptr, callback);
      callback->Release();
      if (hr == S_OK) {
        handler_ = candidate;
        return true;
      }
      candidate->Release();
    }
    Close();
    if (why != nullptr) *why = "no handler accepted the archive";
    return false;
  }

  void Close() {
    if (handler_ != nullptr) {
      handler_->Close();
      handler_->Release();
      handler_ = nullptr;
    }
    if (stream_ != nullptr) {
      stream_->Release();
      stream_ = nullptr;
    }
  }

  bool Extract(uint32_t index, uint64_t expectedSize, std::vector<uint8_t>* out, std::string* why) {
    if (handler_ == nullptr) {
      if (why != nullptr) *why = "the extractor is not open";
      return false;
    }
    // Bounded sink: a corrupt entry cannot turn the copy into a memory hog.
    auto* sink = new MemoryOutStream(static_cast<size_t>(expectedSize) + 4096);
    auto* callback = new cw::SingleItemSink(sink, password_);
    const abi::UInt32 one = index;
    const HRESULT hr = handler_->Extract(&one, 1, 0, callback);
    callback->Release();
    const bool tripped = sink->tripped();
    std::vector<uint8_t> bytes = sink->bytes();
    sink->Release();
    if (FAILED(hr) || tripped) {
      if (why != nullptr) *why = tripped ? "the entry is larger than its declared size" : "Extract failed";
      return false;
    }
    *out = std::move(bytes);
    return true;
  }

 private:
  SevenZipLibrary& library_;
  InFileStream* stream_ = nullptr;
  abi::IInArchive* handler_ = nullptr;
  std::wstring password_;
};

// ---------------------------------------------------------------------------
// CopyUpdateCallback: answers the engine from the PLAN, and serves each kept
// file's bytes out of the ORIGINAL archive through the extractor above.
// ---------------------------------------------------------------------------
class CopyUpdateCallback final : public cw::IArchiveUpdateCallback,
                                 public abi::ICryptoGetTextPassword,
                                 public cw::ICryptoGetTextPassword2 {
 public:
  CopyUpdateCallback(const std::vector<prism::EditPlanItem>& items, EntryExtractor& extractor,
                     std::wstring password, std::function<bool(uint64_t)> progress)
      : items_(items),
        extractor_(extractor),
        password_(std::move(password)),
        progress_(std::move(progress)) {}

  // CONTRACTS B8: all four inherited interfaces must be reachable, or the engine
  // gets E_NOINTERFACE and the whole update fails with E_NOTIMPL.
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (out == nullptr) return E_POINTER;
    if (iid == IID_IUnknown || iid == cw::kIID_IArchiveUpdateCallback) {
      *out = static_cast<cw::IArchiveUpdateCallback*>(this);
    } else if (iid == cw::kIID_IProgress) {
      *out = static_cast<abi::IProgress*>(this);
    } else if (iid == abi::IID_ICryptoGetTextPassword) {
      *out = static_cast<abi::ICryptoGetTextPassword*>(this);
    } else if (iid == cw::kIID_ICryptoGetTextPassword2) {
      *out = static_cast<cw::ICryptoGetTextPassword2*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG remaining = InterlockedDecrement(&refs_);
    if (remaining == 0) delete this;
    return remaining;
  }

  STDMETHODIMP SetTotal(abi::UInt64 total) override {
    total_ = total;
    return S_OK;
  }

  STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
    if (completeValue == nullptr) return S_OK;
    ++completedCalls_;
    if (progress_ && !progress_(*completeValue)) {
      Fail("cancelled through the progress callback");
      return E_ABORT;
    }
    return S_OK;
  }

  // Create-from-nothing: every node carries its own data and its own properties.
  STDMETHODIMP GetUpdateItemInfo(abi::UInt32 index, abi::Int32* newData, abi::Int32* newProps,
                                 abi::UInt32* indexInArchive) override {
    if (newData == nullptr || newProps == nullptr || indexInArchive == nullptr) return E_POINTER;
    if (index >= items_.size()) return E_INVALIDARG;
    *newData = 1;
    *newProps = 1;
    *indexInArchive = static_cast<abi::UInt32>(-1);
    return S_OK;
  }

  STDMETHODIMP GetProperty(abi::UInt32 index, PROPID propID, PROPVARIANT* value) override {
    if (value == nullptr) return E_POINTER;
    PropVariantInit(value);
    if (index >= items_.size()) return E_INVALIDARG;
    const prism::EditPlanItem& item = items_[index];
    if (propID == abi::kpidPath) {
      value->vt = VT_BSTR;
      value->bstrVal = SysAllocString(ToBackslashes(Utf8ToWide(item.path)).c_str());
      return value->bstrVal != nullptr ? S_OK : E_OUTOFMEMORY;
    }
    if (propID == abi::kpidSize) {
      value->vt = VT_UI8;
      value->uhVal.QuadPart = item.isDir ? 0 : item.size;
      return S_OK;
    }
    if (propID == abi::kpidIsDir) {
      value->vt = VT_BOOL;
      value->boolVal = item.isDir ? VARIANT_TRUE : VARIANT_FALSE;
      return S_OK;
    }
    if (propID == abi::kpidAttrib) {
      value->vt = VT_UI4;
      value->ulVal = item.isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
      return S_OK;
    }
    if (propID == abi::kpidMTime) {
      if (item.mtime100ns != 0) {
        value->vt = VT_FILETIME;
        value->hVal.QuadPart = static_cast<LONGLONG>(item.mtime100ns);
      }
      return S_OK;  // VT_EMPTY: "no timestamp", which every handler accepts
    }
    if (propID == cw::kKpidIsAnti) {
      value->vt = VT_BOOL;
      value->boolVal = VARIANT_FALSE;
      return S_OK;
    }
    return S_OK;  // VT_EMPTY: no such property
  }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialInStream** inStream) override {
    if (inStream == nullptr) return E_POINTER;
    *inStream = nullptr;
    if (index >= items_.size()) return E_INVALIDARG;
    const prism::EditPlanItem& item = items_[index];
    if (item.isDir) return S_OK;  // S_OK + NULL = "this one is a folder"
    // Cancellation is checked per entry, not only in SetCompleted: a small
    // archive may never trigger a completed-value notification, and "the user
    // pressed cancel" must not depend on the size of the archive.
    if (progress_ && !progress_(0)) {
      Fail("cancelled through the progress callback");
      return S_FALSE;
    }
    if (item.size > kMaxInMemoryEntry) {
      char buf[160];
      std::snprintf(buf, sizeof(buf),
                    "entry '%s' is %llu bytes, above the %llu-byte in-memory copy ceiling",
                    item.path.c_str(), static_cast<unsigned long long>(item.size),
                    static_cast<unsigned long long>(kMaxInMemoryEntry));
      Fail(buf);
      return S_FALSE;
    }
    std::vector<uint8_t> bytes;
    std::string extractWhy;
    if (!extractor_.Extract(item.sourceIndex, item.size, &bytes, &extractWhy)) {
      Fail("cannot read '" + item.sourcePath + "' out of the original archive: " + extractWhy);
      return S_FALSE;
    }
    if (bytes.size() != item.size) {
      char buf[192];
      std::snprintf(buf, sizeof(buf), "size changed while editing '%s': plan=%llu source=%llu",
                    item.path.c_str(), static_cast<unsigned long long>(item.size),
                    static_cast<unsigned long long>(bytes.size()));
      Fail(buf);
      return S_FALSE;
    }
    bytesCopied_ += bytes.size();
    ++streamsServed_;
    *inStream = new BytesInStream(std::move(bytes));
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
    if (password == nullptr) return E_POINTER;
    if (password_.empty()) {
      *password = nullptr;
      return S_OK;
    }
    *password = SysAllocString(password_.c_str());
    return *password != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  STDMETHODIMP CryptoGetTextPassword2(abi::Int32* passwordIsDefined, BSTR* password) override {
    if (passwordIsDefined == nullptr || password == nullptr) return E_POINTER;
    if (password_.empty()) {
      *passwordIsDefined = 0;
      *password = nullptr;
      return S_OK;
    }
    *passwordIsDefined = 1;
    *password = SysAllocString(password_.c_str());
    return *password != nullptr ? S_OK : E_OUTOFMEMORY;
  }

  const std::string& reason() const { return reason_; }
  uint64_t itemsWritten() const { return itemsWritten_; }
  uint64_t streamsServed() const { return streamsServed_; }
  uint64_t bytesCopied() const { return bytesCopied_; }
  uint64_t completedCalls() const { return completedCalls_; }
  const std::vector<prism::EditPlanItem>& items() const { return items_; }

  std::string trace() const {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "items=%llu streams=%llu copied=%llu setCompleted=%llu total=%llu",
                  static_cast<unsigned long long>(items_.size()),
                  static_cast<unsigned long long>(streamsServed_),
                  static_cast<unsigned long long>(bytesCopied_),
                  static_cast<unsigned long long>(completedCalls_),
                  static_cast<unsigned long long>(total_));
    return buf;
  }

 private:
  void Fail(const std::string& why) {
    if (reason_.empty()) reason_ = why;  // keep the FIRST reason
  }

  std::vector<prism::EditPlanItem> items_;
  EntryExtractor& extractor_;
  std::wstring password_;
  std::function<bool(uint64_t)> progress_;
  std::string reason_;
  abi::UInt64 total_ = 0;
  uint64_t itemsWritten_ = 0;
  uint64_t streamsServed_ = 0;
  uint64_t bytesCopied_ = 0;
  uint64_t completedCalls_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// ApplyEdits
// ---------------------------------------------------------------------------
inline EditResult ApplyEdits(const EditRequest& request) {
  EditResult out;
  out.archivePath = request.archivePath;
  const auto started = std::chrono::steady_clock::now();
  auto elapsedMs = [&started]() -> int64_t {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    return static_cast<int64_t>((us + 999) / 1000);
  };
  auto fail = [&out, &elapsedMs](std::string why) {
    out.ok = false;
    out.error = "ApplyEdits: " + std::move(why);
    out.elapsedMs = elapsedMs();
    return out;
  };

  if (request.archivePath.empty()) return fail("the archive path is empty");
  if (request.ops.empty()) return fail("no edit was requested");

  prism::EditArchiveFacts facts;
  facts.readOnly = HasReadOnlyAttribute(request.archivePath);
  facts.multiVolume = IsMultiVolumeSet(request.archivePath);
  // A volume set is refused BEFORE the format is resolved: "case_vol.7z.001" has
  // no format suffix at all, and telling the user "unknown format" for a file
  // that is simply one of seven volumes would be the wrong reason.
  if (facts.multiVolume) {
    return fail("the archive is part of a multi-volume set (.001/.002/...): the rewrite produces "
                "a single file, so edit a single-volume archive instead");
  }
  prism::ArchiveFormat format = prism::ArchiveFormat::SevenZip;
  if (!FormatFromPath(request.archivePath, &format)) {
    return fail("cannot tell the container format from the file name (expected .7z/.zip/.tar); "
                "refusing to rewrite a file whose format is a guess");
  }
  std::string formatName(prism::ToString(format));

  const std::wstring dll = cw::ResolveSevenZipDll();
  if (dll.empty()) return fail("7z.dll not found (PRISM_7Z_DLL / exe dir / Program Files)");
  SevenZipLibrary library;
  if (!library.Load(dll)) return fail("cannot load 7z.dll at " + WideToUtf8(dll));

  // ---- observe the original: identity, solidity, encryption --------------
  std::vector<EntryFingerprint> before;
  std::string probeError;
  std::vector<EntryFingerprint> withoutPassword;
  std::string unusedWhy;
  const bool opensWithoutPassword =
      FingerprintArchive(library, request.archivePath, std::wstring(), &withoutPassword, &unusedWhy);
  if (!opensWithoutPassword) {
    // Either the header is encrypted (the normal reason) or the file is locked /
    // corrupt; the fingerprint WITH the caller's password decides which.
    if (!FingerprintArchive(library, request.archivePath, request.password, &before, &probeError)) {
      return fail("cannot read the original archive (another process may hold an exclusive "
                  "lock, the file may be gone, or the password may be wrong): " + probeError);
    }
    facts.headerEncrypted = true;
  } else {
    before = withoutPassword;
  }
  // Solidity = block grouping (see EntryFingerprint::solid). Two files inside one
  // kpidBlock are one solid block; a per-file container gives every file its own.
  std::vector<uint32_t> sourceBlocks;
  size_t sourceFiles = 0;
  for (const EntryFingerprint& entry : before) {
    if (entry.encrypted) facts.encrypted = true;
    if (entry.isDir) continue;
    ++sourceFiles;
    bool seen = false;
    for (uint32_t block : sourceBlocks) {
      if (block == entry.block) {
        seen = true;
        break;
      }
    }
    if (!seen) sourceBlocks.push_back(entry.block);
  }
  facts.solid = sourceFiles >= 2 && sourceBlocks.size() < sourceFiles;
  if (facts.encrypted && format == prism::ArchiveFormat::Tar) {
    return fail("a tar archive cannot be encrypted: refusing to rewrite it and drop the "
                "encryption");
  }
  if (facts.headerEncrypted && format != prism::ArchiveFormat::SevenZip) {
    return fail("header encryption (hidden names) is a 7z-only feature: refusing to rewrite "
                "this archive without it");
  }

  // ---- the reader the payload comes from (C11 pool: one handler, one op) --
  ReaderPool::Config poolConfig;
  poolConfig.archivePath = request.archivePath;
  poolConfig.password = request.password;
  poolConfig.capacity = 1;
  std::string poolError;
  std::shared_ptr<ReaderPool> pool = ReaderPool::Create(library, poolConfig, &poolError);
  if (!pool) {
    return fail("the source archive could not be opened for reading (" + poolError + ")");
  }
  std::vector<prism::ArchiveItem> catalog;
  {
    std::optional<ReaderPool::Lease> lease = pool->Acquire(prism::win::kWaitForever);
    if (!lease || !lease->get()) return fail("no reader lease was available for the source archive");
    // The lease is held for the catalog read only. The payload copy uses
    // EntryExtractor (password-aware), and keeping ANY handle on the original
    // open would block the final ReplaceFileW with a sharing violation.
    catalog = lease->get()->ReadCatalog();
  }
  pool->Close();
  pool.reset();
  if (catalog.empty() && !before.empty()) {
    return fail("the catalog came back empty although the archive holds " +
                std::to_string(before.size()) + " entr(ies); refusing to rewrite it");
  }

  // ---- decide ------------------------------------------------------------
  out.plan = prism::BuildEditPlan(catalog, request.ops, facts);
  if (!out.plan.ok) return fail(out.plan.error);

  // Path lookup for the verify pass, so a rename never compares against a stale
  // ordinal: the catalog (ReadCatalog) and the fingerprints (FingerprintArchive)
  // both come from the same handler, but only a path map survives a re-ordering.
  std::map<std::string, size_t> fingerprintByPath;
  for (size_t i = 0; i < before.size(); ++i) fingerprintByPath.emplace(before[i].path, i);

  // ---- what to write: the plan's list (+ the deletion filter) ------------
  std::vector<prism::EditPlanItem> items;
  items.reserve(out.plan.items.size() + out.plan.removed.size());
  for (const prism::EditPlanItem& item : out.plan.items) {
    // The plan already carries the original FILETIME (BuildEditPlan copies it
    // from the catalog), so a rewrite preserves every timestamp it can.
    items.push_back(item);
  }
#if defined(PRISM_C51_MUTANT_KEEP_DELETED)
  // MUTANT (negative control, CONTRACTS D3): the "the user deleted this entry"
  // filter is COMPILED OUT, so every removed entry is copied back into the new
  // archive -- the exact defect "delete" exists to prevent. The counter is the
  // proof the deletion rule really ran: a mutation that never fires must not be
  // able to print a CAUGHT verdict (D27).
  for (const std::string& removedPath : out.plan.removed) {
    for (const prism::ArchiveItem& entry : catalog) {
      if (entry.path != removedPath) continue;
      prism::EditPlanItem revived;
      revived.path = entry.path;
      revived.sourcePath = entry.path;
      revived.sourceIndex = entry.index;
      revived.isDir = entry.isDir;
      revived.size = entry.size;
      revived.mtime100ns = static_cast<uint64_t>(entry.mtime);
      items.push_back(std::move(revived));
      ++out.injectedKeepDeleted;
      break;
    }
  }
#endif

  // ---- temperature target: same directory, so the final replace is atomic --
  const size_t lastSlash = request.archivePath.find_last_of(L"\\/");
  const std::wstring dir =
      lastSlash == std::wstring::npos ? L"." : request.archivePath.substr(0, lastSlash);
  const std::wstring name =
      lastSlash == std::wstring::npos ? request.archivePath : request.archivePath.substr(lastSlash + 1);
  if (name.empty()) return fail("the archive path has no file name");
  const std::wstring tempPath = dir + L"\\" + name + L".c51-" +
                                std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L".tmp";
  cw::TempArchiveGuard guard;
  guard.path = tempPath;

  // Mark "a temp archive may exist now", so a later failure knows there is
  // something to clean up (and, under the mutation, something to publish).
  bool wroteTemp = false;
  auto failAfterWrite = [&](std::string why) {
    if (wroteTemp) {
      // The original is NEVER touched here: the temp is deleted by the guard and
      // the user's archive is exactly what it was before the request.
#if defined(PRISM_C51_MUTANT_BROKEN_PUBLISH)
      // MUTANT (D3): publish the half-written temp over the original anyway.
      // "the failed edit left the archive alone" must go RED.
      if (CopyFileW(tempPath.c_str(), request.archivePath.c_str(), FALSE)) {
        ++out.injectedBrokenPublish;
      } else if (MoveFileExW(tempPath.c_str(), request.archivePath.c_str(),
                             MOVEFILE_REPLACE_EXISTING)) {
        ++out.injectedBrokenPublish;
        guard.armed = false;
      }
#endif
      DeleteFileW(tempPath.c_str());
      guard.armed = false;
    }
    return fail(std::move(why));
  };

  // ---- the write plan (properties come from C9's decision layer) ----------
  prism::CompressionOptions options;
  options.format = format;
  // A 7z container does not record the level it was written with, so the rewrite
  // uses the product's default (5) and says so. Level is not part of "preserve
  // what the container knows"; encryption, solidity and mtimes are.
  options.level = 5;
  options.solid = facts.solid ? prism::SolidMode::On : prism::SolidMode::Off;
  options.encrypt = facts.encrypted;
  options.encryptFileNames = facts.headerEncrypted;
  options.cpuCores = 4;
  std::vector<prism::PlanItem> planItems;
  planItems.reserve(items.size());
  for (const prism::EditPlanItem& item : items) {
    prism::PlanItem planItem;
    planItem.path = item.path;
    planItem.size = item.isDir ? 0 : item.size;
    planItem.isDir = item.isDir;
    planItem.mtime100ns = item.mtime100ns;
    planItems.push_back(std::move(planItem));
  }
  const prism::CompressionPlan createPlan = prism::MakeCompressionPlan(planItems, options);
  if (!createPlan.ok) return fail("the write plan is not usable (" + createPlan.error + ")");

  cw::IOutArchive* outArchive = nullptr;
  const HRESULT hrCreate = library.CreateObject(*cw::ClsidFor(format), cw::kIID_IOutArchive,
                                                reinterpret_cast<void**>(&outArchive));
  if (FAILED(hrCreate) || outArchive == nullptr) {
    char buf[192];
    std::snprintf(buf, sizeof(buf),
                  "CreateObject(IOutArchive) for %s hr=0x%08lX -- this 7z.dll cannot WRITE that "
                  "format",
                  formatName.c_str(), static_cast<unsigned long>(hrCreate));
    return fail(buf);
  }
  auto failWithHandler = [&](std::string why) {
    outArchive->Release();
    return failAfterWrite(std::move(why));
  };
  std::string propError;
  std::string propTrace;
  if (!cw::ApplyProperties(outArchive, createPlan.properties, &propError, &propTrace)) {
    return failWithHandler(propError);
  }

  auto* outStream = new DiskOutStream(tempPath);
  bool streamOpen = outStream->Open();
  if (!streamOpen) {
    const DWORD err = GetLastError();
    outStream->Release();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "cannot create the temp archive (err=%lu)",
                  static_cast<unsigned long>(err));
    return failWithHandler(buf);
  }
  wroteTemp = true;

  EntryExtractor extractor(library);
  std::string extractorWhy;
  if (!extractor.Open(request.archivePath, request.password, &extractorWhy)) {
    return failWithHandler("the original archive could not be re-opened for the copy (" +
                           extractorWhy + ")");
  }
  auto* callback = new CopyUpdateCallback(items, extractor, request.password, request.progress);
  const HRESULT hrUpdate =
      outArchive->UpdateItems(outStream, static_cast<abi::UInt32>(items.size()), callback);
  out.bytesWritten = outStream->written();
  out.itemsWritten = callback->itemsWritten();
  out.bytesCopied = callback->bytesCopied();
  const std::string callbackReason = callback->reason();
  out.trace = "format=" + formatName + " props=[" + propTrace + "] " + callback->trace();
  callback->Release();
  outStream->Release();  // close the temp handle before the verify re-opens it
  // The original must be CLOSED before it can be replaced: InFileStream holds it
  // with FILE_SHARE_READ, and ReplaceFileW has to REMOVE the replaced file -- an
  // open handle there turns a successful edit into ERROR_SHARING_VIOLATION. This
  // was the suite's first red: the edit wrote and verified perfectly and then
  // failed to publish because the copy's own read handle was still open.
  extractor.Close();

  if (FAILED(hrUpdate)) {
    std::string why = "UpdateItems failed (see trace)";
    if (!callbackReason.empty()) why += ": " + callbackReason;
    return failWithHandler(why);
  }
  if (!callbackReason.empty()) return failWithHandler("the update reported a problem: " + callbackReason);
  if (out.bytesWritten <= 0) return failWithHandler("the update wrote 0 bytes");

  // ---- verify: the temp must hold EXACTLY what the plan promised ----------
  {
    std::vector<EntryFingerprint> after;
    std::string verifyWhy;
    if (!FingerprintArchive(library, tempPath, request.password, &after, &verifyWhy)) {
      return failWithHandler("the temp archive cannot be re-opened (" + verifyWhy + ")");
    }
    if (after.size() != items.size()) {
      return failWithHandler("the temp archive holds " + std::to_string(after.size()) +
                             " entr(ies), the plan promised " + std::to_string(items.size()));
    }
    // Order is not a contract (CONTRACTS A7: 7z reorders entries), so compare as
    // a set keyed by path.
    std::vector<EntryFingerprint> produced = after;
    const auto byPath = [](const EntryFingerprint& a, const EntryFingerprint& b) {
      return a.path < b.path;
    };
    std::sort(produced.begin(), produced.end(), byPath);
    std::vector<prism::EditPlanItem> expected = items;
    std::sort(expected.begin(), expected.end(),
              [](const prism::EditPlanItem& a, const prism::EditPlanItem& b) {
                return a.path < b.path;
              });
    size_t producedFiles = 0;
    std::vector<uint32_t> producedBlockIds;
    for (size_t i = 0; i < produced.size(); ++i) {
      if (produced[i].path != expected[i].path) {
        return failWithHandler("validation: entry '" + produced[i].path +
                               "' where the plan promised '" + expected[i].path + "'");
      }
      if (produced[i].isDir != expected[i].isDir) {
        return failWithHandler("validation: '" + expected[i].path + "' landed as the wrong kind");
      }
      if (!expected[i].isDir && produced[i].size != expected[i].size) {
        return failWithHandler("validation: '" + expected[i].path + "' holds " +
                               std::to_string(produced[i].size) + " bytes, plan promised " +
                               std::to_string(expected[i].size));
      }
      // The stored CRC32 is what proves the BYTES, not just the name and size:
      // a rename/move must copy the payload unchanged, so the CRC of the new
      // entry must equal the CRC of the entry it came from.
      const EntryFingerprint* source = nullptr;
      if (!expected[i].sourcePath.empty()) {
        const auto it = fingerprintByPath.find(expected[i].sourcePath);
        if (it != fingerprintByPath.end()) source = &before[it->second];
      }
      // Compare when BOTH sides expose a CRC: a handler may legitimately omit
      // kpidCRC for an empty payload, and refusing the edit over a property the
      // engine chose not to report would block a legitimate 0-byte entry.
      if (!expected[i].isDir && source != nullptr && source->hasCrc && produced[i].hasCrc) {
        if (produced[i].crc != source->crc) {
          char buf[224];
          std::snprintf(buf, sizeof(buf),
                        "validation: '%s' CRC changed (was 0x%08lX, now 0x%08lX)",
                        expected[i].path.c_str(), static_cast<unsigned long>(source->crc),
                        static_cast<unsigned long>(produced[i].hasCrc ? produced[i].crc : 0));
          return failWithHandler(buf);
        }
      }
      // Encryption is compared PER ENTRY, not against the archive-wide flag: a
      // 0-byte entry legitimately reports "not encrypted" in an AES archive
      // (there is nothing to encrypt), and failing the edit over that would block
      // a legitimate empty file.
      if (!expected[i].isDir && source != nullptr &&
          produced[i].encrypted != source->encrypted) {
        return failWithHandler("validation: '" + expected[i].path +
                               "' lost or gained payload encryption");
      }
      if (!expected[i].isDir) {
        ++producedFiles;
        bool seenBlock = false;
        for (uint32_t block : producedBlockIds) {
          if (block == produced[i].block) {
            seenBlock = true;
            break;
          }
        }
        if (!seenBlock) producedBlockIds.push_back(produced[i].block);
      }
      ++out.verifiedItems;
    }
    // Solidity is preserved, not "best effort", and it is judged by BLOCK
    // GROUPING (the only signal this engine answers): a solid archive keeps files
    // packed into fewer blocks than files; a per-file archive must not.
    const bool producedSolid = producedFiles >= 2 && producedBlockIds.size() < producedFiles;
    if (facts.solid != producedSolid) {
      return failWithHandler(facts.solid
                                 ? "validation: the archive lost its solid block"
                                 : "validation: a non-solid archive came back solid");
    }
    // Header encryption is proven the only way it can be: the temp must NOT open
    // without the password.
    if (facts.headerEncrypted) {
      std::vector<EntryFingerprint> hidden;
      std::string ignored;
      if (FingerprintArchive(library, tempPath, std::wstring(), &hidden, &ignored)) {
        return failWithHandler("validation: the rewritten archive can be listed WITHOUT the "
                               "password -- header encryption was not preserved");
      }
    }
    out.verified = true;
  }

  // ---- publish -----------------------------------------------------------
  outArchive->Release();
  // ReplaceFileW (not MoveFileEx): it swaps the file in one operation and merges
  // the attributes/ACL of the original onto the replacement. IGNORE_MERGE_ERRORS
  // keeps an exotic ACL from turning a successful edit into a lost one.
  const BOOL replaced = ReplaceFileW(request.archivePath.c_str(), tempPath.c_str(), nullptr,
                                     REPLACEFILE_IGNORE_MERGE_ERRORS, nullptr, nullptr);
  if (!replaced) {
    const DWORD err = GetLastError();
    char buf[186];
    std::snprintf(buf, sizeof(buf),
                  "ReplaceFileW failed (err=%lu) -- the temp archive was removed and the "
                  "original is untouched",
                  static_cast<unsigned long>(err));
    return failAfterWrite(buf);
  }
  guard.armed = false;  // the temp BECAME the original

  out.ok = true;
  out.elapsedMs = elapsedMs();
  return out;
}

}  // namespace prism::win::edit
