// Vocabulary stand-in for the (main-branch, unmodified) 7z.dll bridge.
//
// The drag bridge needs exactly two shapes from it: SevenZipLibrary (only ever
// handed to ArchiveReader by the caller) and ArchiveReader::ExtractBatch. Those
// are declared here with the SAME signatures as the real header so the drag
// bridge's producer/callback wiring typechecks; NOTHING here decodes anything,
// and the Windows CI lane still runs the real 7z.dll end to end.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../SevenZipAbi.hpp"  // the real ABI header, unmodified

namespace prism::win {

class SevenZipLibrary {
 public:
  bool Load(const std::wstring&) { return false; }
  HRESULT CreateObject(const GUID&, const GUID&, void**) { return E_FAIL; }
};

class ArchiveReader {
 public:
  explicit ArchiveReader(SevenZipLibrary& library, void* transcoder = nullptr)
      : library_(library), transcoder_(transcoder) {}
  bool Open(const std::wstring&, const std::wstring& = {}) { return false; }
  void Close() {}
  bool opened() const { return false; }
  const std::string& openTrace() const { return trace_; }
  std::vector<prism::ArchiveItem> ReadCatalog() const { return {}; }
  bool ExtractBatch(const std::vector<uint32_t>&, abi::IArchiveExtractCallback*) const {
    return false;
  }

 private:
  SevenZipLibrary& library_;
  void* transcoder_;
  std::string trace_;
};

}  // namespace prism::win
