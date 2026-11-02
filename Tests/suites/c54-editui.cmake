# c54-editui: EDITING AN EXISTING ARCHIVE FROM THE UI (delete / rename / new folder)
# on a real HWND, through C51's plan + copy-on-write executor. One card = one file.
#
# The suite proves the UI wiring, not the engine rules (C51 owns those):
#   * Delete / F2 / Ctrl+Shift+N produce intents from the real selection;
#   * the confirmation SAYS "copy-on-write rewrites the whole archive" and quotes
#     C51's affected-entry count and estimated write bytes;
#   * a delete and a rename reach the archive (count -1 / name changed) while every
#     kept entry's payload bytes and stored CRC are unchanged;
#   * CANCEL leaves the archive file byte-for-byte identical;
#   * read-only / multi-volume / exclusively locked archives disable the entry
#     with a readable reason;
#   * while an edit is in flight a second extract/preview is REFUSED, not run in
#     parallel.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_edit_ui_test win/test_edit_ui.cpp)
  target_link_libraries(win_edit_ui_test PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  # SDK headers are not /W4-clean; the card's own code is.
  target_compile_options(win_edit_ui_test PRIVATE /WX-)
  add_test(NAME "c54.edit_ui" COMMAND win_edit_ui_test)
  # D1: the marker is a CONJUNCTION with the exit code, and a missing 7z.dll /
  # 7z.exe returns 77 (a visible Skip, never a fake green). D29: a real budget.
  set_tests_properties(c54.edit_ui PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL EDIT-UI CHECKS PASSED")

  # NEGATIVE CONTROLS (D3 + D26 + D27). Same source, ONE rule deleted per build;
  # no WILL_FAIL anywhere. The binary arms the verdict BEFORE the first case and
  # prints the marker at the first failure -- only while the predicate proves the
  # injected defect fired. A stale injection stays RED on the missing marker.
  #   cancel_writes    -> a cancelled dialog is treated as accepted;
  #   allow_concurrent -> the single-operation gate no longer refuses.
  foreach(mutant IN ITEMS CANCEL_STILL_WRITES ALLOW_CONCURRENT)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    if(mutant STREQUAL "CANCEL_STILL_WRITES")
      set(mutant_marker "cancel_writes")
    else()
      set(mutant_marker "allow_concurrent")
    endif()
    add_executable("win_edit_ui_mutant_${mutant_lc}" win/test_edit_ui.cpp)
    target_link_libraries("win_edit_ui_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib ole32.lib d2d1.lib
                          dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
                          shell32.lib)
    target_compile_options("win_edit_ui_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_edit_ui_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C54_MUTANT_${mutant}=1")
    add_test(NAME "c54.mutation.${mutant_id}"
             COMMAND "win_edit_ui_mutant_${mutant_lc}")
    set_tests_properties("c54.mutation.${mutant_id}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C54 MUTATION CAUGHT: ${mutant_marker}")
  endforeach()
endif()
