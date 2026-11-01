if(WIN32 AND PRISM_BUILD_WIN_LAYER)
# c20-dragout: the drag-out chain (blueprint ch. 4.4 / ch. 8.1), real 7z.dll.
#
# The test builds a genuine SOLID 7z with the real 7z.exe, turns a catalog
# selection into an IDataObject, enumerates its formats, parses the
# FILEDESCRIPTORW blob back, reads every CFSTR_FILECONTENTS stream and compares
# SHA-256 with a DIRECT extraction -- then seeks through the middle of a solid
# item and back, and finally proves %TEMP% stayed empty (trap directory plus a
# recursive archive-dir snapshot).
#
# One card = one file; see suites/README.md. Tests/CMakeLists.txt is untouched.
#
# D3 negative controls: the SAME source compiled with one rule broken. WILL_FAIL
# makes CTest green only while the suite goes red on it, so a mutation that
# stopped matching would fail here instead of silently passing.
add_executable(win_dragout_test win/test_dragout.cpp)
target_link_libraries(win_dragout_test PRIVATE prism_core prism_win_bridge
                      user32.lib gdi32.lib ole32.lib oleaut32.lib
                      windowscodecs.lib bcrypt.lib)
# /WX- : the Windows SDK headers are not warning-clean under /W4 (contract B9).
target_compile_options(win_dragout_test PRIVATE /WX-)
add_test(NAME "win.dragout" COMMAND win_dragout_test)
set_tests_properties(win.dragout PROPERTIES
    PASS_REGULAR_EXPRESSION "ALL DRAG-OUT CHECKS PASSED"
    TIMEOUT 300
    SKIP_RETURN_CODE 77)
# The marker above is a CONJUNCTION with the exit code: a silent skip, a crash
# after printing, or a suite that stopped asserting can never look green (D1).
# SKIP_RETURN_CODE 77 is the LABELLED skip: the test explains why it cannot run
# (no real 7z.dll / 7z.exe) before exiting with 77, so CTest reports Skipped and
# the reason stays in the log instead of inventing a pass.

# D3 control #1: GetData answers E_NOTIMPL (the "wired up" stub).
add_executable(win_dragout_mutant_getdata_notimpl win/test_dragout.cpp)
target_link_libraries(win_dragout_mutant_getdata_notimpl PRIVATE prism_core prism_win_bridge
                      user32.lib gdi32.lib ole32.lib oleaut32.lib
                      windowscodecs.lib bcrypt.lib)
target_compile_options(win_dragout_mutant_getdata_notimpl PRIVATE /WX-)
target_compile_definitions(win_dragout_mutant_getdata_notimpl
                           PRIVATE PRISM_C20_MUTANT_GETDATA_NOTIMPL=1)
add_test(NAME "mutation.dragout.getdata_notimpl"
         COMMAND win_dragout_mutant_getdata_notimpl)
set_tests_properties(mutation.dragout.getdata_notimpl PROPERTIES WILL_FAIL TRUE TIMEOUT 300)

# D3 control #2: the IStream claims one byte more than the decoder delivers.
add_executable(win_dragout_mutant_size_lie win/test_dragout.cpp)
target_link_libraries(win_dragout_mutant_size_lie PRIVATE prism_core prism_win_bridge
                      user32.lib gdi32.lib ole32.lib oleaut32.lib
                      windowscodecs.lib bcrypt.lib)
target_compile_options(win_dragout_mutant_size_lie PRIVATE /WX-)
target_compile_definitions(win_dragout_mutant_size_lie PRIVATE PRISM_C20_MUTANT_SIZE_LIE=1)
add_test(NAME "mutation.dragout.size_lie" COMMAND win_dragout_mutant_size_lie)
set_tests_properties(mutation.dragout.size_lie PROPERTIES WILL_FAIL TRUE TIMEOUT 300)
endif()
