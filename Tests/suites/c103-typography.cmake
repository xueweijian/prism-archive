# c103-typography: ONE font stack, ONE TextFormat entry (card C103, BLUEPRINT:170).
#
# Two halves, because the defect had two shapes:
#   core.c103_font_literals   the mechanical guard over Src/: the literal
#                             "Segoe UI" appears ZERO times outside
#                             Typography.hpp, and no source line calls DWrite's
#                             CreateTextFormat directly. The scan also asserts it
#                             walked >= 50 files and saw Typography.hpp, so a
#                             wrong root is RED, not a vacuous zero.
#   win.c103_typography       the REAL resolver against INJECTED collections:
#                             {Inter, Segoe UI} -> Inter, only Segoe -> Segoe,
#                             order (SF Pro before Inter) and the documented
#                             fallback; the second call costs zero
#                             FindFamilyName calls; CreateTextFormat's format
#                             reports the same family the resolver returned.
#
# NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): one executable per injected defect,
# compiled from the SAME source, verdict = PASS_REGULAR_EXPRESSION on the D27
# marker. No WILL_FAIL is stacked on a regex anywhere in this file.
#   mutation.c103.segoe_literal   the scanner is pointed at a fixture that
#                                 contains the literal -> assertion 1 red.
#   mutation.c103.segoe           ResolveFamily always returns Segoe UI (the
#                                 pre-C103 behaviour) -> assertion 2 red.
add_executable(core_c103_font_literals test_c103_font_literals.cpp)
target_link_libraries(core_c103_font_literals PRIVATE prism_core)
target_compile_definitions(core_c103_font_literals
                           PRIVATE "PRISM_C103_SRC_DIR=\"${CMAKE_SOURCE_DIR}/Src\"")
add_test(NAME "core.c103_font_literals" COMMAND core_c103_font_literals)
set_tests_properties(core.c103_font_literals PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C103 FONT-LITERAL CHECKS PASSED")

add_executable(core_c103_font_literals_mutant_segoe test_c103_font_literals.cpp)
target_link_libraries(core_c103_font_literals_mutant_segoe PRIVATE prism_core)
target_compile_definitions(core_c103_font_literals_mutant_segoe PRIVATE
    "PRISM_C103_SRC_DIR=\"${CMAKE_SOURCE_DIR}/Tests/fixtures/c103\""
    PRISM_C103_SCAN_FIXTURE=1)
add_test(NAME "mutation.c103.segoe_literal"
         COMMAND core_c103_font_literals_mutant_segoe)
set_tests_properties(mutation.c103.segoe_literal PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C103 MUTATION CAUGHT: segoe_literal")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c103_typography win/test_c103_typography.cpp)
  target_link_libraries(win_c103_typography PRIVATE prism_core prism_win_bridge
                        user32.lib dwrite.lib)
  target_compile_options(win_c103_typography PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c103_typography" COMMAND win_c103_typography)
  set_tests_properties(win.c103_typography PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C103 TYPOGRAPHY CHECKS PASSED")

  # Arm: the pre-C103 resolver -- every lookup answers Segoe UI. The injected
  # branch counts its hits, so the verdict can only print if it really ran.
  add_executable(win_c103_typography_mutant_segoe win/test_c103_typography.cpp)
  target_link_libraries(win_c103_typography_mutant_segoe PRIVATE prism_core
                        prism_win_bridge user32.lib dwrite.lib)
  target_compile_options(win_c103_typography_mutant_segoe PRIVATE /WX-)
  target_compile_definitions(win_c103_typography_mutant_segoe
                             PRIVATE PRISM_C103_MUTANT_SEGOE=1)
  add_test(NAME "mutation.c103.segoe" COMMAND win_c103_typography_mutant_segoe)
  set_tests_properties(mutation.c103.segoe PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C103 MUTATION CAUGHT: segoe")
endif()
