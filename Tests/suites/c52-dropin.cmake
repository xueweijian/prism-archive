# c52-dropin: dropping files INTO the window (Explorer -> PrismArchive).
#
# The model (Src/PrismCore/DropPlan.hpp) answers ONE question as pure data:
# given the dragged tree, whether an archive is open and whether it is read-only,
# what does the drop MEAN? The window (Src/PrismWin/GalleryWindow.hpp) only
# converts HDROP -> tree, paints the hint and forwards the plan.
#
# NEGATIVE CONTROLS (D3/D26/D27). Two mutants compile the SAME core source with
# one rule switched off, and each is registered with its own "MUTATION CAUGHT"
# marker -- no WILL_FAIL anywhere in this file, because D26 forbids stacking it
# on a pass marker. The clean binary (core.drop_plan) is the paired control; the
# Windows mutant below compiles win/test_drop_in.cpp with the model mutant so the
# window path (HDROP -> folder walk -> plan) proves the recursion too.
#
# Arm names avoid clean/unarmed/control (D28: those words mark a CONTROL entry in
# the mutation matrix).
add_executable(core_drop_plan_test test_drop_plan.cpp)
target_link_libraries(core_drop_plan_test PRIVATE prism_core)
add_test(NAME "core.drop_plan" COMMAND core_drop_plan_test)
set_tests_properties(core.drop_plan PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL DROP-PLAN CHECKS PASSED")

# D3 control 1: a read-only archive accepts content ("just write anyway").
add_executable(core_drop_plan_mutant_readonly test_drop_plan.cpp)
target_link_libraries(core_drop_plan_mutant_readonly PRIVATE prism_core)
target_compile_definitions(core_drop_plan_mutant_readonly
                           PRIVATE PRISM_C52_MUTANT_READONLY_ALLOWS_ADD=1)
add_test(NAME "mutation.drop_plan.readonly_allows_add"
         COMMAND core_drop_plan_mutant_readonly)
set_tests_properties(mutation.drop_plan.readonly_allows_add PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C52 MUTATION CAUGHT: readonly_allows_add")

# D3 control 2: a dropped folder is handed over as ONE opaque member.
add_executable(core_drop_plan_mutant_flat test_drop_plan.cpp)
target_link_libraries(core_drop_plan_mutant_flat PRIVATE prism_core)
target_compile_definitions(core_drop_plan_mutant_flat
                           PRIVATE PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION=1)
add_test(NAME "mutation.drop_plan.no_recursive_expansion"
         COMMAND core_drop_plan_mutant_flat)
set_tests_properties(mutation.drop_plan.no_recursive_expansion PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C52 MUTATION CAUGHT: no_recursive_expansion")
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # The real HWND: WS_EX_ACCEPTFILES, a hand-built HDROP, the disk walk and a
  # real WM_PAINT that draws the overlay (see the file header).
  add_executable(win_drop_in_test win/test_drop_in.cpp)
  # d2d1/dwrite: GalleryWindow.hpp owns a Direct2D render target + DirectWrite
  # factory, so the overlay's paint path is linked in this suite for real.
  target_link_libraries(win_drop_in_test PRIVATE prism_core prism_win_bridge
                        user32.lib gdi32.lib shell32.lib ole32.lib
                        d2d1.lib dwrite.lib)
  target_compile_options(win_drop_in_test PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "win.drop_in" COMMAND win_drop_in_test)
  set_tests_properties(win.drop_in PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL DROP-IN CHECKS PASSED")

  # D3 control on the WINDOW path: the model mutation is compiled into the suite
  # that walks a real folder, so "the walk found the files but the plan has one
  # member" is caught where the user would feel it.
  add_executable(win_drop_in_mutant_folder_flat win/test_drop_in.cpp)
  target_link_libraries(win_drop_in_mutant_folder_flat PRIVATE prism_core
                        prism_win_bridge user32.lib gdi32.lib shell32.lib ole32.lib
                        d2d1.lib dwrite.lib)
  target_compile_options(win_drop_in_mutant_folder_flat PRIVATE /WX-)
  target_compile_definitions(win_drop_in_mutant_folder_flat
                             PRIVATE PRISM_C52_MUTANT_NO_RECURSIVE_EXPANSION=1)
  add_test(NAME "mutation.drop_in.folder_flat"
           COMMAND win_drop_in_mutant_folder_flat)
  set_tests_properties(mutation.drop_in.folder_flat PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C52 MUTATION CAUGHT: folder_flat")
endif()