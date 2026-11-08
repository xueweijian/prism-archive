// PrismArchive -- PrismCore
// C124: THE SKIN -- one accent, five surfaces, a gold folder, one line.
//
// THE MEASURED DEFECTS (UI-AUDIT 12.2 / 12.5 / 12.6 / 12.7, DESIGN-PROPOSAL
// 16.1 / 16.3):
//   * three blues doing three jobs (selection #0A84FF, tab rule #0A79EB, folder
//     #2480DF) -- "the same 210-214 degrees", so colour could not carry meaning;
//   * ELEVEN surface tones with the hierarchy backwards: the passive rail
//     measured 1.32:1 against the canvas while the action island -- the thing
//     that should read as "in front" -- measured 1.15:1;
//   * a folder that was a tint of the accent, i.e. the fourth blue;
//   * .md and .bin drawn by the SAME glyph, so the file icon said nothing.
//
// WHAT THIS LANE ASSERTS (pure PrismCore: the Linux lane runs it too):
//   V1  ONE accent: no accent literal survives outside ThemeTokens.hpp, and the
//       accent resolves to the same #0A84FF in both design themes;
//   V2  FIVE surfaces: the ladder has five tiers, its PRESENTED Midnight pixels
//       are the card's five hexes, and the steps are dE 4.1 / 4.5 / 5.2 / 5.3
//       apart -- no sixth tone. The tab plate is the card tier, not its own;
//   V3  the hierarchy: rail vs canvas in [1.03, 1.10] (it steps BACK to 1.06)
//       and action island vs canvas >= 1.28 (it comes FORWARD to 1.32);
//   V4  the folder's three tones decrease in relative luminance (highlight >
//       body > shadow) and the body measures ~1.61:1 on white -- a MATERIAL
//       colour, not an accent;
//   V5a the type classifier names four kinds and the four silhouettes differ.
//
// PRISM_C124_MUTANT_OLD_PALETTE is the pre-card palette compiled in (the 11
// tones, Daylight #007AFF vs Midnight #0A84FF, a folder with no gold): V1, V2
// and V4 MUST go red on that arm, and the verdict is armed on the mutation
// proving it is active -- never on "a failure happened somewhere" (D3/D27).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mini_test.hpp"

#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/FileTypeIcon.hpp"
#include "../Src/PrismCore/ThemeTokens.hpp"

#ifndef PRISM_C124_SRC_DIR
#error "c124: PRISM_C124_SRC_DIR must name the directory to scan"
#endif

namespace {

namespace fs = std::filesystem;
namespace ch = prism::chrome;
namespace th = prism::theme;
namespace icons = prism::icons;

using th::Rgba;
using th::SurfaceTier;
using th::TokenId;
using th::Variant;

#if defined(PRISM_C124_MUTANT_OLD_PALETTE)
constexpr const char* kMutantBanner = "C124 MUTANT ACTIVE: old_palette";
constexpr const char* kMutantMarker = "C124 MUTATION CAUGHT: old_palette";
#else
constexpr const char* kMutantBanner = nullptr;
constexpr const char* kMutantMarker = nullptr;
#endif

// ---- the card's numbers, spelled once -------------------------------------
struct Pixel {
  int r = 0;
  int g = 0;
  int b = 0;
};

Pixel ByteOf(const Rgba& c) {
  const auto q = [](float v) {
    const double scaled = static_cast<double>(v) * 255.0 + 0.5;
    return static_cast<int>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
  };
  return Pixel{q(c.r), q(c.g), q(c.b)};
}

// The ladder (DESIGN-PROPOSAL 12.7): five steps, four deltas.
constexpr Pixel kLadder[5] = {{0x0B, 0x0D, 0x12},
                              {0x12, 0x15, 0x1C},
                              {0x1A, 0x1E, 0x26},
                              {0x23, 0x28, 0x33},
                              {0x2E, 0x33, 0x40}};
constexpr double kLadderDeltaE[4] = {4.1, 4.5, 5.2, 5.3};
constexpr double kDeltaETolerance = 0.35;  // the audit printed one decimal

// dE76, the metric the audit reported its 4.1/4.5/5.2/5.3 in.
struct Lab {
  double l = 0.0;
  double a = 0.0;
  double b = 0.0;
};

double SrgbToLinear(double c) {
  return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

Lab ToLab(const Pixel& p) {
  const double r = SrgbToLinear(p.r / 255.0);
  const double g = SrgbToLinear(p.g / 255.0);
  const double b = SrgbToLinear(p.b / 255.0);
  const double x = (0.4124564 * r + 0.3575761 * g + 0.1804375 * b) / 0.95047;
  const double y = 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  const double z = (0.0193339 * r + 0.1191920 * g + 0.9503041 * b) / 1.08883;
  const auto f = [](double t) {
    return t > 0.008856451679 ? std::cbrt(t) : (7.787037037 * t + 16.0 / 116.0);
  };
  const double fx = f(x);
  const double fy = f(y);
  const double fz = f(z);
  return Lab{116.0 * fy - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double DeltaE(const Pixel& a, const Pixel& b) {
  const Lab la = ToLab(a);
  const Lab lb = ToLab(b);
  const double dl = la.l - lb.l;
  const double da = la.a - lb.a;
  const double db = la.b - lb.b;
  return std::sqrt(dl * dl + da * da + db * db);
}

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

std::string Lower(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string StripComments(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '/') {
      while (i < text.size() && text[i] != '\n') ++i;
      out.push_back('\n');
    } else if (text[i] == '/' && i + 1 < text.size() && text[i + 1] == '*') {
      i += 2;
      while (i + 1 < text.size() && !(text[i] == '*' && text[i + 1] == '/')) ++i;
      ++i;
      out.push_back(' ');
    } else {
      out.push_back(text[i]);
    }
  }
  return out;
}

struct Scan {
  std::size_t files = 0;
  bool themeTokensSeen = false;
  bool rendererSeen = false;
  // Accent values found OUTSIDE ThemeTokens.hpp, as hex-digit strings, so the
  // assertion can print WHICH value leaked instead of just "something did".
  std::vector<std::string> accents;
  std::vector<std::string> where;
};

// The accent literal spellings this product has shipped. Each is a VALUE, in
// however it was written at the time; the scan strips comments first so prose
// about the palette is not an offence.
struct Needle {
  const char* text;
  const char* value;
};

constexpr Needle kAccentNeedles[] = {
    {"#0a84ff", "#0A84FF"},
    {"#0a79eb", "#0A79EB"},
    {"#2480df", "#2480DF"},
    {"#3e8bff", "#3E8BFF"},
    {"#0055d4", "#0055D4"},
    {"#0060df", "#0060DF"},
    {"0.039216f", "#0A84FF"},
    {"0.03922f", "#0A84FF"},
    {"0.517647f", "#0A84FF"},
    {"0.51765f", "#0A84FF"},
    {"0.243f", "#3E8BFF"},
    {"0.545f", "#3E8BFF"},
    {"0.478f", "#007AFF"},
    {"0.478431f", "#007AFF"},
};

Scan ScanTree(const fs::path& root) {
  Scan scan;
  std::error_code error;
  if (!fs::exists(root, error) || error) return scan;
  fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied,
                                     error);
  const fs::recursive_directory_iterator end;
  for (; !error && it != end; it.increment(error)) {
    const fs::directory_entry& entry = *it;
    std::error_code entryError;
    if (!entry.is_regular_file(entryError) || entryError) continue;
    const std::string ext = entry.path().extension().string();
    if (ext != ".hpp" && ext != ".h" && ext != ".cpp" && ext != ".cc") continue;
    ++scan.files;
    const std::string name = entry.path().filename().string();
    const bool isTable = name == "ThemeTokens.hpp";
    if (isTable) scan.themeTokensSeen = true;
    if (name == "GalleryRenderer.hpp") scan.rendererSeen = true;
    std::ifstream input(entry.path());
    std::string text((std::istreambuf_iterator<char>(input)),
                     std::istreambuf_iterator<char>());
    if (isTable) continue;  // the table IS the one place an accent is defined
    const std::string lowered = Lower(StripComments(text));
    for (const Needle& needle : kAccentNeedles) {
      if (lowered.find(needle.text) != std::string::npos) {
        scan.accents.push_back(needle.value);
        scan.where.push_back(name + " (" + needle.text + ")");
      }
    }
  }
  return scan;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
    // D3: the verdict is armed on the injected palette actually being live, so
    // a red that came from somewhere else cannot print CAUGHT.
    ::prism::test::SetMutationVerdict(kMutantMarker, [] { return th::detail::kOldPalette; });
  } else {
    static_cast<void>(kMutantMarker);
    std::puts("C124 MUTANT: none (clean build)");
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::puts("C124 TOKEN CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C124 TOKEN CHECKS PASSED");
  return 0;
}

// ---------------------------------------------------------------------------
// V1: ONE ACCENT
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v1_one_accent") {
  // (a) THE SOURCE: no accent literal survives outside the token table. This is
  // the mechanical half -- a call site that paints "the blue" is how three blues
  // happened in the first place.
  const Scan scan = ScanTree(fs::path(PRISM_C124_SRC_DIR));
  std::printf("  [c124] scanned %zu sources under %s\n", scan.files, PRISM_C124_SRC_DIR);
  // Non-vacuity: a wrong root must be RED, never a silent zero-hit.
  PRISM_CHECK(scan.files >= 40);
  PRISM_CHECK(scan.themeTokensSeen);
  PRISM_CHECK(scan.rendererSeen);
  for (std::size_t i = 0; i < scan.accents.size(); ++i) {
    std::printf("  [c124] ACCENT LITERAL %s in %s\n", scan.accents[i].c_str(),
                scan.where[i].c_str());
  }
  // The ONE allowed value outside the table is #0A84FF itself (the renderer's
  // build-time brush default, recoloured every frame by AccentOf). Anything
  // else is a second accent.
  std::vector<std::string> distinct;
  for (const std::string& value : scan.accents) {
    if (std::find(distinct.begin(), distinct.end(), value) == distinct.end()) {
      distinct.push_back(value);
    }
  }
  std::printf("  [c124] distinct accent values outside ThemeTokens.hpp: %zu", distinct.size());
  for (const std::string& value : distinct) std::printf(" %s", value.c_str());
  std::printf("\n");
  PRISM_CHECK_EQ(distinct.size(), std::size_t{1});
  if (distinct.size() == 1) PRISM_CHECK_EQ(distinct[0], std::string("#0A84FF"));

  // (b) THE RUNTIME: the accent resolves to ONE value in both design themes.
  // HighContrast is the documented accessibility override and is not in scope.
  th::live::Reset();
  const Rgba day = th::EffectiveAccent(Variant::Daylight);
  const Rgba night = th::EffectiveAccent(Variant::Midnight);
  std::printf("  [c124] accent day=(%d,%d,%d) night=(%d,%d,%d)\n", ByteOf(day).r,
              ByteOf(day).g, ByteOf(day).b, ByteOf(night).r, ByteOf(night).g,
              ByteOf(night).b);
  PRISM_CHECK(Near(day.r, night.r, 0.0005));
  PRISM_CHECK(Near(day.g, night.g, 0.0005));
  PRISM_CHECK(Near(day.b, night.b, 0.0005));
  PRISM_CHECK(Near(day.r, th::ChannelFromByte(10), 0.0005));
  PRISM_CHECK(Near(day.g, th::ChannelFromByte(132), 0.0005));
  PRISM_CHECK(Near(day.b, th::ChannelFromByte(255), 0.0005));

  // The gradient's far stop is the same value: a second stop is a second accent
  // on every surface that can express a gradient.
  const th::Gradient g = th::Lookup(TokenId::AccentLiquidPrimary, Variant::Daylight)
                             .gradient;
  PRISM_CHECK(Near(g.from.r, g.to.r, 0.0005));
  PRISM_CHECK(Near(g.from.g, g.to.g, 0.0005));
  PRISM_CHECK(Near(g.from.b, g.to.b, 0.0005));
}

// ---------------------------------------------------------------------------
// V2: FIVE SURFACES
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v2_five_surface_tiers") {
  PRISM_CHECK_EQ(th::kSurfaceTierCount, std::size_t{5});

  // The five names, in ladder order: a rename is a design change and must be
  // visible here.
  const char* names[5] = {"canvas", "panel", "card", "floating", "border"};
  for (std::size_t i = 0; i < th::kSurfaceTierCount; ++i) {
    std::printf("  [c124] tier %zu = %s\n", i,
                std::string(th::SurfaceTierName(th::kSurfaceTiers[i])).c_str());
    PRISM_CHECK_EQ(std::string(th::SurfaceTierName(th::kSurfaceTiers[i])),
                   std::string(names[i]));
  }

  // The ladder's PRESENTED pixels are the card's five hexes.
  for (std::size_t i = 0; i < th::kSurfaceTierCount; ++i) {
    const Pixel got = ByteOf(ch::SurfaceTierPresented(th::kSurfaceTiers[i], Variant::Midnight));
    std::printf("  [c124] Midnight %-8s = #%02X%02X%02X (want #%02X%02X%02X)\n",
                std::string(th::SurfaceTierName(th::kSurfaceTiers[i])).c_str(), got.r,
                got.g, got.b, kLadder[i].r, kLadder[i].g, kLadder[i].b);
    PRISM_CHECK_EQ(got.r, kLadder[i].r);
    PRISM_CHECK_EQ(got.g, kLadder[i].g);
    PRISM_CHECK_EQ(got.b, kLadder[i].b);
  }

  // ... and the four steps between them are the audit's deltas. The ORDER is
  // part of the assertion: a ladder that is still four steps but climbs the
  // wrong way (darker as it rises) is not a ladder.
  const Pixel canvas = ByteOf(ch::SurfaceTierPresented(SurfaceTier::Canvas, Variant::Midnight));
  const Pixel panel = ByteOf(ch::SurfaceTierPresented(SurfaceTier::Panel, Variant::Midnight));
  const Pixel card = ByteOf(ch::SurfaceTierPresented(SurfaceTier::Card, Variant::Midnight));
  const Pixel floating =
      ByteOf(ch::SurfaceTierPresented(SurfaceTier::Floating, Variant::Midnight));
  const Pixel border = ByteOf(ch::SurfaceTierPresented(SurfaceTier::Border, Variant::Midnight));
  const Pixel steps[5] = {canvas, panel, card, floating, border};
  for (int i = 0; i + 1 < 5; ++i) {
    const double measured = DeltaE(steps[i], steps[i + 1]);
    std::printf("  [c124] dE(%s -> %s) = %.2f (card %.1f)\n", names[i], names[i + 1],
                measured, kLadderDeltaE[i]);
    PRISM_CHECK(Near(measured, kLadderDeltaE[i], kDeltaETolerance));
  }

  // NO SIXTH TONE: the raised tab plate is the CARD tier, in every theme. C79
  // shipped it as a value of its own -- one of the eleven.
  const Variant variants[3] = {Variant::Daylight, Variant::Midnight, Variant::HighContrast};
  for (const Variant variant : variants) {
    const Pixel plate = ByteOf(ch::SurfaceTierPresented(SurfaceTier::Card, variant));
    const Pixel tab = ByteOf(ch::ThemeSurfaceColor(TokenId::SurfaceTabActive, variant));
    PRISM_CHECK_EQ(th::TierOf(TokenId::SurfaceTabActive), SurfaceTier::Card);
    std::printf("  [c124] %s tab plate (%d,%d,%d) card tier (%d,%d,%d)\n",
                std::string(th::VariantName(variant)).c_str(), tab.r, tab.g, tab.b, plate.r,
                plate.g, plate.b);
    PRISM_CHECK_EQ(tab.r, plate.r);
    PRISM_CHECK_EQ(tab.g, plate.g);
    PRISM_CHECK_EQ(tab.b, plate.b);
  }

  // The tier vocabulary is total: each tier names a token that IS that tier, and
  // the six surface tokens collapse to exactly five tones.
  for (std::size_t i = 0; i < th::kSurfaceTierCount; ++i) {
    const SurfaceTier tier = th::kSurfaceTiers[i];
    PRISM_CHECK_EQ(th::TierOf(th::SurfaceTierToken(tier)), tier);
  }
  std::vector<std::string> tones;
  const char* surfaceTokens[6] = {"--surface-canvas-base", "--surface-panel",
                                  "--surface-card",        "--surface-floating-island",
                                  "--surface-border",      "--surface-tab-active"};
  for (const char* token : surfaceTokens) {
    const Rgba c = th::Lookup(std::string_view(token), Variant::Midnight).color;
    const Pixel p = ByteOf(c);
    char key[32];
    std::snprintf(key, sizeof(key), "#%02X%02X%02X", p.r, p.g, p.b);
    if (std::find(tones.begin(), tones.end(), std::string(key)) == tones.end()) {
      tones.push_back(key);
    }
  }
  std::printf("  [c124] distinct surface TONES across six surface tokens: %zu\n",
              tones.size());
  PRISM_CHECK_EQ(tones.size(), std::size_t{5});
}

// ---------------------------------------------------------------------------
// V3: THE HIERARCHY (rail back, island forward)
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v3_surface_contrast") {
  // The measured relationships, through the ONE compositing entry. The rail is
  // the PANEL tier; the action island is the FLOATING tier.
  for (const Variant variant : {Variant::Daylight, Variant::Midnight}) {
    const Rgba canvas = ch::CanvasPresentedColor(variant);
    const Rgba rail = ch::SurfaceTierPresented(SurfaceTier::Panel, variant);
    const Rgba island = ch::SurfaceTierPresented(SurfaceTier::Floating, variant);
    const double railCr = th::ContrastRatio(rail, canvas);
    const double islandCr = th::ContrastRatio(island, canvas);
    std::printf("  [c124] %s rail/canvas CR=%.3f island/canvas CR=%.3f\n",
                std::string(th::VariantName(variant)).c_str(), railCr, islandCr);
    // The rail STEPS BACK in BOTH themes: it is a passive column, and 1.32:1
    // made it the loudest surface in the window (UI-AUDIT 12.2).
    PRISM_CHECK(railCr <= 1.10);
    PRISM_CHECK(railCr >= 1.03);
    if (variant == Variant::Midnight) {
      // The action island comes FORWARD: >= 1.28 (the card's floor; it measures
      // 1.317). Material rule R4 (DESIGN-PROPOSAL 12.8) is the same number.
      // WHY ONLY MIDNIGHT: 1.32 is a DARK-theme number -- the audit sampled a
      // Midnight window. On Daylight the canvas is #F5F5F7 paper and a white
      // glass sheet cannot reach 1.28:1 against it at ANY alpha (pure white
      // white on #F5F5F7 is 1.098), so the light theme's island contract is
      // C102's ">= 8 levels off the canvas", asserted below. Quoting the dark
      // number at the light theme would have failed for arithmetic reasons, not
      // design ones.
      PRISM_CHECK(islandCr >= 1.28);
      // And the ORDER is the point: the island must out-rank the rail.
      PRISM_CHECK(islandCr > railCr + 0.15);
    } else {
      // C102's floor, measured on the same pixels: the island has body.
      const Pixel islandPx = ByteOf(island);
      const Pixel canvasPx = ByteOf(canvas);
      const int levels = (std::max)({std::abs(islandPx.r - canvasPx.r),
                                     std::abs(islandPx.g - canvasPx.g),
                                     std::abs(islandPx.b - canvasPx.b)});
      std::printf("  [c124] Daylight island levels off canvas = %d (C102 floor 8)\n",
                  levels);
      PRISM_CHECK(levels >= 8);
      PRISM_CHECK(islandCr > 1.0);
    }
  }

  // The text roles still clear WCAG on the new canvas: a darker canvas is only
  // acceptable if the ink on it still reads.
  for (const Variant variant : {Variant::Daylight, Variant::Midnight}) {
    const double ink =
        th::ContrastRatio(ch::ThemeSurfaceColor(TokenId::TextPrimary, variant),
                          ch::CanvasPresentedColor(variant));
    std::printf("  [c124] %s body ink CR=%.2f\n",
                std::string(th::VariantName(variant)).c_str(), ink);
    PRISM_CHECK(ink >= th::kWcagBodyTextMinRatio);
  }
}

// ---------------------------------------------------------------------------
// V4: THE FOLDER IS GOLD (and gold is a material, not an accent)
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v4_folder_tones") {
  const Rgba highlight = th::Lookup(TokenId::FolderHighlight, Variant::Daylight).color;
  const Rgba body = th::Lookup(TokenId::FolderBody, Variant::Daylight).color;
  const Rgba shadow = th::Lookup(TokenId::FolderShadow, Variant::Daylight).color;
  const double lh = th::RelativeLuminance(highlight);
  const double lb = th::RelativeLuminance(body);
  const double ls = th::RelativeLuminance(shadow);
  std::printf("  [c124] folder luminance highlight=%.4f body=%.4f shadow=%.4f\n", lh, lb,
              ls);
  // Strictly decreasing: light from above, a solid object. Equal tones would be
  // a flat sticker; increasing ones would be lit from below.
  PRISM_CHECK(lh > lb);
  PRISM_CHECK(lb > ls);

  // The body is the audit's #FFB666 and measures ~1.61:1 on white: a MATERIAL
  // colour that is recognisable without competing with the accent.
  const Pixel got = ByteOf(body);
  std::printf("  [c124] folder body #%02X%02X%02X on white CR=%.2f\n", got.r, got.g,
              got.b, th::ContrastRatio(body, Rgba(1.0f, 1.0f, 1.0f, 1.0f)));
  PRISM_CHECK_EQ(got.r, 255);
  PRISM_CHECK_EQ(got.g, 182);
  PRISM_CHECK_EQ(got.b, 102);
  // THE CARD QUOTES 1.61:1 "on white". That figure is the audit's SCREENSHOT
  // sample of Bandizip's folder -- an anti-aliased glyph blended over white --
  // while the pure token measures 1.73 (0.5568 relative luminance:
  // (1.05)/(0.6068)). Both say the same thing, and THAT is what is asserted: a
  // material colour far below the 3:1 at which it would start competing with
  // the accent, and still well above a whisper. Pinning 1.61 +-0.05 would have
  // encoded a sampling artefact of someone else's screenshot into our token.
  const double onWhite = th::ContrastRatio(body, Rgba(1.0f, 1.0f, 1.0f, 1.0f));
  PRISM_CHECK(onWhite >= 1.5);
  PRISM_CHECK(onWhite <= 2.0);
  PRISM_CHECK(onWhite < th::kWcagSecondaryTextMinRatio);
  PRISM_CHECK_EQ(ByteOf(highlight).g, 217);
  PRISM_CHECK_EQ(ByteOf(shadow).r, 219);

  // The SAME three tones in every theme: a folder is a material, not a tint of
  // whatever the theme is doing.
  for (const Variant variant : {Variant::Midnight, Variant::HighContrast}) {
    PRISM_CHECK(Near(th::Lookup(TokenId::FolderBody, variant).color.g,
                     th::ChannelFromByte(182), 0.001));
    PRISM_CHECK(Near(th::Lookup(TokenId::FolderShadow, variant).color.r,
                     th::ChannelFromByte(219), 0.001));
  }

  // ... and it is NOT the accent any more (the defect: #2480DF was the fourth
  // blue, indistinguishable in hue from the selection bar).
  const Rgba accent = th::EffectiveAccent(Variant::Midnight);
  std::printf("  [c124] dE(folder body, accent) = %.1f\n",
              DeltaE(ByteOf(body), ByteOf(accent)));
  PRISM_CHECK(DeltaE(ByteOf(body), ByteOf(accent)) > 40.0);
}

// ---------------------------------------------------------------------------
// V5a: THE TYPE CLASSIFIER (the pixel half is win.test_c124)
// ---------------------------------------------------------------------------
PRISM_TEST("c124.v5a_file_kind_classifier") {
  struct Sample {
    const char* path;
    icons::FileKind want;
  };
  const Sample samples[] = {
      {"readme.md", icons::FileKind::Document},
      {"data.bin", icons::FileKind::Binary},
      {"Berlin/gate_front.png", icons::FileKind::Image},
      {"src/main.cpp", icons::FileKind::Code},
      {"notes.xyz", icons::FileKind::Unknown},
      {"noext", icons::FileKind::Unknown},
      {"README.MD", icons::FileKind::Document},
      {"dir.with.dots/file", icons::FileKind::Unknown},
  };
  for (const Sample& sample : samples) {
    const icons::FileKind got = icons::KindFromPath(sample.path);
    std::printf("  [c124] %-24s -> %s\n", sample.path,
                std::string(icons::FileKindName(got)).c_str());
    PRISM_CHECK_EQ(got, sample.want);
  }

  // Four kinds + Unknown, each with a silhouette inside the 16 DIP box, and each
  // stroke at the card's 16 px tier (1.25 DIP). The last point matters: a
  // bitmap or a 1 px stroke would not survive C120's PerMonitorV2 scaling.
  for (const icons::FileKind kind : icons::kFileKinds) {
    const icons::Geometry geometry = icons::GeometryOf(kind);
    PRISM_CHECK(geometry.count >= 3);
    for (std::size_t i = 0; i < geometry.count; ++i) {
      const icons::Shape& shape = geometry.shapes[i];
      PRISM_CHECK(shape.x0 >= 0.0f && shape.x1 <= icons::kIconSizeDip);
      PRISM_CHECK(shape.y0 >= 0.0f && shape.y1 <= icons::kIconSizeDip);
      if (shape.type != icons::ShapeType::FillEllipse &&
          shape.type != icons::ShapeType::FillRect) {
        PRISM_CHECK(Near(shape.stroke, icons::kIconStrokeDip, 0.001));
      }
    }
  }

  // No two kinds share a silhouette: this is what makes the pixel assertion in
  // the Windows lane possible at all.
  for (const icons::FileKind a : icons::kFileKinds) {
    for (const icons::FileKind b : icons::kFileKinds) {
      if (a == b) continue;
      const icons::Geometry ga = icons::GeometryOf(a);
      const icons::Geometry gb = icons::GeometryOf(b);
      bool identical = ga.count == gb.count;
      for (std::size_t i = 0; identical && i < ga.count; ++i) {
        const icons::Shape& sa = ga.shapes[i];
        const icons::Shape& sb = gb.shapes[i];
        identical = sa.type == sb.type && Near(sa.x0, sb.x0) && Near(sa.y0, sb.y0) &&
                    Near(sa.x1, sb.x1) && Near(sa.y1, sb.y1);
      }
      PRISM_CHECK(!identical);
    }
  }
}
