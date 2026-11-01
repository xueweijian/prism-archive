# c36-parallel: make the Windows lane genuinely parallel-safe, so ctest -j is
# allowed to come back.
#
# WHY THIS FILE EXISTS
# --------------------
# The first "ctest -j 8" run on the Windows lane failed win.reader_pool with
# "7z.exe could not author the solid fixture archive". Eight suites were
# authoring fixtures at the same instant, and every one of them spelled its
# scratch directory as a hard-coded constant under %TEMP% -- two processes then
# wiped and packed the same directory. The first half of the fix lives in the
# tests (unique RAII scratch trees via Tests/win/UniqueTempDir.hpp); this file
# owns the half that cannot live in a test, because it is a CTest property:
#
#   every test that shells out to 7z.exe holds the named RESOURCE_LOCK
#   "prism-7z-cli", so CTest serialises THAT GROUP only and leaves every other
#   suite running in parallel.
#
# 7z.exe is a separate process and shares no memory with us, but it writes its
# own temporaries next to the fixture and it inherits our TMP/TEMP, and one lock
# is much cheaper than chasing a 1-in-50 fixture failure on a 2-core runner.
#
# The list is deliberately explicit instead of derived from a name pattern:
# set_tests_properties on a missing test is a hard configure error, so a rename
# must not be able to quietly drop a lock. Missing names are collected and
# reported instead (CONTRACTS D1: a no-op has to look like a no-op).
#
# One card = one file; Tests/CMakeLists.txt is untouched (suites/README.md).
#
# Self-check registered here as well: the naming rule the rest of the lane now
# depends on is asserted, not assumed, plus its D3 negative control.

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The scratch-directory policy itself: card + pid + counter, RAII cleanup on
  # the error path, and stale sweeping that never touches a live neighbour.
  add_executable(c36_unique_tempdir win/test_unique_tempdir.cpp)
  target_compile_options(c36_unique_tempdir PRIVATE /WX-)
  add_test(NAME "c36.unique_tempdir" COMMAND c36_unique_tempdir)
  set_tests_properties(c36.unique_tempdir PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C36 TEMP-DIR CHECKS PASSED")

  # D3 negative control: the same source with the naming rule broken (pid and
  # counter dropped). It must go RED, and it prints "C36 MUTANT ACTIVE" so the
  # log proves the injection was compiled in rather than merely declared.
  # WILL_FAIL alone -- never WILL_FAIL + PASS_REGULAR_EXPRESSION (CONTRACTS D26).
  add_executable(c36_unique_tempdir_mutant_shared_dir win/test_unique_tempdir.cpp)
  target_compile_options(c36_unique_tempdir_mutant_shared_dir PRIVATE /WX-)
  target_compile_definitions(c36_unique_tempdir_mutant_shared_dir
                             PRIVATE PRISM_C36_MUTANT_SHARED_DIR=1)
  add_test(NAME "mutation.c36.shared_dir"
           COMMAND c36_unique_tempdir_mutant_shared_dir)
  set_tests_properties(mutation.c36.shared_dir PROPERTIES WILL_FAIL TRUE TIMEOUT 120)
endif()

# ---------------------------------------------------------------------------
# The 7z.exe group. Deferred to the end of this directory: the tests are
# registered across Tests/CMakeLists.txt and a dozen other suite files, and
# "if(TEST <name>)" can only succeed once they all exist.
# ---------------------------------------------------------------------------
function(prism_c36_lock_external_cli)
  set(_cli_tests
      # Tests/CMakeLists.txt
      win.solid_sweep
      win.bench_solid
      win.gallery_snapshot
      win.vtree_exec
      win.mkdir_matrix
      win.dnd_seek
      win.pwd_probe
      # suites/c1, c7, c9, c10, c11, c20, c21, c25
      win.app_lifecycle
      mutation.app_lifecycle.no_scan_abort
      win.bench_startup
      mutation.bench_startup.timer_scale
      win.bench_memory
      mutation.bench_memory.block_buffer
      win.bench_scale
      mutation.bench_scale.temp_write
      win.write_archive
      mutation.write_archive.dir_as_file
      mutation.write_archive.no_size
      win.volumes
      win.reader_pool
      mutation.reader_pool.shared_handler
      win.dragout
      mutation.dragout.getdata_notimpl
      mutation.dragout.size_lie
      win.preview_providers
      win.app_wiring
      mutation.app_wiring.shared_reader
      mutation.app_wiring.no_motw)

  # The installer round-trip suite (c3-installer) writes ONE per-user scratch
  # registry key (HKCU:\Software\PrismArchiveSelfTest) and resets it between
  # cases. Two of those processes at once undo each other's staging, and the
  # mutation controls then report "not caught" for a reason that has nothing to
  # do with the guard under test -- measured here on the first -j 8 run
  # (mutation.installer.rebackup / envundersoftware went red under -j 8 and pass
  # serially). One lock for the group, same mechanism as the 7z.exe group.
  list(APPEND _cli_tests
       win.installer_selftest
       mutation.installer.skipdefaulticon
       mutation.installer.nobackup
       mutation.installer.norestore
       mutation.installer.unquotedverb
       mutation.installer.rebackup
       mutation.installer.envundersoftware)

  set(_locked 0)
  set(_missing "")
  foreach(_test IN LISTS _cli_tests)
    if(TEST ${_test})
      set_tests_properties("${_test}" PROPERTIES RESOURCE_LOCK "prism-7z-cli")
      math(EXPR _locked "${_locked} + 1")
    else()
      list(APPEND _missing "${_test}")
    endif()
  endforeach()

  if(_missing)
    message(WARNING
        "c36: RESOURCE_LOCK 'prism-7z-cli' was NOT applied to unregistered test(s): "
        "${_missing}. Rename or re-add them here -- an unlocked 7z.exe suite is "
        "the failure mode this card exists to remove.")
  endif()
  message(STATUS "c36: RESOURCE_LOCK 'prism-7z-cli' on ${_locked} 7z.exe suite(s)")
endfunction()

cmake_language(DEFER CALL prism_c36_lock_external_cli)
