# c115-narrow-layout: the breadcrumb pill vs the action island at narrow widths.
#
# THE DEFECT: at 1028 px the toolbar sat 144 px inside the breadcrumb pill
# (pill 16..576, island 432..1012). The overlap is not born at 1028 -- it exists
# at every width below 1172 and is exactly (1172 - w) px. The full story, the
# measurements and the fix are in the suite's own header.
#
# WHAT IS REGISTERED
#   core.c115_narrow_layout          the real suite: a 900..1920 step-4 sweep in
#                                    which the pill and the island must never
#                                    intersect, the 1264/1482 baselines pinned
#                                    to the pixel, and the crumb text box pinned
#                                    inside the pill.
#   mutation.c115.ignore_narrow      the D3 control: PRISM_C115_MUTANT_IGNORE_NARROW
#                                    restores "the pill never yields", so the
#                                    sweep must go red. It prints
#                                    "C115 MUTATION CAUGHT: ignore_narrow" at
#                                    the first failure and exits 0, and the entry
#                                    decides with PASS_REGULAR_EXPRESSION --
#                                    never WILL_FAIL (CONTRACTS D26/D27).
#   win.c115_narrow_render           (Windows layer) the same two widths through
#                                    the REAL GalleryRenderer, writing
#                                    docs/assets/ui/c115-1028.png and
#                                    c115-1264.png, and asserting the pill the
#                                    renderer paints is the pill the layout
#                                    publishes.
#
# Everything pure here is integer geometry: no window, no D2D, no 7z.dll, so the
# Linux lane runs the sweep too.
add_executable(core_c115_narrow test_c115_narrow_layout.cpp)
target_link_libraries(core_c115_narrow PRIVATE prism_core)
add_test(NAME "core.c115_narrow_layout" COMMAND core_c115_narrow)
set_tests_properties(core.c115_narrow_layout PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C115 NARROW-LAYOUT CHECKS PASSED")

add_executable(core_c115_narrow_mutant_ignore_narrow test_c115_narrow_layout.cpp)
target_link_libraries(core_c115_narrow_mutant_ignore_narrow PRIVATE prism_core)
target_compile_definitions(core_c115_narrow_mutant_ignore_narrow
                           PRIVATE PRISM_C115_MUTANT_IGNORE_NARROW=1)
add_test(NAME "mutation.c115.ignore_narrow"
         COMMAND core_c115_narrow_mutant_ignore_narrow)
set_tests_properties(mutation.c115.ignore_narrow PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C115 MUTATION CAUGHT: ignore_narrow")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c115_narrow_render win/test_c115_narrow_render.cpp)
  target_link_libraries(win_c115_narrow_render PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c115_narrow_render PRIVATE /WX-)
  add_test(NAME "win.c115_narrow_render" COMMAND win_c115_narrow_render)
  set_tests_properties(win.c115_narrow_render PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C115 NARROW RENDER CHECKS PASSED")
endif()
