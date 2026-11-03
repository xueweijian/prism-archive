// PrismArchive -- PrismWin
// C66, PROCESS LEVEL: the number the shipped exe writes to settings.json.
//
// The card's own verification method: "open -> close five times, then read
// settings.json; if the width grows by 16 every time, the shadow border is being
// persisted". This suite does exactly that against PrismArchive.exe, with
// %LOCALAPPDATA% redirected to a scratch folder so the user's real settings file
// is never touched.
//
// THE SEEDED FILE IS THE USER'S: window.width = 1552, window.height = 832 on a
// 1536x816 work area. After the first launch + close the file must hold a size
// that FITS the work area, and the next four cycles must not change it by more
// than one pixel -- no +16 drift, which is what the pre-fix binary did.
//
// Every wait is bounded and also watches the child process (CONTRACTS D29); the
// evidence is a BYTE fact (the stderr line / the file's contents), never a
// stopwatch.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../win/UniqueTempDir.hpp"
#include "../../Src/PrismWin/Win32Transcoder.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

#ifndef PRISM_C66_EXE
#error "C66 needs PRISM_C66_EXE (the exe under test) from the suite's CMake file"
#endif

namespace {

constexpr int kSkipReturnCode = 77;
constexpr DWORD kLineTimeoutMs = 30'000;
constexpr DWORD kKillTimeoutMs = 5'000;

bool g_noFixture = false;
std::string g_why;

// The JSON is read with a substring search on purpose: the assertions are about
// two integers, and a full parser here would be a second implementation of the
// document the product already owns (and a second thing to be wrong).
int ReadIntField(const std::string& text, const std::string& key, int fallback) {
  const std::string needle = "\"" + key + "\"";
  const std::size_t at = text.find(needle);
  if (at == std::string::npos) return fallback;
  const std::size_t colon = text.find(':', at + needle.size());
  if (colon == std::string::npos) return fallback;
  std::size_t begin = colon + 1;
  while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t' ||
                                 text[begin] == '\r' || text[begin] == '\n')) {
    ++begin;
  }
  return std::atoi(text.c_str() + begin);
}

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool WriteText(const fs::path& path, const std::string& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

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

struct World {
  prism::testtmp::UniqueDir dir{L"c66", L"launch"};
  fs::path localAppData;
  fs::path settingsFile;
  std::wstring archive;
  bool ready = false;
};

World& W() {
  static World world;
  return world;
}

bool Setup() {
  World& w = W();
  if (w.ready) return true;
  if (!g_why.empty()) return false;
  if (!w.dir.ok()) {
    g_why = w.dir.error();
    return false;
  }
  w.localAppData = w.dir.path() / L"LocalAppData";
  w.settingsFile = w.localAppData / L"PrismArchive" / L"settings.json";
  std::error_code ec;
  fs::create_directories(w.settingsFile.parent_path(), ec);
  if (ec) {
    g_why = "cannot create the settings folder";
    return false;
  }
  w.archive = (w.dir.path() / L"fixture.7z").wstring();
  if (!WriteText(w.dir.path() / L"note.txt", "c66 payload")) {
    g_why = "cannot write the payload";
    return false;
  }
  wchar_t dll[MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, ARRAYSIZE(dll)) == 0) {
    g_why = "PRISM_7Z_DLL is not set";
    return false;
  }
  const fs::path sevenZip = fs::path(dll).parent_path() / L"7z.exe";
  if (!fs::exists(sevenZip)) {
    g_why = "7z.exe is missing beside 7z.dll";
    return false;
  }
  if (!Run7z(sevenZip.wstring(), L"a -t7z -ms=off \"fixture.7z\" note.txt", w.dir.wstr())) {
    g_why = "7z.exe could not author the fixture";
    return false;
  }
  w.ready = true;
  return true;
}

// The user's file, verbatim: 1552x832 on a 1536x816 screen.
std::string SeedSettings() {
  return std::string(
      "{\n"
      "  \"schemaVersion\": 3,\n"
      "  \"window\": { \"width\": 1552, \"height\": 832, \"listMode\": true }\n"
      "}\n");
}

struct Child {
  PROCESS_INFORMATION info{};
  HANDLE output = nullptr;
  bool started = false;
  bool exited = false;

  ~Child() {
    if (started && !exited) {
      TerminateProcess(info.hProcess, 0xC66);
      WaitForSingleObject(info.hProcess, kKillTimeoutMs);
    }
    if (info.hProcess != nullptr) CloseHandle(info.hProcess);
    if (info.hThread != nullptr) CloseHandle(info.hThread);
    if (output != nullptr) CloseHandle(output);
  }
};

bool StartChild(const fs::path& outputPath, Child* child) {
  std::wstring command = std::wstring(L"\"") + prism::win::Utf8ToWide(PRISM_C66_EXE) + L"\" \"" +
                        W().archive + L"\"";
  SECURITY_ATTRIBUTES security{};
  security.nLength = sizeof(security);
  security.bInheritHandle = TRUE;
  child->output = CreateFileW(outputPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (child->output == INVALID_HANDLE_VALUE) {
    child->output = nullptr;
    return false;
  }
  std::vector<wchar_t> environment;
  {
    LPWCH block = GetEnvironmentStringsW();
    const std::wstring overrideEntry = L"LOCALAPPDATA=" + W().localAppData.wstring();
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
                                      W().dir.wstr().c_str(), &startup, &child->info);
  if (!created) return false;
  child->started = true;
  return true;
}

bool ChildAlive(const Child& child) {
  return child.started && WaitForSingleObject(child.info.hProcess, 0) == WAIT_TIMEOUT;
}

bool WaitForLine(Child* child, const fs::path& path, const std::string& needle,
                 std::string* text) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLineTimeoutMs);
  for (;;) {
    *text = ReadText(path);
    if (text->find(needle) != std::string::npos) return true;
    if (!ChildAlive(*child)) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

struct WindowSearch {
  DWORD pid = 0;
  HWND hwnd = nullptr;
};

BOOL CALLBACK WindowProcFind(HWND hwnd, LPARAM param) {
  auto* find = reinterpret_cast<WindowSearch*>(param);
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid != find->pid || IsWindowVisible(hwnd) == FALSE) return TRUE;
  wchar_t title[256]{};
  GetWindowTextW(hwnd, title, ARRAYSIZE(title));
  if (wcsstr(title, L"PrismArchive") == nullptr) return TRUE;
  find->hwnd = hwnd;
  return FALSE;
}

HWND WaitForWindow(const Child& child) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLineTimeoutMs);
  for (;;) {
    WindowSearch find;
    find.pid = child.info.dwProcessId;
    EnumWindows(WindowProcFind, reinterpret_cast<LPARAM>(&find));
    if (find.hwnd != nullptr) return find.hwnd;
    if (!ChildAlive(child)) return nullptr;
    if (std::chrono::steady_clock::now() >= deadline) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

// One launch: seed (first cycle only), wait for the restore line, close the
// window, wait for the process to exit, then read what was written.
struct Cycle {
  bool ok = false;
  int width = 0;
  int height = 0;
  int workWidth = 0;
  int workHeight = 0;
  std::string log;
};

Cycle RunCycle(int index, bool seed) {
  Cycle cycle;
  World& w = W();
  if (seed && !WriteText(w.settingsFile, SeedSettings())) return cycle;
  const fs::path output = w.dir.path() / (L"run" + std::to_wstring(index) + L".log");
  Child child;
  if (!StartChild(output, &child)) return cycle;
  std::string text;
  if (!WaitForLine(&child, output, "window restore ", &text)) {
    cycle.log = text;
    return cycle;
  }
  const HWND hwnd = WaitForWindow(child);
  if (hwnd == nullptr) {
    cycle.log = text;
    return cycle;
  }
  PostMessageW(hwnd, WM_CLOSE, 0, 0);
  // Bounded: 15 s for the process to leave after its window closed.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (ChildAlive(child) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  child.exited = !ChildAlive(child);
  if (!child.exited) {
    cycle.log = ReadText(output);
    return cycle;
  }
  cycle.log = ReadText(output);
  const std::string saved = ReadText(w.settingsFile);
  cycle.width = ReadIntField(saved, "width", -1);
  cycle.height = ReadIntField(saved, "height", -1);
  // The exe printed the work area it clamped into; that is the oracle for
  // "fits", so the test never hardcodes the runner's screen size (D21).
  const std::size_t work = cycle.log.find("work=");
  if (work != std::string::npos) {
    std::sscanf(cycle.log.c_str() + work, "work=%dx%d", &cycle.workWidth, &cycle.workHeight);
  }
  cycle.ok = cycle.width > 0 && cycle.height > 0 && cycle.workWidth > 0;
  return cycle;
}

}  // namespace

int main(int argc, char** argv) {
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures == 0 && g_noFixture) {
    std::puts("C66-LAUNCH SKIPPED: fixture unavailable");
    return kSkipReturnCode;
  }
  if (failures == 0) std::puts("ALL WINDOW-PERSIST CHECKS PASSED");
  return failures;
}

// ---------------------------------------------------------------------------
// The card's verification: five open/close cycles, then settings.json.
// ---------------------------------------------------------------------------
PRISM_TEST("c66 the exe never persists a window bigger than the work area") {
  if (!Setup()) {
    std::printf("  [skip] %s\n", g_why.c_str());
    g_noFixture = true;
    return;
  }
  Cycle first = RunCycle(0, /*seed=*/true);
  std::printf("  [cycle 0] settings=%dx%d work=%dx%d ok=%d\n", first.width, first.height,
              first.workWidth, first.workHeight, static_cast<int>(first.ok));
  if (!PRISM_CHECK(first.ok)) {
    std::printf("%s\n", first.log.c_str());
    return;
  }
  // The seeded 1552x832 does NOT fit; what the fixed exe writes must.
  PRISM_CHECK(first.width <= first.workWidth);
  PRISM_CHECK(first.height <= first.workHeight);
  PRISM_CHECK(first.width >= 640);    // the documented minimum
  PRISM_CHECK(first.height >= 480);

  // Four more cycles, each starting from what the previous close wrote.
  //
  // The comparison starts at the SECOND cycle on purpose. Cycle 0 restores a
  // seeded value that no window can have (1552x832 on a 1536x816 screen) and the
  // clamp produces the largest frame that fits; on a DWM-capable machine that
  // frame is the work area minus the shadow border, so exactly ONE convergence
  // step is expected and correct. What must never happen is a step per cycle --
  // the pre-fix binary added the border on EVERY close (+16 px, forever), and
  // that is what the cycle 1..4 comparison catches.
  int previousWidth = 0;
  int previousHeight = 0;
  std::string settled;
  for (int cycle = 1; cycle < 5; ++cycle) {
    const Cycle next = RunCycle(cycle, /*seed=*/false);
    std::printf("  [cycle %d] settings=%dx%d work=%dx%d ok=%d\n", cycle, next.width,
                next.height, next.workWidth, next.workHeight, static_cast<int>(next.ok));
    if (!PRISM_CHECK(next.ok)) break;
    PRISM_CHECK(next.width <= next.workWidth);
    PRISM_CHECK(next.height <= next.workHeight);
    PRISM_CHECK(next.width >= 640);
    PRISM_CHECK(next.height >= 480);
    if (cycle > 1) {
      // One pixel of rounding is allowed; a border is not.
      const int deltaWidth = next.width - previousWidth;
      const int deltaHeight = next.height - previousHeight;
      PRISM_CHECK(deltaWidth <= 1 && deltaWidth >= -1);
      PRISM_CHECK(deltaHeight <= 1 && deltaHeight >= -1);
    }
    previousWidth = next.width;
    previousHeight = next.height;
  }
}
