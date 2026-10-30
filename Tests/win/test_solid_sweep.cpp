// Windows integration test: the solid-block single-pass sweep against the
// REAL 7z.dll. Builds a genuine solid 7z (via the 7z.exe that ships beside
// 7z.dll), harvests WIC thumbnails in one Extract call, and proves the
// abandon-and-drain contract. SKIPs loudly when 7z is not on the runner.
#include <windows.h>

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
#include "SolidSweeper.hpp"

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace {

std::wstring g_dllPath;
std::wstring g_exePath;

bool LocateSevenZip() {
  wchar_t envValue[MAX_PATH];
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", envValue, ARRAYSIZE(envValue)) == 0) {
    return false;
  }
  g_dllPath = envValue;
  std::filesystem::path exe = std::filesystem::path(g_dllPath).parent_path() / L"7z.exe";
  if (!std::filesystem::exists(exe)) return false;
  g_exePath = exe.wstring();
  return true;
}

// --- WIC PNG fixtures -------------------------------------------------------
bool WritePng(const std::wstring& path, UINT width, UINT height, uint8_t b, uint8_t g,
              uint8_t r) {
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
    return false;
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapCacheOnDemand, bitmap.ReleaseAndGetAddressOf()))) {
    return false;
  }
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = b;
    pixels[i + 1] = g;
    pixels[i + 2] = r;
    pixels[i + 3] = 255;
  }
  if (FAILED(bitmap->CopyPixels(nullptr, width * 4,
                                static_cast<UINT>(pixels.size()), pixels.data()))) {
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.ReleaseAndGetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
  ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                    encoder.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return false;
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> bag;
  if (FAILED(encoder->CreateNewFrame(frame.ReleaseAndGetAddressOf(),
                                     bag.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(frame->Initialize(bag.Get())) || FAILED(frame->SetSize(width, height))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) return false;
  if (FAILED(frame->WriteSource(bitmap.Get(), nullptr))) return false;
  return SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

bool RunSevenZip(const std::wstring& args) {
  std::wstring cmd = L"\"" + g_exePath + L"\" " + args;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                      nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 60'000);
  DWORD code = 1;
  GetExitCodeProcess(pi.hProcess, &code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return code == 0;
}

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
  if (!LocateSevenZip()) {
    std::cout << "    FAIL: 7z.exe not found beside 7z.dll (windows-latest ships it)" << std::endl;
    return 1;
  }

  namespace fs = std::filesystem;
  const fs::path work = fs::temp_directory_path() / "prism_solid_sweep";
  fs::remove_all(work);
  fs::create_directories(work);
  const std::wstring dir = work.wstring();

  // A small icon (never upscaled), a mid photo, and one larger than the box.
  if (!PRISM_CHECK(WritePng(dir + L"\\icon.png", 64, 64, 0, 0, 255))) return 1;
  if (!PRISM_CHECK(WritePng(dir + L"\\photo.png", 120, 90, 0, 255, 0))) return 1;
  if (!PRISM_CHECK(WritePng(dir + L"\\large.png", 300, 200, 255, 0, 0))) return 1;
  if (!PRISM_CHECK(RunSevenZip(L"a -t7z -ms=on \"" + dir + L"\\solid.7z\" " + dir +
                               L"\\icon.png " + dir + L"\\photo.png " + dir + L"\\large.png"))) {
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
  prism::gallery::ThumbnailLru lru{32 << 20};

  // THE contract: one Extract call, all three thumbnails harvested.
  const auto result = prism::win::sweep::RunSolidSweep(reader, indices, sizes, spy, lru);
  if (!PRISM_CHECK(result.ok)) return 1;
  if (!PRISM_CHECK_EQ(result.itemsEntered, 3)) return 1;  // one pass, every item
  if (!PRISM_CHECK_EQ(result.decoded, 3)) return 1;
  if (!PRISM_CHECK_EQ(result.skipped, 0)) return 1;
  if (!PRISM_CHECK_EQ(result.fraction, 1.0)) return 1;
  if (!PRISM_CHECK_EQ(lru.Count(), 3u)) return 1;

  // Thumbnail geometry: aspect preserved, box-capped, never upscaled.
  if (!PRISM_CHECK_EQ(spy.dims.size(), size_t(3))) return 1;
  if (!PRISM_CHECK_EQ(spy.dims[0].first, 64u)) return 1;    // icon kept its size
  if (!PRISM_CHECK_EQ(spy.dims[0].second, 64u)) return 1;
  if (!PRISM_CHECK_EQ(spy.dims[1].first, 120u)) return 1;   // mid photo kept size
  if (!PRISM_CHECK_EQ(spy.dims[1].second, 90u)) return 1;
  if (!PRISM_CHECK_EQ(spy.dims[2].first, 256u)) return 1;   // 300x200 -> fit box
  if (!PRISM_CHECK(spy.dims[2].second >= 170 && spy.dims[2].second <= 171)) return 1;

  // Abandon-and-drain: viewport moved on -> zero decodes, stream consumed.
  prism::gallery::ThumbnailLru lru2{32 << 20};
  const auto abandoned =
      prism::win::sweep::RunSolidSweep(reader, indices, sizes, spy, lru2,
                                       prism::QuickLookPlanner::kDefaultByteLimit,
                                       /*startAbandoned=*/true);
  if (!PRISM_CHECK(abandoned.ok)) return 1;
  if (!PRISM_CHECK_EQ(abandoned.itemsEntered, 3)) return 1;  // still consumed
  if (!PRISM_CHECK_EQ(abandoned.decoded, 0)) return 1;       // nobody decoded
  if (!PRISM_CHECK_EQ(lru2.Count(), 0u)) return 1;

  std::cout << "    solid sweep: " << result.decoded << " thumbs in one pass, "
            << result.drainedBytes << " bytes streamed" << std::endl;
  std::cout << "    ALL SOLID-SWEEP CHECKS PASSED" << std::endl;
  return 0;
}
