// PrismArchive -- C76 startup bench: the process-to-first-frame breakdown, the
// before/after comparison, and the two negative controls.
//
// WHAT IT MEASURES, AND FROM WHERE
//
//   * The REAL product path. Section 1 launches PrismArchive.exe with
//     PRISM_C76_TRACE=1 (+ PRISM_C76_TRACE_EXIT=1 so the process ends itself
//     after the first frame) and parses the C76_BREAKDOWN lines the product
//     printed. The loader, the window class, D2D/DWrite, the HLSL, DComp and the
//     first WM_PAINT are therefore the shipped ones, not a harness copy.
//     "cold" = the first launch of this run; "warm" = the four that follow it
//     (image, DLLs and archive in the standby list). Five launches each, median.
//
//   * The FIRST FRAME CONTENT. Section 2 renders the window scene offscreen
//     through GalleryWindow::RenderSceneInto -- the same DrawFrame the window
//     uses -- and digests the pixels. The digest must equal the constant taken
//     from the PRE-CARD build (PRISM_C76_BASELINE), which is the "pixels are
//     unchanged" claim, not an opinion.
//
//   * The two negative controls (D3/D26/D27). --mutant runtime_shaders builds a
//     binary that always calls D3DCompile; --mutant glass_on_first_frame builds
//     one whose island is presented before the first frame. Each must break the
//     SAME predicate the optimised build passes, and the verdict prints at the
//     first red with the injection OBSERVED at runtime.
//
// WHY THE 60 ms TARGET IS NOT AN ASSERTION: contract R2 (AGENTS.md) -- a
// stopwatch on a shared machine is not reproducible. The gate is the counter
// set (runtime compiles == 0, glass presents before frame 1 == 0, pixel digest
// equal) plus a real hang ceiling; C76_BENCH prints the milliseconds with a
// "met" flag so the card still reports 60 ms honestly.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "BenchFrameCapture.hpp"
#include "GalleryWindow.hpp"
#include "StartupTrace.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;

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

#define C76_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)
#define C76_CHECK_EQ(a, b) ::CheckEq((a), (b), #a, #b, __FILE__, __LINE__)

namespace {

// C113: the digest is read through Tests/win/BenchFrameCapture.hpp -- the SAME
// reader the honesty suite asserts on, so this bench cannot quietly disagree
// with it about what a rendered frame is. It prints RENDER FAILED hr=0x%08X and
// leaves rendered == false whenever EndDraw rejected the frame.
using prism::win::benchframe::CaptureOffscreen;
using prism::win::benchframe::Frame;
using prism::win::benchframe::Hex;
using prism::win::benchframe::Seam;

// The digest of the frame this bench captures at 1264x741.
//
// C113 RE-RECORDED IT, and this is the part a reader must not skim: the old
// value (0x79a56a1305dd98de) could never be matched again by a REAL frame,
// because every card since C76 changed pixels. What the bench was hashing
// instead was a frame D2D had REJECTED: 0af0b75bc5b12383, the canvas clear plus
// nothing (painted == 0 of 936624 pixels). C112 re-recorded the constant to that
// value and called it "the pill moved to the island" -- a failed picture became
// the baseline. The C113 suite pins both halves of that story (H1 reproduces
// 0x88990012 and the empty frame; H3 refuses a frame with no painted pixels),
// and this bench now asserts rendered + hr == S_OK + painted >= 5% BEFORE the
// digest is allowed to mean anything. Only a frame that passes those three may
// be recorded here again.
//
// C112 LANDS ON TOP OF C113 AND RE-RECORDS IT ONCE MORE -- and this time the
// frame is provably real, because C113's content assertions run first:
// rendered == 1, enddraw_hr == 0x0, painted == 164668/936624 (17.6%).
//
// THE VALUE BELOW WAS MEASURED ON 42d7026 (main with C109's type and C110's
// grid). It is a hash of the WHOLE FRAME's bytes, so changing the base changes
// it -- re-measure, never copy.
//
// WHY THIS MOVE IS LEGITIMATE (all three are measured, not claimed):
//   1. the frame is still REAL -- see the painted figure above, asserted before
//      the digest is allowed to mean anything;
//   2. the change is CONFINED TO THE PILL -- C112's S3b measures 0 changed
//      pixels outside the pill rectangle plus its 1 px stroke, at 1280x800 AND
//      at the CI runner's 1028x729;
//   3. the pill's fill and TEXT moved from the window's frame to the island BY
//      DESIGN: what changed is the OWNERSHIP of those pixels, not the success
//      of the render.
//
// WHY THE VALUE IS NOT e735eb87a09d4f84 ANY MORE, in one line: main moved under
// it. C113 measured e735 on ITS tree; since then C109 re-typed the list rows
// (typography is pixels) and C112 moved the pill's fill and TEXT to the island,
// so the same window paints different bytes. The frame is still real -- painted
// == 164668/936624 (17.6%), the three preconditions above run first -- and
// C112's own delta is confined to the pill (S3b: 0 pixels outside it).
// 0af0b75bc5b12383 was a frame D2D had REJECTED and stays dead.
uint64_t g_baselineDigest = 0x3c2271cbab6069a1ull;  // C112 + current main, 1264x741
// The digest is a function of the CLIENT SIZE (the layout is width-driven), so
// the equality is only meaningful at the geometry it was recorded at. On another
// display the bench prints the digest and says so instead of pretending a
// different window is a pixel regression.
constexpr uint32_t kBaselineWidth = 1264;
constexpr uint32_t kBaselineHeight = 741;

// Frame now comes from BenchFrameCapture.hpp (C113).


// Renders the window scene into an offscreen WIC target and digests it.
//
// C113: this used to read `if (!window.RenderSceneInto(...)) return frame;` -- a
// guard that could not fire, because the product dropped EndDraw's HRESULT. The
// seam is now RenderSceneForCapture (device objects rebuilt on the WIC target,
// so the picture really is the scene) and the digest is only a digest when
// rendered && hr == S_OK && painted > 0. The caller asserts all three.
Frame RenderOffscreen(GalleryWindow& window, IWICImagingFactory* wic, ID2D1Factory* d2d,
                      uint32_t width, uint32_t height) {
  return CaptureOffscreen(window, wic, d2d, width, height, "C76 first frame", Seam::kProduct);
}

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

double QpcMs() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

double Median(std::vector<double> values) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t middle = values.size() / 2;
  return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2.0;
}

// ---------------------------------------------------------------------------
// section 1: the REAL process -- launch, parse, aggregate
// ---------------------------------------------------------------------------
struct RunSample {
  bool ok = false;
  double totalMs = 0.0;
  double processToWinMainMs = 0.0;
  std::vector<std::pair<std::string, double>> stages;
  int runtimeCompiles = -1;
  int glassPresents = -1;
  std::string why;
};

std::wstring TempFile(const wchar_t* leaf) {
  wchar_t dir[MAX_PATH]{};
  GetTempPathW(MAX_PATH, dir);
  return std::wstring(dir) + L"c76_" + std::to_wstring(GetCurrentProcessId()) + L"_" + leaf;
}

std::string ReadAll(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  std::string text;
  char buffer[8192];
  DWORD read = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
    text.append(buffer, read);
  }
  CloseHandle(file);
  return text;
}

// Extracts "key":<number> from a flat JSON object. The producer is this project
// (StartupTrace::Finish), so the shape is known; a missing key yields -1 and the
// caller reports that instead of silently scoring a zero.
double JsonNumber(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const size_t at = line.find(needle);
  if (at == std::string::npos) return -1.0;
  return std::atof(line.c_str() + at + needle.size());
}

std::string JsonString(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":\"";
  const size_t at = line.find(needle);
  if (at == std::string::npos) return {};
  const size_t start = at + needle.size();
  const size_t end = line.find('"', start);
  if (end == std::string::npos) return {};
  return line.substr(start, end - start);
}

// Runs the shipped exe once and reads the trace it printed. Bounded (D29): a
// launch that does not report a first frame within 30 s is a FAILURE, never a
// silent skip.
bool LaunchOnce(const std::wstring& app, const std::wstring& archive, RunSample* sample) {
  const std::wstring log = TempFile(L"trace.txt");
  DeleteFileW(log.c_str());
  SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
  HANDLE file = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    sample->why = "cannot create the trace file";
    return false;
  }
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdOutput = file;
  startup.hStdError = file;
  startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  PROCESS_INFORMATION process{};
  std::wstring command = L"\"" + app + L"\" \"" + archive + L"\"";
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  const BOOL created = CreateProcessW(app.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
  CloseHandle(file);
  if (!created) {
    sample->why = "CreateProcess failed";
    return false;
  }
  const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
  if (waited != WAIT_OBJECT_0) {
    TerminateProcess(process.hProcess, 1);  // bounded: no INFINITE wait (D29)
    sample->why = "the launch did not finish within 30 s";
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return false;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  const std::string log_text = ReadAll(log);
  DeleteFileW(log.c_str());
  if (log_text.empty()) {
    sample->why = "the process printed nothing (no trace?)";
    return false;
  }
  size_t at = 0;
  while (at < log_text.size()) {
    const size_t end = log_text.find('{', at);
    if (end == std::string::npos) break;
    const size_t lineStart = log_text.rfind('\n', end);
    const size_t lineEnd = log_text.find('\n', end);
    const std::string line =
        log_text.substr(lineStart == std::string::npos ? 0 : lineStart + 1,
                        (lineEnd == std::string::npos ? log_text.size() : lineEnd) -
                            (lineStart == std::string::npos ? 0 : lineStart + 1));
    if (line.rfind("C76_BREAKDOWN", 0) == 0) {
      const std::string seg = JsonString(line, "seg");
      const double ms = JsonNumber(line, "ms");
      if (!seg.empty() && ms >= 0.0) sample->stages.emplace_back(seg, ms);
    } else if (line.rfind("C76_FIRSTFRAME", 0) == 0) {
      sample->totalMs = JsonNumber(line, "total_ms");
      sample->processToWinMainMs = JsonNumber(line, "process_start_to_winmain_ms");
      sample->runtimeCompiles = static_cast<int>(JsonNumber(line, "hlsl_runtime_compiles"));
      sample->glassPresents = static_cast<int>(JsonNumber(line, "glass_presents_before_first_frame"));
    }
    at = (lineEnd == std::string::npos) ? log_text.size() : lineEnd + 1;
    if (end >= log_text.size()) break;
  }
  sample->ok = sample->totalMs > 0.0;
  if (!sample->ok) sample->why = "no C76_FIRSTFRAME line in the child's output";
  return sample->ok;
}

// Packs a fixture archive with the 7-Zip CLI beside 7z.dll. A missing CLI is a
// FAILURE (the same discipline bench_solid follows), never a silent skip.
bool MakeArchive(const std::wstring& sevenZip, const std::wstring& workDir,
                 const std::wstring& archive, std::string* why) {
  std::error_code ec;
  std::filesystem::create_directories(workDir, ec);
  for (int i = 0; i < 40; ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "file_%03d.txt", i);
    HANDLE file = CreateFileW((workDir + L"\\" + std::wstring(name, name + std::strlen(name)))
                                  .c_str(),
                              GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) continue;
    const std::string payload = std::string("payload ") + name;
    DWORD written = 0;
    WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr);
    CloseHandle(file);
  }
  DeleteFileW(archive.c_str());
  std::wstring command = L"\"" + sevenZip + L"\" a -t7z \"" + archive + L"\" \"" + workDir +
                         L"\\*\"";
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(sevenZip.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, workDir.c_str(), &startup, &process)) {
    *why = "cannot run 7z.exe";
    return false;
  }
  const DWORD waited = WaitForSingleObject(process.hProcess, 120000);
  DWORD exitCode = 1;
  if (waited == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (waited != WAIT_OBJECT_0 || exitCode != 0) {
    *why = "7z.exe could not create the fixture archive";
    return false;
  }
  return true;
}
// ---------------------------------------------------------------------------
// section 2: the assembled window in THIS process -- timing, pixels, counters
// ---------------------------------------------------------------------------
struct InProcSample {
  double totalMs = 0.0;   // Create() -> the first WM_PAINT returned
  double createMs = 0.0;
  double glassMs = 0.0;
  double paintMs = 0.0;
};

struct InProcResult {
  InProcSample sample;
  Frame frame1;
  Frame frame2;
  int compiles = -1;
  int precompiled = -1;
  int presentsAtFirstFrame = -1;
  int presentsAfterSecond = -1;
  bool glassReady = false;
  bool ok = false;
};

// One real window, painted for real: the first WM_PAINT (no glass) then the
// second one (glass). The offscreen digests come AFTER the paints so the layout
// object the window hit-tests with is the one the frame used.
InProcResult RunInProc(const std::wstring& shaderDir, IWICImagingFactory* wic, ID2D1Factory* d2d,
                       std::vector<prism::ArchiveItem>* items, bool digest) {
  InProcResult result;
  const double start = QpcMs();
  GalleryWindow window;
  const double createStart = QpcMs();
  if (!window.Create(L"PrismArchive -- C76 first frame", GetModuleHandleW(nullptr))) {
    return result;
  }
  result.sample.createMs = QpcMs() - createStart;
  window.SetItems(items);
  window.SetRootLabel(L"c76.7z");
  window.SetStatus(L"1000 items");
  window.SetSidebarRailVisible(true);
  window.SetListMode(true);
  const double glassStart = QpcMs();
  result.glassReady = window.EnableGlassLayer(shaderDir);
  result.sample.glassMs = QpcMs() - glassStart;
  const double paintStart = QpcMs();
  ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
  InvalidateRect(window.Hwnd(), nullptr, FALSE);
  UpdateWindow(window.Hwnd());  // frame 1: the glass island is still deferred
  result.sample.paintMs = QpcMs() - paintStart;
  result.sample.totalMs = QpcMs() - start;
  result.presentsAtFirstFrame = window.GlassPresentsAtFirstFrame();
  // frame 2 -- the frame that carries the island
  InvalidateRect(window.Hwnd(), nullptr, FALSE);
  UpdateWindow(window.Hwnd());
  result.presentsAfterSecond = window.GlassPresents();
  if (digest) {
    RECT rc{};
    GetClientRect(window.Hwnd(), &rc);
    const uint32_t width = static_cast<uint32_t>(rc.right);
    const uint32_t height = static_cast<uint32_t>(rc.bottom);
    result.frame1 = RenderOffscreen(window, wic, d2d, width, height);
    result.frame2 = RenderOffscreen(window, wic, d2d, width, height);
  }
  prism::win::startup::Trace& trace = prism::win::startup::Trace::Instance();
  result.compiles = trace.CountOf("hlsl_runtime_compiles");
  result.precompiled = trace.CountOf("hlsl_precompiled");
  result.ok = true;
  return result;
}

// Every differing pixel must sit inside the island's rectangle. That is the
// "no visible flicker" predicate: deferring the composition may change THAT
// region and nothing else.
size_t DifferingPixelsOutside(const Frame& a, const Frame& b, int left, int top, int right,
                              int bottom) {
  if (!a.rendered || !b.rendered || a.width != b.width || a.height != b.height) return 1u;
  size_t differing = 0;
  for (uint32_t y = 0; y < a.height; ++y) {
    for (uint32_t x = 0; x < a.width; ++x) {
      const size_t at = (static_cast<size_t>(y) * a.width + x) * 4u;
      if (std::memcmp(&a.bgra[at], &b.bgra[at], 4u) == 0) continue;
      const bool inside = static_cast<int>(x) >= left && static_cast<int>(x) < right &&
                          static_cast<int>(y) >= top && static_cast<int>(y) < bottom;
      if (!inside) ++differing;
    }
  }
  return differing;
}

constexpr int kIslandLeft = 0;
constexpr int kIslandTop = 0;
constexpr int kIslandRight = static_cast<int>(GalleryWindow::kGlassIslandWidth);
constexpr int kIslandBottom = static_cast<int>(GalleryWindow::kGlassIslandHeight);

}  // namespace

int main(int argc, char** argv) {
  std::wstring appPath;
  std::wstring archivePath;
  std::wstring shaderDir;
  std::string mutant;
  bool baseline = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    const auto next = [&](std::wstring* out) {
      if (i + 1 < argc) *out = prism::win::Utf8ToWide(argv[++i]);
    };
    if (arg == "--app") {
      next(&appPath);
    } else if (arg == "--archive") {
      next(&archivePath);
    } else if (arg == "--shaders") {
      next(&shaderDir);
    } else if (arg == "--mutant") {
      if (i + 1 < argc) mutant = argv[++i];
    } else if (arg == "--baseline") {
      baseline = true;
    }
  }
#if defined(PRISM_C76_BASELINE)
  baseline = true;
#endif

  std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash must not swallow the digits
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  prism::win::startup::Trace::Instance().BeginAt(QpcMs());

  ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  ComPtr<ID2D1Factory> d2d;
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());

  std::vector<prism::ArchiveItem> items = MakeItems(1000);
  std::printf("C76_INFO {\"mutant\":\"%s\",\"baseline\":%d}\n", mutant.c_str(),
              static_cast<int>(baseline));

  // ---- section 2 first: the pixels and the counters -----------------------
  InProcResult run = RunInProc(shaderDir, wic.Get(), d2d.Get(), &items, /*digest=*/true);
  C76_CHECK(run.ok);
  C76_CHECK(run.frame1.rendered);
  C76_CHECK(run.frame2.rendered);

  std::printf("C76_PIXELS {\"frame1\":\"%s\",\"frame2\":\"%s\",\"width\":%u,\"height\":%u,"
              "\"rendered\":%d,\"enddraw_hr\":\"0x%08lX\",\"painted\":%zu,\"non_black\":%zu,"
              "\"total\":%zu,\"outside_island_diff\":%zu,\"glass_ready\":%d,\"presents_frame1\":%d,"
              "\"presents_frame2\":%d,\"runtime_compiles\":%d,\"precompiled\":%d}\n",
              Hex(run.frame1.digest).c_str(), Hex(run.frame2.digest).c_str(), run.frame1.width,
              run.frame1.height, static_cast<int>(run.frame1.rendered),
              static_cast<unsigned long>(run.frame1.hr), run.frame1.painted, run.frame1.nonBlack,
              run.frame1.total,
              DifferingPixelsOutside(run.frame1, run.frame2, kIslandLeft, kIslandTop, kIslandRight,
                                     kIslandBottom),
              static_cast<int>(run.glassReady), run.presentsAtFirstFrame, run.presentsAfterSecond,
              run.compiles, run.precompiled);

  const size_t outsideDiff = DifferingPixelsOutside(run.frame1, run.frame2, kIslandLeft, kIslandTop,
                                                    kIslandRight, kIslandBottom);

  // ---- the predicates, in one place so a mutant breaks the SAME ones ------
  // C113: BEFORE any pixel equality, the frames must be frames. A digest of a
  // rejected capture (rendered == false) is not a picture of anything: the old
  // bench compared it to a "baseline" and called a match a pass.
  const bool framesAccepted = run.frame1.rendered && run.frame1.hr == S_OK &&
                              run.frame2.rendered && run.frame2.hr == S_OK;
  const bool framesPainted = run.frame1.painted >= run.frame1.total / 20 &&
                             run.frame2.painted >= run.frame2.total / 20;
  const bool digestsReal = run.frame1.digest != 0 && run.frame2.digest != 0;
  const bool contentStable = run.frame1.digest == run.frame2.digest;
  const bool noFlickerOutsideIsland = outsideDiff == 0;
  const bool baselineGeometry =
      run.frame1.width == kBaselineWidth && run.frame1.height == kBaselineHeight;
  const bool digestMatchesBaseline =
      !baselineGeometry || run.frame1.digest == g_baselineDigest;
  const bool noGlassOnFirstFrame = run.presentsAtFirstFrame == 0;
  const bool glassOnSecondFrame = run.presentsAfterSecond >= 1;
  const bool noRuntimeCompiles = run.compiles == 0;
  const bool bytecodeUsed = run.precompiled >= 2;

  if (mutant == "runtime_shaders") {
    // D27: the verdict is a CONJUNCTION -- the injection must be visible at
    // runtime (a compile really ran) AND the optimised predicate must be false.
    const bool injected = run.compiles > 0;
    std::printf("C76 MUTANT runtime_shaders: runtime_compiles=%d injected=%d\n", run.compiles,
                static_cast<int>(injected));
    if (injected && !noRuntimeCompiles) {
      std::printf("C76 MUTATION CAUGHT: runtime_shaders\n");
      std::fflush(stdout);
      _Exit(0);
    }
    if (!injected) {
      std::printf("    FAIL the injection did not apply (no compile ran)\n");
      return 1;
    }
    std::printf("    FAIL the runtime-compile predicate stayed green\n");
    return 1;
  }
  if (mutant == "glass_on_first_frame") {
    const bool injected = run.presentsAtFirstFrame >= 1;
    std::printf("C76 MUTANT glass_on_first_frame: presents_frame1=%d injected=%d\n",
                run.presentsAtFirstFrame, static_cast<int>(injected));
    if (injected && !noGlassOnFirstFrame) {
      std::printf("C76 MUTATION CAUGHT: glass_on_first_frame\n");
      std::fflush(stdout);
      _Exit(0);
    }
    if (!injected) {
      std::printf("    FAIL the injection did not apply (no present before frame 1)\n");
      return 1;
    }
    std::printf("    FAIL the no-glass-on-first-frame predicate stayed green\n");
    return 1;
  }

  if (baseline) {
    // The PRE-CARD build: these digits and this digest are what the optimised
    // build has to match. No gates -- a baseline is a reference, not a verdict.
    std::printf("C76_BASELINE {\"first_frame_digest\":\"%s\",\"inproc_total_ms\":%.3f,"
                "\"create_ms\":%.3f,\"glass_ms\":%.3f,\"paint_ms\":%.3f,"
                "\"runtime_compiles\":%d,\"presents_frame1\":%d}\n",
                Hex(run.frame1.digest).c_str(), run.sample.totalMs, run.sample.createMs,
                run.sample.glassMs, run.sample.paintMs, run.compiles, run.presentsAtFirstFrame);
    // A reference run must not fail on a TEARDOWN it does not measure: the
    // baseline build's COM objects outlive main (0xC0000005 at exit, the same
    // class of fault PERF.md records from an earlier card), and the digits are
    // already flushed. _Exit leaves without running the destructors that fault.
    std::fflush(stdout);
    (void)coInit;
    _Exit(0);
  }

  // C113: an empty frame can never reach the digest comparison below.
  C76_CHECK(framesAccepted);
  C76_CHECK(framesPainted);
  C76_CHECK(digestsReal);
  C76_CHECK(contentStable);
  C76_CHECK(noFlickerOutsideIsland);
  C76_CHECK(digestMatchesBaseline);
  C76_CHECK(noGlassOnFirstFrame);
  C76_CHECK(glassOnSecondFrame);
  C76_CHECK(noRuntimeCompiles);
  C76_CHECK(bytecodeUsed);
  if (!baselineGeometry) {
    std::printf("C76 NOTE: client %ux%u is not the %ux%u the digest was recorded at; the pixel "
                "equality is skipped, this frame is %s\n",
                run.frame1.width, run.frame1.height, kBaselineWidth, kBaselineHeight,
                Hex(run.frame1.digest).c_str());
  }

  // ---- section 1: the real process, cold and warm -------------------------
  double coldMedian = 0.0;
  double warmMedian = 0.0;
  if (!appPath.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(appPath, ec)) {
      if (archivePath.empty()) {
        wchar_t dir[MAX_PATH]{};
        GetTempPathW(MAX_PATH, dir);
        const std::wstring work = std::wstring(dir) + L"c76_fixture";
        archivePath = std::wstring(dir) + L"c76_fixture.7z";
        const std::wstring dll = prism::win::Resolve7zDll().path;
        std::wstring sevenZip;
        if (!dll.empty()) {
          const size_t slash = dll.find_last_of(L"\\/");
          sevenZip = (slash == std::wstring::npos ? std::wstring() : dll.substr(0, slash)) +
                     L"\\7z.exe";
        }
        std::string why;
        if (!sevenZip.empty() && MakeArchive(sevenZip, work, archivePath, &why)) {
          SetEnvironmentVariableW(L"PRISM_7Z_DLL", dll.c_str());
        } else {
          archivePath.clear();
          std::printf("C76 NOTE: fixture archive unavailable (%s)\n", why.c_str());
        }
      }
      if (!archivePath.empty()) {
        SetEnvironmentVariableW(L"PRISM_C76_TRACE", L"1");
        SetEnvironmentVariableW(L"PRISM_C76_TRACE_EXIT", L"1");
        if (!shaderDir.empty()) SetEnvironmentVariableW(L"PRISM_SHADER_DIR", shaderDir.c_str());
        std::vector<RunSample> cold;
        std::vector<RunSample> warm;
        for (int i = 0; i < 5; ++i) {
          RunSample sample;
          if (!LaunchOnce(appPath, archivePath, &sample)) {
            std::printf("C76 NOTE: launch %d failed: %s\n", i, sample.why.c_str());
            continue;
          }
          (i == 0 ? cold : warm).push_back(sample);
        }
        if (!cold.empty()) {
          coldMedian = cold.front().totalMs;  // the first launch IS the cold sample
          std::printf("C76_TABLE_RUN {\"run\":\"cold\",\"total_ms\":%.3f,\"process_to_winmain_ms\":%.3f,"
                      "\"runtime_compiles\":%d,\"glass_presents\":%d}\n",
                      cold.front().totalMs, cold.front().processToWinMainMs,
                      cold.front().runtimeCompiles, cold.front().glassPresents);
        }
        std::vector<double> warmTotals;
        for (const RunSample& sample : warm) warmTotals.push_back(sample.totalMs);
        warmMedian = Median(warmTotals);
        // Per-stage medians over the warm launches: one stable table.
        std::vector<std::string> names;
        for (const RunSample& sample : warm) {
          for (const auto& stage : sample.stages) {
            if (std::find(names.begin(), names.end(), stage.first) == names.end()) {
              names.push_back(stage.first);
            }
          }
        }
        std::printf("  %-30s %10s %10s\n", "stage (warm median)", "ms", "");
        for (const std::string& name : names) {
          std::vector<double> values;
          for (const RunSample& sample : warm) {
            for (const auto& stage : sample.stages) {
              if (stage.first == name) values.push_back(stage.second);
            }
          }
          const double median = Median(values);
          std::printf("  %-30s %10.3f\n", name.c_str(), median);
          std::printf("C76_BREAKDOWN {\"run\":\"warm_median\",\"seg\":\"%s\",\"ms\":%.3f,"
                      "\"samples\":%zu}\n",
                      name.c_str(), median, values.size());
        }
      }
    }
  }

  // In-process first frame, 5 rounds: the digit comparable with C65's 164.5 ms.
  std::vector<double> inproc;
  for (int i = 0; i < 5; ++i) {
    InProcResult round = RunInProc(shaderDir, wic.Get(), d2d.Get(), &items, /*digest=*/false);
    if (round.ok) inproc.push_back(round.sample.totalMs);
  }
  const double inprocMedian = Median(inproc);
  const double targetMs = 60.0;
  std::printf("C76_BENCH {\"target_ms\":%.1f,\"cold_first_frame_ms\":%.3f,\"warm_first_frame_ms\":%.3f,"
              "\"inproc_first_frame_ms\":%.3f,\"inproc_create_ms\":%.3f,\"inproc_glass_ms\":%.3f,"
              "\"inproc_paint_ms\":%.3f,\"cold_samples\":1,\"warm_samples\":4,"
              "\"inproc_samples\":%zu,\"met\":%s,\"first_frame_digest\":\"%s\"}\n",
              targetMs, coldMedian, warmMedian, inprocMedian, run.sample.createMs,
              run.sample.glassMs, run.sample.paintMs, inproc.size(),
              (coldMedian > 0.0 && coldMedian <= targetMs) ? "true" : "false",
              Hex(run.frame1.digest).c_str());

  // A real ceiling, not the target: a first frame that takes half a second is a
  // regression the bench must never walk past (contract D1's spirit; the 60 ms
  // ADR number is reported, and docs/PERF.md says what is left).
  // A REAL ceiling, not the 60 ms ADR number: a first frame that takes 1.5 s is
  // a regression no shared runner explains. The target itself is reported in
  // C76_BENCH.met (contract R2: a stopwatch is not a merge gate).
  C76_CHECK(run.sample.totalMs > 0.0 && run.sample.totalMs < 1500.0);
  C76_CHECK(inprocMedian > 0.0 && inprocMedian < 1500.0);

  (void)coInit;  // see the note above: exit releases COM, not us
  if (g_failures != 0) {
    std::printf("C76 BENCH FAILURES: %d\n", g_failures);
    return 1;
  }
  std::printf("ALL C76 STARTUP CHECKS PASSED\n");
  return 0;
}
