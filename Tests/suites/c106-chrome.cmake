# c106-chrome: the MENU BAR is the TITLE BAR, and Alt puts it away (card C106).
#
# THE DEFECT (ui-redesign-plan section 4). The top of the window is a DWM caption
# over a Win32 menu bar -- two systems' pixels. C83 made both read the same token,
# but nothing compared the two SURFACES with each other: a bar pinned to white
# (the plan's "突兀白条") left every existing gate green. And the bar always
# occupied SM_CYMENU pixels: there was no way to put it away.
#
# TWO LAYERS, because the fix has two halves:
#   core.c106_menu_autohide  the rules as numbers: one shell surface within 2
#                            levels per channel (and the palette that produces it
#                            is one function), the auto-hide state machine (a
#                            bare Alt flips, a chord does not), and the geometry
#                            (hiding gives back exactly the bar's height, >= 20).
#   win.c106_chrome          a REAL window: the caption and the menu plate sampled
#                            from SCREEN pixels in all three themes and compared
#                            with each other (<= 2 per channel); Alt removing the
#                            bar from the non-client area (GetMenu() == null, a
#                            menu bar has no HWND) with the content moving up by
#                            exactly SM_CYMENU; Ctrl+O/Ctrl+E still firing while
#                            the bar is down; and the OBJID_MENU object still
#                            enumerating its four items and still running its
#                            WM_COMMAND door.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27): each arm is the SAME source with
# exactly one rule broken, and its CTest entry requires the D27 marker, so a
# stale injection (suite still green) leaves the mutant entry RED.
#   white_bar  the menu plate is pinned to white in the product's own brush ->
#              the three-theme menu==title pixel gate goes red
#   alt_noop   prism::chrome::Toggle returns its argument -> the Alt assertions
#              (core AND the window's geometry) go red

add_executable(core_c106_menu_autohide test_c106_menu_autohide.cpp)
target_link_libraries(core_c106_menu_autohide PRIVATE prism_core)
add_test(NAME "core.c106_menu_autohide" COMMAND core_c106_menu_autohide)
set_tests_properties(core.c106_menu_autohide PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL MENU-BAR CHECKS PASSED")

# D3 control 1 (pure): the toggle does nothing -- the pre-card state, where Alt
# had no meaning at all. The "one Alt press flips the bar" assertions go red.
add_executable(core_c106_menu_autohide_mutant_alt_noop test_c106_menu_autohide.cpp)
target_link_libraries(core_c106_menu_autohide_mutant_alt_noop PRIVATE prism_core)
target_compile_definitions(core_c106_menu_autohide_mutant_alt_noop
                           PRIVATE PRISM_C106_MUTANT_ALT_NOOP=1)
add_test(NAME "c106.mutation.chrome.alt_noop"
         COMMAND core_c106_menu_autohide_mutant_alt_noop)
set_tests_properties(c106.mutation.chrome.alt_noop PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C106 MUTATION CAUGHT: alt_noop")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # user32/gdi32/d2d1/dwrite: the window; dwmapi: the caption; windowscodecs: the
  # PNG evidence; oleacc: the OBJID_MENU accessibility object.
  set(PRISM_C106_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      dwmapi.lib windowscodecs.lib oleacc.lib)

  add_executable(win_c106_chrome_test win/test_c106_chrome.cpp)
  target_link_libraries(win_c106_chrome_test PRIVATE ${PRISM_C106_WINDOW_LIBS})
  target_compile_options(win_c106_chrome_test PRIVATE /WX-)
  add_test(NAME "win.c106_chrome" COMMAND win_c106_chrome_test)
  # RESOURCE_LOCK: the assertions read REAL SCREEN pixels, so this must not run
  # while c83 (or another capture) has its own window in front of ours. The lock
  # name is the one c83 already holds for the same resource -- two names for one
  # screen would not serialise anything.
  set_tests_properties(win.c106_chrome PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      RESOURCE_LOCK "c83_screen"
      PASS_REGULAR_EXPRESSION "ALL C106 CHROME CHECKS PASSED")

  # D3 control 2: the menu plate is pinned to white while the caption keeps the
  # theme's colour -- the plan's white bar. The menu==title pixel gate MUST go
  # red, and the verdict is armed on a WHITE plate sampled in a DARK theme.
  add_executable(win_c106_chrome_mutant_white_bar win/test_c106_chrome.cpp)
  target_link_libraries(win_c106_chrome_mutant_white_bar PRIVATE ${PRISM_C106_WINDOW_LIBS})
  target_compile_options(win_c106_chrome_mutant_white_bar PRIVATE /WX-)
  target_compile_definitions(win_c106_chrome_mutant_white_bar
                             PRIVATE PRISM_C106_MUTANT_WHITE_BAR=1)
  add_test(NAME "c106.mutation.chrome.white_bar"
           COMMAND win_c106_chrome_mutant_white_bar)
  set_tests_properties(c106.mutation.chrome.white_bar PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "c83_screen"
      PASS_REGULAR_EXPRESSION "C106 MUTATION CAUGHT: white_bar")
endif()
