// PrismArchive -- PrismWin
// Companion preview window (blueprint ch. 4.1 / milestone M2): the
// non-activating Liquid Glass flyout that shows the inline preview next to the
// Explorer context menu.
//
// Card C30 wires the flyout to the shell: the payload now arrives over the
// shared-memory channel declared in CompanionPreviewBridge.hpp (the shell
// extension writes it, this window reads and renders it), and the window is
// placed next to the click with a right-edge flip. The window itself stays the
// M2 GDI skeleton -- the DirectComposition optics land in M3 -- but the
// CONTRACT that survives that upgrade is here: no activation, always inside the
// work area, closes on ESC / deactivation, and never blocks the shell.
//
// Focus rules (both are asserted by Tests/win/test_companion_preview.cpp):
//   * created with WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
//   * shown/moved with SetWindowPos(SWP_NOACTIVATE) / SW_SHOWNOACTIVATE only,
//   * WM_MOUSEACTIVATE answers MA_NOACTIVATE, so a click into the flyout cannot
//     pull focus out of the Explorer window that owns the menu.
// NOREDIRECTIONBITMAP is deliberate: the window has no redirection surface by
// design -- its pixels come from a DirectComposition island. Card C48 supplies
// that island (FlyoutSurface.hpp): Create() attaches one, WM_PAINT paints the
// glass through it, and the GDI body below survives as the documented fallback
// for a machine where composition is unavailable (remote sessions, composition
// disabled). The window is therefore a lifecycle shell PLUS real pixels, and
// Tests/win/test_flyout_paint.cpp proves the pixels offscreen.
//
// GEOMETRY: the row model is FlyoutSurface's Metrics (DPI-scaled), not a second
// set of constants. At 96 dpi the metrics reproduce kHeaderHeight/kRowHeight
// exactly, which is what lets C30's hit-test expectations keep holding on any
// machine: SetDpi() is an explicit OPT-IN the host performs, never something
// Create() infers, so a HiDPI desktop cannot silently move another card's rows.
#pragma once

#include <windows.h>
#include <windowsx.h>  // GET_WHEEL_DELTA_WPARAM / GET_X_LPARAM

#include <cstdint>
#include <string>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"
#include "CompanionPreviewBridge.hpp"
#include "FlyoutSurface.hpp"  // C48: the DComp island + the D2D content painter

namespace prism::win::companion {

class CompanionWindow {
 public:
  static constexpr wchar_t kClassName[] = L"PrismArchiveCompanionPreview";

  // The 96-dpi DESIGN values (also the row model the hover hit-test uses). The
  // live numbers come from flyout::MetricsForDpi(dpi_) -- these constants are
  // what that function returns at 96 dpi, and what C30's suite pins.
  static constexpr int kHeaderHeight = flyout::kBaseHeaderHeight;  // 24
  static constexpr int kRowHeight = flyout::kBaseRowHeight;        // 18
  // One screenful, exactly the packet's entry budget (ch. 4.1: "first 12").
  static constexpr int kMaxVisibleRows = flyout::kMaxRows;  // 12

  // The focus policy in ONE place: the window creation, the test assertion and
  // the negative-control mutant all read this function, so "created without
  // NOACTIVATE" and "asserted without NOACTIVATE" cannot drift apart.
  static DWORD ExStylePolicy() noexcept {
#ifdef PRISM_C30_MUTANT_DROP_NOACTIVATE
    // MUTATION: the flyout is created as an ordinary activating popup.
    return WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
#else
    return WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP;
#endif
  }

  bool RegisterClass(HINSTANCE instance) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    // IDC_ARROW expands to MAKEINTRESOURCEA when UNICODE is not defined for this
    // TU; name the wide integer directly so the call stays ASCII/Unicode-proof.
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
    // No activation on click: the flyout must never steal focus from Explorer.
    wc.style = CS_HREDRAW | CS_VREDRAW;
    return RegisterClassExW(&wc) != 0;
  }

  bool Create(HINSTANCE instance, int width, int height) {
    hwnd_ = CreateWindowExW(
        ExStylePolicy(), kClassName, L"PrismArchive Preview", WS_POPUP, CW_USEDEFAULT,
        CW_USEDEFAULT, width, height, nullptr, nullptr, instance, this);
    if (hwnd_ == nullptr) return false;
    // C48: attach the DirectComposition island. Failure is a supported outcome
    // (remote session / composition disabled): surfaceReady_ stays false and
    // WM_PAINT keeps its GDI body, so the flyout degrades instead of vanishing.
    surfaceReady_ = SUCCEEDED(surface_.Initialize()) &&
                    surface_.InitializeComposition(hwnd_, dpi_);
    return true;
  }

  // ---- DPI (C48) --------------------------------------------------------
  // Explicit opt-in, deliberately NOT inferred in Create(): the host knows when
  // the flyout is per-monitor DPI aware, and a test that pins 96-dpi rows must
  // not turn red on a 125% desktop.
  void SetDpi(int dpi) {
    dpi_ = dpi < 48 ? 48 : (dpi > 480 ? 480 : dpi);
    ApplyContentSize();
  }
  void SetDpiFromWindow() {
    if (hwnd_ != nullptr) SetDpi(static_cast<int>(::GetDpiForWindow(hwnd_)));
  }
  int Dpi() const noexcept { return dpi_; }
  flyout::Metrics Metrics() const { return flyout::MetricsForDpi(dpi_); }

  // The theme whose C15 tokens the island resolves (the app runs dark glass).
  void SetVariant(prism::theme::Variant variant) noexcept { variant_ = variant; }
  prism::theme::Variant Variant() const noexcept { return variant_; }

  bool UsesComposition() const noexcept { return surfaceReady_; }
  const flyout::FlyoutSurface& Surface() const noexcept { return surface_; }

  // The window is exactly as tall as the rows it shows (header + N * pitch) and
  // always 360 wide: the peek never grows past one screenful.
  void ApplyContentSize() {
    if (hwnd_ == nullptr) return;
    const flyout::Metrics m = Metrics();
    const int height = m.HeightForRows(static_cast<int>(entries_.size()));
    SetWindowPos(hwnd_, nullptr, 0, 0, m.width, height,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOZORDER);
    if (surfaceReady_) surface_.Resize(m.width, height);
  }

  // ---- model inputs -----------------------------------------------------
  void SetCatalogItem(const prism::ArchiveItem& item, uint64_t previewLimit) {
    const auto decision = prism::QuickLookPlanner::Decide(item, previewLimit);
    kindLine_ = KindLabel(decision.kind);
    if (decision.exceedsLimit) kindLine_ += L"  (large -- preview capped)";
  }

  // Payload from the shell extension. Rejects a torn / foreign-layout packet and
  // keeps the previous list instead of painting half of the new one.
  bool ApplyPreview(const PreviewPacket& packet) {
    if (!ValidPacket(packet)) return false;
    archivePath_ = ArchivePathOf(packet);  // bounded: the section is untrusted
    entries_.clear();
    entries_.reserve(packet.entryCount);
    for (uint32_t i = 0; i < packet.entryCount; ++i) entries_.push_back(packet.entries[i]);
    cursorX_ = packet.cursorX;
    cursorY_ = packet.cursorY;
    needsScan_ = (packet.flags & kFlagNeedsCatalogScan) != 0;
    scroll_ = 0;
    hover_ = -1;
    // C48: the island's height is the payload's height (clamped to 12 rows), so
    // the window and its pixels agree about what "one screenful" means.
    ApplyContentSize();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
  }

  void SetProgress(prism::ProgressHud& hud, uint64_t nowMs) {
    const auto view = hud.Render(nowMs);
    switch (view.phase) {
      case prism::HudPhase::Hidden:
        progressLine_.clear();
        break;
      case prism::HudPhase::Compact:
        progressLine_ = L"Progress " + std::to_wstring(view.percent) + L"%";
        break;
      case prism::HudPhase::Expanded:
        progressLine_ = WideFromUtf8(view.headline) + L"  " + std::to_wstring(view.percent) +
                        L"%  ETA " + std::to_wstring(static_cast<int>(view.etaSeconds)) + L"s";
        break;
      case prism::HudPhase::Done:
        progressLine_ = L"Done";
        break;
    }
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  // ---- placement --------------------------------------------------------
  // Where the flyout belongs for a click at cursorScreen inside workArea. Pure
  // geometry lives in the bridge header (ComputeFlyoutRect) so it is testable
  // without a window; this only remembers the answer.
  FlyoutRect PlacementFor(POINT cursorScreen, RECT workArea) noexcept {
    RECT client{0, 0, 0, 0};
    SIZE size{};
    if (hwnd_) {
      GetWindowRect(hwnd_, &client);
      size.cx = client.right - client.left;
      size.cy = client.bottom - client.top;
    }
    placement_ = ComputeFlyoutRect(cursorScreen, size, workArea);
    return placement_;
  }

  // Move + show WITHOUT activation: SWP_NOACTIVATE is the Win32 proof that this
  // path cannot take focus (SetWindowPos refuses to activate on request here).
  void PlaceForCursor(POINT cursorScreen, RECT workArea) {
    const FlyoutRect rect = PlacementFor(cursorScreen, workArea);
    if (!hwnd_) return;
    SetWindowPos(hwnd_, nullptr, rect.left, rect.top, rect.right - rect.left,
                 rect.bottom - rect.top, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
  }

  FlyoutRect Placement() const noexcept { return placement_; }

  // ---- interaction state ------------------------------------------------
  void ScrollBy(int rows) noexcept {
    int target = scroll_ + rows;
    const int count = static_cast<int>(entries_.size());
    if (target > count - 1) target = count > 0 ? count - 1 : 0;
    if (target < 0) target = 0;
    if (target == scroll_) return;
    scroll_ = target;
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  int ScrollOffset() const noexcept { return scroll_; }

  // Row under a client-space y, in ENTRIES_ index space (scroll offset applied).
  // -1 for the header, an empty list, or past the last row.
  int HitTestRow(int clientY) const noexcept {
    const flyout::Metrics m = Metrics();
    if (clientY < m.headerHeight) return -1;
    const int row = (clientY - m.headerHeight) / m.rowHeight;
    if (row < 0 || row >= kMaxVisibleRows) return -1;
    const int index = scroll_ + row;
    if (index < 0 || index >= static_cast<int>(entries_.size())) return -1;
    return index;
  }

  void SetHoverFromClient(int clientY) noexcept {
    const int hit = HitTestRow(clientY);
    if (hit == hover_) return;
    hover_ = hit;
    InvalidateRect(hwnd_, nullptr, TRUE);
  }

  int HoveredIndex() const noexcept { return hover_; }
  const std::wstring& ArchivePath() const noexcept { return archivePath_; }
  size_t EntryCount() const noexcept { return entries_.size(); }
  bool NeedsCatalogScan() const noexcept { return needsScan_; }
  POINT Cursor() const noexcept {
    POINT p{};
    p.x = cursorX_;
    p.y = cursorY_;
    return p;
  }

  void Show() const {
    if (hwnd_) ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
  }
  void Hide() const {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
  }
  bool IsVisible() const { return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE; }
  HWND Hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<CompanionWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE && lparam) {
      self = reinterpret_cast<CompanionWindow*>(
          reinterpret_cast<const CREATESTRUCTW*>(lparam)->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(hwnd, msg, wparam, lparam);
    switch (msg) {
      case WM_MOUSEACTIVATE:
        // A click inside the flyout must leave the Explorer window activated:
        // the preview is a peek, not a window the user can "enter".
#ifdef PRISM_C30_MUTANT_DROP_NOACTIVATE
        return MA_ACTIVATE;  // MUTATION
#else
        return MA_NOACTIVATE;
#endif
      case WM_ACTIVATE:
        // Losing activation closes the peek (blueprint ch. 4.1); with
        // WS_EX_NOACTIVATE this only fires when the owning app goes away.
        if (LOWORD(wparam) == WA_INACTIVE) {
          self->Hide();
          return 0;
        }
        break;
      case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
          self->Hide();
          return 0;
        }
        break;
      case WM_MOUSEWHEEL:
        // One entry per notch; GET_WHEEL_DELTA_WPARAM is signed.
        self->ScrollBy(-GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA);
        return 0;
      case WM_MOUSEMOVE:
        self->SetHoverFromClient(GET_Y_LPARAM(lparam));
        return 0;
      case WM_ERASEBKGND:
        return 1;  // painted in WM_PAINT; no flicker between the two
      case WM_PAINT:
        self->OnPaint();
        return 0;
      case WM_DESTROY:
        return 0;
      default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
  }

  // The scene the island paints: the payload's rows, in the flyout's own model.
  // Building it here (not inside FlyoutSurface) keeps the surface free of the
  // IPC contract and keeps the conversion -- bounded name reads, the 12-row
  // budget, the scroll offset -- in the one place that owns the packet.
  flyout::Scene BuildScene() const {
    flyout::Scene scene;
    scene.variant = variant_;
    scene.dpi = dpi_;
    scene.title = archivePath_.empty() ? kindLine_ : archivePath_;
    scene.rows.reserve(static_cast<size_t>(kMaxVisibleRows));
    for (int row = 0; row < kMaxVisibleRows; ++row) {
      const int index = scroll_ + row;
      if (index < 0 || index >= static_cast<int>(entries_.size())) break;
      const PreviewEntry& entry = entries_[static_cast<size_t>(index)];
      flyout::Row out;
      out.name = EntryName(entry);  // bounded: never a raw C-string read
      out.sizeBytes = entry.sizeBytes;
      out.directory = entry.kind == static_cast<uint32_t>(PreviewEntryKind::Directory);
      scene.rows.push_back(std::move(out));
    }
    if (!scene.rows.empty()) {
      scene.subtitle = std::to_wstring(scene.rows.size()) + L" items";
    } else if (needsScan_) {
      scene.subtitle = L"scanning";
    }
    // The hover index is in ENTRY space; the scene wants the visible row.
    const int hoverRow = hover_ >= 0 ? hover_ - scroll_ : -1;
    scene.hover = hoverRow >= 0 && hoverRow < static_cast<int>(scene.rows.size()) ? hoverRow : -1;
    return scene;
  }

  void OnPaint() {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd_, &ps);
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    // C48: the real pixels. The island is composited by DWM (the window has no
    // redirection bitmap), so this only has to hand the painter the scene --
    // BeginPaint/EndPaint still validate the update region.
    if (surfaceReady_ && surface_.Paint(BuildScene())) {
      EndPaint(hwnd_, &ps);
      return;
    }
    // Fallback: no composition on this machine. Legible GDI text, no glass.
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x20, 0x24, 0x2E));
    const std::wstring header = archivePath_.empty() ? kindLine_ : archivePath_;
    RECT headerRect = rc;
    headerRect.bottom = headerRect.top + kHeaderHeight;
    DrawTextW(dc, header.c_str(), -1, &headerRect,
              DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

    const int visible = static_cast<int>(entries_.size()) - scroll_;
    for (int row = 0; row < visible && row < kMaxVisibleRows; ++row) {
      const int index = scroll_ + row;
      RECT rowRect = rc;
      rowRect.top = kHeaderHeight + row * kRowHeight;
      rowRect.bottom = rowRect.top + kRowHeight;
      if (rowRect.top >= rc.bottom) break;
      if (index == hover_) {
        HBRUSH brush = CreateSolidBrush(RGB(0xE8, 0xEE, 0xF9));
        FillRect(dc, &rowRect, brush);
        DeleteObject(brush);
      }
      const PreviewEntry& entry = entries_[static_cast<size_t>(index)];
      std::wstring line = EntryName(entry);  // bounded: never a raw C-string read
      if (entry.kind == static_cast<uint32_t>(PreviewEntryKind::Directory)) line += L"  <dir>";
      RECT textRect = rowRect;
      textRect.left += 8;
      DrawTextW(dc, line.c_str(), -1, &textRect,
                DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    if (!progressLine_.empty()) {
      RECT below = rc;
      below.top = kHeaderHeight + kMaxVisibleRows * kRowHeight;
      DrawTextW(dc, progressLine_.c_str(), -1, &below, DT_LEFT | DT_TOP | DT_SINGLELINE);
    }
    EndPaint(hwnd_, &ps);
  }

  static const wchar_t* KindLabel(prism::PreviewKind kind) {
    switch (kind) {
      case prism::PreviewKind::Image: return L"Image preview";
      case prism::PreviewKind::Text: return L"Text preview";
      case prism::PreviewKind::Pdf: return L"PDF preview";
      case prism::PreviewKind::Media: return L"Media preview";
      case prism::PreviewKind::Directory: return L"Folder";
      case prism::PreviewKind::None: return L"No preview available";
    }
    return L"";
  }

  static std::wstring WideFromUtf8(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                         static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(wide, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        out.data(), wide);
    return out;
  }

  HWND hwnd_ = nullptr;
  std::wstring kindLine_;
  std::wstring progressLine_;
  std::wstring archivePath_;
  std::vector<PreviewEntry> entries_;
  bool needsScan_ = false;
  int32_t cursorX_ = 0;
  int32_t cursorY_ = 0;
  int scroll_ = 0;
  int hover_ = -1;
  int dpi_ = 96;  // 96 until the host opts in (see SetDpi)
  prism::theme::Variant variant_ = prism::theme::Variant::Midnight;
  flyout::FlyoutSurface surface_;
  bool surfaceReady_ = false;
  FlyoutRect placement_{};
};

}  // namespace prism::win::companion
