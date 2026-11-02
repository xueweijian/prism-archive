# c56-vaultui: the password-vault manager screen (card C56).
#
# C5 shipped the DPAPI store and C35 shipped a dialog that can PICK a stored
# secret; neither let a user see what is stored, delete one entry, rename it, or
# wipe the vault. This card adds the list screen, and this file is its whole
# registration (one card = one file, see suites/README.md).
#
# ONE source file for the pure rules (Tests/test_vault_admin.cpp) and ONE for the
# HWNDs (Tests/win/test_vault_dialog.cpp):
#   core.c56_vault_admin    the mask, the row, delete/rename/clear, corruption
#                           accounting -- EVERY lane, no window station (a rule
#                           that only ran on Windows would be skipped by Linux).
#   win.c56_vault_dialog    the SAME rules through a REAL HWND and a REAL DPAPI
#                           file: the delete must change the file BYTES and C5's
#                           reader must see one entry fewer.
#
# CONTRACTS:
#   D1   the Windows test prints "ALL VAULT-DIALOG CHECKS PASSED" and the marker
#        is a CONJUNCTION with the exit code; a session with no window station
#        returns 77 (SKIP_RETURN_CODE) after printing the reason.
#   D3   each mutation is the SAME source with ONE rule compiled out.
#   D26  the Windows mutants decide with the marker only (no WILL_FAIL), the core
#        mutants with WILL_FAIL only -- never both on one test.
#   D27  the mutants arm their verdict before the first assertion.
#   D28  no arm name carries clean/unarmed/control.
#   D29  every entry has a real TIMEOUT budget (no default 1500s hang).
#
# The Windows block comes FIRST on purpose: tools/preflight.py's D1 gate maps a
# source file to the FIRST add_executable that mentions it and then requires
# PASS_REGULAR_EXPRESSION on that executable's test.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_vault_dialog win/test_vault_dialog.cpp)
  target_link_libraries(win_vault_dialog PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib comctl32.lib ole32.lib crypt32.lib d2d1.lib
                        dwrite.lib)
  target_compile_options(win_vault_dialog PRIVATE /WX-)
  add_test(NAME "win.c56_vault_dialog" COMMAND win_vault_dialog)
  set_tests_properties(win.c56_vault_dialog PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL VAULT-DIALOG CHECKS PASSED")

  # Two deliberate defects on the window lane. A dead injection prints no marker,
  # so the mutant entry goes RED -- that is the "the injection stopped working"
  # alarm (CONTRACTS D3). No WILL_FAIL is ever stacked on the marker (D26).
  foreach(mutant IN ITEMS SHOW_PLAINTEXT NO_CLEAR_CONFIRM)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_vault_dialog_mutant_${mutant}" win/test_vault_dialog.cpp)
    target_link_libraries("win_vault_dialog_mutant_${mutant}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib comctl32.lib ole32.lib
                          crypt32.lib d2d1.lib dwrite.lib)
    target_compile_options("win_vault_dialog_mutant_${mutant}" PRIVATE /WX-)
    target_compile_definitions("win_vault_dialog_mutant_${mutant}"
                               PRIVATE "PRISM_VAULT_UI_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c56_vaultui.${mutant_lc}"
             COMMAND "win_vault_dialog_mutant_${mutant}")
    set_tests_properties("mutation.c56_vaultui.${mutant_lc}" PROPERTIES TIMEOUT 180
        PASS_REGULAR_EXPRESSION "C56 VAULT-UI MUTANT CAUGHT")
  endforeach()
endif()

# The pure rules: every lane, no window, milliseconds.
add_executable(core_test_vault_admin test_vault_admin.cpp)
target_link_libraries(core_test_vault_admin PRIVATE prism_core)
if(WIN32)
  target_compile_options(core_test_vault_admin PRIVATE /WX-)
endif()
add_test(NAME "core.c56_vault_admin" COMMAND core_test_vault_admin)
set_tests_properties(core.c56_vault_admin PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL VAULT-ADMIN CHECKS PASSED")

# The same two defects on the pure rules, registered WILL_FAIL: ctest is green
# only while the mutation is still caught. No PASS_REGULAR_EXPRESSION here
# (CONTRACTS D26); the core binary can only return 0/1, never 77.
foreach(mutant IN ITEMS SHOW_PLAINTEXT NO_CLEAR_CONFIRM)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("core_test_vault_admin_mutant_${mutant}" test_vault_admin.cpp)
  target_link_libraries("core_test_vault_admin_mutant_${mutant}" PRIVATE prism_core)
  target_compile_definitions("core_test_vault_admin_mutant_${mutant}"
                             PRIVATE "PRISM_VAULT_UI_MUTANT_${mutant}=1")
  if(WIN32)
    target_compile_options("core_test_vault_admin_mutant_${mutant}" PRIVATE /WX-)
  endif()
  add_test(NAME "mutation.c56_vaultadmin.${mutant_lc}"
           COMMAND "core_test_vault_admin_mutant_${mutant}")
  set_tests_properties("mutation.c56_vaultadmin.${mutant_lc}" PROPERTIES WILL_FAIL TRUE
                       TIMEOUT 120)
endforeach()
