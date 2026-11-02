// PrismArchive -- the password-vault manager driven through REAL HWNDs (card C56).
//
// The rules live in Src/PrismCore/VaultAdminModel.hpp and are proved headless by
// Tests/test_vault_admin.cpp. This file proves the WIRING and the FILES:
//   * the dialog is built from code (no .rc): every control is a real HWND, and
//     the list shows one row per stored credential;
//   * NO control text, NO listbox item and NO window title ever contains a
//     plaintext or a >= 4-char fragment of one (read back with GetWindowTextW /
//     LB_GETTEXT, CONTRACTS D2);
//   * the vault it reads is the REAL DPAPI file C5's DpapiVault writes (so the
//     on-disk layout contract is proved across both implementations), and after
//     a delete the FILE BYTES changed and C5 reads one entry FEWER;
//   * clear is two clicks: the first one leaves the file and the list untouched,
//     the second one empties both;
//   * a corrupt vault opens and says "unreadable" instead of crashing, and every
//     destructive button is disabled (an admin action must not rewrite data it
//     could not parse);
//   * a blob this machine cannot decrypt is skipped individually and counted,
//     the good entries still list, and the orphan blob survives a later edit;
//   * keyboard: Tab walks the tab order, Delete deletes, Enter renames, Esc
//     closes.
//
// SKIP policy (CONTRACTS D1): a window station that refuses CreateWindowExW is a
// PRINTED skip with exit 77; the suite registers SKIP_RETURN_CODE 77 AND
// PASS_REGULAR_EXPRESSION, so a silent skip can never count as a pass.
//
// Negative controls (CONTRACTS D3, registered in Tests/suites/c56-vaultui.cmake):
//   PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT    the model's mask returns plaintext.
//   PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM  the first clear click erases.
// main() arms "C56 VAULT-UI MUTANT CAUGHT" BEFORE the first assertion (D27) with
// the injection predicate (D3: red for an unrelated reason must NOT claim
// CAUGHT); mini_test prints it at the first failed check and leaves with 0.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include "../mini_test.hpp"
#include "UniqueTempDir.hpp"
#include "../../Src/PrismCore/VaultAdminModel.hpp"
#include "../../Src/PrismWin/DpapiVault.hpp"
#include "../../Src/PrismWin/VaultDialog.hpp"

namespace {

using prism::vaultadmin::FormatEntryRow;
using prism::vaultadmin::VaultAdminState;
using prism::win::DpapiVault;
namespace ui = prism::win::vaultui;

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// Content-level leak check (CONTRACTS D2/D5): the full secret OR any fragment of
// FOUR OR MORE CODE POINTS is a leak. Counting code points (not bytes) keeps the
// documented two-character preview legal: for a CJK secret the last two
// characters are six bytes, and a byte-wise scan would call the mask itself a
// leak. For ASCII secrets this is exactly the C35 rule.
bool Leaks(const std::string& haystack, const std::string& secret) {
  if (secret.empty()) return false;
  if (Contains(haystack, secret)) return true;
  std::vector<std::size_t> starts;
  starts.push_back(0);
  for (std::size_t i = 0; i < secret.size(); ++i) {
    if ((static_cast<unsigned char>(secret[i]) & 0xC0u) != 0x80u && i != 0) starts.push_back(i);
  }
  starts.push_back(secret.size());
  const std::size_t chars = starts.size() - 1;
  if (chars < 4) return false;
  for (std::size_t i = 0; i + 4 <= chars; ++i) {
    for (std::size_t j = i + 4; j <= chars; ++j) {
      if (Contains(haystack, secret.substr(starts[i], starts[j] - starts[i]))) return true;
    }
  }
  return false;
}

std::vector<std::string> SampleSecrets() {
  return {"hunter2SuperSecret", "zip-2024-&!pass",
          "\xE5\xAF\x86\xE7\xA0\x81\xE4\xB8\x80\xE4\xBA\x8C"};  // 4 CJK characters
}

#if defined(_WIN32)

std::string gSkipReason;

std::wstring WindowTextOf(HWND hwnd) {
  if (hwnd == nullptr) return std::wstring();
  const int length = GetWindowTextLengthW(hwnd);
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  GetWindowTextW(hwnd, text.data(), length + 1);
  text.resize(static_cast<std::size_t>(length));
  return text;
}

std::string ControlTextUtf8(HWND control) { return ui::Utf8FromWide(WindowTextOf(control)); }

struct LeakScan {
  const std::string* secret = nullptr;
  bool leaked = false;
};

BOOL CALLBACK LeakScanProc(HWND child, LPARAM param) {
  auto* scan = reinterpret_cast<LeakScan*>(param);
  const std::string text = ControlTextUtf8(child);
  if (Leaks(text, *scan->secret)) {
    wchar_t klass[64] = L"?";
    GetClassNameW(child, klass, 64);
    std::printf("    [leak] ctrl id=%d class=%ls text=%s\n", GetDlgCtrlID(child), klass,
                text.c_str());
    scan->leaked = true;
    return FALSE;
  }
  return TRUE;
}

// Every child control, every listbox ITEM, and the window title.
bool LeaksInWindow(HWND hwnd, const std::string& secret) {
  if (hwnd == nullptr) return false;
  if (Leaks(ui::Utf8FromWide(WindowTextOf(hwnd)), secret)) return true;
  LeakScan scan;
  scan.secret = &secret;
  EnumChildWindows(hwnd, LeakScanProc, reinterpret_cast<LPARAM>(&scan));
  if (scan.leaked) return true;
  const HWND list = GetDlgItem(hwnd, ui::ids::kList);
  if (list == nullptr) return false;
  const int count = static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0));
  for (int i = 0; i < count; ++i) {
    const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(i), 0);
    if (length == LB_ERR) continue;
    std::wstring item(static_cast<std::size_t>(length) + 1, L'\0');
    SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(i),
                 reinterpret_cast<LPARAM>(item.data()));
    item.resize(static_cast<std::size_t>(length));
    if (Leaks(ui::Utf8FromWide(item), secret)) {
      std::printf("    [leak] list item %d text=%s\n", i, ui::Utf8FromWide(item).c_str());
      return true;
    }
  }
  return false;
}

bool WindowStationUsable() {
  static const bool usable = [] {
    const HWND probe = CreateWindowExW(0, L"STATIC", L"probe", WS_OVERLAPPED, 0, 0, 10, 10,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (probe == nullptr) return false;
    DestroyWindow(probe);
    return true;
  }();
  return usable;
}

bool EnsureWindowStation() {
  if (!gSkipReason.empty()) return false;
  if (WindowStationUsable()) return true;
  gSkipReason = "no interactive window station (CreateWindowExW on a stock class failed)";
  return false;
}

std::vector<std::uint8_t> ReadBytes(const std::wstring& path) {
  std::vector<std::uint8_t> bytes;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return bytes;
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) != FALSE && size.QuadPart > 0) {
    bytes.resize(static_cast<std::size_t>(size.QuadPart));
    DWORD read = 0;
    if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ==
            FALSE ||
        static_cast<std::size_t>(read) != bytes.size()) {
      bytes.clear();
    }
  }
  CloseHandle(file);
  return bytes;
}

bool WriteBytes(const std::wstring& path, const std::vector<std::uint8_t>& bytes) {
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  bool ok = true;
  if (!bytes.empty()) {
    ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) !=
             FALSE &&
         static_cast<std::size_t>(written) == bytes.size();
  }
  CloseHandle(file);
  return ok;
}

bool ContainsBytes(const std::vector<std::uint8_t>& haystack,
                   const std::vector<std::uint8_t>& needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    if (std::memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) return true;
  }
  return false;
}

std::size_t VaultEntryCount(const std::wstring& path) {
  return DpapiVault(path).List().size();
}

void ClickButton(ui::VaultDialog& dialog, int id) {
  SendMessageW(dialog.Control(id), BM_CLICK, 0, 0);
}

void SelectRow(ui::VaultDialog& dialog, int row) {
  const HWND list = dialog.Control(ui::ids::kList);
  SendMessageW(list, LB_SETCURSEL, static_cast<WPARAM>(row), 0);
  dialog.Dispatch(WM_COMMAND, MAKEWPARAM(ui::ids::kList, LBN_SELCHANGE),
                  reinterpret_cast<LPARAM>(list));
}

int ListCount(HWND list) {
  const LRESULT count = SendMessageW(list, LB_GETCOUNT, 0, 0);
  return count == LB_ERR ? 0 : static_cast<int>(count);
}

std::wstring ListItem(HWND list, int index) {
  const LRESULT length = SendMessageW(list, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
  if (length == LB_ERR) return std::wstring();
  std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
  SendMessageW(list, LB_GETTEXT, static_cast<WPARAM>(index),
               reinterpret_cast<LPARAM>(text.data()));
  text.resize(static_cast<std::size_t>(length));
  return text;
}

// Authors a vault the way the APPLICATION would: C5's DpapiVault, one Add per
// secret. This is the interop half of the test -- the dialog must read a file it
// did not write.
void AuthorVault(const std::wstring& path, const std::vector<std::string>& secrets) {
  DpapiVault vault(path);
  for (const std::string& secret : secrets) vault.Add(secret);
}

// The dialog timestamps come from the sidecar; this helper dates them so the row
// text is not all "never" (the assertion below pins the CONTENT).
void WriteIndex(const std::wstring& path, std::size_t count) {
  std::string text = "prism-vault-index 1\n";
  for (std::size_t i = 0; i < count; ++i) {
    text += "1700000000\t1700000100\t";
    text += std::to_string(i + 1);
    text += "\tLabel ";
    text += std::to_string(i + 1);
    text += "\n";
  }
  std::vector<std::uint8_t> bytes(text.begin(), text.end());
  WriteBytes(ui::IndexPathFor(path), bytes);
}

bool OpenDialog(ui::VaultDialog& dialog, const std::wstring& vaultPath) {
  if (!EnsureWindowStation()) return false;
  if (!dialog.Create(GetModuleHandleW(nullptr), vaultPath, L"Password vault - fixture")) {
    std::printf("    [dialog] Create failed: err=%lu missingControlId=%d\n",
                static_cast<unsigned long>(dialog.lastError()), dialog.missingControlId());
    PRISM_CHECK(false);  // our bug, not the environment: this must be RED
    return false;
  }
  return true;
}

#endif  // _WIN32

}  // namespace

#if defined(_WIN32)

// ===========================================================================
// 1. Programmatic construction + the list
// ===========================================================================

PRISM_TEST("win: the dialog is built from code and lists every stored credential") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_basic");
  if (!dir.ok()) {
    gSkipReason = "the scratch directory could not be created";
    return;
  }
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());
  const std::size_t entries = VaultEntryCount(path);
  PRISM_CHECK_EQ(entries, static_cast<std::size_t>(3));

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;

  WNDCLASSEXW info{};
  info.cbSize = sizeof(info);
  PRISM_CHECK(GetClassInfoExW(GetModuleHandleW(nullptr), ui::VaultDialog::kClassName, &info) !=
              FALSE);

  const int controls[] = {ui::ids::kList,     ui::ids::kRenameEdit, ui::ids::kRenameButton,
                          ui::ids::kDeleteButton, ui::ids::kClearButton, ui::ids::kClose,
                          ui::ids::kStatus,   ui::ids::kHint,       ui::ids::kEntryCount};
  for (const int id : controls) {
    if (!PRISM_CHECK(dialog.Control(id) != nullptr)) {
      std::printf("    [dialog] missing control id %d\n", id);
      return;
    }
  }

  const HWND list = dialog.Control(ui::ids::kList);
  PRISM_CHECK_EQ(ListCount(list), 3);
  PRISM_CHECK_EQ(dialog.Model().state(), VaultAdminState::Ready);
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(3));
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kList));
  // Nothing is selected yet, so no destructive action may be armed.
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kDeleteButton));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kRenameButton));

  // The row is CONTENT (D2): mask, label, timestamps and the hit counter.
  const std::string row = ui::Utf8FromWide(ListItem(list, 0));
  PRISM_CHECK(Contains(row, dialog.Model().views()[0].masked));
  PRISM_CHECK(Contains(row, "Label 1"));
  PRISM_CHECK(Contains(row, "added 2023-11-14 22:13:20Z"));
  PRISM_CHECK(Contains(row, "last used 2023-11-14 22:15:00Z"));
  PRISM_CHECK(Contains(row, "1 hit"));
  PRISM_CHECK_EQ(ui::Utf8FromWide(ListItem(list, 2)),
                 FormatEntryRow(dialog.Model().views()[2]));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kStatus), "3 stored credentials"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kEntryCount), "(3)"));
  PRISM_CHECK(dialog.ControlTextUtf8(ui::ids::kHint).empty());
}

PRISM_TEST("win: no control, list item or title carries a plaintext") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_secrecy");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;

  for (const std::string& secret : secrets) {
    PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret));
  }
  // The mask is really the model's, not an accident of the fixture: the last two
  // code points must be visible and the rest must not.
  const std::string masked = ui::Utf8FromWide(ListItem(dialog.Control(ui::ids::kList), 0));
  PRISM_CHECK(Contains(masked, "et"));  // "...Secret" -> last two: "et"
  PRISM_CHECK(!Contains(masked, "hunter2"));
}

// ===========================================================================
// 2. Delete: the file changes and C5 reads one entry fewer
// ===========================================================================

PRISM_TEST("win: delete removes one row and rewrites the vault file") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_delete");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;
  const std::vector<std::uint8_t> before = ReadBytes(path);
  PRISM_CHECK(!before.empty());

  SelectRow(dialog, 1);
  PRISM_CHECK_EQ(dialog.Model().selected(), 1);
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kDeleteButton));
  ClickButton(dialog, ui::ids::kDeleteButton);

  // The SCREEN changed by exactly one row and kept the survivors' order.
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)), 2);
  PRISM_CHECK_EQ(ui::Utf8FromWide(ListItem(dialog.Control(ui::ids::kList), 0)),
                 FormatEntryRow(dialog.Model().views()[0]));
  PRISM_CHECK_EQ(dialog.Model().selected(), -1);
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kDeleteButton));

  // The FILE changed, and the C5 reader (not our loader) sees one entry fewer:
  // the on-disk contract is proved across both implementations.
  const std::vector<std::uint8_t> after = ReadBytes(path);
  PRISM_CHECK(!after.empty());
  PRISM_CHECK(after.size() < before.size());
  PRISM_CHECK(after != before || after.size() != before.size());
  PRISM_CHECK_EQ(VaultEntryCount(path), static_cast<std::size_t>(2));

  // The sidecar was rewritten too: two metadata rows, and the survivor's label
  // is still the one the fixture wrote.
  const std::vector<std::uint8_t> index = ReadBytes(ui::IndexPathFor(path));
  const std::string indexText(index.begin(), index.end());
  PRISM_CHECK(Contains(indexText, "Label 1"));
  PRISM_CHECK(!Contains(indexText, "Label 2"));
  PRISM_CHECK(Contains(indexText, "Label 3"));
}

// ===========================================================================
// 3. Clear: two clicks
// ===========================================================================

PRISM_TEST("win: clear needs the second click, then the vault file is empty") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_clear");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;
  const std::vector<std::uint8_t> before = ReadBytes(path);
  PRISM_CHECK(!before.empty());

  ClickButton(dialog, ui::ids::kClearButton);
  // FIRST click: armed, nothing destroyed -- screen, file and C5's reader agree.
  PRISM_CHECK(dialog.Model().ClearPending());
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)), 3);
  PRISM_CHECK_EQ(ReadBytes(path).size(), before.size());
  PRISM_CHECK_EQ(VaultEntryCount(path), static_cast<std::size_t>(3));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kClearButton), std::string("Confirm clear"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kHint), "Click Clear again"));
  // ... while the model's hint says exactly what the second click will do.
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kHint), "3 credentials"));

  ClickButton(dialog, ui::ids::kClearButton);
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(dialog.Model().state(), VaultAdminState::Empty);
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)), 0);
  PRISM_CHECK_EQ(VaultEntryCount(path), static_cast<std::size_t>(0));
  PRISM_CHECK_EQ(ReadBytes(path).size(), static_cast<std::size_t>(0));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kStatus), "No stored credentials"));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kClearButton));
}

// ===========================================================================
// 4. Corruption: open, explain, refuse to destroy
// ===========================================================================

PRISM_TEST("win: a vault with a bogus length prefix opens and says unreadable") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_corrupt");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  // A zero length prefix is the signature of a damaged file (DpapiVault breaks
  // out of its loop on the same condition).
  PRISM_CHECK(WriteBytes(path, std::vector<std::uint8_t>{0, 0, 0, 0}));

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;  // the point: it OPENS, it does not crash

  PRISM_CHECK_EQ(dialog.Model().state(), VaultAdminState::Unreadable);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kStatus), "unreadable"));
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kStatus), "truncated entry"));
  // Every destructive action is disarmed: rewriting an unparsable vault would
  // destroy data the admin never saw.
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kDeleteButton));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kClearButton));
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kRenameButton));
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)), 0);
  // The file is untouched by merely looking at it.
  const std::vector<std::uint8_t> bytes = ReadBytes(path);
  PRISM_CHECK_EQ(bytes.size(), static_cast<std::size_t>(4));
}

PRISM_TEST("win: a truncated vault reports the offset and keeps the readable prefix") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_truncated");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());
  const std::vector<std::uint8_t> full = ReadBytes(path);
  PRISM_CHECK(full.size() > 8);
  // Keep the first blob intact and replace the rest with a length prefix that
  // points past the end: exactly the damage a half-copied file shows. The first
  // entry MUST survive, the file MUST be reported unreadable.
  const std::uint32_t firstLen = static_cast<std::uint32_t>(full[0]) |
                                 (static_cast<std::uint32_t>(full[1]) << 8) |
                                 (static_cast<std::uint32_t>(full[2]) << 16) |
                                 (static_cast<std::uint32_t>(full[3]) << 24);
  PRISM_CHECK(firstLen > 0);
  PRISM_CHECK(4u + firstLen < full.size());
  const std::size_t keep = 4u + static_cast<std::size_t>(firstLen);
  std::vector<std::uint8_t> truncated(full.begin(),
                                      full.begin() + static_cast<std::ptrdiff_t>(keep));
  truncated.push_back(0xFF);
  truncated.push_back(0xFF);
  truncated.push_back(0x00);
  truncated.push_back(0x00);
  PRISM_CHECK(WriteBytes(path, truncated));

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;
  PRISM_CHECK_EQ(dialog.Model().state(), VaultAdminState::Unreadable);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kStatus), "unreadable"));
  // Recovered rows (at least one) are still shown, and the row text is masked.
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)),
                 static_cast<int>(dialog.Model().size()));
  for (const std::string& secret : secrets) {
    PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), secret));
  }
  // No button may rewrite what could not be parsed.
  PRISM_CHECK(!dialog.ControlEnabled(ui::ids::kClearButton));
}

// ===========================================================================
// 5. One undecryptable blob: skip it, count it, keep it
// ===========================================================================

PRISM_TEST("win: a blob DPAPI rejects is skipped, counted and preserved") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_orphan");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = {"firstRealSecret", "secondRealSecret"};
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  // Splice a bogus blob in FRONT of the real ones: valid layout, undecryptable
  // payload. The magic 5 bytes let the preservation check find it again.
  const std::vector<std::uint8_t> bogus = {0xDE, 0xAD, 0xBE, 0xEF, 0x42};
  std::vector<std::uint8_t> raw = {5, 0, 0, 0};
  raw.insert(raw.end(), bogus.begin(), bogus.end());
  const std::vector<std::uint8_t> real = ReadBytes(path);
  raw.insert(raw.end(), real.begin(), real.end());
  PRISM_CHECK(WriteBytes(path, raw));

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;
  // Two readable entries, one skipped -- the bad blob hid nothing.
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(dialog.Model().skippedCount(), static_cast<std::size_t>(1));
  PRISM_CHECK_EQ(dialog.Model().state(), VaultAdminState::Ready);
  PRISM_CHECK(Contains(dialog.ControlTextUtf8(ui::ids::kHint), "1 entry was skipped"));
  PRISM_CHECK_EQ(ListCount(dialog.Control(ui::ids::kList)), 2);
  PRISM_CHECK(!LeaksInWindow(dialog.Hwnd(), "firstRealSecret"));

  // Deleting a readable entry must NOT delete the blob we could not read.
  SelectRow(dialog, 0);
  ClickButton(dialog, ui::ids::kDeleteButton);
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(1));
  const std::vector<std::uint8_t> after = ReadBytes(path);
  PRISM_CHECK(ContainsBytes(after, bogus));
  PRISM_CHECK_EQ(VaultEntryCount(path), static_cast<std::size_t>(1));
}

// ===========================================================================
// 6. Rename persists through the sidecar
// ===========================================================================

PRISM_TEST("win: rename persists and the next dialog shows the new label") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_rename");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;
  SelectRow(dialog, 0);
  PRISM_CHECK(dialog.ControlEnabled(ui::ids::kRenameButton));
  PRISM_CHECK_EQ(dialog.ControlTextUtf8(ui::ids::kRenameEdit), std::string("Label 1"));
  SetWindowTextW(dialog.Control(ui::ids::kRenameEdit), L"  Home NAS  ");
  ClickButton(dialog, ui::ids::kRenameButton);
  PRISM_CHECK_EQ(dialog.Model().views()[0].label, std::string("Home NAS"));
  PRISM_CHECK(Contains(ui::Utf8FromWide(ListItem(dialog.Control(ui::ids::kList), 0)),
                       "Home NAS"));

  // The sidecar is the persistence: a SECOND dialog (no shared state) sees it.
  const std::vector<std::uint8_t> index = ReadBytes(ui::IndexPathFor(path));
  const std::string indexText(index.begin(), index.end());
  PRISM_CHECK(Contains(indexText, "Home NAS"));
  ui::VaultDialog reopened;
  if (!OpenDialog(reopened, path)) return;
  PRISM_CHECK_EQ(reopened.Model().views()[0].label, std::string("Home NAS"));
  PRISM_CHECK_EQ(reopened.Model().size(), static_cast<std::size_t>(3));

  // A blank rename is refused and keeps the old label (and the file).
  SetWindowTextW(reopened.Control(ui::ids::kRenameEdit), L"   ");
  SelectRow(reopened, 0);
  SetWindowTextW(reopened.Control(ui::ids::kRenameEdit), L"   ");
  PRISM_CHECK(!reopened.RenameSelected(L"   "));
  PRISM_CHECK_EQ(reopened.Model().views()[0].label, std::string("Home NAS"));
  PRISM_CHECK(Contains(reopened.ControlTextUtf8(ui::ids::kHint), "cannot be empty"));
}

// ===========================================================================
// 7. Keyboard
// ===========================================================================

PRISM_TEST("win: every interactive control is tab-reachable and the keys work") {
  prism::testtmp::UniqueDir dir(L"c56", L"vault_keys");
  if (!dir.ok()) return;
  const std::wstring path = dir.path() / L"vault.dat";
  const std::vector<std::string> secrets = SampleSecrets();
  AuthorVault(path, secrets);
  WriteIndex(path, secrets.size());

  ui::VaultDialog dialog;
  if (!OpenDialog(dialog, path)) return;

  // The tab order names every control a user can act on...
  const std::vector<int> order = dialog.TabOrder();
  PRISM_CHECK_EQ(order.size(), static_cast<std::size_t>(6));
  const int expected[] = {ui::ids::kList,        ui::ids::kRenameEdit,
                          ui::ids::kRenameButton, ui::ids::kDeleteButton,
                          ui::ids::kClearButton,  ui::ids::kClose};
  for (std::size_t i = 0; i < order.size(); ++i) {
    PRISM_CHECK_EQ(order[i], expected[i]);
    // ...and each of them really carries WS_TABSTOP, which is what makes Tab
    // work in the modal loop (IsDialogMessageW) and in the shell's own handler.
    const LONG style = GetWindowLongW(dialog.Control(order[i]), GWL_STYLE);
    PRISM_CHECK((style & WS_TABSTOP) != 0);
  }

  PRISM_CHECK_EQ(dialog.FocusIndex(), 0);
  PRISM_CHECK_EQ(dialog.FocusControlId(), ui::ids::kList);
  for (int step = 1; step <= 6; ++step) {
    dialog.Dispatch(WM_KEYDOWN, VK_TAB, 0);
    PRISM_CHECK_EQ(dialog.FocusIndex(), step % 6);
  }
  // Shift+Tab walks backwards (from index 0 that is the last control). The
  // shift state is passed explicitly here so a physically held Shift key on the
  // build agent cannot turn this into a no-op.
  PRISM_CHECK_EQ(dialog.FocusIndex(), 0);
  PRISM_CHECK(dialog.HandleShortcutKey(VK_TAB, true));
  PRISM_CHECK_EQ(dialog.FocusIndex(), 5);
  PRISM_CHECK_EQ(dialog.FocusControlId(), ui::ids::kClose);

  // Delete key on a selected row deletes it (the same handler the modal loop
  // routes child-control keys into).
  SelectRow(dialog, 2);
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(3));
  dialog.Dispatch(WM_KEYDOWN, VK_DELETE, 0);
  PRISM_CHECK_EQ(dialog.Model().size(), static_cast<std::size_t>(2));
  PRISM_CHECK_EQ(VaultEntryCount(path), static_cast<std::size_t>(2));

  // Enter in the rename field commits the rename. The field owns the keyboard
  // focus first (a hidden window still accepts SetFocus in this thread), which
  // is the state the real modal loop guarantees.
  SelectRow(dialog, 0);
  SetWindowTextW(dialog.Control(ui::ids::kRenameEdit), L"Keyboard label");
  PRISM_CHECK(SetFocus(dialog.Control(ui::ids::kRenameEdit)) != nullptr);
  dialog.Dispatch(WM_KEYDOWN, VK_RETURN, 0);
  PRISM_CHECK_EQ(dialog.Model().views()[0].label, std::string("Keyboard label"));
  // The persisted row agrees: the keystroke went through the real write path.
  ui::VaultDialog reopened;
  if (OpenDialog(reopened, path)) {
    PRISM_CHECK_EQ(reopened.Model().views()[0].label, std::string("Keyboard label"));
  }

  // Escape closes the dialog (WM_CLOSE -> DestroyWindow), and the object is
  // left in a state the caller can safely destroy again.
  dialog.Dispatch(WM_KEYDOWN, VK_ESCAPE, 0);
  PRISM_CHECK(dialog.Hwnd() == nullptr);
}

#endif  // _WIN32

int main(int argc, char** argv) {
#if defined(_WIN32) &&                                         \
    (defined(PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT) ||          \
     defined(PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM))
  // CONTRACTS D3 + D26 + D27: CAUGHT is a CONJUNCTION -- the run went red AND
  // the injected defect really fired. Armed BEFORE the first assertion so the
  // dialog teardown cannot swallow the verdict.
  prism::test::SetMutationVerdict("C56 VAULT-UI MUTANT CAUGHT",
                                 [] { return prism::vaultadmin::VaultAdminMutationFired(); });
#endif
  const int failures = prism::test::RunAll(argc, argv);

#if defined(_WIN32)
#if defined(PRISM_VAULT_UI_MUTANT_SHOW_PLAINTEXT) || \
    defined(PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM)
  // Reaching here means no failed check went through ReportFailure, so no
  // verdict was printed. Stay RED (exit 1): a run-then-print verdict is exactly
  // what CONTRACTS D27 forbids.
  std::printf("mutant build: no first-failure verdict (checks=%d failures=%d mutation hits=%d)\n",
              prism::test::CheckCount(), failures,
              prism::vaultadmin::VaultAdminMutationCount());
  return 1;
#else
  if (!gSkipReason.empty()) {
    // CONTRACTS D1: a skip is PRINTED and it is never exit 0.
    std::printf("SKIP: %s\n", gSkipReason.c_str());
    return 77;  // SKIP_RETURN_CODE 77 (Tests/suites/c56-vaultui.cmake)
  }
  if (failures != 0) return 1;
  std::puts("ALL VAULT-DIALOG CHECKS PASSED");
  return 0;
#endif
#else
  return failures;
#endif
}