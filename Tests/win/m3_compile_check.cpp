// Milestone M3 compile-and-link contract: the gallery window must build and
// link Direct2D/DirectWrite plus its PrismCore model dependencies (viewport
// math, thumbnail LRU, scroll spring). CI never lets the renderer rot.
#include "../../Src/PrismWin/GalleryWindow.hpp"

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
