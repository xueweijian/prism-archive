# c53-wiring: the three delivered patches are ACTUALLY USED by the app.
#
# C52 shipped the drop model + the window's half and left execution to the app;
# C31's palette overlay and settings menu were delivered as patches nobody
# applied. This suite drives the PRODUCTION executor (AppWiring's
# InstallDropHandler) and the production window, on a real HWND with a real
# hand-built DROPFILES blob, and proves the user-visible consequences:
#   1. a .7z dropped into an empty window reaches the archive-changed flow;
#   2. Ctrl+K opens the overlay and the same chord closes it;
#   3. the menu's Settings item opens a real dialog and Save changes the file;
#   4. a drop and the palette never share the surface (ONE modal overlay);
#   5. AddToArchive refuses explicitly ("append not supported") and writes NOTHING.
#
# NEGATIVE CONTROLS (D3 + D26 + D27): the same source is compiled twice more, each
# with ONE rule compiled out. The mutant prints a banner (proof the injection
# compiled in) and mini_test prints the CAUGHT marker at the FIRST failure and
# leaves with 0 immediately. The marker is the target's PASS_REGULAR_EXPRESSION and
# NO WILL_FAIL is stacked on it (D26). If an injection ever stopped applying the
# suite would stay green, the marker would be absent and the target would fail --
# the guard proves itself. Arm names avoid clean/unarmed/control (D28).
#
# D1: a session with no window station exits 77 (SKIP_RETURN_CODE) after printing
# WHY; the stdout marker stays required, so a skip can never look like a pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_wiring_glue win/test_wiring_glue.cpp)
  target_link_libraries(win_wiring_glue PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib
                        oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
                        comctl32.lib comdlg32.lib shlwapi.lib WindowsApp.lib)
  target_compile_options(win_wiring_glue PRIVATE /WX-)
  add_test(NAME "win.wiring_glue" COMMAND win_wiring_glue)
  set_tests_properties(win.wiring_glue PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C53-WIRING CHECKS PASSED")

  # Mutant 1: AddToArchive "succeeds" without a writer. (5) must go red.
  add_executable(win_wiring_glue_mutant_fake_add win/test_wiring_glue.cpp)
  target_link_libraries(win_wiring_glue_mutant_fake_add PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        shell32.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib
                        dwrite.lib bcrypt.lib comctl32.lib comdlg32.lib shlwapi.lib
                        WindowsApp.lib)
  target_compile_options(win_wiring_glue_mutant_fake_add PRIVATE /WX-)
  target_compile_definitions(win_wiring_glue_mutant_fake_add
                             PRIVATE PRISM_C53_MUTANT_FAKE_ADD=1)
  add_test(NAME "mutation.wiring.fake_add"
           COMMAND win_wiring_glue_mutant_fake_add)
  set_tests_properties(mutation.wiring.fake_add PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C53 MUTATION CAUGHT: fake_add")

  # Mutant 2: the one-overlay rule is compiled out. (4) must go red.
  add_executable(win_wiring_glue_mutant_multi_overlay win/test_wiring_glue.cpp)
  target_link_libraries(win_wiring_glue_mutant_multi_overlay PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        shell32.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib
                        dwrite.lib bcrypt.lib comctl32.lib comdlg32.lib shlwapi.lib
                        WindowsApp.lib)
  target_compile_options(win_wiring_glue_mutant_multi_overlay PRIVATE /WX-)
  target_compile_definitions(win_wiring_glue_mutant_multi_overlay
                             PRIVATE PRISM_C53_MUTANT_MULTI_OVERLAY=1)
  add_test(NAME "mutation.wiring.multi_overlay"
           COMMAND win_wiring_glue_mutant_multi_overlay)
  set_tests_properties(mutation.wiring.multi_overlay PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C53 MUTATION CAUGHT: multi_overlay")
endif()
