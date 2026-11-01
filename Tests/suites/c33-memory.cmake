# ---------------------------------------------------------------------------
# C33 -- resident memory and the frame budget ("where do 58.4 MB come from")
#
# One card = one file (suites/README.md); nothing is appended to
# Tests/CMakeLists.txt.
#
# Two lanes:
#   * core.test_thumb_budget -- always-on arithmetic: the 64 MiB ThumbStore
#     budget is a CAP that saturates exactly (D17) and an idle Trim really
#     returns the pixels;
#   * win.c33.glass_memory   -- the Windows benchmark: per-stage working-set
#     breakdown, glass laziness with pixel identity, thumbnail idle return, and
#     the draw-vs-flush split of the 100k-row frame.
#
# Negative controls (CONTRACTS D3/D26): each mutant is the SAME source compiled
# with one injection, registered with a PASS regex and NO WILL_FAIL -- the
# marker itself is the conjunction (the assertions must go red AND the injected
# branch must be observed at runtime). On this box a regex + WILL_FAIL pair is
# permanently red, and WILL_FAIL alone goes green on a silent skip.
# ---------------------------------------------------------------------------
add_executable(c33_thumb_budget c33_thumb_budget.cpp)
target_link_libraries(c33_thumb_budget PRIVATE prism_core)
add_test(NAME "core.test_thumb_budget" COMMAND c33_thumb_budget)
set_tests_properties(core.test_thumb_budget PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "checks, 0 failures")

function(c33_add_glass_mem exe source)
  add_executable("${exe}" "${source}")
  target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib psapi.lib
                        d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib)
  target_compile_options("${exe}" PRIVATE /WX-)
endfunction()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  c33_add_glass_mem(c33_bench_glass_mem win/bench_glass_mem.cpp)
  add_test(NAME "win.c33.glass_memory" COMMAND c33_bench_glass_mem)
  set_tests_properties(win.c33.glass_memory PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "ALL GLASS-MEMORY CHECKS PASSED")

  c33_add_glass_mem(c33_bench_glass_mem_mutant_no_release win/bench_glass_mem.cpp)
  target_compile_definitions(c33_bench_glass_mem_mutant_no_release
                             PRIVATE PRISM_GLASS_MUTANT_NO_RELEASE=1)
  add_test(NAME "mutation.c33.lazy_release" COMMAND c33_bench_glass_mem_mutant_no_release)
  set_tests_properties(mutation.c33.lazy_release PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "LAZY-RELEASE MUTATION CAUGHT")

  c33_add_glass_mem(c33_bench_glass_mem_mutant_no_trim win/bench_glass_mem.cpp)
  target_compile_definitions(c33_bench_glass_mem_mutant_no_trim
                             PRIVATE PRISM_THUMB_MUTANT_NO_TRIM=1)
  add_test(NAME "mutation.c33.thumb_trim" COMMAND c33_bench_glass_mem_mutant_no_trim)
  set_tests_properties(mutation.c33.thumb_trim PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "THUMBSTORE-TRIM MUTATION CAUGHT")
endif()
