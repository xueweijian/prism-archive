# c62-searchui: the SEARCH BOX model + its paint instructions (no window).
#
# Src/PrismCore/SearchBoxModel.hpp owns the editing/IME/Escape rules and the
# query -> FilterModel composition. Src/PrismWin/SearchBoxPainter.hpp turns the
# model into drawing instructions carrying C15 tokens; it draws nothing, so it
# is asserted headlessly. The UI wiring is a later card on purpose.
#
# The clean core binary owns the positive run (editor semantics, three-stage
# Escape, composition preview vs commit, empty query = no filter, exact state
# strings, determinism).
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27): the same test source builds
# twice with ONE rule broken --
#   esc_closes_on_first  -> the first Escape closes the box
#   empty_query_filters  -> an empty query hides every row
# Neither uses WILL_FAIL. Each binary arms SetMutationVerdict with a runtime
# predicate, prints "C62 MUTATION CAUGHT: <id>" at the FIRST red assertion and
# exits 0, so PASS_REGULAR_EXPRESSION is the verdict; a stale injection leaves
# the target RED on the missing marker. Arm names avoid clean/unarmed/control
# (D28).
add_executable(test_search_box test_search_box.cpp)
target_link_libraries(test_search_box PRIVATE prism_core)
add_test(NAME "core.search_box" COMMAND test_search_box)
set_tests_properties(core.search_box PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL SEARCH-BOX CHECKS PASSED")

foreach(mutant IN ITEMS ESC_CLOSES_ON_FIRST EMPTY_QUERY_FILTERS)
  string(TOLOWER "${mutant}" mutant_lc)
  string(REPLACE "_" "-" mutant_id "${mutant_lc}")
  if(mutant STREQUAL "ESC_CLOSES_ON_FIRST")
    set(mutant_marker "esc_closes_on_first")
  else()
    set(mutant_marker "empty_query_filters")
  endif()
  add_executable("test_search_box_mutant_${mutant_lc}" test_search_box.cpp)
  target_link_libraries("test_search_box_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_search_box_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C62_MUTANT_${mutant}=1")
  add_test(NAME "c62.mutation.search_box.${mutant_id}"
           COMMAND "test_search_box_mutant_${mutant_lc}")
  set_tests_properties("c62.mutation.search_box.${mutant_id}" PROPERTIES TIMEOUT 60
      PASS_REGULAR_EXPRESSION "C62 MUTATION CAUGHT: ${mutant_marker}")
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_search_painter_test win/test_search_painter.cpp)
  target_link_libraries(win_search_painter_test PRIVATE prism_core prism_win_bridge)
  # The SDK headers are not /W4-clean; this card's own code is.
  target_compile_options(win_search_painter_test PRIVATE /WX-)
  add_test(NAME "win.search_painter" COMMAND win_search_painter_test)
  set_tests_properties(win.search_painter PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL SEARCH-PAINTER CHECKS PASSED")
endif()
