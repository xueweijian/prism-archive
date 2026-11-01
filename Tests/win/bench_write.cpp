// PrismArchive -- small-file write benchmark (blueprint ch. 7.2).
//
// The cell: 8 000 x 2 KiB files across 100 directories, written three ways:
//
//   sequential  baseline: per-file CreateDirectoryW + CreateFileW + WriteFile
//   precreate   directory tree made once, then CreateFileW + WriteFile; a
//               repeated destination path reuses its one legal handle
//   ioring      Windows 11 BuildIoRing batch submission (window = 32 files),
//               dynamically loaded; degrades to precreate WITH A REASON when the
//               API/opcode is absent
//
// Honesty rules (docs/PERF.md, contracts D1/D2):
//   * every file's bytes are READ BACK and compared; a timing without that is
//     worth nothing (D2);
//   * the only duration assertion is a loose hang bound (120 s per backend) --
//     a shared 4-core box cannot gate a stopwatch, so the digits are report-only
//     and land in the CI step summary as BENCH lines;
//   * the full cell is timed, i.e. backend construction (which is where the tree
//     pre-creation happens) + writes + destruction.
//
// Output:
//   BENCH {"cell":"small_file_write","backend":"...","files":8000,...}
//   BENCH {"cell":"small_file_write_summary",...,"precreate_speedup":..}
//   BENCH {"cell":"large_pages","export":..,"privilege":..,...}
//   ALL WRITE-BENCH CHECKS PASSED
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "LongPath.hpp"
#include "PrismEngineBridge.hpp"
#include "WriteBackend.hpp"

namespace lp = prism::win::longpath;
namespace wb = prism::win::writeback;

namespace {

constexpr size_t kFiles = 8000;
constexpr size_t kBytesPerFile = 2u * 1024u;
constexpr size_t kDirectories = 100;
constexpr size_t kIoRingBatch = 32;
constexpr double kLooseUpperBoundMs = 120000.0;  // hang detector only

int gFailures = 0;

void Check(bool ok, const char* what) {
  if (!ok) {
    std::printf("    FAIL  %s\n", what);
    ++gFailures;
  }
}

std::string Narrow(std::wstring_view text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) out.push_back(c < 0x80 ? static_cast<char>(c) : '?');
  return out;
}

double NowMs() {
  static LARGE_INTEGER frequency = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return value;
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

std::wstring TempRoot(const char* tag) {
  wchar_t temp[MAX_PATH] = {};
  const DWORD length = ::GetTempPathW(MAX_PATH, temp);
  const std::wstring base(length > 0 ? std::wstring(temp, length) : std::wstring(L".\\"));
  const std::wstring root = base + L"prism_c32_bench_" + std::to_wstring(::GetCurrentProcessId()) +
                            L"_" + std::wstring(tag, tag + std::strlen(tag));
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

std::vector<uint8_t> Payload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* out) {
  HANDLE file = ::CreateFileW(lp::Normalize(path).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  out->clear();
  uint8_t chunk[8192];
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

struct Plan {
  std::vector<std::wstring> paths;
  std::vector<std::vector<uint8_t>> payloads;
  std::vector<wb::WriteItem> items;
};

Plan MakePlan(const std::wstring& root) {
  Plan plan;
  plan.paths.reserve(kFiles);
  plan.payloads.reserve(kFiles);
  plan.items.reserve(kFiles);
  for (size_t i = 0; i < kFiles; ++i) {
    wchar_t directory[16] = {};
    ::swprintf(directory, 16, L"b%03zu", i % kDirectories);
    wchar_t name[32] = {};
    ::swprintf(name, 32, L"f%05zu.bin", i);
    plan.paths.push_back(root + L"\\" + directory + L"\\" + name);
    plan.payloads.push_back(Payload(static_cast<uint32_t>(i + 1), kBytesPerFile));
  }
  for (size_t i = 0; i < kFiles; ++i) {
    plan.items.push_back({plan.paths[i], plan.payloads[i].data(), plan.payloads[i].size()});
  }
  return plan;
}

struct Result {
  const char* backend = "";
  double elapsedMs = 0.0;
  double verifyMs = 0.0;
  uint64_t directoriesCreated = 0;
  uint64_t handlesReused = 0;
  uint64_t submissions = 0;
  uint64_t entriesSubmitted = 0;
  bool ioringAvailable = false;
  std::wstring ioringReason;
  uint64_t files = 0;
  uint64_t bytes = 0;
  size_t verified = 0;
  size_t mismatches = 0;
  bool ok = true;
};

Result RunBackend(wb::BackendKind kind, const char* tag) {
  const std::wstring root = TempRoot(tag);
  const Plan plan = MakePlan(root);
  Result result;
  result.backend = wb::BackendKindId(kind);

  std::vector<std::wstring> destinations;
  destinations.reserve(plan.paths.size());
  for (const std::wstring& path : plan.paths) destinations.push_back(path);

  const double started = NowMs();
  {
    if (kind == wb::BackendKind::Sequential) {
      wb::SequentialBackend backend;
      const wb::WriteStats stats = wb::WriteAll(backend, plan.items);
      result.files = stats.files;
      result.bytes = stats.bytes;
      result.ok = stats.ok;
      result.directoriesCreated = backend.directoriesCreated();
      if (!stats.ok) std::printf("    sequential error: %s\n", Narrow(stats.firstError).c_str());
    } else if (kind == wb::BackendKind::Precreate) {
      wb::PrecreateBackend backend(destinations);
      const wb::WriteStats stats = wb::WriteAll(backend, plan.items);
      result.files = stats.files;
      result.bytes = stats.bytes;
      result.ok = stats.ok;
      result.directoriesCreated = backend.directoriesCreated();
      result.handlesReused = backend.handlesReused();
      if (!stats.ok) std::printf("    precreate error: %s\n", Narrow(stats.firstError).c_str());
    } else {
      wb::IoRingBackend backend(destinations, static_cast<uint32_t>(kIoRingBatch));
      result.ioringAvailable = backend.available();
      result.ioringReason = backend.degradedReason();
      const wb::WriteStats stats = wb::WriteAll(backend, plan.items, kIoRingBatch);
      result.files = stats.files;
      result.bytes = stats.bytes;
      result.ok = stats.ok;
      result.submissions = backend.submissions();
      result.entriesSubmitted = backend.entriesSubmitted();
      result.directoriesCreated = backend.precreate().directoriesCreated();
      if (!stats.ok) std::printf("    ioring error: %s\n", Narrow(stats.firstError).c_str());
    }
  }  // destruction is inside the measured cell (it closes deferred handles)
  result.elapsedMs = NowMs() - started;

  const double verifyStarted = NowMs();
  for (size_t i = 0; i < plan.paths.size(); ++i) {
    std::vector<uint8_t> readBack;
    if (!ReadWhole(plan.paths[i], &readBack) || readBack != plan.payloads[i]) {
      ++result.mismatches;
      if (result.mismatches == 1) {
        std::printf("    byte mismatch: %s\n", Narrow(plan.paths[i]).c_str());
      }
    } else {
      ++result.verified;
    }
  }
  result.verifyMs = NowMs() - verifyStarted;
  RemoveTree(root);
  return result;
}

}  // namespace

int main() {
  std::printf("small-file write benchmark: %zu files x %zu bytes in %zu directories\n", kFiles,
              kBytesPerFile, kDirectories);

  std::vector<Result> results;
  results.push_back(RunBackend(wb::BackendKind::Sequential, "seq"));
  results.push_back(RunBackend(wb::BackendKind::Precreate, "pre"));
  results.push_back(RunBackend(wb::BackendKind::IoRing, "ior"));

  for (const Result& result : results) {
    Check(result.files == kFiles, "all files written");
    Check(result.bytes == kFiles * kBytesPerFile, "all bytes written");
    Check(result.ok, "backend reported success");
    Check(result.mismatches == 0, "every file read back byte-identical");
    Check(result.verified == kFiles, "every file verified");
    Check(result.elapsedMs < kLooseUpperBoundMs, "elapsed within the loose hang bound");
    const double seconds = result.elapsedMs / 1000.0;
    const double filesPerSecond = seconds > 0.0 ? static_cast<double>(kFiles) / seconds : 0.0;
    const double mbPerSecond =
        seconds > 0.0 ? (static_cast<double>(kFiles * kBytesPerFile) / (1024.0 * 1024.0)) / seconds
                      : 0.0;
    std::printf(
        "BENCH {\"cell\":\"small_file_write\",\"backend\":\"%s\",\"files\":%zu,"
        "\"bytes_per_file\":%zu,\"directories\":%zu,\"elapsed_ms\":%.1f,"
        "\"files_per_s\":%.1f,\"mb_per_s\":%.2f,\"dirs_created\":%llu,"
        "\"handles_reused\":%llu,\"verified\":%zu,\"mismatches\":%zu,"
        "\"ioring\":\"%s\",\"ioring_reason\":\"%s\",\"submissions\":%llu,"
        "\"entries_submitted\":%llu}\n",
        result.backend, kFiles, kBytesPerFile, kDirectories, result.elapsedMs, filesPerSecond,
        mbPerSecond, static_cast<unsigned long long>(result.directoriesCreated),
        static_cast<unsigned long long>(result.handlesReused), result.verified, result.mismatches,
        result.ioringAvailable ? "available" : (result.backend[0] == 'i' ? "degraded" : "not-used"),
        Narrow(result.ioringReason).c_str(), static_cast<unsigned long long>(result.submissions),
        static_cast<unsigned long long>(result.entriesSubmitted));
  }

  // Report-only comparison against the baseline. A zero clamp keeps the JSON
  // valid when a sibling build storm makes the baseline absurdly slow.
  const double sequentialMs = results[0].elapsedMs;
  const double precreateMs = results[1].elapsedMs;
  const double ioringMs = results[2].elapsedMs;
  const double precreateSpeedup = precreateMs > 0.0 ? sequentialMs / precreateMs : 0.0;
  const double ioringSpeedup = ioringMs > 0.0 ? sequentialMs / ioringMs : 0.0;
  std::printf(
      "BENCH {\"cell\":\"small_file_write_summary\",\"sequential_ms\":%.1f,"
      "\"precreate_ms\":%.1f,\"ioring_ms\":%.1f,\"precreate_speedup\":%.3f,"
      "\"ioring_speedup\":%.3f,\"files\":%zu}\n",
      sequentialMs, precreateMs, ioringMs, precreateSpeedup, ioringSpeedup, kFiles);

  // Large pages: ask the engine (C1's SevenZipLibrary wrapper) and probe whether
  // the OS can honour it. Report-only; "degraded" is a legitimate outcome.
  wchar_t dllPath[32768] = {};
  const DWORD dllLength = ::GetEnvironmentVariableW(L"PRISM_7Z_DLL", dllPath, 32768);
  const std::wstring sevenZipDll = dllLength > 0 ? std::wstring(dllPath, dllLength) : std::wstring();
  wb::LargePageStatus pages;
  if (!sevenZipDll.empty()) {
    prism::win::SevenZipLibrary library;
    if (library.Load(sevenZipDll)) {
      library.EnableLargePages();
      pages = wb::ProbeLargePages(sevenZipDll);
    } else {
      pages = wb::ProbeLargePages(std::wstring());
      pages.detail = L"degraded: 7z.dll could not be loaded from PRISM_7Z_DLL";
    }
  } else {
    pages = wb::ProbeLargePages(std::wstring());
    pages.detail = L"degraded: PRISM_7Z_DLL is not set, so SetLargePageMode was not requested";
  }
  std::printf(
      "BENCH {\"cell\":\"large_pages\",\"effective\":%s,\"export\":%s,\"privilege\":%s,"
      "\"allocation\":%s,\"min_bytes\":%u,\"last_error\":%u,\"detail\":\"%s\"}\n",
      pages.effective() ? "true" : "false", pages.sevenZipExportPresent ? "true" : "false",
      pages.privilegeHeld ? "true" : "false", pages.allocationWorks ? "true" : "false",
      pages.largePageMinimum, pages.lastError, Narrow(pages.detail).c_str());

  // The probe must never claim more than its own conjunction.
  Check(pages.effective() ==
            (pages.sevenZipExportPresent && pages.privilegeHeld && pages.allocationWorks),
        "large-page probe is self-consistent");

  if (gFailures == 0) {
    std::printf("ALL WRITE-BENCH CHECKS PASSED\n");
    return 0;
  }
  std::printf("WRITE-BENCH FAILED: %d check(s)\n", gFailures);
  return 1;
}
