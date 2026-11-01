// PrismArchive -- PrismCore test
// Command palette ranking (blueprint ch. 9.1 row 3: Ctrl+K fuzzy search over
// in-archive paths and commands).
//
// The assertions are EXACT ORDERINGS, not "it found something" (CONTRACTS C4:
// same input, same output). Each expected order is annotated with the score
// components that produce it, so a future scoring change has to argue with the
// numbers instead of quietly reordering the palette.
#include <cstddef>
#include <string>
#include <vector>

#include "../Src/PrismCore/ArchiveCatalog.hpp"
#include "../Src/PrismCore/CommandPalette.hpp"
#include "mini_test.hpp"

using prism::ArchiveItem;
using prism::palette::Entry;
using prism::palette::Kind;
using prism::palette::Model;

namespace {

void Add(std::vector<ArchiveItem>* items, const char* path, bool dir, uint64_t size) {
  ArchiveItem item;
  item.index = static_cast<uint32_t>(items->size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  items->push_back(item);
}

// A structured archive: nested folders, images at three depths, a folder whose
// name starts with the query (prefix bonus), and a CJK path.
std::vector<ArchiveItem> BuildCatalog() {
  std::vector<ArchiveItem> items;
  Add(&items, "docs", true, 0);                 // 0
  Add(&items, "pack", true, 0);                 // 1
  Add(&items, "pack/img1.png", false, 120);     // 2
  Add(&items, "pack/note.txt", false, 30);      // 3
  Add(&items, "pack/sub", true, 0);             // 4
  Add(&items, "pack/sub/deep.png", false, 60);  // 5
  Add(&items, "root.png", false, 500);          // 6
  Add(&items, "png_backup", true, 0);           // 7
  Add(&items, "png_backup/x.png", false, 10);   // 8
  Add(&items, "文档", true, 0);                  // 9
  Add(&items, "文档/说明.txt", false, 42);       // 10
  return items;
}

char Fold(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

const Entry& At(const Model& model, size_t entry) { return model.entries()[entry]; }

std::vector<std::string> Titles(const Model& model,
                                const std::vector<prism::palette::Match>& results) {
  std::vector<std::string> titles;
  titles.reserve(results.size());
  for (const auto& match : results) titles.push_back(At(model, match.entry).title);
  return titles;
}

}  // namespace

PRISM_TEST("palette: an empty query lists commands first, in declaration order") {
  Model model;
  model.SetCommands(prism::palette::DefaultCommands());
  model.SetCatalog(BuildCatalog());
  const auto& commands = prism::palette::DefaultCommands();
  if (!PRISM_CHECK_EQ(model.entries().size(),
                      commands.size() + BuildCatalog().size())) return;

  const auto results = model.Query("");
  if (!PRISM_CHECK_EQ(results.size(), model.entries().size())) return;
  for (size_t i = 0; i < commands.size(); ++i) {
    if (!PRISM_CHECK_EQ(At(model, results[i].entry).kind, Kind::Command)) return;
    if (!PRISM_CHECK_EQ(At(model, results[i].entry).command, commands[i].command)) return;
    if (!PRISM_CHECK_EQ(results[i].score, 0)) return;
    if (!PRISM_CHECK(results[i].positions.empty())) return;
  }
  // The first file row follows the whole command block, in catalog order.
  if (!PRISM_CHECK_EQ(At(model, results[commands.size()].entry).title, std::string("docs"))) return;
  if (!PRISM_CHECK_EQ(At(model, results[commands.size()].entry).kind, Kind::File)) return;

  // A query of spaces is an EMPTY query (typing two spaces must not blank the
  // list the user is looking at).
  const auto spaces = model.Query("   ");
  if (!PRISM_CHECK_EQ(spaces.size(), results.size())) return;
  for (size_t i = 0; i < spaces.size(); ++i) {
    if (!PRISM_CHECK_EQ(spaces[i].entry, results[i].entry)) return;
  }
  // ... and the limit truncates the flat list from the top.
  const auto limited = model.Query("", 2);
  if (!PRISM_CHECK_EQ(limited.size(), size_t{2})) return;
  if (!PRISM_CHECK_EQ(At(model, limited[0].entry).command, prism::keys::Command::SmartExtract)) return;
  if (!PRISM_CHECK_EQ(At(model, limited[1].entry).command, prism::keys::Command::NewFolder)) return;
}

PRISM_TEST("palette: prefix beats name-boundary, shorter title breaks the tie") {
  Model model;
  model.SetCommands(prism::palette::DefaultCommands());
  model.SetCatalog(BuildCatalog());
  const auto results = model.Query("png");
  const std::vector<std::string> expected = {
      "png_backup",           // 110 + 40 prefix          (10 chars)
      "png_backup/x.png",     // 110 + 40 prefix          (16 chars)
      "root.png",             // 30 + 20 boundary + 60    (8 chars)
      "pack/img1.png",        // same score, longer title (13 chars)
      "pack/sub/deep.png",    // same score, longest      (18 chars)
  };
  const std::vector<std::string> actual = Titles(model, results);
  if (!PRISM_CHECK_EQ(actual.size(), expected.size())) return;
  for (size_t i = 0; i < expected.size(); ++i) {
    if (!PRISM_CHECK_EQ(actual[i], expected[i])) return;
  }
  // Equal-score group really is equal: the tie is broken by the documented
  // chain, not by an accident of the sort.
  if (!PRISM_CHECK_EQ(results[2].score, results[4].score)) return;
  if (!PRISM_CHECK(results[0].score > results[2].score)) return;
}

PRISM_TEST("palette: the boundary bonus decides a near tie") {
  // "mg" against "m_g.png" (both matched characters start words) versus
  // "img.png" (contiguous but mid-word). Either signal alone is not enough:
  // the boundary bonus is what prefers the filename that looks like words.
  std::vector<ArchiveItem> items;
  Add(&items, "img.png", false, 1);
  Add(&items, "m_g.png", false, 1);
  Model model;
  model.SetCatalog(items);
  const int boundary = Model::Score("m_g.png", "mg");  // 30 + (10+20-8) = 52
  const int contiguous = Model::Score("img.png", "mg");  // 10 + (10+30) = 50
  if (!PRISM_CHECK_EQ(boundary, 52)) return;
  if (!PRISM_CHECK_EQ(contiguous, 50)) return;
  const auto results = model.Query("mg");
  if (!PRISM_CHECK_EQ(results.size(), size_t{2})) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).title, std::string("m_g.png"))) return;
  if (!PRISM_CHECK_EQ(At(model, results[1].entry).title, std::string("img.png"))) return;
}

PRISM_TEST("palette: a command wins a score tie against a file") {
  std::vector<ArchiveItem> items;
  Add(&items, "grid.png", false, 1);
  Model model;
  model.SetCommands(prism::palette::DefaultCommands());
  model.SetCatalog(items);
  const auto results = model.Query("grid");
  if (!PRISM_CHECK_EQ(results.size(), size_t{2})) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).kind, Kind::Command)) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).command, prism::keys::Command::ToggleGrid)) return;
  if (!PRISM_CHECK_EQ(At(model, results[1].entry).title, std::string("grid.png"))) return;
  if (!PRISM_CHECK_EQ(results[0].score, results[1].score)) return;  // a real tie
}

PRISM_TEST("palette: commands are searchable by label and carry their hint") {
  Model model;
  model.SetCommands(prism::palette::DefaultCommands());
  const auto results = model.Query("new");
  if (!PRISM_CHECK_EQ(results.size(), size_t{1})) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).title, std::string("New Folder"))) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).subtitle, std::string("Ctrl+Shift+N"))) return;
  // "esm" is a subsequence of "Extract Smart" but not a prefix: it still hits.
  const auto fuzzy = model.Query("esm");
  if (!PRISM_CHECK(!fuzzy.empty())) return;
  if (!PRISM_CHECK_EQ(At(model, fuzzy[0].entry).command, prism::keys::Command::SmartExtract)) return;
}

PRISM_TEST("palette: matching is case-insensitive and subsequence-based") {
  Model model;
  model.SetCatalog(BuildCatalog());
  const auto upper = model.Query("PNG");
  const auto lower = model.Query("png");
  if (!PRISM_CHECK_EQ(upper.size(), lower.size())) return;
  for (size_t i = 0; i < upper.size(); ++i) {
    if (!PRISM_CHECK_EQ(upper[i].entry, lower[i].entry)) return;
    if (!PRISM_CHECK_EQ(upper[i].score, lower[i].score)) return;
  }
  if (!PRISM_CHECK(Model::Matches("root.png", "rng"))) return;   // r..n.g
  if (!PRISM_CHECK(!Model::Matches("root.png", "rnp"))) return;  // p is BEFORE n
  if (!PRISM_CHECK(!Model::Matches("root.png", "pnr"))) return;  // order matters
  if (!PRISM_CHECK_EQ(Model::Score("root.png", "zzz"), -1)) return;
  if (!PRISM_CHECK(model.Query("zzz").empty())) return;
}

PRISM_TEST("palette: CJK paths match by byte subsequence with byte positions") {
  Model model;
  model.SetCatalog(BuildCatalog());
  const auto results = model.Query("说明");
  if (!PRISM_CHECK_EQ(results.size(), size_t{1})) return;
  const Entry& entry = At(model, results[0].entry);
  if (!PRISM_CHECK_EQ(entry.title, std::string("文档/说明.txt"))) return;
  if (!PRISM_CHECK_EQ(entry.index, 10u)) return;
  if (!PRISM_CHECK_EQ(results[0].positions.size(), size_t{6})) return;  // 2 CJK chars
  // "文档" is 6 bytes, the separator is 1 -> the name starts at byte 7.
  if (!PRISM_CHECK_EQ(results[0].positions.front(), size_t{7})) return;
  for (size_t i = 0; i < results[0].positions.size(); ++i) {
    if (!PRISM_CHECK_EQ(results[0].positions[i], size_t{7} + i)) return;
  }
}

PRISM_TEST("palette: a folder component is enough to find a deep file") {
  Model model;
  model.SetCatalog(BuildCatalog());
  const auto results = model.Query("deep");
  if (!PRISM_CHECK_EQ(results.size(), size_t{1})) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).title, std::string("pack/sub/deep.png"))) return;
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).index, 5u)) return;
  // The subtitle is what the shell shows on the right of the row.
  if (!PRISM_CHECK_EQ(At(model, results[0].entry).subtitle, std::string("60 B"))) return;
}

PRISM_TEST("palette: limit truncates, order is deterministic, positions are valid") {
  Model model;
  model.SetCommands(prism::palette::DefaultCommands());
  model.SetCatalog(BuildCatalog());
  const auto full = model.Query("png");
  const auto capped = model.Query("png", 2);
  if (!PRISM_CHECK_EQ(capped.size(), size_t{2})) return;
  for (size_t i = 0; i < capped.size(); ++i) {
    if (!PRISM_CHECK_EQ(capped[i].entry, full[i].entry)) return;
  }
  // Same input twice -> identical output, including highlight offsets (C4).
  const auto again = model.Query("png");
  if (!PRISM_CHECK_EQ(again.size(), full.size())) return;
  for (size_t i = 0; i < full.size(); ++i) {
    if (!PRISM_CHECK_EQ(again[i].entry, full[i].entry)) return;
    if (!PRISM_CHECK_EQ(again[i].score, full[i].score)) return;
    if (!PRISM_CHECK_EQ(again[i].positions.size(), full[i].positions.size())) return;
    for (size_t k = 0; k < full[i].positions.size(); ++k) {
      if (!PRISM_CHECK_EQ(again[i].positions[k], full[i].positions[k])) return;
    }
  }
  // Every highlight must be a real, strictly increasing, spelling alignment,
  // and the trace score must agree with the allocation-free ranker.
  const char* queries[] = {"png", "esm", "deep", "sub", "rnp"};
  for (const char* query : queries) {
    for (const auto& match : model.Query(query)) {
      const std::string& title = At(model, match.entry).title;
      const auto aligned = Model::AlignMatch(title, query);
      if (!PRISM_CHECK_EQ(aligned.score, match.score)) return;
      if (!PRISM_CHECK_EQ(aligned.score, Model::Score(title, query))) return;
      if (!PRISM_CHECK_EQ(aligned.positions.size(), std::string(query).size())) return;
      size_t previous = 0;
      for (size_t i = 0; i < aligned.positions.size(); ++i) {
        const size_t pos = aligned.positions[i];
        if (!PRISM_CHECK(pos < title.size())) return;
        if (i > 0 && !PRISM_CHECK(pos > previous)) return;
        if (!PRISM_CHECK_EQ(Fold(title[pos]), Fold(query[i]))) return;
        previous = pos;
      }
    }
  }
}

PRISM_TEST_MAIN()
