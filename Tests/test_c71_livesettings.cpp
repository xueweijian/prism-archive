// PrismArchive -- C71 (core lane): the two appearance keys and the overwrite
// policy as PURE functions.
//
// The Windows suite (Tests/win/test_c71_livesettings.cpp) proves the pixels and
// the on-disk extraction differences; this lane owns the arithmetic and the
// decision table, and it is the one that runs under ASan/UBSan on Linux.
//
// Everything here is an assertion on a value, never on a name/comment (D2):
//   * the accent sentinel is the SCHEMA default, so a fresh document must NOT
//     repaint the shipping theme;
//   * a custom accent must resolve to the exact RGB the test parsed;
//   * glass intensity is RELATIVE to the schema default 0.72, so the default is
//     alpha * 1.0 (an upgrade is pixel-identical) and 0.36 is exactly half;
//   * overwrite modes yield replace / skip / "name (1).ext" and ask goes through
//     the injected prompt (never a real dialog).
#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "mini_test.hpp"
#include "../Src/PrismCore/Appearance.hpp"
#include "../Src/PrismCore/OverwritePolicy.hpp"
#include "../Src/PrismCore/SettingsStore.hpp"
#include "../Src/PrismCore/ThemeTokens.hpp"

namespace th = prism::theme;
namespace ap = prism::appearance;
namespace ow = prism::overwrite;

namespace {

bool Near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) < eps; }

void ResetRuntime() { th::live::Reset(); }

}  // namespace

PRISM_TEST("appearance: the stored default accent is a sentinel, not an override") {
  ResetRuntime();
  prism::settings::SettingsStore store;
  // The schema default and this header's sentinel must agree; a drift would
  // make a fresh install look like a user-chosen colour.
  PRISM_CHECK_EQ(store.GetString("appearance.accentColor"), std::string(ap::kDefaultAccentHex));
  PRISM_CHECK(Near(store.GetNumber("appearance.glassIntensity", -1.0),
                   ap::kDefaultGlassIntensity));

  const ap::Applied applied = ap::Apply(store);
  PRISM_CHECK(applied.accentParsed);
  PRISM_CHECK(!applied.accentApplied);
  // Midnight is the shipping window theme; with no override the accent is its
  // token, byte for byte.
  const th::Rgba token =
      th::Lookup(th::TokenId::AccentLiquidPrimary, th::Variant::Midnight).gradient.from;
  const th::Rgba live = th::EffectiveAccent(th::Variant::Midnight);
  PRISM_CHECK(Near(live.r, token.r));
  PRISM_CHECK(Near(live.g, token.g));
  PRISM_CHECK(Near(live.b, token.b));
}

PRISM_TEST("appearance: a saved accent becomes the live accent") {
  ResetRuntime();
  prism::settings::SettingsStore store;
  PRISM_CHECK(store.SetString("appearance.accentColor", "#FF8800"));
  const ap::Applied applied = ap::Apply(store);
  PRISM_CHECK(applied.accentParsed);
  PRISM_CHECK(applied.accentApplied);

  th::Rgba parsed;
  PRISM_CHECK(ap::ParseHexColor("#FF8800", &parsed));
  const th::Rgba live = th::EffectiveAccent(th::Variant::Daylight);
  PRISM_CHECK(Near(live.r, parsed.r));
  PRISM_CHECK(Near(live.g, parsed.g));
  PRISM_CHECK(Near(live.b, parsed.b));
  PRISM_CHECK(Near(live.a, 1.0));

  // Invalid text must not invent a colour: the token stays.
  PRISM_CHECK(store.SetString("appearance.accentColor", "not-a-colour"));
  const ap::Applied rejected = ap::Apply(store);
  PRISM_CHECK(!rejected.accentParsed);
  PRISM_CHECK(!rejected.accentApplied);
  const th::Rgba token =
      th::Lookup(th::TokenId::AccentLiquidPrimary, th::Variant::Daylight).gradient.from;
  PRISM_CHECK(Near(th::EffectiveAccent(th::Variant::Daylight).r, token.r));
  ResetRuntime();
}

PRISM_TEST("appearance: glass intensity scales the token alpha around 0.72") {
  ResetRuntime();
  const th::Rgba baseline = th::EffectiveGlassSurface(th::Variant::Midnight);
  const th::Rgba token =
      th::Lookup(th::TokenId::SurfaceFloatingIsland, th::Variant::Midnight).color;
  PRISM_CHECK(Near(baseline.a, token.a));  // default 0.72 == reference == no change

  th::live::SetGlassIntensity(0.36);
  const th::Rgba half = th::EffectiveGlassSurface(th::Variant::Midnight);
  PRISM_CHECK(Near(half.a, token.a * 0.5));

  th::live::SetGlassIntensity(1.0);
  const th::Rgba full = th::EffectiveGlassSurface(th::Variant::Midnight);
  PRISM_CHECK(Near(full.a, (std::min)(1.0, static_cast<double>(token.a) * (1.0 / 0.72))));
  PRISM_CHECK(full.a > baseline.a);  // the two settings are observably different
  ResetRuntime();
}

PRISM_TEST("overwrite: replace / skip / rename decide the target path") {
  std::set<std::string> taken = {"out/note.txt", "out/note (1).txt"};
  const ow::ExistsFn exists = [&taken](const std::string& path) {
    return taken.count(path) != 0;
  };
  const std::string path = "out/note.txt";

  const ow::Decision replace = ow::Decide(path, false, ow::Mode::Replace, exists);
  PRISM_CHECK(replace.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(replace.path, path);

  const ow::Decision skip = ow::Decide(path, false, ow::Mode::Skip, exists);
  PRISM_CHECK(skip.choice == ow::Choice::Skip);
  PRISM_CHECK(skip.path.empty());

  const ow::Decision rename = ow::Decide(path, false, ow::Mode::Rename, exists);
  PRISM_CHECK(rename.choice == ow::Choice::Rename);
  PRISM_CHECK_EQ(rename.path, std::string("out/note (2).txt"));

  // The card's example exactly: existing target -> "xxx (1).txt".
  taken = {"out/xxx.txt"};
  const ow::Decision first = ow::Decide("out/xxx.txt", false, ow::Mode::Rename, exists);
  PRISM_CHECK_EQ(first.path, std::string("out/xxx (1).txt"));

  // A free path is untouched under every mode (no gratuitous rename).
  const ow::Decision free = ow::Decide("out/fresh.txt", false, ow::Mode::Rename, exists);
  PRISM_CHECK(free.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(free.path, std::string("out/fresh.txt"));

  // Directories merge: they are never renamed.
  const ow::Decision dir = ow::Decide("out/sub", true, ow::Mode::Rename, exists);
  PRISM_CHECK_EQ(dir.path, std::string("out/sub"));
}

PRISM_TEST("overwrite: ask goes through the seam, never a silent overwrite") {
  const std::set<std::string> taken = {"out/a.txt"};
  const ow::ExistsFn exists = [&taken](const std::string& path) {
    return taken.count(path) != 0;
  };
  int prompts = 0;
  std::string asked;
  ow::PromptFn prompt = [&](const std::string& path) {
    ++prompts;
    asked = path;
    return ow::Choice::Skip;
  };
  const ow::Decision decided = ow::Decide("out/a.txt", false, ow::Mode::Ask, exists, prompt);
  PRISM_CHECK_EQ(prompts, 1);
  PRISM_CHECK_EQ(asked, std::string("out/a.txt"));
  PRISM_CHECK(decided.choice == ow::Choice::Skip);

  // No prompt == Skip. The one answer that must never happen is Replace.
  prompts = 0;
  const ow::Decision unanswered = ow::Decide("out/a.txt", false, ow::Mode::Ask, exists);
  PRISM_CHECK(unanswered.choice == ow::Choice::Skip);
  PRISM_CHECK_EQ(prompts, 0);
}

PRISM_TEST("the schema keys carry the documented policy values") {
  prism::settings::SettingsStore store;
  PRISM_CHECK(store.SetString("extract.overwrite", "rename"));
  PRISM_CHECK(store.SetBool("extract.verifyAfterExtract", false));
  PRISM_CHECK(ow::ModeFromKey(store.GetString("extract.overwrite", "ask")) ==
              ow::Mode::Rename);
  PRISM_CHECK(!store.GetBool("extract.verifyAfterExtract", true));

  // The schema default is verify=ON; that is the documented behaviour, and the
  // mapping must not quietly invert it.
  prism::settings::SettingsStore fresh;
  PRISM_CHECK(ow::ModeFromKey(fresh.GetString("extract.overwrite", "ask")) ==
              ow::Mode::Ask);
  PRISM_CHECK(fresh.GetBool("extract.verifyAfterExtract", false));
}

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) return 1;
  std::puts("ALL C71 LIVE-SETTINGS CHECKS PASSED");
  return 0;
}
