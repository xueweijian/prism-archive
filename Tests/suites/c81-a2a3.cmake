# c81-a2a3: A2 (list header drawn as a selected row) + A3 (three contradicting
# compression ratios).
#
# Core arm (every platform): CompressionRatioView's contract -- unknown /
# one-decimal shrink / ">100% (real value)" growth / purity, and the list cell
# consuming the SAME function.
#
# Windows arm: rendered-pixel A2 assertions (no accent pixel in the header
# band, the band's background IS the canvas, the 1 px separator exists, and
# the band is byte-identical with the C77 backdrop ON and OFF -- the ghost
# cannot come back), plus the three-surface A3 consistency read-back.
#
# One compile-time negative control (CONTRACTS D3/D26/D27; PASS_REGULAR_
# EXPRESSION on the CAUGHT marker, never WILL_FAIL):
#   header_selected          the audit's original sin is back -> A2 red
#
# C128 RETIRED the second arm, "status_hardcoded_unknown", which made the STATUS
# BAND report 未知 for the ratio. C128 removed the ratio slot from the band (the
# band is three numbers now; the ratio is the info card's), so the surface that
# arm lied about no longer exists and the arm could only go green while claiming
# a defect that cannot reach a pixel. The band's own negative control is C128's
# mutation.c128.old_status (restores the six-slot band).
add_executable(c81_ratio_core test_c81_ratio_core.cpp)
target_link_libraries(c81_ratio_core PRIVATE prism_core)
add_test(NAME "core.c81_ratio" COMMAND c81_ratio_core)
set_tests_properties(core.c81_ratio PROPERTIES TIMEOUT 120)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c81_a2a3 win/test_c81_a2a3.cpp)
  target_link_libraries(win_c81_a2a3 PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c81_a2a3 PRIVATE /WX-)
  add_test(NAME "win.c81_a2a3" COMMAND win_c81_a2a3)
  set_tests_properties(win.c81_a2a3 PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C81 A2A3 CHECKS PASSED")

  # C128: status_hardcoded_unknown retired with the band's ratio slot (see the
  # header). header_selected still guards A2.
  set(C81_MUTANT_ARMS header_selected)
  foreach(arm IN LISTS C81_MUTANT_ARMS)
    string(TOUPPER "${arm}" C81_ARM_UPPER)
    add_executable(win_c81_${arm} win/test_c81_a2a3.cpp)
    target_link_libraries(win_c81_${arm} PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                          ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
    target_compile_options(win_c81_${arm} PRIVATE /WX-)
    target_compile_definitions(win_c81_${arm}
                               PRIVATE PRISM_C81_MUTANT_${C81_ARM_UPPER}=1)
    add_test(NAME "mutation.c81.${arm}" COMMAND win_c81_${arm})
    set_tests_properties(mutation.c81.${arm} PROPERTIES TIMEOUT 300
        PASS_REGULAR_EXPRESSION "C81 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
