# c122-list: THE DEFAULT VIEW IS THE LIST, with Bandizip's four columns.
#
# The defect this card removes is structural, so the assertions are too:
#   win.c122_list                 the real GalleryRenderer (headless WIC frame)
#                                 and the real GalleryWindow: L1 default list,
#                                 L2 four disjoint column cells ending on the
#                                 content edge - 20 DIP, L3 26/22 DIP
#                                 row tiers, L4 the five type words + the empty
#                                 unknown, L5 one numeric baseline per column,
#                                 L6 header sorting + View > 网格/列表.
#   mutation.c122.grid_default    PRISM_C122_MUTANT_GRID_DEFAULT: Scene{}.listMode
#                                 is false again -> L1 MUST go red.
#   mutation.c122.no_type         PRISM_C122_MUTANT_NO_TYPE: every type cell is ""
#                                 -> L4 MUST go red.
# NEGATIVE CONTROLS follow CONTRACTS D3/D26/D27: no WILL_FAIL anywhere; each
# mutant prints "C122 MUTATION CAUGHT: <arm>" at the FIRST failing check (armed
# on the injected observable, not on the macro) and the CTest entry's
# PASS_REGULAR_EXPRESSION is the verdict.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C122_LIST_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
      ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)

  add_executable(win_c122_list win/test_c122_list.cpp)
  target_link_libraries(win_c122_list PRIVATE ${PRISM_C122_LIST_LIBS})
  target_compile_options(win_c122_list PRIVATE /WX-)
  add_test(NAME "win.c122_list" COMMAND win_c122_list)
  set_tests_properties(win.c122_list PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C122 LIST CHECKS PASSED")

  add_executable(win_c122_mutant_grid_default win/test_c122_list.cpp)
  target_link_libraries(win_c122_mutant_grid_default PRIVATE ${PRISM_C122_LIST_LIBS})
  target_compile_options(win_c122_mutant_grid_default PRIVATE /WX-)
  target_compile_definitions(win_c122_mutant_grid_default
                             PRIVATE PRISM_C122_MUTANT_GRID_DEFAULT=1)
  add_test(NAME "mutation.c122.grid_default" COMMAND win_c122_mutant_grid_default)
  set_tests_properties(mutation.c122.grid_default PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C122 MUTATION CAUGHT: grid_default")

  add_executable(win_c122_mutant_no_type win/test_c122_list.cpp)
  target_link_libraries(win_c122_mutant_no_type PRIVATE ${PRISM_C122_LIST_LIBS})
  target_compile_options(win_c122_mutant_no_type PRIVATE /WX-)
  target_compile_definitions(win_c122_mutant_no_type PRIVATE PRISM_C122_MUTANT_NO_TYPE=1)
  add_test(NAME "mutation.c122.no_type" COMMAND win_c122_mutant_no_type)
  set_tests_properties(mutation.c122.no_type PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C122 MUTATION CAUGHT: no_type")
endif()
