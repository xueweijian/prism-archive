# c28-openperf: Open() routes by the file's own bytes, not by a probe walk.
#
# Why this suite exists (C7 measured 78.5 s to open a 4 GiB archive, C17
# corroborated it): the pre-C28 code handed every candidate handler a chance to
# walk the archive before it declined. FormatSniffer reads 64 head bytes plus
# the last 1 KiB, says what the container IS, and Open() then asks exactly one
# handler for a format proven by its magic (the extension route and the legacy
# [Zip, 7z] probe stay behind it, so a wrong guess costs nothing but time).
#
# Three executables from one source, each with its own contract:
#   1. win.format_sniffer   -- the pure magic table (no 7z.dll needed);
#   2. win.open_perf        -- real 7z.dll, real 4 GiB sparse fixtures: Open
#                              must read <= 2 MiB of a 4 GiB archive, and the
#                              routing must say "sniff=<fmt> route=sniffed";
#   3. mutation.open_perf.probe_all -- the pre-C28 defect (sniff thrown away).
#      CONTRACTS D26: NO WILL_FAIL next to PASS_REGULAR_EXPRESSION. The marker
#      "C28 MUTANT CAUGHT" is the conjunction -- printed only when a byte-gate
#      assertion actually went red AND the injection counter proves the
#      mutation fired (CONTRACTS D3). A dead mutation prints NOT CAUGHT -> red.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_format_sniffer_test win/test_open_perf.cpp)
  target_link_libraries(win_format_sniffer_test PRIVATE prism_core prism_win_bridge
                                                   ole32.lib oleaut32.lib)
  target_compile_options(win_format_sniffer_test PRIVATE /WX-)
  target_compile_definitions(win_format_sniffer_test PRIVATE PRISM_C28_SNIFF_ONLY=1)
  add_test(NAME "win.format_sniffer" COMMAND win_format_sniffer_test)
  set_tests_properties(win.format_sniffer PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL FORMAT-SNIFFER CHECKS PASSED")

  add_executable(win_open_perf_test win/test_open_perf.cpp)
  target_link_libraries(win_open_perf_test PRIVATE prism_core prism_win_bridge
                                                ole32.lib oleaut32.lib windowscodecs.lib)
  target_compile_options(win_open_perf_test PRIVATE /WX-)
  add_test(NAME "win.open_perf" COMMAND win_open_perf_test)
  # 600 s: the routing arms are milliseconds, but a machine without sparse-file
  # support materialises the 4 GiB holes, and the mutant genuinely reads 4 GiB.
  set_tests_properties(win.open_perf PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL OPEN-PERF CHECKS PASSED")

  add_executable(win_open_perf_mutant win/test_open_perf.cpp)
  target_link_libraries(win_open_perf_mutant PRIVATE prism_core prism_win_bridge
                                                    ole32.lib oleaut32.lib windowscodecs.lib)
  target_compile_options(win_open_perf_mutant PRIVATE /WX-)
  target_compile_definitions(win_open_perf_mutant PRIVATE PRISM_C28_MUTANT_PROBE_ALL=1)
  add_test(NAME "mutation.open_perf.probe_all" COMMAND win_open_perf_mutant)
  set_tests_properties(mutation.open_perf.probe_all PROPERTIES TIMEOUT 600 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C28 MUTANT CAUGHT")
endif()
