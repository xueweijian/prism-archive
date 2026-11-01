# v4-polish: the five defects the v2 screenshot still showed (card V4).
#
# ONE card = ONE file (Tests/suites/README.md): Tests/CMakeLists.txt is not
# touched, and the glob in it picks this file up.
#
# What is registered here
#   * win.visual_polish          the suite. It asserts RECTANGLES, COUNTS and
#                               PIXELS, in this order:
#                                 - a tile-less item is a 32 px BAND spanning
#                                   content.left .. content.right, and adjacent
#                                   bands are contiguous (the "220 px capsule in
#                                   a grid cell" defect);
#                                 - the grid holds ONLY directories and images
#                                   whose thumbnail really exists;
#                                 - 1200x800 list mode shows >= 18 rows at a
#                                   32 px pitch (the layout yields 19);
#                                 - the icon box centre is the label box centre
#                                   minus one fixed offset, the label box is at
#                                   least 64 px wide, the format carries the
#                                   U+2026 ellipsis and no shipping label is
#                                   truncated (measured with DirectWrite);
#                                 - the rail is CHROME: a grid frame reserves
#                                   220 px, an explicit collapse reserves 0 and
#                                   moves the content left by exactly 220 (the
#                                   REAL window is asked, not a model);
#                                 - the status band paints 加密/方法/字典/压缩率
#                                   in the four DESIGN states, with ink inside
#                                   every segment (it used to be an empty strip).
#   * mutation.visual_polish.*   two D27 controls. Each compiles the SAME source
#                               with exactly ONE rule broken and prints an
#                               injection banner. The verdict marker is printed
#                               AT THE FIRST FAILED ASSERTION by
#                               SetMutationVerdict(prefix, predicate) -- with the
#                               runtime predicate, so a mutant that went red for
#                               an unrelated reason does NOT earn a CAUGHT line
#                               (CONTRACTS D3).
#
# CONTRACTS D26: no WILL_FAIL anywhere in this file. WILL_FAIL cannot be
# combined with a pass marker (it inverts the verdict) and it turns a SKIP's
# exit 0 into a false green. D1: a machine with no WIC or no window station
# prints WHY and exits 77 (SKIP_RETURN_CODE); the stdout marker is required all
# the same, so a silent skip can never count as a pass.
#
# /WX-: the Windows SDK headers are not /W4 clean (same rationale as every other
# Windows target here).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_visual_polish win/test_visual_polish.cpp)
  target_link_libraries(win_visual_polish PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib
                        d2d1.lib dwrite.lib)
  target_compile_options(win_visual_polish PRIVATE /WX-)
  add_test(NAME "win.visual_polish" COMMAND win_visual_polish)
  set_tests_properties(win.visual_polish PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL VISUAL-POLISH CHECKS PASSED")

  # ---------------------------------------------------------------------------
  # D3 negative controls.
  #   cell_row        -- every tile-less item keeps its 220 px CELL (V1's shape).
  #                      The band/contiguity assertions must go red.
  #   grid-no-sidebar -- the rail is list-mode chrome again; a grid frame
  #                      reserves 0 px. The "the grid reserves the rail"
  #                      assertions (model AND real window) must go red.
  # The binary prints the dashed id, so the marker below is the proof the FAIL
  # came from the injection (D27) and not from an unrelated break.
  # ---------------------------------------------------------------------------
  foreach(mutant IN ITEMS CELL_ROW GRID_NO_SIDEBAR)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    add_executable("win_visual_polish_mutant_${mutant_lc}" win/test_visual_polish.cpp)
    target_link_libraries("win_visual_polish_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib ole32.lib
                          windowscodecs.lib d2d1.lib dwrite.lib)
    target_compile_options("win_visual_polish_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_visual_polish_mutant_${mutant_lc}"
                               PRIVATE PRISM_V4_MUTANT_${mutant}=1)
    add_test(NAME "mutation.visual_polish.${mutant_id}"
             COMMAND "win_visual_polish_mutant_${mutant_lc}")
    set_tests_properties("mutation.visual_polish.${mutant_id}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "V4 MUTATION CAUGHT: ${mutant_id}")
  endforeach()
endif()
