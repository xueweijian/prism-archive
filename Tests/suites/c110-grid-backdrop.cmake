# c110-grid-backdrop: the GRID's refraction layer must not contain the tiles, and
# a tile must be a MATERIAL in every theme.
#
# C77 re-renders the CONTENT into a 1/8 target and blurs it (BuildBackdrop).
# DrawListMode has taken a "backdropPass" since C90; DrawGrid never did, so every
# tile (6 % fill, accent folder mark, name plate, decoded thumbnail) went into
# the ghost and blurred over the window -- the user's "large grey smear". And the
# tile fill was the literal kTileFill = white 6 %, which on the #F5F5F7 Day canvas
# composites to 246 on 245: the folder tiles "float in the air" with no card.
#
# The suite renders the REAL GalleryRenderer in GRID MODE (1482x991, nine folder
# tiles) into a headless WIC target and asserts PIXELS + COUNTERS:
#   A1  Midnight, backdrop OFF vs ON: the mean luminance inside every tile
#       rectangle moves by at most 4 levels (shipped: ~20+).
#   A2  Daylight: a canvas region below the flow moves by at most 2 levels, and
#       the layer is still composited (BackdropDraws/Rebuilds == 1).
#   A3  Daylight: the tile BODY patch stands >= 6 levels off the canvas (shipped:
#       0.7).
#   A4  Midnight / High Contrast: the body delta stays inside its theme's band
#       (Night 8..24, HC 12..40), so the Day fix cannot leak into them.
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative controls (CONTRACTS D3/D26/D27): the SAME source compiled with one
# rule broken. The entries use PASS_REGULAR_EXPRESSION on the verdict -- NOT
# WILL_FAIL (D26 forbids combining them) -- and the verdict is armed on a counter
# that only the injected branch increments, so a red for an unrelated reason
# stays red WITHOUT a false CAUGHT.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c110_grid_backdrop win/test_c110_grid_backdrop.cpp)
  target_link_libraries(win_c110_grid_backdrop PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c110_grid_backdrop PRIVATE /WX-)
  add_test(NAME "win.c110_grid_backdrop" COMMAND win_c110_grid_backdrop)
  set_tests_properties(win.c110_grid_backdrop PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C110 GRID-BACKDROP CHECKS PASSED")

  # tiles-in-backdrop: DrawGrid's backdrop pass paints the tiles again (the
  # shipped defect) -> A1 red. flat-tile: TileFillFor() is the old white 6 % in
  # every theme -> A3 red.
  add_executable(win_c110_grid_backdrop_tiles win/test_c110_grid_backdrop.cpp)
  target_link_libraries(win_c110_grid_backdrop_tiles PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
  target_compile_options(win_c110_grid_backdrop_tiles PRIVATE /WX-)
  target_compile_definitions(win_c110_grid_backdrop_tiles
                             PRIVATE PRISM_C110_MUTANT_TILES_IN_BACKDROP=1)
  add_test(NAME "mutation.c110.tiles_in_backdrop" COMMAND win_c110_grid_backdrop_tiles)
  set_tests_properties(mutation.c110.tiles_in_backdrop PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C110 MUTATION CAUGHT: tiles-in-backdrop")

  add_executable(win_c110_grid_backdrop_flat win/test_c110_grid_backdrop.cpp)
  target_link_libraries(win_c110_grid_backdrop_flat PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
  target_compile_options(win_c110_grid_backdrop_flat PRIVATE /WX-)
  target_compile_definitions(win_c110_grid_backdrop_flat PRIVATE PRISM_C110_MUTANT_FLAT_TILE=1)
  add_test(NAME "mutation.c110.flat_tile" COMMAND win_c110_grid_backdrop_flat)
  set_tests_properties(mutation.c110.flat_tile PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C110 MUTATION CAUGHT: flat-tile")
endif()
