# c48-flyoutpaint: the inline-preview flyout's REAL pixels (card C48).
#
# C30 shipped the flyout as a lifecycle shell (WS_EX_NOREDIRECTIONBITMAP -- no
# redirection surface BY DESIGN) and C34 hosted it in the exe, so the peek had a
# lifecycle but no glass. This card adds FlyoutSurface: the DirectComposition
# island (D3D11 + IDCompositionDevice + a rounded-rectangle clip + one content
# visual carrying a premultiplied swapchain) and the D2D content painter.
#
# What is registered here (one card = one file; see suites/README.md):
#   * win.flyout_paint          renders the REAL content painter into a headless
#                               WIC target and asserts PIXELS: the surface is the
#                               C15 floating-island token, every row's text sits
#                               on Metrics::RowBaseline, the corners are C15
#                               squircles (classified by theme::SquircleField),
#                               12 rows are 9 pitches taller than 3 at an
#                               unchanged pitch, and the pitch scales with the
#                               DPI. It also builds the real composition island
#                               on a real HWND and checks the visual tree.
#   * mutation.flyout_paint.*   two D27 controls. Each compiles the SAME source
#                               with exactly one rule broken, prints an injection
#                               banner, and the verdict is printed AT THE FIRST
#                               FAILED ASSERTION by SetMutationVerdict. The
#                               PASS_REGULAR_EXPRESSION below is that marker, so
#                               the entry is green only while the suite catches
#                               the defect -- and red if the injection goes stale.
#
# CONTRACTS D26: no WILL_FAIL anywhere in this file. WILL_FAIL cannot be
# combined with a pass marker (the marker would invert the verdict) and it turns
# a SKIP's exit 0 into a false green -- both are the failure shapes this project
# already paid for (C9/C34). D1: a machine with no window station or no
# DirectComposition prints WHY and exits 77 (SKIP_RETURN_CODE); the stdout marker
# is required all the same, so a silent skip can never count as a pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_flyout_paint win/test_flyout_paint.cpp)
  target_link_libraries(win_flyout_paint PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib
                        d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (contract B9).
  target_compile_options(win_flyout_paint PRIVATE /WX-)
  add_test(NAME "win.flyout_paint" COMMAND win_flyout_paint)
  set_tests_properties(win.flyout_paint PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL FLYOUT-PAINT CHECKS PASSED")

  # NEGATIVE CONTROLS (CONTRACTS D3 + D27). One regression per build:
  #   * SQUARE_CORNER      -- the surface is a plain rectangle: the corner probes
  #                           (1 px in from the corner point) must go red.
  #   * FIXED_ROW_HEIGHT   -- the row pitch ignores the DPI: the "pitch scales"
  #                           and "12 rows are 9 pitches taller" checks must go red.
  foreach(mutant IN ITEMS SQUARE_CORNER FIXED_ROW_HEIGHT)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    add_executable("win_flyout_paint_mutant_${mutant_lc}" win/test_flyout_paint.cpp)
    target_link_libraries("win_flyout_paint_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib ole32.lib
                          windowscodecs.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib
                          dcomp.lib)
    target_compile_options("win_flyout_paint_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_flyout_paint_mutant_${mutant_lc}"
                               PRIVATE PRISM_C48_MUTANT_${mutant}=1)
    add_test(NAME "mutation.flyout_paint.${mutant_id}"
             COMMAND "win_flyout_paint_mutant_${mutant_lc}")
    set_tests_properties("mutation.flyout_paint.${mutant_id}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        # The binary prints the lowercase, dashed id (see main()).
        PASS_REGULAR_EXPRESSION "C48 MUTATION CAUGHT: ${mutant_id}")
  endforeach()
endif()
