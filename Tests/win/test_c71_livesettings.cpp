// PrismArchive -- C71 (Windows lane): the four settings that had NO runtime
// consumer now have a measurable one.
//
// The defect (measured by C69's checklist, recorded in docs/USAGE.md): the store
// persisted appearance.accentColor, appearance.glassIntensity, extract.overwrite
// and extract.verifyAfterExtract, and no product path read any of them.
//
// WHAT THIS SUITE PROVES, WITH PIXELS / BYTES / COUNTS:
//   1. ACCENT      -- a real GalleryRenderer frame (WIC software render target)
//                     samples the progress-capsule fill at one fixed coordinate
//                     before and after applying the saved colour. The expected
//                     value is derived from the parsed colour (never typed as a
//                     literal twice), and the theme token is the negative base.
//   2. GLASS       -- the action island's fill at one fixed pixel must equal
//                     CompositeOver(EffectiveGlassSurface(token, intensity), backdrop).
//                     Two intensities must produce two different pixels.
//   3. OVERWRITE   -- a REAL 7z archive extracted over an existing file with
//                     each of replace/skip/rename; the on-disk BYTES and the
//                     callback counters are the verdict. "ask" goes through the
//                     injected prompt (one prompt, no real dialog).
//   4. VERIFY      -- a deliberately corrupted 7z (a byte flipped inside the
//                     good payload marker): with the setting ON the operation
//                     reports failure and verifyFailed; with it OFF the same
//                     plan succeeds.
//
// NEGATIVE CONTROLS (D3/D26/D27): four binaries compile THIS source with one
// consumer removed. Each arms mini_test with the runtime counter that proves
// the injected branch ran, and prints exactly one CAUGHT marker at the first
// red assertion. CTest pins that marker (no WILL_FAIL + regex stacking, D26).
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/App/AppWiring.hpp"
#include "../../Src/PrismCore/Appearance.hpp"
#include "../../Src/PrismCore/OverwritePolicy.hpp"
#include "../../Src/PrismCore/ThemeTokens.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "SolidFixture.hpp"
#include "UniqueTempDir.hpp"

using Microsoft::WRL::ComPtr;
namespace ap = prism::appearance;
namespace ow = prism::overwrite;
namespace th = prism::theme;
namespace fs = std::filesystem;

namespace {

constexpr UINT32 kWidth = 1280;
constexpr UINT32 kHeight = 800;

IWICImagingFactory* gWic = nullptr;
bool g_skip = false;

struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
  int Channel(int x, int y, int shift) const {
    return static_cast<int>((At(x, y) >> shift) & 0xFFu);
  }
};

bool RenderScene(const prism::win::gallery::GalleryRenderer::Scene& scene, UINT32 width,
                 UINT32 height, Frame* frame) {
  prism::win::gallery::GalleryRenderer renderer;
  if (FAILED(renderer.Initialize())) return false;
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
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

  WICRect rc{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) return false;
  UINT stride = 0;
  UINT size = 0;
  BYTE* data = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
    return false;
  }
  frame->width = width;
  frame->height = height;
  frame->pixels.resize(static_cast<size_t>(width) * height);
  for (UINT32 y = 0; y < height; ++y) {
    std::memcpy(frame->pixels.data() + static_cast<size_t>(y) * width,
                data + static_cast<size_t>(y) * stride, static_cast<size_t>(width) * 4);
  }
  return true;
}

int ChannelDelta(UINT32 a, UINT32 b) {
  int sum = 0;
  for (int shift = 0; shift <= 16; shift += 8) {
    sum += std::abs(static_cast<int>((a >> shift) & 0xFFu) -
                    static_cast<int>((b >> shift) & 0xFFu));
  }
  return sum;
}

bool NearPixel(UINT32 measured, const th::Rgba& expected, int tolerance,
               const char* what) {
  const int mb = static_cast<int>(std::lround(expected.b * 255.0f));
  const int mg = static_cast<int>(std::lround(expected.g * 255.0f));
  const int mr = static_cast<int>(std::lround(expected.r * 255.0f));
  const int db = std::abs(static_cast<int>(measured & 0xFFu) - mb);
  const int dg = std::abs(static_cast<int>((measured >> 8) & 0xFFu) - mg);
  const int dr = std::abs(static_cast<int>((measured >> 16) & 0xFFu) - mr);
  const bool ok = db <= tolerance && dg <= tolerance && dr <= tolerance;
  if (!ok) {
    std::printf("    [pixel] %s measured=(%d,%d,%d) expected=(%d,%d,%d)\n", what,
                static_cast<int>(measured & 0xFFu), static_cast<int>((measured >> 8) & 0xFFu),
                static_cast<int>((measured >> 16) & 0xFFu), mb, mg, mr);
  }
  return ok;
}

// C91: the canvas is no longer a literal on the renderer -- it resolves from the
// theme, so the expectation asks the SAME entry the renderer asks (C71's scenes
// leave the refraction layer off, so the flat canvas is the pixel on screen).
th::Rgba Backdrop() { return prism::chrome::CanvasColor(th::Variant::Midnight); }

prism::layout::Layout WindowLayout() {
  return prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                /*sidebarVisible=*/false, /*listMode=*/false);
}

bool ApplyAccent(const char* hex) {
  prism::settings::SettingsStore store;
  if (!store.SetString("appearance.accentColor", hex)) return false;
  ap::Apply(store);
  return true;
}

bool ApplyIntensity(double intensity) {
  prism::settings::SettingsStore store;
  if (!store.SetNumber("appearance.glassIntensity", intensity)) return false;
  ap::Apply(store);
  return true;
}

}  // namespace

// ===========================================================================
// 1. ACCENT -> progress capsule pixels
// ===========================================================================

PRISM_TEST("accent: the saved colour reaches the progress bar pixel") {
  th::live::Reset();
  const prism::layout::Layout layout = WindowLayout();
  const prism::layout::Rect capsule = prism::layout::ProgressCapsuleRect(layout);
  PRISM_CHECK(!capsule.Empty());
  const int cx = capsule.left + 10 + 4;
  const int cy = (capsule.top + capsule.bottom) / 2;

  auto scene = [&layout]() {
    prism::win::gallery::GalleryRenderer::Scene s;
    s.hud.phase = prism::HudPhase::Expanded;
    s.hud.percent = 50;
    s.layout = &layout;
    return s;
  };

  Frame tokenFrame;
  if (!PRISM_CHECK(ApplyAccent(ap::kDefaultAccentHex))) return;
  if (!PRISM_CHECK(RenderScene(scene(), kWidth, kHeight, &tokenFrame))) return;
  const th::Rgba tokenAccent = th::EffectiveAccent(th::Variant::Midnight);
  const UINT32 tokenPixel = tokenFrame.At(cx, cy);
  PRISM_CHECK(NearPixel(tokenPixel, tokenAccent, 3, "token accent"));

  constexpr const char* kCustom = "#FF8800";
  if (!PRISM_CHECK(ApplyAccent(kCustom))) return;
  th::Rgba parsed;
  PRISM_CHECK(ap::ParseHexColor(kCustom, &parsed));
  const UINT32 customPixel = tokenFrame.At(cx, cy);  // placeholder replaced below
  (void)customPixel;
  Frame customFrame;
  if (!PRISM_CHECK(RenderScene(scene(), kWidth, kHeight, &customFrame))) return;
  const UINT32 sampled = customFrame.At(cx, cy);
  PRISM_CHECK(NearPixel(sampled, parsed, 3, "custom accent"));
  // D2: the difference is a value, not "it changed".
  PRISM_CHECK(ChannelDelta(sampled, tokenPixel) > 60);
  std::printf("    [accent] token=%06lX custom=%06lX\n",
              static_cast<unsigned long>(tokenPixel), static_cast<unsigned long>(sampled));
  th::live::Reset();
}

// ===========================================================================
// 2. GLASS INTENSITY -> action island pixels
// ===========================================================================

PRISM_TEST("glass: two intensities give two pixels, each matching token*intensity") {
  th::live::Reset();
  const prism::layout::Layout layout = WindowLayout();
  const prism::actionbar::Layout band = prism::actionbar::Compute(layout);

  prism::win::gallery::GalleryRenderer::Scene scene;
  prism::actionbar::Model model;
  model.SetContext(prism::actionbar::Context{true, true, false});
  scene.actions = &model;
  scene.breadcrumb = L"C71.7z";
  scene.layout = &layout;

  const prism::actionbar::Rect& button = band.buttons[1].rect;
  const int px = button.left + 6;
  const int py = button.top + 5;  // flat island fill, no glyph

  auto renderAt = [&](double intensity, Frame* frame, th::Rgba* expected) {
    if (!ApplyIntensity(intensity)) return false;
    // Resolve the expectation WHILE this intensity is live: the runtime is
    // process-global, so evaluating it after the second render would compare
    // the first frame with the second intensity (a false red).
    *expected =
        th::CompositeOver(th::EffectiveGlassSurface(th::Variant::Midnight), Backdrop());
    return RenderScene(scene, kWidth, kHeight, frame);
  };

  Frame low;
  Frame high;
  th::Rgba lowExpected;
  th::Rgba highExpected;
  if (!PRISM_CHECK(renderAt(0.30, &low, &lowExpected))) return;
  if (!PRISM_CHECK(renderAt(0.90, &high, &highExpected))) return;
  const UINT32 lowPixel = low.At(px, py);
  const UINT32 highPixel = high.At(px, py);
  PRISM_CHECK(NearPixel(lowPixel, lowExpected, 4, "intensity 0.30"));
  PRISM_CHECK(NearPixel(highPixel, highExpected, 4, "intensity 0.90"));
  PRISM_CHECK(ChannelDelta(lowPixel, highPixel) > 15);
  std::printf("    [glass] low=%06lX high=%06lX alpha=%.3f/%.3f\n",
              static_cast<unsigned long>(lowPixel), static_cast<unsigned long>(highPixel),
              static_cast<double>(lowExpected.a), static_cast<double>(highExpected.a));
  th::live::Reset();
}

// ===========================================================================
// 3/4. Extraction fixtures
// ===========================================================================

namespace {

struct EngineFixture {
  std::unique_ptr<prism::testtmp::UniqueDir> dir;
  std::wstring root;
  std::wstring exe;
  std::wstring dll;
  bool ready = false;
  std::string why;
};

EngineFixture& Engine() {
  static EngineFixture fixture;
  return fixture;
}

bool RequireEngine() {
  EngineFixture& fx = Engine();
  if (fx.ready) return true;
  if (!fx.why.empty()) {
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.dll = prism::fixture::DllPath();
  fx.exe = prism::fixture::ExePath();
  if (fx.dll.empty() || fx.exe.empty()) {
    fx.why = prism::fixture::ExePathWhy();
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.dir = std::make_unique<prism::testtmp::UniqueDir>(L"c71", L"engine");
  if (!fx.dir->ok()) {
    fx.why = "cannot create the scratch directory: " + fx.dir->error();
    std::printf("    [skip] %s\n", fx.why.c_str());
    g_skip = true;
    return false;
  }
  fx.root = fx.dir->path().wstring();
  fx.ready = true;
  return true;
}

bool WriteText(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::string ReadText(const fs::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

std::wstring Join(const std::wstring& dir, const wchar_t* name) {
  return dir + L"\\" + name;
}

struct ArchiveFixture {
  std::wstring archivePath;
  std::vector<prism::ArchiveItem> catalog;
  prism::win::SevenZipLibrary library;
  bool ready = false;
};

bool OpenFixture(ArchiveFixture* out, const wchar_t* name) {
  if (!RequireEngine()) return false;
  EngineFixture& engine = Engine();
  out->archivePath = Join(engine.root, name);
  if (!out->library.Load(engine.dll)) return false;
  prism::win::ArchiveReader probe(out->library);
  if (!probe.Open(out->archivePath)) {
    std::printf("    [fixture] open failed: %s\n", probe.openTrace().c_str());
    return false;
  }
  out->catalog = probe.ReadCatalog();
  probe.Close();
  out->ready = !out->catalog.empty();
  return out->ready;
}

std::shared_ptr<prism::app::OperationHandles> MakeOps(
    const std::wstring& archive, prism::win::SevenZipLibrary& library) {
  prism::app::OperationHandles::Config config;
  config.archivePath = archive;
  config.handlerSlots = 2;
  config.backlog = 4;
  std::string why;
  auto ops = prism::app::OperationHandles::Create(library, config, &why);
  if (!ops) std::printf("    [ops] create failed: %s\n", why.c_str());
  return ops;
}

prism::app::ExtractionPlan FlatPlan(const std::wstring& targetDir) {
  prism::app::ExtractionPlan plan;
  plan.mode = prism::app::LaunchMode::kExtractHere;
  plan.plan.strategy = prism::SmartStrategy::FlatDirectly;
  plan.plan.baseTargetDir = prism::win::WideToUtf8(targetDir);
  plan.plan.reason = "C71 test: extract the archive tree verbatim";
  return plan;
}

}  // namespace

PRISM_TEST("overwrite: replace/skip/rename change the bytes on disk") {
  th::live::Reset();
  ArchiveFixture fixture;
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path input = root / L"in";
  fs::create_directories(input);
  if (!PRISM_CHECK(WriteText(input / L"note.txt", "archive-payload"))) return;
  if (!PRISM_CHECK(prism::fixture::RunSevenZip(
          Engine().exe, L"a -t7z -mx=0 -ms=off \"" + Join(Engine().root, L"pack.7z") +
                             L"\" \"" + (input / L"note.txt").wstring() + L"\""))) {
    return;
  }
  if (!PRISM_CHECK(OpenFixture(&fixture, L"pack.7z"))) return;
  const fs::path out = root / L"out";
  fs::create_directories(out);
  const fs::path target = out / L"note.txt";

  auto reset = [&]() {
    fs::remove_all(out);
    fs::create_directories(out);
    return WriteText(target, "user-file");
  };

  auto extract = [&](ow::Mode mode, ow::PromptFn prompt, prism::app::ExtractionOutcome* outcome) {
    auto ops = MakeOps(fixture.archivePath, fixture.library);
    if (!ops) return false;
    prism::app::ExtractionOptions options;
    options.overwrite = mode;
    options.prompt = std::move(prompt);
    options.verifyAfterExtract = false;
    *outcome = prism::app::RunExtraction(*ops, fixture.catalog,
                                         FlatPlan(out.wstring()), fixture.archivePath,
                                         /*scanExecutables=*/false,
                                         prism::win::amsi::InitMode::LoadAmsi, options);
    ops->Close();
    return true;
  };

  // REPLACE ---------------------------------------------------------------
  if (!PRISM_CHECK(reset())) return;
  prism::app::ExtractionOutcome outcome;
  if (!PRISM_CHECK(extract(ow::Mode::Replace, {}, &outcome))) return;
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(ReadText(target), std::string("archive-payload"));

  // SKIP ------------------------------------------------------------------
  if (!PRISM_CHECK(reset())) return;
  if (!PRISM_CHECK(extract(ow::Mode::Skip, {}, &outcome))) return;
  PRISM_CHECK(outcome.ok);  // skipping an existing file is not a failure
  PRISM_CHECK_EQ(ReadText(target), std::string("user-file"));
  PRISM_CHECK_EQ(outcome.filesWritten, uint64_t(0));

  // RENAME ----------------------------------------------------------------
  if (!PRISM_CHECK(reset())) return;
  if (!PRISM_CHECK(extract(ow::Mode::Rename, {}, &outcome))) return;
  PRISM_CHECK(outcome.ok);
  PRISM_CHECK_EQ(ReadText(target), std::string("user-file"));
  PRISM_CHECK_EQ(ReadText(out / L"note (1).txt"), std::string("archive-payload"));
  PRISM_CHECK_EQ(outcome.filesWritten, uint64_t(1));

  // ASK -> the injected prompt decides (never a real dialog) ---------------
  if (!PRISM_CHECK(reset())) return;
  int prompts = 0;
  std::string asked;
  auto prompt = [&prompts, &asked](const std::string& path) {
    ++prompts;
    asked = path;
    return ow::Choice::Skip;
  };
  if (!PRISM_CHECK(extract(ow::Mode::Ask, prompt, &outcome))) return;
  PRISM_CHECK_EQ(prompts, 1);
  PRISM_CHECK(asked.find("note.txt") != std::string::npos);
  PRISM_CHECK_EQ(ReadText(target), std::string("user-file"));

  // ASK -> Rename is honoured too.
  if (!PRISM_CHECK(reset())) return;
  prompts = 0;
  auto rename = [&prompts](const std::string&) {
    ++prompts;
    return ow::Choice::Rename;
  };
  if (!PRISM_CHECK(extract(ow::Mode::Ask, rename, &outcome))) return;
  PRISM_CHECK_EQ(prompts, 1);
  PRISM_CHECK_EQ(ReadText(out / L"note (1).txt"), std::string("archive-payload"));
}

PRISM_TEST("verify: a corrupted entry fails only when verifyAfterExtract is on") {
  th::live::Reset();
  if (!RequireEngine()) return;
  const fs::path root = fs::path(Engine().root);
  const fs::path input = root / L"vin";
  fs::create_directories(input);
  const std::string marker = "PRISM-C71-CORRUPT-MARKER-0123456789";
  std::string good(2048, 'G');
  std::string bad = marker + std::string(64 * 1024, 'B');
  if (!PRISM_CHECK(WriteText(input / L"a_good.bin", good))) return;
  if (!PRISM_CHECK(WriteText(input / L"b_bad.bin", bad))) return;
  const std::wstring archive = Join(Engine().root, L"corrupt.7z");
  if (!PRISM_CHECK(prism::fixture::RunSevenZip(
          Engine().exe, L"a -t7z -mx=0 -ms=off \"" + archive + L"\" \"" +
                             (input / L"a_good.bin").wstring() + L"\" \"" +
                             (input / L"b_bad.bin").wstring() + L"\""))) {
    return;
  }
  // Flip one byte INSIDE the bad payload's unique marker: the 7z header stays
  // valid (the archive still opens), the entry's CRC no longer matches.
  {
    std::fstream file(archive, std::ios::in | std::ios::out | std::ios::binary);
    file.seekg(0, std::ios::end);
    const std::streamoff size = file.tellg();
    file.seekg(0, std::ios::beg);
    std::string bytes(static_cast<size_t>(size), '\0');
    file.read(bytes.data(), size);
    const size_t at = bytes.find(marker);
    if (!PRISM_CHECK(at != std::string::npos)) return;
    bytes[at] = static_cast<char>(bytes[at] ^ 0x5A);
    file.seekp(0, std::ios::beg);
    file.write(bytes.data(), size);
    file.flush();
  }

  ArchiveFixture fixture;
  if (!PRISM_CHECK(OpenFixture(&fixture, L"corrupt.7z"))) return;

  // The GOOD entry is the extraction target; the corrupt one is skipped by the
  // plan. Extraction therefore succeeds, and only the verification pass can
  // notice the corruption -- which is the difference the setting must make.
  uint32_t badIndex = 0xFFFFFFFFu;
  for (const prism::ArchiveItem& item : fixture.catalog) {
    if (item.path.find("b_bad.bin") != std::string::npos) badIndex = item.index;
  }
  if (!PRISM_CHECK(badIndex != 0xFFFFFFFFu)) return;

  const fs::path out = root / L"vout";
  auto run = [&](bool verify, prism::app::ExtractionOutcome* outcome) {
    fs::remove_all(out);
    fs::create_directories(out);
    auto ops = MakeOps(fixture.archivePath, fixture.library);
    if (!ops) return false;
    prism::app::ExtractionPlan plan = FlatPlan(out.wstring());
    plan.plan.ignoredIndices.insert(badIndex);
    prism::app::ExtractionOptions options;
    options.overwrite = ow::Mode::Replace;
    options.verifyAfterExtract = verify;
    *outcome = prism::app::RunExtraction(*ops, fixture.catalog, plan, fixture.archivePath,
                                         /*scanExecutables=*/false,
                                         prism::win::amsi::InitMode::LoadAmsi, options);
    ops->Close();
    return true;
  };

  prism::app::ExtractionOutcome off;
  if (!PRISM_CHECK(run(false, &off))) return;
  PRISM_CHECK(off.ok);
  PRISM_CHECK(!off.verified);
  PRISM_CHECK_EQ(off.filesWritten, uint64_t(1));

  prism::app::ExtractionOutcome on;
  if (!PRISM_CHECK(run(true, &on))) return;
  PRISM_CHECK(!on.ok);
  PRISM_CHECK(on.verified);
  PRISM_CHECK(on.verifyFailed);
  PRISM_CHECK(on.error.find("verification") != std::string::npos);
}

int main() {
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    std::puts("FAIL: COM init failed");
    return 1;
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_IWICImagingFactory,
                              reinterpret_cast<void**>(&gWic)))) {
    std::puts("FAIL: WIC factory");
    return 1;
  }
#if defined(PRISM_C71_MUTANT_IGNORE_ACCENT)
  std::puts("C71 MUTANT ACTIVE: ignore_accent");
  prism::test::SetMutationVerdict("C71 MUTATION CAUGHT: ignore_accent", [] {
    return th::live::MutantAccentHits() > 0;
  });
#elif defined(PRISM_C71_MUTANT_IGNORE_GLASS)
  std::puts("C71 MUTANT ACTIVE: ignore_glass");
  prism::test::SetMutationVerdict("C71 MUTATION CAUGHT: ignore_glass", [] {
    return th::live::MutantGlassHits() > 0;
  });
#elif defined(PRISM_C71_MUTANT_IGNORE_OVERWRITE)
  std::puts("C71 MUTANT ACTIVE: ignore_overwrite");
  prism::test::SetMutationVerdict("C71 MUTATION CAUGHT: ignore_overwrite", [] {
    return prism::win::MutantOverwriteHits() > 0;
  });
#elif defined(PRISM_C71_MUTANT_IGNORE_VERIFY)
  std::puts("C71 MUTANT ACTIVE: ignore_verify");
  prism::test::SetMutationVerdict("C71 MUTATION CAUGHT: ignore_verify", [] {
    return prism::app::MutantVerifyHits() > 0;
  });
#endif
  const int failures = prism::test::RunAll(0, nullptr);
  if (g_skip) {
    std::printf("SKIP: %s\n", Engine().why.c_str());
    return 77;
  }
  if (failures != 0) return 1;
  std::puts("ALL C71 LIVE-SETTINGS CHECKS PASSED");
  return 0;
}
