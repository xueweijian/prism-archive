// Card C93b -- the menu bar speaks ONE language.
//
// Measured on the installed window (2026-11-05): a zh-CN menu bar read
//   文件  编辑  设置(S)...  Help
// Four labels came from BuildMenuLabels and the fifth from GalleryWindow's
// L"Help" default, because SetAboutMenuLabels was never called. This test pins
// the two Help labels to the SAME source as every other menu label, so a later
// refactor cannot quietly drop the call again.
#include <cstdio>
#include <string>

#include "AppWiring.hpp"
#include "I18n.hpp"
#include "mini_test.hpp"

namespace {

// The app resolves keys through Translator::Or(key, fallback); with an empty
// table every label is the fallback -- exactly what a fresh install shows
// before a catalog loads.
prism::app::MenuLabels LabelsFor(const wchar_t* tag) {
  // BuildMenuLabels takes Tr (a thin wrapper over a Translator pointer),
  // not the Translator itself. An empty table is the state a fresh install
  // shows before a catalog loads, so every label must fall back.
  prism::i18n::Translator tr;
  prism::app::Tr wrapper;
  wrapper.translator = &tr;
  return prism::app::BuildMenuLabels(wrapper, tag);
}

}  // namespace

PRISM_TEST("c93b the Help menu is translated like every other label") {
  const auto zh = LabelsFor(L"zh-CN");
  const auto en = LabelsFor(L"en-US");
  std::printf("  [c93b] zh help=\"%ls\" about=\"%ls\"\n", zh.help.c_str(),
              zh.about.c_str());
  // The four labels that already worked must keep working: this test exists
  // because ONE of the five was missing, not because the other four were fine.
  PRISM_CHECK(zh.file == L"文件");
  PRISM_CHECK(zh.edit == L"编辑");
  PRISM_CHECK(zh.settings == L"设置(&S)...");
  PRISM_CHECK(zh.help == L"帮助");
  PRISM_CHECK(zh.about == L"关于 PrismArchive");
  PRISM_CHECK(en.help == L"&Help");
  PRISM_CHECK(en.about == L"&About PrismArchive");
  // The regression string itself: the window's default must never be what a
  // translated build shows.
  PRISM_CHECK(zh.help != L"Help");
}

int main(int argc, char** argv) {
#if defined(PRISM_C93B_MUTANT_NO_HELP)
  std::printf("C93B MUTANT ACTIVE: no_help\n");
  ::prism::test::SetMutationVerdict("C93B MUTATION CAUGHT: no_help");
#endif
  return ::prism::test::RunAll(argc, argv);
}
