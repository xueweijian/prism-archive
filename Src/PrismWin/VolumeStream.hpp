// PrismArchive -- PrismWin
// VolumeStream: multi-volume archive I/O against the real 7z.dll (blueprint 2.1
// "volume merge"). Three pieces, each with a contract that comes from 7-Zip
// upstream source rather than guesswork:
//
//   1. VolumeStream      - presents an ordered volume set as ONE seekable
//                          IInStream, so a handler that only understands a
//                          single stream (7z, zip, ...) reads the concatenation.
//                          Missing middle volumes are refused before the engine
//                          is touched (VolumePlan proves them by name).
//   2. VolumeOpenCallback - the real IArchiveOpenVolumeCallback (family 06,
//                          member 0x30; CPP/7zip/Archive/IArchive.h):
//                            GetProperty(kpidName) -> BSTR volume name
//                            GetStream(name)       -> IInStream for that volume
//                          It answers that IID through QueryInterface (D10/B8:
//                          7-Zip QIs, it never static_casts).
//   3. VolumeSplitWriter - splits an IOutStream into "<prefix>.001", ".002"...
//                          This is where 7-Zip implements -v: Update.cpp wraps
//                          the output in CMultiOutStream; the handler NEVER sees
//                          a volume-size property (there is no such ISetProperties
//                          key in 7zHandlerOut.cpp -- checked upstream, not assumed).
#pragma once

#include <windows.h>
#include <propidl.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "SevenZipAbi.hpp"
#include "VolumePlan.hpp"

namespace prism::win {

// {23170F69-40C1-278A-0000-000600300000} (family 06, member 0x30)
constexpr GUID IID_IArchiveOpenVolumeCallback = {0x23170F69, 0x40C1, 0x278A,
                                                 {0x00, 0x00, 0x00, 0x06, 0x00, 0x30, 0x00, 0x00}};

// The handler-facing half of the volume contract. Kept here (not in
// SevenZipAbi.hpp) so this card touches exactly one shared header: it is only
// needed by volume-aware readers.
struct IArchiveOpenVolumeCallback : public IUnknown {
  STDMETHOD(GetProperty)(PROPID propID, PROPVARIANT* value) PURE;
  STDMETHOD(GetStream)(const wchar_t* name, abi::IInStream** inStream) PURE;
};

inline std::wstring Utf8ToWideVolume(std::string_view utf8) {
  if (utf8.empty()) return {};
  const int need = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                       nullptr, 0);
  if (need <= 0) return {};
  std::wstring out(static_cast<size_t>(need), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), need);
  return out;
}

inline std::string WideToUtf8Volume(std::wstring_view wide) {
  if (wide.empty()) return {};
  const int need = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                       nullptr, 0, nullptr, nullptr);
  if (need <= 0) return {};
  std::string out(static_cast<size_t>(need), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), need,
                      nullptr, nullptr);
  return out;
}

inline std::wstring JoinVolumePath(const std::wstring& directory, const std::string& name) {
  std::wstring out = directory;
  if (!out.empty() && out.back() != L'\\' && out.back() != L'/') out += L'\\';
  out += Utf8ToWideVolume(name);
  return out;
}

// PrismCore speaks '/' (CONTRACTS B4); Win32 wants '\'. Absolute paths get the
// \\?\ prefix so long temp paths and >MAX_PATH names still open -- the prefix
// disables Win32 normalization, which is exactly why the conversion comes first.
inline std::wstring ToWin32VolumePath(const std::wstring& raw) {
  std::wstring path = raw;
  for (wchar_t& c : path) {
    if (c == L'/') c = L'\\';
  }
  if (path.size() >= 4 && path.compare(0, 4, L"\\\\?\\") == 0) return path;
  if (path.size() >= 2 && path[1] == L':') return L"\\\\?\\" + path;
  if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
    return L"\\\\?\\UNC\\" + path.substr(2);
  }
  return path;
}

// ---------------------------------------------------------------------------
// One physical volume file, opened for reading (IInStream + IStreamGetSize)
// ---------------------------------------------------------------------------
class VolumeFileStream final : public abi::IInStream, public abi::IStreamGetSize {
 public:
  explicit VolumeFileStream(std::wstring path) : path_(std::move(path)) {
    handle_ = CreateFileW(ToWin32VolumePath(path_).c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                          nullptr);
    if (handle_ != INVALID_HANDLE_VALUE) {
      LARGE_INTEGER size{};
      if (GetFileSizeEx(handle_, &size)) size_ = static_cast<std::uint64_t>(size.QuadPart);
    } else {
      // Capture immediately: the next allocation may clobber the thread's last
      // error, and a stale 0 would turn "cannot open" into HRESULT_FROM_WIN32(0)
      // == S_OK with a null out-parameter (the engine would dereference null).
      error_ = GetLastError();
    }
  }
  ~VolumeFileStream() {
    if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
  }
  VolumeFileStream(const VolumeFileStream&) = delete;
  VolumeFileStream& operator=(const VolumeFileStream&) = delete;

  bool valid() const { return handle_ != INVALID_HANDLE_VALUE; }
  DWORD lastError() const { return error_; }
  const std::wstring& path() const { return path_; }
  std::uint64_t size() const { return size_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream || iid == abi::IID_IInStream) {
      *out = static_cast<abi::IInStream*>(this);
    } else if (iid == abi::IID_IStreamGetSize) {
      *out = static_cast<abi::IStreamGetSize*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    DWORD read = 0;
    if (!ReadFile(handle_, data, size, &read, nullptr)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (processed) *processed = read;
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 origin, abi::UInt64* newPos) override {
    LARGE_INTEGER move{};
    move.QuadPart = offset;
    DWORD method = FILE_BEGIN;
    if (origin == 1) method = FILE_CURRENT;
    if (origin == 2) method = FILE_END;
    if (!SetFilePointerEx(handle_, move, &move, method)) {
      return HRESULT_FROM_WIN32(GetLastError());
    }
    if (newPos) *newPos = static_cast<abi::UInt64>(move.QuadPart);
    return S_OK;
  }

  STDMETHODIMP GetSize(abi::UInt64* size) override {
    if (!size) return E_POINTER;
    *size = size_;
    return S_OK;
  }

 private:
  std::wstring path_;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  std::uint64_t size_ = 0;
  DWORD error_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// IArchiveOpenVolumeCallback: answers "give me volume <name>" for handlers that
// ask for one named volume at a time (CONTRACTS B8 -- the IID must be answered).
// ---------------------------------------------------------------------------
class VolumeOpenCallback final : public abi::IArchiveOpenCallback,
                                 public IArchiveOpenVolumeCallback {
 public:
  VolumeOpenCallback(std::wstring directory, prism::VolumePlan plan, std::string requestedName)
      : directory_(std::move(directory)),
        plan_(std::move(plan)),
        requestedName_(std::move(requestedName)) {}

  std::uint32_t streamRequests() const { return streamRequests_; }
  std::uint32_t streamsServed() const { return streamsServed_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_IArchiveOpenCallback) {
      *out = static_cast<abi::IArchiveOpenCallback*>(this);
    } else if (iid == IID_IArchiveOpenVolumeCallback) {
      *out = static_cast<IArchiveOpenVolumeCallback*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*, const abi::UInt64*) override { return S_OK; }

  STDMETHODIMP GetProperty(PROPID propID, PROPVARIANT* value) override {
    if (!value) return E_POINTER;
    PropVariantInit(value);
    if (propID == 4 /* kpidName */) {
      value->vt = VT_BSTR;
      const std::wstring name = Utf8ToWideVolume(requestedName_);
      value->bstrVal = SysAllocString(name.c_str());
      if (!value->bstrVal) return E_OUTOFMEMORY;
    }
    return S_OK;
  }

  STDMETHODIMP GetStream(const wchar_t* name, abi::IInStream** inStream) override {
    if (!inStream) return E_POINTER;
    *inStream = nullptr;
    if (!name) return E_INVALIDARG;
    ++streamRequests_;
    const std::string requested = WideToUtf8Volume(name);
    const std::string requestedLower = volume_detail::Lower(requested);
    for (const prism::VolumeEntry& entry : plan_.volumes) {
      if (volume_detail::Lower(entry.name) != requestedLower) continue;
      auto* stream = new VolumeFileStream(JoinVolumePath(directory_, entry.name));
      if (!stream->valid()) {
        const DWORD cause = stream->lastError();
        delete stream;
        const HRESULT hr = HRESULT_FROM_WIN32(cause);
        // HRESULT_FROM_WIN32(ERROR_SUCCESS) is S_OK: never let an unopenable
        // volume come back as "here is your stream" with a null pointer.
        return FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_OPEN_FAILED);
      }
      ++streamsServed_;
      *inStream = stream;  // one reference, transferred to the caller
      return S_OK;
    }
    // S_FALSE is 7-Zip's "that volume does not exist"; the handler then reports
    // a missing-volume error instead of silently reading a shifted stream.
    return S_FALSE;
  }

 private:
  std::wstring directory_;
  prism::VolumePlan plan_;
  std::string requestedName_;
  std::uint32_t streamRequests_ = 0;
  std::uint32_t streamsServed_ = 0;
  LONG refs_ = 1;
};


// ---------------------------------------------------------------------------
// The ordered volume set as ONE seekable IInStream
// ---------------------------------------------------------------------------
class VolumeStream final : public abi::IInStream, public abi::IStreamGetSize {
 public:
  VolumeStream() = default;
  ~VolumeStream() { Close(); }
  VolumeStream(const VolumeStream&) = delete;
  VolumeStream& operator=(const VolumeStream&) = delete;

  // Refuses a plan that does not prove its own completeness (missing middle
  // volume, missing final .zip/.rar, non-ascending order). The engine is never
  // handed a shifted stream: that is how a "half-read" archive silently turns
  // into a corrupt extraction.
  bool Open(const std::wstring& directory, const prism::VolumePlan& plan, std::string* error) {
    Close();
    if (!plan.ok()) {
      if (error) *error = plan.error.empty() ? "invalid volume plan" : plan.error;
      return false;
    }
    for (const prism::VolumeEntry& entry : plan.volumes) {
      auto* file = new VolumeFileStream(JoinVolumePath(directory, entry.name));
      if (!file->valid()) {
        if (error) *error = "cannot open volume: " + entry.name;
        delete file;
        Close();
        return false;
      }
      starts_.push_back(total_);
      total_ += file->size();
      files_.push_back(file);
    }
    if (files_.empty()) {
      if (error) *error = "volume plan has no volumes";
      return false;
    }
    pos_ = 0;
    return true;
  }

  void Close() {
    for (VolumeFileStream* file : files_) file->Release();
    files_.clear();
    starts_.clear();
    total_ = 0;
    pos_ = 0;
  }

  bool opened() const { return !files_.empty(); }
  std::uint64_t size() const { return total_; }
  std::uint64_t position() const { return pos_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialInStream || iid == abi::IID_IInStream) {
      *out = static_cast<abi::IInStream*>(this);
    } else if (iid == abi::IID_IStreamGetSize) {
      *out = static_cast<abi::IStreamGetSize*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP Read(void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    if (!opened()) return E_UNEXPECTED;
    auto* out = static_cast<std::uint8_t*>(data);
    std::uint64_t pos = pos_;
    abi::UInt32 done = 0;
    while (done < size && pos < total_) {
      const size_t index = IndexFor(pos);
      const std::uint64_t local = pos - starts_[index];
      const std::uint64_t available = files_[index]->size() - local;
      if (available == 0) break;
      const abi::UInt32 want = static_cast<abi::UInt32>(
          (std::min)(static_cast<std::uint64_t>(size - done), available));
      if (files_[index]->Seek(static_cast<abi::Int64>(local), 0, nullptr) != S_OK) {
        return E_FAIL;
      }
      abi::UInt32 got = 0;
      const HRESULT hr = files_[index]->Read(out + done, want, &got);
      if (FAILED(hr)) return hr;
      if (got == 0) break;
      done += got;
      pos += got;
    }
    pos_ = pos;
    if (processed) *processed = done;
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 origin, abi::UInt64* newPos) override {
    if (!opened()) return E_UNEXPECTED;
    if (origin > 2) return E_INVALIDARG;
    if (origin == 0) {
      if (offset < 0) return E_INVALIDARG;
      pos_ = static_cast<std::uint64_t>(offset);
    } else if (origin == 1) {
      if (offset < 0) {
        // Negate without overflowing on INT64_MIN.
        const std::uint64_t back = static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (back > pos_) return E_INVALIDARG;
        pos_ -= back;
      } else {
        pos_ += static_cast<std::uint64_t>(offset);
      }
    } else {
      if (offset > 0) return E_INVALIDARG;  // no reading past EOF
      const std::uint64_t back = static_cast<std::uint64_t>(-offset);
      if (back > total_) return E_INVALIDARG;
      pos_ = total_ - back;
    }
    if (newPos) *newPos = pos_;
    return S_OK;
  }

  STDMETHODIMP GetSize(abi::UInt64* size) override {
    if (!size) return E_POINTER;
    *size = total_;
    return S_OK;
  }

 private:
  size_t IndexFor(std::uint64_t pos) const {
    size_t index = 0;
    while (index + 1 < starts_.size() && starts_[index + 1] <= pos) ++index;
    return index;
  }

  std::vector<VolumeFileStream*> files_;
  std::vector<std::uint64_t> starts_;
  std::uint64_t total_ = 0;
  std::uint64_t pos_ = 0;
  LONG refs_ = 1;
};

// ---------------------------------------------------------------------------
// IOutStream that splits its output into "<prefix>.001", ".002", ...
// Mirrors 7-Zip's CMultiOutStream (Update.cpp): -v is a UI-layer stream wrapper,
// NOT an IOutArchive property. The handler writes one continuous stream.
// ---------------------------------------------------------------------------
class VolumeSplitWriter final : public abi::IOutStream {
 public:
  VolumeSplitWriter() = default;
  ~VolumeSplitWriter() { Finish(); }
  VolumeSplitWriter(const VolumeSplitWriter&) = delete;
  VolumeSplitWriter& operator=(const VolumeSplitWriter&) = delete;

  bool Open(std::wstring prefix, std::uint64_t volumeSize, std::string* error) {
    Finish();
    if (prefix.empty() || volumeSize == 0) {
      if (error) *error = "invalid split-writer arguments";
      return false;
    }
    prefix_ = std::move(prefix);
    volumeSize_ = volumeSize;
    open_ = true;
    return true;
  }

  HRESULT Finish() {
    for (HANDLE handle : handles_) {
      if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
    handles_.clear();
    open_ = false;
    return S_OK;
  }

  std::uint32_t volumeCount() const { return static_cast<std::uint32_t>(sizes_.size()); }
  std::uint64_t totalBytes() const { return total_; }
  const std::vector<std::wstring>& paths() const { return paths_; }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream ||
        iid == abi::IID_IOutStream) {
      *out = static_cast<abi::IOutStream*>(this);
    } else {
      *out = nullptr;
      return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = 0;
    if (!data || size == 0) return S_OK;
    if (!open_) return E_UNEXPECTED;
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    abi::UInt32 done = 0;
    while (done < size) {
      if (pos_ >= total_) {
        if (!AppendVolumeIfNeeded()) return E_FAIL;
      }
      const size_t index = IndexFor(pos_);
      const std::uint64_t local = pos_ - VolumeStart(index);
      const std::uint64_t limit = VolumeCapacity(index);
      if (local >= limit) return E_NOTIMPL;  // a random write crossing a volume
      const abi::UInt32 chunk = static_cast<abi::UInt32>(
          (std::min)(static_cast<std::uint64_t>(size - done), limit - local));
      if (chunk == 0) return E_NOTIMPL;
      DWORD written = 0;
      LARGE_INTEGER move{};
      move.QuadPart = static_cast<LONGLONG>(local);
      if (!SetFilePointerEx(handles_[index], move, nullptr, FILE_BEGIN)) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      if (!WriteFile(handles_[index], bytes + done, chunk, &written, nullptr)) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      if (written == 0) return E_FAIL;
      done += written;
      pos_ += written;
      if (pos_ > total_) total_ = pos_;
      if (local + written > sizes_[index]) sizes_[index] = local + written;
    }
    if (processed) *processed = done;
    return S_OK;
  }

  STDMETHODIMP Seek(abi::Int64 offset, abi::UInt32 origin, abi::UInt64* newPos) override {
    if (!open_) return E_UNEXPECTED;
    if (origin > 2) return E_INVALIDARG;
    std::uint64_t target = 0;
    if (origin == 0) {
      if (offset < 0) return E_INVALIDARG;
      target = static_cast<std::uint64_t>(offset);
    } else if (origin == 1) {
      if (offset < 0) {
        const std::uint64_t back = static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (back > pos_) return E_INVALIDARG;
        target = pos_ - back;
      } else {
        target = pos_ + static_cast<std::uint64_t>(offset);
      }
    } else {
      if (offset > 0) return E_INVALIDARG;
      const std::uint64_t back = static_cast<std::uint64_t>(-offset);
      if (back > total_) return E_INVALIDARG;
      target = total_ - back;
    }
    if (target > total_) return E_NOTIMPL;  // no sparse gaps in a volume chain
    pos_ = target;
    if (newPos) *newPos = pos_;
    return S_OK;
  }

  // Only truncation is supported: a growing SetSize would have to materialise
  // zero-filled volumes, and nothing in the 7z writer asks for that.
  STDMETHODIMP SetSize(abi::UInt64 newSize) override {
    if (!open_) return E_UNEXPECTED;
    if (newSize == total_) return S_OK;
    if (newSize > total_) return E_NOTIMPL;
    while (!sizes_.empty() && Starts().back() >= newSize && sizes_.size() > 1) {
      CloseVolume(sizes_.size() - 1);
      sizes_.pop_back();
      paths_.pop_back();
      handles_.pop_back();
    }
    if (!sizes_.empty()) {
      const size_t index = sizes_.size() - 1;
      const std::uint64_t local = newSize - Starts()[index];
      LARGE_INTEGER move{};
      move.QuadPart = static_cast<LONGLONG>(local);
      if (!SetFilePointerEx(handles_[index], move, nullptr, FILE_BEGIN)) {
        return HRESULT_FROM_WIN32(GetLastError());
      }
      if (!SetEndOfFile(handles_[index])) return HRESULT_FROM_WIN32(GetLastError());
      sizes_[index] = local;
    }
    total_ = newSize;
    if (pos_ > total_) pos_ = total_;
    return S_OK;
  }

 private:
  std::vector<std::uint64_t> Starts() const {
    std::vector<std::uint64_t> starts;
    std::uint64_t offset = 0;
    for (const std::uint64_t size : sizes_) {
      starts.push_back(offset);
      offset += size;
    }
    return starts;
  }

  std::uint64_t VolumeCapacity(size_t index) const {
#ifdef PRISM_VOLUME_WRITER_MUTANT_NO_SPLIT
    (void)index;
    return static_cast<std::uint64_t>(-1);  // mutant: never fill a volume
#else
    return volumeSize_;  // every volume is capped; the last one grows to the cap
#endif
  }

  std::uint64_t VolumeStart(size_t index) const {
    std::uint64_t offset = 0;
    for (size_t i = 0; i < index; ++i) offset += sizes_[i];
    return offset;
  }

  size_t IndexFor(std::uint64_t pos) const {
    const std::vector<std::uint64_t> starts = Starts();
    size_t index = 0;
    while (index + 1 < starts.size() && starts[index + 1] <= pos) ++index;
    return index;
  }

  bool AppendVolumeIfNeeded() {
    if (!sizes_.empty() && sizes_.back() < volumeSize_) return true;
    return OpenNextVolume();
  }

  bool OpenNextVolume() {
    const std::uint64_t ordinal = static_cast<std::uint64_t>(sizes_.size()) + 1;
    std::string digits = std::to_string(ordinal);
    while (digits.size() < 3) digits.insert(digits.begin(), '0');
    const std::wstring path = prefix_ + L"." + Utf8ToWideVolume(digits);
    HANDLE handle = CreateFileW(ToWin32VolumePath(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    handles_.push_back(handle);
    sizes_.push_back(0);
    paths_.push_back(path);
    open_ = true;
    return true;
  }

  void CloseVolume(size_t index) {
    if (index < handles_.size() && handles_[index] != INVALID_HANDLE_VALUE) {
      CloseHandle(handles_[index]);
      handles_[index] = INVALID_HANDLE_VALUE;
    }
  }

  std::wstring prefix_;
  std::uint64_t volumeSize_ = 0;
  std::vector<HANDLE> handles_;
  std::vector<std::uint64_t> sizes_;
  std::vector<std::wstring> paths_;
  std::uint64_t total_ = 0;
  std::uint64_t pos_ = 0;
  bool open_ = false;
  LONG refs_ = 1;
};

}  // namespace prism::win
