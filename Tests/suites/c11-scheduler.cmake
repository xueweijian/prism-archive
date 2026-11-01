# c11-scheduler: extract scheduling concurrency + reader pool
# (docs/PROGRESS known debt: sweep and extract shared ONE ArchiveReader, while a
# 7z.dll handler is not thread-safe -- 7-Zip's rule is one handler per
# concurrent operation).
#
# One card = one file; see suites/README.md. Tests/CMakeLists.txt is untouched.
#
# The scheduler is pure logic, so Linux CI owns its positive run AND three
# negative controls. Each mutant is the same executable with one rule disabled;
# WILL_FAIL makes CTest green only while the mutation is caught (CONTRACTS D3).
add_executable(test_extract_scheduler test_extract_scheduler.cpp)
target_link_libraries(test_extract_scheduler PRIVATE prism_core)
add_test(NAME "core.test_extract_scheduler" COMMAND test_extract_scheduler)
set_tests_properties(core.test_extract_scheduler PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL EXTRACT-SCHEDULER CHECKS PASSED")

foreach(mutant IN ITEMS IGNORE_PRIORITY UNBOUNDED_BACKLOG SHARE_SLOT)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_extract_scheduler_mutant_${mutant_lc}" test_extract_scheduler.cpp)
  target_link_libraries("test_extract_scheduler_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_extract_scheduler_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C11_MUTANT_${mutant}=1")
  add_test(NAME "mutation.extract_scheduler.${mutant_lc}"
           COMMAND "test_extract_scheduler_mutant_${mutant_lc}")
  set_tests_properties("mutation.extract_scheduler.${mutant_lc}" PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 120)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # Real 7z.dll: sweep+extract concurrency, 4-way concurrency, scheduler-driven
  # preemption. SHA-256 is the cross-talk detector. A missing 7z.dll/7z.exe is a
  # FAIL, never a silent pass (CONTRACTS D1) -- hence the pinned stdout marker.
  add_executable(win_reader_pool_test win/test_reader_pool.cpp)
  target_link_libraries(win_reader_pool_test PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib bcrypt.lib)
  target_compile_options(win_reader_pool_test PRIVATE /WX-)
  add_test(NAME "win.reader_pool" COMMAND win_reader_pool_test)
  set_tests_properties(win.reader_pool PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL READER-POOL CHECKS PASSED")

  # Negative control for the shared-handler path: the SAME suite compiled with
  # the old "one reader for everybody" behaviour. WILL_FAIL is the verdict; the
  # binary also prints "C11 MUTANT ACTIVE", so the log proves the injection was
  # really compiled in (a mutation that stopped matching would leave the mutant
  # green and turn THIS test red).
  add_executable(win_reader_pool_mutant_shared_handler win/test_reader_pool.cpp)
  target_link_libraries(win_reader_pool_mutant_shared_handler PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib
                        windowscodecs.lib bcrypt.lib)
  target_compile_options(win_reader_pool_mutant_shared_handler PRIVATE /WX-)
  target_compile_definitions(win_reader_pool_mutant_shared_handler
                             PRIVATE PRISM_C11_MUTANT_SHARED_HANDLER=1)
  add_test(NAME "mutation.reader_pool.shared_handler"
           COMMAND win_reader_pool_mutant_shared_handler)
  set_tests_properties(mutation.reader_pool.shared_handler PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 300)
endif()
