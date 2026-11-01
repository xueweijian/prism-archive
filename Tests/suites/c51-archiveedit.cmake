# c51-archiveedit: EDIT AN EXISTING ARCHIVE (rename / delete / new folder / move)
# without a full extraction -- the decision layer, the copy-on-write executor, and
# two negative controls. One card = one file (see suites/README.md).
#
# DECISION LAYER (every lane, milliseconds): PrismCore/EditPlan.hpp turns a
# catalog + a list of edits into "what the archive will contain", what each edit
# touches, whether a solid block is re-encoded, what it costs, and which edits are
# ILLEGAL (duplicate names, path traversal, deleting a non-empty folder without
# recurse, read-only archive, multi-volume set, malformed catalog names).
add_executable(core_test_edit_plan test_edit_plan.cpp)
target_link_libraries(core_test_edit_plan PRIVATE prism_core)
add_test(NAME "c51.edit_plan" COMMAND core_test_edit_plan)
# D1: the marker is a CONJUNCTION with the exit code, never decoration.
set_tests_properties(c51.edit_plan PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL EDIT-PLAN CHECKS PASSED")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # EXECUTOR (real 7z.dll): create a 5-entry solid .7z with Chinese names + a
  # folder, then run all four operations, and after EVERY one re-open the new
  # archive and compare entry names, kinds, sizes, payload bytes, stored CRC32,
  # the encrypted flag and the solid flag against the original. /WX- because the
  # Windows SDK headers are not /W4-clean.
  add_executable(win_archive_edit_test win/test_archive_edit.cpp)
  target_link_libraries(win_archive_edit_test PRIVATE prism_core prism_win_bridge
                        ole32.lib oleaut32.lib user32.lib)
  target_compile_options(win_archive_edit_test PRIVATE /WX-)
  add_test(NAME "c51.edit.roundtrip" COMMAND win_archive_edit_test)
  # CONTRACTS D1: a missing 7z.dll returns 77 (a visible Skip, never a fake
  # green) and the stdout marker is required on top of exit code 0.
  set_tests_properties(c51.edit.roundtrip PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL ARCHIVE-EDIT CHECKS PASSED")

  # NEGATIVE CONTROLS (D3 + D26 + D27). The same source is built twice with ONE
  # rule deleted each time:
  #   keep_deleted    -> the "skip the entries the user deleted" filter is gone,
  #                      so a deleted entry is copied back into the new archive;
  #   broken_publish  -> "a failed edit leaves the original alone" is gone, so the
  #                      half-written temp file is published over the original.
  # Neither uses WILL_FAIL (D26 forbids stacking it on a pass marker). Instead the
  # binary arms SetMutationVerdict(prefix, predicate) BEFORE the first case: the
  # first assertion failure prints "C51 MUTATION CAUGHT: <arm>" and exits 0 only
  # while the injection counter proves the defect actually fired. A stale
  # injection therefore stays RED (missing marker), and an unrelated red can never
  # masquerade as CAUGHT.
  foreach(mutant IN ITEMS KEEP_DELETED BROKEN_PUBLISH)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_archive_edit_mutant_${mutant_lc}" win/test_archive_edit.cpp)
    target_link_libraries("win_archive_edit_mutant_${mutant_lc}" PRIVATE prism_core
                          prism_win_bridge ole32.lib oleaut32.lib user32.lib)
    target_compile_options("win_archive_edit_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_archive_edit_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C51_MUTANT_${mutant}=1")
    add_test(NAME "c51.mutation.${mutant_lc}"
             COMMAND "win_archive_edit_mutant_${mutant_lc}")
    set_tests_properties("c51.mutation.${mutant_lc}" PROPERTIES TIMEOUT 300
        SKIP_RETURN_CODE 77
        PASS_REGULAR_EXPRESSION "C51 MUTATION CAUGHT")
  endforeach()
endif()
