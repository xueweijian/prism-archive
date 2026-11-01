// PrismArchive -- Windows integration test: shell double-stack registration.
//
// Card C4. Three things are proven here, in increasing cost:
//
//  1. PURE  -- the strings both stacks depend on: the verb -> CLI flag table
//     (c1/c3 alignment), argv quoting, file-URI encoding, and the deploy
//     verdict rule "ok only if the package is actually registered".
//  2. MANIFEST -- Assets/Package/AppxManifest.xml is parsed as text and
//     compared with the CLSIDs/names compiled into SparsePackageRegistrar.hpp.
//     A drift here is invisible at runtime (the verb just never appears), which
//     is why it gets a test instead of a comment. Two mutants verify the check
//     bites (D3): one drops uap10:AllowExternalContent, one swaps the CLSID.
//  3. CLASSIC -- register the real PrismShellExtClassic.dll under HKCU, ask the
//     REAL shell for the item's IContextMenu (the exact code path Explorer
//     uses), and require a "PrismArchive" submenu with >= 4 model items, then
//     unregister and require it to disappear. That disappearance is the
//     negative control: the submenu cannot come from a stale machine-wide key.
//
// The sparse round-trip (Deploy -> IsRegistered -> Unregister) needs a signed
// .msix, so it runs only when tools/msix/build-sparse.ps1 has handed one over:
//   PRISM_SPARSE_MSIX=<path to .msix>  PRISM_SPARSE_EXTERNAL=<install dir>
// Without that, the section prints SKIP: sparse:<reason> and the process exits
// 77 -- never 0. CTest maps 77 to SKIP_RETURN_CODE, so a skip is reported as a
// skip and can never be counted as a pass (contract D1).
#include <windows.h>

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "../../Src/PrismCore/ContextMenuModel.hpp"
#include "../../Src/PrismWin/PrismEngineBridge.hpp"
#include "../../Src/PrismWin/SparsePackageRegistrar.hpp"
#include "../mini_test.hpp"

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
using prism::win::shell_ids::GuidToRegistryString;
using prism::win::shell_launch::ArgsForVerb;
using prism::win::shell_launch::BuildCommandLine;
using prism::win::shell_launch::QuoteArg;
using prism::win::sparse::BuildRequest;
using prism::win::sparse::DeployOutcome;
using prism::win::sparse::DeployVerdict;
using prism::win::sparse::Evaluate;
using prism::win::sparse::MakeFileUri;

namespace {

constexpr int kSkipExitCode = 77;
std::vector<std::string> g_skips;

std::string LowerAscii(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

// Every comparison in this file is ASCII on both sides (flags, URIs, CLSIDs,
// menu titles) and mini_test's CheckEq streams its operands into an
// ostringstream, which has no std::wstring overload. Narrow once, compare strings.
std::string A(const std::wstring& text) {
  std::string out;
  out.reserve(text.size());
  for (const wchar_t c : text) out.push_back(static_cast<char>(c < 128 ? c : '?'));
  return out;
}

// Lowercase + collapse runs of whitespace to one space. Keeps attribute pairs
// contiguous without pretending to be an XML parser.
std::string Normalise(std::string text) {
  std::string out;
  out.reserve(text.size());
  bool pendingSpace = false;
  for (const char c : text) {
    const bool space = (c == ' ' || c == '\t' || c == '\r' || c == '\n');
    if (space) {
      pendingSpace = !out.empty();
      continue;
    }
    if (pendingSpace) {
      out.push_back(' ');
      pendingSpace = false;
    }
    out.push_back(static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c));
  }
  return out;
}

bool Contains(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

std::string ReadFileText(const fs::path& path) {
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) return {};
  std::string text;
  char buffer[4096];
  size_t read = 0;
  while ((read = fread(buffer, 1, sizeof(buffer), file)) > 0) text.append(buffer, read);
  fclose(file);
  return text;
}

fs::path RepoRootFromThisFile() {
  // .../Tests/win/test_sparse_registrar.cpp -> repo root
  return fs::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string Hex(unsigned long value) {
  char buffer[16]{};
  sprintf_s(buffer, "%08lX", value);
  return buffer;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. PURE layer
// ---------------------------------------------------------------------------
PRISM_TEST("verb -> cli flag table matches the c1/c3 contract") {
  const std::vector<std::wstring> one = {L"C:\\archives\\pack.7z"};
  PRISM_CHECK_EQ(A(ArgsForVerb("smart.extract.here", one)),
                 std::string("--smart-extract \"C:\\archives\\pack.7z\""));
  PRISM_CHECK_EQ(A(ArgsForVerb("extract.here", one)),
                 std::string("--extract-here \"C:\\archives\\pack.7z\""));
  PRISM_CHECK_EQ(A(ArgsForVerb("extract.to.subfolder", one)),
                 std::string("--extract-sub \"C:\\archives\\pack.7z\""));
  PRISM_CHECK_EQ(A(ArgsForVerb("test.integrity", one)),
                 std::string("--test \"C:\\archives\\pack.7z\""));
  // open.in.prism carries no flag: a bare path opens the gallery.
  PRISM_CHECK_EQ(A(ArgsForVerb("open.in.prism", one)), std::string("\"C:\\archives\\pack.7z\""));

  const std::vector<std::wstring> many = {L"C:\\a b\\one.txt", L"C:\\a b\\two.txt"};
  PRISM_CHECK_EQ(A(ArgsForVerb("compress.to.zip", many)),
                 std::string("--compress-zip \"C:\\a b\\one.txt\" \"C:\\a b\\two.txt\""));

  // A verb the view does not know must NOT produce a command line: refusing is
  // the only safe behaviour for a string that arrives from the shell.
  PRISM_CHECK(ArgsForVerb("frobnicate.everything", one).empty());
  PRISM_CHECK(ArgsForVerb("smart.extract.here", {}).empty());
}

PRISM_TEST("argv quoting survives spaces, quotes and trailing backslashes") {
  PRISM_CHECK_EQ(A(QuoteArg(L"C:\\Program Files\\PrismArchive\\PrismArchive.exe")),
                 std::string("\"C:\\Program Files\\PrismArchive\\PrismArchive.exe\""));
  // 2n backslashes before a quote collapse to n plus a literal quote.
  // A literal quote is escaped; CommandLineToArgvW turns 2n+1 backslashes before
  // a quote back into n backslashes plus the quote, so the round-trip is exact.
  PRISM_CHECK_EQ(A(QuoteArg(L"say \"hi\"")), std::string("\"say \\\"hi\\\"\""));
  // Trailing backslashes are doubled, otherwise the closing quote is escaped.
  PRISM_CHECK_EQ(A(QuoteArg(L"C:\\dir\\")), std::string("\"C:\\dir\\\\\""));
  PRISM_CHECK_EQ(A(BuildCommandLine(L"C:\\x\\PrismArchive.exe", L"--test \"p.7z\"")),
                 std::string("\"C:\\x\\PrismArchive.exe\" --test \"p.7z\""));
}

PRISM_TEST("file URI encoding for PackageManager") {
  PRISM_CHECK_EQ(A(MakeFileUri(L"C:\\build\\out\\PrismArchive.ShellExt.msix")),
                 std::string("file:///C:/build/out/PrismArchive.ShellExt.msix"));
  PRISM_CHECK_EQ(A(MakeFileUri(L"C:\\a b\\pack age.msix")),
                 std::string("file:///C:/a%20b/pack%20age.msix"));
  // Non-ASCII is percent-encoded as UTF-8 bytes, not as UTF-16 code units.
  const std::wstring unicodePath = {L'C', L':', L'\\', 0x4E2D, 0x6587, L'\\', 0x5305,
                                    L'.', L'm', L's', L'i', L'x'};
  PRISM_CHECK_EQ(A(MakeFileUri(unicodePath)),
                 std::string("file:///C:/%E4%B8%AD%E6%96%87/%E5%8C%85.msix"));
  // UNC keeps its host outside the path segment.
  PRISM_CHECK_EQ(A(MakeFileUri(L"\\\\srv\\share\\x.msix")),
                 std::string("file://srv/share/x.msix"));
  PRISM_CHECK(MakeFileUri(L"relative\\x.msix").empty());
}

PRISM_TEST("registration request refuses relative paths and pins the directory slash") {
  const auto ok = BuildRequest(L"C:\\out\\p.msix", L"C:\\Apps\\Prism");
  PRISM_CHECK(ok.valid);
  PRISM_CHECK_EQ(A(ok.packageUri), std::string("file:///C:/out/p.msix"));
  PRISM_CHECK_EQ(A(ok.externalLocationUri), std::string("file:///C:/Apps/Prism/"));

  const auto relative = BuildRequest(L"p.msix", L"C:\\Apps");
  PRISM_CHECK(!relative.valid);
  const auto relativeExternal = BuildRequest(L"C:\\out\\p.msix", L"Apps");
  PRISM_CHECK(!relativeExternal.valid);
}

PRISM_TEST("a deploy that leaves nothing registered is a failure, not a success") {
  PRISM_CHECK(Evaluate(DeployOutcome{true, true}) == DeployVerdict::Success);
  PRISM_CHECK(Evaluate(DeployOutcome{false, false}) == DeployVerdict::FailedByHresult);
  PRISM_CHECK(Evaluate(DeployOutcome{false, true}) == DeployVerdict::FailedByHresult);
  // The exact bug this guard exists for: SUCCEEDED(hresult) but no package.
  PRISM_CHECK(Evaluate(DeployOutcome{true, false}) == DeployVerdict::Unverified);
}

// ---------------------------------------------------------------------------
// 2. Manifest <-> compiled constants (cross-file drift guard)
// ---------------------------------------------------------------------------
namespace {

std::string ReplaceAll(std::string text, const std::string& from, const std::string& to) {
  if (from.empty()) return text;
  size_t at = 0;
  while ((at = text.find(from, at)) != std::string::npos) {
    text.replace(at, from.size(), to);
    at += to.size();
  }
  return text;
}

// Reads the shipping manifest and applies the compile-time mutation, if any.
// A mutant whose replacement string does not match is itself a failure: an
// injection that silently does nothing would "pass" the mutation target (D3).
std::string LoadManifestForCheck(bool& mutationApplied) {
  mutationApplied = true;
  std::string text = ReadFileText(RepoRootFromThisFile() / "Assets" / "Package" / "AppxManifest.xml");
  if (text.empty()) {
    mutationApplied = false;
    return text;
  }
#if defined(PRISM_MUTANT_MANIFEST_DROP_EXTERNAL_CONTENT)
  const std::string marker = "<uap10:AllowExternalContent>true</uap10:AllowExternalContent>";
  if (!Contains(text, marker)) {
    mutationApplied = false;
    return text;
  }
  text = ReplaceAll(text, marker, "");
#endif
#if defined(PRISM_MUTANT_MANIFEST_WRONG_CLSID)
  const std::string real = "6F6A5E2C-3E7B-4F8A-9C1D-0A1B2C3D4E5F";
  if (!Contains(text, real)) {
    mutationApplied = false;
    return text;
  }
  text = ReplaceAll(text, real, "11111111-2222-3333-4444-555555555555");
#endif
  return text;
}

void CheckManifestText(const std::string& raw, bool mutationApplied) {
  PRISM_CHECK(mutationApplied);
  const std::string text = Normalise(raw);
  PRISM_CHECK(!text.empty());
  const std::string clsid = LowerAscii(A(GuidToRegistryString(
      prism::win::shell_ids::kExplorerCommandClsid)));
  const std::string dll = LowerAscii(A(prism::win::shell_ids::kShellExtDllName));

  // Identity: the registrar cannot deploy a package whose publisher does not
  // match the signing certificate's subject (0x800B010F).
  PRISM_CHECK(Contains(text, "name=\"" + LowerAscii(A(prism::win::shell_ids::kPackageName)) + "\""));
  PRISM_CHECK(Contains(text, "publisher=\"" +
                                LowerAscii(A(prism::win::shell_ids::kPackagePublisher)) +
                                "\""));
  // Sparse package requirements.
  PRISM_CHECK(Contains(text, "<uap10:allowexternalcontent>true</uap10:allowexternalcontent>"));
  PRISM_CHECK(Contains(text, "uap10:trustlevel=\"mediumil\""));
  PRISM_CHECK(Contains(text, "uap10:runtimebehavior=\"win32app\""));
  PRISM_CHECK(Contains(text, "applistentry=\"none\""));
  // Win11 verb registration: without these three the package registers
  // successfully and the context menu simply never appears.
  PRISM_CHECK(Contains(text, "category=\"windows.comserver\""));
  PRISM_CHECK(Contains(text, "path=\"" + dll + "\""));
  PRISM_CHECK(Contains(text, "threadingmodel=\"sta\""));
  PRISM_CHECK(Contains(text, "id=\"" + clsid + "\""));
  PRISM_CHECK(Contains(text, "category=\"windows.fileexplorercontextmenus\""));
  PRISM_CHECK(Contains(text, "clsid=\"" + clsid + "\""));
  PRISM_CHECK(Contains(text, "itemtype type=\"*\""));
  // Capabilities the registrar relies on (runFullTrust for the out-of-process
  // launcher, unvirtualizedResources so the install dir stays a real path).
  PRISM_CHECK(Contains(text, "name=\"runfulltrust\""));
  PRISM_CHECK(Contains(text, "name=\"unvirtualizedresources\""));
}

}  // namespace

PRISM_TEST("package manifest matches the compiled CLSID and sparse contract") {
  bool mutationApplied = true;
  const std::string raw = LoadManifestForCheck(mutationApplied);
  CheckManifestText(raw, mutationApplied);
}

// ---------------------------------------------------------------------------
// 3. Classic IContextMenu through the real shell, per-user registration
// ---------------------------------------------------------------------------
namespace {

std::wstring BracedGuid(const GUID& id) {
  return L"{" + GuidToRegistryString(id) + L"}";
}

bool WriteString(HKEY root, const std::wstring& subkey, const wchar_t* name,
                 const std::wstring& value) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key,
                      nullptr) != ERROR_SUCCESS) {
    return false;
  }
  const DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
  const LONG status = RegSetValueExW(key, name, 0, REG_SZ,
                                     reinterpret_cast<const BYTE*>(value.c_str()), bytes);
  RegCloseKey(key);
  return status == ERROR_SUCCESS;
}

bool DeleteTree(HKEY root, const std::wstring& subkey) {
  const LONG status = RegDeleteTreeW(root, subkey.c_str());
  return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

// The archive extensions the model offers verbs for. Registering under
// SystemFileAssociations means we apply to the file TYPE regardless of which
// ProgID currently owns it -- the same shape 7-Zip and WinRAR use.
const std::wstring kArchiveExtensions[] = {L".7z", L".zip", L".rar", L".tar",
                                           L".gz", L".xz",  L".zst", L".bz2",
                                           L".cab", L".iso", L".tgz"};

std::wstring HandlerKey(const std::wstring& extension) {
  return L"Software\\Classes\\SystemFileAssociations\\" + extension +
         L"\\shellex\\ContextMenuHandlers\\PrismArchiveC4Test";
}

std::string g_classicSkip;

bool RegisterClassic(const std::wstring& dll) {
  const std::wstring clsid = BracedGuid(prism::win::shell_ids::kClassicMenuClsid);
  const std::wstring base = L"Software\\Classes\\CLSID\\" + clsid;
  if (!WriteString(HKEY_CURRENT_USER, base + L"\\InprocServer32", nullptr, dll)) {
    g_classicSkip = "classic:hkcu-write-denied";
    return false;
  }
  if (!WriteString(HKEY_CURRENT_USER, base + L"\\InprocServer32", L"ThreadingModel",
                   L"Apartment")) {
    g_classicSkip = "classic:hkcu-write-denied";
    return false;
  }
  for (const std::wstring& extension : kArchiveExtensions) {
    if (!WriteString(HKEY_CURRENT_USER, HandlerKey(extension), nullptr, clsid)) {
      g_classicSkip = "classic:hkcu-write-denied";
      return false;
    }
  }
  return true;
}

void UnregisterClassic() {
  const std::wstring clsid = BracedGuid(prism::win::shell_ids::kClassicMenuClsid);
  DeleteTree(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\" + clsid);
  for (const std::wstring& extension : kArchiveExtensions) {
    DeleteTree(HKEY_CURRENT_USER, HandlerKey(extension));
  }
}

// An IDataObject that carries exactly one path as CF_HDROP -- what Explorer
// hands IShellExtInit::Initialize. Small on purpose: this is the minimum needed
// to drive the REAL exported handler through its real entry points.
class DropDataObject final : public IDataObject {
 public:
  explicit DropDataObject(std::wstring path) : path_(std::move(path)) {}

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDataObject) {
      *ppv = static_cast<IDataObject*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
  ULONG STDMETHODCALLTYPE Release() override {
    const ULONG left = InterlockedDecrement(&refs_);
    if (left == 0) delete this;
    return left;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC* format, STGMEDIUM* medium) override {
    if (!format || !medium) return E_POINTER;
    if (format->cfFormat != CF_HDROP) return DV_E_FORMATETC;
    const SIZE_T bytes = sizeof(DROPFILES) + (path_.size() + 2) * sizeof(wchar_t);
    HGLOBAL shared = GlobalAlloc(GHND | GMEM_SHARE, bytes);
    if (!shared) return E_OUTOFMEMORY;
    auto* drop = static_cast<DROPFILES*>(GlobalLock(shared));
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES), path_.c_str(),
                (path_.size() + 1) * sizeof(wchar_t));
    const wchar_t terminator = L'\0';
    std::memcpy(reinterpret_cast<BYTE*>(drop) + sizeof(DROPFILES) +
                    (path_.size() + 1) * sizeof(wchar_t),
                &terminator, sizeof(wchar_t));
    GlobalUnlock(shared);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = shared;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* format) override {
    if (!format) return E_POINTER;
    return format->cfFormat == CF_HDROP ? S_OK : DV_E_FORMATETC;
  }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
    if (out) out->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC*, STGMEDIUM*, BOOL) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD, IEnumFORMATETC** out) override {
    if (out) *out = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA** out) override {
    if (out) *out = nullptr;
    return OLE_E_ADVISENOTSUPPORTED;
  }

 private:
  LONG refs_ = 1;
  std::wstring path_;
};

// Walks the menu the SHELL built for this file -- the same code path Explorer
// uses -- and returns our submenu's item titles.
bool CollectPrismSubmenu(const std::wstring& file, std::vector<std::wstring>& titles) {
  titles.clear();
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninit = SUCCEEDED(coInit);

  PIDLIST_ABSOLUTE pidl = nullptr;
  IShellFolder* parent = nullptr;
  PCUITEMID_CHILD child = nullptr;
  IContextMenu* contextMenu = nullptr;
  bool found = false;

  if (SUCCEEDED(SHParseDisplayName(file.c_str(), nullptr, &pidl, 0, nullptr)) &&
      SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&parent), &child)) &&
      SUCCEEDED(parent->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                      reinterpret_cast<void**>(&contextMenu)))) {
    HMENU menu = CreatePopupMenu();
    if (menu && SUCCEEDED(contextMenu->QueryContextMenu(menu, 0, 1, 0x7FFE, CMF_NORMAL))) {
      const int count = GetMenuItemCount(menu);
      for (int i = 0; i < count && !found; ++i) {
        wchar_t label[256]{};
        if (GetMenuStringW(menu, static_cast<UINT>(i), label, ARRAYSIZE(label),
                           MF_BYPOSITION) == 0) {
          continue;
        }
        if (std::wstring(label) != L"PrismArchive") continue;
        HMENU submenu = GetSubMenu(menu, i);
        if (!submenu) continue;
        found = true;
        const int subCount = GetMenuItemCount(submenu);
        for (int j = 0; j < subCount; ++j) {
          wchar_t title[512]{};
          GetMenuStringW(submenu, static_cast<UINT>(j), title, ARRAYSIZE(title),
                         MF_BYPOSITION);
          titles.emplace_back(title);
        }
      }
    }
    if (menu) DestroyMenu(menu);
  }

  if (contextMenu) contextMenu->Release();
  if (parent) parent->Release();
  if (pidl) CoTaskMemFree(pidl);
  if (uninit) CoUninitialize();
  return found;
}

// The merged menu above proves the shell REGISTERS us. It cannot prove the
// command IDs line up, because the shell renumbers every handler's range. So the
// verb contract is checked on the handler we export, through CoCreateInstance:
// DllGetClassObject -> CLSID accepted -> IShellExtInit parses CF_HDROP ->
// QueryContextMenu consumes exactly model.size() ids -> GetCommandString(id)
// returns the model verb. That is also the QI-reachability check (contract B8):
// the shell asks for IShellExtInit and IContextMenu separately.
void CheckDirectComHandler(const std::wstring& file,
                           const std::vector<prism::MenuCommand>& model) {
  const HRESULT coInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninit = SUCCEEDED(coInit);

  ComPtr<IContextMenu> handler;
  const HRESULT created = CoCreateInstance(prism::win::shell_ids::kClassicMenuClsid,
                                           nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&handler));
  PRISM_CHECK(SUCCEEDED(created));
  if (SUCCEEDED(created)) {
    ComPtr<IShellExtInit> init;
    PRISM_CHECK(SUCCEEDED(handler.As(&init)));
    ComPtr<DropDataObject> data;
    data.Attach(new DropDataObject(file));
    PRISM_CHECK(SUCCEEDED(init->Initialize(nullptr, data.Get(), nullptr)));

    HMENU menu = CreatePopupMenu();
    const HRESULT queried = handler->QueryContextMenu(menu, 0, 1, 0x7FFE, CMF_NORMAL);
    PRISM_CHECK(SUCCEEDED(queried));
    PRISM_CHECK_EQ(static_cast<size_t>(HRESULT_CODE(queried)), model.size());
    HMENU submenu = GetSubMenu(menu, 0);
    PRISM_CHECK(submenu != nullptr);
    if (submenu) {
      PRISM_CHECK_EQ(static_cast<size_t>(GetMenuItemCount(submenu)), model.size());
      for (size_t j = 0; j < model.size(); ++j) {
        wchar_t verb[256]{};
        // IContextMenu::GetCommandString's idCmd is the ZERO-BASED OFFSET into
        // the range QueryContextMenu returned, not the absolute command id.
        const HRESULT asked = handler->GetCommandString(
            static_cast<UINT_PTR>(j), GCS_VERBW, nullptr,
            reinterpret_cast<LPSTR>(verb), sizeof(verb));
        PRISM_CHECK(SUCCEEDED(asked));
        PRISM_CHECK_EQ(A(verb), model[j].verb);
      }
      // The bold/primary item must be the one the model marks default.
      size_t defaultIndex = model.size();
      for (size_t j = 0; j < model.size(); ++j) {
        if (model[j].isDefault) {
          defaultIndex = j;
          break;
        }
      }
      if (defaultIndex < model.size()) {
        PRISM_CHECK_EQ(static_cast<size_t>(GetMenuDefaultItem(submenu, FALSE, 0 /* GMF_BYCOMMAND */)),
                       defaultIndex + 1);
      }
      // An offset outside the model must be refused, not silently mapped.
      wchar_t verb[8]{};
      PRISM_CHECK(FAILED(handler->GetCommandString(
          static_cast<UINT_PTR>(model.size()), GCS_VERBW, nullptr,
          reinterpret_cast<LPSTR>(verb), sizeof(verb))));
    }
    if (menu) DestroyMenu(menu);
  }
  if (uninit) CoUninitialize();
}
}  // namespace

void RunClassicSection(const std::wstring& classicDll) {
  if (classicDll.empty() || GetFileAttributesW(classicDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
    g_skips.push_back("classic:PrismShellExtClassic.dll not supplied");
    return;
  }
  if (!RegisterClassic(classicDll)) {
    g_skips.push_back(g_classicSkip.empty() ? "classic:registration-failed" : g_classicSkip);
    return;
  }

  // A real archive-named file: the model decides by extension, the shell asks
  // by item, exactly like a user right-click.
  wchar_t tempRoot[MAX_PATH * 2]{};
  GetTempPathW(ARRAYSIZE(tempRoot), tempRoot);
  const fs::path sample =
      fs::path(tempRoot) / L"prism-c4-shell" / L"prism-c4-sample.7z";
  std::error_code ec;
  fs::create_directories(sample.parent_path(), ec);
  FILE* file = nullptr;
  if (_wfopen_s(&file, sample.c_str(), L"wb") == 0 && file) fclose(file);

  // The direct-COM checks below do not need a running shell; the merged-menu
  // check does (the shell aggregates SystemFileAssociations handlers itself).
  // A Windows Server CI runner has no Explorer, so that part is a precise SKIP
  // there instead of a false red -- and a false green is impossible because the
  // marker/pass regex never prints on the skip path.
  const bool interactiveShell = ::GetShellWindow() != nullptr;

  std::vector<std::wstring> titles;
  const bool visible = CollectPrismSubmenu(sample.wstring(), titles);
  if (interactiveShell) {
    PRISM_CHECK(visible);
  }

  // Expected = the model itself. If the view ever re-orders, re-labels or drops
  // a verb, this comparison fails instead of quietly shipping a different menu.
  const auto model = prism::ContextMenuModel::Build(
      {prism::win::WideToUtf8(sample.wstring())});
  PRISM_CHECK(model.size() >= 4);
  if (interactiveShell) {
    PRISM_CHECK_EQ(titles.size(), model.size());
    for (size_t i = 0; i < model.size() && i < titles.size(); ++i) {
      PRISM_CHECK_EQ(A(titles[i]), model[i].title);
    }
  }

  // Command ids and verbs are checked on our exported handler (the shell
  // renumbers merged ranges, so the merged menu cannot answer this).
  CheckDirectComHandler(sample.wstring(), model);

  // Negative control: once the registration is gone the submenu must be gone.
  // That is what proves the items above came from THIS registration.
  UnregisterClassic();
  titles.clear();
  PRISM_CHECK(!CollectPrismSubmenu(sample.wstring(), titles));

  if (!interactiveShell) {
    g_skips.push_back(
        "classic:no-interactive-shell (Explorer is not running, so the merged "
        "IContextMenu path cannot be exercised; the exported handler was still "
        "created through CoCreateInstance and its menu/verbs were asserted)");
  }

  std::error_code ignored;
  fs::remove(sample, ignored);
  fs::remove(sample.parent_path(), ignored);
}

// ---------------------------------------------------------------------------
// 4. Sparse package round-trip (needs a signed .msix produced by
//    tools/msix/build-sparse.ps1; skipped with a machine-readable reason
//    otherwise, and a skip is never a pass -- contract D1)
// ---------------------------------------------------------------------------
namespace {

std::wstring EnvOr(const wchar_t* name) {
  const wchar_t* value = _wgetenv(name);
  return value ? std::wstring(value) : std::wstring();
}

fs::path BuiltDll(const wchar_t* name) {
  std::vector<fs::path> candidates;
#if defined(PRISM_BUILD_ROOT)
  candidates.emplace_back(fs::path(L"" PRISM_BUILD_ROOT) / L"Src" / L"PrismWin" / name);
  candidates.emplace_back(fs::path(L"" PRISM_BUILD_ROOT) / name);
#endif
  wchar_t module[MAX_PATH * 2]{};
  if (GetModuleFileNameW(nullptr, module, ARRAYSIZE(module)) > 0) {
    const fs::path here(module);
    candidates.push_back(here.parent_path() / name);
    candidates.push_back(here.parent_path().parent_path() / L"Src" / L"PrismWin" / name);
  }
  for (const fs::path& candidate : candidates) {
    std::error_code ec;
    if (fs::is_regular_file(candidate, ec)) return candidate;
  }
  return {};
}

// A closed set of HRESULTs that mean "this machine's trust/policy state refused
// the package", not "our code is wrong". They are classified as SKIP (exit 77),
// never as a pass -- the deploy path WAS exercised and the platform said no.
// Everything outside this list stays a hard FAIL.
bool IsEnvironmentTrustRefusal(std::int32_t hresult) {
  switch (static_cast<std::uint32_t>(hresult)) {
    case 0x800B0100u:  // TRUST_E_NOSIGNATURE   -- package is not signed
    case 0x800B0101u:  // CERT_E_EXPIRED
    case 0x800B0109u:  // CERT_E_UNTRUSTEDROOT  -- observed on a non-admin box
    case 0x800B010Au:  // CERT_E_CHAINING
    case 0x800B0110u:  // CERT_E_WRONG_USAGE
    case 0x80073CFFu:  // developer mode / sideloading policy
      return true;
    default:
      return false;
  }
}

void RunSparseSection() {
  using namespace prism::win::sparse;
  const std::wstring packagePath = EnvOr(L"PRISM_SPARSE_MSIX");
  const std::wstring external = EnvOr(L"PRISM_SPARSE_EXTERNAL");
  if (packagePath.empty() || external.empty()) {
    g_skips.push_back(
        "sparse:no-signed-package (run tools/msix/build-sparse.ps1 -Sign, then set "
        "PRISM_SPARSE_MSIX + PRISM_SPARSE_EXTERNAL)");
    return;
  }

  const std::wstring& name = prism::win::shell_ids::kPackageName;
  // Start from a known-clean state so "registered" cannot be a leftover.
  if (IsRegistered(name) == RegistrationState::Registered) {
    Unregister(FindPackageFullName(name), name);
  }

  const DeployResult deploy = Deploy(packagePath, external);
  if (!deploy.ok) {
    std::printf("    DEPLOY FAILED  hr=0x%08X verdict=%d  %s\n", deploy.hresult,
                static_cast<int>(deploy.verdict), deploy.message.c_str());
    if (IsEnvironmentTrustRefusal(deploy.hresult)) {
      char reason[320]{};
      sprintf_s(reason,
                "sparse:package-not-trusted hr=0x%08X (deploy path exercised; the "
                "certificate must be trusted in a ROOT store -- CurrentUser\Root "
                "import needs an interactive consent dialog, LocalMachine\Root "
                "needs admin; neither exists in a headless non-admin session)",
                deploy.hresult);
      g_skips.push_back(reason);
      return;
    }
  }
  PRISM_CHECK(deploy.ok);
  PRISM_CHECK(IsRegistered(name) == RegistrationState::Registered);
  PRISM_CHECK(!deploy.packageFullName.empty());

  // Idempotent re-registration is what an installer upgrade does.
  const DeployResult again = Deploy(packagePath, external);
  if (!again.ok) {
    std::printf("    RE-DEPLOY FAILED  hr=0x%08X  %s\n", again.hresult,
                again.message.c_str());
  }
  PRISM_CHECK(again.ok);

  const std::wstring fullName = FindPackageFullName(name);
  PRISM_CHECK(!fullName.empty());
  PRISM_CHECK(Unregister(fullName, name));
  PRISM_CHECK(IsRegistered(name) != RegistrationState::Registered);
}

}  // namespace

int main(int argc, char** argv) {
  const int caseFailures = prism::test::RunAll(argc, argv);

#if !defined(PRISM_TEST_PURE_ONLY)
  const int beforeIntegration = prism::test::FailureCount();
  RunClassicSection(BuiltDll(prism::win::shell_ids::kClassicDllName).wstring());
  RunSparseSection();
  const bool integrationFailed = prism::test::FailureCount() != beforeIntegration;
#else
  const bool integrationFailed = false;
#endif

  if (caseFailures != 0 || integrationFailed || prism::test::FailureCount() != 0) {
    std::printf("FAILED: sparse-registrar checks red (see FAIL lines above)\n");
    return 1;
  }
  if (!g_skips.empty()) {
    // A skip must be machine-readable AND fatal to "green": exit 77 is mapped
    // to CTest's SKIP_RETURN_CODE, so ctest reports Skipped, never Passed.
    for (const std::string& reason : g_skips) {
      std::printf("SKIP: %s\n", reason.c_str());
    }
    return kSkipExitCode;
  }
  std::printf("ALL SPARSE-REGISTRAR CHECKS PASSED\n");
  return 0;
}