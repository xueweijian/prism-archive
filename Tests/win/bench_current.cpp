// PrismArchive -- card C49: RE-MEASURE the assembly, not the parts.
//
// docs/PERF.md's headline digits were taken BEFORE the assembly existed: first
// frame 22.7 ms, resident 58.4 MB, 100k-row p95 15.6 ms, 4 GiB open 78.5 s.
// Since then V1 (one layout authority), V2 (the action island), C29 (sidebar
// rail), C45 (thumbnail pipeline), C43 (format sniffing) and C28 (routing) all
// landed, and every one of them changes what a user pays. This benchmark
// measures the ASSEMBLED application on one box, in one run, with the same
// sampling discipline as the earlier cards:
//
//   * every cell is sampled at least five times and reported as median + p95;
//     a p95 over five samples is the worst of the five, and this file says so
//     instead of pretending otherwise;
//   * the 4 GiB cell is a BYTE COUNT (prism::win::IoCounters), never a
//     stopwatch: "how much of a 4 GiB archive does Open() read" is reproducible
//     on a shared runner, "how many seconds" is not;
//   * the 800-thumbnail cell is the C45 pipeline re-run (serial vs parallel),
//     so the earlier number is CONFIRMED rather than restated;
//   * nothing here gates wall-clock. The assertions are shape checks (bytes,
//     counts, pixel identity, rows drawn) plus hang detectors.
//
// BENCH lines (C42 report lane: one marker family, one well-formed JSON object
// per line, ONE line per marker -- the extractor treats a second line with the
// same (test, marker) and a different payload as an ambiguity error):
//
//   BENCH_C49_STARTUP {"first_frame_median_ms":..,"first_frame_p95_ms":..,...}
//   BENCH_C49_MEMORY  {"empty_mb":..,"archive_mb":..,"glass_full_mb":..,...}
//   BENCH_C49_OPEN4G  {"zip_bytes":..,"sevenz_bytes":..,...}
//   BENCH_C49_FRAME   {"rows":100000,"median_ms":..,"p95_ms":..,...}
//   BENCH_C49_THUMBS  {"items":800,"serial_ms":..,"parallel_ms":..,...}
//
// Memory is measured by SAMPLING A FRESH PROCESS, never the bench process:
// every earlier arm in this binary (fixtures, 4 GiB streams, 800 thumbnails)
// would otherwise be inside the same working set, and a tier compared against
// ADR-006's 96/40 MB has to be a number about the STATE, not about the harness.
// The child mode ("--memory-stage empty|archive|glass") builds the real
// GalleryWindow (real D3D11/D2D/DWrite/glass stack), settles, and the parent
// samples its working set from outside -- the same black-box method the shipped
// PrismArchive.exe states use.
//
// CONTRACTS D27: the negative control arms prism::test::SetMutationVerdict with
// its complete prefix and a runtime predicate (the injection counter), so the
// verdict is printed at the FIRST failed assertion and the process leaves with
// 0 before any teardown can swallow it. D3: the predicate is what keeps a red
// from an unrelated cause from claiming CAUGHT.
// /WX-: the Windows SDK headers are not /W4 clean (same rationale as every
// other Windows target in this repo).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "GalleryWindow.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "ThumbnailPipeline.hpp"
#include "../mini_test.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;

// ---------------------------------------------------------------------------
// instrument (D4: calibrate it before believing any digit)
// ---------------------------------------------------------------------------
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

double Median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t n = values.size();
  if (n % 2 == 1) return values[n / 2];
  return 0.5 * (values[n / 2 - 1] + values[n / 2]);
}

// Nearest-rank percentile. At n=5 this IS the maximum, and the report says so.
double P95(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  size_t index = static_cast<size_t>(0.95 * static_cast<double>(values.size()));
  if (index >= values.size()) index = values.size() - 1;
  return values[index];
}

double Mb(size_t bytes) { return static_cast<double>(bytes) / (1024.0 * 1024.0); }

size_t CurrentWorkingSet(HANDLE process) {
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!GetProcessMemoryInfo(process, &pmc, sizeof(pmc))) return 0;
  return pmc.WorkingSetSize;
}

// Samples another process's working set: 20 reads 20 ms apart, after the caller
// has let the state settle. Median and p95 come from those reads, so a single
// spike cannot become the reported tier.
struct WsSample {
  double medianMb = 0.0;
  double p95Mb = 0.0;
  double peakMb = 0.0;
  bool measured = false;
};

WsSample SampleWorkingSet(HANDLE process, int reads = 20, int intervalMs = 20) {
  WsSample sample;
  std::vector<double> values;
  size_t peak = 0;
  for (int i = 0; i < reads; ++i) {
    const size_t now = CurrentWorkingSet(process);
    if (now > 0) {
      values.push_back(Mb(now));
      if (now > peak) peak = now;
    }
    Sleep(static_cast<DWORD>(intervalMs));
  }
  sample.measured = !values.empty();
  sample.medianMb = Median(values);
  sample.p95Mb = P95(values);
  sample.peakMb = Mb(peak);
  return sample;
}

fs::path WorkRoot() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_C49_ROOT", overridePath, MAX_PATH) > 0) {
    return fs::path(overridePath);
  }
  // Unique per process (D9 discipline: one fixture directory per run, never a
  // shared constant two parallel ctest jobs could delete under each other).
  return fs::temp_directory_path() /
         (L"prism_c49_bench_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
          std::to_wstring(GetTickCount64()));
}

std::wstring SelfExe() {
  wchar_t module[MAX_PATH]{};
  const DWORD got = GetModuleFileNameW(nullptr, module, MAX_PATH);
  return got == 0 ? std::wstring() : std::wstring(module, got);
}

// The shipped app, next to the build tree this benchmark lives in.
std::wstring LocateAppExe() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_ARCHIVE_EXE", overridePath, MAX_PATH) > 0) {
    return fs::exists(overridePath) ? std::wstring(overridePath) : std::wstring();
  }
  const std::wstring self = SelfExe();
  if (self.empty()) return {};
  // <build>/Tests/<bench>.exe -> <build>/Src/PrismWin/PrismArchive.exe
  const fs::path candidate =
      fs::path(self).parent_path().parent_path() / L"Src" / L"PrismWin" / L"PrismArchive.exe";
  return fs::exists(candidate) ? candidate.wstring() : std::wstring();
}

// ---------------------------------------------------------------------------
// fixture helpers
// ---------------------------------------------------------------------------
bool WriteStatus(const std::string& path, const std::string& text) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written,
                            nullptr) != 0;
  CloseHandle(file);
  return ok;
}

std::string ReadStatus(const std::string& path) {
  HANDLE file = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  char buffer[256] = {};
  DWORD read = 0;
  const bool ok = ReadFile(file, buffer, sizeof(buffer) - 1, &read, nullptr) != 0;
  CloseHandle(file);
  return ok ? std::string(buffer, read) : std::string();
}

// 1000 archive entries: enough rows that the V1 layout, the C29 rail and the
// C13 list view all have real work, cheap enough to author in milliseconds.
std::vector<prism::ArchiveItem> MakeItems(size_t count) {
  std::vector<prism::ArchiveItem> items;
  items.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = "photo_" + std::to_string(i) + ".png";
    item.isDir = false;
    item.size = 65536ull + i;
    items.push_back(std::move(item));
  }
  return items;
}

// ---------------------------------------------------------------------------
// section 1: cold start -> first frame, on the ASSEMBLED window
// ---------------------------------------------------------------------------
struct FirstFrameArm {
  double totalMs = 0.0;
  double createMs = 0.0;
  bool created = false;
  bool glassReady = false;
  bool painted = false;
  double paintMs = 0.0;   // ShowWindow -> the first WM_PAINT returned
  double glassMs = 0.0;   // EnableGlassLayer (composition host + 2 HLSL compiles)
};

// One sample: the real GalleryWindow with the V1 layout, the C29 rail, the V2
// action island and (when composition is available) the C33 glass island, from
// an empty scope to the first WM_PAINT that ran BeginDraw/Draw/EndDraw on the
// window's D2D target.
FirstFrameArm MeasureFirstFrame(std::vector<prism::ArchiveItem>& items) {
  FirstFrameArm arm;
  Timer total;
  total.Begin();
  {
    GalleryWindow window;
    Timer create;
    create.Begin();
    const bool created =
        window.Create(L"PrismArchive -- C49 first frame", GetModuleHandleW(nullptr));
    arm.createMs = create.Ms();
    if (!created) return arm;
    arm.created = true;
    window.SetItems(&items);
    window.SetRootLabel(L"remeasure.7z");
    window.SetStatus(L"1000 items");
    window.SetSidebarRailVisible(true);  // V1 + C29: the rail reserves width
    window.SetListMode(true);            // the list is the denser paint path
    Timer glass;
    glass.Begin();
    arm.glassReady = window.EnableGlassLayer();  // C33/C34 island, like Main.cpp
    arm.glassMs = glass.Ms();
    // The paint half is timed on its own: the earlier card's 22.7 ms was a
    // three-island PAINT, not a window creation, and comparing the two without
    // this split would be an apples-to-oranges number.
    Timer paint;
    paint.Begin();
    ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
    InvalidateRect(window.Hwnd(), nullptr, FALSE);
    // UpdateWindow sends WM_PAINT synchronously: the digit ends when the first
    // frame has really been drawn (and the glass frame presented).
    UpdateWindow(window.Hwnd());
    arm.paintMs = paint.Ms();
    arm.painted = true;
    arm.totalMs = total.Ms();
    // Destroying the window is NOT part of the first-frame cell; the destructor
    // runs at the end of this scope, after the timestamp.
  }
  return arm;
}

// ---------------------------------------------------------------------------
// section 2: resident memory, one FRESH process per state
// ---------------------------------------------------------------------------
struct StageResult {
  WsSample ws;
  bool glassReady = false;
  bool measured = false;
  std::string status;
  std::string reason;
};

// Child mode: build exactly one state, settle, publish "glass=<0|1>", wait for
// the parent to sample. The parent never guesses what the child built.
int RunMemoryStage(const char* stage, const char* statusPath) {
  struct ComApartment {
    HRESULT hr = E_FAIL;
    ComApartment() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComApartment() {
      if (SUCCEEDED(hr)) CoUninitialize();
    }
  } apartment;
  if (FAILED(apartment.hr)) {
    WriteStatus(statusPath, "create=0 glass=0 error=com");
    return 1;
  }
  std::vector<prism::ArchiveItem> items = MakeItems(1000);
  GalleryWindow window;
  if (!window.Create(L"PrismArchive -- C49 memory stage", GetModuleHandleW(nullptr))) {
    WriteStatus(statusPath, "create=0 glass=0 error=create");
    return 1;
  }
  const bool withArchive = std::strcmp(stage, "empty") != 0;
  if (withArchive) {
    window.SetItems(&items);
    window.SetRootLabel(L"remeasure.7z");
    window.SetStatus(L"1000 items");
    window.SetSidebarRailVisible(true);
    window.SetListMode(true);
  }
  bool glass = false;
  if (std::strcmp(stage, "glass") == 0) glass = window.EnableGlassLayer();
  ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
  InvalidateRect(window.Hwnd(), nullptr, FALSE);
  UpdateWindow(window.Hwnd());
  Sleep(800);  // settle: first paint, allocations, text/font caches
  const std::string status = "create=1 glass=" + std::string(glass ? "1" : "0");
  WriteStatus(statusPath, status);
  // Give the parent a generous window to take its 20 reads, then leave.
  Sleep(3000);
  return 0;
}

StageResult MeasureStage(const std::string& stage, const fs::path& work) {
  StageResult result;
  const std::wstring self = SelfExe();
  if (self.empty()) {
    result.reason = "cannot locate this executable";
    return result;
  }
  const fs::path statusFile = work / ("stage_" + stage + ".txt");
  std::error_code ec;
  fs::remove(statusFile, ec);
  const std::wstring wideStage(stage.begin(), stage.end());
  std::wstring cmd = L"\"" + self + L"\" --memory-stage " + wideStage + L" \"" +
                     statusFile.wstring() + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    result.reason = "CreateProcess failed for stage " + stage;
    return result;
  }
  std::string status;
  for (int i = 0; i < 200; ++i) {  // up to 20 s for the state to be built
    status = ReadStatus(statusFile.string());
    if (!status.empty()) break;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
    Sleep(100);
  }
  if (status.empty()) {
    result.reason = "stage " + stage + " never published its state";
  } else {
    result.status = status;
    result.glassReady = status.find("glass=1") != std::string::npos;
    result.ws = SampleWorkingSet(pi.hProcess);
    result.measured = result.ws.measured;
    if (!result.measured) result.reason = "GetProcessMemoryInfo(remote) returned 0";
  }
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return result;
}

// ---------------------------------------------------------------------------
// section 2b: the SHIPPED app, black box (the ADR-006 tiers)
// ---------------------------------------------------------------------------
struct AppState {
  WsSample ws;
  bool measured = false;
  bool minimized = false;
  std::string reason;
};

struct EnumCtx {
  DWORD pid = 0;
  HWND found = nullptr;
};

BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM param) {
  auto* ctx = reinterpret_cast<EnumCtx*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != ctx->pid) return TRUE;
  if (!IsWindowVisible(hwnd)) return TRUE;
  if (GetWindow(hwnd, GW_OWNER) != nullptr) return TRUE;
  ctx->found = hwnd;
  return FALSE;
}

AppState MeasureAppState(const std::wstring& appExe, const std::wstring& archive,
                         bool minimize) {
  AppState state;
  if (appExe.empty()) {
    state.reason = "PrismArchive.exe not built (set PRISM_ARCHIVE_EXE)";
    return state;
  }
  std::wstring cmd = L"\"" + appExe + L"\" \"" + archive + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    state.reason = "CreateProcess failed";
    return state;
  }
  WaitForInputIdle(pi.hProcess, 30000);
  const DWORD deadline = GetTickCount() + 30000;
  EnumCtx ctx{};
  ctx.pid = pi.dwProcessId;
  while (GetTickCount() < deadline) {
    ctx.found = nullptr;
    EnumWindows(&FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) break;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
    Sleep(10);
  }
  if (!ctx.found) {
    state.reason = "no top-level window appeared within 30 s";
  } else {
    Sleep(2500);  // catalog + thumbnail sweep + first paints settle
    if (minimize) {
      // The minimise path is the one C34 wired: WM_SIZE(SIZE_MINIMIZED) ->
      // ReleaseGlassLayer + TrimThumbnails. Sending it to the REAL window is
      // what makes this a measurement of the product, not of a model.
      ShowWindow(ctx.found, SW_MINIMIZE);
      state.minimized = true;
      Sleep(1500);  // Trim + IDXGIDevice3::Trim are lazy; let them land
    }
    state.ws = SampleWorkingSet(pi.hProcess);
    state.measured = state.ws.measured;
    if (!state.measured) state.reason = "GetProcessMemoryInfo(remote) returned 0";
  }
  TerminateProcess(pi.hProcess, 0);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return state;
}

// ---------------------------------------------------------------------------
// section 3: 4 GiB sparse zip / 7z -- BYTES READ, not seconds
// ---------------------------------------------------------------------------
constexpr uint64_t kOpenByteBudget = 2ull << 20;  // C28's gate, unchanged
constexpr uint64_t kFourGiB = 4ull << 30;

uint32_t Le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t Le64(const uint8_t* p) {
  return static_cast<uint64_t>(Le32(p)) | (static_cast<uint64_t>(Le32(p + 4)) << 32);
}

void PutLe32(uint8_t* p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

void PutLe64(uint8_t* p, uint64_t value) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
}

uint32_t Crc32(const uint8_t* data, size_t size) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
  }
  return ~crc;
}

bool ReadAllBytes(const fs::path& path, std::vector<uint8_t>* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size{};
  bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0;
  if (ok) {
    out->resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != 0 &&
         read == out->size();
  }
  CloseHandle(file);
  return ok;
}

HANDLE CreateSparseFile(const fs::path& path, uint64_t size) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
  DWORD returned = 0;
  DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
  LARGE_INTEGER end{};
  end.QuadPart = static_cast<LONGLONG>(size);
  if (!SetFilePointerEx(file, end, nullptr, FILE_BEGIN) || !SetEndOfFile(file)) {
    CloseHandle(file);
    return INVALID_HANDLE_VALUE;
  }
  return file;
}

bool WriteAt(HANDLE file, uint64_t offset, const uint8_t* data, size_t size) {
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) return false;
  DWORD written = 0;
  return WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != 0 &&
         written == size;
}

bool RunSevenZipIn(const std::wstring& exe, const std::wstring& args, const fs::path& cwd) {
  std::wstring command = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

bool PackSmall(const std::wstring& exe, const fs::path& work, const wchar_t* type,
               const wchar_t* name) {
  const fs::path payload = work / L"payload.txt";
  if (!fs::exists(payload)) {
    HANDLE file = CreateFileW(payload.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const char text[] = "prism archive payload\n";
    DWORD written = 0;
    WriteFile(file, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(file);
  }
  const fs::path target = work / name;
  std::error_code ec;
  fs::remove(target, ec);
  const std::wstring args = std::wstring(L"a -t") + type + L" \"" + target.wstring() + L"\" \"" +
                            payload.filename().wstring() + L"\"";
  return RunSevenZipIn(exe, args, work);
}

// A genuine zip grown to totalSize by a HOLE between the last local entry and
// the central directory; only the EOCD's central-directory offset moves.
bool BuildSparseZip(const fs::path& source, const fs::path& target, uint64_t totalSize,
                    std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadAllBytes(source, &bytes)) {
    *error = "cannot read the small zip";
    return false;
  }
  size_t eocd = std::string::npos;
  for (size_t i = bytes.size() - 22 + 1; i-- > 0;) {
    if (std::memcmp(bytes.data() + i, "PK\x05\x06", 4) == 0) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) {
    *error = "the small zip has no EOCD";
    return false;
  }
  const uint32_t cdSize = Le32(bytes.data() + eocd + 12);
  const uint32_t cdOffset = Le32(bytes.data() + eocd + 16);
  if (cdOffset == 0xFFFFFFFFu || static_cast<uint64_t>(cdOffset) + cdSize > bytes.size()) {
    *error = "the small zip is zip64 or not sane";
    return false;
  }
  const uint64_t newCdOffset = totalSize - 22 - cdSize;
  HANDLE file = CreateSparseFile(target, totalSize);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "cannot create the sparse file";
    return false;
  }
  bool ok = WriteAt(file, 0, bytes.data(), cdOffset) &&
            WriteAt(file, newCdOffset, bytes.data() + cdOffset, cdSize);
  if (ok) {
    std::vector<uint8_t> tail(bytes.begin() + static_cast<std::ptrdiff_t>(eocd),
                              bytes.begin() + static_cast<std::ptrdiff_t>(eocd + 22));
    PutLe32(tail.data() + 16, static_cast<uint32_t>(newCdOffset));
    ok = WriteAt(file, totalSize - 22, tail.data(), tail.size());
  }
  CloseHandle(file);
  if (!ok) *error = "cannot write the sparse zip";
  return ok;
}

// A genuine 7z grown to totalSize by moving only its END header to EOF. The
// start-header CRC at offset 8 must be recomputed, or 7z rejects the file and
// SCANS the whole archive -- the 4 GiB read this cell exists to catch.
bool BuildSparse7z(const fs::path& source, const fs::path& target, uint64_t totalSize,
                   std::string* error) {
  std::vector<uint8_t> bytes;
  if (!ReadAllBytes(source, &bytes)) {
    *error = "cannot read the small 7z";
    return false;
  }
  const uint8_t magic[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
  if (bytes.size() < 32 || std::memcmp(bytes.data(), magic, 6) != 0) {
    *error = "the small 7z has no signature header";
    return false;
  }
  const uint64_t headerOffset = Le64(bytes.data() + 12);
  const uint64_t headerSize = Le64(bytes.data() + 20);
  if (32 + headerOffset + headerSize != bytes.size()) {
    *error = "the small 7z layout is not start-header/pack/header";
    return false;
  }
  const uint64_t newHeaderOffset = totalSize - 32 - headerSize;
  HANDLE file = CreateSparseFile(target, totalSize);
  if (file == INVALID_HANDLE_VALUE) {
    *error = "cannot create the sparse file";
    return false;
  }
  bool ok = WriteAt(file, 0, bytes.data(), static_cast<size_t>(32 + headerOffset));
  if (ok) {
    std::vector<uint8_t> start(bytes.begin(), bytes.begin() + 32);
    PutLe64(start.data() + 12, newHeaderOffset);
    PutLe32(start.data() + 8, Crc32(start.data() + 12, 20));
    ok = WriteAt(file, 0, start.data(), start.size()) &&
         WriteAt(file, 32 + newHeaderOffset, bytes.data() + 32 + headerOffset,
                 static_cast<size_t>(headerSize));
  }
  CloseHandle(file);
  if (!ok) *error = "cannot write the sparse 7z";
  return ok;
}

struct OpenSample {
  uint64_t bytes = 0;
  uint64_t sniffBytes = 0;
  double ms = 0.0;
  bool opened = false;
  std::string trace;
};

OpenSample MeasureOpen(prism::win::SevenZipLibrary& library, const fs::path& file) {
  OpenSample sample;
  prism::win::ResetIoCounters();
  prism::win::ArchiveReader reader(library);
  Timer timer;
  timer.Begin();
  sample.opened = reader.Open(file.wstring());
  sample.ms = timer.Ms();
  sample.bytes = prism::win::IoCounters().bytesRead.load();
  sample.sniffBytes = prism::win::IoCounters().sniffBytes.load();
  sample.trace = reader.openTrace();
  reader.Close();
  return sample;
}

// ---------------------------------------------------------------------------
// section 4: 100 000 rows, the real GalleryRenderer, software target
// ---------------------------------------------------------------------------
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

  uint32_t DrawFrame(prism::win::gallery::GalleryRenderer::Scene& scene, uint32_t scroll,
                     double* drawMs, double* endMs) {
    scene.listScroll = scroll;
    const uint32_t viewportH =
        static_cast<uint32_t>((std::max)(0.0f, (height_ - 56.0f) - (68.0f + 36.0f)));
    const auto window = prism::list::RowWindow(scroll, viewportH,
                                               static_cast<uint32_t>(scene.listItems->size()));
    Timer timer;
    timer.Begin();
    target_->BeginDraw();
    renderer_.Draw(target_.Get(), scene);
    const double draw = timer.Ms();
    target_->EndDraw();
    const double end = timer.Ms() - draw;
    if (drawMs) *drawMs = draw;
    if (endMs) *endMs = end;
    return window.last - window.first + 1;
  }

  bool BackdropPainted() {
    WICRect rect{0, 0, static_cast<INT>(width_), static_cast<INT>(height_)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap_->Lock(&rect, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
      return false;
    }
    UINT stride = 0, size = 0;
    BYTE* buffer = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &buffer)) ||
        buffer == nullptr) {
      return false;
    }
    return buffer[0] == 0x13 && buffer[1] == 0x0E && buffer[2] == 0x0B;
  }

 private:
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  ComPtr<IWICImagingFactory> wic_;
  ComPtr<IWICBitmap> bitmap_;
  ComPtr<ID2D1RenderTarget> target_;
  prism::win::gallery::GalleryRenderer renderer_;
};

struct FrameArm {
  double drawMedian = 0.0, drawP95 = 0.0;
  double endMedian = 0.0, endP95 = 0.0;
  double totalMedian = 0.0, totalP95 = 0.0;
  uint32_t maxRows = 0;
  uint32_t frames = 0;
  bool painted = false;
  bool ok = false;
};

// One round = `frames` frames spread across the whole 100 000-row list. The
// round's own p95 is the sample; the section reports the MEDIAN of five
// rounds' p95 (a p95 that only happened once is not a p95).
FrameArm RunFrameRound(uint32_t rowCount, uint32_t frames) {
  FrameArm arm;
  std::vector<prism::list::SortItem> rows;
  rows.reserve(rowCount);
  for (uint32_t i = 0; i < rowCount; ++i) {
    prism::list::SortItem row;
    row.name = "photo_" + std::to_string(i) + ".png";
    row.size = 1048576ull + i;
    row.packedSize = 262144ull + i;
    row.kind = prism::PreviewKind::Image;
    rows.push_back(std::move(row));
  }
  ListFrameBench bench;
  if (!bench.Initialize(1200, 800)) return arm;
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.listMode = true;
  scene.listItems = &rows;
  scene.selectedRow = -1;
  std::vector<double> drawMs, endMs, totalMs;
  drawMs.reserve(frames);
  endMs.reserve(frames);
  totalMs.reserve(frames);
  const double span = static_cast<double>(rowCount) * static_cast<double>(prism::list::kRowHeight);
  for (uint32_t i = 0; i < frames; ++i) {
    const uint32_t scroll =
        static_cast<uint32_t>(span * static_cast<double>(i) / static_cast<double>(frames));
    double draw = 0.0, end = 0.0;
    const uint32_t drawn = bench.DrawFrame(scene, scroll, &draw, &end);
    drawMs.push_back(draw);
    endMs.push_back(end);
    totalMs.push_back(draw + end);
    if (drawn > arm.maxRows) arm.maxRows = drawn;
  }
  arm.drawMedian = Median(drawMs);
  arm.drawP95 = P95(drawMs);
  arm.endMedian = Median(endMs);
  arm.endP95 = P95(endMs);
  arm.totalMedian = Median(totalMs);
  arm.totalP95 = P95(totalMs);
  arm.frames = frames;
  arm.painted = bench.BackdropPainted();
  arm.ok = true;
  return arm;
}

// ---------------------------------------------------------------------------
// section 5: 800 thumbnails, serial vs parallel (the C45 pipeline, re-run)
// ---------------------------------------------------------------------------
constexpr uint32_t kThumbSide = 128;
constexpr size_t kThumbBytes = static_cast<size_t>(kThumbSide) * kThumbSide * 4;
constexpr uint32_t kThumbItems = 800;
constexpr size_t kThumbBudget = 64u * 1024u * 1024u;

prism::gallery::DecodedThumb SyntheticDecode(uint32_t item, uint64_t ordinal) {
  (void)ordinal;
  prism::gallery::DecodedThumb thumb;
  thumb.width = kThumbSide;
  thumb.height = kThumbSide;
  thumb.pixels.resize(kThumbBytes);
  uint32_t seed = item * 0x9E3779B1u + 0x1234567u;
  uint8_t* out = thumb.pixels.data();
  for (uint32_t y = 0; y < kThumbSide; ++y) {
    for (uint32_t x = 0; x < kThumbSide; ++x) {
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

using ThumbPipeline = prism::win::gallery::ThumbnailPipeline;

LRESULT CALLBACK BenchHostProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (message == ThumbPipeline::kThumbReady) {
    auto* pipeline = reinterpret_cast<ThumbPipeline*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (pipeline != nullptr) {
      pipeline->OnThumbMessage(lparam);
    } else {
      delete reinterpret_cast<ThumbPipeline::Payload*>(lparam);
    }
    return 0;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

class BenchHost {
 public:
  BenchHost() {
    ensureClass();
    hwnd_ = CreateWindowExW(0, className(), L"c49-bench-thumbs", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
  }
  ~BenchHost() {
    if (hwnd_ != nullptr) DestroyWindow(hwnd_);
  }
  BenchHost(const BenchHost&) = delete;
  BenchHost& operator=(const BenchHost&) = delete;
  bool valid() const { return hwnd_ != nullptr; }
  HWND hwnd() const { return hwnd_; }
  void attach(ThumbPipeline* pipeline) {
    if (hwnd_ != nullptr) {
      SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pipeline));
    }
  }
  void pump() {
    MSG msg{};
    while (PeekMessageW(&msg, hwnd_, ThumbPipeline::kThumbReady, ThumbPipeline::kThumbReady,
                        PM_REMOVE)) {
      DispatchMessageW(&msg);
    }
  }

 private:
  static const wchar_t* className() { return L"PrismC49ThumbBenchHost"; }
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

bool PumpUntilIdle(ThumbPipeline& pipeline, BenchHost& host, int timeoutMs = 120000) {
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

uint64_t DigestStore(ThumbPipeline& pipeline) {
  uint64_t hash = 1469598103934665603ull;
  for (uint32_t index = 0; index < kThumbItems; ++index) {
    const prism::gallery::DecodedThumb* thumb = pipeline.Store().Lookup(index);
    if (thumb == nullptr) return 0;
    for (const uint8_t byte : thumb->pixels) {
      hash ^= byte;
      hash *= 1099511628211ull;
    }
  }
  return hash;
}

struct ThumbRun {
  double ms = 0.0;
  size_t workers = 0;
  uint64_t decoded = 0;
  uint64_t applied = 0;
  uint64_t secondPassHits = 0;
  size_t residentBytes = 0;
  uint64_t digest = 0;
  bool ok = false;
};

ThumbRun RunThumbBatch(BenchHost& host, size_t workerCount) {
  ThumbRun run;
  ThumbPipeline::Config config;
  config.hwnd = host.hwnd();
  config.workerCount = workerCount;
  config.budgetBytes = kThumbBudget;
  ThumbPipeline pipeline(config, [](uint32_t index, uint64_t ordinal) {
    return SyntheticDecode(index, ordinal);
  });
  host.attach(&pipeline);
  run.workers = pipeline.WorkerCount();
  std::vector<uint32_t> all;
  all.reserve(kThumbItems);
  for (uint32_t i = 0; i < kThumbItems; ++i) all.push_back(i);
  Timer timer;
  timer.Begin();
  pipeline.RequestVisible(all);
  const bool idle = PumpUntilIdle(pipeline, host);
  run.ms = timer.Ms();
  const ThumbPipeline::Stats first = pipeline.Snapshot();
  run.decoded = first.decoded;
  run.applied = first.applied;
  run.residentBytes = first.residentBytes;
  run.digest = DigestStore(pipeline);
  run.ok = idle;
  pipeline.RequestVisible(all);
  run.ok = PumpUntilIdle(pipeline, host) && run.ok;
  const ThumbPipeline::Stats second = pipeline.Snapshot();
  run.secondPassHits = second.hits - first.hits;
  run.ok = run.ok && (second.decoded == first.decoded);
  return run;
}

}  // namespace

// ===========================================================================
// child mode: build ONE memory state and let the parent sample it
// ===========================================================================
int RunMemoryStageMain(const char* stage, const char* statusPath) {
  return RunMemoryStage(stage, statusPath);
}

int main(int argc, char** argv) {
  // Unbuffered: a benchmark that hangs must show how far it got, and CI must
  // keep the digits even if the timeout kills the process.
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  if (argc >= 4 && std::strcmp(argv[1], "--memory-stage") == 0) {
    return RunMemoryStageMain(argv[2], argv[3]);
  }

  // COM apartment guard, declared FIRST so every ComPtr dies while the
  // apartment is still alive (the 0xC0000005-after-the-digits bug from C7).
  struct ComApartment {
    HRESULT hr = E_FAIL;
    ComApartment() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComApartment() {
      if (SUCCEEDED(hr)) CoUninitialize();
    }
  } apartment;
  if (FAILED(apartment.hr)) {
    std::printf("    FAIL: COM init\n");
    return 1;
  }

  // CONTRACTS D27 + D3: armed with the caller's complete prefix and the runtime
  // predicate that proves the injection really fired. A red from an unrelated
  // cause records the failure and stays red (no false CAUGHT).
#ifdef PRISM_C49_MUTANT_NO_SNIFF
  prism::test::SetMutationVerdict("C49 MUTATION CAUGHT: no_sniff", [] {
    return prism::win::IoCounters().mutantProbeAll.load(std::memory_order_relaxed) > 0;
  });
  std::printf("=== c49 remeasure [MUTANT: sniff disabled] ===\n");
#else
  std::printf("=== c49 remeasure (assembled app, after V1/V2/C29/C45/C43/C28) ===\n");
#endif

  // ---- 0. calibrate the instrument (D4) before believing a digit ----
  {
    Timer calibration;
    calibration.Begin();
    Sleep(50);
    const double measured = calibration.Ms();
    std::printf("    timer calibration: Sleep(50) measured %.3f ms\n", measured);
    if (!PRISM_CHECK(measured >= 40.0 && measured <= 400.0)) {
      std::printf("    FAIL: timer is not calibrated -- every duration below is untrustworthy\n");
      return 1;
    }
  }

  const fs::path work = WorkRoot();
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  std::printf("    work root: %s\n", work.string().c_str());

  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    // D1: a visible skip, never a fake green.
    std::printf("SKIP: 7z.dll/7z.exe not found (set PRISM_7Z_DLL)\n");
    return 77;
  }

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dllPath))) {
    std::printf("    FAIL: cannot load 7z.dll\n");
    return 1;
  }
  if (!PRISM_CHECK(PackSmall(exePath, work, L"zip", L"small.zip")) ||
      !PRISM_CHECK(PackSmall(exePath, work, L"7z", L"small.7z"))) {
    std::printf("    FAIL: cannot pack the small routing fixtures\n");
    return 1;
  }
  const fs::path bigZip = work / L"sparse4g.zip";
  const fs::path big7z = work / L"sparse4g.7z";
  std::string sparseError;
  if (!BuildSparseZip(work / L"small.zip", bigZip, kFourGiB, &sparseError) ||
      !BuildSparse7z(work / L"small.7z", big7z, kFourGiB, &sparseError)) {
    std::printf("SKIP: %s\n", sparseError.c_str());
    return 77;
  }

#ifdef PRISM_C49_MUTANT_NO_SNIFF
  // One sample: a mutant is a probe, not a measurement, and its no-sniff path
  // reads all 4 GiB (minutes per sample by design).
  //
  // The SHARP fixture is a genuine 4 GiB 7z wearing a .tar name. A mutant that
  // only threw the sniff away would still route honest .zip/.7z names by their
  // EXTENSION, and the extension route is as cheap as the sniff -- the gate
  // stayed green with the injection firing (measured: bytes=263688,
  // mutantProbeAll=2). Tar has no index, so the extension route makes it walk
  // every 512-byte block: that is the pre-C28 cost this card re-measures.
  const fs::path lyingTar = work / L"sparse4g-lie.tar";
  if (!BuildSparse7z(work / L"small.7z", lyingTar, kFourGiB, &sparseError)) {
    std::printf("SKIP: %s\n", sparseError.c_str());
    return 77;
  }
  const fs::path mutantFiles[3] = {lyingTar, bigZip, big7z};
  const char* mutantLabels[3] = {"sparse4g-lie.tar", "sparse4g.zip", "sparse4g.7z"};
  for (int i = 0; i < 3; ++i) {
    const OpenSample sample = MeasureOpen(library, mutantFiles[i]);
    std::printf("    %s: bytes=%llu\n", mutantLabels[i],
                static_cast<unsigned long long>(sample.bytes));
    PRISM_CHECK(sample.opened);
    // THE assertion the injection must break. With the sniff thrown away the
    // extension route sends a whole-file walk at a 4 GiB container.
    PRISM_CHECK(sample.bytes <= kOpenByteBudget);
  }
  std::printf("C49 MUTATION NOT CAUGHT: the byte gate stayed green (mutantProbeAll=%llu)\n",
              static_cast<unsigned long long>(prism::win::IoCounters().mutantProbeAll.load()));
  return 1;
#else
  // -----------------------------------------------------------------------
  // 1. cold start -> first frame (the ASSEMBLED window)
  // -----------------------------------------------------------------------
  std::vector<prism::ArchiveItem> benchItems = MakeItems(1000);
  std::printf("  -- first frame (assembled GalleryWindow: V1 layout + C29 rail + glass) --\n");
  std::vector<double> firstFrameMs, firstCreateMs, firstPaintMs, firstGlassMs;
  bool firstFramePainted = false;
  bool glassReady = false;
  {
    constexpr int kSamples = 9;
    for (int i = 0; i < kSamples; ++i) {
      const FirstFrameArm arm = MeasureFirstFrame(benchItems);
      if (!PRISM_CHECK(arm.created)) break;
      firstFrameMs.push_back(arm.totalMs);
      firstCreateMs.push_back(arm.createMs);
      firstPaintMs.push_back(arm.paintMs);
      firstGlassMs.push_back(arm.glassMs);
      firstFramePainted = firstFramePainted || arm.painted;
      glassReady = glassReady || arm.glassReady;
    }
    if (firstFrameMs.empty()) {
      std::printf("    FAIL: no assembled window could be created (no window station?)\n");
      return 1;
    }
  }
  const double firstFrameMedian = Median(firstFrameMs);
  const double firstFrameP95 = P95(firstFrameMs);
  std::printf("    first frame: median %.3f ms, p95 %.3f ms over %zu samples "
              "(Create %.3f + glass %.3f + paint %.3f ms median, glass=%s, painted=%s)\n",
              firstFrameMedian, firstFrameP95, firstFrameMs.size(), Median(firstCreateMs),
              Median(firstGlassMs), Median(firstPaintMs), glassReady ? "on" : "off",
              firstFramePainted ? "yes" : "no");
  PRISM_CHECK(firstFramePainted);
  // Hang detector only: 5 s. The digit is report-only (a shared box cannot gate
  // a stopwatch -- docs/PERF.md).
  PRISM_CHECK(firstFrameP95 < 5000.0);

  // A modest real archive for the shipped-app memory states.
  const std::wstring appExe = LocateAppExe();
  const std::wstring appArchive = (work / L"app_catalog.7z").wstring();
  {
    std::vector<std::wstring> files;
    for (int i = 0; i < 64; ++i) {
      const std::wstring name = L"t" + std::to_wstring(i) + L".png";
      if (!PRISM_CHECK(prism::fixture::WritePng((work / name).wstring(), 320, 240,
                                                static_cast<uint8_t>(4 * i),
                                                static_cast<uint8_t>(255 - 4 * i), 128))) {
        return 1;
      }
      files.push_back(name);
    }
    if (!PRISM_CHECK(prism::fixture::PackSolid(exePath, work.wstring(), L"app_catalog.7z",
                                               files))) {
      std::printf("    FAIL: cannot pack the app fixture\n");
      return 1;
    }
  }

  // -----------------------------------------------------------------------
  // 2. resident memory: four states, each a FRESH process (ADR-006 tiers)
  // -----------------------------------------------------------------------
  std::printf("  -- resident memory (fresh process per state, 5 samples each) --\n");
  struct MemoryArm {
    std::vector<double> values;
    bool measured = false;
    bool glass = false;
    std::string reason;
  };
  auto stageArm = [&](const char* stage) {
    MemoryArm arm;
    for (int i = 0; i < 5; ++i) {
      const StageResult result = MeasureStage(stage, work);
      if (!result.measured) {
        arm.reason = result.reason;
        continue;
      }
      arm.values.push_back(result.ws.medianMb);
      arm.measured = true;
      arm.glass = arm.glass || result.glassReady;
    }
    return arm;
  };
  const MemoryArm emptyArm = stageArm("empty");
  const MemoryArm archiveArm = stageArm("archive");
  const MemoryArm glassArm = stageArm("glass");

  auto appArm = [&](bool minimize) {
    MemoryArm arm;
    for (int i = 0; i < 5; ++i) {
      const AppState state = MeasureAppState(appExe, appArchive, minimize);
      if (!state.measured) {
        arm.reason = state.reason;
        continue;
      }
      arm.values.push_back(state.ws.medianMb);
      arm.measured = true;
    }
    return arm;
  };
  const MemoryArm appArchiveArm = appArm(false);
  const MemoryArm appMinArm = appArm(true);

  std::printf("    empty window (no archive, no glass): %.1f MB median (p95 %.1f)\n",
              Median(emptyArm.values), P95(emptyArm.values));
  std::printf("    + 1000-entry catalog: %.1f MB median (p95 %.1f)\n",
              Median(archiveArm.values), P95(archiveArm.values));
  std::printf("    + glass island live: %.1f MB median (p95 %.1f, glass=%s)\n",
              Median(glassArm.values), P95(glassArm.values), glassArm.glass ? "on" : "off");
  std::printf("    shipped app, archive open: %.1f MB median (p95 %.1f)\n",
              Median(appArchiveArm.values), P95(appArchiveArm.values));
  std::printf("    shipped app, minimised: %.1f MB median (p95 %.1f)\n",
              Median(appMinArm.values), P95(appMinArm.values));
  PRISM_CHECK(emptyArm.measured);
  PRISM_CHECK(archiveArm.measured);
  PRISM_CHECK(glassArm.measured);
  if (!appArchiveArm.measured) {
    std::printf("    note: shipped-app tiers not measured: %s\n", appArchiveArm.reason.c_str());
  }

  // -----------------------------------------------------------------------
  // 3. 4 GiB open: BYTES READ (median + p95 over five opens)
  // -----------------------------------------------------------------------
  std::printf("  -- 4 GiB open: bytes read, not seconds --\n");
  struct OpenArm {
    std::vector<double> byteValues;
    std::vector<double> msValues;
    std::vector<double> sniffValues;
    bool opened = false;
    std::string trace;
  };
  auto openArm = [&](const fs::path& file) {
    OpenArm arm;
    for (int i = 0; i < 5; ++i) {
      const OpenSample sample = MeasureOpen(library, file);
      arm.opened = arm.opened || sample.opened;
      arm.byteValues.push_back(static_cast<double>(sample.bytes));
      arm.sniffValues.push_back(static_cast<double>(sample.sniffBytes));
      arm.msValues.push_back(sample.ms);
      if (arm.trace.empty()) arm.trace = sample.trace;
    }
    return arm;
  };
  const OpenArm zipArm = openArm(bigZip);
  const OpenArm sevenArm = openArm(big7z);
  std::printf("    sparse4g.zip: %.0f bytes median (p95 %.0f), %.1f ms median | %s\n",
              Median(zipArm.byteValues), P95(zipArm.byteValues), Median(zipArm.msValues),
              zipArm.trace.c_str());
  std::printf("    sparse4g.7z : %.0f bytes median (p95 %.0f), %.1f ms median | %s\n",
              Median(sevenArm.byteValues), P95(sevenArm.byteValues), Median(sevenArm.msValues),
              sevenArm.trace.c_str());
  PRISM_CHECK(zipArm.opened);
  PRISM_CHECK(sevenArm.opened);
  // C28's gate, unchanged: identifying + indexing a 4 GiB archive must not read
  // more than 2 MiB of it. This is the assertion the no-sniff mutant breaks.
  PRISM_CHECK(Median(zipArm.byteValues) <= static_cast<double>(kOpenByteBudget));
  PRISM_CHECK(Median(sevenArm.byteValues) <= static_cast<double>(kOpenByteBudget));

  // -----------------------------------------------------------------------
  // 4. 100 000 rows: frame time median + p95 (five rounds x 60 frames)
  // -----------------------------------------------------------------------
  std::printf("  -- 100k rows: frame time (5 rounds x 60 frames, 1200x800 software) --\n");
  std::vector<double> roundMedian, roundP95;
  double worstDrawP95 = 0.0, worstEndP95 = 0.0;
  uint32_t rowsDrawn = 0;
  bool framePainted = false;
  for (int round = 0; round < 5; ++round) {
    const FrameArm arm = RunFrameRound(100000, 60);
    if (!PRISM_CHECK(arm.ok)) break;
    roundMedian.push_back(arm.totalMedian);
    roundP95.push_back(arm.totalP95);
    worstDrawP95 = (std::max)(worstDrawP95, arm.drawP95);
    worstEndP95 = (std::max)(worstEndP95, arm.endP95);
    rowsDrawn = (std::max)(rowsDrawn, arm.maxRows);
    framePainted = framePainted || arm.painted;
    std::printf("    round %d: median %.3f ms, p95 %.3f ms (draw p95 %.3f + flush p95 %.3f), "
                "rows %u\n",
                round + 1, arm.totalMedian, arm.totalP95, arm.drawP95, arm.endP95, arm.maxRows);
  }
  const double frameMedian = Median(roundMedian);
  const double frameP95 = Median(roundP95);
  std::printf("    across rounds: median %.3f ms, p95 %.3f ms -> %.1f FPS median, %.1f FPS p95\n",
              frameMedian, frameP95, frameMedian > 0.0 ? 1000.0 / frameMedian : 0.0,
              frameP95 > 0.0 ? 1000.0 / frameP95 : 0.0);
  PRISM_CHECK(framePainted);
  // Virtualisation is the assertion (a count check, not a stopwatch): the
  // viewport, not the row count, decides how many rows are drawn.
  PRISM_CHECK(rowsDrawn > 0 && rowsDrawn < 100);
  // Hang detector only.
  PRISM_CHECK(frameP95 < 5000.0);

  // -----------------------------------------------------------------------
  // 5. 800 thumbnails: serial vs parallel (C45's pipeline, re-run)
  // -----------------------------------------------------------------------
  std::printf("  -- 800 thumbnails @128x128: serial vs parallel (5 rounds each) --\n");
  BenchHost host;
  if (!PRISM_CHECK(host.valid())) {
    std::printf("    FAIL: message-only host window\n");
    return 1;
  }
  std::vector<double> serialMs, parallelMs;
  ThumbRun serial{};
  ThumbRun parallel{};
  for (int round = 0; round < 5; ++round) {
    serial = RunThumbBatch(host, 1);
    parallel = RunThumbBatch(host, 0);
    if (!PRISM_CHECK(serial.ok) || !PRISM_CHECK(parallel.ok)) break;
    serialMs.push_back(serial.ms);
    parallelMs.push_back(parallel.ms);
  }
  const double serialMedian = Median(serialMs);
  const double parallelMedian = Median(parallelMs);
  const double speedup = parallelMedian > 0.0 ? serialMedian / parallelMedian : 0.0;
  std::printf("    serial: %.1f ms median (p95 %.1f) | parallel: %.1f ms median (p95 %.1f), "
              "speedup %.2fx, workers %zu\n",
              serialMedian, P95(serialMs), parallelMedian, P95(parallelMs), speedup,
              parallel.workers);
  PRISM_CHECK_EQ(serial.decoded, static_cast<uint64_t>(kThumbItems));
  PRISM_CHECK_EQ(parallel.decoded, static_cast<uint64_t>(kThumbItems));
  PRISM_CHECK_EQ(parallel.secondPassHits, static_cast<uint64_t>(kThumbItems));
  PRISM_CHECK(parallel.digest == serial.digest && serial.digest != 0);
  PRISM_CHECK(parallel.residentBytes <= kThumbBudget);
  PRISM_CHECK(parallel.workers >= 2);

  // -----------------------------------------------------------------------
  // the digits
  // -----------------------------------------------------------------------
  if (prism::test::FailureCount() != 0) {
    std::printf("    (%d check(s) failed before the digits were printed)\n",
                prism::test::FailureCount());
    return 1;
  }

  std::printf(
      "BENCH_C49_STARTUP {\"first_frame_median_ms\":%.3f,\"first_frame_p95_ms\":%.3f,"
      "\"samples\":%zu,\"create_median_ms\":%.3f,\"glass_median_ms\":%.3f,"
      "\"paint_median_ms\":%.3f,\"paint_p95_ms\":%.3f,\"glass\":\"%s\",\"painted\":%s}\n",
      firstFrameMedian, firstFrameP95, firstFrameMs.size(), Median(firstCreateMs),
      Median(firstGlassMs), Median(firstPaintMs), P95(firstPaintMs), glassReady ? "on" : "off",
      firstFramePainted ? "true" : "false");
  std::printf(
      "BENCH_C49_MEMORY {\"empty_mb\":%.2f,\"empty_p95_mb\":%.2f,"
      "\"archive_mb\":%.2f,\"archive_p95_mb\":%.2f,"
      "\"glass_full_mb\":%.2f,\"glass_full_p95_mb\":%.2f,\"glass_live\":%s,"
      "\"app_archive_mb\":%.2f,\"app_archive_p95_mb\":%.2f,"
      "\"app_minimized_mb\":%.2f,\"app_minimized_p95_mb\":%.2f,"
      "\"app_measured\":%s,\"samples\":5,\"adr006_glass_budget_mb\":96,"
      "\"adr006_min_budget_mb\":40}\n",
      Median(emptyArm.values), P95(emptyArm.values), Median(archiveArm.values),
      P95(archiveArm.values), Median(glassArm.values), P95(glassArm.values),
      glassArm.glass ? "true" : "false",
      appArchiveArm.measured ? Median(appArchiveArm.values) : -1.0,
      appArchiveArm.measured ? P95(appArchiveArm.values) : -1.0,
      appMinArm.measured ? Median(appMinArm.values) : -1.0,
      appMinArm.measured ? P95(appMinArm.values) : -1.0,
      appArchiveArm.measured && appMinArm.measured ? "true" : "false");
  std::printf(
      "BENCH_C49_OPEN4G {\"zip_bytes\":%.0f,\"zip_p95_bytes\":%.0f,\"zip_ms\":%.1f,"
      "\"sevenz_bytes\":%.0f,\"sevenz_p95_bytes\":%.0f,\"sevenz_ms\":%.1f,"
      "\"zip_sniff_bytes\":%.0f,\"sevenz_sniff_bytes\":%.0f,"
      "\"budget_bytes\":%llu,\"file_bytes\":%llu,\"samples\":5}\n",
      Median(zipArm.byteValues), P95(zipArm.byteValues), Median(zipArm.msValues),
      Median(sevenArm.byteValues), P95(sevenArm.byteValues), Median(sevenArm.msValues),
      Median(zipArm.sniffValues), Median(sevenArm.sniffValues),
      static_cast<unsigned long long>(kOpenByteBudget),
      static_cast<unsigned long long>(kFourGiB));
  std::printf(
      "BENCH_C49_FRAME {\"rows\":100000,\"rounds\":%zu,\"frames_per_round\":60,"
      "\"median_ms\":%.3f,\"p95_ms\":%.3f,\"median_fps\":%.1f,\"p95_fps\":%.1f,"
      "\"draw_p95_ms\":%.3f,\"flush_p95_ms\":%.3f,\"rows_drawn\":%u,\"painted\":%s}\n",
      roundMedian.size(), frameMedian, frameP95,
      frameMedian > 0.0 ? 1000.0 / frameMedian : 0.0,
      frameP95 > 0.0 ? 1000.0 / frameP95 : 0.0, worstDrawP95, worstEndP95, rowsDrawn,
      framePainted ? "true" : "false");
  std::printf(
      "BENCH_C49_THUMBS {\"items\":%u,\"side\":%u,\"serial_ms\":%.3f,\"serial_p95_ms\":%.3f,"
      "\"parallel_ms\":%.3f,\"parallel_p95_ms\":%.3f,\"speedup\":%.3f,\"workers\":%zu,"
      "\"decoded\":%llu,\"second_pass_hits\":%llu,\"resident_bytes\":%zu,"
      "\"budget_bytes\":%zu,\"pixel_digest\":\"%016llx\",\"rounds\":%zu}\n",
      static_cast<unsigned>(kThumbItems), static_cast<unsigned>(kThumbSide), serialMedian,
      P95(serialMs), parallelMedian, P95(parallelMs), speedup, parallel.workers,
      static_cast<unsigned long long>(parallel.decoded),
      static_cast<unsigned long long>(parallel.secondPassHits), parallel.residentBytes,
      kThumbBudget, static_cast<unsigned long long>(parallel.digest), serialMs.size());

  fs::remove_all(work, ec);
  std::printf("ALL C49 REMEASURE CHECKS PASSED\n");
  return 0;
#endif  // PRISM_C49_MUTANT_NO_SNIFF
}
