// PrismArchive -- PrismWin
// GlassIslandHost: the DirectComposition host for the floating Liquid Glass
// islands (blueprint ch. 3.1/3.2) AND the Direct2D 1.3 effect pipeline that
// actually renders them.
//
// There are two layers in this file, on purpose:
//
//   GlassEffectRuntime / GlassChain -- the optics. Two registered custom
//     effects (PrismKawaseBlur, PrismLiquidGlass) composed as
//         backdrop bitmap -> Kawase pass 1 -> Kawase pass 2 -> LiquidGlass
//     and drawn with ID2D1DeviceContext::DrawImage. Works on any
//     ID2D1DeviceContext (hardware, WARP, or a composition swapchain surface),
//     which is what makes it pixel-testable headlessly in
//     Tests/win/gallery_snapshot.cpp.
//
//   GlassIslandHost -- the composition. IDCompositionDevice + one
//     CreateSwapChainForComposition (DXGI_ALPHA_MODE_PREMULTIPLIED) + one
//     visual per island, exactly as ADR-001 requires. If DirectComposition is
//     unavailable the host reports UsesComposition() == false and the caller
//     keeps drawing straight to the window's D2D target (the pre-DComp path).
//
// ADR-001 red lines enforced here:
//   * no Windows App SDK / Microsoft.UI.Xaml / WinUI: this TU includes only
//     d3d11 / dxgi1_3 / dcomp / d2d1_3 / d3dcompiler;
//   * the glass refracts CONTENT WE RENDER (the backdrop bitmap handed to
//     GlassChain), never the desktop or another process's window.
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1_3.h>
#include <d2d1effectauthor.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_3.h>

#include <wrl/client.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "../PrismCore/GlassIsland.hpp"

// The header compiles its own shaders (D3DCompile), so every consumer needs the
// compiler import library; asking each CMake target to remember it is how a
// link error appears only on the target nobody rebuilt.
#pragma comment(lib, "d3dcompiler.lib")

namespace prism::win::glass {

using Microsoft::WRL::ComPtr;

// Effect CLSIDs. Fixed forever: a saved effect graph or a cached shader blob is
// keyed by these, and two different GUIDs for the same effect is how a
// "registered" effect silently never gets found.
inline constexpr GUID kKawaseBlurEffectId = {
    0x9f2c41a7, 0x5b3e, 0x4c88, {0x9d, 0x1a, 0x2b, 0x7e, 0x40, 0x11, 0x6c, 0x51}};
inline constexpr GUID kLiquidGlassEffectId = {
    0x9f2c41a8, 0x5b3e, 0x4c88, {0x9d, 0x1a, 0x2b, 0x7e, 0x40, 0x11, 0x6c, 0x52}};

// A6: the LiquidGlassConstants cbuffer is 5 registers / 20 floats, written by
// prism::glass::GlassIslandScene::WriteCbuffer (bit-asserted in
// Tests/test_glass_island.cpp). Never reorder.
inline constexpr UINT32 kGlassConstantFloats = 20;
inline constexpr UINT32 kGlassConstantBytes = kGlassConstantFloats * sizeof(float);

struct GlassConstants {
  float v[kGlassConstantFloats] = {};
};

// One register (16 B) for the Kawase pass: x = tap offset in pixels,
// y = pass scale (1.0 first pass, 0.5 second pass).
struct KawaseConstants {
  float radiusPx = 8.0f;
  float passScale = 1.0f;
  float reserved0 = 0.0f;
  float reserved1 = 0.0f;
};

namespace detail {

class GlassEffectImpl;

// How the constant buffers reach the shader.
//
// The documented route is a custom D2D property on the effect, updated with
// ID2D1Effect::SetValue. That route is closed on this SDK: every typed custom
// property (float / vector4 / blob, with or without a D2D1_PROPERTY_BINDING)
// makes RegisterEffectFromString return E_INVALIDARG, while the same XML with no
// custom property registers fine -- probed in isolation (six variants,
// 2026-11-01). D2D's effect model exposes no other public channel for per-frame
// uniform data (ID2D1DrawInfo is handed to the transform, not to the caller),
// so the chain that owns the ID2D1Effect hands the constants to the instance
// D2D just created. CreateEffect is synchronous and rendering is single
// threaded by contract, so the thread-local below is exact, not a race.
inline thread_local GlassEffectImpl* g_lastCreated = nullptr;

// Shader bytecode + the D2D factory it was registered against. D2D registers
// effects per factory, so a second factory needs a second registration.
struct EffectRegistry {
  ID2D1Factory1* factory = nullptr;
  std::vector<BYTE> kawaseCode;
  std::vector<BYTE> liquidCode;
  bool registered = false;
};
inline EffectRegistry& Registry() {
  static EffectRegistry registry;
  return registry;
}

// Resolves #include "..." relative to the shader's own directory. D3DCompile's
// default handler searches the working directory, which is wrong here (the app
// runs from its install dir, the test from the build dir).
class SiblingInclude final : public ID3DInclude {
 public:
  explicit SiblingInclude(std::wstring dir) : dir_(std::move(dir)) {}
  HRESULT __stdcall Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID, LPCVOID* data,
                         UINT* bytes) override {
    if (!name || !data || !bytes) return E_INVALIDARG;
    const std::wstring wide = Widen(name);
    const std::wstring full = dir_ + L"\\" + wide;
    HANDLE file = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return E_FAIL;
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > (1 << 20)) {
      CloseHandle(file);
      return E_FAIL;
    }
    auto* buffer = new (std::nothrow) char[static_cast<size_t>(size.QuadPart)];
    if (!buffer) {
      CloseHandle(file);
      return E_OUTOFMEMORY;
    }
    DWORD read = 0;
    const BOOL ok = ReadFile(file, buffer, static_cast<DWORD>(size.QuadPart), &read, nullptr);
    CloseHandle(file);
    if (!ok || read != static_cast<DWORD>(size.QuadPart)) {
      delete[] buffer;
      return E_FAIL;
    }
    *data = buffer;
    *bytes = read;
    return S_OK;
  }
  HRESULT __stdcall Close(LPCVOID data) override {
    delete[] static_cast<const char*>(data);
    return S_OK;
  }

 private:
  static std::wstring Widen(const char* text) {
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wide <= 1) return {};
    std::wstring out(static_cast<size_t>(wide - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), wide);
    return out;
  }
  std::wstring dir_;
};

inline bool ReadFileBytes(const std::wstring& path, std::string* out) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER size = {};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) {
    CloseHandle(file);
    return false;
  }
  out->resize(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, out->data(), static_cast<DWORD>(out->size()), &read, nullptr);
  CloseHandle(file);
  return ok && read == out->size();
}

// Effect implementation shared by both shaders. The only differences are the
// CLSID, the bytecode and the constant-buffer size.
class GlassEffectImpl final : public ID2D1EffectImpl, public ID2D1DrawTransform {
 public:
  GlassEffectImpl(const GUID& id, const BYTE* code, UINT32 size, UINT32 constantBytes)
      : id_(id), code_(code, code + size), constantBytes_(constantBytes) {
    constants_.assign(constantBytes, 0);
  }

  // --- ID2D1EffectImpl -----------------------------------------------------
  IFACEMETHODIMP Initialize(ID2D1EffectContext* context, ID2D1TransformGraph* graph) override {
    if (!context || !graph) return E_INVALIDARG;
    const HRESULT hr = context->LoadPixelShader(id_, code_.data(),
                                                static_cast<UINT32>(code_.size()));
    if (FAILED(hr)) return hr;
    return graph->SetSingleTransformNode(this);
  }
  IFACEMETHODIMP PrepareForRender(D2D1_CHANGE_TYPE) override {
    if (!drawInfo_) return E_UNEXPECTED;
    return drawInfo_->SetPixelShaderConstantBuffer(constants_.data(), constantBytes_);
  }
  IFACEMETHODIMP SetGraph(ID2D1TransformGraph* graph) override {
    return graph ? graph->SetSingleTransformNode(this) : E_INVALIDARG;
  }

  // --- ID2D1DrawTransform --------------------------------------------------
  IFACEMETHODIMP SetDrawInfo(ID2D1DrawInfo* drawInfo) override {
    if (!drawInfo) return E_INVALIDARG;
    drawInfo_ = drawInfo;
    return drawInfo_->SetPixelShader(id_);
  }

  // --- ID2D1Transform ------------------------------------------------------
  // Pass-through geometry: the optics live in the shader, and growing the
  // output rect here would make D2D clamp the very taps we need.
  IFACEMETHODIMP MapOutputRectToInputRects(const D2D1_RECT_L* outputRect, D2D1_RECT_L* inputRects,
                                           UINT32 inputRectsCount) const override {
    if (!outputRect || !inputRects || inputRectsCount != 1) return E_INVALIDARG;
    inputRects[0] = *outputRect;
    return S_OK;
  }
  IFACEMETHODIMP MapInputRectsToOutputRect(const D2D1_RECT_L* inputRects,
                                           const D2D1_RECT_L* inputOpaqueSubRects,
                                           UINT32 inputRectCount, D2D1_RECT_L* outputRect,
                                           D2D1_RECT_L* outputOpaqueSubRect) override {
    if (!inputRects || !outputRect || !outputOpaqueSubRect || inputRectCount != 1) {
      return E_INVALIDARG;
    }
    *outputRect = inputRects[0];
    *outputOpaqueSubRect = inputOpaqueSubRects ? inputOpaqueSubRects[0] : inputRects[0];
    return S_OK;
  }
  IFACEMETHODIMP MapInvalidRect(UINT32, D2D1_RECT_L invalidInputRect,
                                D2D1_RECT_L* invalidOutputRect) const override {
    if (!invalidOutputRect) return E_INVALIDARG;
    *invalidOutputRect = invalidInputRect;
    return S_OK;
  }

  // --- ID2D1TransformNode --------------------------------------------------
  IFACEMETHODIMP_(UINT32) GetInputCount() const override { return 1; }

  // --- constants (called by the owning GlassChain, never by D2D) -----------
  void ApplyConstants(const void* data, UINT32 size) {
    if (data && size == constantBytes_) std::memcpy(constants_.data(), data, size);
  }

  // --- IUnknown ------------------------------------------------------------
  IFACEMETHODIMP QueryInterface(REFIID iid, void** object) override {
    if (!object) return E_POINTER;
    *object = nullptr;
    void* found = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(ID2D1EffectImpl)) {
      found = static_cast<ID2D1EffectImpl*>(this);
    } else if (iid == __uuidof(ID2D1DrawTransform)) {
      found = static_cast<ID2D1DrawTransform*>(this);
    } else if (iid == __uuidof(ID2D1Transform)) {
      found = static_cast<ID2D1Transform*>(this);
    } else if (iid == __uuidof(ID2D1TransformNode)) {
      found = static_cast<ID2D1TransformNode*>(this);
    } else {
      return E_NOINTERFACE;
    }
    *object = found;
    AddRef();
    return S_OK;
  }
  IFACEMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  IFACEMETHODIMP_(ULONG) Release() override {
    const ULONG left = --refs_;
    if (!left) delete this;
    return left;
  }

 private:
  ~GlassEffectImpl() = default;

  GUID id_;
  std::vector<BYTE> code_;
  UINT32 constantBytes_ = 0;
  std::vector<BYTE> constants_;
  ID2D1DrawInfo* drawInfo_ = nullptr;  // owned by D2D, valid between SetDrawInfo calls
  ULONG refs_ = 1;                     // the factory callback hands over one reference
};

inline HRESULT CALLBACK CreateKawaseEffect(IUnknown** effectImpl) {
  if (!effectImpl) return E_POINTER;
  const EffectRegistry& registry = Registry();
  auto* impl = new (std::nothrow) GlassEffectImpl(
      kKawaseBlurEffectId, registry.kawaseCode.data(),
      static_cast<UINT32>(registry.kawaseCode.size()), sizeof(KawaseConstants));
  if (!impl) return E_OUTOFMEMORY;
  g_lastCreated = impl;  // claimed by the GlassChain that called CreateEffect
  *effectImpl = static_cast<ID2D1EffectImpl*>(impl);
  return S_OK;
}

inline HRESULT CALLBACK CreateLiquidGlassEffect(IUnknown** effectImpl) {
  if (!effectImpl) return E_POINTER;
  const EffectRegistry& registry = Registry();
  auto* impl = new (std::nothrow) GlassEffectImpl(
      kLiquidGlassEffectId, registry.liquidCode.data(),
      static_cast<UINT32>(registry.liquidCode.size()), kGlassConstantBytes);
  if (!impl) return E_OUTOFMEMORY;
  g_lastCreated = impl;  // claimed by the GlassChain that called CreateEffect
  *effectImpl = static_cast<ID2D1EffectImpl*>(impl);
  return S_OK;
}

inline const wchar_t* KawaseXml() {
  return L"<?xml version='1.0'?>"
         L"<Effect>"
         L"<Property name='DisplayName' type='string' value='Prism Kawase Blur'/>"
         L"<Property name='Author' type='string' value='PrismArchive'/>"
         L"<Property name='Category' type='string' value='Prism'/>"
         L"<Property name='Description' type='string' value='Dual-pass Kawase blur'/>"
         L"<Inputs><Input name='Source'/></Inputs>"
         L"</Effect>";
}

inline const wchar_t* LiquidGlassXml() {
  return L"<?xml version='1.0'?>"
         L"<Effect>"
         L"<Property name='DisplayName' type='string' value='Prism Liquid Glass'/>"
         L"<Property name='Author' type='string' value='PrismArchive'/>"
         L"<Property name='Category' type='string' value='Prism'/>"
         L"<Property name='Description' type='string' value='Liquid Glass optics'/>"
         L"<Inputs><Input name='Source'/></Inputs>"
         L"</Effect>";
}

}  // namespace detail

// Compiles one effect shader from <shaderDir>\<file> and registers both effects
// on the factory. Idempotent per factory. Returns the failing HRESULT so the
// caller can report exactly which stage broke instead of "glass is grey".
class GlassEffectRuntime {
 public:
  // D3DCompile is available on every Windows 10+ machine (d3dcompiler_47.dll).
  // A shipping build should embed the .cso instead of compiling at startup;
  // see the PR description for the CMake patch that closes that gap.
  HRESULT Initialize(ID2D1Factory1* factory, const std::wstring& shaderDir) {
    if (!factory) return E_INVALIDARG;
    factory_ = factory;
    detail::EffectRegistry& registry = detail::Registry();
    if (registry.registered && registry.factory == factory) return S_OK;

    const std::wstring liquidPath = shaderDir + L"\\LiquidGlassEffect.hlsl";
    const std::wstring kawasePath = shaderDir + L"\\KawaseDualBlur.hlsl";
    HRESULT hr = Compile(liquidPath, nullptr, &registry.liquidCode);
    if (FAILED(hr)) return hr;
    const D3D_SHADER_MACRO kawaseDefines[] = {{"PRISM_D2D_EFFECT", "1"}, {nullptr, nullptr}};
    hr = Compile(kawasePath, kawaseDefines, &registry.kawaseCode);
    if (FAILED(hr)) return hr;

    registry.factory = factory;
    hr = factory->RegisterEffectFromString(kKawaseBlurEffectId, detail::KawaseXml(), nullptr, 0,
                                           detail::CreateKawaseEffect);
    if (FAILED(hr)) return hr;
    hr = factory->RegisterEffectFromString(kLiquidGlassEffectId, detail::LiquidGlassXml(),
                                           nullptr, 0, detail::CreateLiquidGlassEffect);
    if (FAILED(hr)) return hr;
    registry.registered = true;
    return S_OK;
  }

  // Diagnostics the snapshot test prints, so a red run says WHY.
  HRESULT LastCompileError() const { return lastCompileError_; }
  const std::string& LastCompileMessage() const { return lastCompileMessage_; }

  bool Ready() const { return detail::Registry().registered; }

 private:
  HRESULT Compile(const std::wstring& path, const D3D_SHADER_MACRO* defines,
                  std::vector<BYTE>* bytecode) {
    std::string source;
    if (!detail::ReadFileBytes(path, &source)) {
      lastCompileError_ = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
      lastCompileMessage_ = "shader not found";
      return lastCompileError_;
    }
    const size_t slash = path.find_last_of(L"\\/");
    detail::SiblingInclude include(slash == std::wstring::npos ? std::wstring()
                                                               : path.substr(0, slash));
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "prism_glass.hlsl", defines,
                                  &include, "main", "ps_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, code.GetAddressOf(),
                                  errors.GetAddressOf());
    lastCompileError_ = hr;
    if (FAILED(hr)) {
      lastCompileMessage_.assign(
          errors ? static_cast<const char*>(errors->GetBufferPointer()) : "D3DCompile failed");
      return hr;
    }
    bytecode->assign(static_cast<const BYTE*>(code->GetBufferPointer()),
                     static_cast<const BYTE*>(code->GetBufferPointer()) + code->GetBufferSize());
    return S_OK;
  }

  ID2D1Factory1* factory_ = nullptr;
  HRESULT lastCompileError_ = S_OK;
  std::string lastCompileMessage_;
};

// The composition: backdrop -> blur -> blur -> glass. Owns three effects and
// nothing else, so it can be used on a swapchain surface, a WARP device
// context in a test, or any other ID2D1DeviceContext.
class GlassChain {
 public:
  enum class Kind { KawaseEffect, LiquidEffect };

  HRESULT Initialize(ID2D1DeviceContext* context, const KawaseConstants& first,
                     const KawaseConstants& second) {
    if (!context) return E_INVALIDARG;
    context_ = context;
    HRESULT hr = Create(Kind::KawaseEffect, blurA_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = Create(Kind::KawaseEffect, blurB_.GetAddressOf());
    if (FAILED(hr)) return hr;
    hr = Create(Kind::LiquidEffect, liquid_.GetAddressOf());
    if (FAILED(hr)) return hr;
    SetKawaseConstants(first, second);
    // backdrop -> blurA -> blurB -> glass. Every node needs its input, or D2D
    // reports D2DERR_INVALID_GRAPH_CONFIGURATION at EndDraw (not at DrawImage).
    blurB_->SetInputEffect(0, blurA_.Get());
    liquid_->SetInputEffect(0, blurB_.Get());
    return S_OK;
  }

  // constants == the island's GlassIslandScene::WriteCbuffer output.
  HRESULT Draw(ID2D1Bitmap* backdrop, const GlassConstants& constants) {
    if (!blurA_ || !blurB_) return E_UNEXPECTED;
    blurA_->SetInput(0, backdrop);

    // Rebuild the glass node for this frame. D2D only re-uploads a custom
    // effect's shader constants from PrepareForRender, and it schedules that
    // when ITS model changes -- not when an outside owner changes a buffer
    // (see the note on g_lastCreated: the property system, the normal way to
    // tell D2D, rejects custom properties on this SDK). A fresh instance is
    // guaranteed to run Initialize -> SetDrawInfo -> PrepareForRender once, so
    // the constants are always the ones for this frame. Revisit when the
    // property route works; the optics do not depend on the workaround.
    liquid_.Reset();
    liquidImpl_ = nullptr;
    HRESULT hr = Create(Kind::LiquidEffect, liquid_.GetAddressOf());
    if (FAILED(hr)) return hr;
    liquid_->SetInputEffect(0, blurB_.Get());

#ifdef PRISM_GLASS_MUTANT_NO_REFRACTION
    // D3 negative control: the lens band is flattened, so the snapshot's
    // "band differs from the pure-blur baseline" assertion must go red.
    GlassConstants mutated = constants;
    mutated.v[10] = 0.0f;  // u_iorStrength
    SetGlassConstants(mutated);
#else
    SetGlassConstants(constants);
#endif
    context_->DrawImage(liquid_.Get(), D2D1_INTERPOLATION_MODE_LINEAR,
                        D2D1_COMPOSITE_MODE_SOURCE_OVER);
    return S_OK;
  }

  ID2D1Effect* Output() const { return liquid_.Get(); }
  bool Ready() const { return liquid_ && blurA_ && blurB_; }

  // The chain holds the effect instances, so it also holds their constant
  // buffers: Kawase registers are fixed at build time, the glass block is
  // refreshed per frame (pointer light, spring morph, theme).
  void SetGlassConstants(const GlassConstants& constants) {
    if (liquidImpl_) liquidImpl_->ApplyConstants(&constants, sizeof(constants));
  }
  void SetKawaseConstants(const KawaseConstants& first, const KawaseConstants& second) {
    if (blurAImpl_) blurAImpl_->ApplyConstants(&first, sizeof(first));
    if (blurBImpl_) blurBImpl_->ApplyConstants(&second, sizeof(second));
  }

  // CreateEffect runs our D2D factory callback synchronously, so the instance
  // it just built is available on this thread immediately afterwards.
  HRESULT Create(Kind kind, ID2D1Effect** effect) {
    detail::g_lastCreated = nullptr;
    const GUID& id = kind == Kind::KawaseEffect ? kKawaseBlurEffectId : kLiquidGlassEffectId;
    const HRESULT hr = context_->CreateEffect(id, effect);
    if (FAILED(hr)) return hr;
    detail::GlassEffectImpl* impl = detail::g_lastCreated;
    detail::g_lastCreated = nullptr;
    if (!impl) return E_UNEXPECTED;
    if (kind == Kind::KawaseEffect) {
      (blurA_ ? blurBImpl_ : blurAImpl_) = impl;
    } else {
      liquidImpl_ = impl;
    }
    return S_OK;
  }

  ComPtr<ID2D1DeviceContext> context_;
  ComPtr<ID2D1Effect> blurA_;
  ComPtr<ID2D1Effect> blurB_;
  ComPtr<ID2D1Effect> liquid_;
  detail::GlassEffectImpl* blurAImpl_ = nullptr;
  detail::GlassEffectImpl* blurBImpl_ = nullptr;
  detail::GlassEffectImpl* liquidImpl_ = nullptr;
};

// ---------------------------------------------------------------------------
// DirectComposition host: one visual + one premultiplied composition swapchain
// per island. Every method is a no-op returning false when composition is
// unavailable -- callers check UsesComposition() once and then either paint
// into the swapchain (here) or straight into the window's D2D target.
// ---------------------------------------------------------------------------
class GlassIslandHost {
 public:
  struct IslandSurface {
    ComPtr<IDXGISwapChain1> swapchain;
    ComPtr<ID2D1Bitmap1> d2dBitmap;  // the D2D target bound to the surface
    ComPtr<IDCompositionVisual> visual;
    UINT width = 1;
    UINT height = 1;
  };

  bool Initialize(HWND hwnd, const std::wstring& shaderDir) {
    hwnd_ = hwnd;
    // 1. D3D11 device (BGRA support is required for D2D interop).
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, d3d_.ReleaseAndGetAddressOf(), nullptr,
                                   nullptr);
    if (FAILED(hr)) {
      // WARP keeps the glass alive on machines with no usable GPU driver.
      hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                             d3d_.ReleaseAndGetAddressOf(), nullptr, nullptr);
      if (FAILED(hr)) return false;
    }
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(d3d_.As(&dxgiDevice))) return false;

    // 2. D2D factory (Factory1: RegisterEffectFromString lives there) + device.
    D2D1_FACTORY_OPTIONS options = {};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                           reinterpret_cast<void**>(d2dFactory_.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) return false;
    hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), d2dDevice_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;
    hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
                                         d2dContext_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return false;

    // 3. Optics (optional: without shaders we still composite, just flat).
    effects_.Initialize(d2dFactory_.Get(), shaderDir);
    opticsReady_ = effects_.Ready();

    // 4. DirectComposition. Failure is a supported outcome (remote session,
    //    composition disabled): the caller falls back to direct D2D.
    compositionReady_ = false;
    if (SUCCEEDED(DCompositionCreateDevice(
            dxgiDevice.Get(), __uuidof(IDCompositionDevice),
            reinterpret_cast<void**>(dcomp_.ReleaseAndGetAddressOf())))) {
      if (SUCCEEDED(dcomp_->CreateTargetForHwnd(hwnd, /*topmost=*/TRUE,
                                                target_.ReleaseAndGetAddressOf())) &&
          SUCCEEDED(dcomp_->CreateVisual(root_.ReleaseAndGetAddressOf())) &&
          SUCCEEDED(target_->SetRoot(root_.Get())) && SUCCEEDED(dcomp_->Commit())) {
        compositionReady_ = true;
      }
    }
    return true;
  }

  // Creates one per-island composition surface. Returns false when composition
  // is unavailable or the size is degenerate.
  bool AddIsland(UINT width, UINT height) {
    if (!compositionReady_ || width == 0 || height == 0) return false;
    IslandSurface surface;
    surface.width = width;
    surface.height = height;
    ComPtr<IDXGIFactory2> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory2),
                                  reinterpret_cast<void**>(factory.ReleaseAndGetAddressOf())))) {
      return false;
    }
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.Stereo = FALSE;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;  // ADR-001
    if (FAILED(factory->CreateSwapChainForComposition(
            d3d_.Get(), &desc, nullptr, surface.swapchain.ReleaseAndGetAddressOf()))) {
      return false;
    }
    ComPtr<IDXGISurface> dxgiSurface;
    if (FAILED(surface.swapchain->GetBuffer(
            0, __uuidof(IDXGISurface),
            reinterpret_cast<void**>(dxgiSurface.ReleaseAndGetAddressOf())))) {
      return false;
    }
    D2D1_BITMAP_PROPERTIES1 props = {};
    props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    if (FAILED(d2dContext_->CreateBitmapFromDxgiSurface(
            dxgiSurface.Get(), props, surface.d2dBitmap.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(dcomp_->CreateVisual(surface.visual.ReleaseAndGetAddressOf()))) return false;
    if (FAILED(surface.visual->SetContent(surface.swapchain.Get()))) return false;
    if (FAILED(root_->AddVisual(surface.visual.Get(), FALSE, nullptr))) return false;
    surfaces_.push_back(std::move(surface));
    return true;
  }

  // Positions an island and draws it. The paint callback draws the island
  // CONTENT (labels, track, glyphs) into the surface; backdrop is the content
  // underneath the island -- that is what the glass refracts (never the
  // desktop, ADR-001).
  template <typename Paint>
  bool PaintIsland(size_t islandIndex, const prism::glass::IslandRect& rect,
                   const GlassConstants& constants,
                   ID2D1Bitmap* backdrop, Paint&& paint) {
    if (islandIndex >= surfaces_.size()) return false;
    IslandSurface& surface = surfaces_[islandIndex];
    if (FAILED(surface.visual->SetOffsetX(static_cast<float>(rect.x))) ||
        FAILED(surface.visual->SetOffsetY(static_cast<float>(rect.y)))) {
      return false;
    }
    if (surface.width != rect.w || surface.height != rect.h) return false;  // caller resizes

    d2dContext_->SetTarget(surface.d2dBitmap.Get());
    d2dContext_->BeginDraw();
    d2dContext_->Clear(D2D1::ColorF(0, 0.0f));
    if (opticsReady_ && backdrop && chain_.Ready()) {
      chain_.Draw(backdrop, constants);
    }
    paint(d2dContext_.Get(), static_cast<float>(rect.w), static_cast<float>(rect.h));
    const HRESULT hr = d2dContext_->EndDraw();
    if (FAILED(hr)) return false;
    if (FAILED(surface.swapchain->Present(1, 0))) return false;
    return SUCCEEDED(dcomp_->Commit());
  }

  // Builds the shared effect chain (two Kawase passes + glass) on this host's
  // device context.
  HRESULT InitializeOptics(const KawaseConstants& first, const KawaseConstants& second) {
    if (!opticsReady_) return E_FAIL;
    return chain_.Initialize(d2dContext_.Get(), first, second);
  }

  bool UsesComposition() const { return compositionReady_; }
  bool OpticsReady() const { return opticsReady_; }
  const std::string& ShaderError() const { return effects_.LastCompileMessage(); }
  ID2D1DeviceContext* Context() const { return d2dContext_.Get(); }
  IDCompositionDevice* CompositionDevice() const { return dcomp_.Get(); }
  size_t IslandCount() const { return surfaces_.size(); }
  GlassChain& Chain() { return chain_; }
  ID2D1Factory1* Factory() const { return d2dFactory_.Get(); }

 private:
  HWND hwnd_ = nullptr;
  ComPtr<ID3D11Device> d3d_;
  ComPtr<ID2D1Factory1> d2dFactory_;
  ComPtr<ID2D1Device> d2dDevice_;
  ComPtr<ID2D1DeviceContext> d2dContext_;
  ComPtr<IDCompositionDevice> dcomp_;
  ComPtr<IDCompositionTarget> target_;
  ComPtr<IDCompositionVisual> root_;
  std::vector<IslandSurface> surfaces_;
  GlassEffectRuntime effects_;
  GlassChain chain_;
  bool compositionReady_ = false;
  bool opticsReady_ = false;
};

// Spring-driven island morph (blueprint ch. 3.4): the pill's width and the blur
// depth are retargeted on the same fluid-sheet preset, so shape and optics
// settle together instead of one lagging the other.
class IslandShapeAnimator {
 public:
  IslandShapeAnimator()
      : width_(prism::kSpringFluidSheet, 0.0), blur_(prism::kSpringFluidSheet, 28.0) {}

  void Retarget(double widthPx, double blurRadiusPx, double nowSeconds) {
    width_.Retarget(widthPx, nowSeconds);
    blur_.Retarget(blurRadiusPx, nowSeconds);
  }
  void Tick(double nowSeconds) {
    width_.Tick(nowSeconds);
    blur_.Tick(nowSeconds);
  }
  double Width() const { return width_.Value(); }
  double BlurRadius() const { return blur_.Value(); }
  bool IsSettled() const { return width_.IsSettled() && blur_.IsSettled(); }

 private:
  prism::SpringAnimator width_;
  prism::SpringAnimator blur_;
};

}  // namespace prism::win::glass