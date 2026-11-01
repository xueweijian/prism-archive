// PrismArchive -- PrismCore (card C52)
// DROP-INTO-THE-WINDOW, as a rule matrix on pure data.
//
// C20 proved the drag-OUT direction. This suite owns the other direction and
// nothing about it needs Windows: the plan is a function of
// (dragged tree, archive open?, archive read-only?).
//
// WHAT IS ASSERTED
//   1. the thirteen-ish combinations of that matrix, as the ACTION + the
//      REASON (never "it returned something");
//   2. the recursive-expansion rule: a dropped folder contributes the files
//      INSIDE it, in drag order, with the exact paths (D2: content, not a count
//      that a broken walk could still satisfy);
//   3. every refusal carries a named reason and the user-facing text names it
//      too, so the overlay can never say "nothing happened";
//   4. the hint rectangle is inside the content area for real layouts and empty
//      for a degenerate one;
//   5. the classifier agrees with the context-menu model about what an archive
//      is (two tables that drift would ship one behaviour in the menu and
//      another on a drop).
//
// NEGATIVE CONTROLS (D3 + D26 + D27)
//   Two mutation builds compile this same file:
//     PRISM_C52_MUTANT_READONLY_ALLOWS_ADD    a read-only archive accepts an add
//     PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION a folder arrives as one member
//   Each arms SetMutationVerdict with a predicate that proves the injection
//   really fired, so the binary prints "C52 MUTATION CAUGHT: ..." at the first
//   red assertion instead of after a teardown that may never run. The marker IS
//   the CTest verdict (no WILL_FAIL stacked on a regex).
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../Src/PrismCore/ContextMenuModel.hpp"
#include "../Src/PrismCore/DropPlan.hpp"
#include "../Src/PrismCore/WindowLayout.hpp"
#include "mini_test.hpp"

namespace drop = prism::drop;
using drop::ArchiveKind;
using drop::DropAction;
using drop::DropEntry;
using drop::DropInput;
using drop::DropPlan;
using drop::DropReason;

namespace {

DropEntry File(const char* path) {
  DropEntry entry;
  entry.path = path;
  entry.isDirectory = false;
  return entry;
}

DropEntry Folder(const char* path, std::vector<DropEntry> children = {}) {
  DropEntry entry;
  entry.path = path;
  entry.isDirectory = true;
  entry.children = std::move(children);
  return entry;
}

// The archive-open / read-only context is a pair of bools in every case below;
// naming it keeps a mis-ordered argument from silently testing the wrong row.
struct Context {
  bool open = false;
  bool readOnly = false;
};

DropInput Input(Context context, std::vector<DropEntry> entries, bool hintDirectory = false) {
  DropInput input;
  input.entries = std::move(entries);
  input.containsDirectory = hintDirectory;
  input.archiveOpen = context.open;
  input.archiveReadOnly = context.readOnly;
  return input;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Every accepted plan must say what it will do and must NOT carry a refusal
// reason; every refusal must name one. This is the C5 invariant restated for a
// drop and it is applied to every case below.
void CheckPlanShape(const DropPlan& plan) {
  PRISM_CHECK(plan.HasExplicitReason());
  PRISM_CHECK(!plan.headline.empty());
  PRISM_CHECK(!plan.detail.empty());
  if (plan.primary == DropAction::Refuse) {
    PRISM_CHECK(plan.reason != DropReason::None);
    PRISM_CHECK(Contains(plan.detail, drop::ToString(plan.reason)));
    PRISM_CHECK(plan.alternatives.empty());
    PRISM_CHECK(!plan.asksUser);
  } else {
    PRISM_CHECK(plan.reason == DropReason::None);
    if (plan.asksUser) PRISM_CHECK(!plan.alternatives.empty());
  }
}

// D3 predicates: they answer "did the injected defect actually fire?", which is
// what separates a CAUGHT mutation from a red run for an unrelated reason.
// [[maybe_unused]]: each predicate is referenced only from its own mutant build,
// and the GCC lane runs -Werror=unused-function (MSVC does not warn).
[[maybe_unused]] bool ReadOnlyInjectionFired() {
  const DropPlan plan = drop::PlanDrop(Input({true, true}, {File("C:/drop/note.txt")}));
  return plan.primary == DropAction::AddToArchive;
}

[[maybe_unused]] bool FlattenInjectionFired() {
  const DropPlan plan =
      drop::PlanDrop(Input({true, false},
                           {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt"),
                                                    File("C:/drop/pack/b.txt")})}));
  return plan.fileCount == 1 && plan.members.size() == 1 &&
         plan.members.front() == "C:/drop/pack";
}

}  // namespace

// ---------------------------------------------------------------------------
// The matrix
// ---------------------------------------------------------------------------
PRISM_TEST("drop_plan.empty_list_is_refused_with_a_named_reason") {
  for (const Context context : {Context{false, false}, Context{true, false}, Context{true, true}}) {
    const DropPlan plan = drop::PlanDrop(Input(context, {}));
    CheckPlanShape(plan);
    PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
    PRISM_CHECK_EQ(plan.reason, DropReason::EmptySelection);
    PRISM_CHECK_EQ(plan.fileCount, std::size_t(0));
  }
}

PRISM_TEST("drop_plan.single_container_opens_when_no_archive_is_open") {
  for (const char* name : {"C:/drop/photos.7z", "C:/drop/bundle.zip", "C:/drop/old.rar"}) {
    const DropPlan plan = drop::PlanDrop(Input({false, false}, {File(name)}));
    CheckPlanShape(plan);
    PRISM_CHECK_EQ(plan.primary, DropAction::OpenArchive);
    PRISM_CHECK_EQ(plan.reason, DropReason::None);
    PRISM_CHECK_EQ(plan.fileCount, std::size_t(1));
    PRISM_CHECK_EQ(plan.containerCount, std::size_t(1));
    PRISM_CHECK_EQ(plan.archiveCount, std::size_t(1));
    PRISM_CHECK(!plan.asksUser);  // one archive, one obvious reading
    if (!PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(1))) continue;
    PRISM_CHECK_EQ(plan.alternatives.front().action, DropAction::ExtractHere);
  }
}

PRISM_TEST("drop_plan.plain_file_with_no_archive_is_refused_as_not_an_archive") {
  const DropPlan plan = drop::PlanDrop(Input({false, false}, {File("C:/drop/notes.txt")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::NotAnArchive);
  // The refusal NAMES the file: "something is not an archive" is not actionable.
  PRISM_CHECK(Contains(plan.detail, "notes.txt"));
  PRISM_CHECK(Contains(plan.detail, "NotAnArchive"));
  PRISM_CHECK(Contains(plan.headline, "归档"));
}

PRISM_TEST("drop_plan.several_archives_ask_which_one_opens") {
  const DropPlan plan = drop::PlanDrop(
      Input({false, false}, {File("C:/drop/a.7z"), File("C:/drop/b.zip"), File("C:/drop/c.tar")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::OpenArchive);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(3));
  PRISM_CHECK_EQ(plan.archiveCount, std::size_t(3));
  PRISM_CHECK(plan.asksUser);
  if (PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(1))) {
    PRISM_CHECK_EQ(plan.alternatives.front().action, DropAction::ExtractHere);
  }
}

PRISM_TEST("drop_plan.single_stream_extracts_instead_of_opening") {
  for (const char* name : {"C:/drop/note.txt.gz", "C:/drop/blob.zst", "C:/drop/x.bz2"}) {
    const DropPlan plan = drop::PlanDrop(Input({false, false}, {File(name)}));
    CheckPlanShape(plan);
    PRISM_CHECK_EQ(plan.primary, DropAction::ExtractHere);
    PRISM_CHECK_EQ(plan.singleStreamCount, std::size_t(1));
    PRISM_CHECK_EQ(plan.containerCount, std::size_t(0));
    if (PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(1))) {
      PRISM_CHECK_EQ(plan.alternatives.front().action, DropAction::OpenArchive);
    }
  }
}

PRISM_TEST("drop_plan.several_single_streams_extract_and_ask") {
  const DropPlan plan =
      drop::PlanDrop(Input({false, false}, {File("C:/drop/a.gz"), File("C:/drop/b.zst")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::ExtractHere);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(2));
  PRISM_CHECK(plan.asksUser);
}

PRISM_TEST("drop_plan.plain_files_add_to_the_open_archive") {
  const DropPlan plan = drop::PlanDrop(
      Input({true, false}, {File("C:/drop/a.txt"), File("C:/drop/b.png")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::AddToArchive);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(2));
  PRISM_CHECK_EQ(plan.archiveCount, std::size_t(0));
  PRISM_CHECK(!plan.asksUser);  // adding is the only reading
  PRISM_CHECK(plan.alternatives.empty());
  PRISM_CHECK(Contains(plan.detail, "AddToArchive"));
}

PRISM_TEST("drop_plan.archive_onto_an_open_archive_asks_with_add_as_default") {
  const DropPlan plan =
      drop::PlanDrop(Input({true, false}, {File("C:/drop/inner.7z")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::AddToArchive);  // the drop landed ON an archive
  PRISM_CHECK(plan.asksUser);                              // ... but "open it" is equally plausible
  if (!PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(2))) return;
  PRISM_CHECK_EQ(plan.alternatives[0].action, DropAction::OpenArchive);
  PRISM_CHECK_EQ(plan.alternatives[1].action, DropAction::TestArchive);
}

PRISM_TEST("drop_plan.read_only_archive_refuses_an_add") {
  // The D3 negative control: with PRISM_C52_MUTANT_READONLY_ALLOWS_ADD this
  // becomes AddToArchive and the first assertion goes red.
  const DropPlan plan =
      drop::PlanDrop(Input({true, true}, {File("C:/drop/notes.txt")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::ReadOnlyArchive);
  PRISM_CHECK(Contains(plan.detail, "ReadOnlyArchive"));
  PRISM_CHECK(Contains(plan.detail, "只读"));
  PRISM_CHECK(Contains(plan.headline, "只读"));
}

PRISM_TEST("drop_plan.read_only_archive_refuses_a_folder_too") {
  const DropPlan plan = drop::PlanDrop(Input(
      {true, true}, {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt")})}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::ReadOnlyArchive);
}

PRISM_TEST("drop_plan.archive_onto_a_read_only_archive_tests_instead_of_adding") {
  const DropPlan plan =
      drop::PlanDrop(Input({true, true}, {File("C:/drop/inner.7z")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::TestArchive);
  PRISM_CHECK(plan.asksUser);
  if (!PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(2))) return;
  PRISM_CHECK_EQ(plan.alternatives[0].action, DropAction::OpenArchive);
  PRISM_CHECK_EQ(plan.alternatives[1].action, DropAction::ExtractHere);
  for (const drop::DropStep& step : plan.alternatives) PRISM_CHECK(step.action != DropAction::AddToArchive);
  PRISM_CHECK(Contains(plan.detail, "只读"));
}

PRISM_TEST("drop_plan.folder_is_expanded_recursively_in_drag_order") {
  // pack/{a.txt, sub/{b.txt}, c.txt}: the MEMBERS are the three files, in the
  // order the walk visited them -- the folder itself is never a member. Kill
  // the recursion (PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION) and this reds.
  const DropPlan plan = drop::PlanDrop(Input(
      {true, false},
      {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt"),
                               Folder("C:/drop/pack/sub", {File("C:/drop/pack/sub/b.txt")}),
                               File("C:/drop/pack/c.txt")})}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::AddToArchive);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(3));
  PRISM_CHECK_EQ(plan.directoryCount, std::size_t(2));
  PRISM_CHECK(plan.recursiveExpansion);
  if (!PRISM_CHECK_EQ(plan.members.size(), std::size_t(3))) return;
  PRISM_CHECK_EQ(plan.members[0], std::string("C:/drop/pack/a.txt"));
  PRISM_CHECK_EQ(plan.members[1], std::string("C:/drop/pack/sub/b.txt"));
  PRISM_CHECK_EQ(plan.members[2], std::string("C:/drop/pack/c.txt"));
  for (const std::string& member : plan.members) {
    PRISM_CHECK(member != std::string("C:/drop/pack"));
    PRISM_CHECK(member != std::string("C:/drop/pack/sub"));
  }
  PRISM_CHECK_EQ(plan.archiveCount, std::size_t(0));
}

PRISM_TEST("drop_plan.folder_without_an_open_archive_is_refused") {
  const DropPlan plan = drop::PlanDrop(
      Input({false, false}, {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt")})}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::NoArchiveOpen);
  PRISM_CHECK(Contains(plan.detail, "NoArchiveOpen"));
}

PRISM_TEST("drop_plan.empty_folder_is_refused_rather_than_added") {
  const DropPlan plan = drop::PlanDrop(Input({true, false}, {Folder("C:/drop/empty")}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::EmptySelection);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(0));
  PRISM_CHECK_EQ(plan.directoryCount, std::size_t(1));
}

PRISM_TEST("drop_plan.mixed_content_adds_when_open_and_refuses_when_not") {
  const std::vector<DropEntry> mixed = {File("C:/drop/notes.txt"), File("C:/drop/inner.7z")};
  const DropPlan adding = drop::PlanDrop(Input({true, false}, mixed));
  CheckPlanShape(adding);
  PRISM_CHECK_EQ(adding.primary, DropAction::AddToArchive);
  PRISM_CHECK_EQ(adding.fileCount, std::size_t(2));
  PRISM_CHECK_EQ(adding.archiveCount, std::size_t(1));
  PRISM_CHECK(!adding.asksUser);  // mixed intent is not ambiguous: add it all

  const DropPlan refused = drop::PlanDrop(Input({false, false}, mixed));
  CheckPlanShape(refused);
  PRISM_CHECK_EQ(refused.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(refused.reason, DropReason::NotAnArchive);

  const DropPlan readOnly = drop::PlanDrop(Input({true, true}, mixed));
  CheckPlanShape(readOnly);
  PRISM_CHECK_EQ(readOnly.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(readOnly.reason, DropReason::ReadOnlyArchive);
}

PRISM_TEST("drop_plan.member_bound_refuses_instead_of_truncating") {
  std::vector<DropEntry> many;
  for (int i = 0; i < 40; ++i) {
    many.push_back(File(("C:/drop/file" + std::to_string(i) + ".txt").c_str()));
  }
  DropInput input = Input({true, false}, many);
  input.maxMembers = 16;  // the bound is a policy input, so the test can lower it
  const DropPlan plan = drop::PlanDrop(input);
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::TooManyMembers);
  PRISM_CHECK(Contains(plan.detail, "16"));
}

PRISM_TEST("drop_plan.depth_bound_refuses_instead_of_truncating") {
  // A tree deeper than kMaxDepth must be REFUSED: silently adding part of it
  // would be data loss dressed up as success.
  DropEntry root = Folder("C:/drop/root");
  DropEntry* cursor = &root;
  for (int i = 0; i < prism::drop::kMaxDepth + 2; ++i) {
    cursor->children.push_back(Folder("C:/drop/level"));
    cursor = &cursor->children.back();
  }
  cursor->children.push_back(File("C:/drop/level/leaf.txt"));
  const DropPlan plan = drop::PlanDrop(Input({true, false}, {root}));
  CheckPlanShape(plan);
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::TooManyMembers);
}

PRISM_TEST("drop_plan.callers_directory_hint_never_overrides_the_tree") {
  // A stale hint is REPORTED, and the verdict still comes from the entries.
  const DropPlan lyingHint = drop::PlanDrop(Input(
      {true, false}, {File("C:/drop/notes.txt")}, /*hintDirectory=*/true));
  PRISM_CHECK(lyingHint.declaredDirectoryMismatch);
  PRISM_CHECK_EQ(lyingHint.primary, DropAction::AddToArchive);  // file rules, not folder rules
  PRISM_CHECK_EQ(lyingHint.directoryCount, std::size_t(0));

  const DropPlan hiddenFolder = drop::PlanDrop(Input(
      {true, false}, {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt")})},
      /*hintDirectory=*/false));
  PRISM_CHECK(hiddenFolder.declaredDirectoryMismatch);
  PRISM_CHECK_EQ(hiddenFolder.primary, DropAction::AddToArchive);
  PRISM_CHECK_EQ(hiddenFolder.directoryCount, std::size_t(1));
  PRISM_CHECK(hiddenFolder.recursiveExpansion);

  const DropPlan agreeing = drop::PlanDrop(Input(
      {true, false}, {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt")})},
      /*hintDirectory=*/true));
  PRISM_CHECK(!agreeing.declaredDirectoryMismatch);
}


PRISM_TEST("drop_plan.classifier_covers_every_extension_the_menu_model_knows") {
  // Two tables (the context menu's and this one) decide "is this an archive?".
  // Drift would ship one behaviour in the right-click menu and another on a
  // drop, so the drop table is checked AGAINST the menu model, name by name.
  const char* names[] = {"a.zip",  "a.7z",     "a.rar",     "a.tar",    "a.gz",
                         "a.xz",   "a.zst",    "a.bz2",     "a.tar.gz", "a.tar.zst",
                         "a.tgz",  "a.cab",    "a.iso",     "a.tar.bz2"};
  for (const char* name : names) {
    const std::string path = std::string("C:/drop/") + name;
    PRISM_CHECK(prism::ContextMenuModel::IsArchivePath(path));
    PRISM_CHECK(drop::Classify(path) != ArchiveKind::NotArchive);
  }
  PRISM_CHECK_EQ(drop::Classify("C:/drop/notes.txt"), ArchiveKind::NotArchive);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/photo.png"), ArchiveKind::NotArchive);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/README"), ArchiveKind::NotArchive);
  PRISM_CHECK_EQ(drop::Classify(""), ArchiveKind::NotArchive);
}

PRISM_TEST("drop_plan.classifier_reads_multi_part_suffixes_and_case") {
  // ExtensionOf() answers only the LAST dot component, so ".tar.gz" can only be
  // recognised by a suffix test. A .tar.gz is a CONTAINER (a tar inside gzip),
  // not a single stream -- getting this wrong would offer "extract" for an
  // archive the gallery can browse.
  PRISM_CHECK_EQ(drop::Classify("C:/drop/MOVIE.TAR.GZ"), ArchiveKind::Container);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/movie.tar.zst"), ArchiveKind::Container);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/PHOTOS.7Z"), ArchiveKind::Container);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/note.txt.ZST"), ArchiveKind::SingleStream);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/pack.tgz"), ArchiveKind::Container);
  PRISM_CHECK_EQ(drop::Classify("C:/drop/pack/thing.7Z"), ArchiveKind::Container);
}

PRISM_TEST("drop_plan.hint_rect_stays_inside_the_content_area") {
  struct Case {
    int width;
    int height;
    bool sidebar;
    bool listMode;
  };
  const Case cases[] = {{1280, 800, false, false}, {1280, 800, true, true},
                        {900, 600, false, true},   {640, 480, true, false},
                        {320, 200, false, false}};
  for (const Case& c : cases) {
    const prism::layout::Layout layout =
        prism::layout::Compute(c.width, c.height, c.sidebar, c.listMode);
    const prism::layout::Rect hint = drop::HintRect(layout.contentInner);
    if (!PRISM_CHECK(!hint.Empty())) continue;
    // The hint is a band INSIDE the content: it must never invite a click on
    // the action band, the status bar or a rail that it does not cover.
    PRISM_CHECK(layout.contentInner.Contains(hint));
    PRISM_CHECK(layout.content.Contains(hint));
    PRISM_CHECK(!hint.Intersects(layout.actionIsland));
    PRISM_CHECK(!hint.Intersects(layout.statusBar));
    PRISM_CHECK(hint.Height() <= drop::kHintMaxHeight);
    PRISM_CHECK(hint.Height() > 0);
    PRISM_CHECK(hint.Width() > 0);
    // Centred vertically inside the padded content, not pinned to an edge.
    const prism::layout::Rect inner = prism::layout::Inset(layout.contentInner, drop::kHintPadding);
    PRISM_CHECK(hint.top >= inner.top);
    PRISM_CHECK(hint.bottom <= inner.bottom);
  }
  // A degenerate client has no content area: the hint degrades to "nothing to
  // paint" instead of an inverted rectangle.
  const prism::layout::Layout tiny = prism::layout::Compute(40, 30, false, false);
  PRISM_CHECK(drop::HintRect(tiny.contentInner).Empty());
}

PRISM_TEST("drop_plan.every_matrix_row_keeps_the_plan_invariants") {
  const std::vector<std::vector<DropEntry>> rows = {
      {},
      {File("C:/drop/notes.txt")},
      {File("C:/drop/inner.7z")},
      {File("C:/drop/note.txt.gz")},
      {File("C:/drop/a.7z"), File("C:/drop/b.zip")},
      {File("C:/drop/notes.txt"), File("C:/drop/inner.7z")},
      {Folder("C:/drop/pack", {File("C:/drop/pack/a.txt")})},
      {Folder("C:/drop/empty")},
      {Folder("C:/drop/pack", {Folder("C:/drop/pack/sub", {File("C:/drop/pack/sub/x.gz")})})},
  };
  const Context contexts[] = {{false, false}, {true, false}, {true, true}};
  for (const std::vector<DropEntry>& entries : rows) {
    for (const Context& context : contexts) {
      const DropPlan plan = drop::PlanDrop(Input(context, entries));
      CheckPlanShape(plan);
      PRISM_CHECK_EQ(plan.members.size(), plan.fileCount);
      PRISM_CHECK_EQ(plan.primary == DropAction::Refuse, !plan.accepted());
    }
  }
}

PRISM_TEST("drop_plan.route_table_is_complete") {
  // The one-line summary of the whole rule set, so a reader (and a reviewer)
  // can see the matrix without reading every case above.
  struct Row {
    bool open;
    bool readOnly;
    std::vector<DropEntry> entries;
    DropAction expected;
  };
  const std::vector<Row> rows = {
      {false, false, {File("C:/a.7z")}, DropAction::OpenArchive},
      {false, false, {File("C:/a.txt")}, DropAction::Refuse},
      {false, false, {File("C:/a.gz")}, DropAction::ExtractHere},
      {false, false, {Folder("C:/d", {File("C:/d/f.txt")})}, DropAction::Refuse},
      {true, false, {File("C:/a.txt")}, DropAction::AddToArchive},
      {true, false, {File("C:/a.7z")}, DropAction::AddToArchive},
      {true, false, {Folder("C:/d", {File("C:/d/f.txt")})}, DropAction::AddToArchive},
      {true, true, {File("C:/a.txt")}, DropAction::Refuse},
      {true, true, {File("C:/a.7z")}, DropAction::TestArchive},
      {true, true, {Folder("C:/d", {File("C:/d/f.txt")})}, DropAction::Refuse},
  };
  for (const Row& row : rows) {
    const DropPlan plan = drop::PlanDrop(Input({row.open, row.readOnly}, row.entries));
    PRISM_CHECK_EQ(plan.primary, row.expected);
  }
}

int main(int argc, char** argv) {
#if defined(PRISM_C52_MUTANT_READONLY_ALLOWS_ADD)
  std::cout << "C52 MUTANT ACTIVE: a read-only archive accepts AddToArchive" << std::endl;
  // D27 + D3: the marker prints at the FIRST red assertion and only while the
  // predicate proves the injected defect really fired (exit 0 is the harness's;
  // CTest decides with PASS_REGULAR_EXPRESSION, D26).
  prism::test::SetMutationVerdict("C52 MUTATION CAUGHT: readonly_allows_add",
                                  [] { return ReadOnlyInjectionFired(); });
#endif
#if defined(PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION)
  std::cout << "C52 MUTANT ACTIVE: a dropped folder is not expanded recursively" << std::endl;
  prism::test::SetMutationVerdict("C52 MUTATION CAUGHT: no_recursive_expansion",
                                  [] { return FlattenInjectionFired(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL DROP-PLAN CHECKS PASSED" << std::endl;
  return 0;
}
