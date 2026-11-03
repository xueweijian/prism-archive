# c69-settings: the Settings dialog must ACTUALLY change what the user sees.
#
# MEASURED BEFORE THIS CARD (see the test file's header): the theme combo
# repainted nothing (BuildModel hardcoded Variant::Midnight) and the language
# combo changed neither the bar nor a single row -- the dialog's labels were
# literal English with control id 0, and the app rebuilt its menu only at
# startup.
#
# THE VERDICT IS PIXELS AND HWND STRINGS, NEVER A COMMENT:
#   win.c69_settings  a REAL dialog HWND: the same coordinate is sampled after
#                     each theme (the sample is the primary button, whose fill is
#                     the theme's accent token, so the expected RGB comes from
#                     ThemeTokens); a REAL GalleryWindow menu bar is read back
#                     with GetMenuStringW after the dialog's language combo moves.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27/D28): two more binaries compile the
# SAME source with one rule removed. Each arms mini_test's mutation verdict with
# the "injection is active" predicate and prints "C69 MUTATION CAUGHT: <arm>" at
# the first red assertion. No WILL_FAIL is stacked on a PASS_REGULAR_EXPRESSION
# (D26); a stale injection prints nothing and leaves the mutant entry RED.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C69_LIBS prism_core prism_win_bridge prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib comctl32.lib comdlg32.lib shlwapi.lib)

  add_executable(win_c69_settings win/test_c69_settings.cpp)
  target_link_libraries(win_c69_settings PRIVATE ${PRISM_C69_LIBS})
  target_compile_options(win_c69_settings PRIVATE /WX-)  # SDK headers: B9
  target_compile_definitions(win_c69_settings PRIVATE
      "PRISM_C69_LANG_DIR=\"${CMAKE_SOURCE_DIR}/Assets/Lang\"")
  add_test(NAME "win.c69_settings" COMMAND win_c69_settings)
  set_tests_properties(win.c69_settings PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C69 SETTINGS-EFFECT CHECKS PASSED")

  # D3 control 1: the theme branch stops invalidating the window, so the frame on
  # screen keeps the tokens of the theme the dialog opened with.
  add_executable(win_c69_settings_mutant_no_theme_invalidate win/test_c69_settings.cpp)
  target_link_libraries(win_c69_settings_mutant_no_theme_invalidate PRIVATE
                        ${PRISM_C69_LIBS})
  target_compile_options(win_c69_settings_mutant_no_theme_invalidate PRIVATE /WX-)
  target_compile_definitions(win_c69_settings_mutant_no_theme_invalidate PRIVATE
      "PRISM_C69_LANG_DIR=\"${CMAKE_SOURCE_DIR}/Assets/Lang\""
      PRISM_C69_MUTANT_NO_THEME_INVALIDATE=1)
  add_test(NAME "c69.mutation.settings.no_theme_invalidate"
           COMMAND win_c69_settings_mutant_no_theme_invalidate)
  set_tests_properties(c69.mutation.settings.no_theme_invalidate PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C69 MUTATION CAUGHT: no_theme_invalidate")

  # D3 control 2: ApplyMenuLabels() becomes a no-op -- the bar keeps the words it
  # had before the language switch.
  add_executable(win_c69_settings_mutant_no_menu_rebuild win/test_c69_settings.cpp)
  target_link_libraries(win_c69_settings_mutant_no_menu_rebuild PRIVATE
                        ${PRISM_C69_LIBS})
  target_compile_options(win_c69_settings_mutant_no_menu_rebuild PRIVATE /WX-)
  target_compile_definitions(win_c69_settings_mutant_no_menu_rebuild PRIVATE
      "PRISM_C69_LANG_DIR=\"${CMAKE_SOURCE_DIR}/Assets/Lang\""
      PRISM_C69_MUTANT_NO_MENU_REBUILD=1)
  add_test(NAME "c69.mutation.settings.no_menu_rebuild"
           COMMAND win_c69_settings_mutant_no_menu_rebuild)
  set_tests_properties(c69.mutation.settings.no_menu_rebuild PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C69 MUTATION CAUGHT: no_menu_rebuild")
endif()
