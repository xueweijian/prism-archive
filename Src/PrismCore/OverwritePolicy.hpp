// PrismArchive -- PrismCore
// C71: extract.overwrite -- the decision "a file already exists, now what?".
//
// BEFORE THIS FILE
// The setting was persisted and validated with four documented values
// (ask/replace/skip/rename) and read by nothing: SmartExtractCallback always
// opened its DiskOutStream with CREATE_ALWAYS, i.e. every policy behaved like
// "replace". PathSafety::CollisionResolver existed but had no extraction caller
// (grep: only its own unit test).
//
// WHY THE DECISION IS PURE
// It is filesystem-free by injection (ExistsFn), so the Linux lane owns the
// rename chain, the skip answer and the ask seam; PrismWin only supplies the
// real existence probe and the real prompt.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace prism::overwrite {

enum class Mode : std::uint8_t { Ask = 0, Replace, Skip, Rename };

inline const char* KeyOf(Mode mode) {
  switch (mode) {
    case Mode::Ask: return "ask";
    case Mode::Replace: return "replace";
    case Mode::Skip: return "skip";
    case Mode::Rename: return "rename";
  }
  return "ask";
}

inline Mode ModeFromKey(std::string_view key) {
  if (key == "replace") return Mode::Replace;
  if (key == "skip") return Mode::Skip;
  if (key == "rename") return Mode::Rename;
  return Mode::Ask;  // "ask" and anything unrecognised
}

enum class Choice : std::uint8_t { Replace = 0, Skip, Rename };

inline const char* ChoiceName(Choice choice) {
  switch (choice) {
    case Choice::Replace: return "replace";
    case Choice::Skip: return "skip";
    case Choice::Rename: return "rename";
  }
  return "?";
}

struct Decision {
  Choice choice = Choice::Replace;
  std::string path;  // empty for Skip
};

using ExistsFn = std::function<bool(const std::string&)>;
// The UI seam: the product supplies a message box, a test supplies a counter.
// No prompt means Ask behaves like Skip -- never like a silent overwrite.
using PromptFn = std::function<Choice(const std::string&)>;

namespace detail {

inline std::string StemOf(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return path;
  return path.substr(0, path.size() - (name.size() - dot));
}

inline std::string ExtensionOf(const std::string& path) {
  const std::size_t slash = path.find_last_of("/\\");
  const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::size_t dot = name.find_last_of('.');
  if (dot == std::string::npos || dot == 0) return {};
  return name.substr(dot);
}

}  // namespace detail

// First free "name (1).ext", "name (2).ext", ... (the card's example is
// "xxx (1).txt"; CollisionResolver starts at 2 for a different, pre-existing
// contract -- this function owns the overwrite-rename spelling).
inline std::string RenameCandidate(const std::string& finalPath, int suffix) {
  return detail::StemOf(finalPath) + " (" + std::to_string(suffix) + ")" +
         detail::ExtensionOf(finalPath);
}

// isDirectory: directories merge instead of being renamed/skipped, which is the
// pre-existing extraction contract (SmartExtractionPlanner comment).
inline Decision Decide(const std::string& finalPath, bool isDirectory, Mode mode,
                       const ExistsFn& exists, const PromptFn& prompt = {}) {
  const auto taken = [&exists](const std::string& path) {
    return exists && exists(path);
  };
  if (isDirectory || !taken(finalPath)) {
    return Decision{Choice::Replace, finalPath};
  }
  Choice choice = Choice::Replace;
  switch (mode) {
    case Mode::Replace: choice = Choice::Replace; break;
    case Mode::Skip: choice = Choice::Skip; break;
    case Mode::Rename: choice = Choice::Rename; break;
    case Mode::Ask: choice = prompt ? prompt(finalPath) : Choice::Skip; break;
  }
  if (choice == Choice::Skip) return Decision{Choice::Skip, std::string()};
  if (choice == Choice::Replace) return Decision{Choice::Replace, finalPath};
  for (int suffix = 1;; ++suffix) {
    const std::string candidate = RenameCandidate(finalPath, suffix);
    if (!taken(candidate)) return Decision{Choice::Rename, candidate};
  }
}

}  // namespace prism::overwrite
