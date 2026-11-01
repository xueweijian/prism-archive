// PrismArchive -- SessionStore suite (card C46).
//
// The session's decision layer is pure PrismCore, so the rules the card cares
// about are tested in milliseconds, on every lane:
//   * the v1 -> v2 migration (folders + view modes did not exist in v1);
//   * a schema version this build does not know is DISCARDED with one warning,
//     never half-read and never a crash;
//   * a broken JSON document is DISCARDED the same way;
//   * the crash guard: an unclean shutdown restores NOTHING, whatever the
//     document still lists;
//   * the 8-entry ceiling, MRU ordering, de-duplication, path/folder
//     normalization and the position bookkeeping the exe performs.
//
// NEGATIVE CONTROLS (CONTRACTS D3): three more executables compile ONE rule out
// each. The mutant prints "C46 MUTANT ACTIVE" (proof the injection built) and
// only prints "C46 MUTATION CAUGHT" when this suite really went red -- the
// marker is a conjunction, so a stale injection turns the mutant target RED
// instead of quietly green (CONTRACTS D26: no WILL_FAIL on a marker target).
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "../Src/PrismCore/SessionStore.hpp"

namespace session = prism::session;

namespace {

#if defined(PRISM_C46_MUTANT_KEEP_BAD_JSON)
constexpr const char* kMutant = "keep_bad_json";
#elif defined(PRISM_C46_MUTANT_IGNORE_SCHEMA_VERSION)
constexpr const char* kMutant = "ignore_schema_version";
#elif defined(PRISM_C46_MUTANT_IGNORE_CLEAN_SHUTDOWN)
constexpr const char* kMutant = "ignore_clean_shutdown";
#else
constexpr const char* kMutant = nullptr;
#endif

bool HasWarning(const session::SessionLoadReport& report, const char* needle) {
  for (const std::string& warning : report.warnings) {
    if (warning.find(needle) != std::string::npos) return true;
  }
  return false;
}

const session::SessionEntry* EntryAt(const session::SessionStore& store, std::size_t index) {
  if (index >= store.entries().size()) return nullptr;
  return &store.entries()[index];
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. The document round-trips: everything the window needs comes back.
// ---------------------------------------------------------------------------
PRISM_TEST("session.v2_round_trip_keeps_paths_folders_views_and_active") {
  session::SessionStore original;
  original.MarkCleanShutdown(true);
  std::vector<session::SessionEntry> entries;
  session::SessionEntry first;
  first.path = "C:/archives/photos.7z";
  first.folder = "2024/trip";
  first.listMode = true;
  entries.push_back(first);
  session::SessionEntry second;
  second.path = "D:/work/source.zip";
  second.folder = "";
  second.listMode = false;
  entries.push_back(second);
  original.Replace(entries, 1);
  PRISM_CHECK_EQ(original.entries().size(), std::size_t{2});
  PRISM_CHECK_EQ(original.activeIndex(), 1);

  const std::string document = original.ToJson();
  // The three facts the reader has to find without a schema registry.
  PRISM_CHECK(document.find("\"schemaVersion\": 2") != std::string::npos);
  PRISM_CHECK(document.find("\"cleanShutdown\": true") != std::string::npos);
  PRISM_CHECK(document.find("2024/trip") != std::string::npos);

  session::SessionStore readBack;
  const session::SessionLoadReport report = readBack.Load(document);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kLoaded);
  PRISM_CHECK(report.cleanShutdown);
  PRISM_CHECK(report.discardedEntries == 0);
  PRISM_CHECK(report.warnings.empty());
  if (!PRISM_CHECK_EQ(readBack.entries().size(), std::size_t{2})) return;
  PRISM_CHECK_EQ(readBack.entries()[0].path, std::string("C:/archives/photos.7z"));
  PRISM_CHECK_EQ(readBack.entries()[0].folder, std::string("2024/trip"));
  PRISM_CHECK_EQ(readBack.entries()[0].listMode, true);
  PRISM_CHECK_EQ(readBack.entries()[1].path, std::string("D:/work/source.zip"));
  PRISM_CHECK_EQ(readBack.entries()[1].folder, std::string());
  PRISM_CHECK_EQ(readBack.entries()[1].listMode, false);
  PRISM_CHECK_EQ(readBack.activeIndex(), 1);
  // ... and it is restorable, because the previous run closed cleanly.
  PRISM_CHECK_EQ(readBack.RestorableEntries().size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// 2. v1 -> v2: paths only. The migration is a real branch, not a comment.
// ---------------------------------------------------------------------------
PRISM_TEST("session.v1_document_migrates_to_folders_and_view_modes") {
  const std::string v1 = R"({"schemaVersion":1,"cleanShutdown":true,
      "archives":["C:\\archives\\old.7z","D:/work/two.zip"]})";
  session::SessionStore store;
  const session::SessionLoadReport report = store.Load(v1);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kMigrated);
  PRISM_CHECK(report.cleanShutdown);
  PRISM_CHECK(HasWarning(report, "migrated"));
  PRISM_CHECK_EQ(store.entries().size(), std::size_t{2});
  if (store.entries().size() != 2) return;
  // Backslashes become PrismCore's '/' (CONTRACTS B4).
  PRISM_CHECK_EQ(store.entries()[0].path, std::string("C:/archives/old.7z"));
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string());
  PRISM_CHECK_EQ(store.entries()[0].listMode, false);
  PRISM_CHECK_EQ(store.entries()[1].path, std::string("D:/work/two.zip"));
  // The migrated document is written as v2 from now on.
  const std::string rewritten = store.ToJson();
  PRISM_CHECK(rewritten.find("\"schemaVersion\": 2") != std::string::npos);
  PRISM_CHECK(rewritten.find("\"entries\"") != std::string::npos);
  PRISM_CHECK(rewritten.find("\"archives\"") == std::string::npos);
}

// ---------------------------------------------------------------------------
// 3. A newer schema is DISCARDED, never guessed at.
// ---------------------------------------------------------------------------
PRISM_TEST("session.unknown_schema_version_is_discarded_with_one_warning") {
  const std::string future = R"({"schemaVersion":99,"cleanShutdown":true,
      "entries":[{"path":"C:/a.7z","folder":"x","view":"list"}]})";
  session::SessionStore store;
  const session::SessionLoadReport report = store.Load(future);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kUnsupportedVersion);
  PRISM_CHECK(report.hadDocument);
  PRISM_CHECK_EQ(report.warnings.size(), std::size_t{1});
  PRISM_CHECK(HasWarning(report, "99"));
  PRISM_CHECK(HasWarning(report, "discarded"));
  // Discarded really means discarded: no entry survives to be restored.
  PRISM_CHECK(store.entries().empty());
  PRISM_CHECK_EQ(store.activeIndex(), -1);
  PRISM_CHECK(store.RestorableEntries().empty());

  // A version below the oldest this build knows is equally unusable.
  session::SessionStore older;
  const session::SessionLoadReport olderReport = older.Load(R"({"schemaVersion":0})");
  PRISM_CHECK(olderReport.status == session::SessionLoadStatus::kCorrupt);
  PRISM_CHECK(older.entries().empty());
}

// ---------------------------------------------------------------------------
// 4. Broken JSON: discard, do not crash (the card's first negative control).
// ---------------------------------------------------------------------------
PRISM_TEST("session.broken_json_is_discarded_and_never_half_read") {
  session::SessionStore store;
  // Seed state that a half-read document would have to clear.
  store.Replace({session::SessionEntry{"C:/stale.7z", "deep", true}}, 0);
  store.MarkCleanShutdown(true);
  PRISM_CHECK_EQ(store.entries().size(), std::size_t{1});

  const session::SessionLoadReport report =
      store.Load(R"({"schemaVersion":2,"cleanShutdown":true,"entries":[{"path":"C:/a.7z"})");
  PRISM_CHECK(report.status == session::SessionLoadStatus::kCorrupt);
  PRISM_CHECK_EQ(report.warnings.size(), std::size_t{1});
  PRISM_CHECK(HasWarning(report, "not JSON"));
  PRISM_CHECK(store.entries().empty());
  PRISM_CHECK(!store.cleanShutdown());

  // A JSON document that is not an OBJECT is equally useless.
  const session::SessionLoadReport arrayReport = store.Load(R"([1,2,3])");
  PRISM_CHECK(arrayReport.status == session::SessionLoadStatus::kCorrupt);
  PRISM_CHECK(store.entries().empty());

  // Garbage bytes (a torn write from a build without the atomic path) too.
  const session::SessionLoadReport garbage = store.Load(std::string("\x01\x02\xff{}", 5));
  PRISM_CHECK(garbage.status == session::SessionLoadStatus::kCorrupt);
  PRISM_CHECK(store.entries().empty());
}

// ---------------------------------------------------------------------------
// 5. No document is not an error: the first run of a fresh profile.
// ---------------------------------------------------------------------------
PRISM_TEST("session.absent_or_blank_document_is_missing_not_corrupt") {
  session::SessionStore store;
  const session::SessionLoadReport empty = store.Load("");
  PRISM_CHECK(empty.status == session::SessionLoadStatus::kMissing);
  PRISM_CHECK(!empty.hadDocument);
  PRISM_CHECK(empty.warnings.empty());
  PRISM_CHECK(store.entries().empty());

  const session::SessionLoadReport blank = store.Load("  \r\n\t ");
  PRISM_CHECK(blank.status == session::SessionLoadStatus::kMissing);
  PRISM_CHECK(!blank.hadDocument);
}

// ---------------------------------------------------------------------------
// 6. The crash guard (the card's second negative control).
// ---------------------------------------------------------------------------
PRISM_TEST("session.unclean_shutdown_restores_nothing") {
  const std::string killed = R"({"schemaVersion":2,"cleanShutdown":false,
      "active":0,
      "entries":[{"path":"C:/a.7z","folder":"sub","view":"list"},
                 {"path":"D:/b.7z","folder":"","view":"grid"}]})";
  session::SessionStore store;
  const session::SessionLoadReport report = store.Load(killed);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kLoaded);
  PRISM_CHECK(!report.cleanShutdown);
  PRISM_CHECK(!store.cleanShutdown());
  // The document still LISTS the tabs (diagnostics, the sidebar's recents), but
  // the restore policy hands out NOTHING: a killed process may not resurrect.
  PRISM_CHECK_EQ(store.entries().size(), std::size_t{2});
  PRISM_CHECK(store.RestorableEntries().empty());

  // A clean shutdown of the same document is restorable.
  store.MarkCleanShutdown(true);
  PRISM_CHECK_EQ(store.RestorableEntries().size(), std::size_t{2});
  // ... and arming the dirty mark again (a new process started) withdraws it.
  store.MarkCleanShutdown(false);
  PRISM_CHECK(store.RestorableEntries().empty());

  // A document with NO cleanShutdown member at all is a crash, not a clean run:
  // absent must never be treated as true.
  session::SessionStore anonymous;
  const session::SessionLoadReport noFlag = anonymous.Load(
      R"({"schemaVersion":2,"entries":[{"path":"C:/a.7z"}]})");
  PRISM_CHECK(noFlag.status == session::SessionLoadStatus::kLoaded);
  PRISM_CHECK(!noFlag.cleanShutdown);
  PRISM_CHECK(anonymous.RestorableEntries().empty());
}

// ---------------------------------------------------------------------------
// 7. The ceiling and the MRU order.
// ---------------------------------------------------------------------------
PRISM_TEST("session.keeps_at_most_eight_archives_most_recent_first") {
  session::SessionStore store;
  for (int i = 0; i < 10; ++i) {
    const std::string path = "C:/archives/" + std::to_string(i) + ".7z";
    PRISM_CHECK(store.RecordOpen(path, "", i % 2 == 0));
  }
  PRISM_CHECK_EQ(store.entries().size(), session::kMaxEntries);
  PRISM_CHECK_EQ(store.entries().front().path, std::string("C:/archives/9.7z"));
  PRISM_CHECK_EQ(store.entries().back().path, std::string("C:/archives/2.7z"));
  // The two oldest fell off the tail, not the head.
  PRISM_CHECK_EQ(store.IndexOf("C:/archives/0.7z"), -1);
  PRISM_CHECK_EQ(store.IndexOf("C:/archives/1.7z"), -1);

  // A document that claims more is clipped on read, and the clip is REPORTED.
  std::string document = R"({"schemaVersion":2,"cleanShutdown":true,"entries":[)";
  for (int i = 0; i < 10; ++i) {
    if (i != 0) document += ",";
    document += "{\"path\":\"C:/over/" + std::to_string(i) + ".7z\"}";
  }
  document += "]}";
  session::SessionStore readBack;
  const session::SessionLoadReport report = readBack.Load(document);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kLoaded);
  PRISM_CHECK_EQ(readBack.entries().size(), session::kMaxEntries);
  PRISM_CHECK_EQ(report.discardedEntries, std::size_t{2});
}

PRISM_TEST("session.reopening_moves_to_the_front_and_never_duplicates") {
  session::SessionStore store;
  store.RecordOpen("C:/a.7z", "one", false);
  store.RecordOpen("C:/b.7z", "two", true);
  PRISM_CHECK_EQ(store.entries().size(), std::size_t{2});
  store.RecordOpen("C:/a.7z", "", true);  // reopen with no folder supplied
  if (!PRISM_CHECK_EQ(store.entries().size(), std::size_t{2})) return;
  PRISM_CHECK_EQ(store.entries()[0].path, std::string("C:/a.7z"));
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string("one"));  // remembered
  PRISM_CHECK_EQ(store.entries()[0].listMode, true);               // updated
  PRISM_CHECK_EQ(store.activeIndex(), 0);
}

// ---------------------------------------------------------------------------
// 8. Normalization: a session file is data, and data does not walk out.
// ---------------------------------------------------------------------------
PRISM_TEST("session.normalizes_paths_and_folders") {
  PRISM_CHECK_EQ(session::NormalizePath("  C:\\work\\\\a.7z  "), std::string("C:/work/a.7z"));
  PRISM_CHECK_EQ(session::NormalizePath("C:\\"), std::string("C:/"));
  PRISM_CHECK_EQ(session::NormalizePath("//server/share/x.7z"), std::string("/server/share/x.7z"));
  PRISM_CHECK_EQ(session::NormalizePath(""), std::string());

  PRISM_CHECK_EQ(session::NormalizeFolder(""), std::string());
  PRISM_CHECK_EQ(session::NormalizeFolder("/"), std::string());
  PRISM_CHECK_EQ(session::NormalizeFolder("a/b/"), std::string("a/b"));
  PRISM_CHECK_EQ(session::NormalizeFolder("/a//b/./c/"), std::string("a/b/c"));
  PRISM_CHECK_EQ(session::NormalizeFolder("a\\b"), std::string("a/b"));
  // ".." cannot escape the archive root: it clamps to the root.
  PRISM_CHECK_EQ(session::NormalizeFolder("a/../../etc"), std::string("etc"));

  const std::vector<std::string> segments = session::FolderSegments("/a//b/");
  if (!PRISM_CHECK_EQ(segments.size(), std::size_t{2})) return;
  PRISM_CHECK_EQ(segments[0], std::string("a"));
  PRISM_CHECK_EQ(segments[1], std::string("b"));

  // A hostile document cannot make the store remember a traversal either.
  session::SessionStore store;
  store.RecordOpen("C:/a.7z", "../../secret", false);
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string("secret"));
}

// ---------------------------------------------------------------------------
// 9. Malformed entries: dropped and COUNTED, the document still loads.
// ---------------------------------------------------------------------------
PRISM_TEST("session.malformed_entries_are_dropped_and_counted") {
  const std::string document = R"({"schemaVersion":2,"cleanShutdown":true,"active":7,
      "entries":["bare-string", {}, {"path":""}, {"path":5},
                 {"path":"C:/good.7z","folder":"docs","view":"hologram"}]})";
  session::SessionStore store;
  const session::SessionLoadReport report = store.Load(document);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kLoaded);
  PRISM_CHECK_EQ(report.discardedEntries, std::size_t{4});
  if (!PRISM_CHECK_EQ(store.entries().size(), std::size_t{1})) return;
  PRISM_CHECK_EQ(store.entries()[0].path, std::string("C:/good.7z"));
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string("docs"));
  // An unknown view mode is repaired to the grid and SAID OUT LOUD.
  PRISM_CHECK_EQ(store.entries()[0].listMode, false);
  PRISM_CHECK(HasWarning(report, "hologram"));
  // An out-of-range active index falls back to the head, never to garbage.
  PRISM_CHECK_EQ(store.activeIndex(), 0);
}

// ---------------------------------------------------------------------------
// 10. Position bookkeeping: only a path the session owns can be updated.
// ---------------------------------------------------------------------------
PRISM_TEST("session.position_updates_only_known_archives") {
  session::SessionStore store;
  store.RecordOpen("C:/a.7z", "", false);
  PRISM_CHECK(store.UpdatePosition("C:/a.7z", "sub/dir", true));
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string("sub/dir"));
  PRISM_CHECK_EQ(store.entries()[0].listMode, true);
  PRISM_CHECK(!store.UpdatePosition("C:/not-in-session.7z", "x", false));
  PRISM_CHECK(!store.Remove("C:/not-in-session.7z"));
  PRISM_CHECK(store.Remove("C:/a.7z"));
  PRISM_CHECK(store.entries().empty());
  PRISM_CHECK_EQ(store.activeIndex(), -1);
}

// ---------------------------------------------------------------------------
// 11. Bytes are bytes: a non-ASCII archive path survives the round trip.
// ---------------------------------------------------------------------------
PRISM_TEST("session.utf8_paths_round_trip_byte_for_byte") {
  const std::string path = "C:/archives/\xE5\xBD\x92\xE6\xA1\xA3\xE5\xBA\x93.7z";  // 归档库.7z
  const std::string folder = "\xE5\x9B\xBE\xE7\x89\x87";                              // 图片
  session::SessionStore store;
  store.RecordOpen(path, folder, true);
  const std::string document = store.ToJson();
  session::SessionStore readBack;
  const session::SessionLoadReport report = readBack.Load(document);
  PRISM_CHECK(report.status == session::SessionLoadStatus::kLoaded);
  if (!PRISM_CHECK_EQ(readBack.entries().size(), std::size_t{1})) return;
  PRISM_CHECK_EQ(readBack.entries()[0].path, path);
  PRISM_CHECK_EQ(readBack.entries()[0].folder, folder);
  PRISM_CHECK_EQ(readBack.entries()[0].path.size(), path.size());
}

// ---------------------------------------------------------------------------
// 12. Arguments win: Replace() is the "this launch defined the session" path.
// ---------------------------------------------------------------------------
PRISM_TEST("session.replace_caps_deduplicates_and_ignores_blank_paths") {
  session::SessionStore store;
  store.RecordOpen("C:/old.7z", "old", true);
  std::vector<session::SessionEntry> args;
  session::SessionEntry a;
  a.path = "C:/new-a.7z";
  a.folder = "a/b";
  args.push_back(a);
  args.push_back(session::SessionEntry{"", "ghost", false});          // dropped
  args.push_back(session::SessionEntry{"C:/new-a.7z", "dup", false});  // duplicate
  session::SessionEntry b;
  b.path = "C:/new-b.7z";
  args.push_back(b);
  store.Replace(args, 1);
  PRISM_CHECK_EQ(store.entries().size(), std::size_t{2});
  PRISM_CHECK_EQ(store.entries()[0].path, std::string("C:/new-a.7z"));
  PRISM_CHECK_EQ(store.entries()[0].folder, std::string("a/b"));
  PRISM_CHECK_EQ(store.entries()[1].path, std::string("C:/new-b.7z"));
  PRISM_CHECK_EQ(store.activeIndex(), 1);
  // The previous session is NOT in the new one: the arguments were authoritative.
  PRISM_CHECK_EQ(store.IndexOf("C:/old.7z"), -1);
}

int main(int argc, char** argv) {
  if (kMutant != nullptr) {
    std::printf("C46 MUTANT ACTIVE: %s (one rule compiled out)\n", kMutant);
  }
  const int rc = prism::test::RunAll(argc, argv);
  if (kMutant != nullptr) {
    if (rc == 0) {
      std::printf("C46 MUTATION NOT CAUGHT: the suite stayed green with %s compiled in\n", kMutant);
      return 1;
    }
    std::printf("C46 MUTATION CAUGHT: %s\n", kMutant);
    return 0;
  }
  if (rc != 0) return rc;
  std::printf("ALL SESSION-STORE CHECKS PASSED\n");
  return 0;
}
