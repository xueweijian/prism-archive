// PrismArchive -- PrismWin
// DpiAwareness: the process is PER-MONITOR V2 (card C120).
//
// WHY THIS FILE EXISTS. The exe shipped as a DPI-UNAWARE process: on a 125%
// monitor Windows lays the window out at 96 DPI and then bitmap-scales the whole
// frame by 1.25x, so every glyph and every 1 px line was resampled -- "all the
// text and all the hairlines are blurry" (docs/UI-AUDIT-BANDIZIP.md). The
// manifest (Src/App/PrismArchive.manifest, merged in by Src/PrismWin/
// CMakeLists.txt) is the DECLARATION; this header is the RUNTIME half, because
// a manifest can be dropped by a resource editor, replaced by a packer, or
// simply not reach the loader (a copied or renamed binary), and the process must
// still come up per-monitor.
//
// ORDER IS THE CONTRACT: ApplyProcessDpiAwareness() must run before ANY window
// is created. A window created first freezes the process's awareness at the old
// value and Windows then refuses every later change for the life of the process
// (E_ACCESSDENIED). Main.cpp calls it as the FIRST statement of wWinMain.
//
// THE LADDER (each rung is the next generation's API):
//   1. SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)
//      -- Windows 10 1703+. The only rung that gives the v2 behaviour (child
//      window DPI messages, system-scaled non-client area, no bitmap scaling).
//   2. SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE) -- Windows 8.1+.
//      Per-monitor v1: sharp, but the non-client area is scaled by us.
//   2. SetProcessDPIAware() -- Vista+. SYSTEM DPI: one scale for the whole
//      desktop. It is the pre-card behaviour and the mutation arm; a process
//      that stops here is still blurry after a monitor move.
//
// A FAILURE AT RUNG 1 IS NORMAL. When the manifest already declared PMv2,
// Windows answers E_ACCESSDENIED ("awareness is already set"), rungs 2 and 3
// fail the same way, and the effective awareness is therefore never downgraded:
// the process keeps exactly what the manifest chose. The rung is reported, not
// asserted, so a caller can log which half won.
//
// EVERYTHING IS RESOLVED THROUGH GetProcAddress. shcore.dll does not exist on
// Windows 7 and the awareness APIs are newer than some pinned SDKs; a header
// that needs a new import library just to say "make me aware" is a link error
// waiting for the first machine with an older SDK.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace prism::win::dpi {

// The PROCESS_DPI_AWARENESS values, spelled here so no caller has to include
// <shellscalingapi.h> (and link shcore.lib) to name a number.
enum class Awareness : int {
  Unavailable = -1,  // could not be asked (no shcore and no Win10 user32)
  Unaware = 0,       // the pre-card state: Windows bitmap-scales everything
  System = 1,        // one scale for the desktop (the mutation arm)
  PerMonitor = 2,    // per-monitor, v1 or v2
};

inline const char* AwarenessName(Awareness awareness) {
  switch (awareness) {
    case Awareness::Unaware:
      return "unaware";
    case Awareness::System:
      return "system";
    case Awareness::PerMonitor:
      return "per-monitor";
    case Awareness::Unavailable:
    default:
      return "unavailable";
  }
}

// True for the one state this card exists to reach.
inline bool IsPerMonitor(Awareness awareness) {
  return awareness == Awareness::PerMonitor;
}

namespace detail {

// DPI_AWARENESS_CONTEXT is an opaque HANDLE and its documented values are small
// negative integers. Spelled numerically so this header compiles against an SDK
// that predates the enum: -4 is PER_MONITOR_AWARE_V2 in every shipped SDK.
using DpiAwarenessContext = HANDLE;

inline DpiAwarenessContext ContextFromValue(int value) {
  return reinterpret_cast<DpiAwarenessContext>(static_cast<INT_PTR>(value));
}

inline DpiAwarenessContext PerMonitorAwareV2() { return ContextFromValue(-4); }

using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(DpiAwarenessContext);
using SetProcessDpiAwarenessFn = HRESULT(WINAPI*)(int);
using GetProcessDpiAwarenessFn = HRESULT(WINAPI*)(HANDLE, int*);
using GetThreadDpiAwarenessContextFn = DpiAwarenessContext(WINAPI*)();
using GetAwarenessFromDpiAwarenessContextFn = int(WINAPI*)(DpiAwarenessContext);
using AreDpiAwarenessContextsEqualFn = BOOL(WINAPI*)(DpiAwarenessContext, DpiAwarenessContext);
using GetDpiForSystemFn = UINT(WINAPI*)();

inline HMODULE User32() {
  static HMODULE module = ::GetModuleHandleW(L"user32.dll");
  return module;
}

// Loaded on demand and kept loaded: the alternative (FreeLibrary) would unload
// shcore out from under a call in another thread for no gain.
inline HMODULE Shcore() {
  static HMODULE module = ::LoadLibraryW(L"shcore.dll");
  return module;
}

template <typename Fn>
inline Fn Resolve(HMODULE module, const char* name) {
  if (module == nullptr) return nullptr;
  return reinterpret_cast<Fn>(::GetProcAddress(module, name));
}

}  // namespace detail

// What the CURRENT THREAD's context is. Before the first window exists the
// thread inherits the process's awareness, so this is the same answer; a thread
// may also opt into a narrower/wider context of its own (SetThreadDpiAwareness-
// Context), and then only this one describes the window it creates.
inline Awareness QueryThreadAwareness() {
  if (auto context = detail::Resolve<detail::GetThreadDpiAwarenessContextFn>(
          detail::User32(), "GetThreadDpiAwarenessContext")) {
    if (auto awareness = detail::Resolve<detail::GetAwarenessFromDpiAwarenessContextFn>(
            detail::User32(), "GetAwarenessFromDpiAwarenessContext")) {
      const int value = awareness(context());
      if (value >= 0 && value <= 2) return static_cast<Awareness>(value);
    }
  }
  return Awareness::Unavailable;
}

// True when the calling thread's context IS the predefined PER_MONITOR_AWARE_V2
// one -- the difference between "per-monitor" and "the v2 behaviour this card
// asks for". Awareness alone cannot tell v1 from v2: both answer
// PROCESS_PER_MONITOR_DPI_AWARE. False on a Windows that predates the context
// API, where the manifest's dpiAware=true/pm is the best available answer.
inline bool ThreadContextIsPerMonitorV2() {
  auto equal = detail::Resolve<detail::AreDpiAwarenessContextsEqualFn>(
      detail::User32(), "AreDpiAwarenessContextsEqual");
  auto context = detail::Resolve<detail::GetThreadDpiAwarenessContextFn>(
      detail::User32(), "GetThreadDpiAwarenessContext");
  if (!equal || !context) return false;
  return equal(context(), detail::PerMonitorAwareV2()) != FALSE;
}

// What the process's awareness is RIGHT NOW.
//
// shcore!GetProcessDpiAwareness is the direct answer, and it is asked through a
// REAL process handle (the pseudo-handle GetCurrentProcess() is not documented
// to satisfy this API, and a query that silently fails would make every caller
// believe "unavailable"). When shcore is absent (Windows 7) the thread's own
// context answers instead: before the first window exists the thread inherits
// the process awareness, and after it exists the main thread is the one that
// matters anyway.
inline Awareness QueryProcessAwareness() {
  if (auto get = detail::Resolve<detail::GetProcessDpiAwarenessFn>(
          detail::Shcore(), "GetProcessDpiAwareness")) {
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                   ::GetCurrentProcessId());
    if (process != nullptr) {
      int value = -1;
      const HRESULT hr = get(process, &value);
      ::CloseHandle(process);
      if (SUCCEEDED(hr) && value >= 0 && value <= 2) {
        return static_cast<Awareness>(value);
      }
    }
  }
  return QueryThreadAwareness();
}

// The desktop's DPI (96 == 100%). Diagnostic only -- it says what the monitor
// the window happens to be on reports, never what the process is allowed to do
// with it. Resolved dynamically for the same reason as everything above: the
// declaration is gated on WINVER >= 0x0605 and a pinned SDK would fail to link.
inline unsigned SystemDpi() {
  if (auto get = detail::Resolve<detail::GetDpiForSystemFn>(detail::User32(),
                                                            "GetDpiForSystem")) {
    const UINT dpi = get();
    if (dpi != 0) return dpi;
  }
  HDC dc = ::GetDC(nullptr);
  if (dc == nullptr) return 96;
  const int dpi = ::GetDeviceCaps(dc, LOGPIXELSY);
  ::ReleaseDC(nullptr, dc);
  return dpi > 0 ? static_cast<unsigned>(dpi) : 96;
}

// NEGATIVE CONTROL (CONTRACTS D3/D27). PRISM_C120_MUTANT_UNAWARE compiles the
// pre-card runtime rung -- system DPI only -- into the same source. The probe
// that runs it has NO manifest, so this call is the only thing that decides the
// process's awareness and c120.d1 MUST go red. MutationActive() is the runtime
// proof the injected branch REALLY ran, which is what lets the mutant print
// CAUGHT without lying (D3: a mutant build can be red for an unrelated reason).
#if defined(PRISM_C120_MUTANT_UNAWARE)
inline unsigned& MutationHits() {
  static unsigned hits = 0;
  return hits;
}
inline bool MutationActive() { return MutationHits() != 0; }
inline const char* MutationName() { return "unaware_runtime_fallback"; }
#endif

struct ApplyResult {
  Awareness before = Awareness::Unavailable;  // what the process had on entry
  Awareness after = Awareness::Unavailable;   // what it has when this returns
  bool accepted = false;   // the awareness is set (by us, or already by the loader)
  // "manifest" (already per-monitor before this call: the manifest won) |
  // "permonitorv2" | "permonitor" | "system" (the fallback rungs) | "none".
  const char* rung = "none";
};

// The runtime half of the card. Idempotent and safe to call twice: once the
// awareness is set, Windows answers E_ACCESSDENIED to every setter, the rung
// stays "none" and "after" keeps telling the truth.
inline ApplyResult ApplyProcessDpiAwareness() {
  ApplyResult result;
  result.before = QueryProcessAwareness();

#if defined(PRISM_C120_MUTANT_UNAWARE)
  // MUTATION (CONTRACTS D3/D27): the pre-card rung. The process becomes
  // SYSTEM-DPI aware, so on a 125% monitor Windows still bitmap-scales the whole
  // window by 1.25x. c120.d1 (process >= per-monitor) MUST go red.
  ++MutationHits();
  result.accepted = ::SetProcessDPIAware() != FALSE;
  if (result.accepted) result.rung = "system";
#else
  // ALREADY THERE: stop. Measured on Windows 11, in a process the manifest made
  // per-monitor, SetProcessDPIAware() answers TRUE and changes nothing -- so
  // running the ladder anyway would log "rung=system" for a process that never
  // left PMv2. A rung that reports success while changing nothing is a lie the
  // next reader would trust, and the setters cannot improve on what the loader
  // already did. The measured state stays the only authority.
  if (IsPerMonitor(result.before)) {
    result.accepted = true;
    result.rung = "manifest";
    result.after = result.before;
    return result;
  }
  if (auto set = detail::Resolve<detail::SetProcessDpiAwarenessContextFn>(
          detail::User32(), "SetProcessDpiAwarenessContext")) {
    if (set(detail::PerMonitorAwareV2()) != FALSE) {
      result.accepted = true;
      result.rung = "permonitorv2";
    }
  }
  if (!result.accepted) {
    if (auto set = detail::Resolve<detail::SetProcessDpiAwarenessFn>(
            detail::Shcore(), "SetProcessDpiAwareness")) {
      // PROCESS_PER_MONITOR_DPI_AWARE == 2.
      if (SUCCEEDED(set(2))) {
        result.accepted = true;
        result.rung = "permonitor";
      }
    }
  }
  if (!result.accepted) {
    if (::SetProcessDPIAware() != FALSE) {
      result.accepted = true;
      result.rung = "system";
    }
  }
#endif

  result.after = QueryProcessAwareness();
  return result;
}

}  // namespace prism::win::dpi
