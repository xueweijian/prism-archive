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
#include <cstring>
#include <deque>
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
// 1..N-1. This is not an implementation detail of ours -- it is what 7-Zip
// itself does, and it is why the format has no random-access promise:
//
//   * 7zExtract.cpp:325  fileIndex = _db.FolderStartFileIndex[folderIndex]
//     -- every extract call for an item inside a solid folder rewinds to the
//     FIRST item of that folder, never to the item requested.
//   * 7zExtract.cpp:330-337  the requested indices are walked forward and the
//     entries in between are handed askMode kSkip, so the decoder still runs
//     over their bytes and we simply do not keep them.
//   * 7zExtract.cpp:260-272  SetTotal reports that whole discarded prefix as
//     "importantTotalUnpacked", i.e. 7-Zip itself counts the prefix as the
//     honest price of the extraction.
//   * 7zFolderInStream.cpp:GetSubStreamSize answers only about a stream it is
//     *currently* reading: it reports S_FALSE / estimates for anything past
//     _pos. There is no ICompressGetSubStreamSize on CFolderOutStream at all
//     (7zExtract.cpp:22 has it commented out), so nothing in 7z.dll claims a
//     solid folder can be seeked into.
//
// So an IStream handed to Explorer (which seeks freely) must pay for every
// backward step somewhere. We plan each seek into exactly three kinds:
//   CursorOnly     -- forward seek: skip ahead, or land inside the ring.
//   LookbackHit    -- small backward seek served from the recent-bytes ring
//                     (no decoder work at all).
//   RestartDecode  -- rewind the decoder to the block start and re-decode,
//                     dropping everything before the target (the honest solid
//                     cost, recorded in the stream diagnostics).
// A stream that cannot afford a restart is REFUSED with a reason a user can
// act on: re-decoding a multi-GB solid block on every back-step is worse than
// telling them to use the right-click extract once.
//
// PlanSeek is a pure function: the caller's mutable state (restarts and bytes
// already spent) travels in the SeekPolicy snapshot, so the same inputs always
// yield the same plan (contract C4's determinism discipline).
// ---------------------------------------------------------------------------
namespace mutation {
#if defined(PRISM_DND_MUTANT_SEEK_FORWARD_ONLY)
constexpr bool kSeekForwardOnly = true;
#else
constexpr bool kSeekForwardOnly = false;
#endif
#if defined(PRISM_DND_MUTANT_SEEK_IGNORE_BUDGET)
constexpr bool kSeekIgnoreBudget = true;
#else
constexpr bool kSeekIgnoreBudget = false;
#endif
#if defined(PRISM_DND_MUTANT_SEEK_LOOKBACK_OFF_BY_ONE)
constexpr bool kSeekLookbackOffByOne = true;
#else
constexpr bool kSeekLookbackOffByOne = false;
#endif
// PR8's Read() dropped the bytes of a forward skip WITHOUT re-anchoring the
// ring, so after a skip the retained window still claimed to start where it
// did before and the next read copied from ring_.data() + offset with
// offset >= ring_.size() -- a heap over-read that serves garbage (or traps).
// This mutant reproduces exactly that; the "forward skip" case must go red.
#if defined(PRISM_DND_MUTANT_RING_KEEP_SKIPPED)
constexpr bool kRingKeepSkipped = true;
#else
constexpr bool kRingKeepSkipped = false;
#endif
// The ring may only serve bytes it actually holds. PR8's DelayedExtractStream
//::Read went straight to producer_(count) and advanced position_ by what came
// back, so a seek that did not restart the decoder served the WRONG BYTES while
// reporting success -- the worst possible failure for a copy engine. This
// mutant makes the cursor skip the residency check and trust the frontier; the
// lookback cases must go red.
#if defined(PRISM_DND_MUTANT_SERVE_UNRESIDENT)
constexpr bool kServeUnresident = true;
#else
constexpr bool kServeUnresident = false;
#endif
}  // namespace mutation

// Default lookback ring = 4 MiB. Measured, not invented (see
// Tests/test_dnd_seek.cpp "measured:" cases and the table in the report):
// serving a back-step from the ring is a memcpy at ~15.8 GiB/s, i.e. 0.063 ms
// per MiB, while re-decoding the same bytes costs 16.54 ms per MiB of LZMA2
// text and 1.40 ms per MiB of stored data -- 22x to 262x more. 4 MiB covers
// the read-ahead prefix a real drop target issues before seeking back to 0
// (Explorer and every format sniffer read a few hundred KiB at most) while
// costing 4 MiB of RAM per live stream, which is the same order as the 4 MiB
// pipe high-water mark the producer already holds.
inline constexpr size_t kDefaultSeekLookbackBytes = 4u * 1024 * 1024;

// Default re-decode byte budget = 16 MiB. PR8 bounded restarts by COUNT (2),
// and a count cannot bound latency: two restarts into a 256 MiB solid block
// are 2 x 256 x 16.54 ms = 8.5 s of decode before the target sees a byte.
// A byte budget bounds it directly. 16 MiB of discarded prefix measured
// 264 ms worst case (LZMA2 text) and 22 ms best case (stored), which is the
// most we are willing to make a drop target wait before it gets an answer at
// all -- past that the honest answer is "extract it, don't drag it".
inline constexpr size_t kDefaultRestartDecodeBudgetBytes = 16u * 1024 * 1024;

struct SeekPolicy {
  bool useLookback = true;
  size_t lookbackBytes = kDefaultSeekLookbackBytes;  // recent-bytes ring the stream keeps
  // Two independent budgets; BOTH must hold. maxRestarts is the cheap guard
  // against a pathological seek storm (each restart also costs a pipe flush),
  // restartDecodeBudget is the one that bounds wall-clock time.
  size_t maxRestarts = 2;  // after this many re-decodes, refuse with a reason
  size_t restartDecodeBudget = kDefaultRestartDecodeBudgetBytes;
  // State snapshot supplied by the stream owner: what this stream already
  // spent. Kept here (not as a PlanSeek parameter) so the plan function stays
  // signature-stable and pure.
  size_t restartsUsed = 0;
  size_t restartBytesUsed = 0;
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
  // no-op. The forward-only mutant makes backward seeks plan as CursorOnly
  // with a negative discard -- nonsense the tests must catch.
  if (mutation::kSeekForwardOnly || target >= current) {
    plan.kind = SeekPlan::CursorOnly;
    plan.discardBytes = target - current;
    return plan;
  }
  const int64_t backBytes = current - target;
  // Inclusive boundary: a rewind of exactly lookbackBytes still hits the ring.
  // The off-by-one mutant turns this into a strict '<' and the boundary case
  // in the suite goes red.
  const bool inRing = mutation::kSeekLookbackOffByOne
                          ? backBytes < static_cast<int64_t>(policy.lookbackBytes)
                          : backBytes <= static_cast<int64_t>(policy.lookbackBytes);
  if (policy.useLookback && inRing) {
    plan.kind = SeekPlan::LookbackHit;
    plan.discardBytes = 0;
    return plan;
  }
  // Both budgets are checked BEFORE granting, so a refusal never moves the
  // cursor and never starts a decode it cannot finish.
  const bool budgetOk = mutation::kSeekIgnoreBudget
                            ? true
                            : (policy.restartBytesUsed + static_cast<size_t>(target) <=
                               policy.restartDecodeBudget);
  if (policy.restartsUsed >= policy.maxRestarts) {
    plan.refused = true;
    plan.reason = "seek refused: solid-decoder restart budget exhausted (" +
                  std::to_string(policy.restartsUsed) + "/" +
                  std::to_string(policy.maxRestarts) +
                  " restarts spent); re-decoding the whole solid block again is too "
                  "expensive -- extract this file with the right-click menu instead";
    return plan;
  }
  if (!budgetOk) {
    plan.refused = true;
    plan.reason =
        "seek refused: solid re-decode budget exhausted (re-decoding " +
        std::to_string(target) + " bytes would spend " +
        std::to_string(policy.restartBytesUsed + static_cast<size_t>(target)) +
        " of " + std::to_string(policy.restartDecodeBudget) +
        " byte budget); this solid block is too large to seek within -- extract "
        "this file with the right-click menu instead";
    return plan;
  }
  plan.kind = SeekPlan::RestartDecode;
  plan.discardBytes = target;  // re-decode from the block start, drop to target
  return plan;
}

// ---------------------------------------------------------------------------
// Recent-byte ring: the "lookback" half of the seek story.
//
// PR8 kept the ring in a std::string and called ring_.erase(0, excess) on every
// append past capacity, which is an O(capacity) memmove per chunk once the ring
// is full -- 0.25 ms per 64 KiB read at 4 MiB (memmove measured at 15.8 GiB/s
// on this machine), i.e. +40% on a 4 MiB sequential read. Tests/test_dnd_seek.cpp
// measures both and asserts the chunked ring wins; the chunked form below is
// O(1) amortised because a chunk that falls out of the window is simply popped.
//
// Invariant: end() - base() == retained bytes; Copy() clamps to what is
// resident, and the caller (DelayedSeekCursor) treats a short copy as
// "not resident" rather than guessing.
// ---------------------------------------------------------------------------
class RecentByteRing {
 public:
  explicit RecentByteRing(size_t capacity) : capacity_(capacity) {}

  void Clear() {
    chunks_.clear();
    base_ = 0;
    end_ = 0;
  }

  // Drop everything and re-anchor at `offset` (used when the decoder skipped
  // forward: those bytes were never retained and must not appear resident).
  void ResetTo(uint64_t offset) {
    chunks_.clear();
    base_ = offset;
    end_ = offset;
  }

  void Append(std::string chunk) {
    if (chunk.empty()) return;
    end_ += chunk.size();
    chunks_.push_back(std::move(chunk));
    // Evict whole chunks from the front. A ring smaller than one chunk keeps
    // that chunk (a short window is worse than an oversized one).
    while (chunks_.size() > 1 && (end_ - base_) > capacity_) {
      base_ += chunks_.front().size();
      chunks_.pop_front();
    }
    if (end_ - base_ > capacity_ && chunks_.size() == 1) {
      // One oversized chunk: keep only its tail.
      const size_t excess = static_cast<size_t>(end_ - base_ - capacity_);
      chunks_.front().erase(0, excess);
      base_ += excess;
    }
  }

  // Copy the retained bytes starting at absolute offset `start`. Returns the
  // number actually copied (0 when `start` is not resident).
  size_t Copy(uint64_t start, uint8_t* out, size_t count) const {
    if (count == 0 || start < base_ || start >= end_) return 0;
    size_t skip = static_cast<size_t>(start - base_);
    size_t copied = 0;
    for (const std::string& chunk : chunks_) {
      if (skip >= chunk.size()) {
        skip -= chunk.size();
        continue;
      }
      const size_t avail = chunk.size() - skip;
      const size_t n = avail < (count - copied) ? avail : (count - copied);
      std::memcpy(out + copied, chunk.data() + skip, n);
      copied += n;
      skip = 0;
      if (copied == count) break;
    }
    return copied;
  }

  bool Holds(uint64_t offset) const { return offset >= base_ && offset <= end_; }
  uint64_t base() const { return base_; }
  uint64_t end() const { return end_; }
  size_t capacity() const { return capacity_; }
  // Retained bytes plus chunk bookkeeping -- what the ring actually costs.
  size_t residentBytes() const {
    size_t total = 0;
    for (const std::string& c : chunks_) total += c.size();
    return total;
  }

 private:
  size_t capacity_;
  std::deque<std::string> chunks_;
  uint64_t base_ = 0;
  uint64_t end_ = 0;
};

// ---------------------------------------------------------------------------
// DelayedSeekCursor: the pure state machine behind IStream::Read/Seek.
//
// The Windows class (PrismWin/PrismVirtualDragDrop.hpp) is a COM shell around
// this, so byte-exactness, ring residency and restart accounting are all
// verifiable on Linux in milliseconds instead of one Windows CI round each.
//
// Invariants (asserted by the Linux suite against a synthetic solid block):
//   * position() <= totalSize(); ring.end() == frontier()
//   * a Read never serves a byte it did not pull or retain
//   * a refused Seek never moves position() and never spends a restart
//   * restartBytesUsed() is booked from the PLAN, before the decode runs, so an
//     abandoned re-decode can never look free
// ---------------------------------------------------------------------------
class DelayedSeekCursor {
 public:
  // Next sequential bytes from the decoder frontier; empty means "decoder
  // ended or failed before the declared size" (latched loudly).
  using PullFn = std::function<std::string(size_t)>;
  // Rewind the decoder to stream offset 0, keeping the session alive.
  using RestartFn = std::function<bool()>;

  struct ReadOutcome {
    size_t served = 0;
    bool eof = false;
    bool failed = false;
  };

  // Default-constructible so a COM wrapper can hold one as a plain member; an
  // un-initialised cursor reports a zero-length, already-EOF stream.
  DelayedSeekCursor() : ring_(SeekPolicy{}.lookbackBytes) {}

  DelayedSeekCursor(uint64_t totalSize, PullFn pull, RestartFn restart,
                    SeekPolicy policy = {})
      : totalSize_(totalSize),
        ring_(policy.lookbackBytes),
        pull_(std::move(pull)),
        restart_(std::move(restart)),
        policy_(policy) {
    policy_.restartsUsed = 0;
    policy_.restartBytesUsed = 0;
  }

  // Decoder pull granularity. Small enough that the ring never trails the
  // frontier by more than one chunk, so a lookback window inside the ring is
  // always fully resident.
  static constexpr size_t kPullChunkBytes = 64 * 1024;

  ReadOutcome Read(uint8_t* out, size_t count) {
    ReadOutcome result;
    if (failed_) {
      result.failed = true;
      return result;
    }
    if (count == 0 || position_ >= totalSize_) {
      result.eof = position_ >= totalSize_;
      return result;
    }
    size_t served = 0;
    // (1) Serve what the cursor still covers from the recent-bytes ring.
    while (served < count && position_ < frontier_) {
      const size_t avail = static_cast<size_t>(
          (frontier_ - position_) < (count - served) ? (frontier_ - position_)
                                                     : (count - served));
      const size_t got = mutation::kServeUnresident
                             ? ring_.Copy(ring_.base(), out + served, avail)
                             : ring_.Copy(position_, out + served, avail);
      if (got == 0) {
        LatchFailure("lookback ring gap at offset " + std::to_string(position_));
        break;
      }
      served += got;
      position_ += got;
      if (got < avail) {
        LatchFailure("lookback ring short at offset " + std::to_string(position_));
        break;
      }
    }
    // (2) Align the decoder: a previous seek may have parked the cursor ahead
    // of the frontier (forward skip) or rewound the decoder to zero (restart
    // prefix). Drop exactly (position_ - frontier_) decoded bytes, recomputed
    // from absolute positions every read so no seek can desynchronise it.
    // Dropped bytes are NOT retained -- the ring re-anchors at the frontier,
    // otherwise frontier_-relative offsets would read past the retained bytes.
    int64_t toDrop = static_cast<int64_t>(position_) - static_cast<int64_t>(frontier_);
    while (served < count && !failed_ && toDrop > 0) {
      const size_t want = static_cast<size_t>(toDrop) < kPullChunkBytes
                              ? static_cast<size_t>(toDrop) : kPullChunkBytes;
      const std::string chunk = pull_ ? pull_(want) : std::string();
      if (chunk.empty()) {
        LatchFailure("decoder ended while skipping to offset " + std::to_string(position_));
        break;
      }
      frontier_ += chunk.size();
      if (!mutation::kRingKeepSkipped) ring_.ResetTo(frontier_);
      if (droppingForRestart_) {
        wastedBytes_ += chunk.size();  // re-decoded purely to reach the target
      } else {
        skippedBytes_ += chunk.size();  // forward-skip cost of a solid block
      }
      toDrop -= static_cast<int64_t>(chunk.size());
    }
    if (toDrop <= 0) droppingForRestart_ = false;
    // (3) Pull fresh bytes and retain them for small back-steps.
    while (served < count && !failed_ && position_ < totalSize_) {
      const size_t want = (count - served) < kPullChunkBytes ? (count - served)
                                                             : kPullChunkBytes;
      std::string chunk = pull_ ? pull_(want) : std::string();
      if (chunk.empty()) {
        LatchFailure("decoder ended early at frontier " + std::to_string(frontier_));
        break;
      }
      const size_t n = chunk.size() < (count - served) ? chunk.size() : (count - served);
      std::memcpy(out + served, chunk.data(), n);
      frontier_ += chunk.size();
      ring_.Append(std::move(chunk));
      served += n;
      position_ += n;
    }
    result.served = served;
    result.failed = failed_;
    result.eof = !failed_ && position_ >= totalSize_;
    return result;
  }

  // Absolute seek (the COM layer resolves STREAM_SEEK_SET/CUR/END first).
  // A refusal leaves position() untouched and spends nothing.
  bool Seek(int64_t target, std::string* reason) {
    if (failed_) {
      if (reason) *reason = "stream is in a failed state";
      return false;
    }
    const int64_t current = static_cast<int64_t>(position_);
    policy_.restartsUsed = static_cast<size_t>(restarts_);
    policy_.restartBytesUsed = static_cast<size_t>(restartBytesUsed_);
    const SeekPlan plan =
        PlanSeek(current, target, static_cast<int64_t>(totalSize_), policy_);
    if (plan.refused) {
      // Budget refusals surface here with the planner's reason (which names
      // the right-click-extract escape hatch); the cursor stays untouched.
      diagnostics_ += plan.reason + "; ";
      if (reason) *reason = plan.reason;
      return false;
    }
    switch (plan.kind) {
      case SeekPlan::CursorOnly:
        // No decoder action now: Read() lazily drops (position_ - frontier_)
        // bytes on the next demand, so seeking across an unexplored solid
        // block stays O(1) until bytes are actually pulled.
        position_ = static_cast<uint64_t>(target);
        return true;
      case SeekPlan::LookbackHit:
        if (ring_.Holds(static_cast<uint64_t>(target))) {
          position_ = static_cast<uint64_t>(target);  // served from the ring
          return true;
        }
        // The planner's window is cursor-relative, the ring is
        // frontier-relative; when the planned window is not resident (a
        // back-step after a back-step), pay the honest restart instead of
        // guessing at bytes that left the ring. The fallback is re-planned
        // through the SAME pure function with lookback disabled, so it is
        // still budget-checked -- it can refuse, it can never over-spend.
        {
          SeekPolicy noRing = policy_;
          noRing.useLookback = false;
          const SeekPlan fallback =
              PlanSeek(current, target, static_cast<int64_t>(totalSize_), noRing);
          if (fallback.refused) {
            diagnostics_ += fallback.reason + "; ";
            if (reason) *reason = fallback.reason;
            return false;
          }
          if (!DoRestartDecode(fallback.discardBytes)) {
            if (reason) *reason = "decoder restart failed";
            return false;
          }
          position_ = static_cast<uint64_t>(target);
          return true;
        }
      case SeekPlan::RestartDecode:
        if (!DoRestartDecode(plan.discardBytes)) {
          if (reason) *reason = "decoder restart failed";
          return false;
        }
        position_ = static_cast<uint64_t>(target);
        return true;
    }
    return false;
  }

  uint64_t position() const { return position_; }
  uint64_t frontier() const { return frontier_; }
  uint64_t totalSize() const { return totalSize_; }
  bool failed() const { return failed_; }
  uint64_t restarts() const { return restarts_; }
  uint64_t restartBytesUsed() const { return restartBytesUsed_; }
  uint64_t wastedBytes() const { return wastedBytes_; }
  uint64_t skippedBytes() const { return skippedBytes_; }
  const RecentByteRing& ring() const { return ring_; }
  const std::string& diagnostics() const { return diagnostics_; }

 private:
  // Rewind the producer to the stream start on the SAME session. `prefixBytes`
  // is what the planner said this restart will discard, and it is what the byte
  // budget is charged for -- booked up front (see the class invariant).
  bool DoRestartDecode(int64_t prefixBytes) {
    if (!restart_ || !restart_()) {
      LatchFailure("decoder restart failed");
      return false;
    }
    ++restarts_;
    if (prefixBytes > 0) restartBytesUsed_ += static_cast<uint64_t>(prefixBytes);
    ring_.ResetTo(0);
    frontier_ = 0;
    droppingForRestart_ = true;
    diagnostics_ += "restart#" + std::to_string(restarts_) + "(" +
                    std::to_string(prefixBytes) + "B); ";
    return true;
  }

  void LatchFailure(const std::string& what) {
    if (failed_) return;
    failed_ = true;
    diagnostics_ += what + "; ";
  }

  uint64_t totalSize_ = 0;
  uint64_t position_ = 0;  // the caller's cursor -- never advanced speculatively
  uint64_t frontier_ = 0;  // absolute offset of the decoder's next byte
  RecentByteRing ring_;
  PullFn pull_;
  RestartFn restart_;
  SeekPolicy policy_;
  bool droppingForRestart_ = false;
  uint64_t restarts_ = 0;
  uint64_t restartBytesUsed_ = 0;
  uint64_t wastedBytes_ = 0;
  uint64_t skippedBytes_ = 0;
  bool failed_ = false;
  std::string diagnostics_;
};

}  // namespace prism::dnd
