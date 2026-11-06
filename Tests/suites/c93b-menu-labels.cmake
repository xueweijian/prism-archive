# c93b-menu-labels: the menu bar must not mix languages.
#
# Measured on the installed window (2026-11-05): a zh-CN menu bar read
#   文件  编辑  设置(S)...  Help
# -- four labels from BuildMenuLabels and the fifth from GalleryWindow's
# L"Help" default, because SetAboutMenuLabels was never called. This suite
# pins all five labels to the one source the app already uses.

# This suite compiles Src/App/AppWiring.hpp, which includes <windows.h>: it can
# only exist on the Windows lane. Every other win suite carries the same guard
# (c104 line 25); without it the Linux lane tries to build it and dies on
# "windows.h: No such file or directory" (measured on PR #132).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)

add_executable(win_c93b_menu_labels win/test_c93b_menu_labels.cpp)
target_link_libraries(win_c93b_menu_labels PRIVATE prism_core prism_win_bridge
  prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib
  oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib)
target_include_directories(win_c93b_menu_labels PRIVATE ${CMAKE_SOURCE_DIR}/Src/App ${CMAKE_SOURCE_DIR}/Tests)
add_test(NAME "win.c93b_menu_labels" COMMAND win_c93b_menu_labels)
set_tests_properties(win.c93b_menu_labels PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "ALL C93B MENU-LABEL CHECKS PASSED")

# Negative control (D3): the pre-fix behaviour is "the Help labels are never
# assigned", so the zh-CN assertion must go red and the harness must say CAUGHT.
add_executable(win_c93b_menu_labels_mutant_no_help win/test_c93b_menu_labels.cpp)
target_link_libraries(win_c93b_menu_labels_mutant_no_help PRIVATE prism_core
  prism_win_bridge prism_shell_contract user32.lib gdi32.lib shell32.lib ole32.lib
  oleaut32.lib windowscodecs.lib d2d1.lib dwrite.lib bcrypt.lib)
target_compile_definitions(win_c93b_menu_labels_mutant_no_help PRIVATE
  PRISM_C93B_MUTANT_NO_HELP)
target_include_directories(win_c93b_menu_labels_mutant_no_help PRIVATE ${CMAKE_SOURCE_DIR}/Src/App)
add_test(NAME "mutation.c93b.no_help" COMMAND win_c93b_menu_labels_mutant_no_help)
set_tests_properties(mutation.c93b.no_help PROPERTIES TIMEOUT 60
  PASS_REGULAR_EXPRESSION "C93B MUTATION CAUGHT: no_help")

endif()
