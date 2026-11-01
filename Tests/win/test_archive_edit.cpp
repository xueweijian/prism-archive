// PrismArchive -- editing an EXISTING archive against the real 7z.dll (card C51).
//
// The blueprint promises "rename / delete / new folder / move WITHOUT extracting
// the archive". This suite creates a real 5-entry solid .7z whose names are
// Chinese and which contains a directory and a ZERO-BYTE file, runs each of the
// four operations on its own copy, and after EVERY operation RE-OPENS the new
// archive and compares, entry by entry:
//   name, kind, size, stored CRC32, the encrypted flag, the solid flag, and the
//   payload BYTES (D2: content, not existence).
//
// Then the boundary cases, each with an explicit verdict instead of a crash:
//   read-only archive          -> refused, the reason names the attribute
//   held by another owner      -> refused, the reason names the lock
//   part of a multi-volume set -> refused, the reason names the volume set
//   deleting a non-empty dir   -> refused unless recurse=true
//   a CANCELLED edit           -> failed, and the ORIGINAL is byte-for-byte intact
//   a malformed catalog name ("", ".") -> refused by the same decision function
//   a 0-byte entry             -> survives a rename/move with size 0
//   an unknown container       -> refused instead of rewritten as a guess
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). This source also builds twice
// with ONE rule deleted:
//   keep_deleted   -> the deleted-entry filter is compiled out (the entry comes
//                     back in the new archive);
//   broken_publish -> "a failed edit leaves the original alone" is compiled out
//                     (the temp is published over the original anyway).
// Each binary arms SetMutationVerdict(prefix, predicate) BEFORE the first case, so
// the FIRST failed assertion prints "C51 MUTATION CAUGHT: <arm>" and leaves with 0
// -- and only while the injection counter proves the defect actually fired. The
// CTest entry pins that marker (never WILL_FAIL: D26), so a stale injection stays
// RED instead of quietly green.
//
// Exit policy (D1): the success marker is printed only when every check passed, and
// a missing 7z.dll prints a SKIP line and returns 77 (SKIP_RETURN_CODE 77).
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "ArchiveEditor.hpp"
#include "PrismEngineBridge.hpp"
#include "UniqueTempDir.hpp"
#include "../mini_test.hpp"

namespace fs = std::filesystem;
namespace edit = prism::win::edit;

namespace {

// --- negative-control bookkeeping (CONTRACTS D3) ---------------------------
int gInjectedKeepDeleted = 0;
int gInjectedBrokenPublish = 0;

#if defined(PRISM_C51_MUTANT_KEEP_DELETED)
// MUTANT (D3): "the user deleted this" is gone. The literal sits inside the
// guard on purpose -- tools/mutation/mutant_matrix.py looks for a MUTANT ACTIVE
// string within 800 bytes of the guard as proof the injection can build.
static const char* kMutantBanner =
    "C51 MUTANT ACTIVE: keep_deleted (the deleted-entry filter is compiled out)";
#elif defined(PRISM_C51_MUTANT_BROKEN_PUBLISH)
static const char* kMutantBanner =
    "C51 MUTANT ACTIVE: broken_publish (a failed edit publishes the temp anyway)";
#else
static const char* kMutantBanner = nullptr;
#endif

constexpr const wchar_t* kPassword = L"c51-密码-2026";

prism::win::SevenZipLibrary gLibrary;
prism::testtmp::UniqueDir* gScratch = nullptr;

void Emit(const std::string& line) { std::printf("%s\n", line.c_str()); }

std::wstring Join(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 131 + seed * 17 + (i >> 3)) & 0xFF);
  }
  return bytes;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>((std::min)(size_t{1} << 16, bytes.size() - done));
    if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
      CloseHandle(h);
      return false;
    }
    done += wrote;
  }
  CloseHandle(h);
  return true;
}

// --- one archive, fully described ------------------------------------------
struct Snapshot {
  std::vector<edit::EntryFingerprint> entries;
  std::map<std::string, std::vector<uint8_t>> payload;  // files only
  std::map<std::string, edit::EntryFingerprint> byPath;
};

bool TakeSnapshot(const std::wstring& path, const std::wstring& password, Snapshot* out,
                  std::string* why) {
  out->entries.clear();
  out->payload.clear();
  out->byPath.clear();
  if (!edit::FingerprintArchive(gLibrary, path, password, &out->entries, why)) return false;
  for (const auto& entry : out->entries) out->byPath[entry.path] = entry;
  // EntryExtractor answers ICryptoGetTextPassword, so the payload comparison is
  // a real byte comparison for an ENCRYPTED fixture too (ArchiveReader's
  // in-memory sink cannot serve an encrypted payload -- the C9 finding).
  edit::EntryExtractor extractor(gLibrary);
  if (!extractor.Open(path, password, why)) return false;
  for (size_t i = 0; i < out->entries.size(); ++i) {
    const edit::EntryFingerprint& entry = out->entries[i];
    if (entry.isDir) continue;
    std::vector<uint8_t> bytes;
    std::string extractWhy;
    if (!extractor.Extract(static_cast<uint32_t>(i), entry.size, &bytes, &extractWhy)) {
      if (why != nullptr) *why = "cannot extract '" + entry.path + "' (" + extractWhy + ")";
      return false;
    }
    if (bytes.size() != entry.size) {
      if (why != nullptr) *why = "the payload of '" + entry.path + "' is shorter than its size";
      return false;
    }
    out->payload[entry.path] = std::move(bytes);
  }
  return true;
}

// Every kept entry must arrive with the same bytes, CRC, kind, size and flags.
bool SameKept(const Snapshot& before, const Snapshot& after, const std::string& oldPath,
              const std::string& newPath, std::string* why) {
  const auto oldEntry = before.byPath.find(oldPath);
  if (oldEntry == before.byPath.end()) {
    *why = "fixture problem: '" + oldPath + "' was not in the original";
    return false;
  }
  const auto newEntry = after.byPath.find(newPath);
  if (newEntry == after.byPath.end()) {
    *why = "'" + newPath + "' is missing from the rewritten archive";
    return false;
  }
  if (oldEntry->second.isDir != newEntry->second.isDir) {
    *why = "'" + newPath + "' changed kind";
    return false;
  }
  if (oldEntry->second.isDir) return true;
  if (oldEntry->second.size != newEntry->second.size) {
    *why = "'" + newPath + "' changed size (" + std::to_string(oldEntry->second.size) + " -> " +
           std::to_string(newEntry->second.size) + ")";
    return false;
  }
  if (oldEntry->second.hasCrc && newEntry->second.hasCrc &&
      oldEntry->second.crc != newEntry->second.crc) {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "'%s' CRC changed (0x%08lX -> 0x%08lX)", newPath.c_str(),
                  static_cast<unsigned long>(oldEntry->second.crc),
                  static_cast<unsigned long>(newEntry->second.crc));
    *why = buf;
    return false;
  }
  if (oldEntry->second.encrypted != newEntry->second.encrypted) {
    *why = "'" + newPath + "' changed its encrypted flag";
    return false;
  }
  const auto oldBytes = before.payload.find(oldPath);
  const auto newBytes = after.payload.find(newPath);
  if (oldBytes == before.payload.end() || newBytes == after.payload.end()) {
    *why = "'" + newPath + "' has no payload in one of the two snapshots";
    return false;
  }
  if (oldBytes->second != newBytes->second) {
    *why = "'" + newPath + "' payload bytes differ";
    return false;
  }
  return true;
}

std::vector<std::string> Names(const Snapshot& snapshot) {
  std::vector<std::string> names;
  for (const auto& entry : snapshot.entries) names.push_back(entry.path);
  std::sort(names.begin(), names.end());
  return names;
}

std::string JoinNames(const std::vector<std::string>& names);

// The rewritten archive must hold EXACTLY the expected names. Sorted byte order,
// so the comparison never depends on the order 7z chose (CONTRACTS A7).
bool SameNameSet(const Snapshot& after, std::vector<std::string> expected, std::string* why) {
  std::sort(expected.begin(), expected.end());
  const std::vector<std::string> got = Names(after);
  if (got == expected) return true;
  *why = "got {" + JoinNames(got) + "} but expected {" + JoinNames(expected) + "}";
  return false;
}

// Solid = files packed into fewer blocks than files (see EntryFingerprint::solid:
// this engine answers kpidSolid=false even for a solid one-folder archive).
bool ArchiveIsSolid(const Snapshot& snapshot) {
  std::vector<uint32_t> blocks;
  size_t files = 0;
  for (const auto& entry : snapshot.entries) {
    if (entry.isDir) continue;
    ++files;
    if (std::find(blocks.begin(), blocks.end(), entry.block) == blocks.end()) {
      blocks.push_back(entry.block);
    }
  }
  return files >= 2 && blocks.size() < files;
}

std::string JoinNames(const std::vector<std::string>& names) {
  std::string out;
  for (const std::string& name : names) {
    if (!out.empty()) out += ", ";
    out += name;
  }
  return out;
}

// --- fixture ---------------------------------------------------------------
struct Fixtures {
  bool ready = false;
  std::string error;
  std::wstring plain;      // solid, unencrypted
  std::wstring encrypted;  // solid + payload AES + header encryption
  std::wstring source;     // the files the fixture was created from
};

bool MakeFixture(const std::wstring& tag, bool encrypt, bool headerEncrypt, bool solid,
                 std::wstring* archivePath, std::wstring* sourceRoot, std::string* why) {
  const std::wstring base = Join(gScratch->wstr(), tag);
  const std::wstring src = Join(base, L"src");
  std::error_code ec;
  fs::create_directories(src, ec);
  if (!fs::is_directory(src, ec)) {
    *why = "cannot create the fixture source directory";
    return false;
  }
  const std::wstring docDir = Join(src, L"资料");
  fs::create_directories(docDir, ec);
  if (!WriteBytes(Join(docDir, L"说明.txt"), Pattern(1024, 1)) ||
      !WriteBytes(Join(docDir, L"图片.bin"), Pattern(4096, 2)) ||
      !WriteBytes(Join(src, L"零字节.txt"), {}) ||
      !WriteBytes(Join(src, L"根文件.log"), Pattern(777, 3))) {
    *why = "cannot write the fixture payloads";
    return false;
  }

  // Five entries: a folder, two Chinese-named files under it, a zero-byte file
  // and one plain name. The folder node is explicit because a directory entry is
  // exactly what a rename/delete must preserve.
  std::vector<prism::PlanItem> items;
  const auto add = [&items](const char* path, bool isDir, uint64_t size) {
    prism::PlanItem item;
    item.path = path;
    item.size = size;
    item.isDir = isDir;
    items.push_back(std::move(item));
  };
  add("资料", true, 0);
  add("资料/说明.txt", false, 1024);
  add("资料/图片.bin", false, 4096);
  add("零字节.txt", false, 0);
  add("根文件.log", false, 777);

  prism::CompressionOptions options;
  options.format = prism::ArchiveFormat::SevenZip;
  options.level = 5;
  options.solid = solid ? prism::SolidMode::On : prism::SolidMode::Off;
  options.encrypt = encrypt;
  options.encryptFileNames = headerEncrypt;
  options.cpuCores = 4;
  const prism::CompressionPlan plan = prism::MakeCompressionPlan(items, options);
  if (!plan.ok) {
    *why = "the fixture plan is not usable (" + plan.error + ")";
    return false;
  }
  *archivePath = Join(base, tag + L".7z");
  *sourceRoot = src;
  prism::win::cw::WriteRequest request;
  request.archivePath = *archivePath;
  request.sourceRoot = src;
  request.plan = plan;
  request.password = encrypt ? kPassword : L"";
  const prism::win::cw::WriteResult wrote = prism::win::cw::CreateArchive(request);
  if (!wrote.ok || !wrote.verified) {
    *why = "the fixture archive could not be created: " + wrote.error;
    return false;
  }
  return true;
}

Fixtures BuildFixtures() {
  Fixtures fixtures;
  if (gScratch == nullptr) {
    fixtures.error = "no scratch directory";
    return fixtures;
  }
  std::wstring ignoredSource;
  if (!MakeFixture(L"plain", false, false, true, &fixtures.plain, &fixtures.source,
                   &fixtures.error)) {
    return fixtures;
  }
  if (!MakeFixture(L"encrypted", true, true, true, &fixtures.encrypted, &ignoredSource,
                   &fixtures.error)) {
    return fixtures;
  }
  fixtures.ready = true;
  return fixtures;
}

Fixtures& Fx() {
  static Fixtures fixtures = BuildFixtures();
  return fixtures;
}

// A private copy per case: one case must never see another case's archive.
std::wstring CopyOf(const std::wstring& source, const std::wstring& tag) {
  const std::wstring target = Join(gScratch->wstr(), tag + L".7z");
  if (!CopyFileW(source.c_str(), target.c_str(), FALSE)) return std::wstring();
  return target;
}

// The executor must leave no ".c51-<pid>-<tick>.tmp" behind on ANY path.
bool NoTempDebris(std::string* found) {
  std::error_code ec;
  for (fs::recursive_directory_iterator it(gScratch->path(), ec), end; !ec && it != end;
       it.increment(ec)) {
    const std::wstring name = it->path().filename().wstring();
    if (name.find(L".c51-") != std::wstring::npos && name.size() > 4 &&
        name.compare(name.size() - 4, 4, L".tmp") == 0) {
      *found = prism::win::WideToUtf8(it->path().wstring());
      return false;
    }
  }
  return true;
}

edit::EditResult Run(const std::wstring& archivePath, std::vector<prism::EditOp> ops,
                     const std::wstring& password = L"",
                     std::function<bool(uint64_t)> progress = nullptr) {
  edit::EditRequest request;
  request.archivePath = archivePath;
  request.password = password;
  request.ops = std::move(ops);
  request.progress = std::move(progress);
  edit::EditResult result = edit::ApplyEdits(request);
  gInjectedKeepDeleted += result.injectedKeepDeleted;
  gInjectedBrokenPublish += result.injectedBrokenPublish;
  Emit("EDIT " + prism::win::WideToUtf8(archivePath) + ": ok=" + (result.ok ? "1" : "0") +
       " verified=" + (result.verified ? "1" : "0") +
       " items=" + std::to_string(result.itemsWritten) +
       " copied=" + std::to_string(result.bytesCopied) + " injected(keepDeleted=" +
       std::to_string(result.injectedKeepDeleted) + ",brokenPublish=" +
       std::to_string(result.injectedBrokenPublish) + ")" +
       (result.error.empty() ? std::string() : " err=" + result.error) + " trace=" + result.trace);
  return result;
}

prism::EditOp Op(prism::EditOpKind kind, const char* path, const char* target = "",
                 bool recurse = false) {
  prism::EditOp op;
  op.kind = kind;
  op.path = path;
  op.target = target;
  op.recurse = recurse;
  return op;
}

}  // namespace

// ---------------------------------------------------------------------------
PRISM_TEST("rename: the entry keeps its bytes under the new Chinese name") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  Snapshot before;
  std::string why;
  PRISM_CHECK(TakeSnapshot(fx.plain, L"", &before, &why));
  const std::wstring target = CopyOf(fx.plain, L"case_rename");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;

  const edit::EditResult result =
      Run(target, {Op(prism::EditOpKind::Rename, "根文件.log", "资料/重命名.log")});
  PRISM_CHECK(result.ok);
  if (!result.ok) return;
  PRISM_CHECK(result.verified);

  Snapshot after;
  PRISM_CHECK(TakeSnapshot(target, L"", &after, &why) || (Emit("WHY: " + why), false));
  PRISM_CHECK(SameNameSet(after,
                          {"资料", "资料/说明.txt", "资料/图片.bin", "资料/重命名.log", "零字节.txt"},
                          &why) || (Emit("WHY: " + why), false));
  PRISM_CHECK(SameKept(before, after, "根文件.log", "资料/重命名.log", &why) ||
              (Emit("WHY: " + why), false));
  for (const char* kept : {"资料/说明.txt", "资料/图片.bin", "零字节.txt"}) {
    PRISM_CHECK(SameKept(before, after, kept, kept, &why) || (Emit("WHY: " + why), false));
  }
  PRISM_CHECK(after.byPath.find("根文件.log") == after.byPath.end());
  std::string debris;
  PRISM_CHECK(NoTempDebris(&debris) || (Emit("DEBRIS: " + debris), false));
}

PRISM_TEST("delete: the entry is gone and every other byte is untouched") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  Snapshot before;
  std::string why;
  PRISM_CHECK(TakeSnapshot(fx.plain, L"", &before, &why));
  const std::wstring target = CopyOf(fx.plain, L"case_delete");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;

  const edit::EditResult result = Run(target, {Op(prism::EditOpKind::Delete, "资料/图片.bin")});
  PRISM_CHECK(result.ok);
  if (!result.ok) return;
  PRISM_CHECK(result.verified);

  Snapshot after;
  PRISM_CHECK(TakeSnapshot(target, L"", &after, &why));
  PRISM_CHECK(SameNameSet(after, {"资料", "资料/说明.txt", "零字节.txt", "根文件.log"}, &why) ||
              (Emit("WHY: " + why), false));
  PRISM_CHECK(after.byPath.find("资料/图片.bin") == after.byPath.end());
  for (const char* kept : {"资料/说明.txt", "零字节.txt", "根文件.log"}) {
    PRISM_CHECK(SameKept(before, after, kept, kept, &why) || (Emit("WHY: " + why), false));
  }
}

PRISM_TEST("new folder: nested directories are inserted, nothing is decoded") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  Snapshot before;
  std::string why;
  PRISM_CHECK(TakeSnapshot(fx.plain, L"", &before, &why));
  const std::wstring target = CopyOf(fx.plain, L"case_newfolder");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;

  const edit::EditResult result =
      Run(target, {Op(prism::EditOpKind::NewFolder, "新目录/子目录")});
  PRISM_CHECK(result.ok);
  if (!result.ok) return;
  PRISM_CHECK(result.verified);

  Snapshot after;
  PRISM_CHECK(TakeSnapshot(target, L"", &after, &why));
  PRISM_CHECK(SameNameSet(after,
                          {"新目录", "新目录/子目录", "资料", "资料/说明.txt", "资料/图片.bin",
                           "零字节.txt", "根文件.log"},
                          &why) || (Emit("WHY: " + why), false));
  for (const char* dir : {"新目录", "新目录/子目录"}) {
    const auto it = after.byPath.find(dir);
    PRISM_CHECK(it != after.byPath.end());
    if (it != after.byPath.end()) PRISM_CHECK(it->second.isDir);
  }
  PRISM_CHECK_EQ(after.payload.size(), before.payload.size());
  for (const char* kept : {"资料/说明.txt", "资料/图片.bin", "零字节.txt", "根文件.log"}) {
    PRISM_CHECK(SameKept(before, after, kept, kept, &why) || (Emit("WHY: " + why), false));
  }
}

PRISM_TEST("move: a zero-byte entry lands in a folder and stays empty") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  Snapshot before;
  std::string why;
  PRISM_CHECK(TakeSnapshot(fx.plain, L"", &before, &why));
  const std::wstring target = CopyOf(fx.plain, L"case_move");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;

  const edit::EditResult result = Run(target, {Op(prism::EditOpKind::Move, "零字节.txt", "资料")});
  PRISM_CHECK(result.ok);
  if (!result.ok) return;
  PRISM_CHECK(result.verified);

  Snapshot after;
  PRISM_CHECK(TakeSnapshot(target, L"", &after, &why));
  PRISM_CHECK(SameNameSet(after,
                          {"资料", "资料/说明.txt", "资料/图片.bin", "资料/零字节.txt", "根文件.log"},
                          &why) || (Emit("WHY: " + why), false));
  PRISM_CHECK(after.byPath.find("零字节.txt") == after.byPath.end());
  PRISM_CHECK(SameKept(before, after, "零字节.txt", "资料/零字节.txt", &why) ||
              (Emit("WHY: " + why), false));
  const auto moved = after.byPath.find("资料/零字节.txt");
  if (moved != after.byPath.end()) {
    PRISM_CHECK_EQ(moved->second.size, uint64_t{0});
    PRISM_CHECK(moved->second.isDir == false);
  }
}

PRISM_TEST("encryption, header encryption and solidity survive a rename") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  Snapshot before;
  std::string why;
  PRISM_CHECK(TakeSnapshot(fx.encrypted, kPassword, &before, &why));
  const std::wstring target = CopyOf(fx.encrypted, L"case_encrypted");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;

  const edit::EditResult result =
      Run(target, {Op(prism::EditOpKind::Rename, "根文件.log", "资料/重命名.log")}, kPassword);
  PRISM_CHECK(result.ok);
  if (!result.ok) return;
  PRISM_CHECK(result.verified);

  // Header encryption: the names must STILL be hidden without the password.
  std::vector<edit::EntryFingerprint> hidden;
  std::string ignored;
  const bool listed = edit::FingerprintArchive(gLibrary, target, L"", &hidden, &ignored);
  PRISM_CHECK(!listed || hidden.empty());

  Snapshot after;
  PRISM_CHECK(TakeSnapshot(target, kPassword, &after, &why));
  size_t encryptedFiles = 0;
  for (const auto& entry : after.entries) {
    if (entry.isDir) continue;
    if (entry.encrypted) ++encryptedFiles;
  }
  // The three non-empty files are AES-encrypted; the 0-byte entry has nothing to
  // encrypt and the engine reports it unencrypted on BOTH sides, which is exactly
  // why the verify compares per entry instead of against the archive-wide flag.
  PRISM_CHECK_EQ(encryptedFiles, std::size_t{3});
  PRISM_CHECK(ArchiveIsSolid(before) || (Emit("WHY: the fixture is not solid"), false));
  PRISM_CHECK(ArchiveIsSolid(after) || (Emit("WHY: the rewrite is not solid"), false));
  PRISM_CHECK(SameKept(before, after, "根文件.log", "资料/重命名.log", &why) ||
              (Emit("WHY: " + why), false));
  for (const char* kept : {"资料/说明.txt", "资料/图片.bin", "零字节.txt"}) {
    PRISM_CHECK(SameKept(before, after, kept, kept, &why) || (Emit("WHY: " + why), false));
  }
  // The wrong password must still not open it.
  std::vector<edit::EntryFingerprint> wrong;
  PRISM_CHECK(!edit::FingerprintArchive(gLibrary, target, L"not-the-password", &wrong, &ignored) ||
              wrong.empty());
}

PRISM_TEST("a cancelled edit fails and leaves the original byte-for-byte intact") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  const std::wstring target = CopyOf(fx.plain, L"case_cancel");
  PRISM_CHECK(!target.empty());
  if (target.empty()) return;
  std::vector<uint8_t> originalBytes;
  PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &originalBytes));
  PRISM_CHECK(originalBytes.size() > 100);

  const edit::EditResult result =
      Run(target, {Op(prism::EditOpKind::Delete, "资料/图片.bin")}, L"",
          [](uint64_t) { return false; });
  PRISM_CHECK(!result.ok);
  PRISM_CHECK(result.error.find("cancel") != std::string::npos);
  PRISM_CHECK(!result.verified);

  // The whole point: the user's archive is the same file it was before.
  std::vector<uint8_t> afterBytes;
  PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &afterBytes));
  PRISM_CHECK_EQ(afterBytes.size(), originalBytes.size());
  PRISM_CHECK(afterBytes == originalBytes);
  std::string debris;
  PRISM_CHECK(NoTempDebris(&debris) || (Emit("DEBRIS: " + debris), false));
}

PRISM_TEST("read-only, locked, multi-volume and non-empty folder are refused") {
  Fixtures& fx = Fx();
  PRISM_CHECK(fx.ready);
  if (!fx.ready) return;
  std::string why;
  std::vector<uint8_t> beforeBytes;

  // ---- read-only ----------------------------------------------------------
  {
    const std::wstring target = CopyOf(fx.plain, L"case_readonly");
    PRISM_CHECK(!target.empty());
    if (!target.empty()) {
      PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &beforeBytes));
      SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
      const edit::EditResult result = Run(target, {Op(prism::EditOpKind::Delete, "根文件.log")});
      PRISM_CHECK(!result.ok);
      PRISM_CHECK(result.error.find("read-only") != std::string::npos);
      PRISM_CHECK(!result.verified);
      SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_NORMAL);
      std::vector<uint8_t> afterBytes;
      PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &afterBytes));
      PRISM_CHECK(afterBytes == beforeBytes);
    }
  }

  // ---- held by another owner (an exclusive handle, exactly as a running
  // archiver would hold it) -------------------------------------------------
  {
    const std::wstring target = CopyOf(fx.plain, L"case_locked");
    PRISM_CHECK(!target.empty());
    if (!target.empty()) {
      HANDLE holder = CreateFileW(target.c_str(), GENERIC_READ, /*no sharing*/ 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      PRISM_CHECK(holder != INVALID_HANDLE_VALUE);
      const edit::EditResult result = Run(target, {Op(prism::EditOpKind::Delete, "根文件.log")});
      if (holder != INVALID_HANDLE_VALUE) CloseHandle(holder);
      PRISM_CHECK(!result.ok);
      PRISM_CHECK(result.error.find("lock") != std::string::npos);
      PRISM_CHECK(!result.verified);
      // The file is still a readable archive: nothing was renamed or truncated.
      Snapshot intact;
      PRISM_CHECK(TakeSnapshot(target, L"", &intact, &why));
      PRISM_CHECK_EQ(intact.entries.size(), std::size_t{5});
    }
  }

  // ---- part of a multi-volume set, by name (the only signal that exists
  // before every sibling is opened) -----------------------------------------
  {
    const std::wstring volume = Join(gScratch->wstr(), L"case_vol.7z.001");
    PRISM_CHECK(CopyFileW(fx.plain.c_str(), volume.c_str(), FALSE) != 0);
    const edit::EditResult result = Run(volume, {Op(prism::EditOpKind::Delete, "根文件.log")});
    PRISM_CHECK(!result.ok);
    PRISM_CHECK(result.error.find("multi-volume") != std::string::npos);
  }
  {
    const std::wstring base = CopyOf(fx.plain, L"case_volset");
    PRISM_CHECK(!base.empty());
    if (!base.empty()) {
      PRISM_CHECK(CopyFileW(base.c_str(), (base + L".001").c_str(), FALSE) != 0);
      const edit::EditResult result = Run(base, {Op(prism::EditOpKind::Delete, "根文件.log")});
      PRISM_CHECK(!result.ok);
      PRISM_CHECK(result.error.find("multi-volume") != std::string::npos);
    }
  }

  // ---- deleting a non-empty folder without recurse ------------------------
  {
    const std::wstring target = CopyOf(fx.plain, L"case_rmdir");
    PRISM_CHECK(!target.empty());
    if (!target.empty()) {
      PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &beforeBytes));
      const edit::EditResult refused = Run(target, {Op(prism::EditOpKind::Delete, "资料")});
      PRISM_CHECK(!refused.ok);
      PRISM_CHECK(refused.error.find("recurse") != std::string::npos);
      std::vector<uint8_t> afterBytes;
      PRISM_CHECK(prism::win::cw::ReadWholeFile(target, &afterBytes));
      PRISM_CHECK(afterBytes == beforeBytes);

      // ...and WITH recurse it is exactly what the user asked for.
      const edit::EditResult recursive =
          Run(target, {Op(prism::EditOpKind::Delete, "资料", "", true)});
      PRISM_CHECK(recursive.ok);
      if (recursive.ok) {
        Snapshot after;
        PRISM_CHECK(TakeSnapshot(target, L"", &after, &why));
        PRISM_CHECK(SameNameSet(after, {"零字节.txt", "根文件.log"}, &why) ||
                    (Emit("WHY: " + why), false));
      }
    }
  }

  // ---- a container we cannot name is refused, not guessed -----------------
  {
    const std::wstring odd = Join(gScratch->wstr(), L"case_unknown.bin");
    PRISM_CHECK(CopyFileW(fx.plain.c_str(), odd.c_str(), FALSE) != 0);
    const edit::EditResult result = Run(odd, {Op(prism::EditOpKind::Delete, "根文件.log")});
    PRISM_CHECK(!result.ok);
    PRISM_CHECK(result.error.find("format") != std::string::npos);
  }
}

PRISM_TEST("malformed catalog names are refused by the decision the executor uses") {
  // A 7z.dl-authored archive can never hold these names, so the policy is proven
  // on the exact function ApplyEdits calls with the catalog it just read.
  prism::ArchiveItem folder;
  folder.index = 0;
  folder.path = "资料";
  folder.isDir = true;
  const char* malformed[] = {"", "."};
  for (const char* name : malformed) {
    prism::ArchiveItem strange;
    strange.index = 1;
    strange.path = name;
    strange.size = 16;
    const auto plan = prism::BuildEditPlan({folder, strange},
                                           {Op(prism::EditOpKind::Delete, "资料", "", true)},
                                           prism::EditArchiveFacts{});
    PRISM_CHECK(!plan.ok);
    PRISM_CHECK(!plan.error.empty());
    PRISM_CHECK(plan.items.empty());
    Emit("REFUSED malformed '" + std::string(name) + "': " + plan.error);
  }
  // A traversal is a refusal of its own kind.
  prism::ArchiveItem traversal;
  traversal.index = 1;
  traversal.path = "../逃出去.txt";
  const auto plan = prism::BuildEditPlan({folder, traversal},
                                         {Op(prism::EditOpKind::Delete, "资料", "", true)},
                                         prism::EditArchiveFacts{});
  PRISM_CHECK(!plan.ok);
  PRISM_CHECK(plan.refusal == prism::EditRefusal::UnsafePath);
}

int main(int argc, char** argv) {
  if (kMutantBanner != nullptr) {
    std::printf("%s\n", kMutantBanner);
  }
#if defined(PRISM_C51_MUTANT_KEEP_DELETED)
  prism::test::SetMutationVerdict("C51 MUTATION CAUGHT: keep_deleted",
                                  [] { return gInjectedKeepDeleted > 0; });
#elif defined(PRISM_C51_MUTANT_BROKEN_PUBLISH)
  prism::test::SetMutationVerdict("C51 MUTATION CAUGHT: broken_publish",
                                  [] { return gInjectedBrokenPublish > 0; });
#endif

  const std::wstring dll = prism::win::cw::ResolveSevenZipDll();
  if (dll.empty()) {
    std::printf("SKIP: 7z.dll was not found (PRISM_7Z_DLL / exe dir / Program Files)\n");
    return 77;
  }
  if (!gLibrary.Load(dll)) {
    std::printf("SKIP: 7z.dll at %s could not be loaded\n",
                prism::win::WideToUtf8(dll).c_str());
    return 77;
  }
  prism::testtmp::UniqueDir scratch(L"c51", L"archive_edit");
  if (!scratch.ok()) {
    std::printf("FIXTURE: cannot create the scratch directory (%s)\n", scratch.error().c_str());
    return 1;
  }
  gScratch = &scratch;
  const int rc = prism::test::RunAll(argc, argv);
  gScratch = nullptr;
  if (rc != 0) return rc;
  std::printf("ALL ARCHIVE-EDIT CHECKS PASSED\n");
  return 0;
}
