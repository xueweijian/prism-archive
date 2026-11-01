# c46-session: the last session (up to 8 archives + their folder + view mode),
# its schema migration, the crash guard, and the exe that restores it.
#
# One card = one file; Tests/CMakeLists.txt is untouched (see suites/README.md).
#
# Two layers, because the card has two halves:
#   * the DECISION layer (PrismCore/SessionStore.hpp) is pure, so every lane runs
#     it and three negative controls compile ONE rule out each;
#   * the JOIN (Src/App/AppWiring.hpp's file layer + Main.cpp's startup/exit
#     wiring) only exists on Windows, where the suite starts the REAL exe twice
#     -- once killed, once closed -- and reads the session document back.
#
# Verdict shape (CONTRACTS D1 + D26): every target pins its stdout marker. The
# mutants do NOT use WILL_FAIL: their binary prints "C46 MUTATION CAUGHT" only
# when the suite really went red, so a stale injection turns the target RED.
#
# NAMING RULE (C23 matrix). A test or target whose name contains clean, unarmed
# or control ANYWHERE is classified as a CONTROL entry, and a control that carries
# an injection macro is rejected as a contradiction. ARM NAMES are therefore part
# of the interface: the shutdown arm is "ignore_shutdown_flag", never
# "..._ignore_clean_shutdown". (Macro identifiers are not part of that decision,
# so PRISM_C46_MUTANT_IGNORE_SHUTDOWN_FLAG can keep its descriptive name.)
#
# CLEAN CONTROLS: each mutant is paired with the UNARMED suite built from the same
# source file, and the pairing is read from THIS file rather than from a PR
# description:
#   mutation.session_store.*    <-> core.test_session_store  (test_session_store.cpp)
#   mutation.session_restore.*  <-> win.session_restore      (win/test_session_restore.cpp)
# Neither control carries an injection define; each is the executable the matrix
# looks for when it asks "what proves the mutant's rule is the only difference?".
add_executable(test_session_store test_session_store.cpp)
target_link_libraries(test_session_store PRIVATE prism_core)
add_test(NAME "core.test_session_store" COMMAND test_session_store)
set_tests_properties(core.test_session_store PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL SESSION-STORE CHECKS PASSED")

foreach(mutant IN ITEMS KEEP_BAD_JSON IGNORE_SCHEMA_VERSION IGNORE_SHUTDOWN_FLAG)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_session_store_mutant_${mutant_lc}" test_session_store.cpp)
  target_link_libraries("test_session_store_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_session_store_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C46_MUTANT_${mutant}=1")
  add_test(NAME "mutation.session_store.${mutant_lc}"
           COMMAND "test_session_store_mutant_${mutant_lc}")
  set_tests_properties("mutation.session_store.${mutant_lc}" PROPERTIES TIMEOUT 120
                       PASS_REGULAR_EXPRESSION "C46 MUTATION CAUGHT")
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The exe under test. The suite drives the SHIPPED binary (no re-implementation
  # of the startup order), with %LOCALAPPDATA% pointed at a fixture directory so
  # the developer's own session is never touched.
  set(PRISM_C46_EXE "$<TARGET_FILE:PrismArchive>")

  # One MUTANT exe: the SAME Main.cpp with the shutdown-flag guard compiled out
  # of SessionStore::RestorableEntries. The killed-process case then reopens the
  # dead session, which is exactly the defect the card's second negative control
  # names. /WX- like every other Windows target (SDK headers are not /W4 clean).
  add_executable(prism_archive_mutant_ignore_shutdown_flag
                 "${CMAKE_SOURCE_DIR}/Src/App/Main.cpp"
                 "${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.rc")
  target_include_directories(prism_archive_mutant_ignore_shutdown_flag
                             PRIVATE "${CMAKE_SOURCE_DIR}/Src")
  target_link_libraries(prism_archive_mutant_ignore_shutdown_flag PRIVATE prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib comdlg32.lib shell32.lib shlwapi.lib comctl32.lib)
  set_target_properties(prism_archive_mutant_ignore_shutdown_flag
                        PROPERTIES WIN32_EXECUTABLE TRUE)
  target_compile_definitions(prism_archive_mutant_ignore_shutdown_flag
                             PRIVATE UNICODE _UNICODE
                                     PRISM_C46_MUTANT_IGNORE_SHUTDOWN_FLAG=1)
  target_compile_options(prism_archive_mutant_ignore_shutdown_flag PRIVATE /WX-)

  add_executable(win_session_restore_test win/test_session_restore.cpp)
  target_link_libraries(win_session_restore_test PRIVATE prism_core prism_win_bridge
                        user32.lib ole32.lib shell32.lib)
  target_compile_options(win_session_restore_test PRIVATE /WX-)
  target_compile_definitions(win_session_restore_test
                             PRIVATE "PRISM_C46_EXE=\"${PRISM_C46_EXE}\"")
  add_test(NAME "win.session_restore" COMMAND win_session_restore_test)
  set_tests_properties(win.session_restore PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "ALL SESSION-RESTORE CHECKS PASSED")

  # Same suite, pointed at the mutant exe. Its cases fail (the crash restore is
  # non-empty), so the conjunction prints CAUGHT; a stale injection would leave
  # the suite green and this target RED.
  add_executable(win_session_restore_mutant_ignore_shutdown_flag win/test_session_restore.cpp)
  target_link_libraries(win_session_restore_mutant_ignore_shutdown_flag
                        PRIVATE prism_core prism_win_bridge user32.lib ole32.lib shell32.lib)
  target_compile_options(win_session_restore_mutant_ignore_shutdown_flag PRIVATE /WX-)
  target_compile_definitions(win_session_restore_mutant_ignore_shutdown_flag
                             PRIVATE "PRISM_C46_EXE=\"$<TARGET_FILE:prism_archive_mutant_ignore_shutdown_flag>\""
                                     "PRISM_C46_MUTANT_NAME=\"ignore_shutdown_flag\"")
  add_dependencies(win_session_restore_mutant_ignore_shutdown_flag
                   prism_archive_mutant_ignore_shutdown_flag)
  add_test(NAME "mutation.session_restore.ignore_shutdown_flag"
           COMMAND win_session_restore_mutant_ignore_shutdown_flag)
  set_tests_properties(mutation.session_restore.ignore_shutdown_flag PROPERTIES TIMEOUT 600
                       PASS_REGULAR_EXPRESSION "C46 MUTATION CAUGHT")
endif()
