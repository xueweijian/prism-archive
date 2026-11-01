// CI design snapshot: renders the M3 gallery design to WIC bitmaps (headless,
// D2D software target) and writes PNGs for artifact upload. This is the REAL
// renderer the window uses -- not a mock -- so what ships in the artifact is
// what the app paints.
//
//   gallery_snapshot.exe <out-dir>
//   -> <out-dir>/prism_gallery.png       grid + breadcrumb + progress capsule
//   -> <out-dir>/prism_quicklook.png     same + Quick Look glass card
//   -> <out-dir>/prism_gallery_real.png  REAL thumbnails: a genuine solid 7z
//                                        swept single-pass through the real
//                                        7z.dll + WIC pipeline, decoded BGRA
//                                        painted into the tiles
//   -> <out-dir>/prism_list.png          main-window file list mode: 名称 |
//                                        原始大小 | 压缩后 | 压缩率 with
//                                        semantic micro-capsule bars
#include <d2d1_3.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "../../Src/PrismWin/SolidSweeper.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "windowscodecs.lib")
// The optics section renders through a WARP D3D11 device; linking here keeps
// the change inside this file (Tests/CMakeLists.txt is a shared file).
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using namespace prism;
using namespace prism::win::gallery;
using Microsoft::WRL::ComPtr;

namespace {

std::wstring Utf8(std::string_view s) {
  if (s.empty()) return {};
  const int wide = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                       nullptr, 0);
  std::wstring out(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wide);
  return out;
}

ArchiveItem MakeItem(const std::string& path, uint64_t size, uint32_t block, bool dir = false) {
  ArchiveItem item;
  item.path = path;
  item.size = size;
  item.block = block;
  item.isDir = dir;
  return item;
}

std::vector<ArchiveItem> MockBerlinTrip() {
  std::vector<ArchiveItem> items;
  items.push_back(MakeItem("Brandenburg_Gate/", 0, 0, true));
  items.push_back(MakeItem("Brandenburg_Gate/gate_front.png", 4'812'331, 0));
  items.push_back(MakeItem("Brandenburg_Gate/gate_night.png", 3'905'112, 0));
  items.push_back(MakeItem("Brandenburg_Gate/quadriga_closeup.png", 2'214'908, 0));
  items.push_back(MakeItem("Museum_Island/", 0, 1, true));
  items.push_back(MakeItem("Museum_Island/bode_morning.png", 5'118'774, 1));
  items.push_back(MakeItem("Museum_Island/pergamon_hall.png", 6'402'155, 1));
  items.push_back(MakeItem("Museum_Island/neues_dome.png", 4'007'281, 1));
  items.push_back(MakeItem("East_Side_Gallery/", 0, 2, true));
  items.push_back(MakeItem("East_Side_Gallery/mural_01.png", 7'220'464, 2));
  items.push_back(MakeItem("East_Side_Gallery/mural_02.png", 6'833'210, 2));
  items.push_back(MakeItem("East_Side_Gallery/mural_03.png", 5'471'662, 2));
  items.push_back(MakeItem("Kreuzberg/", 0, 3, true));
  for (int i = 1; i <= 14; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "Kreuzberg/street_%02d.png", i);
    items.push_back(MakeItem(name, 3'100'000u + 977u * i, 3));
  }
  items.push_back(MakeItem("itinerary.md", 8'912, 4));
  items.push_back(MakeItem("hotel_booking.pdf", 214'553, 4));
  items.push_back(MakeItem("train_tickets.pdf", 96'204, 4));
  items.push_back(MakeItem("expenses.csv", 4'820, 4));
  return items;
}

}  // namespace

// ---------------------------------------------------------------------------
// Liquid Glass optics: the REAL Direct2D 1.3 effect chain, rendered headlessly
// on a WARP device context and asserted on pixels (ADR-001 + CONTRACTS A6/D2).
//
// What is proven here, in one run:
//   1. LiquidGlassEffect.hlsl + KawaseDualBlur.hlsl (@PRISM_D2D_EFFECT)
//      compile and register through ID2D1Factory1::RegisterEffectFromString;
//   2. the graph backdrop -> Kawase pass 1 -> Kawase pass 2 -> PrismLiquidGlass
//      produces pixels on a real device context;
//   3. inside the refraction BAND the output differs from a pure-blur baseline
//      of the same backdrop (dispersion actually moved the samples), while the
//      panel interior stays identical -- so the effect is not just a global
//      brightness change;
//   4. outside the island every pixel is fully transparent (the shader clips).
//
// Negative control (CONTRACTS D3): build with PRISM_GLASS_MUTANT_NO_REFRACTION
// -- GlassChain::Draw then forces u_iorStrength = 0 and assertion (3) must go
// red. Evidence recorded in the PR.
// ---------------------------------------------------------------------------
namespace {

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

std::wstring FindShaderDir() {
  wchar_t env[MAX_PATH] = {};
  const DWORD len = GetEnvironmentVariableW(L"PRISM_SHADER_DIR", env, MAX_PATH);
  if (len > 0 && len < MAX_PATH) return env;
  namespace fs = std::filesystem;
  fs::path probe = fs::absolute(fs::path(__FILE__)).parent_path();
  for (int hop = 0; hop < 8 && !probe.empty(); ++hop) {
    const fs::path candidate = probe / "Assets" / "Shaders";
    if (fs::exists(candidate / "LiquidGlassEffect.hlsl")) return candidate.wstring();
    probe = probe.parent_path();
  }
  return L"Assets\\Shaders";
}

// High-frequency backdrop: 6 px colour bars plus a secondary checker. A 14 px
// refraction shift has to be obvious, and so does a pure blur.
void FillBackdrop(std::vector<UINT32>* pixels) {
  static const UINT32 kPalette[4] = {0xFFC85A28u, 0xFF28325Au, 0xFF3CB45Au, 0xFF3C8CC8u};
  pixels->resize(static_cast<size_t>(kGlassW) * kGlassH);
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      // Hue ramp in x (survives the blur, so a refraction shift is a real
      // colour displacement) plus a 3 px bar pattern (survives enough to make
      // the blur itself measurable).
      const int ramp = static_cast<int>(x * 120 / kGlassW);
      int blue = 50 + ramp, green = 90, red = 235 - ramp;
      const int bar = (x / 3) % 2 == 0 ? 40 : -40;
      blue += bar; red -= bar;
      blue = (std::min)((std::max)(blue, 0), 255);
      red = (std::min)((std::max)(red, 0), 255);
      const UINT32 color = 0xFF000000u | (static_cast<UINT32>(red) << 16) |
                           (static_cast<UINT32>(green) << 8) | static_cast<UINT32>(blue);
      (*pixels)[static_cast<size_t>(y) * kGlassW + x] = color;
    }
  }
}

bool ReadTarget(ID2D1DeviceContext* dc, ID2D1Bitmap1* target, std::vector<UINT32>* out) {
  D2D1_BITMAP_PROPERTIES1 props = {};
  props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                        D2D1_ALPHA_MODE_PREMULTIPLIED);
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

bool WriteBgraPng(IWICImagingFactory* wic, const char* outDir, const wchar_t* fileName,
                  const std::vector<UINT32>& pixels) {
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%s", outDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmapFromMemory(kGlassW, kGlassH, GUID_WICPixelFormat32bppPBGRA,
                                         kGlassW * 4,
                                         static_cast<UINT>(pixels.size() * 4),
                                         reinterpret_cast<BYTE*>(
                                             const_cast<UINT32*>(pixels.data())),
                                         bitmap.GetAddressOf()))) {
    return false;
  }
  return SUCCEEDED(wic->CreateStream(stream.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                           bag.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(frame->Initialize(bag.Get())) && SUCCEEDED(frame->SetSize(kGlassW, kGlassH)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

// Mean absolute per-channel difference over a pixel predicate.
template <typename Predicate>
double MeanAbsDiff(const std::vector<UINT32>& a, const std::vector<UINT32>& b, Predicate inside,
                   int* samples) {
  double sum = 0.0;
  int count = 0;
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const float dist = SdRoundedBox(static_cast<float>(x) + 0.5f - kPanelCx,
                                      static_cast<float>(y) + 0.5f - kPanelCy, kPanelHx,
                                      kPanelHy, kPanelRadius);
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

int VerifyGlassOptics(IWICImagingFactory* wic, const char* outDir) {
  using namespace prism::win::glass;
  const std::wstring shaderDir = FindShaderDir();
  std::printf("GLASS OPTICS: shaderDir=%ls\n", shaderDir.c_str());

  ComPtr<ID3D11Device> d3d;
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, d3d.GetAddressOf(), nullptr, nullptr);
  if (FAILED(hr)) {
    std::printf("FAIL: WARP device hr=%08lx -- the optics test needs a D3D11 device\n",
                static_cast<unsigned long>(hr));
    return 1;
  }
  ComPtr<IDXGIDevice> dxgi;
  if (FAILED(d3d.As(&dxgi))) {
    std::puts("FAIL: IDXGIDevice");
    return 1;
  }
  ComPtr<ID2D1Factory1> factory;
  hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr,
                         reinterpret_cast<void**>(factory.GetAddressOf()));
  if (FAILED(hr)) {
    std::printf("FAIL: ID2D1Factory1 hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }
  ComPtr<ID2D1Device> device;
  hr = factory->CreateDevice(dxgi.Get(), device.GetAddressOf());
  if (FAILED(hr)) {
    std::printf("FAIL: ID2D1Device hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }
  ComPtr<ID2D1DeviceContext> dc;
  hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, dc.GetAddressOf());
  if (FAILED(hr)) {
    std::printf("FAIL: ID2D1DeviceContext hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }

  GlassEffectRuntime runtime;
  hr = runtime.Initialize(factory.Get(), shaderDir);
  if (FAILED(hr)) {
    std::printf("FAIL: effect registration hr=%08lx %s\n", static_cast<unsigned long>(hr),
                runtime.LastCompileMessage().c_str());
    return 1;
  }
  std::puts("GLASS OPTICS: PrismKawaseBlur + PrismLiquidGlass registered");

  D2D1_BITMAP_PROPERTIES1 targetProps = {};
  targetProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                              D2D1_ALPHA_MODE_PREMULTIPLIED);
  targetProps.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
  ComPtr<ID2D1Bitmap1> target;
  hr = dc->CreateBitmap(D2D1::SizeU(kGlassW, kGlassH), nullptr, 0, targetProps,
                        target.GetAddressOf());
  if (FAILED(hr)) {
    std::printf("FAIL: target bitmap hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }

  std::vector<UINT32> backdropPixels;
  FillBackdrop(&backdropPixels);
  D2D1_BITMAP_PROPERTIES1 sourceProps = {};
  sourceProps.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                              D2D1_ALPHA_MODE_PREMULTIPLIED);
  ComPtr<ID2D1Bitmap1> backdrop;
  hr = dc->CreateBitmap(D2D1::SizeU(kGlassW, kGlassH), backdropPixels.data(), kGlassW * 4,
                        sourceProps, backdrop.GetAddressOf());
  if (FAILED(hr)) {
    std::printf("FAIL: backdrop bitmap hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }

  GlassChain chain;
  KawaseConstants pass1{8.0f, 1.0f, 0.0f, 0.0f};
  KawaseConstants pass2{8.0f, 0.5f, 0.0f, 0.0f};
  hr = chain.Initialize(dc.Get(), pass1, pass2);
  if (FAILED(hr)) {
    std::printf("FAIL: GlassChain::Initialize hr=%08lx\n", static_cast<unsigned long>(hr));
    return 1;
  }

  // The same island the window paints, serialized by the REAL producer.
  prism::glass::GlassIslandScene scene;
  scene.Layout(kGlassW, kGlassH);
  scene.SetPointer(kPanelCx, kPanelCy);
  scene.SetTheme(prism::glass::Theme::Daylight);
  GlassConstants liquid = {};
  scene.WriteCbuffer(prism::glass::IslandId::Context, kGlassW, kGlassH, liquid.v);
  // Pin the panel to the geometry this test measures (Layout's context pill is
  // canvas-wide; the assertion needs a known rounded box).
  liquid.v[0] = kPanelCx;
  liquid.v[1] = kPanelCy;
  liquid.v[2] = kPanelHx;
  liquid.v[3] = kPanelHy;
  liquid.v[6] = 1.0f / kGlassW;
  liquid.v[7] = 1.0f / kGlassH;
  liquid.v[8] = kPanelRadius;
  std::printf("GLASS OPTICS: cbuffer center=(%.1f,%.1f) half=(%.1f,%.1f) radius=%.1f "
              "bevel=%.1f ior=%.1f tint.a=%.2f\n",
              liquid.v[0], liquid.v[1], liquid.v[2], liquid.v[3], liquid.v[8], liquid.v[9],
              liquid.v[10], liquid.v[15]);

  const auto render = [&](const GlassConstants& constants, std::vector<UINT32>* pixels) {
    dc->SetTarget(target.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    const HRESULT drawHr = chain.Draw(backdrop.Get(), constants);
    const HRESULT endHr = dc->EndDraw();
    if (FAILED(drawHr) || FAILED(endHr)) {
      std::printf("FAIL: draw hr=%08lx end=%08lx\n", static_cast<unsigned long>(drawHr),
                  static_cast<unsigned long>(endHr));
      return false;
    }
    return ReadTarget(dc.Get(), target.Get(), pixels);
  };

  std::vector<UINT32> refracted;
  std::vector<UINT32> baseline;
  GlassConstants noRefraction = liquid;
  noRefraction.v[10] = 0.0f;  // pure-blur baseline of the SAME graph
  if (!render(liquid, &refracted)) {
    std::puts("FAIL: refracted render failed");
    return 1;
  }
  if (!render(noRefraction, &baseline)) {
    std::puts("FAIL: baseline render failed");
    return 1;
  }
  if (!WriteBgraPng(wic, outDir, L"prism_glass_refracted.png", refracted)) {
    std::puts("FAIL: refract PNG write");
    return 1;
  }

  int bandSamples = 0;
  int interiorSamples = 0;
  int outsideSamples = 0;
  const double bandDiff = MeanAbsDiff(
      refracted, baseline, [](float dist) { return dist > -8.0f && dist <= 0.5f; },
      &bandSamples);
  // Worst-pixel difference inside the band: a hard edge shifted by the lens is
  // far above the mean, and the mean alone would let a "blur everything"
  // regression hide (blurring changes the mean too, shifting does not).
  int bandMax = 0;
  int bandMoved = 0;  // band pixels whose strongest channel moved by >= 8/255
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const float dist = SdRoundedBox(static_cast<float>(x) + 0.5f - kPanelCx,
                                      static_cast<float>(y) + 0.5f - kPanelCy, kPanelHx,
                                      kPanelHy, kPanelRadius);
      if (!(dist > -8.0f && dist <= 0.5f)) continue;
      const size_t i = static_cast<size_t>(y) * kGlassW + x;
      int pixelMax = 0;
      for (int shift = 0; shift <= 16; shift += 8) {
        const int d = std::abs(static_cast<int>((refracted[i] >> shift) & 0xFF) -
                               static_cast<int>((baseline[i] >> shift) & 0xFF));
        pixelMax = (std::max)(pixelMax, d);
      }
      bandMax = (std::max)(bandMax, pixelMax);
      if (pixelMax >= 8) ++bandMoved;
    }
  }
  const double interiorDiff = MeanAbsDiff(
      refracted, baseline, [](float dist) { return dist < -20.0f; }, &interiorSamples);
  // Blur evidence: the baseline interior must not equal the sharp backdrop.
  const double blurDelta = MeanAbsDiff(
      baseline, backdropPixels, [](float dist) { return dist < -20.0f; }, &interiorSamples);
  // Clip evidence: nothing outside the rounded box may survive.
  int opaqueOutside = 0;
  for (UINT32 y = 0; y < kGlassH; ++y) {
    for (UINT32 x = 0; x < kGlassW; ++x) {
      const float dist = SdRoundedBox(static_cast<float>(x) + 0.5f - kPanelCx,
                                      static_cast<float>(y) + 0.5f - kPanelCy, kPanelHx,
                                      kPanelHy, kPanelRadius);
      if (dist <= 0.5f) continue;
      ++outsideSamples;
      if ((refracted[static_cast<size_t>(y) * kGlassW + x] >> 24) != 0) ++opaqueOutside;
    }
  }
  std::printf("GLASS OPTICS: bandDiff=%.2f bandMax=%d bandMoved=%d/%d interiorDiff=%.2f "
              "interiorPx=%d blurDelta=%.2f opaqueOutside=%d/%d\n",
              bandDiff, bandMax, bandMoved, bandSamples, interiorDiff, interiorSamples,
              blurDelta, opaqueOutside, outsideSamples);

  int failures = 0;
  if (bandSamples < 1000 || interiorSamples < 1000) {
    std::printf("FAIL: band/interior sample counts are too small (band=%d interior=%d)\n",
                bandSamples, interiorSamples);
    ++failures;
  }
  // Calibration (2026-11-01, WARP): 480x200 backdrop, iorStrength 14 px,
  // bevel 8 px -> bandDiff ~0.9 / bandMax ~32 / bandMoved ~1e3 pixels.
  // The pure-blur baseline (u_iorStrength = 0) measures exactly 0 / 0 / 0, and
  // the interior must not move at all: refraction is a band, not a filter.
  if (bandDiff < 0.4) {
    std::printf("FAIL: refraction band is indistinguishable from the pure-blur baseline "
                "(bandDiff=%.2f < 0.4)\n", bandDiff);
    ++failures;
  }
  if (bandMax < 16) {
    std::printf("FAIL: no pixel in the refraction band moved (bandMax=%d < 16)\n", bandMax);
    ++failures;
  }
  if (bandMoved < 120) {
    std::printf("FAIL: only %d band pixels moved by >= 8/255 (< 120)\n", bandMoved);
    ++failures;
  }
  if (interiorDiff > 1.0) {
    std::printf("FAIL: panel interior changed with refraction off (interiorDiff=%.2f > 1.0) -- "
                "the effect is a global change, not a lens\n", interiorDiff);
    ++failures;
  }
  if (blurDelta < 3.0) {
    std::printf("FAIL: Kawase passes did not blur the backdrop (blurDelta=%.2f < 3.0)\n",
                blurDelta);
    ++failures;
  }
  if (opaqueOutside != 0) {
    std::printf("FAIL: %d pixels outside the island were not clipped\n", opaqueOutside);
    ++failures;
  }
  if (failures) {
    std::printf("GLASS OPTICS: %d assertion(s) failed\n", failures);
    return 1;
  }
  std::puts("GLASS OPTICS CHECKS PASSED");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : ".";
  // The ctest argument is $ENV{TEMP}/prism_snapshots, which does not exist on
  // a clean runner. Without this the encode step fell through to "SKIP:
  // encoder setup" and the artifact simply never appeared -- evidence that
  // silently is not evidence. The optics section fails hard if its PNG cannot
  // be written, so the directory has to be real.
  std::error_code dirError;
  std::filesystem::create_directories(outDir, dirError);
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::puts("SKIP: COM init failed");
    return 0;
  }

  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("SKIP: WIC unavailable");
    return 0;
  }

  constexpr UINT32 kW = 1600, kH = 1000;
  constexpr D2D1_PIXEL_FORMAT kPix = {DXGI_FORMAT_B8G8R8A8_UNORM,
                                      D2D1_ALPHA_MODE_PREMULTIPLIED};

  const auto renderScene = [&](const GalleryRenderer::Scene& scene, const wchar_t* fileName) {
    GalleryRenderer renderer;
    if (FAILED(renderer.Initialize())) {
      std::puts("SKIP: renderer init");
      return;
    }
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(wic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnDemand,
                                 bitmap.ReleaseAndGetAddressOf()))) {
      std::puts("SKIP: WIC bitmap");
      return;
    }
    ComPtr<ID2D1RenderTarget> target;
    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, kPix);
    // CreateWicBitmapRenderTarget takes a bare **out (IID_PPV_ARGS would
    // expand into two arguments and break the arity -- lesson from M3).
    if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
            bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
      std::puts("SKIP: WIC render target");
      return;
    }
    if (FAILED(renderer.CreateDeviceResources(target.Get()))) {
      std::puts("SKIP: device resources");
      return;
    }
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    std::printf("  renderer dbg: lookupHits=%d bitmaps=%d draws=%d\n",
                renderer.dbgThumbLookupHits, renderer.dbgThumbBitmaps, renderer.dbgThumbDraws);
    if (FAILED(target->EndDraw())) {
      std::puts("SKIP: EndDraw");
      return;
    }

    wchar_t path[MAX_PATH];
    std::swprintf(path, MAX_PATH, L"%hs\\%s", outDir, fileName);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> bag;
    if (FAILED(wic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
        FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
        FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                  encoder.ReleaseAndGetAddressOf())) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                       bag.ReleaseAndGetAddressOf())) ||
        FAILED(frame->Initialize(bag.Get())) || FAILED(frame->SetSize(kW, kH))) {
      std::puts("SKIP: encoder setup");
      return;
    }
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
    if (FAILED(frame->SetPixelFormat(&format)) ||
        FAILED(frame->WriteSource(bitmap.Get(), nullptr)) || FAILED(frame->Commit()) ||
        FAILED(encoder->Commit())) {
      std::puts("SKIP: encode");
      return;
    }
    std::wprintf(L"SNAPSHOT %s\n", path);
  };

  auto items = MockBerlinTrip();

  // Scene 1: mid-scroll gallery (fluid-sheet spring gliding, HUD compact 45%).
  gallery::ViewportWindow viewport{{.columns = 5, .cellHeight = 220.0},
                                   static_cast<int>(items.size())};
  viewport.SetViewportSize(kW, kH);
  viewport.SetScroll(620.0);  // a living mid-scroll moment
  GalleryRenderer::Scene scene;
  scene.items = &items;
  scene.viewport = &viewport;
  scene.breadcrumb = L"Berlin_Trip_2026.zip  |  39 items  |  5 solid blocks";
  prism::ProgressHud hud;
  hud.OnStart("Extracting Kreuzberg/street_09.png", 0);
  hud.OnProgress(1'400, 3'150, 7'000);  // ~45 % compact capsule
  scene.hud = hud.Render(3'000);
  renderScene(scene, L"prism_gallery.png");

  // Scene 2: Quick Look open on a photo.
  scene.quickLookOpen = true;
  scene.quickLookTitle = L"Kreuzberg/street_07.png";
  scene.quickLookMeta = L"PNG  |  3.3 MB  |  solid block 3/5  |  decoded in 41 ms";
  renderScene(scene, L"prism_quicklook.png");

  // ---------------------------------------------------------------
  // Scene 3: REAL thumbnails -- the end-to-end proof. A genuine solid 7z is
  // packed on the runner, swept in ONE Extract call through the real 7z.dll
  // bridge and WIC decoder, and the harvested BGRA pixels are painted into
  // the tiles by the same renderer the window uses.
  // ---------------------------------------------------------------
  {
    const std::wstring exe = prism::fixture::ExePath();
    const std::wstring dll = prism::fixture::DllPath();
    if (exe.empty() || dll.empty()) {
      std::puts("FAIL: 7z.exe not found beside 7z.dll -- real-thumbnail snapshot cannot run");
      return 1;
    }
    namespace fs = std::filesystem;
    const fs::path work = fs::temp_directory_path() / L"prism_snapshot_solid";
    fs::remove_all(work);
    fs::create_directories(work);
    const std::wstring dir = work.wstring();
    if (!prism::fixture::WritePng(dir + L"\\gate_front.png", 320, 200, 40, 60, 90)) return 1;
    if (!prism::fixture::WritePng(dir + L"\\quadriga.png", 640, 400, 180, 60, 30)) return 1;
    if (!prism::fixture::WritePng(dir + L"\\mural.png", 900, 600, 30, 120, 160)) return 1;
    if (!prism::fixture::WritePng(dir + L"\\street.png", 512, 512, 200, 180, 40)) return 1;
    if (!prism::fixture::PackSolid(exe, dir, L"solid.7z",
                                   {L"gate_front.png", L"quadriga.png", L"mural.png",
                                    L"street.png"})) {
      std::puts("FAIL: solid pack failed");
      return 1;
    }

    prism::win::SevenZipLibrary library;
    if (!library.Load(dll)) {
      std::puts("FAIL: 7z.dll load failed");
      return 1;
    }
    prism::win::ArchiveReader reader(library);
    if (!reader.Open(dir + L"\\solid.7z")) {
      std::puts("FAIL: open failed");
      return 1;
    }
    const auto catalog = reader.ReadCatalog();

    std::vector<uint32_t> indices;
    std::vector<uint64_t> sizes;
    std::vector<ArchiveItem> real;
    for (const auto& item : catalog) {
      if (item.isDir) continue;
      indices.push_back(item.index);
      sizes.push_back(item.size);
      real.push_back(item);
    }
    prism::win::sweep::WicThumbDecoder wic;
    if (!wic.Initialize()) {
      std::puts("FAIL: WIC init failed");
      return 1;
    }
    prism::gallery::ThumbStore store{64 << 20};
    const auto sweep = prism::win::sweep::RunSolidSweep(reader, indices, sizes, wic, store);
    if (sweep.decoded != static_cast<int>(indices.size())) {
      std::printf("FAIL: sweep decoded %d of %zu\n", sweep.decoded, indices.size());
      return 1;
    }

    gallery::ViewportWindow viewport2{{.columns = 4, .cellHeight = 220.0},
                                      static_cast<int>(real.size())};
    viewport2.SetViewportSize(kW, kH);
    GalleryRenderer::Scene sceneReal;
    sceneReal.items = &real;
    sceneReal.viewport = &viewport2;
    sceneReal.thumbs = &store;
    sceneReal.breadcrumb = L"Design_Assets_v4.7z  |  4 items  |  solid block 1/1  |  REAL sweep";
    for (const auto& item : real) {
      std::printf("  catalog[%u] path=%s size=%llu\n", item.index, item.path.c_str(),
                  static_cast<unsigned long long>(item.size));
    }
    std::printf("  store: count=%zu version=%llu\n", store.Count(),
                static_cast<unsigned long long>(store.Version()));
    for (uint32_t i = 0; i < indices.size(); ++i) {
      const auto* t = store.Lookup(indices[i]);
      if (!t) continue;
      size_t nonzero = 0;
      for (uint8_t v : t->pixels) nonzero += v != 0;
      std::printf("  thumb[%u] %ux%u px0=%d,%d,%d,%d nonzero=%zu/%zu\n", indices[i], t->width,
                  t->height, t->pixels[0], t->pixels[1], t->pixels[2], t->pixels[3], nonzero,
                  t->pixels.size());
    }
    renderScene(sceneReal, L"prism_gallery_real.png");

    // Scene 4: the main-window file list mode, straight from the swept
    // catalog plus a couple of synthesized rows for folder/crypto coverage.
    std::vector<prism::list::SortItem> rows;
    rows.push_back({"01_Brand_Guidelines", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({"02_UI_Kit_iOS26", 0, 0, true, false, prism::PreviewKind::Directory});
    for (const auto& item : real) {
      prism::list::SortItem row;
      row.name = item.path;
      row.size = item.size;
      row.packedSize = item.size * 3 / 4;  // plausible LZMA2 ratios for the visual
      row.kind = prism::QuickLookPlanner::KindByName(item.path);
      rows.push_back(row);
    }
    prism::list::SortItem encrypted;
    encrypted.name = "client_w9_encrypted.pdf";
    encrypted.size = 4'200'000;
    encrypted.packedSize = 1'100'000;
    encrypted.encrypted = true;
    encrypted.kind = prism::PreviewKind::Pdf;
    rows.push_back(encrypted);
    prism::list::FolderFirstOrder(rows);

    GalleryRenderer::Scene sceneList;
    sceneList.listMode = true;
    sceneList.listItems = &rows;
    sceneList.listScroll = 0;
    sceneList.selectedRow = 4;
    sceneList.breadcrumb = L"Design_Assets_v4.7z  |  7 items";
    renderScene(sceneList, L"prism_list.png");
  }

  // The Liquid Glass optics, pixel-asserted (see VerifyGlassOptics).
  if (const int optics = VerifyGlassOptics(wic.Get(), outDir); optics != 0) return optics;

  std::puts("ALL SNAPSHOT CHECKS PASSED");
  return 0;
}