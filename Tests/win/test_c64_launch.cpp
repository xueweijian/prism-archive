// PrismArchive -- PrismWin
// C64, PROCESS LEVEL: "PrismArchive.exe a.7z b.7z" really opens TWO tabs.
//
// The in-process suite (win.c64_tabs) proves the window wires a click to a
// switch. This one proves the COMMAND LINE reaches that model in the shipped
// exe: two archives on the command line, one window, two tabs, the FIRST one
// active. A missing third argument is skipped and counted -- not a message box
// per file, which is why the app must reach its window and print the line
// instead of blocking in a dialog.
//
// The app reports the count on stderr (a GUI CRT has no console), redirected to
// a file and read back. Readiness is a BYTE fact ("the line is in the file"),
// never a stopwatch; every wait is bounded and also watches the child
// (CONTRACTS D29 + the C46 precedent).
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "../win/UniqueTempDir.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

#ifndef PRISM_C64_EXE
#error "C64 needs PRISM_C64_EXE (the exe under test) from the suite's CMake file"
#endif

namespace {

constexpr int kSkipReturnCode = 77;
constexpr DWORD kLineTimeoutMs = 20'000;
constexpr DWORD kKillTimeoutMs = 5'000;

bool g_noFixture = false;

std::wstring ExePath() { return prism::win::Utf8ToWide(PRISM_C64_EXE); }

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
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

std::string Normalize(const std::wstring& path) {
  std::string text = prism::win::WideToUtf8(path);
  for (char& c : text) {
    if (c == '\\') c = '/';
  }
  while (text.size() > 1 && text.back() == '/') text.pop_back();
  return text;
}

struct Child {
  PROCESS_INFORMATION info{};
  HANDLE stderrFile = nullptr;
  bool started = false;
  bool exited = false;

  ~Child() {
    if (started && !exited) {
      TerminateProcess(info.hProcess, 0xC64);
      WaitForSingleObject(info.hProcess, kKillTimeoutMs);
    }
    if (info.hProcess != nullptr) CloseHandle(info.hProcess);
    if (info.hThread != nullptr) CloseHandle(info.hThread);
    if (stderrFile != nullptr) CloseHandle(stderrFile);
  }
};

bool StartChild(const std::wstring& exe, const std::vector<std::wstring>& args,
                const fs::path& environmentDir, const fs::path& stderrPath, Child* child,
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
    *why = "CreateFileW(stderr) failed";
    return false;
  }

  std::vector<wchar_t> environment;
  {
    LPWCH block = GetEnvironmentStringsW();
    const std::wstring overrideEntry = L"LOCALAPPDATA=" + environmentDir.wstring();
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
  startup.hStdInput = child->stderrFile;
  startup.hStdOutput = child->stderrFile;
  startup.hStdError = child->stderrFile;
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                                      CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                      environmentDir.c_str(), &startup, &child->info);
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

// Wait for one stderr line; both exits are bounded (deadline and child death).
bool WaitForLine(Child* child, const fs::path& stderrPath, const std::string& needle,
                 std::string* text) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kLineTimeoutMs);
  for (;;) {
    *text = ReadText(stderrPath);
    if (Contains(*text, needle)) return true;
    if (!ChildAlive(*child)) return false;
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
}

void StopChild(Child* child) {
  if (!child->started || child->exited) return;
  TerminateProcess(child->info.hProcess, 0xC64);
  WaitForSingleObject(child->info.hProcess, kKillTimeoutMs);
  child->exited = true;
}

struct Toy {
  prism::testtmp::UniqueDir dir{L"c64", L"launch"};
  std::wstring alpha;
  std::wstring beta;
  std::wstring missing;
  std::wstring sevenZip;
  bool ready = false;
  std::string why;
};

Toy& World() {
  static Toy toy;
  return toy;
}

bool Setup() {
  Toy& toy = World();
  if (toy.ready) return true;
  if (!toy.why.empty()) return false;
  if (!toy.dir.ok()) {
    toy.why = toy.dir.error();
    return false;
  }
  wchar_t dll[MAX_PATH]{};
  if (!GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, ARRAYSIZE(dll))) {
    toy.why = "PRISM_7Z_DLL is not set";
    return false;
  }
  const fs::path exe = fs::path(dll).parent_path() / L"7z.exe";
  if (!fs::exists(exe)) {
    toy.why = "7z.exe is missing beside 7z.dll";
    return false;
  }
  toy.sevenZip = exe.wstring();
  toy.alpha = (toy.dir.path() / L"alpha.7z").wstring();
  toy.beta = (toy.dir.path() / L"beta.7z").wstring();
  toy.missing = (toy.dir.path() / L"not-there.7z").wstring();
  if (!WriteText(toy.dir.path() / L"a.txt", "alpha") ||
      !WriteText(toy.dir.path() / L"b.txt", "beta")) {
    toy.why = "cannot write the payloads";
    return false;
  }
  const std::wstring cwd = toy.dir.wstr();
  if (!Run7z(toy.sevenZip, L"a -t7z -ms=off \"alpha.7z\" a.txt", cwd) ||
      !Run7z(toy.sevenZip, L"a -t7z -ms=off \"beta.7z\" b.txt", cwd)) {
    toy.why = "7z.exe could not author the archives";
    return false;
  }
  toy.ready = true;
  return true;
}

}  // namespace

PRISM_TEST("c64 process: two command-line archives -> two tabs, first active") {
  Toy& toy = World();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  const fs::path stderrPath = toy.dir.path() / L"first.err";
  Child child;
  std::string why;
  if (!PRISM_CHECK(StartChild(ExePath(), {toy.alpha, toy.beta}, toy.dir.path(), stderrPath, &child,
                              &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  std::string text;
  const std::string needle = "tabs opened=2 skipped=0 active=" + Normalize(toy.alpha);
  const bool found = WaitForLine(&child, stderrPath, needle, &text);
  if (!found) {
    std::printf("    stderr tail:\n%s\n", text.substr(text.size() > 2000 ? text.size() - 2000 : 0).c_str());
  }
  StopChild(&child);
  PRISM_CHECK(found);
}

PRISM_TEST("c64 process: a missing third argument is skipped, never a dialog") {
  Toy& toy = World();
  if (!Setup()) {
    g_noFixture = true;
    return;
  }
  const fs::path stderrPath = toy.dir.path() / L"second.err";
  Child child;
  std::string why;
  if (!PRISM_CHECK(StartChild(ExePath(), {toy.alpha, toy.beta, toy.missing}, toy.dir.path(),
                              stderrPath, &child, &why))) {
    std::printf("    %s\n", why.c_str());
    return;
  }
  std::string text;
  const std::string needle = "tabs opened=2 skipped=1 active=" + Normalize(toy.alpha);
  const bool found = WaitForLine(&child, stderrPath, needle, &text);
  if (!found) {
    std::printf("    stderr tail:\n%s\n", text.substr(text.size() > 2000 ? text.size() - 2000 : 0).c_str());
  }
  StopChild(&child);
  PRISM_CHECK(found);
}

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("C64-LAUNCH SKIP: no COM on this window station");
    return kSkipReturnCode;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc != 0) return rc;
  if (g_noFixture) {
    std::printf("C64-LAUNCH SKIP: fixture unavailable (%s)\n", World().why.c_str());
    return kSkipReturnCode;
  }
  std::puts("ALL C64 LAUNCH CHECKS PASSED");
  return 0;
}
