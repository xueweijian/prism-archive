// Minimal semantic stand-in for <wrl/client.h> + <wrl/implements.h>.
//
// This exists ONLY so the PrismWin headers can be typechecked and -Werror'd on
// a Linux box before a CI round is spent (CONTRACTS D8). It emulates the two
// WRL facilities the drag layer uses -- RuntimeClass/MakeAndInitialize and
// ComPtr -- faithfully enough that a wrong override signature, a missing
// `override`, an unused member or a bad template argument is a compile error
// here instead of a red Windows lane.
//
// It is NOT coverage: nothing here behaves like the real Shell or 7z.dll, and
// no test may claim behaviour from it. The Windows CI lane stays authoritative.
#pragma once

#pragma once

#ifndef PRISM_WRL_STUB_INCLUDED
#define PRISM_WRL_STUB_INCLUDED

#include <new>
#include <utility>

#include <windows.h>

namespace Microsoft {
namespace WRL {

struct RuntimeClassFlagsBase {};
template <int N>
struct RuntimeClassFlags : RuntimeClassFlagsBase {
  enum { value = N };
};
enum { ClassicCom = 1, WinRt = 2, FtmBase = 4 };

// Single-interface RuntimeClass: the shape every class in this project uses.
template <class Flags, class Interface, class... Rest>
class RuntimeClass : public Interface, public Rest... {
 public:
  RuntimeClass() = default;
  RuntimeClass(const RuntimeClass&) = delete;
  RuntimeClass& operator=(const RuntimeClass&) = delete;

  HRESULT QueryInterface(REFIID, void** out) override {
    if (!out) return E_POINTER;
    *out = nullptr;
    return E_NOINTERFACE;
  }
  ULONG AddRef() override { return static_cast<ULONG>(++refs_); }
  ULONG Release() override {
    LONG r = --refs_;
    if (r == 0) delete this;
    return static_cast<ULONG>(r);
  }

 private:
  LONG refs_ = 1;
};

template <class T>
class ComPtr;

// Real WRL lets call sites write `&comPtr` where a T**/void** is expected
// (that is how `ss->Item(i, &dispatch)` and `IID_PPV_ARGS(&p)` compile at all).
// ComPtrRef is the shim that makes it work; without it the stub rejects code
// the real headers accept, which would be a false failure.
namespace Details {
template <class T>
struct ComPtrRef {
  explicit ComPtrRef(ComPtr<T>* p) : p_(p) {}
  operator ComPtr<T>*() const { return p_; }
  operator T**() const { return p_->ReleaseAndGetAddressOf(); }
  // Two user-defined conversions in a row are not allowed in C++, so the
  // ComPtr<U>* destination has to be reachable directly.
  template <class U>
  operator ComPtr<U>*() const {
    return reinterpret_cast<ComPtr<U>*>(p_);
  }
  operator void**() const {
    return reinterpret_cast<void**>(p_->ReleaseAndGetAddressOf());
  }
  T* const* operator->() const { return p_->GetAddressOf(); }

 private:
  ComPtr<T>* p_;
};
}  // namespace Details

template <class T>
class ComPtr {
 public:
  friend struct Details::ComPtrRef<T>;
  ComPtr() = default;
  ComPtr(std::nullptr_t) {}   // WRL allows `return nullptr;` from a factory
  explicit ComPtr(T* p) : p_(p) {}
  ComPtr(const ComPtr& other) : p_(other.p_) {
    if (p_) p_->AddRef();
  }
  ComPtr(ComPtr&& other) noexcept : p_(other.p_) { other.p_ = nullptr; }
  ~ComPtr() { InternalRelease(); }

  ComPtr& operator=(const ComPtr& other) {
    if (this != &other) {
      InternalRelease();
      p_ = other.p_;
      if (p_) p_->AddRef();
    }
    return *this;
  }
  ComPtr& operator=(ComPtr&& other) noexcept {
    if (this != &other) {
      InternalRelease();
      p_ = other.p_;
      other.p_ = nullptr;
    }
    return *this;
  }

  T* Get() const { return p_; }
  T** GetAddressOf() { return &p_; }
  T** ReleaseAndGetAddressOf() {
    InternalRelease();
    return &p_;
  }
  T* operator->() const { return p_; }
  explicit operator bool() const { return p_ != nullptr; }
  T* Detach() {
    T* p = p_;
    p_ = nullptr;
    return p;
  }
  void Attach(T* p) {
    InternalRelease();
    p_ = p;
  }
  void Reset() { InternalRelease(); }
  // Real WRL resolves `&comPtr` through ComPtrRef, which converts to T**,
  // void** and ComPtr<T>* alike -- that is how `IID_PPV_ARGS(&x)`,
  // `obj.As(&y)` and `MakeAndInitialize<T>(&x, ...)` all compile. Reproducing
  // the shim (rather than just returning T**) is what keeps this gate from
  // reporting failures the real headers would accept.
  Details::ComPtrRef<T> operator&() { return Details::ComPtrRef<T>(this); }

  // Real WRL accepts both As(U**) and As(ComPtr<U>*) -- code in this project
  // writes `dispatch.As(&browser)` with a ComPtr, so the stub must too.
  template <class U>
  HRESULT As(ComPtr<U>* out) const {
    if (!out) return E_POINTER;
    return p_ ? p_->QueryInterface(GUID{}, reinterpret_cast<void**>(out->ReleaseAndGetAddressOf()))
              : E_POINTER;
  }

  template <class U>
  HRESULT As(Details::ComPtrRef<U> out) const {
    return As(static_cast<U**>(out));
  }

  template <class U>
  HRESULT As(U** out) const {
    if (!out) return E_POINTER;
    *out = nullptr;
    return p_ ? p_->QueryInterface(GUID{}, reinterpret_cast<void**>(out)) : E_POINTER;
  }

 private:
  void InternalRelease() {
    if (p_) {
      p_->Release();
      p_ = nullptr;
    }
  }
  T* p_ = nullptr;
};

// Make<T>: construct and hand over one reference.
template <class T, class... Args>
ComPtr<T> Make(Args&&... args) {
  return ComPtr<T>(new T(std::forward<Args>(args)...));
}

// MakeAndInitialize<T>: uniform-initialize via RuntimeClassInitialize.
template <class T, class... Args>
HRESULT MakeAndInitialize(ComPtr<T>* out, Args&&... args) {
  if (!out) return E_POINTER;
  out->Reset();
  T* object = new (std::nothrow) T();
  if (!object) return E_OUTOFMEMORY;
  const HRESULT hr = object->RuntimeClassInitialize(std::forward<Args>(args)...);
  if (FAILED(hr)) {
    object->Release();
    return hr;
  }
  out->Attach(object);
  return S_OK;
}

}  // namespace WRL
}  // namespace Microsoft

#endif  // PRISM_WRL_STUB_INCLUDED
