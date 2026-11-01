// PrismArchive -- PrismWin
// CodecProbe.hpp: ask an ALREADY-LOADED 7z.dll which codecs it can decode.
//
// Why this exists (blueprint ch.2, card C17): the blueprint wants Zstd / LZ4 /
// Brotli on top of the LZMA family. Two very different ways to get them:
//
//   (a) ship / point at the 7-Zip ZS build (mcmilk), whose 7z.dll carries the
//       extra codecs *inside* the same COM surface we already drive, or
//   (b) link the codec libraries ourselves and build a private engine.
//
// (a) is only worth anything if we can (1) tell the two DLLs apart at runtime,
// and (2) never let a capability guess become a read gate (CONTRACTS D16: the
// discovery set is diagnostics, never a gate). This header is that diagnostic:
// it enumerates the DLL's handler table (GetNumberOfFormats/GetHandlerProperty2)
// and its method table (GetNumberOfMethods/GetMethodProperty), and classifies
// the result. It performs no extraction and mutates nothing.
//
// The classification half (ClassifyCodecs) is a pure function over two string
// vectors, so the interesting logic is unit-testable without any DLL at all --
// including the negative control (the stock 7-Zip 25.01 table must classify as
// "zstd handler only, no zstd method, no lz4/brotli"; conflation is the bug this
// control catches, see docs/ENGINE-ZS.md).
#pragma once

#include <windows.h>
#include <propidl.h>
#include <winver.h>  // GetFileVersionInfoW / VS_FIXEDFILEINFO (version.lib)

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SevenZipAbi.hpp"

namespace prism::win {

// ---------------------------------------------------------------------------
// Pure classification (no DLL involved)
// ---------------------------------------------------------------------------
// 7-Zip's method table reports UPPER CASE names ("ZSTD"), its handler table
// reports lower case ones ("zstd"). Both sides are folded to lower case before
// matching so the classifier never depends on that accident.
inline std::string LowerAscii(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return out;
}

struct CodecInventory {
  std::vector<std::string> formats;  // handler names, lower case ("7z", "zstd", ...)
  std::vector<std::string> methods;  // codec names, lower case ("lzma2", "zstd", ...)
  // Numeric method ids, parallel to methods. Filled by the LIVE probe only (the
  // pure classifier has no ids to work with). ZSTD = 0x4F71101, LZ4 = 0x4F71104,
  // BROTLI = 0x4F71102 -- the numbers that appear in a .7z header method list.
  std::vector<std::uint64_t> methodIds;

  std::uint32_t formatCount = 0;
  std::uint32_t methodCount = 0;

  // The three codecs this spike is about (blueprint ch.2). These are the UNION
  // of "a handler row names it" and "a method row names it".
  bool zstd = false;
  bool lz4 = false;
  bool brotli = false;

  // ...and the split view, because the two halves are different capabilities and
  // real builds differ exactly there: 7-Zip 25.01 ships a "zstd" HANDLER
  // (decompression of bare .zst only -- its License.txt credits the BSD-3 zstd
  // decoder) and no ZSTD METHOD, so it cannot read a zstd-compressed .7z and
  // cannot write one. Conflating the two would have called the stock engine a
  // ZS build (measured 2026-11-01, see docs/ENGINE-ZS.md).
  bool zstdHandler = false;
  bool zstdMethod = false;
  bool lz4Handler = false;
  bool lz4Method = false;
  bool brotliHandler = false;
  bool brotliMethod = false;
  // ZS-only extras, reported so a log is self-describing; never used as a gate.
  bool lizard = false;
  bool lz5 = false;
  bool flzma2 = false;
  // Baselines that every real 7z.dll must have. If one of these is missing the
  // probe read the table wrong, so the test asserts them: a classifier that
  // returns "false" for everything would otherwise look like a clean negative.
  bool lzma2 = false;
  bool deflate = false;

  bool HasFormat(std::string_view name) const;
  bool HasMethod(std::string_view name) const;
  // True when the DLL carries a zstd/lz4/brotli METHOD, i.e. it can round-trip
  // those codecs inside 7z/ZIP. Handler presence alone is NOT this: stock 7-Zip
  // has the .zst handler.
  bool IsZsBuild() const { return zstdMethod || lz4Method || brotliMethod; }
  // One line for the CI log: counts + the codec verdict.
  std::string Describe() const;
};

// The one place the "which codec is this" decision lives.
CodecInventory ClassifyCodecs(std::vector<std::string> formats, std::vector<std::string> methods);

// ---------------------------------------------------------------------------
// Handler CLSIDs
// ---------------------------------------------------------------------------
// 7-Zip hands out handler CLSIDs as {23170F69-40C1-278A-1000-000110<II>0000},
// where <II> is a per-handler constant compiled into the DLL (CArcInfo::Id in
// 7-Zip's RegisterArc.h), NOT the handler's position in the GetHandlerProperty2
// table. Do not "derive" it from the table index: on 7-Zip 25.01 the "7z" handler
// sits at table index 48 yet carries id 0x07, so an index-derived GUID is a
// different, wrong GUID that CreateObject rejects (the integration test pins
// exactly that pair of facts).
//
// The ids below are read from the sources of the two builds this spike covers
// and every one of them is exercised against a real DLL by win.codec_probe:
//   7-Zip 25.01 (official): zip 0x01, 7z 0x07
//   7-Zip ZS 26.02 v1.5.7:  zstd 0x0E, lz4 0x0F, lz5 0x10, lizard 0x11,
//                           brotli 0x1F
inline GUID HandlerClsid(unsigned char handlerId) {
  GUID clsid = {0x23170F69,
                0x40C1,
                0x278A,
                {0x10, 0x00, 0x00, 0x01, 0x10, handlerId, 0x00, 0x00}};
  return clsid;
}

// One row per single-stream codec handler the ZS build registers, so callers
// (and the test) never hand-type a GUID.
struct KnownCodecHandler {
  const char* codec;     // "zstd" -- the handler's name in GetHandlerProperty2
  unsigned char id;      // 0x0E   -- the CLSID id byte
  const char* extension; // "zst"  -- the usual file extension (for fixtures)
};

inline const std::vector<KnownCodecHandler>& KnownCodecHandlers() {
  static const std::vector<KnownCodecHandler> handlers = {
      {"zstd", 0x0E, "zst"},  {"lz4", 0x0F, "lz4"},   {"lz5", 0x10, "lz5"},
      {"lizard", 0x11, "liz"}, {"brotli", 0x1F, "br"}};
  return handlers;
}

// ---------------------------------------------------------------------------
// Live probe
// ---------------------------------------------------------------------------
struct CodecProbeResult {
  bool loaded = false;         // DLL mapped and the format export resolved
  bool hasFormatTable = false; // GetNumberOfFormats + GetHandlerProperty2
  bool hasMethodTable = false; // GetNumberOfMethods + GetMethodProperty
  bool alreadyLoaded = false;  // the module was already in the process
  std::wstring dllPath;
  std::string fileVersion;  // from the PE VERSIONINFO, "" when unreadable
  std::string error;        // why loaded == false
  CodecInventory inventory;

  // index of a handler by name, or -1. Lets a caller derive its CLSID.
  int FormatIndex(std::string_view name) const;
  std::string Describe() const;
};

// Probes the DLL at dllPath. An already-mapped module is reused (GetModuleHandleW)
// so probing never changes the process's module refcounts; an unmapped one is
// loaded and deliberately NOT freed -- same trade SevenZipLibrary::Load makes,
// because 7z.dll keeps process-wide state and unload/reload cycles are a
// documented 7-Zip hazard.
CodecProbeResult ProbeCodecs(const std::wstring& dllPath);

// "a.b.c.d" from the file's VERSIONINFO block; "" when the file has none.
std::string ReadFileVersion(const std::wstring& dllPath);

}  // namespace prism::win


// ---------------------------------------------------------------------------
// Implementation (header-only: PrismWin is an INTERFACE library)
// ---------------------------------------------------------------------------
namespace prism::win::detail {

// The discovery tables do NOT use the item PropID enum from PropID.h. 7-Zip
// keeps two separate enums for them (CPP/7zip/Archive/IArchive.h):
//   NArchive::kName = 0, kClassID = 1, kExtension = 2, kAddExtension = 3,
//   kUpdate = 4, kKeepName = 5, kSignature = 6, kMultiSignature = 7,
//   kSignatureOffset = 8, ...
//   NMethodPropID::kID = 0 (VT_UI8), kName = 1, kDecoder = 2, kEncoder = 3, ...
// Measured, not assumed: reading handler pid 4 (PropID.h's kpidName) returns
// VT_BOOL on both DLLs, so a first cut of this probe reported "0 formats" with
// every export resolving fine. Dumping pids 0..8 against 7-Zip 25.01 and
// 7-Zip ZS 26.02 is what identified the enums (see docs/ENGINE-ZS.md).
constexpr PROPID kHandlerNameProperty = 0;   // NArchive::kName
constexpr PROPID kMethodIdProperty = 0;      // NMethodPropID::kID, VT_UI8
constexpr PROPID kMethodNameProperty = 1;    // NMethodPropID::kName

using FnGetNumberOfMethods = HRESULT(WINAPI*)(abi::UInt32* numMethods);
using FnGetMethodProperty = HRESULT(WINAPI*)(abi::UInt32 index, PROPID propID,
                                             PROPVARIANT* value);

// PROPVARIANT holding a BSTR -> UTF-8. Always clears the variant.
inline std::string TakeVariantString(PROPVARIANT& value) {
  std::string out;
  if (value.vt == VT_BSTR && value.bstrVal) {
    const int wideLen = ::SysStringLen(value.bstrVal);
    if (wideLen > 0) {
      const int utf8Len = ::WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, wideLen, nullptr, 0,
                                                nullptr, nullptr);
      if (utf8Len > 0) {
        out.resize(static_cast<std::size_t>(utf8Len));
        ::WideCharToMultiByte(CP_UTF8, 0, value.bstrVal, wideLen, out.data(), utf8Len, nullptr,
                              nullptr);
      }
    }
  }
  ::PropVariantClear(&value);
  return out;
}

}  // namespace prism::win::detail

namespace prism::win {

inline bool CodecInventory::HasFormat(std::string_view name) const {
  for (const std::string& f : formats) {
    if (f == name) return true;
  }
  return false;
}

inline bool CodecInventory::HasMethod(std::string_view name) const {
  for (const std::string& m : methods) {
    if (m == name) return true;
  }
  return false;
}

inline std::string CodecInventory::Describe() const {
  // "handler+method" = full support, "handler" = decompress a bare stream only,
  // "method" = only inside 7z/ZIP, "no" = absent.
  auto state = [](bool handler, bool method) {
    if (handler && method) return std::string("handler+method");
    if (handler) return std::string("handler-only");
    if (method) return std::string("method-only");
    return std::string("no");
  };
  return "formats=" + std::to_string(formatCount) + " methods=" + std::to_string(methodCount) +
         " zstd=" + state(zstdHandler, zstdMethod) + " lz4=" + state(lz4Handler, lz4Method) +
         " brotli=" + state(brotliHandler, brotliMethod);
}

inline CodecInventory ClassifyCodecs(std::vector<std::string> formats,
                                     std::vector<std::string> methods) {
  CodecInventory inv;
  inv.formats.reserve(formats.size());
  for (std::string& f : formats) inv.formats.push_back(LowerAscii(f));
  inv.methods.reserve(methods.size());
  for (std::string& m : methods) inv.methods.push_back(LowerAscii(m));
  inv.formatCount = static_cast<std::uint32_t>(inv.formats.size());
  inv.methodCount = static_cast<std::uint32_t>(inv.methods.size());

  // A codec counts as present when EITHER table names it. The ZS build puts
  // zstd/lz4/brotli in both (a handler for .zst/.lz4/.br *and* a method for the
  // 7z container), but the two are different capabilities: a build with only the
  // method can still decode a zstd-compressed .7z, and one with only the handler
  // can still read a bare .zst. The flags are the union; the vectors keep the
  // distinction for the caller.
  auto present = [&inv](std::string_view name) {
    return inv.HasFormat(name) || inv.HasMethod(name);
  };
  inv.zstdHandler = inv.HasFormat("zstd");
  inv.zstdMethod = inv.HasMethod("zstd");
  inv.lz4Handler = inv.HasFormat("lz4");
  inv.lz4Method = inv.HasMethod("lz4");
  inv.brotliHandler = inv.HasFormat("brotli");
  inv.brotliMethod = inv.HasMethod("brotli");
  inv.zstd = present("zstd");
  inv.lz4 = present("lz4");
  inv.brotli = present("brotli");
  inv.lizard = present("lizard");
  inv.lz5 = present("lz5");
  inv.flzma2 = present("flzma2");
  inv.lzma2 = inv.HasMethod("lzma2");
  inv.deflate = inv.HasMethod("deflate");
  return inv;
}

inline std::string CodecProbeResult::Describe() const {
  if (!loaded) return "codec-probe: NOT LOADED (" + error + ")";
  return "codec-probe: " + std::string(fileVersion.empty() ? "?" : fileVersion) + " " +
         inventory.Describe();
}

inline int CodecProbeResult::FormatIndex(std::string_view name) const {
  const std::string want = LowerAscii(name);
  for (std::size_t i = 0; i < inventory.formats.size(); ++i) {
    if (inventory.formats[i] == want) return static_cast<int>(i);
  }
  return -1;
}

inline std::string ReadFileVersion(const std::wstring& dllPath) {
  DWORD ignored = 0;
  const DWORD size = ::GetFileVersionInfoSizeW(dllPath.c_str(), &ignored);
  if (size == 0) return {};
  std::vector<unsigned char> buffer(size);
  if (!::GetFileVersionInfoW(dllPath.c_str(), 0, size, buffer.data())) return {};
  VS_FIXEDFILEINFO* info = nullptr;
  UINT infoSize = 0;
  if (!::VerQueryValueW(buffer.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) || !info ||
      infoSize < sizeof(VS_FIXEDFILEINFO)) {
    return {};
  }
  const unsigned hi = info->dwFileVersionMS;
  const unsigned lo = info->dwFileVersionLS;
  char text[64]{};
  std::snprintf(text, sizeof(text), "%u.%u.%u.%u", hi >> 16, hi & 0xFFFFu, lo >> 16, lo & 0xFFFFu);
  return text;
}

inline CodecProbeResult ProbeCodecs(const std::wstring& dllPath) {
  CodecProbeResult result;
  result.dllPath = dllPath;

  // Reuse an already-mapped image when there is one: probing must not change the
  // module's refcount (SevenZipLibrary owns that lifetime). A fresh load is
  // deliberately not freed -- same trade SevenZipLibrary::Load documents.
  HMODULE module = ::GetModuleHandleW(dllPath.c_str());
  result.alreadyLoaded = module != nullptr;
  if (!module) module = ::LoadLibraryW(dllPath.c_str());
  if (!module) {
    result.error = "LoadLibraryW failed, GetLastError=" + std::to_string(::GetLastError());
    return result;
  }

  result.fileVersion = ReadFileVersion(dllPath);

  const auto getNumberOfFormats =
      reinterpret_cast<abi::FnGetNumberOfFormats>(::GetProcAddress(module, "GetNumberOfFormats"));
  const auto getHandlerProperty2 =
      reinterpret_cast<abi::FnGetHandlerProperty2>(::GetProcAddress(module, "GetHandlerProperty2"));
  const auto getNumberOfMethods =
      reinterpret_cast<detail::FnGetNumberOfMethods>(::GetProcAddress(module, "GetNumberOfMethods"));
  const auto getMethodProperty =
      reinterpret_cast<detail::FnGetMethodProperty>(::GetProcAddress(module, "GetMethodProperty"));

  result.loaded = true;
  result.hasFormatTable = getNumberOfFormats && getHandlerProperty2;
  result.hasMethodTable = getNumberOfMethods && getMethodProperty;

  std::vector<std::string> formats;
  std::vector<std::string> methods;
  if (result.hasFormatTable) {
    abi::UInt32 count = 0;
    if (SUCCEEDED(getNumberOfFormats(&count))) {
      for (abi::UInt32 i = 0; i < count; ++i) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        if (SUCCEEDED(getHandlerProperty2(i, detail::kHandlerNameProperty, &value))) {
          std::string name = detail::TakeVariantString(value);
          if (!name.empty()) formats.push_back(std::move(name));
        }
      }
    }
  }
  std::vector<std::uint64_t> methodIds;
  if (result.hasMethodTable) {
    abi::UInt32 count = 0;
    if (SUCCEEDED(getNumberOfMethods(&count))) {
      for (abi::UInt32 i = 0; i < count; ++i) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        std::uint64_t id = 0;
        if (SUCCEEDED(getMethodProperty(i, detail::kMethodIdProperty, &value))) {
          if (value.vt == VT_UI8) id = value.uhVal.QuadPart;
          else if (value.vt == VT_UI4) id = value.ulVal;
          ::PropVariantClear(&value);
        }
        PROPVARIANT nameValue;
        ::PropVariantInit(&nameValue);
        if (SUCCEEDED(getMethodProperty(i, detail::kMethodNameProperty, &nameValue))) {
          std::string name = detail::TakeVariantString(nameValue);
          if (!name.empty()) {
            methods.push_back(std::move(name));
            methodIds.push_back(id);
          }
        }
      }
    }
  }
  result.inventory = ClassifyCodecs(std::move(formats), std::move(methods));
  result.inventory.methodIds = std::move(methodIds);
  if (!result.hasFormatTable && !result.hasMethodTable) {
    result.error = "neither GetNumberOfFormats nor GetNumberOfMethods resolved";
    result.loaded = false;
  }
  return result;
}

}  // namespace prism::win
