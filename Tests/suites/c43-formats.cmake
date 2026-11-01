# c43-formats: format coverage -- the sniffed handler table for rar/iso/cab/
# wim/msi/chm/cpio/arj/lzh/dmg, and the EXPLICIT "this engine cannot read it"
# answer instead of a whole-file probe.
#
# Three executables from one source (the C28 pattern):
#   1. win.format_coverage        -- pure sniffer rules + real 7z.dll + sparse
#                                    8 MiB fixtures (head/tail only). Byte gate:
#                                    Open() must read < 64 KiB of each fixture.
#   2. win.format_coverage_sniff  -- the pure half only, no 7z.dll needed.
#   3. mutation.format_coverage.no_iso_sniff -- the iso rule deleted at compile
#      time. CONTRACTS D26: NO WILL_FAIL next to PASS_REGULAR_EXPRESSION. The
#      marker "C43 MUTANT CAUGHT" is the conjunction -- printed only when a
#      byte-gate assertion really went red AND the injection counter proves the
#      deletion fired (CONTRACTS D3).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # D1: this is the FIRST add_executable for the source, so preflight maps the
  # SKIP-capable source to it -- the marker must stay pinned right here.
  add_executable(win_format_coverage_test win/test_format_coverage.cpp)
  target_link_libraries(win_format_coverage_test PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib windowscodecs.lib)
  target_compile_options(win_format_coverage_test PRIVATE /WX-)
  add_test(NAME "win.format_coverage" COMMAND win_format_coverage_test)
  set_tests_properties(win.format_coverage PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL FORMAT-COVERAGE CHECKS PASSED")

  add_executable(win_format_coverage_sniff_test win/test_format_coverage.cpp)
  target_link_libraries(win_format_coverage_sniff_test PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib windowscodecs.lib)
  target_compile_options(win_format_coverage_sniff_test PRIVATE /WX-)
  target_compile_definitions(win_format_coverage_sniff_test PRIVATE PRISM_C43_SNIFF_ONLY=1)
  add_test(NAME "win.format_coverage_sniff" COMMAND win_format_coverage_sniff_test)
  set_tests_properties(win.format_coverage_sniff PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL FORMAT-COVERAGE SNIFFER CHECKS PASSED")

  add_executable(win_format_coverage_mutant_test win/test_format_coverage.cpp)
  target_link_libraries(win_format_coverage_mutant_test PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib windowscodecs.lib)
  target_compile_options(win_format_coverage_mutant_test PRIVATE /WX-)
  target_compile_definitions(win_format_coverage_mutant_test PRIVATE
                             PRISM_C43_MUTANT_NO_ISO_SNIFF=1)
  add_test(NAME "mutation.format_coverage.no_iso_sniff"
           COMMAND win_format_coverage_mutant_test)
  set_tests_properties(mutation.format_coverage.no_iso_sniff PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77 PASS_REGULAR_EXPRESSION "C43 MUTANT CAUGHT")
endif()
