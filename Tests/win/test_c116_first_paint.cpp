// PrismArchive -- PrismWin
// C116: the first frame, and the frame that carries the data.
//
// THE REPORTED DEFECT. Open demo.zip and the window shows the caption and the
// menu bar over an empty client area; the accessibility tree, meanwhile, lists
// the whole model (eight buttons, a tab, a listbox with the folder's rows, the
// status band). The hypothesis this card was opened on: "the first frame was
// painted before the scan landed (scene.items == nullptr) and nothing ever
// asked for a second one".
//
// WHAT THIS SUITE PINS, and why it is stated the way it is:
//   * the window is CLEAN before the catalog arrives and DIRTY immediately
//     after it. The predicate is GetUpdateRect -- the platform's own answer to
//     "is a WM_PAINT pending?" -- not a counter the window keeps about itself,
//     so a build that repaints "in theory" cannot pass it;
//   * the frame painted AFTER the data is not the frame painted before it: the
//     content band carries thousands of pixels that were not there, rendered by
//     the SAME GalleryWindow::RenderSceneInto the window's own WM_PAINT uses
//     (C76's seam, a real D2D target, no lookalike painter).
//
// NEGATIVE CONTROL (CONTRACTS D3/D26/D27): PRISM_C116_MUTANT_NO_REPAINT
// closes GalleryWindow::Invalidate() (kRepaintRequestsArmed), i.e. the ONE door
// every repaint request goes through -- the scene still rebuilds, no frame is
// ever asked for.
// The dirty assertion MUST go red, the mutant prints
// "C116 MUTATION CAUGHT: no_repaint" at the FIRST failure, and the target's
// PASS_REGULAR_EXPRESSION is what turns that into a green CTest -- no
// WILL_FAIL is stacked on it.
//
// D1: a session with no window station exits 77 after printing WHY.
// D29: every wait is bounded; this suite has none.
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;

namespace {

constexpr int kSkipReturnCode = 77;

#if defined(PRISM_C116_MUTANT_NO_REPAINT)
constexpr const char* kMutation = "no_repaint";
#else
constexpr const char* kMutation = nullptr;
#endif

GalleryWindow* g_window = nullptr;
IWICImagingFactory* g_wic = nullptr;
ID2D1Factory* g_d2d = nullptr;
std::vector<prism::ArchiveItem> g_catalog;
std::vector<prism::ArchiveItem> g_otherCatalog;

// The runtime proof the mutation verdict reads: the window was left DIRTY by
// the catalog arriving. Only a build without the request can leave it false.
bool g_dirtyAfterData = false;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA

  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
};

bool RenderWindow(GalleryWindow& window, Frame* out) {
  RECT rc{};
  if (!::GetClientRect(window.Hwnd(), &rc)) return false;
  const UINT32 width = static_cast<UINT32>(rc.right - rc.left);
  const UINT32 height = static_cast<UINT32>(rc.bottom - rc.top);
  if (width == 0 || height == 0) return false;

  ComPtr<IWICBitmap> bitmap;
  if (FAILED(g_wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                 WICBitmapCacheOnLoad, bitmap.GetAddressOf()))) {
    return false;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  ComPtr<ID2D1RenderTarget> target;
  if (FAILED(g_d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                target.GetAddressOf()))) {
    return false;
  }
  if (!window.RenderSceneInto(target.Get())) return false;

  WICRect lockRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockRead, lock.GetAddressOf()))) return false;
  UINT stride = 0;
  UINT bytes = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bytes, &data))) {
    return false;
  }
  out->width = width;
  out->height = height;
  out->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(out->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
  }
  return true;
}

int DifferingPixels(const Frame& a, const Frame& b, const prism::layout::Rect& rect) {
  if (a.width != b.width || a.height != b.height) return -1;
  int differing = 0;
  for (int y = rect.top; y < rect.bottom; ++y) {
    for (int x = rect.left; x < rect.right; ++x) {
      if (a.At(x, y) != b.At(x, y)) ++differing;
    }
  }
  return differing;
}

// The frame painted BEFORE any catalog was attached. Captured in main() so the
// ordering is explicit and no test depends on another test having run first.
Frame g_beforeData;
int g_rowsBeforeData = -1;

void Add(std::vector<prism::ArchiveItem>* catalog, const char* path, bool dir,
         std::uint64_t size) {
  prism::ArchiveItem item;
  item.index = static_cast<std::uint32_t>(catalog->size());
  item.path = path;
  item.isDir = dir;
  item.size = size;
  item.packSize = size / 2;
  catalog->push_back(item);
}

void BuildCatalogs() {
  Add(&g_catalog, "docs", true, 0);
  for (int i = 0; i < 8; ++i) {
    char name[32]{};
    std::snprintf(name, sizeof(name), "file%03d.txt", i);
    Add(&g_catalog, name, false, static_cast<std::uint64_t>(1024 + i * 17));
  }
  Add(&g_otherCatalog, "readme.txt", false, 1202);
  Add(&g_otherCatalog, "notes.md", false, 3402);
}

// The window is clean = the platform has no WM_PAINT pending for it.
bool WindowIsDirty(HWND hwnd) {
  RECT pending{};
  return ::GetUpdateRect(hwnd, &pending, FALSE) != FALSE;
}

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (no window station)" << std::endl;
    return kSkipReturnCode;
  }
  if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWICImagingFactory,
                                reinterpret_cast<void**>(&g_wic)))) {
    std::cout << "SKIP: WIC factory unavailable" << std::endl;
    return kSkipReturnCode;
  }
  if (FAILED(::D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d))) {
    std::cout << "SKIP: D2D factory unavailable" << std::endl;
    return kSkipReturnCode;
  }

  BuildCatalogs();
  static GalleryWindow window;
  if (!window.Create(L"PrismArchive C116 first paint", ::GetModuleHandleW(nullptr))) {
    std::cout << "SKIP: window creation failed (no interactive window station)" << std::endl;
    return kSkipReturnCode;
  }
  g_window = &window;
  window.SetRootLabel(L"demo.zip");
  window.Show();
  // Frame 1: the window exists, it has a client area and NO catalog. This is
  // the frame the card says stayed on screen. Paint it and clear the update
  // region, so "the window is clean" is a measured baseline and not an
  // assumption about what ShowWindow left behind.
  ::UpdateWindow(window.Hwnd());
  ::ValidateRect(window.Hwnd(), nullptr);
  if (!RenderWindow(window, &g_beforeData)) {
    std::cout << "SKIP: could not render the pre-data frame" << std::endl;
    return kSkipReturnCode;
  }
  g_rowsBeforeData = window.RowCellsPainted();
  std::printf("  [before] rows painted with no catalog = %d\n", g_rowsBeforeData);

  if (kMutation != nullptr) {
    std::cout << "C116 MUTANT ACTIVE: " << kMutation << std::endl;
    prism::test::SetMutationVerdict("C116 MUTATION CAUGHT: no_repaint",
                                    [] { return !g_dirtyAfterData; });
  }

  const int rc = prism::test::RunAll(argc, argv);
  if (kMutation != nullptr) {
    if (rc == 0) {
      std::cout << "C116 MUTATION NOT CAUGHT: the suite stayed green with the repaint "
                   "request compiled out -- the negative control is stale"
                << std::endl;
    }
    return 1;
  }
  if (rc != 0) {
    std::cout << "C116 FIRST-PAINT CHECKS FAILED" << std::endl;
    return rc;
  }
  std::cout << "ALL C116 FIRST-PAINT CHECKS PASSED (" << prism::test::CheckCount()
            << " checks)" << std::endl;
  ::CoUninitialize();
  return 0;
}

// ---------------------------------------------------------------------------
// 1. THE REPAINT REQUEST. A catalog that arrives after the first frame must
//    leave the window DIRTY. This is the window's half of the contract the
//    card states: "the scan finished" has to mean "ask for the frame".
// ---------------------------------------------------------------------------
PRISM_TEST("c116 the catalog arriving makes the window paint again") {
  HWND hwnd = g_window->Hwnd();
  PRISM_CHECK(hwnd != nullptr);
  PRISM_CHECK(!WindowIsDirty(hwnd));  // the baseline: frame 1 was consumed

  g_window->SetItems(&g_catalog);  // the scan completes

  g_dirtyAfterData = WindowIsDirty(hwnd);
  PRISM_CHECK(g_dirtyAfterData);
  std::printf("  [dirty] update region pending after SetItems = %d\n",
              g_dirtyAfterData ? 1 : 0);
}

// ---------------------------------------------------------------------------
// 2. THE FRAME THAT CARRIES THE DATA. The next painted frame must differ from
//    the pre-data frame ACROSS THE CONTENT BAND -- not merely in the status
//    line or the tab strip. Both frames come from RenderSceneInto, the same
//    DrawFrame the window's WM_PAINT runs.
// ---------------------------------------------------------------------------
PRISM_TEST("c116 the first frame after the data has a content band") {
  Frame after;
  PRISM_CHECK(RenderWindow(*g_window, &after));
  PRISM_CHECK_EQ(after.width, g_beforeData.width);
  PRISM_CHECK_EQ(after.height, g_beforeData.height);

  const prism::layout::Rect content = g_window->ContentArea();
  std::printf("  [content] %d,%d %dx%d\n", content.left, content.top, content.Width(),
              content.Height());
  PRISM_CHECK(content.Width() > 100);
  PRISM_CHECK(content.Height() > 100);

  const int differing = DifferingPixels(g_beforeData, after, content);
  std::printf("  [pixels] content-band pixels changed vs the pre-data frame = %d\n",
              differing);
  // Eight rows/tiles and the rail: thousands of pixels. The floor is far below
  // the real number so a palette tweak cannot turn this red, and far above zero
  // so "the same blank canvas" can never pass.
  PRISM_CHECK(differing >= 2000);
  // The pre-data frame painted NO rows; the frame the data produced painted
  // them. Both facts are asserted, so "the canvas changed colour" cannot stand
  // in for "the rows are there".
  PRISM_CHECK(g_rowsBeforeData == 0);
  const int rows = g_window->RowCellsPainted();
  std::printf("  [rows] grid rows painted after the data = %d (before = %d)\n", rows,
              g_rowsBeforeData);
  PRISM_CHECK(rows > 0);
  PRISM_CHECK(g_window->ItemCount() == static_cast<int>(g_catalog.size()));
}

// ---------------------------------------------------------------------------
// 3. THE SAME DOOR ON THE NEXT CATALOG. A second archive (or a folder change)
//    rebuilds the scene through the same RebuildFolderView; it must dirt the
//    window again, or the user keeps looking at the previous archive's pixels.
// ---------------------------------------------------------------------------
PRISM_TEST("c116 every scene rebuild asks for its own frame") {
  HWND hwnd = g_window->Hwnd();
  ::ValidateRect(hwnd, nullptr);  // consume whatever the previous case left
  PRISM_CHECK(!WindowIsDirty(hwnd));

  g_window->SetItems(&g_otherCatalog);

  PRISM_CHECK(WindowIsDirty(hwnd));
  Frame after;
  PRISM_CHECK(RenderWindow(*g_window, &after));
  const prism::layout::Rect content = g_window->ContentArea();
  const int differing = DifferingPixels(g_beforeData, after, content);
  std::printf("  [pixels] second catalog vs pre-data frame = %d\n", differing);
  PRISM_CHECK(differing >= 1000);
  PRISM_CHECK(g_window->ItemCount() == static_cast<int>(g_otherCatalog.size()));
  PRISM_CHECK(g_window->RowCellsPainted() > 0);
}
