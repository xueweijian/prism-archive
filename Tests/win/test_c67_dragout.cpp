// Card C67 -- dragging files OUT of the archive, with a REAL 7z.dll.
//
// What this proves (each line is an assertion below):
//   1. the object a drop target receives ADVERTISES CF_HDROP (EnumFormatEtc),
//      and QueryGetData -- the "hovering" probe Explorer sends on DragEnter --
//      answers S_OK WITHOUT extracting anything and WITHOUT creating the temp
//      directory (not even an empty one);
//   2. a drag that is cancelled (or simply never lands) leaves NOTHING behind:
//      the payload is produced on the FIRST GetData(CF_HDROP), not before;
//   3. GetData(CF_HDROP) hands over one entry per selected catalog item, each
//      an ABSOLUTE path that EXISTS and whose bytes are the archive's bytes
//      (read back through DragQueryFileW -- the same call a real target makes);
//   4. a second GetData does not extract twice (a target asks for sizing, then
//      for the copy), and
//   5. when the drag ends -- drop, cancel or error -- the temp tree is removed
//      (the FileDragOut::Finish invariant the app relies on).
//
// SKIP POLICY (D1): needs a real 7z.dll + 7z.exe; missing => print why, exit 77
// (registered as SKIP_RETURN_CODE). The success marker is a conjunction with
// exit 0, so a silent skip can never look green.
#include <windows.h>
#include <shlobj_core.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/DragOutPlan.hpp"
#include "../../Src/PrismWin/DragOutToShell.hpp"

namespace {

namespace fs = std::filesystem;
using prism::win::dragoutfiles::FileDragOut;
using prism::win::dragoutfiles::HdropFormat;
using prism::win::dragoutfiles::MaterializeResult;

constexpr int kSkipReturnCode = 77;

std::wstring g_dllPath;
std::wstring g_exePath;

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

std::string ReadText(const std::wstring& path) {
  std::ifstream in(fs::path(path), std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}  // namespace

PRISM_TEST("c67.dragout_hover_extracts_nothing_and_a_cancel_leaves_no_temp") {
  prism::testtmp::UniqueDir dir(L"c67", L"hover");
  if (!PRISM_CHECK(dir.ok())) return;
  if (!PRISM_CHECK(WriteText(dir.path() / L"alpha.txt", "alpha-bytes"))) return;
  if (!PRISM_CHECK(WriteText(dir.path() / L"beta.txt", "beta-bytes"))) return;
  if (!PRISM_CHECK(WriteText(dir.path() / L"gamma.txt", "gamma-bytes"))) return;
  if (!PRISM_CHECK(prism::fixture::PackSolid(g_exePath, dir.wstr(), L"pack.7z",
                                             {L"alpha.txt", L"beta.txt", L"gamma.txt"}))) {
    return;
  }
  const std::wstring archive = (dir.path() / L"pack.7z").wstring();

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) return;
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(archive))) return;
  const std::vector<prism::ArchiveItem> catalog = reader.ReadCatalog();
  reader.Close();
  PRISM_CHECK_EQ(catalog.size(), static_cast<std::size_t>(3));

  // FIRST and LAST, deliberately NON-CONTIGUOUS: the engine indexes its
  // callback catalog by archive index, so a two-entry CONTIGUOUS pick would
  // still "work" with the broken (filtered-catalog) shape and hide the bug this
  // card measured. With 0 and 2 the middle file must be skipped, not served in
  // its place.
  std::vector<uint32_t> selection;
  std::vector<uint32_t> files;
  for (const prism::ArchiveItem& item : catalog) {
    if (!item.isDir) files.push_back(item.index);
  }
  if (!PRISM_CHECK_EQ(files.size(), static_cast<std::size_t>(3))) return;
  selection = {files.front(), files.back()};

  prism::testtmp::UniqueDir tempRoot(L"c67", L"temp");
  if (!PRISM_CHECK(tempRoot.ok())) return;

  std::string why;
  auto drag = FileDragOut::Create(tempRoot.wstr(), FileDragOut::Extractor{}, &why);
  if (!PRISM_CHECK(drag != nullptr)) {
    std::cout << "    reason: " << why << std::endl;
    return;
  }
  const std::wstring tempDir = drag->temp_dir();
  drag->SetExtractor([&](MaterializeResult* out) {
    const prism::app::DragOutMaterialization produced = prism::app::MaterializeSelectionForDragOut(
        library, archive, L"", catalog, selection, tempDir);
    out->ok = produced.ok;
    out->error = produced.error;
    out->paths = produced.paths;
    return produced.ok;
  });

  // ---- 1. the hover probes: CF_HDROP is advertised, nothing is extracted ----
  FORMATETC format = HdropFormat();
  PRISM_CHECK_EQ(drag->object()->QueryGetData(&format), S_OK);
  Microsoft::WRL::ComPtr<IEnumFORMATETC> formats;
  PRISM_CHECK(SUCCEEDED(drag->object()->EnumFormatEtc(DATADIR_GET, &formats)));
  FORMATETC enumerated{};
  ULONG fetched = 0;
  PRISM_CHECK_EQ(formats->Next(1, &enumerated, &fetched), S_OK);
  PRISM_CHECK_EQ(fetched, 1UL);
  PRISM_CHECK_EQ(enumerated.cfFormat, static_cast<CLIPFORMAT>(CF_HDROP));
  PRISM_CHECK_EQ(drag->extractions(), static_cast<std::size_t>(0));
  PRISM_CHECK(!drag->temp_dir_exists());  // NOT EVEN the empty directory
  PRISM_CHECK_EQ(drag->files(), static_cast<std::size_t>(0));

  // ---- 2. a drag that never lands: Finish() is what DoDragDrop's return
  //         path calls, and it must leave no trace. ---------------------------
  drag->Finish();
  PRISM_CHECK(!drag->temp_dir_exists());
  PRISM_CHECK_EQ(drag->files(), static_cast<std::size_t>(0));

  // ---- 3. a real drop: one HDROP entry per selected item --------------------
  auto drop = FileDragOut::Create(tempRoot.wstr(), FileDragOut::Extractor{}, &why);
  if (!PRISM_CHECK(drop != nullptr)) return;
  const std::wstring dropDir = drop->temp_dir();
  drop->SetExtractor([&](MaterializeResult* out) {
    const prism::app::DragOutMaterialization produced = prism::app::MaterializeSelectionForDragOut(
        library, archive, L"", catalog, selection, dropDir);
    out->ok = produced.ok;
    out->error = produced.error;
    out->paths = produced.paths;
    return produced.ok;
  });
  PRISM_CHECK_EQ(drop->object()->QueryGetData(&format), S_OK);
  PRISM_CHECK_EQ(drop->extractions(), static_cast<std::size_t>(0));

  STGMEDIUM medium{};
  PRISM_CHECK_EQ(drop->object()->GetData(&format, &medium), S_OK);
  PRISM_CHECK_EQ(medium.tymed, static_cast<DWORD>(TYMED_HGLOBAL));
  HDROP hdrop = static_cast<HDROP>(medium.hGlobal);
  if (!PRISM_CHECK(hdrop != nullptr)) {
    ReleaseStgMedium(&medium);
    return;
  }
  const UINT count = DragQueryFileW(hdrop, 0xFFFFFFFFu, nullptr, 0);
  PRISM_CHECK_EQ(count, static_cast<UINT>(selection.size()));
  std::vector<std::wstring> dropped;
  for (UINT i = 0; i < count; ++i) {
    wchar_t path[MAX_PATH]{};
    const UINT chars = DragQueryFileW(hdrop, i, path, MAX_PATH);
    if (!PRISM_CHECK(chars > 0)) continue;
    dropped.emplace_back(path, chars);
  }
  PRISM_CHECK_EQ(drop->files(), static_cast<std::size_t>(2));
  std::size_t alpha = 0;
  std::size_t gamma = 0;
  for (const std::wstring& path : dropped) {
    PRISM_CHECK(prism::dragout::IsAbsoluteWindowsPath(path));
    std::error_code ec;
    PRISM_CHECK(fs::exists(fs::path(path), ec) && !ec);  // a REAL file the target can copy
    const std::string content = ReadText(path);
    if (content == "alpha-bytes") ++alpha;
    if (content == "gamma-bytes") ++gamma;
    PRISM_CHECK(content != "beta-bytes");  // the UNselected middle file
  }
  PRISM_CHECK_EQ(alpha, static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(gamma, static_cast<std::size_t>(1));
  // The temp directory IS the payload's home while the drag is alive.
  PRISM_CHECK(drop->temp_dir_exists());
  ReleaseStgMedium(&medium);

  // ---- 4. a second GetData (sizing pass, then the copy) must not extract twice
  STGMEDIUM again{};
  PRISM_CHECK_EQ(drop->object()->GetData(&format, &again), S_OK);
  PRISM_CHECK_EQ(DragQueryFileW(static_cast<HDROP>(again.hGlobal), 0xFFFFFFFFu, nullptr, 0),
                 count);  // same payload, read BEFORE the medium is released
  ReleaseStgMedium(&again);
  PRISM_CHECK_EQ(drop->extractions(), static_cast<std::size_t>(1));

  // ---- 5. the drag ends: the temp tree goes away (drop OR cancel) ----------
  drop->Finish();
  PRISM_CHECK(!drop->temp_dir_exists());
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed" << std::endl;
    return 1;  // D1: a broken COM apartment is a failure, never a pass
  }
  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  if (g_dllPath.empty() || g_exePath.empty()) {
    std::cout << "SKIP: " << prism::fixture::ExePathWhy() << std::endl;
    ::CoUninitialize();
    return kSkipReturnCode;
  }
  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL C67 DRAG-OUT CHECKS PASSED" << std::endl;
  return 0;
}
