// PrismArchive -- About / diagnostics window (card C57), Windows lane.
//
// ONE source file, THREE ctest entries (Tests/suites/c57-about.cmake):
//
//   win.about_dialog                          the shipped build
//   mutation.about_dialog.no_redaction        PRISM_C57_MUTANT_NO_REDACTION
//   mutation.about_dialog.hardcoded_version   PRISM_C57_MUTANT_HARDCODED_VERSION
//
// WHAT MAKES THIS A REAL TEST
//   Every assertion reads the REPORT CONTROL (a real EDIT HWND) through
//   GetWindowTextW -- never a member of the dialog. Copy and Save are driven by
//   clicking the real buttons (WM_COMMAND/BN_CLICKED), and their results are
//   compared BYTE FOR BYTE with what the control holds. The version is compared
//   with an INDEPENDENT reader (VerQueryValueW, below) against a fixture
//   executable whose VERSIONINFO says 9.8.7.6, so "the dialog read the file"
//   is the only way the two numbers can agree.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27)
//   * no_redaction: PrismCore's redactor hands the raw path through, so the
//     "no full path / no user name" assertions MUST go red;
//   * hardcoded_version: AboutDialog returns the literal "0.1.0.0", so the
//     fixture assertion MUST go red.
//   mini_test's mutant verdict is armed BEFORE the first case with a predicate
//   that proves the injection fired; it prints "C57 MUTATION CAUGHT: <arm>" at
//   the FIRST failure and leaves with 0. The ctest entry requires that marker
//   and nothing else -- no WILL_FAIL is ever combined with it (D26), and a
//   stale injection leaves this suite green, which turns the mutant entry red.
//   Arm names avoid clean/unarmed/control (D28).
//
// CONTRACTS D31: no sign-compare, no helper that only one arm uses.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winver.h>  // VS_FIXEDFILEINFO: the test's OWN version reader (an oracle)
#endif

#include "../mini_test.hpp"
#include "../../Src/PrismCore/DiagnosticsReport.hpp"
#include "../../Src/PrismWin/AboutDialog.hpp"

namespace about = prism::win::about;
namespace diag = prism::diagnostics;

namespace {

#if defined(_WIN32)

// ---------------------------------------------------------------------------
// Independent oracles. Deliberately NOT AboutDialog's own helpers: the point of
// the fixture is that two implementations of "read the VERSIONINFO" agree.
// ---------------------------------------------------------------------------
std::string VersionFromFile(const std::wstring& path) {
  DWORD ignored = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (size == 0) return std::string();
  std::vector<unsigned char> buffer(size);
  if (!::GetFileVersionInfoW(path.c_str(), 0, size, buffer.data())) return std::string();
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) ||
      info == nullptr) {
    return std::string();
  }
  char text[64] = {};
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", info->dwFileVersionMS >> 16,
                info->dwFileVersionMS & 0xFFFFu, info->dwFileVersionLS >> 16,
                info->dwFileVersionLS & 0xFFFFu);
  return text;
}

std::string Utf8(const std::wstring& wide) { return prism::win::WideToUtf8(wide); }

std::string ControlTextUtf8(HWND parent, int id) {
  const HWND control = ::GetDlgItem(parent, id);
  if (control == nullptr) return std::string();
  const int length = ::GetWindowTextLengthW(control);
  std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
  ::GetWindowTextW(control, buffer.data(), length + 1);
  buffer.resize(static_cast<std::size_t>(length));
  return Utf8(buffer);
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// A multiline EDIT normalizes "\n" to "\r\n" (and GetWindowTextW hands the
// CRLF form back). The comparisons below are about CONTENT, so the parser drops
// the CR while Copy/Save keep comparing the control's raw bytes.
std::string ValueOf(const std::string& report, std::string_view key) {
  const std::string prefix = std::string(key) + " = ";
  std::size_t at = 0;
  while (at <= report.size()) {
    const std::size_t end = report.find('\n', at);
    std::string line = report.substr(at, end == std::string::npos ? std::string::npos : end - at);
    while (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.compare(0, prefix.size(), prefix) == 0) return line.substr(prefix.size());
    if (end == std::string::npos) break;
    at = end + 1;
  }
  return std::string("<missing>");
}

std::wstring TempDirectory() {
  wchar_t buffer[MAX_PATH] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, buffer);
  std::wstring root(length > 0 ? std::wstring(buffer, length) : std::wstring(L".\\"));
  while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
  const std::wstring dir = root + L"\\prism-c57-about-" + std::to_wstring(::GetCurrentProcessId());
  ::CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::string ReadWholeFile(const std::wstring& path) {
  HANDLE file = ::CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return std::string();
  std::string bytes;
  char chunk[4096];
  DWORD got = 0;
  while (::ReadFile(file, chunk, sizeof(chunk), &got, nullptr) != FALSE && got > 0) {
    bytes.append(chunk, got);
  }
  ::CloseHandle(file);
  return bytes;
}

std::wstring ReadClipboardText() {
  bool opened = false;
  for (int attempt = 0; attempt < 20 && !opened; ++attempt) {
    opened = ::OpenClipboard(nullptr) != FALSE;
    if (!opened) ::Sleep(10);  // bounded: another process may hold it for a moment
  }
  if (!opened) return std::wstring();
  std::wstring text;
  const HANDLE data = ::GetClipboardData(CF_UNICODETEXT);
  if (data != nullptr) {
    const wchar_t* raw = static_cast<const wchar_t*>(::GlobalLock(data));
    if (raw != nullptr) text.assign(raw);
    ::GlobalUnlock(data);
  }
  ::CloseClipboard();
  return text;
}

// "Can this session create ANY window?" -- asked with a stock class so the
// answer is about the environment, never about our code (C18's rule: a broken
// dialog must never masquerade as a SKIP, and a SKIP must never pass, D1).
bool WindowStationUsable() {
  const HWND probe = ::CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10, nullptr,
                                       nullptr, ::GetModuleHandleW(nullptr), nullptr);
  if (probe == nullptr) return false;
  ::DestroyWindow(probe);
  return true;
}

std::string gSkipReason;  // non-empty => the whole run is a SKIP (never a pass)

// A dialog with the version pointed at 'module', everything else fixed. The
// report is generated by the production path (AboutFacts -> Snapshot ->
// BuildReport); only the inputs are pinned.
bool OpenWithVersion(about::AboutDialog& dialog, const std::wstring& module,
                     const std::wstring& enginePath) {
  about::AboutFacts facts;
  facts.versionModule = module;
  facts.enginePath = enginePath;
  facts.osVersion = "10.0.22631";
  facts.cpuArch = "x64";
  facts.language = "zh-CN";
  facts.userName = "alice";  // a NEEDLE, never a value
  facts.installMode = "installed";
  facts.progIdRegistered = true;
  facts.menuVerbRegistered = false;
  facts.generatedAtMs = 1762042000000;  // fixed: the report is reproducible
  dialog.SetFacts(std::move(facts));
  if (!dialog.Create(::GetModuleHandleW(nullptr), nullptr, L"About (test)")) {
    std::printf("    [dialog] Create failed: err=%lu\n",
                static_cast<unsigned long>(dialog.lastError()));
    PRISM_CHECK(false);  // our bug, not the environment
    return false;
  }
  return true;
}

void ClickButton(HWND parent, int id) {
  const HWND button = ::GetDlgItem(parent, id);
  ::SendMessageW(parent, WM_COMMAND, MAKEWPARAM(static_cast<WORD>(id), BN_CLICKED),
                 reinterpret_cast<LPARAM>(button));
}

// The fixture executable's VERSIONINFO is 9.8.7.6 (Tests/win/about_version_fixture.rc).
// The app's own resource file says 0.1.0.0 (PRISM_C57_EXPECT_VERSION, derived from
// project(PrismArchive VERSION ...)), so the two numbers can never be confused.
constexpr char kFixtureVersion[] = "9.8.7.6";

#ifndef PRISM_C57_VERSION_FIXTURE
#define PRISM_C57_VERSION_FIXTURE ""
#endif
#ifndef PRISM_C57_EXPECT_VERSION
#define PRISM_C57_EXPECT_VERSION ""
#endif

constexpr char kEnginePath[] = "C:\\Users\\alice\\AppData\\Local\\Programs\\PrismArchive\\7z.dll";

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// 1. The version is READ from a file, and the file decides (not the source).
// ===========================================================================
PRISM_TEST("version: the control shows the fixture's VERSIONINFO (9.8.7.6)") {
  if (!gSkipReason.empty()) return;
  const std::wstring fixture = prism::win::Utf8ToWide(PRISM_C57_VERSION_FIXTURE);
  // Calibrate the oracle itself before trusting it (CONTRACTS D4).
  PRISM_CHECK_EQ(VersionFromFile(fixture), std::string(kFixtureVersion));

  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, fixture, prism::win::Utf8ToWide(kEnginePath))) return;
  const std::string text = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  std::printf("    [report] %s\n", ValueOf(text, "version").c_str());
  PRISM_CHECK_EQ(ValueOf(text, "version"), std::string(kFixtureVersion));
  PRISM_CHECK(Contains(text, "9.8.7.6"));
  // The engine did not vanish: the NAME is the diagnostic fact, and it survives.
  PRISM_CHECK(Contains(text, "7z.dll"));
  PRISM_CHECK_EQ(ValueOf(text, "engine.path"), std::string("C:\\") + diag::kEllipsis + "\\7z.dll");
  PRISM_CHECK_EQ(ValueOf(text, "engine.version"), std::string("unknown"));  // no such file
  dialog.Destroy();
}

PRISM_TEST("version: the shipped PrismArchive.rc is what this build reports") {
  if (!gSkipReason.empty()) return;
  // The test executable carries the SAME resource file the app ships
  // (Src/App/PrismArchive.rc), so a drift between project() and the VERSIONINFO
  // shows up here before a user sees it in Properties.
  const std::string expected(PRISM_C57_EXPECT_VERSION);
  PRISM_CHECK(expected.size() >= 5);  // "0.1.0.0" -- the suite must pass it
  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, about::OwnModulePath(), prism::win::Utf8ToWide(kEnginePath))) return;
  const std::string text = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  PRISM_CHECK_EQ(ValueOf(text, "version"), expected);
  PRISM_CHECK_EQ(VersionFromFile(about::OwnModulePath()), expected);
  dialog.Destroy();
}

PRISM_TEST("version: a file without VERSIONINFO prints unknown, never a literal") {
  if (!gSkipReason.empty()) return;
  const std::wstring plain = TempDirectory() + L"\\no-version.txt";
  const HANDLE file = ::CreateFileW(plain.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);
  PRISM_CHECK_EQ(VersionFromFile(plain), std::string());  // the oracle agrees there is none

  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, plain, L"")) return;
  const std::string text = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  PRISM_CHECK_EQ(ValueOf(text, "version"), std::string("unknown"));  // D1: never omitted
  PRISM_CHECK_EQ(ValueOf(text, "engine.path"), std::string("unknown"));
  dialog.Destroy();
}

// ===========================================================================
// 2. Redaction -- the text in the control is what leaves the machine.
// ===========================================================================
PRISM_TEST("report: the control holds no full path and no user name") {
  if (!gSkipReason.empty()) return;
  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, about::OwnModulePath(), prism::win::Utf8ToWide(kEnginePath))) return;
  const std::string text = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  if (Contains(text, "alice") || Contains(text, "Users")) {
    std::printf("    [leaked] %s\n", text.c_str());
  }
  PRISM_CHECK(!Contains(text, "alice"));
  PRISM_CHECK(!Contains(text, "Users"));
  PRISM_CHECK(!Contains(text, "AppData"));
  // The drive letter survives, the tree does not.
  PRISM_CHECK(Contains(text, "C:\\"));
  PRISM_CHECK(Contains(text, diag::kEllipsis));
  dialog.Destroy();
}

// ===========================================================================
// 3. The two buttons, driven through the real HWND.
// ===========================================================================
PRISM_TEST("clipboard: the Copy button puts the control's exact text on the clipboard") {
  if (!gSkipReason.empty()) return;
  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, prism::win::Utf8ToWide(PRISM_C57_VERSION_FIXTURE),
                       prism::win::Utf8ToWide(kEnginePath))) {
    return;
  }
  const std::string before = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  PRISM_CHECK(before.size() > 100);  // a report, not a placeholder

  // Something else on the clipboard first: "empty -> filled" would otherwise be
  // indistinguishable from "never touched".
  {
    const std::wstring marker = L"not-the-report";
    if (::OpenClipboard(nullptr)) {
      ::EmptyClipboard();
      HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, (marker.size() + 1) * sizeof(wchar_t));
      if (block != nullptr) {
        void* target = ::GlobalLock(block);
        if (target != nullptr) {
          std::memcpy(target, marker.c_str(), (marker.size() + 1) * sizeof(wchar_t));
          ::GlobalUnlock(block);
          if (::SetClipboardData(CF_UNICODETEXT, block) == nullptr) ::GlobalFree(block);
        }
      }
      ::CloseClipboard();
    }
  }
  PRISM_CHECK(ReadClipboardText() == std::wstring(L"not-the-report"));  // the decoy is there

  ClickButton(dialog.Hwnd(), about::ids::kCopy);
  const std::wstring copied = ReadClipboardText();
  PRISM_CHECK_EQ(Utf8(copied), before);  // BYTE for byte, not "contains"
  dialog.Destroy();
}

PRISM_TEST("save: the Save button writes the control's exact bytes") {
  if (!gSkipReason.empty()) return;
  const std::wstring path = TempDirectory() + L"\\prism-report.txt";
  ::DeleteFileW(path.c_str());

  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, prism::win::Utf8ToWide(PRISM_C57_VERSION_FIXTURE),
                       prism::win::Utf8ToWide(kEnginePath))) {
    return;
  }
  // The picker is the only part a test may replace (a modal file dialog cannot
  // be driven headless); everything else is the shipped path.
  dialog.SetSavePathHandler([path](HWND) { return path; });
  const std::string expected = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  ClickButton(dialog.Hwnd(), about::ids::kSave);
  PRISM_CHECK(dialog.savedPath() == path);
  const std::string written = ReadWholeFile(path);
  PRISM_CHECK_EQ(written, expected);  // D2: the CONTENT, byte for byte
  PRISM_CHECK_EQ(written.size(), expected.size());
  // No BOM, no trailing newline that the control does not have.
  PRISM_CHECK(written.compare(0, 3, "\xEF\xBB\xBF") != 0);
  ::DeleteFileW(path.c_str());
  dialog.Destroy();
}

// ===========================================================================
// 4. Keyboard: Tab / Shift+Tab / Enter / Esc -- and Esc from INSIDE the report.
// ===========================================================================
PRISM_TEST("keyboard: Tab cycles the buttons, Enter activates, Esc closes") {
  if (!gSkipReason.empty()) return;
  const std::wstring path = TempDirectory() + L"\\prism-report-keys.txt";
  ::DeleteFileW(path.c_str());
  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, prism::win::Utf8ToWide(PRISM_C57_VERSION_FIXTURE),
                       prism::win::Utf8ToWide(kEnginePath))) {
    return;
  }
  dialog.SetSavePathHandler([path](HWND) { return path; });

  PRISM_CHECK_EQ(dialog.FocusedId(), -1);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));
  PRISM_CHECK_EQ(dialog.FocusedId(), about::ids::kCopy);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));
  PRISM_CHECK_EQ(dialog.FocusedId(), about::ids::kSave);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, false));  // wraps, like every V3 surface
  PRISM_CHECK_EQ(dialog.FocusedId(), about::ids::kCopy);
  PRISM_CHECK(dialog.DispatchKey(VK_TAB, true));
  PRISM_CHECK_EQ(dialog.FocusedId(), about::ids::kSave);

  // Enter on the focused button is the SAME path a click takes (WM_COMMAND).
  PRISM_CHECK(dialog.DispatchKey(VK_RETURN, false));
  PRISM_CHECK(dialog.savedPath() == path);
  PRISM_CHECK_EQ(ReadWholeFile(path), ControlTextUtf8(dialog.Hwnd(), about::ids::kReport));

  const HWND window = dialog.Hwnd();
  PRISM_CHECK(dialog.DispatchKey(VK_ESCAPE, false));
  PRISM_CHECK_EQ(::IsWindow(window), FALSE);  // Esc really closes it
  PRISM_CHECK(dialog.Hwnd() == nullptr);
  ::DeleteFileW(path.c_str());
}

PRISM_TEST("keyboard: Esc and Tab work while the caret is inside the report") {
  if (!gSkipReason.empty()) return;
  about::AboutDialog dialog;
  if (!OpenWithVersion(dialog, prism::win::Utf8ToWide(PRISM_C57_VERSION_FIXTURE),
                       prism::win::Utf8ToWide(kEnginePath))) {
    return;
  }
  const HWND report = dialog.Control(about::ids::kReport);
  PRISM_CHECK(report != nullptr);
  // The control is a real read-only multiline EDIT: that is what makes the
  // report selectable, scrollable and Ctrl+C-able by a human.
  const LONG style = ::GetWindowLongW(report, GWL_STYLE);
  PRISM_CHECK((style & ES_MULTILINE) != 0);
  PRISM_CHECK((style & ES_READONLY) != 0);
  PRISM_CHECK((style & WS_VSCROLL) != 0);

  ::SetFocus(report);
  // Tab from inside the text still moves the dialog's focus (the edit forwards
  // the key to the dialog instead of swallowing it).
  ::SendMessageW(report, WM_KEYDOWN, VK_TAB, 0);
  PRISM_CHECK_EQ(dialog.FocusedId(), about::ids::kCopy);
  ::SetFocus(report);
  const HWND window = dialog.Hwnd();
  ::SendMessageW(report, WM_KEYDOWN, VK_ESCAPE, 0);
  PRISM_CHECK_EQ(::IsWindow(window), FALSE);
}

// ===========================================================================
// 5. The live machine: the default facts reach the text, and nothing leaks.
// ===========================================================================
PRISM_TEST("live: the default dialog reports this machine, and still hides it") {
  if (!gSkipReason.empty()) return;
  about::AboutDialog dialog;  // facts come from ProbeFacts()
  if (!dialog.Create(::GetModuleHandleW(nullptr), nullptr, L"About (live)")) {
    PRISM_CHECK(false);
    return;
  }
  const std::string text = ControlTextUtf8(dialog.Hwnd(), about::ids::kReport);
  PRISM_CHECK(text.size() > 200);
  PRISM_CHECK_EQ(ValueOf(text, "version"), VersionFromFile(about::OwnModulePath()));
  PRISM_CHECK_EQ(ValueOf(text, "log.window"), std::string("20"));
  PRISM_CHECK_EQ(ValueOf(text, "recent.count"), std::string("0"));
  const std::string user = about::EnvironmentText(L"USERNAME");
  if (!user.empty() && user.size() >= diag::kMinNeedleChars) {
    PRISM_CHECK(!Contains(text, user.c_str()));  // the real user, not just "alice"
  }
  dialog.Destroy();
}

#endif  // _WIN32

int main(int argc, char** argv) {
#if defined(PRISM_C57_MUTANT_NO_REDACTION)
  // The matrix's injection evidence (D28): the binary announces itself, so the
  // copy in the control visibly carries the path the suite forbids.
  std::puts("C57 MUTANT ACTIVE: no_redaction (full paths and the user name survive)");
  // D27: armed before the first case, with the D3 predicate that proves the
  // injected defect actually ran. The marker is the ctest regexp; nothing else.
  ::prism::test::SetMutationVerdict("C57 MUTATION CAUGHT: no_redaction",
                                    [] { return diag::RedactionMutantFired(); });
#elif defined(PRISM_C57_MUTANT_HARDCODED_VERSION)
  std::puts("C57 MUTANT ACTIVE: hardcoded_version (the version is a literal again)");
  ::prism::test::SetMutationVerdict("C57 MUTATION CAUGHT: hardcoded_version",
                                    [] { return about::VersionMutantFired(); });
#endif

#if defined(_WIN32)
  if (!WindowStationUsable()) gSkipReason = "no interactive window station";
#endif
  const int failures = ::prism::test::RunAll(argc, argv);

#if defined(_WIN32)
  if (!gSkipReason.empty()) {
    // D1: a skip is PRINTED and it is never exit 0 (SKIP_RETURN_CODE 77).
    std::printf("SKIP: %s\n", gSkipReason.c_str());
    return 77;
  }
#endif
  if (failures != 0) return 1;
  std::puts("ALL ABOUT-DIALOG CHECKS PASSED");
  return 0;
}
