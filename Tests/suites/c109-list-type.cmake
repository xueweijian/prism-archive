# c109-list-type: the file list's typography and vertical rhythm.
#
# The defect: size/ratio cells used the proportional UI face, so `6.3 KB` and
# `11.2 KB` put their decimal point on different pixels, and 32 px rows packed
# a data-dense listing into a wall of numbers.
#
# Core arm (every platform, no <windows.h>): 36 px paint/virtualization/hit
# pitch, the third-row hit test, and the >=16 px column gutters with one right
# edge (baseline) per numeric column.
#
# Windows arm (real DirectWrite + real GalleryRenderer + real GalleryWindow):
#   1. advances of 1/8/0/. agree within 0.5 px -- measured through the SAME
#      ListType.hpp format the renderer paints with;
#   2. the rightmost ink pixel of three 1/2/3-digit size cells differs by <=1 px
#      on a headless WIC frame (which is also where c109-list-type.png comes
#      from);
#   3. a click on the third 36 px row is the third item.
#
# Negative control (CONTRACTS D3/D26/D27): PRISM_C109_MUTANT_PROPORTIONAL
# restores proportional Segoe UI with no tnum. Assertion 1 MUST go red; the
# verdict is PASS_REGULAR_EXPRESSION on the CAUGHT marker, never WILL_FAIL.
add_executable(c109_list_type_core test_c109_list_type.cpp)
target_link_libraries(c109_list_type_core PRIVATE prism_core)
add_test(NAME "core.c109_list_type" COMMAND c109_list_type_core)
set_tests_properties(core.c109_list_type PROPERTIES TIMEOUT 120)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c109_list_type win/test_c109_list_type.cpp)
  target_link_libraries(win_c109_list_type PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c109_list_type PRIVATE /WX-)
  add_test(NAME "win.c109_list_type" COMMAND win_c109_list_type)
  set_tests_properties(win.c109_list_type PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C109 LIST-TYPE CHECKS PASSED")

  add_executable(win_c109_proportional win/test_c109_list_type.cpp)
  target_link_libraries(win_c109_proportional PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c109_proportional PRIVATE /WX-)
  target_compile_definitions(win_c109_proportional
                             PRIVATE PRISM_C109_MUTANT_PROPORTIONAL=1)
  add_test(NAME "mutation.c109.proportional" COMMAND win_c109_proportional)
  set_tests_properties(mutation.c109.proportional PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C109 MUTATION CAUGHT: proportional")
endif()
