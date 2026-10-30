// PrismArchive -- PrismCore
// Runtime safety guards (blueprint ch. 7.1):
//   * BombFuse       -- asymmetric expansion-rate circuit breaker (42.zip class)
//   * WriteGuard     -- per-file Zip Slip / symlink-escape gate
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>
#include <string>
#include <string_view>
#include <vector>

#include "PathSafety.hpp"

namespace prism {

struct BombLimits {
  uint64_t windowBytes = 4ull * 1024 * 1024;  // sliding window over *compressed* bytes
  double maxRatio = 1000.0;                   // decompressed / compressed
  double maxDiskFraction = 0.85;              // stop before the disk is full
  uint64_t hardCeilingBytes = 0;              // 0 = unlimited
};

// Sliding-window expansion monitor fed from
// IArchiveExtractCallback::SetCompleted(completeValue) + the packed byte counter.
class BombFuse {
 public:
  explicit BombFuse(BombLimits limits = {}) : limits_(limits) {}

  // Returns false once the fuse has tripped; caller must abort the extraction.
  bool Feed(uint64_t compressedBytes, uint64_t producedBytes, uint64_t freeDiskBytes) {
    if (tripped_) return false;

    totalCompressed_ += compressedBytes;
    totalProduced_ += producedBytes;
    window_.push_back({compressedBytes, producedBytes});
    winCompressed_ += compressedBytes;
    winProduced_ += producedBytes;

    // Slide the window: keep at most windowBytes of compressed input in view.
    while (!window_.empty() && winCompressed_ > limits_.windowBytes &&
           window_.size() > 1) {
      winCompressed_ -= window_.front().first;
      winProduced_ -= window_.front().second;
      window_.pop_front();
    }

    if (winCompressed_ >= limits_.windowBytes) {
      const double ratio = static_cast<double>(winProduced_) /
                           static_cast<double>(winCompressed_ > 0 ? winCompressed_ : 1);
      if (ratio > limits_.maxRatio && winProduced_ > (64ull * 1024 * 1024)) {
        Trip("expansion ratio " + Trim(ratio) + ":1 over a " +
             std::to_string(limits_.windowBytes / (1024 * 1024)) +
             " MiB window exceeded the " + Trim(limits_.maxRatio) + ":1 limit");
        return false;
      }
    }

    if (freeDiskBytes > 0 && totalProduced_ >= static_cast<uint64_t>(freeDiskBytes *
                                                                     limits_.maxDiskFraction)) {
      Trip("output would consume >= " + Trim(limits_.maxDiskFraction * 100.0) +
           "% of the free space on the target volume");
      return false;
    }

    if (limits_.hardCeilingBytes > 0 && totalProduced_ > limits_.hardCeilingBytes) {
      Trip("output exceeded the configured hard ceiling of " +
           std::to_string(limits_.hardCeilingBytes) + " bytes");
      return false;
    }
    return true;
  }

  bool tripped() const { return tripped_; }
  const std::string& reason() const { return reason_; }
  uint64_t totalProduced() const { return totalProduced_; }
  uint64_t totalCompressed() const { return totalCompressed_; }
  double currentRatio() const {
    if (winCompressed_ == 0) return 0.0;
    return static_cast<double>(winProduced_) / static_cast<double>(winCompressed_);
  }

 private:
  void Trip(std::string why) {
    tripped_ = true;
    reason_ = std::move(why);
  }

  static std::string Trim(double v) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f", v);
    return std::string(buf);
  }

  struct Sample {
    uint64_t first, second;
  };

  BombLimits limits_;
  std::deque<Sample> window_;
  uint64_t winCompressed_ = 0;
  uint64_t winProduced_ = 0;
  uint64_t totalCompressed_ = 0;
  uint64_t totalProduced_ = 0;
  bool tripped_ = false;
  std::string reason_;
};

// Per-entry write gate. Blueprint ch. 7.1 defence #2: verify the final path is
// still inside the target root *after* all smart-extraction rewriting, and
// defer symlinks to a post-pass.
class WriteGuard {
 public:
  explicit WriteGuard(std::string targetRoot) : root_(NormalizeArchivePath(targetRoot)) {}

  bool Approve(std::string_view finalPath, std::string* normalizedOut = nullptr) const {
    // Resolve ".." lexically first: a path that merely *starts* with the root
    // prefix can still climb out of it (C:/out/root/../../Windows/evil.dll).
    std::vector<std::string> stack;
    for (const std::string& part : SplitPath(NormalizeArchivePath(finalPath))) {
      if (part.empty() || part == ".") continue;
      if (part == "..") {
        if (!stack.empty()) stack.pop_back();
        continue;
      }
      stack.push_back(part);
    }
    std::string canon;
    for (const std::string& part : stack) canon = canon.empty() ? part : JoinPath(canon, part);
    if (normalizedOut) *normalizedOut = canon;
    if (canon.empty()) return false;
    return IsContainedIn(root_, canon);
  }

  // Symlink/reparse entries are collected and replayed only after every regular
  // file has landed, so a link can never redirect an earlier write out of root.
  void DeferLink(uint32_t index, std::string_view path) {
    deferred_.push_back({index, std::string(path)});
  }
  const std::vector<std::pair<uint32_t, std::string>>& deferredLinks() const {
    return deferred_;
  }

  const std::string& root() const { return root_; }

 private:
  std::string root_;
  std::vector<std::pair<uint32_t, std::string>> deferred_;
};

}  // namespace prism
