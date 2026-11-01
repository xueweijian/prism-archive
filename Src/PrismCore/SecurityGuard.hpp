// PrismArchive -- PrismCore
// Runtime safety guards (blueprint ch. 7.1):
//   * BombFuse      -- asymmetric expansion-rate + disk-proximity circuit breaker
//   * WriteGuard    -- per-file Zip Slip / symlink-escape gate
//   * Motw policy   -- inherit the source archive's Zone.Identifier (7.1 #1)
//   * AMSI policy   -- Allow / Block / Unavailable, never folded into Allow (7.1 #1)
//
// Everything here is decision-only: no <windows.h>, no filesystem, no AMSI
// import. The Win32 mechanics (ADS read/write + read-back, amsi.dll) live in
// Src/PrismWin and are exercised by Tests/win/test_motw_stamp.cpp. That split is
// what lets the *policy* run on Linux in seconds (Tests/test_amsi_policy.cpp)
// while the system calls stay on one platform layer.
#pragma once

#include <cstdint>
#include <cstdio>
#include <deque>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "PathSafety.hpp"

namespace prism {

// ---------------------------------------------------------------------------
// Negative-control hooks (mutation builds, see Tests/CMakeLists.txt).
//
// Each constant is flipped by a separate test binary registered with
// WILL_FAIL, so if a mutation ever survives -- i.e. the suite stopped observing
// that rule -- CTest itself turns red. Injection is compile-time, not a text
// edit, because a sed that silently fails to match is indistinguishable from a
// sharp test (CONTRACTS D3).
// ---------------------------------------------------------------------------
namespace security_mutation {
#if defined(PRISM_SEC_MUTANT_UNAVAILABLE_IS_ALLOW)
constexpr bool kUnavailableIsAllow = true;
#else
constexpr bool kUnavailableIsAllow = false;
#endif
#if defined(PRISM_SEC_MUTANT_NEVER_STAMP)
constexpr bool kNeverStamp = true;
#else
constexpr bool kNeverStamp = false;
#endif
#if defined(PRISM_SEC_MUTANT_IGNORE_PER_FILE_RATIO)
constexpr bool kIgnorePerFileRatio = true;
#else
constexpr bool kIgnorePerFileRatio = false;
#endif
#if defined(PRISM_SEC_MUTANT_IGNORE_DISK_FRACTION)
constexpr bool kIgnoreDiskFraction = true;
#else
constexpr bool kIgnoreDiskFraction = false;
#endif
#if defined(PRISM_SEC_MUTANT_IGNORE_CHUNK_OVERLAP)
constexpr bool kIgnoreChunkOverlap = true;
#else
constexpr bool kIgnoreChunkOverlap = false;
#endif
}  // namespace security_mutation

// ---------------------------------------------------------------------------
// One verdict vocabulary for every security decision, so the wiring layer has
// exactly one shape to render (blueprint's red "security warning" glass card).
// ---------------------------------------------------------------------------
enum class SecurityAction : uint8_t { Allow, Warn, Block };

inline const char* ToString(SecurityAction action) {
  switch (action) {
    case SecurityAction::Allow: return "Allow";
    case SecurityAction::Warn: return "Warn";
    case SecurityAction::Block: return "Block";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, SecurityAction action) {
  return os << ToString(action);
}

// ===========================================================================
// Mark-of-the-Web inheritance policy (blueprint 7.1 #1)
// ===========================================================================
//
// Windows stores the download origin in the NTFS alternate data stream
// "<file>:Zone.Identifier" as INI text:
//
//   [ZoneTransfer]
//   ZoneId=3
//   HostUrl=https://example.com/Invoice.zip
//   ReferrerUrl=https://example.com/
//
// The contract is *verbatim* inheritance: byte-for-byte the same blob, so
// SmartScreen still sees ZoneId=3 + the original URLs. Parsing exists only to
// VERIFY the copy; it is never used to re-serialise the stream (a re-serialised
// blob could drop unknown fields).
struct MotwFields {
  bool present = false;
  int zoneId = -1;  // 3 = Internet (URLZONE_INTERNET)
  std::string hostUrl;
  std::string referrerUrl;
};

namespace motw_text {

inline bool AsciiIEquals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = a[i];
    char cb = b[i];
    if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca - 'A' + 'a');
    if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb - 'A' + 'a');
    if (ca != cb) return false;
  }
  return true;
}

inline std::string_view TrimAscii(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

}  // namespace motw_text

// Parses the Zone.Identifier text. Tolerant on line endings (the stream is CRLF
// on disk) and key case, strict on ZoneId being decimal digits.
inline MotwFields ParseMotwFields(std::string_view zoneIdentifier) {
  MotwFields fields;
  size_t pos = 0;
  while (pos < zoneIdentifier.size()) {
    const size_t newline = zoneIdentifier.find('\n', pos);
    const size_t end = (newline == std::string_view::npos) ? zoneIdentifier.size() : newline;
    std::string_view line = zoneIdentifier.substr(pos, end - pos);
    const size_t eq = line.find('=');
    if (eq != std::string_view::npos) {
      const std::string_view key = motw_text::TrimAscii(line.substr(0, eq));
      const std::string_view value = motw_text::TrimAscii(line.substr(eq + 1));
      if (motw_text::AsciiIEquals(key, "ZoneId")) {
        // Reject overflow instead of wrapping into a bogus zone.
        int id = 0;
        bool ok = !value.empty();
        for (const char c : value) {
          if (c < '0' || c > '9' || id > 100000) {
            ok = false;
            break;
          }
          id = id * 10 + (c - '0');
        }
        if (ok) {
          fields.zoneId = id;
          fields.present = true;
        }
      } else if (motw_text::AsciiIEquals(key, "HostUrl")) {
        fields.hostUrl.assign(value);
        fields.present = true;
      } else if (motw_text::AsciiIEquals(key, "ReferrerUrl")) {
        fields.referrerUrl.assign(value);
        fields.present = true;
      }
    }
    if (newline == std::string_view::npos) break;
    pos = newline + 1;
  }
  return fields;
}

// What the platform layer found when it opened "<archive>:Zone.Identifier".
enum class MotwSource : uint8_t {
  Absent,      // the stream does not exist: the archive was never marked
  Present,     // read successfully; the blob must be cloned verbatim
  Unreadable   // exists or may exist, but could not be read (sharing lock, I/O)
};

inline const char* ToString(MotwSource source) {
  switch (source) {
    case MotwSource::Absent: return "Absent";
    case MotwSource::Present: return "Present";
    case MotwSource::Unreadable: return "Unreadable";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, MotwSource source) {
  return os << ToString(source);
}

struct MotwPolicy {
  // A failed stamp leaves an extracted file WITHOUT the download mark the
  // blueprint requires it to carry. "We warned nobody" is not acceptable, so the
  // default is Warn; Block is opt-in because aborting an 80k-entry extraction
  // over one ADS write is a product trade-off, not a security default.
  SecurityAction onStampFailure = SecurityAction::Warn;
};

struct MotwPlan {
  bool stamp = false;              // attempt to clone the stream onto this file
  SecurityAction action = SecurityAction::Allow;  // verdict for the read side
  const char* reason = "";
};

// The decision the wiring layer asks before stamping every written file.
inline MotwPlan PlanMotwInheritance(MotwSource source, const MotwPolicy& policy = {}) {
  (void)policy;
  switch (source) {
    case MotwSource::Absent:
      // Stamping here would INVENT provenance the archive never had.
      return {false, SecurityAction::Allow, "source archive carries no Zone.Identifier"};
    case MotwSource::Present:
      if (security_mutation::kNeverStamp) {
        return {false, SecurityAction::Allow, "MUTANT: inheritance disabled"};
      }
      return {true, SecurityAction::Allow, "inherit the source Zone.Identifier verbatim"};
    case MotwSource::Unreadable:
      // We do not know whether a mark exists, so a silent Allow would claim
      // knowledge we do not have. Warn, and do not invent or drop a mark.
      return {false, SecurityAction::Warn, "source Zone.Identifier could not be read"};
  }
  return {false, SecurityAction::Warn, "unknown MoTW source state"};
}

// Verdict for "the clone was attempted and failed" (the platform layer reports
// the bool). Kept separate from the plan so a caller cannot accidentally treat
// a failure as success by confusing the two.
inline SecurityAction MotwStampFailureAction(const MotwPolicy& policy = {}) {
  return policy.onStampFailure;
}

// ===========================================================================
// AMSI pre-scan policy (blueprint 7.1 #1)
// ===========================================================================
//
// AmsiScanBuffer's AMSI_RESULT is documented (learn.microsoft.com/windows/win32/
// api/amsi/ne-amsi-amsi_result) as:
//   0                              CLEAN: no detection, stable result
//   1                              NOT_DETECTED: no detection, may change later
//   0x4000..0x4FFF (16384..20479)  BLOCKED_BY_ADMIN: policy says block
//   >= 0x8000 (32768)              DETECTED: considered malware, block it
//   2..32767 outside that range    provider-specific risk estimate
//
// The last bucket is neither clean nor a documented detection: mapping it to
// Allow would silently wave through content AMSI explicitly called "riskier to
// continue". It maps to Unavailable (no definitive verdict), which warns.
enum class AmsiVerdict : uint8_t {
  Allow,        // a provider completed the scan and found nothing
  Block,        // detection or administrator block
  Unavailable   // not scanned, scan failed, or no definitive verdict
};

inline const char* ToString(AmsiVerdict verdict) {
  switch (verdict) {
    case AmsiVerdict::Allow: return "Allow";
    case AmsiVerdict::Block: return "Block";
    case AmsiVerdict::Unavailable: return "Unavailable";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, AmsiVerdict verdict) {
  return os << ToString(verdict);
}

constexpr long kAmsiResultClean = 0;
constexpr long kAmsiResultNotDetected = 1;
constexpr long kAmsiResultBlockedByAdminStart = 0x4000;
constexpr long kAmsiResultBlockedByAdminEnd = 0x4FFF;
constexpr long kAmsiResultDetected = 0x8000;

// The documented mapping, in core so it is unit-testable without amsi.dll.
inline AmsiVerdict ClassifyAmsiResult(long amsiResult) {
  if (amsiResult == kAmsiResultClean || amsiResult == kAmsiResultNotDetected) {
    return AmsiVerdict::Allow;
  }
  if (amsiResult >= kAmsiResultDetected) return AmsiVerdict::Block;
  if (amsiResult >= kAmsiResultBlockedByAdminStart && amsiResult <= kAmsiResultBlockedByAdminEnd) {
    return AmsiVerdict::Block;
  }
  // Anything else is an estimate or an out-of-contract value: no definitive
  // answer, therefore not Allow.
  return AmsiVerdict::Unavailable;
}

struct AmsiPolicy {
  SecurityAction onBlock = SecurityAction::Block;
  // "We did not scan" is not "we scanned and it was clean". Configuring Allow
  // here is clamped to Warn on purpose: the blueprint requires an alert, and a
  // silent allow is exactly the failure this guard exists to prevent.
  SecurityAction onUnavailable = SecurityAction::Warn;
};

inline SecurityAction DecideAmsiAction(AmsiVerdict verdict, const AmsiPolicy& policy = {}) {
  switch (verdict) {
    case AmsiVerdict::Allow:
      return SecurityAction::Allow;
    case AmsiVerdict::Block:
      return policy.onBlock;
    case AmsiVerdict::Unavailable: {
      if (security_mutation::kUnavailableIsAllow) return SecurityAction::Allow;
      const SecurityAction configured = policy.onUnavailable;
      return configured == SecurityAction::Allow ? SecurityAction::Warn : configured;
    }
  }
  return SecurityAction::Warn;
}

// Only executable / script / macro-capable payloads are worth an AMSI pass.
// Scanning every extracted .jpg would pay a provider round-trip per file for no
// security value; skipping .exe would defeat the guard. The list is deliberately
// explicit (no "contains a dot" heuristic) so it is auditable.
inline bool IsAmsiTarget(std::string_view writtenPath) {
  const size_t slash = writtenPath.find_last_of("/\\");
  const size_t dot = writtenPath.find_last_of('.');
  if (dot == std::string_view::npos || (slash != std::string_view::npos && dot < slash)) {
    return false;
  }
  const std::string_view ext = writtenPath.substr(dot + 1);
  if (ext.empty() || ext.size() > 8) return false;
  static constexpr std::string_view kTargets[] = {
      // Executable images and loaders.
      "exe", "dll", "sys", "scr", "com", "cpl", "ocx", "drv",
      // Scripts the shell executes directly.
      "bat", "cmd", "ps1", "psm1", "vbs", "vbe", "js", "jse", "wsf", "wsh", "hta",
      // Installers.
      "msi", "msp",
      // Macro-capable Office documents (OOXML macro variants + legacy binary).
      "docm", "dotm", "xlsm", "xltm", "xlam", "xlsb", "pptm", "potm", "ppam", "ppsm",
      "sldm", "doc", "dot", "xls", "xlt", "ppt", "pot"};
  for (const std::string_view target : kTargets) {
    if (motw_text::AsciiIEquals(ext, target)) return true;
  }
  return false;
}

// AMSI takes a ULONG and a single in-memory buffer. A full-file scan is chunked
// with overlap so (a) memory stays bounded for a 4 GiB payload, and (b) a
// signature that straddles a chunk boundary is still fully inside one buffer.
struct AmsiChunk {
  uint64_t offset = 0;
  uint64_t length = 0;
};

inline std::vector<AmsiChunk> PlanAmsiChunks(uint64_t totalBytes,
                                             uint64_t chunkBytes = 4ull * 1024 * 1024,
                                             uint64_t overlapBytes = 64ull * 1024) {
  std::vector<AmsiChunk> plan;
  if (totalBytes == 0) return plan;
  if (chunkBytes < 2) chunkBytes = 2;
  if (security_mutation::kIgnoreChunkOverlap) overlapBytes = 0;
  if (overlapBytes >= chunkBytes) overlapBytes = chunkBytes / 2;

  uint64_t offset = 0;
  for (;;) {
    uint64_t length = totalBytes - offset;
    if (length > chunkBytes) length = chunkBytes;
    plan.push_back({offset, length});
    const uint64_t end = offset + length;
    if (end >= totalBytes) break;
    offset = end - overlapBytes;
  }
  return plan;
}

// ===========================================================================
// Zip-bomb fuse (blueprint 7.1 #3)
// ===========================================================================
//
// The blueprint's scenario is "single-file ratio > 1000:1 AND output approaching
// 85% of the volume's free space". Three independent detectors are implemented,
// because any one of them failing must not leave the machine unprotected:
//
//   1. per-entry ratio   -- catches one giant bomb entry diluted by incompressible
//                           neighbours (the aggregate window ratio then stays low);
//   2. window ratio      -- catches many medium-ratio entries that never look big
//                           one at a time;
//   3. disk proximity    -- catches a bomb of any ratio as the volume fills.
//
// When 1 or 2 fires TOGETHER with 3 in the same sample, critical() is latched so
// the UI can raise the blueprint's red glass alert. Both conditions are therefore
// present AND observable, without weakening either guard to an "or" that could be
// trivially dodged (a 2000:1 entry on a 2 TB volume is still a bomb).
struct BombLimits {
  uint64_t windowBytes = 4ull * 1024 * 1024;  // sliding window over *compressed* bytes
  double maxRatio = 1000.0;                   // decompressed / compressed
  double maxDiskFraction = 0.85;              // stop before the disk is full
  uint64_t hardCeilingBytes = 0;              // 0 = unlimited
  // Ratios measured on less than this much output are noise (a 40-byte file
  // "expanding" to 40 KiB is not a bomb). 64 MiB at 1000:1 is 64 KiB packed.
  uint64_t minRatioSampleBytes = 64ull * 1024 * 1024;
};

class BombFuse {
 public:
  explicit BombFuse(BombLimits limits = {}) : limits_(limits) {}

  // Call at each entry boundary (7-Zip: SetOperationResult). packedBytes is the
  // entry's size in the archive when it is known up front (kpidPackSize); pass 0
  // and feed the packed deltas through Feed() when it is not. Without one of the
  // two the per-entry ratio cannot be judged and only the window/disk guards run.
  void BeginFile(uint64_t packedBytes = 0) {
    filePacked_ = packedBytes;
    fileCompressed_ = packedBytes;
    fileProduced_ = 0;
  }

  // Returns false once the fuse has tripped; caller must abort the extraction.
  // freeDiskBytes is the free space on the target volume (the first value seen is
  // latched as the extraction-start baseline, so a caller that passes the
  // *current* free space does not make the 85% line chase its own tail).
  bool Feed(uint64_t compressedBytes, uint64_t producedBytes, uint64_t freeDiskBytes) {
    if (tripped_) return false;

    totalCompressed_ += compressedBytes;
    totalProduced_ += producedBytes;
    fileProduced_ += producedBytes;
    if (filePacked_ == 0) fileCompressed_ += compressedBytes;

    window_.push_back({compressedBytes, producedBytes});
    winCompressed_ += compressedBytes;
    winProduced_ += producedBytes;

    // Slide the window: keep at most windowBytes of compressed input in view.
    while (!window_.empty() && winCompressed_ > limits_.windowBytes && window_.size() > 1) {
      winCompressed_ -= window_.front().first;
      winProduced_ -= window_.front().second;
      window_.pop_front();
    }

    if (freeDiskLimitBytes_ == 0 && freeDiskBytes > 0) freeDiskLimitBytes_ = freeDiskBytes;

    bool windowRatioExceeded = false;
    if (winCompressed_ >= limits_.windowBytes && winProduced_ >= limits_.minRatioSampleBytes) {
      windowRatioExceeded = Ratio(winProduced_, winCompressed_) > limits_.maxRatio;
    }

    bool fileRatioExceeded = false;
    if (!security_mutation::kIgnorePerFileRatio && fileCompressed_ > 0 &&
        fileProduced_ >= limits_.minRatioSampleBytes) {
      fileRatioExceeded = Ratio(fileProduced_, fileCompressed_) > limits_.maxRatio;
    }

    bool diskExceeded = false;
    if (!security_mutation::kIgnoreDiskFraction && freeDiskLimitBytes_ > 0) {
      const double budget =
          static_cast<double>(freeDiskLimitBytes_) * limits_.maxDiskFraction;
      diskExceeded = static_cast<double>(totalProduced_) >= budget;
    }

    // The blueprint's exact scenario: anomalous expansion AND the volume about to
    // fill, observed in the same sample.
    critical_ = (windowRatioExceeded || fileRatioExceeded) && diskExceeded;

    if (windowRatioExceeded || fileRatioExceeded) {
      std::string why;
      if (fileRatioExceeded) {
        why = "single entry expanded " + Trim(Ratio(fileProduced_, fileCompressed_)) + ":1";
      }
      if (windowRatioExceeded) {
        if (!why.empty()) why += "; ";
        why += "window expanded " + Trim(Ratio(winProduced_, winCompressed_)) + ":1 over " +
               std::to_string(limits_.windowBytes / (1024 * 1024)) + " MiB";
      }
      if (diskExceeded) {
        why += " AND output reached " + Trim(limits_.maxDiskFraction * 100.0) +
               "% of the free space at extraction start";
      }
      Trip(why + " (limit " + Trim(limits_.maxRatio) + ":1)");
      return false;
    }

    if (diskExceeded) {
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
  // True only when the ratio anomaly and the disk proximity were seen together.
  bool critical() const { return critical_; }
  const std::string& reason() const { return reason_; }
  uint64_t totalProduced() const { return totalProduced_; }
  uint64_t totalCompressed() const { return totalCompressed_; }
  uint64_t fileProduced() const { return fileProduced_; }
  uint64_t extractionStartFreeBytes() const { return freeDiskLimitBytes_; }
  double currentRatio() const {
    if (winCompressed_ == 0) return 0.0;
    return static_cast<double>(winProduced_) / static_cast<double>(winCompressed_);
  }
  // Diagnostic: whether the WINDOW detector had enough packed input and output
  // to judge at all. It is false while a single entry is still smaller than the
  // window, which is exactly why the per-entry detector has to exist.
  bool windowArmed() const {
    return winCompressed_ >= limits_.windowBytes && winProduced_ >= limits_.minRatioSampleBytes;
  }
  uint64_t windowCompressed() const { return winCompressed_; }
  double currentFileRatio() const {
    if (fileCompressed_ == 0) return 0.0;
    return static_cast<double>(fileProduced_) / static_cast<double>(fileCompressed_);
  }

 private:
  static double Ratio(uint64_t produced, uint64_t compressed) {
    return static_cast<double>(produced) / static_cast<double>(compressed == 0 ? 1 : compressed);
  }

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
  uint64_t filePacked_ = 0;
  uint64_t fileCompressed_ = 0;
  uint64_t fileProduced_ = 0;
  uint64_t freeDiskLimitBytes_ = 0;
  bool tripped_ = false;
  bool critical_ = false;
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
