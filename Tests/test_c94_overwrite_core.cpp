// PrismArchive -- C94 (core lane): ONE overwrite decision per extraction.
//
// THE DEFECT THIS CARD REMOVES
// C71 asked once per conflicting file (a PromptFn returning one Choice). A
// 14-file extraction over 14 existing targets raised 14 dialogs and offered no
// way to answer for the rest.
//
// WHAT IS ASSERTED HERE (values, never names -- D2)
//   * a 14-conflict Session answers all 14 from ONE prompt when the answer is
//     "all", and the per-file decisions are the same ones OverwritePolicy
//     produces (byte-identical paths: replace keeps the target, skip yields no
//     path, rename yields "name (1).ext");
//   * "just this one" is NOT remembered (prompts == 14), the "remember" box is
//     what makes it stick (prompts == 1);
//   * no prompt at all == Skip, never Replace: a missing dialog is not consent;
//   * an explicit mode (replace/skip/rename) never reaches the UI.
//
// The Windows lane (win/test_c94_overwrite_all.cpp) proves the same thing on
// real 7z bytes and pins the REAL dialog's creation count to 1.
#include <set>
#include <string>
#include <vector>

#include "mini_test.hpp"
#include "../Src/PrismCore/OverwriteSession.hpp"

namespace ow = prism::overwrite;

namespace {

constexpr int kFiles = 14;

std::string Target(int i) {
  return std::string("out/item") + (i < 10 ? "0" : "") + std::to_string(i) + ".txt";
}

// The 14 existing targets, as the extraction would find them on disk.
ow::ExistsFn ExistingTargets() {
  std::set<std::string> taken;
  for (int i = 1; i <= kFiles; ++i) taken.insert(Target(i));
  return [taken](const std::string& path) { return taken.count(path) != 0; };
}

struct PromptLog {
  int calls = 0;
  std::vector<std::string> paths;
  ow::Reply reply;
};

ow::BatchPromptFn ScriptedPrompt(PromptLog* log, ow::Reply reply) {
  log->reply = reply;
  return [log, reply](const std::string& path) {
    ++log->calls;
    log->paths.push_back(path);
    return reply;
  };
}

}  // namespace

PRISM_TEST("all-replace: 14 conflicts, ONE prompt, 14 replacements") {
  PromptLog log;
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::ReplaceAll)),
                      ExistingTargets());
  int replaced = 0;
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    if (d.choice == ow::Choice::Replace && d.path == Target(i)) ++replaced;
  }
  PRISM_CHECK_EQ(replaced, kFiles);
  PRISM_CHECK_EQ(log.calls, 1);          // the UI seam was asked ONCE
  PRISM_CHECK_EQ(session.prompts(), 1u);
  PRISM_CHECK_EQ(session.conflicts(), static_cast<unsigned>(kFiles));
  PRISM_CHECK_EQ(session.remembered(), static_cast<unsigned>(kFiles - 1));
  PRISM_CHECK_EQ(session.latchedAfter(), 1u);
  PRISM_CHECK_EQ(log.paths.size(), 1u);  // and it named a real target
  PRISM_CHECK(log.paths[0].find("item01.txt") != std::string::npos);
}

PRISM_TEST("all-skip: 14 conflicts, ONE prompt, no target is touched") {
  PromptLog log;
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::SkipAll)),
                      ExistingTargets());
  int skipped = 0;
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    if (d.choice == ow::Choice::Skip && d.path.empty()) ++skipped;
  }
  PRISM_CHECK_EQ(skipped, kFiles);
  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK_EQ(session.remembered(), static_cast<unsigned>(kFiles - 1));
}

PRISM_TEST("all-rename: 14 conflicts, ONE prompt, each lands at name (1).ext") {
  PromptLog log;
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::RenameAll)),
                      ExistingTargets());
  int renamed = 0;
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    // The exact spelling the card requires, per file, with the original name
    // never reused for the new payload.
    const std::string expected = std::string("out/item") + (i < 10 ? "0" : "") +
                                 std::to_string(i) + " (1).txt";
    if (d.choice == ow::Choice::Rename && d.path == expected) ++renamed;
  }
  PRISM_CHECK_EQ(renamed, kFiles);
  PRISM_CHECK_EQ(log.calls, 1);
}

PRISM_TEST("one-only: the answer is NOT remembered, the next conflict asks again") {
  PromptLog log;
  // The user picks 覆盖 for item01 and does not tick the box.
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::ReplaceOne, false)),
                      ExistingTargets());
  const ow::Decision first = session.Decide(Target(1), false);
  PRISM_CHECK(first.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(log.calls, 1);
  // The second conflict is a NEW question: "just this one" said nothing about
  // the file after it.
  const ow::Decision second = session.Decide(Target(2), false);
  PRISM_CHECK(second.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(log.calls, 2);
  PRISM_CHECK(!session.latched());
}

PRISM_TEST("remember box: a one-only answer becomes the operation's answer") {
  PromptLog log;
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::SkipOne, true)),
                      ExistingTargets());
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    PRISM_CHECK(d.choice == ow::Choice::Skip);
  }
  PRISM_CHECK_EQ(log.calls, 1);
  PRISM_CHECK(session.latched());
}

PRISM_TEST("ask with no prompt is SKIP, never a silent overwrite") {
  ow::Session session(ow::Mode::Ask, {}, ExistingTargets());
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    PRISM_CHECK(d.choice == ow::Choice::Skip);
  }
  PRISM_CHECK_EQ(session.prompts(), 0u);
  // The default construction must still be "ask": no code path may turn a
  // missing seam into consent.
  PRISM_CHECK(ow::Session().mode() == ow::Mode::Ask);
}

PRISM_TEST("an explicit mode never reaches the UI") {
  PromptLog log;
  ow::Session session(ow::Mode::Rename,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::SkipAll)),
                      ExistingTargets());
  for (int i = 1; i <= kFiles; ++i) {
    const ow::Decision d = session.Decide(Target(i), false);
    PRISM_CHECK(d.choice == ow::Choice::Rename);
  }
  PRISM_CHECK_EQ(log.calls, 0);
  PRISM_CHECK_EQ(session.prompts(), 0u);
}

PRISM_TEST("a free path and a directory never ask, even in ask mode") {
  PromptLog log;
  ow::Session session(ow::Mode::Ask,
                      ScriptedPrompt(&log, ow::ReplyOf(ow::Answer::SkipAll)),
                      ExistingTargets());
  const ow::Decision fresh = session.Decide("out/fresh.txt", false);
  PRISM_CHECK(fresh.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(fresh.path, std::string("out/fresh.txt"));
  const ow::Decision dir = session.Decide("out/item01.txt", true);
  PRISM_CHECK(dir.choice == ow::Choice::Replace);
  PRISM_CHECK_EQ(log.calls, 0);
}

PRISM_TEST("the C71 per-file seam still means per-file (no accidental scope)") {
  // SequentialPrompt is the compatibility bridge: a caller that only knows the
  // yes/no/cancel answer must keep being asked for EVERY file.
  int calls = 0;
  ow::PromptFn legacy = [&calls](const std::string&) {
    ++calls;
    return ow::Choice::Replace;
  };
  ow::Session session(ow::Mode::Ask, ow::SequentialPrompt(legacy), ExistingTargets());
  for (int i = 1; i <= kFiles; ++i) session.Decide(Target(i), false);
  PRISM_CHECK_EQ(calls, kFiles);
  PRISM_CHECK_EQ(session.remembered(), 0u);
}

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) return 1;
  std::puts("ALL C94 OVERWRITE-ALL CHECKS PASSED");
  return 0;
}
