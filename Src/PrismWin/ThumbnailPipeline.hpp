// PrismArchive -- PrismWin
// ThumbnailPipeline: the decode half of the gallery's thumbnail cache.
//
// WHY THIS EXISTS (card C45)
//   C33 made ThumbStore's 64 MiB budget a real contract and C7 measured the
//   100k-row scroll. Decoding, though, was still serial: the sweep handed
//   thumbnails over one at a time while the UI thread waited in front of the
//   queue, and the flush path was 45-71% of the p95 frame (C33). This header
//   owns the missing piece:
//
//   1. A FIXED pool of N = min(4, hardware threads) decoder workers. Tasks are
//      ordered by the visible list (index 0 first) and a viewport change
//      CANCELS the queued work that scrolled out -- the decoder is never called
//      for a task that left the viewport before a worker picked it up
//      (Stats::cancelledBeforeDecode is the proof, not a comment).
//   2. Workers never touch the UI: they decode, then PostMessage(kThumbReady)
//      an owned payload to the window. The UI thread is the ONLY thread that
//      calls ThumbStore::OnThumb (taking the bitmap) and computes the
//      invalidation rectangle. CONTRACTS B9 is about exactly this shape: the
//      worker reaches the window through PostMessage and nothing else; there
//      is no detach() anywhere -- Stop() joins every thread.
//   3. Three loud counters (hits / misses / cancelled) so a hit rate is a
//      measurement, not a claim, and ThumbStore stays the single owner of the
//      pixel budget (this file does NOT modify it; C33's contract is intact).
//
// NEGATIVE CONTROLS (compile-time, registered in Tests/suites/c45-thumbs.cmake)
//   PRISM_C45_MUTANT_NO_CANCEL  -- the scroll-out branch is skipped, so queued
//                                  tasks still decode. The cancellation
//                                  assertions must go red; the mutant test only
//                                  prints CAUGHT when the injected branch was
//                                  actually observed at runtime.
//   PRISM_C45_MUTANT_ONE_WORKER -- the pool is forced to one worker, so the
//                                  parallelism assertion must go red.
//
// TIMING: this header deliberately owns no clock. The bench measures
// wall-clock; the tests assert bytes/counts (card discipline: no stopwatch
// verdicts).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h must not own min/max (PrismCore uses std::min/max)
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../PrismCore/ThumbStore.hpp"

namespace prism::win::gallery {

class ThumbnailPipeline final {
 public:
  // Worker -> UI. The LPARAM is an owned Payload*; the UI thread takes it by
  // value and deletes it. A failed PostMessage frees it at the source, the
  // same ownership rule GalleryWindow uses for its posted strings.
  static constexpr UINT kThumbReady = WM_APP + 0x45;

  static constexpr size_t kDefaultBudgetBytes = 64u * 1024u * 1024u;
  static constexpr size_t kMaxWorkers = 4;

  // Called on a WORKER thread only. The ordinal is the dispatch order assigned
  // when the task was popped (0-based, strictly increasing): it is the visible
  // priority made observable, so a test can assert the order without timing.
  using DecodeFn =
      std::function<prism::gallery::DecodedThumb(uint32_t itemIndex, uint64_t ordinal)>;

  struct Config {
    HWND hwnd = nullptr;  // the UI thread's window; must outlive the pipeline
    // 0 => min(4, hardware_concurrency()). 1 is legal and is what the bench's
    // serial leg uses; the shipped app leaves it 0.
    size_t workerCount = 0;
    size_t budgetBytes = kDefaultBudgetBytes;
    int gridColumns = 4;  // invalidation geometry only
    uint32_t tileWidth = 128;
    uint32_t tileHeight = 128;
  };

  struct Stats {
    size_t workers = 0;
    uint64_t visibleRequests = 0;  // items asked for, across every call
    uint64_t hits = 0;             // served by ThumbStore without a decode
    uint64_t misses = 0;           // scheduled for decode
    uint64_t cancelled = 0;        // tasks dropped by a viewport change
    uint64_t cancelledBeforeDecode = 0;  // ...that never reached the decoder
    uint64_t cancelledInFlight = 0;      // results discarded after decode began
    uint64_t discarded = 0;              // payloads thrown away, not posted
    uint64_t decoded = 0;                // decoder invocations that returned
    uint64_t posted = 0;                 // PostMessageW succeeded
    uint64_t applied = 0;                // consumed by the UI thread
    uint64_t rejected = 0;               // ThumbStore refused (over budget)
    uint64_t invalidRects = 0;           // UI-thread invalidations produced
    uint64_t postFailures = 0;
    uint64_t decodeFailures = 0;  // decoder threw; task discarded loudly
    uint64_t uiThreadViolations = 0;  // RequestVisible/OnThumbMessage off-thread
    size_t maxConcurrentDecodes = 0;
    size_t peakQueue = 0;
    size_t residentBytes = 0;  // ThumbStore::SizeBytes() at snapshot time
  };

  struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    bool empty = true;

    void Union(const Rect& other) {
      if (other.empty) return;
      if (empty) {
        *this = other;
        return;
      }
      left = (std::min)(left, other.left);
      top = (std::min)(top, other.top);
      right = (std::max)(right, other.right);
      bottom = (std::max)(bottom, other.bottom);
    }
  };

  // The posted-message payload. Public so a window procedure that receives
  // kThumbReady can free it even after the pipeline is gone (early shutdown).
  struct Payload {
    uint32_t index = 0;
    prism::gallery::DecodedThumb thumb;
  };

  ThumbnailPipeline(Config config, DecodeFn decode)
      : config_(config),
        decode_(std::move(decode)),
        store_(config.budgetBytes),
        uiThreadId_(GetCurrentThreadId()) {
    config_.workerCount = ResolveWorkerCount(config.workerCount);
    stats_.workers = config_.workerCount;
    workers_.reserve(config_.workerCount);
    for (size_t i = 0; i < config_.workerCount; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  ThumbnailPipeline(const ThumbnailPipeline&) = delete;
  ThumbnailPipeline& operator=(const ThumbnailPipeline&) = delete;

  // Joins every worker (never detached) and drains any unread kThumbReady
  // payloads, so no heap block outlives the pipeline. The HWND must still be
  // valid here; the window owns the message queue and outlives this object.
  ~ThumbnailPipeline() { Stop(); }

  // ---- injection facts (the negative controls print these) -----------------
  static bool MutationCancellationDisabled() {
#if defined(PRISM_C45_MUTANT_NO_CANCEL)
    return true;
#else
    return false;
#endif
  }
  static bool MutationSingleWorker() {
#if defined(PRISM_C45_MUTANT_ONE_WORKER)
    return true;
#else
    return false;
#endif
  }

  static size_t DefaultWorkerCount() {
#if defined(PRISM_C45_MUTANT_ONE_WORKER)
    return 1;
#else
    const unsigned hardware = std::thread::hardware_concurrency();
    // hardware_concurrency() may report 0 when it cannot tell; four is the
    // documented "unknown" value, not a guess about this machine.
    const size_t threads = hardware == 0 ? kMaxWorkers : static_cast<size_t>(hardware);
    return (std::min)(kMaxWorkers, threads);
#endif
  }

  size_t WorkerCount() const { return config_.workerCount; }
  const Config& config() const { return config_; }

  // ---- UI-thread entry points ---------------------------------------------

  // The viewport now shows these indices, in painting order (first == top-left).
  // Hits are served from the store; everything else is scheduled. Any queued
  // or in-flight task no longer visible is cancelled here.
  void RequestVisible(const std::vector<uint32_t>& indices) {
    if (GetCurrentThreadId() != uiThreadId_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.uiThreadViolations;
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    visible_.clear();
    visibleSet_.clear();
    for (const uint32_t index : indices) {
      if (visibleSet_.insert(index).second) visible_.push_back(index);
    }
    stats_.visibleRequests += visible_.size();

    if (!MutationCancellationDisabled()) {
      // Queued work that scrolled out is DROPPED, not deferred: the decoder is
      // never called for it, which is what "cancel really releases work" means.
      for (auto it = queue_.begin(); it != queue_.end();) {
        if (visibleSet_.find(*it) == visibleSet_.end()) {
          queued_.erase(*it);
          it = queue_.erase(it);
          ++stats_.cancelled;
          ++stats_.cancelledBeforeDecode;
        } else {
          ++it;
        }
      }
      // In-flight work cannot be stopped mid-call, but its RESULT is discarded
      // and the discard is counted (cancelledInFlight).
      for (auto& entry : inFlight_) {
        if (!entry.second && visibleSet_.find(entry.first) == visibleSet_.end()) {
          entry.second = true;  // cancelled
          ++stats_.cancelled;
          ++stats_.cancelledInFlight;
        }
      }
      // Re-anchor the queue to the new visible order (visible-first priority).
      if (!queue_.empty()) {
        std::deque<uint32_t> reordered;
        for (const uint32_t index : visible_) {
          if (queued_.find(index) != queued_.end()) reordered.push_back(index);
        }
        queue_.swap(reordered);
      }
    }

    for (const uint32_t index : visible_) {
      if (store_.Lookup(index) != nullptr) {
        ++stats_.hits;
        continue;
      }
      if (queued_.find(index) != queued_.end()) continue;
      if (inFlight_.find(index) != inFlight_.end()) continue;
      queue_.push_back(index);
      queued_.insert(index);
      ++stats_.misses;
      if (queue_.size() > stats_.peakQueue) stats_.peakQueue = queue_.size();
    }
    cv_.notify_all();
  }

  // The UI thread's whole job for a decoded thumbnail: take the bitmap and
  // produce one invalidation rectangle. Returns false when the payload was
  // refused by the budget (loudly counted, never silent).
  bool OnThumbMessage(LPARAM payload) {
    std::unique_ptr<Payload> owned(reinterpret_cast<Payload*>(payload));
    if (!owned) return false;
    if (GetCurrentThreadId() != uiThreadId_) {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.uiThreadViolations;
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.applied;
    }
    const bool accepted = store_.OnThumb(owned->index, owned->thumb);
    if (accepted) {
      const Rect rect = TileRect(owned->index);
      {
        std::lock_guard<std::mutex> lock(mutex_);
        dirty_.Union(rect);
        ++stats_.invalidRects;
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      ++stats_.rejected;
    }
    cv_.notify_all();
    return accepted;
  }

  // Consume the invalidation produced since the last call.
  bool TakeDirtyRect(Rect* out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dirty_.empty) return false;
    if (out != nullptr) *out = dirty_;
    dirty_ = Rect{};
    return true;
  }

  // ---- observation ---------------------------------------------------------

  Stats Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats copy = stats_;
    copy.residentBytes = store_.SizeBytes();
    return copy;
  }

  // The store is UI-thread-only (Lookup refreshes LRU recency, OnThumb mutates
  // the budget); the const overload is for read-only observation.
  prism::gallery::ThumbStore& Store() { return store_; }
  const prism::gallery::ThumbStore& Store() const { return store_; }
  size_t ResidentBytes() const { return store_.SizeBytes(); }

  bool Idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty() && inFlightCount_ == 0 && stats_.posted == stats_.applied;
  }
  size_t QueueDepth() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }
  size_t InFlight() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inFlightCount_;
  }

  // Test/bench synchronisation. All three return false on timeout instead of
  // pretending the condition held.
  bool WaitForConcurrentDecodes(size_t atLeast, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout,
                        [&] { return stopping_ || stats_.maxConcurrentDecodes >= atLeast; });
  }
  bool WaitForPosted(uint64_t atLeast, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return stopping_ || stats_.posted >= atLeast; });
  }

  // Joins the pool. Idempotent.
  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopping_) return;
      stopping_ = true;
      // Queued work is released, not decoded.
      for (const uint32_t index : queue_) {
        (void)index;
        ++stats_.cancelled;
        ++stats_.cancelledBeforeDecode;
      }
      queue_.clear();
      queued_.clear();
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) worker.join();
    }
    // A UI thread that never pumped leaves messages behind; free the payloads
    // here so shutdown cannot leak one block per unread thumbnail.
    if (config_.hwnd != nullptr) {
      MSG msg{};
      while (PeekMessageW(&msg, config_.hwnd, kThumbReady, kThumbReady, PM_REMOVE)) {
        delete reinterpret_cast<Payload*>(msg.lParam);
      }
    }
  }

 private:
  static size_t ResolveWorkerCount(size_t requested) {
#if defined(PRISM_C45_MUTANT_ONE_WORKER)
    (void)requested;
    return 1;  // negative control: the parallel assertion must go red
#else
    if (requested > 0) return (std::min)(requested, size_t{8});
    return DefaultWorkerCount();
#endif
  }

  Rect TileRect(uint32_t index) const {
    const int columns = config_.gridColumns > 0 ? config_.gridColumns : 1;
    const int column = static_cast<int>(index) % columns;
    const int row = static_cast<int>(index) / columns;
    Rect rect;
    rect.left = column * static_cast<int>(config_.tileWidth);
    rect.top = row * static_cast<int>(config_.tileHeight);
    rect.right = rect.left + static_cast<int>(config_.tileWidth);
    rect.bottom = rect.top + static_cast<int>(config_.tileHeight);
    rect.empty = false;
    return rect;
  }

  void WorkerLoop() {
    for (;;) {
      uint32_t index = 0;
      uint64_t ordinal = 0;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (stopping_) return;
        index = queue_.front();
        queue_.pop_front();
        queued_.erase(index);
        inFlight_[index] = false;
        ++inFlightCount_;
        if (inFlightCount_ > stats_.maxConcurrentDecodes) {
          stats_.maxConcurrentDecodes = inFlightCount_;
        }
        ordinal = dispatchSeq_++;  // 0-based dispatch order (visible priority)
        cv_.notify_all();  // WaitForConcurrentDecodes
      }

      prism::gallery::DecodedThumb thumb;
      bool threw = false;
      try {
        thumb = decode_(index, ordinal);  // WORKER THREAD ONLY
      } catch (...) {
        threw = true;
      }

      {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.decoded;
        if (threw) ++stats_.decodeFailures;
        bool discard = threw;
        auto it = inFlight_.find(index);
        if (it == inFlight_.end()) {
          discard = true;
        } else {
          discard = discard || it->second;
          inFlight_.erase(it);
        }
        if (inFlightCount_ > 0) --inFlightCount_;
        discard = discard || stopping_;
        if (!discard) {
          // The Worker -> UI hand-off is ONE PostMessage: no UI state is
          // touched here, and the payload owns the pixels until the UI takes
          // them (CONTRACTS B9).
          auto* payload = new Payload{index, std::move(thumb)};
          if (PostMessageW(config_.hwnd, kThumbReady, 0, reinterpret_cast<LPARAM>(payload))) {
            ++stats_.posted;
          } else {
            delete payload;
            ++stats_.postFailures;
          }
        } else {
          ++stats_.discarded;
        }
      }
      cv_.notify_all();
    }
  }

  Config config_;
  DecodeFn decode_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<std::thread> workers_;
  bool stopping_ = false;

  std::deque<uint32_t> queue_;
  std::unordered_set<uint32_t> queued_;
  std::unordered_map<uint32_t, bool> inFlight_;  // index -> cancelled
  size_t inFlightCount_ = 0;
  uint64_t dispatchSeq_ = 0;
  std::vector<uint32_t> visible_;
  std::unordered_set<uint32_t> visibleSet_;
  Rect dirty_;

  prism::gallery::ThumbStore store_;
  const DWORD uiThreadId_;
  Stats stats_;
};

}  // namespace prism::win::gallery
