// Milestone M3 compile-and-link contract: the gallery window must build and
// link Direct2D/DirectWrite plus its PrismCore model dependencies (viewport
// math, thumbnail LRU, scroll spring). CI never lets the renderer rot.
#include "../../Src/PrismWin/GalleryWindow.hpp"
#include "../../Src/PrismWin/GlassIslandHost.hpp"
#include "../../Src/PrismWin/PrismDragDecisionBridge.hpp"

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

PRISM_TEST("M3 drag bridge: decision core drives a real CFSTR_FILEDESCRIPTORW IDataObject") {
  using prism::dnd::Channel;
  using prism::dnd::DenyReason;
  using prism::dnd::VirtualFileDescriptor;

  // The decision core picks channel B for an IStream-only target...
  prism::dnd::DragTargetDescription target;
  target.acceptsIStream = true;
  const auto decision = prism::dnd::DecideDragChannel(target);
  PRISM_CHECK_EQ(decision.channel, Channel::MemoryStream);

  // ...and the payload names come from the sanitised descriptor: a hostile
  // archive path must never reach the clipboard verbatim.
  const auto group = prism::dnd::BuildGroupDescriptor(std::vector<VirtualFileDescriptor>{
      {"raw/../../../evil/CON.txt", 5, 0, false}, {"dir/note.md", 2, 0, false}});
  PRISM_CHECK_EQ(group.error, DenyReason::None);

  Microsoft::WRL::ComPtr<IDataObject> data;
  HRESULT hr = prism::win::dragdrop::CreateVirtualFileDataObject(
      group, [](size_t) -> Microsoft::WRL::ComPtr<IStream> { return nullptr; }, &data);
  PRISM_CHECK(SUCCEEDED(hr));

  FORMATETC descriptorEtc{};
  // L"" literals (not the TEXT-based CFSTR_* macros): this project is not a
  // UNICODE build, so the macros expand narrow and MSVC rejects them here.
  descriptorEtc.cfFormat =
      static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
  descriptorEtc.dwAspect = DVASPECT_CONTENT;
  descriptorEtc.lindex = -1;
  descriptorEtc.tymed = TYMED_HGLOBAL;
  PRISM_CHECK_EQ(data->QueryGetData(&descriptorEtc), S_OK);

  STGMEDIUM medium{};
  hr = data->GetData(&descriptorEtc, &medium);
  PRISM_CHECK(SUCCEEDED(hr));
  PRISM_CHECK_EQ(medium.tymed, static_cast<DWORD>(TYMED_HGLOBAL));
  const auto* descriptors = static_cast<const FILEGROUPDESCRIPTORW*>(GlobalLock(medium.hGlobal));
  PRISM_CHECK(descriptors != nullptr);
  if (descriptors) {
    PRISM_CHECK_EQ(descriptors->cItems, 2u);
    // "CON.txt" must arrive device-renamed, not as the raw archive name.
    PRISM_CHECK(wcscmp(descriptors->fgd[0].cFileName, L"CON_.txt") == 0);
    PRISM_CHECK_EQ(descriptors->fgd[0].nFileSizeLow, 5ul);
    PRISM_CHECK_EQ(wcscmp(descriptors->fgd[1].cFileName, L"note.md"), 0);
    GlobalUnlock(medium.hGlobal);
  }
  ReleaseStgMedium(&medium);  // hands the copied HGLOBAL back: no leak

  // FILECONTENTS is per-item TYMED_ISTREAM; the skeleton producer yields no
  // stream, so the failure must be loud instead of delivering an empty file.
  FORMATETC contentsEtc{};
  contentsEtc.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
  contentsEtc.dwAspect = DVASPECT_CONTENT;
  contentsEtc.lindex = 0;
  contentsEtc.tymed = TYMED_ISTREAM;
  PRISM_CHECK_EQ(data->QueryGetData(&contentsEtc), S_OK);
  STGMEDIUM streamMedium{};
  hr = data->GetData(&contentsEtc, &streamMedium);
  PRISM_CHECK(FAILED(hr));

  // Channel A guard: UNC shares look like directories but must never be
  // trusted for a direct write.
  PRISM_CHECK(!prism::win::dragdrop::IsTrustedDirectDropTarget(L"\\\\server\\share"));
}
