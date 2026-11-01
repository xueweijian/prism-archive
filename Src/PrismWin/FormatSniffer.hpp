// PrismArchive -- PrismWin
// FormatSniffer: decide which container a file IS from ~64 head bytes plus the
// last 1 KiB, so Open() asks exactly ONE handler instead of probing (card C28).
//
// Why this exists (C7 measurement, C17 cross-check): opening a 4 GiB archive
// cost 78.5 s -- ~55 MB/s of sequential read, i.e. the shape of a whole-file
// scan, not of an index read. The probe list handed every candidate a chance to
// walk the file before it declined. A container's own bytes already say what it
// is, so the routing decision must not be a search.
//
// This header is deliberately PURE: no <windows.h>, no 7-Zip ABI, no I/O. The
// caller fills Window with bytes it read and gets a Finding back, so every rule
// below is unit-testable against a byte array (Tests/win/test_open_perf.cpp).
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace prism::win::sniff {

enum class ArchiveFormat : std::uint8_t {
  kUnknown = 0,
  kSevenZip,  // 7z
  kZip,
  kRar5,
  kRar4,
  kTar,
  kGzip,
  kXz,
  kBzip2,
  kZstd,
  kLz4,
  kBrotli,
  // Card C43: the formats that used to fall through to "probe every handler".
  kIso,       // ISO9660 / UDF bridge: "CD001" at sector 16 (offset 0x8001)
  kCab,       // Microsoft cabinet: "MSCF"
  kWim,       // Windows Imaging Format: "MSWIM\0"
  kCompound,  // OLE compound document: D0CF11E0A1B11AE1 (the .msi container)
  kChm,       // HTML Help: "ITSF"
  kCpio,      // POSIX cpio: "070701" (newc) / "070702" (crc) / "070707" (odc)
  kArj,       // ARJ: 0x60 0xEA
  kLzh,       // LHA/LZH: "-lh5-" at offset 2 of a level-0/1/2 header
  kDmg,       // Apple disk image: "koly" in the LAST 512-byte block
};

// How much the bytes PROVE.
//   kMagic -- the container's own signature. The router may ask a single
//             handler: a wrong signature is a corrupt file, not a different
//             format that another handler would rescue.
//   kHint  -- plausible but not proof. Brotli has no signature at all, and a
//             self-extracting exe carries a zip payload behind an MZ header.
//             The router keeps its probe list for these (CONTRACTS D18: do not
//             claim the fast path for a case the detector cannot prove).
enum class Confidence : std::uint8_t { kNone = 0, kHint, kMagic };

struct Window {
  const std::uint8_t* head = nullptr;  // first bytes of the file
  std::size_t headSize = 0;
  const std::uint8_t* tail = nullptr;  // last bytes of the file
  std::size_t tailSize = 0;
  // Bytes AT file offset 0x8000 (sector 16, 2048-byte sectors). ISO9660 puts
  // its primary volume descriptor there, 32 KiB into the file -- far outside a
  // 512-byte head, so it is a separate, explicitly-offset probe (C43). The
  // caller reads only the few bytes this rule needs.
  const std::uint8_t* sector16 = nullptr;
  std::size_t sector16Size = 0;
  std::uint64_t fileSize = 0;
  // Lowercase extension WITHOUT the dot, empty when the name has none. Used
  // ONLY as a tie-break for formats that carry no signature: a real magic byte
  // always beats the name (a file called .zip that really is a 7z must route to
  // 7z -- that is the exact case the pre-C28 probe got wrong, in the expensive
  // direction).
  std::string_view extensionHint;
};

struct Finding {
  ArchiveFormat format = ArchiveFormat::kUnknown;
  Confidence confidence = Confidence::kNone;
  std::string evidence;  // "<what proved it>", e.g. "head=377abcaf271c" or "ustar@257"

  bool singleHandler() const { return confidence == Confidence::kMagic; }
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
inline bool BytesAre(const std::uint8_t* data, std::size_t size, std::size_t offset,
                     std::initializer_list<std::uint8_t> magic) {
  if (!data || offset > size) return false;
  if (size - offset < magic.size()) return false;
  std::size_t i = 0;
  for (const std::uint8_t expected : magic) {
    if (data[offset + i] != expected) return false;
    ++i;
  }
  return true;
}

inline std::string HexBytes(const std::uint8_t* data, std::size_t count) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(count * 2);
  for (std::size_t i = 0; i < count; ++i) {
    out.push_back(kDigits[(data[i] >> 4) & 0x0F]);
    out.push_back(kDigits[data[i] & 0x0F]);
  }
  return out;
}

inline const char* FormatName(ArchiveFormat format) {
  switch (format) {
    case ArchiveFormat::kSevenZip: return "7z";
    case ArchiveFormat::kZip: return "zip";
    case ArchiveFormat::kRar5: return "rar5";
    case ArchiveFormat::kRar4: return "rar4";
    case ArchiveFormat::kTar: return "tar";
    case ArchiveFormat::kGzip: return "gzip";
    case ArchiveFormat::kXz: return "xz";
    case ArchiveFormat::kBzip2: return "bzip2";
    case ArchiveFormat::kZstd: return "zstd";
    case ArchiveFormat::kLz4: return "lz4";
    case ArchiveFormat::kBrotli: return "br";
    case ArchiveFormat::kIso: return "iso";
    case ArchiveFormat::kCab: return "cab";
    case ArchiveFormat::kWim: return "wim";
    case ArchiveFormat::kCompound: return "compound";
    case ArchiveFormat::kChm: return "chm";
    case ArchiveFormat::kCpio: return "cpio";
    case ArchiveFormat::kArj: return "arj";
    case ArchiveFormat::kLzh: return "lzh";
    case ArchiveFormat::kDmg: return "dmg";
    case ArchiveFormat::kUnknown: break;
  }
  return "unknown";
}

inline const char* ConfidenceName(Confidence confidence) {
  switch (confidence) {
    case Confidence::kMagic: return "magic";
    case Confidence::kHint: return "hint";
    case Confidence::kNone: break;
  }
  return "none";
}

// The extension token 7z.dll's handler table uses for this format (lowercase,
// no dot). Null for formats the bridge has no route for.
inline const char* FormatExtension(ArchiveFormat format) {
  switch (format) {
    case ArchiveFormat::kSevenZip: return "7z";
    case ArchiveFormat::kZip: return "zip";
    case ArchiveFormat::kRar5: return "rar";
    case ArchiveFormat::kRar4: return "rar";
    case ArchiveFormat::kTar: return "tar";
    case ArchiveFormat::kGzip: return "gz";
    case ArchiveFormat::kXz: return "xz";
    case ArchiveFormat::kBzip2: return "bz2";
    case ArchiveFormat::kZstd: return "zst";
    case ArchiveFormat::kLz4: return "lz4";
    case ArchiveFormat::kBrotli: return "br";
    case ArchiveFormat::kIso: return "iso";
    case ArchiveFormat::kCab: return "cab";
    case ArchiveFormat::kWim: return "wim";
    // The engine calls the handler "Compound"; the extension that routes to it
    // in an installer context is .msi (it also claims msp/doc/xls/ppt).
    case ArchiveFormat::kCompound: return "msi";
    case ArchiveFormat::kChm: return "chm";
    case ArchiveFormat::kCpio: return "cpio";
    case ArchiveFormat::kArj: return "arj";
    case ArchiveFormat::kLzh: return "lzh";
    case ArchiveFormat::kDmg: return "dmg";
    case ArchiveFormat::kUnknown: break;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// Brotli: the only listed format with NO signature
// ---------------------------------------------------------------------------
// RFC 7932 §9.2 opens the stream with WBITS. The decoder (c/dec/decode.c,
// DecodeWindowBits) reads it LSB-first:
//   bit0 == 0                  -> 16
//   else n = 3 bits; n != 0    -> 17 + n   (18..24)
//   else n = 3 bits; n == 1    -> large-window marker (invalid in the standard
//                                 stream header, only valid with the
//                                 BROTLI_LARGE_MAX_WINDOW_BITS extension)
//                    n != 0    -> 8 + n    (9..15)
//                    n == 0    -> 17
// Every byte value decodes to SOME window, which is exactly why this is a hint
// and never a magic: it can only ever confirm the name, not replace it.
inline std::optional<unsigned> BrotliWindowBits(std::uint8_t firstByte) {
  const unsigned bits = firstByte;
  if ((bits & 0x01u) == 0) return 16u;
  const unsigned n1 = (bits >> 1) & 0x07u;
  if (n1 != 0) return 17u + n1;
  const unsigned n2 = (bits >> 4) & 0x07u;
  if (n2 == 1u) return std::nullopt;  // large-window marker: not a standard stream
  if (n2 != 0) return 8u + n2;
  return 17u;
}

// ---------------------------------------------------------------------------
// Structural evidence: a zip end-of-central-directory record that TERMINATES
// at EOF, and a tar header at its fixed offset
// ---------------------------------------------------------------------------
// The EOCD must END the file: its 2-byte comment length has to account for
// every remaining byte. Matching the bare signature anywhere in the tail would
// also fire on a .png that happens to contain the four bytes.
inline bool HasZipEndRecord(const Window& window, std::size_t* at = nullptr) {
  // 22 = the fixed EOCD fields before the (variable) comment.
  if (!window.tail || window.tailSize < 22) return false;
  for (std::size_t i = window.tailSize - 22 + 1; i-- > 0;) {
    if (!BytesAre(window.tail, window.tailSize, i, {0x50, 0x4B, 0x05, 0x06})) continue;
    const std::size_t comment = static_cast<std::size_t>(window.tail[i + 20]) |
                                (static_cast<std::size_t>(window.tail[i + 21]) << 8);
    if (i + 22 + comment != window.tailSize) continue;
    if (at) *at = i;
    return true;
  }
  return false;
}

// "MZ" -- a Windows executable. It may still be a self-extracting zip, which is
// why the router does NOT commit to one handler on the strength of an EOCD
// found behind an MZ header.
inline bool LooksLikeExecutable(const Window& window) {
  return BytesAre(window.head, window.headSize, 0, {0x4D, 0x5A});
}

// POSIX tar: "ustar" lives at offset 257 of the first 512-byte header block, in
// every flavour we care about (POSIX ustar, GNU, PAX -- they all keep it).
inline bool HasTarHeader(const Window& window) {
  if (!BytesAre(window.head, window.headSize, 257, {'u', 's', 't', 'a', 'r'})) return false;
  // "ustar\0" (POSIX) or "ustar  " (GNU); reject a text file that merely spells
  // the word at that offset.
  const std::uint8_t next = window.head[262];
  return next == 0x00 || next == 0x20;
}

inline Finding MakeFinding(ArchiveFormat format, Confidence confidence, std::string evidence) {
  Finding finding;
  finding.format = format;
  finding.confidence = confidence;
  finding.evidence = std::move(evidence);
  return finding;
}

// The one decision this header exists for.
inline Finding Sniff(const Window& window) {
  const std::uint8_t* h = window.head;
  const std::size_t n = window.headSize;
  if (!h || n == 0) return MakeFinding(ArchiveFormat::kUnknown, Confidence::kNone, "empty");

  // 1. Signatures. Ordered so the answer never depends on the extension.
  if (BytesAre(h, n, 0, {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C})) {
    return MakeFinding(ArchiveFormat::kSevenZip, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 6 ? n : 6));
  }
  // PK\x03\x04 local header / PK\x05\x06 empty archive / PK\x07\x08 spanned.
  if (BytesAre(h, n, 0, {0x50, 0x4B, 0x03, 0x04}) || BytesAre(h, n, 0, {0x50, 0x4B, 0x05, 0x06}) ||
      BytesAre(h, n, 0, {0x50, 0x4B, 0x07, 0x08})) {
    return MakeFinding(ArchiveFormat::kZip, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 4 ? n : 4));
  }
  if (BytesAre(h, n, 0, {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00})) {
    return MakeFinding(ArchiveFormat::kRar5, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 8 ? n : 8));
  }
  if (BytesAre(h, n, 0, {0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00})) {
    return MakeFinding(ArchiveFormat::kRar4, Confidence::kMagic, "head=" + HexBytes(h, 7));
  }
  if (HasTarHeader(window)) {
    return MakeFinding(ArchiveFormat::kTar, Confidence::kMagic, "ustar@257");
  }
  if (BytesAre(h, n, 0, {0x1F, 0x8B, 0x08})) {
    return MakeFinding(ArchiveFormat::kGzip, Confidence::kMagic, "head=1f8b08");
  }
  if (BytesAre(h, n, 0, {0xFD, 0x37, 0x7A, 0x58, 0x5A, 0x00})) {
    return MakeFinding(ArchiveFormat::kXz, Confidence::kMagic, "head=fd377a585a00");
  }
  // "BZh" + block-size digit 1..9. "BZh0" is not a bzip2 stream.
  if (BytesAre(h, n, 0, {0x42, 0x5A, 0x68}) && h[3] >= '1' && h[3] <= '9') {
    return MakeFinding(ArchiveFormat::kBzip2, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 4 ? n : 4));
  }
  if (BytesAre(h, n, 0, {0x28, 0xB5, 0x2F, 0xFD})) {
    return MakeFinding(ArchiveFormat::kZstd, Confidence::kMagic, "head=28b52ffd");
  }
  // LZ4 frame magic, plus the legacy (pre-1.0) magic that older archives keep.
  if (BytesAre(h, n, 0, {0x04, 0x22, 0x4D, 0x18}) ||
      BytesAre(h, n, 0, {0x02, 0x21, 0x4C, 0x18})) {
    return MakeFinding(ArchiveFormat::kLz4, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 4 ? n : 4));
  }

  // 1b. Card C43: the formats the extension table did not cover. Each one is a
  //     signature the container itself writes, so a hit is proof (kMagic): the
  //     router asks ONE handler and never walks the file looking for another.
  if (BytesAre(h, n, 0, {0x4D, 0x53, 0x43, 0x46})) {  // "MSCF"
    return MakeFinding(ArchiveFormat::kCab, Confidence::kMagic, "head=4d534346");
  }
  // WIM header magic is "MSWIM" followed by two NUL bytes; the first NUL is
  // part of the eight-byte magic and is what keeps "MSWIMX" out.
  if (BytesAre(h, n, 0, {0x4D, 0x53, 0x57, 0x49, 0x4D, 0x00})) {
    return MakeFinding(ArchiveFormat::kWim, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 6 ? n : 6));
  }
  // OLE Compound File Binary: the container of .msi (and .doc/.xls/.ppt).
  if (BytesAre(h, n, 0, {0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1})) {
    return MakeFinding(ArchiveFormat::kCompound, Confidence::kMagic, "head=d0cf11e0a1b11ae1");
  }
  if (BytesAre(h, n, 0, {0x49, 0x54, 0x53, 0x46})) {  // "ITSF"
    return MakeFinding(ArchiveFormat::kChm, Confidence::kMagic, "head=49545346");
  }
  // cpio writes its magic as ASCII: newc "070701", crc "070702", odc "070707".
  if (BytesAre(h, n, 0, {'0', '7', '0', '7', '0', '1'}) ||
      BytesAre(h, n, 0, {'0', '7', '0', '7', '0', '2'}) ||
      BytesAre(h, n, 0, {'0', '7', '0', '7', '0', '7'})) {
    return MakeFinding(ArchiveFormat::kCpio, Confidence::kMagic,
                       "head=" + HexBytes(h, n < 6 ? n : 6));
  }
  if (BytesAre(h, n, 0, {0x60, 0xEA})) {
    return MakeFinding(ArchiveFormat::kArj, Confidence::kMagic, "head=60ea");
  }
  // LHA/LZH level-0/1/2 header: byte 0 is the header size, then "-lh<d>-".
  // Requiring the closing '-' (not just "-lh") is what stops a text file whose
  // third character happens to be '-' from routing here.
  if (n >= 7 && h[2] == '-' && h[3] == 'l' && h[4] == 'h' && h[5] >= '0' && h[5] <= '9' &&
      h[6] == '-') {
    return MakeFinding(ArchiveFormat::kLzh, Confidence::kMagic,
                       "method=" + std::string(reinterpret_cast<const char*>(h + 2), 4));
  }
  // ISO9660: the primary volume descriptor at sector 16 starts with type 0x01
  // and carries "CD001" at +1. A UDF-only image keeps "BEA01" there instead, so
  // this rule fires only on the ISO9660 descriptor.
#ifndef PRISM_C43_MUTANT_NO_ISO_SNIFF
  if (window.sector16 && BytesAre(window.sector16, window.sector16Size, 1,
                                  {'C', 'D', '0', '0', '1'})) {
    return MakeFinding(ArchiveFormat::kIso, Confidence::kMagic, "cd001@32769");
  }
#endif  // PRISM_C43_MUTANT_NO_ISO_SNIFF -- negative control (CONTRACTS D3):
        // the rule is deleted ON PURPOSE so the iso fixture falls back to the
        // extension route + the legacy probe, which walks the whole file.
  // Apple disk image: the "koly" trailer occupies the LAST 512-byte block, so
  // the signature must sit exactly at tailSize-512 -- finding it anywhere else
  // in the tail is a payload coincidence, not a disk image.
  if (window.tail && window.fileSize >= 512 && window.tailSize >= 512) {
    const std::size_t koly = window.tailSize - 512;
    if (BytesAre(window.tail, window.tailSize, koly, {'k', 'o', 'l', 'y'})) {
      return MakeFinding(ArchiveFormat::kDmg, Confidence::kMagic,
                         "koly@" + std::to_string(window.fileSize - 512));
    }
  }

  // 2. Structural evidence, which is a HINT, not proof.
  std::size_t eocd = 0;
  if (HasZipEndRecord(window, &eocd)) {
    // Behind an MZ header this is a self-extracting archive: the zip handler is
    // the most likely owner, but the payload may just as well be a 7z SFX, so
    // the router probes (CONTRACTS D18).
    if (LooksLikeExecutable(window)) {
      return MakeFinding(ArchiveFormat::kZip, Confidence::kHint, "mz+eocd@" + std::to_string(eocd));
    }
    // No PK at offset 0 but a well-formed EOCD at EOF: a zip with a non-zip
    // prefix (installers do this). Still a hint -- the prefix is unexplained.
    return MakeFinding(ArchiveFormat::kZip, Confidence::kHint,
                       "eocd@" + std::to_string(eocd));
  }

  // 3. Formats with no signature: the name may confirm them, nothing else can.
  if (window.extensionHint == "br") {
    if (const std::optional<unsigned> bits = BrotliWindowBits(h[0])) {
      return MakeFinding(ArchiveFormat::kBrotli, Confidence::kHint,
                         "wbits=" + std::to_string(*bits));
    }
  }
  // n may be smaller than the 4 bytes a signature would need (a truncated
  // read): never read past the window.
  return MakeFinding(ArchiveFormat::kUnknown, Confidence::kNone,
                     "head=" + HexBytes(h, n < 4 ? n : 4));
}

// "sniff=zip route=sniffed" -- the diagnostic line Open() puts in its trace.
inline std::string DescribeRoute(const Finding& finding) {
  std::string line = "sniff=";
  line += FormatName(finding.format);
  if (finding.confidence == Confidence::kHint) {
    line += "(hint)";
  }
  line += finding.singleHandler() ? " route=sniffed" : " route=probe";
  return line;
}

}  // namespace prism::win::sniff
