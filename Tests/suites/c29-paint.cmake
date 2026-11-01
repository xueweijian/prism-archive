# c29-paint: the sidebar reaches the SCREEN (blueprint 3.2, card C29).
#
# C12 owns the sidebar's draw-command list, C13 owns the list ordering, C15 owns
# the design tokens; this card is the painter that consumes them. One card = one
# file (see Tests/suites/README.md): Tests/CMakeLists.txt is untouched.
#
# The clean binary renders the REAL GalleryRenderer into a headless WIC target
# and asserts PIXELS: the column surface inside the panel box, a local focus
# pill, the width fold pinned at its threshold, the list gaining a column of
# width when the sidebar folds, the scroll clip at the content bottom, and the
# superellipse corners solved against the C15 SquircleSdf.
#
# The stdout marker is a CONJUNCTION with the exit code (CONTRACTS D1): a crash
# after the checks or a silent early return can never count as a pass.
#
# Negative controls (CONTRACTS D3): each mutant is the SAME source with exactly
# one rule broken, registered WILL_FAIL so CTest is green only while the suite
# catches it. No PASS_REGULAR_EXPRESSION on those on purpose (D26: combining the
# two would invert the verdict). Each prints "C29 MUTANT ACTIVE: <name>" so the
# log proves the define actually reached the renderer -- a macro that silently
# stopped matching would leave the mutant green and turn its registration red.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_sidebar_paint win/test_sidebar_paint.cpp)
  target_link_libraries(win_sidebar_paint PRIVATE prism_core prism_win_bridge
                                                user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                windowscodecs.lib ole32.lib)
  target_compile_options(win_sidebar_paint PRIVATE /WX-)
  add_test(NAME "win.sidebar_paint" COMMAND win_sidebar_paint)
  set_tests_properties(win.sidebar_paint PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "ALL SIDEBAR-PAINT CHECKS PASSED")

  foreach(mutant IN ITEMS NO_SIDEBAR_PAINT COLLAPSE_INVERTED)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_sidebar_paint_mutant_${mutant_lc}" win/test_sidebar_paint.cpp)
    target_link_libraries("win_sidebar_paint_mutant_${mutant_lc}"
                          PRIVATE prism_core prism_win_bridge
                                  user32.lib gdi32.lib d2d1.lib dwrite.lib
                                  windowscodecs.lib ole32.lib)
    target_compile_options("win_sidebar_paint_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_sidebar_paint_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C29_MUTANT_${mutant}=1")
    add_test(NAME "mutation.sidebar_paint.${mutant_lc}"
             COMMAND "win_sidebar_paint_mutant_${mutant_lc}")
    set_tests_properties("mutation.sidebar_paint.${mutant_lc}" PROPERTIES
                         WILL_FAIL TRUE TIMEOUT 240)
  endforeach()
endif()
