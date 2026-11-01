# c12-sidebar: sidebar of blueprint 3.2 -- FAVORITES / RECENT / ARCHIVE TREE /
# bottom telemetry capsule, its draw-command view, and the keyboard focus order.
#
# One card = one file; see Tests/suites/README.md. The model suite is compiled
# once clean and once per deliberate rule mutation; each mutant is registered
# WILL_FAIL, so CTest is green only while the mutation is really caught
# (CONTRACTS D3). The marker on the clean target is a CONJUNCTION with the exit
# code (D1): a silent skip or an early return can never pass.
add_executable(test_sidebar_model test_sidebar_model.cpp)
target_link_libraries(test_sidebar_model PRIVATE prism_core)
add_test(NAME "core.test_sidebar_model" COMMAND test_sidebar_model)
set_tests_properties(core.test_sidebar_model PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL SIDEBAR-MODEL CHECKS PASSED")

foreach(mutant IN ITEMS NO_RECENT_DEDUPE NO_RECENT_CAP NO_FOCUS_WRAP)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_sidebar_model_mutant_${mutant_lc}" test_sidebar_model.cpp)
  target_link_libraries("test_sidebar_model_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_sidebar_model_mutant_${mutant_lc}"
                             PRIVATE "PRISM_SIDEBAR_MUTANT_${mutant}=1")
  add_test(NAME "mutation.sidebar.${mutant_lc}"
           COMMAND "test_sidebar_model_mutant_${mutant_lc}")
  set_tests_properties("mutation.sidebar.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The view is pure geometry: no D2D, no HWND, no font. It still belongs to
  # the Windows lane because the renderer that consumes it does (card C6).
  add_executable(win_sidebar_view win/test_sidebar_view.cpp)
  target_link_libraries(win_sidebar_view PRIVATE prism_core prism_win_bridge)
  target_compile_options(win_sidebar_view PRIVATE /WX-)
  add_test(NAME "win.sidebar_view" COMMAND win_sidebar_view)
  set_tests_properties(win.sidebar_view PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL SIDEBAR-VIEW CHECKS PASSED")

  foreach(mutant IN ITEMS NO_INDENT NO_CULL)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_sidebar_view_mutant_${mutant_lc}" win/test_sidebar_view.cpp)
    target_link_libraries("win_sidebar_view_mutant_${mutant_lc}"
                          PRIVATE prism_core prism_win_bridge)
    target_compile_options("win_sidebar_view_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_sidebar_view_mutant_${mutant_lc}"
                               PRIVATE "PRISM_SIDEBAR_MUTANT_VIEW_${mutant}=1")
    add_test(NAME "mutation.sidebar.view_${mutant_lc}"
             COMMAND "win_sidebar_view_mutant_${mutant_lc}")
    set_tests_properties("mutation.sidebar.view_${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
  endforeach()
endif()
