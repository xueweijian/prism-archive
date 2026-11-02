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
#include "../PrismWin/EditConfirmDialog.hpp"  // C54: the edit confirmation (programmatic)
#include "../PrismWin/GalleryWindow.hpp"
#include "../PrismWin/PasswordDialog.hpp"    // C35's dialog (C40 wires it into open)
#include "../PrismWin/PrismEngineBridge.hpp"  // Resolve7zDll, WideToUtf8
#include "../PrismWin/SidebarView.hpp"      // C40: SetTranslator (four headings)
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
  // C37: every path carries its SOURCE. "C:\Program Files\7-Zip\7z.dll" listed
  // without saying whether it came from PATH, a registry key or a hard-coded
  // probe leaves the reader unable to tell a truncated search from a wrong one.
  for (std::size_t i = 0; i < lookup.candidates.size(); ++i) {
    const std::wstring source = i < lookup.candidateSources.size()
                                    ? L"[" + lookup.candidateSources[i] + L"] "
                                    : std::wstring();
    text += L"  " + source + lookup.candidates[i] + L"\n";
  }
  text +=
      L"\n\u4FEE\u590D\u65B9\u6CD5\uFF08\u4EFB\u9009\u4E00\uFF09\uFF1A\n"
      L"  1. \u5B89\u88C5 7-Zip\uFF08https://www.7-zip.org/\uFF09\uFF1B\n"
      L"  2. \u8BBE\u7F6E\u73AF\u5883\u53D8\u91CF PRISM_7Z_DLL "
      L"\u6307\u5411 7z.dll \u7684\u5B8C\u6574\u8DEF\u5F84\uFF1B\n"
      L"  3. \u628A 7z.dll \u653E\u5230\u672C\u7A0B\u5E8F\u540C\u76EE"
      L"\u5F55\uFF1B\n"
      L"  4. \u628A 7-Zip \u7684\u76EE\u5F55\u52A0\u5165 PATH\uFF08\u4FBF\u643A\u7248\u9002\u7528\uFF09\u3002\n\n";
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
  // C34: bumped every time the session switches archives. The one-shot sweep
  // captured CATALOG INDICES from the archive it started on; a result that lands
  // after a switch belongs to a different file and must be discarded, not
  // painted (the window's rows are the new archive's).
  std::atomic<uint64_t> archiveGeneration{0};
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

// ---------------------------------------------------------------------------
// C40: the password gate -- C5's ring, C35's dialog, and the app's threads.
//
// The contract this function exists to keep (CONTRACTS B9):
//   * Submit arrives on the UI thread (the dialog's WM_COMMAND);
//   * the probe -- a real 7z.dll Open, seconds on a big header -- runs on ONE
//     worker, whose only way back is PostMessage into PasswordResultWindow;
//   * FinishProbe runs on the UI thread, because it touches controls;
//   * "remember" writes the DPAPI vault only after the UI thread has seen the
//     hit, and only for a password the probe PROVED.
//
// The gate is modal for the open: it pumps messages until the dialog is gone.
// That pump is also what delivers the worker's posted result -- which is why the
// loop blocks in GetMessageW only while the dialog is still alive.
//
// LIMIT (stated, not hidden): the password is used for Open only. Extraction
// through ExtractWithPlan has no password parameter yet (C5's follow-up), so a
// header-encrypted archive opens and lists here but its extraction will still
// fail -- the next card that touches the extract callback owns that.
// ---------------------------------------------------------------------------
std::wstring RunPasswordGate(HINSTANCE instance, const std::wstring& archive,
                             prism::win::SevenZipLibrary& library) {
  prism::pwd::PasswordUiConfig config;
  config.archiveName = prism::win::WideToUtf8(FileNameWide(archive));
  const std::wstring vaultPath = prism::app::DefaultVaultPath();
  if (!vaultPath.empty()) {
    // Reading the vault is a DPAPI call per entry; it happens once, here, before
    // the dialog exists.
    prism::win::DpapiVault vault(vaultPath);
    config.vaultEntries = vault.List();
  }

  prism::win::pwd_ui::PasswordDialog dialog;
  if (!dialog.Create(instance, L"PrismArchive")) return std::wstring();
  dialog.SetConfig(config);

  prism::app::PasswordProbeRunner runner;
  prism::app::PasswordResultWindow sink;
  std::wstring opened;

  // The UI thread's half of the hop: a posted result is applied HERE, and the
  // dialog is the only thing that sees it.
  if (!sink.Create(instance, [&dialog](const prism::app::PasswordProbeResult& result) {
        dialog.FinishProbe(result.found, result.cancelled, result.password,
                           result.diagnostics);
      })) {
    dialog.Destroy();
    return std::wstring();
  }

  dialog.SetSubmitHandler([&runner, &sink, &library, archive](const std::string& password) {
    runner.Start(
        password,
        [&library, archive](const std::string& candidate, std::string* why) {
          // WORKER THREAD. Its own reader: 7z.dll handlers are not thread-safe
          // and the UI thread may still hold the failed probe.
          prism::win::ArchiveReader reader(library);
          const bool ok = reader.Open(archive, prism::win::Utf8ToWide(candidate));
          if (why != nullptr) *why = reader.openTrace();
          return ok;
        },
        [&sink](const prism::app::PasswordProbeResult& result) {
          sink.Post(result);  // POST only: no window call on this thread
        });
  });
  dialog.SetOpenedHandler([&opened, vaultPath](const std::string& password, bool remember) {
    opened = prism::win::Utf8ToWide(password);
    if (remember) prism::app::RememberPassword(vaultPath, password);
  });
  dialog.Start();

  MSG message{};
  while (dialog.Hwnd() != nullptr) {
    if (GetMessageW(&message, nullptr, 0, 0) <= 0) break;
    TranslateMessage(&message);
    DispatchMessageW(&message);
  }
  // Never detach: the worker must be finished before "library" is used again by
  // the caller's retry, and before this stack frame dies.
  runner.Join();
  sink.Destroy();
  dialog.Destroy();
  return opened;
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

  // ---- C46: the previous session, read BEFORE anything opens --------------
  // The session is a SEPARATE document from settings on purpose: settings are
  // the user's preferences (merge on write, newer documents are read-only),
  // while a session is a snapshot of one run (rewritten every start and exit).
  // One corrupt session must therefore never cost the user their settings.
  prism::app::SessionBootstrap session = prism::app::LoadSession();
  if (!session.detail.empty()) {
    logger.Emit(prism::log::Level::kWarn, "session", "session could not be read",
                {{"detail", session.detail},
                 {"path", prism::win::WideToUtf8(session.file)}});
  }
  // C27's persisted view state is loaded here (not at the window call site) so
  // C46 can record the same view mode the session opens with.
  const prism::app::WindowState windowState = prism::app::LoadWindowState(settings.store);
  for (const std::string& warning : session.report.warnings) {
    // One warn line per repair/discard. A v99 document from a newer build, a
    // truncated file and a migrated v1 all say what happened on stderr.
    logger.Emit(prism::log::Level::kWarn, "session", warning,
                {{"status", prism::session::SessionLoadStatusName(session.report.status)},
                 {"path", prism::win::WideToUtf8(session.file)}});
  }

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
  // C46: the view a restored archive opens with (only set on the restore path).
  prism::session::SessionEntry restoredView;
  bool haveRestoredView = false;
  std::size_t sessionTried = 0;
  std::size_t sessionSkipped = 0;

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
  if (!app->library.Load(dll)) {
    // C40: error.engine.load has shipped since C19 with a {path} placeholder;
    // the English "7z.dll failed to load" literal was the last hardcoded error on
    // this path. Sub() keeps the Chinese fallback for a checkout without tables.
    const std::wstring message =
        tr.Sub("error.engine.load", {{"path", prism::win::WideToUtf8(dll)}},
               std::wstring(L"\u52A0\u8F7D 7z.dll \u5931\u8D25\uFF1A") + dll);
    MessageBoxW(nullptr, message.c_str(), L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }

  // ---- C46: arguments win; otherwise the last session decides --------------
  //
  // "Arguments win" is not just "ignore the session": the session document is
  // REBUILT around the paths this launch opened, so a later no-argument start
  // restores exactly what this one showed. A path that was already remembered
  // keeps its folder and view mode.
  if (!launch.archives.empty()) {
    std::vector<prism::session::SessionEntry> opened;
    for (const std::wstring& arg : launch.archives) {
      prism::session::SessionEntry entry;
      entry.path = prism::win::WideToUtf8(arg);
      const int known = session.store.IndexOf(entry.path);
      if (known >= 0) entry = session.store.entries()[static_cast<std::size_t>(known)];
      opened.push_back(std::move(entry));
    }
    session.store.Replace(std::move(opened), 0);
  } else if (archive.empty() && launch.mode == prism::app::LaunchMode::kGallery) {
    // No argument: reopen the previous session. Every candidate is validated
    // with the SAME probe the real open uses -- a missing or damaged archive is
    // skipped SILENTLY (counted, one stderr line) instead of raising a dialog.
    // This is the contract that keeps eight stale tabs from eight message boxes.
    const prism::app::SessionRestorePlan plan = prism::app::PlanSessionRestore(
        session.store, [&app](const std::wstring& path) {
          std::error_code ec;
          if (!std::filesystem::exists(path, ec) || ec) return false;
          prism::win::ArchiveReader probe(app->library);
          return probe.Open(path);
        });
    sessionTried = plan.tried;
    sessionSkipped = plan.skipped;
    // The document's own size, captured BEFORE the pruning below: the log must
    // say "the file listed 3, the guard allowed 3, 2 were dead" -- not the size
    // this run already cleaned up.
    const std::size_t documentEntries = session.store.entries().size();
    // A missing archive is no longer part of the session: keeping it would only
    // make the next start skip it again.
    if (plan.skipped > 0) {
      for (const prism::session::SessionEntry& entry : session.store.entries()) {
        bool survived = false;
        for (const prism::session::SessionEntry& restorable : plan.entries) {
          if (restorable.path == entry.path) {
            survived = true;
            break;
          }
        }
        if (!survived) {
          logger.Emit(prism::log::Level::kInfo, "session",
                      "session entry skipped (the archive is gone or unreadable)",
                      {{"path", entry.path}});
        }
      }
    }
    if (!plan.entries.empty()) {
      session.store.Replace(plan.entries, 0);
      restoredView = plan.entries.front();
      haveRestoredView = true;
      archive = prism::win::Utf8ToWide(restoredView.path);
    }
    // "entries" is what the DOCUMENT listed; "tried" is what the crash guard let
    // through. A killed process is entries=2 tried=0 -- the two numbers together
    // are what makes the guard visible in a log instead of a mystery.
    std::fprintf(stderr,
                 "session restore entries=%zu tried=%zu skipped=%zu restored=%zu status=%s clean=%d\n",
                 documentEntries, sessionTried, sessionSkipped, plan.entries.size(),
                 prism::session::SessionLoadStatusName(session.report.status),
                 static_cast<int>(session.report.cleanShutdown));
    if (archive.empty()) {
      // First run (or nothing survived): the picker keeps the old behaviour.
      archive = PickArchive();
      if (archive.empty()) {
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 0;  // user closed the picker: not an error
      }
      session.store.Replace(
          {prism::session::SessionEntry{prism::win::WideToUtf8(archive), std::string(), false}}, 0);
    }
  }

  app->archivePath = archive;
  if (archive.empty()) {  // unreachable, but never open "" silently
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 2;
  }

  // The catalog is read ONCE, on the UI thread, from a probe handler that is
  // released before the pool opens its own (one seek+header read, blueprint's
  // zero-payload scan). The pool's handlers are the ones that decode.
  // C40: the password the gate PROVES, kept for the handler pool below -- the
  // pool (C34/C35) opens every extraction handler with it, so an archive that
  // opened with a password can also be extracted.
  std::wstring archivePassword;
  {
    std::string trace;  // the engine trace is narrow; only the message is wide
    bool opened = false;
    {
      prism::win::ArchiveReader probe(app->library);
      opened = probe.Open(archive);
      if (opened) {
        app->catalog = probe.ReadCatalog();
      } else {
        trace = probe.openTrace();
      }
    }
    // C40: the password gate. A header-encrypted archive makes our own open
    // callback answer E_ABORT (C5's measured truth table), which is the one
    // signal that says "ask the user" rather than "the file is broken".
    if (!opened && prism::app::OpenNeedsPassword(trace)) {
      archivePassword = RunPasswordGate(instance, archive, app->library);
      if (!archivePassword.empty()) {
        // A fresh reader: the failed one already holds a stream and a partial
        // archive object, and re-Open()ing it would leak both.
        prism::win::ArchiveReader retry(app->library);
        opened = retry.Open(archive, archivePassword);
        if (opened) {
          app->catalog = retry.ReadCatalog();
        } else {
          trace = retry.openTrace();
        }
      }
    }
    if (!opened) {
      MessageBoxW(nullptr,
                  (prism::app::OpenFailedStatus(tr, FileNameWide(archive)) + L"\n\n" +
                   prism::win::Utf8ToWide(trace))
                      .c_str(),
                  L"PrismArchive", MB_ICONERROR);
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 1;
    }
  }
  app->items = app->catalog;

  // ---- C46: this process now OWNS the session ------------------------------
  // Arm the crash guard and publish the document BEFORE the window exists. From
  // here on a killed process leaves "cleanShutdown": false on disk, which is
  // what makes the next start restore nothing instead of reopening dead tabs.
  {
    const int known = session.store.IndexOf(prism::win::WideToUtf8(archive));
    const bool viewMode =
        haveRestoredView
            ? restoredView.listMode
            : (known >= 0 ? session.store.entries()[static_cast<std::size_t>(known)].listMode
                          : windowState.listMode);
    session.store.RecordOpen(prism::win::WideToUtf8(archive),
                             haveRestoredView ? restoredView.folder : std::string(), viewMode);
    session.store.MarkCleanShutdown(false);
    std::string detail;
    if (!prism::app::SaveSession(&session, &detail)) {
      logger.Emit(prism::log::Level::kWarn, "session", "session was not saved",
                  {{"detail", detail},
                   {"path", prism::win::WideToUtf8(session.file)}});
    }
    std::fprintf(stderr, "session armed entries=%zu file=%s\n", session.store.entries().size(),
                 prism::win::WideToUtf8(session.file).c_str());
  }

  // Two handlers: the solid sweep and the user's extraction are the two
  // operations that can genuinely overlap (large archive + double-click).
  prism::app::OperationHandles::Config opsConfig;
  opsConfig.archivePath = archive;
  opsConfig.handlerSlots = 2;
  // Empty for an unencrypted archive; the gate's proven secret otherwise. This is
  // the join between C40's open flow and C34/C35's pool: without it the catalogue
  // would open and every extraction handler would fail on the same header.
  opsConfig.password = archivePassword;
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

  // C40: the sidebar's four section headings come from the same tables as every
  // other sentence. The renderer's call site belongs to another card, so the
  // translator is installed here, once, before the first layout is built.
  prism::win::sidebar::SetTranslator(&language.translator);

  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive -- " + FileNameWide(archive), instance)) {
    app->ops->Close();
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  // ---- C27: restored view state, sidebar port, menu bar -------------------
  // The geometry/mode/sort the user closed with is applied BEFORE the first
  // paint, so the window never flashes a layout it is about to replace.
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
  // C46: a restored archive opens in the view mode the session remembers for
  // THAT archive; a freshly opened one keeps C27's global last-view setting.
  window.SetListMode(haveRestoredView ? restoredView.listMode : windowState.listMode);

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
  // C54: the Edit menu. The labels are the app's words (the window only paints
  // them), and the same three actions are bound to Delete / F2 / Ctrl+Shift+N in
  // PrismCore's key table.
  window.SetEditMenuLabels(
      tr.Or("menu.edit", language.tag == L"zh-CN" ? L"\u7F16\u8F91" : L"&Edit"),
      tr.Or("menu.edit.delete", language.tag == L"zh-CN" ? L"\u5220\u9664" : L"&Delete"),
      tr.Or("menu.edit.rename", language.tag == L"zh-CN" ? L"\u91CD\u547D\u540D" : L"Rena&me"),
      tr.Or("menu.edit.newFolder",
            language.tag == L"zh-CN" ? L"\u65B0\u5EFA\u6587\u4EF6\u5939" : L"New &Folder"));
  // Requirement 3: the entries are disabled BEFORE the user confirms something
  // that can only fail, and the reason is C51's own diagnosis (read-only /
  // multi-volume / held exclusively / wrong container).
  {
    const prism::app::ArchiveEditGate gate = prism::app::ProbeEditGate(app->archivePath);
    window.SetEditAvailability(gate.enabled, gate.reason);
    if (!gate.enabled) {
      logger.Emit(prism::log::Level::kInfo, "edit",
                  "archive editing disabled", {{"reason", prism::win::WideToUtf8(gate.reason)}});
    }
  }

  // The window's navigator owns the current folder from here on: the catalog
  // goes in once, and every later question ("which rows?", "which crumbs?",
  // "which level did that click hit?") is answered by PrismCore.
  window.SetItems(&app->items);
  window.SetRootLabel(FileNameWide(archive));
  // C46: the per-archive place. The walk goes through the window's own
  // ActivateRow (the double-click path), so navigation stays owned by the
  // window while GALLERY V2 rewrites it.
  if (haveRestoredView) {
    const bool complete = prism::app::ApplySessionView(window, restoredView);
    if (!complete && !restoredView.folder.empty()) {
      logger.Emit(prism::log::Level::kInfo, "session",
                  "the saved folder is gone; restored to the deepest level that still exists",
                  {{"folder", restoredView.folder}, {"archive", prism::win::WideToUtf8(archive)}});
    }
  }
  std::fprintf(stderr, "session opened archive=%s folder=%s view=%s restored=%d\n",
               prism::win::WideToUtf8(archive).c_str(), window.Folder().c_str(),
               prism::session::ViewModeName(window.ListMode()),
               static_cast<int>(haveRestoredView));
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
  // ---- C54: editing an existing archive (C51's engine, this card's UI) ----
  // One dialog and one worker, like the compressor above. The window's gate
  // (BeginOperation/EndOperation) is what makes the edit exclusive: while it is
  // held, a double-click, Quick Look or a second edit is REFUSED with a reason.
  std::unique_ptr<prism::win::editui::EditConfirmDialog> editDialog;
  std::thread editWorker;
  const std::wstring editHeadline =
      tr.Or("progress.edit", language.tag == L"zh-CN" ? L"\u7F16\u8F91\u5F52\u6863"
                                                     : L"Editing archive");
  const std::wstring editDonePrefix =
      tr.Or("progress.edited", language.tag == L"zh-CN" ? L"\u5DF2\u7F16\u8F91\u5F52\u6863"
                                                       : L"Archive edited");
  const std::wstring editFailPrefix =
      tr.Or("progress.editFailed",
            language.tag == L"zh-CN" ? L"\u7F16\u8F91\u5931\u8D25" : L"Edit failed");
  const std::wstring editCancelledText =
      tr.Or("progress.editCancelled",
            language.tag == L"zh-CN" ? L"\u5DF2\u53D6\u6D88\u7F16\u8F91\uFF0C\u5F52"
                                       L"\u6863\u672A\u6539\u52A8"
                                     : L"Edit cancelled, the archive was not changed");
  const std::wstring editBlockedText =
      tr.Or("progress.editBlocked",
            language.tag == L"zh-CN" ? L"\u6B63\u5728\u89E3\u538B\u6216\u626B\u63CF"
                                       L"\uFF0C\u7A0D\u540E\u518D\u7F16\u8F91"
                                     : L"Extract or scan is running; edit later");
  const std::wstring editNoNameText =
      tr.Or("error.edit.noName",
            language.tag == L"zh-CN" ? L"\u540D\u79F0\u4E0D\u80FD\u4E3A\u7A7A"
                                     : L"A name is required");
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
  // C40: the worker may not touch the translator, so the "no reason" sentence is
  // resolved HERE, on the UI thread, and captured by value (the same rule the
  // extraction headline follows above).
  const std::wstring compressNoReason = prism::app::EngineNoReasonText(tr);

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
                                        compressDonePrefix, compressFailPrefix,
                                        compressNoReason]() {
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
              // C40: a real newline (this was a literal backslash-n), and the
              // engine's reason or the translated "no reason" sentence.
              status += L"\n";
              status += prism::app::CompressFailureDetail(compressNoReason, outcome.error);
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

  // ---- C54: the Edit menu / Delete / F2 / Ctrl+Shift+N ---------------------
  //
  // The window turns its selection into an INTENT (archive-relative path, kind);
  // this handler owns everything else: C51's plan for the sentence, the real
  // confirmation dialog, the exclusive-operation gate, one worker that calls
  // ArchiveEditor::ApplyEdits, the HUD, and the refresh through C34's switch.
  //
  // THREE CONTRACTS ARE KEPT HERE, IN THIS ORDER:
  //   1. no confirmation -> no write (the dialog's Cancel is the app's answer
  //      through EditMayProceed; PRISM_C54_MUTANT_CANCEL_STILL_WRITES removes it);
  //   2. no concurrent operation -> the pool is closed before the rewrite
  //      (CONTRACTS D30 fact 3: ReplaceFileW fails while any reader holds the
  //      file), and the gate refuses a second edit/extract/preview while this
  //      one is in flight;
  //   3. a failed rewrite leaves the original byte-for-byte untouched -- C51's
  //      executor guarantees it, and the suite asserts the file's bytes.
  window.SetEditHandler([&window, app, instance, archivePassword, &editDialog, &editWorker,
                         editHeadline, editDonePrefix, editFailPrefix, editCancelledText,
                         editBlockedText, editNoNameText](
                            const prism::win::editui::EditIntent& intent) {
    namespace editui = prism::win::editui;
    // The dialog AND the rewrite are one operation: while it is up, no preview,
    // no extraction and no second edit may start (requirement 4).
    if (!window.BeginOperation(GalleryWindow::OperationKind::Edit, L"编辑归档")) return;

    // The plan quoted by the dialog. Rename/new-folder start with the current
    // name; the user edits it in the dialog, and the accepted text is re-planned
    // and re-validated before anything touches the file.
    std::vector<prism::EditOp> ops = editui::DefaultEditOps(intent);
    if (ops.empty()) {
      window.EndOperation();
      window.SetStatus(editNoNameText);
      return;
    }
    const prism::EditPlan preview =
        prism::app::PreviewArchiveEdit(app->archivePath, app->catalog, ops);
    if (!preview.ok) {
      window.EndOperation();
      window.SetStatus(L"编辑被拒绝：" + prism::win::Utf8ToWide(preview.error));
      return;
    }
    const std::wstring archiveName = FileNameWide(app->archivePath);
    editui::EditDialogModel model =
        editui::BuildEditDialogModel(intent.action, intent.path, preview, archiveName);
    editDialog = std::make_unique<editui::EditConfirmDialog>();
    if (!editDialog->Create(instance, model.title)) {
      editDialog.reset();
      window.EndOperation();
      window.SetStatus(editFailPrefix + L"：无法创建确认对话框");
      return;
    }
    editDialog->SetModel(model);
    editDialog->SetResultHandler(
        [&window, app, intent, archivePassword, &editWorker, editHeadline,
         editDonePrefix, editFailPrefix, editCancelledText, editBlockedText,
         editNoNameText](const editui::EditDialogResult& result) {
          // (1) THE CANCEL RULE. Cancel means the archive is not touched; the
          // negative control turns this into "write anyway".
          if (!editui::EditMayProceed(result)) {
            window.EndOperation();
            window.SetStatus(editCancelledText);
            return;
          }
          std::vector<prism::EditOp> acceptedOps = editui::BuildEditOps(intent, result.input);
          if (acceptedOps.empty()) {
            window.EndOperation();
            window.SetStatus(editNoNameText);
            return;
          }
          const prism::EditPlan plan = prism::app::PreviewArchiveEdit(
              app->archivePath, app->catalog, acceptedOps);
          if (!plan.ok) {
            window.EndOperation();
            window.SetStatus(L"编辑被拒绝：" + prism::win::Utf8ToWide(plan.error));
            return;
          }
          // (2) No reader may be live when the file is replaced. The window's
          // gate already refuses a NEW operation; these are the operations that
          // started before the user asked to edit.
          if (app->extracting.load() || app->extractReader.load() != nullptr ||
              app->sweepReader.load() != nullptr) {
            window.EndOperation();
            window.SetStatus(editBlockedText);
            return;
          }
          // CONTRACTS D30 fact 3: a live ReaderPool holds the original with
          // FILE_SHARE_READ, and ReplaceFileW cannot remove a handle-held file.
          // Closing the pool is not enough -- the pool OBJECT owns the readers,
          // so it has to be destroyed before the worker starts.
          if (app->ops) {
            app->ops->Close();
            app->ops.reset();
          }
          if (editWorker.joinable()) editWorker.join();
          const uint64_t total = plan.estimatedWriteBytes;
          editWorker = std::thread([app, &window, acceptedOps, total, archivePassword,
                                    editHeadline, editDonePrefix, editFailPrefix]() {
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            window.PostHudStart(prism::win::WideToUtf8(editHeadline));
            prism::app::ArchiveEditRequest request;
            request.archivePath = app->archivePath;
            request.password = archivePassword;
            request.ops = acceptedOps;
            request.progress = [&window, total](uint64_t done) {
              window.PostProgress(done, total);
              return true;
            };
            const prism::app::ArchiveEditOutcome outcome = prism::app::RunArchiveEdit(request);
            std::wstring status;
            if (outcome.ok) {
              status = editDonePrefix + L"：" +
                       std::to_wstring(outcome.entriesAffected) + L" 个条目受影响，写入 " +
                       std::to_wstring(outcome.bytesWritten) + L" 字节";
            } else {
              status = editFailPrefix + L"\n" + prism::win::Utf8ToWide(outcome.error) +
                       L"\n原归档未改动（字节不变）";
            }
            std::fprintf(stderr,
                         "edit ok=%d entries=%llu write=%llu copied=%llu items=%llu ms=%lld\n",
                         static_cast<int>(outcome.ok),
                         static_cast<unsigned long long>(outcome.entriesAffected),
                         static_cast<unsigned long long>(outcome.bytesWritten),
                         static_cast<unsigned long long>(outcome.estimatedWriteBytes),
                         static_cast<unsigned long long>(outcome.itemsWritten),
                         static_cast<long long>(outcome.elapsedMs));
            // The pool was destroyed for the replace; C34's switch rebuilds it
            // and re-reads the catalogue. Post the refresh FIRST so the edit
            // result is the status line the user ends up reading.
            window.PostArchiveChanged(app->archivePath);
            window.PostOperationFinished(status);
            CoUninitialize();
          });
        });
    editDialog->Show();
  });

  // A finished re-pack lands in a NEW file, so "refresh" means "switch": replace
  // the catalogue, the rows and the handler pool. C11's pool is what makes that
  // safe at all -- the app only has to guarantee that no lease is live (the sweep
  // is the one operation that can still be running), and prism::app::SwitchArchive
  // does the ordering: open + lease the new file, THEN close the old pool. A
  // failed switch therefore leaves the session on the archive it was showing.
  window.SetArchiveChangedHandler([&window, app, tr, &session, &logger](
                                     const std::wstring& newPath) {
    if (app->extractReader.load() != nullptr || app->sweepReader.load() != nullptr) {
      // C40: the sentence (and its real-Chinese fallback) lives in AppWiring so
      // the suite can assert its bytes; C38's audit found it double-escaped.
      window.SetStatus(prism::app::RefreshDeferredStatus(tr));
      return;
    }
    prism::app::ArchiveSession archiveSession{&app->library, &app->ops, &app->catalog, &app->items,
                                              &app->archivePath, 2};
    const prism::app::ArchiveSwitch switched = prism::app::SwitchArchive(archiveSession, newPath);
    if (!switched.ok) {
      // C40: error.open is the table's key (it names the file); the engine's own
      // reason stays verbatim after the colon.
      window.SetStatus(prism::app::OpenFailedStatus(tr, FileNameWide(newPath)) + L"\uFF1A" +
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
    ++app->archiveGeneration;
    window.SetThumbs(nullptr);
    window.SetItems(&app->items);
    window.SetRootLabel(FileNameWide(app->archivePath));
    // C54: the file may have changed on disk (an edit wrote a new container),
    // so the read-only/lock/volume diagnosis is re-run for the SAME path.
    {
      const prism::app::ArchiveEditGate gate = prism::app::ProbeEditGate(app->archivePath);
      window.SetEditAvailability(gate.enabled, gate.reason);
    }
    if (window.Hwnd() != nullptr) {
      const std::wstring title = L"PrismArchive -- " + FileNameWide(app->archivePath);
      SetWindowTextW(window.Hwnd(), title.c_str());
    }
    window.SetStatus(prism::app::ItemsCountStatus(tr, app->items.size()));
    // C46: a switch is an OPEN -- the new archive becomes the session's active
    // entry, so the next launch offers the file the user actually ended up on.
    session.store.RecordOpen(prism::win::WideToUtf8(app->archivePath), std::string(),
                             window.ListMode());
    session.store.MarkCleanShutdown(false);
    std::string switchDetail;
    if (!prism::app::SaveSession(&session, &switchDetail)) {
      logger.Emit(prism::log::Level::kWarn, "session", "session was not saved",
                  {{"detail", switchDetail}});
    }
  });

  window.SetStatus(prism::app::ItemsCountStatus(tr, app->items.size()));

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
    // The indices below belong to the archive that was open when this thread
    // started; a switch while we decode makes them meaningless (C34).
    const uint64_t generation = app->archiveGeneration.load();
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
        // A generation bump means the session switched archives while we were
        // decoding: these pixels are indexed against a file nobody is showing.
        if (app->archiveGeneration.load() == generation) {
          window.SetThumbs(store);
          PostMessageW(window.Hwnd(), GalleryWindow::kThumbsChanged, 0, 0);
        } else {
          std::fprintf(stderr, "sweep discarded: the session switched archives\n");
        }
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
  // C54: an edit in flight destroyed the pool before ReplaceFileW; a close on
  // the null pointer would crash exactly the shutdown the edit was waiting on.
  if (app->ops) app->ops->Close();  // no new work; queued tasks are cancelled

  // Join EVERY worker before "window" leaves scope: it is a stack object and the
  // workers talk to it. Joining is what makes the posting contract sound -- a
  // Post* that lands after WM_DESTROY simply fails and frees its payload.
  // Never detach: a detached worker here would outlive "window" and "app".
  worker.join();
  if (extractWorker.joinable()) extractWorker.join();
  if (compressWorker.joinable()) compressWorker.join();
  if (editWorker.joinable()) editWorker.join();
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
  // ---- C46: the session's last write is the CLEAN one ---------------------
  // The active archive learns where the user actually finished (folder + view),
  // then the crash guard is cleared. Order matters: everything that can fail
  // (frame size, sidebar, settings) has already been read, so "clean" is only
  // claimed once the state it describes is in hand.
  session.store.UpdatePosition(prism::win::WideToUtf8(archive), window.Folder(),
                               window.ListMode());
  session.store.MarkCleanShutdown(true);
  {
    std::string detail;
    if (!prism::app::SaveSession(&session, &detail)) {
      logger.Emit(prism::log::Level::kWarn, "session", "session was not saved",
                  {{"detail", detail},
                   {"path", prism::win::WideToUtf8(session.file)}});
    }
    std::fprintf(stderr, "session saved entries=%zu clean=%d active=%s folder=%s view=%s\n",
                 session.store.entries().size(),
                 static_cast<int>(session.store.cleanShutdown()),
                 prism::win::WideToUtf8(archive).c_str(), window.Folder().c_str(),
                 prism::session::ViewModeName(window.ListMode()));
  }
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
