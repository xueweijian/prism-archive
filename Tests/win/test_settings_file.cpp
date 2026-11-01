// PrismArchive -- Windows settings-file suite (card C14).
//
// The PrismCore suite (Tests/test_settings_store.cpp) owns the schema rules.
// THIS suite owns the system calls, on a real NTFS temp directory:
//   * the location is %LOCALAPPDATA%\PrismArchive\settings.json
//   * the write is atomic: a concurrent reader must never parse a torn document
//     (this is the case the non-atomic mutant exists to fail)
//   * a corrupt document is backed up BYTE FOR BYTE before defaults replace it
//   * a first run creates the folder and the defaults
//   * a document from a newer build is never rewritten
//
// Fixtures live under %TEMP%: the user's real settings folder is never touched,
// and every sandbox is removed again. The harness reads/writes its fixtures with
// its own Win32 calls on purpose -- if the test shared the code under test, one
// bug in that code could make both sides agree on the wrong bytes (D4).
//
// Nothing here can SKIP: an unresolvable %LOCALAPPDATA% is a hard failure, so
// the success marker below is pinned as part of the verdict (CONTRACTS D1).
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "SettingsFile.hpp"

namespace {

namespace sf = prism::win::settingsfile;
using prism::settings::JsonParseError;
using prism::settings::JsonValue;
using prism::settings::LoadStatus;
using prism::settings::SettingsStore;

// ----- independent fixture I/O (deliberately not the code under test) ------

bool ReadRaw(const std::wstring& path, std::string& out) {
  out.clear();
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  char buffer[16 * 1024];
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(handle, buffer, static_cast<DWORD>(sizeof buffer), &read, nullptr)) {
      CloseHandle(handle);
      return false;
    }
    if (read == 0) break;
    out.append(buffer, static_cast<std::size_t>(read));
  }
  CloseHandle(handle);
  return true;
}

bool WriteRaw(const std::wstring& path, std::string_view bytes) {
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  bool ok = true;
  if (!bytes.empty()) {
    DWORD written = 0;
    ok = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) !=
             FALSE &&
         written == bytes.size();
  }
  CloseHandle(handle);
  return ok;
}

void RemoveTree(const std::wstring& path) {
  WIN32_FIND_DATAW entry = {};
  HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      const std::wstring name = entry.cFileName;
      if (name == L"." || name == L"..") continue;
      const std::wstring child = path + L"\\" + name;
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(child);
      } else {
        DeleteFileW(child.c_str());
      }
    } while (FindNextFileW(find, &entry) != FALSE);
    FindClose(find);
  }
  RemoveDirectoryW(path.c_str());
}

int CountFilesWithPrefix(const std::wstring& directory, const std::wstring& prefix) {
  int count = 0;
  WIN32_FIND_DATAW entry = {};
  HANDLE find = FindFirstFileW((directory + L"\\" + prefix + L"*").c_str(), &entry);
  if (find == INVALID_HANDLE_VALUE) return 0;
  do {
    if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) ++count;
  } while (FindNextFileW(find, &entry) != FALSE);
  FindClose(find);
  return count;
}

// A private directory per case, removed when the case ends.
class Sandbox {
 public:
  explicit Sandbox(const wchar_t* tag) {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    std::wstring root(temp, length > 0 ? length : 0);
    while (!root.empty() && root.back() == L'\\') root.pop_back();
    root_ = root + L"\\prism_c14_" + std::wstring(tag) + L"_" +
            std::to_wstring(GetCurrentProcessId());
    RemoveTree(root_);
    if (!CreateDirectoryW(root_.c_str(), nullptr)) root_.clear();
    paths_ = sf::PathsUnder(root_);
    // Fixtures are written before any save in some cases, so the app folder has
    // to exist independently of the code under test.
    if (!paths_.valid() || !CreateDirectoryW(paths_.root.c_str(), nullptr)) root_.clear();
  }
  ~Sandbox() {
    if (!root_.empty()) RemoveTree(root_);
  }
  Sandbox(const Sandbox&) = delete;
  Sandbox& operator=(const Sandbox&) = delete;

  const std::wstring& root() const { return root_; }
  const sf::Paths& paths() const { return paths_; }

 private:
  std::wstring root_;
  sf::Paths paths_;
};

std::wstring ReadEnv(const wchar_t* name) {
  wchar_t buffer[32768] = {};
  const DWORD length = GetEnvironmentVariableW(name, buffer, 32768);
  if (length == 0 || length >= 32768) return {};
  return std::wstring(buffer, length);
}

void SetEnv(const wchar_t* name, const std::wstring& value) {
  SetEnvironmentVariableW(name, value.empty() ? nullptr : value.c_str());
}

bool CheckStoreStatus(LoadStatus got, LoadStatus want, const char* file, int line) {
  if (got == want) return true;
  prism::test::ReportFailure(
      file, line, std::string("store load status: expected ") +
                      prism::settings::LoadStatusName(want) + ", got " +
                      prism::settings::LoadStatusName(got));
  return false;
}

#define C14_CHECK_STORE_STATUS(got, want) CheckStoreStatus((got), (want), __FILE__, __LINE__)

bool ParsesToCompleteDocument(const std::string& bytes, std::string& detail) {
  JsonValue document;
  JsonParseError error;
  if (!prism::settings::ParseJson(bytes, document, &error)) {
    detail = "torn document (" + std::to_string(bytes.size()) + " bytes): " + error.message;
    return false;
  }
  const JsonValue* version = document.Find("schemaVersion");
  if (version == nullptr || version->type != prism::settings::JsonType::Number ||
      version->number != static_cast<double>(SettingsStore::kSchemaVersion)) {
    detail = "document without the current schemaVersion";
    return false;
  }
  return true;
}

}  // namespace

PRISM_TEST("location: the settings file lives in %LOCALAPPDATA%/PrismArchive") {
  Sandbox sandbox(L"location");
  PRISM_CHECK(!sandbox.root().empty());

  const std::wstring previous = ReadEnv(L"LOCALAPPDATA");
  SetEnv(L"LOCALAPPDATA", sandbox.root());
  const sf::Paths paths = sf::DefaultPaths();
  SetEnv(L"LOCALAPPDATA", previous);

  PRISM_CHECK(paths.valid());
  PRISM_CHECK_EQ(sf::WideToUtf8(paths.root), sf::WideToUtf8(sandbox.root() + L"\\PrismArchive"));
  PRISM_CHECK_EQ(sf::WideToUtf8(paths.file),
                 sf::WideToUtf8(sandbox.root() + L"\\PrismArchive\\settings.json"));
  // The fallback path (SHGetKnownFolderPath) must not have been used: the
  // resolved file has to live under the folder we just pointed at.
  PRISM_CHECK(paths.file.rfind(sandbox.root() + L"\\", 0) == 0);
}

PRISM_TEST("atomic save: complete payload, ReplaceFile on rewrite, no temp leftovers") {
  Sandbox sandbox(L"atomic");
  SettingsStore store;
  PRISM_CHECK(store.Load(R"json({"schemaVersion": 3, "appearance": {"theme": "dark"}})json")
                  .readable);
  const std::string expected = store.ToJson();

  // First save: the target does not exist, so a single rename is the whole job.
  sf::SaveResult first;
  PRISM_CHECK(sf::SaveStore(store, sandbox.paths(), &first));
  PRISM_CHECK(first.mechanism == sf::SaveMechanism::MoveFile);
  std::string disk;
  PRISM_CHECK(ReadRaw(sandbox.paths().file, disk));
  PRISM_CHECK_EQ(disk, expected);

  // Second save: the target exists, so the contract is temp file + ReplaceFileW.
  PRISM_CHECK(store.SetString("appearance.theme", "light"));
  sf::SaveResult second;
  PRISM_CHECK(sf::SaveStore(store, sandbox.paths(), &second));
  if (second.mechanism != sf::SaveMechanism::ReplaceFile || second.fellBack) {
    std::cout << "    save mechanism=" << sf::SaveMechanismName(second.mechanism)
              << " fellBack=" << (second.fellBack ? "yes" : "no")
              << " error=" << second.error << std::endl;
  }
  PRISM_CHECK(second.mechanism == sf::SaveMechanism::ReplaceFile);
  PRISM_CHECK(!second.fellBack);
  PRISM_CHECK(ReadRaw(sandbox.paths().file, disk));
  PRISM_CHECK_EQ(disk, store.ToJson());
  PRISM_CHECK(disk.find("\"light\"") != std::string::npos);

  // The temp sibling was renamed into place, not abandoned next to the target.
  PRISM_CHECK_EQ(CountFilesWithPrefix(sandbox.paths().root, L"settings.json.tmp."), 0);

  // And what is on disk reads back as the same document.
  SettingsStore reloaded;
  const sf::LoadReport report = sf::LoadStore(reloaded, sandbox.paths());
  PRISM_CHECK(report.outcome == sf::LoadOutcome::Loaded);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::Ok);
  PRISM_CHECK_EQ(reloaded.GetString("appearance.theme"), std::string("light"));
}

PRISM_TEST("atomic save: a concurrent reader never sees a torn document") {
  Sandbox sandbox(L"race");
  const sf::Paths paths = sandbox.paths();

  // A 16 KiB unknown key: this build preserves it, and it makes the payload big
  // enough that a non-atomic writer has a wide, observable window. 8 bytes of
  // JSON would fit in one sector write and hide the defect.
  const std::string bulk(16 * 1024, 'x');
  std::string seed = "{\"schemaVersion\": 3, \"futureBlob\": \"";
  seed += bulk;
  seed += "\", \"paths\": {\"lastExtractDir\": \"C:/seed\"}}";
  {
    SettingsStore writer;
    PRISM_CHECK(writer.Load(seed).readable);
    sf::SaveResult saved;
    PRISM_CHECK(sf::SaveStore(writer, paths, &saved));
  }

  constexpr int kWriterThreads = 3;
  constexpr int kIterations = 30;
  std::atomic<bool> start{false};
  std::atomic<bool> stop{false};
  std::atomic<int> completeReads{0};
  std::atomic<int> tornReads{0};
  std::atomic<int> readFailures{0};
  std::atomic<int> saveFailures{0};
  std::mutex problemMutex;
  std::string firstProblem;

  auto record = [&](const std::string& problem) {
    std::lock_guard<std::mutex> lock(problemMutex);
    if (firstProblem.empty()) firstProblem = problem;
  };

  std::vector<std::thread> writers;
  writers.reserve(kWriterThreads);
  for (int w = 0; w < kWriterThreads; ++w) {
    writers.emplace_back([&, w]() {
      SettingsStore writer;
      if (!writer.Load(seed).readable) {
        saveFailures.fetch_add(1);
        record("writer could not load the seed document");
        return;
      }
      while (!start.load()) std::this_thread::yield();
      for (int i = 0; i < kIterations; ++i) {
        const std::string marker = "writer-" + std::to_string(w) + "-" + std::to_string(i);
        if (!writer.SetString("paths.lastExtractDir", marker)) {
          saveFailures.fetch_add(1);
          record("SetString refused");
          return;
        }
        sf::SaveResult saved;
        if (!sf::SaveStore(writer, paths, &saved)) {
          saveFailures.fetch_add(1);
          record("save failed: mechanism=" + std::string(sf::SaveMechanismName(saved.mechanism)) +
                 " error=" + std::to_string(saved.error));
          return;
        }
      }
    });
  }

  std::thread reader([&]() {
    while (!start.load()) std::this_thread::yield();
    while (!stop.load()) {
      std::string bytes;
      DWORD readError = 0;
      bool opened = false;
      // ReplaceFileW holds the target briefly; one of those instants can deny an
      // open. A sharing violation is a retry, not a verdict -- a torn document
      // is the verdict, and that is what the parse below judges.
      // ReplaceFileW moves the previous document to the backup name before it
      // moves the replacement in, so the path itself can be absent for a few
      // microseconds during a swap. An absent path is transient; a TORN
      // document is the defect this case exists to catch.
      for (int attempt = 0; attempt < 200; ++attempt) {
        if (sf::ReadAllBytes(paths.file, bytes, &readError)) {
          opened = true;
          break;
        }
        if (readError != ERROR_SHARING_VIOLATION && readError != ERROR_ACCESS_DENIED &&
            readError != ERROR_LOCK_VIOLATION && readError != ERROR_FILE_NOT_FOUND) {
          break;
        }
        // A yield is far shorter than the swap window; sleep a scheduler tick.
        Sleep(1);
      }
      if (!opened) {
        readFailures.fetch_add(1);
        record("reader could not open the settings file: " + std::to_string(readError));
        continue;
      }
      std::string detail;
      if (!ParsesToCompleteDocument(bytes, detail)) {
        tornReads.fetch_add(1);
        record(detail);
        continue;
      }
      completeReads.fetch_add(1);
    }
  });

  start.store(true);
  for (std::thread& writer : writers) writer.join();
  stop.store(true);
  reader.join();

  if (!firstProblem.empty()) std::cout << "    FIRST PROBLEM: " << firstProblem << std::endl;
  PRISM_CHECK_EQ(saveFailures.load(), 0);
  PRISM_CHECK_EQ(readFailures.load(), 0);
  PRISM_CHECK_EQ(tornReads.load(), 0);
  // The observer must actually have observed: a reader that never ran would
  // "pass" this case without proving anything (CONTRACTS D2/D18).
  PRISM_CHECK(completeReads.load() > 100);

  // The last document on disk is a complete one, and no temp sibling survived.
  std::string finalBytes;
  PRISM_CHECK(ReadRaw(paths.file, finalBytes));
  std::string detail;
  PRISM_CHECK(ParsesToCompleteDocument(finalBytes, detail));
  PRISM_CHECK_EQ(CountFilesWithPrefix(paths.root, L"settings.json.tmp."), 0);
}

PRISM_TEST("recovery: a corrupt document is backed up byte for byte, then defaulted") {
  Sandbox sandbox(L"corrupt");
  std::string garbage = "{\"schemaVersion\": 3, \"appearance\": {\"theme\": \"dark\" ";
  garbage += std::string(4096, ' ');
  PRISM_CHECK(WriteRaw(sandbox.paths().file, garbage));
  // The backup must be byte-identical, so the file has to be big enough that a
  // truncated copy would be detectable (D2).
  PRISM_CHECK(ReadRaw(sandbox.paths().file, garbage));

  SettingsStore store;
  const sf::LoadReport report = sf::LoadStore(store, sandbox.paths());
  PRISM_CHECK(report.outcome == sf::LoadOutcome::RecoveredFromCorrupt);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::Corrupt);
  PRISM_CHECK(!report.backupPath.empty());
  PRISM_CHECK(report.backupPath.rfind(sandbox.paths().file, 0) == 0);
  PRISM_CHECK(report.backupPath.find(L".corrupt-") != std::wstring::npos);

  std::string backupBytes;
  PRISM_CHECK(ReadRaw(report.backupPath, backupBytes));
  PRISM_CHECK_EQ(backupBytes.size(), garbage.size());
  PRISM_CHECK(backupBytes == garbage);

  // The live path is a valid defaults document again, and it is what is on disk.
  std::string live;
  PRISM_CHECK(ReadRaw(sandbox.paths().file, live));
  PRISM_CHECK_EQ(live, store.ToJson());
  C14_CHECK_STORE_STATUS(store.report().status, LoadStatus::Defaults);
  SettingsStore reloaded;
  const sf::LoadReport second = sf::LoadStore(reloaded, sandbox.paths());
  PRISM_CHECK(second.outcome == sf::LoadOutcome::Loaded);
  C14_CHECK_STORE_STATUS(second.storeStatus, LoadStatus::Ok);
  PRISM_CHECK_EQ(reloaded.GetString("appearance.theme"), std::string("system"));
}

PRISM_TEST("recovery: an empty (zero-byte) file is corrupt, not an empty document") {
  Sandbox sandbox(L"empty");
  PRISM_CHECK(WriteRaw(sandbox.paths().file, std::string()));
  DWORD attributes = GetFileAttributesW(sandbox.paths().file.c_str());
  PRISM_CHECK(attributes != INVALID_FILE_ATTRIBUTES);

  SettingsStore store;
  const sf::LoadReport report = sf::LoadStore(store, sandbox.paths());
  PRISM_CHECK(report.outcome == sf::LoadOutcome::RecoveredFromCorrupt);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::Corrupt);
  std::string backupBytes;
  PRISM_CHECK(ReadRaw(report.backupPath, backupBytes));
  PRISM_CHECK(backupBytes.empty());
  PRISM_CHECK_EQ(CountFilesWithPrefix(sandbox.paths().root, L"settings.json.corrupt-"), 1);
}

PRISM_TEST("first run: a missing file produces defaults on disk") {
  Sandbox sandbox(L"fresh");
  // A nested root that does not exist yet proves EnsureDirectory too.
  const sf::Paths nested = sf::PathsUnder(sandbox.root() + L"\\one\\two\\three");
  PRISM_CHECK(nested.valid());

  SettingsStore store;
  const sf::LoadReport report = sf::LoadStore(store, nested);
  PRISM_CHECK(report.outcome == sf::LoadOutcome::FreshDefaults);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::Defaults);
  C14_CHECK_STORE_STATUS(store.report().status, LoadStatus::Defaults);

  std::string disk;
  PRISM_CHECK(ReadRaw(nested.file, disk));
  PRISM_CHECK_EQ(disk, store.ToJson());
  PRISM_CHECK(disk.find("\"schemaVersion\": 3") != std::string::npos);

  // The second start reads that document back instead of writing it again.
  SettingsStore reloaded;
  const sf::LoadReport second = sf::LoadStore(reloaded, nested);
  PRISM_CHECK(second.outcome == sf::LoadOutcome::Loaded);
  C14_CHECK_STORE_STATUS(second.storeStatus, LoadStatus::Ok);
}

PRISM_TEST("unknown keys survive a file round trip; known edits are persisted") {
  Sandbox sandbox(L"unknown");
  const std::string document = R"json({
  "schemaVersion": 3,
  "appearance": {"theme": "dark", "futureTint": "#101010"},
  "extract": {"closeWhenDone": false},
  "future.telemetry": {"enabled": true}
})json";
  PRISM_CHECK(WriteRaw(sandbox.paths().file, document));

  SettingsStore store;
  const sf::LoadReport report = sf::LoadStore(store, sandbox.paths());
  PRISM_CHECK(report.outcome == sf::LoadOutcome::Loaded);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::FilledDefaults);

  PRISM_CHECK(store.SetBool("extract.closeWhenDone", true));
  PRISM_CHECK(store.SetString("language.tag", "zh-CN"));
  sf::SaveResult saved;
  PRISM_CHECK(sf::SaveStore(store, sandbox.paths(), &saved));

  std::string disk;
  PRISM_CHECK(ReadRaw(sandbox.paths().file, disk));
  PRISM_CHECK(disk.find("\"futureTint\"") != std::string::npos);
  PRISM_CHECK(disk.find("\"future.telemetry\"") != std::string::npos);
  PRISM_CHECK(disk.find("\"closeWhenDone\": true") != std::string::npos);

  SettingsStore reloaded;
  PRISM_CHECK(sf::LoadStore(reloaded, sandbox.paths()).outcome == sf::LoadOutcome::Loaded);
  PRISM_CHECK_EQ(reloaded.GetString("appearance.futureTint"), std::string("#101010"));
  const JsonValue* telemetry = reloaded.mutableRoot().Find("future.telemetry");
  PRISM_CHECK(telemetry != nullptr);
  PRISM_CHECK(telemetry != nullptr && telemetry->Find("enabled") != nullptr &&
              telemetry->Find("enabled")->boolean);
  PRISM_CHECK(reloaded.GetBool("extract.closeWhenDone", false));
  PRISM_CHECK_EQ(reloaded.GetString("language.tag"), std::string("zh-CN"));
}

PRISM_TEST("too new: a newer document is read, and never rewritten") {
  Sandbox sandbox(L"future");
  const std::string document = R"json({
  "schemaVersion": 99,
  "appearance": {"theme": "dark", "futureTint": "#101010"},
  "window": {"width": 999999}
})json";
  PRISM_CHECK(WriteRaw(sandbox.paths().file, document));

  SettingsStore store;
  const sf::LoadReport report = sf::LoadStore(store, sandbox.paths());
  PRISM_CHECK(report.outcome == sf::LoadOutcome::Loaded);
  C14_CHECK_STORE_STATUS(report.storeStatus, LoadStatus::TooNew);
  PRISM_CHECK(!store.writable());

  // A save is refused with a write-protect error, and the file is untouched.
  sf::SaveResult saved;
  PRISM_CHECK(!sf::SaveStore(store, sandbox.paths(), &saved));
  PRISM_CHECK_EQ(saved.error, static_cast<DWORD>(ERROR_WRITE_PROTECT));
  std::string disk;
  PRISM_CHECK(ReadRaw(sandbox.paths().file, disk));
  PRISM_CHECK(disk == document);
  // No backup and no temp sibling: nothing tried to "fix" the future.
  PRISM_CHECK_EQ(CountFilesWithPrefix(sandbox.paths().root, L"settings.json.corrupt-"), 0);
  PRISM_CHECK_EQ(CountFilesWithPrefix(sandbox.paths().root, L"settings.json.tmp."), 0);
}

int main(int argc, char** argv) {
  std::cout << "=== settings file layer (schema v" << SettingsStore::kSchemaVersion << ")"
            << std::endl;
#if defined(PRISM_C14_MUTANT_NONATOMIC_SAVE)
  std::cout << "C14 MUTANT ACTIVE: NONATOMIC_SAVE -- the concurrent-reader case MUST fail"
            << std::endl;
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc == 0) std::cout << "ALL SETTINGS-FILE CHECKS PASSED" << std::endl;
  return rc;
}
