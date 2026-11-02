// PrismArchive -- PrismCore
// DiagnosticsReport: the ONE thing a user can paste into a support thread.
//
// WHY THIS EXISTS
//   A user installs PrismArchive, the right-click entry never appears, and the
//   only two things we can do are "send a screenshot" and "run these commands".
//   Nothing in the product can answer "what is actually on that machine". This
//   module is the answer: the version, the build flavour, the OS, the CPU arch,
//   which 7z.dll was found and what version it is, how many formats the engine
//   reports, whether this is an installed or a portable copy, whether the .7z
//   ProgID and the static menu verbs exist in the registry, the language, the
//   theme, the recent files (NAME ONLY) and a level histogram of the last 20 log
//   records.
//
// WHAT IT MUST NEVER CONTAIN (this is the hard part)
//   The report is meant to be pasted in public. So:
//     * NO user name, ever -- every value is passed through a needle list and a
//       hit becomes "[user]";
//     * NO full path, ever -- a path collapses to "C:\...\a.7z" (drive, an
//       ellipsis, the last component). The directory tree is what identifies a
//       person ("C:\Users\alice\Documents\divorce\..."), and it is never useful
//       for diagnosis;
//     * NO vault content, NO password: this module never receives them. The log
//       section carries COUNTS ONLY, so a message that leaked a secret upstream
//       cannot reach the report through the histogram (defence in depth on top
//       of C16's redaction -- CONTRACTS D25);
//     * one value = one line: control characters are replaced, so a value can
//       never forge a second line (the same framing rule C16's log applies).
//
// CONTRACTS
//   B1  parenthesised (std::max)/(std::min) -- a consumer TU may include
//       windows.h without NOMINMAX.
//   B2  this header includes <cstdint> itself.
//   D1  nothing here skips: a missing value is printed as "unknown", never
//       omitted, so the shape of the report is stable.
//   D3  PRISM_C57_MUTANT_NO_REDACTION compiles the redaction away and bumps a
//       counter, so the suite can prove the injection actually fired.
//   D17 the caps below are promises: the report is bounded by construction.
//   D25 redaction happens BEFORE formatting -- the redactor produces the only
//       path/filename strings the writer ever sees.
#pragma once

#include <cstddef>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Log.hpp"

#if defined(PRISM_C57_MUTANT_NO_REDACTION)
#define PRISM_C57_REDACTION_MUTANT 1
#endif

namespace prism::diagnostics {

// ---------------------------------------------------------------------------
// 1. Caps (CONTRACTS D17: a capacity is a promise, so they are all explicit)
// ---------------------------------------------------------------------------
inline constexpr std::size_t kMaxLineChars = 120;   // one rendered line
inline constexpr std::size_t kMaxValueChars = 44;   // one scalar value
inline constexpr std::size_t kMaxNameChars = 28;    // the surviving name of a path
inline constexpr std::size_t kMaxRecentFiles = 16;  // recent.* entries
inline constexpr std::size_t kLogWindow = 20;       // "the last 20 log records"
inline constexpr std::size_t kMinNeedleChars = 3;   // shorter than this matches everything

inline constexpr char kEllipsis[] = "\xE2\x80\xA6";  // U+2026, UTF-8
inline constexpr char kUserPlaceholder[] = "[user]";

// ---------------------------------------------------------------------------
// 2. Vocabulary
// ---------------------------------------------------------------------------
enum class BuildFlavor : std::uint8_t { kRelease = 0, kDebug, kAddressSanitizer };

inline const char* BuildFlavorName(BuildFlavor flavor) {
  switch (flavor) {
    case BuildFlavor::kRelease: return "Release";
    case BuildFlavor::kDebug: return "Debug";
    case BuildFlavor::kAddressSanitizer: return "ASan";
  }
  return "unknown";
}

// CONTRACTS B6: an enum that reaches PRISM_CHECK_EQ must be printable.
inline std::ostream& operator<<(std::ostream& os, BuildFlavor flavor) {
  return os << BuildFlavorName(flavor);
}

// The flavour is a BUILD fact, not a runtime option: a user reporting a crash
// on an ASan build must be recognisable from the report alone.
inline BuildFlavor CompileTimeBuildFlavor() {
#if defined(__SANITIZE_ADDRESS__) || defined(PRISM_ASAN) || defined(_SANITIZE_ADDRESS_)
  return BuildFlavor::kAddressSanitizer;
#elif !defined(NDEBUG)
  return BuildFlavor::kDebug;
#else
  return BuildFlavor::kRelease;
#endif
}

enum class InstallMode : std::uint8_t { kUnknown = 0, kInstalled, kPortable };

inline const char* InstallModeName(InstallMode mode) {
  switch (mode) {
    case InstallMode::kInstalled: return "installed";
    case InstallMode::kPortable: return "portable";
    case InstallMode::kUnknown: return "unknown";
  }
  return "unknown";
}

inline std::ostream& operator<<(std::ostream& os, InstallMode mode) {
  return os << InstallModeName(mode);
}

// ---------------------------------------------------------------------------
// 3. Deliberate-defect bookkeeping (CONTRACTS D3)
// ---------------------------------------------------------------------------
inline int& RedactionMutantCount() {
  static int count = 0;
  return count;
}
inline bool RedactionMutantFired() { return RedactionMutantCount() != 0; }
inline void ResetRedactionMutant() { RedactionMutantCount() = 0; }

// ---------------------------------------------------------------------------
// 4. Text hygiene: strip, collapse, cap
// ---------------------------------------------------------------------------
inline bool IsPathSeparator(char c) { return c == '\\' || c == '/'; }

// One value = one line. Control characters (including CR/LF) become spaces, so a
// value can never forge a line of its own.
inline std::string StripControl(std::string_view raw) {
  std::string out;
  out.reserve(raw.size());
  bool pendingSpace = false;
  for (const char ch : raw) {
    const unsigned char u = static_cast<unsigned char>(ch);
    if (u < 0x20 || u == 0x7F) {
      pendingSpace = !out.empty();
      continue;
    }
    if (ch == ' ') {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(ch);
  }
  return out;
}

// UTF-8 safe: never cut in the middle of a multi-byte sequence.
inline std::string TruncateText(std::string text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) return text;
  const std::size_t marker = sizeof(kEllipsis) - 1;
  std::size_t cut = maxBytes > marker ? maxBytes - marker : 0;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) --cut;
  text.resize(cut);
  text += kEllipsis;
  return text;
}

// Truncation that keeps the extension: ".7z" is the diagnostic fact (it says
// which handler the shell tried), the 300 characters before it are not.
inline std::string TruncateKeepingExtension(std::string name, std::size_t maxBytes) {
  if (name.size() <= maxBytes) return name;
  const std::size_t dot = name.find_last_of('.');
  std::string extension;
  if (dot != std::string::npos && dot > 0 && name.size() - dot <= 12) extension = name.substr(dot);
  const std::size_t marker = sizeof(kEllipsis) - 1;
  std::size_t budget = maxBytes > extension.size() + marker ? maxBytes - extension.size() - marker : 0;
  std::string head = name.substr(0, budget);
  while (!head.empty() && (static_cast<unsigned char>(head.back()) & 0xC0) == 0x80) head.pop_back();
  return head + kEllipsis + extension;
}

inline char LowerAscii(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

inline std::string LowerAsciiCopy(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char ch : text) out.push_back(LowerAscii(ch));
  return out;
}

// The last component of a path (both separators, Windows and POSIX).
inline std::string_view LastPathComponent(std::string_view path) {
  std::size_t start = 0;
  for (std::size_t i = 0; i < path.size(); ++i) {
    if (IsPathSeparator(path[i])) start = i + 1;
  }
  return path.substr(start);
}

// ---------------------------------------------------------------------------
// 5. The redactor
//
// Every string in the report is produced HERE. There is no second way to put a
// path into a line, which is what makes "no full path in any line" a property of
// the code rather than of a reviewer's attention (CONTRACTS D25).
// ---------------------------------------------------------------------------
class Redactor {
 public:
  Redactor() = default;
  explicit Redactor(std::vector<std::string> needles) {
    for (std::string& needle : needles) AddNeedle(std::move(needle));
  }

  void AddNeedle(std::string needle) {
    std::string cleaned = StripControl(needle);
    if (cleaned.size() < kMinNeedleChars) return;  // too short to match safely
    needles_.push_back(std::move(cleaned));
  }

  const std::vector<std::string>& needles() const { return needles_; }

  // A scalar: control-stripped, needle-scrubbed, path-collapsed, capped.
  std::string Text(std::string_view raw) const {
    const std::string cleaned = StripControl(raw);
    const std::string collapsed = CollapsePaths(cleaned);
    return TruncateText(ApplyNeedles(collapsed), kMaxValueChars);
  }

  // A value that IS a path: "C:\Users\alice\AppData\...\7z.dll" -> "C:\...\7z.dll".
  std::string Path(std::string_view raw) const {
    const std::string cleaned = StripControl(raw);
#ifdef PRISM_C57_REDACTION_MUTANT
    // MUTANT (CONTRACTS D3): the path survives verbatim.
    static_cast<void>(RedactionMutantCount()++);
    return TruncateText(cleaned, kMaxLineChars);
#else
    return TruncateText(ApplyNeedles(RedactPathToken(cleaned)), kMaxLineChars);
#endif
  }

  // A recent-file entry: NAME ONLY, and the name is scrubbed too (a user can
  // name a file after themselves, and "alice-divorce.7z" is as identifying as
  // the folder it lived in).
  std::string FileName(std::string_view raw) const {
    const std::string cleaned = StripControl(raw);
#ifdef PRISM_C57_REDACTION_MUTANT
    // MUTANT (CONTRACTS D3): the whole path survives, not just the name.
    static_cast<void>(RedactionMutantCount()++);
    return TruncateText(cleaned, kMaxLineChars);
#else
    const std::string_view name = LastPathComponent(cleaned);
    return TruncateKeepingExtension(ApplyNeedles(std::string(name)), kMaxNameChars);
#endif
  }

 private:
  // A path token: drive (if any) + ellipsis + last component. UNC and device
  // prefixes lose their server/share/prefix entirely.
  static std::string RedactPathToken(std::string_view token) {
    if (token.size() >= 4 && (token[0] == '\\' || token[0] == '/') &&
        (token[1] == '\\' || token[1] == '/') && (token[2] == '?' || token[2] == '.')) {
      token = token.substr(4);  // \\?\C:\... / \\.\C:\... -- the prefix hides nothing
    }
    std::string drive;
    if (token.size() >= 3 && ((token[0] >= 'A' && token[0] <= 'Z') || (token[0] >= 'a' && token[0] <= 'z')) &&
        token[1] == ':' && IsPathSeparator(token[2])) {
      drive.push_back(static_cast<char>(token[0] >= 'a' ? token[0] - 'a' + 'A' : token[0]));
      token = token.substr(2);
    }
    const std::string_view name = LastPathComponent(token);
    std::string out = drive.empty() ? std::string(kEllipsis) : drive + ":\\" + kEllipsis;
    if (!name.empty()) {
      out += "\\";
      out += TruncateKeepingExtension(std::string(name), kMaxNameChars);
    }
    return out;
  }

  // Free text: everything from the FIRST path-looking token onward collapses to
  // one redacted path. Conservative on purpose -- losing text that was not a
  // path is cheap; leaking the tail of a path that contained spaces (the default
  // "C:\Program Files\...") is not.
  static std::string CollapsePaths(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
      const bool driveStart = i + 2 < text.size() &&
                              ((text[i] >= 'A' && text[i] <= 'Z') || (text[i] >= 'a' && text[i] <= 'z')) &&
                              text[i + 1] == ':' && IsPathSeparator(text[i + 2]);
      const bool uncStart = i + 1 < text.size() && IsPathSeparator(text[i]) &&
                            IsPathSeparator(text[i + 1]);
      if (!driveStart && !uncStart) continue;
      std::string out(text.substr(0, i));
      out += RedactPathToken(text.substr(i));
      return out;
    }
    return std::string(text);
  }

  // Case-insensitive replacement. The needles are short and the values are
  // capped, so a plain scan is both exact and cheap -- and unlike a case-folded
  // copy it never shifts the offsets it is writing into.
  std::string ApplyNeedles(std::string text) const {
    for (const std::string& needle : needles_) {
      const std::string lowerNeedle = LowerAsciiCopy(needle);
      const std::size_t replacement = sizeof(kUserPlaceholder) - 1;
      std::size_t at = 0;
      while (at + lowerNeedle.size() <= text.size()) {
        bool match = true;
        for (std::size_t k = 0; k < lowerNeedle.size(); ++k) {
          if (LowerAscii(text[at + k]) != lowerNeedle[k]) {
            match = false;
            break;
          }
        }
        if (!match) {
          ++at;
          continue;
        }
        text.replace(at, needle.size(), kUserPlaceholder);
        at += replacement;
      }
    }
    return text;
  }

  std::vector<std::string> needles_;
};

// ---------------------------------------------------------------------------
// 6. Install mode: a pure rule, so it is testable on every lane.
// ---------------------------------------------------------------------------
inline std::string NormalizeForPrefix(std::string_view path) {
  std::string out;
  out.reserve(path.size());
  for (const char ch : path) out.push_back(IsPathSeparator(ch) ? '\\' : LowerAscii(ch));
  while (!out.empty() && out.back() == '\\') out.pop_back();
  return out;
}

inline bool HasPrefix(std::string_view text, std::string_view prefix) {
  return !prefix.empty() && text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

// "installed" = the copy lives where the installers put it (a per-user
// Programs folder, or Program Files). "portable" = any other folder: a copy the
// user unzipped or moved. A paid-for distinction, because a portable copy has no
// per-machine registration and that is exactly what breaks the context menu.
inline InstallMode DetectInstallMode(std::string_view exePath, std::string_view localAppData,
                                     bool portableMarkerPresent) {
  const std::string exe = NormalizeForPrefix(exePath);
  if (exe.empty()) return InstallMode::kUnknown;
  if (portableMarkerPresent) return InstallMode::kPortable;
  const std::string local = NormalizeForPrefix(localAppData);
  if (!local.empty() && HasPrefix(exe, local + "\\programs\\")) return InstallMode::kInstalled;
  if (HasPrefix(exe, "c:\\program files\\")) return InstallMode::kInstalled;
  if (HasPrefix(exe, "c:\\program files (x86)\\")) return InstallMode::kInstalled;
  return InstallMode::kPortable;
}

// ---------------------------------------------------------------------------
// 7. Log histogram -- COUNTS ONLY, never a message.
// ---------------------------------------------------------------------------
struct LogLevelCounts {
  std::size_t sampled = 0;  // records inside the window
  std::size_t trace = 0;
  std::size_t debug = 0;
  std::size_t info = 0;
  std::size_t warn = 0;
  std::size_t error = 0;
  std::size_t fatal = 0;
  std::size_t other = 0;  // a line the wire format does not explain

  std::size_t Total() const { return trace + debug + info + warn + error + fatal + other; }
};

// C16's wire form is "<ts> <LEVEL> <component>: <message> <k>=\"<v>\" ...". The
// level is the first token after the timestamp. Anything else is "other", which
// keeps a hand-edited or truncated log line from crashing the count.
inline std::size_t* LevelSlot(LogLevelCounts& counts, std::string_view name) {
  if (name == "TRACE") return &counts.trace;
  if (name == "DEBUG") return &counts.debug;
  if (name == "INFO") return &counts.info;
  if (name == "WARN") return &counts.warn;
  if (name == "ERROR") return &counts.error;
  if (name == "FATAL") return &counts.fatal;
  return nullptr;
}

inline LogLevelCounts CountLogLevels(const std::vector<std::string>& lines) {
  LogLevelCounts counts;
  const std::size_t begin = lines.size() > kLogWindow ? lines.size() - kLogWindow : 0;
  for (std::size_t i = begin; i < lines.size(); ++i) {
    ++counts.sampled;
    const std::string& line = lines[i];
    std::size_t at = line.find(' ');
    if (at == std::string::npos) {
      ++counts.other;
      continue;
    }
    while (at < line.size() && line[at] == ' ') ++at;
    std::size_t end = at;
    while (end < line.size() && line[end] != ' ') ++end;
    if (end == at) {
      ++counts.other;
      continue;
    }
    std::string token = line.substr(at, end - at);
    for (char& ch : token) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
    std::size_t* slot = LevelSlot(counts, token);
    if (slot != nullptr) {
      ++(*slot);
    } else {
      ++counts.other;
    }
  }
  return counts;
}

// ---------------------------------------------------------------------------
// 8. The snapshot: everything the probe knows, nothing it does not
// ---------------------------------------------------------------------------
struct Snapshot {
  std::string version;        // VERSIONINFO FileVersion of the running module
  std::string buildType;      // empty = the compile-time flavour
  std::string osVersion;      // "10.0.22631"
  std::string cpuArch;        // "x64"
  std::string enginePath;     // FULL path of the 7z.dll that was found
  std::string engineVersion;  // that file's FileVersion
  int engineFormatCount = -1;  // < 0 = the engine could not be asked
  InstallMode installMode = InstallMode::kUnknown;
  bool progIdRegistered = false;   // HKCU\Software\Classes\.7z -> PrismArchive.7z
  bool menuHandlerRegistered = false;  // HKCU\Software\Classes\*\shell\PrismArchive*\command
  std::string language;       // "zh-CN"
  std::string theme;          // "night"
  std::vector<std::string> recentFiles;  // FULL paths, newest first
  std::vector<std::string> logLines;     // formatted C16 records, oldest first
  std::string userName;       // a redaction NEEDLE, never printed
  int64_t generatedAtMs = 0;  // 0 = "unknown" (no fake 1970 timestamp)
};

// The value of a boolean registry fact, spelled the same way everywhere so a
// grep over a pasted report is unambiguous.
inline const char* PresenceName(bool present) { return present ? "present" : "absent"; }

inline void AppendLine(std::vector<std::string>& lines, std::string_view key, std::string value) {
  if (value.empty()) value = "unknown";
  std::string line(key);
  line += " = ";
  line += value;
  lines.push_back(TruncateText(std::move(line), kMaxLineChars));
}

// ---------------------------------------------------------------------------
// 9. The report
// ---------------------------------------------------------------------------
inline std::vector<std::string> BuildReportLines(const Snapshot& snapshot) {
  Redactor redactor;
  redactor.AddNeedle(snapshot.userName);

  std::vector<std::string> lines;
  lines.push_back("# PrismArchive diagnostics report");
  AppendLine(lines, "generated",
             snapshot.generatedAtMs > 0 ? prism::log::FormatUtcTimestamp(snapshot.generatedAtMs)
                                        : std::string("unknown"));
  AppendLine(lines, "version", redactor.Text(snapshot.version));
  AppendLine(lines, "build", snapshot.buildType.empty()
                                ? std::string(BuildFlavorName(CompileTimeBuildFlavor()))
                                : redactor.Text(snapshot.buildType));
  AppendLine(lines, "os", redactor.Text(snapshot.osVersion));
  AppendLine(lines, "arch", redactor.Text(snapshot.cpuArch));
  AppendLine(lines, "engine.path", redactor.Path(snapshot.enginePath));
  AppendLine(lines, "engine.version", redactor.Text(snapshot.engineVersion));
  AppendLine(lines, "engine.formats", snapshot.engineFormatCount < 0
                                          ? std::string("unknown")
                                          : std::to_string(snapshot.engineFormatCount));
  AppendLine(lines, "install.mode", InstallModeName(snapshot.installMode));
  AppendLine(lines, "registry.progid", PresenceName(snapshot.progIdRegistered));
  AppendLine(lines, "registry.menu", PresenceName(snapshot.menuHandlerRegistered));
  AppendLine(lines, "ui.language", redactor.Text(snapshot.language));
  AppendLine(lines, "ui.theme", redactor.Text(snapshot.theme));

  const std::size_t emitted =
      snapshot.recentFiles.size() > kMaxRecentFiles ? kMaxRecentFiles : snapshot.recentFiles.size();
  AppendLine(lines, "recent.count", std::to_string(emitted));
  for (std::size_t i = 0; i < emitted; ++i) {
    AppendLine(lines, "recent." + std::to_string(i + 1), redactor.FileName(snapshot.recentFiles[i]));
  }
  if (snapshot.recentFiles.size() > emitted) {
    AppendLine(lines, "recent.truncated", std::to_string(snapshot.recentFiles.size() - emitted));
  }

  const LogLevelCounts counts = CountLogLevels(snapshot.logLines);
  AppendLine(lines, "log.window", std::to_string(kLogWindow));
  AppendLine(lines, "log.sampled", std::to_string(counts.sampled));
  AppendLine(lines, "log.trace", std::to_string(counts.trace));
  AppendLine(lines, "log.debug", std::to_string(counts.debug));
  AppendLine(lines, "log.info", std::to_string(counts.info));
  AppendLine(lines, "log.warn", std::to_string(counts.warn));
  AppendLine(lines, "log.error", std::to_string(counts.error));
  AppendLine(lines, "log.fatal", std::to_string(counts.fatal));
  AppendLine(lines, "log.other", std::to_string(counts.other));
  lines.push_back("# end of report");
  return lines;
}

inline std::string BuildReport(const Snapshot& snapshot) {
  std::string out;
  for (const std::string& line : BuildReportLines(snapshot)) {
    out += line;
    out.push_back('\n');
  }
  return out;
}

}  // namespace prism::diagnostics
