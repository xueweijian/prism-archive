# c58-compressverbs: the right-click CREATION verbs actually work.
#
# C4's registrar has rendered --compress-7z / --compress-zip /
# --smart-extract-each since the shell extension landed; C25 pinned the exe
# REFUSING them, so the menu advertised three commands the product answered with
# "this version does not support that command". This suite drives the exact
# functions Main.cpp calls against a real 7z.dll, and then drives the REAL
# PrismArchive.exe through a real command line (exit code + redirected stderr).
#
# One card = one file; see Tests/suites/README.md.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_compress_verbs win/test_compress_verbs.cpp)
  target_link_libraries(win_compress_verbs PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib shlwapi.lib windowscodecs.lib
                        user32.lib gdi32.lib)
  target_compile_options(win_compress_verbs PRIVATE /WX-)
  # The exe path is part of the CONTRACT under test: the last case spawns the
  # shipped binary and asserts its exit code and stderr, so a wiring that only
  # works in-process cannot pass.
  add_dependencies(win_compress_verbs PrismArchive)
  # The exe path reaches the test through ENVIRONMENT (a generator expression),
  # not argv: tools/preflight.py's D1 gate matches "COMMAND <exe>" exactly, and
  # a second argv token would make the SKIP gate unreadable to it.
  add_test(NAME "win.compress_verbs" COMMAND win_compress_verbs)
  set_tests_properties(win.compress_verbs PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C58_EXE=$<TARGET_FILE:PrismArchive>"
      PASS_REGULAR_EXPRESSION "ALL COMPRESS-VERBS CHECKS PASSED")

  # Negative control 1 (D3/D26/D27): the FORMAT mapping is constant 7z, so
  # --compress-zip produces a .7z under a .zip name -- the exact defect the menu
  # promises it will not do. The zip assertions must go red.
  add_executable(win_compress_verbs_mutant_zip_as_7z win/test_compress_verbs.cpp)
  target_link_libraries(win_compress_verbs_mutant_zip_as_7z PRIVATE prism_core
                        prism_win_bridge ole32.lib oleaut32.lib shlwapi.lib
                        windowscodecs.lib user32.lib gdi32.lib)
  target_compile_options(win_compress_verbs_mutant_zip_as_7z PRIVATE /WX-)
  target_compile_definitions(win_compress_verbs_mutant_zip_as_7z
                             PRIVATE PRISM_C58_MUTANT_ZIP_AS_7Z=1)
  add_dependencies(win_compress_verbs_mutant_zip_as_7z PrismArchive)
  add_test(NAME "mutation.compress_verbs.zip_as_7z"
           COMMAND win_compress_verbs_mutant_zip_as_7z)
  set_tests_properties(mutation.compress_verbs.zip_as_7z PROPERTIES
      TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C58_EXE=$<TARGET_FILE:PrismArchive>"
      PASS_REGULAR_EXPRESSION "C58 MUTATION CAUGHT: zip_as_7z")

  # Negative control 2 (D3/D26/D27): a failed creation leaves an empty output
  # file behind. The failure-path "nothing on disk" assertion must go red.
  add_executable(win_compress_verbs_mutant_create_on_failure win/test_compress_verbs.cpp)
  target_link_libraries(win_compress_verbs_mutant_create_on_failure PRIVATE prism_core
                        prism_win_bridge ole32.lib oleaut32.lib shlwapi.lib
                        windowscodecs.lib user32.lib gdi32.lib)
  target_compile_options(win_compress_verbs_mutant_create_on_failure PRIVATE /WX-)
  target_compile_definitions(win_compress_verbs_mutant_create_on_failure
                             PRIVATE PRISM_C58_MUTANT_CREATE_ON_FAILURE=1)
  add_dependencies(win_compress_verbs_mutant_create_on_failure PrismArchive)
  add_test(NAME "mutation.compress_verbs.create_on_failure"
           COMMAND win_compress_verbs_mutant_create_on_failure)
  set_tests_properties(mutation.compress_verbs.create_on_failure PROPERTIES
      TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C58_EXE=$<TARGET_FILE:PrismArchive>"
      PASS_REGULAR_EXPRESSION "C58 MUTATION CAUGHT: create_on_failure")
endif()
