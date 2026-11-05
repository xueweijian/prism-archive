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
#include "../PrismCore/ChromeTheme.hpp"   // C83: the shell palette (Day/Night/HC)
#include "../PrismCore/I18n.hpp"           // C19: resource tables + language choice
#include "../PrismCore/Log.hpp"            // C16: the one warn line a missing key gets
#include "../PrismCore/Preview.hpp"        // QuickLookPlanner::KindByName
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismWin/CompressDialog.hpp"  // C18: the create dialog (programmatic)
#include "../PrismWin/DragOutToShell.hpp"   // C67: CF_HDROP drag out (lazy extract)
#include "../PrismWin/EditConfirmDialog.hpp"  // C54: the edit confirmation (programmatic)
#include "../PrismWin/GalleryWindow.hpp"
#include "../PrismWin/LogFile.hpp"           // C85: the daily file sink + the wall clock
#include "../PrismWin/PasswordDialog.hpp"    // C35's dialog (C40 wires it into open)
#include "../PrismWin/PrismEngineBridge.hpp"  // Resolve7zDll, WideToUtf8
#include "../PrismWin/SettingsDialog.hpp"   // C53: Settings... menu + dialog (C31/V3)
#include "../PrismWin/StartupTrace.hpp"  // C76: the QPC segment recorder
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
  // C71: extract.overwrite + extract.verifyAfterExtract, resolved from the
  // settings document this launch loaded (and refreshed after a save).
  prism::app::ExtractionOptions extractOptions;
};

// A context-menu verb we do not implement must SAY so on stderr -- and must not
// open a modal box. Explorer and scripts can invoke the exe for a whole
// selection; a MessageBox on an unsupported flag hangs the caller (measured:
// the process was still alive after 12 s with the box up), which is worse than
// the refusal. The exit code (2) is the verdict, the line below is the reason.
// The window path (double-click / no arguments) can never carry an unknown
// flag, so no interactive fallback is lost here.
void ReportRefusedVerb(const prism::app::LaunchRequest& request) {
  const std::string flag =
      request.unknownText.empty() ? std::string("(unknown)")
                                  : prism::win::WideToUtf8(request.unknownText);
  std::fprintf(stderr,
               "verb refused: unsupported flag %s; supported: --smart-extract "
               "--extract-here --extract-sub --test --compress-here --compress-7z "
               "--compress-zip --smart-extract-each\n",
               flag.c_str());
}

// C71: the GUI's overwrite prompt. The policy decides WHEN to ask (only "ask"
// mode + an existing file); this function is the UI seam. Yes = replace,
// No = skip, Cancel = save beside as "name (1).ext". No owner window: the
// extraction runs on a worker thread and must not touch GalleryWindow state
// (CONTRACTS B9), and a MessageBox is safe from any thread.
prism::app::ExtractionOptions LiveExtractionOptions(
    const prism::settings::SettingsStore& store) {
  prism::app::ExtractionOptions options =
      prism::app::ExtractionOptionsFromSettings(store);
  options.prompt = [](const std::string& path) {
    const std::wstring text =
        L"\u76EE\u6807\u5DF2\u5B58\u5728\uFF1A\n" + prism::win::Utf8ToWide(path) +
        L"\n\n\u662F = \u8986\u76D6\uFF0C\u5426 = \u8DF3\u8FC7\uFF0C"
        L"\u53D6\u6D88 = \u53E6\u5B58\u4E3A (1)";
    const int answer = MessageBoxW(nullptr, text.c_str(), L"PrismArchive",
                                  MB_YESNOCANCEL | MB_ICONQUESTION);
    if (answer == IDYES) return prism::overwrite::Choice::Replace;
    if (answer == IDCANCEL) return prism::overwrite::Choice::Rename;
    return prism::overwrite::Choice::Skip;  // No / dismissed
  };
  return options;
}

// One line of machine-readable telemetry. A GUI app has no console, so this is
// diagnostic only; the exit code carries the verdict.
void Telemetry(const char* what, const prism::app::ExtractionOutcome& outcome) {
  std::fprintf(stderr,
               "%s ok=%d files=%llu bytes=%llu skipped=%llu stamped=%llu amsiBlocked=%llu\n",
               what, static_cast<int>(outcome.ok),
               static_cast<unsigned long long>(outcome.filesWritten),
               static_cast<unsigned long long>(outcome.bytesWritten),
               static_cast<unsigned long long>(outcome.skippedFiles),
               static_cast<unsigned long long>(outcome.security.filesStamped),
               static_cast<unsigned long long>(outcome.security.amsiBlocked));
}

// C82: --password-file. One small UTF-8 file holds ONE password: a BOM and a
// trailing newline may ride along, nothing else. Returns false when the file
// cannot be read; the caller then refuses the launch on stderr (a credential
// that cannot be delivered is a caller bug, never an interactive prompt).
bool ReadPasswordFileUtf8(const std::wstring& path, std::wstring* password) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  std::string bytes;
  char buffer[512];
  DWORD got = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    bytes.append(buffer, got);
    if (bytes.size() > 4096) break;  // one credential, not a novel
  }
  CloseHandle(file);
  if (bytes.size() >= 3 && (bytes[0] & 0xFF) == 0xEF && (bytes[1] & 0xFF) == 0xBB &&
      (bytes[2] & 0xFF) == 0xBF) {
    bytes.erase(0, 3);  // UTF-8 BOM
  }
  while (!bytes.empty() && (bytes.back() == '\n' || bytes.back() == '\r' ||
                            bytes.back() == ' ' || bytes.back() == '\t')) {
    bytes.pop_back();
  }
  *password = prism::win::Utf8ToWide(bytes);
  return !password->empty();
}

// Headless path for the three extracting verbs and for --test. Returns the
// process exit code: 0 on success, 1 on a diagnosable failure (with the reason
// on stderr -- C82 removed this path's last modals), 2 when the verb needs an
// archive it was not given.
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
      // C82: a headless verb reports on stderr -- a modal here hangs the batch
      // caller (the exact defect this card removes for the password gate).
      std::fprintf(stderr, "verify failed: %s\n", outcome.error.c_str());
      return 1;
    }
    return 0;
  }

  // A shell verb runs with no window on purpose: it must NEVER block on a modal
  // box (the c58 suite measures this). The GUI prompt in app.extractOptions is
  // therefore dropped here; an "ask" conflict becomes Skip, which is the only
  // safe non-interactive answer.
  prism::app::ExtractionOptions options = app.extractOptions;
  options.prompt = {};
  const prism::app::ExtractionOutcome outcome = prism::app::RunExtraction(
      *app.ops, app.catalog, app.plan, app.archivePath, /*scanExecutables=*/true,
      prism::win::amsi::InitMode::LoadAmsi, options);
  Telemetry("extract", outcome);
  if (!outcome.ok) {
    // C82: headless failure = stderr + exit code. The box is gone: a verb run
    // by Explorer or a script has nobody to click OK, and the caller hangs.
    std::fprintf(stderr, "extract failed: %s\n",
                 outcome.error.empty() ? "(engine reported no reason)"
                                       : outcome.error.c_str());
    return 1;
  }
  // A security finding is not a silent success: the user gets told that some
  // payload could not be scanned or was flagged (C5's Unavailable contract).
  if (outcome.security.amsiBlocked > 0 || outcome.security.amsiCouldNotScan > 0) {
    std::fprintf(stderr, "security: %s\n",
                 prism::win::WideToUtf8(outcome.status).c_str());
  }
  return 0;
}

// --compress-here / --compress-7z / --compress-zip <files...> (C27 + C58):
// archive creation without a window. The verb decides the FORMAT
// (CompressionFormatOf); everything else -- plan, write, verify -- is the one
// path C9 shipped.
//
// Exit code is the verdict and stderr is the machine-readable detail -- the
// shape the other verbs already use. NO message box here on purpose: Explorer
// may invoke these verbs for a whole multi-selection, and a dialog per file
// would hang a batch that nobody is watching.
int RunCompressLaunch(const prism::app::LaunchRequest& request) {
  uint64_t lastPercent = 1000;
  const prism::app::CompressVerbOutcome outcome = prism::app::RunCompressVerb(
      request, [&lastPercent](uint64_t done, uint64_t total) {
        // The engine's completed counter can pass the INPUT total for zip (it
        // counts container bytes: headers, padding). A command line that prints
        // "compress 107%" looks broken, so the rendered percentage is clamped;
        // the raw byte pair stays visible for diagnosis.
        const uint64_t raw = total == 0 ? 0 : (done * 100) / total;
        const uint64_t percent = raw > 100 ? 100 : raw;
        if (percent != lastPercent) {
          lastPercent = percent;
          std::fprintf(stderr, "compress %llu%% (%llu/%llu bytes)\n",
                       static_cast<unsigned long long>(percent),
                       static_cast<unsigned long long>(done),
                       static_cast<unsigned long long>(total));
        }
        return true;
      });
  for (const std::string& line : outcome.lines) std::fprintf(stderr, "%s\n", line.c_str());
  return outcome.exitCode;
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
                             prism::win::SevenZipLibrary& library,
                             const prism::i18n::Translator* translator,
                             const std::wstring& title) {
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
  // C88: the gate reads the same tables as the rest of the window. The
  // translator must be installed BEFORE Create -- the labels are created from
  // it (SetTranslator refreshes an already-open dialog too, for completeness).
  dialog.SetTranslator(translator);
  // C84: Create shows + centers the gate; FocusInput is the modal half (a
  // visible dialog with no foreground/keyboard still looks hung).
  if (!dialog.Create(instance, title.empty() ? L"PrismArchive" : title)) return std::wstring();
  // C84: Start() is what makes the password field editable (canSubmit), so the
  // focus call belongs after it -- focusing a disabled edit is a silent no-op.
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
  // C84: the gate's visible half is Create(); its USABLE half is foreground +
  // keyboard on the field, so the user can type instead of clicking first.
  dialog.FocusInput();

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
  // C76: the startup timeline. Begin() is the FIRST thing this process does in
  // our code, so every later span is measured on one clock and the gap between
  // any two stages is arithmetic, not a guess. Off unless PRISM_C76_TRACE=1.
  prism::win::startup::Trace& c76 = prism::win::startup::Trace::Instance();
  c76.Begin();
  double c76T = c76.NowMs();

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
  // C76 segment: apartment + command line + verb parse.
  c76.Add("com_argv", "CoInitializeEx+CommandLineToArgvW+ParseLaunch", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // C82: fold --password-file into the resolved password BEFORE any verb runs.
  // ParseLaunch consumed the flag's value, so the file's PATH can never leak
  // into the archive list; the file's CONTENT is the credential. An unreadable
  // file is a launch refusal on stderr -- one line, exit 2, no dialog.
  if (!launch.passwordFile.empty()) {
    std::wstring fromFile;
    if (!ReadPasswordFileUtf8(launch.passwordFile, &fromFile)) {
      std::fprintf(stderr, "password-file: cannot read \"%s\" (expected one UTF-8 line)\n",
                   prism::win::WideToUtf8(launch.passwordFile).c_str());
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 2;
    }
    if (launch.password.empty()) launch.password = fromFile;
  }
  // C82 negative control (CONTRACTS D3/D27): the PRE-C82 defect, injected at
  // runtime. With the variable set, a CLI verb is treated like the gallery, so
  // "--test enc.7z" brings up the modal password gate and the process hangs --
  // exactly the hang this card removes. The mutant test demands that hang: the
  // process must be killed at the bound and the case must go red.
  wchar_t cliModalMutant[64]{};
  const bool mutantCliModal = GetEnvironmentVariableW(L"PRISM_C82_MUTANT_CLI_MODAL",
                                                      cliModalMutant,
                                                      ARRAYSIZE(cliModalMutant)) > 0;

  // ---- persistence + language, BEFORE any user-facing string --------------
  // C14: one load at startup. A corrupt document is backed up by the file layer
  // (never silently overwritten) and the store reports it; a document from a
  // newer build is READ-ONLY, which SaveSettings honours at exit.
  double c76Sub = c76.NowMs();
  prism::app::SettingsBootstrap settings = prism::app::LoadSettings();
  c76.Add("  .load_settings", "LoadSettings (JSON + paths)", c76Sub, c76.NowMs() - c76Sub);
  // C16: the logger exists before anything can fail, so the reason is on stderr
  // instead of only in a message box nobody can copy.
  //
  // C85: and it now has a CLOCK and a FILE. The shipped window wrote only to
  // stderr, from a GUI process with no console (so nobody saw it) and with no
  // timestamp (so every line read 1970-01-01T00:00:00.000Z). A user reporting a
  // failure had nothing to attach; %LOCALAPPDATA%\PrismArchive\logs held no
  // file at all.
  prism::log::Logger logger(prism::log::Level::kInfo, 512);
  logger.SetClock(prism::win::logfile::SystemNowMillis);
  // The file half. Declared static so it is still open while the logger's last
  // record is written (the logger outlives this scope's locals by declaration
  // order, and a sink that closed first would drop that line).
  static prism::win::logfile::DailyFileSink logFile(
      prism::win::logfile::LogDirectory(), prism::win::logfile::Policy{},
      prism::win::logfile::SystemNowMillis);
  logger.SetSink([](const std::string& line) {
    // ONE line, two destinations: the file is what a user can attach to a bug
    // report, stderr is what a developer sees from a console run. Neither is
    // allowed to throw the other away.
    logFile.Write(line);
    std::fprintf(stderr, "%s\n", line.c_str());
  });
  // C85: ONE record that says "this build really ran". A log file that exists
  // but carries no line from the run being reported is as useless as no file.
  logger.Emit(prism::log::Level::kInfo, "startup", "app started",
              {{"pid", std::to_string(GetCurrentProcessId())},
               {"mode", prism::app::LaunchModeName(launch.mode)},
               {"archives", std::to_string(launch.archives.size())},
               {"log_dir", prism::win::logfile::NarrowPath(
                               std::filesystem::path(prism::win::logfile::LogDirectory()))}});
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
  c76Sub = c76.NowMs();
  LanguageRuntime language = LoadLanguage(settings.store, &logger);
  c76.Add("  .load_language", "LoadLanguage (Assets\Lang catalogs)", c76Sub, c76.NowMs() - c76Sub);
  const Tr tr{&language.translator};

  // C71: appearance.accentColor / appearance.glassIntensity become the live
  // theme overrides BEFORE the first frame. Anything painted later (action
  // island, focus ring, progress capsule, the glass cbuffer) reads them.
  prism::appearance::Apply(settings.store);

  // ---- C46: the previous session, read BEFORE anything opens --------------
  // The session is a SEPARATE document from settings on purpose: settings are
  // the user's preferences (merge on write, newer documents are read-only),
  // while a session is a snapshot of one run (rewritten every start and exit).
  // One corrupt session must therefore never cost the user their settings.
  c76Sub = c76.NowMs();
  prism::app::SessionBootstrap session = prism::app::LoadSession();
  c76.Add("  .load_session", "LoadSession", c76Sub, c76.NowMs() - c76Sub);
  if (!session.detail.empty()) {
    logger.Emit(prism::log::Level::kWarn, "session", "session could not be read",
                {{"detail", session.detail},
                 {"path", prism::win::WideToUtf8(session.file)}});
  }
  // C27's persisted view state is loaded here (not at the window call site) so
  // C46 can record the same view mode the session opens with.
  c76Sub = c76.NowMs();
  const prism::app::WindowState windowState = prism::app::LoadWindowState(settings.store);
  c76.Add("  .window_state", "LoadWindowState", c76Sub, c76.NowMs() - c76Sub);
  for (const std::string& warning : session.report.warnings) {
    // One warn line per repair/discard. A v99 document from a newer build, a
    // truncated file and a migrated v1 all say what happened on stderr.
    logger.Emit(prism::log::Level::kWarn, "session", warning,
                {{"status", prism::session::SessionLoadStatusName(session.report.status)},
                 {"path", prism::win::WideToUtf8(session.file)}});
  }

  // C76 segment: settings document + language tables + session document.
  c76.Add("settings_language_session", "LoadSettings+LoadLanguage+LoadSession", c76T,
          c76.NowMs() - c76T);
  c76T = c76.NowMs();

  if (launch.unknownFlag) {
    // C58: stderr + exit 2, NEVER a modal box (a batch caller must not hang).
    ReportRefusedVerb(launch);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 2;
  }
  // C9 + C58: archive creation has no archive to open -- dispatch it before the
  // open/picker logic. The three creation verbs share this one door; the format
  // is the verb's (--compress-here/7z -> .7z, --compress-zip -> .zip).
  if (prism::app::Compresses(launch.mode)) {
    const int code = RunCompressLaunch(launch);
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
      // C82: stderr, not a box -- the verb is headless by design.
      std::fprintf(stderr, "verb refused: missing archive path\n");
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 2;
    }
  }

  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  if (lookup.path.empty()) {
    const std::wstring message = DescribeMissing7zDll(lookup, tr);
    if (launch.mode != prism::app::LaunchMode::kGallery) {
      // C82: the headless verb says why on stderr and leaves.
      std::fprintf(stderr, "7z.dll: %s\n", prism::win::WideToUtf8(message).c_str());
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 1;
    }
    MessageBoxW(nullptr, message.c_str(), L"PrismArchive", MB_ICONERROR);
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  const std::wstring& dll = lookup.path;

  auto app = std::make_shared<AppContext>();
  app->extractOptions = LiveExtractionOptions(settings.store);
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

  // C76 segment: 7z.dll resolution + LoadLibrary.
  c76.Add("engine_load", "Resolve7zDll+SevenZipLibrary::Load", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // C58: --smart-extract-each <archives...>. Each archive is opened, planned and
  // extracted into its OWN folder (the --extract-sub plan), with one handler per
  // archive. Dispatched BEFORE the session logic: a batch verb is not a window
  // session, and it must not rewrite the user's restore list. The verdict is
  // stderr + exit code, never a dialog.
  if (launch.mode == prism::app::LaunchMode::kSmartExtractEach) {
    const prism::app::ExtractEachOutcome outcome = prism::app::RunExtractEach(
        launch.archives, app->library, /*scanExecutables=*/true,
        prism::app::ExtractionOptionsFromSettings(settings.store), launch.password);
    for (const std::string& line : outcome.lines) std::fprintf(stderr, "%s\n", line.c_str());
    if (SUCCEEDED(coInit)) CoUninitialize();
    return outcome.exitCode;
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
  // C82: the gallery is the ONLY mode allowed to ask. Every other verb runs
  // headless by design (Explorer / batch scripts), and a modal there hangs the
  // caller (C58 measured it: still alive after 12 s). mutantCliModal (D3) puts
  // the pre-C82 shape back: CLI treated as gallery, gate up, hang -- the mutant
  // test must catch that.
  const bool interactiveOpen =
      (launch.mode == prism::app::LaunchMode::kGallery) && !mutantCliModal;
  {
    std::string trace;  // the engine trace is narrow; only the message is wide
    // C85: the machine REASON and its HRESULT travel with the trace. Without
    // them the dialog could only say "CreateFileW failed; " -- the semicolon the
    // user reported, with nothing after it.
    std::string openReason;
    unsigned long openFailureHr = 0;
    bool opened = false;
    {
      prism::win::ArchiveReader probe(app->library);
      opened = probe.Open(archive, launch.password);
      if (opened) {
        app->catalog = probe.ReadCatalog();
        // C82: the command line's password PROVED itself; the handler pool
        // below must decrypt with the same secret (the C40 join).
        archivePassword = launch.password;
      } else {
        trace = probe.openTrace();
        openReason = probe.failureReason();
        openFailureHr = probe.failureHr();
      }
    }
    // C40: the password gate. A header-encrypted archive makes our own open
    // callback answer E_ABORT (C5's measured truth table), which is the one
    // signal that says "ask the user" rather than "the file is broken".
    if (!opened && prism::app::OpenNeedsPassword(trace)) {
      if (!interactiveOpen && !mutantCliModal) {
        // C82: headless verbs never open a modal. A password on the command
        // line (--password / --password-file) is tried ONCE below; anything
        // else is a one-line refusal and the exit code is the verdict.
        if (launch.password.empty()) {
          std::fprintf(stderr,
                       "encrypted: password required (supply via --password or "
                       "--password-file)\n");
          if (SUCCEEDED(coInit)) CoUninitialize();
          return 2;
        }
        archivePassword = launch.password;
      } else {
        // The gallery (or, under the D3 injection, the poor CLI verb again):
        // the modal gate is the RIGHT interface for an interactive open.
        archivePassword = RunPasswordGate(instance, archive, app->library,
                                          &language.translator, tr("password.title"));
      }
      if (!archivePassword.empty()) {
        // A fresh reader: the failed one already holds a stream and a partial
        // archive object, and re-Open()ing it would leak both.
        prism::win::ArchiveReader retry(app->library);
        opened = retry.Open(archive, archivePassword);
        if (opened) {
          app->catalog = retry.ReadCatalog();
        } else {
          trace = retry.openTrace();
          openReason = retry.failureReason();
          openFailureHr = retry.failureHr();
        }
      }
    }
    if (!opened) {
      // C85: ONE sentence builder for both surfaces (dialog and stderr), so the
      // reason a script reads is the reason a user sees.
      const std::wstring failureText = prism::app::OpenFailureText(
          tr, FileNameWide(archive), openReason, openFailureHr, trace);
      logger.Emit(prism::log::Level::kError, "open", "archive could not be opened",
                  {{"path", prism::win::WideToUtf8(archive)},
                   {"reason", openReason.empty() ? std::string("(none)") : openReason},
                   {"hr", prism::win::HresultText(openFailureHr)},
                   {"trace", trace}});
      if (!interactiveOpen) {
        // C82: same rule for a plain failed open -- the reason goes to stderr,
        // the exit code carries the verdict, and no box waits for a click that
        // a batch caller will never make.
        std::fprintf(stderr, "open failed: %s\n",
                     prism::win::WideToUtf8(failureText).c_str());
        if (SUCCEEDED(coInit)) CoUninitialize();
        return 1;
      }
      MessageBoxW(nullptr, failureText.c_str(), L"PrismArchive", MB_ICONERROR);
      if (SUCCEEDED(coInit)) CoUninitialize();
      return 1;
    }
  }
  app->items = app->catalog;
  // C76 segment: probe handler create + Open + ReadCatalog (the zero-payload scan).
  c76.Add("archive_open_catalog", "ArchiveReader::Open+ReadCatalog", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // ---- C46: this process now OWNS the session ------------------------------
  // Arm the crash guard and publish the document. From here on a killed process
  // leaves "cleanShutdown": false on disk, which is what makes the next start
  // restore nothing instead of reopening dead tabs.
  //
  // C76: the ARM is still decided here (RecordOpen needs the same view mode the
  // window is about to open with), but the WRITE -- one atomic JSON file, 26.1 ms
  // measured -- happens after the first frame, from the window's own first-frame
  // boundary. The window between "ready to write" and "written" is the first
  // frame, which is the one interval in which a crash is already unobservable to
  // the user; docs/PERF.md records that trade explicitly.
  auto armSession = [&]() {
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
  };  // C76: defined here, invoked after the first frame (see SetFirstFrameHandler)
  c76T = c76.NowMs();

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

  // C76 segment: engine handler pool + the extraction plan.
  c76.Add("ops_plan", "OperationHandles::Create+BuildExtractionPlan", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  prism::win::gallery::GalleryWindow window;
  if (!window.Create(L"PrismArchive -- " + FileNameWide(archive), instance)) {
    app->ops->Close();
    if (SUCCEEDED(coInit)) CoUninitialize();
    return 1;
  }
  // C83: the shell follows the saved appearance BEFORE the first frame. The
  // mapping is pure (prism::chrome::ShellVariantFor): "dark" and the schema's
  // "system" default paint the Midnight shell the canvas already uses, "light"
  // paints the daylight one, and appearance.highContrast wins over both. The
  // title bar and the owner-drawn menu bar both come from that one variant.
  window.SetChromeVariant(prism::chrome::ShellVariantFor(
      settings.store.GetString(prism::win::settingsui::kThemeKey, "system"),
      settings.store.GetBool(prism::win::settingsui::kHighContrastKey, false)));
  // C40 + C74: the sidebar's headings and the action band's eight words come
  // from the same tables as every other sentence. ONE door (ApplyLanguage) so a
  // language switch re-reads both, and a band that forgot to rebuild is the
  // defect the switch suite can see.
  window.ApplyLanguage(&language.translator);
  // C76: Create()'s own stages are measured above, so the clock is re-based
  // here -- a segment that contained another segment would be double-counted.
  c76T = c76.NowMs();

  // ---- C27: restored view state, sidebar port, menu bar -------------------
  // The geometry/mode/sort the user closed with is applied BEFORE the first
  // paint, so the window never flashes a layout it is about to replace.
  // C66: the saved number is the VISIBLE frame; restoring it goes through the
  // clamp so a value larger than the work area (the 1552x832 a maximized window
  // used to write on a 1536x816 screen) can never put the tail of the action
  // island off screen again. The stderr line is the observable the process-level
  // suite reads: it names the saved, the work area and the applied rectangles.
  if (windowState.width > 0 && windowState.height > 0) {
    // C86 (S3-2): the saved document now carries the ORIGIN and the maximized
    // bit as well as the size. A pre-C86 document has hasPosition == false, so
    // the window keeps the position it was created with and only the size is
    // applied -- the C66 behaviour, which is why an upgrade cannot drag the
    // window to the top-left corner.
    prism::layout::SavedWindow saved;
    saved.visible = prism::layout::Rect{windowState.x, windowState.y,
                                        windowState.x + windowState.width,
                                        windowState.y + windowState.height};
    saved.hasPosition = windowState.hasPosition;
    saved.maximized = windowState.maximized;
    const prism::win::frame::Restore restored = window.RestoreFrame(saved);
    // The observable the process-level suite reads: the saved (and clamped)
    // rectangles, the position included, and whether the mode was re-applied.
    std::fprintf(stderr,
                 "window restore saved=%d,%d %dx%d savedpos=%d work=%dx%d"
                 " requested=%d,%d %dx%d window=%d,%d %dx%d visible=%d,%d %dx%d"
                 " max=%d appmax=%d ok=%d\n",
                 restored.saved.left, restored.saved.top, restored.saved.Width(),
                 restored.saved.Height(), static_cast<int>(saved.hasPosition),
                 restored.work.Width(), restored.work.Height(), restored.requested.left,
                 restored.requested.top, restored.requested.Width(),
                 restored.requested.Height(), restored.applied.left, restored.applied.top,
                 restored.applied.Width(), restored.applied.Height(), restored.visible.left,
                 restored.visible.top, restored.visible.Width(), restored.visible.Height(),
                 static_cast<int>(saved.maximized), static_cast<int>(restored.maximized),
                 static_cast<int>(restored.ok));
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

  // C76 segment: frame restore + sidebar store + sort/columns/view mode.
  c76.Add("view_state", "RestoreFrame+AttachSidebarStore+SetListMode", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // ---- C34: the glass layer (C33) now has a call site ---------------------
  // The window owns the composition host; it is OPT-IN so a test-created window
  // never grows a DirectComposition target. A machine without composition says
  // so through the return value and the app keeps the pre-DComp paint path --
  // no glass memory is ever allocated there, so there is nothing to release.
  if (!window.EnableGlassLayer()) {
    logger.Emit(prism::log::Level::kInfo, "glass",
                "DirectComposition unavailable, glass layer stays off", {});
  }
  // C76: re-base after the glass host -- its stages have their own spans.
  c76T = c76.NowMs();

  // C19: the menu bar is the app's, painted by the window. The keys exist in
  // both shipped tables, so a language switch is visible here (the window's
  // read-back in the suite asserts exactly that).
  // C69: the words are a VALUE built in one place (AppWiring's BuildMenuLabels),
  // so the same call can run again when the Settings dialog changes the language.
  const prism::app::MenuLabels startupMenu = prism::app::BuildMenuLabels(tr, language.tag);
  prism::app::ApplyMenuLabels(window, startupMenu);
  // C76 segment: nine menu labels applied to the bar the window already owns.
  c76.Add("menu_labels", "BuildMenuLabels+ApplyMenuLabels", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

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

  // C76 segment: the catalog goes into the navigator (folder view + layout).
  c76.Add("set_items", "GalleryWindow::SetItems", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // ---- C60: the tab strip ------------------------------------------------
  //
  // ONE door for "show this archive": the strip posts C34's archive-changed
  // message, whose handler runs prism::app::SwitchArchive and then records the
  // tab. A tab click, Ctrl+Tab, a middle-click close and a menu open therefore
  // all end in the SAME switch -- the card's requirement 4, and the reason the
  // window owns no file handling of its own.
  //
  // An EMPTY path is the empty state (the last tab was closed): the window has
  // already cleared its rows and hidden the strip, and the app only says so.
  window.SetTabHandler([&window, tr](const std::string& utf8Path) {
    if (utf8Path.empty()) {
      window.SetStatus(tr.Or("sidebar.empty.noArchive", L"\u6CA1\u6709\u6253\u5F00\u7684\u5F52\u6863"));
      return;
    }
    window.PostArchiveChanged(prism::win::Utf8ToWide(utf8Path));
  });
  // The archive this launch opened is tab 0. OpenTab only tells the MODEL, so
  // it cannot re-enter the switch above.
  window.OpenTab(prism::win::WideToUtf8(archive), true);
  // ---- C64: every archive on the command line becomes a tab ---------------
  //
  // "PrismArchive.exe a.7z b.7z" opened only a.7z: C46 recorded b.7z in the
  // session document, so the user saw ONE tab and the next launch restored the
  // second archive as if it had been open. The FIRST argument stays active; the
  // rest are OPENED (a header read each) and added as tabs. A missing/damaged
  // extra argument is SKIPPED and counted -- never a message box per file (the
  // same contract the session restore already follows).
  int extraTabsSkipped = 0;
  for (std::size_t i = 1; i < launch.archives.size(); ++i) {
    const std::wstring& extra = launch.archives[i];
    if (prism::tabs::PathKey(prism::win::WideToUtf8(extra)) ==
        prism::tabs::PathKey(prism::win::WideToUtf8(archive))) {
      continue;  // duplicate argument: TabSet would dedupe it; do not probe twice
    }
    prism::win::ArchiveReader probe(app->library);
    if (!probe.Open(extra)) {
      ++extraTabsSkipped;
      logger.Emit(prism::log::Level::kInfo, "tabs",
                  "command-line archive skipped (missing or unreadable)",
                  {{"path", prism::win::WideToUtf8(extra)}});
      continue;
    }
    const prism::tabs::OpenOutcome opened =
        window.OpenTab(prism::win::WideToUtf8(extra), true, /*activate=*/false);
    if (opened.verdict != prism::tabs::TabVerdict::kOk) ++extraTabsSkipped;
  }
  std::fprintf(stderr, "tabs opened=%zu skipped=%d active=%s\n", window.Tabs().size(),
               extraTabsSkipped,
               window.Tabs().active() != nullptr ? window.Tabs().active()->path.c_str() : "");

  // C76 segment: tab strip + the tab handler.
  c76.Add("tabs", "OpenTab+SetTabHandler", c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

  // ---- C31/V3 + C53: the Settings... menu item and its modal gate ----------
  //
  // The window already owns the menu bar (C27's File menu), so it appends the
  // item itself in RebuildMenu; the V3 patch's SettingsDialog::InstallMenu
  // replaces the WHOLE bar, which would erase File > Compress/Exit. InstallMenu
  // is used only when no menu exists, so the delivered helper is still wired.
  // C64: the item's word is the app's (menu.settings), like every other title.
  const std::wstring settingsMenuText = startupMenu.settings;
  if (GetMenu(window.Hwnd()) == nullptr) {
    prism::win::settingsui::SettingsDialog::InstallMenu(window.Hwnd(), settingsMenuText);
  }
  // The dialog must read and write the document THIS launch loaded (a portable
  // build puts it beside the exe, not in %LOCALAPPDATA%), and a save must not
  // leave the app's in-memory copy stale for the exit save to overwrite.
  // C69: the translator the launch resolved, held by reference for the launcher
  // below (the dialog reads the SAME table the bar does).
  const prism::i18n::Translator& startupTranslator = language.translator;
  window.SetSettingsLauncher([&window, &settings, &app, &logger, &startupTranslator]() {
    prism::win::settingsui::SettingsDialog dialog;
    dialog.SetPaths(settings.paths);
    dialog.SetTranslator(&startupTranslator);   // C69: the rows read the same table
    // C69: a language picked in the OPEN dialog takes effect on the bar before
    // the user presses Save -- and the translator it will keep reading lives for
    // the whole modal call, not just for the handler.
    prism::app::LanguageRuntime liveLanguage;
    prism::app::Tr liveTr{&liveLanguage.translator};
    dialog.SetLanguageHandler([&](const std::string& tag) {
      prism::settings::SettingsStore staged = settings.store;
      staged.SetString(prism::win::settingsui::kLanguageKey, tag);
      liveLanguage = prism::app::LoadLanguageFor(LanguageDirectory(), staged,
                                                 SystemLocaleUtf8(), &logger);
      liveTr.translator = &liveLanguage.translator;
      prism::app::ApplyMenuLabels(
          window, prism::app::BuildMenuLabels(liveTr, prism::win::Utf8ToWide(tag)));
      // C86 (S3-4): the item count is no longer pushed from here. The window
      // derives it from the folder it is showing, so it cannot go stale on
      // navigation -- and it carries the archive total in parentheses.
      // C74: the action band and the sidebar's info card read the same table as
      // the menu, so the language picked in the dialog reaches every one of them
      // before the user presses Save.
      window.ApplyLanguage(liveTr.translator);
      dialog.SetTranslator(liveTr.translator);
    });
    std::string loadError;
    if (!dialog.LoadFrom(settings.paths, &loadError)) {
      window.SetStatus(L"\u8BBE\u7F6E\u8BFB\u53D6\u5931\u8D25\uFF1A" +
                       prism::win::Utf8ToWide(loadError));
    }
    dialog.OpenModal(GetModuleHandleW(nullptr), window.Hwnd());  // the modal gate
    if (dialog.saved()) {
      settings.report = prism::win::settingsfile::LoadStore(settings.store, settings.paths);
      settings.loaded =
          settings.report.outcome != prism::win::settingsfile::LoadOutcome::Failed;
      // C71: a saved accent/intensity takes effect on the NEXT PAINT, and a
      // saved overwrite/verify policy on the next extraction. Both are re-read
      // from the document the dialog just wrote (never from the draft).
      prism::appearance::Apply(settings.store);
      // C83: a Day/Night/HighContrast choice repaints the SHELL on the next
      // frame: the caption (DWM) and the owner-drawn menu bar are re-applied
      // from the document the dialog just wrote.
      window.SetChromeVariant(prism::chrome::ShellVariantFor(
          settings.store.GetString(prism::win::settingsui::kThemeKey, "system"),
          settings.store.GetBool(prism::win::settingsui::kHighContrastKey, false)));
      app->extractOptions = LiveExtractionOptions(settings.store);
      InvalidateRect(window.Hwnd(), nullptr, FALSE);
      window.SetStatus(L"\u8BBE\u7F6E\u5DF2\u4FDD\u5B58");
    }
  });

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
  // C76 segment: the settings menu item + the launcher closure + the restored
  // folder walk (ActivateRow per segment).
  c76.Add("settings_menu_session_view", "InstallMenu+SetSettingsLauncher+ApplySessionView",
          c76T, c76.NowMs() - c76T);
  c76T = c76.NowMs();

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
  std::function<void(int)> activateHandler =
      [&window, app, &extractWorker, extractHeadline](int catalogIndex) {
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
        // C87: the HUD's percent is MEASURED. The engine's write stream reports
        // uncompressed bytes as they land; the worker only posts when the
        // integer percent moves, so a 30 MB run is ~100 messages, not one per
        // 64 KiB chunk. Without this the capsule sat indeterminate (or at the
        // final "files written / catalog size" ratio) for the whole extraction.
        int lastPostedPercent = -1;
        auto reportProgress = [&window, &lastPostedPercent](uint64_t done, uint64_t total) {
          if (total == 0) return;
          const int percent = static_cast<int>((done * 100ull) / total);
          if (percent == lastPostedPercent) return;
          lastPostedPercent = percent;
          window.PostProgress(done, total);
        };
        bool ok = task->reader->ExtractWithPlan(
            app->catalog, app->plan.plan, app->plan.plan.baseTargetDir, nullptr, &result,
            app->extractOptions.overwrite, app->extractOptions.prompt,
            app->extractOptions.exists, reportProgress);
        app->extractReader.store(nullptr);
        app->ops->Finish(*task, ok);
        outcome.ok = ok;
        outcome.filesWritten = result.filesWritten;
        outcome.bytesWritten = result.bytesWritten;
        outcome.skippedFiles = result.skippedFiles;
        if (ok) {
          outcome.security = prism::app::ApplySecurityPass(
              app->archivePath, result.writtenPaths, /*scanExecutables=*/true);
          // C71: the SAME C13 verification the headless path runs.
          if (app->extractOptions.verifyAfterExtract) {
            const prism::app::VerifyOutcome verified = prism::app::VerifyIntegrity(
                *app->ops, app->catalog, app->archivePath);
            outcome.verified = true;
            outcome.verifyFailed = !verified.ok;
            if (!verified.ok) {
              ok = false;
              outcome.ok = false;
              outcome.error = "verification after extraction failed: " + verified.error;
            }
          }
        }
        outcome.status =
            prism::app::detail::BuildStatusText(outcome, ok, app->plan.plan.baseTargetDir);
        if (outcome.verifyFailed) {
          outcome.status += L" | \u6821\u9A8C\u5931\u8D25";
        }
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
      // C87: and the HUD is TOLD the operation ended. This message is the whole
      // "frozen at 100%" fix: before it, nothing ever left the HUD's live state.
      window.PostHudFinish(outcome.ok ? prism::HudOutcome::Success
                                      : prism::HudOutcome::Failed);
      Telemetry("extract", outcome);
      std::fprintf(stderr, "extract wall=%llums\n",
                   static_cast<unsigned long long>(GalleryWindow::NowMs() - t0));
      app->extracting = false;
      CoUninitialize();
    });
  };
  window.SetActivateHandler(activateHandler);

  // ---- C67: Alt+double-click = "extract into <archive name>\" --------------
  //
  // The gesture's DECISION is PrismCore's (prism::keys::ResolvePointer: Alt + a
  // double click is SmartExtract, a bare one is the ordinary open). The app owns
  // the meaning, and it is deliberately the SAME plan the --extract-sub verb
  // builds (C25's BuildExtractionPlan), so the two can never drift.
  window.SetSmartExtractHandler([app, &activateHandler](int /*catalogIndex*/) {
    app->plan = prism::app::BuildExtractionPlan(
        prism::app::LaunchMode::kExtractSub, prism::win::WideToUtf8(app->archivePath),
        app->catalog,
        [](const std::string& path) {
          return std::filesystem::exists(prism::win::Utf8ToWide(path));
        });
    std::fprintf(stderr, "gesture alt-dblclick mode=%s target=%s\n",
                 prism::app::LaunchModeName(app->plan.mode),
                 app->plan.plan.baseTargetDir.c_str());
    activateHandler(-1);  // the same worker, the same HUD, the same status line
  });

  // ---- C67: "extract to..." over a SELECTION -------------------------------
  //
  // C67's third behaviour: with more than one row selected the band's
  // ExtractTo must extract EXACTLY those entries (the count is asserted by
  // win.c67_gestures). One row / no row keeps the whole-archive plan the
  // double-click and the shell verbs use.
  std::thread selectedWorker;
  auto runSelectedExtraction = [&window, app, &selectedWorker, extractHeadline](
                                   std::vector<uint32_t> selection) {
    if (app->extracting.exchange(true)) return;
    if (selectedWorker.joinable()) selectedWorker.join();
    selectedWorker = std::thread([app, &window, extractHeadline, selection]() {
      CoInitializeEx(nullptr, COINIT_MULTITHREADED);
      window.PostHudStart(extractHeadline);
      const prism::app::ExtractionOutcome outcome = prism::app::RunSelectionExtraction(
          *app->ops, app->catalog, selection, app->plan.plan.baseTargetDir, app->archivePath,
          /*scanExecutables=*/true, app->extractOptions);
      window.PostProgress(outcome.filesWritten, outcome.filesWritten);
      window.PostStatus(outcome.status);
      window.PostHudFinish(outcome.ok ? prism::HudOutcome::Success
                                      : prism::HudOutcome::Failed);
      Telemetry("extract-selection", outcome);
      std::fprintf(stderr, "extract-selection requested=%zu files=%llu ok=%d\n", selection.size(),
                   static_cast<unsigned long long>(outcome.filesWritten),
                   static_cast<int>(outcome.ok));
      app->extracting = false;
      CoUninitialize();
    });
  };

  // ---- C67: drag OUT of the window ----------------------------------------
  //
  // The window reports "the user dragged these catalog entries" only after the
  // press travelled past Explorer's own threshold, and the payload is produced
  // on the FIRST GetData(CF_HDROP) -- a hovering target costs nothing. The temp
  // tree is removed when DoDragDrop returns, i.e. on drop AND on cancel.
  window.SetDragOutHandler([app, archivePassword, &window](
                               HWND owner, const std::vector<uint32_t>& selection) {
    namespace dragout = prism::win::dragoutfiles;
    std::error_code ec;
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
    if (ec) {
      window.SetStatus(L"\u62D6\u51FA\u5931\u8D25\uFF1A\u6CA1\u6709\u4E34\u65F6\u76EE\u5F55");
      return false;
    }
    std::string why;
    auto drag = dragout::FileDragOut::Create(tempRoot.wstring(), dragout::FileDragOut::Extractor{},
                                             &why);
    if (!drag) {
      window.SetStatus(L"\u62D6\u51FA\u5931\u8D25\uFF1A" + prism::win::Utf8ToWide(why));
      return false;
    }
    const std::wstring dir = drag->temp_dir();
    drag->SetExtractor([app, archivePassword, selection, dir](dragout::MaterializeResult* out) {
      const prism::app::DragOutMaterialization produced =
          prism::app::MaterializeSelectionForDragOut(app->library, app->archivePath,
                                                     archivePassword, app->catalog, selection, dir);
      out->ok = produced.ok;
      out->error = produced.error;
      out->paths = produced.paths;
      return produced.ok;
    });
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = drag->Run(owner, DROPEFFECT_COPY, &effect);
    std::fprintf(stderr,
                 "dragout hr=0x%08lX effect=%lu requested=%zu files=%zu extracted=%zu temp=%d\n",
                 static_cast<unsigned long>(hr), static_cast<unsigned long>(effect),
                 selection.size(), drag->files(), drag->extractions(),
                 static_cast<int>(drag->temp_dir_exists()));
    if (drag->files() > 0) {
      window.SetStatus(L"\u5DF2\u62D6\u51FA " + std::to_wstring(drag->files()) +
                       L" \u4E2A\u6587\u4EF6");  // 已拖出 N 个文件
    } else if (hr == DRAGDROP_S_CANCEL) {
      window.SetStatus(L"\u5DF2\u53D6\u6D88\u62D6\u51FA");  // 已取消拖出
    }
    return drag->files() > 0;
  });

  // ---- C52 + C53: a drop ONTO the window -----------------------------------
  //
  // The window turned Explorer's HDROP into a tree and asked PrismCore what it
  // means (prism::drop::PlanDrop). C52 shipped no executor, so a user saw the
  // hint and then nothing; the executor now lives in AppWiring (one pure
  // DecideDrop + this install call), which is also the function the Windows
  // suite drives. Contract, in one sentence: a drop always answers, and a drop
  // that cannot be executed says so instead of pretending.
  //
  // AddToArchive is REFUSED EXPLICITLY: ArchiveWriter has no incremental
  // "append these files to this archive" operation (it writes a NEW archive from
  // a source root), so "Added N files" would be a lie and a half-write would be
  // invisible. The status line says "暂不支持追加" and nothing is written; the
  // suite asserts the bytes did not move. ExtractHere/TestArchive still need the
  // C4 flow callable with an explicit file list -- they report, they do not run.
  // OpenArchive reuses the archive-changed flow (the same door the re-pack uses),
  // and Refuse carries the model's own reason.
  prism::app::dropin::InstallDropHandler(window);

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
            window.PostHudFinish(outcome.ok ? prism::HudOutcome::Success
                                            : prism::HudOutcome::Failed);
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
    // C84: the owner is what makes Create() center the card over the gallery
    // instead of leaving it at the screen's default corner.
    if (!compressDialog->Create(instance, compressHeadline, window.Hwnd())) {
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
            window.PostHudFinish(outcome.ok ? prism::HudOutcome::Success
                                            : prism::HudOutcome::Failed);
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
    // C64: was this message caused by a TAB CLICK (or Ctrl+Tab)? A background
    // re-pack posts the same message, and its failure must not move the strip.
    const bool fromTab = window.ConsumeTabSwitchIntent(prism::win::WideToUtf8(newPath));
    if (app->extractReader.load() != nullptr || app->sweepReader.load() != nullptr) {
      // C40: the sentence (and its real-Chinese fallback) lives in AppWiring so
      // the suite can assert its bytes; C38's audit found it double-escaped.
      if (fromTab) (void)window.RefuseTabSwitch(prism::win::WideToUtf8(newPath));
      window.SetStatus(prism::app::RefreshDeferredStatus(tr));
      return;
    }
    prism::app::ArchiveSession archiveSession{&app->library, &app->ops, &app->catalog, &app->items,
                                              &app->archivePath, 2};
    // C64: SwitchArchive moves the session; ApplyArchiveSwitch republishes the
    // rows and the tab. Without the second half the strip says beta while the
    // grid still paints alpha (the defect this card closes).
    const prism::app::ArchiveSwitch switched =
        prism::app::ApplyArchiveSwitch(window, archiveSession, newPath);
    if (!switched.ok) {
      // The tab is already active in the model (the click activated it first).
      // Put it back: the user keeps the archive whose rows are still on screen,
      // and the readable reason names the file that could not be opened.
      if (fromTab) (void)window.RefuseTabSwitch(prism::win::WideToUtf8(newPath));
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
    // ApplyArchiveSwitch already republished the rows, the root label and the
    // tab (C64) -- doing it twice would reset the navigator's folder after the
    // switch had just landed.
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
    // C86 (S3-4): the count follows the folder, published by the window itself
    // (this line used to write the ARCHIVE's total into the breadcrumb).
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

  // ---- C64: the action island's missing wire -------------------------------
  //
  // The eight buttons (and their Alt chords) were painted, enabled and
  // clickable -- and every click ended at FireAction with nowhere to go,
  // because Main never called SetActionHandler. The window now refuses
  // silently? No: with no handler it logs "action unhandled" and sets a status.
  // This is the app's half, mapped onto the paths the rest of Main already
  // owns (no second implementation of extraction/compression/edit).
#if !defined(PRISM_C64_MUTANT_NO_ACTION_HANDLER)
  window.SetActionHandler([&window, app, tr, &activateHandler,
                           &runSelectedExtraction](prism::actionbar::ActionId action, int item) {
    std::fprintf(stderr, "action id=%s item=%d\n", prism::actionbar::ActionName(action), item);
    switch (action) {
      case prism::actionbar::ActionId::Open: {
        // The SAME picker the no-argument start uses; a pick goes through the
        // archive-changed door so the switch/tab/session rules stay in C34/C60.
        const std::wstring picked = PickArchive();
        if (!picked.empty()) window.PostArchiveChanged(picked);
        break;
      }
      case prism::actionbar::ActionId::ExtractTo: {
        // C67: a real multi-selection means "extract exactly these" (Bandizip's
        // habit); one row or none keeps the whole-archive plan. The window owns
        // the row -> catalog-index mapping, so the app never re-derives it.
        const std::vector<uint32_t> selected = window.SelectedCatalogIndices();
        if (selected.size() > 1) {
          std::fprintf(stderr, "action extract-to selection=%zu\n", selected.size());
          runSelectedExtraction(selected);
          break;
        }
        // The same worker double-click uses (whole-archive v1 plan). The
        // dropdown item index is passed through untouched.
        activateHandler(item);
        break;
      }
      case prism::actionbar::ActionId::Compress:
        // The File > Compress... menu item is the one C18/C27 entry.
        window.Dispatch(WM_COMMAND, GalleryWindow::kMenuCompress, 0);
        break;
      case prism::actionbar::ActionId::Add:
        // ArchiveWriter has no incremental append. Refuse with a reason instead
        // of pretending; the refusal is counted (stderr) and visible (status).
        std::fprintf(stderr, "action refused id=Add reason=no-incremental-write\n");
        window.SetStatus(L"\u8FFD\u52A0\u6682\u4E0D\u652F\u6301\uFF1A"
                         L"\u672C\u7248\u672C\u6CA1\u6709\u589E\u91CF\u5199\u5165");  // 追加暂不支持
        break;
      case prism::actionbar::ActionId::Delete:
        // C54's edit request (confirmation + exclusive gate) owns the write.
        window.Dispatch(WM_COMMAND, GalleryWindow::kMenuEditDelete, 0);
        break;
      case prism::actionbar::ActionId::Test: {
        window.SetStatus(L"\u6B63\u5728\u6821\u9A8C\u5F52\u6863\u2026");  // 正在校验归档…
        const prism::app::VerifyOutcome outcome =
            prism::app::VerifyIntegrity(*app->ops, app->catalog, app->archivePath);
        std::fprintf(stderr, "verify ok=%d items=%llu bytes=%llu failedIndex=%d %s\n",
                     static_cast<int>(outcome.ok),
                     static_cast<unsigned long long>(outcome.items),
                     static_cast<unsigned long long>(outcome.bytes),
                     static_cast<int>(outcome.failedIndex), outcome.error.c_str());
        window.SetStatus(outcome.ok
                             ? std::wstring(L"\u6821\u9A8C\u901A\u8FC7")  // 校验通过
                             : tr.Or("error.verify.failed", L"\u6821\u9A8C\u5931\u8D25\uFF1A") +
                                   prism::win::Utf8ToWide(outcome.error));
        break;
      }
      case prism::actionbar::ActionId::QuickLook:
        // Space is C48's one door; the window owns the overlay's lifetime.
        window.Dispatch(WM_KEYDOWN, VK_SPACE, 0);
        break;
      case prism::actionbar::ActionId::Search:
        // C62's field IS wired now (C66 applied the patch); the button and
        // Ctrl+F must open the same thing, otherwise the toolbar advertises a
        // feature that only exists on the keyboard.
        window.OpenSearch();
        break;
      case prism::actionbar::ActionId::Count:
        break;
    }
  });
#endif

  // C86 (S3-4): the breadcrumb's count segment belongs to the window now (the
  // folder's own count, with the archive total in parentheses), so the app does
  // not write a second, file-wide number next to a folder name.

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
  //
  // C76: the thread is STARTED after the first frame (SetFirstFrameHandler
  // below), not before Run(). It competes for the same file cache and CPU as the
  // first paint, and its pixels cannot be on screen before the first frame
  // anyway -- deferring it is both faster to first paint and MORE deterministic
  // (the first frame never contains a thumbnail that happened to arrive).
  std::thread worker;
  auto startSweep = [&]() {
  // The HWND is read HERE, on the UI thread (this handler runs from WM_PAINT),
  // and travels by VALUE into the worker: a worker that asked the window for its
  // handle would be touching UI state off the UI thread (CONTRACTS B9).
  const HWND sweepTarget = window.Hwnd();
  worker = std::thread([app, &window, sweepIndices, sweepSizes, sweepTarget]() {
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
          // B9: post, never call. The UI thread takes the store in WndProc.
          window.PostThumbs(store, sweepTarget);
        } else {
          std::fprintf(stderr, "sweep discarded: the session switched archives\n");
        }
      } else {
        std::fprintf(stderr, "sweep refused: %s\n", why.c_str());
      }
    }
    CoUninitialize();
  });
  };  // C76: startSweep (invoked after the first frame)

  // C76: ONE door for "the first frame is on screen". The session's crash-guard
  // write and the thumbnail sweep both move behind it; neither changes a pixel
  // of frame 1.
  window.SetFirstFrameHandler([&]() {
    armSession();
    startSweep();
  });

  // C76 segment: the rest of the wiring (handlers, drag out, quick look,
  // companion preview).
  c76.Add("wiring_tail", "handler wiring + companion preview", c76T, c76.NowMs() - c76T);

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
  if (worker.joinable()) worker.join();  // C76: the sweep may never have started
  if (extractWorker.joinable()) extractWorker.join();
  if (selectedWorker.joinable()) selectedWorker.join();
  if (compressWorker.joinable()) compressWorker.join();
  if (editWorker.joinable()) editWorker.join();
  // The dialog outlives Run() only when the user closed the main window while it
  // was open; its destructor destroys the HWND, which is safe after the loop.
  compressDialog.reset();

  // ---- C27: persist the session (C14) -------------------------------------
  // Read the state the user ACTUALLY left: the frame size captured in WM_CLOSE
  // (the HWND is gone now), the view mode, the sort key and the column
  // geometry. Then the sidebar blob, then one atomic write of the document.
  prism::app::WindowState finalState;
  // C86: the WHOLE closed frame -- origin, size and the maximized bit. Only the
  // size used to travel, which is why settings.json never held an x or a y.
  const prism::layout::SavedWindow closed = window.ClosedWindow();
  if (closed.visible.Width() > 0 && closed.visible.Height() > 0) {
    finalState.width = closed.visible.Width();
    finalState.height = closed.visible.Height();
    finalState.x = closed.visible.left;
    finalState.y = closed.visible.top;
    finalState.hasPosition = closed.hasPosition;
    finalState.maximized = closed.maximized;
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
