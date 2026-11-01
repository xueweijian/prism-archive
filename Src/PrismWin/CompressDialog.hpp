// PrismArchive -- PrismWin
// CompressDialog: the "create an archive" dialog -- the UI half of blueprint
// ch.2 (C9 shipped the DECISION half, CompressionPlan.hpp; this card consumes
// it and never re-derives a rule the plan already owns).
//
// WHY THIS FILE HAS NO .rc RESOURCE: the dialog is built at runtime with
// CreateWindowExW + child controls. A dialog template (and therefore a resource
// script) would put the controls in a file the Linux lane cannot read, in a
// format only the Windows resource compiler understands, and would force a
// CMake change in a file this card does not own (Src/PrismWin/CMakeLists.txt is
// card C3's). Programmatic creation also makes the control set inspectable from
// the test: every assertion below targets a real HWND, not a template string.
//
// TWO HALVES, ONE HEADER, ON PURPOSE:
//
//   1. DialogState -- pure C++20. No windows.h, no HWND, no I/O, no 7-Zip
//      headers. Every rule the dialog enforces lives here: the control-value to
//      CompressionPlan mapping (both directions), which combinations are
//      illegal and what happens when they meet (soft downgrade vs hard refusal),
//      the unit conversions the UI prints, the defaults, and whether "OK" may be
//      pressed. That is what makes the Linux lane able to prove the whole rule
//      set in milliseconds (Tests/win/test_compress_dialog.cpp is built TWICE:
//      once headless as core.test_compress_dialog, once against a real HWND as
//      win.compress_dialog).
//
//   2. CompressDialog -- the Win32 shell. It owns the HWND, creates the child
//      controls and translates WM_COMMAND/WM_NOTIFY/WM_HSCROLL into calls on
//      DialogState. It contains NO rules: if a message handler has an "if" that
//      decides what is legal, that "if" belongs in DialogState.
//
// THE TWO KINDS OF "ILLEGAL COMBINATION" ARE NOT THE SAME THING:
//
//   * SOFT (downgrade + tell the user): zip/tar + solid. Neither container has
//     a solid block; CompressionPlan resolves it to false and only emits a
//     warning for an explicit SolidMode::On. A silent downgrade would leave a
//     checked box promising something the archive will not have, so the dialog
//     turns the control OFF and prints why (Adjustment).
//   * HARD (refuse + tell the user): tar + password, non-7z + header
//     encryption, header encryption without a password, unreadable thread count.
//     These are security- or correctness-relevant: answering "ok" would hand
//     the user an unencrypted archive they asked to encrypt. CompressionPlan
//     refuses them too (MakeCompressionPlan returns ok=false); the dialog
//     refuses EARLIER so the reason appears next to the control instead of as a
//     create-time failure.
//
// COST MODEL IS C9's, NOT A NEW ONE (docs/ADR.md ADR-002, CompressionPlan.hpp
// CostModel): creating an archive is exactly one pass over the input, progress
// must come from the engine's byte counter, and for stored data (level 0 / tar)
// the input size is a hard lower bound while for compressed data there is no
// honest lower bound at all. The estimate line therefore prints an UPPER bound
// for size and time plus the lower bound only when one exists -- it never
// promises a compression ratio.
//
// Layer rule (HANDOVER section 4): the pure half has no windows.h, so it compiles
// in the Linux lane; the Win32 half is behind #if defined(_WIN32).
#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/CompressionPlan.hpp"

#if defined(_WIN32)
// The Win32 half of this header. These includes sit at FILE scope, never inside
// the namespace: including <windows.h> inside a namespace wraps every SDK
// declaration into it, which compiles until the first type is used from outside.
#include <windows.h>

#include <commctrl.h>

#include <functional>
#endif

namespace prism::win::cd {

// ---------------------------------------------------------------------------
// Control ids. They live in the pure half so the headless test and the HWND test
// name the same control; ids::kOk is IDOK's value (1) and ids::kCancel is
// IDCANCEL's (2), so a C25 harness can forward a real dialog message unchanged.
// ---------------------------------------------------------------------------
namespace ids {
constexpr int kFormat = 1001;
constexpr int kLevel = 1002;
constexpr int kDictionary = 1003;
constexpr int kSolid = 1004;
constexpr int kThreads = 1005;
constexpr int kPassword = 1006;
constexpr int kShowPassword = 1007;
constexpr int kEncryptNames = 1008;
constexpr int kTarget = 1009;
constexpr int kBrowse = 1010;
constexpr int kEstimate = 1011;
constexpr int kReason = 1012;
constexpr int kOk = 1;      // == IDOK
constexpr int kCancel = 2;  // == IDCANCEL
}  // namespace ids

// Which control a rule is talking about. Used by Adjustment (what changed) and
// by WhyDisabled (why the control cannot be touched).
enum class Field {
  Format,
  Level,
  Dictionary,
  Solid,
  Threads,
  Password,
  ShowPassword,
  EncryptFileNames,
  Target,
  Browse,
};

inline std::string_view ToString(Field field) {
  switch (field) {
    case Field::Format: return "format";
    case Field::Level: return "level";
    case Field::Dictionary: return "dictionary";
    case Field::Solid: return "solid";
    case Field::Threads: return "threads";
    case Field::Password: return "password";
    case Field::ShowPassword: return "showPassword";
    case Field::EncryptFileNames: return "encryptFileNames";
    case Field::Target: return "target";
    case Field::Browse: return "browse";
  }
  return "?";
}

// CONTRACTS B6: an enum printed by a failing assertion needs operator<<. Field
// lives outside PrismCore so the preflight heuristic does not scan it, but a
// suite that prints a Field would otherwise dump an integer.
inline std::ostream& operator<<(std::ostream& os, Field field) { return os << ToString(field); }

// ---------------------------------------------------------------------------
// Bounds and the dictionary ladder (both taken FROM CompressionPlan, never
// re-invented here: the combo must offer exactly what the plan can request).
// ---------------------------------------------------------------------------
inline constexpr int kLevelMin = 0;
inline constexpr int kLevelMax = 9;
inline constexpr int kThreadMin = 1;
inline constexpr int kThreadMax = 64;  // 7-Zip's multi-thread ceiling (CompressionPlan)

// One entry per level 1..9, plus index 0 = "Auto" (0 bytes == let the plan
// derive it from level AND input size).
inline constexpr size_t kDictionaryChoiceCount = 1 + static_cast<size_t>(kLevelMax);

inline std::vector<uint64_t> DictionaryChoices() {
  std::vector<uint64_t> choices;
  choices.reserve(kDictionaryChoiceCount);
  choices.push_back(0);  // Auto
  for (int level = 1; level <= kLevelMax; ++level) {
    choices.push_back(prism::LevelDictionaryBytes(level));
  }
  return choices;
}

inline int DictionaryIndexFor(uint64_t bytes) {
  const std::vector<uint64_t> choices = DictionaryChoices();
  for (size_t i = 0; i < choices.size(); ++i) {
    if (choices[i] == bytes) return static_cast<int>(i);
  }
  return 0;  // not on the ladder: fall back to Auto rather than lie
}

// ---------------------------------------------------------------------------
// Unit conversion (what the dialog prints). 1024-based, one decimal from 1 KB
// up: the numbers a settings dialog shows.
// ---------------------------------------------------------------------------
inline std::string FormatBytes(uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  const char* const units[] = {"KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int unit = -1;
  do {
    value /= 1024.0;
    ++unit;
  } while (value >= 1024.0 && unit < 3);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.1f %s", value, units[unit]);
  return std::string(buffer);
}

inline std::string FormatDurationMs(int64_t ms) {
  if (ms <= 0) return "0 s";
  if (ms < 1000) return "<1 s";
  const int64_t seconds = (ms + 999) / 1000;
  if (seconds < 60) return std::to_string(seconds) + " s";
  const int64_t minutes = seconds / 60;
  const int64_t rest = seconds % 60;
  if (minutes < 60) {
    return std::to_string(minutes) + " min" + (rest != 0 ? " " + std::to_string(rest) + " s" : "");
  }
  return std::to_string(minutes / 60) + " h " + std::to_string(minutes % 60) + " min";
}

inline std::string Trim(std::string_view text) {
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1])) != 0) --end;
  return std::string(text.substr(begin, end - begin));
}

inline std::string LowerAscii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

// The threads edit is text, so the conversion has to be explicit and it has to
// refuse rather than guess: "8x" used to be a number in some parsers, and a
// silently-ignored thread count is the kind of bug that shows up as a 6-hour
// compression run.
struct ParsedThreads {
  bool valid = true;
  int threads = 0;  // 0 == auto (CompressionOptions::threads contract)
  bool clamped = false;
  std::string reason;
};

inline ParsedThreads ParseThreads(std::string_view raw) {
  ParsedThreads out;
  const std::string text = Trim(raw);
  if (text.empty()) return out;  // empty == auto
  if (LowerAscii(text) == "auto") return out;
  int value = 0;
  for (char c : text) {
    if (c < '0' || c > '9') {
      out.valid = false;
      out.reason = "Threads must be a number from 1 to 64, or empty/Auto.";
      return out;
    }
    if (value < 1000000) value = value * 10 + (c - '0');
  }
  if (value <= 0) return out;  // 0 == auto, same meaning as CompressionOptions
  if (value > kThreadMax) {
    out.threads = kThreadMax;
    out.clamped = true;
    out.reason = "7-Zip runs at most 64 threads; using 64.";
    return out;
  }
  if (value < kThreadMin) {
    out.threads = kThreadMin;
    out.clamped = true;
    out.reason = "At least one thread is needed; using 1.";
    return out;
  }
  out.threads = value;
  return out;
}

inline std::string FormatThreadsText(int threads) {
  return threads <= 0 ? std::string("Auto") : std::to_string(threads);
}

// ---------------------------------------------------------------------------
// What the estimate line promises.
//
// The two rates are MEASURED-THEN-ROUNDED-DOWN throughputs, deliberately
// pessimistic: they are used to print an UPPER bound, so over-promising speed is
// the only failure mode that matters. They are not a benchmark and the text
// never presents them as a prediction (see Estimate::text).
// ---------------------------------------------------------------------------
inline constexpr uint64_t kLzmaBytesPerSecondPerThread = 8ull << 20;  // conservative LZMA2
inline constexpr uint64_t kStoreBytesPerSecond = 200ull << 20;        // plain copy, one pass
inline constexpr int64_t kFixedOverheadMs = 60;                       // handler load + publish

inline int64_t MsForBytes(uint64_t bytes, uint64_t bytesPerSecond) {
  if (bytesPerSecond == 0) return 0;
  const uint64_t whole = bytes / bytesPerSecond;
  const uint64_t rest = bytes % bytesPerSecond;
  const uint64_t ms = whole * 1000 + (rest * 1000 + bytesPerSecond - 1) / bytesPerSecond;
  return static_cast<int64_t>(ms);
}

struct Estimate {
  bool ok = false;
  // The one line the dialog prints under the controls.
  std::string text;
  uint64_t inputBytes = 0;
  // 0 == no honest lower bound exists (compressed output has none: incompressible
  // data is stored, so the archive CAN exceed the input but is never bounded below).
  uint64_t lowerOutputBytes = 0;
  // Upper bound: the payload plus a per-entry record and one archive header.
  uint64_t upperOutputBytes = 0;
  int64_t upperMs = 0;
  int threads = 0;
};

// ---------------------------------------------------------------------------
// One control value the dialog changed on the user's behalf, and why. A reason
// is NEVER empty: a silent change is a lie the user cannot see through.
// ---------------------------------------------------------------------------
struct Adjustment {
  Field field = Field::Format;
  std::string reason;
};

// What "OK" hands to the caller (the wiring in Src/App, owned by card C25).
struct CreateRequest {
  prism::CompressionPlan plan;
  std::wstring archivePath;
  std::wstring password;
  std::wstring sourceRoot;
};

// ===========================================================================
// DialogState -- the pure half.
// ===========================================================================
class DialogState {
 public:
  DialogState() { Reset(); }

  // The defaults, in ONE place, so the dialog and its test cannot disagree.
  // They mirror CompressionOptions' defaults: 7z / level 5 / auto dictionary /
  // auto solid / auto threads / no password.
  void Reset() {
    format_ = prism::ArchiveFormat::SevenZip;
    level_ = 5;
    dictionaryIndex_ = 0;  // Auto
    solidMode_ = prism::SolidMode::Auto;
    threadsText_ = "Auto";
    threads_ = 0;
    threadsValid_ = true;
    password_.clear();
    showPassword_ = false;
    encryptFileNames_ = false;
    targetPath_.clear();
    items_.clear();
    sourceRoot_.clear();
    cpuCores_ = 0;
    adjustments_.clear();
  }

  // ---- control -> state ---------------------------------------------------
  void SetFormat(prism::ArchiveFormat format) {
    if (format != format_) {
      format_ = format;
      RetargetExtensionFor(format_);
    }
    Normalize();
  }

  void SetLevel(int level) {
    level_ = level;
    Normalize();
  }

  void SetDictionaryIndex(int index) {
    dictionaryIndex_ = index;
    Normalize();
  }

  void SetSolid(bool on) {
    solidMode_ = on ? prism::SolidMode::On : prism::SolidMode::Off;
    Normalize();
  }

  void SetThreadsText(std::string text) {
    threadsText_ = std::move(text);
    Normalize();
  }

  void SetPassword(std::string password) {
    password_ = std::move(password);
    Normalize();
  }

  void SetShowPassword(bool show) { showPassword_ = show; }

  void SetEncryptFileNames(bool on) {
    encryptFileNames_ = on;
    Normalize();
  }

  void SetTargetPath(std::string path) {
    targetPath_ = std::move(path);
    Normalize();
  }

  // The selection the app wants archived (UTF-8, '/'-separated -- PrismCore's
  // separator contract). sourceRoot is the directory those relative paths live
  // under and is carried through untouched for ArchiveWriter.
  void SetInput(std::vector<prism::PlanItem> items, std::string sourceRoot = {}) {
    items_ = std::move(items);
    sourceRoot_ = std::move(sourceRoot);
    Normalize();
  }

  // Cores are PASSED IN (CompressionPlan's layer rule): 0 means unknown and the
  // plan falls back to its own ceiling. The dialog only uses this for the
  // estimate, never as a default for the threads edit.
  void SetCpuCores(int cores) { cpuCores_ = cores; }

  // ---- state -> control ---------------------------------------------------
  prism::ArchiveFormat format() const { return format_; }
  int level() const { return level_; }
  int dictionaryIndex() const { return dictionaryIndex_; }
  uint64_t dictionaryBytes() const {
    const std::vector<uint64_t> choices = DictionaryChoices();
    const size_t index = static_cast<size_t>(dictionaryIndex_);
    return index < choices.size() ? choices[index] : 0;
  }
  prism::SolidMode solidMode() const { return solidMode_; }
  // What the CHECKBOX shows. Auto means "the format's honest default": solid for
  // 7z, never for zip/tar. Showing a checked box for a zip would be a promise
  // the engine cannot keep (CompressionPlan::ResolveSolid).
  bool solid() const {
    if (format_ == prism::ArchiveFormat::SevenZip) return solidMode_ != prism::SolidMode::Off;
    return solidMode_ == prism::SolidMode::On;
  }
  int threads() const { return threads_; }
  const std::string& threadsText() const { return threadsText_; }
  bool threadsValid() const { return threadsValid_; }
  const std::string& password() const { return password_; }
  bool encrypts() const { return !password_.empty(); }
  bool showPassword() const { return showPassword_; }
  bool encryptFileNames() const { return encryptFileNames_; }
  const std::string& targetPath() const { return targetPath_; }
  const std::vector<prism::PlanItem>& input() const { return items_; }
  const std::string& sourceRoot() const { return sourceRoot_; }
  uint64_t inputBytes() const {
    uint64_t total = 0;
    for (const prism::PlanItem& item : items_) {
      if (!item.isDir) total += item.size;
    }
    return total;
  }
  size_t inputCount() const { return items_.size(); }

  // ---- mapping: state -> plan (the direction the product uses) ------------
  prism::CompressionOptions ToOptions() const {
    prism::CompressionOptions options;
    options.format = format_;
    // tar is a container: CompressionPlan forces level 0 and warns when a level
    // was requested. Sending 0 says what the engine will actually do, so the
    // plan's warning list stays meaningful for the archive itself.
    options.level = format_ == prism::ArchiveFormat::Tar ? 0 : level_;
    // Zip's deflate has no dictionary knob and tar has no coder; promising one
    // would make SetProperties fail for a cosmetic reason (CompressionPlan caps
    // it to 0 for those formats -- we do it here so the estimate matches).
    options.dictionaryBytes =
        format_ == prism::ArchiveFormat::SevenZip ? dictionaryBytes() : uint64_t{0};
    options.solid = solidMode_;
    options.threads = threads_;
    options.encrypt = !password_.empty();
    options.encryptFileNames = encryptFileNames_;
    options.cpuCores = cpuCores_;
    return options;
  }

  prism::CompressionPlan BuildPlan() const {
    return prism::MakeCompressionPlan(items_, ToOptions());
  }

  // ---- mapping: plan -> state (the other direction) -----------------------
  // Adopts the RESOLVED values a plan actually holds, which is what the dialog
  // must show when it re-opens on an existing choice: plan.solid is what the
  // engine will do (not what was requested), plan.dictionaryBytes is the size
  // that survived the input-size clamp. The password is not part of a plan and
  // is left alone.
  void AdoptPlan(const prism::CompressionPlan& plan) {
    format_ = plan.format;
    level_ = plan.level;
    solidMode_ = plan.solid ? prism::SolidMode::On : prism::SolidMode::Off;
    threadsText_ = std::to_string(plan.threads);
    threads_ = plan.threads;
    threadsValid_ = true;
    dictionaryIndex_ = DictionaryIndexFor(plan.dictionaryBytes);
    encryptFileNames_ = plan.encryptFileNames;
    Normalize();
  }

  // ---- enablement ---------------------------------------------------------
  bool IsEnabled(Field field) const {
    switch (field) {
      case Field::Format:
      case Field::Target:
      case Field::Browse: return true;
      case Field::Level: return format_ != prism::ArchiveFormat::Tar;
      case Field::Dictionary:
        return format_ == prism::ArchiveFormat::SevenZip && level_ != 0;
      case Field::Solid: return format_ == prism::ArchiveFormat::SevenZip;
      case Field::Threads: return format_ != prism::ArchiveFormat::Tar;
      case Field::Password:
      case Field::ShowPassword: return format_ != prism::ArchiveFormat::Tar;
      case Field::EncryptFileNames:
        return format_ == prism::ArchiveFormat::SevenZip && !password_.empty();
    }
    return true;
  }

  // The sentence printed next to a greyed-out control. Empty when it is enabled:
  // a disabled control without a reason is a bug report waiting to happen.
  std::string WhyDisabled(Field field) const {
    if (IsEnabled(field)) return std::string();
    switch (field) {
      case Field::Level:
        return "tar is a container: it stores files verbatim, so there is no level to pick.";
      case Field::Dictionary:
        if (format_ != prism::ArchiveFormat::SevenZip) {
          return std::string(prism::ToString(format_)) + " has no dictionary setting.";
        }
        return "level 0 stores the data (Copy), so no dictionary is allocated.";
      case Field::Solid:
        return std::string(prism::ToString(format_)) +
               " cannot be solid: entries are written independently.";
      case Field::Threads:
        return "tar is I/O bound; the engine writes one stream, so threads buy nothing.";
      case Field::Password:
        return "tar has no cipher: encryption cannot be requested at all.";
      case Field::ShowPassword: return WhyDisabled(Field::Password);
      case Field::EncryptFileNames:
        if (password_.empty()) {
          return "Set a password first: header encryption without one is refused.";
        }
        return "Only 7z encrypts file names (header encryption).";
      default: return std::string();
    }
  }

  // ---- hard refusals: "OK" is disabled while this is non-empty -------------
  std::vector<std::string> Blockers() const {
    std::vector<std::string> blockers;
    if (items_.empty()) {
      blockers.push_back("Nothing selected to archive.");
    } else if (targetPath_.empty()) {
      blockers.push_back("Choose where to save the archive.");
    } else if (targetPath_.find_first_of("*?\"<>|") != std::string::npos) {
      // Same guard ArchiveWriter applies before writing a byte (it cannot be a
      // legal Windows path either, so catching it here saves a failed create).
      blockers.push_back("The target path contains one of * ? \" < > |.");
    }
    if (!threadsValid_) {
      blockers.push_back("Threads must be a number from 1 to 64, or empty/Auto.");
    }
    // SECURITY: tar has no cipher. Refuse; dropping the password silently would
    // hand back an unencrypted archive the user believes is encrypted.
    if (format_ == prism::ArchiveFormat::Tar && !password_.empty()) {
      blockers.push_back(
          "tar cannot encrypt: the tar container has no cipher. Pick 7z or zip, or clear the "
          "password.");
    }
    if (encryptFileNames_ && password_.empty()) {
      blockers.push_back("Encrypting file names needs a password.");
    }
    if (blockers.empty()) {
      // Last word goes to the decision layer itself: whatever CompressionPlan
      // refuses (an unrepresentable path, an option the format cannot honour) is
      // a refusal here too, with the plan's own sentence.
      const prism::CompressionPlan plan = BuildPlan();
      if (!plan.ok) blockers.push_back(plan.error);
    }
    return blockers;
  }

  bool OkEnabled() const { return Blockers().empty(); }

  // The one sentence the dialog paints under the controls. Order of authority:
  // what the dialog just changed on the user's behalf, then the refusal that is
  // stopping "OK", and -- when nothing is refused -- the reason the first greyed
  // out control is greyed out. A dialog that disables a control without saying
  // why is a bug report waiting to happen, so the last case is not optional.
  std::string ReasonLine() const {
    std::string line;
    auto append = [&line](const std::string& text) {
      if (text.empty()) return;
      if (!line.empty()) line += "  ";
      line += text;
    };
    for (const Adjustment& adjustment : adjustments_) append(adjustment.reason);

    const std::vector<std::string> blockers = Blockers();
    if (!blockers.empty()) {
      append(blockers.front());
      return line;
    }
    for (const Field field :
         {Field::Solid, Field::Dictionary, Field::Level, Field::Threads, Field::Password,
          Field::EncryptFileNames}) {
      if (IsEnabled(field)) continue;
      if (HasAdjustment(field)) continue;  // already explained above, in its own words
      const std::string why = WhyDisabled(field);
      if (!why.empty()) {
        append(why);
        break;
      }
    }
    return line;
  }

  // ---- estimate (C9's cost model, printed honestly) -----------------------
  Estimate ComputeEstimate() const {
    Estimate estimate;
    const prism::CompressionPlan plan = BuildPlan();
    estimate.inputBytes = plan.ok ? plan.inputBytes : inputBytes();
    if (!plan.ok) {
      estimate.text = "Nothing to estimate: " + plan.error;
      return estimate;
    }
    estimate.ok = true;
    estimate.threads = plan.threads;
    const uint64_t records = plan.fileCount + plan.dirCount;
    const uint64_t overhead = 4096 + 512 * records;  // header + one entry per item
    estimate.upperOutputBytes = plan.inputBytes + overhead;
    if (plan.cost.driver == prism::CostDriver::StoreInputOnce) {
      // Stored data: every input byte is in the archive, so the input size is a
      // HARD lower bound (CostModel::lowerBoundBytes says exactly this).
      estimate.lowerOutputBytes = plan.inputBytes;
      estimate.upperMs = MsForBytes(plan.inputBytes, kStoreBytesPerSecond) + kFixedOverheadMs;
    } else {
      estimate.lowerOutputBytes = 0;  // no honest lower bound; see CostModel
      const uint64_t rate = kLzmaBytesPerSecondPerThread *
                            static_cast<uint64_t>(plan.threads > 0 ? plan.threads : 1);
      estimate.upperMs = MsForBytes(plan.inputBytes, rate) + kFixedOverheadMs;
    }
    std::string text = "<= " + FormatBytes(estimate.upperOutputBytes) + " output";
    if (estimate.lowerOutputBytes != 0) {
      text += " (never below " + FormatBytes(estimate.lowerOutputBytes) +
              ": every input byte is stored)";
    } else {
      text += " (no lower bound: incompressible data is stored as-is)";
    }
    text += " | <= " + FormatDurationMs(estimate.upperMs) + " | 1 pass, " + plan.methodName;
    if (plan.cost.driver == prism::CostDriver::CompressInputOnce && plan.threads > 1) {
      text += " x" + std::to_string(plan.threads) + " threads";
    }
    estimate.text = std::move(text);
    return estimate;
  }

  // ---- what changed, and why (drained by whoever prints it) ---------------
  std::vector<Adjustment> TakeAdjustments() {
    std::vector<Adjustment> drained;
    drained.swap(adjustments_);
    return drained;
  }
  const std::vector<Adjustment>& adjustments() const { return adjustments_; }

  // ---- deliberate-defect bookkeeping (CONTRACTS D3) ----------------------
  // Every mutant branch below bumps this counter, and the mutant test binary
  // prints its "CAUGHT" marker only when the run went red AND this is non-zero
  // (CONTRACTS D26: the marker is the conjunction, never a WILL_FAIL + regex
  // pair). Otherwise a mutation that silently stopped firing would still look
  // like a sharp test.
  static int& MutationCount() {
    static int count = 0;
    return count;
  }
  static bool MutationFired() { return MutationCount() != 0; }

 private:
  bool HasAdjustment(Field field) const {
    for (const Adjustment& adjustment : adjustments_) {
      if (adjustment.field == field) return true;
    }
    return false;
  }

  void Note(Field field, std::string reason) {
    if (reason.empty()) return;
    for (const Adjustment& existing : adjustments_) {
      if (existing.field == field && existing.reason == reason) return;
    }
    adjustments_.push_back(Adjustment{field, std::move(reason)});
  }

  // A format switch renames the target when the current extension is one of
  // ours ("out.zip" -> "out.7z"). A name with a foreign extension is left alone:
  // silently appending ".7z" to "backup.dat" would be a rename the user did not
  // ask for.
  void RetargetExtensionFor(prism::ArchiveFormat format) {
    if (targetPath_.empty()) return;
    const size_t dot = targetPath_.find_last_of('.');
    const size_t slash = targetPath_.find_last_of("\\/");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return;
    const std::string extension = LowerAscii(targetPath_.substr(dot));
    const std::string wanted(prism::ExtensionFor(format));
    if (extension != ".7z" && extension != ".zip" && extension != ".tar") return;
    if (extension == wanted) return;
    targetPath_ = targetPath_.substr(0, dot) + wanted;
    Note(Field::Target, "Target renamed to " + targetPath_ + " to match " +
                            std::string(prism::ToString(format)) + ".");
  }

  void Normalize() {
    // 1. level: the slider can only produce 0..9, so an out-of-range number is a
    //    caller bug -- clamp it (never reject) and say so.
    if (level_ < kLevelMin || level_ > kLevelMax) {
      const int clamped = (std::min)((std::max)(level_, kLevelMin), kLevelMax);
      Note(Field::Level, "The level slider covers 0..9; using " + std::to_string(clamped) + ".");
      level_ = clamped;
    }
    // 2. dictionary: an index outside the ladder is not a size at all.
    if (dictionaryIndex_ < 0 || static_cast<size_t>(dictionaryIndex_) >= kDictionaryChoiceCount) {
      Note(Field::Dictionary, "No such dictionary size in the LZMA2 ladder; using Auto.");
      dictionaryIndex_ = 0;
    }
    // 3. threads: text -> number, refusing what is not a number.
    const ParsedThreads parsed = ParseThreads(threadsText_);
    threadsValid_ = parsed.valid;
    if (parsed.valid) {
      threads_ = parsed.threads;
      if (!parsed.reason.empty()) Note(Field::Threads, parsed.reason);
    } else {
      threads_ = 0;
    }
    // 4. solid on a container that has no solid block: SOFT downgrade, but the
    //    user is told and the control turns off (a checked box that does nothing
    //    is the lie this rule exists to prevent).
#if defined(PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED)
    // MUTANT (CONTRACTS D3): accept the illegal combination. zip keeps "solid"
    // checked while CompressionPlan silently resolves it to false, so the UI
    // promises a solid archive that will never exist. The suite must go red.
    if (format_ != prism::ArchiveFormat::SevenZip && solidMode_ == prism::SolidMode::On) {
      static_cast<void>(MutationCount()++);
    }
#else
    if (format_ != prism::ArchiveFormat::SevenZip && solidMode_ == prism::SolidMode::On) {
      solidMode_ = prism::SolidMode::Off;
      Note(Field::Solid, std::string(prism::ToString(format_)) +
                             " cannot be solid: entries are written independently, so solid is "
                             "off.");
    }
#endif
    // 5. header encryption is a 7z-only capability. Asking for it elsewhere would
    //    produce a listing with the file names in plain sight.
    if (format_ != prism::ArchiveFormat::SevenZip && encryptFileNames_) {
      encryptFileNames_ = false;
      Note(Field::EncryptFileNames,
           std::string(prism::ToString(format_)) +
               " cannot encrypt file names: only 7z encrypts its header. The option is off.");
    }
    // 6. tar + password: HARD refusal (see Blockers). The mutant below turns it
    //    into the classic fail-open downgrade so the suite can prove it is sharp.
#if defined(PRISM_CD_MUTANT_TAR_ENCRYPT_ALLOWED)
    // MUTANT (CONTRACTS D3): silently drop the password instead of refusing, so
    // the user gets an unencrypted tar they asked to encrypt.
    if (format_ == prism::ArchiveFormat::Tar && !password_.empty()) {
      static_cast<void>(MutationCount()++);
      password_.clear();
    }
#endif
  }

  prism::ArchiveFormat format_ = prism::ArchiveFormat::SevenZip;
  int level_ = 5;
  int dictionaryIndex_ = 0;
  prism::SolidMode solidMode_ = prism::SolidMode::Auto;
  std::string threadsText_ = "Auto";
  int threads_ = 0;
  bool threadsValid_ = true;
  std::string password_;
  bool showPassword_ = false;
  bool encryptFileNames_ = false;
  std::string targetPath_;
  std::vector<prism::PlanItem> items_;
  std::string sourceRoot_;
  int cpuCores_ = 0;
  std::vector<Adjustment> adjustments_;
};

// ===========================================================================
// CompressDialog -- the Win32 shell (no rules live below this line).
// ===========================================================================
#if defined(_WIN32)

inline std::wstring WideFromUtf8(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                         nullptr, 0);
  std::wstring wide(static_cast<size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
  return wide;
}

inline std::string Utf8FromWide(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string utf8(static_cast<size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed,
                      nullptr, nullptr);
  return utf8;
}

class CompressDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveCompressDialog";

  // Client-area geometry: one column of labels, one of controls, then the two
  // message lines and the buttons. Fixed because the layout is trivial and a
  // resize handler would be more code than the dialog deserves.
  static constexpr int kClientWidth = 560;
  static constexpr int kClientHeight = 480;

  using AcceptHandler = std::function<bool(const CreateRequest&)>;
  using BrowseHandler = std::function<std::wstring()>;

  CompressDialog() = default;
  ~CompressDialog() { Destroy(); }
  CompressDialog(const CompressDialog&) = delete;
  CompressDialog& operator=(const CompressDialog&) = delete;

  // Diagnostics for the only two things that can make Create() fail: the window
  // itself, or a child control whose class is not registered. A test that cannot
  // create a window for ENVIRONMENT reasons must skip; a control that fails here
  // is our bug and must go red, so the two are reported separately.
  DWORD lastError() const { return lastError_; }
  int missingControlId() const { return missingControlId_; }

  bool Create(HINSTANCE instance, const std::wstring& title = L"Create archive") {
    instance_ = instance;
    EnsureCommonControls();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &CompressDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW (wide on purpose)
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
      lastError_ = GetLastError();
      if (lastError_ != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    RECT frame{0, 0, kClientWidth, kClientHeight};
    AdjustWindowRectEx(&frame, WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED, FALSE,
                       WS_EX_DLGMODALFRAME);
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title.c_str(),
                            WS_CAPTION | WS_SYSMENU | WS_OVERLAPPED | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left,
                            frame.bottom - frame.top, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) lastError_ = GetLastError();
    return hwnd_ != nullptr;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }

  DialogState& State() { return state_; }
  const DialogState& State() const { return state_; }

  void SetAcceptHandler(AcceptHandler handler) { onAccept_ = std::move(handler); }
  // C25 supplies the real shell/COM "save as" picker (comdlg32's GetSaveFileNameW
  // or the IFileSaveDialog shell item); this dialog only asks for a path.
  void SetBrowseHandler(BrowseHandler handler) { onBrowse_ = std::move(handler); }

  void SetSource(std::vector<prism::PlanItem> items, std::wstring sourceRoot) {
    sourceRootWide_ = std::move(sourceRoot);
    state_.SetInput(std::move(items), Utf8FromWide(sourceRootWide_));
    SyncControls();
  }

  const CreateRequest& LastRequest() const { return lastRequest_; }
  bool Accepted() const { return accepted_; }

  // The message entry point. WndProc calls it; the Windows test calls it too, so
  // a case can drive the REAL handler chain without a message pump (the pattern
  // Tests/win/test_navigation.cpp established for GalleryWindow).
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  std::wstring ControlText(int id) const {
    const HWND control = Control(id);
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
  }

  std::string ControlTextUtf8(int id) const { return Utf8FromWide(ControlText(id)); }

  bool ControlEnabled(int id) const {
    const HWND control = Control(id);
    return control != nullptr && IsWindowEnabled(control) != FALSE;
  }

  // state -> controls. Public so a test (or the app, after AdoptPlan) can force a
  // refresh; every message handler calls it at the end.
  void SyncControls() {
    if (hwnd_ == nullptr) return;
    // SetWindowTextW on an edit control notifies EN_CHANGE back to us; the guard
    // keeps a programmatic sync from being interpreted as user input (which
    // would reset the caret on every keystroke).
    syncing_ = true;
    SendMessageW(formatBox_, CB_SETCURSEL, FormatIndex(state_.format()), 0);
    SendMessageW(levelSlider_, TBM_SETPOS, TRUE, state_.level());
    SendMessageW(dictionaryBox_, CB_SETCURSEL, state_.dictionaryIndex(), 0);
    SendMessageW(solidCheck_, BM_SETCHECK, state_.solid() ? BST_CHECKED : BST_UNCHECKED, 0);
    SetTextIfDifferent(threadsEdit_, WideFromUtf8(state_.threadsText()));
    SetTextIfDifferent(passwordEdit_, WideFromUtf8(state_.password()));
    SendMessageW(showPasswordCheck_, BM_SETCHECK,
                 state_.showPassword() ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(encryptNamesCheck_, BM_SETCHECK,
                 state_.encryptFileNames() ? BST_CHECKED : BST_UNCHECKED, 0);
    SetTextIfDifferent(targetEdit_, WideFromUtf8(state_.targetPath()));
    ApplyPasswordMask();

    EnableWindow(formatBox_, TRUE);
    EnableWindow(levelSlider_, state_.IsEnabled(Field::Level) ? TRUE : FALSE);
    EnableWindow(dictionaryBox_, state_.IsEnabled(Field::Dictionary) ? TRUE : FALSE);
    EnableWindow(solidCheck_, state_.IsEnabled(Field::Solid) ? TRUE : FALSE);
    EnableWindow(threadsEdit_, state_.IsEnabled(Field::Threads) ? TRUE : FALSE);
    EnableWindow(passwordEdit_, state_.IsEnabled(Field::Password) ? TRUE : FALSE);
    EnableWindow(showPasswordCheck_, state_.IsEnabled(Field::ShowPassword) ? TRUE : FALSE);
    EnableWindow(encryptNamesCheck_, state_.IsEnabled(Field::EncryptFileNames) ? TRUE : FALSE);

    SetWindowTextW(estimateText_, WideFromUtf8(state_.ComputeEstimate().text).c_str());
    SetWindowTextW(reasonText_, WideFromUtf8(state_.ReasonLine()).c_str());
    EnableWindow(okButton_, state_.OkEnabled() ? TRUE : FALSE);
    syncing_ = false;
  }

 private:
  static constexpr WPARAM kPasswordChar = 0x25CF;  // Black circle, the Win32 default mask

  static void EnsureCommonControls() {
    // msctls_trackbar32 is a common control: without InitCommonControlsEx the
    // CreateWindowExW for the slider fails and the dialog silently loses its
    // level control (the classic "works on my box" regression).
    //
    // The return value is NOT used as the gate: measured on this project's dev
    // box (2026-11-01) InitCommonControlsEx returns FALSE with GetLastError()==0
    // while the trackbar class IS registered and creates fine -- a v5 comctl32
    // whose activation context is not established yet behaves exactly that way.
    // Gating on it made the whole dialog refuse to open. The honest gate is the
    // control creation itself (see MakeControl), which reports WHICH id failed.
    static const bool once = [] {
      INITCOMMONCONTROLSEX settings{};
      settings.dwSize = sizeof(settings);
      settings.dwICC = ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
      InitCommonControlsEx(&settings);
      return true;
    }();
    static_cast<void>(once);
  }

  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CompressDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<CompressDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // WM_CREATE (and therefore CreateChildren) runs INSIDE CreateWindowExW,
      // before the caller can assign the returned handle. Without this line the
      // child controls are created with a null parent -- they become orphan
      // top-level windows and the dialog loses every control it drew.
      self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }

  // hwnd is passed in (not read from hwnd_) so the default branch still has a
  // valid window after WM_DESTROY cleared the member.
  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        return CreateChildren() ? 0 : -1;
      case WM_COMMAND:
        if (!syncing_) OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
        return 0;
      case WM_HSCROLL:
        if (!syncing_) OnHScroll(reinterpret_cast<HWND>(lparam));
        return 0;
      case WM_NOTIFY:
        if (!syncing_) OnNotify(reinterpret_cast<const NMHDR*>(lparam));
        return 0;
      case WM_CLOSE:
        // No PostQuitMessage: this dialog lives inside the app's message loop,
        // and quitting the process is the app's decision, not a dialog's.
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        hwnd_ = nullptr;
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }

  HWND MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, int x, int y,
                   int width, int height, DWORD exStyle = 0) {
    const HWND control = CreateWindowExW(
        exStyle, klass, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (control == nullptr) {
      // Remember WHICH control failed: "the dialog did not open" is not a
      // diagnosable report, "control 1002 (trackbar) failed" is.
      missingControlId_ = id;
      lastError_ = GetLastError();
      return nullptr;
    }
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),
                 TRUE);
    return control;
  }

  HWND MakeLabel(const wchar_t* text, int x, int y, int width) {
    return MakeControl(L"STATIC", text, SS_LEFT, 0, x, y, width, 20);
  }

  bool CreateChildren() {
    constexpr int kLabelX = 16;
    constexpr int kControlX = 168;
    constexpr int kControlWidth = 360;
    constexpr int kRow = 32;

    formatBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                             ids::kFormat, kControlX, 14, 200, 200);
    levelSlider_ = MakeControl(L"msctls_trackbar32", L"",
                               TBS_HORZ | TBS_AUTOTICKS | TBS_BOTTOM | WS_TABSTOP, ids::kLevel,
                               kControlX, 14 + kRow * 1, kControlWidth - 60, 32);
    dictionaryBox_ = MakeControl(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP,
                                 ids::kDictionary, kControlX, 14 + kRow * 2, 260, 260);
    solidCheck_ = MakeControl(L"BUTTON", L"Solid archive (one block improves the ratio)",
                              BS_AUTOCHECKBOX | WS_TABSTOP, ids::kSolid, kControlX,
                              14 + kRow * 3, kControlWidth, 20);
    threadsEdit_ = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                               ids::kThreads, kControlX, 14 + kRow * 4, 90, 22);
    passwordEdit_ = MakeControl(L"EDIT", L"", ES_PASSWORD | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                                ids::kPassword, kControlX, 14 + kRow * 5, 260, 22);
    showPasswordCheck_ =
        MakeControl(L"BUTTON", L"Show password", BS_AUTOCHECKBOX | WS_TABSTOP,
                    ids::kShowPassword, kControlX, 14 + kRow * 6, 200, 20);
    encryptNamesCheck_ =
        MakeControl(L"BUTTON", L"Encrypt file names (7z header)", BS_AUTOCHECKBOX | WS_TABSTOP,
                    ids::kEncryptNames, kControlX, 14 + kRow * 7, 300, 20);
    targetEdit_ = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, ids::kTarget,
                              kControlX, 14 + kRow * 8, 250, 22);
    browseButton_ =
        MakeControl(L"BUTTON", L"Browse...", BS_PUSHBUTTON | WS_TABSTOP, ids::kBrowse,
                    kControlX + 258, 14 + kRow * 8, 90, 24);
    estimateText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kEstimate, 16, 14 + kRow * 9 + 8,
                                kClientWidth - 32, 34);
    reasonText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kReason, 16, 14 + kRow * 9 + 46,
                              kClientWidth - 32, 52);
    okButton_ = MakeControl(L"BUTTON", L"Create", BS_DEFPUSHBUTTON | WS_TABSTOP, ids::kOk,
                            kClientWidth - 200, kClientHeight - 44, 84, 28);
    cancelButton_ = MakeControl(L"BUTTON", L"Cancel", BS_PUSHBUTTON | WS_TABSTOP, ids::kCancel,
                                kClientWidth - 104, kClientHeight - 44, 84, 28);

    MakeLabel(L"Format", kLabelX, 18, 140);
    MakeLabel(L"Compression level", kLabelX, 18 + kRow, 140);
    MakeLabel(L"Dictionary", kLabelX, 18 + kRow * 2, 140);
    MakeLabel(L"Threads (empty = auto)", kLabelX, 18 + kRow * 4, 140);
    MakeLabel(L"Password", kLabelX, 18 + kRow * 5, 140);
    MakeLabel(L"Save to", kLabelX, 18 + kRow * 8, 140);

    if (formatBox_ == nullptr || levelSlider_ == nullptr || dictionaryBox_ == nullptr ||
        solidCheck_ == nullptr || threadsEdit_ == nullptr || passwordEdit_ == nullptr ||
        showPasswordCheck_ == nullptr || encryptNamesCheck_ == nullptr || targetEdit_ == nullptr ||
        browseButton_ == nullptr || estimateText_ == nullptr || reasonText_ == nullptr ||
        okButton_ == nullptr || cancelButton_ == nullptr) {
      return false;
    }

    SendMessageW(levelSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(kLevelMin, kLevelMax));
    SendMessageW(levelSlider_, TBM_SETTICFREQ, 1, 0);
    for (const std::string& label : FormatChoices()) {
      SendMessageW(formatBox_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(WideFromUtf8(label).c_str()));
    }
    for (const std::string& label : DictionaryChoiceLabels()) {
      SendMessageW(dictionaryBox_, CB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(WideFromUtf8(label).c_str()));
    }
    SyncControls();
    return true;
  }

 public:
  static std::vector<std::string> FormatChoices() {
    return {std::string(prism::ToString(prism::ArchiveFormat::SevenZip)),
            std::string(prism::ToString(prism::ArchiveFormat::Zip)),
            std::string(prism::ToString(prism::ArchiveFormat::Tar))};
  }

  static int FormatIndex(prism::ArchiveFormat format) {
    switch (format) {
      case prism::ArchiveFormat::SevenZip: return 0;
      case prism::ArchiveFormat::Zip: return 1;
      case prism::ArchiveFormat::Tar: return 2;
    }
    return 0;
  }

  static prism::ArchiveFormat FormatAt(int index) {
    switch (index) {
      case 1: return prism::ArchiveFormat::Zip;
      case 2: return prism::ArchiveFormat::Tar;
      default: return prism::ArchiveFormat::SevenZip;
    }
  }

  static std::vector<std::string> DictionaryChoiceLabels() {
    std::vector<std::string> labels;
    const std::vector<uint64_t> choices = DictionaryChoices();
    for (size_t i = 0; i < choices.size(); ++i) {
      if (i == 0) {
        labels.push_back("Auto (from level and input size)");
      } else {
        labels.push_back(FormatBytes(choices[i]));
      }
    }
    return labels;
  }

 private:
  void OnCommand(int id, int code, HWND control) {
    if (control == nullptr) return;
    switch (id) {
      case ids::kFormat:
        if (code == CBN_SELCHANGE) {
          const int index = static_cast<int>(SendMessageW(formatBox_, CB_GETCURSEL, 0, 0));
          state_.SetFormat(FormatAt(index));
        } else {
          return;
        }
        break;
      case ids::kDictionary:
        if (code == CBN_SELCHANGE) {
          const int index = static_cast<int>(SendMessageW(dictionaryBox_, CB_GETCURSEL, 0, 0));
          state_.SetDictionaryIndex(index);
        } else {
          return;
        }
        break;
      case ids::kSolid:
        if (code != BN_CLICKED) return;
        state_.SetSolid(SendMessageW(solidCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
      case ids::kThreads:
        if (code != EN_CHANGE) return;
        state_.SetThreadsText(ControlTextUtf8(ids::kThreads));
        break;
      case ids::kPassword:
        if (code != EN_CHANGE) return;
        state_.SetPassword(ControlTextUtf8(ids::kPassword));
        break;
      case ids::kShowPassword:
        if (code != BN_CLICKED) return;
        state_.SetShowPassword(SendMessageW(showPasswordCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED);
        break;
      case ids::kEncryptNames:
        if (code != BN_CLICKED) return;
        state_.SetEncryptFileNames(SendMessageW(encryptNamesCheck_, BM_GETCHECK, 0, 0) ==
                                   BST_CHECKED);
        break;
      case ids::kTarget:
        if (code != EN_CHANGE) return;
        state_.SetTargetPath(ControlTextUtf8(ids::kTarget));
        break;
      case ids::kBrowse:
        if (code != BN_CLICKED) return;
        OnBrowse();
        return;
      case ids::kOk:
        if (code != BN_CLICKED) return;
        OnAccept();
        return;
      case ids::kCancel:
        if (code != BN_CLICKED) return;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
      default: return;
    }
    SyncControls();
  }

  // The slider speaks WM_HSCROLL (not WM_COMMAND); a real trackbar puts its own
  // HWND in lParam, which is how the control is identified here.
  void OnHScroll(HWND control) {
    if (control != levelSlider_) return;
    SyncLevelFromSlider();
    SyncControls();
  }

  // WM_NOTIFY carries the trackbar's NM_RELEASEDCAPTURE -- the "the user let go"
  // moment. Committing there (as well as live on WM_HSCROLL) is what makes a
  // drag end on an exact position.
  void OnNotify(const NMHDR* header) {
    if (header == nullptr) return;
    if (header->idFrom == static_cast<UINT_PTR>(ids::kLevel) && header->code == NM_RELEASEDCAPTURE) {
      SyncLevelFromSlider();
      SyncControls();
    }
  }

  void SyncLevelFromSlider() {
    const int position = static_cast<int>(SendMessageW(levelSlider_, TBM_GETPOS, 0, 0));
    state_.SetLevel(position);
  }

  void OnBrowse() {
    if (!onBrowse_) return;
    const std::wstring chosen = onBrowse_();
    if (chosen.empty()) return;
    state_.SetTargetPath(Utf8FromWide(chosen));
    SyncControls();
  }

  void OnAccept() {
    if (!state_.OkEnabled()) {
      SyncControls();  // the reason line already explains why the button could not fire
      return;
    }
    CreateRequest request;
    request.plan = state_.BuildPlan();
    if (!request.plan.ok) {
      SyncControls();
      return;
    }
    request.archivePath = WideFromUtf8(state_.targetPath());
    request.password = WideFromUtf8(state_.password());
    request.sourceRoot = sourceRootWide_;
    lastRequest_ = request;
    accepted_ = true;
    // The handler owns the create (ArchiveWriter::CreateArchive + progress into
    // the HUD). Returning true closes the dialog; false keeps it open so a
    // failed create can be retried with the same settings.
    if (onAccept_ && onAccept_(request)) SendMessageW(hwnd_, WM_CLOSE, 0, 0);
  }

  void SetTextIfDifferent(HWND control, const std::wstring& text) const {
    if (control == nullptr) return;
    if (ControlTextOf(control) == text) return;
    SetWindowTextW(control, text.c_str());
  }

  static std::wstring ControlTextOf(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<size_t>(length));
    return buffer;
  }

  void ApplyPasswordMask() const {
    if (passwordEdit_ == nullptr) return;
    SendMessageW(passwordEdit_, EM_SETPASSWORDCHAR, state_.showPassword() ? 0 : kPasswordChar, 0);
    InvalidateRect(passwordEdit_, nullptr, TRUE);
  }

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  DWORD lastError_ = 0;
  int missingControlId_ = 0;
  DialogState state_;
  std::wstring sourceRootWide_;
  bool syncing_ = false;
  bool accepted_ = false;
  CreateRequest lastRequest_;

  HWND formatBox_ = nullptr;
  HWND levelSlider_ = nullptr;
  HWND dictionaryBox_ = nullptr;
  HWND solidCheck_ = nullptr;
  HWND threadsEdit_ = nullptr;
  HWND passwordEdit_ = nullptr;
  HWND showPasswordCheck_ = nullptr;
  HWND encryptNamesCheck_ = nullptr;
  HWND targetEdit_ = nullptr;
  HWND browseButton_ = nullptr;
  HWND estimateText_ = nullptr;
  HWND reasonText_ = nullptr;
  HWND okButton_ = nullptr;
  HWND cancelButton_ = nullptr;

  AcceptHandler onAccept_;
  BrowseHandler onBrowse_;
};

#endif  // _WIN32

}  // namespace prism::win::cd
