# c67-interaction: drag OUT of the archive, Alt+double-click, multi-select.
#
# THREE decisions, each proven at the layer that OWNS it:
#   * the multi-selection rules + the drag-out gate are pure data
#     (Src/PrismCore/SelectionModel.hpp, Src/PrismCore/DragOutPlan.hpp) and run
#     on every lane;
#   * the CF_HDROP data object, the IDropSource and the temp-directory cleanup
#     are Windows code (Src/PrismWin/DragOutToShell.hpp) and are proven against
#     a REAL 7z.dll (win.c67_dragout);
#   * the gestures (Alt+double-click, Ctrl/Shift/Ctrl+A, batch extraction of the
#     selection only) are proven on a REAL HWND (win.c67_gestures).
#
# NEGATIVE CONTROLS (D3/D26/D27). Every mutant prints "C67 MUTATION CAUGHT: <arm>"
# AT the first failure (mini_test's verdict, armed below the guard in the test
# TU) and the test's PASS_REGULAR_EXPRESSION is that marker -- no WILL_FAIL is
# stacked on a regex anywhere in this file (D26).
add_executable(core_selection_model_test test_selection_model.cpp)
target_link_libraries(core_selection_model_test PRIVATE prism_core)
add_test(NAME "core.selection_model" COMMAND core_selection_model_test)
set_tests_properties(core.selection_model PROPERTIES TIMEOUT 60)

add_executable(core_dragout_plan_test test_dragout_plan.cpp)
target_link_libraries(core_dragout_plan_test PRIVATE prism_core)
add_test(NAME "core.dragout_plan" COMMAND core_dragout_plan_test)
set_tests_properties(core.dragout_plan PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL DRAG-OUT-GATE CHECKS PASSED")

# Arm 1: hovering materialises the payload (the "drag past a window => %TEMP%
# full of leftovers" bug). Arm name avoids clean/unarmed/control (D28).
add_executable(core_dragout_plan_mutant_hover_extracts test_dragout_plan.cpp)
target_link_libraries(core_dragout_plan_mutant_hover_extracts PRIVATE prism_core)
target_compile_definitions(core_dragout_plan_mutant_hover_extracts
                           PRIVATE PRISM_C67_MUTANT_EAGER_MATERIALIZE=1)
add_test(NAME "mutation.c67.hover_extracts"
         COMMAND core_dragout_plan_mutant_hover_extracts)
set_tests_properties(mutation.c67.hover_extracts PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C67 MUTATION CAUGHT: eager_materialize")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # A real 7z.dll + 7z.exe author a real archive; the CF_HDROP payload is read
  # back through DragQueryFileW exactly like a drop target reads it.
  add_executable(win_c67_dragout_test win/test_c67_dragout.cpp)
  target_link_libraries(win_c67_dragout_test PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib
                        oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_c67_dragout_test PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c67_dragout" COMMAND win_c67_dragout_test)
  set_tests_properties(win.c67_dragout PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C67 DRAG-OUT CHECKS PASSED")

  # Real HWND: Alt+double-click, Ctrl/Shift/Ctrl+A and batch extraction.
  add_executable(win_c67_gestures_test win/test_c67_gestures.cpp)
  target_link_libraries(win_c67_gestures_test PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib
                        d2d1.lib dwrite.lib windowscodecs.lib bcrypt.lib WindowsApp.lib)
  target_compile_options(win_c67_gestures_test PRIVATE /WX-)
  add_test(NAME "win.c67_gestures" COMMAND win_c67_gestures_test)
  set_tests_properties(win.c67_gestures PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C67 GESTURE CHECKS PASSED")

  # Arm 2: the Alt modifier is ignored, so Alt+double-click opens the file
  # instead of extracting it.
  add_executable(win_c67_gestures_mutant_alt_opens win/test_c67_gestures.cpp)
  target_link_libraries(win_c67_gestures_mutant_alt_opens PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        shell32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        bcrypt.lib WindowsApp.lib)
  target_compile_options(win_c67_gestures_mutant_alt_opens PRIVATE /WX-)
  target_compile_definitions(win_c67_gestures_mutant_alt_opens
                             PRIVATE PRISM_C67_MUTANT_IGNORE_ALT=1)
  add_test(NAME "mutation.c67.alt_opens"
           COMMAND win_c67_gestures_mutant_alt_opens)
  set_tests_properties(mutation.c67.alt_opens PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C67 MUTATION CAUGHT: alt_opens")

  # Arm 3 (the C66 half-band): the painter skips the LAST button, so the
  # geometry says eight while the screen shows seven. The band test's
  # "painted == buttons" assertion must go red.
  add_executable(win_c67_gestures_mutant_skip_last_paint win/test_c67_gestures.cpp)
  target_link_libraries(win_c67_gestures_mutant_skip_last_paint PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        shell32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        bcrypt.lib WindowsApp.lib)
  target_compile_options(win_c67_gestures_mutant_skip_last_paint PRIVATE /WX-)
  target_compile_definitions(win_c67_gestures_mutant_skip_last_paint
                             PRIVATE PRISM_C67_MUTANT_SKIP_LAST_PAINT=1)
  add_test(NAME "mutation.c67.skip_last_paint"
           COMMAND win_c67_gestures_mutant_skip_last_paint)
  set_tests_properties(mutation.c67.skip_last_paint PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C67 MUTATION CAUGHT: skip_last_paint")

  # Arm 4: "extract to..." hands the WHOLE archive to the engine instead of the
  # selection, so the extracted-file count assertion must go red.
  add_executable(win_c67_gestures_mutant_extract_all win/test_c67_gestures.cpp)
  target_link_libraries(win_c67_gestures_mutant_extract_all PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        shell32.lib ole32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        bcrypt.lib WindowsApp.lib)
  target_compile_options(win_c67_gestures_mutant_extract_all PRIVATE /WX-)
  target_compile_definitions(win_c67_gestures_mutant_extract_all
                             PRIVATE PRISM_C67_MUTANT_EXTRACT_ALL=1)
  add_test(NAME "mutation.c67.extract_all"
           COMMAND win_c67_gestures_mutant_extract_all)
  set_tests_properties(mutation.c67.extract_all PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C67 MUTATION CAUGHT: extract_all")
endif()
