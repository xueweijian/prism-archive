# c30-inlinepreview: the right-click inline preview companion flyout
# (blueprint ch. 4.1). One card = one file; see suites/README.md.
#
# What is registered here
#   * win.companion_preview       the real suite: IPC packet contract, cross-
#                                 thread AND cross-process round trip, the
#                                 no-reader / wedged-reader degradation paths,
#                                 pure placement geometry, and the real HWND's
#                                 no-activate flags + close paths.
#   * mutation.inlinepreview.*    three D3 controls. Each compiles the SAME
#                                 source with exactly one rule broken, and each
#                                 is registered WILL_FAIL with NO
#                                 PASS_REGULAR_EXPRESSION (D26: those two
#                                 properties must never be stacked -- the regex
#                                 would turn the expected failure into a pass).
#
# Everything here runs without Explorer, without a user and without 7z.dll.
# The visual result on a real desktop is a MANUAL acceptance item in the PR.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_companion_preview_test win/test_companion_preview.cpp)
  target_link_libraries(win_companion_preview_test PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (contract B9).
  target_compile_options(win_companion_preview_test PRIVATE /WX-)
  add_test(NAME "win.companion_preview" COMMAND win_companion_preview_test)
  # D1: the stdout marker is a CONJUNCTION with exit 0, and the only honoured
  # skip is exit 77 (a window station that refuses a window), which prints its
  # reason -- a silent skip can never look green.
  set_tests_properties(win.companion_preview PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL COMPANION-PREVIEW CHECKS PASSED"
      SKIP_RETURN_CODE 77)

  # -------------------------------------------------------------------------
  # D3 negative controls. The source prints "C30 MUTATION ACTIVE: ..." when an
  # injection is compiled in, so a red run names the rule it caught.
  # -------------------------------------------------------------------------
  # 1. The right-edge FLIP is removed: the flyout runs off the work area.
  add_executable(win_companion_preview_mutant_no_flip win/test_companion_preview.cpp)
  target_link_libraries(win_companion_preview_mutant_no_flip PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib)
  target_compile_options(win_companion_preview_mutant_no_flip PRIVATE /WX-)
  target_compile_definitions(win_companion_preview_mutant_no_flip
                             PRIVATE PRISM_C30_MUTANT_NO_FLIP=1)
  add_test(NAME "mutation.inlinepreview.no_flip"
           COMMAND win_companion_preview_mutant_no_flip)
  set_tests_properties(mutation.inlinepreview.no_flip PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

  # 2. WS_EX_NOACTIVATE is dropped from the window AND MA_ACTIVATE is returned:
  #    the flyout steals focus from Explorer.
  add_executable(win_companion_preview_mutant_drop_noactivate win/test_companion_preview.cpp)
  target_link_libraries(win_companion_preview_mutant_drop_noactivate PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib)
  target_compile_options(win_companion_preview_mutant_drop_noactivate PRIVATE /WX-)
  target_compile_definitions(win_companion_preview_mutant_drop_noactivate
                             PRIVATE PRISM_C30_MUTANT_DROP_NOACTIVATE=1)
  add_test(NAME "mutation.inlinepreview.drop_noactivate"
           COMMAND win_companion_preview_mutant_drop_noactivate)
  set_tests_properties(mutation.inlinepreview.drop_noactivate PROPERTIES WILL_FAIL TRUE
      TIMEOUT 300)

  # 3. The ack budget becomes INFINITE: a wedged companion would hang explorer.
  add_executable(win_companion_preview_mutant_ignore_timeout win/test_companion_preview.cpp)
  target_link_libraries(win_companion_preview_mutant_ignore_timeout PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib)
  target_compile_options(win_companion_preview_mutant_ignore_timeout PRIVATE /WX-)
  target_compile_definitions(win_companion_preview_mutant_ignore_timeout
                             PRIVATE PRISM_C30_MUTANT_IGNORE_TIMEOUT=1)
  add_test(NAME "mutation.inlinepreview.ignore_timeout"
           COMMAND win_companion_preview_mutant_ignore_timeout)
  set_tests_properties(mutation.inlinepreview.ignore_timeout PROPERTIES WILL_FAIL TRUE
      TIMEOUT 300)
endif()
