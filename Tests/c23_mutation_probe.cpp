// C23 -- fixture for the mutation harness's own negative control (CONTRACTS D3).
//
// The exact defect shape this project keeps hitting: a mutant macro that stops
// matching (a typo, a renamed guard) turns the mutant into the CLEAN binary.
// "The suite stayed green" then means nothing. tools/mutation/negctl.py must
// call that state MUTATION NOT IN EFFECT, and this fixture gives it three real
// binaries to prove the classifier is calibrated (CONTRACTS D4):
//
//   c23_mutant_probe_clean  no macro        -> green, no banner
//   c23_mutant_probe_active PRISM_C23_MUTANT_PROBE  -> banner + one red check
//   c23_mutant_probe_typo   PRISM_C23_MUTANT_PROBE_TYPO -> behaves clean (DEAD)
//
// Tests/suites/c23-coverage-mutation.cmake drives all three through negctl.py.
#include <iostream>

#include "mini_test.hpp"

namespace {

void PrintMutantBanner() {
#if defined(PRISM_C23_MUTANT_PROBE)
  std::cout << "C23 MUTANT ACTIVE: PROBE (one deliberate defect; this suite MUST go red)"
            << std::endl;
#endif
}

}  // namespace

PRISM_TEST("c23.probe.sanity") { PRISM_CHECK_EQ(2 + 2, 4); }

PRISM_TEST("c23.probe.mutant_only_defect") {
#if defined(PRISM_C23_MUTANT_PROBE)
  // Deliberate defect: only the injected build reaches this assertion.
  PRISM_CHECK_EQ(2 + 2, 5);
#endif
  PRISM_CHECK_EQ(6 * 7, 42);
}

int main(int argc, char** argv) {
  PrintMutantBanner();
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL C23 PROBE CHECKS PASSED" << std::endl;
  return 0;
}
