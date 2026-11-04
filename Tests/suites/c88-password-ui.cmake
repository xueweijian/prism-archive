# c88-password-ui: the password gate's language and pixels (card C88).
#
# C84 made the gate VISIBLE. C88 makes it speak the app's language and paint the
# app's surface:
#   * with the shipped zh-CN table installed, every UIA name in the dialog is
#     Chinese (no 3+ letter ASCII word survives), and the fixed labels equal the
#     table rows exactly;
#   * with no table (the C35 shape) every visible string equals the en-US row, so
#     the code fallback and the shipped table cannot drift;
#   * pixel samples prove the card interior is CompositeOver(glass, canvas), the
#     primary button is CompositeOver(accent, card), body text clears 4.5:1, and
#     the password field's focus ring is the accent colour only while focused;
#   * behaviour is re-asserted through the real WndProc: a wrong password shows
#     the translated error line with the dialog still up, the two memo radios
#     stay mutually exclusive, and "Use selected" is disabled with no selection.
#
# D3 negative controls (D26/D27: PASS_REGULAR_EXPRESSION only, never WILL_FAIL):
#   PRISM_C88_MUTANT_SYSTEM_GREY        the card falls back to COLOR_BTNFACE;
#   PRISM_C88_MUTANT_HARDCODED_ENGLISH  the prompt is an English literal again.
# Each mutant's marker prints at the FIRST failed check; a mutation that stopped
# applying never prints it, so that test is red instead of vacuously green.
#
# D1: the language directory is passed as an environment input (the suite has no
# legitimate SKIP -- it only needs one plain window).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C88_LIBS
      prism_core
      prism_win_bridge
      user32.lib
      gdi32.lib
      ole32.lib
      oleaut32.lib
      comctl32.lib
      d2d1.lib
      dwrite.lib
      dwmapi.lib
      uiautomationcore.lib
      shell32.lib)

  add_executable(win_c88_password_ui win/test_c88_password_ui.cpp)
  target_link_libraries(win_c88_password_ui PRIVATE ${PRISM_C88_LIBS})
  target_compile_options(win_c88_password_ui PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c88_password_ui" COMMAND win_c88_password_ui)
  set_tests_properties(win.c88_password_ui PROPERTIES
      TIMEOUT 180
      ENVIRONMENT "PRISM_C88_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "ALL C88 PASSWORD-UI CHECKS PASSED")

  # Negative control 1: the card is painted with the system face colour.
  add_executable(win_c88_password_ui_mutant_system_grey win/test_c88_password_ui.cpp)
  target_link_libraries(win_c88_password_ui_mutant_system_grey PRIVATE ${PRISM_C88_LIBS})
  target_compile_options(win_c88_password_ui_mutant_system_grey PRIVATE /WX-)
  target_compile_definitions(win_c88_password_ui_mutant_system_grey
                             PRIVATE PRISM_C88_MUTANT_SYSTEM_GREY=1)
  add_test(NAME "mutation.c88.system_grey" COMMAND win_c88_password_ui_mutant_system_grey)
  set_tests_properties(mutation.c88.system_grey PROPERTIES
      TIMEOUT 180
      ENVIRONMENT "PRISM_C88_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "C88 MUTATION CAUGHT: system_grey")

  # Negative control 2: one visible string is an English literal again.
  add_executable(win_c88_password_ui_mutant_hardcoded_english win/test_c88_password_ui.cpp)
  target_link_libraries(win_c88_password_ui_mutant_hardcoded_english PRIVATE ${PRISM_C88_LIBS})
  target_compile_options(win_c88_password_ui_mutant_hardcoded_english PRIVATE /WX-)
  target_compile_definitions(win_c88_password_ui_mutant_hardcoded_english
                             PRIVATE PRISM_C88_MUTANT_HARDCODED_ENGLISH=1)
  add_test(NAME "mutation.c88.hardcoded_english"
           COMMAND win_c88_password_ui_mutant_hardcoded_english)
  set_tests_properties(mutation.c88.hardcoded_english PROPERTIES
      TIMEOUT 180
      ENVIRONMENT "PRISM_C88_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "C88 MUTATION CAUGHT: hardcoded_english")
endif()
