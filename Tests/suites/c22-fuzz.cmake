# c22-fuzz: malformed-archive corpus (blueprint Phase 1 exit criterion: 10 000
# real malformed / nested / mojibake / encrypted samples).
#
# One card = one file; see suites/README.md. Nothing else in Tests/ is touched.
#
# Shape of the gate:
#   fuzz.gen_corpus      the generator runs as the fixture SETUP -- it is a gate
#                        too, so a corpus that cannot be produced is a red test,
#                        not a quietly skipped one;
#   core.test_fuzz_smoke the corpus is fed through PathSafety / SmartExtractor /
#                        SecurityGuard and every rejection must carry a reason;
#   fuzz.corpus_contract the generator's own contract (50..200 recipes/class,
#                        determinism, valid control, 10 000 capacity);
#   mutation.fuzz.*      two negative controls + one that proves the injection
#                        really happened (CONTRACTS D3).
#
# CI time: the 10 000 samples are generated in ~1 s and consumed in ~1 s (packed
# container: one 5 MB file instead of 10 000 tiny files, which cost 31 s of
# real-time AV on Windows). This is why the card ships a deterministic corpus
# instead of wiring in libFuzzer/AFL -- see tools/fuzz/README.md.

find_package(Python3 COMPONENTS Interpreter QUIET)

set(C22_FUZZ_TOTAL 10000)
set(C22_FUZZ_DIR "${CMAKE_CURRENT_BINARY_DIR}/fuzz-corpus")

add_executable(test_fuzz_smoke test_fuzz_smoke.cpp)
target_link_libraries(test_fuzz_smoke PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_fuzz_smoke PRIVATE /WX-)
endif()

if(Python3_FOUND)
  # Fixture SETUP: generated once per build tree, then consumed by every
  # target that declares FIXTURES_REQUIRED c22_corpus (including both mutants).
  add_test(NAME fuzz.gen_corpus
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/../tools/fuzz/gen_corpus.py"
                   --out "${C22_FUZZ_DIR}" --total ${C22_FUZZ_TOTAL} --pack --loose 24)
  set_tests_properties(fuzz.gen_corpus PROPERTIES
      FIXTURES_SETUP c22_corpus
      TIMEOUT 300
      PASS_REGULAR_EXPRESSION "FUZZ CORPUS READY: 10000 samples")

  add_test(NAME core.test_fuzz_smoke
           COMMAND test_fuzz_smoke "${C22_FUZZ_DIR}" ${C22_FUZZ_TOTAL})
  set_tests_properties(core.test_fuzz_smoke PROPERTIES
      FIXTURES_REQUIRED c22_corpus
      TIMEOUT 600
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL FUZZ CHECKS PASSED")

  # The generator's own contract. D4: the checker that says "the control archive
  # is well formed" must itself be calibrated against a known-good archive.
  add_test(NAME fuzz.corpus_contract
           COMMAND "${Python3_EXECUTABLE}"
                   "${CMAKE_CURRENT_SOURCE_DIR}/../tools/fuzz/gen_corpus.py"
                   --check --total ${C22_FUZZ_TOTAL})
  set_tests_properties(fuzz.corpus_contract PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "FUZZ CORPUS CONTRACT OK")

  # ---- negative controls (CONTRACTS D3) ----------------------------------
  foreach(mutant IN ITEMS ACCEPT_ALL VALID_REJECTED)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("test_fuzz_smoke_mutant_${mutant_lc}" test_fuzz_smoke.cpp)
    target_link_libraries("test_fuzz_smoke_mutant_${mutant_lc}" PRIVATE prism_core)
    target_compile_definitions("test_fuzz_smoke_mutant_${mutant_lc}"
                               PRIVATE "PRISM_FUZZ_MUTANT_${mutant}=1")
    if(WIN32)
      target_compile_options("test_fuzz_smoke_mutant_${mutant_lc}" PRIVATE /WX-)
    endif()
    add_test(NAME "mutation.fuzz.${mutant_lc}"
             COMMAND "test_fuzz_smoke_mutant_${mutant_lc}" "${C22_FUZZ_DIR}" 0)
    # WILL_FAIL: green only while the mutation is actually caught.
    set_tests_properties("mutation.fuzz.${mutant_lc}" PROPERTIES
        FIXTURES_REQUIRED c22_corpus
        TIMEOUT 600
        WILL_FAIL TRUE)
  endforeach()

  # "The injection happened": a misspelled compile definition would leave the
  # mutant identical to the clean binary, which WILL_FAIL would then report as a
  # failure to catch -- indistinguishable from a sharp test. The mutant must
  # announce itself, and the clean binary must deny it.
  add_test(NAME mutation.fuzz.injection_armed
           COMMAND test_fuzz_smoke_mutant_accept_all --self-report-only=armed)
  set_tests_properties(mutation.fuzz.injection_armed PROPERTIES
      TIMEOUT 120
      PASS_REGULAR_EXPRESSION "FUZZ MUTATION ARMED: accept-all")

  add_test(NAME mutation.fuzz.clean_build_unarmed
           COMMAND test_fuzz_smoke --self-report-only=clean)
  set_tests_properties(mutation.fuzz.clean_build_unarmed PROPERTIES
      TIMEOUT 120
      PASS_REGULAR_EXPRESSION "FUZZ MUTATION SELF-REPORT: NOT armed")
else()
  # No Python3: the corpus cannot be produced. Register the smoke test anyway so
  # it reports a visible SKIP (77) instead of vanishing from the suite -- a
  # silently unregistered test is the false green D1 exists to prevent.
  message(WARNING "c22-fuzz: Python3 not found -- fuzz smoke test will SKIP")
  add_test(NAME core.test_fuzz_smoke COMMAND test_fuzz_smoke)
  set_tests_properties(core.test_fuzz_smoke PROPERTIES
      TIMEOUT 600
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL FUZZ CHECKS PASSED")
endif()