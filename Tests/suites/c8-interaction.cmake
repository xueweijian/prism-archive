# ---------------------------------------------------------------------------
# C8 interaction batch (APPENDED; no registration above was touched or
# reordered -- the same rule the M4 dnd-seek block followed).
#
# Blueprint ch. 9.1: the key table, the palette ranker, the navigation-history
# stack and the Ctrl+Shift+N decision are PrismCore modules, so they run on
# Linux in milliseconds instead of burning Windows CI rounds. Each guard has a
# mutation build that MUST go red on the SAME suite (CONTRACTS D3): the mutant
# is a compile definition, so injection cannot silently fail to apply (D3's
# own lesson: a text replacement that does not match looks like a green).
# ---------------------------------------------------------------------------
add_executable(test_shortcuts test_shortcuts.cpp)
target_link_libraries(test_shortcuts PRIVATE prism_core)
add_test(NAME "core.test_shortcuts" COMMAND test_shortcuts)

add_executable(test_command_palette test_command_palette.cpp)
target_link_libraries(test_command_palette PRIVATE prism_core)
add_test(NAME "core.test_command_palette" COMMAND test_command_palette)

add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE prism_core)
add_test(NAME "core.test_history" COMMAND test_history)

# The zoom ladder must clamp to the blueprint's 64..512 px window.
add_executable(test_shortcuts_mutant_no_clamp test_shortcuts.cpp)
target_link_libraries(test_shortcuts_mutant_no_clamp PRIVATE prism_core)
target_compile_definitions(test_shortcuts_mutant_no_clamp PRIVATE PRISM_ZOOM_MUTANT_NO_CLAMP=1)
add_test(NAME "mutation.zoom.no_clamp" COMMAND test_shortcuts_mutant_no_clamp)
set_tests_properties(mutation.zoom.no_clamp PROPERTIES WILL_FAIL TRUE)

# Ctrl+Shift+N must refuse names Windows would reinterpret.
add_executable(test_shortcuts_mutant_allow_reserved test_shortcuts.cpp)
target_link_libraries(test_shortcuts_mutant_allow_reserved PRIVATE prism_core)
target_compile_definitions(test_shortcuts_mutant_allow_reserved PRIVATE PRISM_NEWFOLDER_MUTANT_ALLOW_RESERVED=1)
add_test(NAME "mutation.mkdir.allow_reserved" COMMAND test_shortcuts_mutant_allow_reserved)
set_tests_properties(mutation.mkdir.allow_reserved PROPERTIES WILL_FAIL TRUE)

# The palette's word-boundary bonus is what makes it prefer filenames.
add_executable(test_command_palette_mutant_no_boundary test_command_palette.cpp)
target_link_libraries(test_command_palette_mutant_no_boundary PRIVATE prism_core)
target_compile_definitions(test_command_palette_mutant_no_boundary PRIVATE PRISM_PALETTE_MUTANT_NO_BOUNDARY=1)
add_test(NAME "mutation.palette.no_boundary" COMMAND test_command_palette_mutant_no_boundary)
set_tests_properties(mutation.palette.no_boundary PROPERTIES WILL_FAIL TRUE)

# Shorter-title tie-break: a reversed comparator must reorder the results.
add_executable(test_command_palette_mutant_longer_first test_command_palette.cpp)
target_link_libraries(test_command_palette_mutant_longer_first PRIVATE prism_core)
target_compile_definitions(test_command_palette_mutant_longer_first PRIVATE PRISM_PALETTE_MUTANT_LONGER_FIRST=1)
add_test(NAME "mutation.palette.longer_first" COMMAND test_command_palette_mutant_longer_first)
set_tests_properties(mutation.palette.longer_first PROPERTIES WILL_FAIL TRUE)

# History capacity is a promise (CONTRACTS D17): keeping one extra entry is a
# real bug class, so the off-by-one is a permanent mutant.
add_executable(test_history_mutant_off_by_one test_history.cpp)
target_link_libraries(test_history_mutant_off_by_one PRIVATE prism_core)
target_compile_definitions(test_history_mutant_off_by_one PRIVATE PRISM_HISTORY_MUTANT_OFF_BY_ONE=1)
add_test(NAME "mutation.history.off_by_one" COMMAND test_history_mutant_off_by_one)
set_tests_properties(mutation.history.off_by_one PROPERTIES WILL_FAIL TRUE)

# Adjacent de-duplication: without it Back appears broken on a repeated crumb.
add_executable(test_history_mutant_no_dedupe test_history.cpp)
target_link_libraries(test_history_mutant_no_dedupe PRIVATE prism_core)
target_compile_definitions(test_history_mutant_no_dedupe PRIVATE PRISM_HISTORY_MUTANT_NO_DEDUPE=1)
add_test(NAME "mutation.history.no_dedupe" COMMAND test_history_mutant_no_dedupe)
set_tests_properties(mutation.history.no_dedupe PROPERTIES WILL_FAIL TRUE)

# Windows lane: the key table against the REAL VK_* values and the real
# WndProc, plus the end-to-end journeys (real HWND + posted messages).
# Both markers are a CONJUNCTION with the exit code (D1): a silent SKIP, a
# crash after printing, or a missing window station all fail.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_keys win/test_keys.cpp)
  target_link_libraries(win_keys PRIVATE prism_core prism_win_bridge
                                              user32.lib gdi32.lib ole32.lib
                                              windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_keys PRIVATE /WX-)
  add_test(NAME "win.keys" COMMAND win_keys)
  set_tests_properties(win.keys PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL KEYS CHECKS PASSED")

  add_executable(win_ui_journey win/test_ui_journey.cpp)
  target_link_libraries(win_ui_journey PRIVATE prism_core prism_win_bridge
                                                   user32.lib gdi32.lib ole32.lib
                                                   windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_ui_journey PRIVATE /WX-)
  add_test(NAME "win.ui_journey" COMMAND win_ui_journey)
  set_tests_properties(win.ui_journey PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL UI-JOURNEY CHECKS PASSED")
endif()

