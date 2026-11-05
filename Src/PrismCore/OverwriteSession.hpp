// PrismArchive -- PrismCore
// C94: extract.overwrite -- ONE decision for the WHOLE extraction.
//
// BEFORE THIS CARD
// C71 landed the overwrite policy (ask/replace/skip/rename) and wired it into
// GetStream, but the "ask" seam was a per-file PromptFn: extracting 14 files
// over 14 existing targets raised the message box 14 times with no way to say
// "do that for the rest". That dialog had three answers (yes=replace,
// no=skip, cancel=save-beside) and no scope.
//
// WHAT THIS FILE OWNS
// The scope. A Session lives for exactly ONE extraction operation (the extract
// callback owns it), remembers the answer once the user says "apply to all",
// and answers every later conflict from that memory without touching the UI
// again. The decision table itself stays in OverwritePolicy.hpp -- this class
// never re-implements the rename chain.
//
// SEMANTICS THAT MUST NOT DRIFT
//   * the default is still ASK: with no prompt at all the honest answer is
//     Skip, never a silent overwrite;
//   * "just this one" answers are NOT remembered (the next conflict asks
//     again) unless the dialog's "remember" box was ticked;
//   * a skip writes nothing; a rename lands beside the target as
//     "name (1).ext" and leaves the original exactly as it was.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "OverwritePolicy.hpp"

namespace prism::overwrite {

// The buttons the ONE conflict dialog offers. "All" answers apply to every
// later conflict in this operation; "One" answers cover the file in the body.
enum class Answer : std::uint8_t {
  ReplaceOne = 0,
  SkipOne,
  RenameOne,
  ReplaceAll,
  SkipAll,
  RenameAll,
};

inline const char* AnswerName(Answer answer) {
  switch (answer) {
    case Answer::ReplaceOne: return "replace-one";
    case Answer::SkipOne: return "skip-one";
    case Answer::RenameOne: return "rename-one";
    case Answer::ReplaceAll: return "replace-all";
    case Answer::SkipAll: return "skip-all";
    case Answer::RenameAll: return "rename-all";
  }
  return "?";
}

inline bool AppliesToAll(Answer answer) {
  return answer == Answer::ReplaceAll || answer == Answer::SkipAll ||
         answer == Answer::RenameAll;
}

inline Choice ChoiceOf(Answer answer) {
  switch (answer) {
    case Answer::ReplaceAll:
    case Answer::ReplaceOne:
      return Choice::Replace;
    case Answer::SkipAll:
    case Answer::SkipOne:
      return Choice::Skip;
    case Answer::RenameAll:
    case Answer::RenameOne:
      return Choice::Rename;
  }
  return Choice::Skip;
}

// What the dialog hands back: the button, plus the state of the
// "remember my choice for this operation" box. An "all" button implies the box
// (that is what it means); the box is what makes a "just this one" answer stick.
struct Reply {
  Choice choice = Choice::Skip;
  bool remember = false;
};

inline Reply ReplyOf(Answer answer, bool rememberChecked = false) {
  Reply reply;
  reply.choice = ChoiceOf(answer);
  reply.remember = AppliesToAll(answer) || rememberChecked;
  return reply;
}

// The one-dialog seam. The product supplies the real window; a test supplies a
// counter (or the real window driven by its own test seam).
using BatchPromptFn = std::function<Reply(const std::string&)>;

// Bridges the C71 per-file prompt (yes/no/cancel) onto the batch seam WITHOUT
// remembering: an old caller keeps asking per conflict, it just cannot silently
// apply one answer to the rest.
inline BatchPromptFn SequentialPrompt(PromptFn prompt) {
  if (!prompt) return {};
  return [prompt = std::move(prompt)](const std::string& path) {
    Reply reply;
    reply.choice = prompt(path);
    reply.remember = false;
    return reply;
  };
}

inline Mode ModeOf(Choice choice) {
  switch (choice) {
    case Choice::Replace: return Mode::Replace;
    case Choice::Skip: return Mode::Skip;
    case Choice::Rename: return Mode::Rename;
  }
  return Mode::Ask;
}

#if defined(PRISM_C94_MUTANT_PER_FILE)
// NEGATIVE CONTROL (CONTRACTS D3): the remembered answer is never applied --
// every conflict goes back to the UI, which is exactly the pre-C94 defect.
// The counter is the proof the injected branch ran; the suite prints its CAUGHT
// marker only when it did (D27).
inline std::uint64_t& MutantPerFileHits() {
  static std::uint64_t hits = 0;
  return hits;
}
#endif

// One extraction operation's overwrite state.
class Session {
 public:
  Session() = default;
  Session(Mode mode, BatchPromptFn prompt, ExistsFn exists = {})
      : mode_(mode), prompt_(std::move(prompt)), exists_(std::move(exists)) {}

  // The single decision point every extracted file goes through. Directories
  // merge (never renamed/skipped), a free path is written as-is, and only a
  // real conflict can reach the UI.
  Decision Decide(const std::string& finalPath, bool isDirectory) {
    if (isDirectory || !exists_ || !exists_(finalPath)) {
      return Decision{Choice::Replace, finalPath};
    }
    ++conflicts_;

#if defined(PRISM_C94_MUTANT_PER_FILE)
    ++MutantPerFileHits();
    if (mode_ == Mode::Ask) {
      // No prompt == Skip, never a silent overwrite (same rule as the clean
      // build; the mutation only removes the MEMORY).
      if (!prompt_) return Decision{Choice::Skip, std::string()};
      ++prompts_;
      const Reply reply = prompt_(finalPath);
      return prism::overwrite::Decide(finalPath, false, ModeOf(reply.choice), exists_);
    }
    return prism::overwrite::Decide(finalPath, false, mode_, exists_);
#else
    if (latched_) {
      ++remembered_;
      return prism::overwrite::Decide(finalPath, false, ModeOf(sticky_), exists_);
    }
    Choice choice = Choice::Replace;
    if (mode_ != Mode::Ask) {
      choice = mode_ == Mode::Replace ? Choice::Replace
             : mode_ == Mode::Skip  ? Choice::Skip
                                    : Choice::Rename;
    } else {
      // A missing dialog is not consent: the one answer that must never happen
      // here is Replace.
      if (!prompt_) return Decision{Choice::Skip, std::string()};
      ++prompts_;
      const Reply reply = prompt_(finalPath);
      choice = reply.choice;
      if (reply.remember) {
        latched_ = true;
        sticky_ = choice;
        ++latchedAfter_;
      }
    }
    return prism::overwrite::Decide(finalPath, false, ModeOf(choice), exists_);
#endif
  }

  Mode mode() const { return mode_; }
  // How many times the UI seam was invoked. The C94 suite pins this to 1 for a
  // 14-conflict extraction; the per-file mutant makes it 14.
  std::uint64_t prompts() const { return prompts_; }
  std::uint64_t conflicts() const { return conflicts_; }
  // Conflicts answered from the remembered answer (no UI).
  std::uint64_t remembered() const { return remembered_; }
  // Prompts after which the answer became sticky (0 or 1 in a sane run).
  std::uint64_t latchedAfter() const { return latchedAfter_; }
  bool latched() const { return latched_; }

 private:
  Mode mode_ = Mode::Ask;
  BatchPromptFn prompt_;
  ExistsFn exists_;
  bool latched_ = false;
  Choice sticky_ = Choice::Replace;
  std::uint64_t prompts_ = 0;
  std::uint64_t conflicts_ = 0;
  std::uint64_t remembered_ = 0;
  std::uint64_t latchedAfter_ = 0;
};

}  // namespace prism::overwrite
