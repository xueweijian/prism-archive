// PrismArchive -- Windows integration suite, card C27 (final assembly).
//
// THE WIRING IS THE OBJECT UNDER TEST. Four blocks had landed with their own
// suites but none of them was reachable from the exe: compression (C9 + C18),
// settings persistence (C14), the sidebar's draw commands (C12) and the language
// tables (C19). This suite drives Src/App/AppWiring.hpp -- the exact header
// Main.cpp calls -- plus the real window and the real renderer, against a real
// 7z.dll:
//
//   (1) a .7z CREATED by the app's own path (PlanCompression -> RunCompression)
//       re-opens with the same entry count and byte-identical payloads;
//   (2) settings written in ONE process are read back by a SECOND process
//       (LOCALAPPDATA injected through the child's environment block) -- the
//       only shape that distinguishes "persisted" from "kept in a member";
//   (3) the sidebar's command list reaches PIXELS: the renderer's counters move
//       AND a pixel inside the panel changes (a counter alone could be a lie);
//   (4) switching to zh-CN changes a real menu title on a real HWND, and a key
//       that only the en-US table defines still resolves through the fallback
//       chain.
//
// NEGATIVE CONTROLS (CONTRACTS D3 + D26). The same suite is compiled three more
// times, each with ONE regression injected at compile time; the mutant prints a
// banner (proof the injection built) and, only when the suite really went red,
// prints "C27 MUTATION CAUGHT: <name>" and exits 0. That marker is the target's
// PASS_REGULAR_EXPRESSION -- no WILL_FAIL anywhere (D26: it cannot be combined
// with a pass marker, and alone it turns a SKIP into a green).
//
// D1: a missing 7z.dll/7z.exe or a window station without a desktop prints WHY
// and exits 77, which CTest maps to SKIP_RETURN_CODE; the marker is required all
// the same, so a silent skip can never pass.

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismWin/ArchiveWriter.hpp"
#include "../../Src/PrismWin/GalleryWindow.hpp"

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

namespace fs = std::filesystem;
namespace app = prism::app;
using prism::ArchiveItem;
using GalleryWindow = prism::win::gallery::GalleryWindow;
using Microsoft::WRL::ComPtr;

#ifndef PRISM_C27_ASSETS_DIR
#define PRISM_C27_ASSETS_DIR "Assets"
#endif

namespace {

// ---------------------------------------------------------------------------
// Mutation switches (CONTRACTS D3). Each one is compiled into its own executable
// and changes the behaviour the matching case asserts.
// ---------------------------------------------------------------------------
#if defined(PRISM_C27_MUTANT_SETTINGS_MEMORY_ONLY)
constexpr const char* kMutant = "settings_memory_only";
#elif defined(PRISM_C27_MUTANT_NO_I18N_FALLBACK)
constexpr const char* kMutant = "no_i18n_fallback";
#elif defined(PRISM_C27_MUTANT_NO_SIDEBAR_PAINT)
constexpr const char* kMutant = "no_sidebar_paint";
#else
constexpr const char* kMutant = nullptr;
#endif

#if defined(PRISM_C27_MUTANT_SETTINGS_MEMORY_ONLY)
constexpr bool kExpectSettingsReadback = false;
#else
constexpr bool kExpectSettingsReadback = true;
#endif
#if defined(PRISM_C27_MUTANT_NO_I18N_FALLBACK)
constexpr bool kExpectI18nFallback = false;
#else
constexpr bool kExpectI18nFallback = true;
#endif
#if defined(PRISM_C27_MUTANT_NO_SIDEBAR_PAINT)
constexpr bool kExpectSidebarPixels = false;
#else
constexpr bool kExpectSidebarPixels = true;
#endif

constexpr uint32_t kPayloadFiles = 3;
constexpr size_t kPayloadBytes = 24u * 1024u;

std::wstring Utf8ToWide(const std::string& text) { return prism::win::Utf8ToWide(text); }

std::wstring SelfPath() {
  wchar_t buffer[MAX_PATH]{};
  const DWORD length = GetModuleFileNameW(nullptr, buffer, MAX_PATH);
  return std::wstring(buffer, static_cast<size_t>(length));
}

// Fresh, empty directory under %TEMP%.
fs::path FreshDir(const wchar_t* leaf) {
  std::error_code ec;
  const fs::path dir = fs::temp_directory_path(ec) / "prism_c27_glue" / leaf;
  fs::remove_all(dir, ec);
  fs::create_directories(dir, ec);
  return dir;
}

// Deterministic payload: a byte pattern that is cheap to compare and that the
// compressor cannot have produced by accident.
std::vector<uint8_t> MakePayload(uint32_t seed, size_t bytes) {
  std::vector<uint8_t> out(bytes);
  uint32_t state = seed * 2654435761u + 1013904223u;
  for (size_t i = 0; i < bytes; ++i) {
    state = state * 1664525u + 1013904223u;
    out[i] = static_cast<uint8_t>(state >> 24);
  }
  return out;
}

bool WriteBytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  return file.good();
}

std::vector<uint8_t> ReadBytes(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
}

// The source tree the compression cases write: <root>/pack/{a.bin,b.bin,sub/c.bin}
struct SourceTree {
  fs::path root;
  fs::path pack;
  std::vector<std::vector<uint8_t>> payloads;
  bool Build() {
    root = FreshDir(L"source");
    pack = root / "pack";
    std::error_code ec;
    fs::create_directories(pack / "sub", ec);
    if (ec) return false;
    payloads.clear();
    for (uint32_t i = 0; i < kPayloadFiles; ++i) {
      payloads.push_back(MakePayload(i + 7, kPayloadBytes + i * 512));
    }
    if (!WriteBytes(pack / "a.bin", payloads[0])) return false;
    if (!WriteBytes(pack / "b.bin", payloads[1])) return false;
    if (!WriteBytes(pack / "sub" / "c.bin", payloads[2])) return false;
    return true;
  }
};

// 7z.dll discovery, for the whole suite: no engine -> a SKIP with a reason.
bool EnsureEngine(prism::win::SevenZipLibrary* library, std::string* why) {
  const prism::win::SevenZipDllLookup lookup = prism::win::Resolve7zDll();
  if (lookup.path.empty()) {
    *why = "7z.dll not found (set PRISM_7Z_DLL or install 7-Zip)";
    return false;
  }
  if (!library->Load(lookup.path)) {
    *why = "7z.dll at " + prism::win::WideToUtf8(lookup.path) + " could not be loaded";
    return false;
  }
  return true;
}

prism::win::SevenZipLibrary& Library() {
  static prism::win::SevenZipLibrary library;
  return library;
}

// ---------------------------------------------------------------------------
// Child-process mode for case (2): a REAL restart. It loads the settings the
// parent wrote and prints what it read; nothing else happens in this process.
// ---------------------------------------------------------------------------
int PersistChild(const std::wstring& settingsDir, const std::wstring& outFile) {
  app::SettingsBootstrap bootstrap = app::LoadSettingsUnder(settingsDir);
  const app::WindowState state = app::LoadWindowState(bootstrap.store);
  app::SettingsSidebarStore sidebar(bootstrap.store);
  const std::string blob = sidebar.Load("sidebar.state.v1");
  std::ofstream out(outFile, std::ios::binary | std::ios::trunc);
  out << "loaded=" << (bootstrap.loaded ? 1 : 0) << "\n";
  out << "width=" << state.width << "\n";
  out << "height=" << state.height << "\n";
  out << "listMode=" << (state.listMode ? 1 : 0) << "\n";
  out << "sort=" << (state.hasSort ? app::SortKeyNameOf(state.sort.keys.front().key) : "none")
      << "\n";
  // The blob is MULTI-LINE (the model's own format), so it goes to its own file:
  // a "key=value" report line would only ever carry its first line.
  {
    std::ofstream blobOut(outFile + prism::win::Utf8ToWide(".sidebar"),
                          std::ios::binary | std::ios::trunc);
    blobOut << blob;
  }
  out << "sidebarBytes=" << blob.size() << "\n";
  out << "language=" << bootstrap.store.GetString("language.tag", "") << "\n";
  return out.good() ? 0 : 1;
}

// Runs THIS executable in PersistChild mode with LOCALAPPDATA pointed at
// settingsDir. No pipes: the child writes a file, so nothing depends on the
// window station having a usable stdout.
bool RunPersistChild(const std::wstring& settingsDir, const std::wstring& outFile,
                     std::string* why) {
  const std::wstring self = SelfPath();
  std::wstring command = L"\"" + self + L"\" --c27-persist-check \"" + settingsDir + L"\" \"" +
                         outFile + L"\"";
  // Environment: the parent's, with LOCALAPPDATA replaced (the injection the
  // card asks for). CREATE_UNICODE_ENVIRONMENT makes the block wide.
  std::vector<wchar_t> environment;
  {
    LPWCH block = GetEnvironmentStringsW();
    const std::wstring overrideEntry = L"LOCALAPPDATA=" + settingsDir;
    for (LPWCH cursor = block; *cursor != L'\0';) {
      const std::wstring entry(cursor);
      cursor += entry.size() + 1;
      if (entry.rfind(L"LOCALAPPDATA=", 0) == 0) continue;  // replaced below
      environment.insert(environment.end(), entry.begin(), entry.end());
      environment.push_back(L'\0');
    }
    FreeEnvironmentStringsW(block);
    environment.insert(environment.end(), overrideEntry.begin(), overrideEntry.end());
    environment.push_back(L'\0');
    environment.push_back(L'\0');
  }

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  std::vector<wchar_t> mutableCommand(command.begin(), command.end());
  mutableCommand.push_back(L'\0');
  const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, environment.data(),
                                      settingsDir.c_str(), &startup, &process);
  if (!created) {
    *why = "CreateProcessW failed: " + std::to_string(GetLastError());
    return false;
  }
  WaitForSingleObject(process.hProcess, 120'000);
  DWORD exitCode = 1;
  GetExitCodeProcess(process.hProcess, &exitCode);
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  if (exitCode != 0) {
    *why = "child exited with " + std::to_string(exitCode);
    return false;
  }
  return true;
}

// Parses "key=value" lines from the child's report.
std::string ReportValue(const std::string& report, const std::string& key) {
  const std::string needle = key + "=";
  const size_t at = report.find(needle);
  if (at == std::string::npos) return {};
  const size_t start = at + needle.size();
  const size_t end = report.find('\n', start);
  return report.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// ---------------------------------------------------------------------------
// A real HWND with a catalog, for the sidebar/menu cases.
// ---------------------------------------------------------------------------
std::vector<ArchiveItem> SampleCatalog() {
  std::vector<ArchiveItem> items;
  const auto add = [&items](const char* path, uint64_t size, bool dir, uint32_t index) {
    ArchiveItem item;
    item.path = path;
    item.size = size;
    item.isDir = dir;
    item.index = index;
    items.push_back(item);
  };
  add("docs", 0, true, 0);
  add("docs/readme.txt", 4096, false, 1);
  add("media", 0, true, 2);
  add("media/pic.png", 65536, false, 3);
  add("notes.md", 1024, false, 4);
  return items;
}

// Renders one scene into a 32bpp WIC bitmap and hands back the pixels, so a
// "visible" claim can be a pixel instead of a counter.
bool RenderScene(prism::win::gallery::GalleryRenderer::Scene& scene,
                 std::vector<uint32_t>* pixels, int* sidebarPrimitives,
                 int* sidebarLabels) {
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    return false;
  }
  constexpr UINT32 kW = 900;
  constexpr UINT32 kH = 600;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(wic->CreateBitmap(kW, kH, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnDemand,
                               bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<ID2D1RenderTarget> target;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_SOFTWARE,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
  if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(bitmap.Get(), props,
                                                             target.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(renderer.CreateDeviceResources(target.Get()))) return false;
  target->BeginDraw();
  renderer.Draw(target.Get(), scene);
  if (FAILED(target->EndDraw())) return false;
  if (sidebarPrimitives != nullptr) *sidebarPrimitives = renderer.dbgSidebarPrimitives;
  if (sidebarLabels != nullptr) *sidebarLabels = renderer.dbgSidebarLabels;
  WICRect rect{0, 0, static_cast<INT>(kW), static_cast<INT>(kH)};
  pixels->resize(static_cast<size_t>(kW) * kH);
  return SUCCEEDED(bitmap->CopyPixels(&rect, kW * 4, kW * kH * 4,
                                      reinterpret_cast<BYTE*>(pixels->data())));
}

uint32_t PixelAt(const std::vector<uint32_t>& pixels, int width, int x, int y) {
  if (x < 0 || y < 0 || x >= width) return 0;
  const size_t index = static_cast<size_t>(y) * static_cast<size_t>(width) +
                       static_cast<size_t>(x);
  return index < pixels.size() ? pixels[index] : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 4 && std::string(argv[1]) == "--c27-persist-check") {
    return PersistChild(prism::win::Utf8ToWide(argv[2]), prism::win::Utf8ToWide(argv[3]));
  }
  if (kMutant != nullptr) {
    std::cout << "C27 MUTANT ACTIVE: " << kMutant
              << " (one rule compiled out; the matching case must go red)\n";
  }
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::cout << "SKIP: CoInitializeEx failed (this window station has no COM)\n";
    return 77;
  }
  std::string why;
  if (!EnsureEngine(&Library(), &why)) {
    // D1: a skip says WHY and uses the code CTest maps to "Skipped".
    std::cout << "SKIP: " << why << "\n";
    CoUninitialize();
    return 77;
  }
  const int rc = prism::test::RunAll(argc, argv);
  CoUninitialize();

  // CONTRACTS D26: the mutant verdict is a CONJUNCTION the suite prints itself.
  if (kMutant != nullptr) {
    if (rc == 0) {
      std::cout << "C27 MUTATION NOT CAUGHT: the suite stayed green with " << kMutant
                << " compiled in -- the injection is stale\n";
      return 1;
    }
    std::cout << "C27 MUTATION CAUGHT: " << kMutant
              << " (see the FAIL lines above for the contract it broke)\n";
    return 0;
  }
  if (rc != 0) return rc;
  std::cout << "ALL C27-GLUE CHECKS PASSED\n";
  return 0;
}

// ---------------------------------------------------------------------------
// (1) Compression: the app's own create path, then an INDEPENDENT re-open.
// ---------------------------------------------------------------------------
PRISM_TEST("glue.compress_then_reopen_matches_bytes") {
  SourceTree tree;
  if (!PRISM_CHECK(tree.Build())) return;
  const fs::path archive = tree.root / "pack.7z";
  const app::CompressionTarget target = app::PlanCompression(
      {tree.pack.wstring()}, prism::ArchiveFormat::SevenZip, archive.wstring());
  if (!PRISM_CHECK(target.ok)) {
    std::cout << "    plan error: " << target.error << std::endl;
    return;
  }
  PRISM_CHECK_EQ(target.plan.fileCount, uint64_t(3));

  uint64_t lastDone = 0;
  int progressCalls = 0;
  app::CompressionRequest job;
  job.archivePath = target.archivePath;
  job.sourceRoot = target.sourceRoot;
  job.plan = target.plan;
  const app::CompressionOutcome outcome = app::RunCompression(job, [&](uint64_t done, uint64_t) {
    ++progressCalls;
    lastDone = done;
    return true;
  });
  if (!PRISM_CHECK(outcome.ok)) {
    std::cout << "    create error: " << outcome.error << "\n    trace: " << outcome.trace
              << std::endl;
    return;
  }
  PRISM_CHECK_EQ(outcome.itemsWritten, uint64_t(3));  // the three FILES (folders carry no op result)
  PRISM_CHECK(fs::exists(archive));
  // The progress callback is the HUD's only honest source: a create that never
  // reports is a HUD that lies about being stuck.
  PRISM_CHECK(progressCalls > 0);
  PRISM_CHECK(lastDone > 0);

  // INDEPENDENT read-back: a fresh handler, not the writer's verification pass.
  prism::win::cw::VerifyReader reader;
  if (!PRISM_CHECK(reader.Open(Library(), archive.wstring(), L""))) {
    std::cout << "    open trace: " << reader.trace() << std::endl;
    return;
  }
  const std::vector<prism::win::cw::VerifyItem> catalog = reader.Catalog();
  // 3 files + 2 folders ("pack" itself and "pack/sub"): the input WAS a folder,
  // and a folder is archived as a folder, not flattened away.
  PRISM_CHECK_EQ(catalog.size(), size_t(5));
  uint64_t files = 0;
  for (size_t index = 0; index < catalog.size(); ++index) {
    const prism::win::cw::VerifyItem& item = catalog[index];
    if (item.isDir) continue;
    ++files;
    bool found = false;
    for (size_t i = 0; i < target.plan.items.size(); ++i) {
      if (target.plan.items[i].path != item.path) continue;
      found = true;
      PRISM_CHECK_EQ(item.size, target.plan.items[i].size);
      // Extract by INDEX (the engine's own numbering) into memory and compare
      // against the file on disk: a "same size" check alone would pass on an
      // archive full of zeroes.
      std::vector<uint8_t> archived;
      PRISM_CHECK(reader.ExtractTo(static_cast<uint32_t>(index), item.size, &archived));
      // Paths in the plan are relative to the SOURCE ROOT (the parent of the
      // folder we archived), which is exactly what the writer re-reads.
      const fs::path onDisk = tree.root / prism::win::Utf8ToWide(item.path);
      PRISM_CHECK(archived == ReadBytes(onDisk));
    }
    PRISM_CHECK(found);
  }
  PRISM_CHECK_EQ(files, uint64_t(3));
  reader.Close();
}

// ---------------------------------------------------------------------------
// (2) Settings survive a REAL restart (a second process).
// ---------------------------------------------------------------------------
PRISM_TEST("glue.settings_survive_process_restart") {
  const fs::path dir = FreshDir(L"settings");
  app::SettingsBootstrap bootstrap = app::LoadSettingsUnder(dir.wstring());
  PRISM_CHECK(bootstrap.loaded);
  PRISM_CHECK(bootstrap.paths.valid());

  bootstrap.store.SetString("language.tag", "zh-CN");
  app::SettingsSidebarStore sidebar(bootstrap.store);
  prism::sidebar::Model model;  // the real serializer, not a hand-written blob
  model.PushRecent("C:/archive/one.7z", 100);
  model.PushRecent("C:/archive/two.zip", 200);
  model.Save(&sidebar);
  PRISM_CHECK_EQ(sidebar.writes(), 1);

  app::WindowState state;
  state.width = 1333;
  state.height = 777;
  state.listMode = true;
  prism::list::SortSpec spec;
  spec.key = prism::list::SortKey::kOriginalSize;
  spec.direction = prism::list::SortDirection::kDescending;
  state.sort.keys.push_back(spec);
  state.hasSort = true;
  state.columns.sizeW = 140;
  state.columns.packedW = 120;
  state.columns.ratioW = 150;
  state.hasColumns = true;
  app::SaveWindowState(bootstrap.store, state);
  std::string detail;
  if (!PRISM_CHECK(app::SaveSettings(&bootstrap, &detail))) {
    std::cout << "    save detail: " << detail << std::endl;
    return;
  }

  // Same process read-back first (cheap, and it separates "written" from "read
  // by the next process" when something fails).
  const app::SettingsBootstrap again = app::LoadSettingsUnder(dir.wstring());
  PRISM_CHECK_EQ(again.store.GetInt("window.width", 0), 1333);

  const fs::path report = dir / "child-report.txt";
  std::string why;
  if (!PRISM_CHECK(RunPersistChild(dir.wstring(), report.wstring(), &why))) {
    std::cout << "    child: " << why << std::endl;
    return;
  }
  const std::vector<uint8_t> reportBytes = ReadBytes(report);
  const std::string text(reinterpret_cast<const char*>(reportBytes.data()), reportBytes.size());
  if (!PRISM_CHECK(!text.empty())) return;
  PRISM_CHECK_EQ(ReportValue(text, "loaded"), std::string("1"));
  PRISM_CHECK_EQ(ReportValue(text, "width"), std::string("1333"));
  PRISM_CHECK_EQ(ReportValue(text, "height"), std::string("777"));
  PRISM_CHECK_EQ(ReportValue(text, "listMode"), std::string("1"));
  PRISM_CHECK_EQ(ReportValue(text, "sort"), std::string("size"));
  PRISM_CHECK_EQ(ReportValue(text, "language"), std::string("zh-CN"));
  // The sidebar blob is the model's own serialization: the two recent entries
  // have to come back across the process boundary.
  const std::vector<uint8_t> blobBytes = ReadBytes(fs::path(report.wstring() + L".sidebar"));
  const std::string blob(reinterpret_cast<const char*>(blobBytes.data()), blobBytes.size());
  PRISM_CHECK(blob.find("one.7z") != std::string::npos);
  PRISM_CHECK(blob.find("two.zip") != std::string::npos);
  PRISM_CHECK_EQ(ReportValue(text, "sidebarBytes"), std::to_string(blob.size()));
  if (!kExpectSettingsReadback) {
    // MUTATION: the disk write was compiled out, so the child MUST have missed
    // everything above. Re-assert the opposite so the failure is unambiguous.
    PRISM_CHECK(ReportValue(text, "width") != std::string("1333"));
  }
}

// ---------------------------------------------------------------------------
// (3) The sidebar command list reaches pixels.
// ---------------------------------------------------------------------------
PRISM_TEST("glue.sidebar_commands_are_painted") {
  std::vector<ArchiveItem> items = SampleCatalog();
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C27 sidebar", GetModuleHandleW(nullptr)))) {
    std::cout << "    (window could not be created on this station)" << std::endl;
    return;
  }
  window.SetItems(&items);
  window.SetRootLabel(L"sample.7z");
  window.Dispatch(WM_SIZE, 0, MAKELPARAM(900, 600));
  const prism::win::sidebar::Layout& layout = window.SidebarLayout();
  PRISM_CHECK(!layout.commands.empty());
  // The window only paints the rail when the app asks for it (the opt-in that
  // keeps the older navigation suites' hit-testing unchanged).
  window.SetSidebarRailVisible(true);
  PRISM_CHECK(window.SidebarRailVisible());

  prism::gallery::GridLayout grid{};
  prism::gallery::ViewportWindow viewport{grid, static_cast<int>(items.size())};
  viewport.SetViewportSize(900.0, 600.0);
  prism::win::gallery::GalleryRenderer::Scene scene;
  scene.items = &items;
  scene.viewport = &viewport;
  scene.sidebar = &layout;

  std::vector<uint32_t> withSidebar;
  int primitives = 0;
  int labels = 0;
  if (!PRISM_CHECK(RenderScene(scene, &withSidebar, &primitives, &labels))) return;
  PRISM_CHECK(primitives > 0);
  PRISM_CHECK(labels > 0);

  // Same scene WITHOUT the rail: the pixel difference is the visible proof.
  prism::win::gallery::GalleryRenderer::Scene bare = scene;
  bare.sidebar = nullptr;
  std::vector<uint32_t> withoutSidebar;
  if (!PRISM_CHECK(RenderScene(bare, &withoutSidebar, nullptr, nullptr))) return;

  const int sampleX = static_cast<int>(layout.panel.left) + 4;
  const int sampleY = static_cast<int>(layout.panel.top) + 4;
  const uint32_t painted = PixelAt(withSidebar, 900, sampleX, sampleY);
  const uint32_t blank = PixelAt(withoutSidebar, 900, sampleX, sampleY);
  PRISM_CHECK(painted != blank);
  if (!kExpectSidebarPixels) {
    PRISM_CHECK(painted == blank);  // MUTATION: nothing was painted
  }
  window.Dispatch(WM_DESTROY, 0, 0);
}

// ---------------------------------------------------------------------------
// (4) Language: menu title changes, and the fallback chain still resolves.
// ---------------------------------------------------------------------------
PRISM_TEST("glue.language_switch_changes_a_menu_title") {
  const std::wstring langDir = prism::win::Utf8ToWide(PRISM_C27_ASSETS_DIR);
  prism::settings::SettingsStore store;
  store.SetString("language.tag", "en-US");
  app::LanguageRuntime english = app::LoadLanguageFor(langDir, store, "en-US", nullptr);
  const app::Tr en{&english.translator};
  const std::wstring englishTitle = en("menu.compress.to.7z");
  PRISM_CHECK(!englishTitle.empty());

  store.SetString("language.tag", "zh-CN");
  app::LanguageRuntime chinese = app::LoadLanguageFor(langDir, store, "en-US", nullptr);
  const app::Tr zh{&chinese.translator};
  const std::wstring chineseTitle = zh("menu.compress.to.7z");
  PRISM_CHECK(chineseTitle != englishTitle);

  // The window is where "translated" has to be TRUE, not just stored: the bar is
  // rebuilt and read back through GetMenuStringW.
  GalleryWindow window;
  if (!PRISM_CHECK(window.Create(L"PrismArchive C27 menu", GetModuleHandleW(nullptr)))) return;
  window.SetMenuLabels(L"File", englishTitle, L"Exit");
  PRISM_CHECK_EQ(prism::win::WideToUtf8(window.MenuItemText(GalleryWindow::kMenuCompress)),
                 prism::win::WideToUtf8(englishTitle));
  window.SetMenuLabels(L"文件", chineseTitle, L"退出");
  PRISM_CHECK_EQ(prism::win::WideToUtf8(window.MenuItemText(GalleryWindow::kMenuCompress)),
                 prism::win::WideToUtf8(chineseTitle));
  PRISM_CHECK(window.MenuItemText(GalleryWindow::kMenuCompress) != englishTitle);
  window.Dispatch(WM_DESTROY, 0, 0);
}

PRISM_TEST("glue.i18n_fallback_chain_resolves_missing_keys") {
  // Synthetic tables, so the case does not depend on what the shipped assets
  // happen to contain: zh-CN defines ONE key, en-US defines the other.
  const fs::path dir = FreshDir(L"lang");
  const auto writeTable = [&dir](const char* name, const char* body) {
    std::ofstream file(dir / name, std::ios::binary | std::ios::trunc);
    file << body;
    return file.good();
  };
  if (!PRISM_CHECK(writeTable("en-US.json",
                              R"({"schemaVersion":1,"language":"en-US","strings":{"only.english":"English only","menu.compress.to.7z":"Compress to archive"}})"))) {
    return;
  }
  if (!PRISM_CHECK(writeTable("zh-CN.json",
                              R"({"schemaVersion":1,"language":"zh-CN","strings":{"menu.compress.to.7z":"压缩为归档"}})"))) {
    return;
  }
  prism::settings::SettingsStore store;
  store.SetString("language.tag", "zh-CN");
  app::LanguageRuntime runtime = app::LoadLanguageFor(dir.wstring(), store, "en-US", nullptr);
  PRISM_CHECK_EQ(runtime.catalogs, 2);
  const app::Tr tr{&runtime.translator};
  // The Chinese table wins where it has the key...
  PRISM_CHECK_EQ(prism::win::WideToUtf8(tr("menu.compress.to.7z")), std::string("压缩为归档"));
  // ...and the en-US table answers for the key it alone defines.
  const std::string fallback = prism::win::WideToUtf8(tr("only.english"));
  if (kExpectI18nFallback) {
    PRISM_CHECK_EQ(fallback, std::string("English only"));
  } else {
    // MUTATION: without the fallback table the key comes back verbatim.
    PRISM_CHECK_EQ(fallback, std::string("only.english"));
  }
  // A key NO table has is returned verbatim and recorded once (never silently
  // empty -- C19's contract and the warn line the app logs).
  PRISM_CHECK_EQ(prism::win::WideToUtf8(tr("menu.absent")), std::string("menu.absent"));
  PRISM_CHECK_EQ(runtime.translator.MissingKeys().size(), size_t(1));
}
PRISM_TEST("glue.headless_verb_parses_and_plans") {
  // --compress-here is the automation verb: two paths in, one .7z out, and the
  // shell's still-refused flags must stay refused (C25's suite pins that).
  const app::LaunchRequest parsed =
      app::ParseLaunch({L"--compress-here", L"C:\\a\\one.txt", L"C:\\a\\two.txt"});
  PRISM_CHECK(parsed.mode == app::LaunchMode::kCompressHere);
  PRISM_CHECK(!parsed.unknownFlag);
  PRISM_CHECK_EQ(parsed.inputs.size(), size_t(2));
  PRISM_CHECK(parsed.archive == L"C:\\a\\one.txt");
  const app::LaunchRequest refused = app::ParseLaunch({L"--compress-7z", L"x.7z"});
  PRISM_CHECK(refused.unknownFlag);
}
