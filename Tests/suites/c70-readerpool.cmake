# c70-readerpool: WHOSE verdict is it? One item's 7-Zip code must never be
# charged to another item.
#
# WHY THIS FILE EXISTS
#   win.reader_pool has gone red three times with the same impossible-looking
#   line: the worker's payload is COMPLETE and its SHA-256 matches the fixture,
#   yet ok[i] is false (refused=0, hr=0x0). ok was
#   "verdict.ok && !collector->itemFailed(index)", and itemFailed() was the
#   guess: the collector charged every non-zero SetOperationResult to whatever
#   index its own cursor pointed at.
#
#   7-Zip's handler answers "which item" one call earlier than the code:
#   CFolderOutStream::OpenFile calls GetStream(index, askMode) and the matching
#   SetOperationResult is emitted for THAT item (CPP/7zip/Archive/7z/
#   Extract.cpp). A solid block therefore reports codes for the neighbours it
#   decodes through, and a verdict that charges them to the requested index
#   blames a payload that decoded perfectly.
#
#   C70 moves the attribution into the product: ArchiveReader::BatchVerdict
#   carries one ItemVerdict per SetOperationResult, attributed by the engine's
#   own GetStream pairing, and the suite asserts itemFailed(index).
#
# NEGATIVE CONTROL (D3 + D26 + D27 + D28)
#   The same source built with PRISM_C70_MUTANT_BLAME_REQUESTED, which charges
#   every code to the requested index (the pre-C70 shape). It arms the verdict
#   with the D3 predicate "the injection fired at least once", prints
#   "C70 MUTATION CAUGHT: blame_requested" at the FIRST failing assertion and
#   exits 0 -- the marker is the conjunction, so a dead injection goes red
#   WITHOUT the marker instead of faking a catch. No WILL_FAIL is stacked on a
#   PASS_REGULAR_EXPRESSION (D26).
#
# C36 owns the lane-wide rule "every suite that shells out to 7z.exe holds the
# named RESOURCE_LOCK prism-7z-cli". Both binaries here author their solid
# fixture with 7z.exe, so they take the same lock from their own suite file.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_reader_pool_mutant_blame_requested win/test_reader_pool.cpp)
  target_link_libraries(win_reader_pool_mutant_blame_requested PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib ole32.lib
                        windowscodecs.lib bcrypt.lib)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (B9).
  target_compile_options(win_reader_pool_mutant_blame_requested PRIVATE /WX-)
  target_compile_definitions(win_reader_pool_mutant_blame_requested
                             PRIVATE PRISM_C70_MUTANT_BLAME_REQUESTED=1)
  add_test(NAME "mutation.reader_pool.blame_requested"
           COMMAND win_reader_pool_mutant_blame_requested)
  set_tests_properties(mutation.reader_pool.blame_requested PROPERTIES TIMEOUT 300
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "C70 MUTATION CAUGHT: blame_requested")
endif()
