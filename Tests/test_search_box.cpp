// PrismArchive -- SEARCH BOX suite (card C62)
//
// The search field's MODEL is pure PrismCore, so every rule the window and the
// painter must not re-decide is asserted here in milliseconds, on every lane:
//
//   1. typing appends at the caret; the query is UTF-8 and the caret/selection
//      are BYTE offsets snapped to code-point boundaries (Backspace on a CJK
//      character removes the whole character, never one of its bytes);
//   2. Left/Right/Home/End move by code point, Shift extends, a plain arrow
//      collapses a selection to its edge, SelectAll + typing replaces it;
//   3. an IME composition is a PREVIEW: DisplayText shows it, Query() and the
//      match count do not change, arrow/Backspace belong to the IME, Commit
//      splices the committed string in, Cancel drops the preview;
//   4. Escape has three stages: cancel composition -> clear query -> close.
//      The two-stage (clear, then close) contract is the card's requirement;
//   5. filtering reuses C13's FilterModel: the query is the substring, the
//      visible list is catalog-ordered, the selection keeps its catalog
//      indices and hidden entries are COUNTED (hiddenSelection);
//   6. an EMPTY query is "no filter", not "matches nothing";
//   7. "无结果" / "过滤中 N 条" are exact byte strings and only appear in the
//      state that owns them;
//   8. the same input always yields the same visible list.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). The same source builds twice
// with ONE rule broken:
//   PRISM_C62_MUTANT_ESC_CLOSES_ON_FIRST    the first Escape closes the box
//   PRISM_C62_MUTANT_EMPTY_QUERY_FILTERS    an empty query hides every row
// Each arms SetMutationVerdict with a runtime predicate that re-runs the
// scenario, so the marker is printed at the FIRST red assertion and only when
// the injected defect really fired. No WILL_FAIL is stacked on a
// PASS_REGULAR_EXPRESSION (D26).
//
// ARM NAMES avoid clean / unarmed / control anywhere (D28).
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "SearchBoxModel.hpp"
#include "mini_test.hpp"

namespace {

using prism::list::Row;
using prism::list::TypeTagOf;
using prism::search::Effect;
using prism::search::Key;
using prism::search::KeyEvent;
using prism::search::Model;
using prism::search::Phase;
using prism::search::Status;

Row MakeRow(std::string name, uint32_t index, bool isDir = false) {
  Row row;
  row.catalogIndex = index;
  row.name = std::move(name);
  row.typeTag = TypeTagOf(row.name);
  row.size = 100;
  row.packSize = 50;
  row.isDir = isDir;
  if (isDir) {
    row.size = 0;
    row.packSize = 0;
    row.typeTag.clear();
  }
  return row;
}

// Catalog order matters: the filter must only remove rows, never reorder them.
std::vector<Row> Fixture() {
  return {
      MakeRow("年度报告.txt", 0),
      MakeRow("report.docx", 1),
      MakeRow("照片", 2, true),
      MakeRow("notes.md", 3),
      MakeRow("报告草稿.txt", 4),
  };
}

std::vector<uint32_t> Indices(std::initializer_list<uint32_t> values) {
  return std::vector<uint32_t>(values);
}

std::string Join(const std::vector<uint32_t>& values) {
  std::string out;
  for (uint32_t value : values) {
    if (!out.empty()) out += ',';
    out += std::to_string(value);
  }
  return out;
}

bool SameIndices(const std::vector<uint32_t>& actual, std::initializer_list<uint32_t> expected) {
  return actual == Indices(expected);
}

// ---------------------------------------------------------------------------
// D3 predicates: "did the injected defect actually fire?" They answer with a
// scenario, not with the compile flag, so an unrelated red can never print
// CAUGHT. [[maybe_unused]] because each is referenced only from its own mutant
// build and the GCC lane runs -Werror=unused-function (D31).
// ---------------------------------------------------------------------------
[[maybe_unused]] bool EscClosesOnFirstFired() {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("ab");
  model.Press(Key::Escape);
  return !model.IsOpen() && model.Query().empty();
}

[[maybe_unused]] bool EmptyQueryFiltersFired() {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  return model.VisibleIndices().size() != model.Rows().size();
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Typing / caret.
// ---------------------------------------------------------------------------
PRISM_TEST("search.typing_inserts_at_the_caret") {
  Model model;
  model.Open();
  const Effect typed = model.InsertText("abc");
  PRISM_CHECK_EQ(typed.kind, Effect::Kind::FilterChanged);
  PRISM_CHECK(typed.queryChanged);
  PRISM_CHECK(typed.consumed);
  PRISM_CHECK_EQ(model.Query(), std::string("abc"));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{3});
  PRISM_CHECK(!model.HasSelection());

  // Home then typing inserts at the FRONT; the caret follows the inserted run.
  PRISM_CHECK_EQ(model.Press(Key::Home).kind, Effect::Kind::Edited);
  model.InsertText("Z");
  PRISM_CHECK_EQ(model.Query(), std::string("Zabc"));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{1});
}

PRISM_TEST("search.utf8_caret_and_backspace_never_split_a_code_point") {
  Model model;
  model.Open();
  model.InsertText("报告.txt");  // 3 + 3 + 1 + 1 + 1 + 1 = 10 bytes
  PRISM_CHECK_EQ(model.Query().size(), std::size_t{10});
  PRISM_CHECK_EQ(model.Caret(), std::size_t{10});

  const Effect erased = model.Press(Key::Backspace);
  PRISM_CHECK_EQ(erased.kind, Effect::Kind::FilterChanged);
  PRISM_CHECK_EQ(model.Query(), std::string("报告.tx"));
  PRISM_CHECK_EQ(model.Query().size(), std::size_t{9});

  PRISM_CHECK_EQ(model.Press(Key::Home).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});
  PRISM_CHECK_EQ(model.Press(Key::Right).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{3});  // after 报, not inside it

  model.Press(Key::Backspace);
  PRISM_CHECK_EQ(model.Query(), std::string("告.tx"));
  PRISM_CHECK_EQ(model.Query().size(), std::size_t{6});
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});

  // A byte offset handed in from the outside snaps back to a boundary.
  model.SetCaret(1);  // inside 告 (bytes 0..2)
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});
  model.SetCaret(2);  // still inside 告
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});
  model.SetCaret(4);  // exactly after '.' (3 + 1), a real boundary
  PRISM_CHECK_EQ(model.Caret(), std::size_t{4});
}

PRISM_TEST("search.delete_forward_and_ctrl_backspace_respect_code_points") {
  Model model;
  model.Open();
  model.InsertText("报告.txt");  // 10 bytes
  model.Press(Key::Home);
  model.Press(Key::Right);  // caret 3: before 告
  const Effect deleted = model.Press(Key::Delete);
  PRISM_CHECK_EQ(deleted.kind, Effect::Kind::FilterChanged);
  PRISM_CHECK(deleted.queryChanged);
  PRISM_CHECK_EQ(model.Query(), std::string("报.txt"));  // whole 告, 7 bytes
  PRISM_CHECK_EQ(model.Query().size(), std::size_t{7});
  PRISM_CHECK_EQ(model.Caret(), std::size_t{3});

  // Delete at the end has nothing to remove and must not claim the key.
  model.Press(Key::End);
  PRISM_CHECK_EQ(model.Press(Key::Delete).kind, Effect::Kind::None);

  // Ctrl+Backspace removes one WORD (the whole last token), not one byte.
  model.SetQuery("alpha beta");
  PRISM_CHECK_EQ(model.Press(Key::Backspace, true).kind, Effect::Kind::FilterChanged);
  PRISM_CHECK_EQ(model.Query(), std::string("alpha "));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{6});
}

PRISM_TEST("search.select_all_and_typing_replaces") {
  Model model;
  model.Open();
  model.InsertText("abc");
  PRISM_CHECK(!model.HasSelection());
  const Effect selected = model.Press(Key::SelectAll);
  PRISM_CHECK_EQ(selected.kind, Effect::Kind::Edited);
  PRISM_CHECK(model.HasSelection());
  PRISM_CHECK_EQ(model.SelectionStart(), std::size_t{0});
  PRISM_CHECK_EQ(model.SelectionEnd(), std::size_t{3});

  model.InsertText("x");
  PRISM_CHECK_EQ(model.Query(), std::string("x"));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{1});
  PRISM_CHECK(!model.HasSelection());
}

PRISM_TEST("search.shift_extends_and_plain_arrow_collapses") {
  Model model;
  model.Open();
  model.InsertText("abcd");
  PRISM_CHECK_EQ(model.Press(Key::Left).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{3});
  PRISM_CHECK_EQ(model.Press(Key::Left, false, true).kind, Effect::Kind::Edited);
  PRISM_CHECK(model.HasSelection());
  PRISM_CHECK_EQ(model.SelectionStart(), std::size_t{2});
  PRISM_CHECK_EQ(model.SelectionEnd(), std::size_t{3});
  // A plain arrow collapses to the matching edge instead of moving two steps.
  PRISM_CHECK_EQ(model.Press(Key::Left).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{2});
  PRISM_CHECK(!model.HasSelection());
  // Shift+End from there selects "cd".
  model.Press(Key::End, false, true);
  PRISM_CHECK_EQ(model.SelectionStart(), std::size_t{2});
  PRISM_CHECK_EQ(model.SelectionEnd(), std::size_t{4});
  model.InsertText("X");
  PRISM_CHECK_EQ(model.Query(), std::string("abX"));
}

PRISM_TEST("search.home_end_and_word_movement") {
  Model model;
  model.Open();
  model.InsertText("alpha beta");
  PRISM_CHECK_EQ(model.Press(Key::Home).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});
  PRISM_CHECK_EQ(model.Press(Key::End).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{10});
  // Ctrl+Left jumps over the word "beta"; Ctrl+Left again over the space and
  // "alpha" -> 0.
  PRISM_CHECK_EQ(model.Press(Key::Left, true).kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{6});
  model.Press(Key::Left, true);
  PRISM_CHECK_EQ(model.Caret(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 2. IME composition.
// ---------------------------------------------------------------------------
PRISM_TEST("search.ime_composition_previews_without_filtering") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});

  model.BeginComposition();
  PRISM_CHECK(model.Composing());
  const Effect preview = model.UpdateComposition("报告");
  PRISM_CHECK_EQ(preview.kind, Effect::Kind::Edited);
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.DisplayText(), std::string("报告"));
  PRISM_CHECK_EQ(model.DisplayCaret(), std::size_t{6});
  // The preview does NOT filter: the count is still the "no query" count.
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
  PRISM_CHECK_EQ(model.GetStatus(), Status::Unfiltered);

  // Backspace and the arrows belong to the IME while composing.
  PRISM_CHECK_EQ(model.Press(Key::Backspace).kind, Effect::Kind::None);
  PRISM_CHECK(!model.Press(Key::Backspace).consumed);
  PRISM_CHECK_EQ(model.Press(Key::Left).kind, Effect::Kind::None);
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.Composition(), std::string("报告"));

  // Cancel drops the preview and filters nothing.
  const Effect cancelled = model.CancelComposition();
  PRISM_CHECK_EQ(cancelled.kind, Effect::Kind::Edited);
  PRISM_CHECK(!model.Composing());
  PRISM_CHECK_EQ(model.DisplayText(), std::string(""));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
}

PRISM_TEST("search.ime_commit_splices_the_text_and_filters") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.BeginComposition();
  model.UpdateComposition("baogao");
  const Effect committed = model.CommitComposition("报告");
  PRISM_CHECK_EQ(committed.kind, Effect::Kind::FilterChanged);
  PRISM_CHECK(committed.queryChanged);
  PRISM_CHECK(!model.Composing());
  PRISM_CHECK_EQ(model.Query(), std::string("报告"));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{6});
  PRISM_CHECK_EQ(model.GetStatus(), Status::Matches);
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{2});
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 4}));
  PRISM_CHECK_EQ(model.StatusText(), std::string("过滤中 2 条"));
}

PRISM_TEST("search.ime_composition_in_the_middle_replaces_the_selection") {
  Model model;
  model.Open();
  model.InsertText("年度.txt");
  model.Press(Key::Home);
  model.Press(Key::Right);               // after 年 (3 bytes)
  model.Press(Key::Right, false, true);  // Shift+Right selects 度 (3..6)
  PRISM_CHECK(model.HasSelection());
  PRISM_CHECK_EQ(model.SelectionStart(), std::size_t{3});
  PRISM_CHECK_EQ(model.SelectionEnd(), std::size_t{6});
  model.BeginComposition();
  model.UpdateComposition("报告");
  // The preview REPLACES the selected code point (度 disappears), and it still
  // does not touch Query(): commit is the only filter-visible transition.
  PRISM_CHECK_EQ(model.DisplayText(), std::string("年报告.txt"));
  PRISM_CHECK_EQ(model.Query(), std::string("年度.txt"));
  model.CommitComposition("报告");
  PRISM_CHECK_EQ(model.Query(), std::string("年报告.txt"));
  PRISM_CHECK_EQ(model.Caret(), std::size_t{9});  // 3 + 6 bytes
}

// ---------------------------------------------------------------------------
// 3. Escape: three staged semantics.
// ---------------------------------------------------------------------------
PRISM_TEST("search.escape_cancels_composition_first") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  model.BeginComposition();
  model.UpdateComposition("草稿");

  const Effect cancel = model.Press(Key::Escape);
  PRISM_CHECK_EQ(cancel.kind, Effect::Kind::Edited);
  PRISM_CHECK(model.IsOpen());
  PRISM_CHECK(!model.Composing());
  PRISM_CHECK_EQ(model.Query(), std::string("报告"));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{2});
}

PRISM_TEST("search.escape_clears_then_second_escape_closes") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  PRISM_CHECK_EQ(model.Query(), std::string("报告"));

  const Effect first = model.Press(Key::Escape);
  PRISM_CHECK_EQ(first.kind, Effect::Kind::FilterChanged);
  PRISM_CHECK(first.queryChanged);
  PRISM_CHECK(!first.closed);
  PRISM_CHECK(model.IsOpen());  // still open: the filter was cleared, not the box
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.GetStatus(), Status::Unfiltered);
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});

  const Effect second = model.Press(Key::Escape);
  PRISM_CHECK_EQ(second.kind, Effect::Kind::Closed);
  PRISM_CHECK(second.closed);
  PRISM_CHECK(!model.IsOpen());
  PRISM_CHECK_EQ(model.GetPhase(), Phase::Closed);
}

PRISM_TEST("search.escape_on_an_empty_box_closes_immediately") {
  Model model;
  model.Open();
  const Effect closed = model.Press(Key::Escape);
  PRISM_CHECK_EQ(closed.kind, Effect::Kind::Closed);
  PRISM_CHECK(!model.IsOpen());
}

// ---------------------------------------------------------------------------
// 4. Filter composition: query -> visible catalog indices.
// ---------------------------------------------------------------------------
PRISM_TEST("search.query_filters_in_catalog_order") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  PRISM_CHECK_EQ(model.GetStatus(), Status::Matches);
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 4}));

  // ASCII-folded substring, via C13's ContainsFold.
  model.SetQuery("REPORT");
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {1}));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{1});

  // UTF-8 bytes are compared exactly: a folder name still matches.
  model.SetQuery("照片");
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {2}));
}

PRISM_TEST("search.filter_keeps_selection_by_catalog_index_and_counts_hidden") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.SetSelection(Indices({0, 3}));
  model.InsertText("报告");
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 4}));
  // The selection is catalog INDICES, not row positions: 0 survives, 3 is hidden.
  PRISM_CHECK(SameIndices(model.Selection(), {0}));
  PRISM_CHECK_EQ(model.HiddenSelection(), std::size_t{1});
  PRISM_CHECK(model.Filter().IsVisible(4));
  PRISM_CHECK(!model.Filter().IsVisible(3));
}

PRISM_TEST("search.empty_query_does_not_filter") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.GetStatus(), Status::Unfiltered);
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 1, 2, 3, 4}));
  PRISM_CHECK_EQ(model.StatusText(), std::string(""));
  PRISM_CHECK_EQ(model.EmptyStateText(), std::string(""));
  // An empty query hides nothing even when a selection exists.
  model.SetSelection(Indices({1, 3}));
  PRISM_CHECK(SameIndices(model.Selection(), {1, 3}));
  PRISM_CHECK_EQ(model.HiddenSelection(), std::size_t{0});

  // ... and clearing a real query returns to that same unfiltered state.
  model.InsertText("报告");
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{2});
  model.ClearQuery();
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
  PRISM_CHECK(SameIndices(model.Selection(), {1, 3}));
}

PRISM_TEST("search.no_results_state_and_exact_texts") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("zzz");
  PRISM_CHECK_EQ(model.GetStatus(), Status::NoResults);
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{0});
  PRISM_CHECK(model.VisibleIndices().empty());
  // "无结果" is three UTF-8 code points / nine bytes -- asserted as BYTES.
  PRISM_CHECK_EQ(model.StatusText(), std::string("无结果"));
  PRISM_CHECK_EQ(model.StatusText().size(), std::size_t{9});
  PRISM_CHECK_EQ(model.EmptyStateText(), std::string("无结果"));
  PRISM_CHECK_EQ(model.EmptyStateText().size(), std::size_t{9});

  model.SetQuery("报告");
  // "过滤中 " + "2" + " 条" = 9 + 1 + 1 + 1 + 3 bytes.
  PRISM_CHECK_EQ(model.StatusText(), std::string("过滤中 2 条"));
  PRISM_CHECK_EQ(model.StatusText().size(), std::size_t{15});
  PRISM_CHECK_EQ(model.EmptyStateText(), std::string(""));
}

PRISM_TEST("search.same_input_always_yields_the_same_visible_list") {
  Model first;
  Model second;
  const std::vector<Row> rows = Fixture();
  first.SetRows(rows);
  second.SetRows(rows);
  first.Open();
  second.Open();
  for (const char* query : {"报", "报告", "报告草", "R", "REPORT", "zzz"}) {
    first.SetQuery(query);
    second.SetQuery(query);
    PRISM_CHECK(first.VisibleIndices() == second.VisibleIndices());
    PRISM_CHECK_EQ(first.MatchedCount(), second.MatchedCount());
    PRISM_CHECK_EQ(first.StatusText(), second.StatusText());
    // Recomputing (SetRows with the same catalog) is idempotent.
    first.SetRows(rows);
    PRISM_CHECK(first.VisibleIndices() == second.VisibleIndices());
  }
}

PRISM_TEST("search.catalog_change_keeps_the_query_and_recounts") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{2});
  std::vector<Row> rows = Fixture();
  rows.push_back(MakeRow("第三份报告.md", 5));
  model.SetRows(rows);
  PRISM_CHECK_EQ(model.Query(), std::string("报告"));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{3});
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 4, 5}));
}

// ---------------------------------------------------------------------------
// 5. Session / ignored keys.
// ---------------------------------------------------------------------------
PRISM_TEST("search.closed_model_ignores_every_key") {
  Model model;
  model.SetRows(Fixture());
  // Never opened: the box is not there, so nothing may type into it.
  PRISM_CHECK_EQ(model.InsertText("x").kind, Effect::Kind::None);
  PRISM_CHECK(!model.InsertText("x").consumed);
  PRISM_CHECK_EQ(model.Press(Key::Escape).kind, Effect::Kind::None);
  PRISM_CHECK_EQ(model.Press(Key::Enter).kind, Effect::Kind::None);
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
}

PRISM_TEST("search.enter_accepts_and_close_restores_the_unfiltered_list") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{2});
  const Effect accepted = model.Press(Key::Enter);
  PRISM_CHECK_EQ(accepted.kind, Effect::Kind::Accepted);
  PRISM_CHECK(accepted.accepted);
  PRISM_CHECK(model.IsOpen());  // Enter lets the window keep the box open
  PRISM_CHECK_EQ(model.Query(), std::string("报告"));

  model.Close();
  PRISM_CHECK(!model.IsOpen());
  PRISM_CHECK_EQ(model.Query(), std::string(""));
  PRISM_CHECK_EQ(model.MatchedCount(), std::size_t{5});
  PRISM_CHECK(SameIndices(model.VisibleIndices(), {0, 1, 2, 3, 4}));
}

PRISM_TEST("search.close_clears_the_query_and_open_can_select") {
  Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");
  model.Press(Key::Escape);  // clear (still open)
  model.InsertText("report");
  model.Close();
  // Close clears the query, so a reopen starts empty rather than restoring a
  // filter the user thought was gone.
  model.Open();
  PRISM_CHECK_EQ(model.Query(), std::string(""));

  // Open(selectAll=false) keeps the caret at the end.
  model.InsertText("abc");
  model.Close();
  model.Open(false);
  PRISM_CHECK_EQ(model.Query(), std::string(""));

  // And Open(true) over programmatic text selects it for replacement.
  model.SetQuery("abc");
  model.Open(true);
  PRISM_CHECK(model.HasSelection());
  PRISM_CHECK_EQ(model.SelectionStart(), std::size_t{0});
  PRISM_CHECK_EQ(model.SelectionEnd(), std::size_t{3});
}

int main(int argc, char** argv) {
#if defined(PRISM_C62_MUTANT_ESC_CLOSES_ON_FIRST)
  // Banner INSIDE the guard: the C23 matrix scans the 800 bytes after
  // "#if defined(...)" for the literal (CONTRACTS D28).
  std::cout << "C62 MUTANT ACTIVE: the first Escape closes the search box instead of "
               "clearing the query"
            << std::endl;
  prism::test::SetMutationVerdict("C62 MUTATION CAUGHT: esc_closes_on_first",
                                  [] { return EscClosesOnFirstFired(); });
  const char* mutant = "esc_closes_on_first";
#endif
#if defined(PRISM_C62_MUTANT_EMPTY_QUERY_FILTERS)
  std::cout << "C62 MUTANT ACTIVE: an empty query hides every row instead of filtering "
               "nothing"
            << std::endl;
  prism::test::SetMutationVerdict("C62 MUTATION CAUGHT: empty_query_filters",
                                  [] { return EmptyQueryFiltersFired(); });
  const char* mutant = "empty_query_filters";
#endif

  const int rc = prism::test::RunAll(argc, argv);
#if defined(PRISM_C62_MUTANT_ESC_CLOSES_ON_FIRST) || \
    defined(PRISM_C62_MUTANT_EMPTY_QUERY_FILTERS)
  // Reaching here with rc == 0 means the suite stayed GREEN with the defect
  // compiled in: the marker (the CTest verdict) is missing, so fail loudly
  // instead of printing the clean marker.
  if (rc == 0) {
    std::cout << "C62 MUTATION NOT CAUGHT: the suite stayed green with " << mutant
              << " compiled in" << std::endl;
  }
  return 1;
#else
  if (rc != 0) return rc;
  std::cout << "ALL SEARCH-BOX CHECKS PASSED" << std::endl;
  return 0;
#endif
}
