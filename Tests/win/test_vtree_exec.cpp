// Windows integration test for the in-archive edit executor (batch B1): a
// VirtualTree CoW decision plan is driven through the REAL 7z.dll
// IOutArchive::UpdateItems() and the committed archive is re-opened and
// verified byte-for-byte. Missing 7z.exe/7z.dll is a hard FAIL, never a skip:
// the whole point of this lane is the real DLL round trip.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include <wincodec.h>

#include "../mini_test.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "VirtualTreeExecutor.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

namespace fs = std::filesystem;

bool EnsureCom() {
  static const bool ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
  return ok;
}

std::vector<uint8_t> ReadAllBytes(const std::wstring& path) {
  std::ifstream in(path, std::ios::binary);
  const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                              std::istreambuf_iterator<char>());
  return std::vector<uint8_t>(raw.begin(), raw.end());
}

using Payloads = std::map<std::string, std::vector<uint8_t>>;
using TreeShape = std::map<std::string, bool>;  // normalized path -> isDir

// Every file payload in the archive, keyed by normalized path: the basis of
// the CONTENT assertions (CONTRACTS D2: assert bytes, not existence).
Payloads ReadPayloads(prism::win::ArchiveReader& reader) {
  Payloads out;
  for (const auto& item : reader.ReadCatalog()) {
    if (item.isDir) continue;
    auto bytes = reader.ExtractToMemory(item.index, 64ull << 20);
    if (!PRISM_CHECK(bytes.has_value())) return {};
    out[item.path] = std::move(*bytes);
  }
  return out;
}

TreeShape ReadShape(prism::win::ArchiveReader& reader) {
  TreeShape out;
  for (const auto& item : reader.ReadCatalog()) out[item.path] = item.isDir;
  return out;
}

size_t CountTempLitter(const std::wstring& dir) {
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == L".tmp") ++n;
  }
  return n;
}

struct Fixture {
  std::wstring dir;
  std::wstring archive;
  Payloads originals;  // the three source files, byte-exact

  bool Make(const wchar_t* name) {
    dir = (fs::temp_directory_path() / "prism_vtree_exec" / name).wstring();
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    struct Png {
      const wchar_t* file;
      UINT w, h;
      uint8_t b, g, r;
    };
    const Png files[] = {
        {L"icon.png", 64, 64, 0, 0, 255},
        {L"photo.png", 120, 90, 0, 255, 0},
        {L"large.png", 300, 200, 255, 0, 0},
    };
    for (const Png& f : files) {
      if (!prism::fixture::WritePng(dir + L"\\" + f.file, f.w, f.h, f.b, f.g, f.r)) return false;
      originals[prism::win::WideToUtf8(f.file)] = ReadAllBytes(dir + L"\\" + f.file);
    }
    archive = dir + L"\\solid.7z";
    return prism::fixture::PackSolid(prism::fixture::ExePath(), dir, L"solid.7z",
                                     {L"icon.png", L"photo.png", L"large.png"});
  }
};

// Opens an archive and builds the CoW tree + catalog from it. The reader must
// be Close()d by the caller BEFORE CommitTree runs (no FILE_SHARE_DELETE).
bool BuildTree(const std::wstring& archive, prism::win::SevenZipLibrary& library,
               prism::VirtualArchiveTree* tree, std::vector<prism::ArchiveItem>* catalog) {
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(archive))) {
    std::cout << "    open trace: " << reader.openTrace() << std::endl;
    return false;
  }
  *catalog = reader.ReadCatalog();
  tree->Rebuild(*catalog);  // in place: moves would dangle parent pointers
  reader.Close();
  return true;
}

// Re-opens the committed archive and compares shape + payloads against the
// expectation, printing the diff so a red run is diagnosable from the log.
bool VerifyCommitted(const std::wstring& archive, prism::win::SevenZipLibrary& library,
                     const TreeShape& wantShape, const Payloads& wantPayloads) {
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(archive))) {
    std::cout << "    reopen trace: " << reader.openTrace() << std::endl;
    return false;
  }
  bool ok = true;
  const TreeShape gotShape = ReadShape(reader);
  if (gotShape != wantShape) {
    std::cout << "    shape mismatch: got " << gotShape.size() << " entries, want "
              << wantShape.size() << std::endl;
    for (const auto& kv : gotShape) {
      const auto it = wantShape.find(kv.first);
      if (it == wantShape.end()) std::cout << "      unexpected: " << kv.first << std::endl;
    }
    for (const auto& kv : wantShape) {
      if (gotShape.find(kv.first) == gotShape.end())
        std::cout << "      missing: " << kv.first << std::endl;
    }
    ok = false;
  }
  const Payloads gotPayloads = ReadPayloads(reader);
  reader.Close();
  if (gotPayloads != wantPayloads) {
    std::cout << "    payload mismatch: got " << gotPayloads.size() << " files, want "
              << wantPayloads.size() << std::endl;
    for (const auto& kv : wantPayloads) {
      const auto it = gotPayloads.find(kv.first);
      if (it == gotPayloads.end()) {
        std::cout << "      missing payload: " << kv.first << std::endl;
      } else if (it->second != kv.second) {
        std::cout << "      payload differs: " << kv.first << " (" << it->second.size()
                  << " vs " << kv.second.size() << " bytes)" << std::endl;
      }
    }
    for (const auto& kv : gotPayloads) {
      if (wantPayloads.find(kv.first) == wantPayloads.end())
        std::cout << "      unexpected payload: " << kv.first << std::endl;
    }
    ok = false;
  }
  return ok;
}

}  // namespace

// Case 1: the canonical CoW commit -- rename one file inside a solid 7z, the
// decision layer plans ONE metadata-only entry, and every payload survives
// byte-identical (CONTRACTS D2). Honest progress arrives in copied bytes.
PRISM_TEST("rename commits as metadata-only and keeps payloads byte-identical") {
  if (!PRISM_CHECK(EnsureCom())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(prism::fixture::DllPath()))) return;
  Fixture fx;
  if (!PRISM_CHECK(fx.Make(L"case1_rename"))) return;

  prism::VirtualArchiveTree tree;
  std::vector<prism::ArchiveItem> catalog;
  if (!PRISM_CHECK(BuildTree(fx.archive, library, &tree, &catalog))) return;
  PRISM_CHECK_EQ(catalog.size(), size_t(3));
  // Evidence for the rename path: the real 7z.exe catalog is the one input a
  // Linux repro cannot fake. stderr + newline: unbuffered, survives crashes.
  std::fprintf(stderr, "step: tree built\n");
  for (const auto& it : catalog) {
    std::fprintf(stderr, "  catalog: '%s' idx=%u dir=%d\n", it.path.c_str(), it.index,
                 static_cast<int>(it.isDir));
  }
  std::fprintf(stderr, "step: find icon=%d find renamed=%d root children=%d\n",
               tree.Find("icon.png") != nullptr, tree.Find("renamed.png") != nullptr,
               tree.RootChildrenCount());
  std::fprintf(stderr, "step: before rename\n");
  PRISM_CHECK(tree.Rename("icon.png", "renamed.png"));
  std::fprintf(stderr, "step: rename ok\n");
  PRISM_CHECK(tree.dirty());

  // The plan IS the contract the executor implements: one entry, data untouched.
  const auto plan = tree.MakeUpdatePlan();
  PRISM_CHECK_EQ(plan.size(), size_t(1));
  PRISM_CHECK(!plan[0].newData);
  PRISM_CHECK(plan[0].newProps);
  PRISM_CHECK(!plan[0].removed);

  std::vector<int64_t> progress;
  const auto result = prism::win::vt::CommitTree(
      fx.archive, tree, [&](int64_t copied) { progress.push_back(copied); return true; });
  if (!result.ok) std::cout << "    commit error: " << result.error << std::endl;
  PRISM_CHECK(result.ok);
  PRISM_CHECK_EQ(prism::win::WideToUtf8(result.newArchivePath),
                 prism::win::WideToUtf8(fx.archive));  // narrow: ostream cannot print wstring

  TreeShape want;
  want["renamed.png"] = false;
  want["photo.png"] = false;
  want["large.png"] = false;
  Payloads wantPayloads = fx.originals;
  wantPayloads["renamed.png"] = wantPayloads["icon.png"];
  wantPayloads.erase("icon.png");
  if (!PRISM_CHECK(VerifyCommitted(fx.archive, library, want, wantPayloads))) return;

  PRISM_CHECK(!progress.empty());       // progress was fed at all
  PRISM_CHECK(progress.back() > 0);     // in bytes, not fake percents
  PRISM_CHECK_EQ(CountTempLitter(fx.dir), size_t(0));  // no temp left behind
}

// Case 2: create an empty folder and move a file into it -- new node carries
// properties only (indexInArchive = -1), the moved file rides its old data.
PRISM_TEST("mkdir and move rebuild the archive structure") {
  if (!PRISM_CHECK(EnsureCom())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(prism::fixture::DllPath()))) return;
  Fixture fx;
  if (!PRISM_CHECK(fx.Make(L"case2_mkdir_move"))) return;

  prism::VirtualArchiveTree tree;
  std::vector<prism::ArchiveItem> catalog;
  if (!PRISM_CHECK(BuildTree(fx.archive, library, &tree, &catalog))) return;
  PRISM_CHECK(tree.Mkdir("docs"));
  PRISM_CHECK(tree.Move("photo.png", "docs"));

  const auto result = prism::win::vt::CommitTree(fx.archive, tree, nullptr);
  if (!result.ok) std::cout << "    commit error: " << result.error << std::endl;
  PRISM_CHECK(result.ok);
  PRISM_CHECK(result.copiedBytes > 0);

  TreeShape want;
  want["icon.png"] = false;
  want["large.png"] = false;
  want["docs"] = true;            // the brand-new empty folder exists
  want["docs/photo.png"] = false;  // and holds the moved file
  Payloads wantPayloads = fx.originals;
  wantPayloads["docs/photo.png"] = wantPayloads["photo.png"];
  wantPayloads.erase("photo.png");
  if (!PRISM_CHECK(VerifyCommitted(fx.archive, library, want, wantPayloads)) ||
      false) {
    // Evidence dump: committed catalog entry-by-entry + the engine ask-trace.
    prism::win::ArchiveReader dumpReader(library);
    if (dumpReader.Open(fx.archive)) {
      for (const auto& it : dumpReader.ReadCatalog()) {
        std::cout << "  committed: '" << it.path << "' dir=" << it.isDir
                  << " size=" << it.size << std::endl;
      }
      dumpReader.Close();
    } else {
      std::cout << "  reopen for dump failed: " << dumpReader.openTrace()
                << std::endl;
    }
    std::cout << "  update-trace:" << std::endl << result.updateTrace;
    return;  // PRISM_TEST bodies are void
  }
  PRISM_CHECK_EQ(CountTempLitter(fx.dir), size_t(0));
}

// Case 3: failure containment. (a) An archive path whose directory does not
// exist fails loudly BEFORE anything is touched. (b) A read-only target fails
// at the very LAST stage (the replace) -- the temp must be cleaned up and the
// original must remain openable with its tree unchanged (CONTRACTS C5: the
// refusal carries its reason).
PRISM_TEST("failed commit cleans up and leaves the original intact") {
  if (!PRISM_CHECK(EnsureCom())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(prism::fixture::DllPath()))) return;
  Fixture fx;
  if (!PRISM_CHECK(fx.Make(L"case3_failure"))) return;

  // (a) open-stage failure: the output directory does not exist.
  prism::VirtualArchiveTree tree;
  std::vector<prism::ArchiveItem> catalog;
  if (!PRISM_CHECK(BuildTree(fx.archive, library, &tree, &catalog))) return;
  PRISM_CHECK(tree.Rename("icon.png", "renamed.png"));
  const auto ghost = prism::win::vt::CommitTree(
      (fs::path(fx.dir) / L"missing_dir" / L"solid.7z").wstring(), tree, nullptr);
  PRISM_CHECK(!ghost.ok);
  PRISM_CHECK(!ghost.error.empty());
  PRISM_CHECK(ghost.newArchivePath.empty());

  // (b) replace-stage failure: read-only target file.
  const std::wstring locked = (fs::path(fx.dir) / L"locked.7z").wstring();
  if (!PRISM_CHECK(CopyFileW(fx.archive.c_str(), locked.c_str(), FALSE))) return;
  if (!PRISM_CHECK(SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_READONLY))) return;
  prism::VirtualArchiveTree tree2;
  std::vector<prism::ArchiveItem> catalog2;
  if (!PRISM_CHECK(BuildTree(locked, library, &tree2, &catalog2))) return;
  PRISM_CHECK(tree2.Rename("large.png", "moved.png"));
  const auto denied = prism::win::vt::CommitTree(locked, tree2, nullptr);
  PRISM_CHECK(!denied.ok);
  if (!denied.ok) {
    std::cout << "    (b) refused as: " << denied.error << std::endl;
  }
  PRISM_CHECK(!denied.error.empty());
  PRISM_CHECK(denied.newArchivePath.empty());
  PRISM_CHECK(SetFileAttributesW(locked.c_str(), FILE_ATTRIBUTE_NORMAL));

  // The locked archive is byte-for-byte the state before the failed commit,
  // still fully openable, and no .tmp litter survived the cleanup path.
  TreeShape want;
  want["icon.png"] = false;
  want["photo.png"] = false;
  want["large.png"] = false;
  if (!PRISM_CHECK(VerifyCommitted(locked, library, want, fx.originals))) return;
  PRISM_CHECK_EQ(CountTempLitter(fx.dir), size_t(0));
}

// Case 4: the honest cost model (ADR-002): copiedBytes equals the committed
// file size (the update IS a full sequential copy) and the elapsed time is
// real, non-zero wall clock.
PRISM_TEST("cost model reports true copied bytes and wall time") {
  if (!PRISM_CHECK(EnsureCom())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(prism::fixture::DllPath()))) return;
  Fixture fx;
  if (!PRISM_CHECK(fx.Make(L"case4_cost"))) return;

  prism::VirtualArchiveTree tree;
  std::vector<prism::ArchiveItem> catalog;
  if (!PRISM_CHECK(BuildTree(fx.archive, library, &tree, &catalog))) return;
  PRISM_CHECK(tree.Rename("photo.png", "renamed_photo.png"));

  const auto result = prism::win::vt::CommitTree(fx.archive, tree, nullptr);
  if (!result.ok) std::cout << "    commit error: " << result.error << std::endl;
  PRISM_CHECK(result.ok);
  PRISM_CHECK(result.copiedBytes > 0);
  PRISM_CHECK(result.elapsedMs > 0);
  // Full-copy evidence: the sink saw every byte that ended up in the file.
  PRISM_CHECK_EQ(result.copiedBytes,
                 static_cast<int64_t>(fs::file_size(fx.archive)));
  std::cout << "    commit cost: " << result.copiedBytes << " bytes in "
            << result.elapsedMs << " ms" << std::endl;
}

PRISM_TEST("no-op tree does not rewrite the archive") {
  if (!PRISM_CHECK(EnsureCom())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(prism::fixture::DllPath()))) return;
  Fixture fx;
  if (!PRISM_CHECK(fx.Make(L"case5_noop"))) return;

  prism::VirtualArchiveTree tree;
  std::vector<prism::ArchiveItem> catalog;
  if (!PRISM_CHECK(BuildTree(fx.archive, library, &tree, &catalog))) return;
  PRISM_CHECK(!tree.dirty());

  const auto result = prism::win::vt::CommitTree(fx.archive, tree, nullptr);
  if (!result.ok) std::cout << "    commit error: " << result.error << std::endl;
  PRISM_CHECK(result.ok);
  PRISM_CHECK_EQ(result.copiedBytes, int64_t(0));  // nothing copied, nothing touched
  PRISM_CHECK_EQ(CountTempLitter(fx.dir), size_t(0));
  TreeShape want;
  want["icon.png"] = false;
  want["photo.png"] = false;
  want["large.png"] = false;
  if (!PRISM_CHECK(VerifyCommitted(fx.archive, library, want, fx.originals))) return;
}

int main(int argc, char** argv) {
  if (!EnsureCom() || prism::fixture::DllPath().empty() || prism::fixture::ExePath().empty()) {
    std::cout << "    FAIL: 7z.exe not found beside 7z.dll (PRISM_7Z_DLL unset)"
              << std::endl;
    return 1;
  }
  const int rc = prism::test::RunAll(argc, argv);
  // The stdout marker is part of the verdict (CONTRACTS D1): a missing marker
  // can never count as a pass, even if the exit code lies.
  if (rc == 0) std::cout << "    ALL VTREE-EXEC CHECKS PASSED" << std::endl;
  return rc;
}
