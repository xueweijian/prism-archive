// PrismArchive -- PrismWin
// Quick Look content PROVIDERS for the classes PrismCore names (blueprint 9.1,
// ADR-001: system components only -- Windows.Data.Pdf, Media Foundation, WIC;
// no WinUI/XAML, no vendored PDF/audio engine).
//
// Three rules this file exists to enforce:
//   1. NO DISK. The text provider reads the first N KiB straight out of the
//      7-Zip extraction stream (in-memory sink), and the PDF provider feeds
//      Windows.Data.Pdf an IRandomAccessStream built from memory. The Windows
//      suite proves it by snapshotting the temp directory around both calls.
//   2. NO SILENT DEGRADATION. Every provider reports ok=false with a sentence;
//      the caller decides whether to SKIP (component missing -- CONTRACTS D1:
//      exit code + reason, never a quiet pass) or to FAIL.
//   3. BOUNDED WORK. Budgets come from PreviewPlan, and a provider never reads
//      more than the plan allowed: a Quick Look card must not turn a keystroke
//      into a 900 MB decompression.
#pragma once

#include <windows.h>

#include <objbase.h>
#include <roapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winstring.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if defined(__has_include)
#if __has_include(<winrt/Windows.Data.Pdf.h>)
#define PRISM_HAVE_WINDOWS_DATA_PDF 1
#endif
#endif

#ifdef PRISM_HAVE_WINDOWS_DATA_PDF
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>
#endif

#include "../PrismCore/PreviewPlan.hpp"
#include "PrismEngineBridge.hpp"

namespace prism::win::preview {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// 1. TEXT -- first N KiB through the 7-Zip stream, never a temp file
// ---------------------------------------------------------------------------
// Bounded sink: it ACCEPTS everything (7-Zip must never see a failed write and
// abort the extraction) but only the first capacity bytes are retained.
class PreviewPrefixSink final : public abi::ISequentialOutStream {
 public:
  explicit PreviewPrefixSink(size_t capacity) : capacity_(capacity) {}

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
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP Write(const void* data, abi::UInt32 size, abi::UInt32* processed) override {
    if (processed) *processed = size;  // consumed, just not all retained
    total_ += size;
    if (!data || size == 0) return S_OK;
    if (bytes_.size() < capacity_) {
      const size_t room = capacity_ - bytes_.size();
      const size_t take = size < room ? size : static_cast<size_t>(size);
      const auto* p = static_cast<const uint8_t*>(data);
      bytes_.insert(bytes_.end(), p, p + take);
    }
    return S_OK;
  }

  const std::vector<uint8_t>& bytes() const { return bytes_; }
  uint64_t total() const { return total_; }

 private:
  size_t capacity_;
  std::vector<uint8_t> bytes_;
  uint64_t total_ = 0;
  LONG refs_ = 1;
};

// Extract callback for ONE item: hands 7-Zip a bounded sink and keeps its own
// reference so the bytes survive the engine's Release (the out-parameter
// transfers one reference; this object owns the other).
class PrefixCapture final : public abi::IArchiveExtractCallback {
 public:
  PrefixCapture(abi::UInt32 target, size_t capacity) : target_(target), capacity_(capacity) {}

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
    const ULONG refs = InterlockedDecrement(&refs_);
    if (refs == 0) delete this;
    return refs;
  }

  STDMETHODIMP SetTotal(abi::UInt64) override { return S_OK; }
  STDMETHODIMP SetCompleted(const abi::UInt64*) override { return S_OK; }
  STDMETHODIMP PrepareOperation(abi::Int32) override { return S_OK; }

  STDMETHODIMP GetStream(abi::UInt32 index, abi::ISequentialOutStream** outStream,
                         abi::Int32 askExtractMode) override {
    if (!outStream) return E_POINTER;
    *outStream = nullptr;
    if (askExtractMode != abi::kExtract) return S_OK;
    if (index != target_) return S_OK;
    sink_ = new PreviewPrefixSink(capacity_);
    sink_->AddRef();  // the engine's reference; ours is the constructor's
    *outStream = sink_;
    return S_OK;
  }

  STDMETHODIMP SetOperationResult(abi::Int32 operationResult) override {
    if (operationResult != 0) failed_ = true;
    return S_OK;
  }

  PreviewPrefixSink* sink() const { return sink_; }
  bool failed() const { return failed_; }

  ~PrefixCapture() {
    if (sink_) sink_->Release();
  }

 private:
  abi::UInt32 target_;
  size_t capacity_;
  PreviewPrefixSink* sink_ = nullptr;
  bool failed_ = false;
  LONG refs_ = 1;
};

struct PrefixRead {
  bool ok = false;
  std::vector<uint8_t> bytes;
  uint64_t totalBytes = 0;  // real item size, even when only a prefix is kept
  std::string error;
};

// Read at most "capacity" payload bytes of one entry, in memory.
inline PrefixRead ReadItemPrefix(ArchiveReader& reader, uint32_t index, size_t capacity) {
  PrefixRead out;
  auto* capture = new PrefixCapture(index, capacity);
  const bool extracted = reader.ExtractBatch(std::vector<uint32_t>{index}, capture);
  if (capture->sink() != nullptr) {
    out.bytes = capture->sink()->bytes();
    out.totalBytes = capture->sink()->total();
  }
  out.ok = extracted && !capture->failed() && capture->sink() != nullptr;
  if (!extracted) {
    out.error = "7z Extract failed for the preview stream";
  } else if (capture->sink() == nullptr) {
    out.error = "7z never asked for the preview stream";
  } else if (capture->failed()) {
    out.error = "7z reported a failed item operation";
  }
  capture->Release();
  return out;
}

// GBK/GB18030 -> UTF-8 through the OS NLS tables (no ICU, no committed table).
// GB18030 (54936) is a superset of GBK; 936 is the fallback for systems whose
// NLS files predate it.
inline std::string GbkToUtf8(std::string_view raw) {
  for (const UINT codepage : {54936u, 936u}) {
    const int wideLen = MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(),
                                            static_cast<int>(raw.size()), nullptr, 0);
    if (wideLen <= 0) continue;
    std::wstring wide(static_cast<size_t>(wideLen), L'\0');
    if (MultiByteToWideChar(codepage, MB_ERR_INVALID_CHARS, raw.data(),
                            static_cast<int>(raw.size()), wide.data(), wideLen) <= 0) {
      continue;
    }
    return WideToUtf8(wide);
  }
  return {};
}

struct TextPreview {
  bool ok = false;
  prism::TextEncoding encoding = prism::TextEncoding::Binary;
  prism::TextModel model;
  uint64_t payloadBytes = 0;
  std::string error;
};

// The product path for a text/code card: bounded read -> encoding decision ->
// lined model. GBK is transcoded here because the core has no codepage tables.
inline TextPreview ReadTextPreview(ArchiveReader& reader, uint32_t index,
                                   const prism::TextOptions& options = prism::TextOptions()) {
  TextPreview out;
  // A few bytes past the budget let the model tell "exactly the budget" from
  // "there was more" without a second read.
  const size_t capacity = static_cast<size_t>(options.maxBytes) + 8;
  const PrefixRead prefix = ReadItemPrefix(reader, index, capacity);
  if (!prefix.ok) {
    out.error = prefix.error;
    return out;
  }
  out.payloadBytes = prefix.totalBytes;
  const std::span<const uint8_t> bytes(prefix.bytes.data(), prefix.bytes.size());
  out.encoding = prism::DetectEncoding(bytes);
  if (out.encoding == prism::TextEncoding::Binary) {
    out.error = "Payload is not text (encoding decision: binary)";
    return out;
  }
  const prism::TextTranscoder transcoder =
      out.encoding == prism::TextEncoding::Gbk
          ? prism::TextTranscoder([](std::string_view raw) { return GbkToUtf8(raw); })
          : prism::TextTranscoder();
  out.model = prism::BuildTextModel(bytes, out.encoding, options, transcoder);
  if (out.model.transcoderMissing) {
    out.error = "GBK payload but no codepage transcoder available";
    return out;
  }
  out.ok = !out.model.lines.empty();
  if (!out.ok) out.error = "Text payload decoded to zero lines";
  return out;
}

// ---------------------------------------------------------------------------
// 2. PDF -- Windows.Data.Pdf renders page 1 into an in-memory stream, WIC
//    converts it to the BGRA bitmap the renderer already knows (contract C1).
// ---------------------------------------------------------------------------
#ifdef PRISM_HAVE_WINDOWS_DATA_PDF

struct PdfPagePreview {
  bool ok = false;
  uint32_t pageCount = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t stride = 0;
  std::vector<uint8_t> pixelsBgra;  // premultiplied, top-down (C1)
  std::string error;
};

// Activation probe: the class must exist before we promise a PDF card. Called
// once at startup; a false answer becomes the SKIP reason on a bare runner.
// RoGetActivationFactory (not a try/catch) because this probe must stay safe in
// a process that never otherwise touches C++/WinRT.
inline bool PdfRenderAvailable(std::string* why) {
  // RoGetActivationFactory needs an initialised apartment. Doing it here keeps
  // the probe honest in a bare test process as well as in the app (which already
  // initialised COM on its UI thread).
  const HRESULT init = RoInitialize(RO_INIT_MULTITHREADED);
  if (FAILED(init) && init != RPC_E_CHANGED_MODE) {
    if (why) {
      *why = "RoInitialize failed for the PDF probe (hr=0x" +
             std::to_string(static_cast<unsigned long>(init)) + ")";
    }
    return false;
  }
  const wchar_t* className = L"Windows.Data.Pdf.PdfDocument";
  HSTRING handle = nullptr;
  if (FAILED(WindowsCreateString(className, static_cast<UINT32>(wcslen(className)), &handle))) {
    if (why) *why = "WindowsCreateString failed for the PDF class name";
    return false;
  }
  IUnknown* factory = nullptr;
  const HRESULT hr = RoGetActivationFactory(handle, IID_PPV_ARGS(&factory));
  WindowsDeleteString(handle);
  if (SUCCEEDED(hr) && factory != nullptr) {
    factory->Release();
    return true;
  }
  if (why) {
    *why = "Windows.Data.Pdf.PdfDocument unavailable (hr=0x" +
           std::to_string(static_cast<unsigned long>(hr)) + ")";
  }
  return false;
}

// Decode any WIC-supported encoded image (the PDF renderer emits PNG) to
// 32bpp PBGRA: exactly the layout GalleryRenderer/D2D expect.
inline bool DecodeImageToBgra(const std::vector<uint8_t>& encoded, std::vector<uint8_t>* pixels,
                              uint32_t* width, uint32_t* height, uint32_t* stride) {
  if (encoded.empty() || pixels == nullptr || width == nullptr || height == nullptr ||
      stride == nullptr) {
    return false;
  }
  ComPtr<IWICImagingFactory> factory;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(factory.ReleaseAndGetAddressOf())))) {
    return false;
  }
  ComPtr<IWICStream> stream;
  if (FAILED(factory->CreateStream(stream.ReleaseAndGetAddressOf()))) return false;
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()),
                                          static_cast<DWORD>(encoded.size())))) {
    return false;
  }
  ComPtr<IWICBitmapDecoder> decoder;
  if (FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                              decoder.ReleaseAndGetAddressOf()))) {
    return false;
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  if (FAILED(decoder->GetFrame(0, frame.ReleaseAndGetAddressOf()))) return false;
  UINT w = 0, h = 0;
  if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) return false;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(factory->CreateFormatConverter(converter.ReleaseAndGetAddressOf()))) return false;
  if (FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return false;
  }
  const uint32_t rowBytes = static_cast<uint32_t>(w) * 4u;
  std::vector<uint8_t> buffer(static_cast<size_t>(rowBytes) * h);
  if (FAILED(converter->CopyPixels(nullptr, rowBytes, static_cast<UINT>(buffer.size()),
                                   buffer.data()))) {
    return false;
  }
  *pixels = std::move(buffer);
  *width = w;
  *height = h;
  *stride = rowBytes;
  return true;
}

// Render page 1. The PDF bytes go into an InMemoryRandomAccessStream: no file
// is created, so Quick Look never leaves a document copy behind (the Windows
// suite asserts the temp directory is unchanged before/after).
inline PdfPagePreview RenderPdfFirstPage(std::span<const uint8_t> pdf, uint32_t maxEdge = 1600) {
  PdfPagePreview out;
  if (pdf.empty()) {
    out.error = "Empty PDF payload";
    return out;
  }
  try {
    using namespace winrt::Windows::Data::Pdf;
    using namespace winrt::Windows::Storage::Streams;

    InMemoryRandomAccessStream input;
    {
      DataWriter writer(input);
      writer.WriteBytes(winrt::array_view<const uint8_t>(pdf.data(), pdf.data() + pdf.size()));
      writer.StoreAsync().get();
      writer.DetachStream();
    }
    input.Seek(0);

    const PdfDocument document = PdfDocument::LoadFromStreamAsync(input).get();
    out.pageCount = document.PageCount();
    if (out.pageCount == 0) {
      out.error = "PDF has no pages";
      return out;
    }
    const PdfPage page = document.GetPage(0);
    const winrt::Windows::Foundation::Size pageSize = page.Size();

    // Units, measured on this machine rather than assumed (the first version of
    // this code guessed and produced 267 px for a 200 pt page):
    //   * PdfPage.Size() is the page box in POINTS (1/72 inch);
    //   * Windows.Data.Pdf rasterises at 96 DPI, so the bitmap is points * 4/3;
    //   * DestinationWidth/Height are DIPs: 100 DIP came back as 133 px.
    // So one DIP equals one point here, and the preview keeps the native 96 DPI
    // density a PDF viewer shows. Only maxEdge (in PIXELS) caps it.
    const float pixelsPerPoint = 96.0f / 72.0f;
    const float longestPoints = pageSize.Width > pageSize.Height ? pageSize.Width : pageSize.Height;
    const float longestPixels = longestPoints * pixelsPerPoint;
    const float scale = longestPixels > static_cast<float>(maxEdge)
                            ? static_cast<float>(maxEdge) / longestPixels
                            : 1.0f;
    uint32_t dipWidth = static_cast<uint32_t>(pageSize.Width * scale + 0.5f);
    uint32_t dipHeight = static_cast<uint32_t>(pageSize.Height * scale + 0.5f);
    if (dipWidth == 0) dipWidth = 1;
    if (dipHeight == 0) dipHeight = 1;
    PdfPageRenderOptions renderOptions;
    renderOptions.DestinationWidth(dipWidth);
    renderOptions.DestinationHeight(dipHeight);

    InMemoryRandomAccessStream rendered;
    page.RenderToStreamAsync(rendered, renderOptions).get();
    const uint64_t renderedSize = rendered.Size();
    if (renderedSize == 0) {
      out.error = "Windows.Data.Pdf rendered zero bytes";
      return out;
    }
    rendered.Seek(0);
    DataReader reader(rendered.GetInputStreamAt(0));
    reader.LoadAsync(static_cast<uint32_t>(renderedSize)).get();
    std::vector<uint8_t> encoded(reader.UnconsumedBufferLength());
    if (!encoded.empty()) {
      reader.ReadBytes(winrt::array_view<uint8_t>(encoded.data(), encoded.data() + encoded.size()));
    }

    if (!DecodeImageToBgra(encoded, &out.pixelsBgra, &out.width, &out.height, &out.stride)) {
      out.error = "Rendered PDF page did not decode through WIC";
      return out;
    }
    out.ok = true;
    return out;
  } catch (const winrt::hresult_error& e) {
    out.error = "Windows.Data.Pdf failed (hr=0x" +
                std::to_string(static_cast<unsigned long>(e.code().value)) + ")";
    return out;
  } catch (...) {
    out.error = "Windows.Data.Pdf threw a non-HRESULT exception";
    return out;
  }
}

#endif  // PRISM_HAVE_WINDOWS_DATA_PDF

// ---------------------------------------------------------------------------
// 3. AUDIO -- Media Foundation source reader over a memory stream. Metadata
//    only: nothing is played, so a preview costs no audio device.
// ---------------------------------------------------------------------------
struct AudioMetadata {
  bool ok = false;
  uint64_t duration100ns = 0;  // MF_PD_DURATION unit (100 ns ticks)
  uint32_t bitrate = 0;
  uint32_t sampleRate = 0;
  uint16_t channels = 0;
  uint16_t bitsPerSample = 0;
  std::string source;  // "mediafoundation" | "wav-header"
  std::string error;
};

// MFStartup is refcounted; a failed startup means the component is missing and
// the caller must SKIP with this sentence (CONTRACTS D1).
inline bool MediaFoundationAvailable(std::string* why) {
  const HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
  if (SUCCEEDED(hr)) {
    MFShutdown();
    return true;
  }
  if (why) {
    *why = "Media Foundation unavailable (MFStartup hr=0x" +
           std::to_string(static_cast<unsigned long>(hr)) + ")";
  }
  return false;
}

inline AudioMetadata ReadAudioMetadata(std::span<const uint8_t> audio) {
  AudioMetadata out;
  if (audio.empty()) {
    out.error = "Empty audio payload";
    return out;
  }
  // The WAV header is the oracle the pure suite asserts; it also fills fields
  // MF occasionally omits for PCM.
  const prism::WavInfo wav = prism::ParseWavHeader(audio);

  if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET))) {
    out.error = "MFStartup failed";
    return out;
  }
  HRESULT hr = S_OK;
  {
    ComPtr<IStream> memoryStream(SHCreateMemStream(audio.data(), static_cast<UINT>(audio.size())));
    if (!memoryStream) {
      out.error = "SHCreateMemStream failed";
      MFShutdown();
      return out;
    }
    ComPtr<IMFByteStream> byteStream;
    hr = MFCreateMFByteStreamOnStream(memoryStream.Get(), byteStream.ReleaseAndGetAddressOf());
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(hr)) {
      hr = MFCreateSourceReaderFromByteStream(byteStream.Get(), nullptr,
                                              reader.ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(hr)) {
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute(
              static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) &&
          value.vt == VT_UI8) {
        out.duration100ns = static_cast<uint64_t>(value.uhVal.QuadPart);
      }
      PropVariantClear(&value);

      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute(
              static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_AUDIO_ENCODING_BITRATE,
              &value)) &&
          value.vt == VT_UI4) {
        out.bitrate = value.ulVal;
      }
      PropVariantClear(&value);

      ComPtr<IMFMediaType> mediaType;
      if (SUCCEEDED(reader->GetCurrentMediaType(
              static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM),
              mediaType.ReleaseAndGetAddressOf()))) {
        out.sampleRate =
            MFGetAttributeUINT32(mediaType.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
        out.channels = static_cast<uint16_t>(
            MFGetAttributeUINT32(mediaType.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
        out.bitsPerSample = static_cast<uint16_t>(
            MFGetAttributeUINT32(mediaType.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0));
        if (out.bitrate == 0) {
          out.bitrate = MFGetAttributeUINT32(mediaType.Get(), MF_MT_AVG_BITRATE, 0);
        }
      }
      out.source = "mediafoundation";
    }
  }
  MFShutdown();

  if (SUCCEEDED(hr)) {
    // Fill gaps from the container header rather than reporting zeros.
    if (wav.ok) {
      if (out.sampleRate == 0) out.sampleRate = wav.sampleRate;
      if (out.channels == 0) out.channels = wav.channels;
      if (out.bitsPerSample == 0) out.bitsPerSample = wav.bitsPerSample;
      if (out.bitrate == 0) out.bitrate = wav.bitrate;
      if (out.duration100ns == 0) {
        out.duration100ns = wav.duration100ns;
        out.source = "wav-header";
      }
    }
    out.ok = out.sampleRate != 0;
    if (!out.ok) out.error = "Media Foundation opened the stream but reported no audio type";
    return out;
  }

  // MF failed. A PCM WAV is still describable from its header, and the source
  // tag keeps that degradation visible instead of pretending.
  if (wav.ok) {
    out.duration100ns = wav.duration100ns;
    out.bitrate = wav.bitrate;
    out.sampleRate = wav.sampleRate;
    out.channels = wav.channels;
    out.bitsPerSample = wav.bitsPerSample;
    out.source = "wav-header";
    out.ok = true;
    return out;
  }
  out.error =
      "MF source reader failed (hr=0x" + std::to_string(static_cast<unsigned long>(hr)) + ")";
  return out;
}

}  // namespace prism::win::preview
