// PrismArchive -- PrismCore
// PasswordUiModel: the POLICY half of the password dialog (card C35).
//
// C5 shipped the ring (PasswordRing.hpp), the open flow (OpenFlow.hpp), the
// DPAPI vault (PrismWin/DpapiVault.hpp) and the probe executor -- but nothing
// put them on screen. This header is the UI-facing decision layer: which state
// the dialog is in, what it may show, how many retries are left, what "remember"
// means, and -- the load-bearing rule -- what may NEVER be shown.
//
// The Win32 shell (Src/PrismWin/PasswordDialog.hpp) owns no rules: if a message
// handler there would contain an "if" that decides legality, that "if" belongs in
// this file. The same split as C18's CompressDialog/DialogState, for the same
// reason: the rules must be provable headless on every lane, and the HWND test
// must prove the wiring rather than re-derive the policy.
//
// ---------------------------------------------------------------------------
// SECRECY CONTRACT (CONTRACTS D2/D3/D25 today, and the reason this file has a
// negative control of its own):
//
//   * No public UiView field ever carries a plaintext password. The view has
//     "masked" (last two characters only, or all bullets for <= 2 chars) and
//     "message" (counts, fixed labels, redacted diagnostics). That is the whole
//     surface a renderer can touch.
//   * MaskSecret("") == ""; a 1- or 2-character secret is masked COMPLETELY.
//     Showing the last two characters of a two-character password would be a
//     full echo, so short secrets sacrifice the tail preview on purpose.
//   * The opened password is available through openedPassword() (the caller has
//     to open the archive with it) and never through the view. The vault copy is
//     one-shot: TakeVaultPassword() returns it once and then refuses, so a
//     "remember" retry cannot add the same secret twice.
//   * Publish() runs Redact() over message_ LAST and against every secret the
//     model knows (candidates, vault entries, pending attempt, opened password),
//     so a future message edit that embeds plaintext is still scrubbed. The
//     tests inject a decoy secret through diagnostics to prove that gate is live
//     (CONTRACTS D3: verify the injection actually fired).
//
// ---------------------------------------------------------------------------
// STATE MACHINE (one test case per transition; see
// Tests/test_password_ui_model.cpp):
//
//   Idle --Start()--> NeedPassword            when there is nothing to try silently
//   Idle --Start()--> Probing(0/N)            when the ring has candidates
//   Probing --OnProgress()--> Probing(d/N)    monotone high-water mark, never rewinds
//   Probing --hit--> Opened                   Opened wins even over a simultaneous cancel
//   Probing(candidates) --all miss--> NeedPassword   "12/12 missed, type one"
//   NeedPassword/Wrong --Submit()/PickVault()--> Probing(0/1)
//   Probing(manual) --miss--> Wrong           "attempt 1, 2 left"
//   Wrong --Submit()/PickVault()--> Probing
//   Probing(manual) --miss with no budget left--> Exhausted   (terminal)
//   any live state --Cancel()--> Cancelled    a late hit still opens (see above)
//
// Budget: kManualAttemptBudget typed attempts. The budget is consumed when an
// attempt STARTS, not when it fails, so a hit on the last attempt still opens
// and a user cannot be charged twice for one probe. The negative control
// PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES removes the check; the suite must go red
// only when the mutation actually granted an over-budget attempt.
//
// "Remember" semantics: the choice is frozen at success. Remember only persists
// a password the user typed/selected AND the probe proved; a wrong attempt is
// never persisted. Automatic candidate hits are never persisted behind the
// user's back -- they were already in the vault or were guessed from a filename.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "PasswordRing.hpp"   // Candidate / Source / Rank / SecureZero / RedactSecrets
#include "PasswordVault.hpp"  // PasswordKdfClass (budget provenance lives there)

namespace prism::pwd {

// ---------------------------------------------------------------------------
// States and the remember policy. Both are printed by failing assertions in the
// suites, so CONTRACTS B6 requires operator<< in THIS header.
// ---------------------------------------------------------------------------
enum class UiState : uint8_t {
  Idle,
  NeedPassword,
  Probing,
  Wrong,
  Exhausted,
  Opened,
  Cancelled
};

inline const char* UiStateName(UiState state) {
  switch (state) {
    case UiState::Idle: return "Idle";
    case UiState::NeedPassword: return "NeedPassword";
    case UiState::Probing: return "Probing";
    case UiState::Wrong: return "Wrong";
    case UiState::Exhausted: return "Exhausted";
    case UiState::Opened: return "Opened";
    case UiState::Cancelled: return "Cancelled";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, UiState state) {
  return os << UiStateName(state);
}

// "Remember" vs "this time only". Default is Once: a password manager that
// remembers by default is a password manager that leaks by default.
enum class RememberChoice : uint8_t { Once = 0, Remember = 1 };

inline const char* RememberChoiceName(RememberChoice choice) {
  switch (choice) {
    case RememberChoice::Once: return "Once";
    case RememberChoice::Remember: return "Remember";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, RememberChoice choice) {
  return os << RememberChoiceName(choice);
}

// How many typed/selected guesses the dialog allows before it locks. Three is
// the same order of magnitude as the AES silent budget's high-prior tier and is
// small enough that "unlimited retry" is a visible, testable defect.
inline constexpr int kManualAttemptBudget = 3;

// How many trailing characters a mask may show. Two: enough for a human to tell
// two candidate passwords apart, not enough to reconstruct one.
inline constexpr size_t kMaskSuffixChars = 2;

// ---------------------------------------------------------------------------
// D3 mutation plumbing. Only the mutant builds enter these branches; the counter
// is shared by every inline copy through ODR, so the test's main() can ask
// whether the injection ever ran (CONTRACTS D3: a dead mutation prints no
// CAUGHT marker and the suite stays red instead of silently passing).
// ---------------------------------------------------------------------------
inline int& UiMutationCount() {
  static int count = 0;
  return count;
}

inline bool UiMutationFired() { return UiMutationCount() > 0; }

// ---------------------------------------------------------------------------
// UTF-8 masking. Byte-wise masking would split CJK passwords and produce
// invalid UTF-8 in a control; counting code points keeps the tail readable.
// ---------------------------------------------------------------------------
inline size_t Utf8CharCount(std::string_view text) {
  size_t count = 0;
  for (unsigned char byte : text) {
    if ((byte & 0xC0u) != 0x80u) ++count;  // not a continuation byte
  }
  return count;
}

inline std::string Utf8Suffix(std::string_view text, size_t chars) {
  if (chars == 0 || text.empty()) return std::string();
  size_t seen = 0;
  size_t pos = text.size();
  while (pos > 0) {
    --pos;
    if ((static_cast<unsigned char>(text[pos]) & 0xC0u) != 0x80u) {
      if (seen == chars) {
        ++pos;
        break;
      }
      ++seen;
    }
  }
  if (seen < chars) return std::string(text);
  return std::string(text.substr(pos));
}

// The ONLY mask implementation. The mutant returns the plaintext so the suite
// goes red exactly where an accidental echo would; it also increments the
// counter so a green mutant is impossible to mistake for a caught mutation.
inline std::string MaskSecret(std::string_view secret) {
  if (secret.empty()) return std::string();
#if defined(PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT)
  ++UiMutationCount();
  return std::string(secret);
#else
  const std::string bullet = "\xE2\x80\xA2";  // U+2022 BULLET
  const size_t chars = Utf8CharCount(secret);
  const size_t visible = chars > kMaskSuffixChars ? kMaskSuffixChars : 0;
  std::string out;
  out.reserve((chars - visible) * bullet.size() + visible * 2);
  for (size_t i = 0; i < chars - visible; ++i) out += bullet;
  out += Utf8Suffix(secret, visible);
  return out;
#endif
}

// Budget gate. Kept as a function so the mutant has one injection point and the
// public CanSubmit()/Submit() cannot drift apart (two checks would let one be
// mutated while the other kept the test green).
inline bool UiBudgetAllows(int attemptsUsed, int budget) {
#if defined(PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES)
  (void)attemptsUsed;
  (void)budget;
  return true;
#else
  return attemptsUsed < budget;
#endif
}

// ---------------------------------------------------------------------------
// Views. Every string in these structs is safe to render: no plaintext.
// ---------------------------------------------------------------------------
struct CandidateView {
  std::string label;   // "filename hint ****56"
  std::string masked;  // "****56"
  Source source = Source::UserInput;
  size_t index = 0;
};

struct VaultItemView {
  std::string label;   // "vault 1 (****56)"
  std::string masked;  // "****56"
  size_t index = 0;
};

struct PasswordUiConfig {
  std::string archiveName;                // shown in the title, never a secret
  std::vector<Candidate> candidates;      // sniffed / commented / vault / typed
  std::vector<std::string> vaultEntries;  // DPAPI plaintext in memory, labels on screen
  int manualBudget = kManualAttemptBudget;
  bool canRemember = true;  // false when the vault backend is absent
};

struct UiView {
  UiState state = UiState::Idle;
  bool showDialog = false;    // password dialog belongs on screen
  bool showProgress = false;  // probe progress belongs on screen
  int done = 0;
  int total = 0;
  std::string progressLabel;  // "matching 3/12"
  std::string message;        // already redacted; counts and labels only
  std::string masked;         // current attempt/success, last two chars only
  std::string sourceLabel;    // fixed source tier, never the text
  int wrongCount = 0;
  int attemptsUsed = 0;
  int attemptsLeft = 0;
  int budget = 0;
  bool canSubmit = false;
  bool exhausted = false;
  bool opened = false;
  bool shouldPersist = false;  // success AND the user chose Remember
  bool canRemember = true;
  RememberChoice remember = RememberChoice::Once;
  bool showPassword = false;  // UI toggle, no effect on secrecy
};

// ---------------------------------------------------------------------------
// The model. Construct once per archive, drive it with engine/UI events.
// ---------------------------------------------------------------------------
class PasswordUiModel {
 public:
  PasswordUiModel() { Reset(PasswordUiConfig{}); }
  explicit PasswordUiModel(PasswordUiConfig config) { Reset(std::move(config)); }
  ~PasswordUiModel() { ScrubSecrets(); }
  PasswordUiModel(const PasswordUiModel&) = delete;
  PasswordUiModel& operator=(const PasswordUiModel&) = delete;

  // Re-arms for a new archive. Also the dialog's SetConfig path: no copy/move of
  // live secrets is needed, so no secret can survive in a moved-from buffer.
  void Reset(PasswordUiConfig config) {
    ScrubSecrets();
    archiveName_ = std::move(config.archiveName);
    budget_ = (std::max)(1, config.manualBudget);
    canRemember_ = config.canRemember;
    rememberChoice_ = RememberChoice::Once;
    showPassword_ = false;

    ranked_ = PasswordRing::Rank(std::move(config.candidates));
    candidateViews_.clear();
    candidateViews_.reserve(ranked_.size());
    for (size_t i = 0; i < ranked_.size(); ++i) {
      CandidateView view;
      view.masked = MaskSecret(ranked_[i].text);
      view.label = std::string(SourceLabel(ranked_[i].source)) + " " + view.masked;
      view.source = ranked_[i].source;
      view.index = i;
      candidateViews_.push_back(std::move(view));
    }

    vaultSecrets_ = std::move(config.vaultEntries);
    vaultViews_.clear();
    vaultViews_.reserve(vaultSecrets_.size());
    for (size_t i = 0; i < vaultSecrets_.size(); ++i) {
      VaultItemView view;
      view.masked = MaskSecret(vaultSecrets_[i]);
      view.label = "\xE4\xBF\x9D\xE9\x99\xA9\xE5\xBA\x93 " + std::to_string(i + 1) +
                   "\xEF\xBC\x88" + view.masked + "\xEF\xBC\x89";
      view.index = i;
      vaultViews_.push_back(std::move(view));
    }

    state_ = UiState::Idle;
    mode_ = ProbeMode::None;
    done_ = 0;
    total_ = 0;
    wrongCount_ = 0;
    attemptsUsed_ = 0;
    shouldPersist_ = false;
    vaultPasswordTaken_ = false;
    message_.clear();
    ScrubPending();
    Publish();
  }

  const std::string& archiveName() const { return archiveName_; }
  const std::vector<CandidateView>& Candidates() const { return candidateViews_; }
  const std::vector<VaultItemView>& VaultItems() const { return vaultViews_; }
  const UiView& view() const { return view_; }

  UiState state() const { return state_; }
  int attemptsUsed() const { return attemptsUsed_; }
  int attemptsLeft() const { return (std::max)(0, budget_ - attemptsUsed_); }
  int budget() const { return budget_; }
  int wrongCount() const { return wrongCount_; }
  bool ShouldPersist() const { return state_ == UiState::Opened && shouldPersist_; }
  const std::string& openedPassword() const { return openedSecret_; }

  // The secret a manual attempt is currently probing. Non-empty only while
  // UiState::Probing (or until OnProbeFinished scrubs it); this is the ONE
  // handoff to the engine, never a display source.
  const std::string& pendingPassword() const { return pendingSecret_; }

  // One-shot vault handoff. Empty unless the open succeeded AND the user chose
  // Remember; the second call is empty so a retry cannot double-add the secret.
  std::string TakeVaultPassword() {
    if (state_ != UiState::Opened || !shouldPersist_ || vaultPasswordTaken_) {
      return std::string();
    }
    vaultPasswordTaken_ = true;
    return openedSecret_;
  }

  // Idle -> NeedPassword | Probing.
  UiView Start() {
    if (state_ != UiState::Idle) return view_;
    if (ranked_.empty()) {
      state_ = UiState::NeedPassword;
      mode_ = ProbeMode::None;
      message_ =
          "This archive is password protected. No candidate password is "
          "available; type the password.";
    } else {
      state_ = UiState::Probing;
      mode_ = ProbeMode::Candidates;
      done_ = 0;
      total_ = static_cast<int>(ranked_.size());
      message_.clear();
    }
    Publish();
    return view_;
  }

  // Probing progress, forwarded from the C5 OpenFlow/pwd::Probe progress
  // callback. Monotone: a stale worker callback must never rewind the pill.
  UiView OnProgress(int done, int total) {
    if (state_ != UiState::Probing) return view_;
    if (total > 0) total_ = total;
    if (done < 0) done = 0;
    if (total_ > 0 && done > total_) done = total_;
    if (done > done_) done_ = done;
    Publish();
    return view_;
  }

  // Probe finished. The password argument is only read when found; for a manual
  // attempt the caller may pass an empty string and the model uses its own
  // pending secret. Diagnostics is untrusted text (C5 builds it, but this layer
  // defends anyway) and is redacted by Publish().
  UiView OnProbeFinished(bool found, bool cancelled, const std::string& password = {},
                         const std::string& diagnostics = {}) {
    if (state_ != UiState::Probing && state_ != UiState::Cancelled) return view_;

    if (found) {  // a hit outranks a simultaneous cancel (same rule as OpenFlow)
      std::string secret = password.empty() ? pendingSecret_ : password;
      openedSecret_ = std::move(secret);
      openedSourceLabel_ = pendingSourceLabel_;
      if (openedSourceLabel_.empty() && mode_ == ProbeMode::Candidates && done_ > 0 &&
          static_cast<size_t>(done_) <= ranked_.size()) {
        openedSourceLabel_ = SourceLabel(ranked_[static_cast<size_t>(done_) - 1].source);
      }
      if (openedSourceLabel_.empty()) openedSourceLabel_ = "candidate";
      // Automatic ring hits were already in the vault or guessed from the file
      // name: persisting them would be a decision the user never made.
      shouldPersist_ = mode_ == ProbeMode::Manual && canRemember_ &&
                       rememberChoice_ == RememberChoice::Remember &&
                       !openedSecret_.empty();
      vaultPasswordTaken_ = false;
      ScrubPending();
      state_ = UiState::Opened;
      message_ = "password matched (attempt " + std::to_string(attemptsUsed_) +
                 "), source: " + openedSourceLabel_ + ".";
      Publish();
      return view_;
    }

    if (cancelled) {
      ScrubPending();
      state_ = UiState::Cancelled;
      message_ = "cancelled: remaining candidates were not tried.";
      Publish();
      return view_;
    }

    if (state_ == UiState::Cancelled) {  // a late miss after cancel stays cancelled
      Publish();
      return view_;
    }

    // Diagnostics can embed engine exception text (C5 redacts, but this layer
    // defends anyway). Redact it BEFORE ScrubPending(), because Redact() uses
    // the pending secret as one of its needles -- scrubbing first would make the
    // decoy invisible to the filter (the tests inject one to prove this order).
    const std::string safeDiagnostics =
        diagnostics.empty() ? std::string() : Redact(diagnostics);

    if (mode_ == ProbeMode::Candidates) {
      const int tried = (std::max)(done_, 0);
      const int total = (std::max)(total_, 0);
      ScrubPending();
      state_ = UiState::NeedPassword;
      message_ = "tried " + std::to_string(tried) + "/" + std::to_string(total) +
                 " candidates, none matched. Type the password.";
      if (!safeDiagnostics.empty()) message_ += " " + safeDiagnostics;
      Publish();
      return view_;
    }

    // A typed/selected guess missed. The budget check here is the SAME predicate
    // as CanSubmit(), so the mutation has exactly one place to break.
    ScrubPending();
    ++wrongCount_;
    if (UiBudgetAllows(attemptsUsed_, budget_)) {
      state_ = UiState::Wrong;
      message_ = "wrong password (attempt " + std::to_string(wrongCount_) + "), " +
                 std::to_string(attemptsLeft()) + " tries left.";
    } else {
      state_ = UiState::Exhausted;
      message_ = "wrong password (attempt " + std::to_string(wrongCount_) +
                 "). retry budget exhausted (" + std::to_string(budget_) + "/" +
                 std::to_string(budget_) + "); no more attempts.";
    }
    if (!safeDiagnostics.empty()) message_ += " " + safeDiagnostics;
    Publish();
    return view_;
  }

  // NeedPassword/Wrong -> Probing(0/1). Empty input and an exhausted budget are
  // refused with a reason, not silently ignored (a disabled button with no
  // explanation is how users conclude the dialog is broken).
  UiView Submit(const std::string& typed, RememberChoice choice) {
    if (!CanSubmit()) {
      RefuseSubmit();
      Publish();
      return view_;
    }
    if (typed.empty()) {
      message_ = "enter a password.";
      Publish();
      return view_;
    }
    rememberChoice_ = canRemember_ ? choice : RememberChoice::Once;
    if (attemptsUsed_ >= budget_) ++UiMutationCount();  // only reachable in the mutant
    BeginManualAttempt(typed, Source::UserInput);
    Publish();
    return view_;
  }

  // "pick from the vault": a vault entry is an explicit user choice and consumes
  // one attempt, exactly like typing it. The plaintext never leaves this object
  // except through openedPassword()/TakeVaultPassword().
  bool PickVault(size_t index, RememberChoice choice) {
    if (!CanSubmit() || index >= vaultSecrets_.size()) {
      RefuseSubmit();
      Publish();
      return false;
    }
    rememberChoice_ = canRemember_ ? choice : RememberChoice::Once;
    if (attemptsUsed_ >= budget_) ++UiMutationCount();  // only reachable in the mutant
    BeginManualAttempt(vaultSecrets_[index], Source::VaultHistory);
    Publish();
    return true;
  }

  // Any live state -> Cancelled. Cancelling an opened archive is not a thing:
  // the user already has the archive, so the state stays Opened.
  UiView Cancel() {
    if (state_ == UiState::Opened || state_ == UiState::Cancelled) return view_;
    ScrubPending();
    state_ = UiState::Cancelled;
    mode_ = ProbeMode::None;
    message_ = "cancelled.";
    Publish();
    return view_;
  }

  // The remember choice, frozen once the run has opened (the persistence
  // decision was already taken; changing the radio afterwards would be a lie).
  void SetRememberChoice(RememberChoice choice) {
    if (state_ == UiState::Opened) return;
    rememberChoice_ = canRemember_ ? choice : RememberChoice::Once;
    Publish();
  }

  RememberChoice rememberChoice() const { return rememberChoice_; }

  void SetShowPassword(bool show) {
    showPassword_ = show;
    Publish();
  }

  bool showPassword() const { return showPassword_; }

 private:
  enum class ProbeMode : uint8_t { None, Candidates, Manual };

  bool CanSubmit() const {
    if (state_ != UiState::NeedPassword && state_ != UiState::Wrong) return false;
    return UiBudgetAllows(attemptsUsed_, budget_);
  }

  void RefuseSubmit() {
    switch (state_) {
      case UiState::Exhausted:
        message_ = "retry budget exhausted; no more attempts.";
        break;
      case UiState::Probing:
        message_ = "a probe is already running.";
        break;
      case UiState::Idle:
        message_ = "the password flow has not started.";
        break;
      case UiState::Cancelled:
        message_ = "cancelled; no more attempts.";
        break;
      case UiState::Opened:
        message_ = "the password already matched.";
        break;
      case UiState::NeedPassword:
      case UiState::Wrong:
        message_ = "retry budget exhausted; no more attempts.";
        break;
    }
  }

  void BeginManualAttempt(const std::string& secret, Source source) {
    ++attemptsUsed_;
    pendingSecret_ = secret;
    pendingMasked_ = MaskSecret(secret);
    pendingSourceLabel_ = SourceLabel(source);
    mode_ = ProbeMode::Manual;
    done_ = 0;
    total_ = 1;
    state_ = UiState::Probing;
    shouldPersist_ = false;
    vaultPasswordTaken_ = false;
    message_.clear();
  }

  void ScrubPending() {
    PasswordRing::SecureZero(pendingSecret_);
    pendingMasked_.clear();
    pendingSourceLabel_.clear();
  }

  void ScrubSecrets() {
    ScrubPending();
    PasswordRing::SecureZero(openedSecret_);
    for (Candidate& candidate : ranked_) PasswordRing::SecureZero(candidate.text);
    for (std::string& secret : vaultSecrets_) PasswordRing::SecureZero(secret);
  }

  std::string Redact(const std::string& text) const {
    std::vector<std::string> secrets;
    secrets.reserve(ranked_.size() + vaultSecrets_.size() + 2);
    for (const Candidate& candidate : ranked_) secrets.push_back(candidate.text);
    for (const std::string& secret : vaultSecrets_) secrets.push_back(secret);
    if (!pendingSecret_.empty()) secrets.push_back(pendingSecret_);
    if (!openedSecret_.empty()) secrets.push_back(openedSecret_);
    return PasswordRing::RedactSecrets(text, secrets);
  }

  void Publish() {
    view_ = UiView{};
    view_.state = state_;
    view_.showDialog = state_ == UiState::NeedPassword || state_ == UiState::Wrong ||
                       state_ == UiState::Exhausted;
    view_.showProgress = state_ == UiState::Probing;
    view_.done = done_;
    view_.total = total_;
    view_.wrongCount = wrongCount_;
    view_.attemptsUsed = attemptsUsed_;
    view_.attemptsLeft = attemptsLeft();
    view_.budget = budget_;
    view_.canSubmit = CanSubmit();
    view_.exhausted = state_ == UiState::Exhausted;
    view_.opened = state_ == UiState::Opened;
    view_.shouldPersist = shouldPersist_;
    view_.canRemember = canRemember_;
    view_.remember = rememberChoice_;
    view_.showPassword = showPassword_;

    if (state_ == UiState::Probing) {
      view_.progressLabel = "matching " + PasswordRing::ProgressLabel(
                                             static_cast<size_t>(done_),
                                             static_cast<size_t>(total_));
      view_.masked = pendingMasked_;
      view_.sourceLabel = pendingSourceLabel_;
    } else if (state_ == UiState::Opened) {
      view_.masked = MaskSecret(openedSecret_);
      view_.sourceLabel = openedSourceLabel_;
    }

    // The ONE redaction site for everything that can reach a user. Runs last,
    // over the whole message, against every secret the model knows.
    view_.message = Redact(message_);
  }

  std::string archiveName_;
  std::vector<Candidate> ranked_;
  std::vector<CandidateView> candidateViews_;
  std::vector<std::string> vaultSecrets_;
  std::vector<VaultItemView> vaultViews_;
  UiState state_ = UiState::Idle;
  ProbeMode mode_ = ProbeMode::None;
  int budget_ = kManualAttemptBudget;
  int attemptsUsed_ = 0;
  int wrongCount_ = 0;
  int done_ = 0;
  int total_ = 0;
  bool canRemember_ = true;
  bool shouldPersist_ = false;
  bool vaultPasswordTaken_ = false;
  bool showPassword_ = false;
  RememberChoice rememberChoice_ = RememberChoice::Once;
  std::string pendingSecret_;
  std::string pendingMasked_;
  std::string pendingSourceLabel_;
  std::string openedSecret_;
  std::string openedSourceLabel_;
  std::string message_;
  UiView view_;
};

}  // namespace prism::pwd
