// PrismArchive -- PrismWin
// VaultDialog: the password-vault manager screen (card C56).
//
// THE GAP THIS FILLS
//   C5 shipped the DPAPI store and C35 shipped a dialog that can PICK a stored
//   secret, but nothing let a user see WHAT is stored, delete one entry, rename
//   it, or wipe the vault. This is that screen.
//
// THREE HALVES, ONE CARD
//   * Src/PrismCore/VaultAdminModel.hpp owns every rule (listing, labels, mask,
//     delete, rename, the two-step clear, corruption accounting). Pure, proved
//     on every lane.
//   * The store half below (LoadVaultFile / SaveVaultFile / the index sidecar)
//     is the ONLY place that touches the disk. It reads the SAME on-disk layout
//     C5's DpapiVault writes -- [uint32 LE length][CryptProtectData blob]* -- and
//     rewrites it VERBATIM blob by blob, so metadata edits never re-encrypt and
//     never lose an entry. The window test proves the interop in both
//     directions (C5 writes -> this reads; this deletes -> C5 reads N-1).
//   * The HWND half is programmatic Win32 (the C18/C31 rule: no .rc, no dialog
//     template, every control a real HWND the test can read back). The V3 shell
//     (Src/PrismWin/DialogChrome.hpp) paints the dark card, the title bar and
//     the text; the list/edit/buttons stay real controls so the keyboard and
//     the UI Automation tree actually work.
//
// SECRECY (why the window test enumerates every control)
//   The plaintext exists in exactly two places: the model's private entry list
//   and this class's record blobs (needed to rewrite the file). NO control, no
//   listbox ITEM and no window title ever receives it -- rows are built from
//   VaultEntryView, whose only secret representation is the mask.
//
// CORRUPTION / DECRYPTION FAILURES
//   A blob that CryptUnprotectData rejects is skipped ONE AT A TIME and counted;
//   the rest of the vault still lists. A layout that cannot be parsed (a length
//   prefix that overruns the file, trailing garbage) makes the dialog report
//   "unreadable" and REFUSE to rewrite the file -- an admin action must never
//   destroy data it could not read. The dialog still opens: a crash here would
//   leave the user with no way to see (or clear) a broken vault.
//
// CONTRACTS
//   B1  parenthesized (std::min)/(std::max) only.
//   B2  <cstdint> is included by this header itself.
//   B9  no worker thread touches a control: everything runs on the UI thread
//       and the modal loop never blocks on INFINITE (D29).
//   D1  no silent skip: OpenModal reports failure, the test prints its marker.
//   D7  whole-file rewrite only (temp + ReplaceFile-style move), never a
//       partial edit at a computed offset.
//   D29 every wait in the modal loop is a message wait with a real exit path;
//       the store does no retry loop at all.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/VaultAdminModel.hpp"

#if defined(_WIN32)

// File scope, never inside the namespace: including windows.h inside a
// namespace wraps every SDK declaration into it (the C18 lesson).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dpapi.h>
#include <windowsx.h>  // GET_X_LPARAM / GET_Y_LPARAM

#include <commctrl.h>

#include <d2d1.h>
#include <d2d1helper.h>

#include "DialogChrome.hpp"

#pragma comment(lib, "crypt32.lib")

namespace prism::win::vaultui {

using prism::vaultadmin::VaultAdminModel;
using prism::vaultadmin::VaultAdminState;
using prism::vaultadmin::VaultEntryInput;
using prism::vaultadmin::VaultEntryView;
using prism::vaultadmin::VaultLoadReport;

// Local transcoders (the C35 rule: do not import two identically named inline
// helpers from unrelated PrismWin headers into one TU).
inline std::wstring WideFromUtf8(const std::string& utf8) {
  if (utf8.empty()) return std::wstring();
  const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring wide(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                      needed);
  return wide;
}

inline std::string Utf8FromWide(const std::wstring& wide) {
  if (wide.empty()) return std::string();
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                         static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                         nullptr);
  std::string utf8(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                      needed, nullptr, nullptr);
  return utf8;
}

// Control ids live here (not in the model) so the HWND test names what the
// dialog created.
namespace ids {
constexpr int kList = 3601;
constexpr int kRenameEdit = 3602;
constexpr int kRenameButton = 3603;
constexpr int kDeleteButton = 3604;
constexpr int kClearButton = 3605;
constexpr int kStatus = 3606;
constexpr int kHint = 3607;
constexpr int kClose = 3608;
constexpr int kCaption = 3609;
constexpr int kRenameCaption = 3610;
constexpr int kEntryCount = 3611;
}  // namespace ids

// ---------------------------------------------------------------------------
// Store half: the on-disk vault. Layout is C5's (DpapiVault.cpp/hpp), byte for
// byte, and this code never re-encrypts: records keep the ORIGINAL ciphertext so
// a rename cannot change (or lose) a secret.
// ---------------------------------------------------------------------------
struct VaultRecord {
  std::uint64_t id = 0;
  std::string secret;                       // plaintext, wiped with the dialog
  std::vector<std::uint8_t> blob;           // original CryptProtectData output
};

struct VaultFileReport {
  std::vector<VaultRecord> records;
  // Blobs this machine cannot decrypt. They are PRESERVED (rewritten untouched)
  // because an admin action must not delete what it could not read.
  std::vector<std::vector<std::uint8_t>> orphans;
  bool unreadable = false;
  std::string detail;
  std::size_t skipped() const { return orphans.size(); }
};

inline std::wstring IndexPathFor(const std::wstring& vaultPath) { return vaultPath + L".index"; }

inline bool ReadWholeFile(const std::wstring& path, std::vector<std::uint8_t>* out,
                          DWORD* error, bool* missing) {
  out->clear();
  if (error != nullptr) *error = 0;
  if (missing != nullptr) *missing = false;
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    const DWORD code = GetLastError();
    if (missing != nullptr) {
      *missing = code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
    }
    if (error != nullptr) *error = code;
    return false;
  }
  LARGE_INTEGER size{};
  if (GetFileSizeEx(file, &size) == FALSE || size.QuadPart < 0 ||
      static_cast<std::uint64_t>(size.QuadPart) > (64ull * 1024ull * 1024ull)) {
    if (error != nullptr) *error = GetLastError();
    CloseHandle(file);
    return false;
  }
  out->resize(static_cast<std::size_t>(size.QuadPart));
  bool ok = true;
  if (!out->empty()) {
    DWORD read = 0;
    ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) != FALSE &&
         static_cast<std::size_t>(read) == out->size();
    if (!ok && error != nullptr) *error = GetLastError();
  }
  CloseHandle(file);
  return ok;
}

// Whole-file rewrite: temp file in the SAME directory, then one replace. A
// half-written vault is the one failure mode a password store cannot survive.
inline bool WriteWholeFile(const std::wstring& path, const void* data, std::size_t size,
                           DWORD* error) {
  if (error != nullptr) *error = 0;
  const std::wstring temp = path + L".tmp";
  const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    if (error != nullptr) *error = GetLastError();
    return false;
  }
  bool ok = true;
  if (size > 0) {
    DWORD written = 0;
    ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) != FALSE &&
         static_cast<std::size_t>(written) == size;
    if (!ok && error != nullptr) *error = GetLastError();
  }
  if (ok) ok = FlushFileBuffers(file) != FALSE;
  CloseHandle(file);
  if (!ok) {
    DeleteFileW(temp.c_str());
    return false;
  }
  if (MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
    if (error != nullptr) *error = GetLastError();
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

// CryptProtectData output -> the narrow string the rest of the app uses. The
// conversion matches DpapiVault::Load exactly (byte-per-wchar), which is why a
// vault written by C5 reads identically here. A blob that is not a terminated
// wide string is NOT trusted (it is skipped, not read past its end).
inline bool DecodeProtectedBlob(const std::vector<std::uint8_t>& blob, std::string* secret) {
  DATA_BLOB in{};
  in.pbData = const_cast<BYTE*>(blob.data());
  in.cbData = static_cast<DWORD>(blob.size());
  DATA_BLOB out{};
  if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                         &out) == FALSE) {
    return false;
  }
  bool ok = false;
  if (out.pbData != nullptr && out.cbData >= sizeof(wchar_t) &&
      (out.cbData % sizeof(wchar_t)) == 0) {
    const std::size_t count = static_cast<std::size_t>(out.cbData) / sizeof(wchar_t);
    const wchar_t* wide = reinterpret_cast<const wchar_t*>(out.pbData);
    if (wide[count - 1] == L'\0') {
      std::wstring text(wide, count - 1);
      *secret = std::string(text.begin(), text.end());
      ok = true;
      volatile wchar_t* wipe = const_cast<volatile wchar_t*>(wide);
      for (std::size_t i = 0; i < count; ++i) wipe[i] = 0;
    }
  }
  if (out.pbData != nullptr) {
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
  }
  return ok;
}

inline VaultFileReport LoadVaultFile(const std::wstring& path) {
  VaultFileReport report;
  std::vector<std::uint8_t> raw;
  DWORD error = 0;
  bool missing = false;
  if (!ReadWholeFile(path, &raw, &error, &missing)) {
    if (!missing) {
      report.unreadable = true;
      report.detail = "the vault file could not be read (error " + std::to_string(error) + ")";
    }
    return report;
  }
  std::size_t pos = 0;
  std::uint64_t nextId = 1;
  while (pos + 4 <= raw.size()) {
    const std::uint32_t len = static_cast<std::uint32_t>(raw[pos]) |
                              (static_cast<std::uint32_t>(raw[pos + 1]) << 8) |
                              (static_cast<std::uint32_t>(raw[pos + 2]) << 16) |
                              (static_cast<std::uint32_t>(raw[pos + 3]) << 24);
    pos += 4;
    if (len == 0 || static_cast<std::size_t>(len) > prism::vaultadmin::kMaxEntryBytes ||
        pos + len > raw.size()) {
      // A length prefix that overruns the file is the signature of a truncated
      // or damaged vault. Stop here; the readable prefix is still shown.
      report.unreadable = true;
      report.detail = "truncated entry at byte " + std::to_string(pos - 4);
      return report;
    }
    std::vector<std::uint8_t> blob(raw.begin() + static_cast<std::ptrdiff_t>(pos),
                                   raw.begin() + static_cast<std::ptrdiff_t>(pos + len));
    pos += len;
    VaultRecord record;
    if (DecodeProtectedBlob(blob, &record.secret)) {
      record.id = nextId++;
      record.blob = std::move(blob);
      report.records.push_back(std::move(record));
    } else {
      report.orphans.push_back(std::move(blob));
    }
  }
  if (pos != raw.size() && !report.unreadable) {
    report.unreadable = true;
    report.detail = "trailing bytes at offset " + std::to_string(pos);
  }
  return report;
}

inline bool SaveVaultFile(const std::wstring& path, const std::vector<VaultRecord>& records,
                          const std::vector<std::vector<std::uint8_t>>& orphans,
                          DWORD* error = nullptr) {
  std::vector<std::uint8_t> raw;
  auto append = [&raw](const std::vector<std::uint8_t>& blob) {
    const std::uint32_t len = static_cast<std::uint32_t>(blob.size());
    raw.push_back(static_cast<std::uint8_t>(len & 0xFFu));
    raw.push_back(static_cast<std::uint8_t>((len >> 8) & 0xFFu));
    raw.push_back(static_cast<std::uint8_t>((len >> 16) & 0xFFu));
    raw.push_back(static_cast<std::uint8_t>((len >> 24) & 0xFFu));
    raw.insert(raw.end(), blob.begin(), blob.end());
  };
  for (const VaultRecord& record : records) append(record.blob);
  for (const std::vector<std::uint8_t>& blob : orphans) append(blob);
  return WriteWholeFile(path, raw.data(), raw.size(), error);
}

// The secret-free sidecar that carries labels / timestamps / hit counts.
inline std::size_t LoadVaultIndex(const std::wstring& path,
                                  std::vector<VaultEntryInput>* entries) {
  entries->clear();
  std::vector<std::uint8_t> raw;
  DWORD error = 0;
  bool missing = false;
  if (!ReadWholeFile(path, &raw, &error, &missing)) return missing ? 0 : 1;
  const std::string text(raw.begin(), raw.end());
  prism::vaultadmin::VaultIndexParse parsed = prism::vaultadmin::ParseVaultIndex(text);
  *entries = std::move(parsed.entries);
  std::size_t skipped = parsed.skipped;
  if (!text.empty() && !parsed.headerPresent) skipped += 1;
  return skipped;
}

inline bool SaveVaultIndex(const std::wstring& path,
                           const std::vector<VaultEntryInput>& entries, DWORD* error = nullptr) {
  const std::string text = prism::vaultadmin::SerializeVaultIndex(entries);
  return WriteWholeFile(path, text.data(), text.size(), error);
}

// ---------------------------------------------------------------------------
// The dialog
// ---------------------------------------------------------------------------
class VaultDialog {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveVaultDialog";
  static constexpr int kClientWidth = 680;
  static constexpr int kClientHeight = 448;

  VaultDialog() = default;
  ~VaultDialog() { Destroy(); }
  VaultDialog(const VaultDialog&) = delete;
  VaultDialog& operator=(const VaultDialog&) = delete;

  DWORD lastError() const { return lastError_; }
  int missingControlId() const { return missingControlId_; }
  const std::string& loadError() const { return loadError_; }

  // Loads the vault + sidecar and then creates the window. A corrupt vault still
  // opens (state Unreadable); only a window-station failure returns false.
  bool Create(HINSTANCE instance, std::wstring vaultPath,
              std::wstring title = L"Password vault") {
    if (instance == nullptr) instance = GetModuleHandleW(nullptr);
    Destroy();  // a second Create on one object must not keep the first window
    instance_ = instance;
    vaultPath_ = std::move(vaultPath);
    title_ = std::move(title);
    Reload();  // fills model_ / records_ / orphans_ before WM_CREATE

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &VaultDialog::ProcThunk;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW, wide
    wc.lpszClassName = kClassName;
    if (RegisterClassExW(&wc) == 0) {
      const DWORD code = GetLastError();
      if (code != ERROR_CLASS_ALREADY_EXISTS) {
        lastError_ = code;
        return false;
      }
    }
    dpi_ = chrome::CurrentDpi(nullptr);
    const float scale = static_cast<float>(dpi_) / 96.0f;
    const int width = static_cast<int>(std::lround(kClientWidth * scale));
    const int height = static_cast<int>(std::lround(kClientHeight * scale));
    hwnd_ = CreateWindowExW(WS_EX_DLGMODALFRAME, kClassName, title_.c_str(),
                            WS_POPUP | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, width,
                            height, nullptr, nullptr, instance_, this);
    if (hwnd_ == nullptr) {
      lastError_ = GetLastError();
      return false;
    }
    return true;
  }

  void Destroy() {
    if (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
    WipeRecords();
    orphans_.clear();
  }

  HWND Hwnd() const { return hwnd_; }
  HWND Control(int id) const { return hwnd_ != nullptr ? GetDlgItem(hwnd_, id) : nullptr; }

  const std::wstring& vaultPath() const { return vaultPath_; }
  std::wstring IndexPath() const { return IndexPathFor(vaultPath_); }

  VaultAdminModel& Model() { return model_; }
  const VaultAdminModel& Model() const { return model_; }
  const std::vector<VaultRecord>& records() const { return records_; }

  // Re-reads both files. Safe with or without a window.
  bool Reload() {
    VaultFileReport report = LoadVaultFile(vaultPath_);
    // Count BEFORE the vectors are moved out: skipped() is orphans.size(), and
    // a moved-from vector is empty -- reading it afterwards silently reported
    // "0 undecryptable entries" for a vault that had one.
    const std::size_t storeSkipped = report.skipped();
    WipeRecords();
    records_ = std::move(report.records);
    orphans_ = std::move(report.orphans);

    std::vector<VaultEntryInput> indexEntries;
    std::size_t indexSkipped = LoadVaultIndex(IndexPath(), &indexEntries);

    // Entries in the index but not in the vault file are stale metadata: they
    // are skipped and COUNTED rather than silently reappearing as empty rows.
    std::size_t extra = indexEntries.size() > records_.size()
                            ? indexEntries.size() - records_.size()
                            : 0;

    VaultLoadReport modelReport;
    modelReport.skipped = storeSkipped + indexSkipped + extra;
    modelReport.unreadable = report.unreadable;
    modelReport.detail = report.detail;

    std::vector<VaultEntryInput> inputs;
    inputs.reserve(records_.size());
    for (std::size_t i = 0; i < records_.size(); ++i) {
      VaultEntryInput input;
      input.id = records_[i].id;
      input.secret = records_[i].secret;
      if (i < indexEntries.size()) {
        input.label = indexEntries[i].label;
        input.addedAt = indexEntries[i].addedAt;
        input.lastUsedAt = indexEntries[i].lastUsedAt;
        input.hitCount = indexEntries[i].hitCount;
      }
      inputs.push_back(std::move(input));
    }
    model_.Load(std::move(inputs), modelReport);
    loadError_ = report.detail;
    if (hwnd_ != nullptr) {
      Populate();
      SyncControls();
    }
    return !report.unreadable;
  }

  // ---- operations the handlers and the tests share ---------------------
  bool DeleteSelected() {
    if (model_.state() == VaultAdminState::Unreadable) {
      return false;  // never rewrite a file we could not parse
    }
    const int selected = model_.selected();
    if (selected < 0) return false;
    const std::uint64_t id = model_.views()[static_cast<std::size_t>(selected)].id;
    if (!model_.RemoveById(id)) return false;
    for (auto it = records_.begin(); it != records_.end(); ++it) {
      if (it->id != id) continue;
      // Wipe before the vector drops the plaintext.
      volatile char* data = it->secret.empty() ? nullptr : &it->secret[0];
      for (std::size_t i = 0; data != nullptr && i < it->secret.size(); ++i) data[i] = 0;
      records_.erase(it);
      break;
    }
    model_.ClearSelection();
    if (!PersistAll()) {
      Reload();
      return false;
    }
    Populate();
    SyncControls();
    return true;
  }

  // Clear is TWO-STEP. The first click only arms it (model_.RequestClear), the
  // second one erases. PRISM_VAULT_UI_MUTANT_NO_CLEAR_CONFIRM removes the arming
  // step entirely, which is exactly the defect the suite must catch.
  void RequestClear() {
    model_.RequestClear();
    if (!model_.ClearPending()) {
      SyncControls();
      return;
    }
    SyncControls();
  }

  bool ConfirmClear() {
    if (!model_.ConfirmClear()) {
      SyncControls();
      return false;
    }
    WipeRecords();
    records_.clear();
    orphans_.clear();  // a confirmed wipe is a wipe: no blob survives it
    if (!PersistAll()) {
      Reload();
      return false;
    }
    Populate();
    SyncControls();
    return true;
  }

  void CancelClear() {
    model_.CancelClear();
    SyncControls();
  }

  bool RenameSelected(const std::wstring& label) {
    if (model_.state() == VaultAdminState::Unreadable) return false;
    if (!model_.RenameSelected(Utf8FromWide(label))) {
      SyncControls();
      return false;
    }
    if (!PersistAll()) {
      Reload();
      return false;
    }
    Populate();
    SyncControls();
    return true;
  }

  bool RenameAt(std::size_t index, const std::wstring& label) {
    if (model_.state() == VaultAdminState::Unreadable) return false;
    if (!model_.RenameByIndex(index, Utf8FromWide(label))) {
      SyncControls();
      return false;
    }
    if (!PersistAll()) {
      Reload();
      return false;
    }
    Populate();
    SyncControls();
    return true;
  }

  // ---- test surface ----------------------------------------------------
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return HandleMessage(hwnd_, message, wparam, lparam);
  }

  std::wstring ControlText(int id) const { return ControlTextOf(Control(id)); }
  std::string ControlTextUtf8(int id) const { return Utf8FromWide(ControlText(id)); }

  bool ControlEnabled(int id) const {
    const HWND control = Control(id);
    return control != nullptr && IsWindowEnabled(control) != FALSE;
  }

  // WS_VISIBLE is the honest "would be visible" question while the window is
  // created hidden by the test (the C35 rule).
  bool ControlVisible(int id) const {
    const HWND control = Control(id);
    return control != nullptr &&
           (GetWindowLongW(control, GWL_STYLE) & static_cast<LONG>(WS_VISIBLE)) != 0;
  }

  const std::vector<int>& TabOrder() const { return tabOrder_; }
  int FocusIndex() const { return focusIndex_; }
  int FocusControlId() const {
    if (tabOrder_.empty() || focusIndex_ < 0 ||
        static_cast<std::size_t>(focusIndex_) >= tabOrder_.size()) {
      return -1;
    }
    return tabOrder_[static_cast<std::size_t>(focusIndex_)];
  }

  // The one keyboard entry point. WndProc calls it for WM_KEYDOWN and the modal
  // loop calls it for keys that land on a CHILD (a listbox never forwards Tab or
  // Delete to its parent) -- one implementation, two message paths.
  bool HandleShortcutKey(UINT vk, bool shift = false) {
    if (hwnd_ == nullptr) return false;
    switch (vk) {
      case VK_TAB: {
        const int count = static_cast<int>(tabOrder_.size());
        if (count <= 0) return false;
        focusIndex_ = (focusIndex_ + (shift ? count - 1 : 1)) % count;
        const HWND target = Control(tabOrder_[static_cast<std::size_t>(focusIndex_)]);
        if (target != nullptr) SetFocus(target);
        return true;
      }
      case VK_DELETE:
        return !model_.empty() && DeleteSelected();
      case VK_ESCAPE:
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return true;
      case VK_RETURN: {
        const HWND focused = GetFocus();
        if (focused == nullptr || focused == renameEdit_) {
          RenameSelected(ControlText(ids::kRenameEdit));
          return true;
        }
        if (focused == list_) return false;
        SendMessageW(focused, BM_CLICK, 0, 0);
        return true;
      }
      default: return false;
    }
  }

  // Modal driver: the caller is one line. Bounded by the window's lifetime and
  // the app's WM_QUIT; no INFINITE wait of its own (CONTRACTS D29).
  bool OpenModal(HINSTANCE instance, HWND owner, std::wstring vaultPath,
                 std::wstring title = L"Password vault") {
    if (!Create(instance, std::move(vaultPath), std::move(title))) return false;
    owner_ = owner;
    if (owner_ != nullptr) EnableWindow(owner_, FALSE);
    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);
    MSG msg{};
    while (hwnd_ != nullptr && IsWindow(hwnd_) != FALSE) {
      const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
      if (got <= 0) break;  // WM_QUIT: the app is going away, do not eat it
      // Keys that land on a child control (the listbox swallows Tab/Delete)
      // are routed through the SAME handler the tests drive.
      if (msg.message == WM_KEYDOWN &&
          HandleShortcutKey(static_cast<UINT>(msg.wParam),
                            (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
        continue;
      }
      if (IsDialogMessageW(hwnd_, &msg) == FALSE) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if (owner_ != nullptr && IsWindow(owner_) != FALSE) EnableWindow(owner_, TRUE);
    return true;
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<VaultDialog*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam != 0) {
      self = reinterpret_cast<VaultDialog*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
      // CreateChildren runs inside CreateWindowExW, before the caller can assign
      // the returned handle; without this line the children get a null parent.
      self->hwnd_ = hwnd;
    }
    if (self == nullptr) return DefWindowProcW(hwnd, message, wparam, lparam);
    return self->HandleMessage(hwnd, message, wparam, lparam);
  }

  static std::wstring ControlTextOf(HWND control) {
    if (control == nullptr) return std::wstring();
    const int length = GetWindowTextLengthW(control);
    std::wstring buffer(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, buffer.data(), length + 1);
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
  }

  static void SetTextIfDifferent(HWND control, const std::wstring& text) {
    if (control == nullptr) return;
    if (ControlTextOf(control) == text) return;
    SetWindowTextW(control, text.c_str());
  }

  HWND MakeControl(const wchar_t* klass, const wchar_t* text, DWORD style, int id, int x,
                   int y, int width, int height, bool visible) {
    const DWORD childStyle = WS_CHILD | style | (visible ? WS_VISIBLE : 0);
    const HWND control = CreateWindowExW(
        0, klass, text, childStyle, x, y, width, height, hwnd_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    if (control == nullptr) {
      missingControlId_ = id;
      lastError_ = GetLastError();
      return nullptr;
    }
    SendMessageW(control, WM_SETFONT,
                 reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return control;
  }

  int S(int value) const {
    const float scale = static_cast<float>(dpi_) / 96.0f;
    return static_cast<int>(std::lround(static_cast<float>(value) * scale));
  }

  bool CreateChildren() {
    darkBrush_ = CreateSolidBrush(RGB(18, 20, 27));

    // Captions and the two hint lines are painted by DialogChrome; the hidden
    // STATIC twins exist so the test can read back exactly what is displayed
    // (the C31 "invisible adapter" pattern).
    MakeControl(L"STATIC", L"Stored credentials", SS_LEFT, ids::kCaption, S(16), S(40),
                S(300), S(20), false);
    MakeControl(L"STATIC", L"Rename label", SS_LEFT, ids::kRenameCaption, S(16), S(316),
                S(120), S(20), false);
    statusText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kStatus, S(16), S(344),
                              S(kClientWidth - 32), S(22), false);
    hintText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kHint, S(16), S(368),
                            S(kClientWidth - 32), S(40), false);
    entryCountText_ = MakeControl(L"STATIC", L"", SS_LEFT, ids::kEntryCount, S(220), S(40),
                                  S(200), S(20), false);

    list_ = MakeControl(L"LISTBOX", L"",
                        LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_BORDER |
                            WS_TABSTOP,
                        ids::kList, S(16), S(64), S(kClientWidth - 32), S(240), true);
    renameEdit_ = MakeControl(L"EDIT", L"", ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP,
                              ids::kRenameEdit, S(132), S(312), S(330), S(24), true);
    renameButton_ = MakeControl(L"BUTTON", L"Rename", BS_PUSHBUTTON | WS_TABSTOP,
                                ids::kRenameButton, S(474), S(311), S(80), S(26), true);
    deleteButton_ = MakeControl(L"BUTTON", L"Delete selected", BS_PUSHBUTTON | WS_TABSTOP,
                                ids::kDeleteButton, S(16), S(412), S(140), S(28), true);
    clearButton_ = MakeControl(L"BUTTON", L"Clear vault...", BS_PUSHBUTTON | WS_TABSTOP,
                               ids::kClearButton, S(166), S(412), S(160), S(28), true);
    closeButton_ = MakeControl(L"BUTTON", L"Close", BS_DEFPUSHBUTTON | WS_TABSTOP,
                               ids::kClose, S(kClientWidth - 104), S(412), S(88), S(28),
                               true);

    if (list_ == nullptr || renameEdit_ == nullptr || renameButton_ == nullptr ||
        deleteButton_ == nullptr || clearButton_ == nullptr || closeButton_ == nullptr ||
        statusText_ == nullptr || hintText_ == nullptr || entryCountText_ == nullptr) {
      return false;
    }

    tabOrder_ = {ids::kList,      ids::kRenameEdit, ids::kRenameButton,
                 ids::kDeleteButton, ids::kClearButton, ids::kClose};
    focusIndex_ = 0;

    Populate();
    SyncControls();
    return true;
  }

  void Populate() {
    if (list_ == nullptr) return;
    syncing_ = true;
    SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    for (const VaultEntryView& view : model_.views()) {
      const std::wstring row = WideFromUtf8(prism::vaultadmin::FormatEntryRow(view));
      SendMessageW(list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
    }
    if (model_.selected() >= 0) {
      SendMessageW(list_, LB_SETCURSEL,
                   static_cast<WPARAM>(model_.selected()), 0);
    } else {
      SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    }
    const VaultEntryView* selected = model_.viewAt(
        model_.selected() < 0 ? model_.size() : static_cast<std::size_t>(model_.selected()));
    SetTextIfDifferent(renameEdit_, selected != nullptr ? WideFromUtf8(selected->label)
                                                        : std::wstring());
    syncing_ = false;
  }

  void SyncControls() {
    if (hwnd_ == nullptr) return;
    const bool unreadable = model_.state() == VaultAdminState::Unreadable;
    const bool hasSelection = model_.selected() >= 0;
    const bool canMutate = !unreadable;
    EnableWindow(deleteButton_, (canMutate && hasSelection) ? TRUE : FALSE);
    EnableWindow(clearButton_, (canMutate && !model_.empty()) ? TRUE : FALSE);
    EnableWindow(renameButton_, (canMutate && hasSelection) ? TRUE : FALSE);
    EnableWindow(renameEdit_, (canMutate && hasSelection) ? TRUE : FALSE);
    EnableWindow(list_, TRUE);
    SetTextIfDifferent(clearButton_, model_.ClearPending() ? L"Confirm clear" : L"Clear vault...");
    SetTextIfDifferent(statusText_, WideFromUtf8(model_.StatusLine()));
    SetTextIfDifferent(hintText_, WideFromUtf8(model_.HintLine()));
    SetTextIfDifferent(entryCountText_,
                       WideFromUtf8("(" + std::to_string(model_.size()) + ")"));
    if (hwnd_ != nullptr) InvalidateRect(hwnd_, nullptr, FALSE);
  }

  // Writes the vault body (blobs verbatim, minus the deleted ones) and then the
  // sidecar. On any failure the caller reloads from disk so the screen never
  // shows a state that was not persisted.
  bool PersistAll() {
    if (model_.state() == VaultAdminState::Unreadable) return false;
    DWORD error = 0;
    if (!SaveVaultFile(vaultPath_, records_, orphans_, &error)) {
      loadError_ = "the vault file could not be written (error " + std::to_string(error) + ")";
      return false;
    }
    std::vector<VaultEntryInput> snapshot = model_.MetadataSnapshot();
    std::vector<VaultEntryInput> ordered;
    ordered.reserve(records_.size());
    for (const VaultRecord& record : records_) {
      for (const VaultEntryInput& meta : snapshot) {
        if (meta.id != record.id) continue;
        VaultEntryInput copy = meta;
        copy.id = record.id;
        ordered.push_back(std::move(copy));
        break;
      }
    }
    if (!SaveVaultIndex(IndexPath(), ordered, &error)) {
      loadError_ = "the vault index could not be written (error " + std::to_string(error) + ")";
      return false;
    }
    model_.ClearDirty();
    return true;
  }

  void OnCommand(int id, int code, HWND control) {
    if (control == nullptr || syncing_) return;
    switch (id) {
      case ids::kList:
        if (code != LBN_SELCHANGE) return;
        model_.Select(static_cast<int>(SendMessageW(list_, LB_GETCURSEL, 0, 0)));
        Populate();
        SyncControls();
        return;
      case ids::kDeleteButton:
        if (code != BN_CLICKED) return;
        DeleteSelected();
        return;
      case ids::kClearButton:
        if (code != BN_CLICKED) return;
        if (model_.ClearPending()) {
          ConfirmClear();
        } else {
          RequestClear();
        }
        return;
      case ids::kRenameButton:
        if (code != BN_CLICKED) return;
        RenameSelected(ControlText(ids::kRenameEdit));
        return;
      case ids::kClose:
        if (code != BN_CLICKED) return;
        SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        return;
      default:
        return;
    }
  }

  // ---- V3 shell --------------------------------------------------------
  chrome::ChromeModel BuildModel() const {
    using chrome::WidgetKind;
    chrome::ChromeModel model;
    model.variant = prism::theme::Variant::Midnight;
    model.dpi = dpi_;
    model.width = static_cast<float>(kClientWidth);
    model.height = static_cast<float>(kClientHeight);
    model.title = title_;

    auto label = [&model](const wchar_t* text, float x, float y, float w, float h) {
      chrome::Widget widget;
      widget.kind = WidgetKind::Label;
      widget.rect = D2D1::RectF(x, y, x + w, y + h);
      widget.label = text;
      model.widgets.push_back(std::move(widget));
    };
    auto hint = [&model](const std::wstring& text, float x, float y, float w, float h,
                         bool error) {
      chrome::Widget widget;
      widget.kind = WidgetKind::Hint;
      widget.rect = D2D1::RectF(x, y, x + w, y + h);
      widget.label = text;
      widget.error = error;
      model.widgets.push_back(std::move(widget));
    };

    label(L"Stored credentials", 16.0f, 40.0f, 200.0f, 20.0f);
    label(L"Rename label", 16.0f, 316.0f, 110.0f, 20.0f);
    hint(WideFromUtf8(model_.StatusLine()), 16.0f, 344.0f,
         static_cast<float>(kClientWidth) - 32.0f, 20.0f, false);
    hint(WideFromUtf8(model_.HintLine()), 16.0f, 368.0f,
         static_cast<float>(kClientWidth) - 32.0f, 36.0f,
         !model_.lastError().empty() || model_.ClearPending());
    return model;
  }

  void PaintChrome() {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(hwnd_, &paint);
    bool painted = false;
    if (SUCCEEDED(chromePainter_.Initialize())) {
      if (!chromeTarget_) {
        RECT client{};
        GetClientRect(hwnd_, &client);
        const D2D1_SIZE_U size = D2D1::SizeU(
            static_cast<UINT32>(client.right - client.left),
            static_cast<UINT32>(client.bottom - client.top));
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f,
            96.0f);
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProperties =
            D2D1::HwndRenderTargetProperties(hwnd_, size, D2D1_PRESENT_OPTIONS_NONE);
        if (SUCCEEDED(chromePainter_.Factory()->CreateHwndRenderTarget(
                properties, hwndProperties, chromeTarget_.ReleaseAndGetAddressOf()))) {
          chromeTarget_->SetDpi(96.0f, 96.0f);
        }
      }
      if (chromeTarget_) {
        chromeTarget_->BeginDraw();
        chromePainter_.Draw(chromeTarget_.Get(), BuildModel());
        if (SUCCEEDED(chromeTarget_->EndDraw())) painted = true;
      }
    }
    if (!painted) {
      RECT client{};
      GetClientRect(hwnd_, &client);
      const HBRUSH brush = CreateSolidBrush(RGB(11, 12, 16));
      FillRect(dc, &client, brush);
      DeleteObject(brush);
    }
    EndPaint(hwnd_, &paint);
  }

  LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_CREATE:
        return CreateChildren() ? 0 : -1;
      case WM_COMMAND:
        OnCommand(LOWORD(wparam), HIWORD(wparam), reinterpret_cast<HWND>(lparam));
        return 0;
      case WM_KEYDOWN:
        if (HandleShortcutKey(static_cast<UINT>(wparam),
                              (GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
          return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
      case WM_PAINT:
        PaintChrome();
        return 0;
      case WM_ERASEBKGND:
        return 1;
      case WM_CTLCOLORLISTBOX:
      case WM_CTLCOLOREDIT:
      case WM_CTLCOLORSTATIC: {
        const HDC target = reinterpret_cast<HDC>(wparam);
        SetTextColor(target, RGB(232, 236, 244));
        SetBkColor(target, RGB(18, 20, 27));
        return reinterpret_cast<LRESULT>(darkBrush_);
      }
      case WM_MOUSEMOVE:
        return 0;
      case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const chrome::ChromeModel model = BuildModel();
        if (chrome::HitCloseScreen(model, static_cast<float>(x), static_cast<float>(y))) {
          SendMessageW(hwnd_, WM_CLOSE, 0, 0);
          return 0;
        }
        if (chrome::HitTitleScreen(model, static_cast<float>(x), static_cast<float>(y))) {
          SendMessageW(hwnd_, WM_NCLBUTTONDOWN, HTCAPTION, 0);
          return 0;
        }
        return 0;
      }
      case WM_DPICHANGED: {
        dpi_ = HIWORD(wparam);
        const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
          SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                       suggested->right - suggested->left, suggested->bottom - suggested->top,
                       SWP_NOZORDER | SWP_NOACTIVATE);
        }
        chromeTarget_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
      }
      case WM_SIZE:
        if (chromeTarget_) {
          chromeTarget_->Resize(D2D1::SizeU(static_cast<UINT32>(LOWORD(lparam)),
                                            static_cast<UINT32>(HIWORD(lparam))));
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
      case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
      case WM_DESTROY:
        chromeTarget_.Reset();
        chromePainter_.ResetDeviceResources();
        if (darkBrush_ != nullptr) {
          DeleteObject(darkBrush_);
          darkBrush_ = nullptr;
        }
        hwnd_ = nullptr;
        return 0;
      default:
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
  }

  void WipeRecords() {
    for (VaultRecord& record : records_) {
      volatile char* data = record.secret.empty() ? nullptr : &record.secret[0];
      for (std::size_t i = 0; data != nullptr && i < record.secret.size(); ++i) data[i] = 0;
      record.secret.clear();
    }
  }

  HWND hwnd_ = nullptr;
  HINSTANCE instance_ = nullptr;
  DWORD lastError_ = 0;
  int missingControlId_ = 0;
  int dpi_ = 96;
  bool syncing_ = false;
  std::wstring vaultPath_;
  std::wstring title_ = L"Password vault";
  std::string loadError_;

  VaultAdminModel model_;
  std::vector<VaultRecord> records_;
  std::vector<std::vector<std::uint8_t>> orphans_;

  HWND list_ = nullptr;
  HWND renameEdit_ = nullptr;
  HWND renameButton_ = nullptr;
  HWND deleteButton_ = nullptr;
  HWND clearButton_ = nullptr;
  HWND closeButton_ = nullptr;
  HWND statusText_ = nullptr;
  HWND hintText_ = nullptr;
  HWND entryCountText_ = nullptr;
  HBRUSH darkBrush_ = nullptr;

  std::vector<int> tabOrder_;
  int focusIndex_ = 0;
  HWND owner_ = nullptr;

  chrome::ChromePainter chromePainter_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> chromeTarget_;
};

}  // namespace prism::win::vaultui

#endif  // _WIN32
