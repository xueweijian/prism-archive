// PrismArchive -- PrismCore
// VaultAdminModel: the *policy* half of the password-vault manager (card C56).
//
// C5 shipped the DPAPI store (Src/PrismWin/DpapiVault.hpp) and C35 shipped a
// password dialog that can PICK a stored secret. Neither lets a user SEE what is
// stored, delete one entry, rename it, or wipe the vault. This header is the
// screen's brain: it owns the list, the labels, the rename/delete/clear rules,
// the "clear needs a second confirmation" state machine, and the corruption /
// decryption-failure accounting. It has no HWND, no registry, no clock of its
// own -- every timestamp and hit count arrives from the caller, so the rules are
// proved in milliseconds on every lane (D1: a rule that only ran on Windows
// would be skipped by the Linux lane).
//
// SECRECY (the reason this model is small and suspicious)
//   * The plaintext NEVER leaves this class. The only outward representation of
//     a secret is VaultEntryView::masked: bullets plus the last two UTF-8 code
//     points (the C35 convention: enough to tell two entries apart, not enough
//     to reconstruct one). Two code points or fewer are masked COMPLETELY.
//   * MaskSecret is the ONE mask implementation (CONTRACTS D3). The mutant
//     build returns the plaintext and bumps a counter, so a green mutant run is
//     impossible to mistake for a caught mutation.
//   * The destructor wipes the stored secrets. A vault manager that left them in
//     freed heap memory would undo the reason DPAPI exists.
//
// CORRUPTION / DECRYPTION FAILURES (security requirement of this card)
//   The store reports how many blobs it could not decrypt and whether the file
//   layout itself was unreadable. Those entries are SKIPPED one at a time and
//   COUNTED -- one bad blob must never hide the rest of the vault -- and an
//   unreadable file is a state the screen can render, not an exception.
//
// CONTRACTS
//   B1  every std::min/max below is parenthesized (a consumer TU may include
//       windows.h without NOMINMAX).
//   B2  this header includes <cstdint> itself (uint64_t/uint32_t).
//   B6  the enums are printable (operator<<) because PRISM_CHECK_EQ prints them.
//   D3  two compile-time mutants with a fired counter (see the suite file).
//   D7  whole-file authoring only: nothing here slices or patch-edits a file.
//   D27 the mutants arm their verdict before the first assertion (test side).
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::vaultadmin {

// ---------------------------------------------------------------------------
// Vocabulary
// ---------------------------------------------------------------------------

// What the screen is looking at. Empty and Ready are separate so the dialog can
// say "no stored passwords" instead of an empty list, and Unreadable is a state
// rather than an error return: the window must OPEN and explain itself.
enum class VaultAdminState : std::uint8_t { Empty = 0, Ready, Unreadable };

inline const char* VaultAdminStateName(VaultAdminState state) {
  switch (state) {
    case VaultAdminState::Empty: return "empty";
    case VaultAdminState::Ready: return "ready";
    case VaultAdminState::Unreadable: return "unreadable";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, VaultAdminState state) {
  return os << VaultAdminStateName(state);
}

// How many trailing characters a mask may show (C35 uses the same two).
inline constexpr std::size_t kMaskSuffixChars = 2;
// A label is display metadata, not a note field: 64 code points is a list row.
inline constexpr std::size_t kMaxLabelChars = 64;
// A corrupted length prefix must never make us allocate a gigabyte.
inline constexpr std::size_t kMaxEntryBytes = 16u * 1024u * 1024u;
// More entries than this is a file we refuse to list rather than a UI we block.
inline constexpr std::size_t kMaxEntries = 512;

// ---------------------------------------------------------------------------
// D3 mutation plumbing. Only mutant builds enter these branches; the counter is
// shared through ODR so the test's main() can ask whether the injection really
// fired (a dead mutation must stay RED, not silently pass).
// ---------------------------------------------------------------------------
inline int& VaultAdminMutationCount() {
  static int count = 0;
  return count;
}

inline bool VaultAdminMutationFired() { return VaultAdminMutationCount() > 0; }

inline void NoteVaultAdminMutation() { ++VaultAdminMutationCount(); }

// ---------------------------------------------------------------------------
// UTF-8 helpers. Byte-wise truncation would split a CJK password or label and
// put invalid UTF-8 into a control; counting code points keeps the tail
// readable (the C35 lesson).
// ---------------------------------------------------------------------------
inline std::size_t Utf8CharCount(std::string_view text) {
  std::size_t count = 0;
  for (unsigned char byte : text) {
    if ((byte & 0xC0u) != 0x80u) ++count;  // not a continuation byte
  }
  return count;
}

inline std::string Utf8Suffix(std::string_view text, std::size_t chars) {
  if (chars == 0 || text.empty()) return std::string();
  // Walk back to the START of the chars-th character from the end. Counting
  // non-continuation bytes and returning "one past the next one" (the C35
  // formulation) lands mid-character when the tail is multibyte: for
  // "一二三四" the last two characters are 6 bytes, and cutting at the byte
  // after the third lead byte produces 0xA0..., which is not UTF-8 at all.
  std::size_t start = text.size();
  std::size_t remaining = chars;
  for (std::size_t pos = text.size(); pos > 0;) {
    --pos;
    if ((static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) continue;  // continuation
    start = pos;
    if (--remaining == 0) break;
  }
  if (remaining > 0) return std::string(text);  // fewer characters than asked
  return std::string(text.substr(start));
}

inline std::string Utf8Prefix(std::string_view text, std::size_t chars) {
  if (chars == 0 || text.empty()) return std::string();
  std::size_t seen = 0;
  for (std::size_t pos = 0; pos < text.size(); ++pos) {
    if ((static_cast<unsigned char>(text[pos]) & 0xC0u) != 0x80u) {
      if (seen == chars) return std::string(text.substr(0, pos));
      ++seen;
    }
  }
  return std::string(text);
}

// The ONLY mask implementation. The mutant returns the plaintext and bumps the
// counter; every other path (labels, rows, status lines) is derived from this.
inline std::string MaskSecret(std::string_view secret) {
  if (secret.empty()) return std::string();
#if defined(PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT)
  NoteVaultAdminMutation();
  return std::string(secret);
#else
  const std::string bullet = "\xE2\x80\xA2";  // U+2022 BULLET
  const std::size_t chars = Utf8CharCount(secret);
  const std::size_t visible = chars > kMaskSuffixChars ? kMaskSuffixChars : 0;
  std::string out;
  out.reserve((chars - visible) * bullet.size() + visible * 4);
  for (std::size_t i = 0; i < chars - visible; ++i) out += bullet;
  out += Utf8Suffix(secret, visible);
  return out;
#endif
}

// A label is user input: trim the ends, fold control characters to spaces and
// cap the length in CODE POINTS. Empty means "not a label" and is rejected by
// Rename (never silently stored as an empty row).
inline std::string SanitizeLabel(std::string_view label) {
  std::string cleaned;
  cleaned.reserve(label.size());
  for (const char raw : label) {
    const unsigned char byte = static_cast<unsigned char>(raw);
    if (byte < 0x20u || byte == 0x7Fu) {
      cleaned.push_back(' ');
    } else {
      cleaned.push_back(raw);
    }
  }
  auto isSpace = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  std::size_t begin = 0;
  while (begin < cleaned.size() && isSpace(cleaned[begin])) ++begin;
  std::size_t end = cleaned.size();
  while (end > begin && isSpace(cleaned[end - 1])) --end;
  cleaned = cleaned.substr(begin, end - begin);
  return Utf8Prefix(cleaned, kMaxLabelChars);
}

// ---------------------------------------------------------------------------
// Timestamps. UTC only: a local-time render would make the suite depend on the
// runner's time zone (CONTRACTS D21). 0 is "never".
// ---------------------------------------------------------------------------
inline std::string FormatTimestampUtc(std::uint64_t seconds) {
  if (seconds == 0) return std::string("never");
  const std::int64_t days = static_cast<std::int64_t>(seconds / 86400u);
  const std::int64_t rem = static_cast<std::int64_t>(seconds % 86400u);
  // Howard Hinnant's civil_from_days, integer-only and locale-free.
  const std::int64_t z = days + 719468;
  const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const std::uint32_t doe = static_cast<std::uint32_t>(z - era * 146097);
  const std::uint32_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
  const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
  const std::uint32_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
  const std::uint32_t mp = (5u * doy + 2u) / 153u;
  const std::uint32_t day = doy - (153u * mp + 2u) / 5u + 1u;
  const std::uint32_t month = mp < 10u ? mp + 3u : mp - 9u;
  const std::int64_t civilYear = year + (month <= 2u ? 1 : 0);

  auto two = [](std::int64_t value) {
    std::string out = std::to_string(value);
    if (out.size() < 2) out.insert(out.begin(), '0');
    return out;
  };
  const std::int64_t hours = rem / 3600;
  const std::int64_t minutes = (rem % 3600) / 60;
  const std::int64_t secs = rem % 60;

  std::string out = std::to_string(civilYear);
  out += '-';
  out += two(static_cast<std::int64_t>(month));
  out += '-';
  out += two(static_cast<std::int64_t>(day));
  out += ' ';
  out += two(hours);
  out += ':';
  out += two(minutes);
  out += ':';
  out += two(secs);
  out += 'Z';
  return out;
}

inline std::string HitCountText(std::uint32_t hits) {
  return std::to_string(hits) + (hits == 1 ? " hit" : " hits");
}

// ---------------------------------------------------------------------------
// Entry shapes. Input carries the secret (the model is the only holder); View
// carries the MASK only -- there is no field to accidentally echo.
// ---------------------------------------------------------------------------
struct VaultEntryInput {
  std::uint64_t id = 0;       // opaque, stable for this session; the store's key
  std::string secret;         // never rendered
  std::string label;          // display metadata from the index file
  std::uint64_t addedAt = 0;  // UTC seconds, 0 = unknown
  std::uint64_t lastUsedAt = 0;
  std::uint32_t hitCount = 0;
};

struct VaultEntryView {
  std::uint64_t id = 0;
  std::size_t index = 0;
  std::string label;
  std::string masked;
  std::uint64_t addedAt = 0;
  std::uint64_t lastUsedAt = 0;
  std::uint32_t hitCount = 0;
};

struct VaultLoadReport {
  std::size_t skipped = 0;   // undecryptable blobs the store had to drop
  bool unreadable = false;   // the file layout itself could not be trusted
  std::string detail;        // "truncated entry at byte 37", never a secret
};

// One row as the list shows it. Pure, so the window test's "no plaintext"
// assertion reads exactly what the user reads.
inline std::string FormatEntryRow(const VaultEntryView& view) {
  std::string row = view.masked;
  row += "  |  ";
  row += view.label;
  row += "  |  added ";
  row += FormatTimestampUtc(view.addedAt);
  row += "  |  last used ";
  row += FormatTimestampUtc(view.lastUsedAt);
  row += "  |  ";
  row += HitCountText(view.hitCount);
  return row;
}

// ---------------------------------------------------------------------------
// The label/timestamp sidecar. The DPAPI blob format (C5) is [len][cipher]* and
// carries no metadata -- and it must not change, because C5 owns it. The
// metadata that makes a vault usable (a human label, when it was added, how
// often it matched) therefore lives in a SECRET-FREE sidecar next to it, owned
// by this header so both producers agree on one format.
//
// Format (UTF-8, '\n' terminated):
//   prism-vault-index 1
//   <added>\t<lastUsed>\t<hits>\t<escaped label>
// A line that does not parse is skipped and COUNTED; a header that does not
// match is reported so the caller can say the index was not readable.
// ---------------------------------------------------------------------------
inline constexpr char kVaultIndexHeader[] = "prism-vault-index 1";

inline std::string EscapeLabel(std::string_view label) {
  std::string out;
  out.reserve(label.size());
  for (const char c : label) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

inline std::string UnescapeLabel(std::string_view escaped) {
  std::string out;
  out.reserve(escaped.size());
  for (std::size_t i = 0; i < escaped.size(); ++i) {
    if (escaped[i] != '\\' || i + 1 >= escaped.size()) {
      out.push_back(escaped[i]);
      continue;
    }
    ++i;
    switch (escaped[i]) {
      case 't': out.push_back('\t'); break;
      case 'n': out.push_back('\n'); break;
      case 'r': out.push_back('\r'); break;
      case '\\': out.push_back('\\'); break;
      default: out.push_back(escaped[i]); break;
    }
  }
  return out;
}

struct VaultIndexParse {
  std::vector<VaultEntryInput> entries;  // id = position, secret left empty
  std::size_t skipped = 0;
  bool headerPresent = false;
};

inline std::string SerializeVaultIndex(const std::vector<VaultEntryInput>& entries) {
  std::string out = kVaultIndexHeader;
  out += '\n';
  for (const VaultEntryInput& entry : entries) {
    out += std::to_string(entry.addedAt);
    out += '\t';
    out += std::to_string(entry.lastUsedAt);
    out += '\t';
    out += std::to_string(entry.hitCount);
    out += '\t';
    out += EscapeLabel(SanitizeLabel(entry.label));
    out += '\n';
  }
  return out;
}

inline std::uint64_t ParseU64(std::string_view text, bool* ok) {
  if (text.empty()) {
    *ok = false;
    return 0;
  }
  std::uint64_t value = 0;
  for (const char c : text) {
    if (c < '0' || c > '9') {
      *ok = false;
      return 0;
    }
    const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
    if (value > (UINT64_MAX - digit) / 10u) {
      *ok = false;
      return 0;
    }
    value = value * 10u + digit;
  }
  *ok = true;
  return value;
}

inline VaultIndexParse ParseVaultIndex(std::string_view text) {
  VaultIndexParse result;
  std::size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    const std::size_t end = text.find('\n', pos);
    const std::string_view line =
        end == std::string_view::npos ? text.substr(pos) : text.substr(pos, end - pos);
    pos = end == std::string_view::npos ? text.size() : end + 1;
    if (first) {
      first = false;
      result.headerPresent = line == kVaultIndexHeader;
      if (result.headerPresent) continue;
      // No header: the file is not ours. Nothing below is guessed.
      result.skipped += 1;
      return result;
    }
    if (line.empty()) continue;
    // Three numeric fields then a free-form label.
    const std::size_t tab1 = line.find('\t');
    const std::size_t tab2 = tab1 == std::string_view::npos ? std::string_view::npos
                                                            : line.find('\t', tab1 + 1);
    const std::size_t tab3 = tab2 == std::string_view::npos ? std::string_view::npos
                                                            : line.find('\t', tab2 + 1);
    if (tab1 == std::string_view::npos || tab2 == std::string_view::npos ||
        tab3 == std::string_view::npos) {
      result.skipped += 1;
      continue;
    }
    bool okAdded = false;
    bool okUsed = false;
    bool okHits = false;
    const std::uint64_t added = ParseU64(line.substr(0, tab1), &okAdded);
    const std::uint64_t used = ParseU64(line.substr(tab1 + 1, tab2 - tab1 - 1), &okUsed);
    const std::uint64_t hits = ParseU64(line.substr(tab2 + 1, tab3 - tab2 - 1), &okHits);
    if (!okAdded || !okUsed || !okHits || hits > 0xFFFFFFFFu) {
      result.skipped += 1;
      continue;
    }
    VaultEntryInput entry;
    entry.id = result.entries.size();
    entry.addedAt = added;
    entry.lastUsedAt = used;
    entry.hitCount = static_cast<std::uint32_t>(hits);
    entry.label = SanitizeLabel(UnescapeLabel(line.substr(tab3 + 1)));
    result.entries.push_back(std::move(entry));
  }
  return result;
}

inline std::string DefaultLabelFor(std::size_t index) {
  return "Credential " + std::to_string(index + 1);
}

// ---------------------------------------------------------------------------
// The model
// ---------------------------------------------------------------------------
class VaultAdminModel {
 public:
  VaultAdminModel() = default;
  ~VaultAdminModel() { Wipe(); }
  VaultAdminModel(const VaultAdminModel&) = delete;
  VaultAdminModel& operator=(const VaultAdminModel&) = delete;
  // Movable so a helper can return a loaded model by value. The moved-from
  // object is emptied explicitly -- its destructor wipes only what it still
  // owns, so a move can never zero a buffer the new owner is using.
  VaultAdminModel(VaultAdminModel&& other) noexcept { MoveFrom(other); }
  VaultAdminModel& operator=(VaultAdminModel&& other) noexcept {
    if (this != &other) {
      Wipe();
      entries_.clear();
      views_.clear();
      MoveFrom(other);
    }
    return *this;
  }

  // Replaces the whole list. Duplicate or empty ids, empty secrets and entries
  // beyond kMaxEntries are BAD ENTRIES: skipped one at a time and counted
  // (requirement: one corrupt row must not hide the rest of the vault).
  void Load(std::vector<VaultEntryInput> entries, VaultLoadReport report = {}) {
    Wipe();
    entries_.clear();
    views_.clear();
    selected_ = -1;
    clearPending_ = false;
    dirty_ = false;
    lastError_.clear();
    skipped_ = report.skipped;
    unreadable_ = report.unreadable;
    detail_ = std::move(report.detail);
    std::vector<std::uint64_t> seen;
    for (VaultEntryInput& entry : entries) {
      if (entry.secret.empty() || entry.id == 0 ||
          std::find(seen.begin(), seen.end(), entry.id) != seen.end()) {
        ++skipped_;
        continue;
      }
      if (entries_.size() >= kMaxEntries) {
        ++skipped_;
        continue;
      }
      seen.push_back(entry.id);
      entry.label = entry.label.empty() ? DefaultLabelFor(entries_.size())
                                        : SanitizeLabel(entry.label);
      entries_.push_back(std::move(entry));
    }
    RebuildViews();
  }

  void LoadUnreadable(std::string detail) {
    VaultLoadReport report;
    report.unreadable = true;
    report.detail = std::move(detail);
    Load({}, std::move(report));
  }

  std::size_t size() const { return views_.size(); }
  bool empty() const { return views_.empty(); }

  VaultAdminState state() const {
    if (unreadable_) return VaultAdminState::Unreadable;
    return views_.empty() ? VaultAdminState::Empty : VaultAdminState::Ready;
  }

  const std::vector<VaultEntryView>& views() const { return views_; }
  const VaultEntryView* viewAt(std::size_t index) const {
    return index < views_.size() ? &views_[index] : nullptr;
  }
  const VaultEntryView* FindView(std::uint64_t id) const {
    for (const VaultEntryView& view : views_) {
      if (view.id == id) return &view;
    }
    return nullptr;
  }

  int selected() const { return selected_; }
  bool Select(int index) {
    if (index < 0 || static_cast<std::size_t>(index) >= views_.size()) {
      selected_ = -1;
      return false;
    }
    selected_ = index;
    return true;
  }
  void ClearSelection() { selected_ = -1; }
  bool SelectNext(int delta) {
    if (views_.empty()) return false;
    int next = selected_ + delta;
    if (next < 0) next = static_cast<int>(views_.size()) - 1;
    if (next >= static_cast<int>(views_.size())) next = 0;
    selected_ = next;
    return true;
  }

  // Deletion by stable id (the store's key), plus the index/selection forms the
  // listbox drives. Returns false for an unknown id -- never a silent no-op
  // that the caller could mistake for "deleted and saved".
  bool RemoveById(std::uint64_t id) {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i].id != id) continue;
      entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
      // Repair the selection BEFORE rebuilding: RebuildViews clamps an
      // out-of-range index, and clamping a value that is about to be shifted
      // would shift it twice (2 -> 1 -> 0 in the C56 unit run).
      if (selected_ == static_cast<int>(i)) {
        selected_ = -1;
      } else if (selected_ > static_cast<int>(i)) {
        --selected_;
      }
      RebuildViews();
      dirty_ = true;
      lastError_.clear();
      return true;
    }
    lastError_ = "the entry no longer exists";
    return false;
  }

  bool RemoveSelected() {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= views_.size()) {
      lastError_ = "select an entry first";
      return false;
    }
    return RemoveById(views_[static_cast<std::size_t>(selected_)].id);
  }

  // Rename. Empty (after trim) is rejected: a blank row is not a name. An
  // unchanged label is a successful no-op and does NOT mark the model dirty.
  bool RenameById(std::uint64_t id, std::string_view label) {
    const std::string cleaned = SanitizeLabel(label);
    if (cleaned.empty()) {
      lastError_ = "a label cannot be empty";
      return false;
    }
    for (VaultEntryInput& entry : entries_) {
      if (entry.id != id) continue;
      if (entry.label == cleaned) {
        lastError_.clear();
        return true;
      }
      entry.label = cleaned;
      RebuildViews();
      dirty_ = true;
      lastError_.clear();
      return true;
    }
    lastError_ = "the entry no longer exists";
    return false;
  }

  bool RenameByIndex(std::size_t index, std::string_view label) {
    if (index >= views_.size()) {
      lastError_ = "the entry no longer exists";
      return false;
    }
    return RenameById(views_[index].id, label);
  }

  bool RenameSelected(std::string_view label) {
    if (selected_ < 0 || static_cast<std::size_t>(selected_) >= views_.size()) {
      lastError_ = "select an entry first";
      return false;
    }
    return RenameById(views_[static_cast<std::size_t>(selected_)].id, label);
  }

  // Clear is a TWO-STEP operation: RequestClear only arms it, and nothing is
  // destroyed until ConfirmClear. The mutant removes the confirmation entirely
  // so the "still there after the first click" assertion goes red.
  void RequestClear() {
    lastError_.clear();
#if defined(PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM)
    NoteVaultAdminMutation();
    ClearNow();
#else
    if (views_.empty()) {
      clearPending_ = false;
      lastError_ = "the vault is already empty";
      return;
    }
    clearPending_ = true;
#endif
  }

  bool ClearPending() const { return clearPending_; }

  bool ConfirmClear() {
    if (!clearPending_) {
      lastError_ = "there is nothing to confirm";
      return false;
    }
    ClearNow();
    return true;
  }

  void CancelClear() {
    clearPending_ = false;
    lastError_.clear();
  }

  bool dirty() const { return dirty_; }
  void ClearDirty() { dirty_ = false; }

  std::size_t skippedCount() const { return skipped_; }
  const std::string& detail() const { return detail_; }
  const std::string& lastError() const { return lastError_; }

  // Hit bookkeeping for the wiring patch (the engine reports a successful
  // match). Keeping it here means the counter has ONE owner.
  bool NoteUse(std::uint64_t id, std::uint64_t when) {
    for (VaultEntryInput& entry : entries_) {
      if (entry.id != id) continue;
      entry.lastUsedAt = when;
      if (entry.hitCount < 0xFFFFFFFFu) ++entry.hitCount;
      RebuildViews();
      dirty_ = true;
      return true;
    }
    return false;
  }

  std::string StatusLine() const {
    if (unreadable_) {
      return "Vault file is unreadable" +
             (detail_.empty() ? std::string(" (the layout could not be parsed)")
                              : std::string(" (") + detail_ + ")") +
             " - showing " + std::to_string(views_.size()) + " recovered entr" +
             (views_.size() == 1 ? "y" : "ies");
    }
    if (views_.empty()) return "No stored credentials";
    return std::to_string(views_.size()) + " stored credential" +
           (views_.size() == 1 ? "" : "s");
  }

  std::string HintLine() const {
    if (clearPending_) {
      return "Click Clear again to erase all " + std::to_string(views_.size()) +
             " credentials (this cannot be undone)";
    }
    if (!lastError_.empty()) return lastError_;
    if (skipped_ > 0) {
      return std::to_string(skipped_) + " entr" + (skipped_ == 1 ? "y was" : "ies were") +
             " skipped (could not be decrypted)";
    }
    return std::string();
  }

  // The sidecar document for the current list (secret-free by construction).
  std::vector<VaultEntryInput> MetadataSnapshot() const {
    std::vector<VaultEntryInput> out;
    out.reserve(entries_.size());
    for (const VaultEntryInput& entry : entries_) {
      VaultEntryInput meta;
      meta.id = entry.id;
      meta.label = entry.label;
      meta.addedAt = entry.addedAt;
      meta.lastUsedAt = entry.lastUsedAt;
      meta.hitCount = entry.hitCount;
      out.push_back(std::move(meta));
    }
    return out;
  }

 private:
  void MoveFrom(VaultAdminModel& other) noexcept {
    entries_ = std::move(other.entries_);
    views_ = std::move(other.views_);
    selected_ = other.selected_;
    clearPending_ = other.clearPending_;
    dirty_ = other.dirty_;
    unreadable_ = other.unreadable_;
    skipped_ = other.skipped_;
    detail_ = std::move(other.detail_);
    lastError_ = std::move(other.lastError_);
    other.entries_.clear();
    other.views_.clear();
    other.selected_ = -1;
    other.clearPending_ = false;
    other.dirty_ = false;
    other.unreadable_ = false;
    other.skipped_ = 0;
    other.detail_.clear();
    other.lastError_.clear();
  }

  void ClearNow() {
    entries_.clear();
    views_.clear();
    selected_ = -1;
    clearPending_ = false;
    dirty_ = true;
    lastError_.clear();
  }

  void RebuildViews() {
    views_.clear();
    views_.reserve(entries_.size());
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const VaultEntryInput& entry = entries_[i];
      VaultEntryView view;
      view.id = entry.id;
      view.index = i;
      view.label = entry.label;
      view.masked = MaskSecret(entry.secret);
      view.addedAt = entry.addedAt;
      view.lastUsedAt = entry.lastUsedAt;
      view.hitCount = entry.hitCount;
      views_.push_back(std::move(view));
    }
    if (selected_ >= static_cast<int>(views_.size())) {
      selected_ = views_.empty() ? -1 : static_cast<int>(views_.size()) - 1;
    }
  }

  void Wipe() {
    for (VaultEntryInput& entry : entries_) {
      volatile char* data = entry.secret.empty() ? nullptr : &entry.secret[0];
      for (std::size_t i = 0; data != nullptr && i < entry.secret.size(); ++i) data[i] = 0;
      entry.secret.clear();
    }
  }

  std::vector<VaultEntryInput> entries_;
  std::vector<VaultEntryView> views_;
  int selected_ = -1;
  bool clearPending_ = false;
  bool dirty_ = false;
  bool unreadable_ = false;
  std::size_t skipped_ = 0;
  std::string detail_;
  std::string lastError_;
};

}  // namespace prism::vaultadmin
