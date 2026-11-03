// Card C67 -- the three gestures, on a REAL HWND and a REAL 7z.dll.
//
//   A. Alt+double-click EXTRACTS instead of opening. The window's decision is
//      PrismCore's ResolvePointer (it has been since C8); this suite proves the
//      call site exists, that it passes the CATALOG index, that a bare
//      double-click still opens, and that a folder still drills in.
//      Alt is injected through SetAltDownProbe: a headless window station has no
//      keyboard, and a test that pressed a real Alt key would be measuring the
//      desktop, not the product.
//   B. Ctrl+click / Shift+click / Ctrl+A build an ORDERED selection, and the
//      action band's hasSelection follows it; a click on empty space clears it.
//   C. "Extract to..." over that selection extracts EXACTLY the selected
//      entries -- the file count is the assertion (AppWiring's
//      RunSelectionExtraction, the same function Main's action handler calls).
//   D. The extraction those two paths run is the production plan: the
//      --extract-sub plan (BuildExtractionPlan) lands the archive's content in
//      "<parent>\<stem>" and that folder really contains the payload.
//
// NEGATIVE CONTROLS (D3/D26/D27), both compiled from THIS source:
//   * PRISM_C67_MUTANT_IGNORE_ALT   -- the Alt chord is ignored, so Alt+double-
//     click opens instead of extracting (arm alt_opens).
//   * PRISM_C67_MUTANT_EXTRACT_ALL  -- the selection filter is dropped, so
//     "extract to..." writes the whole archive (arm extract_all).
// Each mutant prints "C67 MUTATION CAUGHT: <arm>" AT its first failure and
// exits 0; the CTest PASS_REGULAR_EXPRESSION is that marker (never WILL_FAIL on
// top of a regex).
//
// SKIP POLICY (D1): needs a real 7z.dll + 7z.exe; missing => exit 77.
#include <windows.h>

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
#include "../../Src/PrismCore/SelectionModel.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

namespace {

namespace fs = std::filesystem;
using prism::win::gallery::GalleryWindow;

constexpr int kSkipReturnCode = 77;
std::wstring g_dllPath;
std::wstring g_exePath;

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  return static_cast<bool>(out);
}

std::string ReadText(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

std::size_t CountFiles(const fs::path& root) {
  std::error_code ec;
  std::size_t count = 0;
  for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
    if (it->is_regular_file(ec)) ++count;
  }
  return count;
}

// Row -> the pixel its item is painted at, computed from the LAYOUT definition
// (same helper shape as win.tree_nav; asserting against the window's own
// hit test below keeps "what I clicked" and "what it resolved" separate).
bool RowCentre(GalleryWindow& window, int row, int* px, int* py) {
  const prism::layout::Layout layout = window.LayoutForTest();
  const auto& items = window.Items();
  if (row < 0 || row >= static_cast<int>(items.size())) return false;
  int bandTop = layout.contentInner.top;
  int column = 0;
  for (int i = 0; i <= row; ++i) {
    if (items[static_cast<std::size_t>(i)].isDir) {
      if (i == row) {
        *px = layout.contentInner.left + column * (layout.tileWidth + layout.gap) +
              layout.tileWidth / 2;
        *py = bandTop + layout.cellHeight / 2;
        return true;
      }
      if (++column == layout.columns) {
        bandTop += layout.cellHeight;
        column = 0;
      }
    } else {
      if (column > 0) {
        bandTop += layout.cellHeight;
        column = 0;
      }
      if (i == row) {
        *px = (layout.content.left + layout.content.right) / 2;
        *py = bandTop + 16;
        return true;
      }
      bandTop += 32;
    }
  }
  return false;
}

int RowOfPath(const GalleryWindow& window, const std::string& path) {
  const auto& rows = window.Items();
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

bool ClickRow(GalleryWindow& window, int row, WPARAM keys) {
  int x = 0, y = 0;
  if (!RowCentre(window, row, &x, &y)) return false;
  if (window.HitTestForTest(x, y) != row) return false;
  window.Dispatch(WM_LBUTTONDOWN, keys, MAKELPARAM(x, y));
  window.Dispatch(WM_LBUTTONUP, 0, MAKELPARAM(x, y));
  return true;
}

// One archive fixture: three files with distinguishable bytes.
struct Fixture {
  prism::testtmp::UniqueDir dir{L"c67", L"archive"};
  std::wstring archive;
  std::vector<prism::ArchiveItem> catalog;
};

bool BuildFixture(Fixture& fixture) {
  if (!fixture.dir.ok()) return false;
  if (!WriteText(fixture.dir.path() / L"alpha.txt", "alpha-bytes")) return false;
  if (!WriteText(fixture.dir.path() / L"beta.txt", "beta-bytes")) return false;
  if (!WriteText(fixture.dir.path() / L"gamma.txt", "gamma-bytes")) return false;
  if (!prism::fixture::PackSolid(g_exePath, fixture.dir.wstr(), L"pack.7z",
                                 {L"alpha.txt", L"beta.txt", L"gamma.txt"})) {
    return false;
  }
  fixture.archive = (fixture.dir.path() / L"pack.7z").wstring();
  prism::win::SevenZipLibrary library;
  if (!library.Load(g_dllPath)) return false;
  prism::win::ArchiveReader reader(library);
  if (!reader.Open(fixture.archive)) return false;
  fixture.catalog = reader.ReadCatalog();
  reader.Close();
  return true;
}

// The two files this suite works with, in catalog order.
std::vector<uint32_t> FirstTwoFiles(const std::vector<prism::ArchiveItem>& catalog) {
  std::vector<uint32_t> out;
  for (const prism::ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    out.push_back(item.index);
    if (out.size() == 2) break;
  }
  return out;
}

}  // namespace

PRISM_TEST("c67.alt_double_click_extracts_a_plain_one_opens") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  std::vector<prism::ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c67 gestures", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 720));
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return;

  bool alt = true;
  window.SetAltDownProbe([&alt]() { return alt; });
  std::vector<int> smart;
  std::vector<int> opened;
  window.SetSmartExtractHandler([&smart](int index) { smart.push_back(index); });
  window.SetActivateHandler([&opened](int index) { opened.push_back(index); });

  const int row = RowOfPath(window, "beta.txt");
  if (!PRISM_CHECK(row >= 0)) return;
  int x = 0, y = 0;
  if (!PRISM_CHECK(RowCentre(window, row, &x, &y))) return;
  if (!PRISM_CHECK_EQ(window.HitTestForTest(x, y), row)) return;
  window.Dispatch(WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
  // A. the Alt chord reaches the extract handler with the CATALOG index ...
  PRISM_CHECK_EQ(smart.size(), static_cast<std::size_t>(1));
  if (!smart.empty()) PRISM_CHECK_EQ(smart[0], static_cast<int>(items[static_cast<std::size_t>(row)].index));
  // ... and never the ordinary open.
  PRISM_CHECK_EQ(opened.size(), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(window.Selection(), row);

  // A bare double-click keeps the old behaviour: activate, not extract.
  alt = false;
  window.Dispatch(WM_LBUTTONDBLCLK, 0, MAKELPARAM(x, y));
  PRISM_CHECK_EQ(opened.size(), static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(smart.size(), static_cast<std::size_t>(1));
}

PRISM_TEST("c67.ctrl_shift_ctrl_a_build_an_ordered_selection_and_the_band_follows") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  std::vector<prism::ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c67 selection", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 720));
  if (!PRISM_CHECK_EQ(window.ItemCount(), 3)) return;

  // plain click: one row, and the band says "something is selected"
  if (!PRISM_CHECK(ClickRow(window, 0, MK_LBUTTON))) return;
  PRISM_CHECK_EQ(window.SelectionCount(), 1);
  PRISM_CHECK(window.ActionContext().hasSelection);

  // Ctrl+click: a second row is ADDED, the set stays ascending
  if (!PRISM_CHECK(ClickRow(window, 2, MK_LBUTTON | MK_CONTROL))) return;
  PRISM_CHECK_EQ(window.SelectionCount(), 2);
  {
    const std::vector<int> expected{0, 2};
    PRISM_CHECK(window.SelectedRows() == expected);
    PRISM_CHECK_EQ(window.SelectedCatalogIndices().size(), static_cast<std::size_t>(2));
  }

  // Shift+click: the range from the ANCHOR (row 2) to row 1
  if (!PRISM_CHECK(ClickRow(window, 1, MK_LBUTTON | MK_SHIFT))) return;
  {
    const std::vector<int> expected{1, 2};
    PRISM_CHECK(window.SelectedRows() == expected);
  }

  // Ctrl+A: every row of the folder
  prism::keys::Stroke selectAll;
  selectAll.key = 'A';
  selectAll.ctrl = true;
  window.DispatchKeyStroke(selectAll);
  PRISM_CHECK_EQ(window.SelectionCount(), 3);
  PRISM_CHECK(window.ActionContext().hasSelection);

  // A click on empty space clears it, and the band's hasSelection follows.
  const int emptyX = window.ClientWidthForTest() / 2;
  const int emptyY = window.ClientHeightForTest() - 4;
  if (window.HitTestForTest(emptyX, emptyY) >= 0) return;  // the point must BE empty
  window.Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(emptyX, emptyY));
  PRISM_CHECK_EQ(window.SelectionCount(), 0);
  PRISM_CHECK(!window.ActionContext().hasSelection);
}

PRISM_TEST("c67.extract_to_extracts_only_the_selected_entries") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  std::vector<prism::ArchiveItem> items = fixture.catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c67 extract-selection", GetModuleHandleW(nullptr)))) return;
  window.SetItems(&items);
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1280, 720));

  // Select alpha.txt and gamma.txt (rows 0 and 2 in this fixture).
  if (!PRISM_CHECK(ClickRow(window, 0, MK_LBUTTON))) return;
  if (!PRISM_CHECK(ClickRow(window, 2, MK_LBUTTON | MK_CONTROL))) return;
  const std::vector<uint32_t> selection = window.SelectedCatalogIndices();
  if (!PRISM_CHECK_EQ(selection.size(), static_cast<std::size_t>(2))) return;

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) return;
  prism::app::OperationHandles::Config config;
  config.archivePath = fixture.archive;
  config.handlerSlots = 2;
  std::string why;
  auto handles = prism::app::OperationHandles::Create(library, config, &why);
  if (!PRISM_CHECK(handles != nullptr)) return;

  prism::testtmp::UniqueDir target(L"c67", L"selected");
  if (!PRISM_CHECK(target.ok())) return;
  const prism::app::ExtractionOutcome outcome = prism::app::RunSelectionExtraction(
      *handles, items, selection, prism::win::WideToUtf8(target.wstr()), fixture.archive, false);
  handles->Close();
  if (!outcome.ok) {
    std::fprintf(stderr, "selection extract failed: files=%llu error=%s status=%s\n",
                 static_cast<unsigned long long>(outcome.filesWritten), outcome.error.c_str(),
                 prism::win::WideToUtf8(outcome.status).c_str());
  }
  // THE contract of this card: the number of files written is the number the
  // user selected, not the archive's count.
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(2));
  PRISM_CHECK_EQ(CountFiles(target.path()), static_cast<std::size_t>(2));
  PRISM_CHECK(fs::exists(target.path() / L"alpha.txt"));
  PRISM_CHECK(fs::exists(target.path() / L"gamma.txt"));
  PRISM_CHECK(!fs::exists(target.path() / L"beta.txt"));
  PRISM_CHECK_EQ(ReadText(target.path() / L"alpha.txt"), std::string("alpha-bytes"));
  PRISM_CHECK_EQ(ReadText(target.path() / L"gamma.txt"), std::string("gamma-bytes"));
}

PRISM_TEST("c67.extract_sub_plan_lands_the_payload_in_the_named_subfolder") {
  Fixture fixture;
  if (!PRISM_CHECK(BuildFixture(fixture))) return;
  // This is exactly what Main's Alt+double-click handler runs: C25's plan for
  // the --extract-sub verb, over the app's own catalog.
  const prism::app::ExtractionPlan plan = prism::app::BuildExtractionPlan(
      prism::app::LaunchMode::kExtractSub, prism::win::WideToUtf8(fixture.archive),
      fixture.catalog, [](const std::string& path) {
        return fs::exists(prism::win::Utf8ToWide(path));
      });
  const fs::path expected = fixture.dir.path() / L"pack";  // <parent>\<archive stem>
  // PrismCore spells separators '/'; Win32 spells '\'. The PLAN is core data,
  // so compare after normalising, never by re-spelling the production value.
  std::wstring spelled = prism::win::Utf8ToWide(plan.plan.baseTargetDir);
  for (wchar_t& ch : spelled) {
    if (ch == L'/') ch = L'\\';
  }
  if (spelled != expected.wstring()) {
    std::fprintf(stderr, "plan target=%s expected=%s\n", prism::win::WideToUtf8(spelled).c_str(),
                 prism::win::WideToUtf8(expected.wstring()).c_str());
  }
  PRISM_CHECK(spelled == expected.wstring());
  PRISM_CHECK(!fs::exists(expected));  // nothing has been written yet

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) return;
  prism::app::OperationHandles::Config config;
  config.archivePath = fixture.archive;
  config.handlerSlots = 2;
  std::string why;
  auto handles = prism::app::OperationHandles::Create(library, config, &why);
  if (!PRISM_CHECK(handles != nullptr)) return;
  const prism::app::ExtractionOutcome outcome =
      prism::app::RunExtraction(*handles, fixture.catalog, plan, fixture.archive, false);
  handles->Close();

  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(outcome.filesWritten, static_cast<uint64_t>(3));
  PRISM_CHECK(fs::exists(expected));
  PRISM_CHECK_EQ(CountFiles(expected), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(ReadText(expected / L"beta.txt"), std::string("beta-bytes"));
}

PRISM_TEST("c67.action_band_paints_exactly_the_buttons_its_geometry_has") {
  // C66's rule, measured instead of eyeballed: the band either holds ALL eight
  // buttons or it is not drawn at all. A 7-of-8 screenshot is the state that
  // rule exists to forbid, and this case is the pixel/geometry pair that tells
  // which half was lying.
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c67 action band", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items;
  window.SetItems(&items);
  // A 1536x816 FRAME is ~1504x777 of CLIENT area (borders + title): the size the
  // report came from.
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(1504, 777));

  const GalleryWindow::ActionPaintCounters before = window.PaintedActionCounters();
  const prism::actionbar::Layout band = window.ActionBarLayout();
  window.Dispatch(WM_PAINT, 0, 0);
  const GalleryWindow::ActionPaintCounters after = window.PaintedActionCounters();
  const int paintedButtons = after.buttons - before.buttons;
  const int paintedIcons = after.icons - before.icons;
  const int paintedLabels = after.labels - before.labels;
  const int paintedIslands = after.islands - before.islands;
  std::cout << "BAND width=" << window.ClientWidthForTest()
            << " buttons=" << band.buttons.size() << " dropped=" << band.dropped
            << " painted=" << paintedButtons << " icons=" << paintedIcons
            << " labels=" << paintedLabels << " islands=" << paintedIslands
            << " clipped=" << (after.clippedLabels - before.clippedLabels) << std::endl;

  // 1. the model's own invariant: eight, or none (never seven).
  PRISM_CHECK_EQ(band.buttons.size() + static_cast<std::size_t>(band.dropped),
                 static_cast<std::size_t>(prism::actionbar::kActionCount));
  PRISM_CHECK(band.dropped == 0 || band.buttons.empty());
  // 2. at the reported window size the band MUST be complete.
  PRISM_CHECK_EQ(band.buttons.size(), static_cast<std::size_t>(8));
  PRISM_CHECK_EQ(band.dropped, 0);
  // 3. the last button (Search) is inside the client area and inside the island.
  if (band.buttons.size() == 8) {
    const prism::actionbar::Button& last = band.buttons.back();
    PRISM_CHECK_EQ(last.id, prism::actionbar::ActionId::Search);
    PRISM_CHECK(last.rect.right <= window.ClientWidthForTest());
    PRISM_CHECK(last.rect.right <= band.island.right);
    PRISM_CHECK(band.island.right <= window.ClientWidthForTest());
    std::cout << "BAND LAST id=" << prism::actionbar::ActionName(last.id)
              << " rect=" << last.rect.left << ".." << last.rect.right << " island="
              << band.island.left << ".." << band.island.right
              << " client=" << window.ClientWidthForTest() << std::endl;
    // 4. and a click at its centre is resolved to THAT button: geometry that is
    //    painted but not hittable is a button the user cannot press.
    const int centreX = (last.rect.left + last.rect.right) / 2;
    const int centreY = (last.rect.top + last.rect.bottom) / 2;
    PRISM_CHECK_EQ(window.ActionBarButtonAt(centreX, centreY), 7);
  }
  // 5. the pixels agree with the geometry: one island, one icon and one label
  //    per button -- the counter the screenshot's "7" is measured against.
  PRISM_CHECK_EQ(paintedIslands, 1);
  PRISM_CHECK_EQ(paintedButtons, static_cast<int>(band.buttons.size()));
  PRISM_CHECK_EQ(paintedIcons, static_cast<int>(band.buttons.size()));
  PRISM_CHECK_EQ(paintedLabels, static_cast<int>(band.buttons.size()));

  // 6. the other half of the rule: a band too narrow for eight usable buttons
  //    draws NO island and NO button (not a squeezed seven).
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(520, 600));
  const prism::actionbar::Layout narrow = window.ActionBarLayout();
  const GalleryWindow::ActionPaintCounters narrowBefore = window.PaintedActionCounters();
  window.Dispatch(WM_PAINT, 0, 0);
  window.Dispatch(WM_PAINT, 0, 0);  // a second frame must add NOTHING
  const GalleryWindow::ActionPaintCounters narrowAfter = window.PaintedActionCounters();
  std::cout << "BAND NARROW width=" << window.ClientWidthForTest()
            << " buttons=" << narrow.buttons.size() << " dropped=" << narrow.dropped
            << " painted=" << (narrowAfter.buttons - narrowBefore.buttons) << std::endl;
  PRISM_CHECK(narrow.buttons.empty());
  PRISM_CHECK_EQ(narrow.dropped, prism::actionbar::kActionCount);
  PRISM_CHECK(narrow.island.Empty());
  PRISM_CHECK_EQ(narrowAfter.buttons - narrowBefore.buttons, 0);
  PRISM_CHECK_EQ(narrowAfter.islands - narrowBefore.islands, 0);
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed" << std::endl;
    return 1;
  }
  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  if (g_dllPath.empty() || g_exePath.empty()) {
    std::cout << "SKIP: " << prism::fixture::ExePathWhy() << std::endl;
    ::CoUninitialize();
    return kSkipReturnCode;
  }
#if defined(PRISM_C67_MUTANT_IGNORE_ALT)
  std::cout << "C67 MUTANT ACTIVE: the Alt chord is ignored on double-click" << std::endl;
  prism::test::SetMutationVerdict("C67 MUTATION CAUGHT: alt_opens",
                                  [] { return !GalleryWindow::AltChordEnabled(); });
#endif
#if defined(PRISM_C67_MUTANT_SKIP_LAST_PAINT)
  std::cout << "C67 MUTANT ACTIVE: the painter skips the last action button" << std::endl;
  prism::test::SetMutationVerdict("C67 MUTATION CAUGHT: skip_last_paint", [] {
    return prism::win::actionbar::ActionBarPainter::PaintSkipsLastButton();
  });
#endif
#if defined(PRISM_C67_MUTANT_EXTRACT_ALL)
  // MUTANT ACTIVE: "extract to..." hands the WHOLE archive to the engine.
  std::cout << "C67 MUTANT ACTIVE: the selection filter is dropped" << std::endl;
  prism::test::SetMutationVerdict("C67 MUTATION CAUGHT: extract_all", [] {
    return !prism::app::kSelectionFilterArmed;
  });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();
  if (rc != 0) return rc;
  std::cout << "ALL C67 GESTURE CHECKS PASSED" << std::endl;
  return 0;
}
