// PrismArchive -- card C113: BENCH HONESTY. A failed frame may never impersonate
// a successful one.
//
// THE DEFECT THIS SUITE PINS. GalleryWindow::DrawFrame called target->EndDraw()
// and discarded the HRESULT, then returned true. On a window that has painted
// for real, the renderer's cached C77 backdrop bitmap belongs to the HWND
// target's device; painting it into a WIC target makes D2D reject the whole
// frame at EndDraw with D2DERR_WRONG_RESOURCE_DOMAIN (0x88990012). What lands in
// the bitmap is the canvas clear only -- B=19,G=14,R=11 on every one of the
// 936624 pixels at 1264x741 -- so the frame is NOT black, it is a bare canvas.
// bench_c76_startup.cpp hashed exactly that frame and compared it to a pixel
// "baseline"; C112 then re-recorded the baseline to 0af0b75bc5b12383, which is
// this suite's H1 frame. The gate was measuring a rejected picture.
//
// THE THREE HYPOTHESES (each one turns red on its own mutant):
//   H1  the cross-device rejection is a NAMED fact, not an occasional glitch:
//       after a real paint, the raw seam returns false AND LastRenderError() is
//       0x88990012, and the frame it leaves behind has painted == 0. On a
//       platform that gives both targets ONE device the frame must instead be a
//       REAL frame -- the branch is printed, never assumed.
//   H2  a capture is either honestly failed or explicitly rendered: the product
//       seam (RenderSceneForCapture) returns rendered == true and hr == S_OK,
//       and the helper prints RENDER FAILED plus leaves rendered == false on
//       every failure path, so silence cannot pass for success.
//   H3  a frame that claims to be rendered carries the scene: digest != 0, at
//       least half the pixels non-black (weak, the canvas passes it) and at
//       least 5% of the pixels PAINTED -- which the canvas-only frame fails at 0.
//   H4  (--digest) the C76 pixel claim, reachable from CI: ci.yml sets
//       PRISM_CTEST_EXCLUDE="bench|fuzz", so c76.bench_startup has never run in
//       the merge gate. This arm paints the SAME window the C76 bench paints
//       (real WM_PAINTs, island presented), captures it through the product
//       seam, and pins painted >= 5% plus the recorded digest
//       5a0c0a1946978d9c (C112 + C113; see the constant for why the baseline
//       moved). The name carries no "bench" token on purpose.
//
// NEGATIVE CONTROLS (CONTRACTS D3/D26/D27): two arms compiled from this same
// source, one rule broken each, armed on a runtime proof before the verdict
// prints, PASS_REGULAR_EXPRESSION on the CAUGHT marker (never WILL_FAIL):
//   PRISM_C113_MUTANT_SILENT_FAILURE  EndDraw's HRESULT is dropped again -> a
//       rejected frame reports success with painted == 0  -> H1 red
//   PRISM_C113_MUTANT_BLACK_FRAME     the frame is accepted and cleared to black
//       -> H3 red
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "BenchFrameCapture.hpp"
#include "GalleryWindow.hpp"

using Microsoft::WRL::ComPtr;
using prism::win::gallery::GalleryWindow;
using prism::win::benchframe::CaptureOffscreen;
using prism::win::benchframe::Frame;
using prism::win::benchframe::Seam;

namespace {

int g_failures = 0;

bool CheckTrue(bool ok, const char* expr, int line) {
  if (!ok) {
    std::printf("    FAIL test_c113_bench_honesty.cpp:%d  expected true: %s\n", line, expr);
    ++g_failures;
  }
  return ok;
}

// D2DERR_WRONG_RESOURCE_DOMAIN. Spelled out instead of included from d2derr.h so
// the number in the log and the number in the assertion are the same one.
constexpr HRESULT kWrongResourceDomain = static_cast<HRESULT>(0x88990012L);

// H3's floors. Measured on this box at 1264x741: a real frame has
// painted = 193466 / 936624 (20.7%) on the C113 tree and 167729 / 936624 (17.9%)
// once C112 moves the pill's pixels to the island; the rejected frame has
// painted = 0 and nonBlack = 936624. 5% and 50% leave the real frame a 3x margin
// and still fail the canvas.
constexpr size_t kPaintedDivisor = 20;

// H4: the frame the C76 bench records, at the geometry it records it at. The
// value was re-recorded by C113 from a frame that passed the content assertions
// (rendered, hr == S_OK, painted = 193466/936624) -- NOT from the rejected frame
// 0af0b75bc5b12383 the previous constant held.
//
// C112 RE-RECORDED IT AGAIN, and this is the part a reader must not skim: C112
// moves the breadcrumb pill's fill and TEXT from the window's frame to the
// island (a DirectComposition visual above it), so the window's steady-state
// frame legitimately loses those pixels and the whole-frame digest must change.
// The move is legitimate for three MEASURED reasons, not because a constant was
// inconvenient:
//   1. the frame is still REAL -- rendered == 1, hr == S_OK,
//      painted == 164668/936624 (17.6%), and the content assertions above are
//      checked before this digest is allowed to mean anything;
//
// The value below was measured on 42d7026 (main with C109's type and C110's
// grid). It is a hash of the WHOLE FRAME's bytes, so changing the base changes
// it -- re-measure, never copy.
//   2. C112's OWN change is CONFINED TO THE PILL -- its S3b measures 0 changed
//      pixels outside the pill rectangle plus its 1 px stroke, at 1280x800 and
//      at 1028x729 (the rest of the frame moved with main, not with C112: C109
//      re-typed the list rows after C113 measured e735eb87a09d4f84);
//   3. the pill's content moved from the window's frame to the island BY DESIGN:
//      what changed is the OWNERSHIP of those pixels, not the success of the
//      render.
//
// C124 RE-RECORDED IT: this card repaints every surface (canvas #0B0D12, the
// five-tier ladder, the gold folder, the file-type glyphs, the seam), so the
// whole-frame hash of the SAME scene MUST move. Measured on the C124 tree:
// rendered == 1, hr == S_OK, geometry_matches == 1,
// painted == 192486/936624 (20.5%), digest == a36870e3bffa86ff. Re-measured with
// "c113_bench_honesty --digest --shaders <build>/Assets/Shaders", never copied.
constexpr uint64_t kDigest = 0xa36870e3bffa86ffull;
constexpr uint32_t kDigestWidth = 1264;
constexpr uint32_t kDigestHeight = 741;
constexpr size_t kNonBlackDivisor = 2;

struct Fixture {
  GalleryWindow window;
  std::vector<prism::ArchiveItem> items;
};

// The labels are parameters because H4 must paint the EXACT scene the C76 bench
// paints: the breadcrumb text is pixels, so a different root name is a different
// digest.
bool Configure(Fixture& fixture, const std::wstring& shaderDir,
               const wchar_t* rootLabel = L"c113.7z") {
  fixture.items.clear();
  for (size_t i = 0; i < 1000; ++i) {
    prism::ArchiveItem item;
    item.index = static_cast<uint32_t>(i);
    item.path = "photo_" + std::to_string(i) + ".png";
    item.isDir = false;
    item.size = 65536ull + i;
    fixture.items.push_back(std::move(item));
  }
  if (!fixture.window.Create(L"PrismArchive -- C113 bench honesty", GetModuleHandleW(nullptr))) {
    return false;
  }
  fixture.window.SetItems(&fixture.items);
  fixture.window.SetRootLabel(rootLabel);
  fixture.window.SetStatus(L"1000 items");
  fixture.window.SetSidebarRailVisible(true);
  fixture.window.SetListMode(true);
  if (!shaderDir.empty()) (void)fixture.window.EnableGlassLayer(shaderDir);
  return true;
}

bool ClientSize(GalleryWindow& window, uint32_t* width, uint32_t* height) {
  RECT rc{};
  if (!::GetClientRect(window.Hwnd(), &rc)) return false;
  *width = static_cast<uint32_t>(rc.right - rc.left);
  *height = static_cast<uint32_t>(rc.bottom - rc.top);
  return *width != 0 && *height != 0;
}

const char* HexHr(HRESULT hr, char* buffer, size_t size) {
  std::snprintf(buffer, size, "0x%08lX", static_cast<unsigned long>(hr));
  return buffer;
}

}  // namespace

int main(int argc, char** argv) {
  std::wstring shaderDir;
  std::string mutant;
  bool digestArm = false;
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--shaders" && i + 1 < argc) {
      shaderDir = prism::win::Utf8ToWide(argv[++i]);
    } else if (arg == "--mutant" && i + 1 < argc) {
      mutant = argv[++i];
    } else if (arg == "--digest") {
      digestArm = true;
    }
  }
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

  ComPtr<IWICImagingFactory> wic;
  CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
  ComPtr<ID2D1Factory> d2d;
  D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
  if (!wic || !d2d) {
    std::printf("C113 CANNOT START: no WIC/D2D factory\n");
    return 1;
  }
  std::printf("C113_INFO {\"mutant\":\"%s\"}\n", mutant.c_str());

  if (digestArm) {
    // ---- H4: the C76 pixel claim, reachable from CI ------------------------
    // The SAME window the C76 bench paints (same labels, two real WM_PAINTs,
    // the island presented on the second), captured through the PRODUCT seam.
    Fixture fixture;
    if (!Configure(fixture, shaderDir, L"c76.7z")) {
      std::printf("C113 CANNOT START: the digest fixture window could not be created\n");
      return 1;
    }
    ShowWindow(fixture.window.Hwnd(), SW_SHOWNORMAL);
    InvalidateRect(fixture.window.Hwnd(), nullptr, FALSE);
    UpdateWindow(fixture.window.Hwnd());
    InvalidateRect(fixture.window.Hwnd(), nullptr, FALSE);
    UpdateWindow(fixture.window.Hwnd());
    uint32_t digestWidth = 0;
    uint32_t digestHeight = 0;
    if (!ClientSize(fixture.window, &digestWidth, &digestHeight)) {
      std::printf("C113 CANNOT START: the digest window has no client area\n");
      return 1;
    }
    const Frame frame = CaptureOffscreen(fixture.window, wic.Get(), d2d.Get(), digestWidth,
                                         digestHeight, "H4 digest", Seam::kProduct);
    const bool framePainted = frame.rendered && frame.hr == S_OK &&
                              frame.painted >= frame.total / kPaintedDivisor;
    const bool digestGeometry = frame.width == kDigestWidth && frame.height == kDigestHeight;
    // The digest is only meaningful at the geometry it was recorded at (the
    // layout is width-driven); elsewhere the CONTENT assertions still run and
    // the bench says so instead of pretending another window is a regression.
    const bool digestOk = !digestGeometry || frame.digest == kDigest;
    char digestHr[16]{};
    std::printf("C113_DIGEST {\"digest\":\"%s\",\"expected\":\"%016llx\",\"painted\":%zu,"
                "\"non_black\":%zu,\"total\":%zu,\"rendered\":%d,\"hr\":\"%s\","
                "\"width\":%u,\"height\":%u,\"geometry_matches\":%d}\n",
                prism::win::benchframe::Hex(frame.digest).c_str(),
                static_cast<unsigned long long>(kDigest), frame.painted, frame.nonBlack,
                frame.total, static_cast<int>(frame.rendered),
                HexHr(frame.hr, digestHr, sizeof(digestHr)), frame.width, frame.height,
                static_cast<int>(digestGeometry));
    if (mutant == "drop_enddraw") {
      // The pre-C113 product: EndDraw's HRESULT is dropped, so the rejected
      // frame reports rendered == true and carries nothing. Observed, then
      // judged -- the digest MUST NOT match when nothing was painted.
      const bool injected = frame.rendered && frame.painted == 0;
      std::printf("C113 MUTANT drop_enddraw: rendered=%d painted=%zu digest=%s injected=%d\n",
                  static_cast<int>(frame.rendered), frame.painted,
                  prism::win::benchframe::Hex(frame.digest).c_str(), static_cast<int>(injected));
      if (injected && !(framePainted && digestOk)) {
        std::printf("C113 MUTATION CAUGHT: drop_enddraw\n");
        std::fflush(stdout);
        _Exit(0);
      }
      if (!injected) {
        std::printf("    FAIL the injection did not apply: the rejected frame was reported honestly\n");
        return 1;
      }
      std::printf("    FAIL the pixel claim stayed green on an empty frame\n");
      return 1;
    }
    CheckTrue(framePainted, "H4: the C76 window's frame is accepted and PAINTED", __LINE__);
    CheckTrue(digestOk, "H4: the C76 window's digest is the recorded one", __LINE__);
    if (!digestGeometry) {
      std::printf("C113 NOTE: client %ux%u is not the %ux%u the digest was recorded at; the "
                  "digest equality is skipped, this frame is %s\n",
                  frame.width, frame.height, kDigestWidth, kDigestHeight,
                  prism::win::benchframe::Hex(frame.digest).c_str());
    }
    if (g_failures != 0) {
      std::printf("C113 DIGEST FAILURES: %d\n", g_failures);
      return 1;
    }
    std::printf("ALL C113 DIGEST CHECKS PASSED\n");
    return 0;
  }

  // ---------------- H1: the rejection is a named, admitted fact --------------
  // The window paints for REAL first: that is what puts the renderer's backdrop
  // on the HWND target's device. Then the RAW seam paints into a WIC target.
  Fixture painted;
  if (!Configure(painted, shaderDir)) {
    std::printf("C113 CANNOT START: the painted fixture window could not be created\n");
    return 1;
  }
  ShowWindow(painted.window.Hwnd(), SW_SHOWNORMAL);
  InvalidateRect(painted.window.Hwnd(), nullptr, FALSE);
  UpdateWindow(painted.window.Hwnd());
  InvalidateRect(painted.window.Hwnd(), nullptr, FALSE);
  UpdateWindow(painted.window.Hwnd());
  uint32_t width = 0;
  uint32_t height = 0;
  if (!ClientSize(painted.window, &width, &height)) {
    std::printf("C113 CANNOT START: the painted window has no client area\n");
    return 1;
  }
  const Frame rejected =
      CaptureOffscreen(painted.window, wic.Get(), d2d.Get(), width, height, "H1 raw seam", Seam::kRaw);
  char hrText[16]{};
  const bool h1NamedFailure = !rejected.rendered && rejected.hr == kWrongResourceDomain;
  // A platform that hands the HWND target and the WIC target the SAME device
  // does not reject the frame; then the only honest answer left is "it is a
  // real frame". Either way a silently empty frame is a failure.
  const bool h1RealFrame = rejected.rendered && rejected.hr == S_OK &&
                           rejected.painted >= rejected.total / kPaintedDivisor;
  const bool h1 = h1NamedFailure || h1RealFrame;
  std::printf("C113_H1 {\"branch\":\"%s\",\"rendered\":%d,\"hr\":\"%s\",\"digest\":\"%s\","
              "\"painted\":%zu,\"non_black\":%zu,\"total\":%zu,\"width\":%u,\"height\":%u}\n",
              h1NamedFailure ? "device_domain" : (h1RealFrame ? "shared_device" : "silent_empty"),
              static_cast<int>(rejected.rendered), HexHr(rejected.hr, hrText, sizeof(hrText)),
              prism::win::benchframe::Hex(rejected.digest).c_str(), rejected.painted,
              rejected.nonBlack, rejected.total, width, height);
  CheckTrue(h1, "H1: the cross-device frame is either rejected with 0x88990012 or really painted",
            __LINE__);

  // ---------------- H2/H3: an accepted frame carries the scene -----------------
  // A fresh window that has NOT painted, captured through the product seam: the
  // device objects are rebuilt on the WIC target, so the picture is the scene.
  Fixture capture;
  if (!Configure(capture, shaderDir)) {
    std::printf("C113 CANNOT START: the capture fixture window could not be created\n");
    return 1;
  }
  // The steady state the product presents, one island frame BEFORE the capture.
  const bool presented = capture.window.PresentGlassFrameForTest();
  const Frame frame1 = CaptureOffscreen(capture.window, wic.Get(), d2d.Get(), width, height,
                                        "H2 product seam #1", Seam::kProduct);
  const Frame frame2 = CaptureOffscreen(capture.window, wic.Get(), d2d.Get(), width, height,
                                        "H2 product seam #2", Seam::kProduct);
  const bool h2 = frame1.rendered && frame1.hr == S_OK && frame2.rendered && frame2.hr == S_OK;
  std::printf("C113_H2 {\"presented\":%d,\"rendered\":%d,\"hr\":\"%s\",\"rendered2\":%d,"
              "\"hr2\":\"%s\"}\n",
              static_cast<int>(presented), static_cast<int>(frame1.rendered),
              HexHr(frame1.hr, hrText, sizeof(hrText)), static_cast<int>(frame2.rendered),
              HexHr(frame2.hr, hrText, sizeof(hrText)));
  CheckTrue(h2, "H2: the capture seam reports rendered == true and hr == S_OK", __LINE__);

  const auto frameIsReal = [](const Frame& frame) {
    return frame.rendered && frame.digest != 0 &&
           frame.nonBlack >= frame.total / kNonBlackDivisor &&
           frame.painted >= frame.total / kPaintedDivisor;
  };
  const bool real1 = frameIsReal(frame1);
  const bool real2 = frameIsReal(frame2);
  const bool h3 = real1 && real2;
  std::printf("C113_H3 {\"digest\":\"%s\",\"digest2\":\"%s\",\"painted\":%zu,\"non_black\":%zu,"
              "\"total\":%zu,\"painted_floor\":%zu,\"stable\":%d}\n",
              prism::win::benchframe::Hex(frame1.digest).c_str(),
              prism::win::benchframe::Hex(frame2.digest).c_str(), frame1.painted, frame1.nonBlack,
              frame1.total, frame1.total / kPaintedDivisor,
              static_cast<int>(frame1.digest == frame2.digest));
  CheckTrue(h3, "H3: a rendered frame has digest != 0, non-black and PAINTED pixels", __LINE__);
  CheckTrue(frame1.digest == frame2.digest,
            "H3: two captures of one scene are the same pixels", __LINE__);

  // ---------------- the arms --------------------------------------------------
  if (mutant == "silent_failure") {
    // The injection is real when the RAW seam, which paints with the window's
    // device cache, told the caller it had rendered a frame it did not paint.
    const bool injected = rejected.rendered && rejected.hr == S_OK && rejected.painted == 0;
    std::printf("C113 MUTANT silent_failure: rendered=%d hr=%s painted=%zu injected=%d\n",
                static_cast<int>(rejected.rendered), HexHr(rejected.hr, hrText, sizeof(hrText)),
                rejected.painted, static_cast<int>(injected));
    if (injected && !h1) {
      std::printf("C113 MUTATION CAUGHT: silent_failure\n");
      std::fflush(stdout);
      _Exit(0);
    }
    if (!injected) {
      std::printf("    FAIL the injection did not apply: the rejected frame was reported honestly\n");
      return 1;
    }
    std::printf("    FAIL H1 stayed green on a frame nobody painted\n");
    return 1;
  }
  if (mutant == "black_frame") {
    const bool injected = frame1.rendered && frame1.hr == S_OK && frame1.painted == 0;
    std::printf("C113 MUTANT black_frame: rendered=%d hr=%s painted=%zu digest=%s injected=%d\n",
                static_cast<int>(frame1.rendered), HexHr(frame1.hr, hrText, sizeof(hrText)),
                frame1.painted, prism::win::benchframe::Hex(frame1.digest).c_str(),
                static_cast<int>(injected));
    if (injected && !h3) {
      std::printf("C113 MUTATION CAUGHT: black_frame\n");
      std::fflush(stdout);
      _Exit(0);
    }
    if (!injected) {
      std::printf("    FAIL the injection did not apply: the frame still carries the scene\n");
      return 1;
    }
    std::printf("    FAIL H3 stayed green on an empty frame\n");
    return 1;
  }

  if (g_failures != 0) {
    std::printf("C113 HONESTY FAILURES: %d\n", g_failures);
    return 1;
  }
  std::printf("ALL C113 HONESTY CHECKS PASSED\n");
  return 0;
}