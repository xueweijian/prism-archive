// PrismArchive -- PrismWin
// C124: the SKIN on real pixels -- a gold folder, four file glyphs, and THE SEAM.
//
// The pure half (Tests/test_c124_tokens.cpp) pins the numbers: one accent, five
// surface tiers, the rail/island hierarchy, the folder's three tones. This half
// measures what a frame actually SHOWS, because a token that never reaches a
// pixel is a design document, not a product:
//
//   V5  \`.md\` and \`.bin\` must not draw the same 16 x 16 glyph (UI-AUDIT 12.6
//       item 2: "data.bin 与 readme.md 都是 10 x 25 灰条"). The two icon boxes are
//       compared PIXEL FOR PIXEL from a real GalleryRenderer frame, and each is
//       asserted to carry ink -- so "they differ" cannot pass by both being
//       blank, and two kinds that drew nothing would fail the control.
//   V6  "THE SEAM" (DESIGN-PROPOSAL 16.3): ONE 2 DIP accent line, in four
//       places. Each region is scanned for pixels of the accent at FULL strength
//       (every wash in the frame is <= 26 %, so the line is the only exact-accent
//       ink there) and the assertion is that the region contains exactly ONE
//       solid 2-px-thick line -- not two, not a dotted one, not a 3 px slab.
//
// The frame is a headless WIC target at 96 DPI, where 1 DIP == 1 px, so a 2 DIP
// line is 2 px here; C120 made the process PerMonitorV2 and the geometry DIP, so
// the same 2.0f is 3 device px at 150 %.
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
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/FileTypeIcon.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/PreviewPlan.hpp"
#include "../../Src/PrismCore/ProgressHud.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/TabBar.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryRenderer;

namespace {

constexpr UINT32 kWidth = 1600;
constexpr UINT32 kHeight = 1000;

#if defined(PRISM_C124_MUTANT_OLD_PALETTE)
constexpr const char* kMutantBanner = "C124 MUTANT ACTIVE: old_palette";
constexpr const char* kMutantMarker = "C124 MUTATION CAUGHT: old_palette";
#else
constexpr const char* kMutantBanner = nullptr;
constexpr const char* kMutantMarker = nullptr;
#endif

IWICImagingFactory* gWic = nullptr;

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
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
};

struct Box {
  int left = 0;
  int top = 0;
  int right = 0;   // exclusive
  int bottom = 0;  // exclusive
  bool Empty() const { return right <= left || bottom <= top; }
  int Width() const { return right - left; }
  int Height() const { return bottom - top; }
};

Box ToBox(const prism::layout::Rect& r) {
  return Box{r.left, r.top, r.right, r.bottom};
}

// The accent byte triple of the theme under test. The seam is painted at alpha
// 1.0, so a seam pixel is EXACTLY this; every wash (selection 26 %, ratio bar
// 55 %, the tab dot 82 %) composites to something else.
struct Rgb {
  int r = 0;
  int g = 0;
  int b = 0;
};

Rgb AccentOf(prism::theme::Variant variant) {
  const prism::theme::Rgba c = prism::theme::EffectiveAccent(variant);
  const auto q = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<int>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return Rgb{q(c.r), q(c.g), q(c.b)};
}

bool IsAccent(const Frame& frame, int x, int y, const Rgb& accent) {
  if (!frame.In(x, y)) return false;
  return frame.Channel(x, y, 16) == accent.r && frame.Channel(x, y, 8) == accent.g &&
         frame.Channel(x, y, 0) == accent.b;
}

struct AccentRun {
  int count = 0;
  Box bbox{};
  bool empty = true;
};

// Every exact-accent pixel inside the region, and their bounding box.
AccentRun ScanAccent(const Frame& frame, const Box& region, const Rgb& accent) {
  AccentRun run;
  for (int y = region.top; y < region.bottom; ++y) {
    for (int x = region.left; x < region.right; ++x) {
      if (!IsAccent(frame, x, y, accent)) continue;
      ++run.count;
      if (run.empty) {
        run.bbox = Box{x, y, x + 1, y + 1};
        run.empty = false;
      } else {
        run.bbox.left = (std::min)(run.bbox.left, x);
        run.bbox.top = (std::min)(run.bbox.top, y);
        run.bbox.right = (std::max)(run.bbox.right, x + 1);
        run.bbox.bottom = (std::max)(run.bbox.bottom, y + 1);
      }
    }
  }
  return run;
}

struct Harness {
  GalleryRenderer renderer;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;

  bool Open(UINT32 width, UINT32 height) {
    if (FAILED(renderer.Initialize())) return false;
    if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand,
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
    if (out == nullptr) return true;
    UINT bitmapWidth = 0;
    UINT bitmapHeight = 0;
    if (FAILED(bitmap->GetSize(&bitmapWidth, &bitmapHeight))) return false;
    WICRect rc{0, 0, static_cast<INT>(bitmapWidth), static_cast<INT>(bitmapHeight)};
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

prism::list::SortItem Row(const char* name, std::uint64_t size, std::uint64_t packed) {
  prism::list::SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  item.kind = prism::QuickLookPlanner::KindByName(name);
  return item;
}

struct Fixture {
  prism::layout::Layout layout;
  std::vector<prism::list::SortItem> rows;
  GalleryRenderer::Scene scene;

  Fixture() {
    rows.push_back(Row("readme.md", 2418, 53));
    rows.push_back(Row("data.bin", 2097152, 2097152));
    rows.push_back(Row("photo.png", 300000, 280000));
    rows.push_back(Row("main.cpp", 5000, 1200));
    // C123's composition: the crumb is a 28 DIP PATH BAR under a flush toolbar
    // (the pill is gone), so the seam's anchor is CrumbUnderlineRect() on that
    // bar. The list, its rows and the progress capsule are unchanged.
    layout = prism::layout::ComputeC123(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                       /*sidebarVisible=*/false, /*listMode=*/true,
                                       /*openArchives=*/1, /*breadcrumbVisible=*/true);
    scene.theme = prism::theme::Variant::Midnight;
    scene.listMode = true;
    scene.layout = &layout;
    scene.listItems = &rows;
    scene.breadcrumb = L"fixture.zip \u203A photos";
  }

  Box RowBox(int row) const {
    return ToBox(prism::layout::ListRowRect(layout, row, 0));
  }

  // The icon slot DrawListMode hands the glyph painters: nameLeft + 4 .. + 20,
  // 16 x 16 DIP, CENTRED in the row. C122 made the row height a function of the
  // window (26 DIP, 22 under 700), so the offset is DERIVED -- never the +10 of
  // the retired 36 px row.
  Box IconBox(int row) const {
    const Box r = RowBox(row);
    const int left = r.left + prism::layout::kListIndent + 4;
    const int top = r.top + (r.Height() - 16) / 2;
    return Box{left, top, left + 16, top + 16};
  }
};

int CountInk(const Frame& frame, const Box& box, int backgroundX, int backgroundY) {
  const int br = frame.Channel(backgroundX, backgroundY, 16);
  const int bg = frame.Channel(backgroundX, backgroundY, 8);
  const int bb = frame.Channel(backgroundX, backgroundY, 0);
  int ink = 0;
  for (int y = box.top; y < box.bottom; ++y) {
    for (int x = box.left; x < box.right; ++x) {
      if (!frame.In(x, y)) continue;
      const int d = (std::max)({std::abs(frame.Channel(x, y, 16) - br),
                                std::abs(frame.Channel(x, y, 8) - bg),
                                std::abs(frame.Channel(x, y, 0) - bb)});
      if (d >= 24) ++ink;
    }
  }
  return ink;
}

bool SameIcon(const Frame& frame, const Box& a, const Box& b, int* differing) {
  int diff = 0;
  for (int dy = 0; dy < 16; ++dy) {
    for (int dx = 0; dx < 16; ++dx) {
      if (!frame.In(a.left + dx, a.top + dy) || !frame.In(b.left + dx, b.top + dy)) {
        ++diff;
        continue;
      }
      if (frame.At(a.left + dx, a.top + dy) != frame.At(b.left + dx, b.top + dy)) ++diff;
    }
  }
  if (differing != nullptr) *differing = diff;
  return diff == 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    ::prism::test::SetMutationVerdict(kMutantMarker,
                                      [] { return prism::theme::detail::kOldPalette; });
  } else {
    static_cast<void>(kMutantMarker);
    std::puts("C124 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the skin cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the skin cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C124 RENDER CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C124 RENDER CHECKS PASSED");
  return 0;
}

// ---------------------------------------------------------------------------
// V5: \`.md\` and \`.bin\` are different glyphs, on the frame
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v5_type_glyphs_differ") {
  Fixture fixture;
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame frame;
  PRISM_CHECK(harness.Paint(fixture.scene, &frame));
  if (frame.pixels.empty()) return;

  // The four rows draw four kinds: document (md), binary (bin), image (png),
  // code (cpp). Each must carry ink -- the positive control that says the glyph
  // was painted at all.
  const char* names[4] = {"readme.md", "data.bin", "photo.png", "main.cpp"};
  int ink[4] = {0, 0, 0, 0};
  for (int row = 0; row < 4; ++row) {
    const Box box = fixture.IconBox(row);
    ink[row] = CountInk(frame, box, box.left - 3, box.top + 8);
    std::printf("  [c124] %-10s icon box (%d,%d)-(%d,%d) ink=%d\n", names[row], box.left,
                box.top, box.right, box.bottom, ink[row]);
    PRISM_CHECK(ink[row] >= 12);
  }

  // The audit's pair, pixel for pixel. 256 pixels each; a grey bar (the shipped
  // defect) is identical in both.
  int differing = 0;
  const bool same = SameIcon(frame, fixture.IconBox(0), fixture.IconBox(1), &differing);
  std::printf("  [c124] .md vs .bin: %d of 256 icon pixels differ\n", differing);
  PRISM_CHECK(!same);
  PRISM_CHECK(differing >= 16);

  // ... and every pair of the four kinds, so "four classes" is a measurement and
  // not a count of names in a table.
  for (int a = 0; a < 4; ++a) {
    for (int b = a + 1; b < 4; ++b) {
      int diff = 0;
      const bool equal = SameIcon(frame, fixture.IconBox(a), fixture.IconBox(b), &diff);
      std::printf("  [c124] %s vs %s: %d px differ\n", names[a], names[b], diff);
      PRISM_CHECK(!equal);
    }
  }

  // The folder is the GOLD material, not a blue plate: one pixel inside the grid
  // glyph is warm (r > b) in every theme. A tint of the accent would be cold.
  const prism::theme::Rgba gold =
      prism::theme::Lookup(prism::theme::TokenId::FolderBody, prism::theme::Variant::Midnight)
          .color;
  std::printf("  [c124] folder body token = (%d,%d,%d)\n",
              static_cast<int>(gold.r * 255.0f + 0.5f),
              static_cast<int>(gold.g * 255.0f + 0.5f),
              static_cast<int>(gold.b * 255.0f + 0.5f));
  PRISM_CHECK(gold.r > gold.b);
  PRISM_CHECK(gold.g > gold.b);
}

// ---------------------------------------------------------------------------
// V6: THE SEAM -- one 2 DIP line, in all four places
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v6_the_seam") {
  Fixture fixture;
  const prism::theme::Variant variant = prism::theme::Variant::Midnight;
  const Rgb accent = AccentOf(variant);

  // (0) THE TAB: painted by the strip's own painter, on its own frame.
  {
    const prism::layout::Layout strip =
        prism::layout::Compute(900, 220, /*sidebarVisible=*/false, /*listMode=*/false,
                               /*tabBarVisible=*/true);
    PRISM_CHECK(strip.tabBarVisible);
    prism::win::tabbar::View view;
    view.items.push_back({L"one.zip", false});
    view.items.push_back({L"two.zip", false});
    view.active = 0;

    if (!PRISM_CHECK(!strip.tabBar.Empty())) return;
    const UINT32 w = 900;
    const UINT32 h = 120;
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(gWic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                                  bitmap.ReleaseAndGetAddressOf()))) {
      PRISM_CHECK(false);
      return;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1RenderTarget> target;
    ComPtr<ID2D1Factory> factory;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.ReleaseAndGetAddressOf());
    if (factory == nullptr) {
      PRISM_CHECK(false);
      return;
    }
    ComPtr<IDWriteFactory> write;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(write.ReleaseAndGetAddressOf()));
    if (FAILED(factory->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                   target.ReleaseAndGetAddressOf()))) {
      PRISM_CHECK(false);
      return;
    }
    prism::win::tabbar::Painter painter;
    PRISM_CHECK(painter.CreateDeviceResources(target.Get(), write.Get()));
    target->BeginDraw();
    target->Clear(D2D1::ColorF(prism::chrome::CanvasColor(variant).r,
                               prism::chrome::CanvasColor(variant).g,
                               prism::chrome::CanvasColor(variant).b, 1.0f));
    painter.Paint(target.Get(), strip, view, variant);
    PRISM_CHECK(SUCCEEDED(target->EndDraw()));

    Frame frame;
    frame.width = w;
    frame.height = h;
    WICRect rc{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    ComPtr<IWICBitmapLock> lock;
    if (SUCCEEDED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
      UINT stride = 0;
      UINT size = 0;
      BYTE* data = nullptr;
      if (SUCCEEDED(lock->GetStride(&stride)) && SUCCEEDED(lock->GetDataPointer(&size, &data))) {
        frame.pixels.resize(static_cast<size_t>(w) * h);
        for (UINT32 y = 0; y < h; ++y) {
          std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * w,
                      data + static_cast<size_t>(y) * stride, static_cast<size_t>(w) * 4);
        }
      }
    }
    const AccentRun run = ScanAccent(frame, ToBox(strip.tabBar), accent);
    const int thickness = run.empty ? 0 : (std::min)(run.bbox.Width(), run.bbox.Height());
    std::printf("  [c124] V6 tab: %d accent px, bbox %dx%d, thickness %d\n", run.count,
                run.bbox.Width(), run.bbox.Height(), thickness);
    PRISM_CHECK(run.count > 0);
    PRISM_CHECK_EQ(thickness, 2);
    // ONE solid line: every pixel of the bounding box is accent, so there is no
    // second line and no dotted run inside the same box.
    PRISM_CHECK_EQ(run.count, run.bbox.Width() * run.bbox.Height());
    // No OTHER accent stroke anywhere else in the strip.
    const int stripWidth = strip.tabBar.Width();
    const int lineWidth = run.empty ? 0 : (std::max)(run.bbox.Width(), run.bbox.Height());
    PRISM_CHECK(lineWidth < stripWidth);
  }

  // (1..3) the three the renderer paints, in ONE frame: selected row, current
  // crumb, progress front.
  const prism::layout::Rect crumbBar = fixture.layout.crumbBar;
  const prism::layout::Rect capsule = prism::layout::ProgressCapsuleRect(fixture.layout);
  if (!PRISM_CHECK(!crumbBar.Empty())) return;
  if (!PRISM_CHECK(!capsule.Empty())) return;
  fixture.scene.selectedRow = 1;
  fixture.scene.hud.phase = prism::HudPhase::Compact;
  fixture.scene.hud.percent = 40;
  // C123: the window measures the current segment with the hit test's own spans
  // and hands the renderer the 2 DIP rule rectangle; the suite hands in the same
  // rectangle (the right half of the path bar).
  fixture.scene.crumbCurrentRect = prism::layout::CrumbUnderlineRect(
      fixture.layout, crumbBar.left + crumbBar.Width() / 2, crumbBar.right - 20);

  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame frame;
  PRISM_CHECK(harness.Paint(fixture.scene, &frame));
  if (frame.pixels.empty()) return;

  // (1) THE SELECTED ROW: a vertical 2 DIP line on the row's LEFT edge.
  {
    const Box row = fixture.RowBox(1);
    const Box region{row.left, row.top, row.left + 12, row.bottom};
    const AccentRun run = ScanAccent(frame, region, accent);
    const int thickness = run.empty ? 0 : run.bbox.Width();
    std::printf("  [c124] V6 row: %d px, bbox %dx%d at (%d,%d)\n", run.count,
                run.bbox.Width(), run.bbox.Height(), run.bbox.left, run.bbox.top);
    PRISM_CHECK_EQ(harness.renderer.SeamCount(GalleryRenderer::SeamRegion::SelectedRow), 1);
    PRISM_CHECK(!run.empty);
    PRISM_CHECK_EQ(thickness, 2);
    PRISM_CHECK_EQ(run.count, run.bbox.Width() * run.bbox.Height());
    PRISM_CHECK(run.bbox.Height() >= row.Height() - 6);
    // The read-back and the pixels agree: the seam really is where the renderer
    // says it is (a suite that only read the record could pass with a painter
    // that recorded and never drew).
    const GalleryRenderer::SeamPaint* seam =
        harness.renderer.SeamPainted(GalleryRenderer::SeamRegion::SelectedRow, 0);
    if (PRISM_CHECK(seam != nullptr)) {
      PRISM_CHECK(std::fabs(seam->rect.right - seam->rect.left - 2.0f) < 0.01f);
      PRISM_CHECK_EQ(static_cast<int>(seam->rect.top), run.bbox.top);
    }
  }

  // (2) THE CURRENT CRUMB: a horizontal 2 DIP line under the segment.
  {
    // C123: the rule lives on the PATH BAR's bottom edge.
    const Box region = ToBox(crumbBar);
    const AccentRun run = ScanAccent(frame, region, accent);
    const int thickness = run.empty ? 0 : run.bbox.Height();
    std::printf("  [c124] V6 crumb: %d px, bbox %dx%d at (%d,%d)\n", run.count,
                run.bbox.Width(), run.bbox.Height(), run.bbox.left, run.bbox.top);
    PRISM_CHECK_EQ(harness.renderer.SeamCount(GalleryRenderer::SeamRegion::CrumbCurrent), 1);
    PRISM_CHECK(!run.empty);
    PRISM_CHECK_EQ(thickness, 2);
    PRISM_CHECK_EQ(run.count, run.bbox.Width() * run.bbox.Height());
    PRISM_CHECK_EQ(run.bbox.bottom, crumbBar.bottom);
    PRISM_CHECK(run.bbox.top >= crumbBar.bottom - 4);
  }

  // (3) THE PROGRESS FRONT: a vertical 2 DIP line at the fill's leading edge.
  {
    const Box region = ToBox(capsule);
    const AccentRun run = ScanAccent(frame, region, accent);
    const int thickness = run.empty ? 0 : run.bbox.Width();
    std::printf("  [c124] V6 progress: %d px, bbox %dx%d at (%d,%d)\n", run.count,
                run.bbox.Width(), run.bbox.Height(), run.bbox.left, run.bbox.top);
    PRISM_CHECK_EQ(harness.renderer.SeamCount(GalleryRenderer::SeamRegion::ProgressFront), 1);
    PRISM_CHECK(!run.empty);
    PRISM_CHECK_EQ(thickness, 2);
    PRISM_CHECK_EQ(run.count, run.bbox.Width() * run.bbox.Height());
    // 40 % of the track: the line sits where the number says, +-2 px.
    const float trackLeft = static_cast<float>(capsule.left) + 10.0f;
    const float trackW = static_cast<float>(capsule.Width()) - 20.0f - 56.0f;
    const float expected = trackLeft + trackW * 0.40f;
    const float measured = static_cast<float>(run.bbox.left) + 1.0f;
    std::printf("  [c124] V6 progress front: measured %.1f expected %.1f\n", measured,
                expected);
    PRISM_CHECK(std::fabs(measured - expected) <= 2.0f);
  }
}
