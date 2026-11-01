# c17-zstd: 7-Zip ZS (Zstd / LZ4 / Brotli) engine extension spike
# (blueprint ch.2 "merge 7-Zip ZS's Zstd/LZ4/Brotli"). One card = one file;
# see Tests/suites/README.md. Tests/CMakeLists.txt stays untouched.
#
# What this buys, in one executable:
#   * pure classification of a codec table (no DLL, always judged), including the
#     negative control: the stock 7-Zip 25.01 table must classify as "no zstd";
#   * a live probe of a real 7-Zip ZS 7z.dll (format + method tables + version);
#   * a zstd-compressed .7z extracted through the UNCHANGED bridge, bare
#     .zst/.lz4/.br decoded through their handler CLSIDs, and the same zstd
#     fixture failing on the stock DLL (so the positive case cannot be fake).
#
# The ZS DLL is opt-in: without PRISM_7Z_ZS_DLL (and without the local default
# path) the suite prints its reason and exits 77. SKIP_RETURN_CODE + the required
# stdout marker keep that visible -- a silent skip can never count as a pass
# (CONTRACTS D1).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_codec_probe win/test_codec_probe.cpp)
  # version.lib: CodecProbe reads the DLL's VERSIONINFO so the log names the exact
  # engine build that was probed.
  target_link_libraries(win_codec_probe PRIVATE prism_core prism_win_bridge version.lib)
  target_compile_options(win_codec_probe PRIVATE /WX-)
  # Fixtures are tiny committed archives (docs/ENGINE-ZS.md has the exact
  # regeneration commands); the path is baked in so the suite runs from any cwd.
  target_compile_definitions(win_codec_probe PRIVATE
      "PRISM_ZS_FIXTURE_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/fixtures/zs\"")
  add_test(NAME "win.codec_probe" COMMAND win_codec_probe)
  set_tests_properties(win.codec_probe PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77
      PASS_REGULAR_EXPRESSION "ALL CODEC-PROBE CHECKS PASSED")
endif()
