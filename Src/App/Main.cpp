// PrismArchive -- the runnable application (batch A assembly).
//
// This is the entry point the project never had: every part below was already
// proven in isolation (engine bridge against real 7z.dll, single-pass solid
// sweep with pixel-verified thumbnails, renderer validated by CI snapshots);
// this file is deliberately boring glue, because assembly bugs live in
// ordering and lifetime, not in cleverness.
//
// Assembly contract (batch A):
//   1. COM apartment-threaded for the UI thread; the workers use MTA.
//   2. Archive path: argv[1], else a common dialog. No archive -> quiet exit.
//   3. 7z.dll resolution lives in PrismEngineBridge::Resolve7zDll (env ->
//      app dir -> 7-Zip's own registry InstallDir -> the two Program Files
//      probes). Not finding it is a diagnosable failure, not a silent one: the
//      message box lists every path that was tried.
//   4. Catalog loads on the UI thread (v1: one seek+header read; directories
//      load in milliseconds per the blueprint's zero-payload scan).
//   5. Thumbnails: ONE background sweep over all image entries (single-pass
//      per solid block) into a worker-owned ThumbStore, handed to the window
//      by pointer swap; the window repaints via kThumbsChanged. The app
//      context is kept alive by shared_ptr on both sides.
//   6. Folder navigation (increment 5) is owned by the window's
//      prism::tree::Navigator: this file only sets the catalog down and names
//      the root crumb. The sweep stays ARCHIVE-WIDE on purpose (see below).
//
// Thumbnail scope after navigation -- the decision, stated once:
//   The solid sweep is a whole-archive, single-pass job (7z hands out solid
//   blocks in one decode stream; re-running it per folder would re-decode the
//   same dictionary once per folder). Its store is keyed by CATALOG index, and
//   the navigator's per-folder rows carry those same indices, so entering a
//   folder is a re-filter, never a rescan, and a thumbnail decoded before the
//   switch is still a hit after it. The only thing that changes on navigation
//   is which rows the renderer asks about.
//
// Shutdown contract (card C1: app lifecycle, 2026-11-01):
//   close window -> WM_CLOSE -> WM_DESTROY -> PostQuitMessage -> Run() returns
//   -> the scan is CANCELLED (ArchiveReader::RequestBatchAbort, which makes the
//   sweep's extract callback answer E_ABORT and lets 7-Zip stop inside its own
//   decode loop) -> every worker is JOINED -> the window object leaves scope ->
//   the process exits with code 0. No detach anywhere: the window is a stack
//   object and the workers talk to it through Post* only, so a join is what
//   makes that contract sound.
//   The measured failure this replaces: closing the window left the process
//   alive forever. Root cause was not the close path at all -- the gallery
//   renderer was never Initialize()d, so the first WM_PAINT dereferenced a null
//   DirectWrite factory, the UI thread took an access violation inside
//   DispatchMessageW, and the process sat in that fault (with the window
//   already gone) instead of unwinding to GetMessageW and seeing WM_QUIT. The
//   fix is in GalleryWindow::Create (it now initializes the renderer before the
//   first paint) plus this file's cancel-then-join shutdown and a lifecycle
//   test that drives a REAL window and a REAL WM_CLOSE
//   (Tests/win/test_app_lifecycle.cpp).
//
// History note: the inc-2 edit of this file was applied by an index-based
// patch that cut at the wrong offset and left the previous handler's tail
// dangling below the new one (main CI red twice). Rewritten as one piece;
// lesson recorded in docs/CONTRACTS.md (D7: no index-surgery on whole files).

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
#include <cstdio>      // fprintf (extraction telemetry)
#include <filesystem>  // parent_path for the default output dir
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../PrismCore/Preview.hpp"        // QuickLookPlanner::KindByName
#include "../PrismCore/SmartExtractor.hpp" // SmartExtractionPlanner
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismWin/GalleryWindow.hpp"
#include "../PrismWin/PrismEngineBridge.hpp"  // Resolve7zDll, WideToUtf8, SmartExtractCallback
#include "../PrismWin/SolidSweeper.hpp"

#pragma comment(lib, "advapi32.lib")  // RegOpenKeyExW in Resolve7zDll
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

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

std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                       static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(wide), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), wide);
  return out;
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
// threading contract: `window` is a stack object in wWinMain, destroyed after
// Run() returns. A worker must therefore never hold a reference to it across a
// window close -- that is why the extract worker is joinable and why it talks
// to the window only through the POSTING helpers (PostHudStart/PostStatus/
// PostProgress), which are no-ops once the HWND is gone.
struct AppContext {
  prism::win::SevenZipLibrary library;
  std::unique_ptr<prism::win::ArchiveReader> reader;
  std::vector<prism::ArchiveItem> items;    // full catalog: what the window navigates
  std::vector<prism::ArchiveItem> catalog;  // full catalog: what extraction plans over
  std::atomic<bool> extracting{false};
  // Set by a worker that can no longer finish. Read after join: if it is true
  // the archive may be half-written and the READER must not be used again.
  std::atomic<bool> shuttingDown{false};
  std::wstring archivePath;
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  // B7 (preflight): every GalleryWindow:: static below needs this alias in the
  // TU -- the class lives in prism::win::gallery and Main.cpp has no local
  // MSVC compile to catch a bare unqualified reference.
  using GalleryWindow = prism::win::gallery::GalleryWindow;
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  std::wstring archive = (argv && argc > 1) ? argv[1] : std::wstring();
  if (argv) LocalFree(argv);
  if (archive.empty()) archive = PickArchive();
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
  app->reader = std::make_unique<prism::win::ArchiveReader>(app->library);
  if (!app->reader->Open(archive)) {
    const std::wstring trace(app->reader->openTrace().begin(),
                             app->reader->openTrace().end());
    MessageBoxW(nullptr, (L"Failed to open:\n" + archive + L"\n\n" + trace).c_str(),
                L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  app->catalog = app->reader->ReadCatalog();
  app->items = app->catalog;

  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive -- " + FileNameWide(archive), instance)) {
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  // The window's navigator owns the current folder from here on: the catalog
  // goes in once, and every later question ("which rows?", "which crumbs?",
  // "which level did that click hit?") is answered by PrismCore.
  window.SetItems(&app->items);
  window.SetRootLabel(FileNameWide(archive));

  // Double-click on a FILE = smart extract (Bandizip's core interaction; the
  // folder case never reaches this handler -- the navigator drills in first).
  // The handler receives the CATALOG index of the row that was activated.
  // v1 keeps the batch-A semantics: the plan covers the whole archive, so the
  // index is deliberately unused here -- it is the seam for "extract just this
  // one" later without touching the window again.
  //
  // THREADING: this handler runs on the UI thread but the work is long, so it
  // hands off to a JOINABLE worker stored in `extractWorker`. It used to be a
  // detached thread capturing &window by reference: the widget is a stack
  // object destroyed right after Run(), so closing the app mid-extraction left
  // that thread writing into freed memory -- and it also raced on hud_ (which
  // the window feeds from WndProc "single-threaded by rule"). Both are fixed by
  // (a) the worker reaching the window only through Post* helpers, and (b) the
  // join below, which is what makes the window's lifetime outlast the worker.
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
      namespace fs = std::filesystem;
      const std::string outDir = prism::win::WideToUtf8(
          fs::path(app->archivePath).parent_path().wstring());
      const auto plan = prism::SmartExtractionPlanner::Build(
          prism::win::WideToUtf8(app->archivePath), outDir, app->catalog,
          [](const std::string&) { return false; });
      prism::win::SmartExtractCallback::Result result;
      const bool ok = app->reader->ExtractWithPlan(app->catalog, plan,
                                                   plan.baseTargetDir, nullptr,
                                                   &result);
      // Posted, not called: hud_ belongs to the UI thread.
      window.PostProgress(result.filesWritten, app->catalog.size());
      // Never hold a c_str() of a temporary: build the wide string first.
      std::wstring done = ok ? std::wstring(L"\x5DF2\x89E3\x538B")
                             : std::wstring(L"\x89E3\x538B\x5931\x8D25");
      if (ok && !plan.baseTargetDir.empty()) {
        done += L" \x2192 " + Utf8ToWide(plan.baseTargetDir);
      }
      // A status line, NOT a crumb: only the crumbs navigate (SetBreadcrumb is
      // kept as an alias for the batch-A call sites and the API contract).
      // Posted for the same reason as the progress above: status_ and the
      // breadcrumb layout it rebuilds are UI-thread state.
      window.PostStatus(done);
      std::fprintf(stderr, "extract ok=%d files=%llu bytes=%llu in %llums\n",
                   static_cast<int>(ok),
                   static_cast<unsigned long long>(result.filesWritten),
                   static_cast<unsigned long long>(result.bytesWritten),
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
  // THREADING: the sweep reads the same ArchiveReader that extraction uses, and
  // 7z.dll handlers are not thread-safe -- 7-Zip's own guidance is one handler
  // per concurrent operation. Both workers are joined before the app exits, so
  // the second statement below is about a REAL interleaving: the sweep is still
  // running (a large archive takes seconds) when the user double-clicks a file.
  // Sharing one reader across them is an open defect, recorded as debt in
  // docs/PROGRESS.md rather than hidden behind a "works on my archive" run;
  // fixing it needs a second handle/reader, which is a real chunk of work.
  std::thread worker([app, &window, sweepIndices, sweepSizes]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    prism::win::sweep::WicThumbDecoder wic;
    if (wic.Initialize() && !sweepIndices.empty()) {
      auto store = std::make_shared<prism::gallery::ThumbStore>(64ull << 20);
      prism::win::sweep::RunSolidSweep(*app->reader, sweepIndices, sweepSizes,
                                       wic, *store);
      window.SetThumbs(store);
      PostMessageW(window.Hwnd(), GalleryWindow::kThumbsChanged, 0, 0);
    }
    CoUninitialize();
  });

  window.Run();

  // The window is gone: cancel the thumbnail scan BEFORE joining it. The sweep
  // is one whole-archive Extract inside 7-Zip; without this it would run to
  // completion (seconds on a large archive) with no window left to hand the
  // result to, and closing the app would look like a hang. RequestBatchAbort
  // makes the extract callback answer E_ABORT, 7-Zip stops the decode, and the
  // join below returns in milliseconds.
  app->reader->RequestBatchAbort();

  // Join EVERY worker before `window` leaves scope: it is a stack object and the
  // workers talk to it. Joining is what makes the posting contract sound -- a
  // Post* that lands after WM_DESTROY simply fails and frees its payload.
  // Never detach: a detached worker here would outlive `window` and `app`.
  worker.join();
  if (extractWorker.joinable()) extractWorker.join();

  if (SUCCEEDED(coInit)) CoUninitialize();
  return 0;
}
