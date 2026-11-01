# c25-assembly: the EXE actually USES the modules that were merged (wave 2).
#
# Fourteen modules landed on main with their own suites; nothing in the
# executable called them. This suite is the wiring's own proof, against a real
# 7z.dll, a real archive and a real HWND:
#   (1) sweep + extract run at the same time on DIFFERENT handler slots (the
#       docs/PROGRESS shared-reader debt) without cross-talk;
#   (2) extracted files inherit :Zone.Identifier byte-for-byte, and an archive
#       without a mark stamps nothing;
#   (3) C4's ArgsForVerb output parses into four DIFFERENT plans, and the three
#       extracting verbs really produce three different on-disk layouts, while
#       --test decodes everything and writes nothing;
#   (4) the C8 key table changes real window state (list mode, palette, history,
#       smart extract); (5) the C13 list view is sorted/filtered and the
#       selection still resolves to the right folder row.
#
# D1: the suite can SKIP (no 7z.dll on the runner) -- it prints WHY and exits 77,
# which CTest maps to SKIP_RETURN_CODE, and the stdout marker is required, so a
# silent skip can never count as a pass.
#
# D3 negative controls: the same suite compiled twice more, each with ONE
# deliberate regression injected at compile time (a definition cannot silently
# fail to apply). Shape per CONTRACTS D26 -- NO WILL_FAIL anywhere:
#   * the mutant prints a banner (proof the injection compiled in) and, only if
#     the suite actually went RED, the marker "C25 MUTATION CAUGHT";
#   * the marker is this target's PASS_REGULAR_EXPRESSION and the binary exits 0
#     only when it printed it. If the mutation ever stops applying the suite stays
#     green, no marker appears and this target fails -- the guard proves itself.
# (WILL_FAIL + a pass marker is always red; WILL_FAIL alone turns a SKIP into a
# green. Both are the failure modes D26 was written for.)
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_app_wiring win/test_app_wiring.cpp)
  target_link_libraries(win_app_wiring PRIVATE prism_core prism_win_bridge
                        prism_shell_contract user32.lib gdi32.lib ole32.lib
                        windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_app_wiring PRIVATE /WX-)
  add_test(NAME "win.app_wiring" COMMAND win_app_wiring)
  set_tests_properties(win.app_wiring PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL APP-WIRING CHECKS PASSED")

  # Negative control 1: the pre-C11 shape -- ONE ArchiveReader shared by every
  # concurrent operation. (1) must go red.
  add_executable(win_app_wiring_mutant_shared_reader win/test_app_wiring.cpp)
  target_link_libraries(win_app_wiring_mutant_shared_reader PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        ole32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_app_wiring_mutant_shared_reader PRIVATE /WX-)
  target_compile_definitions(win_app_wiring_mutant_shared_reader
                             PRIVATE PRISM_C25_MUTANT_SHARED_READER=1)
  add_test(NAME "mutation.app_wiring.shared_reader"
           COMMAND win_app_wiring_mutant_shared_reader)
  set_tests_properties(mutation.app_wiring.shared_reader PROPERTIES
                       TIMEOUT 300 SKIP_RETURN_CODE 77
                       PASS_REGULAR_EXPRESSION "C25 MUTATION CAUGHT: shared_reader")

  # Negative control 2: :Zone.Identifier stamping removed. (2) must go red.
  add_executable(win_app_wiring_mutant_no_motw win/test_app_wiring.cpp)
  target_link_libraries(win_app_wiring_mutant_no_motw PRIVATE prism_core
                        prism_win_bridge prism_shell_contract user32.lib gdi32.lib
                        ole32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
                        WindowsApp.lib)
  target_compile_options(win_app_wiring_mutant_no_motw PRIVATE /WX-)
  target_compile_definitions(win_app_wiring_mutant_no_motw
                             PRIVATE PRISM_C25_MUTANT_NO_MOTW=1)
  add_test(NAME "mutation.app_wiring.no_motw" COMMAND win_app_wiring_mutant_no_motw)
  set_tests_properties(mutation.app_wiring.no_motw PROPERTIES
                       TIMEOUT 300 SKIP_RETURN_CODE 77
                       PASS_REGULAR_EXPRESSION "C25 MUTATION CAUGHT: no_motw")
endif()
