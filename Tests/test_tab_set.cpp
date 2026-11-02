// PrismArchive -- TAB SET suite (card C59)
//
// The tab strip's MODEL is pure PrismCore, so every rule the next card must not
// re-decide is asserted here in milliseconds, on every lane:
//
//   1. opening appends a tab, normalizes the path and activates it;
//   2. one archive == one tab: reopening activates the EXISTING tab (compared on
//      the normalized path, case-insensitively and separator-insensitively);
//   3. the eighth..sixteenth tab is accepted, the SEVENTEENTH is REFUSED with a
//      reason -- never silently dropped;
//   4. closing the ACTIVE tab activates its RIGHT neighbour, or the LEFT one
//      when there is no right neighbour (the tab that slid into its place);
//   5. closing the LAST tab is an empty state, not a shutdown;
//   6. a DIRTY tab answers needsConfirm and changes nothing until confirmed;
//   7. closing a BACKGROUND tab keeps the user on the tab they were reading;
//   8. MRU order: activation moves a tab to the front, a reorder does not;
//   9. the C46 session projection is MRU-ordered with the active archive first,
//      and ToSession(FromSession(p)) == p (round trip, asserted by CONTENT);
//  10. a projection longer than C46's ceiling reports how many tabs it could not
//      hold, and the active archive is never one of them;
//  11. restoring a document drops blank / duplicate / non-archive / overflow
//      entries and REPORTS each drop.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27)
//   Two more executables compile THIS file with one rule switched off:
//     PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR  "closing always activates tab 0"
//     PRISM_C59_MUTANT_DEDUP_OPEN            "reopening appends a second tab"
//   Each arms SetMutationVerdict WITH A PREDICATE that re-runs the scenario and
//   proves the injected defect really fired, so the marker is printed at the
//   FIRST red assertion (not after a teardown that may never run) and a red that
//   came from something else never claims CAUGHT. The marker IS the CTest
//   verdict: no WILL_FAIL is stacked on a PASS_REGULAR_EXPRESSION (D26).
//
// ARM NAMES avoid clean / unarmed / control anywhere (D28: those words make the
// C23 matrix read an entry as a CONTROL, and an armed control is a contradiction).
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../Src/PrismCore/TabSet.hpp"
#include "mini_test.hpp"

namespace tabs = prism::tabs;
namespace session = prism::session;

namespace {

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// Three tabs, in bar order A, B, C, with the given one active.
tabs::TabSet ThreeTabs(int activeIndex) {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  set.Open("C:/b.7z");
  set.Open("C:/c.7z");
  set.Activate(activeIndex);
  return set;
}

// D3 predicates: "did the injected defect actually fire?" -- this is what makes
// a CAUGHT verdict honest instead of "the suite went red for some reason".
// [[maybe_unused]]: each is referenced only from its own mutant build, and the
// GCC lane runs -Werror=unused-function (MSVC does not warn).
[[maybe_unused]] bool CloseNeighbourInjectionFired() {
  tabs::TabSet set = ThreeTabs(2);   // C active: the LAST tab
  set.Close(2, false);               // clean build: B (left neighbour)
  const tabs::Tab* active = set.active();
  return active != nullptr && active->path == std::string("C:/a.7z");  // mutated: tab 0
}

[[maybe_unused]] bool DedupInjectionFired() {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  set.Open("C:/a.7z");
  return set.size() == std::size_t{2};
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Open: a tab, its normalized path, its place on the strip.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.opening_appends_a_tab_and_activates_it") {
  tabs::TabSet set;
  tabs::OpenRequest request;
  request.path = "C:\\work\\photos.7z";            // separators are normalized
  request.view.folder = "\\2024\\trip\\";          // ... and so is the landing folder
  request.view.listMode = true;

  const tabs::OpenOutcome first = set.Open(request);
  PRISM_CHECK_EQ(first.verdict, tabs::TabVerdict::kOk);
  PRISM_CHECK(first.created);
  PRISM_CHECK(!first.activatedExisting);
  PRISM_CHECK_EQ(first.index, 0);
  PRISM_CHECK_EQ(first.path, std::string("C:/work/photos.7z"));
  PRISM_CHECK_EQ(set.size(), std::size_t{1});
  PRISM_CHECK_EQ(set.activeIndex(), 0);
  const tabs::Tab* active = set.active();
  if (!PRISM_CHECK(active != nullptr)) return;
  PRISM_CHECK_EQ(active->path, std::string("C:/work/photos.7z"));
  PRISM_CHECK_EQ(active->displayName, std::string("photos.7z"));
  PRISM_CHECK_EQ(active->view.folder, std::string("2024/trip"));
  PRISM_CHECK_EQ(active->view.listMode, true);
  PRISM_CHECK(!active->dirty);

  const tabs::OpenOutcome second = set.Open("D:/archive.zip");
  PRISM_CHECK(second.created);
  PRISM_CHECK_EQ(second.index, 1);
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
  PRISM_CHECK_EQ(set.activeIndex(), 1);
  // Bar order is append order, not recency: the first tab stays first.
  if (!PRISM_CHECK_EQ(set.size(), std::size_t{2})) return;
  PRISM_CHECK_EQ(set.At(0)->displayName, std::string("photos.7z"));
  PRISM_CHECK_EQ(set.At(1)->displayName, std::string("archive.zip"));
}

// ---------------------------------------------------------------------------
// 2. ONE archive == ONE tab (the second negative control's target).
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.reopening_an_open_archive_activates_it_instead_of_duplicating") {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  set.Open("C:/b.7z");
  PRISM_CHECK_EQ(set.activeIndex(), 1);

  const tabs::OpenOutcome again = set.Open("C:/a.7z");
  PRISM_CHECK_EQ(again.verdict, tabs::TabVerdict::kOk);
  PRISM_CHECK(!again.created);
  PRISM_CHECK(again.activatedExisting);
  PRISM_CHECK_EQ(again.index, 0);
  PRISM_CHECK_EQ(set.size(), std::size_t{2});        // still two tabs, not three
  PRISM_CHECK_EQ(set.activeIndex(), 0);
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/a.7z"));
}

PRISM_TEST("tabs.identity_is_case_and_separator_insensitive") {
  tabs::TabSet set;
  set.Open("C:/Work/Photos.7Z");
  PRISM_CHECK_EQ(set.size(), std::size_t{1});

  // Different case, backslashes, doubled separators, a trailing separator: the
  // same archive on Windows, therefore the same tab.
  const tabs::OpenOutcome again = set.Open("c:\\\\work\\\\\\photos.7z\\");
  PRISM_CHECK(again.activatedExisting);
  PRISM_CHECK(!again.created);
  PRISM_CHECK_EQ(set.size(), std::size_t{1});
  PRISM_CHECK_EQ(set.IndexOf("C:/WORK/photos.7z"), 0);
  // The tab keeps the spelling it was opened with (the title bar shows that one).
  PRISM_CHECK_EQ(set.At(0)->path, std::string("C:/Work/Photos.7Z"));
  // ... and non-ASCII bytes are NOT case-folded into garbage.
  set.Open("C:/\xE5\xBD\x92\xE6\xA1\xA3.7z");
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
  PRISM_CHECK_EQ(set.IndexOf("c:/work/PHOTOS.7z"), 0);
}

// ---------------------------------------------------------------------------
// 3. The ceiling: refused with a reason, never silently dropped.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.seventeenth_open_is_rejected_with_a_named_reason") {
  tabs::TabSet set;
  for (std::size_t i = 0; i < tabs::kMaxTabs; ++i) {
    const tabs::OpenOutcome opened =
        set.Open("C:/arch/" + std::to_string(i) + ".7z");
    PRISM_CHECK_EQ(opened.verdict, tabs::TabVerdict::kOk);
    PRISM_CHECK(opened.created);
  }
  PRISM_CHECK_EQ(tabs::kMaxTabs, std::size_t{16});
  PRISM_CHECK_EQ(set.size(), std::size_t{16});

  // An archive that is ALREADY open is still reachable at the ceiling: the
  // de-duplication branch runs before the limit, so the user never loses access
  // to a tab because the strip is full.
  const tabs::OpenOutcome existing = set.Open("C:/arch/3.7z");
  PRISM_CHECK(existing.activatedExisting);
  PRISM_CHECK_EQ(set.size(), std::size_t{16});

  const tabs::OpenOutcome refused = set.Open("C:/arch/16.7z");
  PRISM_CHECK_EQ(refused.verdict, tabs::TabVerdict::kLimitReached);
  PRISM_CHECK(!refused.created);
  PRISM_CHECK(!refused.activatedExisting);
  PRISM_CHECK_EQ(refused.index, -1);
  PRISM_CHECK_EQ(set.size(), std::size_t{16});          // nothing vanished
  PRISM_CHECK_EQ(set.IndexOf("C:/arch/16.7z"), -1);     // and nothing appeared
  PRISM_CHECK(Contains(refused.reason, "16"));          // the ceiling is named
  PRISM_CHECK(Contains(refused.reason, "16.7z"));       // the archive is named
}

// ---------------------------------------------------------------------------
// 4. Closing the ACTIVE tab: right neighbour, else left (first negative control).
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.closing_the_active_tab_activates_its_right_neighbour") {
  tabs::TabSet set = ThreeTabs(0);   // A active, right neighbour is B
  const tabs::CloseOutcome closed = set.CloseActive(false);
  PRISM_CHECK(closed.closed);
  PRISM_CHECK(!closed.needsConfirm);
  PRISM_CHECK(!closed.emptyState);
  PRISM_CHECK_EQ(closed.index, 0);
  PRISM_CHECK_EQ(closed.activeIndex, 0);      // B slid into slot 0
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/b.7z"));

  // A middle tab's right neighbour is the tab that follows it.
  tabs::TabSet middle = ThreeTabs(1);         // B active, close it -> C
  const tabs::CloseOutcome closedMiddle = middle.Close(1, false);
  PRISM_CHECK_EQ(closedMiddle.activeIndex, 1);
  PRISM_CHECK_EQ(middle.active()->path, std::string("C:/c.7z"));
  PRISM_CHECK_EQ(middle.At(0)->path, std::string("C:/a.7z"));
}

PRISM_TEST("tabs.closing_the_last_tab_activates_the_left_neighbour") {
  tabs::TabSet set = ThreeTabs(2);            // C active, no right neighbour
  const tabs::CloseOutcome closed = set.CloseActive(false);
  PRISM_CHECK(closed.closed);
  PRISM_CHECK_EQ(closed.index, 2);
  PRISM_CHECK_EQ(closed.activeIndex, 1);      // B, the left neighbour
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/b.7z"));
  PRISM_CHECK_EQ(set.At(0)->path, std::string("C:/a.7z"));
}

PRISM_TEST("tabs.closing_the_only_tab_leaves_an_empty_state_not_a_shutdown") {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  const tabs::CloseOutcome closed = set.CloseActive(false);
  PRISM_CHECK(closed.closed);
  PRISM_CHECK(closed.emptyState);
  PRISM_CHECK_EQ(closed.activeIndex, -1);
  PRISM_CHECK(set.empty());
  PRISM_CHECK_EQ(set.size(), std::size_t{0});
  PRISM_CHECK_EQ(set.activeIndex(), -1);
  PRISM_CHECK(set.active() == nullptr);
  PRISM_CHECK(Contains(closed.reason, "empty"));

  // Closing again is a NAMED refusal, not a crash and not a silent no-op.
  const tabs::CloseOutcome again = set.Close(0, false);
  PRISM_CHECK_EQ(again.verdict, tabs::TabVerdict::kNoSuchTab);
  PRISM_CHECK(!again.closed);
  PRISM_CHECK_EQ(again.index, -1);
  PRISM_CHECK_EQ(again.activeIndex, -1);

  // A tab can be opened again from the empty state, and it becomes active.
  const tabs::OpenOutcome reopened = set.Open("C:/b.7z");
  PRISM_CHECK(reopened.created);
  PRISM_CHECK_EQ(set.activeIndex(), 0);
}

// ---------------------------------------------------------------------------
// 5. Dirty tabs: ask first, and only then close.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.dirty_tab_requires_confirmation_before_it_closes") {
  tabs::TabSet set = ThreeTabs(1);            // B active
  PRISM_CHECK(set.SetDirty(1, true));
  PRISM_CHECK_EQ(set.DirtyCount(), std::size_t{1});

  const tabs::CloseOutcome asked = set.CloseActive(false);
  PRISM_CHECK_EQ(asked.verdict, tabs::TabVerdict::kNeedsConfirm);
  PRISM_CHECK(asked.needsConfirm);
  PRISM_CHECK(!asked.closed);
  PRISM_CHECK_EQ(asked.index, 1);
  PRISM_CHECK_EQ(asked.activeIndex, 1);
  PRISM_CHECK_EQ(set.size(), std::size_t{3});         // nothing was closed
  PRISM_CHECK_EQ(set.activeIndex(), 1);               // ... and nothing moved
  PRISM_CHECK(Contains(asked.reason, "b.7z"));        // the user is told WHICH tab

  // Confirmed: it closes exactly like any other tab (right neighbour becomes
  // active), and the dirty flag goes away with it.
  const tabs::CloseOutcome done = set.Close(1, true);
  PRISM_CHECK(done.closed);
  PRISM_CHECK(!done.needsConfirm);
  PRISM_CHECK_EQ(done.activeIndex, 1);
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/c.7z"));
  PRISM_CHECK_EQ(set.DirtyCount(), std::size_t{0});
}

// ---------------------------------------------------------------------------
// 6. Closing a BACKGROUND tab must not move the user.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.closing_a_background_tab_keeps_the_user_on_their_tab") {
  tabs::TabSet set = ThreeTabs(2);            // C active at index 2
  const tabs::CloseOutcome left = set.Close(0, false);   // A is behind the user
  PRISM_CHECK(left.closed);
  PRISM_CHECK_EQ(left.activeIndex, 1);        // C moved left; it is STILL C
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/c.7z"));

  set.Open("C:/d.7z");                        // active D at index 2
  const tabs::CloseOutcome right = set.Close(0, false);  // B is behind the user
  PRISM_CHECK(right.closed);
  PRISM_CHECK_EQ(right.activeIndex, 1);
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/d.7z"));
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// 7. MRU: activating is using; reordering is not.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.activating_moves_the_tab_to_the_front_of_the_mru") {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  set.Open("C:/b.7z");
  set.Open("C:/c.7z");
  std::vector<std::string> mru = set.MruPaths();
  if (!PRISM_CHECK_EQ(mru.size(), std::size_t{3})) return;
  PRISM_CHECK_EQ(mru[0], std::string("C:/c.7z"));   // newest open is the most recent
  PRISM_CHECK_EQ(mru[1], std::string("C:/b.7z"));
  PRISM_CHECK_EQ(mru[2], std::string("C:/a.7z"));

  PRISM_CHECK(set.Activate(0));                     // the user clicks A
  mru = set.MruPaths();
  PRISM_CHECK_EQ(mru[0], std::string("C:/a.7z"));
  PRISM_CHECK_EQ(mru[1], std::string("C:/c.7z"));
  PRISM_CHECK_EQ(mru[2], std::string("C:/b.7z"));

  PRISM_CHECK(set.ActivatePath("c:/B.7z"));         // by path, case-insensitively
  PRISM_CHECK_EQ(set.activeIndex(), 1);
  PRISM_CHECK_EQ(set.MruPaths().front(), std::string("C:/b.7z"));

  // Out of range / unknown: refused, and nothing is reordered or activated.
  PRISM_CHECK(!set.Activate(7));
  PRISM_CHECK(!set.ActivatePath("C:/nope.7z"));
  PRISM_CHECK_EQ(set.activeIndex(), 1);
  PRISM_CHECK_EQ(set.MruPaths().front(), std::string("C:/b.7z"));
}

PRISM_TEST("tabs.reordering_keeps_the_active_tab_and_its_view_binding") {
  tabs::TabSet set = ThreeTabs(0);                  // A active
  PRISM_CHECK(set.SetViewState(0, "deep/path", true));
  const std::uint64_t idA = set.At(0)->viewId;
  const std::uint64_t idB = set.At(1)->viewId;
  const std::uint64_t idC = set.At(2)->viewId;
  PRISM_CHECK(idA != 0);
  PRISM_CHECK(idA != idB);
  PRISM_CHECK(idB != idC);
  PRISM_CHECK(idA != idC);
  const std::vector<std::string> mruBefore = set.MruPaths();

  PRISM_CHECK(set.Move(2, 0));                      // drag C to the front
  PRISM_CHECK_EQ(set.At(0)->path, std::string("C:/c.7z"));
  PRISM_CHECK_EQ(set.At(0)->viewId, idC);           // identity travelled with the tab
  PRISM_CHECK_EQ(set.activeIndex(), 1);             // A is still the active tab
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/a.7z"));
  PRISM_CHECK_EQ(set.active()->viewId, idA);
  PRISM_CHECK_EQ(set.active()->view.folder, std::string("deep/path"));
  PRISM_CHECK_EQ(set.active()->view.listMode, true);
  const std::vector<std::string> mruAfter = set.MruPaths();
  if (!PRISM_CHECK_EQ(mruAfter.size(), mruBefore.size())) return;
  for (std::size_t i = 0; i < mruBefore.size(); ++i) {
    PRISM_CHECK_EQ(mruAfter[i], mruBefore[i]);      // recency is NOT position
  }

  PRISM_CHECK(set.Move(0, 2));                      // and back again
  PRISM_CHECK_EQ(set.activeIndex(), 0);
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/a.7z"));
  PRISM_CHECK(!set.Move(0, 0));                     // no-op: reported as not moved
  PRISM_CHECK(!set.Move(0, 9));
  PRISM_CHECK(!set.Move(-1, 0));
  PRISM_CHECK_EQ(set.size(), std::size_t{3});
}

// ---------------------------------------------------------------------------
// 8. Per-tab state: nothing leaks between tabs.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.view_state_and_dirty_flags_are_per_tab") {
  tabs::TabSet set = ThreeTabs(0);
  PRISM_CHECK(set.SetViewState(0, "one", true));
  PRISM_CHECK(set.SetViewState(1, "two", false));
  PRISM_CHECK(!set.SetViewState(9, "x", false));
  PRISM_CHECK(set.SetDirty(2, true));
  PRISM_CHECK_EQ(set.DirtyCount(), std::size_t{1});
  PRISM_CHECK_EQ(set.At(0)->view.folder, std::string("one"));
  PRISM_CHECK_EQ(set.At(1)->view.folder, std::string("two"));
  PRISM_CHECK_EQ(set.At(2)->view.folder, std::string());
  PRISM_CHECK(!set.At(0)->dirty);
  PRISM_CHECK(set.At(2)->dirty);
  PRISM_CHECK(!set.SetDirty(9, true));
  PRISM_CHECK(set.SetDirty(2, false));
  PRISM_CHECK_EQ(set.DirtyCount(), std::size_t{0});
  PRISM_CHECK_EQ(set.tabs().size(), std::size_t{3});
  PRISM_CHECK_EQ(set.At(3), nullptr);
  PRISM_CHECK_EQ(set.At(-1), nullptr);
  PRISM_CHECK_EQ(set.IndexOf(""), -1);
}

PRISM_TEST("tabs.a_reopened_archive_gets_a_new_view_binding") {
  tabs::TabSet set = ThreeTabs(0);
  const std::uint64_t closedId = set.At(1)->viewId;
  set.Close(1, false);
  set.Open("C:/b.7z");
  const tabs::Tab* reopened = set.active();
  if (!PRISM_CHECK(reopened != nullptr)) return;
  PRISM_CHECK_EQ(reopened->path, std::string("C:/b.7z"));
  // The UI must not serve the old tab's scroll offset to a fresh archive.
  PRISM_CHECK(reopened->viewId != closedId);
  PRISM_CHECK_EQ(reopened->view.folder, std::string());
}

// ---------------------------------------------------------------------------
// 9. Refusals: named, reasoned, and counted (nothing silently dropped).
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.unsupported_or_missing_archives_are_refused_with_a_reason") {
  tabs::TabSet set;
  const tabs::OpenOutcome text = set.Open("C:/notes.txt");
  PRISM_CHECK_EQ(text.verdict, tabs::TabVerdict::kUnsupportedFormat);
  PRISM_CHECK_EQ(text.index, -1);
  PRISM_CHECK(Contains(text.reason, ".txt"));
  PRISM_CHECK(set.empty());

  tabs::OpenRequest missing;
  missing.path = "C:/gone.7z";
  missing.exists = false;                       // the caller's answer: no such file
  const tabs::OpenOutcome absent = set.Open(missing);
  PRISM_CHECK_EQ(absent.verdict, tabs::TabVerdict::kMissingFile);
  PRISM_CHECK(Contains(absent.reason, "gone.7z"));
  PRISM_CHECK(set.empty());

  const tabs::OpenOutcome blank = set.Open("   ");
  PRISM_CHECK_EQ(blank.verdict, tabs::TabVerdict::kEmptyPath);
  PRISM_CHECK(set.empty());

  // The format rule IS C52's classifier, not a second table: the tab strip and
  // the drop path must agree about what an archive is, or one advertises a tab
  // the other refuses.
  struct Row {
    const char* path;
    bool archive;
  };
  const Row rows[] = {
      {"C:/a.7z", true},    {"C:/a.zip", true},     {"C:/a.rar", true},
      {"C:/a.tar", true},   {"C:/a.tar.gz", true},  {"C:/a.tgz", true},
      {"C:/a.iso", true},   {"C:/a.7z.001", false}, {"C:/a.txt", false},
      {"C:/a.exe", false},  {"C:/a", false},
  };
  for (const Row& row : rows) {
    PRISM_CHECK_EQ(tabs::IsSupportedArchivePath(row.path), row.archive);
    tabs::TabSet probe;
    const tabs::OpenOutcome outcome = probe.Open(row.path);
    PRISM_CHECK_EQ(outcome.verdict == tabs::TabVerdict::kOk, row.archive);
    PRISM_CHECK_EQ(probe.size(), row.archive ? std::size_t{1} : std::size_t{0});
  }
}

// ---------------------------------------------------------------------------
// 10. The C46 session mapping (round trip is the contract).
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.session_projection_is_mru_with_the_active_archive_first") {
  tabs::TabSet set;
  set.Open("C:/a.7z");
  set.Open("C:/b.7z");
  set.Open("C:/c.7z");
  set.SetViewState(1, "docs/2024", true);        // B's place inside its archive
  set.SetViewState(0, "", false);
  set.Activate(0);                               // A is what the user is looking at

  const tabs::SessionProjection projection = set.ToSession();
  if (!PRISM_CHECK_EQ(projection.entries.size(), std::size_t{3})) return;
  PRISM_CHECK_EQ(projection.active, 0);          // "active" is always the first entry
  PRISM_CHECK(!projection.lossy());
  PRISM_CHECK_EQ(projection.entries[0].path, std::string("C:/a.7z"));
  PRISM_CHECK_EQ(projection.entries[1].path, std::string("C:/c.7z"));
  PRISM_CHECK_EQ(projection.entries[2].path, std::string("C:/b.7z"));
  // The tab's place travels with it: folder + view mode are per archive.
  PRISM_CHECK_EQ(projection.entries[2].folder, std::string("docs/2024"));
  PRISM_CHECK_EQ(projection.entries[2].listMode, true);
  PRISM_CHECK_EQ(projection.entries[0].listMode, false);
}

PRISM_TEST("tabs.session_projection_round_trips_through_restore") {
  tabs::TabSet original;
  original.Open("C:/a.7z");
  original.Open("C:/b.7z");
  original.Open("D:/c.zip");
  original.Open("E:/d.tar.gz");
  original.SetViewState(0, "one/two", true);
  original.SetViewState(2, "", false);
  original.SetDirty(2, true);      // NOT part of the session: a new process has no edits
  original.Activate(2);

  const tabs::SessionProjection written = original.ToSession();
  if (!PRISM_CHECK_EQ(written.entries.size(), std::size_t{4})) return;
  PRISM_CHECK_EQ(written.entries[0].path, std::string("D:/c.zip"));

  tabs::RestoreReport report;
  const tabs::TabSet restored =
      tabs::TabSet::FromSession(written.entries, written.active, &report);
  PRISM_CHECK_EQ(report.restored, std::size_t{4});
  PRISM_CHECK_EQ(report.droppedBlank + report.droppedDuplicate + report.droppedUnsupported +
                     report.droppedOverLimit,
                 std::size_t{0});
  PRISM_CHECK_EQ(restored.size(), std::size_t{4});
  PRISM_CHECK_EQ(restored.activeIndex(), 0);                        // active first
  if (!PRISM_CHECK(restored.active() != nullptr)) return;
  PRISM_CHECK_EQ(restored.active()->path, std::string("D:/c.zip"));  // the archive they were in
  PRISM_CHECK_EQ(restored.active()->view.listMode, false);
  PRISM_CHECK(!restored.active()->dirty);                            // dirty is not persisted

  // THE ROUND TRIP: what we would write after restoring is exactly the
  // projection we restored from (paths, order, folders, view modes, active).
  const tabs::SessionProjection again = restored.ToSession();
  if (!PRISM_CHECK_EQ(again.entries.size(), written.entries.size())) return;
  PRISM_CHECK_EQ(again.active, written.active);
  PRISM_CHECK(!again.lossy());
  for (std::size_t i = 0; i < written.entries.size(); ++i) {
    PRISM_CHECK_EQ(again.entries[i].path, written.entries[i].path);
    PRISM_CHECK_EQ(again.entries[i].folder, written.entries[i].folder);
    PRISM_CHECK_EQ(again.entries[i].listMode, written.entries[i].listMode);
  }
  // The restored tab for C:/a.7z still remembers its folder.
  const int aIndex = restored.IndexOf("C:/a.7z");
  if (PRISM_CHECK(aIndex >= 0)) {
    PRISM_CHECK_EQ(restored.At(aIndex)->view.folder, std::string("one/two"));
    PRISM_CHECK_EQ(restored.At(aIndex)->view.listMode, true);
  }
}

PRISM_TEST("tabs.session_projection_reports_tabs_the_session_cannot_hold") {
  tabs::TabSet set;
  for (int i = 0; i < 10; ++i) set.Open("C:/arch/" + std::to_string(i) + ".7z");
  set.Activate(5);                          // the user goes back to an older tab
  const tabs::SessionProjection projection = set.ToSession();
  // C46's document holds eight; the tab set holds sixteen. The projection says
  // so instead of quietly pretending the other two never existed.
  PRISM_CHECK_EQ(projection.entries.size(), session::kMaxEntries);
  PRISM_CHECK_EQ(projection.dropped, std::size_t{2});
  PRISM_CHECK(projection.lossy());
  PRISM_CHECK_EQ(projection.active, 0);
  // The clip takes the TAIL of the MRU order, so the active archive is first --
  // and is never one of the two that were lost.
  PRISM_CHECK_EQ(projection.entries[0].path, std::string("C:/arch/5.7z"));
  PRISM_CHECK_EQ(projection.entries[session::kMaxEntries - 1].path,
                 std::string("C:/arch/2.7z"));

  const tabs::TabSet restored =
      tabs::TabSet::FromSession(projection.entries, projection.active, nullptr);
  PRISM_CHECK_EQ(restored.size(), session::kMaxEntries);
  if (!PRISM_CHECK(restored.active() != nullptr)) return;
  PRISM_CHECK_EQ(restored.active()->path, std::string("C:/arch/5.7z"));
}

// ---------------------------------------------------------------------------
// 11. Restoring a document: drop and COUNT, never half-apply.
// ---------------------------------------------------------------------------
PRISM_TEST("tabs.restore_drops_and_reports_blank_duplicate_and_foreign_entries") {
  std::vector<session::SessionEntry> entries;
  entries.push_back(session::SessionEntry{"C:/a.7z", "one", true});
  entries.push_back(session::SessionEntry{"", "blank", false});          // no path
  entries.push_back(session::SessionEntry{"c:/A.7Z", "dup", false});     // same archive
  entries.push_back(session::SessionEntry{"C:/notes.txt", "", false});   // not an archive
  entries.push_back(session::SessionEntry{"C:/b.zip", "", false});       // active entry

  tabs::RestoreReport report;
  const tabs::TabSet set = tabs::TabSet::FromSession(entries, 4, &report);
  PRISM_CHECK_EQ(report.restored, std::size_t{2});
  PRISM_CHECK_EQ(report.droppedBlank, std::size_t{1});
  PRISM_CHECK_EQ(report.droppedDuplicate, std::size_t{1});
  PRISM_CHECK_EQ(report.droppedUnsupported, std::size_t{1});
  PRISM_CHECK_EQ(report.droppedOverLimit, std::size_t{0});
  PRISM_CHECK_EQ(report.warnings.size(), std::size_t{3});   // every drop is stated
  PRISM_CHECK_EQ(set.size(), std::size_t{2});
  if (!PRISM_CHECK(set.active() != nullptr)) return;
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/b.zip"));
  // The duplicate did not overwrite the first entry's place in the archive.
  PRISM_CHECK_EQ(set.At(0)->view.folder, std::string("one"));
  PRISM_CHECK_EQ(set.At(0)->view.listMode, true);
  PRISM_CHECK_EQ(set.At(0)->displayName, std::string("a.7z"));
}

PRISM_TEST("tabs.restore_clips_an_over_long_session_and_reports_the_clip") {
  std::vector<session::SessionEntry> entries;
  for (int i = 0; i < 20; ++i) {
    entries.push_back(session::SessionEntry{"C:/x/" + std::to_string(i) + ".7z", "", false});
  }
  tabs::RestoreReport report;
  const tabs::TabSet set = tabs::TabSet::FromSession(entries, 0, &report);
  PRISM_CHECK_EQ(set.size(), tabs::kMaxTabs);
  PRISM_CHECK_EQ(report.restored, tabs::kMaxTabs);
  PRISM_CHECK_EQ(report.droppedOverLimit, std::size_t{4});
  PRISM_CHECK_EQ(report.warnings.size(), std::size_t{4});
  if (!PRISM_CHECK(set.active() != nullptr)) return;
  PRISM_CHECK_EQ(set.active()->path, std::string("C:/x/0.7z"));

  // An empty document is an empty STATE, not an error and not a crash.
  tabs::RestoreReport emptyReport;
  const std::vector<session::SessionEntry> nothing;
  const tabs::TabSet none = tabs::TabSet::FromSession(nothing, -1, &emptyReport);
  PRISM_CHECK(none.empty());
  PRISM_CHECK_EQ(none.activeIndex(), -1);
  PRISM_CHECK(none.active() == nullptr);
  PRISM_CHECK_EQ(emptyReport.restored, std::size_t{0});
  PRISM_CHECK_EQ(none.ToSession().active, -1);
  PRISM_CHECK_EQ(none.ToSession().entries.size(), std::size_t{0});
}

int main(int argc, char** argv) {
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR) || defined(PRISM_C59_MUTANT_DEDUP_OPEN)
  const char* mutant = "?";
#endif
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR)
  // The banner sits INSIDE the guard on purpose: the C23 matrix scans the 800
  // bytes after the #if defined(...) guard for that literal, and a banner at the
  // end of main() reads as "no banner" (CONTRACTS D28).
  std::cout << "C59 MUTANT ACTIVE: closing the active tab always activates tab 0"
            << std::endl;
  prism::test::SetMutationVerdict("C59 MUTATION CAUGHT: close_right_neighbor",
                                  [] { return CloseNeighbourInjectionFired(); });
  mutant = "close_right_neighbor";
#endif
#if defined(PRISM_C59_MUTANT_DEDUP_OPEN)
  std::cout << "C59 MUTANT ACTIVE: reopening an open archive appends a duplicate tab"
            << std::endl;
  prism::test::SetMutationVerdict("C59 MUTATION CAUGHT: dedup_open",
                                  [] { return DedupInjectionFired(); });
  mutant = "dedup_open";
#endif

  const int rc = prism::test::RunAll(argc, argv);
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR) || defined(PRISM_C59_MUTANT_DEDUP_OPEN)
  // A mutant run leaves normally in exactly two cases: the verdict already
  // fired (the harness exits 0 at the first failure -- we never reach here), or
  // the suite did NOT catch it. The second must be RED (the marker is the
  // verdict, D26), so say so out loud and fail instead of printing the clean
  // marker.
  if (rc == 0) {
    std::cout << "C59 MUTATION NOT CAUGHT: the suite stayed green with " << mutant
              << " compiled in" << std::endl;
  }
  return 1;
#else
  if (rc != 0) return rc;
  std::cout << "ALL TAB-SET CHECKS PASSED" << std::endl;
  return 0;
#endif
}
