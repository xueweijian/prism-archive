# c83-chrome: the dark window SHELL (card C83, UI-AUDIT A4).
#
# THE DEFECT. The top 64 px of the window were the system's white caption plus a
# light Win32 menu bar; the canvas below them is #0B0E13. The shell and the
# content did not read as one application.
#
# TWO LAYERS, because the fix has two halves:
#   * core.chrome_theme   the shell palette as pure numbers: the menu plate IS
#                         --surface-backdrop-reference, the item text clears
#                         WCAG 4.5:1 on the resting AND the selected row, the
#                         dark shell is within the card's 12-level luma band of
#                         the canvas, and the appearance key maps to one variant;
#   * win.c83_chrome      a REAL window: DwmSetWindowAttribute (attribute 20,
#                         falling back to 19) applied and recorded, the caption
#                         and menu bar sampled from real screen pixels against
#                         the canvas, the four menu items and the Settings id
#                         still on the HMENU, and WM_DRAWITEM painting a
#                         dropdown row with the token.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27): each mutant is the SAME source
# with exactly one rule broken, and its CTest entry requires the D27 marker, so
# a stale injection (suite still green) leaves the mutant entry RED.
#   menu_system_light  the menu plate is the system's white one -> contrast red
#   no_dwm             Apply() makes no DWM call -> caption brightness red
#   no_owner_draw      the bar is appended as MF_STRING -> menu plate red
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # user32/gdi32/d2d1/dwrite/... are the libraries every GalleryWindow consumer
  # names; dwmapi is what this card adds (DwmSetWindowAttribute).
  set(PRISM_C83_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      dwmapi.lib windowscodecs.lib)

  add_executable(win_c83_chrome_test win/test_c83_chrome.cpp)
  target_link_libraries(win_c83_chrome_test PRIVATE ${PRISM_C83_WINDOW_LIBS})
  target_compile_options(win_c83_chrome_test PRIVATE /WX-)
  add_test(NAME "win.c83_chrome" COMMAND win_c83_chrome_test)
  # RESOURCE_LOCK: the assertion reads REAL SCREEN pixels, so two of these must
  # never run at once (a parallel ctest run put another test's window in front
  # of the capture and the mutant skipped instead of going red).
  set_tests_properties(win.c83_chrome PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      RESOURCE_LOCK "c83_screen"
      PASS_REGULAR_EXPRESSION "ALL DARK-SHELL CHECKS PASSED")

  # D3 control 1: no DWM call at all -- the caption keeps the system's light
  # chrome. The caption brightness assertion MUST go red.
  add_executable(win_c83_chrome_mutant_no_dwm win/test_c83_chrome.cpp)
  target_link_libraries(win_c83_chrome_mutant_no_dwm PRIVATE ${PRISM_C83_WINDOW_LIBS})
  target_compile_options(win_c83_chrome_mutant_no_dwm PRIVATE /WX-)
  target_compile_definitions(win_c83_chrome_mutant_no_dwm
                             PRIVATE PRISM_C83_MUTANT_NO_DWM=1)
  add_test(NAME "c83.mutation.chrome.no_dwm" COMMAND win_c83_chrome_mutant_no_dwm)
  set_tests_properties(c83.mutation.chrome.no_dwm PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "c83_screen"
      PASS_REGULAR_EXPRESSION "C83 MUTATION CAUGHT: no_dwm")

  # D3 control 2: the bar goes back to system-painted MF_STRING items. The menu
  # plate assertion MUST go red.
  add_executable(win_c83_chrome_mutant_no_owner_draw win/test_c83_chrome.cpp)
  target_link_libraries(win_c83_chrome_mutant_no_owner_draw PRIVATE ${PRISM_C83_WINDOW_LIBS})
  target_compile_options(win_c83_chrome_mutant_no_owner_draw PRIVATE /WX-)
  target_compile_definitions(win_c83_chrome_mutant_no_owner_draw
                             PRIVATE PRISM_C83_MUTANT_NO_OWNERDRAW=1)
  add_test(NAME "c83.mutation.chrome.no_owner_draw"
           COMMAND win_c83_chrome_mutant_no_owner_draw)
  set_tests_properties(c83.mutation.chrome.no_owner_draw PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "c83_screen"
      PASS_REGULAR_EXPRESSION "C83 MUTATION CAUGHT: no_owner_draw")
endif()

# The pure half runs on every lane (the Linux lane included): it needs no HWND.
add_executable(test_chrome_theme test_chrome_theme.cpp)
target_link_libraries(test_chrome_theme PRIVATE prism_core)
add_test(NAME "core.chrome_theme" COMMAND test_chrome_theme)
set_tests_properties(core.chrome_theme PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL SHELL-PALETTE CHECKS PASSED")

# D3 control 3 (pure): the plate is the system's white one while the ink stays
# the theme's -- white on white. The contrast assertion MUST go red.
add_executable(test_chrome_theme_mutant_menu_system_light test_chrome_theme.cpp)
target_link_libraries(test_chrome_theme_mutant_menu_system_light PRIVATE prism_core)
target_compile_definitions(test_chrome_theme_mutant_menu_system_light
                           PRIVATE PRISM_C83_MUTANT_MENU_SYSTEM_LIGHT=1)
add_test(NAME "c83.mutation.chrome.menu_system_light"
         COMMAND test_chrome_theme_mutant_menu_system_light)
set_tests_properties(c83.mutation.chrome.menu_system_light PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C83 MUTATION CAUGHT: menu_system_light")
