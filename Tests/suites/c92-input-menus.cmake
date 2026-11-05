# c92-input-menus: the two interactions the field report measured as dead.
#
#   * DOUBLE-CLICK: the window class now carries CS_DBLCLKS (without it Windows
#     never posts WM_LBUTTONDBLCLK, and the handler in GalleryWindow was dead
#     code). A POSTED DOWN/UP/DBLCLK/UP sequence must enter a folder and must
#     open a file through the production helper -- the entry lands in a
#     %TEMP%\PrismArchive-Open directory with its bytes. Enter is dispatched in
#     the same case and must reach the SAME handler.
#   * RIGHT-CLICK: WM_CONTEXTMENU / Shift+F10 build three DIFFERENT menus (row /
#     empty space / sidebar node) from PrismCore's model, each >= 5 entries, with
#     pairwise-disjoint id sets; a disabled item's reason reaches the status line.
#
# LAYERS: the model is pure PrismCore (core.c92_context_menu) and runs on every
# lane; the window is proven against a REAL HWND and a REAL 7z.dll
# (win.c92_input_menus).
#
# NEGATIVE CONTROLS (D3/D26/D27): one executable per injected defect, compiled
# from the SAME source. Each prints "C92 MUTATION CAUGHT: <arm>" at its FIRST
# failure and exits 0; CTest's PASS_REGULAR_EXPRESSION is that marker. No
# WILL_FAIL is stacked on a regex anywhere in this file.
#
# D1: the Windows case is an integration test. It needs a real 7z.dll + 7z.exe
# (PRISM_7Z_DLL) to author and read the archive; missing => exit 77 and the
# suite reports SKIP through CTest. The PASS regex still names the marker, so a
# silent early return cannot pass.
add_executable(core_c92_context_menu_test test_c92_context_menu_core.cpp)
target_link_libraries(core_c92_context_menu_test PRIVATE prism_core)
target_compile_definitions(core_c92_context_menu_test
                           PRIVATE PRISM_C92_LANG_DIR="${CMAKE_SOURCE_DIR}/Assets/Lang")
add_test(NAME "core.c92_context_menu" COMMAND core_c92_context_menu_test)
set_tests_properties(core.c92_context_menu PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C92 INPUT-MENU CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C92_LIBS
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

  add_executable(win_c92_input_menus win/test_c92_input_menus.cpp)
  target_link_libraries(win_c92_input_menus PRIVATE ${PRISM_C92_LIBS})
  target_compile_options(win_c92_input_menus PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.c92_input_menus" COMMAND win_c92_input_menus)
  set_tests_properties(win.c92_input_menus PROPERTIES
      TIMEOUT 180
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C92_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "ALL C92 INPUT-MENU CHECKS PASSED")

  # Arm 1: the class loses CS_DBLCLKS again -- the pre-fix state where the
  # double-click handler is unreachable.
  add_executable(win_c92_input_menus_mutant_no_dblclks win/test_c92_input_menus.cpp)
  target_link_libraries(win_c92_input_menus_mutant_no_dblclks PRIVATE ${PRISM_C92_LIBS})
  target_compile_options(win_c92_input_menus_mutant_no_dblclks PRIVATE /WX-)
  target_compile_definitions(win_c92_input_menus_mutant_no_dblclks
                             PRIVATE PRISM_C92_MUTANT_NO_DBLCLKS=1)
  add_test(NAME "mutation.c92.no_dblclks"
           COMMAND win_c92_input_menus_mutant_no_dblclks)
  set_tests_properties(mutation.c92.no_dblclks PROPERTIES
      TIMEOUT 180
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C92_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "C92 MUTATION CAUGHT: no_dblclks")

  # Arm 2: WM_CONTEXTMENU opens nothing -- the "right-click does nothing" bug.
  add_executable(win_c92_input_menus_mutant_no_context win/test_c92_input_menus.cpp)
  target_link_libraries(win_c92_input_menus_mutant_no_context PRIVATE ${PRISM_C92_LIBS})
  target_compile_options(win_c92_input_menus_mutant_no_context PRIVATE /WX-)
  target_compile_definitions(win_c92_input_menus_mutant_no_context
                             PRIVATE PRISM_C92_MUTANT_NO_CONTEXT=1)
  add_test(NAME "mutation.c92.no_context"
           COMMAND win_c92_input_menus_mutant_no_context)
  set_tests_properties(mutation.c92.no_context PROPERTIES
      TIMEOUT 180
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C92_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "C92 MUTATION CAUGHT: no_context")

  # Arm 3 (follow-up): the right-click no longer selects the row under it, so
  # "Open" is greyed on the very file the user pointed at.
  add_executable(win_c92_input_menus_mutant_no_preselect win/test_c92_input_menus.cpp)
  target_link_libraries(win_c92_input_menus_mutant_no_preselect PRIVATE ${PRISM_C92_LIBS})
  target_compile_options(win_c92_input_menus_mutant_no_preselect PRIVATE /WX-)
  target_compile_definitions(win_c92_input_menus_mutant_no_preselect
                             PRIVATE PRISM_C92_MUTANT_NO_PRESELECT=1)
  add_test(NAME "mutation.c92.no_preselect"
           COMMAND win_c92_input_menus_mutant_no_preselect)
  set_tests_properties(mutation.c92.no_preselect PROPERTIES
      TIMEOUT 180
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C92_LANG_DIR=${CMAKE_SOURCE_DIR}/Assets/Lang"
      PASS_REGULAR_EXPRESSION "C92 MUTATION CAUGHT: no_preselect")
endif()
