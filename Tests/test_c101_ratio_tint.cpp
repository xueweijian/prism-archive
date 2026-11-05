// PrismArchive -- PrismCore
// C101: the compression-ratio bar's BANDS and its MUTE.
//
// THE DEFECT (ui-redesign-plan §2.2, measured on the shipped build at
// GalleryRenderer.hpp:1709-1730): the bar was trackW = cols.ratioW - 64.0f
// (the whole 130 px ratio column), a 6 px tall capsule, filled with the
// file-kind colour at alpha 1.0. The user's words: "刺眼荧光绿厚条".
//
// THE FIX this suite pins, in pure numbers so the Linux lane runs it too:
//   * the hue comes from the MEASURED ratio, not from the file kind:
//     <=50 % #34C759, 51..80 % #FF9F0A, >80 % #FF3B30;
//   * the bar is 4 px tall, radius 2, at most 56 px wide, alpha 0.55;
//   * the fill is that hue DESATURATED (the plan's own pixel ceiling is
//     HSV S <= 0.45 and <= 60 levels above the canvas; at alpha 0.55 over the
//     Midnight canvas the raw hues read S 0.70..0.90, i.e. the ceiling is
//     unreachable without the mute -- assertion (4) proves exactly that, and it
//     is what makes a "just fade it" regression red).
//
// Negative control lives on the Windows arm (PRISM_C101_MUTANT_LOUD restores
// 6 px / alpha 1.0 / full column / kind colour); see Tests/suites/c101-ratio-bar.cmake.
#include <cmath>
#include <cstdio>

#include "../Tests/mini_test.hpp"
#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/ListLayout.hpp"

namespace {

// The canvas the renderer clears to, asked of the SAME theme entry the renderer
// asks (C91): a literal here would be a second authority that drifts.
prism::list::Rgb Canvas() {
  const prism::theme::Rgba token = prism::chrome::CanvasColor(prism::theme::Variant::Midnight);
  return {token.r, token.g, token.b};
}

// The 4 px capsule sits on the groove the renderer paints first: kTileBorder,
// white at 12 %.
prism::list::Rgb Groove() {
  const prism::list::Rgb canvas = Canvas();
  return {0.12f + 0.88f * canvas.r, 0.12f + 0.88f * canvas.g, 0.12f + 0.88f * canvas.b};
}

float HueDegrees(const prism::list::Rgb& c) {
  const float high = (std::fmax)(c.r, (std::fmax)(c.g, c.b));
  const float low = (std::fmin)(c.r, (std::fmin)(c.g, c.b));
  const float delta = high - low;
  if (delta <= 0.0f) return 0.0f;
  float hue = 0.0f;
  if (high == c.r) {
    hue = 60.0f * std::fmod((c.g - c.b) / delta, 6.0f);
  } else if (high == c.g) {
    hue = 60.0f * (((c.b - c.r) / delta) + 2.0f);
  } else {
    hue = 60.0f * (((c.r - c.g) / delta) + 4.0f);
  }
  if (hue < 0.0f) hue += 360.0f;
  return hue;
}

float HueDistance(float a, float b) {
  float d = std::fabs(a - b);
  if (d > 180.0f) d = 360.0f - d;
  return d;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1: the threshold decides the band (and the boundaries are the plan's)
// ---------------------------------------------------------------------------
PRISM_TEST("c101.bands_follow_the_measured_ratio") {
  PRISM_CHECK(prism::list::RatioBandOf(0) == prism::list::RatioBand::Low);
  PRISM_CHECK(prism::list::RatioBandOf(1) == prism::list::RatioBand::Low);
  PRISM_CHECK(prism::list::RatioBandOf(50) == prism::list::RatioBand::Low);
  PRISM_CHECK(prism::list::RatioBandOf(51) == prism::list::RatioBand::Medium);
  PRISM_CHECK(prism::list::RatioBandOf(80) == prism::list::RatioBand::Medium);
  PRISM_CHECK(prism::list::RatioBandOf(81) == prism::list::RatioBand::High);
  PRISM_CHECK(prism::list::RatioBandOf(100) == prism::list::RatioBand::High);

  // The plan's three hues, verbatim: #34C759 / #FF9F0A / #FF3B30.
  const prism::list::Rgb low = prism::list::RatioHueOf(prism::list::RatioBand::Low);
  const prism::list::Rgb mid = prism::list::RatioHueOf(prism::list::RatioBand::Medium);
  const prism::list::Rgb high = prism::list::RatioHueOf(prism::list::RatioBand::High);
  PRISM_CHECK(std::fabs(low.r - 0.204f) < 0.002f && std::fabs(low.g - 0.780f) < 0.002f &&
              std::fabs(low.b - 0.349f) < 0.002f);
  PRISM_CHECK(std::fabs(mid.r - 1.0f) < 0.002f && std::fabs(mid.g - 0.624f) < 0.002f &&
              std::fabs(mid.b - 0.039f) < 0.002f);
  PRISM_CHECK(std::fabs(high.r - 1.0f) < 0.002f && std::fabs(high.g - 0.231f) < 0.002f &&
              std::fabs(high.b - 0.188f) < 0.002f);
  std::printf("C101 bands: low=#34C759 mid=#FF9F0A high=#FF3B30\n");
}

// ---------------------------------------------------------------------------
// 2: the plan's geometry and alpha are the model's, not the painter's
// ---------------------------------------------------------------------------
PRISM_TEST("c101.geometry_is_4px_56px_alpha_055") {
  PRISM_CHECK_EQ(prism::list::kRatioBarHeight, 4.0f);
  PRISM_CHECK_EQ(prism::list::kRatioBarRadius, 2.0f);
  PRISM_CHECK_EQ(prism::list::kRatioBarMaxWidth, 56.0f);
  PRISM_CHECK_EQ(prism::list::kRatioBarAlpha, 0.55f);
  // The shipped column is 130 px wide, and the bar may never fill it.
  constexpr float kRatioColumn = 130.0f;
  const float barWidth = (prism::list::kRatioBarMaxWidth < kRatioColumn - 64.0f)
                             ? prism::list::kRatioBarMaxWidth
                             : kRatioColumn - 64.0f;
  PRISM_CHECK_EQ(barWidth, 56.0f);
  PRISM_CHECK(barWidth <= 56.0f);
  PRISM_CHECK(barWidth < kRatioColumn - 16.0f);
  std::printf("C101 geometry: height=%.0f radius=%.0f maxWidth=%.0f alpha=%.2f\n",
              static_cast<double>(prism::list::kRatioBarHeight),
              static_cast<double>(prism::list::kRatioBarRadius),
              static_cast<double>(prism::list::kRatioBarMaxWidth),
              static_cast<double>(prism::list::kRatioBarAlpha));
}

// ---------------------------------------------------------------------------
// 3: every percent the bar can show passes the plan's pixel ceilings
//    (S <= 0.45, <= 60 levels above the canvas) AND keeps its band's hue
// ---------------------------------------------------------------------------
PRISM_TEST("c101.every_band_passes_the_pixel_ceiling") {
  const prism::list::Rgb groove = Groove();
  const float canvasMean = prism::list::RgbMean(Canvas());
  std::printf("C101 canvas: mean=%.1f levels\n", static_cast<double>(canvasMean * 255.0f));
  float worstSat = 0.0f;
  float worstLift = 0.0f;
  for (uint32_t percent = 0; percent <= 100; ++percent) {
    const prism::list::Rgb composite = prism::list::RatioBarComposite(percent, groove);
    const float sat = prism::list::RgbSaturation(composite);
    // Levels of 255, because the plan's ceiling is "<= 60 levels above the
    // canvas" -- the same unit the pixel suite measures.
    const float lift = (prism::list::RgbMean(composite) - canvasMean) * 255.0f;
    if (sat > worstSat) worstSat = sat;
    if (lift > worstLift) worstLift = lift;
    PRISM_CHECK(sat <= 0.45f);
    PRISM_CHECK(lift <= 60.0f);
  }
  std::printf("C101 pixels: worst S=%.3f (limit 0.45) worst lift=%.1f levels (limit 60)\n",
              static_cast<double>(worstSat), static_cast<double>(worstLift));

  // The hue still says WHICH band: the threshold is what the eye reads.
  const uint32_t samples[] = {10, 40, 50, 51, 70, 80, 81, 95, 100};
  for (uint32_t percent : samples) {
    const prism::list::Rgb composite = prism::list::RatioBarComposite(percent, groove);
    const prism::list::Rgb hue = prism::list::RatioHueOf(prism::list::RatioBandOf(percent));
    const float distance = HueDistance(HueDegrees(composite), HueDegrees(hue));
    if (distance > 20.0f) {
      std::printf("C101 hue: percent=%u measured=%.1f expected=%.1f\n", percent,
                  static_cast<double>(HueDegrees(composite)),
                  static_cast<double>(HueDegrees(hue)));
    }
    PRISM_CHECK(distance <= 20.0f);
  }
}

// ---------------------------------------------------------------------------
// 4: THE MUTE IS LOAD-BEARING. Composite the RAW palette at the same alpha and
//    the plan's own ceiling fails -- that is the "just fade it" regression, and
//    this assertion is what makes removing the desaturation go red.
// ---------------------------------------------------------------------------
PRISM_TEST("c101.alpha_alone_would_not_pass_the_ceiling") {
  const prism::list::Rgb groove = Groove();
  const uint32_t bands[] = {10, 65, 95};
  for (uint32_t percent : bands) {
    const prism::list::Rgb hue = prism::list::RatioHueOf(prism::list::RatioBandOf(percent));
    const float a = prism::list::kRatioBarAlpha;
    const prism::list::Rgb raw{a * hue.r + (1.0f - a) * groove.r,
                              a * hue.g + (1.0f - a) * groove.g,
                              a * hue.b + (1.0f - a) * groove.b};
    const float rawSat = prism::list::RgbSaturation(raw);
    const float mutedSat = prism::list::RgbSaturation(prism::list::RatioBarComposite(percent, groove));
    std::printf("C101 mute: percent=%u rawS=%.3f mutedS=%.3f\n", percent,
                static_cast<double>(rawSat), static_cast<double>(mutedSat));
    // The raw hue at 0.55 is still saturated far past the ceiling ...
    PRISM_CHECK(rawSat > 0.45f);
    // ... and the shipped tint is the one that passes it.
    PRISM_CHECK(mutedSat <= 0.45f);
    PRISM_CHECK(mutedSat < rawSat);
  }
}

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C101 RATIO TINT CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C101 RATIO TINT CHECKS PASSED");
  return 0;
}
