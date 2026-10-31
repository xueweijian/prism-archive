// PrismArchive -- PrismCore
// Zero-temp-copy virtual drag & drop: the decision core (blueprint ch. 4.4 / 8.1).
//
// 7-Zip's original behaviour writes the dragged payload into %TEMP% first and
// then lets Explorer copy it to the drop folder: a 4 GB video crosses the disk
// twice and dies outright when C: has less than 4 GB free. PrismArchive picks
// one of three transports *before* a single byte is written:
//
//   DirectToPath      -- the drop landed on a file-manager window whose folder
//                        we reverse-resolved to a real physical path: 7z.dll
//                        streams straight into that folder (zero temp files).
//   MemoryStream      -- the target is an application (WeChat, Photoshop, a
//                        browser upload box): CFSTR_FILEDESCRIPTORW +
//                        CFSTR_FILECONTENTS, decompressed on demand.
//   TempCopyFallback  -- we could not prove either zero-copy route is safe.
//                        This is a *refusal*, and it always names its reason.
//
// Everything in this file is platform-neutral: no <windows.h>, no 7-Zip
// headers, no filesystem access. That is what makes the rules testable on
// Linux in seconds (Tests/test_drag_drop.cpp), including a mutation build that
// must go red if the rules ever stop biting (Tests/CMakeLists.txt).
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "PathSafety.hpp"

namespace prism::dnd {

// ---------------------------------------------------------------------------
// Negative-control hook (mutation testing, see Tests/CMakeLists.txt).
//
// The suite is compiled twice: once clean, once with a rule deliberately
// broken (kMutantTrustAll / kMutantIgnoreSpace). The broken binary is
// registered with WILL_FAIL, so if a mutation ever survives -- i.e. the tests
// have stopped observing that rule -- CTest turns red. Without this, a test
// suite can quietly become a rubber stamp.
// ---------------------------------------------------------------------------
namespace mutation {
#if defined(PRISM_DND_MUTANT_TRUST_ALL)
constexpr bool kTrustAll = true;
#else
constexpr bool kTrustAll = false;
#endif
#if defined(PRISM_DND_MUTANT_IGNORE_SPACE)
constexpr bool kIgnoreSpace = true;
#else
constexpr bool kIgnoreSpace = false;
#endif
}  // namespace mutation

// ---------------------------------------------------------------------------
// Transport channel
// ---------------------------------------------------------------------------
enum class Channel : uint8_t {
  DirectToPath,     // channel A: 7z.dll writes into the resolved drop folder
  MemoryStream,     // channel B: CFSTR_FILECONTENTS, lazy IStream
  TempCopyFallback  // %TEMP% then shell copy -- only with a named reason
};

inline const char* ToString(Channel c) {
  switch (c) {
    case Channel::DirectToPath: return "DirectToPath";
    case Channel::MemoryStream: return "MemoryStream";
    case Channel::TempCopyFallback: return "TempCopyFallback";
  }
  return "Unknown";
}

// PRISM_CHECK_EQ prints operands on failure; without these stream inserters a
// red mutant build dies with a compile error instead of a readable diff.
inline std::ostream& operator<<(std::ostream& os, Channel c) { return os << ToString(c); }

// Why the *better* channel was refused. TempCopyFallback must always carry one:
// a silent downgrade is exactly the 7-Zip behaviour this module exists to kill,
// and a silent downgrade is also undebuggable from a user's crash report.
enum class DenyReason : uint8_t {
  None = 0,
  NotAFileManager,          // drop target is an app window, not a folder view
  EmptySelection,           // nothing to hand over (caller bug)
  PasswordNotResolved,      // encrypted and the keychain has not hit yet
  PathEmpty,                // file manager, but the folder lookup came back empty
  PathRelative,             // no drive root -> cannot name a destination
  PathUnc,                  // \\server\share: network redirector, not a local disk
  PathDeviceNamespace,      // \\.\COM1 and friends: writing there is destructive
  PathExtendedMixedSep,     // \\?\C:/x -- the prefix disables '/'-normalisation
  PathShellNamespace,       // "::{20D04FE0-...}" / "This PC": no physical path
  PathTraversal,            // a '..' component survived normalisation
  PathReservedChars,        // < > : " | ? * or a control byte
  PathTooLong,              // longer than MAX_PATH without an extended prefix
  DiskSpaceUnknown,         // free space unreadable -> cannot prove it fits
  DiskSpaceInsufficient,    // destination cannot hold the payload
  TargetRejectsStream,      // no IStream and no CF_HDROP: nothing to deliver
  NoDeliverableFormat       // not even CF_HDROP works
};

inline const char* ToString(DenyReason r) {
  switch (r) {
    case DenyReason::None: return "None";
    case DenyReason::NotAFileManager: return "NotAFileManager";
    case DenyReason::EmptySelection: return "EmptySelection";
    case DenyReason::PasswordNotResolved: return "PasswordNotResolved";
    case DenyReason::PathEmpty: return "PathEmpty";
    case DenyReason::PathRelative: return "PathRelative";
    case DenyReason::PathUnc: return "PathUnc";
    case DenyReason::PathDeviceNamespace: return "PathDeviceNamespace";
    case DenyReason::PathExtendedMixedSep: return "PathExtendedMixedSep";
    case DenyReason::PathShellNamespace: return "PathShellNamespace";
    case DenyReason::PathTraversal: return "PathTraversal";
    case DenyReason::PathReservedChars: return "PathReservedChars";
    case DenyReason::PathTooLong: return "PathTooLong";
    case DenyReason::DiskSpaceUnknown: return "DiskSpaceUnknown";
    case DenyReason::DiskSpaceInsufficient: return "DiskSpaceInsufficient";
    case DenyReason::TargetRejectsStream: return "TargetRejectsStream";
    case DenyReason::NoDeliverableFormat: return "NoDeliverableFormat";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, DenyReason r) { return os << ToString(r); }

// ---------------------------------------------------------------------------
// UTF-16 length, computed from UTF-8 without a converter.
//
// FILEGROUPDESCRIPTORW::cFileName is WCHAR[MAX_PATH]: the shell measures names
// in UTF-16 code units, not in bytes and not in code points. A Japanese name of
// 100 characters is 300 UTF-8 bytes but 100 units, while an emoji of one
// character is 4 bytes and 2 units. Comparing byte lengths (the obvious wrong
// move) both truncates legitimate CJK names and lets surrogate pairs overflow.
// ---------------------------------------------------------------------------
inline size_t Utf16CodeUnits(std::string_view utf8) {
  size_t units = 0;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    uint32_t cp = c;
    size_t len = 1;
    if (c >= 0xF0) {
      len = 4;
      cp = static_cast<uint32_t>(c & 0x07);
    } else if (c >= 0xE0) {
      len = 3;
      cp = static_cast<uint32_t>(c & 0x0F);
    } else if (c >= 0xC0) {
      len = 2;
      cp = static_cast<uint32_t>(c & 0x1F);
    }
    if (i + len > utf8.size()) {  // truncated tail: count the byte as one unit
      ++units;
      break;
    }
    for (size_t k = 1; k < len; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    }
    units += (cp >= 0x10000) ? 2 : 1;  // surrogate pair
    i += len;
  }
  return units;
}

// Drop whole code points from the end until the string fits `maxUnits`.
inline std::string TruncateUtf16(std::string_view utf8, size_t maxUnits) {
  if (Utf16CodeUnits(utf8) <= maxUnits) return std::string(utf8);
  std::string out;
  size_t units = 0;
  for (size_t i = 0; i < utf8.size();) {
    const unsigned char c = static_cast<unsigned char>(utf8[i]);
    size_t len = 1;
    uint32_t cp = c;
    if (c >= 0xF0) {
      len = 4;
      cp = static_cast<uint32_t>(c & 0x07);
    } else if (c >= 0xE0) {
      len = 3;
      cp = static_cast<uint32_t>(c & 0x0F);
    } else if (c >= 0xC0) {
      len = 2;
      cp = static_cast<uint32_t>(c & 0x1F);
    }
    if (i + len > utf8.size()) break;
    for (size_t k = 1; k < len; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3F);
    }
    const size_t width = (cp >= 0x10000) ? 2 : 1;
    if (units + width > maxUnits) break;
    out.append(utf8.substr(i, len));
    units += width;
    i += len;
  }
  return out;
}

}  // namespace prism::dnd

namespace prism::dnd {

// A caller-provided string is not trusted merely because it came from a Shell
// API. This value can only be constructed by validating a fully-qualified
// drive path (e.g. `D:/Videos`); PrismCore keeps `/` separators, and the Win32
// adapter converts to `\\` only at the final API boundary. UNC and device paths
// are rejected: they are not local physical disk paths and make a free-space
// check / direct write silently target a network provider or device namespace.
struct DropTargetPath {
  std::string value;
  bool trusted = false;
  DenyReason rejection = DenyReason::PathEmpty;

  static DropTargetPath Parse(std::string_view raw) {
    DropTargetPath result;
    if (mutation::kTrustAll) {
      result.value.assign(raw);
      result.trusted = true;
      result.rejection = DenyReason::None;
      return result;
    }
    if (raw.empty()) return result;

    std::string path(raw);
    // Extended-length paths have their own Win32 namespace semantics. Refuse
    // them at the transport boundary rather than partially treating a `\\?\\`
    // prefix as an ordinary slash-normalised path.
    if (path.rfind("//?", 0) == 0 || path.rfind("\\\\?", 0) == 0) {
      if (path.find('/') != std::string::npos && path.find('\\') != std::string::npos) {
        result.rejection = DenyReason::PathExtendedMixedSep;
      } else {
        result.rejection = DenyReason::PathDeviceNamespace;
      }
      return result;
    }
    if (path.rfind("//.", 0) == 0 || path.rfind("\\\\.", 0) == 0) {
      result.rejection = DenyReason::PathDeviceNamespace;
      return result;
    }
    if (path.rfind("//", 0) == 0 || path.rfind("\\\\", 0) == 0) {
      result.rejection = DenyReason::PathUnc;
      return result;
    }
    if (path.rfind("::", 0) == 0) {
      result.rejection = DenyReason::PathShellNamespace;
      return result;
    }

    for (char& c : path) {
      if (c == '\\') c = '/';
    }
    if (path.size() < 3 ||
        !((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) ||
        path[1] != ':' || path[2] != '/') {
      result.rejection = DenyReason::PathRelative;
      return result;
    }
    for (size_t i = 0; i < path.size(); ++i) {
      const unsigned char c = static_cast<unsigned char>(path[i]);
      if (c < 0x20 || c == '<' || c == '>' || c == '"' || c == '|' || c == '?' || c == '*') {
        result.rejection = DenyReason::PathReservedChars;
        return result;
      }
      if (c == ':' && i != 1) {
        result.rejection = DenyReason::PathReservedChars;
        return result;
      }
    }
    // Repeated separators collapse; dot segments are rejected, not resolved:
    // a direct write must not depend on current-directory or reparse-point rules.
    std::string normalized;
    normalized.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
      if (path[i] == '/' && !normalized.empty() && normalized.back() == '/') continue;
      normalized.push_back(path[i]);
    }
    for (const std::string& segment : SplitPath(normalized.substr(3))) {
      if (segment == "." || segment == "..") {
        result.rejection = DenyReason::PathTraversal;
        return result;
      }
    }
    if (normalized.size() > 259) {
      result.rejection = DenyReason::PathTooLong;
      return result;
    }
    while (normalized.size() > 3 && normalized.back() == '/') normalized.pop_back();
    result.value = std::move(normalized);
    result.trusted = true;
    result.rejection = DenyReason::None;
    return result;
  }
};

// ---------------------------------------------------------------------------
// Solid-block cost explanation
// ---------------------------------------------------------------------------
struct SolidCost {
  bool applies = false;
  uint64_t selectedBytes = 0;
  uint64_t blockBytes = 0;
  bool worthDirect = true;
  std::string explanation;
};

// If an item is inside a solid block, 7z cannot begin decoding that item at
// its byte offset: the preceding stream is the dictionary state. Extracting
// one 4 KB text file from a 4 GB block can therefore decompress ~4 GB. The
// threshold is a policy, not a format fact; surface it in the result so the UI
// can explain the latency instead of pretending a direct write is free.
inline SolidCost ExplainSolidCost(bool solidBlock, uint64_t selectedBytes,
                                  uint64_t blockBytes,
                                  uint64_t expensiveThresholdBytes = 512ull * 1024 * 1024) {
  SolidCost cost;
  cost.applies = solidBlock;
  cost.selectedBytes = selectedBytes;
  cost.blockBytes = blockBytes;
  cost.worthDirect = !solidBlock || blockBytes <= expensiveThresholdBytes;
  if (solidBlock) {
    cost.explanation = "solid archive: dragging one item still requires decoding the entire solid block";
    if (!cost.worthDirect) cost.explanation += "; block exceeds the expensive-decode threshold";
  }
  return cost;
}

struct DragTargetDescription {
  bool isFileManager = false;
  std::string physicalPath;       // raw Shell result; validated here
  std::optional<uint64_t> freeBytes;  // nullopt means the volume query failed
  uint64_t archiveTotalBytes = 0;
  bool encrypted = false;
  bool passwordResolved = false;
  bool solidBlock = false;
  uint64_t solidBlockBytes = 0;
  bool acceptsIStream = false;
  bool acceptsFileDrop = true;    // CF_HDROP fallback, if the application supports it
  bool nonEmptySelection = true;
  uint64_t expensiveSolidThresholdBytes = 512ull * 1024 * 1024;
};

struct DragDecision {
  Channel channel = Channel::TempCopyFallback;
  DenyReason reason = DenyReason::NoDeliverableFormat;
  DropTargetPath targetPath;
  SolidCost solidCost;
  bool zeroTempFiles = false;
  std::string explanation;

  bool HasExplicitReason() const { return channel != Channel::TempCopyFallback || reason != DenyReason::None; }
};

// Destination space must cover extracted payload plus a small safety margin.
// A strict `free >= archiveTotal` check has a TOCTOU window and can fill the
// volume with directory metadata; reserve the larger of 16 MiB or 1%.
inline uint64_t RequiredFreeBytes(uint64_t payloadBytes) {
  const uint64_t onePercent = payloadBytes / 100 + (payloadBytes % 100 != 0 ? 1 : 0);
  const uint64_t margin = onePercent > 16ull * 1024 * 1024 ? onePercent : 16ull * 1024 * 1024;
  return payloadBytes > UINT64_MAX - margin ? UINT64_MAX : payloadBytes + margin;
}

inline DragDecision DecideDragChannel(const DragTargetDescription& target) {
  DragDecision decision;
  decision.solidCost = ExplainSolidCost(target.solidBlock, target.archiveTotalBytes,
                                        target.solidBlockBytes,
                                        target.expensiveSolidThresholdBytes);
  if (!target.nonEmptySelection) {
    decision.reason = DenyReason::EmptySelection;
    decision.explanation = "drag refused: the selection contains no files";
    return decision;
  }
  if (target.encrypted && !target.passwordResolved) {
    decision.reason = DenyReason::PasswordNotResolved;
    decision.explanation = "zero-copy refused: encrypted content has no verified password yet";
    return decision;
  }

  if (target.isFileManager) {
    decision.targetPath = DropTargetPath::Parse(target.physicalPath);
    if (!decision.targetPath.trusted) {
      decision.reason = decision.targetPath.rejection;
      decision.explanation = std::string("direct path refused: ") + ToString(decision.reason);
      return decision;
    }
    if (!target.freeBytes.has_value()) {
      decision.reason = DenyReason::DiskSpaceUnknown;
      decision.explanation = "direct path refused: destination free space could not be verified";
      return decision;
    }
    const uint64_t required = mutation::kIgnoreSpace ? 0 : RequiredFreeBytes(target.archiveTotalBytes);
    if (*target.freeBytes < required) {
      decision.reason = DenyReason::DiskSpaceInsufficient;
      decision.explanation = "direct path refused: destination volume lacks required free space";
      return decision;
    }
    decision.channel = Channel::DirectToPath;
    decision.reason = DenyReason::None;
    decision.zeroTempFiles = true;
    decision.explanation = "direct-to-path accepted: 7z.dll writes the payload into the trusted destination without a temp copy";
    if (decision.solidCost.applies) {
      decision.explanation += "; " + decision.solidCost.explanation;
    }
    return decision;
  }

  if (target.acceptsIStream) {
    decision.channel = Channel::MemoryStream;
    decision.reason = DenyReason::None;
    decision.zeroTempFiles = true;
    decision.explanation = "memory stream accepted: CFSTR_FILECONTENTS is supplied lazily on demand";
    if (decision.solidCost.applies) decision.explanation += "; " + decision.solidCost.explanation;
    return decision;
  }

  decision.reason = target.acceptsFileDrop ? DenyReason::TargetRejectsStream
                                           : DenyReason::NoDeliverableFormat;
  decision.explanation = std::string("zero-copy refused: ") + ToString(decision.reason);
  return decision;
}

// ---------------------------------------------------------------------------
// Virtual file descriptors and stable group construction
// ---------------------------------------------------------------------------
struct VirtualFileDescriptor {
  std::string name;       // basename only, UTF-8; archive path is never exposed
  uint64_t size = 0;
  int64_t modifiedTime = 0;  // PrismCore ticks; Win layer owns FILETIME conversion
  bool isDirectory = false;
};

struct GroupDescriptor {
  std::vector<VirtualFileDescriptor> files;  // archive input order is preserved
  DenyReason error = DenyReason::None;
  std::string message;
};

struct DescriptorOptions {
  size_t maxFiles = 4096;       // Shell's cItems is UINT; our bound prevents a hostile archive from allocating unbounded HGLOBAL.
  size_t maxNameUtf16Units = 259;  // FILEDESCRIPTORW::cFileName has MAX_PATH=260 including NUL.
  bool deduplicateCaseInsensitively = true;  // match CollisionResolver's Windows plan-set behaviour
};

inline std::string LowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

// Build the payload list in catalog order; duplicate basenames are renamed by
// CollisionResolver's familiar "name (2).ext" rule (directories merge, files
// rename). The in-memory planned-set makes duplicates in the same archive
// visible even though none has hit disk yet. An archive path is reduced to its
// basename first so a descriptor cannot smuggle `../` into a receiving app.
inline GroupDescriptor BuildGroupDescriptor(const std::vector<VirtualFileDescriptor>& input,
                                             DescriptorOptions options = {}) {
  GroupDescriptor group;
  if (input.size() > options.maxFiles) {
    group.error = DenyReason::NoDeliverableFormat;
    group.message = "descriptor refused: selected file count exceeds configured limit";
    return group;
  }
  std::vector<std::string> reserved;
  reserved.reserve(input.size());
  for (const VirtualFileDescriptor& source : input) {
    std::string name = std::string(FileNameOf(NormalizeArchivePath(source.name)));
    if (name.empty() || name == "." || name == "..") {
      group.error = DenyReason::PathTraversal;
      group.message = "descriptor refused: entry has no safe basename";
      group.files.clear();
      return group;
    }
    // The planned name must equal the name the receiving filesystem will
    // actually create: "x." loses its dot (and would alias a verbatim "x",
    // silently overwriting it past the collision plan), "CON.txt" opens a
    // device, "a.txt:b" becomes an invisible ADS. Sanitise BEFORE collision
    // planning so the planned-set deduplicates post-filesystem names; the
    // exact rules live in SanitizeWindowsName (single source of truth).
    name = SanitizeWindowsName(name);
    // Use exactly the ExtensionOf + stem rule from CollisionResolver. Only
    // ASCII folding is intentional; valid UTF-8 byte sequences pass through.
    const size_t dot = name.find_last_of('.');
    const std::string stem = (dot == std::string::npos || dot == 0) ? name : name.substr(0, dot);
    const std::string ext = (dot == std::string::npos || dot == 0) ? "" : name.substr(dot);
    auto folded = [&options](const std::string& s) {
      return options.deduplicateCaseInsensitively ? LowerAscii(s) : s;
    };
    auto taken = [&reserved, &folded](const std::string& candidate) {
      const std::string key = folded(candidate);
      for (const std::string& old : reserved) if (folded(old) == key) return true;
      return false;
    };
    if (taken(name) && !source.isDirectory) {
      bool found = false;
      for (unsigned n = 2; n <= 33; ++n) {
        const std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
        if (!taken(candidate)) {
          name = candidate;
          found = true;
          break;
        }
      }
      if (!found) {
        group.error = DenyReason::PathReservedChars;
        group.message = "descriptor refused: collision rename limit exhausted";
        group.files.clear();
        return group;
      }
    }
    if (source.isDirectory) {
      // CollisionResolver merges directory names rather than renaming them.
      bool already = false;
      for (const std::string& old : reserved) if (folded(old) == folded(name)) already = true;
      if (!already) reserved.push_back(name);
    } else {
      reserved.push_back(name);
    }
    if (Utf16CodeUnits(name) > options.maxNameUtf16Units) {
      const std::string truncated = TruncateUtf16(name, options.maxNameUtf16Units);
      if (truncated.empty()) {
        group.error = DenyReason::PathTooLong;
        group.message = "descriptor refused: filename cannot fit FILEDESCRIPTORW";
        group.files.clear();
        return group;
      }
      name = truncated;
      // Truncation may introduce a collision: don't silently alias two virtual
      // files after the rename step. A subsequent collision gets a deterministic
      // suffix; if it cannot fit, refuse rather than truncate away uniqueness.
      if (taken(name)) {
        bool found = false;
        for (unsigned n = 2; n <= 33; ++n) {
          const std::string suffix = " (" + std::to_string(n) + ")";
          const size_t keep = options.maxNameUtf16Units - Utf16CodeUnits(suffix + ext);
          if (keep == 0) continue;
          std::string candidate = TruncateUtf16(stem, keep) + suffix + ext;
          if (!taken(candidate)) { name = candidate; found = true; break; }
        }
        if (!found) {
          group.error = DenyReason::PathTooLong;
          group.message = "descriptor refused: truncation collision cannot be made unique";
          group.files.clear();
          return group;
        }
      }
    }
    VirtualFileDescriptor file = source;
    file.name = std::move(name);
    group.files.push_back(std::move(file));
  }
  return group;
}

// Convert the ArchiveCatalog in its stable insertion order. The index/path
// access stays in PrismCore UTF-8; the Win32 IDataObject adapter does UTF-8 ->
// UTF-16 when filling FILEDESCRIPTORW.
inline GroupDescriptor BuildGroupDescriptor(const std::vector<ArchiveItem>& catalog,
                                             DescriptorOptions options = {}) {
  std::vector<VirtualFileDescriptor> files;
  files.reserve(catalog.size());
  for (const ArchiveItem& item : catalog) {
    if (item.isDir) continue;  // folders are not stream contents; hierarchy is flattened
    files.push_back({item.path, item.size, item.mtime, false});
  }
  return BuildGroupDescriptor(files, options);
}

// ---------------------------------------------------------------------------
// Ordered on-demand stream plan
// ---------------------------------------------------------------------------
enum class StreamError : uint8_t {
  None = 0,
  InvalidRead,
  OutOfOrder,
  SeekUnsupported,
  SeekOutOfRange,
  ProducerFailed,
  ProducerOverrun,
  ProducerUnderrun,
  SizeLimitExceeded,
  Eof
};

inline const char* ToString(StreamError e) {
  switch (e) {
    case StreamError::None: return "None";
    case StreamError::InvalidRead: return "InvalidRead";
    case StreamError::OutOfOrder: return "OutOfOrder";
    case StreamError::SeekUnsupported: return "SeekUnsupported";
    case StreamError::SeekOutOfRange: return "SeekOutOfRange";
    case StreamError::ProducerFailed: return "ProducerFailed";
    case StreamError::ProducerOverrun: return "ProducerOverrun";
    case StreamError::ProducerUnderrun: return "ProducerUnderrun";
    case StreamError::SizeLimitExceeded: return "SizeLimitExceeded";
    case StreamError::Eof: return "Eof";
  }
  return "Unknown";
}

inline std::ostream& operator<<(std::ostream& os, StreamError e) { return os << ToString(e); }

struct StreamReadResult {
  size_t bytesRead = 0;
  bool eof = false;
  StreamError error = StreamError::None;
  std::string message;
};

// StreamPumpPlan is the platform-neutral state machine behind IStream::Read.
// Its producer receives the absolute offset and requested byte count; it must
// return exactly that range (except at EOF). A random-access producer can
// therefore implement IStream::Seek, while an underlying sequential 7z decoder
// can detect a backward seek and reopen/restart explicitly. We never claim
// success after silently re-reading the wrong byte range.
class StreamPumpPlan {
 public:
  using Producer = std::function<std::optional<std::string>(uint64_t, size_t)>;

  StreamPumpPlan(uint64_t totalSize, uint64_t maxSize, Producer producer)
      : totalSize_(totalSize), maxSize_(maxSize), producer_(std::move(producer)) {
    if (totalSize_ > maxSize_) SetError(StreamError::SizeLimitExceeded,
                                        "stream size exceeds configured hard limit");
  }

  StreamReadResult Read(size_t count) {
    if (error_ != StreamError::None) return {0, false, error_, message_};
    if (count == 0) return {0, position_ == totalSize_, StreamError::None, {}};
    if (position_ == totalSize_) return {0, true, StreamError::Eof, "end of stream"};
    const uint64_t remaining = totalSize_ - position_;
    const size_t wanted = remaining < static_cast<uint64_t>(count)
                              ? static_cast<size_t>(remaining) : count;
    if (!producer_) {
      SetError(StreamError::ProducerFailed, "stream producer is not configured");
      return {0, false, error_, message_};
    }
    const auto produced = producer_(position_, wanted);
    if (!produced) {
      SetError(StreamError::ProducerFailed, "stream producer failed before the declared EOF");
      return {0, false, error_, message_};
    }
    if (produced->size() > wanted) {
      SetError(StreamError::ProducerOverrun, "stream producer returned more bytes than requested");
      return {0, false, error_, message_};
    }
    if (produced->size() < wanted) {
      SetError(StreamError::ProducerUnderrun, "stream producer ended before the declared size");
      return {0, false, error_, message_};
    }
    position_ += static_cast<uint64_t>(produced->size());
    return {produced->size(), position_ == totalSize_, StreamError::None, {}};
  }

  // Sequential COM streams commonly receive seek probes. The plan is
  // seek-capable only if its producer promises absolute reads; a failed seek
  // leaves the cursor untouched. This explicit capability is safer than
  // pretending a one-way decoder can rewind itself.
  bool Seek(uint64_t absoluteOffset, bool producerSupportsRandomAccess = true) {
    if (error_ != StreamError::None) return false;
    if (!producerSupportsRandomAccess) {
      SetError(StreamError::SeekUnsupported, "producer cannot satisfy a seek request");
      return false;
    }
    if (absoluteOffset > totalSize_) {
      SetError(StreamError::SeekOutOfRange, "seek offset is beyond the declared stream size");
      return false;
    }
    position_ = absoluteOffset;
    return true;
  }

  uint64_t position() const { return position_; }
  uint64_t size() const { return totalSize_; }
  StreamError error() const { return error_; }
  const std::string& message() const { return message_; }

 private:
  void SetError(StreamError error, std::string message) {
    error_ = error;
    message_ = std::move(message);
  }

  uint64_t totalSize_ = 0;
  uint64_t maxSize_ = 0;
  uint64_t position_ = 0;
  Producer producer_;
  StreamError error_ = StreamError::None;
  std::string message_;
};

// ---------------------------------------------------------------------------
// Delayed-extract seek planning (M4: the streaming completion of channel B).
//
// A solid 7z block decodes strictly forward: file N's bytes follow files
// 1..N-1. Explorer holds an IStream and may seek anywhere, so every backward
// seek over data the decoder has already passed must be paid for somewhere:
//   CursorOnly     -- forward seek: skip ahead, or land inside the ring.
//   LookbackHit    -- small backward seek served from the recent-bytes ring
//                     (no decoder work at all).
//   RestartDecode  -- rewind the decoder to the block start and re-decode,
//                     dropping everything before the target (the honest solid
//                     cost; recorded in the stream diagnostics).
// A stream that has already spent its restart budget is REFUSED with a reason
// a user can act on: re-decoding a multi-GB solid block on every back-step is
// worse than telling them to use the right-click extract once.
//
// PlanSeek is a pure function: the caller's mutable state (how many restarts
// the stream already spent) travels in the SeekPolicy snapshot, so the same
// inputs always yield the same plan (contract C4's determinism discipline).
// ---------------------------------------------------------------------------
namespace mutation {
#if defined(PRISM_DND_MUTANT_SEEK_FORWARD_ONLY)
constexpr bool kSeekForwardOnly = true;
#else
constexpr bool kSeekForwardOnly = false;
#endif
}  // namespace mutation

struct SeekPolicy {
  bool useLookback = true;
  size_t lookbackBytes = 4u * 1024 * 1024;  // recent-bytes ring the stream keeps
  size_t maxRestarts = 2;  // after this many re-decodes, refuse with a reason
  // State snapshot supplied by the stream owner: restarts already spent on
  // this stream. Kept here (not as a PlanSeek parameter) so the plan function
  // stays signature-stable and pure.
  size_t restartsUsed = 0;
};

struct SeekPlan {
  enum Kind : uint8_t { CursorOnly, LookbackHit, RestartDecode };
  Kind kind = CursorOnly;
  // CursorOnly: forward bytes to skip before serving.
  // RestartDecode: bytes to re-decode and drop after rewinding (= target).
  // LookbackHit: 0 -- nothing is decoded.
  int64_t discardBytes = 0;
  bool refused = false;
  std::string reason;  // mandatory when refused; names the escape for the user
};

inline const char* ToString(SeekPlan::Kind kind) {
  switch (kind) {
    case SeekPlan::CursorOnly: return "CursorOnly";
    case SeekPlan::LookbackHit: return "LookbackHit";
    case SeekPlan::RestartDecode: return "RestartDecode";
  }
  return "Unknown";
}

// PRISM_CHECK_EQ prints operands on failure (contract B6).
inline std::ostream& operator<<(std::ostream& os, SeekPlan::Kind kind) {
  return os << ToString(kind);
}

inline SeekPlan PlanSeek(int64_t current, int64_t target, int64_t streamSize,
                         const SeekPolicy& policy) {
  SeekPlan plan;
  if (target < 0) {
    plan.refused = true;
    plan.reason = "seek refused: target offset is negative";
    return plan;
  }
  if (streamSize >= 0 && target > streamSize) {
    plan.refused = true;
    plan.reason = "seek refused: target offset is beyond the declared stream size";
    return plan;
  }
  // Forward (and in-place) seeks never touch the decoder's past: skip or
  // no-op. The negative-discard mutant makes backward seeks plan as
  // CursorOnly with a negative discard -- nonsense the tests must catch.
  if (mutation::kSeekForwardOnly || target >= current) {
    plan.kind = SeekPlan::CursorOnly;
    plan.discardBytes = target - current;
    return plan;
  }
  const int64_t backBytes = current - target;
  // Inclusive boundary: a rewind of exactly lookbackBytes still hits the ring.
  if (policy.useLookback && backBytes <= static_cast<int64_t>(policy.lookbackBytes)) {
    plan.kind = SeekPlan::LookbackHit;
    plan.discardBytes = 0;
    return plan;
  }
  if (policy.restartsUsed >= policy.maxRestarts) {
    plan.refused = true;
    plan.reason = "seek refused: solid-decoder restart budget exhausted (" +
                  std::to_string(policy.restartsUsed) + "/" +
                  std::to_string(policy.maxRestarts) +
                  " restarts spent); re-decoding the whole solid block again is too "
                  "expensive -- extract this file with the right-click menu instead";
    return plan;
  }
  plan.kind = SeekPlan::RestartDecode;
  plan.discardBytes = target;  // re-decode from the block start, drop to target
  return plan;
}

}  // namespace prism::dnd
