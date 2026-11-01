# c10-volumes: multi-volume planning + real 7z.dll volume streams (blueprint 2.1
# "extract the volume-merge logic"). One card = one file; see suites/README.md.
#
# The pure planner suite is compiled three extra times with a deliberate
# mutation each (CONTRACTS D3): a guard that stays green under its own mutant is
# not a guard. WILL_FAIL makes CTest green only while the mutation is caught.
add_executable(test_volume_plan test_volume_plan.cpp)
target_link_libraries(test_volume_plan PRIVATE prism_core)
add_test(NAME "core.test_volume_plan" COMMAND test_volume_plan)

foreach(mutant IN ITEMS IGNORE_GAPS ACCEPT_UNSORTED LAST_VOLUME_OPTIONAL)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_volume_plan_mutant_${mutant_lc}" test_volume_plan.cpp)
  target_link_libraries("test_volume_plan_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_volume_plan_mutant_${mutant_lc}"
                             PRIVATE "PRISM_VOLUME_MUTANT_${mutant}=1")
  add_test(NAME "mutation.volume_plan.${mutant_lc}"
           COMMAND "test_volume_plan_mutant_${mutant_lc}")
  set_tests_properties("mutation.volume_plan.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # Real 7z.dll over a real three-volume 7z authored by the 7z.exe that ships
  # beside it; fixtures are generated at runtime and never committed.
  add_executable(win_volumes win/test_volumes.cpp)
  target_link_libraries(win_volumes PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib bcrypt.lib)
  target_compile_options(win_volumes PRIVATE /WX-)
  add_test(NAME "win.volumes" COMMAND win_volumes)
  # The stdout marker is part of the verdict (CONTRACTS D1): a silent SKIP when
  # 7z.dll is absent can never count as a pass.
  set_tests_properties(win.volumes PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL VOLUME CHECKS PASSED")
endif()
