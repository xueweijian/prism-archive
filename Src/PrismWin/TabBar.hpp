// PrismArchive -- PrismWin
// C60: THE TAB STRIP -- the pixels and the hit test for C59's TabSet.
//
// C59 shipped the MODEL (Src/PrismCore/TabSet.hpp: which archives are open, in
// which order, which one is active, what closing does, when a dirty tab needs a
// confirmation). The window still showed ONE archive at a time because nothing
// drew the strip. This header is that missing half, and it owns EXACTLY two
// things:
//
//   1. GEOMETRY -- where every tab, its title and its close button sit. The
//      band itself comes from prism::layout::Compute (WindowLayout.hpp), which
//      is the one geometry authority (DESIGN.md §2.1: 32 px, between the title
//      and the action island, hidden when no archive is open). Nothing here
//      invents an absolute coordinate.
//   2. PIXELS -- the active tab's SurfaceFloatingIsland plate, the inactive
//      canvas plate, the dirty dot behind the title and the "x" close glyph.
//      Every colour is a C15 TOKEN (prism::theme::Lookup); a literal RGBA in
//      this file would be a design-drift bug, so there are none.
//
// It decides NO rule: "which tab is active after closing", "does a dirty tab
// ask first", "11 tabs means tab 0" are C59's answers and this file only paints
// what TabSet already said. The window (GalleryWindow) owns the TabSet and calls
// into this header for rects and clicks.
//
// The two D3 negative controls (Tests/suites/c60-tabbar.cmake) are compile-time
// switches consumed HERE, because this card is where the rules become visible:
//   PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR -> the strip keeps C59's injected
//       "closing activates tab 0" rule (kCloseKeepsRightNeighbour is false, so
//       the suite's right-neighbour case MUST go red);
//   PRISM_C60_MUTANT_DIRTY_NO_CONFIRM -> the window closes a dirty tab without
//       asking (kDirtyNeedsConfirm is false, so the suite's cancel case MUST go
//       red).
// Both are VALUES, not #ifdef islands, so the clean build still type-checks the
// statement each mutant disables -- an injected branch that no longer compiles
// proves nothing (CONTRACTS D3).
//
// Contracts touched: D1 (the stdout marker is the verdict), D3 (controls must be
// able to go red for the INJECTED reason), B1 (parenthesised std::min/max: this
// header is included by TUs that also include windows.h), B2 (self-contained
// includes), D26/D27 (marker as a conjunction, printed at the first failure),
// D28 (arm names avoid clean/unarmed/control), D31.
#pragma once

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "../PrismCore/ThemeTokens.hpp"
#include "../PrismCore/WindowLayout.hpp"

namespace prism::win::tabbar {

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------- mutations
#if defined(PRISM_C60_MUTANT_DIRTY_NO_CONFIRM)
// C60 MUTANT ACTIVE: dirty_no_confirm -- a dirty tab closes without asking.
inline constexpr bool kDirtyNeedsConfirm = false;
#else
inline constexpr bool kDirtyNeedsConfirm = true;
#endif
#if defined(PRISM_C59_MUTANT_CLOSE_RIGHT_NEIGHBOR)
// C60 MUTANT ACTIVE: close_first_tab -- C59's injected rule keeps tab 0 active.
inline constexpr bool kCloseKeepsRightNeighbour = false;
#else
inline constexpr bool kCloseKeepsRightNeighbour = true;
#endif

// The runtime proof that the injection really compiled in (D3: a macro that
// stopped matching must leave the mutant target RED, never fake-green).
inline std::string_view ActiveMutation() {
  if (!kDirtyNeedsConfirm) return "dirty_no_confirm";
  if (!kCloseKeepsRightNeighbour) return "close_first_tab";
  return {};
}

// ------------------------------------------------------------------ metrics
// DESIGN.md §4's spacing scale only (4/8/12/16): 7 or 13 would be a token
// violation, and the strip is the one band where a 1 px drift is visible.
struct Metrics {
  static constexpr int kHeight = prism::layout::kTabBarHeight;  // 32
  static constexpr int kMargin = 8;        // strip inset on both sides
  static constexpr int kGap = 2;           // between two tabs
  static constexpr int kMinWidth = 120;    // below this a name is unreadable
  static constexpr int kMaxWidth = 224;
  static constexpr int kPadX = 12;         // text inset inside a tab
  static constexpr int kPadY = 6;
  static constexpr int kCloseSize = 16;    // the x hit box
  static constexpr int kCloseInset = 8;    // from the tab's right edge
  static constexpr int kTextGap = 6;       // title -> x
  static constexpr int kDotRadius = 3;
  static constexpr int kDotGap = 12;       // dot centre -> x
  static constexpr int kInsertWidth = 2;   // drag insertion mark
  static constexpr int kDragSlop = 4;      // px before a press becomes a drag
};

// What the strip is handed every frame. Built from a TabSet by the window; the
// painter keeps no state of its own, so a snapshot of this struct is the whole
// input to one frame.
struct Item {
  std::wstring title;   // the archive's FILE NAME (TabSet::displayName)
  bool dirty = false;   // unsaved changes -> dot behind the title
};

struct View {
  std::vector<Item> items;
  int active = -1;        // index into items (-1: empty state)
  int hover = -1;         // tab under the pointer (-1: none)
  int hoverClose = -1;    // tab whose x is under the pointer (-1: none)
  int pressedClose = -1;  // tab whose x is held down (-1: none)
  int dragFrom = -1;      // tab being dragged (-1: not dragging)
  int dropIndex = -1;     // insertion SLOT while dragging (0..items.size())
};

struct TabGeometry {
  int index = -1;
  prism::layout::Rect rect;    // the tab plate
  prism::layout::Rect title;   // where the name is drawn (clipped)
  prism::layout::Rect close;   // the x hit box
};

// One tab's width: an equal share of the strip, clamped to [min, max]. Fixed
// min/max is what keeps a 16-tab strip from collapsing every name to one glyph
// and a 2-tab strip from turning each tab into a banner.
inline int TabWidth(const prism::layout::Rect& bar, int count) {
  if (count <= 0 || bar.Empty()) return 0;
  const int available = bar.Width() - 2 * Metrics::kMargin;
  if (available <= 0) return 0;
  int width = available / count;
  width = (std::min)(width, Metrics::kMaxWidth);
  width = (std::max)(width, Metrics::kMinWidth);
  return width;
}

// Every tab's three rectangles, left to right. A tab whose plate would cross
// the strip's right edge keeps its FULL rect (the painter clips); the hit test
// below is what refuses a click outside the strip, so a half-visible tab still
// responds where it is visible.
inline std::vector<TabGeometry> TabRects(const prism::layout::Rect& bar, const View& view) {
  std::vector<TabGeometry> out;
  const int count = static_cast<int>(view.items.size());
  if (count <= 0 || bar.Empty()) return out;
  const int width = TabWidth(bar, count);
  if (width <= 0) return out;
  out.reserve(view.items.size());
  const int top = bar.top + (bar.Height() - (Metrics::kHeight - 2 * Metrics::kPadY)) / 2;
  const int bottom = top + (Metrics::kHeight - 2 * Metrics::kPadY);
  int x = bar.left + Metrics::kMargin;
  for (int i = 0; i < count; ++i) {
    TabGeometry tab;
    tab.index = i;
    tab.rect = prism::layout::Rect{x, top, x + width, bottom};
    // The x is right-aligned; the title ends before it (and before the dot when
    // the tab is dirty), so the two never overlap even at the minimum width.
    const int closeLeft = tab.rect.right - Metrics::kCloseInset - Metrics::kCloseSize;
    const int closeTop = tab.rect.top + (tab.rect.Height() - Metrics::kCloseSize) / 2;
    tab.close = prism::layout::Rect{closeLeft, closeTop, closeLeft + Metrics::kCloseSize,
                                    closeTop + Metrics::kCloseSize};
    int titleRight = closeLeft - Metrics::kTextGap;
    if (view.items[static_cast<std::size_t>(i)].dirty) {
      titleRight = (std::min)(titleRight, closeLeft - Metrics::kDotGap - Metrics::kDotRadius);
    }
    const int titleLeft = tab.rect.left + Metrics::kPadX;
    tab.title = prism::layout::Rect{titleLeft, tab.rect.top + 2,
                                    (std::max)(titleLeft, titleRight), tab.rect.bottom - 2};
    out.push_back(tab);
    x = tab.rect.right + Metrics::kGap;
  }
  return out;
}

// Where the dirty dot is painted: a fixed 12 px left of the x, vertically
// centred. Fixed (not "just after the text") for two reasons: the dot must be
// visible even when the name was clipped to nothing, and a pixel assertion has
// to be able to compute its centre without measuring a glyph run.
inline D2D1_POINT_2F DirtyDotCentre(const TabGeometry& tab) {
  const float cy = static_cast<float>(tab.rect.top + tab.rect.Height() / 2);
  const float cx = static_cast<float>(tab.close.left) - static_cast<float>(Metrics::kDotGap);
  return D2D1::Point2F(cx, cy);
}

// --------------------------------------------------------------- hit testing
// The tab under a client point, or -1. Only the strip's own rectangle counts:
// a click one pixel above it belongs to the title bar, not to a tab.
inline int TabAt(const prism::layout::Rect& bar, const std::vector<TabGeometry>& tabs, int x,
                 int y) {
  if (bar.Empty() || x < bar.left || x >= bar.right || y < bar.top || y >= bar.bottom) {
    return -1;
  }
  for (const TabGeometry& tab : tabs) {
    if (x >= tab.rect.left && x < tab.rect.right && y >= tab.rect.top && y < tab.rect.bottom) {
      return tab.index;
    }
  }
  return -1;
}

// The tab whose CLOSE BUTTON owns the point, or -1. The x is a child of the
// plate, so a click on it must never fall through to "activate this tab".
inline int CloseAt(const prism::layout::Rect& bar, const std::vector<TabGeometry>& tabs, int x,
                   int y) {
  if (bar.Empty() || x < bar.left || x >= bar.right || y < bar.top || y >= bar.bottom) {
    return -1;
  }
  for (const TabGeometry& tab : tabs) {
    if (x >= tab.close.left && x < tab.close.right && y >= tab.close.top &&
        y < tab.close.bottom) {
      return tab.index;
    }
  }
  return -1;
}

// The insertion SLOT (0 .. count) a drag at x would drop into: the slot whose
// marker is nearest to the pointer. Slot k means "before tab k"; slot count is
// "after the last tab". The marker is what makes the drop position visible, so
// the arithmetic that computes it and the arithmetic that picks it must be the
// same function.
inline int InsertSlot(const prism::layout::Rect& bar, const std::vector<TabGeometry>& tabs, int x) {
  const int count = static_cast<int>(tabs.size());
  if (count <= 0 || bar.Empty()) return -1;
  if (x < bar.left) return 0;
  if (x >= bar.right) return count;
  int best = 0;
  int bestDistance = -1;
  for (int slot = 0; slot <= count; ++slot) {
    const int edge = slot < count ? tabs[static_cast<std::size_t>(slot)].rect.left
                                  : tabs[static_cast<std::size_t>(count - 1)].rect.right;
    const int distance = x >= edge ? x - edge : edge - x;
    if (bestDistance < 0 || distance < bestDistance) {
      bestDistance = distance;
      best = slot;
    }
  }
  return best;
}

// The x-coordinate of the insertion marker for a slot.
inline int InsertMarkX(const std::vector<TabGeometry>& tabs, int slot) {
  const int count = static_cast<int>(tabs.size());
  if (count <= 0) return 0;
  if (slot <= 0) return tabs.front().rect.left;
  if (slot >= count) return tabs.back().rect.right;
  return tabs[static_cast<std::size_t>(slot)].rect.left;
}

// TabSet::Move(from, to) erases BEFORE inserting, so a forward move must lose
// one index. Getting this wrong is the classic "drag right by one does nothing"
// bug -- the drop slot and the move target are therefore two named functions,
// not one subtraction buried in the event handler.
inline int MoveTarget(int from, int slot) {
  return slot > from ? slot - 1 : slot;
}

// Ctrl+Tab / Ctrl+Shift+Tab: a WRAPPING ring over the open tabs. An empty strip
// answers -1 (there is nothing to cycle to).
inline int WrapIndex(int current, int delta, int count) {
  if (count <= 0) return -1;
  const int base = (current < 0 || current >= count) ? 0 : current;
  int next = (base + delta) % count;
  if (next < 0) next += count;
  return next;
}

// ------------------------------------------------------------------- painter
// One brush, two text formats. The brush is recoloured per token rather than
// allocated per colour: a strip with 16 tabs would otherwise create 16 brushes
// every frame (the same lesson C29 recorded for the rail).
class Painter {
 public:
  bool CreateDeviceResources(ID2D1RenderTarget* target, IDWriteFactory* factory) {
    if (target == nullptr || factory == nullptr) return false;
    factory_ = factory;
    if (FAILED(target->CreateSolidColorBrush(ToD2d(kFallback),
                                             brush_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         11.0f, L"en-US", label_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (label_) {
      label_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
      label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (FAILED(factory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                         DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                         12.0f, L"en-US", close_.ReleaseAndGetAddressOf()))) {
      return false;
    }
    if (close_) {
      close_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
      close_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      close_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    // The ellipsis sign is created ONCE: CreateEllipsisTrimmingSign per frame
    // would allocate a COM object per tab.
    if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(label_.Get(),
                                                      ellipsis_.ReleaseAndGetAddressOf()))) {
      trimming_ = DWRITE_TRIMMING{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    }
    return brush_ != nullptr && label_ != nullptr && close_ != nullptr;
  }

  // The strip paints NOTHING when it is hidden: an empty view (or an empty
  // band) is the "no archive is open" state, and a painted background there
  // would leave a 32 px ghost band above the action island -- the exact defect
  // case 7 of the suite measures.
  void Paint(ID2D1RenderTarget* target, const prism::layout::Layout& layout, const View& view,
             prism::theme::Variant variant = prism::theme::Variant::Midnight) {
    lastPainted_ = 0;
    lastDirtyDots_ = 0;
    lastInsertMarks_ = 0;
    if (target == nullptr || !brush_ || !label_) return;
    if (!layout.tabBarVisible || layout.tabBar.Empty() || view.items.empty()) return;
    const prism::layout::Rect bar = layout.tabBar;
    const std::vector<TabGeometry> tabs = TabRects(bar, view);
    if (tabs.empty()) return;

    target->PushAxisAlignedClip(ToRectF(bar), D2D1_ANTIALIAS_MODE_ALIASED);

    // The strip's own background is the CANVAS token, so the band reads as the
    // window's backdrop with tabs floating on it -- never as a third surface.
    SetToken(prism::theme::TokenId::SurfaceCanvasBase, variant);
    target->FillRectangle(ToRectF(bar), brush_.Get());

    for (const TabGeometry& tab : tabs) {
      const Item& item = view.items[static_cast<std::size_t>(tab.index)];
      if (tab.index == view.active) {
        // Active plate: SurfaceFloatingIsland composited over the canvas, i.e.
        // exactly the window's island surface (DESIGN.md §4).
        SetComposite(prism::theme::TokenId::SurfaceFloatingIsland,
                     prism::theme::TokenId::SurfaceCanvasBase, variant);
        target->FillRectangle(ToRectF(tab.rect), brush_.Get());
        // The 1 px specular rim is what makes the plate read as glass, and it
        // is a token, not a lighter grey.
        SetComposite(prism::theme::TokenId::BorderSpecularRim,
                     prism::theme::TokenId::SurfaceFloatingIsland, variant);
        target->DrawRectangle(ToRectF(tab.rect), brush_.Get(), 1.0f);
      } else {
        // Inactive: the canvas itself. A hovered tab gets the island at a low
        // alpha so the pointer is visible without inventing a new colour.
        if (tab.index == view.hover) {
          SetComposite(prism::theme::TokenId::SurfaceFloatingIsland,
                       prism::theme::TokenId::SurfaceCanvasBase, variant, 0.45f);
          target->FillRectangle(ToRectF(tab.rect), brush_.Get());
        }
      }
      if (item.dirty) {
        SetToken(prism::theme::TokenId::AccentLiquidPrimary, variant);
        const D2D1_POINT_2F centre = DirtyDotCentre(tab);
        target->FillEllipse(
            D2D1::Ellipse(centre, static_cast<float>(Metrics::kDotRadius),
                          static_cast<float>(Metrics::kDotRadius)),
            brush_.Get());
        ++lastDirtyDots_;
      }
      DrawTitle(target, tab, item, tab.index == view.active, variant);
      DrawClose(target, tab, view, variant);
      ++lastPainted_;
    }

    // The drag's insertion marker: a 2 px accent bar in the slot the tab would
    // land in. It is painted LAST so a half-clipped neighbour cannot cover it.
    if (view.dragFrom >= 0 && view.dropIndex >= 0 && view.dropIndex <= static_cast<int>(tabs.size())) {
      const float x = static_cast<float>(InsertMarkX(tabs, view.dropIndex));
      SetToken(prism::theme::TokenId::AccentLiquidPrimary, variant);
      target->FillRectangle(
          D2D1::RectF(x - static_cast<float>(Metrics::kInsertWidth) / 2.0f,
                      static_cast<float>(bar.top + Metrics::kPadY),
                      x + static_cast<float>(Metrics::kInsertWidth) / 2.0f,
                      static_cast<float>(bar.bottom - Metrics::kPadY)),
          brush_.Get());
      ++lastInsertMarks_;
    }
    target->PopAxisAlignedClip();
  }

  // Paint evidence (CONTRACTS D2/D3): "the model says two tabs" is geometry;
  // these counters are what proves the strip reached the device, and case 7
  // reads them to prove it did NOT.
  int TabsPainted() const { return lastPainted_; }
  int DirtyDotsPainted() const { return lastDirtyDots_; }
  int InsertMarksPainted() const { return lastInsertMarks_; }

 private:
  static D2D1_RECT_F ToRectF(const prism::layout::Rect& r) {
    return D2D1::RectF(static_cast<float>(r.left), static_cast<float>(r.top),
                       static_cast<float>(r.right), static_cast<float>(r.bottom));
  }

  static D2D1_COLOR_F ToD2d(const prism::theme::Rgba& c) {
    D2D1_COLOR_F out;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
    return out;
  }

  // A token's PAINTABLE colour. Gradient tokens (border-specular-rim,
  // accent-liquid-primary) leave TokenValue::color zeroed -- a bare
  // Lookup(...).color for them is OPAQUE BLACK, which painted the active tab's
  // rim as a dark seam and the dirty dot as a black blob (measured at x = 233
  // of a two-tab frame: rgb(8,9,12) on a canvas of rgb(16,18,24)). A gradient
  // token's paint colour is the from-stop, which is also what the suite's
  // AccentPixel() expects.
  static prism::theme::Rgba Token(prism::theme::TokenId id, prism::theme::Variant variant) {
    const prism::theme::TokenValue value = prism::theme::Lookup(id, variant);
    return value.kind == prism::theme::Kind::Gradient ? value.gradient.from : value.color;
  }

  void SetToken(prism::theme::TokenId id, prism::theme::Variant variant) {
    brush_->SetColor(ToD2d(Token(id, variant)));
  }

  // A token composited over another one, which is how the design language
  // stacks a semi-opaque surface on the canvas (the flyout suite does the same
  // for its own plate). Alpha is scaled for the hover tint only.
  void SetComposite(prism::theme::TokenId top, prism::theme::TokenId bottom,
                    prism::theme::Variant variant, float alpha = 1.0f) {
    prism::theme::Rgba color =
        prism::theme::CompositeOver(Token(top, variant), Token(bottom, variant));
    color.a *= alpha;
    brush_->SetColor(ToD2d(color));
  }

  // A long name is ELLIPSIZED to the title rectangle (DESIGN.md §2.1) instead of
  // running under the x: the layout is given the title's width, character
  // trimming is armed with the factory's own ellipsis sign, and the draw is
  // clipped as a second guard.
  void DrawTitle(ID2D1RenderTarget* target, const TabGeometry& tab, const Item& item,
                 bool active, prism::theme::Variant variant) {
    if (item.title.empty() || tab.title.Empty() || !factory_) return;
    const float width = static_cast<float>(tab.title.Width());
    const float height = static_cast<float>(tab.title.Height());
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory_->CreateTextLayout(item.title.c_str(),
                                          static_cast<std::uint32_t>(item.title.size()),
                                          label_.Get(), width, height,
                                          layout.ReleaseAndGetAddressOf()))) {
      return;
    }
    if (ellipsis_) layout->SetTrimming(&trimming_, ellipsis_.Get());
    const D2D1_POINT_2F origin = D2D1::Point2F(static_cast<float>(tab.title.left),
                                               static_cast<float>(tab.title.top));
    // The active tab's name is primary text; an inactive one is secondary. Both
    // are C15 roles with a measured contrast floor (DESIGN.md §4), so the two
    // plates are distinguishable without a second background colour.
    SetToken(active ? prism::theme::TokenId::TextPrimary
                    : prism::theme::TokenId::TextSecondary,
             variant);
    target->DrawTextLayout(origin, layout.Get(), brush_.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  void DrawClose(ID2D1RenderTarget* target, const TabGeometry& tab, const View& view,
                 prism::theme::Variant variant) {
    if (tab.close.Empty()) return;
    const bool hot = view.hoverClose == tab.index || view.pressedClose == tab.index;
    SetComposite(hot ? prism::theme::TokenId::TextPrimary
                     : prism::theme::TokenId::TextSecondary,
                 prism::theme::TokenId::SurfaceCanvasBase, variant);
    static constexpr wchar_t kCloseGlyph[] = L"\u00D7";  // multiplication sign
    target->DrawText(kCloseGlyph, 1, close_.Get(), ToRectF(tab.close), brush_.Get(),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  ComPtr<IDWriteFactory> factory_;
  ComPtr<ID2D1SolidColorBrush> brush_;
  ComPtr<IDWriteTextFormat> label_;
  ComPtr<IDWriteTextFormat> close_;
  ComPtr<IDWriteInlineObject> ellipsis_;
  DWRITE_TRIMMING trimming_{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  int lastPainted_ = 0;
  int lastDirtyDots_ = 0;
  int lastInsertMarks_ = 0;
  // A brush must exist before the first SetColor; magenta is C15's "missing
  // token" sentinel, so an unconfigured painter is visibly wrong, never black.
  inline static constexpr prism::theme::Rgba kFallback{1.0f, 0.0f, 1.0f, 1.0f};
};

}  // namespace prism::win::tabbar
