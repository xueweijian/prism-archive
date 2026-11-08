# c123-layout: THE TOP SEAM -- flush toolbar, breadcrumb path bar, tabs on
# demand (card C123).
#
# THE DEFECT (proposal 14.4 / 17.3, measured at 125 %): the OPEN button's centre
# was (643, 138) against Bandizip's (32, 83) -- 611 px too far right and 55 too
# low -- because the breadcrumb PILL shares the 56 px action band with the
# toolbar (actionbar::Compute anchors the first button at pill.right + 12) and
# the 32 px tab strip pushed the row down. The path itself was CENTRED in its
# pill: container 28..718, text 234..495, 62.2 % dead air.
#
# WHAT IS REGISTERED
#   core.c123_layout              the real suite: T1..T5 (tabs on demand, the
#                                 OPEN button's centre, flush edges, the crumb's
#                                 left edge, and the band tiling) over the
#                                 640..1920 sweep the product supports.
#   mutation.c123.floating_toolbar the D3 control:
#                                 PRISM_C123_MUTANT_FLOATING_TOOLBAR restores
#                                 the pre-card floating island, so T2 and T3 must
#                                 go red. It prints
#                                 "C123 MUTATION CAUGHT: floating_toolbar" at the
#                                 first failure and exits 0; the entry decides
#                                 with PASS_REGULAR_EXPRESSION -- never
#                                 WILL_FAIL (CONTRACTS D26/D27).
#   win.c123_window               (Windows layer) the REAL window: the toolbar
#                                 the renderer hit-tests is the flush band, the
#                                 OPEN button's centre is at the corner, and the
#                                 crumb's text box is the layout's.
#
# The core suite is integer geometry (no window, no D2D, no 7z.dll), so the
# Linux lane runs it too.
add_executable(core_c123_layout test_c123_layout.cpp)
target_link_libraries(core_c123_layout PRIVATE prism_core)
add_test(NAME "core.c123_layout" COMMAND core_c123_layout)
set_tests_properties(core.c123_layout PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C123 LAYOUT CHECKS PASSED")

add_executable(core_c123_layout_mutant_floating_toolbar test_c123_layout.cpp)
target_link_libraries(core_c123_layout_mutant_floating_toolbar PRIVATE prism_core)
target_compile_definitions(core_c123_layout_mutant_floating_toolbar
                           PRIVATE PRISM_C123_MUTANT_FLOATING_TOOLBAR=1)
add_test(NAME "mutation.c123.floating_toolbar"
         COMMAND core_c123_layout_mutant_floating_toolbar)
set_tests_properties(mutation.c123.floating_toolbar PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C123 MUTATION CAUGHT: floating_toolbar")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The real window: the same geometry, measured through the band the renderer
  # paints and the hit test resolves (GalleryWindow::ActionBarLayout).
  add_executable(win_c123_window win/test_c123_window.cpp)
  target_link_libraries(win_c123_window PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  # SDK headers are not /W4-clean; the card's own code is.
  target_compile_options(win_c123_window PRIVATE /WX-)
  add_test(NAME "win.c123_window" COMMAND win_c123_window)
  # D1: the marker is a CONJUNCTION with the exit code; a window station that
  # cannot create the HWND returns 77 (a visible Skip, never a fake green).
  set_tests_properties(win.c123_window PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C123 WINDOW CHECKS PASSED")
endif()
