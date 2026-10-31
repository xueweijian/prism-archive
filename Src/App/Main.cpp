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
//   3. 7z.dll resolution: PRISM_7Z_DLL env, then the standard install paths.
//   4. Catalog loads on the UI thread (v1: one seek+header read; directories
//      load in milliseconds per the blueprint's zero-payload scan).
//   5. Thumbnails: ONE background sweep over all image entries (single-pass
//      per solid block) into a worker-owned ThumbStore, handed to the window
//      by pointer swap; the window repaints via kThumbsChanged. The app
//      context is kept alive by shared_ptr on both sides; the sweep worker is
//      joined after the message loop exits.
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
#include <shlwapi.h>   // PathFileExistsW

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
#include "../PrismWin/PrismEngineBridge.hpp"  // WideToUtf8, SmartExtractCallback
#include "../PrismWin/SolidSweeper.hpp"

#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace {

std::wstring Locate7zDll() {
  wchar_t buf[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", buf, MAX_PATH) > 0 && PathFileExistsW(buf)) {
    return buf;
  }
  for (const wchar_t* candidate : {L"C:\\Program Files\\7-Zip\\7z.dll",
                                   L"C:\\Program Files (x86)\\7-Zip\\7z.dll"}) {
    if (PathFileExistsW(candidate)) return candidate;
  }
  return {};
}

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

// Everything the UI thread and the workers share. Kept alive by shared_ptr on
// both sides; the sweep worker is joined after the message loop.
struct AppContext {
  prism::win::SevenZipLibrary library;
  std::unique_ptr<prism::win::ArchiveReader> reader;
  std::vector<prism::ArchiveItem> items;
  std::vector<prism::ArchiveItem> catalog;
  std::atomic<bool> extracting{false};
  std::wstring archivePath;
};

}  // namespace

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
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

  const std::wstring dll = Locate7zDll();
  if (dll.empty()) {
    MessageBoxW(nullptr, L"7z.dll not found.\nInstall 7-Zip or set PRISM_7Z_DLL.",
                L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }

  auto app = std::make_shared<AppContext>();
  app->archivePath = archive;
  if (!app->library.Load(dll)) {
    MessageBoxW(nullptr, L"Failed to load 7z.dll.", L"PrismArchive", MB_ICONERROR);
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
  if (!window.Create(L"PrismArchive -- " + FileNameWide(archive), instance)) return 1;
  window.SetItems(&app->items);
  // Double-click = smart extract (Bandizip's core interaction). The worker
  // thread owns the archive handle for the duration; the UI gets progress
  // messages and a breadcrumb update. One extraction at a time.
  window.SetActivateHandler([&window, app](int) {
    if (app->extracting.exchange(true)) return;
    std::thread([app, &window]() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      const uint64_t t0 = GalleryWindow::NowMs();
      window.Hud().OnStart("\xe6\x99\xba\xe8\x83\xbd\xe8\xa7\xa3\xe5\x8e\x8b",
                           GalleryWindow::NowMs());
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
      PostMessageW(window.Hwnd(), GalleryWindow::kProgressMessage,
                   static_cast<WPARAM>(result.filesWritten),
                   static_cast<LPARAM>(app->catalog.size()));
      // Never hold a c_str() of a temporary: build the wide string first.
      std::wstring done = ok ? std::wstring(L"\x5DF2\x89E3\x538B")
                             : std::wstring(L"\x89E3\x538B\x5931\x8D25");
      if (ok && !plan.baseTargetDir.empty()) {
        done += L" \x2192 " + Utf8ToWide(plan.baseTargetDir);
      }
      window.SetBreadcrumb(done);
      std::fprintf(stderr, "extract ok=%d files=%llu bytes=%llu in %llums\n",
                   static_cast<int>(ok),
                   static_cast<unsigned long long>(result.filesWritten),
                   static_cast<unsigned long long>(result.bytesWritten),
                   static_cast<unsigned long long>(GalleryWindow::NowMs() - t0));
      app->extracting = false;
      CoUninitialize();
    }).detach();
  });
  window.SetBreadcrumb(FileNameWide(archive) + L"  |  " +
                       std::to_wstring(app->items.size()) + L" items");

  // One background pass: every image entry, single Extract per solid block.
  std::vector<uint32_t> sweepIndices;
  std::vector<uint64_t> sweepSizes;
  for (const auto& item : app->items) {
    if (item.isDir) continue;
    if (prism::QuickLookPlanner::KindByName(item.path) != prism::PreviewKind::Image) continue;
    sweepIndices.push_back(item.index);
    sweepSizes.push_back(item.size);
  }
  std::thread worker([app, &window, sweepIndices, sweepSizes]() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    prism::win::sweep::WicThumbDecoder wic;
    if (wic.Initialize()) {
      auto store = std::make_shared<prism::gallery::ThumbStore>(64ull << 20);
      prism::win::sweep::RunSolidSweep(*app->reader, sweepIndices, sweepSizes,
                                       wic, *store);
      window.SetThumbs(store);
      PostMessageW(window.Hwnd(), GalleryWindow::kThumbsChanged, 0, 0);
    }
    CoUninitialize();
  });

  window.Run();
  worker.join();

  if (SUCCEEDED(coInit)) CoUninitialize();
  return 0;
}
