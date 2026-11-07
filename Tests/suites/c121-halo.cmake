# c121-halo: the refraction layer carries the PAGE, never the archive.
#
# THE DEFECT (docs/DESIGN-PROPOSAL-BANDIZIP.md 2.4 + 12.1; docs/design/C #1).
# C110 kept the decoded thumbnail in the C77 refraction source "because it is
# archive content", and C77-1 ("the layer must refract the items, over8 > 20000
# px") is the assertion that demanded it. A 1/8 sample of a photo, blurred 28 px
# and washed at 35 %, is a colour PLATE:
#     0.35 x (210,60,50) + 0.65 x (12,13,17) = (81,29,29)    model
#     measured in the 13 px gap between tiles = (81,29,28)   chroma 53 (7 with
#     the layer off), spread 46-48 lp = 4.4x the gap, 21 % of the content area.
# The same layer also moved the CANVAS: (16,17,23) -> (13,14,18), dE 2.4,
# because the Midnight veil was darker than the canvas it washes.
#
# WHAT THIS SUITE ASSERTS (real GalleryRenderer, headless WIC target, grid with
# saturated photo tiles -- the scene the audit measured):
#   H1  the chroma in the gap between two adjacent photo tiles is <= 10;
#   H2  toggling backdropRefraction moves the far canvas by dE <= 1;
#   H3  the chroma spread below a photo tile is <= 12 px;
#   H4  the cache key carries the THEME (a theme switch rebuilds the source, so
#       the second frame shows the second theme's canvas) and the SCROLL is not
#       an input (100 px later the ghost is the same bitmap, byte for byte);
#   H5  a page panel's edge fades into the canvas NEUTRALLY and within one blur
#       radius (+4/+4/+5 levels at 1 px, <= 1 by 32 px, 0 by 64 px, chroma flat):
#       the approved consequence of a page-scoped source, pinned so it cannot
#       grow back into a halo.
#
# NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): each arm is the shipped defect in the
# same source, registered as its own target, and the verdict is the entry's
# PASS_REGULAR_EXPRESSION -- never WILL_FAIL (D26 forbids combining them):
#   PRISM_C121_MUTANT_TILES_IN_GHOST   the decoded thumbnails go back into the
#       source -> H1/H3 MUST go red.
#   PRISM_C121_MUTANT_KEY_NO_THEME     the key drops the theme (the shipped key)
#       -> H4 MUST go red.
#   PRISM_C121_MUTANT_PANEL_GLOW       the blur widens from 28 to 64 px -> the
#       panel edges' approved NEUTRAL falloff runs past 32 px -> H5 MUST go red
#       ("a bounded neutral falloff" is approved behaviour; "a halo" is not).
#   PRISM_C121_MUTANT_EMPTY_GHOST      the source is emptied (compiled into the
#       C77 target below): C77-1's "the source carries the rail's panel" MUST go
#       red, which is what stops "fix the halo by deleting the layer".
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c121_halo win/test_c121_halo.cpp)
  target_link_libraries(win_c121_halo PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c121_halo PRIVATE /WX-)
  add_test(NAME "win.c121_halo" COMMAND win_c121_halo)
  set_tests_properties(win.c121_halo PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "ALL C121 HALO CHECKS PASSED")

  foreach(mutant IN ITEMS TILES_IN_GHOST KEY_NO_THEME PANEL_GLOW)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_c121_halo_mutant_${mutant_lc}" win/test_c121_halo.cpp)
    target_link_libraries("win_c121_halo_mutant_${mutant_lc}"
                          PRIVATE prism_core prism_win_bridge
                                  user32.lib gdi32.lib d2d1.lib dwrite.lib
                                  windowscodecs.lib ole32.lib)
    target_compile_options("win_c121_halo_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_c121_halo_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C121_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c121.${mutant_lc}"
             COMMAND "win_c121_halo_mutant_${mutant_lc}")
    # The verdict names the injection it caught; the regex IS the verdict, and
    # the renderer counts the injected branch so a red for another reason stays
    # red without a false CAUGHT.
    if(mutant STREQUAL "TILES_IN_GHOST")
      set_tests_properties("mutation.c121.${mutant_lc}" PROPERTIES TIMEOUT 240
                           PASS_REGULAR_EXPRESSION "C121 MUTATION CAUGHT: tiles-in-ghost")
    elseif(mutant STREQUAL "KEY_NO_THEME")
      set_tests_properties("mutation.c121.${mutant_lc}" PROPERTIES TIMEOUT 240
                           PASS_REGULAR_EXPRESSION "C121 MUTATION CAUGHT: key-no-theme")
    else()
      set_tests_properties("mutation.c121.${mutant_lc}" PROPERTIES TIMEOUT 240
                           PASS_REGULAR_EXPRESSION "C121 MUTATION CAUGHT: panel-glow")
    endif()
  endforeach()

  # The empty-ghost arm lives on the C77 suite's SOURCE: its assertion (1) is the
  # one that must notice an empty refraction source. Registered here because
  # C121 owns the arm; C77's own file keeps its two.
  add_executable(win_c77_glass_mutant_empty_ghost win/test_c77_glass.cpp)
  target_link_libraries(win_c77_glass_mutant_empty_ghost PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
  target_compile_options(win_c77_glass_mutant_empty_ghost PRIVATE /WX-)
  target_compile_definitions(win_c77_glass_mutant_empty_ghost
                             PRIVATE PRISM_C121_MUTANT_EMPTY_GHOST=1)
  add_test(NAME "mutation.c121.empty_ghost_breaks_c77_1"
           COMMAND win_c77_glass_mutant_empty_ghost)
  set_tests_properties(mutation.c121.empty_ghost_breaks_c77_1 PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "C77 MUTATION CAUGHT: empty-ghost")
endif()
