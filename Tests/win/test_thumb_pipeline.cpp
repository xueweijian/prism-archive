// C45: the thumbnail DECODE pipeline -- a fixed worker pool, visible-first
// ordering, real cancellation, and a PostMessage-only hand-off to the UI thread.
//
// The suite asserts the three things C33/C7 left open:
//
//   1. a scroll-out CANCELS queued work before the decoder runs (the callback
//      itself is counted, so "cancelled" cannot mean "still decoded quietly");
//   2. the UI thread never decodes and never writes ThumbStore before it pumps
//      the posted message -- the worker's only door is PostMessage (B9);
//   3. hits / misses / cancelled are counted, a second request for the same
//      batch is a 100% hit, and resident pixels never exceed the ThumbStore
//      budget (C33's contract, untouched).
//
// Negative controls (CONTRACTS D3/D26): the same source is compiled twice.
//   PRISM_C45_MUTANT_NO_CANCEL   -- scroll-out keeps the queued tasks, so the
//                                   cancellation assertions must go red.
//   PRISM_C45_MUTANT_ONE_WORKER  -- the pool is one thread, so the parallelism
//                                   assertion must go red.
// Each mutant registers with a PASS regex, never WILL_FAIL: the CAUGHT line is
// printed only when the injected branch was OBSERVED at runtime AND the suite
// went red. A typo in the macro therefore prints NOT IN EFFECT and fails.
#include "ThumbnailPipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../mini_test.hpp"

namespace {

using prism::gallery::DecodedThumb;
using prism::win::gallery::ThumbnailPipeline;

constexpr uint32_t kSide = 128;
constexpr size_t kThumbBytes = static_cast<size_t>(kSide) * kSide * 4;

std::atomic<bool> g_cancelMutantObserved{false};
std::atomic<bool> g_workerMutantObserved{false};

DecodedThumb MakeThumb(uint32_t item) {
  DecodedThumb thumb;
  thumb.width = kSide;
  thumb.height = kSide;
  thumb.pixels.assign(kThumbBytes, static_cast<uint8_t>((item * 37u + 11u) & 0xFFu));
  return thumb;
}

// A one-shot gate: decoder calls park here so the test can cancel while the
// queue is still full (deterministic, no sleeps as verdicts).
struct Gate {
  void Enter() {
    std::unique_lock<std::mutex> lock(mutex_);
    ++active_;
    cv_.notify_all();
    cv_.wait(lock, [this] { return open_; });
  }
  bool WaitActive(size_t atLeast, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return active_ >= atLeast; });
  }
  void Open() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
    }
    cv_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  size_t active_ = 0;
  bool open_ = false;
};

LRESULT CALLBACK HostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == ThumbnailPipeline::kThumbReady) {
    auto* pipeline =
        reinterpret_cast<ThumbnailPipeline*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (pipeline != nullptr) {
      pipeline->OnThumbMessage(lparam);
    } else {
      delete reinterpret_cast<ThumbnailPipeline::Payload*>(lparam);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

// A message-only window is the smallest real UI thread: messages are posted to
// a genuine HWND and dispatched by a genuine window procedure.
class ThumbHost {
 public:
  ThumbHost() {
    EnsureClass();
    hwnd_ = CreateWindowExW(0, ClassName(), L"c45-thumb-host", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  ~ThumbHost() {
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
  }
  ThumbHost(const ThumbHost&) = delete;
  ThumbHost& operator=(const ThumbHost&) = delete;

  bool valid() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }
  void attach(ThumbnailPipeline* pipeline) {
    if (hwnd_ != nullptr) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pipeline));
    }
  }
  void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, hwnd_, ThumbnailPipeline::kThumbReady,
                        ThumbnailPipeline::kThumbReady, PM_REMOVE)) {
      DispatchMessageW(&msg);
    }
  }

 private:
  static const wchar_t* ClassName() { return L"PrismC45ThumbPipelineHost"; }
  static void EnsureClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &HostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = ClassName();
    if (RegisterClassExW(&wc) != 0) registered = true;
  }
  HWND hwnd_ = nullptr;
};

bool PumpUntilIdle(ThumbnailPipeline& pipeline, ThumbHost& host, int timeoutMs = 20000) {
  const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
  for (;;) {
    host.pump();
    if (pipeline.Idle()) return true;
    if (GetTickCount() > deadline) {
      host.pump();
      return pipeline.Idle();
    }
    Sleep(1);
  }
}

std::vector<uint32_t> Range(uint32_t count) {
  std::vector<uint32_t> indices;
  indices.reserve(count);
  for (uint32_t i = 0; i < count; ++i) indices.push_back(i);
  return indices;
}

size_t HardwareWorkerCount() {
  const unsigned hardware = std::thread::hardware_concurrency();
  const size_t threads = hardware == 0 ? ThumbnailPipeline::kMaxWorkers
                                       : static_cast<size_t>(hardware);
  return threads < ThumbnailPipeline::kMaxWorkers ? threads : ThumbnailPipeline::kMaxWorkers;
}

PRISM_TEST("the pool is fixed at min(4, hardware) and decodes in visible order") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  std::mutex logMutex;
  std::vector<std::pair<uint64_t, uint32_t>> calls;
  std::vector<DWORD> decodeThreads;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 16 * kThumbBytes;
  ThumbnailPipeline pipeline(config, [&](uint32_t index, uint64_t ordinal) {
    {
      std::lock_guard<std::mutex> lock(logMutex);
      calls.push_back({ordinal, index});
      decodeThreads.push_back(GetCurrentThreadId());
    }
    return MakeThumb(index);
  });
  host.attach(&pipeline);

  PRISM_CHECK_EQ(pipeline.WorkerCount(), HardwareWorkerCount());
  PRISM_CHECK(pipeline.WorkerCount() >= 2);

  const std::vector<uint32_t> visible{5, 3, 9, 1, 7, 2, 11, 0, 4, 6, 8, 10};
  pipeline.RequestVisible(visible);
  PRISM_CHECK(PumpUntilIdle(pipeline, host));

  std::sort(calls.begin(), calls.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  PRISM_CHECK_EQ(calls.size(), visible.size());
  for (size_t i = 0; i < visible.size() && i < calls.size(); ++i) {
    PRISM_CHECK_EQ(calls[i].first, static_cast<uint64_t>(i));
    PRISM_CHECK_EQ(calls[i].second, visible[i]);
  }

  const DWORD uiThread = GetCurrentThreadId();
  bool decodedOnUiThread = false;
  for (const DWORD thread : decodeThreads) {
    if (thread == uiThread) decodedOnUiThread = true;
  }
  PRISM_CHECK(!decodedOnUiThread);

  const ThumbnailPipeline::Stats stats = pipeline.Snapshot();
  PRISM_CHECK_EQ(stats.decoded, static_cast<uint64_t>(visible.size()));
  PRISM_CHECK_EQ(stats.posted, static_cast<uint64_t>(visible.size()));
  PRISM_CHECK_EQ(stats.applied, static_cast<uint64_t>(visible.size()));
  PRISM_CHECK_EQ(stats.uiThreadViolations, uint64_t{0});
}

PRISM_TEST("a second request for the same visible batch is a 100% hit") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 8 * kThumbBytes;
  ThumbnailPipeline pipeline(config,
                             [](uint32_t index, uint64_t) { return MakeThumb(index); });
  host.attach(&pipeline);

  const std::vector<uint32_t> batch{0, 1, 2, 3};
  pipeline.RequestVisible(batch);
  PRISM_CHECK(PumpUntilIdle(pipeline, host));
  const ThumbnailPipeline::Stats first = pipeline.Snapshot();
  PRISM_CHECK_EQ(first.hits, uint64_t{0});
  PRISM_CHECK_EQ(first.misses, static_cast<uint64_t>(batch.size()));
  PRISM_CHECK_EQ(first.applied, static_cast<uint64_t>(batch.size()));

  pipeline.RequestVisible(batch);
  const ThumbnailPipeline::Stats second = pipeline.Snapshot();
  PRISM_CHECK_EQ(second.hits - first.hits, static_cast<uint64_t>(batch.size()));
  PRISM_CHECK_EQ(second.misses - first.misses, uint64_t{0});
  PRISM_CHECK_EQ(second.decoded - first.decoded, uint64_t{0});
  PRISM_CHECK_EQ(pipeline.Store().Count(), batch.size());
  PRISM_CHECK(pipeline.Idle());
}

PRISM_TEST("scrolling a task out cancels it before the decoder ever runs") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  Gate gate;
  std::mutex logMutex;
  std::vector<uint32_t> decodedIndices;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 32 * kThumbBytes;
  ThumbnailPipeline pipeline(config, [&](uint32_t index, uint64_t) {
    {
      std::lock_guard<std::mutex> lock(logMutex);
      decodedIndices.push_back(index);
    }
    gate.Enter();
    return MakeThumb(index);
  });
  host.attach(&pipeline);

  constexpr uint32_t kItems = 12;
  pipeline.RequestVisible(Range(kItems));
  const size_t workers = pipeline.WorkerCount();
  PRISM_CHECK(gate.WaitActive(workers, std::chrono::milliseconds(5000)));
  PRISM_CHECK(pipeline.WaitForConcurrentDecodes(workers, std::chrono::milliseconds(5000)));

  // Only the first N tasks are still visible; the rest scrolled out.
  std::vector<uint32_t> kept;
  for (size_t i = 0; i < workers; ++i) kept.push_back(static_cast<uint32_t>(i));
  pipeline.RequestVisible(kept);

  const uint64_t expectedCancelled = static_cast<uint64_t>(kItems) - workers;
  const ThumbnailPipeline::Stats mid = pipeline.Snapshot();
  PRISM_CHECK_EQ(mid.cancelled, expectedCancelled);
  PRISM_CHECK_EQ(mid.cancelledBeforeDecode, expectedCancelled);

  gate.Open();
  PRISM_CHECK(PumpUntilIdle(pipeline, host));

  // The decoder was called exactly N times: cancelled work was NOT decoded,
  // which is the only honest reading of "cancel releases work".
  {
    std::lock_guard<std::mutex> lock(logMutex);
    PRISM_CHECK_EQ(decodedIndices.size(), workers);
    for (const uint32_t index : decodedIndices) {
      PRISM_CHECK(index < static_cast<uint32_t>(workers));
    }
  }
  const ThumbnailPipeline::Stats done = pipeline.Snapshot();
  PRISM_CHECK_EQ(done.decoded, static_cast<uint64_t>(workers));
  PRISM_CHECK_EQ(done.posted, static_cast<uint64_t>(workers));
  PRISM_CHECK_EQ(done.cancelled, expectedCancelled);

#if defined(PRISM_C45_MUTANT_NO_CANCEL)
  if (done.cancelled == 0 && done.decoded == static_cast<uint64_t>(kItems)) {
    g_cancelMutantObserved.store(true);
  }
#endif
}

PRISM_TEST("cancelling an in-flight item discards its pixels instead of posting") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  Gate gate;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 8 * kThumbBytes;
  ThumbnailPipeline pipeline(config, [&](uint32_t index, uint64_t) {
    gate.Enter();
    return MakeThumb(index);
  });
  host.attach(&pipeline);

  pipeline.RequestVisible({7});
  PRISM_CHECK(gate.WaitActive(1, std::chrono::milliseconds(5000)));
  pipeline.RequestVisible({});
  const ThumbnailPipeline::Stats mid = pipeline.Snapshot();
  PRISM_CHECK_EQ(mid.cancelledInFlight, uint64_t{1});
  PRISM_CHECK_EQ(mid.posted, uint64_t{0});

  gate.Open();
  PRISM_CHECK(PumpUntilIdle(pipeline, host));
  const ThumbnailPipeline::Stats done = pipeline.Snapshot();
  PRISM_CHECK_EQ(done.applied, uint64_t{0});
  PRISM_CHECK_EQ(done.posted, uint64_t{0});
  PRISM_CHECK_EQ(done.discarded, uint64_t{1});
  PRISM_CHECK_EQ(pipeline.Store().Count(), size_t{0});

#if defined(PRISM_C45_MUTANT_NO_CANCEL)
  if (done.cancelledInFlight == 0 && done.posted == 1) {
    g_cancelMutantObserved.store(true);
  }
#endif
}

PRISM_TEST("the pool really decodes concurrently (parallelism, not just threads)") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  Gate gate;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 16 * kThumbBytes;
  ThumbnailPipeline pipeline(config, [&](uint32_t index, uint64_t) {
    gate.Enter();
    return MakeThumb(index);
  });
  host.attach(&pipeline);

  const size_t workers = pipeline.WorkerCount();
  PRISM_CHECK(workers >= 2);
  pipeline.RequestVisible(Range(8));
  PRISM_CHECK(gate.WaitActive(workers, std::chrono::milliseconds(5000)));
  const ThumbnailPipeline::Stats stats = pipeline.Snapshot();
  PRISM_CHECK_EQ(stats.maxConcurrentDecodes, workers);
  PRISM_CHECK(stats.maxConcurrentDecodes >= 2);

#if defined(PRISM_C45_MUTANT_ONE_WORKER)
  if (workers == 1 && stats.maxConcurrentDecodes <= 1 &&
      HardwareWorkerCount() >= 2) {
    g_workerMutantObserved.store(true);
  }
#endif

  gate.Open();
  PRISM_CHECK(PumpUntilIdle(pipeline, host));
}

PRISM_TEST("resident pixels never exceed the ThumbStore budget") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 4 * kThumbBytes;
  ThumbnailPipeline pipeline(config,
                             [](uint32_t index, uint64_t) { return MakeThumb(index); });
  host.attach(&pipeline);

  pipeline.RequestVisible(Range(12));
  PRISM_CHECK(PumpUntilIdle(pipeline, host));
  for (int round = 0; round < 3; ++round) {
    const ThumbnailPipeline::Stats stats = pipeline.Snapshot();
    PRISM_CHECK(stats.residentBytes <= pipeline.Store().BudgetBytes());
    PRISM_CHECK_EQ(stats.residentBytes, pipeline.Store().SizeBytes());
    PRISM_CHECK(pipeline.Store().Count() <= size_t{4});
    PRISM_CHECK_EQ(stats.posted, stats.applied);  // pipeline holds no pixels
    pipeline.RequestVisible(Range(12));
    PRISM_CHECK(PumpUntilIdle(pipeline, host));
  }
  const ThumbnailPipeline::Stats stats = pipeline.Snapshot();
  PRISM_CHECK_EQ(stats.residentBytes, static_cast<size_t>(4) * kThumbBytes);
  PRISM_CHECK_EQ(pipeline.Store().Count(), size_t{4});
  PRISM_CHECK_EQ(stats.rejected, uint64_t{0});
}

PRISM_TEST("the UI thread turns a decoded thumb into exactly one tile rectangle") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 8 * kThumbBytes;
  config.gridColumns = 4;
  config.tileWidth = kSide;
  config.tileHeight = kSide;
  ThumbnailPipeline pipeline(config,
                             [](uint32_t index, uint64_t) { return MakeThumb(index); });
  host.attach(&pipeline);

  pipeline.RequestVisible({5});
  PRISM_CHECK(PumpUntilIdle(pipeline, host));

  ThumbnailPipeline::Rect rect;
  PRISM_CHECK(pipeline.TakeDirtyRect(&rect));
  PRISM_CHECK(!rect.empty);
  PRISM_CHECK_EQ(rect.left, 128);
  PRISM_CHECK_EQ(rect.top, 128);
  PRISM_CHECK_EQ(rect.right, 256);
  PRISM_CHECK_EQ(rect.bottom, 256);
  PRISM_CHECK(!pipeline.TakeDirtyRect(&rect));  // consumed once, not sticky
  PRISM_CHECK_EQ(pipeline.Snapshot().invalidRects, uint64_t{1});
}

PRISM_TEST("workers only post: nothing enters ThumbStore until the UI pumps") {
  ThumbHost host;
  if (!PRISM_CHECK(host.valid())) return;

  Gate gate;
  std::mutex logMutex;
  std::vector<DWORD> decodeThreads;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.budgetBytes = 8 * kThumbBytes;
  ThumbnailPipeline pipeline(config, [&](uint32_t index, uint64_t) {
    {
      std::lock_guard<std::mutex> lock(logMutex);
      decodeThreads.push_back(GetCurrentThreadId());
    }
    gate.Enter();
    return MakeThumb(index);
  });
  host.attach(&pipeline);

  pipeline.RequestVisible({2});
  PRISM_CHECK(gate.WaitActive(1, std::chrono::milliseconds(5000)));
  PRISM_CHECK_EQ(pipeline.Store().Count(), size_t{0});  // decode in progress

  gate.Open();
  PRISM_CHECK(pipeline.WaitForPosted(1, std::chrono::milliseconds(5000)));
  // The worker has posted and touched NOTHING else: the store is still empty.
  PRISM_CHECK_EQ(pipeline.Store().Count(), size_t{0});
  PRISM_CHECK_EQ(pipeline.Snapshot().applied, uint64_t{0});

  host.pump();
  PRISM_CHECK_EQ(pipeline.Store().Count(), size_t{1});
  PRISM_CHECK_EQ(pipeline.Snapshot().applied, uint64_t{1});

  const DWORD uiThread = GetCurrentThreadId();
  bool decodedOnUiThread = false;
  {
    std::lock_guard<std::mutex> lock(logMutex);
    for (const DWORD thread : decodeThreads) {
      if (thread == uiThread) decodedOnUiThread = true;
    }
  }
  PRISM_CHECK(!decodedOnUiThread);
}

}  // namespace

int main(int argc, char** argv) {
  const int result = prism::test::RunAll(argc, argv);

#if defined(PRISM_C45_MUTANT_NO_CANCEL) || defined(PRISM_C45_MUTANT_ONE_WORKER)
#if defined(PRISM_C45_MUTANT_NO_CANCEL)
  const bool armed = ThumbnailPipeline::MutationCancellationDisabled();
  const bool observed = g_cancelMutantObserved.load();
  const char* const marker = "C45 CANCEL MUTATION CAUGHT";
  const char* const name = "cancellation-disabled";
#else
  const bool armed = ThumbnailPipeline::MutationSingleWorker();
  const bool observed = g_workerMutantObserved.load();
  const char* const marker = "C45 ONE-WORKER MUTATION CAUGHT";
  const char* const name = "single-worker";
#endif
  if (!armed) {
    std::printf("C45 MUTATION NOT IN EFFECT: %s macro is declared but the branch "
                "did not compile\n", name);
    return 1;
  }
  std::printf("C45 MUTATION ACTIVE: %s (run exit code %d)\n", name, result);
  if (result != 0 && observed) {
    std::printf("%s\n", marker);
    return 0;
  }
  std::printf("C45 MUTATION NOT CAUGHT: the injected branch ran, but the suite "
              "stayed green (exit code %d, observed=%d)\n", result, observed ? 1 : 0);
  return 1;
#else
  if (result == 0) std::printf("ALL C45 THUMB-PIPELINE CHECKS PASSED\n");
  return result;
#endif
}
