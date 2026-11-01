# c47-shelli18n: the Win11 IExplorerCommand handler renders the SAME translated
# menu the Win10 classic handler does, through the shared ShellTranslator.
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# D1: the tables are a COMPILE-TIME input -- a worktree that lost Assets/Lang
# fails the CONFIGURE step instead of passing a suite that reads nothing, and the
# trailing stdout marker is the verdict.
#
# D26: NO WILL_FAIL anywhere. The negative control is its own DLL (the verb id
# where the translated title belongs) plus its own executable, and the
# "C47 MUTATION CAUGHT: verb_title" marker is that target's
# PASS_REGULAR_EXPRESSION. The executable prints it only when the suite went red
# AND the injected shape was actually observed, so a stale injection leaves the
# suite green, the marker absent and the entry red.
#
# Note: D27's SetMutationVerdict is not used because main currently carries the
# revert of card C44 (ce6cf26); C44's re-land (PR #67) will arm the same
# predicate from the first failure.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C47_LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Lang")
  if(NOT EXISTS "${PRISM_C47_LANG_DIR}/en-US.json" OR
     NOT EXISTS "${PRISM_C47_LANG_DIR}/zh-CN.json")
    message(FATAL_ERROR "c47-shelli18n: Assets/Lang/{en-US,zh-CN}.json are missing")
  endif()

  # The handler source and its export list. The shipping target (PrismWin/
  # CMakeLists.txt) stays untouched; the test loads a COPY of the real DLL from a
  # fixture of its own, so "resolved from the module directory, not the host exe"
  # is observable rather than assumed.
  set(PRISM_C47_HANDLER_SOURCE "${CMAKE_SOURCE_DIR}/Src/PrismWin/PrismShellCommand.cpp")
  set(PRISM_C47_HANDLER_DEF "${CMAKE_SOURCE_DIR}/Src/PrismWin/PrismShellExt.def")
  set(PRISM_C47_WIN_LIBS user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
                        shlwapi.lib WindowsApp.lib)

  add_executable(win_shell_i18n win/test_shell_i18n.cpp)
  target_link_libraries(win_shell_i18n PRIVATE prism_core prism_win_bridge
                        prism_shell_contract ${PRISM_C47_WIN_LIBS})
  target_compile_options(win_shell_i18n PRIVATE /WX-)
  target_compile_definitions(win_shell_i18n PRIVATE
                             "PRISM_C47_LANG_DIR=\"${PRISM_C47_LANG_DIR}\""
                             "PRISM_C47_SHELL_DLL=\"$<TARGET_FILE:PrismShellExt>\""
                             "PRISM_C47_CLASSIC_DLL=\"$<TARGET_FILE:PrismShellExtClassic>\"")
  add_dependencies(win_shell_i18n PrismShellExt PrismShellExtClassic)
  add_test(NAME "win.shell_i18n" COMMAND win_shell_i18n)
  set_tests_properties(win.shell_i18n PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C47 SHELL-I18N CHECKS PASSED")

  # --- Negative control (CONTRACTS D3): the handler with the translation removed.
  #     Its own DLL, its own export list, its own executable -- the shipping
  #     PrismShellExt target is never built with a mutation.
  add_library(prism_c47_shell_verb_title SHARED "${PRISM_C47_HANDLER_SOURCE}")
  target_link_libraries(prism_c47_shell_verb_title PRIVATE prism_core prism_shell_contract
                        shlwapi.lib)
  target_compile_options(prism_c47_shell_verb_title PRIVATE /WX-)
  target_compile_definitions(prism_c47_shell_verb_title PRIVATE
                             PRISM_C47_MUTANT_VERB_TITLE=1)
  set_target_properties(prism_c47_shell_verb_title PROPERTIES
                        OUTPUT_NAME "PrismShellExt_c47_verb_title")
  if(MSVC)
    set_target_properties(prism_c47_shell_verb_title PROPERTIES
                          LINK_FLAGS "/DEF:${PRISM_C47_HANDLER_DEF}")
  endif()

  add_executable(win_shell_i18n_mutant_verb_title win/test_shell_i18n.cpp)
  target_link_libraries(win_shell_i18n_mutant_verb_title PRIVATE prism_core prism_win_bridge
                        prism_shell_contract ${PRISM_C47_WIN_LIBS})
  target_compile_options(win_shell_i18n_mutant_verb_title PRIVATE /WX-)
  target_compile_definitions(win_shell_i18n_mutant_verb_title PRIVATE
                             PRISM_C47_MUTANT_VERB_TITLE=1
                             "PRISM_C47_LANG_DIR=\"${PRISM_C47_LANG_DIR}\""
                             "PRISM_C47_SHELL_DLL=\"$<TARGET_FILE:prism_c47_shell_verb_title>\""
                             "PRISM_C47_CLASSIC_DLL=\"$<TARGET_FILE:PrismShellExtClassic>\"")
  add_dependencies(win_shell_i18n_mutant_verb_title prism_c47_shell_verb_title
                   PrismShellExtClassic)
  add_test(NAME "mutation.shell_i18n.verb_title" COMMAND win_shell_i18n_mutant_verb_title)
  set_tests_properties("mutation.shell_i18n.verb_title" PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "C47 MUTATION CAUGHT: verb_title")
endif()