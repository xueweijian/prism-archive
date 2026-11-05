// PrismArchive -- PrismCore
// C108: THE VISUAL BASELINE RECORDER -- the numbers docs/UI-BASELINE.md quotes.
//
// WHY THIS FILE EXISTS. Nine cards (C100..C107, C109) are about to change the
// same pixels. "Looks better" is not reviewable, so this card ships ONE
// executable that measures the eleven claims the matrix is made of, prints them
// as machine-readable rows, and -- the load-bearing half -- proves each target
// predicate can REJECT a degraded value. A predicate that cannot go red is a
// slogan.
//
// WHAT IS MEASURED HERE (source + token arithmetic, no device needed):
//   mnemonic_letters    ASCII mnemonic letters the band and the menu paint:
//                       kActions[].mnemonic != 0 (8 today) + "(&X)" labels in
//                       Assets/Lang/zh-CN.json (1 today: 设置(&S)...).
//   day_canvas          CanvasPresentedColor(Daylight) -- the pixel a resting
//                       canvas shows, through the ONE theme entry.
//   island_delta        max channel |island - canvas| in Daylight; the plan's
//                       "the island must have a body" rule is >= 8 levels.
//   segoe_literals      occurrences of the literal "Segoe UI" under Src/
//                       (the C103 guard: zero outside the font entry).
//   menu_caption_delta  max channel |PaletteFor(v).background - the caption
//                       token| per variant (the C106 "one shell" rule, <= 2).
//   optics_live         member-call sites of InitializeOptics under Src/
//                       (the C107 wiring; 0 means the chain never runs).
//   panel_backdrop_delta derived: 0 while optics_live == 0 (a flat fill cannot
//                       follow the content under it). C107's own Windows suite
//                       measures the red -> blue delta on pixels; this row says
//                       WHY it is zero today, not a second opinion about it.
//
// The PIXEL rows of the matrix (compress-icon centre, ratio-bar geometry, the
// sidebar's selection corner, the info-card hairline) are measured from the
// committed baseline screenshot by tools/visual_baseline.ps1; this binary owns
// the rows a screenshot cannot answer (literals, tokens, wiring).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): three arms, each compiled from THIS
// source with exactly one rule broken, each printing its verdict at the first
// failed assertion and exiting 0:
//   PRISM_C108_MUTANT_EMPTY_ROOT   the scanner is pointed at a root with no Src/
//   PRISM_C108_MUTANT_FLAT_DAY     the Day canvas is forced to the island colour
//   PRISM_C108_MUTANT_MNEMONIC     the menu's mnemonic letter is forced back in
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "mini_test.hpp"

#include "../Src/PrismCore/ChromeTheme.hpp"
#include "../Src/PrismCore/ActionBarModel.hpp"
#include "../Src/PrismCore/ThemeTokens.hpp"

#ifndef PRISM_C108_SOURCE_DIR
#define PRISM_C108_SOURCE_DIR "."
#endif

namespace {

namespace fs = std::filesystem;
using prism::theme::Rgba;
using prism::theme::TokenId;
using prism::theme::Variant;
using prism::chrome::PaletteFor;
using prism::chrome::CanvasPresentedColor;
using prism::chrome::ThemeSurfaceColor;

#if defined(PRISM_C108_MUTANT_EMPTY_ROOT)
constexpr const char* kMutation = "scan_root";
#elif defined(PRISM_C108_MUTANT_FLAT_DAY)
constexpr const char* kMutation = "flat_day";
#elif defined(PRISM_C108_MUTANT_MNEMONIC)
constexpr const char* kMutation = "mnemonic";
#else
constexpr const char* kMutation = nullptr;
#endif

[[maybe_unused]] int& InjectedHits() {
  static int hits = 0;
  return hits;
}

int Level(float v) {
  const double scaled = static_cast<double>(v) * 255.0 + 0.5;
  return static_cast<int>(scaled < 0.0 ? 0.0 : (scaled > 255.0 ? 255.0 : scaled));
}

int MaxChannelDelta(const Rgba& a, const Rgba& b) {
  return (std::max)({(std::abs)(Level(a.r) - Level(b.r)), (std::abs)(Level(a.g) - Level(b.g)),
                     (std::abs)(Level(a.b) - Level(b.b))});
}

// ---------------------------------------------------------------- the scan

struct Scan {
  std::size_t files = 0;
  std::size_t segoeLiterals = 0;
  std::size_t opticsCallSites = 0;
  std::size_t menuMnemonics = 0;
  bool sawLangTable = false;
  bool sawRoot = false;
};

bool IsTextSource(const fs::path& p) {
  const std::string ext = p.extension().string();
  return ext == ".hpp" || ext == ".cpp" || ext == ".h" || ext == ".cc" || ext == ".rc" ||
         ext == ".def";
}

// Count non-overlapping occurrences of needle in text.
std::size_t CountOf(const std::string& text, const std::string& needle) {
  if (needle.empty()) return 0;
  std::size_t count = 0;
  std::size_t at = text.find(needle);
  while (at != std::string::npos) {
    ++count;
    at = text.find(needle, at + needle.size());
  }
  return count;
}

// Scan <root>/Src for the literals and call sites this card measures. The file
// counter is the anti-vacuousness proof: a wrong root must be RED, never a
// comfortable zero.
Scan ScanSource(const std::string& root) {
  Scan scan;
  const fs::path src = fs::path(root) / "Src";
  std::error_code ec;
  if (!fs::is_directory(src, ec)) return scan;
  scan.sawRoot = true;
  for (fs::recursive_directory_iterator it(src, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const fs::path& path = it->path();
    if (!IsTextSource(path)) continue;
    std::ifstream in(path, std::ios::binary);
    if (!in) continue;
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    ++scan.files;
    scan.segoeLiterals += CountOf(text, "Segoe UI");
    scan.opticsCallSites += CountOf(text, ".InitializeOptics(");
    scan.opticsCallSites += CountOf(text, "->InitializeOptics(");
  }

  // The menu bar's mnemonic letters: "(&S)" style markers in the shipped table.
  const fs::path lang = fs::path(root) / "Assets" / "Lang" / "zh-CN.json";
  if (fs::is_regular_file(lang, ec)) {
    std::ifstream in(lang, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());
    scan.sawLangTable = !text.empty();
    for (std::size_t i = 0; i + 3 < text.size(); ++i) {
      if (text[i] != '(' || text[i + 1] != '&') continue;
      const char letter = text[i + 2];
      const bool ascii = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
      if (ascii && text[i + 3] == ')') ++scan.menuMnemonics;
    }
  }
  return scan;
}

// --------------------------------------------------------------- the table

struct Metric {
  std::string id;
  std::string card;
  double value = 0.0;     // the measured number
  std::string shown;      // what the row prints for a human
  std::string threshold;  // the target, as text
  bool ok = false;        // the TARGET predicate's verdict
};

// The target predicates, in one place so the recorder, the arms and the doc
// cannot disagree about where a line sits.
bool TargetMnemonic(double v) { return v <= 0.0; }
bool TargetDayCanvas(int r, int g, int b) {
  return (std::abs)(r - 245) <= 2 && (std::abs)(g - 245) <= 2 && (std::abs)(b - 247) <= 2;
}
bool TargetIslandDelta(double v) { return v >= 8.0; }
bool TargetSegoe(double v) { return v <= 0.0; }
bool TargetMenuCaption(double v) { return v <= 2.0; }
bool TargetOptics(double v) { return v >= 1.0; }
bool TargetPanelBackdrop(double v) { return v >= 4.0; }
bool TargetRatioHeight(double v) { return v == 4.0; }
bool TargetRatioAlpha(double v) { return v <= 0.56; }
bool TargetRatioSaturation(double v) { return v <= 0.45; }

std::vector<Metric> Measure() {
  std::vector<Metric> rows;
  const std::string root = PRISM_C108_SOURCE_DIR;
  const Scan scan = ScanSource(root);

  // 1. mnemonic letters.
  int actionLetters = 0;
#if !defined(PRISM_C108_MUTANT_MNEMONIC)
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    if (prism::actionbar::SpecOf(i).mnemonic != 0) ++actionLetters;
  }
#else
  actionLetters = prism::actionbar::kActionCount;  // the pre-C100 band: every letter
  ++InjectedHits();
#endif
  double menuLetters = static_cast<double>(scan.menuMnemonics);
#if defined(PRISM_C108_MUTANT_MNEMONIC)
  if (menuLetters == 0) {  // the arm must still carry the pre-C106 menu letter
    menuLetters = 1;
    ++InjectedHits();
  }
#endif
  const double mnemonics = static_cast<double>(actionLetters) + menuLetters;
  {
    Metric m;
    m.id = "mnemonic_letters";
    m.card = "C100,C106";
    m.value = mnemonics;
    m.shown = std::to_string(actionLetters) + " band + " + std::to_string(
                  static_cast<int>(menuLetters)) + " menu";
    m.threshold = "== 0";
    m.ok = TargetMnemonic(mnemonics);
    rows.push_back(m);
  }

  // 2. the Day canvas.
  Rgba canvas = CanvasPresentedColor(Variant::Daylight);
#if defined(PRISM_C108_MUTANT_FLAT_DAY)
  canvas = ThemeSurfaceColor(TokenId::SurfaceFloatingIsland, Variant::Daylight);
  ++InjectedHits();
#endif
  {
    Metric m;
    m.id = "day_canvas";
    m.card = "C102";
    m.value = Level(canvas.r) * 65536 + Level(canvas.g) * 256 + Level(canvas.b);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", Level(canvas.r), Level(canvas.g),
                  Level(canvas.b));
    m.shown = buf;
    m.threshold = "#F5F5F7 +-2/channel";
    m.ok = TargetDayCanvas(Level(canvas.r), Level(canvas.g), Level(canvas.b));
    rows.push_back(m);
  }

  // 3. the island's body.
  const Rgba island = ThemeSurfaceColor(TokenId::SurfaceFloatingIsland, Variant::Daylight);
  {
    Metric m;
    m.id = "island_delta";
    m.card = "C102";
    m.value = MaxChannelDelta(island, canvas);
    m.shown = std::to_string(static_cast<int>(m.value)) + " levels";
    // Why 8: one JND at these luminances is ~2-3 levels, and C102's plan fixes
    // the floor at 8 so the island keeps a body at 100% and at 150% DPI.
    m.threshold = ">= 8";
    m.ok = TargetIslandDelta(m.value);
    rows.push_back(m);
  }

  // 4. one font stack.
  {
    Metric m;
    m.id = "segoe_literals";
    m.card = "C103";
    m.value = static_cast<double>(scan.segoeLiterals);
    m.shown = std::to_string(scan.segoeLiterals) + " in " + std::to_string(scan.files) +
              " files";
    m.threshold = "== 0";
    m.ok = TargetSegoe(m.value);
    rows.push_back(m);
  }

  // 5. one shell: the menu plate IS the caption colour, in every theme.
  {
    double worst = 0.0;
    std::string worstVariant = "-";
    for (const Variant v : {Variant::Daylight, Variant::Midnight, Variant::HighContrast}) {
      const Rgba plate = PaletteFor(v).background;
      const Rgba caption = prism::theme::Lookup(TokenId::SurfaceBackdropReference, v).color;
      const double d = static_cast<double>(MaxChannelDelta(plate, caption));
      if (d >= worst) {
        worst = d;
        worstVariant = std::string(prism::theme::VariantName(v));
      }
    }
    Metric m;
    m.id = "menu_caption_delta";
    m.card = "C106";
    m.value = worst;
    m.shown = std::to_string(static_cast<int>(worst)) + " levels (" + worstVariant + ")";
    m.threshold = "<= 2";
    m.ok = TargetMenuCaption(m.value);
    rows.push_back(m);
  }

  // 6. the optical chain is connected.
  {
    Metric m;
    m.id = "optics_live";
    m.card = "C107";
    m.value = static_cast<double>(scan.opticsCallSites);
    m.shown = std::to_string(scan.opticsCallSites) + " member call sites";
    m.threshold = ">= 1";
    m.ok = TargetOptics(m.value);
    rows.push_back(m);
  }

  // 7. the panel follows what is under it (0 while the chain is off).
  {
    Metric m;
    m.id = "panel_backdrop_delta";
    m.card = "C107";
    m.value = scan.opticsCallSites == 0 ? 0.0 : -1.0;
    m.shown = scan.opticsCallSites == 0 ? "0 (chain off: flat fill)"
                                        : "n/a (win.c107_optics measures it)";
    m.threshold = ">= 4 (R)";
    m.ok = TargetPanelBackdrop(m.value);
    rows.push_back(m);
  }

  return rows;
}

// The bar's geometry/alpha/saturation live in the Windows painter and are read
// by tools/visual_baseline.ps1 from GalleryRenderer.hpp + the screenshot; the
// core lane still owns their THRESHOLDS (the arms assert them).
bool ArmPredicatesHold() {
  const bool a = !TargetRatioHeight(6.0) && TargetRatioHeight(4.0);
  const bool b = !TargetRatioAlpha(1.0) && TargetRatioAlpha(0.55);
  const bool c = !TargetRatioSaturation(0.738) && TargetRatioSaturation(0.30);
  return a && b && c;
}

const std::vector<Metric>& Cached() {
  static const std::vector<Metric> rows = Measure();
  return rows;
}

// One row by id. A missing id is a programming error in the suite, so the
// fallback keeps the reference valid and makes the assertion below fail.
[[maybe_unused]] const Metric& Row(const std::vector<Metric>& rows, const std::string& id) {
  static const Metric kMissing{};
  for (const Metric& m : rows) {
    if (m.id == id) return m;
  }
  return kMissing;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::printf("C108 MUTANT ACTIVE: %s\n", kMutation);
    static const std::string prefix = std::string("C108 MUTATION CAUGHT: ") + kMutation;
    // CONTRACTS D3: the verdict is armed on a RUNTIME proof that the injected
    // branch ran, not on "the build flag was defined". empty_root is proven by
    // the scan finding no Src/ at all; the other two by their injection counter.
#if defined(PRISM_C108_MUTANT_EMPTY_ROOT)
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] {
      return !ScanSource(PRISM_C108_SOURCE_DIR).sawRoot;
    });
#else
    ::prism::test::SetMutationVerdict(prefix.c_str(), [] { return InjectedHits() > 0; });
#endif
  }
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0) std::puts("ALL C108 BASELINE CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// 1. THE SCANNER IS NOT VACUOUS. A wrong root, a renamed folder or a filter
//    that matches nothing must be RED here, not a comfortable zero downstream.
// ---------------------------------------------------------------------------
PRISM_TEST("c108 the baseline scan really walked the tree") {
  const std::string root = PRISM_C108_SOURCE_DIR;
  const Scan scan = ScanSource(root);
  PRISM_CHECK(scan.sawRoot);
  PRISM_CHECK(scan.files >= 50);        // the repo has ~120 headers today
  PRISM_CHECK(scan.segoeLiterals >= 10);  // pre-C103 truth; C103 drives it to 0
  PRISM_CHECK(scan.sawLangTable);
  PRISM_CHECK(scan.menuMnemonics >= 1);  // "设置(&S)..." is in the shipped table

  int actionLetters = 0;
  for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
    if (prism::actionbar::SpecOf(i).mnemonic != 0) ++actionLetters;
  }
  PRISM_CHECK_EQ(actionLetters, 8);      // the eight corner letters C100 removes

  // Every theme resolves through the one entry, and none falls back to the
  // magenta sentinel (a "known" token that is actually missing).
  for (const Variant v : {Variant::Daylight, Variant::Midnight, Variant::HighContrast}) {
    const Rgba c = CanvasPresentedColor(v);
    PRISM_CHECK(!(c.r > 0.95f && c.g < 0.05f && c.b > 0.95f));
  }
}

// ---------------------------------------------------------------------------
// 2. THE TABLE IS COMPLETE AND STABLE. Every row the doc quotes is printed, and
//    a second measurement produces the same numbers (no time, no randomness).
// ---------------------------------------------------------------------------
PRISM_TEST("c108 the metric table is complete, stable and printed") {
  const std::vector<Metric>& first = Cached();
  const std::vector<Metric> second = Measure();
  PRISM_CHECK_EQ(first.size(), static_cast<std::size_t>(7));
  PRISM_CHECK_EQ(first.size(), second.size());

  std::puts("C108 METRIC TABLE (source + token arithmetic; the pixel rows live in "
            "tools/visual_baseline.ps1)");
  for (std::size_t i = 0; i < first.size(); ++i) {
    const Metric& m = first[i];
    PRISM_CHECK_EQ(m.id, second[i].id);
    PRISM_CHECK_EQ(m.value, second[i].value);
    std::printf("C108 METRIC %s=%s target=%s card=%s verdict=%s\n", m.id.c_str(),
                m.shown.c_str(), m.threshold.c_str(), m.card.c_str(),
                m.ok ? "PASS" : "FAIL");
  }
  PRISM_CHECK(ArmPredicatesHold());
}

// ---------------------------------------------------------------------------
// 3. THE TARGET PREDICATES REJECT A DEGRADED VALUE. Only compiled into an arm:
//    on the clean build the measured value is whatever the product is, and this
//    case would confuse "the baseline" with "the target".
// ---------------------------------------------------------------------------
#if defined(PRISM_C108_MUTANT_EMPTY_ROOT) || defined(PRISM_C108_MUTANT_FLAT_DAY) || \
    defined(PRISM_C108_MUTANT_MNEMONIC)
// THE ARM. Each control compiles ONE degradation in and asserts the row that
// degradation moves is GREEN against its target. It cannot be: the injected
// value is the pre-fix one, so the check fails and -- because the verdict is
// armed on the runtime proof that the injection fired -- the harness prints
// CAUGHT and leaves. A clean build never reaches this case.
PRISM_TEST("c108 the injected degradation is rejected by its target predicate") {
#if defined(PRISM_C108_MUTANT_EMPTY_ROOT)
  const Scan scan = ScanSource(PRISM_C108_SOURCE_DIR);
  PRISM_CHECK(scan.sawRoot);
  PRISM_CHECK(scan.files >= 50);
#elif defined(PRISM_C108_MUTANT_FLAT_DAY)
  const Metric& m = Row(Cached(), "day_canvas");
  PRISM_CHECK(m.ok);  // the forced island-coloured canvas must not pass #F5F5F7
#else
  const Metric& m = Row(Cached(), "mnemonic_letters");
  PRISM_CHECK(m.ok);  // the eight corner letters must not pass "== 0"
#endif
}
#endif
