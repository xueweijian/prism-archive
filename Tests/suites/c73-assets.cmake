# c73-assets: the README screenshots are artifacts, so they are tested (card C73).
#
# What is proven, and what would make it a lie:
#   docs.c73_assets   the four PNGs exist, decode as PNG, are exactly 1280x800,
#                     stay under 400 KB each, ARE referenced by README.md, and
#                     the README no longer claims they were never taken.
#
# Skipped when python is unavailable (D1: the skip marker is part of the test).
find_package(Python3 COMPONENTS Interpreter QUIET)

if(Python3_Interpreter_FOUND)
  add_test(NAME docs.c73_assets
           COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tools/check_assets.py")
  set_tests_properties(docs.c73_assets PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "ASSETS GATE: PASS"
    SKIP_RETURN_CODE 77)
else()
  add_test(NAME docs.c73_assets COMMAND "${CMAKE_COMMAND}" -E echo "python missing: skip")
  set_tests_properties(docs.c73_assets PROPERTIES
    PASS_REGULAR_EXPRESSION "python missing: skip"
    SKIP_RETURN_CODE 77)
endif()
