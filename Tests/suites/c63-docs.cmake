# c63-docs: the user-facing documents are part of the product (card C63).
#
# One card = one file; see suites/README.md. This suite registers no compiled
# target: the artifact under test is README.md + docs/USAGE.md +
# docs/TROUBLESHOOTING.md + docs/assets/README.md, checked by
# tools/check_docs.py.
#
# What is proven here, and what would make each of these a lie:
#
#   docs.c63_gate            the four documents exist with real content, have
#                            exactly one H1 and no skipped heading levels, fence
#                            every code block with a language, contain no
#                            hard-coded C:\Users\ path, and every relative link
#                            (anchors included) resolves. The README's promised
#                            sections and the eight troubleshooting symptoms are
#                            asserted by keyword, so "I wrote a page" cannot pass
#                            with the promised chapter missing.
#
#   docs.c63_gate.selftest   CONTRACTS D3: the gate proves it can go RED. It
#                            builds a known-good document set in a UNIQUE temp
#                            directory, then injects one defect at a time
#                            (missing file, no H1, skipped level, bare fence,
#                            C:\Users\, dangling link, dangling anchor). Green
#                            means the good set passed AND every defect was
#                            caught; a broken pattern that silently matches
#                            nothing turns this test red, not green.
#
# D1: the success markers are part of the verdict -- CTest requires exit 0 AND
#     the marker, so a crash after printing still fails.
# D26: no WILL_FAIL here; a mutant would be the wrong tool for a static gate,
#     and the selftest already carries the negative controls.
# D28: neither test name contains clean / unarmed / control.
# D29: both run in seconds; TIMEOUT 120 is a real budget, not the 1500 s default.
#
# Python is the same interpreter the manifest gate already uses; when it is
# absent the suite says so instead of silently registering nothing.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  add_test(NAME docs.c63_gate
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_SOURCE_DIR}/tools/check_docs.py"
                   --root "${CMAKE_SOURCE_DIR}")
  set_tests_properties(docs.c63_gate PROPERTIES
      TIMEOUT 120
      PASS_REGULAR_EXPRESSION "DOCS GATE: OK")

  add_test(NAME docs.c63_gate.selftest
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_SOURCE_DIR}/tools/check_docs.py"
                   --selftest)
  set_tests_properties(docs.c63_gate.selftest PROPERTIES
      TIMEOUT 120
      PASS_REGULAR_EXPRESSION "DOCS GATE SELFTEST: PASS")
else()
  message(STATUS "c63-docs: Python3 not found -- documentation gate not registered")
endif()
