// CI design snapshot: renders the M3 gallery design to WIC bitmaps (headless,
// D2D software target) and writes PNGs for artifact upload. This is the REAL
// renderer the window uses -- not a mock -- so what ships in the artifact is
// what the app paints.
//
//   gallery_snapshot.exe <out-dir>
//   -> <out-dir>/prism_gallery.png      grid + breadcrumb + progress capsule
//   -> <out-dir>/prism_quicklook.png    same + Quick Look glass card
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdio>
#include <cwchar>
#include <vector>

#include "../../Src/PrismWin/GalleryRenderer.hpp"

#pragma comment(lib, "windowscodecs.lib")

using namespace prism;
using namespace prism::win::gallery;
using Microsoft::WRL::ComPtr;

namespace {

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

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : ".";
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

  std::puts("DONE");
  return 0;
}
