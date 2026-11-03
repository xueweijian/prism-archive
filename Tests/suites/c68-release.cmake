# c68-release: the zip a user actually installs (card C68).
#
# One card = one file; see suites/README.md.  C24 proved the release engineer
# can verify bytes; this suite proves the USER can install: unzip, INSTALL.cmd,
# right-click menu, extract, UNINSTALL.cmd.
#
# What is proven here, and what would make each of these a lie:
#
#   win.c68_package        tools/package.ps1 cuts PrismArchive-<version>-win-x64.zip
#                          from the "cmake --install" layout (CONTRACTS D3), reads
#                          the version from Src/App/PrismArchive.rc VERSIONINFO and
#                          cross-checks it against the built exe, ships 7z.dll and
#                          the two .cmd entry points, re-opens the zip and re-hashes
#                          every entry, and -AssertIdempotent builds it twice and
#                          requires the identical manifest content hash.  The
#                          success marker is a CONJUNCTION: "PACKAGE: PASS" and
#                          "idempotent=YES" on the same line, plus exit 0.
#   win.c68_verify         tools/verify-package.ps1 unzips into a unique %TEMP%
#                          directory, runs INSTALL.cmd -Quiet into a unique
#                          install folder + scratch registry scope, asserts the
#                          payload bytes, a settings.json write, both right-click
#                          verb keys and their exact command lines, a real
#                          --compress-7z/--extract-here round trip, then
#                          UNINSTALL.cmd and asserts every key/file is gone.
#                          The pre-existing real menu is snapshotted and restored.
#   mutation.c68.*         two negative controls (CONTRACTS D3): one deletes a
#                          DLL from the extracted package, one deletes a verb key
#                          after install.  Each injection is ASSERTED to have
#                          happened and the verdict is printed at the first red
#                          assertion (CONTRACTS D27); exit 0 only when CAUGHT.
#
# D1: the PASS regexes are part of the verdict; a crash after printing still
#     fails.  Missing 7z.dll (package) or an account that cannot write
#     HKCU\Software\Classes (verify) exits 77 and ctest says "Skipped" -- never
#     a silent green.
# D26: no WILL_FAIL anywhere; each mutant carries its own CAUGHT marker.
# D28: no test name contains clean / unarmed / control.
# D29: TIMEOUT values are real budgets (the scripts bound every child process).
# DEPENDS: verify/mutants run after win.c68_package.  If packaging SKIPs (no
#     7z.dll on the machine, exit 77), verify-package itself exits 77 with the
#     reason instead of becoming a hard "Not Run" -- a skip stays a skip.
# RESOURCE_LOCK: package runs "cmake --install" into the shared build tree
#     (install_manifest.txt), so it takes C24's lock; verify/mutants do not.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  find_program(PRISM_PWSH NAMES pwsh)
  if(NOT PRISM_PWSH)
    message(STATUS "c68-release: pwsh not found -- installable-package suite not registered")
  else()
    set(PRISM_C68_TOOLS "${CMAKE_SOURCE_DIR}/tools")
    set(PRISM_C68_OUT "${CMAKE_BINARY_DIR}/c68-package")

    add_test(NAME "win.c68_package"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_C68_TOOLS}/package.ps1"
                     -RepoRoot "${CMAKE_SOURCE_DIR}"
                     -BuildDir "${CMAKE_BINARY_DIR}"
                     -Config "$<CONFIG>"
                     -OutDir "${PRISM_C68_OUT}"
                     -AssertIdempotent)
    set_tests_properties(win.c68_package PROPERTIES
        TIMEOUT 600
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "PACKAGE: PASS .*idempotent=YES"
        RESOURCE_LOCK "prism-release-install"
        LABELS "release;packaging;distribution")

    add_test(NAME "win.c68_verify"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_C68_TOOLS}/verify-package.ps1"
                     -ZipDir "${PRISM_C68_OUT}")
    set_tests_properties(win.c68_verify PROPERTIES
        TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "PACKAGE VERIFY: PASS"
        DEPENDS "win.c68_package"
        LABELS "release;installer;distribution")

    foreach(mutant IN ITEMS DropShellDll SkipVerbs)
      string(TOLOWER "${mutant}" mutant_lc)
      add_test(NAME "mutation.c68.${mutant_lc}"
               COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                       -File "${PRISM_C68_TOOLS}/verify-package.ps1"
                       -ZipDir "${PRISM_C68_OUT}"
                       -Mutate "${mutant}")
      set_tests_properties("mutation.c68.${mutant_lc}" PROPERTIES
          TIMEOUT 300
          SKIP_RETURN_CODE 77
          PASS_REGULAR_EXPRESSION "PACKAGE VERIFY\\(MUTATION ${mutant}\\): CAUGHT"
          DEPENDS "win.c68_package"
          LABELS "release;mutation")
    endforeach()
  endif()
endif()