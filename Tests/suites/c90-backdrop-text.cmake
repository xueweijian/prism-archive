# c90-backdrop-text: the list's white blurred plate -- the C77 refraction source
# must not contain the ROW GLYPHS.
#
# Research A measured it on the shipped exe: the name column's real ink coverage
# is 0.092, a faithful 1/8 blur would read lum 16.8, the frame read 78.7 (a
# source coverage of 0.81, 8.8x the ink). The layer draws the CONTENT into a
# 1/8 target; in list mode that pass re-drew every row label, and a 13 px run of
# near-white text at 1/8 rasterizes into a solid mini row. 4 mini px of row
# pitch plus sigma 1.75 fuses the rows; the veil leaves 35% -> the user's two
# white rectangles. The list HEADER was excluded for the same reason by A2; the
# rows were missed, and test_c77_glass.cpp only ever renders the GRID scene, so
# CI stayed green.
#
# The suite renders the REAL GalleryRenderer in LIST MODE (listMode + 14 rows +
# the rail) into a headless WIC target and asserts PIXELS:
#   * name band (x 400..1400, the audit's box), over the pixels that are canvas
#     in the backdrop-OFF reference: maxLum(ON) <= canvas + 24. Shipped code
#     reads ~91 against a limit of 38 -- RED, and the mutant arm re-prints it.
#   * the SELECTED row leaves no band ACROSS the row (x 400..1400 every 100 px,
#     every y of the rows band): at a reference-canvas pixel ON may differ by at
#     most 4 levels. The 32 px accent pill used to be 4 mini px of solid accent
#     in the 1/8 source -- a full-width band that outlived the selection.
#   * positive control, ratio column (x 1730..1830): the capsule bar is still
#     content, so ON and OFF still differ by >= 8 levels on >= 1500 pixels.
#     Switching the whole layer off turns THIS red.
#   * rebuild policy: one build per content change, never on a scroll.
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative control (CONTRACTS D3/D26/D27): the SAME source compiled with
# drawGlyphs forced back to true. The verdict is a CONJUNCTION -- printed at the
# FIRST failing check and only when the injected branch actually ran -- and the
# entry uses PASS_REGULAR_EXPRESSION on the verdict, never WILL_FAIL (D26
# forbids combining them).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c90_backdrop_text win/test_c90_backdrop_text.cpp)
  target_link_libraries(win_c90_backdrop_text PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c90_backdrop_text PRIVATE /WX-)
  add_test(NAME "win.c90_backdrop_text" COMMAND win_c90_backdrop_text)
  set_tests_properties(win.c90_backdrop_text PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C90 BACKDROP-TEXT CHECKS PASSED")

  add_executable(win_c90_text_in_backdrop win/test_c90_backdrop_text.cpp)
  target_link_libraries(win_c90_text_in_backdrop PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
  target_compile_options(win_c90_text_in_backdrop PRIVATE /WX-)
  target_compile_definitions(win_c90_text_in_backdrop
                             PRIVATE PRISM_C77_MUTANT_TEXT_IN_BACKDROP=1)
  add_test(NAME "mutation.c90.text_in_backdrop" COMMAND win_c90_text_in_backdrop)
  set_tests_properties(mutation.c90.text_in_backdrop PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C77 MUTATION CAUGHT: text-in-backdrop")

  # The follow-up's negative control: the SELECTED ROW's accent pill back in the
  # 1/8 source. It is the band that survived the text fix -- full row width at
  # the row, and still there after the selection moved on, because the backdrop
  # key never carried selectedRow.
  add_executable(win_c90_state_in_backdrop win/test_c90_backdrop_text.cpp)
  target_link_libraries(win_c90_state_in_backdrop PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)
  target_compile_options(win_c90_state_in_backdrop PRIVATE /WX-)
  target_compile_definitions(win_c90_state_in_backdrop
                             PRIVATE PRISM_C77_MUTANT_STATE_IN_BACKDROP=1)
  add_test(NAME "mutation.c90.state_in_backdrop" COMMAND win_c90_state_in_backdrop)
  set_tests_properties(mutation.c90.state_in_backdrop PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C77 MUTATION CAUGHT: state-in-backdrop")
endif()
