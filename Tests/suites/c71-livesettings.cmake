# c71-livesettings: the four settings C69 measured as inert now have a runtime
# consumer, and each one has a negative control that MUST make its assertion red.
#
#   appearance.accentColor       -> progress-capsule pixel (and every accent use)
#   appearance.glassIntensity    -> action-island pixel == token * intensity
#   extract.overwrite            -> replace / skip / rename on real 7z bytes
#   extract.verifyAfterExtract   -> corrupted entry: off succeeds, on fails
#
# The clean target pins ALL C71 LIVE-SETTINGS CHECKS PASSED (D1: exit code AND
# marker). Every mutant compiles the SAME source with one consumer removed and
# pins "C71 MUTATION CAUGHT: <arm>"; the marker is printed at the first failing
# assertion by mini_test's D27 mechanism, so no WILL_FAIL is stacked on the
# regex (D26). The arms are named without clean/unarmed/control (D28).
add_executable(test_c71_livesettings test_c71_livesettings.cpp)
target_link_libraries(test_c71_livesettings PRIVATE prism_core)
add_test(NAME "core.c71_livesettings" COMMAND test_c71_livesettings)
set_tests_properties(core.c71_livesettings PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C71 LIVE-SETTINGS CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C71_LIBS prism_core prism_win_bridge prism_shell_contract
      user32.lib gdi32.lib shell32.lib ole32.lib oleaut32.lib windowscodecs.lib
      d2d1.lib dwrite.lib bcrypt.lib comctl32.lib comdlg32.lib shlwapi.lib)

  add_executable(win_c71_livesettings win/test_c71_livesettings.cpp)
  target_link_libraries(win_c71_livesettings PRIVATE ${PRISM_C71_LIBS})
  target_compile_options(win_c71_livesettings PRIVATE /WX-)  # B9: SDK headers
  add_test(NAME "win.c71_livesettings" COMMAND win_c71_livesettings)
  set_tests_properties(win.c71_livesettings PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      RESOURCE_LOCK "prism-7z-cli"
      PASS_REGULAR_EXPRESSION "ALL C71 LIVE-SETTINGS CHECKS PASSED")

  # One binary per consumer. The macro lives in the consumer header, so the
  # injection is compiled from the SAME source the clean target uses.
  set(PRISM_C71_ARMS ignore_accent ignore_glass ignore_overwrite ignore_verify)
  foreach(arm IN LISTS PRISM_C71_ARMS)
    string(TOUPPER "${arm}" arm_upper)
    add_executable("win_c71_livesettings_mutant_${arm}" win/test_c71_livesettings.cpp)
    target_link_libraries("win_c71_livesettings_mutant_${arm}" PRIVATE ${PRISM_C71_LIBS})
    target_compile_options("win_c71_livesettings_mutant_${arm}" PRIVATE /WX-)
    target_compile_definitions("win_c71_livesettings_mutant_${arm}"
                               PRIVATE "PRISM_C71_MUTANT_${arm_upper}=1")
    add_test(NAME "c71.mutation.livesettings.${arm}"
             COMMAND "win_c71_livesettings_mutant_${arm}")
    set_tests_properties("c71.mutation.livesettings.${arm}" PROPERTIES TIMEOUT 300
        RESOURCE_LOCK "prism-7z-cli"
        PASS_REGULAR_EXPRESSION "C71 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
