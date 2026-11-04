# c84-dialog-visible: a dialog that Create() made must actually be on screen.
#
# MEASURED BEFORE THIS CARD (agent-desktop 02 report, S1-3 / S2-5):
#   * Settings... created the card WITHOUT WS_VISIBLE and OpenModal() entered
#     its message loop without a ShowWindow: the owner was disabled and the
#     process painted nothing (the user's "settings freeze").
#   * Compress created the same kind of window and no product call ever showed
#     it, so clicking the verb produced no window at all.
#   * Open on enc.7z created the password gate and pumped messages with no
#     visible window: process alive, data: [] on the desktop.
# The fix is in Create() of the three dialogs (center over owner + ShowWindow +
# UpdateWindow); the modal paths add foreground + focus. This suite drives the
# product paths (a real GalleryWindow's WM_COMMAND, and a spawned
# PrismArchive.exe on enc.7z with an isolated %LOCALAPPDATA%), not just Create.
#
# NEGATIVE CONTROL (CONTRACTS D3/D26/D27, NO WILL_FAIL): the mutant target
# compiles the same source -- and therefore the same dialog headers -- with
# PRISM_C84_MUTANT_NO_SHOW, which removes the three ShowWindow calls. Every
# visibility assertion must go red; mini_test prints
# "C84 MUTATION CAUGHT: no_show" at the first failure, and that marker is this
# entry's PASS_REGULAR_EXPRESSION. A mutation that stopped applying stays red.
#
# D1: the spawned-app cases need PrismArchive.exe + Tests/fixtures/c82/enc.7z +
# PRISM_7Z_DLL. A missing input prints WHY and exits 77; SKIP_RETURN_CODE 77
# keeps that visible and never lets it count as a pass.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C84_LIBS
      prism_core
      prism_win_bridge
      prism_shell_contract
      user32.lib
      gdi32.lib
      shell32.lib
      ole32.lib
      oleaut32.lib
      windowscodecs.lib
      d2d1.lib
      dwrite.lib
      bcrypt.lib
      comctl32.lib
      comdlg32.lib
      shlwapi.lib
      crypt32.lib
      version.lib
      advapi32.lib)

  add_executable(win_c84_dialog_visible win/test_c84_dialog_visible.cpp)
  target_link_libraries(win_c84_dialog_visible PRIVATE ${PRISM_C84_LIBS})
  target_compile_options(win_c84_dialog_visible PRIVATE /WX-)  # SDK headers: B9
  # The product path is a SPAWNED app: build it before the test, pass its path
  # through the environment (preflight D1 wants "COMMAND <exe>" as the command).
  add_dependencies(win_c84_dialog_visible PrismArchive)
  add_test(NAME "win.c84_dialog_visible" COMMAND win_c84_dialog_visible)
  set_tests_properties(win.c84_dialog_visible PROPERTIES
      TIMEOUT 300
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C84_EXE=$<TARGET_FILE:PrismArchive>;PRISM_C84_FIXTURES=${CMAKE_SOURCE_DIR}/Tests/fixtures/c82"
      PASS_REGULAR_EXPRESSION "ALL C84 DIALOG-VISIBLE CHECKS PASSED")

  # Negative control (D3/D26/D27): ShowWindow is compiled out of all three
  # dialogs; the visibility assertions must be red and print the marker.
  add_executable(win_c84_dialog_visible_mutant_no_show win/test_c84_dialog_visible.cpp)
  target_link_libraries(win_c84_dialog_visible_mutant_no_show PRIVATE ${PRISM_C84_LIBS})
  target_compile_options(win_c84_dialog_visible_mutant_no_show PRIVATE /WX-)
  target_compile_definitions(win_c84_dialog_visible_mutant_no_show
                             PRIVATE PRISM_C84_MUTANT_NO_SHOW=1)
  add_dependencies(win_c84_dialog_visible_mutant_no_show PrismArchive)
  add_test(NAME "c84.mutation.dialog.no_show"
           COMMAND win_c84_dialog_visible_mutant_no_show)
  set_tests_properties(c84.mutation.dialog.no_show PROPERTIES
      TIMEOUT 300
      SKIP_RETURN_CODE 77
      ENVIRONMENT "PRISM_C84_EXE=$<TARGET_FILE:PrismArchive>;PRISM_C84_FIXTURES=${CMAKE_SOURCE_DIR}/Tests/fixtures/c82"
      PASS_REGULAR_EXPRESSION "C84 MUTATION CAUGHT: no_show")
endif()
