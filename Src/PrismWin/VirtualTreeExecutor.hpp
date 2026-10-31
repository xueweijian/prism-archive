// PrismArchive -- PrismWin
// In-archive edit executor (batch B1): wires the PrismCore VirtualTree CoW
// decision plan to the REAL 7z.dll IOutArchive::UpdateItems() call.
//
// HONEST COST MODEL (ADR-002 "'10 GB rename in 0.05 s' has been corrected",
// CONTRACTS E): newData = 0 / newProps = 1 does NOT patch the archive in
// place. The 7-Zip out engine streams every item's compressed data into a
// brand-new archive (7zUpdate.cpp CRepackStreamBase reads old item data
// straight from the open handler's database and CRC-checks each copied item),
// then the caller replaces the original file. A commit therefore costs one
// full sequential copy plus an atomic same-directory MoveFileExW. We do not
// advertise instant renames: progress is fed in copied BYTES and
// CommitResult.copiedBytes carries the true number of bytes written into the
// new file.
//
// ABI facts pinned against the official 7-Zip sources (ip7z/7zip main:
// Guid.txt, IArchive.h, IPassword.h, Archive/7z/7zHandlerOut.cpp), per
// CONTRACTS A4:
//   IID_IOutArchive             = {23170F69-40C1-278A-0000-000600A00000}  (family 06, member 0xA0)
//   IID_IArchiveUpdateCallback  = {23170F69-40C1-278A-0000-000600800000}  (family 06, member 0x80)
//   IID_ICryptoGetTextPassword2 = {23170F69-40C1-278A-0000-000500110000}  (family 05, member 0x11)
// GetUpdateItemInfo contract (IArchive.h "UPDATE" comment block):
//   newData=0 newProps=0  -> copy data AND properties from the old archive
//   newData=0 newProps=1  -> copy data from archive, request new properties
//                            (exactly our metadata-only CoW plan)
//   indexInArchive = (UInt32)-1 marks a brand-new item.
// GetStream() is only ever called for newData=1 items; the repack path never
// asks for fresh streams. We answer it with a loud E_NOTIMPL (CONTRACTS C5:
// a refusal carries its reason) -- silently returning NULL would make the
// engine write an empty file.
// kpidPath is VT_BSTR; the handler runs ReplaceSlashes_OsToUnix, so the
// '\'-separated form demanded by CONTRACTS B4 is accepted verbatim.
// A new empty folder is (newData=0, newProps=1, indexInArchive=-1) and its
// GetProperty(kpidIsDir) MUST answer VT_BOOL(TRUE): with VT_EMPTY the handler
// would treat the node as an empty file.
//
// Caller contract: no other handle may hold the archive open while CommitTree
// runs. InFileStream opens without FILE_SHARE_DELETE, so a stale reader turns
// the final MoveFileExW into a sharing violation (loud, original untouched).
#pragma once

#include <windows.h>
#include <propidl.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../PrismCore/VirtualTree.hpp"
#include "PrismEngineBridge.hpp"
#include "SevenZipAbi.hpp"

namespace prism::win::vt {

// ---- update-side ABI (provenance in the file header comment) ---------------
// IProgress = family 00, member 0x05 (SevenZipAbi.hpp predates the update path
// and pins no IID for it, so the executor carries its own).
constexpr GUID kIID_IProgress = {0x23170F69, 0x40C1, 0x278A,
                                 {0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00}};
constexpr GUID kIID_IOutArchive = {0x23170F69, 0x40C1, 0x278A,
                                   {0x00, 0x00, 0x00, 0x06, 0x00, 0xA0, 0x00, 0x00}};
constexpr GUID kIID_IArchiveUpdateCallback = {0x23170F69, 0x40C1, 0x278A,
                                              {0x00, 0x00, 0x00, 0x06, 0x00, 0x80, 0x00, 0x00}};
constexpr GUID kIID_ICryptoGetTextPassword2 = {0x23170F69, 0x40C1, 0x278A,
                                               {0x00, 0x00, 0x00, 0x05, 0x00, 0x11, 0x00, 0x00}};

// Vtable order is the binary contract with 7z.dll: IProgress first, then the
// four update methods, exactly as Z7_IFACEM_IArchiveUpdateCallback lists them.
struct IArchiveUpdateCallback : public abi::IProgress {
  STDMETHOD(GetUpdateItemInfo)(abi::UInt32 index, abi::Int32* newData, abi::Int32* newProps,
                               abi::UInt32* indexInArchive) PURE;
  STDMETHOD(GetProperty)(abi::UInt32 index, PROPID propID, PROPVARIANT* value) PURE;
  STDMETHOD(GetStream)(abi::UInt32 index, abi::ISequentialInStream** inStream) PURE;
  STDMETHOD(SetOperationResult)(abi::Int32 operationResult) PURE;
};

struct IOutArchive : public IUnknown {
  STDMETHOD(UpdateItems)(abi::ISequentialOutStream* outStream, abi::UInt32 numItems,
                         IArchiveUpdateCallback* updateCallback) PURE;
  STDMETHOD(GetFileTimeType)(abi::UInt32* type) PURE;
};

// IPassword.h family 05 member 0x11. SevenZipAbi.hpp pins only the OPEN-side
// password (member 0x10); updates are asked for the "2" variant, so the
// executor carries its own declaration (same binary vtable contract).
// NOTE: the 7-Zip pure interfaces derive the GLOBAL IUnknown (unknwn.h), not
// an abi-scoped alias -- SevenZipAbi.hpp does the same.
struct ICryptoGetTextPassword2 : public IUnknown {
  STDMETHOD(CryptoGetTextPassword2)(abi::Int32* passwordIsDefined, BSTR* password) PURE;
};

struct CommitResult {
  bool ok = false;
  std::string error;
  int64_t copiedBytes = 0;
  int64_t elapsedMs = 0;
  std::wstring newArchivePath;
  std::string updateTrace;  // diagnostic: what the engine asked the callback
};

namespace detail {

// One entry of the NEW archive, in update-callback index order.
struct OutEntry {
  int64_t indexInArchive = -1;  // -1 = brand-new node, not in the old archive
  std::wstring newPath;         // empty = untouched item (props come from the old archive)
  bool newProps = false;
  bool newData = false;         // brand-new dirs report newData=1 with a NULL stream
  bool isDir = false;           // (the canonical FS-driven flow; see GetStream)
  std::string expectedPath;     // normalized UTF-8 '/' path, for the reopen validation
};

// Minimal per-item property readers. PrismEngineBridge's helpers live inside
// ArchiveReader (private) and the reader object cannot hand out its open
// IInArchive, so the executor runs its own open + catalog pass; these mirror
// ArchiveReader's Get* semantics on purpose (same PROPID ordinals, CONTRACTS A3).
inline std::string PropString(abi::IInArchive* archive, abi::UInt32 index, PROPID propId) {
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

inline bool PropBool(abi::IInArchive* archive, abi::UInt32 index, PROPID propId) {
  PROPVARIANT value{};
  PropVariantInit(&value);
  bool out = false;
  if (archive && SUCCEEDED(archive->GetProperty(index, propId, &value))) {
    if (value.vt == VT_BOOL) out = value.boolVal != VARIANT_FALSE;
  }
  PropVariantClear(&value);
  return out;
}

inline uint64_t PropUInt(abi::IInArchive* archive, abi::UInt32 index, PROPID propId) {
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

struct CatalogEntry {
  std::string path;  // normalized UTF-8, '/'-separated (PrismCore contract)
  bool isDir = false;
  uint64_t size = 0;
};

inline std::vector<CatalogEntry> ReadMiniCatalog(abi::IInArchive* archive) {
  std::vector<CatalogEntry> catalog;
  abi::UInt32 count = 0;
  if (!archive || FAILED(archive->GetNumberOfItems(&count))) return catalog;
  catalog.reserve(count);
  for (abi::UInt32 i = 0; i < count; ++i) {
    CatalogEntry entry;
    entry.path = prism::NormalizeArchivePath(PropString(archive, i, abi::kpidPath));
    entry.isDir = PropBool(archive, i, abi::kpidIsDir);
    entry.size = PropUInt(archive, i, abi::kpidSize);
    catalog.push_back(std::move(entry));
  }
  return catalog;
}

// The update callback the engine drives. Everything it answers is derived from
// the decision plan; failures are recorded as readable reasons, never swallowed.
class TreeUpdateCallback final : public IArchiveUpdateCallback,
                                 public ICryptoGetTextPassword2 {
 public:
  TreeUpdateCallback(std::vector<OutEntry> entries, abi::IInArchive* oldArchive,
                     std::function<bool(int64_t)> progress)
      : entries_(std::move(entries)), old_(oldArchive), progress_(std::move(progress)) {}

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == kIID_IArchiveUpdateCallback) {
      *out = static_cast<IArchiveUpdateCallback*>(this);
    } else if (iid == kIID_IProgress) {
      *out = static_cast<abi::IProgress*>(this);
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

  // The engine reports work in bytes (repack = sequential copy); this is the
  // honest progress the UI feeds on, not a fake percent.
  STDMETHODIMP SetCompleted(const abi::UInt64* completeValue) override {
    if (!completeValue) return S_OK;
    if (progress_ && !progress_(static_cast<int64_t>(*completeValue))) {
      Fail("cancelled through the progress callback");
      return E_ABORT;
    }
    return S_OK;
  }

  STDMETHODIMP GetUpdateItemInfo(abi::UInt32 index, abi::Int32* newData, abi::Int32* newProps,
                                 abi::UInt32* indexInArchive) override {
    if (!newData || !newProps || !indexInArchive) return E_POINTER;
    if (index >= entries_.size()) return E_INVALIDARG;
    const OutEntry& entry = entries_[index];
    *newProps = entry.newProps ? 1 : 0;
    *indexInArchive = static_cast<abi::UInt32>(entry.indexInArchive);  // -1 -> 0xFFFFFFFF
#if defined(PRISM_VTREE_MUTANT_STREAM_TOUCHED)
    // MUTANT (METHODOLOGY section 4, phase 3): metadata-only items pretend to
    // carry fresh data, so the engine calls GetStream() and the commit must
    // fail loudly instead of silently recompressing.
    *newData = entry.newProps ? 1 : 0;
#else
    *newData = entry.newData ? 1 : 0;
#endif
    return S_OK;
  }

  STDMETHODIMP GetProperty(abi::UInt32 index, PROPID propId, PROPVARIANT* value) override {
    if (!value) return E_POINTER;
    PropVariantInit(value);
    if (index >= entries_.size()) return E_INVALIDARG;
    const OutEntry& entry = entries_[index];
    // Diagnostic ask-trace (evidence round): what the engine asked and which
    // branch answered. Cheap always-on; the test prints it on failure only.
    {
      char line[64];
      std::snprintf(line, sizeof(line), "ask i=%u pid=%u entry.idx=%lld\n",
                    static_cast<unsigned>(index), static_cast<unsigned>(propId),
                    static_cast<long long>(entry.indexInArchive));
      trace_ += line;
    }
    // The new path wins for touched/new items; everything else is forwarded to
    // the old archive so untouched metadata (times, attributes, sizes)
    // survives the rewrite. Reentrancy is safe: the engine calls us on the
    // same thread and the handler's GetProperty only reads its parsed database
    // (7-Zip's own update callback forwards the very same way).
    if (propId == abi::kpidPath && !entry.newPath.empty()) {
      value->vt = VT_BSTR;
      value->bstrVal = SysAllocString(entry.newPath.c_str());
      return value->bstrVal ? S_OK : E_OUTOFMEMORY;
    }
    if (entry.indexInArchive >= 0 && old_ != nullptr) {
      trace_ += "  -> FWD old-archive\n";
      return old_->GetProperty(static_cast<abi::UInt32>(entry.indexInArchive), propId, value);
    }
    trace_ += "  -> EMPTY\n";
    // Brand-new node: an empty folder carries its directory flag AND the
    // directory attribute. Evidence round 3 proved the handler receives our
    // VT_BOOL(kpidIsDir)=TRUE yet still writes a file, so mirror the
    // canonical FS-driven caller too: real folders always carry
    // FILE_ATTRIBUTE_DIRECTORY, and 7-Zip's SetDirStatusFromAttrib path keys
    // off exactly that. Answering both covers either engine derivation.
    if (propId == abi::kpidIsDir) {
      value->vt = VT_BOOL;
      value->boolVal = VARIANT_TRUE;
      trace_ += "  -> NEW-DIR TRUE\n";
    } else if (propId == abi::kpidAttrib) {
      value->vt = VT_UI4;
      value->ulVal = FILE_ATTRIBUTE_DIRECTORY;
      trace_ += "  -> NEW-DIR ATTRIB\n";
    } else if (propId == abi::kpidMTime) {
      // The FS-driven caller ALWAYS has an MTime (7zHandlerOut: "we need
      // MTime to sort files"); we are the only caller answering VT_EMPTY.
      // FILETIME epoch 1970-01-01 = 11644473600s * 1e7.
      value->vt = VT_FILETIME;
      value->hVal.QuadPart = static_cast<LONGLONG>(116444736000000000LL);
      trace_ += "  -> NEW-DIR MTIME\n";
    }
    return S_OK;
  }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialInStream** inStream) override {
    if (!inStream) return E_POINTER;
    *inStream = nullptr;
    // Brand-new directory: the canonical CArchiveUpdateCallback answers
    // S_OK with a NULL stream for folders (UpdateCallback.cpp GetStream2 --
    // "if (isDir) return S_OK;" with *inStream left NULL). Evidence rounds
    // 3-7 showed the (newData=0, index=-1) combination never reaches the
    // directory writer, so mirror the one flow 7-Zip itself exercises.
    if (index < entries_.size() && entries_[index].newData && entries_[index].isDir) {
      return S_OK;
    }
    // Unreachable by construction: every entry reports newData = 0 and the out
    // engine only asks for streams of newData = 1 items. Answer loudly (C5) --
    // a NULL stream here would make the engine write a silent empty file.
    Fail("update engine requested a data stream for new-archive item " +
         std::to_string(index) + " but the virtual-tree plan is metadata-only");
    return E_NOTIMPL;
  }

  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
    // The 7z out engine reports through HRESULT and never calls this, but the
    // interface allows other handlers to; a non-zero result is a failure.
    if (operationResult != 0) {
      Fail("update operation reported failure result=" + std::to_string(operationResult));
    }
    return S_OK;
  }

  STDMETHODIMP CryptoGetTextPassword2(abi::Int32* passwordIsDefined, BSTR* password) override {
    if (!passwordIsDefined || !password) return E_POINTER;
    // 0 = "no password from the UI layer": the handler then reuses the
    // password the archive was OPENED with, if it had one (7zHandlerOut.cpp
    // update path). We never invent a password here.
    *passwordIsDefined = 0;
    *password = nullptr;
    return S_OK;
  }

  const std::string& reason() const { return reason_; }
  const std::string& trace() const { return trace_; }

 private:
  void Fail(const std::string& why) {
    if (reason_.empty()) reason_ = why;  // keep the FIRST reason, lose none
  }

  std::vector<OutEntry> entries_;
  std::string trace_;
  abi::IInArchive* old_ = nullptr;
  std::function<bool(int64_t)> progress_;
  std::string reason_;
  abi::UInt64 total_ = 0;
  LONG refs_ = 1;
};

// Deletes the temp file on every exit path that did not end in a successful
// replace, so a failed commit can never litter the archive directory (and the
// failure-injection test asserts exactly that).
struct TempFileGuard {
  std::wstring path;  // plain Win32 path, backslashes
  bool armed = true;

  ~TempFileGuard() {
#if defined(PRISM_VTREE_MUTANT_KEEP_TEMP)
    // MUTANT (METHODOLOGY section 4, phase 3): leak the temp file on the
    // failure path; the leftover assertion must go red.
    (void)path;
    (void)armed;
#else
    if (armed && !path.empty()) {
      // \\?\ keeps long-path cleanup working; the prefix disables path
      // normalization so the separators must already be backslashes (B4).
      DeleteFileW((L"\\\\?\\" + path).c_str());
    }
#endif
  }
};

// Locates 7z.dll without a registry: explicit env override first (tests and
// embedding hosts pin it), then the shipping layout beside our exe, then the
// stock 7-Zip install directory.
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

// Opens the archive for update: candidate handlers in order, S_OK-only accept
// (CONTRACTS A1), stream rewound between candidates (CONTRACTS A2). The
// accepted handler + its stream stay alive until UpdateItems is done, because
// the engine reads the old items' data through exactly this handler.
struct OpenedArchive {
  abi::IInArchive* handler = nullptr;
  InFileStream* stream = nullptr;
  std::string trace;

  bool Open(SevenZipLibrary& library, const std::wstring& path) {
    stream = new InFileStream(path);
    if (!stream->valid()) {
      trace = "CreateFileW failed for the archive itself";
      return false;
    }
    for (const GUID* clsid : {&abi::CLSID_CFormatZip, &abi::CLSID_CFormat7z}) {
      abi::IInArchive* candidate = nullptr;
      if (FAILED(library.CreateObject(*clsid, abi::IID_IInArchive,
                                      reinterpret_cast<void**>(&candidate))) ||
          !candidate) {
        trace += "(no handler object) ";
        continue;
      }
      // A rejected Open consumes stream position; rewind so the next candidate
      // sees the real magic bytes instead of leftovers.
      abi::UInt64 newPos = 0;
      stream->Seek(0, /*STREAM_SEEK_SET=*/0, &newPos);
      auto* callback = new OpenCallback();
      // 7-Zip convention: S_OK = mine, S_FALSE = "not my format, try next".
      // SUCCEEDED(S_FALSE) is TRUE, so only an exact S_OK may accept a handler.
      const HRESULT result = candidate->Open(stream, nullptr, callback);
      callback->Release();
      if (result == S_OK) {
        handler = candidate;
        trace += std::string((clsid == &abi::CLSID_CFormatZip) ? "Zip" : "7z") + ".Open OK";
        return true;
      }
      char buf[64];
      std::snprintf(buf, sizeof(buf), "Open hr=0x%08lX; ", static_cast<unsigned long>(result));
      trace += buf;
      candidate->Release();
    }
    return false;
  }

  // Releases both COM objects and the file handle; MUST run before the final
  // MoveFileExW (the handle is opened without FILE_SHARE_DELETE). Idempotent.
  void Close() {
    if (handler) {
      handler->Close();
      handler->Release();
      handler = nullptr;
    }
    if (stream) {
      stream->Release();
      stream = nullptr;
    }
  }
};

inline std::wstring ToBackslashes(std::wstring p) {
  for (wchar_t& c : p) {
    if (c == L'/') c = L'\\';
  }
  return p;
}

}  // namespace detail

// Executes one VirtualTree transaction against the archive on disk: builds the
// metadata-only update plan, feeds it to IOutArchive::UpdateItems() through a
// temporary file in the SAME directory, re-opens the result to validate the
// directory tree, then atomically replaces the original. On any failure the
// temp file is deleted, a readable error is returned, and the original archive
// is guaranteed untouched. progress (optional) receives copied bytes as the
// engine streams them; returning false cancels the commit with E_ABORT.
inline CommitResult CommitTree(const std::wstring& archivePath,
                               const prism::VirtualArchiveTree& tree,
                               std::function<bool(int64_t copiedBytes)> progress = nullptr) {
  CommitResult out;
  const auto started = std::chrono::steady_clock::now();
  // Ceil to whole milliseconds: never under-report the cost the user paid.
  auto elapsedMs = [&started]() -> int64_t {
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - started)
                        .count();
    return static_cast<int64_t>((us + 999) / 1000);
  };
  auto fail = [&out, &elapsedMs](std::string why) {
    out.ok = false;
    out.error = "CommitTree: " + std::move(why);
    out.elapsedMs = elapsedMs();
    return out;
  };

  if (archivePath.empty()) return fail("archive path is empty");
  if (archivePath.find_first_of(L"*?\"<>|") != std::wstring::npos) {
    return fail("archive path contains wildcard characters");
  }

  const auto plan = tree.MakeUpdatePlan();
  if (plan.empty()) {
    if (tree.dirty()) return fail("decision tree is dirty but the update plan is empty");
    // No-op commit: rewriting a 10 GB archive to change nothing would be the
    // opposite of honest. Nothing is touched; the tree already IS the archive.
    out.ok = true;
    out.newArchivePath = archivePath;
    out.elapsedMs = elapsedMs();
    return out;
  }

  SevenZipLibrary library;
  const std::wstring dll = detail::ResolveSevenZipDll();
  if (dll.empty()) return fail("7z.dll not found (PRISM_7Z_DLL / exe dir / Program Files)");
  if (!library.Load(dll)) return fail("cannot load 7z.dll at " + WideToUtf8(dll));

  detail::OpenedArchive opened;
  if (!opened.Open(library, archivePath)) {
    return fail("open failed for " + WideToUtf8(archivePath) + " (" + opened.trace + ")");
  }

  // The decision plan -> the full new-archive item list. UpdateItems needs an
  // entry for EVERY item of the new archive, untouched ones included.
  const auto catalog = detail::ReadMiniCatalog(opened.handler);
  std::map<int64_t, const prism::UpdateItem*> plannedByOldIndex;
  std::map<uint32_t, bool> erased;  // old archive index -> removed by the plan
  for (const auto& u : plan) {
    if (u.removed) {
      if (u.index < 0 || static_cast<size_t>(u.index) >= catalog.size()) {
        opened.Close();
        return fail("plan removes index " + std::to_string(u.index) +
                    " which the archive does not contain");
      }
      erased[static_cast<uint32_t>(u.index)] = true;
    } else if (u.index >= 0) {
      plannedByOldIndex[u.index] = &u;
    }
  }

  std::vector<detail::OutEntry> entries;
  entries.reserve(catalog.size() + plan.size());
  for (size_t i = 0; i < catalog.size(); ++i) {
    if (erased.count(static_cast<uint32_t>(i)) != 0) continue;
    detail::OutEntry entry;
    entry.indexInArchive = static_cast<int64_t>(i);
    entry.isDir = catalog[i].isDir;
    entry.expectedPath = catalog[i].path;
    const auto it = plannedByOldIndex.find(static_cast<int64_t>(i));
    if (it != plannedByOldIndex.end()) {
      const std::string newPath = prism::NormalizeArchivePath(it->second->newPath);
      if (newPath.empty()) {
        opened.Close();
        return fail("plan renames item " + std::to_string(i) + " to an empty path");
      }
      entry.newProps = true;
      // B4: '\' separators at the Win32 layer (the handler re-normalizes).
      entry.newPath = detail::ToBackslashes(Utf8ToWide(newPath));
      entry.expectedPath = newPath;
    }
    entries.push_back(std::move(entry));
  }
  for (const auto& u : plan) {
    if (u.removed || u.index >= 0) continue;  // removals already shrank the list
    const std::string newPath = prism::NormalizeArchivePath(u.newPath);
    if (newPath.empty()) {
      opened.Close();
      return fail("plan creates a node with an empty path");
    }
    detail::OutEntry entry;
    entry.indexInArchive = -1;
    entry.newProps = true;
    entry.newData = true;  // the canonical FS flow: new items are "new data",
    entry.isDir = true;    // and directories answer GetStream with S_OK+NULL
    entry.newPath = detail::ToBackslashes(Utf8ToWide(newPath));
    entry.expectedPath = newPath;
    entries.push_back(std::move(entry));
  }

  // QI the OPEN handler for the out side: the update engine reads the old
  // items' data through this very object, so a fresh unopened handler would
  // not do (CONTRACTS A4 discipline: one wrong IID here = E_NOINTERFACE).
  IOutArchive* outArchive = nullptr;
  const HRESULT hrQI =
      opened.handler->QueryInterface(kIID_IOutArchive, reinterpret_cast<void**>(&outArchive));
  if (FAILED(hrQI) || !outArchive) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "QueryInterface(IID_IOutArchive) hr=0x%08lX",
                  static_cast<unsigned long>(hrQI));
    opened.Close();
    return fail(buf);
  }

  // Temp file in the SAME directory as the archive: only a same-volume rename
  // can ever be atomic (a cross-directory temp would copy the payload twice).
  const size_t lastSlash = archivePath.find_last_of(L"\\/");
  const std::wstring dir =
      (lastSlash == std::wstring::npos) ? L"." : archivePath.substr(0, lastSlash);
  const std::wstring name =
      (lastSlash == std::wstring::npos) ? archivePath : archivePath.substr(lastSlash + 1);
  if (name.empty()) {
    outArchive->Release();
    opened.Close();
    return fail("archive path has no file name");
  }
  const std::wstring tempPath = dir + L"\\" + name + L".vtcommit-" +
                                std::to_wstring(GetCurrentProcessId()) + L"-" +
                                std::to_wstring(GetTickCount64()) + L".tmp";

  detail::TempFileGuard guard;
  guard.path = detail::ToBackslashes(tempPath);

  // Every failure after the QI releases the out archive, closes the original,
  // and lets the guard delete the temp. Single exit discipline per stage.
  auto failCleanup = [&](std::string why) {
    if (outArchive != nullptr) {
      outArchive->Release();
      outArchive = nullptr;
    }
    opened.Close();
    return fail(std::move(why));
  };

  auto* outStream = new DiskOutStream(tempPath);
  if (!outStream->Open()) {
    const DWORD err = GetLastError();
    outStream->Release();
    char buf[160];
    std::snprintf(buf, sizeof(buf), "cannot create temp file %s err=%lu",
                  WideToUtf8(tempPath).c_str(), static_cast<unsigned long>(err));
    return failCleanup(buf);
  }

  auto* callback = new detail::TreeUpdateCallback(entries, opened.handler, std::move(progress));
  const HRESULT hrUpdate =
      outArchive->UpdateItems(outStream, static_cast<abi::UInt32>(entries.size()), callback);
  out.copiedBytes = static_cast<int64_t>(outStream->written());
  out.updateTrace = "handler=" + opened.trace + "\n" + callback->trace();
  const std::string cbReason = callback->reason();
  callback->Release();
  outStream->Release();  // closes the temp handle before validation re-opens it
  char hrBuf[32];
  std::snprintf(hrBuf, sizeof(hrBuf), "hr=0x%08lX", static_cast<unsigned long>(hrUpdate));
  if (FAILED(hrUpdate)) {
    std::string why = "UpdateItems failed " + std::string(hrBuf);
    if (!cbReason.empty()) why += " (" + cbReason + ")";
    return failCleanup(why);
  }
  if (!cbReason.empty()) {
    return failCleanup("UpdateItems reported a problem (" + cbReason + ")");
  }
  if (out.copiedBytes <= 0) {
    return failCleanup("UpdateItems wrote " + std::to_string(out.copiedBytes) +
                       " bytes; a valid archive always carries a header");
  }

  // Validation: re-open the temp archive and prove the resulting directory
  // tree is exactly what the decision plan promised, BEFORE the original is
  // touched. This gate keeps a half-written archive from ever replacing the
  // user's file.
  {
    detail::OpenedArchive check;
    if (!check.Open(library, tempPath)) {
      return failCleanup("temp archive failed re-open validation (" + check.trace + ")");
    }
    const auto produced = detail::ReadMiniCatalog(check.handler);
    check.Close();
    if (produced.size() != entries.size()) {
      return failCleanup("validation: new archive holds " + std::to_string(produced.size()) +
                         " items, plan promised " + std::to_string(entries.size()));
    }
    std::vector<std::pair<std::string, bool>> got;
    got.reserve(produced.size());
    for (const auto& item : produced) got.push_back({item.path, item.isDir});
    std::vector<std::pair<std::string, bool>> want;
    want.reserve(entries.size());
    for (const auto& entry : entries) want.push_back({entry.expectedPath, entry.isDir});
    const auto less = [](const std::pair<std::string, bool>& a,
                         const std::pair<std::string, bool>& b) {
      if (a.first != b.first) return a.first < b.first;
      return a.second < b.second;
    };
    std::sort(got.begin(), got.end(), less);
    std::sort(want.begin(), want.end(), less);
    if (got != want) {
      std::string why = "validation: directory tree mismatch";
      for (size_t i = 0; i < got.size() && i < want.size(); ++i) {
        if (got[i] != want[i]) {
          why += "; got '" + got[i].first + "' want '" + want[i].first + "'";
          break;
        }
      }
      return failCleanup(why);
    }
  }

  // Both handles to the ORIGINAL must be gone before the replace: the file was
  // opened without FILE_SHARE_DELETE, so any stale handle = sharing violation.
  outArchive->Release();
  outArchive = nullptr;
  opened.Close();
  const HRESULT moved = MoveFileExW(detail::ToBackslashes(tempPath).c_str(),
                                    detail::ToBackslashes(archivePath).c_str(),
                                    MOVEFILE_REPLACE_EXISTING);
  if (!moved) {
    const DWORD err = GetLastError();
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "MoveFileExW replace failed err=%lu -- temp removed, original untouched",
                  static_cast<unsigned long>(err));
    return failCleanup(buf);
  }
  guard.armed = false;  // the temp no longer exists; it BECAME the archive

  out.ok = true;
  out.newArchivePath = archivePath;
  out.elapsedMs = elapsedMs();
  return out;
}

}  // namespace prism::win::vt
