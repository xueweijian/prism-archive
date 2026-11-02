// PrismArchive -- PrismWin (card C54)
// EDITING AN ARCHIVE FROM THE UI, on a real HWND.
//
// C51 proved the engine (EditPlan + copy-on-write ApplyEdits). This suite proves
// the part the user actually touches:
//
//   1. DELETE: the Delete key on a selected row produces the intent, the
//      confirmation quotes C51's plan (affected entries + estimated write bytes
//      + "copy-on-write"), the edit runs through prism::app::RunArchiveEdit, and
//      after re-opening the archive the count is one lower while EVERY other
//      entry's payload bytes are unchanged.
//   2. RENAME (F2): the dialog carries the current name, the accepted name lands
//      in the archive, and the payload bytes are unchanged.
//   3. CANCEL: the dialog's Cancel writes NOTHING -- the archive file is
//      byte-for-byte identical afterwards. This is the card's most important
//      assertion; the negative control below makes it red on purpose.
//   4. CONCURRENCY: while an edit is in flight, a second operation (extraction
//      via row activation, or preview via Space) is REFUSED and the status line
//      says why -- never allowed to run in parallel.
//   5. ENTRY GATE: read-only / multi-volume / exclusively locked archives are
//      disabled with a readable reason before the user can confirm anything.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). The same source builds twice
// with ONE rule deleted:
//   cancel_writes      -- a cancelled dialog is treated as accepted;
//   allow_concurrent   -- the single-operation gate no longer refuses.
// No WILL_FAIL anywhere (D26): the binary arms SetMutationVerdict(prefix,
// predicate) BEFORE the first case, so the first failed assertion prints
// "C54 MUTATION CAUGHT: <id>" and leaves with 0 -- and only while the predicate
// proves the injected defect actually fired.
//
// D1: a missing 7z.dll/7z.exe prints WHY and exits 77 (the suite file maps it to
// SKIP_RETURN_CODE); the success marker is still required.
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/Shortcuts.hpp"
#include "../../Src/PrismWin/ArchiveEditor.hpp"
#include "../../Src/PrismWin/EditConfirmDialog.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace edit = prism::win::edit;
namespace editui = prism::win::editui;
using prism::ArchiveItem;
using prism::win::gallery::GalleryWindow;

namespace {

#if defined(PRISM_C54_MUTANT_CANCEL_STILL_WRITES)
constexpr const char* kMutation = "cancel_writes";
#elif defined(PRISM_C54_MUTANT_ALLOW_CONCURRENT)
constexpr const char* kMutation = "allow_concurrent";
#else
constexpr const char* kMutation = nullptr;
#endif

// CONTRACTS D3: the verdict predicate asks the SAME production rule the failed
// assertion came from, so a red for an unrelated reason can never print CAUGHT.
[[maybe_unused]] bool CancelInjectionFired() {
  editui::EditDialogResult cancelled;
  cancelled.accepted = false;
  return editui::EditMayProceed(cancelled);
}

[[maybe_unused]] bool ConcurrentInjectionFired() {
  return !editui::ConcurrencyGateActive();
}

bool Contains(const std::wstring& text, const std::wstring& needle) {
  return text.find(needle) != std::wstring::npos;
}

std::wstring DllPath() {
  wchar_t value[MAX_PATH]{};
  if (::GetEnvironmentVariableW(L"PRISM_7Z_DLL", value, MAX_PATH) == 0) return std::wstring();
  return value;
}

std::wstring ExePath() {
  const std::wstring dll = DllPath();
  if (dll.empty()) return std::wstring();
  const fs::path exe = fs::path(dll).parent_path() / L"7z.exe";
  std::error_code ec;
  return fs::exists(exe, ec) ? exe.wstring() : std::wstring();
}

bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring command = L"\"" + exe + L"\" " + args;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, cwd.c_str(), &startup, &process)) {
    return false;
  }
  const DWORD waited = ::WaitForSingleObject(process.hProcess, 120000);  // D29: bounded
  DWORD code = 1;
  ::GetExitCodeProcess(process.hProcess, &code);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return waited == WAIT_OBJECT_0 && code == 0;
}

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 131u + seed * 17u + (i >> 3)) & 0xFFu);
  }
  return bytes;
}

bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
  return out.good();
}

std::vector<uint8_t> ReadWholeFile(const std::wstring& path) {
  std::vector<uint8_t> bytes;
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return bytes;
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
      size.QuadPart <= static_cast<LONGLONG>(64u << 20)) {
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t done = 0;
    while (done < bytes.size()) {
      DWORD got = 0;
      const DWORD want = static_cast<DWORD>((std::min)(size_t{1} << 16, bytes.size() - done));
      if (!::ReadFile(file, bytes.data() + done, want, &got, nullptr) || got == 0) {
        bytes.clear();
        break;
      }
      done += got;
    }
  }
  ::CloseHandle(file);
  return bytes;
}

// --- one archive, fully described (same evidence shape as C51) --------------
struct Snapshot {
  std::vector<edit::EntryFingerprint> entries;
  std::map<std::string, std::vector<uint8_t>> payload;
  std::map<std::string, edit::EntryFingerprint> byPath;
};

bool TakeSnapshot(prism::win::SevenZipLibrary& library, const std::wstring& path,
                  Snapshot* out, std::string* why) {
  out->entries.clear();
  out->payload.clear();
  out->byPath.clear();
  if (!edit::FingerprintArchive(library, path, std::wstring(), &out->entries, why)) return false;
  for (const auto& entry : out->entries) out->byPath[entry.path] = entry;
  edit::EntryExtractor extractor(library);
  if (!extractor.Open(path, std::wstring(), why)) return false;
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

bool SameKept(const Snapshot& before, const Snapshot& after, const std::string& oldPath,
              const std::string& newPath, std::string* why) {
  const auto oldEntry = before.byPath.find(oldPath);
  const auto newEntry = after.byPath.find(newPath);
  if (oldEntry == before.byPath.end()) {
    *why = "fixture problem: '" + oldPath + "' was not in the original";
    return false;
  }
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
    *why = "'" + newPath + "' changed size";
    return false;
  }
  if (oldEntry->second.hasCrc && newEntry->second.hasCrc &&
      oldEntry->second.crc != newEntry->second.crc) {
    *why = "'" + newPath + "' changed its stored CRC";
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

bool ReadCatalog(prism::win::SevenZipLibrary& library, const std::wstring& path,
                 std::vector<ArchiveItem>* out, std::string* why) {
  prism::win::ArchiveReader probe(library);
  if (!probe.Open(path)) {
    *why = "cannot open " + prism::win::WideToUtf8(path) + ": " + probe.openTrace();
    return false;
  }
  *out = probe.ReadCatalog();
  if (out->empty()) {
    *why = "the fixture archive has an empty catalog";
    return false;
  }
  return true;
}

// A real 7z.exe fixture: four files under one solid block, plus a subfolder.
bool MakeArchive(prism::testtmp::UniqueDir& dir, const wchar_t* name, std::wstring* archive,
                 std::string* why) {
  const fs::path src = dir.path() / (std::wstring(name) + L".src");
  std::error_code ec;
  fs::create_directories(src / L"sub", ec);
  if (ec) {
    *why = "cannot create the fixture source tree";
    return false;
  }
  if (!WriteBytes(src / L"a.txt", Pattern(181, 3)) ||
      !WriteBytes(src / L"b.txt", Pattern(257, 5)) ||
      !WriteBytes(src / L"c.txt", Pattern(313, 7)) ||
      !WriteBytes(src / L"sub" / L"d.bin", Pattern(97, 11))) {
    *why = "cannot write the fixture payloads";
    return false;
  }
  *archive = (dir.path() / (std::wstring(name) + L".7z")).wstring();
  const std::wstring args =
      L"a -t7z -ms=on -y \"" + *archive + L"\" a.txt b.txt c.txt sub";
  if (!Run7z(ExePath(), args, src.wstring())) {
    *why = "7z.exe could not author the fixture archive";
    return false;
  }
  return true;
}

int FindRow(const std::vector<ArchiveItem>& items, const char* path) {
  for (size_t i = 0; i < items.size(); ++i) {
    if (!items[i].isDir && items[i].path == path) return static_cast<int>(i);
  }
  return -1;
}

prism::keys::Stroke Key(uint32_t key, bool ctrl = false, bool shift = false, bool alt = false) {
  prism::keys::Stroke stroke;
  stroke.key = key;
  stroke.ctrl = ctrl;
  stroke.shift = shift;
  stroke.alt = alt;
  return stroke;
}

// The production shape of "confirm, then execute": the plan and the dialog are
// built BEFORE the pool is closed, and the write is the only thing after it.
struct EditRun {
  bool ok = false;
  bool confirmed = false;
  std::string error;
  app::ArchiveEditOutcome outcome;
};

[[maybe_unused]] bool ExecuteAfterDialog(GalleryWindow& window, prism::win::SevenZipLibrary& library,
                                         std::shared_ptr<app::OperationHandles>* ops,
                                         const std::wstring& archive,
                                         const std::vector<ArchiveItem>& catalog,
                                         const editui::EditIntent& intent,
                                         const editui::EditDialogResult& answer, EditRun* run) {
  run->confirmed = editui::EditMayProceed(answer);
  if (!run->confirmed) return true;  // cancelled: nothing is written, by rule
  const std::vector<prism::EditOp> accepted = editui::BuildEditOps(intent, answer.input);
  if (accepted.empty()) {
    run->error = "the accepted name is empty";
    return false;
  }
  const prism::EditPlan plan = app::PreviewArchiveEdit(archive, catalog, accepted);
  if (!plan.ok) {
    run->error = plan.error;
    return false;
  }
  if (*ops) {
    (*ops)->Close();
    ops->reset();
  }
  app::ArchiveEditRequest request;
  request.archivePath = archive;
  request.ops = accepted;
  run->outcome = app::RunArchiveEdit(request);
  run->ok = run->outcome.ok;
  if (!run->ok) run->error = run->outcome.error;
  (void)window;
  (void)library;
  return run->ok;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Delete through the REAL window and the REAL engine; payload bytes of every
//    kept entry are compared before/after.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.delete_rewrites_minus_one_and_keeps_every_other_byte") {
  prism::testtmp::UniqueDir dir(L"c54", L"delete");
  if (!PRISM_CHECK(dir.ok())) return;
  const std::wstring dll = DllPath();
  const std::wstring exe = ExePath();
  if (!PRISM_CHECK(!dll.empty() && !exe.empty())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dll))) return;

  std::wstring archive;
  std::string why;
  if (!PRISM_CHECK(MakeArchive(dir, L"delete", &archive, &why))) {
    std::cout << "    fixture: " << why << std::endl;
    return;
  }
  std::vector<ArchiveItem> catalog;
  if (!PRISM_CHECK(ReadCatalog(library, archive, &catalog, &why))) {
    std::cout << "    catalog: " << why << std::endl;
    return;
  }
  Snapshot before;
  if (!PRISM_CHECK(TakeSnapshot(library, archive, &before, &why))) {
    std::cout << "    snapshot: " << why << std::endl;
    return;
  }
  const size_t countBefore = catalog.size();
  PRISM_CHECK(countBefore >= static_cast<size_t>(4));  // a/b/c + sub/d (+ the folder node)

  app::OperationHandles::Config config;
  config.archivePath = archive;
  config.handlerSlots = 2;
  std::string poolError;
  std::shared_ptr<app::OperationHandles> ops =
      app::OperationHandles::Create(library, config, &poolError);
  if (!PRISM_CHECK(ops != nullptr)) {
    std::cout << "    pool: " << poolError << std::endl;
    return;
  }

  std::vector<ArchiveItem> view = catalog;
  std::vector<ArchiveItem> sessionCatalog = catalog;
  std::wstring sessionPath = archive;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 delete", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  window.SetRootLabel(L"delete.7z");
  window.SetEditAvailability(true);

  int intents = 0;
  editui::EditIntent seen;
  window.SetEditHandler([&intents, &seen](const editui::EditIntent& intent) {
    ++intents;
    seen = intent;
  });
  const int row = FindRow(view, "b.txt");
  if (!PRISM_CHECK(row >= 0)) return;
  window.SetSelection(row);
  // The shortcut route: WM_KEYDOWN would build exactly this stroke.
  window.DispatchKeyStroke(Key(prism::keys::kKeyDelete));
  PRISM_CHECK_EQ(intents, 1);
  PRISM_CHECK_EQ(seen.action, editui::EditAction::Delete);
  PRISM_CHECK_EQ(seen.path, std::string("b.txt"));
  PRISM_CHECK(!seen.recurse);

  // The confirmation quotes C51's plan -- the affected count and the write cost
  // are the plan's, never a second estimate.
  const std::vector<prism::EditOp> opsV = editui::BuildEditOps(seen);
  const prism::EditPlan preview = app::PreviewArchiveEdit(archive, catalog, opsV);
  if (!PRISM_CHECK(preview.ok)) {
    std::cout << "    plan: " << preview.error << std::endl;
    return;
  }
  PRISM_CHECK_EQ(preview.entriesAffected, static_cast<uint64_t>(1));
  PRISM_CHECK(preview.estimatedWriteBytes > 0);
  const editui::EditDialogModel model =
      editui::BuildEditDialogModel(seen.action, seen.path, preview, L"delete.7z");
  PRISM_CHECK(model.dangerous);
  PRISM_CHECK(!model.needsInput);
  PRISM_CHECK(Contains(model.body, L"copy-on-write"));
  PRISM_CHECK(Contains(model.body, L"重写整个归档"));
  PRISM_CHECK(Contains(model.body, L"受影响条目：" + std::to_wstring(preview.entriesAffected)));
  PRISM_CHECK(Contains(model.body, L"预估写入量：" +
                                       std::to_wstring(preview.estimatedWriteBytes)));

  editui::EditConfirmDialog dialog;
  if (!PRISM_CHECK(dialog.Create(::GetModuleHandleW(nullptr), model.title))) return;
  dialog.SetModel(model);
  PRISM_CHECK(!dialog.InputVisible());
  editui::EditDialogResult answer;
  dialog.SetResultHandler([&answer](const editui::EditDialogResult& result) { answer = result; });
  // REAL control path: the OK button posts WM_COMMAND, exactly like a click.
  ::SendMessageW(dialog.Hwnd(), WM_COMMAND, MAKEWPARAM(editui::ids::kOk, BN_CLICKED),
                 reinterpret_cast<LPARAM>(dialog.Control(editui::ids::kOk)));
  PRISM_CHECK(answer.accepted);

  EditRun run;
  PRISM_CHECK(ExecuteAfterDialog(window, library, &ops, sessionPath, catalog, seen, answer, &run));
  PRISM_CHECK(run.ok);
  PRISM_CHECK(run.outcome.verified);
  std::cout << "    edit: entries=" << run.outcome.itemsWritten
            << " written=" << run.outcome.bytesWritten
            << " verified=" << run.outcome.verifiedItems << std::endl;

  // C34's refresh: the SAME path is re-opened, the catalogue re-read and the view
  // republished. This is the production refresh, not a test-only re-catalog.
  app::ArchiveSession session{&library, &ops, &sessionCatalog, &view, &sessionPath, 2};
  const app::ArchiveSwitch switched = app::SwitchArchive(session, archive);
  if (!PRISM_CHECK(switched.ok)) {
    std::cout << "    switch: " << switched.error << std::endl;
    return;
  }
  PRISM_CHECK(switched.switched);
  window.SetItems(&view);
  PRISM_CHECK_EQ(view.size(), countBefore - 1);
  // The window shows the ROOT folder, so compare its row count with the root
  // entries of the refreshed catalog (sub/d.bin lives one level down).
  int rootRows = 0;
  for (const ArchiveItem& item : view) {
    if (item.path.find('/') == std::string::npos) ++rootRows;
  }
  PRISM_CHECK_EQ(window.ItemCount(), rootRows);

  Snapshot after;
  if (!PRISM_CHECK(TakeSnapshot(library, archive, &after, &why))) {
    std::cout << "    snapshot after: " << why << std::endl;
    return;
  }
  PRISM_CHECK_EQ(after.entries.size(), countBefore - 1);
  PRISM_CHECK(after.byPath.find("b.txt") == after.byPath.end());
  for (const char* kept : {"a.txt", "c.txt", "sub/d.bin"}) {
    if (!PRISM_CHECK(SameKept(before, after, kept, kept, &why))) {
      std::cout << "    kept: " << why << std::endl;
    }
  }
  if (ops) ops->Close();
}

// ---------------------------------------------------------------------------
// 2. F2 rename: the dialog's text field carries the current name, the archive
//    gets the new one, and the payload is the same payload.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.rename_via_f2_changes_the_name_not_the_bytes") {
  prism::testtmp::UniqueDir dir(L"c54", L"rename");
  if (!PRISM_CHECK(dir.ok())) return;
  const std::wstring dll = DllPath();
  if (!PRISM_CHECK(!dll.empty() && !ExePath().empty())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dll))) return;

  std::wstring archive;
  std::string why;
  if (!PRISM_CHECK(MakeArchive(dir, L"rename", &archive, &why))) {
    std::cout << "    fixture: " << why << std::endl;
    return;
  }
  std::vector<ArchiveItem> catalog;
  if (!PRISM_CHECK(ReadCatalog(library, archive, &catalog, &why))) return;
  Snapshot before;
  if (!PRISM_CHECK(TakeSnapshot(library, archive, &before, &why))) return;

  std::vector<ArchiveItem> view = catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 rename", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  window.SetEditAvailability(true);
  int intents = 0;
  editui::EditIntent seen;
  window.SetEditHandler([&intents, &seen](const editui::EditIntent& intent) {
    ++intents;
    seen = intent;
  });
  const int row = FindRow(view, "a.txt");
  if (!PRISM_CHECK(row >= 0)) return;
  window.SetSelection(row);
  window.DispatchKeyStroke(Key(prism::keys::kKeyF2));
  PRISM_CHECK_EQ(intents, 1);
  PRISM_CHECK_EQ(seen.action, editui::EditAction::Rename);
  PRISM_CHECK_EQ(seen.path, std::string("a.txt"));
  PRISM_CHECK_EQ(seen.leaf, std::string("a.txt"));

  const prism::EditPlan preview = app::PreviewArchiveEdit(archive, catalog,
                                                          editui::DefaultEditOps(seen));
  if (!PRISM_CHECK(preview.ok)) return;
  editui::EditDialogModel model =
      editui::BuildEditDialogModel(seen.action, seen.path, preview, L"rename.7z");
  PRISM_CHECK(model.needsInput);
  PRISM_CHECK(Contains(model.body, L"copy-on-write"));

  editui::EditConfirmDialog dialog;
  if (!PRISM_CHECK(dialog.Create(::GetModuleHandleW(nullptr), model.title))) return;
  dialog.SetModel(model);
  PRISM_CHECK(dialog.InputVisible());
  PRISM_CHECK(dialog.InputText() == L"a.txt");
  dialog.SetInputText(L"renamed.txt");
  editui::EditDialogResult answer;
  dialog.SetResultHandler([&answer](const editui::EditDialogResult& result) { answer = result; });
  ::SendMessageW(dialog.Hwnd(), WM_COMMAND, MAKEWPARAM(editui::ids::kOk, BN_CLICKED),
                 reinterpret_cast<LPARAM>(dialog.Control(editui::ids::kOk)));
  PRISM_CHECK(answer.accepted);
  PRISM_CHECK(answer.input == L"renamed.txt");

  std::shared_ptr<app::OperationHandles> ops;  // no pool needed: this edit owns its readers
  EditRun run;
  PRISM_CHECK(ExecuteAfterDialog(window, library, &ops, archive, catalog, seen, answer, &run));
  PRISM_CHECK(run.ok);
  PRISM_CHECK(run.outcome.verified);

  Snapshot after;
  if (!PRISM_CHECK(TakeSnapshot(library, archive, &after, &why))) return;
  PRISM_CHECK(after.byPath.find("a.txt") == after.byPath.end());
  if (!PRISM_CHECK(SameKept(before, after, "a.txt", "renamed.txt", &why))) {
    std::cout << "    rename: " << why << std::endl;
  }
  if (!PRISM_CHECK(SameKept(before, after, "b.txt", "b.txt", &why))) {
    std::cout << "    sibling: " << why << std::endl;
  }
}

// ---------------------------------------------------------------------------
// 3. CANCEL WRITES NOTHING. The whole archive file is compared byte-for-byte.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.cancel_leaves_the_archive_byte_identical") {
  prism::testtmp::UniqueDir dir(L"c54", L"cancel");
  if (!PRISM_CHECK(dir.ok())) return;
  const std::wstring dll = DllPath();
  if (!PRISM_CHECK(!dll.empty() && !ExePath().empty())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dll))) return;

  std::wstring archive;
  std::string why;
  if (!PRISM_CHECK(MakeArchive(dir, L"cancel", &archive, &why))) return;
  std::vector<ArchiveItem> catalog;
  if (!PRISM_CHECK(ReadCatalog(library, archive, &catalog, &why))) return;
  const std::vector<uint8_t> fileBefore = ReadWholeFile(archive);
  if (!PRISM_CHECK(!fileBefore.empty())) return;

  std::vector<ArchiveItem> view = catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 cancel", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  window.SetEditAvailability(true);
  editui::EditIntent seen;
  window.SetEditHandler([&seen](const editui::EditIntent& intent) { seen = intent; });
  const int row = FindRow(view, "c.txt");
  if (!PRISM_CHECK(row >= 0)) return;
  window.SetSelection(row);
  window.DispatchKeyStroke(Key(prism::keys::kKeyDelete));

  const std::vector<prism::EditOp> opsV = editui::BuildEditOps(seen);
  const prism::EditPlan preview = app::PreviewArchiveEdit(archive, catalog, opsV);
  if (!PRISM_CHECK(preview.ok)) return;
  editui::EditDialogModel model =
      editui::BuildEditDialogModel(seen.action, seen.path, preview, L"cancel.7z");

  std::shared_ptr<app::OperationHandles> ops;
  bool wrote = false;
  bool sawCancel = false;
  editui::EditConfirmDialog dialog;
  if (!PRISM_CHECK(dialog.Create(::GetModuleHandleW(nullptr), model.title))) return;
  dialog.SetModel(model);
  dialog.SetResultHandler([&](const editui::EditDialogResult& result) {
    // EXACTLY the production rule: a cancelled dialog means no write.
    if (!editui::EditMayProceed(result)) {
      sawCancel = true;
      return;
    }
    wrote = true;
    EditRun run;
    (void)ExecuteAfterDialog(window, library, &ops, archive, catalog, seen, result, &run);
  });
  // The user presses Cancel (IDCANCEL), through the real WM_COMMAND handler.
  ::SendMessageW(dialog.Hwnd(), WM_COMMAND, MAKEWPARAM(editui::ids::kCancel, BN_CLICKED),
                 reinterpret_cast<LPARAM>(dialog.Control(editui::ids::kCancel)));
  PRISM_CHECK(sawCancel);
  PRISM_CHECK(!wrote);

  const std::vector<uint8_t> fileAfter = ReadWholeFile(archive);
  PRISM_CHECK_EQ(fileAfter.size(), fileBefore.size());
  PRISM_CHECK(fileAfter == fileBefore);
  if (ops) ops->Close();
}

// ---------------------------------------------------------------------------
// 4. The single-operation gate: a second extract/preview is REFUSED with a
//    readable reason, and the app's handler never runs.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.edit_refuses_concurrent_extract_and_preview") {
  prism::testtmp::UniqueDir dir(L"c54", L"busy");
  if (!PRISM_CHECK(dir.ok())) return;
  std::vector<ArchiveItem> view;
  ArchiveItem a;
  a.index = 0;
  a.path = "a.txt";
  a.size = 8;
  view.push_back(a);
  ArchiveItem b;
  b.index = 1;
  b.path = "b.txt";
  b.size = 9;
  view.push_back(b);

  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 busy", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  window.SetEditAvailability(true);
  int activations = 0;
  window.SetActivateHandler([&activations](int) { ++activations; });
  window.SetSelection(1);
  PRISM_CHECK(!window.OperationBusy());

  // The app begins an edit; from here the window owns the refusal.
  PRISM_CHECK(window.BeginOperation(GalleryWindow::OperationKind::Edit, L"编辑归档"));
  PRISM_CHECK(window.OperationBusy());
  PRISM_CHECK(window.BusyKind() == GalleryWindow::OperationKind::Edit);

  // A double-click / Enter on a file starts an extraction in this product. It
  // must be refused, not run in parallel.
  PRISM_CHECK(window.ActivateRow(1));
  PRISM_CHECK_EQ(activations, 0);
  PRISM_CHECK_EQ(window.RefusedOperationCount(), 1);
  PRISM_CHECK(Contains(window.LastOperationRefusal(), L"编辑"));
  PRISM_CHECK(Contains(window.LastOperationRefusal(), L"解压"));
  PRISM_CHECK(Contains(window.StatusText(), L"解压"));

  // Space opens Quick Look -- also refused.
  window.DispatchKeyStroke(Key(prism::keys::kKeySpace));
  PRISM_CHECK_EQ(activations, 0);  // still no extraction
  PRISM_CHECK_EQ(window.RefusedOperationCount(), 2);
  PRISM_CHECK(Contains(window.LastOperationRefusal(), L"预览"));
  PRISM_CHECK(!window.QuickLookOpen());
  std::cout << "    refusal: " << prism::win::WideToUtf8(window.LastOperationRefusal())
            << std::endl;

  window.EndOperation();
  PRISM_CHECK(!window.OperationBusy());
  // With the gate released the same activation is allowed again.
  PRISM_CHECK(window.ActivateRow(1));
  PRISM_CHECK_EQ(activations, 1);
}

// ---------------------------------------------------------------------------
// 5. The entry gate: read-only / multi-volume / exclusive lock are diagnosed
//    BEFORE the confirm, and the window's Edit menu is greyed.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.read_only_multivolume_and_locked_entries_are_disabled") {
  prism::testtmp::UniqueDir dir(L"c54", L"gate");
  if (!PRISM_CHECK(dir.ok())) return;
  const std::wstring dll = DllPath();
  if (!PRISM_CHECK(!dll.empty() && !ExePath().empty())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dll))) return;

  std::wstring archive;
  std::string why;
  if (!PRISM_CHECK(MakeArchive(dir, L"gate", &archive, &why))) return;
  std::vector<ArchiveItem> catalog;
  if (!PRISM_CHECK(ReadCatalog(library, archive, &catalog, &why))) return;

  // (a) read-only.
  if (!PRISM_CHECK(::SetFileAttributesW(archive.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE)) return;
  app::ArchiveEditGate readOnly = app::ProbeEditGate(archive);
  PRISM_CHECK(!readOnly.enabled);
  PRISM_CHECK_EQ(readOnly.refusal, prism::EditRefusal::ReadOnlyArchive);
  PRISM_CHECK(Contains(readOnly.reason, L"只读"));
  ::SetFileAttributesW(archive.c_str(), FILE_ATTRIBUTE_NORMAL);

  std::vector<ArchiveItem> view = catalog;
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 gate", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  int edits = 0;
  window.SetEditHandler([&edits](const editui::EditIntent&) { ++edits; });
  window.SetEditAvailability(readOnly.enabled, readOnly.reason);
  PRISM_CHECK(!window.EditEnabled());
  PRISM_CHECK(!window.MenuItemEnabled(GalleryWindow::kMenuEditDelete));
  PRISM_CHECK(!window.MenuItemEnabled(GalleryWindow::kMenuEditRename));
  window.SetSelection(FindRow(view, "a.txt"));
  window.DispatchKeyStroke(Key(prism::keys::kKeyDelete));
  PRISM_CHECK_EQ(edits, 0);  // the entry is disabled BEFORE the intent exists
  PRISM_CHECK(Contains(window.StatusText(), L"只读"));

  // (b) multi-volume: "x.7z" beside "x.7z.001".
  const std::wstring volumeBase = (dir.path() / L"vol.7z").wstring();
  std::error_code ec;
  fs::copy_file(fs::path(archive), fs::path(volumeBase), fs::copy_options::overwrite_existing, ec);
  if (!PRISM_CHECK(!ec)) return;
  fs::copy_file(fs::path(archive), fs::path(volumeBase + L".001"),
                fs::copy_options::overwrite_existing, ec);
  if (!PRISM_CHECK(!ec)) return;
  const app::ArchiveEditGate multi = app::ProbeEditGate(volumeBase);
  PRISM_CHECK(!multi.enabled);
  PRISM_CHECK_EQ(multi.refusal, prism::EditRefusal::MultiVolumeArchive);
  PRISM_CHECK(Contains(multi.reason, L"分卷"));

  // (c) exclusively held by "another process" (this test's own handle).
  HANDLE exclusive = ::CreateFileW(archive.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!PRISM_CHECK(exclusive != INVALID_HANDLE_VALUE)) return;
  const app::ArchiveEditGate locked = app::ProbeEditGate(archive);
  ::CloseHandle(exclusive);
  PRISM_CHECK(!locked.enabled);
  PRISM_CHECK(Contains(locked.reason, L"独占"));
}

// ---------------------------------------------------------------------------
// 6. Ctrl+Shift+N is a REAL archive edit entry: it produces a NewFolder intent
//    and a plan whose confirmation carries the same rewrite warning.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.ctrl_shift_n_creates_a_new_folder_intent") {
  prism::testtmp::UniqueDir dir(L"c54", L"mkdir");
  if (!PRISM_CHECK(dir.ok())) return;
  std::vector<ArchiveItem> view;
  ArchiveItem a;
  a.index = 0;
  a.path = "a.txt";
  a.size = 4;
  view.push_back(a);
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C54 mkdir", ::GetModuleHandleW(nullptr)))) return;
  window.SetItems(&view);
  window.SetEditAvailability(true);
  editui::EditIntent seen;
  int intents = 0;
  window.SetEditHandler([&seen, &intents](const editui::EditIntent& intent) {
    seen = intent;
    ++intents;
  });
  window.DispatchKeyStroke(Key('N', /*ctrl=*/true, /*shift=*/true));
  PRISM_CHECK_EQ(intents, 1);
  PRISM_CHECK_EQ(seen.action, editui::EditAction::NewFolder);
  PRISM_CHECK_EQ(seen.directory, std::string());
  const std::vector<prism::EditOp> ops = editui::DefaultEditOps(seen);
  if (!PRISM_CHECK_EQ(ops.size(), static_cast<size_t>(1))) return;
  PRISM_CHECK_EQ(ops[0].kind, prism::EditOpKind::NewFolder);

  prism::EditArchiveFacts facts;
  const prism::EditPlan plan = prism::BuildEditPlan(view, ops, facts);
  if (!PRISM_CHECK(plan.ok)) return;
  const editui::EditDialogModel model =
      editui::BuildEditDialogModel(seen.action, seen.path, plan, L"mkdir.7z");
  PRISM_CHECK(!model.dangerous);
  PRISM_CHECK(model.needsInput);
  PRISM_CHECK(Contains(model.body, L"copy-on-write"));
  PRISM_CHECK(Contains(model.body, L"受影响条目："));
}

// ---------------------------------------------------------------------------
// 7. A FAILED rewrite leaves the original byte-for-byte untouched. The failure
//    is forced DURING the update (the progress callback asks to stop), so the
//    temp archive really exists and C51's cleanup is what protects the file.
// ---------------------------------------------------------------------------
PRISM_TEST("c54.failed_edit_keeps_the_archive_byte_identical") {
  prism::testtmp::UniqueDir dir(L"c54", L"failure");
  if (!PRISM_CHECK(dir.ok())) return;
  const std::wstring dll = DllPath();
  if (!PRISM_CHECK(!dll.empty() && !ExePath().empty())) return;
  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(dll))) return;

  std::wstring archive;
  std::string why;
  if (!PRISM_CHECK(MakeArchive(dir, L"failure", &archive, &why))) return;
  std::vector<ArchiveItem> catalog;
  if (!PRISM_CHECK(ReadCatalog(library, archive, &catalog, &why))) return;
  const std::vector<uint8_t> before = ReadWholeFile(archive);
  if (!PRISM_CHECK(!before.empty())) return;

  app::ArchiveEditRequest request;
  request.archivePath = archive;
  prism::EditOp op;
  op.kind = prism::EditOpKind::Delete;
  op.path = "b.txt";
  request.ops.push_back(op);
  // Stop at the first progress callback: UpdateItems fails, the temp is deleted
  // and the original is never published.
  request.progress = [](uint64_t) { return false; };
  const app::ArchiveEditOutcome outcome = app::RunArchiveEdit(request);
  PRISM_CHECK(!outcome.ok);
  PRISM_CHECK(!outcome.error.empty());
  std::cout << "    failure: " << outcome.error << std::endl;

  const std::vector<uint8_t> after = ReadWholeFile(archive);
  PRISM_CHECK_EQ(after.size(), before.size());
  PRISM_CHECK(after == before);
}


int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return 77;
  }
  if (DllPath().empty() || ExePath().empty()) {
    std::cout << "SKIP: 7z.dll/7z.exe not available -- set PRISM_7Z_DLL to a 7z.dll that has "
                 "7z.exe beside it"
              << std::endl;
    return 77;
  }
  if (kMutation != nullptr) {
    std::cout << "C54 MUTANT ACTIVE: " << kMutation << std::endl;
    // CONTRACTS D27: arm BEFORE the first assertion; the first failure prints the
    // marker and leaves with 0, and only while the injected defect really fired.
    if (std::strcmp(kMutation, "cancel_writes") == 0) {
      prism::test::SetMutationVerdict("C54 MUTATION CAUGHT: cancel_writes",
                                      [] { return CancelInjectionFired(); });
    } else {
      prism::test::SetMutationVerdict("C54 MUTATION CAUGHT: allow_concurrent",
                                      [] { return ConcurrentInjectionFired(); });
    }
  }
  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::cout << "C54 MUTATION NOT CAUGHT: the suite stayed green with the regression compiled "
                   "in -- the injection is stale"
                << std::endl;
      return 1;
    }
    std::cout << "C54 MUTATION CAUGHT: " << kMutation << " (suite red, see FAIL lines)"
              << std::endl;
    return 0;
  }
  if (rc != 0) return rc;
  std::cout << "ALL EDIT-UI CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  ::CoUninitialize();
  return 0;
}
