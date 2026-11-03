# c76-startup: the process-to-first-frame breakdown, the before/after digits, and
# the two negative controls (card C76).
#
# The harness measures the SHIPPED exe: it launches PrismArchive.exe with
# PRISM_C76_TRACE=1 and parses the C76_BREAKDOWN / C76_FIRSTFRAME lines the
# product printed, then renders the first frame offscreen and digests it. The
# 60 ms target is REPORTED (C76_BENCH.met), never asserted -- AGENTS.md R2: a
# stopwatch on a shared runner is not a merge gate. What IS gated:
#   * the first-frame pixel digest (content unchanged),
#   * zero runtime HLSL compiles (the pre-compiled bytecode really is used),
#   * zero glass presents before the first frame, at least one after the second,
#   * no pixel differing outside the glass island between frame 1 and frame 2.
#
# D26/D27: the mutants print "C76 MUTATION CAUGHT: <name>" at the first red and
# exit 0, so no WILL_FAIL is needed -- the marker IS the conjunction, and the
# injection is observed at runtime before the verdict is printed.
# D29: every wait is bounded (30 s per launch, ctest TIMEOUT 300).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(c76_add_bench exe)
    add_executable("${exe}" win/bench_c76_startup.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib windowscodecs.lib
                          d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
    if(TARGET prism_glass_shaders)
      add_dependencies("${exe}" prism_glass_shaders)
    endif()
  endfunction()

  set(C76_SHADERS "${CMAKE_BINARY_DIR}/Assets/Shaders")

  c76_add_bench(c76_bench_startup)
  add_test(NAME "c76.bench_startup" COMMAND c76_bench_startup
           --app $<TARGET_FILE:PrismArchive> --shaders "${C76_SHADERS}")
  set_tests_properties(c76.bench_startup PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C76 STARTUP CHECKS PASSED")

  # The PRE-CARD behaviour in one binary: runtime HLSL compile, glass on the
  # first frame. It prints the reference digits and the reference pixel digest
  # (docs/PERF.md) -- a baseline, not a verdict.
  c76_add_bench(c76_bench_baseline)
  target_compile_definitions(c76_bench_baseline PRIVATE PRISM_C76_BASELINE=1)
  add_test(NAME "c76.bench_baseline" COMMAND c76_bench_baseline --shaders "${C76_SHADERS}")
  set_tests_properties(c76.bench_baseline PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C76_BASELINE")

  # Negative controls: each must go RED on the predicate the optimised build
  # passes, with the injection observed at runtime.
  c76_add_bench(c76_bench_mutant_runtime_shaders)
  target_compile_definitions(c76_bench_mutant_runtime_shaders
                             PRIVATE PRISM_C76_MUTANT_RUNTIME_SHADERS=1)
  add_test(NAME "mutation.c76.runtime_shaders" COMMAND c76_bench_mutant_runtime_shaders
           --mutant runtime_shaders --shaders "${C76_SHADERS}")
  set_tests_properties(mutation.c76.runtime_shaders PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C76 MUTATION CAUGHT: runtime_shaders")

  c76_add_bench(c76_bench_mutant_glass_on_first_frame)
  target_compile_definitions(c76_bench_mutant_glass_on_first_frame
                             PRIVATE PRISM_C76_MUTANT_GLASS_ON_FIRST_FRAME=1)
  add_test(NAME "mutation.c76.glass_on_first_frame"
           COMMAND c76_bench_mutant_glass_on_first_frame
                   --mutant glass_on_first_frame --shaders "${C76_SHADERS}")
  set_tests_properties(mutation.c76.glass_on_first_frame PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C76 MUTATION CAUGHT: glass_on_first_frame")
endif()
