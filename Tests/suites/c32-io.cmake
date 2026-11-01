# c32-io: small-file write backends (blueprint ch. 7.2)
#
# One card = one file; see suites/README.md. Tests/CMakeLists.txt is untouched.
#
# What is registered here:
#   win.write_backend            real NTFS integration: long-path / reserved-name
#                                round trips, the injectable three-method driver,
#                                all three backends byte-for-byte, and the
#                                large-page probe. Marker-pinned (D1).
#   mutation.write_backend.no_predir
#   mutation.longpath.no_prefix  the two negative controls (D3). NOT WILL_FAIL:
#                                each mutant binary prints "... MUTANT CAUGHT"
#                                only when its guard went red AND the injection
#                                counter proves the mutation compiled in, so the
#                                marker itself is the conjunction (D26).
#   win.bench_write              8000 x 2 KiB, three backends, BENCH lines for
#                                the step summary. Excluded from the merge gate by
#                                the name filter (bench|fuzz) and run by the
#                                nightly report lane.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(prism_c32_io_target exe)
    add_executable("${exe}" win/test_write_backend.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib windowscodecs.lib advapi32.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
  endfunction()

  prism_c32_io_target(win_write_backend_test)
  add_test(NAME "win.write_backend" COMMAND win_write_backend_test)
  set_tests_properties(win.write_backend PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL WRITE-BACKEND CHECKS PASSED")

  # Negative control 1: directory pre-creation is compiled out.
  prism_c32_io_target(win_write_backend_mutant_no_predir)
  target_compile_definitions(win_write_backend_mutant_no_predir
                             PRIVATE PRISM_WRITE_MUTANT_NO_PREDIR=1)
  add_test(NAME "mutation.write_backend.no_predir" COMMAND win_write_backend_mutant_no_predir)
  set_tests_properties(mutation.write_backend.no_predir PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "WRITE-BACKEND MUTANT CAUGHT")

  # Negative control 2: the extended-path prefix is compiled out.
  prism_c32_io_target(win_longpath_mutant_no_prefix)
  target_compile_definitions(win_longpath_mutant_no_prefix
                             PRIVATE PRISM_LONGPATH_MUTANT_STRIP_PREFIX=1)
  add_test(NAME "mutation.longpath.no_prefix" COMMAND win_longpath_mutant_no_prefix)
  set_tests_properties(mutation.longpath.no_prefix PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "LONGPATH MUTANT CAUGHT")

  add_executable(win_bench_write win/bench_write.cpp)
  target_link_libraries(win_bench_write PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib advapi32.lib)
  target_compile_options(win_bench_write PRIVATE /WX-)
  add_test(NAME "win.bench_write" COMMAND win_bench_write)
  set_tests_properties(win.bench_write PROPERTIES TIMEOUT 1800
      PASS_REGULAR_EXPRESSION "ALL WRITE-BENCH CHECKS PASSED")
endif()
