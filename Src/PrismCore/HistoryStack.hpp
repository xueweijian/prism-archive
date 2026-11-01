// PrismArchive -- PrismCore
// Navigation history for the gallery (blueprint ch. 9.1 row 6:
// "Alt+Left/Right / mouse side buttons / two-finger swipe").
//
// The archive view pushes a folder on every transition and pops on Back/
// Forward. Two properties are contractual, not incidental:
//   * ADJACENT de-duplication: re-entering the folder you are already in must
//     not create a second history entry (a crumb click on the current level,
//     or a double-click that lands on the same folder, would otherwise make
//     the Back button appear broken: the first press does nothing visible);
//   * the capacity is a PROMISE, not a hint (CONTRACTS D17): after N visits
//     BackCount() == capacity exactly, never capacity-1. A ring that silently
//     keeps one less entry than it advertises is the same class of bug that
//     cost a re-decode in the drag-out seek work.
//
// The stack stores PrismCore-normalized folders, so "" and "/" are the same
// (archive root) and "a//b" cannot masquerade as a different level.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "PathSafety.hpp"

namespace prism::history {

class Stack {
 public:
  // 64 levels is deeper than any archive a human navigates by hand, and it
  // keeps the per-entry cost (one normalized path) negligible.
  static constexpr size_t kDefaultCapacity = 64;

  explicit Stack(size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  size_t capacity() const { return capacity_; }
  const std::string& Current() const { return current_; }
  bool CanBack() const { return !back_.empty(); }
  bool CanForward() const { return !forward_.empty(); }
  size_t BackCount() const { return back_.size(); }
  size_t ForwardCount() const { return forward_.size(); }

  // Record a landing in a folder. Returns false when it was a no-op: the
  // folder is the current one (de-dupe), which also leaves the forward stack
  // intact -- "I clicked where I already am" is not a navigation.
  bool Visit(std::string_view folder) {
    std::string normalized = prism::NormalizeArchivePath(folder);
#ifndef PRISM_HISTORY_MUTANT_NO_DEDUPE
    if (normalized == current_) return false;
#endif
    back_.push_back(current_);
    Trim(back_);
    current_ = std::move(normalized);
    forward_.clear();  // a new branch: the old future is unreachable
    return true;
  }

  // Alt+Left. Returns false at the beginning of history (out is untouched).
  bool Back(std::string* out) {
    if (back_.empty()) return false;
    forward_.push_back(current_);
    Trim(forward_);
    current_ = std::move(back_.back());
    back_.pop_back();
    if (out) *out = current_;
    return true;
  }

  // Alt+Right. Returns false at the end of history.
  bool Forward(std::string* out) {
    if (forward_.empty()) return false;
    back_.push_back(current_);
    Trim(back_);
    current_ = std::move(forward_.back());
    forward_.pop_back();
    if (out) *out = current_;
    return true;
  }

  void Clear() {
    back_.clear();
    forward_.clear();
    current_.clear();
  }

 private:
  // Drop the OLDEST entries once the promise is exceeded. The loop (not an
  // if) is deliberate: capacity_ can be small and Trim runs on every push.
  static void Trim(std::vector<std::string>& side, size_t capacity) {
    while (side.size() > capacity) side.erase(side.begin());
  }
  void Trim(std::vector<std::string>& side) {
#ifdef PRISM_HISTORY_MUTANT_OFF_BY_ONE
    Trim(side, capacity_ + 1);  // mutant: keeps one entry more than promised
#else
    Trim(side, capacity_);
#endif
  }

  std::vector<std::string> back_;
  std::vector<std::string> forward_;
  std::string current_;  // "" == the archive root (a real, visitable level)
  size_t capacity_ = kDefaultCapacity;
};

}  // namespace prism::history
