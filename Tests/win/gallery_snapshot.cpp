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
#include "UniqueTempDir.hpp"

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

// Read a rendered WIC bitmap back into BGRA words (the sidebar assertions
// below compare frames pixel by pixel; a PNG comparison would need a decoder).
bool ReadWicPixels(IWICBitmap* bitmap, UINT32 width, UINT32 height,
                   std::vector<UINT32>* out) {
  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
    return false;
  }
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  out->resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(out->data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

// Mean per-channel difference of two frames over a rectangle.
double MeanDiffRect(const std::vector<UINT32>& a, const std::vector<UINT32>& b, UINT32 width,
                    int left, int top, int right, int bottom) {
  double sum = 0.0;
  int count = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const size_t i = static_cast<size_t>(y) * width + static_cast<size_t>(x);
      for (int shift = 0; shift <= 16; shift += 8) {
        sum += std::abs(static_cast<int>((a[i] >> shift) & 0xFFu) -
                        static_cast<int>((b[i] >> shift) & 0xFFu));
      }
      ++count;
    }
  }
  return count > 0 ? sum / (count * 3.0) : 0.0;
}

// Pixels that belong to the selection pill (blue-dominant accent over glass).
int CountAccent(const std::vector<UINT32>& pixels, UINT32 width, int left, int top,
                int right, int bottom) {
  int count = 0;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      const UINT32 value = pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
      const int blue = static_cast<int>(value & 0xFFu);
      const int red = static_cast<int>((value >> 16) & 0xFFu);
      if (blue - red >= 40) ++count;
    }
  }
  return count;
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

// ---------------------------------------------------------------------------
// V1 layout pixels (DESIGN.md §2/§5): one catalog, two window widths.
//
//   1. RECTANGLES, not pixels: the five bands never overlap, every fully
//      visible cell sits inside the content, and 1600 px reflows to more
//      columns than 1200 px (the P0-3 acceptance).
//   2. PIXELS: a text item (and an image whose thumbnail never arrived) must
//      NOT show the skeleton highlight -- the P0-2 bug the user photographed.
//      The detector is calibrated on a control render that forces the tile
//      path, so "no skeleton" means the rule held, not that the check is blind
//      (CONTRACTS D4/D18).
//   3. The same run proves the tile path still paints real thumbnails.
// ---------------------------------------------------------------------------
bool ReadBack(IWICBitmap* bitmap, UINT32 width, UINT32 height, std::vector<UINT32>* pixels) {
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(nullptr, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
    return false;
  }
  UINT stride = 0;
  UINT lockW = 0;
  UINT lockH = 0;
  UINT bytes = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetSize(&lockW, &lockH)) ||
      FAILED(lock->GetDataPointer(&bytes, &data))) {
    return false;
  }
  (void)lockW;
  (void)lockH;
  pixels->resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(pixels->data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

bool RenderToPixels(IWICImagingFactory* wic, const GalleryRenderer::Scene& scene, UINT32 width,
                    UINT32 height, std::vector<UINT32>* pixels, int* skeletonCells) {
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand,
                               bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
          bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;
  *skeletonCells = renderer.dbgSkeletonCells;
  return ReadBack(bitmap.Get(), width, height, pixels);
}


bool WritePixelsPng(IWICImagingFactory* wic, const char* outDir, const wchar_t* fileName,
                    UINT32 width, UINT32 height, const std::vector<UINT32>& pixels) {
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%s", outDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmapFromMemory(
          width, height, GUID_WICPixelFormat32bppPBGRA, width * 4,
          static_cast<UINT>(pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(pixels.data())),
          bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  return SUCCEEDED(wic->CreateStream(stream.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(stream->InitializeFromFilename(path, GENERIC_WRITE)) &&
         SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                      encoder.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
         SUCCEEDED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                           bag.ReleaseAndGetAddressOf())) &&
         SUCCEEDED(frame->Initialize(bag.Get())) && SUCCEEDED(frame->SetSize(width, height)) &&
         SUCCEEDED(frame->WriteSource(bitmap.Get(), nullptr)) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(encoder->Commit());
}

// Is this pixel the skeleton highlight (#FF9500) the renderer reserves for a
// tile that has no pixels?
bool IsSkeletonHighlight(UINT32 bgra) {
  const int b = static_cast<int>(bgra & 0xFF);
  const int g = static_cast<int>((bgra >> 8) & 0xFF);
  const int r = static_cast<int>((bgra >> 16) & 0xFF);
  return std::abs(r - 255) <= 6 && std::abs(g - 149) <= 8 && std::abs(b - 0) <= 6;
}

int CountSkeleton(const std::vector<UINT32>& pixels, UINT32 width, const prism::layout::Rect& r) {
  int hits = 0;
  for (int y = (std::max)(0, r.top); y < r.bottom; ++y) {
    for (int x = (std::max)(0, r.left); x < r.right; ++x) {
      const size_t i = static_cast<size_t>(y) * width + static_cast<size_t>(x);
      if (i < pixels.size() && IsSkeletonHighlight(pixels[i])) ++hits;
    }
  }
  return hits;
}

int VerifyLayoutPixels(IWICImagingFactory* wic, const char* outDir) {
  using prism::layout::Cell;
  using prism::layout::CellKind;
  int failures = 0;

  // One catalog: a folder, three images WITH thumbnails, one image whose
  // thumbnail never arrived, and two documents. This is the exact mixture the
  // user's screenshot showed as "everything is a skeleton".
  std::vector<ArchiveItem> items;
  items.push_back(MakeItem("photos/", 0, 0, true));
  items.push_back(MakeItem("photos/gate.png", 4'812'331, 0));
  items.push_back(MakeItem("photos/mural.png", 7'220'464, 0));
  items.push_back(MakeItem("photos/night.png", 3'905'112, 0));
  items.push_back(MakeItem("photos/pending.png", 2'214'908, 0));  // no thumb yet
  items.push_back(MakeItem("notes.txt", 8'912, 0));
  items.push_back(MakeItem("manual.pdf", 214'553, 0));
  // Stable catalog indices: the ThumbStore is keyed by them, exactly like the
  // real sweep (MakeItem leaves the field at its default).
  for (size_t i = 0; i < items.size(); ++i) {
    items[i].index = static_cast<uint32_t>(i);
  }

  prism::gallery::ThumbStore store{8u << 20};
  for (uint32_t i = 1; i <= 3; ++i) {
    prism::gallery::DecodedThumb thumb;
    thumb.width = 8;
    thumb.height = 8;
    thumb.pixels.assign(8u * 8u * 4u, 0xC0);
    thumb.pixels[3] = 0xFF;
    if (!store.OnThumb(i, thumb)) {
      std::puts("FAIL: thumb store rejected a fixture thumbnail");
      return 1;
    }
  }

  const auto makeScene = [&](const prism::layout::Layout& layout, bool forceSkeleton) {
    GalleryRenderer::Scene scene;
    scene.items = &items;
    scene.thumbs = &store;
    scene.layout = &layout;
    scene.debugForceSkeleton = forceSkeleton;
    scene.breadcrumb = L"Layout_Proof.7z  |  7 items";
    scene.selectedItem = -1;
    return scene;
  };

  // ---------------------------------------------------------- 1. rectangles
  const prism::layout::Layout wide = prism::layout::Compute(1600, 1000, false, false);
  const prism::layout::Layout narrow = prism::layout::Compute(1200, 1000, false, false);
  const prism::layout::Rect* bands[] = {&wide.titleBar, &wide.actionIsland, &wide.sidebar,
                                        &wide.content, &wide.statusBar};
  for (int a = 0; a < 5; ++a) {
    for (int b = a + 1; b < 5; ++b) {
      if (bands[a]->Intersects(*bands[b])) {
        std::printf("FAIL: layout bands %d and %d overlap\n", a, b);
        ++failures;
      }
    }
  }
  if (!(wide.columns > narrow.columns)) {
    std::printf("FAIL: 1600 px does not reflow wider than 1200 px (%d vs %d)\n",
                wide.columns, narrow.columns);
    ++failures;
  }
  const std::vector<Cell> wideCells = prism::layout::VisibleCells(wide, 7, 0);
  for (const Cell& cell : wideCells) {
    if (cell.rect.top >= wide.contentInner.top &&
        cell.rect.bottom <= wide.contentInner.bottom &&
        !wide.contentInner.Contains(cell.rect)) {
      std::printf("FAIL: item %d escapes the content area\n", cell.index);
      ++failures;
    }
  }
  std::printf("LAYOUT: 1600px cols=%d tile=%d cell=%d | 1200px cols=%d\n", wide.columns,
              wide.tileWidth, wide.cellHeight, narrow.columns);

  // ------------------------------------------------------------- 2. pixels
  std::vector<UINT32> real;
  int realSkeletons = 0;
  const GalleryRenderer::Scene realScene = makeScene(wide, false);
  if (!RenderToPixels(wic, realScene, 1600, 1000, &real, &realSkeletons)) {
    std::puts("FAIL: layout pixel render failed");
    return 1;
  }
  std::vector<UINT32> control;
  int controlSkeletons = 0;
  const GalleryRenderer::Scene controlScene = makeScene(wide, true);
  if (!RenderToPixels(wic, controlScene, 1600, 1000, &control, &controlSkeletons)) {
    std::puts("FAIL: control pixel render failed");
    return 1;
  }
  if (!WritePixelsPng(wic, outDir, L"prism_v1_layout_1600.png", 1600, 1000, real) ||
      !WritePixelsPng(wic, outDir, L"prism_v1_layout_control.png", 1600, 1000, control)) {
    std::puts("FAIL: layout PNG write");
    return 1;
  }

  const int contentSkeletons = CountSkeleton(real, 1600, wide.contentInner);
  // The text item's cell: P0-2's literal acceptance ("text region shows no
  // skeleton highlight"). Its rect comes from the same layout the renderer used.
  const int textIndex = 5;
  const prism::layout::Rect textCell = prism::layout::TileRect(wide, textIndex, 0);
  const int textSkeletons = CountSkeleton(real, 1600, textCell);
  const int controlTextSkeletons = CountSkeleton(control, 1600, textCell);

  std::printf("LAYOUT PIXELS: realSkeletonCells=%d realContentPx=%d realTextCellPx=%d "
              "controlSkeletonCells=%d controlTextCellPx=%d\n",
              realSkeletons, contentSkeletons, textSkeletons, controlSkeletons,
              controlTextSkeletons);

  // The detector is calibrated: the forced-tile control MUST show the colour.
  if (controlSkeletons <= 0 || controlTextSkeletons <= 0) {
    std::puts("FAIL: skeleton detector is blind -- the control render has no highlight");
    ++failures;
  }
  if (realSkeletons != 0 || contentSkeletons != 0 || textSkeletons != 0) {
    std::printf("FAIL: a skeleton card was painted for an item without a thumbnail "
                "(cells=%d content=%d text=%d)\n", realSkeletons, contentSkeletons,
                textSkeletons);
    ++failures;
  }

  std::puts(failures == 0 ? "LAYOUT PIXELS: OK" : "LAYOUT PIXELS: FAILED");
  return failures == 0 ? 0 : 1;
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

  const auto renderScene = [&](const GalleryRenderer::Scene& scene, const wchar_t* fileName,
                               std::vector<UINT32>* outPixels = nullptr) {
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
    if (outPixels != nullptr && !ReadWicPixels(bitmap.Get(), kW, kH, outPixels)) {
      std::puts("FAIL: the sidebar assertions need the rendered pixels");
      outPixels->clear();
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

  // Design artifact: the gallery now needs REAL thumbnail availability to draw
  // a tile (P0-2). These stand-ins keep the photo tiles visible in the PNG
  // while the .md/.pdf/.csv rows demonstrate the fallback.
  prism::gallery::ThumbStore demoThumbs{16u << 20};
  for (const ArchiveItem& item : items) {
    if (item.isDir || prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) {
      continue;
    }
    prism::gallery::DecodedThumb thumb;
    thumb.width = 8;
    thumb.height = 8;
    thumb.pixels.assign(8u * 8u * 4u, 0x90);
    thumb.pixels[3] = 0xFF;
    demoThumbs.OnThumb(item.index, thumb);
  }

  // Scene 1: mid-scroll gallery (fluid-sheet spring gliding, HUD compact 45%).
  gallery::ViewportWindow viewport{{.columns = 5, .cellHeight = 220.0},
                                   static_cast<int>(items.size())};
  viewport.SetViewportSize(kW, kH);
  viewport.SetScroll(620.0);  // a living mid-scroll moment
  GalleryRenderer::Scene scene;
  scene.items = &items;
  scene.viewport = &viewport;
  scene.thumbs = &demoThumbs;
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
    // Unique per process (card C36, Tests/win/UniqueTempDir.hpp). The OUTPUT
    // directory is shared on purpose (ctest passes $ENV{TEMP}/prism_snapshots and
    // CI uploads it as an artifact); the packed FIXTURE must not be.
    prism::testtmp::UniqueDir scratch(L"c36", L"snapshot_solid");
    if (!scratch.ok()) {
      std::puts("FAIL: cannot create the solid fixture directory");
      return 1;
    }
    const std::wstring dir = scratch.wstr();
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

    // ---------------------------------------------------------------
    // Scene 5 (card C29): the SIDEBAR, painted from C12's command list.
    // Until this card the sidebar had a model and a layout and no pixels; the
    // snapshot is the design-review evidence AND a pixel assertion, so "the
    // command list is consumed" is not a claim but a measurement.
    // ---------------------------------------------------------------
    // The sidebar tree lists FOLDERS (the blueprint's tree never lists files),
    // so the model needs a catalog with real folder rows: focus index 1 and 3
    // below are two different tree rows, which is what the pill assertion needs.
    std::vector<ArchiveItem> sidebarCatalog;
    sidebarCatalog.push_back(MakeItem("Design_Assets/", 0, 0, true));
    sidebarCatalog.push_back(MakeItem("Design_Assets/01_Brand/", 1, 0, true));
    sidebarCatalog.push_back(MakeItem("Design_Assets/02_UI_Kit/", 2, 0, true));
    for (const auto& item : real) sidebarCatalog.push_back(item);
    prism::sidebar::Model model(&sidebarCatalog);
    model.SetArchiveName("Design_Assets_v4.7z");
    model.SetFavorites({
        prism::sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"},
        prism::sidebar::Favorite{"Downloads", "C:/Users/designer/Downloads", "download"},
    });
    model.PushRecent("D:/Work/Design_Assets.7z", 500);
    prism::sidebar::ArchiveInfo archiveInfo;
    archiveInfo.method = "LZMA2";
    archiveInfo.dictionaryBytes = 64ull << 20;
    archiveInfo.encryption = prism::sidebar::Encryption::Aes256;
    model.SetArchiveInfo(archiveInfo);
    model.SetFocus(prism::sidebar::FocusTarget{prism::sidebar::Section::Tree, 1});

    // V1: the rail is a COLUMN inside the content band, exactly like the
    // window builds it (220 px, between the action island and the status bar).
    const prism::layout::Layout sidebarFrame =
        prism::layout::Compute(static_cast<int>(kW), static_cast<int>(kH), true, true);
    prism::win::sidebar::Metrics sidebarMetrics;
    sidebarMetrics.width = GalleryRenderer::kSidebarColumnWidth;
    sidebarMetrics.viewportTop = static_cast<float>(sidebarFrame.content.top);
    sidebarMetrics.viewportBottom = static_cast<float>(sidebarFrame.content.bottom);
    const prism::win::sidebar::Layout sidebarLayout =
        prism::win::sidebar::Build(model, sidebarMetrics);

    GalleryRenderer::Scene sceneSidebar;
    sceneSidebar.listMode = true;
    sceneSidebar.listItems = &rows;
    sceneSidebar.selectedRow = 4;
    sceneSidebar.breadcrumb = L"Design_Assets_v4.7z  |  7 items  |  C29 sidebar";
    sceneSidebar.sidebar = &sidebarLayout;

    std::vector<UINT32> frameSidebar;
    renderScene(sceneSidebar, L"prism_sidebar.png", &frameSidebar);

    GalleryRenderer::Scene sceneNoSidebar = sceneSidebar;
    sceneNoSidebar.sidebar = nullptr;
    std::vector<UINT32> frameNoSidebar;
    renderScene(sceneNoSidebar, L"prism_sidebar_off.png", &frameNoSidebar);

    // Leak check pair: the SAME frame without the list, once with and once
    // without the column. Every pixel right of the column must be identical --
    // that is the "the sidebar does not paint over the content" invariant,
    // measured without the list's own (legitimate) inset shift in the way.
    GalleryRenderer::Scene panelOnly = sceneSidebar;
    panelOnly.listMode = true;  // the sidebar is list-mode chrome
    panelOnly.listItems = nullptr;
    panelOnly.selectedRow = -1;
    std::vector<UINT32> framePanelOnly;
    renderScene(panelOnly, L"prism_sidebar_panel.png", &framePanelOnly);
    GalleryRenderer::Scene blank = panelOnly;
    blank.sidebar = nullptr;
    std::vector<UINT32> frameBlank;
    renderScene(blank, L"prism_sidebar_blank.png", &frameBlank);

    // The focus pill moves to a different tree row: only that row may change.
    GalleryRenderer::Scene sceneOtherFocus = sceneSidebar;
    model.SetFocus(prism::sidebar::FocusTarget{prism::sidebar::Section::Tree, 3});
    const prism::win::sidebar::Layout otherFocusLayout =
        prism::win::sidebar::Build(model, sidebarMetrics);
    sceneOtherFocus.sidebar = &otherFocusLayout;
    std::vector<UINT32> frameOtherFocus;
    renderScene(sceneOtherFocus, L"prism_sidebar_focus.png", &frameOtherFocus);
    model.SetFocus(prism::sidebar::FocusTarget{prism::sidebar::Section::Tree, 1});

    // ... and the fold: same scene, one flag, the list gets a column of width.
    GalleryRenderer::Scene sceneFolded = sceneSidebar;
    sceneFolded.sidebarCollapsed = true;
    std::vector<UINT32> frameFolded;
    renderScene(sceneFolded, L"prism_sidebar_collapsed.png", &frameFolded);

    int sidebarFailures = 0;
    constexpr int kColumnRight = static_cast<int>(GalleryRenderer::kSidebarColumnWidth);
    const int selectedRowTop = 104 + prism::layout::kListHeaderHeight +
                               4 * prism::layout::kListRowHeight;
    const int selectedRowBottom = selectedRowTop + prism::layout::kListRowHeight;
    const auto selectionBox = [](const prism::win::sidebar::Layout& layout,
                                prism::win::sidebar::Box* out) {
      for (const auto& command : layout.commands) {
        if (command.kind != prism::win::sidebar::PrimKind::Selection) continue;
        *out = command.box;
        return true;
      }
      return false;
    };
    prism::win::sidebar::Box pillBox{};
    prism::win::sidebar::Box otherPillBox{};
    const bool focusPillFound = selectionBox(sidebarLayout, &pillBox);
    const bool otherPillFound = selectionBox(otherFocusLayout, &otherPillBox);
    const int pillLeft = static_cast<int>(pillBox.left);
    const int pillRight = static_cast<int>(pillBox.right);
    const int pillTop = static_cast<int>(pillBox.top);
    const int pillBottom = static_cast<int>(pillBox.bottom);
    if (frameSidebar.empty() || frameNoSidebar.empty() || frameOtherFocus.empty() ||
        frameFolded.empty()) {
      std::puts("FAIL: sidebar frames were not rendered");
      ++sidebarFailures;
    } else {
      // 1. The column paints its surface inside the panel box and NOTHING to the
      //    right of it (the list is untouched by the sidebar's presence).
      const double columnDiff =
          MeanDiffRect(frameSidebar, frameNoSidebar, kW, 4, 90, kColumnRight - 4, 700);
      const double leakDiff =
          MeanDiffRect(framePanelOnly, frameBlank, kW, kColumnRight + 2, 90, 1500, 700);
      int differingPixels = 0;
      for (int y = 90; y < 700; ++y) {
        for (int x = 4; x < kColumnRight - 4; ++x) {
          const size_t i = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
          if (frameSidebar[i] != frameNoSidebar[i]) ++differingPixels;
        }
      }
      std::printf("SIDEBAR: columnDiff=%.2f leakDiff=%.2f differingPixels=%d focusPill=%d\n",
                  columnDiff, leakDiff, differingPixels, focusPillFound ? 1 : 0);
      if (columnDiff < 8.0) {
        std::printf("FAIL: the sidebar column did not change its region (columnDiff=%.2f)\n",
                    columnDiff);
        ++sidebarFailures;
      }
      if (differingPixels < 4000) {
        std::printf("FAIL: only %d pixels changed inside the column\n", differingPixels);
        ++sidebarFailures;
      }
      if (leakDiff > 0.001) {
        std::printf("FAIL: the sidebar painted right of its column (leakDiff=%.2f)\n",
                    leakDiff);
        ++sidebarFailures;
      }

      // 2. The keyboard focus pill is a real colour difference inside its row,
      //    and only there.
      if (!focusPillFound) {
        std::puts("FAIL: the layout emitted no Selection command for the focused row");
        ++sidebarFailures;
      } else {
        const double pillDiff = MeanDiffRect(frameSidebar, frameOtherFocus, kW, pillLeft,
                                             pillTop, pillRight, pillBottom);
        // Locality: outside BOTH pills (plus a 2 px antialias margin) the two
        // frames must be BIT-identical -- the highlight may not repaint
        // neighbouring rows, the column or the list.
        const auto inner = [](const prism::win::sidebar::Box& box, int x, int y) {
          return x >= static_cast<int>(box.left) - 2 && x <= static_cast<int>(box.right) + 2 &&
                 y >= static_cast<int>(box.top) - 2 && y <= static_cast<int>(box.bottom) + 2;
        };
        int outsideDiff = 0;
        for (int y = 0; y < static_cast<int>(kH); ++y) {
          for (int x = 0; x < static_cast<int>(kW); ++x) {
            if (inner(pillBox, x, y) || inner(otherPillBox, x, y)) continue;
            const size_t i = static_cast<size_t>(y) * kW + static_cast<size_t>(x);
            if (frameSidebar[i] != frameOtherFocus[i]) ++outsideDiff;
          }
        }
        std::printf("SIDEBAR: focusPillDiff=%.2f outsidePillPixels=%d otherPill=%d\n", pillDiff,
                    outsideDiff, otherPillFound ? 1 : 0);
        if (pillDiff < 6.0) {
          std::printf("FAIL: the focused row is not highlighted (diff=%.2f)\n", pillDiff);
          ++sidebarFailures;
        }
        if (outsideDiff != 0) {
          std::printf("FAIL: the highlight is not local (%d pixels changed outside the pill)\n",
                      outsideDiff);
          ++sidebarFailures;
        }
      }

      // 3. Folded: the list starts at the 16 px inset instead of after the
      //    column -- a full 220 px of extra row width, measured in pixels.
      const int accentLeftExpanded =
          CountAccent(frameSidebar, kW, 16, selectedRowTop + 2, kColumnRight + 4,
                      selectedRowBottom - 2);
      const int accentLeftFolded =
          CountAccent(frameFolded, kW, 16, selectedRowTop + 2, kColumnRight + 4,
                      selectedRowBottom - 2);
      const int accentRightExpanded =
          CountAccent(frameSidebar, kW, kColumnRight + 4, selectedRowTop + 2, 1400,
                      selectedRowBottom - 2);
      size_t foldedRightEdge = 0;
      for (int y = selectedRowTop + 2; y < selectedRowBottom - 2; ++y) {
        for (int x = static_cast<int>(foldedRightEdge); x < 1400; ++x) {
          const UINT32 value = frameFolded[static_cast<size_t>(y) * kW + x];
          if (static_cast<int>(value & 0xFFu) - static_cast<int>((value >> 16) & 0xFFu) >= 40) {
            foldedRightEdge = static_cast<size_t>(x);
          }
        }
      }
      (void)foldedRightEdge;
      std::printf("SIDEBAR: accent before column expanded=%d folded=%d rightOfColumn=%d\n",
                  accentLeftExpanded, accentLeftFolded, accentRightExpanded);
      if (accentLeftExpanded != 0) {
        std::printf("FAIL: the list painted inside the sidebar column (%d accent pixels)\n",
                    accentLeftExpanded);
        ++sidebarFailures;
      }
      if (accentLeftFolded < 200) {
        std::printf("FAIL: the folded list did not reclaim the column (%d accent pixels)\n",
                    accentLeftFolded);
        ++sidebarFailures;
      }
      if (accentRightExpanded < 2000) {
        std::printf("FAIL: the expanded selection pill is missing (%d accent pixels)\n",
                    accentRightExpanded);
        ++sidebarFailures;
      }
    }
    if (sidebarFailures != 0) {
      std::printf("FAIL: sidebar pixel assertions: %d failure(s)\n", sidebarFailures);
      return 1;
    }
  }

  // V1 layout: rectangles + skeleton-colour pixels at two window widths.
  if (const int layout = VerifyLayoutPixels(wic.Get(), outDir); layout != 0) return layout;

  // The Liquid Glass optics, pixel-asserted (see VerifyGlassOptics).
  if (const int optics = VerifyGlassOptics(wic.Get(), outDir); optics != 0) return optics;

  std::puts("ALL SNAPSHOT CHECKS PASSED");
  return 0;
}