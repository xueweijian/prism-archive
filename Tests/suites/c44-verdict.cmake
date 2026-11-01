# c44-verdict: CONTRACTS D27 -- the mutation verdict is printed at the FIRST
# failed assertion and the process leaves with 0 immediately. One card = one
# file; see suites/README.md.
#
# CTest is the spawner on purpose. The test binary must never create a child:
# on the CI runner (MSVC 14.51 + ASan) in-process child creation fail-fasts with
# 0xC0000409 after ~0.76 s and no ASan report, while MSVC 14.44 locally cannot
# reproduce it. Three commands, each a complete executable body, keep this card
# independent of any CRT/ASan runtime behaviour.
add_executable(core_test_mutation_verdict test_mutation_verdict.cpp)
target_link_libraries(core_test_mutation_verdict PRIVATE prism_core)

# Armed + injection active: the first failure prints the marker and exits 0.
# D1: the marker is part of the verdict, not decoration.
add_test(NAME "c44.verdict.armed"
         COMMAND core_test_mutation_verdict --armed-child)
set_tests_properties(c44.verdict.armed PROPERTIES
    PASS_REGULAR_EXPRESSION "C44 MUTATION CAUGHT")

# Armed + injection NOT active (D3): the failure is recorded, no marker may be
# printed, and the process exits 1. WILL_FAIL is legal only here, and never
# stacked with PASS_REGULAR_EXPRESSION (D26).
add_test(NAME "c44.verdict.inactive"
         COMMAND core_test_mutation_verdict --inactive-child)
set_tests_properties(c44.verdict.inactive PROPERTIES WILL_FAIL TRUE)

# Unarmed: passing checks, no marker, exit 0. The PASS regex pins the internal
# success line so a silent wrong exit can never count.
add_test(NAME "c44.verdict.clean"
         COMMAND core_test_mutation_verdict --clean-child)
set_tests_properties(c44.verdict.clean PROPERTIES
    PASS_REGULAR_EXPRESSION "C44 VERDICT CLEAN: checks ran, no verdict armed, no marker printed")
