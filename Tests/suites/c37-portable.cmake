# c37-portable: 7z.dll discovery for a PORTABLE 7-Zip (card C37).
#
# Why this card exists (measured on the dev box, 2026-11-01): 7-Zip unpacked into
# E:\DevTools\7-Zip and never installed. C1's ladder (env -> exe dir -> registry
# -> the two Program Files probes) therefore resolved nothing, PrismArchive.exe
# exited 0 without an artifact, and the "7z.dll not found" box listed three dead
# ends. This suite proves the repaired ladder: env -> exe dir -> registry -> PATH
# (in PATH order) -> common per-user dirs -> the two Program Files probes, every
# tried path labelled with its source.
#
# The suite NEVER skips: discovery only stats files, so the fixture plants a real
# 7z.dll copy (or a 4-byte placeholder) in its own %TEMP% tree, unique per process
# and per card. A machine with no 7-Zip at all still runs every case.
#
# D1: the stdout marker is part of the verdict -- CTest needs exit 0 AND
# "ALL PORTABLE-LOOKUP CHECKS PASSED", so a silent skip or a crash after printing
# is still red.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_portable_lookup win/test_portable_lookup.cpp)
  target_link_libraries(win_portable_lookup PRIVATE prism_core prism_win_bridge
                        advapi32.lib)
  target_compile_options(win_portable_lookup PRIVATE /WX-)
  add_test(NAME "win.portable_lookup" COMMAND win_portable_lookup)
  set_tests_properties(win.portable_lookup PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL PORTABLE-LOOKUP CHECKS PASSED")

  # D3 negative control, shape per CONTRACTS D26 -- NO WILL_FAIL anywhere.
  # The SAME suite compiled once more with the PATH rung removed from
  # Resolve7zDllFrom (PrismEngineBridge.hpp). The binary prints a banner (proof
  # the definition was applied), and prints "C37 MUTATION CAUGHT: no_path_scan"
  # ONLY when the suite actually went red AND it independently measured zero
  # PATH-sourced candidates -- so an injection that stopped applying (suite
  # green, or the rung still there) cannot be mistaken for a sharp test: no
  # marker, this target goes red.
  add_executable(win_portable_lookup_mutant_no_path win/test_portable_lookup.cpp)
  target_link_libraries(win_portable_lookup_mutant_no_path PRIVATE prism_core
                        prism_win_bridge advapi32.lib)
  target_compile_options(win_portable_lookup_mutant_no_path PRIVATE /WX-)
  target_compile_definitions(win_portable_lookup_mutant_no_path
                             PRIVATE PRISM_C37_MUTANT_NO_PATH_SCAN=1)
  add_test(NAME "mutation.portable_lookup.no_path_scan"
           COMMAND win_portable_lookup_mutant_no_path)
  set_tests_properties(mutation.portable_lookup.no_path_scan PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C37 MUTATION CAUGHT: no_path_scan")
endif()
