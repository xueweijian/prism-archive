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

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <memory>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <mutex>
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
#include "../PrismCore/Shortcuts.hpp"       // the ONE key table (C8)
#include "../PrismCore/SidebarModel.hpp"    // sidebar state (C12)
#include "../PrismCore/SpringAnimator.hpp"
#include "../PrismCore/ThumbStore.hpp"
#include "../PrismCore/WindowLayout.hpp"  // V1: the ONE geometry authority
#include "CommandPalette.hpp"               // palette shell: geometry + text buffer
#include "EditConfirmDialog.hpp"            // C54: edit intents, confirmation model, gate
#include "GalleryRenderer.hpp"
#include "GlassIslandHost.hpp"              // C34: the composition/glass layer (C33)
#include "SidebarView.hpp"                  // sidebar draw commands (C12)

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

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

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
  const std::wstring& StatusText() const { return status_; }
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
  bool ClickAt(int x, int y) {
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
    SetSelection(ItemAtPoint(x, y));  // -1 clears: click on empty space
    return true;
  }

  bool DoubleClickAt(int x, int y) {
    if (CrumbClick(x, y)) return true;
    return ActivateRow(ItemAtPoint(x, y));
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
          // C34: nothing of this window is on screen, so the transient device
          // layers go back to the system -- the glass swapchains + effect graph
          // through ReleaseTransient (C33's IDXGIDevice3::Trim is what makes the
          // release real) and the thumbnail pixels down to their idle target.
          // No resize here: the client rect keeps the size it had, so a restore
          // repaints exactly what was there instead of a 0x0 frame.
          ReleaseGlassLayer();
          TrimThumbnails();
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
        // C34: back on screen -> rebuild what the minimise path dropped.
        if (wparam == SIZE_RESTORED || wparam == SIZE_MAXIMIZED) RestoreGlassLayer();
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
      case WM_KEYDOWN: {
        // CARD C25: the key chain that used to live here (a dozen ifs next to
        // the pixel work) is now PrismCore's table. This case does ONE thing:
        // read the thread's modifier state, describe the stroke, and hand it to
        // the single router that also serves DispatchKeyStroke.
        // CARD V2: the band gets first refusal on the keys it owns (Tab ring,
        // Enter on a focused button, arrows inside a menu). Only keys it
        // declines fall through to the gallery table below, so no existing
        // chord changes meaning.
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
        if (actionPressed_ != -1) {
          actionPressed_ = -1;
          Invalidate();
          return 0;
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_LBUTTONDBLCLK: {
        DoubleClickAt(MouseX(lparam), MouseY(lparam));
        return 0;
      }
      case WM_LBUTTONDOWN: {
        // C52: a click anywhere dismisses a drop hint that is still on screen
        // (the app clears it as soon as it acts; this is the "user changed
        // their mind" path, so an overlay can never become permanent chrome).
        if (dropHintVisible_) ClearDropHint();
        // C-V2: the band is chrome ABOVE the rows: a button click must never
        // also select the tile that happens to be behind it.
        if (ActionBarClick(MouseX(lparam), MouseY(lparam))) return 0;
        ClickAt(MouseX(lparam), MouseY(lparam));
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
        // Menu items only (the gallery owns no child controls). The command id is
        // forwarded verbatim: mapping an id to an ACTION is the app's job.
        const UINT command = LOWORD(wparam);
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
          RECT frame{};
          if (GetWindowRect(host_.hwnd, &frame)) {
            closedWidth_ = frame.right - frame.left;
            closedHeight_ = frame.bottom - frame.top;
          }
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

  // Signed client coordinates: a drag can deliver negative values.
  static int MouseX(LPARAM lparam) { return static_cast<short>(LOWORD(lparam)); }
  static int MouseY(LPARAM lparam) { return static_cast<short>(HIWORD(lparam)); }

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
    if (onAction_) onAction_(action, item);
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
    context.hasSelection = selection_ >= 0 && selection_ < ItemCount();
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
    // The palette owns the keyboard while it is open: it gets the stroke first
    // (that is how typing reaches the query field) and only a stroke it declines
    // is re-resolved against the gallery table.
    if (palette_.IsOpen()) {
      const prism::win::palette::PaletteShell::KeyOutcome outcome = palette_.OnKey(stroke);
      if (outcome.consumed) {
        if (outcome.command == prism::keys::Command::ActivateSelection) RunPalette();
        Invalidate();
        return 0;
      }
    }
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
          if (!OperationAllowed(L"\u9884\u89C8")) return;  // 预览
          quickLook_->ToggleAt(static_cast<uint32_t>(selection_));
          Invalidate();
        }
        return;
      case prism::keys::Command::QuickLookClose:
        if (quickLookClose()) Invalidate();
        return;
      case prism::keys::Command::OpenPalette:
        OpenPalette();
        return;
      case prism::keys::Command::ClosePalette:
        palette_.Close();
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
    palette_.Open();
    Invalidate();
  }

  // Enter inside the palette. A COMMAND runs through the same ExecuteCommand
  // switch the keyboard uses; a FILE is navigated to (its folder + selection),
  // never extracted -- opening a payload is the app's decision, not the list's.
  void RunPalette() {
    const prism::palette::Entry* entry = palette_.Highlighted();
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
    renderer_.Draw(target_.Get(), scene);
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
    layout_ = prism::layout::Compute(static_cast<int>(clientW_),
                                     static_cast<int>(clientH_), rail, listMode_);
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
  prism::gallery::ThumbnailLru thumbnails_{256ull * 1024 * 1024};  // blueprint budget
  std::vector<prism::list::SortItem> listItems_;  // what list mode PAINTS
  // C13: the view behind list mode. listRows_ is the sorted/filtered model row
  // set; the two index vectors join it to the folder rows (see RebuildListItems).
  std::vector<prism::list::Row> listRows_;
  std::vector<int> listRowToFolder_;   // view row -> folder row
  std::vector<int> folderToListRow_;   // folder row -> view row (-1: filtered out)
  prism::list::FilterSpec filter_;     // "" / nullopt everywhere == show everything
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
  int selection_ = -1;
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
  prism::layout::Rect dropHintRect_;
  std::wstring dropHintTitle_;
  std::wstring dropHintDetail_;
  POINT dropPoint_{};               // where the drop landed (client coords)
  std::size_t dropScanBudget_ = 0;  // entries left in the current walk
  int dropHintDraws_ = 0;           // frames that actually painted the overlay
  int paintMessages_ = 0;           // WM_PAINTs handled (paint evidence)
};

}  // namespace prism::win::gallery
