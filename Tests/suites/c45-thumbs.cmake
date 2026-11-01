# c45-thumbs: the thumbnail DECODE pipeline (parallel decode + hit rate + budget).
#
# One card = one file (Tests/suites/README.md); Tests/CMakeLists.txt is not
# touched. All targets are Windows-only: the pipeline posts its decoded
# payloads to a real HWND (CONTRACTS B9), and the suite drives a message-only
# window rather than a mock.
#
# D1: the stdout marker is part of the verdict (ctest needs exit 0 AND the
# line), so a silent skip or a crash after printing is still red.
# D26: the mutants use a marker-as-conjunction and NO WILL_FAIL. Each mutant
# binary prints its CAUGHT line only when the injected branch was observed at
# runtime AND the assertions went red; a typo in the macro prints NOT IN EFFECT
# and fails.
# /WX-: the Windows SDK headers are not warning-clean under /W4 (same rationale
# as every other Windows target).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # ---- the pipeline: cancellation, hit rate, budget, B9 hand-off ----------
  add_executable(c45_thumb_pipeline win/test_thumb_pipeline.cpp)
  target_link_libraries(c45_thumb_pipeline PRIVATE prism_win_bridge user32.lib)
  target_compile_options(c45_thumb_pipeline PRIVATE /WX-)
  add_test(NAME "c45.thumb_pipeline" COMMAND c45_thumb_pipeline)
  set_tests_properties(c45.thumb_pipeline PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C45 THUMB-PIPELINE CHECKS PASSED")

  # Negative control 1 (D3): scroll-out no longer cancels, so queued tasks are
  # decoded after they left the viewport. The cancellation assertions must go
  # red; the CAUGHT line requires the injected branch to have been observed.
  add_executable(c45_thumb_pipeline_mutant_no_cancel win/test_thumb_pipeline.cpp)
  target_link_libraries(c45_thumb_pipeline_mutant_no_cancel PRIVATE prism_win_bridge
                        user32.lib)
  target_compile_options(c45_thumb_pipeline_mutant_no_cancel PRIVATE /WX-)
  target_compile_definitions(c45_thumb_pipeline_mutant_no_cancel
                             PRIVATE PRISM_C45_MUTANT_NO_CANCEL=1)
  add_test(NAME "mutation.c45.no_cancel"
           COMMAND c45_thumb_pipeline_mutant_no_cancel)
  set_tests_properties(mutation.c45.no_cancel PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C45 CANCEL MUTATION CAUGHT")

  # Negative control 2 (D3): the pool is forced to one worker, so the
  # parallelism assertion must go red.
  add_executable(c45_thumb_pipeline_mutant_one_worker win/test_thumb_pipeline.cpp)
  target_link_libraries(c45_thumb_pipeline_mutant_one_worker PRIVATE prism_win_bridge
                        user32.lib)
  target_compile_options(c45_thumb_pipeline_mutant_one_worker PRIVATE /WX-)
  target_compile_definitions(c45_thumb_pipeline_mutant_one_worker
                             PRIVATE PRISM_C45_MUTANT_ONE_WORKER=1)
  add_test(NAME "mutation.c45.one_worker"
           COMMAND c45_thumb_pipeline_mutant_one_worker)
  set_tests_properties(mutation.c45.one_worker PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C45 ONE-WORKER MUTATION CAUGHT")

  # ---- 800 x 128x128, serial vs parallel (durations REPORTED, not gated) ---
  # The BENCH_THUMBS line is parsed by tools/report/extract_bench.py; the test
  # name contains "bench" so a green run with no BENCH line is a report error.
  add_executable(c45_bench_thumbs win/bench_thumbs.cpp)
  target_link_libraries(c45_bench_thumbs PRIVATE prism_win_bridge user32.lib)
  target_compile_options(c45_bench_thumbs PRIVATE /WX-)
  add_test(NAME "c45.bench_thumbs" COMMAND c45_bench_thumbs)
  set_tests_properties(c45.bench_thumbs PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "ALL C45 THUMB BENCH CHECKS PASSED")
endif()
