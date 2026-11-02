// PrismArchive -- the vault manager's decision model (card C56).
//
// Headless: no windows.h, no DPAPI, no clock. Everything the Win32 dialog is NOT
// allowed to re-derive is proved here in milliseconds on every lane (CONTRACTS
// D1: a rule that only runs on Windows is skipped by the Linux lane).
//
// What is asserted, in the order the screen runs:
//   * the mask: bullets + the last two CODE POINTS, full mask for <= 2, and a
//     content-level absence check for the whole plaintext (D2) -- the view has
//     no field that could carry it;
//   * the row: label + mask + added-at + last-used + hits, all UTC so the suite
//     does not depend on the runner's time zone (D21);
//   * delete by stable id: the list shrinks by exactly one, the surviving ORDER
//     is kept, the selection is repaired and the model is marked dirty;
//   * rename: trim, control-character folding, code-point cap (a CJK label must
//     stay valid UTF-8), empty is refused, an unchanged label is a no-op;
//   * clear is TWO-STEP: nothing is destroyed until the second confirmation, and
//     cancelling leaves the list untouched;
//   * bad rows are skipped ONE AT A TIME and counted (empty secret, duplicate
//     id, over the entry cap), and an unreadable file is a RENDERABLE state;
//   * the sidecar document round-trips escaped labels and skips garbage lines.
//
// Negative controls (CONTRACTS D3, registered in Tests/suites/c56-vaultui.cmake):
//   PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT     MaskSecret returns the plaintext.
//   PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM   RequestClear erases immediately.
//   PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT -> the "view never contains the secret"
//     checks go red.
//   PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM -> the "still there after the first
//     click" check goes red.
// main() arms "C56 VAULT-UI MUTANT CAUGHT" BEFORE the first assertion (D27) with
// the injection predicate, so a red run for an UNRELATED reason never claims
// CAUGHT (D3) and teardown cannot swallow the verdict.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "../Src/PrismCore/VaultAdminModel.hpp"
#include "mini_test.hpp"

namespace {

using prism::vaultadmin::DefaultLabelFor;
using prism::vaultadmin::EscapeLabel;
using prism::vaultadmin::FormatEntryRow;
using prism::vaultadmin::FormatTimestampUtc;
using prism::vaultadmin::kMaskSuffixChars;
using prism::vaultadmin::MaskSecret;
using prism::vaultadmin::ParseVaultIndex;
using prism::vaultadmin::SanitizeLabel;
using prism::vaultadmin::SerializeVaultIndex;
using prism::vaultadmin::UnescapeLabel;
using prism::vaultadmin::VaultAdminModel;
using prism::vaultadmin::VaultAdminState;
using prism::vaultadmin::VaultEntryInput;
using prism::vaultadmin::VaultLoadReport;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string Bullets(std::size_t count) {
  std::string out;
  for (std::size_t i = 0; i < count; ++i) out += "\xE2\x80\xA2";
  return out;
}

// Content-level leak check (CONTRACTS D2/D5): the full secret OR any fragment of
// FOUR OR MORE CODE POINTS is a leak. Counting code points (not bytes) keeps the
// documented two-character preview legal: for a CJK secret the last two
// characters are six bytes, and a byte-wise scan would call the mask itself a
// leak. For ASCII secrets this is exactly the C35 rule.
bool Leaks(const std::string& haystack, const std::string& secret) {
  if (secret.empty()) return false;
  if (Contains(haystack, secret)) return true;
  std::vector<std::size_t> starts;
  starts.push_back(0);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    if ((static_cast<unsigned char>(secret[i]) & 0xC0u) != 0x80u && i != 0) starts.push_back(i);
  }
  starts.push_back(secret.size());
  const std::size_t chars = starts.size() - 1;
  if (chars < 4) return false;
  for (std::size_t i = 0; i + 4 <= chars; ++i) {
    for (std::size_t j = i + 4; j <= chars; ++j) {
      if (Contains(haystack, secret.substr(starts[i], starts[j] - starts[i]))) return true;
    }
  }
  return false;
}

VaultEntryInput Entry(std::uint64_t id, const std::string& secret, const std::string& label,
                      std::uint64_t added = 0, std::uint64_t used = 0,
                      std::uint32_t hits = 0) {
  VaultEntryInput entry;
  entry.id = id;
  entry.secret = secret;
  entry.label = label;
  entry.addedAt = added;
  entry.lastUsedAt = used;
  entry.hitCount = hits;
  return entry;
}

VaultAdminModel Loaded(const std::vector<VaultEntryInput>& entries,
                       VaultLoadReport report = {}) {
  VaultAdminModel model;
  model.Load(entries, report);
  return model;
}

// The three sample secrets deliberately differ in length and character set: a
// byte-wise mask would split the CJK one and a suffix rule that is off by one
// would show three characters of the 12-character one.
const char* kSecretA = "hunter2SuperSecret";   // 17 ASCII
const char* kSecretB = "zip-2024-&!pass";      // 15 ASCII
const char* kSecretC = "\xE5\xAF\x86\xE7\xA0\x81\xE4\xB8\x80\xE4\xBA\x8C";  // 4 CJK chars

std::vector<VaultEntryInput> ThreeEntries() {
  return {Entry(1, kSecretA, "Home NAS", 1700000000ull, 1700000100ull, 3),
          Entry(2, kSecretB, "Work share", 1700000200ull, 0, 0),
          Entry(3, kSecretC, "Family photos", 1700000300ull, 1700000400ull, 12)};
}

// ===========================================================================
// 1. The mask -- the ONLY outward representation of a secret
// ===========================================================================

PRISM_TEST("mask: bullets plus the last two code points, never the plaintext") {
  // Explicitly pin the constants the dialog and the row formatter share.
  PRISM_CHECK_EQ(kMaskSuffixChars, static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(MaskSecret("abcdef"), Bullets(4) + "ef");
  PRISM_CHECK_EQ(MaskSecret("abc"), Bullets(1) + "bc");
  PRISM_CHECK_EQ(MaskSecret("ab"), Bullets(2));   // <= 2: fully masked
  PRISM_CHECK_EQ(MaskSecret("a"), Bullets(1));
  PRISM_CHECK_EQ(MaskSecret(""), std::string());
  // CJK: four code points, so two bullets and the last two characters -- not a
  // byte-wise split that would emit invalid UTF-8.
  PRISM_CHECK_EQ(MaskSecret(kSecretC), Bullets(2) + "\xE4\xB8\x80\xE4\xBA\x8C");
  PRISM_CHECK(!Leaks(MaskSecret(kSecretA), kSecretA));
  PRISM_CHECK(!Leaks(MaskSecret(kSecretB), kSecretB));
  PRISM_CHECK(!Leaks(MaskSecret(kSecretC), kSecretC));
}

PRISM_TEST("view: no field carries the plaintext, and the row is masked") {
  VaultAdminModel model = Loaded(ThreeEntries());
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(model.state(), VaultAdminState::Ready);
  const VaultEntryInput secrets[] = {Entry(1, kSecretA, ""), Entry(2, kSecretB, ""),
                                     Entry(3, kSecretC, "")};
  for (std::size_t i = 0; i < model.views().size(); ++i) {
    const prism::vaultadmin::VaultEntryView& view = model.views()[i];
    PRISM_CHECK(!Leaks(view.label, secrets[i].secret));
    PRISM_CHECK(!Leaks(view.masked, secrets[i].secret));
    const std::string row = FormatEntryRow(view);
    PRISM_CHECK(!Leaks(row, secrets[i].secret));
    // The row is CONTENT, not "something appeared": mask, label, timestamps and
    // the hit counter must all be in it (D2).
    PRISM_CHECK(Contains(row, view.masked));
    PRISM_CHECK(Contains(row, view.label));
    PRISM_CHECK(Contains(row, "added "));
    PRISM_CHECK(Contains(row, "last used "));
    PRISM_CHECK(Contains(row, " hit"));
  }
}

PRISM_TEST("row: UTC timestamps and the hit counter") {
  PRISM_CHECK_EQ(FormatTimestampUtc(0), std::string("never"));
  PRISM_CHECK_EQ(FormatTimestampUtc(86400ull), std::string("1970-01-02 00:00:00Z"));
  PRISM_CHECK_EQ(FormatTimestampUtc(1700000000ull), std::string("2023-11-14 22:13:20Z"));
  PRISM_CHECK_EQ(FormatTimestampUtc(1735689600ull), std::string("2025-01-01 00:00:00Z"));
  // A leap day proves the civil-date arithmetic, not just the epoch.
  PRISM_CHECK_EQ(FormatTimestampUtc(951782400ull), std::string("2000-02-29 00:00:00Z"));

  VaultAdminModel model = Loaded(ThreeEntries());
  const std::string first = FormatEntryRow(model.views()[0]);
  PRISM_CHECK(Contains(first, "Home NAS"));
  PRISM_CHECK(Contains(first, "added 2023-11-14 22:13:20Z"));
  PRISM_CHECK(Contains(first, "last used 2023-11-14 22:15:00Z"));
  PRISM_CHECK(Contains(first, "3 hits"));
  const std::string second = FormatEntryRow(model.views()[1]);
  PRISM_CHECK(Contains(second, "last used never"));
  PRISM_CHECK(Contains(second, "0 hits"));
}

// ===========================================================================
// 2. Delete
// ===========================================================================

PRISM_TEST("delete: exactly one row goes, the order survives, the selection repairs") {
  VaultAdminModel model = Loaded(ThreeEntries());
  PRISM_CHECK(model.Select(1));
  PRISM_CHECK_EQ(model.selected(), 1);
  const std::uint64_t secondId = model.views()[1].id;
  PRISM_CHECK(model.RemoveById(secondId));
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(model.selected(), -1);  // the deleted row was the selected one
  PRISM_CHECK_EQ(model.views()[0].label, std::string("Home NAS"));
  PRISM_CHECK_EQ(model.views()[1].label, std::string("Family photos"));
  PRISM_CHECK(model.dirty());
  // Deleting something that is not there is a REPORTED failure, not a no-op.
  PRISM_CHECK(!model.RemoveById(secondId));
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(2));
  PRISM_CHECK(Contains(model.lastError(), "no longer exists"));

  // Deleting ABOVE the selection shifts it down instead of losing it.
  VaultAdminModel other = Loaded(ThreeEntries());
  PRISM_CHECK(other.Select(2));
  PRISM_CHECK(other.RemoveById(other.views()[0].id));
  PRISM_CHECK_EQ(other.selected(), 1);
  PRISM_CHECK_EQ(other.views()[other.selected()].label, std::string("Family photos"));

  // Deleting the selected row clears the selection (nothing is silently
  // selected on the user's behalf), so the next delete needs a new pick. The
  // last deletion empties the vault: Empty, not a phantom Ready.
  PRISM_CHECK(other.RemoveSelected());
  PRISM_CHECK_EQ(other.selected(), -1);
  PRISM_CHECK(other.Select(0));
  PRISM_CHECK(other.RemoveSelected());
  PRISM_CHECK_EQ(other.size(), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(other.state(), VaultAdminState::Empty);
}

// ===========================================================================
// 3. Rename
// ===========================================================================

PRISM_TEST("rename: trimmed, control-folded, code-point capped, empty refused") {
  VaultAdminModel model = Loaded(ThreeEntries());
  PRISM_CHECK_EQ(SanitizeLabel("  spaced  "), std::string("spaced"));
  PRISM_CHECK_EQ(SanitizeLabel("tab\there"), std::string("tab here"));
  // The string is split on purpose: "byte" is ONE hex escape (0x1B) in C++,
  // not a control byte followed by "byte".
  PRISM_CHECK_EQ(SanitizeLabel("nul\x01" "byte"), std::string("nul byte"));
  PRISM_CHECK_EQ(SanitizeLabel("   "), std::string());

  PRISM_CHECK(model.Select(0));
  PRISM_CHECK(model.RenameSelected("  Living room NAS  "));
  PRISM_CHECK_EQ(model.views()[0].label, std::string("Living room NAS"));
  PRISM_CHECK(model.dirty());
  PRISM_CHECK(Contains(FormatEntryRow(model.views()[0]), "Living room NAS"));

  // Empty is refused and the old label stays.
  PRISM_CHECK(!model.RenameSelected(" \t "));
  PRISM_CHECK_EQ(model.views()[0].label, std::string("Living room NAS"));
  PRISM_CHECK(Contains(model.lastError(), "empty"));

  // An unchanged label is a successful no-op, not an error.
  PRISM_CHECK(model.RenameSelected("Living room NAS"));
  PRISM_CHECK(model.lastError().empty());

  // The cap counts CODE POINTS: 80 CJK characters must truncate to 64 valid
  // characters, never to a broken byte sequence.
  const std::string cjk = "\xE5\xAF\x86";  // one 3-byte character
  std::string longLabel;
  for (int i = 0; i < 80; ++i) longLabel += cjk;
  PRISM_CHECK(model.RenameSelected(longLabel));
  PRISM_CHECK_EQ(prism::vaultadmin::Utf8CharCount(model.views()[0].label),
                 static_cast<std::size_t>(64));
  PRISM_CHECK_EQ(model.views()[0].label.size(), static_cast<std::size_t>(192));
}

// ===========================================================================
// 4. Clear -- two steps
// ===========================================================================

PRISM_TEST("clear: the first click only arms, the second one erases") {
  VaultAdminModel model = Loaded(ThreeEntries());
  model.RequestClear();
  PRISM_CHECK(model.ClearPending());
  // The whole point of the confirm: nothing is gone after the first click.
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(3));
  PRISM_CHECK(Contains(model.HintLine(), "Click Clear again"));
  PRISM_CHECK(!model.dirty());

  model.CancelClear();
  PRISM_CHECK(!model.ClearPending());
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(3));

  model.RequestClear();
  PRISM_CHECK(model.ConfirmClear());
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(model.state(), VaultAdminState::Empty);
  PRISM_CHECK(!model.ClearPending());
  PRISM_CHECK(model.dirty());

  // Confirming twice is refused instead of "succeeding" on an empty vault.
  PRISM_CHECK(!model.ConfirmClear());
  PRISM_CHECK(Contains(model.lastError(), "nothing to confirm"));

  VaultAdminModel empty = Loaded({});
  empty.RequestClear();
  PRISM_CHECK(!empty.ClearPending());
  PRISM_CHECK(Contains(empty.lastError(), "already empty"));
}

// ===========================================================================
// 5. Corruption / decryption failures
// ===========================================================================

PRISM_TEST("bad rows are skipped one at a time and counted") {
  VaultLoadReport report;
  report.skipped = 2;  // what the store could not decrypt
  report.detail = "truncated entry at byte 41";
  std::vector<VaultEntryInput> entries = ThreeEntries();
  entries.push_back(Entry(4, "", "no secret"));          // bad row
  entries.push_back(Entry(1, "dup-id", "duplicate id")); // duplicate key
  VaultAdminModel model = Loaded(entries, report);
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(model.skippedCount(), static_cast<std::size_t>(4));
  PRISM_CHECK_EQ(model.state(), VaultAdminState::Ready);
  PRISM_CHECK(Contains(model.HintLine(), "4 entries were skipped"));
  PRISM_CHECK(!Contains(model.HintLine(), "no secret"));  // the detail never echoes a secret
}

PRISM_TEST("an unreadable vault is a state the screen can render") {
  VaultAdminModel model;
  model.LoadUnreadable("trailing bytes at offset 37");
  PRISM_CHECK_EQ(model.state(), VaultAdminState::Unreadable);
  PRISM_CHECK_EQ(model.size(), static_cast<std::size_t>(0));
  PRISM_CHECK(Contains(model.StatusLine(), "unreadable"));
  PRISM_CHECK(Contains(model.StatusLine(), "trailing bytes at offset 37"));
  // The window disables every destructive button on this state; the model is the
  // authority for that decision (the dialog only reads state()).
  PRISM_CHECK_EQ(model.state(), VaultAdminState::Unreadable);
}

PRISM_TEST("the entry cap is a skip counter, not an unbounded list") {
  std::vector<VaultEntryInput> entries;
  for (std::size_t i = 0; i < prism::vaultadmin::kMaxEntries + 5; ++i) {
    entries.push_back(Entry(static_cast<std::uint64_t>(i) + 1, "secret-" + std::to_string(i),
                            "row " + std::to_string(i)));
  }
  VaultAdminModel model = Loaded(entries);
  PRISM_CHECK_EQ(model.size(), prism::vaultadmin::kMaxEntries);
  PRISM_CHECK_EQ(model.skippedCount(), static_cast<std::size_t>(5));
}

// ===========================================================================
// 6. The label/timestamp sidecar
// ===========================================================================

PRISM_TEST("index: escaped labels round-trip and garbage lines are skipped") {
  PRISM_CHECK_EQ(EscapeLabel("a\tb"), std::string("a\\tb"));
  PRISM_CHECK_EQ(EscapeLabel("a\\b"), std::string("a\\\\b"));
  PRISM_CHECK_EQ(EscapeLabel("a\nb"), std::string("a\\nb"));
  PRISM_CHECK_EQ(UnescapeLabel("a\\tb"), std::string("a\tb"));
  PRISM_CHECK_EQ(UnescapeLabel("a\\\\b"), std::string("a\\b"));
  PRISM_CHECK_EQ(UnescapeLabel("trailing\\"), std::string("trailing\\"));

  std::vector<VaultEntryInput> entries = ThreeEntries();
  entries[0].label = "home NAS \\ living room";  // a backslash must survive
  const std::string document = SerializeVaultIndex(entries);
  PRISM_CHECK(Contains(document, "prism-vault-index 1"));
  PRISM_CHECK(Contains(document, "home NAS \\\\ living room"));  // escaped on disk
  PRISM_CHECK(!Contains(document, "hunter2SuperSecret"));  // secret-free by construction

  const prism::vaultadmin::VaultIndexParse parsed = ParseVaultIndex(document);
  PRISM_CHECK(parsed.headerPresent);
  PRISM_CHECK_EQ(parsed.entries.size(), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(parsed.skipped, static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(parsed.entries[0].label, std::string("home NAS \\ living room"));
  PRISM_CHECK_EQ(parsed.entries[0].addedAt, 1700000000ull);
  PRISM_CHECK_EQ(parsed.entries[2].hitCount, static_cast<std::uint32_t>(12));

  // Garbage: one line with too few fields, one with a non-numeric field, and a
  // missing header must not be guessed at.
  const std::string broken = "prism-vault-index 1\nonly-two\tfields\n1\t2\tx\tlabel\n";
  const prism::vaultadmin::VaultIndexParse brokenParse = ParseVaultIndex(broken);
  PRISM_CHECK_EQ(brokenParse.entries.size(), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(brokenParse.skipped, static_cast<std::size_t>(2));

  const prism::vaultadmin::VaultIndexParse foreign = ParseVaultIndex("not our file\n");
  PRISM_CHECK(!foreign.headerPresent);
  PRISM_CHECK_EQ(foreign.skipped, static_cast<std::size_t>(1));
  PRISM_CHECK(ParseVaultIndex("").entries.empty());
}

PRISM_TEST("defaults: an unlabelled entry still gets a readable row") {
  VaultAdminModel model = Loaded({Entry(7, kSecretA, "")});
  PRISM_CHECK_EQ(model.views()[0].label, DefaultLabelFor(0));
  PRISM_CHECK_EQ(model.views()[0].label, std::string("Credential 1"));
}

PRISM_TEST("note-use bumps the hit counter and the author-supplied label survives") {
  VaultAdminModel model = Loaded(ThreeEntries());
  PRISM_CHECK(model.NoteUse(model.views()[0].id, 1700009999ull));
  PRISM_CHECK_EQ(model.views()[0].hitCount, static_cast<std::uint32_t>(4));
  PRISM_CHECK_EQ(model.views()[0].lastUsedAt, 1700009999ull);
  PRISM_CHECK(Contains(FormatEntryRow(model.views()[0]), "4 hits"));
  PRISM_CHECK(model.dirty());
  PRISM_CHECK_EQ(model.MetadataSnapshot()[0].label, std::string("Home NAS"));
}

}  // namespace

int main(int argc, char** argv) {
#if defined(PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT)
  std::printf("C56 VAULT-UI MUTANT ACTIVE: mask returns plaintext\n");
#elif defined(PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM)
  std::printf("C56 VAULT-UI MUTANT ACTIVE: clear skips the confirmation\n");
#endif

  const int failures = prism::test::RunAll(argc, argv);

#if defined(PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT) || \
    defined(PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM)
  // D26: the core mutants are registered WILL_FAIL, so the verdict must be the
  // ordinary NON-ZERO exit -- arming a marker verdict here would exit 0 and turn
  // WILL_FAIL into a permanent red. D3 asks for the injection to be PROVED, so
  // the banner above and this counter are printed either way; a dead injection
  // leaves the suite green and exits 0, which WILL_FAIL reports as the alarm.
  std::printf("mutation build: %d checks, %d failures, %d injected defects fired\n",
              prism::test::CheckCount(), failures,
              prism::vaultadmin::VaultAdminMutationCount());
  return failures == 0 ? 0 : 1;
#else
  if (failures != 0) return 1;
  std::puts("ALL VAULT-ADMIN CHECKS PASSED");
  return 0;
#endif
}