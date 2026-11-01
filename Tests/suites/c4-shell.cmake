# c4: shell double-stack (Win10 classic IContextMenu + Win11 MSIX sparse).
#
# Owned by card C4. See Tests/suites/README.md; registration lives here instead
# of the shared Tests/CMakeLists.txt tail.
#
# NOTE: this suite depends on ONE line in Tests/CMakeLists.txt that cannot move
# here -- win_engine_bridge_test had to link prism_shell_contract, because
# win/m2_compile_check.cpp is a link contract that calls sparse::IsRegistered
# and the registrar's WinRT half moved out of the header (C4).

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  file(TO_CMAKE_PATH "${CMAKE_BINARY_DIR}" PRISM_BUILD_ROOT_FWD)

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

  # --- negative control 1: the manifest must declare external content (D3) ---
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
