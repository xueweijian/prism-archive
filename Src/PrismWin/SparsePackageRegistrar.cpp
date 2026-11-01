// PrismArchive -- PrismWin
// Implementation of the shell registration contract:
//   * GUID/CLSID + package identity strings (shared with the manifest)
//   * the verb -> command line table (shared with both shell DLLs)
//   * sparse-package deploy / verify / unregister through C++/WinRT
//
// The WinRT calls are the only part that needs WindowsApp.lib, which is why the
// declarations stay in the header and this file is a separate static library.
#include "SparsePackageRegistrar.hpp"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Management.Deployment.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace prism::win::shell_ids {

std::wstring GuidToRegistryString(const GUID& id) {
  wchar_t buffer[64]{};
  // "{%08lX-...}" then strip the braces: MSIX manifest text is compared
  // case-insensitively after brace removal, and the registry wants the bare form.
  if (::StringFromGUID2(id, buffer, ARRAYSIZE(buffer)) <= 0) return {};
  std::wstring text(buffer);
  if (!text.empty() && text.front() == L'{') text.erase(0, 1);
  if (!text.empty() && text.back() == L'}') text.pop_back();
  return text;
}

}  // namespace prism::win::shell_ids

namespace prism::win::shell_launch {

namespace {

std::wstring Join(const std::vector<std::wstring>& parts, const wchar_t* separator) {
  std::wstring out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += separator;
    out += parts[i];
  }
  return out;
}

bool IsFile(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attributes = ::GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ReadInstallDir(HKEY root) {
  HKEY key = nullptr;
  if (::RegOpenKeyExW(root, L"SOFTWARE\\PrismArchive", 0,
                      KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
    return {};
  }
  wchar_t value[MAX_PATH * 2]{};
  DWORD bytes = sizeof(value) - sizeof(wchar_t);
  DWORD type = 0;
  const LONG status =
      ::RegQueryValueExW(key, L"InstallDir", nullptr, &type,
                         reinterpret_cast<LPBYTE>(value), &bytes);
  ::RegCloseKey(key);
  if (status != ERROR_SUCCESS || type != REG_SZ) return {};
  return value;
}

std::wstring ExpandEnvironment(const wchar_t* raw) {
  wchar_t buffer[MAX_PATH * 2]{};
  if (::ExpandEnvironmentStringsW(raw, buffer, ARRAYSIZE(buffer)) == 0) return {};
  return buffer;
}

}  // namespace

std::wstring QuoteArg(const std::wstring& arg) {
  // CommandLineToArgvW rules: inside quotes, 2n backslashes followed by a quote
  // produce n backslashes + a literal quote; a run of backslashes at the end
  // must be doubled so the closing quote is not escaped.
  std::wstring out;
  out.reserve(arg.size() + 8);
  out.push_back(L'"');
  size_t backslashes = 0;
  for (const wchar_t c : arg) {
    if (c == L'\\') {
      ++backslashes;
      continue;
    }
    if (c == L'"') {
      out.append(backslashes * 2 + 1, L'\\');
      out.push_back(L'"');
      backslashes = 0;
      continue;
    }
    out.append(backslashes, L'\\');
    backslashes = 0;
    out.push_back(c);
  }
  out.append(backslashes * 2, L'\\');
  out.push_back(L'"');
  return out;
}

std::wstring ArgsForVerb(const std::string& verb,
                         const std::vector<std::wstring>& selected) {
  if (selected.empty()) return {};
  const wchar_t* flag = nullptr;
  if (verb == "smart.extract.here") {
    flag = kSmartExtract;
  } else if (verb == "extract.here") {
    flag = kExtractHere;
  } else if (verb == "extract.to.subfolder") {
    flag = kExtractSub;
  } else if (verb == "test.integrity") {
    flag = kTest;
  } else if (verb == "smart.extract.each.to.own.folder") {
    flag = kSmartExtractEach;
  } else if (verb == "compress.to.7z") {
    flag = kCompress7z;
  } else if (verb == "compress.to.zip") {
    flag = kCompressZip;
  } else if (verb != "open.in.prism") {
    return {};  // unknown verb: refuse, never guess a command line
  }

  std::vector<std::wstring> quoted;
  quoted.reserve(selected.size());
  for (const std::wstring& path : selected) quoted.push_back(QuoteArg(path));
  const std::wstring joined = Join(quoted, L" ");
  if (!flag) return joined;  // open.in.prism: bare path opens the gallery
  return std::wstring(flag) + L" " + joined;
}

std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& args) {
  if (exe.empty()) return {};
  // Quote the executable itself: an install under "C:\Program Files" would
  // otherwise split into two arguments, and CreateProcessW would look for
  // C:\Program.exe (a real, documented hijack primitive).
  const std::wstring quotedExe = QuoteArg(exe);
  return args.empty() ? quotedExe : quotedExe + L" " + args;
}

bool ResolveCompanionExe(std::wstring& out) {
  out.clear();
  for (HKEY root : {HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE}) {
    const std::wstring dir = ReadInstallDir(root);
    if (dir.empty()) continue;
    std::wstring candidate = dir;
    if (!candidate.empty() && candidate.back() != L'\\') candidate.push_back(L'\\');
    candidate += shell_ids::kCompanionExeName;
    if (IsFile(candidate)) {
      out = candidate;
      return true;
    }
  }

  // Next to this module: the common case for a portable / developer layout.
  HMODULE self = nullptr;
  if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ResolveCompanionExe), &self)) {
    wchar_t module[MAX_PATH * 2]{};
    if (::GetModuleFileNameW(self, module, ARRAYSIZE(module)) > 0) {
      std::wstring dir(module);
      const size_t slash = dir.find_last_of(L"\\/");
      if (slash != std::wstring::npos) {
        dir.erase(slash + 1);
        std::wstring candidate = dir + shell_ids::kCompanionExeName;
        if (IsFile(candidate)) {
          out = candidate;
          return true;
        }
      }
    }
  }

  for (const wchar_t* pattern : {L"%ProgramFiles%\\PrismArchive\\" ,
                                 L"%LOCALAPPDATA%\\Programs\\PrismArchive\\"}) {
    std::wstring dir = ExpandEnvironment(pattern);
    if (dir.empty()) continue;
    const std::wstring candidate = dir + shell_ids::kCompanionExeName;
    if (IsFile(candidate)) {
      out = candidate;
      return true;
    }
  }
  return false;
}

bool LaunchCompanion(const std::string& verb, const std::vector<std::wstring>& selected) {
  const std::wstring args = ArgsForVerb(verb, selected);
  if (args.empty()) return false;
  std::wstring exe;
  if (!ResolveCompanionExe(exe)) return false;
  const std::wstring commandLine = BuildCommandLine(exe, args);

  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  // CreateProcessW takes a mutable buffer; the string must outlive the call.
  std::vector<wchar_t> mutableLine(commandLine.begin(), commandLine.end());
  mutableLine.push_back(L'\0');
  const BOOL created =
      ::CreateProcessW(exe.c_str(), mutableLine.data(), nullptr, nullptr, FALSE,
                       CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process);
  if (!created) return false;
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  return true;
}

}  // namespace prism::win::shell_launch

namespace prism::win::sparse {

namespace {

// WinRT needs an initialized apartment on the calling thread. The shell and the
// installer may already be STA, in which case init_apartment throws
// RPC_E_CHANGED_MODE -- the thread is initialized either way, so swallow it.
struct ApartmentGuard {
  ApartmentGuard() noexcept {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
  }
};

bool IsAbsolutePath(const std::wstring& path) {
  if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':') {
    return path[2] == L'\\' || path[2] == L'/';
  }
  if (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/')) return true;  // UNC/rooted
  return false;
}

std::string Utf8FromWide(const std::wstring& text) {
  if (text.empty()) return {};
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                          static_cast<int>(text.size()), nullptr, 0,
                                          nullptr, nullptr);
  if (bytes <= 0) return {};
  std::string out(static_cast<size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), bytes, nullptr, nullptr);
  return out;
}

bool IsUnreserved(const unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
         c == '-' || c == '.' || c == '_' || c == '~';
}

std::wstring PercentEncode(const std::wstring& text, bool keepSlash) {
  static const wchar_t* kHex = L"0123456789ABCDEF";
  const std::string utf8 = Utf8FromWide(text);
  std::wstring out;
  out.reserve(utf8.size() * 3);
  for (const char raw : utf8) {
    const unsigned char c = static_cast<unsigned char>(raw);
    if (IsUnreserved(c) || (keepSlash && c == '/') || c == ':') {
      out.push_back(static_cast<wchar_t>(c));
    } else {
      out.push_back(L'%');
      out.push_back(kHex[(c >> 4) & 0xF]);
      out.push_back(kHex[c & 0xF]);
    }
  }
  return out;
}

std::wstring NormaliseSlashes(std::wstring path) {
  std::replace(path.begin(), path.end(), L'\\', L'/');
  return path;
}

}  // namespace

DeployVerdict Evaluate(const DeployOutcome& outcome) {
  if (!outcome.hresultOk) return DeployVerdict::FailedByHresult;
#if defined(PRISM_MUTANT_ACCEPT_UNVERIFIED)
  // NEGATIVE CONTROL ONLY (Tests/CMakeLists.txt mutation.sparse_registrar.
  // accept_unverified). Trusting SUCCEEDED(ExtendedErrorCode) without asking
  // the package manager again is exactly the bug this guard exists for; the
  // mutation must turn the verdict test red.
  return DeployVerdict::Success;
#else
  // A deploy that reports success but leaves nothing registered is a FAILURE.
  // Anything else makes "registered" a claim instead of a fact.
  if (!outcome.registeredAfter) return DeployVerdict::Unverified;
  return DeployVerdict::Success;
#endif
}

std::wstring MakeFileUri(const std::wstring& absolutePath) {
  if (!IsAbsolutePath(absolutePath)) return {};
  const std::wstring path = NormaliseSlashes(absolutePath);

  if (path.size() >= 2 && path[0] == L'/' && path[1] == L'/') {  // UNC: //host/share/...
    const size_t hostEnd = path.find(L'/', 2);
    if (hostEnd == std::wstring::npos) return L"file://" + PercentEncode(path.substr(2), false);
    const std::wstring host = path.substr(2, hostEnd - 2);
    const std::wstring rest = path.substr(hostEnd + 1);
    return L"file://" + PercentEncode(host, false) + L"/" + PercentEncode(rest, true);
  }

  if (path.size() >= 3 && std::iswalpha(path[0]) && path[1] == L':') {
    const std::wstring drive = path.substr(0, 2);          // "C:"
    const std::wstring rest = path.size() > 3 ? path.substr(3) : std::wstring();
    std::wstring uri = L"file:///" + drive + L"/";
    if (!rest.empty()) uri += PercentEncode(rest, true);
    return uri;
  }

  return L"file://" + PercentEncode(path, true);  // rooted path, no drive
}

RegistrationRequest BuildRequest(const std::wstring& packagePath,
                                 const std::wstring& externalLocation) {
  RegistrationRequest request;
  if (!IsAbsolutePath(packagePath) || !IsAbsolutePath(externalLocation)) return request;
  request.packageUri = MakeFileUri(packagePath);
  request.externalLocationUri = MakeFileUri(externalLocation);
  if (request.packageUri.empty() || request.externalLocationUri.empty()) return request;
  // AddPackageOptions wants a directory URI; keep the trailing slash explicit so
  // "C:\Apps\Prism" never becomes a URI that resolves to the parent.
  if (!request.externalLocationUri.empty() &&
      request.externalLocationUri.back() != L'/') {
    request.externalLocationUri.push_back(L'/');
  }
  request.valid = true;
  return request;
}

RegistrationState IsRegistered(const std::wstring& packageName) {
  if (packageName.empty()) return RegistrationState::Unavailable;
  ApartmentGuard guard;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    for (const auto& package : manager.FindPackagesForUser(L"")) {
      if (package.Id().Name() == packageName) return RegistrationState::Registered;
    }
    return RegistrationState::NotRegistered;
  } catch (const winrt::hresult_error&) {
    // Deliberately distinct from NotRegistered: "could not ask" must never be
    // reported as "not installed", or the installer skips a needed deploy.
    return RegistrationState::Unavailable;
  }
}

std::wstring FindPackageFullName(const std::wstring& packageName) {
  if (packageName.empty()) return {};
  ApartmentGuard guard;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    for (const auto& package : manager.FindPackagesForUser(L"")) {
      if (package.Id().Name() == packageName) return std::wstring(package.Id().FullName());
    }
  } catch (const winrt::hresult_error&) {
    return {};
  }
  return {};
}

DeployResult Deploy(const std::wstring& packagePath, const std::wstring& externalLocation) {
  DeployResult result;
  const RegistrationRequest request = BuildRequest(packagePath, externalLocation);
  if (!request.valid) {
    result.verdict = DeployVerdict::FailedByHresult;
    result.hresult = static_cast<std::int32_t>(E_INVALIDARG);
    result.message = "invalid path: package and external location must be absolute";
    return result;
  }

  ApartmentGuard guard;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    winrt::Windows::Management::Deployment::AddPackageOptions options;
    // Required for a sparse package: without it the deployment cannot resolve
    // the manifest's Application/@Executable against the install directory.
    options.ExternalLocationUri(winrt::Windows::Foundation::Uri{request.externalLocationUri});
    options.DeferRegistrationWhenPackagesAreInUse(false);
    const auto deployment = manager
                                .AddPackageByUriAsync(
                                    winrt::Windows::Foundation::Uri{request.packageUri}, options)
                                .get();
    const winrt::hresult status = deployment.ExtendedErrorCode();
    result.hresult = static_cast<std::int32_t>(status);
    if (FAILED(static_cast<HRESULT>(status))) {
      result.message = winrt::to_string(deployment.ErrorText());
    }
    result.verdict = Evaluate(DeployOutcome{SUCCEEDED(static_cast<HRESULT>(status)),
                                            IsRegistered(shell_ids::kPackageName) ==
                                                RegistrationState::Registered});
  } catch (const winrt::hresult_error& error) {
    result.hresult = static_cast<std::int32_t>(error.code());
    result.message = winrt::to_string(error.message());
    result.verdict = Evaluate(DeployOutcome{false, false});
  }
  result.ok = result.verdict == DeployVerdict::Success;
  if (result.ok) result.packageFullName = FindPackageFullName(shell_ids::kPackageName);
  return result;
}

bool Unregister(const std::wstring& packageFullName, const std::wstring& packageName) {
  if (packageFullName.empty()) return false;
  ApartmentGuard guard;
  try {
    const winrt::Windows::Management::Deployment::PackageManager manager;
    const auto status = manager.RemovePackageAsync(packageFullName).get();
    if (FAILED(static_cast<HRESULT>(status.ExtendedErrorCode()))) return false;
  } catch (const winrt::hresult_error&) {
    return false;
  }
  // Verify the removal instead of trusting the status: a package that is still
  // registered after "uninstall" is the same class of bug as a deploy that is
  // still absent after "install".
  return IsRegistered(packageName) != RegistrationState::Registered;
}

}  // namespace prism::win::sparse
