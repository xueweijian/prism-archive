// PrismArchive -- PrismCore
// Settings store: schema versioning, defaults, migration, validation and
// unknown-key preservation for the on-disk settings document (blueprint ch. 8.4).
//
// WHY A HAND-ROLLED JSON DOM
// The project rule is "no third-party JSON library": PrismCore is header-only
// and every CI lane must compile it in seconds without a fetch. The subset this
// file needs is small but it is NOT a toy parser -- it validates strict JSON
// (escapes, surrogate pairs, number grammar, trailing garbage), keeps numbers as
// their source lexeme so a value we never touched round-trips byte-for-byte, and
// keeps object member order so unknown keys stay exactly where the writer that
// understands them put them.
//
// THE THREE RULES THAT MAKE UPGRADES SURVIVABLE
//   1. schemaVersion is authoritative. Absent => v1 (documents predate the
//      field). Newer than this build => readable but NOT writable: rewriting a
//      future document would silently delete every field we cannot name.
//   2. Migration only ever moves data forward. It never clobbers a value a newer
//      writer already wrote under the new name.
//   3. Unknown keys are preserved verbatim. A build that knows less must not
//      destroy a setting a build that knows more wrote.
//
// The file side (atomic write, %LOCALAPPDATA% placement, corruption recovery)
// lives in Src/PrismWin/SettingsFile.hpp so these rules stay platform-free and
// unit-testable on every CI lane.
//
// CONTRACTS: B1 (parenthesized std::min/max -- this header may be consumed by a
// TU that includes windows.h without NOMINMAX), B2 (fixed-width ints include
// <cstdint> themselves), D2 (assertions cover content, not existence).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism::settings {

// ---------------------------------------------------------------------------
// 1. Minimal JSON value + strict parser + stable serializer.
// ---------------------------------------------------------------------------

enum class JsonType { Null, Bool, Number, String, Array, Object };

inline const char* JsonTypeName(JsonType type) {
  switch (type) {
    case JsonType::Null: return "null";
    case JsonType::Bool: return "bool";
    case JsonType::Number: return "number";
    case JsonType::String: return "string";
    case JsonType::Array: return "array";
    case JsonType::Object: return "object";
  }
  return "?";
}

// One JSON node.
//
// The text field carries two different things on purpose:
//   * String  -> the decoded payload (valid UTF-8)
//   * Number  -> the SOURCE LEXEME ("1.50", "1e2", ...)
// Keeping the lexeme means a number we do not normalise is written back exactly
// as it arrived; a machine-owned "1.50" never turns into "1.5" behind the user.
// Numbers this store creates (defaults, clamps, migrations) get their lexeme
// from FormatJsonNumber, so what we write stays readable.
struct JsonValue {
  JsonType type = JsonType::Null;
  bool boolean = false;
  double number = 0.0;
  std::string text;
  std::vector<JsonValue> items;
  std::vector<std::pair<std::string, JsonValue>> members;

  bool IsObject() const { return type == JsonType::Object; }
  bool IsString() const { return type == JsonType::String; }
  bool IsNumber() const { return type == JsonType::Number; }

  const JsonValue* Find(std::string_view key) const {
    for (const std::pair<std::string, JsonValue>& member : members) {
      if (std::string_view(member.first) == key) return &member.second;
    }
    return nullptr;
  }

  JsonValue* Find(std::string_view key) {
    for (std::pair<std::string, JsonValue>& member : members) {
      if (std::string_view(member.first) == key) return &member.second;
    }
    return nullptr;
  }

  // Sets a key in place when it exists (order is preserved) and appends it
  // otherwise. Unknown keys therefore keep the position the document gave them.
  JsonValue& SetMember(std::string_view key, JsonValue value) {
    if (JsonValue* existing = Find(key)) {
      *existing = std::move(value);
      return *existing;
    }
    members.emplace_back(std::string(key), std::move(value));
    return members.back().second;
  }

  bool EraseMember(std::string_view key) {
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (std::string_view(members[i].first) == key) {
        members.erase(members.begin() + static_cast<std::ptrdiff_t>(i));
        return true;
      }
    }
    return false;
  }

  // Puts an EXISTING member first (schemaVersion reads better on top). A
  // missing key is left missing: callers decide when to create it.
  bool MoveMemberToFront(std::string_view key) {
    for (std::size_t i = 0; i < members.size(); ++i) {
      if (std::string_view(members[i].first) != key) continue;
      if (i == 0) return true;
      std::rotate(members.begin(), members.begin() + static_cast<std::ptrdiff_t>(i),
                  members.begin() + static_cast<std::ptrdiff_t>(i) + 1);
      return true;
    }
    return false;
  }
};

inline JsonValue MakeObject() {
  JsonValue value;
  value.type = JsonType::Object;
  return value;
}

inline JsonValue MakeBool(bool payload) {
  JsonValue value;
  value.type = JsonType::Bool;
  value.boolean = payload;
  return value;
}

inline JsonValue MakeNumber(double payload, std::string_view lexeme = {}) {
  JsonValue value;
  value.type = JsonType::Number;
  value.number = payload;
  value.text.assign(lexeme.data(), lexeme.size());
  return value;
}

inline JsonValue MakeString(std::string payload) {
  JsonValue value;
  value.type = JsonType::String;
  value.text = std::move(payload);
  return value;
}

// Shortest readable form the store ever synthesizes. 15 significant digits
// covers every value we create (schema bounds and percent -> fraction), and it
// keeps the file human-readable. Values that came from disk keep their lexeme
// and never reach this function.
inline std::string FormatJsonNumber(double value) {
  if (!std::isfinite(value)) return "null";  // JSON has no inf/nan
  char buffer[64];
  const int written = std::snprintf(buffer, sizeof buffer, "%.15g", value);
  if (written <= 0) return "0";
  const std::size_t length =
      (std::min)(static_cast<std::size_t>(written), sizeof buffer - 1);
  return std::string(buffer, length);
}

// Parse failure detail. The offset is the byte where the parser gave up and the
// message is the deepest (most specific) complaint, not the last one.
struct JsonParseError {
  bool ok = true;
  std::size_t offset = 0;
  std::string message;
};

namespace detail {

constexpr int kMaxJsonDepth = 64;

inline bool IsJsonDigit(char c) { return c >= '0' && c <= '9'; }

class JsonParser {
 public:
  explicit JsonParser(std::string_view text) : text_(text) {}

  bool Parse(JsonValue& out) {
    SkipWhitespace();
    if (!ParseValue(out, 0)) return false;
    SkipWhitespace();
    if (pos_ != text_.size()) return Fail("trailing characters after the JSON value");
    return true;
  }

  const JsonParseError& error() const { return error_; }

 private:
  bool Fail(const char* message) {
    if (error_.ok) {  // the first failure is the deepest one
      error_.ok = false;
      error_.offset = pos_;
      error_.message = message;
    }
    return false;
  }

  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  bool ParseValue(JsonValue& out, int depth) {
    if (depth > kMaxJsonDepth) return Fail("nesting is deeper than 64 levels");
    SkipWhitespace();
    if (pos_ >= text_.size()) return Fail("unexpected end of input");
    const char c = text_[pos_];
    switch (c) {
      case '{': return ParseObject(out, depth);
      case '[': return ParseArray(out, depth);
      case '"': {
        std::string payload;
        if (!ParseString(payload)) return false;
        out = MakeString(std::move(payload));
        return true;
      }
      case 't': return ParseLiteral("true", MakeBool(true), out);
      case 'f': return ParseLiteral("false", MakeBool(false), out);
      case 'n': return ParseLiteral("null", JsonValue{}, out);
      default:
        if (c == '-' || IsJsonDigit(c)) return ParseNumber(out);
        return Fail("unexpected character where a JSON value was expected");
    }
  }

  bool ParseLiteral(std::string_view literal, JsonValue value, JsonValue& out) {
    if (text_.size() - pos_ < literal.size()) return Fail("truncated literal");
    if (text_.compare(pos_, literal.size(), literal) != 0) return Fail("unknown literal");
    pos_ += literal.size();
    out = std::move(value);
    return true;
  }

  bool ParseObject(JsonValue& out, int depth) {
    ++pos_;  // '{'
    out = MakeObject();
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    for (;;) {
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') return Fail("expected a quoted object key");
      std::string key;
      if (!ParseString(key)) return false;
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':') return Fail("expected ':' after an object key");
      ++pos_;
      JsonValue value;
      if (!ParseValue(value, depth + 1)) return false;
      // RFC 8259 leaves duplicate keys to the reader. Last one wins: the
      // realistic source is two writers racing, where the later write is newer.
      out.SetMember(key, std::move(value));
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}' in an object");
    }
  }

  bool ParseArray(JsonValue& out, int depth) {
    ++pos_;  // '['
    out = JsonValue{};
    out.type = JsonType::Array;
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    for (;;) {
      JsonValue item;
      if (!ParseValue(item, depth + 1)) return false;
      out.items.push_back(std::move(item));
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']' in an array");
    }
  }

  bool ParseString(std::string& out) {
    ++pos_;  // opening quote
    out.clear();
    for (;;) {
      if (pos_ >= text_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return Fail("raw control character inside a string");
      if (c != '\\') {
        out.push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;  // backslash
      if (pos_ >= text_.size()) return Fail("unterminated escape sequence");
      const char escape = text_[pos_++];
      switch (escape) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
          std::uint32_t code = 0;
          if (!ParseHex4(code)) return false;
          if (code >= 0xD800 && code <= 0xDBFF) {
            // A high surrogate MUST be followed by a low one. Dropping an
            // unpaired surrogate silently would corrupt the persisted text.
            if (pos_ + 1 >= text_.size() || text_[pos_] != '\\' || text_[pos_ + 1] != 'u') {
              return Fail("unpaired UTF-16 high surrogate in a \\u escape");
            }
            pos_ += 2;
            std::uint32_t low = 0;
            if (!ParseHex4(low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) {
              return Fail("invalid UTF-16 low surrogate in a \\u escape");
            }
            code = 0x10000u + ((code - 0xD800u) << 10) + (low - 0xDC00u);
          } else if (code >= 0xDC00 && code <= 0xDFFF) {
            return Fail("unpaired UTF-16 low surrogate in a \\u escape");
          }
          AppendUtf8(code, out);
          break;
        }
        default: return Fail("unknown escape character in a string");
      }
    }
  }

  bool ParseHex4(std::uint32_t& out) {
    if (text_.size() - pos_ < 4) return Fail("truncated \\u escape");
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      int digit = 0;
      if (c >= '0' && c <= '9') {
        digit = c - '0';
      } else if (c >= 'a' && c <= 'f') {
        digit = c - 'a' + 10;
      } else if (c >= 'A' && c <= 'F') {
        digit = c - 'A' + 10;
      } else {
        return Fail("non-hex digit in a \\u escape");
      }
      value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    out = value;
    return true;
  }

  static void AppendUtf8(std::uint32_t code, std::string& out) {
    if (code <= 0x7F) {
      out.push_back(static_cast<char>(code));
    } else if (code <= 0x7FF) {
      out.push_back(static_cast<char>(0xC0u | (code >> 6)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else if (code <= 0xFFFF) {
      out.push_back(static_cast<char>(0xE0u | (code >> 12)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    } else {
      out.push_back(static_cast<char>(0xF0u | (code >> 18)));
      out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3Fu)));
      out.push_back(static_cast<char>(0x80u | (code & 0x3Fu)));
    }
  }

  bool ParseNumber(JsonValue& out) {
    const std::size_t start = pos_;
    if (pos_ < text_.size() && text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return Fail("truncated number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && IsJsonDigit(text_[pos_])) ++pos_;
    } else {
      return Fail("a JSON number starts with a digit (or '-' and a digit)");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      if (pos_ >= text_.size() || !IsJsonDigit(text_[pos_])) {
        return Fail("a fraction needs at least one digit");
      }
      while (pos_ < text_.size() && IsJsonDigit(text_[pos_])) ++pos_;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      if (pos_ >= text_.size() || !IsJsonDigit(text_[pos_])) {
        return Fail("an exponent needs at least one digit");
      }
      while (pos_ < text_.size() && IsJsonDigit(text_[pos_])) ++pos_;
    }
    const std::string lexeme(text_.substr(start, pos_ - start));
    char* end = nullptr;
    const double value = std::strtod(lexeme.c_str(), &end);
    if (end == nullptr || static_cast<std::size_t>(end - lexeme.c_str()) != lexeme.size()) {
      return Fail("the number could not be converted");
    }
    if (!std::isfinite(value)) return Fail("the number is outside the representable range");
    out = MakeNumber(value, lexeme);
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  JsonParseError error_;
};

}  // namespace detail

// Parses one complete JSON document. Trailing whitespace is allowed; trailing
// non-whitespace is an error (a settings file must be exactly one document).
inline bool ParseJson(std::string_view text, JsonValue& out, JsonParseError* error = nullptr) {
  detail::JsonParser parser(text);
  const bool ok = parser.Parse(out);
  if (error) *error = parser.error();
  return ok;
}

struct JsonWriteOptions {
  bool pretty = true;
  std::size_t indent = 2;
};

namespace detail {

inline void AppendIndent(const JsonWriteOptions& options, int depth, std::string& out) {
  if (depth <= 0) return;
  out.append(static_cast<std::size_t>(depth) * options.indent, ' ');
}

inline void AppendEscapedString(std::string_view raw, std::string& out) {
  out.push_back('"');
  for (const char ch : raw) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char escaped[7] = {};
          std::snprintf(escaped, sizeof escaped, "\\u%04x", static_cast<unsigned>(c));
          out += escaped;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  out.push_back('"');
}

inline void SerializeInto(const JsonValue& value, const JsonWriteOptions& options, int depth,
                          std::string& out) {
  switch (value.type) {
    case JsonType::Null:
      out += "null";
      return;
    case JsonType::Bool:
      out += value.boolean ? "true" : "false";
      return;
    case JsonType::Number:
      out += value.text.empty() ? FormatJsonNumber(value.number) : value.text;
      return;
    case JsonType::String:
      AppendEscapedString(value.text, out);
      return;
    case JsonType::Array: {
      if (value.items.empty()) {
        out += "[]";
        return;
      }
      out.push_back('[');
      for (std::size_t i = 0; i < value.items.size(); ++i) {
        if (i != 0) out.push_back(',');
        if (options.pretty) {
          out.push_back('\n');
          AppendIndent(options, depth + 1, out);
        }
        SerializeInto(value.items[i], options, depth + 1, out);
      }
      if (options.pretty) {
        out.push_back('\n');
        AppendIndent(options, depth, out);
      }
      out.push_back(']');
      return;
    }
    case JsonType::Object: {
      if (value.members.empty()) {
        out += "{}";
        return;
      }
      out.push_back('{');
      for (std::size_t i = 0; i < value.members.size(); ++i) {
        if (i != 0) out.push_back(',');
        if (options.pretty) {
          out.push_back('\n');
          AppendIndent(options, depth + 1, out);
        }
        AppendEscapedString(value.members[i].first, out);
        out.push_back(':');
        if (options.pretty) out.push_back(' ');
        SerializeInto(value.members[i].second, options, depth + 1, out);
      }
      if (options.pretty) {
        out.push_back('\n');
        AppendIndent(options, depth, out);
      }
      out.push_back('}');
      return;
    }
  }
}

}  // namespace detail

// Stable serialization: members keep their DOM order, numbers keep their lexeme,
// strings are escaped minimally (UTF-8 bytes pass through).
inline std::string SerializeJson(const JsonValue& value, JsonWriteOptions options = {}) {
  std::string out;
  out.reserve(256);
  detail::SerializeInto(value, options, 0, out);
  return out;
}

// Structural equality. Objects compare order-insensitively (JSON objects are
// unordered); numbers compare by value, not by lexeme ("1.50" == "1.5").
inline bool JsonEquals(const JsonValue& a, const JsonValue& b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case JsonType::Null: return true;
    case JsonType::Bool: return a.boolean == b.boolean;
    case JsonType::Number: return a.number == b.number;
    case JsonType::String: return a.text == b.text;
    case JsonType::Array: {
      if (a.items.size() != b.items.size()) return false;
      for (std::size_t i = 0; i < a.items.size(); ++i) {
        if (!JsonEquals(a.items[i], b.items[i])) return false;
      }
      return true;
    }
    case JsonType::Object: {
      if (a.members.size() != b.members.size()) return false;
      for (const std::pair<std::string, JsonValue>& member : a.members) {
        const JsonValue* other = b.Find(member.first);
        if (other == nullptr || !JsonEquals(member.second, *other)) return false;
      }
      return true;
    }
  }
  return false;
}

// ---------------------------------------------------------------------------
// 2. Dotted paths ("appearance.theme").
// ---------------------------------------------------------------------------

// Splits a dotted path. An empty path, a leading/trailing dot or a doubled dot
// is malformed and yields an empty vector (callers treat that as "not found").
inline std::vector<std::string> SplitJsonPath(std::string_view dotted) {
  std::vector<std::string> parts;
  if (dotted.empty()) return parts;
  std::size_t start = 0;
  for (;;) {
    const std::size_t dot = dotted.find('.', start);
    const std::size_t end = dot == std::string_view::npos ? dotted.size() : dot;
    if (end == start) return {};
    parts.emplace_back(dotted.substr(start, end - start));
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return parts;
}

inline const JsonValue* JsonAtPath(const JsonValue& root, std::string_view dotted) {
  const std::vector<std::string> parts = SplitJsonPath(dotted);
  if (parts.empty()) return nullptr;
  const JsonValue* node = &root;
  for (const std::string& part : parts) {
    if (!node->IsObject()) return nullptr;
    node = node->Find(part);
    if (node == nullptr) return nullptr;
  }
  return node;
}

inline JsonValue* JsonAtPath(JsonValue& root, std::string_view dotted) {
  const std::vector<std::string> parts = SplitJsonPath(dotted);
  if (parts.empty()) return nullptr;
  JsonValue* node = &root;
  for (const std::string& part : parts) {
    if (!node->IsObject()) return nullptr;
    node = node->Find(part);
    if (node == nullptr) return nullptr;
  }
  return node;
}

// Truncates on a UTF-8 code-point boundary: cutting a multi-byte character in
// half would put invalid bytes into a file that a UTF-16 consumer must read.
inline std::string TruncateUtf8(std::string_view text, std::size_t maxBytes) {
  if (text.size() <= maxBytes) return std::string(text);
  std::size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) --cut;
  return std::string(text.substr(0, cut));
}

// ---------------------------------------------------------------------------
// 3. The schema: every persisted key, its type, its default and its bounds.
// ---------------------------------------------------------------------------

enum class SettingType { Bool, Int, Number, String, Enum };

struct SettingSpec {
  const char* path;
  SettingType type;
  const char* defaultText;     // decimal for numbers, literal for strings/bools
  int addedInVersion;
  int removedInVersion;        // 0 = live; >0 = migration-only, never written
  double minValue;             // Int/Number inclusive lower bound
  double maxValue;             // Int/Number inclusive upper bound
  const char* const* allowed;  // nullptr-terminated allow-list, nullptr = any
  std::size_t maxLength;       // String/Enum byte cap, 0 = unlimited
};

inline const char* const kThemes[] = {"light", "dark", "system", nullptr};
inline const char* const kOverwriteModes[] = {"ask", "replace", "skip", "rename", nullptr};
inline const char* const kLanguageTags[] = {"auto", "zh-CN", "en-US", "ja-JP", nullptr};

// The live schema, in the order defaults are written. "ui.language" is listed
// ONLY so the v1 -> v2 migration has a name for it; it is never written.
inline const std::vector<SettingSpec>& SettingsSpecs() {
  static const std::vector<SettingSpec> specs = {
      {"appearance.theme", SettingType::Enum, "system", 1, 0, 0.0, 0.0, kThemes, 0},
      {"appearance.glassIntensity", SettingType::Number, "0.72", 2, 0, 0.0, 1.0, nullptr, 0},
      {"appearance.accentColor", SettingType::String, "#4F8CFF", 2, 0, 0.0, 0.0, nullptr, 9},
      {"archive.recentCount", SettingType::Int, "12", 2, 0, 0.0, 50.0, nullptr, 0},
      {"extract.overwrite", SettingType::Enum, "ask", 1, 0, 0.0, 0.0, kOverwriteModes, 0},
      {"extract.closeWhenDone", SettingType::Bool, "false", 1, 0, 0.0, 0.0, nullptr, 0},
      {"extract.verifyAfterExtract", SettingType::Bool, "true", 3, 0, 0.0, 0.0, nullptr, 0},
      {"language.tag", SettingType::String, "auto", 2, 0, 0.0, 0.0, kLanguageTags, 0},
      {"paths.lastExtractDir", SettingType::String, "", 3, 0, 0.0, 0.0, nullptr, 260},
      {"window.width", SettingType::Int, "1200", 1, 0, 640.0, 8192.0, nullptr, 0},
      {"window.height", SettingType::Int, "800", 1, 0, 480.0, 8192.0, nullptr, 0},
      // v1 spelling, renamed by the 1 -> 2 migration. Not written any more.
      {"ui.language", SettingType::String, "auto", 1, 2, 0.0, 0.0, kLanguageTags, 0},
  };
  return specs;
}

inline const SettingSpec* FindSettingSpec(std::string_view path) {
  for (const SettingSpec& spec : SettingsSpecs()) {
    if (std::string_view(spec.path) == path) return &spec;
  }
  return nullptr;
}

inline bool SettingAllowsValue(const SettingSpec& spec, const std::string& value) {
  if (spec.allowed == nullptr) return true;
  for (const char* const* candidate = spec.allowed; *candidate != nullptr; ++candidate) {
    if (value == *candidate) return true;
  }
  return false;
}

inline JsonValue DefaultValueOf(const SettingSpec& spec) {
  switch (spec.type) {
    case SettingType::Bool:
      return MakeBool(std::string_view(spec.defaultText) == "true");
    case SettingType::Int:
    case SettingType::Number: {
      char* end = nullptr;
      const double parsed = std::strtod(spec.defaultText, &end);
      return MakeNumber(parsed, spec.defaultText);
    }
    case SettingType::String:
    case SettingType::Enum:
      return MakeString(spec.defaultText);
  }
  return JsonValue{};
}

// ---------------------------------------------------------------------------
// 4. The store.
// ---------------------------------------------------------------------------

enum class LoadStatus {
  Ok,              // exactly the current schema, nothing repaired
  Defaults,        // a freshly constructed store (no document loaded yet)
  Migrated,        // an older document was carried forward
  FilledDefaults,  // known keys were missing and were defaulted
  Repaired,        // invalid values were corrected (type/range/allow-list/length)
  TooNew,          // written by a newer build: readable, NOT writable
  Corrupt,         // not a settings document; defaults restored
};

inline const char* LoadStatusName(LoadStatus status) {
  switch (status) {
    case LoadStatus::Ok: return "Ok";
    case LoadStatus::Defaults: return "Defaults";
    case LoadStatus::Migrated: return "Migrated";
    case LoadStatus::FilledDefaults: return "FilledDefaults";
    case LoadStatus::Repaired: return "Repaired";
    case LoadStatus::TooNew: return "TooNew";
    case LoadStatus::Corrupt: return "Corrupt";
  }
  return "?";
}

// One applied correction. The kind field is stable vocabulary a gate can assert
// on: "structure" (an intermediate was not an object), "type", "range",
// "not-allowed", "truncated".
struct Repair {
  std::string path;
  std::string kind;
  std::string detail;
};

struct LoadReport {
  LoadStatus status = LoadStatus::Ok;
  bool readable = true;   // false only for Corrupt
  bool writable = true;   // false for TooNew
  int sourceVersion = 0;  // schemaVersion found in the document (0 = absent, i.e. v1)
  int targetVersion = 0;  // kSchemaVersion after the load
  std::string error;      // parse/format failure detail
};

class SettingsStore {
 public:
  static constexpr int kSchemaVersion = 3;

  SettingsStore() { ResetToDefaults(); }

  // Loads a document. A corrupt document is NOT accepted silently: the caller
  // gets status Corrupt plus the parser's byte offset, and the store is left at
  // defaults so the app can still start (the file layer backs the bad bytes up
  // before anything is written).
  LoadReport Load(std::string_view document) {
    repairs_.clear();
    migrations_.clear();
    defaulted_.clear();
    report_ = LoadReport{};

    JsonValue parsed;
    JsonParseError parseError;
    if (!ParseJson(document, parsed, &parseError)) {
      ResetToDefaults();
      report_.status = LoadStatus::Corrupt;
      report_.readable = false;
      report_.error = "invalid JSON at byte " + std::to_string(parseError.offset) + ": " +
                      parseError.message;
      report_.targetVersion = kSchemaVersion;
      return report_;
    }
    if (!parsed.IsObject()) {
      ResetToDefaults();
      report_.status = LoadStatus::Corrupt;
      report_.readable = false;
      report_.error = "the settings document must be a JSON object, got " +
                      std::string(JsonTypeName(parsed.type));
      report_.targetVersion = kSchemaVersion;
      return report_;
    }

    root_ = std::move(parsed);

    // Documents written before the field existed are v1 by definition.
    int version = 1;
    const JsonValue* versionNode = root_.Find("schemaVersion");
    if (versionNode != nullptr) {
      const bool whole = versionNode->type == JsonType::Number &&
                         std::floor(versionNode->number) == versionNode->number;
      if (!whole || versionNode->number < 1.0 || versionNode->number > 1000000.0) {
        ResetToDefaults();
        report_.status = LoadStatus::Corrupt;
        report_.readable = false;
        report_.error = "schemaVersion must be a positive whole number, got " +
                        SerializeJson(*versionNode, JsonWriteOptions{false, 0});
        report_.targetVersion = kSchemaVersion;
        return report_;
      }
      version = static_cast<int>(versionNode->number);
      report_.sourceVersion = version;
    }

    if (version > kSchemaVersion) {
      // A newer build wrote this file. Read it, never rewrite it: rewriting a
      // future document would silently delete every field this build cannot
      // name, which is exactly the data loss schema versioning exists to stop.
      writable_ = false;
      schemaVersion_ = version;
      report_.status = LoadStatus::TooNew;
      report_.targetVersion = version;
      return report_;
    }

    schemaVersion_ = kSchemaVersion;
    if (version < kSchemaVersion) ApplyMigrations(version);
    SetSchemaVersionInDocument();
    Normalize();

    if (!migrations_.empty()) {
      report_.status = LoadStatus::Migrated;
    } else if (!repairs_.empty()) {
      report_.status = LoadStatus::Repaired;
    } else if (!defaulted_.empty()) {
      report_.status = LoadStatus::FilledDefaults;
    } else {
      report_.status = LoadStatus::Ok;
    }
    report_.targetVersion = kSchemaVersion;
    report_.writable = writable_;
    return report_;
  }

  // Fresh store: every live key at its documented default, schemaVersion
  // current. This is also the state a corrupt document falls back to.
  void ResetToDefaults() {
    root_ = MakeObject();
    for (const SettingSpec& spec : SettingsSpecs()) {
      if (spec.removedInVersion != 0) continue;
      JsonValue* leaf = EnsureMutableLeaf(spec.path);
      if (leaf != nullptr) *leaf = DefaultValueOf(spec);
    }
    SetSchemaVersionInDocument();
    schemaVersion_ = kSchemaVersion;
    writable_ = true;
    repairs_.clear();
    migrations_.clear();
    defaulted_.clear();
    report_ = LoadReport{};
    report_.status = LoadStatus::Defaults;
    report_.targetVersion = kSchemaVersion;
  }

  // Serializes the document the app would persist. A read-only (TooNew) store
  // is emitted exactly as parsed so a caller can still show it.
  std::string ToJson(bool pretty = true) const {
    JsonValue out = root_;
    if (writable_) {
      out.SetMember("schemaVersion",
                    MakeNumber(static_cast<double>(kSchemaVersion),
                               std::to_string(kSchemaVersion)));
      out.MoveMemberToFront("schemaVersion");
    }
    JsonWriteOptions options;
    options.pretty = pretty;
    options.indent = 2;
    std::string text = SerializeJson(out, options);
    text.push_back('\n');
    return text;
  }

  const JsonValue& root() const { return root_; }
  JsonValue& mutableRoot() { return root_; }
  int schemaVersion() const { return schemaVersion_; }
  bool writable() const { return writable_; }
  const LoadReport& report() const { return report_; }
  const std::vector<Repair>& repairs() const { return repairs_; }
  const std::vector<std::string>& appliedMigrations() const { return migrations_; }
  const std::vector<std::string>& defaultedPaths() const { return defaulted_; }

  bool Has(std::string_view path) const { return JsonAtPath(root_, path) != nullptr; }

  bool GetBool(std::string_view path, bool fallback) const {
    const JsonValue* node = JsonAtPath(root_, path);
    if (node == nullptr || node->type != JsonType::Bool) return fallback;
    return node->boolean;
  }

  int GetInt(std::string_view path, int fallback) const {
    const JsonValue* node = JsonAtPath(root_, path);
    if (node == nullptr || node->type != JsonType::Number) return fallback;
    // A document this build refused to validate (TooNew) can hold any number;
    // converting an out-of-range double to int is undefined behaviour, so the
    // caller's fallback is the only safe answer.
    const double value = node->number;
    if (value < -2147483648.0 || value > 2147483647.0) return fallback;
    return static_cast<int>(value);
  }

  double GetNumber(std::string_view path, double fallback) const {
    const JsonValue* node = JsonAtPath(root_, path);
    if (node == nullptr || node->type != JsonType::Number) return fallback;
    return node->number;
  }

  std::string GetString(std::string_view path, std::string_view fallback = {}) const {
    const JsonValue* node = JsonAtPath(root_, path);
    if (node == nullptr || node->type != JsonType::String) return std::string(fallback);
    return node->text;
  }

  bool SetBool(std::string_view path, bool value) {
    if (!writable_) return false;
    JsonValue* leaf = EnsureMutableLeaf(path);
    if (leaf == nullptr) return false;
    *leaf = MakeBool(value);
    return true;
  }

  bool SetInt(std::string_view path, int value) {
    if (!writable_) return false;
    JsonValue* leaf = EnsureMutableLeaf(path);
    if (leaf == nullptr) return false;
    *leaf = MakeNumber(static_cast<double>(value), std::to_string(value));
    return true;
  }

  bool SetNumber(std::string_view path, double value) {
    if (!writable_) return false;
    JsonValue* leaf = EnsureMutableLeaf(path);
    if (leaf == nullptr) return false;
    *leaf = MakeNumber(value, FormatJsonNumber(value));
    return true;
  }

  bool SetString(std::string_view path, std::string_view value) {
    if (!writable_) return false;
    JsonValue* leaf = EnsureMutableLeaf(path);
    if (leaf == nullptr) return false;
    *leaf = MakeString(std::string(value));
    return true;
  }

  bool Erase(std::string_view path) {
    if (!writable_) return false;
    const std::vector<std::string> parts = SplitJsonPath(path);
    if (parts.empty()) return false;
    JsonValue* node = &root_;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      if (!node->IsObject()) return false;
      node = node->Find(parts[i]);
      if (node == nullptr) return false;
    }
    if (!node->IsObject()) return false;
    return node->EraseMember(parts.back());
  }

 private:
  // Creates (or repairs) the object chain that holds the dotted path in an
  // arbitrary document, returning the leaf slot. A non-object intermediate is
  // replaced -- Normalize() is the caller that records that repair.
  static JsonValue* EnsureLeafIn(JsonValue& document, std::string_view dotted) {
    const std::vector<std::string> parts = SplitJsonPath(dotted);
    if (parts.empty()) return nullptr;
    JsonValue* node = &document;
    for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
      JsonValue* child = node->Find(parts[i]);
      if (child == nullptr) child = &node->SetMember(parts[i], MakeObject());
      if (!child->IsObject()) *child = MakeObject();
      node = child;
    }
    JsonValue* leaf = node->Find(parts.back());
    if (leaf == nullptr) leaf = &node->SetMember(parts.back(), JsonValue{});
    return leaf;
  }

  JsonValue* EnsureMutableLeaf(std::string_view dotted) {
    return EnsureLeafIn(root_, dotted);
  }

  void SetSchemaVersionInDocument() {
    root_.SetMember("schemaVersion", MakeNumber(static_cast<double>(kSchemaVersion),
                                                std::to_string(kSchemaVersion)));
    root_.MoveMemberToFront("schemaVersion");
  }

  // Carries an older document forward one version at a time, so a v1 file goes
  // through BOTH steps instead of being guessed at.
  void ApplyMigrations(int fromVersion) {
    for (int version = fromVersion; version < kSchemaVersion; ++version) {
#if defined(PRISM_C14_MUTANT_SKIP_MIGRATION)
      // NEGATIVE CONTROL (CONTRACTS D3): every step below is compiled out.
      // The migration cases in Tests/test_settings_store.cpp MUST go red; the
      // CTest entry is WILL_FAIL, so red here is green there. If the macro were
      // misspelled the suite would pass, and THAT is what turns the mutant entry
      // red -- the inversion is the verdict, not this comment.
      migrations_.push_back("skipped " + std::to_string(version) + "->" +
                            std::to_string(version + 1));
#else
      if (version == 1) {
        MigrateOneToTwo();
        migrations_.push_back("1->2: ui.language -> language.tag");
      } else if (version == 2) {
        MigrateTwoToThree();
        migrations_.push_back("2->3: appearance.glassIntensity percent -> fraction");
      }
#endif
    }
  }

  // v1 stored the UI language as the nested "ui": {"language": ...}; v2 moved
  // it to "language.tag" and widened the tag set. The flat spelling is accepted
  // too, because a hand-edited file is a realistic input.
  void MigrateOneToTwo() {
    std::string carried;
    bool haveCarried = false;
    if (JsonValue* ui = root_.Find("ui")) {
      if (ui->IsObject()) {
        if (const JsonValue* legacy = ui->Find("language")) {
          if (legacy->type == JsonType::String) {
            carried = legacy->text;
            haveCarried = true;
          }
        }
      }
    }
    if (!haveCarried) {
      if (const JsonValue* flat = root_.Find("ui.language")) {
        if (flat->type == JsonType::String) {
          carried = flat->text;
          haveCarried = true;
        }
      }
    }
    if (haveCarried && JsonAtPath(root_, "language.tag") == nullptr) {
      // A v2 writer may already have written the new key. It wins: migration
      // moves data forward, it never rolls a newer value back.
      JsonValue* leaf = EnsureMutableLeaf("language.tag");
      if (leaf != nullptr) *leaf = MakeString(carried);
    }
    // The v1 spelling is deleted either way: a v2 file must not keep a key that
    // a later migration would trip over.
    EraseLegacyLanguageSpellings();
  }

  void EraseLegacyLanguageSpellings() {
    root_.EraseMember("ui.language");
    JsonValue* ui = root_.Find("ui");
    if (ui != nullptr && ui->IsObject()) {
      ui->EraseMember("language");
      if (ui->members.empty()) root_.EraseMember("ui");
    }
  }

  // v2 stored the glass intensity as whole percent (0..100); v3 stores the
  // fraction the renderer actually multiplies by. Anything above 1 can only be
  // a percent value, because v3's documented range is [0,1].
  void MigrateTwoToThree() {
    JsonValue* intensity = JsonAtPath(root_, "appearance.glassIntensity");
    if (intensity == nullptr || intensity->type != JsonType::Number) return;
    if (intensity->number > 1.0) {
      const double fraction = intensity->number / 100.0;
      *intensity = MakeNumber(fraction, FormatJsonNumber(fraction));
    }
  }

  // Single pass over the live schema, in schema order:
  //   * absent leaf             -> default, recorded in defaultedPaths()
  //   * non-object intermediate -> rebuilt, recorded as "structure"
  //   * wrong type / bad value  -> repaired, recorded in repairs()
  // Unknown keys are never visited, which is what preserves them.
  void Normalize() {
#if defined(PRISM_C14_MUTANT_DROP_UNKNOWN_KEYS)
    // NEGATIVE CONTROL (CONTRACTS D3): rebuild the document from the live
    // schema only, so every key this build cannot name is lost. The unknown-key
    // cases MUST go red; the CTest entry is WILL_FAIL, so red there is green.
    // A mutation that quietly stops applying would leave the suite green and
    // turn the mutant entry red -- that inversion is the verdict.
    JsonValue pruned = MakeObject();
    if (const JsonValue* version = root_.Find("schemaVersion")) {
      pruned.SetMember("schemaVersion", *version);
    }
    for (const SettingSpec& spec : SettingsSpecs()) {
      if (spec.removedInVersion != 0) continue;
      const JsonValue* leaf = JsonAtPath(root_, spec.path);
      if (leaf == nullptr) continue;
      JsonValue* slot = EnsureLeafIn(pruned, spec.path);
      if (slot != nullptr) *slot = *leaf;
    }
    root_ = std::move(pruned);
#endif
    for (const SettingSpec& spec : SettingsSpecs()) {
      if (spec.removedInVersion != 0) continue;
      const std::vector<std::string> parts = SplitJsonPath(spec.path);
      if (parts.empty()) continue;
      JsonValue* node = &root_;
      for (std::size_t i = 0; i + 1 < parts.size(); ++i) {
        JsonValue* child = node->Find(parts[i]);
        if (child == nullptr) child = &node->SetMember(parts[i], MakeObject());
        if (!child->IsObject()) {
          *child = MakeObject();
          repairs_.push_back({JoinPath(parts, i + 1), "structure",
                              "an object was expected; the subtree was rebuilt"});
        }
        node = child;
      }
      JsonValue* leaf = node->Find(parts.back());
      if (leaf == nullptr) {
        node->SetMember(parts.back(), DefaultValueOf(spec));
        defaulted_.push_back(spec.path);
        continue;
      }
      NormalizeLeaf(spec, *leaf);
    }
  }

  void NormalizeLeaf(const SettingSpec& spec, JsonValue& leaf) {
#if defined(PRISM_C14_MUTANT_IGNORE_VALIDATION)
    // NEGATIVE CONTROL (CONTRACTS D3): every repair below is compiled out. The
    // validation cases MUST go red; WILL_FAIL is the verdict (and a mutation
    // that stopped applying would invert it, which is the point).
    (void)spec;
    (void)leaf;
    return;
#else
    switch (spec.type) {
      case SettingType::Bool: {
        if (leaf.type != JsonType::Bool) {
          repairs_.push_back({spec.path, "type",
                              std::string("expected bool, got ") + JsonTypeName(leaf.type)});
          leaf = DefaultValueOf(spec);
        }
        return;
      }
      case SettingType::Int:
      case SettingType::Number: {
        if (leaf.type != JsonType::Number) {
          repairs_.push_back({spec.path, "type",
                              std::string("expected number, got ") + JsonTypeName(leaf.type)});
          leaf = DefaultValueOf(spec);
          return;
        }
        if (spec.type == SettingType::Int && std::trunc(leaf.number) != leaf.number) {
          const double truncated = std::trunc(leaf.number);
          repairs_.push_back({spec.path, "type", "a whole number was expected"});
          leaf = MakeNumber(truncated, FormatJsonNumber(truncated));
        }
        const double value = leaf.number;
        if (value < spec.minValue) {
          repairs_.push_back({spec.path, "range",
                              "below the documented minimum " + FormatJsonNumber(spec.minValue)});
          leaf = MakeNumber(spec.minValue, FormatJsonNumber(spec.minValue));
        } else if (value > spec.maxValue) {
          repairs_.push_back({spec.path, "range",
                              "above the documented maximum " + FormatJsonNumber(spec.maxValue)});
          leaf = MakeNumber(spec.maxValue, FormatJsonNumber(spec.maxValue));
        }
        return;
      }
      case SettingType::String:
      case SettingType::Enum: {
        if (leaf.type != JsonType::String) {
          repairs_.push_back({spec.path, "type",
                              std::string("expected string, got ") + JsonTypeName(leaf.type)});
          leaf = DefaultValueOf(spec);
          return;
        }
        if (!SettingAllowsValue(spec, leaf.text)) {
          repairs_.push_back({spec.path, "not-allowed",
                              "\"" + leaf.text + "\" is not a documented value"});
          leaf = DefaultValueOf(spec);
          return;
        }
        if (spec.maxLength != 0 && leaf.text.size() > spec.maxLength) {
          const std::string trimmed = TruncateUtf8(leaf.text, spec.maxLength);
          repairs_.push_back({spec.path, "truncated",
                              std::to_string(leaf.text.size()) + " bytes exceed the " +
                                  std::to_string(spec.maxLength) + " byte cap"});
          leaf.text = trimmed;
        }
        return;
      }
    }
#endif
  }

  static std::string JoinPath(const std::vector<std::string>& parts, std::size_t count) {
    std::string joined;
    for (std::size_t i = 0; i < count && i < parts.size(); ++i) {
      if (i != 0) joined.push_back('.');
      joined += parts[i];
    }
    return joined;
  }

  JsonValue root_;
  int schemaVersion_ = kSchemaVersion;
  bool writable_ = true;
  LoadReport report_;
  std::vector<Repair> repairs_;
  std::vector<std::string> migrations_;
  std::vector<std::string> defaulted_;
};

}  // namespace prism::settings
