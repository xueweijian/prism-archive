// PrismArchive -- Windows integration test, card C53 (the wiring of three
// delivered patches into the executable).
//
// The three cards below shipped their own suites; what was never proven is that
// the APP uses them:
//   1. C52's drop executor -- a real HWND, a hand-built DROPFILES blob, the
//      Model's plan, and THIS app's production handler (AppWiring's
//      InstallDropHandler). An archive dropped on an empty window must really
//      reach the archive-changed flow, and an "add" drop must refuse in the
//      user's words without touching the archive (no half-write);
//   2. C31's palette overlay -- Ctrl+K opens it, Ctrl+K closes it (the second
//      press must not fall through and reopen it), and the draw-command list is
//      non-empty;
//   3. C31/V3's settings dialog -- the menu bar carries the item, the real
//      dialog exists, and Save really changes the settings file;
//   4. the C53 rule: at most ONE modal overlay (drop hint / palette / Quick
//      Look / settings) at a time.
//
// NEGATIVE CONTROLS (D3/D26/D27): the same source is compiled twice more with
// one rule compiled out:
//   * PRISM_C53_MUTANT_FAKE_ADD          -> AddToArchive claims success;
//   * PRISM_C53_MUTANT_MULTI_OVERLAY     -> the exclusivity rule is gone.
// Each mutant prints a banner and arms the CAUGHT marker BEFORE the first case;
// mini_test prints it at the first failure and leaves with 0 (D27). No WILL_FAIL
// is stacked on a pass marker anywhere (D26), and the arm names avoid
// clean/control/unarmed (D28).
//
// D1: a session without a window station prints WHY and returns 77, which the
// suite maps to SKIP_RETURN_CODE; the stdout marker stays required, so a silent
// skip can never count as a pass.
#include <windows.h>
// DROPFILES (the header of the HGLOBAL an HDROP points at) lives in
// shlobj_core.h, not in shellapi.h -- see C52's suite for the same note.
#include <shlobj_core.h>

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/DropPlan.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/SettingsDialog.hpp"

namespace fs = std::filesystem;
using prism::drop::DropAction;
using prism::drop::DropPlan;
using prism::win::gallery::ActiveOverlay;
using prism::win::gallery::GalleryWindow;

namespace {

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (chars <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
  return out;
}

bool Contains(const std::wstring& text, const std::wstring& needle) {
  return text.find(needle) != std::wstring::npos;
}

// A REAL HDROP, byte-for-byte the shape the shell allocates (C52's suite builds
// the same blob; the point here is that the APP's handler sees a real handle).
HDROP MakeDrop(const std::vector<std::wstring>& paths, POINT point) {
  std::size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
  for (const std::wstring& path : paths) bytes += (path.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GHND, bytes);
  if (memory == nullptr) return nullptr;
  auto* header = static_cast<DROPFILES*>(::GlobalLock(memory));
  if (header == nullptr) {
    ::GlobalFree(memory);
    return nullptr;
  }
  std::memset(header, 0, bytes);
  header->pFiles = static_cast<DWORD>(sizeof(DROPFILES));
  header->pt = point;
  header->fNC = FALSE;
  header->fWide = TRUE;
  auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(header) + sizeof(DROPFILES));
  for (const std::wstring& path : paths) {
    ::memcpy(cursor, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    cursor += path.size() + 1;
  }
  *cursor = L'\0';
  ::GlobalUnlock(memory);
  return static_cast<HDROP>(memory);
}

std::string ReadBytes(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

std::vector<prism::ArchiveItem> BuildCatalog() {
  std::vector<prism::ArchiveItem> items;
  prism::ArchiveItem item;
  item.index = 0;
  item.path = "docs/readme.txt";
  item.isDir = false;
  item.size = 12;
  items.push_back(item);
  return items;
}

// A key stroke as WM_KEYDOWN would describe it (the SAME router serves both).
prism::keys::Stroke Stroke(uint32_t key, bool ctrl) {
  prism::keys::Stroke stroke;
  stroke.key = key;
  stroke.ctrl = ctrl;
  return stroke;
}

// Pump every QUEUED message the drop handler posted (PostArchiveChanged /
// PostStatus). PeekMessage never blocks and the loop is bounded (D29), so a
// missing message fails the assertions instead of hanging the suite.
int PumpPostedMessages(int budget = 64) {
  int dispatched = 0;
  MSG msg{};
  while (dispatched < budget && ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE) != FALSE) {
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
    ++dispatched;
  }
  return dispatched;
}

// D3 predicates: did the compiled-in defect really fire? The suite only prints
// CAUGHT when one of these says yes, so a red for an unrelated reason stays red.
[[maybe_unused]] bool FakeAddInjectionFired() {
  DropPlan plan;
  plan.primary = DropAction::AddToArchive;
  plan.fileCount = 2;
  plan.headline = "x";
  plan.detail = "y";
  const prism::app::dropin::DropDecision decision = prism::app::dropin::DecideDrop(plan);
  return decision.effect != prism::app::dropin::DropEffect::UnsupportedAdd;
}

[[maybe_unused]] bool MultiOverlayInjectionFired() {
  return !prism::win::gallery::kOverlayExclusivityEnforced;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. C52 + C53: an archive dropped into the EMPTY window opens it
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.drop_archive_into_an_empty_window_opens_it") {
  prism::testtmp::UniqueDir dir(L"c53", L"open");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path archive = dir.path() / L"photos.7z";
  WriteBytes(archive, "7z-signature");

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c53 open", GetModuleHandleW(nullptr)))) return;
  PRISM_CHECK(!window.ArchiveOpen());  // no catalog attached: the empty window
  int changed = 0;
  std::wstring opened;
  window.SetArchiveChangedHandler([&changed, &opened](const std::wstring& path) {
    ++changed;
    opened = path;
  });
  // THE production executor Main.cpp installs, not a copy of it.
  prism::app::dropin::InstallDropHandler(window);

  HDROP drop = MakeDrop({archive.wstring()}, POINT{320, 240});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);  // consumes the handle (DragFinish)
  PRISM_CHECK_EQ(window.LastDropPlan().primary, DropAction::OpenArchive);
  PRISM_CHECK(window.DropHintVisible());
  PRISM_CHECK_EQ(window.CurrentOverlay(), ActiveOverlay::DropHint);
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);
  PRISM_CHECK(window.StatusText().empty());  // no refusal text for an open

  PumpPostedMessages();
  PRISM_CHECK_EQ(changed, 1);  // PostArchiveChanged really reached the app seam
  PRISM_CHECK(opened == archive.wstring());  // the SAME path, not any path
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);
}

// ---------------------------------------------------------------------------
// 2. C31's overlay: Ctrl+K opens it and the SAME chord closes it
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.ctrl_k_opens_and_closes_the_palette_overlay") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c53 palette", GetModuleHandleW(nullptr)))) return;
  const prism::keys::Stroke ctrlK = Stroke(static_cast<uint32_t>('K'), true);
  window.DispatchKeyStroke(ctrlK);
  PRISM_CHECK(window.PaletteOverlayOpen());  // the OVERLAY, not just the shell
  PRISM_CHECK(window.PaletteOpen());
  PRISM_CHECK_EQ(window.CurrentOverlay(), ActiveOverlay::Palette);
  // The overlay is "visible" when it has draw commands to paint: geometry from
  // the shell (D7), not a second layout.
  const std::vector<prism::win::palette::DrawCommand> commands =
      window.Overlay().Build(900, 700);
  PRISM_CHECK(!commands.empty());

  // A second Ctrl+K must CLOSE. The bug this test exists for: the overlay closes
  // first, then the key falls through to the gallery table and reopens it.
  window.DispatchKeyStroke(ctrlK);
  PRISM_CHECK(!window.PaletteOverlayOpen());
  PRISM_CHECK(!window.PaletteOpen());
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 0);
  PRISM_CHECK_EQ(window.OverlayInvariantViolations(), 0);
}

// ---------------------------------------------------------------------------
// 3. C31/V3: the menu item opens a REAL dialog, and Save moves the file
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.settings_menu_opens_a_real_dialog_and_saves_the_file") {
  prism::testtmp::UniqueDir dir(L"c53", L"settings");
  if (!PRISM_CHECK(dir.ok())) return;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c53 settings", GetModuleHandleW(nullptr)))) return;

  // The production menu bar (C27's File menu) + this card's appended item; the
  // item is located BY ITS ID, exactly as WM_COMMAND will deliver it.
  window.SetMenuLabels(L"&File", L"&Compress...", L"E&xit");
  HMENU menu = ::GetMenu(window.Hwnd());
  if (!PRISM_CHECK(menu != nullptr)) return;
  const UINT settingsId =
      static_cast<UINT>(prism::win::settingsui::SettingsDialog::kMenuSettingsId);
  bool found = false;
  const int itemCount = ::GetMenuItemCount(menu);
  for (int index = 0; index < itemCount; ++index) {
    if (::GetMenuItemID(menu, static_cast<UINT>(index)) == settingsId) found = true;
  }
  PRISM_CHECK(found);  // the menu bar really carries Settings...

  const prism::win::settingsfile::Paths paths =
      prism::win::settingsfile::PathsUnder(dir.path().wstring());
  bool ran = false;
  bool dialogExisted = false;
  bool saved = false;
  bool fileChanged = false;
  std::string saveError;
  window.SetSettingsLauncher([&] {
    ran = true;
    // The gate must already report Settings as THE active overlay.
    PRISM_CHECK(window.SettingsModalOpen());
    PRISM_CHECK_EQ(window.CurrentOverlay(), ActiveOverlay::Settings);
    PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);
    prism::win::settingsui::SettingsDialog dialog;
    dialog.SetPaths(paths);
    std::string loadError;
    dialog.LoadFrom(paths, &loadError);  // writes defaults if the file is absent
    dialogExisted = dialog.Create(GetModuleHandleW(nullptr), L"c53 settings dialog");
    PRISM_CHECK(dialogExisted);
    const std::string before = ReadBytes(paths.file);
    // A user change the schema accepts: the theme is a plain enum.
    dialog.State().SetTheme(prism::win::settingsui::Theme::Midnight);
    saved = dialog.Save(&saveError);
    const std::string after = ReadBytes(paths.file);
    fileChanged = !before.empty() && before != after;
    if (!saved) std::printf("    save error: %s\n", saveError.c_str());
    dialog.Destroy();
  });
  window.Dispatch(WM_COMMAND, MAKEWPARAM(prism::win::settingsui::SettingsDialog::kMenuSettingsId, 0),
                  0);
  PRISM_CHECK(ran);
  PRISM_CHECK(dialogExisted);
  PRISM_CHECK(saved);
  PRISM_CHECK(fileChanged);  // D2: the FILE changed, not the draft
  PRISM_CHECK(!window.SettingsModalOpen());  // the gate is closed again
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 0);
  PRISM_CHECK_EQ(window.OverlayInvariantViolations(), 0);
}

// ---------------------------------------------------------------------------
// 4. C53: a drop and the palette never share the surface
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.only_one_modal_overlay_is_active_at_a_time") {
  prism::testtmp::UniqueDir dir(L"c53", L"overlay");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path notes = dir.path() / L"notes.txt";
  WriteBytes(notes, "not an archive");

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c53 overlay", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);  // an open archive: the drop reads as AddToArchive
  prism::app::dropin::InstallDropHandler(window);

  window.DispatchKeyStroke(Stroke(static_cast<uint32_t>('K'), true));
  PRISM_CHECK(window.PaletteOverlayOpen());
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);

  HDROP drop = MakeDrop({notes.wstring()}, POINT{120, 90});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);  // the drop takes the surface from the palette

  PRISM_CHECK(!window.PaletteOverlayOpen());  // exactly one survives...
  PRISM_CHECK(window.DropHintVisible());
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);  // ...and it is the hint
  PRISM_CHECK_EQ(window.CurrentOverlay(), ActiveOverlay::DropHint);
  PRISM_CHECK_EQ(window.OverlayInvariantViolations(), 0);

  // Opening the palette again dismisses the hint in the other direction.
  window.DispatchKeyStroke(Stroke(static_cast<uint32_t>('K'), true));
  PRISM_CHECK(window.PaletteOverlayOpen());
  PRISM_CHECK(!window.DropHintVisible());
  PRISM_CHECK_EQ(window.ActiveOverlayCount(), 1);
  PRISM_CHECK_EQ(window.OverlayInvariantViolations(), 0);
}

// ---------------------------------------------------------------------------
// 5. C53: AddToArchive refuses EXPLICITLY and writes nothing (no half-write)
// ---------------------------------------------------------------------------
PRISM_TEST("wiring.add_to_archive_refuses_explicitly_and_leaves_the_archive_alone") {
  prism::testtmp::UniqueDir dir(L"c53", L"append");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path archive = dir.path() / L"photos.7z";
  const std::string original = "7z-signature: this file must not move";
  WriteBytes(archive, original);
  const fs::path notes = dir.path() / L"notes.txt";
  WriteBytes(notes, "new content");

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"c53 append", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);
  prism::app::dropin::InstallDropHandler(window);

  HDROP drop = MakeDrop({notes.wstring()}, POINT{20, 20});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);
  PRISM_CHECK_EQ(window.LastDropPlan().primary, DropAction::AddToArchive);
  PumpPostedMessages();

  // The user is TOLD, in the app's words, that append is not implemented yet...
  PRISM_CHECK(Contains(window.StatusText(), ToWide("\u6682\u4E0D\u652F\u6301\u8FFD\u52A0")));
  PRISM_CHECK(Contains(window.StatusText(), ToWide("AddToArchive")));
  // ...and the claim is never made: the mutant build's "已添加 N 项" fails here.
  PRISM_CHECK(!Contains(window.StatusText(), ToWide("\u5DF2\u6DFB\u52A0")));
  // The archive on disk is byte-for-byte what it was (D2/D18: not "still
  // exists", the SAME BYTES).
  PRISM_CHECK_EQ(ReadBytes(archive), original);
  PRISM_CHECK_EQ(window.CurrentOverlay(), ActiveOverlay::DropHint);
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "    SKIP: CoInitializeEx failed (no window station?)" << std::endl;
    return 77;
  }
#if defined(PRISM_C53_MUTANT_FAKE_ADD)
  std::cout << "C53 MUTANT ACTIVE: AddToArchive is reported as done" << std::endl;
  prism::test::SetMutationVerdict("C53 MUTATION CAUGHT: fake_add",
                                  [] { return FakeAddInjectionFired(); });
#elif defined(PRISM_C53_MUTANT_MULTI_OVERLAY)
  std::cout << "C53 MUTANT ACTIVE: two modal overlays may be active" << std::endl;
  prism::test::SetMutationVerdict("C53 MUTATION CAUGHT: multi_overlay",
                                  [] { return MultiOverlayInjectionFired(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL C53-WIRING CHECKS PASSED" << std::endl;
  return 0;
}
