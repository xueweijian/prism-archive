# c24-release: one command -> a distributable, verifiable artifact (card C24).
#
# Owned by card C24.  See Tests/suites/README.md; registration lives here
# instead of the shared Tests/CMakeLists.txt tail.
#
# What is proven here, and what would make each of these a lie:
#
#   win.release_package   the real pipeline: cmake --install -> stage ->
#                         prism-package.json -> SHA256SUMS.txt -> zip -> RE-OPEN
#                         the archive and re-hash every entry.  The expected
#                         content is derived from the install tree on disk, so a
#                         copy step that forgets a file is red instead of
#                         producing a thinner package (CONTRACTS D3).
#   win.release_verify    a second, independent entry point (Test-ReleasePackage)
#                         checks the produced archive the way a downloading user
#                         would: self-description + sidecar archive hash.
#   mutation.release.*    one defect each, injected at a different stage, and the
#                         injection itself is asserted to have happened -- a
#                         mutation that matched nothing would otherwise be
#                         reported as "the guard is sharp".
#   win.release_sign      Authenticode applies when a certificate exists and
#                         SKIPs with a reason when it does not (CONTRACTS D1:
#                         exit 77 + SKIP_RETURN_CODE, never a silent green).
#
# RESOURCE_LOCK: several tests run "cmake --install" against the same build
# tree, which writes install_manifest.txt there.  Under "ctest -j" that is a
# shared write, so the packaging tests take one lock.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  find_program(PRISM_PWSH NAMES pwsh)
  if(NOT PRISM_PWSH)
    message(STATUS "c24-release: pwsh not found -- release pipeline suite not registered")
  else()
    set(PRISM_RELEASE_TOOLS "${CMAKE_SOURCE_DIR}/tools/release")
    set(PRISM_RELEASE_OUT "${CMAKE_BINARY_DIR}/release")

    add_test(NAME "win.release_package"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_RELEASE_TOOLS}/New-PortablePackage.ps1"
                     -BuildDir "${CMAKE_BINARY_DIR}"
                     -Config "$<CONFIG>"
                     -OutDir "${PRISM_RELEASE_OUT}")
    set_tests_properties(win.release_package PROPERTIES
        TIMEOUT 600
        PASS_REGULAR_EXPRESSION "RELEASE PACKAGE: PASS"
        RESOURCE_LOCK "prism-release-install"
        FIXTURES_SETUP "prism_release_package"
        LABELS "release;distribution")

    add_test(NAME "win.release_verify"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_RELEASE_TOOLS}/Test-ReleasePackage.ps1"
                     -ZipPath "${PRISM_RELEASE_OUT}")
    set_tests_properties(win.release_verify PROPERTIES
        TIMEOUT 300
        PASS_REGULAR_EXPRESSION "VERIFY: PASS"
        FIXTURES_REQUIRED "prism_release_package"
        LABELS "release;distribution")

    # --- negative controls (CONTRACTS D3) ---------------------------------
    # Each one exits 0 ONLY when the package verification went red, and exits 1
    # when the injection never happened ("NOT INJECTED") -- that second failure
    # mode is the one that makes mutation testing self-deceiving.
    foreach(mutant IN ITEMS DropShellExtClassic Extra7zDll SumGap)
      string(TOLOWER "${mutant}" mutant_lc)
      add_test(NAME "mutation.release.${mutant_lc}"
               COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                       -File "${PRISM_RELEASE_TOOLS}/New-PortablePackage.ps1"
                       -BuildDir "${CMAKE_BINARY_DIR}"
                       -Config "$<CONFIG>"
                       -Mutate "${mutant}")
      set_tests_properties("mutation.release.${mutant_lc}" PROPERTIES
          TIMEOUT 600
          PASS_REGULAR_EXPRESSION "MUTATION ${mutant}: CAUGHT"
          RESOURCE_LOCK "prism-release-install"
          LABELS "release;mutation")
    endforeach()

    # Signing needs a certificate this repository must never contain.  Without
    # one the step exits 77 and ctest reports "Skipped" -- the run page then
    # shows WHY, instead of a green tick over an unsigned artifact.
    add_test(NAME "win.release_sign"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_RELEASE_TOOLS}/Sign-ReleaseArtifacts.ps1"
                     -Path "${CMAKE_BINARY_DIR}/Src/PrismWin")
    set_tests_properties(win.release_sign PROPERTIES
        TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "SIGN: (PASS|SKIP)"
        LABELS "release;signing")
  endif()
endif()
