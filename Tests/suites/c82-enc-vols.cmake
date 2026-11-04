# c82-enc-vols: CLI verbs must not hang on encrypted archives (P1) and
# numeric-split 7z volumes must open (P2).
#
# The suite spawns the REAL PrismArchive.exe with redirected stderr and a real
# time bound (the win.compress_verbs shape): a verb that raises the modal
# password box never exits, and a hung batch caller is worse than an error
# code. The volume cases drive the bridge in-process against the committed
# fixtures (enc.7z header-encrypted with password PrismTest2026; the
# vols.7z.001/.002/.003 set holds one 25000-byte file, cross-checked with real
# 7-Zip when the fixtures were generated).
#
# Negative controls (D26/D27, NO WILL_FAIL -- the CAUGHT marker is the
# conjunction "the clean assertion went red AND the injection fired"):
#   * mutation.c82.cli_modal         -- the exe runs with PRISM_C82_MUTANT_CLI_MODAL=1:
#     the pre-C82 shape treats a CLI verb like the gallery, the gate comes up and
#     the process must be killed at the bound.
#   * mutation.c82.no_volume_concat  -- the bridge skips the volume concat, the
#     .001 is handed over as a lone file, and the 25000-byte round trip goes red.
#
# One card = one file; see Tests/suites/README.md.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c82_enc_vols win/test_c82_enc_vols.cpp)
  target_link_libraries(win_c82_enc_vols PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib shlwapi.lib windowscodecs.lib
                        user32.lib gdi32.lib)
  target_compile_options(win_c82_enc_vols PRIVATE /WX-)
  # The exe path and the fixture directory are part of the CONTRACT under test:
  # they reach the test through ENVIRONMENT, not argv (preflight's D1 gate wants
  # "COMMAND <exe>" to be the whole command).
  add_dependencies(win_c82_enc_vols PrismArchive)
  add_test(NAME "win.c82_enc_vols" COMMAND win_c82_enc_vols)
  set_tests_properties(win.c82_enc_vols PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C82_EXE=$<TARGET_FILE:PrismArchive>;PRISM_C82_FIXTURES=${CMAKE_SOURCE_DIR}/Tests/fixtures/c82"
      PASS_REGULAR_EXPRESSION "ALL C82 CHECKS PASSED")

  # Negative control 1 (D3/D26/D27): the pre-C82 defect -- the CLI verb opens
  # the modal password gate and hangs. The bounded spawn must kill it and the
  # case must go red.
  add_executable(win_c82_enc_vols_mutant_cli_modal win/test_c82_enc_vols.cpp)
  target_link_libraries(win_c82_enc_vols_mutant_cli_modal PRIVATE prism_core
                        prism_win_bridge ole32.lib oleaut32.lib shlwapi.lib
                        windowscodecs.lib user32.lib gdi32.lib)
  target_compile_options(win_c82_enc_vols_mutant_cli_modal PRIVATE /WX-)
  target_compile_definitions(win_c82_enc_vols_mutant_cli_modal
                             PRIVATE PRISM_C82_MUTANT_CLI_MODAL=1)
  add_dependencies(win_c82_enc_vols_mutant_cli_modal PrismArchive)
  add_test(NAME "mutation.c82.cli_modal" COMMAND win_c82_enc_vols_mutant_cli_modal)
  set_tests_properties(mutation.c82.cli_modal PROPERTIES
      TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C82_EXE=$<TARGET_FILE:PrismArchive>;PRISM_C82_FIXTURES=${CMAKE_SOURCE_DIR}/Tests/fixtures/c82"
      PASS_REGULAR_EXPRESSION "C82 MUTATION CAUGHT: cli_modal")

  # Negative control 2 (D3/D26/D27): no concatenated stream -- the handler gets
  # the lone .001 and the 25000-byte case must go red.
  add_executable(win_c82_enc_vols_mutant_no_concat win/test_c82_enc_vols.cpp)
  target_link_libraries(win_c82_enc_vols_mutant_no_concat PRIVATE prism_core
                        prism_win_bridge ole32.lib oleaut32.lib shlwapi.lib
                        windowscodecs.lib user32.lib gdi32.lib)
  target_compile_options(win_c82_enc_vols_mutant_no_concat PRIVATE /WX-)
  target_compile_definitions(win_c82_enc_vols_mutant_no_concat
                             PRIVATE PRISM_C82_MUTANT_NO_VOLUME_CONCAT=1)
  add_dependencies(win_c82_enc_vols_mutant_no_concat PrismArchive)
  add_test(NAME "mutation.c82.no_volume_concat" COMMAND win_c82_enc_vols_mutant_no_concat)
  set_tests_properties(mutation.c82.no_volume_concat PROPERTIES
      TIMEOUT 300 SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C82_EXE=$<TARGET_FILE:PrismArchive>;PRISM_C82_FIXTURES=${CMAKE_SOURCE_DIR}/Tests/fixtures/c82"
      PASS_REGULAR_EXPRESSION "C82 MUTATION CAUGHT: no_volume_concat")
endif()
