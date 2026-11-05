// PrismArchive -- PrismWin
// C109: list typography -- tabular numerals, one column baseline, 36 px rows.
//
// The DEFECT this suite is armed against: the size / packed / ratio cells were
// drawn in the proportional UI face, so '6.3 KB' and '11.2 KB' put their
// decimal point on different pixels, and every row was 32 px tall -- a wall of
// numbers with no breathing room.
//
// WHAT IS ASSERTED (all on the real production path):
//   1. ADVANCES. DWrite measures the advance of '1', '8', '0' and '.' using the
//      SAME ListType.hpp format the renderer paints with. Any two must agree
//      within 0.5 px. (On Segoe UI the digits already agree but '.' is 4.19 px
//      narrower -- which is why the fix is a monospaced numeric column.)
//   2. RIGHT EDGES. Three rows whose size strings have 1/2/3 digits are rendered
//      through GalleryRenderer into a headless WIC target; the rightmost ink
//      pixel of each size cell must differ by <= 1 px.
//   3. ROW HEIGHT + HIT TEST. A real GalleryWindow in list mode reports the
//      third row for a click on the third 36 px band (and the row above/below
//      for the boundary pixels).
//   4. NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the same source compiled with
//      PRISM_C109_MUTANT_PROPORTIONAL restores proportional Segoe UI with no
//      tnum. Assertion (1) must go red and the suite must print
//      'C109 MUTATION CAUGHT: proportional' at that first failure.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/ListType.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;
using prism::ArchiveItem;
using prism::list::SortItem;
using prism::win::gallery::GalleryRenderer;

namespace {

constexpr UINT32 kWidth = 1920;
constexpr UINT32 kHeight = 1080;

#if defined(PRISM_C109_MUTANT_PROPORTIONAL)
inline constexpr const char* kMutantBanner = "C109 MUTANT ACTIVE: proportional";
inline constexpr const char* kMutantMarker = "C109 MUTATION CAUGHT: proportional";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
#endif

// Armed on the MEASURED divergence, not on the macro alone: a mutant run that
// dies before measuring advances must stay red without a false CAUGHT (D3).
float gAdvanceSpread = 0.0f;

IWICImagingFactory* gWic = nullptr;
IDWriteFactory* gDwrite = nullptr;
const char* gOutDir = nullptr;

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

int MaxChannelDelta(UINT32 a, UINT32 b) {
  int delta = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    const int d = static_cast<int>((a >> shift) & 0xFFu) - static_cast<int>((b >> shift) & 0xFFu);
    delta = (std::max)(delta, std::abs(d));
  }
  return delta;
}

UINT32 CanvasPixel() {
  const prism::theme::Rgba token =
      prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  const auto q = [](float v) {
    return static_cast<UINT32>((std::min)(255.0f, (std::max)(0.0f, v * 255.0f + 0.5f)));
  };
  return (q(token.a) << 24) | (q(token.r) << 16) | (q(token.g) << 8) | q(token.b);
}

struct Harness {
  GalleryRenderer renderer;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;

  bool Open(UINT32 width, UINT32 height) {
    if (FAILED(renderer.Initialize())) return false;
    if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                  bitmap.ReleaseAndGetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
            bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer.CreateDeviceResources(target.Get()));
  }

  bool Paint(const GalleryRenderer::Scene& scene, Frame* out) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    if (FAILED(target->EndDraw())) return false;
    UINT bitmapWidth = 0;
    UINT bitmapHeight = 0;
    if (FAILED(bitmap->GetSize(&bitmapWidth, &bitmapHeight))) return false;
    WICRect rc{0, 0, static_cast<INT>(bitmapWidth), static_cast<INT>(bitmapHeight)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
      return false;
    }
    out->width = rc.Width;
    out->height = rc.Height;
    out->pixels.resize(static_cast<size_t>(out->width) * out->height);
    for (UINT32 y = 0; y < out->height; ++y) {
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * out->width,
                  data + static_cast<size_t>(y) * stride,
                  static_cast<size_t>(out->width) * 4);
    }
    return true;
  }
};

bool WritePng(const wchar_t* fileName, const Frame& frame) {
  if (gOutDir == nullptr) return true;  // assertions only (the CI path)
  wchar_t path[MAX_PATH];
  std::swprintf(path, MAX_PATH, L"%hs\\%s", gOutDir, fileName);
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> encodeFrame;
  ComPtr<IPropertyBag2> bag;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          frame.width, frame.height, GUID_WICPixelFormat32bppPBGRA, frame.width * 4,
          static_cast<UINT>(frame.pixels.size() * 4),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data())),
          bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromFilename(path, GENERIC_WRITE)) ||
      FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(encodeFrame.ReleaseAndGetAddressOf(),
                                     bag.ReleaseAndGetAddressOf())) ||
      FAILED(encodeFrame->Initialize(bag.Get())) ||
      FAILED(encodeFrame->SetSize(frame.width, frame.height))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  if (FAILED(encodeFrame->SetPixelFormat(&format)) ||
      FAILED(encodeFrame->WriteSource(bitmap.Get(), nullptr)) ||
      FAILED(encodeFrame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  std::printf("  wrote %ls\n", path);
  return true;
}

// ---------------------------------------------------------------- assertion 1
float SingleAdvance(IDWriteTextFormat* format, const wchar_t* text) {
  IDWriteTextLayout* layout = nullptr;
  if (FAILED(prism::win::listtype::CreateNumericLayout(gDwrite, format, text, 1, 200.0f,
                                                       24.0f, &layout))) {
    return -1.0f;
  }
  DWRITE_TEXT_METRICS metrics{};
  layout->GetMetrics(&metrics);
  const float width = metrics.widthIncludingTrailingWhitespace;
  layout->Release();
  return width;
}

PRISM_TEST("c109.advances: 1/8/0/. share one advance within 0.5 px") {
  IDWriteTextFormat* format = nullptr;
  PRISM_CHECK(SUCCEEDED(prism::win::listtype::CreateNumericFormat(gDwrite, &format)));
  if (format == nullptr) return;
  const wchar_t* chars[] = {L"1", L"8", L"0", L"."};
  float widths[4] = {0, 0, 0, 0};
  for (int i = 0; i < 4; ++i) widths[i] = SingleAdvance(format, chars[i]);
  format->Release();

  float minWidth = widths[0];
  float maxWidth = widths[0];
  for (int i = 0; i < 4; ++i) {
    PRISM_CHECK(widths[i] > 0.0f);
    minWidth = (std::min)(minWidth, widths[i]);
    maxWidth = (std::max)(maxWidth, widths[i]);
  }
  gAdvanceSpread = maxWidth - minWidth;
  std::printf("C109 advances: 1=%.3f 8=%.3f 0=%.3f .=%.3f spread=%.3f family=%ls tnum=%d\n",
              widths[0], widths[1], widths[2], widths[3], static_cast<double>(gAdvanceSpread),
              prism::win::listtype::kNumericFamily, prism::win::listtype::kTabularNumbers ? 1 : 0);
  PRISM_CHECK(gAdvanceSpread <= 0.5f);
}

// ---------------------------------------------------------------- fixture
SortItem Row(const char* name, std::uint64_t size, std::uint64_t packed) {
  SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  return item;
}

// Rows 0..2 are the three different-length size strings the card names; the
// rest fill the frame so the screenshot is a real listing, not a toy scene.
std::vector<SortItem> PayloadRows() {
  std::vector<SortItem> rows;
  rows.push_back(Row("quarterly_report_q4_final.txt", 6451, 3001));
  rows.push_back(Row("transcript_2026-11-04_standup.txt", 11469, 5020));
  rows.push_back(Row("product_brief_localization_zh.txt", 129394278, 40211300));
  rows.push_back(Row("error_log_2026-10-31_2300.txt", 3301122, 780221));
  rows.push_back(Row("notes_from_the_field_team.txt", 445120, 99001));
  rows.push_back(Row("api_reference_archive_storage.txt", 1820333, 512000));
  rows.push_back(Row("customer_interview_digest.txt", 2200000, 812345));
  rows.push_back(Row("design_review_backdrop_layer.txt", 1550000, 400000));
  rows.push_back(Row("screenshot_captions_draft.txt", 660000, 120000));
  rows.push_back(Row("changelog_before_after.txt", 300022, 88000));
  rows.push_back(Row("renderer_glyph_audit.md", 120000, 40000));
  rows.push_back(Row("list_mode_backdrop_report.md", 240000, 60000));
  rows.push_back(Row("solid_block_manifest.txt", 88000, 30000));
  rows.push_back(Row("release_notes_2026-11.txt", 91200, 41000));
  rows.push_back(Row("design_token_audit.csv", 1841100, 700400));
  rows.push_back(Row("waveform_cache.bin", 4820331, 900112));
  rows.push_back(Row("screenshot_main.png", 1204224, 1100440));
  rows.push_back(Row("sample_video_clip.mp4", 73400320, 68000200));
  rows.push_back(Row("trace_2026-11-05.json", 204800, 12000));
  SortItem dir;
  dir.name = "payload_text/";
  dir.isDir = true;
  rows.push_back(dir);
  return rows;
}

struct Fixture {
  std::vector<ArchiveItem> catalog;
  std::vector<SortItem> rows;
  prism::sidebar::Model model;
  prism::win::sidebar::Layout rail;
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  Fixture() : rows(PayloadRows()), model(&catalog) {
    const char* paths[] = {"quarterly_report_q4_final.txt",
                           "transcript_2026-11-04_standup.txt",
                           "product_brief_localization_zh.txt"};
    for (std::size_t i = 0; i < 3; ++i) {
      ArchiveItem item;
      item.path = paths[i];
      item.index = static_cast<std::uint32_t>(i);
      item.size = 6451u * static_cast<std::uint32_t>(i + 1);
      item.packSize = 3001u;
      catalog.push_back(item);
    }
    layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                    /*sidebarVisible=*/true, /*listMode=*/true);
    prism::win::sidebar::Metrics metrics;
    metrics.width = static_cast<float>(prism::layout::kSidebarWidth);
    metrics.viewportTop = static_cast<float>(layout.actionIsland.bottom);
    metrics.viewportBottom = static_cast<float>(layout.statusBar.top);
    rail = prism::win::sidebar::Build(model, metrics);

    scene.listMode = true;
    scene.listItems = &rows;
    scene.sidebar = &rail;
    scene.layout = &layout;
    scene.breadcrumb = L"payload.7z  |  20 items  |  solid block 3/5";
    scene.glassPanels = true;
    scene.status.itemCount = static_cast<int>(rows.size());
    scene.status.ratioView = prism::CompressionRatioView(21737891, 4522292);
    scene.backdropRefraction = false;
  }
};

// ---------------------------------------------------------------- assertion 2
int RightInkEdge(const Frame& frame, const prism::layout::Rect& body,
                 const prism::list::ColumnLayout& cols, int row) {
  const int top = body.top + row * prism::layout::kListRowHeight;
  const int left = static_cast<int>(cols.sizeLeft(static_cast<std::uint32_t>(body.right)));
  const int right = static_cast<int>(cols.sizeRight(static_cast<std::uint32_t>(body.right)));
  const UINT32 canvas = CanvasPixel();
  int edge = -1;
  for (int y = top + 2; y < top + prism::layout::kListRowHeight - 2; ++y) {
    for (int x = right - 1; x >= left; --x) {
      if (!frame.In(x, y)) continue;
      if (MaxChannelDelta(frame.At(x, y), canvas) >= 50) {
        edge = (std::max)(edge, x);
        break;
      }
    }
  }
  return edge;
}

PRISM_TEST("c109.right_edges: 1/2/3-digit sizes end within 1 px") {
  Fixture fixture;
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  Frame frame;
  PRISM_CHECK(harness.Paint(fixture.scene, &frame));
  // The screenshot is a rendered artifact of THIS pass, not a mock-up; write it
  // before the assertions so a red still leaves the visual evidence behind.
  if (!frame.pixels.empty()) {
    if (!WritePng(L"c109-list-type.png", frame)) std::puts("  WARN: PNG failed");
  }
  const prism::layout::Rect body = prism::layout::ListBodyRect(fixture.layout);
  const prism::list::ColumnLayout cols;
  const int e0 = RightInkEdge(frame, body, cols, 0);
  const int e1 = RightInkEdge(frame, body, cols, 1);
  const int e2 = RightInkEdge(frame, body, cols, 2);
  std::printf("C109 right edges: row0=%d row1=%d row2=%d spread=%d (size column %u..%u)\n",
              e0, e1, e2, (std::max)({e0, e1, e2}) - (std::min)({e0, e1, e2}),
              cols.sizeLeft(static_cast<std::uint32_t>(body.right)),
              cols.sizeRight(static_cast<std::uint32_t>(body.right)));
  PRISM_CHECK(e0 > 0);
  PRISM_CHECK(e1 > 0);
  PRISM_CHECK(e2 > 0);
  PRISM_CHECK((std::max)({e0, e1, e2}) - (std::min)({e0, e1, e2}) <= 1);
}

// ---------------------------------------------------------------- assertion 3
PRISM_TEST("c109.hit: the third 36 px row is the third item") {
  prism::win::gallery::GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive c109 hit", GetModuleHandleW(nullptr)))) return;
  std::vector<ArchiveItem> items;
  const char* names[] = {"a.txt", "b.txt", "c.txt", "d.txt", "e.txt"};
  for (int i = 0; i < 5; ++i) {
    ArchiveItem item;
    item.path = names[i];
    item.index = static_cast<std::uint32_t>(i);
    item.size = 1024u * static_cast<std::uint32_t>(i + 1);
    item.packSize = 512u;
    items.push_back(item);
  }
  window.SetItems(&items);
  window.SetListMode(true);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1200, 800));
  const prism::layout::Layout layout = window.LayoutForTest();
  const prism::layout::Rect body = prism::layout::ListBodyRect(layout);
  PRISM_CHECK_EQ(prism::layout::kListRowHeight, 36);
  const int x = body.left + 60;
  for (int i = 0; i < 5; ++i) {
    const int top = body.top + i * 36;
    PRISM_CHECK_EQ(window.HitTestForTest(x, top + 1), i);
    PRISM_CHECK_EQ(window.HitTestForTest(x, top + 35), i);
    if (i > 0) PRISM_CHECK_EQ(window.HitTestForTest(x, top - 1), i - 1);
  }
  // The named case, stated the way the card does: third row -> item 2.
  std::printf("C109 hit: click y=%d -> item %d\n", body.top + 2 * 36 + 10,
              window.HitTestForTest(x, body.top + 2 * 36 + 10));
  PRISM_CHECK_EQ(window.HitTestForTest(x, body.top + 2 * 36 + 10), 2);
  PRISM_CHECK_EQ(window.HitTestForTest(x, body.top + 3 * 36 - 1), 2);
  PRISM_CHECK_EQ(window.HitTestForTest(x, body.top + 3 * 36), 3);
  window.Dispatch(WM_DESTROY, 0, 0);
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    prism::test::SetMutationVerdict(kMutantMarker, []() { return gAdvanceSpread > 0.5f; });
  } else {
    std::puts("C109 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the list typography cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the list typography cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(&gDwrite)))) {
    std::puts("FAIL: DWrite unavailable -- the list typography cannot be verified");
    return 1;
  }
  if (argc > 1) {
    std::error_code error;
    std::filesystem::create_directories(argv[1], error);
    gOutDir = argv[1];
  }
  const int failures = prism::test::RunAll(argc, argv);
  gDwrite->Release();
  if (failures != 0) {
    std::puts("C109 LIST-TYPE CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C109 LIST-TYPE CHECKS PASSED");
  return 0;
}
