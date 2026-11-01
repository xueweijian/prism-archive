// PrismArchive -- CONTRACTS D27 regression (card C44).
//
// The verdict mechanism is process-level: the FIRST failed assertion must print
// the caller's marker and leave with 0 immediately. This binary deliberately
// does NOT create that process from inside itself: on the CI runner
// (MSVC 14.51 + ASan) in-process child creation fail-fasts with 0xC0000409
// after ~0.76 s and no ASan report, while MSVC 14.44 locally cannot reproduce
// it. CTest is the spawner instead -- the only design that does not depend on a
// CRT/ASan runtime version.
//
// main() is three complete executables in one file, each registered as its own
// CTest entry (Tests/suites/c44-verdict.cmake):
//
//   * --armed-child    : armed + predicate true. The failing check must print
//                        "C44 MUTATION CAUGHT" and exit 0 (PASS regex, D1).
//   * --inactive-child : armed + predicate false (D3: red for an unrelated
//                        reason). The failure is recorded, no marker may be
//                        printed, the process exits 1. Only WILL_FAIL is
//                        attached (D26: never with a PASS regex).
//   * --clean-child    : no verdict armed. Passing checks, no marker, exit 0;
//                        the PASS regex pins the internal success line.
//
// Negative control (D3): force the predicate check in ReportFailure to be true
// and c44.verdict.inactive must fail -- the armed process now prints the marker
// and exits 0, which breaks WILL_FAIL. That is how the harness proves it can
// still go red for the right reason.
#include <cstdio>
#include <cstring>

#include "mini_test.hpp"

namespace {

const char* kMarker = "C44 MUTATION CAUGHT: probe";

}  // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "--armed-child") == 0) {
    // D27 + D3: armed, and the predicate proves the injected defect fired.
    prism::test::SetMutationVerdict(kMarker, [] { return true; });
    PRISM_CHECK(false);  // ReportFailure must print the marker and leave with 0.
    std::printf("C44 VERDICT ARMED: NOT CAUGHT -- the harness did not leave\n");
    return 0;
  }

  if (argc == 2 && std::strcmp(argv[1], "--inactive-child") == 0) {
    // D27 + D3: armed, but this red came from something other than the
    // injection. No CAUGHT may be printed and the run must stay red.
    prism::test::SetMutationVerdict(kMarker, [] { return false; });
    PRISM_CHECK(false);  // recorded; the predicate sends us down the ordinary path
    if (prism::test::MutationVerdictPrinted()) {
      // A marker would have left the process; reaching here is a harness bug.
      std::printf("C44 VERDICT INACTIVE: BAD -- a CAUGHT marker was printed\n");
      return 0;  // exit 0 so WILL_FAIL reports this as RED, not as an expected failure
    }
    if (prism::test::FailureCount() != 1) {
      std::printf("C44 VERDICT INACTIVE: BAD -- the failure was not recorded\n");
      return 0;
    }
    std::printf("C44 VERDICT INACTIVE: no marker printed, failure recorded, staying red\n");
    return 1;  // WILL_FAIL: this non-zero exit is the expected (green) outcome
  }

  if (argc == 2 && std::strcmp(argv[1], "--clean-child") == 0) {
    // No verdict armed: the old behaviour must be untouched.
    PRISM_CHECK(true);
    PRISM_CHECK(!prism::test::MutationVerdictArmed());
    PRISM_CHECK(!prism::test::MutationVerdictPrinted());
    std::printf("C44 VERDICT CLEAN: checks ran, no verdict armed, no marker printed\n");
    return 0;
  }

  std::printf(
      "usage: core_test_mutation_verdict --armed-child|--inactive-child|--clean-child\n");
  return 2;
}
