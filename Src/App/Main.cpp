// PrismArchive -- the runnable application (batch A assembly + card C25 wiring).
//
// This is the entry point the project never had: every part below was already
// proven in isolation (engine bridge against real 7z.dll, single-pass solid
// sweep with pixel-verified thumbnails, renderer validated by CI snapshots);
// this file is deliberately boring glue, because assembly bugs live in
// ordering and lifetime, not in cleverness.
//
// Card C25 (wave 2) changed exactly five things here, and nothing else:
//   1. HANDLERS PER OPERATION (C11). The sweep and the extract used to share one
//      ArchiveReader -- a data race inside 7z.dll and one shared file pointer
//      (docs/PROGRESS known debt). Every operation now leases its own handler
//      from prism::app::OperationHandles, which pairs ExtractScheduler's slot
//      with ReaderPool's lease.
//   2. SECURITY PASS (C5). After a successful extraction, :Zone.Identifier is
//      inherited from the archive onto every written file, and executable/script
//      payloads get an AMSI pre-scan whose Unavailable state is reported, never
//      silently treated as Allow (prism::app::ApplySecurityPass).
//   3. COMMAND LINE (C4). --smart-extract / --extract-here / --extract-sub /
//      --test are the four verbs the shell DLLs launch us with; each one builds
//      its own plan (prism::app::BuildExtractionPlan) and --test runs the
//      verify-only path instead of opening a window.
//   4. KEYS/PALETTE/HISTORY (C8), LIST VIEW (C13) and SIDEBAR (C12) are wired in
//      GalleryWindow (WM_KEYDOWN now goes through PrismCore's key table). This
//      file only has to hand the window the catalog it already owns.
//   5. 7z.dll resolution is untouched: C1's cross-drive probe order stays.
//
// Assembly contract (batch A, still true):
//   1. COM apartment-threaded for the UI thread; the workers use MTA.
//   2. Archive path: argv, else a common dialog. No archive -> quiet exit.
//   3. 7z.dll resolution lives in PrismEngineBridge::Resolve7zDll (env ->
//      app dir -> 7-Zip's own registry InstallDir -> the two Program Files
//      probes). Not finding it is a diagnosable failure, not a silent one: the
//      message box lists every path that was tried.
//   4. Catalog loads on the UI thread (v1: one seek+header read; directories
//      load in milliseconds per the blueprint's zero-payload scan).
//   5. Thumbnails: ONE background sweep over all image entries (single-pass
//      per solid block) into a worker-owned ThumbStore, handed to the window
//      by pointer swap; the window repaints via kThumbsChanged.
//   6. Folder navigation is owned by the window's prism::tree::Navigator.
//
// Shutdown contract (card C1, still true with two handlers):
//   close window -> WM_CLOSE -> WM_DESTROY -> PostQuitMessage -> Run() returns
//   -> every worker's reader is ABORTED (RequestBatchAbort makes the extract
//   callback answer E_ABORT, so 7-Zip stops inside its own decode loop) -> every
//   worker is JOINED -> the window object leaves scope -> exit code 0. No detach
//   anywhere: the window is a stack object and the workers talk to it through
//   Post* only, so a join is what makes that contract sound.
//   With the pool, "the reader to abort" is no longer app->reader: each worker
//   publishes its own handler pointer before it starts decoding, and the UI
//   thread aborts whichever pointers are published. The publication order (store
//   pointer, THEN check the shutdown flag; UI sets the flag, THEN reads the
//   pointer) closes both interleavings without a lock.
//
// History note: the inc-2 edit of this file was applied by an index-based patch
// that cut at the wrong offset and left the previous handler's tail dangling
// below the new one (main CI red twice). Rewritten as one piece; lesson recorded
// in docs/CONTRACTS.md (D7: no index-surgery on whole files). This file was
// rewritten whole again for C25 for the same reason.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>   // GetOpenFileNameW
#include <shellapi.h>  // CommandLineToArgvW

#include <atomic>
#include <cstdio>      // fprintf (extraction / verification telemetry)
#include <filesystem>  // parent_path, exists (the plan's collision probe)
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AppWiring.hpp"
#include "../PrismCore/Preview.hpp"        // QuickLookPlanner::KindByName
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismWin/GalleryWindow.hpp"
#include "../PrismWin/PrismEngineBridge.hpp"  // Resolve7zDll, WideToUtf8
#include "../PrismWin/SolidSweeper.hpp"

#pragma comment(lib, "advapi32.lib")  // RegOpenKeyExW in Resolve7zDll
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

// B7 (preflight): every GalleryWindow:: static below needs this alias in the
// TU -- the class lives in prism::win::gallery and Main.cpp has no local MSVC
// compile to catch a bare unqualified reference.
using GalleryWindow = prism::win::gallery::GalleryWindow;

std::wstring PickArchive() {
  OPENFILENAMEW ofn{};
  wchar_t file[MAX_PATH]{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr;
  ofn.lpstrFilter = L"Archives\0*.zip;*.7z;*.rar\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
  if (GetOpenFileNameW(&ofn)) return file;
  return {};
}

std::wstring FileNameWide(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"/\\");
  return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// "7z.dll not found" is only actionable if it names the paths that were tried
// (D12: an absence has to say what should have been there). Chinese, like the
// rest of the user-facing strings in this app.
std::wstring DescribeMissing7zDll(const prism::win::SevenZipDllLookup& lookup) {
  std::wstring text =
      L"\u672A\u627E\u5230 7z.dll\uFF0CPrismArchive \u9700\u8981 7-Zip "
      L"\u5F15\u64CE\u624D\u80FD\u6253\u5F00\u5F52\u6863\u3002\n\n"
      L"\u5DF2\u4F9D\u6B21\u5C1D\u8BD5\u4EE5\u4E0B\u8DEF\u5F84\uFF1A\n";
  if (lookup.candidates.empty()) {
    text += L"  (\u65E0\uFF1A\u73AF\u5883\u53D8\u91CF PRISM_7Z_DLL "
            L"\u672A\u8BBE\u7F6E\uFF0C\u4E14\u6CE8\u518C\u8868\u4E0E"
            L"\u9ED8\u8BA4\u5B89\u88C5\u76EE\u5F55\u5747\u65E0 7z.dll)\n";
  }
  for (const std::wstring& candidate : lookup.candidates) {
    text += L"  " + candidate + L"\n";
  }
  text +=
      L"\n\u4FEE\u590D\u65B9\u6CD5\uFF08\u4EFB\u9009\u4E00\uFF09\uFF1A\n"
      L"  1. \u5B89\u88C5 7-Zip\uFF08https://www.7-zip.org/\uFF09\uFF1B\n"
      L"  2. \u8BBE\u7F6E\u73AF\u5883\u53D8\u91CF PRISM_7Z_DLL "
      L"\u6307\u5411 7z.dll \u7684\u5B8C\u6574\u8DEF\u5F84\uFF1B\n"
      L"  3. \u628A 7z.dll \u653E\u5230\u672C\u7A0B\u5E8F\u540C\u76EE"
      L"\u5F55\u3002\n\n";
  const std::wstring appDir = prism::win::ExecutableDirectory();
  if (!appDir.empty()) text += L"\u7A0B\u5E8F\u76EE\u5F55\uFF1A" + appDir + L"\n";
  return text;
}

// Everything the UI thread and the workers share. Kept alive by shared_ptr on
// both sides; every worker is JOINED after the message loop.
//
// threading contract: "window" is a stack object in wWinMain, destroyed after
// Run() returns. A worker must therefore never hold a reference to it across a
// window close -- that is why every worker is joinable and why it talks to the
// window only through the POSTING helpers (PostHudStart/PostStatus/
// PostProgress), which are no-ops once the HWND is gone.
//
// The two "*Reader" slots are the C25 publication points described in the
// shutdown contract above: a worker stores the handler it is about to decode on
// and THEN checks "shuttingDown"; the UI thread sets "shuttingDown" and THEN
// reads the pointers. Neither order can miss the other side.
struct AppContext {
  prism::win::SevenZipLibrary library;
  std::shared_ptr<prism::app::OperationHandles> ops;
  std::vector<prism::ArchiveItem> items;    // full catalog: what the window navigates
  std::vector<prism::ArchiveItem> catalog;  // full catalog: what plans are built over
  std::atomic<bool> extracting{false};
  std::atomic<bool> shuttingDown{false};
  std::atomic<prism::win::ArchiveReader*> sweepReader{nullptr};
  std::atomic<prism::win::ArchiveReader*> extractReader{nullptr};
  std::wstring archivePath;
  prism::app::ExtractionPlan plan;
};

// A context-menu verb we do not implement yet must SAY so. Falling back to
// "open the gallery" would look like the command worked (and, for --compress-*,
// would silently do nothing to the user's archive) -- see TODO(C9) in
// AppWiring.hpp.
void ReportRefusedVerb(const prism::app::LaunchRequest& request) {
  std::wstring text =
      L"\u672C\u7248\u672C\u8FD8\u4E0D\u652F\u6301\u8FD9\u4E2A\u547D\u4EE4\uFF1A\n\n  ";
  text += request.unknownText.empty() ? std::wstring(L"(unknown)") : request.unknownText;
  text +=
      L"\n\n\u5DF2\u652F\u6301\uFF1A--smart-extract / --extract-here / --extract-sub / --test";
  MessageBoxW(nullptr, text.c_str(), L"PrismArchive", MB_ICONWARNING);
}

// One line of machine-readable telemetry. A GUI app has no console, so this is
// diagnostic only; the exit code carries the verdict.
void Telemetry(const char* what, const prism::app::ExtractionOutcome& outcome) {
  std::fprintf(stderr, "%s ok=%d files=%llu bytes=%llu stamped=%llu amsiBlocked=%llu\n", what,
               static_cast<int>(outcome.ok),
               static_cast<unsigned long long>(outcome.filesWritten),
               static_cast<unsigned long long>(outcome.bytesWritten),
               static_cast<unsigned long long>(outcome.security.filesStamped),
               static_cast<unsigned long long>(outcome.security.amsiBlocked));
}

// Headless path for the three extracting verbs and for --test. Returns the
// process exit code: 0 on success, 1 on a diagnosable failure (with a message
// box), 2 when the verb needs an archive it was not given.
int RunHeadless(const prism::app::LaunchRequest& request, AppContext& app) {
  if (request.mode == prism::app::LaunchMode::kTest) {
    const prism::app::VerifyOutcome outcome =
        prism::app::VerifyIntegrity(*app.ops, app.catalog, app.archivePath);
    std::fprintf(stderr, "verify ok=%d items=%llu bytes=%llu failedIndex=%d %s\n",
                 static_cast<int>(outcome.ok),
                 static_cast<unsigned long long>(outcome.items),
                 static_cast<unsigned long long>(outcome.bytes),
                 static_cast<int>(outcome.failedIndex), outcome.error.c_str());
    if (!outcome.ok) {
      const std::wstring text =
          L"\u6821\u9A8C\u5931\u8D25\uFF1A\n" + prism::win::Utf8ToWide(outcome.error);
      MessageBoxW(nullptr, text.c_str(), L"PrismArchive", MB_ICONERROR);
      return 1;
    }
    return 0;
  }

  const prism::app::ExtractionOutcome outcome = prism::app::RunExtraction(
      *app.ops, app.catalog, app.plan, app.archivePath, /*scanExecutables=*/true);
  Telemetry("extract", outcome);
  if (!outcome.ok) {
    std::wstring text = L"\u89E3\u538B\u5931\u8D25\n\n";
    text += outcome.error.empty()
                ? std::wstring(L"(\u5F15\u64CE\u672A\u62A5\u544A\u539F\u56E0)")
                : prism::win::Utf8ToWide(outcome.error);
    MessageBoxW(nullptr, text.c_str(), L"PrismArchive", MB_ICONERROR);
    return 1;
  }
  // A security finding is not a silent success: the user gets told that some
  // payload could not be scanned or was flagged (C5's Unavailable contract).
  if (outcome.security.amsiBlocked > 0 || outcome.security.amsiCouldNotScan > 0) {
    MessageBoxW(nullptr, outcome.status.c_str(), L"PrismArchive", MB_ICONWARNING);
  }
  return 0;
}

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::vector<std::wstring> args;
  if (argv) {
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);
    LocalFree(argv);
  }
  // C4 contract: the four flags the shell DLLs launch us with are parsed from
  // the SAME constants the registrar renders, so the two halves cannot drift.
  prism::app::LaunchRequest launch = prism::app::ParseLaunch(args);
  if (launch.unknownFlag) {
    ReportRefusedVerb(launch);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 2;
  }
  std::wstring archive = launch.archive;
  if (archive.empty()) {
    // A verb without a path is a caller bug, not "open the picker".
    if (launch.mode != prism::app::LaunchMode::kGallery) {
      MessageBoxW(nullptr, L"\u547D\u4EE4\u7F3A\u5C11\u5F52\u6863\u8DEF\u5F84\u3002",
                  L"PrismArchive", MB_ICONERROR);
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 2;
    }
    archive = PickArchive();
  }
  if (archive.empty()) {
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 0;  // user closed the picker: not an error
  }

  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  if (lookup.path.empty()) {
    const std::wstring message = DescribeMissing7zDll(lookup);
    MessageBoxW(nullptr, message.c_str(), L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  const std::wstring& dll = lookup.path;

  auto app = std::make_shared<AppContext>();
  app->archivePath = archive;
  if (!app->library.Load(dll)) {
    MessageBoxW(nullptr, (L"7z.dll \u52A0\u8F7D\u5931\u8D25\uFF1A\n" + dll).c_str(),
                L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }

  // The catalog is read ONCE, on the UI thread, from a probe handler that is
  // released before the pool opens its own (one seek+header read, blueprint's
  // zero-payload scan). The pool's handlers are the ones that decode.
  {
    prism::win::ArchiveReader probe(app->library);
    if (!probe.Open(archive)) {
      const std::wstring trace(probe.openTrace().begin(), probe.openTrace().end());
      MessageBoxW(nullptr, (L"Failed to open:\n" + archive + L"\n\n" + trace).c_str(),
                  L"PrismArchive", MB_ICONERROR);
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 1;
    }
    app->catalog = probe.ReadCatalog();
  }
  app->items = app->catalog;

  // Two handlers: the solid sweep and the user's extraction are the two
  // operations that can genuinely overlap (large archive + double-click).
  prism::app::OperationHandles::Config opsConfig;
  opsConfig.archivePath = archive;
  opsConfig.handlerSlots = 2;
  std::string opsError;
  app->ops = prism::app::OperationHandles::Create(app->library, opsConfig, &opsError);
  if (!app->ops) {
    const std::wstring text =
        L"\u6253\u5F00\u5F15\u64CE\u53E5\u67C4\u5931\u8D25\uFF1A\n" +
        prism::win::Utf8ToWide(opsError);
    MessageBoxW(nullptr, text.c_str(), L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }

  // One plan for the whole session: the verb chose it, and the gallery's
  // double-click uses the smart plan -- the SAME builder, so a fix lands twice.
  app->plan = prism::app::BuildExtractionPlan(
      launch.mode, prism::win::WideToUtf8(archive), app->catalog,
      [](const std::string& path) {
        return std::filesystem::exists(prism::win::Utf8ToWide(path));
      });

  if (launch.mode != prism::app::LaunchMode::kGallery) {
    const int code = RunHeadless(launch, *app);
    app->ops->Close();
    if (SUCCEEDED(coInit)) CoUninitialize();
    return code;
  }

  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive -- " + FileNameWide(archive), instance)) {
    app->ops->Close();
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  // The window's navigator owns the current folder from here on: the catalog
  // goes in once, and every later question ("which rows?", "which crumbs?",
  // "which level did that click hit?") is answered by PrismCore.
  window.SetItems(&app->items);
  window.SetRootLabel(FileNameWide(archive));
  // C12: the sidebar's telemetry pill needs the engine facts the catalog cannot
  // derive. v1 knows the encryption bit from the catalog itself; the method and
  // dictionary names have no accessor in the bridge yet (kept honest, not faked).
  {
    prism::sidebar::ArchiveInfo info;
    for (const prism::ArchiveItem& item : app->catalog) {
      if (item.encrypted) info.encryption = prism::sidebar::Encryption::Unknown;
    }
    window.SetArchiveInfo(info);
  }

  // Double-click on a FILE = smart extract (Bandizip's core interaction; the
  // folder case never reaches this handler -- the navigator drills in first).
  // Ctrl+E reaches the same door with a catalog index (C8 wiring in the window).
  //
  // THREADING: this handler runs on the UI thread but the work is long, so it
  // hands off to a JOINABLE worker stored in "extractWorker". The worker reaches
  // the window only through Post* helpers.
  std::thread extractWorker;
  window.SetActivateHandler([&window, app, &extractWorker](int catalogIndex) {
    (void)catalogIndex;  // the whole-archive plan is v1's behaviour; see above
    if (app->extracting.exchange(true)) return;
    // A previous run must be finished before its std::thread is assigned over.
    if (extractWorker.joinable()) extractWorker.join();
    extractWorker = std::thread([app, &window]() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      const uint64_t t0 = GalleryWindow::NowMs();
      window.PostHudStart("\xe6\x99\xba\xe8\x83\xbd\xe8\xa7\xa3\xe5\x8e\x8b");
      prism::app::ExtractionOutcome outcome;
      std::string why;
      auto task = app->ops->Begin(prism::core::sched::Kind::kExtract,
                                  prism::core::sched::Priority::kUser,
                                  "extract:" + prism::win::WideToUtf8(app->archivePath),
                                  "extract", &why);
      if (task.has_value()) {
        app->extractReader.store(task->reader);
        // Published BEFORE the flag check: see the shutdown contract above.
        if (app->shuttingDown.load()) task->reader->RequestBatchAbort();
        prism::win::SmartExtractCallback::Result result;
        const bool ok = task->reader->ExtractWithPlan(
            app->catalog, app->plan.plan, app->plan.plan.baseTargetDir, nullptr, &result);
        app->extractReader.store(nullptr);
        app->ops->Finish(*task, ok);
        outcome.ok = ok;
        outcome.filesWritten = result.filesWritten;
        outcome.bytesWritten = result.bytesWritten;
        if (ok) {
          outcome.security = prism::app::ApplySecurityPass(
              app->archivePath, result.writtenPaths, /*scanExecutables=*/true);
        }
        outcome.status =
            prism::app::detail::BuildStatusText(outcome, ok, app->plan.plan.baseTargetDir);
      } else {
        outcome.status = L"\x89E3\x538B\x88AB\x62D2\x7EDD";
        if (!why.empty()) outcome.status += L" (" + prism::win::Utf8ToWide(why) + L")";
      }
      // Posted, not called: hud_ / status_ belong to the UI thread.
      window.PostProgress(outcome.filesWritten, app->catalog.size());
      window.PostStatus(outcome.status);
      Telemetry("extract", outcome);
      std::fprintf(stderr, "extract wall=%llums\n",
                   static_cast<unsigned long long>(GalleryWindow::NowMs() - t0));
      app->extracting = false;
      CoUninitialize();
    });
  });
  window.SetStatus(L"\x5171 " + std::to_wstring(app->items.size()) + L" \x9879");

  // One background pass: every image entry in the WHOLE archive, a single
  // Extract per solid block. Folder navigation must never re-run this -- the
  // store is keyed by catalog index, which the per-folder rows preserve.
  std::vector<uint32_t> sweepIndices;
  std::vector<uint64_t> sweepSizes;
  for (const auto& item : app->items) {
    if (item.isDir) continue;
    if (prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) continue;
    sweepIndices.push_back(item.index);
    sweepSizes.push_back(item.size);
  }
  // THREADING (C25): the sweep gets its OWN handler from the pool -- 7z.dll
  // handlers are not thread-safe, so the sweep and a double-click extraction now
  // run on two leases instead of racing on one (docs/PROGRESS known debt).
  std::thread worker([app, &window, sweepIndices, sweepSizes]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    prism::win::sweep::WicThumbDecoder wic;
    if (wic.Initialize() && !sweepIndices.empty()) {
      auto store = std::make_shared<prism::gallery::ThumbStore>(64ull << 20);
      std::string why;
      auto task = app->ops->Begin(prism::core::sched::Kind::kSweep,
                                  prism::core::sched::Priority::kBackground,
                                  "sweep:" + prism::win::WideToUtf8(app->archivePath), "sweep",
                                  &why);
      if (task.has_value()) {
        app->sweepReader.store(task->reader);
        if (app->shuttingDown.load()) task->reader->RequestBatchAbort();
        prism::win::sweep::RunSolidSweep(*task->reader, sweepIndices, sweepSizes, wic, *store);
        app->sweepReader.store(nullptr);
        app->ops->Finish(*task, true);
        // SetThumbs takes its own mutex and touches nothing else (documented
        // worker-safe); the repaint is a posted message, never a direct call.
        window.SetThumbs(store);
        PostMessageW(window.Hwnd(), GalleryWindow::kThumbsChanged, 0, 0);
      } else {
        std::fprintf(stderr, "sweep refused: %s\n", why.c_str());
      }
    }
    CoUninitialize();
  });

  window.Run();

  // The window is gone. Stop both operations BEFORE joining: RequestBatchAbort
  // makes the extract callback answer E_ABORT, so 7-Zip stops inside its own
  // decode loop and the joins return in milliseconds instead of "seconds on a
  // large archive" (C1's close-latency measurement depends on this).
  app->shuttingDown.store(true);
  if (auto* reader = app->sweepReader.load()) reader->RequestBatchAbort();
  if (auto* reader = app->extractReader.load()) reader->RequestBatchAbort();
  app->ops->Close();  // no new work; queued tasks are cancelled

  // Join EVERY worker before "window" leaves scope: it is a stack object and the
  // workers talk to it. Joining is what makes the posting contract sound -- a
  // Post* that lands after WM_DESTROY simply fails and frees its payload.
  // Never detach: a detached worker here would outlive "window" and "app".
  worker.join();
  if (extractWorker.joinable()) extractWorker.join();

  if (SUCCEEDED(coInit)) CoUninitialize();
  return 0;
}
