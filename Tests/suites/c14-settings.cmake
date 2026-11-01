# c14-settings: settings persistence (schema version + defaults + migration +
# validation + unknown-key preservation; file layer: %LOCALAPPDATA% placement,
# temp+ReplaceFile atomic write, corruption backup/recovery).
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# The store is pure PrismCore logic, so the Linux lanes own its positive run AND
# three negative controls. Each mutant is the same executable with one rule
# compiled out; WILL_FAIL makes CTest green only while the mutation is actually
# caught (CONTRACTS D3). Every mutant binary prints "C14 MUTANT ACTIVE" so the
# log proves the injection was compiled in, and -- more importantly -- a macro
# that stopped applying would leave the suite green and turn THAT entry red.
add_executable(test_settings_store test_settings_store.cpp)
target_link_libraries(test_settings_store PRIVATE prism_core)
add_test(NAME "core.test_settings_store" COMMAND test_settings_store)
set_tests_properties(core.test_settings_store PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL SETTINGS-STORE CHECKS PASSED")

foreach(mutant IN ITEMS SKIP_MIGRATION DROP_UNKNOWN_KEYS IGNORE_VALIDATION)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_settings_store_mutant_${mutant_lc}" test_settings_store.cpp)
  target_link_libraries("test_settings_store_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_settings_store_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C14_MUTANT_${mutant}=1")
  add_test(NAME "mutation.settings_store.${mutant_lc}"
           COMMAND "test_settings_store_mutant_${mutant_lc}")
  set_tests_properties("mutation.settings_store.${mutant_lc}" PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 120)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The file layer, for real: real %LOCALAPPDATA%, real ReplaceFileW, real
  # concurrent writers and readers. Nothing here can SKIP, so a failure to
  # resolve the folder is a hard FAIL rather than a silent pass (CONTRACTS D1);
  # the stdout marker is pinned as part of the verdict anyway.
  add_executable(win_settings_file_test win/test_settings_file.cpp)
  target_link_libraries(win_settings_file_test PRIVATE prism_core prism_win_bridge
                        user32.lib ole32.lib shell32.lib)
  target_compile_options(win_settings_file_test PRIVATE /WX-)
  add_test(NAME "win.settings_file" COMMAND win_settings_file_test)
  set_tests_properties(win.settings_file PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL SETTINGS-FILE CHECKS PASSED")

  # Negative control for the atomic write: the SAME suite compiled with the save
  # path replaced by "truncate the live file and stream the bytes in". The
  # concurrent-reader case must catch the torn window; WILL_FAIL is the verdict,
  # and "C14 MUTANT ACTIVE" in the log is the injection's own evidence.
  add_executable(win_settings_file_mutant_inplace win/test_settings_file.cpp)
  target_link_libraries(win_settings_file_mutant_inplace PRIVATE prism_core prism_win_bridge
                        user32.lib ole32.lib shell32.lib)
  target_compile_options(win_settings_file_mutant_inplace PRIVATE /WX-)
  target_compile_definitions(win_settings_file_mutant_inplace
                             PRIVATE PRISM_C14_MUTANT_NONATOMIC_SAVE=1)
  add_test(NAME "mutation.settings_file.inplace_save"
           COMMAND win_settings_file_mutant_inplace)
  set_tests_properties(mutation.settings_file.inplace_save PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 300)
endif()
