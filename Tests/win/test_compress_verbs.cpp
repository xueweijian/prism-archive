// PrismArchive -- card C58: the context menu's creation verbs really work.
//
// WHY THIS FILE EXISTS
// --------------------
// C4's registrar has rendered --compress-7z / --compress-zip /
// --smart-extract-each since the shell extension landed. C25's suite pinned the
// exe REFUSING them ("this version does not support that command"), which was
// honest but left the menu advertising three commands the product could not
// perform -- the defect class this card exists to kill. This suite proves the
// implementation end to end, against a real 7z.dll:
//
//   (1) the parser maps each C4 flag to its own mode, and the mode to the right
//       FORMAT (--compress-zip must not quietly write a .7z);
//   (2) --compress-7z writes a .7z that re-opens with exactly two entries and
//       byte-identical payloads;
//   (3) --compress-zip writes a real zip (PK local header; the reader's own
//       sniff route says "zip"), same two-entry proof;
//   (4) --smart-extract-each puts each archive in its OWN folder with the right
//       bytes (two archives, two folders, no cross-talk);
//   (5) a missing input exits 1, says WHY on stderr, and leaves NOTHING on disk;
//   (6) the same two verdicts through the REAL command line: this suite spawns
//       the built PrismArchive.exe (path passed by CMake) and reads its exit
//       code and redirected stderr -- the wiring is only worth anything if the
//       shipped binary answers this way;
//   (7) EVERY menu verb as its own process with a 15-second BOUND: no run may
//       still be alive (a modal box hangs a batch caller), exit codes must be
//       0/2 as promised, successful verbs must produce their files, and
//       --extract-here/--test are the known-good controls that prove the bound
//       can tell a working verb from a hung one.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26/D27), compiled into this same source:
//   * PRISM_C58_MUTANT_ZIP_AS_7Z        -- the format mapping is constant 7z, so
//     "--compress-zip" produces a .7z. The zip assertions MUST go red.
//   * PRISM_C58_MUTANT_CREATE_ON_FAILURE -- the failure path leaves an empty
//     output file. The "nothing on disk" assertion MUST go red.
// No WILL_FAIL: the mutant builds arm mini_test's verdict BEFORE the first
// assertion (D27), print "C58 MUTATION CAUGHT: <name>" at the first real
// failure and leave with 0; that marker is the CTest PASS_REGULAR_EXPRESSION,
// so a mutation that silently stopped applying leaves the suite green, prints
// no marker and turns the target RED (D26).
//
// D1: no 7z.dll -> print WHY and exit 77 (SKIP_RETURN_CODE), never a fake green.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismWin/ArchiveWriter.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "UniqueTempDir.hpp"

namespace fs = std::filesystem;
namespace app = prism::app;
namespace win = prism::win;
using prism::ArchiveItem;

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace {

#if defined(PRISM_C58_MUTANT_ZIP_AS_7Z)
constexpr bool kZipAs7zMutant = true;
constexpr bool kCreateOnFailureMutant = false;
#elif defined(PRISM_C58_MUTANT_CREATE_ON_FAILURE)
constexpr bool kZipAs7zMutant = false;
constexpr bool kCreateOnFailureMutant = true;
#else
constexpr bool kZipAs7zMutant = false;
constexpr bool kCreateOnFailureMutant = false;
#endif

std::wstring gRoot;        // unique scratch root for this process
std::wstring gExe;         // the real PrismArchive.exe (argv[1], from CMake)
win::SevenZipLibrary gLibrary;

void Emit(const std::string& line) { std::cout << line << std::endl; }

std::vector<uint8_t> Pattern(size_t n, uint8_t seed) {
  std::vector<uint8_t> bytes(n);
  for (size_t i = 0; i < n; ++i) {
    bytes[i] = static_cast<uint8_t>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
  }
  return bytes;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  while (done < bytes.size()) {
    DWORD wrote = 0;
    const DWORD want = static_cast<DWORD>((std::min)(size_t{1} << 16, bytes.size() - done));
    if (!WriteFile(h, bytes.data() + done, want, &wrote, nullptr) || wrote == 0) {
      CloseHandle(h);
      return false;
    }
    done += wrote;
  }
  CloseHandle(h);
  return true;
}

std::wstring Join(const std::wstring& a, const std::wstring& b) {
  if (a.empty()) return b;
  if (a.back() == L'\\' || a.back() == L'/') return a + b;
  return a + L"\\" + b;
}

// A fresh sub-directory under this process's unique scratch root.
std::wstring FreshDir(const wchar_t* leaf) {
  const std::wstring dir = Join(gRoot, leaf);
  std::error_code ec;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return ec ? std::wstring() : dir;
}

// Counts regular files directly under 'dir' (non-recursive). The failure-path
// contract is "not one file appears", so directories are not counted.
size_t CountFiles(const std::wstring& dir) {
  std::error_code ec;
  size_t files = 0;
  for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    std::error_code entryEc;
    if (it->is_regular_file(entryEc)) ++files;
  }
  return files;
}

// Opens 'archive' and returns the non-directory entries (path -> payload), or
// nullopt when the archive cannot be opened. This is the SAME reader the app
// ships; the payload comparison is byte-for-byte (CONTRACTS: assertions use
// bytes and counts, never "it exists").
struct Reopened {
  bool ok = false;
  std::string trace;
  std::map<std::string, std::vector<uint8_t>> files;
  size_t entries = 0;
  size_t dirs = 0;
};

Reopened Reopen(const std::wstring& archive) {
  Reopened out;
  win::ArchiveReader reader(gLibrary);
  if (!reader.Open(archive)) {
    out.trace = reader.openTrace();
    return out;
  }
  out.trace = reader.openTrace();
  const std::vector<ArchiveItem> catalog = reader.ReadCatalog();
  out.entries = catalog.size();
  for (const ArchiveItem& item : catalog) {
    if (item.isDir) {
      ++out.dirs;
      continue;
    }
    const std::optional<std::vector<uint8_t>> bytes = reader.ExtractToMemory(item.index);
    if (!bytes.has_value()) {
      out.trace += " cannot-extract=" + item.path;
      reader.Close();
      return out;
    }
    out.files[prism::NormalizeArchivePath(item.path)] = *bytes;
  }
  reader.Close();
  out.ok = true;
  return out;
}

std::string ReadText(const std::wstring& path) {
  std::vector<uint8_t> bytes;
  if (!win::cw::ReadWholeFile(path, &bytes)) return {};
  return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// One real command line, with a BOUNDED wait. The bound is the point: a verb
// that opens a modal message box never exits, and "the batch caller hangs" is
// exactly the defect this suite must catch (C58 follow-up: measured by the
// reviewer as >12 s alive on --compress-7z / --compress-zip /
// --smart-extract-each when the refusal path still owned a MessageBox).
struct BoundedRun {
  int exitCode = -1;      // -1 = could not start, -2 = killed on timeout
  bool timedOut = false;
  std::string text;       // stdout+stderr, as redirected
};

BoundedRun RunExeBounded(const std::wstring& exe, const std::vector<std::wstring>& args,
                         const std::wstring& workDir, const std::wstring& logPath,
                         DWORD timeoutMs) {
  BoundedRun out;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return out;
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::wstring command = L"\"" + exe + L"\"";
  for (const std::wstring& arg : args) command += L" \"" + arg + L"\"";

  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESTDHANDLES;
  si.hStdOutput = log;
  si.hStdError = log;
  si.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : INVALID_HANDLE_VALUE;
  PROCESS_INFORMATION pi{};
  const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                      CREATE_NO_WINDOW, nullptr,
                                      workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
  CloseHandle(log);
  if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
  if (!started) return out;
  const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
  if (wait == WAIT_TIMEOUT) {
    // A verb that is still alive at the bound is a hang (a modal box is the
    // usual cause). Kill it so the suite can finish and report the verdict.
    out.timedOut = true;
    out.exitCode = -2;
    TerminateProcess(pi.hProcess, 0xDEAD);
    WaitForSingleObject(pi.hProcess, 5000);
  } else {
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    out.exitCode = static_cast<int>(code);
  }
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  out.text = ReadText(logPath);
  return out;
}

// Exit-code-only wrapper for the cases that do not inspect the timeout flag.
int RunExeCapture(const std::wstring& exe, const std::vector<std::wstring>& args,
                  const std::wstring& workDir, const std::wstring& logPath) {
  return RunExeBounded(exe, args, workDir, logPath, 20000).exitCode;
}

// One archive's source: a file name and the exact bytes it must contain after a
// round trip.
struct SourceFile {
  std::wstring name;
  std::vector<uint8_t> bytes;
};

bool WriteSources(const std::wstring& dir, const std::vector<SourceFile>& files) {
  for (const SourceFile& file : files) {
    if (!WriteBytes(Join(dir, file.name), file.bytes)) return false;
  }
  return true;
}

// Compares a reopened archive against the sources: exact count, exact names,
// exact bytes. Prints what differed (a bare false would make a red run useless).
bool Matches(const Reopened& reopened, const std::vector<SourceFile>& files, const char* label) {
  const std::string tag = std::string("[") + label + "] ";
  if (!reopened.ok) {
    Emit(tag + "re-open failed: " + reopened.trace);
    return false;
  }
  if (reopened.entries != files.size() || reopened.dirs != 0) {
    Emit(tag + "entry count " + std::to_string(reopened.entries) + " (dirs " +
         std::to_string(reopened.dirs) + "), expected " + std::to_string(files.size()) + " files");
    return false;
  }
  for (const SourceFile& file : files) {
    const auto it = reopened.files.find(win::WideToUtf8(file.name));
    if (it == reopened.files.end()) {
      Emit(tag + "missing entry " + win::WideToUtf8(file.name));
      return false;
    }
    if (it->second != file.bytes) {
      Emit(tag + "payload mismatch for " + win::WideToUtf8(file.name) + " (" +
           std::to_string(it->second.size()) + " vs " + std::to_string(file.bytes.size()) +
           " bytes)");
      return false;
    }
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// (1) The parser contract: three verbs, three modes, two formats.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.parse_modes_and_formats") {
  const app::LaunchRequest seven =
      app::ParseLaunch({L"--compress-7z", L"C:\\src\\one.bin", L"C:\\src\\two.bin"});
  PRISM_CHECK(!seven.unknownFlag);
  PRISM_CHECK(seven.mode == app::LaunchMode::kCompress7z);
  PRISM_CHECK(app::Compresses(seven.mode));
  PRISM_CHECK_EQ(seven.inputs.size(), size_t(2));
  PRISM_CHECK_EQ(static_cast<int>(app::CompressionFormatOf(seven.mode)),
                 static_cast<int>(prism::ArchiveFormat::SevenZip));

  const app::LaunchRequest zip = app::ParseLaunch({L"--compress-zip", L"C:\\src\\one.bin"});
  PRISM_CHECK(!zip.unknownFlag);
  PRISM_CHECK(zip.mode == app::LaunchMode::kCompressZip);
  PRISM_CHECK(app::Compresses(zip.mode));
  PRISM_CHECK_EQ(static_cast<int>(app::CompressionFormatOf(zip.mode)),
                 static_cast<int>(prism::ArchiveFormat::Zip));

  const app::LaunchRequest each =
      app::ParseLaunch({L"--smart-extract-each", L"C:\\a\\one.7z", L"C:\\a\\two.zip"});
  PRISM_CHECK(!each.unknownFlag);
  PRISM_CHECK(each.mode == app::LaunchMode::kSmartExtractEach);
  PRISM_CHECK(app::Extracts(each.mode));
  PRISM_CHECK(!app::Compresses(each.mode));
  PRISM_CHECK_EQ(each.archives.size(), size_t(2));
  PRISM_CHECK_EQ(app::LaunchModeName(each.mode), std::string("SmartExtractEach"));

  // The refusal contract survives for everything that is still not implemented.
  PRISM_CHECK(app::ParseLaunch({L"--compress-rar", L"x"}).unknownFlag);
}

// ---------------------------------------------------------------------------
// (2) --compress-7z against the real engine.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.compress_7z_is_a_real_7z_with_the_right_bytes") {
  const std::wstring dir = FreshDir(L"seven");
  if (!PRISM_CHECK(!dir.empty())) return;
  const std::vector<SourceFile> files = {
      {L"alpha.bin", Pattern(4096, 1)},
      {L"beta.bin", Pattern(301, 2)},
  };
  if (!PRISM_CHECK(WriteSources(dir, files))) return;

  const app::LaunchRequest request =
      app::ParseLaunch({L"--compress-7z", Join(dir, L"alpha.bin"), Join(dir, L"beta.bin")});
  const app::CompressVerbOutcome outcome = app::RunCompressVerb(request);
  for (const std::string& line : outcome.lines) Emit("  stderr: " + line);

  PRISM_CHECK_EQ(outcome.exitCode, 0);
  if (!PRISM_CHECK(outcome.ok)) return;
  PRISM_CHECK(outcome.archivePath == Join(dir, L"alpha.7z"));
  std::error_code ec;
  PRISM_CHECK(fs::exists(outcome.archivePath, ec));

  const Reopened reopened = Reopen(outcome.archivePath);
  PRISM_CHECK(Matches(reopened, files, "7z"));
  // A .7z is not a zip: the first bytes are the 7z signature, so a "7z" that is
  // really a zip could never pass both this case and (3).
  std::vector<uint8_t> head;
  if (PRISM_CHECK(win::cw::ReadWholeFile(outcome.archivePath, &head))) {
    PRISM_CHECK(head.size() > 6);
    const uint8_t signature[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};
    PRISM_CHECK(std::memcmp(head.data(), signature, 6) == 0);
  }
}

// ---------------------------------------------------------------------------
// (3) --compress-zip must be a ZIP, not a 7z with a .zip name (the mutation).
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.compress_zip_is_a_real_zip_with_the_right_bytes") {
  const std::wstring dir = FreshDir(L"zip");
  if (!PRISM_CHECK(!dir.empty())) return;
  const std::vector<SourceFile> files = {
      {L"one.bin", Pattern(2048, 3)},
      {L"two.txt", Pattern(97, 4)},
  };
  if (!PRISM_CHECK(WriteSources(dir, files))) return;

  const app::LaunchRequest request =
      app::ParseLaunch({L"--compress-zip", Join(dir, L"one.bin"), Join(dir, L"two.txt")});
  const app::CompressVerbOutcome outcome = app::RunCompressVerb(request);
  for (const std::string& line : outcome.lines) Emit("  stderr: " + line);

  PRISM_CHECK_EQ(outcome.exitCode, 0);
  if (!PRISM_CHECK(outcome.ok)) return;
  // The extension is part of the promise the menu makes ("compress to zip").
  PRISM_CHECK(outcome.archivePath == Join(dir, L"one.zip"));

  std::vector<uint8_t> head;
  if (!PRISM_CHECK(win::cw::ReadWholeFile(outcome.archivePath, &head))) return;
  PRISM_CHECK(head.size() > 4);
  // PK\x03\x04 -- a local file header, i.e. a container Explorer can open.
  PRISM_CHECK(head[0] == 'P' && head[1] == 'K' && head[2] == 0x03 && head[3] == 0x04);

  const Reopened reopened = Reopen(outcome.archivePath);
  PRISM_CHECK(Matches(reopened, files, "zip"));
  // The reader routes by MAGIC, and the trace names the handler it used. This is
  // the "opened by the zip handler" half: a .7z named .zip would say sniff=7z.
  Emit("  zip open trace: " + reopened.trace);
  PRISM_CHECK(reopened.trace.find("sniff=zip") != std::string::npos);
}

// ---------------------------------------------------------------------------
// (4) --smart-extract-each: N archives -> N folders named after them.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.smart_extract_each_gives_every_archive_its_own_folder") {
  const std::wstring root = FreshDir(L"each");
  if (!PRISM_CHECK(!root.empty())) return;
  const std::wstring srcA = Join(root, L"src-a");
  const std::wstring srcB = Join(root, L"src-b");
  std::error_code ec;
  fs::create_directories(srcA, ec);
  fs::create_directories(srcB, ec);
  if (!PRISM_CHECK(!ec)) return;

  const std::vector<SourceFile> payloadA = {{L"payload-a.bin", Pattern(1500, 5)}};
  const std::vector<SourceFile> payloadB = {{L"payload-b.bin", Pattern(2400, 6)}};
  if (!PRISM_CHECK(WriteSources(srcA, payloadA))) return;
  if (!PRISM_CHECK(WriteSources(srcB, payloadB))) return;

  // Fixtures are authored through C9's own plan/write path, NOT through the verb
  // under test -- otherwise a broken verb could author its own passing input.
  const auto makeArchive = [&root](const std::wstring& sourceFile,
                                   const std::wstring& archiveName) {
    const std::wstring target = Join(root, archiveName);
    const app::CompressionTarget plan =
        app::PlanCompression({sourceFile}, prism::ArchiveFormat::SevenZip, target);
    if (!plan.ok) return std::wstring();
    app::CompressionRequest job;
    job.archivePath = plan.archivePath;
    job.sourceRoot = plan.sourceRoot;
    job.plan = plan.plan;
    const app::CompressionOutcome outcome = app::RunCompression(job);
    return outcome.ok ? target : std::wstring();
  };
  const std::wstring packA = makeArchive(Join(srcA, L"payload-a.bin"), L"pack-a.7z");
  const std::wstring packB = makeArchive(Join(srcB, L"payload-b.bin"), L"pack-b.7z");
  if (!PRISM_CHECK(!packA.empty() && !packB.empty())) return;

  const app::ExtractEachOutcome outcome = app::RunExtractEach({packA, packB}, gLibrary);
  for (const std::string& line : outcome.lines) Emit("  stderr: " + line);
  PRISM_CHECK_EQ(outcome.exitCode, 0);
  PRISM_CHECK_EQ(outcome.archives, size_t(2));
  PRISM_CHECK_EQ(outcome.succeeded, size_t(2));
  PRISM_CHECK_EQ(outcome.failed, size_t(0));
  PRISM_CHECK_EQ(outcome.lines.size(), size_t(2));

  // Each archive's stem names its OWN folder, and the bytes are there.
  const std::wstring dirA = Join(root, L"pack-a");
  const std::wstring dirB = Join(root, L"pack-b");
  PRISM_CHECK(fs::is_directory(dirA, ec));
  PRISM_CHECK(fs::is_directory(dirB, ec));
  std::vector<uint8_t> gotA;
  std::vector<uint8_t> gotB;
  PRISM_CHECK(win::cw::ReadWholeFile(Join(dirA, L"payload-a.bin"), &gotA));
  PRISM_CHECK(win::cw::ReadWholeFile(Join(dirB, L"payload-b.bin"), &gotB));
  PRISM_CHECK(gotA == payloadA.front().bytes);
  PRISM_CHECK(gotB == payloadB.front().bytes);
  // Cross-talk guard: B's payload must not land in A's folder.
  PRISM_CHECK(!fs::exists(Join(dirA, L"payload-b.bin"), ec));
  PRISM_CHECK(!fs::exists(Join(dirB, L"payload-a.bin"), ec));
}

// ---------------------------------------------------------------------------
// (5) Failure path: exit 1, a reason on stderr, and nothing on disk.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.failed_input_exits_1_and_produces_no_file") {
  const std::wstring dir = FreshDir(L"fail");
  if (!PRISM_CHECK(!dir.empty())) return;

  for (const wchar_t* flag : {L"--compress-7z", L"--compress-zip"}) {
    const app::LaunchRequest request =
        app::ParseLaunch({flag, Join(dir, L"missing.bin")});
    const app::CompressVerbOutcome outcome = app::RunCompressVerb(request);
    for (const std::string& line : outcome.lines) Emit("  stderr: " + line);

    PRISM_CHECK_EQ(outcome.exitCode, 1);
    PRISM_CHECK(!outcome.ok);
    PRISM_CHECK_EQ(outcome.lines.size(), size_t(1));
    if (!outcome.lines.empty()) {
      const std::string& line = outcome.lines.front();
      PRISM_CHECK(line.find("compress failed") != std::string::npos);
      // "compress failed: " plus a real reason -- never a bare verdict.
      PRISM_CHECK(line.size() > std::strlen("compress failed: "));
    }
    // The contract: a verb that fails writes NOTHING (no empty archive, no
    // half a header). This is the assertion the CREATE_ON_FAILURE mutation
    // exists to break.
    const size_t files = CountFiles(dir);
    if (files != 0) Emit("  unexpected files: " + std::to_string(files));
    PRISM_CHECK_EQ(files, size_t(0));
  }
  // --smart-extract-each without an archive is refused the same way.
  const app::ExtractEachOutcome each = app::RunExtractEach({}, gLibrary);
  PRISM_CHECK_EQ(each.exitCode, 1);
  PRISM_CHECK_EQ(each.lines.size(), size_t(1));
}

// ---------------------------------------------------------------------------
// (6) The REAL command line: the shipped exe must answer the same verdicts.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.real_command_line_compresses_and_refuses") {
  if (!PRISM_CHECK(!gExe.empty())) return;

  // --compress-7z <two files> through a real process.
  const std::wstring okDir = FreshDir(L"cli-ok");
  if (!PRISM_CHECK(!okDir.empty())) return;
  const std::vector<SourceFile> files = {
      {L"cli-one.bin", Pattern(3072, 7)},
      {L"cli-two.bin", Pattern(211, 8)},
  };
  if (!PRISM_CHECK(WriteSources(okDir, files))) return;
  // The logs live OUTSIDE the directories under inspection: the "nothing was
  // written" assertion counts files, and its own capture file would be a false
  // positive (found the hard way on the first run).
  const std::wstring okLog = Join(gRoot, L"cli-ok.log");
  const BoundedRun okRun = RunExeBounded(
      gExe, {L"--compress-7z", Join(okDir, L"cli-one.bin"), Join(okDir, L"cli-two.bin")}, okDir,
      okLog, 20000);
  const std::string okText = okRun.text;
  Emit("  CLI --compress-7z exit=" + std::to_string(okRun.exitCode) +
       " timedOut=" + std::string(okRun.timedOut ? "1" : "0"));
  Emit("  CLI output: " + okText);
  PRISM_CHECK(!okRun.timedOut);
  PRISM_CHECK_EQ(okRun.exitCode, 0);
  PRISM_CHECK(okText.find("compress ok=1") != std::string::npos);
  const Reopened reopened = Reopen(Join(okDir, L"cli-one.7z"));
  PRISM_CHECK(Matches(reopened, files, "cli-7z"));

  // A missing input through the same binary: exit 1, reason on stderr, nothing
  // written. A message box here would hang a batch caller -- this proves there
  // is none (the process would not have exited within the timeout otherwise).
  const std::wstring failDir = FreshDir(L"cli-fail");
  if (!PRISM_CHECK(!failDir.empty())) return;
  const std::wstring failLog = Join(gRoot, L"cli-fail.log");
  const BoundedRun failRun = RunExeBounded(
      gExe, {L"--compress-7z", Join(failDir, L"missing.bin")}, failDir, failLog, 20000);
  const std::string failText = failRun.text;
  Emit("  CLI failure exit=" + std::to_string(failRun.exitCode) +
       " timedOut=" + std::string(failRun.timedOut ? "1" : "0"));
  Emit("  CLI output: " + failText);
  PRISM_CHECK(!failRun.timedOut);
  PRISM_CHECK_EQ(failRun.exitCode, 1);
  PRISM_CHECK(failText.find("compress failed") != std::string::npos);
  PRISM_CHECK_EQ(CountFiles(failDir), size_t(0));
}

// ---------------------------------------------------------------------------
// (7) The verb matrix, each verb as its own process, with a BOUNDED wait.
//
// A verb that opens a modal message box never exits: from Explorer or a script
// that is a hung caller, which is worse than an error code. This case runs every
// menu verb the exe accepts, plus an unknown flag, each as a separate process
// with a 15-second bound, and asserts:
//   * no run timed out (the anti-modal assertion);
//   * the exit code is the contract (0 success, 1 failure, 2 refused);
//   * the verbs that succeed really produced their files.
// --extract-here and --test are the KNOWN-GOOD controls: if this harness could
// not tell a working verb from a hung one, those two cases would expose it.
// ---------------------------------------------------------------------------
PRISM_TEST("compress_verbs.every_verb_returns_without_a_modal_box") {
  if (!PRISM_CHECK(!gExe.empty())) return;
  const std::wstring root = FreshDir(L"verbs");
  if (!PRISM_CHECK(!root.empty())) return;
  const std::wstring files = Join(root, L"files");
  const std::wstring arch = Join(root, L"arch");
  const std::wstring here = Join(root, L"here");
  const std::wstring tested = Join(root, L"tested");
  std::error_code ec;
  for (const std::wstring* dir : {&files, &arch, &here, &tested}) {
    fs::create_directories(*dir, ec);
  }
  if (!PRISM_CHECK(!ec)) return;

  // Fixtures are authored through C9's path, never through the verbs under test.
  const auto author = [](const std::wstring& dir, const wchar_t* payloadName,
                         const std::vector<uint8_t>& bytes,
                         const wchar_t* archiveName) -> std::wstring {
    if (!WriteBytes(Join(dir, payloadName), bytes)) return {};
    const std::wstring target = Join(dir, archiveName);
    const app::CompressionTarget plan = app::PlanCompression(
        {Join(dir, payloadName)}, prism::ArchiveFormat::SevenZip, target);
    if (!plan.ok) return {};
    app::CompressionRequest job;
    job.archivePath = plan.archivePath;
    job.sourceRoot = plan.sourceRoot;
    job.plan = plan.plan;
    return app::RunCompression(job).ok ? target : std::wstring();
  };

  const std::wstring packA = author(arch, L"payload-a.bin", Pattern(700, 11), L"pack-a.7z");
  const std::wstring packB = author(arch, L"payload-b.bin", Pattern(900, 12), L"pack-b.7z");
  const std::wstring packHere =
      author(here, L"payload-here.bin", Pattern(500, 13), L"pack-here.7z");
  const std::wstring packTest =
      author(tested, L"payload-test.bin", Pattern(300, 14), L"pack-test.7z");
  if (!PRISM_CHECK(!packA.empty() && !packB.empty() && !packHere.empty() && !packTest.empty())) {
    return;
  }

  const std::wstring aFile = Join(files, L"report-a.txt");
  const std::wstring bFile = Join(files, L"report-b.bin");
  if (!PRISM_CHECK(WriteBytes(aFile, Pattern(100, 15)) &&
                   WriteBytes(bFile, Pattern(2048, 16)))) {
    return;
  }

  const std::wstring log = Join(root, L"verb.log");
  const auto runVerb = [&](const wchar_t* label, const std::vector<std::wstring>& args,
                           const std::wstring& workDir, int expected) {
    const BoundedRun run = RunExeBounded(gExe, args, workDir, log, 15000);
    Emit("  [" + win::WideToUtf8(label) + "] exit=" + std::to_string(run.exitCode) +
         " timedOut=" + std::string(run.timedOut ? "1" : "0"));
    Emit("    " + run.text);
    PRISM_CHECK(!run.timedOut);  // a modal box would still be alive here
    PRISM_CHECK_EQ(run.exitCode, expected);
    return run;
  };

  runVerb(L"--compress-7z", {L"--compress-7z", aFile, bFile}, files, 0);
  PRISM_CHECK(fs::exists(Join(files, L"report-a.7z"), ec));

  runVerb(L"--compress-zip", {L"--compress-zip", aFile, bFile}, files, 0);
  PRISM_CHECK(fs::exists(Join(files, L"report-a.zip"), ec));

  // Each folder is named after its archive and lives BESIDE it (the archive's
  // own parent), which is the --extract-sub contract.
  runVerb(L"--smart-extract-each", {L"--smart-extract-each", packA, packB}, root, 0);
  PRISM_CHECK(fs::exists(Join(Join(arch, L"pack-a"), L"payload-a.bin"), ec));
  PRISM_CHECK(fs::exists(Join(Join(arch, L"pack-b"), L"payload-b.bin"), ec));

  // Known-good control 1: --extract-here writes the payload and exits 0.
  runVerb(L"--extract-here", {L"--extract-here", packHere}, here, 0);
  PRISM_CHECK(fs::exists(Join(here, L"payload-here.bin"), ec));

  // Known-good control 2: --test decodes and writes NOTHING.
  const size_t beforeTest = CountFiles(tested);
  const BoundedRun verify = runVerb(L"--test", {L"--test", packTest}, tested, 0);
  PRISM_CHECK(verify.text.find("verify ok=1") != std::string::npos);
  PRISM_CHECK_EQ(CountFiles(tested), beforeTest);

  // An unknown flag: exit 2, a reason on stderr, and -- the point of this case
  // -- the process RETURNS. It used to raise a MessageBox and hang.
  const size_t beforeUnknown = CountFiles(arch);
  const BoundedRun refused =
      runVerb(L"unknown flag", {L"--extract-wherever", packA}, arch, 2);
  PRISM_CHECK(refused.text.find("unsupported flag") != std::string::npos);
  PRISM_CHECK_EQ(CountFiles(arch), beforeUnknown);
}

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  if (kZipAs7zMutant) {
    Emit("C58 MUTANT ACTIVE: PRISM_C58_MUTANT_ZIP_AS_7Z -- every creation verb writes 7z");
  }
  if (kCreateOnFailureMutant) {
    Emit("C58 MUTANT ACTIVE: PRISM_C58_MUTANT_CREATE_ON_FAILURE -- a failed run leaves an "
         "empty output file");
  }
  if (kZipAs7zMutant || kCreateOnFailureMutant) {
    // CONTRACTS D27: arm before the first assertion; the first real failure
    // prints the marker and leaves with 0 on the spot. CONTRACTS D3: the
    // predicate only lets a failure claim CAUGHT when the injection actually
    // fired (counter moved), so an unrelated red stays red.
    prism::test::SetMutationVerdict(
        kZipAs7zMutant ? "C58 MUTATION CAUGHT: zip_as_7z"
                       : "C58 MUTATION CAUGHT: create_on_failure",
        [] {
          return kZipAs7zMutant
                     ? app::CompressVerbMutations().zipForcedTo7z.load(std::memory_order_relaxed) > 0
                     : app::CompressVerbMutations().emptyFileOnFailure.load(
                           std::memory_order_relaxed) > 0;
        });
  }

  // CMake hands the built exe over through PRISM_C58_EXE (see
  // Tests/suites/c58-compressverbs.cmake: tools/preflight.py's D1 gate wants
  // "COMMAND <exe>" to be the whole command). argv[1] stays accepted for a
  // hand-run of the binary.
  if (argc > 1 && argv[1] != nullptr) gExe = win::Utf8ToWide(argv[1]);
  if (gExe.empty()) {
    wchar_t fromEnv[2 * MAX_PATH]{};
    const DWORD written = GetEnvironmentVariableW(L"PRISM_C58_EXE", fromEnv,
                                                  ARRAYSIZE(fromEnv));
    if (written > 0 && written < ARRAYSIZE(fromEnv)) gExe.assign(fromEnv, written);
  }
  if (gExe.empty()) {
    Emit("COMPRESS-VERBS SKIPPED: no PrismArchive.exe path (set PRISM_C58_EXE or pass "
         "argv[1]); the real command-line contract cannot be proven without it");
    return 77;
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    Emit("COMPRESS-VERBS SKIPPED: CoInitializeEx failed");
    return 77;
  }
  const std::wstring dll = [] {
    wchar_t value[MAX_PATH]{};
    const DWORD n = GetEnvironmentVariableW(L"PRISM_7Z_DLL", value, ARRAYSIZE(value));
    return n > 0 && n < MAX_PATH ? std::wstring(value, n) : std::wstring();
  }();
  if (dll.empty() || !gLibrary.Load(dll)) {
    Emit("COMPRESS-VERBS SKIPPED: PRISM_7Z_DLL is not set or 7z.dll cannot be loaded");
    CoUninitialize();
    return 77;
  }
  static prism::testtmp::UniqueDir scratch(L"c58", L"compress_verbs");
  if (!scratch.ok()) {
    Emit("COMPRESS-VERBS FAILED: cannot create the fixture directory: " + scratch.error());
    CoUninitialize();
    return 1;
  }
  gRoot = scratch.wstr();
  Emit("FIXTURE root=" + win::WideToUtf8(gRoot) + " dll=" + win::WideToUtf8(dll) +
       " exe=" + win::WideToUtf8(gExe));

  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();

  if (kZipAs7zMutant || kCreateOnFailureMutant) {
    if (rc == 0) {
      Emit("C58 MUTATION NOT CAUGHT: the suite stayed green with the regression compiled in");
      return 1;
    }
    Emit(std::string("C58 MUTATION CAUGHT: ") +
         (kZipAs7zMutant ? "zip_as_7z" : "create_on_failure"));
    return 0;
  }
  if (rc != 0) return rc;
  Emit("ALL COMPRESS-VERBS CHECKS PASSED");
  return 0;
}
