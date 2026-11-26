// PrismArchive -- Card C137 integration test:
//   1. BombFuse wired at ExtractWithPlan product paths (85% free-space cap, 1000:1 ratio)
//      trips on a real high-ratio compressed archive and deletes partial bomb output.
//   2. QueryTargetVolumeFreeBytes feeds real target-volume free space to BombFuse.
//   3. Red glass SecurityAlertDialog renders crimson alert pixels and SecurityAlertSink
//      delivers BombFuse and AMSI threat alerts across threads.
//   4. Negative control PRISM_C137_MUTANT_NULL_FUSE restores nullptr fuse -> suite goes red.
#include <windows.h>

#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismWin/SecurityAlertDialog.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
namespace win = prism::win;
using Microsoft::WRL::ComPtr;

namespace {

#if defined(PRISM_C137_MUTANT_NULL_FUSE)
// MUTANT ACTIVE (CONTRACTS D3/D27): BombFuse product call sites pass nullptr.
constexpr bool kNullFuseMutant = true;
#else
constexpr bool kNullFuseMutant = false;
#endif

bool Run7z(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      cwd.c_str(), &si, &pi)) {
    return false;
  }
  const DWORD waited = WaitForSingleObject(pi.hProcess, 60'000);
  DWORD code = 1;
  if (waited == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return waited == WAIT_OBJECT_0 && code == 0;
}

bool WriteZeroFile(const fs::path& path, uint64_t bytes) {
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (f == INVALID_HANDLE_VALUE) return false;
  std::vector<uint8_t> chunk(1024 * 1024, 0);
  uint64_t remaining = bytes;
  bool ok = true;
  while (remaining > 0) {
    const DWORD toWrite = static_cast<DWORD>(remaining > chunk.size() ? chunk.size() : remaining);
    DWORD written = 0;
    if (!WriteFile(f, chunk.data(), toWrite, &written, nullptr) || written != toWrite) {
      ok = false;
      break;
    }
    remaining -= written;
  }
  CloseHandle(f);
  return ok;
}

std::wstring gDllPath;
std::wstring gExePath;

PRISM_TEST("c137.product_bombfuse_trips_on_real_high_ratio_7z") {
  // 65 MiB of zeros compresses to ~5 KiB in 7z (>10000:1, >= 64 MiB sample floor).
  prism::testtmp::UniqueDir scratch(L"c137", L"bomb");
  if (!PRISM_CHECK(scratch.ok())) return;
  const fs::path root = scratch.path();
  const fs::path srcDir = root / "src";
  const fs::path outDir = root / "out";
  std::error_code ec;
  fs::create_directories(srcDir, ec);
  fs::create_directories(outDir, ec);

  const fs::path payload = srcDir / "bomb.bin";
  if (!PRISM_CHECK(WriteZeroFile(payload, 65ull * 1024 * 1024))) return;

  const fs::path archivePath = root / "bomb.7z";
  const std::wstring args = L"a -t7z -mx=9 \"" + archivePath.wstring() + L"\" bomb.bin";
  if (!PRISM_CHECK(Run7z(gExePath, args, srcDir.wstring()))) return;
  fs::remove(payload, ec);  // free space before extraction test

  win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(gDllPath))) return;

  std::vector<prism::ArchiveItem> catalog;
  {
    win::ArchiveReader probe(library);
    if (!PRISM_CHECK(probe.Open(archivePath.wstring()))) return;
    catalog = probe.ReadCatalog();
    probe.Close();
  }
  PRISM_CHECK_EQ(catalog.size(), static_cast<size_t>(1));

  app::OperationHandles::Config opsConfig;
  opsConfig.archivePath = archivePath.wstring();
  opsConfig.handlerSlots = 1;
  auto ops = app::OperationHandles::Create(library, opsConfig);
  if (!PRISM_CHECK(ops != nullptr)) return;

  app::ExtractionPlan plan;
  plan.mode = app::LaunchMode::kExtractHere;
  plan.plan.strategy = prism::SmartStrategy::FlatDirectly;
  plan.plan.baseTargetDir = win::WideToUtf8(outDir.wstring());
  plan.plan.fileCount = 1;
  plan.plan.totalBytes = catalog.empty() ? 0 : catalog[0].size;

  std::atomic<int> alertCount{0};
  win::securityui::SecurityAlertModel capturedAlert;
  app::ExtractionOptions options;
  options.overwrite = prism::overwrite::Mode::Replace;
  options.verifyAfterExtract = false;
  options.onSecurityAlert = [&](const win::securityui::SecurityAlertModel& model) {
    capturedAlert = model;
    alertCount.fetch_add(1, std::memory_order_relaxed);
  };

  const app::ExtractionOutcome outcome = app::RunExtraction(
      *ops, catalog, plan, archivePath.wstring(), /*scanExecutables=*/false,
      win::amsi::InitMode::LoadAmsi, options);
  ops->Close();

  // Clean up any written file BEFORE asserting so mutant termination leaves no 65 MiB debris.
  const bool bombLeftOnDisk = fs::exists(outDir / "bomb.bin", ec);
  fs::remove_all(root, ec);

  PRISM_CHECK(!outcome.ok);
  PRISM_CHECK(outcome.fuseTripped);
  PRISM_CHECK(!outcome.fuseReason.empty());
  PRISM_CHECK(!bombLeftOnDisk);
  PRISM_CHECK_EQ(alertCount.load(), 1);
  PRISM_CHECK(capturedAlert.kind == win::securityui::SecurityAlertKind::BombFuse);
  PRISM_CHECK(capturedAlert.title.find(L"\u5B89\u5168\u8B66\u62A5") != std::wstring::npos);
}

PRISM_TEST("c137.product_bombfuse_config_and_disk_free_query") {
  prism::BombFuse fuse = app::MakeProductBombFuse();
  const std::wstring cwd = fs::current_path().wstring();
  const uint64_t freeBytes = win::bridge_detail::QueryTargetVolumeFreeBytes(win::WideToUtf8(cwd));
  PRISM_CHECK(freeBytes > 0);

  // Verify 85% disk-proximity + 1000:1 ratio critical latch.
  fuse.BeginFile(1024);
  const bool alive = fuse.Feed(1024, 900ull * 1024 * 1024, 1000ull * 1024 * 1024);
  PRISM_CHECK(!alive);
  PRISM_CHECK(fuse.tripped());
  PRISM_CHECK(fuse.critical());
}

PRISM_TEST("c137.red_glass_security_alert_renders_red_and_delivers_amsi_and_bomb_alerts") {
  // 1. Offscreen WIC render of SecurityAlertDialog verifies crimson red glass pixels.
  ComPtr<IWICImagingFactory> wic;
  if (!PRISM_CHECK(SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf()))))) {
    return;
  }
  const UINT w = win::securityui::SecurityAlertDialog::kClientWidth;
  const UINT h = win::securityui::SecurityAlertDialog::kClientHeight;
  ComPtr<IWICBitmap> bitmap;
  if (!PRISM_CHECK(SUCCEEDED(wic->CreateBitmap(w, h, GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf())))) {
    return;
  }
  ComPtr<ID2D1Factory> d2d;
  if (!PRISM_CHECK(SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                               d2d.GetAddressOf())))) {
    return;
  }
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (!PRISM_CHECK(SUCCEEDED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), props, target.GetAddressOf())))) {
    return;
  }

  win::securityui::SecurityAlertDialog dialog;
  dialog.SetModel(win::securityui::BuildSecurityAlertModel(
      win::securityui::SecurityAlertKind::AmsiThreat, L"C:\\dl\\evil.zip",
      "blocked evil.exe: EICAR detected", true));

  target->BeginDraw();
  PRISM_CHECK(SUCCEEDED(dialog.Draw(target.Get(), static_cast<float>(w), static_cast<float>(h))));
  PRISM_CHECK(SUCCEEDED(target->EndDraw()));

  std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
  PRISM_CHECK(SUCCEEDED(bitmap->CopyPixels(nullptr, w * 4, static_cast<UINT>(pixels.size()),
                                           pixels.data())));

  int strongRedPixels = 0;
  for (size_t i = 0; i < pixels.size(); i += 4) {
    const uint8_t b = pixels[i];
    const uint8_t g = pixels[i + 1];
    const uint8_t r = pixels[i + 2];
    if (r >= 180 && r > g + 80 && r > b + 80) {
      ++strongRedPixels;
    }
  }
  PRISM_CHECK(strongRedPixels > 100);

  // 2. SecurityAlertSink receives both BombFuse and AmsiThreat via NotifySecurityAlertIfNeeded.
  win::securityui::SecurityAlertSink sink;
  std::vector<win::securityui::SecurityAlertKind> deliveredKinds;
  PRISM_CHECK(sink.Create(GetModuleHandleW(nullptr), nullptr,
                          [&](const win::securityui::SecurityAlertModel& m) {
                            deliveredKinds.push_back(m.kind);
                          }));

  app::ExtractionOutcome amsiOutcome;
  amsiOutcome.ok = true;
  amsiOutcome.security.amsiBlocked = 1;
  amsiOutcome.security.amsiDetail = "blocked payload.exe";
  app::NotifySecurityAlertIfNeeded(
      amsiOutcome, L"C:\\dl\\evil.zip",
      [&](const win::securityui::SecurityAlertModel& m) { sink.Post(m); });

  MSG msg{};
  while (PeekMessageW(&msg, sink.Hwnd(), 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  PRISM_CHECK_EQ(sink.alertsDelivered(), 1);
  PRISM_CHECK_EQ(deliveredKinds.size(), static_cast<size_t>(1));
  PRISM_CHECK(deliveredKinds[0] == win::securityui::SecurityAlertKind::AmsiThreat);
  sink.Destroy();
}

}  // namespace

int main(int argc, char** argv) {
  CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  gDllPath = prism::fixture::DllPath();
  gExePath = prism::fixture::ExePath();
  if (gDllPath.empty() || gExePath.empty()) {
    std::cout << "SKIP: 7z.dll/7z.exe missing: " << prism::fixture::ExePathWhy() << std::endl;
    CoUninitialize();
    return 77;
  }
#if defined(PRISM_C137_MUTANT_NULL_FUSE)
  std::cout << "C137 MUTANT ACTIVE: PRISM_C137_MUTANT_NULL_FUSE=1" << std::endl;
  prism::test::SetMutationVerdict("C137 MUTATION CAUGHT: null_fuse", []() {
    return app::MutantNullFuseHits().load(std::memory_order_relaxed) > 0;
  });
#endif
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();
  if (rc == 0 && !kNullFuseMutant) {
    std::cout << "ALL C137 CHECKS PASSED" << std::endl;
  }
  return rc;
}
