// PrismArchive -- C113: the ONE offscreen frame reader the benches share.
//
// WHY THIS FILE EXISTS. The C76 bench used to read its "first frame" like this:
//
//     if (!window.RenderSceneInto(target.Get())) return frame;  // frame.rendered
//
// and that line could not fire: DrawFrame called target->EndDraw() and threw
// the HRESULT away, so a frame D2D had REJECTED came back as a success. What
// D2D rejects is the C77 backdrop: on a window that has painted for real, the
// cached backdrop bitmap belongs to the HWND target's device, and sampling it
// from a WIC target fails EndDraw with D2DERR_WRONG_RESOURCE_DOMAIN
// (0x88990012). The frame that survives such a rejection carries nothing but the
// canvas clear -- and the old code hashed it and compared it to a "baseline",
// which is why C112 could re-record 0af0b75bc5b12383 without noticing that its
// picture had no scene in it at all (see the C113 suite's H1/H3 and docs/PERF.md).
//
// The reader below is deliberately unforgiving:
//   * it paints through GalleryWindow::RenderSceneForCapture, the seam that
//     rebuilds the renderer's DEVICE objects on the WIC target -- so the same
//     window can be captured again and again (the raw seam succeeds once);
//   * it records the HRESULT and prints RENDER FAILED hr=0x%08X instead of
//     returning quietly, and it STILL reads the pixels back, so a rejected
//     frame's own bytes are the evidence rather than a claim;
//   * it counts PAINTED pixels, not merely non-zero ones. The rejected frame
//     measured here is NOT black: every one of its 936624 pixels is the canvas
//     fill (B=19,G=14,R=11), so "non-black" would call it a picture. "painted"
//     (brightest channel >= 32) is 0 on that frame and ~193k on a real one.
#ifndef PRISM_TESTS_BENCH_FRAME_CAPTURE_HPP
#define PRISM_TESTS_BENCH_FRAME_CAPTURE_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "GalleryWindow.hpp"

namespace prism::win::benchframe {

// A channel at or above this is a pixel a scene actually painted. The canvas
// the window clears to is (11,13,18) -- its BRIGHTEST channel is 18, which is
// what this threshold has to clear.
//
// C124 lowered it from 32 to 24. The old number was calibrated when the rail was
// (26,29,37) and every band sat above 32; the surface ladder puts the rail at
// (18,21,28) -- max 28 -- so a real frame that paints the rail stopped counting
// it as "painted" and the "this is a real frame" gate (C112/C113) started
// failing on frames with MORE scene in them, not less. 24 separates the canvas's
// 18 from the rail's 28 with 6 levels of headroom on each side.
constexpr uint8_t kPaintedChannel = 24;

// WHICH SEAM. kProduct is the capture contract (device objects rebuilt on the
// target, so a capture always succeeds); kRaw is the pre-C113 seam, kept ONLY so
// the cross-device rejection stays observable (H1). A caller that is measuring
// pixels must use kProduct.
enum class Seam { kProduct, kRaw };

struct Frame {
  std::vector<uint8_t> bgra;
  uint32_t width = 0;
  uint32_t height = 0;
  uint64_t digest = 0;
  HRESULT hr = E_FAIL;    // what EndDraw really said (S_OK when accepted)
  bool rendered = false;  // "the frame the caller asked for is IN bgra"
  size_t total = 0;
  size_t nonBlack = 0;  // some channel != 0 -- weak: the canvas passes it
  size_t painted = 0;   // brightest channel >= kPaintedChannel -- the real one
};

inline uint64_t Fnv1a(const std::vector<uint8_t>& bytes) {
  uint64_t hash = 1469598103934665603ull;
  for (uint8_t byte : bytes) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return hash;
}

inline std::string Hex(uint64_t value) {
  char text[24]{};
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
  return std::string(text);
}

// Renders the window's scene into an offscreen WIC target and reads it back.
// EVERY failure path sets hr and prints, and leaves rendered == false: a caller
// that forgets to check still cannot mistake this for a picture, because
// painted == 0 and the line is on the console.
inline Frame CaptureOffscreen(prism::win::gallery::GalleryWindow& window,
                              IWICImagingFactory* wic, ID2D1Factory* d2d, uint32_t width,
                              uint32_t height, const char* label, Seam seam) {
  Frame frame;
  const auto note = [&](HRESULT hr, const char* why) {
    frame.hr = hr;
    std::printf("    RENDER FAILED hr=0x%08lX (%s, %s)\n", static_cast<unsigned long>(hr), why,
                label != nullptr ? label : "frame");
  };
  frame.rendered = false;
  if (wic == nullptr || d2d == nullptr || width == 0 || height == 0) {
    note(E_INVALIDARG, "bad factory or geometry");
    return frame;
  }
  Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
  HRESULT hr = wic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad,
                                 bitmap.GetAddressOf());
  if (FAILED(hr)) {
    note(hr, "CreateBitmap");
    return frame;
  }
  const D2D1_RENDER_TARGET_PROPERTIES props =
      D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                                   D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                     D2D1_ALPHA_MODE_PREMULTIPLIED));
  Microsoft::WRL::ComPtr<ID2D1RenderTarget> target;
  hr = d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf());
  if (FAILED(hr)) {
    note(hr, "CreateWicBitmapRenderTarget");
    return frame;
  }
  const bool drawn = seam == Seam::kProduct ? window.RenderSceneForCapture(target.Get())
                                            : window.RenderSceneInto(target.Get());
  if (!drawn) note(window.LastRenderError(), "render seam");
  // Read the pixels back even when EndDraw rejected the frame: the bytes are
  // the evidence (canvas fill vs scene), and a caller must be able to say what
  // it would have hashed.
  WICRect lock{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  Microsoft::WRL::ComPtr<IWICBitmapLock> locked;
  if (FAILED(bitmap->Lock(&lock, WICBitmapLockRead, locked.GetAddressOf()))) {
    if (drawn) note(E_FAIL, "WICBitmap::Lock");
    return frame;
  }
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(locked->GetStride(&stride)) || FAILED(locked->GetDataPointer(&size, &data))) {
    if (drawn) note(E_FAIL, "WICBitmapLock stride/pointer");
    return frame;
  }
  frame.bgra.resize(static_cast<size_t>(width) * height * 4u);
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * width * 4u,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4u);
  }
  frame.width = width;
  frame.height = height;
  frame.total = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < frame.total; ++i) {
    const uint8_t* pixel = &frame.bgra[i * 4u];
    const uint8_t brightest = (std::max)(pixel[0], (std::max)(pixel[1], pixel[2]));
    if (brightest != 0) ++frame.nonBlack;
    if (brightest >= kPaintedChannel) ++frame.painted;
  }
  frame.digest = Fnv1a(frame.bgra);
  frame.rendered = drawn;
  if (drawn) frame.hr = S_OK;
  return frame;
}

}  // namespace prism::win::benchframe

#endif  // PRISM_TESTS_BENCH_FRAME_CAPTURE_HPP