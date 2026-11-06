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

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <cstring>  // C92: memcpy into the clipboard block
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
#include "../PrismCore/InAppContextMenu.hpp" // C92: right-click model (3 surfaces)
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
#include "Accessible.hpp"                    // C80: WM_GETOBJECT -> IAccessible (MSAA)
#include "../PrismCore/A11yTree.hpp"         // C80: what the client area publishes
#include "CommandPalette.hpp"               // palette shell: geometry + text buffer
#include "EditConfirmDialog.hpp"            // C54: edit intents, confirmation model, gate
#include "PaletteOverlay.hpp"               // C53: Ctrl+K paints THIS layer (C31)
#include "SearchBoxPainter.hpp"              // C62: C15-tokened drawing instructions
#include "GalleryRenderer.hpp"
#include "GlassIslandHost.hpp"
#include "StartupTrace.hpp"              // C34: the composition/glass layer (C33)
#include "SettingsDialog.hpp"               // C53: the Settings... menu item (C31/V3)
#include "SidebarView.hpp"                  // sidebar draw commands (C12)
#include "WindowFrame.hpp"                  // C66: visible frame, work area, clamp
#include "WindowChrome.hpp"                  // C83: DWM dark caption + rim
#include "../PrismCore/ChromeTheme.hpp"      // C83: the shell palette (tokens only)
#include "../PrismCore/ChromeMenuBar.hpp"    // C106: bar visibility + the 2-level gate
#include "TabBar.hpp"                        // C60: the tab strip (pixels + hits)
#include "Typography.hpp"                    // C103: the ONE font stack (BLUEPRINT:170)

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

// ------------------------------------------------------- C86 negative controls
// CONTRACTS D3: each switch turns EXACTLY ONE published rule off, so the same
// suite source proves the rule instead of a copy of it.
//   PRISM_C86_MUTANT_NO_STATUS_SEGMENTS -> the status band is ONE element again
//        (the pre-fix tree: a single node whose name is the transient status),
//        so the "each segment is reachable" assertions go red.
//   PRISM_C86_MUTANT_MAXIMIZED_AS_NORMAL -> a maximized window's rectangle is
//        persisted as the normal frame, which is the 1552x832 trap C66 closed
//        and this card must not re-open.
#if defined(PRISM_C86_MUTANT_NO_STATUS_SEGMENTS)
inline constexpr bool kPublishStatusSegments = false;
#else
inline constexpr bool kPublishStatusSegments = true;
#endif

// C93 (CONTRACTS D3): the two dead interactions the field report measured.
//   PRISM_C93_MUTANT_DEAD_CRUMB -> no crumb pixel resolves to a level, so the
//        bar paints text that answers nothing (the pre-C93 product).
//   PRISM_C93_MUTANT_ROOT_NOOP  -> the tree's ROOT row answers nothing while
//        sub-folders still navigate (the pre-C93 "can not get back to the root").
// Each arm is its own executable compiled from the same suite source, so the
// binary that goes red IS the one that runs these checks.
inline constexpr std::string_view ActiveC93Mutation() {
  if (!prism::tree::kCrumbClickEnabled) return "dead_crumb";
#if defined(PRISM_C93_MUTANT_ROOT_NOOP)
  return "root_noop";
#else
  return {};
#endif
}

inline constexpr std::string_view ActiveGalleryMutation() {
  if (!prism::layout::kPersistWindowPosition) return "no_position";
  if (!kPublishStatusSegments) return "no_status_segments";
  if (!prism::layout::kMaximizedIsASeparateBit) return "maximized_as_normal";
  if (!prism::layout::kBreadcrumbCountsFolder) return "archive_count";
  return {};
}

using Microsoft::WRL::ComPtr;

// C85 (CONTRACTS D3): the tab strip publishes a CLOSE element per tab. The
// shipped tree had none -- a UIA walk stopped at "tab | Photos.7z" and the only
// close on the whole window was the title bar's. The injection removes the node
// so the suite's "two tabs -> two close elements" assertion MUST go red.
#if defined(PRISM_C85_MUTANT_NO_CLOSE_NODE)
inline constexpr bool kA11yTabCloseNode = false;
inline constexpr std::string_view ActiveTabCloseMutation() { return "no_close_node"; }
#else
inline constexpr bool kA11yTabCloseNode = true;
inline constexpr std::string_view ActiveTabCloseMutation() { return {}; }
#endif

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

// C74 negative control (CONTRACTS D3). In this build ApplyLanguage() refreshes
// the sidebar but never rebuilds the band's labels, i.e. the exact defect the
// card closes: the words stay whatever they were when the window opened. The
// switch suite MUST go red; Tests/suites/c74-i18nlast.cmake builds that binary.
#if defined(PRISM_C74_MUTANT_NO_LABEL_REBUILD)
// MUTANT ACTIVE (C74 arm no_label_rebuild): a language switch leaves the eight
// band labels at their previous language.
inline constexpr bool kActionBarLabelsRebuilt = false;
#else
inline constexpr bool kActionBarLabelsRebuilt = true;
#endif

// C87 negative controls (CONTRACTS D3/D26/D27; one per requirement).
//   hud_no_dismiss    : the finish message no longer dismisses the HUD, i.e.
//                       exactly the "frozen at the last percent" defect.
//   no_progress_a11y  : the HUD is painted but never published to MSAA/UIA, so
//                       a screen reader is blind to it again.
#if defined(PRISM_C87_MUTANT_HUD_NO_DISMISS)
inline constexpr bool kHudDismissOnFinish = false;
#else
inline constexpr bool kHudDismissOnFinish = true;
#endif

// C92 negative controls (CONTRACTS D3; one per published rule).
//   PRISM_C92_MUTANT_NO_DBLCLKS  : the window class drops CS_DBLCLKS again, i.e.
//                                  the pre-fix state where WM_LBUTTONDBLCLK is
//                                  never delivered and the handler is dead code.
//   PRISM_C92_MUTANT_NO_CONTEXT  : WM_CONTEXTMENU / Shift+F10 stop opening a
//                                  menu, i.e. the "right-click does nothing" bug.
//   PRISM_C92_MUTANT_NO_PRESELECT: a right-click on an unselected row leaves the
//                                  selection alone, so "Open" is greyed out on
//                                  the row the user pointed at.
// The suite builds each as its own executable and pins "C92 MUTATION CAUGHT: <arm>"
// as the PASS regex (D26: never WILL_FAIL stacked on a regex).
#if defined(PRISM_C92_MUTANT_NO_DBLCLKS)
inline constexpr bool kDoubleClicksDelivered = false;
#else
inline constexpr bool kDoubleClicksDelivered = true;
#endif
#if defined(PRISM_C92_MUTANT_NO_CONTEXT)
inline constexpr bool kContextMenusEnabled = false;
#else
inline constexpr bool kContextMenusEnabled = true;
#endif
#if defined(PRISM_C92_MUTANT_NO_PRESELECT)
inline constexpr bool kContextPreselectsRow = false;
#else
inline constexpr bool kContextPreselectsRow = true;
#endif
#if defined(PRISM_C87_MUTANT_NO_PROGRESS_A11Y)
inline constexpr bool kHudPublishedToA11y = false;
#else
inline constexpr bool kHudPublishedToA11y = true;
#endif

// C116 (CONTRACTS D3/D26): the repaint door, as one named predicate. Every
// path that REBUILDS the scene (attaching a catalog, changing folder) asks for
// a frame through Invalidate(), so "the model changed and the window was never
// dirtied" is a state this window cannot be put in -- unless the mutant below
// closes the door. The suite asserts the WINDOW'S UPDATE REGION (GetUpdateRect),
// which is the platform's own answer to "is a WM_PAINT pending?", not a counter
// this window could keep for itself.
#if defined(PRISM_C116_MUTANT_NO_REPAINT)
inline constexpr bool kRepaintRequestsArmed = false;
#else
inline constexpr bool kRepaintRequestsArmed = true;
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
  // C76: the sweep worker's hand-off. Ownership of the store travels in the
  // message (a heap shared_ptr WndProc takes), so the worker never calls a
  // setter on a window it does not own (B9).
  static constexpr UINT kThumbsReady = WM_APP + 7;  // 5/6 are archive/operation
  // C87: the operation ENDED (success / failure / cancel). Posted, like the
  // progress samples: the worker owns the engine, the UI thread owns the HUD.
  static constexpr UINT kHudFinish = WM_APP + 8;

  // The window owns an HWND and a device-dependent renderer; both are released
  // in the destructor, in that order. Main.cpp's `window` is a stack object, so
  // this runs on the UI thread after Run() returned and after every worker was
  // joined -- the only point where the HWND and the renderer are guaranteed to
  // be quiescent.
  ~GalleryWindow() {
    DetachAccessibility();
    DestroyHostWindow();
    ReleaseMenuResources();  // C83: the owner-drawn bar's font + brushes
  }

  struct Host {
    HWND hwnd = nullptr;
    std::vector<prism::ArchiveItem>* items = nullptr;  // CURRENT FOLDER rows (not owned)
  };

  bool Create(const std::wstring& title, HINSTANCE instance) {
    // C76: Create() is split into the stages that can be moved or removed. The
    // spans are the only change to the order of operations -- nothing here was
    // reordered to make a number look better.
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    // C92: CS_DBLCLKS is what makes Windows deliver WM_LBUTTONDBLCLK at all.
    // Without it the double-click case below is unreachable dead code, which is
    // exactly what the field report measured. The mutant removes it again.
    wc.style = CS_HREDRAW | CS_VREDRAW;
    if (kDoubleClicksDelivered) wc.style |= CS_DBLCLKS;
    wc.lpfnWndProc = &GalleryWindow::ProcThunk;
    wc.hInstance = instance;
    // IDC_ARROW expands to MAKEINTRESOURCEA in a non-UNICODE TU; name the wide
    // integer directly (32512) so the call is ASCII/Unicode-proof.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.lpszClassName = L"PrismGalleryWindow";
    {
      PRISM_C76_SPAN("class_register", "GalleryWindow::Create: RegisterClassExW");
      RegisterClassExW(&wc);
    }
    {
      // C76: this is the process's FIRST DirectWrite text format, so it is also
      // the first font-collection load. It gets its own span because a lazy-font
      // change has to show up here or not at all.
      PRISM_C76_SPAN("dwrite_first_format", "GalleryWindow::Create: EnsureTextMeasurement");
      EnsureTextMeasurement();  // breadcrumb widths exist before the first layout
    }

    {
      PRISM_C76_SPAN("create_window_ex", "GalleryWindow::Create: CreateWindowExW");
      host_.hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(),
                                   WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                   1280, 800, nullptr, nullptr, instance, this);
    }
    if (!host_.hwnd) return false;
    // C83: the caption joins the shell BEFORE the first frame. A light caption
    // that turned dark on the first theme change would be a visible flash.
    titleBar_ = prism::win::chrome::Apply(host_.hwnd, chromeVariant_);
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
    {
      PRISM_C76_SPAN("renderer_factories", "GalleryRenderer::Initialize (D2D+DWrite)");
      rendererInitialized_ = SUCCEEDED(renderer_.Initialize());
    }
    // The bar exists from the first frame: a window without a menu, later
    // growing one, is a visible flicker AND a different client rect, which would
    // move every tile after the user opened it.
    {
      PRISM_C76_SPAN("menu_bar", "GalleryWindow::RebuildMenu");
      RebuildMenu();
    }
    if (!rendererInitialized_) return false;
    {
      PRISM_C76_SPAN("d2d_hwnd_target", "GalleryWindow::EnsureDeviceResources");
      if (!EnsureDeviceResources()) return false;
    }
    return true;
  }

  // Attach the archive catalog (the app owns the snapshot; it outlives the
  // window). The navigator resets to the archive root.
  //
  // The SAME snapshot also feeds the two C12/C13 authorities: the sidebar model
  // (which builds the archive tree and the telemetry totals from it) and the
  // palette ranker (which searches the whole archive, not just this folder).
  // One catalog, three readers -- never a second copy that could drift.
  void SetItems(std::vector<prism::ArchiveItem>* items) {
    // C87: a new archive is a new session; a HUD frozen on the previous one must
    // not survive the switch.
    ResetHud();
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
    // C81/A3: the status band's 压缩率 comes from the SAME catalog snapshot,
    // through the SAME canonical view the list cell and the info card render
    // ("one catalog, three readers" -- and now one string, never three
    // formulas; UI-AUDIT A3: the row said 100%, the card 126.7%, the band
    // 未知 because nothing ever fed it). Crypto keeps whatever SetArchiveInfo
    // derived; only the ratio is (re)computed here, so a tab switch that
    // re-attaches the catalog cannot leave a stale number behind.
    if (catalog_ != nullptr) {
      std::uint64_t originalBytes = 0;
      std::uint64_t compressedBytes = 0;
      for (const prism::ArchiveItem& item : *catalog_) {
        originalBytes += item.size;
        compressedBytes += item.packSize;
      }
      archiveStatus_.ratioView = prism::CompressionRatioView(originalBytes, compressedBytes);
    } else {
      archiveStatus_.ratioView.clear();
    }
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

  // ---------- card C92: double-click OPENS, right-click has a MENU -----------
  //
  // "Open" is a different job from "smart-extract the archive": the app extracts
  // the ONE entry to a temp directory and hands it to ShellExecute. It gets its
  // own seam because Enter and double-click must share ONE call site
  // (ActivateRow), and because a suite has to prove the file route without a
  // real shell association (the handler is the injection point). With no handler
  // installed the old activate path still runs -- no behaviour silently dies.
  void SetOpenHandler(std::function<void(int)> handler) { onOpen_ = std::move(handler); }

  // The archive-level verbs the window cannot perform itself. The window keeps
  // the row -> catalog-index mapping; the app owns what "extract" means (same
  // split as SetActivateHandler / SetSmartExtractHandler).
  void SetContextCommandHandler(
      std::function<void(prism::ctxmenu::Command)> handler) {
    onContextCommand_ = std::move(handler);
  }
  // Where the archive is, for Copy Path and "Show Archive in Explorer". Unset =>
  // those two items are DISABLED with a reason, never a silent no-op.
  void SetArchivePath(const std::wstring& path) { archivePath_ = path; }

  // The exact model and HMENU the REAL WM_CONTEXTMENU path built, immediately
  // before it would enter TrackPopupMenu. A probe suppresses the modal loop only
  // (a headless suite cannot dismiss a system menu); everything else -- the
  // surface decision, the i18n words, the ids, the enabled/reason rules -- is
  // the production path. HMENU is valid during the call and destroyed after.
  struct ContextMenuInvocation {
    prism::ctxmenu::Surface surface = prism::ctxmenu::Surface::Empty;
    prism::ctxmenu::Menu model;
    std::vector<UINT> ids;
    HMENU menu = nullptr;
    POINT screenPoint{0, 0};
    bool fromKeyboard = false;
  };
  void SetContextMenuProbe(
      std::function<void(const ContextMenuInvocation&)> probe) {
    contextProbe_ = std::move(probe);
  }
  int ContextMenusShown() const { return contextMenusShown_; }
  const prism::ctxmenu::Menu& LastContextMenuForTest() const { return lastContextMenu_; }
  // The id -> verb table the open menu published; a test resolves an id the same
  // way WM_COMMAND does instead of guessing from the slot number.
  prism::ctxmenu::Command ContextCommandForId(UINT id) const {
    if (!prism::ctxmenu::IsContextCommandId(id)) return prism::ctxmenu::Command::None;
    const std::size_t surface = static_cast<std::size_t>(prism::ctxmenu::SurfaceOfCommandId(id));
    const std::size_t slot = prism::ctxmenu::SlotOfCommandId(id);
    if (surface >= contextCommands_.size() || slot >= contextCommands_[surface].size()) {
      return prism::ctxmenu::Command::None;
    }
    return contextCommands_[surface][slot];
  }
  // Runs a verb through the SAME switch TrackPopupMenu's return value reaches.
  // The suite calls this with an id from the probe, so the command path is
  // exercised end to end instead of being re-implemented next to it.
  void RunContextCommandForTest(prism::ctxmenu::Surface surface, UINT id) {
    RunContextCommand(surface, id);
  }
  // Keyboard invocation (Shift+F10 / the Menu key) with a client point of its
  // own. -1,-1 means "the same point WM_CONTEXTMENU(-1,-1) would pick".
  void OpenContextMenuForTest(int clientX, int clientY) {
    if (clientX < 0 && clientY < 0) {
      OpenContextMenu(-1, -1, true);
      return;
    }
    POINT point{clientX, clientY};
    ClientToScreen(host_.hwnd, &point);
    OpenContextMenu(point.x, point.y, false);
  }
  // A headless window station has no keyboard: the suite injects the modifier
  // state here. Production reads the thread's REAL key state.
  void SetAltDownProbe(std::function<bool()> probe) { altDownProbe_ = std::move(probe); }
  bool AltDown() const {
    return altDownProbe_ ? altDownProbe_() : ((::GetKeyState(VK_MENU) & 0x8000) != 0);
  }
  // D3: the one place the Alt chord can be switched off, so the mutant build
  // proves the injection compiled in (the verdict's predicate asks THIS).
  // C92: the published rules, asked at runtime by the mutant verdicts.
  static constexpr bool DoubleClicksDelivered() { return kDoubleClicksDelivered; }
  static constexpr bool ContextMenusEnabled() { return kContextMenusEnabled; }
  static constexpr bool ContextPreselectsRow() { return kContextPreselectsRow; }
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

  // C86 (S3-11): the facts the band PUBLISHES. The counts are filled in here,
  // from the folder the user is actually looking at, because the band and the
  // accessibility tree must not each invent a number: before this, the renderer
  // derived 条目/选中 from the scene while the tree read archiveStatus_ and
  // answered "条目 --". Both now call THIS, so paint and UIA cannot disagree.
  prism::layout::StatusInfo StatusForPublish() const {
    prism::layout::StatusInfo info = archiveStatus_;
    if (info.itemCount < 0) info.itemCount = ItemCount();
    if (info.selectedCount < 0) info.selectedCount = SelectionCount();
    return info;
  }
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

  // ---------- card C83: the dark shell -------------------------------------
  //
  // ONE palette for the whole shell (prism::chrome::PaletteFor): the caption
  // (DWM), the menu bar plate, the dropdown plate and the selected row. The app
  // tells the window which Variant the saved appearance document asks for; the
  // default is Midnight because that is the surface the canvas already paints,
  // so a fresh install's shell is continuous with the content on day one.
  void SetChromeVariant(prism::theme::Variant variant) {
    chromeVariant_ = variant;
    // C91: the SAME variant drives the CONTENT. Until this card the shell
    // (caption + owner-drawn menu) followed the setting while every renderer
    // surface kept its Midnight default -- which is why a whole-window day ->
    // night diff moved 7.96 % of the pixels, all of them in the title bar. One
    // setter, one field, both halves of the window.
    ApplyChromeTheme();
    Invalidate();
  }
  prism::theme::Variant ChromeVariant() const { return chromeVariant_; }
  // C91: what the renderer is handed as Scene::theme. It is the SAME field the
  // shell uses, so "the caption changed but the canvas did not" is no longer a
  // state this window can be put in.
  prism::theme::Variant ContentTheme() const { return chromeVariant_; }
  const prism::win::chrome::TitleBarRecord& TitleBar() const { return titleBar_; }

  // The words the owner-drawn bar carries. The OS no longer stores an
  // owner-drawn item's text (the item data IS the text), so the window is the
  // only place they can be read back: the language suite and the UIA probe use
  // this, and it is the SAME string WM_DRAWITEM paints -- not a second copy.
  int MenuBarItemCount() const { return static_cast<int>(menuBarItems_.size()); }
  std::wstring MenuBarItemText(int index) const {
    if (index < 0 || index >= MenuBarItemCount()) return std::wstring();
    return menuBarItems_[static_cast<std::size_t>(index)]->text;
  }

  // ---------- card C106: the menu bar's visibility -------------------------
  //
  // THE OBSERVABLE. A Win32 menu bar is NOT a window: there is no HWND, so
  // IsWindowVisible has no meaning for it and a suite that waited for one would
  // wait forever. The OS's own observables are (a) GetMenu() -- null when no bar
  // is installed, which is exactly "the bar is not on screen" -- and (b) the
  // CLIENT AREA's origin, which moves up by SM_CYMENU the moment the bar stops
  // reserving non-client space. Both are what win.c106_chrome asserts; the second
  // is the card's ">= 20 px".
  int MenuBarHeight() const { return ::GetSystemMetrics(SM_CYMENU); }
  bool MenuBarVisible() const { return menuBarVisible_; }
  bool MenuAutoHide() const { return menuAutoHide_; }
  // The bar Alt brings back while the menu bar is down (null when it is up).
  HMENU ParkedMenu() const { return parkedMenu_; }
  // The live state, as the pure model describes it (PrismCore/ChromeMenuBar.hpp).
  prism::chrome::MenuBarState MenuBarStateNow() const {
    prism::chrome::MenuBarState state;
    state.visible = menuBarVisible_;
    state.autoHide = menuAutoHide_;
    return state;
  }
  // The pixels the content gains at the top while the bar is hidden.
  int MenuBarHiddenShift() const {
    return prism::chrome::HiddenShift(MenuBarStateNow(), MenuBarHeight());
  }
  // True in the PRISM_C106_MUTANT_WHITE_BAR build. A constexpr predicate, so the
  // suite can tell "the injection is in this binary" from "the gate happened to
  // fail"; CONTRACTS D3 requires the CAUGHT verdict to be armed on evidence, not
  // on a red.
  static constexpr bool MenuWhiteBarMutant() {
#if defined(PRISM_C106_MUTANT_WHITE_BAR)
    return true;
#else
    return false;
#endif
  }

  // The SETTING ("auto-hide the menu bar"). Applied once, before the first
  // ShowWindow, so the bar never flashes on screen for a user who turned it off.
  void SetMenuAutoHide(bool autoHide) {
    menuAutoHide_ = autoHide;
    if (autoHide) SetMenuBarVisible(false);
  }

  // Install or withdraw the bar. Withdrawing PARKS the HMENU instead of
  // destroying it (see RebuildMenu): same bar, same item data, same UIA object.
  void SetMenuBarVisible(bool visible) {
    if (!host_.hwnd) {
      menuBarVisible_ = visible;
      return;
    }
    if (visible == menuBarVisible_) return;
    const bool wasVisible = menuBarVisible_;
    menuBarVisible_ = visible;
    if (visible) {
      if (parkedMenu_ != nullptr && ::SetMenu(host_.hwnd, parkedMenu_) != FALSE) {
        parkedMenu_ = nullptr;
      } else {
        RebuildMenu();  // no parked bar (or the OS refused it): build one
      }
    } else {
      HMENU bar = ::GetMenu(host_.hwnd);
      if (bar != nullptr && ::SetMenu(host_.hwnd, nullptr) != FALSE) {
        parkedMenu_ = bar;
      } else {
        // The OS kept the bar: report the state the window is really in, not the
        // one it asked for.
        menuBarVisible_ = wasVisible;
        return;
      }
    }
    ::DrawMenuBar(host_.hwnd);
    // SetMenu already ran WM_NCCALCSIZE, but the frame's cached rectangles only
    // refresh on SWP_FRAMECHANGED, and the renderer needs the new client size
    // before the next frame (that is what "the content moves up" means here).
    ::SetWindowPos(host_.hwnd, nullptr, 0, 0, 0, 0,
                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE |
                       SWP_FRAMECHANGED);
    Invalidate();
  }
  // The flip is PrismCore's (prism::chrome::Toggle), not a second "!" here: the
  // suite and the window must be flipping the same rule.
  void ToggleMenuBar() {
    SetMenuBarVisible(prism::chrome::Toggle(MenuBarStateNow()).visible);
  }

  // Ctrl+O. The window does not know what "open an archive" means -- the app owns
  // the picker and the switch path -- so the chord only asks. The card's point is
  // that this route does not depend on the menu bar being on screen.
  void SetOpenArchiveHandler(std::function<void()> handler) {
    onOpenArchive_ = std::move(handler);
  }

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
    // C106: a hidden bar is PARKED, and its items are still commands the app can
    // run (UIA, Ctrl chords, the action island). Answering "disabled" for a bar
    // that is merely off screen would make the model disagree with the pixels.
    HMENU bar = host_.hwnd ? GetMenu(host_.hwnd) : nullptr;
    if (!bar) bar = parkedMenu_;
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
  //
  // C83: the bar is OWNER-DRAWN, so the OS stores no string for its items -- the
  // item data IS the text. The window's own MenuDrawItem is therefore the
  // authority: it is the exact string WM_DRAWITEM paints, WM_SYSCHAR matches and
  // the OBJID_MENU tree publishes. GetMenuStringW stays as the fallback for a
  // bar installed by someone else (SettingsDialog::InstallMenu's no-menu path,
  // which is a plain MF_STRING bar).
  std::wstring MenuItemText(UINT command) const {
    for (const std::unique_ptr<MenuDrawItem>& item : menuItems_) {
      if (item != nullptr && !item->popup && item->command == command) return item->text;
    }
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
    if (!menuBarItems_.empty() && menuBarItems_.front() != nullptr) {
      return menuBarItems_.front()->text;
    }
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
    if (width) *width = closedWindow_.visible.Width();
    if (height) *height = closedWindow_.visible.Height();
    return closedWindow_.visible.Width() > 0 && closedWindow_.visible.Height() > 0;
  }

  // C86: the WHOLE frame the user closed -- origin, size AND the maximized bit
  // (the C66 accessor above carried a size only, which is why the position never
  // reached settings.json). Read after Run() returns, captured in WM_CLOSE.
  const prism::layout::SavedWindow& ClosedWindow() const { return closedWindow_; }
  bool ClosedMaximized() const { return closedWindow_.maximized; }

  // C66: apply a persisted frame through the ONE policy (visible frame + work
  // area clamp). Main.cpp calls this instead of a bare SetWindowPos, because a
  // second call site is a second chance to forget the clamp -- and forgetting it
  // is exactly what put the last three action buttons off screen.
  //
  // The saved value is a SIZE, not a position: the position the window was
  // created at is kept when it fits, and the clamp moves it on screen when it
  // does not (a stale position from an unplugged monitor, negative coordinates).
  prism::win::frame::Restore RestoreFrame(int width, int height) {
    prism::layout::SavedWindow saved;
    saved.visible = prism::layout::Rect{0, 0, width, height};
    saved.hasPosition = false;  // C66's contract: a size, and the window keeps its origin
    return RestoreFrame(saved);
  }

  // C86: the same door, with the ORIGIN and the maximized bit. A document that
  // predates C86 has hasPosition == false, so the window keeps the origin it was
  // created with and only the size is applied -- exactly the C66 behaviour, now
  // written down instead of implied.
  prism::win::frame::Restore RestoreFrame(prism::layout::SavedWindow saved) {
    if (host_.hwnd == nullptr || saved.visible.Width() <= 0 || saved.visible.Height() <= 0) {
      return prism::win::frame::Restore{};
    }
    if (!saved.hasPosition) {
      const prism::layout::Rect current = prism::win::frame::ReadWindowRect(host_.hwnd);
      const int width = saved.visible.Width();
      const int height = saved.visible.Height();
      saved.visible = prism::layout::Rect{current.left, current.top, current.left + width,
                                          current.top + height};
    }
    prism::win::frame::Restore restored =
        prism::win::frame::RestoreSavedFrame(host_.hwnd, saved);
    RecomputeLayout();
    // C86: a restore that landed on a normal frame is the cheapest place to
    // re-measure the borders the close path will need.
    RememberNormalBorders();
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
    // C107: THE OPTICS. Until this call the two shaders had never run in the
    // product: InitializeOptics existed with zero call sites, so every "glass"
    // panel was a translucent fill with a 1 px rim. The Kawase radius comes
    // from appearance.glassIntensity (default 0.72); a failure is recorded on
    // the host (ShaderError()) and logged here instead of silently leaving a
    // grey pill behind.
    const double glassIntensity = prism::theme::live::Current().glassIntensity;
    if (FAILED(host->InitializeOpticsForIntensity(glassIntensity))) {
      std::fprintf(stderr, "c107: EnableGlassLayer: optics chain failed: %s\n",
                   host->ShaderError().c_str());
    }
    glass_ = std::move(host);
#if defined(PRISM_C76_MUTANT_GLASS_ON_FIRST_FRAME) || defined(PRISM_C76_BASELINE)
    // Pre-C76 behaviour (and the negative control): the island is presented
    // BEFORE the window has painted anything, so the composition submit is part
    // of the first frame. The mutant build must fail C76's
    // "no glass before the first frame" predicate.
    PresentGlassFrame();
#else
    // C76: the island's FIRST present waits for the SECOND frame. Nothing the
    // user sees changes -- the island clears to transparent and composites over
    // the window's own opaque content -- but the first frame no longer pays for
    // the composition submit (measured cost is in docs/PERF.md).
    glassPresentPending_ = true;
#endif
    return true;
  }
  bool GlassLayerReady() const { return glass_ != nullptr; }
  bool GlassLayerVisible() const { return glass_ != nullptr && glass_->Visible(); }
  size_t GlassTransientBytes() const { return glass_ ? glass_->TransientBytes() : 0; }
  bool GlassTransientLive() const { return glass_ ? glass_->TransientLive() : false; }

  // ---- card C107: the optics chain, as the window sees it -------------------
  // "The layer is ready" and "the optics are ready" are two different facts and
  // the suite asserts both: a composition host with unregistered shaders
  // composites a flat pill, which is exactly the pre-C107 look.
  bool GlassOpticsReady() const { return glass_ && glass_->OpticsLive(); }
  const std::string& GlassOpticsError() const {
    static const std::string kNoLayer = "no glass layer";
    return glass_ ? glass_->ShaderError() : kNoLayer;
  }
  int GlassOpticsInitializations() const { return glass_ ? glass_->OpticsInitializations() : 0; }
  // The panel the island covers, straight from the LAYOUT (the same rect the
  // breadcrumb pill is painted and hit-tested with) -- never a fixed 960x48.
  prism::glass::IslandRect GlassPanelRect() const {
    const prism::layout::Rect pill = prism::layout::NavPillRect(layout_);
    if (pill.Empty()) return prism::glass::IslandRect{};
    return prism::glass::IslandRect{static_cast<uint32_t>(pill.left),
                                    static_cast<uint32_t>(pill.top),
                                    static_cast<uint32_t>(pill.Width()),
                                    static_cast<uint32_t>(pill.Height())};
  }
  // What the last composed island was fed with: the capture ran, the backdrop
  // is a bitmap on the host's device, and the frame count proves the paint path
  // (not just EnableGlassLayer) reached the chain.
  // The COMPOSITION of GlassTransientBytes(), term by term (C107): a test that
  // asserts the total against this sum cannot be invalidated by a layout change
  // that resizes the island, and one that asserts a constant can.
  size_t GlassIslandSurfaceBytes() const { return glass_ ? glass_->IslandSurfaceBytes() : 0; }
  size_t GlassGraphBytes() const { return glass_ ? glass_->GraphBytes() : 0; }
  bool GlassBackdropLive() const { return glass_ && glass_->Backdrop() != nullptr; }
  size_t GlassBackdropBytes() const { return glass_ ? glass_->BackdropBytes() : 0; }
  int GlassBackdropCaptures() const { return glass_ ? glass_->BackdropCaptures() : 0; }
  int GlassComposedFrames() const { return glassComposedFrames_; }
  int GlassDroppedBackdrops() const { return glass_ ? glass_->DroppedBackdrops() : 0; }
  bool GlassLastFrameOptical() const { return glassLastFrameOptical_; }

  // ---- card C112: WHO paints the pill's content -----------------------------
  //
  // The defect C112 exists for: the island is a CreateTargetForHwnd(topmost)
  // visual ABOVE the window, and its backdrop is a capture of the window's own
  // pixels -- which, pre-C112, already contained the breadcrumb text. The
  // optics then blurred that text and painted the blur back OVER it, so every
  // glyph got a second, offset, blurred copy of itself (the user's "重影").
  //
  // The fix is ownership, not a tuning knob: while the island is composited the
  // pill (fill, rim, hover capsule and TEXT) belongs to the ISLAND, and the
  // window keeps only the geometry it hit-tests with.
  bool GlassPillOwnsContent() const {
#if defined(PRISM_C112_MUTANT_GLASS_OVER_TEXT)
    // NEGATIVE CONTROL (CONTRACTS D3/D27): the pre-C112 arm -- the window keeps
    // painting the pill's text and the island is composed on top of it. The
    // suite's sharpness assertion MUST go red, and the verdict it prints names
    // this mutant.
    return false;
#else
    return glass_ && glass_->UsesComposition() && glass_->Visible();
#endif
  }
  // The device context the island's frames are composed on (the test creates an
  // offscreen target there; a cross-device target is D2DERR_WRONG_RESOURCE_DOMAIN).
  ID2D1DeviceContext* GlassContext() const { return glass_ ? glass_->Context() : nullptr; }
  // (PresentGlassFrameForTest lives with the other C113 capture seams further
  // down; the two cards defined it independently and the merge keeps ONE.)
  // One island frame, with the content underneath supplied by the caller instead
  // of captured from the window. This is the SAME composition the product
  // presents (PresentGlassFrame -> PaintIslandContent); a suite that painted a
  // lookalike would not be asserting the frames the user gets.
  bool ComposeIslandForTest(ID2D1Bitmap1* target, ID2D1Bitmap* backdrop) {
    if (!glass_ || !target) return false;
    const prism::glass::IslandRect rect = GlassPanelRect();
    if (rect.w == 0 || rect.h == 0) return false;
    prism::win::glass::GlassConstants constants{};
    BuildGlassConstants(rect, &constants);
    glassLastFrameOptical_ = backdrop != nullptr && glass_->OpticsLive();
    return glass_->PaintIslandInto(
        target, rect, backdrop, constants,
        [this](ID2D1DeviceContext* dc, float width, float height) {
          PaintIslandContent(dc, width, height);
        });
  }
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
    if (glassPresentPending_) {
      // C76: the FIRST restore arrives from the window's own WM_SIZE during
      // ShowWindow -- before frame 1 exists. Presenting here would put the
      // island on screen with the first frame, which is exactly what the
      // deferral removes (and what the glass_on_first_frame mutant does). The
      // transient surfaces ARE built; only the submit waits for frame 2.
      Invalidate();
      return true;
    }
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
    // Route through the C34 seam, not glass_->SetVisible directly: that seam is
    // what PRISM_C34_MUTANT_NO_TRIM instruments, and a minimise path that
    // bypasses it silently disarms C34's "no trim must be red" guard (the CI
    // failure that prompted this). The seam is idempotent and reports the bytes
    // it really returned; zero first so a no-glass window cannot report a stale
    // number from an earlier release.
    glassReleasedBytes_ = 0;
    ReleaseGlassLayer();
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
    idleReleased_ = true;
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
    idleReleased_ = false;
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
  // C87: "the operation is over" travels the same way the samples do. Without
  // this message the HUD had no terminal transition at all, so the capsule kept
  // painting the last sample forever (the reported freeze at 19% / 67% / 100%).
  void PostHudFinish(prism::HudOutcome outcome) {
    if (!host_.hwnd) return;
    PostMessageW(host_.hwnd, kHudFinish, static_cast<WPARAM>(outcome), 0);
  }
  // UI-thread half, also the deterministic seam the C87 suite drives.
  void FinishHud(prism::HudOutcome outcome) {
    if (!kHudDismissOnFinish) {
      // MUTANT (hud_no_dismiss): the operation ends, the capsule stays.
      Invalidate();
      return;
    }
    hud_.OnFinish(NowMs(), outcome);
    Invalidate();
  }
  // Cross-archive / cross-tab reset: a new session must not inherit a frozen
  // percent from the previous one.
  void ResetHud() {
    hud_.Reset();
    Invalidate();
  }
  // C76 + B9: the sweep worker hands the decoded store over HERE. It touches no
  // member (the target HWND was captured on the UI thread), the UI thread takes
  // ownership in WndProc and calls SetThumbs there -- the same shape as
  // PostStatus/PostHudStart, which is what the contract asks for.
  void PostThumbs(std::shared_ptr<prism::gallery::ThumbStore> store, HWND target) {
    if (!target) return;
    auto* heap = new std::shared_ptr<prism::gallery::ThumbStore>(std::move(store));
    if (!PostMessageW(target, kThumbsReady, 0, reinterpret_cast<LPARAM>(heap))) delete heap;
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
      // C92: Enter and double-click reach THIS line, so "open" and "Enter" are
      // one behaviour by construction, not by two handlers agreeing.
      if (onOpen_) {
        onOpen_(static_cast<int>(result.catalogIndex));
      } else if (onActivate_) {
        onActivate_(static_cast<int>(result.catalogIndex));
      }
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
  // C116: what the last GRID paint actually put on screen, from the renderer's
  // own counter. "The model has nine rows" and "nine rows reached the device"
  // are two different facts; this seam is the second one.
  int RowCellsPainted() const { return renderer_.dbgRowCells; }
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

  // C74: the word one button paints right now. A read-back seam, so a suite
  // proves a language switch reached the band without OCR-ing the frame.
  std::string ActionBarLabelAt(int index) const { return actionBar_.Label(index); }

  // C74: the ONE door for a language switch. The app calls it at startup and
  // from the Settings dialog's language handler. The sidebar and the band read
  // the SAME translator, so they cannot disagree about the language; the band's
  // eight words are rebuilt here (that rebuild is the negative control).
  void ApplyLanguage(const prism::i18n::Translator* translator) {
    // Copy the resolved WORDS, never the translator pointer: the Settings dialog
    // hands us a translator that lives only for its modal call, and a member
    // pointer would dangle the moment the dialog closed.
    sidebarLabels_ = prism::win::sidebar::LabelsFrom(translator);
    // C92: the right-click words are copied for the same reason (no dangling
    // translator pointer after the modal Settings call returns).
    ctxLabels_ = prism::ctxmenu::LabelsFrom(translator);
    if (kActionBarLabelsRebuilt) {
      actionBar_.SetLabels(prism::win::actionbar::LabelsFromTranslator(translator));
    }
    sidebarDirty_ = true;
    Invalidate();
  }

  // ---------- card C80: the client area made visible to MSAA/UIA ----------
  //
  // BuildAccessibleTree() (defined with the other C80 members, next to
  // FireAction) is the ONE description of what a screen reader is told, derived
  // from the same state the renderer paints: band layout, tab rectangles, folder
  // rows, sidebar model, status line. PrismWin/Accessible.hpp turns it into
  // IAccessible objects; WM_GETOBJECT(OBJID_CLIENT) hands the root to the
  // platform, whose MSAA proxy is what UIA clients then read.
  //
  // WHY A TREE, NOT A WIDGET PER ROW: our drawing is the product. The bridge
  // describes it; it does not replace it with standard controls.
  //
  // The verbs an accessible element may perform route through the SAME window
  // path a click and a key use (ActionBarClick / ActivateRow / ActivateTab /
  // GoToFolder) -- there is no second implementation of an action. Every live
  // IAccessible is detached on WM_DESTROY, so a client that kept a reference
  // across the close gets an empty answer, never a use-after-free.
  int AccessibleElementCount() { return prism::a11y::CountNodes(BuildAccessibleTree()); }

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

  // C93: the crumb the POINTER is over (kNoLevel when none). Hovering a crumb
  // must LOOK different from hovering the rest of the pill -- the bar shipped as
  // dead text partly because nothing suggested it could be pressed. Derived from
  // the same hit test the click uses, so "looks clickable" and "is clickable"
  // cannot disagree.
  int CrumbHoverLevel() const { return crumbHover_; }
  bool HoveringCrumb() const { return crumbHover_ != prism::tree::kNoLevel; }

  // The rectangle the renderer paints behind the hovered crumb: the crumb's own
  // measured span (NOT the whole bar -- a 700 px highlight under a 90 px word
  // is the "hit area covers the bar" defect), inset vertically inside the pill.
  prism::layout::Rect CrumbHoverRect() const {
    EnsureNavIsland();
    if (crumbHover_ == prism::tree::kNoLevel) return prism::layout::Rect{};
    for (const auto& span : crumbLayout_.spans) {
      if (span.level != crumbHover_ || span.Empty()) continue;
      return prism::layout::Rect{span.left, barTop_ + 6, span.right, barBottom_ - 6};
    }
    return prism::layout::Rect{};
  }

  // How many REAL folder transitions the window performed. A click on the crumb
  // the user is already on must not move it (C93 assertion 3).
  int NavigationEvents() const { return nav_.NavigationCount(); }

  // C93 test seams: the rail's rows, and the hit box of one of them. The box is
  // the SAME RowHit command SidebarClick resolves, so the suite aims at the row
  // a user can press instead of re-deriving the rail's arithmetic.
  int SidebarRowCountForTest() { return static_cast<int>(sidebar_.Tree().size()); }
  std::string SidebarRowPathForTest(int row) {
    const std::vector<prism::sidebar::TreeRow>& rows = sidebar_.Tree();
    if (row < 0 || row >= static_cast<int>(rows.size())) return {};
    return rows[static_cast<std::size_t>(row)].path;
  }
  std::string SidebarFocusedPathForTest() { return sidebar_.FocusedPath(); }
  bool SidebarRowRectForTest(int row, int* left, int* top, int* right, int* bottom) {
    EnsureSidebar();
    for (const prism::win::sidebar::Command& command : sidebarLayout_.commands) {
      if (command.kind != prism::win::sidebar::PrimKind::RowHit) continue;
      if (command.section != prism::sidebar::Section::Tree) continue;
      if (command.payload != row) continue;
      *left = static_cast<int>(command.box.left);
      *top = static_cast<int>(command.box.top);
      *right = static_cast<int>(command.box.right);
      *bottom = static_cast<int>(command.box.bottom);
      return true;
    }
    return false;
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
  // C85 / D3: the SAME predicate the band branches on, readable by a mutant
  // build so its arm can PROVE the injection fired (a stale macro must stay
  // RED, never green). TRUE means "this Tab is a chord, the band gives it up".
  static bool TabIsChordForStripForTest() { return TabIsChordForStrip(); }
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
        // C86: remember the shadow border of a NORMAL frame while the window
        // still is one. The close path needs it to turn GetWindowPlacement's
        // normal rectangle into the visible frame -- a maximized window's own
        // extended bounds are a different number (measured on the CI runner:
        // {1,8,1,1} vs {0,0,0,0}) and mixing them moved the saved frame by 9 px.
        RememberNormalBorders();
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
        // ONLY after the minimise path really ran. CreateWindowEx sends a
        // first WM_SIZE(SIZE_RESTORED), and an ordinary Resize has a live
        // target: painting synchronously there adds a frame without a
        // WM_PAINT and breaks the "one invalidation, one draw" contract other
        // suites pin (C52 drop-in: paints 1 vs draws 2).
        if ((wparam == SIZE_RESTORED || wparam == SIZE_MAXIMIZED) && idleReleased_) {
          RestoreRenderMemory();
        }
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
        ApplySearchText(WideToUtf8(wide), /*replace=*/false);
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
        // C92: Shift+F10 and the Menu key open the SAME context menu the mouse
        // does. They arrive as WM_KEYDOWN (F10 with Shift is not a sys key);
        // WM_CONTEXTMENU(-1,-1) is what DefWindowProc would then send, so the
        // keyboard route calls that one entry point directly.
        if (wparam == VK_APPS ||
            (wparam == VK_F10 && (::GetKeyState(VK_SHIFT) & 0x8000) != 0)) {
          OpenContextMenu(-1, -1, true);
          return 0;
        }
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
      case WM_CONTEXTMENU: {
        // C92: the door that did not exist at all. Windows sends this for a
        // right-click AND for Shift+F10/the Menu key; both become the same
        // surface decision (row / empty space / sidebar node) and the same
        // TrackPopupMenu. (-1,-1) means "the keyboard asked, you pick the point".
        if (!kContextMenusEnabled) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        const int screenX = static_cast<int>(static_cast<short>(LOWORD(lparam)));
        const int screenY = static_cast<int>(static_cast<short>(HIWORD(lparam)));
        OpenContextMenu(screenX, screenY, screenX == -1 && screenY == -1);
        return 0;
      }
      case WM_MENUSELECT: {
        // C92: the reason a context item is greyed. WM_MENUSELECT is sent while
        // the pointer walks the open menu; the id is ours or it is the menu
        // bar's (owner-drawn C83 items), and only ours explain themselves.
        if (kContextMenusEnabled) {
          const UINT flags = HIWORD(wparam);
          if (!(flags == 0xFFFFu)) {  // 0xFFFF == the menu just closed
            ExplainDisabledContextItem(LOWORD(wparam));
          }
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
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
      case kThumbsReady: {
        // C76: the worker's fresh store, taken by value on the UI thread.
        std::unique_ptr<std::shared_ptr<prism::gallery::ThumbStore>> payload(
            reinterpret_cast<std::shared_ptr<prism::gallery::ThumbStore>*>(lparam));
        if (payload && *payload) {
          SetThumbs(*payload);
          Invalidate();
        }
        return 0;
      }
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
        if (glassPresentsAtFirstFrame_ < 0) {
          // C76: "was the island on screen before frame 1?" is asked HERE, at
          // the frame boundary, before this paint can present anything. Asking
          // it after the fact would count the deferral's own present.
          glassPresentsAtFirstFrame_ = glassPresents_;
        }
        AdvanceScrollSpring();
        Render();
        EndPaint(host_.hwnd, &ps);
        return 0;
      }
      case WM_ERASEBKGND:
        return 1;  // D2D paints the whole client area
      // CARD C80: every pixel below the menu bar is ours (D2D), so the client
      // area has no child HWNDs for the OS to introspect. WM_GETOBJECT is the
      // ONE door that makes painted content visible to assistive technology:
      // answer OBJID_CLIENT with an IAccessible and the platform's MSAA proxy
      // turns that tree into UIA elements (Name/Role/Bounds/Enabled/Invoke).
      // OBJID_WINDOW is deliberately NOT ours -- the frame keeps the OS object.
      case WM_GETOBJECT: {
        const LONG objectId = static_cast<LONG>(lparam);
        // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix state in one line.
        if (!prism::win::a11y::kAccessibleBridgeArmed) return 0;
        // C83: the owner-drawn bar publishes its own names (an owner-drawn item
        // has no OS string, so the system menu object would say nothing).
        if (objectId == OBJID_MENU) {
          return prism::win::a11y::HandleGetObjectFor(menuA11y_, objectId, wparam);
        }
        if (objectId != OBJID_CLIENT) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        return prism::win::a11y::HandleGetObject(a11y_, wparam, lparam);
      }
      // CARD V2: an Alt chord never reaches TranslateMessage as a plain key --
      // it arrives as WM_SYSKEYDOWN. That is the ONLY door an Alt mnemonic can
      // come through, which is why the window-level test dispatches THIS
      // message instead of setting a synthetic Alt state.
      // C83: the mnemonic of an owner-drawn item. WM_SYSCHAR is where an Alt
      // chord lands after TranslateMessage; the system cannot match a label it
      // does not store, so the window matches the painted text itself.
      case WM_SYSCHAR: {
        if (OpenMnemonic(static_cast<wchar_t>(wparam))) return 0;
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      // CARD C106: the Alt release. A BARE Alt (no key pressed while it was
      // down) flips the bar; a chord keeps its meaning and is left alone. The
      // return is 0 in both arms because DefWindowProc's WM_SYSKEYUP(VK_MENU)
      // is what enters the system menu loop -- with the bar toggling under it,
      // that loop would be the old modal menu over a bar that just vanished.
      case WM_SYSKEYUP: {
        if (wparam == VK_MENU) {
          const bool bare = prism::chrome::ShouldToggleOnAltRelease(altTracker_);
          altTracker_ = prism::chrome::AltTracker{};
          if (bare) ToggleMenuBar();
          return 0;
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
      }
      case WM_SYSKEYDOWN: {
        // C92: a bare F10 lands here; with Shift held the frame usually sends
        // WM_KEYDOWN, but a host that sends WM_SYSKEYDOWN must get the menu too.
        if (wparam == VK_F10 && (::GetKeyState(VK_SHIFT) & 0x8000) != 0) {
          OpenContextMenu(-1, -1, true);
          return 0;
        }
        // CARD C106: the Alt press itself. It is NOT the toggle -- Windows sends
        // this very message for the Alt half of Alt+F / Alt+S / Alt+H, so
        // toggling here would swallow every mnemonic the bar owns. The press only
        // arms the "bare Alt" tracker; WM_SYSKEYUP decides.
        if (wparam == VK_MENU) {
          altTracker_ = prism::chrome::AltTracker{};
          return 0;
        }
        // Any OTHER sys key while Alt is down means this was a chord, not a bare
        // Alt (F10 with Shift is handled above and is not an Alt chord).
        if ((::GetKeyState(VK_MENU) & 0x8000) != 0) {
          prism::chrome::NoteAltChordKey(&altTracker_);
        }
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
        // C93: the breadcrumb's hover feedback, from the click's own hit test.
        const int crumb = CrumbLevelAt(MouseX(lparam), MouseY(lparam));
        if (crumb != crumbHover_) {
          crumbHover_ = crumb;
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
        // C93: leaving the window drops the crumb's hover state too, otherwise
        // the highlight would freeze on whatever crumb the pointer left through.
        if (crumbHover_ != prism::tree::kNoLevel) {
          crumbHover_ = prism::tree::kNoLevel;
          Invalidate();
        }
        return 0;
      }
      case WM_SETCURSOR: {
        // C93: a crumb is a LINK. The hand pointer is the second half of "this
        // word can be pressed" (the painted hover capsule is the first).
        if (LOWORD(lparam) == HTCLIENT) {
          POINT point{};
          if (GetCursorPos(&point) && ScreenToClient(host_.hwnd, &point) &&
              CrumbLevelAt(point.x, point.y) != prism::tree::kNoLevel) {
            // IDC_HAND expands through MAKEINTRESOURCE, whose TCHAR-ness follows
            // UNICODE. This header is also compiled by test TUs that do not set
            // UNICODE, so the wide spelling is cast explicitly instead.
            SetCursor(LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_HAND)));
            return TRUE;
          }
        }
        return DefWindowProcW(host_.hwnd, message, wparam, lparam);
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
      case kHudFinish: {
        // C87: the terminal transition. Success, failure and cancel all dismiss
        // the capsule NOW; a leftover HUD after the work is the defect.
        FinishHud(static_cast<prism::HudOutcome>(wparam));
        return 0;
      }
      case kStatusText: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lparam));
        if (text) SetStatus(*text);
        return 0;
      }
      // C83: the other half of the owner-drawn bar. The system asks for a size
      // and then for pixels; both answers come from the SAME MenuDrawItem the
      // menu holds, so what is measured is exactly what is painted.
      case WM_MEASUREITEM: {
        auto* measure = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
        if (measure == nullptr || measure->CtlType != ODT_MENU) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        const MenuDrawItem* item =
            reinterpret_cast<const MenuDrawItem*>(measure->itemData);
        if (item == nullptr) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        HDC dc = ::GetDC(host_.hwnd);
        int textWidth = 0;
        int textHeight = 0;
        MeasureMenuText(dc, item->text, &textWidth, &textHeight);
        if (dc != nullptr) ::ReleaseDC(host_.hwnd, dc);
        measure->itemWidth = static_cast<UINT>(textWidth + 2 * kMenuTextPaddingX);
        measure->itemHeight = static_cast<UINT>(textHeight + 2 * kMenuTextPaddingY);
        return TRUE;
      }
      case WM_DRAWITEM: {
        auto* draw = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        if (draw == nullptr || draw->CtlType != ODT_MENU) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        const MenuDrawItem* item = reinterpret_cast<const MenuDrawItem*>(draw->itemData);
        if (item == nullptr) {
          return DefWindowProcW(host_.hwnd, message, wparam, lparam);
        }
        const prism::chrome::Palette palette =
            prism::chrome::PaletteFor(chromeVariant_);
        const bool selected = (draw->itemState & ODS_SELECTED) != 0;
        const bool disabled =
            item->disabled || (draw->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;
        HBRUSH fill = selected ? menuHighlightBrush_ : menuBrush_;
        if (fill != nullptr) ::FillRect(draw->hDC, &draw->rcItem, fill);
        RECT textRect = draw->rcItem;
        textRect.left += kMenuTextPaddingX;
        ::SetBkMode(draw->hDC, TRANSPARENT);
        ::SetTextColor(draw->hDC,
                       prism::win::chrome::ToColorRef(disabled
                                                          ? palette.textDisabled
                                                          : (selected ? palette.highlightText
                                                                      : palette.text)));
        HGDIOBJ previous = menuFont_ != nullptr ? ::SelectObject(draw->hDC, menuFont_)
                                                : nullptr;
        const std::wstring plain = MenuTextPlain(item->text);
        ::DrawTextW(draw->hDC, plain.c_str(), -1, &textRect,
                    DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);
        if (previous != nullptr) ::SelectObject(draw->hDC, previous);
        return TRUE;
      }
      case WM_COMMAND: {
        // Menu items only (the gallery owns no child controls). C53: the menu
        // bar carries Settings... (C31/V3) -- a MODAL surface, so it goes through
        // the one-overlay gate BEFORE the dialog runs. Every other id is
        // forwarded verbatim: mapping an id to an ACTION is the app's job.
        const UINT command = LOWORD(wparam);
        // C92: a context-menu id (from the test seam, an accelerator, or a host
        // that forwards the choice) reaches the same verb switch
        // TrackPopupMenu's return value does.
        if (prism::ctxmenu::IsContextCommandId(command)) {
          RunContextCommand(prism::ctxmenu::SurfaceOfCommandId(command), command);
          return 0;
        }
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
          // C86: the frame AND the maximized bit. The C66 code read the same
          // rectangle and threw the origin away, which is the whole S3-2 defect.
          closedWindow_ = prism::win::frame::SavedFrameToPersist(
              host_.hwnd, normalBordersKnown_ ? &normalBorders_ : nullptr);
          DestroyWindow(host_.hwnd);
        }
        return 0;
      case WM_DESTROY:
        // C80: the accessible tree is a view of THIS window; once the HWND is
        // gone every element a client still holds must answer nothing.
        DetachAccessibility();
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
    if (activeAfter != activeBefore) ResetHud();  // C87: no cross-tab carry-over
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
    ResetHud();
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

  // ---------- card C80: the accessibility tree and its verbs -------------
  //
  // ONE tree, built on demand from live state. Nothing is cached here on
  // purpose: a cached accessible tree is a screenshot of an interface that has
  // moved on, and the client would then press a button that no longer exists.
  prism::a11y::Node BuildAccessibleTree() {
    using prism::a11y::Node;
    using prism::a11y::Role;

    Node root;
    root.role = Role::Client;
    root.bounds =
        prism::layout::Rect{0, 0, static_cast<int>(clientW_), static_cast<int>(clientH_)};
    {
      // The window's own title is what a screen reader announces when focus
      // lands in the client area; falling back to the archive name keeps the
      // name non-empty before the first paint.
      wchar_t title[512]{};
      const int length = host_.hwnd != nullptr
                             ? ::GetWindowTextW(host_.hwnd, title, 511)
                             : 0;
      root.name = length > 0
                      ? WideToUtf8(std::wstring(title, static_cast<std::size_t>(length)))
                      : WideToUtf8(rootLabel_);
    }
    // ONE focus owner, in the order the KEY ROUTER actually uses (Proc sends
    // every key to the open search field before the band sees it): the search
    // field, else the band's ring, else the selected row, else the window.
    // accFocus answers exactly one element, because a tree where two claim
    // focus is a lie the client cannot resolve.
    const bool searchFocused = search_.IsOpen();
    const bool bandFocused = !searchFocused && actionBar_.FocusIndex() >= 0;
    root.focused = !bandFocused && !searchFocused && selection_ < 0;

    // (1) the eight actions, in BAND order -- the order the user sees, so the
    // screen reader's list matches the toolbar left to right.
    const prism::actionbar::Layout band = BandLayout();
    for (int i = 0; i < prism::actionbar::kActionCount; ++i) {
      const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(i);
      Node button;
      button.role = Role::PushButton;
      button.name = actionBar_.Label(i);  // C74: the ACTIVE language's word
      button.description = prism::actionbar::ActionName(spec.id);
      button.action = "\u6309\u4E0B";  // 按下: what accDoDefaultAction does
      button.shortcut = std::string("Alt+") + spec.mnemonic;
      button.index = i;
      // The band's ONE legality rule, not a second opinion: a disabled action
      // is UNAVAILABLE to a screen reader exactly when the band paints it grey.
      button.enabled = actionBar_.Enabled(spec.id);
      button.focused = bandFocused && actionBar_.FocusIndex() == i;
      if (i < static_cast<int>(band.buttons.size())) {
        button.bounds = band.buttons[static_cast<std::size_t>(i)].rect;
      }
      root.children.push_back(std::move(button));
    }

    // (2) the tab strip. A tab is a PAGE TAB with its file name; "selected"
    // is the active tab, and the description carries the close/dirty state a
    // PAGETAB role cannot express in a state bit.
    const std::vector<prism::win::tabbar::TabGeometry> tabRects = TabBarRects();
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
      const prism::tabs::Tab* tab = tabs_.At(i);
      if (tab == nullptr) continue;
      Node node;
      node.role = Role::PageTab;
      node.name = tab->displayName;  // the archive's file name
      node.index = i;
      node.selected = i == tabs_.activeIndex();
      node.action = "\u9009\u62E9";  // 选择
      node.description = tab->dirty ? "\u5DF2\u4FEE\u6539\uFF0C\u53EF\u5173\u95ED"
                                    : "\u53EF\u5173\u95ED";
      const prism::win::tabbar::TabGeometry* geometryOfTab = nullptr;
      for (const prism::win::tabbar::TabGeometry& geometry : tabRects) {
        if (geometry.index == i) {
          node.bounds = geometry.rect;
          geometryOfTab = &geometry;
          break;
        }
      }
      // C85: the x, as its OWN element. Measured before this: walking the UIA
      // tree stopped at "tab | Photos.7z" and the window's only "close" was the
      // title bar's button, so an assistive-technology user could open a second
      // tab and never close it. A child button of the tab (the painted x sits
      // inside the plate, so the geometry already exists) with the same verb
      // Ctrl+W runs: RequestCloseTab.
      if (kA11yTabCloseNode && geometryOfTab != nullptr) {
        Node close;
        close.role = Role::CloseButton;
        close.name = "关闭 " + tab->displayName;  // 关闭 <name>
        close.index = i;
        close.action = "关闭";  // 关闭
        close.description = "关闭这个标签";  // 关闭这个标签
        close.shortcut = "Ctrl+W";  // the chord, on the ACTIVE tab
        // Every tab can be closed; a DIRTY one asks first (C59), which is the
        // dialog verb, not a different enabled state.
        close.enabled = true;
        close.bounds = geometryOfTab->close;
        node.children.push_back(std::move(close));
      }
      root.children.push_back(std::move(node));
    }

    // (3) the search field, only while it is open. Its VALUE is the visible
    // text (an IME composition included), so the element a client reads is the
    // string the user is looking at.
    if (search_.IsOpen()) {
      const prism::win::searchbox::Box box = SearchBounds();
      Node field;
      field.role = Role::Text;
      field.name = "\u641C\u7D22\u6846";  // 搜索框
      field.value = search_.DisplayText();
      // "Press" on a field means "put the cursor in it"; the VALUE is writable
      // through put_accValue, which is the same model path a typed character
      // takes (see ApplySearchText).
      field.action = "输入";  // 输入
      field.focused = searchFocused;
      field.bounds = prism::layout::Rect{static_cast<int>(box.left), static_cast<int>(box.top),
                                        static_cast<int>(box.right),
                                        static_cast<int>(box.bottom)};
      root.children.push_back(std::move(field));
    }

    // (4) the current folder's rows. The container exists even when the list is
    // empty (a folder with no entries is not the same thing as no archive), and
    // an absent catalog simply yields zero children -- the card's "does not
    // crash" requirement is a property of the build, not of a special case.
    Node list;
    list.role = Role::List;
    list.name = breadcrumbText_.empty() ? WideToUtf8(rootLabel_)
                                        : WideToUtf8(breadcrumbText_);
    list.bounds = layout_.content;
    // The PAINTED rows, in the order they are painted: listRowToFolder_ is the
    // filtered, sorted view that list mode draws and that the hit test resolves.
    // Walking folderItems_ instead told a screen reader about rows the user
    // could not see -- the writable search field made that visible, because a
    // query narrowed the paint to one row while the tree still listed twelve.
    for (std::size_t viewRow = 0; viewRow < listRowToFolder_.size(); ++viewRow) {
      const int row = listRowToFolder_[viewRow];
      if (row < 0 || row >= ItemCount()) continue;
      const prism::ArchiveItem& item = folderItems_[static_cast<std::size_t>(row)];
      Node entry;
      entry.role = Role::ListItem;
      entry.name = A11yLeafName(item.path);
      entry.value = std::to_string(static_cast<unsigned long long>(item.size));
      entry.index = row;
      entry.action = "\u6253\u5F00";  // 打开
      entry.description = item.isDir ? "\u6587\u4EF6\u5939" : "\u6587\u4EF6";
      entry.selected = sel_.Contains(row);
      entry.focused = !bandFocused && !searchFocused && selection_ == row;
      entry.bounds = A11yRowRect(static_cast<int>(viewRow), row);
      list.children.push_back(std::move(entry));
    }
    root.children.push_back(std::move(list));

    // (4) the sidebar's archive tree. Exposed even while the rail is folded:
    // navigation by tree is exactly what a screen reader user wants, and the
    // pixels not being painted is no reason to hide the structure.
    Node outline;
    outline.role = Role::Outline;
    outline.name = WideToUtf8(rootLabel_);
    if (sidebarRailVisible_) outline.bounds = layout_.sidebar;
    const std::vector<prism::sidebar::TreeRow>& rows = sidebar_.Tree();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      Node item;
      item.role = Role::OutlineItem;
      item.name = rows[i].name;
      item.key = rows[i].path;
      item.index = static_cast<int>(i);
      item.level = rows[i].depth;
      item.expandable = rows[i].expandable;
      item.expanded = rows[i].expanded;
      item.selected = rows[i].path == nav_.Folder();
      item.action = "\u6253\u5F00";  // 打开
      item.description = rows[i].isRoot ? "\u5F52\u6863\u6839" : "\u6587\u4EF6\u5939";
      // The rail's own RowHit box is the rectangle its click test uses, so the
      // element a client is told about is the one the user can see and press.
      item.bounds = A11ySidebarRowRect(static_cast<int>(i));
      outline.children.push_back(std::move(item));
    }
    root.children.push_back(std::move(outline));

    // (5) the status line. C80 published ONE element whose name was the
    // transient status line -- measured through UIA as "status | 共 3 项" -- so
    // 未加密 / 方法未知 / 字典未知 / 条目 / 选中 / 压缩率 were painted and
    // unreachable. C86 (S3-11) publishes one child per PAINTED segment, in the
    // band's own order, with the painter's own label: the rectangles come from
    // StatusSegments(), the words from StatusSegmentLabel(), so the tree is the
    // band rather than a second opinion about it.
    Node status;
    status.role = Role::StatusBar;
    status.name = WideToUtf8(status_);
    status.value = WideToUtf8(breadcrumbText_);
    status.bounds = layout_.statusBar;
    if (kPublishStatusSegments) {
      // C86 (ASan, CI): the StatusInfo is a NAMED local on purpose. Handing
      // StatusForPublish() straight to StatusSegments() makes it a temporary
      // that dies at the end of the range-init, and any segment that views into
      // it (the ratio's text was one) is then read out of a dead stack frame.
      const prism::layout::StatusInfo info = StatusForPublish();
      const std::vector<prism::layout::StatusSegment> segments =
          prism::layout::StatusSegments(layout_, info);
      for (const prism::layout::StatusSegment& segment : segments) {
        const std::string label = prism::layout::StatusSegmentLabel(segment);
        if (label.empty()) continue;  // the divider is ink, not a reading
        Node part;
        part.role = Role::Text;
        part.name = label;
        part.value = label;
        part.index = static_cast<int>(segment.id);
        part.bounds = segment.rect;
        status.children.push_back(std::move(part));
      }
    }
    root.children.push_back(std::move(status));

    // (6) C87: the live progress capsule, published ONLY while it is painted.
    // The element and the pixels come from the same model (ProgressHud::Render),
    // so "the HUD is on screen" and "a screen reader can hear it" cannot drift.
    // name = the human-readable operation, value = the percentage -- exactly
    // what MSAA's ROLE_SYSTEM_PROGRESSBAR turns into UIA's ProgressBar element
    // with a ValuePattern.
    if (kHudPublishedToA11y) {
      const prism::ProgressHud::View hudView = hud_.Render(NowMs());
      if (hudView.phase != prism::HudPhase::Hidden) {
        Node progress;
        progress.role = Role::ProgressBar;
        progress.name = hudView.headline.empty() ? "\u8FDB\u5EA6"  // 进度
                                                  : hudView.headline;
        progress.value = hudView.percent >= 0
                             ? std::to_string(hudView.percent) + "%"
                             : std::string("\u2026");  // …
        progress.description = "\u6B63\u5728\u8FDB\u884C\u7684\u64CD\u4F5C";  // 正在进行的操作
        progress.bounds = prism::layout::ProgressCapsuleRect(layout_);
        root.children.push_back(std::move(progress));
      }
    }

    return root;
  }

  // C86: remember the border of a NORMAL frame. Called from WM_SIZE and from a
  // restore that left the window restored; a zoomed or minimized window answers
  // nothing, because its own frame is the work area, not the frame a normal
  // window would have.
  void RememberNormalBorders() {
    if (host_.hwnd == nullptr || ::IsZoomed(host_.hwnd) || ::IsIconic(host_.hwnd)) return;
    const prism::win::frame::Measurement m = prism::win::frame::Measure(host_.hwnd);
    if (!m.dwm) return;  // nothing measured: keep whatever was known
    normalBorders_ = m.borders;
    normalBordersKnown_ = true;
  }

  // A row's leaf name: catalog paths are archive-relative ("dir/file.txt"), and
  // a screen reader announcing the whole path for every row is noise.
  static std::string A11yLeafName(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= path.size()) return path;
    return path.substr(slash + 1);
  }

  // The rectangle the renderer gave this row, in the CURRENT view mode. Both
  // branches read the one layout authority, so the element a client is told
  // about is the rectangle the user can see. 'viewRow' is the position in the
  // filtered/sorted view (the list view draws it), 'folderRow' the entry.
  prism::layout::Rect A11yRowRect(int viewRow, int folderRow) {
    if (listMode_) {
      return prism::layout::ListRowRect(layout_, viewRow,
                                        static_cast<int>(viewport_.Scroll()));
    }
    return ItemRect(folderRow);
  }

  // The rail's painted row box for tree row 'row'. RowHit is the same command
  // the sidebar's own click test resolves, so the published rectangle and the
  // pressable one cannot disagree.
  prism::layout::Rect A11ySidebarRowRect(int row) {
    if (!sidebarRailVisible_) return prism::layout::Rect{};
    EnsureSidebar();
    for (const prism::win::sidebar::Command& command : sidebarLayout_.commands) {
      if (command.kind != prism::win::sidebar::PrimKind::RowHit) continue;
      if (command.section != prism::sidebar::Section::Tree) continue;
      if (command.payload != row) continue;
      return prism::layout::Rect{static_cast<int>(command.box.left),
                                 static_cast<int>(command.box.top),
                                 static_cast<int>(command.box.right),
                                 static_cast<int>(command.box.bottom)};
    }
    return prism::layout::Rect{};
  }

  // A button pressed by assistive technology. When the band painted the button,
  // this IS ActionBarClick at its centre -- including the arrow zone, the focus
  // ring and the enable matrix. Only a button the band could not fit (a window
  // too narrow for eight buttons) falls back to the mnemonic path, which is the
  // same PressAction -> FireAction chain without a pixel.
  void A11yInvokeAction(int index) {
    if (index < 0 || index >= prism::actionbar::kActionCount) return;
    const prism::actionbar::Layout band = BandLayout();
    if (index < static_cast<int>(band.buttons.size())) {
      const prism::actionbar::Rect& rect = band.buttons[static_cast<std::size_t>(index)].rect;
      if (!rect.Empty()) {
        ActionBarClick((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
        return;
      }
    }
    const prism::actionbar::Spec& spec = prism::actionbar::SpecOf(index);
    actionBar_.SetFocusIndex(index);
    const prism::actionbar::Effect effect = actionBar_.PressAction(spec.id);
    if (effect.kind == prism::actionbar::Effect::Kind::Invoke && effect.enabled) {
      FireAction(effect.action, effect.item);
    }
    Invalidate();
  }

  void A11yFocusAction(int index) {
    actionBar_.SetFocusIndex(index);
    Invalidate();
  }

  // accSelect on a row: the same two lines a click runs (the navigator keeps the
  // row Backspace restores, the selection drives the band's legality), plus the
  // repaint. "Synchronised with the window state" is that, not a second flag.
  void A11ySelectRow(int row) {
    if (row < 0 || row >= ItemCount()) return;
    nav_.SetSelection(row);
    ApplySelection(row);
  }

  void A11yActivateRow(int row) { (void)ActivateRow(row); }
  void A11yActivateTab(int index) { (void)ActivateTab(index); }
  // C85: the accessibility door to "close this tab". It is RequestCloseTab, the
  // SAME call Ctrl+W and a click on the painted x make -- including C59's dirty
  // confirmation, so an assistive-technology user is never asked less than a
  // mouse user and never loses work a mouse user would have kept.
  void A11yCloseTab(int index) {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) return;
    (void)RequestCloseTab(index);
  }

  void A11yActivateFolder(const std::string& path) {
    if (!path.empty()) (void)GoToFolder(path);
  }

  // After this, every live IAccessible answers with an empty tree and a null
  // HWND: an assistive-technology client that kept a reference across the close
  // gets no answer instead of a use-after-free.
  void DetachAccessibility() {
    if (a11y_) a11y_->Detach();
  }

  // C85: is this Tab a CHORD that belongs to the tab strip rather than to the
  // band's focus ring?
  //
  // Ctrl+Tab and Ctrl+Shift+Tab are the strip's (HandleTabKey), and Alt+Tab
  // never reaches a window at all. The band owns the BARE Tab only. Named as a
  // predicate rather than inlined because the D3 negative control has to be
  // able to invert exactly this rule and nothing else.
  static bool TabIsChordForStrip() {
#if defined(PRISM_C85_MUTANT_BAND_EATS_CTRL_TAB)
    // MUTANT: the shipped defect -- the band claims every Tab, so the chord
    // never reaches HandleTabKey.
    return false;
#else
    return (GetKeyState(VK_CONTROL) & 0x8000) != 0 || (GetKeyState(VK_MENU) & 0x8000) != 0;
#endif
  }

  // Tab / Enter / arrows / Esc -- but only for the keys the band actually owns:
  // a declined key returns false and the gallery table keeps its meaning.
  bool HandleActionBarKey(UINT vk) {
    using prism::actionbar::Effect;
    switch (vk) {
      case VK_TAB:
        // C85: a Tab with Ctrl or Alt held is a CHORD, not a traversal. This
        // branch used to return true unconditionally, so Ctrl+Tab and
        // Ctrl+Shift+Tab never reached HandleTabKey (below in HandleKey) and
        // both were dead in the shipped window -- measured with real keys:
        // the title stayed on the first archive through two presses.
        if (TabIsChordForStrip()) return false;
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
      // CARD C106: Ctrl+O. The chord asks the APP for an archive (the picker and
      // the switch path are the app's); it is deliberately NOT routed through the
      // menu bar, so it works while the bar is hidden -- which is the assertion
      // win.c106_chrome makes.
      case prism::keys::Command::OpenArchive:
        if (onOpenArchive_) onOpenArchive_();
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
      if (command.section == prism::sidebar::Section::Tree) {
        // C93: a tree row CLICK is a navigation, not a focus move (Bandizip's
        // tree has always behaved this way). The root row's folder is the EMPTY
        // path -- which is why "go to the archive root" and "do nothing" used to
        // be the same value, and the root row shipped dead.
        const prism::sidebar::TreeClickTarget plan =
            prism::sidebar::PlanTreeClick(sidebar_.Tree(), command.payload);
        if (plan.navigate) {
          const prism::tree::NavResult result = nav_.GoTo(plan.folder);
          if (result.action == prism::tree::NavAction::FolderChanged) {
            AfterFolderChange();  // user navigation: it belongs in the history
          }
        }
      }
      Invalidate();
      return true;
    }
    return true;  // inside the chrome: consumed, nothing to focus
  }

  // ---------- card C92: the in-app right-click menus ------------------------
  //
  // ONE model (PrismCore/InAppContextMenu.hpp) describes what each surface
  // offers; this file is the RENDERER: ids from CommandId(surface, slot), words
  // from the i18n table, greyed state from Facts, and the reason for a greyed
  // item from the model. Nothing here invents a verb, and nothing in the model
  // knows what an HMENU is.
  bool SidebarContains(int x, int y) const {
    EnsureSidebar();
    const prism::win::sidebar::Box& panel = sidebarLayout_.panel;
    if (panel.Empty()) return false;
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= panel.left && fx < panel.right && fy >= panel.top && fy < panel.bottom;
  }

  prism::ctxmenu::Facts ContextFacts() const {
    prism::ctxmenu::Facts facts;
    // archiveOpen_ is the window's own "an archive is open" bit (SetArchiveOpen);
    // catalog_ is the rows. Both must hold for the archive verbs to be offered.
    facts.archiveOpen = archiveOpen_ && catalog_ != nullptr;
    facts.rowCount = ItemCount();
    facts.hasSelection = !sel_.Empty();
    if (selection_ >= 0 && selection_ < ItemCount()) {
      const prism::ArchiveItem& item = folderItems_[static_cast<std::size_t>(selection_)];
      facts.singleFileSelected = !item.isDir && sel_.Size() == 1;
      facts.singleFolderSelected = item.isDir && sel_.Size() == 1;
    }
    return facts;
  }

  // Which of the three menus a client point belongs to. The sidebar rail is
  // painted OVER the grid, so it wins; the breadcrumb bar and the action band are
  // chrome and belong to no list surface, so a right-click there falls back to
  // the empty-space menu rather than pretending a row is under the pointer.
  prism::ctxmenu::Surface ContextSurfaceAt(int x, int y) {
    if (sidebarRailVisible_ && SidebarContains(x, y)) return prism::ctxmenu::Surface::Sidebar;
    if (ItemAtPoint(x, y) >= 0) return prism::ctxmenu::Surface::Row;
    return prism::ctxmenu::Surface::Empty;
  }

  // C92 follow-up: a right-click on a row the user has NOT selected selects that
  // row first, exactly like Explorer and Bandizip. Without it the first thing a
  // user tries -- right-click a file, choose Open -- is greyed out, and the
  // feature reads as broken.
  //
  // A row that is ALREADY in the selection is left alone: that is the multi-
  // select case, where re-selecting would silently throw away the other rows.
  // Empty space and the sidebar never reach this function at all, so a
  // right-click on chrome cannot clear a selection.
  void SelectRowUnderContextPoint(int x, int y) {
    const int row = ItemAtPoint(x, y);
    if (row < 0 || row >= ItemCount()) return;
    for (int selected : sel_.Selected()) {
      if (selected == row) return;  // inside the existing set: keep it
    }
    sel_.Click(row, prism::sel::ClickModifiers{});
    selection_ = sel_.Empty() ? -1 : sel_.Focus();
    nav_.SetSelection(selection_);
    SyncActionContext();
    Invalidate();
  }

  // Builds and shows the menu. The model is kept as lastContextMenu_ so a test
  // (or a future a11y bridge) reads the verbs the user was actually offered.
  void OpenContextMenu(int screenX, int screenY, bool fromKeyboard) {
    if (!kContextMenusEnabled || host_.hwnd == nullptr) return;
    int clientX = screenX;
    int clientY = screenY;
    if (fromKeyboard && screenX < 0 && screenY < 0) {
      // WM_CONTEXTMENU(-1,-1): the system asks the window where "here" is. The
      // content centre is inside the list; the surface decision below is exactly
      // the one a click at that pixel would get.
      clientX = static_cast<int>(clientW_ / 2.0f);
      clientY = static_cast<int>((layout_.content.top + layout_.content.bottom) / 2);
      POINT point{clientX, clientY};
      ClientToScreen(host_.hwnd, &point);
      screenX = point.x;
      screenY = point.y;
    } else {
      POINT point{screenX, screenY};
      ScreenToClient(host_.hwnd, &point);
      clientX = point.x;
      clientY = point.y;
    }
    const prism::ctxmenu::Surface surface = ContextSurfaceAt(clientX, clientY);
    // BEFORE the model is built: the menu's enabled state must describe the
    // selection the user is about to act on. A right-click on empty space or a
    // sidebar node never touches the selection (a click on chrome must not clear
    // it). PRISM_C92_MUTANT_NO_PRESELECT compiles the row preselection out, so
    // the "right-click an unselected row" assertion MUST go red (D3).
    if (surface == prism::ctxmenu::Surface::Row && kContextPreselectsRow) {
      SelectRowUnderContextPoint(clientX, clientY);
    }
    const prism::ctxmenu::Menu model =
        prism::ctxmenu::Build(surface, ContextFacts(), ctxLabels_);
    lastContextMenu_ = model;
    // Kept per surface: WM_MENUSELECT can name an id from the menu that is open,
    // and the reason lookup must find THAT surface's model, not whichever menu
    // was built last (they share ids only by surface band).
    if (static_cast<std::size_t>(surface) < 3) {
      lastMenuBySurface_[static_cast<std::size_t>(surface)] = model;
    }

    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) return;
    const std::size_t surfaceIndex = static_cast<std::size_t>(surface);
    if (surfaceIndex >= contextCommands_.size()) {
      ::DestroyMenu(menu);
      return;
    }
    contextCommands_[surfaceIndex].assign(model.items.size(), prism::ctxmenu::Command::None);
    ContextMenuInvocation invocation;
    invocation.surface = surface;
    invocation.model = model;
    invocation.menu = menu;
    invocation.screenPoint = POINT{screenX, screenY};
    invocation.fromKeyboard = fromKeyboard;
    for (std::size_t slot = 0; slot < model.items.size(); ++slot) {
      const prism::ctxmenu::Item& item = model.items[slot];
      if (item.separatorBefore) ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
      const UINT id = prism::ctxmenu::CommandId(surface, slot);
      contextCommands_[surfaceIndex][slot] = item.command;
      invocation.ids.push_back(id);
      ::AppendMenuW(menu,
                    MF_STRING | (item.enabled ? MF_ENABLED : MF_GRAYED), id,
                    Utf8ToWide(item.label).c_str());
    }
    // The status line explains a greyed item while the pointer is over it. The
    // previous value is restored when the menu closes.
    statusBeforeMenu_ = status_;
    if (contextProbe_) {
      contextProbe_(invocation);
      ::DestroyMenu(menu);
      return;
    }
    ++contextMenusShown_;
    if (host_.hwnd != nullptr) ::SetForegroundWindow(host_.hwnd);
    const UINT chosen = static_cast<UINT>(::TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, screenX, screenY, 0,
        host_.hwnd, nullptr));
    ::DestroyMenu(menu);
    SetStatus(statusBeforeMenu_);  // the hover explanation was temporary
    if (chosen != 0) RunContextCommand(surface, chosen);
    Invalidate();
  }

  // The ONE switch a menu selection reaches. TPM_RETURNCMD means the production
  // path arrives here with the id; WM_COMMAND (a synthesised accelerator, the
  // test seam) resolves the same id through ContextCommandForId.
  void RunContextCommand(prism::ctxmenu::Surface surface, UINT id) {
    if (!prism::ctxmenu::IsContextCommandId(id)) return;
    if (prism::ctxmenu::SurfaceOfCommandId(id) != surface) return;
    const std::size_t surfaceIndex = static_cast<std::size_t>(surface);
    const std::size_t slot = prism::ctxmenu::SlotOfCommandId(id);
    if (surfaceIndex >= contextCommands_.size() ||
        slot >= contextCommands_[surfaceIndex].size()) {
      return;
    }
    ExecuteContextCommand(contextCommands_[surfaceIndex][slot]);
  }

  void ExecuteContextCommand(prism::ctxmenu::Command command) {
    switch (command) {
      case prism::ctxmenu::Command::Open: {
        if (selection_ < 0 || selection_ >= ItemCount()) return;
        const int catalogIndex =
            static_cast<int>(folderItems_[static_cast<std::size_t>(selection_)].index);
        if (onOpen_) {
          onOpen_(catalogIndex);
        } else if (onActivate_) {
          onActivate_(catalogIndex);
        }
        return;
      }
      case prism::ctxmenu::Command::ExtractSelected:
      case prism::ctxmenu::Command::ExtractAll:
      case prism::ctxmenu::Command::SmartExtractHere: {
        if (onContextCommand_) {
          onContextCommand_(command);
          return;
        }
        // No app wiring: the verbs degrade to the doors this window already has,
        // so a menu item can never be a dead entry.
        if (command == prism::ctxmenu::Command::SmartExtractHere && onSmartExtract_ &&
            selection_ >= 0 && selection_ < ItemCount()) {
          onSmartExtract_(
              static_cast<int>(folderItems_[static_cast<std::size_t>(selection_)].index));
        } else if (onActivate_) {
          onActivate_(-1);
        } else {
          SetStatus(std::wstring(L"\u672A\u63A5\u7EBF\uFF1A") +
                    Utf8ToWide(prism::ctxmenu::CommandName(command)));
        }
        return;
      }
      case prism::ctxmenu::Command::CopyPath: {
        std::wstring text;
        if (selection_ >= 0 && selection_ < ItemCount()) {
          text = Utf8ToWide(folderItems_[static_cast<std::size_t>(selection_)].path);
        }
        if (text.empty()) text = archivePath_;
        if (text.empty()) {
          SetStatus(L"\u6CA1\u6709\u53EF\u590D\u5236\u7684\u8DEF\u5F84");  // 没有可复制的路径
          return;
        }
        CopyTextToClipboard(text);
        SetStatus(std::wstring(L"\u5DF2\u590D\u5236\u8DEF\u5F84\uFF1A") + text);  // 已复制路径：
        return;
      }
      case prism::ctxmenu::Command::RevealArchive:
        RevealArchiveInExplorer();
        return;
      case prism::ctxmenu::Command::SelectAll: {
        sel_.SelectAll();
        selection_ = sel_.Empty() ? -1 : sel_.Focus();
        nav_.SetSelection(selection_);
        SyncActionContext();
        Invalidate();
        return;
      }
      case prism::ctxmenu::Command::Refresh:
        Invalidate();
        return;
      case prism::ctxmenu::Command::None:
      case prism::ctxmenu::Command::Count:
        return;
    }
  }

  bool CopyTextToClipboard(const std::wstring& text) {
    if (host_.hwnd == nullptr || !::OpenClipboard(host_.hwnd)) return false;
    ::EmptyClipboard();
    const std::size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL block = ::GlobalAlloc(GMEM_MOVEABLE, bytes);
    bool stored = false;
    if (block != nullptr) {
      void* target = ::GlobalLock(block);
      if (target != nullptr) {
        std::memcpy(target, text.c_str(), bytes);
        ::GlobalUnlock(block);
        stored = ::SetClipboardData(CF_UNICODETEXT, block) != nullptr;
      }
      if (!stored) ::GlobalFree(block);
    }
    ::CloseClipboard();
    return stored;
  }

  void RevealArchiveInExplorer() {
    if (archivePath_.empty()) {
      SetStatus(L"\u5C1A\u672A\u6253\u5F00\u5F52\u6863");  // 尚未打开归档
      return;
    }
    // /select needs the path quoted; the archive is the only thing Explorer can
    // show, because the selected ENTRY lives inside it.
    const std::wstring arguments = L"/select,\"" + archivePath_ + L"\"";
    ::ShellExecuteW(nullptr, L"open", L"explorer.exe", arguments.c_str(), nullptr,
                    SW_SHOWNORMAL);
  }

  // A greyed context item explains itself while hovered; the words are the
  // model's reason string (which is empty for every ENABLED item by rule).
  bool ExplainDisabledContextItem(UINT id) {
    if (!prism::ctxmenu::IsContextCommandId(id)) return false;
    const std::size_t surface = static_cast<std::size_t>(prism::ctxmenu::SurfaceOfCommandId(id));
    const std::size_t slot = prism::ctxmenu::SlotOfCommandId(id);
    if (surface >= 3 || slot >= lastMenuBySurface_[surface].items.size()) return false;
    const prism::ctxmenu::Item& item = lastMenuBySurface_[surface].items[slot];
    if (item.enabled || item.disabledReason.empty()) return false;
    SetStatus(Utf8ToWide(item.disabledReason));
    return true;
  }

  // The real Windows menu bar, rebuilt from the labels the app supplied.
  //
  // C83: every item is MFT_OWNERDRAW. The system still owns the BAR (SetMenu,
  // the menu loop, the Alt mnemonics, the UIA MenuBar object); the window owns
  // the PIXELS. A system-painted bar is light on every Windows build, which is
  // what made the top 64 px look like a different application (UI-AUDIT A4).
  //
  // The text is stored in MenuDrawItem, because an owner-drawn item's item data
  // IS its text as far as the OS is concerned: the pointer must outlive the
  // menu, so the items live in menuItems_ and are swapped in only AFTER the old
  // menu is destroyed.
  //
  // NEGATIVE CONTROL (CONTRACTS D3): PRISM_C83_MUTANT_NO_OWNERDRAW appends the
  // items as MF_STRING, i.e. the pre-fix system-painted bar. The suite's
  // "menu bar background == the dark token" assertion MUST go red.
  void RebuildMenu() {
    if (!host_.hwnd) return;  // Create() installs it once the HWND exists
    EnsureMenuFont();
    const prism::chrome::Palette palette = prism::chrome::PaletteFor(chromeVariant_);
    // C106: the plate is the SAME colour the caption was given, through the same
    // function (prism::chrome::ShellSurface -- see WindowChrome::Apply). It used
    // to be a second PaletteFor(variant).background lookup in a second file: the
    // two values agreed, but nothing made them agree.
    //
    // NEGATIVE CONTROL (CONTRACTS D3): PRISM_C106_MUTANT_WHITE_BAR pins the plate
    // to white -- the plan's "突兀白条" -- while the caption keeps the theme's
    // colour. The three-theme menu==title pixel gate MUST go red; the mutation
    // verdict is armed on the OBSERVABLE proof that the injection ran (a white
    // plate in a theme whose ShellSurface is not white).
    const prism::chrome::Rgba plate = MenuWhiteBarMutant()
                                          ? prism::chrome::Rgba(1.0f, 1.0f, 1.0f, 1.0f)
                                          : prism::chrome::ShellSurface(chromeVariant_);
    HBRUSH background = CreateSolidBrush(prism::win::chrome::ToColorRef(plate));
    HBRUSH highlight =
        CreateSolidBrush(prism::win::chrome::ToColorRef(palette.highlight));
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    HMENU edit = CreatePopupMenu();
    if (!bar || !file || !edit) {
      if (bar) DestroyMenu(bar);
      if (file) DestroyMenu(file);
      if (edit) DestroyMenu(edit);
      if (background) DeleteObject(background);
      if (highlight) DeleteObject(highlight);
      return;
    }
    std::vector<std::unique_ptr<MenuDrawItem>> items;
    std::vector<MenuDrawItem*> barItems;
    // One door for every item: the text is owned here, the HMENU keeps the
    // pointer, and MF_OWNERDRAW is what makes WM_DRAWITEM ours.
    const auto addItem = [&items](HMENU menu, UINT flags, UINT_PTR id,
                                  const std::wstring& text) -> bool {
      auto item = std::make_unique<MenuDrawItem>();
      item->text = text;
      item->popup = (flags & MF_POPUP) != 0;
      item->command = item->popup ? 0u : static_cast<UINT>(id);
      MenuDrawItem* raw = item.get();
      items.push_back(std::move(item));
#if defined(PRISM_C83_MUTANT_NO_OWNERDRAW)
      // NEGATIVE CONTROL (CONTRACTS D3): the system paints the item again.
      return AppendMenuW(menu, flags & ~static_cast<UINT>(MF_OWNERDRAW), id,
                         reinterpret_cast<LPCWSTR>(raw->text.c_str())) != FALSE;
#else
      return AppendMenuW(menu, flags | MF_OWNERDRAW, id,
                         reinterpret_cast<LPCWSTR>(raw)) != FALSE;
#endif
    };
    addItem(file, MF_STRING, kMenuCompress, compressMenuLabel_);
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    addItem(file, MF_STRING, kMenuExit, exitMenuLabel_);
    // C54's Edit menu. A disabled entry is GREY, and RequestEdit still answers
    // with the readable reason if a chord reaches it anyway.
    addItem(edit, MF_STRING, kMenuEditDelete, editDeleteLabel_);
    addItem(edit, MF_STRING, kMenuEditRename, editRenameLabel_);
    addItem(edit, MF_STRING, kMenuEditNewFolder, editNewFolderLabel_);
    if (!editEnabled_) {
      EnableMenuItem(edit, kMenuEditDelete, MF_BYCOMMAND | MF_GRAYED);
      EnableMenuItem(edit, kMenuEditRename, MF_BYCOMMAND | MF_GRAYED);
      EnableMenuItem(edit, kMenuEditNewFolder, MF_BYCOMMAND | MF_GRAYED);
    }
    addItem(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(file), fileMenuLabel_);
    barItems.push_back(items.back().get());
    addItem(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), editMenuLabel_);
    barItems.push_back(items.back().get());
    // C53: C31/V3's Settings item lives on the SAME bar, not on a second menu
    // that would REPLACE this one. (SettingsDialog::InstallMenu builds a whole
    // bar of its own; Main only falls back to it when the window has no menu.)
    addItem(bar, MF_STRING, prism::win::settingsui::SettingsDialog::kMenuSettingsId,
            settingsMenuLabel_);
    barItems.push_back(items.back().get());
    // C57: Help > About. The label is literal here like "Settings..." above: the
    // item is chrome on this window, and the dialog it opens is where the words
    // that matter (the report) live.
    HMENU help = CreatePopupMenu();
    if (help != nullptr) {
      addItem(help, MF_STRING, prism::win::about::AboutDialog::kMenuAboutId,
              aboutMenuLabel_);
      addItem(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(help), helpMenuLabel_);
      barItems.push_back(items.back().get());
    }
    HMENU previous = GetMenu(host_.hwnd);
    // C106: a HIDDEN bar is parked, not destroyed. Its items are owner-drawn and
    // their text lives in menuItems_, so the HMENU (and the UIA object built from
    // it) must survive the hide: Alt has to bring back the SAME bar, with the
    // same item order, not a rebuilt lookalike.
    HMENU parked = parkedMenu_;
    if (menuBarVisible_) {
      if (!SetMenu(host_.hwnd, bar)) {
        DestroyMenu(bar);
        if (background) DeleteObject(background);
        if (highlight) DeleteObject(highlight);
        return;
      }
      parkedMenu_ = nullptr;
    } else {
      // No SetMenu: the bar does not reserve non-client space while it is down.
      parkedMenu_ = bar;
    }
    // The old bars are gone before their item data is: DestroyMenu first, THEN
    // the swap that frees the previous MenuDrawItems.
    if (previous) DestroyMenu(previous);
    if (parked) DestroyMenu(parked);
    menuItems_ = std::move(items);
    menuBarItems_ = std::move(barItems);
    HBRUSH oldBackground = menuBrush_;
    HBRUSH oldHighlight = menuHighlightBrush_;
    menuBrush_ = background;
    menuHighlightBrush_ = highlight;
    if (oldBackground) DeleteObject(oldBackground);
    if (oldHighlight) DeleteObject(oldHighlight);
    // The plate BETWEEN owner-drawn items (padding, separators, the dropdown's
    // border) is painted by the system with THIS brush; the items paint their
    // own rectangles in WM_DRAWITEM.
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = menuBrush_;
    SetMenuInfo(bar, &info);
    // A parked bar has no window to draw on: DrawMenuBar on a bar the window does
    // not own is a no-op at best and a stale non-client repaint at worst.
    if (menuBarVisible_) DrawMenuBar(host_.hwnd);
  }

  // C83: the menu's font. The system's menu font (not a hardcoded face), so the
  // owner-drawn bar keeps the size the user configured in Windows.
  void EnsureMenuFont() {
    if (menuFont_ != nullptr) return;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
      menuFont_ = ::CreateFontIndirectW(&metrics.lfMenuFont);
    }
    if (menuFont_ == nullptr) {
      menuFont_ = static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    }
  }

  // The mnemonic marker is not drawn: "&File" paints as "File", "&&" as "&".
  static std::wstring MenuTextPlain(const std::wstring& text) {
    std::wstring plain;
    plain.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] != L'&') {
        plain.push_back(text[i]);
        continue;
      }
      if (i + 1 < text.size() && text[i + 1] == L'&') {
        plain.push_back(L'&');
        ++i;
      }
    }
    return plain;
  }

  void MeasureMenuText(HDC dc, const std::wstring& text, int* width, int* height) const {
    if (width == nullptr || height == nullptr) return;
    *width = 0;
    *height = 0;
    if (dc == nullptr) return;
    const std::wstring plain = MenuTextPlain(text);
    HGDIOBJ previous = menuFont_ != nullptr ? ::SelectObject(dc, menuFont_) : nullptr;
    SIZE size{};
    if (::GetTextExtentPoint32W(dc, plain.c_str(), static_cast<int>(plain.size()),
                                &size) != FALSE) {
      *width = static_cast<int>(size.cx);
      *height = static_cast<int>(size.cy);
    }
    if (previous != nullptr) ::SelectObject(dc, previous);
  }

  void ReleaseMenuResources() {
    if (menuFont_ != nullptr) {
      ::DeleteObject(menuFont_);
      menuFont_ = nullptr;
    }
    if (menuBrush_ != nullptr) {
      ::DeleteObject(menuBrush_);
      menuBrush_ = nullptr;
    }
    if (menuHighlightBrush_ != nullptr) {
      ::DeleteObject(menuHighlightBrush_);
      menuHighlightBrush_ = nullptr;
    }
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
    // C74: the words are the values ApplyLanguage() copied in, not a lookup in
    // a global translator slot (that slot could outlive its owner).
    sidebarLayout_ = prism::win::sidebar::Build(sidebar_, metrics, sidebarLabels_);
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
    SyncSidebarToFolder();     // C93: the tree highlight IS the current folder
  }

  // C93: the rail's highlight is the CURRENT FOLDER, not "the last row the user
  // pressed". Without this the tree kept painting payload_text after Backspace
  // had already taken the list back to the archive root -- the second half of
  // the field report's "the tree still says I am in the subfolder".
  void SyncSidebarToFolder() {
    const std::vector<prism::sidebar::TreeRow>& rows = sidebar_.Tree();
    for (std::size_t i = 0; i < rows.size(); ++i) {
      if (rows[i].path != nav_.Folder()) continue;
      const bool moved = sidebar_.SetFocus(prism::sidebar::FocusTarget{
          prism::sidebar::Section::Tree, static_cast<int>(i)});
      if (moved) sidebarDirty_ = true;  // the painted Selection moved with it
      return;
    }
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
    // C116: a rebuilt scene asks for the frame that shows it. A window whose
    // rows were just replaced but that is not dirty keeps the previous frame on
    // screen until something unrelated invalidates it; that is the "the model
    // is complete, the pixels are stale" state the card is about. The request
    // goes through Invalidate(), the one door the mutant closes.
    if (kRepaintRequestsArmed) Invalidate();
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
    // C86 (S3-4): the crumb's trailing count belongs to the CURRENT folder. It
    // used to be whatever the app had last written into status_ -- "共 16 项",
    // the archive total, set once at open -- so a two-entry subfolder read
    // "… › docs › 共 16 项" while the status band said 条目 2. The window owns
    // the number now, because the window is what knows which folder is on
    // screen; the total survives in parentheses.
    if (archiveOpen_ && catalog_ != nullptr) {
      const std::string count = prism::layout::BreadcrumbCountText(
          ItemCount(), static_cast<int>(catalog_->size()), nav_.Folder().empty());
      if (!count.empty()) {
        crumbSegments_.push_back({std::string(prism::tree::kBreadcrumbSeparator),
                                  prism::tree::kNoLevel});
        crumbSegments_.push_back({count, prism::tree::kNoLevel});
      }
    }
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

  // DirectWrite measurement in the renderer's crumb format (the resolved stack,
  // 15 px semi-bold). Without a factory every width is 0 -> no crumb is clickable,
  // which degrades to "Backspace still works" instead of navigating blind.
  int MeasureUtf8(std::string_view text) {
    EnsureTextMeasurement();
    if (!crumbFormat_) return 0;
    const std::wstring wide = Utf8ToWide(text);
    if (wide.empty()) return 0;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(prism::win::typography::CreateTextLayout(
            writeFactory_.Get(), wide.c_str(), static_cast<UINT32>(wide.size()),
            crumbFormat_.Get(), 8192.0f, 64.0f, layout.GetAddressOf()))) {
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
    prism::win::typography::CreateTextFormat(
        writeFactory_.Get(), 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
        L"en-US", crumbFormat_.ReleaseAndGetAddressOf());
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
      {
        PRISM_C76_SPAN("renderer_device_resources",
                       "GalleryRenderer::CreateDeviceResources (first frame)");
        rendererReady_ = SUCCEEDED(renderer_.CreateDeviceResources(target_.Get()));
      }
      if (!rendererReady_) return;
    }
    if (!DrawFrame(target_.Get())) return;
    AfterWindowFrame();
  }

  // C76: the frame body, independent of WHICH target it paints into. The window
  // passes its HwndRenderTarget; the first-frame digest test passes an offscreen
  // one. "The pixels are the same" is therefore a claim about THIS code path,
  // not about a lookalike the harness re-implements.
  bool DrawFrame(ID2D1RenderTarget* target) {
    if (!target) return false;
    prism::win::startup::Trace& trace = prism::win::startup::Trace::Instance();
    const double frameStart = trace.NowMs();
    target->BeginDraw();
    {
      // C76: everything between BeginDraw and renderer_.Draw -- the sidebar
      // command list, the tab view, the HUD snapshot.
      PRISM_C76_SPAN("scene_build", "GalleryWindow::Render: scene assembly");
      EnsureSidebar();  // the commands are painted, not computed by the renderer
    }
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
    // C77: the glass layer. BOTH flags come from the appearance setting, which
    // defaults ON -- a window that shipped with the refraction layer off would
    // have shipped the pre-C77 look (a translucent panel over a flat colour).
    scene.glassPanels = prism::theme::BackdropRefractionEnabled();
    scene.backdropRefraction = scene.glassPanels;
    // C112: WHO PAINTS THE BREADCRUMB. While the island is composited, the pill
    // -- fill, rim, hover capsule and TEXT -- belongs to it. The window keeps
    // the drop shadow and the hit-test geometry; it must NOT paint the text,
    // because the island's backdrop is a capture of THIS frame and the optics
    // would blur that text and paint the blur back over it (the C107 ghost).
    //
    // ... but only once the island has actually PRESENTED a frame (C76 defers
    // the composition's first present to the second paint). Ownership that
    // started one frame early would leave the pill EMPTY on the first frame -- a
    // hole where the breadcrumb is -- because the window would have stopped
    // painting it while the island above it was not there yet. Both arms paint
    // the same pixels, so the handover frame is invisible (C76's "every
    // differing pixel sits inside the island" invariant).
    scene.pillOwnedByIsland = GlassPillOwnsContent() && glassPresents_ > 0;
    // C91: the frame's ONE theme input. It is the same field the shell paints
    // from (chromeVariant_), so a theme switch can no longer reach the caption
    // and stop there.
    scene.theme = chromeVariant_;
    // V4: the status band's four segments read the archive properties the
    // engine reported (encryption from SetArchiveInfo, the rest from
    // SetArchiveStatus). Unknown is a state, not an empty strip.
    // C86: the SAME facts the accessibility tree publishes (counts filled in).
    scene.status = StatusForPublish();
    // C-V2: the band. The model is the SAME object the click path hit-tests, so
    // a button can never be painted somewhere it cannot be pressed.
    scene.actions = &actionBar_;
    scene.actionHover = actionHover_;
    scene.actionPressed = actionPressed_;
    // C93: the crumb under the pointer, and the exact span to highlight.
    scene.crumbHoverLevel = crumbHover_;
    if (crumbHover_ != prism::tree::kNoLevel) scene.crumbHoverRect = CrumbHoverRect();
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
        scene.quickLookItem = static_cast<int>(item.index);
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
    {
      PRISM_C76_SPAN("renderer_draw", "GalleryRenderer::Draw (rows+rails+bands)");
      renderer_.Draw(target, scene);
    }
    // The two overlays below write through the WINDOW's target on purpose: they
    // are transient UI that never appears in a first frame, and an offscreen
    // digest render must not touch the window's device context.
    if (target == target_.Get()) DrawSearchBox();
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
    if (target == target_.Get()) DrawDropHint();
#if defined(PRISM_C113_MUTANT_BLACK_FRAME)
    // NEGATIVE CONTROL (CONTRACTS D3/D26/D27): a frame EndDraw ACCEPTS that
    // carries no scene at all. The honesty suite's non-empty assertion
    // (painted >= N, digest != 0) MUST go red; the verdict names this mutant.
    target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f));
#endif
    // C113: EndDraw IS the verdict. For an HwndRenderTarget it is the submit;
    // for a WIC target it is where D2D reports that the frame was rejected
    // (D2DERR_WRONG_RESOURCE_DOMAIN). Dropping this HRESULT is what let a
    // rejected frame travel on as frame.rendered == true (see RenderSceneInto).
#if defined(PRISM_C113_MUTANT_SILENT_FAILURE)
    // NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the pre-C113 product, restored --
    // the HRESULT is dropped and EVERY frame, accepted or rejected, answers
    // true. The digest arm of the C113 suite reads an empty frame through this
    // and MUST go red; so does H1 of the honesty arm.
    target->EndDraw();
    lastRenderError_ = S_OK;
#else
    const HRESULT endDraw = target->EndDraw();
    lastRenderError_ = endDraw;
    if (FAILED(endDraw)) return false;
#endif
    // C76: the first frame's own render cost, on the same clock as every other
    // stage. EndDraw is the submit for an HwndRenderTarget.
    trace.Add("first_frame_render", "BeginDraw+draw+EndDraw", frameStart,
              trace.NowMs() - frameStart);
    return true;
  }

  // C76: what the WINDOW does after a frame it painted itself (never for the
  // offscreen digest path): report the first frame once, and present the glass
  // island on the SECOND frame when the deferral is on.
  void AfterWindowFrame() {
    if (glass_ && glass_->Visible()) {
      if (glassPresentPending_ && paintMessages_ < 2) {
        // Ask for the frame that carries the glass. Without this the island
        // would wait for the next unrelated repaint -- "deferred" must mean
        // "next frame", not "eventually".
        Invalidate();
      } else {
        // C112: EVERY window frame re-composes the island, not just the first
        // one. The pill's TEXT lives in this surface now; a one-shot present
        // would freeze the breadcrumb at whatever it said the first time the
        // window painted (navigate into a folder and the bar would never
        // change). The backdrop is captured and the chain re-run per frame,
        // which is what C107's "captured every frame" already described; the
        // capture bitmap is reused, so this is not an allocation per frame.
        glassPresentPending_ = false;
        PresentGlassFrame();
      }
    } else if (glassPresentPending_) {
      Invalidate();
    }
    if (!firstFrameReported_) {
      firstFrameReported_ = true;
      ReportFirstFrame();
      // C76: work that does not change one pixel of the first frame runs HERE,
      // after it: the session's crash-guard write and the background thumbnail
      // sweep. Both measured, both were on the pre-card critical path.
      if (onFirstFrame_) {
        auto handler = onFirstFrame_;
        handler();
      }
    }
  }

 public:
  // The app's "the first frame is on screen" door. Post-first-frame work is not
  // a timer guess: it is the exact frame boundary.
  void SetFirstFrameHandler(std::function<void()> handler) { onFirstFrame_ = std::move(handler); }

 private:

  // One line of evidence + the whole table. PRISM_C76_TRACE gates it; a shipping
  // run therefore prints nothing and pays one branch.
  void ReportFirstFrame() {
    prism::win::startup::Trace& trace = prism::win::startup::Trace::Instance();
    if (!trace.Enabled()) return;
    // A COUNT, not a duration: it is the deterministic half of the evidence and
    // the predicate the "glass on the first frame" mutant has to break.
    std::fprintf(stderr, "C76_GLASS {\"presents_before_first_frame\":%d}\n", glassPresents_);
    trace.Finish("app");
    if (trace.ExitAfterFirstFrame() && host_.hwnd) {
      // Measurement only: the bench needs a REAL process that ends after the
      // first frame rather than a WM_CLOSE posted at a guessed moment.
      PostMessageW(host_.hwnd, WM_CLOSE, 0, 0);
    }
  }

 public:
  // C76 seams. Above they are private because nothing outside the window may
  // report or present on its behalf; these three are what the digest suite reads.
  //
  // The renderer's brushes are DEVICE-DEPENDENT, so they are recreated against
  // whatever target is being painted and then restored for the window's own. A
  // brush drawn on a target that did not create it is undefined behaviour, and
  // the digest would be a picture of the wrong thing.
  bool RenderSceneInto(ID2D1RenderTarget* target) {
    lastRenderError_ = S_OK;
    if (!target) {
      lastRenderError_ = E_POINTER;
      return false;
    }
    // C113: the failure is REPORTED, never absorbed. When this window has
    // painted for real, the renderer's cached C77 backdrop bitmap belongs to
    // the HWND target's device; a WIC target is a different device, and D2D
    // rejects the frame at EndDraw with D2DERR_WRONG_RESOURCE_DOMAIN
    // (0x88990012). That is a KNOWN, ADMITTED fact, not an occasional glitch:
    // H1 of the C113 suite pins it, and the C76 bench renders its digest frame
    // BEFORE the first WM_PAINT for exactly this reason.
    renderer_.CreateDeviceResources(target);
    const bool drawn = DrawFrame(target);
    // The round trip is part of the seam: the cache now belongs to the
    // target's device, so drop it before the window paints again -- otherwise
    // the NEXT window frame would sample the foreign backdrop and fail one
    // frame late.
    renderer_.ReleaseDeviceResources();
    if (target_) renderer_.CreateDeviceResources(target_.Get());
    return drawn;
  }
  // C113: the CAPTURE seam -- the one a digest/screenshot caller must use.
  //
  // RenderSceneInto above deliberately keeps its pre-C113 shape (paint with the
  // renderer's CURRENT device cache) so the cross-device failure stays
  // observable: that is H1 of the C113 suite, a measured 0x88990012. A capture
  // is a different contract -- the picture must be the scene, not a rejection
  // -- so this seam builds the renderer's device objects on the target it is
  // given, paints, and then hands the device back to the window. Without the
  // release/re-create, the SECOND capture into a fresh WIC target is rejected
  // too (the C113 probe measured 0x88990012 on every render after the first),
  // which is how a digest ends up hashing a frame D2D threw away.
  bool RenderSceneForCapture(ID2D1RenderTarget* target) {
    lastRenderError_ = S_OK;
    if (!target) {
      lastRenderError_ = E_POINTER;
      return false;
    }
#if defined(PRISM_C113_MUTANT_SILENT_FAILURE)
    // NEGATIVE CONTROL (CONTRACTS D3/D26/D27): the pre-C113 capture, restored --
    // paint with whatever device cache the renderer already holds (after a real
    // WM_PAINT that cache lives on the window's device, so the WIC frame is
    // rejected with D2DERR_WRONG_RESOURCE_DOMAIN), and DrawFrame drops that
    // HRESULT like it used to. The pixel arm must then read an empty frame and
    // go red; nothing here is allowed to be reported as a picture.
    renderer_.CreateDeviceResources(target);
    (void)DrawFrame(target);
    renderer_.ReleaseDeviceResources();
    if (target_) renderer_.CreateDeviceResources(target_.Get());
    return true;
#else
    renderer_.ReleaseDeviceResources();
    renderer_.CreateDeviceResources(target);
    const bool drawn = DrawFrame(target);
    renderer_.ReleaseDeviceResources();
    if (target_) renderer_.CreateDeviceResources(target_.Get());
    return drawn;
#endif
  }
  // C113: the HRESULT the last render really returned from EndDraw. S_OK for a
  // frame that was accepted; D2DERR_WRONG_RESOURCE_DOMAIN for the cross-device
  // case RenderSceneInto documents; E_POINTER for a null target.
  HRESULT LastRenderError() const { return lastRenderError_; }
  int GlassPresents() const { return glassPresents_; }
  int GlassPresentsAtFirstFrame() const { return glassPresentsAtFirstFrame_; }
  // C113: present ONE island frame through the product's own PresentGlassFrame,
  // without waiting for a WM_PAINT. A capture suite needs the steady state (the
  // island composited) BEFORE it renders the window's frame offscreen: after a
  // real paint the renderer's cached C77 backdrop lives on the window's device
  // and D2D refuses to sample it from a WIC target (D2DERR_WRONG_RESOURCE_DOMAIN,
  // measured -- see RenderSceneInto). The seam keeps the production path --
  // capture, constants, chain, panel chrome -- and only changes who calls it.
  bool PresentGlassFrameForTest() {
    if (!glass_) return false;
    const int before = glassComposedFrames_;
    PresentGlassFrame();
    return glassComposedFrames_ > before;
  }
  bool FirstFrameReported() const { return firstFrameReported_; }
  // C87: how many Quick Look frames painted DECODED pixels (see the renderer's
  // counter). The suite asserts this alongside the pixel variance so "the card
  // is not a stand-in" is proven twice, from two directions.
  int QuickLookThumbDraws() const { return renderer_.dbgQuickLookThumbDraws; }

 private:
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
    // C76: PRESENT IMMEDIATELY. The default present waits for the vertical
    // blank, so the FIRST EndDraw can block for up to one refresh interval
    // (16.7 ms at 60 Hz) on a frame nobody has seen yet. The pixels are the
    // same; only the wait before returning from Present disappears.
    if (FAILED(factory_->CreateHwndRenderTarget(props,
                                                D2D1::HwndRenderTargetProperties(
                                                    host_.hwnd,
                                                    D2D1::SizeU(static_cast<UINT32>(rc.right),
                                                                static_cast<UINT32>(rc.bottom)),
                                                    D2D1_PRESENT_OPTIONS_IMMEDIATELY),
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
    // C76: an explicit override first. The installed layout puts the effects in
    // <exe>\Assets\Shaders and needs nothing else; a build tree (and the C76
    // bench, which launches the real exe) can point at the checkout's assets
    // without a copy step, which is also what makes the shader measurement
    // describe the shipped bytes instead of a "file not found" fallback.
    wchar_t overridePath[MAX_PATH]{};
    const DWORD overrideLength = GetEnvironmentVariableW(L"PRISM_SHADER_DIR", overridePath, MAX_PATH);
    if (overrideLength > 0 && overrideLength < MAX_PATH) return std::wstring(overridePath);
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
    ++glassPresents_;
    // C107: the island is the panel the LAYOUT gives us (the breadcrumb pill),
    // not a 960x48 rectangle at (0,0) that covered nothing in particular.
    const prism::glass::IslandRect rect = GlassPanelRect();
    if (rect.w == 0 || rect.h == 0) return;
    if (!glass_->ResizeIsland(0, rect.w, rect.h)) return;

    // 1. THE BACKDROP. The window has just painted this frame, so the pixels
    // under the panel are on screen (and on the window's own surface). Captured
    // every frame, handed to the chain -- never nullptr, which is what kept the
    // two shaders from ever executing.
    const bool captured = glass_->GrabBackdrop(host_.hwnd, rect);
    // 2. THE CONSTANTS. The same GlassIslandScene::WriteCbuffer the shader's
    // cbuffer is bit-asserted against, at the panel's own size, so the
    // refraction band and the blur agree about where the edge is.
    prism::win::glass::GlassConstants constants{};
    BuildGlassConstants(rect, &constants);
    // 3. THE COMPOSITION: backdrop -> Kawase -> Kawase -> LiquidGlass -> panel
    //    chrome, into the island's premultiplied swapchain.
    glassLastFrameOptical_ = captured && glass_->OpticsLive();
    const bool composed =
        glass_->PaintIsland(0, rect, constants, captured ? glass_->Backdrop() : nullptr,
                            [this](ID2D1DeviceContext* dc, float width, float height) {
                              PaintIslandContent(dc, width, height);
                            });
    if (composed) ++glassComposedFrames_;
  }

  // C112: EVERYTHING the island shows for one frame, in island-local pixels.
  // The order is the contract: the optics are already on the surface when this
  // runs (PaintIslandInto draws the chain first), so whatever this paints is on
  // top of the glass and is never fed back into it. That is why the TEXT can be
  // sharp here and can never be blurred by the backdrop capture.
  void PaintIslandContent(ID2D1DeviceContext* dc, float width, float height) {
    if (!dc || width <= 0.0f || height <= 0.0f) return;
    if (GlassPillOwnsContent()) {
      // C112: the whole pill, into the island's own surface. The optics are
      // already underneath (PaintIslandInto draws the chain first), so this text
      // is painted OVER the glass and stays sharp -- and because it is painted
      // AFTER the chain ran, it can never become part of the next backdrop.
      renderer_.PaintPillOnIsland(dc, breadcrumbText_, layout_, &actionBar_, chromeVariant_,
                                  CrumbHoverRect(), prism::theme::BackdropRefractionEnabled());
      return;
    }
    // Without composition there is no island above the window: the window still
    // paints the pill (its own DrawBreadcrumbIsland), and this call only has the
    // rim to add to the transparent surface.
    PaintGlassPanelChrome(dc, width, height, chromeVariant_);
  }

  // C107: the island's own chrome -- the specular rim and the hairline border,
  // from the SAME panel language the window paints (GlassPanel). The fill and
  // the shadow are deliberately NOT painted again: they are part of what the
  // glass refracts, and a second fill here would double the material.
  void PaintGlassPanelChrome(ID2D1DeviceContext* dc, float width, float height,
                             prism::theme::Variant variant) {
    if (!dc || width <= 0.0f || height <= 0.0f) return;
    if (!glassRimBrush_) {
      if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                                           glassRimBrush_.ReleaseAndGetAddressOf()))) {
        return;
      }
    }
    prism::win::glass::PanelPainter painter;
    painter.Bind(dc, glassRimBrush_.Get());
    prism::win::glass::Parts parts;
    parts.shadow = false;
    parts.fill = false;
    parts.rim = true;
    parts.border = true;
    painter.PaintRim(dc, D2D1::RectF(0.0f, 0.0f, width, height), variant, parts, nullptr);
    painter.Unbind();
  }

  // The shader's 20 floats, from the model that owns them.
  void BuildGlassConstants(const prism::glass::IslandRect& rect,
                           prism::win::glass::GlassConstants* constants) {
    if (!constants) return;
    glassScene_.Layout(rect.w, rect.h);
    glassScene_.SetTheme(chromeVariant_ == prism::theme::Variant::Daylight
                             ? prism::glass::Theme::Daylight
                             : prism::glass::Theme::Midnight);
    glassScene_.SetPointer(static_cast<float>(rect.w) * 0.5f, static_cast<float>(rect.h) * 0.5f);
    glassScene_.WriteCbuffer(prism::glass::IslandId::Nav, rect.w, rect.h, constants->v);
    // The model's Nav rect is its own idea of where the island is; the LAYOUT's
    // panel is the authority here, so the center/half-size are overwritten with
    // the surface's own geometry (the bitmap's origin is the panel's corner).
    constants->v[0] = static_cast<float>(rect.w) * 0.5f;
    constants->v[1] = static_cast<float>(rect.h) * 0.5f;
    constants->v[2] = static_cast<float>(rect.w) * 0.5f;
    constants->v[3] = static_cast<float>(rect.h) * 0.5f;
    constants->v[8] = static_cast<float>(rect.h) * 0.5f;
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

  // ---------- card C80: ONE write path for the query ----------------------
  //
  // A typed character (WM_CHAR) and an assistive-technology VALUE write
  // (IAccessible::put_accValue, i.e. UIA's ValuePattern.SetValue) must not be
  // two implementations: both end HERE, on the search model's own editing
  // functions -- InsertText for one code unit, SetQuery for a whole replacement
  // -- and on the SAME ApplySearchFilter. "The list narrowed" therefore means
  // exactly the same thing however the text arrived.
  void ApplySearchText(const std::string& utf8, bool replace) {
    if (replace) {
      search_.SetQuery(utf8);  // the model's whole-string update (caret included)
      ApplySearchFilter();
      Invalidate();
      return;
    }
    const prism::search::Effect effect = search_.InsertText(utf8);
    if (!effect.queryChanged) {
      Invalidate();
      return;
    }
    ApplySearchFilter();
    Invalidate();
  }

  // The accessible verbs for the field. Writing its value is the product's own
  // state change (SetQuery + the one filter), and "default action" puts the
  // keyboard into it -- never a second copy of the query.
  void SetSearchValueFromAccessible(const std::string& utf8) {
    if (!search_.IsOpen()) return;
    ApplySearchText(utf8, /*replace=*/true);
  }
  void FocusSearchField() {
    if (!search_.IsOpen()) return;
    search_.SetFocused(true);
    Invalidate();
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

  // C116: the ONE door every repaint request goes through, so "this window was
  // told its pixels are stale" is a single, inspectable fact. The mutant below
  // closes the door: the model still changes and WM_PAINT still paints whatever
  // is in the scene -- but no frame is ever requested, which is the reported
  // "the model is complete and the screen is empty" state.
  void Invalidate() {
    if (!kRepaintRequestsArmed) return;
    if (host_.hwnd) InvalidateRect(host_.hwnd, nullptr, FALSE);
  }

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

  // ---------- card C80: the bridge's view of THIS window -------------------
  //
  // Nested so it reaches the private helpers without widening the public
  // surface, and shared (not a raw pointer) so an IAccessible a client still
  // holds after the window dies answers "empty" instead of following a dead
  // pointer. Detach() is the one write that makes that guarantee true.
  struct A11yHost final : prism::win::a11y::Host {
    explicit A11yHost(GalleryWindow* owner) : window(owner) {}

    HWND Window() const override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      return owner != nullptr ? owner->host_.hwnd : nullptr;
    }
    prism::a11y::Node Tree() override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      return owner != nullptr ? owner->BuildAccessibleTree() : prism::a11y::Node{};
    }
    void InvokeAction(int index) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yInvokeAction(index);
    }
    void FocusAction(int index) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yFocusAction(index);
    }
    void SelectRow(int row) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11ySelectRow(row);
    }
    void ActivateRow(int row) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yActivateRow(row);
    }
    void ActivateTab(int index) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yActivateTab(index);
    }
    void CloseTab(int index) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yCloseTab(index);
    }
    void ActivateFolder(const std::string& path) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->A11yActivateFolder(path);
    }
    void SetSearchValue(const std::string& utf8) override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->SetSearchValueFromAccessible(utf8);
    }
    void FocusSearchField() override {
      GalleryWindow* owner = window.load(std::memory_order_acquire);
      if (owner != nullptr) owner->FocusSearchField();
    }

    void Detach() { window.store(nullptr, std::memory_order_release); }

    std::atomic<GalleryWindow*> window;
  };

  // C83: the MENU BAR as an accessible object.
  //
  // An owner-drawn item has no OS string, so the system menu object publishes
  // four UNNAMED items -- measured with agent-desktop after the first
  // owner-draw build. That is the regression the card forbids. The window
  // therefore answers OBJID_MENU with a tree built from the SAME MenuDrawItems
  // the painter uses, so a screen reader hears the labels the user sees. The
  // real HMENU keeps the keyboard, the mouse and the menu loop.
  class MenuA11yHost final : public prism::win::a11y::Host {
   public:
    explicit MenuA11yHost(GalleryWindow* window) : window_(window) {}
    HWND Window() const override {
      return window_ != nullptr ? window_->host_.hwnd : nullptr;
    }
    prism::a11y::Node Tree() override {
      return window_ != nullptr ? window_->MenuA11yTree() : prism::a11y::Node{};
    }
    void InvokeAction(int index) override {
      if (window_ != nullptr) window_->InvokeMenuA11y(index);
    }
    void FocusAction(int) override {}
    void SelectRow(int) override {}
    void ActivateRow(int) override {}
    void ActivateTab(int) override {}
    // C85: the menu bar's tree publishes MENU items, never a tab's x -- but the
    // verb still forwards to the window's ONE close door, so a client that
    // reaches a CloseButton through this host cannot silently do nothing.
    void CloseTab(int index) override {
      if (window_ != nullptr) window_->A11yCloseTab(index);
    }
    void ActivateFolder(const std::string&) override {}
    void SetSearchValue(const std::string&) override {}
    void FocusSearchField() override {}

   private:
    GalleryWindow* window_;
  };

  // One node per top-level item, in bar order, with the SCREEN rectangle
  // GetMenuItemRect reports converted to client coordinates (the menu bar sits
  // above the client, so its y is negative -- that is a real location, not an
  // empty one).
  prism::a11y::Node MenuA11yTree() {
    prism::a11y::Node root;
    root.role = prism::a11y::Role::MenuBar;
    root.name = "PrismArchive";
    HMENU bar = host_.hwnd != nullptr ? ::GetMenu(host_.hwnd) : nullptr;
    POINT origin{0, 0};
    if (host_.hwnd != nullptr) ::ClientToScreen(host_.hwnd, &origin);
    root.children.reserve(menuBarItems_.size());
    for (std::size_t i = 0; i < menuBarItems_.size(); ++i) {
      const MenuDrawItem* item = menuBarItems_[i];
      if (item == nullptr) continue;
      prism::a11y::Node node;
      node.role = prism::a11y::Role::MenuItem;
      node.name = WideToUtf8(MenuTextPlain(item->text));
      node.index = static_cast<int>(i);
      node.enabled = !item->disabled;
      node.expandable = item->popup;
      node.expanded = false;
      if (bar != nullptr) {
        RECT rect{};
        if (::GetMenuItemRect(host_.hwnd, bar, static_cast<UINT>(i), &rect) != FALSE) {
          node.bounds = prism::layout::Rect{rect.left - origin.x, rect.top - origin.y,
                                            rect.right - origin.x, rect.bottom - origin.y};
        }
      }
      root.children.push_back(std::move(node));
    }
    return root;
  }

  // C83: owner-drawn items have no OS text, so DefWindowProc's mnemonic lookup
  // cannot match Alt+F. The window matches the SAME strings the bar paints; a
  // popup opens through the system menu loop (arrows, Enter and Esc are still
  // the OS's), a plain item posts its WM_COMMAND. True means "consumed".
  bool OpenMnemonic(wchar_t letter) {
    if (host_.hwnd == nullptr) return false;
    HMENU bar = ::GetMenu(host_.hwnd);
    if (bar == nullptr) return false;
    for (std::size_t i = 0; i < menuBarItems_.size(); ++i) {
      const MenuDrawItem* item = menuBarItems_[i];
      if (item == nullptr || !prism::chrome::MnemonicMatches(item->text, letter)) continue;
      if (!item->popup) {
        if (item->command != 0) {
          ::PostMessageW(host_.hwnd, WM_COMMAND, MAKEWPARAM(item->command, 0), 0);
        }
        return true;
      }
      RECT rect{};
      if (::GetMenuItemRect(host_.hwnd, bar, static_cast<UINT>(i), &rect) == FALSE) {
        return true;
      }
      HMENU popup = ::GetSubMenu(bar, static_cast<int>(i));
      if (popup == nullptr) return true;
      const UINT command = static_cast<UINT>(::TrackPopupMenu(
          popup, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RETURNCMD | TPM_NONOTIFY, rect.left,
          rect.bottom, 0, host_.hwnd, nullptr));
      if (command != 0) {
        ::PostMessageW(host_.hwnd, WM_COMMAND, MAKEWPARAM(command, 0), 0);
      }
      return true;
    }
    return false;
  }

  // A plain item runs its command through the SAME WM_COMMAND door the mouse
  // uses; a popup item expands, which is the menu loop's job, not ours.
  void InvokeMenuA11y(int index) {
    if (index < 0 || index >= MenuBarItemCount() || host_.hwnd == nullptr) return;
    const MenuDrawItem* item = menuBarItems_[static_cast<std::size_t>(index)];
    if (item == nullptr || item->popup || item->command == 0) return;
    ::PostMessageW(host_.hwnd, WM_COMMAND, MAKEWPARAM(item->command, 0), 0);
  }

  // ---------------------------------------------------------------- state
  static constexpr double kWheelScrollPixels = 120.0 * 3.0;

  // ---- card C83: the shell ----
  // One item of the owner-drawn bar. The HMENU holds a POINTER to this, so the
  // object lives in menuItems_ and is only freed after the menu that points at
  // it was destroyed (see RebuildMenu).
  struct MenuDrawItem {
    std::wstring text;
    bool disabled = false;
    bool popup = false;   // opens a submenu (the bar's File/Edit/Help)
    UINT command = 0;     // the WM_COMMAND id a plain item runs
  };
  static constexpr int kMenuTextPaddingX = 10;
  static constexpr int kMenuTextPaddingY = 4;
  prism::theme::Variant chromeVariant_ = prism::theme::Variant::Midnight;
  prism::win::chrome::TitleBarRecord titleBar_;
  // C83: the bar's accessible object. Created with the window like a11y_, and
  // safe to outlive it (Host::Window() answers null once the HWND is gone).
  std::shared_ptr<MenuA11yHost> menuA11y_ = std::make_shared<MenuA11yHost>(this);
  std::vector<std::unique_ptr<MenuDrawItem>> menuItems_;  // every item, alive with the bar
  std::vector<MenuDrawItem*> menuBarItems_;               // the four top-level items
  HFONT menuFont_ = nullptr;
  HBRUSH menuBrush_ = nullptr;
  HBRUSH menuHighlightBrush_ = nullptr;
  // ---- card C106: the bar's visibility ----
  // menuBarVisible_ is the LIVE state; menuAutoHide_ is the preference it was
  // seeded from. Keeping them apart is what lets Alt show the bar "temporarily"
  // without rewriting what the user asked for.
  bool menuBarVisible_ = true;
  bool menuAutoHide_ = false;
  HMENU parkedMenu_ = nullptr;  // the bar while it is down (alive, not installed)
  prism::chrome::AltTracker altTracker_;  // bare Alt vs Alt+F
  std::function<void()> onOpenArchive_;   // Ctrl+O, wired by the app

  // C83: re-apply the caption and rebuild the bar for the current variant.
  void ApplyChromeTheme() {
    if (!host_.hwnd) return;
    titleBar_ = prism::win::chrome::Apply(host_.hwnd, chromeVariant_);
    RebuildMenu();
    Invalidate();
  }

  // C80: created with the window, detached when the HWND dies.
  std::shared_ptr<A11yHost> a11y_ = std::make_shared<A11yHost>(this);

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
  // ---- card C92: open + right-click state ----
  // "Open" (Enter / double-click on a file) has its own door; onActivate_ stays
  // the whole-archive path the context menu's ExtractAll still uses.
  std::function<void(int)> onOpen_;
  std::function<void(prism::ctxmenu::Command)> onContextCommand_;
  std::function<void(const ContextMenuInvocation&)> contextProbe_;
  prism::ctxmenu::Labels ctxLabels_;
  prism::ctxmenu::Menu lastContextMenu_;
  prism::ctxmenu::Menu lastMenuBySurface_[3];
  // id -> verb per surface, filled when the popup is built. The vector is sized
  // to the model, so an id from a PREVIOUS menu whose slot no longer exists
  // resolves to None instead of indexing past the end.
  std::vector<std::vector<prism::ctxmenu::Command>> contextCommands_{3};
  std::wstring archivePath_;
  std::wstring statusBeforeMenu_;
  int contextMenusShown_ = 0;
  // The app's persisted column geometry; the renderer paints it verbatim.
  prism::list::ColumnLayout listColumns_;
  bool hasListColumns_ = false;
  // Captured in WM_CLOSE (the only moment the real frame still exists).
  // C86: origin + size + maximized, in one value -- the app persists all three.
  prism::layout::SavedWindow closedWindow_;
  // C86: the shadow border measured while the window was NORMAL. A maximized
  // window's own extended bounds are not the normal frame's (see the close path).
  prism::layout::FrameBorders normalBorders_{};
  bool normalBordersKnown_ = false;
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
  int crumbHover_ = prism::tree::kNoLevel;  // C93: crumb under the pointer
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
  // C74: the section headings + telemetry words, resolved once per language.
  prism::win::sidebar::Labels sidebarLabels_;
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
  // ---- card C107: the optics chain's per-frame state ----
  prism::glass::GlassIslandScene glassScene_;        // the cbuffer's owner
  ComPtr<ID2D1SolidColorBrush> glassRimBrush_;       // the island's specular rim
  int glassComposedFrames_ = 0;                      // islands the chain composed
  bool glassLastFrameOptical_ = false;               // last frame had a backdrop
  size_t glassReleasedBytes_ = 0;  // C33's accounting, last release
  size_t lastThumbTrimBytes_ = 0;  // bytes the idle Trim returned
  // ---- card C75: idle/release accounting ----
  size_t lastReleasedTargetBytes_ = 0;  // back buffer the last release dropped
  int idleReleases_ = 0;                // times the minimise path really ran
  int restoreRebuilds_ = 0;             // Resize callbacks that rebuilt + painted
  bool idleReleased_ = false;           // released and not yet rebuilt/painted

  // ---- card C76: the first frame's evidence ----
  // glassPresentPending_ is the deferral: the island's first present waits for
  // the second frame (EnableGlassLayer sets it; AfterWindowFrame clears it).
  bool glassPresentPending_ = false;
  bool firstFrameReported_ = false;
  std::function<void()> onFirstFrame_;  // C76: post-first-frame work (app-owned)
  int glassPresents_ = 0;               // every PresentGlassFrame call, counted
  int glassPresentsAtFirstFrame_ = -1;  // the count when the first frame reported
  // ---- card C113: a failed frame may never look like a successful one ----
  // The HRESULT the last DrawFrame really got from EndDraw. Until C113 the
  // value was dropped on the floor ("target->EndDraw();" and then "return
  // true"), so a frame D2D rejected -- D2DERR_WRONG_RESOURCE_DOMAIN above all,
  // when a cached device object from the HWND target is sampled by a WIC target
  // -- came back as a SUCCESS carrying nothing but the canvas clear. Render
  // paths now hand it to LastRenderError() and callers must assert on it.
  HRESULT lastRenderError_ = S_OK;
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
