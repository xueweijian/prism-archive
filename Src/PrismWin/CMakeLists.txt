# Windows-only sources. Built on windows-latest CI (MSVC 19.x, C++20).
add_library(prism_win_bridge INTERFACE)
target_include_directories(prism_win_bridge INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(prism_win_bridge INTERFACE prism_core)
target_link_libraries(prism_win_bridge INTERFACE shlwapi.lib)

# The RC language has to be enabled explicitly: project() declares CXX only, and
# the Ninja generator has no rule for .rc without it. Windows-only file, so this
# lives here rather than in the top-level CMakeLists.txt.
enable_language(RC)

# The runnable application (batch A): thin glue over proven parts. The entry
# point lives in Src/App; the include root is Src so it reaches both layers.
# PrismArchive.rc carries the icon + VERSIONINFO (installer/Explorer read both).
add_executable(PrismArchive ../App/Main.cpp ../App/PrismArchive.rc)
target_include_directories(PrismArchive PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(PrismArchive PRIVATE prism_win_bridge
                      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                      ole32.lib comdlg32.lib shell32.lib shlwapi.lib
                      # comctl32: the compression dialog's level slider is a
                      # msctls_trackbar32 common control (C18). Without it the
                      # link fails only once the dialog is actually referenced.
                      comctl32.lib)
# GUI subsystem + UNICODE: wWinMain is only selected when UNICODE is defined
# (otherwise the CRT wants plain main). The other TUs use explicit W APIs, so
# the define changes nothing for them.
set_target_properties(PrismArchive PROPERTIES WIN32_EXECUTABLE TRUE)
target_compile_definitions(PrismArchive PRIVATE UNICODE _UNICODE)

# ---------------------------------------------------------------------------
# C120: PER-MONITOR V2 (docs/UI-AUDIT-BANDIZIP.md: the process shipped as
# DPI-UNAWARE, so 125% monitors bitmap-scaled the whole window by 1.25x and
# every glyph and 1 px hairline was resampled).
#
# The manifest is MERGED into the one link.exe generates (/MANIFESTINPUT)
# rather than embedded from the .rc file: the generated half carries the
# requestedExecutionLevel the installer and the shell verbs rely on, and an
# .rc-provided RT_MANIFEST would collide with it (CVT1100: duplicate resource,
# type MANIFEST, name 1). The linker is also the component that owns the
# manifest, so this is where the declaration belongs.
#
# The runtime half of the same decision is Src/PrismWin/DpiAwareness.hpp, called
# first in wWinMain. Keep the two in step: the manifest is what a fresh
# ProcessStart sees, the header is what a rebuilt/renamed binary still gets.
# ---------------------------------------------------------------------------
if(MSVC)
  target_link_options(PrismArchive PRIVATE
    "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/../App/PrismArchive.manifest")
endif()
# /WX- : same rationale as the other Windows targets -- the SDK headers are
# not warning-clean under /W4 (the renderer's float<->pixel conversions are
# exactness-benign by construction; cast-polish is deferred, not forgotten).
target_compile_options(PrismArchive PRIVATE /WX-)

# ---------------------------------------------------------------------------
# C80 negative control (CONTRACTS D3): the SAME application with the writable
# search value disarmed (put_accValue -> E_NOTIMPL, the pre-fix state the UIA
# driver reported as "POLICY_DENIED: supported_action"). The process-level
# accessibility suite (Tests/suites/c80-a11y.cmake) drives THIS binary and must
# observe the refusal; a build that quietly accepts the write leaves that target
# red. Never installed -- it exists to prove the assertion is the thing that
# catches the defect.
# ---------------------------------------------------------------------------
add_executable(PrismArchive_c80_readonly ../App/Main.cpp ../App/PrismArchive.rc)
target_include_directories(PrismArchive_c80_readonly PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(PrismArchive_c80_readonly PRIVATE prism_win_bridge
                      user32.lib gdi32.lib d2d1.lib dwrite.lib windowscodecs.lib
                      ole32.lib comdlg32.lib shell32.lib shlwapi.lib
                      comctl32.lib)
set_target_properties(PrismArchive_c80_readonly PROPERTIES WIN32_EXECUTABLE TRUE)
target_compile_definitions(PrismArchive_c80_readonly PRIVATE UNICODE _UNICODE
                           PRISM_C80_MUTANT_READONLY_SEARCH=1)
target_compile_options(PrismArchive_c80_readonly PRIVATE /WX-)

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

# ---------------------------------------------------------------------------
# Install layout: a flat per-user folder, the shape tools/install.ps1 deploys and
# Installer/prism.iss packages. "cmake --install" therefore produces exactly the
# same tree the installers copy, which is what makes the payload reviewable
# without running either of them.
#    <prefix>/PrismArchive.exe
#    <prefix>/PrismShellExt.dll
#    <prefix>/PrismShellExtClassic.dll
#    <prefix>/Assets/**            (shaders + icon; the generator .py stays source)
#    <prefix>/install.ps1
#    <prefix>/uninstall.ps1
# ---------------------------------------------------------------------------
set(PRISM_ASSETS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../Assets")
set(PRISM_TOOLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../tools")

install(TARGETS PrismArchive RUNTIME DESTINATION .)
install(TARGETS PrismShellExt PrismShellExtClassic
        RUNTIME DESTINATION . LIBRARY DESTINATION .)
# 7z.dll is deliberately absent: LGPL engine, discovered at runtime (blueprint ch.2).
install(DIRECTORY "${PRISM_ASSETS_DIR}/" DESTINATION Assets
        PATTERN "*.py" EXCLUDE)
# register-static-verbs.ps1 is not optional chrome: install.ps1 calls it from
# its own directory to write the two right-click verbs, and the zip must carry
# it or "unzip, run install.ps1" silently installs without a context menu.
install(FILES "${PRISM_TOOLS_DIR}/install.ps1"
              "${PRISM_TOOLS_DIR}/uninstall.ps1"
              "${PRISM_TOOLS_DIR}/register-static-verbs.ps1"
        DESTINATION .)
# ---------------------------------------------------------------------------
# C76: pre-compile the Liquid Glass HLSL to .cso bytecode AT BUILD TIME.
#
# Measured before this card (docs/PERF.md, C76_BREAKDOWN): the two D3DCompile
# calls in GlassEffectRuntime::Initialize cost 80.9 ms of the first frame. The
# shipped bytes are the same shader with the same entry point, profile and
# optimisation level, so the runtime only has to read them.
#
# fxc.exe ships with the Windows SDK. When it is absent nothing is generated
# and GlassEffectRuntime falls back to D3DCompile (slower, never broken) -- a
# checkout without the SDK must still render the glass.
# ---------------------------------------------------------------------------
set(PRISM_FXC "")
if(WIN32)
  file(GLOB PRISM_FXC_CANDIDATES "C:/Program Files (x86)/Windows Kits/10/bin/*/x64/fxc.exe")
  if(PRISM_FXC_CANDIDATES)
    list(GET PRISM_FXC_CANDIDATES -1 PRISM_FXC)
  endif()
endif()
set(PRISM_CSO_DIR "${CMAKE_BINARY_DIR}/Assets/Shaders")
if(PRISM_FXC)
  set(PRISM_CSO_FILES
      "${PRISM_CSO_DIR}/LiquidGlassEffect.cso"
      "${PRISM_CSO_DIR}/KawaseDualBlur.D2DEffect.cso")
  add_custom_command(
    OUTPUT ${PRISM_CSO_FILES}
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PRISM_CSO_DIR}"
    COMMAND "${PRISM_FXC}" /nologo /T ps_5_0 /E main /O3
            "/Fo${PRISM_CSO_DIR}/LiquidGlassEffect.cso"
            "${PRISM_ASSETS_DIR}/Shaders/LiquidGlassEffect.hlsl"
    COMMAND "${PRISM_FXC}" /nologo /T ps_5_0 /E main /O3 /DPRISM_D2D_EFFECT=1
            "/Fo${PRISM_CSO_DIR}/KawaseDualBlur.D2DEffect.cso"
            "${PRISM_ASSETS_DIR}/Shaders/KawaseDualBlur.hlsl"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PRISM_ASSETS_DIR}/Shaders/LiquidGlassEffect.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/KawaseDualBlur.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/LiquidGlassMath.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/PrismD2DEffectEntry.hlsli"
            "${PRISM_CSO_DIR}"
    DEPENDS "${PRISM_ASSETS_DIR}/Shaders/LiquidGlassEffect.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/KawaseDualBlur.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/LiquidGlassMath.hlsl"
            "${PRISM_ASSETS_DIR}/Shaders/PrismD2DEffectEntry.hlsli"
    COMMENT "C76: pre-compiling the glass shaders (.cso)")
  add_custom_target(prism_glass_shaders DEPENDS ${PRISM_CSO_FILES})
  add_dependencies(PrismArchive prism_glass_shaders)
  install(FILES ${PRISM_CSO_FILES} DESTINATION Assets/Shaders OPTIONAL)
  message(STATUS "C76: glass shaders pre-compiled with ${PRISM_FXC}")
else()
  message(STATUS "C76: fxc.exe not found -- glass shaders compile at runtime")
endif()
