// PrismArchive -- PrismCore
// Quick Look CONTENT plan (blueprint 9.1: Space previews images, PDF,
// Markdown/code and audio/video; ADR-001: system APIs only --
// Windows.Data.Pdf / Media Foundation / WIC / DirectWrite, never WinUI/XAML).
//
// Why a second header next to Preview.hpp: Preview.hpp answers ONE question
// ("is this an image/pdf/text/media?") with five kinds because the gallery and
// the list rows only need a glyph. Quick Look needs three more answers per
// item:
//   * which PROVIDER will produce the pixels/characters,
//   * which SYSTEM CAPABILITY the machine must have for that provider,
//   * and, when nothing can be shown, the sentence the user gets instead of a
//     blank card.
// The old five-way enum is kept untouched (CompanionWindow / Gallery /
// ListLayout / Main.cpp all depend on it); this header ADDS the six content
// classes the blueprint names (Image/Text/Pdf/Audio/Video/Binary) and maps
// back onto PreviewKind so wiring the overlay is a table lookup, not a rewrite.
//
// Everything here is a pure function of (name, head bytes, size, flags):
// no Windows headers, no file system, no 7-Zip -- so the whole decision table
// is unit-testable on the Linux lane (CONTRACTS B1/B2, D3).
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "CharsetRepair.hpp"  // IsUtf8Valid -- one UTF-8 validator, not two
#include "PathSafety.hpp"
#include "Preview.hpp"

namespace prism {

// ---------------------------------------------------------------------------
// The six content classes of blueprint 9.1 (+ Directory, which is structural:
// a folder has no payload, so it never reaches a provider).
// ---------------------------------------------------------------------------
enum class PreviewClass { Image, Text, Pdf, Audio, Video, Binary, Directory };

inline const char* PreviewClassName(PreviewClass klass) {
  switch (klass) {
    case PreviewClass::Image: return "Image";
    case PreviewClass::Text: return "Text";
    case PreviewClass::Pdf: return "PDF";
    case PreviewClass::Audio: return "Audio";
    case PreviewClass::Video: return "Video";
    case PreviewClass::Binary: return "Binary";
    case PreviewClass::Directory: return "Folder";
  }
  return "Binary";
}

inline std::ostream& operator<<(std::ostream& os, PreviewClass klass) {
  return os << PreviewClassName(klass);
}

// Which implementation produces the preview. One provider per class that we
// can actually show today; Video has none yet (see Plan's refusal reason).
enum class PreviewProvider { None, WicImage, TextModel, PdfRender, AudioMetadata };

inline const char* PreviewProviderName(PreviewProvider provider) {
  switch (provider) {
    case PreviewProvider::None: return "none";
    case PreviewProvider::WicImage: return "WIC image decoder";
    case PreviewProvider::TextModel: return "text model";
    case PreviewProvider::PdfRender: return "Windows.Data.Pdf";
    case PreviewProvider::AudioMetadata: return "Media Foundation";
  }
  return "none";
}

inline std::ostream& operator<<(std::ostream& os, PreviewProvider provider) {
  return os << PreviewProviderName(provider);
}

// Why a card cannot show content. None == previewable.
enum class PreviewRefusal {
  None,
  IsDirectory,      // folder: no payload, the shell draws the folder chrome
  UnsupportedType,  // Binary / Video today
  Encrypted,        // needs the password first
  OverBudget,       // the provider cannot be fed safely (whole-file consumers)
  Empty,            // zero bytes: nothing to show, say so
};

inline const char* PreviewRefusalName(PreviewRefusal refusal) {
  switch (refusal) {
    case PreviewRefusal::None: return "none";
    case PreviewRefusal::IsDirectory: return "directory";
    case PreviewRefusal::UnsupportedType: return "unsupported-type";
    case PreviewRefusal::Encrypted: return "encrypted";
    case PreviewRefusal::OverBudget: return "over-budget";
    case PreviewRefusal::Empty: return "empty";
  }
  return "none";
}

inline std::ostream& operator<<(std::ostream& os, PreviewRefusal refusal) {
  return os << PreviewRefusalName(refusal);
}

// System capability bits. A plan names the bits its provider needs; the host
// compares against what it probed at startup (PdfAvailable / AudioAvailable)
// and degrades to the refusal line instead of an empty card.
namespace preview_cap {
constexpr uint32_t kWic = 1u << 0;              // windowscodecs (image + PDF bitmap)
constexpr uint32_t kWindowsDataPdf = 1u << 1;   // Windows.Data.Pdf.PdfDocument
constexpr uint32_t kMediaFoundation = 1u << 2;  // IMFSourceReader metadata
constexpr uint32_t kDirectWrite = 1u << 3;      // text rasterisation of the model
}  // namespace preview_cap

inline bool HasCapability(uint32_t mask, uint32_t bit) { return (mask & bit) != 0; }

// "WIC + Windows.Data.Pdf" -- for the diagnostics line, never parsed back.
inline std::string CapabilityNames(uint32_t mask) {
  std::string out;
  auto add = [&out, mask](uint32_t bit, const char* name) {
    if (!HasCapability(mask, bit)) return;
    if (!out.empty()) out += " + ";
    out += name;
  };
  add(preview_cap::kWic, "WIC");
  add(preview_cap::kWindowsDataPdf, "Windows.Data.Pdf");
  add(preview_cap::kMediaFoundation, "Media Foundation");
  add(preview_cap::kDirectWrite, "DirectWrite");
  return out.empty() ? std::string("none") : out;
}

// ---------------------------------------------------------------------------
// Text: encoding detection + the lined model the renderer draws
// ---------------------------------------------------------------------------
enum class TextEncoding { Utf8, Utf16Le, Utf16Be, Gbk, Binary };

inline const char* TextEncodingName(TextEncoding encoding) {
  switch (encoding) {
    case TextEncoding::Utf8: return "UTF-8";
    case TextEncoding::Utf16Le: return "UTF-16LE";
    case TextEncoding::Utf16Be: return "UTF-16BE";
    case TextEncoding::Gbk: return "GBK/GB18030";
    case TextEncoding::Binary: return "binary";
  }
  return "binary";
}

inline std::ostream& operator<<(std::ostream& os, TextEncoding encoding) {
  return os << TextEncodingName(encoding);
}

namespace preview_detail {

inline bool InByteRange(uint8_t value, uint8_t lo, uint8_t hi) {
  return value >= lo && value <= hi;
}

// Control bytes that a text file legitimately contains.
inline bool IsTextControl(uint8_t value) {
  return value == 0x09 || value == 0x0A || value == 0x0C || value == 0x0D || value == 0x1B;
}

}  // namespace preview_detail

// GBK/GB18030 structural validity (the same lead/trail ranges CharsetRepair
// uses for filenames). Table-free on purpose: committing a 100 KB codepage
// table to "support GBK" is a product decision nobody made.
inline bool IsValidGbk(std::span<const uint8_t> bytes) {
  size_t i = 0;
  while (i < bytes.size()) {
    const uint8_t lead = bytes[i];
    if (lead < 0x80) {
      if (lead < 0x20 && !preview_detail::IsTextControl(lead)) return false;
      if (lead == 0x7F) return false;
      ++i;
      continue;
    }
    if (!preview_detail::InByteRange(lead, 0x81, 0xFE)) return false;
    if (i + 1 >= bytes.size()) return true;  // truncated by the read budget: accept
    const uint8_t trail = bytes[i + 1];
    if (trail == 0x7F || !preview_detail::InByteRange(trail, 0x40, 0xFE)) return false;
    i += 2;
  }
  return true;
}

// Decide how to read the payload. Order matters and is the contract this
// suite pins down (CONTRACTS D2: the DECISION is the assertion, not "it decoded"):
//   1. a BOM outranks every heuristic -- it is a statement by the producer;
//   2. a NUL byte means "not text" (every UTF-16 case without a BOM is
//      indistinguishable from a binary blob, and guessing produces garbage);
//   3. valid UTF-8 wins over GBK (GBK byte pairs are almost never valid UTF-8);
//   4. structurally valid GBK is the legacy-Windows answer;
//   5. everything else is Binary and gets an honest refusal.
inline TextEncoding DetectEncoding(std::span<const uint8_t> bytes) {
  if (bytes.empty()) return TextEncoding::Binary;
#ifdef PRISM_PREVIEW_MUTANT_UTF8_ALWAYS
  // Negative control (CONTRACTS D3): pretend every payload is UTF-8. The GBK
  // and binary fixtures must go red -- that is what proves the encoding
  // decision is actually being asserted.
  return TextEncoding::Utf8;
#endif
  if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF) {
    return TextEncoding::Utf8;
  }
  if (bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xFE) {
    // FF FE 00 00 is UTF-32LE: reject rather than render mojibake.
    if (bytes.size() >= 4 && bytes[2] == 0x00 && bytes[3] == 0x00) return TextEncoding::Binary;
    return TextEncoding::Utf16Le;
  }
  if (bytes.size() >= 2 && bytes[0] == 0xFE && bytes[1] == 0xFF) return TextEncoding::Utf16Be;

  const size_t probe = bytes.size() < 4096 ? bytes.size() : 4096;
  for (size_t i = 0; i < probe; ++i) {
    if (bytes[i] == 0x00) return TextEncoding::Binary;
  }
  if (prism::IsUtf8Valid(
          std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()))) {
    return TextEncoding::Utf8;
  }
  if (IsValidGbk(bytes)) return TextEncoding::Gbk;
  return TextEncoding::Binary;
}

struct TextOptions {
  uint32_t maxBytes = 64u << 10;  // how much of the payload we may read
  uint32_t maxLines = 400;        // lines shown in the card
  uint32_t maxLineChars = 200;    // codepoints per line (0 disables clipping)
};

struct TextLine {
  uint32_t number = 0;  // 1-based, over the WHOLE stream (not the visible slice)
  std::string text;     // UTF-8
  bool clipped = false;
};

struct TextModel {
  TextEncoding encoding = TextEncoding::Binary;
  bool bomStripped = false;
  bool byteLimited = false;  // the payload was longer than maxBytes
  bool lineLimited = false;  // more lines existed than maxLines
  uint32_t clippedLines = 0;
  uint64_t bytesExamined = 0;
  bool transcoderMissing = false;  // GBK with no transcoder: refuse, never mojibake
  std::vector<TextLine> lines;

  std::string Text() const {
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
      if (i != 0) out.push_back('\n');
      out += lines[i].text;
    }
    return out;
  }
};

// GBK -> UTF-8 needs the platform's NLS tables, so the CORE names the encoding
// and the WIN layer injects the transcoder (Win32Transcoder). Pure tests inject
// a stub; that keeps the decision layer OS-free without pretending the core can
// decode a codepage it has no tables for.
using TextTranscoder = std::function<std::string(std::string_view)>;

namespace preview_detail {

// One UTF-8 codepoint width starting at i (1..4; 1 for a malformed lead).
inline size_t Utf8Width(std::string_view text, size_t i) {
  const auto c = static_cast<uint8_t>(text[i]);
  size_t width = 1;
  if ((c & 0xE0) == 0xC0) {
    width = 2;
  } else if ((c & 0xF0) == 0xE0) {
    width = 3;
  } else if ((c & 0xF8) == 0xF0) {
    width = 4;
  }
  if (i + width > text.size()) return 1;
  for (size_t k = 1; k < width; ++k) {
    if ((static_cast<uint8_t>(text[i + k]) & 0xC0) != 0x80) return 1;
  }
  return width;
}

// UTF-16 code units -> UTF-8. Lone surrogates become U+FFFD (a truncated read
// budget can cut a pair in half; that must not lose the rest of the line).
inline std::string Utf16ToUtf8(std::span<const uint8_t> bytes, bool littleEndian) {
  std::string out;
  auto unit = [&](size_t i) -> uint32_t {
    if (littleEndian) {
      return static_cast<uint32_t>(bytes[i]) | (static_cast<uint32_t>(bytes[i + 1]) << 8);
    }
    return (static_cast<uint32_t>(bytes[i]) << 8) | static_cast<uint32_t>(bytes[i + 1]);
  };
  auto append = [&out](uint32_t cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  };
  size_t i = 0;
  while (i + 1 < bytes.size()) {
    const uint32_t first = unit(i);
    if (first >= 0xD800 && first <= 0xDBFF && i + 3 < bytes.size()) {
      const uint32_t second = unit(i + 2);
      if (second >= 0xDC00 && second <= 0xDFFF) {
        append(0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00));
        i += 4;
        continue;
      }
    }
    if (first >= 0xD800 && first <= 0xDFFF) {
      append(0xFFFD);
      i += 2;
      continue;
    }
    append(first);
    i += 2;
  }
  return out;
}

}  // namespace preview_detail

// How many payload bytes a text card is allowed to read. The provider reads
// through the 7-Zip stream (never a temp file) and stops here.
inline uint64_t TextReadBudget(const TextOptions& options) { return options.maxBytes; }

// Build the lined model. The input bytes are the prefix the provider could
// afford (already capped by TextOptions::maxBytes when the provider reads
// through the 7-Zip stream); the model re-caps defensively so the pure tests
// can pass a bigger buffer and still get the same bounded answer.
inline TextModel BuildTextModel(std::span<const uint8_t> bytes, TextEncoding encoding,
                                const TextOptions& options,
                                const TextTranscoder& transcoder = TextTranscoder()) {
  TextModel model;
  model.encoding = encoding;
  if (encoding == TextEncoding::Binary) return model;

  size_t usable = bytes.size();
  if (options.maxBytes != 0 && usable > options.maxBytes) {
    usable = options.maxBytes;
    model.byteLimited = true;
  }
  // Never end on half a code unit / half a DBCS pair: that turns a clean
  // truncation into a mojibake tail.
  if (encoding == TextEncoding::Utf16Le || encoding == TextEncoding::Utf16Be) {
    if (usable >= 2 && (usable % 2) != 0) --usable;
  } else if (encoding == TextEncoding::Gbk && usable < bytes.size() && usable != 0) {
    // The cut may have landed on a lead byte; the last byte of a GBK pair is
    // always a trail (0x40..0xFE). If the byte before the cut is a lead, drop it.
    const uint8_t last = bytes[usable - 1];
    if (preview_detail::InByteRange(last, 0x81, 0xFE)) --usable;
  }
  std::span<const uint8_t> slice(bytes.data(), usable);

  size_t offset = 0;
  if (encoding == TextEncoding::Utf8 && slice.size() >= 3 && slice[0] == 0xEF &&
      slice[1] == 0xBB && slice[2] == 0xBF) {
    offset = 3;
    model.bomStripped = true;
  } else if ((encoding == TextEncoding::Utf16Le || encoding == TextEncoding::Utf16Be) &&
             slice.size() >= 2) {
    offset = 2;  // the BOM decided the encoding
    model.bomStripped = true;
  }
  slice = slice.subspan(offset);
  model.bytesExamined = slice.size();

  std::string utf8;
  switch (encoding) {
    case TextEncoding::Utf8:
      utf8.assign(reinterpret_cast<const char*>(slice.data()), slice.size());
      break;
    case TextEncoding::Utf16Le:
      utf8 = preview_detail::Utf16ToUtf8(slice, true);
      break;
    case TextEncoding::Utf16Be:
      utf8 = preview_detail::Utf16ToUtf8(slice, false);
      break;
    case TextEncoding::Gbk:
      if (!transcoder) {
        // Silent replacement characters are how a preview lies about a file.
        model.transcoderMissing = true;
        return model;
      }
      utf8 = transcoder(
          std::string_view(reinterpret_cast<const char*>(slice.data()), slice.size()));
      break;
    case TextEncoding::Binary:
      return model;
  }

  // Split on CRLF / LF / CR and build the numbered lines.
  size_t lineStart = 0;
  uint32_t lineNumber = 1;
  for (size_t i = 0; i <= utf8.size(); ++i) {
    const bool atEnd = i == utf8.size();
    if (!atEnd && utf8[i] != '\n' && utf8[i] != '\r') continue;
    // A file that ENDS with a newline has no phantom last line: the card would
    // otherwise always show one empty line it invented.
    if (atEnd && lineStart >= utf8.size() && !model.lines.empty()) break;
    if (lineNumber > options.maxLines) {
      model.lineLimited = true;
      break;
    }
    std::string_view raw(utf8.data() + lineStart, i - lineStart);
    TextLine line;
    line.number = lineNumber;
    if (options.maxLineChars != 0) {
      size_t chars = 0;
      size_t cursor = 0;
      while (cursor < raw.size() && chars < options.maxLineChars) {
        cursor += preview_detail::Utf8Width(raw, cursor);
        ++chars;
      }
#ifndef PRISM_PREVIEW_MUTANT_KEEP_LONG_LINES
      if (cursor < raw.size()) {
        line.text.assign(raw.data(), cursor);
        line.clipped = true;
        ++model.clippedLines;
      } else {
        line.text.assign(raw.data(), raw.size());
      }
#else
      // Negative control (CONTRACTS D3): ignore the clip budget. The
      // long-line fixture must go red on line length + clipped count.
      line.text.assign(raw.data(), raw.size());
      (void)cursor;
#endif
    } else {
      line.text.assign(raw.data(), raw.size());
    }
    model.lines.push_back(std::move(line));
    ++lineNumber;
    lineStart = i + 1;
    // CRLF: consume the LF as part of the same break, never as an empty line.
    if (!atEnd && utf8[i] == '\r' && i + 1 < utf8.size() && utf8[i + 1] == '\n') {
      ++i;
      ++lineStart;
    }
  }
  return model;
}

// ---------------------------------------------------------------------------
// WAV header: the pure oracle for the Media Foundation assertion.
// MF reports duration/bitrate/sample rate; the test builds the fixture from
// known values, and this parser proves the fixture really contains them
// (CONTRACTS D2: assert content, not existence).
// ---------------------------------------------------------------------------
struct WavInfo {
  bool ok = false;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  uint32_t sampleRate = 0;
  uint32_t byteRate = 0;
  uint32_t bitrate = 0;        // byteRate * 8
  uint64_t dataBytes = 0;
  uint64_t duration100ns = 0;  // same unit as MF_PD_DURATION
};

inline uint32_t LeU32(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint32_t>(b[at]) | (static_cast<uint32_t>(b[at + 1]) << 8) |
         (static_cast<uint32_t>(b[at + 2]) << 16) | (static_cast<uint32_t>(b[at + 3]) << 24);
}

inline uint16_t LeU16(std::span<const uint8_t> b, size_t at) {
  return static_cast<uint16_t>(static_cast<uint16_t>(b[at]) |
                               (static_cast<uint16_t>(b[at + 1]) << 8));
}

inline WavInfo ParseWavHeader(std::span<const uint8_t> bytes) {
  WavInfo info;
  if (bytes.size() < 12) return info;
  const bool riff = bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F' && bytes[3] == 'F';
  const bool wave = bytes[8] == 'W' && bytes[9] == 'A' && bytes[10] == 'V' && bytes[11] == 'E';
  if (!riff || !wave) return info;
  size_t at = 12;
  bool haveFmt = false;
  uint16_t format = 0;
  while (at + 8 <= bytes.size()) {
    const std::string_view id(reinterpret_cast<const char*>(bytes.data() + at), 4);
    const uint32_t size = LeU32(bytes, at + 4);
    const size_t body = at + 8;
    if (id == "fmt " && body + 16 <= bytes.size()) {
      format = LeU16(bytes, body);
      info.channels = LeU16(bytes, body + 2);
      info.sampleRate = LeU32(bytes, body + 4);
      info.byteRate = LeU32(bytes, body + 8);
      info.bitsPerSample = LeU16(bytes, body + 14);
      haveFmt = true;
    } else if (id == "data") {
      // A streamed WAV may claim 0xFFFFFFFF; clamp to what we actually hold.
      const uint64_t declared = size;
      const uint64_t available = static_cast<uint64_t>(bytes.size() - body);
      info.dataBytes = declared == 0 || declared > available ? available : declared;
      break;
    }
    at = body + size + (size & 1u);  // chunks are word-aligned
  }
  if (!haveFmt || format != 1 /*PCM*/ || info.byteRate == 0) return info;
  info.bitrate = info.byteRate * 8u;
  info.duration100ns = info.dataBytes * 10000000ull / info.byteRate;
  info.ok = true;
  return info;
}

// ---------------------------------------------------------------------------
// Magic sniffing (the extension lies often enough that Quick Look must not
// trust it alone: a ".jpg" that is really a PDF is a normal archive accident).
// ---------------------------------------------------------------------------
enum class MagicKind {
  Unknown, Png, Jpeg, Gif, Bmp, Tiff, Webp, Pdf, Wav, Avi, Ogg, Flac, Mp3, Mp4, Mkv, Zip,
  SevenZip, Rar, Pe, Text
};

inline std::ostream& operator<<(std::ostream& os, MagicKind kind) {
  switch (kind) {
    case MagicKind::Unknown: return os << "Unknown";
    case MagicKind::Png: return os << "Png";
    case MagicKind::Jpeg: return os << "Jpeg";
    case MagicKind::Gif: return os << "Gif";
    case MagicKind::Bmp: return os << "Bmp";
    case MagicKind::Tiff: return os << "Tiff";
    case MagicKind::Webp: return os << "Webp";
    case MagicKind::Pdf: return os << "Pdf";
    case MagicKind::Wav: return os << "Wav";
    case MagicKind::Avi: return os << "Avi";
    case MagicKind::Ogg: return os << "Ogg";
    case MagicKind::Flac: return os << "Flac";
    case MagicKind::Mp3: return os << "Mp3";
    case MagicKind::Mp4: return os << "Mp4";
    case MagicKind::Mkv: return os << "Mkv";
    case MagicKind::Zip: return os << "Zip";
    case MagicKind::SevenZip: return os << "SevenZip";
    case MagicKind::Rar: return os << "Rar";
    case MagicKind::Pe: return os << "Pe";
    case MagicKind::Text: return os << "Text";
  }
  return os << "?";
}

inline MagicKind SniffMagic(std::span<const uint8_t> head) {
  auto at = [&head](size_t i) { return i < head.size() ? head[i] : uint8_t(0); };
  auto tag = [&head](size_t off, const char* four) {
    for (size_t i = 0; i < 4; ++i) {
      if (off + i >= head.size() || head[off + i] != static_cast<uint8_t>(four[i])) return false;
    }
    return true;
  };
  if (head.size() >= 5 && tag(0, "%PDF")) return MagicKind::Pdf;
  if (head.size() >= 4 && at(0) == 0x89 && at(1) == 'P' && at(2) == 'N' && at(3) == 'G') {
    return MagicKind::Png;
  }
  if (head.size() >= 3 && at(0) == 0xFF && at(1) == 0xD8 && at(2) == 0xFF) return MagicKind::Jpeg;
  if (head.size() >= 4 && tag(0, "GIF8")) return MagicKind::Gif;
  if (head.size() >= 2 && at(0) == 'B' && at(1) == 'M') return MagicKind::Bmp;
  if (head.size() >= 4 && ((at(0) == 'I' && at(1) == 'I' && at(2) == 0x2A && at(3) == 0x00) ||
                           (at(0) == 'M' && at(1) == 'M' && at(2) == 0x00 && at(3) == 0x2A))) {
    return MagicKind::Tiff;
  }
  if (head.size() >= 12 && tag(0, "RIFF")) {
    if (tag(8, "WEBP")) return MagicKind::Webp;
    if (tag(8, "WAVE")) return MagicKind::Wav;
    if (tag(8, "AVI ")) return MagicKind::Avi;
  }
  if (head.size() >= 4 && tag(0, "OggS")) return MagicKind::Ogg;
  if (head.size() >= 4 && tag(0, "fLaC")) return MagicKind::Flac;
  if (head.size() >= 3 && tag(0, "ID3")) return MagicKind::Mp3;
  if (head.size() >= 8 && tag(4, "ftyp")) return MagicKind::Mp4;
  if (head.size() >= 4 && at(0) == 0x1A && at(1) == 0x45 && at(2) == 0xDF && at(3) == 0xA3) {
    return MagicKind::Mkv;
  }
  if (head.size() >= 4 && at(0) == 'P' && at(1) == 'K' && (at(2) == 3 || at(2) == 5)) {
    return MagicKind::Zip;
  }
  if (head.size() >= 6 && at(0) == '7' && at(1) == 'z' && at(2) == 0xBC && at(3) == 0xAF) {
    return MagicKind::SevenZip;
  }
  if (head.size() >= 4 && tag(0, "Rar!")) return MagicKind::Rar;
  if (head.size() >= 2 && at(0) == 'M' && at(1) == 'Z') return MagicKind::Pe;
  if (DetectEncoding(head) == TextEncoding::Utf8) return MagicKind::Text;
  return MagicKind::Unknown;
}

inline PreviewClass ClassOfMagic(MagicKind magic) {
  switch (magic) {
    case MagicKind::Png:
    case MagicKind::Jpeg:
    case MagicKind::Gif:
    case MagicKind::Bmp:
    case MagicKind::Tiff:
    case MagicKind::Webp: return PreviewClass::Image;
    case MagicKind::Pdf: return PreviewClass::Pdf;
    case MagicKind::Wav:
    case MagicKind::Ogg:
    case MagicKind::Flac:
    case MagicKind::Mp3: return PreviewClass::Audio;
    case MagicKind::Avi:
    case MagicKind::Mp4:
    case MagicKind::Mkv: return PreviewClass::Video;
    case MagicKind::Zip:
    case MagicKind::SevenZip:
    case MagicKind::Rar:
    case MagicKind::Pe: return PreviewClass::Binary;
    case MagicKind::Text: return PreviewClass::Text;
    case MagicKind::Unknown: return PreviewClass::Binary;
  }
  return PreviewClass::Binary;
}

// ---------------------------------------------------------------------------
// The planner
// ---------------------------------------------------------------------------
struct PreviewBudget {
  uint64_t imageBytes = 32ull << 20;  // WIC decodes whole images: bomb ceiling
  uint64_t textBytes = 64ull << 10;   // characters we actually need for a card
  uint64_t pdfBytes = 16ull << 20;    // Windows.Data.Pdf wants the whole file
  uint64_t audioBytes = 64ull << 20;  // MF reads the memory stream
  uint64_t videoBytes = 64ull << 20;
};

struct PreviewPlan {
  PreviewClass klass = PreviewClass::Binary;
  PreviewProvider provider = PreviewProvider::None;
  uint32_t capabilities = 0;  // preview_cap:: bits the provider needs
  uint64_t byteBudget = 0;    // payload bytes the provider may read
  bool bounded = false;       // payload is bigger than byteBudget
  bool exceedsLimit = false;  // mirrors QuickLookPlanner::Decision::exceedsLimit
  PreviewRefusal refusal = PreviewRefusal::None;
  std::string reason;  // user-facing sentence; empty iff previewable()

  bool previewable() const { return provider != PreviewProvider::None; }
  // Back-compat bridge for the overlay/list rows that still speak PreviewKind.
  PreviewKind Kind() const {
    switch (klass) {
      case PreviewClass::Image: return PreviewKind::Image;
      case PreviewClass::Text: return PreviewKind::Text;
      case PreviewClass::Pdf: return PreviewKind::Pdf;
      case PreviewClass::Audio:
      case PreviewClass::Video: return PreviewKind::Media;
      case PreviewClass::Directory: return PreviewKind::Directory;
      case PreviewClass::Binary: return PreviewKind::None;
    }
    return PreviewKind::None;
  }
};

inline std::ostream& operator<<(std::ostream& os, const PreviewPlan& plan) {
  return os << PreviewClassName(plan.klass) << "/" << plan.provider << "/"
            << PreviewRefusalName(plan.refusal) << "/budget=" << plan.byteBudget
            << "/reason=" << (plan.reason.empty() ? "-" : plan.reason.c_str());
}

class PreviewPlanner {
 public:
  // Known binary payloads: never text-sniff these even when the head happens to
  // be printable (a .exe's DOS stub is "MZ" + ASCII, and showing it as text is
  // a lie about the file).
  static bool IsKnownBinaryExtension(std::string_view path) {
    const std::string ext = ExtensionOf(path);
    return ext == ".exe" || ext == ".dll" || ext == ".sys" || ext == ".so" || ext == ".dylib" ||
           ext == ".7z" || ext == ".zip" || ext == ".rar" || ext == ".tar" || ext == ".gz" ||
           ext == ".bz2" || ext == ".xz" || ext == ".zst" || ext == ".cab" || ext == ".iso" ||
           ext == ".msi" || ext == ".dmg" || ext == ".pkg" || ext == ".deb" || ext == ".rpm" ||
           ext == ".class" || ext == ".jar" || ext == ".pyc" || ext == ".o" || ext == ".a" ||
           ext == ".lib" || ext == ".obj" || ext == ".pdb" || ext == ".bin" || ext == ".dat" ||
           ext == ".db" || ext == ".sqlite" || ext == ".pak" || ext == ".wad" || ext == ".ttf" ||
           ext == ".otf" || ext == ".woff" || ext == ".woff2";
  }

  static PreviewClass ClassifyName(std::string_view path) {
    const std::string name = ToLower(std::string(FileNameOf(NormalizeArchivePath(path))));
    const std::string ext = ExtensionOf(path);  // ".ext", lowercased
    auto oneOf = [&ext](std::initializer_list<const char*> list) {
      for (const char* candidate : list) {
        if (ext == candidate) return true;
      }
      return false;
    };

    if (oneOf({".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".avif", ".heic", ".heif",
               ".svg", ".psd", ".tif", ".tiff", ".ico", ".jxl", ".cr2", ".nef", ".arw",
               ".dng"})) {
      return PreviewClass::Image;
    }
    if (ext == ".pdf") return PreviewClass::Pdf;
    if (oneOf({".mp3", ".wav", ".flac", ".ogg", ".oga", ".m4a", ".aac", ".wma", ".opus", ".ape",
               ".aiff", ".aif", ".mid", ".midi", ".amr", ".ac3", ".dts", ".mka"})) {
      return PreviewClass::Audio;
    }
    if (oneOf({".mp4", ".mov", ".mkv", ".avi", ".webm", ".wmv", ".flv", ".m4v", ".mpg", ".mpeg",
               ".ts", ".m2ts", ".3gp", ".ogv"})) {
      return PreviewClass::Video;
    }
    if (oneOf({".txt", ".md", ".markdown", ".rst", ".adoc", ".log", ".csv", ".tsv", ".json",
               ".jsonc", ".xml", ".xsd", ".yaml", ".yml", ".toml", ".ini", ".cfg", ".conf",
               ".env", ".properties", ".gradle", ".cmake", ".mk", ".make", ".diff", ".patch",
               ".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".hh", ".hxx", ".m", ".mm", ".cs",
               ".java", ".kt", ".kts", ".swift", ".go", ".rs", ".rb", ".py", ".pyi", ".pl",
               ".lua", ".php", ".js", ".mjs", ".cjs", ".ts", ".tsx", ".jsx", ".vue", ".svelte",
               ".sh", ".bash", ".zsh", ".fish", ".bat", ".cmd", ".ps1", ".psm1", ".sql", ".html",
               ".htm", ".css", ".scss", ".sass", ".less", ".tex", ".bib", ".asm", ".s", ".v",
               ".sv", ".vhd", ".url", ".nfo", ".sub", ".srt", ".vtt", ".desktop"})) {
      return PreviewClass::Text;
    }
    // Extension-less files that are text by convention (Makefile, LICENSE...).
    if (ext.empty()) {
      if (name == "makefile" || name == "gnumakefile" || name == "dockerfile" ||
          name == "containerfile" || name == "rakefile" || name == "gemfile" ||
          name == "vagrantfile" || name == "procfile" || name == "license" ||
          name == "licence" || name == "readme" || name == "changelog" || name == "authors" ||
          name == "copying" || name == "notice" || name == "todo" || name == "install" ||
          name == "configure") {
        return PreviewClass::Text;
      }
    }
    return PreviewClass::Binary;
  }

  // Name first, magic second: magic only ADDS information.
  //   * a container magic (PDF/image/WAV/AVI/MP4/MKV) outranks a name, because
  //     the bytes are what the decoder will actually see;
  //   * a text sniff only upgrades a name that never claimed to be text, and
  //     never a known binary extension (.exe stays binary).
  static PreviewClass Classify(std::string_view path, std::span<const uint8_t> head = {}) {
    const PreviewClass byName = ClassifyName(path);
    if (head.empty()) return byName;
    const PreviewClass byMagic = ClassOfMagic(SniffMagic(head));
    if (byMagic == PreviewClass::Binary) return byName;
    if (byMagic == PreviewClass::Text) {
      if (byName == PreviewClass::Text) return PreviewClass::Text;
      if (byName == PreviewClass::Binary && !IsKnownBinaryExtension(path)) {
        return PreviewClass::Text;
      }
      return byName;
    }
    // Strong container magic: trust the bytes over the extension.
    return byMagic;
  }

  static PreviewBudget DefaultBudget() { return PreviewBudget(); }

  static PreviewPlan Plan(const ArchiveItem& item, std::span<const uint8_t> head = {},
                          const PreviewBudget& budget = PreviewBudget()) {
    PreviewPlan plan;
    if (item.isDir) {
      plan.klass = PreviewClass::Directory;
      plan.refusal = PreviewRefusal::IsDirectory;
      plan.reason = "Folder: nothing to preview";
      return plan;
    }
    plan.klass = Classify(item.path, head);

    // Capability + budget per class. This table IS the product decision the
    // blueprint asks for, and the tests assert it verbatim.
    switch (plan.klass) {
      case PreviewClass::Image:
        plan.provider = PreviewProvider::WicImage;
        plan.capabilities = preview_cap::kWic;
        plan.byteBudget = budget.imageBytes;
        break;
      case PreviewClass::Text:
        plan.provider = PreviewProvider::TextModel;
        plan.capabilities = preview_cap::kDirectWrite;  // the model is drawn
        plan.byteBudget = budget.textBytes;
        break;
      case PreviewClass::Pdf:
        plan.provider = PreviewProvider::PdfRender;
        plan.capabilities = preview_cap::kWindowsDataPdf | preview_cap::kWic;
        plan.byteBudget = budget.pdfBytes;
        break;
      case PreviewClass::Audio:
        plan.provider = PreviewProvider::AudioMetadata;
        plan.capabilities = preview_cap::kMediaFoundation;
        plan.byteBudget = budget.audioBytes;
        break;
      case PreviewClass::Video:
        plan.provider = PreviewProvider::None;
        plan.capabilities = preview_cap::kMediaFoundation;
        plan.byteBudget = budget.videoBytes;
        plan.refusal = PreviewRefusal::UnsupportedType;
        plan.reason = "Video preview is not wired yet (audio metadata only)";
        break;
      case PreviewClass::Binary:
        plan.provider = PreviewProvider::None;
        plan.refusal = PreviewRefusal::UnsupportedType;
        plan.reason = "No preview for binary files";
        break;
      case PreviewClass::Directory:
        plan.refusal = PreviewRefusal::IsDirectory;
        plan.reason = "Folder: nothing to preview";
        break;
    }

    if (item.encrypted) {
      plan.provider = PreviewProvider::None;
      plan.refusal = PreviewRefusal::Encrypted;
      plan.reason = "Encrypted entry: enter the password to preview";
      return plan;
    }
    if (plan.provider == PreviewProvider::None) return plan;

    plan.exceedsLimit = item.size > plan.byteBudget;
    plan.bounded = plan.exceedsLimit;
    if (item.size == 0) {
      plan.provider = PreviewProvider::None;
      plan.refusal = PreviewRefusal::Empty;
      plan.reason = "Empty file";
      return plan;
    }
    // The text card never needs the whole file, so a 900 MB log is fine. The
    // other three providers consume their payload whole: feeding them a slice
    // produces a corrupt render, so they refuse loudly instead of showing a
    // half-decoded page.
    if (plan.klass != PreviewClass::Text && plan.exceedsLimit) {
      plan.provider = PreviewProvider::None;
      plan.refusal = PreviewRefusal::OverBudget;
      plan.reason = std::string("Too large to preview (") + PreviewClassName(plan.klass) +
                    " > " + std::to_string(plan.byteBudget >> 20) + " MiB)";
      return plan;
    }
    return plan;
  }
};

}  // namespace prism
