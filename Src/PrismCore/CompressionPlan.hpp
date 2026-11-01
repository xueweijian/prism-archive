// PrismArchive -- PrismCore
// CompressionPlan: the DECISION half of blueprint ch.2 (create an archive).
//
// Blueprint ch.2.2 documented the IInArchive side of 7z.dll (open / catalog /
// extract) and that half is implemented and heavily tested. The other half --
// IOutArchive::UpdateItems() driven by IArchiveUpdateCallback -- is what turns
// "a bunch of files" into a real .7z/.zip/.tar, and until now PrismArchive had
// no product path for it. This header is the platform-neutral half of that
// path: every choice (format, level, dictionary, solidity, thread count,
// encryption) is decided HERE, so it runs in the Linux lane in milliseconds and
// the Windows layer only marshals decisions into PROPVARIANTs.
//
// Layer rule (HANDOVER section 4): no windows.h, no 7-Zip headers, no I/O.
// Everything below is a pure function of its arguments, including the core
// count (passed in, never queried from the OS).
//
// COST MODEL IS HONEST (docs/ADR.md ADR-002, docs/CONTRACTS.md section E):
// IOutArchive::UpdateItems() never patches an archive in place -- the 7-Zip out
// engine streams EVERY item into a brand-new archive and the caller replaces
// the original. Creating from nothing is therefore exactly one pass over the
// input: input bytes go through the compressor once, and the result is at least
// the size of the archive header. We do not advertise "instant" creation for a
// later append either; the plan says so in CostModel::note and the UI is
// expected to show byte progress from the engine (see ArchiveWriter).
#pragma once

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace prism {

// ---------------------------------------------------------------------------
// Enumerations (operator<< is required: PRISM_CHECK_EQ prints enum values, and
// CONTRACTS B6 makes a missing one a preflight failure).
// ---------------------------------------------------------------------------
enum class ArchiveFormat { SevenZip, Zip, Tar };

// Auto means "the format's honest default": solid for 7z, never for zip/tar.
enum class SolidMode { Off, Auto, On };

enum class Cipher { None, Aes256 };

// Why the engine has to write this many bytes -- the UI labels progress with it.
enum class CostDriver {
  CompressInputOnce,  // create-from-nothing: every input byte is compressed once
  StoreInputOnce,     // level 0 / tar: every input byte is copied verbatim
};

inline std::string_view ToString(ArchiveFormat f) {
  switch (f) {
    case ArchiveFormat::SevenZip: return "7z";
    case ArchiveFormat::Zip: return "zip";
    case ArchiveFormat::Tar: return "tar";
  }
  return "?";
}

inline std::string_view ToString(SolidMode s) {
  switch (s) {
    case SolidMode::Off: return "off";
    case SolidMode::Auto: return "auto";
    case SolidMode::On: return "on";
  }
  return "?";
}

inline std::string_view ToString(Cipher c) { return c == Cipher::Aes256 ? "AES-256" : "none"; }

inline std::string_view ToString(CostDriver d) {
  return d == CostDriver::CompressInputOnce ? "compress-input-once" : "store-input-once";
}

inline std::ostream& operator<<(std::ostream& os, ArchiveFormat f) { return os << ToString(f); }
inline std::ostream& operator<<(std::ostream& os, SolidMode s) { return os << ToString(s); }
inline std::ostream& operator<<(std::ostream& os, Cipher c) { return os << ToString(c); }
inline std::ostream& operator<<(std::ostream& os, CostDriver d) { return os << ToString(d); }

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
struct CompressionOptions {
  ArchiveFormat format = ArchiveFormat::SevenZip;
  // 0 = store, 9 = maximum. Values outside 0..9 are CLAMPED (never rejected):
  // the UI slider can only produce 0..9, so an out-of-range number is a caller
  // bug, and silently writing a 1 GB-dictionary archive because someone passed
  // 99 would be the expensive kind of "fail open".
  int level = 5;
  // 0 = derive from level and from the actual input size (see ResolveDictionary):
  // paying for a 64 MB dictionary to compress 12 KB is not a feature.
  uint64_t dictionaryBytes = 0;
  SolidMode solid = SolidMode::Auto;
  // 0 = auto (== the resolved dictionary, which is 7-Zip's own default) --
  // a solid block larger than the dictionary buys nothing.
  uint64_t solidBlockBytes = 0;
  // 0 = auto (== cpuCores), clamped to [1, 64] (7-Zip's multi-thread ceiling).
  int threads = 0;
  bool encrypt = false;
  // Header encryption hides FILE NAMES. Only 7z can do it; asking for it on zip
  // or tar is an ERROR rather than a silent downgrade: a user who believes the
  // names are hidden must not get a listing that shows them.
  bool encryptFileNames = false;
  // Passed in so this layer stays OS-agnostic; 0 means "unknown".
  int cpuCores = 0;
};

// One source file/folder to be written. path is a UTF-8, '/'-separated path
// RELATIVE to the source root (PrismCore's separator contract, CONTRACTS B4);
// pruning to the Win32 layer is the writer's job.
struct PlanItem {
  std::string path;
  uint64_t size = 0;
  bool isDir = false;
  // FILETIME ticks (100 ns since 1601-01-01) when known; 0 = let the writer
  // substitute a real timestamp from disk. Kept as an integer so PrismCore
  // stays free of windows.h while the value stays byte-exact for 7-Zip.
  uint64_t mtime100ns = 0;
};

// ---------------------------------------------------------------------------
// Outputs
// ---------------------------------------------------------------------------
enum class PropKind { Bool, UInt, Str };

// One ISetProperties() entry, already resolved to the exact name string the
// 7-Zip out handler parses (they are the same short names as the -m switches;
// see Archive/Common/HandlerOut.cpp CMultiMethodProps::SetProperty and
// Archive/7z/7zHandlerOut.cpp COutHandler::SetProperty, pinned 2026-11-01).
struct PropertySetting {
  std::string name;
  PropKind kind = PropKind::Bool;
  bool boolValue = false;
  uint32_t uintValue = 0;
  std::string strValue;
};

struct CostModel {
  CostDriver driver = CostDriver::CompressInputOnce;
  uint64_t inputBytes = 0;
  // Number of times the whole payload crosses the compressor/writer.
  uint32_t writePasses = 1;
  // What the archive can never be smaller than: a stored (level 0) archive
  // holds every input byte verbatim, so inputBytes is a hard lower bound there;
  // for level > 0 there is no useful lower bound and this stays 0.
  uint64_t lowerBoundBytes = 0;
  // Human-readable honesty, surfaced by the UI progress HUD.
  std::string note;
};

struct CompressionPlan {
  bool ok = false;
  std::string error;

  ArchiveFormat format = ArchiveFormat::SevenZip;
  int level = 5;
  // What the engine will actually run: "LZMA2", "Copy", "Deflate", "Store".
  std::string methodName;
  uint64_t dictionaryBytes = 0;
  bool solid = true;
  uint64_t solidBlockBytes = 0;
  int threads = 1;
  Cipher cipher = Cipher::None;
  bool encryptFileNames = false;

  std::vector<PlanItem> items;
  uint64_t fileCount = 0;
  uint64_t dirCount = 0;
  uint64_t inputBytes = 0;

  CostModel cost;
  // Exact ISetProperties() payload, ordered as the engine expects it
  // (solid on/off first, solid block size second).
  std::vector<PropertySetting> properties;
  // Honest, non-fatal downgrades ("zip cannot be solid"). Never used for
  // security-relevant requests -- those fail.
  std::vector<std::string> warnings;
};

// ---------------------------------------------------------------------------
// Pure helpers
// ---------------------------------------------------------------------------
inline int ClampLevel(int level) {
#if defined(PRISM_CP_MUTANT_NO_CLAMP)
  // MUTANT: pass the caller's number straight through (a 99 would become a
  // 1 GB dictionary request). The clamp assertions must go red.
  return level;
#else
  if (level < 0) return 5;   // negative is nonsense, not "store"
  if (level > 9) return 9;
  return level;
#endif
}


// 7-Zip's LZMA2 dictionary ladder, as implemented by this product (we always
// pass "d" explicitly, so the engine obeys this table instead of its own).
// Level 0 is Copy and needs no dictionary at all.
inline uint64_t LevelDictionaryBytes(int clampedLevel) {
  switch (clampedLevel) {
    case 0: return 0;
    case 1: return 256ull << 10;
    case 2: return 1ull << 20;
    case 3: return 4ull << 20;
    case 4: return 16ull << 20;
    case 5: return 32ull << 20;
    case 6: return 64ull << 20;
    case 7: return 128ull << 20;
    case 8: return 256ull << 20;
    default: return 512ull << 20;
  }
}

inline uint64_t NextPowerOfTwo(uint64_t v) {
  if (v <= 1) return 1;
  uint64_t p = 1;
  while (p < v && p < (1ull << 62)) p <<= 1;
  return p;
}

// A dictionary bigger than the payload cannot match anything: it only costs
// allocation. So the honest dictionary is min(level ladder, round-up(input)),
// floored at 4 KiB (LZMA2's own minimum window: ICoder.h kDicLogSizeMin = 12)
// and capped at 1 GiB.
inline uint64_t ResolveDictionary(ArchiveFormat format, int clampedLevel, uint64_t inputBytes) {
  if (format == ArchiveFormat::Tar) return 0;      // tar is a container, not a coder
  if (format == ArchiveFormat::Zip) return 0;      // deflate has no dictionary knob
  if (clampedLevel == 0) return 0;                 // Copy
  const uint64_t ladder = LevelDictionaryBytes(clampedLevel);
#if defined(PRISM_CP_MUTANT_NO_INPUT_CLAMP)
  // MUTANT: ignore how much data there is; always pay for the ladder value.
  // The cast is not decoration: the mutation makes the parameter unused and
  // this tree builds with -Werror -Wextra, so without it the mutant TU stops
  // COMPILING on the Linux lane -- a mutation that fails to build proves
  // nothing (C2's CI is the authority that caught this).
  (void)inputBytes;
  return ladder;
#else
  const uint64_t useful = NextPowerOfTwo(inputBytes == 0 ? 1 : inputBytes);
  uint64_t dict = (std::min)(ladder, useful);
  if (dict < (4ull << 10)) dict = 4ull << 10;
  const uint64_t cap = 1ull << 30;
  if (dict > cap) dict = cap;
  return dict;
#endif
}

inline bool ResolveSolid(ArchiveFormat format, SolidMode mode) {
#if defined(PRISM_CP_MUTANT_SOLID_ANY_FORMAT)
  // MUTANT: honour SolidMode::On for every format, including zip and tar --
  // neither can be solid, so a plan that claims otherwise is a lie the UI
  // would print ("solid" on a zip).
  (void)format;  // see ResolveDictionary: unused parameter under -Werror
  return mode != SolidMode::Off;
#else
  // zip is a per-file container by design, tar is a raw concatenation: neither
  // has a solid block. Only asking for it "Auto" is free (it downgrades to off).
  if (format != ArchiveFormat::SevenZip) return false;
  return mode != SolidMode::Off;
#endif
}

inline uint64_t ResolveSolidBlockBytes(bool solid, uint64_t dictionaryBytes, uint64_t inputBytes) {
  if (!solid) return 0;
  // A solid block larger than the dictionary cannot be compressed as one block;
  // a block larger than the payload is pure bookkeeping.
  uint64_t block = dictionaryBytes != 0 ? dictionaryBytes : (16ull << 20);
  // Never reserve bookkeeping for data that does not exist.
  if (inputBytes != 0) block = (std::min)(block, NextPowerOfTwo(inputBytes));
  return block;
}

inline int ResolveThreads(int requested, int cores) {
#if defined(PRISM_CP_MUTANT_NO_THREAD_CLAMP)
  // MUTANT: believe the caller (0 threads -> the engine picks, 999 -> it tries).
  (void)cores;
  return requested;
#else
  int ceiling = cores > 0 ? cores : 8;
  if (ceiling > 64) ceiling = 64;              // 7-Zip's own multi-thread ceiling
  if (ceiling < 1) ceiling = 1;
  if (requested <= 0) return ceiling;
  return (std::min)(requested, ceiling);
#endif
}


inline std::string_view ExtensionFor(ArchiveFormat format) {
  switch (format) {
    case ArchiveFormat::SevenZip: return ".7z";
    case ArchiveFormat::Zip: return ".zip";
    case ArchiveFormat::Tar: return ".tar";
  }
  return "";
}

// "docs" -> "docs.7z"; "report.tar" stays "report.tar" (already right).
inline std::string DefaultArchiveFileName(std::string_view stem, ArchiveFormat format) {
  std::string out(stem);
  const std::string ext(ExtensionFor(format));
  if (out.size() >= ext.size() && out.compare(out.size() - ext.size(), ext.size(), ext) == 0) {
    return out;
  }
  return out + ext;
}

// Rejects anything that could escape the archive root or cannot be stored.
// Returns false and fills why (never a silent empty path: an empty kpidPath
// makes 7-Zip invent a name).
inline bool SanitizeEntryPath(std::string_view raw, bool isDir, std::string* out,
                              std::string* why) {
  std::string path(raw);
  const char kBackslash = static_cast<char>(92);
  for (char& c : path) {
    if (c == kBackslash) c = '/';
  }
  if (!isDir) {
    // A FILE whose name ends in a separator becomes a directory in some
    // handlers and a slash-named file in others. Refuse instead of guessing.
    if (path.size() > 1 && path.back() == '/') {
      if (why) *why = "file path ends with a separator: " + path;
      return false;
    }
  } else {
    while (path.size() > 1 && path.back() == '/') path.pop_back();
  }
  if (path.empty()) {
    if (why) *why = "empty path";
    return false;
  }
  if (path.front() == '/') {
    if (why) *why = "absolute path: " + path;
    return false;
  }
  if (path.size() >= 2 && path[1] == ':') {
    if (why) *why = "drive-qualified path: " + path;
    return false;
  }
  // Component-wise: no "." / ".." / empty components ("a//b").
  size_t start = 0;
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const size_t end = slash == std::string::npos ? path.size() : slash;
    const std::string comp = path.substr(start, end - start);
    if (comp.empty()) {
      if (why) *why = "empty path component: " + path;
      return false;
    }
    if (comp == "." || comp == "..") {
      if (why) *why = "path traversal component '" + comp + "': " + path;
      return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  if (out) *out = path;
  return true;
}


// ---------------------------------------------------------------------------
// The plan
// ---------------------------------------------------------------------------
inline PropertySetting PropBool(std::string name, bool v) {
  PropertySetting p;
  p.name = std::move(name);
  p.kind = PropKind::Bool;
  p.boolValue = v;
  return p;
}

inline PropertySetting PropUInt(std::string name, uint32_t v) {
  PropertySetting p;
  p.name = std::move(name);
  p.kind = PropKind::UInt;
  p.uintValue = v;
  return p;
}

inline PropertySetting PropStr(std::string name, std::string v) {
  PropertySetting p;
  p.name = std::move(name);
  p.kind = PropKind::Str;
  p.strValue = std::move(v);
  return p;
}

inline CompressionPlan MakeCompressionPlan(const std::vector<PlanItem>& input,
                                          const CompressionOptions& options) {
  CompressionPlan plan;
  plan.format = options.format;
  plan.level = ClampLevel(options.level);

  // ---- gather + validate items -------------------------------------------
  plan.items.reserve(input.size());
  for (const PlanItem& raw : input) {
    std::string clean;
    std::string why;
    if (!SanitizeEntryPath(raw.path, raw.isDir, &clean, &why)) {
      plan.error = "invalid entry path (" + why + ")";
      return plan;
    }
    PlanItem item = raw;
    item.path = clean;
    if (item.isDir) {
      ++plan.dirCount;
    } else {
      ++plan.fileCount;
      plan.inputBytes += item.size;
    }
    plan.items.push_back(std::move(item));
  }
  if (plan.items.empty()) {
    plan.error = "nothing to archive";
    return plan;
  }
  // Deterministic order: the plan is a decision artifact, and 7z reorders solid
  // blocks internally anyway (CONTRACTS A7), so fixing the input order keeps
  // tests and diffs stable without pretending the engine preserves it.
  (std::sort)(plan.items.begin(), plan.items.end(), [](const PlanItem& a, const PlanItem& b) {
    if (a.path != b.path) return a.path < b.path;
    return a.isDir && !b.isDir;
  });

  // ---- security-relevant refusals (never degraded silently) --------------
  if (options.encrypt && plan.format == ArchiveFormat::Tar) {
    // tar has no encryption at all. Answering "ok" here would hand the user an
    // unencrypted archive they asked to be encrypted.
    plan.error = "tar cannot encrypt: no cipher exists in the tar container";
    return plan;
  }
  if (options.encryptFileNames) {
    if (!options.encrypt) {
      plan.error = "encryptFileNames requires encrypt (header encryption without a password)";
      return plan;
    }
    if (plan.format != ArchiveFormat::SevenZip) {
      plan.error = std::string(ToString(plan.format)) +
                   " cannot encrypt file names: only 7z encrypts its header";
      return plan;
    }
  }

  // ---- dictionary / solidity / threads -----------------------------------
  const uint64_t requestedDict = options.dictionaryBytes;
  const uint64_t dictionary =
      requestedDict != 0 ? requestedDict
                         : ResolveDictionary(plan.format, plan.level, plan.inputBytes);
  // A caller-supplied dictionary is still bounded by honesty: cap at 1 GiB and
  // never let zip/tar claim one (they have no such knob; the engine would
  // reject the property and the whole create would fail for a cosmetic reason).
  plan.dictionaryBytes =
      (plan.format == ArchiveFormat::SevenZip) ? (std::min)(dictionary, uint64_t{1} << 30) : 0;

  plan.solid = ResolveSolid(plan.format, options.solid);
  if (!plan.solid && options.solid == SolidMode::On && plan.format != ArchiveFormat::SevenZip) {
    plan.warnings.push_back(std::string(ToString(plan.format)) +
                            " cannot be solid; writing independent entries");
  }
  plan.solidBlockBytes = options.solidBlockBytes != 0
                             ? (plan.solid ? options.solidBlockBytes : 0)
                             : ResolveSolidBlockBytes(plan.solid, plan.dictionaryBytes,
                                                      plan.inputBytes);
  plan.threads = ResolveThreads(options.threads, options.cpuCores);

  plan.cipher = options.encrypt ? Cipher::Aes256 : Cipher::None;
  plan.encryptFileNames = options.encryptFileNames;

  // ---- method name + cost model ------------------------------------------
  if (plan.format == ArchiveFormat::Tar) {
    plan.methodName = "Store";
    plan.level = 0;  // tar is a container: a "level" here would be a lie
    if (options.level != 0) {
      plan.warnings.push_back("tar stores files verbatim; the requested level was ignored");
    }
    plan.cost.driver = CostDriver::StoreInputOnce;
    plan.cost.lowerBoundBytes = plan.inputBytes;
    plan.cost.note =
        "tar concatenates the payload: one pass, " + std::to_string(plan.inputBytes) +
        " payload bytes plus 512-byte headers; no compression, no encryption.";
  } else if (plan.level == 0) {
    plan.methodName = plan.format == ArchiveFormat::SevenZip ? "Copy" : "Store";
    plan.cost.driver = CostDriver::StoreInputOnce;
    plan.cost.lowerBoundBytes = plan.inputBytes;
    plan.cost.note =
        "level 0 stores the payload: the archive holds every input byte verbatim, so it can "
        "never be smaller than " +
        std::to_string(plan.inputBytes) + " bytes.";
  } else {
    plan.methodName = plan.format == ArchiveFormat::SevenZip ? "LZMA2" : "Deflate";
    plan.cost.driver = CostDriver::CompressInputOnce;
    plan.cost.lowerBoundBytes = 0;
    plan.cost.note =
        "UpdateItems compresses every input byte once into a brand-new archive and the caller "
        "replaces the target atomically; it never patches in place (ADR-002), so this is one "
        "full write of " +
        std::to_string(plan.inputBytes) +
        " payload bytes. Progress must come from the engine byte counter, not a timer.";
  }
  plan.cost.inputBytes = plan.inputBytes;
  plan.cost.writePasses = 1;

  // ---- ISetProperties payload, in engine order ---------------------------
  // Names are the -m switch names, verified against the upstream sources
  // (Archive/Common/HandlerOut.cpp, Archive/7z/7zHandlerOut.cpp,
  // Archive/Zip/ZipHandlerOut.cpp) on 2026-11-01.
  if (plan.format == ArchiveFormat::SevenZip || plan.format == ArchiveFormat::Zip) {
    plan.properties.push_back(PropUInt("x", static_cast<uint32_t>(plan.level)));
    plan.properties.push_back(PropUInt("mt", static_cast<uint32_t>(plan.threads)));
  }
  if (plan.format == ArchiveFormat::SevenZip) {
    if (!plan.methodName.empty()) plan.properties.push_back(PropStr("m", plan.methodName));
    if (plan.dictionaryBytes != 0) {
      // "<bytes>b" -- NOT a bare number and not "64m": the method-property
      // parser demands a size STRING with a unit suffix. Measured against the
      // shipped 7z.dll on 2026-11-01: BSTR "16384b" and "16k" are accepted,
      // VT_UI4 16384 and BSTR "16384" are both refused with E_INVALIDARG,
      // which fails the WHOLE create (not just the dictionary). The explicit
      // "b" form is exact for any byte count, so it is what we always send.
      plan.properties.push_back(PropStr("d", std::to_string(plan.dictionaryBytes) + "b"));
    }
    // Solid on/off is a BOOL; the block size rides in the same property name
    // with a num-plus-b string (COutHandler::SetSolidFromString). Order
    // matters: the boolean initialises the solid state, the size refines it.
    plan.properties.push_back(PropBool("s", plan.solid));
    if (plan.solid && plan.solidBlockBytes != 0) {
      plan.properties.push_back(PropStr("s", std::to_string(plan.solidBlockBytes) + "b"));
    }
    if (plan.encryptFileNames) plan.properties.push_back(PropBool("he", true));
  }
  if (plan.format == ArchiveFormat::Zip && options.encrypt) {
    // Zip AES-256 (CHandler::SetProperties in ZipHandlerOut.cpp accepts
    // "AES", "AES192", "AES256"). Without this a password would silently
    // produce legacy ZipCrypto.
    plan.properties.push_back(PropStr("em", "AES256"));
  }

  plan.ok = true;
  return plan;
}

}  // namespace prism
