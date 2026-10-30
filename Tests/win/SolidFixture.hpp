// Shared Windows integration fixture: locate 7z.exe beside 7z.dll, author
// PNG fixtures via WIC, and pack a genuine SOLID 7z via the CLI. Used by both
// the sweep integration test and the design snapshot (which proves the whole
// pipeline: solid archive -> single-pass sweep -> real thumbnails in tiles).
#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <wincodec.h>
#include <wrl/client.h>

namespace prism::fixture {

using Microsoft::WRL::ComPtr;

inline std::wstring DllPath() {
  wchar_t value[MAX_PATH];
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", value, ARRAYSIZE(value)) == 0) return {};
  return value;
}

inline std::wstring ExePath() {
  const std::wstring dll = DllPath();
  if (dll.empty()) return {};
  const std::filesystem::path exe = std::filesystem::path(dll).parent_path() / L"7z.exe";
  return std::filesystem::exists(exe) ? exe.wstring() : std::wstring();
}

inline bool WritePng(const std::wstring& path, UINT width, UINT height, uint8_t b, uint8_t g,
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
  if (FAILED(bitmap->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()),
                                pixels.data()))) {
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

inline bool RunSevenZip(const std::wstring& exe, const std::wstring& args) {
  std::wstring cmd = L"\"" + exe + L"\" " + args;
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

// Packs `files` (relative names) from `dir` into a solid 7z at `dir\name`.
inline bool PackSolid(const std::wstring& exe, const std::wstring& dir, const std::wstring& name,
                      const std::vector<std::wstring>& files) {
  std::wstring args = L"a -t7z -ms=on \"" + dir + L"\\" + name + L"\"";
  for (const auto& f : files) args += L" \"" + dir + L"\\" + f + L"\"";
  return RunSevenZip(exe, args);
}

}  // namespace prism::fixture
