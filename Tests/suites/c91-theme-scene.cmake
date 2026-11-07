# c91-theme-scene: THE THEME REACHES THE CONTENT (card C91, research B).
#
# THE DEFECT. A 1,544,424 px whole-window diff of day -> night moved 122,969 px
# (7.96 %), all of them in y=0..62 (title bar + menu bar). y=64..810 -- 92 % of
# the window -- was pixel-identical in all three themes: the canvas was the
# literal #0B0E13 and GalleryRenderer's sidebarTheme / actionTheme / tabTheme /
# glassTheme were never set by the window.
#
# TWO LAYERS, because the fix has two halves:
#   core.c91_theme_surface  the canvas/surface arithmetic as pure numbers:
#                           canvas day (245,245,247) / night (16,17,23) /
#                           high-contrast (0,0,0), the island, the rail panel,
#                           the tab plate, the selection wash, the info card and
#                           the status strip -- plus C83's shell-continuity rule
#                           applied to ALL THREE themes (in Day it used to be
#                           false by construction: shell 255 vs canvas 12.9);
#   win.c91_theme_scene     the renderer's OWN pixels: one scene, three themes,
#                           sampled at the card's regions, compared against
#                           prism::chrome::ThemeSurfaceColor (the one entry), and
#                           the headline measurement -- the content band must
#                           differ over >= 80 % of its pixels (it was 0 %).
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27): each arm is the SAME source with
# exactly one rule broken, and its CTest entry requires the D27 marker, so a
# stale injection (suite still green) leaves the mutant entry RED.
#   core_midnight    CanvasColor resolves every variant to Midnight
#   scene_midnight   Draw() repaints the whole frame with Midnight
#   brushes_once     the text brushes keep their build-time (literal) colours
#   island_literal   the islands keep the pre-C91 kIslandFill literal

add_executable(core_c91_theme_surface test_c91_theme_surface.cpp)
target_link_libraries(core_c91_theme_surface PRIVATE prism_core)
add_test(NAME "core.c91_theme_surface" COMMAND core_c91_theme_surface)
set_tests_properties(core.c91_theme_surface PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL THEME-SURFACE CHECKS PASSED")

# D3 control 0 (pure): every variant resolves to Midnight. The Day canvas
# assertions MUST go red.
add_executable(core_c91_theme_surface_mutant_midnight test_c91_theme_surface.cpp)
target_link_libraries(core_c91_theme_surface_mutant_midnight PRIVATE prism_core)
target_compile_definitions(core_c91_theme_surface_mutant_midnight
                           PRIVATE PRISM_C91_MUTANT_CORE_MIDNIGHT=1)
add_test(NAME "c91.mutation.theme.core_midnight"
         COMMAND core_c91_theme_surface_mutant_midnight)
set_tests_properties(c91.mutation.theme.core_midnight PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C91 MUTATION CAUGHT: core_midnight")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # d2d1/dwrite/windowscodecs: the offscreen WIC render target every pixel case
  # reads back from. dwmapi/user32: the window-wiring case.
  set(PRISM_C91_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      dwmapi.lib windowscodecs.lib)

  add_executable(win_c91_theme_scene_test win/test_c91_theme_scene.cpp)
  target_link_libraries(win_c91_theme_scene_test PRIVATE ${PRISM_C91_WINDOW_LIBS})
  target_compile_options(win_c91_theme_scene_test PRIVATE /WX-)
  add_test(NAME "win.c91_theme_scene" COMMAND win_c91_theme_scene_test)
  set_tests_properties(win.c91_theme_scene PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL THEME-SCENE CHECKS PASSED")

  # D3 control 1: the whole frame repaints as Midnight while the caller asked for
  # another theme. Every Day assertion MUST go red.
  add_executable(win_c91_theme_scene_mutant_scene_midnight win/test_c91_theme_scene.cpp)
  target_link_libraries(win_c91_theme_scene_mutant_scene_midnight
                        PRIVATE ${PRISM_C91_WINDOW_LIBS})
  target_compile_options(win_c91_theme_scene_mutant_scene_midnight PRIVATE /WX-)
  target_compile_definitions(win_c91_theme_scene_mutant_scene_midnight
                             PRIVATE PRISM_C91_MUTANT_SCENE_MIDNIGHT=1)
  add_test(NAME "c91.mutation.theme.scene_midnight"
           COMMAND win_c91_theme_scene_mutant_scene_midnight)
  set_tests_properties(c91.mutation.theme.scene_midnight PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C91 MUTATION CAUGHT: scene_midnight")

  # D3 control 2: the text brushes are built once and never recoloured -- the
  # "surfaces change, every glyph stays midnight" half of the defect. The list
  # row ink assertion MUST go red on the daylight frame.
  add_executable(win_c91_theme_scene_mutant_brushes_once win/test_c91_theme_scene.cpp)
  target_link_libraries(win_c91_theme_scene_mutant_brushes_once
                        PRIVATE ${PRISM_C91_WINDOW_LIBS})
  target_compile_options(win_c91_theme_scene_mutant_brushes_once PRIVATE /WX-)
  target_compile_definitions(win_c91_theme_scene_mutant_brushes_once
                             PRIVATE PRISM_C91_MUTANT_BRUSHES_ONCE=1)
  add_test(NAME "c91.mutation.theme.brushes_once"
           COMMAND win_c91_theme_scene_mutant_brushes_once)
  set_tests_properties(c91.mutation.theme.brushes_once PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C91 MUTATION CAUGHT: brushes_once")

  # D3 control 3: IslandOf keeps "(void)variant" and returns the literal. The
  # action island / breadcrumb assertions MUST go red on the daylight frame.
  add_executable(win_c91_theme_scene_mutant_island_literal win/test_c91_theme_scene.cpp)
  target_link_libraries(win_c91_theme_scene_mutant_island_literal
                        PRIVATE ${PRISM_C91_WINDOW_LIBS})
  target_compile_options(win_c91_theme_scene_mutant_island_literal PRIVATE /WX-)
  target_compile_definitions(win_c91_theme_scene_mutant_island_literal
                             PRIVATE PRISM_C91_MUTANT_ISLAND_LITERAL=1)
  add_test(NAME "c91.mutation.theme.island_literal"
           COMMAND win_c91_theme_scene_mutant_island_literal)
  set_tests_properties(c91.mutation.theme.island_literal PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C91 MUTATION CAUGHT: island_literal")
endif()
