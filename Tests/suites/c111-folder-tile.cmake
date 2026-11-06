# c111-folder-tile: THE FOLDER ICON IN THE GRID (card C111).
#
# THE MEASURED DEFECT. DrawFolderTile painted the folder as two solid rounded
# rectangles (68 x 46 front + 40 x 16 tab), both the same accent, with no rim and
# no specular edge: "还是两块圆角矩形，离 Apple 的立体文件夹还有距离".
#
# WHAT THE SUITE MEASURES (real GalleryRenderer::Draw frames, WIC target,
# read back as BGRA -- not a mock painter):
#   B1  the front face's aspect ratio is 1.15..1.45 (the old 68 x 46 face
#       measured 1.478; the whole silhouette is printed too);
#   B2  the icon's top 2 px average >= 8 luma levels above its centre (the
#       1 px specular edge; a flat fill measures ~0);
#   B3  the back tab's mean luma is below the front face's;
#   B4  the bounding box tracks the cell width (128 px vs 178 px tiles,
#       bbox ratio == cell ratio +-10 %; the old fixed 68 px was off by 28 %).
#
# The screenshot docs/assets/ui/c111-folder-tile.png is a 4x blow-up of the same
# frame, written only when the executable gets an output directory, so ctest
# never dirties the checkout.
#
# NEGATIVE CONTROLS (CONTRACTS D3 / D26 / D27):
#   PRISM_C111_MUTANT_TWO_RECTS    -> the pre-C111 two-rectangle glyph; B1 red.
#   PRISM_C111_MUTANT_NO_HIGHLIGHT -> no 1 px specular edges; B2 red.
# Both CTest entries require the D27 CAUGHT marker, and the verdict is armed on
# a counter the injected branch itself increments (an unrelated failure stays
# red without printing CAUGHT).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C111_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)

  add_executable(win_c111_folder_tile win/test_c111_folder_tile.cpp)
  target_link_libraries(win_c111_folder_tile PRIVATE ${PRISM_C111_LIBS})
  target_compile_options(win_c111_folder_tile PRIVATE /WX-)  # B9: SDK headers
  add_test(NAME "win.c111_folder_tile" COMMAND win_c111_folder_tile)
  set_tests_properties(win.c111_folder_tile PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C111 FOLDER-TILE CHECKS PASSED")

  add_executable(win_c111_folder_tile_mutant_two_rects win/test_c111_folder_tile.cpp)
  target_link_libraries(win_c111_folder_tile_mutant_two_rects PRIVATE ${PRISM_C111_LIBS})
  target_compile_options(win_c111_folder_tile_mutant_two_rects PRIVATE /WX-)
  target_compile_definitions(win_c111_folder_tile_mutant_two_rects PRIVATE
      PRISM_C111_MUTANT_TWO_RECTS=1)
  add_test(NAME "c111.mutation.two_rects"
           COMMAND win_c111_folder_tile_mutant_two_rects)
  set_tests_properties(c111.mutation.two_rects PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C111 MUTATION CAUGHT: two-rects")

  add_executable(win_c111_folder_tile_mutant_no_highlight win/test_c111_folder_tile.cpp)
  target_link_libraries(win_c111_folder_tile_mutant_no_highlight PRIVATE ${PRISM_C111_LIBS})
  target_compile_options(win_c111_folder_tile_mutant_no_highlight PRIVATE /WX-)
  target_compile_definitions(win_c111_folder_tile_mutant_no_highlight PRIVATE
      PRISM_C111_MUTANT_NO_HIGHLIGHT=1)
  add_test(NAME "c111.mutation.no_highlight"
           COMMAND win_c111_folder_tile_mutant_no_highlight)
  set_tests_properties(c111.mutation.no_highlight PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C111 MUTATION CAUGHT: no-highlight")
endif()
