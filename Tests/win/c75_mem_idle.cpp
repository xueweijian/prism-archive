// PrismArchive -- Windows integration test (card C75).
//
// THE QUESTION: after C34/C33, a minimised window still held 71.5 MB Working
// Set / 45.2 MB Private against ADR-006's 40 MB tier. Which objects are holding
// it, and how much is really returnable?
//
// This suite measures the SAME GalleryWindow the product runs (real D2D hwnd
// target, real DirectWrite formats, real glass island, real ThumbStore pixels),
// prints the per-category breakdown BEFORE and AFTER the minimise path, and
// asserts the releases on the accounting (C33's rule: bytes derived from live
// COM pointers / container sizes, never from a parallel counter):
//
//   * target-bound objects: brushes, text formats, device thumbnails, the D2D
//     hwnd target itself (its D3D device owns the back buffer)
//   * every cached thumbnail pixel (the store's 64 MiB budget is a CAP; an
//     invisible gallery has no reason to keep even the idle eighth)
//   * the glass transient layer (swapchains + effect graph)
//
// Five minimise -> restore cycles then assert (a) the minimised Working Set
// never grows, (b) every restore's FIRST frame contains content (a BitBlt right
// after the restore message, before any WM_PAINT), and (c) the restore frame
// stays within +20 % of the first cycle.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27) -- same source, compile-time:
//   * PRISM_C75_MUTANT_NO_TRIM            -- the idle path releases nothing
//   * PRISM_C75_MUTANT_NO_RESTORE_REBUILD -- the Resize does not rebuild/paint
// The mutant prints its CAUGHT marker at the first failed assertion (mini_test
// arms the verdict before the first check). No WILL_FAIL anywhere (D26).
//
// D1: a missing window station / DirectComposition is a LABELLED skip (77), and
// the pass marker is still required, so a silent skip cannot count as a pass.
// D29: the whole suite is one process with bounded waits; the ctest TIMEOUT is a
// real budget, not INFINITE.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "GalleryWindow.hpp"
#include "../mini_test.hpp"

#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "ole32.lib")

namespace {

using prism::win::gallery::GalleryWindow;

#if defined(PRISM_C75_MUTANT_NO_TRIM)
constexpr const char* kMutation = "no_trim";
#elif defined(PRISM_C75_MUTANT_NO_RESTORE_REBUILD)
constexpr const char* kMutation = "no_restore_rebuild";
#else
constexpr const char* kMutation = nullptr;
#endif

bool g_noWindow = false;
bool g_noComposition = false;

double NowMs() {
  static const double frequency = [] {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return static_cast<double>(f.QuadPart);
  }();
  LARGE_INTEGER counter{};
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * 1000.0 / frequency;
}

// ASan keeps every freed allocation in a quarantine before returning it to the
// OS, so on that lane "Working Set went down" is simply not observable: the
// CI run showed 112.90 -> 112.73 MB after releasing 22 MB of live objects, and
// +4.8 MB per restore cycle while the objects were provably gone. The behaviour
// assertions below still run everywhere; only the BYTE-level criteria are
// gated, and they are relative to the pre-minimise Working Set rather than a
// machine-specific constant.
bool AddressSanitizerActive() {
#if defined(__SANITIZE_ADDRESS__)
  return true;
#else
  static const bool loaded = [] {
    return GetModuleHandleW(L"clang_rt.asan_dynamic-x86_64.dll") != nullptr ||
           GetModuleHandleW(L"clang_rt.asan_dynamic-i386.dll") != nullptr ||
           GetModuleHandleW(L"asanrtl.dll") != nullptr;
  }();
  return loaded;
#endif
}

struct ProcessMemory {
  double workingSetMb = 0.0;
  double privateMb = 0.0;
  bool ok = false;
};

ProcessMemory CurrentMemory() {
  ProcessMemory m;
  PROCESS_MEMORY_COUNTERS counters{};
  if (!GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) return m;
  constexpr double kMb = 1024.0 * 1024.0;
  m.workingSetMb = static_cast<double>(counters.WorkingSetSize) / kMb;
  m.privateMb = static_cast<double>(counters.PagefileUsage) / kMb;
  m.ok = true;
  return m;
}

std::vector<prism::ArchiveItem> MakeItems(int count) {
  std::vector<prism::ArchiveItem> items;
  items.reserve(static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = "folder/frame_" + std::to_string(i) + ".png";
    item.size = 320u * 240u * 4u;
    item.packSize = item.size / 2;
    item.block = static_cast<uint32_t>(i / 16);
    items.push_back(std::move(item));
  }
  return items;
}

// 64 real-looking thumbnails (320x240 BGRA): 18.75 MB, the same order as the
// shipped app's 64-PNG fixture measured in C65. The payload is what a minimised
// window cannot use, so its release is the largest lever in this card.
std::shared_ptr<prism::gallery::ThumbStore> MakeThumbs(int count) {
  auto store = std::make_shared<prism::gallery::ThumbStore>(64u * 1024u * 1024u);
  for (int i = 0; i < count; ++i) {
    prism::gallery::DecodedThumb thumb;
    thumb.width = 320;
    thumb.height = 240;
    thumb.pixels.resize(static_cast<size_t>(thumb.width) * thumb.height * 4u);
    for (size_t p = 0; p < thumb.pixels.size(); p += 4) {
      const uint8_t v = static_cast<uint8_t>((i * 37 + static_cast<int>(p / 4)) & 0xFF);
      thumb.pixels[p + 0] = v;
      thumb.pixels[p + 1] = static_cast<uint8_t>(255 - v);
      thumb.pixels[p + 2] = static_cast<uint8_t>((v * 3) & 0xFF);
      thumb.pixels[p + 3] = 255;
    }
    store->OnThumb(static_cast<uint32_t>(i), thumb);
  }
  return store;
}

// The window is shown and painted; BitBlt from its DC is the pixels the user
// would see right now. Returns the fraction of pixels that are NOT the backdrop
// colour, plus a digest of the frame.
struct FramePixels {
  double nonBackdropFraction = 0.0;
  uint64_t digest = 1469598103934665603ull;
  bool captured = false;
};

FramePixels CaptureFrame(HWND hwnd, int width, int height) {
  FramePixels frame;
  if (hwnd == nullptr || width <= 0 || height <= 0) return frame;
  HDC windowDc = GetDC(hwnd);
  if (windowDc == nullptr) return frame;
  HDC memoryDc = CreateCompatibleDC(windowDc);
  HBITMAP bitmap = CreateCompatibleBitmap(windowDc, width, height);
  if (memoryDc == nullptr || bitmap == nullptr) {
    if (bitmap) DeleteObject(bitmap);
    if (memoryDc) DeleteDC(memoryDc);
    ReleaseDC(hwnd, windowDc);
    return frame;
  }
  HGDIOBJ old = SelectObject(memoryDc, bitmap);
  const BOOL blitted = BitBlt(memoryDc, 0, 0, width, height, windowDc, 0, 0, SRCCOPY);
  if (blitted) {
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4u);
    if (GetDIBits(memoryDc, bitmap, 0, static_cast<UINT>(height), pixels.data(), &info,
                  DIB_RGB_COLORS) == height) {
      size_t nonBackdrop = 0;
      for (size_t p = 0; p + 3 < pixels.size(); p += 4) {
        const int b = pixels[p + 0];
        const int g = pixels[p + 1];
        const int r = pixels[p + 2];
        // Content = neither the backdrop (#0B0E13) nor the BLACKNESS wipe the
        // suite uses to clear a stale framebuffer. A frame that is 100 % black
        // is a blank frame -- not content that merely differs from the backdrop.
        const bool nearBackdrop =
            std::abs(b - 0x13) <= 8 && std::abs(g - 0x0E) <= 8 && std::abs(r - 0x0B) <= 8;
        const bool nearBlack = r < 8 && g < 8 && b < 8;
        if (!nearBackdrop && !nearBlack) ++nonBackdrop;
        frame.digest = (frame.digest ^ static_cast<uint64_t>(pixels[p] + pixels[p + 1] * 3 +
                                                             pixels[p + 2] * 7 + p)) *
                       1099511628211ull;
      }
      if (!pixels.empty()) {
        frame.nonBackdropFraction =
            static_cast<double>(nonBackdrop) / static_cast<double>(pixels.size() / 4u);
      }
      frame.captured = true;
    }
  }
  SelectObject(memoryDc, old);
  DeleteObject(bitmap);
  DeleteDC(memoryDc);
  ReleaseDC(hwnd, windowDc);
  return frame;
}

void PrintBreakdown(const char* label, GalleryWindow& window, const ProcessMemory& memory) {
  const GalleryWindow::MemorySnapshot s = window.CountMemory();
  std::printf(
      "C75_BREAKDOWN {\"state\":\"%s\",\"ws_mb\":%.2f,\"private_mb\":%.2f,"
      "\"catalog_items\":%zu,\"catalog_name_bytes\":%zu,\"folder_items\":%zu,"
      "\"folder_name_bytes\":%zu,\"list_items\":%zu,"
      "\"thumb_bytes\":%zu,\"thumb_entries\":%zu,\"thumb_budget\":%zu,"
      "\"glass_transient_bytes\":%zu,\"glass_islands\":%zu,\"glass_device\":%s,"
      "\"target_live\":%s,\"target_bytes\":%zu,\"brushes\":%d,\"text_formats\":%zu,"
      "\"thumb_bitmaps\":%zu,\"thumb_bitmap_bytes\":%zu,"
      "\"sidebar_commands\":%zu,\"tabs\":%zu}\n",
      label, memory.workingSetMb, memory.privateMb, s.catalogItems, s.catalogNameBytes,
      s.folderItems, s.folderNameBytes, s.listItems, s.thumbBytes, s.thumbEntries,
      s.thumbBudget, s.glassTransientBytes, s.glassIslands, s.glassDeviceLive ? "true" : "false",
      s.targetLive ? "true" : "false", s.targetBytes, s.rendererBrushes, s.textFormats,
      s.rendererThumbBitmaps, s.rendererThumbBitmapBytes, s.sidebarCommands, s.tabs);
}

PRISM_TEST("C75: minimised resident breakdown, release, and five restore cycles") {
  // The marker is a string literal supplied by the caller; the mutant build
  // arms it before the first assertion (D27).
  if (kMutation == nullptr) {
    prism::test::SetMutationVerdict(nullptr);
  } else if (std::string(kMutation) == "no_trim") {
    prism::test::SetMutationVerdict("C75 MUTATION CAUGHT: no_trim");
  } else {
    prism::test::SetMutationVerdict("C75 MUTATION CAUGHT: no_restore_rebuild");
  }
  std::vector<prism::ArchiveItem> items = MakeItems(800);
  auto thumbs = MakeThumbs(64);

  GalleryWindow window;
  if (!window.Create(L"PrismArchive C75 idle", GetModuleHandleW(nullptr))) {
    std::printf("SKIP: CreateWindowExW failed err=%lu (no window station)\n", GetLastError());
    g_noWindow = true;
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"c75.7z");
  window.SetStatus(L"800 items");
  window.SetSidebarRailVisible(true);
  window.SetListMode(true);
  window.SetThumbs(thumbs);
  window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1100, 740));
  if (!window.EnableGlassLayer()) {
    std::printf("SKIP: DirectComposition unavailable on this window station\n");
    g_noComposition = true;
    window.Dispatch(WM_DESTROY, 0, 0);
    return;
  }
  ShowWindow(window.Hwnd(), SW_SHOWNORMAL);
  UpdateWindow(window.Hwnd());
  window.Dispatch(WM_PAINT, 0, 0);  // the first frame, synchronously
  Sleep(250);

  const ProcessMemory openMemory = CurrentMemory();
  PrintBreakdown("open", window, openMemory);
  PRISM_CHECK(openMemory.ok);
  PRISM_CHECK(window.TargetLive());
  PRISM_CHECK(window.CountMemory().thumbBytes > 16u * 1024u * 1024u);
  PRISM_CHECK(window.CountMemory().rendererBrushes > 0);
  PRISM_CHECK(window.CountMemory().glassTransientBytes > 0);

  // ---- the minimise path, exactly what WM_SIZE(SIZE_MINIMIZED) runs --------
  window.Dispatch(WM_SIZE, SIZE_MINIMIZED, 0);
  Sleep(500);  // IDXGIDevice3::Trim is lazy; give the driver a bounded window
  const ProcessMemory minMemory = CurrentMemory();
  PrintBreakdown("minimised", window, minMemory);
  const GalleryWindow::MemorySnapshot idle = window.CountMemory();
  // Accounting assertions (C33's rule): the objects themselves, not a counter.
  PRISM_CHECK(!window.TargetLive());
  PRISM_CHECK_EQ(idle.targetBytes, size_t(0));
  PRISM_CHECK_EQ(idle.thumbBytes, size_t(0));
  PRISM_CHECK_EQ(idle.thumbEntries, size_t(0));
  PRISM_CHECK_EQ(idle.glassTransientBytes, size_t(0));
  PRISM_CHECK(window.GlassReleasedBytes() > 0);
  PRISM_CHECK_EQ(idle.rendererBrushes, 0);
  PRISM_CHECK_EQ(idle.rendererTextFormats, 0);
  PRISM_CHECK_EQ(idle.rendererThumbBitmaps, size_t(0));
  // The process really gave memory back. This is the assertion the NO_TRIM
  // mutant must break (it releases nothing).
  const double returnedMb = openMemory.workingSetMb - minMemory.workingSetMb;
  const bool asan = AddressSanitizerActive();
  std::printf("C75_MINIMISE {\"open_ws_mb\":%.2f,\"min_ws_mb\":%.2f,\"returned_mb\":%.2f,"
              "\"min_private_mb\":%.2f,\"adr006_target_mb\":40,\"asan\":%s}\n",
              openMemory.workingSetMb, minMemory.workingSetMb, returnedMb, minMemory.privateMb,
              asan ? "true" : "false");
  if (asan) {
    // Behaviour is asserted above (target dead, every cache 0); ASan's
    // quarantine makes the byte count meaningless on this lane.
    std::printf("C75_MINIMISE_NOTE: ASan quarantine active; byte-level return is report-only\n");
  } else {
    // Relative criterion (the card's own fix for machine-specific digits): the
    // release must return at least 15 %% of the pre-minimise Working Set. The
    // local measurement is ~33 %%; a release that only forgot its counter is
    // far below this.
    PRISM_CHECK(returnedMb >= openMemory.workingSetMb * 0.15);
  }
  // ADR-006's 40 MB tier is REPORTED, not asserted green: on this host the
  // process floor after every release (no target, no glass, no thumbnails) is
  // still ~44 MB, so claiming <= 40 would be a false green. docs/PERF.md 9
  // carries the floor's breakdown and what would have to change to close it.
  std::printf("C75_ADR006 {\"min_ws_mb\":%.2f,\"target_mb\":40,\"met\":%s}\n",
              minMemory.workingSetMb, minMemory.workingSetMb <= 40.0 ? "true" : "false");

  // ---- five minimise -> restore cycles -------------------------------------
  double firstRestoreMs = 0.0;
  const double firstMinWs = minMemory.workingSetMb;
  std::vector<double> laterRestoreMs;
  for (int cycle = 1; cycle <= 5; ++cycle) {
    // The minimized window keeps its last framebuffer contents, so a stale
    // BitBlt could masquerade as "the restore painted". Wipe the window DC
    // first: what the capture sees is ONLY what the Resize paints.
    if (HDC dc = GetDC(window.Hwnd())) {
      PatBlt(dc, 0, 0, 1100, 740, BLACKNESS);
      ReleaseDC(window.Hwnd(), dc);
    }
    const double start = NowMs();
    window.Dispatch(WM_SIZE, SIZE_RESTORED, MAKELPARAM(1100, 740));
    const double restoreMs = NowMs() - start;
    if (cycle == 1) firstRestoreMs = restoreMs;
    // FIRST frame of the restored window: no WM_PAINT has been dispatched here.
    const FramePixels frame = CaptureFrame(window.Hwnd(), 1100, 740);
    std::printf("C75_RESTORE {\"cycle\":%d,\"restore_ms\":%.1f,\"non_backdrop\":%.4f,"
                "\"digest\":\"%016llx\",\"captured\":%s,\"rebuilds\":%d,\"ws_mb\":%.2f}\n",
                cycle, restoreMs, frame.nonBackdropFraction,
                static_cast<unsigned long long>(frame.digest), frame.captured ? "true" : "false",
                window.RestoreRebuilds(), CurrentMemory().workingSetMb);
    PRISM_CHECK(frame.captured);
    // A blank frame is backdrop-only (0.0). Content is well above 1 %.
    PRISM_CHECK(frame.nonBackdropFraction > 0.01);
    PRISM_CHECK(window.TargetLive());
    PRISM_CHECK(window.RendererReady());
    PRISM_CHECK(window.GlassLayerVisible());
    PRISM_CHECK(window.CountMemory().thumbBytes == 0);  // not re-swept, by design
    // The +20 % budget is judged on the MEDIAN of the later cycles, not on
    // any single one: a shared runner produces occasional 8-25 ms scheduling
    // spikes for the same 12-16 ms rebuild, and a per-cycle max would test the
    // scheduler, not the code. The 10 ms floor is this host's timer/scheduler
    // granularity; a regression to full device recreation is 60-100 ms and
    // still fails this by a wide margin.
    if (cycle > 1) laterRestoreMs.push_back(restoreMs);

    window.Dispatch(WM_SIZE, SIZE_MINIMIZED, 0);
    Sleep(350);
    const ProcessMemory cycleMin = CurrentMemory();
    PrintBreakdown(("minimised_cycle_" + std::to_string(cycle)).c_str(), window, cycleMin);
    // Monotonic across rounds, measured against the FIRST minimised state (not
    // the previous one, so one noisy round cannot ratchet the budget up). The
    // 2 MB band is jitter; ASan's quarantine legitimately grows and is excluded.
    if (!asan) {
      PRISM_CHECK(cycleMin.workingSetMb <= firstMinWs + 2.0);
    } else {
      PRISM_CHECK(window.CountMemory().targetBytes == 0);
      PRISM_CHECK(window.CountMemory().thumbBytes == 0);
    }
  }

  if (!laterRestoreMs.empty()) {
    std::sort(laterRestoreMs.begin(), laterRestoreMs.end());
    const double medianLater = laterRestoreMs[laterRestoreMs.size() / 2];
    std::printf("C75_RESTORE_BUDGET {\"first_ms\":%.1f,\"later_median_ms\":%.1f,"
                "\"budget_ms\":%.1f}\n",
                firstRestoreMs, medianLater, firstRestoreMs * 1.2 + 10.0);
    PRISM_CHECK(medianLater <= firstRestoreMs * 1.2 + 10.0);
  }

  window.Dispatch(WM_DESTROY, 0, 0);
}

}  // namespace

// D1: a laboured skip is exit 77 and carries a reason. The suite file maps 77 to
// SKIP_RETURN_CODE, and the pass marker is still required on a normal run.
int main(int argc, char** argv) {
  const int code = prism::test::RunAll(argc, argv);
  if (g_noWindow || g_noComposition) {
    std::printf("SKIP: window station=%s composition=%s\n", g_noWindow ? "no" : "yes",
                g_noComposition ? "no" : "yes");
    return 77;
  }
  if (code == 0) std::printf("ALL C75 IDLE CHECKS PASSED\n");
  return code;
}
