// PrismArchive -- PrismWin
// The gallery FRAME renderer: draws the complete Liquid Glass design scene
// (full-bleed grid, floating breadcrumb island, progress capsule, Quick Look
// card) onto ANY ID2D1RenderTarget.
//
// Why a separate renderer instead of drawing inside the window class:
//  (a) the CI snapshot test renders the SAME code to a WIC bitmap (headless)
//      and ships the PNG as an artifact -- design review without a desktop,
//  (b) the window class adopts this next batch; one renderer, two hosts.
//
// All DECISIONS come from PrismCore (ViewportWindow / ProgressHud /
// QuickLookPlanner); this file only paints what they say.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <string>
#include <vector>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/PathSafety.hpp"  // FileNameOf
#include "../PrismCore/ProgressHud.hpp"

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

class GalleryRenderer {
 public:
  // The design tokens (iOS 26 dark glass language).
  static constexpr D2D1_COLOR_F kBackdrop = {0.043f, 0.055f, 0.075f, 1.0f};   // #0B0E13
  static constexpr D2D1_COLOR_F kTileFill = {1.0f, 1.0f, 1.0f, 0.06f};        // glass pane
  static constexpr D2D1_COLOR_F kTileBorder = {1.0f, 1.0f, 1.0f, 0.12f};
  static constexpr D2D1_COLOR_F kIslandFill = {0.10f, 0.12f, 0.16f, 0.72f};   // floating glass
  static constexpr D2D1_COLOR_F kIslandBorder = {1.0f, 1.0f, 1.0f, 0.16f};
  static constexpr D2D1_COLOR_F kAccent = {0.243f, 0.545f, 1.0f, 1.0f};       // #3E8BFF
  static constexpr D2D1_COLOR_F kTextPrimary = {0.910f, 0.925f, 0.945f, 1.0f};
  static constexpr D2D1_COLOR_F kTextSecondary = {0.604f, 0.655f, 0.706f, 1.0f};

  struct Scene {
    std::vector<prism::ArchiveItem>* items = nullptr;
    prism::gallery::ViewportWindow* viewport = nullptr;
    prism::ProgressHud::View hud;
    bool quickLookOpen = false;
    std::wstring quickLookTitle;
    std::wstring quickLookMeta;
    std::wstring breadcrumb;
  };

  HRESULT Initialize() {
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   factory_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    return DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                               reinterpret_cast<IUnknown**>(writeFactory_.ReleaseAndGetAddressOf()));
  }

  ID2D1Factory* Factory() const { return factory_.Get(); }

  // Per-target brush creation (call once per render target).
  HRESULT CreateDeviceResources(ID2D1RenderTarget* target) {
    HRESULT hr = target->CreateSolidColorBrush(kAccent, accent_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTileFill, tile_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTileBorder, tileBorder_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kIslandFill, island_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kIslandBorder, islandBorder_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTextPrimary, textPrimary_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    hr = target->CreateSolidColorBrush(kTextSecondary, textSecondary_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;

    // Sunset-ish gradient palette for photo placeholders (design stand-ins
    // until the ring-buffer WIC decode pipeline lands).
    const D2D1_COLOR_F stops[][2] = {
        {{0.99f, 0.62f, 0.28f, 0.95f}, {0.86f, 0.26f, 0.36f, 0.95f}},  // sunset
        {{0.24f, 0.66f, 0.78f, 0.95f}, {0.16f, 0.30f, 0.62f, 0.95f}},  // lagoon
        {{0.58f, 0.42f, 0.95f, 0.95f}, {0.26f, 0.20f, 0.55f, 0.95f}},  // dusk
    };
    for (int i = 0; i < 3; ++i) {
      const D2D1_GRADIENT_STOP gs[2] = {{0.0f, stops[i][0]}, {1.0f, stops[i][1]}};
      ComPtr<ID2D1GradientStopCollection> collection;
      hr = target->CreateGradientStopCollection(gs, 2, D2D1_GAMMA_2_2,
                                                D2D1_EXTEND_MODE_CLAMP,
                                                collection.ReleaseAndGetAddressOf());
      if (FAILED(hr)) return hr;
      hr = target->CreateLinearGradientBrush(
          D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 200)),
          collection.Get(), thumbs_[i].ReleaseAndGetAddressOf());
      if (FAILED(hr)) return hr;
    }

    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    13.0f, L"en-US", caption_.ReleaseAndGetAddressOf());
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    15.0f, L"en-US", title_.ReleaseAndGetAddressOf());
    writeFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                    12.0f, L"en-US", small_.ReleaseAndGetAddressOf());
    if (caption_) caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (title_) title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return S_OK;
  }

  void Draw(ID2D1RenderTarget* target, const Scene& scene) {
    const D2D1_SIZE_F size = target->GetSize();
    target->Clear(kBackdrop);

    if (scene.viewport && scene.items) DrawGrid(target, scene, size);

    DrawBreadcrumbIsland(target, scene, size);
    DrawProgressCapsule(target, scene, size);
    if (scene.quickLookOpen) DrawQuickLookCard(target, scene, size);
  }

 private:
  static std::wstring Utf8(const std::string& s) {
    if (s.empty()) return {};
    const int wide = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                         nullptr, 0);
    std::wstring out(static_cast<size_t>(wide), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), wide);
    return out;
  }

  void DrawGrid(ID2D1RenderTarget* target, const Scene& scene, const D2D1_SIZE_F& size) {
    const auto range = scene.viewport->Visible(2);
    if (range.Empty()) return;
    const int columns = scene.viewport->Columns();
    const float cellW = static_cast<float>(scene.viewport->ColumnWidth());
    const float cellH = static_cast<float>(scene.viewport->CellHeight());
    const float scroll = static_cast<float>(scene.viewport->Scroll());
    constexpr float kInset = 8.0f;
    constexpr float kRadius = 16.0f;

    for (int index = range.first; index <= range.last; ++index) {
      if (index >= static_cast<int>(scene.items->size())) break;
      const auto& item = (*scene.items)[index];
      const int col = index % columns;
      const int row = index / columns;
      const float left = static_cast<float>(col) * cellW + kInset;
      const float top = static_cast<float>(row) * cellH - scroll + kInset;
      const float right = static_cast<float>(col + 1) * cellW - kInset;
      const float bottom = static_cast<float>(row + 1) * cellH - scroll - kInset;
      if (bottom < 0 || top > size.height) continue;

      if (item.isDir) {
        // Folder tile: glass pane + simple vector folder glyph.
        const D2D1_ROUNDED_RECT pane =
            D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), kRadius, kRadius);
        target->FillRoundedRectangle(pane, tile_.Get());
        target->DrawRoundedRectangle(pane, tileBorder_.Get(), 1.0f);
        const float cx = (left + right) / 2.0f;
        const float cy = (top + bottom) / 2.0f - 14.0f;
        D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(cx - 34.0f, cy - 22.0f, cx + 34.0f, cy + 24.0f), 7.0f, 7.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        folder = D2D1::RoundedRect(D2D1::RectF(cx - 34.0f, cy - 30.0f, cx + 6.0f, cy - 14.0f),
                                   6.0f, 6.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
      } else {
        // Photo tile: gradient placeholder + name plate.
        thumbs_[index % 3]->ResetTransform();
        thumbs_[index % 3]->SetStartPoint(D2D1::Point2F(left, top));
        thumbs_[index % 3]->SetEndPoint(D2D1::Point2F(left, bottom - 30.0f));
        D2D1_ROUNDED_RECT photo = D2D1::RoundedRect(
            D2D1::RectF(left, top, right, bottom - 30.0f), kRadius, kRadius);
        target->FillRoundedRectangle(photo, thumbs_[index % 3].Get());

        const D2D1_ROUNDED_RECT pane =
            D2D1::RoundedRect(D2D1::RectF(left, bottom - 28.0f, right, bottom), 10.0f, 10.0f);
        target->FillRoundedRectangle(pane, tile_.Get());
        const std::wstring name = Utf8(FileNameOf(item.path));
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(left + 8.0f, bottom - 26.0f,
                                                            right - 8.0f, bottom - 4.0f),
                  textPrimary_.Get());
      }
    }
  }

  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const D2D1_SIZE_F& size) {
    const float width = size.width * 0.62f;
    const float left = (size.width - width) / 2.0f;
    const D2D1_ROUNDED_RECT pill =
        D2D1::RoundedRect(D2D1::RectF(left, 16.0f, left + width, 60.0f), 22.0f, 22.0f);
    target->FillRoundedRectangle(pill, island_.Get());
    target->DrawRoundedRectangle(pill, islandBorder_.Get(), 1.0f);
    DrawLabel(target, title_.Get(), scene.breadcrumb,
              D2D1::RectF(left + 24.0f, 26.0f, left + width - 24.0f, 50.0f),
              textPrimary_.Get());
  }

  void DrawProgressCapsule(ID2D1RenderTarget* target, const Scene& scene,
                           const D2D1_SIZE_F& size) {
    if (scene.hud.phase == prism::HudPhase::Hidden) return;
    const float width = 260.0f, height = 46.0f;
    const float left = (size.width - width) / 2.0f;
    const float top = size.height - height - 22.0f;
    const D2D1_ROUNDED_RECT pill =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height), height / 2.0f,
                          height / 2.0f);
    target->FillRoundedRectangle(pill, island_.Get());
    target->DrawRoundedRectangle(pill, islandBorder_.Get(), 1.0f);

    // Progress track + fill (the HUD model owns the percent; -1 == spin).
    const float trackInset = 10.0f;
    const float trackW = width - 2.0f * trackInset - 56.0f;
    D2D1_ROUNDED_RECT track = D2D1::RoundedRect(
        D2D1::RectF(left + trackInset, top + height / 2.0f - 3.0f,
                    left + trackInset + trackW, top + height / 2.0f + 3.0f),
        3.0f, 3.0f);
    target->FillRoundedRectangle(track, tileBorder_.Get());
    if (scene.hud.percent >= 0) {
      const float fillW = trackW * scene.hud.percent / 100.0f;
      if (fillW > 1.0f) {
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
            D2D1::RectF(track.rect.left, track.rect.top, track.rect.left + fillW,
                        track.rect.bottom),
            3.0f, 3.0f);
        target->FillRoundedRectangle(fill, accent_.Get());
      }
    }
    const std::wstring label =
        scene.hud.percent >= 0 ? std::to_wstring(scene.hud.percent) + L"%" : L"...";
    DrawLabel(target, caption_.Get(), label,
              D2D1::RectF(left + trackInset + trackW + 8.0f, top + 13.0f,
                          left + width - trackInset, top + height - 13.0f),
              textPrimary_.Get());
  }

  void DrawQuickLookCard(ID2D1RenderTarget* target, const Scene& scene,
                         const D2D1_SIZE_F& size) {
    const float width = 380.0f, height = 500.0f;
    const float left = size.width - width - 48.0f;
    const float top = 96.0f;
    const D2D1_ROUNDED_RECT card =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height), 24.0f, 24.0f);
    target->FillRoundedRectangle(card, island_.Get());
    target->DrawRoundedRectangle(card, islandBorder_.Get(), 1.2f);

    // Preview area: gradient stand-in for the decoded texture.
    thumbs_[1]->ResetTransform();
    thumbs_[1]->SetStartPoint(D2D1::Point2F(left, top));
    thumbs_[1]->SetEndPoint(D2D1::Point2F(left, top + height - 110.0f));
    D2D1_ROUNDED_RECT preview = D2D1::RoundedRect(
        D2D1::RectF(left + 14.0f, top + 14.0f, left + width - 14.0f, top + height - 110.0f),
        16.0f, 16.0f);
    target->FillRoundedRectangle(preview, thumbs_[1].Get());

    DrawLabel(target, title_.Get(), scene.quickLookTitle,
              D2D1::RectF(left + 18.0f, top + height - 98.0f, left + width - 18.0f,
                          top + height - 70.0f), textPrimary_.Get());
    DrawLabel(target, small_.Get(), scene.quickLookMeta,
              D2D1::RectF(left + 18.0f, top + height - 64.0f, left + width - 18.0f,
                          top + height - 42.0f), textSecondary_.Get());
    DrawLabel(target, small_.Get(), L"Space close  \u00B7  Esc close  \u00B7  arrows browse",
              D2D1::RectF(left + 18.0f, top + height - 36.0f, left + width - 18.0f,
                          top + height - 12.0f), textSecondary_.Get());
  }

  static void DrawLabel(ID2D1RenderTarget* target, IDWriteTextFormat* format,
                        const std::wstring& text, const D2D1_RECT_F& layout,
                        ID2D1Brush* brush) {
    if (!format || text.empty()) return;
    target->DrawText(text.c_str(), static_cast<UINT32>(text.size()), format, layout,
                     brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
  }

  ComPtr<ID2D1Factory> factory_;
  ComPtr<IDWriteFactory> writeFactory_;
  ComPtr<ID2D1SolidColorBrush> accent_, tile_, tileBorder_, island_, islandBorder_;
  ComPtr<ID2D1SolidColorBrush> textPrimary_, textSecondary_;
  ComPtr<ID2D1LinearGradientBrush> thumbs_[3];
  ComPtr<IDWriteTextFormat> caption_, title_, small_;
};

}  // namespace prism::win::gallery
