# c16-logging: graded structured logging (redaction / throttling / ring) and
# the crash-dump handler.
#
# One card = one file; see suites/README.md. Tests/CMakeLists.txt is untouched.
#
# The logging core is pure C++20, so the Linux lane owns its positive run AND the
# negative control. The crash-dump half needs SetUnhandledExceptionFilter and
# MiniDumpWriteDump, so it lives in the Windows lane -- and it too has a negative
# control, because "credentials never reach a crash dump" is only a claim until a
# mutation makes it red.

add_executable(test_log test_log.cpp)
target_link_libraries(test_log PRIVATE prism_core)
add_test(NAME "core.test_log" COMMAND test_log)
# The stdout marker is part of the verdict (CONTRACTS D1): a silent early return
# can never be mistaken for a pass.
set_tests_properties(core.test_log PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL LOG CHECKS PASSED")

# CONTRACTS D3 negative control: the SAME suite with redaction compiled out. The
# redaction cases must fail; if the injection macro ever stops matching they pass
# and this registration goes red, so the mutation is verified rather than assumed.
add_executable(test_log_mutant_no_redaction test_log.cpp)
target_link_libraries(test_log_mutant_no_redaction PRIVATE prism_core)
target_compile_definitions(test_log_mutant_no_redaction
                           PRIVATE PRISM_C16_MUTANT_NO_REDACTION=1)
add_test(NAME "mutation.log.no_redaction" COMMAND test_log_mutant_no_redaction)
set_tests_properties(mutation.log.no_redaction PROPERTIES WILL_FAIL TRUE TIMEOUT 120)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The crash handler needs SetUnhandledExceptionFilter + MiniDumpWriteDump, so
  # only the Windows lane can run it. The suite re-launches ITSELF: the crashing
  # exception happens in a child process, never in the test process (a crashed
  # ctest entry is a crash, not a verdict).
  #
  # dbghelp.lib is deliberately NOT linked: the writer resolves
  # MiniDumpWriteDump at crash time, and a missing dbghelp must cost the dump,
  # not the application's startup. ole32 supplies nothing here but keeps
  # CoInitialize-free paths honest if the header grows a Shell call.
  add_executable(win_crash_dump_test win/test_crash_dump.cpp)
  target_link_libraries(win_crash_dump_test PRIVATE prism_core prism_win_bridge
                        user32.lib ole32.lib)
  target_compile_options(win_crash_dump_test PRIVATE /WX-)
  add_test(NAME "win.crash_dump" COMMAND win_crash_dump_test)
  set_tests_properties(win.crash_dump PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL CRASH-DUMP CHECKS PASSED")

  # CONTRACTS D3 negative control for the "credentials never reach a crash dump"
  # leg of B7: the SAME suite with redaction compiled out. The credential then
  # shows up in the child's report, the content assertions go red, and WILL_FAIL
  # is what makes that a green registration.
  add_executable(win_crash_dump_mutant_no_redaction win/test_crash_dump.cpp)
  target_link_libraries(win_crash_dump_mutant_no_redaction PRIVATE prism_core
                        prism_win_bridge user32.lib ole32.lib)
  target_compile_options(win_crash_dump_mutant_no_redaction PRIVATE /WX-)
  target_compile_definitions(win_crash_dump_mutant_no_redaction
                             PRIVATE PRISM_C16_MUTANT_NO_REDACTION=1)
  add_test(NAME "mutation.crash_dump.no_redaction"
           COMMAND win_crash_dump_mutant_no_redaction)
  set_tests_properties(mutation.crash_dump.no_redaction PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 300)
endif()
