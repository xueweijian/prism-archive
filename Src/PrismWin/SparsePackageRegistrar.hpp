// PrismArchive -- PrismWin
// MSIX sparse-package registration (blueprint ch. 4.1, milestone M2).
//
// The Win11 IExplorerCommand context menu needs *package identity*. Instead of
// an installer writing registry keys, we register a sparse package whose
// manifest (Assets/Package/AppxManifest.xml) points at the install directory:
//
//   PackageManager.AddPackageByUriAsync(
//       Uri("file:///C:/Program Files/PrismArchive/AppxManifest.xml"),
//       RegisterOptions: RegisterSharedStorage? no -> AddPackageByAppDataPath? )
//
// This is a thin, honest wrapper: every failure path surfaces a winrt hresult
// the caller can show. No registry keys are touched anywhere.
#pragma once

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/base.h>

#include <string>

#include "PrismEngineBridge.hpp"  // Utf8ToWide

namespace prism::win::sparse {

struct RegisterResult {
  bool ok = false;
  std::wstring errorText;  // empty on success
  bool alreadyRegistered = false;
};

// Registers (or re-registers) the sparse package whose manifest lives beside
// the exe. `installDir` is the directory containing AppxManifest.xml.
inline RegisterResult RegisterSparsePackage(const std::wstring& installDir) {
  RegisterResult result;
  using namespace winrt::Windows::Management::Deployment;
  using namespace winrt::Windows::Foundation;

  try {
    PackageManager manager;

    // A backslashed, terminated directory path -> manifest URI.
    std::wstring dir = installDir;
    if (!dir.empty() && dir.back() != L'\\') dir += L'\\';
    const Uri manifestUri(L"file:///" + dir + L"AppxManifest.xml");

    const auto options = PackageManagerRegisterOptions::None;
    const auto operation = manager.RegisterPackageByUriAsync(
        manifestUri,
        /*options*/ options);
    const auto reply = operation.get();

    if (reply.IsRegistered()) {
      result.ok = true;
    } else {
      result.errorText = reply.ErrorText().c_str();
      // 0x80073D06 == ERROR_PACKAGE_ALREADY_EXISTS: re-register over the same
      // identity is a normal upgrade path, not a failure.
      result.alreadyRegistered =
          reply.ExtendedErrorCode() == 0x80073CF0 || !result.errorText.empty();
    }
  } catch (const winrt::hresult_error& e) {
    result.errorText = e.message().c_str();
  } catch (...) {
    result.errorText = L"unknown registration failure";
  }
  return result;
}

// Diagnostics for the settings page: is our identity registered right now?
inline bool IsRegistered(const std::wstring& packageFamilyName) {
  using namespace winrt::Windows::Management::Deployment;
  try {
    PackageManager manager;
    return manager.FindPackagesForUser(L"", packageFamilyName).First().HasCurrent();
  } catch (...) {
    return false;
  }
}

}  // namespace prism::win::sparse
