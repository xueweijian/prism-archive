// Vocabulary-only stand-in for the Windows SDK surface PrismWin touches.
//
// PURPOSE (and its limit): let a Linux box TYPECHECK the PrismWin headers under
// -Wall -Wextra -Wpedantic -Werror, so the compile-error classes that used to
// cost a CI round each (wrong override signature, unparenthesized std::min
// under the windows.h macro, transitive-<cstdint> reliance, a std::function
// built from a nullptr literal, a mismatched std::chrono template argument)
// fail here in two seconds instead of twenty minutes.
//
// It declares types, constants and function SIGNATURES. It implements nothing:
// every function returns a zero/失败 value, no stream moves a byte, no archive
// is read. Therefore NO test may draw a behavioural conclusion from compiling
// against it -- the Windows CI lane with the real 7z.dll and the real Shell is
// still the only proof that these TUs build and work.
//
// Guard: every stubbed SDK header maps to this same file, so the content has to
// be one idempotent unit (real <windows.h> is the common root anyway).
#pragma once

#ifndef PRISM_WIN_STUB_INCLUDED
#define PRISM_WIN_STUB_INCLUDED

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

// ---- scalar aliases --------------------------------------------------------
using BOOL = int;
using BYTE = unsigned char;
using WORD = unsigned short;
using DWORD = unsigned long;
using UINT = unsigned int;
using LONG = long;
using SHORT = short;
using USHORT = unsigned short;
using ULONG = unsigned long;
using LONGLONG = long long;
using ULONGLONG = unsigned long long;
using LONG_PTR = long long;
using ULONG_PTR = unsigned long long;
using SHANDLE_PTR = long long;
using HRESULT = long;
using VARTYPE = unsigned short;
using PROPID = unsigned int;
using CLIPFORMAT = unsigned short;
using BSTR = wchar_t*;
using PWSTR = wchar_t*;
using PCWSTR = const wchar_t*;
using HINSTANCE = void*;
using HKEY = void*;
using HDC = void*;
using HBITMAP = void*;
using HGDIOBJ = void*;
using HWND = void*;
using HANDLE = void*;
using HMODULE = void*;
using HGLOBAL = void*;
using PIDLIST_ABSOLUTE = void*;
using WPARAM = unsigned long long;
using LPARAM = long long;
using LRESULT = long long;
using SIZE_T = size_t;
using SSIZE_T = long long;

#define WINAPI
#define STDMETHODCALLTYPE
#define STDMETHODIMP HRESULT STDMETHODCALLTYPE
#define STDMETHODIMP_(type) type STDMETHODCALLTYPE
#define STDMETHOD(name) virtual HRESULT STDMETHODCALLTYPE name
#define PURE = 0
#define IFACEMETHODIMP STDMETHODIMP
#define IFACEMETHOD(name) STDMETHOD(name)
#define MAX_PATH 260
#define ARRAYSIZE(a) (sizeof(a) / sizeof((a)[0]))
#define FAILED(hr) (((HRESULT)(hr)) < 0)
#define SUCCEEDED(hr) (((HRESULT)(hr)) >= 0)
#define HRESULT_FROM_WIN32(x) \
  ((HRESULT)(x) <= 0 ? ((HRESULT)(x)) : ((HRESULT)(((x) & 0x0000FFFF) | (7 << 16) | 0x80000000)))
#define INVALID_HANDLE_VALUE (reinterpret_cast<HANDLE>(static_cast<LONG_PTR>(-1)))
#define IID_PPV_ARGS(ppType) GUID{}, static_cast<void**>(ppType)
#define TEXT(x) L##x
#define _T(x) L##x

// ---- registry + module-path vocabulary (2026-11-01) -------------------------
// Vocabulary only, like everything else here: the gate compiles headers against
// this file with -fsyntax-only and never links, so no behaviour can leak out.
// Needed the moment PrismEngineBridge.hpp started resolving 7z.dll through
// HKLM/HKCU SOFTWARE\7-Zip InstallDir (card C1): the Windows-TU gate caught it
// on main within minutes, which is exactly the job it exists to do.
using LSTATUS = LONG;
using LPBYTE = BYTE*;
#define KEY_QUERY_VALUE 0x0001L
#define ERROR_SUCCESS 0L
#define INVALID_FILE_ATTRIBUTES (static_cast<DWORD>(-1))
#define REG_SZ 1u
#define REG_EXPAND_SZ 2u
#define HKEY_LOCAL_MACHINE (reinterpret_cast<HKEY>(static_cast<LONG_PTR>(0x80000002L)))
#define HKEY_CURRENT_USER (reinterpret_cast<HKEY>(static_cast<LONG_PTR>(0x80000001L)))

DWORD GetFileAttributesW(PCWSTR path);
DWORD GetModuleFileNameW(HMODULE module, PWSTR filename, DWORD size);
DWORD GetEnvironmentVariableW(PCWSTR name, PWSTR buffer, DWORD size);
LSTATUS RegOpenKeyExW(HKEY key, PCWSTR subKey, DWORD options, DWORD samDesired, HKEY* result);
LSTATUS RegQueryValueExW(HKEY key, PCWSTR valueName, DWORD* reserved, DWORD* type,
                         BYTE* data, DWORD* dataSize);
LSTATUS RegCloseKey(HKEY key);

enum : HRESULT {
  S_OK = 0,
  S_FALSE = 1,
  E_NOTIMPL = static_cast<HRESULT>(0x80004001L),
  E_NOINTERFACE = static_cast<HRESULT>(0x80004002L),
  E_POINTER = static_cast<HRESULT>(0x80004003L),
  E_FAIL = static_cast<HRESULT>(0x80004005L),
  E_INVALIDARG = static_cast<HRESULT>(0x80070057L),
  E_OUTOFMEMORY = static_cast<HRESULT>(0x8007000EL),
  E_ABORT = static_cast<HRESULT>(0x80004004L),
  E_ACCESSDENIED = static_cast<HRESULT>(0x80070005L),
  E_UNEXPECTED = static_cast<HRESULT>(0x8000FFFFL),
  CLASS_E_NOAGGREGATION = static_cast<HRESULT>(0x80040110L),
  STG_E_MEDIUMFULL = static_cast<HRESULT>(0x80030070L),
  STG_E_READFAULT = static_cast<HRESULT>(0x8003001EL),
  STG_E_WRITEFAULT = static_cast<HRESULT>(0x8003001DL),
  STG_E_INVALIDFUNCTION = static_cast<HRESULT>(0x80030001L),
  OLE_E_ADVISENOTSUPPORTED = static_cast<HRESULT>(0x80040003L),
  DV_E_FORMATETC = static_cast<HRESULT>(0x80040064L),
  DV_E_DVASPECT = static_cast<HRESULT>(0x8004006BL),
  DV_E_TYMED = static_cast<HRESULT>(0x80040069L),
  DV_E_LINDEX = static_cast<HRESULT>(0x80040068L),
  DATA_S_SAMEFORMATETC = 0x00040130L,
  REGDB_E_CLASSNOTREG = static_cast<HRESULT>(0x80040154L),
};

// ---- forward declarations ---------------------------------------------------
struct IUnknown;
struct IDispatch;
struct ISequentialStream;
struct IStream;

// ---- aggregates ------------------------------------------------------------
struct GUID {
  unsigned int Data1;
  unsigned short Data2;
  unsigned short Data3;
  unsigned char Data4[8];
};
using IID = GUID;
using CLSID = GUID;
using REFIID = const GUID&;
using REFCLSID = const GUID&;

inline bool operator==(const GUID& a, const GUID& b) {
  return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}
inline bool operator!=(const GUID& a, const GUID& b) { return !(a == b); }

struct POINT {
  LONG x;
  LONG y;
};
struct SIZE {
  LONG cx;
  LONG cy;
};
struct RECT {
  LONG left;
  LONG top;
  LONG right;
  LONG bottom;
};
struct FILETIME {
  DWORD dwLowDateTime;
  DWORD dwHighDateTime;
};
struct LARGE_INTEGER {
  union {
    struct {
      DWORD LowPart;
      LONG HighPart;
    };
    LONGLONG QuadPart;
  };
};
struct ULARGE_INTEGER {
  union {
    struct {
      DWORD LowPart;
      DWORD HighPart;
    };
    ULONGLONG QuadPart;
  };
};
struct VARIANT {
  VARTYPE vt;
  WORD wReserved1;
  WORD wReserved2;
  WORD wReserved3;
  union {
    LONG lVal;
    BOOL boolVal;
    IDispatch* pdispVal;
    IUnknown* punkVal;
  };
};
struct PROPVARIANT {
  VARTYPE vt;
  WORD wReserved1;
  WORD wReserved2;
  WORD wReserved3;
  union {
    LONG lVal;
    ULONG ulVal;
    SHORT iVal;
    USHORT uiVal;
    BOOL boolVal;
    LONGLONG llVal;
    ULONGLONG uhValRaw;
    ULARGE_INTEGER uhVal;
    ULARGE_INTEGER hVal;
    FILETIME filetime;
    BSTR bstrVal;
    wchar_t* pwszVal;
    void* everything;
  };
};
struct STATSTG {
  wchar_t* pwcsName;
  DWORD type;
  ULARGE_INTEGER cbSize;
  FILETIME mtime;
  FILETIME ctime;
  FILETIME atime;
  DWORD grfMode;
  DWORD grfLocksSupported;
  CLSID clsid;
  DWORD grfStateBits;
  DWORD reserved;
};

enum {
  VT_EMPTY = 0,
  VT_NULL = 1,
  VT_I2 = 2,
  VT_I4 = 3,
  VT_BSTR = 8,
  VT_BOOL = 11,
  VT_UI2 = 18,
  VT_UI4 = 19,
  VT_I8 = 20,
  VT_UI8 = 21,
  VT_LPWSTR = 31,
  VT_FILETIME = 64
};
enum { VARIANT_FALSE = 0, VARIANT_TRUE = -1 };
enum { FILE_BEGIN = 0, FILE_CURRENT = 1, FILE_END = 2 };
enum { GENERIC_READ = static_cast<int>(0x80000000), GENERIC_WRITE = 0x40000000 };
enum { FILE_SHARE_READ = 0x1, FILE_SHARE_WRITE = 0x2 };
enum { OPEN_EXISTING = 3, CREATE_ALWAYS = 2, CREATE_NEW = 1, OPEN_ALWAYS = 4 };
enum {
  FILE_ATTRIBUTE_NORMAL = 0x80,
  FILE_ATTRIBUTE_DIRECTORY = 0x10,
  FILE_ATTRIBUTE_ARCHIVE = 0x20,
  FILE_ATTRIBUTE_HIDDEN = 0x2,
  FILE_ATTRIBUTE_READONLY = 0x1,
  FILE_FLAG_SEQUENTIAL_SCAN = 0x08000000
};
enum { GMEM_MOVEABLE = 0x0002, GMEM_ZEROINIT = 0x0040 };
enum { DVASPECT_CONTENT = 1, DVASPECT_THUMBNAIL = 2 };
enum { TYMED_HGLOBAL = 1, TYMED_FILE = 2, TYMED_ISTREAM = 4, TYMED_ISTORAGE = 8 };
enum { DATADIR_GET = 1, DATADIR_SET = 2 };
enum { STREAM_SEEK_SET = 0, STREAM_SEEK_CUR = 1, STREAM_SEEK_END = 2 };
enum { STGTY_STREAM = 2, STGTY_STORAGE = 1 };
enum { COINIT_APARTMENTTHREADED = 0x2, COINIT_MULTITHREADED = 0x0 };
enum { CLSCTX_INPROC_SERVER = 0x1, CLSCTX_LOCAL_SERVER = 0x4, CLSCTX_ALL = 0x17 };
enum { GA_ROOT = 2, GA_PARENT = 1 };
enum { CP_UTF8 = 65001, CP_ACP = 0 };
enum { MOVEFILE_REPLACE_EXISTING = 0x1 };
enum { STATFLAG_DEFAULT = 0, STATFLAG_NONAME = 1, STATFLAG_NOOPEN = 2 };
enum { MK_LBUTTON = 0x0001, MK_RBUTTON = 0x0002, MK_SHIFT = 0x0004, MK_CONTROL = 0x0008 };
enum { DRAGDROP_S_DROP = 0x00040100L, DRAGDROP_S_CANCEL = 0x00040101L,
       DRAGDROP_S_USEDEFAULTCURSORS = 0x00040102L };
enum { DROPEFFECT_NONE = 0, DROPEFFECT_COPY = 1, DROPEFFECT_MOVE = 2, DROPEFFECT_LINK = 4 };
enum { OLE_E_ADVISENOTSUPPORTED_2 = 0 };
enum { OLEDC_NODROP = 0x00000001 };
enum { GPFIDL_DEFAULT = 0x0 };
enum { STG_E_ACCESSDENIED = static_cast<HRESULT>(0x80030005L) };
enum { CF_HDROP = 15 };
enum {
  FD_CLSID = 0x1,
  FD_SIZEPOINT = 0x2,
  FD_ATTRIBUTES = 0x4,
  FD_CREATETIME = 0x8,
  FD_ACCESSTIME = 0x10,
  FD_WRITESTIME = 0x20,
  FD_FILESIZE = 0x40,
  FD_PROGRESSUI = 0x4000,
  FD_UNICODE = static_cast<int>(0x80000000)
};

struct FORMATETC {
  CLIPFORMAT cfFormat;
  void* ptd;
  DWORD dwAspect;
  LONG lindex;
  DWORD tymed;
};
struct STGMEDIUM {
  DWORD tymed;
  union {
    HANDLE hGlobal;
    IStream* pstm;
    void* pUnkForRelease;
  };
};
// ---- interfaces ------------------------------------------------------------
struct IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) = 0;
  virtual ULONG STDMETHODCALLTYPE AddRef() = 0;
  virtual ULONG STDMETHODCALLTYPE Release() = 0;
};
struct ISequentialStream : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Read(void*, ULONG, ULONG*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Write(const void*, ULONG, ULONG*) = 0;
};
struct IStream : ISequentialStream {
  virtual HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER, DWORD, ULARGE_INTEGER*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) = 0;
  virtual HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*,
                                           ULARGE_INTEGER*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Commit(DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE Revert() = 0;
  virtual HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE Stat(STATSTG*, DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE Clone(IStream**) = 0;
};
struct IDispatch : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, unsigned long, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, wchar_t**, UINT, unsigned long,
                                                  long*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Invoke(long, REFIID, unsigned long, WORD, void*, void*,
                                           void*, unsigned int*) = 0;
};
struct IEnumFORMATETC : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE Next(ULONG, FORMATETC*, ULONG*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Skip(ULONG) = 0;
  virtual HRESULT STDMETHODCALLTYPE Reset() = 0;
  virtual HRESULT STDMETHODCALLTYPE Clone(IEnumFORMATETC**) = 0;
};
struct IEnumSTATDATA : IUnknown {};
struct IAdviseSink : IUnknown {};
struct IDataObject : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetData(FORMATETC*, STGMEDIUM*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) = 0;
  virtual HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC**) = 0;
  virtual HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) = 0;
  virtual HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) = 0;
};
struct IDropSource : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escapePressed, DWORD keyState) = 0;
  virtual HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD effect) = 0;
};
struct IDropTarget : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE DragEnter(IDataObject*, DWORD, POINT, DWORD*) = 0;
  virtual HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINT, DWORD*) = 0;
  virtual HRESULT STDMETHODCALLTYPE DragLeave() = 0;
  virtual HRESULT STDMETHODCALLTYPE Drop(IDataObject*, DWORD, POINT, DWORD*) = 0;
};
struct IServiceProvider : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryService(REFIID, REFIID, void**) = 0;
};
struct IPersistFolder2 : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetCurFolder(PIDLIST_ABSOLUTE*) = 0;
};
struct IFolderView : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetFolder(REFIID, void**) = 0;
};
struct IShellView : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryStatus() = 0;
};
struct IShellBrowser : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE QueryActiveShellView(IShellView**) = 0;
};
struct IWebBrowserApp : IDispatch {
  virtual HRESULT STDMETHODCALLTYPE get_HWND(SHANDLE_PTR*) = 0;
};
struct IShellWindows : IDispatch {
  virtual HRESULT STDMETHODCALLTYPE get_Count(long*) = 0;
  virtual HRESULT STDMETHODCALLTYPE Item(VARIANT, IDispatch**) = 0;
};

struct FILEDESCRIPTORW {
  DWORD dwFlags;
  CLSID clsid;
  SIZE sizel;
  POINT pointl;
  DWORD dwFileAttributes;
  FILETIME ftCreationTime;
  FILETIME ftLastAccessTime;
  FILETIME ftLastWriteTime;
  DWORD nFileSizeHigh;
  DWORD nFileSizeLow;
  wchar_t cFileName[MAX_PATH];
};
struct FILEGROUPDESCRIPTORW {
  UINT cItems;
  FILEDESCRIPTORW fgd[1];
};

// ---- externs the code references ------------------------------------------
extern const GUID IID_IUnknown;
extern const GUID IID_IStream;
extern const GUID IID_IDataObject;
extern const GUID IID_ISequentialStream;
extern const GUID SID_STopLevelBrowser;
extern const GUID CLSID_ShellWindows;
extern const GUID FOLDERID_Desktop;

// ---- functions (signatures only) ------------------------------------------
inline LONG InterlockedIncrement(LONG* v) { return ++*v; }
inline LONG InterlockedDecrement(LONG* v) { return --*v; }
inline DWORD GetLastError() { return 0; }
inline HANDLE CreateFileW(const wchar_t*, DWORD, DWORD, void*, DWORD, DWORD, HANDLE) {
  return INVALID_HANDLE_VALUE;
}
inline BOOL GetFileSizeEx(HANDLE, LARGE_INTEGER*) { return 0; }
inline BOOL SetFilePointerEx(HANDLE, LARGE_INTEGER, LARGE_INTEGER*, DWORD) { return 0; }
inline BOOL SetEndOfFile(HANDLE) { return 0; }
inline BOOL ReadFile(HANDLE, void*, DWORD, DWORD*, void*) { return 0; }
inline BOOL WriteFile(HANDLE, const void*, DWORD, DWORD*, void*) { return 0; }
inline BOOL CloseHandle(HANDLE) { return 1; }
inline BOOL GetDiskFreeSpaceExW(const wchar_t*, ULARGE_INTEGER*, ULARGE_INTEGER*,
                                ULARGE_INTEGER*) {
  return 0;
}
inline HGLOBAL GlobalAlloc(UINT, size_t) { return nullptr; }
inline void* GlobalLock(HGLOBAL) { return nullptr; }
inline BOOL GlobalUnlock(HGLOBAL) { return 1; }
inline size_t GlobalSize(HGLOBAL) { return 0; }
inline HGLOBAL GlobalFree(HGLOBAL) { return nullptr; }
inline CLIPFORMAT RegisterClipboardFormatW(const wchar_t*) { return 0; }
inline HMODULE LoadLibraryW(const wchar_t*) { return nullptr; }
inline void* GetProcAddress(HMODULE, const char* /*the 7z exports are undecorated*/) {
  return nullptr;
}
inline BOOL FreeLibrary(HMODULE) { return 1; }
inline HRESULT CoInitializeEx(void*, DWORD) { return S_OK; }
inline void CoUninitialize() {}
inline HRESULT CoCreateInstance(REFCLSID, IUnknown*, DWORD, REFIID, void**) { return E_FAIL; }
inline HRESULT CoTaskMemFree(void*) { return S_OK; }
inline HRESULT SHGetKnownFolderPath(REFIID, DWORD, HANDLE, PWSTR*) { return E_FAIL; }
inline HWND WindowFromPoint(POINT) { return nullptr; }
inline HWND GetAncestor(HWND, UINT) { return nullptr; }
inline int GetClassNameW(HWND, wchar_t* out, int n) {
  if (n > 0 && out) out[0] = 0;
  return 0;
}
inline int MultiByteToWideChar(UINT, DWORD, const char*, int, wchar_t*, int) { return 0; }
inline int WideCharToMultiByte(UINT, DWORD, const wchar_t*, int, char*, int, const char*,
                               BOOL*) {
  return 0;
}
inline void PropVariantInit(PROPVARIANT* v) {
  if (v) *v = PROPVARIANT{};
}
inline HRESULT PropVariantClear(PROPVARIANT*) { return S_OK; }
inline BSTR SysAllocString(const wchar_t*) { return nullptr; }
inline void SysFreeString(BSTR) {}
inline BOOL CreateDirectoryW(const wchar_t*, void*) { return 0; }
inline BOOL RemoveDirectoryW(const wchar_t*) { return 0; }
inline BOOL DeleteFileW(const wchar_t*) { return 0; }
inline BOOL MoveFileExW(const wchar_t*, const wchar_t*, DWORD) { return 0; }
inline wchar_t* lstrcpynW(wchar_t* dst, const wchar_t* src, int n) {
  if (!dst || n <= 0) return dst;
  int i = 0;
  for (; i + 1 < n && src && src[i]; ++i) dst[i] = src[i];
  dst[i] = 0;
  return dst;
}
inline DWORD GetFileAttributesW(const wchar_t*) { return 0xFFFFFFFFu; }
inline BOOL SHGetPathFromIDListEx(PIDLIST_ABSOLUTE, PWSTR, DWORD, int) { return 0; }
inline HRESULT SHGetDesktopFolder(void**) { return E_FAIL; }
inline HWND GetForegroundWindow() { return nullptr; }
inline BOOL GetCursorPos(POINT*) { return 0; }
inline BOOL ScreenToClient(HWND, POINT*) { return 0; }
inline DWORD GetCurrentThreadId() { return 0; }
inline DWORD WaitForSingleObject(HANDLE, DWORD) { return 0; }
inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* f) {
  if (f) f->QuadPart = 1000000;
  return 1;
}
inline BOOL QueryPerformanceCounter(LARGE_INTEGER* c) {
  if (c) c->QuadPart = 0;
  return 1;
}

#endif  // PRISM_WIN_STUB_INCLUDED
