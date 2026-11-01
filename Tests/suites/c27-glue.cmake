# c27-glue: the four blocks that landed with suites of their own but were never
# reachable from the exe are now WIRED (wave 3). One card = one file; see
# Tests/suites/README.md and CONTRACTS D1/D3/D26.
#
# The suite drives Src/App/AppWiring.hpp -- the header Main.cpp calls -- against a
# real 7z.dll, a real HWND and a real Direct2D render target:
#   (1) compress -> re-open -> byte-compare (C9 + C18 entry),
#   (2) settings written by one process and read by a SECOND one (C14),
#   (3) the sidebar command list reaches pixels (C12 -> C6 renderer),
#   (4) a language switch changes a real menu title, and the fallback chain
#       resolves keys only en-US defines (C19).
#
# D1: a runner without 7z.dll prints WHY and exits 77 (SKIP_RETURN_CODE); the
# stdout marker is required, so a silent skip can never pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The language tables are a COMPILE-TIME input: a worktree that lost
  # Assets/Lang fails the build instead of passing a suite that reads nothing.
  set(PRISM_C27_LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Lang")
  if(NOT EXISTS "${PRISM_C27_LANG_DIR}/zh-CN.json")
    message(FATAL_ERROR "c27-glue: Assets/Lang/zh-CN.json is missing")
  endif()

  add_executable(win_glue win/test_glue.cpp)
  target_link_libraries(win_glue PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
                        shlwapi.lib windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_glue PRIVATE /WX-)
  target_compile_definitions(win_glue PRIVATE
                             "PRISM_C27_ASSETS_DIR=\"${PRISM_C27_LANG_DIR}\"")
  add_test(NAME "win.glue" COMMAND win_glue)
  set_tests_properties(win.glue PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C27-GLUE CHECKS PASSED")

  # NEGATIVE CONTROLS (CONTRACTS D3 + D26): one regression per build, NO
  # WILL_FAIL anywhere. The mutant prints a banner, and only when the suite went
  # RED does it print "C27 MUTATION CAUGHT: <id>" and exit 0 -- that marker is
  # this target's PASS_REGULAR_EXPRESSION. If the injection ever stops applying,
  # the suite stays green, no marker appears and the entry goes red.
  foreach(mutant IN ITEMS SETTINGS_MEMORY_ONLY NO_I18N_FALLBACK NO_SIDEBAR_PAINT)
    string(TOLOWER "${mutant}" mutant_lc)
    string(REPLACE "_" "-" mutant_id "${mutant_lc}")
    add_executable("win_glue_mutant_${mutant_lc}" win/test_glue.cpp)
    target_link_libraries("win_glue_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge user32.lib gdi32.lib ole32.lib
                          oleaut32.lib shell32.lib shlwapi.lib windowscodecs.lib
                          d2d1.lib dwrite.lib)
    target_compile_options("win_glue_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_glue_mutant_${mutant_lc}" PRIVATE
                               "PRISM_C27_MUTANT_${mutant}=1"
                               "PRISM_C27_ASSETS_DIR=\"${PRISM_C27_LANG_DIR}\"")
    add_test(NAME "mutation.glue.${mutant_id}" COMMAND "win_glue_mutant_${mutant_lc}")
    set_tests_properties("mutation.glue.${mutant_id}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C27 MUTATION CAUGHT: ${mutant_id}")
  endforeach()
endif()
