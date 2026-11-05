// Card C92 -- the right-click MODEL, on every lane (PrismCore only).
//
// The window's HMENU is a renderer of this model; asserting it here means a
// Linux lane also proves the three surfaces, their id ranges and the "a disabled
// item always has a reason" rule. The Windows suite proves the same model is
// what WM_CONTEXTMENU actually produces.
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "I18n.hpp"
#include "InAppContextMenu.hpp"
#include "mini_test.hpp"

#if !defined(PRISM_C92_LANG_DIR)
#error "PRISM_C92_LANG_DIR must name Assets/Lang (see Tests/suites/c92-input-menus.cmake)"
#endif

namespace {

using prism::ctxmenu::Command;
using prism::ctxmenu::Facts;
using prism::ctxmenu::Labels;
using prism::ctxmenu::Menu;
using prism::ctxmenu::Surface;

Facts RowFacts() {
  Facts facts;
  facts.archiveOpen = true;
  facts.hasSelection = true;
  facts.singleFileSelected = true;
  facts.rowCount = 3;
  return facts;
}

bool Contains(const Menu& menu, Command command) { return menu.Find(command) != nullptr; }

std::size_t SlotOf(const Menu& menu, Command command) {
  for (std::size_t i = 0; i < menu.items.size(); ++i) {
    if (menu.items[i].command == command) return i;
  }
  return menu.items.size();
}

}  // namespace

PRISM_TEST("c92.model_three_surfaces_with_distinct_id_ranges") {
  const Labels labels;
  const Menu row = prism::ctxmenu::Build(Surface::Row, RowFacts(), labels);
  const Menu empty = prism::ctxmenu::Build(Surface::Empty, RowFacts(), labels);
  const Menu sidebar = prism::ctxmenu::Build(Surface::Sidebar, RowFacts(), labels);

  // The card's minimum entry counts.
  PRISM_CHECK(row.items.size() >= 6);
  PRISM_CHECK(empty.items.size() >= 5);
  PRISM_CHECK(sidebar.items.size() >= 5);

  // The required verbs, per surface.
  PRISM_CHECK(Contains(row, Command::Open));
  PRISM_CHECK(Contains(row, Command::ExtractSelected));
  PRISM_CHECK(Contains(row, Command::ExtractAll));
  PRISM_CHECK(Contains(row, Command::SmartExtractHere));
  PRISM_CHECK(Contains(row, Command::CopyPath));
  PRISM_CHECK(Contains(row, Command::RevealArchive));
  PRISM_CHECK(Contains(empty, Command::ExtractAll));
  PRISM_CHECK(Contains(empty, Command::SelectAll));
  PRISM_CHECK(Contains(empty, Command::Refresh));
  PRISM_CHECK(Contains(empty, Command::RevealArchive));
  PRISM_CHECK(Contains(sidebar, Command::RevealArchive));
  PRISM_CHECK(Contains(sidebar, Command::CopyPath));

  // The three id sets cannot collide: every id is inside its surface's band.
  for (std::size_t i = 0; i < row.items.size(); ++i) {
    const std::uint32_t id = prism::ctxmenu::CommandId(Surface::Row, i);
    PRISM_CHECK(prism::ctxmenu::IsContextCommandId(id));
    PRISM_CHECK(prism::ctxmenu::SurfaceOfCommandId(id) == Surface::Row);
    PRISM_CHECK_EQ(prism::ctxmenu::SlotOfCommandId(id), i);
  }
  for (std::size_t i = 0; i < empty.items.size(); ++i) {
    PRISM_CHECK(prism::ctxmenu::SurfaceOfCommandId(prism::ctxmenu::CommandId(Surface::Empty, i)) ==
                Surface::Empty);
  }
  for (std::size_t i = 0; i < sidebar.items.size(); ++i) {
    PRISM_CHECK(
        prism::ctxmenu::SurfaceOfCommandId(prism::ctxmenu::CommandId(Surface::Sidebar, i)) ==
        Surface::Sidebar);
  }
  // ... and no id of one surface is even a valid slot of another.
  PRISM_CHECK(prism::ctxmenu::CommandId(Surface::Row, 0) !=
              prism::ctxmenu::CommandId(Surface::Empty, 0));
  PRISM_CHECK(prism::ctxmenu::CommandId(Surface::Empty, 0) !=
              prism::ctxmenu::CommandId(Surface::Sidebar, 0));

  // "Smart extract here" was NOT deleted with the old double-click meaning: it
  // is offered on all three surfaces, which is where the card moved it to.
  PRISM_CHECK(Contains(row, Command::SmartExtractHere));
  PRISM_CHECK(Contains(empty, Command::SmartExtractHere));
  PRISM_CHECK(Contains(sidebar, Command::SmartExtractHere));
}

PRISM_TEST("c92.every_disabled_item_carries_a_reason") {
  Facts bare;  // no archive, no selection: everything archive-scoped is greyed
  for (const Surface surface : {Surface::Row, Surface::Empty, Surface::Sidebar}) {
    const Menu menu = prism::ctxmenu::Build(surface, bare, Labels());
    PRISM_CHECK(menu.ReasonsOnEveryDisabledItem());
    for (const prism::ctxmenu::Item& item : menu.items) {
      if (item.enabled) {
        PRISM_CHECK(item.disabledReason.empty());
      } else {
        PRISM_CHECK(!item.disabledReason.empty());
      }
    }
    // With nothing to act on, the archive verbs are disabled -- not silently
    // absent. A menu that hides the verb cannot explain why it is unavailable.
    PRISM_CHECK(Contains(menu, Command::ExtractAll));
  }

  // The two facts that turn "Open" on: exactly one FILE selected.
  Facts file = RowFacts();
  const Menu openMenu = prism::ctxmenu::Build(Surface::Row, file, Labels());
  const prism::ctxmenu::Item* open = openMenu.Find(Command::Open);
  if (PRISM_CHECK(open != nullptr)) PRISM_CHECK(open->enabled);

  Facts folder = RowFacts();
  folder.singleFileSelected = false;
  folder.singleFolderSelected = true;
  const Menu folderMenu = prism::ctxmenu::Build(Surface::Row, folder, Labels());
  const prism::ctxmenu::Item* folderOpen = folderMenu.Find(Command::Open);
  if (PRISM_CHECK(folderOpen != nullptr)) {
    PRISM_CHECK(!folderOpen->enabled);
    PRISM_CHECK(!folderOpen->disabledReason.empty());
  }
}

PRISM_TEST("c92.both_tables_define_every_context_key") {
  prism::i18n::Translator en;
  prism::i18n::Translator zh;
  std::string why;
  const std::string base = std::string(PRISM_C92_LANG_DIR);
  if (!PRISM_CHECK(en.LoadCatalogFile(base + "/en-US.json", &why))) {
    std::fprintf(stderr, "en-US: %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(zh.LoadCatalogFile(base + "/zh-CN.json", &why))) {
    std::fprintf(stderr, "zh-CN: %s\n", why.c_str());
    return;
  }
  en.SetLanguage("en-US");
  zh.SetLanguage("zh-CN");

  const char* const keys[] = {
      "context.open",
      "context.extract.selected",
      "context.extract.all",
      "context.smart.extract.here",
      "context.copy.path",
      "context.reveal.archive",
      "context.select.all",
      "context.refresh",
      "context.disabled.no.selection",
      "context.disabled.not.a.file",
      "context.disabled.no.archive",
  };
  for (const char* key : keys) {
    if (!PRISM_CHECK(en.Has(key))) std::fprintf(stderr, "en-US lacks %s\n", key);
    if (!PRISM_CHECK(zh.Has(key))) std::fprintf(stderr, "zh-CN lacks %s\n", key);
  }

  // The model's label keys are exactly the table rows, and the Chinese table
  // really is Chinese (the English default must not leak into it).
  const Labels zhLabels = prism::ctxmenu::LabelsFrom(&zh);
  PRISM_CHECK_EQ(zhLabels.open, zh.Translate("context.open"));
  PRISM_CHECK(!zhLabels.open.empty());
  PRISM_CHECK(zhLabels.open != Labels().open);
  const Labels enLabels = prism::ctxmenu::LabelsFrom(&en);
  PRISM_CHECK_EQ(enLabels.open, std::string("Open"));
  PRISM_CHECK_EQ(enLabels.reasonNoArchive, en.Translate("context.disabled.no.archive"));
}

PRISM_TEST("c92.model_ids_are_stable_across_languages") {
  // A translated label must never move a command id: the id is STRUCTURE.
  prism::i18n::Translator zh;
  std::string why;
  const std::string base = std::string(PRISM_C92_LANG_DIR);
  if (!zh.LoadCatalogFile(base + "/zh-CN.json", &why)) return;
  zh.SetLanguage("zh-CN");
  const Menu english = prism::ctxmenu::Build(Surface::Row, RowFacts(), Labels());
  const Menu chinese = prism::ctxmenu::Build(Surface::Row, RowFacts(), prism::ctxmenu::LabelsFrom(&zh));
  PRISM_CHECK_EQ(english.items.size(), chinese.items.size());
  for (std::size_t i = 0; i < english.items.size(); ++i) {
    PRISM_CHECK(english.items[i].command == chinese.items[i].command);
  }
  PRISM_CHECK_EQ(SlotOf(english, Command::SmartExtractHere),
                 SlotOf(chinese, Command::SmartExtractHere));
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  // D1: CTest's PASS_REGULAR_EXPRESSION is this marker, so a suite that read
  // nothing and a suite that passed are not the same exit.
  std::cout << "ALL C92 INPUT-MENU CHECKS PASSED" << std::endl;
  return 0;
}
