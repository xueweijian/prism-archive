# c100-actionbar: the action band's PURGE -- no mnemonic letters, eight CURVE
# icons (Liquid Glass v2, card C100).
#
# The clean target asserts, on the DEVICE (the painter's readbacks + rendered
# pixels from the real GalleryRenderer path):
#   * the band paints ZERO mnemonic letters and keeps ONE text format -- and the
#     old top-right letter box is bare island on all eight buttons;
#   * the compress icon's centre pixel (and its 3 x 3 neighbourhood) is
#     BACKGROUND, and no stroke runs toward any diagonal: the pre-fix glyph was
#     four straight lines into the middle, i.e. a big X;
#   * every icon reports >= 2 curve primitives (AddBezier / AddArc) and really
#     puts ink on the device;
#   * the shared ID2D1StrokeStyle reads back D2D1_CAP_STYLE_ROUND (start / end /
#     dash) and D2D1_LINE_JOIN_ROUND, i.e. "圆头" is a property, not a promise.
#
# Two compile-time negative controls (CONTRACTS D3 + D26/D27, never WILL_FAIL):
#   mutation.c100.mnemonic       the 10 px corner letter comes back  -> test 1 RED
#   mutation.c100.line_compress  the four-line X comes back          -> test 2 RED
# Each prints "C100 MUTATION CAUGHT: <arm> (first failure at ...)" from the FIRST
# failed assertion and exits 0 (D27), so a build whose injection stopped
# compiling, or a suite that went green with the defect compiled in, is RED.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c100_icons win/test_c100_icons.cpp)
  target_link_libraries(win_c100_icons PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_c100_icons PRIVATE /WX-)
  add_test(NAME "win.c100_icons" COMMAND win_c100_icons)
  set_tests_properties(win.c100_icons PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C100 ICON AND MNEMONIC CHECKS PASSED")

  set(C100_MUTANT_ARMS mnemonic line_compress)
  foreach(arm IN LISTS C100_MUTANT_ARMS)
    string(TOUPPER "${arm}" C100_ARM_UPPER)
    add_executable(win_c100_${arm} win/test_c100_icons.cpp)
    target_link_libraries(win_c100_${arm} PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                          ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
    target_compile_options(win_c100_${arm} PRIVATE /WX-)
    target_compile_definitions(win_c100_${arm}
                               PRIVATE PRISM_C100_MUTANT_${C100_ARM_UPPER}=1)
    add_test(NAME "mutation.c100.${arm}" COMMAND win_c100_${arm})
    set_tests_properties(mutation.c100.${arm} PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C100 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
