# c15-theme: design tokens, blueprint ch. 3.3 (the whole table).
#
# One card = one file; Tests/CMakeLists.txt is untouched (suites/README.md).
#
# The clean binary owns the positive run: exact token values, the lookup
# fallback chain, WCAG body/secondary contrast after compositing, the n=4.2
# squircle SDF, and the 5-register / 80-byte LiquidGlass cbuffer hand-off.
# The stdout marker is part of the verdict (CONTRACTS D1): a crash after the
# checks or a silent no-op cannot count as a pass.
add_executable(test_theme_tokens test_theme_tokens.cpp)
target_link_libraries(test_theme_tokens PRIVATE prism_core)
add_test(NAME "core.test_theme_tokens" COMMAND test_theme_tokens)
set_tests_properties(core.test_theme_tokens PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL THEME-TOKEN CHECKS PASSED")

# Negative controls (CONTRACTS D3). Each binary compiles ONE rule broken and is
# registered WILL_FAIL: CTest is green only while the mutation is actually
# caught. Injection is a compile definition, never a text edit, and the mutant
# prints "C15 MUTANT ACTIVE: <name>" so the log proves the switch reached the
# token header (a sed that silently stopped matching would be invisible).
# No PASS_REGULAR_EXPRESSION here on purpose: with WILL_FAIL a matching marker
# would invert the verdict into a failure.
foreach(mutant IN ITEMS SWAP_THEME NO_FALLBACK LINEAR_RAW CIRCLE_EXPONENT)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_theme_tokens_mutant_${mutant_lc}" test_theme_tokens.cpp)
  target_link_libraries("test_theme_tokens_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_theme_tokens_mutant_${mutant_lc}"
                             PRIVATE "PRISM_C15_MUTANT_${mutant}=1")
  add_test(NAME "mutation.theme.${mutant_lc}"
           COMMAND "test_theme_tokens_mutant_${mutant_lc}")
  set_tests_properties("mutation.theme.${mutant_lc}" PROPERTIES WILL_FAIL TRUE
                       TIMEOUT 60)
endforeach()
