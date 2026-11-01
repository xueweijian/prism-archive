# c41-poolrace: CONCURRENT EXTRACTION MUST NOT RETURN A "SUCCESSFUL" EMPTY ITEM.
#
# CI kept seeing win.reader_pool fail in three different shapes while four
# extractions overlapped: ok[i]=false on one worker, or a worker reporting
# success whose payload hashed to the SHA-256 of the EMPTY STRING
# (e3b0c442...) -- a silent 0-byte file, or a correct-looking file with the
# WRONG bytes. Reproduced on 2026-11-02: that is the signature of TWO engine
# operations sharing ONE 7z handler (185/200 ok=false, 44/200 wrong payload).
# The pool already promises one handler per operation; C41 makes the promise
# enforced inside ArchiveReader, so a violation is refused instead of losing
# data, and ExtractWithPlan can no longer call "wrote nothing" a success.
#
# This suite drives the real product path (ReaderPool lease -> ExtractWithPlan
# -> SmartExtractCallback) with four handlers on one solid archive for 50
# rounds and asserts BYTES + SHA-256 per worker (D2), never existence.
#
# Negative control (D3/D26): the same source built with
# PRISM_C41_MUTANT_NO_OPERATION_GUARD, which compiles the guard out. The
# "second operation is refused" case MUST go red, and the binary prints
# "C41 MUTANT CAUGHT" only when the injection provably fired -- the marker is
# the conjunction, so a dead injection turns this test red instead of green.
# No WILL_FAIL is stacked on the marker (D26).
#
# C36 owns the lane-wide rule "every suite that shells out to 7z.exe holds the
# named RESOURCE_LOCK prism-7z-cli". Both binaries here author their solid
# fixture with 7z.exe, so they take the same lock from their own suite file --
# C36's list is explicit and does not know about C41's targets.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_pool_race_test win/test_pool_race.cpp)
  target_link_libraries(win_pool_race_test PRIVATE prism_core prism_win_bridge
                        ole32.lib bcrypt.lib)
  target_compile_options(win_pool_race_test PRIVATE /WX-)
  add_test(NAME "win.pool_race" COMMAND win_pool_race_test)
  set_tests_properties(win.pool_race PROPERTIES TIMEOUT 600
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "ALL POOL-RACE CHECKS PASSED")

  add_executable(win_pool_race_mutant_no_guard win/test_pool_race.cpp)
  target_link_libraries(win_pool_race_mutant_no_guard PRIVATE prism_core prism_win_bridge
                        ole32.lib bcrypt.lib)
  target_compile_options(win_pool_race_mutant_no_guard PRIVATE /WX-)
  target_compile_definitions(win_pool_race_mutant_no_guard
                             PRIVATE PRISM_C41_MUTANT_NO_OPERATION_GUARD=1)
  add_test(NAME "mutation.pool_race.no_operation_guard"
           COMMAND win_pool_race_mutant_no_guard)
  set_tests_properties(mutation.pool_race.no_operation_guard PROPERTIES TIMEOUT 600
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "C41 MUTANT CAUGHT")
endif()
