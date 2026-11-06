# c113-bench-honesty: a FAILED frame may never impersonate a successful one
# (card C113).
#
# WHY THIS SUITE EXISTS. bench_c76_startup.cpp's RenderOffscreen did
#   if (!window.RenderSceneInto(target.Get())) return frame;
# and that guard could not fire: GalleryWindow::DrawFrame called
# target->EndDraw(), dropped the HRESULT and returned true. On a window that has
# painted for real, the renderer's cached C77 backdrop bitmap belongs to the HWND
# target's device, so D2D rejects the whole frame at EndDraw with
# D2DERR_WRONG_RESOURCE_DOMAIN (0x88990012) and leaves the canvas clear behind.
# The C76 digest gate hashed that frame; C112 re-recorded it as a baseline.
#
# The suite pins three things on the product path:
#   H1  the cross-device rejection is NAMED (false + 0x88990012, painted == 0)
#       instead of arriving as a quiet empty frame;
#   H2  the capture seam reports rendered == true + S_OK, and the shared reader
#       prints RENDER FAILED and leaves rendered == false on every failure;
#   H3  a rendered frame carries the scene: digest != 0, non-black, and at least
#       5% of the pixels PAINTED (the canvas-only frame fails at 0).
#
# NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): the two arms are compiled from the
# same source with one rule broken, each prints its CAUGHT marker at the first
# red after the injection is observed at runtime, and the registration uses
# PASS_REGULAR_EXPRESSION -- never WILL_FAIL.
#
# TEST NAMES carry no "bench" token on purpose: ci.yml excludes 'bench|fuzz'
# from the merge gate, which is how the C76 pixel assertion stayed dead while
# it was red on a 1264x741 desktop. These run in CI.
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  function(c113_add exe)
    add_executable("${exe}" win/test_c113_bench_honesty.cpp)
    target_link_libraries("${exe}" PRIVATE prism_core prism_win_bridge
                          user32.lib gdi32.lib ole32.lib windowscodecs.lib
                          d2d1.lib dwrite.lib d3d11.lib dxgi.lib dcomp.lib)
    target_compile_options("${exe}" PRIVATE /WX-)
    if(TARGET prism_glass_shaders)
      add_dependencies("${exe}" prism_glass_shaders)
    endif()
  endfunction()

  set(C113_SHADERS "${CMAKE_BINARY_DIR}/Assets/Shaders")

  c113_add(c113_bench_honesty)
  add_test(NAME "c113.render_honesty" COMMAND c113_bench_honesty --shaders "${C113_SHADERS}")
  set_tests_properties(c113.render_honesty PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C113 HONESTY CHECKS PASSED")

  # H4: the C76 PIXEL claim in a test CI actually runs. c76.bench_startup is
  # excluded from the merge gate by PRISM_CTEST_EXCLUDE="bench|fuzz", so its
  # digest assertion has never executed in CI -- the name below deliberately
  # carries no "bench" token. Same window as the bench (same labels, two real
  # WM_PAINTs), captured through the product seam: painted >= 5% AND the
  # recorded digest, at the geometry it was recorded at.
  add_test(NAME "c113.render_digest" COMMAND c113_bench_honesty --digest
           --shaders "${C113_SHADERS}")
  set_tests_properties(c113.render_digest PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "ALL C113 DIGEST CHECKS PASSED")

  # D3 control 1: EndDraw's HRESULT is dropped again -- a rejected frame reports
  # success with painted == 0, so H1 MUST go red.
  c113_add(c113_bench_honesty_mutant_silent_failure)
  target_compile_definitions(c113_bench_honesty_mutant_silent_failure
                             PRIVATE PRISM_C113_MUTANT_SILENT_FAILURE=1)
  add_test(NAME "mutation.c113.silent_failure"
           COMMAND c113_bench_honesty_mutant_silent_failure
                   --mutant silent_failure --shaders "${C113_SHADERS}")
  set_tests_properties(mutation.c113.silent_failure PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C113 MUTATION CAUGHT: silent_failure")

  # D3 control 2: the frame is accepted and cleared to black -- H3 MUST go red.
  c113_add(c113_bench_honesty_mutant_black_frame)
  target_compile_definitions(c113_bench_honesty_mutant_black_frame
                             PRIVATE PRISM_C113_MUTANT_BLACK_FRAME=1)
  add_test(NAME "mutation.c113.black_frame"
           COMMAND c113_bench_honesty_mutant_black_frame
                   --mutant black_frame --shaders "${C113_SHADERS}")
  set_tests_properties(mutation.c113.black_frame PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C113 MUTATION CAUGHT: black_frame")

  # D3 control 3: the pre-C113 product (EndDraw's HRESULT dropped) read through
  # the SAME pixel arm -- the rejected frame must not satisfy painted/digest.
  add_test(NAME "mutation.c113.drop_enddraw"
           COMMAND c113_bench_honesty_mutant_silent_failure --mutant drop_enddraw
                   --digest --shaders "${C113_SHADERS}")
  set_tests_properties(mutation.c113.drop_enddraw PROPERTIES TIMEOUT 120
      PASS_REGULAR_EXPRESSION "C113 MUTATION CAUGHT: drop_enddraw")
endif()