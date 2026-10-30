// Milestone M3 compile-and-link contract: the gallery window must build and
// link Direct2D/DirectWrite plus its PrismCore model dependencies (viewport
// math, thumbnail LRU, scroll spring). CI never lets the renderer rot.
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"

#include "../mini_test.hpp"

PRISM_TEST("M3 gallery window compiles and links its model dependencies") {
  prism::win::gallery::GalleryWindow window;
  std::vector<prism::ArchiveItem> items;
  window.SetItems(&items);
  window.Thumbnails().Insert(1, 100);
  window.Viewport().SetViewportSize(1280.0, 800.0);
  const auto range = window.Viewport().Visible(2);
  PRISM_CHECK(range.Empty());  // no items -> nothing rendered
}

PRISM_TEST("M3 glass island host + scene model cbuffer contract") {
  prism::win::glass::GlassIslandHost host;  // links d3d11/dxgi/dcomp
  prism::glass::GlassIslandScene scene;
  scene.Layout(1600, 1000);
  scene.SetPointer(300.0f, 40.0f);
  float cb[20];
  scene.WriteCbuffer(prism::glass::IslandId::Nav, 1600, 1000, cb);
  PRISM_CHECK_EQ(host.IslandCount(), size_t(0));  // not initialized here
  PRISM_CHECK(cb[2] > 0 && cb[3] == 22.0f);       // nav half-h = 44/2
}
