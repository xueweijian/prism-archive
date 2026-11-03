# c61-msix-free: the certificate-free MSIX paths (card C61).
#
# Owned by card C61.  See Tests/suites/README.md; registration lives here instead
# of the shared Tests/CMakeLists.txt tail.
#
# What is proven (all of it by tools/msix/Test-MsixFree.ps1, which runs the card's
# scripts as child processes):
#
#   win.msix_free_syntax
#     * every .ps1 in tools/msix parses with BOTH gates (Parser::ParseFile and
#       [scriptblock]::Create), is pure ASCII and BOM-free (Windows PowerShell 5.1
#       reads a BOM-less script as ANSI), and the count/byte totals are printed so
#       a silently skipped file is visible. Separate ctest entry because a syntax
#       regression must be a named red test, not a paragraph inside a bigger one.
#
#   win.msix_free_selftest
#     * every script parses (double gate), declares the parameters callers pass,
#       and is -WhatIf capable;
#     * register-loose.ps1's Add-AppxPackage call carries BOTH -Register and
#       -ExternalLocation -- the second one is not optional: measured
#       0x80073CF9 "must be installed using an external location" without it;
#     * the checked-in manifest parses and agrees with the compiled constants
#       (kPackageName / kPackagePublisher / kExplorerCommandClsid) in
#       Src/PrismWin/SparsePackageRegistrar.hpp;
#     * four broken manifests are rejected with their own actionable reason, and
#       a missing shell DLL is only a WARNING (measured: registration succeeds
#       without it; the menu is what breaks);
#     * the 0x80073CFF branch: with Developer Mode OFF (forced through the
#       PRISM_MSIX_DEV_MODE test seam, because CI must not write HKLM) the plan
#       prints BLOCKER + the absolute path of enable-dev-mode-and-register.cmd,
#       the real run exits 1 with reason=dev-mode-off and registers nothing;
#     * all 12 HRESULT rows map from the message text Add-AppxPackage produces,
#       plus the unknown-code default (this assertion caught the signedness bug:
#       0x80073CFF/0x800B0109/0x800B010F were dead rows in a uint32 switch);
#     * unregister-loose.ps1 is idempotent when the package was never installed;
#     * selfsign-and-install.ps1 -WhatIf creates no certificate (store counted
#       before/after), never prints the PFX password, and reports the admin
#       requirement for -Machine as an executable Start-Process line;
#     * docs/MSIX-SIGNING.md carries the trade-off table (cost / admin /
#       distribution columns), every path's command block and the measured
#       HRESULTs.
#
#   mutation.c61.msix_free.<name>
#     One injected defect each, and the injection is verified to have landed
#     ("NOT INJECTED" exits 1). The test exits 0 ONLY when the EXPECTED check
#     went red -- "the suite is red for some other reason" is NOT a caught
#     mutation (CONTRACTS D3/D27). No WILL_FAIL anywhere: the CAUGHT marker plus
#     exit 0 is the verdict, per the shape C3/C24 established (CONTRACTS D26).
#
# No build products are needed: the suite is PowerShell + XML + a temp folder, so
# it is safe on a shared runner. It never registers, trusts or installs anything;
# real installs stay manual (docs/MSIX-SIGNING.md sections 2-3) because the CI
# runner has no interactive desktop and the package database is user state.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  find_program(PRISM_PWSH NAMES pwsh)
  if(NOT PRISM_PWSH)
    message(STATUS "c61-msix-free: pwsh not found -- certificate-free MSIX suite not registered")
  else()
    set(PRISM_MSIX_SELFTEST "${CMAKE_SOURCE_DIR}/tools/msix/Test-MsixFree.ps1")

    add_test(NAME "win.msix_free_syntax"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_MSIX_SELFTEST}" -SyntaxOnly)
    set_tests_properties("win.msix_free_syntax" PROPERTIES
        TIMEOUT 120
        PASS_REGULAR_EXPRESSION "MSIX-SYNTAX: PASS"
        LABELS "msix;distribution")

    # 600s is a real budget, not the 1500s default (D29): the full assertion run
    # measured 53s on an idle box (ctest -j 3) and 290s while an 11-way MSVC build
    # hammered the same machine, so the budget has to survive a loaded runner
    # without turning a hang into a lane-wide stall.
    add_test(NAME "win.msix_free_selftest"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_MSIX_SELFTEST}")
    set_tests_properties("win.msix_free_selftest" PROPERTIES
        TIMEOUT 600
        PASS_REGULAR_EXPRESSION "MSIX-FREE: PASS"
        LABELS "msix;distribution")

    foreach(mutant IN ITEMS DropExternalLocation DropManifestGate DropPublisherGate
                              DropDevModeReport LeakPassword BreakUnregisterNoOp
                              BreakSyntax DropDevModeFix DropClassicCertFree ShiftHresultKey)
      string(TOLOWER "${mutant}" mutant_lc)
      add_test(NAME "mutation.c61.msix_free.${mutant_lc}"
               COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                       -File "${PRISM_MSIX_SELFTEST}" -Mutate "${mutant}")
      # Same budget as the full run: a mutation arm runs the same assertions and
      # stops at its first expected red, so it can never outlast it.
      set_tests_properties("mutation.c61.msix_free.${mutant_lc}" PROPERTIES
          TIMEOUT 600
          PASS_REGULAR_EXPRESSION "MSIX-FREE\\(MUTATION ${mutant}\\): CAUGHT"
          LABELS "msix;mutation")
    endforeach()
  endif()
endif()
