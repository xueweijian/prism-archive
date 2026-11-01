// PrismArchive -- card C22: malformed-archive corpus smoke test.
//
// Feeds the deterministic corpus from tools/fuzz/gen_corpus.py through the
// PrismCore DECISION layer (PathSafety / SmartExtractor / SecurityGuard) on
// every platform, and asserts the two-sided contract that makes a rejection
// assertion meaningful:
//
//   * a hostile entry (traversal, overlong, over-deep, mojibake, encrypted,
//     inconsistent) is NEVER accepted -- and every non-Accept verdict carries a
//     non-empty, specific reason;
//   * a plainly clean entry is ALWAYS accepted -- otherwise "everything is
//     rejected" would look like a sharp test (the classic false green);
//   * nothing crashes, nothing is read out of bounds (the reader is
//     bounds-checked at every field, and the ASan/UBSan CI lane is the hard
//     proof), and every transformed path stays inside the plan's base dir.
//
// Why a deterministic corpus and not libFuzzer/AFL (also in tools/fuzz/README.md):
// the Phase 1 exit criterion is 10 000 samples of a KNOWN distribution (one per
// defence), reproducible byte-for-byte from a seed, generated in under a second
// and consumed in under a second -- so it can be a per-push gate. A coverage
// fuzzer is neither reproducible across header-only rebuilds nor affordable in a
// ~2.5 minute push lane; it would end up as an unwatched nightly job.
//
// Two negative controls are compiled from this same file (CONTRACTS D3):
//   PRISM_FUZZ_MUTANT_ACCEPT_ALL      a permissive guard must turn the suite red;
//   PRISM_FUZZ_MUTANT_VALID_REJECTED  "expected reject, but it passed" on a
//                                     genuinely valid sample must turn it red.
// Both are registered with WILL_FAIL, which is green only while the mutation is
// caught. Each mutant prints "FUZZ MUTATION ARMED: ...", and one CTest entry
// requires that marker in the mutant binary's output -- a typo in the compile
// definition would otherwise be a silent no-op.
//
// Unlike an integration test this one never skips on a real CI: a missing corpus
// fixture exits 77 with SKIP_RETURN_CODE (D1: visible "Skipped", never a pass).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "ArchiveCatalog.hpp"
#include "CharsetRepair.hpp"
#include "PathSafety.hpp"
#include "SecurityGuard.hpp"
#include "SmartExtractor.hpp"
#include "mini_test.hpp"

namespace {

// ---------------------------------------------------------------------------
// Mutation switches (see the two WILL_FAIL targets in c22-fuzz.cmake).
// ---------------------------------------------------------------------------
#if defined(PRISM_FUZZ_MUTANT_ACCEPT_ALL)
constexpr bool kAcceptAllGuard = true;
#else
constexpr bool kAcceptAllGuard = false;
#endif
#if defined(PRISM_FUZZ_MUTANT_VALID_REJECTED)
constexpr bool kExpectValidRejected = true;
#else
constexpr bool kExpectValidRejected = false;
#endif

bool MutationArmed() { return kAcceptAllGuard || kExpectValidRejected; }

// ---------------------------------------------------------------------------
// The corpus ABI: this list must equal the generator's CLASSES, in order. The
// packed container carries the names, so a one-sided rename is a hard failure
// instead of "0 samples in that class". This is encoded here (not in the corpus)
// because the per-class expectations below ARE the contract.
// ---------------------------------------------------------------------------
const char* const kClasses[] = {
    "truncated",  "bad-crc",     "overlong-path", "deep-nesting", "traversal",
    "fake-size",  "bad-utf8",    "empty",         "zero-byte",    "encrypted",
    "zip64-lies",
};
constexpr size_t kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
constexpr size_t kMinSamplesPerClass = 50;   // generator contract
constexpr size_t kMaxEntriesPerSample = 4096;  // sanity bound on the parser

// ---------------------------------------------------------------------------
// Policy budgets. These are the limits the decision layer enforces; they are
// deliberately explicit so an "overlong" sample either trips one of them or the
// test says which limit it expected to trip and why it is not a rejection.
// ---------------------------------------------------------------------------
constexpr uint64_t kMaxComponentBytes = 255;    // NTFS / most filesystems
constexpr uint64_t kMaxPathBytes = 32767;       // Windows NT maximum path
constexpr size_t kMaxDepth = 128;               // practical nesting budget

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
uint32_t Crc32(const std::string& data) {
  uint32_t crc = 0xFFFFFFFFu;
  for (const char ch : data) {
    crc ^= static_cast<unsigned char>(ch);
    for (int k = 0; k < 8; ++k) {
      crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
  }
  return ~crc;
}

uint32_t Rd16(const std::string& b, uint64_t off) {
  return static_cast<uint32_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8);
}

uint32_t Rd32(const std::string& b, uint64_t off) {
  return static_cast<uint32_t>(static_cast<unsigned char>(b[off])) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 1])) << 8) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 2])) << 16) |
         (static_cast<uint32_t>(static_cast<unsigned char>(b[off + 3])) << 24);
}

bool ReadFile(const std::string& path, std::string* out) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return false;
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size < 0 || size > (64ll << 20)) return false;  // 64 MiB cap: corpus ABI
  in.seekg(0, std::ios::beg);
  out->assign(static_cast<size_t>(size), '\0');
  if (size > 0) in.read(out->data(), size);
  return static_cast<bool>(in);
}

std::string JoinFs(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  const char last = dir.back();
  if (last == '/' || last == '\\') return dir + name;
  return dir + "/" + name;
}

bool IsDirEntryName(const std::string& raw) {
  return !raw.empty() && (raw.back() == '/' || raw.back() == '\\');
}

// Counts '/'-separated depth of a path, using the same lexical rules the core
// uses (so the test cannot disagree with the code under test about "depth").
size_t DepthOf(const std::string& normalized) {
  return prism::SplitPath(normalized).size();
}

bool HasOverlongComponent(const std::string& normalized) {
  if (normalized.size() > kMaxPathBytes) return true;
  for (const std::string& part : prism::SplitPath(normalized)) {
    if (part.size() > kMaxComponentBytes) return true;
  }
  return false;
}

// A name the extractor must not take at face value: anything that has to be
// rewritten or refused. Kept deliberately simple -- a predicate that mirrors the
// implementation would be a tautology, not a test.
bool LooksHostile(const std::string& raw) {
  if (raw.empty()) return true;
  if (raw.find(':') != std::string::npos) return true;  // drive / ADS
  if (raw.find('\0') != std::string::npos) return true;
  if (raw[0] == '/' || raw[0] == '\\') return true;     // absolute or UNC
  // Segment-wise, NOT a substring search: "....//evil" is a directory literally
  // named "....", which the core deliberately keeps (see test_path_safety.cpp),
  // while ".." as a segment must never survive.
  size_t start = 0;
  while (start <= raw.size()) {
    size_t sep = raw.find_first_of("/\\", start);
    if (sep == std::string::npos) sep = raw.size();
    const std::string segment = raw.substr(start, sep - start);
    if (segment == ".." || segment == ".") return true;
    if (sep == raw.size()) break;
    start = sep + 1;
  }
  const std::string name = std::string(prism::FileNameOf(prism::NormalizeArchivePath(raw)));
  if (!name.empty() && name != prism::SanitizeWindowsName(name)) return true;  // CON, trailing dot
  return false;
}

// ---------------------------------------------------------------------------
// A bounds-checked ZIP walker. It is NOT a ZIP implementation: it only lifts the
// fields the decision layer consumes, and every read is guarded, because the
// input is malformed by construction.
// ---------------------------------------------------------------------------
struct ZipEntryProbe {
  std::string name;
  uint32_t flags = 0;
  uint32_t method = 0;
  uint32_t crc = 0;
  uint64_t compSize = 0;
  uint64_t uncompSize = 0;
  uint64_t dataOffset = 0;
  uint64_t dataLen = 0;
  bool isDir = false;
  bool encrypted = false;
  bool sizeIsPlaceholder = false;  // 0xFFFFFFFF without a zip64 resolution
};

struct ProbeResult {
  bool eocd = false;
  std::string error;  // non-empty whenever the file could not be walked cleanly
  std::vector<ZipEntryProbe> entries;
};

constexpr uint64_t kNoPos = static_cast<uint64_t>(-1);

ProbeResult ProbeZip(const std::string& b) {
  ProbeResult r;
  if (b.size() < 22) {
    r.error = "shorter than a 22-byte end-of-central-directory record";
    return r;
  }
  const uint64_t span = 65557ull + 22ull;  // max comment + EOCD
  const uint64_t lowest = (b.size() > span) ? b.size() - span : 0;
  uint64_t eocd = kNoPos;
  for (uint64_t p = b.size() - 22;; --p) {
    if (Rd32(b, p) == 0x06054B50u) {
      eocd = p;
      break;
    }
    if (p == lowest) break;
  }
  if (eocd == kNoPos) {
    r.error = "no EOCD signature in the last 64 KiB";
    return r;
  }
  r.eocd = true;
  const uint32_t count = Rd16(b, eocd + 10);
  const uint64_t cdSize = Rd32(b, eocd + 12);
  const uint64_t cdOff = Rd32(b, eocd + 16);
  if (cdOff > b.size() || cdSize > b.size() - cdOff) {
    r.error = "central directory lies past the end of the file";
    return r;
  }
  uint64_t pos = cdOff;
  const uint64_t cdEnd = cdOff + cdSize;
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 46 > b.size() || pos + 46 > cdEnd) {
      r.error = "central directory entry truncated";
      return r;
    }
    if (Rd32(b, pos) != 0x02014B50u) {
      r.error = "central directory entry has a bad signature";
      return r;
    }
    ZipEntryProbe e;
    e.flags = Rd16(b, pos + 8);
    e.method = Rd16(b, pos + 10);
    e.crc = Rd32(b, pos + 16);
    e.compSize = Rd32(b, pos + 20);
    e.uncompSize = Rd32(b, pos + 24);
    const uint32_t nameLen = Rd16(b, pos + 28);
    const uint32_t extraLen = Rd16(b, pos + 30);
    const uint32_t commentLen = Rd16(b, pos + 32);
    const uint64_t nameOff = pos + 46;
    if (nameOff + nameLen > b.size()) {
      r.error = "entry name lies past the end of the file";
      return r;
    }
    e.name.assign(b, static_cast<size_t>(nameOff), nameLen);
    e.isDir = IsDirEntryName(e.name);
    e.encrypted = (e.flags & 0x0001u) != 0;
    e.sizeIsPlaceholder = (e.compSize == 0xFFFFFFFFull || e.uncompSize == 0xFFFFFFFFull);
    const uint64_t localOff = Rd32(b, pos + 42);
    if (localOff + 30 <= b.size() && Rd32(b, localOff) == 0x04034B50u) {
      const uint32_t localNameLen = Rd16(b, localOff + 26);
      const uint32_t localExtraLen = Rd16(b, localOff + 28);
      const uint64_t data = localOff + 30 + localNameLen + localExtraLen;
      e.dataOffset = (data <= b.size()) ? data : b.size();
      const uint64_t avail = b.size() - e.dataOffset;
      e.dataLen = (std::min)(avail, e.compSize);
    }
    r.entries.push_back(e);
    if (r.entries.size() > kMaxEntriesPerSample) {
      r.error = "entry count above the sanity bound";
      return r;
    }
    pos = nameOff + nameLen + extraLen + commentLen;
  }
  return r;
}

// ---------------------------------------------------------------------------
// The decision under test: one verdict per entry, composed from the core
// primitives exactly the way the extract wiring composes them.
// ---------------------------------------------------------------------------
enum class Outcome { Accept, Rewrite, Reject, NeedsPassword };

const char* OutcomeName(Outcome o) {
  switch (o) {
    case Outcome::Accept: return "Accept";
    case Outcome::Rewrite: return "Rewrite";
    case Outcome::Reject: return "Reject";
    case Outcome::NeedsPassword: return "NeedsPassword";
  }
  return "?";
}

struct Verdict {
  Outcome outcome = Outcome::Reject;
  std::string reason;
  std::string finalPath;
};

struct FuzzGate {
  std::string outRoot = "/prism-fuzz-out";

  Verdict Decide(const ZipEntryProbe& e) const {
    // MUTANT: a guard that approves everything must be caught by the class
    // expectations below -- that is the point of the negative control. It still
    // builds a well-formed final path, so the ONLY thing that can turn this
    // build red is the acceptance of something that must not be accepted.
    if (kAcceptAllGuard) {
      return {Outcome::Accept, "MUTANT: accept-all guard",
              prism::JoinPath(outRoot, prism::SanitizeRelativePath(e.name))};
    }

    if (e.name.empty()) return {Outcome::Reject, "entry name is empty", {}};
    if (e.encrypted) {
      return {Outcome::NeedsPassword,
              "entry is encrypted (general purpose bit 0): password required "
              "before any path decision",
              {}};
    }
    if (!prism::IsUtf8Valid(e.name)) {
      return {Outcome::Reject,
              "entry name is not valid UTF-8; the charset layer must repair it "
              "before it can become a path",
              {}};
    }
    const std::string normalized = prism::NormalizeArchivePath(e.name);
    const std::string safe = prism::SanitizeRelativePath(e.name);
    if (safe.empty()) {
      return {Outcome::Reject, "path collapses to nothing after sanitisation", {}};
    }
    if (HasOverlongComponent(safe)) {
      return {Outcome::Reject,
              "path exceeds the extract budget (component > " +
                  std::to_string(kMaxComponentBytes) + " B or path > " +
                  std::to_string(kMaxPathBytes) + " B)",
              {}};
    }
    if (DepthOf(safe) > kMaxDepth) {
      return {Outcome::Reject,
              "nesting depth " + std::to_string(DepthOf(safe)) + " exceeds " +
                  std::to_string(kMaxDepth),
              {}};
    }
    prism::WriteGuard guard(outRoot);
    const std::string joined = prism::JoinPath(outRoot, safe);
    if (!guard.Approve(joined)) {
      return {Outcome::Reject, "path escapes the target root after rewriting", {}};
    }
    if (safe != e.name) {
      return {Outcome::Rewrite,
              "hostile path rewritten: '" + e.name + "' -> '" + safe + "'", joined};
    }
    (void)normalized;
    const std::string name = std::string(prism::FileNameOf(safe));
    const std::string mangled = prism::SanitizeWindowsName(name);
    if (mangled != name) {
      return {Outcome::Rewrite,
              "Windows name hygiene: '" + name + "' -> '" + mangled + "'", joined};
    }
    return {Outcome::Accept, "clean relative path inside the target root", joined};
  }
};

// ---------------------------------------------------------------------------
// Corpus loading (packed container, or loose <class>-NNNN.zip files).
// ---------------------------------------------------------------------------
struct Sample {
  std::string label;
  std::string cls;
  std::string bytes;
  bool control = false;
};

struct Corpus {
  bool loaded = false;
  std::vector<Sample> samples;
  std::vector<Sample> controls;
  uint32_t fnv = 0;
  std::string error;
};

Corpus g_corpus;

uint32_t Fnv1a(const std::string& data) {
  uint32_t h = 2166136261u;
  for (const char ch : data) {
    h ^= static_cast<unsigned char>(ch);
    h *= 16777619u;
  }
  return h;
}

bool LoadPacked(const std::string& path, Corpus* corpus) {
  std::string blob;
  if (!ReadFile(path, &blob)) {
    corpus->error = "cannot read " + path;
    return false;
  }
  static const char kMagic[] = "PRISMFZ1\n";
  const size_t magicLen = sizeof(kMagic) - 1;
  if (blob.size() < magicLen || blob.compare(0, magicLen, kMagic) != 0) {
    corpus->error = "corpus.bin has a bad magic";
    return false;
  }
  size_t pos = magicLen;
  if (pos + 2 > blob.size()) {
    corpus->error = "corpus.bin truncated in the class table";
    return false;
  }
  const size_t classCount = Rd16(blob, pos);
  pos += 2;
  std::vector<std::string> names;
  for (size_t i = 0; i < classCount; ++i) {
    if (pos + 1 > blob.size()) {
      corpus->error = "corpus.bin truncated in the class table";
      return false;
    }
    const size_t len = static_cast<unsigned char>(blob[pos]);
    ++pos;
    if (pos + len > blob.size()) {
      corpus->error = "corpus.bin truncated in the class table";
      return false;
    }
    names.push_back(blob.substr(pos, len));
    pos += len;
  }
  if (names.size() != kClassCount) {
    std::ostringstream os;
    os << "class table mismatch: corpus has " << names.size() << " classes, this binary "
       << kClassCount << " -- generator and consumer ABI diverged";
    corpus->error = os.str();
    return false;
  }
  for (size_t i = 0; i < kClassCount; ++i) {
    if (names[i] != kClasses[i]) {
      corpus->error = "class table mismatch at index " + std::to_string(i) + ": corpus has '" +
                      names[i] + "', this binary expects '" + kClasses[i] + "'";
      return false;
    }
  }
  corpus->fnv = Fnv1a(blob);
  size_t index = 0;
  while (pos < blob.size()) {
    if (pos + 8 > blob.size()) {
      corpus->error = "corpus.bin truncated in a sample header";
      return false;
    }
    const uint32_t classIndex = Rd32(blob, pos);
    const uint32_t size = Rd32(blob, pos + 4);
    pos += 8;
    if (classIndex >= kClassCount) {
      corpus->error = "corpus.bin sample names an unknown class index " +
                      std::to_string(classIndex);
      return false;
    }
    if (size > blob.size() - pos) {
      corpus->error = "corpus.bin sample payload past the end of the file";
      return false;
    }
    Sample s;
    s.cls = kClasses[classIndex];
    s.label = s.cls + "-" + std::to_string(index);
    s.bytes.assign(blob, pos, size);
    pos += size;
    corpus->samples.push_back(std::move(s));
    ++index;
  }
  return true;
}

bool LoadLoose(const std::string& dir, Corpus* corpus) {
  // Fallback for a by-hand run: <dir>/<class>-NNNN.zip plus <dir>/control/*.zip.
  // No directory_iterator on purpose -- this file must stay dependency-free.
  size_t found = 0;
  for (size_t c = 0; c < kClassCount; ++c) {
    for (int n = 0; n < 100000; ++n) {
      char name[64];
      std::snprintf(name, sizeof(name), "%s-%04d.zip", kClasses[c], n);
      std::string bytes;
      if (!ReadFile(JoinFs(dir, name), &bytes)) {
        if (n == 0) break;  // this class has no loose files
        break;
      }
      Sample s;
      s.cls = kClasses[c];
      s.label = name;
      s.bytes = std::move(bytes);
      corpus->samples.push_back(std::move(s));
      ++found;
    }
  }
  for (const char* kind : {"valid", "nested-doll", "promo-noise"}) {
    std::string bytes;
    const std::string path = JoinFs(JoinFs(dir, "control"), std::string(kind) + ".zip");
    if (!ReadFile(path, &bytes)) continue;
    Sample s;
    s.control = true;
    s.cls = "control";
    s.label = std::string("control-") + kind;
    s.bytes = std::move(bytes);
    corpus->controls.push_back(std::move(s));
  }
  return found > 0;
}

bool LoadControlDir(const std::string& dir, Corpus* corpus) {
  for (const char* kind : {"valid", "nested-doll", "promo-noise"}) {
    std::string bytes;
    const std::string path = JoinFs(JoinFs(dir, "control"), std::string(kind) + ".zip");
    if (!ReadFile(path, &bytes)) continue;
    Sample s;
    s.control = true;
    s.cls = "control";
    s.label = std::string("control-") + kind;
    s.bytes = std::move(bytes);
    corpus->controls.push_back(std::move(s));
  }
  return corpus->controls.size() == 3;
}

// ---------------------------------------------------------------------------
// Per-sample analysis: everything the assertions need, computed once.
// ---------------------------------------------------------------------------
struct FuseResult {
  bool tripped = false;
  bool critical = false;
  std::string reason;
};

struct Analysis {
  ProbeResult probe;
  std::vector<Verdict> verdicts;
  FuseResult fuse;
  size_t crcMismatches = 0;
  bool planReasonNonEmpty = false;
  bool planStrategyValid = false;
  bool planCountsConsistent = false;
  bool planDestInsideRoot = false;
  bool transformsContained = true;
  bool collisionsUnique = true;
  bool collisionsExplained = true;
  size_t cleanEntries = 0;
  size_t hostileEntries = 0;
  size_t overlongEntries = 0;
  size_t deepEntries = 0;
  size_t invalidUtf8Entries = 0;
  size_t validUtf8Entries = 0;
  size_t encryptedEntries = 0;
};

bool StrategyIsKnown(prism::SmartStrategy s) {
  switch (s) {
    case prism::SmartStrategy::EmptyArchive:
    case prism::SmartStrategy::FlatDirectly:
    case prism::SmartStrategy::StripRedundantNestedRoot:
    case prism::SmartStrategy::WrapIntoNewNamedSubfolder:
    case prism::SmartStrategy::WrapAndFilterPromoNoise:
      return true;
  }
  return false;
}

size_t CrcMismatches(const ProbeResult& probe, const std::string& bytes) {
  size_t bad = 0;
  for (const ZipEntryProbe& e : probe.entries) {
    if (e.compSize == 0) {
      if (e.crc != 0) ++bad;
      continue;
    }
    if (e.dataOffset > bytes.size() || e.dataLen != e.compSize) continue;  // not judgeable
    if (Crc32(bytes.substr(static_cast<size_t>(e.dataOffset), static_cast<size_t>(e.compSize))) !=
        e.crc) {
      ++bad;
    }
  }
  return bad;
}

FuseResult RunBombFuse(const ProbeResult& probe) {
  FuseResult out;
  prism::BombFuse fuse;  // shipped defaults: 1000:1, 64 MiB sample floor, 0.85 disk
  const uint64_t freeDisk = 100ull << 20;
  for (const ZipEntryProbe& e : probe.entries) {
    if (e.uncompSize < (64ull << 20)) continue;  // too small to judge a ratio
    fuse.BeginFile(e.compSize == 0 ? 1u : e.compSize);
    fuse.Feed(0, e.uncompSize, freeDisk);
  }
  out.tripped = fuse.tripped();
  out.critical = fuse.critical();
  out.reason = fuse.reason();
  return out;
}

Analysis Analyze(const Sample& sample) {
  Analysis a;
  a.probe = ProbeZip(sample.bytes);
  const FuzzGate gated;
  const std::string outDir = "/prism-fuzz-out";

  for (const ZipEntryProbe& e : a.probe.entries) {
    a.verdicts.push_back(gated.Decide(e));
  }
  a.crcMismatches = CrcMismatches(a.probe, sample.bytes);
  a.fuse = RunBombFuse(a.probe);

  prism::ArchiveCatalog catalog;
  for (size_t i = 0; i < a.probe.entries.size(); ++i) {
    const ZipEntryProbe& e = a.probe.entries[i];
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = e.name;
    item.size = e.uncompSize;
    item.packSize = e.compSize;
    item.isDir = e.isDir;
    item.encrypted = e.encrypted;
    catalog.Add(std::move(item));
  }
  const prism::SmartPlan plan = prism::SmartExtractionPlanner::Build(
      "fuzz/" + sample.label + ".zip", outDir, catalog.Items(),
      [](const std::string&) { return false; });
  a.planReasonNonEmpty = !plan.reason.empty();
  a.planStrategyValid = StrategyIsKnown(plan.strategy);
  a.planDestInsideRoot =
      plan.baseTargetDir == outDir || prism::IsContainedIn(outDir, plan.baseTargetDir);

  uint64_t expectedBytes = 0;
  size_t expectedFiles = 0;
  bool overflow = false;
  for (const prism::ArchiveItem& item : catalog.Items()) {
    if (plan.ignoredIndices.count(item.index) != 0) continue;
    if (item.isDir) continue;
    ++expectedFiles;
    if (item.size > UINT64_MAX - expectedBytes) overflow = true;
    expectedBytes += item.size;
  }
  a.planCountsConsistent =
      !overflow && expectedFiles == plan.fileCount && expectedBytes == plan.totalBytes;

  // Every item's final path must stay inside the plan's destination, and the
  // collision resolver must never hand back an empty name without a reason.
  std::set<std::string> taken;
  prism::CollisionResolver resolver([](const std::string&) { return false; });
  for (const prism::ArchiveItem& item : catalog.Items()) {
    const std::string finalPath = prism::SmartExtractionPlanner::TransformItemPath(plan, item.path);
    const bool contained = finalPath == plan.baseTargetDir ||
                           prism::IsContainedIn(plan.baseTargetDir, finalPath);
    if (!contained) a.transformsContained = false;
    for (const std::string& part : prism::SplitPath(prism::NormalizeArchivePath(finalPath))) {
      if (part == "..") a.transformsContained = false;
    }
    if (item.isDir) continue;
    const std::string resolved = resolver.Resolve(finalPath, /*isDirectory=*/false);
    if (resolved.empty()) {
      if (!resolver.error()) a.collisionsExplained = false;
      continue;
    }
    if (!taken.insert(resolved).second) a.collisionsUnique = false;
  }

  for (const ZipEntryProbe& e : a.probe.entries) {
    const std::string normalized = prism::NormalizeArchivePath(e.name);
    if (e.encrypted) ++a.encryptedEntries;
    if (prism::IsUtf8Valid(e.name)) {
      ++a.validUtf8Entries;
    } else {
      ++a.invalidUtf8Entries;
    }
    if (LooksHostile(e.name)) ++a.hostileEntries;
    if (HasOverlongComponent(normalized)) ++a.overlongEntries;
    if (DepthOf(normalized) > kMaxDepth) ++a.deepEntries;
    if (e.encrypted) continue;
    if (!prism::IsUtf8Valid(e.name)) continue;
    if (LooksHostile(e.name)) continue;
    const std::string safe = prism::SanitizeRelativePath(e.name);
    if (safe.empty() || safe != e.name) continue;
    if (HasOverlongComponent(safe) || DepthOf(safe) > kMaxDepth) continue;
    if (prism::SanitizeWindowsName(std::string(prism::FileNameOf(safe))) !=
        std::string(prism::FileNameOf(safe))) {
      continue;
    }
    ++a.cleanEntries;
  }
  return a;
}

std::vector<Analysis>& Analyses() {
  static std::vector<Analysis> cache;
  if (cache.size() != g_corpus.samples.size()) {
    cache.clear();
    cache.reserve(g_corpus.samples.size());
    for (const Sample& s : g_corpus.samples) cache.push_back(Analyze(s));
  }
  return cache;
}

// A red build prints every failure, but not forever: the accept-all mutant
// trips ~10^5 assertions, and 25 s of stdout is CI time spent on nothing. The
// first kMaxReportedFailures lines carry the reasons; the rest are counted.
constexpr size_t kMaxReportedFailures = 40;

bool Expect(bool cond, const std::string& what) {
  ++::prism::test::CheckCount();
  if (cond) return true;
  static size_t reported = 0;
  ++reported;
  if (reported <= kMaxReportedFailures) {
    ::prism::test::ReportFailure(__FILE__, __LINE__, what);
  } else if (reported == kMaxReportedFailures + 1) {
    ::prism::test::ReportFailure(__FILE__, __LINE__,
                                 "further failures suppressed after " +
                                     std::to_string(kMaxReportedFailures) +
                                     " -- this build is red (counts still accumulate)");
  }
  return false;
}

const char* Name(Verdict const& v) { return OutcomeName(v.outcome); }

}  // namespace

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
PRISM_TEST("corpus: container parses and the class table matches this binary") {
  Expect(g_corpus.loaded, "corpus was not loaded: " + g_corpus.error);
  Expect(g_corpus.error.empty(), "corpus load error: " + g_corpus.error);
  Expect(g_corpus.samples.size() >= kClassCount * kMinSamplesPerClass,
         "corpus has " + std::to_string(g_corpus.samples.size()) + " samples, expected at least " +
             std::to_string(kClassCount * kMinSamplesPerClass));
  Expect(g_corpus.controls.size() == 3,
         "corpus must ship the 3 control archives (valid / nested-doll / promo-noise), got " +
             std::to_string(g_corpus.controls.size()));
  for (size_t c = 0; c < kClassCount; ++c) {
    const size_t n = static_cast<size_t>(std::count_if(
        g_corpus.samples.begin(), g_corpus.samples.end(),
        [c](const Sample& s) { return s.cls == kClasses[c]; }));
    Expect(n >= kMinSamplesPerClass,
           std::string("class '") + kClasses[c] + "' has " + std::to_string(n) +
               " samples, below the generator contract of " +
               std::to_string(kMinSamplesPerClass));
  }
}

PRISM_TEST("invariants: every verdict, plan and transformed path is safe") {
  auto& analyses = Analyses();
  Expect(analyses.size() == g_corpus.samples.size(), "analysis pass skipped samples");
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    const Sample& s = g_corpus.samples[i];
    const Analysis& a = analyses[i];

    // 1. A walk failure must be explained, never silent (D1 spirit).
    if (!a.probe.eocd || !a.probe.error.empty()) {
      Expect(!a.probe.error.empty(),
             s.label + ": the walker failed without a reason (silent truncation)");
    }
    Expect(a.probe.entries.size() <= kMaxEntriesPerSample,
           s.label + ": entry count above the sanity bound");

    // 2. One verdict per entry, each with a non-empty, specific reason.
    Expect(a.verdicts.size() == a.probe.entries.size(),
           s.label + ": verdict count does not match the entry count");
    for (size_t k = 0; k < a.verdicts.size(); ++k) {
      const Verdict& v = a.verdicts[k];
      Expect(!v.reason.empty(), s.label + ": verdict without a reason");
      Expect(v.reason.size() >= 8, s.label + ": verdict reason is a placeholder ('" + v.reason + "')");
      if (v.outcome == Outcome::Reject) {
        Expect(v.finalPath.empty(),
               s.label + ": a rejected entry must not carry a final path");
      }
      if (v.outcome == Outcome::Rewrite || v.outcome == Outcome::Accept) {
        Expect(prism::IsContainedIn("/prism-fuzz-out", v.finalPath) ||
                   v.finalPath == "/prism-fuzz-out",
               s.label + ": accepted/rewritten path '" + v.finalPath + "' is outside the root");
      }
    }

    // 3. The two-sided contract: clean names are accepted, hostile ones are not.
    for (size_t k = 0; k < a.probe.entries.size(); ++k) {
      const ZipEntryProbe& e = a.probe.entries[k];
      const Verdict& v = a.verdicts[k];
      const std::string normalized = prism::NormalizeArchivePath(e.name);
      const bool overlong = HasOverlongComponent(normalized);
      const bool tooDeep = DepthOf(normalized) > kMaxDepth;
      if (e.encrypted) {
        Expect(v.outcome == Outcome::NeedsPassword,
               s.label + ": encrypted entry must be surfaced as NeedsPassword, got " +
                   std::string(Name(v)));
      } else if (!prism::IsUtf8Valid(e.name)) {
        Expect(v.outcome != Outcome::Accept,
               s.label + ": invalid UTF-8 name was accepted as a path");
      } else if (LooksHostile(e.name)) {
        Expect(v.outcome != Outcome::Accept,
               s.label + ": hostile name '" + e.name + "' was accepted untouched");
      }
      if (overlong) {
        Expect(v.outcome == Outcome::Reject,
               s.label + ": overlong path was not rejected (got " + Name(v) + ")");
      }
      if (tooDeep) {
        Expect(v.outcome == Outcome::Reject,
               s.label + ": over-deep path was not rejected (got " + Name(v) + ")");
      }
    }
    for (size_t k = 0; k < a.probe.entries.size(); ++k) {
      const ZipEntryProbe& e = a.probe.entries[k];
      const Verdict& v = a.verdicts[k];
      if (e.encrypted || !prism::IsUtf8Valid(e.name) || LooksHostile(e.name)) continue;
      const std::string safe = prism::SanitizeRelativePath(e.name);
      if (safe.empty() || safe != e.name) continue;
      if (HasOverlongComponent(safe) || DepthOf(safe) > kMaxDepth) continue;
      Expect(v.outcome == Outcome::Accept,
             s.label + ": a clean entry was not accepted (over-rejection makes every "
                       "rejection assertion meaningless)");
    }

    // 4. The smart-extraction plan stays well formed (review item 5.5 / C6).
    Expect(a.planReasonNonEmpty, s.label + ": smart plan has no reason");
    Expect(a.planStrategyValid, s.label + ": smart plan has an unknown strategy");
    Expect(a.planDestInsideRoot, s.label + ": smart plan destination escapes the output dir");
    Expect(a.planCountsConsistent, s.label + ": smart plan counts disagree with the catalog");
    Expect(a.transformsContained,
           s.label + ": a transformed path escaped the plan destination (Zip Slip)");
    Expect(a.collisionsUnique, s.label + ": the collision resolver produced a duplicate name");
    Expect(a.collisionsExplained,
           s.label + ": the collision resolver failed without setting an error");
  }
}

PRISM_TEST("class: truncated -- no crash, and a failed walk always has a reason") {
  auto& analyses = Analyses();
  size_t unparsed = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "truncated") continue;
    const Analysis& a = analyses[i];
    if (!a.probe.eocd || !a.probe.error.empty()) {
      ++unparsed;
      Expect(!a.probe.error.empty(), g_corpus.samples[i].label + ": silent walk failure");
    }
  }
  // The family is *defined* by truncation: if nothing failed to parse, the
  // samples stopped being truncated and this test would be vacuous.
  Expect(unparsed > 0, "truncated family contains no unparseable sample");
}

PRISM_TEST("class: bad-crc -- the CRC detector fires here and not on the control") {
  auto& analyses = Analyses();
  size_t withMismatch = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "bad-crc") continue;
    if (analyses[i].crcMismatches > 0) ++withMismatch;
    Expect(analyses[i].probe.eocd || !analyses[i].probe.error.empty(),
           g_corpus.samples[i].label + ": walker failed without a reason");
  }
  Expect(withMismatch > 0, "bad-crc family produced no CRC mismatch -- the detector is asleep");
}

PRISM_TEST("class: overlong-path and deep-nesting are refused where they exceed a limit") {
  auto& analyses = Analyses();
  size_t overlong = 0;
  size_t deep = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    const Sample& s = g_corpus.samples[i];
    if (s.cls != "overlong-path" && s.cls != "deep-nesting") continue;
    overlong += analyses[i].overlongEntries;
    deep += analyses[i].deepEntries;
  }
  Expect(overlong > 0, "overlong-path family contains no over-long component");
  Expect(deep > 0, "deep-nesting family contains no over-deep path");
}

PRISM_TEST("class: traversal -- every hostile name is rewritten or refused") {
  auto& analyses = Analyses();
  size_t total = 0;
  size_t hostile = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "traversal") continue;
    ++total;
    hostile += analyses[i].hostileEntries;
    for (size_t k = 0; k < analyses[i].verdicts.size(); ++k) {
      const Verdict& v = analyses[i].verdicts[k];
      if (v.outcome != Outcome::Accept) continue;
      // An accepted entry must be byte-for-byte a clean relative path.
      const ZipEntryProbe& e = analyses[i].probe.entries[k];
      Expect(!LooksHostile(e.name),
             g_corpus.samples[i].label + ": traversal entry '" + e.name + "' was accepted");
    }
  }
  Expect(hostile > 0, "traversal family contains no hostile name");
  Expect(total > 0, "traversal family is empty");
}

PRISM_TEST("class: fake-size -- the bomb fuse trips with a reason") {
  auto& analyses = Analyses();
  size_t samples = 0;
  size_t tripped = 0;
  size_t critical = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "fake-size") continue;
    ++samples;
    const Analysis& a = analyses[i];
    if (a.fuse.tripped) {
      ++tripped;
      Expect(!a.fuse.reason.empty(),
             g_corpus.samples[i].label + ": the bomb fuse tripped without a reason");
      if (a.fuse.critical) ++critical;
    }
  }
  Expect(samples > 0, "fake-size family is empty");
  Expect(tripped == samples,
         "the bomb fuse stayed silent on " + std::to_string(samples - tripped) +
             " declared-huge samples");
  Expect(critical > 0, "no sample produced the blueprint's dual-condition (ratio AND disk) alert");
}

PRISM_TEST("class: bad-utf8 -- mojibake never becomes a path unchecked") {
  auto& analyses = Analyses();
  size_t invalid = 0;
  size_t valid = 0;
  size_t repaired = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "bad-utf8") continue;
    invalid += analyses[i].invalidUtf8Entries;
    valid += analyses[i].validUtf8Entries;
    for (const ZipEntryProbe& e : analyses[i].probe.entries) {
      if (prism::IsUtf8Valid(e.name)) continue;
      // The repair pipeline may be unavailable (no iconv in a Windows build of
      // core); when it does claim a repair, its output must be valid UTF-8.
      std::unique_ptr<prism::ITranscoder> transcoder = prism::CreateSystemTranscoder();
      const prism::RepairResult repairedName =
          prism::RepairArchiveName(e.name, /*utf8Flagged=*/false, transcoder.get());
      if (repairedName.repaired) {
        ++repaired;
        Expect(prism::IsUtf8Valid(repairedName.name),
               g_corpus.samples[i].label + ": the repair pipeline produced invalid UTF-8");
      }
    }
  }
  // Both halves must be present: an always-false UTF-8 check would be as blind
  // as an always-true one, and the family deliberately also ships valid names.
  Expect(invalid > 0, "bad-utf8 family contains no invalid name");
  Expect(valid > 0, "bad-utf8 family contains no valid name -- the check cannot discriminate");
  (void)repaired;
}

PRISM_TEST("class: empty and zero-byte keep the planner honest") {
  auto& analyses = Analyses();
  size_t emptySamples = 0;
  size_t zeroSamples = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    const Sample& s = g_corpus.samples[i];
    const Analysis& a = analyses[i];
    if (s.cls == "empty") {
      ++emptySamples;
      if (a.probe.entries.empty()) {
        const prism::SmartPlan plan = prism::SmartExtractionPlanner::Build(
            "fuzz/" + s.label + ".zip", "/prism-fuzz-out", {},
            [](const std::string&) { return false; });
        Expect(plan.strategy == prism::SmartStrategy::EmptyArchive,
               s.label + ": an entry-less archive was not planned as EmptyArchive");
        Expect(!plan.reason.empty(), s.label + ": EmptyArchive plan has no reason");
      }
    } else if (s.cls == "zero-byte") {
      ++zeroSamples;
      for (const ZipEntryProbe& e : a.probe.entries) {
        Expect(e.uncompSize == 0 && e.compSize == 0,
               s.label + ": zero-byte family entry carries a non-zero size");
        Expect(e.crc == 0, s.label + ": zero-byte entry has a non-zero CRC");
      }
      Expect(a.crcMismatches == 0, s.label + ": zero-byte entries failed the CRC check");
    }
  }
  Expect(emptySamples > 0, "empty family is empty");
  Expect(zeroSamples > 0, "zero-byte family is empty");
}

PRISM_TEST("class: encrypted -- password is required, never silently planned") {
  auto& analyses = Analyses();
  size_t encrypted = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "encrypted") continue;
    encrypted += analyses[i].encryptedEntries;
  }
  Expect(encrypted > 0, "encrypted family contains no encrypted entry");
}

PRISM_TEST("class: zip64-lies -- placeholder sizes never overflow a plan") {
  auto& analyses = Analyses();
  size_t samples = 0;
  size_t placeholders = 0;
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    if (g_corpus.samples[i].cls != "zip64-lies") continue;
    ++samples;
    for (const ZipEntryProbe& e : analyses[i].probe.entries) {
      if (e.sizeIsPlaceholder) ++placeholders;
    }
    // The plan's byte total is asserted overflow-free in the invariants test;
    // here we require that the lie was actually seen by the walker.
    Expect(analyses[i].planCountsConsistent,
           g_corpus.samples[i].label + ": plan counts inconsistent with placeholder sizes");
  }
  Expect(samples > 0, "zip64-lies family is empty");
  Expect(placeholders > 0, "zip64-lies family produced no 0xFFFFFFFF size field");
}

PRISM_TEST("negative control: the genuinely valid archives are ACCEPTED") {
  Expect(g_corpus.controls.size() == 3, "the three control archives are missing");
  const bool expectAccept = !kExpectValidRejected;
  for (const Sample& s : g_corpus.controls) {
    const Analysis a = Analyze(s);
    Expect(a.probe.error.empty(), s.label + ": the valid control does not parse: " + a.probe.error);
    Expect(a.crcMismatches == 0, s.label + ": the valid control fails its own CRC check");
    Expect(!a.probe.entries.empty(), s.label + ": the valid control has no entries");
    Expect(a.planReasonNonEmpty, s.label + ": the valid control has no plan reason");
    for (size_t k = 0; k < a.verdicts.size(); ++k) {
      const Verdict& v = a.verdicts[k];
      const bool accepted = v.outcome == Outcome::Accept;
      // The mutant build inverts THIS expectation. Because the sample is a real,
      // well-formed archive, a rule that refuses it must be reported -- that is
      // how "expected reject, but it passed" becomes red instead of a pass.
      Expect(accepted == expectAccept,
             s.label + ": CONTROL-VIOLATION entry '" + a.probe.entries[k].name +
                 "' -> " + std::string(Name(v)) + " (" + v.reason + ")");
    }
  }
}

// ---------------------------------------------------------------------------
// main: argument handling, corpus loading, then mini_test's runner.
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
  if (kAcceptAllGuard) std::cout << "FUZZ MUTATION ARMED: accept-all" << std::endl;
  if (kExpectValidRejected) std::cout << "FUZZ MUTATION ARMED: expect-valid-rejected" << std::endl;

  std::string corpusDir;
  long expectedTotal = 0;
  bool selfReportOnly = false;
  std::string selfReportWant;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg.rfind("--self-report-only", 0) == 0) {
      selfReportOnly = true;
      const size_t eq = arg.find('=');
      if (eq != std::string::npos) selfReportWant = arg.substr(eq + 1);
    } else if (corpusDir.empty()) {
      corpusDir = arg;
    } else {
      expectedTotal = std::atol(arg.c_str());
    }
  }
  if (selfReportOnly) {
    // Proves the compile-time injection really happened (CONTRACTS D3): the
    // mutant binary must announce itself, so a typo'd definition cannot turn
    // WILL_FAIL into a silent pass. The exit code carries the verdict too, so
    // this is decidable even if a runner only looks at the status.
    std::cout << "FUZZ MUTATION SELF-REPORT: "
              << (MutationArmed() ? "armed" : "NOT armed (clean build)") << std::endl;
    if (selfReportWant == "armed") return MutationArmed() ? 0 : 3;
    if (selfReportWant == "clean") return MutationArmed() ? 3 : 0;
    return MutationArmed() ? 0 : 3;
  }

  if (corpusDir.empty()) {
    std::cout << "FUZZ SMOKE SKIPPED: no corpus directory given" << std::endl;
    return 77;
  }
  std::string packed;
  if (ReadFile(JoinFs(corpusDir, "corpus.bin"), &packed)) {
    g_corpus.loaded = LoadPacked(JoinFs(corpusDir, "corpus.bin"), &g_corpus);
    g_corpus.loaded = g_corpus.loaded && LoadControlDir(corpusDir, &g_corpus);
  } else {
    g_corpus.loaded = LoadLoose(corpusDir, &g_corpus);
    if (g_corpus.loaded) g_corpus.fnv = 0;
  }
  if (!g_corpus.loaded) {
    std::cout << "FUZZ SMOKE SKIPPED: corpus missing in " << corpusDir
              << " (" << (g_corpus.error.empty() ? "no corpus.bin and no loose samples"
                                                 : g_corpus.error)
              << ")" << std::endl;
    return 77;
  }

  std::cout << "FUZZ CORPUS: " << g_corpus.samples.size() << " samples, "
            << g_corpus.controls.size() << " controls, fnv1a=0x" << std::hex << g_corpus.fnv
            << std::dec << std::endl;
  if (expectedTotal > 0) {
    if (!Expect(static_cast<long>(g_corpus.samples.size()) == expectedTotal,
                "corpus carries " + std::to_string(g_corpus.samples.size()) +
                    " samples but the runner expected " + std::to_string(expectedTotal))) {
      std::cout << "FUZZ CORPUS MISMATCH" << std::endl;
      return 1;
    }
  }

  const int rc = ::prism::test::RunAll(argc, argv);

  // Per-class table: the run page must show what was exercised and how the
  // decision layer answered, not just "ok". A class whose every entry is
  // "Accept" would be a corpus bug, and this table is where that shows up.
  struct Row {
    size_t samples = 0, entries = 0, accept = 0, rewrite = 0, reject = 0, password = 0;
    size_t unparsed = 0, fuse = 0;
  };
  std::map<std::string, Row> rows;
  const auto& analyses = Analyses();
  for (size_t i = 0; i < g_corpus.samples.size(); ++i) {
    Row& row = rows[g_corpus.samples[i].cls];
    ++row.samples;
    const Analysis& a = analyses[i];
    row.entries += a.probe.entries.size();
    if (!a.probe.eocd || !a.probe.error.empty()) ++row.unparsed;
    if (a.fuse.tripped) ++row.fuse;
    for (const Verdict& v : a.verdicts) {
      switch (v.outcome) {
        case Outcome::Accept: ++row.accept; break;
        case Outcome::Rewrite: ++row.rewrite; break;
        case Outcome::Reject: ++row.reject; break;
        case Outcome::NeedsPassword: ++row.password; break;
      }
    }
  }
  std::cout << "  --- corpus distribution (samples/entries accept+rewrite+reject+pw "
               "| unparsed | fuse) ---"
            << std::endl;
  for (size_t c = 0; c < kClassCount; ++c) {
    const Row& row = rows[kClasses[c]];
    std::cout << "  " << kClasses[c] << ": " << row.samples << "/" << row.entries << " "
              << row.accept << "+" << row.rewrite << "+" << row.reject << "+" << row.password
              << " | unparsed=" << row.unparsed << " | fuse=" << row.fuse << std::endl;
  }
  if (rc == 0) std::cout << "ALL FUZZ CHECKS PASSED" << std::endl;
  return rc;
}