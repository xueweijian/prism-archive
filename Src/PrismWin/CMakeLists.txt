# Windows-only sources. Built on windows-latest CI (MSVC 19.x, C++20).
add_library(prism_win_bridge INTERFACE)
target_include_directories(prism_win_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(prism_win_bridge INTERFACE prism_core)
target_link_libraries(prism_win_bridge INTERFACE shlwapi.lib)

# Win11 modern context menu (IExplorerCommand). Needs package identity at
# runtime (MSIX sparse package) but compiles and links standalone.
add_library(PrismShellExt SHARED PrismShellCommand.cpp)
target_link_libraries(PrismShellExt PRIVATE prism_core shlwapi.lib)
# /WX- : Windows SDK headers are not warning-clean under /W4 /WX.
target_compile_options(PrismShellExt PRIVATE /WX-)
set_target_properties(PrismShellExt PROPERTIES OUTPUT_NAME "PrismShellExt")
if(MSVC)
  set_target_properties(PrismShellExt PROPERTIES
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/PrismShellExt.def")
endif()
