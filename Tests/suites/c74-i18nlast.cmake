# c74-i18nlast: the last English/Chinese literals leave the product code.
#
# WHAT WAS STILL BROKEN (C69's own report, docs/USAGE.md):
#   * the action island's eight labels came from a COMPILE-TIME Chinese table in
#     PrismCore/ActionBarModel.hpp, so a language switch changed the menu and the
#     Settings rows but left the band in Chinese;
#   * the sidebar's telemetry capsule (NOT ENCRYPTED / UNKNOWN | DICT -- /
#     RATIO 53.6%) was literals in SidebarView.hpp -- English in a Chinese build;
#   * the Settings dialog's summary line and in-place hints were English
#     diagnostics built by DialogState, outside the resource table.
#
# THE VERDICT IS WORDS AND PIXELS, NEVER A COMMENT:
#   core.actionbar_labels   the model: keys, injected English/Chinese tables, and
#                           the no-table fallback (PrismCore never includes the
#                           translator). Runs on every lane.
#   win.i18n_switch         a REAL GalleryWindow: ApplyLanguage() must change the
#                           eight labels read back from the band, the sidebar's
#                           ARCHIVE TREE heading, the capsule's three lines, and
#                           the label PIXELS rendered through GalleryRenderer.
#
# NEGATIVE CONTROLS (CONTRACTS D3 + D26 + D27/D28): each mutant compiles the SAME
# source with one rule removed and prints its "C74 MUTATION CAUGHT: <arm>" marker
# at the FIRST red assertion, only when the injection is proved active. No
# WILL_FAIL is stacked on a PASS_REGULAR_EXPRESSION (D26); a stale injection
# prints nothing and leaves the mutant entry RED.
#
# The Windows block comes FIRST: tools/preflight.py's D1 gate binds a source file
# to the FIRST add_executable that mentions it and requires the PASS regex there.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  set(PRISM_C74_LANG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../Assets/Lang")
  if(NOT EXISTS "${PRISM_C74_LANG_DIR}/en-US.json" OR
     NOT EXISTS "${PRISM_C74_LANG_DIR}/zh-CN.json")
    message(FATAL_ERROR "c74-i18nlast: Assets/Lang/{en-US,zh-CN}.json are missing")
  endif()

  set(PRISM_C74_LIBS prism_core prism_win_bridge user32.lib gdi32.lib shell32.lib
                     ole32.lib windowscodecs.lib d2d1.lib dwrite.lib)

  add_executable(win_c74_i18n_switch win/test_c74_i18n_switch.cpp)
  target_link_libraries(win_c74_i18n_switch PRIVATE ${PRISM_C74_LIBS})
  target_compile_options(win_c74_i18n_switch PRIVATE /WX-)
  target_compile_definitions(win_c74_i18n_switch PRIVATE
      "PRISM_C74_LANG_DIR=\"${PRISM_C74_LANG_DIR}\"")
  add_test(NAME "win.i18n_switch" COMMAND win_c74_i18n_switch)
  set_tests_properties(win.i18n_switch PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL C74 I18N-SWITCH CHECKS PASSED")

  # D3 control: ApplyLanguage() refreshes the sidebar but never rebuilds the
  # band's labels -- the exact pre-C74 defect.
  add_executable(win_c74_i18n_switch_mutant_no_label_rebuild win/test_c74_i18n_switch.cpp)
  target_link_libraries(win_c74_i18n_switch_mutant_no_label_rebuild PRIVATE ${PRISM_C74_LIBS})
  target_compile_options(win_c74_i18n_switch_mutant_no_label_rebuild PRIVATE /WX-)
  target_compile_definitions(win_c74_i18n_switch_mutant_no_label_rebuild PRIVATE
      "PRISM_C74_LANG_DIR=\"${PRISM_C74_LANG_DIR}\""
      PRISM_C74_MUTANT_NO_LABEL_REBUILD=1)
  add_test(NAME "c74.mutation.i18n_switch.no_label_rebuild"
           COMMAND win_c74_i18n_switch_mutant_no_label_rebuild)
  set_tests_properties(c74.mutation.i18n_switch.no_label_rebuild PROPERTIES TIMEOUT 180
      SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "C74 MUTATION CAUGHT: no_label_rebuild")
endif()

# The model contract: every lane, no window.
add_executable(test_c74_actionbar_labels test_c74_actionbar_labels.cpp)
target_link_libraries(test_c74_actionbar_labels PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_c74_actionbar_labels PRIVATE /WX-)
endif()
add_test(NAME "core.actionbar_labels" COMMAND test_c74_actionbar_labels)
set_tests_properties(core.actionbar_labels PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "ALL C74 ACTIONBAR-LABEL CHECKS PASSED")

# D3 control: SetLabels() is ignored, so every lookup returns the built-in
# Chinese word. The injected English/Chinese cases MUST go red.
add_executable(test_c74_actionbar_labels_mutant_fixed_labels test_c74_actionbar_labels.cpp)
target_link_libraries(test_c74_actionbar_labels_mutant_fixed_labels PRIVATE prism_core)
if(WIN32)
  target_compile_options(test_c74_actionbar_labels_mutant_fixed_labels PRIVATE /WX-)
endif()
target_compile_definitions(test_c74_actionbar_labels_mutant_fixed_labels
                           PRIVATE PRISM_C74_MUTANT_FIXED_LABELS=1)
add_test(NAME "c74.mutation.actionbar_labels.fixed_labels"
         COMMAND test_c74_actionbar_labels_mutant_fixed_labels)
set_tests_properties(c74.mutation.actionbar_labels.fixed_labels PROPERTIES TIMEOUT 120
    PASS_REGULAR_EXPRESSION "C74 MUTATION CAUGHT: fixed_labels")
