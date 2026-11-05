# c107-optics: the Liquid Glass OPTICAL CHAIN, connected (P2 core).
#
# Before this card the two shaders had never run in the product:
# InitializeOptics(...) had zero call sites and PaintIsland(...) was called with
# backdrop = nullptr from a callback that only cleared the surface. The suite
# asserts, in pixels, that (1) the chain is live and ShaderError() is empty,
# (2) the panel differs from a flat fill by >= 6 levels, (3) the panel follows
# the window content captured under it (red -> blue, delta R/B >= 4), (4) 200
# composed frames leave GlassTransientBytes() unchanged, and (5) a real
# GalleryWindow initializes the chain and covers NavPillRect.
#
# One card = one file (see Tests/suites/README.md): Tests/CMakeLists.txt is
# untouched; the glob picks this up.
#
# Negative control (CONTRACTS D3/D26/D27): the SAME source compiled with
# PRISM_C107_MUTANT_NO_BACKDROP (the backdrop handed to the composition is
# dropped -- the pre-C107 call site). The entry is registered with a
# PASS_REGULAR_EXPRESSION on the verdict, never with WILL_FAIL, and the verdict
# only prints when the injection was observed at runtime (DroppedBackdrops()).
if(WIN32 AND PRISM_BUILD_WIN_LAYER)
  add_executable(win_c107_optics win/test_c107_optics.cpp)
  target_link_libraries(win_c107_optics PRIVATE prism_core prism_win_bridge
                                                user32.lib gdi32.lib d2d1.lib dwrite.lib
                                                d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib
                                                windowscodecs.lib ole32.lib)
  target_compile_options(win_c107_optics PRIVATE /WX-)
  add_test(NAME "win.c107.optics" COMMAND win_c107_optics)
  set_tests_properties(win.c107.optics PROPERTIES TIMEOUT 300
      PASS_REGULAR_EXPRESSION "ALL C107 OPTICS CHECKS PASSED")

  add_executable(win_c107_optics_mutant_no_backdrop win/test_c107_optics.cpp)
  target_link_libraries(win_c107_optics_mutant_no_backdrop
                        PRIVATE prism_core prism_win_bridge
                                user32.lib gdi32.lib d2d1.lib dwrite.lib
                                d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib
                                windowscodecs.lib ole32.lib)
  target_compile_options(win_c107_optics_mutant_no_backdrop PRIVATE /WX-)
  target_compile_definitions(win_c107_optics_mutant_no_backdrop
                             PRIVATE PRISM_C107_MUTANT_NO_BACKDROP=1)
  add_test(NAME "mutation.c107.optics.no_backdrop"
           COMMAND win_c107_optics_mutant_no_backdrop)
  set_tests_properties("mutation.c107.optics.no_backdrop" PROPERTIES
                       TIMEOUT 300
                       PASS_REGULAR_EXPRESSION "C107 MUTATION CAUGHT: no_backdrop")
endif()
