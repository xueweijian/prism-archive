# c60-tabbar: THE TAB STRIP -- C59's TabSet drawn on a real HWND.
#
# The model (Src/PrismCore/TabSet.hpp) already answers every rule; this card is
# the band that shows it (Src/PrismWin/TabBar.hpp) plus the window wiring that
# turns clicks, Ctrl+Tab, middle-click and drags into TabSet calls. The suite
# drives the REAL window (Dispatch -> WndProc) and asserts pixels through a
# headless WIC target, so "the strip exists" is measured rather than assumed:
#
#   c60.tab_bar            the seven cases (see the header of the test)
#   c60.mutation.*         two D3 controls, same source, ONE rule removed
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). No WILL_FAIL anywhere: each
# binary arms prism::test::SetMutationVerdict with a RUNTIME predicate, prints
# "C60 MUTATION CAUGHT: <arm>" at the FIRST red assertion and exits 0, so CTest's
# PASS_REGULAR_EXPRESSION is the verdict. A stale injection (the mutated
# statement no longer fires) leaves the suite green and the target RED -- the
# marker is a conjunction, not a replacement (D26).
#
#   close_right_neighbor -> closing the active tab activates tab 0
#                           (PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR: C59's own
#                           switch, so the SAME constant the model reads);
#   dirty_no_confirm     -> a dirty tab closes without asking
#                           (PRISM_C60_MUTANT_DIRTY_NO_CONFIRM).
#
# ARM NAMES avoid clean / unarmed / control anywhere (D28). The unarmed control
# is c60.tab_bar: same source file, no injection define.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_tab_bar_test win/test_tab_bar.cpp)
  target_link_libraries(win_tab_bar_test PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  # SDK headers are not /W4-clean; the card's own code is.
  target_compile_options(win_tab_bar_test PRIVATE /WX-)
  add_test(NAME "c60.tab_bar" COMMAND win_tab_bar_test)
  # D1: the marker is a CONJUNCTION with the exit code; a window station that
  # cannot create the HWND returns 77 (a visible Skip, never a fake green).
  # D29: a real budget (the suite renders two 900x600 frames and one window).
  set_tests_properties(c60.tab_bar PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL TAB-BAR CHECKS PASSED")

  add_executable(win_tab_bar_mutant_close_right_neighbor win/test_tab_bar.cpp)
  target_link_libraries(win_tab_bar_mutant_close_right_neighbor PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
                        shell32.lib)
  target_compile_options(win_tab_bar_mutant_close_right_neighbor PRIVATE /WX-)
  target_compile_definitions(win_tab_bar_mutant_close_right_neighbor
                             PRIVATE PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR=1)
  add_test(NAME "c60.mutation.close_right_neighbor"
           COMMAND win_tab_bar_mutant_close_right_neighbor)
  set_tests_properties(c60.mutation.close_right_neighbor PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C60 MUTATION CAUGHT: close_first_tab")

  add_executable(win_tab_bar_mutant_dirty_no_confirm win/test_tab_bar.cpp)
  target_link_libraries(win_tab_bar_mutant_dirty_no_confirm PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
                        shell32.lib)
  target_compile_options(win_tab_bar_mutant_dirty_no_confirm PRIVATE /WX-)
  target_compile_definitions(win_tab_bar_mutant_dirty_no_confirm
                             PRIVATE PRISM_C60_MUTANT_DIRTY_NO_CONFIRM=1)
  add_test(NAME "c60.mutation.dirty_no_confirm"
           COMMAND win_tab_bar_mutant_dirty_no_confirm)
  set_tests_properties(c60.mutation.dirty_no_confirm PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C60 MUTATION CAUGHT: dirty_no_confirm")
endif()
