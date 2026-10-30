// PrismArchive -- PrismCore
// Legacy CJK filename auto-repair (blueprint ch. 7.1 defence #4).
//
// Old ZIPs from JP/KR/TW/HK and pre-Unicode Windows tools ship filenames
// without the UTF-8 flag (general purpose bit 11), so a GBK name read as
// Latin-1 renders as "ÎÄµµ" and a Shift-JIS name as "Êé¡î".
//
// Strategy (deliberately table-free -- no 100 KB mapping tables committed):
//   1. score each candidate encoding by *structural validity* (lead/trail byte
//      ranges) plus a *common-band plausibility* term (GB2312 level-1,
//      Big5 level-1, CP932 kanji/kana, KS X 1001 hangul blocks);
//   2. hand the winner to the platform transcoder (iconv / Win32 codepages).
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32) && defined(__has_include)
#if __has_include(<iconv.h>)
#define PRISM_HAVE_ICONV 1
#endif
#endif

#if defined(PRISM_HAVE_ICONV)
#include <iconv.h>
#include <cerrno>
#endif

namespace prism {

enum class Encoding { Utf8, Gb18030, Big5, ShiftJis, EucKr, Latin1, Unknown };

inline const char* EncodingName(Encoding enc) {
  switch (enc) {
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Gb18030: return "GB18030 (Simplified Chinese)";
    case Encoding::Big5: return "Big5 (Traditional Chinese)";
    case Encoding::ShiftJis: return "Shift-JIS (Japanese)";
    case Encoding::EucKr: return "EUC-KR (Korean)";
    case Encoding::Latin1: return "ISO-8859-1 / Latin-1";
    case Encoding::Unknown: return "unknown";
  }
  return "unknown";
}

// Name understood by iconv / WideCharToMultiByte.
inline const char* EncodingSystemName(Encoding enc) {
  switch (enc) {
    case Encoding::Utf8: return "UTF-8";
    case Encoding::Gb18030: return "GB18030";
    case Encoding::Big5: return "BIG5";
    case Encoding::ShiftJis: return "SHIFT_JIS";
    case Encoding::EucKr: return "EUC-KR";
    case Encoding::Latin1: return "ISO-8859-1";
    case Encoding::Unknown: return "ASCII";
  }
  return "ASCII";
}

inline bool IsUtf8Valid(std::string_view bytes, bool* hasMultibyte = nullptr) {
  size_t i = 0;
  bool multi = false;
  while (i < bytes.size()) {
    const auto c = static_cast<uint8_t>(bytes[i]);
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t extra;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
      extra = 1;
      cp = c & 0x1Fu;
    } else if ((c & 0xF0) == 0xE0) {
      extra = 2;
      cp = c & 0x0Fu;
    } else if ((c & 0xF8) == 0xF0) {
      extra = 3;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (i + extra >= bytes.size()) return false;  // truncated sequence
    multi = true;
    for (size_t k = 1; k <= extra; ++k) {
      const auto cc = static_cast<uint8_t>(bytes[i + k]);
      if ((cc & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (cc & 0x3Fu);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
    if (extra == 1 && cp < 0x80) return false;   // overlong
    if (extra == 2 && cp < 0x800) return false;  // overlong
    if (extra == 3 && cp < 0x10000) return false;
    i += extra + 1;
  }
  if (hasMultibyte) *hasMultibyte = multi;
  return true;
}

namespace detail {

inline bool InRange(uint8_t v, uint8_t lo, uint8_t hi) { return v >= lo && v <= hi; }

inline bool DbcsLead(Encoding enc, uint8_t b) {
  switch (enc) {
    case Encoding::Gb18030: return InRange(b, 0x81, 0xFE);
    case Encoding::Big5: return InRange(b, 0xA1, 0xF9);
    case Encoding::ShiftJis: return InRange(b, 0x81, 0x9F) || InRange(b, 0xE0, 0xFC);
    case Encoding::EucKr: return InRange(b, 0xA1, 0xFE);
    default: return false;
  }
}

inline bool DbcsPairValid(Encoding enc, uint8_t lead, uint8_t trail) {
  (void)lead;  // only GB18030's 4-byte extension would need it
  switch (enc) {
    case Encoding::Gb18030:
      // trail 0x40..0xFE minus 0x7F (GB18030 four-byte extension is ignored)
      return trail != 0x7F && InRange(trail, 0x40, 0xFE);
    case Encoding::Big5:
      return InRange(trail, 0x40, 0x7E) || InRange(trail, 0xA1, 0xFE);
    case Encoding::ShiftJis:
      return trail != 0x7F && InRange(trail, 0x40, 0xFC);
    case Encoding::EucKr:
      return InRange(trail, 0xA1, 0xFE);
    default:
      return false;
  }
}

// "Common" code bands: the high-frequency glyph blocks of each legacy charset.
inline bool DbcsPairCommon(Encoding enc, uint16_t pair) {
  switch (enc) {
    case Encoding::Gb18030:
      return (pair >= 0xA1A1 && pair <= 0xA9FE) ||  // GB2312 symbols
             (pair >= 0xB0A1 && pair <= 0xD7F9);    // GB2312 level-1 hanzi
    case Encoding::Big5:
      return (pair >= 0xA140 && pair <= 0xA3BF) ||  // symbols
             (pair >= 0xA440 && pair <= 0xC67E);    // Big5 level-1 hanzi
    case Encoding::ShiftJis:
      return (pair >= 0x829F && pair <= 0x9FFC);    // kana + kanji
    case Encoding::EucKr:
      return (pair >= 0xA1A1 && pair <= 0xACFE) ||
             (pair >= 0xB0A1 && pair <= 0xC8FE);    // hangul + hanja
    default:
      return false;
  }
}

struct DbcsScore {
  double score = 0.0;
  size_t pairs = 0;
  size_t invalid = 0;
};

inline DbcsScore ScoreDbcs(std::string_view bytes, Encoding enc) {
  DbcsScore out;
  size_t i = 0;
  size_t common = 0;
  while (i < bytes.size()) {
    const auto b = static_cast<uint8_t>(bytes[i]);
    if (b < 0x80) {
      ++i;
      continue;
    }
    if (DbcsLead(enc, b) && i + 1 < bytes.size() &&
        DbcsPairValid(enc, b, static_cast<uint8_t>(bytes[i + 1]))) {
      const uint16_t pair =
          static_cast<uint16_t>((static_cast<uint16_t>(b) << 8) | static_cast<uint8_t>(bytes[i + 1]));
      ++out.pairs;
      if (DbcsPairCommon(enc, pair)) ++common;
      i += 2;
    } else {
      ++out.invalid;
      ++i;  // resync on the next byte
    }
  }
  if (out.pairs == 0) return out;
  if (out.invalid > 0) {
    const double ratio =
        static_cast<double>(out.pairs) / static_cast<double>(out.pairs + out.invalid);
    out.score = 0.30 * ratio;
    return out;
  }
  out.score = 0.40 + 0.45 * (static_cast<double>(common) / static_cast<double>(out.pairs));
  return out;
}

}  // namespace detail

struct Detection {
  Encoding encoding = Encoding::Unknown;
  double score = 0.0;
};

inline std::vector<Detection> DetectEncodings(std::string_view bytes) {
  std::vector<Detection> out;
  bool multi = false;
  const bool utf8Ok = IsUtf8Valid(bytes, &multi);
  if (utf8Ok) out.push_back({Encoding::Utf8, multi ? 0.95 : 0.90});

  for (const Encoding enc : {Encoding::Gb18030, Encoding::Big5, Encoding::ShiftJis,
                             Encoding::EucKr}) {
    const auto s = detail::ScoreDbcs(bytes, enc);
    if (s.pairs > 0) out.push_back({enc, s.score});
  }

  bool hasHigh = false;
  for (const char c : bytes) {
    if (static_cast<uint8_t>(c) >= 0x80) {
      hasHigh = true;
      break;
    }
  }
  if (hasHigh) out.push_back({Encoding::Latin1, 0.25});
  if (out.empty()) out.push_back({Encoding::Utf8, 0.90});  // pure ASCII

  // stable_sort: equal scores must keep a deterministic order so tests and the
  // UI behave identically run to run.
  std::stable_sort(out.begin(), out.end(),
                   [](const Detection& a, const Detection& b) { return a.score > b.score; });
  return out;
}

// True when the runner-up is statistically indistinguishable from the winner.
// Blueprint ch. 7.1 explicitly wants an "encoding switcher" in the UI: when a
// name is ambiguous we auto-pick but tell the UI to offer the alternatives.
inline bool IsAmbiguous(const std::vector<Detection>& ranked) {
  if (ranked.size() < 2) return false;
  return ranked[0].score - ranked[1].score < 0.05;
}

inline Encoding DetectEncoding(std::string_view bytes) {
  const auto all = DetectEncodings(bytes);
  return all.empty() ? Encoding::Unknown : all.front().encoding;
}

// Platform transcoder: legacy byte string -> UTF-8.
class ITranscoder {
 public:
  virtual ~ITranscoder() = default;
  virtual std::optional<std::string> ToUtf8(Encoding from, std::string_view raw) = 0;
  virtual const char* BackendName() const = 0;
};

#if defined(PRISM_HAVE_ICONV)
class IconvTranscoder final : public ITranscoder {
 public:
  std::optional<std::string> ToUtf8(Encoding from, std::string_view raw) override {
    if (from == Encoding::Utf8) return std::string(raw);
    const iconv_t cd = iconv_open("UTF-8", EncodingSystemName(from));
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::nullopt;

    std::string out;
    out.resize(raw.size() * 4 + 16);
    size_t inLeft = raw.size();
    size_t outLeft = out.size();
    char* inPtr = const_cast<char*>(raw.data());
    char* outPtr = out.data();
    const size_t res = iconv(cd, &inPtr, &inLeft, &outPtr, &outLeft);
    iconv_close(cd);
    if (res == static_cast<size_t>(-1)) return std::nullopt;
    out.resize(out.size() - outLeft);
    return out;
  }
  const char* BackendName() const override { return "iconv"; }
};
#endif

// Creates the best available transcoder; nullptr when none is compiled in.
inline std::unique_ptr<ITranscoder> CreateSystemTranscoder() {
#if defined(PRISM_HAVE_ICONV)
  return std::make_unique<IconvTranscoder>();
#else
  return nullptr;  // Windows build supplies DpapiVault-side Win32 transcoder
#endif
}

// Heuristic: a UTF-8 string made of accented Latin-1 letters is almost always
// a legacy CJK name that was decoded with the wrong codepage ("ÎÄµµ", "æ–‡").
inline bool LooksLikeMojibake(std::string_view utf8) {
  size_t accented = 0;
  size_t cjk = 0;
  size_t i = 0;
  while (i < utf8.size()) {
    const auto c = static_cast<uint8_t>(utf8[i]);
    uint32_t cp = c;
    size_t adv = 1;
    if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
      adv = 2;
    } else if ((c & 0xF0) == 0xE0 && i + 2 < utf8.size()) {
      cp = ((c & 0x0Fu) << 12) | ((static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu) << 6) |
           (static_cast<uint8_t>(utf8[i + 2]) & 0x3Fu);
      adv = 3;
    }
    if (cp >= 0xC0 && cp <= 0xFF && cp != 0xD7 && cp != 0xF7) ++accented;
    if (cp >= 0x2E80) ++cjk;
    i += adv;
  }
  return accented >= 2 && cjk == 0;
}

// Undo a Latin-1 misreading: codepoints < 0x100 go back to raw bytes, then the
// detector gets a second chance on the original byte stream.
inline std::string MojibakeToRawBytes(std::string_view utf8) {
  std::string bytes;
  bytes.reserve(utf8.size());
  size_t i = 0;
  while (i < utf8.size()) {
    const auto c = static_cast<uint8_t>(utf8[i]);
    if ((c & 0xE0) == 0xC0 && i + 1 < utf8.size()) {
      const uint32_t cp = ((c & 0x1Fu) << 6) | (static_cast<uint8_t>(utf8[i + 1]) & 0x3Fu);
      if (cp < 0x100) {
        bytes.push_back(static_cast<char>(cp));
        i += 2;
        continue;
      }
    }
    bytes.push_back(static_cast<char>(c));
    ++i;
  }
  return bytes;
}

struct RepairResult {
  std::string name;         // UTF-8 (best effort)
  Encoding encoding = Encoding::Unknown;
  bool repaired = false;    // true when we overrode the naive UTF-8 reading
  double confidence = 0.0;
  bool ambiguous = false;   // runner-up within 0.05 -> offer alternatives in UI
  std::vector<Encoding> alternatives;
};

// Full pipeline used by the engine bridge for every kpidPath value.
// `utf8Flagged` = ZIP general purpose bit 11 / 7z UTF-16 attribute.
inline RepairResult RepairArchiveName(std::string_view rawBytes, bool utf8Flagged,
                                      ITranscoder* transcoder) {
  RepairResult result;
  bool multi = false;
  if (utf8Flagged && IsUtf8Valid(rawBytes, &multi)) {
    result.name = std::string(rawBytes);
    result.encoding = Encoding::Utf8;
    result.confidence = 1.0;
    return result;
  }

  // Case 1: the bytes are already a plausible legacy encoding.
  const auto detections = DetectEncodings(rawBytes);
  Encoding best = detections.empty() ? Encoding::Unknown : detections.front().encoding;
  double score = detections.empty() ? 0.0 : detections.front().score;
  result.ambiguous = IsAmbiguous(detections);
  for (const Detection& d : detections) {
    if (d.encoding != Encoding::Latin1) result.alternatives.push_back(d.encoding);
  }

  if (best != Encoding::Utf8 && best != Encoding::Unknown && best != Encoding::Latin1 &&
      transcoder != nullptr) {
    if (auto decoded = transcoder->ToUtf8(best, rawBytes)) {
      result.name = *decoded;
      result.encoding = best;
      result.repaired = true;
      result.confidence = score;
      return result;
    }
  }

  // Case 2: the caller already handed us UTF-8 text that *looks* like mojibake
  // (a previous layer decoded raw bytes as Latin-1).
  if (IsUtf8Valid(rawBytes) && LooksLikeMojibake(rawBytes)) {
    const std::string raw = MojibakeToRawBytes(rawBytes);
    const auto reDetect = DetectEncodings(raw);
    if (!reDetect.empty() && reDetect.front().encoding != Encoding::Utf8 &&
        transcoder != nullptr) {
      if (auto decoded = transcoder->ToUtf8(reDetect.front().encoding, raw)) {
        result.name = *decoded;
        result.encoding = reDetect.front().encoding;
        result.repaired = true;
        result.confidence = reDetect.front().score;
        return result;
      }
    }
  }

  result.name = std::string(rawBytes);
  result.encoding = Encoding::Utf8;
  result.confidence = score;
  return result;
}

}  // namespace prism
