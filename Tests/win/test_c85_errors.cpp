// PrismArchive -- PrismWin
// C85, PROCESS LEVEL: the failure surface and the log a user can attach
// (card C85, S3-3 + S3-1).
//
// THE TWO DEFECTS THIS FILE PINS, both reproduced by hand against the shipped
// exe before the fix:
//
//   1. A failed open produced a dialog with NO readable reason. Launching on a
//      path that does not exist showed "打开失败 x.7z" and then
//      "CreateFileW failed; " -- a machine token with nothing after the
//      semicolon, because the Win32 code and the path were dropped at the one
//      place that knew them.
//   2. No log file existed at all: %LOCALAPPDATA%\PrismArchive held no .log,
//      and every stderr line was stamped 1970-01-01T00:00:00.000Z because no
//      timestamp was ever supplied.
//
// WHAT IS ASSERTED, all of it against the SHIPPED exe (nothing is
// re-implemented here):
//   * a MISSING file    -> the message box exists, its text is non-empty and
//                          says 找不到文件, names the file, and carries a Win32
//                          code (three failure classes, three sentences);
//   * a NON-ARCHIVE     -> the box says 打开失败 plus the engine's own refusal
//                          keyword (handler-rejected / declined) and an HRESULT;
//   * a VOLUME SET with a hole -> the box NAMES the missing volume
//                          ("missing volume(s)" + vols.7z.002), which is what
//                          makes that sentence actionable;
//   * ONE launch         -> <LOCALAPPDATA>\PrismArchive\logs\prism-YYYYMMDD.log
//                          EXISTS, its FIRST line's year is not 1970, and it
//                          carries this run's "app started" record (pid match).
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27), same source, one rule each:
//   PRISM_C85_MUTANT_EMPTY_REASON  -> AppWiring's OpenFailureText drops the
//                                     reason (arm: empty_reason);
//   PRISM_C85_MUTANT_NO_FILE_LOG   -> LogFile's sink writes nothing
//                                     (arm: no_file_log).
// Both arms are the SAME test source pointed at a mutant exe. The clean
// assertions go red, mini_test prints "C85 MUTATION CAUGHT: <arm>" at the FIRST
// failure and exits 0, so the target's PASS_REGULAR_EXPRESSION is the verdict.
// The runtime predicate proves the injection really compiled into that exe.
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

#include "../mini_test.hpp"
#include "../win/UniqueTempDir.hpp"
#include "../../Src/App/AppWiring.hpp"  // the ONE failure sentence builder
#include "../../Src/PrismCore/Log.hpp"
#include "../../Src/PrismWin/LogFile.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"

// C85: the sniff hint must OWN its bytes. Open() assigns it from a temporary
// (WideToUtf8(...)), so a view member dangles before Sniff() ever reads it --
// that was a real stack-use-after-scope, caught by ASan on the Windows lane and
// by nothing at all on a build whose layout happened to keep the slot
// addressable. A type change is the only guard that cannot be re-broken by
// accident.
static_assert(std::is_same_v<decltype(prism::win::sniff::Window::extensionHint), std::string>,
              "sniff::Window::extensionHint must OWN its bytes: Open() assigns a temporary");

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

#ifndef PRISM_C85_EXE
#error "C85 needs PRISM_C85_EXE (the exe under test) from the suite's CMake file"
#endif

namespace {

constexpr int kSkipReturnCode = 77;
constexpr DWORD kDialogTimeoutMs = 30'000;
constexpr DWORD kExitTimeoutMs = 60'000;

bool g_noFixture = false;
// D3 runtime proof that the INJECTION fired, observed in the product: the arm
// may only print CAUGHT when the defect it injects is really on screen.
bool g_reasonMissing = false;
bool g_logMissing = false;

#if defined(PRISM_C85_MUTANT_NAME)
constexpr const char* kMutant = PRISM_C85_MUTANT_NAME;
#else
constexpr const char* kMutant = nullptr;
#endif

std::wstring ExePath() { return prism::win::Utf8ToWide(PRISM_C85_EXE); }

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool WriteBytes(const fs::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

std::string Utf8(const std::wstring& wide) { return prism::win::WideToUtf8(wide); }

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

// ---------------------------------------------------------------- fixtures
struct World {
  prism::testtmp::UniqueDir dir{L"c85", L"errors"};
  fs::path localAppData;   // the LOCALAPPDATA the child is given
  fs::path stderrPath;
  std::wstring sevenZip;
  std::wstring goodArchive;
  std::wstring missing;
  std::wstring garbage;
  std::wstring volumeHead;
  std::wstring volumeGapName;  // the volume the hole removed (vols.7z.002)
  bool ready = false;
  std::string why;
};

World& Fixture() {
  static World world;
  return world;
}

bool Setup() {
  World& world = Fixture();
  if (world.ready) return true;
  if (!world.why.empty()) return false;
  if (!world.dir.ok()) {
    world.why = world.dir.error();
    return false;
  }
  wchar_t dll[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, ARRAYSIZE(dll))) {
    world.why = "PRISM_7Z_DLL is not set";
    return false;
  }
  const fs::path exe = fs::path(dll).parent_path() / L"7z.exe";
  if (!fs::exists(exe)) {
    world.why = "7z.exe is missing beside 7z.dll";
    return false;
  }
  world.sevenZip = exe.wstring();
  world.localAppData = world.dir.path() / L"localappdata";
  std::error_code ec;
  fs::create_directories(world.localAppData, ec);
  world.stderrPath = world.dir.path() / L"stderr.txt";
  world.goodArchive = (world.dir.path() / L"good.7z").wstring();
  world.missing = (world.dir.path() / L"not-there.7z").wstring();
  world.garbage = (world.dir.path() / L"garbage.dat").wstring();

  if (!WriteBytes(world.dir.path() / L"payload.txt", "c85 payload")) {
    world.why = "cannot write the payload";
    return false;
  }
  // Not an archive: 4 KiB of 'A' has no signature this engine knows, so every
  // handler declines it -- the "engine rejected the file" class.
  if (!WriteBytes(world.dir.path() / L"garbage.dat", std::string(4096, 'A'))) {
    world.why = "cannot write the non-archive fixture";
    return false;
  }
  const std::wstring cwd = world.dir.wstr();
  if (!Run7z(world.sevenZip, L"a -t7z -ms=off \"good.7z\" payload.txt", cwd)) {
    world.why = "7z.exe could not author good.7z";
    return false;
  }
  // A three-volume set with the MIDDLE one deleted: the plan can PROVE the hole
  // by name, which is the third failure class. The payload must be BIGGER than
  // one volume or 7z.exe writes a single .001 and there is no set to hole.
  std::string volumePayload(64 * 1024, '\0');
  for (size_t i = 0; i < volumePayload.size(); ++i) {
    // Poorly compressible on purpose: 12 KiB of one repeated byte compresses to
    // a few hundred bytes and 7z.exe then writes a SINGLE volume, so there is no
    // set to put a hole in.
    volumePayload[i] = static_cast<char>((i * 37u + (i / 251u) * 11u) & 0xFF);
  }
  if (!WriteBytes(world.dir.path() / L"big.bin", volumePayload)) {
    world.why = "cannot write the volume payload";
    return false;
  }
  // -m0=Copy: a COMPRESSED 64 KiB of patterned bytes collapses to ~500 bytes and
  // 7z.exe then writes one volume; the set has to be big enough to split.
  if (!Run7z(world.sevenZip, L"a -t7z -m0=Copy -ms=off -v4k \"vols.7z\" big.bin", cwd)) {
    world.why = "7z.exe could not author the volume set";
    return false;
  }
  fs::path part2 = world.dir.path() / L"vols.7z.002";
  fs::path part3 = world.dir.path() / L"vols.7z.003";
  if (!fs::exists(part3, ec)) {
    world.why = "the volume set did not produce vols.7z.003 (7z -v4k?)";
    return false;
  }
  world.volumeGapName = L"vols.7z.002";
  fs::remove(part2, ec);
  world.volumeHead = (world.dir.path() / L"vols.7z.001").wstring();
  world.ready = true;
  return true;
}

// ---------------------------------------------------------------- the child
struct Child {
  PROCESS_INFORMATION info{};
  HANDLE output = nullptr;
  bool started = false;
  bool exited = false;

  ~Child() {
    Stop();
    if (info.hProcess != nullptr) CloseHandle(info.hProcess);
    if (info.hThread != nullptr) CloseHandle(info.hThread);
    if (output != nullptr) CloseHandle(output);
  }

  void Stop() {
    if (!started || exited) return;
    TerminateProcess(info.hProcess, 0xC85);
    WaitForSingleObject(info.hProcess, 5'000);
    exited = true;
  }
};

bool StartChild(const std::vector<std::wstring>& args, const fs::path& localAppData,
                const fs::path& stdoutPath, Child* child, std::string* why) {
  std::wstring command = L"\"" + ExePath() + L"\"";
  for (const std::wstring& arg : args) command += L" \"" + arg + L"\"";

  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  child->output = CreateFileW(stdoutPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child->output == INVALID_HANDLE_VALUE) {
    child->output = nullptr;
    *why = "CreateFileW(stderr) failed";
    return false;
  }
  // The child gets THIS test's LOCALAPPDATA, so the log file it writes can be
  // read back without guessing where the real profile is -- and so a test run
  // can never pollute the developer's own log directory.
  std::vector<wchar_t> environment;
  {
    LPWCH block = GetEnvironmentStringsW();
    const std::wstring overrideEntry = L"LOCALAPPDATA=" + localAppData.wstring();
    for (LPWCH cursor = block; *cursor != L'\0';) {
      const std::wstring entry(cursor);
      cursor += entry.size() + 1;
      if (entry.rfind(L"LOCALAPPDATA=", 0) == 0) continue;
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
  startup.hStdInput = child->output;
  startup.hStdOutput = child->output;
  startup.hStdError = child->output;
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                      localAppData.c_str(), &startup, &child->info);
  if (!created) {
    *why = "CreateProcessW failed: " + std::to_string(GetLastError());
    return false;
  }
  child->started = true;
  return true;
}

bool ChildAlive(const Child& child) {
  return child.started && WaitForSingleObject(child.info.hProcess, 0) == WAIT_TIMEOUT;
}

bool WaitForExit(Child* child, DWORD budgetMs, DWORD* exitCode) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
  for (;;) {
    if (WaitForSingleObject(child->info.hProcess, 25) == WAIT_OBJECT_0) {
      DWORD code = 0;
      GetExitCodeProcess(child->info.hProcess, &code);
      *exitCode = code;
      child->exited = true;
      return true;
    }
    if (std::chrono::steady_clock::now() >= deadline) return false;
  }
}

// ------------------------------------------------------------ the message box
struct DialogSearch {
  DWORD pid = 0;
  HWND box = nullptr;
};

BOOL CALLBACK DialogSearchProc(HWND hwnd, LPARAM param) {
  auto* find = reinterpret_cast<DialogSearch*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != find->pid) return TRUE;
  wchar_t className[64]{};
  if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0) return TRUE;
  if (wcscmp(className, L"#32770") != 0) return TRUE;  // the MessageBox class
  find->box = hwnd;
  return FALSE;
}

HWND WaitForMessageBox(const Child& child, DWORD budgetMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
  for (;;) {
    DialogSearch find;
    find.pid = child.info.dwProcessId;
    EnumWindows(DialogSearchProc, reinterpret_cast<LPARAM>(&find));
    if (find.box != nullptr) return find.box;
    if (!ChildAlive(child)) return nullptr;
    if (std::chrono::steady_clock::now() >= deadline) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

struct StaticText {
  HWND box = nullptr;
  std::wstring text;
};

BOOL CALLBACK CollectStaticProc(HWND hwnd, LPARAM param) {
  auto* out = reinterpret_cast<StaticText*>(param);
  wchar_t className[64]{};
  if (GetClassNameW(hwnd, className, ARRAYSIZE(className)) == 0) return TRUE;
  if (wcscmp(className, L"Static") != 0) return TRUE;
  const int length = GetWindowTextLengthW(hwnd);
  if (length <= 0) return TRUE;
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
  if (copied <= 0) return TRUE;
  text.resize(static_cast<std::size_t>(copied));
  if (!out->text.empty()) out->text += L"\n";
  out->text += text;
  return TRUE;
}

std::wstring ReadDialogText(HWND box) {
  if (box == nullptr) return std::wstring();
  StaticText out;
  out.box = box;
  EnumChildWindows(box, CollectStaticProc, reinterpret_cast<LPARAM>(&out));
  return out.text;
}

// A MessageBox is created BEFORE its static child is filled in, so a client that
// enumerates the window the instant EnumWindows finds it can read an empty
// control (measured here: one run in three). Bounded poll, never a Sleep(n) and
// hope: the dialog's text is the assertion, so waiting for it is the point.
std::wstring ReadDialogTextWhenReady(HWND box, DWORD budgetMs) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budgetMs);
  std::wstring text;
  for (;;) {
    text = ReadDialogText(box);
    if (!text.empty()) return text;
    if (std::chrono::steady_clock::now() >= deadline) return text;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
}

// Close the box the way a user's Enter would, then let the process exit.
void DismissMessageBox(HWND box) {
  if (box == nullptr) return;
  PostMessageW(box, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
  PostMessageW(box, WM_CLOSE, 0, 0);
}

bool WideContains(const std::wstring& haystack, const std::wstring& needle) {
  return haystack.find(needle) != std::wstring::npos;
}

std::wstring FirstLine(const std::wstring& text) {
  const size_t end = text.find(L'\n');
  return end == std::wstring::npos ? text : text.substr(0, end);
}

// One dialog case: start the exe on 'archive', read the box, dismiss it.
bool RunDialogCase(const std::wstring& archive, std::wstring* text, std::string* why) {
  World& world = Fixture();
  const fs::path outPath = world.dir.path() / L"dialog.txt";
  Child child;
  if (!StartChild({archive}, world.localAppData, outPath, &child, why)) return false;
  HWND box = WaitForMessageBox(child, kDialogTimeoutMs);
  if (box == nullptr) {
    *why = "no message box appeared";
    child.Stop();
    return false;
  }
  *text = ReadDialogTextWhenReady(box, 5'000);
  DismissMessageBox(box);
  DWORD code = 0;
  if (!WaitForExit(&child, kExitTimeoutMs, &code)) {
    child.Stop();
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. A path that does not exist: the box names the file and says 找不到文件.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 a missing archive produces a dialog that says why") {
  World& world = Fixture();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  std::wstring text;
  std::string why;
  if (!PRISM_CHECK(RunDialogCase(world.missing, &text, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (text.empty()) std::printf("    dialog text was EMPTY\n");
  // The D3 proof, recorded BEFORE the assertion it belongs to: only a dialog
  // that really has no reason may claim the injected arm.
  g_reasonMissing = !WideContains(text, L"\u627E\u4E0D\u5230\u6587\u4EF6");  // 找不到文件
  // Three independent facts, so "non-empty" cannot pass by accident:
  // the human sentence, the file it is about, and the machine code.
  PRISM_CHECK(WideContains(text, L"\u627E\u4E0D\u5230\u6587\u4EF6"));  // 找不到文件
  PRISM_CHECK(WideContains(text, L"not-there.7z"));
  PRISM_CHECK(WideContains(text, L"0x"));
}

// ---------------------------------------------------------------------------
// 2. A file this engine refuses: the box carries the engine's own reason.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 a non-archive produces a dialog with the engine's reason") {
  World& world = Fixture();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  std::wstring text;
  std::string why;
  if (!PRISM_CHECK(RunDialogCase(world.garbage, &text, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (text.empty()) std::printf("    dialog text was EMPTY\n");
  // The refusal, in the engine's own words, plus the HRESULT the reason carries.
  const bool refused = WideContains(text, L"handler-rejected") ||
                       WideContains(text, L"declined") || WideContains(text, L"\u62D2\u7EDD");
  PRISM_CHECK(refused);
  PRISM_CHECK(WideContains(text, L"0x"));
  PRISM_CHECK(WideContains(text, L"garbage.dat"));
}

// ---------------------------------------------------------------------------
// 3. A volume set with a hole: the box NAMES the missing volume.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 a volume set with a hole names the missing volume") {
  World& world = Fixture();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  std::wstring text;
  std::string why;
  if (!PRISM_CHECK(RunDialogCase(world.volumeHead, &text, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  if (text.empty()) std::printf("    dialog text was EMPTY\n");
  PRISM_CHECK(WideContains(text, L"missing volume"));
  PRISM_CHECK(WideContains(text, world.volumeGapName));  // vols.7z.002
}

// ---------------------------------------------------------------------------
// 4. The log: a file exists after one launch, its first line has a REAL year,
//    and it carries this run's own record.
// ---------------------------------------------------------------------------
PRISM_TEST("c85 one launch leaves a dated log file with real timestamps") {
  World& world = Fixture();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  const fs::path outPath = world.dir.path() / L"test-run.txt";
  Child child;
  std::string why;
  // --test is the headless verify verb: it starts up, logs, opens the archive
  // and exits on its own (no window, no dialog to dismiss, bounded by design).
  if (!PRISM_CHECK(StartChild({L"--test", world.goodArchive}, world.localAppData, outPath,
                              &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  DWORD exitCode = 1;
  const bool exited = WaitForExit(&child, kExitTimeoutMs, &exitCode);
  const std::string stderrText = ReadText(outPath);
  if (!exited) child.Stop();
  PRISM_CHECK(exited);
  if (!exited) {
    std::printf("    stderr: %s\n", stderrText.c_str());
    return;
  }

  // (a) the file exists, at the documented path.
  const int64_t now = prism::win::logfile::SystemNowMillis();
  const fs::path logPath = prism::win::logfile::DailyPath(
      (world.localAppData / L"PrismArchive" / L"logs").wstring(), now);
  std::error_code ec;
  const bool exists = fs::exists(logPath, ec);
  g_logMissing = !exists;  // the D3 proof for this arm (see main())
  if (!exists) {
    std::printf("    expected log: %s\n", logPath.string().c_str());
    std::printf("    stderr: %s\n", stderrText.c_str());
  }
  PRISM_CHECK(exists);
  if (!exists) return;

  const std::string logText = ReadText(logPath);
  // (b) the FIRST line's year is the real one: the shipped prefix was
  // 1970-01-01T00:00:00.000Z.
  char year[5]{};
  for (int i = 0; i < 4 && i < static_cast<int>(logText.size()); ++i) year[i] = logText[i];
  const std::string currentYear = std::to_string(
      prism::log::CivilFromDays(now / 86400000).year);
  PRISM_CHECK_EQ(std::string(year), currentYear);
  PRISM_CHECK(logText.rfind("1970", 0) != 0);
  // (c) it carries THIS run: the startup record with this child's pid.
  PRISM_CHECK(Contains(logText, "app started"));
  PRISM_CHECK(Contains(logText, "pid=\""));
  PRISM_CHECK(Contains(logText, "pid=\"" + std::to_string(child.info.dwProcessId) + "\""));
  // (d) the stderr prefix is a real timestamp too (that is the line the user
  // photographed).
  PRISM_CHECK(stderrText.rfind("1970", 0) != 0);
  PRISM_CHECK(Contains(stderrText, currentYear + "-"));
}

// ---------------------------------------------------------------------------
// 5. The SESSION-RESTORE path still works with the new logger and the new error
//    surface -- and if it ever dies, this case prints the WHOLE child stderr
//    (an ASan report's STACK is at the top of that file; a tail of shadow bytes
//    names no frames, which is exactly what a truncated report cost us once).
// ---------------------------------------------------------------------------
PRISM_TEST("c85 the session-restore path survives the new logger") {
  World& world = Fixture();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  // The sandbox a previous CLEAN run would have left behind: one archive that is
  // gone, one that is not an archive at all, and one that is fine AND has a
  // folder inside, restored into that folder in LIST mode -- the exact shape
  // C46's "skips missing and damaged" case seeds, reproduced here because a
  // restore that only ever lands on a root grid is not the same code path.
  const fs::path sandbox = world.dir.path() / L"restore-lapp";
  std::error_code ec;
  fs::create_directories(sandbox, ec);
  const fs::path stage = world.dir.path() / L"foldered_src";
  fs::create_directories(stage / L"docs", ec);
  if (!PRISM_CHECK(WriteBytes(stage / L"docs" / L"notes.txt", "restore me") &&
                   WriteBytes(stage / L"payload.bin", "restore me too"))) {
    return;
  }
  // 7z.exe packs the DIRECTORY name, so the archive root holds "foldered_src/"
  // and the restored folder is "foldered_src/docs" -- a NON-root folder.
  const fs::path foldered = world.dir.path() / L"foldered.7z";
  if (!PRISM_CHECK(Run7z(world.sevenZip, L"a -t7z -ms=off \"foldered.7z\" foldered_src",
                         world.dir.wstr()))) {
    return;
  }
  const std::string folderedFolder = "foldered_src/docs";
  prism::app::SessionBootstrap bootstrap = prism::app::LoadSessionUnder(sandbox.wstring());
  if (!PRISM_CHECK(!bootstrap.file.empty())) return;
  std::vector<prism::session::SessionEntry> entries;
  entries.push_back({Utf8(world.missing), "", false});
  entries.push_back({Utf8(world.garbage), "", false});
  entries.push_back({Utf8(foldered.wstring()), folderedFolder, true});
  bootstrap.store.Replace(entries, 0);
  bootstrap.store.MarkCleanShutdown(true);
  std::string why;
  if (!PRISM_CHECK(prism::app::SaveSession(&bootstrap, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }

  const fs::path outPath = world.dir.path() / L"restore.txt";
  Child child;
  if (!PRISM_CHECK(StartChild({}, sandbox, outPath, &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExitTimeoutMs);
  std::string text;
  bool found = false;
  // The FULL line C46 waits for: the counts are part of the claim, and a
  // restore that decides differently must not look like a timeout.
  const char* kRestoreLine = "session restore entries=3 tried=3 skipped=2 restored=1";
  for (;;) {
    text = ReadText(outPath);
    if (Contains(text, kRestoreLine)) {
      found = true;
      break;
    }
    if (!ChildAlive(child) || std::chrono::steady_clock::now() >= deadline) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  // Let the window come up (the app prints "session opened" once the restored
  // archive reached the view), then re-read: a crash ANYWHERE in that window --
  // including AFTER the restore decision -- leaves an ASan report behind, and
  // that report is the only thing that names the frame.
  const auto openedDeadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(10'000);
  while (std::chrono::steady_clock::now() < openedDeadline) {
    text = ReadText(outPath);
    if (Contains(text, "session opened") || !ChildAlive(child)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  text = ReadText(outPath);
  if (found) PRISM_CHECK(Contains(text, std::string("folder=") + folderedFolder));
  const bool skipped = Contains(text, "skipped=2");
  const bool restored = Contains(text, "restored=1");
  const bool aborted = Contains(text, "AddressSanitizer") || Contains(text, "==ABORTING==");
  if (!found || aborted) {
    // EVERYTHING, not a tail: this is the diagnostic that names the frame.
    std::printf("    child stderr (FULL, %zu bytes):\n%s\n", text.size(), text.c_str());
  }
  child.Stop();
  PRISM_CHECK(found);
  PRISM_CHECK(skipped);
  PRISM_CHECK(restored);
  // The child must still be a normal app: an ASan abort here is the C46 red we
  // are chasing, caught with its stack instead of a tail of shadow bytes.
  PRISM_CHECK(!aborted);
}

int main(int argc, char** argv) {
  if (kMutant != nullptr) {
    std::printf("C85 MUTANT ACTIVE: %s\n", kMutant);
    prism::test::SetMutationVerdict(
        (std::string("C85 MUTATION CAUGHT: ") + kMutant).c_str(),
        [mutation = kMutant] {
          // The proof is the PRODUCT's behaviour, not a macro in this binary:
          // the log arm is real when the log file is really absent, the reason
          // arm when the dialog really has no human sentence in it.
          return std::string(mutation) == "no_file_log" ? g_logMissing : g_reasonMissing;
        });
  }
  if (!Setup()) {
    std::printf("C85-ERRORS SKIP: fixture unavailable (%s)\n", Fixture().why.c_str());
    return kSkipReturnCode;
  }
  const int rc = prism::test::RunAll(argc, argv);
  if (kMutant != nullptr) {
    if (rc == 0) {
      std::printf("C85 MUTATION NOT CAUGHT: %s stayed green\n", kMutant);
      return 1;
    }
    return 0;
  }
  if (rc != 0) return rc;
  if (g_noFixture) {
    std::printf("C85-ERRORS SKIP: fixture unavailable (%s)\n", Fixture().why.c_str());
    return kSkipReturnCode;
  }
  std::puts("ALL C85 ERROR-SURFACE CHECKS PASSED");
  return 0;
}
