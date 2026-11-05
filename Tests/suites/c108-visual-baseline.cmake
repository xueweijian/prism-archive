# c108-visual-baseline: THE REFEREE (card C108). docs/UI-BASELINE.md quotes the
# numbers this file produces; the eleven visual claims of the redesign wave are
# each one row, and every row names the card that must move it.
#
# WHY A RECORDER AND NOT ANOTHER FIX SUITE. C108 changes no product code. Its job
# is to make "looks better" reviewable: seven rows are source/token facts this
# binary measures (mnemonic letters, the Day canvas, the island's body, the
# "Segoe UI" literal count, the menu/caption delta, the optics wiring, and the
# panel's backdrop delta), and four are pixel facts measured from the committed
# 1600 x 1000 baseline screenshot by tools/visual_baseline.ps1.
#
# The CLEAN target asserts the MEASUREMENT is valid -- the scan walked >= 50
# files, the language table was found, all eight band mnemonics exist, every
# token resolves, and the table is stable across two runs -- and prints the
# verdict of each TARGET predicate. It does NOT assert the target itself: on
# main most rows are still FAIL, and a suite that is red on push would teach
# everyone to ignore it. Card owners assert their own row in their own suite
# (c100..c107, c109) and update the matrix's "current" column.
#
# NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): three arms compiled from the SAME
# source, one rule broken each, armed on a runtime proof (the scan really found
# no Src/, or the injection counter really fired). Registered with
# PASS_REGULAR_EXPRESSION on the verdict -- never WILL_FAIL.
#   mutation.c108.scan_root   the scanner is pointed at a tree with no Src/
#   mutation.c108.flat_day    the Day canvas is forced to the island colour
#   mutation.c108.mnemonic    8 band letters + the menu letter are forced back
add_executable(core_c108_baseline test_c108_baseline.cpp)
target_link_libraries(core_c108_baseline PRIVATE prism_core)
target_compile_definitions(core_c108_baseline PRIVATE
                           "PRISM_C108_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\"")
add_test(NAME "core.c108_visual_baseline" COMMAND core_c108_baseline)
set_tests_properties(core.c108_visual_baseline PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C108 BASELINE CHECKS PASSED")

# D3 control 1: a root with no Src/ -- the "vacuous zero" the file counter
# exists to catch. The scan-validity assertion MUST go red.
add_executable(core_c108_baseline_mutant_scan_root test_c108_baseline.cpp)
target_link_libraries(core_c108_baseline_mutant_scan_root PRIVATE prism_core)
target_compile_definitions(core_c108_baseline_mutant_scan_root PRIVATE
    "PRISM_C108_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}/Tests/fixtures/c108-empty\""
    PRISM_C108_MUTANT_EMPTY_ROOT=1)
add_test(NAME "mutation.c108.scan_root" COMMAND core_c108_baseline_mutant_scan_root)
set_tests_properties(mutation.c108.scan_root PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C108 MUTATION CAUGHT: scan_root")

# D3 control 2: the Day canvas becomes the island colour. The #F5F5F7 row MUST
# go red, which is what makes "the canvas moved" a red-able statement.
add_executable(core_c108_baseline_mutant_flat_day test_c108_baseline.cpp)
target_link_libraries(core_c108_baseline_mutant_flat_day PRIVATE prism_core)
target_compile_definitions(core_c108_baseline_mutant_flat_day PRIVATE
                           "PRISM_C108_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
                           PRISM_C108_MUTANT_FLAT_DAY=1)
add_test(NAME "mutation.c108.flat_day" COMMAND core_c108_baseline_mutant_flat_day)
set_tests_properties(mutation.c108.flat_day PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C108 MUTATION CAUGHT: flat_day")

# D3 control 3: the pre-C100 band letters and the pre-C106 menu letter come
# back. The mnemonic row MUST go red.
add_executable(core_c108_baseline_mutant_mnemonic test_c108_baseline.cpp)
target_link_libraries(core_c108_baseline_mutant_mnemonic PRIVATE prism_core)
target_compile_definitions(core_c108_baseline_mutant_mnemonic PRIVATE
                           "PRISM_C108_SOURCE_DIR=\"${CMAKE_SOURCE_DIR}\""
                           PRISM_C108_MUTANT_MNEMONIC=1)
add_test(NAME "mutation.c108.mnemonic" COMMAND core_c108_baseline_mutant_mnemonic)
set_tests_properties(mutation.c108.mnemonic PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C108 MUTATION CAUGHT: mnemonic")
