// PrismArchive -- PrismWin
// C120, PROCESS LEVEL: the executable really is PER-MONITOR V2.
//
// WHY A PROCESS TEST. The card can be "done" three ways that are all invisible
// in a header: the manifest file exists but is not merged into the exe; the
// runtime call is compiled but never runs; or both are right and Windows
// disagrees. Only a PROCESS can answer "what am I", so this suite runs inside
// three separately linked executables and reads the loader's own verdict:
//
//   c120_dpi_probe            manifest MERGED + runtime call  (the shipped shape)
//   c120_dpi_probe_runtime    NO manifest, runtime call only  (the fallback half)
//   c120_dpi_probe_mutant     NO manifest + PRISM_C120_MUTANT_UNAWARE
//                             (the pre-card rung: SetProcessDPIAware, system DPI)
//
// D1 (process >= PROCESS_PER_MONITOR_DPI_AWARE) is asserted by ask-the-OS:
// shcore!GetProcessDpiAwareness on a real process handle. That value is 0 on the
// pre-card build, which is what makes this suite red before the fix -- and the
// mutant arm exists so it stays a MEASURED claim (CONTRACTS D3/D27: the mutant
// prints "C120 MUTATION CAUGHT: ..." at the first red assertion and leaves 0,
// and CTest's PASS_REGULAR_EXPRESSION is the verdict -- no WILL_FAIL anywhere).
//
// D2 (the SHIPPED exe's embedded manifest contains PerMonitorV2) is asserted by
// reading RT_MANIFEST out of PrismArchive.exe itself, loaded as a data file:
// the manifest is what the loader reads, so the bytes are the contract -- not
// the source file that was supposed to reach it.
#include <windows.h>

#include <cstdio>
#include <string>

#include "../mini_test.hpp"
#include "../../Src/PrismWin/DpiAwareness.hpp"

#pragma comment(lib, "user32.lib")  // GetDC (the GDI fallback in SystemDpi)
#pragma comment(lib, "gdi32.lib")   // GetDeviceCaps

namespace dpi = prism::win::dpi;

namespace {

// RT_MANIFEST == 24. Spelled here rather than included from winuser.h so the
// check does not depend on which SDK macro spelling the include pulls in.
constexpr WORD kRtManifest = 24;

// The SHIPPED binary under test, handed in as argv[1] by the CTest entry
// ($<TARGET_FILE:PrismArchive>). As an ARGUMENT rather than a compile-time
// literal: a path baked into -D goes through three quoters (CMake, Ninja, the
// shell) and a backslash that survives two of them is a red CI round.
std::wstring g_appExe;

std::wstring Utf8ToWide(const char* text) {
  if (text == nullptr || *text == '\0') return {};
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
  if (length <= 1) return {};
  std::wstring wide(static_cast<std::size_t>(length - 1), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text, -1, wide.data(), length);
  return wide;
}

bool Contains(const std::string& haystack, const char* needle) {
  return haystack.find(needle) != std::string::npos;
}

// The manifest resource of an EXE on disk, byte for byte. LoadLibraryEx with
// LOAD_LIBRARY_AS_IMAGE_RESOURCE maps it without running a byte of its code.
bool ManifestBytes(const wchar_t* path, std::string* out) {
  if (path == nullptr || out == nullptr) return false;
  // AS_IMAGE_RESOURCE: map the file as a resource-only module without running
  // an instruction of it. (Combining it with AS_DATAFILE is legal but pointless
  // here; one flag means one behaviour to reason about.)
  HMODULE module = ::LoadLibraryExW(path, nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE);
  if (module == nullptr) {
    std::printf("  (cannot open %ls: error %lu)\n", path, ::GetLastError());
    return false;
  }
  bool ok = false;
  HRSRC resource = ::FindResourceW(module, MAKEINTRESOURCEW(1),
                                   MAKEINTRESOURCEW(kRtManifest));
  if (resource != nullptr) {
    const DWORD size = ::SizeofResource(module, resource);
    HGLOBAL loaded = ::LoadResource(module, resource);
    if (loaded != nullptr && size > 0) {
      const void* bytes = ::LockResource(loaded);
      if (bytes != nullptr) {
        out->assign(static_cast<const char*>(bytes), static_cast<std::size_t>(size));
        ok = true;
      }
    }
  }
  ::FreeLibrary(module);
  return ok;
}

}  // namespace

// ---------------------------------------------------------------- assertion D1
PRISM_TEST("c120.d1: the process is PER-MONITOR (>= PROCESS_PER_MONITOR_DPI_AWARE)") {
  const dpi::Awareness process = dpi::QueryProcessAwareness();
  const dpi::Awareness thread = dpi::QueryThreadAwareness();
  std::printf("c120 d1: process=%s (%d) thread=%s (%d) system_dpi=%u\n",
              dpi::AwarenessName(process), static_cast<int>(process),
              dpi::AwarenessName(thread), static_cast<int>(thread), dpi::SystemDpi());
  // THE assertion: 2 == PROCESS_PER_MONITOR_DPI_AWARE. The pre-card build
  // answers 0 (UNAWARE); the mutant answers 1 (SYSTEM) -- both red.
  PRISM_CHECK(dpi::IsPerMonitor(process));
  PRISM_CHECK(dpi::IsPerMonitor(thread));
  PRISM_CHECK(process != dpi::Awareness::Unavailable);
}

// ---------------------------------------------------------------- assertion D2
PRISM_TEST("c120.d2: the executed image carries a PerMonitorV2 manifest") {
#if defined(PRISM_C120_EXPECT_MANIFEST)
  std::string manifest;
  const std::wstring self = [&] {
    wchar_t buffer[MAX_PATH * 2]{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return std::wstring(buffer, buffer + length);
  }();
  const bool loaded = ManifestBytes(self.c_str(), &manifest);
  std::printf("c120 d2: own manifest = %zu bytes (loaded=%d)\n", manifest.size(),
              loaded ? 1 : 0);
  PRISM_CHECK(loaded);
  PRISM_CHECK(Contains(manifest, "PerMonitorV2"));
  PRISM_CHECK(Contains(manifest, "true/pm"));
#else
  // This build deliberately has NO manifest (the runtime-only and mutant arms),
  // so there is nothing here to assert; the D1 case is the one that must speak.
  std::puts("c120 d2: this arm carries no manifest by design (runtime-only)");
#endif
}

// ---------------------------------------------------------------- assertion D2b
PRISM_TEST("c120.d2: the SHIPPED exe declares PerMonitorV2") {
#if defined(PRISM_C120_EXPECT_APP_EXE)
  // THE shipped binary. Missing here means the CTest entry lost its argument --
  // a registration defect, not a skip, so it fails loudly.
  PRISM_CHECK(!g_appExe.empty());
  if (g_appExe.empty()) return;
  std::string manifest;
  const bool loaded = ManifestBytes(g_appExe.c_str(), &manifest);
  std::printf("c120 d2: %ls -> %zu bytes (loaded=%d)\n", g_appExe.c_str(), manifest.size(),
              loaded ? 1 : 0);
  PRISM_CHECK(loaded);
  PRISM_CHECK(Contains(manifest, "PerMonitorV2"));
  PRISM_CHECK(Contains(manifest, "true/pm"));
  // The generated half must still be there: the installer and the shell verbs
  // depend on requestedExecutionLevel=asInvoker (an elevated app could not be
  // launched from Explorer's context menu without a UAC prompt).
  PRISM_CHECK(Contains(manifest, "asInvoker"));
#else
  std::puts("c120 d2b: this arm is not pointed at the shipped exe (runtime-only)");
#endif
}

int main(int argc, char** argv) {
#if defined(PRISM_C120_MUTANT_UNAWARE)
  // CONTRACTS D3/D27: CAUGHT only when the injected rung really fired.
  ::prism::test::SetMutationVerdict("C120 MUTATION CAUGHT: unaware_runtime_fallback",
                                    [] { return dpi::MutationActive(); });
#endif
  if (argc > 1) g_appExe = Utf8ToWide(argv[1]);
  // The process decision, taken BEFORE any window exists -- exactly where
  // Main.cpp takes it. In a test binary, main() is the earliest point there is.
  const dpi::ApplyResult applied = dpi::ApplyProcessDpiAwareness();
  std::printf("c120: dpi awareness before=%s after=%s rung=%s\n",
              dpi::AwarenessName(applied.before), dpi::AwarenessName(applied.after),
              applied.rung);
  const int failures = ::prism::test::RunAll(argc, argv);
  if (failures != 0) {
    // A mutant that failed to print CAUGHT must stay RED: exit non-zero, and the
    // required marker is still missing from stdout.
    std::puts("C120 DPI CHECKS FAILED");
    return 1;
  }
  std::puts("ALL C120 DPI CHECKS PASSED");
  return 0;
}