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

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "../PrismCore/Gallery.hpp"
#include "../PrismCore/GlassIsland.hpp"
#include "../PrismCore/ListLayout.hpp"
#include "../PrismCore/PathSafety.hpp"  // FileNameOf
#include "../PrismCore/Preview.hpp"
#include "../PrismCore/ProgressHud.hpp"
#include "../PrismCore/ThumbStore.hpp"

namespace prism::win::gallery {

using Microsoft::WRL::ComPtr;

class GalleryRenderer {
 public:
  // The design tokens (iOS 26 dark glass language).
  static constexpr D2D1_COLOR_F kBackdrop = {0.043f, 0.055f, 0.075f, 1.0f};   // #0B0E13
  static constexpr D2D1_COLOR_F kTileFill = {1.0f, 1.0f, 1.0f, 0.06f};        // glass pane
  static constexpr D2D1_COLOR_F kTileBorder = {1.0f, 1.0f, 1.0f, 0.12f};
  static constexpr D2D1_COLOR_F kIslandFill = {0.10f, 0.12f, 0.16f, 0.72f};   // floating glass
  static constexpr D2D1_COLOR_F kCardFill = {0.086f, 0.10f, 0.13f, 0.90f};    // Quick Look body
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

    // Batch 3: real thumbnails from the solid sweep (nullptr -> gradient
    // placeholders), the glass island model (nullptr -> static islands),
    // and the main-window list mode.
    prism::gallery::ThumbStore* thumbs = nullptr;
    prism::glass::GlassIslandScene* islands = nullptr;
    bool listMode = false;
    std::vector<prism::list::SortItem>* listItems = nullptr;
    uint32_t listScroll = 0;
    int selectedRow = -1;
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
    hr = target->CreateSolidColorBrush(kCardFill, card_.ReleaseAndGetAddressOf());
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

    if (scene.listMode) {
      DrawListMode(target, scene, size);
    } else {
      DrawGrid(target, scene, size);
    }
    DrawBreadcrumbIsland(target, scene, size);
    DrawProgressCapsule(target, scene, size);
    if (scene.quickLookOpen) DrawQuickLookCard(target, scene, size);
  }

 private:
  static std::wstring Utf8(std::string_view s) {
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
      } else if (prism::QuickLookPlanner::KindByName(item.path) == prism::PreviewKind::Image) {
        // Photo tile: REAL thumbnail when the sweep harvested one, the ⚡
        // streaming badge for over-limit blocks, gradient stand-in otherwise.
        // Everything is clipped by a squircle layer: DrawBitmap ignores the
        // rounded rect, so without the mask a wide photo bleeds past the
        // tile's corners (visible immediately in the snapshot).
        const D2D1_RECT_F photoRect = D2D1::RectF(left, top, right, bottom - 30.0f);
        const prism::gallery::DecodedThumb* thumb =
            scene.thumbs ? scene.thumbs->Lookup(item.index) : nullptr;
        if (thumb) ++dbgThumbLookupHits;

        ComPtr<ID2D1PathGeometry> clipGeometry;
        if (SUCCEEDED(CreateSquircleGeometry(target, photoRect, kRadius,
                                             clipGeometry.ReleaseAndGetAddressOf()))) {
          D2D1_LAYER_PARAMETERS layer = D2D1::LayerParameters(
              photoRect, clipGeometry.Get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
          target->PushLayer(layer, nullptr);
        }

        if (thumb) {
          SyncThumbBitmap(target, item.index, *thumb, scene.thumbs->Version());
          const auto it = thumbBitmaps_.find(item.index);
          if (it != thumbBitmaps_.end() && it->second) {
            ++dbgThumbBitmaps;
            const float areaW = right - left;
            const float areaH = bottom - 30.0f - top;
            const float scale = (std::min)(areaW / thumb->width, areaH / thumb->height);
            const float drawW = thumb->width * scale;
            const float drawH = thumb->height * scale;
            const D2D1_RECT_F fit = D2D1::RectF(left + (areaW - drawW) / 2.0f,
                                                top + (areaH - drawH) / 2.0f,
                                                left + (areaW + drawW) / 2.0f,
                                                top + (areaH + drawH) / 2.0f);
            target->FillRectangle(photoRect, tile_.Get());
            ++dbgThumbDraws;
            target->DrawBitmap(it->second.Get(), fit, 1.0f,
                               D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
          }
        } else if (item.size > prism::QuickLookPlanner::kDefaultByteLimit) {
          // Over-budget solid block: elegant "tap to stream" badge.
          thumbs_[index % 3]->SetStartPoint(D2D1::Point2F(left, top));
          thumbs_[index % 3]->SetEndPoint(D2D1::Point2F(left, bottom - 30.0f));
          target->FillRectangle(photoRect, thumbs_[index % 3].Get());
          DrawLabel(target, caption_.Get(), L"\x26A1 \x70B9\x51FB\x6D41\x5F0F\x52A0\x8F7D",
                    D2D1::RectF(left, top + (bottom - top) / 2.0f - 12.0f, right,
                                top + (bottom - top) / 2.0f + 12.0f),
                    textPrimary_.Get());
        } else {
          thumbs_[index % 3]->SetStartPoint(D2D1::Point2F(left, top));
          thumbs_[index % 3]->SetEndPoint(D2D1::Point2F(left, bottom - 30.0f));
          target->FillRectangle(photoRect, thumbs_[index % 3].Get());
        }
        if (clipGeometry) target->PopLayer();

        // Name plate under the photo (the grid is unusable without labels).
        const D2D1_ROUNDED_RECT plate = D2D1::RoundedRect(
            D2D1::RectF(left, bottom - 28.0f, right, bottom), 10.0f, 10.0f);
        target->FillRoundedRectangle(plate, tile_.Get());
        DrawLabel(target, caption_.Get(), Utf8(FileNameOf(item.path)),
                  D2D1::RectF(left + 8.0f, bottom - 26.0f, right - 8.0f, bottom - 4.0f),
                  textPrimary_.Get());
      } else {
        // Document tile: paper pane + text lines (the doc quick look reads
        // exactly this silhouette).
        D2D1_ROUNDED_RECT paper = D2D1::RoundedRect(
            D2D1::RectF(left, top, right, bottom - 30.0f), kRadius, kRadius);
        target->FillRoundedRectangle(paper, island_.Get());
        target->DrawRoundedRectangle(paper, tileBorder_.Get(), 1.0f);
        const float lineTop = top + 24.0f;
        const float lineW[4] = {0.62f, 0.84f, 0.74f, 0.50f};
        for (int line = 0; line < 4; ++line) {
          const float w = (right - left - 32.0f) * lineW[line];
          D2D1_ROUNDED_RECT textLine = D2D1::RoundedRect(
              D2D1::RectF(left + 16.0f, lineTop + line * 22.0f, left + 16.0f + w,
                          lineTop + line * 22.0f + 7.0f),
              3.5f, 3.5f);
          target->FillRoundedRectangle(textLine, line == 0 ? accent_.Get() : tileBorder_.Get());
        }

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

  // Device-bitmap cache for decoded thumbnails; invalidated exactly when the
  // ThumbStore mutates (Version()), never per frame.
  void SyncThumbBitmap(ID2D1RenderTarget* target, uint32_t itemIndex,
                       const prism::gallery::DecodedThumb& thumb, uint64_t version) {
    if (version != thumbVersion_) {
      thumbBitmaps_.clear();
      thumbVersion_ = version;
    }
    auto& slot = thumbBitmaps_[itemIndex];
    if (slot) return;
    D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(target->CreateBitmap(D2D1::SizeU(thumb.width, thumb.height),
                                    thumb.pixels.data(),
                                    thumb.width * 4, props,
                                    slot.ReleaseAndGetAddressOf()))) {
      slot.Reset();
    }
  }

  // Squircle (G2 continuous corner) clip path for a photo tile. D2D1 has no
  // "clip to rounded rect" on DrawBitmap, so the tile pushes a geometry
  // layer instead; superellipse exponent 4.2 per the design tokens.
  static HRESULT CreateSquircleGeometry(ID2D1RenderTarget* target, const D2D1_RECT_F& rect,
                                        float radius, ID2D1PathGeometry** out) {
    ComPtr<ID2D1Factory> factory;
    target->GetFactory(factory.ReleaseAndGetAddressOf());
    ComPtr<ID2D1PathGeometry> path;
    HRESULT hr = factory->CreatePathGeometry(path.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    ComPtr<ID2D1GeometrySink> sink;
    hr = path->Open(sink.ReleaseAndGetAddressOf());
    if (FAILED(hr)) return hr;
    const float cx = (rect.left + rect.right) / 2.0f;
    const float cy = (rect.top + rect.bottom) / 2.0f;
    const float halfW = (rect.right - rect.left) / 2.0f;
    const float halfH = (rect.bottom - rect.top) / 2.0f;
    (void)radius;
    // Superellipse sampled as a polyline; 32 segments per quadrant is well
    // under a pixel of sag at tile sizes and keeps the mask cheap. `radius`
    // only feeds the fallback assumption that the tile is at least as tall
    // as its corner radius (asserted by the caller's geometry).
    constexpr int kSegments = 32;
    constexpr float kExponent = 4.2f;
    constexpr float kPi = 3.14159265358979f;
    const auto pointAt = [&](float t) {
      // Parameterize one quadrant of the superellipse: |x/a|^n + |y/b|^n = 1
      const float c = std::cos(t);
      const float s = std::sin(t);
      const float ex = std::pow(std::fabs(c), 2.0f / kExponent);
      const float ey = std::pow(std::fabs(s), 2.0f / kExponent);
      return D2D1::Point2F(cx + (c >= 0 ? ex : -ex) * halfW,
                           cy + (s >= 0 ? ey : -ey) * halfH);
    };
    sink->BeginFigure(pointAt(0.0f), D2D1_FIGURE_BEGIN_FILLED);
    for (int i = 1; i <= kSegments * 4; ++i) {
      const float t = (2.0f * kPi) * (static_cast<float>(i) / (kSegments * 4));
      sink->AddLine(pointAt(t));
    }
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    hr = sink->Close();
    if (FAILED(hr)) return hr;
    *out = path.Detach();
    return S_OK;
  }

  static D2D1_COLOR_F BarColorOf(prism::list::BarColor color) {
    switch (color) {
      case prism::list::BarColor::Teal: return {0.0f, 0.780f, 0.745f, 1.0f};   // #00C7BE
      case prism::list::BarColor::Blue: return {0.0f, 0.478f, 1.0f, 1.0f};     // #007AFF
      case prism::list::BarColor::Green: return {0.204f, 0.780f, 0.349f, 1.0f}; // #34C759
      case prism::list::BarColor::Amber: return {1.0f, 0.584f, 0.0f, 1.0f};    // #FF9500
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
  }

  // The main-window file list (blueprint ch. 3.2): 名称 | 原始大小 | 压缩后 |
  // 压缩率, 36px virtualized rows, semantic micro-capsule bars, folders first.
  void DrawListMode(ID2D1RenderTarget* target, const Scene& scene,
                    const D2D1_SIZE_F& size) {
    if (!scene.listItems) return;
    const auto& rows = *scene.listItems;
    const prism::list::ColumnLayout cols;
    const float headerY = 68.0f;
    const float footerY = size.height - 56.0f;
    const float contentTop = headerY + 36.0f;
    const uint32_t viewportH =
        static_cast<uint32_t>((std::max)(0.0f, footerY - contentTop));

    // Header
    DrawLabel(target, caption_.Get(), L"\x540D\x79F0 ^", D2D1::RectF(24.0f, headerY,
                                                cols.sizeX(size.width) - 12.0f,
                                                headerY + 26.0f), textSecondary_.Get());
    DrawTrailingLabel(target, L"\x539F\x59CB\x5927\x5C0F", size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                      headerY, cols.sizeW - 18, textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x540E", size.width - cols.packedW - cols.ratioW + 6,
                      headerY, cols.packedW - 18, textSecondary_.Get());
    DrawTrailingLabel(target, L"\x538B\x7F29\x7387", size.width - cols.ratioW + 6, headerY,
                      cols.ratioW - 18, textSecondary_.Get());

    const auto window =
        prism::list::RowWindow(scene.listScroll, viewportH, static_cast<uint32_t>(rows.size()));
    for (uint32_t i = window.first; i <= window.last && i < rows.size(); ++i) {
      const auto& row = rows[i];
      const float top = contentTop + static_cast<float>(i) * prism::list::kRowHeight -
                        static_cast<float>(scene.listScroll);
      const float bottom = top + prism::list::kRowHeight;
      if (bottom < contentTop || top > footerY) continue;

      if (static_cast<int>(i) == scene.selectedRow) {
        // Selection highlight: G2 squircle R=10, accent fluid fill.
        D2D1_ROUNDED_RECT sel =
            D2D1::RoundedRect(D2D1::RectF(16.0f, top + 3.0f, size.width - 16.0f, bottom - 3.0f),
                              10.0f, 10.0f);
        D2D1_COLOR_F selColor = kAccent;
        selColor.a = 0.26f;
        accent_->SetColor(selColor);
        target->FillRoundedRectangle(sel, accent_.Get());
        accent_->SetColor(kAccent);
      }

      const std::wstring name = Utf8(row.name);
      if (row.isDir) {
        const D2D1_ROUNDED_RECT folder = D2D1::RoundedRect(
            D2D1::RectF(28.0f, top + 9.0f, 60.0f, top + 27.0f), 5.0f, 5.0f);
        target->FillRoundedRectangle(folder, accent_.Get());
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(70.0f, top + 5.0f,
                      cols.sizeX(size.width) - 80.0f, bottom - 4.0f), textPrimary_.Get());
        DrawTrailingLabel(target, L"--", size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.sizeW - 18, textSecondary_.Get());
        DrawTrailingLabel(target, L"--", size.width - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.packedW - 18, textSecondary_.Get());
      } else {
        DrawLabel(target, caption_.Get(), name, D2D1::RectF(44.0f, top + 5.0f,
                      cols.sizeX(size.width) - 52.0f, bottom - 4.0f), textPrimary_.Get());
        const std::wstring sizeText = Utf8(prism::list::FormatBytes(row.size));
        const std::wstring packedText = Utf8(prism::list::FormatBytes(row.packedSize));
        DrawTrailingLabel(target, sizeText, size.width - cols.sizeW - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.sizeW - 18, textPrimary_.Get());
        DrawTrailingLabel(target, packedText, size.width - cols.packedW - cols.ratioW + 6,
                          top + 6.0f, cols.packedW - 18, textPrimary_.Get());
      }

      // Semantic micro-capsule ratio bar (blueprint ch. 3.2.3).
      if (const auto bar = prism::list::RatioBar(row)) {
        const float trackX = size.width - cols.ratioW + 8.0f;
        const float trackW = cols.ratioW - 64.0f;
        const float cy = top + prism::list::kRowHeight / 2.0f;
        D2D1_ROUNDED_RECT track =
            D2D1::RoundedRect(D2D1::RectF(trackX, cy - 3.0f, trackX + trackW, cy + 3.0f),
                              3.0f, 3.0f);
        tileBorder_->SetColor(kTileBorder);
        target->FillRoundedRectangle(track, tileBorder_.Get());
        D2D1_ROUNDED_RECT fill = D2D1::RoundedRect(
            D2D1::RectF(trackX, cy - 3.0f, trackX + trackW * bar->percent / 100.0f, cy + 3.0f),
            3.0f, 3.0f);
        tileBorder_->SetColor(BarColorOf(bar->color));
        target->FillRoundedRectangle(fill, tileBorder_.Get());
        tileBorder_->SetColor(kTileBorder);
        wchar_t pct[8];
        swprintf_s(pct, L"%d%%", bar->percent);
        DrawTrailingLabel(target, pct, size.width - cols.ratioW + 8.0f + trackW + 6.0f, top + 6.0f,
                          40, textSecondary_.Get());
      }
    }
  }

  void DrawTrailingLabel(ID2D1RenderTarget* target, const std::wstring& text, float x,
                         float y, float w, ID2D1Brush* brush) {
    if (!caption_ || text.empty()) return;
    caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    DrawLabel(target, caption_.Get(), text, D2D1::RectF(x, y, x + w, y + 24.0f), brush);
    caption_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }

  void DrawBreadcrumbIsland(ID2D1RenderTarget* target, const Scene& scene,
                            const D2D1_SIZE_F& size) {
    float width = size.width * 0.62f;
    float left = (size.width - width) / 2.0f;
    float top = 16.0f;
    float height = 44.0f;
    if (scene.islands) {
      // The island model owns the geometry (blueprint ch. 3.2); the renderer
      // only paints what the scene computed.
      const auto r = scene.islands->RectOf(prism::glass::IslandId::Nav);
      left = static_cast<float>(r.x);
      top = static_cast<float>(r.y);
      width = static_cast<float>(r.w);
      height = static_cast<float>(r.h);
    }
    const D2D1_ROUNDED_RECT pill =
        D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height),
                          height / 2.0f, height / 2.0f);
    target->FillRoundedRectangle(pill, island_.Get());
    target->DrawRoundedRectangle(pill, islandBorder_.Get(), 1.0f);
    DrawLabel(target, title_.Get(), scene.breadcrumb,
              D2D1::RectF(left + 24.0f, top + 10.0f, left + width - 24.0f, top + 34.0f),
              textPrimary_.Get());
    if (scene.islands) {
      // Pointer-tracked specular rim (blueprint ch. 3.1 Layer 5): a bright
      // segment flows along the top edge toward the cursor. In the real
      // pipeline this comes from the HLSL shader; the software snapshot
      // approximates it from the SAME cbuffer params (pointer xy).
      float cb[20];
      scene.islands->WriteCbuffer(prism::glass::IslandId::Nav,
                                  static_cast<uint32_t>(size.width),
                                  static_cast<uint32_t>(size.height), cb);
      const float px = (std::min)((std::max)(cb[4], left + 70.0f), left + width - 70.0f);
      D2D1_COLOR_F rim = {1.0f, 1.0f, 1.0f, 0.62f};
      islandBorder_->SetColor(rim);
      D2D1_ROUNDED_RECT glow = D2D1::RoundedRect(
          D2D1::RectF(px - 62.0f, top + 1.0f, px + 62.0f, top + 3.0f), 1.0f, 1.0f);
      target->FillRoundedRectangle(glow, islandBorder_.Get());
      islandBorder_->SetColor(kIslandBorder);
    }
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
    target->FillRoundedRectangle(card, card_.Get());
    target->DrawRoundedRectangle(card, islandBorder_.Get(), 1.2f);

    // Preview area: gradient stand-in for the decoded texture.
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
  ComPtr<ID2D1SolidColorBrush> accent_, tile_, tileBorder_, island_, card_, islandBorder_;
  ComPtr<ID2D1SolidColorBrush> textPrimary_, textSecondary_;
  ComPtr<ID2D1LinearGradientBrush> thumbs_[3];
  std::unordered_map<uint32_t, ComPtr<ID2D1Bitmap>> thumbBitmaps_;
  uint64_t thumbVersion_ = 0;

 public:
  // Debug counters (printed by the snapshot; drop when the pipeline is
  // proven -- they cost nothing).
  int dbgThumbLookupHits = 0;
  int dbgThumbBitmaps = 0;
  int dbgThumbDraws = 0;

 private:
  ComPtr<IDWriteTextFormat> caption_, title_, small_;
};

}  // namespace prism::win::gallery
