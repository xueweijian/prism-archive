# c21-preview: Quick Look CONTENT plan + providers (blueprint 9.1 / ADR-001).
# One card = one file; see suites/README.md.
#
# The pure suite is compiled twice more with a deliberate mutation each
# (CONTRACTS D3): a clip/encoding guard that stays green under its own mutant is
# not a guard. WILL_FAIL makes CTest green only while the mutation is caught.
add_executable(test_preview_plan test_preview_plan.cpp)
target_link_libraries(test_preview_plan PRIVATE prism_core)
add_test(NAME "core.test_preview_plan" COMMAND test_preview_plan)

foreach(mutant IN ITEMS KEEP_LONG_LINES UTF8_ALWAYS)
  string(TOLOWER "${mutant}" mutant_lc)
  add_executable("test_preview_plan_mutant_${mutant_lc}" test_preview_plan.cpp)
  target_link_libraries("test_preview_plan_mutant_${mutant_lc}" PRIVATE prism_core)
  target_compile_definitions("test_preview_plan_mutant_${mutant_lc}"
                             PRIVATE "PRISM_PREVIEW_MUTANT_${mutant}=1")
  add_test(NAME "mutation.preview_plan.${mutant_lc}"
           COMMAND "test_preview_plan_mutant_${mutant_lc}")
  set_tests_properties("mutation.preview_plan.${mutant_lc}" PROPERTIES WILL_FAIL TRUE)
endforeach()

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # Real 7z.dll: fixtures (UTF-8 text, GBK text, minimal PDF, WAV, binary blob)
  # are authored on disk, packed into ONE archive, and only ever read back
  # through the 7-Zip stream -- no temp extraction, no %TEMP% detour.
  add_executable(win_preview_providers win/test_preview_providers.cpp)
  target_link_libraries(win_preview_providers PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib ole32.lib oleaut32.lib windowscodecs.lib
                        mfplat.lib mfuuid.lib mfreadwrite.lib shlwapi.lib WindowsApp.lib)
  target_compile_options(win_preview_providers PRIVATE /WX-)
  add_test(NAME "win.preview_providers" COMMAND win_preview_providers)
  # The stdout marker is part of the verdict (CONTRACTS D1): a silent skip when
  # 7z.dll / Windows.Data.Pdf / Media Foundation is missing can never pass.
  # Exit 77 is the ONLY honoured skip, and it prints its reason.
  set_tests_properties(win.preview_providers PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL PREVIEW-PROVIDER CHECKS PASSED"
      SKIP_RETURN_CODE 77)
endif()
