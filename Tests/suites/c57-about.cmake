# c57-about: the About / diagnostics window (card C57).
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# Two layers, because the card has two halves:
#   * the REPORT (PrismCore/DiagnosticsReport.hpp) is pure C++, so the shape,
#     the redaction and the caps run on every lane in milliseconds;
#   * the WINDOW (PrismWin/AboutDialog.hpp) is programmatic Win32, so it is
#     proved on the Windows lane with a real HWND: the text control is read
#     back, Copy is compared with the clipboard, Save is compared byte for byte,
#     and Tab/Enter/Esc are driven through real WM_KEYDOWN.
#
# VERDICT SHAPE (CONTRACTS D1 + D26 + D27 + D28)
#   Every target pins its stdout marker. The mutants do NOT use WILL_FAIL:
#   their binary arms mini_test's mutation verdict with the "injection fired"
#   predicate and prints "C57 MUTATION CAUGHT: <arm>" AT THE FIRST FAILURE, so a
#   stale injection (suite still green) leaves the mutant entry RED -- the guard
#   proves itself. Arm names avoid clean/unarmed/control (D28: those words mean
#   CONTROL in the mutation matrix).
add_executable(test_diagnostics_report test_diagnostics_report.cpp)
target_link_libraries(test_diagnostics_report PRIVATE prism_core)
add_test(NAME "core.diagnostics_report" COMMAND test_diagnostics_report)
set_tests_properties(core.diagnostics_report PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL DIAGNOSTICS-REPORT CHECKS PASSED")

# D3 control: the redactor hands the full path and the user name through. The
# "no full path / no user name" cases MUST go red, and the marker is only printed
# when RedactionMutantFired() proves that really happened.
add_executable(test_diagnostics_report_mutant_no_redaction test_diagnostics_report.cpp)
target_link_libraries(test_diagnostics_report_mutant_no_redaction PRIVATE prism_core)
target_compile_definitions(test_diagnostics_report_mutant_no_redaction
                           PRIVATE PRISM_C57_MUTANT_NO_REDACTION=1)
add_test(NAME "mutation.diagnostics_report.no_redaction"
         COMMAND test_diagnostics_report_mutant_no_redaction)
set_tests_properties(mutation.diagnostics_report.no_redaction PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C57 MUTATION CAUGHT: no_redaction")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # ---- the version fixture ------------------------------------------------
  # A VERSIONINFO whose version (9.8.7.6) is not the product's. The About dialog
  # is pointed at this executable, so "the version was read from the file" is the
  # only explanation for the report showing 9.8.7.6.
  add_executable(win_about_version_fixture win/about_version_fixture.cpp
                                           win/about_version_fixture.rc)
  target_compile_options(win_about_version_fixture PRIVATE /WX-)  # SDK headers: B9

  # THE CARRIED RESOURCE: the suite executable is linked with the SAME
  # Src/App/PrismArchive.rc the product ships, so the "0.1.0.0" case proves the
  # real resource file, and PRISM_C57_EXPECT_VERSION (project()'s version) is an
  # independent oracle for it -- a drift between CMakeLists.txt and the
  # VERSIONINFO goes red here, before a user reads it in Properties.
  # The sources are spelled out in every add_executable on purpose: preflight's
  # D1 gate maps a "can SKIP" source to its executable by reading this list
  # literally, and a variable there is invisible to it (a false "not registered"
  # red). The resource file is the product's own VERSIONINFO.
  set(PRISM_C57_ABOUT_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      version.lib advapi32.lib comdlg32.lib)
  set(PRISM_C57_ABOUT_DEFS
      "PRISM_C57_VERSION_FIXTURE=\"$<TARGET_FILE:win_about_version_fixture>\""
      "PRISM_C57_EXPECT_VERSION=\"${PROJECT_VERSION}.0\"")

  add_executable(win_about_dialog_test win/test_about_dialog.cpp
                 "${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.rc")
  target_link_libraries(win_about_dialog_test PRIVATE ${PRISM_C57_ABOUT_LIBS})
  target_compile_options(win_about_dialog_test PRIVATE /WX-)  # SDK headers: B9
  target_compile_definitions(win_about_dialog_test PRIVATE ${PRISM_C57_ABOUT_DEFS})
  add_dependencies(win_about_dialog_test win_about_version_fixture)
  add_test(NAME "win.about_dialog" COMMAND win_about_dialog_test)
  set_tests_properties(win.about_dialog PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL ABOUT-DIALOG CHECKS PASSED")
  # TIMEOUT 120: every wait inside the suite is bounded (clipboard retries are
  # 20 x 10 ms), so a hang is a red that names the test instead of a stalled lane
  # (CONTRACTS D29). 77 = the documented SKIP for a session with no window
  # station; a SKIP never passes because the marker above is still required.

  # ---- the two negative controls, compiled from the SAME sources -----------
  #   no_redaction        PrismCore's redactor is compiled away -> the control's
  #                       text carries a full path and the user name;
  #   hardcoded_version   AboutDialog returns the literal "0.1.0.0" -> the
  #                       fixture case (9.8.7.6) and the "no VERSIONINFO ->
  #                       unknown" case both go red.
  foreach(arm IN ITEMS no_redaction hardcoded_version)
    if(arm STREQUAL "no_redaction")
      set(arm_define "PRISM_C57_MUTANT_NO_REDACTION=1")
    else()
      set(arm_define "PRISM_C57_MUTANT_HARDCODED_VERSION=1")
    endif()
    add_executable("win_about_dialog_mutant_${arm}" win/test_about_dialog.cpp
                   "${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.rc")
    target_link_libraries("win_about_dialog_mutant_${arm}" PRIVATE ${PRISM_C57_ABOUT_LIBS})
    target_compile_options("win_about_dialog_mutant_${arm}" PRIVATE /WX-)
    target_compile_definitions("win_about_dialog_mutant_${arm}" PRIVATE ${PRISM_C57_ABOUT_DEFS}
                               "${arm_define}")
    add_dependencies("win_about_dialog_mutant_${arm}" win_about_version_fixture)
    add_test(NAME "mutation.about_dialog.${arm}"
             COMMAND "win_about_dialog_mutant_${arm}")
    set_tests_properties("mutation.about_dialog.${arm}" PROPERTIES TIMEOUT 120
        PASS_REGULAR_EXPRESSION "C57 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
