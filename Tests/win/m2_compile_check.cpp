// Milestone M2 compile-and-link contract: the sparse-package registrar and the
// companion preview window must build and link their model dependencies. This
// TU is part of the Windows test binary so CI never lets either header rot.
#include "../../Src/PrismWin/CompanionWindow.hpp"
#include "../../Src/PrismWin/SparsePackageRegistrar.hpp"

#include "../mini_test.hpp"

PRISM_TEST("M2 headers compile and link their model dependencies") {
  (void)prism::win::sparse::IsRegistered(L"PrismArchive.ShellExt");
  prism::win::companion::CompanionWindow window;
  PRISM_CHECK(window.Hwnd() == nullptr);  // never created in this test
  prism::ArchiveItem item;
  item.path = "photo.png";
  window.SetCatalogItem(item, prism::QuickLookPlanner::kDefaultByteLimit);
}
