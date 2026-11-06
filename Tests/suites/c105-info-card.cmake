# c105-info-card: the INFO CARD and the STATUS BAND (plan 3.2).
#
# The user's report: "左下信息卡有一条硬下划线、标签与值挤在同一行" and "状态栏是
# 六段竖线切割的灰条". This suite renders the REAL GalleryRenderer into a headless
# WIC target and measures the two surfaces:
#   1. no full-width 1 px rule anywhere in the card block (the 12 px of air above
#      the card included -- that is where the old hairline sat);
#   2. the label format is 10 px and the value format is 12 px, read off the LIVE
#      IDWriteTextFormat objects the painter draws with;
#   3. adjacent status segments are at least 16 px apart, read from their rects;
#   4. the band paints no vertical rule column;
#   5. the value run of the ratio segment is the PRIMARY role (>= 20 levels
#      brighter than its label), at the x the painter actually used;
#   5b. the ratio BAR does not intersect the ratio VALUE and sits below it -- the
#      integration build showed the bar drawn straight through "95.0%";
#   6. the four i18n keys this card added are in the census exactly once and the
#      census carries no duplicate (a +N drift cannot hide behind a count).
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative controls (CONTRACTS D3/D26/D27): the SAME source compiled with one
# rule restored or squeezed. The mutant entries use PASS_REGULAR_EXPRESSION on
# the verdict -- NOT WILL_FAIL (D26 forbids combining them) -- and the verdict is
# printed at the FIRST failing check and only when the injected branch is
# compiled in. A clean build never prints it, so an injection that silently
# stopped matching turns its registration red instead of green.
#
# The loop is the repo's conventional shape (foreach IN ITEMS + string(TOLOWER))
# on purpose: tools/mutation/mutant_matrix.py expands exactly that form, and a
# list(GET)-derived name stays literal and is reported as a wiring problem
# (measured: it was, on the first push of this card).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c105_info_card win/test_c105_info_card.cpp)
  target_link_libraries(win_c105_info_card PRIVATE prism_core prism_win_bridge
                                                 user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                 windowscodecs.lib ole32.lib)
  target_compile_options(win_c105_info_card PRIVATE /WX-)
  add_test(NAME "win.c105_info_card" COMMAND win_c105_info_card)
  set_tests_properties(win.c105_info_card PROPERTIES TIMEOUT 240
      PASS_REGULAR_EXPRESSION "ALL C105 INFO-CARD CHECKS PASSED")

  # The verdict names the injection it caught; the regex IS the verdict.
  foreach(mutant IN ITEMS UNDERLINE STATUS_DIVIDER STATUS_TIGHT_GAP BAR_OVER_VALUE)
    string(TOLOWER "${mutant}" mutant_lc)
    add_executable("win_c105_info_card_mutant_${mutant_lc}" win/test_c105_info_card.cpp)
    target_link_libraries("win_c105_info_card_mutant_${mutant_lc}"
                          PRIVATE prism_core prism_win_bridge
                                  user32.lib gdi32.lib d2d1.lib dwrite.lib
                                  windowscodecs.lib ole32.lib)
    target_compile_options("win_c105_info_card_mutant_${mutant_lc}" PRIVATE /WX-)
    target_compile_definitions("win_c105_info_card_mutant_${mutant_lc}"
                               PRIVATE "PRISM_C105_MUTANT_${mutant}=1")
    add_test(NAME "mutation.c105.${mutant_lc}"
             COMMAND "win_c105_info_card_mutant_${mutant_lc}")
    set_tests_properties("mutation.c105.${mutant_lc}" PROPERTIES
                         TIMEOUT 240
                         PASS_REGULAR_EXPRESSION "C105 MUTATION CAUGHT: ${mutant_lc}")
  endforeach()
endif()
