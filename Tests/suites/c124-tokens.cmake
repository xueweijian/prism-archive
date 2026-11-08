# c124-tokens: ONE accent, FIVE surfaces, a GOLD folder, ONE seam (card C124).
#
# THE MEASURED DEFECTS (UI-AUDIT 12.2 / 12.5 / 12.6 / 12.7): three blues doing
# three jobs, eleven surface tones with the hierarchy backwards (the rail read
# 1.32:1 against the canvas, the action island 1.15:1), a folder tinted with the
# accent, and .md / .bin drawn by the SAME glyph.
#
# WHAT THE TWO LANES MEASURE
#   core.c124_tokens   V1 one accent (no accent literal outside ThemeTokens.hpp,
#                      and the same #0A84FF in both design themes);
#                      V2 five surface tiers, presented as the card's five hexes,
#                      dE 4.1 / 4.5 / 5.2 / 5.3 apart, with the tab plate ON the
#                      card tier (no sixth tone);
#                      V3 rail/canvas in [1.03, 1.10], island/canvas >= 1.28;
#                      V4 the folder's three tones decrease in luminance and the
#                      body measures ~1.61:1 on white;
#                      V5a four file kinds + five distinct silhouettes.
#   win.c124_tokens    V5 the two 16 x 16 glyphs differ PIXEL FOR PIXEL on a real
#                      GalleryRenderer frame (256 px compared), with an ink
#                      control so two blank boxes cannot pass;
#                      V6 "the seam": exactly ONE solid 2 DIP accent line in each
#                      of the four regions -- the active tab's bottom edge (the
#                      strip's own frame), the current crumb's bottom edge, the
#                      selected row's left edge, the progress fill's front edge.
#
# NEGATIVE CONTROL (CONTRACTS D3 / D26 / D27): PRISM_C124_MUTANT_OLD_PALETTE
# compiles the PRE-CARD palette into the same source -- the eleven tones, the
# Daylight/Midnight accent split, a folder with no gold. V1/V2/V4 (core) and V5
# (windows) MUST go red, and each CTest entry requires the D27 marker, so a
# mutation that stopped being observable leaves the entry RED instead of looking
# sharp. No WILL_FAIL is stacked on a regex anywhere in this file.
add_executable(core_c124_tokens test_c124_tokens.cpp)
target_link_libraries(core_c124_tokens PRIVATE prism_core)
target_compile_definitions(core_c124_tokens
                           PRIVATE "PRISM_C124_SRC_DIR=\"${CMAKE_SOURCE_DIR}/Src\"")
add_test(NAME "core.c124_tokens" COMMAND core_c124_tokens)
set_tests_properties(core.c124_tokens PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C124 TOKEN CHECKS PASSED")

add_executable(core_c124_tokens_mutant_old_palette test_c124_tokens.cpp)
target_link_libraries(core_c124_tokens_mutant_old_palette PRIVATE prism_core)
target_compile_definitions(core_c124_tokens_mutant_old_palette PRIVATE
    PRISM_C124_MUTANT_OLD_PALETTE=1
    "PRISM_C124_SRC_DIR=\"${CMAKE_SOURCE_DIR}/Src\"")
add_test(NAME "mutation.c124.old_palette"
         COMMAND core_c124_tokens_mutant_old_palette)
set_tests_properties(mutation.c124.old_palette PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C124 MUTATION CAUGHT: old_palette")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # d2d1/dwrite/windowscodecs: the offscreen WIC target every pixel case reads
  # back from. user32/ole32: COM + the window classes the painter touches.
  set(PRISM_C124_WIN_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib ole32.lib)

  add_executable(win_c124_tokens win/test_c124_tokens.cpp)
  target_link_libraries(win_c124_tokens PRIVATE ${PRISM_C124_WIN_LIBS})
  target_compile_options(win_c124_tokens PRIVATE /WX-)  # SDK headers are not /W4
  add_test(NAME "win.c124_tokens" COMMAND win_c124_tokens)
  set_tests_properties(win.c124_tokens PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL C124 RENDER CHECKS PASSED")

  # The pre-card palette on the RENDERED frame: the folder is the accent again,
  # so V5's "gold is warm" control MUST go red.
  add_executable(win_c124_tokens_mutant_old_palette win/test_c124_tokens.cpp)
  target_link_libraries(win_c124_tokens_mutant_old_palette PRIVATE ${PRISM_C124_WIN_LIBS})
  target_compile_options(win_c124_tokens_mutant_old_palette PRIVATE /WX-)
  target_compile_definitions(win_c124_tokens_mutant_old_palette
                             PRIVATE PRISM_C124_MUTANT_OLD_PALETTE=1)
  add_test(NAME "mutation.c124.render_old_palette"
           COMMAND win_c124_tokens_mutant_old_palette)
  set_tests_properties(mutation.c124.render_old_palette PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C124 MUTATION CAUGHT: old_palette")
endif()
