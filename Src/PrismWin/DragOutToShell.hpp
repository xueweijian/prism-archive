// PrismArchive -- PrismWin
// Dragging files OUT of the archive and into whatever the shell decides
// (card C67). The data object carries CF_HDROP, i.e. REAL PATHS on disk --
// the shape Explorer, Explorer-adjacent apps and most third-party drop targets
// understand without our code running on the other side.
//
// WHY CF_HDROP AND NOT CFSTR_FILEDESCRIPTORW: the virtual-file channel exists
// (C20/DragOutSession) and is the right tool for an application that streams a
// 4 GB member, but Explorer's own "copy here" only sat down for HDROP in this
// product's testing, and a drop target that does not ask for FILECONTENTS sees
// nothing at all. HDROP means the bytes must exist on disk first; that is why
// the payload is EXTRACTED INTO A PRIVATE TEMP DIRECTORY and removed again when
// the drag ends (drop, cancel, escape, crash of the target -- DoDragDrop
// returning is the one signal that covers all of them).
//
// THE COST RULE (card C67, the reason MaterializeGate exists): dragging a
// selection PAST an Explorer window must not extract anything. DoDragDrop pumps
// messages for as long as the user holds the button, so anything started before
// it is paid for by a drag the user may abandon one pixel later. The payload is
// therefore produced on the FIRST GetData(CF_HDROP) -- the moment a target
// actually offers to copy -- and never on QueryGetData/EnumFormatEtc, which is
// all a hovering target needs.
#pragma once

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../PrismCore/DragOutPlan.hpp"

namespace prism::win::dragoutfiles {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

// What the extractor produced. Paths are the REAL files now in the temp tree;
// an empty list is a failure, never a silent empty drop.
struct MaterializeResult {
  bool ok = false;
  std::vector<std::wstring> paths;
  std::string error;
};

// The FORMATETC for CF_HDROP. One helper so every method agrees on tymed,
// lindex and ptd -- a mismatch between QueryGetData and GetData is the classic
// "advertises but never serves" bug.
inline FORMATETC HdropFormat() {
  FORMATETC format{};
  format.cfFormat = static_cast<CLIPFORMAT>(CF_HDROP);
  format.ptd = nullptr;
  format.dwAspect = DVASPECT_CONTENT;
  format.lindex = -1;
  format.tymed = TYMED_HGLOBAL;
  return format;
}

inline bool IsHdropFormat(const FORMATETC* format) {
  return format != nullptr && format->cfFormat == static_cast<CLIPFORMAT>(CF_HDROP) &&
         format->dwAspect == DVASPECT_CONTENT && format->lindex == -1 &&
         (format->tymed & TYMED_HGLOBAL) != 0;
}

// The HGLOBAL an HDROP points at: DROPFILES header + double-NUL wide path list.
// Returns nullptr when the list is empty (an empty HDROP would make the target
// "succeed" with nothing copied) or when the allocation fails.
inline HGLOBAL BuildHdrop(const std::vector<std::wstring>& paths) {
  if (paths.empty()) return nullptr;
  if (!prism::dragout::CheckPathList(paths).ok()) return nullptr;
  std::size_t bytes = sizeof(DROPFILES) + sizeof(wchar_t);
  for (const std::wstring& path : paths) bytes += (path.size() + 1) * sizeof(wchar_t);
  HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
  if (memory == nullptr) return nullptr;
  auto* header = static_cast<DROPFILES*>(::GlobalLock(memory));
  if (header == nullptr) {
    ::GlobalFree(memory);
    return nullptr;
  }
  header->pFiles = static_cast<DWORD>(sizeof(DROPFILES));
  header->fWide = TRUE;
  auto* cursor = reinterpret_cast<wchar_t*>(reinterpret_cast<BYTE*>(header) + sizeof(DROPFILES));
  for (const std::wstring& path : paths) {
    std::memcpy(cursor, path.c_str(), (path.size() + 1) * sizeof(wchar_t));
    cursor += path.size() + 1;
  }
  *cursor = L'\0';
  ::GlobalUnlock(memory);
  return memory;
}

// A one-entry IEnumFORMATETC. shell32's SHCreateStdEnumFMTETC would do it, but
// one live object with no dependency is easier to reason about (and to test:
// "the object advertises CF_HDROP" is read through THIS enumerator).
class HdropFormatEnumerator final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IEnumFORMATETC> {
 public:
  HdropFormatEnumerator() = default;
  IFACEMETHODIMP Next(ULONG count, FORMATETC* out, ULONG* fetched) override {
    if (out == nullptr) return E_POINTER;
    if (fetched != nullptr) *fetched = 0;
    if (index_ != 0 || count == 0) return S_FALSE;
    out[0] = HdropFormat();
    index_ = 1;
    if (fetched != nullptr) *fetched = 1;
    return count == 1 ? S_OK : S_FALSE;
  }
  IFACEMETHODIMP Skip(ULONG count) override {
    index_ += count;
    return index_ <= 1 ? S_OK : S_FALSE;
  }
  IFACEMETHODIMP Reset() override {
    index_ = 0;
    return S_OK;
  }
  IFACEMETHODIMP Clone(IEnumFORMATETC** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    ComPtr<HdropFormatEnumerator> copy;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<HdropFormatEnumerator>(&copy))) return E_FAIL;
    copy->index_ = index_;
    return copy->QueryInterface(IID_PPV_ARGS(out));
  }

 private:
  ULONG index_ = 0;
};

// The IDataObject Explorer sees while the drag is in flight.
class HdropDataObject final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDataObject> {
 public:
  using Extractor = std::function<bool(MaterializeResult*)>;

  HdropDataObject() = default;

  void SetExtractor(Extractor extractor) {
    extractor_ = std::move(extractor);
    gate_.SetExtractor([this]() { return Materialize(); });
  }

  // Evidence for the suite/app: how often a target probed, how often it asked
  // for bytes, and whether the payload exists. Reported, not assumed.
  const prism::dragout::MaterializeGate& gate() const { return gate_; }
  const MaterializeResult& result() const { return result_; }

  IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (format == nullptr || medium == nullptr) return E_POINTER;
    std::memset(medium, 0, sizeof(*medium));
    if (!IsHdropFormat(format)) return DV_E_FORMATETC;
    // The ONLY door to extraction (MaterializeGate::Demand).
    if (!gate_.Demand()) return E_FAIL;
    HGLOBAL memory = BuildHdrop(result_.paths);
    if (memory == nullptr) return STG_E_MEDIUMFULL;
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = memory;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
  IFACEMETHODIMP QueryGetData(FORMATETC* format) override {
    if (format == nullptr) return E_POINTER;
    if (!IsHdropFormat(format)) return DV_E_FORMATETC;
    // Offer: answers "yes, I have CF_HDROP" WITHOUT touching the disk.
    return gate_.Offer() ? S_OK : E_FAIL;
  }
  IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC* in, FORMATETC* out) override {
    if (out == nullptr) return E_POINTER;
    out->ptd = nullptr;
    if (in != nullptr) {
      *out = *in;
      out->ptd = nullptr;
    }
    return DATA_S_SAMEFORMATETC;
  }
  IFACEMETHODIMP SetData(FORMATETC*, STGMEDIUM*, BOOL) override { return E_NOTIMPL; }
  IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
    if (out == nullptr) return E_POINTER;
    *out = nullptr;
    if (direction != DATADIR_GET) return E_NOTIMPL;
    ComPtr<HdropFormatEnumerator> enumerator;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<HdropFormatEnumerator>(&enumerator))) {
      return E_FAIL;
    }
    return enumerator->QueryInterface(IID_PPV_ARGS(out));
  }
  IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

 private:
  bool Materialize() {
    if (!extractor_) {
      result_.ok = false;
      result_.error = "no extractor";
      return false;
    }
    MaterializeResult produced;
    const bool ok = extractor_(&produced);
    result_ = ok ? produced : MaterializeResult{};
    if (!ok && result_.error.empty()) result_.error = "extraction failed";
    return ok && !result_.paths.empty();
  }

  Extractor extractor_;
  prism::dragout::MaterializeGate gate_;
  MaterializeResult result_;
};

// The drag source: left button released over a target == a DROP, escape ==
// CANCEL, anything else keeps the drag alive. This is what makes "let go over
// the desktop" work and "press Esc mid-drag" leave nothing behind.
class HdropDropSource final
    : public RuntimeClass<RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDropSource> {
 public:
  IFACEMETHODIMP QueryContinueDrag(BOOL escapePressed, DWORD keyState) override {
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & MK_LBUTTON) == 0 && (keyState & MK_RBUTTON) == 0) return DRAGDROP_S_DROP;
    return S_OK;
  }
  IFACEMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

// One drag: the data object, the source, the private temp directory and the
// cleanup. The temp directory is created by the EXTRACTOR (lazily) and removed
// by Finish(), which Run() calls on every path -- including a failed DoDragDrop.
class FileDragOut {
 public:
  using Extractor = HdropDataObject::Extractor;

  // tempRoot is %TEMP% in production and a scratch directory in tests. The
  // unique leaf comes from PrismCore so the naming rule is testable offline.
  static std::unique_ptr<FileDragOut> Create(const std::wstring& tempRoot, Extractor extractor,
                                             std::string* error = nullptr) {
    auto fail = [error](const char* why) -> std::unique_ptr<FileDragOut> {
      if (error != nullptr) *error = why;
      return nullptr;
    };
    if (tempRoot.empty()) return fail("dragout: temp root is empty");
    auto drag = std::unique_ptr<FileDragOut>(new FileDragOut());
    drag->tempDir_ = tempRoot + L"\\" + prism::dragout::TempDirLeaf(
                                      static_cast<std::uint32_t>(::GetCurrentProcessId()),
                                      ++counter());
    ComPtr<HdropDataObject> object;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<HdropDataObject>(&object))) {
      return fail("dragout: data object could not be created");
    }
    object->SetExtractor(std::move(extractor));
    drag->object_ = object;
    return drag;
  }

  ~FileDragOut() { Finish(); }

  FileDragOut(const FileDragOut&) = delete;
  FileDragOut& operator=(const FileDragOut&) = delete;

  // The real drag, bounded by the user's own input (DoDragDrop returns when the
  // button is released or Esc is pressed). ALWAYS cleans up afterwards.
  HRESULT Run(HWND owner, DWORD allowedEffects = DROPEFFECT_COPY, DWORD* effectOut = nullptr) {
    ComPtr<HdropDropSource> source;
    if (FAILED(Microsoft::WRL::MakeAndInitialize<HdropDropSource>(&source))) {
      Finish();
      return E_FAIL;
    }
    DWORD effect = DROPEFFECT_NONE;
    const HRESULT hr = ::DoDragDrop(object_.Get(), source.Get(), allowedEffects, &effect);
    Finish();
    if (effectOut != nullptr) *effectOut = effect;
    return hr;
  }

  // Removes the temp tree. Idempotent, best effort, and safe on the cancel path.
  void Finish() {
    if (finished_ || tempDir_.empty()) return;
    finished_ = true;
    std::error_code ec;
    for (int attempt = 0; attempt < 4; ++attempt) {
      std::filesystem::remove_all(std::filesystem::path(tempDir_), ec);
      ec.clear();
      if (!std::filesystem::exists(std::filesystem::path(tempDir_), ec)) break;
      ::Sleep(static_cast<DWORD>(10 * (attempt + 1)));
    }
  }

  // The extractor is installed AFTER Create only because it needs the temp
  // directory this object owns; Create's argument is the same seam.
  void SetExtractor(Extractor extractor) { object_->SetExtractor(std::move(extractor)); }

  IDataObject* object() const { return object_.Get(); }
  const std::wstring& temp_dir() const { return tempDir_; }
  bool temp_dir_exists() const {
    std::error_code ec;
    return std::filesystem::exists(std::filesystem::path(tempDir_), ec);
  }
  std::size_t extractions() const { return object_->gate().extractions(); }
  std::size_t offers() const { return object_->gate().offers(); }
  std::size_t files() const { return object_->result().paths.size(); }
  prism::dragout::Stage stage() const { return object_->gate().stage(); }

 private:
  FileDragOut() = default;

  static std::uint64_t& counter() {
    static std::uint64_t value = 0;
    return value;
  }

  std::wstring tempDir_;
  ComPtr<HdropDataObject> object_;
  bool finished_ = false;
};

}  // namespace prism::win::dragoutfiles
