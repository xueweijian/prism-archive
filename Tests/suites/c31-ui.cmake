# c31-ui: the settings dialog + the command palette overlay (blueprint ch. 8.4
# screenshot + ch. 9.1 row 3). C14 shipped SettingsStore, C27 landed it on disk,
# C8 shipped the palette model and its geometry. This card is the screen and the
# paint instructions -- and it is the gate that an illegal value never reaches
# the disk.
#
# ONE source file, TWO pure-carrying executables and FOUR negative controls:
#   core.c31_settings_ui        DialogState + PaletteOverlay rules, EVERY lane,
#                               no window station (the C18 split: a rule that
#                               only ran on Windows would be skipped by Linux).
#   win.c31_settings_ui         the SAME rules through a REAL HWND and a real
#                               atomic save into a temp folder (Windows lane).
#
# CONTRACTS D1: the Windows test prints "ALL SETTINGS-UI CHECKS PASSED" and the
# marker is a CONJUNCTION with the exit code; a session with no window station
# returns 77 (SKIP_RETURN_CODE) after printing the reason, so a skip is visible
# and can never be mistaken for a pass.
#
# CONTRACTS D3 + D26: each mutation is the same source with ONE rule compiled
# out, and the verdict is a marker that is itself a conjunction -- the run went
# red AND DialogState::MutationFired() is non-zero. A dead injection prints no
# marker, so the mutant entry goes RED (the alarm the ledger asks for). No
# PASS_REGULAR_EXPRESSION is ever combined with WILL_FAIL on the same test: the
# Windows mutants use the marker only, the core mutants use WILL_FAIL only.
set(PRISM_C31_WIN_LIBS "")
if(WIN32)
  set(PRISM_C31_WIN_LIBS user32.lib gdi32.lib ole32.lib shell32.lib)
endif()

# The Windows block comes FIRST on purpose: tools/preflight.py's D1 gate maps a
# source file to the FIRST add_executable that mentions it and then requires
# PASS_REGULAR_EXPRESSION on that executable's test (this file can SKIP).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_settings_ui win/test_settings_ui.cpp)
  target_link_libraries(win_settings_ui PRIVATE prism_core prism_win_bridge
                        ${PRISM_C31_WIN_LIBS})
  target_compile_options(win_settings_ui PRIVATE /WX-)
  add_test(NAME "win.c31_settings_ui" COMMAND win_settings_ui)
  set_tests_properties(win.c31_settings_ui PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL SETTINGS-UI CHECKS PASSED")

  # The three deliberate defects. The dialog's Save must refuse an illegal value
  # BECAUSE the rule fired, not because a control happened to be disabled --
  # every mutant case therefore dispatches the click at the handler.
  foreach(mutant IN ITEMS ACCEPT_BAD_DIR ACCEPT_BAD_THUMBNAIL SKIP_STORE_PROBE)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_settings_ui_mutant_${mutant_lc}" win/test_settings_ui.cpp)
    target_link_libraries("win_settings_ui_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge ${PRISM_C31_WIN_LIBS})
    target_compile_options("win_settings_ui_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_settings_ui_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C31_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c31_ui.${mutant_lc}"
             COMMAND "win_settings_ui_mutant_${mutant_lc}")
    # No WILL_FAIL: the marker is the verdict (CONTRACTS D26).
    set_tests_properties("mutation.c31_ui.${mutant_lc}" PROPERTIES TIMEOUT 180
        PASS_REGULAR_EXPRESSION "SETTINGS-UI MUTANT CAUGHT")
  endforeach()
endif()

# The pure rules: every lane, no window, milliseconds. PRISM_C31_CORE_ONLY keeps
# the Windows-lane core target from opening a window, so this entry can never
# become a silent SKIP on a headless runner.
add_executable(core_test_settings_ui win/test_settings_ui.cpp)
target_link_libraries(core_test_settings_ui PRIVATE prism_core ${PRISM_C31_WIN_LIBS})
target_compile_definitions(core_test_settings_ui PRIVATE PRISM_C31_CORE_ONLY=1)
if(WIN32)
  target_compile_options(core_test_settings_ui PRIVATE /WX-)
endif()
add_test(NAME "core.c31_settings_ui" COMMAND core_test_settings_ui)
set_tests_properties(core.c31_settings_ui PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL SETTINGS-UI CHECKS PASSED")

# The same three mutations on the pure rules, registered WILL_FAIL: ctest is
# green only while the mutation is still caught. No PASS_REGULAR_EXPRESSION here
# (CONTRACTS D26), and the core binary can only return 0/1 -- never 77.
foreach(mutant IN ITEMS ACCEPT_BAD_DIR ACCEPT_BAD_THUMBNAIL SKIP_STORE_PROBE)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("core_test_settings_ui_mutant_${mutant_lc}" win/test_settings_ui.cpp)
  target_link_libraries("core_test_settings_ui_mutant_${mutant_lc}" PRIVATE prism_core
                        ${PRISM_C31_WIN_LIBS})
  target_compile_definitions("core_test_settings_ui_mutant_${mutant_lc}"
                             PRIVATE PRISM_C31_CORE_ONLY=1 "PRISM_C31_MUTANT_${mutant}=1")
  if(WIN32)
    target_compile_options("core_test_settings_ui_mutant_${mutant_lc}" PRIVATE /WX-)
  endif()
  add_test(NAME "mutation.c31_ui.core_${mutant_lc}"
           COMMAND "core_test_settings_ui_mutant_${mutant_lc}")
  set_tests_properties("mutation.c31_ui.core_${mutant_lc}" PROPERTIES WILL_FAIL TRUE
                       TIMEOUT 120)
endforeach()
