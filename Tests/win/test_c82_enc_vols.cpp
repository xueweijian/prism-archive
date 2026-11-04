// PrismArchive -- card C82: CLI verbs must not hang on encrypted archives, and
// numeric-split 7z volumes (.7z.001/.002/.003) must open.
//
// WHY THIS FILE EXISTS
// --------------------
// (1) P1-1: "--test enc.7z" with no password brought up the modal password
//     gate and the process hung (>20 s measured by the desktop-test lane). The
//     rule C58/C71 established -- a headless verb NEVER opens a modal -- now
//     covers the password: the verb prints one stderr line and exits 2.
//     --password / --password-file feed the verb's open instead.
// (2) P2-2: "vols.7z.001" was handed to the 7z handler as a lone file; the
//     handler failed addressing past the first volume and the open died as
//     "declined=7z reason=handler-rejected" while real 7-Zip accepted the same
//     set. The bridge now presents the SET as one seekable stream, and a set
//     with a missing volume refuses BY NAME (vols.7z.003), never silently.
// (3) the C40 GUI contract must not regress: opening enc.7z in the gallery
//     still brings the password dialog up.
//
// FIXTURES (Tests/fixtures/c82, committed binaries cross-checked with real
// 7-Zip): enc.7z is header-encrypted (password PrismTest2026); vols.7z.001/
// .002/.003 are a byte-split of one 7z holding "vols-payload.bin", 25000 bytes
// of a deterministic LCG stream -- x = (x*1103515245+12345) mod 2^31,
// payload[i] = (x >> 16) & 0xFF. This source regenerates those exact bytes.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26/D27), compiled into this same source:
//   * PRISM_C82_MUTANT_CLI_MODAL -- the mutant build sets the same-named env
//     var, and the exe restores the pre-C82 shape (a CLI verb is treated like
//     the gallery): the modal gate comes up and the process HANGS. The bounded
//     spawn must kill it, and the case must go red.
//   * PRISM_C82_MUTANT_NO_VOLUME_CONCAT -- the bridge skips the concat (the
//     .001 goes to the handler as a lone file). The 25000-byte round trip MUST
//     go red.
// No WILL_FAIL (D26): each mutant build arms mini_test's verdict BEFORE the
// first assertion (D27); the first real failure prints
// "C82 MUTATION CAUGHT: <name>" and leaves with 0, and that marker is the CTest
// PASS_REGULAR_EXPRESSION. A mutation that stopped applying stays red.
//
// D1: no 7z.dll or no PrismArchive.exe -> print WHY and exit 77, never a fake green.
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "UniqueTempDir.hpp"

#ifndef PRISM_C82_MUTANT_CLI_MODAL
#define PRISM_C82_MUTANT_CLI_MODAL 0
#endif
#ifndef PRISM_C82_MUTANT_NO_VOLUME_CONCAT
#define PRISM_C82_MUTANT_NO_VOLUME_CONCAT 0
#endif

namespace {

constexpr wchar_t kPasswordDialogClass[] = L"PrismArchivePasswordDialog";

std::wstring gExe;       // PrismArchive.exe, handed over by CMake through env
std::wstring gFixtures;  // Tests/fixtures/c82
std::wstring gRoot;      // scratch dir for this run
prism::win::SevenZipLibrary gLibrary;

std::atomic<bool> gCliModalHangObserved{false};

void Emit(const std::string& line) { std::cout << line << "\n"; }

std::wstring Join(const std::wstring& dir, const std::wstring& name) {
  std::wstring out = dir;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
  out += name;
  return out;
}

// The EXACT payload the volume fixture was built from (see the header comment):
// the comparison is byte-for-byte, not "about the right size".
std::vector<uint8_t> ExpectedPayload() {
  std::vector<uint8_t> out;
  out.reserve(25000);
  uint32_t x = 1;
  for (int i = 0; i < 25000; ++i) {
    x = static_cast<uint32_t>((static_cast<uint64_t>(x) * 1103515245ull + 12345ull) &
                              0x7FFFFFFFull);
    out.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
  }
  return out;
}

bool WriteBytes(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok =
      bytes.empty()
          ? TRUE
          : WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
  CloseHandle(file);
  return ok != FALSE && written == bytes.size();
}

std::vector<uint8_t> ReadBytes(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  std::vector<uint8_t> out;
  if (file == INVALID_HANDLE_VALUE) return out;
  uint8_t buffer[8192];
  DWORD got = 0;
  while (ReadFile(file, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
    out.insert(out.end(), buffer, buffer + got);
  }
  CloseHandle(file);
  return out;
}

bool CopyFixture(const std::wstring& name, const std::wstring& intoDir) {
  return CopyFileW(Join(gFixtures, name).c_str(), Join(intoDir, name).c_str(), FALSE) != 0;
}

bool MakeDir(const std::wstring& path) {
  if (CreateDirectoryW(path.c_str(), nullptr) != 0) return true;
  return GetLastError() == ERROR_ALREADY_EXISTS;
}

// One real command line, with a BOUNDED wait (the C58 pattern). The bound is
// the point: the pre-C82 verb opened a modal password box and NEVER exited, so
// "still alive at the bound" is the observable defect, recorded as timedOut.
struct BoundedRun {
  int exitCode = -1;  // -1 = could not start, -2 = killed on timeout
  bool timedOut = false;
  std::string text;  // stdout+stderr, as redirected
};

BoundedRun RunExeBounded(const std::vector<std::wstring>& args, const std::wstring& workDir,
                         const std::wstring& logPath, DWORD timeoutMs) {
  BoundedRun out;
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;
  HANDLE log = CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
  if (log == INVALID_HANDLE_VALUE) return out;
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

  std::wstring command = L"\"" + gExe + L"\"";
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
    // Still alive at the bound = a hang (a modal box is the usual cause). Kill
    // it so the suite can finish and report the verdict.
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
  HANDLE readBack = CreateFileW(logPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (readBack != INVALID_HANDLE_VALUE) {
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readBack, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
      out.text.append(buffer, got);
    }
    CloseHandle(readBack);
  }
  return out;
}

// Opens the archive in-process, lists the catalog, decodes the first non-dir
// entry. Every failure keeps its evidence: the open trace and the reader's
// explicit verdict (which is what the missing-volume assertions read).
struct CatalogProbe {
  bool opened = false;
  std::string trace;
  std::string failure;
  size_t entries = 0;
  uint64_t entryBytes = 0;
  std::vector<uint8_t> payload;
};

CatalogProbe ProbeFirstEntry(const std::wstring& archive, const std::wstring& password = {}) {
  CatalogProbe out;
  prism::win::ArchiveReader reader(gLibrary);
  if (!reader.Open(archive, password)) {
    out.trace = reader.openTrace();
    out.failure = reader.failureReason();
    return out;
  }
  out.opened = true;
  const std::vector<prism::ArchiveItem> catalog = reader.ReadCatalog();
  out.entries = catalog.size();
  for (const prism::ArchiveItem& item : catalog) {
    if (item.isDir) continue;
    out.entryBytes = item.size;
    const std::optional<std::vector<uint8_t>> bytes = reader.ExtractToMemory(item.index);
    if (bytes.has_value()) out.payload = *bytes;
    break;
  }
  reader.Close();
  return out;
}

// Polls for the password dialog for up to timeoutMs: TRUE when THIS spawn's
// gate brought its window up. The pid match is not decoration: a stale dialog
// left behind by an earlier crashed run would otherwise satisfy the check.
bool WaitForPasswordDialogOf(DWORD pid, DWORD timeoutMs) {
  const DWORD deadline = GetTickCount() + timeoutMs;
  while (GetTickCount() < deadline) {
    const HWND window = FindWindowW(kPasswordDialogClass, nullptr);
    if (window != nullptr) {
      DWORD owner = 0;
      GetWindowThreadProcessId(window, &owner);
      if (owner == pid) return true;
    }
    Sleep(150);
  }
  return false;
}

// Ground truth for a red run: EVERY top-level window the spawned process owns,
// by class name. A missing dialog must say what IS there instead.
std::vector<std::string> WindowClassesOf(DWORD pid) {
  struct Ctx {
    DWORD pid;
    std::vector<std::string>* out;
  } ctx{pid, nullptr};
  std::vector<std::string> classes;
  ctx.out = &classes;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* self = reinterpret_cast<Ctx*>(lp);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == self->pid) {
          wchar_t cls[128] = {};
          GetClassNameW(hwnd, cls, 128);
          self->out->push_back(prism::win::WideToUtf8(cls));
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return classes;
}

}  // namespace

// ---------------------------------------------------------------------------
// P1-1: the CLI verb meets an encrypted archive with NO password.
// The process must be GONE well inside the 15 s bound, exit 2, one stderr line.
// ---------------------------------------------------------------------------
PRISM_TEST("c82_cli_encrypted_no_password") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  PRISM_CHECK(CopyFixture(L"enc.7z", gRoot));
  const std::wstring log = Join(gRoot, L"enc-nopw.log");
  const BoundedRun run = RunExeBounded({L"--test", enc}, gRoot, log, 15000);
  if (run.timedOut) gCliModalHangObserved.store(true);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 2);
  PRISM_CHECK(run.text.find("encrypted") != std::string::npos);
  PRISM_CHECK(run.text.find("password required") != std::string::npos);
}

// --password feeds the verb: verify succeeds, exit 0.
PRISM_TEST("c82_cli_encrypted_with_password_flag") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  const std::wstring log = Join(gRoot, L"enc-pw.log");
  const BoundedRun run =
      RunExeBounded({L"--test", enc, L"--password", L"PrismTest2026"}, gRoot, log, 20000);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 0);
  PRISM_CHECK(run.text.find("verify ok=1") != std::string::npos);
}

// --password-file feeds the verb: one UTF-8 line (BOM/newlines stripped).
PRISM_TEST("c82_cli_encrypted_with_password_file") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  const std::wstring pwFile = Join(gRoot, L"secret.txt");
  const std::vector<uint8_t> secret = {'P', 'r', 'i', 's', 'm', 'T', 'e', 's', 't',
                                       '2', '0', '2', '6', '\r', '\n'};
  PRISM_CHECK(WriteBytes(pwFile, secret));
  const std::wstring log = Join(gRoot, L"enc-pwfile.log");
  const BoundedRun run =
      RunExeBounded({L"--test", enc, L"--password-file", pwFile}, gRoot, log, 20000);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 0);
  PRISM_CHECK(run.text.find("verify ok=1") != std::string::npos);

  // An unreadable password file is a caller bug: one stderr line, exit 2.
  const std::wstring log2 = Join(gRoot, L"enc-pwfile-missing.log");
  const BoundedRun bad =
      RunExeBounded({L"--test", enc, L"--password-file", Join(gRoot, L"no-such-secret.txt")},
                    gRoot, log2, 20000);
  PRISM_CHECK(!bad.timedOut);
  PRISM_CHECK_EQ(bad.exitCode, 2);
  PRISM_CHECK(bad.text.find("password-file") != std::string::npos);
}

// smart-extract-each hits the encrypted member: refused per line, exit 2,
// NO hang and NO dialog.
PRISM_TEST("c82_cli_extract_each_encrypted_refused") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  const std::wstring log = Join(gRoot, L"enc-each.log");
  const BoundedRun run = RunExeBounded({L"--smart-extract-each", enc}, gRoot, log, 15000);
  if (run.timedOut) gCliModalHangObserved.store(true);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 2);
  PRISM_CHECK(run.text.find("encrypted: password required") != std::string::npos);
}

// ---------------------------------------------------------------------------
// C40 no-regression: the GALLERY still opens the password dialog for enc.7z.
// The gate runs before the main window exists, so the dialog is findable by
// class name; the process must still be alive while the dialog is up (the
// gate is modal), and the suite kills the process afterwards -- a test never
// leaves a window behind.
// ---------------------------------------------------------------------------
PRISM_TEST("c82_gui_password_dialog_still_opens") {
  const std::wstring enc = Join(gRoot, L"enc.7z");
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  std::wstring command = L"\"" + gExe + L"\" \"" + enc + L"\"";
  PRISM_CHECK(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                             nullptr, &si, &pi) != 0);
  const bool dialog = WaitForPasswordDialogOf(pi.dwProcessId, 10000);
  if (!dialog) {
    for (const std::string& cls : WindowClassesOf(pi.dwProcessId)) {
      Emit("c82 gui window present: class=" + cls);
    }
    TerminateProcess(pi.hProcess, 0xDEAD);
    WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  }
  PRISM_CHECK(dialog);
  if (!dialog) return;
  DWORD code = 0;
  GetExitCodeProcess(pi.hProcess, &code);
  PRISM_CHECK_EQ(code, static_cast<DWORD>(STILL_ACTIVE));
  TerminateProcess(pi.hProcess, 0xDEAD);
  WaitForSingleObject(pi.hProcess, 5000);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
}

// ---------------------------------------------------------------------------
// P2-2: the three-volume set opens through the concatenated stream.
// ---------------------------------------------------------------------------
PRISM_TEST("c82_volumes_concat_open_and_bytes") {
  const std::wstring setDir = Join(gRoot, L"volset");
  PRISM_CHECK(MakeDir(setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.001", setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.002", setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.003", setDir));

  const CatalogProbe probe = ProbeFirstEntry(Join(setDir, L"vols.7z.001"));
  PRISM_CHECK(probe.opened);
  if (!probe.opened) {
    Emit("c82 concat open failed: " + probe.trace);
    return;
  }
  PRISM_CHECK_EQ(probe.entries, static_cast<size_t>(1));
  PRISM_CHECK_EQ(probe.entryBytes, static_cast<uint64_t>(25000));
  const std::vector<uint8_t> expected = ExpectedPayload();
  PRISM_CHECK_EQ(probe.payload.size(), expected.size());
  bool same = probe.payload.size() == expected.size();
  for (size_t i = 0; same && i < expected.size(); ++i) same = probe.payload[i] == expected[i];
  PRISM_CHECK(same);
  PRISM_CHECK(prism::win::IoCounters().volumeConcatOpens.load(std::memory_order_relaxed) > 0);
}

// The SAME set through the REAL command line: --smart-extract-each must unpack
// 25000 bytes to disk, byte-correct.
PRISM_TEST("c82_volumes_cli_extract_bytes") {
  const std::wstring setDir = Join(gRoot, L"volset-cli");
  PRISM_CHECK(MakeDir(setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.001", setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.002", setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.003", setDir));

  const std::wstring log = Join(gRoot, L"vols-cli.log");
  const BoundedRun run =
      RunExeBounded({L"--smart-extract-each", Join(setDir, L"vols.7z.001")}, gRoot, log, 20000);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 0);
  PRISM_CHECK(run.text.find("ok=1") != std::string::npos);
  PRISM_CHECK(run.text.find("bytes=25000") != std::string::npos);

  // The extracted payload on disk, byte for byte.
  const std::vector<uint8_t> expected = ExpectedPayload();
  bool found = false;
  bool same = true;
  std::error_code walkEc;
  for (std::filesystem::recursive_directory_iterator it(setDir,
           std::filesystem::directory_options::skip_permission_denied, walkEc),
       end;
       !walkEc && it != end; it.increment(walkEc)) {
    if (it->path().filename().wstring() != L"vols-payload.bin") continue;
    found = true;
    const std::vector<uint8_t> onDisk = ReadBytes(it->path().wstring());
    if (onDisk.size() != expected.size()) {
      same = false;
      break;
    }
    for (size_t i = 0; i < expected.size(); ++i) {
      if (onDisk[i] != expected[i]) {
        same = false;
        break;
      }
    }
    break;
  }
  PRISM_CHECK(found);
  PRISM_CHECK(same);
}

// Missing tail volume: the open must REFUSE, naming vols.7z.003 -- the 7z
// start header knows the set's full size, so a short set is never handed to
// the handler as if it were whole.
PRISM_TEST("c82_volumes_missing_tail_named") {
  const std::wstring setDir = Join(gRoot, L"volset-missing");
  PRISM_CHECK(MakeDir(setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.001", setDir));
  PRISM_CHECK(CopyFixture(L"vols.7z.002", setDir));
  // .003 deliberately NOT copied.

  const CatalogProbe probe = ProbeFirstEntry(Join(setDir, L"vols.7z.001"));
  PRISM_CHECK(!probe.opened);
  PRISM_CHECK(probe.failure.find("vols.7z.003") != std::string::npos);
  PRISM_CHECK(probe.trace.find("reason=missing-volume") != std::string::npos);

  // The same refusal through the real command line, on stderr.
  const std::wstring log = Join(gRoot, L"vols-missing.log");
  const BoundedRun run =
      RunExeBounded({L"--smart-extract-each", Join(setDir, L"vols.7z.001")}, gRoot, log, 20000);
  PRISM_CHECK(!run.timedOut);
  PRISM_CHECK_EQ(run.exitCode, 1);
  PRISM_CHECK(run.text.find("vols.7z.003") != std::string::npos);
}

int main(int argc, char** argv) {
  std::cout << std::unitbuf;
  if (PRISM_C82_MUTANT_CLI_MODAL) {
    Emit("C82 MUTANT ACTIVE: PRISM_C82_MUTANT_CLI_MODAL -- CLI verbs use the modal gate");
    SetEnvironmentVariableW(L"PRISM_C82_MUTANT_CLI_MODAL", L"1");
    // CONTRACTS D27: arm BEFORE the first assertion. The predicate demands the
    // injected hang (the bounded spawn had to kill the process) -- a red for
    // any other reason stays red (D3).
    prism::test::SetMutationVerdict("C82 MUTATION CAUGHT: cli_modal",
                                    [] { return gCliModalHangObserved.load(); });
  }
  if (PRISM_C82_MUTANT_NO_VOLUME_CONCAT) {
    Emit("C82 MUTANT ACTIVE: PRISM_C82_MUTANT_NO_VOLUME_CONCAT -- the .001 reaches the "
         "handler as a lone file");
    SetEnvironmentVariableW(L"PRISM_C82_MUTANT_NO_VOLUME_CONCAT", L"1");
    prism::test::SetMutationVerdict("C82 MUTATION CAUGHT: no_volume_concat", [] {
      return prism::win::IoCounters().mutantVolumeConcatOff.load(std::memory_order_relaxed) > 0;
    });
  }

  wchar_t fromEnv[2 * MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_C82_EXE", fromEnv, ARRAYSIZE(fromEnv)) > 0) {
    gExe.assign(fromEnv);
  }
  if (gExe.empty()) {
    Emit("C82 SKIPPED: no PrismArchive.exe path (set PRISM_C82_EXE); the command-line "
         "contract cannot be proven without it");
    return 77;
  }
  wchar_t fixturesEnv[2 * MAX_PATH]{};
  if (GetEnvironmentVariableW(L"PRISM_C82_FIXTURES", fixturesEnv, ARRAYSIZE(fixturesEnv)) > 0) {
    gFixtures.assign(fixturesEnv);
  }
  if (gFixtures.empty()) {
    Emit("C82 SKIPPED: no fixture directory (set PRISM_C82_FIXTURES)");
    return 77;
  }
  wchar_t dllEnv[MAX_PATH]{};
  if (!(GetEnvironmentVariableW(L"PRISM_7Z_DLL", dllEnv, ARRAYSIZE(dllEnv)) > 0) ||
      !gLibrary.Load(std::wstring(dllEnv))) {
    Emit("C82 SKIPPED: PRISM_7Z_DLL is not set or 7z.dll cannot be loaded");
    return 77;
  }
  // RAII for the whole run: the destructor removes the tree even when a case
  // bails out early -- a test never leaves fixture debris in %TEMP%.
  prism::testtmp::UniqueDir scratch(L"c82", L"enc_vols");
  if (!scratch.ok()) {
    Emit("C82 FAILED: cannot create the fixture directory: " + scratch.error());
    return 1;
  }
  gRoot = scratch.wstr();
  Emit("FIXTURE root=" + prism::win::WideToUtf8(gRoot) + " dll=" +
       prism::win::WideToUtf8(dllEnv) + " exe=" + prism::win::WideToUtf8(gExe));

  const int rc = prism::test::RunAll(argc, argv);

  if (PRISM_C82_MUTANT_CLI_MODAL || PRISM_C82_MUTANT_NO_VOLUME_CONCAT) {
    if (rc == 0) {
      Emit("C82 MUTATION NOT CAUGHT: the suite stayed green with the regression in");
      return 1;
    }
    return 0;  // the verdict was printed at the first failure (D27)
  }
  if (rc != 0) return rc;
  Emit("ALL C82 CHECKS PASSED");
  return 0;
}
