// PrismArchive -- Windows memory benchmark (blueprint ch. 7.2, cell 2).
//
// Three claims from the blueprint/ADR-001, each turned into a number:
//
//   1. IDLE RESIDENT SET. Measured twice: this process before the engine is
//      touched (a floor), and the SHIPPED PrismArchive.exe (black box: launch
//      it, sample its working set for two seconds, close it). The app number is
//      the one the "<= 25 MB" cell must be answered with.
//
//   2. EXTRACTING A ~100 MB ARCHIVE. Peak working set while a genuine 7z.dll
//      Extract runs, sampled at 2 ms through GetProcessMemoryInfo. The number
//      says whether we stream or slurp.
//
//   3. THUMBNAIL SCAN PEAK IS INDEPENDENT OF BLOCK SIZE (contract C2, docs/PERF.md
//      invariant). Two archives are swept: one whose solid block carries ~1 MB
//      of payload, one carrying ~100 MB. The same 8 thumbnails come out of
//      both; the peaks must agree within a small slack. A mutant build
//      (PRISM_BENCH_MUTANT_BLOCK_BUFFER) materialises every item into memory --
//      the exact regression the invariant forbids -- and MUST bust it; that
//      mutant is a WILL_FAIL ctest target, so this is a guard, not arithmetic.
//
// Sampling rather than PeakWorkingSetSize: the OS peak is monotone for the whole
// process, so it cannot be attributed to one arm. A counter thread samples
// WorkingSetSize every 2 ms while each arm runs; for arms that hold megabytes
// for seconds (which is what a buffering regression looks like) that is exact
// enough, and the mutant proves it catches the case it exists for.
//
// Output:
//   BENCH_MEMORY {"idle_mb":..,"app_idle_mb":..,"sweep_small_peak_mb":..,...}
//   ALL MEMORY BENCH CHECKS PASSED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <wrl/client.h>

#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "SolidSweeper.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

int g_failures = 0;

bool CheckTrue(bool ok, const char* expr, const char* file, int line) {
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

double Mb(size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

double QpcFrequency() {
  LARGE_INTEGER f{};
  QueryPerformanceFrequency(&f);
  return static_cast<double>(f.QuadPart);
}

struct Timer {
  LARGE_INTEGER start{};
  void Begin() { QueryPerformanceCounter(&start); }
  double Ms() const {
    LARGE_INTEGER end{};
    QueryPerformanceCounter(&end);
    return (static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0) / QpcFrequency();
  }
};

size_t CurrentWorkingSet(HANDLE process) {
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!GetProcessMemoryInfo(process, &pmc, sizeof(pmc))) return 0;
  return pmc.WorkingSetSize;
}

// Samples the working set while an arm runs; the peak per arm is what the
// block-size invariant compares.
class WorkingSetSampler {
 public:
  explicit WorkingSetSampler(HANDLE process) : process_(process) {}
  ~WorkingSetSampler() { Stop(); }

  void Start() {
    stop_ = false;
    peak_ = 0;
    thread_ = std::thread([this] {
      while (!stop_) {
        const size_t now = CurrentWorkingSet(process_);
        if (now > peak_) peak_ = now;
        Sleep(2);
      }
    });
  }

  size_t Stop() {
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    // A thread that never got scheduled (or an arm shorter than one tick) would
    // otherwise report a 0 peak, i.e. a fake number in the table: fall back to
    // the live value, and the caller asserts the result is positive.
    if (peak_ == 0) peak_ = CurrentWorkingSet(process_);
    return peak_;
  }

 private:
  HANDLE process_;
  std::atomic<bool> stop_{true};
  std::atomic<size_t> peak_{0};
  std::thread thread_;
};

std::filesystem::path BenchRoot() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_BENCH_ROOT", overridePath, MAX_PATH) > 0) {
    return std::filesystem::path(overridePath);
  }
  // temp_directory_path() may carry a trailing separator, in which case
  // parent_path() is the temp dir ITSELF -- normalise first so the work root is
  // really outside %TEMP% (the zero-%TEMP% cell depends on that).
  std::filesystem::path tmp = std::filesystem::temp_directory_path();
  if (tmp.filename().empty()) tmp = tmp.parent_path();
  // Card C36: the leaf carries the pid. Three benchmark binaries share this
  // helper, and with "ctest -j" they run at the same time -- a constant
  // "prism_bench" meant one benchmark deleting another's 4 GiB archive while it
  // was being packed. (Deliberately still a literal sibling of %TEMP%, see above.)
  return tmp.parent_path() / (L"prism_bench_p" + std::to_wstring(GetCurrentProcessId()));
}

// A deterministic pseudo-random payload: random enough that no codec can turn
// it into a no-op, cheap enough to author 100 MB in well under a second.
bool WritePayload(const std::filesystem::path& path, uint64_t bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::vector<uint8_t> chunk(1u << 20);
  uint32_t state = 0x12345678u;
  uint64_t written = 0;
  bool ok = true;
  while (written < bytes) {
    for (size_t i = 0; i < chunk.size(); i += 4) {
      state = state * 1664525u + 1013904223u;
      chunk[i] = static_cast<uint8_t>(state >> 24);
      chunk[i + 1] = static_cast<uint8_t>(state >> 16);
      chunk[i + 2] = static_cast<uint8_t>(state >> 8);
      chunk[i + 3] = static_cast<uint8_t>(state);
    }
    const uint64_t left = bytes - written;
    const DWORD take = static_cast<DWORD>((std::min)(left, static_cast<uint64_t>(chunk.size())));
    DWORD done = 0;
    if (!WriteFile(file, chunk.data(), take, &done, nullptr) || done != take) {
      ok = false;
      break;
    }
    written += done;
  }
  CloseHandle(file);
  return ok;
}

struct SweepArm {
  size_t peakBytes = 0;
  double ms = 0.0;
  int decoded = 0;
  uint64_t drainedBytes = 0;
  bool ok = false;
};

// One sweep arm. The mutant path replaces the streaming sweep with "extract
// every item into memory and keep it" -- a plausible naive implementation and
// precisely the C2 violation the peak invariant exists to catch.
SweepArm RunSweepArm(prism::win::ArchiveReader& reader,
                     const std::vector<prism::ArchiveItem>& catalog,
                     const std::vector<uint32_t>& imageIndices,
                     const std::vector<uint64_t>& imageSizes, int expectedImages) {
  SweepArm arm;
  prism::win::sweep::WicThumbDecoder decoder;
  if (!decoder.Initialize()) return arm;
  WorkingSetSampler sampler(GetCurrentProcess());
  Timer timer;
  sampler.Start();
  timer.Begin();
#ifdef PRISM_BENCH_MUTANT_BLOCK_BUFFER
  std::vector<std::vector<uint8_t>> retained;
  retained.reserve(catalog.size());
  for (const auto& item : catalog) {
    if (item.isDir) continue;
    auto bytes = reader.ExtractToMemory(item.index, static_cast<size_t>(item.size) + (1u << 20));
    if (!bytes) continue;
    retained.push_back(std::move(*bytes));
  }
  arm.decoded = expectedImages;
  arm.drainedBytes = 0;
  for (const auto& item : catalog) {
    if (!item.isDir) arm.drainedBytes += item.size;
  }
  arm.ok = retained.size() >= static_cast<size_t>(expectedImages);
#else
  prism::gallery::ThumbStore store{64ull << 20};
  const auto result =
      prism::win::sweep::RunSolidSweep(reader, imageIndices, imageSizes, decoder, store);
  arm.decoded = result.decoded;
  arm.drainedBytes = result.drainedBytes;
  arm.ok = result.ok && result.decoded == expectedImages;
#endif
  arm.ms = timer.Ms();
  arm.peakBytes = sampler.Stop();
  return arm;
}

// Black box: how much memory the shipped app actually sits at.
struct AppIdle {
  bool measured = false;
  double idleMb = 0.0;
  double peakMb = 0.0;
  std::string reason;
};

std::wstring LocateAppExe() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_ARCHIVE_EXE", overridePath, MAX_PATH) > 0) {
    return std::filesystem::exists(overridePath) ? std::wstring(overridePath) : std::wstring();
  }
  wchar_t module[MAX_PATH]{};
  if (GetModuleFileNameW(nullptr, module, MAX_PATH) == 0) return {};
  const std::filesystem::path candidate =
      std::filesystem::path(module).parent_path().parent_path() / L"Src" / L"PrismWin" /
      L"PrismArchive.exe";
  return std::filesystem::exists(candidate) ? candidate.wstring() : std::wstring();
}

AppIdle SampleAppIdle(const std::wstring& appExe, const std::wstring& archive) {
  AppIdle result;
  if (appExe.empty()) {
    result.reason = "PrismArchive.exe not built (set PRISM_ARCHIVE_EXE)";
    return result;
  }
  std::wstring cmd = L"\"" + appExe + L"\" \"" + archive + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    result.reason = "CreateProcess failed";
    return result;
  }
  WaitForInputIdle(pi.hProcess, 30000);
  // Idle = after the catalog load and the first paint have settled.
  Sleep(2000);
  size_t peak = 0;
  std::vector<size_t> samples;
  for (int i = 0; i < 20; ++i) {
    const size_t now = CurrentWorkingSet(pi.hProcess);
    if (now > peak) peak = now;
    samples.push_back(now);
    Sleep(20);
  }
  std::sort(samples.begin(), samples.end());
  result.measured = !samples.empty() && samples[samples.size() / 2] > 0;
  result.idleMb = Mb(samples.empty() ? 0 : samples[samples.size() / 2]);
  result.peakMb = Mb(peak);
  if (!result.measured) result.reason = "GetProcessMemoryInfo(remote) returned 0";

  // Terminate rather than WM_CLOSE: the close path has its own cell (bench_startup,
  // and card c1 owns the bug); this benchmark must never leave a window behind.
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return result;
}

}  // namespace

#define BENCH_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define BENCH_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  // Unbuffered: a benchmark that hangs must show which step it reached (and CI
  // logs must keep the digits even if the process is killed by the timeout).
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // COM apartment guard, declared FIRST so it is destroyed LAST: every ComPtr in
  // this function must be released while the apartment is still alive. The
  // reverse order crashed the scale benchmark at exit with 0xC0000005 AFTER its
  // digits were printed -- and a ctest PASS_REGULAR_EXPRESSION cannot see that,
  // because the exit code is part of the verdict.
  struct ComApartment {
    HRESULT hr = E_FAIL;
    ComApartment() { hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED); }
    ~ComApartment() {
      if (SUCCEEDED(hr)) CoUninitialize();
    }
  } apartment;
  if (FAILED(apartment.hr)) {
    std::printf("    FAIL: COM init\n");
    return 1;
  }

  // ---- arm 0: idle working set of this process, before the engine ----
  Sleep(300);
  const size_t idleBytes = CurrentWorkingSet(GetCurrentProcess());
  std::printf("    idle working set (engine untouched): %.1f MB\n", Mb(idleBytes));

  namespace fs = std::filesystem;
  const fs::path work = BenchRoot() / "memory";
  std::error_code ec;
  fs::create_directories(work, ec);
  const std::wstring dir = work.wstring();

  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    std::printf("    FAIL: 7z.exe/7z.dll not found (set PRISM_7Z_DLL)\n");
    return 1;
  }

  // ---- fixtures: same 8 images, two payload sizes ----
  constexpr int kImages = 8;
  constexpr uint64_t kSmallPayload = 1ull << 20;         // 1 MiB
  constexpr uint64_t kBigPayload = 100ull << 20;         // 100 MiB
  std::vector<std::wstring> files;
  for (int i = 0; i < kImages; ++i) {
    const std::wstring name = L"t" + std::to_wstring(i) + L".png";
    if (!BENCH_CHECK((prism::fixture::WritePng(dir + L"\\" + name, 320, 240,
                                               static_cast<uint8_t>(16 * i),
                                               static_cast<uint8_t>(240 - 16 * i), 96)))) {
      return 1;
    }
    files.push_back(name);
  }
  const std::wstring smallBlob = L"small.bin";
  const std::wstring bigBlob = L"big.bin";
  if (!BENCH_CHECK(WritePayload(work / smallBlob, kSmallPayload)) ||
      !BENCH_CHECK(WritePayload(work / bigBlob, kBigPayload))) {
    return 1;
  }
  auto pack = [&](const std::wstring& archiveName, const std::wstring& blob) {
    std::vector<std::wstring> all = files;
    all.push_back(blob);
    // -mx0 (store) keeps the fixture cheap while STILL putting every file in one
    // solid block, which is the only property this benchmark needs.
    std::wstring args = L"a -t7z -mx0 -ms=on \"" + dir + L"\\" + archiveName + L"\"";
    for (const auto& f : all) args += L" \"" + dir + L"\\" + f + L"\"";
    return prism::fixture::RunSevenZip(exePath, args);
  };
  const std::wstring smallArchive = dir + L"\\small_payload.7z";
  const std::wstring bigArchive = dir + L"\\big_payload.7z";
  if (!BENCH_CHECK(pack(L"small_payload.7z", smallBlob))) {
    std::printf("    FAIL: pack small fixture\n");
    return 1;
  }
  if (!BENCH_CHECK(pack(L"big_payload.7z", bigBlob))) {
    std::printf("    FAIL: pack big fixture\n");
    return 1;
  }

  prism::win::SevenZipLibrary library;
  if (!BENCH_CHECK(library.Load(dllPath))) return 1;

  // ---- arm 1: Open + catalog of the 100 MB archive ----
  prism::win::ArchiveReader reader(library);
  size_t openBytes = 0;
  {
    WorkingSetSampler sampler(GetCurrentProcess());
    sampler.Start();
    const bool opened = reader.Open(bigArchive);
    const auto catalog = reader.ReadCatalog();
    openBytes = sampler.Stop();
    if (!BENCH_CHECK(opened)) {
      std::printf("    open trace: %s\n", reader.openTrace().c_str());
      return 1;
    }
    BENCH_CHECK_EQ(catalog.size(), static_cast<size_t>(kImages + 1));
  }

  std::vector<uint32_t> imageIndices;
  std::vector<uint64_t> imageSizes;
  {
    const auto catalog = reader.ReadCatalog();
    for (const auto& item : catalog) {
      if (item.isDir) continue;
      if (prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) continue;
      imageIndices.push_back(item.index);
      imageSizes.push_back(item.size);
    }
    if (!BENCH_CHECK_EQ(imageIndices.size(), static_cast<size_t>(kImages))) return 1;
  }

  // ---- arm 2/3: sweep small payload vs big payload ----
  // NOTE: "small"/"big" are rpcndr.h macros (-> char); the arms are named
  // *Arm deliberately.
  const SweepArm bigArm =
      RunSweepArm(reader, reader.ReadCatalog(), imageIndices, imageSizes, kImages);
  reader.Close();

  prism::win::ArchiveReader smallReader(library);
  if (!BENCH_CHECK(smallReader.Open(smallArchive))) {
    std::printf("    open trace: %s\n", smallReader.openTrace().c_str());
    return 1;
  }
  std::vector<uint32_t> smallIndices;
  std::vector<uint64_t> smallSizes;
  for (const auto& item : smallReader.ReadCatalog()) {
    if (item.isDir) continue;
    if (prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) continue;
    smallIndices.push_back(item.index);
    smallSizes.push_back(item.size);
  }
  const SweepArm smallArm =
      RunSweepArm(smallReader, smallReader.ReadCatalog(), smallIndices, smallSizes, kImages);
  const auto smallCatalog = smallReader.ReadCatalog();
  smallReader.Close();

  std::printf("    sweep: small payload %.3f MB peak / %.1f ms, big payload %.3f MB peak / %.1f ms\n",
              Mb(smallArm.peakBytes), smallArm.ms, Mb(bigArm.peakBytes), bigArm.ms);
  if (!BENCH_CHECK(smallArm.ok) || !BENCH_CHECK(bigArm.ok)) return 1;
  // A 0-byte peak is not a small peak, it is a broken sample (see Stop()).
  if (!BENCH_CHECK(smallArm.peakBytes > 0) || !BENCH_CHECK(bigArm.peakBytes > 0)) return 1;

  // THE invariant (docs/PERF.md, contract C2): the peak may not follow the
  // payload. 12 MiB slack covers allocator/page-granularity noise, nothing else
  // -- a buffering regression moves the peak by ~100 MiB.
  constexpr size_t kSlackBytes = 12ull << 20;
  const bool independent = bigArm.peakBytes <= smallArm.peakBytes + kSlackBytes;
  if (!BENCH_CHECK(independent)) {
    std::printf("    FAIL: sweep peak follows block size (%zu vs %zu bytes)\n", bigArm.peakBytes,
                smallArm.peakBytes);
  }

  // ---- arm 4: extract the whole 100 MB archive onto disk ----
  prism::win::ArchiveReader extractReader(library);
  if (!BENCH_CHECK(extractReader.Open(bigArchive))) return 1;
  const auto extractCatalog = extractReader.ReadCatalog();
  const fs::path outDir = work / "out";
  fs::remove_all(outDir, ec);
  fs::create_directories(outDir, ec);
  const auto plan = prism::SmartExtractionPlanner::Build(prism::win::WideToUtf8(bigArchive),
                                                         prism::win::WideToUtf8(outDir.wstring()),
                                                         extractCatalog,
                                                         [](const std::string&) { return false; });
  size_t extractPeak = 0;
  double extractMs = 0.0;
  {
    WorkingSetSampler sampler(GetCurrentProcess());
    Timer timer;
    sampler.Start();
    timer.Begin();
    prism::win::SmartExtractCallback::Result result;
    const bool ok = extractReader.ExtractWithPlan(extractCatalog, plan, plan.baseTargetDir,
                                                  nullptr, &result);
    extractMs = timer.Ms();
    extractPeak = sampler.Stop();
    if (!BENCH_CHECK(ok)) return 1;
    std::printf("    extract: %llu files / %llu bytes in %.1f ms, peak %.1f MB\n",
                static_cast<unsigned long long>(result.filesWritten),
                static_cast<unsigned long long>(result.bytesWritten), extractMs, Mb(extractPeak));
    BENCH_CHECK_EQ(result.filesWritten, static_cast<uint64_t>(kImages + 1));
  }
  extractReader.Close();

  // ---- arm 5: the shipped app's idle working set ----
  AppIdle appIdle;
  if (!smallCatalog.empty()) {
    const std::wstring appExe = LocateAppExe();
    appIdle = SampleAppIdle(appExe, smallArchive);
  }
  if (appIdle.measured) {
    std::printf("    app idle working set: %.1f MB (peak %.1f MB)\n", appIdle.idleMb,
                appIdle.peakMb);
  } else {
    std::printf("    app idle working set NOT measured: %s\n", appIdle.reason.c_str());
  }

  PROCESS_MEMORY_COUNTERS pmc{};
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));

  fs::remove_all(work / "out", ec);
  fs::remove_all(work / "big.bin", ec);

  if (g_failures > 0) {
    std::printf("    (%d check(s) failed before the digits were printed)\n", g_failures);
    return 1;
  }
  if (!BENCH_CHECK(extractPeak > 0) || !BENCH_CHECK(idleBytes > 0)) return 1;

  std::printf(
      "BENCH_MEMORY {\"idle_mb\":%.1f,\"app_idle_mb\":%.1f,\"app_idle_measured\":%s,"
      "\"open_mb\":%.1f,\"sweep_small_peak_mb\":%.1f,\"sweep_big_peak_mb\":%.1f,"
      "\"sweep_small_drained_mb\":%.1f,\"sweep_big_drained_mb\":%.1f,"
      "\"sweep_big_ms\":%.1f,\"extract_peak_mb\":%.1f,\"extract_ms\":%.1f,"
      "\"process_peak_mb\":%.1f,\"payload_mb\":%d}\n",
      Mb(idleBytes), appIdle.measured ? appIdle.idleMb : -1.0,
      appIdle.measured ? "true" : "false", Mb(openBytes), Mb(smallArm.peakBytes),
      Mb(bigArm.peakBytes), Mb(smallArm.drainedBytes), Mb(bigArm.drainedBytes), bigArm.ms,
      Mb(extractPeak), extractMs, Mb(pmc.PeakWorkingSetSize), static_cast<int>(kBigPayload >> 20));
  std::printf("ALL MEMORY BENCH CHECKS PASSED\n");
  return 0;
}
