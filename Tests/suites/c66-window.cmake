# c66-window: the window frame that was 16 px wider than the screen (card C66),
# plus the two patches this card wires (Help > About, Ctrl+F search).
#
# THE DEFECT, to the pixel: a 1536x816 work area, a setting of 1552x832.
# GetWindowRect includes the DWM invisible resize border (8 px per side), and a
# MAXIMIZED window's GetWindowRect is the work area inflated by exactly that
# border -- so "save GetWindowRect, restore it with SetWindowPos" both stores a
# rectangle no screen can hold and adds the border again on the way in. The last
# three action buttons (添加/删除/测试) end up off screen. That is the user's
# screenshot.
#
# TWO LAYERS, because the fix has two halves:
#   * core.window_frame      the policy as pure integer math (clamp, visible <->
#                            window rect, minimum size), so the Linux lane
#                            catches a regression in milliseconds;
#   * win.c66_window         a REAL window: the maximized rectangle measured,
#                            an oversized saved frame restored, five
#                            save/restore cycles, negative coordinates, the
#                            action island at 800/1280/1536 px, the Help > About
#                            menu opening its dialog through WM_COMMAND, and
#                            Ctrl+F filtering the list.
#
# VERDICT SHAPE (CONTRACTS D1 + D26 + D27 + D28)
#   Every target pins its stdout marker. The two negative controls do NOT use
#   WILL_FAIL: each binary arms mini_test's mutation verdict with the "injection
#   fired" predicate and prints "C66 MUTATION CAUGHT: <arm>" at the FIRST red
#   assertion, so a stale injection (suite still green) leaves the mutant entry
#   RED -- the guard proves itself. Arm names avoid clean/unarmed/control.
add_executable(test_window_frame test_window_frame.cpp)
target_link_libraries(test_window_frame PRIVATE prism_core)
add_test(NAME "core.window_frame" COMMAND test_window_frame)
set_tests_properties(core.window_frame PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL WINDOW-FRAME CHECKS PASSED")

# D3 control 1: the work-area clamp is compiled out. The oversized-frame and
# five-cycle assertions MUST go red; the marker is only printed when the clamp
# is proved NOT to clamp, so an unrelated red cannot claim CAUGHT.
add_executable(test_window_frame_mutant_no_workarea_clamp test_window_frame.cpp)
target_link_libraries(test_window_frame_mutant_no_workarea_clamp PRIVATE prism_core)
target_compile_definitions(test_window_frame_mutant_no_workarea_clamp
                           PRIVATE PRISM_C66_MUTANT_NO_WORKAREA_CLAMP=1)
add_test(NAME "c66.mutation.window_frame.no_workarea_clamp"
         COMMAND test_window_frame_mutant_no_workarea_clamp)
set_tests_properties(c66.mutation.window_frame.no_workarea_clamp PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C66 MUTATION CAUGHT: no_workarea_clamp")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # user32/gdi32/d2d1/dwrite/... are the same libraries every GalleryWindow
  # consumer names; dwmapi is what C66 adds (DWMWA_EXTENDED_FRAME_BOUNDS).
  set(PRISM_C66_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      dwmapi.lib windowscodecs.lib)

  add_executable(win_c66_window_test win/test_c66_window.cpp)
  target_link_libraries(win_c66_window_test PRIVATE ${PRISM_C66_WINDOW_LIBS})
  target_compile_options(win_c66_window_test PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c66_window" COMMAND win_c66_window_test)
  set_tests_properties(win.c66_window PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL WINDOW-RECT CHECKS PASSED")

  # D3 control 2: the DWM extended frame is not read, so the shadow border is
  # counted as part of the window again -- the pre-fix reading. The five-cycle
  # stability assertions MUST go red.
  add_executable(win_c66_window_mutant_no_dwm win/test_c66_window.cpp)
  target_link_libraries(win_c66_window_mutant_no_dwm PRIVATE ${PRISM_C66_WINDOW_LIBS})
  target_compile_options(win_c66_window_mutant_no_dwm PRIVATE /WX-)
  target_compile_definitions(win_c66_window_mutant_no_dwm PRIVATE PRISM_C66_MUTANT_NO_DWM=1)
  add_test(NAME "c66.mutation.window.no_dwm" COMMAND win_c66_window_mutant_no_dwm)
  set_tests_properties(c66.mutation.window.no_dwm PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C66 MUTATION CAUGHT: no_dwm")

  # D3 control 3: the clamp is compiled out on the WINDOW lane too, so the
  # "an oversized saved frame lands inside the work area" assertion is proved to
  # be the thing that catches the user's 1552x832.
  add_executable(win_c66_window_mutant_no_clamp win/test_c66_window.cpp)
  target_link_libraries(win_c66_window_mutant_no_clamp PRIVATE ${PRISM_C66_WINDOW_LIBS})
  target_compile_options(win_c66_window_mutant_no_clamp PRIVATE /WX-)
  target_compile_definitions(win_c66_window_mutant_no_clamp
                             PRIVATE PRISM_C66_MUTANT_NO_WORKAREA_CLAMP=1)
  add_test(NAME "c66.mutation.window.no_clamp" COMMAND win_c66_window_mutant_no_clamp)
  set_tests_properties(c66.mutation.window.no_clamp PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C66 MUTATION CAUGHT: no_workarea_clamp")

  # D3 control 4: the clamp is removed from the PERSIST path only. Restore still
  # clamps, so "the window came back inside the screen" stays green -- what goes
  # red is "the file never holds a frame bigger than the screen", which is the
  # invariant the CI runner's smaller desktop exposed (the first version of this
  # suite trusted the local desktop and went green there).
  add_executable(win_c66_window_mutant_no_persist_clamp win/test_c66_window.cpp)
  target_link_libraries(win_c66_window_mutant_no_persist_clamp PRIVATE ${PRISM_C66_WINDOW_LIBS})
  target_compile_options(win_c66_window_mutant_no_persist_clamp PRIVATE /WX-)
  target_compile_definitions(win_c66_window_mutant_no_persist_clamp
                             PRIVATE PRISM_C66_MUTANT_NO_PERSIST_CLAMP=1)
  add_test(NAME "c66.mutation.window.no_persist_clamp"
           COMMAND win_c66_window_mutant_no_persist_clamp)
  set_tests_properties(c66.mutation.window.no_persist_clamp PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C66 MUTATION CAUGHT: no_persist_clamp")

  # PROCESS LEVEL: the number the SHIPPED exe writes to settings.json. The card's
  # own verification method -- open, close, read the file, five times -- with
  # %LOCALAPPDATA% redirected so the user's real settings are untouched. A seeded
  # 1552x832 (the reported value) must come back as a frame that fits the work
  # area, and four more cycles must not move it by more than one pixel.
  add_executable(win_c66_launch win/test_c66_launch.cpp)
  target_link_libraries(win_c66_launch PRIVATE prism_core prism_win_bridge
                                                user32.lib shell32.lib)
  target_compile_options(win_c66_launch PRIVATE /WX-)
  target_compile_definitions(win_c66_launch
                             PRIVATE "PRISM_C66_EXE=\"$<TARGET_FILE:PrismArchive>\"")
  add_dependencies(win_c66_launch PrismArchive)
  add_test(NAME "win.c66_launch" COMMAND win_c66_launch)
  set_tests_properties(win.c66_launch PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL WINDOW-PERSIST CHECKS PASSED")
endif()
