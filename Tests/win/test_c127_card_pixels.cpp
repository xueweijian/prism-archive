// PrismArchive -- C127: the info card's FIRST LABEL, measured in PIXELS.
//
// WHY A PIXEL TEST WHEN THE COMMAND LIST ALREADY SAYS "加密"
// The C74 suite asserts the Labels object, and a unit test can pass while the
// window paints something else (R1: tests are not the product). This case uses
// the REAL GalleryWindow + GalleryRenderer path -- Create -> SetItems ->
// ApplyLanguage -> SidebarLayout -> Draw onto a WIC target -- then measures the
// INK WIDTH of the first card label and compares it with the widths the SAME
// DirectWrite format produces for the candidate strings:
//     complete zh-CN table : "加密"      (the shipping fix)
//     stale table (no keys): "ENCRYPTION" (the screenshot's defect)
// A renderer that ignored Command::text and painted an English literal would
// fail the first comparison; a stale install fails the second one, which is the
// mechanism the card documents.
//
// SKIP: no interactive window station -> exit 77 (the suite names SKIP_RETURN_CODE).
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;

#ifndef PRISM_C127_LANG_DIR
#define PRISM_C127_LANG_DIR "Assets\\Lang"
#endif

namespace {

bool g_skip = false;

bool WindowStationUsable() {
  static const bool usable = [] {
    const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (probe == nullptr) return false;
    DestroyWindow(probe);
    return true;
  }();
  return usable;
}

std::string ReadWholeFile(const std::string& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return std::string();
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

// zh-CN with the four card labels removed: byte-for-byte the shape of the
// 2026-11-05 install payload that produced the screenshot. Everything else
// (menu, status values) still resolves in Chinese.
std::string StaleTableDocument() {
  const std::string document = ReadWholeFile(std::string(PRISM_C127_LANG_DIR) + "\\zh-CN.json");
  std::string stale;
  stale.reserve(document.size());
  std::istringstream lines(document);
  std::string line;
  while (std::getline(lines, line)) {
    if (line.find("\"sidebar.card.") != std::string::npos) continue;
    stale += line;
    stale += '\n';
  }
  return stale;
}

bool LoadCompleteTranslator(prism::i18n::Translator* translator) {
  std::string why;
  const std::string dir(PRISM_C127_LANG_DIR);
  if (!translator->LoadCatalogFile(dir + "\\en-US.json", &why)) {
    std::printf("    FAIL cannot load en-US: %s\n", why.c_str());
    return false;
  }
  if (!translator->LoadCatalogFile(dir + "\\zh-CN.json", &why)) {
    std::printf("    FAIL cannot load zh-CN: %s\n", why.c_str());
    return false;
  }
  translator->SetLanguage("zh-CN");
  return true;
}

bool LoadStaleTranslator(prism::i18n::Translator* translator) {
  std::string why;
  const std::string dir(PRISM_C127_LANG_DIR);
  if (!translator->LoadCatalogFile(dir + "\\en-US.json", &why)) return false;
  prism::i18n::Catalog staleCatalog;
  if (!staleCatalog.LoadFromJson(StaleTableDocument(), &why)) {
    std::printf("    FAIL stale table: %s\n", why.c_str());
    return false;
  }
  if (!translator->AddCatalog(std::move(staleCatalog), &why)) return false;
  translator->SetLanguage("zh-CN");
  return true;
}

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;

  UINT32 At(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)];
  }
};

int Channel(UINT32 pixel, int shift) {
  return static_cast<int>((pixel >> shift) & 0xFFu);
}

int Distance(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = Channel(a, shift) - Channel(b, shift);
    sum += d * d;
  }
  return static_cast<int>(std::sqrt(static_cast<double>(sum)));
}

bool Capture(IWICBitmap* bitmap, Frame* frame) {
  UINT width = 0;
  UINT height = 0;
  if (FAILED(bitmap->GetSize(&width, &height))) return false;
  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) return false;
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<std::size_t>(width) * height);
  for (UINT y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<std::size_t>(y) * width,
                data + static_cast<std::size_t>(y) * stride, static_cast<std::size_t>(width) * 4);
  }
  return true;
}

D2D1_RENDER_TARGET_PROPERTIES WicTargetProperties() {
  return D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
}

bool RenderWindow(prism::win::gallery::GalleryWindow& window, IWICImagingFactory* wic,
                  Frame* frame) {
  constexpr UINT32 kWidth = 1280;
  constexpr UINT32 kHeight = 800;
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), WicTargetProperties(),
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;

  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.layout = &window.LayoutForTest();
  scene.sidebar = &window.SidebarLayout();
  scene.sidebarCollapsed = false;
  scene.actions = &window.ActionBarState();
  scene.actionHover = -1;
  scene.actionPressed = -1;
  scene.breadcrumb = L"c127.7z";

  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;
  return Capture(bitmap.Get(), frame);
}

const prism::win::sidebar::Command* FirstCardLabel(
    const prism::win::sidebar::Layout& layout) {
  for (const prism::win::sidebar::Command& command : layout.commands) {
    if (command.kind == prism::win::sidebar::PrimKind::StatusLabel &&
        command.payload == 0) {
      return &command;
    }
  }
  return nullptr;
}

// The ink extent (x only) of a run inside a rectangle, measured against a
// sampled background colour. Returns 0 when the run painted nothing.
int InkWidth(const Frame& frame, const prism::win::sidebar::Box& box, UINT32 background) {
  const int x0 = (std::max)(0, static_cast<int>(box.left));
  const int y0 = (std::max)(0, static_cast<int>(box.top));
  const int x1 = (std::min)(static_cast<int>(frame.width), static_cast<int>(box.right));
  const int y1 = (std::min)(static_cast<int>(frame.height), static_cast<int>(box.bottom));
  int left = -1;
  int right = -1;
  for (int y = y0; y < y1; ++y) {
    for (int x = x0; x < x1; ++x) {
      if (Distance(frame.At(x, y), background) <= 40) continue;
      if (left < 0 || x < left) left = x;
      if (right < 0 || x > right) right = x;
    }
  }
  return left < 0 ? 0 : right - left + 1;
}

// The width the SAME DirectWrite format gives a candidate string, measured the
// same way (draw -> count ink), so font fallback for CJK is included.
int CandidateWidth(IWICImagingFactory* wic, const std::wstring& text) {
  constexpr UINT32 kWidth = 512;
  constexpr UINT32 kHeight = 32;
  prism::win::gallery::GalleryRenderer measure;
  if (FAILED(measure.Initialize())) return 0;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA,
                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(measure.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), WicTargetProperties(),
                                                            target.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  if (FAILED(measure.CreateDeviceResources(target.Get()))) return 0;
  ComPtr<ID2D1SolidColorBrush> brush;
  if (FAILED(target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White),
                                           brush.ReleaseAndGetAddressOf()))) {
    return 0;
  }
  target->BeginDraw();
  target->Clear(D2D1::ColorF(D2D1::ColorF::Black));
  target->DrawText(text.c_str(), static_cast<UINT32>(text.size()),
                   measure.CardLabelFormat(), D2D1::RectF(0.0f, 0.0f, 512.0f, 24.0f),
                   brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  if (FAILED(target->EndDraw())) return 0;
  Frame frame;
  if (!Capture(bitmap.Get(), &frame)) return 0;
  prism::win::sidebar::Box box{0.0f, 0.0f, 512.0f, 24.0f};
  return InkWidth(frame, box, 0xFF000000u);
}

}  // namespace

PRISM_TEST("c127 the FIRST card label's ink is the Chinese word, not ENCRYPTION") {
  if (!WindowStationUsable()) {
    g_skip = true;
    std::printf("SKIP: no interactive window station\n");
    return;
  }
  ComPtr<IWICImagingFactory> wic;
  if (!PRISM_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                              CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                              reinterpret_cast<void**>(
                                                  wic.ReleaseAndGetAddressOf()))))) {
    return;
  }

  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c127 card pixels", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items;
  items.push_back(prism::ArchiveItem{1, "report.txt", 1000, 988, 0, 0, false, false});
  window.SetItems(&items);
  window.SetRootLabel(L"c127.7z");
  prism::sidebar::ArchiveInfo info;
  info.method = "LZMA2";
  info.dictionaryBytes = 64u * 1024u * 1024u;
  info.encryption = prism::sidebar::Encryption::None;
  window.SetArchiveInfo(info);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 800));

  const int widthEnglish = CandidateWidth(wic.Get(), L"ENCRYPTION");
  const int widthChinese = CandidateWidth(wic.Get(), L"\u52A0\u5BC6");                    // 加密
  const int widthLong = CandidateWidth(wic.Get(), L"\u52A0\u5BC6\u72B6\u6001");           // 加密状态

  // ---- 1. complete table: the shipping fix ------------------------------
  prism::i18n::Translator complete;
  if (!PRISM_CHECK(LoadCompleteTranslator(&complete))) return;
  window.ApplyLanguage(&complete);
  const prism::win::sidebar::Command* label = FirstCardLabel(window.SidebarLayout());
  if (!PRISM_CHECK(label != nullptr)) return;
  PRISM_CHECK_EQ(label->text, std::string("\xE5\x8A\xA0\xE5\xAF\x86"));
  Frame zhFrame;
  if (!PRISM_CHECK(RenderWindow(window, wic.Get(), &zhFrame))) return;
  const UINT32 zhBackground = zhFrame.At(static_cast<int>(label->box.left) + 1,
                                         static_cast<int>(label->box.top) + 1);
  const int zhInk = InkWidth(zhFrame, label->box, zhBackground);
  std::printf("  [c127] card label ink: observed=%d zh=%d en=%d long=%d\n", zhInk,
              widthChinese, widthEnglish, widthLong);
  PRISM_CHECK(zhInk > 4);
  if (std::abs(zhInk - widthChinese) > 2) {
    prism::test::ReportFailure(__FILE__, __LINE__,
                               "the painted label width does not match 加密 (it may still be ENCRYPTION)");
  }
  if (std::abs(zhInk - widthEnglish) <= 8) {
    prism::test::ReportFailure(__FILE__, __LINE__,
                               "the painted label is as wide as ENCRYPTION");
  }
  PRISM_CHECK(zhFrame.At(static_cast<int>(label->box.left) + 2,
                         static_cast<int>(label->box.top) + 1) != 0);

  // ---- 2. stale table: the screenshot's mechanism ------------------------
  prism::i18n::Translator stale;
  if (!PRISM_CHECK(LoadStaleTranslator(&stale))) return;
  const std::vector<std::string> missing = prism::i18n::MissingRequiredKeys(stale);
  std::printf("  [c127] stale table reports %zu missing keys\n", missing.size());
  PRISM_CHECK_EQ(missing.size(), std::size_t{4});
  window.ApplyLanguage(&stale);
  const prism::win::sidebar::Command* staleLabel = FirstCardLabel(window.SidebarLayout());
  if (!PRISM_CHECK(staleLabel != nullptr)) return;
  PRISM_CHECK_EQ(staleLabel->text, std::string("ENCRYPTION"));
  Frame staleFrame;
  if (!PRISM_CHECK(RenderWindow(window, wic.Get(), &staleFrame))) return;
  const UINT32 staleBackground = staleFrame.At(static_cast<int>(staleLabel->box.left) + 1,
                                               static_cast<int>(staleLabel->box.top) + 1);
  const int staleInk = InkWidth(staleFrame, staleLabel->box, staleBackground);
  std::printf("  [c127] stale label ink: observed=%d en=%d\n", staleInk, widthEnglish);
  if (std::abs(staleInk - widthEnglish) > 2) {
    prism::test::ReportFailure(__FILE__, __LINE__,
                               "a table without card keys did not paint ENCRYPTION as the screenshot showed");
  }

  ::DestroyWindow(window.Hwnd());
}

int main(int argc, char** argv) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const int code = prism::test::RunAll(argc, argv);
  if (SUCCEEDED(com)) CoUninitialize();
  if (g_skip) return 77;
  if (code == 0) std::puts("ALL C127 CARD-PIXEL CHECKS PASSED");
  return code;
}
