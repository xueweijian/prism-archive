# c59-tabs: the TAB SET model (several archives open at once).
#
# The model is Src/PrismCore/TabSet.hpp -- pure, header-only, no window, no
# filesystem, no 7-Zip. The card that DRAWS the strip (the next one) must not
# have to decide any rule: open/close/activate/reorder, the right-then-left
# neighbour rule, the dirty-tab confirmation, the 16-tab ceiling and the C46
# session projection (MRU, active first, exact round trip) are all answered here
# and asserted in Tests/test_tab_set.cpp on every lane.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). Two mutants compile the SAME
# source with one rule switched off:
#
#   mutation.tab_set.close_right_neighbor  closing the active tab always
#                                          activates tab 0
#   mutation.tab_set.dedup_open            reopening an open archive appends a
#                                          second tab for the same path
#
# Neither uses WILL_FAIL: each binary arms prism::test::SetMutationVerdict with a
# RUNTIME predicate, prints "C59 MUTATION CAUGHT: <arm>" at the FIRST red
# assertion and exits 0, so CTest's PASS_REGULAR_EXPRESSION is the verdict. A
# stale injection (the mutated statement no longer fires) leaves the suite green
# and the target RED -- the marker is a conjunction,  not a replacement (D26).
#
# ARM NAMES avoid clean / unarmed / control anywhere (D28: those words make the
# C23 matrix read an entry as a CONTROL, and an armed control is a contradiction).
# The clean control is core.tab_set: same source, no injection define.
add_executable(core_tab_set_test test_tab_set.cpp)
target_link_libraries(core_tab_set_test PRIVATE prism_core)
add_test(NAME "core.tab_set" COMMAND core_tab_set_test)
set_tests_properties(core.tab_set PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL TAB-SET CHECKS PASSED")

# D3 control 1: "closing always activates the first tab".
add_executable(core_tab_set_mutant_close_right_neighbor test_tab_set.cpp)
target_link_libraries(core_tab_set_mutant_close_right_neighbor PRIVATE prism_core)
target_compile_definitions(core_tab_set_mutant_close_right_neighbor
                           PRIVATE PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR=1)
add_test(NAME "mutation.tab_set.close_right_neighbor"
         COMMAND core_tab_set_mutant_close_right_neighbor)
set_tests_properties(mutation.tab_set.close_right_neighbor PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C59 MUTATION CAUGHT: close_right_neighbor")

# D3 control 2: "the same archive opened twice is two tabs".
add_executable(core_tab_set_mutant_dedup_open test_tab_set.cpp)
target_link_libraries(core_tab_set_mutant_dedup_open PRIVATE prism_core)
target_compile_definitions(core_tab_set_mutant_dedup_open
                           PRIVATE PRISM_C59_MUTANT_DEDUP_OPEN=1)
add_test(NAME "mutation.tab_set.dedup_open"
         COMMAND core_tab_set_mutant_dedup_open)
set_tests_properties(mutation.tab_set.dedup_open PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C59 MUTATION CAUGHT: dedup_open")
