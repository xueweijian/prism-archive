# c40-i18nwire: the i18n wiring (the three double-escaped fallbacks, error.open /
# error.engine.load, the eight menu + four sidebar keys) and the password gate
# (C35's dialog driven by C40's worker/post/DPAPI wiring).
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# D1: the tables are a COMPILE-TIME input -- a worktree that lost Assets/Lang
# fails the CONFIGURE step instead of passing a suite that reads nothing.
#
# D26: NO WILL_FAIL anywhere. Each mutation is its own executable that prints
# "C40 MUTATION CAUGHT: <id>" only when the suite went RED with the injection
# compiled in, and that marker is the target's PASS_REGULAR_EXPRESSION. A stale
# injection leaves the suite green, the marker absent and the entry red.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C40_LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Lang")
  if(NOT EXISTS "${PRISM_C40_LANG_DIR}/en-US.json" OR
     NOT EXISTS "${PRISM_C40_LANG_DIR}/zh-CN.json")
    message(FATAL_ERROR "c40-i18nwire: Assets/Lang/{en-US,zh-CN}.json are missing")
  endif()

  add_executable(win_i18n_wire win/test_i18n_wire.cpp)
  target_link_libraries(win_i18n_wire PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib ole32.lib
                        windowscodecs.lib bcrypt.lib crypt32.lib comctl32.lib
                        WindowsApp.lib)
  target_compile_options(win_i18n_wire PRIVATE /WX-)
  target_compile_definitions(win_i18n_wire PRIVATE
                             "PRISM_C40_LANG_DIR=\"${PRISM_C40_LANG_DIR}\"")
  add_test(NAME "win.i18n_wire" COMMAND win_i18n_wire)
  set_tests_properties(win.i18n_wire PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C40 I18N-WIRE CHECKS PASSED")

  # Two negative controls (CONTRACTS D3). Each mutates ONE rule the suite pins:
  #   escaped_fallback -- one of the three fallbacks goes back to the
  #                       double-escaped literal C38 found; the "no backslash"
  #                       case MUST go red.
  #   unlimited_budget  -- C35's retry gate returns true forever; "three wrong
  #                       attempts reach Exhausted" MUST go red.
  foreach(mutant IN ITEMS ESCAPED_FALLBACK UNLIMITED_BUDGET)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    set(mutant_exe "win_i18n_wire_mutant_${mutant_lc}")

    add_executable("${mutant_exe}" win/test_i18n_wire.cpp)
    target_link_libraries("${mutant_exe}" PRIVATE prism_core prism_win_bridge
                          prism_shell_contract user32.lib gdi32.lib ole32.lib
                          windowscodecs.lib bcrypt.lib crypt32.lib comctl32.lib
                          WindowsApp.lib)
    target_compile_options("${mutant_exe}" PRIVATE /WX-)
    target_compile_definitions("${mutant_exe}" PRIVATE
                               "PRISM_C40_LANG_DIR=\"${PRISM_C40_LANG_DIR}\"")
    if(mutant STREQUAL "ESCAPED_FALLBACK")
      target_compile_definitions("${mutant_exe}" PRIVATE
                                 PRISM_C40_MUTANT_ESCAPED_FALLBACK=1)
    else()
      # C35's own injection point: the budget gate lives in PasswordUiModel.
      target_compile_definitions("${mutant_exe}" PRIVATE
                                 PRISM_PWD_UI_MUTANT_UNLIMITED_RETRIES=1)
    endif()

    add_test(NAME "mutation.i18n_wire.${mutant_id}" COMMAND "${mutant_exe}")
    set_tests_properties("mutation.i18n_wire.${mutant_id}" PROPERTIES TIMEOUT 300
        # The binary prints the C++ switch spelling (underscores); the CTest entry
        # name uses dashes. Pinning the wrong one makes a CAUGHT mutation look
        # stale, so the two spellings are named separately on purpose.
        PASS_REGULAR_EXPRESSION "C40 MUTATION CAUGHT: ${mutant_lc}")
  endforeach()
endif()
