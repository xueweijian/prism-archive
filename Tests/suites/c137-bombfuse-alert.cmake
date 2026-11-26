# c137-bombfuse-alert: BombFuse wired at all 4 ExtractWithPlan sites + red glass
# security alert modal/card + real bomb-fuse trip integration test & mutant.
#
# Negative control (CONTRACTS D3/D26/D27):
#   * mutation.c137.null_fuse -- SelectProductBombFuse returns nullptr; the
#     65 MiB bomb extracts without tripping and the clean assertion goes red.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c137_bombfuse_alert win/test_c137_bombfuse_alert.cpp)
  target_link_libraries(win_c137_bombfuse_alert PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib ole32.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_c137_bombfuse_alert PRIVATE /WX-)
  add_test(NAME "win.c137_bombfuse_alert" COMMAND win_c137_bombfuse_alert)
  set_tests_properties(win.c137_bombfuse_alert PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C137 CHECKS PASSED")

  add_executable(win_c137_mutant_null_fuse win/test_c137_bombfuse_alert.cpp)
  target_link_libraries(win_c137_mutant_null_fuse PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib ole32.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_c137_mutant_null_fuse PRIVATE /WX-)
  target_compile_definitions(win_c137_mutant_null_fuse PRIVATE PRISM_C137_MUTANT_NULL_FUSE=1)
  add_test(NAME "mutation.c137.null_fuse" COMMAND win_c137_mutant_null_fuse)
  set_tests_properties(mutation.c137.null_fuse PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C137 MUTATION CAUGHT: null_fuse")
endif()
