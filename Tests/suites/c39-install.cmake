# c39-install: clean-user install -> open -> right-click -> uninstall -> restore
# (card C39).  One card = one file; see suites/README.md.
#
# tools/verify-install.ps1 is the end-to-end proof, not a restatement of
# install.ps1: it snapshots the registry itself, installs, asserts the .7z
# ProgID/command/icon/Open-with entry, runs the INSTALLED exe headlessly with
# --test against a real .7z, uninstalls, and compares the whole .7z key tree
# byte-for-byte with the pre-install snapshot.  It also asserts that a second
# install writes nothing and that uninstall-before-install / a second uninstall
# are safe no-ops.
#
# The CI-safe mode is -Sandbox: a unique HKCU scratch key + %TEMP% directory,
# removed even on failure.  Real HKCU associations are only touched by the
# manual run documented in README.md / docs/RELEASE.md.
#
# SKIP: missing build output or 7z.dll exits 77 with an explicit reason, and
# ctest reports "Skipped" rather than a green tick (CONTRACTS D1).  The PASS
# regex is a conjunction with exit code 0: a crash that prints PASS still fails.
#
# Negative controls (CONTRACTS D3) forward one real mutation from
# install.ps1/uninstall.ps1, verify the injection actually changed the state,
# and exit 0 only when the corresponding assertion went red.  No WILL_FAIL is
# used, and PASS_REGULAR_EXPRESSION + WILL_FAIL are never combined (D26).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  find_program(PRISM_PWSH NAMES pwsh)
  if(NOT PRISM_PWSH)
    message(STATUS "c39-install: pwsh not found -- install end-to-end suite not registered")
  else()
    set(PRISM_VERIFY_INSTALL "${CMAKE_SOURCE_DIR}/tools/verify-install.ps1")

    add_test(NAME "win.verify_install"
             COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                     -File "${PRISM_VERIFY_INSTALL}"
                     -SourceDir "$<TARGET_FILE_DIR:PrismArchive>"
                     -Sandbox)
    set_tests_properties(win.verify_install PROPERTIES
        TIMEOUT 600
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "VERIFY-INSTALL: PASS"
        LABELS "installer;distribution")

    foreach(mutant IN ITEMS UnquotedVerb NoRestore)
      string(TOLOWER "${mutant}" mutant_lc)
      add_test(NAME "mutation.c39.${mutant_lc}"
               COMMAND "${PRISM_PWSH}" -NoProfile -ExecutionPolicy Bypass
                       -File "${PRISM_VERIFY_INSTALL}"
                       -SourceDir "$<TARGET_FILE_DIR:PrismArchive>"
                       -Sandbox -Mutate "${mutant}")
      set_tests_properties("mutation.c39.${mutant_lc}" PROPERTIES
          TIMEOUT 600
          SKIP_RETURN_CODE 77
          PASS_REGULAR_EXPRESSION "VERIFY-INSTALL\\(MUTATION ${mutant}\\): CAUGHT"
          LABELS "installer;mutation")
    endforeach()
  endif()
endif()
