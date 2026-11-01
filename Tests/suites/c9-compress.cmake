# c9-compress: archive CREATION (blueprint ch.2 -- the IOutArchive::UpdateItems
# half that the product was missing). One card = one file; see suites/README.md.
#
# Decision layer (Linux lane, milliseconds): format -> method, level ->
# dictionary, solidity, thread count, encryption, and the exact
# ISetProperties payload the Windows layer will send.
add_executable(core_test_compression_plan test_compression_plan.cpp)
target_link_libraries(core_test_compression_plan PRIVATE prism_core)
add_test(NAME "core.test_compression_plan" COMMAND core_test_compression_plan)

# Four deliberate rule mutations, each compiled from the SAME source and
# registered WILL_FAIL: ctest is green only while the mutation is still caught
# (CONTRACTS D3). A mutation whose assertion silently stops matching would
# otherwise look like a sharp test.
foreach(mutant IN ITEMS NO_CLAMP NO_INPUT_CLAMP SOLID_ANY_FORMAT NO_THREAD_CLAMP)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_compression_plan_mutant_${mutant_lc}" test_compression_plan.cpp)
  target_link_libraries("test_compression_plan_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_compression_plan_mutant_${mutant_lc}"
                             PRIVATE "PRISM_CP_MUTANT_${mutant}=1")
  add_test(NAME "mutation.compress_plan.${mutant_lc}"
           COMMAND "test_compression_plan_mutant_${mutant_lc}")
  set_tests_properties("mutation.compress_plan.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # Real 7z.dll: create .7z / .zip / .tar, then re-open and compare every entry
  # (kind, size, payload bytes) plus a 7z.exe cross-check. oleaut32 supplies
  # SysAllocString (kpidPath is VT_BSTR).
  add_executable(win_write_archive win/test_write_archive.cpp)
  target_link_libraries(win_write_archive PRIVATE prism_core prism_win_bridge
                                               ole32.lib oleaut32.lib shlwapi.lib
                                               windowscodecs.lib user32.lib)
  target_compile_options(win_write_archive PRIVATE /WX-)
  add_test(NAME "win.write_archive" COMMAND win_write_archive)
  # CONTRACTS D1: the stdout marker is a CONJUNCTION with the exit code, and a
  # missing 7z.dll returns 77 (SKIP_RETURN_CODE) -- a visible Skip, never a
  # fake green.
  set_tests_properties(win.write_archive PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL WRITE-ARCHIVE CHECKS PASSED")

  # Negative controls for the two engine-side contracts this writer must never
  # get wrong: kpidIsDir (a folder announced as a file) and kpidSize (a file
  # announced without one). Each mutant asserts that the deliberate defect
  # actually reached the engine (injectedDirAsFile / injectedMissingSize), so
  # the marker plus the inverted exit code prove the injection fired.
  #
  # DELIBERATELY NOT WILL_FAIL. Measured against this CMake on 2026-11-01: when
  # PASS_REGULAR_EXPRESSION is set, a MATCHING regex passes the test and the exit
  # code is ignored; WILL_FAIL then inverts that pass into a failure, so
  # "WILL_FAIL + PASS_REGULAR_EXPRESSION on the failure marker" is always RED.
  # (And WILL_FAIL without a regex turns a SKIP into a pass -- the fake green D1
  # forbids.) The marker therefore has to be the CONJUNCTION itself: the mutant
  # binary prints WRITE-ARCHIVE MUTANT CAUGHT only when the checks went red AND
  # the injection counter is non-zero. Dead mutation -> plain failure text ->
  # regex mismatch -> RED; mutation that no longer breaks anything -> the
  # normal ALL ... PASSED marker -> regex mismatch -> RED.
  foreach(mutant IN ITEMS DIR_AS_FILE NO_SIZE)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_write_archive_mutant_${mutant_lc}" win/test_write_archive.cpp)
    target_link_libraries("win_write_archive_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge ole32.lib oleaut32.lib shlwapi.lib
                          windowscodecs.lib user32.lib)
    target_compile_options("win_write_archive_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_write_archive_mutant_${mutant_lc}"
                               PRIVATE "PRISM_CW_MUTANT_${mutant}=1")
    add_test(NAME "mutation.write_archive.${mutant_lc}"
             COMMAND "win_write_archive_mutant_${mutant_lc}")
    set_tests_properties("mutation.write_archive.${mutant_lc}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "WRITE-ARCHIVE MUTANT CAUGHT")
  endforeach()
endif()
