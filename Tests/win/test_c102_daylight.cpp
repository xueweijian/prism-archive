// PrismArchive -- PrismWin
// C102: DAYLIGHT LAYERED MATERIAL -- asserted on the RENDERER'S OWN PIXELS, and
// photographed from the very frames it measured.
//
// THE MEASURED DEFECT (ui-redesign-plan 2.1). A resting Daylight canvas
// measured (247,249,252) and the floating island (252,252,254): five levels
// apart. The island had no body, so a floating control read as "a plastic pill
// on a washed-out white page" -- only the rim and the shadow were visible.
//
// WHAT THIS FILE DOES
//   1. renders ONE scene (list mode, tab strip, rail, action island,
//      breadcrumb, status segments) through GalleryRenderer::Draw into a WIC
//      target -- the shipping paint path, not a re-implementation of the
//      arithmetic;
//   2. samples the canvas and the action island from the PIXELS and asserts
//      canvas == #F5F5F7 +-2, island - canvas >= 8 levels, night == (16,17,23),
//      high contrast == (0,0,0);
//   3. given an output directory on the command line, writes
//      docs/assets/ui/c102-day.png and c102-night.png from those same bitmaps --
//      the card's screenshots ARE this suite's frames, so a stale screenshot
//      cannot survive a green run. With no argument (the ctest/CI path) it only
//      asserts, so a test run cannot dirty the checkout.
//
// NEGATIVE CONTROL (CONTRACTS D3 / D26 / D27): PRISM_C102_MUTANT_FLAT puts the
// Daylight island alpha back to .58 on EffectiveGlassSurface(); the level
// assertion MUST go red, and the verdict is armed on the injection counter so
// an unrelated failure can never print CAUGHT.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")

namespace {

using Microsoft::WRL::ComPtr;
using prism::theme::TokenId;
using prism::theme::Variant;
using prism::win::gallery::GalleryRenderer;

constexpr UINT32 kW = 1280;
constexpr UINT32 kH = 800;

IWICImagingFactory* gWic = nullptr;
// Where the card's screenshots go. Empty (the CI path, and what ctest runs) =
// assertions only: a test run must not dirty the checkout. Pass a directory on
// the command line to write the two PNGs the card ships.
std::string gOutDir;

#if defined(PRISM_C102_MUTANT_FLAT)
constexpr const char* kMutation = "flat";
#else
constexpr const char* kMutation = nullptr;
#endif

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

int Channel(UINT32 p, int shift) { return static_cast<int>((p >> shift) & 0xFFu); }

struct Pixel {
  int r = 0;
  int g = 0;
  int b = 0;
};

Pixel ToPixel(UINT32 p) { return Pixel{Channel(p, 16), Channel(p, 8), Channel(p, 0)}; }

// The card's metric: the largest single-channel gap, in levels. The pre-fix
// measurement is 5; the floor is 8.
int MaxChannelDiff(const Pixel& a, const Pixel& b) {
  return (std::max)({std::abs(a.r - b.r), std::abs(a.g - b.g), std::abs(a.b - b.b)});
}

bool Near(const char* label, const Pixel& got, const Pixel& want, int tolerance) {
  const bool ok = MaxChannelDiff(got, want) <= tolerance;
  std::printf("  [c102] %-30s got=(%3d,%3d,%3d) want=(%3d,%3d,%3d) +-%d %s\n", label, got.r,
              got.g, got.b, want.r, want.g, want.b, tolerance, ok ? "ok" : "RED");
  return ok;
}

// The most frequent colour of a box: the SURFACE, with glyphs and rims outvoted.
Pixel ModalColor(const Frame& frame, int left, int top, int right, int bottom) {
  std::map<UINT32, int> histogram;
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      if (!frame.In(x, y)) continue;
      ++histogram[frame.At(x, y) & 0x00FFFFFFu];
    }
  }
  UINT32 best = 0;
  int bestCount = -1;
  for (const auto& entry : histogram) {
    if (entry.second > bestCount) {
      bestCount = entry.second;
      best = entry.first;
    }
  }
  return ToPixel(best);
}

// ---------------------------------------------------------------- the scene

struct Rig {
  std::vector<prism::ArchiveItem> catalog;
  std::vector<prism::list::SortItem> rows;
  prism::sidebar::Model model;
  prism::layout::Layout frame;
  prism::win::sidebar::Layout rail;
  prism::win::tabbar::View tabs;
  prism::actionbar::Model actions;

  Rig() : model(&catalog) {
    const auto folder = [this](const char* path, uint32_t index) {
      prism::ArchiveItem item;
      item.index = index;
      item.path = path;
      item.isDir = true;
      catalog.push_back(item);
    };
    const auto file = [this](const char* path, uint64_t size, uint32_t index) {
      prism::ArchiveItem item;
      item.index = index;
      item.path = path;
      item.size = size;
      item.packSize = size * 3 / 4;
      catalog.push_back(item);
    };
    folder("Design_Assets/", 0);
    folder("Design_Assets/01_Brand/", 1);
    folder("Design_Assets/02_UI_Kit/", 2);
    file("Design_Assets/gate_front.png", 4'812'331, 3);
    file("Design_Assets/mural_01.png", 7'220'464, 4);
    file("Design_Assets/quadriga.png", 2'214'908, 5);

    model.SetArchiveName("Design_Assets_v4.7z");
    prism::sidebar::ArchiveInfo info;
    info.method = "LZMA2";
    info.dictionaryBytes = 64ull << 20;
    info.encryption = prism::sidebar::Encryption::Aes256;
    model.SetArchiveInfo(info);
    model.PushRecent("D:/Work/Design_Assets.7z", 500);
    model.SetFocus(prism::sidebar::FocusTarget{prism::sidebar::Section::Tree, 1});

    rows.push_back({"01_Brand_Guidelines", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({"02_UI_Kit_iOS26", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({"gate_front.png", 4'812'331, 3'609'248, false, false,
                    prism::PreviewKind::Image});
    rows.push_back({"mural_01.png", 7'220'464, 5'415'348, false, false,
                    prism::PreviewKind::Image});

    frame = prism::layout::Compute(static_cast<int>(kW), static_cast<int>(kH),
                                   /*sidebarVisible=*/true, /*listMode=*/true,
                                   /*tabBarVisible=*/true);
    prism::win::sidebar::Metrics metrics;
    metrics.width = GalleryRenderer::kSidebarColumnWidth;
    metrics.viewportTop = static_cast<float>(frame.content.top);
    metrics.viewportBottom = static_cast<float>(frame.content.bottom);
    rail = prism::win::sidebar::Build(model, metrics);

    tabs.items = {{L"Design_Assets_v4.7z", false}, {L"Brand_2026.7z", true}};
    tabs.active = 0;
    actions.SetContext(prism::actionbar::Context{true, true, false});
  }

  GalleryRenderer::Scene SceneFor(Variant variant) {
    GalleryRenderer::Scene scene;
    scene.theme = variant;
    scene.listMode = true;
    scene.listItems = &rows;
    scene.selectedRow = 2;
    scene.layout = &frame;
    scene.sidebar = &rail;
    scene.tabs = &tabs;
    scene.actions = &actions;
    scene.breadcrumb = L"Design_Assets_v4.7z";
    // C128: the band carries three NUMBERS (the card's words are not on it).
    scene.status.fileCount = 12;
    scene.status.folderCount = 3;
    scene.status.archiveBytes = 9'780'000;
    // The shipping default: the C77 refraction layer is ON, so the canvas pixel
    // is the presented (veiled) composite -- the number the card quotes.
    scene.backdropRefraction = true;
    // The C77 PANEL language is off: it paints drop shadows over the islands,
    // and this card measures the SURFACE each region resolves to.
    scene.glassPanels = false;
    return scene;
  }
};

bool RenderFrame(Rig& rig, Variant variant, Frame* out) {
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), rig.SceneFor(variant));
  if (FAILED(target->EndDraw())) return false;

  WICRect rc{0, 0, static_cast<INT>(kW), static_cast<INT>(kH)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  out->width = kW;
  out->height = kH;
  out->pixels.resize(static_cast<size_t>(kW) * kH);
  for (UINT32 y = 0; y < kH; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * kW,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(kW) * 4);
  }
  return true;
}

// The card's screenshot: the frame this suite just measured, encoded by WIC.
bool WritePng(const Frame& frame, const wchar_t* name) {
  if (gOutDir.empty()) {
    std::printf("  [c102] no out-dir: screenshot %ls not written (CI path)\n", name);
    return true;
  }
  CreateDirectoryA(gOutDir.c_str(), nullptr);
  const std::wstring path = std::wstring(gOutDir.begin(), gOutDir.end()) + L"/" + name;
  ComPtr<IWICStream> stream;
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
    std::printf("  [c102] screenshot path unusable: %ls\n", path.c_str());
    return false;
  }
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> pngFrame;
  if (FAILED(encoder->CreateNewFrame(pngFrame.ReleaseAndGetAddressOf(), nullptr))) return false;
  if (FAILED(pngFrame->Initialize(nullptr))) return false;
  if (FAILED(pngFrame->SetSize(frame.width, frame.height))) return false;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(pngFrame->SetPixelFormat(&format))) return false;
  const UINT stride = frame.width * 4u;
  std::vector<BYTE> bytes(reinterpret_cast<const BYTE*>(frame.pixels.data()),
                          reinterpret_cast<const BYTE*>(frame.pixels.data()) +
                              static_cast<size_t>(stride) * frame.height);
  if (FAILED(pngFrame->WritePixels(frame.height, stride, stride * frame.height,
                                   bytes.data()))) {
    return false;
  }
  if (FAILED(pngFrame->Commit())) return false;
  if (FAILED(encoder->Commit())) return false;
  std::printf("  [c102] wrote %ls\n", path.c_str());
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C102 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C102 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C102_MUTANT_FLAT)
      return prism::theme::MutantFlatHits() > 0;
#else
      return false;
#endif
    });
  }
  if (argc > 1 && argv[1] != nullptr) gOutDir = argv[1];
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("  [skip] COM could not be initialised");
    std::puts("C102 SKIPPED: no COM");
    return 77;
  }
  IWICImagingFactory* wic = nullptr;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&wic)))) {
    std::puts("  [skip] WIC unavailable");
    std::puts("C102 SKIPPED: no WIC");
    CoUninitialize();
    return 77;
  }
  gWic = wic;
  const int failures = ::prism::test::RunAll(argc, argv);
  wic->Release();
  CoUninitialize();
  if (failures == 0) std::puts("ALL C102 RENDER CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE CANVAS: the pixel a resting Daylight region shows, plus the card's
//    Daylight screenshot (same frame).
// ---------------------------------------------------------------------------
PRISM_TEST("c102 the daylight canvas renders as #F5F5F7") {
  Rig rig;
  Frame day;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Daylight, &day))) return;

  // (1200,500): inside the content band, right of the rail, below the rows --
  // the same empty-canvas sample the C91 suite uses.
  const int canvasX = 1200;
  const int canvasY = 500;
  PRISM_CHECK(rig.frame.contentInner.left < canvasX);
  PRISM_CHECK(rig.frame.contentInner.top < canvasY);
  const prism::layout::Rect lastRow = prism::layout::ListRowRect(rig.frame, 3, 0);
  PRISM_CHECK(lastRow.bottom < canvasY);

  const Pixel canvas = ToPixel(day.At(canvasX, canvasY));
  std::printf("  [c102] daylight canvas @(%d,%d)=(%d,%d,%d)\n", canvasX, canvasY, canvas.r,
              canvas.g, canvas.b);
  PRISM_CHECK(Near("canvas (day)", canvas, Pixel{245, 245, 247}, 2));

  PRISM_CHECK(WritePng(day, L"c102-day.png"));
}

// ---------------------------------------------------------------------------
// 2. THE ISLAND vs THE CANVAS, in rendered pixels. THIS is the assertion the
//    flat mutant kills.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 the rendered island stands at least 8 levels off the canvas") {
  Rig rig;
  Frame day;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Daylight, &day))) return;

  const Pixel canvas = ToPixel(day.At(1200, 500));

  // The action island: --surface-floating-island over the canvas the frame
  // actually cleared to (GalleryRenderer::IslandOf).
  const prism::actionbar::Layout band = prism::actionbar::Compute(rig.frame);
  if (!PRISM_CHECK(!band.island.Empty())) return;
  const Pixel island =
      ModalColor(day, band.island.left + 6, band.island.top + 4, band.island.left + 120,
                 band.island.bottom - 4);
  const int levels = MaxChannelDiff(island, canvas);
  std::printf("  [c102] island=(%d,%d,%d) canvas=(%d,%d,%d) levels=%d (floor 8)\n",
              island.r, island.g, island.b, canvas.r, canvas.g, canvas.b, levels);
  PRISM_CHECK(levels >= 8);

  // The breadcrumb island is the same token through the C77 panel painter; it
  // must read as the same material (rim and shadow included, hence +-3).
  const prism::layout::Rect nav = prism::layout::NavPillRect(rig.frame);
  const Pixel breadcrumb = ModalColor(day, nav.left + 6, nav.top + 4, nav.left + 80,
                                      nav.bottom - 4);
  std::printf("  [c102] breadcrumb=(%d,%d,%d) island=(%d,%d,%d)\n", breadcrumb.r,
              breadcrumb.g, breadcrumb.b, island.r, island.g, island.b);
  PRISM_CHECK(Near("breadcrumb island", breadcrumb, island, 3));
}

// ---------------------------------------------------------------------------
// 3. THE REGRESSION: night and high contrast render exactly as before, and the
//    night screenshot is the same frame.
// ---------------------------------------------------------------------------
PRISM_TEST("c102 night and high contrast render unchanged") {
  Rig rig;
  Frame night;
  Frame hc;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::Midnight, &night))) return;
  if (!PRISM_CHECK(RenderFrame(rig, Variant::HighContrast, &hc))) return;

  const Pixel nightCanvas = ToPixel(night.At(1200, 500));
  const Pixel hcCanvas = ToPixel(hc.At(1200, 500));
  std::printf("  [c102] night canvas=(%d,%d,%d) hc canvas=(%d,%d,%d)\n", nightCanvas.r,
              nightCanvas.g, nightCanvas.b, hcCanvas.r, hcCanvas.g, hcCanvas.b);
  // C121: the C77 veil is the canvas colour in every theme (it used to be the
  // darker --surface-backdrop-reference), so Midnight's resting canvas is the
  // plain composite instead of its veil. C124: that composite is the surface
  // ladder's canvas, #0B0D12 -- the audit's measured #0C0D11 +-1.
  PRISM_CHECK(Near("canvas (night)", nightCanvas, Pixel{11, 13, 18}, 2));
  // High contrast is opaque black: no tolerance is honest here.
  PRISM_CHECK(Near("canvas (hc)", hcCanvas, Pixel{0, 0, 0}, 0));

  PRISM_CHECK(WritePng(night, L"c102-night.png"));
}
