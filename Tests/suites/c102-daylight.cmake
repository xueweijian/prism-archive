# c102-daylight: DAYLIGHT LAYERED MATERIAL + ISLAND CONTRAST (card C102).
#
# THE MEASURED DEFECT (ui-redesign-plan 2.1). The Daylight canvas presented as
# (247,249,252) and the floating island as (252,252,254): five levels apart, so
# the island had no body at all -- what the user saw was "a plastic pill on a
# washed-out white page", rim and shadow only.
#
# THE PLAN'S THREE LAYERS: canvas #F5F5F7, content plates #FFFFFF (kept),
# floating island rgba(255,255,255,.82).
#
# TWO LAYERS, because the fix has two halves:
#   core.c102_daylight   the numbers, through prism::chrome's ONE theme entry:
#                        Day canvas (245,245,247) +-2, island - canvas >= 8
#                        levels, Night (13,14,18) and HighContrast (0,0,0)
#                        rows bit-for-bit unchanged (the regression arm),
#                        HighContrast text still >= 4.5:1, the content plate
#                        still #FFFFFF;
#   win.c102_daylight    the renderer's OWN pixels: the same assertions on a
#                        real GalleryRenderer::Draw frame, plus the card's two
#                        screenshots -- docs/assets/ui/c102-day.png and
#                        c102-night.png are written from the frames this suite
#                        just measured, so a stale picture cannot survive a
#                        green run.
#
# NEGATIVE CONTROL (CONTRACTS D3 + D26 + D27): PRISM_C102_MUTANT_FLAT puts the
# Daylight island alpha back to the pre-C102 .58 on EffectiveGlassSurface() (the
# one path every painter of "glass" uses). The island/canvas assertion MUST go
# red, and the CTest entry requires the D27 marker so a stale injection (suite
# still green) leaves the mutant entry RED.
add_executable(core_c102_daylight test_c102_daylight.cpp)
target_link_libraries(core_c102_daylight PRIVATE prism_core)
add_test(NAME "core.c102_daylight" COMMAND core_c102_daylight)
set_tests_properties(core.c102_daylight PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C102 DAYLIGHT CHECKS PASSED")

add_executable(core_c102_daylight_mutant_flat test_c102_daylight.cpp)
target_link_libraries(core_c102_daylight_mutant_flat PRIVATE prism_core)
target_compile_definitions(core_c102_daylight_mutant_flat PRIVATE PRISM_C102_MUTANT_FLAT=1)
add_test(NAME "c102.mutation.daylight.flat"
         COMMAND core_c102_daylight_mutant_flat)
set_tests_properties(c102.mutation.daylight.flat PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C102 MUTATION CAUGHT: flat")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # d2d1/dwrite/windowscodecs: the offscreen WIC render target every pixel case
  # reads back from AND the PNG the card ships. dwmapi/user32: window wiring.
  set(PRISM_C102_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      dwmapi.lib windowscodecs.lib)

  add_executable(win_c102_daylight_test win/test_c102_daylight.cpp)
  target_link_libraries(win_c102_daylight_test PRIVATE ${PRISM_C102_WINDOW_LIBS})
  target_compile_options(win_c102_daylight_test PRIVATE /WX-)  # B9: SDK headers
  # The screenshots ARE the frames this suite measures; they are written only
  # when an output directory is passed on the command line (see the file header),
  # so the CTest entry below asserts without touching the source tree.
  add_test(NAME "win.c102_daylight" COMMAND win_c102_daylight_test)
  set_tests_properties(win.c102_daylight PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C102 RENDER CHECKS PASSED")

  # D3 control: the island goes back to .58. The level assertion MUST go red on
  # the daylight frame.
  add_executable(win_c102_daylight_mutant_flat win/test_c102_daylight.cpp)
  target_link_libraries(win_c102_daylight_mutant_flat PRIVATE ${PRISM_C102_WINDOW_LIBS})
  target_compile_options(win_c102_daylight_mutant_flat PRIVATE /WX-)
  target_compile_definitions(win_c102_daylight_mutant_flat PRIVATE
      PRISM_C102_MUTANT_FLAT=1)
  add_test(NAME "c102.mutation.daylight.render_flat"
           COMMAND win_c102_daylight_mutant_flat)
  set_tests_properties(c102.mutation.daylight.render_flat PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C102 MUTATION CAUGHT: flat")
endif()
