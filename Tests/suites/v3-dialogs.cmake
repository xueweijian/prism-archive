# v3-dialogs: the Liquid Glass shell over C18/C31 (blueprint ch.3, all four
# states, DPI at 96/144/192).
#
# ONE source file, THREE executables:
#   win.dialog_visual                 D2D/WIC pixel assertions + real-window
#                                     keyboard assertions (SKIP 77 + stdout
#                                     marker; CONTRACTS D1).
#   mutation.v3_dialogs.no_hover      hover folded into Normal -> RED.
#   mutation.v3_dialogs.fixed_dpi     every DPI folded into 96 -> RED.
#
# CONTRACTS D26: the two mutants are WILL_FAIL ONLY. No
# PASS_REGULAR_EXPRESSION is placed on the same test; the marker they print is
# evidence, not the verdict.
set(PRISM_V3_WIN_LIBS user32.lib gdi32.lib ole32.lib shell32.lib comctl32.lib
    d2d1.lib dwrite.lib windowscodecs.lib)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_dialog_visual win/test_dialog_visual.cpp)
  target_link_libraries(win_dialog_visual PRIVATE prism_core prism_win_bridge
                        ${PRISM_V3_WIN_LIBS})
  target_compile_options(win_dialog_visual PRIVATE /WX-)
  add_test(NAME "win.dialog_visual" COMMAND win_dialog_visual)
  set_tests_properties(win.dialog_visual PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL DIALOG-VISUAL CHECKS PASSED")

  foreach(mutant IN ITEMS NO_HOVER FIXED_DPI)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_dialog_visual_mutant_${mutant_lc}" win/test_dialog_visual.cpp)
    target_link_libraries("win_dialog_visual_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge ${PRISM_V3_WIN_LIBS})
    target_compile_options("win_dialog_visual_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_dialog_visual_mutant_${mutant_lc}"
                               PRIVATE "PRISM_V3_MUTANT_${mutant}=1")
    add_test(NAME "mutation.v3_dialogs.${mutant_lc}"
             COMMAND "win_dialog_visual_mutant_${mutant_lc}")
    set_tests_properties("mutation.v3_dialogs.${mutant_lc}" PROPERTIES WILL_FAIL TRUE
                         TIMEOUT 180)
  endforeach()
endif()
