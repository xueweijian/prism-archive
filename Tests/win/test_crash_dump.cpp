// PrismArchive -- Windows crash-dump suite (card C16)
//
// What this proves, and why it needs a SECOND process:
//   1. SetUnhandledExceptionFilter really is installed by crash::Install(),
//   2. MiniDumpWriteDump really wrote an MDMP (signature + size, not "a file
//      exists" -- CONTRACTS D2),
//   3. the report next to it carries exactly the LAST 64 log records (the ring
//      held 72, so 8 must be gone -- a "tail" that silently keeps everything
//      would pass a weaker assertion),
//   4. CONTRACTS B7: neither the dump's text report nor its log tail contains the
//      credential that was in the ring when the process died.
//
// The crash MUST NOT happen in this process: an exception here would take the
// suite down and CTest would report a crash instead of a verdict. So the suite
// re-launches ITSELF with --crash-child (directory passed by environment
// variable, so no command-line encoding games), waits with a timeout, and then
// inspects the artifacts the child left behind. A second, in-process call to
// crash::WriteCrashReport with a fabricated EXCEPTION_POINTERS covers the writer
// quickly; the child covers the filter wiring.
//
// Negative control (CONTRACTS D3, registered WILL_FAIL in
// Tests/suites/c16-logging.cmake): the same file built with
// PRISM_C16_MUTANT_NO_REDACTION. The credential then appears in the report, the
// redaction assertions go red, and the mutant registration is what turns CI red
// if that ever stops being caught.
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

#include "../mini_test.hpp"
#include "CrashDump.hpp"
#include "Log.hpp"

namespace crash = prism::win::crash;
namespace plog = prism::log;

namespace {

// Application-defined exception code (0xE0000000-0xEFFFFFFF is ours to use), so
// the crash can never be confused with a real access violation. An access
// violation would also be intercepted by the ASan runtime this project's Windows
// lane builds with, which is exactly the sort of interference that makes a test
// prove something other than what it claims.
constexpr DWORD kCrashCode = 0xE000C016;
constexpr size_t kRingCapacity = 72;
constexpr size_t kTail = 64;
constexpr const char kSecret[] = "hunter2-c16-secret";

std::wstring SelfPath() {
  wchar_t buffer[32768];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
  return std::wstring(buffer, length);
}

std::filesystem::path TempRoot() {
  wchar_t temp[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  std::filesystem::path root =
      std::filesystem::path(std::wstring(temp, length)) /
      (L"prism_c16_crash_" + std::to_wstring(static_cast<unsigned long>(GetCurrentProcessId())));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::filesystem::create_directories(root, ec);
  return root;
}

std::string ReadBytes(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::string();
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

bool EndsWith(const std::wstring& text, const std::wstring& suffix) {
  return text.size() >= suffix.size() &&
         text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool HasMiniDumpSignature(const std::filesystem::path& path) {
  const std::string bytes = ReadBytes(path);
  // "MDMP" is the first field of MINIDUMP_HEADER. A zero-length or truncated
  // file cannot pass this.
  return bytes.size() > 4096 && bytes.compare(0, 4, "MDMP") == 0;
}

std::vector<std::filesystem::path> FilesMatching(const std::filesystem::path& directory,
                                                 const std::wstring& extension) {
  std::vector<std::filesystem::path> found;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec)) continue;
    if (entry.path().extension() == extension) found.push_back(entry.path());
  }
  return found;
}

// Fills a ring with 72 numbered records plus one credential-bearing record, so
// "the last 64" is falsifiable in both directions (008 must be gone, 009 and 071
// must be there).
void FillRing(plog::Logger& logger) {
  for (int index = 0; index < 72; ++index) {
    char label[8];
    std::snprintf(label, sizeof(label), "%03d", index);
    logger.Emit(plog::Level::kInfo, "sweep", std::string("ring line ") + label,
                {{"index", label}}, index);
  }
  logger.Emit(plog::Level::kError, "vault",
              std::string("unlock failed password=") + kSecret, {{"password", kSecret}}, 72);
}

// ---- child process: install the filter, then really die ---------------
int CrashChildMain() {
  // No modal box, no "file not found" prompt: a crashing process must not ask
  // the (possibly absent) user a question.
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);

  wchar_t directory[32768] = {};
  const DWORD length = GetEnvironmentVariableW(L"PRISM_C16_CRASH_DIR", directory, 32768);
  if (length == 0 || length >= 32768) {
    std::cout << "child: PRISM_C16_CRASH_DIR missing" << std::endl;
    return 90;
  }

  plog::Logger logger(plog::Level::kInfo, kRingCapacity);
  FillRing(logger);

  crash::Options options;
  options.directory = directory;
  options.ring = &logger.ring();
  options.fileNameStamp = L"20261101-092631";
  options.sequence = 1;
  if (!crash::Install(options)) {
    std::cout << "child: SetUnhandledExceptionFilter was refused" << std::endl;
    return 90;
  }
  if (!crash::IsInstalled()) {
    std::cout << "child: Install() reported success but the handler is not armed" << std::endl;
    return 90;
  }
  std::cout << "child: filter armed, raising 0xE000C016" << std::endl;
  std::cout.flush();
  RaiseException(kCrashCode, EXCEPTION_NONCONTINUABLE, 0, nullptr);
  // Reaching this line means the filter did NOT take over: that is a failure of
  // the wiring, and the parent reports it as one.
  std::cout << "child: RaiseException RETURNED -- the unhandled filter never ran" << std::endl;
  return 91;
}

}  // namespace

// ---- pure path/naming surface ----------------------------------------
PRISM_TEST("crash.default_directory_is_under_localappdata") {
  const std::wstring directory = crash::DefaultCrashDirectory();
  PRISM_CHECK(!directory.empty());
  PRISM_CHECK(EndsWith(directory, std::wstring(L"PrismArchive\\crash")));
  wchar_t localAppData[32768] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, 32768);
  if (length > 0 && length < 32768) {
    PRISM_CHECK(directory.rfind(std::wstring(localAppData), 0) == 0);
  }
  // The naming surface is pure: same inputs -> same paths, and a trailing
  // separator must not produce a doubled one.
  const std::wstring stamp = L"20261101-092631";
  const crash::Paths plain = crash::MakePaths(L"C:\\tmp\\crash", stamp, 4242, 0);
  const crash::Paths trailing = crash::MakePaths(L"C:\\tmp\\crash\\", stamp, 4242, 0);
  PRISM_CHECK(plain.dumpPath == trailing.dumpPath);
  PRISM_CHECK(plain.logPath == trailing.logPath);
  PRISM_CHECK(EndsWith(plain.dumpPath, L"crash-20261101-092631-4242.dmp"));
  PRISM_CHECK(EndsWith(plain.logPath, L"crash-20261101-092631-4242.txt"));
  PRISM_CHECK(plain.dumpPath.find(L"\\\\") == std::wstring::npos);
  // A second crash in the same second must not overwrite the first.
  const crash::Paths second = crash::MakePaths(L"C:\\tmp\\crash", stamp, 4242, 2);
  PRISM_CHECK(second.dumpPath != plain.dumpPath);
  PRISM_CHECK(EndsWith(second.dumpPath, L"crash-20261101-092631-4242-2.dmp"));

  SYSTEMTIME utc{};
  utc.wYear = 2026;
  utc.wMonth = 11;
  utc.wDay = 1;
  utc.wHour = 9;
  utc.wMinute = 26;
  utc.wSecond = 31;
  PRISM_CHECK(crash::detail::FormatFileStamp(utc) == std::wstring(L"20261101-092631"));
}

// ---- install / uninstall ---------------------------------------------
PRISM_TEST("crash.install_and_uninstall_are_idempotent") {
  crash::Options options;
  options.directory = L"";
  options.ring = nullptr;
  PRISM_CHECK(crash::Install(options));
  PRISM_CHECK(crash::IsInstalled());
  PRISM_CHECK(crash::Install(options));  // re-arming is not an error
  PRISM_CHECK(crash::IsInstalled());
  crash::Uninstall();
  PRISM_CHECK(!crash::IsInstalled());
}

// ---- the writer, in-process (no crash involved) -----------------------
PRISM_TEST("crash.writer_emits_a_real_minidump_and_a_redacted_tail") {
  const std::filesystem::path root = TempRoot();
  const std::filesystem::path directory = root / "inproc";
  plog::Logger logger(plog::Level::kInfo, kRingCapacity);
  FillRing(logger);

  CONTEXT context{};
  RtlCaptureContext(&context);
  EXCEPTION_RECORD record{};
  record.ExceptionCode = kCrashCode;
  record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
  record.ExceptionAddress = reinterpret_cast<PVOID>(&context);
  EXCEPTION_POINTERS pointers{&record, &context};

  crash::Options options;
  options.directory = directory.wstring();
  options.ring = &logger.ring();
  options.fileNameStamp = L"inproc";
  const crash::Outcome outcome = crash::WriteCrashReport(&pointers, options);
  std::cout << "    in-process: dump=" << (outcome.dumpWritten ? "ok" : "failed")
            << " win32=" << outcome.dumpError << " log=" << (outcome.logWritten ? "ok" : "failed")
            << " tail=" << outcome.logTailCount << " dir=" << root.string() << std::endl;

  if (!PRISM_CHECK(outcome.logWritten)) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return;
  }
  PRISM_CHECK(outcome.dumpWritten);
  PRISM_CHECK_EQ(outcome.dumpError, DWORD(0));
  PRISM_CHECK_EQ(outcome.logTailCount, size_t(64));  // 73 records, 64 kept
  {
    std::error_code sizeEc;
    const auto bytes = std::filesystem::file_size(outcome.dumpPath, sizeEc);
    const std::string head = ReadBytes(outcome.dumpPath).substr(0, 8);
    std::cout << "    inproc dump path=" << crash::detail::Utf8FromWide(outcome.dumpPath)
              << " bytes="
              << (sizeEc ? -1LL : static_cast<long long>(bytes)) << " head=" << head
              << std::endl;
  }
  if (!PRISM_CHECK(HasMiniDumpSignature(outcome.dumpPath))) {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    return;
  }
  const std::string report = ReadBytes(outcome.logPath);
  PRISM_CHECK(!report.empty());
  PRISM_CHECK(Contains(report, "PrismArchive crash report"));
  PRISM_CHECK(Contains(report, "exception-code: 0xE000C016"));
  PRISM_CHECK(Contains(report, "exception-flags: 0x00000001"));
  PRISM_CHECK(Contains(report, "log-tail-count: 64"));
  PRISM_CHECK(Contains(report, "log-ring-size: 72/72"));
  PRISM_CHECK(Contains(report, "--- crash-time log tail (oldest first) ---"));
  PRISM_CHECK(Contains(report, "dump-status: ok"));
  PRISM_CHECK(Contains(report, "ring line 009"));   // the oldest surviving record
  PRISM_CHECK(Contains(report, "ring line 071"));    // the newest one
  PRISM_CHECK(!Contains(report, "ring line 008"));  // evicted by the ring
  PRISM_CHECK(!Contains(report, "ring line 000"));
  // CONTRACTS B7: the credential is absent from the report, the marker is not.
  PRISM_CHECK(!Contains(report, kSecret));
  PRISM_CHECK(Contains(report, plog::kRedacted));
  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}

// ---- the real thing: a child process dies, the parent inspects ---------
PRISM_TEST("crash.child_exception_writes_dump_and_last_64_records") {
  const std::filesystem::path root = TempRoot();
  const std::filesystem::path directory = root / "child";
  std::error_code ec;
  std::filesystem::create_directories(directory, ec);
  std::cout << "    child artifacts: " << root.string() << std::endl;

  if (!PRISM_CHECK(SetEnvironmentVariableW(L"PRISM_C16_CRASH_DIR", directory.c_str()) != FALSE)) {
    std::filesystem::remove_all(root, ec);
    return;
  }

  const std::wstring self = SelfPath();
  std::wstring commandLine = L"\"" + self + L"\" --crash-child";
  std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
  mutableCommand.push_back(L'\0');

  const std::filesystem::path childOutput = root / "child.out";
  SECURITY_ATTRIBUTES attributes{};
  attributes.nLength = sizeof(attributes);
  attributes.bInheritHandle = TRUE;
  HANDLE output = CreateFileW(childOutput.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  if (output != INVALID_HANDLE_VALUE) {
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = output;
    startup.hStdError = output;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
  }
  PROCESS_INFORMATION process{};
  const BOOL started =
      CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                     output != INVALID_HANDLE_VALUE ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                     nullptr, &startup, &process);
  if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
  if (!PRISM_CHECK(started != FALSE)) {
    std::cout << "    CreateProcessW failed, win32=" << GetLastError() << std::endl;
    SetEnvironmentVariableW(L"PRISM_C16_CRASH_DIR", nullptr);
    std::filesystem::remove_all(root, ec);
    return;
  }

  const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
  DWORD exitCode = 0;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hProcess);
  CloseHandle(process.hThread);
  SetEnvironmentVariableW(L"PRISM_C16_CRASH_DIR", nullptr);
  const std::string childStdout = ReadBytes(childOutput);
  std::cout << "    child wait=" << wait << " exit=0x" << std::hex << exitCode << std::dec
            << " stdout=" << (childStdout.empty() ? "(none)" : childStdout.substr(0, 160))
            << std::endl;

  if (!PRISM_CHECK_EQ(wait, DWORD(WAIT_OBJECT_0))) {
    std::filesystem::remove_all(root, ec);  // keep the tree on failure? the log above names it
    return;
  }
  // EXCEPTION_EXECUTE_HANDLER terminating the process yields the exception code
  // as the exit code -- the first proof the filter took over rather than the
  // default handler.
  if (!PRISM_CHECK_EQ(exitCode, DWORD(kCrashCode))) {
    std::filesystem::remove_all(root, ec);
    return;
  }

  const std::vector<std::filesystem::path> dumps = FilesMatching(directory, L".dmp");
  const std::vector<std::filesystem::path> reports = FilesMatching(directory, L".txt");
  if (!PRISM_CHECK_EQ(dumps.size(), size_t(1))) {
    std::filesystem::remove_all(root, ec);
    return;
  }
  PRISM_CHECK_EQ(reports.size(), size_t(1));
  if (reports.empty()) {
    std::filesystem::remove_all(root, ec);
    return;
  }
  PRISM_CHECK(HasMiniDumpSignature(dumps[0]));
  const std::string report = ReadBytes(reports[0]);
  PRISM_CHECK(Contains(report, "exception-code: 0xE000C016"));
  PRISM_CHECK(Contains(report, "log-tail-count: 64"));
  PRISM_CHECK(Contains(report, "log-ring-size: 72/72"));
  PRISM_CHECK(Contains(report, "dump-status: ok"));
  PRISM_CHECK(Contains(report, "ring line 009"));
  PRISM_CHECK(Contains(report, "ring line 071"));
  PRISM_CHECK(!Contains(report, "ring line 008"));
  PRISM_CHECK(!Contains(report, "ring line 000"));
  // B7 through the whole crash path: the value was in the ring, in the message
  // AND in a field, and it is in neither the report nor the dump's text.
  PRISM_CHECK(!Contains(report, kSecret));
  PRISM_CHECK(Contains(report, plog::kRedacted));
  PRISM_CHECK(Contains(report, "password=\"[redacted]\""));
  std::filesystem::remove_all(root, ec);
}

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "--crash-child") {
    return CrashChildMain();  // this process is expected to die inside RaiseException
  }
#ifdef PRISM_C16_MUTANT_NO_REDACTION
  std::cout << "C16 MUTANT ACTIVE: NO_REDACTION (crash-dump redaction cases MUST fail)"
            << std::endl;
#endif
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL CRASH-DUMP CHECKS PASSED" << std::endl;
  return 0;
}
