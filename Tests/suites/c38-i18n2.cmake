# c38-i18n2: the i18n KEY AUDIT -- source call sites versus both shipped tables.
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# The asset directory AND the source root are COMPILE-TIME inputs. A worktree
# that moved either one fails the CONFIGURE (FATAL_ERROR) or the COMPILE
# (#error in test_i18n_audit.cpp) instead of passing a suite that read nothing
# -- CONTRACTS D1, the "a test that cannot SKIP must not be able to pass
# vacuously" rule. The suite's own assertions add the runtime half of that
# guard (>= 20 files, >= 12 distinct keys).
set(PRISM_C38_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PRISM_C38_LANG_DIR "${PRISM_C38_SOURCE_DIR}/Assets/Lang")

if(NOT EXISTS "${PRISM_C38_SOURCE_DIR}/Src/App/Main.cpp")
  message(FATAL_ERROR "c38-i18n2: ${PRISM_C38_SOURCE_DIR}/Src/App/Main.cpp is missing; "
                      "the audit would scan nothing")
endif()
if(NOT EXISTS "${PRISM_C38_LANG_DIR}/en-US.json" OR
   NOT EXISTS "${PRISM_C38_LANG_DIR}/zh-CN.json")
  message(FATAL_ERROR "c38-i18n2: Assets/Lang/{en-US,zh-CN}.json are missing")
endif()

add_executable(test_i18n_audit test_i18n_audit.cpp)
target_link_libraries(test_i18n_audit PRIVATE prism_core)
target_compile_definitions(test_i18n_audit PRIVATE
                           "PRISM_C38_LANG_DIR=\"${PRISM_C38_LANG_DIR}\""
                           "PRISM_C38_SOURCE_DIR=\"${PRISM_C38_SOURCE_DIR}\"")
add_test(NAME "core.test_i18n_audit" COMMAND test_i18n_audit)
# D1: the marker is part of the verdict (exit 0 alone is not a pass).
set_tests_properties(core.test_i18n_audit PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL I18N AUDIT CHECKS PASSED")

# ---------------------------------------------------------------------------
# Negative control (CONTRACTS D3 + D26). The SAME suite is rebuilt with one
# well-formed key injected that no table defines ("menu.file.typo.control").
# The audit must NAME it against both tables and go red.
#
# D26 shape, deliberately NOT the WILL_FAIL+regex pair (permanently red on this
# CTest) and not a bare WILL_FAIL (a crash also exits non-zero, so a crashing
# audit would look caught): the mutant binary prints
#   "C38 MUTANT CAUGHT: <key> is in no shipped table"
# ONLY when the injection landed AND the clean pass is green AND the key is
# reported missing in BOTH tables. That marker is the whole pass condition; the
# exit code is irrelevant once the regex is set, which is exactly why the
# marker has to be a conjunction.
# ---------------------------------------------------------------------------
add_executable(test_i18n_audit_mutant_fake_key test_i18n_audit.cpp)
target_link_libraries(test_i18n_audit_mutant_fake_key PRIVATE prism_core)
target_compile_definitions(test_i18n_audit_mutant_fake_key PRIVATE
                           "PRISM_C38_MUTANT_FAKE_KEY=1"
                           "PRISM_C38_LANG_DIR=\"${PRISM_C38_LANG_DIR}\""
                           "PRISM_C38_SOURCE_DIR=\"${PRISM_C38_SOURCE_DIR}\"")
add_test(NAME "mutation.i18n_audit.fake_key" COMMAND test_i18n_audit_mutant_fake_key)
set_tests_properties("mutation.i18n_audit.fake_key" PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C38 MUTANT CAUGHT: menu\\.file\\.typo\\.control")
