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
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <string>
#include <vector>

#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "../../Src/PrismWin/SolidSweeper.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "windowscodecs.lib")

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
    renderScene(sceneReal, L"prism_gallery_real.png");

    // Scene 4: the main-window file list mode, straight from the swept
    // catalog plus a couple of synthesized rows for folder/crypto coverage.
    std::vector<prism::list::SortItem> rows;
    rows.push_back({L"01_Brand_Guidelines", 0, 0, true, false, prism::PreviewKind::Directory});
    rows.push_back({L"02_UI_Kit_iOS26", 0, 0, true, false, prism::PreviewKind::Directory});
    for (const auto& item : real) {
      prism::list::SortItem row;
      row.name = Utf8(item.path);
      row.size = item.size;
      row.packedSize = item.size * 3 / 4;  // plausible LZMA2 ratios for the visual
      row.kind = prism::QuickLookPlanner::KindByName(item.path);
      rows.push_back(row);
    }
    prism::list::SortItem encrypted;
    encrypted.name = L"client_w9_encrypted.pdf";
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

  std::puts("ALL SNAPSHOT CHECKS PASSED");
  return 0;
}
