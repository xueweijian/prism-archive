# c93-navigation: the two dead interactions the field report measured (pa-08,
# pa-09/pa-11) plus the "no navigation event" rule that keeps a re-click quiet.
#
#   * BREADCRUMB: every crumb answers its own measured text span; segment 0 is
#     the archive root. A click there from "docs/sub" lands on CurrentPath()==""
#     with the root's row count. A click on the crumb already current is a no-op
#     and does not count as a navigation.
#   * TREE ROOT: a click on the sidebar's root row navigates to "" and moves the
#     tree highlight with it.
#   * HOVER: the feedback rect is the crumb's own span (not the whole bar).
#
# LAYERS: the model is pure PrismCore (core.c93_navigation) and runs on every
# lane; the window is proven against a REAL HWND (win.c93_navigation) with a
# catalog handed through the production SetItems door -- no 7z.dll, so it cannot
# skip silently.
#
# NEGATIVE CONTROLS (D3/D26/D27): one executable per injected defect, compiled
# from the SAME source. Each prints "C93 MUTATION CAUGHT: <arm>" at its FIRST
# failure and exits 0; CTest's PASS_REGULAR_EXPRESSION is that marker. No
# WILL_FAIL is stacked on a regex anywhere in this file.
add_executable(core_c93_navigation_test test_c93_navigation_core.cpp)
target_link_libraries(core_c93_navigation_test PRIVATE prism_core)
add_test(NAME "core.c93_navigation" COMMAND core_c93_navigation_test)
set_tests_properties(core.c93_navigation PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C93 NAVIGATION CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C93_LIBS
      prism_core
      prism_win_bridge
      prism_shell_contract
      user32.lib
      gdi32.lib
      ole32.lib
      oleaut32.lib
      shell32.lib
      d2d1.lib
      dwrite.lib
      windowscodecs.lib
      bcrypt.lib
      WindowsApp.lib)

  add_executable(win_c93_navigation win/test_c93_navigation.cpp)
  target_link_libraries(win_c93_navigation PRIVATE ${PRISM_C93_LIBS})
  target_compile_options(win_c93_navigation PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c93_navigation" COMMAND win_c93_navigation)
  set_tests_properties(win.c93_navigation PROPERTIES
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL C93 NAVIGATION CHECKS PASSED")

  # Arm 1: the bar is dead text again -- HitBreadcrumb never resolves a level,
  # so a click on segment 0 is swallowed by the chrome. Assertion 1 must go red.
  add_executable(win_c93_navigation_mutant_dead_crumb win/test_c93_navigation.cpp)
  target_link_libraries(win_c93_navigation_mutant_dead_crumb PRIVATE ${PRISM_C93_LIBS})
  target_compile_options(win_c93_navigation_mutant_dead_crumb PRIVATE /WX-)
  target_compile_definitions(win_c93_navigation_mutant_dead_crumb
                             PRIVATE PRISM_C93_MUTANT_DEAD_CRUMB=1)
  add_test(NAME "mutation.c93.dead_crumb"
           COMMAND win_c93_navigation_mutant_dead_crumb)
  set_tests_properties(mutation.c93.dead_crumb PROPERTIES
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C93 MUTATION CAUGHT: dead_crumb")

  # Arm 2: the tree's ROOT row answers nothing while sub-folders still navigate
  # -- the pre-C93 "I can not get back to the root". Assertion 2 must go red.
  add_executable(win_c93_navigation_mutant_root_noop win/test_c93_navigation.cpp)
  target_link_libraries(win_c93_navigation_mutant_root_noop PRIVATE ${PRISM_C93_LIBS})
  target_compile_options(win_c93_navigation_mutant_root_noop PRIVATE /WX-)
  target_compile_definitions(win_c93_navigation_mutant_root_noop
                             PRIVATE PRISM_C93_MUTANT_ROOT_NOOP=1)
  add_test(NAME "mutation.c93.root_noop"
           COMMAND win_c93_navigation_mutant_root_noop)
  set_tests_properties(mutation.c93.root_noop PROPERTIES
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C93 MUTATION CAUGHT: root_noop")

  # Arm 3: pressing the crumb you are already on counts as a navigation. The
  # path still does not move, so ONLY the counter can catch it (assertion 3).
  add_executable(win_c93_navigation_mutant_crumb_resets win/test_c93_navigation.cpp)
  target_link_libraries(win_c93_navigation_mutant_crumb_resets PRIVATE ${PRISM_C93_LIBS})
  target_compile_options(win_c93_navigation_mutant_crumb_resets PRIVATE /WX-)
  target_compile_definitions(win_c93_navigation_mutant_crumb_resets
                             PRIVATE PRISM_C93_MUTANT_CRUMB_RESETS=1)
  add_test(NAME "mutation.c93.crumb_resets"
           COMMAND win_c93_navigation_mutant_crumb_resets)
  set_tests_properties(mutation.c93.crumb_resets PROPERTIES
      TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C93 MUTATION CAUGHT: crumb_resets")
endif()
