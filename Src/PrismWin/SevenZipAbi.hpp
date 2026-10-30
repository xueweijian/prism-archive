// PrismArchive -- PrismWin
// Minimal, self-contained copy of the 7-Zip COM ABI (IArchive.h / IStream.h /
// IPassword.h) so PrismCore stays 100% Windows-free.
//
// These vtables are a *binary contract* with 7z.dll: they are validated by
// Tests/win/test_engine_bridge.cpp, which loads the real DLL and round-trips an
// archive on every Windows CI run. Do not reorder without that test going green.
#pragma once

#include <windows.h>
#include <propidl.h>

namespace prism::win::abi {

using UInt32 = unsigned int;
using UInt64 = unsigned long long;
using Int32 = int;
using Int64 = long long;

// ---- IID / CLSID -----------------------------------------------------------
// {23170F69-40C1-278A-0000-000600600000}
constexpr GUID IID_IInArchive = {0x23170F69, 0x40C1, 0x278A,
                                 {0x00, 0x00, 0x00, 0x06, 0x00, 0x06, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000300030000}
constexpr GUID IID_IInStream = {0x23170F69, 0x40C1, 0x278A,
                                {0x00, 0x00, 0x00, 0x03, 0x00, 0x03, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000300020000}  (Guid.txt: family 03, member 02)
constexpr GUID IID_ISequentialOutStream = {0x23170F69, 0x40C1, 0x278A,
                                           {0x00, 0x00, 0x00, 0x03, 0x00, 0x02, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000300010000}
constexpr GUID IID_ISequentialInStream = {0x23170F69, 0x40C1, 0x278A,
                                          {0x00, 0x00, 0x00, 0x03, 0x00, 0x01, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000300060000}  -- member 06; member 04 is IOutStream!
constexpr GUID IID_IStreamGetSize = {0x23170F69, 0x40C1, 0x278A,
                                     {0x00, 0x00, 0x00, 0x03, 0x00, 0x06, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000500100000}  (IPassword.h: family 05, member 10)
constexpr GUID IID_ICryptoGetTextPassword = {0x23170F69, 0x40C1, 0x278A,
                                             {0x00, 0x00, 0x00, 0x05, 0x00, 0x10, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000600100000}  (IArchive.h: family 06, member 10)
constexpr GUID IID_IArchiveOpenCallback = {0x23170F69, 0x40C1, 0x278A,
                                           {0x00, 0x00, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00}};
// {23170F69-40C1-278A-0000-000600200000}  (IArchive.h: family 06, member 20)
constexpr GUID IID_IArchiveExtractCallback = {0x23170F69, 0x40C1, 0x278A,
                                              {0x00, 0x00, 0x00, 0x06, 0x00, 0x20, 0x00, 0x00}};

// Format CLSIDs (CreateObject). Handler GUIDs use a DIFFERENT pattern from
// interface IIDs -- Guid.txt: {23170F69-40C1-278A-1000-000110xx0000}.
constexpr GUID CLSID_CFormat7z = {0x23170F69, 0x40C1, 0x278A,
                                  {0x10, 0x00, 0x00, 0x01, 0x10, 0x07, 0x00, 0x00}};
constexpr GUID CLSID_CFormatZip = {0x23170F69, 0x40C1, 0x278A,
                                   {0x10, 0x00, 0x00, 0x01, 0x10, 0x01, 0x00, 0x00}};

// ---- Streams ---------------------------------------------------------------
struct ISequentialInStream : public IUnknown {
  STDMETHOD(Read)(void* data, UInt32 size, UInt32* processedSize) PURE;
};

struct ISequentialOutStream : public IUnknown {
  STDMETHOD(Write)(const void* data, UInt32 size, UInt32* processedSize) PURE;
};

struct IInStream : public ISequentialInStream {
  STDMETHOD(Seek)(Int64 offset, UInt32 seekOrigin, UInt64* newPosition) PURE;
};

struct IStreamGetSize : public IUnknown {
  STDMETHOD(GetSize)(UInt64* size) PURE;
};

// ---- Progress & callbacks --------------------------------------------------
struct IProgress : public IUnknown {
  STDMETHOD(SetTotal)(UInt64 total) PURE;
  STDMETHOD(SetCompleted)(const UInt64* completeValue) PURE;
};

// NOTE: the two-pointer SetTotal/SetCompleted are *additional* vtable slots;
// they hide (but do not replace) the IProgress ones declared above.
struct IArchiveOpenCallback : public IProgress {
  STDMETHOD(SetTotal)(const UInt64* files, const UInt64* bytes) PURE;
  STDMETHOD(SetCompleted)(const UInt64* files, const UInt64* bytes) PURE;
};

struct IArchiveExtractCallback : public IProgress {
  STDMETHOD(GetStream)(UInt32 index, ISequentialOutStream** outStream,
                       Int32 askExtractMode) PURE;
  STDMETHOD(PrepareOperation)(Int32 askExtractMode) PURE;
  STDMETHOD(SetOperationResult)(Int32 operationResult) PURE;
};

struct ICryptoGetTextPassword : public IUnknown {
  STDMETHOD(CryptoGetTextPassword)(BSTR* password) PURE;
};

// ---- IInArchive ------------------------------------------------------------
struct IInArchive : public IUnknown {
  STDMETHOD(Open)(IInStream* stream, const UInt64* maxCheckStartPosition,
                  IArchiveOpenCallback* openCallback) PURE;
  STDMETHOD(Close)() PURE;
  STDMETHOD(GetNumberOfItems)(UInt32* numItems) PURE;
  STDMETHOD(GetProperty)(UInt32 index, PROPID propID, PROPVARIANT* value) PURE;
  STDMETHOD(Extract)(const UInt32* indices, UInt32 numItems, Int32 testMode,
                     IArchiveExtractCallback* extractCallback) PURE;
  STDMETHOD(GetArchiveProperty)(PROPID propID, PROPVARIANT* value) PURE;
  STDMETHOD(GetNumberOfProperties)(UInt32* numProperties) PURE;
  STDMETHOD(GetPropertyInfo)(UInt32 index, BSTR* name, PROPID* propID,
                             VARTYPE* varType) PURE;
  STDMETHOD(GetNumberOfArchiveProperties)(UInt32* numProperties) PURE;
  STDMETHOD(GetArchivePropertyInfo)(UInt32 index, BSTR* name, PROPID* propID,
                                    VARTYPE* varType) PURE;
};

// ---- 7z.dll exports --------------------------------------------------------
using FnCreateObject = HRESULT(WINAPI*)(const GUID* clsid, const GUID* iid, void** outObject);
using FnGetNumberOfFormats = HRESULT(WINAPI*)(UInt32* numFormats);
using FnGetHandlerProperty2 = HRESULT(WINAPI*)(UInt32 formatIndex, PROPID propID,
                                               PROPVARIANT* value);
using FnSetLargePageMode = HRESULT(WINAPI*)();

// Blueprint ch. 2.2: kpidPath..kpidEncrypted are stable across every 7-Zip
// release we target; everything beyond that is discovered by name at runtime
// through IInArchive::GetPropertyInfo (see ArchiveReader::PropertyIdByName).
enum : PROPID {
  kpidPath = 3,
  kpidIsDir = 6,
  kpidSize = 7,
  kpidPackSize = 8,
  kpidAttrib = 9,
  kpidMTime = 12,
  kpidEncrypted = 15
};

// AskExtractMode
enum : Int32 { kExtract = 0, kTest = 1, kSkip = 2 };

}  // namespace prism::win::abi
