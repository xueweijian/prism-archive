# c80-a11y: THE CLIENT AREA MADE VISIBLE TO ASSISTIVE TECHNOLOGY (card C80).
#
# THE DEFECT, measured with a UIA client against the shipped window: nine
# elements -- the title bar's three buttons, the system menu and four menu
# items. The eight action buttons, the tab strip, every row, the sidebar tree
# and the status line were invisible, because they are D2D paint rather than
# child HWNDs and the window had no WM_GETOBJECT handler. A screen-reader user
# could not use the product at all, and automation could only assert pixels.
#
# THE FIX is a bridge, not a rewrite: the drawing stays ours, and
# WM_GETOBJECT(OBJID_CLIENT) answers with an IAccessible tree
# (Src/PrismWin/Accessible.hpp) that the platform's MSAA proxy turns into UIA
# elements. Three layers are asserted separately, because a bug in one of them
# is not a bug in the others:
#
#   core.a11y_tree     the pure tree (Src/PrismCore/A11yTree.hpp): counting,
#                      name lookup, deepest-match hit test, 1-based child-id
#                      paths, "at most one focused element". Runs everywhere.
#   c80.a11y           the REAL window: AccessibleObjectFromWindow ->
#                      IAccessible -> walk -> at least 20 elements, the eight
#                      buttons named in the active language, the tab, the list
#                      rows, accSelect moving the WINDOW's selection,
#                      accDoDefaultAction opening the search field, accFocus /
#                      accHitTest, and an archive-less window that publishes an
#                      EMPTY list instead of crashing.
#   c80.a11y_uia       the same claims through an actual UIA client
#                      (agent-desktop snapshot/click), i.e. through the MSAA
#                      proxy the card was measured with. SKIPPED (exit 77) on a
#                      machine without the tool -- never silently green.
#
# NEGATIVE CONTROL (CONTRACTS D3 + D26 + D27 + D28). PRISM_C80_MUTANT_NO_GETOBJECT
# makes WM_GETOBJECT answer 0 -- the pre-fix behaviour. The SAME test source is
# built again; the element-count assertion MUST go red, and mini_test's armed
# verdict prints "C80 MUTATION CAUGHT: no_getobject" at the FIRST failure and
# exits 0 (no WILL_FAIL anywhere, D26). The predicate also proves at runtime
# that the window REALLY answers nothing, so an unrelated red cannot claim
# CAUGHT. The arm name avoids clean/unarmed/control (D28).
add_executable(test_a11y_tree test_a11y_tree.cpp)
target_link_libraries(test_a11y_tree PRIVATE prism_core)
add_test(NAME "core.a11y_tree" COMMAND test_a11y_tree)
set_tests_properties(core.a11y_tree PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL A11Y-TREE CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C80_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib ole32.lib oleacc.lib oleaut32.lib
      d2d1.lib dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
      shell32.lib imm32.lib)

  add_executable(win_c80_a11y_test win/test_c80_a11y.cpp)
  target_link_libraries(win_c80_a11y_test PRIVATE ${PRISM_C80_LIBS})
  # The SDK headers are not /W4-clean; this card's own code is (B9).
  target_compile_options(win_c80_a11y_test PRIVATE /WX-)
  add_test(NAME "c80.a11y" COMMAND win_c80_a11y_test)
  # D1: the marker is a CONJUNCTION with exit 0; a window station that cannot
  # create the HWND returns 77 (a visible skip, never a fake green).
  # D29: a real budget (COM + two windows + a full UIA walk).
  set_tests_properties(c80.a11y PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL A11Y CHECKS PASSED")

  add_executable(win_c80_a11y_mutant_no_getobject win/test_c80_a11y.cpp)
  target_link_libraries(win_c80_a11y_mutant_no_getobject PRIVATE ${PRISM_C80_LIBS})
  target_compile_options(win_c80_a11y_mutant_no_getobject PRIVATE /WX-)
  target_compile_definitions(win_c80_a11y_mutant_no_getobject
                             PRIVATE PRISM_C80_MUTANT_NO_GETOBJECT=1)
  add_test(NAME "c80.mutation.no_getobject"
           COMMAND win_c80_a11y_mutant_no_getobject)
  set_tests_properties(c80.mutation.no_getobject PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C80 MUTATION CAUGHT: no_getobject")

  # The process-level UIA assertion. Registered on every Windows lane; the
  # script SKIPS (77) when agent-desktop or 7z.exe is absent, so CI without a
  # desktop driver stays honest instead of green.
  add_test(NAME "c80.a11y_uia"
           COMMAND powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass
                   -File "${CMAKE_CURRENT_SOURCE_DIR}/../tools/c80-a11y-uia.ps1"
                   -Exe "$<TARGET_FILE:PrismArchive>")
  set_tests_properties(c80.a11y_uia PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C80 UIA CHECKS PASSED")
endif()
