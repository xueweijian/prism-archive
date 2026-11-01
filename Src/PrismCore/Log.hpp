// PrismArchive -- PrismCore
// Log: the graded, structured, redacted, rate-limited log pipeline behind the
// crash report (blueprint ch. 7.4 "diagnostics" + CONTRACTS B7).
//
// WHY THIS IS PrismCore AND NOT PrismWin
//   Everything here is pure C++20 -- a level filter, a deterministic formatter,
//   a secret filter, a token bucket, a ring buffer and a rolling file sink. That
//   is the entire decision surface of logging, so it is unit tested in seconds on
//   Linux/CI. PrismWin/CrashDump.hpp only adds the two things that need the OS:
//   SetUnhandledExceptionFilter and MiniDumpWriteDump.
//
// THE FOUR PROPERTIES, EACH ONE WITH A FAILURE MODE IT PREVENTS
//   1. GRADED     -- a minimum level drops the rest before formatting, so debug
//                    chatter can never cost bytes in a release build.
//   2. STRUCTURED -- one record is ONE line with explicit "key=value" fields and
//                    every value escaped. A message containing a newline must
//                    not be able to forge a second record (log injection).
//   3. REDACTED   -- CONTRACTS B7: credentials are never printed. Two independent
//                    passes: (a) a field whose KEY names a credential is replaced
//                    wholesale, (b) the free text is scanned for
//                    "password=..."-shaped spans, URL userinfo, and every known
//                    secret (full match AND >=4-char fragments -- a half-leaked
//                    password is a leak). Redaction is what the dump in
//                    %LOCALAPPDATA%\PrismArchive\crash\ is safe to share because of.
//   4. BOUNDED    -- the ring keeps the last N formatted lines for the crash
//                    report, and the throttle answers "this message repeated 400
//                    times in 10 ms" with a suppression COUNT instead of 400
//                    lines. Both are byte-bounded by construction (CONTRACTS D17:
//                    a capacity is a promise, so 0 throws instead of silently
//                    degrading).
//
// MUTATION SEAM (CONTRACTS D3)
//   PRISM_C16_MUTANT_NO_REDACTION compiles the redaction away. The suite is built
//   a second time with it and registered WILL_FAIL: the redaction cases MUST go
//   red, and if the macro ever stops matching they pass, which turns the mutant
//   registration red. The injection is verified, not assumed.
#pragma once

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(PRISM_C16_MUTANT_NO_REDACTION)
#define PRISM_C16_LOG_MUTANT 1
#endif

namespace prism::log {

// ---------------------------------------------------------------------------
// Levels
// ---------------------------------------------------------------------------
enum class Level : int {
  kTrace = 0,
  kDebug = 1,
  kInfo = 2,
  kWarn = 3,
  kError = 4,
  kFatal = 5,
};

inline const char* LevelName(Level level) {
  switch (level) {
    case Level::kTrace: return "TRACE";
    case Level::kDebug: return "DEBUG";
    case Level::kInfo: return "INFO";
    case Level::kWarn: return "WARN";
    case Level::kError: return "ERROR";
    case Level::kFatal: return "FATAL";
  }
  return "INFO";
}

// CONTRACTS B6: this enum reaches PRISM_CHECK_EQ, so it must be printable.
inline std::ostream& operator<<(std::ostream& os, Level level) { return os << LevelName(level); }

// The single literal a redaction leaves behind. Pinned by the tests: the crash
// report is read by humans and by grep, so the marker must be stable.
inline const char kRedacted[] = "[redacted]";

// ---------------------------------------------------------------------------
// Deterministic UTC timestamps (no <chrono> calendar dependency: the civil
// conversion below is Howard Hinnant's days->y/m/d algorithm, which is exact for
// every date in the proleptic Gregorian calendar and therefore identical on
// every toolchain).
// ---------------------------------------------------------------------------
// Zero-padded decimal, no printf: see FormatUtcTimestamp for why.
inline void AppendPadded(std::string& out, int64_t value, size_t width) {
  const bool negative = value < 0;
  std::string digits = std::to_string(negative ? -value : value);
  while (digits.size() < width) digits.insert(digits.begin(), '0');
  if (negative) digits.insert(digits.begin(), '-');
  out += digits;
}

inline std::string FormatUtcTimestamp(int64_t epochMillis) {
  // Pre-epoch input is clamped: a negative timestamp is a caller bug, and a
  // deterministic 1970 line is better than a wrong 1969 one.
  const int64_t ms = epochMillis < 0 ? 0 : epochMillis;
  const int64_t days = ms / 86400000;
  int64_t rest = ms % 86400000;
  const int64_t hours = rest / 3600000;
  rest %= 3600000;
  const int64_t minutes = rest / 60000;
  rest %= 60000;
  const int64_t seconds = rest / 1000;
  const int64_t millis = rest % 1000;

  const int64_t z = days + 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const int64_t doe = z - era * 146097;
  const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const int64_t y = yoe + era * 400;
  const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const int64_t mp = (5 * doy + 2) / 153;
  const int64_t day = doy - (153 * mp + 2) / 5 + 1;
  const int64_t month = mp < 10 ? mp + 3 : mp - 9;
  const int64_t year = month <= 2 ? y + 1 : y;

  // Built with std::to_string, not snprintf: gcc's -Wformat-truncation cannot
  // bound %04lld against a 20-digit int64 and fails the build under -Werror
  // (measured on ubuntu-latest, 2026-11-01). Zero-padding by hand is also the
  // more honest expression of "these fields have fixed widths".
  std::string out;
  out.reserve(24);
  AppendPadded(out, year, 4);
  out.push_back('-');
  AppendPadded(out, month, 2);
  out.push_back('-');
  AppendPadded(out, day, 2);
  out.push_back('T');
  AppendPadded(out, hours, 2);
  out.push_back(':');
  AppendPadded(out, minutes, 2);
  out.push_back(':');
  AppendPadded(out, seconds, 2);
  out.push_back('.');
  AppendPadded(out, millis, 3);
  out.push_back('Z');
  return out;
}

// ---------------------------------------------------------------------------
// Escaping. One record is one line, always: control characters are escaped
// instead of being emitted, so log text can never break the record framing.
// ---------------------------------------------------------------------------
inline std::string EscapeText(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (const char raw : text) {
    const unsigned char c = static_cast<unsigned char>(raw);
    switch (raw) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20 || c == 0x7F) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\x%02X", static_cast<unsigned>(c));
          out += buffer;
        } else {
          out.push_back(raw);  // UTF-8 bytes pass through untouched
        }
    }
  }
  return out;
}

struct Field {
  std::string key;
  std::string value;
};

struct Record {
  int64_t timestampMs = 0;
  Level level = Level::kInfo;
  std::string component;  // "openflow", "crash", ... (empty = no prefix)
  std::string message;
  std::vector<Field> fields;
  // Empty = this record is never throttled. Otherwise records sharing this key
  // share one token bucket (that is what turns 400 repeats into one line).
  std::string throttleKey;
};

// The canonical wire form. Assumes the record was already sanitized; Logger is
// the only caller that formats, so sanitization cannot be forgotten.
//   <ts> <LEVEL> <component>: <message> <key>="<value>" ...
inline std::string FormatRecord(const Record& record) {
  std::string out = FormatUtcTimestamp(record.timestampMs);
  out.push_back(' ');
  const std::string name = LevelName(record.level);
  out += name;
  for (size_t i = name.size(); i < 5; ++i) out.push_back(' ');
  out.push_back(' ');
  if (!record.component.empty()) {
    out += EscapeText(record.component);
    out += ": ";
  }
  out += EscapeText(record.message);
  for (const Field& field : record.fields) {
    if (field.key.empty()) continue;
    out.push_back(' ');
    out += EscapeText(field.key);
    out += "=\"";
    out += EscapeText(field.value);
    out.push_back('"');
  }
  return out;
}

// ---------------------------------------------------------------------------
// Redaction (CONTRACTS B7: environment/env credentials are never printed).
// ---------------------------------------------------------------------------
// Keys are compared with separators removed and ASCII case folded, so
// "API_KEY", "api-key" and "ApiKey" are one key.
inline std::string NormalizeKey(const std::string& key) {
  std::string out;
  out.reserve(key.size());
  for (const char raw : key) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (c == '_' || c == '-' || c == ' ' || c == '.' || c == '/') continue;
    out.push_back(static_cast<char>(std::tolower(c)));
  }
  return out;
}

inline bool IsSensitiveKey(const std::string& key) {
  const std::string normal = NormalizeKey(key);
  if (normal.empty()) return false;
  static const char* const kExact[] = {
      "pwd",        "pass",       "password",  "passwd",    "passphrase", "secret",
      "token",      "apikey",     "apitoken",  "accesskey", "privatekey", "key",
      "auth",       "authorization", "credentials", "credential", "cookie", "sessionkey",
      "salt",
  };
  for (const char* candidate : kExact) {
    if (normal == candidate) return true;
  }
  // A qualifying suffix/prefix ("db_password", "user_token") is the same secret.
  static const char* const kContains[] = {
      "password", "passwd", "passphrase", "secret", "token", "credential",
      "apikey",   "accesskey", "privatekey",
  };
  for (const char* candidate : kContains) {
    if (normal.find(candidate) != std::string::npos) return true;
  }
  return false;
}

inline bool IsKeyChar(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  return std::isalnum(u) != 0 || c == '_' || c == '-';
}

// Delimiters that end an unquoted value.
inline bool IsValueDelimiter(char c) {
  const unsigned char u = static_cast<unsigned char>(c);
  if (std::isspace(u) != 0) return true;
  switch (c) {
    case ',': case ';': case '&': case ')': case ']': case '}':
    case '<': case '>': case '"': case '\'': case '|':
      return true;
    default:
      return false;
  }
}

inline bool IsAuthorityDelimiter(char c) {
  return c == '/' || c == '?' || c == '#' || c == ' ' || c == '\t' || c == '\r' ||
         c == '\n' || c == '"' || c == '\'';
}

// Free-text pass A: "password=...", "pwd: ...", "--token=..." style spans. The
// KEY does not have to be one the caller declared -- that is the whole point:
// a diagnostic string built somewhere else still cannot leak a credential.
inline std::string RedactTokenSpans(const std::string& text) {
  std::string out = text;
  size_t cursor = 0;
  while (cursor < out.size()) {
    // A sensitive token must be a whole word: the preceding character cannot
    // continue it ("notpassword" is not "password").
    const bool atWordStart = cursor == 0 || !IsKeyChar(out[cursor - 1]);
    size_t keyEnd = cursor;
    if (atWordStart) {
      while (keyEnd < out.size() && IsKeyChar(out[keyEnd])) ++keyEnd;
    }
    if (keyEnd > cursor && IsSensitiveKey(out.substr(cursor, keyEnd - cursor))) {
      size_t p = keyEnd;
      while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
      if (p < out.size() && (out[p] == '=' || out[p] == ':')) {
        ++p;
        while (p < out.size() && (out[p] == ' ' || out[p] == '\t')) ++p;
        const size_t markerLength = sizeof(kRedacted) - 1;
        if (p < out.size() && (out[p] == '"' || out[p] == '\'')) {
          const char quote = out[p];
          size_t close = out.find(quote, p + 1);
          if (close == std::string::npos) close = out.size();
          const size_t valueStart = p + 1;
          if (close > valueStart && out.compare(valueStart, close - valueStart, kRedacted) != 0) {
            out.replace(valueStart, close - valueStart, kRedacted);
            cursor = valueStart + markerLength;
            continue;
          }
          cursor = close < out.size() ? close + 1 : out.size();
          continue;
        }
        size_t valueEnd = p;
        while (valueEnd < out.size() && !IsValueDelimiter(out[valueEnd])) ++valueEnd;
        if (valueEnd > p && out.compare(p, valueEnd - p, kRedacted) != 0) {
          out.replace(p, valueEnd - p, kRedacted);
          cursor = p + markerLength;
          continue;
        }
        cursor = valueEnd;
        continue;
      }
    }
    ++cursor;
  }
  return out;
}

// Free-text pass B: scheme://user:password@host -- proxy URLs and engine URLs
// carry credentials in the authority.
inline std::string RedactUrlUserInfo(const std::string& text) {
  std::string out = text;
  const size_t markerLength = sizeof(kRedacted) - 1;
  size_t at = 0;
  while ((at = out.find("://", at)) != std::string::npos) {
    const size_t authorityStart = at + 3;
    size_t authorityEnd = authorityStart;
    while (authorityEnd < out.size() && !IsAuthorityDelimiter(out[authorityEnd])) ++authorityEnd;
    if (authorityEnd > authorityStart) {
      const size_t atSign = out.rfind('@', authorityEnd - 1);
      if (atSign != std::string::npos && atSign > authorityStart && atSign < authorityEnd) {
        const size_t colon = out.find(':', authorityStart);
        if (colon != std::string::npos && colon + 1 < atSign) {
          out.replace(colon + 1, atSign - colon - 1, kRedacted);
          at = colon + 1 + markerLength;
          continue;
        }
      }
    }
    at = authorityEnd;
  }
  return out;
}

class Redactor {
 public:
  void AddSecret(std::string secret) {
    if (secret.empty()) return;
    secrets_.push_back(std::move(secret));
  }
  void ClearSecrets() { secrets_.clear(); }
  size_t secretCount() const { return secrets_.size(); }
  const std::vector<std::string>& secrets() const { return secrets_; }

  // Known secrets: the full string, and every proper fragment of length >= 4
  // (a half-leaked password is a leak -- same rule as PasswordRing).
  std::string Redact(const std::string& text) const {
#ifdef PRISM_C16_LOG_MUTANT
    (void)text;
    return text;  // MUTANT: redaction compiled out on purpose (WILL_FAIL target)
#else
    std::string out = RedactTokenSpans(text);
    out = RedactUrlUserInfo(out);
    for (const std::string& secret : secrets_) {
      if (secret.size() < 3) continue;  // too short to match anything safely
      size_t at = 0;
      while ((at = out.find(secret, at)) != std::string::npos) {
        out.replace(at, secret.size(), kRedacted);
        at += sizeof(kRedacted) - 1;
      }
      for (size_t length = secret.size() - 1; length >= 4; --length) {
        for (size_t start = 0; start + length <= secret.size(); ++start) {
          const std::string piece = secret.substr(start, length);
          size_t p = 0;
          while ((p = out.find(piece, p)) != std::string::npos) {
            out.replace(p, piece.size(), kRedacted);
            p += sizeof(kRedacted) - 1;
          }
        }
      }
    }
    return out;
#endif
  }

  // A field whose KEY names a credential is replaced wholesale: the value never
  // even reaches the token scanner.
  Field RedactField(const Field& field) const {
#ifdef PRISM_C16_LOG_MUTANT
    return field;
#else
    Field out = field;
    out.value = IsSensitiveKey(field.key) ? std::string(kRedacted) : Redact(field.value);
    return out;
#endif
  }

 private:
  std::vector<std::string> secrets_;
};

// ---------------------------------------------------------------------------
// Ring buffer: the last N formatted lines, for the crash report.
// ---------------------------------------------------------------------------
class RingBuffer {
 public:
  explicit RingBuffer(size_t capacity) : capacity_(capacity) {
    // A 0-capacity ring is a programmer error, not a mode: it would silently
    // promise "the last 64 lines" and keep none (CONTRACTS D17).
    if (capacity == 0) throw std::invalid_argument("RingBuffer capacity must be >= 1");
  }

  void Push(std::string line) {
    if (lines_.size() == capacity_) lines_.pop_front();
    lines_.push_back(std::move(line));
  }

  size_t capacity() const { return capacity_; }
  size_t size() const { return lines_.size(); }
  bool empty() const { return lines_.empty(); }

  // Oldest -> newest. Snapshot() is the whole ring, Last(n) its newest n.
  std::vector<std::string> Snapshot() const { return Last(capacity_); }

  std::vector<std::string> Last(size_t count) const {
    if (count > lines_.size()) count = lines_.size();
    const auto begin = lines_.end() - static_cast<std::ptrdiff_t>(count);
    return std::vector<std::string>(begin, lines_.end());
  }

  void Clear() { lines_.clear(); }

 private:
  size_t capacity_;
  std::deque<std::string> lines_;
};

// ---------------------------------------------------------------------------
// Throttle: a token bucket per key, evaluated against a CALLER-SUPPLIED clock.
// No clock is read here, so a test can drive the window deterministically.
// ---------------------------------------------------------------------------
struct ThrottleConfig {
  size_t capacity = 8;    // records per key per window that are allowed through
  int64_t windowMs = 1000;
  size_t maxKeys = 256;   // tracked keys; more than this is allowed, never dropped
};

struct ThrottleVerdict {
  bool allow = true;
  size_t suppressed = 0;  // dropped since the last allowed record for this key
};

class Throttle {
 public:
  explicit Throttle(ThrottleConfig config) : config_(config) {
    if (config_.capacity == 0) throw std::invalid_argument("Throttle capacity must be >= 1");
    if (config_.windowMs <= 0) throw std::invalid_argument("Throttle window must be > 0");
    if (config_.maxKeys == 0) throw std::invalid_argument("Throttle maxKeys must be >= 1");
  }

  ThrottleVerdict Allow(const std::string& key, int64_t nowMs) {
    Bucket* bucket = Find(key);
    if (bucket == nullptr) {
      if (buckets_.size() >= config_.maxKeys) {
        // The key table is full. Dropping a first-seen record because of memory
        // pressure would hide a brand-new failure, so it is allowed untracked --
        // and counted, so the behaviour is observable instead of silent.
        ++untrackedAllows_;
        return {true, 0};
      }
      Bucket fresh;
      fresh.key = key;
      fresh.windowStartMs = nowMs;
      buckets_.push_back(std::move(fresh));
      bucket = &buckets_.back();
    }
    if (nowMs - bucket->windowStartMs >= config_.windowMs || nowMs < bucket->windowStartMs) {
      // A new window. A clock that moved backwards also resets rather than
      // throttling forever.
      bucket->windowStartMs = nowMs;
      bucket->used = 0;
    }
    if (bucket->used < config_.capacity) {
      ++bucket->used;
      const size_t suppressed = bucket->suppressed;
      bucket->suppressed = 0;
      return {true, suppressed};
    }
    ++bucket->suppressed;
    ++suppressedTotal_;
    return {false, 0};
  }

  size_t totalSuppressed() const { return suppressedTotal_; }
  size_t untrackedAllows() const { return untrackedAllows_; }
  size_t trackedKeys() const { return buckets_.size(); }
  const ThrottleConfig& config() const { return config_; }

  void Reset() {
    buckets_.clear();
    suppressedTotal_ = 0;
    untrackedAllows_ = 0;
  }

 private:
  struct Bucket {
    std::string key;
    int64_t windowStartMs = 0;
    size_t used = 0;
    size_t suppressed = 0;
  };

  Bucket* Find(const std::string& key) {
    for (Bucket& bucket : buckets_) {
      if (bucket.key == key) return &bucket;
    }
    return nullptr;
  }

  ThrottleConfig config_;
  std::vector<Bucket> buckets_;  // insertion-ordered: deterministic, no hashing
  size_t suppressedTotal_ = 0;
  size_t untrackedAllows_ = 0;
};

// ---------------------------------------------------------------------------
// Rolling file sink: one current file plus N-1 rotated ones. Rotation is part of
// the logging contract, not an afterthought: an unbounded file is how a log
// fills a user's disk after a week of thumbnail scanning.
//
// Naming: "dir/prism.log" -> "dir/prism.1.log" (previous) ... "dir/prism.<N-1>.log"
// (oldest). A record is written as one line + '\n'; a file may exceed maxBytes
// only by the single record that crossed the line (refusing to write it would
// lose the very record that explains the failure).
// ---------------------------------------------------------------------------
class RollingFileSink {
 public:
  RollingFileSink(std::string basePath, size_t maxBytes, size_t maxFiles)
      : base_(std::move(basePath)), maxBytes_(maxBytes), maxFiles_(maxFiles) {
    if (base_.empty()) throw std::invalid_argument("RollingFileSink base path must not be empty");
    if (maxBytes_ == 0) throw std::invalid_argument("RollingFileSink maxBytes must be >= 1");
    if (maxFiles_ == 0) throw std::invalid_argument("RollingFileSink maxFiles must be >= 1");
    Open();
  }

  ~RollingFileSink() { Close(); }

  RollingFileSink(const RollingFileSink&) = delete;
  RollingFileSink& operator=(const RollingFileSink&) = delete;

  bool Write(const std::string& line) {
    if (!stream_.is_open()) return false;
    const size_t bytes = line.size() + 1;
    // Rotate BEFORE writing, but only when something is already in the file:
    // an oversized first record must still land.
    if (currentBytes_ > 0 && currentBytes_ + bytes > maxBytes_) {
      if (!Rotate()) return false;
    }
    stream_ << line << '\n';
    if (!stream_) {
      ok_ = false;
      error_ = "write failed on " + base_;
      return false;
    }
    stream_.flush();  // a log line that is still in a user-space buffer when the
                      // process dies is exactly the line the crash report needs.
    currentBytes_ += bytes;
    bytesWritten_ += bytes;
    return true;
  }

  void Close() {
    if (stream_.is_open()) stream_.close();
  }

  bool ok() const { return ok_; }
  const std::string& error() const { return error_; }
  const std::string& basePath() const { return base_; }
  size_t maxBytes() const { return maxBytes_; }
  size_t maxFiles() const { return maxFiles_; }
  size_t rotations() const { return rotations_; }
  size_t currentBytes() const { return currentBytes_; }
  uint64_t bytesWritten() const { return bytesWritten_; }

  // index 0 = the current file, 1 = the previous one, ...
  std::string PathFor(size_t index) const {
    if (index == 0) return base_;
    const size_t dot = base_.find_last_of('.');
    const size_t slash = base_.find_last_of("/\\");
    const std::string suffix = "." + std::to_string(index);
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
      return base_ + suffix;
    }
    return base_.substr(0, dot) + suffix + base_.substr(dot);
  }

 private:
  void Open() {
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(base_).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    const auto size = std::filesystem::file_size(base_, ec);
    currentBytes_ = ec ? 0 : static_cast<size_t>(size);
    stream_.open(base_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
      ok_ = false;
      error_ = "cannot open " + base_;
      return;
    }
    ok_ = true;
  }

  bool Rotate() {
    stream_.close();
    std::error_code ec;
    if (maxFiles_ <= 1) {
      std::filesystem::remove(PathFor(0), ec);
    } else {
      std::filesystem::remove(PathFor(maxFiles_ - 1), ec);  // the oldest goes first
      for (size_t index = maxFiles_ - 1; index >= 2; --index) {
        std::filesystem::rename(PathFor(index - 1), PathFor(index), ec);
        ec.clear();
      }
      std::filesystem::rename(PathFor(0), PathFor(1), ec);
      ec.clear();
    }
    ++rotations_;
    currentBytes_ = 0;
    stream_.open(base_, std::ios::binary | std::ios::app);
    if (!stream_.is_open()) {
      ok_ = false;
      error_ = "cannot reopen " + base_ + " after rotation";
      return false;
    }
    return true;
  }

  std::string base_;
  size_t maxBytes_;
  size_t maxFiles_;
  std::ofstream stream_;
  size_t currentBytes_ = 0;
  uint64_t bytesWritten_ = 0;
  size_t rotations_ = 0;
  bool ok_ = false;
  std::string error_;
};

// ---------------------------------------------------------------------------
// Logger: filter -> sanitize -> throttle -> format -> ring -> sink. The order is
// the contract: redaction happens BEFORE formatting, so no sink can ever receive
// a secret (that is what makes "the crash dump has no password" true by
// construction rather than by a reviewer's diligence).
// ---------------------------------------------------------------------------
class Logger {
 public:
  using Sink = std::function<void(const std::string&)>;

  Logger(Level minLevel, size_t ringCapacity, ThrottleConfig throttleConfig = {})
      : minLevel_(minLevel), ring_(ringCapacity), throttle_(throttleConfig) {}

  void SetMinLevel(Level level) { minLevel_ = level; }
  Level minLevel() const { return minLevel_; }

  Redactor& redactor() { return redactor_; }
  const Redactor& redactor() const { return redactor_; }

  RingBuffer& ring() { return ring_; }
  const RingBuffer& ring() const { return ring_; }

  Throttle& throttle() { return throttle_; }
  const Throttle& throttle() const { return throttle_; }

  // A sink is optional: the ring alone is a valid configuration (a test, or a
  // build whose only consumer is the crash report).
  void SetSink(Sink sink) { sink_ = std::move(sink); }
  void ClearSink() { sink_ = nullptr; }

  bool Emit(const Record& record) {
    if (static_cast<int>(record.level) < static_cast<int>(minLevel_)) {
      ++filtered_;
      return false;
    }
    Record sanitized = record;
    sanitized.component = redactor_.Redact(record.component);
    sanitized.message = redactor_.Redact(record.message);
    sanitized.fields.clear();
    sanitized.fields.reserve(record.fields.size());
    for (const Field& field : record.fields) {
      sanitized.fields.push_back(redactor_.RedactField(field));
    }

    if (!sanitized.throttleKey.empty()) {
      const ThrottleVerdict verdict = throttle_.Allow(sanitized.throttleKey, sanitized.timestampMs);
      if (!verdict.allow) {
        ++suppressed_;
        return false;
      }
      if (verdict.suppressed > 0) {
        // The count is the evidence: a reader must be able to tell "this failed
        // once" from "this failed 400 times".
        sanitized.fields.push_back({"suppressed", std::to_string(verdict.suppressed)});
      }
    }

    const std::string line = FormatRecord(sanitized);
    ring_.Push(line);
    if (sink_) sink_(line);
    ++emitted_;
    return true;
  }

  bool Emit(Level level, std::string component, std::string message,
            std::vector<Field> fields = {}, int64_t timestampMs = 0,
            std::string throttleKey = std::string()) {
    Record record;
    record.timestampMs = timestampMs;
    record.level = level;
    record.component = std::move(component);
    record.message = std::move(message);
    record.fields = std::move(fields);
    record.throttleKey = std::move(throttleKey);
    return Emit(record);
  }

  size_t emitted() const { return emitted_; }
  size_t filtered() const { return filtered_; }
  size_t suppressed() const { return suppressed_; }

 private:
  Level minLevel_;
  Redactor redactor_;
  RingBuffer ring_;
  Throttle throttle_;
  Sink sink_;
  size_t emitted_ = 0;
  size_t filtered_ = 0;
  size_t suppressed_ = 0;
};

}  // namespace prism::log
