# c26-engine: the ENGINE CALLBACK'S FAILURE SEMANTICS + extension -> CLSID routing.
#
# One card = one file; see suites/README.md. Two things are proven here against
# the REAL 7z.dll:
#   1. IInArchive::Extract() answers S_OK while SetOperationResult(opResult)
#      reports kUnsupportedMethod. The bridge must surface that as a FAILURE
#      with an actionable reason -- before C26 it was a "successful" extraction
#      of zero bytes (card C17 measured the engine doing exactly this).
#   2. ArchiveReader::Open routes by extension (table) instead of brute-forcing
#      two handlers, which is what makes a bare .zst / .tar / .gz openable.
#
# D1: the stdout marker is part of the verdict (exit 0 AND the marker), and a
# missing 7z.dll exits 77 -> a visible Skip, never a fake green.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_extract_failure_test win/test_extract_failure.cpp)
  target_link_libraries(win_extract_failure_test PRIVATE prism_core prism_win_bridge
                                                     ole32.lib oleaut32.lib)
  target_compile_options(win_extract_failure_test PRIVATE /WX-)
  add_test(NAME "win.extract_failure" COMMAND win_extract_failure_test)
  set_tests_properties(win.extract_failure PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL EXTRACT-FAILURE CHECKS PASSED")

  # Negative control (CONTRACTS D3): the SAME source built with the pre-C26
  # defect injected -- SetOperationResult reads opResult and throws it away.
  # The opResult checks MUST go red, and the binary prints MUTANT CAUGHT only
  # when they did AND the injection counter proves the mutation fired.
  #
  # DELIBERATELY NOT WILL_FAIL (CONTRACTS D26): with PASS_REGULAR_EXPRESSION set,
  # a matching regex passes and the exit code is ignored; adding WILL_FAIL would
  # invert that pass into an always-red test. The marker IS the conjunction:
  # dead mutation -> normal marker -> regex mismatch -> RED.
  add_executable(win_extract_failure_mutant win/test_extract_failure.cpp)
  target_link_libraries(win_extract_failure_mutant PRIVATE prism_core prism_win_bridge
                                                         ole32.lib oleaut32.lib)
  target_compile_options(win_extract_failure_mutant PRIVATE /WX-)
  target_compile_definitions(win_extract_failure_mutant
                             PRIVATE PRISM_C26_MUTANT_IGNORE_OPRESULT=1)
  add_test(NAME "mutation.extract_failure.ignore_opresult"
           COMMAND win_extract_failure_mutant)
  set_tests_properties(mutation.extract_failure.ignore_opresult PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "EXTRACT-FAILURE MUTANT CAUGHT")

  # Bare .zst, its own executable: 7-Zip 25.x decodes zstd, older engines do
  # not. On an engine without the handler the test exits 77 (visible Skip)
  # instead of asserting an environment fact (CONTRACTS D21).
  add_executable(win_raw_zst_test win/test_extract_failure.cpp)
  target_link_libraries(win_raw_zst_test PRIVATE prism_core prism_win_bridge
                                               ole32.lib oleaut32.lib)
  target_compile_options(win_raw_zst_test PRIVATE /WX-)
  target_compile_definitions(win_raw_zst_test PRIVATE PRISM_C26_ZST_ONLY=1)
  add_test(NAME "win.raw_zst" COMMAND win_raw_zst_test)
  set_tests_properties(win.raw_zst PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL EXTRACT-FAILURE AND RAW-ZST CHECKS PASSED")
endif()
