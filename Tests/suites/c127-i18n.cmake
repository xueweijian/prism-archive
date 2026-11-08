# c127-i18n: the info card speaks ONE language, and the title follows Windows
# order (card C127).
#
# The two measured defects:
#   1. the info card's four field labels were English ("ENCRYPTION / METHOD /
#      DICTIONARY / RATIO") above Chinese values -- two languages in one card;
#   2. the window caption was "PrismArchive -- fixture.zip" instead of
#      "fixture.zip - PrismArchive".
#
# WHY THIS SUITE IS CROSS-PLATFORM (core lane, no WIN32 guard)
#   I1 builds the SHIPPING SidebarView layout and reads its StatusLabel
#   commands; I2 calls the pure WindowTitle rule; I3 reads RequiredKeys() and
#   both JSON tables. None of the three needs a window, Direct2D or the Windows
#   SDK -- exactly like C124's core lane. That is deliberate: the census half is
#   the one that must also fail on the Linux lane when a table forgets a key
#   (D35: CI is the verdict, both platforms).
#
# NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C127_MUTANT_HARDCODED_LABEL
# compiles LabelsFrom with the English defaults restored. I1 MUST go red, and the
# CTest entry requires the "C127 MUTATION CAUGHT: hardcoded_label" marker (not
# WILL_FAIL). The marker is printed at the first failing check and only when the
# injected branch is active, so an injection that stopped firing leaves the
# entry RED instead of vacuously green.
set(PRISM_C127_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(PRISM_C127_LANG_DIR "${PRISM_C127_SOURCE_DIR}/Assets/Lang")

# D1: the inputs are compile-time, so a moved tree breaks CONFIGURE instead of
# passing a suite that read nothing.
if(NOT EXISTS "${PRISM_C127_SOURCE_DIR}/Src/PrismCore/WindowTitle.hpp")
  message(FATAL_ERROR "c127-i18n: WindowTitle.hpp is missing; I2 would assert nothing")
endif()
if(NOT EXISTS "${PRISM_C127_SOURCE_DIR}/Src/PrismWin/SidebarView.hpp")
  message(FATAL_ERROR "c127-i18n: SidebarView.hpp is missing; I1 would assert nothing")
endif()
if(NOT EXISTS "${PRISM_C127_LANG_DIR}/en-US.json" OR
   NOT EXISTS "${PRISM_C127_LANG_DIR}/zh-CN.json")
  message(FATAL_ERROR "c127-i18n: Assets/Lang/{en-US,zh-CN}.json are missing")
endif()

add_executable(core_c127_i18n test_c127_i18n.cpp)
target_link_libraries(core_c127_i18n PRIVATE prism_core)
# SidebarView.hpp is the Windows layer's header, but it is dependency-free
# (no windows.h / D2D): the card's labels are a pure command list. Including
# the directory keeps I1 reading the SHIPPING header instead of a copy.
target_include_directories(core_c127_i18n PRIVATE "${PRISM_C127_SOURCE_DIR}/Src/PrismWin")
target_compile_definitions(core_c127_i18n PRIVATE
    "PRISM_C127_LANG_DIR=\"${PRISM_C127_LANG_DIR}\""
    "PRISM_C127_SOURCE_DIR=\"${PRISM_C127_SOURCE_DIR}\"")
add_test(NAME "core.c127_i18n" COMMAND core_c127_i18n)
set_tests_properties(core.c127_i18n PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C127 I18N CHECKS PASSED")

# The same source with the English defaults compiled back in.
add_executable(core_c127_i18n_mutant_hardcoded_label test_c127_i18n.cpp)
target_link_libraries(core_c127_i18n_mutant_hardcoded_label PRIVATE prism_core)
target_include_directories(core_c127_i18n_mutant_hardcoded_label PRIVATE
    "${PRISM_C127_SOURCE_DIR}/Src/PrismWin")
target_compile_definitions(core_c127_i18n_mutant_hardcoded_label PRIVATE
    PRISM_C127_MUTANT_HARDCODED_LABEL=1
    "PRISM_C127_LANG_DIR=\"${PRISM_C127_LANG_DIR}\""
    "PRISM_C127_SOURCE_DIR=\"${PRISM_C127_SOURCE_DIR}\"")
add_test(NAME "mutation.c127.hardcoded_label"
         COMMAND core_c127_i18n_mutant_hardcoded_label)
set_tests_properties(mutation.c127.hardcoded_label PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C127 MUTATION CAUGHT: hardcoded_label")

# ---------------------------------------------------------------------------
# The Windows half: the SAME real window path the screen uses. The command-list
# assertion (I1) can be green while the painter draws something else -- R1's
# "tests are not the product". This case renders a GalleryWindow frame and
# measures the first card label's INK WIDTH against the widths the same
# DirectWrite format gives for 加密 vs ENCRYPTION, then repeats with a table
# that lacks the card keys (the stale-install screenshot).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C127_WIN_LIBS prism_core prism_win_bridge user32.lib gdi32.lib shell32.lib
      ole32.lib windowscodecs.lib d2d1.lib dwrite.lib)

  add_executable(win_c127_card_pixels win/test_c127_card_pixels.cpp)
  target_link_libraries(win_c127_card_pixels PRIVATE ${PRISM_C127_WIN_LIBS})
  target_compile_options(win_c127_card_pixels PRIVATE /WX-)  # SDK headers are not /W4
  target_compile_definitions(win_c127_card_pixels PRIVATE
      "PRISM_C127_LANG_DIR=\"${PRISM_C127_LANG_DIR}\"")
  add_test(NAME "win.c127_card_pixels" COMMAND win_c127_card_pixels)
  set_tests_properties(win.c127_card_pixels PROPERTIES TIMEOUT 240 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C127 CARD-PIXEL CHECKS PASSED")
endif()
