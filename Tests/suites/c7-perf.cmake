# ---------------------------------------------------------------------------
# C7 perf lane (blueprint ch. 7.2): startup / memory / scale benchmarks.
# APPENDED at the end on purpose -- no registration above was touched or
# reordered (the same convention the dnd-seek batch used).
#
# Timing policy: the digits are REPORT-ONLY. The PASS regex pins the BENCH line
# existing plus the shape checks inside each benchmark; the only duration
# assertions are hang detectors (5 s for a frame, 33 ms for a list frame). A
# shared 4-core machine cannot gate a stopwatch, and docs/PERF.md says so.
#
# Anti-fake-green: each benchmark ships a mutant that violates the property it
# claims to prove -- a mis-scaled timer (the instrument), a block-buffering
# sweep (the peak-memory invariant), an intermediate %TEMP% copy (the zero-temp
# cell). WILL_FAIL makes ctest green only when the mutant really goes red.
function(prism_add_bench exe source)
  add_executable("${exe}" "${source}")
  target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib psapi.lib
                        d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
  target_compile_options("${exe}" PRIVATE /WX-)
endfunction()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  prism_add_bench(win_bench_startup win/bench_startup.cpp)
  add_test(NAME "win.bench_startup" COMMAND win_bench_startup)
  set_tests_properties(win.bench_startup PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL STARTUP BENCH CHECKS PASSED")

  prism_add_bench(win_bench_startup_mutant_timer win/bench_startup.cpp)
  target_compile_definitions(win_bench_startup_mutant_timer
                             PRIVATE PRISM_BENCH_MUTANT_TIMER_SCALE=1)
  add_test(NAME "mutation.bench_startup.timer_scale"
           COMMAND win_bench_startup_mutant_timer)
  set_tests_properties(mutation.bench_startup.timer_scale PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

  prism_add_bench(win_bench_memory win/bench_memory.cpp)
  add_test(NAME "win.bench_memory" COMMAND win_bench_memory)
  set_tests_properties(win.bench_memory PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "ALL MEMORY BENCH CHECKS PASSED")

  prism_add_bench(win_bench_memory_mutant_buffering win/bench_memory.cpp)
  target_compile_definitions(win_bench_memory_mutant_buffering
                             PRIVATE PRISM_BENCH_MUTANT_BLOCK_BUFFER=1)
  add_test(NAME "mutation.bench_memory.block_buffer"
           COMMAND win_bench_memory_mutant_buffering)
  set_tests_properties(mutation.bench_memory.block_buffer PROPERTIES WILL_FAIL TRUE TIMEOUT 600)

  prism_add_bench(win_bench_scale win/bench_scale.cpp)
  add_test(NAME "win.bench_scale" COMMAND win_bench_scale)
  # A 10k-entry fixture pack plus a 4 GiB fixture pack/extract is minutes.
  set_tests_properties(win.bench_scale PROPERTIES TIMEOUT 1800
      PASS_REGULAR_EXPRESSION "ALL SCALE BENCH CHECKS PASSED")

  prism_add_bench(win_bench_scale_mutant_temp win/bench_scale.cpp)
  target_compile_definitions(win_bench_scale_mutant_temp PRIVATE PRISM_BENCH_MUTANT_TEMP_WRITE=1)
  add_test(NAME "mutation.bench_scale.temp_write" COMMAND win_bench_scale_mutant_temp)
  set_tests_properties(mutation.bench_scale.temp_write PROPERTIES WILL_FAIL TRUE TIMEOUT 1800)
endif()

