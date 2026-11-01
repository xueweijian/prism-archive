// PrismArchive -- the runnable application (batch A + cards C25/C26/C27).
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
// Card C27 (wave 3) added the last four things the exe was missing, and again
// nothing else:
//   1. COMPRESSION. The window menu's Compress item opens C18's dialog, which
//      builds a C9 CompressionPlan; the accept handler stages the archive's
//      payload into %TEMP% through a leased engine handler (a window shows what
//      is INSIDE an archive, while the writer reads FILES) and hands that to
//      prism::app::RunCompression -> ArchiveWriter::CreateArchive, with progress
//      posted into the HUD. --compress-here <files...> runs the same path with
//      no window, for automation.
//   2. SETTINGS (C14). Startup loads the document and the sidebar port, the
//      window applies the saved size/view/sort/columns, exit writes all of it
//      back through the atomic file layer. A settings failure is logged, never
//      fatal -- a UI that refuses to open because a JSON file is odd is worse
//      than one that starts with defaults.
//   3. LANGUAGE (C19). tables are loaded from <exe>\Assets\Lang (or
//      PRISM_LANG_DIR), the language comes from settings.language.tag (default
//      "auto" -> the system locale), and the menu/status/error text goes
//      through the translator. A missing key is shown verbatim and logged ONCE.
//   4. C26's failure semantics: a handler that answers S_OK with a non-zero
//      opResult (the zstd case: 0 bytes written, "success") now has its reason
//      appended to the status line instead of being reported as success.
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
#include "../PrismCore/I18n.hpp"           // C19: resource tables + language choice
#include "../PrismCore/Log.hpp"            // C16: the one warn line a missing key gets
#include "../PrismCore/Preview.hpp"        // QuickLookPlanner::KindByName
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismWin/CompressDialog.hpp"  // C18: the create dialog (programmatic)
#include "../PrismWin/GalleryWindow.hpp"
#include "../PrismWin/PrismEngineBridge.hpp"  // Resolve7zDll, WideToUtf8
#include "../PrismWin/SolidSweeper.hpp"

#pragma comment(lib, "advapi32.lib")  // RegOpenKeyExW in Resolve7zDll
#pragma comment(lib, "comctl32.lib")  // InitCommonControlsEx (C18's trackbar)
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

// ---------------------------------------------------------------------------
// Language (card C19) and the one place a user-facing string is born.
//
// The app asks the translator for a KEY and gets the sentence back; it never
// switches on a language tag. A key that is not in the table comes back as the
// key itself (C19's contract: ugly but debuggable) and is logged ONCE as a warn,
// which is the only way "we shipped an untranslated string" becomes visible from
// a user's machine instead of from a reviewer's eye.
// ---------------------------------------------------------------------------
// The runtime itself lives in AppWiring.hpp (prism::app::LanguageRuntime) so the
// WINDOWS SUITE can drive the SAME loader and translator the exe uses: a test
// that re-implements the wiring proves nothing about the wiring.
using LanguageRuntime = prism::app::LanguageRuntime;
using Tr = prism::app::Tr;

// Where the tables live: PRISM_LANG_DIR wins (a dev tree or the CI checkout),
// else <exe dir>\Assets\Lang -- the layout "cmake --install" produces.
std::wstring LanguageDirectory() {
  wchar_t overrideDir[MAX_PATH]{};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_LANG_DIR", overrideDir, MAX_PATH);
  if (length > 0 && length < MAX_PATH) return std::wstring(overrideDir, length);
  const std::wstring appDir = prism::win::ExecutableDirectory();
  if (appDir.empty()) return {};
  return appDir + L"\\Assets\\Lang";
}

std::string SystemLocaleUtf8() {
  wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  const int length = GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH);
  return length > 1 ? prism::win::WideToUtf8(std::wstring(locale, static_cast<size_t>(length - 1)))
                    : std::string();
}

// The exe's door into prism::app::LoadLanguageFor: it only decides WHERE the
// tables are (env override, else <exe>\Assets\Lang) and what the system locale
// is. Everything else -- tables, choice, misses, the warn -- is the shared code.
LanguageRuntime LoadLanguage(const prism::settings::SettingsStore& store,
                             prism::log::Logger* logger) {
  return prism::app::LoadLanguageFor(LanguageDirectory(), store, SystemLocaleUtf8(), logger);
}

// (the loader and the translator live in AppWiring.hpp: one implementation for
// the exe and for the Windows suite)

// The "save as" half of the compress dialog (C18 asks the app for a path).
std::wstring PickSaveArchive(const std::wstring& suggested) {
  OPENFILENAMEW ofn{};
  wchar_t file[MAX_PATH]{};
  if (!suggested.empty() && suggested.size() < MAX_PATH) {
    wmemcpy(file, suggested.c_str(), suggested.size());
  }
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = nullptr;
  ofn.lpstrFilter = L"7z archive\0*.7z\0Zip archive\0*.zip\0All files\0*.*\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"7z";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (GetSaveFileNameW(&ofn)) return file;
  return {};
}

// "7z.dll not found" is only actionable if it names the paths that were tried
// (D12: an absence has to say what should have been there). Chinese, like the
// rest of the user-facing strings in this app.
std::wstring DescribeMissing7zDll(const prism::win::SevenZipDllLookup& lookup, const Tr& tr) {
  // C19: the sentence comes from the resource table; the PATHS below it are data
  // and stay verbatim (a translated path is a lie).
  // The table owns the sentence (C19 key); the fallback is the app's own text,
  // so a checkout without Assets/Lang still says something actionable.
  std::wstring text =
      tr.Or("error.engine.missing",
            L"\u672A\u627E\u5230 7z.dll\uFF0CPrismArchive \u9700\u8981 7-Zip "
            L"\u5F15\u64CE\u624D\u80FD\u6253\u5F00\u5F52\u6863\u3002") +
      L"\n\n"
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
void ReportRefusedVerb(const prism::app::LaunchRequest& request, const Tr& tr) {
  std::wstring text =
      tr.Or("error.verb.unsupported",
            L"\u672C\u7248\u672C\u8FD8\u4E0D\u652F\u6301\u8FD9\u4E2A\u547D\u4EE4\uFF1A") +
      L"\n\n  ";
  text += request.unknownText.empty() ? std::wstring(L"(unknown)") : request.unknownText;
  text += L"\n\n" +
          tr.Or("error.verb.supported",
                L"\u5DF2\u652F\u6301\uFF1A--smart-extract / --extract-here / --extract-sub "
                L"/ --test / --compress-here");
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
int RunHeadless(const prism::app::LaunchRequest& request, AppContext& app, const Tr& tr) {
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
          tr.Or("error.verify.failed", L"\u6821\u9A8C\u5931\u8D25\uFF1A") + L"\n" +
          prism::win::Utf8ToWide(outcome.error);
      MessageBoxW(nullptr, text.c_str(), L"PrismArchive", MB_ICONERROR);
      return 1;
    }
    return 0;
  }

  const prism::app::ExtractionOutcome outcome = prism::app::RunExtraction(
      *app.ops, app.catalog, app.plan, app.archivePath, /*scanExecutables=*/true);
  Telemetry("extract", outcome);
  if (!outcome.ok) {
    std::wstring text = tr.Or("progress.failed", L"\u89E3\u538B\u5931\u8D25") + L"\n\n";
    text += outcome.error.empty()
                ? tr.Or("error.engine.noReason",
                        L"(\u5F15\u64CE\u672A\u62A5\u544A\u539F\u56E0)")
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

// --compress-here <files...> (card C27): archive creation without a window.
//
// Exit code is the verdict and stderr is the machine-readable detail -- the
// shape the other verbs already use. NO message box here on purpose: this verb
// exists for automation, and a dialog would hang a CI step that nobody is
// watching.
int RunCompressHere(const prism::app::LaunchRequest& request) {
  const prism::app::CompressionTarget target =
      prism::app::PlanCompression(request.inputs, prism::ArchiveFormat::SevenZip);
  if (!target.ok) {
    std::fprintf(stderr, "compress failed: %s\n", target.error.c_str());
    return 1;
  }
  prism::app::CompressionRequest job;
  job.archivePath = target.archivePath;
  job.sourceRoot = target.sourceRoot;
  job.plan = target.plan;
  uint64_t lastPercent = 1000;
  const prism::app::CompressionOutcome outcome = prism::app::RunCompression(
      job, [&lastPercent](uint64_t done, uint64_t total) {
        const uint64_t percent = total == 0 ? 0 : (done * 100) / total;
        if (percent != lastPercent) {
          lastPercent = percent;
          std::fprintf(stderr, "compress %llu%% (%llu/%llu bytes)\n",
                       static_cast<unsigned long long>(percent),
                       static_cast<unsigned long long>(done),
                       static_cast<unsigned long long>(total));
        }
        return true;
      });
  std::fprintf(stderr,
               "compress ok=%d items=%llu bytes=%llu verified=%llu ms=%lld target=%s error=%s\n",
               static_cast<int>(outcome.ok),
               static_cast<unsigned long long>(outcome.itemsWritten),
               static_cast<unsigned long long>(outcome.bytesWritten),
               static_cast<unsigned long long>(outcome.verifiedItems),
               static_cast<long long>(outcome.elapsedMs),
               prism::win::WideToUtf8(outcome.archivePath).c_str(), outcome.error.c_str());
  return outcome.ok ? 0 : 1;
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

  // ---- persistence + language, BEFORE any user-facing string --------------
  // C14: one load at startup. A corrupt document is backed up by the file layer
  // (never silently overwritten) and the store reports it; a document from a
  // newer build is READ-ONLY, which SaveSettings honours at exit.
  prism::app::SettingsBootstrap settings = prism::app::LoadSettings();
  // C16: the logger exists before anything can fail, so the reason is on stderr
  // instead of only in a message box nobody can copy.
  prism::log::Logger logger(prism::log::Level::kInfo, 512);
  logger.SetSink([](const std::string& line) { std::fprintf(stderr, "%s\n", line.c_str()); });
  if (!settings.loaded) {
    logger.Emit(prism::log::Level::kWarn, "settings", "settings could not be read",
                {{"detail", settings.report.detail},
                 {"path", prism::win::WideToUtf8(settings.paths.file)}});
  } else if (settings.report.outcome != prism::win::settingsfile::LoadOutcome::Loaded) {
    logger.Emit(prism::log::Level::kInfo, "settings",
                std::string("settings ") +
                    prism::win::settingsfile::LoadOutcomeName(settings.report.outcome),
                {{"status", prism::settings::LoadStatusName(settings.report.storeStatus)},
                 {"path", prism::win::WideToUtf8(settings.paths.file)}});
  }

  // C19: the tables are loaded from disk, then the language is chosen from
  // settings.language.tag (default "auto" == follow the system locale).
  LanguageRuntime language = LoadLanguage(settings.store, &logger);
  const Tr tr{&language.translator};

  if (launch.unknownFlag) {
    ReportRefusedVerb(launch, tr);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 2;
  }
  // C9: archive creation has no archive to open -- dispatch it before the
  // open/picker logic, and give it the same 7z.dll resolution as everything else.
  if (launch.mode == prism::app::LaunchMode::kCompressHere) {
    const int code = RunCompressHere(launch);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return code;
  }
  std::wstring archive = launch.archive;
  if (archive.empty()) {
    // A verb without a path is a caller bug, not "open the picker".
    if (launch.mode != prism::app::LaunchMode::kGallery) {
      MessageBoxW(nullptr,
                  tr.Or("error.missingPath",
                        L"\u547D\u4EE4\u7F3A\u5C11\u5F52\u6863\u8DEF\u5F84\u3002")
                      .c_str(),
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
    const std::wstring message = DescribeMissing7zDll(lookup, tr);
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
    const int code = RunHeadless(launch, *app, tr);
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
  // ---- C27: restored view state, sidebar port, menu bar -------------------
  // The geometry/mode/sort the user closed with is applied BEFORE the first
  // paint, so the window never flashes a layout it is about to replace.
  const prism::app::WindowState windowState = prism::app::LoadWindowState(settings.store);
  if (windowState.width > 0 && windowState.height > 0) {
    SetWindowPos(window.Hwnd(), nullptr, 0, 0, windowState.width, windowState.height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  // C12 + C14: the sidebar model gets the settings-backed store, so favourites,
  // recent archives and collapsed folders are restored here and written back at
  // exit. Without this line the port is null and the lists are session-only.
  prism::app::SettingsSidebarStore sidebarStore(settings.store);
  window.AttachSidebarStore(&sidebarStore);
  window.SetSidebarRailVisible(true);  // C12's draw commands are painted from here
  if (windowState.hasSort) window.SetSortSpec(windowState.sort);
  if (windowState.hasColumns) window.SetListColumns(windowState.columns);
  window.SetListMode(windowState.listMode);

  // ---- C34: the glass layer (C33) now has a call site ---------------------
  // The window owns the composition host; it is OPT-IN so a test-created window
  // never grows a DirectComposition target. A machine without composition says
  // so through the return value and the app keeps the pre-DComp paint path --
  // no glass memory is ever allocated there, so there is nothing to release.
  if (!window.EnableGlassLayer()) {
    logger.Emit(prism::log::Level::kInfo, "glass",
                "DirectComposition unavailable, glass layer stays off", {});
  }

  // C19: the menu bar is the app's, painted by the window. The keys exist in
  // both shipped tables, so a language switch is visible here (the window's
  // read-back in the suite asserts exactly that).
  const std::wstring fileMenuTitle =
      tr.Or("menu.file", language.tag == L"zh-CN" ? L"\u6587\u4EF6" : L"&File");
  window.SetMenuLabels(fileMenuTitle, tr("menu.compress.to.7z"),
                       tr.Or("action.exit", language.tag == L"zh-CN" ? L"\u9000\u51FA"
                                                                    : L"E&xit"));

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
  // The worker may not touch the translator (its miss list is mutable and
  // unsynchronized), so every sentence it needs is BUILT HERE, on the UI
  // thread, and captured by value.
  const std::string extractHeadline = prism::win::WideToUtf8(tr("action.extract"));
  window.SetActivateHandler([&window, app, &extractWorker, extractHeadline](int catalogIndex) {
    (void)catalogIndex;  // the whole-archive plan is v1's behaviour; see above
    if (app->extracting.exchange(true)) return;
    // A previous run must be finished before its std::thread is assigned over.
    if (extractWorker.joinable()) extractWorker.join();
    extractWorker = std::thread([app, &window, extractHeadline]() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      const uint64_t t0 = GalleryWindow::NowMs();
      window.PostHudStart(extractHeadline);
      prism::app::ExtractionOutcome outcome;
      std::string why;
      // Declared OUTSIDE the task branch: the C26 failure reason is composed
      // after the branch (both branches share the one status line).
      prism::win::SmartExtractCallback::Result result;
      auto task = app->ops->Begin(prism::core::sched::Kind::kExtract,
                                  prism::core::sched::Priority::kUser,
                                  "extract:" + prism::win::WideToUtf8(app->archivePath),
                                  "extract", &why);
      if (task.has_value()) {
        app->extractReader.store(task->reader);
        // Published BEFORE the flag check: see the shutdown contract above.
        if (app->shuttingDown.load()) task->reader->RequestBatchAbort();
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
      // C26 (delivered by the engine card, wired here): a handler can answer
      // S_OK while reporting a per-item failure through opResult -- the zstd
      // case where 7z.dll writes 0 bytes and calls it success. The reason names
      // the method and the fix; appending it is what turns "extraction failed"
      // into something the user can act on.
      std::wstring status = outcome.status;
      if (!outcome.ok && result.failed() && !result.failureReason.empty()) {
        status += L"\uFF1A" + prism::win::Utf8ToWide(result.failureReason);
      }
      // Posted, not called: hud_ / status_ belong to the UI thread.
      window.PostProgress(outcome.filesWritten, app->catalog.size());
      window.PostStatus(status);
      Telemetry("extract", outcome);
      std::fprintf(stderr, "extract wall=%llums\n",
                   static_cast<unsigned long long>(GalleryWindow::NowMs() - t0));
      app->extracting = false;
      CoUninitialize();
    });
  });
  // ---- C27: the compress entry (menu -> C18 dialog -> C9 writer -> HUD) ----
  //
  // The window shows the CONTENT of an open archive, and ArchiveWriter writes
  // from files on disk, so "compress what I see" is a re-pack: the payload is
  // staged into %TEMP% through the same engine handler every other operation
  // leases, then written by ArchiveWriter::CreateArchive. ONE job at a time and
  // ONE dialog at a time (the dialog is a top-level window, not modal, so the
  // message loop keeps painting while it is open).
  std::unique_ptr<prism::win::cd::CompressDialog> compressDialog;
  std::thread compressWorker;
  std::atomic<bool> compressBusy{false};
  const std::wstring compressHeadline = tr("menu.compress.to.7z");
  const std::wstring compressBusyText =
      tr.Or("progress.compress.busy",
            language.tag == L"zh-CN" ? L"\u6B63\u5728\u538B\u7F29\uFF0C\u8BF7\u7A0D\u540E"
                                     : L"Compression already running");
  const std::wstring compressDonePrefix =
      tr.Or("progress.compressed", language.tag == L"zh-CN" ? L"\u5DF2\u538B\u7F29"
                                                            : L"Compressed");
  const std::wstring compressFailPrefix =
      tr.Or("progress.compressFailed", language.tag == L"zh-CN" ? L"\u538B\u7F29\u5931\u8D25"
                                                                : L"Compression failed");

  window.SetMenuCommandHandler([&, instance, compressHeadline, compressBusyText,
                                compressDonePrefix, compressFailPrefix](UINT command) {
    if (command == GalleryWindow::kMenuExit) {
      window.RequestClose();
      return;
    }
    if (command != GalleryWindow::kMenuCompress) return;
    if (compressBusy.load()) {
      window.SetStatus(compressBusyText);
      return;
    }
    if (compressDialog && compressDialog->Hwnd() != nullptr) {
      SetForegroundWindow(compressDialog->Hwnd());
      return;
    }
    // Default target: next to the archive, "<stem>.7z".
    const std::wstring stem = prism::app::StemName(FileNameWide(app->archivePath));
    const std::wstring defaultTarget =
        prism::app::ParentDirectory(app->archivePath) + L"\\" +
        prism::win::Utf8ToWide(prism::DefaultArchiveFileName(
            prism::app::EntryPathOf(stem), prism::ArchiveFormat::SevenZip));

    compressDialog = std::make_unique<prism::win::cd::CompressDialog>();
    compressDialog->State().SetTargetPath(prism::win::WideToUtf8(defaultTarget));
    // The estimate line works from the catalog (the bytes are not on disk yet);
    // the RUN uses the plan the dialog builds, which those same items carry.
    compressDialog->SetSource(prism::app::PlanItemsFromCatalog(app->catalog),
                              std::filesystem::temp_directory_path().wstring());
    compressDialog->SetBrowseHandler([defaultTarget]() { return PickSaveArchive(defaultTarget); });
    compressDialog->SetAcceptHandler(
        [&, compressHeadline, compressDonePrefix,
         compressFailPrefix](const prism::win::cd::CreateRequest& request) {
          if (compressBusy.exchange(true)) return false;
          if (compressWorker.joinable()) compressWorker.join();
          std::error_code ec;
          const std::filesystem::path stageRoot =
              std::filesystem::temp_directory_path(ec) / L"PrismArchive";
          ec.clear();
          std::filesystem::create_directories(stageRoot, ec);
          const std::wstring stage =
              (stageRoot / (L"repack-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                            std::to_wstring(GalleryWindow::NowMs())))
                  .wstring();
          std::filesystem::create_directories(stage, ec);
          if (ec) {
            compressBusy.store(false);
            window.SetStatus(compressFailPrefix);
            return false;  // keep the dialog open: nothing was started
          }
          compressWorker = std::thread([&, request, stage, compressHeadline,
                                        compressDonePrefix, compressFailPrefix]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            window.PostHudStart(prism::win::WideToUtf8(compressHeadline));
            prism::app::CompressionOutcome outcome;
            bool staged = false;
            std::string why;
            auto task = app->ops->Begin(prism::core::sched::Kind::kExtract,
                                        prism::core::sched::Priority::kUser,
                                        "repack:" + prism::win::WideToUtf8(app->archivePath),
                                        "repack", &why);
            if (task.has_value()) {
              app->extractReader.store(task->reader);
              if (app->shuttingDown.load()) task->reader->RequestBatchAbort();
              prism::SmartPlan plan;
              plan.strategy = prism::SmartStrategy::FlatDirectly;
              plan.baseTargetDir = prism::win::WideToUtf8(stage);
              prism::win::SmartExtractCallback::Result staged_;
              staged = task->reader->ExtractWithPlan(app->catalog, plan, plan.baseTargetDir,
                                                     nullptr, &staged_);
              app->extractReader.store(nullptr);
              app->ops->Finish(*task, staged);
            } else {
              outcome.error = why;
            }
            if (staged) {
              prism::app::CompressionRequest job;
              job.archivePath = request.archivePath;
              job.sourceRoot = stage;
              job.plan = request.plan;
              job.password = request.password;
              outcome = prism::app::RunCompression(
                  job, [&window](uint64_t done, uint64_t total) {
                    window.PostProgress(done, total);  // posts, never touches
                    return true;
                  });
            }
            std::error_code cleanup;
            std::filesystem::remove_all(stage, cleanup);  // best effort, never fatal
            window.PostProgress(outcome.ok ? outcome.itemsWritten : 1,
                                outcome.ok ? outcome.itemsWritten : 1);
            std::wstring status = (outcome.ok ? compressDonePrefix : compressFailPrefix) +
                                  L" " + outcome.archivePath;
            if (!outcome.ok) {
              status += L"\\n";
              status += outcome.error.empty() ? std::wstring(L"(engine reported no reason)")
                                              : prism::win::Utf8ToWide(outcome.error);
            }
            window.PostStatus(status);
            // The re-pack wrote a NEW file: the UI thread switches the session
            // to it (C34). Passing the path is what makes that possible without a
            // second member a worker would have to touch.
            if (outcome.ok) window.PostArchiveChanged(outcome.archivePath);
            std::fprintf(stderr, "compress ok=%d items=%llu bytes=%llu verified=%llu ms=%lld\n",
                         static_cast<int>(outcome.ok),
                         static_cast<unsigned long long>(outcome.itemsWritten),
                         static_cast<unsigned long long>(outcome.bytesWritten),
                         static_cast<unsigned long long>(outcome.verifiedItems),
                         static_cast<long long>(outcome.elapsedMs));
            compressBusy.store(false);
            CoUninitialize();
          });
          return true;  // the dialog closes; the worker reports through the HUD
        });
    if (!compressDialog->Create(instance, compressHeadline)) {
      compressDialog.reset();
      window.SetStatus(compressFailPrefix);
    }
  });

  // A finished re-pack lands in a NEW file, so "refresh" means "switch": replace
  // the catalogue, the rows and the handler pool. C11's pool is what makes that
  // safe at all -- the app only has to guarantee that no lease is live (the sweep
  // is the one operation that can still be running), and prism::app::SwitchArchive
  // does the ordering: open + lease the new file, THEN close the old pool. A
  // failed switch therefore leaves the session on the archive it was showing.
  window.SetArchiveChangedHandler([&window, app, tr](const std::wstring& newPath) {
    if (app->extractReader.load() != nullptr || app->sweepReader.load() != nullptr) {
      window.SetStatus(tr.Or("status.refreshDeferred",
                             L"\u5DF2\u5199\u5165\uFF1B\u5F53\u524D\u64CD\u4F5C\u7ED3\u675F"
                             L"\u540E\u5237\u65B0"));
      return;
    }
    prism::app::ArchiveSession session{&app->library, &app->ops, &app->catalog, &app->items,
                                       &app->archivePath, 2};
    const prism::app::ArchiveSwitch switched = prism::app::SwitchArchive(session, newPath);
    if (!switched.ok) {
      window.SetStatus(tr.Or("error.open", L"\u6253\u5F00\u5931\u8D25") + L" " +
                           FileNameWide(newPath) + L"\uFF1A" +
                           prism::win::Utf8ToWide(switched.error));
      return;
    }
    // The plan is derived from the catalogue and the archive's own folder, so it
    // is rebuilt against the file the session now shows instead of the old one.
    app->plan = prism::app::BuildExtractionPlan(
        prism::app::LaunchMode::kGallery, prism::win::WideToUtf8(app->archivePath), app->catalog,
        [](const std::string& path) {
          return std::filesystem::exists(prism::win::Utf8ToWide(path));
        });
    // Thumbnails are keyed by CATALOG INDEX and the new catalogue belongs to a
    // different file: the old store would paint the previous archive's pixels into
    // the new rows. Drop it; sweeping the new archive is the follow-up this card
    // does not pretend to do.
    window.SetThumbs(nullptr);
    window.SetItems(&app->items);
    window.SetRootLabel(FileNameWide(app->archivePath));
    if (window.Hwnd() != nullptr) {
      const std::wstring title = L"PrismArchive -- " + FileNameWide(app->archivePath);
      SetWindowTextW(window.Hwnd(), title.c_str());
    }
    window.SetStatus(L"\x5171 " + std::to_wstring(app->items.size()) + L" \x9879");
  });

  window.SetStatus(L"\x5171 " + std::to_wstring(app->items.size()) + L" \x9879");

  // ---- C34: the inline-preview companion (C30) is hosted HERE --------------
  //
  // The shell's right-click publishes a wake and expects a running OWNER of the
  // preview section; without one it takes the NoReader fast path and launches
  // the app instead (C30 measured that path at microseconds -- the reason it
  // silently won every time is that nothing on this side ever created the
  // section). This is that owner: the channel, a hidden flyout, and the window's
  // idle tick as the wake poll. The catalogue is handed by POINTER, so a later
  // archive switch is picked up here without touching this code.
  //
  // A failure to start is logged, never fatal: the app is still a gallery, and
  // the shell still has its launch path.
  prism::app::CompanionHost companion;
  {
    prism::app::CompanionHost::Config companionConfig;
    companionConfig.openArchive = &app->archivePath;
    companionConfig.catalog = &app->items;
    companionConfig.library = &app->library;
    std::string why;
    if (companion.Start(instance, companionConfig, &why)) {
      window.SetIdleHandler([&companion]() { companion.Pump(); });
    } else {
      logger.Emit(prism::log::Level::kWarn, "companion",
                  "inline preview companion not started (the shell keeps its NoReader path)",
                  {{"detail", why}});
    }
  }

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

  // C34: the flyout is a second top-level window of this thread and the channel
  // owns a section the shell can see. Both go away here, before the workers are
  // joined and before the stack objects below leave scope.
  companion.Stop();

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
  if (compressWorker.joinable()) compressWorker.join();
  // The dialog outlives Run() only when the user closed the main window while it
  // was open; its destructor destroys the HWND, which is safe after the loop.
  compressDialog.reset();

  // ---- C27: persist the session (C14) -------------------------------------
  // Read the state the user ACTUALLY left: the frame size captured in WM_CLOSE
  // (the HWND is gone now), the view mode, the sort key and the column
  // geometry. Then the sidebar blob, then one atomic write of the document.
  int frameWidth = 0;
  int frameHeight = 0;
  prism::app::WindowState finalState;
  if (window.ClosedFrameSize(&frameWidth, &frameHeight)) {
    finalState.width = frameWidth;
    finalState.height = frameHeight;
  }
  finalState.listMode = window.ListMode();
  finalState.sort = window.SortSpec();
  finalState.columns = window.ListColumns();
  finalState.hasColumns = window.HasListColumns();
  window.SaveSidebar();  // C12 state through C14's port
  prism::app::SaveWindowState(settings.store, finalState);
  std::string saveDetail;
  if (!prism::app::SaveSettings(&settings, &saveDetail)) {
    logger.Emit(prism::log::Level::kWarn, "settings", "settings were not saved",
                {{"detail", saveDetail},
                 {"path", prism::win::WideToUtf8(settings.paths.file)}});
  }

  // C19: every key that never resolved gets ONE warn line (the translator keeps
  // the unique list, so a status bar redrawn 60 times a second cannot flood it).
  for (const std::string& key : language.translator.MissingKeys()) {
    logger.Emit(prism::log::Level::kWarn, "i18n", "missing string key, the key is shown verbatim",
                {{"key", key}, {"language", language.choice.tag}});
  }

  if (SUCCEEDED(coInit)) CoUninitialize();
  return 0;
}
