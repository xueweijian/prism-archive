// PrismArchive -- PrismWin
// C101: the compression-ratio bar is MUTED -- 4 px tall, at most 56 px wide,
// alpha 0.55, hue from the measured ratio.
//
// THE DEFECT (ui-redesign-plan §2.2, GalleryRenderer.hpp:1709-1730 on the
// shipped branch): trackW = cols.ratioW - 64.0f -- the bar spanned the whole
// 130 px ratio column -- 6 px tall (cy +- 3), filled with the file-kind colour
// at alpha 1.0 (#007AFF for text, #34C759 for everything else). The user's
// words: "刺眼荧光绿厚条".
//
// THE ASSERTIONS, on the REAL GalleryRenderer and on PIXELS (headless WIC
// target, the product's own paint path):
//   1. GEOMETRY, read back through LastRatioBar(): every bar the frame painted
//      is exactly 4 px tall with a 2 px radius and at most 56 px wide, and its
//      fill is percent% of that -- the shipped 6 px / full-column shape fails
//      all three (the mutant arm below re-prints it).
//   2. ALPHA: the fill's alpha, read from the BRUSH after SetColor, is 0.55 and
//      never above 0.6.
//   3. PIXELS: for each band (<=50 % / 51..80 % / >80 %) the bar pixel's HSV
//      saturation is <= 0.45 and its mean luminance is <= 60 levels above the
//      canvas -- and its HUE is still the band's (#34C759 / #FF9F0A / #FF3B30),
//      so the fix is a mute, not a colourless stripe.
//   4. NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the same source compiled with
//      PRISM_C101_MUTANT_LOUD restores 6 px / alpha 1.0 / the full column / the
//      kind colour and MUST print "C101 MUTATION CAUGHT: loud" at the first
//      failing check -- and that arm also writes the BEFORE screenshot.
//
// /WX- is the project's Windows-test convention: SDK headers are not /W4.
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
#include <filesystem>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/ChromeTheme.hpp"
#include "../../Src/PrismCore/ListLayout.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"

using Microsoft::WRL::ComPtr;
using prism::list::ColumnLayout;
using prism::list::SortItem;
using prism::win::gallery::GalleryRenderer;

namespace {

constexpr UINT32 kWidth = 1920;
constexpr UINT32 kHeight = 1080;

// The plan's numbers, asserted independently of the product so a constant that
// drifts in PrismCore cannot move the test with it.
constexpr float kBarHeight = 4.0f;
constexpr float kBarRadius = 2.0f;
constexpr float kBarMaxWidth = 56.0f;
constexpr float kBarAlpha = 0.55f;
constexpr float kBarAlphaCeiling = 0.6f;
constexpr float kSatCeiling = 0.45f;
constexpr float kLiftCeiling = 60.0f;
constexpr float kHueTolerance = 20.0f;

#if defined(PRISM_C101_MUTANT_LOUD)
inline constexpr const char* kMutantBanner = "C101 MUTANT ACTIVE: loud";
inline constexpr const char* kMutantMarker = "C101 MUTATION CAUGHT: loud";
inline constexpr const wchar_t* kShotName = L"c101-ratio-bar-before.png";
#else
inline constexpr const char* kMutantBanner = nullptr;
inline constexpr const char* kMutantMarker = nullptr;
inline constexpr const wchar_t* kShotName = L"c101-ratio-bar.png";
#endif

IWICImagingFactory* gWic = nullptr;
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
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
  float Red(int x, int y) const { return static_cast<float>(Channel(x, y, 16)) / 255.0f; }
  float Green(int x, int y) const { return static_cast<float>(Channel(x, y, 8)) / 255.0f; }
  float Blue(int x, int y) const { return static_cast<float>(Channel(x, y, 0)) / 255.0f; }
};

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

// ------------------------------------------------------------------ fixture
SortItem Row(const char* name, std::uint64_t size, std::uint64_t packed) {
  SortItem item;
  item.name = name;
  item.size = size;
  item.packedSize = packed;
  return item;
}

// A real archive listing. The percentages are the card's three bands; every
// name is a compressible text payload, which is also why the SHIPPED colour was
// the same saturated green on every row (BarColor::Green).
std::vector<SortItem> PayloadRows() {
  std::vector<SortItem> rows;
  SortItem dir;
  dir.name = "payload_text/";
  dir.isDir = true;
  rows.push_back(dir);
  rows.push_back(Row("payload_text/quarterly_report_q4_final.txt", 4'820'331, 1'446'099));   // 30 %
  rows.push_back(Row("payload_text/transcript_2026-11-04_standup.txt", 2'000'000, 400'000));  // 20 %
  rows.push_back(Row("payload_text/product_brief_localization_zh.txt", 1'000'000, 650'000));  // 65 %
  rows.push_back(Row("payload_text/error_log_2026-10-31_2300.txt", 3'301'122, 1'155'392));    // 35 %
  rows.push_back(Row("payload_text/notes_from_the_field_team.txt", 1'000'000, 920'000));      // 92 %
  rows.push_back(Row("payload_text/api_reference_archive_storage.txt", 1'820'333, 1'401'656));// 77 %
  rows.push_back(Row("payload_text/customer_interview_digest.txt", 2'200'000, 1'760'000));    // 80 %
  rows.push_back(Row("payload_text/design_review_backdrop_layer.txt", 1'550'000, 1'426'000)); // 92 %
  rows.push_back(Row("payload_text/screenshot_captions_draft.txt", 660'000, 62'700));         // 9 %
  rows.push_back(Row("payload_text/changelog_before_after.txt", 300'022, 270'019));           // 90 %
  rows.push_back(Row("payload_text/renderer_glyph_audit.md", 120'000, 84'000));               // 70 %
  rows.push_back(Row("payload_text/list_mode_backdrop_report.md", 240'000, 120'000));         // 50 %
  return rows;
}

struct Fixture {
  std::vector<SortItem> rows = PayloadRows();
  prism::layout::Layout layout;
  GalleryRenderer::Scene scene;

  Fixture() {
    layout = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                    /*sidebarVisible=*/false, /*listMode=*/true);
    scene.listMode = true;
    scene.listItems = &rows;
    scene.layout = &layout;  // the SAME geometry the renderer would compute
    scene.breadcrumb = L"payload.7z  |  12 items  |  solid block 3/5";
    scene.status.itemCount = static_cast<int>(rows.size());
    scene.status.ratioView = prism::CompressionRatioView(14'250'000, 9'780'000);
  }
};

bool WritePng(const wchar_t* fileName, const Frame& frame) {
  if (gOutDir == nullptr) return true;  // no artifact requested
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

// The canvas colour the renderer clears to, quantized the way the target does.
float CanvasMean() {
  const prism::theme::Rgba token = prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  return (token.r + token.g + token.b) / 3.0f * 255.0f;
}

float HueDegrees(float r, float g, float b) {
  const float high = (std::max)(r, (std::max)(g, b));
  const float low = (std::min)(r, (std::min)(g, b));
  const float delta = high - low;
  if (delta <= 0.0f) return 0.0f;
  float hue = 0.0f;
  if (high == r) {
    hue = 60.0f * std::fmod((g - b) / delta, 6.0f);
  } else if (high == g) {
    hue = 60.0f * (((b - r) / delta) + 2.0f);
  } else {
    hue = 60.0f * (((r - g) / delta) + 4.0f);
  }
  if (hue < 0.0f) hue += 360.0f;
  return hue;
}

float HueDistance(float a, float b) {
  float d = std::fabs(a - b);
  if (d > 180.0f) d = 360.0f - d;
  return d;
}

float Saturation(float r, float g, float b) {
  const float high = (std::max)(r, (std::max)(g, b));
  const float low = (std::min)(r, (std::min)(g, b));
  if (high <= 0.0f) return 0.0f;
  return (high - low) / high;
}

// One pixel of one band, measured. Sampling is split from asserting on purpose:
// the mutant arm must be able to PRINT every ceiling it broke before the first
// assertion leaves the process (CONTRACTS D27), otherwise "the loud bar is red"
// would be a claim with only one of three numbers behind it.
struct BandSample {
  const char* name = "";
  bool found = false;
  bool inFrame = false;
  uint32_t percent = 0;
  int x = 0;
  int y = 0;
  int red = 0;
  int green = 0;
  int blue = 0;
  float sat = 0.0f;
  float mean = 0.0f;
  float hue = 0.0f;
  float expectedHue = 0.0f;
};

BandSample SampleBand(const Frame& frame, const GalleryRenderer& renderer,
                      prism::list::RatioBand band, const char* name) {
  BandSample sample;
  sample.name = name;
  for (int i = 0; i < renderer.RatioBarsPainted(); ++i) {
    const GalleryRenderer::RatioBarPaint& bar = renderer.RatioBarPainted(i);
    if (prism::list::RatioBandOf(bar.percent) != band) continue;
    sample.found = true;
    sample.percent = bar.percent;
    sample.x = static_cast<int>((bar.fill.left + bar.fill.right) / 2.0f);
    sample.y = static_cast<int>((bar.fill.top + bar.fill.bottom) / 2.0f);
    if (!frame.In(sample.x, sample.y)) return sample;
    sample.inFrame = true;
    sample.red = frame.Channel(sample.x, sample.y, 16);
    sample.green = frame.Channel(sample.x, sample.y, 8);
    sample.blue = frame.Channel(sample.x, sample.y, 0);
    const float r = frame.Red(sample.x, sample.y);
    const float g = frame.Green(sample.x, sample.y);
    const float b = frame.Blue(sample.x, sample.y);
    sample.sat = Saturation(r, g, b);
    sample.mean = (r + g + b) / 3.0f * 255.0f;
    sample.hue = HueDegrees(r, g, b);
    const prism::list::Rgb palette = prism::list::RatioHueOf(band);
    sample.expectedHue = HueDegrees(palette.r, palette.g, palette.b);
    return sample;
  }
  return sample;
}

void PrintBand(const BandSample& sample, float canvasMean) {
  if (!sample.found) {
    std::printf("C101 pixels: NO BAR in band %s\n", sample.name);
    return;
  }
  std::printf(
      "C101 pixels: band=%s percent=%u at (%d,%d) rgb=(%d,%d,%d) S=%.3f (limit %.2f) mean=%.1f "
      "lift=%.1f (limit %.0f) hue=%.1f expected=%.1f\n",
      sample.name, sample.percent, sample.x, sample.y, sample.red, sample.green, sample.blue,
      static_cast<double>(sample.sat), static_cast<double>(kSatCeiling),
      static_cast<double>(sample.mean), static_cast<double>(sample.mean - canvasMean),
      static_cast<double>(kLiftCeiling), static_cast<double>(sample.hue),
      static_cast<double>(sample.expectedHue));
}

}  // namespace

// ---------------------------------------------------------------------------
// 1 + 2: the geometry and the alpha that reached the TARGET
// ---------------------------------------------------------------------------
PRISM_TEST("c101.geometry_and_alpha_reach_the_target") {
  Fixture fixture;
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame frame;
  PRISM_CHECK(harness.Paint(fixture.scene, &frame));
  if (frame.pixels.empty()) return;
  if (!WritePng(kShotName, frame)) std::puts("  WARN: PNG failed");

  const ColumnLayout cols;  // the module default the scene falls back to
  const float expectedWidth =
      (std::min)(kBarMaxWidth, static_cast<float>(cols.ratioW) - 64.0f);
  std::printf("C101 geometry: ratioColumn=%u expectedBarWidth=%.0f\n", cols.ratioW,
              static_cast<double>(expectedWidth));

  int fileRows = 0;
  for (const SortItem& row : fixture.rows) {
    if (!row.isDir && row.size > 0 && row.packedSize > 0 && row.packedSize <= row.size) ++fileRows;
  }
  const int painted = harness.renderer.RatioBarsPainted();
  std::printf("C101 geometry: bars painted=%d (file rows in the window=%d)\n", painted, fileRows);
  PRISM_CHECK(painted > 0);
  PRISM_CHECK(painted <= fileRows);

  const float canvasMean = CanvasMean();

  // ---- MEASUREMENT PASS -- everything printed BEFORE the first assertion, so
  // a mutant arm's log carries every ceiling it broke (see BandSample).
  float worstHeight = 0.0f;
  float worstWidth = 0.0f;
  float worstAlpha = 0.0f;
  float worstFillError = 0.0f;
  for (int i = 0; i < painted; ++i) {
    const GalleryRenderer::RatioBarPaint& bar = harness.renderer.RatioBarPainted(i);
    worstHeight = (std::max)(worstHeight, bar.fill.bottom - bar.fill.top);
    worstWidth = (std::max)(worstWidth, bar.track.right - bar.track.left);
    worstAlpha = (std::max)(worstAlpha, bar.color.a);
    const float expectedFill =
        (bar.track.right - bar.track.left) * static_cast<float>(bar.percent) / 100.0f;
    worstFillError = (std::max)(worstFillError,
                                std::fabs((bar.fill.right - bar.fill.left) - expectedFill));
  }
  std::printf(
      "C101 geometry: worst height=%.2f (limit %.0f) worst width=%.1f (limit %.0f) worst "
      "alpha=%.2f (limit %.2f) worst fill error=%.3f px\n",
      static_cast<double>(worstHeight), static_cast<double>(kBarHeight),
      static_cast<double>(worstWidth), static_cast<double>(kBarMaxWidth),
      static_cast<double>(worstAlpha), static_cast<double>(kBarAlphaCeiling),
      static_cast<double>(worstFillError));
  const prism::list::RatioBand bands[] = {prism::list::RatioBand::Low,
                                          prism::list::RatioBand::Medium,
                                          prism::list::RatioBand::High};
  const char* bandName[] = {"low(<=50%)", "medium(51..80%)", "high(>80%)"};
  BandSample samples[3];
  for (int index = 0; index < 3; ++index) {
    samples[index] = SampleBand(frame, harness.renderer, bands[index], bandName[index]);
    PrintBand(samples[index], canvasMean);
  }

  // ---- ASSERTIONS ----
  for (int i = 0; i < painted; ++i) {
    const GalleryRenderer::RatioBarPaint& bar = harness.renderer.RatioBarPainted(i);
    const float height = bar.fill.bottom - bar.fill.top;
    const float width = bar.track.right - bar.track.left;
    const float fillWidth = bar.fill.right - bar.fill.left;
    PRISM_CHECK_EQ(height, kBarHeight);
    PRISM_CHECK_EQ(bar.radius, kBarRadius);
    PRISM_CHECK(width <= kBarMaxWidth);
    PRISM_CHECK_EQ(width, expectedWidth);
    // The bar does not span the ratio column: 130 px - 16 px of padding.
    PRISM_CHECK(width < static_cast<float>(cols.ratioW) - 16.0f);
    PRISM_CHECK(bar.color.a <= kBarAlphaCeiling);
    PRISM_CHECK_EQ(bar.color.a, kBarAlpha);
    const float expectedFill = width * static_cast<float>(bar.percent) / 100.0f;
    if (std::fabs(fillWidth - expectedFill) > 0.05f) {
      std::printf("C101 fill: percent=%u width=%.2f expected=%.2f\n", bar.percent,
                  static_cast<double>(fillWidth), static_cast<double>(expectedFill));
    }
    PRISM_CHECK(std::fabs(fillWidth - expectedFill) <= 0.05f);
    // The track never reaches the column's right edge either.
    PRISM_CHECK(bar.track.right <= static_cast<float>(kWidth));
  }
  for (int index = 0; index < 3; ++index) {
    PRISM_CHECK(samples[index].found);
    PRISM_CHECK(samples[index].inFrame);
  }
}

// ---------------------------------------------------------------------------
// 3: the PIXELS of all three bands -- saturation, luminance, hue
// ---------------------------------------------------------------------------
PRISM_TEST("c101.bar_pixels_are_quiet_and_still_coloured") {
  Fixture fixture;
  Harness harness;
  PRISM_CHECK(harness.Open(kWidth, kHeight));
  if (harness.bitmap == nullptr) return;
  Frame frame;
  PRISM_CHECK(harness.Paint(fixture.scene, &frame));
  if (frame.pixels.empty()) return;

  const float canvasMean = CanvasMean();
  std::printf("C101 pixels: canvas mean=%.1f ceiling=%.1f\n", static_cast<double>(canvasMean),
              static_cast<double>(canvasMean + kLiftCeiling));

  // One representative per BAND (the threshold is what the colour must follow,
  // so the test asks the band rule which bar to look at, not a literal percent).
  // The sample is the CENTRE of the fill: the capsule is 4 px tall, so that
  // pixel is fully covered and is the colour the user actually sees.
  const prism::list::RatioBand bands[] = {prism::list::RatioBand::Low,
                                          prism::list::RatioBand::Medium,
                                          prism::list::RatioBand::High};
  const char* bandName[] = {"low(<=50%)", "medium(51..80%)", "high(>80%)"};
  for (int index = 0; index < 3; ++index) {
    const BandSample sample = SampleBand(frame, harness.renderer, bands[index], bandName[index]);
    PrintBand(sample, canvasMean);
    PRISM_CHECK(sample.found);
    PRISM_CHECK(sample.inFrame);
    // The bar is really there: it is not the canvas.
    PRISM_CHECK(sample.mean - canvasMean > 6.0f);
    PRISM_CHECK(sample.sat <= kSatCeiling);
    PRISM_CHECK(sample.mean - canvasMean <= kLiftCeiling);
    PRISM_CHECK(HueDistance(sample.hue, sample.expectedHue) <= kHueTolerance);
  }

  // The whole filled row of one bar, not just its centre: the mute is uniform.
  for (int i = 0; i < harness.renderer.RatioBarsPainted(); ++i) {
    const GalleryRenderer::RatioBarPaint& bar = harness.renderer.RatioBarPainted(i);
    if (prism::list::RatioBandOf(bar.percent) != prism::list::RatioBand::Medium) continue;
    const int y = static_cast<int>((bar.fill.top + bar.fill.bottom) / 2.0f);
    const int left = static_cast<int>(bar.fill.left) + 3;
    const int right = static_cast<int>(bar.fill.right) - 3;
    float worst = 0.0f;
    for (int x = left; x <= right; ++x) {
      if (!frame.In(x, y)) continue;
      worst = (std::max)(worst, Saturation(frame.Red(x, y), frame.Green(x, y), frame.Blue(x, y)));
    }
    std::printf("C101 pixels: medium band (percent=%u) worst S over %d px = %.3f\n",
                bar.percent, right - left + 1, static_cast<double>(worst));
    PRISM_CHECK(worst <= kSatCeiling);
    break;
  }
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    // D3/D27: the verdict prints at the FIRST failing check, and only when the
    // injected branch actually ran (the renderer counted the loud bars).
    prism::test::SetMutationVerdict(
        kMutantMarker, []() { return GalleryRenderer::dbgMutantRatioBars > 0; });
  } else {
    std::puts("C101 MUTANT: none (clean build)");
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed -- the ratio bar cannot be verified");
    return 1;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("FAIL: WIC unavailable -- the ratio bar cannot be verified");
    return 1;
  }
  gWic = wic.Get();
  if (argc > 1) {
    std::error_code error;
    std::filesystem::create_directories(argv[1], error);
    gOutDir = argv[1];
  }
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C101 RATIO-BAR CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C101 RATIO-BAR CHECKS PASSED");
  return 0;
}
