# c126-toolbar-glass: the liquid-glass island moves onto the flush toolbar and
# OWNS its content (card C126).
#
# THE DEFECT (C123's own measurement, win.c123_window): the breadcrumb pill was
# deleted, GlassPanelRect() returned 0x0, and the composition island, the C121
# refraction ghost and the refraction half of appearance.backdropRefraction all
# lost their consumer. Flipping the setting still moved 222,472 pixels of the
# frame -- but only 1,236 of them inside the toolbar band, because the band had
# no material of its own.
#
# WHAT IS REGISTERED
#   win.c126_toolbar_glass            G1..G6 on the REAL window + the REAL
#                                     composition path: ownership (0 window
#                                     buttons / 1 island frame), sharpness
#                                     parity + an empty window frame under the
#                                     island, a ghost with no toolbar ink, the
#                                     refraction delta (> C123's 1,236 in-band
#                                     pixels), C123's hit rectangles to the DIP,
#                                     and the theme reaching both the island and
#                                     the ghost.
#   mutation.c126.window_paints_toolbar
#                                     PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR:
#                                     the window keeps painting the band with
#                                     the island on top -- the C112 "重影",
#                                     restored. G1/G2 MUST go red.
#   mutation.c126.text_in_ghost       PRISM_C126_MUTANT_TEXT_IN_GHOST: the band's
#                                     ink is re-drawn into the refraction source.
#                                     G3 MUST go red.
#
# Both mutants print their own "C126 MUTATION CAUGHT: ..." line at the FIRST
# failing assertion and leave 0; the entries decide with PASS_REGULAR_EXPRESSION,
# never with WILL_FAIL (CONTRACTS D26/D27). Each verdict is armed on a runtime
# counter, so a red that came from somewhere else stays red WITHOUT a CAUGHT
# (CONTRACTS D3).
#
# D1: a window station that cannot create the HWND returns 77 (a visible Skip);
# a runner without DComp/optics prints "C126 SKIP: <reason>" and the marker still
# names the reason -- never a silent pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c126_toolbar_glass win/test_c126_toolbar_glass.cpp)
  target_link_libraries(win_c126_toolbar_glass PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib
                        shell32.lib)
  # SDK headers are not /W4-clean; the card's own code is.
  target_compile_options(win_c126_toolbar_glass PRIVATE /WX-)
  add_test(NAME "win.c126_toolbar_glass" COMMAND win_c126_toolbar_glass)
  set_tests_properties(win.c126_toolbar_glass PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C126 TOOLBAR-GLASS CHECKS PASSED")

  # --------------------------------------------------------------- D3 arms ---
  # The C112 arm is ALSO built against this suite: the two predicates answer the
  # same question ("does the island own the band?"), and a retired control must
  # not leave the newer assertions green.
  add_executable(win_c126_toolbar_glass_mutant_window_paints
                 win/test_c126_toolbar_glass.cpp)
  target_link_libraries(win_c126_toolbar_glass_mutant_window_paints PRIVATE
                        prism_core prism_win_bridge user32.lib gdi32.lib ole32.lib
                        d2d1.lib dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib
                        dcomp.lib d3dcompiler.lib shell32.lib)
  target_compile_options(win_c126_toolbar_glass_mutant_window_paints PRIVATE /WX-)
  target_compile_definitions(win_c126_toolbar_glass_mutant_window_paints
                             PRIVATE PRISM_C126_MUTANT_WINDOW_PAINTS_TOOLBAR=1)
  add_test(NAME "mutation.c126.window_paints_toolbar"
           COMMAND win_c126_toolbar_glass_mutant_window_paints)
  set_tests_properties(mutation.c126.window_paints_toolbar PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C126 MUTATION CAUGHT: window_paints_toolbar")

  add_executable(win_c126_toolbar_glass_mutant_text_in_ghost
                 win/test_c126_toolbar_glass.cpp)
  target_link_libraries(win_c126_toolbar_glass_mutant_text_in_ghost PRIVATE
                        prism_core prism_win_bridge user32.lib gdi32.lib ole32.lib
                        d2d1.lib dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib
                        dcomp.lib d3dcompiler.lib shell32.lib)
  target_compile_options(win_c126_toolbar_glass_mutant_text_in_ghost PRIVATE /WX-)
  target_compile_definitions(win_c126_toolbar_glass_mutant_text_in_ghost
                             PRIVATE PRISM_C126_MUTANT_TEXT_IN_GHOST=1)
  add_test(NAME "mutation.c126.text_in_ghost"
           COMMAND win_c126_toolbar_glass_mutant_text_in_ghost)
  set_tests_properties(mutation.c126.text_in_ghost PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C126 MUTATION CAUGHT: text_in_ghost")
endif()
