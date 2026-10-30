// PrismArchive -- PrismWin
// Smart Password Ring on Windows: DPAPI (per-user, TPM-backed where available)
// encrypted vault + Mark-of-the-Web propagation.
#pragma once

#include <windows.h>
#include <dpapi.h>

#include <string>
#include <vector>

#include "../PrismCore/PasswordVault.hpp"

#pragma comment(lib, "crypt32.lib")

namespace prism::win {

// Storage backend of IPasswordVault. Secrets never touch the disk in plaintext:
// CryptProtectData binds every blob to the current logon session.
class DpapiVault final : public IPasswordVault {
 public:
  explicit DpapiVault(std::wstring storePath) : storePath_(std::move(storePath)) { Load(); }

  std::vector<std::string> List() const override { return entries_; }

  bool Add(const std::string& password) override {
    std::wstring wide(password.begin(), password.end());
    const std::vector<uint8_t> blob = Protect(wide);
    if (blob.empty()) return false;
    blobs_.push_back(blob);
    entries_.push_back(password);
    return Save();
  }

  bool Remove(const std::string& password) override {
    for (size_t i = 0; i < entries_.size(); ++i) {
      if (entries_[i] == password) {
        entries_.erase(entries_.begin() + static_cast<long>(i));
        blobs_.erase(blobs_.begin() + static_cast<long>(i));
        return Save();
      }
    }
    return false;
  }

  const char* Backend() const override { return "dpapi"; }

 private:
  static std::vector<uint8_t> Protect(const std::wstring& secret) {
    DATA_BLOB in{};
    in.pbData = reinterpret_cast<BYTE*>(const_cast<wchar_t*>(secret.data()));
    in.cbData = static_cast<DWORD>((secret.size() + 1) * sizeof(wchar_t));
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"PrismArchiveVaultEntry", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
      return {};
    }
    std::vector<uint8_t> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);
    return blob;
  }

  void Load() {
    HANDLE file = CreateFileW(storePath_.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::vector<uint8_t> raw(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, raw.data(), static_cast<DWORD>(raw.size()), &read, nullptr);
    CloseHandle(file);
    // Layout: [uint32 little-endian length][blob]*
    size_t pos = 0;
    while (pos + 4 <= raw.size()) {
      const uint32_t len = static_cast<uint32_t>(raw[pos]) |
                           (static_cast<uint32_t>(raw[pos + 1]) << 8) |
                           (static_cast<uint32_t>(raw[pos + 2]) << 16) |
                           (static_cast<uint32_t>(raw[pos + 3]) << 24);
      pos += 4;
      if (len == 0 || pos + len > raw.size()) break;
      std::vector<uint8_t> blob(raw.begin() + static_cast<long>(pos),
                                raw.begin() + static_cast<long>(pos + len));
      pos += len;
      DATA_BLOB in{};
      in.pbData = blob.data();
      in.cbData = static_cast<DWORD>(blob.size());
      DATA_BLOB out{};
      if (CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                             &out)) {
        std::wstring secret(reinterpret_cast<const wchar_t*>(out.pbData));
        SecureZeroMemory(out.pbData, out.cbData);
        LocalFree(out.pbData);
        entries_.push_back(std::string(secret.begin(), secret.end()));
        blobs_.push_back(std::move(blob));
      }
    }
  }

  bool Save() const {
    std::vector<uint8_t> raw;
    for (const auto& blob : blobs_) {
      const uint32_t len = static_cast<uint32_t>(blob.size());
      raw.push_back(static_cast<uint8_t>(len & 0xFF));
      raw.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
      raw.push_back(static_cast<uint8_t>((len >> 16) & 0xFF));
      raw.push_back(static_cast<uint8_t>((len >> 24) & 0xFF));
      raw.insert(raw.end(), blob.begin(), blob.end());
    }
    HANDLE file = CreateFileW(storePath_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, raw.data(), static_cast<DWORD>(raw.size()), &written, nullptr);
    CloseHandle(file);
    return written == raw.size();
  }

  std::wstring storePath_;
  std::vector<std::string> entries_;
  std::vector<std::vector<uint8_t>> blobs_;
};

// Blueprint ch. 7.1 defence #1: every extracted file must inherit the source
// archive's Zone.Identifier so SmartScreen still sees "downloaded from the web".
class MoTWPropagator {
 public:
  // Reads "<archive>:Zone.Identifier" once and stamps every written file.
  explicit MoTWPropagator(const std::wstring& archivePath) {
    const std::wstring stream = archivePath + L":Zone.Identifier";
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;  // no MoTW on the source: nothing to do
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    blob_.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    ReadFile(file, blob_.data(), static_cast<DWORD>(blob_.size()), &read, nullptr);
    CloseHandle(file);
    blob_.resize(read);
    hasMoTW_ = !blob_.empty();
  }

  bool hasMoTW() const { return hasMoTW_; }

  bool Stamp(const std::wstring& writtenFilePath) const {
    if (!hasMoTW_) return false;
    const std::wstring stream = writtenFilePath + L":Zone.Identifier";
    HANDLE file = CreateFileW(stream.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    WriteFile(file, blob_.data(), static_cast<DWORD>(blob_.size()), &written, nullptr);
    CloseHandle(file);
    return written == blob_.size();
  }

 private:
  std::vector<uint8_t> blob_;
  bool hasMoTW_ = false;
};

}  // namespace prism::win
