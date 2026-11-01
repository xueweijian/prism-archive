// PrismArchive -- CONTRACTS D27 regression (card C44).
//
// The property under test is process-level: the mutation verdict must be
// printed at the FIRST failed assertion and the process must leave with 0, so
// no teardown crash or hang can swallow the marker. The suite therefore
// re-executes itself as a child for each shape:
//
//   * --armed-child : SetMutationVerdict("C44 MUTATION CAUGHT: probe") and then
//                     one failing check. The harness must print the caller's
//                     marker immediately after the FAIL line, must NOT execute
//                     the next statement, and must exit 0.
//   * --clean-child : the same failing check without a verdict armed. The old
//                     behaviour must be untouched: FAIL is recorded, execution
//                     continues, the process exits 1.
//
// Parent-side assertions are on strings (D2) and each child writes into its own
// unique temp directory (D9). No GUI, no 7z.dll, no window station.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "mini_test.hpp"

namespace {

namespace fs = std::filesystem;

const char* g_exe = "core_test_mutation_verdict";
const char* kMarker = "C44 MUTATION CAUGHT: probe";

int ProcessId() {
#ifdef _WIN32
  return _getpid();
#else
  return static_cast<int>(getpid());
#endif
}

fs::path UniqueDir(const char* tag) {
  static int counter = 0;
  const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() / ("prism_c44_verdict_" + std::to_string(ProcessId()) + "_" +
                                      std::to_string(stamp) + "_" + std::to_string(++counter) + "_" +
                                      tag);
}

std::string Quote(const fs::path& path) { return "\"" + path.string() + "\""; }

struct ChildRun {
  int exitCode = -1;
  std::string output;
};

ChildRun RunChild(const char* mode) {
  const fs::path dir = UniqueDir(mode + 1);  // skip the leading '-'
  std::error_code ec;
  fs::create_directories(dir, ec);
  const fs::path log = dir / "child.log";
  std::string command = Quote(fs::absolute(g_exe)) + " " + mode + " > " + Quote(log) + " 2>&1";
#ifdef _WIN32
  // cmd.exe strips the first and last quote of "cmd /c <command>" when the
  // command itself starts with a quote. Without this extra outer pair the
  // closing quote of the log path is eaten and system() returns the misleading
  // "filename, directory name, or volume label syntax is incorrect".
  command = "\"" + command + "\"";
#endif
  const int status = std::system(command.c_str());

  ChildRun run;
#ifdef _WIN32
  run.exitCode = status;
#else
  run.exitCode = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : -1;
#endif
  std::ifstream in(log, std::ios::binary);
  if (in) {
    run.output.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  fs::remove_all(dir, ec);
  return run;
}

void DumpOnFailure(const ChildRun& run, bool ok) {
  if (ok) return;
  std::cout << "---- child output (exit " << run.exitCode << ") ----" << std::endl;
  std::cout << run.output;
  std::cout << "---- end child output ----" << std::endl;
}

}  // namespace

PRISM_TEST("verdict.first_failure_prints_marker_and_exits_zero") {
  const ChildRun run = RunChild("--armed-child");
  const std::size_t failAt = run.output.find("    FAIL  ");
  const std::size_t markerAt = run.output.find(kMarker);
  const bool exitZero = run.exitCode == 0;
  const bool failPrinted = failAt != std::string::npos;
  const bool markerPrinted = markerAt != std::string::npos;
  const bool markerAfterFail = failPrinted && markerPrinted && failAt < markerAt;
  // The marker is emitted by the failure callback itself, so the very next
  // output is its "first failure at <file>:<line>" suffix -- not a later test.
  const bool saysFirstFailure = run.output.find("first failure at") != std::string::npos;
  const bool saysImmediateExit = run.output.find("exiting 0 now so teardown") != std::string::npos;
  // Proof the process did not merely survive to the end of main: the statement
  // right after the failing check must never run.
  const bool continuedAfterFailure =
      run.output.find("ARMED CHILD CONTINUED") != std::string::npos;
  DumpOnFailure(run, exitZero && failPrinted && markerPrinted && markerAfterFail &&
                         saysFirstFailure && saysImmediateExit && !continuedAfterFailure);
  PRISM_CHECK(exitZero);
  PRISM_CHECK(failPrinted);
  PRISM_CHECK(markerPrinted);
  PRISM_CHECK(markerAfterFail);
  PRISM_CHECK(saysFirstFailure);
  PRISM_CHECK(saysImmediateExit);
  PRISM_CHECK(!continuedAfterFailure);
}

PRISM_TEST("verdict.unarmed_keeps_record_and_continue_behaviour") {
  const ChildRun run = RunChild("--clean-child");
  const bool exitOne = run.exitCode == 1;
  const bool failPrinted = run.output.find("    FAIL  ") != std::string::npos;
  const bool noMarker = run.output.find("C44 MUTATION CAUGHT") == std::string::npos;
  // Unarmed means old behaviour: the failure is recorded but execution keeps
  // going, exactly as every existing suite relies on.
  const bool continued = run.output.find("CLEAN CHILD CONTINUED") != std::string::npos;
  DumpOnFailure(run, exitOne && failPrinted && noMarker && continued);
  PRISM_CHECK(exitOne);
  PRISM_CHECK(failPrinted);
  PRISM_CHECK(noMarker);
  PRISM_CHECK(continued);
}

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--armed-child") == 0) {
    prism::test::SetMutationVerdict(kMarker);
    PRISM_CHECK(false);  // D27: the harness must print the marker and _Exit(0).
    std::printf("ARMED CHILD CONTINUED (the verdict did not leave at the first failure)\n");
    return 0;
  }
  if (argc > 1 && std::strcmp(argv[1], "--clean-child") == 0) {
    PRISM_CHECK(false);  // No verdict armed: FAIL is recorded and execution goes on.
    std::printf("CLEAN CHILD CONTINUED (no verdict armed; the old behaviour is intact)\n");
    return prism::test::FailureCount() == 0 ? 0 : 1;
  }
  static const std::string exePath =
      argc > 0 && argv[0] != nullptr ? fs::absolute(argv[0]).string() : std::string(g_exe);
  g_exe = exePath.c_str();
  return prism::test::RunAll(argc, argv);
}
