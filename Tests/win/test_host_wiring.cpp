// Windows integration test (card C34): the three joins that make the modules
// that already landed REACHABLE from the running application.
//
// THIS SUITE TESTS WIRING, NOT MODULES. C30 (the preview channel + flyout),
// C33 (GlassIslandHost::ReleaseTransient + ThumbStore::Trim) and C11
// (ReaderPool) each shipped with their own suite; what none of them could prove
// is that the EXE uses them. So the objects under test are the exact ones
// Main.cpp drives:
//
//   (1) THE COMPANION IS HOSTED. With no companion, the shell's publish answers
//       NoReader and every right-click launches the app instead. So: before the
//       host starts nothing owns the section; after Start the SHELL'S OWN PROBE
//       (CompanionPreviewPresent) answers true; the flyout is INVISIBLE until
//       the shell publishes a wake; on the wake it lists the archive's first 12
//       entries, acks the writer, and becomes visible WITHOUT taking focus
//       (GetForegroundWindow is unchanged); ESC closes it again.
//
//   (2) THE MINIMISE PATH GIVES MEMORY BACK. C33 measured that releasing a live
//       glass graph returns 0 bytes without IDXGIDevice3::Trim -- so the
//       assertion is on the ACCOUNTING (TransientBytes derived from live COM
//       pointers): live before the minimise, zero after, and exactly what it
//       held is reported as returned. The thumbnail store is trimmed on the
//       same event, which is asserted on its resident bytes.
//
//   (3) THE FINISHED RE-PACK BECOMES THE OPEN ARCHIVE. A real compression run
//       (C27's path) writes a NEW file; prism::app::SwitchArchive then closes
//       the old pool, opens the new file and republishes the catalogue. The
//       assertions: the session's path and entry count move, the OLD FILE CAN BE
//       DELETED (the old pool really released it -- a leak keeps that handle and
//       fails the call), the NEW pool can still lease a handler, and a switch to
//       a missing file changes NOTHING.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26). The same source is compiled twice
// more with one regression injected at COMPILE TIME:
//   * PRISM_C34_MUTANT_NO_TRIM            -- the minimise path keeps the layer (2)
//   * PRISM_C34_MUTANT_NO_ARCHIVE_SWITCH  -- the refresh keeps the old archive (3)
// The verdict shape is D26's: NO WILL_FAIL anywhere. The mutant prints a banner
// (proof the injection compiled in) and only when the suite really went RED
// prints "C34 MUTATION CAUGHT: <id>" and exits 0 -- that marker is the target's
// PASS_REGULAR_EXPRESSION. If the injection ever stopped applying, the suite
// would stay green, the marker would be absent and the target would fail.
//
// D1: no silent skip. A missing 7z.dll/7z.exe, a window station that refuses a
// window, or a machine without DirectComposition prints WHY and exits 77 (the
// suite file maps that to SKIP_RETURN_CODE); the pass marker is still required.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "SolidFixture.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace companion = prism::win::companion;
using prism::ArchiveItem;

namespace {

#if defined(PRISM_C34_MUTANT_NO_TRIM)
constexpr const char* kMutation = "no_trim";
#elif defined(PRISM_C34_MUTANT_NO_ARCHIVE_SWITCH)
constexpr const char* kMutation = "no_archive_switch";
#else
constexpr const char* kMutation = nullptr;
#endif

// Set when the environment refuses a resource this suite needs. A base run turns
// this into a LABELLED skip (exit 77); a mutation run turns it into RED (a
// mutant that could not run must never look like a caught mutation).
bool g_noWindowStation = false;
bool g_noComposition = false;
// Set when ANOTHER process (a stray PrismArchive.exe from a bench, or a
// developer's running app) already owns the session-wide preview section. The
// channel is "Local" by design, so this suite cannot prove the no-reader
// premise in that state -- it says so and skips instead of reporting a false red.
bool g_foreignCompanion = false;

struct Fixture {
  std::wstring dllPath;
  std::wstring exePath;
  std::wstring root;
  std::wstring archiveOld;  // 15 entries: more than the flyout's 12
  std::wstring archiveNew;  // written by the C27 compression path
  prism::win::SevenZipLibrary library;
  std::vector<ArchiveItem> catalogOld;
  std::vector<ArchiveItem> catalogNew;
  bool ready = false;
  std::string why;
};

Fixture& World() {
  static Fixture fixture;
  return fixture;
}

bool WriteText(const std::wstring& path, const std::string& bytes) {
  std::ofstream file(fs::path(path), std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

// 7z.exe with an explicit working directory, so the STORED names are relative.
bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring command = L"\"" + exe + L"\" " + args;
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &startup, &process)) {
    return false;
  }
  WaitForSingleObject(process.hProcess, 120000);
  DWORD code = 1;
  GetExitCodeProcess(process.hProcess, &code);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return code == 0;
}

bool ReadCatalog(prism::win::SevenZipLibrary& library, const std::wstring& path,
                 std::vector<ArchiveItem>* out, std::string* why) {
  prism::win::ArchiveReader probe(library);
  if (!probe.Open(path)) {
    *why = "cannot open " + prism::win::WideToUtf8(path) + ": " + probe.openTrace();
    return false;
  }
  *out = probe.ReadCatalog();
  return !out->empty();
}

constexpr int kOldEntryCount = 15;  // more than the flyout's 12 on purpose

bool SetupFixture() {
  Fixture& fx = World();
  if (fx.ready) return true;
  if (!fx.why.empty()) return false;
  fx.dllPath = prism::fixture::DllPath();
  fx.exePath = prism::fixture::ExePath();
  if (fx.dllPath.empty() || fx.exePath.empty()) {
    fx.why = "7z.dll/7z.exe not available";
    return false;
  }
  // UNIQUE per run (card + process id): the Windows lane is serial today, and it
  // will be parallel again -- eight suites that shell out to 7z.exe against ONE
  // shared %TEMP% folder is exactly what broke win.reader_pool. A fixture folder
  // nobody else can name cannot collide.
  const fs::path root = fs::temp_directory_path() /
                        ("prism_c34_host_" + std::to_string(::GetCurrentProcessId()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (ec) {
    fx.why = "cannot create the fixture folder";
    return false;
  }
  fx.root = root.wstring();
  fx.archiveOld = (root / "old.7z").wstring();
  fx.archiveNew = (root / "new.7z").wstring();

  // (a) The 15-entry archive the flyout must truncate to 12. Authored by the
  // real 7z.exe so the catalogue is the engine's, not a hand-written struct.
  std::wstring names;
  for (int i = 0; i < kOldEntryCount; ++i) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "f%02d.txt", i);
    const std::wstring name = prism::win::Utf8ToWide(buffer);
    if (!WriteText((root / name).wstring(), "payload " + std::string(buffer))) {
      fx.why = "cannot write the fixture payloads";
      return false;
    }
    names += L" " + name;
  }
  if (!Run7z(fx.exePath, L"a -t7z -ms=on \"" + fx.archiveOld + L"\"" + names, fx.root)) {
    fx.why = "7z.exe could not author old.7z";
    return false;
  }

  // (b) The 3-entry archive the switch target is: written by the C27 path the
  // window's Compress action uses (plan -> RunCompression -> ArchiveWriter).
  const fs::path payload = root / "payload";
  fs::create_directories(payload, ec);
  std::vector<std::wstring> inputs;
  for (int i = 0; i < 3; ++i) {
    const std::wstring name = L"packed" + std::to_wstring(i) + L".bin";
    if (!WriteText((payload / name).wstring(), "repacked bytes")) {
      fx.why = "cannot write the re-pack payloads";
      return false;
    }
    inputs.push_back((payload / name).wstring());
  }
  const app::CompressionTarget target =
      app::PlanCompression(inputs, prism::ArchiveFormat::SevenZip, fx.archiveNew);
  if (!target.ok) {
    fx.why = "compression plan failed: " + target.error;
    return false;
  }
  app::CompressionRequest job;
  job.archivePath = target.archivePath;
  job.sourceRoot = target.sourceRoot;
  job.plan = target.plan;
  const app::CompressionOutcome created = app::RunCompression(job);
  if (!created.ok) {
    fx.why = "the C27 compression path could not write new.7z: " + created.error;
    return false;
  }

  if (!fx.library.Load(fx.dllPath)) {
    fx.why = "LoadLibrary(7z.dll) failed";
    return false;
  }
  if (!ReadCatalog(fx.library, fx.archiveOld, &fx.catalogOld, &fx.why)) return false;
  if (!ReadCatalog(fx.library, fx.archiveNew, &fx.catalogNew, &fx.why)) return false;
  if (fx.catalogOld.size() != static_cast<size_t>(kOldEntryCount)) {
    fx.why = "old.7z should hold " + std::to_string(kOldEntryCount) + " entries, holds " +
             std::to_string(fx.catalogOld.size());
    return false;
  }
  if (fx.catalogNew.size() != 3) {
    fx.why = "new.7z should hold 3 entries, holds " + std::to_string(fx.catalogNew.size());
    return false;
  }
  fx.ready = true;
  return true;
}

prism::gallery::DecodedThumb MakeThumb(uint32_t width, uint32_t height) {
  prism::gallery::DecodedThumb thumb;
  thumb.width = width;
  thumb.height = height;
  thumb.pixels.assign(static_cast<size_t>(width) * height * 4u, 0x7F);
  return thumb;
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) The companion is HOSTED: section owned, hidden until woken, no focus steal.
// ---------------------------------------------------------------------------
PRISM_TEST("companion: the exe owns the channel, stays hidden, wakes into a 12-row flyout") {
  // The premise: nothing owns the section yet, so the shell's probe says "no
  // companion" and every right-click takes the NoReader path. This is the state
  // the whole card exists to end.
  if (companion::CompanionPreviewPresent()) {
    std::cout << "  [skip] another process owns the preview section (stray PrismArchive.exe?)"
              << std::endl;
    g_foreignCompanion = true;
    return;
  }

  app::CompanionHost host;
  app::CompanionHost::Config config;
  config.openArchive = &World().archiveOld;
  config.catalog = &World().catalogOld;  // the fast path: no I/O on a wake
  std::string why;
  if (!PRISM_CHECK(host.Start(::GetModuleHandleW(nullptr), config, &why))) {
    std::cout << "    companion host did not start: " << why << std::endl;
    g_noWindowStation = true;
    return;
  }
  // THE JOIN THAT WAS MISSING: the shell now sees a reader, so it publishes
  // instead of launching the app.
  PRISM_CHECK(host.ChannelOpen());
  PRISM_CHECK(host.ChannelOwner());
  PRISM_CHECK(companion::CompanionPreviewPresent());

  // (a) No wake -> not visible, even after the idle pump ran.
  PRISM_CHECK(!host.Visible());
  host.Pump();
  ::Sleep(60);
  host.Pump();
  PRISM_CHECK(!host.Visible());
  PRISM_CHECK_EQ(host.ShownEntries(), size_t(0));

  // (b) The shell publishes. The writer runs on its own thread because the ack
  // is a handshake: the host must answer WHILE the shell is waiting.
  const POINT click{300, 200};
  companion::PreviewPacket packet =
      companion::MakeShellPacket(World().archiveOld, click);
  const HWND foregroundBefore = ::GetForegroundWindow();
  std::atomic<bool> writerDone{false};
  companion::PublishResult result = companion::PublishResult::NotOpen;
  std::thread writer([&] {
    result = companion::TryPublishPreview(packet, 5000);  // bounded ack budget
    writerDone.store(true);
  });
  for (int attempt = 0; attempt < 600 && !writerDone.load(); ++attempt) {
    host.Pump();
    ::Sleep(5);
  }
  writer.join();
  // Acknowledged == the host read the packet and acked inside the shell's budget.
  PRISM_CHECK(result == companion::PublishResult::Acknowledged);
  PRISM_CHECK_EQ(host.Wakes(), size_t(1));
  PRISM_CHECK_EQ(host.Handled(), size_t(1));

  // (c) Visible now, positioned from the click, and NOT activated: the contract
  // is "peek", so the window that had focus still has it.
  PRISM_CHECK(host.Visible());
  PRISM_CHECK(host.ShownPath() == World().archiveOld);
  PRISM_CHECK_EQ(host.ShownEntries(), static_cast<size_t>(companion::kMaxPreviewEntries));
  PRISM_CHECK_EQ(host.Window().EntryCount(), static_cast<size_t>(companion::kMaxPreviewEntries));
  PRISM_CHECK(!host.Window().NeedsCatalogScan());  // the host DID enumerate
  PRISM_CHECK(::GetForegroundWindow() == foregroundBefore);
  PRISM_CHECK(::GetActiveWindow() != host.Window().Hwnd());
  // The flyout landed INSIDE the monitor's work area (C30's placement contract)
  // and next to the click, whichever side it had to flip to.
  RECT work{};
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (::GetMonitorInfoW(::MonitorFromPoint(click, MONITOR_DEFAULTTONEAREST), &info)) {
    work = info.rcWork;
  }
  RECT flyout{};
  PRISM_CHECK(::GetWindowRect(host.Window().Hwnd(), &flyout) != FALSE);
  PRISM_CHECK(flyout.left >= work.left);
  PRISM_CHECK(flyout.top >= work.top);
  PRISM_CHECK(flyout.right <= work.right);
  PRISM_CHECK(flyout.bottom <= work.bottom);
  PRISM_CHECK(flyout.left != click.x);  // offset from the click, not on top of it
  std::cout << "  [flyout] rect=" << flyout.left << "," << flyout.top << " rows="
            << host.Window().EntryCount() << std::endl;

  // (d) ESC closes the peek (C30's window policy, through the REAL HWND).
  ::SendMessageW(host.Window().Hwnd(), WM_KEYDOWN, VK_ESCAPE, 0);
  PRISM_CHECK(!host.Visible());

  // (e) A wake for an archive the app does NOT have open, with no scanner
  // configured: the flyout SAYS it needs a scan instead of inventing rows.
  companion::PreviewPacket foreign =
      companion::MakeShellPacket(World().archiveNew, POINT{320, 240});
  PRISM_CHECK(companion::TryPublishPreview(foreign, 0) == companion::PublishResult::Delivered);
  host.Pump();
  PRISM_CHECK(host.ShownPath() == World().archiveNew);
  PRISM_CHECK_EQ(host.ShownEntries(), size_t(0));
  PRISM_CHECK(host.Window().NeedsCatalogScan());

  host.Stop();
  // Stopping releases the kernel objects: the shell is back to its fast path.
  PRISM_CHECK(!companion::CompanionPreviewPresent());
  PRISM_CHECK(!host.ChannelOpen());
}

// ---------------------------------------------------------------------------
// (2) The minimise path gives the glass layer and the thumbnails back.
// ---------------------------------------------------------------------------
PRISM_TEST("glass: minimise releases the transient layer, restore rebuilds it") {
  std::vector<ArchiveItem> items = World().catalogOld;
  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive C34 glass", ::GetModuleHandleW(nullptr))) {
    std::cout << "  [skip] CreateWindowExW failed err=" << ::GetLastError() << std::endl;
    g_noWindowStation = true;
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"old.7z");
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 800));

  if (!window.EnableGlassLayer()) {
    std::cout << "  [skip] DirectComposition unavailable on this window station" << std::endl;
    g_noComposition = true;
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  PRISM_CHECK(window.GlassLayerReady());
  PRISM_CHECK(window.GlassLayerVisible());
  const size_t resident = window.GlassTransientBytes();
  PRISM_CHECK(resident > 0);
  PRISM_CHECK(window.GlassTransientLive());
  std::cout << "  [glass] transient before minimise: " << resident << " bytes" << std::endl;

  // A thumbnail store with real pixels: the idle trim must move resident DOWN,
  // and the target is an eighth of the budget (PERF.md 2.1).
  auto store = std::make_shared<prism::gallery::ThumbStore>(1024u * 1024u);
  for (uint32_t i = 0; i < 64; ++i) PRISM_CHECK(store->OnThumb(i, MakeThumb(32, 32)));
  const size_t thumbsBefore = store->SizeBytes();
  PRISM_CHECK(thumbsBefore > store->BudgetBytes() / 8);
  window.SetThumbs(store);

  window.Dispatch(WM_SIZE, SIZE_MINIMIZED, 0);
  // C33's accounting: derived from live COM pointers, so a "release" that only
  // forgot its own counter cannot pass.
  PRISM_CHECK_EQ(window.GlassTransientBytes(), size_t(0));
  PRISM_CHECK(!window.GlassTransientLive());
  PRISM_CHECK_EQ(window.GlassReleasedBytes(), resident);
  // The idle trim really returned pixels.
  PRISM_CHECK(store->SizeBytes() < thumbsBefore);
  PRISM_CHECK(window.LastThumbTrimBytes() > 0);
  std::cout << "  [glass] released " << window.GlassReleasedBytes() << " bytes, thumbs "
            << thumbsBefore << " -> " << store->SizeBytes() << std::endl;

  // Releasing twice reports nothing the second time: the number is accounting,
  // not a counter (which is what makes the negative control trustworthy).
  window.Dispatch(WM_SIZE, SIZE_MINIMIZED, 0);
  PRISM_CHECK_EQ(window.GlassReleasedBytes(), size_t(0));

  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 800));
  PRISM_CHECK_EQ(window.GlassTransientBytes(), resident);
  PRISM_CHECK(window.GlassTransientLive());
  PRISM_CHECK(window.GlassLayerVisible());

  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// (3) A finished re-pack becomes the archive the session shows.
// ---------------------------------------------------------------------------
PRISM_TEST("compress: the new archive is opened, leased and shown") {
  Fixture& fx = World();
  // Work on a COPY so deleting a file the test owns cannot disturb the fixture
  // (the delete is the proof that the old pool let go).
  const fs::path copyPath = fs::path(fx.root) / "session.7z";
  std::error_code ec;
  fs::copy_file(fs::path(fx.archiveOld), copyPath, fs::copy_options::overwrite_existing, ec);
  if (!PRISM_CHECK(!ec)) return;
  const std::wstring sessionPath = copyPath.wstring();

  app::OperationHandles::Config config;
  config.archivePath = sessionPath;
  config.handlerSlots = 2;
  std::string poolError;
  std::shared_ptr<app::OperationHandles> ops = app::OperationHandles::Create(fx.library, config, &poolError);
  if (!PRISM_CHECK(ops != nullptr)) {
    std::cout << "    pool: " << poolError << std::endl;
    return;
  }
  std::vector<ArchiveItem> catalog = fx.catalogOld;
  std::vector<ArchiveItem> items = catalog;
  std::wstring archivePath = sessionPath;
  app::ArchiveSession session{&fx.library, &ops, &catalog, &items, &archivePath, 2};
  const size_t before = items.size();
  PRISM_CHECK_EQ(before, static_cast<size_t>(kOldEntryCount));

  // The view the app refreshes: a real window over the OLD catalogue.
  prism::win::gallery::GalleryWindow window;
  const bool haveWindow =
      window.Create(L"PrismArchive C34 switch", ::GetModuleHandleW(nullptr));
  if (haveWindow) {
    window.SetItems(&items);
    window.SetRootLabel(L"session.7z");
    window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 800));
    PRISM_CHECK_EQ(window.ItemCount(), static_cast<int>(before));
  }

  const app::ArchiveSwitch switched = app::SwitchArchive(session, fx.archiveNew);
  if (!PRISM_CHECK(switched.ok)) {
    std::cout << "    switch: " << switched.error << std::endl;
    ops->Close();
    return;
  }
  PRISM_CHECK(switched.switched);
  PRISM_CHECK(switched.poolRebuilt);
  // THE CARD'S CONTRACT: the current archive path AND the entry count are the
  // new archive's.
  PRISM_CHECK(archivePath == fx.archiveNew);
  PRISM_CHECK_EQ(items.size(), static_cast<size_t>(3));
  PRISM_CHECK(items.size() != before);
  PRISM_CHECK(catalog.size() == items.size());
  for (size_t i = 0; i < items.size(); ++i) {
    PRISM_CHECK(items[i].path == fx.catalogNew[i].path);
  }

  // The old pool really let go of the old file: an open handle would keep this
  // DeleteFileW from succeeding (Windows refuses to delete an open file).
  PRISM_CHECK(::DeleteFileW(sessionPath.c_str()) != FALSE);

  // ... and the NEW pool serves the new archive (a lease + a real dispatch).
  std::string why;
  std::optional<app::OperationHandles::Task> task = ops->Begin(
      app::OperationHandles::Kind::kExtract, app::OperationHandles::Priority::kUser,
      "c34:lease-new", "test", &why);
  PRISM_CHECK(task.has_value());
  if (task.has_value()) ops->Finish(*task, true);
  PRISM_CHECK_EQ(ops->poolStats().capacity, size_t(2));

  // The view refresh the handler performs.
  if (haveWindow) {
    window.SetItems(&items);
    window.SetRootLabel(L"new.7z");
    window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1280, 800));
    PRISM_CHECK_EQ(window.ItemCount(), 3);
    PRISM_CHECK(window.BreadcrumbText().find(L"new.7z") != std::wstring::npos);
  }

  // A switch that cannot open the target changes NOTHING (the session keeps
  // working: this is why the new pool is leased before the old one closes).
  const app::ArchiveSwitch missing =
      app::SwitchArchive(session, (fs::path(fx.root) / "not-here.7z").wstring());
  PRISM_CHECK(!missing.ok);
  PRISM_CHECK(!missing.error.empty());
  PRISM_CHECK(archivePath == fx.archiveNew);
  PRISM_CHECK_EQ(items.size(), static_cast<size_t>(3));

  ops->Close();
  if (haveWindow) window.Dispatch(WM_DESTROY, 0, 0);
}

int main(int argc, char** argv) {
  if (kMutation != nullptr) {
    std::cout << "C34 MUTANT ACTIVE: " << kMutation << std::endl;
  }
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)" << std::endl;
    return 77;
  }
  if (!SetupFixture()) {
    const std::string why = World().why;
    ::CoUninitialize();
    if (why == "7z.dll/7z.exe not available") {
      std::cout << "SKIP: 7z.dll/7z.exe not available -- set PRISM_7Z_DLL to a 7z.dll that has "
                   "7z.exe beside it"
                << std::endl;
      return 77;
    }
    std::cout << "FAIL: fixture setup: " << why << std::endl;
    return 1;
  }
  const int rc = prism::test::RunAll(argc, argv);
  ::CoUninitialize();

  // CONTRACTS D26: the mutant's verdict is a CONJUNCTION printed by the suite
  // itself, never WILL_FAIL (which cannot be combined with a pass marker and
  // would turn a skip into a green).
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::cout << "C34 MUTATION NOT CAUGHT: the suite stayed green with the regression compiled "
                   "in -- the injection is stale"
                << std::endl;
      return 1;
    }
    std::cout << "C34 MUTATION CAUGHT: " << kMutation
              << " (suite red, banner above; see the FAIL lines for the contract it broke)"
              << std::endl;
    return 0;
  }

  if (rc != 0) return rc;
  if (g_foreignCompanion || g_noWindowStation || g_noComposition) {
    std::cout << "C34 SKIP: this machine cannot exercise the wiring end to end (window="
              << (g_noWindowStation ? "no" : "yes")
              << " composition=" << (g_noComposition ? "no" : "yes")
              << " section_free=" << (g_foreignCompanion ? "no" : "yes") << ")" << std::endl;
    return 77;
  }
  std::cout << "ALL HOST-WIRING CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  return 0;
}
