# c42-reportlane: the BENCH extractor that feeds the run summary (card C42).
#
# Why this card exists (C32, 2026-11-02): report.yml ran
# `ctest --output-on-failure -R "bench"`, and ctest does NOT print the stdout of
# a PASSING test. Every `BENCH {json}` line from a green bench suite therefore
# stayed invisible: the summary step printed "(no BENCH lines)" and the run was
# still green. bench_scale / bench_memory / bench_startup were affected the same
# way. A report that silently drops a suite is worse than no report.
#
# The parser (tools/report/extract_bench.py) is therefore tested like a gate, not
# like a helper: fake logs with good AND malformed BENCH lines, no dependency on
# a real benchmark run. Cases: exact table rows for BENCH / BENCH_STARTUP /
# BENCH_MEMORY / BENCH_SCALE / BENCH_GLASS_MEM / CORE-BENCH, truncated JSON,
# non-object, NaN, empty object, marker without payload, trailing junk (all red,
# with the log line number), conflicting duplicates (red), identical duplicates
# (collapsed), a Passed bench suite that printed no BENCH line (red -- the C32
# defect itself), a raw executable log, and an unreadable log.
#
# D1: the stdout marker is part of the verdict -- CTest needs exit 0 AND
# "EXTRACT-BENCH SELFTEST: PASSED", so a silent skip or a crash after printing is
# still red. D26: no WILL_FAIL anywhere; the mutation is demonstrated by
# commenting out the parser's bad-line error (see docs/PERF.md), and this target
# must then go red on its own assertions.
#
# Python-only, so it runs on every platform. NOTE: ci.yml's Windows lane excludes
# "bench" from the merge gate (that exclusion is about 4 GiB fixtures and a 10k
# corpus); this test costs milliseconds and runs in the Linux gates lane, which
# runs the FULL ctest.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME "report.extract_bench"
           COMMAND ${Python3_EXECUTABLE}
                   ${CMAKE_CURRENT_SOURCE_DIR}/../tools/report/test_extract_bench.py)
  set_tests_properties(report.extract_bench PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "EXTRACT-BENCH SELFTEST: PASSED")
else()
  message(STATUS "c42: Python3 not found -- report.extract_bench not registered")
endif()
