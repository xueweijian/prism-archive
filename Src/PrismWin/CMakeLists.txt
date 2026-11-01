# Windows-only sources. Built on windows-latest CI (MSVC 19.x, C++20).
add_library(prism_win_bridge INTERFACE)
target_include_directories(prism_win_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(prism_win_bridge INTERFACE prism_core)
target_link_libraries(prism_win_bridge INTERFACE shlwapi.lib)

# The runnable application (batch A): thin glue over proven parts. The entry
# point lives in Src/App; the include root is Src so it reaches both layers.
add_executable(PrismArchive ../App/Main.cpp)
target_include_directories(PrismArchive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(PrismArchive PRIVATE prism_win_bridge
                      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                      ole32.lib comdlg32.lib shell32.lib shlwapi.lib)
# GUI subsystem + UNICODE: wWinMain is only selected when UNICODE is defined
# (otherwise the CRT wants plain main). The other TUs use explicit W APIs, so
# the define changes nothing for them.
set_target_properties(PrismArchive PROPERTIES WIN32_EXECUTABLE TRUE)
target_compile_definitions(PrismArchive PRIVATE UNICODE _UNICODE)
# /WX- : same rationale as the other Windows targets -- the SDK headers are
# not warning-clean under /W4 (the renderer's float<->pixel conversions are
# exactness-benign by construction; cast-polish is deferred, not forgotten).
target_compile_options(PrismArchive PRIVATE /WX-)

# Win11 modern context menu (IExplorerCommand). Needs package identity at
# runtime (MSIX sparse package) but compiles and links standalone.
add_library(PrismShellExt SHARED PrismShellCommand.cpp)
target_link_libraries(PrismShellExt PRIVATE prism_core prism_shell_contract shlwapi.lib)
# /WX- : Windows SDK headers are not warning-clean under /W4 /WX.
target_compile_options(PrismShellExt PRIVATE /WX-)
set_target_properties(PrismShellExt PROPERTIES OUTPUT_NAME "PrismShellExt")
if(MSVC)
  set_target_properties(PrismShellExt PROPERTIES
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/PrismShellExt.def")
endif()

# Win10 classic IContextMenu layer (separate DLL: its own CLSID + exports).
add_library(PrismShellExtClassic SHARED PrismClassicContextMenu.cpp)
target_link_libraries(PrismShellExtClassic PRIVATE prism_core prism_shell_contract shlwapi.lib shell32.lib)
target_compile_options(PrismShellExtClassic PRIVATE /WX-)
set_target_properties(PrismShellExtClassic PROPERTIES OUTPUT_NAME "PrismShellExtClassic")
if(MSVC)
  set_target_properties(PrismShellExtClassic PROPERTIES
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/PrismClassic.def")
endif()
# ---------------------------------------------------------------------------
# Shell registration contract (card C4, APPENDED). One TU owns:
#   * the CLSIDs both shell DLLs export (must match AppxManifest.xml),
#   * the verb -> CLI flag table (must match the c1/c3 launcher flags),
#   * the sparse-package deploy / verify / unregister path (C++/WinRT).
# It is a static library, not a header-only helper, because the WinRT half needs
# WindowsApp.lib -- the pure decision layer must never pull that in.
# ---------------------------------------------------------------------------
add_library(prism_shell_contract STATIC SparsePackageRegistrar.cpp)
target_include_directories(prism_shell_contract PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(prism_shell_contract PUBLIC WindowsApp.lib shlwapi.lib
                      advapi32.lib shell32.lib ole32.lib)
# /WX- : same rationale as the other Windows targets (SDK headers are not
# warning-clean under /W4).
target_compile_options(prism_shell_contract PRIVATE /WX-)
