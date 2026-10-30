// PrismArchive -- PrismCore
// PasswordRing: the decision core that turns password hints into a
// deterministic, budgeted, non-leaking verification plan (blueprint ch. 4.5).
//
// Storage lives elsewhere (PasswordVault.hpp: DPAPI-backed on Windows); this
// module owns the POLICY:
//   * which candidate to try first (source priors + history + stable order),
//   * which archive entry proves a password (least-work encrypted probe),
//   * which verification route a format allows (header encryption forces a
//     full Open; body encryption allows a single-entry probe),
//   * how many guesses may run silently before the UI must show progress
//     (format-aware: see PasswordVault.hpp KdfClassOf/SilentAttemptBudget),
//   * and secrecy: diagnostics redact the secret, buffers can be zeroed.
//
// Everything is pure and platform-free so all of it is testable on Linux.
#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PasswordVault.hpp"  // PasswordKdfClass / KdfClassOf / budgets
#include "PathSafety.hpp"     // ExtensionOf

namespace prism::pwd {

// Where a candidate came from. Ordered by prior probability of being right --
// a hint written next to the archive beats blind history beats a human typing.
enum class Source : uint8_t {
  FilenameHint = 0,   // "素材包_密码123456.7z"
  ArchiveComment = 1, // ZIP/7z archive comment field
  SidecarText = 2,    // sibling .txt/.nfo description files
  VaultHistory = 3,   // previously successful passwords (user consented)
  UserInput = 4       // typed this session
};

struct Candidate {
  std::string text;
  Source source = Source::UserInput;
  int historyHits = 0;  // filled by Rank() from the history map
};

class PasswordRing {
 public:
  // Prior weight per source (bigger = likelier). Kept as simple integers so
  // the order is exact and reviewable; rescaled weights only matter when two
  // sources disagree, and the constants say FilenameHint > ArchiveComment >
  // SidecarText > VaultHistory > UserInput.
  static int SourcePrior(Source s) {
    switch (s) {
      case Source::FilenameHint: return 50;
      case Source::ArchiveComment: return 40;
      case Source::SidecarText: return 30;
      case Source::VaultHistory: return 20;
      case Source::UserInput: return 10;
    }
    return 0;
  }

  // Deterministic ranking. Order contract (tests pin the exact sequence):
  //   1. prior weight desc (history hits add a bounded boost)
  //   2. history hit count desc (within the same prior)
  //   3. source ordinal asc, then text lexicographic asc -- the tie-breakers
  //      make the plan reproducible, which is what makes failure reports
  //      ("tried these 12, in this order") meaningful.
  // Duplicates collapse to one slot held by the strongest source.
  static std::vector<Candidate> Rank(
      std::vector<Candidate> candidates,
      const std::map<std::string, int>& history = {}) {
    for (auto& c : candidates) {
      auto hit = history.find(c.text);
      c.historyHits = hit == history.end() ? 0 : hit->second;
    }
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                       if (a.text != b.text) return a.text < b.text;  // dedup pass
                       return SourcePrior(a.source) > SourcePrior(b.source);
                     });
    // Dedup: strongest source wins (stable_sort kept it first in each run).
    candidates.erase(std::unique(candidates.begin(), candidates.end(),
                                 [](const Candidate& a, const Candidate& b) {
                                   return a.text == b.text;
                                 }),
                     candidates.end());
    std::stable_sort(candidates.begin(), candidates.end(),
                     [](const Candidate& a, const Candidate& b) {
                       const int wa = SourcePrior(a.source) + HistoryBoost(a.historyHits);
                       const int wb = SourcePrior(b.source) + HistoryBoost(b.historyHits);
                       if (wa != wb) return wa > wb;
                       if (a.historyHits != b.historyHits) return a.historyHits > b.historyHits;
                       if (a.source != b.source) return static_cast<int>(a.source) < static_cast<int>(b.source);
                       return a.text < b.text;
                     });
    return candidates;
  }

  // Pick the entry whose decryption proves a password: an encrypted FILE, the
  // smallest one (least decompression to verify), ties broken by lowest block
  // (least solid-block commitment) then lowest index (reproducibility).
  // nullopt = nothing here can verify anything -- callers must treat that as
  // "no silent fallback", same discipline as the drag-drop refusals.
  static std::optional<uint32_t> PickVerificationProbe(
      const std::vector<ArchiveItem>& items) {
    std::optional<uint32_t> best;
    for (const auto& it : items) {
      if (it.isDir || !it.encrypted) continue;
      if (!best) {
        best = it.index;
        continue;
      }
      const ArchiveItem& cur = items[*best];
      if (it.size < cur.size ||
          (it.size == cur.size && it.block < cur.block) ||
          (it.size == cur.size && it.block == cur.block && it.index < cur.index)) {
        best = it.index;
      }
    }
    return best;
  }

  // Which verification route the format allows.
  //  * Header-encrypted archives keep their catalog locked until a password
  //    opens the whole container: there is nothing to probe piecemeal.
  //  * Body-encrypted archives expose names, so ONE small entry (one AES
  //    block / one ZipCrypto header) verifies a guess at a fraction of the cost.
  //  * No candidates left => Decline. Loud, with a reason -- a silent "skip"
  //    here would strand the user in an empty password dialog forever.
  enum class ProbeStrategy : uint8_t { FullOpen, BodyProbe, Decline };

  static ProbeStrategy StrategyFor(bool headerEncrypted,
                                   size_t candidatesLeft) {
    if (candidatesLeft == 0) return ProbeStrategy::Decline;
    if (headerEncrypted) return ProbeStrategy::FullOpen;
    return ProbeStrategy::BodyProbe;
  }

  static std::string ExhaustionReason(PasswordKdfClass kdf, size_t budget) {
    (void)kdf;  // kdf only shapes the message below
    // kdf shapes the MESSAGE (why the budget was what it was); the budget
    // itself came from SilentAttemptBudget(kdf).
    (void)kdf;
    if (kdf == PasswordKdfClass::AesKeyDerivation) {
      return "password ring exhausted after " + std::to_string(budget) +
             " candidates (AES key derivation is ~10-40 ms per guess; showing "
             "the password dialog instead of stalling)";
    }
    return "password ring exhausted after " + std::to_string(budget) + " candidates";
  }

  struct AttemptPlan {
    std::vector<Candidate> ordered;
    int concurrency = 1;
  };

  // Cheap guesses (ZipCrypto) parallelize across every core; AES key
  // derivation guesses fight for the same SHA-256 pipeline, so beyond a few
  // workers throughput goes DOWN while the UI stalls -- clamp to 4.
  static AttemptPlan PlanAttempts(const std::vector<Candidate>& ranked,
                                  PasswordKdfClass kdf, int threads) {
    AttemptPlan plan;
    plan.ordered = ranked;
    const int wide = kdf == PasswordKdfClass::ZipCrypto ? 8 : 4;
    plan.concurrency = (std::max)(1, (std::min)(threads <= 0 ? 1 : threads, wide));
    return plan;
  }

  // "3/12" -- the micro-label for the non-blocking ring-progress pill.
  static std::string ProgressLabel(size_t done, size_t total) {
    return std::to_string(done) + "/" + std::to_string(total);
  }

  // Secrets must never reach logs, exception text, or crash dumps. Redaction
  // removes full AND partial matches (a half-leaked password is a leak).
  static std::string RedactSecrets(const std::string& text,
                                  const std::vector<std::string>& secrets) {
    std::string out = text;
    for (const std::string& secret : secrets) {
      if (secret.size() < 3) continue;  // too short to match anything safely
      size_t at;
      while ((at = out.find(secret)) != std::string::npos) {
        out.replace(at, secret.size(), "[redacted]");
      }
      // Partial leak: any proper substring of length >= 4 also goes.
      for (size_t len = secret.size() - 1; len >= 4; --len) {
        for (size_t i = 0; i + len <= secret.size(); ++i) {
          const std::string piece = secret.substr(i, len);
          size_t p;
          while ((p = out.find(piece)) != std::string::npos) {
            out.replace(p, piece.size(), "[redacted]");
          }
        }
      }
    }
    return out;
  }

  // Best-effort scrub for in-memory candidates. A std::string may have moved
  // its buffer around during growth, so this is defense in depth (the real
  // guarantee is DPAPI on the Windows side) -- but a zeroed buffer is still
  // strictly better than leaving plaintext in a freed heap block.
  static void SecureZero(std::string& s) {
    if (!s.empty()) {
      volatile char* p = s.data();
      for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    }
    s.clear();
  }

 private:
  static int HistoryBoost(int hits) {
    // Bounded so one lucky password can bury fresh, higher-prior hints
    // forever: history reorders within its own prior tier.
    if (hits <= 0) return 0;
    return (std::min)(10, 5 + hits);
  }
};

}  // namespace prism::pwd
