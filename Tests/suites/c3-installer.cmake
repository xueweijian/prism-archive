# c3-installer: per-user install / file associations / icon / uninstall (card C3)
# One card = one file; see suites/README.md.
#
# This suite is unusual: it registers no compiled target. The artifact under test
# is tools/install.ps1's round trip (dry-run purity, install, idempotent
# reinstall, association restore, clean uninstall, idempotent uninstall, portable
# isolation) which runs in a scratch registry key and a scratch directory, so it
# is safe on a shared machine and needs no build products.
#
# Negative controls: the harness injects one defect per mutation and reports
# "SELFTEST(MUTATION X): CAUGHT (n assertions went red)". WILL_FAIL is
# deliberately NOT used -- the harness exits 0 only when the injection was
# observed, and non-zero when it was NOT ("NOT CAUGHT"), so a plain test with a
# pinned success marker is the correct shape: green means the guard is sharp,
# red means the guard went blind (CONTRACTS D3).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  find_program(PRISM_PWSH NAMES pwsh)
  if(NOT PRISM_PWSH)
    message(STATUS "c3-installer: pwsh not found -- installer round-trip suite not registered")
  else()
    set(PRISM_INSTALLER_FLAGS -NoProfile -ExecutionPolicy Bypass
        -File "${CMAKE_SOURCE_DIR}/tools/install.ps1")

    add_test(NAME "win.installer_selftest"
             COMMAND "${PRISM_PWSH}" ${PRISM_INSTALLER_FLAGS} -SelfTest -Quiet)
    set_tests_properties("win.installer_selftest" PROPERTIES TIMEOUT 300
        PASS_REGULAR_EXPRESSION "SELFTEST: PASS"
        LABELS "installer;distribution")

    foreach(mutant IN ITEMS SkipDefaultIcon NoBackup NoRestore UnquotedVerb
                              Rebackup EnvUnderSoftware)
      string(TOLOWER "${mutant}" mutant_lc)
      add_test(NAME "mutation.installer.${mutant_lc}"
               COMMAND "${PRISM_PWSH}" ${PRISM_INSTALLER_FLAGS} -SelfTest -Quiet -Mutate "${mutant}")
      set_tests_properties("mutation.installer.${mutant_lc}" PROPERTIES TIMEOUT 300
          PASS_REGULAR_EXPRESSION "SELFTEST\\(MUTATION ${mutant}\\): CAUGHT")
    endforeach()
  endif()
endif()
