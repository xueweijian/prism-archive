// Shared Windows integration fixture: locate 7z.exe beside 7z.dll, author
// PNG fixtures via WIC, and pack a genuine SOLID 7z via the CLI. Used by both
// the sweep integration test and the design snapshot (which proves the whole
// pipeline: solid archive -> single-pass sweep -> real thumbnails in tiles).
#pragma once

#include <windows.h>

#include <cstdint>
#include <cstring>
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

inline std::string Utf8String(const std::wstring& wide) {
  if (wide.empty()) return {};
  const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), needed,
                      nullptr, nullptr);
  return out;
}

// C37: name the missing HALF. The fixtures AUTHOR their archives with the 7-Zip
// CLI, so a directory holding only 7z.dll -- an extracted / portable /
// engine-only copy, which is exactly what a "green" 7-Zip leaves behind -- can
// never run them. A bare "7z.exe not found" sends the reader looking for a
// 7-Zip that is standing right there, on the drive in front of them.
// Returns "" when the CLI is available, else one sentence saying WHAT is
// missing, WHERE, and WHY the fixtures need it.
inline std::string ExePathWhy() {
  if (!ExePath().empty()) return {};
  const std::wstring dll = DllPath();
  if (dll.empty()) {
    return "PRISM_7Z_DLL is not set: point it at the 7z.dll of a FULL 7-Zip install -- the "
           "integration fixtures need 7z.exe beside it to author their archives";
  }
  const std::filesystem::path dllPath(dll);
  std::error_code ec;
  if (!std::filesystem::exists(dllPath, ec)) {
    return "PRISM_7Z_DLL points at " + Utf8String(dll) + ", which does not exist";
  }
  return "7z.dll is present at " + Utf8String(dllPath.parent_path().wstring()) +
         ", but 7z.exe (the 7-Zip command-line tool) is MISSING beside it: the integration "
         "fixtures PACK their archives with the CLI, so an engine-only / portable 7z.dll cannot "
         "run them. Install the full 7-Zip package, or copy 7z.exe next to that 7z.dll";
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
  // Author pixels through IWICBitmapLock: WIC has no SetPixels on
  // IWICBitmap (CopyPixels reads OUT -- using it here silently authored an
  // all-transparent PNG, and every "decoded" thumbnail came back zero).
  std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
  for (size_t i = 0; i < pixels.size(); i += 4) {
    pixels[i] = b;      // B
    pixels[i + 1] = g;  // G
    pixels[i + 2] = r;  // R
    pixels[i + 3] = 255;
  }
  WICRect lockRect{0, 0, static_cast<INT>(width), static_cast<INT>(height)};
  ComPtr<IWICBitmapLock> lock;
  if (FAILED(bitmap->Lock(&lockRect, WICBitmapLockWrite,
                          lock.ReleaseAndGetAddressOf()))) {
    return false;
  }
  UINT stride = 0, bufferSize = 0;
  BYTE* buffer = nullptr;
  if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&bufferSize, &buffer)) ||
      buffer == nullptr || bufferSize < static_cast<UINT>(pixels.size())) {
    return false;
  }
  std::memcpy(buffer, pixels.data(), pixels.size());
  // Release the lock before the encoder reads the bitmap.
  lock.Reset();
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

inline bool PackZip(const std::wstring& exe, const std::wstring& dir, const std::wstring& name,
                    const std::vector<std::wstring>& files) {
  std::wstring args = L"a -tzip -ms=off \"" + dir + L"\\" + name + L"\"";
  for (const auto& f : files) args += L" \"" + dir + L"\\" + f + L"\"";
  return RunSevenZip(exe, args);
}

}  // namespace prism::fixture
