# c44-verdict: CONTRACTS D27 -- a mutation verdict is printed at the FIRST
# failed assertion and the process leaves with 0 immediately, so teardown can
# never swallow the marker. One card = one file; see suites/README.md.
#
# The binary is pure logic (child-process probe, no window station, no 7z.dll):
# it proves both the armed shape (marker before the next statement, exit 0) and
# the unarmed shape (record-the-failure-and-continue, exit 1). No PASS regex is
# needed: the suite has no skip and no integration marker.
add_executable(core_test_mutation_verdict test_mutation_verdict.cpp)
target_link_libraries(core_test_mutation_verdict PRIVATE prism_core)
add_test(NAME "core.test_mutation_verdict" COMMAND core_test_mutation_verdict)
