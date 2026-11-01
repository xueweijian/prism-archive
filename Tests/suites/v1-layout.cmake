# c/v1-layout: the main window's ONE geometry authority (DESIGN.md §2/§3).
#
# What is registered here
#   * core.test_layout_invariants   the real suite: bands never overlap, items
#                                   stay inside the content, a collapsed rail
#                                   leaves no ghost, the column formula at
#                                   800/1000/1200/1600, and the P0-2 rule
#                                   "no thumbnail -> 32 px row".
#   * mutation.layout.*             two D3 controls. Each compiles the SAME
#                                   source with exactly one rule broken and is
#                                   registered WILL_FAIL with NO
#                                   PASS_REGULAR_EXPRESSION (D26: those two
#                                   properties must never be stacked).
#
# Everything here is pure integer geometry: no window, no D2D, no 7z.dll.
add_executable(test_layout_invariants test_layout_invariants.cpp)
target_link_libraries(test_layout_invariants PRIVATE prism_core)
if(MSVC)
  # The suite is pure C++20, but the shared /WX policy also covers the
  # generated CMake probe objects; /WX- keeps SDK-adjacent noise out of the way.
  target_compile_options(test_layout_invariants PRIVATE /WX-)
endif()
add_test(NAME "core.test_layout_invariants" COMMAND test_layout_invariants)
# D1: the stdout marker is a CONJUNCTION with exit 0 (this suite cannot skip,
# so no SKIP_RETURN_CODE is involved).
set_tests_properties(core.test_layout_invariants PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL LAYOUT-INVARIANTS CHECKS PASSED")

# ---------------------------------------------------------------------------
# D3 negative controls.
#   no_thumb_fallback: every non-folder item becomes a tile -> the skeleton
#                      card is back and the classification assertions go red.
#   ignore_sidebar:    the rail reserves no width -> the content overlaps it
#                      and the "content starts at sidebar.right" checks go red.
# Each source prints "V1 MUTANT ACTIVE: ..." when its injection is compiled in,
# so a red run names the rule it caught (and a dead injection is visible).
# ---------------------------------------------------------------------------
foreach(mutant IN ITEMS no_thumb_fallback ignore_sidebar)
  add_executable(test_layout_invariants_mutant_${mutant} test_layout_invariants.cpp)
  target_link_libraries(test_layout_invariants_mutant_${mutant} PRIVATE prism_core)
  if(MSVC)
    target_compile_options(test_layout_invariants_mutant_${mutant} PRIVATE /WX-)
  endif()
  if(mutant STREQUAL "no_thumb_fallback")
    target_compile_definitions(test_layout_invariants_mutant_${mutant}
                               PRIVATE PRISM_V1_MUTANT_NO_THUMB_FALLBACK=1)
  else()
    target_compile_definitions(test_layout_invariants_mutant_${mutant}
                               PRIVATE PRISM_V1_MUTANT_IGNORE_SIDEBAR=1)
  endif()
  add_test(NAME "mutation.layout.${mutant}" COMMAND test_layout_invariants_mutant_${mutant})
  set_tests_properties(mutation.layout.${mutant} PROPERTIES WILL_FAIL TRUE TIMEOUT 120)
endforeach()
