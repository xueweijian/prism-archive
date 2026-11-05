# c94-overwrite-all: ONE conflict decision per extraction operation (card C94).
#
# Before this card every conflicting file raised its own yes/no/cancel box, so a
# 14-file extraction over 14 existing targets asked 14 times and could not be
# answered once.
#
#   core.c94_overwrite_all        the decision table, counts and scope semantics
#                                 (no filesystem, runs under ASan on Linux too)
#   win.c94_overwrite_all         REAL 7z bytes: 14 conflicts -> exactly ONE
#                                 dialog for each "all" answer, bytes/mtimes on
#                                 disk as the verdict, plus the dialog's own
#                                 controls and the card's screenshot
#   c94.mutation.per_file         negative control (D3): the session's memory is
#                                 compiled out, so "14 conflicts -> 1 dialog"
#                                 must go red with "C94 MUTATION CAUGHT: per_file"
#
# Every suite that can SKIP carries PASS_REGULAR_EXPRESSION *and*
# SKIP_RETURN_CODE 77, so a missing 7z.dll is visible instead of green (D1).
add_executable(core_c94_overwrite_all test_c94_overwrite_core.cpp)
target_link_libraries(core_c94_overwrite_all PRIVATE prism_core)
add_test(NAME "core.c94_overwrite_all" COMMAND core_c94_overwrite_all)
set_tests_properties(core.c94_overwrite_all PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C94 OVERWRITE-ALL CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C94_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib windowscodecs.lib
      d2d1.lib dwrite.lib bcrypt.lib comctl32.lib comdlg32.lib shlwapi.lib)

  add_executable(win_c94_overwrite_all win/test_c94_overwrite_all.cpp)
  target_link_libraries(win_c94_overwrite_all PRIVATE ${PRISM_C94_LIBS})
  target_compile_options(win_c94_overwrite_all PRIVATE /WX-)  # B9: SDK headers
  add_test(NAME "win.c94_overwrite_all" COMMAND win_c94_overwrite_all)
  set_tests_properties(win.c94_overwrite_all PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "ALL C94 OVERWRITE-ALL CHECKS PASSED")

  # The per-file build compiles THIS source with the session's memory removed.
  add_executable(win_c94_overwrite_all_mutant_per_file win/test_c94_overwrite_all.cpp)
  target_link_libraries(win_c94_overwrite_all_mutant_per_file PRIVATE ${PRISM_C94_LIBS})
  target_compile_options(win_c94_overwrite_all_mutant_per_file PRIVATE /WX-)
  target_compile_definitions(win_c94_overwrite_all_mutant_per_file
                             PRIVATE PRISM_C94_MUTANT_PER_FILE=1)
  add_test(NAME "c94.mutation.per_file" COMMAND win_c94_overwrite_all_mutant_per_file)
  set_tests_properties(c94.mutation.per_file PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "C94 MUTATION CAUGHT: per_file")
endif()
