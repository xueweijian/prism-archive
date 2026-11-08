# c128-status: the status band tells you WHAT you are looking at, and the info
# card stops repeating itself (proposal 5.5 / 5.6, card C128).
#
# THE DEFECT (measured on the report's screenshot): the band printed the card's
# own three facts a second time --
#     info card : ENCRYPTION 未加密 / METHOD 未知 / DICTIONARY -- / RATIO 98.8%
#     status bar: 未加密 / 方法未知 / 字典未知 / 条目 5 / 选中 0 / 压缩率 98.8%
# -- and the card sat OPEN by default, 157 px of the rail's bottom-left corner
# for four numbers the user did not ask for.
#
# WHAT IS REGISTERED
#   win.c128_status              the real suite: S1 (exactly three slots, the
#                                report's numbers, 640..1920 px), S1b (the counts
#                                are the CURRENT FOLDER's, the size is the
#                                archive's), S2 (the card is a 42 px line folded
#                                and 157 px of four label/value rows expanded),
#                                S2b (a click toggles it), S3 (加密/方法/字典 are
#                                not in the band) and S4 (the fold is persisted
#                                and defaults to folded).
#   mutation.c128.old_status     the D3 control: PRISM_C128_MUTANT_OLD_STATUS
#                                restores the six-slot band, so S1 and S3 must go
#                                red. The entry decides with
#                                PASS_REGULAR_EXPRESSION on
#                                "C128 MUTATION CAUGHT: old_status" -- never
#                                WILL_FAIL (CONTRACTS D26/D27) -- and the marker
#                                prints only while ActiveStatusMutation() proves
#                                the injection is in that binary (D3), so an
#                                unrelated red stays red.
#
# The suite drives the REAL GalleryWindow (the catalog the app attaches, the hit
# test the user clicks) and the REAL settings document (serialized, re-parsed),
# not a hand-built model: "the band says three numbers" must be a fact about the
# product, not about a test fixture.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C128_LIBS prism_core prism_win_bridge user32.lib gdi32.lib shell32.lib
      ole32.lib oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib
      comctl32.lib comdlg32.lib shlwapi.lib d3d11.lib dxgi.lib dcomp.lib)

  add_executable(win_c128_status win/test_c128_status.cpp)
  target_link_libraries(win_c128_status PRIVATE ${PRISM_C128_LIBS})
  target_compile_options(win_c128_status PRIVATE /WX-)
  add_test(NAME "win.c128_status" COMMAND win_c128_status)
  # D1: the marker is a CONJUNCTION with the exit code. A window station that
  # cannot create the HWND returns 77 -- a visible Skip, never a fake green.
  set_tests_properties(win.c128_status PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C128 STATUS CHECKS PASSED")

  add_executable(win_c128_status_mutant_old_status win/test_c128_status.cpp)
  target_link_libraries(win_c128_status_mutant_old_status PRIVATE ${PRISM_C128_LIBS})
  target_compile_options(win_c128_status_mutant_old_status PRIVATE /WX-)
  target_compile_definitions(win_c128_status_mutant_old_status
                             PRIVATE PRISM_C128_MUTANT_OLD_STATUS=1)
  add_test(NAME "mutation.c128.old_status"
           COMMAND win_c128_status_mutant_old_status)
  set_tests_properties(mutation.c128.old_status PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C128 MUTATION CAUGHT: old_status")
endif()
