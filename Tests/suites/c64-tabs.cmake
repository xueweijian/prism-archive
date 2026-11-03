# c64-tabs: THE TABS ARE LIVE -- clicking one really loads the archive, the
# command line opens one tab per archive, the Settings item is translated and
# the action island is measured against its DESIGN token.
#
# Three layers:
#   * win.c64_tabs      -- in-process window + real 7z fixtures: click tab ->
#                         C34 SwitchArchive -> rows change (by PATH, because both
#                         fixtures contribute one root row); a deleted target
#                         leaves the original tab active and counts the refusal;
#                         the Settings menu label read back from the HMENU; the
#                         island/canvas/text pixels from the real renderer.
#   * win.c64_launch    -- the SHIPPED exe with two archives on the command line:
#                         stderr must say "tabs opened=2 skipped=0 active=<first>",
#                         and a missing third argument "opened=2 skipped=1" with
#                         no dialog.
#   * two mutants       -- one rule lowered each, the marker is the conjunction
#                         (CONTRACTS D3 + D26/D27; no WILL_FAIL anywhere).
#
# The clean targets carry their own stdout marker; a silent skip is exit 77 and
# still needs the marker (CONTRACTS D1).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c64_tabs win/test_c64_tabs.cpp)
  target_link_libraries(win_c64_tabs PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  target_compile_options(win_c64_tabs PRIVATE /WX-)
  add_test(NAME "win.c64_tabs" COMMAND win_c64_tabs)
  set_tests_properties(win.c64_tabs PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C64 TAB CHECKS PASSED")

  # Defect re-injected: the switch succeeds but the rows stay those of the old
  # archive. "The rows become the second archive (by path)" MUST go red.
  add_executable(win_c64_tabs_mutant_stale_tab_content win/test_c64_tabs.cpp)
  target_link_libraries(win_c64_tabs_mutant_stale_tab_content PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
                        shell32.lib)
  target_compile_options(win_c64_tabs_mutant_stale_tab_content PRIVATE /WX-)
  target_compile_definitions(win_c64_tabs_mutant_stale_tab_content
                             PRIVATE PRISM_C64_MUTANT_STALE_TAB_CONTENT=1)
  add_test(NAME "mutation.c64.stale_tab_content"
           COMMAND win_c64_tabs_mutant_stale_tab_content)
  set_tests_properties(mutation.c64.stale_tab_content PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C64 MUTATION CAUGHT: stale_tab_content")

  # Defect re-injected: a failed switch keeps its (now useless) tab active.
  # "The failed switch keeps the original tab" MUST go red.
  add_executable(win_c64_tabs_mutant_keep_failed_tab win/test_c64_tabs.cpp)
  target_link_libraries(win_c64_tabs_mutant_keep_failed_tab PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
                        shell32.lib)
  target_compile_options(win_c64_tabs_mutant_keep_failed_tab PRIVATE /WX-)
  target_compile_definitions(win_c64_tabs_mutant_keep_failed_tab
                             PRIVATE PRISM_C64_MUTANT_FAILED_SWITCH_KEEPS_TAB=1)
  add_test(NAME "mutation.c64.keep_failed_tab"
           COMMAND win_c64_tabs_mutant_keep_failed_tab)
  set_tests_properties(mutation.c64.keep_failed_tab PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C64 MUTATION CAUGHT: keep_failed_tab")

  # The exe under test. It is the SHIPPED binary: no re-implementation of the
  # startup order, and the two fixtures come from the suite's own unique folder.
  add_executable(win_c64_launch win/test_c64_launch.cpp)
  target_link_libraries(win_c64_launch PRIVATE prism_core prism_win_bridge
                        user32.lib ole32.lib shell32.lib)
  target_compile_options(win_c64_launch PRIVATE /WX-)
  target_compile_definitions(win_c64_launch
                             PRIVATE "PRISM_C64_EXE=\"$<TARGET_FILE:PrismArchive>\"")
  add_dependencies(win_c64_launch PrismArchive)
  add_test(NAME "win.c64_launch" COMMAND win_c64_launch)
  set_tests_properties(win.c64_launch PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C64 LAUNCH CHECKS PASSED")
endif()
