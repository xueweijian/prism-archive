# c78-actionisland: the action island's BUTTON layer (Liquid Glass v2).
#
# One executable, five compile-time negative controls (CONTRACTS D3/D26/D27).
# The clean build asserts, on rendered pixels:
#   * 24 px glyph / 11 px label / 6 px above and below the content stack;
#   * hover lifts the SAME button by 3-8 luminance levels, achromatically;
#   * pressing sinks BOTH the icon and the label by exactly one pixel;
#   * the Group seams recover to alpha 0.06-0.10 on all three C15 themes, and
#     an intra-group gap carries no seam;
#   * a disabled label measures < 3:1 contrast (an enabled one > 4.5:1);
#   * the keyboard focus ring is 2 px and within 6 levels of the accent token's
#     composited colour;
#   * the dropdown chevron appears only for a hovered menu action.
#
# Each mutant makes exactly ONE of those measurements fail. Its CTest entry has
# a PASS_REGULAR_EXPRESSION on the verdict line and NO WILL_FAIL: the suite
# prints "C78 MUTATION CAUGHT: <arm>" at the FIRST failed assertion and exits 0
# (D27), so a build where the injection stopped compiling is RED, not green.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c78_action_island win/test_c78_action_island.cpp)
  target_link_libraries(win_c78_action_island PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c78_action_island PRIVATE /WX-)
  add_test(NAME "win.c78_action_island" COMMAND win_c78_action_island)
  set_tests_properties(win.c78_action_island PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C78 ACTION ISLAND CHECKS PASSED")

  set(C78_MUTANT_ARMS no_group_separator no_focus_ring no_arrow strong_hover
      disabled_bright)
  foreach(arm IN LISTS C78_MUTANT_ARMS)
    string(TOUPPER "${arm}" C78_ARM_UPPER)
    add_executable(win_c78_${arm} win/test_c78_action_island.cpp)
    target_link_libraries(win_c78_${arm} PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                          ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
    target_compile_options(win_c78_${arm} PRIVATE /WX-)
    target_compile_definitions(win_c78_${arm}
                               PRIVATE PRISM_C78_MUTANT_${C78_ARM_UPPER}=1)
    add_test(NAME "mutation.c78.${arm}" COMMAND win_c78_${arm})
    set_tests_properties(mutation.c78.${arm} PROPERTIES TIMEOUT 300
        PASS_REGULAR_EXPRESSION "C78 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
