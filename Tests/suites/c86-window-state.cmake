# c86-window-state: WHERE the window was, WHAT the status band says, and WHICH
# number sits next to a folder name (card C86, S3-2 + S3-11 + S3-4).
#
# THE THREE MEASURED DEFECTS
#   1. %LOCALAPPDATA%\PrismArchive\settings.json held
#      "window": {"width": 1536, "height": 480, "listMode": true, ...} and no
#      origin: a window dragged to (300,150) came back at the CW_USEDEFAULT
#      cascade. C66 fixed WHICH size is written; the position never left the
#      process, because the window captured a size and the app's WindowState had
#      no field for x/y.
#   2. A UIA client saw ONE element for the status band ("status | 共 3 项"), so
#      未加密 / 方法未知 / 字典未知 / 条目 / 选中 / 压缩率 were painted and
#      unreachable.
#   3. Inside a two-entry folder the breadcrumb still read "共 16 项" (the
#      archive total, written once by the app) while the band said 条目 2.
#
# TWO LAYERS, because the fixes have two halves:
#   core.c86_window_state  the policy as pure integer math -- the saved window
#                          (origin + size + maximized bit), the clamp onto a
#                          SHRUNK work area, and the two texts the window
#                          publishes. Runs on every lane in milliseconds.
#   c86.window             a REAL window: move -> close -> open within 2 px,
#                          maximized saved as the normal frame plus a mode,
#                          the band walked through MSAA (the same surface UIA's
#                          proxy uses) segment by segment, accLocation order,
#                          the selection reaching 选中, and the crumb's number
#                          matching 条目 after drilling into a subfolder.
#
# VERDICT SHAPE (CONTRACTS D1 + D26 + D27 + D28)
#   Every target pins its stdout marker; the marker is a CONJUNCTION with exit 0.
#   The four negative controls do NOT use WILL_FAIL: each binary arms mini_test's
#   mutation verdict with a runtime predicate and prints
#   "C86 MUTATION CAUGHT: <arm>" at the FIRST red assertion, so a stale injection
#   (suite still green) leaves the mutant entry RED -- the guard proves itself.
#   Arm names avoid clean/unarmed/control (D28). Every timeout is a real budget
#   (D29).
add_executable(test_c86_window_state test_c86_window_state.cpp)
target_link_libraries(test_c86_window_state PRIVATE prism_core)
add_test(NAME "core.c86_window_state" COMMAND test_c86_window_state)
set_tests_properties(core.c86_window_state PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ALL WINDOW-STATE CHECKS PASSED")

# D3 control 1: the origin is dropped on the way out -- the pre-fix document,
# byte for byte. "The saved window keeps its origin" MUST go red.
add_executable(test_c86_window_state_mutant_no_position test_c86_window_state.cpp)
target_link_libraries(test_c86_window_state_mutant_no_position PRIVATE prism_core)
target_compile_definitions(test_c86_window_state_mutant_no_position
                           PRIVATE PRISM_C86_MUTANT_NO_POSITION=1)
add_test(NAME "c86.mutation.window_state.no_position"
         COMMAND test_c86_window_state_mutant_no_position)
set_tests_properties(c86.mutation.window_state.no_position PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: no_position")

# D3 control 2: the breadcrumb count is the archive total again (S3-4's
# reported number). "The count names the folder" MUST go red.
add_executable(test_c86_window_state_mutant_archive_count test_c86_window_state.cpp)
target_link_libraries(test_c86_window_state_mutant_archive_count PRIVATE prism_core)
target_compile_definitions(test_c86_window_state_mutant_archive_count
                           PRIVATE PRISM_C86_MUTANT_ARCHIVE_COUNT=1)
add_test(NAME "c86.mutation.window_state.archive_count"
         COMMAND test_c86_window_state_mutant_archive_count)
set_tests_properties(c86.mutation.window_state.archive_count PROPERTIES TIMEOUT 60
    PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: archive_count")

if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  # oleacc/oleaut32: the MSAA client; dwmapi: the visible frame C66 measures;
  # d3d11/dxgi/dcomp: GalleryWindow's glass layer names them in its header.
  set(PRISM_C86_WINDOW_LIBS prism_core prism_win_bridge
      user32.lib gdi32.lib shell32.lib ole32.lib oleacc.lib oleaut32.lib
      d2d1.lib dwrite.lib windowscodecs.lib d3d11.lib dxgi.lib dcomp.lib
      dwmapi.lib imm32.lib)

  add_executable(win_c86_window_test win/test_c86_window.cpp)
  target_link_libraries(win_c86_window_test PRIVATE ${PRISM_C86_WINDOW_LIBS})
  target_compile_options(win_c86_window_test PRIVATE /WX-)  # SDK headers: B9
  add_test(NAME "c86.window" COMMAND win_c86_window_test)
  # D1: the marker is a CONJUNCTION with exit 0; a window station that cannot
  # create the HWND returns 77 (a visible skip, never a fake green).
  # D29: a real budget (COM + four windows + four MSAA walks).
  set_tests_properties(c86.window PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL WINDOW-STATE CHECKS PASSED")

  # D3 control 3: the app writes width/height only -- the pre-fix document.
  add_executable(win_c86_window_mutant_no_position win/test_c86_window.cpp)
  target_link_libraries(win_c86_window_mutant_no_position PRIVATE ${PRISM_C86_WINDOW_LIBS})
  target_compile_options(win_c86_window_mutant_no_position PRIVATE /WX-)
  target_compile_definitions(win_c86_window_mutant_no_position
                             PRIVATE PRISM_C86_MUTANT_NO_POSITION=1)
  add_test(NAME "c86.mutation.window.no_position"
           COMMAND win_c86_window_mutant_no_position)
  set_tests_properties(c86.mutation.window.no_position PROPERTIES TIMEOUT 240
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: no_position")

  # D3 control 4: the status band is one element again (the pre-fix tree).
  add_executable(win_c86_window_mutant_no_status_segments win/test_c86_window.cpp)
  target_link_libraries(win_c86_window_mutant_no_status_segments
                        PRIVATE ${PRISM_C86_WINDOW_LIBS})
  target_compile_options(win_c86_window_mutant_no_status_segments PRIVATE /WX-)
  target_compile_definitions(win_c86_window_mutant_no_status_segments
                             PRIVATE PRISM_C86_MUTANT_NO_STATUS_SEGMENTS=1)
  add_test(NAME "c86.mutation.window.no_status_segments"
           COMMAND win_c86_window_mutant_no_status_segments)
  set_tests_properties(c86.mutation.window.no_status_segments PROPERTIES TIMEOUT 240
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: no_status_segments")

  # D3 control 5: a maximized window's rectangle is persisted as the normal
  # frame -- the 1552x832 trap C66 closed and this card must not re-open.
  add_executable(win_c86_window_mutant_maximized_as_normal win/test_c86_window.cpp)
  target_link_libraries(win_c86_window_mutant_maximized_as_normal
                        PRIVATE ${PRISM_C86_WINDOW_LIBS})
  target_compile_options(win_c86_window_mutant_maximized_as_normal PRIVATE /WX-)
  target_compile_definitions(win_c86_window_mutant_maximized_as_normal
                             PRIVATE PRISM_C86_MUTANT_MAXIMIZED_AS_NORMAL=1)
  add_test(NAME "c86.mutation.window.maximized_as_normal"
           COMMAND win_c86_window_mutant_maximized_as_normal)
  set_tests_properties(c86.mutation.window.maximized_as_normal PROPERTIES TIMEOUT 240
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: maximized_as_normal")

  # D3 control 6: the crumb shows the archive total again inside a folder.
  add_executable(win_c86_window_mutant_archive_count win/test_c86_window.cpp)
  target_link_libraries(win_c86_window_mutant_archive_count PRIVATE ${PRISM_C86_WINDOW_LIBS})
  target_compile_options(win_c86_window_mutant_archive_count PRIVATE /WX-)
  target_compile_definitions(win_c86_window_mutant_archive_count
                             PRIVATE PRISM_C86_MUTANT_ARCHIVE_COUNT=1)
  add_test(NAME "c86.mutation.window.archive_count"
           COMMAND win_c86_window_mutant_archive_count)
  set_tests_properties(c86.mutation.window.archive_count PROPERTIES TIMEOUT 240
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C86 MUTATION CAUGHT: archive_count")
endif()
