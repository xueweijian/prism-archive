# c101-ratio-bar: the compression-ratio strip is MUTED (card C101,
# ui-redesign-plan §2.2). The shipped bar was 6 px of full-alpha kind colour
# across the whole 130 px ratio column -- the user's "刺眼荧光绿厚条".
#
# Core arm (every platform): the band thresholds (<=50 % / 51..80 % / >80 %), the
# plan's geometry and alpha constants, and -- the load-bearing one -- the pixel
# ceilings on EVERY percent the bar can show: HSV S <= 0.45 and <= 60 levels
# above the canvas, with the band's hue still readable inside +-20 degrees. The
# last case composites the RAW palette at the same alpha and shows it FAILS the
# ceiling, which is what makes "just fade it" a red test instead of a taste.
#
# Windows arm: the REAL GalleryRenderer, rendered offscreen, measured through
# the paint-time read-back (LastRatioBar: 4 px / radius 2 / <= 56 px / alpha
# 0.55) and on PIXELS for all three bands.
#
# Negative control (CONTRACTS D3/D26/D27): the SAME source compiled with
# PRISM_C101_MUTANT_LOUD restores 6 px / alpha 1.0 / the full column / the
# file-kind colour. Its entry requires the CAUGHT marker, never WILL_FAIL (D26);
# a stale injection leaves the mutant entry red.
add_executable(core_c101_ratio_tint test_c101_ratio_tint.cpp)
target_link_libraries(core_c101_ratio_tint PRIVATE prism_core)
add_test(NAME "core.c101_ratio_tint" COMMAND core_c101_ratio_tint)
set_tests_properties(core.c101_ratio_tint PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C101 RATIO TINT CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C101_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)

  add_executable(win_c101_ratio_bar win/test_c101_ratio_bar.cpp)
  target_link_libraries(win_c101_ratio_bar PRIVATE ${PRISM_C101_LIBS})
  target_compile_options(win_c101_ratio_bar PRIVATE /WX-)
  add_test(NAME "win.c101_ratio_bar" COMMAND win_c101_ratio_bar)
  set_tests_properties(win.c101_ratio_bar PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C101 RATIO-BAR CHECKS PASSED")

  # D3 control: the shipped bar is back. Geometry, alpha and pixel assertions
  # MUST go red, and the arm writes the BEFORE screenshot.
  add_executable(win_c101_ratio_bar_loud win/test_c101_ratio_bar.cpp)
  target_link_libraries(win_c101_ratio_bar_loud PRIVATE ${PRISM_C101_LIBS})
  target_compile_options(win_c101_ratio_bar_loud PRIVATE /WX-)
  target_compile_definitions(win_c101_ratio_bar_loud PRIVATE PRISM_C101_MUTANT_LOUD=1)
  add_test(NAME "mutation.c101.loud" COMMAND win_c101_ratio_bar_loud)
  set_tests_properties(mutation.c101.loud PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C101 MUTATION CAUGHT: loud")
endif()
