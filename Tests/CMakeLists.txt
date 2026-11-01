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
# C8 interaction batch (APPENDED; no registration above was touched or
# reordered -- the same rule the M4 dnd-seek block followed).
#
# Blueprint ch. 9.1: the key table, the palette ranker, the navigation-history
# stack and the Ctrl+Shift+N decision are PrismCore modules, so they run on
# Linux in milliseconds instead of burning Windows CI rounds. Each guard has a
# mutation build that MUST go red on the SAME suite (CONTRACTS D3): the mutant
# is a compile definition, so injection cannot silently fail to apply (D3's
# own lesson: a text replacement that does not match looks like a green).
# ---------------------------------------------------------------------------
add_executable(test_shortcuts test_shortcuts.cpp)
target_link_libraries(test_shortcuts PRIVATE prism_core)
add_test(NAME "core.test_shortcuts" COMMAND test_shortcuts)

add_executable(test_command_palette test_command_palette.cpp)
target_link_libraries(test_command_palette PRIVATE prism_core)
add_test(NAME "core.test_command_palette" COMMAND test_command_palette)

add_executable(test_history test_history.cpp)
target_link_libraries(test_history PRIVATE prism_core)
add_test(NAME "core.test_history" COMMAND test_history)

# The zoom ladder must clamp to the blueprint's 64..512 px window.
add_executable(test_shortcuts_mutant_no_clamp test_shortcuts.cpp)
target_link_libraries(test_shortcuts_mutant_no_clamp PRIVATE prism_core)
target_compile_definitions(test_shortcuts_mutant_no_clamp PRIVATE PRISM_ZOOM_MUTANT_NO_CLAMP=1)
add_test(NAME "mutation.zoom.no_clamp" COMMAND test_shortcuts_mutant_no_clamp)
set_tests_properties(mutation.zoom.no_clamp PROPERTIES WILL_FAIL TRUE)

# Ctrl+Shift+N must refuse names Windows would reinterpret.
add_executable(test_shortcuts_mutant_allow_reserved test_shortcuts.cpp)
target_link_libraries(test_shortcuts_mutant_allow_reserved PRIVATE prism_core)
target_compile_definitions(test_shortcuts_mutant_allow_reserved PRIVATE PRISM_NEWFOLDER_MUTANT_ALLOW_RESERVED=1)
add_test(NAME "mutation.mkdir.allow_reserved" COMMAND test_shortcuts_mutant_allow_reserved)
set_tests_properties(mutation.mkdir.allow_reserved PROPERTIES WILL_FAIL TRUE)

# The palette's word-boundary bonus is what makes it prefer filenames.
add_executable(test_command_palette_mutant_no_boundary test_command_palette.cpp)
target_link_libraries(test_command_palette_mutant_no_boundary PRIVATE prism_core)
target_compile_definitions(test_command_palette_mutant_no_boundary PRIVATE PRISM_PALETTE_MUTANT_NO_BOUNDARY=1)
add_test(NAME "mutation.palette.no_boundary" COMMAND test_command_palette_mutant_no_boundary)
set_tests_properties(mutation.palette.no_boundary PROPERTIES WILL_FAIL TRUE)

# Shorter-title tie-break: a reversed comparator must reorder the results.
add_executable(test_command_palette_mutant_longer_first test_command_palette.cpp)
target_link_libraries(test_command_palette_mutant_longer_first PRIVATE prism_core)
target_compile_definitions(test_command_palette_mutant_longer_first PRIVATE PRISM_PALETTE_MUTANT_LONGER_FIRST=1)
add_test(NAME "mutation.palette.longer_first" COMMAND test_command_palette_mutant_longer_first)
set_tests_properties(mutation.palette.longer_first PROPERTIES WILL_FAIL TRUE)

# History capacity is a promise (CONTRACTS D17): keeping one extra entry is a
# real bug class, so the off-by-one is a permanent mutant.
add_executable(test_history_mutant_off_by_one test_history.cpp)
target_link_libraries(test_history_mutant_off_by_one PRIVATE prism_core)
target_compile_definitions(test_history_mutant_off_by_one PRIVATE PRISM_HISTORY_MUTANT_OFF_BY_ONE=1)
add_test(NAME "mutation.history.off_by_one" COMMAND test_history_mutant_off_by_one)
set_tests_properties(mutation.history.off_by_one PROPERTIES WILL_FAIL TRUE)

# Adjacent de-duplication: without it Back appears broken on a repeated crumb.
add_executable(test_history_mutant_no_dedupe test_history.cpp)
target_link_libraries(test_history_mutant_no_dedupe PRIVATE prism_core)
target_compile_definitions(test_history_mutant_no_dedupe PRIVATE PRISM_HISTORY_MUTANT_NO_DEDUPE=1)
add_test(NAME "mutation.history.no_dedupe" COMMAND test_history_mutant_no_dedupe)
set_tests_properties(mutation.history.no_dedupe PROPERTIES WILL_FAIL TRUE)

# Windows lane: the key table against the REAL VK_* values and the real
# WndProc, plus the end-to-end journeys (real HWND + posted messages).
# Both markers are a CONJUNCTION with the exit code (D1): a silent SKIP, a
# crash after printing, or a missing window station all fail.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_keys win/test_keys.cpp)
  target_link_libraries(win_keys PRIVATE prism_core prism_win_bridge
                                              user32.lib gdi32.lib ole32.lib
                                              windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_keys PRIVATE /WX-)
  add_test(NAME "win.keys" COMMAND win_keys)
  set_tests_properties(win.keys PROPERTIES TIMEOUT 180
      PASS_REGULAR_EXPRESSION "ALL KEYS CHECKS PASSED")

  add_executable(win_ui_journey win/test_ui_journey.cpp)
  target_link_libraries(win_ui_journey PRIVATE prism_core prism_win_bridge
                                                   user32.lib gdi32.lib ole32.lib
                                                   windowscodecs.lib d2d1.lib dwrite.lib)
  target_compile_options(win_ui_journey PRIVATE /WX-)
  add_test(NAME "win.ui_journey" COMMAND win_ui_journey)
  set_tests_properties(win.ui_journey PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL UI-JOURNEY CHECKS PASSED")
endif()

