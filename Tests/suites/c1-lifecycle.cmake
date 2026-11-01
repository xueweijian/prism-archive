# c1-lifecycle: application lifecycle (card C1)
# A REAL window on a REAL message loop closed with a REAL WM_CLOSE, the
# 7z.dll discovery order, and the cancellable whole-archive scan on the real
# engine. Registered from here (one card = one file) instead of appending to
# Tests/CMakeLists.txt.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_app_lifecycle win/test_app_lifecycle.cpp)
  target_link_libraries(win_app_lifecycle PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib ole32.lib
                                                 windowscodecs.lib d2d1.lib dwrite.lib
                                                 advapi32.lib)
  target_compile_options(win_app_lifecycle PRIVATE /WX-)
  add_test(NAME "win.app_lifecycle" COMMAND win_app_lifecycle)
  # 180 s: the suite packs its own solid fixture; a wedged message loop must not
  # block the lane forever. The marker is a CONJUNCTION with the exit code (D1),
  # so a silent skip (no 7z.dll), an early return or a crash after printing is
  # still red.
  set_tests_properties(win.app_lifecycle PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL APP-LIFECYCLE CHECKS PASSED")

  # Negative control (D3): the SAME suite with the abort check compiled out of
  # BatchAbortProbe (PrismEngineBridge.hpp). The scan then runs to completion,
  # so "it stopped early" fails and WILL_FAIL makes ctest green only when it
  # does. If this target ever passes, the cancellation guard is a no-op.
  add_executable(win_app_lifecycle_mutant_no_scan_abort win/test_app_lifecycle.cpp)
  target_link_libraries(win_app_lifecycle_mutant_no_scan_abort
                        PRIVATE prism_core prism_win_bridge
                                user32.lib gdi32.lib ole32.lib
                                windowscodecs.lib d2d1.lib dwrite.lib
                                advapi32.lib)
  target_compile_options(win_app_lifecycle_mutant_no_scan_abort PRIVATE /WX-)
  target_compile_definitions(win_app_lifecycle_mutant_no_scan_abort
                             PRIVATE PRISM_MUTANT_SCAN_ABORT_IGNORED=1)
  add_test(NAME "mutation.app_lifecycle.no_scan_abort"
           COMMAND win_app_lifecycle_mutant_no_scan_abort)
  set_tests_properties(mutation.app_lifecycle.no_scan_abort PROPERTIES
      TIMEOUT 180 WILL_FAIL TRUE)
endif()
