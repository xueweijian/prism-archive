# c77-glass: the BACKDROP LAYER and the GLASS PANEL (DESIGN.md 2.1).
#
# The user's report was "the interface is nowhere near Liquid Glass". Two things
# were missing and this card owns both:
#   * a refraction LAYER under the content (the items at 1/8 resolution, blurred
#     28 px, desaturated to ~0.35) -- without it a translucent panel is grey;
#   * a PANEL LANGUAGE (fill .45-.55 + top 1 px specular rim + drop shadow
#     blur 24 / offset y 6 / alpha .18 + squircle 14) shared by the breadcrumb
#     island, the action island's base, the rail's base and Quick Look.
#
# The suite renders the REAL GalleryRenderer into a headless WIC target and
# asserts PIXELS: the same coordinate differs by > 8 with the layer on, the top
# 1 px of a panel is >= 12 levels brighter than its body, the band 6-10 px
# outside a panel is 3-10 levels darker, the four corner pixels are untouched,
# and the layer is rebuilt on size/content change but never on a scroll.
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative controls (CONTRACTS D3/D26/D27): the SAME source compiled with one
# rule broken. The mutant entries use PASS_REGULAR_EXPRESSION on the verdict --
# NOT WILL_FAIL (D26 forbids combining them) -- and the verdict is a CONJUNCTION:
# it is printed at the FIRST failing check and only when the injected branch
# actually ran. A clean build never prints it, so an injection that silently
# stopped matching turns its registration red instead of green.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c77_glass win/test_c77_glass.cpp)
  target_link_libraries(win_c77_glass PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c77_glass PRIVATE /WX-)
  add_test(NAME "win.c77_glass" COMMAND win_c77_glass)
  set_tests_properties(win.c77_glass PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "ALL C77 GLASS CHECKS PASSED")

  foreach(mutant IN ITEMS NO_BACKDROP NO_SHADOW)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_c77_glass_mutant_${mutant_lc}" win/test_c77_glass.cpp)
    target_link_libraries("win_c77_glass_mutant_${mutant_lc}"
                          PRIVATE prism_core prism_win_bridge
                                  user32.lib gdi32.lib d2d1.lib dwrite.lib
                                  windowscodecs.lib ole32.lib)
    target_compile_options("win_c77_glass_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_c77_glass_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C77_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c77_glass.${mutant_lc}"
             COMMAND "win_c77_glass_mutant_${mutant_lc}")
    # The verdict names the injection it caught; the regex is the verdict.
    set_tests_properties("mutation.c77_glass.${mutant_lc}" PROPERTIES
                         TIMEOUT 240
                         PASS_REGULAR_EXPRESSION "C77 MUTATION CAUGHT: no-")
  endforeach()
endif()
