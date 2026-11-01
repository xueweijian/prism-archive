# c23-coverage-mutation: make "the tests are hard" a measurable number.
#
# This card adds no product code. It registers three kinds of self-check so the
# measurement tooling cannot rot silently:
#
#   c23.mutation.matrix      every negative control is still wired: test ->
#                            target -> source -> injection macro -> clean
#                            control -> marker. Enforces CONTRACTS D26
#                            (WILL_FAIL + PASS_REGULAR_EXPRESSION is always red;
#                            WILL_FAIL + SKIP_RETURN_CODE is a fake green).
#   c23.mutation.negctl.*    the D3 classifier itself is calibrated on three real
#                            binaries built here: clean / injected / DEAD
#                            INJECTION (macro misspelled). The dead one must be
#                            reported NOT-IN-EFFECT -- never CAUGHT, never green.
#   c23.mutation.round       inject -> run -> require red -> restore-control on a
#                            fast, platform-neutral subset (the full round is
#                            tools/mutation/negctl.py run --build-dir <dir>).
#   c23.coverage.selftest    the LCOV merger/policy engine is calibrated on a
#                            fixture before anyone trusts its percentages (D4).
#
# One card = one file; Tests/CMakeLists.txt is untouched (suites/README.md).
if(NOT Python3_FOUND)
  message(STATUS "c23: Python3 not found -- coverage/mutation automation not registered")
  return()
endif()

set(C23_PY "${Python3_EXECUTABLE}")
set(C23_MUT "${CMAKE_SOURCE_DIR}/tools/mutation")
set(C23_COV "${CMAKE_SOURCE_DIR}/tools/cov")

# ---------------------------------------------------------------------------
# Fixtures for the D3 classifier. Same source, three compile definitions: one
# clean control, one injected defect, one MISSPELLED macro (the silent no-op
# that must be detected as such).
# ---------------------------------------------------------------------------
foreach(probe IN ITEMS clean active typo)
  add_executable(c23_mutant_probe_${probe} c23_mutation_probe.cpp)
  if(MSVC)
    target_compile_options(c23_mutant_probe_${probe} PRIVATE /WX-)
  endif()
endforeach()
target_compile_definitions(c23_mutant_probe_active PRIVATE PRISM_C23_MUTANT_PROBE=1)
# Deliberately WRONG macro name (the defect D3 exists for).
target_compile_definitions(c23_mutant_probe_typo PRIVATE PRISM_C23_MUTANT_PROBE_TYPO=1)

add_test(NAME c23.mutation.matrix
         COMMAND "${C23_PY}" "${C23_MUT}/mutant_matrix.py" --check
                 --root "${CMAKE_SOURCE_DIR}"
                 --json "${CMAKE_BINARY_DIR}/c23/mutant-matrix.json"
                 --markdown "${CMAKE_BINARY_DIR}/c23/mutant-matrix.md")
set_tests_properties(c23.mutation.matrix PROPERTIES TIMEOUT 180
    PASS_REGULAR_EXPRESSION "C23 MUTATION MATRIX: OK")

add_test(NAME c23.mutation.negctl.control
         COMMAND "${C23_PY}" "${C23_MUT}/negctl.py" probe
                 --exe "$<TARGET_FILE:c23_mutant_probe_clean>" --expect CLEAN)
set_tests_properties(c23.mutation.negctl.control PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C23 NEGCTL PROBE: CLEAN")

add_test(NAME c23.mutation.negctl.caught
         COMMAND "${C23_PY}" "${C23_MUT}/negctl.py" probe
                 --exe "$<TARGET_FILE:c23_mutant_probe_active>"
                 --macro PRISM_C23_MUTANT_PROBE
                 --build-dir "${CMAKE_BINARY_DIR}" --target c23_mutant_probe_active
                 --marker "C23 MUTANT ACTIVE: PROBE" --expect CAUGHT)
set_tests_properties(c23.mutation.negctl.caught PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C23 NEGCTL PROBE: CAUGHT")

# THE key negative evidence (D3, automated): the macro is spelled wrong, so the
# binary is effectively clean. A harness that calls this "caught" is lying; the
# verdict must be NOT-IN-EFFECT and the ctest line below pins exactly that.
add_test(NAME c23.mutation.negctl.dead_injection
         COMMAND "${C23_PY}" "${C23_MUT}/negctl.py" probe
                 --exe "$<TARGET_FILE:c23_mutant_probe_typo>"
                 --macro PRISM_C23_MUTANT_PROBE
                 --build-dir "${CMAKE_BINARY_DIR}" --target c23_mutant_probe_typo
                 --marker "C23 MUTANT ACTIVE: PROBE" --expect NOT-IN-EFFECT)
set_tests_properties(c23.mutation.negctl.dead_injection PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C23 NEGCTL PROBE: NOT-IN-EFFECT")

# Fast, platform-neutral subset: the full round is the same command without --only.
add_test(NAME c23.mutation.round
         COMMAND "${C23_PY}" "${C23_MUT}/negctl.py" run
                 --build-dir "${CMAKE_BINARY_DIR}"
                 --only "^mutation\\.(list_sort|filter_model|volume_plan|compress_plan|theme|settings_store|sidebar|extract_scheduler|security|history|zoom|mkdir|palette|drag_drop|dnd_seek)\\."
                 --json-out "${CMAKE_BINARY_DIR}/c23/negctl.json")
set_tests_properties(c23.mutation.round PROPERTIES TIMEOUT 1200
    PASS_REGULAR_EXPRESSION "C23 NEGCTL: OK")

add_test(NAME c23.coverage.selftest
         COMMAND "${C23_PY}" "${C23_COV}/coverage.py" selftest)
set_tests_properties(c23.coverage.selftest PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C23 COVERAGE SELFTEST: OK")
