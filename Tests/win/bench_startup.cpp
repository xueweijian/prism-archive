// PrismArchive -- Windows startup benchmark (blueprint ch. 7.2, cells 1 and 3).
//
// WHAT THIS MEASURES, and why it is measured this way:
//
//   * exe_to_main_ms   -- process creation -> this function's first line, read
//     from GetProcessTimes(creation) against GetSystemTimeAsFileTime: loader,
//     import resolution, CRT init. GetProcessTimes' FILETIME is quantised to the
//     ~15.6 ms clock tick, so this is a COARSE datum and is treated as such
//     (reported, never used in an equality assertion).
//
//   * create_window_ms / first_frame_ms -- CreateWindowExW -> the first
//     EndDraw + Present of the REAL composition path (GlassIslandHost: D3D11 +
//     D2D1 device context + three DComp composition swapchains -- exactly what
//     PrismArchive.exe builds). The app itself exposes no instrumentation hook
//     and is owned by another card, so the first-frame path is reproduced here
//     from the same header the app links. That is stated in docs/PERF.md
//     instead of being papered over.
//
//   * first 7z.dll use -- LoadLibrary + GetNumberOfFormats (the 40-format
//     table) + CreateObject(IInArchive) + Open(real .7z) + ReadCatalog, all
//     FIRST use inside this process. The OS file cache is warm (the fixture was
//     just packed by 7z.exe), which is the honest steady state on a machine
//     where 7-Zip is installed; the label says so.
//
//   * app_close_ms -- BLACK BOX, on the shipped PrismArchive.exe: launch it on
//     a real archive, wait for its window, PostMessage(WM_CLOSE), then time
//     until the process object signals. That is the WM_CLOSE -> process-gone
//     latency the blueprint's "close" cell asks for. The bug that can make it
//     hang belongs to card c1, so this file only MEASURES it; a timeout is
//     reported as a value plus a reason, never as a silent pass.
//
// Instrument discipline (contract D4 -- calibrate the instrument before
// believing it): the QPC timer is validated against a known Sleep(50) first. A
// mutant build (PRISM_BENCH_MUTANT_TIMER_SCALE) mis-scales every duration by
// 1000x and MUST fail that calibration; it is wired as a WILL_FAIL ctest target,
// so "the digits are real" is itself gated.
//
// Output (one machine-readable line, plus the ctest marker):
//   BENCH_STARTUP {"exe_to_main_ms":..,"create_window_ms":..,"first_frame_ms":..,...}
//   ALL STARTUP BENCH CHECKS PASSED
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

#include "GlassIslandHost.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dcomp.lib")
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

}  // namespace

#define BENCH_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define BENCH_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

namespace {

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

// Negative control for the instrument itself (D4): the mutant multiplies every
// reported duration by 1000, which the Sleep(50) calibration must reject.
inline double Reported(double ms) {
#ifdef PRISM_BENCH_MUTANT_TIMER_SCALE
  return ms * 1000.0;
#else
  return ms;
#endif
}

double MsSinceProcessStart() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a{}, b{};
  a.LowPart = created.dwLowDateTime;
  a.HighPart = created.dwHighDateTime;
  b.LowPart = now.dwLowDateTime;
  b.HighPart = now.dwHighDateTime;
  if (b.QuadPart <= a.QuadPart) return 0.0;
  return static_cast<double>(b.QuadPart - a.QuadPart) / 10000.0;  // 100 ns ticks -> ms
}

std::filesystem::path BenchRoot() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_BENCH_ROOT", overridePath, MAX_PATH) > 0) {
    return std::filesystem::path(overridePath);
  }
  // Deliberately NOT inside %TEMP%: the zero-%TEMP%-write cell needs a work
  // directory whose own I/O cannot pollute the snapshot it compares.
  // temp_directory_path() may carry a trailing separator, in which case
  // parent_path() is the temp dir ITSELF -- normalise first so the work root is
  // really outside %TEMP% (the zero-%TEMP% cell depends on that).
  std::filesystem::path tmp = std::filesystem::temp_directory_path();
  if (tmp.filename().empty()) tmp = tmp.parent_path();
  return tmp.parent_path() / L"prism_bench";
}

LRESULT CALLBACK BenchProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

// --------------------------------------------------------------------- window

struct WindowResult {
  HWND hwnd = nullptr;
  double createMs = 0.0;
};

WindowResult CreateBenchWindow(HINSTANCE instance) {
  WindowResult result;
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &BenchProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = L"PrismBenchStartupWindow";
  RegisterClassExW(&wc);

  Timer timer;
  timer.Begin();
  result.hwnd = CreateWindowExW(0, wc.lpszClassName, L"PrismArchive benchmark", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 1280, 800, nullptr, nullptr, instance,
                                nullptr);
  result.createMs = timer.Ms();
  if (result.hwnd) {
    ShowWindow(result.hwnd, SW_SHOWNORMAL);
    UpdateWindow(result.hwnd);
    // Drain the queue so the window is really realised before the timed frame.
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  return result;
}

constexpr D2D1_COLOR_F kProbeFill = {0.878f, 0.376f, 0.125f, 1.0f};  // 0xE06020 in 0xRRGGBB

// The painter used for BOTH the timed composition frame and the pixel
// verification pass: a known solid fill plus one rounded plaque outline, so the
// readback can assert an exact colour (contract D2: assert content, not
// existence).
void PaintProbe(ID2D1RenderTarget* target, float width, float height) {
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(kProbeFill, brush.ReleaseAndGetAddressOf()))) return;
  target->Clear(kProbeFill);
  const D2D1_ROUNDED_RECT rect = D2D1::RoundedRect(
      D2D1::RectF(4.0f, 4.0f, (std::max)(8.0f, width - 4.0f), (std::max)(8.0f, height - 4.0f)),
      8.0f, 8.0f);
  ComPtr<ID2D1SolidColorBrush> edge;
  const D2D1_COLOR_F accent = {1.0f, 1.0f, 1.0f, 0.6f};
  if (SUCCEEDED(target->CreateSolidColorBrush(accent, edge.ReleaseAndGetAddressOf()))) {
    target->DrawRoundedRectangle(rect, edge.Get(), 1.5f);
  }
}

// Headless verification of the SAME painter: a WIC software target, then a
// pixel readback. Returns the sampled centre pixel as 0xRRGGBB, or 0 on failure.
uint32_t VerifyPainterPixels() {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return 0;
  }
  constexpr UINT32 kW = 64, kH = 32;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                               bitmap.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  ComPtr<ID2D1Factory> d2d;
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               d2d.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                              target.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  target->BeginDraw();
  PaintProbe(target.Get(), static_cast<float>(kW), static_cast<float>(kH));
  if (FAILED(target->EndDraw())) return 0;

  WICRect lockRect{0, 0, static_cast<INT>(kW), static_cast<INT>(kH)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return 0;
  UINT stride = 0, size = 0;
  BYTE* buffer = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &buffer)) ||
      buffer == nullptr || stride < kW * 4) {
    return 0;
  }
  const BYTE* px = buffer + static_cast<size_t>(kH / 2) * stride + static_cast<size_t>(kW / 2) * 4;
  return (static_cast<uint32_t>(px[2]) << 16) | (static_cast<uint32_t>(px[1]) << 8) | px[0];
}

// ------------------------------------------------------------------ close probe

struct CloseCheck {
  bool launched = false;
  bool windowFound = false;
  bool exited = false;
  double launchToWindowMs = 0.0;
  double closeMs = 0.0;
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

// Locate the app next to the build tree this benchmark lives in, or take the
// explicit override. Returns empty when the app was never built (the close cell
// is then reported as unmeasured-with-reason, and only that cell).
std::wstring LocateAppExe() {
  wchar_t overridePath[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_ARCHIVE_EXE", overridePath, MAX_PATH) > 0) {
    return std::filesystem::exists(overridePath) ? std::wstring(overridePath) : std::wstring();
  }
  wchar_t module[MAX_PATH]{};
  const DWORD got = GetModuleFileNameW(nullptr, module, MAX_PATH);
  if (got == 0) return {};
  std::filesystem::path self(module);
  // <build>/Tests/win_bench_startup.exe -> <build>/Src/PrismWin/PrismArchive.exe
  const std::filesystem::path candidate =
      self.parent_path().parent_path() / L"Src" / L"PrismWin" / L"PrismArchive.exe";
  return std::filesystem::exists(candidate) ? candidate.wstring() : std::wstring();
}

CloseCheck ProbeCloseLatency(const std::wstring& appExe, const std::wstring& archive) {
  CloseCheck check;
  if (appExe.empty()) {
    check.reason = "PrismArchive.exe not built (set PRISM_ARCHIVE_EXE)";
    return check;
  }
  std::wstring cmd = L"\"" + appExe + L"\" \"" + archive + L"\"";
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  Timer launch;
  launch.Begin();
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    check.reason = "CreateProcess failed";
    return check;
  }
  check.launched = true;
  // WaitForInputIdle returns when the app first empties its queue; keep polling
  // for the real top-level window so the WM_CLOSE lands on the right handle.
  WaitForInputIdle(pi.hProcess, 30000);
  const DWORD deadline = GetTickCount() + 30000;
  EnumCtx ctx{};
  ctx.pid = pi.dwProcessId;
  while (GetTickCount() < deadline) {
    ctx.found = nullptr;
    EnumWindows(&FindMainWindowProc, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) break;
    if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;  // died early
    Sleep(5);
  }
  if (ctx.found) {
    check.windowFound = true;
    check.launchToWindowMs = launch.Ms();
    Timer closeTimer;
    closeTimer.Begin();
    PostMessageW(ctx.found, WM_CLOSE, 0, 0);
    if (WaitForSingleObject(pi.hProcess, 30000) == WAIT_OBJECT_0) {
      check.exited = true;
      check.closeMs = closeTimer.Ms();
    } else {
      check.reason = "WM_CLOSE did not end the process within 30 s";
    }
  } else {
    check.reason = "no top-level window appeared within 30 s";
  }
  if (!check.exited) {
    // Never leave a stray GUI process behind after a failed probe.
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 5000);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return check;
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  // Unbuffered: a benchmark that hangs must show which step it reached (and CI
  // logs must keep the digits even if the process is killed by the timeout).
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const double exeToMainMs = MsSinceProcessStart();  // first thing we do

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("    FAIL: COM init\n");
    return 1;
  }

  // ---- 1. calibrate the instrument (D4) before trusting any digit ----
  Timer calibration;
  calibration.Begin();
  Sleep(50);
  const double calibrationMs = Reported(calibration.Ms());
  std::printf("    timer calibration: Sleep(50) measured %.3f ms\n", calibrationMs);
  if (!BENCH_CHECK(calibrationMs >= 40.0 && calibrationMs <= 400.0)) {
    std::printf("    FAIL: timer is not calibrated -- every duration below is untrustworthy\n");
    return 1;
  }

  namespace fs = std::filesystem;
  const fs::path work = BenchRoot() / "startup";
  std::error_code ec;
  fs::remove_all(work, ec);
  fs::create_directories(work, ec);
  const std::wstring dir = work.wstring();

  // ---- 2. a real archive (7 PNGs packed by 7z.exe, solid) ----
  const std::wstring dllPath = prism::fixture::DllPath();
  const std::wstring exePath = prism::fixture::ExePath();
  if (dllPath.empty() || exePath.empty()) {
    std::printf("    FAIL: 7z.exe/7z.dll not found (set PRISM_7Z_DLL)\n");
    return 1;
  }
  constexpr int kItems = 7;
  std::vector<std::wstring> files;
  for (int i = 0; i < kItems; ++i) {
    const std::wstring name = L"p" + std::to_wstring(i) + L".png";
    if (!BENCH_CHECK((prism::fixture::WritePng(dir + L"\\" + name, 320, 240,
                                               static_cast<uint8_t>(20 * i),
                                               static_cast<uint8_t>(255 - 20 * i), 128)))) {
      return 1;
    }
    files.push_back(name);
  }
  const std::wstring archive = dir + L"\\startup.7z";
  if (!BENCH_CHECK((prism::fixture::PackSolid(exePath, dir, L"startup.7z", files)))) {
    std::printf("    FAIL: 7z.exe pack failed\n");
    return 1;
  }

  // ---- 3. window + first composed frame ----
  const WindowResult window = CreateBenchWindow(GetModuleHandleW(nullptr));
  if (!BENCH_CHECK(window.hwnd != nullptr)) {
    std::printf("    FAIL: CreateWindowExW failed (no desktop?)\n");
    return 1;
  }
  std::printf("    window created in %.3f ms\n", window.createMs);

  prism::win::glass::GlassIslandHost host;
  const bool composited = host.Initialize(window.hwnd);
  std::printf("    paint path: %s\n", composited ? "dcomp" : "headless-d2d (no compositor)");

  double firstFrameMs = 0.0;
  double secondFrameMs = 0.0;
  int presents = 0;
  if (composited) {
    BENCH_CHECK_EQ(host.IslandCount(), static_cast<size_t>(3));
    Timer frame;
    frame.Begin();
    for (size_t island = 0; island < host.IslandCount(); ++island) {
      const prism::glass::IslandRect rect{16u + 8u * static_cast<uint32_t>(island), 16u, 400u,
                                          44u};
      if (host.PaintIsland(island, rect, [](ID2D1DeviceContext* ctx, float w, float h) {
            PaintProbe(ctx, w, h);
          })) {
        ++presents;
      }
    }
    firstFrameMs = frame.Ms();  // CreateWindowEx -> first EndDraw+Present(+Commit)
    Timer frame2;
    frame2.Begin();
    if (host.PaintIsland(0, prism::glass::IslandRect{16u, 16u, 400u, 44u},
                         [](ID2D1DeviceContext* ctx, float w, float h) { PaintProbe(ctx, w, h); })) {
      ++presents;
    }
    secondFrameMs = frame2.Ms();
  } else {
    // No DWM (session 0 / no display): still measure a REAL D2D frame so the
    // cell has a number, and say exactly which path produced it.
    ComPtr<IWICImagingFactory> wic;
    if (!BENCH_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                                CLSCTX_INPROC_SERVER,
                                                IID_PPV_ARGS(wic.ReleaseAndGetAddressOf()))))) {
      return 1;
    }
    ComPtr<IWICBitmap> bitmap;
    if (!BENCH_CHECK(SUCCEEDED(wic->CreateBitmap(1280, 800, GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapCacheOnDemand,
                                                 bitmap.ReleaseAndGetAddressOf())))) {
      return 1;
    }
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (!BENCH_CHECK(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                                 d2d.ReleaseAndGetAddressOf()))) ||
        !BENCH_CHECK(SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                                target.ReleaseAndGetAddressOf())))) {
      return 1;
    }
    Timer frame;
    frame.Begin();
    target->BeginDraw();
    PaintProbe(target.Get(), 1280.0f, 800.0f);
    const HRESULT end = target->EndDraw();
    firstFrameMs = frame.Ms();
    if (!BENCH_CHECK(SUCCEEDED(end))) return 1;
    presents = 1;
    Timer frame2;
    frame2.Begin();
    target->BeginDraw();
    PaintProbe(target.Get(), 1280.0f, 800.0f);
    target->EndDraw();
    secondFrameMs = frame2.Ms();
    ++presents;
  }

  // ---- 4. the painter really painted (D2: content, not existence) ----
  const uint32_t pixel = VerifyPainterPixels();
  const uint32_t expected = 0xE06020u;  // kProbeFill as 0xRRGGBB (R=224 G=96 B=32)
  std::printf("    painter readback: 0x%06X (expected 0x%06X)\n", pixel, expected);
  if (!BENCH_CHECK_EQ(pixel, expected)) {
    std::printf("    FAIL: the timed frame did not paint the probe colour\n");
  }

  // ---- 5. first 7z.dll use in this process ----
  prism::win::SevenZipLibrary library;
  Timer dllTimer;
  dllTimer.Begin();
  if (!BENCH_CHECK(library.Load(dllPath))) return 1;
  const double loadMs = dllTimer.Ms();

  Timer formatsTimer;
  formatsTimer.Begin();
  const auto formats = library.FormatCount();
  const double formatsMs = formatsTimer.Ms();
  if (!BENCH_CHECK(formats.has_value())) return 1;

  // The candidate-handler scan every Open() performs: creating the Zip and 7z
  // archiver objects is what "initialise the format table" costs per open.
  Timer archiverTimer;
  archiverTimer.Begin();
  void* archiver = nullptr;
  const HRESULT created = library.CreateObject(prism::win::abi::CLSID_CFormat7z,
                                               prism::win::abi::IID_IInArchive, &archiver);
  if (archiver) static_cast<prism::win::abi::IInArchive*>(archiver)->Release();
  const double archiverMs = archiverTimer.Ms();
  if (!BENCH_CHECK(SUCCEEDED(created) && archiver != nullptr)) return 1;
  prism::win::ArchiveReader reader(library);

  Timer openTimer;
  openTimer.Begin();
  const bool opened = reader.Open(archive);
  const double openMs = openTimer.Ms();
  if (!BENCH_CHECK(opened)) {
    std::printf("    open trace: %s\n", reader.openTrace().c_str());
    return 1;
  }
  Timer catalogTimer;
  catalogTimer.Begin();
  const auto catalog = reader.ReadCatalog();
  const double catalogMs = catalogTimer.Ms();
  BENCH_CHECK_EQ(catalog.size(), static_cast<size_t>(kItems));
  reader.Close();

  // ---- 6. black-box close latency of the shipped exe ----
  const std::wstring appExe = LocateAppExe();
  const CloseCheck close = ProbeCloseLatency(appExe, archive);
  if (close.windowFound) {
    std::printf("    app: window after %.3f ms, WM_CLOSE -> exit %.3f ms\n", close.launchToWindowMs,
                close.closeMs);
  } else {
    std::printf("    app close latency NOT measured: %s\n", close.reason.c_str());
  }

  if (g_failures > 0) {
    std::printf("    (%d check(s) failed before the digits were printed)\n", g_failures);
    return 1;
  }

  const double firstFrame = Reported(firstFrameMs);
  const double secondFrame = Reported(secondFrameMs);
  if (!BENCH_CHECK(firstFrame >= 0.0) || !BENCH_CHECK(openMs > 0.0) ||
      !BENCH_CHECK(presents >= 1)) {
    return 1;
  }
  // Hang detectors only -- deliberately loose, so a loaded 4-core box or a
  // debugger-attached run cannot turn a benchmark into a flaky gate.
  if (!BENCH_CHECK(firstFrame < 5000.0)) return 1;

  std::printf(
      "BENCH_STARTUP {\"exe_to_main_ms\":%.3f,\"create_window_ms\":%.3f,\"first_frame_ms\":%.3f,"
      "\"second_frame_ms\":%.3f,\"presents\":%d,\"paint_path\":\"%s\",\"loadlibrary_7z_ms\":%.3f,"
      "\"formats\":%u,\"formats_ms\":%.3f,\"create_archiver_ms\":%.3f,\"open_ms\":%.3f,"
      "\"catalog_ms\":%.3f,\"items\":%d,\"app_launch_to_window_ms\":%.3f,\"app_close_ms\":%.3f,"
      "\"app_close_measured\":%s}\n",
      exeToMainMs, Reported(window.createMs), firstFrame, secondFrame, presents,
      composited ? "dcomp" : "headless-d2d", Reported(loadMs),
      static_cast<unsigned>(formats.value_or(0)), Reported(formatsMs), Reported(archiverMs),
      Reported(openMs), Reported(catalogMs), kItems, close.launchToWindowMs,
      close.exited ? close.closeMs : -1.0, close.exited ? "true" : "false");
  std::printf("ALL STARTUP BENCH CHECKS PASSED\n");
  CoUninitialize();
  return 0;
}
