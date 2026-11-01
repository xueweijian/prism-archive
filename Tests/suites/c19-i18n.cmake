# c19-i18n: the en/zh resource tables (lookup, fallback chain, placeholders,
# misses) plus the language choice driven by card C14's language.tag.
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# The assets are a COMPILE-TIME input: the path is baked in, so a worktree that
# lost Assets/Lang fails the build instead of passing a suite that reads nothing
# (CONTRACTS D1 -- a test that cannot SKIP must not be able to pass vacuously).
set(PRISM_C19_LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Lang")
if(NOT EXISTS "${PRISM_C19_LANG_DIR}/en-US.json" OR
   NOT EXISTS "${PRISM_C19_LANG_DIR}/zh-CN.json")
  message(FATAL_ERROR "c19-i18n: Assets/Lang/{en-US,zh-CN}.json are missing")
endif()

add_executable(test_i18n test_i18n.cpp)
target_link_libraries(test_i18n PRIVATE prism_core)
target_compile_definitions(test_i18n PRIVATE
                           "PRISM_C19_LANG_DIR=\"${PRISM_C19_LANG_DIR}\"")
add_test(NAME "core.test_i18n" COMMAND test_i18n)
set_tests_properties(core.test_i18n PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL I18N CHECKS PASSED")

# Three negative controls (CONTRACTS D3). Each mutant compiles the SAME suite
# with one rule turned off; WILL_FAIL makes CTest green only while the mutation
# is actually caught. A mutation that silently stops applying leaves the suite
# green and turns THAT entry red -- the inversion is the verdict.
#
# Each mutant also gets a SECOND entry that runs the binary in --mutation-report
# mode and pins "C19 MUTANT ACTIVE: <id>" as a PASS regex. That is the injection's
# own proof: if the macro never reached I18n.hpp, this entry is red even though
# WILL_FAIL never looked at the exit code. (C14's lesson: "the replacement string
# did not match" is a failure mode a suite cannot see from the inside.)
foreach(mutant IN ITEMS NO_FALLBACK NO_PLACEHOLDER NO_MISSING_TRACKING)
  string(TOLOWER "${mutant}" mutant_lc)
  string(REPLACE "_" "-" mutant_id "${mutant_lc}")
  set(mutant_exe "test_i18n_mutant_${mutant_lc}")

  add_executable("${mutant_exe}" test_i18n.cpp)
  target_link_libraries("${mutant_exe}" PRIVATE prism_core)
  target_compile_definitions("${mutant_exe}" PRIVATE
                             "PRISM_C19_MUTANT_${mutant}=1"
                             "PRISM_C19_LANG_DIR=\"${PRISM_C19_LANG_DIR}\"")

  add_test(NAME "mutation.i18n.${mutant_id}" COMMAND "${mutant_exe}")
  set_tests_properties("mutation.i18n.${mutant_id}" PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 120)

  add_test(NAME "mutation.i18n.${mutant_id}.injected"
           COMMAND "${mutant_exe}" --mutation-report)
  set_tests_properties("mutation.i18n.${mutant_id}.injected" PROPERTIES
                       TIMEOUT 60
                       PASS_REGULAR_EXPRESSION "C19 MUTANT ACTIVE: ${mutant_id}")
endforeach()
