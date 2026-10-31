// COM surface contract (B8) -- no 7z.dll needed, runs in milliseconds.
//
// WHY THIS EXISTS (2026-10-31, 18 wasted CI rounds): DiskOutStream inherited
// abi::IOutStream and implemented Seek/SetSize, yet its QueryInterface answered
// only IUnknown + ISequentialOutStream. 7-Zip never static_casts -- it does
//   Z7_DECL_CMyComPtr_QI_FROM(IOutStream, outStream, seqOutStream);
//   if (!outStream) return E_NOTIMPL;      // 7zUpdate.cpp, before any data
// so every mkdir/rename commit died with 0x80004001 while the object was, in
// C++ terms, perfectly correct.
//
// The lesson generalises: for 7-Zip ABI objects, "the class inherits it" is a
// compile-time claim; "QueryInterface answers the IID" is the runtime contract.
// This test pins the runtime side; preflight rule B8 pins the static side.

#include <iostream>

#include "mini_test.hpp"
#include "SevenZipAbi.hpp"

namespace {

using prism::win::abi::IID_IOutStream;
using prism::win::abi::IID_ISequentialOutStream;
using prism::win::abi::IID_IStreamGetSize;

// A stand-in with the SAME inheritance and QI shape the real DiskOutStream
// must have. If someone drops the branch from the real class, preflight B8
// fires; this suite documents WHY the rule exists so it cannot be weakened.
class ProbeOutStream final : public prism::win::abi::IOutStream {
 public:
  STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
    if (!out) return E_POINTER;
    if (iid == IID_IOutStream) {
      *out = static_cast<prism::win::abi::IOutStream*>(this);
      AddRef();
      return S_OK;
    }
    if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
      *out = static_cast<prism::win::abi::ISequentialOutStream*>(this);
      AddRef();
      return S_OK;
    }
    *out = nullptr;
    return E_NOINTERFACE;
  }
  STDMETHODIMP_(ULONG) AddRef() override { return ++refs_; }
  STDMETHODIMP_(ULONG) Release() override { return --refs_; }
  STDMETHODIMP Write(const void*, prism::win::abi::UInt32,
                     prism::win::abi::UInt32* processed) override {
    if (processed) *processed = 0;
    return S_OK;
  }
  STDMETHODIMP Seek(prism::win::abi::Int64, prism::win::abi::UInt32,
                    prism::win::abi::UInt64*) override {
    return S_OK;
  }
  STDMETHODIMP SetSize(prism::win::abi::UInt64) override { return S_OK; }

 private:
  ULONG refs_ = 1;
};

}  // namespace

PRISM_TEST("qi.ioutstream_is_answerable") {
  ProbeOutStream s;
  void* p = nullptr;
  // The single fact that cost 18 CI rounds.
  PRISM_CHECK_EQ(s.QueryInterface(IID_IOutStream, &p), S_OK);
  PRISM_CHECK(p != nullptr);
  // QI must hand back a USABLE IOutStream, not merely a non-null value.
  auto* as_out = static_cast<prism::win::abi::IOutStream*>(p);
  PRISM_CHECK_EQ(as_out->SetSize(4096), S_OK);
  PRISM_CHECK_EQ(as_out->Seek(0, 0, nullptr), S_OK);
  as_out->Release();
}

PRISM_TEST("qi.sequential_still_answerable") {
  ProbeOutStream s;
  void* p = nullptr;
  PRISM_CHECK_EQ(s.QueryInterface(IID_ISequentialOutStream, &p), S_OK);
  PRISM_CHECK(p != nullptr);
  static_cast<prism::win::abi::ISequentialOutStream*>(p)->Release();
}

PRISM_TEST("qi.unknown_iid_rejected_and_nulls_out") {
  ProbeOutStream s;
  void* p = reinterpret_cast<void*>(0x1);
  // A wrong constant must fail AND null the out pointer: a stale non-null out
  // pointer is exactly how a wrong IID "succeeds" silently.
  prism::win::abi::GUID bogus = IID_IOutStream;
  bogus.Data4[5] = 0x07;  // family 03, member 07 -- not a stream interface
  PRISM_CHECK_EQ(s.QueryInterface(bogus, &p), E_NOINTERFACE);
  PRISM_CHECK(p == nullptr);
}

PRISM_TEST("qi.iid_constants_match_guid_txt") {
  // Guid.txt: family 03 = streams. IOutStream is member 04; IStreamGetSize is
  // member 06. Confusing the two was a real defect (host leak, cmds-written).
  PRISM_CHECK_EQ(IID_IOutStream.Data4[3], 0x03);
  PRISM_CHECK_EQ(IID_IOutStream.Data4[5], 0x04);
  PRISM_CHECK_EQ(IID_IStreamGetSize.Data4[5], 0x06);
  PRISM_CHECK_EQ(IID_ISequentialOutStream.Data4[5], 0x01);
  PRISM_CHECK_EQ(IID_IOutStream.Data1, 0x23170F69u);
}

int main(int argc, char** argv) {
  const int failures = prism::test::RunAll(argc, argv);
  if (failures != 0) {
    std::cout << "    COM-QI CONTRACT FAILURES" << std::endl;
    return 1;
  }
  // The marker is part of the verdict (L2): a silent skip can never pass.
  std::cout << "    ALL COM-QI CONTRACT CHECKS PASSED" << std::endl;
  return 0;
}
