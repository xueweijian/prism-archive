// PrismArchive -- PrismWin
// Shell registration contract: the sparse-package registrar, the CLSIDs the
// two shell DLLs expose, and the ONE place a menu verb becomes a command line.
//
// Why one header for three things: the manifest (Assets/Package/AppxManifest.xml),
// the Win10 IContextMenu DLL, the Win11 IExplorerCommand DLL and the installer
// all have to agree on the same CLSIDs, the same package name and the same CLI
// flags. Duplicating those strings is how the two stacks drift apart, and the
// failure mode is silent (menu item missing, or a process launched with a verb
// nobody parses). Tests/win/test_sparse_registrar.cpp asserts the manifest file
// against the constants compiled in here, and asserts the verb -> flag table.
//
// Declarations only: the C++/WinRT implementation lives in
// SparsePackageRegistrar.cpp so this header stays includable from TUs that do
// not link WindowsApp.lib (and from tests that only want the pure helpers).
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace prism::win::shell_ids {

// Win11 IExplorerCommand handler (PrismShellExt.dll). Must match
// com:Class/@Id and desktop5:Verb/@Clsid in the package manifest.
// {6F6A5E2C-3E7B-4F8A-9C1D-0A1B2C3D4E5F}
inline constexpr GUID kExplorerCommandClsid = {
    0x6F6A5E2C, 0x3E7B, 0x4F8A, {0x9C, 0x1D, 0x0A, 0x1B, 0x2C, 0x3D, 0x4E, 0x5F}};

// Win10 classic IContextMenu handler (PrismShellExtClassic.dll). Registered by
// tools/msix/register-classic.ps1 into HKCU\Software\Classes (per-user, no
// elevation, no package identity).
// {8C1B7E52-1F3A-4B7E-9C41-5B3C0000C100}
inline constexpr GUID kClassicMenuClsid = {
    0x8C1B7E52, 0x1F3A, 0x4B7E, {0x9C, 0x41, 0x5B, 0x3C, 0x00, 0x00, 0xC1, 0x00}};

inline constexpr wchar_t kPackageName[] = L"PrismArchive.ShellExt";
// Must equal Identity/@Publisher, because MSIX validates the signer's subject
// against it (CERT_E_WRONG_PRINCIPAL / 0x800B010F if they differ).
inline constexpr wchar_t kPackagePublisher[] = L"CN=PrismArchive Contributors";

inline constexpr wchar_t kShellExtDllName[] = L"PrismShellExt.dll";
inline constexpr wchar_t kClassicDllName[] = L"PrismShellExtClassic.dll";
inline constexpr wchar_t kCompanionExeName[] = L"PrismArchive.exe";

// "8-4-4-4-12" uppercase, no braces -- comparable with normalised manifest text.
std::wstring GuidToRegistryString(const GUID& id);

}  // namespace prism::win::shell_ids

namespace prism::win::shell_launch {

// The CLI flags c1/c3 agreed on. The shell DLLs never link the engine: every
// verb is handed to PrismArchive.exe as a process (blueprint ch. 7 isolation).
inline constexpr wchar_t kSmartExtract[] = L"--smart-extract";
inline constexpr wchar_t kExtractHere[] = L"--extract-here";
inline constexpr wchar_t kExtractSub[] = L"--extract-sub";
inline constexpr wchar_t kTest[] = L"--test";
inline constexpr wchar_t kSmartExtractEach[] = L"--smart-extract-each";
inline constexpr wchar_t kCompress7z[] = L"--compress-7z";
inline constexpr wchar_t kCompressZip[] = L"--compress-zip";

// Argv-safe quoting (CommandLineToArgvW rules: 2n backslashes before a quote
// become n and toggle quote state; trailing backslashes must be doubled).
std::wstring QuoteArg(const std::wstring& arg);

// verb -> "<flag> "<path>" ["<path>" ...]". Returns an EMPTY string for an
// unknown verb: a wrapper that does not understand the model must refuse, never
// launch a guess. Pure function -- unit-tested.
std::wstring ArgsForVerb(const std::string& verb,
                         const std::vector<std::wstring>& selected);

// Quote-safe command line for CreateProcessW(lpApplicationName=nullptr):
// executable first, so nothing is resolved from the caller's CWD.
std::wstring BuildCommandLine(const std::wstring& exe, const std::wstring& args);

// Find PrismArchive.exe. Order: registry InstallDir (HKCU, then HKLM) ->
// directory of this module -> %ProgramFiles%\PrismArchive -> %LOCALAPPDATA%
// \Programs\PrismArchive. Every candidate is verified with GetFileAttributesW,
// so a stale registry value cannot make us launch a nonexistent path.
bool ResolveCompanionExe(std::wstring& out);

// Resolve + launch. Returns false when the exe was not found or CreateProcessW
// failed; callers surface that as HRESULT_FROM_WIN32(GetLastError()).
bool LaunchCompanion(const std::string& verb, const std::vector<std::wstring>& selected);

}  // namespace prism::win::shell_launch

namespace prism::win::sparse {

enum class RegistrationState {
  Registered,    // PackageManager answered and our package is in the list
  NotRegistered, // answered, package absent
  Unavailable,   // could not ask at all (older OS, AppX service down, no identity)
};

enum class DeployVerdict {
  Success,           // deploy said OK *and* the package is now registered
  FailedByHresult,   // deploy itself failed
  Unverified,        // deploy claimed OK but the package is not registered
};

struct DeployOutcome {
  bool hresultOk = false;    // SUCCEEDED(DeploymentResult.ExtendedErrorCode)
  bool registeredAfter = false;  // IsRegistered() immediately after the deploy
};

// The guard that makes "registered" mean registered. The first version of this
// file returned ok=SUCCEEDED(ExtendedErrorCode) alone -- a deploy that reported
// success without leaving a package behind would have been indistinguishable
// from a real one. Kept pure so it can be mutated and tested without AppX.
DeployVerdict Evaluate(const DeployOutcome& outcome);

struct DeployResult {
  bool ok = false;
  std::int32_t hresult = 0;
  DeployVerdict verdict = DeployVerdict::FailedByHresult;
  // WinRT error text or our own explanation, never swallowed (contract B7:
  // print something the next person can act on).
  std::string message;
  std::wstring packageFullName;
};

// RFC 3986 file URI with percent-encoding. PackageManager takes a Uri, and
// Windows.Foundation.Uri will not accept a bare "C:\..." path from C++.
// Pure function -- unit-tested (spaces, unicode, UNC).
std::wstring MakeFileUri(const std::wstring& absolutePath);

struct RegistrationRequest {
  std::wstring packageUri;          // file:///.../<package>.msix
  std::wstring externalLocationUri; // file:///.../<install dir>/
  bool valid = false;
};

// Builds the AddPackageOptions payload for a sparse (external-location)
// package. valid=false when either input is not an absolute path -- registering
// with a relative external location produces a package that cannot resolve its
// own binaries, which is worse than refusing.
RegistrationRequest BuildRequest(const std::wstring& packagePath,
                                 const std::wstring& externalLocation);

RegistrationState IsRegistered(const std::wstring& packageName);
std::wstring FindPackageFullName(const std::wstring& packageName);

// AddPackageByUriAsync + AddPackageOptions{ExternalLocationUri}. Verifies the
// result with IsRegistered() before reporting success (see Evaluate).
DeployResult Deploy(const std::wstring& packagePath, const std::wstring& externalLocation);

// RemovePackageAsync on the full name. Returns false when the package is still
// there afterwards -- a delete that silently did nothing is the same bug as a
// deploy that silently did nothing.
bool Unregister(const std::wstring& packageFullName, const std::wstring& packageName);

}  // namespace prism::win::sparse
