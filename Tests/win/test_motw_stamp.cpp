// Windows LIVE MoTW suite (blueprint 7.1 #1): a real NTFS Zone.Identifier
// alternate data stream. No malicious bytes are involved, so this suite runs by
// default in CI.
//
// The policy decisions are unit-tested in Tests/test_amsi_policy.cpp; this file
// proves the system calls do what the policy assumes:
//   * the cloned stream is byte-identical on disk, including for a >MAX_PATH
//     target, and the read-back comparison is not vacuous (D2/D4 calibration);
//   * an absent / empty / locked / garbage source stream is distinguished from
//     "no mark" instead of silently stamping nothing.
//
// Fixtures are deliberately named *.bin: naming a 18-byte fixture "*.zip" made
// Defender's ML heuristics flag the temp file as a Trojan during the first run of
// this card, which is noise this test does not need.
//
// The AMSI half lives in Tests/win/test_amsi_guard.cpp and is opt-in
// (PRISM_RUN_AV_FIXTURES=1) because it feeds a synthetic EICAR sample.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "MotwGuard.hpp"

namespace {

std::wstring Extended(const std::wstring& path) {
  if (path.rfind(L"\\\\?\\", 0) == 0) return path;
  return L"\\\\?\\" + path;
}

std::wstring AdsPath(const std::wstring& file) { return Extended(file) + L":Zone.Identifier"; }

std::wstring MakeTempRoot() {
  wchar_t temp[MAX_PATH] = {};
  const DWORD n = GetTempPathW(MAX_PATH, temp);
  std::wstring root(temp, n > 0 ? n : 0);
  if (!root.empty() && root.back() == L'\\') root.pop_back();
  root += L"\\prism_c5_motw_" + std::to_wstring(GetCurrentProcessId());
  CreateDirectoryW(Extended(root).c_str(), nullptr);
  return root;
}

bool EnsureDirectory(const std::wstring& directory) {
  std::wstring current;
  size_t i = 0;
  if (directory.size() >= 2 && directory[1] == L':') {
    current = directory.substr(0, 2);
    i = 2;
  }
  while (i < directory.size()) {
    while (i < directory.size() && (directory[i] == L'\\' || directory[i] == L'/')) ++i;
    const size_t start = i;
    while (i < directory.size() && directory[i] != L'\\' && directory[i] != L'/') ++i;
    if (start == i) break;
    current += L"\\" + directory.substr(start, i - start);
    CreateDirectoryW(Extended(current).c_str(), nullptr);
  }
  return GetFileAttributesW(Extended(directory).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool WriteFileBytes(const std::wstring& path, const std::string& bytes) {
  HANDLE file = CreateFileW(Extended(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  size_t total = 0;
  while (total < bytes.size()) {
    DWORD written = 0;
    if (!WriteFile(file, bytes.data() + total, static_cast<DWORD>(bytes.size() - total), &written,
                   nullptr) ||
        written == 0) {
      CloseHandle(file);
      return false;
    }
    total += written;
  }
  CloseHandle(file);
  return true;
}

// Deliberately independent of MotwGuard's own reader: if both sides shared one
// buggy reader, the read-back assertion could be consistently wrong (D4).
bool WriteAdsRaw(const std::wstring& file, const std::string& bytes) {
  HANDLE handle = CreateFileW(AdsPath(file).c_str(), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = bytes.empty() ||
                  (WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                             nullptr) &&
                   written == bytes.size());
  CloseHandle(handle);
  return ok;
}

bool ReadAdsRaw(const std::wstring& file, std::string* out, DWORD* error) {
  HANDLE handle = CreateFileW(AdsPath(file).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    if (error) *error = GetLastError();
    return false;
  }
  LARGE_INTEGER size{};
  GetFileSizeEx(handle, &size);
  out->assign(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const bool ok = out->empty() ||
                  (ReadFile(handle, out->data(), static_cast<DWORD>(out->size()), &read, nullptr) &&
                   read == out->size());
  CloseHandle(handle);
  if (!ok && error) *error = GetLastError();
  return ok;
}

bool AdsExists(const std::wstring& file) {
  return GetFileAttributesW(AdsPath(file).c_str()) != INVALID_FILE_ATTRIBUTES;
}

// A real browser-written mark: ZoneId=3 plus the two URLs, CRLF, verbatim.
const std::string kMark =
    "[ZoneTransfer]\r\n"
    "ZoneId=3\r\n"
    "HostUrl=https://example.com/download/Invoice.zip\r\n"
    "ReferrerUrl=https://example.com/inbox\r\n";

}  // namespace

PRISM_TEST("motw live: an archive with no Zone.Identifier stamps nothing") {
  const std::wstring root = MakeTempRoot();
  const std::wstring archive = root + L"\\plain_archive.bin";
  const std::wstring target = root + L"\\plain_target.bin";
  PRISM_CHECK(WriteFileBytes(archive, "PK\x03\x04 sample payload"));
  PRISM_CHECK(WriteFileBytes(target, "MZ target"));
  PRISM_CHECK(!AdsExists(archive));

  prism::win::motw::SourceMark mark(archive);
  PRISM_CHECK_EQ(mark.state(), prism::win::motw::SourceState::Absent);
  PRISM_CHECK(!mark.hasMoTW());
  PRISM_CHECK_EQ(mark.policySource(), prism::MotwSource::Absent);
  PRISM_CHECK(!mark.Stamp(target));
  PRISM_CHECK_EQ(mark.StampDetailed(target).result, prism::win::motw::StampResult::NoSourceMark);
  PRISM_CHECK(!AdsExists(target));  // stamping must not invent a mark

  const prism::MotwPlan plan = prism::PlanMotwInheritance(mark.policySource());
  PRISM_CHECK(!plan.stamp);
  PRISM_CHECK_EQ(plan.action, prism::SecurityAction::Allow);
}

PRISM_TEST("motw live: the source stream is cloned byte-for-byte and read back") {
  const std::wstring root = MakeTempRoot();
  const std::wstring archive = root + L"\\marked_archive.bin";
  const std::wstring target = root + L"\\marked_target.bin";
  const std::wstring second = root + L"\\marked_target2.bin";
  PRISM_CHECK(WriteFileBytes(archive, "PK\x03\x04 marked payload"));
  PRISM_CHECK(WriteFileBytes(target, "MZ target"));
  PRISM_CHECK(WriteFileBytes(second, "MZ target 2"));
  PRISM_CHECK(WriteAdsRaw(archive, kMark));

  prism::win::motw::SourceMark mark(archive);
  PRISM_CHECK_EQ(mark.state(), prism::win::motw::SourceState::Present);
  PRISM_CHECK(mark.hasMoTW());
  PRISM_CHECK_EQ(mark.policySource(), prism::MotwSource::Present);
  PRISM_CHECK_EQ(mark.blob(), kMark);  // the source itself is read verbatim

  // ZoneId / HostUrl / ReferrerUrl survive the read.
  PRISM_CHECK_EQ(mark.fields().zoneId, 3);
  PRISM_CHECK_EQ(mark.fields().hostUrl, std::string("https://example.com/download/Invoice.zip"));
  PRISM_CHECK_EQ(mark.fields().referrerUrl, std::string("https://example.com/inbox"));

  const prism::MotwPlan plan = prism::PlanMotwInheritance(mark.policySource());
  PRISM_CHECK(plan.stamp);

  PRISM_CHECK(mark.Stamp(target));
  std::string readBack;
  DWORD error = 0;
  PRISM_CHECK(ReadAdsRaw(target, &readBack, &error));
  // Content, not existence: the exact bytes SmartScreen will read.
  PRISM_CHECK_EQ(readBack, kMark);
  PRISM_CHECK_EQ(readBack.size(), kMark.size());
  const prism::MotwFields stamped = prism::ParseMotwFields(readBack);
  PRISM_CHECK_EQ(stamped.zoneId, 3);
  PRISM_CHECK_EQ(stamped.referrerUrl, std::string("https://example.com/inbox"));

  // Calibration (D4): the comparison MUST reject a different mark of the same
  // shape. Without this, "readback == expected" could be a tautology.
  const std::string otherMark =
      "[ZoneTransfer]\r\n"
      "ZoneId=4\r\n"
      "HostUrl=https://example.com/download/Invoice.zip\r\n"
      "ReferrerUrl=https://example.com/inbox\r\n";
  PRISM_CHECK(WriteAdsRaw(second, otherMark));
  std::string otherRead;
  PRISM_CHECK(ReadAdsRaw(second, &otherRead, &error));
  PRISM_CHECK(otherRead != kMark);
  PRISM_CHECK_EQ(prism::ParseMotwFields(otherRead).zoneId, 4);

  // The one-shot API (the signature the assembly layer asked for).
  const std::wstring third = root + L"\\marked_target3.bin";
  PRISM_CHECK(WriteFileBytes(third, "MZ target 3"));
  PRISM_CHECK(prism::win::motw::StampFile(archive, third));
  PRISM_CHECK(ReadAdsRaw(third, &readBack, &error));
  PRISM_CHECK_EQ(readBack, kMark);
}

PRISM_TEST("motw live: a >MAX_PATH target is still stamped") {
  const std::wstring root = MakeTempRoot();
  const std::wstring archive = root + L"\\long_archive.bin";
  PRISM_CHECK(WriteFileBytes(archive, "PK\x03\x04 long payload"));
  PRISM_CHECK(WriteAdsRaw(archive, kMark));

  std::wstring deep = root;
  for (int i = 0; i < 18; ++i) deep += L"\\segment_" + std::to_wstring(100 + i) + L"_padding";
  // > 260 characters: a plain CreateFileW call fails here.
  PRISM_CHECK(deep.size() > 300);
  PRISM_CHECK(EnsureDirectory(deep));
  const std::wstring target = deep + L"\\deep_payload.bin";
  PRISM_CHECK(WriteFileBytes(target, "MZ deep"));

  prism::win::motw::SourceMark mark(archive);
  PRISM_CHECK_EQ(mark.state(), prism::win::motw::SourceState::Present);
  PRISM_CHECK(mark.Stamp(target));
  std::string readBack;
  DWORD error = 0;
  PRISM_CHECK(ReadAdsRaw(target, &readBack, &error));
  PRISM_CHECK_EQ(readBack, kMark);
}

PRISM_TEST("motw live: empty / locked / garbage source streams are not 'absent'") {
  const std::wstring root = MakeTempRoot();

  // Empty stream: exists, but carries no mark. Never treated as Present.
  const std::wstring emptyArchive = root + L"\\empty_mark.bin";
  PRISM_CHECK(WriteFileBytes(emptyArchive, "PK"));
  PRISM_CHECK(WriteAdsRaw(emptyArchive, ""));
  prism::win::motw::SourceMark empty(emptyArchive);
  PRISM_CHECK_EQ(empty.state(), prism::win::motw::SourceState::Empty);
  PRISM_CHECK_EQ(empty.policySource(), prism::MotwSource::Unreadable);
  PRISM_CHECK_EQ(prism::PlanMotwInheritance(empty.policySource()).action,
                 prism::SecurityAction::Warn);

  // Sharing lock (an AV scanner or the browser holding the stream): the mark may
  // well exist, we just cannot read it -- warn, do not claim "no mark". Sharing is
  // enforced per stream on NTFS (measured: holding the base file with share mode 0
  // does NOT block an open of its Zone.Identifier stream), so lock the stream.
  const std::wstring lockedArchive = root + L"\\locked_mark.bin";
  PRISM_CHECK(WriteFileBytes(lockedArchive, "PK"));
  PRISM_CHECK(WriteAdsRaw(lockedArchive, kMark));
  HANDLE lock = CreateFileW(AdsPath(lockedArchive).c_str(), GENERIC_READ, 0, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  PRISM_CHECK(lock != INVALID_HANDLE_VALUE);
  if (lock != INVALID_HANDLE_VALUE) {
    prism::win::motw::SourceMark locked(lockedArchive);
    PRISM_CHECK_EQ(locked.state(), prism::win::motw::SourceState::Unreadable);
    PRISM_CHECK(!locked.detail().empty());
    PRISM_CHECK_EQ(prism::PlanMotwInheritance(locked.policySource()).action,
                   prism::SecurityAction::Warn);
    const std::wstring target = root + L"\\locked_target.bin";
    PRISM_CHECK(WriteFileBytes(target, "MZ"));
    PRISM_CHECK(!locked.Stamp(target));
    PRISM_CHECK(!AdsExists(target));
    CloseHandle(lock);
  }

  // Garbage stream: cloning it into every file would be worse than warning.
  const std::wstring garbageArchive = root + L"\\garbage_mark.bin";
  PRISM_CHECK(WriteFileBytes(garbageArchive, "PK"));
  PRISM_CHECK(WriteAdsRaw(garbageArchive, "this is not a zone map"));
  prism::win::motw::SourceMark garbage(garbageArchive);
  PRISM_CHECK_EQ(garbage.state(), prism::win::motw::SourceState::Unreadable);
  PRISM_CHECK_EQ(garbage.policySource(), prism::MotwSource::Unreadable);
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL MOTW-STAMP CHECKS PASSED" << std::endl;
  return 0;
}
