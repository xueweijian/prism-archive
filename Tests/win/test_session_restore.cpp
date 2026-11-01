// PrismArchive -- session restore, end to end (card C46).
//
// The session rules are pure (core.test_session_store), but the CARD's claim is
// about a process boundary: "close it and reopen it and my archives are back;
// crash it and they are not". That cannot be proven in-process, so this suite
// starts the REAL PrismArchive.exe, with %LOCALAPPDATA% pointed at a fixture
// directory (the injection C27's suite established), and drives it twice:
//
//   1. open two archives, WAIT until the session document is armed, KILL the
//      process, start again with no arguments  -> NOTHING may be restored (the
//      clean-shutdown mark is the guard, and a killed process never wrote it);
//   2. open two archives, close the window for real (WM_CLOSE), start again ->
//      BOTH entries come back, and the next clean exit leaves the document clean;
//   3. a session that names a missing archive and a damaged one restores only
//      the survivors (silently -- the process must reach its window and answer
//      WM_CLOSE, which a message box would have made impossible);
//   4. a corrupt session document is discarded and the process still starts.
//
// Everything the exe reports goes to stderr, redirected to a file by this suite:
// no pipes (a window station need not have a usable console handle) and no
// timing assertions -- readiness is "the line is there", not "500 ms elapsed"
// (CONTRACTS: assert bytes/counts, never a stopwatch).
//
// NEGATIVE CONTROL (CONTRACTS D3 + D26): the same suite is compiled a second
// time against a MUTANT exe whose clean-shutdown guard is compiled out. Case 1
// then sees the killed session restored, the suite goes red, and the binary
// prints "C46 MUTATION CAUGHT" -- the marker is the conjunction, no WILL_FAIL.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/SessionStore.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace session = prism::session;

#ifndef PRISM_C46_EXE
#error "C46 needs PRISM_C46_EXE (the exe under test) from the suite's CMake file"
#endif

namespace {

// The banner literal sits INSIDE the guard block: the C23 matrix looks for a
// "MUTANT ACTIVE" string right after the macro guard as proof the injection
// built (the arm's NAME is passed in as PRISM_C46_MUTANT_NAME, and deliberately
// avoids the words clean/control/unarmed, which the matrix reads as "this is a
// control" -- a control that is armed is a contradiction it rejects).
#if defined(PRISM_C46_MUTANT_NAME)
constexpr const char* kMutantBanner =
    "C46 MUTANT ACTIVE: the exe under test has the shutdown-mark guard compiled out";
constexpr const char* kMutant = PRISM_C46_MUTANT_NAME;
#else
constexpr const char* kMutantBanner = nullptr;
constexpr const char* kMutant = nullptr;
#endif

constexpr DWORD kReadyTimeoutMs = 60'000;
constexpr DWORD kExitTimeoutMs = 60'000;

std::string Narrow(const std::wstring& text) { return prism::win::WideToUtf8(text); }

// The exe under test arrives as a compile-time narrow literal (a target file
// path can contain spaces, so it is quoted by CMake rather than assembled).
std::wstring ExePath() { return prism::win::Utf8ToWide(PRISM_C46_EXE); }

struct Child {
  PROCESS_INFORMATION info{};
  HANDLE stderrFile = nullptr;
  bool started = false;
  bool exited = false;

  ~Child() {
    if (started && !exited) {
      TerminateProcess(info.hProcess, 0xC46);
      WaitForSingleObject(info.hProcess, 10'000);
    }
    if (info.hProcess != nullptr) CloseHandle(info.hProcess);
    if (info.hThread != nullptr) CloseHandle(info.hThread);
    if (stderrFile != nullptr) CloseHandle(stderrFile);
  }
};

bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// The child process, with %LOCALAPPDATA% injected and stderr redirected to a
// file. CREATE_UNICODE_ENVIRONMENT is required for a wide environment block.
// ---------------------------------------------------------------------------
bool StartChild(const std::wstring& exe, const std::vector<std::wstring>& args,
                const fs::path& sandbox, const fs::path& stderrPath, Child* child,
                std::string* why) {
  std::wstring command = L"\"" + exe + L"\"";
  for (const std::wstring& arg : args) command += L" \"" + arg + L"\"";

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  child->stderrFile = CreateFileW(stderrPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child->stderrFile == INVALID_HANDLE_VALUE) {
    child->stderrFile = nullptr;
    *why = "CreateFileW(stderr) failed: " + std::to_string(GetLastError());
    return false;
  }

  std::vector<wchar_t> environment;
  {
    LPWCH block = GetEnvironmentStringsW();
    const std::wstring overrideEntry = L"LOCALAPPDATA=" + sandbox.wstring();
    for (LPWCH cursor = block; *cursor != L'\0';) {
      const std::wstring entry(cursor);
      cursor += entry.size() + 1;
      if (entry.rfind(L"LOCALAPPDATA=", 0) == 0) continue;  // replaced below
      environment.insert(environment.end(), entry.begin(), entry.end());
      environment.push_back(L'\0');
    }
    FreeEnvironmentStringsW(block);
    environment.insert(environment.end(), overrideEntry.begin(), overrideEntry.end());
    environment.push_back(L'\0');
    environment.push_back(L'\0');
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  // The child is a GUI process: its CRT has no console, so ALL three standard
  // handles point at the same file (an inheritable handle, not a pipe).
  startup.hStdInput = child->stderrFile;
  startup.hStdOutput = child->stderrFile;
  startup.hStdError = child->stderrFile;

  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  const BOOL created =
      CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                     CREATE_UNICODE_ENVIRONMENT, environment.data(), sandbox.c_str(), &startup,
                     &child->info);
  if (!created) {
    *why = "CreateProcessW failed: " + std::to_string(GetLastError());
    return false;
  }
  child->started = true;
  return true;
}

// Readiness is a BYTE fact ("the line is in the file"), not a duration: the
// deadline only bounds how long we refuse to believe a silent process.
bool WaitForText(const fs::path& path, const std::string& needle, DWORD timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (Contains(ReadText(path), needle)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return Contains(ReadText(path), needle);
}

bool WaitForExit(Child* child, DWORD timeoutMs, DWORD* exitCode) {
  if (!child->started) return false;
  if (WaitForSingleObject(child->info.hProcess, timeoutMs) != WAIT_OBJECT_0) return false;
  child->exited = true;
  if (exitCode != nullptr) GetExitCodeProcess(child->info.hProcess, exitCode);
  return true;
}

void Kill(Child* child) {
  if (!child->started || child->exited) return;
  TerminateProcess(child->info.hProcess, 0xC46);  // SIGKILL, in Windows spelling
  WaitForSingleObject(child->info.hProcess, 30'000);
  child->exited = true;
}

struct FindWindowState {
  DWORD pid = 0;
  HWND window = nullptr;
};

BOOL CALLBACK FindWindowProc(HWND hwnd, LPARAM param) {
  auto* state = reinterpret_cast<FindWindowState*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != state->pid || !IsWindowVisible(hwnd)) return TRUE;
  wchar_t title[260]{};
  if (GetWindowTextW(hwnd, title, 260) <= 0) return TRUE;
  if (wcsncmp(title, L"PrismArchive --", 15) != 0) return TRUE;
  state->window = hwnd;
  return FALSE;
}

HWND WaitForMainWindow(DWORD pid, DWORD timeoutMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    FindWindowState state;
    state.pid = pid;
    EnumWindows(&FindWindowProc, reinterpret_cast<LPARAM>(&state));
    if (state.window != nullptr) return state.window;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return nullptr;
}

// The real close path a user takes: WM_CLOSE on the gallery window.
bool CloseMainWindow(DWORD pid) {
  const HWND window = WaitForMainWindow(pid, kReadyTimeoutMs);
  if (window == nullptr) return false;
  return PostMessageW(window, WM_CLOSE, 0, 0) != 0;
}

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------
prism::win::SevenZipLibrary& Library() {
  static prism::win::SevenZipLibrary library;
  return library;
}

bool EnsureEngine(std::string* why) {
  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  if (lookup.path.empty()) {
    *why = "7z.dll not found (set PRISM_7Z_DLL or install 7-Zip)";
    return false;
  }
  if (!Library().Load(lookup.path)) {
    *why = "7z.dll at " + Narrow(lookup.path) + " could not be loaded";
    return false;
  }
  return true;
}

std::vector<uint8_t> Payload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

// A real archive, created by the app's own compression path, with a real folder
// inside it ("docs") so the restored folder can be a NON-root one.
bool BuildArchive(const fs::path& archive, uint32_t seed, std::string* why) {
  const fs::path stage = archive.parent_path() / (archive.stem().wstring() + L"_src");
  std::error_code ec;
  fs::create_directories(stage / L"docs", ec);
  if (ec) {
    *why = "create_directories failed: " + ec.message();
    return false;
  }
  if (!WriteBytes(stage / L"payload.bin", Payload(seed, 8u * 1024u)) ||
      !WriteBytes(stage / L"docs" / L"notes.txt", Payload(seed + 1, 2048u))) {
    *why = "fixture payload write failed";
    return false;
  }
  const app::CompressionTarget target =
      app::PlanCompression({stage.wstring()}, prism::ArchiveFormat::SevenZip, archive.wstring());
  if (!target.ok) {
    *why = "plan: " + target.error;
    return false;
  }
  app::CompressionRequest job;
  job.archivePath = target.archivePath;
  job.sourceRoot = target.sourceRoot;
  job.plan = target.plan;
  const app::CompressionOutcome outcome = app::RunCompression(job, [](uint64_t, uint64_t) {
    return true;
  });
  if (!outcome.ok) {
    *why = "create: " + outcome.error;
    return false;
  }
  return true;
}

// Writes a session document the way the exe's own file layer does (atomic,
// %LOCALAPPDATA%\PrismArchive\session.json), so the suite seeds EXACTLY the
// shape the product reads.
bool SeedSession(const fs::path& sandbox, const std::vector<session::SessionEntry>& entries,
                 std::string* why) {
  app::SessionBootstrap bootstrap = app::LoadSessionUnder(sandbox.wstring());
  if (bootstrap.file.empty()) {
    *why = "session file path unresolved";
    return false;
  }
  bootstrap.store.Replace(entries, 0);
  bootstrap.store.MarkCleanShutdown(true);
  return app::SaveSession(&bootstrap, why);
}

bool SessionDocumentSays(const fs::path& sandbox, const std::string& needle) {
  return Contains(ReadText(sandbox / L"PrismArchive" / L"session.json"), needle);
}

size_t EntryCountInDocument(const fs::path& sandbox) {
  const std::string text = ReadText(sandbox / L"PrismArchive" / L"session.json");
  size_t count = 0;
  for (size_t at = text.find("\"path\":"); at != std::string::npos;
       at = text.find("\"path\":", at + 7)) {
    ++count;
  }
  return count;
}

}  // namespace

int main(int argc, char** argv) {
  if (kMutant != nullptr) {
    std::printf("%s [%s]\n", kMutantBanner, kMutant);
    std::printf("    exe: %s\n", PRISM_C46_EXE);  // narrow on purpose: a log line
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::printf("SKIP: CoInitializeEx failed (this window station has no COM)\n");
    return 77;
  }
  std::string why;
  if (!EnsureEngine(&why)) {
    std::printf("SKIP: %s\n", why.c_str());  // D1: code 77 maps to Skipped, marker still required
    CoUninitialize();
    return 77;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (kMutant != nullptr) {
    if (rc == 0) {
      std::printf("C46 MUTATION NOT CAUGHT: the suite stayed green against the mutant exe\n");
      return 1;
    }
    std::printf("C46 MUTATION CAUGHT: %s (see the FAIL lines above)\n", kMutant);
    return 0;
  }
  if (rc != 0) return rc;
  std::printf("ALL SESSION-RESTORE CHECKS PASSED\n");
  return 0;
}

// ---------------------------------------------------------------------------
// Case 1: KILL the process. The next start restores NOTHING.
// ---------------------------------------------------------------------------
PRISM_TEST("restore.killed_process_is_not_restored") {
  prism::testtmp::UniqueDir fixture(L"c46", L"killed");
  const fs::path root = fixture.path();
  const fs::path sandbox = root / L"lapp";
  const fs::path archives = root / L"archives";
  std::error_code ec;
  fs::create_directories(sandbox, ec);
  fs::create_directories(archives, ec);
  if (!PRISM_CHECK(!ec)) return;
  const fs::path first = archives / L"first.7z";
  const fs::path second = archives / L"second.7z";
  std::string why;
  if (!PRISM_CHECK(BuildArchive(first, 11, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(BuildArchive(second, 12, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }

  const fs::path runA = root / L"run-a.err";
  Child a;
  if (!PRISM_CHECK(StartChild(ExePath(), {first.wstring(), second.wstring()}, sandbox, runA, &a,
                              &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(WaitForText(runA, "session armed entries=2", kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(runA).c_str());
    return;
  }
  // The document on disk is the DIRTY one: two entries, cleanShutdown false.
  PRISM_CHECK(SessionDocumentSays(sandbox, "\"cleanShutdown\": false"));
  PRISM_CHECK_EQ(EntryCountInDocument(sandbox), size_t{2});
  PRISM_CHECK(SessionDocumentSays(sandbox, Narrow(first.filename().wstring())));
  PRISM_CHECK(SessionDocumentSays(sandbox, Narrow(second.filename().wstring())));
  Kill(&a);

  // Second start, no arguments: the crash guard must hand out zero entries.
  const fs::path runB = root / L"run-b.err";
  Child b;
  if (!PRISM_CHECK(StartChild(ExePath(), {}, sandbox, runB, &b, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  // entries=2 (the document still lists them) but tried=0: the guard refused.
  if (!PRISM_CHECK(WaitForText(runB, "session restore entries=2 tried=0 skipped=0 restored=0",
                              kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(runB).c_str());
    return;
  }
  // "clean=0" in that line is the guard's own report; assert the shape too.
  PRISM_CHECK(Contains(ReadText(runB), "clean=0"));
  PRISM_CHECK(!Contains(ReadText(runB), "session opened"));
  Kill(&b);
  // A crashed session stays crashed: the second process never claimed a clean
  // shutdown, so the document still says false.
  PRISM_CHECK(SessionDocumentSays(sandbox, "\"cleanShutdown\": false"));
}

// ---------------------------------------------------------------------------
// Case 2: a RECORDED restore of two archives, with the saved folder + view mode.
// ---------------------------------------------------------------------------
PRISM_TEST("restore.reopens_two_archives_with_their_folder_and_view") {
  prism::testtmp::UniqueDir fixture(L"c46", L"recorded");
  const fs::path root = fixture.path();
  const fs::path sandbox = root / L"lapp";
  const fs::path archives = root / L"archives";
  std::error_code ec;
  fs::create_directories(sandbox, ec);
  fs::create_directories(archives, ec);
  if (!PRISM_CHECK(!ec)) return;
  const fs::path first = archives / L"photos.7z";
  const fs::path second = archives / L"work.7z";
  std::string why;
  if (!PRISM_CHECK(BuildArchive(first, 21, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(BuildArchive(second, 22, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  // The session a previous CLEAN run would have left: the first archive was left
  // inside "docs" in list mode, the second at its root in the grid.
  // BuildArchive packs the source FOLDER, so the archive's own root holds it:
  // the place a user would be after drilling in once and then into "docs".
  const std::string firstFolder = Narrow(first.stem().wstring()) + "_src/docs";
  std::vector<session::SessionEntry> entries;
  entries.push_back(session::SessionEntry{Narrow(first.wstring()), firstFolder, true});
  entries.push_back(session::SessionEntry{Narrow(second.wstring()), "", false});
  if (!PRISM_CHECK(SeedSession(sandbox, entries, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }

  const fs::path run = root / L"restore.err";
  Child child;
  if (!PRISM_CHECK(StartChild(ExePath(), {}, sandbox, run, &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(WaitForText(run, "session restore entries=2 tried=2 skipped=0 restored=2",
                              kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  // The folder and the view mode reached the WINDOW, not just the document:
  // "folder=docs view=list restored=1" is printed after the restore walk.
  if (!PRISM_CHECK(WaitForText(run, "session opened", kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  const std::string opened = ReadText(run);
  PRISM_CHECK(Contains(opened, "folder=" + firstFolder));
  PRISM_CHECK(Contains(opened, "view=list"));
  PRISM_CHECK(Contains(opened, "restored=1"));

  // A real WM_CLOSE: the clean path. The document must end up clean + two entries.
  if (!PRISM_CHECK(CloseMainWindow(child.info.dwProcessId))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  DWORD exitCode = 1;
  if (!PRISM_CHECK(WaitForExit(&child, kExitTimeoutMs, &exitCode))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  PRISM_CHECK_EQ(exitCode, DWORD{0});
  PRISM_CHECK(WaitForText(run, "session saved", 5'000));
  PRISM_CHECK(SessionDocumentSays(sandbox, "\"cleanShutdown\": true"));
  PRISM_CHECK_EQ(EntryCountInDocument(sandbox), size_t{2});
}

// ---------------------------------------------------------------------------
// Case 3: missing / damaged archives are SKIPPED silently, and the process is
// still a normal app (a message box would have blocked the window and WM_CLOSE).
// ---------------------------------------------------------------------------
PRISM_TEST("restore.skips_missing_and_damaged_archives_without_dialogs") {
  prism::testtmp::UniqueDir fixture(L"c46", L"skip");
  const fs::path root = fixture.path();
  const fs::path sandbox = root / L"lapp";
  const fs::path archives = root / L"archives";
  std::error_code ec;
  fs::create_directories(sandbox, ec);
  fs::create_directories(archives, ec);
  if (!PRISM_CHECK(!ec)) return;
  const fs::path good = archives / L"good.7z";
  const fs::path damaged = archives / L"damaged.7z";
  std::string why;
  if (!PRISM_CHECK(BuildArchive(good, 31, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  // A file with the right extension and no valid archive inside: 7-Zip refuses it.
  if (!PRISM_CHECK(WriteBytes(damaged, Payload(32, 4096)))) return;
  const fs::path missing = archives / L"gone.7z";

  std::vector<session::SessionEntry> entries;
  const std::string goodFolder = Narrow(good.stem().wstring()) + "_src/docs";
  entries.push_back(session::SessionEntry{Narrow(missing.wstring()), "", false});
  entries.push_back(session::SessionEntry{Narrow(damaged.wstring()), "", false});
  entries.push_back(session::SessionEntry{Narrow(good.wstring()), goodFolder, true});
  if (!PRISM_CHECK(SeedSession(sandbox, entries, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }

  const fs::path run = root / L"skip.err";
  Child child;
  if (!PRISM_CHECK(StartChild(ExePath(), {}, sandbox, run, &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (!PRISM_CHECK(WaitForText(run, "session restore entries=3 tried=3 skipped=2 restored=1",
                              kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  if (!PRISM_CHECK(WaitForText(run, "session opened", kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  PRISM_CHECK(Contains(ReadText(run), "folder=" + goodFolder));
  if (!PRISM_CHECK(CloseMainWindow(child.info.dwProcessId))) return;
  DWORD exitCode = 1;
  if (!PRISM_CHECK(WaitForExit(&child, kExitTimeoutMs, &exitCode))) return;
  PRISM_CHECK_EQ(exitCode, DWORD{0});
  // The dead entries are not carried forward: the session converges on what is
  // actually openable instead of growing a graveyard.
  PRISM_CHECK_EQ(EntryCountInDocument(sandbox), size_t{1});
}

// ---------------------------------------------------------------------------
// Case 4: a corrupt session document is discarded, and the app still starts.
// ---------------------------------------------------------------------------
PRISM_TEST("restore.corrupt_session_starts_anyway") {
  prism::testtmp::UniqueDir fixture(L"c46", L"corrupt");
  const fs::path root = fixture.path();
  const fs::path sandbox = root / L"lapp";
  std::error_code ec;
  fs::create_directories(sandbox / L"PrismArchive", ec);
  if (!PRISM_CHECK(!ec)) return;
  if (!PRISM_CHECK(WriteBytes(sandbox / L"PrismArchive" / L"session.json",
                              std::vector<uint8_t>{
                                  '{', '"', 's', 'c', 'h', 'e', 'm', 'a', 'V', 'e', 'r', 's', 'i',
                                  'o', 'n', '"', ':', '2', ' ', 'o', 'o', 'p', 's'}))) {
    return;
  }
  const fs::path run = root / L"corrupt.err";
  Child child;
  std::string why;
  if (!PRISM_CHECK(StartChild(ExePath(), {}, sandbox, run, &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  // The discard is reported on stderr (one warn line) and nothing is restored.
  if (!PRISM_CHECK(WaitForText(run, "status=corrupt", kReadyTimeoutMs))) {
    std::printf("    child stderr:\n%s\n", ReadText(run).c_str());
    return;
  }
  PRISM_CHECK(Contains(ReadText(run), "restored=0"));
  // Reaching that line at all IS the "did not crash on the document" proof: the
  // corrupt file was read, discarded and reported before anything else ran.
  Kill(&child);
}
