// PrismArchive -- PrismWin
// The gallery window (blueprint ch. 4.3 + ch. 5): a Direct2D host that owns
// the HWND, the swap of user events into PrismCore decisions, and NOTHING
// else. Every pixel is painted by GalleryRenderer from a Scene snapshot --
// the window stopped drawing tiles itself when the renderer was adopted
// (batch A): two paint paths for one model is how drift starts.
//
// Batch A increment 5 -- folder navigation. The window NO LONGER decides what
// "a folder" is: `prism::tree::Navigator` (PrismCore, Linux-tested) owns the
// current folder, the filtered list, the breadcrumb and the selection. This
// file maps pixels -> Navigator calls -> execution:
//   * double-click / Enter on a folder  -> Enter()  (drill in, viewport to top)
//   * Backspace                          -> Back()   (up one level, no-op at root)
//   * click on a breadcrumb crumb        -> GoToLevel()
//   * double-click / Enter on a file     -> the app's activate handler with the
//                                           CATALOG index (smart extract, batch A)
// The clickable geometry of the breadcrumb is computed by PrismCore from the
// SAME segment list that is painted, with text widths measured by DirectWrite
// (GetTextExtent-style guessing drifts from what is drawn).
//
// Threading contract for thumbnails: the sweep worker builds its OWN
// ThumbStore and hands it over via SetThumbs (pointer swap under a mutex).
// Only the UI thread ever calls Lookup (it mutates LRU recency), and it only
// ever touches the swapped-in store -- the worker never writes a store the
// UI can see.
//
// Thumbnail SCOPE after navigation: the sweep still runs ONCE over the whole
// catalog (solid blocks must be read in one pass), and the store is keyed by
// CATALOG index. The navigator keeps catalog indices in the per-folder list
// (ArchiveTreeView.hpp "the folder list keeps catalog ids"), so switching
// folders re-filters rows and never renumbers them: zero rescans, and a
// thumbnail harvested before the switch is still a hit afterwards.
//
// Card C34 added the two pieces of LIFETIME this window owns and nothing else:
//   1. THE GLASS LAYER (C33's GlassIslandHost). A composition stack plus one
//      swapchain per island is memory that a minimised window cannot use, and
//      ReleaseTransient() is the only path that gives it back (C33 measured:
//      without IDXGIDevice3::Trim not one byte returns). WM_SIZE(SIZE_MINIMIZED)
//      releases it, the restore path rebuilds it, and the island is the NAV
//      PILL's own rect -- never the window's (C33 finding 1).
//   2. THE IDLE PUMP. The app's inline-preview companion (C30) is woken by an
//      auto-reset event, and this window owns the message loop, so it offers a
//      timer tick the app can poll. Nothing in the window knows what the handler
//      does: the pump is a seam, not a policy.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX  // windows.h must not own min/max: PrismCore uses std::min/std::max
#endif
#include <windows.h>
// C52: HDROP and the Drag* family live in shellapi.h, which windows.h SKIPS
// under the WIN32_LEAN_AND_MEAN defined above. A header that pulls windows.h in
// itself must therefore name the shell header, or every TU that includes this
// file first fails on "HDROP: undeclared" (the drop-in suite only compiled
// because it happened to include windows.h without the lean-and-mean).
#include <shellapi.h>
#include <imm.h>  // C62: IME composition preview for the search field

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <cassert>
#include <cstdint>
#include <memory>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <mutex>
#include <ostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "../PrismCore/ActionBarModel.hpp"  // C-V2: action table + enable rules
#include "../PrismCore/ArchiveTreeView.hpp"
#include "../PrismCore/CommandPalette.hpp"  // palette ranker (C8 decision layer)
#include "../PrismCore/DropPlan.hpp"       // C52: what a drop ONTO the window means
#include "../PrismCore/FilterModel.hpp"     // list view filter (C13)
#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/HistoryStack.hpp"    // Alt+Left / Alt+Right (C8)
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/ListSortModel.hpp"   // list view order (C13)
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/SearchBoxModel.hpp"  // C62: the search field's state machine
#include "../PrismCore/SelectionModel.hpp"  // C67: the ordered multi-selection
#include "../PrismCore/Shortcuts.hpp"       // the ONE key table (C8)
#include "../PrismCore/SidebarModel.hpp"    // sidebar state (C12)
#include "../PrismCore/SpringAnimator.hpp"
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismCore/TabSet.hpp"      // C59: several archives, one window
#include "../PrismCore/WindowLayout.hpp"  // V1: the ONE geometry authority
#include "AboutDialog.hpp"                  // C57: Help > About (one menu item, one modal)
#include "CommandPalette.hpp"               // palette shell: geometry + text buffer
#include "EditConfirmDialog.hpp"            // C54: edit intents, confirmation model, gate
#include "PaletteOverlay.hpp"               // C53: Ctrl+K paints THIS layer (C31)
#include "SearchBoxPainter.hpp"              // C62: C15-tokened drawing instructions
#include "GalleryRenderer.hpp"
#include "GlassIslandHost.hpp"              // C34: the composition/glass layer (C33)
#include "SettingsDialog.hpp"               // C53: the Settings... menu item (C31/V3)
#include "SidebarView.hpp"                  // sidebar draw commands (C12)
#include "WindowFrame.hpp"                  // C66: visible frame, work area, clamp
#include "TabBar.hpp"                        // C60: the tab strip (pixels + hits)

// C34: the glass layer is composition, so the window's TU needs the three
// import libraries CreateSwapChainForComposition / D3D11 device creation pull
// in. Naming them here keeps every consumer (the exe and each suite that
// includes this header) linkable without editing CMakeLists per target --
// the same device the header relies on is the device it names.
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
// C52: DragAcceptFiles / DragQueryFileW / DragQueryPoint / DragFinish live in
// shell32. Naming the library here keeps every consumer of this header (the exe
// and each suite that creates a real window) linkable without per-target edits
// -- the same reason the three D3D/DComp libraries are named above.
#pragma comment(lib, "shell32.lib")
// C62: ImmGetContext / ImmGetCompositionStringW (IME preview).
#pragma comment(lib, "imm32.lib")

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

// C53: the four surfaces that can own the user's attention at the same time --
// the drop hint (C52), the command palette (C31/C8), Quick Look and the settings
// dialog (C31/V3). Exactly ONE may be active. The enum is the single vocabulary
// for that rule, so a future overlay cannot invent a fifth answer.
enum class ActiveOverlay : std::uint8_t {
  None = 0,
  DropHint,
  Palette,
  QuickLook,
  Settings
};

inline const char* ActiveOverlayName(ActiveOverlay overlay) {
  switch (overlay) {
    case ActiveOverlay::None: return "None";
    case ActiveOverlay::DropHint: return "DropHint";
    case ActiveOverlay::Palette: return "Palette";
    case ActiveOverlay::QuickLook: return "QuickLook";
    case ActiveOverlay::Settings: return "Settings";
  }
  return "?";
}

inline std::ostream& operator<<(std::ostream& os, ActiveOverlay overlay) {
  return os << ActiveOverlayName(overlay);
}

#if defined(PRISM_C53_MUTANT_MULTI_OVERLAY)
// MUTANT ACTIVE (D28: the banner sits inside the guard): the exclusivity rule
// is compiled out, so the palette and the drop hint can be up together.
inline constexpr bool kOverlayExclusivityEnforced = false;
#else
inline constexpr bool kOverlayExclusivityEnforced = true;
#endif

class GalleryWindow {
 public:
  // Monotonic millisecond clock -- public because Main.cpp drives HUD phases
  // on the SAME timeline as the window's internal animation timing.
  static uint64_t NowMs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
  }
  // Fixed scroll animation: the blueprint's "panel smooth transition" preset.
  static constexpr SpringPreset kScrollSpring = kSpringFluidSheet;
  // Posted by the sweep worker when a fresh ThumbStore has been swapped in.
  static constexpr UINT kThumbsChanged = WM_APP + 0x1;
  // Progress: the worker posts kProgressMessage (wparam=done, lparam=total
  // bytes); the UI thread feeds the HUD -- single-threaded by rule.
  static constexpr UINT kProgressMessage = WM_APP + 2;
  // Worker -> UI hand-off for the two pieces of state a worker used to touch
  // directly (see PostHudStart): LPARAM is an owned std::string* (HUD headline,
  // UTF-8) or std::wstring* (status line, wide) that WndProc takes by value and
  // deletes. A failed PostMessage deletes it at the source.
  static constexpr UINT kHudStart = WM_APP + 3;
  static constexpr UINT kStatusText = WM_APP + 4;

  // The window owns an HWND and a device-dependent renderer; both are released
  // in the destructor, in that order. Main.cpp's `window` is a stack object, so
  // this runs on the UI thread after Run() returned and after every worker was
  // joined -- the only point where the HWND and the renderer are guaranteed to
  // be quiescent.
  ~GalleryWindow() { DestroyHostWindow(); }

  struct Host {
    HWND hwnd = nullptr;
    std::vector<prism::ArchiveItem>* items = nullptr;  // CURRENT FOLDER rows (not owned)
  };

  bool Create(const std::wstring& title, HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = &GalleryWindow::ProcThunk;
    wc.hInstance = instance;
    // IDC_ARROW expands to MAKEINTRESOURCEA in a non-UNICODE TU; name the wide
    // integer directly (32512) so the call is ASCII/Unicode-proof.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"PrismGalleryWindow";
    RegisterClassExW(&wc);
    EnsureTextMeasurement();  // breadcrumb widths exist before the first layout

    host_.hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                 WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                 1280, 800, nullptr, nullptr, instance, this);
    if (!host_.hwnd) return false;
    // C52: register the window as a drop target. DragAcceptFiles is the cheap
    // half of OLE drag & drop (it sets WS_EX_ACCEPTFILES and turns Explorer's
    // drop into a WM_DROPFILES); IDropTarget would buy us modifier keys and
    // per-pixel hover we cannot use yet, at the cost of a COM object whose
    // lifetime has to survive every test that owns a window. The message is
    // enough for the decision, and the decision is what this card is about.
    ::DragAcceptFiles(host_.hwnd, TRUE);
    // The renderer's device-independent resources (D2D factory + the DirectWrite
    // factory its CreateTextFormat calls go through) must exist BEFORE the first
    // WM_PAINT. Skipping this left renderer_.writeFactory_ null, so the very
    // first paint called a virtual function through a null vtable slot: an
    // access violation inside DispatchMessageW, after which the UI thread never
    // returned to GetMessageW, ignored the WM_QUIT that closing the window had
    // posted, and the process hung forever with no window. That was the whole
    // "closing does not exit" bug (card C1, 2026-11-01).
    rendererInitialized_ = SUCCEEDED(renderer_.Initialize());
    // The bar exists from the first frame: a window without a menu, later
    // growing one, is a visible flicker AND a different client rect, which would
    // move every tile after the user opened it.
    RebuildMenu();
    return rendererInitialized_ && EnsureDeviceResources();
  }

  // Attach the archive catalog (the app owns the snapshot; it outlives the
  // window). The navigator resets to the archive root.
  //
  // The SAME snapshot also feeds the two C12/C13 authorities: the sidebar model
  // (which builds the archive tree and the telemetry totals from it) and the
  // palette ranker (which searches the whole archive, not just this folder).
  // One catalog, three readers -- never a second copy that could drift.
  void SetItems(std::vector<prism::ArchiveItem>* items) {
    catalog_ = items;
    archiveOpen_ = true;  // C52: a catalog means an archive is open (see SetArchiveOpen)
    nav_.Attach(items);
    selection_ = nav_.Selection();
    sidebar_.AttachCatalog(items);
    sidebar_.SetArchiveName(WideToUtf8(rootLabel_));
    sidebarDirty_ = true;
    palette_.SetCommands(prism::palette::DefaultCommands());
    if (catalog_) palette_.SetCatalog(*catalog_);
    history_.Clear();
    history_.Visit(nav_.Folder());  // the root is a real level (de-duped to no-op)
    RebuildFolderView();
    SyncActionContext();  // a newly attached catalog makes the band legal
  }

  // Cache-policy handle kept for the compile contract (m3_compile_check); the
  // live pixel store arrives via SetThumbs.
  prism::gallery::ThumbnailLru& Thumbnails() { return thumbnails_; }
  bool ListMode() const { return listMode_; }  // Ctrl+1/2 toggles (ch. 9.1)
  prism::gallery::ViewportWindow& Viewport() { return viewport_; }
  HWND Hwnd() const { return host_.hwnd; }

  // ... and what the UI thread currently shows.
  const std::string& Folder() const { return nav_.Folder(); }
  const std::vector<prism::ArchiveItem>& Items() const { return folderItems_; }
  int ItemCount() const { return static_cast<int>(folderItems_.size()); }
  int Selection() const { return selection_; }
  const std::wstring& BreadcrumbText() const { return breadcrumbText_; }
  // The crumb chain the bar currently offers (empty at the archive root).
  const std::vector<prism::tree::Segment>& Crumbs() const { return crumbSegments_; }
  int CrumbLevelCount() const { return static_cast<int>(nav_.Current().breadcrumb.size()); }
  bool QuickLookOpen() const {
    return quickLook_ &&
           quickLook_->state() != prism::gallery::QuickLookSession::State::Closed;
  }
  double ScrollOffset() const { return viewport_.Scroll(); }

  // Ownership handoff from the sweep worker (see threading contract above).
  void SetThumbs(std::shared_ptr<prism::gallery::ThumbStore> store) {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    thumbs_ = std::move(store);
  }

  // The label of the archive root crumb ("PrismArchive.7z"). The app owns the
  // name; the window only paints it (and it is clickable as the root level).
  void SetRootLabel(const std::wstring& label) {
    rootLabel_ = label;
    sidebar_.SetArchiveName(WideToUtf8(label));
    sidebarDirty_ = true;
    RebuildBreadcrumbText();
    Invalidate();
  }

  // Transient status line (extraction result, item count) appended after the
  // crumbs. It is NOT clickable -- only the crumbs are.
  void SetStatus(const std::wstring& text) {
    if (status_ == text) return;
    status_ = text;
    RebuildBreadcrumbText();
    Invalidate();
  }
  // Kept for the batch-A call sites / tests that only want a status line.
  void SetBreadcrumb(const std::wstring& text) { SetStatus(text); }

  // ---------- interaction (batch A increment 2) ----------
  // Double-click activates an item; the app decides what that means (v1:
  // smart-extract the whole archive). The window only maps pixels -> index.
  // The index is the CATALOG index (stable across folder navigation), not the
  // row in the current folder.
  void SetActivateHandler(std::function<void(int)> handler) {
    onActivate_ = std::move(handler);
  }
  // ---- card C67: Alt+double-click, multi-select, drag out ------------------
  //
  // "Alt+double-click extracts, plain double-click opens" is PrismCore's
  // decision (prism::keys::ResolvePointer) and has been since C8; what was
  // missing was this call site. The index handed over is the CATALOG index, the
  // same door Ctrl+E and Enter use, so the app owns what "extract" means.
  void SetSmartExtractHandler(std::function<void(int)> handler) {
    onSmartExtract_ = std::move(handler);
  }
  // A headless window station has no keyboard: the suite injects the modifier
  // state here. Production reads the thread's REAL key state.
  void SetAltDownProbe(std::function<bool()> probe) { altDownProbe_ = std::move(probe); }
  bool AltDown() const {
    return altDownProbe_ ? altDownProbe_() : ((::GetKeyState(VK_MENU) & 0x8000) != 0);
  }
  // D3: the one place the Alt chord can be switched off, so the mutant build
  // proves the injection compiled in (the verdict's predicate asks THIS).
  static constexpr bool AltChordEnabled() {
#if defined(PRISM_C67_MUTANT_IGNORE_ALT)
    return false;
#else
    return true;
#endif
  }
  // The selection the user can SEE: ascending, duplicate-free rows of the
  // CURRENT folder, plus the catalog indices the app batches over.
  bool HasSelection() const { return !sel_.Empty(); }
  int SelectionCount() const { return static_cast<int>(sel_.Size()); }
  const std::vector<int>& SelectedRows() const { return sel_.Selected(); }
  std::vector<uint32_t> SelectedCatalogIndices() const {
    std::vector<uint32_t> out;
    out.reserve(sel_.Size());
    for (int row : sel_.Selected()) {
      if (row >= 0 && row < ItemCount()) {
        out.push_back(folderItems_[static_cast<std::size_t>(row)].index);
      }
    }
    return out;
  }
  // Dragging OUT: the app owns the extraction (it has the engine and the
  // archive path); the window owns WHEN -- a press that moved far enough to be
  // a drag, over a selection. Returns true when the drag ran (the call blocks
  // until the user lets go or cancels).
  void SetDragOutHandler(std::function<bool(HWND, const std::vector<uint32_t>&)> handler) {
    onDragOut_ = std::move(handler);
  }
  void SetSelection(int index) {
    nav_.SetSelection(index);
    ApplySelection(nav_.Selection());
  }

  // ---------- card C25 assembly seams ---------------------------------------
  // The key route. WM_KEYDOWN builds a Stroke from the thread's real modifier
  // state and calls HandleKey; this door exists because a headless window
  // station cannot always synthesise a chord, and because a test that
  // re-implements the routing would prove nothing about the window.
  LRESULT DispatchKeyStroke(const prism::keys::Stroke& stroke) { return HandleKey(stroke); }
  bool PaletteOpen() const { return palette_.IsOpen(); }
  const prism::win::palette::PaletteShell& Palette() const { return palette_; }

  // ---------- card C53: ONE modal overlay at a time -------------------------
  // The window owns three of the four surfaces itself; the settings dialog is
  // hosted by the app, so it reports itself through SetSettingsLauncher's gate
  // (settingsModalOpen_ is true only while that gate is running).
  ActiveOverlay CurrentOverlay() const {
    if (settingsModalOpen_) return ActiveOverlay::Settings;
    if (palette_.IsOpen()) return ActiveOverlay::Palette;
    if (dropHintVisible_) return ActiveOverlay::DropHint;
    if (QuickLookOpen()) return ActiveOverlay::QuickLook;
    return ActiveOverlay::None;
  }
  int ActiveOverlayCount() const {
    int count = 0;
    if (settingsModalOpen_) ++count;
    if (palette_.IsOpen()) ++count;
    if (dropHintVisible_) ++count;
    if (QuickLookOpen()) ++count;
    return count;
  }
  // The invariant the window itself enforces. NDEBUG removes assert(), so the
  // suite reads the counter back (a Release build must not hide a logic error).
  bool SingleOverlayInvariant() const { return ActiveOverlayCount() <= 1; }
  int OverlayInvariantViolations() const { return overlayViolations_; }
  bool SettingsModalOpen() const { return settingsModalOpen_; }
  const prism::win::palette::PaletteOverlay& Overlay() const { return overlay_; }
  bool PaletteOverlayOpen() const { return overlay_.IsOpen(); }
  const std::wstring& StatusText() const { return status_; }
  // The app supplies the launcher so it can load the document from ITS paths
  // (a portable build does not use %LOCALAPPDATA%); tests use the same seam to
  // drive the menu command without entering an unbounded modal loop.
  void SetSettingsLauncher(std::function<void()> launcher) {
    settingsLauncher_ = std::move(launcher);
  }
  size_t HistoryBackCount() const { return history_.BackCount(); }
  size_t HistoryForwardCount() const { return history_.ForwardCount(); }

  // C12: the sidebar is a Model (state) + a Layout (draw commands). The window
  // owns both; painting them is the renderer's job and is NOT wired yet -- see
  // the TODO in AppWiring.hpp / the PR (GalleryRenderer.hpp is another card's
  // file). Everything up to the command list is live and assertable.
  const prism::sidebar::Model& SidebarState() const { return sidebar_; }
  const prism::win::sidebar::Layout& SidebarLayout() const {
    EnsureSidebar();
    return sidebarLayout_;
  }
  // C29: pixels the list content is inset by right now (0 when the rail is
  // folded, off, or the window is too narrow). One formula, used by the paint
  // path and by hit-testing, so the two can never disagree.
  float SidebarInset() const {
    if (!sidebarRailVisible_) return 0.0f;
    return GalleryRenderer::SidebarInset(static_cast<float>(clientW_));
  }

  // V4: the painted rectangle of one item in grid mode (scroll 0). The click
  // path uses this same flow, so a test that wants to click an item asks for
  // its rectangle instead of re-deriving a cell grid that no longer exists.
  prism::layout::Rect ItemRect(int index) {
    const std::vector<prism::layout::FlowItem> flow = FlowInputs();
    return prism::layout::FlowItemRect(layout_, flow, index);
  }

  // The ONE layout the frame painted -- read by the polish suite to assert the
  // rail is part of the grid, not list-mode chrome.
  const prism::layout::Layout& LayoutForTest() const { return layout_; }

  // V4: the window's own hit test, exposed so a suite can compare a point IT
  // computed from the definition with the item the window resolves there.
  int HitTestForTest(int x, int y) { return ItemAtPoint(x, y); }

  // V4: the status band's readout. The app sets it from the engine's archive
  // properties; until it does, every field honestly reads "unknown".
  void SetArchiveStatus(const prism::layout::StatusInfo& status) {
    archiveStatus_ = status;
    Invalidate();
  }
  const prism::layout::StatusInfo& ArchiveStatus() const { return archiveStatus_; }
  // Engine facts the catalog cannot derive (method / dictionary / cipher).
  void SetArchiveInfo(const prism::sidebar::ArchiveInfo& info) {
    sidebar_.SetArchiveInfo(info);
    sidebarDirty_ = true;
    // V4: the status band reads the SAME archive info the rail does. The engine
    // reports "no password" as Unknown, so the band says so instead of lying
    // with "未加密"; the cipher is only shown while the archive really is
    // encrypted.
    using prism::layout::CryptoState;
    switch (info.encryption) {
      case prism::sidebar::Encryption::None:
        archiveStatus_.crypto = CryptoState::Unencrypted;
        break;
      case prism::sidebar::Encryption::Aes256:
        archiveStatus_.crypto = CryptoState::Encrypted;
        break;
      case prism::sidebar::Encryption::Unknown:
        archiveStatus_.crypto = CryptoState::Unknown;
        break;
    }
    // Method and dictionary are engine facts the rail already receives; the
    // status band shows the SAME ones (one archive, one readout).
    statusMethod_ = info.method;
    statusDictionary_ = info.dictionaryBytes != 0
                            ? std::string("字典 ") +
                                  prism::list::FormatBytes(info.dictionaryBytes)
                            : std::string();
    archiveStatus_.method = statusMethod_;
    archiveStatus_.dictionary = statusDictionary_;
    Invalidate();
    Invalidate();
  }

  // ---------- card C27 seams: window menu, sidebar painting, list columns -----
  //
  // MENU. The window owns the bar (it is chrome on its own HWND) but never owns
  // the WORDS: the app hands in translated labels, and every change rebuilds the
  // real HMENU, so a language switch is visible to GetMenuStringW -- a string in
  // a member field would look translated in a test and stay English on screen.
  static constexpr UINT kMenuCompress = 0xE001;  // "compress..." (C9 + C18)
  static constexpr UINT kMenuExit = 0xE002;
  // C54: the Edit menu. The same three actions are also keyboard reachable
  // (Delete / F2 / Ctrl+Shift+N); the menu exists so a user who does not know
  // the chords can still find them.
  static constexpr UINT kMenuEditDelete = 0xE003;
  static constexpr UINT kMenuEditRename = 0xE004;
  static constexpr UINT kMenuEditNewFolder = 0xE005;

  void SetMenuLabels(std::wstring fileTitle, std::wstring compressText, std::wstring exitText) {
    fileMenuLabel_ = std::move(fileTitle);
    compressMenuLabel_ = std::move(compressText);
    exitMenuLabel_ = std::move(exitText);
    RebuildMenu();
  }

  // The Edit menu's words are the app's, like the File menu's (see above).
  void SetEditMenuLabels(std::wstring editTitle, std::wstring deleteText,
                         std::wstring renameText, std::wstring newFolderText) {
    editMenuLabel_ = std::move(editTitle);
    editDeleteLabel_ = std::move(deleteText);
    editRenameLabel_ = std::move(renameText);
    editNewFolderLabel_ = std::move(newFolderText);
    RebuildMenu();
  }
  // C64: the Settings item's word, from the same translator as the rest of the
  // bar (see RebuildMenu; SettingsDialog::InstallMenu takes the same string for
  // the no-menu fallback).
  // C57: the Help submenu's two labels (translated by the app like every other
  // menu string; the defaults are English).
  void SetAboutMenuLabels(std::wstring helpTitle, std::wstring aboutItem) {
    helpMenuLabel_ = std::move(helpTitle);
    aboutMenuLabel_ = std::move(aboutItem);
    RebuildMenu();
  }
  void SetSettingsMenuLabel(std::wstring text) {
    settingsMenuLabel_ = std::move(text);
    RebuildMenu();
  }

  // Requirement 3: a read-only archive, a multi-volume set or a file another
  // process holds exclusively DISABLES the entries before the user confirms a
  // rewrite that can only fail, and the reason is readable (the app builds it
  // from C51's refusal, see prism::app::ProbeEditGate).
  void SetEditAvailability(bool enabled, std::wstring reason = {}) {
    editEnabled_ = enabled;
    editDisabledReason_ = std::move(reason);
    RebuildMenu();
    SyncActionContext();
    Invalidate();
  }
  bool EditEnabled() const { return editEnabled_; }
  const std::wstring& EditDisabledReason() const { return editDisabledReason_; }
  bool MenuItemEnabled(UINT command) const {
    HMENU bar = host_.hwnd ? GetMenu(host_.hwnd) : nullptr;
    if (!bar) return false;
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
      HMENU popup = GetSubMenu(bar, i);
      if (!popup) continue;
      const UINT state = GetMenuState(popup, command, MF_BYCOMMAND);
      if (state != static_cast<UINT>(-1)) return (state & MF_GRAYED) == 0;
    }
    return false;
  }

  // ---------- card C54: editing entry points + the single-operation gate -----
  // The window owns exactly two things here: the INTENT it derives from its own
  // selection (which archive-relative path, which folder) and the GATE that
  // makes one operation exclusive (requirement 4). What an edit costs, whether
  // it is legal, and whether the user confirmed it stay with the plan/dialog/app.
  void SetEditHandler(
      std::function<void(const prism::win::editui::EditIntent&)> handler) {
    onEdit_ = std::move(handler);
  }
  const prism::win::editui::EditIntent& LastEditIntent() const { return lastEdit_; }

  enum class OperationKind : std::uint8_t { None, Edit, Extract, Preview, Compress };

  // Called by the app BEFORE it starts a long operation. False = another one is
  // already running; the caller must NOT start, and this method has already set
  // the readable refusal into the status line.
  bool BeginOperation(OperationKind kind, std::wstring label) {
    if (kind == OperationKind::None) return true;
    if (!prism::win::editui::ConcurrencyGateActive()) {
      // MUTANT: the gate does not refuse. The state is still recorded so the
      // rest of the window behaves as it did on the clean build.
      operationBusy_ = true;
      operationKind_ = kind;
      operationLabel_ = std::move(label);
      return true;
    }
    if (operationBusy_) {
      RefuseOperation(OperationName(kind));
      return false;
    }
    operationBusy_ = true;
    operationKind_ = kind;
    operationLabel_ = std::move(label);
    if (kind == OperationKind::Edit) quickLookClose();  // no preview during an edit
    Invalidate();
    return true;
  }
  void EndOperation() {
    operationBusy_ = false;
    operationKind_ = OperationKind::None;
    operationLabel_.clear();
    Invalidate();
  }
  bool OperationBusy() const { return operationBusy_; }
  OperationKind BusyKind() const { return operationKind_; }
  const std::wstring& OperationLabel() const { return operationLabel_; }
  // Diagnostics the suite asserts: the refusal must be COUNTED and readable.
  int RefusedOperationCount() const { return refusedOperations_; }
  const std::wstring& LastOperationRefusal() const { return lastOperationRefusal_; }
  // WM_COMMAND -> here. The window decides nothing about what "compress" means.
  void SetMenuCommandHandler(std::function<void(UINT)> handler) {
    onMenuCommand_ = std::move(handler);
  }
  // Read-back of what the bar ACTUALLY says (see the note above).
  std::wstring MenuItemText(UINT command) const {
    HMENU bar = host_.hwnd ? GetMenu(host_.hwnd) : nullptr;
    if (!bar) return {};
    const int count = GetMenuItemCount(bar);
    for (int i = 0; i < count; ++i) {
      HMENU popup = GetSubMenu(bar, i);
      if (!popup) continue;
      wchar_t text[256] = {};
      const int length = GetMenuStringW(popup, command, text, 256, MF_BYCOMMAND);
      if (length > 0) return std::wstring(text, static_cast<size_t>(length));
    }
    return {};
  }
  std::wstring MenuTitle() const {
    HMENU bar = host_.hwnd ? GetMenu(host_.hwnd) : nullptr;
    if (!bar) return {};
    wchar_t text[256] = {};
    const int length = GetMenuStringW(bar, 0, text, 256, MF_BYPOSITION);
    return length > 0 ? std::wstring(text, static_cast<size_t>(length)) : std::wstring();
  }

  // C29: the rail is a COLUMN in list mode (default ON). V1: it RESERVES width
  // through the layout, so toggling it reflows the list content.
  void SetSidebarRailVisible(bool visible) {
    if (sidebarRailVisible_ == visible) return;
    sidebarRailVisible_ = visible;
    RecomputeLayout();
    Invalidate();
  }
  bool SidebarRailVisible() const { return sidebarRailVisible_; }

  // C13 columns are geometry, not policy: the app persists what the user had and
  // hands it back here; the renderer paints exactly this.
  void SetListColumns(const prism::list::ColumnLayout& columns) {
    listColumns_ = columns;
    hasListColumns_ = true;
    Invalidate();
  }
  const prism::list::ColumnLayout& ListColumns() const { return listColumns_; }
  bool HasListColumns() const { return hasListColumns_; }

  // The frame size the window had when it closed -- read AFTER Run() returns, so
  // the app can persist the geometry of the window the user actually closed
  // (the HWND is gone by then; this is captured in WM_CLOSE, before the
  // DestroyWindow that ends the message loop).
  bool ClosedFrameSize(int* width, int* height) const {
    if (width) *width = closedWidth_;
    if (height) *height = closedHeight_;
    return closedWidth_ > 0 && closedHeight_ > 0;
  }

  // C66: apply a persisted frame through the ONE policy (visible frame + work
  // area clamp). Main.cpp calls this instead of a bare SetWindowPos, because a
  // second call site is a second chance to forget the clamp -- and forgetting it
  // is exactly what put the last three action buttons off screen.
  //
  // The saved value is a SIZE, not a position: the position the window was
  // created at is kept when it fits, and the clamp moves it on screen when it
  // does not (a stale position from an unplugged monitor, negative coordinates).
  prism::win::frame::Restore RestoreFrame(int width, int height) {
    if (host_.hwnd == nullptr || width <= 0 || height <= 0) {
      return prism::win::frame::Restore{};
    }
    const prism::layout::Rect current = prism::win::frame::ReadWindowRect(host_.hwnd);
    const prism::layout::Rect saved{current.left, current.top, current.left + width,
                                    current.top + height};
    prism::win::frame::Restore restored =
        prism::win::frame::RestoreVisibleFrame(host_.hwnd, saved);
    RecomputeLayout();
    Invalidate();
    return restored;
  }

  // Last sidebar row a click landed on (diagnostics/tests). section None means
  // "no sidebar click since the last reset".
  prism::sidebar::FocusTarget LastSidebarHit() const { return lastSidebarHit_; }

  // C27: C14's persistence port for the sidebar state (favourites, recent,
  // collapsed folders). Restore runs here so the lists survive a restart; Save
  // runs at exit, after Run() returned and before the window dies.
  void AttachSidebarStore(prism::sidebar::SidebarStore* store) {
    sidebarStore_ = store;
    if (sidebarStore_ != nullptr && sidebar_.Restore(sidebarStore_)) {
      sidebarRestored_ = true;
      sidebarDirty_ = true;
      Invalidate();
    }
  }
  bool SaveSidebar() {
    if (sidebarStore_ == nullptr) return false;
    sidebar_.Save(sidebarStore_);
    return true;
  }
  bool SidebarRestored() const { return sidebarRestored_; }

  // The archive the window shows changed on disk (a re-pack finished). Posted by
  // a worker -- the handler runs on the UI thread, where a catalog reload is
  // legal (a worker must never touch the catalog the window navigates).
  //
  // C34: the message carries the NEW archive's path. A re-pack writes a new
  // FILE, so "refresh" means "switch to it" -- the parameter is what makes that
  // possible without a second member the worker would have to touch.
  static constexpr UINT kArchiveChanged = WM_APP + 5;
  // C54: a worker finished an exclusive operation. The UI thread releases the
  // gate and shows the worker's one status line -- a worker must never call
  // EndOperation itself (it is UI-thread state).
  static constexpr UINT kOperationFinished = WM_APP + 6;
  void PostArchiveChanged(std::wstring path) {
    if (!host_.hwnd) return;
    auto* heap = new std::wstring(std::move(path));
    if (!PostMessageW(host_.hwnd, kArchiveChanged, 0, reinterpret_cast<LPARAM>(heap))) {
      delete heap;  // the window is gone; nobody will free it for us
    }
  }
  void PostOperationFinished(std::wstring status) {
    if (!host_.hwnd) return;
    auto* heap = new std::wstring(std::move(status));
    if (!PostMessageW(host_.hwnd, kOperationFinished, 0, reinterpret_cast<LPARAM>(heap))) {
      delete heap;
    }
  }
  void SetArchiveChangedHandler(std::function<void(const std::wstring&)> handler) {
    onArchiveChanged_ = std::move(handler);
  }

  // ---------- card C34: the glass layer's lifetime (C33's call site) --------
  //
  // A composition stack plus one swapchain per island is memory a minimised
  // window cannot use. WM_SIZE(SIZE_MINIMIZED) releases it through
  // GlassIslandHost::ReleaseTransient (which also calls IDXGIDevice3::Trim --
  // without that call not one byte comes back, measured in C33) and the restore
  // path rebuilds it. The island is the NAV PILL's own rect, never the window's
  // (C33 finding 1: window-sized islands are where the 40 MB went).
  //
  // OPT-IN (EnableGlassLayer) on purpose: a window created by another card's
  // suite must not grow a DirectComposition target behind that suite's back.
  // Main.cpp enables it for the app.
  static constexpr UINT kGlassIslandWidth = 960;
  static constexpr UINT kGlassIslandHeight = 48;

  // Creates the composition host and its one island. False means "this machine
  // (or this window station) has no DirectComposition": the window keeps the
  // pre-DComp D2D path and no glass memory is ever held.
  bool EnableGlassLayer(const std::wstring& shaderDirectory = std::wstring()) {
    if (glass_) return true;
    if (!host_.hwnd) return false;
    auto host = std::make_unique<prism::win::glass::GlassIslandHost>();
    const std::wstring shaders =
        shaderDirectory.empty() ? DefaultShaderDirectory() : shaderDirectory;
    if (!host->Initialize(host_.hwnd, shaders) || !host->UsesComposition()) return false;
    // ONE island, sized to the pill it belongs to (C33 finding 1).
    if (!host->AddIsland(kGlassIslandWidth, kGlassIslandHeight)) return false;
    glass_ = std::move(host);
    PresentGlassFrame();
    return true;
  }
  bool GlassLayerReady() const { return glass_ != nullptr; }
  bool GlassLayerVisible() const { return glass_ != nullptr && glass_->Visible(); }
  size_t GlassTransientBytes() const { return glass_ ? glass_->TransientBytes() : 0; }
  bool GlassTransientLive() const { return glass_ ? glass_->TransientLive() : false; }
  // The bytes the last release ACTUALLY returned (C33's accounting, derived
  // from live COM pointers): 0 while nothing was held, 0 again on a second
  // release -- which is how the negative control proves the call really ran.
  size_t GlassReleasedBytes() const { return glassReleasedBytes_; }
  size_t LastThumbTrimBytes() const { return lastThumbTrimBytes_; }

  // The idle path, callable without synthesising WM_SIZE: the app's minimise
  // handler and the suite must take the SAME road.
  //
  // ReleaseTransient is idempotent and reports the bytes it really gave back,
  // so calling it twice is safe and the accounting is honest.
  void ReleaseGlassLayer() {
#if defined(PRISM_C34_MUTANT_NO_TRIM)
    // MUTATION (CONTRACTS D3): the minimise path keeps the layer. The suite's
    // C33 accounting assertion (transient == 0 and not live) must go red, and
    // the mutant's runtime sign is glassReleasedBytes_ staying 0 while the
    // layer is still live.
    glassReleasedBytes_ = 0;
#else
    if (!glass_) return;
    glassReleasedBytes_ = glass_->SetVisible(false);  // -> ReleaseTransient()
#endif
  }

  // The restore path. SetVisible(true) alone is lazy (C33), so the surfaces are
  // rebuilt here rather than on the next paint: a restore the user sees has to
  // be a complete frame, not a first-frame stall.
  bool RestoreGlassLayer() {
    if (!glass_) return false;
    glass_->SetVisible(true);
    if (FAILED(glass_->EnsureTransient())) return false;
    PresentGlassFrame();
    Invalidate();
    return true;
  }

  // C33: the budget is a CAP, not an allocation, so a gallery that once filled
  // it keeps it. The idle target is an eighth (PERF.md 2.1's measured path).
  void TrimThumbnails() {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    if (!thumbs_) return;
    lastThumbTrimBytes_ = thumbs_->Trim(thumbs_->BudgetBytes() / 8);
  }

  // C75: a minimised window shows no tile, so every cached pixel is dead weight.
  // ReleaseAll (not the eighth) is the idle state: ADR-006's tier is a budget on
  // the PROCESS, and keeping 8 MiB of invisible thumbnails is what kept the
  // minimised tier above it. The store is keyed by catalog index, so a restore
  // that re-sweeps repopulates exactly the same entries.
  size_t ReleaseAllThumbnails() {
    std::lock_guard<std::mutex> lock(thumbMutex_);
    if (!thumbs_) return 0;
    lastThumbTrimBytes_ = thumbs_->ReleaseAll();
    return lastThumbTrimBytes_;
  }

  // ---------- card C75: the idle RESIDENT set, counted from live objects -----
  // C33's rule: numbers come from what is actually held (live COM pointers,
  // container sizes), never from a parallel counter that can drift. The test
  // suite prints these before/after the minimise path; docs/PERF.md §9 carries
  // the resulting table.
  struct MemorySnapshot {
    size_t catalogItems = 0;
    size_t catalogNameBytes = 0;
    size_t folderItems = 0;
    size_t folderNameBytes = 0;
    size_t listItems = 0;
    size_t thumbBytes = 0;
    size_t thumbEntries = 0;
    size_t thumbBudget = 0;
    size_t glassTransientBytes = 0;
    size_t glassIslands = 0;
    bool glassDeviceLive = false;
    bool targetLive = false;
    size_t targetWidth = 0;
    size_t targetHeight = 0;
    size_t targetBytes = 0;  // one BGRA back buffer
    int rendererBrushes = 0;
    int rendererTextFormats = 0;
    size_t rendererThumbBitmaps = 0;
    size_t rendererThumbBitmapBytes = 0;
    size_t sidebarCommands = 0;
    size_t tabs = 0;
    size_t textFormats = 0;  // renderer formats + the window's crumb format
  };
  MemorySnapshot CountMemory() {
    MemorySnapshot m;
    if (catalog_) {
      m.catalogItems = catalog_->size();
      for (const prism::ArchiveItem& item : *catalog_) m.catalogNameBytes += item.path.size();
    }
    m.folderItems = folderItems_.size();
    for (const prism::ArchiveItem& item : folderItems_) m.folderNameBytes += item.path.size();
    m.listItems = listItems_.size();
    {
      std::lock_guard<std::mutex> lock(thumbMutex_);
      if (thumbs_) {
        m.thumbBytes = thumbs_->SizeBytes();
        m.thumbEntries = thumbs_->Count();
        m.thumbBudget = thumbs_->BudgetBytes();
      }
    }
    if (glass_) {
      m.glassTransientBytes = glass_->TransientBytes();
      m.glassIslands = glass_->IslandCount();
      m.glassDeviceLive = glass_->DeviceLive();
    }
    if (target_) {
      const D2D1_SIZE_U pixels = target_->GetPixelSize();
      m.targetLive = true;
      m.targetWidth = pixels.width;
      m.targetHeight = pixels.height;
      m.targetBytes = static_cast<size_t>(pixels.width) * pixels.height * 4u;
    }
    m.rendererBrushes = renderer_.BrushCount();
    m.rendererTextFormats = renderer_.TextFormatCount();
    m.rendererThumbBitmaps = renderer_.ThumbBitmapCount();
    m.rendererThumbBitmapBytes = renderer_.ThumbBitmapBytes();
    m.sidebarCommands = sidebarLayout_.commands.size();
    m.tabs = tabs_.size();
    m.textFormats = static_cast<size_t>(m.rendererTextFormats) + (crumbFormat_ ? 1u : 0u);
    return m;
  }

  // ---------- card C75: release / rebuild the idle resident set -------------
  // Order matters only for the accounting: the glass graph and the thumbnails
  // go first (they are the big blocks), then the renderer's target-bound objects
  // and finally the D2D hwnd target itself -- its D3D device owns the back
  // buffer and the driver's intermediate surfaces.
  //
  // The D3D11 device of the glass host is deliberately KEPT: re-creating D3D11 +
  // D2D + the effect factories costs ~19 ms (C65's glass cell) and a restore
  // must stay inside the pre-C75 frame budget. IDXGIDevice3::Trim is what turns
  // the released pointers into working set (C33 measured: without it, 0 bytes).
  size_t ReleaseRenderMemory() {
#if defined(PRISM_C75_MUTANT_NO_TRIM)
    // MUTATION (CONTRACTS D3/D27): the idle path releases nothing. The memory
    // assertion in c75.mem_idle must go red; the runtime predicate is the
    // return value, so a red from another cause cannot claim the mutation.
    glassReleasedBytes_ = 0;
    lastThumbTrimBytes_ = 0;
    return 0;
#else
    size_t returned = 0;
    glassReleasedBytes_ = 0;
    if (glass_) glassReleasedBytes_ = glass_->SetVisible(false);
    returned += glassReleasedBytes_;
    returned += ReleaseAllThumbnails();
    // Count what the target holds before dropping it, so the report is honest
    // about the bytes the release is about (not what Windows later reclaims).
    size_t targetBytes = 0;
    if (target_) {
      const D2D1_SIZE_U pixels = target_->GetPixelSize();
      targetBytes = static_cast<size_t>(pixels.width) * pixels.height * 4u;
    }
    renderer_.ReleaseDeviceResources();
    rendererReady_ = false;
    // The window's own text-measurement factory/format are target-independent
    // but a minimised window measures nothing; dropping them also drops the
    // window's reference to the shared DWrite font cache.
    crumbFormat_.Reset();
    writeFactory_.Reset();
    target_.Reset();
    // One trim for the whole stack, after every pointer is gone.
    const bool trimmed = glass_ ? glass_->TrimDevice() : TrimOwnDevice();
    (void)trimmed;
    returned += targetBytes;
    lastReleasedTargetBytes_ = targetBytes;
    ++idleReleases_;
    return returned;
#endif
  }

  // Rebuilds the target and every target-bound object, then paints ONCE,
  // synchronously, so the window can never present an unpainted client area
  // between the restore and the first WM_PAINT. Called from WM_SIZE
  // (SIZE_RESTORED / SIZE_MAXIMIZED) with the new client size already applied.
  bool RestoreRenderMemory() {
    EnsureTextMeasurement();  // the font side of what the minimise path dropped
    if (!EnsureDeviceResources()) return false;
    if (!rendererInitialized_) rendererInitialized_ = SUCCEEDED(renderer_.Initialize());
    if (!rendererInitialized_) return false;
    rendererReady_ = SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
    if (!rendererReady_) return false;
    if (glass_) {
      glass_->SetVisible(true);
      if (FAILED(glass_->EnsureTransient())) return false;
      PresentGlassFrame();
    }
    Render();
    ++restoreRebuilds_;
    return true;
  }

  size_t LastReleasedTargetBytes() const { return lastReleasedTargetBytes_; }
  int IdleReleases() const { return idleReleases_; }
  int RestoreRebuilds() const { return restoreRebuilds_; }
  bool RendererReady() const { return rendererReady_; }
  bool TargetLive() const { return target_ != nullptr; }

  // The window's own D2D hwnd target creates its D3D device internally; D2D
  // does not expose it as IDXGIDevice (QueryInterface is attempted here, and a
  // failure is not fatal). When composition is on this is a no-op: the glass
  // host's device is the one TrimDevice above already trimmed.
  bool TrimOwnDevice() {
    if (!target_) return false;
    ComPtr<IDXGIDevice3> dxgi;
    if (FAILED(target_.As(&dxgi)) || !dxgi) return false;
    dxgi->Trim();
    return true;
  }

  // ---------- card C34: the idle pump ----------
  // The app's companion preview (C30) is woken by an auto-reset event. The
  // window owns the message loop, so it offers ONE timer tick; the handler runs
  // on the UI thread and the window knows nothing about what it polls.
  static constexpr UINT kIdleTimerId = 0xC34;
  static constexpr UINT kIdleTimerMs = 60;
  void SetIdleHandler(std::function<void()> handler) {
    onIdle_ = std::move(handler);
    EnsureIdleTimer();
  }

  // C13: list-mode rows are a SORTED, FILTERED VIEW of the current folder.
  // Defaults (name ascending, empty filter) reproduce the navigator's order, so
  // wiring them changes no pixels until someone asks for a different order.
  void SetSortSpec(const prism::list::MultiSortSpec& spec) {
    sortSpec_ = spec;
    RebuildListItems();
    Invalidate();
  }
  const prism::list::MultiSortSpec& SortSpec() const { return sortSpec_; }
  void SetFilter(const prism::list::FilterSpec& spec) {
    filter_ = spec;
    RebuildListItems();
    Invalidate();
  }
  const prism::list::FilterSpec& Filter() const { return filter_; }

  // ---------- card C62: the search box --------------------------------------
  // The field is pure PrismCore state (SearchBoxModel) plus a list of drawing
  // instructions (SearchBoxPainter). The window feeds it keys and applies its
  // QUERY to the list filter; it never re-decides Escape, IME or code-point
  // boundaries. C60 owns the final placement, so the rectangle comes from the
  // ONE layout authority, never from a constant in the message handler.
  prism::search::Model& SearchModel() { return search_; }
  const prism::search::Model& SearchModel() const { return search_; }
  bool SearchOpen() const { return search_.IsOpen(); }
  const prism::win::searchbox::Scene& SearchScene() const { return searchScene_; }

  void OpenSearch() {
    if (search_.IsOpen()) {
      search_.SelectAll();  // a second Ctrl+F selects the current query
      Invalidate();
      return;
    }
    // Not a modal overlay -- the list stays visible and filters live -- but it
    // IS a second keyboard owner, so the other two give up the keyboard.
    DismissOtherOverlays(ActiveOverlay::None);
    if (!listMode_) SetListMode(true);  // search results are the list view
    prism::win::searchbox::Options options;
    options.variant = prism::theme::Variant::Daylight;  // the window's palette
    options.dpi = 96;    // HwndRenderTarget coordinates are DIPs already
    options.fontSize = 15.0f;  // matches crumbFormat_ (MeasureUtf8 below)
    options.measure = [this](std::string_view text) {
      return static_cast<float>(MeasureUtf8(text));
    };
    searchPainter_ = prism::win::searchbox::SearchBoxPainter(std::move(options));
    search_.Open();
    Invalidate();
  }

  void CloseSearch() {
    search_.Close();       // clearing restores the rows: one rule, not two
    ApplySearchFilter();
  }
  const std::vector<prism::list::Row>& ListRows() const { return listRows_; }
  int ListRowCount() const { return static_cast<int>(listItems_.size()); }
  // The view row <-> folder row glue, exposed so the suite can prove the two
  // orders cannot hand the selection to the wrong entry.
  int FolderRowForListRow(int row) const {
    if (row < 0 || row >= static_cast<int>(listRowToFolder_.size())) return -1;
    return listRowToFolder_[static_cast<size_t>(row)];
  }
  int ListRowForFolderRow(int row) const {
    if (row < 0 || row >= static_cast<int>(folderToListRow_.size())) return -1;
    return folderToListRow_[static_cast<size_t>(row)];
  }

  // ---------- worker -> UI hand-off (THREADING CONTRACT) ----------
  // Every mutable member of this class belongs to the UI thread. A worker may
  // call ONLY these two functions (and SetThumbs, which takes its own mutex),
  // because they touch nothing and merely POST: the state change happens in
  // WndProc on the UI thread.
  //
  // This exists because batch A's activate handler ran on a detached thread and
  // reached into Hud() and SetStatus() directly. Hud() has no lock (it is fed by
  // kProgressMessage on the UI thread "single-threaded by rule", see kProgress-
  // Message below), so that was a data race on every extraction, and the
  // detached thread could outlive the window: Main.cpp's `window` is a stack
  // object destroyed after Run() returns, so closing the app mid-extraction
  // left the worker writing into freed memory.
  // HUD headline is UTF-8 (ProgressHud's convention); the status line is wide
  // (the window paints it with DirectWrite). Two encodings on purpose -- the
  // helpers are typed so a wrong one is a compile error, not a mojibake bug.
  void PostHudStart(std::string headline) {
    if (!host_.hwnd) return;
    auto* heap = new std::string(std::move(headline));
    if (!PostMessageW(host_.hwnd, kHudStart, 0, reinterpret_cast<LPARAM>(heap))) {
      delete heap;  // the window is gone; nobody will free it for us
    }
  }
  void PostStatus(std::wstring text) {
    if (!host_.hwnd) return;
    auto* heap = new std::wstring(std::move(text));
    if (!PostMessageW(host_.hwnd, kStatusText, 0, reinterpret_cast<LPARAM>(heap))) {
      delete heap;
    }
  }
  // Top-level progress (the HUD's expanded phase). Posted rather than called for
  // the same reason: hud_ is UI-thread state.
  void PostProgress(uint64_t done, uint64_t total) {
    if (!host_.hwnd) return;
    PostMessageW(host_.hwnd, kProgressMessage, static_cast<WPARAM>(done),
                 static_cast<LPARAM>(total));
  }

  // Progress HUD: the worker drives phases; the UI renders.
  // NOT thread-safe on purpose -- see the hand-off block above. Called from
  // WndProc and from tests that drive the window on one thread; a worker uses
  // PostHudStart/PostProgress/PostStatus instead.
  prism::ProgressHud& Hud() { return hud_; }

  // Wakes the message loop so a quit requested by a worker is seen even if the
  // app is idle. Used after a worker sets AppContext::shuttingDown.
  void RequestClose() {
    // IsWindow, not just "hwnd != nullptr": after WM_DESTROY the handle value is
    // stale, and posting to a recycled handle would be closing somebody else's
    // window. A failed post is the expected outcome once the app is going down.
    if (host_.hwnd && IsWindow(host_.hwnd)) PostMessageW(host_.hwnd, WM_CLOSE, 0, 0);
  }

  void Show() {
    if (host_.hwnd) ShowWindow(host_.hwnd, SW_SHOW);
  }

  // The message loop. It returns after WM_QUIT, which WM_DESTROY posts (see
  // Proc) -- i.e. as soon as the last top-level window of this thread is gone.
  // GetMessageW returning -1 (a failure, not a quit) also leaves the loop: a
  // thread that cannot read its own queue must not spin in it forever.
  void Run() {
    Show();
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }

  // ---------- navigation (batch A increment 5) ----------
  // Client-coordinate entry points. The WM_* handlers are two lines each:
  // they forward the pixel and execute whatever PrismCore decided. Keeping
  // them callable also lets the Windows integration test drive the REAL
  // window class without a mouse (L2: the object under test is not a mock).

  // Returns true when the click was consumed (sidebar, breadcrumb or item).
  bool ClickAt(int x, int y) { return ClickAt(x, y, false, false); }
  // C67: the modifiers are the DECISION's input (replace / toggle / extend);
  // PrismCore's SelectionModel owns the rule, this only forwards.
  bool ClickAt(int x, int y, bool ctrl, bool shift) {
    // The sidebar rail is painted OVER the left edge of the content (C27), so a
    // click inside it belongs to the sidebar -- never to a tile hidden under it.
    // RowHit commands are the same boxes the renderer fills, which is what keeps
    // "what I see" and "what I hit" one list.
    // The rail is painted (and therefore clickable) in LIST mode only: the grid
    // keeps its full-bleed geometry and its tiles must stay reachable.
    if (sidebarRailVisible_ && SidebarClick(x, y)) return true;
    if (CrumbClick(x, y)) return true;
    // A click inside the breadcrumb bar that misses every crumb is NOT "click
    // on empty list space" -- swallowing it into SetSelection(-1) silently
    // wiped the user's selection every time they missed a crumb by a few
    // pixels. The bar is chrome, not a deselect surface.
    if (y >= barTop_ && y < barBottom_) return true;
    const int row = ItemAtPoint(x, y);  // -1 == empty space
    // C67: only a press that actually lands ON a row may grow into a drag out;
    // arming it on chrome (sidebar, crumbs, empty space) would start a drag of
    // the previous selection when the user merely dragged the scrollbar area.
    dragArmed_ = row >= 0;
    dragPressX_ = x;
    dragPressY_ = y;
    sel_.Click(row, prism::sel::ClickModifiers{ctrl, shift});
    // The FOCUS row follows the set; an empty set has no focus, so a chord that
    // acts on "the selected row" (Ctrl+E, Quick Look) cannot fire at a row the
    // user just deselected.
    selection_ = sel_.Empty() ? -1 : sel_.Focus();
    nav_.SetSelection(selection_);  // the navigator keeps what Backspace restores
    SyncActionContext();
    Invalidate();
    return true;
  }

  bool DoubleClickAt(int x, int y) { return DoubleClickAt(x, y, false, false); }
  bool DoubleClickAt(int x, int y, bool ctrl, bool shift) {
    if (CrumbClick(x, y)) return true;
    const int row = ItemAtPoint(x, y);
    // MUTANT ACTIVE (card C67, arm alt_opens) lives in AltChordEnabled(): with
    // the guard switched off this reads false and Alt+double-click falls
    // through to the ordinary open. The banner and the verdict are in
    // Tests/win/test_c67_gestures.cpp next to the same macro.
    const bool alt = AltChordEnabled() && AltDown();
    const prism::keys::PointerAction gesture = prism::keys::ResolvePointer(
        prism::keys::PointerStroke{2, alt, ctrl, shift, false});
    if (gesture == prism::keys::PointerAction::SmartExtract) {
      if (row < 0 || row >= ItemCount()) return true;
      // A FOLDER still drills in: "extract this directory" has no meaning the
      // user asked for, and Bandizip does the same.
      if (folderItems_[static_cast<std::size_t>(row)].isDir) return ActivateRow(row);
      if (!OperationAllowed(L"\u89E3\u538B")) return true;  // 解压
      SetSelection(row);
      if (onSmartExtract_) {
        onSmartExtract_(static_cast<int>(folderItems_[static_cast<std::size_t>(row)].index));
      }
      return true;
    }
    return ActivateRow(row);
  }

  // Enter / double-click on a row: folders drill in, files go to the app.
  bool ActivateRow(int row) {
    if (row < 0 || row >= nav_.Count()) return false;
    // Requirement 4: a file activation starts an extraction. While an edit is
    // running, that second operation is REFUSED with a reason instead of being
    // allowed to race the rewrite (the app's reader pool and the editor would
    // fight over the same file). Folder navigation stays allowed: it touches
    // nothing on disk.
    if (!folderItems_[static_cast<size_t>(row)].isDir &&
        !OperationAllowed(L"\u89E3\u538B")) {  // 解压
      return true;  // consumed and refused -- never a silent parallel run
    }
    nav_.SetSelection(row);
    ApplySelection(nav_.Selection());
    const prism::tree::NavResult result = nav_.Enter(row);
    if (result.action == prism::tree::NavAction::FolderChanged) {
      AfterFolderChange();
      return true;
    }
    if (result.action == prism::tree::NavAction::ActivateFile) {
      if (onActivate_) onActivate_(static_cast<int>(result.catalogIndex));
      return true;
    }
    return false;
  }

  // Backspace. False at the root: nothing happens, nothing is redrawn.
  bool Back() {
    const prism::tree::NavResult result = nav_.Back();
    if (result.action != prism::tree::NavAction::FolderChanged) return false;
    AfterFolderChange();
    return true;
  }

  // ---------- card V2: the action band (DESIGN.md §2/§6) ----------
  //
  // The window owns exactly TWO pieces of the band: the CONTEXT (derived from
  // the catalog and the navigator's selection -- never typed in by a widget)
  // and the WIRING (a click or an Alt chord becomes a call). What each action
  // means stays with the app: FireAction hands it over, so a test can count
  // commands without starting an extraction.
  const prism::actionbar::Model& ActionBarState() const { return actionBar_; }
  prism::actionbar::Layout ActionBarLayout() const {
    return prism::actionbar::Compute(WindowLayoutForClient());
  }
  // Which button a click at this pixel lands on (-1 == none). Public for the
  // same reason ActionBarLayout is: a test asserts that the rectangle the
  // renderer PAINTS is the rectangle the hit test RESOLVES.
  int ActionBarButtonAt(int x, int y) const { return BandLayout().Hit(x, y); }
  // What the last paint ACTUALLY drew (the renderer's own counters). Comparing
  // these with ActionBarLayout().buttons is the difference between "the model
  // says eight" and "eight were painted" -- the pair the C66/C67 band rule is
  // about (a 7-of-8 band is exactly the state neither side may produce).
  struct ActionPaintCounters {
    int buttons = 0;
    int icons = 0;
    int labels = 0;
    int islands = 0;
    int clippedLabels = 0;
    int disabled = 0;
  };
  ActionPaintCounters PaintedActionCounters() const {
    return ActionPaintCounters{renderer_.dbgActionButtons, renderer_.dbgActionIcons,
                               renderer_.dbgActionLabels,  renderer_.dbgActionIslands,
                               renderer_.dbgActionClippedLabels, renderer_.dbgActionDisabled};
  }
  void SetActionHandler(std::function<void(prism::actionbar::ActionId, int)> handler) {
    onAction_ = std::move(handler);
  }
  // A read-only archive (opened from a CD or a mounted volume) turns the two
  // destructive actions off BEFORE the user confirms something that can only
  // fail. Default false: an archive opened from disk is writable.
  void SetArchiveReadOnly(bool readOnly) {
    if (readOnly_ == readOnly) return;
    readOnly_ = readOnly;
    SyncActionContext();
    Invalidate();
  }
  bool ArchiveReadOnly() const { return readOnly_; }
  // What the band believes right now (tests assert the derived context, not the
  // three inputs, so a stale context cannot hide behind a green matrix).
  prism::actionbar::Context ActionContext() const { return actionBar_.GetContext(); }
  int ActionBarFocus() const { return actionBar_.FocusIndex(); }

  // The painted bar band (client pixels). Exposed so the Windows integration
  // test can aim a click at the bar it can SEE instead of guessing constants.
  // These four are LAZY: the island geometry is only measured when the crumbs
  // are rebuilt, so a caller that asks before the first breadcrumb refresh
  // would read 0 and aim a click at the top-left corner of the client area.
  // const_cast is deliberate and narrow -- measuring is idempotent, touches no
  // observable state a const caller can see, and the alternative (making the
  // accessors non-const) would spread through every read-only call site.
  int NavBarTop() const { EnsureNavIsland(); return barTop_; }
  int NavBarBottom() const { EnsureNavIsland(); return barBottom_; }
  int NavBarTextLeft() const { EnsureNavIsland(); return textLeft_; }
  int NavBarTextRight() const { EnsureNavIsland(); return textRight_; }

  // Test-only geometry probe: when a click lands nowhere, the numbers decide
  // whether the test aimed wrong or the hit test is wrong.
  int ClientWidthForTest() const { return static_cast<int>(clientW_); }
  int MeasureForTest(std::string_view t) { return MeasureUtf8(t); }
  int NavBarCrumbCountForTest() const { return static_cast<int>(crumbLayout_.spans.size()); }
  int ClientHeightForTest() const { return static_cast<int>(clientH_); }

  // List mode (Ctrl+2) is also reachable programmatically: the app may want to
  // persist the user's last view, and the Windows integration test has no way
  // to synthesise a Ctrl chord on a headless window station.
  void SetListMode(bool listMode) {
    if (listMode_ == listMode) return;
    listMode_ = listMode;
    quickLookClose();
    RecomputeLayout();  // 32 px list rows vs the grid's tile cells
    Invalidate();
  }

  // Message-dispatch seam: EXACTLY what the window procedure runs (same
  // switch, same order), exposed so the Windows integration test drives real
  // WM_KEYDOWN/WM_LBUTTONDBLCLK routing instead of a re-implementation of it.
  LRESULT Dispatch(UINT message, WPARAM wparam, LPARAM lparam) {
    return Proc(message, wparam, lparam);
  }

  // Pixel extent of one crumb, so a test can aim at the text the renderer
  // actually paints. The run is CENTRED inside the island, so textLeft_ is NOT
  // where the first crumb begins -- six red rounds burned on that assumption.
  bool CrumbSpanForTest(int level, int* left, int* right) const {
    for (const auto& span : crumbLayout_.spans) {
      if (span.level != level) continue;
      *left = span.left;
      *right = span.right;
      return true;
    }
    return false;
  }

  // Which breadcrumb level is under this pixel? (kNoLevel when none.)
  int CrumbLevelAt(int x, int y) const {
    EnsureNavIsland();  // a click can arrive before the first crumb rebuild
    if (y < barTop_ || y >= barBottom_) return prism::tree::kNoLevel;
    return prism::tree::HitBreadcrumb(crumbLayout_, x);
  }

  // ---------- card C52: dropping files INTO the window ----------------------
  // Explorer -> window. What the drop MEANS is PrismCore's decision
  // (prism::drop::PlanDrop); this window only turns an HDROP into the tree the
  // model wants, paints the hint and forwards the plan to the app. That is the
  // same split as every other interaction: pixels and messages here, policy in
  // PrismCore. Main.cpp is wired by a patch (this card does not touch it).
  //
  // HandleDropFiles is the EXACT entry point WM_DROPFILES uses, so the Windows
  // suite drives the production path instead of a re-implementation of it (the
  // Dispatch seam above exists for the same reason).
  void SetDropHandler(std::function<void(const prism::drop::DropPlan&)> handler) {
    onDrop_ = std::move(handler);
  }
  // Attaching a catalog (SetItems) means an archive is open; a window that
  // never attached one is the "empty window" case. Exposed so a test can drive
  // either state without an engine.
  bool ArchiveOpen() const { return archiveOpen_; }
  void SetArchiveOpen(bool open) { archiveOpen_ = open; }
  bool DropHintVisible() const { return dropHintVisible_; }
  prism::layout::Rect DropHintRect() const { return dropHintRect_; }
  std::wstring DropHintTitle() const { return dropHintTitle_; }
  std::wstring DropHintDetail() const { return dropHintDetail_; }
  POINT DropPoint() const { return dropPoint_; }
  const prism::drop::DropPlan& LastDropPlan() const { return lastDropPlan_; }
  // Paint evidence: how many WM_PAINTs ran and how many of them actually drew
  // the overlay. "The rect is inside the content area" is geometry; these two
  // counters are what proves the hint reaches the device.
  int PaintMessages() const { return paintMessages_; }
  int DropHintDraws() const { return dropHintDraws_; }
  // The same content rectangle the renderer paints (V1 authority), with the
  // pre-first-size fallback the rest of the window uses.
  prism::layout::Rect ContentArea() const { return ContentAreaForDrop(); }
  void ClearDropHint() {
    if (!dropHintVisible_) return;
    dropHintVisible_ = false;
    Invalidate();
  }
  // Consumes the handle: DragFinish is called before returning, exactly as the
  // WM_DROPFILES contract requires.
  void HandleDropFiles(HDROP drop) { EvaluateDrop(drop); }

  // ---------- card C60: the tab strip (DESIGN.md §2.1) ----------------------
  //
  // The window owns the TabSet (C59) and the strip that paints it; every RULE
  // -- which archive is active after a close, the 16-tab ceiling, "one path is
  // one tab", the dirty refusal -- is C59's answer and is NOT re-derived here.
  // What this class adds is exactly what a window can add:
  //   * geometry + hit tests in client pixels (from WindowLayout's band),
  //   * the pointer/keyboard gestures (click, x, middle-click, Ctrl+Tab,
  //     Ctrl+W, drag-reorder),
  //   * the C54 confirmation box a dirty close goes through,
  //   * and ONE callback: "the active tab is now this path" -- the app performs
  //     the switch through C34's SwitchArchive, so a tab click and a menu
  //     "open" are the same path through the product (the card's requirement
  //     4). An empty path means the EMPTY STATE: the last tab was closed, the
  //     window stays up and the strip hides itself.
  prism::tabs::TabSet& Tabs() { return tabs_; }
  const prism::tabs::TabSet& Tabs() const { return tabs_; }
  bool TabBarVisible() const { return !tabs_.empty(); }
  prism::layout::Rect TabBarRect() const { return layout_.tabBar; }
  // What the current frame paints (hover / drag state included), so a suite
  // asserts the same view the renderer was handed.
  prism::win::tabbar::View TabBarView() const { return TabViewNow(); }

  // The three hit tests, exposed for the same reason HitTestForTest exists: a
  // suite must be able to aim at the rectangle THAT WAS PAINTED instead of
  // re-deriving the strip's arithmetic in the test.
  int TabBarTabAt(int x, int y) const {
    return prism::win::tabbar::TabAt(layout_.tabBar, TabBarRects(), x, y);
  }
  int TabBarCloseAt(int x, int y) const {
    return prism::win::tabbar::CloseAt(layout_.tabBar, TabBarRects(), x, y);
  }
  int TabBarInsertSlot(int x) const {
    return prism::win::tabbar::InsertSlot(layout_.tabBar, TabBarRects(), x);
  }
  std::vector<prism::win::tabbar::TabGeometry> TabBarRects() const {
    return prism::win::tabbar::TabRects(layout_.tabBar, TabViewNow());
  }

  // The app's door: "show this archive now" (empty == empty state). The window
  // never opens a file; it only reports which tab became active.
  void SetTabHandler(std::function<void(const std::string&)> handler) {
    onTab_ = std::move(handler);
  }
  // C64: how the app tells a REFUSED switch apart from a successful one.
  //
  // A tab click activates its tab in the model FIRST (so the strip shows what
  // the user asked for) and then asks the app to load the file. If the app
  // cannot (missing/deleted/corrupt archive), the tab must go back to the one
  // whose content is still on screen -- "the strip says beta while the rows are
  // alpha" is the defect this pair exists to prevent. The app consumes the
  // intent when the archive-changed message arrives, then either commits (the
  // normal C34 path) or calls RefuseTabSwitch.
  //
  // Only a CLICK/Ctrl+Tab intent counts: a background re-pack also posts
  // archive-changed, and a failed refresh must not teleport the strip to an
  // unrelated tab.
  bool ConsumeTabSwitchIntent(std::string_view utf8Path) {
    if (pendingTabSwitch_.empty()) return false;
    const bool matches =
        prism::tabs::PathKey(pendingTabSwitch_) == prism::tabs::PathKey(utf8Path);
    if (matches) pendingTabSwitch_.clear();
    return matches;
  }
  // The switch failed: put the active tab back where it was and count the
  // refusal (RefusedOperationCount is the same diagnostic the edit gate uses).
  // Returns false when there is nothing to restore (the tab was closed, or the
  // active path is no longer the one that failed).
  bool RefuseTabSwitch(std::string_view failedPath) {
#if defined(PRISM_C64_MUTANT_FAILED_SWITCH_KEEPS_TAB)
    // NEGATIVE CONTROL: the failed tab stays active while the rows still show
    // the previous archive. The suite's "still on the original tab" assertion
    // MUST go red.
    (void)failedPath;
    ++refusedOperations_;
    return false;
#else
    const prism::tabs::Tab* active = tabs_.active();
    if (active == nullptr) return false;
    if (prism::tabs::PathKey(active->path) != prism::tabs::PathKey(failedPath)) return false;
    const int origin = tabs_.IndexOf(tabSwitchOrigin_);
    if (origin < 0) return false;
    tabs_.Activate(origin);
    pendingTabSwitch_.clear();
    ++refusedOperations_;
    RecomputeLayout();
    Invalidate();
    return true;
#endif
  }
  // The confirmation seam. nullptr (the default) means the REAL C54 dialog:
  // the window builds C54's EditDialogModel and shows C54's EditConfirmDialog,
  // which completes asynchronously through its result handler. A suite injects
  // its own answer so the headless case is synchronous and observable.
  void SetTabCloseConfirmer(std::function<bool(const std::wstring& archiveName)> ask) {
    onTabCloseConfirm_ = std::move(ask);
  }
  int TabClosePrompts() const { return tabClosePrompts_; }
  int TabCloseRefusals() const { return tabCloseRefusals_; }
  std::wstring LastTabClosePrompt() const { return lastTabClosePrompt_; }
  bool TabCloseDialogOpen() const { return tabCloseDialog_ != nullptr; }

  // Where the pointer is over the strip right now (read back by the suite; the
  // paint uses the same fields).
  int TabHover() const { return tabHover_; }
  int TabHoverClose() const { return tabHoverClose_; }
  bool TabDragActive() const { return tabDragActive_; }
  int TabDragSlot() const { return tabDropSlot_; }

  // Open (or re-activate) a tab. This is the APP's call after C34's
  // SwitchArchive succeeded -- it deliberately does NOT fire the tab handler,
  // or opening an archive would re-enter the switch that just opened it.
  // 'activate' defaults to true: a user-opened archive becomes the one shown.
  // C64's command-line loop passes false for tabs 2..N so the FIRST argument
  // stays active while the rest are merely opened (and validated) as tabs.
  prism::tabs::OpenOutcome OpenTab(const std::string& utf8Path, bool exists = true,
                                  bool activate = true) {
    prism::tabs::OpenRequest request;
    request.path = utf8Path;
    request.exists = exists;
    const int activeBefore = tabs_.activeIndex();
    prism::tabs::OpenOutcome outcome = tabs_.Open(request);
    if (outcome.verdict == prism::tabs::TabVerdict::kOk) {
      // "keep the current tab" must also undo the ACTIVATION Open() performs
      // when the path was already open: otherwise a duplicate argument would
      // steal the active tab from the first archive.
      if (!activate && activeBefore >= 0) {
        (void)tabs_.Activate(activeBefore);
        outcome.activatedExisting = false;
      }
      // A new tab, or an existing one activated: the strip changed either way.
      RecomputeLayout();
      Invalidate();
    }
    return outcome;
  }

  // The raw close (no question asked). The C59 verdict is returned verbatim:
  // kNeedsConfirm means "nothing changed, ask the user".
  prism::tabs::CloseOutcome CloseTab(int index, bool confirmed = false) {
    const std::string activeBefore = ActiveTabPath();
    prism::tabs::CloseOutcome outcome = tabs_.Close(index, confirmed);
    if (outcome.closed) AfterTabsChanged(activeBefore);
    return outcome;
  }

  // The UI door: close tab 'index', asking first when C59 says the tab is
  // dirty. Returns the verdict when it completed synchronously; std::nullopt
  // means C54's dialog is now on screen and the close completes when the user
  // answers it (the handler re-enters CloseTab(index, true)).
  std::optional<prism::tabs::CloseOutcome> RequestCloseTab(int index) {
    const std::string activeBefore = ActiveTabPath();
    // tabbar::kDirtyNeedsConfirm is the D3 control: when it is compiled out the
    // dirty tab is closed as if the user had already agreed.
    prism::tabs::CloseOutcome outcome =
        tabs_.Close(index, !prism::win::tabbar::kDirtyNeedsConfirm);
    if (outcome.closed) {
      AfterTabsChanged(activeBefore);
      return outcome;
    }
    if (!outcome.needsConfirm) return outcome;
    const std::wstring name = TabDisplayName(index);
    ++tabClosePrompts_;
    lastTabClosePrompt_ = name;
    if (onTabCloseConfirm_) {
      if (!onTabCloseConfirm_(name)) {
        ++tabCloseRefusals_;
        return outcome;  // refused: the tab is still there, nothing changed
      }
      const prism::tabs::CloseOutcome closed = tabs_.Close(index, true);
      if (closed.closed) AfterTabsChanged(activeBefore);
      return closed;
    }
    ShowTabCloseDialog(index, name);
    return std::nullopt;  // asynchronous: C54's dialog owns the next step
  }

  bool ActivateTab(int index) {
    const std::string activeBefore = ActiveTabPath();
    if (!tabs_.Activate(index)) return false;
    const std::string activeAfter = ActiveTabPath();
    // C64: remember where a REAL switch started. If the app cannot load the
    // target, RefuseTabSwitch() puts this tab back (no blank window).
    if (activeAfter != activeBefore) {
      tabSwitchOrigin_ = activeBefore;
      pendingTabSwitch_ = activeAfter;
    }
    AfterTabsChanged(activeBefore);
    return true;
  }
  // Ctrl+Tab / Ctrl+Shift+Tab: the wrapping ring over the open tabs.
  bool CycleTabs(int delta) {
    const int next = prism::win::tabbar::WrapIndex(tabs_.activeIndex(), delta,
                                                   static_cast<int>(tabs_.size()));
    return next >= 0 && ActivateTab(next);
  }
  bool MoveTab(int from, int to) {
    if (!tabs_.Move(from, to)) return false;
    RecomputeLayout();  // the strip's widths are an equal share: order matters
    Invalidate();
    return true;
  }
  // A drop at an insertion SLOT (what the marker shows) -> TabSet::Move's
  // erase-then-insert index.
  bool DropTab(int from, int slot) {
    return MoveTab(from, prism::win::tabbar::MoveTarget(from, slot));
  }

  // C54's model for "close a tab that has unsaved changes". Public because the
  // suite builds the SAME dialog the product shows and asserts its words.
  static prism::win::editui::EditDialogModel TabCloseModel(const std::wstring& archiveName) {
    prism::win::editui::EditDialogModel model;
    model.dangerous = true;                     // it discards unsaved work
    model.title = L"\u5173\u95ED\u6807\u7B7E";  // 关闭标签
    model.body = L"\u2018" + archiveName +
                 L"\u2019 \u6709\u672A\u4FDD\u5B58\u7684\u4FEE\u6539\u3002"  // 有未保存的修改。
                 L"\u5173\u95ED\u540E\u8FD9\u4E9B\u4FEE\u6539\u4F1A\u4E22\u5931\u3002";
    model.acceptLabel = L"\u5173\u95ED\u5E76\u4E22\u5F03";  // 关闭并丢弃
    model.cancelLabel = L"\u53D6\u6D88";                     // 取消
    model.needsInput = false;
    return model;
  }

 private:
  static LRESULT CALLBACK ProcThunk(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<GalleryWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE && lparam) {
      self = reinterpret_cast<GalleryWindow*>(
          reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
      self->host_.hwnd = hwnd;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Proc(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
  }

  LRESULT Proc(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
      case WM_SIZE: {
        if (wparam == SIZE_MINIMIZED) {
          // C34 + C75: nothing of this window is on screen, so the entire
          // resident set that a visible frame needs goes back to the system:
          // the glass swapchains + effect graph through ReleaseTransient,
          // EVERY cached thumbnail pixel, the renderer's target-bound objects
          // and the D2D hwnd target itself (its D3D device owns the back
          // buffer). IDXGIDevice3::Trim then makes the release real (C33: no
          // Trim, no bytes). No resize here: the client rect keeps the size it
          // had, so a restore repaints exactly what was there.
          ReleaseRenderMemory();
          return 0;
        }
        if (target_) {
          target_->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
        }
        clientW_ = static_cast<double>(LOWORD(lparam));
        clientH_ = static_cast<double>(HIWORD(lparam));
        // V1: ONE layout authority. WM_SIZE reflows the grid columns, the list
        // body and the breadcrumb pill together -- the components no longer
        // carry their own absolute rectangles.
        RecomputeLayout();
        barKnown_ = false;  // the island is a fraction of the client width
        sidebarDirty_ = true;  // the sidebar's status pill is bottom-anchored
        UpdateBreadcrumbLayout();
        // C75: back on screen -> rebuild AND paint inside THIS Resize. Painting
        // lazily on the next WM_PAINT is what leaves an unpainted client area
        // between the restore and the first frame; the suite captures the
        // window right after ShowWindow returns and requires content.
#if defined(PRISM_C75_MUTANT_NO_RESTORE_REBUILD)
        // MUTATION (CONTRACTS D3/D27): the restore path does not rebuild. The
        // blank-frame assertion in c75.mem_idle must go red.
#else
        if (wparam == SIZE_RESTORED || wparam == SIZE_MAXIMIZED) RestoreRenderMemory();
#endif
        Invalidate();
        return 0;
      }
      case WM_TIMER: {
        // C34: the app's idle tick (the companion preview's wake poll). Only
        // our own id is consumed; every other timer stays with DefWindowProc.
        if (wparam == kIdleTimerId) {
          if (onIdle_) onIdle_();
          return 0;
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_CHAR: {
        // C62: only the search field consumes characters. WM_KEYDOWN already
        // handled Backspace/Delete/arrows; this is text (and IME commits).
        if (!search_.IsOpen()) return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        const wchar_t unit = static_cast<wchar_t>(wparam);
        if (unit == L'\b') return 0;  // the key path edited; WM_CHAR repeats it
        if (unit < L' ') return 0;     // Enter/Tab/Esc are keys, not text
        if (unit >= 0xD800 && unit <= 0xDBFF) {
          pendingHighSurrogate_ = unit;  // a surrogate pair arrives as two units
          return 0;
        }
        std::wstring wide(1, unit);
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
          if (pendingHighSurrogate_ == 0) return 0;  // lone low surrogate
          wide.insert(wide.begin(), pendingHighSurrogate_);
          pendingHighSurrogate_ = 0;
        }
        const prism::search::Effect effect = search_.InsertText(WideToUtf8(wide));
        if (effect.queryChanged) ApplySearchFilter();
        Invalidate();
        return 0;
      }
      case WM_IME_STARTCOMPOSITION: {
        if (!search_.IsOpen()) return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        search_.BeginComposition();
        Invalidate();
        return 0;
      }
      case WM_IME_COMPOSITION: {
        // The preview is DisplayText only; Query()/matched count do not move
        // until GCS_RESULTSTR commits. That split is the model's, not this TU's.
        if (!search_.IsOpen()) return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        HIMC context = ImmGetContext(host_.hwnd);
        if (context == nullptr) return 0;
        if ((lparam & GCS_COMPSTR) != 0) {
          const LONG bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0);
          if (bytes > 0) {
            std::wstring wide(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(context, GCS_COMPSTR, wide.data(),
                                     static_cast<DWORD>(bytes));
            search_.UpdateComposition(WideToUtf8(wide));
          }
        }
        if ((lparam & GCS_RESULTSTR) != 0) {
          const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, nullptr, 0);
          if (bytes > 0) {
            std::wstring wide(static_cast<std::size_t>(bytes) / sizeof(wchar_t), L'\0');
            ImmGetCompositionStringW(context, GCS_RESULTSTR, wide.data(),
                                     static_cast<DWORD>(bytes));
            const prism::search::Effect effect = search_.CommitComposition(WideToUtf8(wide));
            if (effect.queryChanged) ApplySearchFilter();
          }
        }
        ImmReleaseContext(host_.hwnd, context);
        Invalidate();
        return 0;
      }
      case WM_IME_ENDCOMPOSITION: {
        if (!search_.IsOpen()) return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        search_.CancelComposition();
        Invalidate();
        return 0;
      }
      case WM_IME_CHAR:
        // GCS_RESULTSTR already delivered the text; WM_IME_CHAR would double it.
        return search_.IsOpen() ? 0 : DefWindowProcW(host_.hwnd, message, wparam, lparam);
      case WM_KEYDOWN: {
        // CARD C25: the key chain that used to live here (a dozen ifs next to
        // the pixel work) is now PrismCore's table. This case does ONE thing:
        // read the thread's modifier state, describe the stroke, and hand it to
        // the single router that also serves DispatchKeyStroke.
        // CARD V2: the band gets first refusal on the keys it owns (Tab ring,
        // Enter on a focused button, arrows inside a menu). Only keys it
        // declines fall through to the gallery table below, so no existing
        // chord changes meaning.
        // C62: an open search field owns every key on this path; WM_CHAR is
        // the text route, so a letter can never fall through to DeleteSelected.
        if (search_.IsOpen()) return HandleSearchKey(static_cast<UINT>(wparam));
        if (HandleActionBarKey(static_cast<UINT>(wparam))) return 0;
        prism::keys::Stroke stroke;
        stroke.key = static_cast<uint32_t>(wparam);
        stroke.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        stroke.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        stroke.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
        return HandleKey(stroke);
      }
      case WM_MOUSEWHEEL: {
        // Target scroll follows the wheel; the spring makes the motion fluid.
        const double lines = static_cast<short>(GET_WHEEL_DELTA_WPARAM(wparam)) /
                             static_cast<double>(WHEEL_DELTA);
        scrollTarget_ += lines * kWheelScrollPixels;
        viewport_.SetScroll(scrollTarget_);
        scrollSpring_.Retarget(viewport_.Scroll(), NowSeconds());
        Invalidate();
        return 0;
      }
      case WM_DROPFILES: {
        // C52: Explorer hands over an HDROP (a HGLOBAL the shell allocated).
        // The model decides; the window paints and forwards. Returning 0 CLAIMS
        // the drop -- the shell frees nothing on our behalf, which is why
        // EvaluateDrop calls DragFinish.
        EvaluateDrop(reinterpret_cast<HDROP>(wparam));
        return 0;
      }
      case kThumbsChanged:
        Invalidate();
        return 0;
      case kArchiveChanged: {
        // UI thread: the app opens the archive the payload names and rebuilds
        // the view. Ownership of the string came with the message.
        std::unique_ptr<std::wstring> path(reinterpret_cast<std::wstring*>(lparam));
        if (onArchiveChanged_ && path) onArchiveChanged_(*path);
        return 0;
      }
      case kOperationFinished: {
        std::unique_ptr<std::wstring> status(reinterpret_cast<std::wstring*>(lparam));
        EndOperation();
        if (status) SetStatus(*status);
        return 0;
      }
      case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(host_.hwnd, &ps);
        ++paintMessages_;  // C52: paint evidence for the drop-hint suite
        AdvanceScrollSpring();
        Render();
        EndPaint(host_.hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;  // D2D paints the whole client area
      // CARD V2: an Alt chord never reaches TranslateMessage as a plain key --
      // it arrives as WM_SYSKEYDOWN. That is the ONLY door an Alt mnemonic can
      // come through, which is why the window-level test dispatches THIS
      // message instead of setting a synthetic Alt state.
      case WM_SYSKEYDOWN: {
        if (!palette_.IsOpen()) {
          const UINT vk = static_cast<UINT>(wparam);
          if (vk >= 'A' && vk <= 'Z') {
            const prism::actionbar::Effect effect =
                actionBar_.PressMnemonic(static_cast<char>(vk));
            if (effect.action != prism::actionbar::ActionId::Count) {
              ApplyActionEffect(effect);
              Invalidate();
              return 0;  // claimed: a disabled action still owns its chord
            }
          }
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_MOUSEMOVE: {
        // C67: a press that travels far enough becomes a DRAG OUT of the
        // selection (Explorer's own threshold, SM_CXDRAG). The handler blocks
        // inside DoDragDrop until the user lets go; the app extracts lazily, so
        // merely moving the mouse costs nothing.
        if (dragArmed_ && (wparam & MK_LBUTTON) != 0 && onDragOut_) {
          const int dx = MouseX(lparam) - dragPressX_;
          const int dy = MouseY(lparam) - dragPressY_;
          const int threshold = ::GetSystemMetrics(SM_CXDRAG);
          if (dx * dx + dy * dy >= threshold * threshold) {
            dragArmed_ = false;
            quickLookClose();
            const std::vector<uint32_t> selection = SelectedCatalogIndices();
            if (!selection.empty() && onDragOut_) (void)onDragOut_(host_.hwnd, selection);
            return 0;
          }
        }
        HandleTabMouseMove(MouseX(lparam), MouseY(lparam));
        const int index = ActionBarHit(MouseX(lparam), MouseY(lparam));
        if (index != actionHover_) {
          actionHover_ = index;
          Invalidate();
        }
        if (!actionTracking_ && host_.hwnd) {
          TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, host_.hwnd, 0};
          actionTracking_ = TrackMouseEvent(&track) != FALSE;
        }
        return 0;
      }
      case WM_MOUSELEAVE: {
        actionTracking_ = false;
        if (actionHover_ != -1) {
          actionHover_ = -1;
          Invalidate();
        }
        return 0;
      }
      case WM_LBUTTONUP: {
        dragArmed_ = false;  // C67: the press ended without becoming a drag
        // C60: a release that ends a tab press/drag owns the event; the tab was
        // already activated on the press (that is what makes a drag start).
        if (HandleTabMouseUp(MouseX(lparam), MouseY(lparam))) return 0;
        if (actionPressed_ != -1) {
          actionPressed_ = -1;
          Invalidate();
          return 0;
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_LBUTTONDBLCLK: {
        // A double-click on a tab is still a tab click: the strip gets first
        // refusal so the second WM_LBUTTONDOWN of the pair cannot select a row.
        if (TabBarTabAt(MouseX(lparam), MouseY(lparam)) >= 0) return 0;
        // C67: Ctrl/Shift arrive in wparam; Alt is read from the real key state
        // (and is the one modifier PrismCore turns into "extract", not "open").
        DoubleClickAt(MouseX(lparam), MouseY(lparam), (wparam & MK_CONTROL) != 0,
                      (wparam & MK_SHIFT) != 0);
        return 0;
      }
      case WM_MBUTTONDOWN: {
        // C60: middle-click closes the tab under the pointer (C59's rule: a
        // dirty tab refuses and the C54 box asks).
        const int index = TabBarTabAt(MouseX(lparam), MouseY(lparam));
        if (index >= 0) {
          RequestCloseTab(index);
          return 0;
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_LBUTTONDOWN: {
        // C62: the field's clear button is a real hit target derived from the
        // same instructions that were painted -- no second copy of the layout.
        if (HandleSearchClick(MouseX(lparam), MouseY(lparam))) return 0;
        // C52: a click anywhere dismisses a drop hint that is still on screen
        // (the app clears it as soon as it acts; this is the "user changed
        // their mind" path, so an overlay can never become permanent chrome).
        if (dropHintVisible_) ClearDropHint();
        // C60: the strip is chrome ABOVE every row (and above the band's own
        // baseline), so a click on a tab -- or on the strip's background -- is
        // consumed before the action band or the grid can see it.
        if (HandleTabMouseDown(MouseX(lparam), MouseY(lparam))) return 0;
        // C-V2: the band is chrome ABOVE the rows: a button click must never
        // also select the tile that happens to be behind it.
        if (ActionBarClick(MouseX(lparam), MouseY(lparam))) return 0;
        ClickAt(MouseX(lparam), MouseY(lparam), (wparam & MK_CONTROL) != 0,
                (wparam & MK_SHIFT) != 0);
        return 0;
      }
      case kProgressMessage: {
        hud_.OnProgress(NowMs(), static_cast<uint64_t>(wparam),
                        static_cast<uint64_t>(lparam));
        Invalidate();
        return 0;
      }
      case kHudStart: {
        // Ownership transferred with the message: take it and free it. Even on
        // the discard paths below, the pointer must be deleted exactly once.
        std::unique_ptr<std::string> headline(reinterpret_cast<std::string*>(lparam));
        if (headline) hud_.OnStart(*headline, NowMs());
        Invalidate();
        return 0;
      }
      case kStatusText: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        if (text) SetStatus(*text);
        return 0;
      }
      case WM_COMMAND: {
        // Menu items only (the gallery owns no child controls). C53: the menu
        // bar carries Settings... (C31/V3) -- a MODAL surface, so it goes through
        // the one-overlay gate BEFORE the dialog runs. Every other id is
        // forwarded verbatim: mapping an id to an ACTION is the app's job.
        const UINT command = LOWORD(wparam);
        if (command == prism::win::settingsui::SettingsDialog::kMenuSettingsId) {
          RunSettingsCommand();
          return 0;
        }
        // C57: Help > About. The dialog is modal and owns nothing this window
        // paints, so it is opened directly (not behind the overlay gate).
        if (command == prism::win::about::AboutDialog::kMenuAboutId) {
          RunAboutCommand();
          return 0;
        }
        // C54: the Edit menu is the window's own door (it derives the intent from
        // its selection), so it never reaches the app's menu switch.
        if (command == kMenuEditDelete) {
          RequestEdit(prism::win::editui::EditAction::Delete);
          return 0;
        }
        if (command == kMenuEditRename) {
          RequestEdit(prism::win::editui::EditAction::Rename);
          return 0;
        }
        if (command == kMenuEditNewFolder) {
          RequestEdit(prism::win::editui::EditAction::NewFolder);
          return 0;
        }
        if (onMenuCommand_) onMenuCommand_(command);
        return 0;
      }
      case WM_CLOSE:
        // Closing is a two-step contract, and both steps belong in one place:
        //   1. capture the frame size the user chose, so the app can persist it
        //      (the HWND is gone after step 2, and Main.cpp reads this only
        //      after Run() returns);
        //   2. DestroyWindow -> WM_DESTROY -> PostQuitMessage, which is what
        //      makes Run() return (default DefWindowProc does the same thing;
        //      naming it keeps the contract readable and testable).
        // Main.cpp then cancels the scan and joins the workers. The HWND is
        // destroyed here and NOT again in the destructor, which checks IsWindow
        // first for exactly that reason.
        if (host_.hwnd && IsWindow(host_.hwnd)) {
          // C66: the frame the USER sees, not the shadow-inflated window rect
          // (and the normal rectangle when the window is maximized -- a
          // maximized GetWindowRect is the work area plus the border, which no
          // screen can ever hold again).
          const prism::layout::Rect frame = prism::win::frame::FrameToPersist(host_.hwnd);
          closedWidth_ = frame.Width();
          closedHeight_ = frame.Height();
          DestroyWindow(host_.hwnd);
        }
        return 0;
      case WM_DESTROY:
        if (idleTimer_ && host_.hwnd) {
          KillTimer(host_.hwnd, kIdleTimerId);
          idleTimer_ = false;
        }
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
    }
  }

  // ---------- card C60: the tab strip's private half ------------------------
  std::string ActiveTabPath() const {
    const prism::tabs::Tab* tab = tabs_.active();
    return tab != nullptr ? tab->path : std::string();
  }

  std::wstring TabDisplayName(int index) const {
    const prism::tabs::Tab* tab = tabs_.At(index);
    return tab != nullptr ? Utf8ToWide(tab->displayName) : std::wstring();
  }

  // The view the painter and the hit tests both read. Built on demand (<=16
  // tabs, no allocation worth caching) so it cannot go stale against the
  // TabSet -- a cached copy is how a strip starts painting a tab that is gone.
  prism::win::tabbar::View TabViewNow() const {
    prism::win::tabbar::View view;
    view.items.reserve(tabs_.size());
    for (const prism::tabs::Tab& tab : tabs_.tabs()) {
      prism::win::tabbar::Item item;
      item.title = Utf8ToWide(tab.displayName);
      item.dirty = tab.dirty;
      view.items.push_back(std::move(item));
    }
    view.active = tabs_.activeIndex();
    view.hover = tabHover_;
    view.hoverClose = tabHoverClose_;
    view.pressedClose = tabPressedClose_;
    view.dragFrom = tabDragActive_ ? tabDragFrom_ : -1;
    view.dropIndex = tabDragActive_ ? tabDropSlot_ : -1;
    return view;
  }

  // Every mutation ends here: the band may have appeared/disappeared (a close
  // of the last tab frees its 32 px), and the APP is told which archive is now
  // active -- but only when the answer CHANGED. Closing an inactive tab must
  // not re-open the archive that is already on screen (that would reset the
  // user's folder for no reason).
  void AfterTabsChanged(const std::string& activeBefore) {
    const std::string activeAfter = ActiveTabPath();
    RecomputeLayout();
    Invalidate();
    if (activeAfter == activeBefore) return;
    if (activeAfter.empty()) {
      ClearArchiveView();  // the empty state: the window stays up
      if (onTab_) onTab_(std::string());
      return;
    }
    if (onTab_) onTab_(activeAfter);
  }

  // "No archive open" is a STATE, not an exit (C59's CloseOutcome::emptyState).
  // The view is cleared through the same model objects a real open feeds, so
  // the grid, the sidebar and the action band all read "no archive" instead of
  // showing the closed archive's rows.
  void ClearArchiveView() {
    catalog_ = nullptr;
    archiveOpen_ = false;
    nav_.Attach(nullptr);
    selection_ = -1;
    sel_.SetCount(0);
    sidebar_.AttachCatalog(nullptr);
    folderItems_.clear();
    folderRowOfCatalog_.clear();
    listItems_.clear();
    listRowToFolder_.clear();
    folderToListRow_.clear();
    history_.Clear();
    RebuildBreadcrumbText();
    SyncActionContext();
    RecomputeLayout();
    Invalidate();
  }

  // C54's confirmation box for a dirty tab. Non-modal on purpose: the window
  // never blocks its own message loop (C54's dialog completes through its
  // result handler), and the close it authorises runs through the SAME
  // CloseTab(index, true) an injected confirmer would take.
  void ShowTabCloseDialog(int index, const std::wstring& name) {
    if (!host_.hwnd) return;
    auto dialog = std::make_unique<prism::win::editui::EditConfirmDialog>();
    const HINSTANCE instance =
        reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(host_.hwnd, GWLP_HINSTANCE));
    if (!dialog->Create(instance, L"PrismArchive")) {
      // No dialog possible (headless): refuse rather than discard silently --
      // losing the user's edits is the one outcome this path must never take.
      ++tabCloseRefusals_;
      return;
    }
    dialog->SetModel(TabCloseModel(name));
    dialog->SetResultHandler([this, index](const prism::win::editui::EditDialogResult& result) {
      const bool accepted = prism::win::editui::EditMayProceed(result);
      tabCloseDialog_.reset();
      if (!accepted) {
        ++tabCloseRefusals_;
        SetStatus(L"\u5DF2\u53D6\u6D88\u5173\u95ED\u6807\u7B7E");  // 已取消关闭标签
        Invalidate();
        return;
      }
      CloseTab(index, true);
    });
    tabCloseDialog_ = std::move(dialog);
    tabCloseDialog_->Show();
  }

  // One pointer press inside the strip. Returns true when the strip consumed
  // it -- a tab click must never also select the tile painted underneath.
  bool HandleTabMouseDown(int x, int y) {
    if (!TabBarVisible() || layout_.tabBar.Empty()) return false;
    const std::vector<prism::win::tabbar::TabGeometry> tabs = TabBarRects();
    const int close = prism::win::tabbar::CloseAt(layout_.tabBar, tabs, x, y);
    if (close >= 0) {
      tabPressedClose_ = close;
      Invalidate();
      (void)RequestCloseTab(close);
      return true;
    }
    const int index = prism::win::tabbar::TabAt(layout_.tabBar, tabs, x, y);
    if (index < 0) {
      // The strip's own background/insertion area: still the strip's click.
      return x >= layout_.tabBar.left && x < layout_.tabBar.right && y >= layout_.tabBar.top &&
             y < layout_.tabBar.bottom;
    }
    ActivateTab(index);
    // A press on a tab may become a DRAG. The slot is armed at the tab's own
    // position so a press with no movement drops nowhere (Move(from, from)).
    tabDragPending_ = index;
    tabDragFrom_ = index;
    tabDropSlot_ = index;
    tabDragActive_ = false;
    tabDragOrigin_.x = x;
    tabDragOrigin_.y = y;
    if (host_.hwnd) SetCapture(host_.hwnd);
    return true;
  }

  // Motion: arm the reorder marker once the pointer has moved past the slop,
  // and keep the hover state the painter uses.
  void HandleTabMouseMove(int x, int y) {
    if (!TabBarVisible() || layout_.tabBar.Empty()) return;
    const std::vector<prism::win::tabbar::TabGeometry> tabs = TabBarRects();
    const int hover = prism::win::tabbar::TabAt(layout_.tabBar, tabs, x, y);
    const int hoverClose = prism::win::tabbar::CloseAt(layout_.tabBar, tabs, x, y);
    if (hover != tabHover_ || hoverClose != tabHoverClose_) {
      tabHover_ = hover;
      tabHoverClose_ = hoverClose;
      Invalidate();
    }
    if (tabDragPending_ < 0) return;
    const int dx = x - tabDragOrigin_.x;
    const int dy = y - tabDragOrigin_.y;
    const int slop = prism::win::tabbar::Metrics::kDragSlop;
    if (!tabDragActive_ && dx * dx + dy * dy < slop * slop) return;
    tabDragActive_ = true;
    tabDropSlot_ = prism::win::tabbar::InsertSlot(layout_.tabBar, tabs, x);
    Invalidate();
  }

  // Release: a drag that moved applies the reorder; a press that did not is
  // just the activation the press already performed.
  bool HandleTabMouseUp(int x, int y) {
    const bool wasDragging = tabDragActive_;
    const int from = tabDragFrom_;
    const int slot = tabDropSlot_;
    const bool hadPending = tabDragPending_ >= 0;
    tabDragPending_ = -1;
    tabDragFrom_ = -1;
    tabDropSlot_ = -1;
    tabDragActive_ = false;
    if (host_.hwnd) ReleaseCapture();
    if (!hadPending) return false;
    if (wasDragging && from >= 0 && slot >= 0 &&
        prism::win::tabbar::MoveTarget(from, slot) != from) {
      DropTab(from, slot);
    }
    (void)x;
    (void)y;
    Invalidate();
    return true;
  }

  // Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+W get first refusal, exactly like the
  // action band does: the chords are the strip's, and a stroke the strip
  // declines still reaches C25's table unchanged.
  bool HandleTabKey(const prism::keys::Stroke& stroke) {
    if (tabs_.empty()) return false;
    constexpr std::uint32_t kTabKey = 0x09;  // VK_TAB
    constexpr std::uint32_t kKeyW = 'W';
    if (stroke.ctrl && !stroke.alt && stroke.key == kTabKey) {
      CycleTabs(stroke.shift ? -1 : +1);
      return true;
    }
    if (stroke.ctrl && !stroke.shift && !stroke.alt && stroke.key == kKeyW) {
      RequestCloseTab(tabs_.activeIndex());
      return true;
    }
    return false;
  }

  // Signed client coordinates: a drag can deliver negative values.
  static int MouseX(LPARAM lparam) { return static_cast<short>(LOWORD(lparam)); }
  static int MouseY(LPARAM lparam) { return static_cast<short>(HIWORD(lparam)); }

  // ---------- card C53: the one-surface gate --------------------------------
  // Every OPEN of a modal surface funnels through here: "keep" survives, the
  // other three are dismissed. The assertion is the enforcement point the suite
  // reads back in Release builds too (NDEBUG removes assert()).
  void AssertSingleOverlay() {
    if (SingleOverlayInvariant()) return;
    ++overlayViolations_;
    assert(false && "C53: at most one modal overlay may be active");
  }

  void DismissOtherOverlays(ActiveOverlay keep) {
    if (kOverlayExclusivityEnforced) {
      if (keep != ActiveOverlay::DropHint) ClearDropHint();
      if (keep != ActiveOverlay::Palette) {
        overlay_.Close();
        palette_.Close();
      }
      if (keep != ActiveOverlay::QuickLook) quickLookClose();
    }
    AssertSingleOverlay();
  }

  // C53: the Settings... menu item. A dialog is a modal surface, so it goes
  // through the gate before the message loop is entered and reports itself for
  // the whole time the gate runs. The launcher defaults to the real dialog with
  // the real (default) settings file -- Main installs one that uses the paths
  // the app actually loaded.
  void RunSettingsCommand() {
    DismissOtherOverlays(ActiveOverlay::Settings);
    settingsModalOpen_ = true;
    AssertSingleOverlay();
    if (settingsLauncher_) {
      settingsLauncher_();
    } else {
      prism::win::settingsui::SettingsDialog dialog;
      const prism::win::settingsfile::Paths paths =
          prism::win::settingsfile::DefaultPaths();
      dialog.SetPaths(paths);
      std::string error;
      dialog.LoadFrom(paths, &error);  // before Create(): Create syncs the draft
      dialog.OpenModal(GetModuleHandleW(nullptr), host_.hwnd);
    }
    settingsModalOpen_ = false;
    AssertSingleOverlay();
    Invalidate();
  }

  // C57: Help > About. Deliberately NOT behind the one-overlay gate: the gate
  // exists because two PAINTED surfaces would compete for Esc and for the user's
  // eye, and this dialog is modal (it disables the owner and runs its own message
  // loop), so nothing else can open while it is up. It owns no window state
  // either -- the report is a snapshot taken when it opens.
  void RunAboutCommand() {
    prism::win::about::AboutDialog dialog;
    dialog.OpenModal(GetModuleHandleW(nullptr), host_.hwnd);
    Invalidate();
  }

  // ---------- the action band's wiring (card V2) ----------
  // What a click or a chord MEANS is the model's decision, not this file's: the
  // Effect is already "invoke / open menu / move focus". This only translates
  // it into "call the app" and "repaint".
  void ApplyActionEffect(const prism::actionbar::Effect& effect) {
    using prism::actionbar::Effect;
    switch (effect.kind) {
      case Effect::Kind::Invoke:
        if (effect.enabled) FireAction(effect.action, effect.item);
        return;
      case Effect::Kind::None:
      case Effect::Kind::OpenDropdown:
      case Effect::Kind::CloseDropdown:
      case Effect::Kind::MoveFocus:
      case Effect::Kind::MoveDropdown:
        return;
    }
  }

  // The ONE exit of the band: the app owns what "extract" or "test" does, so a
  // test can count commands without starting an extraction.
  void FireAction(prism::actionbar::ActionId action, int item) {
    if (onAction_) {
      onAction_(action, item);
      return;
    }
    // C64: an action with NO app handler is a DEAD BUTTON, not a quiet success.
    // Before this card Main never called SetActionHandler, so all eight buttons
    // (and their Alt chords) did nothing at all. Say it on stderr so the process
    // suite can catch the missing wiring; the UI status line says it too.
    std::fprintf(stderr, "action unhandled id=%s item=%d\n",
                 prism::actionbar::ActionName(action), item);
    SetStatus(std::wstring(L"\u52A8\u4F5C\u672A\u63A5\u7EBF\uFF1A") +
              Utf8ToWide(prism::actionbar::ActionName(action)));
  }

  // Tab / Enter / arrows / Esc -- but only for the keys the band actually owns:
  // a declined key returns false and the gallery table keeps its meaning.
  bool HandleActionBarKey(UINT vk) {
    using prism::actionbar::Effect;
    switch (vk) {
      case VK_TAB:
        ApplyActionEffect(actionBar_.PressTab((GetKeyState(VK_SHIFT) & 0x8000) == 0));
        return true;  // the ring always moves: that IS the traversal
      case VK_RETURN:
        if (actionBar_.FocusIndex() < 0 && !actionBar_.DropdownOpen()) return false;
        ApplyActionEffect(actionBar_.PressEnter());
        return true;
      case VK_DOWN: {
        const Effect effect = actionBar_.PressDown();
        if (effect.Dropped()) return false;  // arrows still navigate the grid
        ApplyActionEffect(effect);
        return true;
      }
      case VK_UP: {
        const Effect effect = actionBar_.PressUp();
        if (effect.Dropped()) return false;
        ApplyActionEffect(effect);
        return true;
      }
      case VK_ESCAPE:
        // Esc leaves the band (menu first, then the ring); with neither up the
        // key belongs to Quick Look / the gallery exactly as before.
        if (!actionBar_.DropdownOpen() && actionBar_.FocusIndex() < 0) return false;
        ApplyActionEffect(actionBar_.PressEscape());
        return true;
      default:
        return false;
    }
  }

  prism::actionbar::Layout BandLayout() const {
    return prism::actionbar::Compute(WindowLayoutForClient());
  }

  int ActionBarHit(int x, int y) const { return BandLayout().Hit(x, y); }

  // A press on a button: focus it, sink it, run its default variant -- unless
  // the pixel is the arrow zone of a dropdown action, which opens the menu.
  // Returning true means "the chrome consumed this click", including the
  // island's own padding, so the tile behind it is never selected by accident.
  bool ActionBarClick(int x, int y) {
    const prism::actionbar::Layout layout = BandLayout();
    const int index = layout.Hit(x, y);
    if (index < 0) {
      if (layout.island.Empty()) return false;
      const prism::actionbar::Rect& r = layout.island;
      return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
    }
    const prism::actionbar::Button& button = layout.buttons[static_cast<size_t>(index)];
    const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(index);
    actionBar_.SetFocusIndex(index);
    actionPressed_ = index;
    if (spec.hasDropdown && x >= button.rect.right - 16) {
      ApplyActionEffect(actionBar_.OpenDropdownFor(button.id));
    } else {
      const prism::actionbar::Effect effect = actionBar_.PressAction(button.id);
      if (effect.kind == prism::actionbar::Effect::Kind::Invoke && effect.enabled) {
        FireAction(effect.action, effect.item);
      }
    }
    Invalidate();
    return true;
  }

  // ---------- navigation plumbing ----------
  void ApplySelection(int selection) {
    selection_ = selection;
    // C67: a programmatic selection (arrow key, palette jump, restore) is ONE
    // row -- the keyboard is not cherry-picking. Click handlers go through
    // sel_.Click directly so Ctrl/Shift are never flattened here.
    sel_.SetSingle(selection);
    // C-V2: the selection IS the enable rule's input (Add/Delete/QuickLook).
    // One call site, so a row selected by a click, an arrow or the palette all
    // reach the band the same way.
    SyncActionContext();
    Invalidate();
  }

  // Derive the band's context from the state the window already owns. Called
  // from every mutation of "is there a catalog" / "is there a selection" so the
  // band can never show a stale legality.
  void SyncActionContext() {
    prism::actionbar::Context context;
    context.archiveOpen = catalog_ != nullptr;
    // C67: the band's rule is "something is selected", and the ordered set is
    // the ONLY authority for that -- a stale selection_ can no longer keep a
    // button enabled after the last selected row went away.
    context.hasSelection = !sel_.Empty() && selection_ >= 0 && selection_ < ItemCount();
    context.readOnly = readOnly_;
    actionBar_.SetContext(context);
  }

  // ---------- key routing (card C25) ----------
  // ONE router for everything: WM_KEYDOWN and DispatchKeyStroke both land here,
  // so the table cannot mean one thing to the app and another to the tests.
  prism::keys::Scope KeyScope() const {
    if (palette_.IsOpen()) return prism::keys::Scope::Palette;
    if (QuickLookOpen()) return prism::keys::Scope::QuickLook;
    return prism::keys::Scope::Gallery;
  }

  LRESULT HandleKey(const prism::keys::Stroke& stroke) {
    // C62: while the search field is open it owns the key route, including Esc
    // (clear, then close) and Enter (move to the results). The model decides
    // which stage a key is in; this router only forwards.
    if (search_.IsOpen()) return HandleSearchKey(stroke.key);
    // The palette owns the keyboard while it is open: it gets the stroke first
    // (that is how typing reaches the query field) and only a stroke it declines
    // is re-resolved against the gallery table.
    if (palette_.IsOpen()) {
      // C53: the OVERLAY gets the stroke before the shell (C31). It owns Tab
      // (scope) and resolves Up/Down/Enter/Esc against the SAME filtered rows it
      // paints. A stroke it consumes must NOT fall through to the shell or to the
      // gallery table -- Ctrl+K inside the palette would otherwise close it and
      // reopen it on the second half of the same key press.
      const prism::win::palette::PaletteOverlay::Outcome overlayOutcome = overlay_.OnKey(stroke);
      if (overlayOutcome.consumed) {
        if (overlayOutcome.activate) RunPalette();
        Invalidate();
        return 0;
      }
      const prism::win::palette::PaletteShell::KeyOutcome outcome = palette_.OnKey(stroke);
      if (outcome.consumed) {
        if (outcome.command == prism::keys::Command::ActivateSelection) RunPalette();
        Invalidate();
        return 0;
      }
    }
    // C60: the strip's chords (Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+W) are claimed
    // BEFORE the gallery table, the same way the action band claims its own.
    // A stroke the strip declines reaches C25's table unchanged, so no existing
    // chord changes meaning.
    if (HandleTabKey(stroke)) return 0;
    ExecuteCommand(prism::keys::Resolve(stroke, KeyScope()));
    return 0;
  }

  // Mapping a Command to the window's own state. Nothing here decides policy:
  // the label, the order and the modifiers all came from PrismCore.
  void ExecuteCommand(prism::keys::Command command) {
    const int count = ItemCount();
    switch (command) {
      case prism::keys::Command::None:
        return;
      case prism::keys::Command::QuickLookToggle:
        if (quickLook_ && selection_ >= 0 && selection_ < count) {
          // C53: Quick Look is a modal surface too; opening it dismisses the
          // drop hint and closes the palette.
          DismissOtherOverlays(ActiveOverlay::QuickLook);
          // C54: ...and an edit in flight owns the handler, so preview waits.
          if (!OperationAllowed(L"\u9884\u89C8")) return;  // 预览
          quickLook_->ToggleAt(static_cast<uint32_t>(selection_));
          AssertSingleOverlay();
          Invalidate();
        }
        return;
      case prism::keys::Command::QuickLookClose:
        if (quickLookClose()) Invalidate();
        return;
      case prism::keys::Command::OpenPalette:
        OpenPalette();
        return;
      case prism::keys::Command::OpenSearch:
        OpenSearch();
        return;
      case prism::keys::Command::ClosePalette:
        // C53: one close closes both halves (Esc reaching the shell directly is
        // still one surface going away, never a dead card left painted).
        overlay_.Close();
        palette_.Close();
        AssertSingleOverlay();
        Invalidate();
        return;
      case prism::keys::Command::SmartExtract:
        // Ctrl+E (blueprint 9.1 row 4). The window still does not know what
        // extraction means: it hands the CATALOG index to the app, exactly like
        // Enter on a file. Same door, same plan, one code path.
        if (selection_ >= 0 && selection_ < count && onActivate_) {
          if (!OperationAllowed(L"\u89E3\u538B")) return;  // 解压
          onActivate_(static_cast<int>(folderItems_[static_cast<size_t>(selection_)].index));
        }
        return;
      case prism::keys::Command::NewFolder:
        // C54: Ctrl+Shift+N is a REAL archive edit now (C51's CoW engine).
        RequestEdit(prism::win::editui::EditAction::NewFolder);
        return;
      case prism::keys::Command::DeleteSelected:
        RequestEdit(prism::win::editui::EditAction::Delete);
        return;
      case prism::keys::Command::RenameSelected:
        RequestEdit(prism::win::editui::EditAction::Rename);
        return;
      case prism::keys::Command::ZoomNext:
        // The ladder's arithmetic is C8's; the renderer has no tile-size input
        // yet, so the stop is computed and reported, not painted.
        zoomPx_ = prism::keys::zoom::NextPreset(zoomPx_);
        SetStatus(L"\u56FE\u6807 " + std::to_wstring(zoomPx_) + L" px");
        return;
      case prism::keys::Command::HistoryBack: {
        std::string target;
        if (history_.Back(&target)) GoToFolder(target);
        return;
      }
      case prism::keys::Command::HistoryForward: {
        std::string target;
        if (history_.Forward(&target)) GoToFolder(target);
        return;
      }
      case prism::keys::Command::ToggleGrid:
        SetListMode(false);
        return;
      case prism::keys::Command::ToggleList:
        SetListMode(true);
        return;
      case prism::keys::Command::SelectAll:
        // C67: every row of THIS folder, anchor at the top like Explorer, focus
        // on the last row so a following Shift+click measures from it.
        quickLookClose();
        sel_.SelectAll();
        selection_ = sel_.Focus();
        nav_.SetSelection(selection_);
        SyncActionContext();
        Invalidate();
        return;
      case prism::keys::Command::ActivateSelection:
        if (selection_ >= 0) ActivateRow(selection_);
        return;
      case prism::keys::Command::GoUp:
        Back();
        return;
      case prism::keys::Command::SelectFirst:
        quickLookClose();
        SetSelection(count ? 0 : -1);
        return;
      case prism::keys::Command::SelectLast:
        quickLookClose();
        SetSelection(count - 1);
        return;
      case prism::keys::Command::SelectLeft:
        MoveSelection(listMode_ ? 0 : -1);
        return;
      case prism::keys::Command::SelectRight:
        MoveSelection(listMode_ ? 0 : +1);
        return;
      case prism::keys::Command::SelectUp:
        MoveSelection(listMode_ ? -1 : -static_cast<int>(viewport_.Columns()));
        return;
      case prism::keys::Command::SelectDown:
        MoveSelection(listMode_ ? +1 : +static_cast<int>(viewport_.Columns()));
        return;
    }
  }

  // ---------- card C54: edit requests and the exclusive-operation gate ------
  //
  // The gate is ONE boolean because every long operation in this product touches
  // the same archive: a reader pool decode, the solid sweep, a re-pack or a CoW
  // rewrite. Two of them at once is not "parallelism", it is two writers on one
  // file (or a reader holding the handle ReplaceFileW needs). The window is the
  // only object both the UI and the workers already talk to, so it owns the flag
  // and the readable refusal; the app owns what each operation MEANS.
  //
  // CONTRACTS D26/D3: the refusal is compiled out by PRISM_C54_MUTANT_ALLOW_
  // CONCURRENT so the suite's "second operation is rejected" case can go red.
  bool OperationAllowed(const wchar_t* action) {
    if (!prism::win::editui::ConcurrencyGateActive()) return true;
    if (!operationBusy_) return true;
    RefuseOperation(action);
    return false;
  }

  static const wchar_t* OperationName(OperationKind kind) {
    switch (kind) {
      case OperationKind::Edit: return L"\u7F16\u8F91";      // 编辑
      case OperationKind::Extract: return L"\u89E3\u538B";   // 解压
      case OperationKind::Preview: return L"\u9884\u89C8";   // 预览
      case OperationKind::Compress: return L"\u538B\u7F29";  // 压缩
      case OperationKind::None: break;
    }
    return L"\u64CD\u4F5C";  // 操作
  }

  void RefuseOperation(const wchar_t* action) {
    ++refusedOperations_;
    std::wstring reason = L"\u64CD\u4F5C\u88AB\u62D2\u7EDD\uFF1A";  // 操作被拒绝：
    if (operationBusy_ && !operationLabel_.empty()) {
      reason += operationLabel_ + L" \u6B63\u5728\u8FDB\u884C";  // 正在进行
    }
    reason += L"\uFF0C\u4E0D\u80FD\u540C\u65F6";  // ，不能同时
    reason += action;
    lastOperationRefusal_ = reason;
    SetStatus(reason);
  }

  // Turn the selection into an archive-relative INTENT. The window never builds
  // an EditPlan (that is C51's job) and never writes (that is the app's).
  bool RequestEdit(prism::win::editui::EditAction action) {
    using prism::win::editui::EditAction;
    if (!archiveOpen_ || catalog_ == nullptr) {
      SetStatus(L"\u6CA1\u6709\u6253\u5F00\u7684\u5F52\u6863");  // 没有打开的归档
      return false;
    }
    if (!OperationAllowed(L"\u7F16\u8F91")) return false;  // 编辑 (a second edit)
    if (!editEnabled_) {
      SetStatus(editDisabledReason_.empty()
                    ? std::wstring(L"\u5F53\u524D\u5F52\u6863\u4E0D\u652F\u6301\u7F16"
                                   L"\u8F91")  // 当前归档不支持编辑
                    : editDisabledReason_);
      return false;
    }
    prism::win::editui::EditIntent intent;
    intent.action = action;
    if (action == EditAction::NewFolder) {
      intent.directory = nav_.Folder();
      intent.leaf = "\u65B0\u5EFA\u6587\u4EF6\u5939";
      intent.path = intent.directory.empty() ? intent.leaf
                                             : intent.directory + "/" + intent.leaf;
    } else {
      if (selection_ < 0 || selection_ >= ItemCount()) {
        SetStatus(L"\u8BF7\u5148\u9009\u4E2D\u4E00\u4E2A\u6761\u76EE");  // 请先选中一个条目
        return false;
      }
      const prism::ArchiveItem& item = folderItems_[static_cast<size_t>(selection_)];
      intent.path = item.path;
      intent.directory = FolderOf(item.path);
      intent.leaf = std::string(prism::FileNameOf(item.path));
      intent.isDir = item.isDir;
      // Deleting a folder means deleting its subtree ON PURPOSE; C51 refuses it
      // without recurse, and the confirmation dialog names the entry count.
      intent.recurse = item.isDir;
    }
    lastEdit_ = intent;
    if (onEdit_) onEdit_(intent);
    return true;
  }

  // Grid mode steps in folder space (one row per tile). List mode steps in VIEW
  // space, because the view is sorted/filtered; the selection itself stays a
  // folder row everywhere else, so the step is mapped there and back.
  void MoveSelection(int delta) {
    if (delta == 0) return;
    const int count = ItemCount();
    if (count <= 0) return;
    int next = selection_;
    if (listMode_) {
      int row = -1;
      if (selection_ >= 0 && selection_ < static_cast<int>(folderToListRow_.size())) {
        row = folderToListRow_[static_cast<size_t>(selection_)];
      }
      row = (row < 0) ? 0 : row + delta;
      if (row < 0 || row >= static_cast<int>(listRowToFolder_.size())) return;
      next = listRowToFolder_[static_cast<size_t>(row)];
    } else {
      next = selection_ + delta;
    }
    if (next < 0 || next >= count) return;
    quickLookClose();
    SetSelection(next);
  }

  // ---------- command palette (C8) ----------
  void OpenPalette() {
    // C53: Ctrl+K opens ONE modal surface. The drop hint and Quick Look are
    // dismissed first (a palette painted over a drop hint would leave two
    // overlays competing for Esc and for the user's eye).
    DismissOtherOverlays(ActiveOverlay::Palette);
    overlay_.Attach(&palette_);  // the shell owns the rectangles (C31/D7)
    palette_.Open();
    overlay_.Open();  // one open state, not two (the overlay closes the shell too)
    AssertSingleOverlay();
    Invalidate();
  }

  // Enter inside the palette. A COMMAND runs through the same ExecuteCommand
  // switch the keyboard uses; a FILE is navigated to (its folder + selection),
  // never extracted -- opening a payload is the app's decision, not the list's.
  void RunPalette() {
    // The overlay filters by scope, so its visible row -- not the unfiltered
    // shell highlight -- is what Enter must run (C53).
    const prism::palette::Entry* entry =
        overlay_.IsOpen() ? overlay_.Highlighted() : palette_.Highlighted();
    if (!entry) {
      palette_.Close();
      Invalidate();
      return;
    }
    if (entry->kind == prism::palette::Kind::Command) {
      const prism::keys::Command command = entry->command;
      palette_.Close();
      ExecuteCommand(command);
      Invalidate();
      return;
    }
    const uint32_t index = entry->index;
    palette_.Close();
    if (!catalog_) {
      Invalidate();
      return;
    }
    std::string folder;
    for (const prism::ArchiveItem& item : *catalog_) {
      if (item.index != index) continue;
      folder = item.isDir ? item.path : FolderOf(item.path);
      break;
    }
    if (!GoToFolder(folder)) {
      Invalidate();
      return;
    }
    for (size_t i = 0; i < folderItems_.size(); ++i) {
      if (folderItems_[i].index != index) continue;
      SetSelection(static_cast<int>(i));
      break;
    }
    Invalidate();
  }

  // "a/b.txt" -> "a" (the folder a hit lives in). Root entries yield "".
  static std::string FolderOf(std::string_view path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
  }

  // Navigate to a folder. The history entry is recorded by the caller path
  // (AfterFolderChange) so a jump does not manufacture a new one.
  bool GoToFolder(const std::string& folder) {
    const prism::tree::NavResult result = nav_.GoTo(folder);
    if (result.action != prism::tree::NavAction::FolderChanged) return false;
    ApplyFolderChange();
    return true;
  }

  // ---------- sidebar (C12 + C27) ----------
  // A click inside the painted rail. Returns true when the rail consumed it,
  // including the "inside the rail but between rows" case: letting that fall
  // through would deselect the grid row behind the panel.
  bool SidebarClick(int x, int y) {
    EnsureSidebar();
    const prism::win::sidebar::Box& panel = sidebarLayout_.panel;
    if (panel.Empty()) return false;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    if (fx < panel.left || fx >= panel.right) return false;
    if (fy < panel.top || fy >= panel.bottom) return false;
    for (const prism::win::sidebar::Command& command : sidebarLayout_.commands) {
      if (command.kind != prism::win::sidebar::PrimKind::RowHit) continue;
      if (fx < command.box.left || fx >= command.box.right) continue;
      if (fy < command.box.top || fy >= command.box.bottom) continue;
      const prism::sidebar::FocusTarget target{command.section, command.payload};
      lastSidebarHit_ = target;
      sidebar_.SetFocus(target);
      sidebarDirty_ = true;
      // Activating a favorite/recent row means "open this path": the app owns
      // what that means, so it goes through the same door as Enter on a row.
      if (command.section != prism::sidebar::Section::Tree) {
        sidebar_.SetFocus(target);
      }
      Invalidate();
      return true;
    }
    return true;  // inside the chrome: consumed, nothing to focus
  }

  // The real Windows menu bar, rebuilt from the labels the app supplied.
  void RebuildMenu() {
    if (!host_.hwnd) return;  // Create() installs it once the HWND exists
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    if (!bar || !file || !edit) {
      if (bar) DestroyMenu(bar);
      if (file) DestroyMenu(file);
      if (edit) DestroyMenu(edit);
      return;
    }
    AppendMenuW(file, MF_STRING, kMenuCompress, compressMenuLabel_.c_str());
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, kMenuExit, exitMenuLabel_.c_str());
    // C54's Edit menu. A disabled entry is GREY, and RequestEdit still answers
    // with the readable reason if a chord reaches it anyway.
    AppendMenuW(edit, MF_STRING, kMenuEditDelete, editDeleteLabel_.c_str());
    AppendMenuW(edit, MF_STRING, kMenuEditRename, editRenameLabel_.c_str());
    AppendMenuW(edit, MF_STRING, kMenuEditNewFolder, editNewFolderLabel_.c_str());
    if (!editEnabled_) {
      EnableMenuItem(edit, kMenuEditDelete, MF_BYCOMMAND | MF_GRAYED);
      EnableMenuItem(edit, kMenuEditRename, MF_BYCOMMAND | MF_GRAYED);
      EnableMenuItem(edit, kMenuEditNewFolder, MF_BYCOMMAND | MF_GRAYED);
    }
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileMenuLabel_.c_str());
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), editMenuLabel_.c_str());
    // C53: C31/V3's Settings item lives on the SAME bar, not on a second menu
    // that would REPLACE this one. (SettingsDialog::InstallMenu builds a whole
    // bar of its own; Main only falls back to it when the window has no menu.)
    AppendMenuW(bar, MF_STRING, prism::win::settingsui::SettingsDialog::kMenuSettingsId,
                settingsMenuLabel_.c_str());
    // C57: Help > About. The label is literal here like "Settings..." above: the
    // item is chrome on this window, and the dialog it opens is where the words
    // that matter (the report) live.
    HMENU help = CreatePopupMenu();
    if (help != nullptr) {
      AppendMenuW(help, MF_STRING, prism::win::about::AboutDialog::kMenuAboutId,
                  aboutMenuLabel_.c_str());
      AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), helpMenuLabel_.c_str());
    }
    HMENU previous = GetMenu(host_.hwnd);
    if (!SetMenu(host_.hwnd, bar)) {
      DestroyMenu(bar);
      return;
    }
    if (previous) DestroyMenu(previous);
    DrawMenuBar(host_.hwnd);
  }

  void EnsureSidebar() const {
    if (!sidebarDirty_) return;
    prism::win::sidebar::Metrics metrics;
    // V1: the rail is exactly the layout's sidebar band -- 220 px (DESIGN.md),
    // starting below the action island and ending above the status bar.
    metrics.width = static_cast<float>(layout_.sidebarVisible ? layout_.sidebar.Width()
                                                              : prism::layout::kSidebarWidth);
    metrics.viewportTop = static_cast<float>(layout_.content.top);
    metrics.viewportBottom = static_cast<float>(layout_.content.bottom);
    metrics.scrollY = sidebarScroll_;
    sidebarLayout_ = prism::win::sidebar::Build(sidebar_, metrics);
    sidebarDirty_ = false;
  }

  bool CrumbClick(int x, int y) {
    const int level = CrumbLevelAt(x, y);
    if (level == prism::tree::kNoLevel) return false;
    const prism::tree::NavResult result = nav_.GoToLevel(level);
    if (result.action == prism::tree::NavAction::FolderChanged) {
      AfterFolderChange();
    }
    // A click INSIDE the bar never falls through to the grid behind it, even
    // when it only re-selects the crumb we are already on.
    return true;
  }

  // One place where a folder transition lands, whatever caused it.
  //
  // Two steps on purpose (C8): every USER navigation is recorded in the history
  // stack, while a history jump itself must not create a new entry (it would
  // make Alt+Right unreachable). HistoryStack de-dupes adjacent repeats, so the
  // record call is harmless on the jump path too.
  void AfterFolderChange() {
    ApplyFolderChange();
    history_.Visit(nav_.Folder());
  }

  // Refresh what a folder transition changes, WITHOUT recording history.
  void ApplyFolderChange() {
    quickLookClose();          // the preview belongs to the old folder
    RebuildFolderView();
  }

  // Re-filter the rows, rebuild the layout, and put the viewport back at the
  // top (blueprint: a new folder always starts at its first row). The
  // selection comes from the navigator's restore rule.
  void RebuildFolderView() {
    folderItems_ = nav_.FolderItems();
    host_.items = &folderItems_;
    quickLook_.reset();
    quickLook_.emplace(folderItems_);
    // Index the folder rows by CATALOG index: the list view is a sorted subset,
    // and the selection is a folder row, so this is the one place the two
    // numberings are joined.
    folderRowOfCatalog_.clear();
    folderRowOfCatalog_.reserve(folderItems_.size());
    for (size_t i = 0; i < folderItems_.size(); ++i) {
      folderRowOfCatalog_.emplace(folderItems_[i].index, static_cast<int>(i));
    }
    RebuildListItems();
    RebuildLayout();
    selection_ = nav_.Selection();
    // C67: a new folder is a new row space -- the old rows would address
    // different entries, so the set is rebuilt, never remapped.
    sel_.SetCount(ItemCount());
    sel_.SetSingle(selection_);
    SyncActionContext();  // the folder changed: the selection may have been cleared
    scrollTarget_ = 0.0;
    viewport_.SetScroll(0.0);
    scrollSpring_ = SpringAnimator(kScrollSpring, 0.0);  // snap, no glide back
    RebuildBreadcrumbText();
    Invalidate();
  }

  void AdvanceScrollSpring() {
    const double now = NowSeconds();
    scrollSpring_.Tick(now);
    if (!scrollSpring_.IsSettled()) {
      viewport_.SetScroll(scrollSpring_.Value());
      Invalidate();  // keep the loop alive until the motion settles
    }
  }

  int ItemAtPoint(int x, int y) {
    if (clientW_ <= 0.0 || clientH_ <= 0.0) return -1;  // no WM_SIZE yet
    // V4: the rail owns the left column in BOTH view modes (it is chrome, not
    // list-mode chrome), and the hit test is y-only in list mode -- one guard.
    if (SidebarInset() > 0.0f && x < static_cast<int>(SidebarInset())) {
      return -1;
    }
    if (listMode_) {
      // The list paints the VIEW, not the folder: filter first, then hit-test.
      const int row = prism::layout::ListRowAt(layout_, x, y,
                                               static_cast<int>(viewport_.Scroll()));
      if (row < 0 || row >= static_cast<int>(listRowToFolder_.size())) return -1;
      return listRowToFolder_[static_cast<size_t>(row)];
    }
    // V4: hit-testing and painting consume the SAME rectangles. The grid is a
    // FLOW now (tiles in rows, tile-less items as full-width bands), so the
    // question "which item is under this point" is answered by the flow and not
    // by a divide-by-column-width that no longer describes the paint.
    if (x < layout_.content.left || x >= layout_.content.right ||
        y < layout_.contentInner.top || y >= layout_.contentInner.bottom) {
      return -1;
    }
    const std::vector<prism::layout::FlowItem> flow = FlowInputs();
    return prism::layout::FlowItemAt(layout_, flow, x, y,
                                     static_cast<int>(viewport_.Scroll()));
  }

  // V4: the classification FACTS for the current folder, in catalog order. Only
  // the ThumbStore knows whether a photo really has pixels; the geometry is the
  // layout's job. One function feeds painting, hit-testing and the test seam.
  std::vector<prism::layout::FlowItem> FlowInputs() {
    std::vector<prism::layout::FlowItem> flow;
    flow.reserve(folderItems_.size());
    std::lock_guard<std::mutex> lock(thumbMutex_);
    for (const prism::ArchiveItem& item : folderItems_) {
      prism::layout::FlowItem entry;
      entry.isDir = item.isDir;
      entry.kind = prism::QuickLookPlanner::KindByName(item.path);
      entry.thumbnailAvailable = thumbs_ != nullptr &&
                                 prism::layout::ThumbnailCapable(entry.kind) &&
                                 thumbs_->Contains(item.index);
      flow.push_back(entry);
    }
    return flow;
  }

  static std::wstring Utf8ToWide(std::string_view text) {
    if (text.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), wide);
    return out;
  }

  static std::string WideToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    const int narrow = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    std::string out(static_cast<size_t>(narrow), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), narrow, nullptr, nullptr);
    return out;
  }

  // ---------- breadcrumb ----------
  // The breadcrumb text AND its clickable spans come from the same segment
  // list; widths are measured with the same format the renderer paints, so a
  // click lands on the text the user sees.
  void RebuildBreadcrumbText() {
    crumbSegments_ = nav_.Segments(WideToUtf8(rootLabel_));
    if (!status_.empty()) {
      crumbSegments_.push_back({std::string(prism::tree::kBreadcrumbSeparator),
                                prism::tree::kNoLevel});
      crumbSegments_.push_back({WideToUtf8(status_), prism::tree::kNoLevel});
    }
    breadcrumbText_ = Utf8ToWide(prism::tree::JoinBreadcrumb(crumbSegments_));
    UpdateBreadcrumbLayout();
  }

  void UpdateBreadcrumbLayout() {
    EnsureNavIsland();
    const auto width = [this](std::string_view text) { return MeasureUtf8(text); };
    crumbLayout_ = prism::tree::LayoutCenteredBreadcrumb(
        crumbSegments_, textLeft_, textRight_, width);
  }

  // Mirrors GalleryRenderer::DrawBreadcrumbIsland's fallback geometry (the
  // window never supplies scene.islands): a 0.62 * width pill, centred, 44 px
  // tall at y=16, text inset 24 px. Kept here because the hit test has to
  // agree with the painted box, and the renderer owns no public accessor.
  void EnsureNavIsland() const {
    if (!barKnown_) const_cast<GalleryWindow*>(this)->MeasureNavIsland();
  }

  // The ONE window layout, exactly as the renderer computes it (V1). Bands, grid
  // metrics and the breadcrumb pill come from here, so the band's hit test and
  // the painted band are the same rectangle by construction.
  prism::layout::Layout WindowLayoutForClient() const {
    int width = static_cast<int>(clientW_);
    int height = static_cast<int>(clientH_);
    if (width <= 0) width = 1280;  // headless default (pre-Create)
    if (height <= 0) height = 800;
    // V4: the rail is chrome (WindowLayout::RailVisible), so it reserves width
    // in BOTH views; only an explicit collapse or a too-narrow window removes it.
    const bool rail = prism::layout::RailVisible(sidebarRailVisible_, listMode_) &&
                      GalleryRenderer::SidebarFits(static_cast<float>(width));
    return prism::layout::Compute(width, height, rail, listMode_);
  }

  void MeasureNavIsland() {
    if (clientW_ <= 0.0) {
      RECT rc{};
      if (host_.hwnd) GetClientRect(host_.hwnd, &rc);
      clientW_ = static_cast<double>(rc.right);
      clientH_ = static_cast<double>(rc.bottom);
    }
    // V1: the pill and its text run come from the layout, so the clickable span
    // is by construction the painted span.
    const prism::layout::Layout layout = WindowLayoutForClient();
    // C-V2: the SPLIT pill. The toolbar may have taken width back from it, and
    // the renderer paints this same rectangle -- so the crumb the user clicks is
    // by construction the crumb they see.
    const prism::actionbar::Layout band = prism::actionbar::Compute(layout);
    const prism::layout::Rect pill = band.navPill;
    const prism::layout::Rect textRect = prism::actionbar::NavTextRect(band);
    barTop_ = pill.top;
    barBottom_ = pill.bottom;
    textLeft_ = textRect.left;
    textRight_ = textRect.right;
    barKnown_ = true;
  }

  // DirectWrite measurement in the renderer's crumb format (Segoe UI 15 px
  // semi-bold). Without a factory every width is 0 -> no crumb is clickable,
  // which degrades to "Backspace still works" instead of navigating blind.
  int MeasureUtf8(std::string_view text) {
    EnsureTextMeasurement();
    if (!crumbFormat_) return 0;
    const std::wstring wide = Utf8ToWide(text);
    if (wide.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(writeFactory_->CreateTextLayout(wide.c_str(),
                                               static_cast<UINT32>(wide.size()),
                                               crumbFormat_.Get(), 8192.0f, 64.0f,
                                               layout.GetAddressOf()))) {
      return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0;
    return static_cast<int>(metrics.widthIncludingTrailingWhitespace + 0.5f);
  }

  void EnsureTextMeasurement() {
    if (!writeFactory_ &&
        FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(
                                       writeFactory_.ReleaseAndGetAddressOf())))) {
      return;
    }
    if (crumbFormat_) return;
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    15.0f, L"en-US", crumbFormat_.ReleaseAndGetAddressOf());
  }

  // ---------- rendering ----------
  bool quickLookClose() {
    if (!quickLook_ || quickLook_->state() ==
                           prism::gallery::QuickLookSession::State::Closed) {
      return false;
    }
    quickLook_->Close();
    return true;
  }

  void Render() {
    // C53: the frame boundary is the cheapest place to prove that two modal
    // surfaces never reached the screen together.
    AssertSingleOverlay();
    if (!EnsureDeviceResources()) return;
    if (!rendererReady_) {
      // Belt and braces for the crash this card fixed: a renderer that was
      // never initialized has a null DirectWrite factory, and the first
      // CreateTextFormat through it faults the UI thread. Create() initializes
      // it up front; this keeps a future call site from resurrecting the bug.
      if (!rendererInitialized_) rendererInitialized_ = SUCCEEDED(renderer_.Initialize());
      if (!rendererInitialized_) return;
      rendererReady_ = SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
      if (!rendererReady_) return;
    }
    target_->BeginDraw();
    EnsureSidebar();  // the commands are painted, not computed by the renderer
    GalleryRenderer::Scene scene;
    scene.items = host_.items;
    scene.viewport = &viewport_;
    scene.layout = clientW_ > 0.0 ? &layout_ : nullptr;  // ONE geometry authority
    // V4: the rail is CHROME (DESIGN.md §2), not list-mode chrome, and folds
    // itself only below its width threshold or when the user collapses it. The
    // window owns that POLICY; the renderer paints whatever Scene it is handed,
    // which is what C27's glue suite relies on (a bare 900 px scene).
    scene.sidebar = (sidebarRailVisible_ &&
                     GalleryRenderer::SidebarFits(static_cast<float>(clientW_)))
                        ? &sidebarLayout_
                        : nullptr;
    scene.sidebarCollapsed = !sidebarRailVisible_;
    scene.listMode = listMode_;
    scene.listColumns = &listColumns_;
    scene.hasListColumns = hasListColumns_;
    scene.listItems = listMode_ ? &listItems_ : nullptr;
    // One scroll source for both painting and hit-testing: the renderer used
    // to read scene.listScroll (always 0) while clicks used the viewport.
    scene.listScroll = listMode_ ? static_cast<uint32_t>(viewport_.Scroll()) : 0u;
    scene.breadcrumb = breadcrumbText_;
    // V4: the status band's four segments read the archive properties the
    // engine reported (encryption from SetArchiveInfo, the rest from
    // SetArchiveStatus). Unknown is a state, not an empty strip.
    scene.status = archiveStatus_;
    // C-V2: the band. The model is the SAME object the click path hit-tests, so
    // a button can never be painted somewhere it cannot be pressed.
    scene.actions = &actionBar_;
    scene.actionHover = actionHover_;
    scene.actionPressed = actionPressed_;
    // C60: the strip's view is built from the SAME TabSet the hit tests read,
    // so a tab can never be painted somewhere it cannot be clicked.
    const prism::win::tabbar::View tabView = TabViewNow();
    scene.tabs = TabBarVisible() ? &tabView : nullptr;
    // C29: the rail's command list is a LAYOUT input; EnsureSidebar rebuilds it
    // only when the model is dirty, so this is a cache hit on every other frame.
    EnsureSidebar();
    {
      std::lock_guard<std::mutex> lock(thumbMutex_);
      scene.thumbs = thumbs_.get();
      // The selection is a FOLDER row; in list mode the renderer needs the VIEW
      // row that shows it (a filtered-out entry simply has no pill).
      const int selectedView =
          (selection_ >= 0 && selection_ < static_cast<int>(folderToListRow_.size()))
              ? folderToListRow_[static_cast<size_t>(selection_)]
              : -1;
      scene.selectedItem = listMode_ ? -1 : selection_;
      scene.selectedRow = listMode_ ? selectedView : -1;
      if (quickLook_ && selection_ >= 0 && selection_ < ItemCount() &&
          quickLook_->state() != prism::gallery::QuickLookSession::State::Closed) {
        const auto& item = folderItems_[static_cast<size_t>(selection_)];
        scene.quickLookOpen = true;
        scene.quickLookTitle = Utf8ToWide(prism::FileNameOf(item.path));
        wchar_t sz[40];
        swprintf_s(sz, L"%llu bytes", static_cast<unsigned long long>(item.size));
        scene.quickLookMeta =
            std::wstring(sz) + (quickLook_->streaming()
                                    ? L"  \x2022 \x26A1 \x6D41\x5F0F\x52A0\x8F7D"
                                    : L"");
      } else {
        scene.quickLookOpen = false;
      }
      scene.hud = hud_.Render(NowMs());
    }
    // C62: the field's instructions are built from the SAME model the keys edit
    // and the SAME layout the frame painted. A closed field emits nothing.
    searchScene_ = search_.IsOpen()
                       ? searchPainter_.Build(search_, SearchBounds())
                       : prism::win::searchbox::Scene{};
    renderer_.Draw(target_.Get(), scene);
    DrawSearchBox();
    // C67: one line per client width. "The band showed only seven buttons" is a
    // question a log can answer (buttons vs dropped at the size that was really
    // on screen) instead of a screenshot the next reader has to re-measure.
    if (bandTraceWidth_ != static_cast<int>(clientW_)) {
      bandTraceWidth_ = static_cast<int>(clientW_);
      const prism::actionbar::Layout band = prism::actionbar::Compute(layout_);
      std::fprintf(stderr, "band width=%d buttons=%zu dropped=%d\n", bandTraceWidth_,
                   band.buttons.size(), band.dropped);
    }
    // C52: the drop hint is a TRANSIENT overlay, not part of the scene snapshot
    // (the renderer must not grow a fourth "mode" for something that lives for
    // one drag). It is painted over the same target, in the same frame, from
    // C15 tokens -- so it can never drift from the window's palette.
    DrawDropHint();
    target_->EndDraw();
  }

  bool EnsureDeviceResources() {
    if (target_) return true;
    if (!host_.hwnd) return false;
    RECT rc{};
    GetClientRect(host_.hwnd, &rc);
    if (rc.right == 0 || rc.bottom == 0) return false;
    if (!factory_) {
      if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   IID_PPV_ARGS(factory_.GetAddressOf())))) {
        return false;
      }
    }
    const D2D1_RENDER_TARGET_PROPERTIES props =
        D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                     D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                                       D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(factory_->CreateHwndRenderTarget(props,
                                                D2D1::HwndRenderTargetProperties(
                                                    host_.hwnd,
                                                    D2D1::SizeU(static_cast<UINT32>(rc.right),
                                                                static_cast<UINT32>(rc.bottom))),
                                                target_.ReleaseAndGetAddressOf()))) {
    // note: CreateHwndRenderTarget takes a bare **out -- IID_PPV_ARGS would
    // expand to two arguments and break the arity.
      return false;
    }
    return true;
  }

  // ---------- C34: the glass layer (composition) ----------
  // <exe dir>\Assets\Shaders is the layout "cmake --install" produces; the
  // effects are optional (a missing .hlsl leaves the host flat, still composing).
  static std::wstring DefaultShaderDirectory() {
    wchar_t module[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, module, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(module, length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) path.resize(slash);
    return path + L"\\Assets\\Shaders";
  }

  // One transparent frame. The composition visual is layered ABOVE the window's
  // D2D content (CreateTargetForHwnd with topmost=TRUE), so a presented frame
  // that only clears to transparent changes no pixels -- it exists so the
  // swapchain has defined contents instead of whatever the driver left in
  // buffer 0. The optics (what WOULD be painted here) belong to the renderer.
  void PresentGlassFrame() {
    if (!glass_) return;
    const prism::glass::IslandRect rect{0, 0, kGlassIslandWidth, kGlassIslandHeight};
    prism::win::glass::GlassConstants constants{};
    glass_->PaintIsland(0, rect, constants, nullptr,
                        [](ID2D1DeviceContext* dc, float, float) {
                          dc->Clear(D2D1::ColorF(0, 0.0f));
                        });
  }

  void EnsureIdleTimer() {
    if (idleTimer_ || !host_.hwnd || !onIdle_) return;
    idleTimer_ = SetTimer(host_.hwnd, kIdleTimerId, kIdleTimerMs, nullptr) != 0;
  }


  // ---------- card C62: search plumbing -------------------------------------
  // The field floats over the top-right of the content. The rectangle comes
  // from the layout the frame is painting; SearchBounds is the ONLY authority,
  // so the painted field and the clear-button hit test cannot drift.
  prism::win::searchbox::Box SearchBounds() const {
    const prism::layout::Rect& content = layout_.contentInner;
    const float right = static_cast<float>(content.right);
    const float left = (std::max)(static_cast<float>(content.left), right - 320.0f);
    const float top = static_cast<float>(content.top);
    return prism::win::searchbox::Box{left, top, right, top + 32.0f};
  }

  // The query owns C13's SUBSTRING field only: a glob/type/size filter the
  // caller already set survives, so search narrows the view instead of
  // resetting it. Clearing the query restores every row (empty == no filter).
  void ApplySearchFilter() {
    prism::list::FilterSpec spec = filter_;
    spec.substring = search_.Query();
    SetFilter(spec);
    RebuildLayout();  // a shorter result list starts at its first row again
  }

  bool HandleSearchClick(int x, int y) {
    if (!search_.IsOpen()) return false;
    switch (searchScene_.HitTest(static_cast<float>(x), static_cast<float>(y))) {
      case prism::win::searchbox::Target::ClearButton:
        search_.ClearQuery();
        ApplySearchFilter();
        Invalidate();
        return true;
      case prism::win::searchbox::Target::Field:
        search_.SetCaret(search_.Query().size());  // click puts the pen at the end
        Invalidate();
        return true;
      case prism::win::searchbox::Target::Badge:
        return true;  // the count is a label, not a button
      case prism::win::searchbox::Target::None:
        return false;
    }
    return false;
  }

  // One key -> one KeyEvent -> one Effect. Printable input does NOT come from
  // here: WM_CHAR is the only route that honours the layout and the IME.
  LRESULT HandleSearchKey(UINT vk) {
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    prism::search::KeyEvent event;
    bool mapped = true;
    switch (vk) {
      case VK_BACK:
        event = prism::search::KeyEvent::Press(prism::search::Key::Backspace, ctrl);
        break;
      case VK_DELETE:
        event = prism::search::KeyEvent::Press(prism::search::Key::Delete, ctrl);
        break;
      case VK_LEFT:
        event = prism::search::KeyEvent::Press(prism::search::Key::Left, ctrl, shift);
        break;
      case VK_RIGHT:
        event = prism::search::KeyEvent::Press(prism::search::Key::Right, ctrl, shift);
        break;
      case VK_HOME:
        event = prism::search::KeyEvent::Press(prism::search::Key::Home, ctrl, shift);
        break;
      case VK_END:
        event = prism::search::KeyEvent::Press(prism::search::Key::End, ctrl, shift);
        break;
      case VK_ESCAPE:
        event = prism::search::KeyEvent::Press(prism::search::Key::Escape);
        break;
      case VK_RETURN:
        event = prism::search::KeyEvent::Press(prism::search::Key::Enter);
        break;
      case 'A':
        if (!ctrl) {
          mapped = false;
          break;
        }
        event = prism::search::KeyEvent::Press(prism::search::Key::SelectAll);
        break;
      default:
        mapped = false;
        break;
    }
    // Unmapped key down: consume it anyway (the field owns the keyboard) and
    // let TranslateMessage produce the WM_CHAR that carries the character.
    if (!mapped) return 0;

    const prism::search::Effect effect = search_.HandleKey(event);
    if (effect.kind == prism::search::Effect::Kind::Closed) {
      CloseSearch();
      return 0;
    }
    if (effect.kind == prism::search::Effect::Kind::Accepted && !listRowToFolder_.empty()) {
      SetSelection(listRowToFolder_.front());  // Enter hands focus to the results
    }
    if (effect.queryChanged) ApplySearchFilter();
    Invalidate();
    return 0;
  }

  // Draw the instruction list. The painter owns geometry/colour/tokens; this
  // loop is the only place that knows Direct2D. Rounded corners and the X
  // strokes are applied here because they are properties of the PRIMITIVE.
  void DrawSearchBox() {
    if (!search_.IsOpen() || !target_) return;
    EnsureTextMeasurement();
    if (!crumbFormat_) return;
    for (const prism::win::searchbox::Instruction& instruction : searchScene_.instructions) {
      ComPtr<ID2D1SolidColorBrush> brush;
      if (FAILED(target_->CreateSolidColorBrush(ToD2DColor(instruction.color),
                                                brush.GetAddressOf()))) {
        continue;
      }
      const D2D1_RECT_F rect =
          D2D1::RectF(instruction.rect.left, instruction.rect.top, instruction.rect.right,
                      instruction.rect.bottom);
      switch (instruction.primitive) {
        case prism::win::searchbox::Primitive::FieldBackground:
        case prism::win::searchbox::Primitive::TextSelection:
        case prism::win::searchbox::Primitive::ClearButton:
        case prism::win::searchbox::Primitive::Badge: {
          const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(
              rect, instruction.cornerRadius, instruction.cornerRadius);
          target_->FillRoundedRectangle(shape, brush.Get());
          break;
        }
        case prism::win::searchbox::Primitive::Caret:
          target_->FillRectangle(rect, brush.Get());
          break;
        case prism::win::searchbox::Primitive::FieldBorder:
        case prism::win::searchbox::Primitive::FocusRing: {
          const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(
              rect, instruction.cornerRadius, instruction.cornerRadius);
          target_->DrawRoundedRectangle(shape, brush.Get(), 1.0f);
          break;
        }
        case prism::win::searchbox::Primitive::ClearGlyph: {
          const float cx = (instruction.rect.left + instruction.rect.right) * 0.5f;
          const float cy = (instruction.rect.top + instruction.rect.bottom) * 0.5f;
          target_->SetTransform(D2D1::Matrix3x2F::Rotation(
              instruction.rotationDegrees, D2D1::Point2F(cx, cy)));
          target_->FillRectangle(rect, brush.Get());
          target_->SetTransform(D2D1::Matrix3x2F::Identity());
          break;
        }
        case prism::win::searchbox::Primitive::QueryText:
        case prism::win::searchbox::Primitive::CompositionText:
        case prism::win::searchbox::Primitive::Placeholder:
        case prism::win::searchbox::Primitive::BadgeText: {
          if (instruction.text.empty()) break;
          const std::wstring wide = Utf8ToWide(instruction.text);
          target_->DrawText(wide.c_str(), static_cast<UINT32>(wide.size()), crumbFormat_.Get(),
                            rect, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP,
                            DWRITE_MEASURING_MODE_NATURAL);
          break;
        }
      }
    }
  }

  // List mode rows are a SORTED, FILTERED VIEW over the current folder (C13).
  //
  // The folder rows keep the navigator's order because the GRID paints those;
  // the view is the authority for list-mode painting AND hit-testing. The two
  // numberings are glued by listRowToFolder_/folderToListRow_, so the selection
  // -- a FOLDER row everywhere else in this class -- can never point at the
  // wrong entry just because the view was re-ordered (the exact class of bug
  // that makes "sort by size" activate a random file).
  void RebuildListItems() {
    listRows_.clear();
    listRows_.reserve(folderItems_.size());
    for (const prism::ArchiveItem& item : folderItems_) {
      prism::list::Row row;
      row.catalogIndex = item.index;
      row.name = item.path;
      row.typeTag = prism::list::TypeTagOf(item.path);
      row.size = item.size;
      row.packSize = item.packSize;
      row.mtime = item.mtime;
      row.isDir = item.isDir;
      listRows_.push_back(std::move(row));
    }
    prism::list::SortRows(listRows_, sortSpec_);

    listItems_.clear();
    listRowToFolder_.clear();
    folderToListRow_.assign(folderItems_.size(), -1);
    const std::vector<uint32_t> noSelection;
    const prism::list::FilterResult filtered =
        prism::list::ApplyFilter(listRows_, noSelection, filter_);
    listItems_.reserve(filtered.visible.size());
    listRowToFolder_.reserve(filtered.visible.size());
    for (const uint32_t catalogIndex : filtered.visible) {
      const auto it = folderRowOfCatalog_.find(catalogIndex);
      if (it == folderRowOfCatalog_.end()) continue;
      const int folderRow = it->second;
      const prism::ArchiveItem& item = folderItems_[static_cast<size_t>(folderRow)];
      prism::list::SortItem paint;
      paint.name = item.path;
      paint.size = item.size;
      paint.packedSize = item.packSize;
      paint.isDir = item.isDir;
      paint.encrypted = item.encrypted;
      paint.kind = prism::QuickLookPlanner::KindByName(item.path);
      listItems_.push_back(paint);
      listRowToFolder_.push_back(folderRow);
      folderToListRow_[static_cast<size_t>(folderRow)] =
          static_cast<int>(listRowToFolder_.size()) - 1;
    }
  }

  // A fresh ViewportWindow forgets the client size (it only ever arrives via
  // WM_SIZE), so the size is re-applied here -- otherwise every navigation
  // would zero the column width and the next hit test would divide by it.
  void RebuildLayout() {
    RecomputeLayout();
    viewport_.SetScroll(0.0);
  }

  // V1: recompute the ONE layout every component reads, and feed the viewport
  // the grid metrics it needs for scroll clamping. The rail counts only while
  // it is really painted: list mode + the C27 toggle + C29's width threshold.
  void RecomputeLayout() {
    const double scroll = viewport_.Scroll();
    const bool rail = prism::layout::RailVisible(sidebarRailVisible_, listMode_) &&
                      GalleryRenderer::SidebarFits(static_cast<float>(clientW_));
    // C60: the strip reserves 32 px only while a tab exists -- an empty strip
    // returns the action island (and the content) to the pre-C60 geometry.
    layout_ = prism::layout::Compute(static_cast<int>(clientW_),
                                     static_cast<int>(clientH_), rail, listMode_,
                                     TabBarVisible());
    // V4: the grid's content height is the FLOW's height (tiles are cellHeight
    // tall, tile-less rows are 32 px bands), so the viewport is fed an average
    // pitch that makes Rows() * pitch == FlowContentHeight(). Scroll clamping
    // and the painted frame therefore agree at every offset, and the list view
    // keeps its 32 px pitch unchanged.
    double pitch = 0.0;
    if (listMode_) {
      pitch = static_cast<double>(prism::layout::kListRowHeight);
    } else {
      const std::vector<prism::layout::FlowItem> flow = FlowInputs();
      const int rows = ItemsCount() > 0 && layout_.columns > 0
                           ? (ItemsCount() + layout_.columns - 1) / layout_.columns
                           : 0;
      const int height = prism::layout::FlowContentHeight(layout_, flow);
      pitch = rows > 0 ? static_cast<double>(height) / rows : 0.0;
    }
    const prism::gallery::GridLayout grid{listMode_ ? 1 : layout_.columns, pitch};
    viewport_ = prism::gallery::ViewportWindow(grid, ItemsCount());
    viewport_.SetViewportSize(static_cast<double>(layout_.contentInner.Width()),
                              static_cast<double>(layout_.contentInner.Height()));
    viewport_.SetScroll(scroll);
    barKnown_ = false;
    sidebarDirty_ = true;
  }

  int ItemsCount() const { return static_cast<int>(folderItems_.size()); }

  void Invalidate() { if (host_.hwnd) InvalidateRect(host_.hwnd, nullptr, FALSE); }

  // ---------- card C52: the drop-in implementation --------------------------
  //
  // One drag = one bounded walk. Two bounds, both deliberate:
  //   * kDropScanDepth     -- a junction/symlink chain cannot recurse forever;
  //   * kDropScanEntries   -- a 200k-file folder cannot stall the UI thread.
  // A drop is untrusted input from another process, and this code runs on the
  // message loop (CONTRACTS D29's rule -- every wait AND every walk is bounded).
  static constexpr int kDropScanDepth = 16;
  static constexpr std::size_t kDropScanEntries = 1024;

  // Win32 refuses paths longer than MAX_PATH unless they carry the extended
  // prefix, and the prefix DISABLES '/'-normalisation (CONTRACTS B4). Explorer
  // hands us backslashes already, so the conversion here is prefix-only.
  static std::wstring ExtendedLength(const std::wstring& path) {
    if (path.size() < MAX_PATH || path.rfind(L"\\\\?\\", 0) == 0) return path;
    if (path.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + path.substr(2);
    return L"\\\\?\\" + path;
  }

  // One dragged item, walked depth-first. Symlinks and junctions are NOT
  // followed: their contents are either a cycle or somebody else's folder, and
  // "add this folder" must not silently pull in a second tree.
  prism::drop::DropEntry ScanDropEntry(const std::wstring& path, int depth) {
    prism::drop::DropEntry entry;
    entry.path = WideToUtf8(path);
    const std::wstring native = ExtendedLength(path);
    const DWORD attributes = ::GetFileAttributesW(native.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return entry;  // vanished mid-drag
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) return entry;
    entry.isDirectory = true;
    if (depth >= kDropScanDepth || dropScanBudget_ == 0) return entry;
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) return entry;
    WIN32_FIND_DATAW data{};
    const std::wstring pattern = native + L"\\*";
    HANDLE find = ::FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return entry;
    do {
      if (::wcscmp(data.cFileName, L".") == 0 || ::wcscmp(data.cFileName, L"..") == 0) continue;
      if (dropScanBudget_ == 0) break;
      --dropScanBudget_;
      entry.children.push_back(ScanDropEntry(path + L"\\" + data.cFileName, depth + 1));
    } while (::FindNextFileW(find, &data) != FALSE);
    ::FindClose(find);
    return entry;
  }

  // Content rect for the hint. layout_ is only valid after the first WM_SIZE, so
  // a drop delivered before that falls back to the same default the renderer
  // uses for a client it has not measured yet.
  prism::layout::Rect ContentAreaForDrop() const {
    if (clientW_ > 0.0 && clientH_ > 0.0) return layout_.contentInner;
    return WindowLayoutForClient().contentInner;
  }

  // HDROP -> DropEntry tree -> PlanDrop -> hint. Owns the handle (DragFinish).
  void EvaluateDrop(HDROP drop) {
    // C53: a drop is itself a modal gesture. While the settings dialog owns the
    // thread a drop can still arrive on the (disabled) owner window; it is
    // consumed and NAMED instead of being planned behind a modal surface.
    if (settingsModalOpen_) {
      if (drop != nullptr) ::DragFinish(drop);
      SetStatus(L"\u8BBE\u7F6E\u5BF9\u8BDD\u6846\u6253\u5F00\u65F6\u4E0D\u63A5\u53D7\u62D6\u5165");
      return;
    }
    // The hint takes the surface from the palette / Quick Look: one overlay.
    DismissOtherOverlays(ActiveOverlay::DropHint);
    std::vector<prism::drop::DropEntry> entries;
    POINT point{};
    dropScanBudget_ = kDropScanEntries;
    if (drop != nullptr) {
      const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
      entries.reserve(count);
      for (UINT i = 0; i < count; ++i) {
        const UINT length = ::DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0 || length > 32767) continue;  // abandoned / hostile entry
        std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
        if (::DragQueryFileW(drop, i, wide.data(), length + 1) == 0) continue;
        wide.resize(static_cast<std::size_t>(length));
        prism::drop::DropEntry entry = ScanDropEntry(wide, 0);
        if (!entry.path.empty()) entries.push_back(std::move(entry));
      }
      (void)::DragQueryPoint(drop, &point);
      ::DragFinish(drop);
    }
    dropPoint_ = point;

    prism::drop::DropInput input;
    input.entries = std::move(entries);
    // The claim we hand the model is the one our own walk just established; the
    // model re-derives it anyway and reports a disagreement instead of trusting
    // it (see DropPlan.hpp).
    input.containsDirectory = prism::drop::TreeContainsDirectory(input.entries);
    input.archiveOpen = archiveOpen_;
    input.archiveReadOnly = readOnly_;
    lastDropPlan_ = prism::drop::PlanDrop(input);

    dropHintRect_ = prism::drop::HintRect(ContentAreaForDrop());
    dropHintAccepted_ = lastDropPlan_.accepted();
    dropHintTitle_ = Utf8ToWide(lastDropPlan_.headline);
    dropHintDetail_ = Utf8ToWide(lastDropPlan_.detail);
    dropHintVisible_ = true;
    AssertSingleOverlay();
    Invalidate();
    // Executing the action is the app's job (it owns the engine, the password
    // prompt and the progress HUD). A window that started extraction from a
    // message handler would be UI-thread policy this card does not get to make.
    if (onDrop_) onDrop_(lastDropPlan_);
  }

  static D2D1_COLOR_F ToD2DColor(const prism::theme::Rgba& color) {
    return D2D1::ColorF(color.r, color.g, color.b, color.a);
  }

  // The overlay: the floating-island token, rimmed by the specular border token,
  // with the refusal rim in the accent token. Two text lines: the headline (the
  // action) and the detail (why, or what will be written). No new colours and no
  // new geometry authority -- the rect comes from prism::drop::HintRect.
  void DrawDropHint() {
    if (!dropHintVisible_ || !target_) return;
    const prism::layout::Rect rect = dropHintRect_;
    if (rect.Empty()) return;
    EnsureTextMeasurement();

    const prism::theme::Variant variant = prism::theme::Variant::Daylight;
    const prism::theme::Rgba island =
        prism::theme::Lookup(prism::theme::TokenId::SurfaceFloatingIsland, variant).color;
    const prism::theme::Rgba rim =
        prism::theme::Lookup(prism::theme::TokenId::BorderSpecularRim, variant).gradient.from;
    const prism::theme::Rgba accent =
        prism::theme::Lookup(prism::theme::TokenId::AccentLiquidPrimary, variant).gradient.from;
    const prism::theme::Rgba titleInk =
        prism::theme::Lookup(prism::theme::TokenId::TextPrimary, variant).color;
    const prism::theme::Rgba detailInk =
        prism::theme::Lookup(prism::theme::TokenId::TextSecondary, variant).color;

    ComPtr<ID2D1SolidColorBrush> fill;
    ComPtr<ID2D1SolidColorBrush> edge;
    ComPtr<ID2D1SolidColorBrush> titleBrush;
    ComPtr<ID2D1SolidColorBrush> detailBrush;
    if (FAILED(target_->CreateSolidColorBrush(
            ToD2DColor(prism::theme::Rgba(island.r, island.g, island.b, 0.94f)),
            fill.GetAddressOf()))) {
      return;
    }
    if (FAILED(target_->CreateSolidColorBrush(
            ToD2DColor(dropHintAccepted_ ? rim : accent), edge.GetAddressOf()))) {
      return;
    }
    if (FAILED(target_->CreateSolidColorBrush(ToD2DColor(titleInk), titleBrush.GetAddressOf()))) {
      return;
    }
    if (FAILED(target_->CreateSolidColorBrush(ToD2DColor(detailInk), detailBrush.GetAddressOf()))) {
      return;
    }

    const D2D1_RECT_F box = D2D1::RectF(
        static_cast<float>(rect.left), static_cast<float>(rect.top),
        static_cast<float>(rect.right), static_cast<float>(rect.bottom));
    const D2D1_ROUNDED_RECT shape = D2D1::RoundedRect(box, 18.0f, 18.0f);
    target_->FillRoundedRectangle(shape, fill.Get());
    target_->DrawRoundedRectangle(shape, edge.Get(), dropHintAccepted_ ? 1.0f : 2.0f);
    if (crumbFormat_) {
      const D2D1_RECT_F titleRect =
          D2D1::RectF(box.left + 18.0f, box.top + 14.0f, box.right - 18.0f, box.bottom);
      target_->DrawText(dropHintTitle_.c_str(), static_cast<UINT32>(dropHintTitle_.size()),
                        crumbFormat_.Get(), titleRect, titleBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
      const D2D1_RECT_F detailRect = D2D1::RectF(titleRect.left, titleRect.top + 26.0f,
                                                 titleRect.right, box.bottom - 12.0f);
      target_->DrawText(dropHintDetail_.c_str(), static_cast<UINT32>(dropHintDetail_.size()),
                        crumbFormat_.Get(), detailRect, detailBrush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    }
    ++dropHintDraws_;
  }


  // Destroy the HWND if it is still alive. Idempotent on purpose: the normal
  // path destroys it in the WM_CLOSE handler, and the destructor must not
  // destroy it a second time (the handle value would be stale or -- worse --
  // recycled by an unrelated window). Device-dependent resources are released
  // BEFORE the HWND disappears on the abandon path, because an
  // ID2D1HwndRenderTarget that outlives its window still has to unbind from it.
  void DestroyHostWindow() {
    if (!host_.hwnd) return;
    if (IsWindow(host_.hwnd)) {
      target_.Reset();
      DestroyWindow(host_.hwnd);
    }
    host_.hwnd = nullptr;
  }

  static double NowSeconds() {
    LARGE_INTEGER freq{}, counter{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
  }

  // ---------------------------------------------------------------- state
  static constexpr double kWheelScrollPixels = 120.0 * 3.0;

  Host host_;
  // V1: the layout the window paints AND hit-tests with. Rebuilt on WM_SIZE,
  // view-mode change and rail toggle -- never by a component on its own.
  prism::layout::Layout layout_;
  prism::gallery::ViewportWindow viewport_{prism::gallery::GridLayout{}, 0};
  bool listMode_ = false;  // Ctrl+2: the four-column file list (ch. 3.2)

  // ---- card C27: menu, sidebar painting, list geometry, close-time size ----
  // English defaults so a window created by a test (which never calls
  // SetMenuLabels) still has a usable bar; the app replaces them with the
  // translated strings at startup.
  std::wstring fileMenuLabel_ = L"&File";
  std::wstring compressMenuLabel_ = L"&Compress...";
  std::wstring exitMenuLabel_ = L"E&xit";
  // C64: the Settings item is part of the same bar, so its label comes from the
  // app's translator like every other menu title (it used to be a literal here
  // AND in SettingsDialog::InstallMenu -- the one English item in a zh build).
  std::wstring settingsMenuLabel_ = L"Settings...";
  // C57: Help > About. Labels are owned here so the suite can read back exactly
  // what RebuildMenu installed (the same contract the Settings label has).
  std::wstring helpMenuLabel_ = L"Help";
  std::wstring aboutMenuLabel_ = L"About PrismArchive...";
  // C54: the Edit menu starts DISABLED and English-defaulted. An app that never
  // calls SetEditAvailability must not offer a delete that can only fail; the
  // window created by a test opts in explicitly.
  std::wstring editMenuLabel_ = L"&Edit";
  std::wstring editDeleteLabel_ = L"&Delete";
  std::wstring editRenameLabel_ = L"Rena&me";
  std::wstring editNewFolderLabel_ = L"New &Folder";
  bool editEnabled_ = false;
  std::wstring editDisabledReason_;
  std::function<void(const prism::win::editui::EditIntent&)> onEdit_;
  prism::win::editui::EditIntent lastEdit_;
  // C54: one operation at a time (see OperationAllowed).
  bool operationBusy_ = false;
  OperationKind operationKind_ = OperationKind::None;
  std::wstring operationLabel_;
  int refusedOperations_ = 0;
  std::wstring lastOperationRefusal_;
  std::function<void(UINT)> onMenuCommand_;
  // The app's persisted column geometry; the renderer paints it verbatim.
  prism::list::ColumnLayout listColumns_;
  bool hasListColumns_ = false;
  // Captured in WM_CLOSE (the only moment the real frame still exists).
  int closedWidth_ = 0;
  int closedHeight_ = 0;
  prism::sidebar::FocusTarget lastSidebarHit_;
  // C29: the rail really paints now, and the grid reflow landed (list inset +
  // hit-test guard), so the rail is ON by default in list mode.
  bool sidebarRailVisible_ = true;
  // V4: the status band's readout. The strings are OWNED here because the
  // model holds views; ArchiveInfo arrives by value on every set.
  prism::layout::StatusInfo archiveStatus_;
  std::string statusMethod_;
  std::string statusDictionary_;
  prism::sidebar::SidebarStore* sidebarStore_ = nullptr;  // C14 port (not owned)
  bool sidebarRestored_ = false;
  std::function<void(const std::wstring&)> onArchiveChanged_;  // C34: names the new file
  // C64: the tab a pending click came from. A successful switch clears the
  // intent in ConsumeTabSwitchIntent; a failed one restores this path.
  std::string tabSwitchOrigin_;
  std::string pendingTabSwitch_;
  prism::gallery::ThumbnailLru thumbnails_{256ull * 1024 * 1024};  // blueprint budget
  std::vector<prism::list::SortItem> listItems_;  // what list mode PAINTS
  // C13: the view behind list mode. listRows_ is the sorted/filtered model row
  // set; the two index vectors join it to the folder rows (see RebuildListItems).
  std::vector<prism::list::Row> listRows_;
  std::vector<int> listRowToFolder_;   // view row -> folder row
  std::vector<int> folderToListRow_;   // folder row -> view row (-1: filtered out)
  prism::list::FilterSpec filter_;     // "" / nullopt everywhere == show everything
  // C62: the field. The painter is rebuilt on OpenSearch so its injected
  // measure callback points at THIS window's DirectWrite factory, never a
  // global one; the scene is the frame's instructions (empty while closed).
  prism::search::Model search_;
  prism::win::searchbox::SearchBoxPainter searchPainter_;
  prism::win::searchbox::Scene searchScene_;
  wchar_t pendingHighSurrogate_ = 0;
  prism::list::MultiSortSpec sortSpec_{
      {{prism::list::SortKey::kName, prism::list::SortDirection::kAscending}}, true};
  // C-V2: the action band. The model owns the table/rules/geometry; the window
  // owns only its context (see SyncActionContext) and its wiring.
  prism::actionbar::Model actionBar_;
  std::function<void(prism::actionbar::ActionId, int)> onAction_;
  int actionHover_ = -1;
  int actionPressed_ = -1;
  bool actionTracking_ = false;
  bool readOnly_ = false;
  // C8: the two authorities the window asks instead of deciding for itself.
  prism::win::palette::PaletteShell palette_;
  prism::win::palette::PaletteOverlay overlay_;  // C53: what Ctrl+K paints (C31)
  prism::history::Stack history_;
  int zoomPx_ = 256;  // Ctrl+Shift+P stop; the renderer has no tile-size input yet
  // C12: sidebar state + its draw-command list (painting is not wired yet).
  prism::sidebar::Model sidebar_;
  mutable prism::win::sidebar::Layout sidebarLayout_;
  mutable bool sidebarDirty_ = true;
  float sidebarScroll_ = 0.0f;
  SpringAnimator scrollSpring_{kScrollSpring, 0.0};
  double scrollTarget_ = 0.0;

  // Navigation state (increment 5). `folderItems_` is the snapshot the
  // renderer and the thumbnail lookup share; its ArchiveItem::index values are
  // CATALOG indices, which is what keeps the whole-archive sweep valid.
  prism::tree::Navigator nav_;
  const std::vector<prism::ArchiveItem>* catalog_ = nullptr;  // full snapshot (borrowed)
  std::unordered_map<uint32_t, int> folderRowOfCatalog_;      // catalog index -> folder row
  std::vector<prism::ArchiveItem> folderItems_;
  int selection_ = -1;  // the FOCUS row (kept for the existing call sites)
  // C67: the ordered selection. selection_ is the focus; this set is what the
  // app batches over, what the band reports, and what a drag carries.
  prism::sel::SelectionModel sel_;
  std::function<void(int)> onSmartExtract_;  // Alt+double-click (catalog index)
  std::function<bool(HWND, const std::vector<uint32_t>&)> onDragOut_;
  std::function<bool()> altDownProbe_;  // test seam; production reads the key state
  int dragPressX_ = 0;
  int dragPressY_ = 0;
  bool dragArmed_ = false;
  int bandTraceWidth_ = -1;  // C67: last client width the band was logged at
  std::wstring rootLabel_ = L"Archive";
  std::wstring status_;
  std::wstring breadcrumbText_;
  std::vector<prism::tree::Segment> crumbSegments_;
  prism::tree::BreadcrumbLayout crumbLayout_;
  double clientW_ = 0.0;
  double clientH_ = 0.0;
  int barTop_ = 0;
  int barBottom_ = 0;
  int textLeft_ = 0;
  int textRight_ = 0;
  bool barKnown_ = false;

  GalleryRenderer renderer_;
  // Set when renderer_.Initialize() succeeded. Both the renderer's D2D factory
  // and its DirectWrite factory come from that call; a paint that runs without
  // it dereferences a null factory (the card-C1 crash, see Create()).
  bool rendererInitialized_ = false;
  bool rendererReady_ = false;
  std::mutex thumbMutex_;
  std::shared_ptr<prism::gallery::ThumbStore> thumbs_;
  std::function<void(int)> onActivate_;
  prism::ProgressHud hud_;
  std::optional<prism::gallery::QuickLookSession> quickLook_;

  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<IDWriteTextFormat> crumbFormat_;
  ComPtr<ID2D1Factory> factory_;
  ComPtr<ID2D1HwndRenderTarget> target_;

  // ---- card C34: the glass layer and the idle pump ----
  // The host is OPT-IN (EnableGlassLayer): a window that nobody asked for glass
  // never allocates a device stack.
  std::unique_ptr<prism::win::glass::GlassIslandHost> glass_;
  size_t glassReleasedBytes_ = 0;  // C33's accounting, last release
  size_t lastThumbTrimBytes_ = 0;  // bytes the idle Trim returned
  // ---- card C75: idle/release accounting ----
  size_t lastReleasedTargetBytes_ = 0;  // back buffer the last release dropped
  int idleReleases_ = 0;                // times the minimise path really ran
  int restoreRebuilds_ = 0;             // Resize callbacks that rebuilt + painted
  std::function<void()> onIdle_;   // the app's companion-preview poll
  bool idleTimer_ = false;

  // ---- card C52: the drop-in overlay (see EvaluateDrop) ----
  // archiveOpen_ is the model's second input. It is set by SetItems (a catalog
  // means an archive is open) and can be forced by a test / the app.
  bool archiveOpen_ = false;
  std::function<void(const prism::drop::DropPlan&)> onDrop_;
  prism::drop::DropPlan lastDropPlan_;
  bool dropHintVisible_ = false;
  bool dropHintAccepted_ = false;
  // C53: the one-overlay rule's own state. settingsModalOpen_ is set by the
  // settings gate in RunSettingsCommand (and read by CurrentOverlay()).
  bool settingsModalOpen_ = false;
  std::function<void()> settingsLauncher_;
  int overlayViolations_ = 0;
  prism::layout::Rect dropHintRect_;
  std::wstring dropHintTitle_;
  std::wstring dropHintDetail_;
  POINT dropPoint_{};               // where the drop landed (client coords)
  std::size_t dropScanBudget_ = 0;  // entries left in the current walk
  int dropHintDraws_ = 0;           // frames that actually painted the overlay
  int paintMessages_ = 0;           // WM_PAINTs handled (paint evidence)

  // ---- card C60: the tab strip (design §2.1) ----
  // tabs_ is C59's model, unmodified. Everything else here is UI state the
  // model must not know about: which tab the pointer is over, whether a press
  // became a drag, and where the insertion marker is.
  prism::tabs::TabSet tabs_;
  int tabHover_ = -1;
  int tabHoverClose_ = -1;
  int tabPressedClose_ = -1;
  int tabDragPending_ = -1;   // pressed on a tab, drag not armed yet
  int tabDragFrom_ = -1;
  int tabDropSlot_ = -1;
  bool tabDragActive_ = false;
  POINT tabDragOrigin_{};
  std::function<void(const std::string&)> onTab_;            // the app's switch
  std::function<bool(const std::wstring&)> onTabCloseConfirm_;  // test/app seam
  std::unique_ptr<prism::win::editui::EditConfirmDialog> tabCloseDialog_;
  int tabClosePrompts_ = 0;    // dirty closes that asked the user
  int tabCloseRefusals_ = 0;   // ... and were answered "cancel"
  std::wstring lastTabClosePrompt_;
};

}  // namespace prism::win::gallery
