# c120-dpi: THE PROCESS IS PER-MONITOR V2 (card C120).
#
# One line of manifest, one call at startup, and one live WM_DPICHANGED branch:
# that is the whole card, and each of the three is invisible in the other two--
# which is why the suite is split into processes.
#
#   win.c120_dpi            the manifest-MERGED probe: the loader's own verdict,
#                           read back with GetProcessDpiAwareness (D1), plus the
#                           PerMonitorV2 bytes inside BOTH this probe and the
#                           SHIPPED PrismArchive.exe (D2).
#   win.c120_dpi_runtime    the SAME source with NO manifest: the runtime ladder
#                           alone must still reach per-monitor, so the fallback
#                           is a measured claim and not a comment.
#   win.c120_dipchange      the real window: WM_DPICHANGED applies lParam's rect
#                           through SetWindowPos and reflows the layout in DIPs
#                           (D3). Synthetic message: a CI runner has one 96 DPI
#                           monitor, and GetDpiForWindow would answer 96 there
#                           whether the process is aware or not.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27). No WILL_FAIL anywhere:
#   unaware_runtime_fallback  the runtime rung lowered to SetProcessDPIAware()
#                             (system DPI) in a build with no manifest ->
#                             D1 must go red.
#   dead_branch               WM_DPICHANGED returns immediately (the pre-card
#                             state) -> D3 must go red.
# Both print their CAUGHT marker at the FIRST red assertion and leave 0; the
# CTest entry's PASS_REGULAR_EXPRESSION is the verdict.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The MANIFEST under test. Same file, same linker switch as the product
  # (Src/PrismWin/CMakeLists.txt): a probe that embedded its own copy would
  # prove nothing about the exe that ships.
  set(PRISM_C120_MANIFEST ${CMAKE_SOURCE_DIR}/Src/App/PrismArchive.manifest)
  if(NOT EXISTS ${PRISM_C120_MANIFEST})
    message(FATAL_ERROR "c120-dpi: ${PRISM_C120_MANIFEST} is missing; the card's
                        declaration cannot be verified")
  endif()

  # user32/gdi32: SystemDpi()'s GetDC/GetDeviceCaps fallback.
  add_executable(c120_dpi_probe win/test_c120_dpi.cpp)
  target_link_libraries(c120_dpi_probe PRIVATE prism_core user32.lib gdi32.lib)
  target_compile_options(c120_dpi_probe PRIVATE /WX-)
  target_compile_definitions(c120_dpi_probe PRIVATE PRISM_C120_EXPECT_MANIFEST=1
                             PRISM_C120_EXPECT_APP_EXE=1)
  target_link_options(c120_dpi_probe PRIVATE "/MANIFESTINPUT:${PRISM_C120_MANIFEST}")
  add_dependencies(c120_dpi_probe PrismArchive)
  # The shipped exe as an ARGUMENT, not a -D literal: a path baked into the
  # command line crosses CMake, Ninja and the shell, and one surviving backslash
  # is a red build for a reason nobody can see in the diff.
  add_test(NAME "win.c120_dpi" COMMAND c120_dpi_probe $<TARGET_FILE:PrismArchive>)
  set_tests_properties(win.c120_dpi PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C120 DPI CHECKS PASSED")

  # The runtime ladder ALONE: no /MANIFESTINPUT, so SetProcessDpiAwarenessContext
  # is the only thing that can make the process per-monitor.
  add_executable(c120_dpi_probe_runtime win/test_c120_dpi.cpp)
  target_link_libraries(c120_dpi_probe_runtime PRIVATE prism_core user32.lib gdi32.lib)
  target_compile_options(c120_dpi_probe_runtime PRIVATE /WX-)
  add_test(NAME "win.c120_dpi_runtime" COMMAND c120_dpi_probe_runtime)
  set_tests_properties(win.c120_dpi_runtime PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C120 DPI CHECKS PASSED")

  # MUTATION: system-DPI only, in a build with no manifest. D1 must go red.
  add_executable(c120_dpi_probe_mutant_unaware win/test_c120_dpi.cpp)
  target_link_libraries(c120_dpi_probe_mutant_unaware PRIVATE prism_core user32.lib gdi32.lib)
  target_compile_options(c120_dpi_probe_mutant_unaware PRIVATE /WX-)
  target_compile_definitions(c120_dpi_probe_mutant_unaware
                             PRIVATE PRISM_C120_MUTANT_UNAWARE=1)
  add_test(NAME "mutation.c120.unaware_runtime_fallback"
           COMMAND c120_dpi_probe_mutant_unaware)
  set_tests_properties(mutation.c120.unaware_runtime_fallback PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C120 MUTATION CAUGHT: unaware_runtime_fallback")

  # D3: the real window and the real renderer, one synthetic monitor move.
  add_executable(c120_dipchange win/test_c120_dipchange.cpp)
  target_link_libraries(c120_dipchange PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  target_compile_options(c120_dipchange PRIVATE /WX-)
  add_test(NAME "win.c120_dipchange" COMMAND c120_dipchange)
  set_tests_properties(win.c120_dipchange PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL C120 DPICHANGED CHECKS PASSED")

  # MUTATION: the pre-card dead branch. D3 must go red.
  add_executable(c120_dipchange_mutant_dead_branch win/test_c120_dipchange.cpp)
  target_link_libraries(c120_dipchange_mutant_dead_branch PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib d2d1.lib dwrite.lib
                        windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib shell32.lib)
  target_compile_options(c120_dipchange_mutant_dead_branch PRIVATE /WX-)
  target_compile_definitions(c120_dipchange_mutant_dead_branch
                             PRIVATE PRISM_C120_MUTANT_NO_DPICHANGED=1)
  add_test(NAME "mutation.c120.dead_branch" COMMAND c120_dipchange_mutant_dead_branch)
  set_tests_properties(mutation.c120.dead_branch PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "C120 DPICHANGED MUTATION CAUGHT: dead_branch")
endif()
