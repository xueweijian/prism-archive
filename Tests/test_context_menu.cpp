// TDD: the context menu is a MODEL, Windows is just a view (blueprint ch. 4.1).
//
// First principles: the Win11 IExplorerCommand handler, the Win10 IContextMenu
// DLL and the in-app right-click menu must all render THE SAME decisions --
// which verbs exist, in which order, and when they are disabled. Those
// decisions are pure logic, so they live in PrismCore and are tested here;
// the Windows layers only translate model -> menu items.
#include "ContextMenuModel.hpp"
#include "mini_test.hpp"

#include <string>
#include <vector>

namespace {

std::vector<std::string> VerbsOf(const std::vector<prism::MenuCommand>& items) {
  std::vector<std::string> out;
  for (const auto& item : items) out.push_back(item.verb);
  return out;
}

bool Has(const std::vector<prism::MenuCommand>& items, const std::string& verb) {
  for (const auto& item : items) {
    if (item.verb == verb) return true;
  }
  return false;
}

PRISM_TEST("single archive: the five Bandizip verbs, in order") {
  const auto items = prism::ContextMenuModel::Build({"D:/dl/Design_Assets_v4.7z"});
  const auto verbs = VerbsOf(items);
  PRISM_CHECK_EQ(verbs.size(), size_t(5));
  PRISM_CHECK_EQ(verbs[0], std::string("smart.extract.here"));
  PRISM_CHECK_EQ(verbs[1], std::string("extract.here"));
  PRISM_CHECK_EQ(verbs[2], std::string("extract.to.subfolder"));
  PRISM_CHECK_EQ(verbs[3], std::string("test.integrity"));
  PRISM_CHECK_EQ(verbs[4], std::string("open.in.prism"));
  // The headline action is also the default (bold in Explorer terms).
  PRISM_CHECK(items.front().isDefault);
}

PRISM_TEST("single archive reports its capsule name for the subfolder verb") {
  const auto items = prism::ContextMenuModel::Build({"D:/dl/Design_Assets_v4.7z"});
  for (const auto& item : items) {
    if (item.verb == "extract.to.subfolder") {
      PRISM_CHECK_EQ(item.titleArgument, std::string("Design_Assets_v4"));
    }
  }
}

PRISM_TEST("multiple archives: per-archive smart extract replaces the single verbs") {
  const auto items =
      prism::ContextMenuModel::Build({"D:/dl/a.zip", "D:/dl/b.7z", "D:/dl/c.rar"});
  PRISM_CHECK(Has(items, "smart.extract.each.to.own.folder"));
  PRISM_CHECK(Has(items, "extract.here"));
  PRISM_CHECK(!Has(items, "extract.to.subfolder"));   // ambiguous with 3 roots
  PRISM_CHECK(Has(items, "test.integrity"));
}

PRISM_TEST("non-archive selection offers compression instead") {
  const auto items = prism::ContextMenuModel::Build({"D:/work/report.docx"});
  PRISM_CHECK(Has(items, "compress.to.7z"));
  PRISM_CHECK(Has(items, "compress.to.zip"));
  PRISM_CHECK(!Has(items, "extract.here"));
}

PRISM_TEST("mixed selection keeps both groups consistent") {
  // One archive + one regular file: the archive drives the extract group, the
  // regular file adds the compress group.
  const auto items = prism::ContextMenuModel::Build({"D:/dl/pack.zip", "D:/work/notes.txt"});
  PRISM_CHECK(Has(items, "smart.extract.here"));
  PRISM_CHECK(Has(items, "extract.here"));
  PRISM_CHECK(Has(items, "compress.to.7z"));
  PRISM_CHECK(Has(items, "compress.to.zip"));
  // ...but the multi-archive verb needs 2+ archives and must not appear.
  PRISM_CHECK(!Has(items, "smart.extract.each.to.own.folder"));
}

PRISM_TEST("empty selection produces no menu") {
  PRISM_CHECK(prism::ContextMenuModel::Build({}).empty());
}

}  // namespace

PRISM_TEST_MAIN()
