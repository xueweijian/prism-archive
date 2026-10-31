// PrismArchive -- PrismWin
// PasswordProbe: the thin Windows bridge of the password ring. The executor
// (Probe, ProbeOutcome, CandidateTexts) lives in PrismCore/PasswordRing.hpp --
// COM-free policy that Linux tests drive with fakes. Everything here is
// guarded _WIN32 and only converts "archive path + candidate" into "did this
// password open it?" through the real 7z.dll bridge.
#pragma once

#include <string>

#include "PasswordRing.hpp"

#if defined(_WIN32)
#include "PrismEngineBridge.hpp"

namespace prism::win::pwd {

// Thin bridge for the real engine: one stateless Open per candidate.
// Concurrency-safe by construction (each call owns its library handle and
// reader); LoadLibraryW refcounts the same module, so 4 workers do not pay
// 4 disk reads.
// NOTE: ArchiveReader::Open takes the password as UTF-16; candidates are
// UTF-8 narrow strings, hence the explicit conversion.
inline bool TryOpenWith7z(const std::wstring& archivePath,
                          const std::string& password) {
  wchar_t dll[MAX_PATH];
  if (GetEnvironmentVariableW(L"PRISM_7Z_DLL", dll, MAX_PATH) == 0) return false;
  prism::win::SevenZipLibrary library;
  if (!library.Load(dll)) return false;
  prism::win::ArchiveReader reader(library);
  return reader.Open(archivePath, prism::win::Utf8ToWide(password));
}

}  // namespace prism::win::pwd

#endif  // _WIN32
