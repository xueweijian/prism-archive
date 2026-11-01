# c5-security: MoTW inheritance + AMSI pre-scan + Zip-bomb dual condition
# (blueprint 7.1 #1, #3). One card = one file; see suites/README.md.
#
# The policy suite is compiled six times: once clean, then once per deliberate
# rule mutation. Each mutant is registered with WILL_FAIL, so CTest is green only
# while the mutation is actually caught (CONTRACTS D3 -- an injection that
# silently stops matching would otherwise look like a sharp test).
add_executable(test_amsi_policy test_amsi_policy.cpp)
target_link_libraries(test_amsi_policy PRIVATE prism_core)
add_test(NAME "core.test_amsi_policy" COMMAND test_amsi_policy)

foreach(mutant IN ITEMS UNAVAILABLE_IS_ALLOW NEVER_STAMP IGNORE_PER_FILE_RATIO
                          IGNORE_DISK_FRACTION IGNORE_CHUNK_OVERLAP)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_amsi_policy_mutant_${mutant_lc}" test_amsi_policy.cpp)
  target_link_libraries("test_amsi_policy_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_amsi_policy_mutant_${mutant_lc}"
                             PRIVATE "PRISM_SEC_MUTANT_${mutant}=1")
  add_test(NAME "mutation.security.${mutant_lc}"
           COMMAND "test_amsi_policy_mutant_${mutant_lc}")
  set_tests_properties("mutation.security.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # Real NTFS Zone.Identifier ADS. No malicious bytes: runs by default in CI.
  add_executable(win_motw_stamp win/test_motw_stamp.cpp)
  target_link_libraries(win_motw_stamp PRIVATE prism_core prism_win_bridge
                                                user32.lib advapi32.lib)
  target_compile_options(win_motw_stamp PRIVATE /WX-)
  add_test(NAME "win.motw_stamp" COMMAND win_motw_stamp)
  set_tests_properties(win.motw_stamp PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL MOTW-STAMP CHECKS PASSED")

  # AMSI live suite: OPT-IN because it synthesises an EICAR sample at runtime
  # (assembled from fragments; no canonical signature in the repo). The default
  # run prints AMSI-LIVE SKIPPED and exits 77, which CTest maps to
  # SKIP_RETURN_CODE -- a visible "Skipped", never a fake green (CONTRACTS D1).
  add_executable(win_amsi_guard win/test_amsi_guard.cpp)
  target_link_libraries(win_amsi_guard PRIVATE prism_core prism_win_bridge
                                                 user32.lib advapi32.lib)
  target_compile_options(win_amsi_guard PRIVATE /WX-)
  add_test(NAME "win.amsi_guard" COMMAND win_amsi_guard)
  set_tests_properties(win.amsi_guard PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL AMSI-GUARD CHECKS PASSED")
endif()
