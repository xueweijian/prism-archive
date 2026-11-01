// PrismArchive -- PrismWin
// DragOutSession (card C20): catalog selection -> REAL IDataObject, zero %TEMP%.
//
// WHAT WAS ALREADY THERE (and why this file exists anyway)
//   PrismVirtualDragDrop.hpp  owns the COM shell (DelayedExtractStream) and the
//                             solid-to-seek mapping.
//   PrismDragDecisionBridge.hpp owns VirtualFileDataObject (CFSTR_FILEDESCRIPTORW
//                             + CFSTR_FILECONTENTS) and ArchiveItemStreamProducer
//                             (7z.dll PUSH -> bounded pipe -> IStream PULL).
//   ReaderPool.hpp (C11)       owns N independent IInArchive handlers.
// What was missing is the WIRING that turns "these catalog entries are selected"
// into a data object Explorer can actually copy from -- and that wiring is where
// three invariants live that no existing test could see:
//
//   1. ONE HANDLER PER LIVE STREAM. A 7z handler is not thread-safe (C11's whole
//      debt note). The producer runs ONE Extract call on a background thread, so
//      two streams sharing the main UI reader would race inside 7z.dll. Every
//      stream therefore leases its OWN reader from a pool created for this drag,
//      never the window's reader.
//   2. ZERO %TEMP%. Nothing in this path opens a file: the descriptor is an
//      HGLOBAL, the payload is a pipe into the receiver's IStream. There is no
//      DiskOutStream here on purpose -- the 7-Zip behaviour this replaces wrote
//      the file twice (to %TEMP%, then the copy). The test proves it by pointing
//      TMP/TEMP at an empty trap directory and asserting it stays empty.
//   3. NO SILENT FAILURE. A refused stream, a starved slot, a bad decode: each
//      one is an HRESULT the drop target sees. Serving zeros or a short file
//      while returning S_OK is the failure mode this file refuses to have.
//
// LIFETIME: the IDataObject captures a shared SessionState (pool + descriptor
// mapping + counters), so the receiver may keep the object (and its streams)
// alive after the window that started the drag is gone. Destroying the session
// handle does not invalidate a data object already handed out.
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/DragDrop.hpp"
#include "PrismDragDecisionBridge.hpp"
#include "PrismEngineBridge.hpp"
#include "PrismVirtualDragDrop.hpp"
#include "ReaderPool.hpp"

namespace prism::win::dragout {

// The COM shell and the push->pull pipe live in prism::win::dragdrop (the drag
// protocol layer); this session is the policy/wiring layer above them.
using prism::win::dragdrop::ArchiveItemStreamProducer;
using prism::win::dragdrop::DelayedExtractStream;

// ---------------------------------------------------------------------------
// Deliberate breakage for the D3 negative control. Both mutants live HERE (not
// in the shared bridge headers) so the card owns its own red run: the suite is
// compiled twice more, once per mutant, and CTest's WILL_FAIL makes green mean
// "the mutation was caught".
// ---------------------------------------------------------------------------
namespace mutation {
// The stub shape: the object advertises both formats and answers E_NOTIMPL on
// GetData. A suite that passes while GetData refuses was never reading bytes.
#if defined(PRISM_C20_MUTANT_GETDATA_NOTIMPL)
constexpr bool kGetDataRefuses = true;
#else
constexpr bool kGetDataRefuses = false;
#endif
// The lying-length shape: the FILEDESCRIPTORW keeps the honest size while the
// IStream claims one byte more than the decoder will ever deliver. A copy
// engine sizes its progress bar from Stat().cbSize, so this is the bug that
// produces "99% then error" -- or worse, a truncated file that looks fine.
#if defined(PRISM_C20_MUTANT_SIZE_LIE)
constexpr bool kStreamSizeLies = true;
#else
constexpr bool kStreamSizeLies = false;
#endif
}  // namespace mutation

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------
struct SessionConfig {
  // The archive this drag comes from. The session opens its OWN readers: the
  // window's reader is the UI thread's and must not be touched from a worker.
  std::wstring archivePath;
  std::wstring password;

  // Independent 7z handlers. == ExtractScheduler::Config::handlerSlots: one
  // slot == one handler == one concurrent Extract. A 20-file selection does not
  // get 20 handlers; the extra streams wait for a slot (and if none frees,
  // they FAIL rather than share a handler).
  size_t maxHandlers = 4;
  size_t pipeHighWaterBytes = 4u * 1024 * 1024;
  std::chrono::milliseconds slotWait{3000};

  // Per-stream seek budget. The defaults are the C11/M4 production values
  // (4 MiB lookback, 2 restarts, 16 MiB re-decode) and are used unchanged
  // unless a caller tightens them for a benchmark/test.
  prism::dnd::SeekPolicy seekPolicy{};
  prism::dnd::DescriptorOptions descriptorOptions{};
};

// Reported facts, not assumptions. tempFilesWritten is structurally zero (this
// file never opens a file); the test proves it from the outside with a trap
// directory, and this counter exists so a future change that DOES spill has to
// lie in two places at once.
struct SessionStats {
  size_t handlers = 0;           // pool capacity actually opened
  uint64_t streamsOpened = 0;
  uint64_t streamsRefused = 0;   // no reader slot / unknown lindex / bad item
  uint64_t bytesPiped = 0;       // produced by 7z.dll into the receivers
  uint64_t solidRestarts = 0;    // re-decodes triggered by backward seeks
  size_t maxConcurrent = 0;      // peak simultaneously leased handlers
  uint64_t tempFilesWritten = 0; // 0 by construction (see above)
};

// One selected catalog entry, in FILEDESCRIPTORW order (== lindex).
struct SelectionEntry {
  uint32_t catalogIndex = 0;
  uint64_t size = 0;
  bool isDirectory = false;
};

namespace detail {

struct Counters {
  std::atomic<uint64_t> streamsOpened{0};
  std::atomic<uint64_t> streamsRefused{0};
  std::atomic<uint64_t> bytesPiped{0};
  std::atomic<uint64_t> solidRestarts{0};
};

// A producer that OWNS the reader lease for exactly as long as the stream can
// pull from it. This is the C11 boundary made structural: the lease is acquired
// before the first byte is decoded and returned only when the last reference to
// the stream goes away, so no two live streams can ever share a handler.
//
// Member order is load-bearing: lease_ first, inner_ second -- destruction runs
// in reverse, so the worker thread joins (inner_) BEFORE the slot is returned.
class LeasedItemProducer final : public DelayedExtractStream::Producer {
 public:
  LeasedItemProducer(prism::win::ReaderPool::Lease lease, uint32_t itemIndex,
                     size_t highWaterBytes, std::shared_ptr<Counters> counters)
      : lease_(std::move(lease)),
        counters_(std::move(counters)),
        inner_(lease_.get(), itemIndex, highWaterBytes) {
    if (counters_) counters_->streamsOpened.fetch_add(1, std::memory_order_relaxed);
  }

  ~LeasedItemProducer() override {
    // inner_ still exists here (it is destroyed after this body): read its
    // verdict before it goes away, and publish it as drag-out evidence.
    if (counters_) {
      counters_->solidRestarts.fetch_add(inner_.restarts(), std::memory_order_relaxed);
      counters_->bytesPiped.fetch_add(inner_.producedBytes(), std::memory_order_relaxed);
    }
  }

  LeasedItemProducer(const LeasedItemProducer&) = delete;
  LeasedItemProducer& operator=(const LeasedItemProducer&) = delete;

  std::string Next(size_t maxBytes) override { return inner_.Next(maxBytes); }
  bool Restart() override { return inner_.Restart(); }

  prism::win::ArchiveReader* reader() const { return lease_.get(); }
  size_t slot() const { return lease_.slot(); }
  bool workerFailed() const { return inner_.workerFailed(); }
  prism::win::abi::Int32 itemErrorCode() const { return inner_.itemErrorCode(); }

 private:
  prism::win::ReaderPool::Lease lease_;
  std::shared_ptr<Counters> counters_;
  ArchiveItemStreamProducer inner_;
};

#if defined(PRISM_C20_MUTANT_GETDATA_NOTIMPL)
// D3 control. Advertises (EnumFormatEtc/QueryGetData delegate) and refuses to
// produce -- the "wired up" stub. The suite must go red at the first GetData.
class RefusingDataObject final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
 public:
  HRESULT RuntimeClassInitialize(IDataObject* inner) {
    if (!inner) return E_INVALIDARG;
    inner_ = inner;
    return S_OK;
  }
  IFACEMETHODIMP GetData(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  IFACEMETHODIMP QueryGetData(FORMATETC* etc) override { return inner_->QueryGetData(etc); }
  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
    return inner_->GetCanonicalFormatEtc(in, out);
  }
  IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    return inner_->EnumFormatEtc(direction, out);
  }
  IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  Microsoft::WRL::ComPtr<IDataObject> inner_;
};
#endif

}  // namespace detail

// ---------------------------------------------------------------------------
// DragOutSession
// ---------------------------------------------------------------------------
class DragOutSession {
 public:
  // Everything a handed-out data object needs, kept alive by shared_ptr so the
  // object outlives the window that created it.
  struct State {
    std::shared_ptr<prism::win::ReaderPool> pool;  // one handler per live stream
    std::vector<SelectionEntry> entries;           // lindex -> catalog entry
    prism::dnd::GroupDescriptor group;             // sanitised descriptors
    SessionConfig config;
    std::shared_ptr<detail::Counters> counters;
  };

  // Build a session over "selection" (catalog indices, e.g. the tree view's
  // selected rows). Descriptor names are basenames sanitised by PrismCore --
  // a raw archive path never reaches a receiving application.
  static std::shared_ptr<DragOutSession> Create(
      SevenZipLibrary& library, const std::vector<ArchiveItem>& catalog,
      const std::vector<uint32_t>& selection, SessionConfig config = {},
      std::string* error = nullptr) {
    auto fail = [error](const char* why) -> std::shared_ptr<DragOutSession> {
      if (error) *error = why;
      return nullptr;
    };
    if (selection.empty()) return fail("dragout: empty selection");
    if (config.archivePath.empty()) return fail("dragout: archive path is empty");

    auto state = std::make_shared<State>();
    state->config = config;
    state->counters = std::make_shared<detail::Counters>();

    // Selection -> descriptors, in caller order (Explorer renders lindex order).
    std::vector<prism::dnd::VirtualFileDescriptor> descriptorInput;
    descriptorInput.reserve(selection.size());
    state->entries.reserve(selection.size());
    size_t fileCount = 0;
    for (uint32_t index : selection) {
      const ArchiveItem* item = nullptr;
      for (const ArchiveItem& candidate : catalog) {
        if (candidate.index == index) {
          item = &candidate;
          break;
        }
      }
      if (!item) return fail("dragout: selection index is not in the catalog");
      SelectionEntry entry;
      entry.catalogIndex = index;
      entry.size = item->size;
      entry.isDirectory = item->isDir;
      state->entries.push_back(entry);
      prism::dnd::VirtualFileDescriptor file;
      file.name = item->path;  // reduced to a sanitised basename by the builder
      file.size = item->size;
      file.modifiedTime = item->mtime;
      file.isDirectory = item->isDir;
      descriptorInput.push_back(std::move(file));
      if (!item->isDir) ++fileCount;
    }
    state->group =
        prism::dnd::BuildGroupDescriptor(descriptorInput, config.descriptorOptions);
    if (state->group.error != prism::dnd::DenyReason::None) {
      if (error) *error = "dragout: descriptor refused: " + state->group.message;
      return nullptr;
    }
    // The builder preserves order and count; if that ever changes, the lindex
    // mapping silently serves the WRONG item's bytes -- refuse instead.
    if (state->group.files.size() != state->entries.size()) {
      return fail("dragout: descriptor order no longer matches the selection");
    }

    // Opening one handler per concurrent stream costs an open per handler, so
    // the pool is sized to the real need and capped by the slot budget.
    if (fileCount > 0) {
      prism::win::ReaderPool::Config poolConfig;
      poolConfig.archivePath = config.archivePath;
      poolConfig.password = config.password;
      poolConfig.capacity = fileCount < config.maxHandlers ? fileCount : config.maxHandlers;
      std::string poolError;
      state->pool = prism::win::ReaderPool::Create(library, poolConfig, &poolError);
      if (!state->pool) {
        if (error) *error = "dragout: reader pool: " + poolError;
        return nullptr;
      }
    }

    auto session = std::shared_ptr<DragOutSession>(new DragOutSession());
    session->state_ = std::move(state);
    return session;
  }

  // One CFSTR_FILECONTENTS payload. lindex indexes the descriptor blob, which
  // is the same order as the selection.
  //
  // Returns nullptr (never a zero-filled stream) when the item is a directory,
  // the lindex is out of range, or no reader slot frees up: GetData turns each
  // of those into a specific HRESULT.
  Microsoft::WRL::ComPtr<DelayedExtractStream> OpenStream(size_t lindex) {
    Microsoft::WRL::ComPtr<DelayedExtractStream> stream;
    if (!state_) return stream;
    const State& state = *state_;
    if (lindex >= state.entries.size()) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return stream;
    }
    const SelectionEntry& entry = state.entries[lindex];
    if (entry.isDirectory) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return stream;  // a folder has no contents stream (GetData -> E_INVALIDARG)
    }
    if (entry.size == 0) {
      // An empty file has no payload to decode, and the decoder agrees: it
      // produces no bytes at all for a 0-byte item, so a pull would park on a
      // pipe that never fills (measured: the D3 size-lie run -- one byte
      // declared on an empty item -- blocked the reader thread forever). The
      // empty memory stream IS that file, and it costs no handler slot.
      return stream;  // callers use OpenContentsStream, which serves it directly
    }
    if (!state.pool) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return stream;
    }
    // A slot is required, not optional: sharing a handler between two pulls is
    // the C11 bug this session exists to avoid. Waiting is bounded so a stuck
    // copy cannot hang the drop target forever.
    std::optional<prism::win::ReaderPool::Lease> lease =
        state.pool->Acquire(state.config.slotWait);
    if (!lease) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return stream;
    }
    auto producer = std::make_shared<detail::LeasedItemProducer>(
        std::move(*lease), entry.catalogIndex, state.config.pipeHighWaterBytes,
        state.counters);
    // The honest size, unless the D3 mutant is compiled in (see mutation{}).
    const uint64_t declared = mutation::kStreamSizeLies ? entry.size + 1 : entry.size;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<DelayedExtractStream>(
            &stream, producer, declared, state.config.seekPolicy))) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    return stream;
  }

  // The payload the Shell actually receives: the decoder-backed stream for a
  // real file, and a zero-length memory stream for a 0-byte item (see above).
  Microsoft::WRL::ComPtr<IStream> OpenContentsStream(size_t lindex) {
    Microsoft::WRL::ComPtr<IStream> out;
    if (!state_) return out;
    const State& state = *state_;
    if (lindex >= state.entries.size()) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return out;
    }
    const SelectionEntry& entry = state.entries[lindex];
    if (entry.isDirectory) {
      state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
      return out;
    }
    if (entry.size == 0) {
      if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, out.GetAddressOf()))) {
        state.counters->streamsRefused.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
      }
      return out;
    }
    Microsoft::WRL::ComPtr<DelayedExtractStream> stream = OpenStream(lindex);
    if (!stream) return nullptr;
    if (FAILED(stream.As(&out))) return nullptr;
    return out;
  }

  // The payload a drop target receives. The stream factory is lazy: no decoder
  // runs until the target asks for CFSTR_FILECONTENTS on a specific lindex.
  HRESULT BuildDataObject(IDataObject** out) const {
    if (!out) return E_POINTER;
    *out = nullptr;
    const State& state = *state_;
    if (state.group.error != prism::dnd::DenyReason::None) return E_INVALIDARG;
    if (state.group.files.empty()) return E_INVALIDARG;

    std::shared_ptr<State> shared = state_;  // capture keeps the pool alive
    auto streamFor = [shared](size_t lindex) -> Microsoft::WRL::ComPtr<IStream> {
      return OpenStreamOn(shared, lindex);
    };

    Microsoft::WRL::ComPtr<IDataObject> object;
    const HRESULT hr = prism::win::dragdrop::CreateVirtualFileDataObject(
        state.group, std::move(streamFor), &object);
    if (FAILED(hr)) return hr;
#if defined(PRISM_C20_MUTANT_GETDATA_NOTIMPL)
    Microsoft::WRL::ComPtr<detail::RefusingDataObject> refusing;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<detail::RefusingDataObject>(
            &refusing, object.Get()))) {
      return E_FAIL;
    }
    Microsoft::WRL::ComPtr<IDataObject> refusingInterface;
    if (FAILED(refusing.As(&refusingInterface))) return E_FAIL;
    *out = refusingInterface.Detach();
    return S_OK;
#else
    *out = object.Detach();
    return S_OK;
#endif
  }

  const prism::dnd::GroupDescriptor& group() const { return state_->group; }
  const std::vector<SelectionEntry>& entries() const { return state_->entries; }
  const SessionConfig& config() const { return state_->config; }

  SessionStats stats() const {
    SessionStats out;
    if (!state_) return out;
    const State& state = *state_;
    if (state.pool) {
      const prism::win::ReaderPool::Stats poolStats = state.pool->stats();
      out.handlers = poolStats.handlers;
      out.maxConcurrent = poolStats.maxConcurrent;
    }
    out.streamsOpened = state.counters->streamsOpened.load(std::memory_order_relaxed);
    out.streamsRefused = state.counters->streamsRefused.load(std::memory_order_relaxed);
    out.bytesPiped = state.counters->bytesPiped.load(std::memory_order_relaxed);
    out.solidRestarts = state.counters->solidRestarts.load(std::memory_order_relaxed);
    out.tempFilesWritten = 0;  // structural: no file is ever opened on this path
    return out;
  }

  // State-only form, so the lambda captured by the data object never depends on
  // a session object surviving the drag.
  static Microsoft::WRL::ComPtr<IStream> OpenStreamOn(const std::shared_ptr<State>& state,
                                                      size_t lindex) {
    if (!state) return nullptr;
    DragOutSession view;
    view.state_ = state;
    // OpenContentsStream yields IStream already, and the decoder-backed one got
    // there through QueryInterface (As()) -- which is exactly how the Shell
    // reaches it (B8: a complete vtable that QI does not answer is invisible).
    return view.OpenContentsStream(lindex);
  }

 private:
  DragOutSession() = default;

  std::shared_ptr<State> state_;
};

}  // namespace prism::win::dragout
