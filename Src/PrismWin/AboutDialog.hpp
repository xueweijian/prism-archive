// PrismArchive -- PrismWin
// AboutDialog: Help > About PrismArchive. The one screen that answers "what am
// I running, and what is this machine actually doing?" without asking the user
// to run commands.
//
// WHY A REPORT AND NOT A SPLASH
//   The support question behind this window is never "which version" -- it is
//   "the right-click entry is missing / the archive will not open". C57's
//   PrismCore half (DiagnosticsReport.hpp) already knows how to write the answer
//   as text that is safe to paste in public (no user name, no full path, counts
//   only for the log). This file is the Win32 shell around that text: it SHOWS
//   it, COPIES it, and SAVES it. It never formats a second version of the
//   report -- BuildReport() is the only formatter (CONTRACTS D7).
//
// PROGRAMMATIC Win32, NO .rc DIALOG
//   Same shape as C18's CompressDialog and C31's SettingsDialog: a registered
//   window class, CreateWindowExW, real child controls, and the V3 glass shell
//   (DialogChrome) for the card, the title bar and the two buttons. The REPORT
//   control is the one visible stock control on purpose: it is what the user
//   reads, selects and scrolls. The two buttons stay invisible adapters (the
//   chrome paints them) exactly like the settings screen, so BM_CLICK and
//   WM_COMMAND still drive the same code the user does.
//
// VERSION: READ, NEVER TYPED
//   The first line of the report that users quote is "version = 0.1.0.0". It
//   comes from the VERSIONINFO block of a real PE file -- by default this
//   executable (Src/App/PrismArchive.rc) -- through CodecProbe's
//   ReadFileVersion. There is no version literal in this header. The suite
//   points the dialog at a fixture executable whose VERSIONINFO says 9.8.7.6;
//   a build that hardcodes the version therefore shows the wrong number and
//   goes red (PRISM_C57_MUTANT_HARDCODED_VERSION proves the control).
//
// KEYBOARD
//   Tab / Shift+Tab cycle Copy -> Save, Enter or Space activates the focused
//   button, Esc closes. The keys are decided by DialogChrome's ClassifyKey /
//   NextFocus (one vocabulary for every V3 surface), and they are handled even
//   while the caret is inside the report: the edit is subclassed to forward
//   WM_KEYDOWN to the dialog, so Esc closes the window instead of doing nothing.
//
// CONTRACTS
//   B1  every std::min/max here is parenthesised (a consumer TU may include
//       windows.h without NOMINMAX).
//   B2  this header includes <cstdint> for the int64_t it uses.
//   B9  this is Windows-only code; nothing here runs on a worker thread, so the
//       worker/UI boundary is never crossed (the dialog owns its whole loop).
//   D1  nothing skips: a value the probe cannot answer becomes "unknown" in the
//       report, and a clipboard/file failure is REPORTED (lastError()), never
//       silently ignored.
//   D2  the save path is asserted on the FILE CONTENT, not on "a file exists".
//   D3  PRISM_C57_MUTANT_HARDCODED_VERSION replaces the VERSIONINFO read with a
//       literal and bumps a counter, so the suite can prove the injection fired.
//   D7  no second report formatter and no second 7z.dll resolver: BuildReport()
//       and Resolve7zDll() are called, not re-implemented.
//   D26 the mutant entries use the CAUGHT marker as a conjunction, never
//       WILL_FAIL (see Tests/suites/c57-about.cmake).
//   D27 the marker is printed from mini_test's mutant verdict at the first
//       failure, with the "injection fired" predicate.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/DiagnosticsReport.hpp"
#include "CodecProbe.hpp"         // ReadFileVersion: the VERSIONINFO reader (B/D7)
#include "DialogChrome.hpp"       // the V3 glass shell this dialog is built from
#include "PrismEngineBridge.hpp"  // Resolve7zDll: which engine the machine has

// The two OS services this header adds beyond DialogChrome's list: version.lib
// reads the VERSIONINFO, comdlg32.lib is the "Save as..." picker. Declared here
// so a TU that only includes this dialog still links (same trade DialogChrome
// makes for d2d1/dwrite).
#pragma comment(lib, "version.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

#if defined(PRISM_C57_MUTANT_HARDCODED_VERSION)
// C57 MUTANT ACTIVE: hardcoded_version -- the version stops coming from the
// VERSIONINFO block and becomes a literal, so the suite's fixture (9.8.7.6)
// must catch it. The counter below is the D3 predicate the verdict uses.
#define PRISM_C57_HARDCODED_VERSION_MUTANT 1
#endif

namespace prism::win::about {

// ---------------------------------------------------------------------------
// 1. Deliberate-defect bookkeeping (CONTRACTS D3)
// ---------------------------------------------------------------------------
inline int& VersionMutantCount() {
  static int count = 0;
  return count;
}
inline bool VersionMutantFired() { return VersionMutantCount() != 0; }
inline void ResetVersionMutant() { VersionMutantCount() = 0; }

// ---------------------------------------------------------------------------
// 2. Text and machine facts
// ---------------------------------------------------------------------------
inline std::wstring OwnModulePath() {
  // 32k: a long-path install must not turn the version line into "unknown".
  wchar_t buffer[32768] = {};
  const DWORD length = ::GetModuleFileNameW(nullptr, buffer, 32768);
  if (length == 0 || length >= 32768) return std::wstring();
  return std::wstring(buffer, length);
}

// The version this build IS. One implementation, and it reads the file:
// the VERSIONINFO block of 'modulePath' (by default this executable, whose
// resource is Src/App/PrismArchive.rc). Empty when the file has no VERSIONINFO,
// which the report prints as "unknown" (D1) instead of inventing a number.
inline std::string VersionText(const std::wstring& modulePath) {
#if defined(PRISM_C57_MUTANT_HARDCODED_VERSION)
  // MUTANT (D3): a literal, exactly the defect the 9.8.7.6 fixture catches.
  static_cast<void>(modulePath);
  ++VersionMutantCount();
  return "0.1.0.0";
#else
  if (modulePath.empty()) return std::string();
  return prism::win::ReadFileVersion(modulePath);
#endif
}

// "10.0.22631" from RtlGetVersion: GetVersionExW lies without a compatibility
// manifest, and a report that says 6.2 on Windows 11 explains nothing.
inline std::string OsVersionText() {
  const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return std::string();
  using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
  const auto fn = reinterpret_cast<RtlGetVersionFn>(
      reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
  if (fn == nullptr) return std::string();
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (fn(&info) != 0) return std::string();
  return std::to_string(info.dwMajorVersion) + "." + std::to_string(info.dwMinorVersion) + "." +
         std::to_string(info.dwBuildNumber);
}

inline std::string CpuArchText() {
  SYSTEM_INFO info{};
  ::GetNativeSystemInfo(&info);
  switch (info.wProcessorArchitecture) {
    case PROCESSOR_ARCHITECTURE_AMD64: return "x64";
    case PROCESSOR_ARCHITECTURE_INTEL: return "x86";
    case PROCESSOR_ARCHITECTURE_ARM: return "arm";
#ifdef PROCESSOR_ARCHITECTURE_ARM64
    case PROCESSOR_ARCHITECTURE_ARM64: return "arm64";
#endif
    default: return "unknown";
  }
}

// The UI language the process would use. A tag ("zh-CN"), not a display name:
// the report is read by a developer, not by the locale.
inline std::string LocaleText() {
  wchar_t buffer[LOCALE_NAME_MAX_LENGTH] = {};
  const int length = ::GetUserDefaultLocaleName(buffer, LOCALE_NAME_MAX_LENGTH);
  if (length <= 0) return std::string();
  return prism::win::WideToUtf8(std::wstring_view(buffer, static_cast<std::size_t>(length - 1)));
}

inline std::string EnvironmentText(const wchar_t* name) {
  const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
  if (needed == 0) return std::string();
  std::wstring value(static_cast<std::size_t>(needed), L'\0');
  const DWORD written = ::GetEnvironmentVariableW(name, value.data(), needed);
  if (written == 0 || written >= needed) return std::string();
  value.resize(written);
  return prism::win::WideToUtf8(value);
}

inline bool RegistryKeyExists(HKEY root, const wchar_t* subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
  ::RegCloseKey(key);
  return true;
}

inline std::wstring RegistryDefaultString(HKEY root, const wchar_t* subkey) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, subkey, 0, KEY_READ, &key) != ERROR_SUCCESS) return std::wstring();
  wchar_t buffer[512] = {};
  DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG status = ::RegQueryValueExW(key, nullptr, nullptr, &type,
                                         reinterpret_cast<BYTE*>(buffer), &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::wstring();
  buffer[bytes / sizeof(wchar_t)] = L'\0';
  return std::wstring(buffer);
}

// "the .7z association points at us": HKCU\Software\Classes\.7z's default value
// is our ProgID. Read the VALUE, not just the key -- an empty .7z key is exactly
// the half-registered state this report exists to expose.
inline bool ProgIdRegistered() {
  const std::wstring value = RegistryDefaultString(HKEY_CURRENT_USER, L"Software\\Classes\\.7z");
  if (value.empty()) return false;
  return ::CompareStringOrdinal(value.c_str(), -1, L"PrismArchive.7z", -1, TRUE) == CSTR_EQUAL;
}

// The static right-click verbs tools/register-static-verbs.ps1 writes. Either
// one is enough: both are registered together, and one surviving after a manual
// cleanup still means the menu will appear.
inline bool MenuVerbRegistered() {
  return RegistryKeyExists(HKEY_CURRENT_USER,
                           L"Software\\Classes\\*\\shell\\PrismArchiveExtract\\command") ||
         RegistryKeyExists(HKEY_CURRENT_USER,
                           L"Software\\Classes\\*\\shell\\PrismArchiveOpen\\command");
}

inline std::string InstallModeText(std::string_view exePath) {
  return prism::diagnostics::InstallModeName(prism::diagnostics::DetectInstallMode(
      exePath, EnvironmentText(L"LOCALAPPDATA"), false));
}

inline int64_t NowEpochMillis() {
  FILETIME stamp{};
  ::GetSystemTimeAsFileTime(&stamp);
  ULARGE_INTEGER ticks{};
  ticks.LowPart = stamp.dwLowDateTime;
  ticks.HighPart = stamp.dwHighDateTime;
  constexpr uint64_t kUnixEpochAsFileTime = 116444736000000000ull;  // 1601 -> 1970, 100 ns
  if (ticks.QuadPart < kUnixEpochAsFileTime) return 0;
  return static_cast<int64_t>((ticks.QuadPart - kUnixEpochAsFileTime) / 10000ull);
}

// Everything the dialog needs from the machine, as data. Split out so the suite
// can drive the dialog with a fixture (a version module, a fake engine path)
// without touching the real installation.
struct AboutFacts {
  std::wstring versionModule;  // whose VERSIONINFO names the version ("" = none)
  std::wstring enginePath;     // the 7z.dll that was found ("" = none)
  std::string osVersion;
  std::string cpuArch;
  std::string language;
  std::string userName;  // a redaction NEEDLE, never printed
  std::string installMode;  // overwritten by FillFromMachine unless preset
  bool progIdRegistered = false;
  bool menuVerbRegistered = false;
  int64_t generatedAtMs = 0;
};

inline AboutFacts ProbeFacts() {
  AboutFacts facts;
  facts.versionModule = OwnModulePath();
  facts.osVersion = OsVersionText();
  facts.cpuArch = CpuArchText();
  facts.language = LocaleText();
  facts.userName = EnvironmentText(L"USERNAME");
  facts.progIdRegistered = ProgIdRegistered();
  facts.menuVerbRegistered = MenuVerbRegistered();
  facts.generatedAtMs = NowEpochMillis();
  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  facts.enginePath = lookup.path;
  facts.installMode =
      InstallModeText(prism::win::WideToUtf8(facts.versionModule));
  return facts;
}

// The report the dialog shows, built from the facts. This is the ONLY place the
// two halves meet; everything identifying is filtered by PrismCore's Redactor
// (CONTRACTS D25) -- the dialog never writes a path into the text itself.
inline prism::diagnostics::Snapshot SnapshotFrom(const AboutFacts& facts) {
  prism::diagnostics::Snapshot snapshot;
  snapshot.version = VersionText(facts.versionModule);
  snapshot.osVersion = facts.osVersion;
  snapshot.cpuArch = facts.cpuArch;
  snapshot.enginePath = prism::win::WideToUtf8(facts.enginePath);
  snapshot.engineVersion = facts.enginePath.empty()
                               ? std::string()
                               : prism::win::ReadFileVersion(facts.enginePath);
  snapshot.engineFormatCount = -1;  // "unknown": this screen does not load the engine
  snapshot.installMode = facts.installMode == "installed"
                             ? prism::diagnostics::InstallMode::kInstalled
                         : facts.installMode == "portable"
                             ? prism::diagnostics::InstallMode::kPortable
                             : prism::diagnostics::InstallMode::kUnknown;
  snapshot.progIdRegistered = facts.progIdRegistered;
  snapshot.menuHandlerRegistered = facts.menuVerbRegistered;
  snapshot.language = facts.language;
  snapshot.theme = "system";  // the dialog itself is the glass shell, not the app theme
  snapshot.userName = facts.userName;
  snapshot.generatedAtMs = facts.generatedAtMs;
  return snapshot;
}

inline std::string ReportFrom(const AboutFacts& facts) {
  return prism::diagnostics::BuildReport(SnapshotFrom(facts));
}

// ---------------------------------------------------------------------------
// 3. Control ids and geometry (DESIGN.md section 2/4, the V3 card)
// ---------------------------------------------------------------------------
namespace ids {
inline constexpr int kReport = 4101;   // the read-only multiline report
inline constexpr int kCopy = 4102;     // "Copy to clipboard"
inline constexpr int kSave = 4103;     // "Save as file..."
}  // namespace ids

inline constexpr int kClientWidth = 640;
inline constexpr int kClientHeight = 480;
// The chrome's focus cycle is exactly the two buttons: the report is the visible
// control, but it is not a "widget" the card paints, and the caret lives there.
inline constexpr int kButtonCount = 2;

// ---------------------------------------------------------------------------
// 4. The dialog
// ---------------------------------------------------------------------------
class AboutDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveAboutDialog";
  // Help > About's command id. The wiring patch (tools/patches/c57-about-menu.patch)
  // puts this on the window's menu bar; the id is part of the seam, like C31's.
  static constexpr UINT kMenuAboutId = 0x4B57;

  // Where "Save as..." writes. The default is the common file dialog; a test
  // installs one that returns a temp path, so the BUTTON path is exercised
  // without automating a modal dialog.
  using SavePathHandler = std::function<std::wstring(HWND owner)>;

  // The machine facts are read once, here: the report is a snapshot, and a
  // dialog that re-probed while it was open would show a different answer to
  // the same question. SetFacts() replaces them wholesale (the suite does).
  AboutDialog() { facts_ = ProbeFacts(); }
  ~AboutDialog() { Destroy(); }
  AboutDialog(const AboutDialog&) = delete;
  AboutDialog& operator=(const AboutDialog&) = delete;

  // ---- seams -----------------------------------------------------------
  void SetFacts(AboutFacts facts) {
    facts_ = std::move(facts);
    hasReport_ = false;
    report_.clear();
  }
  const AboutFacts& facts() const { return facts_; }

  // The module whose VERSIONINFO the report names. Empty = the running exe.
  void SetVersionModule(std::wstring path) {
    facts_.versionModule = std::move(path);
    hasReport_ = false;
  }
  void SetEnginePath(std::wstring path) {
    facts_.enginePath = std::move(path);
    hasReport_ = false;
  }
  void SetSavePathHandler(SavePathHandler handler) { savePath_ = std::move(handler); }

  // A completely pre-built report (the suite uses it to pin the copy/save
  // paths on a known text). An empty override falls back to the live probe.
  void SetReport(std::string utf8) {
    report_ = std::move(utf8);
    hasReport_ = !report_.empty();
  }

  // What the report control holds, UTF-8. This is what Copy and Save move, so
  // "the clipboard equals the control" and "the file equals the control" are
  // assertions about ONE string.
  std::string ReportText() const {
    const HWND report = Control(ids::kReport);
    if (report != nullptr) {
      const int length = ::GetWindowTextLengthW(report);
      std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
      ::GetWindowTextW(report, buffer.data(), length + 1);
      buffer.resize(static_cast<std::size_t>(length));
      return prism::win::WideToUtf8(buffer);
    }
    return ComposeReport();
  }
  std::wstring ReportWide() const { return prism::win::Utf8ToWide(ReportText()); }

  // ---- the two verbs ---------------------------------------------------
  bool CopyReportToClipboard() {
    lastError_ = 0;
    const std::wstring text = ReportWide();
    // D29: a bounded retry. Another process holding the clipboard is a race we
    // survive; waiting forever is not.
    bool opened = false;
    for (int attempt = 0; attempt < 20 && !opened; ++attempt) {
      opened = ::OpenClipboard(hwnd_) != FALSE;
      if (!opened) {
        lastError_ = ::GetLastError();
        ::Sleep(10);
      }
    }
    if (!opened) return false;
    if (!::EmptyClipboard()) {
      lastError_ = ::GetLastError();
      ::CloseClipboard();
      return false;
    }
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (block == nullptr) {
      lastError_ = ::GetLastError();
      ::CloseClipboard();
      return false;
    }
    void* target = ::GlobalLock(block);
    if (target == nullptr) {
      lastError_ = ::GetLastError();
      ::GlobalFree(block);
      ::CloseClipboard();
      return false;
    }
    std::memcpy(target, text.c_str(), bytes);
    ::GlobalUnlock(block);
    if (::SetClipboardData(CF_UNICODETEXT, block) == nullptr) {
      lastError_ = ::GetLastError();
      ::GlobalFree(block);
      ::CloseClipboard();
      return false;
    }
    ::CloseClipboard();
    return true;
  }

  // Writes EXACTLY the control's text as UTF-8 (no BOM, no added newline), which
  // is what makes "byte for byte equal to the control" a meaningful assertion
  // (CONTRACTS D2: content, not existence).
  bool SaveReportToFile(const std::wstring& path) {
    lastError_ = 0;
    const std::string bytes = ReportText();
    HANDLE file = ::CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
      lastError_ = ::GetLastError();
      return false;
    }
    DWORD written = 0;
    bool ok = true;
    if (!bytes.empty()) {
      ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) !=
           FALSE;
    }
    ::CloseHandle(file);
    if (!ok || written != bytes.size()) {
      if (lastError_ == 0) lastError_ = ::GetLastError();
      ::DeleteFileW(path.c_str());  // never leave a half-written report behind
      return false;
    }
    savedPath_ = path;
    return true;
  }

  // ---- window ----------------------------------------------------------
  bool Create(HINSTANCE instance, HWND owner, const std::wstring& title = L"About PrismArchive") {
    instance_ = instance;
    owner_ = owner;
    if (!hasReport_) {
      report_ = ReportFrom(facts_);  // the snapshot was taken in the constructor
      hasReport_ = true;
    }
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &AboutDialog::ProcThunk;
    wc.hInstance = instance;
    wc.hCursor = ::LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, wide on purpose
    wc.lpszClassName = kClassName;
    if (::RegisterClassExW(&wc) == 0) {
      lastError_ = ::GetLastError();
      if (lastError_ != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    dpi_ = chrome::CurrentDpi(nullptr);
    const float scale = static_cast<float>(dpi_) / 96.0f;
    hwnd_ = ::CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title.c_str(),
                              WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(std::lround(kClientWidth * scale)),
                              static_cast<int>(std::lround(kClientHeight * scale)), owner, nullptr,
                              instance, this);
    if (hwnd_ == nullptr) {
      lastError_ = ::GetLastError();
      return false;
    }
    ::ShowWindow(hwnd_, SW_SHOW);
    ::UpdateWindow(hwnd_);
    return true;
  }

  void Destroy() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE) ::DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    reportControl_ = nullptr;
    copyButton_ = nullptr;
    saveButton_ = nullptr;
    ReleaseResources();
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? ::GetDlgItem(hwnd_, id) : nullptr; }

  // Modal driver: the caller is one line, the owner is disabled exactly once.
  bool OpenModal(HINSTANCE instance, HWND owner) {
    if (!Create(instance, owner)) return false;
    if (owner_ != nullptr) ::EnableWindow(owner_, FALSE);
    MSG message{};
    while (hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE) {
      const BOOL got = ::GetMessageW(&message, nullptr, 0, 0);
      if (got <= 0) break;  // WM_QUIT belongs to the app, not to this loop
      ::TranslateMessage(&message);
      ::DispatchMessageW(&message);
    }
    if (owner_ != nullptr && ::IsWindow(owner_) != FALSE) ::EnableWindow(owner_, TRUE);
    return true;
  }

  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  // ---- keyboard --------------------------------------------------------
  int FocusIndex() const { return focus_; }
  int FocusedId() const {
    const chrome::ChromeModel model = BuildModel();
    const chrome::Widget* focused = model.Focused();
    return focused != nullptr ? focused->id : -1;
  }
  void FocusFirst() {
    focus_ = 0;
    RefreshChrome();
  }
  // Returns true when the dialog consumed the key.
  bool DispatchKey(UINT vk, bool shift) {
    const chrome::KeyCommand command = chrome::ClassifyKey(vk, shift, false, false);
    switch (command) {
      case chrome::KeyCommand::Next:
        focus_ = chrome::NextFocus(focus_, kButtonCount, false);
        if (hwnd_ != nullptr) ::SetFocus(hwnd_);  // the caret leaves the report
        RefreshChrome();
        return true;
      case chrome::KeyCommand::Previous:
        focus_ = chrome::NextFocus(focus_, kButtonCount, true);
        if (hwnd_ != nullptr) ::SetFocus(hwnd_);
        RefreshChrome();
        return true;
      case chrome::KeyCommand::Activate: {
        const int id = FocusedId();
        if (id < 0) {
          FocusFirst();  // the first Tab-less Enter selects Copy, it does not run it
          return true;
        }
        Activate(id);
        return true;
      }
      case chrome::KeyCommand::Cancel:
        if (hwnd_ != nullptr) ::SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return true;
      case chrome::KeyCommand::Increase:
      case chrome::KeyCommand::Decrease:
      case chrome::KeyCommand::None:
        break;
    }
    return false;
  }

  DWORD lastError() const { return lastError_; }
  const std::wstring& savedPath() const { return savedPath_; }
  const std::wstring& hint() const { return hint_; }

  // What the chrome paints: the card, the title bar and the two buttons. Public
  // so a pixel/layout assertion does not have to re-derive the geometry.
  chrome::ChromeModel BuildModel() const {
    using chrome::WidgetKind;
    chrome::ChromeModel model;
    model.variant = prism::theme::Variant::Midnight;
    model.dpi = dpi_;
    model.width = static_cast<float>(kClientWidth);
    model.height = static_cast<float>(kClientHeight);
    model.title = L"About PrismArchive";
    model.footer = hint_;
    model.footerError = hintError_;

    const chrome::Metrics metrics = model.metrics();
    const float pad = static_cast<float>(metrics.padding);
    const float buttonWidth = 168.0f;
    const float buttonY = static_cast<float>(kClientHeight - metrics.actionBarHeight) +
                          static_cast<float>((metrics.actionBarHeight - metrics.buttonHeight) / 2);
    const float saveLeft = static_cast<float>(kClientWidth) - pad - buttonWidth;

    int index = 0;
    auto button = [&](int id, const wchar_t* label, float left, bool primary) {
      chrome::Widget widget;
      widget.id = id;
      widget.kind = WidgetKind::Button;
      widget.rect = D2D1::RectF(left, buttonY, left + buttonWidth,
                                buttonY + static_cast<float>(metrics.buttonHeight));
      widget.label = label;
      widget.primary = primary;
      widget.focused = focus_ == index++;
      widget.state = chrome::ResolveState(true, hot_ == id, pressed_ == id, false);
      model.widgets.push_back(std::move(widget));
    };
    button(ids::kCopy, L"Copy to clipboard", saveLeft - 12.0f - buttonWidth, true);
    button(ids::kSave, L"Save as file...", saveLeft, false);
    return model;
  }

  // The report rectangle in DIPs: the one visible control's geometry, shared by
  // CreateChildren and the layout assertion.
  static D2D1_RECT_F ReportRect() {
    constexpr float top = 48.0f;
    constexpr float bottom = 480.0f - 56.0f - 8.0f;
    return D2D1::RectF(16.0f, top, 640.0f - 16.0f, bottom);
  }

  void RefreshChrome() {
    if (hwnd_ != nullptr && ::IsWindow(hwnd_) != FALSE) ::InvalidateRect(hwnd_, nullptr, FALSE);
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<AboutDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<AboutDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // WM_CREATE runs INSIDE CreateWindowExW, before Create() can assign the
      // handle: without this line every child control is created parentless.
      self->hwnd_ = hwnd;
    }
    if (self == nullptr) return ::DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }

  // The report edit's own window proc: Tab / Enter / Esc belong to the dialog
  // even while the caret is inside the text, so the window stays keyboard
  // reachable without a dialog manager eating the keys.
  static LRESULT CALLBACK ReportProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<AboutDialog*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self != nullptr && message == WM_KEYDOWN) {
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (self->DispatchKey(static_cast<UINT>(wparam), shift)) return 0;
    }
    WNDPROC previous = self != nullptr ? self->reportProc_ : &::DefWindowProcW;
    return ::CallWindowProcW(previous, hwnd, message, wparam, lparam);
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
  bool CreateChildren();
  void Activate(int id);
  void SetHint(std::wstring text, bool error);
  void PaintChrome();
  void ReleaseResources();
  std::string ComposeReport() const {
    if (hasReport_) return report_;
    return ReportFrom(facts_);
  }

  HINSTANCE instance_ = nullptr;
  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND reportControl_ = nullptr;
  HWND copyButton_ = nullptr;
  HWND saveButton_ = nullptr;
  HBRUSH background_ = nullptr;
  HFONT reportFont_ = nullptr;
  WNDPROC reportProc_ = nullptr;

  AboutFacts facts_;
  std::string report_;
  bool hasReport_ = false;
  SavePathHandler savePath_;
  std::wstring hint_;
  bool hintError_ = false;
  std::wstring savedPath_;
  DWORD lastError_ = 0;
  int dpi_ = 96;
  int focus_ = -1;
  int hot_ = -1;
  int pressed_ = -1;
  bool closeHover_ = false;
  chrome::ChromePainter painter_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
};

// ---------------------------------------------------------------------------
// 5. Implementation (header-only: PrismWin is an INTERFACE library)
// ---------------------------------------------------------------------------
namespace detail {

inline COLORREF ToColorRef(const prism::theme::Rgba& c) {
  const auto channel = [](float value) {
    const float clamped = (std::max)(0.0f, (std::min)(1.0f, value));
    return static_cast<BYTE>(clamped * 255.0f + 0.5f);
  };
  return RGB(channel(c.r), channel(c.g), channel(c.b));
}

// The common Save-as picker. A cancelled picker returns an empty path, which the
// caller treats as "no decision", never as a failure (D1: report real failures).
inline std::wstring AskForSavePath(HWND owner) {
  wchar_t file[1024] = L"PrismArchive-diagnostics.txt";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = owner;
  ofn.lpstrFilter = L"Text file (*.txt)\0*.txt\0All files (*.*)\0*.*\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = static_cast<DWORD>(sizeof(file) / sizeof(file[0]));
  ofn.lpstrDefExt = L"txt";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if (::GetSaveFileNameW(&ofn) == FALSE) return std::wstring();
  return std::wstring(file);
}

}  // namespace detail

inline bool AboutDialog::CreateChildren() {
  const float scale = static_cast<float>(dpi_) / 96.0f;
  const D2D1_RECT_F report = ReportRect();
  reportControl_ = ::CreateWindowExW(
      WS_EX_STATICEDGE, L"EDIT", L"",
      WS_CHILD | WS_VISIBLE | ES_LEFT | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL |
          WS_VSCROLL,
      static_cast<int>(std::lround(report.left * scale)),
      static_cast<int>(std::lround(report.top * scale)),
      static_cast<int>(std::lround((report.right - report.left) * scale)),
      static_cast<int>(std::lround((report.bottom - report.top) * scale)), hwnd_,
      reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kReport)), instance_, nullptr);
  if (reportControl_ == nullptr) {
    lastError_ = ::GetLastError();
    return false;
  }
  // The text comes from the composed report, NOT from ReportWide(): the control
  // was just created empty, so reading it back here would install an empty
  // report (and "the clipboard equals the control" would then prove nothing).
  const std::wstring text = prism::win::Utf8ToWide(ComposeReport());
  ::SetWindowTextW(reportControl_, text.c_str());
  reportFont_ = ::CreateFontW(-::MulDiv(13, dpi_, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                              DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
  if (reportFont_ != nullptr) {
    ::SendMessageW(reportControl_, WM_SETFONT, reinterpret_cast<WPARAM>(reportFont_), TRUE);
  }
  // The edit forwards Tab/Enter/Esc to the dialog (see ReportProcThunk). The
  // user data must be in place BEFORE the new proc: the first message it sees
  // may already be a key.
  ::SetWindowLongPtrW(reportControl_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  reportProc_ = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(
      reportControl_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AboutDialog::ReportProcThunk)));

  // The two buttons are real controls so BM_CLICK and WM_COMMAND drive the same
  // code the user does; they stay hidden because the chrome paints them (the
  // C31 shape).
  copyButton_ = ::CreateWindowExW(0, L"BUTTON", L"Copy to clipboard", WS_CHILD, 0, 0, 10, 10,
                                  hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kCopy)),
                                  instance_, nullptr);
  saveButton_ = ::CreateWindowExW(0, L"BUTTON", L"Save as file...", WS_CHILD, 0, 0, 10, 10, hwnd_,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids::kSave)),
                                  instance_, nullptr);
  if (copyButton_ == nullptr || saveButton_ == nullptr) {
    lastError_ = ::GetLastError();
    return false;
  }
  const prism::theme::Rgba card = chrome::Card(prism::theme::Variant::Midnight);
  background_ = ::CreateSolidBrush(detail::ToColorRef(card));
  return true;
}

inline void AboutDialog::ReleaseResources() {
  if (background_ != nullptr) {
    ::DeleteObject(background_);
    background_ = nullptr;
  }
  if (reportFont_ != nullptr) {
    ::DeleteObject(reportFont_);
    reportFont_ = nullptr;
  }
  reportProc_ = nullptr;
  target_.Reset();
}

inline void AboutDialog::SetHint(std::wstring text, bool error) {
  hint_ = std::move(text);
  hintError_ = error;
  RefreshChrome();
}

inline void AboutDialog::Activate(int id) {
  switch (id) {
    case ids::kCopy: {
      const bool copied = CopyReportToClipboard();
      SetHint(copied ? L"Copied to the clipboard" : L"Could not open the clipboard", !copied);
      break;
    }
    case ids::kSave: {
      const std::wstring path = savePath_ ? savePath_(hwnd_) : detail::AskForSavePath(hwnd_);
      if (path.empty()) return;  // cancelled: not an error, not a silent success
      if (SaveReportToFile(path)) {
        SetHint(L"Saved " + path, false);
      } else {
        SetHint(L"Could not write the file (error " + std::to_wstring(lastError_) + L")", true);
      }
      break;
    }
    default:
      break;
  }
}

inline void AboutDialog::PaintChrome() {
  PAINTSTRUCT paint{};
  HDC dc = ::BeginPaint(hwnd_, &paint);
  bool painted = false;
  if (SUCCEEDED(painter_.Initialize())) {
    if (!target_) {
      RECT client{};
      ::GetClientRect(hwnd_, &client);
      const D2D1_SIZE_U size = D2D1::SizeU(static_cast<UINT32>(client.right - client.left),
                                           static_cast<UINT32>(client.bottom - client.top));
      const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f, 96.0f);
      const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
          D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_NONE);
      if (SUCCEEDED(painter_.Factory()->CreateHwndRenderTarget(
              properties, hwndProperties, target_.ReleaseAndGetAddressOf()))) {
        target_->SetDpi(96.0f, 96.0f);
      }
    }
    if (target_) {
      target_->BeginDraw();
      painter_.Draw(target_.Get(), BuildModel());
      if (SUCCEEDED(target_->EndDraw())) painted = true;
    }
  }
  if (!painted) {
    // A headless/remote session can fail to create a D2D target; the card must
    // still be a card, not a white rectangle.
    RECT client{};
    ::GetClientRect(hwnd_, &client);
    const HBRUSH brush = ::CreateSolidBrush(detail::ToColorRef(
        chrome::Canvas(prism::theme::Variant::Midnight)));
    ::FillRect(dc, &client, brush);
    ::DeleteObject(brush);
  }
  ::EndPaint(hwnd_, &paint);
}

inline LRESULT AboutDialog::HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  if (hwnd == nullptr) return 0;
  switch (message) {
    case WM_CREATE:
      // A child that cannot be created is OUR bug: fail the creation so Create()
      // reports it instead of opening a window with no report in it.
      return CreateChildren() ? 0 : -1;
    case WM_PAINT:
      PaintChrome();
      return 0;
    case WM_ERASEBKGND:
      return 1;
    case WM_SIZE:
      if (target_) {
        target_->Resize(D2D1::SizeU(static_cast<UINT32>(LOWORD(lparam)),
                                    static_cast<UINT32>(HIWORD(lparam))));
      }
      return 0;
    case WM_DPICHANGED: {
      dpi_ = HIWORD(wparam);
      const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
      if (suggested != nullptr) {
        ::SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
      }
      target_.Reset();
      RefreshChrome();
      return 0;
    }
    case WM_MOUSEMOVE: {
      TRACKMOUSEEVENT track{};
      track.cbSize = sizeof(track);
      track.dwFlags = TME_LEAVE;
      track.hwndTrack = hwnd_;
      ::TrackMouseEvent(&track);
      const chrome::ChromeModel model = BuildModel();
      const bool closeHover = chrome::HitCloseScreen(model, static_cast<float>(GET_X_LPARAM(lparam)),
                                                     static_cast<float>(GET_Y_LPARAM(lparam)));
      const int hot = chrome::HitTestScreen(model, static_cast<float>(GET_X_LPARAM(lparam)),
                                            static_cast<float>(GET_Y_LPARAM(lparam)));
      if (hot != hot_ || closeHover != closeHover_) {
        hot_ = hot;
        closeHover_ = closeHover;
        RefreshChrome();
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      hot_ = -1;
      closeHover_ = false;
      pressed_ = -1;
      RefreshChrome();
      return 0;
    case WM_LBUTTONDOWN: {
      ::SetCapture(hwnd_);
      const chrome::ChromeModel model = BuildModel();
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      if (chrome::HitCloseScreen(model, x, y)) {
        pressed_ = -2;
        RefreshChrome();
        return 0;
      }
      if (chrome::HitTitleScreen(model, x, y)) {
        ::ReleaseCapture();
        ::SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
        return 0;
      }
      const int hit = chrome::HitTestScreen(model, x, y);
      if (hit >= 0) {
        pressed_ = hit;
        focus_ = hit == ids::kCopy ? 0 : 1;
        RefreshChrome();
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      const chrome::ChromeModel model = BuildModel();
      const float x = static_cast<float>(GET_X_LPARAM(lparam));
      const float y = static_cast<float>(GET_Y_LPARAM(lparam));
      const bool wasClose = pressed_ == -2;
      if (::GetCapture() == hwnd_) ::ReleaseCapture();
      if (wasClose) {
        pressed_ = -1;
        if (chrome::HitCloseScreen(model, x, y)) ::SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return 0;
      }
      const int hit = chrome::HitTestScreen(model, x, y);
      const int pressed = pressed_;
      pressed_ = -1;
      if (hit >= 0 && hit == pressed) Activate(hit);
      RefreshChrome();
      return 0;
    }
    case WM_KEYDOWN: {
      const bool shift = (::GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (DispatchKey(static_cast<UINT>(wparam), shift)) return 0;
      return ::DefWindowProcW(hwnd, message, wparam, lparam);
    }
    case WM_COMMAND: {
      const int id = LOWORD(wparam);
      const int code = HIWORD(wparam);
      if (code == BN_CLICKED) {
        Activate(id);
        return 0;
      }
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      if (reinterpret_cast<HWND>(lparam) == reportControl_ && background_ != nullptr) {
        const HDC dc = reinterpret_cast<HDC>(wparam);
        ::SetTextColor(dc, detail::ToColorRef(chrome::TextPrimary(prism::theme::Variant::Midnight)));
        ::SetBkColor(dc, detail::ToColorRef(chrome::Card(prism::theme::Variant::Midnight)));
        return reinterpret_cast<LRESULT>(background_);
      }
      break;
    }
    case WM_CLOSE:
      ::DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      // The window is gone; the modal loop watches hwnd_ and Create() must be
      // callable again, so every HWND here is invalidated exactly once.
      ReleaseResources();
      hwnd_ = nullptr;
      reportControl_ = nullptr;
      copyButton_ = nullptr;
      saveButton_ = nullptr;
      return 0;
    default:
      break;
  }
  return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace prism::win::about

