# c75-idle: the minimised RESIDENT set (card C75).
#
# C65 measured 71.5-71.7 MB Working Set / 45.2 MB Private after minimise against
# ADR-006's 40 MB tier. This suite measures the same GalleryWindow the product
# runs, prints the per-category breakdown before/after the minimise path, and
# asserts the releases on live objects (C33's accounting rule). Five
# minimise/restore cycles pin monotonicity, a non-blank first frame after each
# restore, and a +20 % restore budget.
#
# NEGATIVE CONTROLS (D3/D26/D27), no WILL_FAIL:
#   mutation.c75.no_trim            -- the idle path releases nothing
#   mutation.c75.no_restore_rebuild -- the Resize does not rebuild/paint
# D1: the pass marker is part of the verdict; a skip exits 77 (SKIP_RETURN_CODE)
# and can never look green. D29: the whole suite is one bounded process.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(c75_add_idle exe)
    add_executable("${exe}" win/c75_mem_idle.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
                          shlwapi.lib psapi.lib d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
  endfunction()

  c75_add_idle(c75_mem_idle)
  add_test(NAME "c75.mem_idle" COMMAND c75_mem_idle)
  set_tests_properties(c75.mem_idle PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C75 IDLE CHECKS PASSED")

  c75_add_idle(c75_mem_idle_mutant_no_trim)
  target_compile_definitions(c75_mem_idle_mutant_no_trim
                             PRIVATE PRISM_C75_MUTANT_NO_TRIM=1)
  add_test(NAME "mutation.c75.no_trim" COMMAND c75_mem_idle_mutant_no_trim)
  set_tests_properties(mutation.c75.no_trim PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C75 MUTATION CAUGHT: no_trim")

  c75_add_idle(c75_mem_idle_mutant_no_restore_rebuild)
  target_compile_definitions(c75_mem_idle_mutant_no_restore_rebuild
                             PRIVATE PRISM_C75_MUTANT_NO_RESTORE_REBUILD=1)
  add_test(NAME "mutation.c75.no_restore_rebuild" COMMAND c75_mem_idle_mutant_no_restore_rebuild)
  set_tests_properties(mutation.c75.no_restore_rebuild PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C75 MUTATION CAUGHT: no_restore_rebuild")
endif()
