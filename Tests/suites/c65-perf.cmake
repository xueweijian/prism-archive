# c65-perf: the POST-ASSEMBLY re-measurement (card C65) plus a fast smoke.
#
# docs/PERF.md's C49 digits were taken before the tab bar (C60), the search box
# (C62), the edit UI (C54), the diagnostics window and the vault (C56) were
# assembled into the shipped binary. Nothing since C49 re-measured what a user
# pays. This suite runs the SAME harness in C65 mode:
#
#   c65_bench_current --c65 -> BENCH_C65_STARTUP / _MEMORY / _OPEN4G / _FRAME /
#                              _FRAME_100K / _THUMBS
#   perf_smoke --smoke      -> BENCH_C65_SMOKE (3 s budget, every merge)
#
# The C65 mode differs from C49's in exactly three ways, each demanded by the
# card: (1) memory samples BOTH counters (Working Set and Private Bytes) and
# repeats a fresh process 3x per state; (2) the frame cell includes 10k rows as
# well as C49's 100k; (3) the 4 GiB open cell also records open -> first payload
# byte, because "opened" and "can serve a byte" are two different latencies.
#
# D1: the stdout marker is part of the verdict (exit 0 AND the line), so a
# silent skip or a crash after the digits is still red.
# D3 + D27: the smoke ships a mutant whose injection is OBSERVED at runtime
# before the verdict is printed, so a red from an unrelated cause cannot claim
# CAUGHT. D26: no WILL_FAIL -- the marker itself is the conjunction.
# D29: every wait inside the harness is bounded; TIMEOUT is a real budget.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(c65_add_bench exe)
    add_executable("${exe}" win/bench_current.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
                          shlwapi.lib windowscodecs.lib psapi.lib
                          d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
  endfunction()

  c65_add_bench(c65_bench_current)
  add_test(NAME "c65.bench_remeasure" COMMAND c65_bench_current --c65)
  # 900 s: 3 repeats x 5 fresh memory states, 6 shipped-app launches, two 4 GiB
  # sparse fixtures, two frame cells and five serial+parallel thumbnail rounds.
  set_tests_properties(c65.bench_remeasure PROPERTIES TIMEOUT 900
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C65 REMEASURE CHECKS PASSED")

  c65_add_bench(perf_smoke)
  add_test(NAME "perf_smoke" COMMAND perf_smoke --smoke)
  # A 3-second smoke: one assembled window to first paint + 60 frames of a
  # 10k-row list. 15 s is the hang budget (D29), not the expected runtime.
  set_tests_properties(perf_smoke PROPERTIES TIMEOUT 15
      PASS_REGULAR_EXPRESSION "ALL PERF SMOKE CHECKS PASSED")

  # ---- negative control (D3 + D26 + D27) ---------------------------------
  c65_add_bench(perf_smoke_mutant_no_first_paint)
  target_compile_definitions(perf_smoke_mutant_no_first_paint
                             PRIVATE PRISM_C65_MUTANT_NO_FIRST_PAINT=1)
  add_test(NAME "mutation.c65.no_first_paint"
           COMMAND perf_smoke_mutant_no_first_paint --smoke)
  set_tests_properties(mutation.c65.no_first_paint PROPERTIES TIMEOUT 15
      PASS_REGULAR_EXPRESSION "C65 MUTATION CAUGHT: no_first_paint")
endif()
