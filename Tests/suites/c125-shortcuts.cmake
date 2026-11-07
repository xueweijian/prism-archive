# c125-shortcuts: the two chords that must not act unasked, and a system theme
# that actually follows (card C125).
#
# THE DEFECTS, measured against Bandizip 6.29 and this machine:
#   1. F5 was UNBOUND here; Bandizip binds 设置(&S) to F5. The tempting fix --
#      F5 = extract -- is the accident this card prevents, so S1 asserts both
#      halves (settings, and NOT the extractor).
#   2. Ctrl+E resolved to SmartExtract, i.e. the whole archive written to disk
#      with no prompt. Bandizip opens an extract dialog on the same chord. S2
#      pins the dialog command; the silent path moved to Ctrl+Alt+E.
#   3. "system" was hard-wired to Midnight and Tests/test_chrome_theme.cpp
#      pinned that. S3 pins the mapping from the OS answer, and the Windows
#      suite reads the REAL registry DWORD out of a temporary HKCU key.
#
# TWO LAYERS, because the fix has two halves:
#   core.c125_shortcuts  the decisions (prism::keys::Resolve,
#                        prism::chrome::ShellVariantFor) -- runs on BOTH lanes.
#   win.c125_signals     a REAL window plus the REAL registry: F5 reaching the
#                        settings gate without extracting, Ctrl+E raising the
#                        dialog seam, Ctrl+Alt+E still extracting,
#                        WM_SETTINGCHANGE("ImmersiveColorSet") re-theming,
#                        AppsUseLightTheme=1/0/missing through the production
#                        reader, and the dialog's three facts + cancel.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). No WILL_FAIL anywhere: each arm
# is this SAME core source with ONE rule injected, arms mini_test's verdict on
# the injection provably firing, and its PASS_REGULAR_EXPRESSION is the verdict.
#   f5_extract               F5 -> SmartExtract          -> S1 must go red
#   ctrl_e_smart             Ctrl+E -> SmartExtract      -> S2 must go red
#   system_always_midnight   "system" ignores the OS     -> S3 must go red
add_executable(core_c125_shortcuts test_c125_shortcuts.cpp)
target_link_libraries(core_c125_shortcuts PRIVATE prism_core)
add_test(NAME "core.c125_shortcuts" COMMAND core_c125_shortcuts)
set_tests_properties(core.c125_shortcuts PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL C125 SHORTCUT CHECKS PASSED")

# Arm 1: F5 writes disk instead of opening settings. S1's OpenSettings
# assertion MUST go red.
add_executable(core_c125_shortcuts_mutant_f5_extract test_c125_shortcuts.cpp)
target_link_libraries(core_c125_shortcuts_mutant_f5_extract PRIVATE prism_core)
target_compile_definitions(core_c125_shortcuts_mutant_f5_extract
                           PRIVATE PRISM_C125_MUTANT_F5_EXTRACT=1)
add_test(NAME "c125.mutation.f5_extract"
         COMMAND core_c125_shortcuts_mutant_f5_extract)
set_tests_properties(c125.mutation.f5_extract PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C125 MUTATION CAUGHT: f5_extract")

# Arm 2: Ctrl+E is the silent extractor again (the shipped binding). S2 MUST
# go red.
add_executable(core_c125_shortcuts_mutant_ctrl_e_smart test_c125_shortcuts.cpp)
target_link_libraries(core_c125_shortcuts_mutant_ctrl_e_smart PRIVATE prism_core)
target_compile_definitions(core_c125_shortcuts_mutant_ctrl_e_smart
                           PRIVATE PRISM_C125_MUTANT_CTRL_E_SMART=1)
add_test(NAME "c125.mutation.ctrl_e_smart"
         COMMAND core_c125_shortcuts_mutant_ctrl_e_smart)
set_tests_properties(c125.mutation.ctrl_e_smart PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C125 MUTATION CAUGHT: ctrl_e_smart")

# Arm 3: "system" is Midnight whatever the desktop says (the shipped rule).
# S3's Daylight assertion MUST go red.
add_executable(core_c125_shortcuts_mutant_system_always_midnight test_c125_shortcuts.cpp)
target_link_libraries(core_c125_shortcuts_mutant_system_always_midnight PRIVATE prism_core)
target_compile_definitions(core_c125_shortcuts_mutant_system_always_midnight
                           PRIVATE PRISM_C125_MUTANT_SYSTEM_ALWAYS_MIDNIGHT=1)
add_test(NAME "c125.mutation.system_always_midnight"
         COMMAND core_c125_shortcuts_mutant_system_always_midnight)
set_tests_properties(c125.mutation.system_always_midnight PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C125 MUTATION CAUGHT: system_always_midnight")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # user32/gdi32: the window; d2d1/dwrite/windowscodecs: its renderer;
  # advapi32: the REAL registry read (the probe lives in PrismWin/SystemTheme).
  set(PRISM_C125_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib d2d1.lib dwrite.lib
      windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib dwmapi.lib advapi32.lib)

  add_executable(win_c125_signals win/test_c125_signals.cpp)
  target_link_libraries(win_c125_signals PRIVATE ${PRISM_C125_WINDOW_LIBS})
  # The SDK headers are not /W4-clean; this card's own code is.
  target_compile_options(win_c125_signals PRIVATE /WX-)
  add_test(NAME "win.c125_signals" COMMAND win_c125_signals)
  # D29: a real budget (a window, four key presses, a registry round trip, three
  # dialogs). D1: the marker is a CONJUNCTION with exit 0; no station = 77.
  set_tests_properties(win.c125_signals PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C125 SIGNAL CHECKS PASSED")
endif()
