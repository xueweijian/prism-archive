# c87-hud-lifecycle: progress HUD state machine + Quick Look's real pixels.
#
# Core arm (every platform): OnFinish(Success/Failed/Cancelled) dismisses on the
# NEXT Render, OnStart resets the timeline, Reset() forgets the operation, and
# Role::ProgressBar has a stable a11y spelling.
#
# Windows arm: drives a REAL GalleryWindow with injected progress (no sleeps) and
# asserts, through the shipped paths:
#   * MSAA carries ROLE_SYSTEM_PROGRESSBAR with the operation name and percent
#     while the HUD runs;
#   * after success / failure / cancel two consecutive rendered frames have ZERO
#     HUD pixels in the capsule rectangle and no progress element exists;
#   * a second operation starts below 10 %; a tab / archive switch resets;
#   * Quick Look paints the DECODED thumbnail (variance + quadrant colours of the
#     contain-fit rectangle), not the gradient stand-in.
#
# Three compile-time negative controls (CONTRACTS D3/D26/D27; the CAUGHT marker
# prints at the first failure, PASS_REGULAR_EXPRESSION never WILL_FAIL):
#   hud_no_dismiss        finish leaves the capsule -> residual assertion red
#   no_progress_a11y      painted but unpublished  -> a11y assertion red
#   preview_placeholder   gradient instead of image -> preview assertion red
add_executable(c87_hud_core test_c87_hud_core.cpp)
target_link_libraries(c87_hud_core PRIVATE prism_core)
add_test(NAME "core.c87_hud" COMMAND c87_hud_core)
set_tests_properties(core.c87_hud PROPERTIES TIMEOUT 120)

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c87_hud_lifecycle win/test_c87_hud_lifecycle.cpp)
  target_link_libraries(win_c87_hud_lifecycle PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                        ole32.lib oleacc.lib oleaut32.lib d3d11.lib dxgi.lib dcomp.lib
                        WindowsApp.lib)
  target_compile_options(win_c87_hud_lifecycle PRIVATE /WX-)
  add_test(NAME "win.c87_hud_lifecycle" COMMAND win_c87_hud_lifecycle)
  set_tests_properties(win.c87_hud_lifecycle PROPERTIES TIMEOUT 300
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C87 HUD LIFECYCLE CHECKS PASSED")

  set(C87_MUTANT_ARMS hud_no_dismiss no_progress_a11y preview_placeholder)
  foreach(arm IN LISTS C87_MUTANT_ARMS)
    string(TOUPPER "${arm}" C87_ARM_UPPER)
    add_executable(win_c87_${arm} win/test_c87_hud_lifecycle.cpp)
    target_link_libraries(win_c87_${arm} PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                          ole32.lib oleacc.lib oleaut32.lib d3d11.lib dxgi.lib dcomp.lib
                          WindowsApp.lib)
    target_compile_options(win_c87_${arm} PRIVATE /WX-)
    target_compile_definitions(win_c87_${arm}
                               PRIVATE PRISM_C87_MUTANT_${C87_ARM_UPPER}=1)
    add_test(NAME "mutation.c87.${arm}" COMMAND win_c87_${arm})
    set_tests_properties(mutation.c87.${arm} PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C87 MUTATION CAUGHT: ${arm}")
  endforeach()
endif()
