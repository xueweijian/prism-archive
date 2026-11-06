// PrismArchive -- tools
// d2d_present_probe: "is my white window the APP or the MACHINE?"
//
// WHY THIS FILE EXISTS. A user reported a window with a caption and a menu bar
// over a blank client area while the accessibility tree listed the whole model
// (card C116). The app's own D2D frame was complete: the scene was built, the
// renderer drew it, and EndDraw() returned S_OK. The pixels still never reached
// the screen -- and the same thing happened to a 100-line program with no
// PrismArchive code in it, while GDI FillRect into the SAME window was visible.
// That is a machine-level Direct2D/DXGI window-present failure (virtual display
// drivers, remote sessions, hybrid-GPU output switching), and no amount of
// repainting in the app can fix it.
//
// So the first question in front of a white window is not "which repaint is
// missing", it is "does this machine present D2D at all?". This probe answers
// it in about a second, with no PrismArchive code, no 7z.dll and no settings:
//
//   CreateHwndRenderTarget -> Clear(RED) -> EndDraw()  (three times)
//   GetPixel(window DC) at a point inside the cleared area   -> d2d_pixel
//   FillRect(GREEN) with GDI and GetPixel the same way      -> gdi_pixel
//
// The two halves are the experiment: GDI and D2D go through different
// presentation paths, so the PAIR distinguishes "the app cannot draw" from
// "this machine drops Direct2D presents".
//
// OUTPUT (the last line is the verdict, machine-readable on purpose):
//   D2D_PRESENT {"enddraw":"0x00000000","d2d_pixel":"ff0000","gdi_pixel":"00ff00",
//                "verdict":"D2D_PRESENT_OK"}
//   verdict values:
//     D2D_PRESENT_OK          both paths work -- a white window is the app's bug
//     D2D_PRESENT_BROKEN      GDI shows, D2D does not -- the MACHINE drops D2D
//     D2D_AND_GDI_BROKEN      neither shows -- look at the display/session first
//     D2D_PROBE_FAILED        the render target could not even be created
//
// CONTRACTS D1: this is a DIAGNOSTIC, not a test. It depends on the machine's
// display stack, so it is deliberately NOT registered with CTest (a GPU-less CI
// runner would turn it red for the wrong reason). It always exits 0; the verdict
// line is the answer.
//
// Build and run it through tools/d2d_present_probe.ps1 (it finds the MSVC
// toolchain and compiles this file into %TEMP%).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <dxgi.h>

#include <cstdio>
#include <cstring>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace {

constexpr wchar_t kClassName[] = L"PrismD2DPresentProbe";
constexpr int kWidth = 520;
constexpr int kHeight = 360;

ID2D1Factory* g_factory = nullptr;
ID2D1HwndRenderTarget* g_target = nullptr;
HWND g_hwnd = nullptr;
HRESULT g_lastEndDraw = E_FAIL;
int g_paints = 0;

// One full D2D frame: clear the whole target to RED, present, count it.
void PaintRed() {
  if (!g_target) return;
  g_target->BeginDraw();
  g_target->Clear(D2D1::ColorF(1.0f, 0.0f, 0.0f, 1.0f));
  g_lastEndDraw = g_target->EndDraw();
  ++g_paints;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      PaintRed();
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // D2D owns every pixel
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool IsRed(COLORREF color) {
  return GetRValue(color) >= 200 && GetGValue(color) <= 64 && GetBValue(color) <= 64;
}

// The GDI control: pure GDI, next to (not on top of) the D2D rectangle, so the
// two samples describe two presentation paths on one window.
bool IsGreen(COLORREF color) {
  return GetGValue(color) >= 200 && GetRValue(color) <= 64 && GetBValue(color) <= 64;
}

void PrintPixelJson(const char* key, COLORREF color) {
  std::printf("\"%s\":\"%02x%02x%02x\"", key, (unsigned)GetRValue(color),
              (unsigned)GetGValue(color), (unsigned)GetBValue(color));
}

const char* AdapterName() {
  static char name[256] = "unknown";
  IDXGIFactory* factory = nullptr;
  if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), reinterpret_cast<void**>(&factory))) ||
      factory == nullptr) {
    return name;
  }
  IDXGIAdapter* adapter = nullptr;
  if (factory->EnumAdapters(0, &adapter) != DXGI_ERROR_NOT_FOUND && adapter != nullptr) {
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(adapter->GetDesc(&desc))) {
      WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    }
    adapter->Release();
  }
  factory->Release();
  return name;
}

}  // namespace

int main() {
  // The window must be the same size in pixels as the render target, or D2D
  // scales the clear and the sample point stops being a statement about the
  // present path. Per-monitor-v2 keeps GetClientRect in physical pixels.
  if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
    using SetDpiContext = BOOL(WINAPI*)(void*);
    auto setContext =
        reinterpret_cast<SetDpiContext>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (setContext != nullptr) (void)setContext(reinterpret_cast<void*>(-4));
  }

  std::printf("D2D_PRESENT_PROBE (no PrismArchive code involved)\n");
  BOOL composition = FALSE;
  if (SUCCEEDED(DwmIsCompositionEnabled(&composition))) {
    std::printf("  dwm_composition=%d\n", composition ? 1 : 0);
  }
  std::printf("  adapter=%s\n", AdapterName());

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory))) {
    std::printf("D2D_PRESENT {\"verdict\":\"D2D_PROBE_FAILED\",\"reason\":\"factory\"}\n");
    return 0;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = &WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  wc.lpszClassName = kClassName;
  RegisterClassExW(&wc);

  g_hwnd = CreateWindowExW(WS_EX_TOPMOST, kClassName, L"PrismArchive D2D present probe",
                           WS_OVERLAPPEDWINDOW, 80, 80, kWidth, kHeight, nullptr, nullptr,
                           wc.hInstance, nullptr);
  if (g_hwnd == nullptr) {
    std::printf("D2D_PRESENT {\"verdict\":\"D2D_PROBE_FAILED\",\"reason\":\"window\"}\n");
    return 0;
  }
  ShowWindow(g_hwnd, SW_SHOW);
  UpdateWindow(g_hwnd);

  RECT client{};
  GetClientRect(g_hwnd, &client);
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  const HRESULT created = g_factory->CreateHwndRenderTarget(
      props,
      D2D1::HwndRenderTargetProperties(
          g_hwnd, D2D1::SizeU(static_cast<UINT32>(client.right), static_cast<UINT32>(client.bottom))),
      &g_target);
  if (FAILED(created)) {
    std::printf("D2D_PRESENT {\"verdict\":\"D2D_PROBE_FAILED\",\"create\":\"0x%08lx\"}\n",
                static_cast<unsigned long>(created));
    DestroyWindow(g_hwnd);
    return 0;
  }

  // Three frames, not one: a first present can be swallowed by the compositor
  // while later ones land, and "the first frame is white" is the reported
  // symptom -- this way the verdict describes a steady state.
  for (int i = 0; i < 3; ++i) {
    PaintRed();
    InvalidateRect(g_hwnd, nullptr, FALSE);
    UpdateWindow(g_hwnd);
    Sleep(120);
  }

  HDC windowDc = GetDC(g_hwnd);
  const COLORREF d2d = GetPixel(windowDc, 40, 40);  // inside the red clear

  // The GDI control: a green square in the same window, painted AFTER the last
  // D2D present, so a lost present cannot be mistaken for a lost window.
  RECT control{220, 40, 320, 140};
  HBRUSH green = CreateSolidBrush(RGB(0, 255, 0));
  FillRect(windowDc, &control, green);
  DeleteObject(green);
  const COLORREF gdi = GetPixel(windowDc, 270, 90);
  ReleaseDC(g_hwnd, windowDc);

  std::printf("D2D_PRESENT {");
  std::printf("\"enddraw\":\"0x%08lx\",", static_cast<unsigned long>(g_lastEndDraw));
  std::printf("\"paints\":%d,", g_paints);
  PrintPixelJson("d2d_pixel", d2d);
  std::printf(",");
  PrintPixelJson("gdi_pixel", gdi);
  const char* verdict = "D2D_PRESENT_OK";
  if (!IsGreen(gdi) && !IsRed(d2d)) {
    verdict = "D2D_AND_GDI_BROKEN";
  } else if (!IsRed(d2d)) {
    verdict = "D2D_PRESENT_BROKEN";
  }
  std::printf(",\"verdict\":\"%s\"}\n", verdict);

  if (std::strcmp(verdict, "D2D_PRESENT_BROKEN") == 0) {
    std::printf(
        "  The window is white because this machine drops Direct2D HWND presents:\n"
        "  GDI drew and was seen, D2D cleared the same window to red and was not.\n"
        "  Update/roll back the GPU driver, or try a session without a virtual display\n"
        "  driver (remote-desktop/IDD adapters are the usual cause).\n");
  } else if (std::strcmp(verdict, "D2D_AND_GDI_BROKEN") == 0) {
    std::printf("  Neither path reached the screen: check the desktop session and the "
                "display driver first.\n");
  } else if (std::strcmp(verdict, "D2D_PRESENT_OK") == 0) {
    std::printf("  Both paths work on this machine: a white PrismArchive window is the\n"
                "  application's, not the display stack's. Please attach today's log\n"
                "  from %%LOCALAPPDATA%%\\PrismArchive\\logs to the issue.\n");
  }

  DestroyWindow(g_hwnd);
  return 0;
}
