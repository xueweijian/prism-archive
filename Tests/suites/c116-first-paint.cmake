# c116-first-paint: the frame that carries the data (card C116).
#
# THE REPORTED DEFECT. After opening demo.zip the window shows the caption and
# the (native) menu bar over an empty client area, while the accessibility tree
# lists the whole model. The hypothesis was "the first frame was painted before
# the scan landed and nothing ever asked for a second one".
#
# WHAT THIS SUITE ASSERTS (deterministic; a real GalleryWindow, a real D2D
# target, the SAME RenderSceneInto the window's WM_PAINT calls):
#   1. a catalog that arrives after frame 1 leaves the window DIRTY -- asked of
#      the platform (GetUpdateRect), not of a counter the window keeps;
#   2. the frame rendered after the data differs from the pre-data frame across
#      the content band by thousands of pixels;
#   3. a second catalog rebuilds the scene and dirties the window again.
#
# NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C116_MUTANT_NO_REPAINT closes
# GalleryWindow::Invalidate() -- the one door every repaint request goes through.
# The scene still rebuilds, no frame is ever requested. The dirty assertion must go red, the
# mutant prints "C116 MUTATION CAUGHT: no_repaint" at the FIRST failure, and the
# marker is the target's PASS_REGULAR_EXPRESSION -- no WILL_FAIL is stacked on
# it (D26). A stale injection leaves the marker absent and the target red.
#
# D1: a session with no window station exits 77 after printing WHY, and the
# stdout marker stays required, so a skip can never look like a pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c116_first_paint win/test_c116_first_paint.cpp)
  target_link_libraries(win_c116_first_paint PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib)
  target_compile_options(win_c116_first_paint PRIVATE /WX-)
  add_test(NAME "win.c116_first_paint" COMMAND win_c116_first_paint)
  set_tests_properties(win.c116_first_paint PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C116 FIRST-PAINT CHECKS PASSED")

  # The pre-fix behaviour in one binary: the scene is rebuilt and the window is
  # never dirtied, so the frame on screen stays the pre-data one.
  add_executable(win_c116_first_paint_mutant_no_repaint win/test_c116_first_paint.cpp)
  target_link_libraries(win_c116_first_paint_mutant_no_repaint PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib ole32.lib)
  target_compile_options(win_c116_first_paint_mutant_no_repaint PRIVATE /WX-)
  target_compile_definitions(win_c116_first_paint_mutant_no_repaint
                             PRIVATE PRISM_C116_MUTANT_NO_REPAINT=1)
  add_test(NAME "mutation.c116.no_repaint"
           COMMAND win_c116_first_paint_mutant_no_repaint)
  set_tests_properties(mutation.c116.no_repaint PROPERTIES TIMEOUT 180
                         SKIP_RETURN_CODE 77
                         PASS_REGULAR_EXPRESSION "C116 MUTATION CAUGHT: no_repaint")
endif()
