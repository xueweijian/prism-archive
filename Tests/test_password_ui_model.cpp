// PrismArchive -- the password dialog's decision model (card C35).
//
// Everything here is headless: no windows.h, no 7-Zip, no vault backend. The
// rules pinned below are the ones the Win32 dialog is NOT allowed to re-derive
// (Tests/win/test_password_ui.cpp drives the same model through real HWNDs).
//
// What is asserted, in the order the state machine runs:
//   * every transition of Idle/NeedPassword/Probing/Wrong/Exhausted/Opened/
//     Cancelled, including the monotone 3/12 progress label;
//   * the candidate source tags (filename hint / comment / vault / user input);
//   * masking: last two CHARACTERS at most, full mask for <= 2 characters, and
//     a content-level absence check for the whole plaintext (CONTRACTS D2);
//   * the manual retry budget: the fourth attempt is refused, not silently
//     dropped, and the refusal does not consume another attempt;
//   * remember-vs-once: a wrong guess never persists, an automatic ring hit
//     never persists, and TakeVaultPassword() is one-shot;
//   * redaction: a decoy plaintext injected through diagnostics is scrubbed
//     before it can reach the view (and the decoy is proved present first).
//
// Negative controls (CONTRACTS D3, registered in Tests/suites/c35-pwd.cmake):
//   PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT   MaskSecret returns the plaintext.
//   PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES  UiBudgetAllows always says yes.
// Both are compiled from THIS source. main() arms PASSWORD-UI MUTANT CAUGHT
// BEFORE the first assertion (D27): mini_test prints it at the first failed
// check and leaves with 0, so nothing later can swallow it. The marker is still
// the conjunction (D3 + D26): a dead injection never fails, so it never prints.
#include <cstdio>
#include <string>
#include <vector>

#include "../Src/PrismCore/PasswordUiModel.hpp"
#include "mini_test.hpp"

namespace {

using prism::pwd::Candidate;
using prism::pwd::MaskSecret;
using prism::pwd::PasswordUiConfig;
using prism::pwd::PasswordUiModel;
using prism::pwd::RememberChoice;
using prism::pwd::Source;
using prism::pwd::UiState;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Bullets(size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) out += "\xE2\x80\xA2";
  return out;
}

// Content-level leak check (CONTRACTS D2/D5): a full match OR any >= 4-char
// fragment is a leak. The mask intentionally exposes the last two characters, so
// fragments shorter than 4 are not counted -- that is the documented preview, and
// a test that rejected it would reject the feature. For secrets shorter than 4
// there is no safe fragment: the whole string is checked.
bool Leaks(const std::string& haystack, const std::string& secret) {
  if (secret.empty()) return false;
  if (Contains(haystack, secret)) return true;
  if (secret.size() < 4) return false;
  for (size_t len = 4; len <= secret.size(); ++len) {
    for (size_t i = 0; i + len <= secret.size(); ++i) {
      if (Contains(haystack, secret.substr(i, len))) return true;
    }
  }
  return false;
}

Candidate C(const std::string& text, Source source) {
  Candidate candidate;
  candidate.text = text;
  candidate.source = source;
  return candidate;
}

// The twelve-candidate ring the 3/12 label counts against. All vault tier: the
// hit-at-4 source assertion below then has exactly one answer.
std::vector<Candidate> Ring12() {
  std::vector<Candidate> ring;
  for (int i = 1; i <= 12; ++i) {
    const std::string n = i < 10 ? "0" + std::to_string(i) : std::to_string(i);
    ring.push_back(C("Cand" + n + "#zz", Source::VaultHistory));
  }
  return ring;
}

PasswordUiConfig Config(std::vector<Candidate> candidates, int budget = 3,
                         bool canRemember = true) {
  PasswordUiConfig config;
  config.archiveName = "fixture.7z";
  config.candidates = std::move(candidates);
  config.manualBudget = budget;
  config.canRemember = canRemember;
  return config;
}

// Drives one manual attempt to completion. Returns the finished view.
prism::pwd::UiView Attempt(PasswordUiModel& model, const std::string& secret,
                           bool found, RememberChoice choice = RememberChoice::Once) {
  model.Submit(secret, choice);
  return model.OnProbeFinished(found, /*cancelled=*/false, found ? secret : std::string());
}

}  // namespace

// ===========================================================================
// 1. Start / progress
// ===========================================================================

PRISM_TEST("model: idle start with an empty ring asks for the password") {
  PasswordUiModel model(Config({}));
  PRISM_CHECK_EQ(model.state(), UiState::Idle);
  PRISM_CHECK(!model.view().showDialog);

  const prism::pwd::UiView view = model.Start();
  PRISM_CHECK_EQ(model.state(), UiState::NeedPassword);
  PRISM_CHECK(view.showDialog);
  PRISM_CHECK(!view.showProgress);
  PRISM_CHECK_EQ(view.total, 0);
  PRISM_CHECK(Contains(view.message, "type the password"));
  PRISM_CHECK(view.canSubmit);
}

PRISM_TEST("model: idle start with a ring goes straight to probing 0/12") {
  PasswordUiModel model(Config(Ring12()));
  const prism::pwd::UiView view = model.Start();
  PRISM_CHECK_EQ(model.state(), UiState::Probing);
  PRISM_CHECK(!view.showDialog);
  PRISM_CHECK(view.showProgress);
  PRISM_CHECK_EQ(view.done, 0);
  PRISM_CHECK_EQ(view.total, 12);
  PRISM_CHECK_EQ(view.progressLabel, std::string("matching 0/12"));
}

PRISM_TEST("model: progress is monotone and the label is exactly matching 3/12") {
  PasswordUiModel model(Config(Ring12()));
  model.Start();
  prism::pwd::UiView view = model.OnProgress(3, 12);
  PRISM_CHECK_EQ(view.done, 3);
  PRISM_CHECK_EQ(view.progressLabel, std::string("matching 3/12"));

  // A late worker callback must never rewind the pill: "matching 11/12" falling
  // back to 9/12 reads as a hang.
  view = model.OnProgress(2, 12);
  PRISM_CHECK_EQ(view.done, 3);
  PRISM_CHECK_EQ(view.progressLabel, std::string("matching 3/12"));

  view = model.OnProgress(99, 12);
  PRISM_CHECK_EQ(view.done, 12);
  PRISM_CHECK_EQ(view.progressLabel, std::string("matching 12/12"));

  // Outside Probing progress is ignored entirely.
  model.OnProbeFinished(false, false);
  PRISM_CHECK_EQ(model.state(), UiState::NeedPassword);
  PRISM_CHECK_EQ(model.OnProgress(7, 12).done, 12);
}

// ===========================================================================
// 2. Candidate sources + automatic ring outcomes
// ===========================================================================

PRISM_TEST("model: candidate source tags are the fixed C5 tiers") {
  std::vector<Candidate> ring;
  ring.push_back(C("fileHintPw1", Source::FilenameHint));
  ring.push_back(C("commentPw22", Source::ArchiveComment));
  ring.push_back(C("sidecarPw33", Source::SidecarText));
  ring.push_back(C("vaultPw444", Source::VaultHistory));
  PasswordUiModel model(Config(std::move(ring)));

  const std::vector<prism::pwd::CandidateView>& views = model.Candidates();
  PRISM_CHECK_EQ(views.size(), size_t{4});
  // Prior order: filename (50) > comment (40) > sidecar (30) > vault history (20).
  // Source has no operator<< in C5's header, so these are PRISM_CHECK (the
  // preflight B6 gate only requires operator<< for enums used with CHECK_EQ).
  PRISM_CHECK(views[0].source == Source::FilenameHint);
  PRISM_CHECK(views[1].source == Source::ArchiveComment);
  PRISM_CHECK(views[2].source == Source::SidecarText);
  PRISM_CHECK(views[3].source == Source::VaultHistory);
  PRISM_CHECK(Contains(views[0].label, prism::pwd::SourceLabel(Source::FilenameHint)));
  PRISM_CHECK(Contains(views[1].label, prism::pwd::SourceLabel(Source::ArchiveComment)));
  PRISM_CHECK(Contains(views[2].label, prism::pwd::SourceLabel(Source::SidecarText)));
  PRISM_CHECK(Contains(views[3].label, prism::pwd::SourceLabel(Source::VaultHistory)));
  for (const prism::pwd::CandidateView& view : views) {
    PRISM_CHECK(!view.masked.empty());
    // The label CONTAINS the mask (source + mask) by design; the mask itself may
    // only show two trailing characters, so a 4-character fragment is absent.
    PRISM_CHECK(view.label.find(view.masked) != std::string::npos);
  }
}

PRISM_TEST("model: an automatic ring miss returns to NeedPassword with the counts") {
  PasswordUiModel model(Config(Ring12()));
  model.Start();
  model.OnProgress(12, 12);
  const prism::pwd::UiView view = model.OnProbeFinished(false, false);
  PRISM_CHECK_EQ(model.state(), UiState::NeedPassword);
  PRISM_CHECK(view.showDialog);
  PRISM_CHECK(!view.showProgress);
  PRISM_CHECK(Contains(view.message, "12/12"));
  PRISM_CHECK(Contains(view.message, "Type the password"));
  PRISM_CHECK(model.view().masked.empty());
  PRISM_CHECK(!model.view().message.empty());
}

PRISM_TEST("model: an automatic hit opens, names the source, never the secret") {
  PasswordUiModel model(Config(Ring12()));
  model.Start();
  model.OnProgress(4, 12);
  const prism::pwd::UiView view = model.OnProbeFinished(true, false, "Cand04#zz");
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK(view.opened);
  PRISM_CHECK_EQ(model.openedPassword(), std::string("Cand04#zz"));
  PRISM_CHECK_EQ(view.masked, Bullets(7) + "zz");
  PRISM_CHECK(Contains(view.sourceLabel, prism::pwd::SourceLabel(Source::VaultHistory)));
  PRISM_CHECK(!Leaks(view.message, "Cand04#zz"));
  PRISM_CHECK(!Leaks(view.masked, "Cand04#zz"));
  // Automatic hits are not persisted behind the user's back.
  PRISM_CHECK(!model.ShouldPersist());
  PRISM_CHECK(model.TakeVaultPassword().empty());
}

// ===========================================================================
// 3. Manual retries and the budget
// ===========================================================================

PRISM_TEST("model: a manual submit probes exactly one candidate and masks it") {
  PasswordUiModel model(Config({}));
  model.Start();
  const prism::pwd::UiView view = model.Submit("hunter2", RememberChoice::Once);
  PRISM_CHECK_EQ(model.state(), UiState::Probing);
  PRISM_CHECK_EQ(view.total, 1);
  PRISM_CHECK_EQ(view.done, 0);
  PRISM_CHECK_EQ(view.progressLabel, std::string("matching 0/1"));
  PRISM_CHECK_EQ(view.masked, Bullets(5) + "r2");
  PRISM_CHECK_EQ(view.sourceLabel, std::string(prism::pwd::SourceLabel(Source::UserInput)));
  PRISM_CHECK(!view.canSubmit);  // a probe is in flight
  PRISM_CHECK(!Leaks(view.masked, "hunter2"));
  PRISM_CHECK(!Leaks(view.message, "hunter2"));
  PRISM_CHECK_EQ(model.attemptsUsed(), 1);
  PRISM_CHECK_EQ(model.attemptsLeft(), 2);
}

PRISM_TEST("model: a wrong password counts down, then exhausts the budget") {
  PasswordUiModel model(Config({}));
  model.Start();

  // Secrets deliberately avoid English words: a secret like "wrongOne1" shares
  // a 4-character fragment with the fixed message ("wrong") and would make the
  // leak checker report a false positive (CONTRACTS D2 is content-level).
  prism::pwd::UiView view = Attempt(model, "Xy9#Qp2L", false);
  PRISM_CHECK_EQ(model.state(), UiState::Wrong);
  PRISM_CHECK_EQ(model.wrongCount(), 1);
  PRISM_CHECK_EQ(model.attemptsUsed(), 1);
  PRISM_CHECK_EQ(model.attemptsLeft(), 2);
  PRISM_CHECK_EQ(view.attemptsLeft, 2);
  PRISM_CHECK(view.showDialog);
  PRISM_CHECK(view.masked.empty());  // a rejected guess is not echoed, not even its tail
  PRISM_CHECK(Contains(view.message, "wrong password"));
  PRISM_CHECK(Contains(view.message, "2 tries left"));
  PRISM_CHECK(!Leaks(view.message, "Xy9#Qp2L"));

  view = Attempt(model, "mN4$Rt8v", false);
  PRISM_CHECK_EQ(model.state(), UiState::Wrong);
  PRISM_CHECK_EQ(model.attemptsLeft(), 1);

  view = Attempt(model, "kL6!Wq3z", false);
  PRISM_CHECK_EQ(model.state(), UiState::Exhausted);
  PRISM_CHECK_EQ(model.attemptsUsed(), 3);
  PRISM_CHECK_EQ(model.attemptsLeft(), 0);
  PRISM_CHECK(view.exhausted);
  PRISM_CHECK(view.showDialog);
  PRISM_CHECK(!view.canSubmit);
  PRISM_CHECK(Contains(view.message, "exhausted"));
  PRISM_CHECK(!Leaks(view.message, "kL6!Wq3z"));
}

PRISM_TEST("model: the fourth attempt is refused and consumes no budget (D3 control)") {
  PasswordUiModel model(Config({}, /*budget=*/3));
  model.Start();
  for (int i = 1; i <= 3; ++i) {
    Attempt(model, "spentAttempt" + std::to_string(i), false);
  }
  PRISM_CHECK_EQ(model.state(), UiState::Exhausted);
  const int usedBefore = model.attemptsUsed();
  const int wrongBefore = model.wrongCount();

  const prism::pwd::UiView view = model.Submit("neverTried99", RememberChoice::Once);
  // PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES accepts this submit: state becomes
  // Probing and attemptsUsed grows -- BOTH checks go red and the marker fires.
  PRISM_CHECK_EQ(model.state(), UiState::Exhausted);
  PRISM_CHECK_EQ(model.attemptsUsed(), usedBefore);
  PRISM_CHECK_EQ(model.wrongCount(), wrongBefore);
  PRISM_CHECK(!view.canSubmit);
  PRISM_CHECK(Contains(view.message, "exhausted"));
  PRISM_CHECK(!model.PickVault(0, RememberChoice::Once));
}

PRISM_TEST("model: an empty submit is refused with a reason and consumes nothing") {
  PasswordUiModel model(Config({}));
  model.Start();
  const prism::pwd::UiView view = model.Submit("", RememberChoice::Once);
  PRISM_CHECK_EQ(model.state(), UiState::NeedPassword);
  PRISM_CHECK_EQ(model.attemptsUsed(), 0);
  PRISM_CHECK(Contains(view.message, "enter a password"));
}

PRISM_TEST("model: submitting while probing or before Start is refused") {
  PasswordUiModel idle(Config({}));
  const prism::pwd::UiView idleView = idle.Submit("tooSoon123", RememberChoice::Once);
  PRISM_CHECK_EQ(idle.state(), UiState::Idle);
  PRISM_CHECK_EQ(idle.attemptsUsed(), 0);
  PRISM_CHECK(Contains(idleView.message, "not started"));

  PasswordUiModel probing(Config(Ring12()));
  probing.Start();
  const prism::pwd::UiView probeView = probing.Submit("tooSoon123", RememberChoice::Once);
  PRISM_CHECK_EQ(probing.state(), UiState::Probing);
  PRISM_CHECK(Contains(probeView.message, "already running"));
}

// ===========================================================================
// 4. Cancel semantics
// ===========================================================================

PRISM_TEST("model: cancel stops the run and a late miss stays cancelled") {
  PasswordUiModel model(Config(Ring12()));
  model.Start();
  model.OnProgress(3, 12);
  const prism::pwd::UiView cancelled = model.Cancel();
  PRISM_CHECK_EQ(model.state(), UiState::Cancelled);
  PRISM_CHECK(!cancelled.showProgress);
  PRISM_CHECK(!cancelled.showDialog);
  PRISM_CHECK(cancelled.masked.empty());
  PRISM_CHECK(Contains(cancelled.message, "cancelled"));

  // A stale worker completion arriving after the cancel must not resurrect the
  // flow and must not invent a "wrong password".
  model.OnProgress(7, 12);
  const prism::pwd::UiView late = model.OnProbeFinished(false, false);
  PRISM_CHECK_EQ(model.state(), UiState::Cancelled);
  PRISM_CHECK_EQ(late.done, 3);
  PRISM_CHECK_EQ(model.wrongCount(), 0);
}

PRISM_TEST("model: a hit that lands with the cancel still opens the archive") {
  PasswordUiModel model(Config({}));
  model.Start();
  model.Submit("hunter2", RememberChoice::Once);
  model.Cancel();
  const prism::pwd::UiView view = model.OnProbeFinished(true, false, "hunter2");
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK(view.opened);
  PRISM_CHECK_EQ(model.openedPassword(), std::string("hunter2"));
  // Cancelling an opened archive is a no-op.
  PRISM_CHECK_EQ(model.Cancel().state, UiState::Opened);
}

// ===========================================================================
// 5. Remember vs this time only
// ===========================================================================

PRISM_TEST("model: this time only never persists, not even on success") {
  PasswordUiModel model(Config({}));
  model.Start();
  model.Submit("VaultSecret99", RememberChoice::Once);
  const prism::pwd::UiView view = model.OnProbeFinished(true, false);
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK(view.opened);
  PRISM_CHECK(!view.shouldPersist);
  PRISM_CHECK(!model.ShouldPersist());
  PRISM_CHECK(model.TakeVaultPassword().empty());
  PRISM_CHECK_EQ(model.openedPassword(), std::string("VaultSecret99"));
  PRISM_CHECK(!Leaks(view.masked, "VaultSecret99"));
}

PRISM_TEST("model: remember persists exactly once, and only after a success") {
  PasswordUiModel model(Config({}));
  model.Start();
  model.Submit("VaultSecret99", RememberChoice::Remember);
  // A wrong guess first: the remember box does not persist a rejected secret.
  prism::pwd::UiView view = model.OnProbeFinished(false, false);
  PRISM_CHECK_EQ(model.state(), UiState::Wrong);
  PRISM_CHECK(model.TakeVaultPassword().empty());

  model.Submit("VaultSecret99", RememberChoice::Remember);
  view = model.OnProbeFinished(true, false);
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK(view.shouldPersist);
  PRISM_CHECK(model.ShouldPersist());
  PRISM_CHECK_EQ(model.TakeVaultPassword(), std::string("VaultSecret99"));
  // One-shot: a second save attempt (a retried "remember") must not double-add.
  PRISM_CHECK(model.TakeVaultPassword().empty());
}

PRISM_TEST("model: without a vault backend remember is coerced to once") {
  PasswordUiModel model(Config({}, 3, /*canRemember=*/false));
  PRISM_CHECK_EQ(model.view().remember, RememberChoice::Once);
  model.SetRememberChoice(RememberChoice::Remember);
  PRISM_CHECK_EQ(model.rememberChoice(), RememberChoice::Once);
  PRISM_CHECK(!model.view().canRemember);

  model.Start();
  model.Submit("VaultSecret99", RememberChoice::Remember);
  model.OnProbeFinished(true, false);
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK(!model.ShouldPersist());
  PRISM_CHECK(model.TakeVaultPassword().empty());
}

PRISM_TEST("model: the remember choice is frozen once the archive opened") {
  PasswordUiModel model(Config({}));
  model.Start();
  model.Submit("VaultSecret99", RememberChoice::Once);
  model.OnProbeFinished(true, false);
  model.SetRememberChoice(RememberChoice::Remember);
  PRISM_CHECK_EQ(model.rememberChoice(), RememberChoice::Once);
  PRISM_CHECK(!model.ShouldPersist());
}

// ===========================================================================
// 6. Vault list: labels only, never plaintext
// ===========================================================================

PRISM_TEST("model: vault entries are listed by label and mask, never by secret") {
  PasswordUiConfig config = Config({});
  config.vaultEntries = {"VaultSecretAlpha99", "VaultSecretBeta77"};
  PasswordUiModel model(std::move(config));

  const std::vector<prism::pwd::VaultItemView>& items = model.VaultItems();
  PRISM_CHECK_EQ(items.size(), size_t{2});
  // "VaultSecretAlpha99" is 18 code points -> 16 bullets + "99";
  // "VaultSecretBeta77" is 17 -> 15 bullets + "77".
  PRISM_CHECK_EQ(items[0].masked, Bullets(16) + "99");
  PRISM_CHECK_EQ(items[1].masked, Bullets(15) + "77");
  PRISM_CHECK(!Leaks(items[0].label, "VaultSecretAlpha99"));
  PRISM_CHECK(!Leaks(items[1].label, "VaultSecretBeta77"));
  PRISM_CHECK(items[0].label != "VaultSecretAlpha99");
  PRISM_CHECK(Contains(items[0].label, "99"));
}

PRISM_TEST("model: picking a vault entry probes it and keeps the secret off-screen") {
  PasswordUiConfig config = Config({});
  config.vaultEntries = {"VaultSecretAlpha99", "VaultSecretBeta77"};
  PasswordUiModel model(std::move(config));
  model.Start();
  PRISM_CHECK(model.PickVault(1, RememberChoice::Once));
  prism::pwd::UiView view = model.view();
  PRISM_CHECK_EQ(model.state(), UiState::Probing);
  PRISM_CHECK_EQ(view.total, 1);
  PRISM_CHECK_EQ(view.sourceLabel, std::string(prism::pwd::SourceLabel(Source::VaultHistory)));
  PRISM_CHECK(!Leaks(view.masked, "VaultSecretBeta77"));
  PRISM_CHECK(!Leaks(view.message, "VaultSecretBeta77"));

  view = model.OnProbeFinished(true, false);
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
  PRISM_CHECK_EQ(model.openedPassword(), std::string("VaultSecretBeta77"));
  PRISM_CHECK(!model.ShouldPersist());  // already in the vault, and the choice was Once
  PRISM_CHECK(Contains(view.message, prism::pwd::SourceLabel(Source::VaultHistory)));
  PRISM_CHECK(!Leaks(view.message, "VaultSecretBeta77"));
}

PRISM_TEST("model: a remembered vault pick is handed over once") {
  PasswordUiConfig config = Config({});
  config.vaultEntries = {"VaultSecretAlpha99"};
  PasswordUiModel model(std::move(config));
  model.Start();
  PRISM_CHECK(model.PickVault(0, RememberChoice::Remember));
  model.OnProbeFinished(true, false);
  PRISM_CHECK(model.ShouldPersist());
  PRISM_CHECK_EQ(model.TakeVaultPassword(), std::string("VaultSecretAlpha99"));
  PRISM_CHECK(model.TakeVaultPassword().empty());
  // Out-of-range picks are refused without touching the state.
  PRISM_CHECK(!model.PickVault(99, RememberChoice::Once));
  PRISM_CHECK_EQ(model.state(), UiState::Opened);
}

// ===========================================================================
// 7. Masking itself (the echo negative control lives here)
// ===========================================================================

PRISM_TEST("model: masking shows only the last two code points") {
  // PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT returns the secret here; these checks go
  // red and the suite's D3 marker can fire.
  PRISM_CHECK_EQ(MaskSecret("hunter2"), Bullets(5) + "r2");
  PRISM_CHECK(MaskSecret("hunter2") != std::string("hunter2"));
  PRISM_CHECK(!Leaks(MaskSecret("hunter2"), "hunter2"));
  PRISM_CHECK_EQ(prism::pwd::Utf8CharCount("hunter2"), size_t{7});
  PRISM_CHECK_EQ(prism::pwd::Utf8Suffix("hunter2", 2), std::string("r2"));

  // CJK counts code points, not bytes: masking must not split a UTF-8 sequence.
  const std::string cjk = "\xE5\xAF\x86\xE7\xA0\x81" "123456";  // 8 code points
  PRISM_CHECK_EQ(prism::pwd::Utf8CharCount(cjk), size_t{8});
  PRISM_CHECK_EQ(MaskSecret(cjk), Bullets(6) + "56");
  PRISM_CHECK(!Leaks(MaskSecret(cjk), cjk));
}

PRISM_TEST("model: one- and two-character secrets are masked completely") {
  // A two-character password whose last two characters were shown would be a
  // full echo, so short secrets get no tail preview at all.
  PRISM_CHECK_EQ(MaskSecret("ab"), Bullets(2));
  PRISM_CHECK(MaskSecret("ab").find('a') == std::string::npos);
  PRISM_CHECK(MaskSecret("ab").find('b') == std::string::npos);
  PRISM_CHECK_EQ(MaskSecret("z"), Bullets(1));
  PRISM_CHECK_EQ(MaskSecret(""), std::string());
}

// ===========================================================================
// 8. Redaction + the whole-view absence check
// ===========================================================================

PRISM_TEST("model: diagnostics are redacted before they reach the view") {
  PasswordUiModel model(Config({}));
  model.Start();
  model.Submit("hunter2", RememberChoice::Once);
  const std::string decoy =
      "engine exception: candidate hunter2 failed while hashing unter2";
  // Prove the injection exists before asserting it was scrubbed (D3): if a
  // future edit stops putting the secret in diagnostics, this guard fails too.
  PRISM_CHECK(Contains(decoy, "hunter2"));
  const prism::pwd::UiView view =
      model.OnProbeFinished(false, false, std::string(), decoy);
  PRISM_CHECK(!Leaks(view.message, "hunter2"));
  PRISM_CHECK(Contains(view.message, "wrong password"));
}

PRISM_TEST("model: no view field ever contains the plaintext") {
  PasswordUiModel model(Config({}));
  model.Start();
  const std::string secret = "TopSecret42";
  prism::pwd::UiView view = model.Submit(secret, RememberChoice::Remember);
  PRISM_CHECK(!Leaks(view.masked, secret));
  PRISM_CHECK(!Leaks(view.message, secret));
  PRISM_CHECK(!Leaks(view.sourceLabel, secret));
  PRISM_CHECK(!Leaks(view.progressLabel, secret));
  view = model.OnProbeFinished(true, false);
  PRISM_CHECK(!Leaks(view.masked, secret));
  PRISM_CHECK(!Leaks(view.message, secret));
  PRISM_CHECK(!Leaks(view.sourceLabel, secret));
  PRISM_CHECK_EQ(view.masked, Bullets(9) + "42");
  for (const prism::pwd::CandidateView& candidate : model.Candidates()) {
    PRISM_CHECK(!Leaks(candidate.label, secret));
  }
}

PRISM_TEST("model: showPassword is a display flag with no effect on the state") {
  PasswordUiModel model(Config({}));
  model.Start();
  const UiState before = model.state();
  model.SetShowPassword(true);
  PRISM_CHECK(model.view().showPassword);
  PRISM_CHECK_EQ(model.state(), before);
  model.SetShowPassword(false);
  PRISM_CHECK(!model.view().showPassword);
  PRISM_CHECK_EQ(model.state(), before);
}

int main(int argc, char** argv) {
#if defined(PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT) || \
    defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES)
  // CONTRACTS D3 + D26 + D27: the marker is armed BEFORE the first assertion;
  // ReportFailure prints it at the first failed check and leaves with 0, so no
  // later teardown can swallow it. A dead mutation never fails and the marker
  // never appears, so PASS_REGULAR_EXPRESSION misses and the target is RED.
  prism::test::SetMutationVerdict("PASSWORD-UI MUTANT CAUGHT");
#endif
  const int failures = prism::test::RunAll(argc, argv);
#if defined(PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT) || \
    defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES)
  // Reaching here means no failed check went through ReportFailure, so no
  // verdict was printed. Stay RED (exit 1); never print a late marker (D27).
  std::printf("mutant build: no first-failure verdict (checks=%d failures=%d mutation hits=%d)\n",
              prism::test::CheckCount(), failures, prism::pwd::UiMutationCount());
  return 1;
#else
  return failures;
#endif
}
