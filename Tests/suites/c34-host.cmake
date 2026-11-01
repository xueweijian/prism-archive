# c34-host: the three modules that were "done" but unreachable, now wired into
# the running application (one card = one file; see suites/README.md).
#
#   (1) THE INLINE-PREVIEW COMPANION IS HOSTED: the exe owns the preview section
#       (so the shell's probe stops answering NoReader), the flyout stays hidden
#       until a wake, then lists the archive's first 12 entries, acks the writer
#       and shows WITHOUT taking focus.
#   (2) THE MINIMISE PATH GIVES MEMORY BACK: the window releases the glass
#       layer's transient bytes (C33's accounting: live -> 0 -> rebuilt) and
#       trims the thumbnail store to its idle target.
#   (3) A FINISHED RE-PACK BECOMES THE OPEN ARCHIVE: the old pool is closed, the
#       new file is opened and leased, and the view republishes the new entry
#       count. A switch that cannot open its target changes nothing.
#
# D1: a runner without 7z.dll/7z.exe, without a window station or without
# DirectComposition prints WHY and exits 77 (SKIP_RETURN_CODE); the stdout marker
# is required all the same, so a silent skip can never count as a pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_host_wiring win/test_host_wiring.cpp)
  target_link_libraries(win_host_wiring PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib windowscodecs.lib
                        d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
  target_compile_options(win_host_wiring PRIVATE /WX-)
  add_test(NAME "win.host_wiring" COMMAND win_host_wiring)
  set_tests_properties(win.host_wiring PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL HOST-WIRING CHECKS PASSED")

  # NEGATIVE CONTROLS (CONTRACTS D3 + D26): one regression per build, NO WILL_FAIL
  # anywhere. The mutant prints a banner (proof the injection compiled in) and
  # only when the suite went RED prints "C34 MUTATION CAUGHT: <id>" and exits 0 --
  # that marker is this target's PASS_REGULAR_EXPRESSION. If the injection ever
  # stops applying, the suite stays green, the marker is absent and the entry
  # goes red.
  #   * NO_TRIM            -- the minimise path keeps the glass layer: (2) red.
  #   * NO_ARCHIVE_SWITCH  -- the refresh keeps the old archive: (3) red.
  foreach(mutant IN ITEMS NO_TRIM NO_ARCHIVE_SWITCH)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    add_executable("win_host_wiring_mutant_${mutant_lc}" win/test_host_wiring.cpp)
    target_link_libraries("win_host_wiring_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib ole32.lib
                          windowscodecs.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib
                          dcomp.lib)
    target_compile_options("win_host_wiring_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_host_wiring_mutant_${mutant_lc}"
                               PRIVATE PRISM_C34_MUTANT_${mutant}=1)
    add_test(NAME "mutation.host_wiring.${mutant_id}"
             COMMAND "win_host_wiring_mutant_${mutant_lc}")
    set_tests_properties("mutation.host_wiring.${mutant_id}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        # The binary prints the lowercase spelling; the CTest entry name uses
        # dashes. Pinning the wrong one makes a CAUGHT mutation look stale.
        PASS_REGULAR_EXPRESSION "C34 MUTATION CAUGHT: ${mutant_lc}")
  endforeach()
endif()
