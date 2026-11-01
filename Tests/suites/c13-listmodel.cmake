# c13-listmodel: list ordering, filtering and the column model
# (blueprint ch. 3.2 "列表": multi-key sort, 显示过滤器, 列模型).
#
# One card = one file; see suites/README.md. Tests/CMakeLists.txt is untouched.
#
# Both suites are pure logic, so the Linux lanes own their positive run AND the
# negative controls. Each mutant is the same executable with one rule disabled;
# WILL_FAIL makes CTest green only while that mutation is caught (CONTRACTS D3),
# and each mutant prints "C13 MUTANT ACTIVE: ..." so the log proves the
# injection was really compiled in -- a macro that stopped matching would leave
# the mutant green and turn its registration red.
add_executable(test_list_sort test_list_sort.cpp)
target_link_libraries(test_list_sort PRIVATE prism_core)
add_test(NAME "core.test_list_sort" COMMAND test_list_sort)
set_tests_properties(core.test_list_sort PROPERTIES TIMEOUT 180
    PASS_REGULAR_EXPRESSION "ALL LIST-SORT CHECKS PASSED")

foreach(mutant IN ITEMS UNSTABLE_SORT LEXICOGRAPHIC_NAMES DIRS_INTERLEAVED WIDTH_BYPASSES_MIN)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_list_sort_mutant_${mutant_lc}" test_list_sort.cpp)
  target_link_libraries("test_list_sort_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_list_sort_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C13_MUTANT_${mutant}=1")
  add_test(NAME "mutation.list_sort.${mutant_lc}"
           COMMAND "test_list_sort_mutant_${mutant_lc}")
  set_tests_properties("mutation.list_sort.${mutant_lc}" PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 180)
endforeach()

add_executable(test_filter_model test_filter_model.cpp)
target_link_libraries(test_filter_model PRIVATE prism_core)
add_test(NAME "core.test_filter_model" COMMAND test_filter_model)
set_tests_properties(core.test_filter_model PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL FILTER-MODEL CHECKS PASSED")

foreach(mutant IN ITEMS GLOB_STAR_STOPS_AT_SLASH SELECTION_BY_POSITION)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_filter_model_mutant_${mutant_lc}" test_filter_model.cpp)
  target_link_libraries("test_filter_model_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_filter_model_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C13_MUTANT_${mutant}=1")
  add_test(NAME "mutation.filter_model.${mutant_lc}"
           COMMAND "test_filter_model_mutant_${mutant_lc}")
  set_tests_properties("mutation.filter_model.${mutant_lc}" PROPERTIES
                       WILL_FAIL TRUE TIMEOUT 120)
endforeach()
