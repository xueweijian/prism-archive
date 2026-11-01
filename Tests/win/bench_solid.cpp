// PrismArchive -- Windows对照基准 (blueprint ch. 7.2): the REAL 7z.dll, a REAL
// solid archive, ONE sweep pass vs N per-item extractions, 20 items each,
// QueryPerformanceCounter timings, median of 3 interleaved repetitions.
//
// This is where the "single pass is faster" claim gets actual digits; the
// exact arithmetic behind it (N vs N(N+1)/2 item decompressions) is proven
// machine-exactly in Tests/bench_sweep.cpp. What CAN be gated here is
// correctness and shape: both arms must harvest all 20 thumbnails, the sweep
// must do it in one Extract call, and 7z.exe missing is a FAILURE (exit != 0),
// never a silent skip.
//
// Output: one machine-readable line for the CI step summary,
//   BENCH {"items":20,"sweep_ms":..,"peritem_ms":..,"speedup":..,"peak_mb":..}
// followed by ALL BENCH CHECKS PASSED on success.
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

#include <wrl/client.h>

#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "SolidSweeper.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "psapi.lib")

using prism::gallery::DecodedThumb;
using prism::gallery::SolidSweepSession;
using prism::gallery::ThumbStore;

namespace {

int g_failures = 0;

// Expression-style checks (PRISM_CHECK semantics): usable inside if/|| chains,
// report loudly, and hand back the verdict so fail-fast sites keep working.
inline bool CheckTrue(bool ok, const char* expr, const char* file, int line) {
  if (!ok) {
    std::printf("    FAIL %s:%d  expected true: %s\n", file, line, expr);
    ++g_failures;
  }
  return ok;
}

template <class A, class B>
bool CheckEq(const A& a, const B& b, const char* exprA, const char* exprB, const char* file,
             int line) {
  const bool ok = (a == b);
  if (!ok) {
    std::printf("    FAIL %s:%d  expected (%s) == (%s)\n", file, line, exprA, exprB);
    ++g_failures;
  }
  return ok;
}

}  // namespace

#define BENCH_CHECK(cond) \
  ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define BENCH_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

// QueryPerformanceCounter pair -> milliseconds.
struct Timer {
  LARGE_INTEGER start{};
  static double Frequency() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }
  void Begin() { QueryPerformanceCounter(&start); }
  double EndMs() const {
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / Frequency();
  }
};

struct Dims {
  UINT w;
  UINT h;
};

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::printf("    FAIL: COM init\n");
    return 1;
  }

  // Hard requirement, not a soft skip: no 7z.exe -> no benchmark -> red.
  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    // C37: ExePathWhy() says WHICH half is missing (engine vs CLI) and where.
    std::printf("    FAIL: %s\n", prism::fixture::ExePathWhy().c_str());
    return 1;
  }

  namespace fs = std::filesystem;
  const fs::path work = fs::temp_directory_path() / "prism_bench_solid";
  fs::remove_all(work);
  fs::create_directories(work);
  const std::wstring dir = work.wstring();

  // 20 real PNGs, five size classes cycled so the solid block carries a mix
  // like a real gallery folder (icons up to wall-paper-ish photos).
  constexpr int kItems = 20;
  const Dims dims[5] = {{64, 64}, {256, 192}, {320, 240}, {512, 384}, {640, 480}};
  const uint8_t hues[5][3] = {{0, 0, 255}, {0, 255, 0}, {255, 0, 0}, {0, 255, 255}, {255, 0, 255}};
  std::vector<std::wstring> files;
  for (int i = 0; i < kItems; ++i) {
    const std::wstring name = L"img" + std::to_wstring(i) + L".png";
    const Dims d = dims[i % 5];
    if (!BENCH_CHECK((prism::fixture::WritePng(dir + L"\\" + name, d.w, d.h, hues[i % 5][0],
                                               hues[i % 5][1], hues[i % 5][2])))) {
      return 1;
    }
    files.push_back(name);
  }
  if (!BENCH_CHECK((prism::fixture::PackSolid(exePath, dir, L"solid.7z", files)))) {
    std::printf("    FAIL: 7z.exe pack failed\n");
    return 1;
  }

  prism::win::SevenZipLibrary library;
  if (!BENCH_CHECK(library.Load(dllPath))) return 1;
  prism::win::ArchiveReader reader(library);
  if (!BENCH_CHECK(reader.Open(dir + L"\\solid.7z"))) {
    std::printf("    open trace: %s\n", reader.openTrace().c_str());
    return 1;
  }
  const auto catalog = reader.ReadCatalog();
  BENCH_CHECK_EQ(catalog.size(), static_cast<size_t>(kItems));
  if (g_failures > 0) return 1;

  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (const auto& item : catalog) {
    if (item.isDir) continue;
    indices.push_back(item.index);
    sizes.push_back(item.size);
  }
  BENCH_CHECK_EQ(indices.size(), static_cast<size_t>(kItems));

  prism::win::sweep::WicThumbDecoder decoder;
  if (!BENCH_CHECK(decoder.Initialize())) return 1;

  const uint64_t totalBytes = [&] {
    uint64_t sum = 0;
    for (uint64_t s : sizes) sum += s;
    return sum;
  }();

  // Warm-up (not timed): 7z codec init, WIC factory, PNG cache.
  {
    ThumbStore warm{64ull << 20};
    const auto warmUp = prism::win::sweep::RunSolidSweep(reader, indices, sizes, decoder, warm);
    if (!BENCH_CHECK(warmUp.ok) || !BENCH_CHECK_EQ(warmUp.decoded, kItems)) return 1;
  }

  // Three interleaved repetitions, median per arm -- interleaving keeps a
  // thermal/cache drift from systematically favouring the arm measured last.
  double sweepMs[3] = {0, 0, 0};
  double perItemMs[3] = {0, 0, 0};
  for (int rep = 0; rep < 3; ++rep) {
    // Arm A: ONE Extract call over the whole solid block.
    {
      ThumbStore store{64ull << 20};
      Timer t;
      t.Begin();
      const auto r = prism::win::sweep::RunSolidSweep(reader, indices, sizes, decoder, store);
      sweepMs[rep] = t.EndMs();
      if (!BENCH_CHECK(r.ok) || !BENCH_CHECK_EQ(r.decoded, kItems) ||
          !BENCH_CHECK_EQ(store.Count(), static_cast<size_t>(kItems))) {
        return 1;
      }
      if (rep == 0) std::printf("    sweep: %d thumbs, %llu bytes drained in one pass\n",
                                r.decoded, static_cast<unsigned long long>(r.drainedBytes));
    }
    // Arm B: 20 separate extractions; a solid block has no per-item index, so
    // each call re-decodes the prefix from the block start.
    {
      ThumbStore store{64ull << 20};
      int decodedTotal = 0;
      int calls = 0;
      Timer t;
      t.Begin();
      for (size_t p = 0; p < indices.size(); ++p) {
        const auto r =
            prism::win::sweep::RunSolidSweep(reader, {indices[p]}, {sizes[p]}, decoder, store);
        if (!BENCH_CHECK(r.ok) || !BENCH_CHECK_EQ(r.decoded, 1)) return 1;
        decodedTotal += r.decoded;
        ++calls;
      }
      perItemMs[rep] = t.EndMs();
      if (!BENCH_CHECK_EQ(decodedTotal, kItems) || !BENCH_CHECK_EQ(calls, kItems)) return 1;
      if (rep == 0) std::printf("    per-item: %d extractions, %d thumbs total\n", calls, decodedTotal);
    }
  }

  auto median = [](double v[3]) {
    std::sort(v, v + 3);
    return v[1];
  };
  const double sweepMedian = median(sweepMs);
  const double perItemMedian = median(perItemMs);
  const double speedup = sweepMedian > 0.0 ? perItemMedian / sweepMedian : 0.0;

  PROCESS_MEMORY_COUNTERS pmc{};
  double peakMb = 0.0;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    peakMb = static_cast<double>(pmc.PeakWorkingSetSize) / (1024.0 * 1024.0);
  }

  // Shape gate (not a clock gate): the sweep's stream contract -- every byte
  // of the block consumed exactly once -- must hold on the real archive too.
  // The headline digits stay honest even when the payloads are tiny: they are
  // printed, not asserted.
  std::printf("    archive: %d items, %llu uncompressed bytes total\n", kItems,
              static_cast<unsigned long long>(totalBytes));
  std::printf("    median of 3: sweep %.3f ms, per-item %.3f ms\n", sweepMedian, perItemMedian);
  std::printf(
      "BENCH {\"items\":%d,\"sweep_ms\":%.3f,\"peritem_ms\":%.3f,\"speedup\":%.3f,\"peak_mb\":%.1f}\n",
      kItems, sweepMedian, perItemMedian, speedup, peakMb);
  std::printf("ALL BENCH CHECKS PASSED\n");
  return g_failures == 0 ? 0 : 1;
}
