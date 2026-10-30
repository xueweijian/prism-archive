// Windows integration test: the solid-block single-pass sweep against the
// REAL 7z.dll. Builds a genuine solid 7z (via the 7z.exe that ships beside
// 7z.dll), harvests WIC thumbnails in one Extract call, and proves the
// abandon-and-drain contract. SKIPs loudly when 7z is not on the runner.
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

#include "../mini_test.hpp"
#include "PrismEngineBridge.hpp"
#include "SolidFixture.hpp"
#include "SolidSweeper.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring g_dllPath;
std::wstring g_exePath;

// Records what the real WIC decoder produced, for dimension assertions.
struct SpyDecoder final : prism::gallery::SolidSweepSession::IDecoder {
  explicit SpyDecoder(prism::win::sweep::WicThumbDecoder& inner) : inner_(inner) {}
  std::optional<prism::gallery::DecodedThumb> Decode(const uint8_t* data,
                                                     size_t bytes) override {
    auto thumb = inner_.Decode(data, bytes);
    if (thumb) dims.push_back({thumb->width, thumb->height});
    return thumb;
  }
  prism::win::sweep::WicThumbDecoder& inner_;
  std::vector<std::pair<UINT, UINT>> dims;
};

}  // namespace

int main(int argc, char** argv) {
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::cout << "    FAIL: COM init" << std::endl;
    return 1;
  }
  g_dllPath = prism::fixture::DllPath();
  g_exePath = prism::fixture::ExePath();
  if (g_dllPath.empty() || g_exePath.empty()) {
    std::cout << "    FAIL: 7z.exe not found beside 7z.dll (windows-latest ships it)" << std::endl;
    return 1;
  }

  namespace fs = std::filesystem;
  const fs::path work = fs::temp_directory_path() / "prism_solid_sweep";
  fs::remove_all(work);
  fs::create_directories(work);
  const std::wstring dir = work.wstring();

  // A small icon (never upscaled), a mid photo, and one larger than the box.
  if (!PRISM_CHECK(prism::fixture::WritePng(dir + L"\\icon.png", 64, 64, 0, 0, 255)))
    return 1;
  if (!PRISM_CHECK(prism::fixture::WritePng(dir + L"\\photo.png", 120, 90, 0, 255, 0)))
    return 1;
  if (!PRISM_CHECK(prism::fixture::WritePng(dir + L"\\large.png", 300, 200, 255, 0, 0)))
    return 1;
  if (!PRISM_CHECK(prism::fixture::PackSolid(g_exePath, dir, L"solid.7z",
                                             {L"icon.png", L"photo.png", L"large.png"}))) {
    std::cout << "    FAIL: 7z.exe pack failed" << std::endl;
    return 1;
  }

  prism::win::SevenZipLibrary library;
  if (!PRISM_CHECK(library.Load(g_dllPath))) return 1;
  prism::win::ArchiveReader reader(library);
  if (!PRISM_CHECK(reader.Open(dir + L"\\solid.7z"))) {
    std::cout << "    open trace: " << reader.openTrace() << std::endl;
    return 1;
  }
  const auto catalog = reader.ReadCatalog();
  if (!PRISM_CHECK_EQ(catalog.size(), size_t(3))) return 1;

  std::vector<uint32_t> indices;
  std::vector<uint64_t> sizes;
  for (const auto& item : catalog) {
    if (item.isDir) continue;
    indices.push_back(item.index);
    sizes.push_back(item.size);
  }

  prism::win::sweep::WicThumbDecoder wic;
  if (!PRISM_CHECK(wic.Initialize())) return 1;
  SpyDecoder spy(wic);
  prism::gallery::ThumbStore store{32 << 20};

  // THE contract: one Extract call, all three thumbnails harvested -- pixels
  // included: the store must hold actual BGRA payloads afterwards.
  const auto result = prism::win::sweep::RunSolidSweep(reader, indices, sizes, spy, store);
  if (!PRISM_CHECK(result.ok)) return 1;
  if (!PRISM_CHECK_EQ(result.itemsEntered, 3)) return 1;  // one pass, every item
  if (!PRISM_CHECK_EQ(result.decoded, 3)) return 1;
  if (!PRISM_CHECK_EQ(result.skipped, 0)) return 1;
  if (!PRISM_CHECK_EQ(result.fraction, 1.0)) return 1;
  if (!PRISM_CHECK_EQ(store.Count(), 3u)) return 1;
  for (uint32_t i = 0; i < indices.size(); ++i) {
    const auto* thumb = store.Lookup(indices[i]);
    if (!PRISM_CHECK(thumb != nullptr)) return 1;
    if (!PRISM_CHECK(!thumb->pixels.empty())) return 1;  // real payload
  }

  // Thumbnail geometry: aspect preserved, box-capped, never upscaled.
  // NOTE: solid mode REORDERS entries (7-Zip groups similar files into one
  // dictionary), so decode order is NOT packing order -- assert the SET.
  if (!PRISM_CHECK_EQ(spy.dims.size(), size_t(3))) return 1;
  std::vector<std::pair<UINT, UINT>> sorted = spy.dims;
  std::sort(sorted.begin(), sorted.end());
  if (!PRISM_CHECK_EQ(sorted[0].first, 64u)) return 1;    // icon kept its size
  if (!PRISM_CHECK_EQ(sorted[0].second, 64u)) return 1;
  if (!PRISM_CHECK_EQ(sorted[1].first, 120u)) return 1;   // mid photo kept size
  if (!PRISM_CHECK_EQ(sorted[1].second, 90u)) return 1;
  if (!PRISM_CHECK_EQ(sorted[2].first, 256u)) return 1;   // 300x200 -> fit box
  if (!PRISM_CHECK(sorted[2].second >= 170 && sorted[2].second <= 171)) return 1;

  // Abandon-and-drain: viewport moved on -> zero decodes, stream consumed.
  prism::gallery::ThumbStore store2{32 << 20};
  const auto abandoned =
      prism::win::sweep::RunSolidSweep(reader, indices, sizes, spy, store2,
                                       prism::QuickLookPlanner::kDefaultByteLimit,
                                       /*startAbandoned=*/true);
  if (!PRISM_CHECK(abandoned.ok)) return 1;
  if (!PRISM_CHECK_EQ(abandoned.itemsEntered, 3)) return 1;  // still consumed
  if (!PRISM_CHECK_EQ(abandoned.decoded, 0)) return 1;       // nobody decoded
  if (!PRISM_CHECK_EQ(store2.Count(), 0u)) return 1;

  std::cout << "    solid sweep: " << result.decoded << " thumbs in one pass, "
            << result.drainedBytes << " bytes streamed" << std::endl;
  std::cout << "    ALL SOLID-SWEEP CHECKS PASSED" << std::endl;
  return 0;
}
