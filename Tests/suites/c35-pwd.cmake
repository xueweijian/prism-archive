# c35-pwd: the password ring UI (card C35) -- input, retry budget, probe
# progress and the vault picker. One card = one file; see suites/README.md.
#
# The model is proved headless on every lane (core.test_password_ui_model); the
# same model is then driven through real HWNDs (win.password_dialog), because a
# rule that only ran on Windows would be skipped by the Linux lane and a rule
# that only ran headless would never prove the dialog exists.
#
# D1: the Windows block is FIRST on purpose. tools/preflight.py maps a source
# file to the FIRST add_executable that mentions it and then requires
# PASS_REGULAR_EXPRESSION on that executable's test. Registering a mutant first
# would make the gate report a false D1 red for the real suite.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_password_dialog win/test_password_ui.cpp)
  target_link_libraries(win_password_dialog PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib comctl32.lib ole32.lib Threads::Threads)
  # /WX- : the Windows SDK headers are not warning-clean under /W4 (contract B9).
  target_compile_options(win_password_dialog PRIVATE /WX-)
  add_test(NAME "win.password_dialog" COMMAND win_password_dialog)
  # The stdout marker is a CONJUNCTION with exit 0, and the only honoured skip is
  # exit 77 (a window station that refuses a window), which prints its reason --
  # a silent skip can never look green (CONTRACTS D1).
  set_tests_properties(win.password_dialog PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL PASSWORD-UI CHECKS PASSED"
      SKIP_RETURN_CODE 77)

  # -------------------------------------------------------------------------
  # D3 negative controls. Each compiles the SAME source with exactly one rule
  # broken, and each is registered with PASS_REGULAR_EXPRESSION and NO
  # WILL_FAIL (CONTRACTS D26: stacking the two turns the expected failure into
  # an always-red test). The binary prints PASSWORD-UI MUTANT CAUGHT only when
  # the run went red AND UiMutationFired() > 0, so a dead mutation prints
  # nothing and this test is RED -- the "the injection stopped working" alarm.
  # -------------------------------------------------------------------------
  add_executable(win_password_dialog_mutant_echo win/test_password_ui.cpp)
  target_link_libraries(win_password_dialog_mutant_echo PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib comctl32.lib ole32.lib
                        Threads::Threads)
  target_compile_options(win_password_dialog_mutant_echo PRIVATE /WX-)
  target_compile_definitions(win_password_dialog_mutant_echo
                             PRIVATE PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT=1)
  add_test(NAME "mutation.password_ui.echo_plaintext"
           COMMAND win_password_dialog_mutant_echo)
  set_tests_properties(mutation.password_ui.echo_plaintext PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "PASSWORD-UI MUTANT CAUGHT")

  add_executable(win_password_dialog_mutant_retries win/test_password_ui.cpp)
  target_link_libraries(win_password_dialog_mutant_retries PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib comctl32.lib ole32.lib
                        Threads::Threads)
  target_compile_options(win_password_dialog_mutant_retries PRIVATE /WX-)
  target_compile_definitions(win_password_dialog_mutant_retries
                             PRIVATE PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES=1)
  add_test(NAME "mutation.password_ui.unlimited_retries"
           COMMAND win_password_dialog_mutant_retries)
  set_tests_properties(mutation.password_ui.unlimited_retries PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "PASSWORD-UI MUTANT CAUGHT")
endif()

# The pure rules: every lane, no window, milliseconds. This is where the state
# machine, masking, budget, remember and vault-label assertions live.
add_executable(core_test_password_ui_model test_password_ui_model.cpp)
target_link_libraries(core_test_password_ui_model PRIVATE prism_core)
add_test(NAME "core.test_password_ui_model" COMMAND core_test_password_ui_model)

# Two rule mutations, compiled from the SAME source. The marker is the
# conjunction (red AND the injection fired) rather than WILL_FAIL, per D3/D26.
add_executable(core_test_password_ui_model_mutant_echo test_password_ui_model.cpp)
target_link_libraries(core_test_password_ui_model_mutant_echo PRIVATE prism_core)
target_compile_definitions(core_test_password_ui_model_mutant_echo
                           PRIVATE PRISM_PWD_UI_MUTANT_ECHO_PLAINTEXT=1)
add_test(NAME "mutation.password_ui_model.echo_plaintext"
         COMMAND core_test_password_ui_model_mutant_echo)
set_tests_properties(mutation.password_ui_model.echo_plaintext PROPERTIES
    PASS_REGULAR_EXPRESSION "PASSWORD-UI MUTANT CAUGHT")

add_executable(core_test_password_ui_model_mutant_retries test_password_ui_model.cpp)
target_link_libraries(core_test_password_ui_model_mutant_retries PRIVATE prism_core)
target_compile_definitions(core_test_password_ui_model_mutant_retries
                           PRIVATE PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES=1)
add_test(NAME "mutation.password_ui_model.unlimited_retries"
         COMMAND core_test_password_ui_model_mutant_retries)
set_tests_properties(mutation.password_ui_model.unlimited_retries PROPERTIES
    PASS_REGULAR_EXPRESSION "PASSWORD-UI MUTANT CAUGHT")
