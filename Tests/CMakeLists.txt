# PrismCore unit tests: one executable per suite, registered with CTest.
set(PRISM_CORE_TESTS
    test_path_safety.cpp
    test_smart_extractor.cpp
    test_charset_repair.cpp
    test_virtual_tree.cpp
    test_security_guard.cpp)

foreach(source IN LISTS PRISM_CORE_TESTS)
  get_filename_component(name "${source}" NAME_WE)
  add_executable("${name}" "${source}")
  target_link_libraries("${name}" PRIVATE prism_core)
  add_test(NAME "core.${name}" COMMAND "${name}")
endforeach()

# Windows-only integration test: loads the real 7z.dll and round-trips an
# archive created by the 7-Zip CLI. Skipped (not built) on other platforms.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_engine_bridge_test win/test_engine_bridge.cpp)
  target_link_libraries(win_engine_bridge_test PRIVATE prism_core prism_win_bridge)
  add_test(NAME "win.engine_bridge" COMMAND win_engine_bridge_test)
endif()
