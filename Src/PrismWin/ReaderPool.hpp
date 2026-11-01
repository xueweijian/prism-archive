// PrismArchive -- PrismWin
// ReaderPool: N independent ArchiveReaders, leased one per concurrent
// operation (7-Zip's rule: one handler per concurrent operation).
//
// WHY THIS EXISTS (docs/PROGRESS.md known debt, 2026-11-01)
//   sweep and extract shared ONE ArchiveReader. A 7-Zip handler is not
//   thread-safe AND an ArchiveReader owns a single input stream, so the two
//   threads were racing inside 7z.dll *and* moving one shared file pointer.
//   Both threads were joined (no lifetime bug), but "the user double-clicks
//   while the sweep is still running" is a real overlap on big archives.
//
// THE CONTRACT
//   * Create() opens exactly Config::capacity readers. Each reader is its own
//     IInArchive handler plus its own InFileStream -- never a view of another
//     reader's handler. If any open fails, the pool is not created: a pool that
//     quietly offers fewer handlers than promised would hand a shared handler
//     to the second task, which is the exact bug this class removes.
//   * Acquire() returns a MOVE-ONLY Lease. The Lease is the only way to reach
//     a reader, its destructor is noexcept and returns the slot, so a worker
//     that throws (or an early return) can never leak a handler.
//   * Acquire(milliseconds(0)) is a non-blocking try (the backpressure signal
//     ExtractScheduler turns into kBusy); Acquire(kWaitForever) blocks.
//   * Lease lifetime is independent of the pool object: leases share the
//     underlying state, so destroying the pool while a worker still extracts
//     cannot free the handler under it (the UAF shape B9/PR15 removed from the
//     window layer).
//
// NEVER SHARE. A test that sees two leases pointing at the same ArchiveReader
// has caught the bug; a mutation build (PRISM_C11_MUTANT_SHARED_HANDLER) makes
// that literal so the concurrency suite can prove it goes red.
#pragma once

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "PrismEngineBridge.hpp"

namespace prism::win {

// Block until a slot frees (or the pool closes). Never used on the UI thread;
// the UI thread uses the non-blocking overload and reports backpressure.
inline constexpr std::chrono::milliseconds kWaitForever = (std::chrono::milliseconds::max)();

class ReaderPool {
 public:
  struct Config {
    std::wstring archivePath;
    std::wstring password;
    // == ExtractScheduler::Config::handlerSlots. One slot == one handler.
    size_t capacity = 2;
  };

  struct Stats {
    size_t capacity = 0;        // slots promised
    size_t handlers = 0;        // ArchiveReader objects actually opened
    uint64_t acquired = 0;
    uint64_t returned = 0;
    uint64_t waits = 0;         // acquisitions that found every slot busy
    uint64_t timeouts = 0;      // ...and could not wait
    size_t inUse = 0;
    size_t maxConcurrent = 0;
    size_t distinctSlotsUsed = 0;
    bool sharedHandlerInjected = false;  // mutation-build marker
  };

 private:
  static constexpr size_t kNoSlot = static_cast<size_t>(-1);

  struct Slot {
    std::shared_ptr<ArchiveReader> reader;
    bool busy = false;
    uint64_t leases = 0;
  };

  struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool closed = false;
    std::vector<Slot> slots;
    std::vector<bool> used;
    size_t inUse = 0;
    Stats stats;

    bool FreeExist() const {
#if defined(PRISM_C11_MUTANT_SHARED_HANDLER)
      return true;  // mutation: the old shared-reader path never says "busy"
#else
      for (const Slot& slot : slots) {
        if (!slot.busy) return true;
      }
      return false;
#endif
    }

    size_t Pick() const {
#if defined(PRISM_C11_MUTANT_SHARED_HANDLER)
      return slots.empty() ? kNoSlot : 0;
#else
      for (size_t i = 0; i < slots.size(); ++i) {
        if (!slots[i].busy) return i;
      }
      return kNoSlot;
#endif
    }

    void RefreshDerived() {
      stats.inUse = inUse;
      if (inUse > stats.maxConcurrent) stats.maxConcurrent = inUse;
      size_t distinct = 0;
      for (const bool usedSlot : used) {
        if (usedSlot) ++distinct;
      }
      stats.distinctSlotsUsed = distinct;
    }
  };

 public:
  // RAII lease. Move-only, idempotent Release(), noexcept destructor.
  class Lease {
   public:
    Lease() = default;
    Lease(Lease&& other) noexcept : state_(std::move(other.state_)), slot_(other.slot_) {
      other.slot_ = kNoSlot;
    }
    Lease& operator=(Lease&& other) noexcept {
      if (this != &other) {
        Release();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        other.slot_ = kNoSlot;
      }
      return *this;
    }
    Lease(const Lease&) = delete;
    Lease& operator=(const Lease&) = delete;
    ~Lease() { Release(); }

    void Release() noexcept {
      if (!state_) return;
      {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (slot_ < state_->slots.size()) state_->slots[slot_].busy = false;
        if (state_->inUse > 0) --state_->inUse;
        ++state_->stats.returned;
        state_->RefreshDerived();
      }
      state_->cv.notify_one();
      state_.reset();
      slot_ = kNoSlot;
    }

    ArchiveReader* get() const {
      if (!state_ || slot_ >= state_->slots.size()) return nullptr;
      return state_->slots[slot_].reader.get();
    }
    ArchiveReader* operator->() const { return get(); }
    ArchiveReader& operator*() const { return *get(); }
    // Slot index == ExtractScheduler::Dispatch::slot.
    size_t slot() const { return slot_; }
    explicit operator bool() const { return get() != nullptr; }

   private:
    friend class ReaderPool;
    Lease(std::shared_ptr<State> state, size_t slot)
        : state_(std::move(state)), slot_(slot) {}
    std::shared_ptr<State> state_;
    size_t slot_ = kNoSlot;
  };

  ReaderPool(const ReaderPool&) = delete;
  ReaderPool& operator=(const ReaderPool&) = delete;

  ~ReaderPool() { Close(); }

  // Opens Config::capacity independent handlers. Returns nullptr (and fills
  // *error) if ANY handler fails to open -- a partially capable pool is the
  // shared-handler bug waiting to happen.
  //
  // PRECONDITION: the SevenZipLibrary outlives the pool AND its leases (it owns
  // the loaded 7z.dll; teardown order is the caller's assembly step).
  static std::shared_ptr<ReaderPool> Create(SevenZipLibrary& library, const Config& config,
                                            std::string* error = nullptr) {
    if (config.capacity == 0) {
      if (error) *error = "ReaderPool: capacity must be >= 1";
      return nullptr;
    }
#if defined(PRISM_C11_MUTANT_SHARED_HANDLER)
    const size_t handlerCount = 1;
#else
    const size_t handlerCount = config.capacity;
#endif
    std::vector<std::shared_ptr<ArchiveReader>> readers;
    readers.reserve(handlerCount);
    for (size_t i = 0; i < handlerCount; ++i) {
      auto reader = std::make_shared<ArchiveReader>(library);
      if (!reader->Open(config.archivePath, config.password)) {
        if (error) {
          *error = "ReaderPool: handler #" + std::to_string(i) +
                   " failed to open the archive: " + reader->openTrace();
        }
        return nullptr;
      }
      readers.push_back(std::move(reader));
    }

    auto pool = std::shared_ptr<ReaderPool>(new ReaderPool());
    pool->state_->slots.resize(config.capacity);
    pool->state_->used.assign(config.capacity, false);
    for (size_t i = 0; i < config.capacity; ++i) {
#if defined(PRISM_C11_MUTANT_SHARED_HANDLER)
      pool->state_->slots[i].reader = readers.front();
#else
      pool->state_->slots[i].reader = readers[i];
#endif
    }
    pool->state_->stats.capacity = config.capacity;
    pool->state_->stats.handlers = readers.size();
#if defined(PRISM_C11_MUTANT_SHARED_HANDLER)
    pool->state_->stats.sharedHandlerInjected = true;
#endif
    pool->archivePath_ = config.archivePath;
    return pool;
  }

  // timeout == milliseconds(0): non-blocking try. kWaitForever: wait.
  std::optional<Lease> Acquire(std::chrono::milliseconds timeout = std::chrono::milliseconds(0)) {
    std::shared_ptr<State> state = state_;
#if defined(PRISM_C50_MUTANT_SHARED_LEASE)
    // Negative control (CONTRACTS D3/D27): the lease mutual exclusion is GONE.
    // No slot is marked busy and every caller is handed slot 0, so two threads
    // can hold the SAME ArchiveReader at once -- the exact defect this card
    // exists to prove the pool does not have. The suite prints
    // "C50 MUTATION CAUGHT: shared_lease" ONLY while the C41 counter proves the
    // injection fired (conflicts > 0); a dead injection can go red without ever
    // printing it, so the marker is the conjunction (D26), not a WILL_FAIL.
    {
      std::unique_lock<std::mutex> lock(state->mutex);
      ++state->stats.acquired;
      ++state->inUse;
      if (!state->used.empty()) state->used[0] = true;
      state->RefreshDerived();
    }
    return std::optional<Lease>(Lease(state, 0));
#else
    std::unique_lock<std::mutex> lock(state->mutex);
    if (state->closed) return std::nullopt;
    if (!state->FreeExist()) {
      ++state->stats.waits;
      if (timeout == std::chrono::milliseconds(0)) {
        ++state->stats.timeouts;
        return std::nullopt;
      }
      auto ready = [&state] { return state->closed || state->FreeExist(); };
      if (timeout == kWaitForever) {
        state->cv.wait(lock, ready);
      } else if (!state->cv.wait_for(lock, timeout, ready)) {
        ++state->stats.timeouts;
        return std::nullopt;
      }
      if (state->closed) return std::nullopt;
    }
    const size_t slot = state->Pick();
    if (slot == kNoSlot || slot >= state->slots.size()) {
      ++state->stats.timeouts;
      return std::nullopt;
    }
    // Build the lease BEFORE touching the slot: if the allocation throws, no
    // slot is left marked busy (exception safety).
    Lease lease(state, slot);
    state->slots[slot].busy = true;
    ++state->slots[slot].leases;
    state->used[slot] = true;
    ++state->inUse;
    ++state->stats.acquired;
    state->RefreshDerived();
    return std::optional<Lease>(std::move(lease));
#endif
  }

  // Refuse new leases (shutdown / archive closed). Leases already out keep
  // their handler; the engine is never interrupted from here.
  void Close() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->closed = true;
    state_->cv.notify_all();
  }

  bool closed() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->closed;
  }

  size_t capacity() const { return state_->slots.size(); }

  size_t inUse() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->inUse;
  }

  Stats stats() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stats;
  }

  // C50: READ-ONLY observability. C41's guard refuses a second concurrent
  // operation on one handler and counts it; that counter is the only direct
  // evidence that two threads were handed the same ArchiveReader. Summing it
  // over the pool lets a failing assertion print ONE number (and lets the
  // stress suite assert "zero under normal load") without holding a lease.
  //
  // Zero is the contract on every clean run: a non-zero value beside a red
  // win.reader_pool means the lease, not the engine, was wrong.
  uint64_t operationConflicts() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return ConflictsLocked();
  }

  // Same counter, per slot. Slot index == Lease::slot() == handler index, so a
  // failure log can be matched to the lease that produced it.
  std::vector<uint64_t> operationConflictsBySlot() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::vector<uint64_t> out;
    out.reserve(state_->slots.size());
    for (const Slot& slot : state_->slots) {
      out.push_back(slot.reader ? slot.reader->operationConflicts() : 0);
    }
    return out;
  }

  const std::wstring& archivePath() const { return archivePath_; }

 private:
  ReaderPool() : state_(std::make_shared<State>()) {}

  // Caller holds state_->mutex. Safe from a const method: slots never resize
  // after Create, so the readers are stable for the pool's lifetime.
  uint64_t ConflictsLocked() const {
    uint64_t total = 0;
    for (const Slot& slot : state_->slots) {
      if (slot.reader) total += slot.reader->operationConflicts();
    }
    return total;
  }

  // Non-const because Acquire mutates; kept private for that reason.
  mutable std::shared_ptr<State> state_;
  std::wstring archivePath_;
};

}  // namespace prism::win
