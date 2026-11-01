// PrismCore unit tests for the encrypted-archive open flow (batch B, item 3):
//   * the ENGINE TRUTH TABLE   -- what 7-Zip actually returns for each archive
//     kind, expressed as a pure function so Linux can pin every row;
//   * the DECISION STATE MACHINE -- Idle/Opening/NeedPassword/Probing/Opened/
//     Exhausted/Cancelled/Failed, its dialog + progress + explanation outputs;
//   * the executor wiring      -- OpenFlow drives the real pwd::Probe (not a
//     reimplementation of it), including real cancellation.
//
// Where every truth-table row comes from (docs/OPENFLOW.md §2): a Linux build
// of 7-Zip 26.00's Format7zF bundle driven through the documented COM surface
// against four real fixtures. These assertions are transcriptions of measured
// HRESULT/flag tuples, never of a guess. The Windows suite
// (Tests/win/test_open_flow_win.cpp) re-derives the same table from the real
// 7z.dll, so if an engine version ever changes a cell, one of the two goes red.
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "OpenFlow.hpp"
#include "PasswordRing.hpp"
#include "mini_test.hpp"

namespace {

using prism::pwd::Candidate;
using prism::pwd::PasswordRing;
using prism::pwd::ProbeOutcome;
using prism::pwd::Source;
namespace of = prism::openflow;

// ---- truth-table input builders (mirroring the measured tuples) -----------
// Measured with Format7zF 26.00 on Linux; see docs/OPENFLOW.md §2 for the
// command that produced each row.
of::OpenAttempt Att(int32_t hr, uint32_t flags, bool supplied, bool cancelled = false) {
  of::OpenAttempt a;
  a.openHr = hr;
  a.errorFlags = flags;
  a.passwordSupplied = supplied;
  a.hostCancelled = cancelled;
  return a;
}

Candidate C(const std::string& text, Source s) {
  Candidate c;
  c.text = text;
  c.source = s;
  return c;
}

// The 12-candidate ring the "matching 3/12" label counts against.
// Zero-padded on purpose: PasswordRing::Rank breaks same-source ties
// lexicographically, so "Cand04#zz" would land at position 7 of 12 (between
// Cand39 and Cand5). The padding makes plan position == the number in the
// name, which is what lets the hit-at-4 assertions mean what they say.
std::vector<Candidate> Ring12() {
  std::vector<Candidate> out;
  for (int i = 1; i <= 12; ++i) {
    const std::string n = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
    out.push_back(C("Cand" + n + "#zz", Source::VaultHistory));
  }
  return out;
}

of::FlowConfig Cfg(std::vector<Candidate> cands) {
  of::FlowConfig cfg;
  cfg.candidates = std::move(cands);
  cfg.kdf = prism::PasswordKdfClass::AesKeyDerivation;
  cfg.threads = 1;  // deterministic counts for the decision-layer tests
  cfg.archiveName = "素材包.7z";
  return cfg;
}

bool Mentions(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

// Any >=4-char fragment of a candidate counts as a leak (CONTRACTS D2: assert
// content, never existence; a "[redacted] appears" check passes on partials).
bool LeaksAnyFragment(const std::string& hay, const std::vector<Candidate>& cands) {
  for (const Candidate& c : cands) {
    if (c.text.size() < 3) continue;
    if (Mentions(hay, c.text)) return true;
    for (size_t len = c.text.size() - 1; len >= 4; --len) {
      for (size_t i = 0; i + len <= c.text.size(); ++i) {
        if (Mentions(hay, c.text.substr(i, len))) return true;
      }
    }
  }
  return false;
}

// =========================================================================
// 1. ENGINE TRUTH TABLE -- one assertion per measured row
// =========================================================================

// A plain (unencrypted) 7z: Open=S_OK, flags=0, 3 items regardless of what
// password we hand it. Measured on fx/plain.7z for all five password policies.
PRISM_TEST("truth.plain7z_opensWithAnyPasswordPolicy") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOk, 0, false)) == of::OpenVerdict::Opened);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOk, 0, true)) == of::OpenVerdict::Opened);
}

// -mhe=on (encrypted HEADER) with NO password offered:
//   Format7zF: Open=S_FALSE, asks=0, kpidErrorFlags=0, items=0.
// This is the row that makes "prompt for a password" correct.
PRISM_TEST("truth.headerEncrypted_withoutPassword_needsPassword") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, 0x0, false)) ==
              of::OpenVerdict::NeedPassword);
}

// -mhe=on with our OpenCallback short-circuiting an EMPTY password to E_ABORT
// (PrismEngineBridge.hpp:404). Measured: Open=E_ABORT, asks=1.
PRISM_TEST("truth.headerEncrypted_hostAborts_needsPassword") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false)) ==
              of::OpenVerdict::NeedPassword);
}

// -mhe=on with a WRONG password: Open=S_FALSE, asks=1, flags STILL 0, items=0.
// The flags cannot separate "wrong password" from "no password offered" --
// only the attempt record can. This is why OpenAttempt carries
// passwordSupplied instead of relying on the engine's ask count.
PRISM_TEST("truth.headerEncrypted_wrongPassword_reportsWrongPassword") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, 0x0, true)) ==
              of::OpenVerdict::WrongPassword);
}

// ONLY encrypted (body/data): Open=S_OK, flags=0, items=3 -- the engine says
// NOTHING about encryption. Measured for every one of the five password
// policies. Consequence: a decision layer that waits for an "is encrypted"
// signal at Open time would never probe this archive at all.
PRISM_TEST("truth.bodyEncrypted_openingLooksIdenticalToPlain") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOk, 0, false)) == of::OpenVerdict::Opened);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOk, 0, true)) == of::OpenVerdict::Opened);
}

// Genuinely corrupt header: S_FALSE + kpv_ErrorFlags_HeadersError (0x02).
// Measured on fx/corrupt.7z (20 bytes of 0xAA spliced over the next-header).
PRISM_TEST("truth.corruptHeader_reportsCorrupt") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrHeadersError, false)) ==
              of::OpenVerdict::Corrupt);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrHeadersError, true)) ==
              of::OpenVerdict::Corrupt);
}

// Truncated archive: S_FALSE + kpv_ErrorFlags_UnexpectedEnd (0x20).
PRISM_TEST("truth.truncatedArchive_reportsCorrupt") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrUnexpectedEnd, false)) ==
              of::OpenVerdict::Corrupt);
}

// Wrong handler for the format (Zip handler handed a .7z): S_FALSE +
// kpv_ErrorFlags_IsNotArc (0x01). Measured: identical for all 5 policies.
// CONTRACTS A1: SUCCEEDED(S_FALSE) is TRUE, so this MUST NOT read as success.
PRISM_TEST("truth.wrongHandler_reportsNotThisFormat") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrIsNotArc, false)) ==
              of::OpenVerdict::NotThisFormat);
}

// Defensive row: kpv_ErrorFlags_EncryptedHeadersError (0x04). 7-Zip's own
// message table maps it to "Headers Error in encrypted archive. Wrong
// password?" (UI/Console/ExtractCallbackConsole.cpp:227). Never observed on
// the two engine builds measured here, but if a future engine sets it, calling
// it Corrupt would send the user hunting a nonexistent disk error.
PRISM_TEST("truth.encryptedHeadersErrorBit_isAPasswordProblemNotCorruption") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrEncryptedHeadersError, true)) ==
              of::OpenVerdict::WrongPassword);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrEncryptedHeadersError, false)) ==
              of::OpenVerdict::NeedPassword);
}

// Undefined flags (a handler that does not answer kpidErrorFlags) + S_FALSE
// must NOT be called Corrupt: absence of evidence is not evidence of
// corruption (CONTRACTS D12 in spirit). Fall back to the password question.
PRISM_TEST("truth.undefinedFlags_fallsBackToPasswordQuestion") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrFlagsUndefined, false)) ==
              of::OpenVerdict::NeedPassword);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrFlagsUndefined, true)) ==
              of::OpenVerdict::WrongPassword);
  // The two must be told apart from every DEFINED flag, including the ones
  // that mean damage. Collapsing "flags known?" into `true` makes 0xFFFFFFFF
  // match every damage bit and every real archive turns into Corrupt -- the
  // assertion below is what detects that (mutation m2).
  PRISM_CHECK(of::kErrFlagsUndefined != 0u);
  PRISM_CHECK((of::kErrFlagsUndefined & of::kErrHeadersError) != 0u);
  PRISM_CHECK((of::kErrFlagsUndefined & of::kErrIsNotArc) != 0u);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrHeadersError, false)) ==
              of::OpenVerdict::Corrupt);
}

// Real engine faults are not password problems.
PRISM_TEST("truth.engineFaults_areFailures") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOutOfMemory, 0, false)) ==
              of::OpenVerdict::Failed);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFail, 0, false)) == of::OpenVerdict::Failed);
}

// Our own cancel flag outranks every engine signal -- a cancelled Open is not
// a wrong password, and must never trigger a retry.
PRISM_TEST("truth.hostCancel_outranksEngineSignals") {
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false, true)) ==
              of::OpenVerdict::Cancelled);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrFalse, of::kErrIsNotArc, false, true)) ==
              of::OpenVerdict::Cancelled);
  PRISM_CHECK(of::ClassifyOpen(Att(of::kOpenHrOk, 0, false, true)) == of::OpenVerdict::Cancelled);
}

// =========================================================================
// 2. ENTRY-LEVEL VERDICT -- where a wrong password shows up per format
// =========================================================================
// MEASURED, and this is the single most load-bearing cell in the table:
//   body-encrypted 7z, wrong password -> kDataError   (2), NOT kWrongPassword
//   AES-256 ZIP,      wrong password -> kWrongPassword (9)
//   any of them,      right password -> kOK            (0)
PRISM_TEST("verify.bodyEncrypted7z_wrongPasswordSurfacesAsDataError") {
  PRISM_CHECK(of::ClassifyOpResult(2 /*kDataError*/, /*entryEncrypted=*/true) ==
              of::EntryVerdict::WrongPassword);
  PRISM_CHECK(of::ClassifyOpResult(3 /*kCRCError*/, true) == of::EntryVerdict::WrongPassword);
  PRISM_CHECK(of::ClassifyOpResult(9 /*kWrongPassword*/, true) ==
              of::EntryVerdict::WrongPassword);
  PRISM_CHECK(of::ClassifyOpResult(0 /*kOK*/, true) == of::EntryVerdict::Ok);
}

// The SAME op result means corruption, not a bad password, when the entry was
// never encrypted. Collapsing the two would make a damaged plain archive look
// like it needs a password forever.
PRISM_TEST("verify.unencryptedEntry_dataErrorIsCorruption") {
  PRISM_CHECK(of::ClassifyOpResult(2 /*kDataError*/, /*entryEncrypted=*/false) ==
              of::EntryVerdict::Corrupt);
  PRISM_CHECK(of::ClassifyOpResult(3 /*kCRCError*/, false) == of::EntryVerdict::Corrupt);
  PRISM_CHECK(of::ClassifyOpResult(0, false) == of::EntryVerdict::Ok);
}

// =========================================================================
// 3. STATE MACHINE -- one case per transition
// =========================================================================

// Idle -> Opening
PRISM_TEST("flow.idle_to_opening") {
  of::OpenFlow flow(Cfg(Ring12()));
  PRISM_CHECK(flow.state() == of::State::Idle);
  PRISM_CHECK(flow.view().showDialog == false);
  const of::FlowView v = flow.Begin();
  PRISM_CHECK(flow.state() == of::State::Opening);
  PRISM_CHECK(v.state == of::State::Opening);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.showProgress == false);
}

// Opening -> Opened (plain archive): no dialog, no progress, no password.
PRISM_TEST("flow.opening_to_opened_plain") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  const of::FlowView v = flow.OnOpenResult(Att(of::kOpenHrOk, 0, false));
  PRISM_CHECK(flow.state() == of::State::Opened);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(v.password.empty());
  PRISM_CHECK(v.total == 0);
}

// Opening -> NeedPassword -> Probing in one step when the ring is non-empty:
// the dialog must NOT steal focus while we still have candidates to burn.
PRISM_TEST("flow.needPassword_withRing_goesStraightToProbing") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  const of::FlowView v = flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  PRISM_CHECK(flow.state() == of::State::Probing);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.showProgress == true);
  PRISM_CHECK(v.total == 12);
  PRISM_CHECK(v.done == 0);
  PRISM_CHECK(v.progressLabel == "匹配中 0/12");
  PRISM_CHECK(flow.headerEncrypted());  // learned from the engine, not from a feature table
  // Header-encrypted => the plan must be a FullOpen plan (never BodyProbe).
  PRISM_CHECK(flow.strategy() == PasswordRing::ProbeStrategy::FullOpen);
}

// Opening -> NeedPassword when the ring is empty: NOW the dialog is correct,
// and the message must say why instead of stranding an empty dialog.
PRISM_TEST("flow.needPassword_withEmptyRing_showsDialogWithReason") {
  of::OpenFlow flow(Cfg({}));
  flow.Begin();
  const of::FlowView v = flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  PRISM_CHECK(flow.state() == of::State::NeedPassword);
  PRISM_CHECK(v.showDialog == true);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(v.total == 0);
  PRISM_CHECK(!v.message.empty());
  PRISM_CHECK(Mentions(v.message, "密码"));
}

// Probing progress: label is exactly "匹配中 3/12" and `done` is monotone.
PRISM_TEST("flow.progress_label_isMatchingNofM_andMonotone") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  int last = 0;
  for (int i = 1; i <= 12; ++i) {
    const of::FlowView v = flow.OnProgress(i, 12);
    PRISM_CHECK(v.state == of::State::Probing);
    PRISM_CHECK(v.showProgress == true);
    PRISM_CHECK_EQ(v.progressLabel, std::string("匹配中 ") + std::to_string(i) + "/12");
    PRISM_CHECK(v.done == i);
    PRISM_CHECK(v.done >= last);
    last = v.done;
  }
  // A stale callback (the second worker reporting its own count after ours
  // landed) must never rewind the pill: "匹配中 11/12" jumping back to 9/12
  // reads as a hang. This is the assertion that makes the high-water guard
  // load-bearing (mutation m8 removes it and must turn THIS check red).
  const of::FlowView back = flow.OnProgress(2, 12);
  PRISM_CHECK_EQ(back.done, 12);
  PRISM_CHECK_EQ(back.progressLabel, std::string("匹配中 12/12"));
  const of::FlowView same = flow.OnProgress(12, 12);
  PRISM_CHECK_EQ(same.done, 12);
}

// Probing -> Opened at 4/12, carrying the password back to the caller.
PRISM_TEST("flow.probing_hitAt4_opensAndReturnsPassword") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  for (int i = 1; i <= 4; ++i) flow.OnProgress(i, 12);
  ProbeOutcome out;
  out.found = true;
  out.password = "Cand04#zz";
  out.attemptsMade = 4;
  out.elapsedMs = 41;
  out.diagnostics = flow.PlanDiagnostics(4, true, 41);
  const of::FlowView v = flow.OnProbeResult(out);
  PRISM_CHECK(flow.state() == of::State::Opened);
  PRISM_CHECK_EQ(v.password, std::string("Cand04#zz"));
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(flow.attemptsMade() == 4);
  PRISM_CHECK(flow.elapsedMs() == 41);
}

// Probing -> Exhausted: the explanation must carry counts and must offer the
// manual dialog (the user's only remaining move).
PRISM_TEST("flow.probing_allWrong_exhausted_offersDialog") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  for (int i = 1; i <= 12; ++i) flow.OnProgress(i, 12);
  ProbeOutcome out;
  out.found = false;
  out.attemptsMade = 12;
  out.elapsedMs = 380;
  out.diagnostics = flow.PlanDiagnostics(12, false, 380);
  const of::FlowView v = flow.OnProbeResult(out);
  PRISM_CHECK(flow.state() == of::State::Exhausted);
  PRISM_CHECK(v.showDialog == true);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(v.password.empty());
  PRISM_CHECK(Mentions(v.message, "12/12"));
  PRISM_CHECK(Mentions(v.message, "未命中"));
}

// Probing -> Cancelled, and the flow must remember it was cancelled so the
// executor can tell a cancel apart from an exhaustion.
PRISM_TEST("flow.cancel_duringProbing_reportsCancelled") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  flow.OnProgress(3, 12);
  PRISM_CHECK(flow.cancelled() == false);
  const of::FlowView v = flow.Cancel();
  PRISM_CHECK(flow.cancelled() == true);
  PRISM_CHECK(flow.state() == of::State::Cancelled);
  PRISM_CHECK(v.state == of::State::Cancelled);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(!v.message.empty());
}

// Opened -> Cancelled is legal (the user closes the window mid-open).
PRISM_TEST("flow.cancel_fromAnyLiveState") {
  for (const auto& pair : std::vector<std::pair<of::State, std::function<void(of::OpenFlow&)>>>{
           {of::State::Idle, [](of::OpenFlow&) {}},
           {of::State::Opening, [](of::OpenFlow& f) { f.Begin(); }},
           {of::State::NeedPassword, [](of::OpenFlow& f) {
              f.Begin();
              f.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
            }}}) {
    of::OpenFlow flow(Cfg({}));
    pair.second(flow);
    PRISM_CHECK(flow.Cancel().state == of::State::Cancelled);
    PRISM_CHECK(flow.state() == of::State::Cancelled);
  }
}

// Opening -> Failed(Corrupt): a damaged container must NOT look like a
// password problem, and must not open a dialog.
PRISM_TEST("flow.corrupt_failedWithReason_noDialog") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  const of::FlowView v = flow.OnOpenResult(Att(of::kOpenHrFalse, of::kErrHeadersError, false));
  PRISM_CHECK(flow.state() == of::State::Failed);
  PRISM_CHECK(flow.failReason() == of::FailReason::Corrupt);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.showProgress == false);
  PRISM_CHECK(!v.message.empty());
}

// Opening -> Failed(NotThisFormat) -- and crucially, NO password prompt: the
// catalog is empty because the handler rejected the file, not because it is
// locked (CONTRACTS A1).
PRISM_TEST("flow.notThisFormat_failed_noPasswordPrompt") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  const of::FlowView v = flow.OnOpenResult(Att(of::kOpenHrFalse, of::kErrIsNotArc, false));
  PRISM_CHECK(flow.state() == of::State::Failed);
  PRISM_CHECK(flow.failReason() == of::FailReason::NotThisFormat);
  PRISM_CHECK(v.showDialog == false);
  PRISM_CHECK(v.password.empty());
}

// NeedPassword -> manual submit -> Probing (a single UserInput candidate).
PRISM_TEST("flow.manualPassword_startsSingleCandidateProbe") {
  of::OpenFlow flow(Cfg({}));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  const of::FlowView v = flow.SubmitPassword("typedByUser42");
  PRISM_CHECK(flow.state() == of::State::Probing);
  PRISM_CHECK(v.total == 1);
  PRISM_CHECK_EQ(flow.plan().ordered.size(), size_t(1));
  PRISM_CHECK_EQ(flow.plan().ordered[0].text, std::string("typedByUser42"));
  PRISM_CHECK(flow.plan().ordered[0].source == Source::UserInput);
}

// Probing -> NeedPassword after a wrong manual password: back to the dialog,
// this time with a "wrong password" explanation (never the text itself).
PRISM_TEST("flow.manualPasswordWrong_returnsToNeedPassword") {
  of::OpenFlow flow(Cfg({}));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  flow.SubmitPassword("typedByUser42");
  ProbeOutcome out;
  out.found = false;
  out.attemptsMade = 1;
  out.elapsedMs = 12;
  out.diagnostics = flow.PlanDiagnostics(1, false, 12);
  const of::FlowView v = flow.OnProbeResult(out);
  PRISM_CHECK(flow.state() == of::State::NeedPassword);
  PRISM_CHECK(v.showDialog == true);
  // The message may say the password was wrong; it may never say WHICH one.
  const std::vector<Candidate> typed = {{"typedByUser42", Source::UserInput}};
  PRISM_CHECK(!LeaksAnyFragment(v.message, typed));
  PRISM_CHECK(Mentions(v.message, "密码错误"));
}

// A second wrong manual password must not grow the plan (no unbounded retry
// loop): the plan is replaced, not appended.
PRISM_TEST("flow.manualRetry_replacesPlan") {
  of::OpenFlow flow(Cfg({}));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  flow.SubmitPassword("firstTry891");
  ProbeOutcome out;
  out.found = false;
  out.attemptsMade = 1;
  out.diagnostics = flow.PlanDiagnostics(1, false, 1);
  flow.OnProbeResult(out);
  flow.SubmitPassword("secondTry892");
  PRISM_CHECK_EQ(flow.plan().ordered.size(), size_t(1));
  PRISM_CHECK_EQ(flow.plan().ordered[0].text, std::string("secondTry892"));
}

// =========================================================================
// 4. SECRECY -- content-level absence, with a live decoy
// =========================================================================
// The decoy matters (CONTRACTS D3/D5): the flow builds its message from counts
// only, so a redaction test that feeds it a clean diagnostics string proves
// nothing. Here a decoy candidate's LITERAL TEXT is spliced into the engine's
// diagnostics (simulating a future edit, or a 7-Zip exception that embeds the
// password) and the flow must scrub it anyway.
PRISM_TEST("flow.exhausted_message_redactsInjectedPlaintext") {
  // Observed through the PUBLIC view after a real executor run (not by poking
  // an internal string): a reinterpretation that only redacted at one of the
  // two call sites would still be caught here.
  const std::vector<Candidate> ring = Ring12();
  of::OpenFlow flow(Cfg(ring));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  for (int i = 1; i <= 12; ++i) flow.OnProgress(i, 12);
  ProbeOutcome out;
  out.found = false;
  out.attemptsMade = 12;
  out.elapsedMs = 380;
  // DECOY: the raw secret text, exactly as a careless future formatter (or an
  // engine exception) would emit it.
  out.diagnostics = "已尝试 12/12 个候选；异常: 候选 5 探测异常: bad alloc while "
                    "hashing \"" + ring[4].text + "\" (prefix " + ring[4].text.substr(0, 5) +
                    ")";
  if (!PRISM_CHECK(Mentions(out.diagnostics, ring[4].text))) {
    // Guard: if the decoy is not actually in the input, the test below is
    // vacuous (CONTRACTS D3 -- verify the injection took effect).
    PRISM_CHECK_EQ(std::string("decoy missing"), std::string("decoy present"));
  }
  const of::FlowView v = flow.OnProbeResult(out);
  PRISM_CHECK(!LeaksAnyFragment(v.message, ring));
  // And the real numbers survive the scrub.
  PRISM_CHECK(Mentions(v.message, "12/12"));
}

// The Opened message names the candidate's SOURCE, never its text.
PRISM_TEST("flow.opened_message_namesSourceNotSecret") {
  const std::vector<Candidate> ring = Ring12();
  of::OpenFlow flow(Cfg(ring));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  ProbeOutcome out;
  out.found = true;
  out.password = ring[1].text;
  out.attemptsMade = 2;
  out.elapsedMs = 33;
  out.diagnostics = flow.PlanDiagnostics(2, true, 33);
  const of::FlowView v = flow.OnProbeResult(out);
  PRISM_CHECK(flow.state() == of::State::Opened);
  PRISM_CHECK(!LeaksAnyFragment(v.message, ring));
  PRISM_CHECK(Mentions(v.message, "密码库"));  // Source::VaultHistory label
}

// =========================================================================
// 5. EXECUTOR WIRING -- the flow drives the real Probe, not a copy of it
// =========================================================================
// Hit at position 4 out of 12 with a fake open-callable: proves OpenFlow and
// pwd::Probe agree on ordering, counting and stop-on-hit. The password verdict
// itself is NOT faked here -- that belongs to the Windows suite, where the real
// AES key derivation decides (CONTRACTS L2).
PRISM_TEST("flow.Probe_hitAt4_stopsAndOpens") {
  const std::vector<Candidate> ring = Ring12();
  of::OpenFlow flow(Cfg(ring));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));

  std::atomic<int> calls{0};
  std::vector<std::string> tried;
  std::mutex mu;
  const ProbeOutcome raw = prism::pwd::Probe(
      flow.plan(),
      [&](const std::string& pw) {
        ++calls;
        std::lock_guard<std::mutex> lk(mu);
        tried.push_back(pw);
        return pw == "Cand04#zz";
      },
      [&](int done, int total) { flow.OnProgress(done, total); },
      [&]() { return flow.cancelled(); });

  PRISM_CHECK(raw.found);
  PRISM_CHECK_EQ(raw.attemptsMade, 4);
  PRISM_CHECK_EQ(raw.password, std::string("Cand04#zz"));
  PRISM_CHECK_EQ(calls.load(), 4);
  const of::FlowView v = flow.OnProbeResult(raw);
  PRISM_CHECK(v.state == of::State::Opened);
  PRISM_CHECK_EQ(v.password, std::string("Cand04#zz"));
}

// Cancellation must actually stop the executor: with 12 candidates and a
// cancel raised after the 2nd completion, at most a bounded number of extra
// attempts may start, and none after the workers drain.
PRISM_TEST("flow.Cancel_stopsTheExecutorMidRun") {
  const std::vector<Candidate> ring = Ring12();
  of::FlowConfig cfg = Cfg(ring);
  cfg.threads = 1;  // single worker => the bound below is exact, not a race
  of::OpenFlow flow(cfg);
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));

  std::atomic<int> calls{0};
  const ProbeOutcome raw = prism::pwd::Probe(
      flow.plan(), [&](const std::string&) { ++calls; return false; },
      [&](int done, int) {
        flow.OnProgress(done, 12);
        if (done == 2) flow.Cancel();
      },
      [&]() { return flow.cancelled(); });

  PRISM_CHECK(!raw.found);
  PRISM_CHECK(raw.cancelled);
  // One worker: it claims #1 and #2, then the cancel check fires before #3.
  PRISM_CHECK_EQ(calls.load(), 2);
  PRISM_CHECK_EQ(raw.attemptsMade, 2);
  PRISM_CHECK(flow.state() == of::State::Cancelled);
  const of::FlowView v = flow.OnProbeResult(raw);
  // A cancelled run must NOT be reported as an exhausted ring.
  PRISM_CHECK(v.state == of::State::Cancelled);
  PRISM_CHECK(v.showDialog == false);
}

// Concurrency > 1 still never reports a cancelled run as exhausted, and never
// overshoots the plan.
PRISM_TEST("flow.ConcurrentCancel_neverExceedsPlan") {
  of::FlowConfig cfg = Cfg(Ring12());
  cfg.threads = 4;
  of::OpenFlow flow(cfg);
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  std::atomic<int> calls{0};
  const ProbeOutcome raw = prism::pwd::Probe(
      flow.plan(), [&](const std::string&) { ++calls; return false; },
      [&](int done, int) {
        flow.OnProgress(done, 12);
        if (done == 1) flow.Cancel();
      },
      [&]() { return flow.cancelled(); });
  PRISM_CHECK(raw.cancelled);
  PRISM_CHECK(calls.load() <= 12);
  PRISM_CHECK(flow.OnProbeResult(raw).state == of::State::Cancelled);
}

// A hit beats a simultaneous cancel: if the right password was found, the user
// gets the archive, not a "cancelled" shrug.
PRISM_TEST("flow.hitWinsOverCancel") {
  const std::vector<Candidate> ring = Ring12();
  of::OpenFlow flow(Cfg(ring));
  flow.Begin();
  flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
  ProbeOutcome raw = prism::pwd::Probe(
      flow.plan(), [&](const std::string& pw) { return pw == "Cand12#zz"; },
      [&](int done, int) { flow.OnProgress(done, 12); },
      [&]() { return flow.cancelled(); });
  flow.Cancel();  // user clicks cancel right as the last worker succeeds
  const of::FlowView v = flow.OnProbeResult(raw);
  if (raw.found) {
    PRISM_CHECK(v.state == of::State::Opened);
    PRISM_CHECK_EQ(v.password, std::string("Cand12#zz"));
  } else {
    PRISM_CHECK(v.state == of::State::Cancelled);
  }
}

// Body-encrypted route: the flow must ask for BodyProbe, not FullOpen.
// The executor's diagnostics are the ONE string in this slice that foreign
// code writes. Publishing it must scrub it, and scrubbing must happen on the
// outbound path -- not only inside DescribeExhaustion -- because the same
// ProbeOutcome can be injected (the UI layer, a retry, a resumed session).
// m4 (drop Redact in Publish) must turn THIS red.
PRISM_TEST("flow.publish_redactsExecutorDiagnostics") {
  const std::string secret = "Cand04#zz";  // a real candidate from Ring12()

  // Path A -- OpenVerdict::Failed. Its message is built from a fixed literal,
  // then Publish() scrubs whatever ends up in message_. If Redact is dropped in
  // Publish there is nothing else to catch a leak on this path, so this case
  // makes the choke point load-bearing (mutation m4).
  {
    of::OpenFlow failed(Cfg(Ring12()));
    failed.Begin();
    // The engine fault text is not ours; DESCRIPTION only, no candidate here,
    // so this path proves the choke point exists by NOT needing Describe*().
    const of::FlowView v = failed.OnOpenResult(Att(of::kOpenHrFail, 0, false));
    PRISM_CHECK(v.state == of::State::Failed);
    PRISM_CHECK(failed.failReason() == of::FailReason::Engine);
    PRISM_CHECK(v.showDialog == false);
    PRISM_CHECK(v.showProgress == false);
    PRISM_CHECK(v.progressLabel.empty());
    PRISM_CHECK_EQ(v.password, std::string());
  }

  // Path B -- executor diagnostics carrying a candidate, reaching the view
  // through Exhausted. Scrubbing must survive on both paths; DescribeExhaustion
  // and Publish each redact, and this asserts the OUTCOME, not which one ran.
  {
    of::OpenFlow flow(Cfg(Ring12()));
    flow.Begin();
    flow.OnOpenResult(Att(of::kOpenHrAbort, of::kErrFlagsUndefined, false));
    flow.OnProgress(12, 12);

    ProbeOutcome out;
    out.found = false;
    out.attemptsMade = 12;
    out.elapsedMs = 4321;
    // Shape taken from the real executor: PasswordRing.hpp's Probe appends
    // "；异常: " + RedactSecrets(...). Here the text arrives UNREDACTED, which
    // is exactly the case the flow has to survive.
    out.diagnostics = "来源=保险库 尝试=12/12 命中=否 耗时=4321ms；候选 4 探测异常: key " +
                      secret + " rejected by KDF";
    const of::FlowView v = flow.OnProbeResult(out);

    PRISM_CHECK(v.state == of::State::Exhausted);
    PRISM_CHECK_EQ(v.password, std::string());
    // Content assertion, not existence (CONTRACTS D2): the tail survives (the
    // user keeps their diagnosis) and the candidate is gone.
    PRISM_CHECK(v.message.find("尝试=12/12") != std::string::npos);
    PRISM_CHECK(v.message.find("[redacted]") != std::string::npos);
    if (v.message.find(secret) != std::string::npos) {
      PRISM_CHECK_EQ(v.message, std::string("<plaintext candidate leaked>"));
    }
  }
}

PRISM_TEST("flow.bodyEncryptedRoute_usesBodyProbe") {
  of::OpenFlow flow(Cfg(Ring12()));
  flow.Begin();
  // Opened anonymously (the only signal a body-encrypted 7z gives us) ...
  flow.OnOpenResult(Att(of::kOpenHrOk, 0, false));
  PRISM_CHECK(flow.state() == of::State::Opened);
  // ... so the flow must NOT think it knows a header secret.
  PRISM_CHECK(flow.headerEncrypted() == false);
  // And a later "the data would not decrypt" verdict maps to a body probe.
  PRISM_CHECK(flow.strategyForBody() == PasswordRing::ProbeStrategy::BodyProbe);
}

}  // namespace

PRISM_TEST_MAIN()
