# c50-poollease: ONE LEASE == ONE HANDLER, proven from many threads at once.
#
# WHY THIS FILE EXISTS
#   win.reader_pool.four_concurrent_extractions_each_get_their_own_bytes has gone
#   red 4 times on CI across different PRs. C41 landed the deterministic
#   one-handler-one-operation guard inside ArchiveReader, so a shared handler is
#   now REFUSED and counted instead of silently returning 0 bytes or the wrong
#   payload -- which is what makes its counter a usable signal:
#
#     operationConflicts() > 0  =>  two engine operations were inside ONE handler
#     operationConflicts() == 0 =>  the lease never handed one handler to two
#                                   threads (or nobody looked)
#
#   The existing suite acquires its leases from the main thread, one after the
#   other; it can only ever catch a sequential double-lease. This suite owns the
#   concurrent half: 40 rounds x 4 threads x 4 acquire/release cycles, a
#   per-slot atomic brain flag claimed immediately after every Acquire (a taken
#   flag IS the duplicate lease), real extractions whose bytes and SHA-256 are
#   asserted (D2), and operationConflicts() == 0 as a per-round CONTRACT instead
#   of a diagnostic.
#
# NEGATIVE CONTROL (D3 + D26 + D27)
#   The same source is built with PRISM_C50_MUTANT_SHARED_LEASE, which deletes
#   the lease mutual exclusion in ReaderPool::Acquire (no slot is marked busy,
#   every caller gets slot 0). The binary arms SetMutationVerdict with the D3
#   predicate "operationConflicts() > 0", so it prints
#   "C50 MUTATION CAUGHT: shared_lease" only when the injected defect provably
#   fired; an injection that stopped matching goes red WITHOUT the marker and
#   CTest fails on the missing regex (that is the point). No WILL_FAIL anywhere
#   in this file: D26 forbids stacking it on a pass marker.
#
# C36 owns the lane-wide rule "every suite that shells out to 7z.exe holds the
# RESOURCE_LOCK prism-7z-cli". Both binaries author their solid fixture with
# 7z.exe, so they take the same lock here -- c36's explicit list does not know
# about C50's targets (one card = one file).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_pool_lease_test win/test_pool_lease.cpp)
  target_link_libraries(win_pool_lease_test PRIVATE prism_core prism_win_bridge
                        ole32.lib bcrypt.lib)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (B9).
  target_compile_options(win_pool_lease_test PRIVATE /WX-)
  add_test(NAME "win.pool_lease" COMMAND win_pool_lease_test)
  set_tests_properties(win.pool_lease PROPERTIES TIMEOUT 600
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "ALL POOL-LEASE CHECKS PASSED")

  add_executable(win_pool_lease_mutant_shared_lease win/test_pool_lease.cpp)
  target_link_libraries(win_pool_lease_mutant_shared_lease PRIVATE prism_core
                        prism_win_bridge ole32.lib bcrypt.lib)
  target_compile_options(win_pool_lease_mutant_shared_lease PRIVATE /WX-)
  target_compile_definitions(win_pool_lease_mutant_shared_lease
                             PRIVATE PRISM_C50_MUTANT_SHARED_LEASE=1)
  add_test(NAME "mutation.pool_lease.shared_lease"
           COMMAND win_pool_lease_mutant_shared_lease)
  set_tests_properties(mutation.pool_lease.shared_lease PROPERTIES TIMEOUT 600
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "C50 MUTATION CAUGHT: shared_lease")
endif()
