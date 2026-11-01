// PrismArchive -- Windows integration test for the small-file write backends
// (blueprint ch. 7.2: IORING + directory-tree pre-creation + long paths).
//
// What is proven here, on a real NTFS temp tree:
//
//   1. LongPath: the extended prefix contract -- absolute paths, UNC, relative
//      paths with '.'/'..' (which MUST be resolved before the prefix, because
//      the prefix disables normalisation), reserved DOS device names, display
//      round trip, and a REAL >260-character directory tree that is written,
//      read back byte-for-byte and deleted.
//   2. The three-method backend interface is injectable: a FakeBackend records
//      the exact CreateFile/Write/Close sequence, including Close-on-failure
//      and the pipeline window used by the IoRing batch.
//   3. All three real backends (Sequential / Precreate / IoRing) write the same
//      60-file tree with byte-identical payloads; the IoRing backend reports
//      "degraded: <reason>" instead of pretending when the API is missing.
//   4. The large-page probe is honest: it reports the exact missing link
//      (7z.dll export / SeLockMemoryPrivilege / MEM_LARGE_PAGES allocation) and
//      never claims "enabled" unless all three held.
//
// Negative controls (CONTRACTS D3 + D26): two mutant builds of this same file.
//   * PRISM_WRITE_MUTANT_NO_PREDIR -- PrecreateBackend skips the tree.
//   * PRISM_LONGPATH_MUTANT_STRIP_PREFIX -- LongPath::Normalize drops '\?\'.
// Each mutant prints "<...> MUTANT CAUGHT (injections=N)" and exits 0 ONLY when
// the guard's assertion went red AND the injection counter proves the mutant
// branch ran. The ctest registration pins that marker with
// PASS_REGULAR_EXPRESSION and deliberately does NOT use WILL_FAIL (D26: the two
// together invert a pass into an always-red test).
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <iterator>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "../mini_test.hpp"
#include "LongPath.hpp"
#include "PrismEngineBridge.hpp"
#include "WriteBackend.hpp"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "ole32.lib")

namespace lp = prism::win::longpath;
namespace wb = prism::win::writeback;

namespace {

// mini_test streams values into a narrow ostringstream; wide strings must be
// narrowed before they can be compared with PRISM_CHECK_EQ.
std::string Narrow(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) {
    out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  }
  return out;
}

bool Same(const std::wstring& a, const std::wstring& b) { return a == b; }

std::wstring TempRoot(const wchar_t* tag) {
  static int sequence = 0;
  wchar_t temp[MAX_PATH] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, temp);
  std::wstring base(length > 0 ? std::wstring(temp, length) : std::wstring(L".\\"));
  ++sequence;
  std::wstring root = base + L"prism_c32_" + tag + L"_" + std::to_wstring(::GetCurrentProcessId()) +
                      L"_" + std::to_wstring(sequence);
  ::CreateDirectoryW(lp::Normalize(root).c_str(), nullptr);
  return root;
}

void RemoveTree(const std::wstring& directory) {
  const std::wstring normalized = lp::Normalize(directory);
  WIN32_FIND_DATAW entry{};
  HANDLE find = ::FindFirstFileW((normalized + L"\\*").c_str(), &entry);
  if (find != INVALID_HANDLE_VALUE) {
    do {
      if (::wcscmp(entry.cFileName, L".") == 0 || ::wcscmp(entry.cFileName, L"..") == 0) continue;
      const std::wstring child = directory + L"\\" + entry.cFileName;
      if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        RemoveTree(child);
      } else {
        ::DeleteFileW(lp::Normalize(child).c_str());
      }
    } while (::FindNextFileW(find, &entry));
    ::FindClose(find);
  }
  ::RemoveDirectoryW(normalized.c_str());
}

bool WriteBytesRaw(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE file = ::CreateFileW(lp::Normalize(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) != FALSE &&
                  written == bytes.size();
  ::CloseHandle(file);
  return ok;
}

bool ReadBytesRaw(const std::wstring& path, std::vector<uint8_t>* out) {
  HANDLE file =
      ::CreateFileW(lp::Normalize(path).c_str(), GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  out->clear();
  uint8_t chunk[4096];
  for (;;) {
    DWORD read = 0;
    if (!::ReadFile(file, chunk, sizeof(chunk), &read, nullptr)) {
      ::CloseHandle(file);
      return false;
    }
    if (read == 0) break;
    out->insert(out->end(), chunk, chunk + read);
  }
  ::CloseHandle(file);
  return true;
}

std::vector<uint8_t> Payload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

// ---------------------------------------------------------------------------
// FakeBackend: injectable, records the exact three-method traffic.
// ---------------------------------------------------------------------------
class FakeBackend final : public wb::WriteBackend {
 public:
  enum class Call { Create, Write, Close };

  struct Entry {
    Call call;
    std::wstring path;
    std::vector<uint8_t> bytes;
  };

  size_t failCreateOn = 0;  // 1-based; 0 = never
  size_t failWriteOn = 0;   // 1-based; 0 = never

  wb::FileHandle CreateFile(const std::wstring& path, std::wstring* why) override {
    ++createCalls;
    calls.push_back({Call::Create, path, {}});
    if (failCreateOn != 0 && createCalls == failCreateOn) {
      if (why) *why = L"injected create failure";
      return wb::kInvalidFile;
    }
    const auto handle = reinterpret_cast<wb::FileHandle>(static_cast<uintptr_t>(0x1000 + nextHandle++));
    paths[handle] = path;
    return handle;
  }

  bool Write(wb::FileHandle file, const void* data, size_t size, std::wstring* why) override {
    ++writeCalls;
    calls.push_back({Call::Write, paths[file], {}});
    ++writesForHandle[file];
    if (failWriteOn != 0 && writeCalls == failWriteOn) {
      if (why) *why = L"injected write failure";
      return false;
    }
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    contents[file].assign(bytes, bytes + size);
    return true;
  }

  bool Close(wb::FileHandle file, std::wstring* why) override {
    ++closeCalls;
    calls.push_back({Call::Close, paths[file], {}});
    if (failWriteOn != 0 && writesForHandle[file] == 0) {
      // Closing a handle whose write was never attempted is a driver bug.
      ++closeWithoutWrite;
    }
    ++closed[file];
    (void)why;
    return true;
  }

  size_t createCalls = 0;
  size_t writeCalls = 0;
  size_t closeCalls = 0;
  size_t closeWithoutWrite = 0;
  std::vector<Entry> calls;
  std::map<wb::FileHandle, std::wstring> paths;
  std::map<wb::FileHandle, std::vector<uint8_t>> contents;
  std::map<wb::FileHandle, int> writesForHandle;
  std::map<wb::FileHandle, int> closed;
  uintptr_t nextHandle = 1;
};

// Reads the byte payload of a backend, by kind, for the shared verification.
struct BackendRun {
  wb::WriteStats stats;
  uint64_t directoriesCreated = 0;
  uint64_t handlesReused = 0;
  bool ioringAvailable = false;
  std::wstring ioringReason;
  uint64_t ioringSubmissions = 0;
  uint64_t ioringEntries = 0;
  uint64_t ioringBatch = 0;
};

BackendRun RunRealBackend(const std::vector<wb::WriteItem>& plan, wb::BackendKind kind,
                          size_t window) {
  BackendRun run;
  std::vector<std::wstring> destinations;
  destinations.reserve(plan.size());
  for (const wb::WriteItem& item : plan) destinations.push_back(item.path);

  if (kind == wb::BackendKind::Sequential) {
    wb::SequentialBackend backend;
    run.stats = wb::WriteAll(backend, plan, window);
    run.directoriesCreated = backend.directoriesCreated();
  } else if (kind == wb::BackendKind::Precreate) {
    wb::PrecreateBackend backend(destinations);
    run.stats = wb::WriteAll(backend, plan, window);
    run.directoriesCreated = backend.directoriesCreated();
    run.handlesReused = backend.handlesReused();
  } else {
    wb::IoRingBackend backend(destinations, 8);
    run.ioringAvailable = backend.available();
    run.ioringReason = backend.degradedReason();
    run.ioringBatch = backend.batchSize();
    run.stats = wb::WriteAll(backend, plan, window);
    run.ioringSubmissions = backend.submissions();
    run.ioringEntries = backend.entriesSubmitted();
    run.directoriesCreated = backend.precreate().directoriesCreated();
  }
  return run;
}

// ---------------------------------------------------------------------------
// 1. LongPath: string contract (no disk).
// ---------------------------------------------------------------------------
PRISM_TEST("long_path_string_contract") {
  const std::wstring plain = L"C:\\work\\repo\\sub\\..\\sub\\file.txt";
  const std::wstring normalized = lp::Normalize(plain);
  PRISM_CHECK_EQ(Narrow(normalized), std::string("\\\\?\\C:\\work\\repo\\sub\\file.txt"));
  PRISM_CHECK(lp::IsExtended(normalized));
  PRISM_CHECK_EQ(Narrow(lp::ToDisplay(normalized)), std::string("C:\\work\\repo\\sub\\file.txt"));

  // Idempotent: feeding an extended path back in must not double the prefix.
  PRISM_CHECK(Same(lp::Normalize(normalized), normalized));

  // UNC -> \?\UNC\server\share\...
  const std::wstring unc = lp::Normalize(L"\\\\server\\share\\dir\\file.txt");
  PRISM_CHECK_EQ(Narrow(unc), std::string("\\\\?\\UNC\\server\\share\\dir\\file.txt"));
  PRISM_CHECK_EQ(Narrow(lp::ToDisplay(unc)), std::string("\\\\server\\share\\dir\\file.txt"));
  PRISM_CHECK(Same(lp::Normalize(unc), unc));

  // Device namespace is passed through untouched (not a filesystem path).
  const std::wstring device = L"\\\\.\\PhysicalDrive0";
  PRISM_CHECK(Same(lp::Normalize(device), device));

  // Reserved DOS device names: extension and case do not save them.
  PRISM_CHECK(lp::IsReservedBaseName(L"CON"));
  PRISM_CHECK(lp::IsReservedBaseName(L"con.txt"));
  PRISM_CHECK(lp::IsReservedBaseName(L"Aux.c"));
  PRISM_CHECK(lp::IsReservedBaseName(L"nul"));
  PRISM_CHECK(lp::IsReservedBaseName(L"COM1"));
  PRISM_CHECK(lp::IsReservedBaseName(L"lpt9.log"));
  PRISM_CHECK(!lp::IsReservedBaseName(L"console.txt"));
  PRISM_CHECK(!lp::IsReservedBaseName(L"COM0"));
  PRISM_CHECK(!lp::IsReservedBaseName(L"COM10"));
  PRISM_CHECK(lp::NeedsExtendedPrefix(L"aux.c"));
  PRISM_CHECK(lp::NeedsExtendedPrefix(std::wstring(300, L'a')));

  // Relative paths: resolved against the cwd, and '.'/'..' must be gone BEFORE
  // the prefix is applied (the prefix disables normalisation).
  wchar_t previous[MAX_PATH * 4] = {};
  const DWORD previousLength = ::GetCurrentDirectoryW(MAX_PATH * 4, previous);
  const std::wstring root = TempRoot(L"longpath_string");
  PRISM_CHECK(::SetCurrentDirectoryW(lp::Normalize(root).c_str()) != FALSE);
  const std::wstring relative = lp::Normalize(L"sub\\..\\sub\\x.txt");
  PRISM_CHECK_EQ(Narrow(relative), Narrow(lp::Normalize(root + L"\\sub\\x.txt")));
  PRISM_CHECK(lp::IsExtended(relative));
  if (previousLength > 0) ::SetCurrentDirectoryW(previous);

  // Display round trip over a set of absolute shapes.
  const std::wstring cases[] = {L"C:\\a.txt", L"C:\\dir with space\\b.txt",
                                L"D:\\trailing.\\c.", L"E:\\unicode-\u4e2d\u6587\\d.bin"};
  for (const std::wstring& c : cases) {
    PRISM_CHECK(Same(lp::Normalize(lp::ToDisplay(lp::Normalize(c))), lp::Normalize(c)));
  }
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// 2. Longer than MAX_PATH, on a real disk: directory tree + file + readback.
// ---------------------------------------------------------------------------
PRISM_TEST("long_path_over_max_path_roundtrip") {
  const std::wstring root = TempRoot(L"longpath_disk");
  std::wstring deep = root;
  for (int i = 0; i < 24; ++i) {
    deep += L"\\deep_directory_number_";
    deep += static_cast<wchar_t>(L'a' + (i % 26));
    deep += std::to_wstring(i);
  }
  const std::wstring file = deep + L"\\payload.bin";
  const std::wstring normalized = lp::Normalize(file);
  PRISM_CHECK(lp::ExceedsMaxPath(normalized));
  PRISM_CHECK(normalized.size() > 260);
  PRISM_CHECK(::wcslen(lp::ToDisplay(normalized).c_str()) >= 260);

  {
    const std::vector<uint8_t> bytes = Payload(0xC32u, 4096);
    wb::PrecreateBackend backend({file});
    const std::vector<wb::WriteItem> plan = {{file, bytes.data(), bytes.size()}};
    const wb::WriteStats stats = wb::WriteAll(backend, plan);
    PRISM_CHECK(stats.ok);
    PRISM_CHECK_EQ(stats.files, static_cast<uint64_t>(1));
  }  // backend destroyed -> handles closed before readback

  std::vector<uint8_t> readBack;
  PRISM_CHECK(ReadBytesRaw(file, &readBack));
  PRISM_CHECK_EQ(readBack.size(), static_cast<size_t>(4096));
  const std::vector<uint8_t> expected = Payload(0xC32u, 4096);
  PRISM_CHECK(readBack == expected);
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// 3. Reserved names as REAL files: the extended prefix + POSIX semantics is what
//    makes con.txt / aux.c / nul legal names instead of DOS devices.
// ---------------------------------------------------------------------------
PRISM_TEST("reserved_names_are_real_files") {
  const std::wstring root = TempRoot(L"reserved");
  const std::wstring names[] = {L"aux.c", L"con.txt", L"nul", L"COM1.txt", L"lpt9.log"};
  std::vector<std::wstring> paths;
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<wb::WriteItem> plan;
  for (size_t i = 0; i < std::size(names); ++i) {
    paths.push_back(root + L"\\reserved\\" + names[i]);
    payloads.push_back(Payload(0x100u + static_cast<uint32_t>(i), 777));
  }
  for (size_t i = 0; i < paths.size(); ++i) {
    plan.push_back({paths[i], payloads[i].data(), payloads[i].size()});
  }
  {
    wb::SequentialBackend backend;
    const wb::WriteStats stats = wb::WriteAll(backend, plan);
    PRISM_CHECK(stats.ok);
    if (!stats.ok) std::wcout << L"    reserved first error: " << stats.firstError << std::endl;
  }
  for (size_t i = 0; i < paths.size(); ++i) {
    const DWORD attributes = ::GetFileAttributesW(lp::Normalize(paths[i]).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      std::wcout << L"    reserved name missing: " << names[i] << L" path=" << paths[i]
                 << L" err=" << ::GetLastError() << std::endl;
    }
    PRISM_CHECK(attributes != INVALID_FILE_ATTRIBUTES);
    std::vector<uint8_t> readBack;
    PRISM_CHECK(ReadBytesRaw(paths[i], &readBack));
    PRISM_CHECK(readBack == payloads[i]);
  }
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// 4. FakeBackend: the three-method contract and the driver's error handling.
// ---------------------------------------------------------------------------
PRISM_TEST("fake_backend_driver_contract") {
  const std::vector<std::vector<uint8_t>> payloads = {Payload(1, 16), Payload(2, 32),
                                                      Payload(3, 64)};
  std::vector<wb::WriteItem> plan;
  for (size_t i = 0; i < payloads.size(); ++i) {
    plan.push_back({L"C:\\fake\\f" + std::to_wstring(i) + L".bin", payloads[i].data(),
                    payloads[i].size()});
  }

  {
    FakeBackend backend;
    const wb::WriteStats stats = wb::WriteAll(backend, plan);
    PRISM_CHECK(stats.ok);
    PRISM_CHECK_EQ(stats.files, static_cast<uint64_t>(3));
    PRISM_CHECK_EQ(stats.bytes, static_cast<uint64_t>(16 + 32 + 64));
    PRISM_CHECK_EQ(backend.createCalls, static_cast<size_t>(3));
    PRISM_CHECK_EQ(backend.writeCalls, static_cast<size_t>(3));
    PRISM_CHECK_EQ(backend.closeCalls, static_cast<size_t>(3));
    // Every written handle was closed exactly once with the right bytes.
    for (const auto& entry : backend.contents) {
      PRISM_CHECK_EQ(backend.closed[entry.first], 1);
    }
    size_t index = 0;
    for (const auto& entry : backend.contents) {
      PRISM_CHECK(entry.second == payloads[index]);
      ++index;
    }
  }

  // A create failure must stop the window and still close what was opened.
  {
    FakeBackend backend;
    backend.failCreateOn = 2;
    const wb::WriteStats stats = wb::WriteAll(backend, plan, 3);
    PRISM_CHECK(!stats.ok);
    PRISM_CHECK(!stats.firstError.empty());
    PRISM_CHECK_EQ(backend.createCalls, static_cast<size_t>(2));
    PRISM_CHECK_EQ(backend.closeCalls, static_cast<size_t>(1));  // the first file only
    PRISM_CHECK_EQ(backend.closeWithoutWrite, static_cast<size_t>(0));
  }

  // A write failure must still close the handle it opened.
  {
    FakeBackend backend;
    backend.failWriteOn = 2;
    const wb::WriteStats stats = wb::WriteAll(backend, plan);
    PRISM_CHECK(!stats.ok);
    PRISM_CHECK(!stats.firstError.empty());
    PRISM_CHECK_EQ(backend.closeCalls, static_cast<size_t>(2));
    PRISM_CHECK_EQ(backend.closeWithoutWrite, static_cast<size_t>(0));
  }

  // Pipeline window: opens/writes are batched, closes happen after the window.
  {
    FakeBackend backend;
    const wb::WriteStats stats = wb::WriteAll(backend, plan, 2);
    PRISM_CHECK(stats.ok);
    // 3 items in windows of 2 = two batched files, then the last one alone.
    PRISM_CHECK_EQ(backend.calls.size(), static_cast<size_t>(9));
    PRISM_CHECK(backend.calls[0].call == FakeBackend::Call::Create);
    PRISM_CHECK(backend.calls[1].call == FakeBackend::Call::Write);
    PRISM_CHECK(backend.calls[2].call == FakeBackend::Call::Create);
    PRISM_CHECK(backend.calls[3].call == FakeBackend::Call::Write);
    PRISM_CHECK(backend.calls[4].call == FakeBackend::Call::Close);
    PRISM_CHECK(backend.calls[5].call == FakeBackend::Call::Close);
    PRISM_CHECK(backend.calls[6].call == FakeBackend::Call::Create);
    PRISM_CHECK(backend.calls[7].call == FakeBackend::Call::Write);
    PRISM_CHECK(backend.calls[8].call == FakeBackend::Call::Close);
  }
}

// ---------------------------------------------------------------------------
// 5. All three real backends: same 60-file / 6-directory tree, same bytes.
// ---------------------------------------------------------------------------
PRISM_TEST("real_backends_write_identical_bytes") {
  constexpr size_t kFiles = 60;
  constexpr size_t kBytes = 2048;
  const std::wstring root = TempRoot(L"backends");

  std::vector<std::vector<uint8_t>> payloads;
  payloads.reserve(kFiles);
  for (size_t i = 0; i < kFiles; ++i) {
    payloads.push_back(Payload(static_cast<uint32_t>(i + 1), kBytes));
  }

  const wb::BackendKind kinds[] = {wb::BackendKind::Sequential, wb::BackendKind::Precreate,
                                   wb::BackendKind::IoRing};
  for (const wb::BackendKind kind : kinds) {
    const std::wstring subRoot =
        root + L"\\" + static_cast<wchar_t>(L's') + std::to_wstring(static_cast<int>(kind));
    std::vector<std::wstring> paths;
    std::vector<wb::WriteItem> plan;
    for (size_t i = 0; i < kFiles; ++i) {
      paths.push_back(subRoot + L"\\d" + std::to_wstring(i % 6) + L"\\file" +
                      std::to_wstring(i) + L".bin");
      plan.push_back({paths[i], payloads[i].data(), payloads[i].size()});
    }

    const size_t window = kind == wb::BackendKind::IoRing ? 8 : 1;
    const BackendRun run = RunRealBackend(plan, kind, window);
    PRISM_CHECK(run.stats.ok);
    PRISM_CHECK_EQ(run.stats.files, static_cast<uint64_t>(kFiles));
    PRISM_CHECK_EQ(run.stats.bytes, static_cast<uint64_t>(kFiles * kBytes));
    PRISM_CHECK(run.directoriesCreated >= 6);

    if (kind == wb::BackendKind::IoRing) {
      if (run.ioringAvailable) {
        PRISM_CHECK(run.ioringEntries >= kFiles);
        PRISM_CHECK(run.ioringSubmissions > 0);
        PRISM_CHECK_EQ(run.ioringBatch, static_cast<uint64_t>(8));
      } else {
        // Degradation must be EXPLAINED, and the fallback must still be correct
        // (asserted below by the byte comparison).
        PRISM_CHECK(!run.ioringReason.empty());
        std::wcout << L"    [info] IoRing degraded: " << run.ioringReason << std::endl;
      }
    }

    // Bytes, not existence (contract D2).
    for (size_t i = 0; i < kFiles; ++i) {
      std::vector<uint8_t> readBack;
      PRISM_CHECK(ReadBytesRaw(paths[i], &readBack));
      PRISM_CHECK(readBack == payloads[i]);
    }
    RemoveTree(subRoot);
  }
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// 6. Handle reuse: a destination path that appears twice in the plan reuses the
//    one handle Win32 actually allows (the same path), and the last write wins.
// ---------------------------------------------------------------------------
PRISM_TEST("precreate_reuses_handle_for_repeated_destination") {
  const std::wstring root = TempRoot(L"reuse");
  const std::wstring path = root + L"\\dup\\same.bin";
  const std::vector<uint8_t> first = Payload(11, 512);
  const std::vector<uint8_t> second = Payload(22, 700);
  const std::vector<wb::WriteItem> plan = {{path, first.data(), first.size()},
                                           {path, second.data(), second.size()}};
  {
    wb::PrecreateBackend backend({path, path});
    const wb::WriteStats stats = wb::WriteAll(backend, plan);
    PRISM_CHECK(stats.ok);
    PRISM_CHECK_EQ(backend.handlesReused(), static_cast<uint64_t>(1));
  }
  std::vector<uint8_t> readBack;
  PRISM_CHECK(ReadBytesRaw(path, &readBack));
  PRISM_CHECK(readBack == second);
  RemoveTree(root);
}

// ---------------------------------------------------------------------------
// 7. Large pages: call C1's entry point, then PROBE whether it can work.
// ---------------------------------------------------------------------------
PRISM_TEST("large_pages_probe_reports_the_truth") {
  wchar_t dllPath[32768] = {};
  const DWORD dllLength = ::GetEnvironmentVariableW(L"PRISM_7Z_DLL", dllPath, 32768);
  const std::wstring sevenZipDll = dllLength > 0 ? std::wstring(dllPath, dllLength) : std::wstring();

  wb::LargePageStatus status;
  if (!sevenZipDll.empty()) {
    prism::win::SevenZipLibrary library;
    const bool loaded = library.Load(sevenZipDll);
    PRISM_CHECK(loaded);
    if (loaded) {
      // The blueprint's request side: C1's wrapper calls 7z.dll SetLargePageMode.
      library.EnableLargePages();
      status = wb::ProbeLargePages(sevenZipDll);
      // The engine we claim to call really exports the switch. If this fails,
      // the "large page" cell would be a lie even before the OS is involved.
      PRISM_CHECK(status.sevenZipExportPresent);
    }
  } else {
    status = wb::ProbeLargePages(std::wstring());
    PRISM_CHECK(!status.sevenZipExportPresent);
  }

  // The probe must be internally consistent: "effective" is exactly the
  // conjunction, never a stamp of approval.
  PRISM_CHECK_EQ(status.effective(),
                 status.sevenZipExportPresent && status.privilegeHeld && status.allocationWorks);
  if (status.effective()) {
    PRISM_CHECK(status.largePageMinimum > 0);
    PRISM_CHECK(status.detail.find(L"enabled:") == 0);
  } else {
    PRISM_CHECK(status.detail.find(L"degraded:") == 0);
    PRISM_CHECK(status.detail.size() > 10);
  }

  std::cout << "LARGE-PAGES {\"export\":" << (status.sevenZipExportPresent ? "true" : "false")
            << ",\"privilege\":" << (status.privilegeHeld ? "true" : "false")
            << ",\"allocation\":" << (status.allocationWorks ? "true" : "false")
            << ",\"effective\":" << (status.effective() ? "true" : "false")
            << ",\"min_bytes\":" << status.largePageMinimum
            << ",\"last_error\":" << status.lastError
            << ",\"detail\":\"" << Narrow(status.detail) << "\"}" << std::endl;
}

// ---------------------------------------------------------------------------
// Negative controls. Each returns 0 ONLY when the injected defect was observed
// AND the injection counter proves the branch compiled in (D3), and prints the
// marker the ctest registration pins (D26: no WILL_FAIL on top of a regex).
// ---------------------------------------------------------------------------
int RunNoPredirMutant() {
  const std::wstring root = TempRoot(L"mutant_predir");
  std::cout << "WRITE-BACKEND MUTANT ACTIVE (PRISM_WRITE_MUTANT_NO_PREDIR)" << std::endl;
  std::vector<std::vector<uint8_t>> payloads = {Payload(1, 128), Payload(2, 128)};
  const std::vector<std::wstring> paths = {root + L"\\pre\\nested\\one.bin",
                                           root + L"\\pre\\nested\\two.bin"};
  std::vector<wb::WriteItem> plan;
  for (size_t i = 0; i < paths.size(); ++i) {
    plan.push_back({paths[i], payloads[i].data(), payloads[i].size()});
  }
  bool precreateFailed = false;
  wb::WriteStats stats;
  {
    wb::PrecreateBackend backend(paths);
    precreateFailed = backend.precreateFailed();
    stats = wb::WriteAll(backend, plan);
  }
  const int injections = lp::InjectionCount();
  const bool caught = precreateFailed && !stats.ok && !stats.firstError.empty() && injections > 0;
  RemoveTree(root);
  std::cout << (caught ? "WRITE-BACKEND MUTANT CAUGHT" : "WRITE-BACKEND MUTANT NOT CAUGHT")
            << " (injections=" << injections << ", precreate_failed=" << precreateFailed
            << ", first_error=" << Narrow(stats.firstError) << ")" << std::endl;
  return caught ? 0 : 1;
}

int RunLongPathMutant() {
  std::cout << "LONGPATH MUTANT ACTIVE (PRISM_LONGPATH_MUTANT_STRIP_PREFIX)" << std::endl;
  const std::wstring normalized = lp::Normalize(L"C:\\work\\repo\\file.txt");
  const bool prefixPresent = lp::IsExtended(normalized);
  const int injections = lp::InjectionCount();
  const bool caught = !prefixPresent && injections > 0;
  std::cout << (caught ? "LONGPATH MUTANT CAUGHT" : "LONGPATH MUTANT NOT CAUGHT")
            << " (injections=" << injections << ", normalized=\"" << Narrow(normalized) << "\")"
            << std::endl;
  return caught ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(PRISM_WRITE_MUTANT_NO_PREDIR)
  (void)argc;
  (void)argv;
  return RunNoPredirMutant();
#elif defined(PRISM_LONGPATH_MUTANT_STRIP_PREFIX)
  (void)argc;
  (void)argv;
  return RunLongPathMutant();
#else
  const int result = ::prism::test::RunAll(argc, argv);
  if (result == 0) std::cout << "ALL WRITE-BACKEND CHECKS PASSED" << std::endl;
  return result;
#endif
}
