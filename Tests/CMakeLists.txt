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
    test_archive_tree.cpp
    test_open_flow.cpp)

foreach(source IN LISTS PRISM_CORE_TESTS)
  get_filename_component(name "${source}" NAME_WE)
  add_executable("${name}" "${source}")
  target_link_libraries("${name}" PRIVATE prism_core)
  add_test(NAME "core.${name}" COMMAND "${name}")
endforeach()
# The probe core drives a thread-pool plan; link Threads explicitly.
find_package(Threads REQUIRED)
target_link_libraries(test_pwd_probe_core PRIVATE Threads::Threads)
# OpenFlow drives the same pool (cancellation + hit-stops-the-run cases).
target_link_libraries(test_open_flow PRIVATE Threads::Threads)

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
  # prism_shell_contract: m2_compile_check.cpp is a LINK contract for the sparse
  # registrar (it calls prisms' IsRegistered on purpose). The registrar became a
  # static library when its WinRT half moved out of the header (card C4), so the
  # contract now also proves the library links -- keep this on the same target.
  target_link_libraries(win_engine_bridge_test PRIVATE prism_core prism_win_bridge
                                                      prism_shell_contract)
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

  # Folder navigation against the REAL window class (batch A increment 5).
  # Double-click a directory -> list changes -> Backspace -> back with the
  # selection restored, driven through WndProc, not through the Navigator API
  # alone (the Navigator has its own Linux suite; this proves the wiring).
  add_executable(win_tree_nav win/test_navigation.cpp)
  target_link_libraries(win_tree_nav PRIVATE prism_core prism_win_bridge
                                              user32.lib gdi32.lib ole32.lib
                                              windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_tree_nav PRIVATE /WX-)
  add_test(NAME "win.tree_nav" COMMAND win_tree_nav)
  # Marker is a CONJUNCTION with the exit code: ctest needs exit 0 AND this
  # line, so a silent SKIP (no 7z.dll on the runner) or a crash after printing
  # still fails. This test was written but left UNREGISTERED on first landing
  # -- preflight D1 caught it, which is exactly what that gate is for.
  set_tests_properties(win.tree_nav PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL NAVIGATION CHECKS PASSED")
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
# ---------------------------------------------------------------------------
# M4 dnd-seek batch (APPENDED; no registration above was touched or reordered).
# The delayed-extract seek planner plus three of its own negative controls --
# each mutant must go red on the SAME suite, and WILL_FAIL makes ctest green
# only when it does.
# ---------------------------------------------------------------------------
add_executable(test_dnd_seek test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek PRIVATE prism_core)
add_test(NAME "core.test_dnd_seek" COMMAND test_dnd_seek)
# A hanging seek test must die loud, not block the lane forever; ctest prints
# the partial output on timeout so the stall is diagnosable from the log.
set_tests_properties(core.test_dnd_seek PROPERTIES TIMEOUT 240)

add_executable(test_dnd_seek_mutant_forward_only test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek_mutant_forward_only PRIVATE prism_core)
target_compile_definitions(test_dnd_seek_mutant_forward_only
                           PRIVATE PRISM_DND_MUTANT_SEEK_FORWARD_ONLY=1)
add_test(NAME "mutation.dnd_seek.forward_only"
         COMMAND test_dnd_seek_mutant_forward_only)
set_tests_properties(mutation.dnd_seek.forward_only PROPERTIES WILL_FAIL TRUE)

add_executable(test_dnd_seek_mutant_ignore_budget test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek_mutant_ignore_budget PRIVATE prism_core)
target_compile_definitions(test_dnd_seek_mutant_ignore_budget
                           PRIVATE PRISM_DND_MUTANT_SEEK_IGNORE_BUDGET=1)
add_test(NAME "mutation.dnd_seek.ignore_budget"
         COMMAND test_dnd_seek_mutant_ignore_budget)
set_tests_properties(mutation.dnd_seek.ignore_budget PROPERTIES WILL_FAIL TRUE)

add_executable(test_dnd_seek_mutant_lookback_off_by_one test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek_mutant_lookback_off_by_one PRIVATE prism_core)
target_compile_definitions(test_dnd_seek_mutant_lookback_off_by_one
                           PRIVATE PRISM_DND_MUTANT_SEEK_LOOKBACK_OFF_BY_ONE=1)
add_test(NAME "mutation.dnd_seek.lookback_off_by_one"
         COMMAND test_dnd_seek_mutant_lookback_off_by_one)
set_tests_properties(mutation.dnd_seek.lookback_off_by_one PROPERTIES WILL_FAIL TRUE)

# The ring must re-anchor after a forward skip. Without it, the retained window
# still claims its pre-skip start, so the next read indexes past the retained
# bytes -- PR8's Read() had exactly this hole (dropped bytes were counted in
# the frontier but never removed from the ring).
add_executable(test_dnd_seek_mutant_ring_keep_skipped test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek_mutant_ring_keep_skipped PRIVATE prism_core)
target_compile_definitions(test_dnd_seek_mutant_ring_keep_skipped
                           PRIVATE PRISM_DND_MUTANT_RING_KEEP_SKIPPED=1)
add_test(NAME "mutation.dnd_seek.ring_keep_skipped"
         COMMAND test_dnd_seek_mutant_ring_keep_skipped)
set_tests_properties(mutation.dnd_seek.ring_keep_skipped PROPERTIES WILL_FAIL TRUE)

# The ring may only serve what it holds. Serving from the wrong base is the
# "successful read of the wrong bytes" failure mode the old skeleton had.
add_executable(test_dnd_seek_mutant_serve_unresident test_dnd_seek.cpp)
target_link_libraries(test_dnd_seek_mutant_serve_unresident PRIVATE prism_core)
target_compile_definitions(test_dnd_seek_mutant_serve_unresident
                           PRIVATE PRISM_DND_MUTANT_SERVE_UNRESIDENT=1)
add_test(NAME "mutation.dnd_seek.serve_unresident"
         COMMAND test_dnd_seek_mutant_serve_unresident)
set_tests_properties(mutation.dnd_seek.serve_unresident PROPERTIES WILL_FAIL TRUE)

# Windows lane: the real 7z.dll served through a real CFSTR_FILECONTENTS
# IDataObject. The stdout marker is part of the verdict (contract D1).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_dnd_seek_test win/test_dnd_seek.cpp)
  target_link_libraries(win_dnd_seek_test PRIVATE prism_core prism_win_bridge
                                                   user32.lib gdi32.lib ole32.lib
                                                   oleaut32.lib windowscodecs.lib)
  target_compile_options(win_dnd_seek_test PRIVATE /WX-)
  add_test(NAME "win.dnd_seek" COMMAND win_dnd_seek_test)
  set_tests_properties(win.dnd_seek PROPERTIES TIMEOUT 600
      PASS_REGULAR_EXPRESSION "ALL DND-SEEK CHECKS PASSED")
endif()
# ---------------------------------------------------------------------------
# C4 shell double-stack (APPENDED; nothing above was reordered).
#   win.sparse_registrar          -- verb table / URI encoding / manifest <->
#                                   compiled CLSID / real shell IContextMenu
#                                   round-trip / sparse deploy-unregister
#   mutation.sparse_registrar.*   -- three negative controls (D3):
#       1. manifest without uap10:AllowExternalContent  -> manifest checks red
#       2. manifest with a CLSID that is not the compiled one -> red
#       3. Evaluate() that accepts an UNVERIFIED deploy -> verdict test red
#   Each mutant is WILL_FAIL, so ctest is green only when the suite catches it.
# ---------------------------------------------------------------------------
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  file(TO_CMAKE_PATH ${CMAKE_BINARY_DIR} PRISM_BUILD_ROOT_FWD)

  add_executable(win_sparse_registrar_test win/test_sparse_registrar.cpp)
  target_link_libraries(win_sparse_registrar_test PRIVATE prism_core prism_win_bridge
                        prism_shell_contract shlwapi.lib shell32.lib ole32.lib advapi32.lib)
  target_compile_options(win_sparse_registrar_test PRIVATE /WX-)
  target_compile_definitions(win_sparse_registrar_test PRIVATE
      PRISM_BUILD_ROOT="${PRISM_BUILD_ROOT_FWD}")
  add_dependencies(win_sparse_registrar_test PrismShellExtClassic)
  add_test(NAME "win.sparse_registrar" COMMAND win_sparse_registrar_test)
  set_tests_properties(win.sparse_registrar PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL SPARSE-REGISTRAR CHECKS PASSED")

  # --- negative control 1: the manifest must declare external content ---
  add_executable(win_sparse_registrar_mutant_manifest win/test_sparse_registrar.cpp)
  target_link_libraries(win_sparse_registrar_mutant_manifest PRIVATE prism_core prism_win_bridge
                        prism_shell_contract shlwapi.lib shell32.lib ole32.lib advapi32.lib)
  target_compile_options(win_sparse_registrar_mutant_manifest PRIVATE /WX-)
  target_compile_definitions(win_sparse_registrar_mutant_manifest PRIVATE PRISM_TEST_PURE_ONLY
      PRISM_MUTANT_MANIFEST_DROP_EXTERNAL_CONTENT PRISM_BUILD_ROOT="${PRISM_BUILD_ROOT_FWD}")
  add_test(NAME "mutation.sparse_registrar.manifest_external_content"
           COMMAND win_sparse_registrar_mutant_manifest)
  set_tests_properties(mutation.sparse_registrar.manifest_external_content
      PROPERTIES WILL_FAIL TRUE)

  # --- negative control 2: the manifest CLSID must be the compiled CLSID ---
  add_executable(win_sparse_registrar_mutant_clsid win/test_sparse_registrar.cpp)
  target_link_libraries(win_sparse_registrar_mutant_clsid PRIVATE prism_core prism_win_bridge
                        prism_shell_contract shlwapi.lib shell32.lib ole32.lib advapi32.lib)
  target_compile_options(win_sparse_registrar_mutant_clsid PRIVATE /WX-)
  target_compile_definitions(win_sparse_registrar_mutant_clsid PRIVATE PRISM_TEST_PURE_ONLY
      PRISM_MUTANT_MANIFEST_WRONG_CLSID PRISM_BUILD_ROOT="${PRISM_BUILD_ROOT_FWD}")
  add_test(NAME "mutation.sparse_registrar.manifest_clsid"
           COMMAND win_sparse_registrar_mutant_clsid)
  set_tests_properties(mutation.sparse_registrar.manifest_clsid PROPERTIES WILL_FAIL TRUE)

  # --- negative control 3: a deploy that leaves nothing registered is NOT ok ---
  add_library(prism_shell_contract_mutant_unverified STATIC
              ../Src/PrismWin/SparsePackageRegistrar.cpp)
  target_include_directories(prism_shell_contract_mutant_unverified PUBLIC
                             ../Src/PrismWin)
  target_link_libraries(prism_shell_contract_mutant_unverified PUBLIC WindowsApp.lib
                        shlwapi.lib advapi32.lib shell32.lib)
  target_compile_definitions(prism_shell_contract_mutant_unverified PRIVATE
                             PRISM_MUTANT_ACCEPT_UNVERIFIED=1)
  target_compile_options(prism_shell_contract_mutant_unverified PRIVATE /WX-)
  add_executable(win_sparse_registrar_mutant_unverified win/test_sparse_registrar.cpp)
  target_link_libraries(win_sparse_registrar_mutant_unverified PRIVATE prism_core prism_win_bridge
                        prism_shell_contract_mutant_unverified shlwapi.lib shell32.lib ole32.lib advapi32.lib)
  target_compile_options(win_sparse_registrar_mutant_unverified PRIVATE /WX-)
  target_compile_definitions(win_sparse_registrar_mutant_unverified PRIVATE PRISM_TEST_PURE_ONLY
      PRISM_BUILD_ROOT="${PRISM_BUILD_ROOT_FWD}")
  add_test(NAME "mutation.sparse_registrar.accept_unverified"
           COMMAND win_sparse_registrar_mutant_unverified)
  set_tests_properties(mutation.sparse_registrar.accept_unverified PROPERTIES WILL_FAIL TRUE)
endif()
