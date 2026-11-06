// PrismArchive -- PrismWin
// C111: THE FOLDER TILE'S ICON -- asserted on the RENDERER'S OWN PIXELS.
//
// THE MEASURED DEFECT. DrawFolderTile painted the folder as TWO solid rounded
// rectangles (a 68 x 46 front plus a 40 x 16 tab), both the same accent, with
// no rim and no specular edge. The user's words: "还是两块圆角矩形，离 Apple 的
// 立体文件夹还有距离".
//
// WHAT THIS FILE DOES
//   1. renders ONE folder tile through GalleryRenderer::Draw into a WIC target
//      -- the shipping paint path, not a re-implementation of its arithmetic;
//   2. measures the icon's pixel silhouette:
//        B1  the front face's aspect ratio is 1.15..1.45 (the old 68 x 46 face
//            was 1.478: the two-rectangle shape this card replaces);
//        B2  the icon's top 2 px average at least 8 luma levels above its
//            centre -- the 1 px specular edge of the new folder;
//        B3  the back tab is darker than the front face (mean luma);
//        B4  the icon's bounding box scales with the cell (two cell widths:
//            bbox width ratio == cell width ratio +-10 %);
//   3. given an output directory on the command line, writes
//      docs/assets/ui/c111-folder-tile.png as a 4x nearest-neighbour blow-up of
//      the tile it just measured. With no argument (the ctest/CI path) it only
//      asserts, so a test run cannot dirty the checkout.
//
// NEGATIVE CONTROLS (CONTRACTS D3 / D26 / D27). PRISM_C111_MUTANT_TWO_RECTS
// restores the pre-C111 two-rectangle glyph (B1 must go red) and
// PRISM_C111_MUTANT_NO_HIGHLIGHT removes both 1 px specular edges (B2 must go
// red). The caught verdict is armed on a counter the injected branch itself
// increments, so an unrelated failure can never print CAUGHT.
#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../mini_test.hpp"

#include "../../Src/PrismWin/GalleryRenderer.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryRenderer;

IWICImagingFactory* gWic = nullptr;
std::string gOutDir;

#if defined(PRISM_C111_MUTANT_TWO_RECTS)
constexpr const char* kMutation = "two-rects";
#elif defined(PRISM_C111_MUTANT_NO_HIGHLIGHT)
constexpr const char* kMutation = "no-highlight";
#else
constexpr const char* kMutation = nullptr;
#endif

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major, premultiplied (opaque canvas)

  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
};

int Channel(UINT32 p, int shift) { return static_cast<int>((p >> shift) & 0xFFu); }
// BGRA: blue is the low byte, red the high byte.
int Luma(UINT32 p) {
  return (Channel(p, 16) + Channel(p, 8) + Channel(p, 0)) / 3;
}
int MaxChannelDiff(UINT32 a, UINT32 b) {
  const int dr = std::abs(Channel(a, 16) - Channel(b, 16));
  const int dg = std::abs(Channel(a, 8) - Channel(b, 8));
  const int db = std::abs(Channel(a, 0) - Channel(b, 0));
  return (std::max)(dr, (std::max)(dg, db));
}

struct Box {
  int left = 0, top = 0, right = -1, bottom = -1;
  bool Valid() const { return right >= left && bottom >= top; }
  int Width() const { return right - left + 1; }
  int Height() const { return bottom - top + 1; }
};

// The silhouette of the folder glyph: every pixel inside the tile's icon area
// that differs from the tile pane's own fill. The pane fill is sampled at the
// top of the tile (the name plate starts 28 px above the cell bottom), so the
// mask never sees text. The scan is inset from the cell edges, which keeps the
// pane's 1 px rim and its rounded corners out of the measurement.
struct IconProfile {
  Box box;
  std::vector<int> rowWidth;
  std::vector<int> rowLumaSum;
};

IconProfile MeasureIcon(const Frame& frame, const prism::layout::Rect& cell) {
  IconProfile out;
  out.rowWidth.assign(frame.height, 0);
  out.rowLumaSum.assign(frame.height, 0);
  const int cellW = cell.Width();
  const int cellH = cell.Height();
  const int left = cell.left + cellW * 12 / 100;
  const int right = cell.right - cellW * 12 / 100;
  const int top = cell.top + 12;
  const int bottom = cell.bottom - 36;
  const UINT32 pane = frame.At((cell.left + cell.right) / 2, cell.top + 12);
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      const UINT32 p = frame.At(x, y);
      if (MaxChannelDiff(p, pane) <= 10) continue;
      ++out.rowWidth[y];
      out.rowLumaSum[y] += Luma(p);
      if (!out.box.Valid()) {
        out.box = Box{x, y, x, y};
      } else {
        out.box.left = (std::min)(out.box.left, x);
        out.box.right = (std::max)(out.box.right, x);
        out.box.top = (std::min)(out.box.top, y);
        out.box.bottom = (std::max)(out.box.bottom, y);
      }
    }
  }
  (void)cellH;
  return out;
}

// Mean luma over the glyph's own pixels in [row0, row1] (inclusive).
double MeanInRows(const IconProfile& icon, int row0, int row1) {
  int pixels = 0;
  long long sum = 0;
  for (int y = (std::max)(row0, icon.box.top); y <= (std::min)(row1, icon.box.bottom); ++y) {
    pixels += icon.rowWidth[static_cast<size_t>(y)];
    sum += icon.rowLumaSum[static_cast<size_t>(y)];
  }
  return pixels > 0 ? static_cast<double>(sum) / pixels : 0.0;
}

bool RenderFrame(UINT32 width, UINT32 height, const prism::layout::Layout& layout,
                 Frame* out) {
  GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
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

  std::vector<prism::ArchiveItem> items(1);
  items[0].path = "Design";
  items[0].isDir = true;
  items[0].index = 0;
  GalleryRenderer::Scene scene;
  scene.items = &items;
  scene.layout = &layout;

  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;

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
  out->width = width;
  out->height = height;
  out->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

prism::layout::Layout LayoutAt(int windowW, int windowH) {
  return prism::layout::Compute(windowW, windowH, /*sidebarVisible=*/false,
                                /*listMode=*/false);
}

// The one folder tile's painted rectangle, from the SAME flow the renderer uses.
prism::layout::Rect FolderCell(const prism::layout::Layout& layout) {
  std::vector<prism::layout::FlowItem> flow(1);
  flow[0].isDir = true;
  const std::vector<prism::layout::FlowCell> cells = prism::layout::FlowCells(layout, flow);
  return cells.empty() ? prism::layout::Rect{} : cells[0].rect;
}

bool WritePng(const std::wstring& path, const Frame& frame) {
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> pngFrame;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          frame.width, frame.height, GUID_WICPixelFormat32bppBGRA, frame.width * 4u,
          static_cast<UINT>(frame.pixels.size() * 4u),
          reinterpret_cast<BYTE*>(const_cast<UINT32*>(frame.pixels.data())),
          bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) ||
      FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(pngFrame.ReleaseAndGetAddressOf(), nullptr)) ||
      FAILED(pngFrame->Initialize(nullptr)) || FAILED(pngFrame->SetSize(frame.width,
                                                                       frame.height))) {
    return false;
  }
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
  return SUCCEEDED(pngFrame->Commit()) && SUCCEEDED(encoder->Commit());
}

// 4x nearest neighbour of one rectangle: the card's screenshot is the tile this
// suite just measured, so a stale PNG cannot survive a green run.
Frame BlowUp(const Frame& src, const prism::layout::Rect& crop) {
  Frame out;
  out.width = static_cast<UINT32>(crop.Width()) * 4u;
  out.height = static_cast<UINT32>(crop.Height()) * 4u;
  out.pixels.resize(static_cast<size_t>(out.width) * out.height);
  for (int y = 0; y < crop.Height(); ++y) {
    for (int x = 0; x < crop.Width(); ++x) {
      const UINT32 p = src.At(crop.left + x, crop.top + y);
      for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 4; ++dx) {
          out.pixels[static_cast<size_t>(y * 4 + dy) * out.width +
                     static_cast<size_t>(x * 4 + dx)] = p;
        }
      }
    }
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C111 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C111 MUTATION CAUGHT: ") + kMutation;
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
#if defined(PRISM_C111_MUTANT_TWO_RECTS)
      return prism::win::gallery::MutantTwoRectsHits() > 0;
#elif defined(PRISM_C111_MUTANT_NO_HIGHLIGHT)
      return prism::win::gallery::MutantNoHighlightHits() > 0;
#else
      return false;
#endif
    });
  }
  if (argc > 1 && argv[1] != nullptr) gOutDir = argv[1];
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("C111 SKIPPED: no COM");
    return 77;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(&gWic)))) {
    std::puts("C111 SKIPPED: no WIC");
    CoUninitialize();
    return 77;
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  gWic->Release();
  gWic = nullptr;
  CoUninitialize();
  if (failures == 0) std::puts("ALL C111 FOLDER-TILE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// B1: the front face is 1.15..1.45 wide per unit tall.
//
// The old glyph's front was 68 x 46 = 1.478 (the two-rectangle shape), and its
// tab added only a 40 x 8 strip above that face. The "icon bounding box" the
// card names is the front face's rectangle -- the tab is a narrower layer, not
// part of the body's silhouette. Both numbers are printed so the report can
// distinguish "the whole silhouette" from "the face".
// ---------------------------------------------------------------------------
PRISM_TEST("c111 B1 the folder's front face is 1.15..1.45 wide per unit tall") {
  const prism::layout::Layout layout = LayoutAt(400, 380);
  const prism::layout::Rect cell = FolderCell(layout);
  PRISM_CHECK(cell.Width() > 0);
  Frame frame;
  PRISM_CHECK(RenderFrame(400, 380, layout, &frame));
  const IconProfile icon = MeasureIcon(frame, cell);
  PRISM_CHECK(icon.box.Valid());
  if (!icon.box.Valid()) return;

  int maxWidth = 0;
  for (int y = icon.box.top; y <= icon.box.bottom; ++y) {
    maxWidth = (std::max)(maxWidth, icon.rowWidth[static_cast<size_t>(y)]);
  }
  // The front face is the run of rows at >= 90 % of the widest row; the tab is
  // ~62 % of it, so it can never be mistaken for the face.
  const int solid = maxWidth * 90 / 100;
  int faceTop = -1;
  int faceBottom = -1;
  for (int y = icon.box.top; y <= icon.box.bottom; ++y) {
    if (icon.rowWidth[static_cast<size_t>(y)] < solid) continue;
    if (faceTop < 0) faceTop = y;
    faceBottom = y;
  }
  PRISM_CHECK(faceTop >= 0);
  if (faceTop < 0) return;
  const int faceHeight = faceBottom - faceTop + 1;
  const double faceRatio = static_cast<double>(maxWidth) / static_cast<double>(faceHeight);
  const double silhouetteRatio =
      static_cast<double>(icon.box.Width()) / static_cast<double>(icon.box.Height());
  std::printf("  [c111] B1 cell=%dx%d box=%dx%d silhouette=%.3f face=%dx%d faceRatio=%.3f\n",
              cell.Width(), cell.Height(), icon.box.Width(), icon.box.Height(),
              silhouetteRatio, maxWidth, faceHeight, faceRatio);

  // The whole silhouette must also read as a folder, not as a tall block.
  PRISM_CHECK(silhouetteRatio >= 1.15 && silhouetteRatio <= 1.45);
  PRISM_CHECK(faceRatio >= 1.15 && faceRatio <= 1.45);
}

// ---------------------------------------------------------------------------
// B2: the icon's top 2 px are a specular edge, >= 8 luma levels above the
// glyph's centre. A flat fill (the old two rectangles) measures ~0 or less:
// the top rows are the anti-aliased edge of the same colour.
// ---------------------------------------------------------------------------
PRISM_TEST("c111 B2 the icon's top 2 px are at least 8 levels brighter than its centre") {
  const prism::layout::Layout layout = LayoutAt(400, 380);
  const prism::layout::Rect cell = FolderCell(layout);
  Frame frame;
  PRISM_CHECK(RenderFrame(400, 380, layout, &frame));
  const IconProfile icon = MeasureIcon(frame, cell);
  PRISM_CHECK(icon.box.Valid());
  if (!icon.box.Valid()) return;

  const double topMean = MeanInRows(icon, icon.box.top, icon.box.top + 1);
  const int centreTop = icon.box.top + icon.box.Height() * 42 / 100;
  const int centreBottom = icon.box.top + icon.box.Height() * 58 / 100;
  const double centreMean = MeanInRows(icon, centreTop, centreBottom);
  const double delta = topMean - centreMean;
  std::printf("  [c111] B2 top2=%.2f centre=%.2f delta=%.2f\n", topMean, centreMean, delta);
  PRISM_CHECK(delta >= 8.0);
}

// ---------------------------------------------------------------------------
// B3: the back tab is darker than the front face. Pixels above the face's top
// row are the tab's visible strip; the face is everything below it.
// ---------------------------------------------------------------------------
PRISM_TEST("c111 B3 the back tab is darker than the front face") {
  const prism::layout::Layout layout = LayoutAt(400, 380);
  const prism::layout::Rect cell = FolderCell(layout);
  Frame frame;
  PRISM_CHECK(RenderFrame(400, 380, layout, &frame));
  const IconProfile icon = MeasureIcon(frame, cell);
  PRISM_CHECK(icon.box.Valid());
  if (!icon.box.Valid()) return;

  int maxWidth = 0;
  for (int y = icon.box.top; y <= icon.box.bottom; ++y) {
    maxWidth = (std::max)(maxWidth, icon.rowWidth[static_cast<size_t>(y)]);
  }
  const int solid = maxWidth * 90 / 100;
  int faceTop = icon.box.bottom;
  for (int y = icon.box.top; y <= icon.box.bottom; ++y) {
    if (icon.rowWidth[static_cast<size_t>(y)] >= solid) {
      faceTop = y;
      break;
    }
  }
  const double backMean = MeanInRows(icon, icon.box.top, faceTop - 1);
  const double faceMean = MeanInRows(icon, faceTop, icon.box.bottom);
  std::printf("  [c111] B3 tabRows=[%d,%d] back=%.2f faceRows=[%d,%d] face=%.2f\n",
              icon.box.top, faceTop - 1, backMean, faceTop, icon.box.bottom, faceMean);
  PRISM_CHECK(backMean + 3.0 <= faceMean);
}

// ---------------------------------------------------------------------------
// B4: the glyph scales with the cell. Two REAL layouts (300 px and 400 px wide
// windows -> 128 px and 178 px tiles): the bbox width ratio must track the cell
// width ratio within +-10 %. A fixed 68 px glyph is off by 28 %.
// ---------------------------------------------------------------------------
PRISM_TEST("c111 B4 the icon bbox scales with the cell width") {
  const prism::layout::Layout narrow = LayoutAt(300, 380);
  const prism::layout::Layout wide = LayoutAt(400, 380);
  const prism::layout::Rect smallCell = FolderCell(narrow);
  const prism::layout::Rect largeCell = FolderCell(wide);
  PRISM_CHECK(smallCell.Width() > 0 && largeCell.Width() > 0);
  Frame smallFrame;
  Frame largeFrame;
  PRISM_CHECK(RenderFrame(300, 380, narrow, &smallFrame));
  PRISM_CHECK(RenderFrame(400, 380, wide, &largeFrame));
  const IconProfile smallIcon = MeasureIcon(smallFrame, smallCell);
  const IconProfile largeIcon = MeasureIcon(largeFrame, largeCell);
  PRISM_CHECK(smallIcon.box.Valid() && largeIcon.box.Valid());
  if (!smallIcon.box.Valid() || !largeIcon.box.Valid()) return;

  const double cellRatio = static_cast<double>(largeCell.Width()) / smallCell.Width();
  const double iconRatio =
      static_cast<double>(largeIcon.box.Width()) / smallIcon.box.Width();
  const double error = std::abs(iconRatio / cellRatio - 1.0);
  std::printf("  [c111] B4 cells=%d/%d ratio=%.4f icon=%d/%d ratio=%.4f error=%.2f%%\n",
              smallCell.Width(), largeCell.Width(), cellRatio, smallIcon.box.Width(),
              largeIcon.box.Width(), iconRatio, error * 100.0);
  PRISM_CHECK(error <= 0.10);
  // ... and the glyph lives INSIDE its cell at both sizes (it is not clipped).
  PRISM_CHECK(smallIcon.box.Width() < smallCell.Width());
  PRISM_CHECK(largeIcon.box.Width() < largeCell.Width());
}

// ---------------------------------------------------------------------------
// The card's screenshot: the SAME Draw above, cropped to the tile and blown up
// 4x. Only written when an output directory is passed, so ctest never dirties
// the checkout.
// ---------------------------------------------------------------------------
PRISM_TEST("c111 screenshot: 4x folder tile from the shipping paint path") {
  if (gOutDir.empty()) {
    std::puts("  [c111] screenshot skipped (no output directory)");
    return;
  }
  constexpr int kW = 240;
  constexpr int kH = 400;
  prism::layout::Layout layout = LayoutAt(kW, kH);
  layout.columns = 1;
  layout.tileWidth = 192;  // the DESIGN.md maximum tile width
  layout.cellHeight = 240;
  const prism::layout::Rect cell = FolderCell(layout);
  Frame frame;
  PRISM_CHECK(RenderFrame(kW, kH, layout, &frame));
  if (!frame.pixels.empty()) {
    const prism::layout::Rect crop{cell.left - 8, cell.top - 8, cell.right + 8,
                                   cell.top + 168};
    const Frame big = BlowUp(frame, crop);
    std::string dir = gOutDir;
    if (!dir.empty() && dir.back() != '\\' && dir.back() != '/') dir += '\\';
    const std::string path = dir + "c111-folder-tile.png";
    const std::wstring wide(path.begin(), path.end());
    PRISM_CHECK(WritePng(wide, big));
    std::printf("  [c111] wrote %s (%ux%u)\n", path.c_str(), big.width, big.height);
  }
}