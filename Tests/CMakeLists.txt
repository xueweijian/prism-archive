# PrismCore unit tests: one executable per suite, registered with CTest.
set(PRISM_CORE_TESTS
    test_path_safety.cpp
    test_smart_extractor.cpp
    test_charset_repair.cpp
    test_virtual_tree.cpp
    test_security_guard.cpp
    test_collision_resolver.cpp
    test_preview.cpp
    test_context_menu.cpp
    test_progress_hud.cpp
    test_spring.cpp
    test_gallery.cpp
    test_sweep.cpp
    test_thumb_store.cpp
    test_list_layout.cpp
    test_glass_island.cpp
    test_drag_drop.cpp
    test_pwd_probe_core.cpp
    test_windows_name.cpp
    bench_sweep.cpp
    test_archive_tree.cpp)

foreach(source IN LISTS PRISM_CORE_TESTS)
  get_filename_component(name "${source}" NAME_WE)
  add_executable("${name}" "${source}")
  target_link_libraries("${name}" PRIVATE prism_core)
  add_test(NAME "core.${name}" COMMAND "${name}")
endforeach()
# The probe core drives a thread-pool plan; link Threads explicitly.
find_package(Threads REQUIRED)
target_link_libraries(test_pwd_probe_core PRIVATE Threads::Threads)

# Deliberate rule mutations are expected to fail the same assertions. WILL_FAIL
# makes the mutation check itself green only when the test suite catches it;
# this guards the trust and free-space checks against vacuous/fake-green tests.
foreach(mutant IN ITEMS trust_all ignore_space)
  add_executable("test_drag_drop_mutant_${mutant}" test_drag_drop.cpp)
  target_link_libraries("test_drag_drop_mutant_${mutant}" PRIVATE prism_core)
  if(mutant STREQUAL "trust_all")
    target_compile_definitions("test_drag_drop_mutant_${mutant}" PRIVATE PRISM_DND_MUTANT_TRUST_ALL=1)
  else()
    target_compile_definitions("test_drag_drop_mutant_${mutant}" PRIVATE PRISM_DND_MUTANT_IGNORE_SPACE=1)
  endif()
  add_test(NAME "mutation.drag_drop.${mutant}" COMMAND "test_drag_drop_mutant_${mutant}")
  set_tests_properties("mutation.drag_drop.${mutant}" PROPERTIES WILL_FAIL TRUE)
endforeach()

# Windows-only integration test: loads the real 7z.dll and round-trips an
# archive created by the 7-Zip CLI. Skipped (not built) on other platforms.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_engine_bridge_test win/test_engine_bridge.cpp win/m2_compile_check.cpp win/m3_compile_check.cpp)
  # WindowsApp.lib provides the C++/WinRT activation-factory entry points
  target_link_libraries(win_engine_bridge_test PRIVATE user32.lib gdi32.lib d2d1.lib
                                                      dwrite.lib d3d11.lib dxgi.lib
                                                      dcomp.lib WindowsApp.lib)
  target_compile_options(win_engine_bridge_test PRIVATE /WX-)
  target_link_libraries(win_engine_bridge_test PRIVATE prism_core prism_win_bridge)
  add_test(NAME "win.engine_bridge" COMMAND win_engine_bridge_test)
  # The stdout marker is part of the verdict: a silent SKIP can never count
  # as a pass (L2 negative control for false greens).
  set_tests_properties(win.engine_bridge PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL ENGINE-BRIDGE CHECKS PASSED")

  # COM surface contract (B8). Cheap, no 7z.dll: proves a class that inherits
  # IOutStream actually ANSWERS IID_IOutStream through QueryInterface -- the
  # exact defect that burned 18 CI rounds on 2026-10-31 (7-Zip QIs, never
  # static_casts, so a complete vtable can still be invisible to the engine).
  add_executable(win_com_qi_contract_test test_com_qi_contract.cpp)
  target_link_libraries(win_com_qi_contract_test PRIVATE prism_win_bridge)
  add_test(NAME "win.com_qi_contract" COMMAND win_com_qi_contract_test)
  set_tests_properties(win.com_qi_contract PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL COM-QI CONTRACT CHECKS PASSED")

  # Headless design snapshot: renders the gallery scene to PNG artifacts so
  # the Liquid Glass design can be reviewed from the CI run itself.
  add_executable(win_solid_sweep_test win/test_solid_sweep.cpp)
  target_link_libraries(win_solid_sweep_test PRIVATE prism_core prism_win_bridge
                                                     user32.lib gdi32.lib ole32.lib
                                                     windowscodecs.lib)
  target_compile_options(win_solid_sweep_test PRIVATE /WX-)
  add_test(NAME "win.solid_sweep" COMMAND win_solid_sweep_test)
  set_tests_properties(win.solid_sweep PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL SOLID-SWEEP CHECKS PASSED")

  # Performance benchmark (blueprint ch. 7.2): real 7z.dll, real solid archive,
  # one sweep pass vs per-item extraction. Missing 7z.exe is a FAIL, not a
  # skip -- the BENCH JSON line in stdout feeds the CI step summary.
  add_executable(win_bench_solid win/bench_solid.cpp)
  target_link_libraries(win_bench_solid PRIVATE prism_core prism_win_bridge
                                                user32.lib gdi32.lib ole32.lib
                                                windowscodecs.lib psapi.lib)
  target_compile_options(win_bench_solid PRIVATE /WX-)
  add_test(NAME "win.bench_solid" COMMAND win_bench_solid)
  set_tests_properties(win.bench_solid PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL BENCH CHECKS PASSED")

  add_executable(gallery_snapshot win/gallery_snapshot.cpp)
  target_link_libraries(gallery_snapshot PRIVATE prism_win_bridge user32.lib gdi32.lib
                                                d2d1.lib dwrite.lib windowscodecs.lib
                                                ole32.lib WindowsApp.lib)
  target_compile_options(gallery_snapshot PRIVATE /WX-)
  add_test(NAME "win.gallery_snapshot" COMMAND gallery_snapshot
           $ENV{TEMP}/prism_snapshots)
  set_tests_properties(win.gallery_snapshot PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL SNAPSHOT CHECKS PASSED")
  add_executable(win_vtree_exec_test win/test_vtree_exec.cpp)
  target_link_libraries(win_vtree_exec_test PRIVATE prism_core prism_win_bridge
                                                     user32.lib gdi32.lib ole32.lib
                                                     windowscodecs.lib)
  target_compile_options(win_vtree_exec_test PRIVATE /WX-)
  add_test(NAME "win.vtree_exec" COMMAND win_vtree_exec_test)
  set_tests_properties(win.vtree_exec PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL VTREE-EXEC CHECKS PASSED")

  # mkdir variant MATRIX (batch B1 evidence): eight parameterized "commit a
  # new empty directory" cells against the real 7z.dll in ONE run, each with
  # its own fixture + UpdateItems commit + re-open readback. Verified by an
  # explicit verdict line: REPORT-ONLY ("always exit 1 so the table prints")
  # made green and red indistinguishable -- exactly the D1 trap. The test now
  # exits 0 only when no variant failed beyond the deliberate bare control.
  add_executable(win_mkdir_matrix win/test_mkdir_matrix.cpp)
  target_link_libraries(win_mkdir_matrix PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib ole32.lib
                                                 windowscodecs.lib)
  target_compile_options(win_mkdir_matrix PRIVATE /WX-)
  add_test(NAME "win.mkdir_matrix" COMMAND win_mkdir_matrix)
  # The PASS regex is a CONJUNCTION with the exit code, not a replacement for
  # it: ctest requires exit 0 AND this marker. That is what makes a silent skip
  # (or a crash after printing) still fail while a real success is provable.
  set_tests_properties(win.mkdir_matrix PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "MATRIX VERDICT: OK")
endif()

# The MSIX sparse-package manifest is install-time-critical: parse it on every
# push (Linux lane) so a malformed manifest can never reach a user machine.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME manifest.check
           COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_manifest.py
                   ${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Package/AppxManifest.xml)
endif()

if(WIN32)
  # Password probe (B3): format-aware silent-attempt budget driving the ring.
  add_executable(win_pwd_probe_test win/test_pwd_probe.cpp)
  target_link_libraries(win_pwd_probe_test PRIVATE prism_core prism_win_bridge
                                                   user32.lib gdi32.lib ole32.lib
                                                   windowscodecs.lib)
  target_compile_options(win_pwd_probe_test PRIVATE /WX-)
  add_test(NAME "win.pwd_probe" COMMAND win_pwd_probe_test)
  set_tests_properties(win.pwd_probe PROPERTIES
      PASS_REGULAR_EXPRESSION "ALL PWD-PROBE CHECKS PASSED")

  # Virtual-tree executor (B1): registered ONLY on its feature branch. Main's
  # VirtualTree.hpp still carries the move-semantics UB the branch fixed
  # (SEGFAULT on MSVC, 2026-10-31) -- re-register when B1 lands as a unit.
endif()
