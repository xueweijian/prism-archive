# c114-poollease: the negative control for C114's deterministic capacity contract.
#
# WHY THIS FILE HAS ONLY A MUTANT
#   C114 rewrote assertions in Tests/win/test_pool_lease.cpp -- the binary itself
#   is still registered by c50-poollease.cmake (one card = one registration of a
#   target, and re-registering the same test name would be a CTest error). The
#   new deterministic cases run inside that already-registered binary; what this
#   file adds is the card's OWN negative control.
#
# NEGATIVE CONTROL (CONTRACTS D3 + D26 + D27)
#   PRISM_C114_MUTANT_OVERSELL deletes the ceiling in ReaderPool::Acquire: a
#   request that finds every slot busy is handed slot 0 instead of blocking, so
#   inUse climbs past capacity. The binary arms SetMutationVerdict with the D3
#   predicate "a lease was observed that took inUse past capacity", so it prints
#   "C114 MUTATION CAUGHT: oversell" only when the injected defect provably
#   fired; an injection that stops matching goes red WITHOUT the marker and
#   CTest fails on the missing regex (that is the point). No WILL_FAIL: D26
#   forbids stacking it on a pass marker.
#
# C36 owns the lane-wide rule "every suite that shells out to 7z.exe holds the
# RESOURCE_LOCK prism-7z-cli"; this binary authors its solid fixture with 7z.exe,
# so it takes the same lock.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_pool_lease_mutant_oversell win/test_pool_lease.cpp)
  target_link_libraries(win_pool_lease_mutant_oversell PRIVATE prism_core prism_win_bridge
                        ole32.lib bcrypt.lib)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (B9).
  target_compile_options(win_pool_lease_mutant_oversell PRIVATE /WX-)
  target_compile_definitions(win_pool_lease_mutant_oversell
                             PRIVATE PRISM_C114_MUTANT_OVERSELL=1)
  add_test(NAME "mutation.pool_lease.oversell"
           COMMAND win_pool_lease_mutant_oversell)
  set_tests_properties(mutation.pool_lease.oversell PROPERTIES TIMEOUT 600
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "C114 MUTATION CAUGHT: oversell")
endif()
