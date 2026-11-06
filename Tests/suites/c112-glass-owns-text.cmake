# c112-glass-owns-text: the island owns the pill's content (fill + optics + TEXT).
#
# The defect: the island composes ABOVE the window and its backdrop is a capture
# of the window's own pixels -- which already contained the breadcrumb text. The
# optics blurred that text and painted the blur back over it: every glyph got a
# second, offset, blurred copy of itself ("重影"). C107's own suite could not see
# it: it asserted that the chain was live, never that the text stayed sharp.
#
# This suite measures the pill's TEXT, on the product's own paint path:
#   S1  sharpness retention >= 90% against the same text with the island off;
#   S2  no blur halo in the 1..3 px ring around the glyphs;
#   S3  the window paints nothing in the pill's text band any more.
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative control (CONTRACTS D3/D26/D27): the SAME source compiled with
# PRISM_C112_MUTANT_GLASS_OVER_TEXT -- the text stays on the window and the
# island is composed over it (the pre-C112 arm). S1 must go red; the entry is
# registered with PASS_REGULAR_EXPRESSION on the verdict, never with WILL_FAIL.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c112_glass_owns_text win/test_c112_glass_owns_text.cpp)
  target_link_libraries(win_c112_glass_owns_text PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c112_glass_owns_text PRIVATE /WX-)
  add_test(NAME "win.c112.glass_owns_text" COMMAND win_c112_glass_owns_text)
  set_tests_properties(win.c112.glass_owns_text PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C112 GLASS-OWNS-TEXT CHECKS PASSED")

  add_executable(win_c112_glass_owns_text_mutant_glass_over_text win/test_c112_glass_owns_text.cpp)
  target_link_libraries(win_c112_glass_owns_text_mutant_glass_over_text
                        PRIVATE prism_core prism_win_bridge
                                user32.lib gdi32.lib d2d1.lib dwrite.lib
                                d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib
                                windowscodecs.lib ole32.lib)
  target_compile_options(win_c112_glass_owns_text_mutant_glass_over_text PRIVATE /WX-)
  target_compile_definitions(win_c112_glass_owns_text_mutant_glass_over_text
                             PRIVATE PRISM_C112_MUTANT_GLASS_OVER_TEXT=1)
  add_test(NAME "mutation.c112.glass_over_text"
           COMMAND win_c112_glass_owns_text_mutant_glass_over_text)
  set_tests_properties("mutation.c112.glass_over_text" PROPERTIES
                       TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "C112 MUTATION CAUGHT: glass_over_text")
endif()
