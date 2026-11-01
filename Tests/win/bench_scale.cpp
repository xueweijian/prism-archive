// PrismArchive -- Windows scale benchmark (blueprint ch. 7.2: 10 000-entry
// directory, 4 GB archive with zero %TEMP% traffic, 100 000-row list scrolling).
//
//   A. 10 000 entries. A genuine 10 000-file 7z is packed once by 7z.exe, then
//      Open() and ReadCatalog() are timed (median of 3 fresh readers). The
//      catalog COUNT is asserted exactly -- a timing without the content check
//      would pass on an empty catalog (contract D2).
//
//   B. 4 GB archive, zero %TEMP% writes. TEMP/TMP are redirected to a dedicated
//      EMPTY directory for the arm, so the snapshot attributes writes to this
//      process tree instead of to whatever else the machine is doing. Then a
//      full sweep (4 GB drained through 7z.dll) and a full ExtractWithPlan of
//      the same archive run; afterwards the directory must still be empty. The
//      mutant build (PRISM_BENCH_MUTANT_TEMP_WRITE) makes an intermediate copy
//      into that directory -- exactly the "extract to %TEMP% then move" design
//      this cell forbids -- and MUST go red; it is a WILL_FAIL ctest target.
//
//   C. 100 000-row list. The REAL renderer (GalleryRenderer, list mode) paints
//      a 1200x800 WIC target frame by frame; per-frame cost is reported with its
//      median/p95 and the implied FPS. What is NOT measured -- and is stated as
//      unsupported rather than faked -- is PRESENTED frame rate: the window has
//      no benchmark injection point (GalleryWindow is card c1's file), so there
//      is no vsync/composition number here. The virtualization guard IS exact:
//      the rows drawn per frame must stay bounded and identical at 1 000 and
//      100 000 rows, which is what makes the scroll cost independent of N.
//
// Output:
//   BENCH_SCALE {"open_10k_ms":..,"catalog_10k_ms":..,"entries":10000,
//                "sweep_4gb_ms":..,"extract_4gb_ms":..,"temp_files":0,
//                "list_frame_ms":..,"list_fps":..,"rows_drawn":..}
//   ALL SCALE BENCH CHECKS PASSED
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

#include "GalleryRenderer.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "SolidSweeper.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

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
  return tmp.parent_path() / L"prism_bench";
}

// Zero-filled payload: 4 GiB of it is authored in a few seconds and packs with
// -mx0 (store) in one pass. Content is irrelevant here -- the cell is about
// bytes moving and where they do NOT land.
bool WriteZeros(const std::filesystem::path& path, uint64_t bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::vector<uint8_t> chunk(4u << 20, 0);
  uint64_t written = 0;
  bool ok = true;
  while (written < bytes) {
    const DWORD take =
        static_cast<DWORD>((std::min)(bytes - written, static_cast<uint64_t>(chunk.size())));
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

bool RunSevenZipIn(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd,
                   DWORD timeoutMs) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  const DWORD waited = WaitForSingleObject(pi.hProcess, timeoutMs);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  if (waited != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return waited == WAIT_OBJECT_0 && code == 0;
}

// Every entry under a directory as "relpath|size", sorted. Empty vector == empty
// directory; used for the zero-%TEMP% assertion.
std::vector<std::string> SnapshotTree(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  std::vector<std::string> entries;
  std::error_code ec;
  if (!fs::exists(root, ec)) return entries;
  for (const auto& entry : fs::recursive_directory_iterator(root, ec)) {
    const auto rel = fs::relative(entry.path(), root, ec).string();
    std::string line = rel;
    if (entry.is_regular_file(ec)) {
      line += "|" + std::to_string(entry.file_size(ec));
    } else if (entry.is_directory(ec)) {
      line += "|dir";
    }
    entries.push_back(line);
  }
  std::sort(entries.begin(), entries.end());
  return entries;
}

// The list-mode frame harness: real GalleryRenderer, real DWrite, software D2D
// target so no display is needed.
class ListFrameBench {
 public:
  bool Initialize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(wic_.ReleaseAndGetAddressOf())))) {
      return false;
    }
    if (FAILED(wic_->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand, bitmap_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(renderer_.Initialize())) return false;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(renderer_.Factory()->CreateWicBitmapRenderTarget(
            bitmap_.Get(), props, target_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
  }

  // One frame at the given scroll offset. Returns the drawn-row count so the
  // virtualization bound can be asserted on real work, not on a model.
  uint32_t DrawFrame(prism::win::gallery::GalleryRenderer::Scene& scene, uint32_t scroll) {
    scene.listScroll = scroll;
    const uint32_t viewportH = static_cast<uint32_t>((std::max)(0.0f, (height_ - 56.0f) -
                                                                         (68.0f + 36.0f)));
    const auto window = prism::list::RowWindow(scroll, viewportH,
                                               static_cast<uint32_t>(scene.listItems->size()));
    target_->BeginDraw();
    renderer_.Draw(target_.Get(), scene);
    target_->EndDraw();
    return window.last - window.first + 1;
  }

  // The frame really painted (D2: content, not existence).
  bool BackdropPainted() {
    WICRect rect{0, 0, static_cast<INT>(width_), static_cast<INT>(height_)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap_->Lock(&rect, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
    UINT stride = 0, size = 0;
    BYTE* buffer = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &buffer)) ||
        buffer == nullptr) {
      return false;
    }
    // Corner pixel = backdrop (#0B0E13 as BGR = 13 0E 0B).
    const BYTE* px = buffer;
    return px[0] == 0x13 && px[1] == 0x0E && px[2] == 0x0B;
  }

 private:
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  ComPtr<IWICImagingFactory> wic_;
  ComPtr<IWICBitmap> bitmap_;
  ComPtr<ID2D1RenderTarget> target_;
  prism::win::gallery::GalleryRenderer renderer_;
};

double Percentile(std::vector<double>& values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

struct ListArm {
  double medianMs = 0.0;
  double p95Ms = 0.0;
  uint32_t maxRows = 0;
  bool painted = false;
};

ListArm RunListArm(uint32_t rowCount, uint32_t frames) {
  ListArm arm;
  std::vector<prism::list::SortItem> rows;
  rows.reserve(rowCount);
  for (uint32_t i = 0; i < rowCount; ++i) {
    prism::list::SortItem row;
    row.name = "photo_" + std::to_string(i) + ".png";
    row.size = 1'048'576ull + i;
    row.packedSize = 262'144ull + i;
    row.kind = prism::PreviewKind::Image;
    rows.push_back(std::move(row));
  }
  ListFrameBench bench;
  if (!bench.Initialize(1200, 800)) return arm;

  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.listItems = &rows;
  scene.selectedRow = -1;

  std::vector<double> frameMs;
  frameMs.reserve(frames);
  const double span = static_cast<double>(rowCount) * static_cast<double>(prism::list::kRowHeight);
  for (uint32_t i = 0; i < frames; ++i) {
    // Spread the scroll across the WHOLE list so every frame lands in a
    // different part of it (a 100k list cannot be "warm").
    const uint32_t scroll = static_cast<uint32_t>(span * static_cast<double>(i) /
                                                  static_cast<double>(frames));
    Timer timer;
    timer.Begin();
    const uint32_t drawn = bench.DrawFrame(scene, scroll);
    frameMs.push_back(timer.Ms());
    if (drawn > arm.maxRows) arm.maxRows = drawn;
  }
  arm.medianMs = Percentile(frameMs, 0.5);
  arm.p95Ms = Percentile(frameMs, 0.95);
  arm.painted = bench.BackdropPainted();
  return arm;
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

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path work = BenchRoot() / "scale";
  fs::create_directories(work, ec);

  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    std::printf("    FAIL: 7z.exe/7z.dll not found (set PRISM_7Z_DLL)\n");
    return 1;
  }
  prism::win::SevenZipLibrary library;
  if (!BENCH_CHECK(library.Load(dllPath))) return 1;

  // ------------------------------------------------------------------ A. 10k
  constexpr uint32_t kEntries = 10000;
  const fs::path tree = work / "tree10k";
  const fs::path archive10k = work / "entries10k.7z";
  if (!fs::exists(archive10k, ec)) {
    fs::create_directories(tree, ec);
    std::wstring listPath = (work / "list10k.txt").wstring();
    HANDLE list = CreateFileW(listPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (!BENCH_CHECK(list != INVALID_HANDLE_VALUE)) return 1;
    std::string listText;
    for (uint32_t i = 0; i < kEntries; ++i) {
      char name[32];
      std::snprintf(name, sizeof(name), "f%05u.txt", i);
      const fs::path file = tree / name;
      HANDLE f = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE) {
        CloseHandle(list);
        std::printf("    FAIL: cannot create fixture file %s\n", file.string().c_str());
        return 1;
      }
      const char payload[] = "prism\n";
      DWORD written = 0;
      WriteFile(f, payload, sizeof(payload) - 1, &written, nullptr);
      CloseHandle(f);
      listText += std::string(name) + "\n";
    }
    DWORD written = 0;
    WriteFile(list, listText.data(), static_cast<DWORD>(listText.size()), &written, nullptr);
    CloseHandle(list);
    // @list keeps the command line far below the 32 KB limit; cwd=tree makes the
    // stored names flat ("f00000.txt"), which is what ReadCatalog asserts on.
    const std::wstring args =
        L"a -t7z -mx1 -ms=on \"" + archive10k.wstring() + L"\" @\"" + listPath + L"\"";
    if (!BENCH_CHECK(RunSevenZipIn(exePath, args, tree.wstring(), 600000))) {
      std::printf("    FAIL: packing 10k fixture\n");
      return 1;
    }
  }

  std::vector<double> openMs, catalogMs;
  size_t entries = 0;
  for (int rep = 0; rep < 3; ++rep) {
    prism::win::ArchiveReader reader(library);
    Timer timer;
    timer.Begin();
    const bool opened = reader.Open(archive10k.wstring());
    openMs.push_back(timer.Ms());
    if (!BENCH_CHECK(opened)) {
      std::printf("    open trace: %s\n", reader.openTrace().c_str());
      return 1;
    }
    Timer catalogTimer;
    catalogTimer.Begin();
    const auto catalog = reader.ReadCatalog();
    catalogMs.push_back(catalogTimer.Ms());
    entries = catalog.size();
    reader.Close();
  }
  BENCH_CHECK_EQ(entries, static_cast<size_t>(kEntries));
  const double open10k = Percentile(openMs, 0.5);
  const double catalog10k = Percentile(catalogMs, 0.5);
  std::printf("    10k: open %.3f ms, catalog %.3f ms, %zu entries\n", open10k, catalog10k,
              entries);

  // ------------------------------------------------------------- B. 4 GB
  // The cell asks for 4 GB. The fixture costs ~2x its size on disk (payload +
  // archive), and a hosted runner does not have that headroom, so the size is
  // the LARGEST of {1,2,3,4} GB that fits in an eighth of free space. The actual
  // size is printed as "big_gb" -- the number is never allowed to imply 4 GB when
  // it measured 1 GB. Override with PRISM_BENCH_BIG_GB.
  uint32_t bigGb = 0;
  {
    wchar_t value[32]{};
    if (GetEnvironmentVariableW(L"PRISM_BENCH_BIG_GB", value, 32) > 0) {
      const int parsed = static_cast<int>(wcstol(value, nullptr, 10));
      if (parsed > 0) bigGb = static_cast<uint32_t>(parsed);
    }
  }
  if (bigGb == 0) {
    ULARGE_INTEGER freeBytes{};
    const std::wstring workRoot = work.wstring();
    if (GetDiskFreeSpaceExW(workRoot.c_str(), &freeBytes, nullptr, nullptr)) {
      const uint64_t budget = freeBytes.QuadPart / 8;
      for (uint32_t candidate = 4; candidate >= 1; --candidate) {
        if (static_cast<uint64_t>(candidate) << 30 <= budget) {
          bigGb = candidate;
          break;
        }
      }
    }
    if (bigGb == 0) bigGb = 1;
    std::printf("    big fixture size auto-selected: %u GB (free-space budget)\n", bigGb);
  }
  const uint64_t bigBytes = static_cast<uint64_t>(bigGb) << 30;
  const fs::path bigFile = work / "big.bin";
  const fs::path archiveBig = work / ("big" + std::to_string(bigGb) + "gb.7z");
  // The fixture is cached across runs (packing 4 GiB is minutes), but a cached
  // file is only reusable if it actually OPENS: an interrupted pack leaves a
  // truncated archive, and reusing one silently turns every cell below into a
  // failure of the fixture rather than of the product. Validate, then repack.
  bool bigFixtureReady = false;
  if (fs::exists(archiveBig, ec)) {
    prism::win::ArchiveReader probe(library);
    bigFixtureReady = probe.Open(archiveBig.wstring());
    probe.Close();
    if (!bigFixtureReady) {
      std::printf("    stale %s does not open -- repacking\n", archiveBig.string().c_str());
      fs::remove(archiveBig, ec);
    }
  }
  if (!bigFixtureReady) {
    if (!BENCH_CHECK(WriteZeros(bigFile, bigBytes))) {
      std::printf("    FAIL: cannot author %u GB payload\n", bigGb);
      return 1;
    }
    const std::wstring args = L"a -t7z -mx0 -ms=on \"" + archiveBig.wstring() + L"\" \"" +
                              bigFile.wstring() + L"\"";
    if (!BENCH_CHECK(RunSevenZipIn(exePath, args, work.wstring(), 1800000))) {
      std::printf("    FAIL: packing %u GB fixture\n", bigGb);
      return 1;
    }
    fs::remove(bigFile, ec);  // the archive carries the payload now
    bigFixtureReady = true;
  }
  (void)bigFixtureReady;

  // Controlled %TEMP%: an empty directory owned by this arm.
  const fs::path controlledTemp = work / "temp_probe";
  fs::remove_all(controlledTemp, ec);
  fs::create_directories(controlledTemp, ec);
  SetEnvironmentVariableW(L"TEMP", controlledTemp.wstring().c_str());
  SetEnvironmentVariableW(L"TMP", controlledTemp.wstring().c_str());
  const auto tempBefore = SnapshotTree(controlledTemp);
  BENCH_CHECK_EQ(tempBefore.size(), static_cast<size_t>(0));

  prism::win::ArchiveReader bigReader(library);
  Timer bigOpenTimer;
  bigOpenTimer.Begin();
  if (!BENCH_CHECK(bigReader.Open(archiveBig.wstring()))) {
    std::printf("    open trace: %s\n", bigReader.openTrace().c_str());
    return 1;
  }
  const double bigOpenMs = bigOpenTimer.Ms();
  const auto bigCatalog = bigReader.ReadCatalog();
  BENCH_CHECK_EQ(bigCatalog.size(), static_cast<size_t>(1));

  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (const auto& item : bigCatalog) {
    if (item.isDir) continue;
    indices.push_back(item.index);
    sizes.push_back(item.size);
  }
  prism::win::sweep::WicThumbDecoder decoder;
  if (!BENCH_CHECK(decoder.Initialize())) return 1;
  prism::gallery::ThumbStore store{64ull << 20};
  Timer sweepTimer;
  sweepTimer.Begin();
  const auto sweep =
      prism::win::sweep::RunSolidSweep(bigReader, indices, sizes, decoder, store, 0);
  const double sweepMs = sweepTimer.Ms();
  // thumbnail budget 0 => nothing decoded, everything drained: this arm is about
  // consuming the 4 GB, not about pixels.
  if (!BENCH_CHECK(sweep.ok)) return 1;

  const auto plan = prism::SmartExtractionPlanner::Build(
      prism::win::WideToUtf8(archiveBig.wstring()),
      prism::win::WideToUtf8((work / "out").wstring()), bigCatalog,
      [](const std::string&) { return false; });
  fs::remove_all(work / "out", ec);
  fs::create_directories(work / "out", ec);
  Timer extractTimer;
  extractTimer.Begin();
  prism::win::SmartExtractCallback::Result result;
  const bool extracted =
      bigReader.ExtractWithPlan(bigCatalog, plan, plan.baseTargetDir, nullptr, &result);
  const double extractMs = extractTimer.Ms();
  bigReader.Close();
  if (!BENCH_CHECK(extracted)) return 1;
  BENCH_CHECK_EQ(result.bytesWritten, bigBytes);

#ifdef PRISM_BENCH_MUTANT_TEMP_WRITE
  // The forbidden design: stage the archive through %TEMP% first.
  {
    const fs::path staging = controlledTemp / "staging.7z";
    fs::copy_file(archiveBig, staging, fs::copy_options::overwrite_existing, ec);
  }
#endif

  const auto tempAfter = SnapshotTree(controlledTemp);
  std::printf("    4 GB: open %.1f ms, sweep %.1f ms, extract %.1f ms, temp entries %zu -> %zu\n",
              bigOpenMs, sweepMs, extractMs, tempBefore.size(), tempAfter.size());
  for (const auto& entry : tempAfter) std::printf("      temp: %s\n", entry.c_str());
  if (!BENCH_CHECK_EQ(tempAfter.size(), static_cast<size_t>(0))) {
    std::printf("    FAIL: the pipeline wrote into %%TEMP%% (%zu entries)\n", tempAfter.size());
  }
  BENCH_CHECK_EQ(result.filesWritten, static_cast<uint64_t>(1));
  fs::remove_all(work / "out", ec);

  // ------------------------------------------------------------- C. 100k rows
  const ListArm rows1k = RunListArm(1000, 60);
  const ListArm rows100k = RunListArm(100000, 120);
  std::printf("    100k rows: median %.3f ms/frame (p95 %.3f), max rows drawn %u, painted=%d\n",
              rows100k.medianMs, rows100k.p95Ms, rows100k.maxRows,
              static_cast<int>(rows100k.painted));
  std::printf("     1k  rows: median %.3f ms/frame (p95 %.3f), max rows drawn %u\n",
              rows1k.medianMs, rows1k.p95Ms, rows1k.maxRows);
  if (!BENCH_CHECK(rows100k.painted) || !BENCH_CHECK(rows1k.painted)) {
    std::printf("    FAIL: the list frame did not paint the backdrop\n");
  }
  // Virtualization (deterministic): rows per frame bounded by the viewport, and
  // the same bound at 1k and 100k -- that is what makes N irrelevant.
  constexpr uint32_t kRowBound = 40;
  if (!BENCH_CHECK(rows100k.maxRows <= kRowBound) || !BENCH_CHECK(rows1k.maxRows <= kRowBound)) {
    std::printf("    FAIL: rows drawn per frame %u/%u exceed the viewport bound %u\n",
                rows100k.maxRows, rows1k.maxRows, kRowBound);
  }
  BENCH_CHECK_EQ(rows1k.maxRows, rows100k.maxRows);
  // A frame that takes > 33 ms cannot hold 30 FPS even before presentation.
  if (!BENCH_CHECK(rows100k.medianMs < 33.0)) {
    std::printf("    FAIL: list frame cost %.3f ms/frame is below 30 FPS of CPU time alone\n",
                rows100k.medianMs);
  }

  if (g_failures > 0) {
    std::printf("    (%d check(s) failed before the digits were printed)\n", g_failures);
    return 1;
  }

  std::printf(
      "BENCH_SCALE {\"entries\":%zu,\"open_10k_ms\":%.3f,\"catalog_10k_ms\":%.3f,"
      "\"big_gb\":%u,\"open_big_ms\":%.1f,\"sweep_big_ms\":%.1f,\"extract_big_ms\":%.1f,"
      "\"temp_entries\":%zu,\"list_1k_frame_ms\":%.3f,\"list_100k_frame_ms\":%.3f,"
      "\"list_100k_p95_ms\":%.3f,\"list_100k_fps\":%.1f,\"rows_drawn\":%u}\n",
      entries, open10k, catalog10k, bigGb, bigOpenMs, sweepMs, extractMs, tempAfter.size(),
      rows1k.medianMs, rows100k.medianMs, rows100k.p95Ms,
      rows100k.medianMs > 0.0 ? 1000.0 / rows100k.medianMs : 0.0, rows100k.maxRows);
  std::printf("ALL SCALE BENCH CHECKS PASSED\n");
  return 0;
}
