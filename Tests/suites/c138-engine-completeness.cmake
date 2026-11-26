# c138-engine-completeness: all-family multi-volume VolumeOpenCallback,
# ExtractToMemory ICryptoGetTextPassword (D30), CharsetRepair ambiguity on
# ArchiveItem/ArchiveCatalog + ReopenWithCodePage.
#
# Negative controls (CONTRACTS D3/D26/D27):
#   * mutation.c138.no_volume_callback
#   * mutation.c138.no_mem_crypto
#   * mutation.c138.no_ambiguity
add_executable(test_c138_catalog_charset test_c138_catalog_charset.cpp)
target_link_libraries(test_c138_catalog_charset PRIVATE prism_core)
add_test(NAME "core.test_c138_catalog_charset" COMMAND test_c138_catalog_charset)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c138_engine_completeness win/test_c138_engine_completeness.cpp)
  target_link_libraries(win_c138_engine_completeness PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib d2d1.lib
                        dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib
                        bcrypt.lib WindowsApp.lib)
  target_compile_options(win_c138_engine_completeness PRIVATE /WX-)
  add_test(NAME "win.c138_engine_completeness" COMMAND win_c138_engine_completeness)
  set_tests_properties(win.c138_engine_completeness PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C138 CHECKS PASSED")

  foreach(mutant IN ITEMS NO_VOLUME_CALLBACK NO_MEM_CRYPTO NO_AMBIGUITY)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_c138_mutant_${mutant_lc}" win/test_c138_engine_completeness.cpp)
    target_link_libraries("win_c138_mutant_${mutant_lc}" PRIVATE prism_core prism_win_bridge
                          prism_shell_contract user32.lib gdi32.lib d2d1.lib
                          dwrite.lib windowscodecs.lib ole32.lib oleaut32.lib
                          bcrypt.lib WindowsApp.lib)
    target_compile_options("win_c138_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_c138_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C138_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c138.${mutant_lc}" COMMAND "win_c138_mutant_${mutant_lc}")
    set_tests_properties("mutation.c138.${mutant_lc}" PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C138 MUTATION CAUGHT: ${mutant_lc}")
  endforeach()
endif()
