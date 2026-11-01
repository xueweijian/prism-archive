// PrismArchive -- PrismWin (card C52)
// DROPPING FILES INTO THE WINDOW, on a real HWND.
//
// The model suite (Tests/test_drop_plan.cpp) proves the RULES as numbers. This
// suite proves the three things only Windows can:
//
//   1. the window is really a drop target (DragAcceptFiles sets
//      WS_EX_ACCEPTFILES -- without it Explorer never sends WM_DROPFILES);
//   2. a REAL HDROP (a HAND-BUILT DROPFILES blob, exactly the shape the shell
//      allocates, read back through the same DragQueryFileW the product uses)
//      becomes the right tree: a dropped folder is walked on disk, its files
//      keep their full paths, and the plan handed to the app is right;
//   3. the hint really exists and really paints: the rectangle lies inside the
//      window's own content rect (V1 layout), WM_PAINT runs, and the overlay
//      draws -- measured by counters the paint path increments, not by hope.
//      A refusal carries its reason in the text the overlay shows.
//
// NEGATIVE CONTROL (D3/D27): the same source compiled with
// PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION makes the model hand a folder over as
// ONE member, so the folder case here goes red and the binary prints
// "C52 MUTATION CAUGHT: folder_flat" at the first failure. The marker IS the
// CTest verdict (no WILL_FAIL stacked on a regex, D26).
#include <windows.h>
// DROPFILES (the header of the HGLOBAL an HDROP points at) is declared by
// shlobj_core.h in this SDK's headers, not by shellapi.h -- including the
// latter alone compiles the Drag* calls but not the struct the test has to fill.
#include <shlobj_core.h>

#include <cstddef>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/PrismCore/DropPlan.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

namespace fs = std::filesystem;
using prism::drop::DropAction;
using prism::drop::DropPlan;
using prism::drop::DropReason;
using prism::win::gallery::GalleryWindow;

namespace {

std::string ToUtf8(const std::wstring& text) {
  if (text.empty()) return std::string();
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0, nullptr,
                                          nullptr);
  if (bytes <= 0) return std::string();
  std::string out(static_cast<std::size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes,
                        nullptr, nullptr);
  return out;
}

std::wstring ToWide(const std::string& text) {
  if (text.empty()) return std::wstring();
  const int chars = ::MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0);
  if (chars <= 0) return std::wstring();
  std::wstring out(static_cast<std::size_t>(chars), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
  return out;
}

bool Contains(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

// A REAL HDROP: the shell's own layout is a DROPFILES header followed by a
// double-NUL-terminated list of wide paths. Building it by hand is the only way
// to test the drop path without a mouse -- and DragQueryFileW reads it back
// through the same code the product runs. DragFinish (inside the window) frees
// the HGLOBAL, so the caller must not touch it afterwards.
HDROP MakeDrop(const std::vector<std::wstring>& paths, POINT point) {
  std::size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);  // trailing empty name
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
  header->fWide = TRUE;  // wide path list -- the only form this project accepts
  auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(header) + sizeof(DROPFILES));
  for (const std::wstring& path : paths) {
    ::memcpy(cursor, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    cursor += path.size() + 1;
  }
  *cursor = L'\0';  // the extra terminator: an empty name ends the list
  ::GlobalUnlock(memory);
  return static_cast<HDROP>(memory);
}

void WriteFile(const fs::path& path, const char* contents) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << contents;
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

// D3 predicate: did the injected "no recursion" defect actually fire? The same
// call the failure came from, asked directly, so a red for an unrelated reason
// can never print CAUGHT.
bool FlatInjectionFired() {
  prism::drop::DropEntry folder;
  folder.path = "C:/c52/pack";
  folder.isDirectory = true;
  prism::drop::DropEntry file;
  file.path = "C:/c52/pack/a.txt";
  folder.children.push_back(file);
  prism::drop::DropInput input;
  input.entries.push_back(folder);
  input.archiveOpen = true;
  input.containsDirectory = true;
  const DropPlan plan = prism::drop::PlanDrop(input);
  return plan.fileCount == 1 && plan.members.size() == 1;
}

bool HoldsPath(const DropPlan& plan, const std::wstring& path) {
  const std::string wanted = ToUtf8(path);
  for (const std::string& member : plan.members) {
    if (member == wanted) return true;
  }
  return false;
}

}  // namespace


// ---------------------------------------------------------------------------
// 1. The window must be a drop target at all.
// ---------------------------------------------------------------------------
PRISM_TEST("drop_in.the_window_is_registered_as_a_drop_target") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in registration", GetModuleHandleW(nullptr)))) return;
  // DragAcceptFiles sets WS_EX_ACCEPTFILES; Explorer checks it before it will
  // ever send WM_DROPFILES. Without this assertion the suite could go green on a
  // window that no real drag can reach.
  const LONG_PTR style = ::GetWindowLongPtrW(window.Hwnd(), GWL_EXSTYLE);
  PRISM_CHECK((style & WS_EX_ACCEPTFILES) != 0);
  PRISM_CHECK(!window.DropHintVisible());  // nothing to show before a drop
  PRISM_CHECK(window.DropHintRect().Empty());
}

// ---------------------------------------------------------------------------
// 2. A folder: walked on disk, added to the open archive
// ---------------------------------------------------------------------------
PRISM_TEST("drop_in.dropped_folder_is_walked_on_disk_and_added") {
  prism::testtmp::UniqueDir dir(L"c52", L"walk");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path pack = dir.path() / L"pack";
  std::error_code ec;
  fs::create_directories(pack / L"sub", ec);
  if (!PRISM_CHECK(!ec)) return;
  WriteFile(pack / L"a.txt", "alpha");
  WriteFile(pack / L"sub" / L"b.txt", "bravo");
  WriteFile(pack / L"c.txt", "charlie");

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in folder", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);  // attaching a catalog == an archive is open
  PRISM_CHECK(window.ArchiveOpen());

  HDROP drop = MakeDrop({pack.wstring()}, POINT{320, 240});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);  // consumes the handle (DragFinish)

  const DropPlan& plan = window.LastDropPlan();
  PRISM_CHECK_EQ(plan.primary, DropAction::AddToArchive);
  PRISM_CHECK_EQ(plan.reason, DropReason::None);
  PRISM_CHECK_EQ(plan.fileCount, std::size_t(3));  // the walk found all three files
  PRISM_CHECK_EQ(plan.directoryCount, std::size_t(2));
  PRISM_CHECK(plan.recursiveExpansion);
  // Exact paths (D2): a walk that "found something" is not the contract.
  PRISM_CHECK(HoldsPath(plan, pack / L"a.txt"));
  PRISM_CHECK(HoldsPath(plan, pack / L"sub" / L"b.txt"));
  PRISM_CHECK(HoldsPath(plan, pack / L"c.txt"));
  PRISM_CHECK(!HoldsPath(plan, pack));  // the folder itself is never a member
  PRISM_CHECK_EQ(plan.members.size(), plan.fileCount);
  PRISM_CHECK_EQ(window.DropPoint().x, 320L);
  PRISM_CHECK_EQ(window.DropPoint().y, 240L);

  // The hint exists, sits inside the CONTENT area, and names what will happen.
  PRISM_CHECK(window.DropHintVisible());
  const prism::layout::Rect content = window.ContentArea();
  const prism::layout::Rect hint = window.DropHintRect();
  if (!PRISM_CHECK(!hint.Empty())) return;
  PRISM_CHECK(content.Contains(hint));
  RECT client{};
  PRISM_CHECK(::GetClientRect(window.Hwnd(), &client) != 0);
  const prism::layout::Rect painted =
      prism::layout::Compute(client.right, client.bottom, false, false).contentInner;
  PRISM_CHECK(painted.Contains(hint));
  const std::string title = ToUtf8(window.DropHintTitle());
  const std::string detail = ToUtf8(window.DropHintDetail());
  PRISM_CHECK(Contains(title, "添加"));
  PRISM_CHECK(Contains(detail, "AddToArchive"));
  PRISM_CHECK(Contains(detail, "3"));
}

// ---------------------------------------------------------------------------
// 3. A plain file and NO archive: the window refuses and says why
// ---------------------------------------------------------------------------
PRISM_TEST("drop_in.plain_file_into_an_empty_window_is_refused_with_the_reason") {
  prism::testtmp::UniqueDir dir(L"c52", L"refuse");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path notes = dir.path() / L"notes.txt";
  WriteFile(notes, "not an archive");

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in refusal", GetModuleHandleW(nullptr)))) return;
  PRISM_CHECK(!window.ArchiveOpen());  // no catalog was attached: the empty window

  HDROP drop = MakeDrop({notes.wstring()}, POINT{100, 120});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);

  const DropPlan& plan = window.LastDropPlan();
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::NotAnArchive);
  PRISM_CHECK(!plan.accepted());
  PRISM_CHECK(plan.HasExplicitReason());

  // The overlay text the user reads must NAME the reason (requirement 3).
  const std::string title = ToUtf8(window.DropHintTitle());
  const std::string detail = ToUtf8(window.DropHintDetail());
  PRISM_CHECK(Contains(detail, "NotAnArchive"));
  PRISM_CHECK(Contains(detail, "不是归档"));
  PRISM_CHECK(Contains(title, "归档"));
  PRISM_CHECK(Contains(detail, "notes.txt"));  // and which file it was

  // A refusal still paints a hint, and it is still inside the content area.
  PRISM_CHECK(window.DropHintVisible());
  const prism::layout::Rect hint = window.DropHintRect();
  if (!PRISM_CHECK(!hint.Empty())) return;
  PRISM_CHECK(window.ContentArea().Contains(hint));
}

// ---------------------------------------------------------------------------
// 4. The hint reaches the DEVICE (real WM_PAINT + real Direct2D target)
// ---------------------------------------------------------------------------
PRISM_TEST("drop_in.the_hint_is_painted_by_a_real_paint_pass") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in paint", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);
  HDROP drop = MakeDrop({L"C:\\Windows\\notepad.exe"}, POINT{64, 64});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);  // a plain file onto an open archive: AddToArchive
  PRISM_CHECK_EQ(window.LastDropPlan().primary, DropAction::AddToArchive);
  PRISM_CHECK(window.DropHintVisible());

  // UpdateWindow sends WM_PAINT synchronously on THIS thread, so the counters
  // below are read after the frame really ran (no waiting, nothing to time out).
  ::ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
  ::InvalidateRect(window.Hwnd(), nullptr, FALSE);
  PRISM_CHECK(::UpdateWindow(window.Hwnd()) != 0);
  const int paints = window.PaintMessages();
  const int draws = window.DropHintDraws();
  std::printf("    WM_PAINTs=%d, overlay draws=%d\n", paints, draws);
  PRISM_CHECK(paints >= 1);
  PRISM_CHECK(draws >= 1);
  PRISM_CHECK_EQ(paints, draws);  // one overlay per frame, never a double draw

  // Dismissing the hint removes it from the NEXT frame (a permanent overlay
  // would be a new piece of chrome no user asked for).
  window.ClearDropHint();
  PRISM_CHECK(!window.DropHintVisible());
  ::InvalidateRect(window.Hwnd(), nullptr, FALSE);
  PRISM_CHECK(::UpdateWindow(window.Hwnd()) != 0);
  PRISM_CHECK_EQ(window.DropHintDraws(), draws);
  PRISM_CHECK(window.PaintMessages() > paints);
}

// ---------------------------------------------------------------------------
// 5. The rest of the matrix, through the SAME window entry point
// ---------------------------------------------------------------------------
PRISM_TEST("drop_in.archive_onto_an_open_archive_asks_and_a_click_dismisses") {
  prism::testtmp::UniqueDir dir(L"c52", L"ask");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path archive = dir.path() / L"photos.7z";
  // The suite asserts the DECISION, not the engine: the model classifies by
  // name, so a file with a .7z name is enough here (win.engine_bridge owns the
  // real 7z.dll round-trip).
  WriteFile(archive, "7z-signature");
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in ask", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);

  int calls = 0;
  DropAction seen = DropAction::Refuse;
  window.SetDropHandler([&calls, &seen](const DropPlan& plan) {
    ++calls;
    seen = plan.primary;
  });

  HDROP drop = MakeDrop({archive.wstring()}, POINT{10, 10});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);
  const DropPlan& plan = window.LastDropPlan();
  PRISM_CHECK_EQ(plan.primary, DropAction::AddToArchive);
  PRISM_CHECK(plan.asksUser);
  PRISM_CHECK_EQ(plan.alternatives.size(), std::size_t(2));
  PRISM_CHECK_EQ(plan.alternatives[0].action, DropAction::OpenArchive);
  PRISM_CHECK_EQ(plan.alternatives[1].action, DropAction::TestArchive);
  PRISM_CHECK_EQ(calls, 1);  // the app is told exactly once
  PRISM_CHECK_EQ(seen, DropAction::AddToArchive);

  // A click is the "changed my mind" path: the overlay goes away.
  PRISM_CHECK(window.DropHintVisible());
  window.Dispatch(WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(400, 400));
  PRISM_CHECK(!window.DropHintVisible());
}

PRISM_TEST("drop_in.read_only_archive_refusal_reaches_the_window_text") {
  prism::testtmp::UniqueDir dir(L"c52", L"readonly");
  if (!PRISM_CHECK(dir.ok())) return;
  const fs::path notes = dir.path() / L"notes.txt";
  WriteFile(notes, "x");
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in readonly", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);
  window.SetArchiveReadOnly(true);  // C27's state, C52's second input
  PRISM_CHECK(window.ArchiveReadOnly());
  PRISM_CHECK(window.ArchiveOpen());

  HDROP drop = MakeDrop({notes.wstring()}, POINT{5, 5});
  if (!PRISM_CHECK(drop != nullptr)) return;
  window.HandleDropFiles(drop);
  const DropPlan& plan = window.LastDropPlan();
  PRISM_CHECK_EQ(plan.primary, DropAction::Refuse);
  PRISM_CHECK_EQ(plan.reason, DropReason::ReadOnlyArchive);
  PRISM_CHECK(Contains(ToUtf8(window.DropHintDetail()), "ReadOnlyArchive"));
  PRISM_CHECK(Contains(ToUtf8(window.DropHintDetail()), "只读"));
}

PRISM_TEST("drop_in.empty_and_null_drops_are_refused_not_crashed") {
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"drop-in empty", GetModuleHandleW(nullptr)))) return;
  std::vector<prism::ArchiveItem> items = BuildCatalog();
  window.SetItems(&items);

  // A null handle cannot happen through the message loop, but "no paths" can:
  // an empty HDROP must produce a named refusal, not an empty plan.
  HDROP empty = MakeDrop({}, POINT{0, 0});
  if (!PRISM_CHECK(empty != nullptr)) return;
  window.HandleDropFiles(empty);
  PRISM_CHECK_EQ(window.LastDropPlan().primary, DropAction::Refuse);
  PRISM_CHECK_EQ(window.LastDropPlan().reason, DropReason::EmptySelection);

  window.HandleDropFiles(nullptr);  // and the degenerate call is survivable
  PRISM_CHECK_EQ(window.LastDropPlan().reason, DropReason::EmptySelection);
  PRISM_CHECK(window.DropHintVisible());
}

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "    FAIL: CoInitializeEx" << std::endl;
    return 1;
  }
#if defined(PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION)
  std::cout << "C52 MUTANT ACTIVE: a dropped folder is not expanded recursively" << std::endl;
  prism::test::SetMutationVerdict("C52 MUTATION CAUGHT: folder_flat",
                                  [] { return FlatInjectionFired(); });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL DROP-IN CHECKS PASSED" << std::endl;
  return 0;
}
