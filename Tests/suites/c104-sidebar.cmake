# c104-sidebar: the sidebar goes macOS -- capsule selection + soft folder
# (plan 3.1). One card = one file (Tests/suites/README.md); Tests/CMakeLists.txt
# is untouched.
#
# THE DEFECT. The selected row was a hard-edged light-blue RECTANGLE and the
# folder icon an opaque accent plate -- the user's "Windows 98/XP Explorer".
# Both are painted by GalleryRenderer, so both fixes are verified on PIXELS from
# the real renderer into a headless WIC target:
#   * a capsule's corner pixel is background while its centre is the wash; a
#     square fill makes the two identical, which is the shipped defect;
#   * the wash stays under 15% in Daylight and Midnight (Midnight used to be
#     16%), with the HighContrast accessibility floor pinned separately;
#   * the folder glyph is a two-tone, translucent, panel-lifted accent -- and
#     neither the view nor its painter contains the literal #007AFF;
#   * one indent step is 14..16 px and the emitted rows really move by it.
#
# The clean target's marker is a CONJUNCTION with the exit code (D1): a crash
# after the checks or a silent early return can never count as a pass.
#
# NEGATIVE CONTROLS (D3/D26/D27): no WILL_FAIL anywhere. Each mutant compiles
# the SAME source with exactly one rule broken, arms mini_test's verdict on the
# renderer's own proof that the injected branch ran, and is registered with
# PASS_REGULAR_EXPRESSION on its marker -- so an unrelated red run stays red
# instead of printing a false CAUGHT.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(C104_WIN_LIBS prism_core prism_win_bridge user32.lib gdi32.lib d2d1.lib
      dwrite.lib windowscodecs.lib ole32.lib)

  add_executable(win_c104_sidebar win/test_c104_sidebar.cpp)
  target_link_libraries(win_c104_sidebar PRIVATE ${C104_WIN_LIBS})
  target_compile_options(win_c104_sidebar PRIVATE /WX-)
  # The "no literal #007AFF" half of assertion 3 scans the SHIPPED headers, so
  # the test needs the repo root. A hardcoded path would pass on one machine.
  target_compile_definitions(win_c104_sidebar PRIVATE
                             PRISM_C104_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME "win.c104_sidebar" COMMAND win_c104_sidebar)
  set_tests_properties(win.c104_sidebar PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "ALL C104 SIDEBAR CHECKS PASSED")

  # D3 control 1: the pre-C104 hard-edged rectangle. The corner-versus-centre
  # assertion MUST go red.
  add_executable(win_c104_sidebar_mutant_square win/test_c104_sidebar.cpp)
  target_link_libraries(win_c104_sidebar_mutant_square PRIVATE ${C104_WIN_LIBS})
  target_compile_options(win_c104_sidebar_mutant_square PRIVATE /WX-)
  target_compile_definitions(win_c104_sidebar_mutant_square PRIVATE
                             PRISM_C104_MUTANT_SQUARE=1
                             PRISM_C104_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME "c104.mutation.sidebar.square"
           COMMAND win_c104_sidebar_mutant_square)
  set_tests_properties(c104.mutation.sidebar.square PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "C104 MUTATION CAUGHT: square")

  # D3 control 2: the opaque accent plate for the folder. The translucency
  # assertion MUST go red.
  add_executable(win_c104_sidebar_mutant_folder_opaque win/test_c104_sidebar.cpp)
  target_link_libraries(win_c104_sidebar_mutant_folder_opaque
                        PRIVATE ${C104_WIN_LIBS})
  target_compile_options(win_c104_sidebar_mutant_folder_opaque PRIVATE /WX-)
  target_compile_definitions(win_c104_sidebar_mutant_folder_opaque PRIVATE
                             PRISM_C104_MUTANT_FOLDER_OPAQUE=1
                             PRISM_C104_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  add_test(NAME "c104.mutation.sidebar.folder_opaque"
           COMMAND win_c104_sidebar_mutant_folder_opaque)
  set_tests_properties(c104.mutation.sidebar.folder_opaque PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "C104 MUTATION CAUGHT: folder_opaque")
endif()
