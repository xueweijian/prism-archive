cmake_minimum_required(VERSION 3.20)

project(PrismArchive
        VERSION 0.1.0
        DESCRIPTION "7-Zip engine x Bandizip interaction x Apple Liquid Glass optics"
        LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PRISM_BUILD_TESTS "Build the PrismCore unit-test suite" ON)
option(PRISM_BUILD_WIN_LAYER "Build Windows-only sources (7z bridge, shell ext)" ${WIN32})
option(PRISM_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ON)

# ---------------------------------------------------------------------------
# PrismCore: header-only, platform-neutral brain of the product.
# Everything in here must compile on Linux/macOS/Windows with zero Windows or
# 7-Zip headers -- that is what makes the rules unit-testable in CI.
# ---------------------------------------------------------------------------
add_library(prism_core INTERFACE)
target_include_directories(prism_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/Src/PrismCore)

# Warning policy is global, not on the INTERFACE target: consumers of
# prism_core (shell ext, integration test) compile against Windows SDK headers
# that are not /W4-clean, and escalating *their* warnings would be noise.
if(MSVC)
  # /bigobj: the ASan MSVC lane compiles Main.cpp into more sections than the
  # default COFF limit allows (C1128). This is a compiler capability flag, not a
  # warning-policy relaxation: /W3 /WX stay exactly as they were.
  add_compile_options(/W3 /permissive- /utf-8 /D_CRT_SECURE_NO_WARNINGS /bigobj)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(PRISM_WARNINGS_AS_ERRORS)
  if(MSVC)
    add_compile_options(/WX)
  else()
    add_compile_options(-Werror)
  endif()
endif()

# ---------------------------------------------------------------------------
# Windows-only layer: real 7z.dll bridge, IExplorerCommand shell extension,
# zero-copy drag & drop, DPAPI vault, MoTW propagation.
# ---------------------------------------------------------------------------
if(PRISM_BUILD_WIN_LAYER)
  if(NOT WIN32)
    message(FATAL_ERROR "PRISM_BUILD_WIN_LAYER requires a Windows host")
  endif()
  add_subdirectory(Src/PrismWin)
endif()

if(PRISM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(Tests)
endif()

# HLSL assets are compiled by CI (fxc.exe on windows-latest); see
# .github/workflows/ci.yml and tools/hlsl/compile_hlsl.py.
install(DIRECTORY Assets/Shaders DESTINATION share/prism-archive OPTIONAL)
