// PrismArchive -- PrismWin
// The solid-block sweep executor (blueprint ch. 4.3): ONE 7z Extract call per
// solid block harvests every thumbnail that streams by.
//
// Why this beats per-item extraction (and why Bandizip declined to do it):
//   decompressing item N of a solid block pays for items 0..N-1 again, so
//   k per-item extractions cost k*(k+1)/2 item-decompressions, while ONE pass
//   costs k. The catch -- you must consume the whole block even for items you
//   no longer want -- is handled by the session's abandon-and-drain contract.
//
// Memory first principles: bytes stream into ONE fixed scratch buffer (never
// the whole block); each completed item is decoded immediately, the thumbnail
// enters the LRU, the scratch is reused. Peak memory = scratch + thumbs.
#pragma once

#include <windows.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <vector>

#include <wincodec.h>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/SolidSweep.hpp"
#include "PrismEngineBridge.hpp"

namespace prism::win::sweep {

using Microsoft::WRL::ComPtr;

// Fixed scratch: ALWAYS consumes (the extractor requires every byte to be
// accepted -- failing the write would fail the whole block), but discards
// beyond the cap so one huge file inside the block cannot balloon memory.
// The session's size table is what actually guards the decode.
class ScratchStream final : public abi::ISequentialOutStream {
 public:
  explicit ScratchStream(size_t capacity) : capacity_(capacity) { buffer_.reserve(1u << 20); }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_ISequentialOutStream) {
      *out = static_cast<abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = size;  // consumed, always
    if (!data || size == 0) return S_OK;
    if (buffer_.size() < capacity_) {
      const size_t room = capacity_ - buffer_.size();
      const size_t take = std::min<size_t>(size, room);
      const auto* p = static_cast<const uint8_t*>(data);
      buffer_.insert(buffer_.end(), p, p + take);
    }
    return S_OK;
  }

  void Reset() { buffer_.clear(); }
  const uint8_t* data() const { return buffer_.data(); }
  size_t size() const { return buffer_.size(); }

 private:
  size_t capacity_;
  std::vector<uint8_t> buffer_;
  LONG refs_ = 1;
};

// WIC thumbnail decoder: any container WIC knows -> BGRA, fitted inside the
// 256 px box, aspect preserved, NEVER upscaled (a 64 px icon stays 64 px).
class WicThumbDecoder final : public prism::gallery::SolidSweepSession::IDecoder {
 public:
  static constexpr uint32_t kThumbBox = 256;

  bool Initialize() {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                      IID_PPV_ARGS(factory_.ReleaseAndGetAddressOf())));
  }

  std::optional<prism::gallery::DecodedThumb> Decode(const uint8_t* data,
                                                     size_t bytes) override {
    if (!factory_ || !data || bytes == 0) return std::nullopt;
    ComPtr<IWICStream> stream;
    if (FAILED(factory_->CreateStream(stream.ReleaseAndGetAddressOf()))) return std::nullopt;
    if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(data),
                                           static_cast<DWORD>(bytes)))) {
      return std::nullopt;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory_->CreateDecoderFromStream(stream.Get(), nullptr,
                                                 WICDecodeMetadataCacheOnDemand,
                                                 decoder.ReleaseAndGetAddressOf()))) {
      return std::nullopt;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()))) return std::nullopt;

    UINT width = 0, height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0) {
      return std::nullopt;
    }
    UINT targetW = width, targetH = height;
    if (width > kThumbBox || height > kThumbBox) {
      const double scale = std::min<double>(kThumbBox / static_cast<double>(width),
                                            kThumbBox / static_cast<double>(height));
      targetW = std::max<UINT>(1u, static_cast<UINT>(width * scale));
      targetH = std::max<UINT>(1u, static_cast<UINT>(height * scale));
    }

    ComPtr<IWICBitmapSource> source = frame;
    if (targetW != width || targetH != height) {
      ComPtr<IWICBitmapScaler> scaler;
      if (FAILED(factory_->CreateBitmapScaler(scaler.ReleaseAndGetAddressOf()))) {
        return std::nullopt;
      }
      if (FAILED(scaler->Initialize(frame.Get(), targetW, targetH,
                                    WICBitmapInterpolationModeFant))) {
        return std::nullopt;
      }
      source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory_->CreateFormatConverter(converter.ReleaseAndGetAddressOf()))) {
      return std::nullopt;
    }
    if (FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppBGRA,
                                     WICBitmapDitherTypeNone, nullptr, 0.0,
                                     WICBitmapPaletteTypeCustom))) {
      return std::nullopt;
    }

    prism::gallery::DecodedThumb thumb;
    thumb.width = targetW;
    thumb.height = targetH;
    const size_t stride = static_cast<size_t>(targetW) * 4;
    thumb.pixels.resize(stride * targetH);
    if (FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(stride),
                                     static_cast<UINT>(thumb.pixels.size()),
                                     thumb.pixels.data()))) {
      return std::nullopt;
    }
    return thumb;
  }

 private:
  ComPtr<IWICImagingFactory> factory_;
};

// The one-pass extract callback: routes per-item completion into the session.
class SweepExtractCallback final : public abi::IArchiveExtractCallback {
 public:
  SweepExtractCallback(prism::gallery::SolidSweepSession* session, ScratchStream* scratch)
      : session_(session), scratch_(scratch) {
    scratch_->AddRef();
  }
  ~SweepExtractCallback() { scratch_->Release(); }

  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IUnknown || iid == abi::IID_IArchiveExtractCallback) {
      *out = static_cast<abi::IArchiveExtractCallback*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refs_); }
  STDMETHODIMP_(ULONG) Release() override {
    const ULONG r = InterlockedDecrement(&refs_);
    if (r == 0) delete this;
    return r;
  }

  STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialOutStream** out,
                         abi::Int32 mode) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (mode != abi::kExtract) return S_OK;  // skip: *out stays null
    if (itemsEntered_ > 0) {
      // Defensive: an item ended without SetOperationResult -- flush it.
      FlushCurrent();
    }
    session_->OnItemStart(index);
    currentItem_ = index;
    scratch_->Reset();
    scratch_->AddRef();
    *out = scratch_;
    ++itemsEntered_;
    return S_OK;
  }

  STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }

  STDMETHODIMP SetOperationResult(abi::Int32 result) override {
    if (result == 0) FlushCurrent();  // 0 == kOK (7-Zip OperationResult enum)
    return S_OK;
  }

  int itemsEntered() const { return itemsEntered_; }

 private:
  void FlushCurrent() {
    if (currentItem_ == kNoItem) return;
    session_->OnItemBytes(scratch_->size());
    session_->OnItemEnd(scratch_->data(), scratch_->size());
    currentItem_ = kNoItem;
  }

  static constexpr abi::UInt32 kNoItem = 0xFFFFFFFFu;
  prism::gallery::SolidSweepSession* session_;
  ScratchStream* scratch_;
  abi::UInt32 currentItem_ = kNoItem;
  int itemsEntered_ = 0;
  LONG refs_ = 1;
};

struct SweepResult {
  bool ok = false;
  int decoded = 0;
  int skipped = 0;
  uint64_t drainedBytes = 0;
  int itemsEntered = 0;
  double fraction = 0.0;
};

// The single-pass harvest for one solid block.
inline SweepResult RunSolidSweep(const ArchiveReader& reader,
                                 const std::vector<uint32_t>& indices,
                                 const std::vector<uint64_t>& sizes,
                                 prism::gallery::SolidSweepSession::IDecoder& decoder,
                                 prism::gallery::ThumbnailLru& lru,
                                 uint64_t decodeLimit = prism::QuickLookPlanner::kDefaultByteLimit,
                                 bool startAbandoned = false) {
  prism::gallery::SolidSweepSession session(indices, sizes, decodeLimit, decoder, lru);
  if (startAbandoned) session.Abandon();
  ScratchStream scratch(32ull << 20);  // one 32 MB scratch, reused per item
  SweepExtractCallback callback(&session, &scratch);
  SweepResult result;
  result.ok = reader.ExtractBatch(indices, &callback);
  result.decoded = session.decodedCount;
  result.skipped = session.skippedCount;
  result.drainedBytes = session.drainedBytes;
  result.itemsEntered = callback.itemsEntered();
  result.fraction = session.Fraction();
  return result;
}

}  // namespace prism::win::sweep
