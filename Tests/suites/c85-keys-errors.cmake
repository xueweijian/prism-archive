# c85-keys-errors: THE KEYBOARD AND THE FAILURE SURFACE (card C85).
#
# Four defects, all reproduced by hand against the shipped binary before a line
# of test existed, all with a root cause that the EXISTING suites could not see:
#
#   1. Ctrl+Tab / Ctrl+Shift+Tab were dead. GalleryWindow::HandleActionBarKey()
#      answered "return true" for every VK_TAB, so HandleTabKey() -- which
#      implements both chords -- was unreachable. c60.tab_bar missed it because
#      it calls DispatchKeyStroke(), i.e. it enters AFTER the band's decision.
#   2. Ctrl+W was dead by the same route (see win_c85_keys case 2).
#   3. A failed open showed a dialog with no reason: "CreateFileW failed; " with
#      an empty tail, because the Win32 code and the path were dropped where
#      they were known.
#   4. No log file existed at all, and every stderr line was stamped
#      1970-01-01T00:00:00.000Z (no clock was ever installed).
#   (+) and the tab strip published no CLOSE entry to assistive technology: a
#      UIA walk stopped at "tab | Photos.7z".
#
# THE LAYERS
#   core.test_c85_log   the pure rules: the UTC day a log file is named after,
#                       the clock seam that stops a record formatting as 1970,
#                       and the rotation suffix. Runs everywhere (Linux lane).
#   c85.keys            the REAL window: WM_KEYDOWN with a real thread key
#                       state (the route a physical Ctrl+Tab takes), the title
#                       following the active tab, and the MSAA tree through
#                       AccessibleObjectFromWindow -- two tabs, two close
#                       buttons, accDoDefaultAction closes one.
#   c85.errors          the SHIPPED exe: three failure classes each produce a
#                       non-empty message box carrying its own reason (missing
#                       file, engine refusal, missing volume), and one launch
#                       leaves <LOCALAPPDATA>\PrismArchive\logs\prism-YYYYMMDD.log
#                       whose first line has a REAL year and carries this run's
#                       pid.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27 + D28). No WILL_FAIL anywhere:
# each mutant is the SAME test source (or the same app source) with ONE rule
# injected, it arms mini_test's verdict, prints "C85 MUTATION CAUGHT: <arm>" at
# the FIRST red assertion and exits 0, so CTest's PASS_REGULAR_EXPRESSION is the
# verdict. The arms:
#   band_eats_ctrl_tab  the band claims every Tab again (the shipped defect)
#   no_close_node       the tab publishes no close entry
#   empty_reason        the dialog's reason is dropped (the app mutant)
#   no_file_log         the file sink writes nothing (the app mutant)
# Arm names avoid clean/unarmed/control (D28). The unarmed controls are the
# three plain tests: same sources, no injection define.
add_executable(test_c85_log test_c85_log.cpp)
target_link_libraries(test_c85_log PRIVATE prism_core)
add_test(NAME "core.test_c85_log" COMMAND test_c85_log)
set_tests_properties(core.test_c85_log PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C85 LOG CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C85_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib ole32.lib oleacc.lib oleaut32.lib
      d2d1.lib dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
      shell32.lib imm32.lib)

  add_executable(win_c85_keys win/test_c85_keys.cpp)
  target_link_libraries(win_c85_keys PRIVATE ${PRISM_C85_LIBS})
  # The SDK headers are not /W4-clean; this card's own code is.
  target_compile_options(win_c85_keys PRIVATE /WX-)
  add_test(NAME "c85.keys" COMMAND win_c85_keys)
  # D29: a real budget (two windows, one MSAA walk, a few key presses). D1: the
  # marker is a CONJUNCTION with exit 0; a station with no window is 77.
  set_tests_properties(c85.keys PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C85 KEY CHECKS PASSED")

  # Arm 1: the band eats Ctrl+Tab again. Case 1 MUST go red.
  add_executable(win_c85_keys_mutant_band_eats_ctrl_tab win/test_c85_keys.cpp)
  target_link_libraries(win_c85_keys_mutant_band_eats_ctrl_tab PRIVATE ${PRISM_C85_LIBS})
  target_compile_options(win_c85_keys_mutant_band_eats_ctrl_tab PRIVATE /WX-)
  target_compile_definitions(win_c85_keys_mutant_band_eats_ctrl_tab
                             PRIVATE PRISM_C85_MUTANT_BAND_EATS_CTRL_TAB=1)
  add_test(NAME "c85.mutation.band_eats_ctrl_tab"
           COMMAND win_c85_keys_mutant_band_eats_ctrl_tab)
  set_tests_properties(c85.mutation.band_eats_ctrl_tab PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C85 MUTATION CAUGHT: band_eats_ctrl_tab")

  # Arm 2: no close entry in the tree. Case 3 MUST go red.
  add_executable(win_c85_keys_mutant_no_close_node win/test_c85_keys.cpp)
  target_link_libraries(win_c85_keys_mutant_no_close_node PRIVATE ${PRISM_C85_LIBS})
  target_compile_options(win_c85_keys_mutant_no_close_node PRIVATE /WX-)
  target_compile_definitions(win_c85_keys_mutant_no_close_node
                             PRIVATE PRISM_C85_MUTANT_NO_CLOSE_NODE=1)
  add_test(NAME "c85.mutation.no_close_node"
           COMMAND win_c85_keys_mutant_no_close_node)
  set_tests_properties(c85.mutation.no_close_node PROPERTIES TIMEOUT 120
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C85 MUTATION CAUGHT: no_close_node")

  add_executable(win_c85_errors win/test_c85_errors.cpp)
  # prism_shell_contract: AppWiring.hpp (the ONE failure-sentence builder) is
  # included here, and it reaches the registrar's flag constants through
  # SparsePackageRegistrar.hpp -- the same link contract win_app_wiring carries.
  target_link_libraries(win_c85_errors PRIVATE ${PRISM_C85_LIBS} prism_shell_contract)
  target_compile_options(win_c85_errors PRIVATE /WX-)
  target_compile_definitions(win_c85_errors
                             PRIVATE "PRISM_C85_EXE=\"$<TARGET_FILE:PrismArchive>\"")
  add_dependencies(win_c85_errors PrismArchive)
  add_test(NAME "c85.errors" COMMAND win_c85_errors)
  # Three message boxes, one headless run, three 7z.exe fixtures: a real budget.
  set_tests_properties(c85.errors PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C85 ERROR-SURFACE CHECKS PASSED")

  # Arm 3: the app drops the failure reason. "the dialog says 找不到文件" MUST go
  # red on the mutant exe (it shows only the title and "(no reason reported)").
  add_executable(prism_archive_mutant_c85_empty_reason
                 "${CMAKE_SOURCE_DIR}/Src/App/Main.cpp"
                 "${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.rc")
  target_include_directories(prism_archive_mutant_c85_empty_reason
                             PRIVATE "${CMAKE_SOURCE_DIR}/Src")
  target_link_libraries(prism_archive_mutant_c85_empty_reason PRIVATE prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib comdlg32.lib shell32.lib shlwapi.lib comctl32.lib
                        oleacc.lib)
  set_target_properties(prism_archive_mutant_c85_empty_reason
                        PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(prism_archive_mutant_c85_empty_reason
                             PRIVATE UNICODE _UNICODE
                                     PRISM_C85_MUTANT_EMPTY_REASON=1)
  target_compile_options(prism_archive_mutant_c85_empty_reason PRIVATE /WX-)
  add_executable(win_c85_errors_mutant_empty_reason win/test_c85_errors.cpp)
  target_link_libraries(win_c85_errors_mutant_empty_reason PRIVATE ${PRISM_C85_LIBS}
                        prism_shell_contract)
  target_compile_options(win_c85_errors_mutant_empty_reason PRIVATE /WX-)
  target_compile_definitions(win_c85_errors_mutant_empty_reason
                             PRIVATE "PRISM_C85_EXE=\"$<TARGET_FILE:prism_archive_mutant_c85_empty_reason>\""
                                     "PRISM_C85_MUTANT_NAME=\"empty_reason\"")
  add_dependencies(win_c85_errors_mutant_empty_reason
                   prism_archive_mutant_c85_empty_reason)
  add_test(NAME "c85.mutation.empty_reason"
           COMMAND win_c85_errors_mutant_empty_reason)
  set_tests_properties(c85.mutation.empty_reason PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C85 MUTATION CAUGHT: empty_reason")

  # Arm 4: the file sink writes nothing (the pre-C85 shape: stderr only).
  add_executable(prism_archive_mutant_c85_no_file_log
                 "${CMAKE_SOURCE_DIR}/Src/App/Main.cpp"
                 "${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.rc")
  target_include_directories(prism_archive_mutant_c85_no_file_log
                             PRIVATE "${CMAKE_SOURCE_DIR}/Src")
  target_link_libraries(prism_archive_mutant_c85_no_file_log PRIVATE prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib comdlg32.lib shell32.lib shlwapi.lib comctl32.lib
                        oleacc.lib)
  set_target_properties(prism_archive_mutant_c85_no_file_log
                        PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(prism_archive_mutant_c85_no_file_log
                             PRIVATE UNICODE _UNICODE
                                     PRISM_C85_MUTANT_NO_FILE_LOG=1)
  target_compile_options(prism_archive_mutant_c85_no_file_log PRIVATE /WX-)
  add_executable(win_c85_errors_mutant_no_file_log win/test_c85_errors.cpp)
  target_link_libraries(win_c85_errors_mutant_no_file_log PRIVATE ${PRISM_C85_LIBS}
                        prism_shell_contract)
  target_compile_options(win_c85_errors_mutant_no_file_log PRIVATE /WX-)
  target_compile_definitions(win_c85_errors_mutant_no_file_log
                             PRIVATE "PRISM_C85_EXE=\"$<TARGET_FILE:prism_archive_mutant_c85_no_file_log>\""
                                     "PRISM_C85_MUTANT_NAME=\"no_file_log\"")
  add_dependencies(win_c85_errors_mutant_no_file_log
                   prism_archive_mutant_c85_no_file_log)
  add_test(NAME "c85.mutation.no_file_log"
           COMMAND win_c85_errors_mutant_no_file_log)
  set_tests_properties(c85.mutation.no_file_log PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C85 MUTATION CAUGHT: no_file_log")
endif()
