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
#include <chrono>  // Probe() reports real elapsed time
#include <condition_variable>  // the probe's batch pipeline stages
#include <cstdint>
#include <map>
#include <mutex>  // Probe() reports which worker accepted the hit
#include <set>
#include <thread>  // AttemptPlan.concurrency is a real pool, not a hint
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

// ---------------------------------------------------------------------------
// B3: user-facing probe diagnostics (PrismCore decision, executor-agnostic).
//
// SECURITY CONTRACT (CONTRACTS red line): the string goes to logs and UI, so
// it must never contain candidate plaintext -- not even a >=4-char fragment.
// The formatter only embeds counts and fixed labels, and the WHOLE string is
// still passed through RedactSecrets last: if a future edit ever embeds a
// candidate-derived fragment, the filter is already in the path (the Windows
// test asserts content-level absence, CONTRACTS D2).
// ---------------------------------------------------------------------------
struct ProbeReport {
  std::string diagnostics;
};

// Fixed UI labels; display order == Source ordinal, so the breakdown is
// byte-stable across builds.
inline const char* SourceLabel(Source s) {
  switch (s) {
    case Source::FilenameHint: return "文件名嗅探";
    case Source::ArchiveComment: return "压缩包注释";
    case Source::SidecarText: return "旁车文本";
    case Source::VaultHistory: return "密码库";
    case Source::UserInput: return "用户输入";
  }
  return "未知来源";
}

// Pure formatter for one probe outcome. Deterministic byte-for-byte: the tests
// pin the full string, so format drift turns a test red instead of turning a
// user report vague. The per-source breakdown counts the FIRST
// min(attemptsMade, size) entries of the plan -- for a sequential executor
// that is exactly the set of tried candidates; with concurrency > 1 the
// executor only knows the completion COUNT, and the prefix is the planned
// attempt order (documented approximation, kept deterministic on purpose).
inline ProbeReport BuildDiagnostics(const PasswordRing::AttemptPlan& plan,
                                    int attemptsMade, bool found,
                                    int64_t elapsedMs) {
  std::vector<std::string> secrets;
  secrets.reserve(plan.ordered.size());
  for (const Candidate& c : plan.ordered) secrets.push_back(c.text);

  size_t tried = attemptsMade < 0 ? 0 : static_cast<size_t>(attemptsMade);
  if (tried > plan.ordered.size()) tried = plan.ordered.size();

  std::string text;
  if (plan.ordered.empty()) {
    text = "无可用候选密码，未执行探测（用时 " + std::to_string(elapsedMs) + " ms）";
  } else {
    // Source enum is a dense 0..4 ordinal; count via array, never via map
    // (iteration order would leak into the message otherwise).
    int perSource[5] = {0, 0, 0, 0, 0};
    for (size_t i = 0; i < tried; ++i) {
      const int s = static_cast<int>(plan.ordered[i].source);
      if (s >= 0 && s < 5) ++perSource[s];
    }
    text = "已尝试 " + std::to_string(tried) + "/" +
           std::to_string(plan.ordered.size()) + " 个候选（来源：";
    bool first = true;
    for (int s = 0; s < 5; ++s) {
      if (perSource[s] == 0) continue;
      if (!first) text += "、";
      text += std::string(SourceLabel(static_cast<Source>(s))) + " " +
              std::to_string(perSource[s]);
      first = false;
    }
    text += found ? "），命中" : "），未命中";
    text += "（用时 " + std::to_string(elapsedMs) + " ms）";
  }

  ProbeReport report;
  // Redaction runs LAST and over the WHOLE string -- defense in depth, see
  // the security contract above.
  report.diagnostics = PasswordRing::RedactSecrets(text, secrets);
  return report;
}

}  // namespace prism::pwd

// --- Executor (B3): run a plan against an injected open-callable ---------
// Relocated from PrismWin/PasswordProbe.hpp: the executor is COM-free policy
// (stop on hit, monotonic progress, redacted diagnostics) and PrismCore tests
// drive it -- a PrismWin include there is impossible (they only link prism_core).

namespace prism::pwd {

// Result of one probe run. `password` is the only field that ever carries
// plaintext, and only when found==true (the caller asked for it back);
// `diagnostics` is safe for logs/UI by construction.
struct ProbeOutcome {
  bool found = false;
  std::string password;
  std::string diagnostics;
  int attemptsMade = 0;
  int64_t elapsedMs = 0;
  // Set when the run stopped because the caller asked it to (not because it
  // ran out of candidates). Distinguishing the two is what lets the open flow
  // say "cancelled" instead of "all 12 passwords were wrong" -- a cancelled
  // run has NOT tried everything, so calling it exhausted would be a lie.
  // `found` still wins: a hit that landed in the same instant as the cancel
  // gives the user the archive rather than a shrug.
  bool cancelled = false;
};

// Flattened candidate texts, in plan order -- the secret list for redaction.
inline std::vector<std::string> CandidateTexts(
    const prism::pwd::PasswordRing::AttemptPlan& plan) {
  std::vector<std::string> texts;
  texts.reserve(plan.ordered.size());
  for (const prism::pwd::Candidate& c : plan.ordered) texts.push_back(c.text);
  return texts;
}

// Runs plan.ordered against tryOpen(password)->bool.
//
// Semantics (the tests pin each of these):
//   a. candidates are tried in plan.ordered order, up to plan.concurrency
//      workers at a time (concurrency <= 1 runs on the calling thread's
//      single pool worker, which makes every count deterministic);
//   b. first hit stops the run: no NEW candidate is started after a hit
//      (already-running workers are allowed to finish -- abandoning a
//      half-done 7z Open would leak a stream), found=true, password filled;
//   c. progress(done, total) fires once per completed attempt, under the
//      bookkeeping lock, so `done` can never be observed going backwards;
//      exhausted without a hit => found=false, attemptsMade==ordered.size();
//   d. diagnostics never contain candidate plaintext (RedactSecrets);
//   e. elapsedMs is a real steady-clock measurement.
//   f. shouldCancel() is polled BEFORE claiming each candidate: once it turns
//      true no NEW attempt starts, already-running ones finish (abandoning a
//      half-done 7z Open would leak a stream), and `cancelled` is set. A hit
//      that already landed keeps `found` true.
template <typename OpenFn>
ProbeOutcome Probe(const prism::pwd::PasswordRing::AttemptPlan& plan, OpenFn tryOpen,
                   std::function<void(int done, int total)> progress,
                   std::function<bool()> shouldCancel = {}) {
  const auto started = std::chrono::steady_clock::now();
  ProbeOutcome outcome;
  const int total = static_cast<int>(plan.ordered.size());

  // AES key derivation saturates the SHA pipeline long before many workers
  // help (see PasswordRing::PlanAttempts), so the pool is capped by the plan;
  // an empty plan must not spawn a thread at all.
  int workers = plan.concurrency <= 0 ? 1 : (std::min)(plan.concurrency, total);

  std::mutex gate;                // orders progress + attempt bookkeeping
  std::atomic<int> next{0};       // next candidate index to claim
  std::atomic<bool> stop{false};  // set on hit: no NEW attempt may start
  std::atomic<bool> userStop{false};  // set on cancel: no NEW attempt may start
  std::string failureNotes;       // untrusted exception text, redacted later

  auto worker = [&]() {
    for (;;) {
      if (stop.load(std::memory_order_relaxed)) return;
      // Cancel is checked before the claim, so a cancelled run cannot start a
      // candidate it never reports (the count stays honest). Checked under the
      // same predicate that the caller's UI thread flips, so the last observed
      // state is always the one the user asked for.
      if (shouldCancel && shouldCancel()) {
        userStop.store(true, std::memory_order_relaxed);
        return;
      }
      const int idx = next.fetch_add(1, std::memory_order_relaxed);
      if (idx >= total) return;

      bool hit = false;
      std::string note;
      try {
        hit = tryOpen(plan.ordered[idx].text);
      } catch (const std::exception& e) {
        // Never rethrow with e.what() baked into a public message: the
        // exception text may embed the candidate (that is exactly what the
        // redaction test simulates). Park it; redaction happens below.
        note = e.what();
      } catch (...) {
        note = "unknown error";
      }

      std::lock_guard<std::mutex> lock(gate);
      ++outcome.attemptsMade;
      if (!note.empty()) {
        if (!failureNotes.empty()) failureNotes += "；";
        failureNotes += "候选 " + std::to_string(idx + 1) + " 探测异常: " + note;
      }
      if (hit && !outcome.found) {
        outcome.found = true;
        outcome.password = plan.ordered[idx].text;
        stop.store(true, std::memory_order_relaxed);
      }
      if (progress) progress(outcome.attemptsMade, total);
      // progress() is invoked under the lock on purpose: releasing first
      // would let a second completion overtake this one and a `done` value
      // could be observed going backwards.
    }
  };

  if (total > 0) {
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) pool.emplace_back(worker);
    for (std::thread& t : pool) t.join();
  }

  const auto ended = std::chrono::steady_clock::now();
  const int64_t elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                ended - started).count();

  outcome.diagnostics =
      prism::pwd::BuildDiagnostics(plan, outcome.attemptsMade, outcome.found,
                                   elapsedMs)
          .diagnostics;
  // Nothing is left to try only if every candidate was actually attempted; a
  // cancelled run stopped early, so it is NOT exhausted (see ProbeOutcome).
  outcome.cancelled =
      !outcome.found && (userStop.load() || (shouldCancel && shouldCancel())) &&
      outcome.attemptsMade < total;
  if (!failureNotes.empty()) {
    // The redaction here is load-bearing, not decorative: failureNotes is
    // built from foreign exception text. Mutating this line away must turn
    // the redaction tests red (CONTRACTS D3 -- and they do).
    outcome.diagnostics +=
        "；异常: " + prism::pwd::PasswordRing::RedactSecrets(
                         failureNotes, CandidateTexts(plan));
  }

  outcome.elapsedMs = elapsedMs;
  return outcome;
}

}  // namespace prism::pwd
