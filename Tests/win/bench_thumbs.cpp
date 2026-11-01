// C45 bench: 800 thumbnails at 128x128, SERIAL vs PARALLEL through the real
// ThumbnailPipeline (worker pool + PostMessage hand-off + ThumbStore budget).
//
// What is measured: wall-clock of "request the batch -> every posted payload
// has been applied by the UI thread". What is NOT asserted: those durations.
// A shared runner cannot gate a stopwatch (docs/PERF.md), so the numbers are
// REPORTED in one BENCH_THUMBS JSON line and the assertions are structural:
// every item decoded once, every payload applied once, the second request is a
// 100% hit, resident pixels stay inside the budget, and the pixels produced by
// the parallel run are BYTE-IDENTICAL to the serial run (D2: content, not
// existence).
//
// The decode body is a deterministic synthetic thumbnail (integer hashing per
// pixel). The point of this card is the SCHEDULER, not a codec: a real WIC
// decode would add an I/O dependency and hide the parallelism being measured.
#include "ThumbnailPipeline.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using prism::gallery::DecodedThumb;
using prism::win::gallery::ThumbnailPipeline;

constexpr uint32_t kSide = 128;
constexpr size_t kThumbBytes = static_cast<size_t>(kSide) * kSide * 4;
constexpr uint32_t kItems = 800;
constexpr size_t kBudgetBytes = 64u * 1024u * 1024u;

int g_failures = 0;

bool Check(bool condition, const char* what) {
  if (!condition) {
    ++g_failures;
    std::printf("    FAIL: %s\n", what);
  }
  return condition;
}

DecodedThumb SyntheticDecode(uint32_t item, uint64_t ordinal) {
  (void)ordinal;
  DecodedThumb thumb;
  thumb.width = kSide;
  thumb.height = kSide;
  thumb.pixels.resize(kThumbBytes);
  uint32_t seed = item * 0x9E3779B1u + 0x1234567u;
  uint8_t* out = thumb.pixels.data();
  for (uint32_t y = 0; y < kSide; ++y) {
    for (uint32_t x = 0; x < kSide; ++x) {
      uint32_t h = seed ^ (x * 0x85EBCA77u) ^ (y * 0xC2B2AE3Du);
      h ^= h >> 13;
      h *= 0x27D4EB2Fu;
      h ^= h >> 15;
      out[0] = static_cast<uint8_t>(h & 0xFFu);
      out[1] = static_cast<uint8_t>((h >> 8) & 0xFFu);
      out[2] = static_cast<uint8_t>((h >> 16) & 0xFFu);
      out[3] = 0xFFu;
      out += 4;
    }
  }
  return thumb;
}

LRESULT CALLBACK BenchHostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
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

class BenchHost {
 public:
  BenchHost() {
    ensureClass();
    hwnd_ = CreateWindowExW(0, className(), L"c45-bench-thumbs", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  ~BenchHost() {
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
  }
  BenchHost(const BenchHost&) = delete;
  BenchHost& operator=(const BenchHost&) = delete;

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
  static const wchar_t* className() { return L"PrismC45ThumbBenchHost"; }
  static void ensureClass() {
    static bool registered = false;
    if (registered) return;
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &BenchHostProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = className();
    if (RegisterClassExW(&wc) != 0) registered = true;
  }
  HWND hwnd_ = nullptr;
};

bool PumpUntilIdle(ThumbnailPipeline& pipeline, BenchHost& host, int timeoutMs = 120000) {
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

uint64_t DigestStore(ThumbnailPipeline& pipeline) {
  uint64_t hash = 1469598103934665603ull;  // FNV-1a 64
  for (uint32_t index = 0; index < kItems; ++index) {
    const DecodedThumb* thumb = pipeline.Store().Lookup(index);
    if (thumb == nullptr) return 0;
    for (const uint8_t byte : thumb->pixels) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

struct RunResult {
  double ms = 0.0;
  size_t workers = 0;
  uint64_t decoded = 0;
  uint64_t applied = 0;
  uint64_t secondPassHits = 0;
  size_t residentBytes = 0;
  uint64_t digest = 0;
  bool ok = false;
};

RunResult RunBatch(BenchHost& host, size_t workerCount, const char* label) {
  RunResult result;
  ThumbnailPipeline::Config config;
  config.hwnd = host.hwnd();
  config.workerCount = workerCount;
  config.budgetBytes = kBudgetBytes;
  ThumbnailPipeline pipeline(config,
                             [](uint32_t index, uint64_t ordinal) {
                               return SyntheticDecode(index, ordinal);
                             });
  host.attach(&pipeline);
  result.workers = pipeline.WorkerCount();

  std::vector<uint32_t> all;
  all.reserve(kItems);
  for (uint32_t i = 0; i < kItems; ++i) all.push_back(i);

  const auto begin = std::chrono::steady_clock::now();
  pipeline.RequestVisible(all);
  const bool idle = PumpUntilIdle(pipeline, host);
  const auto end = std::chrono::steady_clock::now();
  result.ms = std::chrono::duration<double, std::milli>(end - begin).count();

  const ThumbnailPipeline::Stats first = pipeline.Snapshot();
  result.decoded = first.decoded;
  result.applied = first.applied;
  result.residentBytes = first.residentBytes;
  result.digest = DigestStore(pipeline);
  result.ok = idle;

  // Second pass over the same batch: must be served from ThumbStore with no
  // decoder call at all (the 100% hit assertion, measured not assumed).
  pipeline.RequestVisible(all);
  result.ok = PumpUntilIdle(pipeline, host) && result.ok;
  const ThumbnailPipeline::Stats second = pipeline.Snapshot();
  result.secondPassHits = second.hits - first.hits;
  result.ok = result.ok && (second.decoded == first.decoded);

  std::printf("    %s: %.3f ms, workers=%zu, decoded=%llu, applied=%llu, "
              "second_pass_hits=%llu, resident=%zu B\n",
              label, result.ms, result.workers,
              static_cast<unsigned long long>(result.decoded),
              static_cast<unsigned long long>(result.applied),
              static_cast<unsigned long long>(result.secondPassHits),
              result.residentBytes);
  return result;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const size_t hardware = std::thread::hardware_concurrency();
  std::printf("    hardware threads: %zu, pool (default config): %zu, items: %u, "
              "side: %u, budget: %zu B\n",
              hardware, ThumbnailPipeline::DefaultWorkerCount(),
              static_cast<unsigned>(kItems), static_cast<unsigned>(kSide), kBudgetBytes);

  BenchHost host;
  if (!Check(host.valid(), "message-only host window")) return 1;

  const RunResult serial = RunBatch(host, 1, "serial");
  const RunResult parallel = RunBatch(host, 0, "parallel");

  Check(serial.decoded == kItems, "serial decoded every item exactly once");
  Check(parallel.decoded == kItems, "parallel decoded every item exactly once");
  Check(serial.applied == kItems, "serial applied every payload exactly once");
  Check(parallel.applied == kItems, "parallel applied every payload exactly once");
  Check(serial.secondPassHits == kItems, "serial second pass is a 100% hit");
  Check(parallel.secondPassHits == kItems, "parallel second pass is a 100% hit");
  Check(serial.residentBytes <= kBudgetBytes, "serial resident within budget");
  Check(parallel.residentBytes <= kBudgetBytes, "parallel resident within budget");
  Check(serial.residentBytes == kItems * kThumbBytes, "serial holds all 800 thumbs");
  Check(parallel.digest == serial.digest && serial.digest != 0,
        "parallel pixels are byte-identical to serial pixels");
  Check(parallel.workers >= 2, "parallel run really used more than one worker");

  if (g_failures != 0) {
    std::printf("    (%d bench check(s) failed)\n", g_failures);
    return 1;
  }

  const double speedup = parallel.ms > 0.0 ? serial.ms / parallel.ms : 0.0;
  std::printf(
      "BENCH_THUMBS {\"items\":%u,\"side\":%u,\"thumb_bytes\":%zu,"
      "\"serial_ms\":%.3f,\"parallel_ms\":%.3f,\"speedup\":%.3f,"
      "\"serial_workers\":%zu,\"parallel_workers\":%zu,"
      "\"decoded\":%llu,\"applied\":%llu,\"second_pass_hits\":%llu,"
      "\"resident_bytes\":%zu,\"budget_bytes\":%zu,"
      "\"pixel_digest\":\"%016llx\"}\n",
      static_cast<unsigned>(kItems), static_cast<unsigned>(kSide), kThumbBytes,
      serial.ms, parallel.ms, speedup, serial.workers, parallel.workers,
      static_cast<unsigned long long>(parallel.decoded),
      static_cast<unsigned long long>(parallel.applied),
      static_cast<unsigned long long>(parallel.secondPassHits),
      parallel.residentBytes, kBudgetBytes,
      static_cast<unsigned long long>(parallel.digest));
  std::printf("ALL C45 THUMB BENCH CHECKS PASSED\n");
  return 0;
}
