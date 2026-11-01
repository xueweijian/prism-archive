# c49-remeasure: the numbers AFTER the assembly (card C49).
#
# docs/PERF.md's headline digits (first frame 22.7 ms, resident 58.4 MB, 100k-row
# p95 15.6 ms, 4 GiB open 78.5 s) were taken before V1 (layout), V2 (action
# island), C29 (sidebar rail), C45 (thumbnail pipeline), C43 (sniffing) and C28
# (routing) were merged. This suite measures the assembled app on one box, in
# one run, and publishes five BENCH lines:
#
#   BENCH_C49_STARTUP / _MEMORY / _OPEN4G / _FRAME / _THUMBS
#
# C42 report lane: one well-formed JSON object per line and exactly ONE line per
# marker (the extractor treats a second line with the same (test, marker) and a
# different payload as an ambiguity error -- exit 3). The test name contains
# "bench", so a green run that prints no BENCH line is a report failure (exit 5)
# rather than a silent omission.
#
# D1: the stdout marker is part of the verdict (ctest needs exit 0 AND the line),
# so a silent skip or a crash after printing is still red. A runner without
# 7z.dll/7z.exe prints WHY and exits 77 (SKIP_RETURN_CODE), never a fake green.
#
# D27 (negative control): the mutant build arms prism::test::SetMutationVerdict
# with its complete prefix AND the runtime predicate (the C28 injection counter),
# so the verdict is printed at the FIRST failed assertion -- before any teardown
# -- and the process leaves with 0 for the PASS_REGULAR_EXPRESSION below. D26:
# no WILL_FAIL anywhere; the marker IS the conjunction. D3: if the injected
# no-sniff path does not fire, no CAUGHT is printed and the entry stays red.
#
# /WX-: the Windows SDK headers are not /W4 clean (same rationale as every other
# Windows target).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(c49_add_bench exe)
    add_executable("${exe}" win/bench_current.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
                          shlwapi.lib windowscodecs.lib psapi.lib
                          d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
  endfunction()

  c49_add_bench(c49_bench_current)
  add_test(NAME "c49.bench_current" COMMAND c49_bench_current)
  # 900 s: five fresh-process memory stages x five samples each, ten app
  # launches, a 4 GiB sparse fixture pack, five 100k-row rounds and five
  # serial+parallel 800-thumbnail rounds. The digits are report-only; this
  # timeout is a hang detector.
  set_tests_properties(c49.bench_current PROPERTIES TIMEOUT 900
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C49 REMEASURE CHECKS PASSED")

  # The negative control: the sniff is thrown away (the pre-C28 defect), so the
  # 4 GiB Open reads the whole file and the byte gate goes red. One sample only:
  # a mutant is a probe, not a measurement.
  c49_add_bench(c49_bench_current_mutant_no_sniff)
  # PRISM_C28_MUTANT_PROBE_ALL is the INJECTION (it lives in
  # PrismEngineBridge.hpp and is what C28's own control uses); PRISM_C49_MUTANT_NO_SNIFF
  # selects this card's single-sample probe path and arms the D27 verdict.
  target_compile_definitions(c49_bench_current_mutant_no_sniff
                             PRIVATE PRISM_C28_MUTANT_PROBE_ALL=1
                                     PRISM_C49_MUTANT_NO_SNIFF=1)
  add_test(NAME "mutation.c49.no_sniff"
           COMMAND c49_bench_current_mutant_no_sniff)
  set_tests_properties(mutation.c49.no_sniff PROPERTIES TIMEOUT 900
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C49 MUTATION CAUGHT: no_sniff")
endif()
