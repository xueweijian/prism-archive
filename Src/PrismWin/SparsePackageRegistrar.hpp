// PrismArchive -- PrismWin
// Sparse package registration (blueprint ch. 2.3).
//
// The point of a sparse package is that we get *package identity* (which the
// Windows 11 modern context menu requires) WITHOUT writing HKCR keys. The
// manifest declares windows.fileExplorerContextMenus; the shell reads it from
// the AppX database instead of the registry.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <filesystem>
#include <string>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>

namespace prism::win::sparse {

struct DeployResult {
  bool ok = false;
  long hresult = 0;
  std::string message;  // winrt error text, never swallowed
};

inline std::wstring WideFromUtf8(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int wide = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
  std::wstring out(static_cast<size_t>(wide), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), wide);
  return out;
}

// Is the sparse package already deployed for the current user?
inline bool IsRegistered(const std::wstring& packageName) {
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    const auto packages = manager.FindPackagesForUser(L"");  // empty = current user
    for (const auto& package : packages) {
      if (package.Id().Name() == packageName) return true;
    }
  } catch (const winrt::hresult_error&) {
    return false;
  }
  return false;
}

// Register (or update) the sparse package from its AppxManifest.xml.
// AddPackageByUriAsync is the supported path for a package that already
// carries identity; the older RegisterPackageByFullNameAsync needs a staged
// package and is not what we ship.
inline DeployResult Deploy(const std::wstring& manifestPath) {
  DeployResult result;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    const winrt::Windows::Foundation::Uri uri{manifestPath};
    const auto deployment = manager.AddPackageByUriAsync(uri, nullptr).get();
    result.ok = SUCCEEDED(static_cast<HRESULT>(deployment.ExtendedErrorCode()));
    result.hresult = static_cast<long>(deployment.ExtendedErrorCode());
    if (!result.ok) result.message = winrt::to_string(deployment.ErrorText());
  } catch (const winrt::hresult_error& error) {
    result.hresult = static_cast<long>(error.code());
    result.message = winrt::to_string(error.message());
  }
  return result;
}

// Remove the deployment again (uninstall path). Never throws.
inline bool Unregister(const std::wstring& packageFullName) {
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    const auto status = manager.RemovePackageAsync(packageFullName).get();
    return SUCCEEDED(static_cast<HRESULT>(status.ExtendedErrorCode()));
  } catch (const winrt::hresult_error&) {
    return false;
  }
}

}  // namespace prism::win::sparse
