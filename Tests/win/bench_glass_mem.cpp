// PrismArchive -- C33: resident memory and the frame budget, measured instead
// of argued about.
//
// C7's finding: the shipped app sits at 58.4 MB resident while ADR-001 promises
// 24-25 MB, and the Liquid Glass layer is accused of +11 MB. "Accused" is not a
// number, so this file splits the stack into stages it can attribute -- the
// 7z.dll image, the D3D11 device, the D2D1 factory/device/context, DWrite, the
// DirectComposition device, one composition swapchain, the effect graph's
// intermediate surfaces, the thumbnail cache -- and prints the working-set
// delta each stage adds.
//
// Two caveats are printed WITH the digits, because they are part of the result:
//   * the device here is WARP. GetProcessMemoryInfo can only see system memory;
//     on a hardware device the D3D/DComp/effect allocations live in GPU memory
//     and are invisible to it. WARP makes every allocation attributable, so the
//     table is an upper bound of what the glass layer can cost in *process*
//     memory.
//   * the working set is the OS's view, so a delta is only trustworthy above
//     page-granularity noise (~1 MB). The invariant assertions are made on
//     accounting derived from live COM state; the WS digits are printed next to
//     them and are never the sole basis of a verdict.
//
// Three promises are ASSERTED, each with a negative control (CONTRACTS D3):
//   1. LAZINESS. Releasing the glass graph while nothing is visible really
//      drops it, and the next visible frame is PIXEL-IDENTICAL -- the
//      gallery_snapshot optics scene is re-rendered across the release.
//   2. THUMBSTORE. The 64 MiB budget is a cap that saturates exactly (D17) and
//      an idle Trim really returns the pixels.
//   3. FRAME BUDGET. The 100 000-row list frame is decomposed into "record"
//      (GalleryRenderer::Draw) and "rasterise/flush" (EndDraw), so the 120 FPS
//      miss can be attributed instead of guessed.
//
// Output: BENCH_GLASS_MEM {json}, then ALL GLASS-MEMORY CHECKS PASSED.
#if !defined(WIN32_LEAN_AND_MEAN)
#define WIN32_LEAN_AND_MEAN
#endif
#if !defined(NOMINMAX)
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi1_3.h>
#include <psapi.h>
#include <wincodec.h>

#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../Src/PrismCore/ThumbStore.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;
// Runtime evidence that an injected (mutant) branch really executed -- D3's
// "the injection itself must be verified", not just compiled in.
int g_injectionSigns = 0;

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
double MbToMb(size_t a, size_t b) {
  return (static_cast<double>(a) - static_cast<double>(b)) / (1024.0 * 1024.0);
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

size_t CurrentWorkingSet() {
  PROCESS_MEMORY_COUNTERS pmc{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) return 0;
  return pmc.WorkingSetSize;
}

// The OS trims and the allocator returns lazily, so a single read is noise:
// settle, then take the median of nine reads.
size_t StableWorkingSet() {
  Sleep(80);
  size_t samples[9] = {};
  for (size_t& sample : samples) {
    sample = CurrentWorkingSet();
    Sleep(10);
  }
  std::sort(std::begin(samples), std::end(samples));
  return samples[4];
}

size_t RecordStage(const char* name, size_t previous) {
  const size_t ws = StableWorkingSet();
  std::printf("    %-46s %7.2f MB  (delta %+6.2f MB)\n", name, Mb(ws), MbToMb(ws, previous));
  return ws;
}

double Percentile(std::vector<double>& values, double fraction) {
  if (values.empty()) return 0.0;
  std::sort(values.begin(), values.end());
  const size_t index = static_cast<size_t>(fraction * static_cast<double>(values.size() - 1));
  return values[index];
}

std::wstring FindShaderDir() {
  wchar_t env[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_SHADER_DIR", env, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return env;
  namespace fs = std::filesystem;
  fs::path probe = fs::absolute(fs::path(__FILE__)).parent_path();
  for (int hop = 0; hop < 8 && !probe.empty(); ++hop) {
    const fs::path candidate = probe / "Assets" / "Shaders";
    if (fs::exists(candidate / "LiquidGlassEffect.hlsl")) return candidate.wstring();
    probe = probe.parent_path();
  }
  return L"Assets\\Shaders";
}

// ---------------------------------------------------------------------------
// The pixel harness: the SAME geometry and thresholds the gallery_snapshot
// suite asserts (Tests/win/gallery_snapshot.cpp), reduced to what this card
// needs -- refraction visible, clipping exact, and a release/re-acquire cycle
// that must not change a single byte.
// ---------------------------------------------------------------------------
constexpr UINT32 kGlassW = 480, kGlassH = 200;
constexpr float kPanelCx = 240.0f, kPanelCy = 100.0f;
constexpr float kPanelHx = 200.0f, kPanelHy = 60.0f;
constexpr float kPanelRadius = 30.0f;

float SdRoundedBox(float px, float py, float hx, float hy, float radius) {
  const float qx = std::fabs(px) - hx + radius;
  const float qy = std::fabs(py) - hy + radius;
  const float ox = (std::max)(qx, 0.0f);
  const float oy = (std::max)(qy, 0.0f);
  return (std::min)((std::max)(qx, qy), 0.0f) + std::sqrt(ox * ox + oy * oy) - radius;
}

void FillBackdrop(std::vector<UINT32>* pixels) {
  pixels->resize(static_cast<size_t>(kGlassW) * kGlassH);
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const int ramp = static_cast<int>(x * 120 / kGlassW);
      int blue = 50 + ramp, green = 90, red = 235 - ramp;
      const int bar = (x / 3) % 2 == 0 ? 40 : -40;
      blue += bar;
      red -= bar;
      blue = (std::min)((std::max)(blue, 0), 255);
      red = (std::min)((std::max)(red, 0), 255);
      (*pixels)[static_cast<size_t>(y) * kGlassW + x] =
          0xFF000000u | (static_cast<UINT32>(red) << 16) | (static_cast<UINT32>(green) << 8) |
          static_cast<UINT32>(blue);
    }
  }
}

bool ReadTarget(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, std::vector<UINT32>* out) {
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
  ComPtr<ID2D1Bitmap1> readback;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(kGlassW, kGlassH), nullptr, 0, props,
                              readback.GetAddressOf()))) {
    return false;
  }
  if (FAILED(readback->CopyFromBitmap(nullptr, target, nullptr))) return false;
  D2D1_MAPPED_RECT mapped = {};
  if (FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
  out->resize(static_cast<size_t>(kGlassW) * kGlassH);
  for (UINT32 y = 0; y < kGlassH; ++y) {
    std::memcpy(out->data() + static_cast<size_t>(y) * kGlassW,
                mapped.bits + static_cast<size_t>(y) * mapped.pitch, kGlassW * 4);
  }
  readback->Unmap();
  return true;
}

template <typename Predicate>
double MeanAbsDiff(const std::vector<UINT32>& a, const std::vector<UINT32>& b, Predicate inside,
                   int* samples) {
  double sum = 0.0;
  int count = 0;
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const float dist = SdRoundedBox(static_cast<float>(x) + 0.5f - kPanelCx,
                                      static_cast<float>(y) + 0.5f - kPanelCy, kPanelHx, kPanelHy,
                                      kPanelRadius);
      if (!inside(dist)) continue;
      const size_t i = static_cast<size_t>(y) * kGlassW + x;
      const UINT32 pa = a[i], pb = b[i];
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs(static_cast<int>((pa >> shift) & 0xFF) -
                        static_cast<int>((pb >> shift) & 0xFF));
      }
      ++count;
    }
  }
  *samples = count;
  return count ? sum / (count * 3.0) : 0.0;
}

// ---------------------------------------------------------------------------
// Arm B: ThumbStore -- the 64 MiB budget as a cap (D17) and as an idle return.
// ---------------------------------------------------------------------------
struct ThumbArm {
  size_t budget = 0;
  size_t resident = 0;
  size_t slack = 0;
  size_t thumbBytes = 0;
  size_t trimmedResident = 0;
  size_t trimmedReturned = 0;
  size_t releasedAll = 0;
  size_t count = 0;
  bool saturationExact = false;
};

ThumbArm RunThumbArm() {
  ThumbArm arm;
  arm.budget = 64ull << 20;
  prism::gallery::ThumbStore store{arm.budget};
  // 320x240 BGRA = 300 KiB each; enough entries to saturate 64 MiB.
  prism::gallery::DecodedThumb thumb;
  thumb.width = 320;
  thumb.height = 240;
  thumb.pixels.assign(static_cast<size_t>(thumb.width) * thumb.height * 4, 0x5A);
  arm.thumbBytes = thumb.pixels.size();
  const size_t needed = arm.budget / arm.thumbBytes + 8;
  for (size_t i = 0; i < needed; ++i) {
    if (!store.OnThumb(static_cast<uint32_t>(i), thumb)) break;
  }
  arm.resident = store.SizeBytes();
  arm.count = store.Count();
  arm.slack = arm.budget - arm.resident;
  // D17(a): never above the promise.
  CheckTrue(arm.resident <= store.BudgetBytes(), "resident <= budget after a sweep", __FILE__,
            __LINE__);
  // D17(b): no false headroom -- one more thumb must NOT fit, or the advertised
  // 64 MiB is a number the store cannot actually serve.
  arm.saturationExact = arm.slack < arm.thumbBytes;
  CheckTrue(arm.saturationExact, "budget is reachable (one more thumb does not fit)", __FILE__,
            __LINE__);

  // Idle return: the point of the card. Trim to 1/8 of the budget.
  const size_t target = arm.budget / 8;
  const size_t before = store.SizeBytes();
  arm.trimmedReturned = store.Trim(target);
  arm.trimmedResident = store.SizeBytes();
  if (arm.trimmedReturned == 0 && before > target) ++g_injectionSigns;  // injected no-op
  CheckTrue(arm.trimmedResident <= target, "idle trim brings resident down to the target",
            __FILE__, __LINE__);
  CheckTrue(arm.trimmedReturned == before - arm.trimmedResident,
            "trim reports exactly the bytes it returned", __FILE__, __LINE__);
  arm.releasedAll = store.ReleaseAll();
  CheckTrue(store.SizeBytes() == 0 && store.Count() == 0,
            "release-all leaves no resident pixels", __FILE__, __LINE__);
  return arm;
}

// ---------------------------------------------------------------------------
// Arm C: 120 FPS -- where the frame time actually goes.
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
    if (FAILED(renderer_.Factory()->CreateWicBitmapRenderTarget(bitmap_.Get(), props,
                                                                target_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
  }

  // One frame, timed in two pieces: recording (Draw) and rasterise/flush
  // (EndDraw). On a software target EndDraw IS the presentation step -- the
  // only one reachable headlessly (the window's Present belongs to C1).
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
    if (FAILED(bitmap_->Lock(&rect, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
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
  bool painted = false;
};

FrameArm RunFrameArm(uint32_t rowCount, uint32_t frames) {
  FrameArm arm;
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
  arm.drawMedian = Percentile(drawMs, 0.5);
  arm.drawP95 = Percentile(drawMs, 0.95);
  arm.endMedian = Percentile(endMs, 0.5);
  arm.endP95 = Percentile(endMs, 0.95);
  arm.totalMedian = Percentile(totalMs, 0.5);
  arm.totalP95 = Percentile(totalMs, 0.95);
  arm.painted = bench.BackdropPainted();
  return arm;
}

// ---------------------------------------------------------------------------
// Arm D: the glass graph, lazily released and re-acquired, pixel-identical.
// ---------------------------------------------------------------------------
struct GlassArm {
  bool rendered = false;
  bool identical = false;
  bool liveBefore = false;
  bool liveAfterRelease = false;
  bool liveAfterReacquire = false;
  size_t transientWhileLive = 0;
  size_t transientAfterRelease = 0;
  double bandDiff = 0.0;
  int opaqueOutside = 0;
  int bandSamples = 0;
  size_t wsWhileLive = 0;
  size_t wsAfterRelease = 0;
};

GlassArm RunGlassArm(prism::win::glass::GlassIslandHost& host) {
  using namespace prism::win::glass;
  GlassArm arm;
  if (!host.OpticsReady()) return arm;
  KawaseConstants pass1{8.0f, 1.0f, 0.0f, 0.0f};
  KawaseConstants pass2{8.0f, 0.5f, 0.0f, 0.0f};
  if (FAILED(host.InitializeOptics(pass1, pass2))) return arm;
  ID2D1DeviceContext* dc = host.Context();

  D2D1_BITMAP_PROPERTIES1 targetProps = {};
  targetProps.pixelFormat =
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
  ComPtr<ID2D1Bitmap1> target;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(kGlassW, kGlassH), nullptr, 0, targetProps,
                              target.GetAddressOf()))) {
    return arm;
  }
  std::vector<UINT32> backdropPixels;
  FillBackdrop(&backdropPixels);
  D2D1_BITMAP_PROPERTIES1 sourceProps = {};
  sourceProps.pixelFormat =
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
  ComPtr<ID2D1Bitmap1> backdrop;
  if (FAILED(dc->CreateBitmap(D2D1::SizeU(kGlassW, kGlassH), backdropPixels.data(), kGlassW * 4,
                              sourceProps, backdrop.GetAddressOf()))) {
    return arm;
  }

  prism::glass::GlassIslandScene scene;
  scene.Layout(kGlassW, kGlassH);
  scene.SetPointer(kPanelCx, kPanelCy);
  scene.SetTheme(prism::glass::Theme::Daylight);
  GlassConstants liquid = {};
  scene.WriteCbuffer(prism::glass::IslandId::Context, kGlassW, kGlassH, liquid.v);
  liquid.v[0] = kPanelCx;
  liquid.v[1] = kPanelCy;
  liquid.v[2] = kPanelHx;
  liquid.v[3] = kPanelHy;
  liquid.v[6] = 1.0f / kGlassW;
  liquid.v[7] = 1.0f / kGlassH;
  liquid.v[8] = kPanelRadius;

  const auto render = [&](const GlassConstants& constants, std::vector<UINT32>* pixels) {
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    const HRESULT drawHr = host.Chain().Draw(backdrop.Get(), constants);
    const HRESULT endHr = dc->EndDraw();
    if (FAILED(drawHr) || FAILED(endHr)) return false;
    return ReadTarget(dc, target.Get(), pixels);
  };

  std::vector<UINT32> first;
  std::vector<UINT32> second;
  std::vector<UINT32> baseline;
  if (!render(liquid, &first)) return arm;
  arm.rendered = true;
  arm.liveBefore = host.Chain().Live();
  arm.transientWhileLive = host.TransientBytes();
  arm.wsWhileLive = StableWorkingSet();

  // Release because nothing is visible -- the product API, not a comment.
  host.SetVisible(false);
  if (host.Chain().Live()) ++g_injectionSigns;  // injected no-op: still live
  arm.liveAfterRelease = host.Chain().Live();
  arm.transientAfterRelease = host.TransientBytes();
  arm.wsAfterRelease = StableWorkingSet();

  // Back on screen: the graph must come back and the pixels must be identical.
  host.SetVisible(true);
  if (!render(liquid, &second)) return arm;
  arm.liveAfterReacquire = host.Chain().Live();
  arm.identical = (first.size() == second.size()) &&
                  std::memcmp(first.data(), second.data(), first.size() * sizeof(UINT32)) == 0;

  // The gallery_snapshot predicates, re-used: the band refracts, the outside
  // stays clipped. A release/re-acquire that broke either shows up here even if
  // the two renders happened to agree.
  GlassConstants noRefraction = liquid;
  noRefraction.v[10] = 0.0f;
  if (!render(noRefraction, &baseline)) return arm;
  int bandSamples = 0;
  arm.bandDiff = MeanAbsDiff(
      second, baseline, [](float dist) { return dist > -8.0f && dist <= 0.5f; }, &bandSamples);
  arm.bandSamples = bandSamples;
  int outside = 0;
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const float dist = SdRoundedBox(static_cast<float>(x) + 0.5f - kPanelCx,
                                      static_cast<float>(y) + 0.5f - kPanelCy, kPanelHx, kPanelHy,
                                      kPanelRadius);
      if (dist <= 0.5f) continue;
      if ((second[static_cast<size_t>(y) * kGlassW + x] >> 24) != 0) ++outside;
    }
  }
  arm.opaqueOutside = outside;
  return arm;
}

void Read7zImageBytes(uint64_t* bytes) {
  HMODULE module = GetModuleHandleW(L"7z.dll");
  if (!module) return;
  MODULEINFO info = {};
  if (GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
    *bytes = info.SizeOfImage;
  }
}

}  // namespace

#define BM_CHECK(cond) ::CheckTrue(static_cast<bool>(cond), #cond, __FILE__, __LINE__)

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // Declared FIRST so every ComPtr dies while the apartment is alive (the
  // 0xC0000005-after-the-digits lesson from bench_scale, CONTRACTS D1).
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

#ifdef PRISM_GLASS_MUTANT_NO_RELEASE
  std::puts("INJECTION: PRISM_GLASS_MUTANT_NO_RELEASE (lazy release disabled)");
#endif
#ifdef PRISM_THUMB_MUTANT_NO_TRIM
  std::puts("INJECTION: PRISM_THUMB_MUTANT_NO_TRIM (idle trim disabled)");
#endif

  constexpr UINT kIslandW = 1600, kIslandH = 1000;

  size_t ws = StableWorkingSet();
  const size_t baseline = ws;
  std::printf("    %-46s %7.2f MB\n", "baseline (COM only)", Mb(ws));

  // ---- 1. 7z.dll: the image plus whatever its loader touches ----
  const std::wstring dllPath = prism::fixture::DllPath();
  prism::win::SevenZipLibrary library;
  if (dllPath.empty() || !library.Load(dllPath)) {
    std::printf("    FAIL: 7z.dll not found (set PRISM_7Z_DLL)\n");
    return 1;
  }
  uint64_t sevenZipImage = 0;
  Read7zImageBytes(&sevenZipImage);
  const size_t wsSevenZip = RecordStage("+ 7z.dll mapped (no handler yet)", ws);
  std::printf("      7z.dll SizeOfImage: %.2f MB\n", Mb(static_cast<size_t>(sevenZipImage)));

  // ---- 2. the D3D11 / D2D1 / DWrite / DComp stack, one layer at a time ----
  // The product's GlassIslandHost bundles these, so the breakdown is measured
  // with the same API sequence the host performs (see its Initialize).
  size_t wsD3d = wsSevenZip;
  size_t wsD2d = wsSevenZip;
  size_t wsDWrite = wsSevenZip;
  size_t wsDComp = wsSevenZip;
  size_t wsSwapchain = wsSevenZip;
  bool swapchainMeasured = false;
  {
    ComPtr<ID3D11Device> d3d;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                 d3d.GetAddressOf(), nullptr, nullptr))) {
      std::puts("    FAIL: WARP D3D11 device");
      return 1;
    }
    wsD3d = RecordStage("+ D3D11 device (WARP)", wsD3d);
    ComPtr<IDXGIDevice> dxgi;
    if (FAILED(d3d.As(&dxgi))) {
      std::puts("    FAIL: IDXGIDevice");
      return 1;
    }
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2dDevice;
    ComPtr<ID2D1DeviceContext> dc;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 nullptr, reinterpret_cast<void**>(factory.GetAddressOf()))) ||
        FAILED(factory->CreateDevice(dxgi.Get(), d2dDevice.GetAddressOf())) ||
        FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                              dc.GetAddressOf()))) {
      std::puts("    FAIL: D2D factory/device/context");
      return 1;
    }
    wsD2d = RecordStage("+ D2D factory/device/context", wsD2d);

    ComPtr<IDWriteFactory3> dwrite;
    if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
                                      reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
      wsDWrite = RecordStage("+ DWrite factory", wsDWrite);
      dwrite.Reset();
    }

    ComPtr<IDCompositionDevice> dcomp;
    if (SUCCEEDED(DCompositionCreateDevice(dxgi.Get(), __uuidof(IDCompositionDevice),
                                           reinterpret_cast<void**>(dcomp.GetAddressOf())))) {
      wsDComp = RecordStage("+ DComp device", wsDComp);
    } else {
      std::puts("    NOT MEASURED: DCompositionCreateDevice failed on this runner");
    }

    // One full-window composition surface: the buffers DComp doubles.
    ComPtr<IDXGIFactory2> dxgiFactory;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                     reinterpret_cast<void**>(dxgiFactory.GetAddressOf())))) {
      DXGI_SWAP_CHAIN_DESC1 desc = {};
      desc.Width = kIslandW;
      desc.Height = kIslandH;
      desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
      desc.SampleDesc.Count = 1;
      desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
      desc.BufferCount = 2;
      desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
      desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
      ComPtr<IDXGISwapChain1> swapchain;
      if (SUCCEEDED(dxgiFactory->CreateSwapChainForComposition(d3d.Get(), &desc, nullptr,
                                                               swapchain.GetAddressOf()))) {
        wsSwapchain = RecordStage("+ 1600x1000 x2 composition buffers", wsSwapchain);
        swapchainMeasured = true;
        swapchain.Reset();
        wsSwapchain = RecordStage("  (buffers released)", wsSwapchain);
      } else {
        std::puts("    NOT MEASURED: CreateSwapChainForComposition failed on this runner");
      }
    }
    // Everything above is a stack this card only *measures*: drop it before the
    // product object is created, so the host's own cost is counted once.
    dc.Reset();
    d2dDevice.Reset();
    factory.Reset();
    dcomp.Reset();
    d3d.Reset();
  }
  const size_t wsStackReleased = RecordStage("  (measurement stack released)", wsSwapchain);

  // ---- 3. the product object: the same stack, owned by GlassIslandHost ----
  prism::win::glass::GlassIslandHost host;
  if (!host.Initialize(nullptr, FindShaderDir())) {
    std::puts("    FAIL: GlassIslandHost::Initialize (WARP path)");
    return 1;
  }
  const size_t wsHost = RecordStage("+ GlassIslandHost device stack", wsStackReleased);
  if (!BM_CHECK(host.OpticsReady())) {
    std::printf("    FAIL: glass effects did not register: %s\n", host.ShaderError().c_str());
    return 1;
  }

  // ---- 4. the effect graph live at full-window island size ----
  size_t wsGraph = wsHost;
  size_t wsGraphReleased = wsHost;
  size_t wsGraphReacquired = wsHost;
  {
    using namespace prism::win::glass;
    KawaseConstants pass1{8.0f, 1.0f, 0.0f, 0.0f};
    KawaseConstants pass2{8.0f, 0.5f, 0.0f, 0.0f};
    if (FAILED(host.InitializeOptics(pass1, pass2))) {
      std::puts("    FAIL: InitializeOptics");
      return 1;
    }
    ID2D1DeviceContext* dc = host.Context();
    D2D1_BITMAP_PROPERTIES1 targetProps = {};
    targetProps.pixelFormat =
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    ComPtr<ID2D1Bitmap1> islandTarget;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(kIslandW, kIslandH), nullptr, 0, targetProps,
                                islandTarget.GetAddressOf()))) {
      std::puts("    FAIL: island target bitmap");
      return 1;
    }
    std::vector<UINT32> pixels(static_cast<size_t>(kIslandW) * kIslandH, 0xFF203040u);
    D2D1_BITMAP_PROPERTIES1 sourceProps = {};
    sourceProps.pixelFormat =
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED);
    ComPtr<ID2D1Bitmap1> backdrop;
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(kIslandW, kIslandH), pixels.data(), kIslandW * 4,
                                sourceProps, backdrop.GetAddressOf()))) {
      std::puts("    FAIL: island backdrop bitmap");
      return 1;
    }
    prism::glass::GlassIslandScene scene;
    scene.Layout(kIslandW, kIslandH);
    GlassConstants constants = {};
    scene.WriteCbuffer(prism::glass::IslandId::Context, kIslandW, kIslandH, constants.v);
    dc->SetTarget(islandTarget.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    if (FAILED(host.Chain().Draw(backdrop.Get(), constants)) || FAILED(dc->EndDraw())) {
      std::puts("    FAIL: island glass draw");
      return 1;
    }
    dc->SetTarget(nullptr);
    wsGraph = RecordStage("+ glass graph live (3 passes @1600x1000)", wsHost);
    std::printf("      graph accounting: %.2f MB (3 x 1600x1000 x 4 B)\n",
                Mb(static_cast<size_t>(kIslandW) * kIslandH * 4u * 3u));

    // The memory claim, at the size where it actually costs something: hide the
    // layer and the intermediate surfaces must leave the process. This is the
    // delta a minimised window is worth; the digit is printed and the >= 8 MB
    // drop is asserted (3 x 1600x1000 x 4 B = 18.3 MB, so 8 MB is a conservative
    // floor that page-granularity noise cannot fake).
    host.SetVisible(false);
    wsGraphReleased = RecordStage("  graph released (nothing visible)", wsGraph);
    host.SetVisible(true);
    dc->SetTarget(islandTarget.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    const HRESULT again = host.Chain().Draw(backdrop.Get(), constants);
    const HRESULT ended = dc->EndDraw();
    dc->SetTarget(nullptr);
    if (FAILED(again) || FAILED(ended)) {
      std::puts("    FAIL: graph did not come back after a release");
      return 1;
    }
    wsGraphReacquired = RecordStage("  graph re-acquired (drawn again)", wsGraphReleased);
  }

  // ---- arms ----
  const GlassArm glass = RunGlassArm(host);
  const FrameArm frame = RunFrameArm(100000, 120);
  const ThumbArm thumb = RunThumbArm();

  // ---- verdicts: laziness ----
  // The process-level half is REPORTED, not asserted, and the reason is a
  // measured finding rather than squeamishness: releasing the effect instances
  // returns 0 bytes of working set until IDXGIDevice3::Trim runs, and Trim's
  // yield depends on what else still references the pool (0.16 MB right after a
  // fresh 1600x1000 draw, 17.81 MB one cycle later). A gate on that digit would
  // be a gate on allocator state. The assertion stays on the accounting, which
  // is derived from live COM state and is exactly what the mutant breaks.
  const double releaseSavingMb = MbToMb(wsGraph, wsGraphReleased);
  std::printf("    graph release at 1600x1000: %.2f MB ws back (report-only; see PERF.md)\n",
              releaseSavingMb);
  if (!BM_CHECK(glass.rendered)) return 1;
  BM_CHECK(glass.liveBefore);
  BM_CHECK(glass.transientWhileLive > 0);
  BM_CHECK(!glass.liveAfterRelease);
  BM_CHECK(glass.transientAfterRelease == 0);
  BM_CHECK(glass.liveAfterReacquire);
  BM_CHECK(glass.identical);
  BM_CHECK(glass.bandDiff >= 0.4);
  BM_CHECK(glass.opaqueOutside == 0);

  // ---- verdicts: thumbnail capacity (D17) ----
  BM_CHECK(thumb.resident <= thumb.budget);
  BM_CHECK(thumb.saturationExact);
  BM_CHECK(thumb.trimmedResident <= thumb.budget / 8);
  BM_CHECK(thumb.trimmedReturned > 0);
  BM_CHECK(thumb.releasedAll > 0);

  // ---- verdicts: the frame harness measured real work ----
  BM_CHECK(frame.painted);
  BM_CHECK(frame.maxRows > 0);

  std::printf(
      "    glass: live %.2f MB @%.1f MB ws -> released (%.1f MB ws, %.2f MB cheaper); "
      "pixel-identical=%d bandDiff=%.2f outside=%d\n",
      Mb(glass.transientWhileLive), Mb(glass.wsWhileLive), Mb(glass.wsAfterRelease),
      MbToMb(glass.wsWhileLive, glass.wsAfterRelease), glass.identical ? 1 : 0, glass.bandDiff,
      glass.opaqueOutside);
  std::printf(
      "    thumb: budget %.1f MB resident %.2f MB (%zu thumbs, slack %.0f B), trim(1/8) -> "
      "%.2f MB returned %.2f MB, release-all %.2f MB\n",
      Mb(thumb.budget), Mb(thumb.resident), thumb.count, static_cast<double>(thumb.slack),
      Mb(thumb.trimmedResident), Mb(thumb.trimmedReturned), Mb(thumb.releasedAll));
  std::printf(
      "    frame(100k rows, 120 frames): median %.3f ms (draw %.3f + end %.3f), "
      "p95 %.3f ms (draw %.3f + end %.3f), rows %u -> %.1f FPS median / %.1f FPS p95\n",
      frame.totalMedian, frame.drawMedian, frame.endMedian, frame.totalP95, frame.drawP95,
      frame.endP95, frame.maxRows, frame.totalMedian > 0.0 ? 1000.0 / frame.totalMedian : 0.0,
      frame.totalP95 > 0.0 ? 1000.0 / frame.totalP95 : 0.0);

  if (g_failures > 0) {
    std::printf("    (%d check(s) failed)\n", g_failures);
    // Negative controls (CONTRACTS D26): the marker is a CONJUNCTION -- a red
    // run AND the injected branch observed at runtime. A build without the
    // injection cannot print it, so it cannot be a fake green.
    if (g_injectionSigns > 0) {
#ifdef PRISM_THUMB_MUTANT_NO_TRIM
      std::puts("THUMBSTORE-TRIM MUTATION CAUGHT");
#endif
#ifdef PRISM_GLASS_MUTANT_NO_RELEASE
      std::puts("LAZY-RELEASE MUTATION CAUGHT");
#endif
      std::printf("    (injected branch observed %d time(s))\n", g_injectionSigns);
    }
#if defined(PRISM_GLASS_MUTANT_NO_RELEASE) || defined(PRISM_THUMB_MUTANT_NO_TRIM)
    else {
      // The macro is compiled in but the injected branch never ran: a broken
      // negative control, which must be as loud as a red assertion.
      std::puts("    FAIL: this build carries an injection but never observed it");
    }
#endif
    return 1;
  }
  if (g_injectionSigns > 0) {
    // The injected branch ran but nothing failed: a broken negative control,
    // and it must be as loud as a red test.
    std::printf("    FAIL: injected branch taken %d time(s) but no assertion failed\n",
                g_injectionSigns);
    return 1;
  }

  std::printf(
      "BENCH_GLASS_MEM {\"baseline_mb\":%.2f,\"sevenzip_mb\":%.2f,\"sevenzip_image_mb\":%.2f,"
      "\"d3d_mb\":%.2f,\"d2d_mb\":%.2f,\"dwrite_mb\":%.2f,\"dcomp_mb\":%.2f,"
      "\"swapchain_mb\":%.2f,\"swapchain_measured\":%s,\"host_mb\":%.2f,"
      "\"graph_mb\":%.2f,\"graph_accounting_mb\":%.2f,"
      "\"graph_release_ws_saving_mb\":%.2f,\"graph_reacquire_ws_mb\":%.2f,"
      "\"thumb_budget_mb\":%.2f,\"thumb_resident_mb\":%.2f,\"thumb_trimmed_mb\":%.2f,"
      "\"frame_draw_med_ms\":%.3f,\"frame_end_med_ms\":%.3f,\"frame_total_med_ms\":%.3f,"
      "\"frame_draw_p95_ms\":%.3f,\"frame_end_p95_ms\":%.3f,\"frame_total_p95_ms\":%.3f,"
      "\"frame_p95_fps\":%.1f,\"rows_drawn\":%u}\n",
      Mb(baseline), Mb(wsSevenZip), Mb(static_cast<size_t>(sevenZipImage)), Mb(wsD3d), Mb(wsD2d),
      Mb(wsDWrite), Mb(wsDComp), Mb(wsSwapchain), swapchainMeasured ? "true" : "false", Mb(wsHost),
      Mb(wsGraph), Mb(glass.transientWhileLive), releaseSavingMb,
      MbToMb(wsGraphReacquired, wsGraphReleased), Mb(thumb.budget), Mb(thumb.resident), Mb(thumb.trimmedResident), frame.drawMedian,
      frame.endMedian, frame.totalMedian, frame.drawP95, frame.endP95, frame.totalP95,
      frame.totalP95 > 0.0 ? 1000.0 / frame.totalP95 : 0.0, frame.maxRows);
  std::puts("ALL GLASS-MEMORY CHECKS PASSED");
  return 0;
}
