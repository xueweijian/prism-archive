// PrismArchive -- PrismCore
// OpenFlow: the state machine that turns "what the engine just said" into
// "what the user sees and what we try next" for password-protected archives.
//
// The blueprint gap this closes (docs/PROGRESS.md, batch B item 3): PasswordRing
// answers "which candidate first, how many at once, what does the miss report
// look like"; nothing answered "do I prompt, do I probe, what do I tell the
// user, and did they cancel". That is what lives here.
//
// Three pure pieces and one driver, all COM-free so Linux tests can pin every
// transition:
//
//   1. ClassifyOpen   -- the ENGINE TRUTH TABLE as a pure function
//                        (OpenAttempt -> OpenVerdict).
//   2. ClassifyOpResult -- where a wrong password actually surfaces, which is
//                        format-dependent and (measured, see below) NOT
//                        uniform: a body-encrypted 7z reports kDataError while
//                        an AES ZIP reports kWrongPassword.
//   3. OpenFlow       -- Idle/Opening/NeedPassword/Probing/Opened/Exhausted/
//                        Cancelled/Failed, emitting a FlowView for the UI.
//
// Ranking, concurrency and redaction are NOT reimplemented here: OpenFlow
// builds a PasswordRing::AttemptPlan and hands it to pwd::Probe.
//
// ---------------------------------------------------------------------------
// MEASURED GROUND TRUTH (docs/OPENFLOW.md §2). Every constant in
// ClassifyOpen below is an observed HRESULT / kpidErrorFlags pair from a real
// engine, not a reading of its source:
//
//   archive kind              password      IInArchive::Open  errorFlags
//   ---------------------------------------------------------------------
//   plain 7z                  any           S_OK              0
//   plain 7z (empty stream)   any           S_OK              0
//   -mhe=on 7z                NOT offered   S_FALSE           0
//   -mhe=on 7z                our E_ABORT   E_ABORT           undefined
//   -mhe=on 7z                WRONG         S_FALSE           0      <-- !!
//   -mhe=on 7z                RIGHT         S_OK              0
//   body-encrypted 7z         any           S_OK              0      <-- !!
//   corrupt next-header 7z    any           S_FALSE           0x02
//   truncated 7z              any           S_FALSE           0x20
//   .7z handed to ZIP handler any           S_FALSE           0x01
//
// Two cells drive the whole design:
//   * a WRONG password on a header-encrypted archive looks EXACTLY like no
//     password at all (S_FALSE, flags 0). No engine signal separates them, so
//     the decision must come from what WE tried, not from the return code.
//     Hence OpenAttempt::passwordSupplied.
//   * a BODY-encrypted archive opens fine anonymously (S_OK, 3 items) and only
//     fails when bytes are pulled. An "is this encrypted?" decision taken at
//     Open time would silently skip probing such archives.
//
// CONTRACTS D16 (capability-discovery sets must never gate a read) applies
// directly: kpidEncrypted and the handler's feature flags are diagnostics for
// the message text only. Nothing in this file reads an "supports encryption?"
// table to decide whether a password is needed -- the engine's behaviour is.
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <vector>

#include "PasswordRing.hpp"
#include "PasswordVault.hpp"

namespace prism::openflow {

// ---------------------------------------------------------------------------
// 1. Engine verdicts
// ---------------------------------------------------------------------------

// Observed HRESULTs, spelled out so the truth table reads like a table and a
// test can name a row instead of a magic number. Values are the real Win32
// constants (S_OK / S_FALSE / E_ABORT / E_FAIL / E_OUTOFMEMORY).
constexpr int32_t kOpenHrOk = 0x00000000;
constexpr int32_t kOpenHrFalse = 0x00000001;
constexpr int32_t kOpenHrAbort = static_cast<int32_t>(0x80004004u);
constexpr int32_t kOpenHrFail = static_cast<int32_t>(0x80004005u);
constexpr int32_t kOpenHrOutOfMemory = static_cast<int32_t>(0x8007000Eu);

// kpv_ErrorFlags_* (7-Zip PropID.h:123-132), the bits we actually act on.
constexpr uint32_t kErrIsNotArc = 1u << 0;               // 0x01
constexpr uint32_t kErrHeadersError = 1u << 1;           // 0x02
constexpr uint32_t kErrEncryptedHeadersError = 1u << 2;   // 0x04
constexpr uint32_t kErrUnexpectedEnd = 1u << 5;           // 0x20
constexpr uint32_t kErrDataAfterEnd = 1u << 6;            // 0x40
constexpr uint32_t kErrUnsupportedFeature = 1u << 8;      // 0x100
// The handler did not answer kpidErrorFlags at all -- distinct from "0".
constexpr uint32_t kErrFlagsUndefined = 0xFFFFFFFFu;

// One Open attempt, flattened into the only facts the decision may use.
struct OpenAttempt {
  int32_t openHr = kOpenHrOk;
  uint32_t errorFlags = kErrFlagsUndefined;
  bool passwordSupplied = false;  // we handed it a non-empty password
  bool hostCancelled = false;     // OUR cancel flag, not the engine's
};

enum class OpenVerdict : uint8_t {
  Opened,        // catalog is readable
  NeedPassword,  // locked, and we have not tried anything yet
  WrongPassword, // locked, and the password we tried did not open it
  NotThisFormat, // this handler rejected the file (CONTRACTS A1: not an error)
  Corrupt,       // the container itself is damaged
  Cancelled,     // the user stopped us
  Failed         // engine fault (OOM, E_FAIL, ...)
};

inline const char* OpenVerdictName(OpenVerdict v) {
  switch (v) {
    case OpenVerdict::Opened: return "Opened";
    case OpenVerdict::NeedPassword: return "NeedPassword";
    case OpenVerdict::WrongPassword: return "WrongPassword";
    case OpenVerdict::NotThisFormat: return "NotThisFormat";
    case OpenVerdict::Corrupt: return "Corrupt";
    case OpenVerdict::Cancelled: return "Cancelled";
    case OpenVerdict::Failed: return "Failed";
  }
  return "?";
}
inline std::ostream& operator<<(std::ostream& os, OpenVerdict v) {
  return os << OpenVerdictName(v);
}

// The truth table. Precedence is deliberate and is asserted row by row in
// Tests/test_open_flow.cpp:
//   a. our own cancel outranks everything (a cancelled Open is not a verdict);
//   b. S_OK means opened -- SUCCEEDED(S_FALSE) is also TRUE, so the check is
//      == S_OK, never SUCCEEDED (CONTRACTS A1);
//   c. the encrypted-header error bit is a PASSWORD problem even though it
//      shares the "flags" channel with corruption;
//   d. IsNotArc means "wrong handler", not "damaged file";
//   e. other defined error bits mean damaged;
//   f. undefined flags never mean damaged (absence of evidence, CONTRACTS D12);
//   g. anything left is the password question, answered by what we tried.
inline OpenVerdict ClassifyOpen(const OpenAttempt& a) {
  if (a.hostCancelled) return OpenVerdict::Cancelled;
  if (a.openHr == kOpenHrOk) return OpenVerdict::Opened;
  if (a.openHr != kOpenHrFalse && a.openHr != kOpenHrAbort) return OpenVerdict::Failed;

  const bool flagsKnown = a.errorFlags != kErrFlagsUndefined;
  if (flagsKnown) {
    if (a.errorFlags & kErrEncryptedHeadersError) {
      return a.passwordSupplied ? OpenVerdict::WrongPassword : OpenVerdict::NeedPassword;
    }
    if (a.errorFlags & kErrIsNotArc) return OpenVerdict::NotThisFormat;
    const uint32_t damage = a.errorFlags & (kErrHeadersError | kErrUnexpectedEnd |
                                            kErrDataAfterEnd | kErrUnsupportedFeature);
    if (damage != 0) return OpenVerdict::Corrupt;
  }
  // E_ABORT with no password supplied is our own OpenCallback short-circuiting
  // an unknown password (PrismEngineBridge.hpp:404), i.e. "please ask the UI".
  return a.passwordSupplied ? OpenVerdict::WrongPassword : OpenVerdict::NeedPassword;
}

// ---------------------------------------------------------------------------
// 2. Entry-level verdicts (the second place a password is judged)
// ---------------------------------------------------------------------------
// Measured: pulling bytes is where a WRONG password on a body-encrypted
// archive is decided, and the code differs per format --
//   body-encrypted 7z : kDataError (2)     [7-AES leaves no password marker]
//   AES-256 ZIP       : kWrongPassword (9) [WzAes checks a 2-byte verifier]
//   ZipCrypto ZIP     : kWrongPassword (9) [1-byte check byte]
//   right password    : kOK (0)
// The SAME kDataError on an UNENCRYPTED entry is corruption. That is why
// `entryEncrypted` is a parameter rather than something we infer.
enum class EntryVerdict : uint8_t { Ok, WrongPassword, Corrupt, Other };

inline const char* EntryVerdictName(EntryVerdict v) {
  switch (v) {
    case EntryVerdict::Ok: return "Ok";
    case EntryVerdict::WrongPassword: return "WrongPassword";
    case EntryVerdict::Corrupt: return "Corrupt";
    case EntryVerdict::Other: return "Other";
  }
  return "?";
}
inline std::ostream& operator<<(std::ostream& os, EntryVerdict v) {
  return os << EntryVerdictName(v);
}

inline EntryVerdict ClassifyOpResult(int32_t opResult, bool entryEncrypted) {
  switch (opResult) {
    case 0:  // kOK
      return EntryVerdict::Ok;
    case 9:  // kWrongPassword -- only ZIP/RAR produce this
      return EntryVerdict::WrongPassword;
    case 2:  // kDataError      -- 7-AES wrong password lands here
    case 3:  // kCRCError
      return entryEncrypted ? EntryVerdict::WrongPassword : EntryVerdict::Corrupt;
    case 8:  // kHeadersError
      return EntryVerdict::Corrupt;
    default:
      return EntryVerdict::Other;
  }
}

// ---------------------------------------------------------------------------
// 3. The state machine
// ---------------------------------------------------------------------------
enum class State : uint8_t {
  Idle,
  Opening,
  NeedPassword,
  Probing,
  Opened,
  Exhausted,
  Cancelled,
  Failed
};

inline const char* StateName(State s) {
  switch (s) {
    case State::Idle: return "Idle";
    case State::Opening: return "Opening";
    case State::NeedPassword: return "NeedPassword";
    case State::Probing: return "Probing";
    case State::Opened: return "Opened";
    case State::Exhausted: return "Exhausted";
    case State::Cancelled: return "Cancelled";
    case State::Failed: return "Failed";
  }
  return "?";
}
inline std::ostream& operator<<(std::ostream& os, State s) { return os << StateName(s); }

enum class FailReason : uint8_t { None, Corrupt, NotThisFormat, Engine };

inline const char* FailReasonName(FailReason r) {
  switch (r) {
    case FailReason::None: return "None";
    case FailReason::Corrupt: return "Corrupt";
    case FailReason::NotThisFormat: return "NotThisFormat";
    case FailReason::Engine: return "Engine";
  }
  return "?";
}
inline std::ostream& operator<<(std::ostream& os, FailReason r) {
  return os << FailReasonName(r);
}

// Everything the UI needs in one value. The UI never reads OpenFlow internals,
// so a rendering change can never reach back into the decision layer.
struct FlowView {
  State state = State::Idle;
  bool showDialog = false;    // the password dialog belongs on screen
  bool showProgress = false;  // the "匹配中 3/12" pill belongs on screen
  int done = 0;
  int total = 0;
  std::string progressLabel;  // "匹配中 3/12"
  std::string message;        // user-facing, already redacted
  std::string password;       // ONLY non-empty in State::Opened
};

struct FlowConfig {
  std::vector<pwd::Candidate> candidates;
  PasswordKdfClass kdf = PasswordKdfClass::AesKeyDerivation;
  int threads = 1;
  std::string archiveName;  // for messages; never trusted as a secret source
};

class OpenFlow {
 public:
  explicit OpenFlow(FlowConfig cfg) : cfg_(std::move(cfg)) {
    cfg_.candidates = pwd::PasswordRing::Rank(std::move(cfg_.candidates));
  }

  State state() const { return state_; }
  FailReason failReason() const { return failReason_; }
  bool cancelled() const { return cancelled_; }
  bool headerEncrypted() const { return headerEncrypted_; }
  int attemptsMade() const { return attemptsMade_; }
  int64_t elapsedMs() const { return elapsedMs_; }
  const pwd::PasswordRing::AttemptPlan& plan() const { return plan_; }
  pwd::PasswordRing::ProbeStrategy strategy() const { return strategy_; }
  const FlowView& view() const { return view_; }

  // Body-encrypted archives verify through a single entry, so the plan for
  // them is BodyProbe whenever there is anything left to try.
  pwd::PasswordRing::ProbeStrategy strategyForBody() const {
    return pwd::PasswordRing::StrategyFor(/*headerEncrypted=*/false,
                                          cfg_.candidates.size());
  }

  // Built from the SAME ring that produced the plan, so a caller cannot
  // accidentally redact against a different candidate list.
  std::string PlanDiagnostics(int attemptsMade, bool found, int64_t elapsedMs) const {
    return pwd::BuildDiagnostics(plan_, attemptsMade, found, elapsedMs).diagnostics;
  }

  // Idle -> Opening
  FlowView Begin() {
    if (state_ == State::Idle) {
      state_ = State::Opening;
      Publish();
    }
    return view_;
  }

  // Opening -> Opened | NeedPassword | Probing | Failed | Cancelled
  FlowView OnOpenResult(const OpenAttempt& attempt) {
    OpenAttempt a = attempt;
    if (cancelled_) a.hostCancelled = true;
    const OpenVerdict verdict = ClassifyOpen(a);
    switch (verdict) {
      case OpenVerdict::Opened:
        state_ = State::Opened;
        break;
      case OpenVerdict::Cancelled:
        state_ = State::Cancelled;
        break;
      case OpenVerdict::NotThisFormat:
        state_ = State::Failed;
        failReason_ = FailReason::NotThisFormat;
        message_ = "这个文件不是本程序认识的压缩包格式（扩展名可能与内容不符）。";
        break;
      case OpenVerdict::Corrupt:
        state_ = State::Failed;
        failReason_ = FailReason::Corrupt;
        message_ = "压缩包已损坏，头部无法解析。它不是密码问题，试密码不会有帮助。";
        break;
      case OpenVerdict::Failed:
        state_ = State::Failed;
        failReason_ = FailReason::Engine;
        message_ = "压缩引擎报告内部错误，无法打开。";
        break;
      case OpenVerdict::NeedPassword:
      case OpenVerdict::WrongPassword:
        // Reaching here means the CATALOG is locked => header encryption. This
        // is a measured fact about this attempt, not a claim about the format.
        headerEncrypted_ = true;
        EnterPasswordQuestion(verdict == OpenVerdict::WrongPassword);
        break;
    }
    Publish();
    return view_;
  }

  // Probing progress. Monotone by construction: a smaller `done` than the
  // high-water mark is ignored (a stale callback must never make the pill jump
  // backwards).
  FlowView OnProgress(int done, int total) {
    if (state_ != State::Probing) return view_;
    if (done > done_) done_ = done;
    if (total > 0) total_ = total;
    Publish();
    return view_;
  }

  // Probing -> Opened | Exhausted | NeedPassword | Cancelled
  FlowView OnProbeResult(const pwd::ProbeOutcome& out) {
    attemptsMade_ = out.attemptsMade;
    elapsedMs_ = out.elapsedMs;
    lastDiagnostics_ = out.diagnostics;

    if (out.found) {  // a hit outranks a simultaneous cancel
      state_ = State::Opened;
      password_ = out.password;
      message_ = DescribeSuccess();
      Publish();
      return view_;
    }
    if (cancelled_ || out.cancelled) {
      cancelled_ = true;
      state_ = State::Cancelled;
      message_ = "已取消：停止尝试剩余候选密码。";
      Publish();
      return view_;
    }
    if (manual_) {
      // A single hand-typed password that missed: the dialog comes back with
      // the reason, not with the text.
      state_ = State::NeedPassword;
      message_ = "密码错误（已尝试 1 次）。";
      Publish();
      return view_;
    }
    state_ = State::Exhausted;
    message_ = DescribeExhaustion();
    Publish();
    return view_;
  }

  // NeedPassword -> Probing (one UserInput candidate; the ring is replaced, not
  // grown, so repeated typing can never build an unbounded retry list).
  FlowView SubmitPassword(std::string typed) {
    pwd::Candidate c;
    c.text = std::move(typed);
    c.source = pwd::Source::UserInput;
    std::vector<pwd::Candidate> one;
    one.push_back(std::move(c));
    manual_ = true;
    plan_ = pwd::PasswordRing::PlanAttempts(
        pwd::PasswordRing::Rank(std::move(one)), cfg_.kdf, cfg_.threads);
    strategy_ = pwd::PasswordRing::StrategyFor(headerEncrypted_, plan_.ordered.size());
    done_ = 0;
    total_ = static_cast<int>(plan_.ordered.size());
    state_ = State::Probing;
    message_.clear();
    Publish();
    return view_;
  }

  // Any live state -> Cancelled. Idempotent: cancelling twice is not an error.
  FlowView Cancel() {
    if (state_ == State::Cancelled) return view_;
    cancelled_ = true;
    state_ = State::Cancelled;
    message_ = "已取消。";
    Publish();
    return view_;
  }

 private:
  // Builds the probe plan and decides whether the dialog is the right next
  // step. The dialog appears ONLY when there is nothing left to try silently --
  // an empty ring, which is exactly where a silent "skip" would strand the
  // user in front of nothing.
  void EnterPasswordQuestion(bool alreadyFailed) {
    manual_ = false;
    plan_ = pwd::PasswordRing::PlanAttempts(
        cfg_.candidates, cfg_.kdf, cfg_.threads);
    strategy_ = pwd::PasswordRing::StrategyFor(headerEncrypted_, plan_.ordered.size());
    done_ = 0;
    total_ = static_cast<int>(plan_.ordered.size());
    password_.clear();

    if (plan_.ordered.empty()) {
      state_ = State::NeedPassword;
      message_ = alreadyFailed
                     ? "密码错误，且没有更多可用候选密码。请手动输入。"
                     : "这个压缩包的内容被密码保护。没有可用候选密码，请手动输入。";
      return;
    }
    state_ = State::Probing;
    message_ = alreadyFailed ? "上一个密码不对，继续尝试候选密码。" : "";
  }

  std::string DescribeSuccess() const {
    std::string m = "密码已匹配（第 " + std::to_string(attemptsMade_) + " 个候选，" +
                    std::to_string(elapsedMs_) + " ms）";
    // Name the SOURCE tier the winner came from -- never the text.
    if (attemptsMade_ > 0 && static_cast<size_t>(attemptsMade_) <= plan_.ordered.size()) {
      m += "，来源：";
      m += pwd::SourceLabel(plan_.ordered[attemptsMade_ - 1].source);
    }
    m += "。";
    return m;
  }

  std::string DescribeExhaustion() const {
    std::string m = lastDiagnostics_.empty() ? PlanDiagnostics(attemptsMade_, false, elapsedMs_)
                                             : lastDiagnostics_;
    if (m.empty()) m = "已尝试全部候选密码，均未命中。";
    m += " 可以手动输入密码再试。";
    return m;
  }

  // The ONE redaction site for everything that can reach the user.
  //
  // Deliberately NOT also applied inside Describe*(): two call sites enforcing
  // one invariant means a regression at either is invisible, and the mutation
  // harness proved exactly that -- dropping the redaction here stayed green
  // because DescribeExhaustion() had already scrubbed the same string. All
  // message_ producers are now plain concatenation and this is the only gate,
  // so "did the scrub happen?" is a single, testable question.
  // Redact against the plan's own candidates every time (CONTRACTS: secrets
  // never reach logs or UI).
  std::string Redact(const std::string& text) const {
    return pwd::PasswordRing::RedactSecrets(text, pwd::CandidateTexts(plan_));
  }

  void Publish() {
    view_.state = state_;
    view_.showDialog = state_ == State::NeedPassword || state_ == State::Exhausted;
    view_.showProgress = state_ == State::Probing;
    view_.done = done_;
    view_.total = total_;
    view_.progressLabel = view_.showProgress
                              ? "匹配中 " + pwd::PasswordRing::ProgressLabel(
                                                static_cast<size_t>(done_),
                                                static_cast<size_t>(total_))
                              : std::string();
    view_.password = state_ == State::Opened ? password_ : std::string();
    // Anything that could carry foreign text goes through Redact exactly once,
    // here, so no state can publish an unscrubbed message.
    view_.message = Redact(message_);
  }

  FlowConfig cfg_;
  pwd::PasswordRing::AttemptPlan plan_;
  pwd::PasswordRing::ProbeStrategy strategy_ = pwd::PasswordRing::ProbeStrategy::Decline;
  State state_ = State::Idle;
  FailReason failReason_ = FailReason::None;
  FlowView view_;
  std::string message_;
  std::string password_;
  std::string lastDiagnostics_;
  bool cancelled_ = false;
  bool headerEncrypted_ = false;
  bool manual_ = false;
  int done_ = 0;
  int total_ = 0;
  int attemptsMade_ = 0;
  int64_t elapsedMs_ = 0;
};

}  // namespace prism::openflow
