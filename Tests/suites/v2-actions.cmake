# v2-actions: the action band / toolbar (DESIGN.md §2 "动作岛 56", §3 four
# states, §6 Bandizip action alignment).
#
# ONE card = ONE file (Tests/suites/README.md): Tests/CMakeLists.txt is not
# touched. The suite is split the same way the code is:
#
#   core.test_action_bar_model  the TABLE, the ENABLE RULES, the mnemonics, the
#                               keyboard state machine and the band geometry.
#                               Pure PrismCore: it runs on EVERY lane, so a rule
#                               regression is caught on Linux in milliseconds
#                               instead of costing a Windows CI round.
#   win.action_bar_paint        the same numbers on the DEVICE: the band paints
#                               inside layout.action, hover lifts a local fill
#                               (achromatic), disabled ink is 40%, pressing
#                               sinks the content 1 px, icons stay in their box,
#                               the dropdown hangs below its button -- and on a
#                               REAL HWND, WM_SYSKEYDOWN runs the action its
#                               Alt mnemonic names (counted by the app handler).
#
# CONTRACTS D1: both clean targets print an "ALL ... PASSED" marker and the
# marker is a CONJUNCTION with the exit code (a silent skip or a crash after the
# checks still fails).
#
# CONTRACTS D3 + D26: each mutant is the SAME source with exactly ONE rule
# compiled out, registered WILL_FAIL with NO PASS_REGULAR_EXPRESSION (combining
# the two inverts the verdict). Each binary prints "V2 MUTANT ACTIVE: <name>",
# so a macro that stopped matching cannot leave the mutant green.
#
# The Windows block comes FIRST: tools/preflight.py's D1 gate binds a source
# file to the FIRST add_executable that mentions it and requires the PASS regex
# on that executable's test.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_action_bar_paint win/test_action_bar_paint.cpp)
  target_link_libraries(win_action_bar_paint PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib d3d11.lib dxgi.lib dcomp.lib WindowsApp.lib)
  target_compile_options(win_action_bar_paint PRIVATE /WX-)
  add_test(NAME "win.action_bar_paint" COMMAND win_action_bar_paint)
  set_tests_properties(win.action_bar_paint PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL ACTION-BAR PAINT CHECKS PASSED")

  # The painter is switched off entirely: every "the band reached pixels"
  # assertion must go red (the counters stay 0).
  add_executable(win_action_bar_paint_mutant_no_paint win/test_action_bar_paint.cpp)
  target_link_libraries(win_action_bar_paint_mutant_no_paint PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib ole32.lib d3d11.lib dxgi.lib dcomp.lib
                        WindowsApp.lib)
  target_compile_options(win_action_bar_paint_mutant_no_paint PRIVATE /WX-)
  target_compile_definitions(win_action_bar_paint_mutant_no_paint
                             PRIVATE PRISM_V2_MUTANT_NO_ACTION_PAINT=1)
  add_test(NAME "mutation.action_bar.no_paint"
           COMMAND win_action_bar_paint_mutant_no_paint)
  set_tests_properties(mutation.action_bar.no_paint PROPERTIES WILL_FAIL TRUE TIMEOUT 300)
endif()

# The model: every lane. /WX- is the project's test convention (tests may use
# comparison idioms the product would not), MSVC only -- gcc has no such flag.
add_executable(test_action_bar_model test_action_bar_model.cpp)
target_link_libraries(test_action_bar_model PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_action_bar_model PRIVATE /WX-)
endif()
add_test(NAME "core.test_action_bar_model" COMMAND test_action_bar_model)
set_tests_properties(core.test_action_bar_model PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL ACTION-BAR MODEL CHECKS PASSED")

# Negative control 1: the enable rules are switched off, so every action is
# legal in every context. The enable matrix MUST go red.
add_executable(test_action_bar_model_mutant_all_enabled test_action_bar_model.cpp)
target_link_libraries(test_action_bar_model_mutant_all_enabled PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_action_bar_model_mutant_all_enabled PRIVATE /WX-)
endif()
target_compile_definitions(test_action_bar_model_mutant_all_enabled
                           PRIVATE PRISM_V2_MUTANT_ALL_ENABLED=1)
add_test(NAME "mutation.action_bar.model_all_enabled"
         COMMAND test_action_bar_model_mutant_all_enabled)
set_tests_properties(mutation.action_bar.model_all_enabled PROPERTIES WILL_FAIL TRUE TIMEOUT 120)

# Negative control 2: Test claims Alt+E, which ExtractTo already owns. The
# mnemonic uniqueness case MUST go red (and the mutant's keyboard case with it).
add_executable(test_action_bar_model_mutant_duplicate_mnemonic test_action_bar_model.cpp)
target_link_libraries(test_action_bar_model_mutant_duplicate_mnemonic PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_action_bar_model_mutant_duplicate_mnemonic PRIVATE /WX-)
endif()
target_compile_definitions(test_action_bar_model_mutant_duplicate_mnemonic
                           PRIVATE PRISM_V2_MUTANT_DUPLICATE_MNEMONIC=1)
add_test(NAME "mutation.action_bar.model_duplicate_mnemonic"
         COMMAND test_action_bar_model_mutant_duplicate_mnemonic)
set_tests_properties(mutation.action_bar.model_duplicate_mnemonic PROPERTIES
                     WILL_FAIL TRUE TIMEOUT 120)
