# c18-compressdialog: the "create an archive" dialog -- the UI half of blueprint
# ch.2 (card C9 shipped CompressionPlan.hpp; this card consumes it).
#
# ONE source file, THREE executables. The pure DialogState is proved headless on
# every lane; the same rules are then proved again through a REAL HWND
# (CreateWindowExW + child controls + real WM_COMMAND/WM_NOTIFY/WM_HSCROLL).
# A rule that only ran on Windows would be skipped by the Linux lane; a rule
# that only ran headless would never prove the dialog exists.
#
# The same TU carries the Win32 half behind #if defined(_WIN32), so even the
# "core" target needs comctl32 on Windows (the level slider is
# msctls_trackbar32, reached through InitCommonControlsEx). On Linux the list is
# empty and the target is exactly prism_core.
set(PRISM_C18_WIN_LIBS "")
if(WIN32)
  set(PRISM_C18_WIN_LIBS comctl32.lib user32.lib gdi32.lib ole32.lib)
endif()

# The Windows block is FIRST on purpose: tools/preflight.py's D1 gate maps a
# source file to the FIRST add_executable that mentions it and then requires
# PASS_REGULAR_EXPRESSION on that executable's test. Registering the core
# target first would make the gate report a false D1 red for a test that skips.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_compress_dialog win/test_compress_dialog.cpp)
  target_link_libraries(win_compress_dialog PRIVATE prism_core prism_win_bridge
                        ${PRISM_C18_WIN_LIBS})
  target_compile_options(win_compress_dialog PRIVATE /WX-)
  add_test(NAME "win.compress_dialog" COMMAND win_compress_dialog)
  # CONTRACTS D1: the stdout marker is a CONJUNCTION with the exit code (ctest
  # needs BOTH), and a missing window station returns 77 (SKIP_RETURN_CODE) with
  # a printed reason -- a visible Skip, never a fake green.
  set_tests_properties(win.compress_dialog PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL COMPRESS-DIALOG CHECKS PASSED")

  # CONTRACTS D3 negative control: the zip+solid downgrade is commented out of
  # DialogState (PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED), so the UI keeps promising a
  # solid zip. The suite MUST go red.
  #
  # DELIBERATELY NOT WILL_FAIL (CONTRACTS D26): with PASS_REGULAR_EXPRESSION set,
  # a matching regex passes the test and the exit code is ignored; WILL_FAIL then
  # inverts that pass into a failure, i.e. an always-red test. The marker is the
  # conjunction instead -- the mutant binary prints COMPRESS-DIALOG MUTANT CAUGHT
  # only when the run went red AND DialogState::MutationFired() is non-zero (the
  # defect actually fired). A dead mutation prints no marker -> regex mismatch ->
  # RED, which is exactly the "the mutation stopped working" alarm D3 wants.
  #
  # No SKIP_RETURN_CODE here: a mutant that cannot run must never look green.
  add_executable(win_compress_dialog_mutant_zip_solid win/test_compress_dialog.cpp)
  target_link_libraries(win_compress_dialog_mutant_zip_solid PRIVATE prism_core
                        prism_win_bridge ${PRISM_C18_WIN_LIBS})
  target_compile_options(win_compress_dialog_mutant_zip_solid PRIVATE /WX-)
  target_compile_definitions(win_compress_dialog_mutant_zip_solid
                             PRIVATE PRISM_CD_MUTANT_ZIP_SOLID_ALLOWED=1)
  add_test(NAME "mutation.compress_dialog.zip_solid"
           COMMAND win_compress_dialog_mutant_zip_solid)
  set_tests_properties(mutation.compress_dialog.zip_solid PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "COMPRESS-DIALOG MUTANT CAUGHT")
endif()

# The pure rules: every lane, no window, milliseconds. This is where the
# combination matrix lives (each illegal pair, each disabled control's reason,
# the unit conversions, the defaults, the estimate and the OK enablement).
add_executable(core_test_compress_dialog win/test_compress_dialog.cpp)
target_link_libraries(core_test_compress_dialog PRIVATE prism_core ${PRISM_C18_WIN_LIBS})
add_test(NAME "core.test_compress_dialog" COMMAND core_test_compress_dialog)

# Two rule mutations, compiled from the SAME source and registered WILL_FAIL:
# ctest is green only while the mutation is still caught (CONTRACTS D3). These
# carry no PASS_REGULAR_EXPRESSION, so WILL_FAIL and the regex never meet
# (CONTRACTS D26) and the core binary cannot skip (it returns 0/1 only).
foreach(mutant IN ITEMS ZIP_SOLID_ALLOWED TAR_ENCRYPT_ALLOWED)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("core_test_compress_dialog_mutant_${mutant_lc}"
                 win/test_compress_dialog.cpp)
  target_link_libraries("core_test_compress_dialog_mutant_${mutant_lc}"
                        PRIVATE prism_core ${PRISM_C18_WIN_LIBS})
  target_compile_definitions("core_test_compress_dialog_mutant_${mutant_lc}"
                             PRIVATE "PRISM_CD_MUTANT_${mutant}=1")
  add_test(NAME "mutation.compress_dialog.${mutant_lc}"
           COMMAND "core_test_compress_dialog_mutant_${mutant_lc}")
  set_tests_properties("mutation.compress_dialog.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()
