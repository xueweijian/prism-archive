// PrismArchive -- PrismWin
// C105: the INFO CARD and the STATUS BAND, asserted in pixels and geometry.
//
// The user's report was two sentences long:
//   "左下信息卡有一条硬下划线、标签与值挤在同一行" and
//   "状态栏是六段竖线切割的灰条".
// Plan 3.2 answers both, and this suite measures the answers on the REAL
// renderer (no mock painter, no synthetic scene):
//
//   1. NO FULL-WIDTH 1 PX RULE inside the card block. Every row of the card
//      (including the 12 px of air above it, where the old hairline sat) is
//      scanned: a row that is one colour for more than 80 % of the card's width
//      AND whose colour differs from the rows 2 px above and below it is a
//      RULE. The count must be 0.
//   2. THE TWO TYPE SIZES are read off the LIVE IDWriteTextFormat objects the
//      painter draws with: the label is 10 px (+/-1), the value is 12 px (+/-1).
//   3. THE STATUS BAND's adjacent segments are at least 16 px apart, read from
//      the segment rectangles (not from a literal).
//   4. THE BAND PAINTS NO VERTICAL RULE: no column of the band may be one
//      colour for more than 80 % of the band's inner height.
//   5. THE TWO-TONE ROLES are real: the value run of the ratio segment is
//      painted at least 20 levels brighter than its label run, at the x the
//      painter actually used.
//
// Negative controls (CONTRACTS D3/D26/D27). Each compiles THIS source with one
// rule restored/squeezed and is registered with PASS_REGULAR_EXPRESSION on the
// verdict -- never WILL_FAIL (D26 forbids combining them). The verdict is
// printed at the FIRST failing check and only when the injected branch is
// compiled in, so an injection that silently stopped matching turns its entry
// red instead of green:
//   underline          the pre-C105 hairline is back        -> (1) red
//   status-divider     the 1 px vertical rule is back       -> (4) red
//   status-tight-gap   the 24 px air collapses to 8 px      -> (3) red
#include <windows.h>

#include <d2d1.h>
#include <d2d1helper.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../Src/PrismCore/I18n.hpp"
#include "../../Src/PrismCore/SidebarModel.hpp"
#include "../../Src/PrismCore/WindowLayout.hpp"
#include "../../Src/PrismWin/GalleryRenderer.hpp"
#include "../../Src/PrismWin/SidebarView.hpp"

using Microsoft::WRL::ComPtr;

namespace {

using prism::ArchiveItem;
using prism::win::gallery::GalleryRenderer;
using prism::win::sidebar::Box;
using prism::win::sidebar::Command;
using prism::win::sidebar::Layout;
using prism::win::sidebar::Metrics;
using prism::win::sidebar::PrimKind;

constexpr UINT32 kWidth = 1600;
constexpr UINT32 kHeight = 1000;

// ---------------------------------------------------------------- mutations
// The verdict string is the LOWER-CASE mutant id, exactly as the suite's
// foreach(mutant IN ITEMS ...) + string(TOLOWER) spells the ctest name: one
// name from the registration to the printed marker, so the PASS_REGULAR_
// EXPRESSION and the marker cannot drift apart.
#if defined(PRISM_C105_MUTANT_UNDERLINE)
// C105 MUTANT ACTIVE: underline
// NEGATIVE CONTROL (CONTRACTS D3): the card's full-width hairline is restored,
// so assertion (1) -- "no full-width 1 px rule inside the card" -- MUST go red.
constexpr const char* kMutationName = "underline";
#elif defined(PRISM_C105_MUTANT_STATUS_DIVIDER)
// C105 MUTANT ACTIVE: status_divider
// NEGATIVE CONTROL (CONTRACTS D3): the band's 1 px vertical rule is restored,
// so assertion (4) MUST go red.
constexpr const char* kMutationName = "status_divider";
#elif defined(PRISM_C105_MUTANT_STATUS_TIGHT_GAP)
// C105 MUTANT ACTIVE: status_tight_gap
// NEGATIVE CONTROL (CONTRACTS D3): the air between segments collapses to 8 px,
// so assertion (3) MUST go red.
constexpr const char* kMutationName = "status_tight_gap";
#elif defined(PRISM_C105_MUTANT_BAR_OVER_VALUE)
// C105 MUTANT ACTIVE: bar_over_value
// NEGATIVE CONTROL (CONTRACTS D3): the ratio bar is drawn back INSIDE the value
// line -- the defect the integration build showed, a blue bar through the
// percentage -- so assertion (7) MUST go red.
constexpr const char* kMutationName = "bar_over_value";
#else
constexpr const char* kMutationName = nullptr;
#endif

int& Failures() {
  static int failures = 0;
  return failures;
}

// The ONE failing path. It prints the mutant verdict only when an injection is
// compiled in (D27: a mutation that never ran must not claim it was caught).
void Fail(const char* what) {
  std::printf("FAIL: %s\n", what);
  if (kMutationName != nullptr) {
    std::printf("C105 MUTATION CAUGHT: %s\n", kMutationName);
  }
  ++Failures();
}

// ------------------------------------------------------------------ pixels
struct Frame {
  UINT32 width = 0;
  UINT32 height = 0;
  std::vector<UINT32> pixels;  // BGRA, row major

  bool In(int x, int y) const {
    return x >= 0 && y >= 0 && x < static_cast<int>(width) && y < static_cast<int>(height);
  }
  UINT32 At(int x, int y) const {
    return pixels[static_cast<size_t>(y) * width + static_cast<size_t>(x)];
  }
  int Luminance(int x, int y) const {
    const UINT32 p = At(x, y);
    return (static_cast<int>((p >> 16) & 0xFFu) + static_cast<int>((p >> 8) & 0xFFu) +
            static_cast<int>(p & 0xFFu)) / 3;
  }
};

IWICImagingFactory* gWic = nullptr;

// One run of identical pixels: its length and its colour. A "full-width rule"
// is a run that covers most of the region AND is only as tall as the line.
struct Run {
  int length = 0;
  UINT32 color = 0;
};

Run LongestRunH(const Frame& frame, int y, int x0, int x1) {
  Run best{};
  int run = 0;
  UINT32 current = 0;
  for (int x = x0; x <= x1; ++x) {
    const UINT32 p = frame.At(x, y);
    if (run == 0 || p != current) {
      current = p;
      run = 1;
    } else {
      ++run;
    }
    if (run > best.length) {
      best.length = run;
      best.color = current;
    }
  }
  return best;
}

Run LongestRunV(const Frame& frame, int x, int y0, int y1) {
  Run best{};
  int run = 0;
  UINT32 current = 0;
  for (int y = y0; y <= y1; ++y) {
    const UINT32 p = frame.At(x, y);
    if (run == 0 || p != current) {
      current = p;
      run = 1;
    } else {
      ++run;
    }
    if (run > best.length) {
      best.length = run;
      best.color = current;
    }
  }
  return best;
}

// A horizontal RULE: a row that is one colour across more than 80 % of the
// region AND is a 1 px band -- the rows 2 px above and below are a DIFFERENT
// colour. That is exactly what a hairline is, and exactly what the card's own
// fill (uniform for many rows) is not.
bool IsHorizontalRule(const Frame& frame, int y, int x0, int x1, int width, Run* out) {
  const Run here = LongestRunH(frame, y, x0, x1);
  if (here.length <= width * 8 / 10) return false;
  const UINT32 above = frame.At((x0 + x1) / 2, y - 2);
  const UINT32 below = frame.At((x0 + x1) / 2, y + 2);
  if (above == here.color || below == here.color) return false;
  if (out != nullptr) *out = here;
  return true;
}

// A vertical RULE: the same predicate on a column. The band's own fill is
// uniform horizontally, so a column of pure fill has neighbours of the same
// colour and is not a rule.
bool IsVerticalRule(const Frame& frame, int x, int y0, int y1, int height, Run* out) {
  const Run here = LongestRunV(frame, x, y0, y1);
  if (here.length <= height * 8 / 10) return false;
  const UINT32 left = frame.At(x - 2, (y0 + y1) / 2);
  const UINT32 right = frame.At(x + 2, (y0 + y1) / 2);
  if (left == here.color || right == here.color) return false;
  if (out != nullptr) *out = here;
  return true;
}

// ----------------------------------------------------------------- harness
struct Harness {
  GalleryRenderer renderer;
  ComPtr<IWICBitmap> bitmap;
  ComPtr<ID2D1RenderTarget> target;

  bool Open(UINT32 width, UINT32 height) {
    if (FAILED(renderer.Initialize())) return false;
    if (FAILED(gWic->CreateBitmap(width, height, GUID_WICPixelFormat32bppPBGRA,
                                  WICBitmapCacheOnDemand,
                                  bitmap.ReleaseAndGetAddressOf()))) {
      return false;
    }
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(renderer.Factory()->CreateWicBitmapRenderTarget(
            bitmap.Get(), props, target.ReleaseAndGetAddressOf()))) {
      return false;
    }
    return SUCCEEDED(renderer.CreateDeviceResources(target.Get()));
  }

  bool Draw(const GalleryRenderer::Scene& scene, Frame* out) {
    target->BeginDraw();
    renderer.Draw(target.Get(), scene);
    if (FAILED(target->EndDraw())) return false;
    if (out == nullptr) return true;
    UINT w = 0, h = 0;
    if (FAILED(bitmap->GetSize(&w, &h))) return false;
    WICRect rc{0, 0, static_cast<INT>(w), static_cast<INT>(h)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap->Lock(&rc, WICBitmapLockRead, lock.ReleaseAndGetAddressOf()))) {
      return false;
    }
    UINT stride = 0, size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &data))) {
      return false;
    }
    out->width = rc.Width;
    out->height = rc.Height;
    out->pixels.resize(static_cast<size_t>(out->width) * out->height);
    for (UINT32 y = 0; y < out->height; ++y) {
      std::memcpy(out->pixels.data() + static_cast<size_t>(y) * out->width,
                  data + static_cast<size_t>(y) * stride, static_cast<size_t>(out->width) * 4);
    }
    return true;
  }
};

// The scene: the real window shape (tabs + rail + list + band) with a status
// band and a card that both carry a method, a dictionary and a ratio -- i.e.
// the state in which a label and a value exist for every segment.
struct SceneRig {
  std::vector<ArchiveItem> catalog;
  prism::sidebar::Model model;
  std::vector<prism::list::SortItem> rows;
  prism::layout::Layout frame;
  Metrics metrics;
  Layout sidebar;
  prism::win::tabbar::View tabs;
  GalleryRenderer::Scene scene;

  SceneRig() : model(&catalog) {}
};

ArchiveItem Dir(const char* path, std::uint32_t index) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.isDir = true;
  return item;
}

ArchiveItem File(const char* path, std::uint32_t index, std::uint64_t size,
                 std::uint64_t packed) {
  ArchiveItem item;
  item.path = path;
  item.index = index;
  item.size = size;
  item.packSize = packed;
  return item;
}

void BuildRig(SceneRig* rig) {
  // Two real FILES so the card's ratio is a measurement, not "unknown": the
  // catalog's totals are what the card renders (53600/100000 -> 53.6%, the same
  // string the band shows), and folders alone would leave it honestly empty.
  rig->catalog = {Dir("Design_Assets/", 0), Dir("Design_Assets/01_Brand/", 1),
                  Dir("Design_Assets/02_UI_Kit/", 2),
                  File("Design_Assets/01_Brand/logo.png", 3, 60000, 32000),
                  File("Design_Assets/02_UI_Kit/kit.png", 4, 40000, 21600)};
  rig->model.SetArchiveName("Design_Assets_v4.7z");
  rig->model.SetFavorites({prism::sidebar::Favorite{"Desktop", "C:/Users/designer/Desktop", "home"}});
  rig->model.PushRecent("D:/Work/Design_Assets.7z", 500);
  prism::sidebar::ArchiveInfo info;
  info.method = "LZMA2";
  info.dictionaryBytes = 64ull << 20;
  info.encryption = prism::sidebar::Encryption::Aes256;
  rig->model.SetArchiveInfo(info);

  for (int i = 0; i < 6; ++i) {
    prism::list::SortItem row;
    row.name = "row_" + std::to_string(i) + ".png";
    row.size = 100000u * static_cast<std::uint64_t>(i + 1);
    row.packedSize = row.size / 2;
    rig->rows.push_back(row);
  }

  rig->frame = prism::layout::Compute(static_cast<int>(kWidth), static_cast<int>(kHeight),
                                      /*rail=*/true, /*listMode=*/true,
                                      /*tabBarVisible=*/true);
  rig->metrics.width = GalleryRenderer::kSidebarColumnWidth;
  rig->metrics.viewportTop = static_cast<float>(rig->frame.content.top);
  rig->metrics.viewportBottom = static_cast<float>(rig->frame.content.bottom);
  rig->sidebar = prism::win::sidebar::Build(rig->model, rig->metrics);
  rig->tabs.items = {{L"Design_Assets_v4.7z", false}, {L"Brand_2026.7z", true}};
  rig->tabs.active = 0;

  GalleryRenderer::Scene& scene = rig->scene;
  scene.theme = prism::theme::Variant::Midnight;
  scene.listMode = true;
  scene.listItems = &rig->rows;
  scene.selectedRow = 2;
  scene.breadcrumb = L"Design_Assets_v4.7z  |  6 items  |  C105 info card";
  scene.layout = &rig->frame;
  scene.sidebar = &rig->sidebar;
  scene.tabs = &rig->tabs;
  scene.status.crypto = prism::layout::CryptoState::Encrypted;
  scene.status.method = "LZMA2";
  scene.status.dictionary = "64.0 MB";
  scene.status.ratioPercent = 54;
  scene.status.ratioView = "53.6%";
  scene.status.itemCount = 6;
  scene.status.selectedCount = 1;
}

// ------------------------------------------------------------------- PNG
bool WritePng(const Frame& frame, int left, int top, int right, int bottom,
              const std::string& path) {
  if (gWic == nullptr) return false;
  // Clamp to the frame: the band's crop adds 8 px of air above and below, and a
  // window whose band touches the bottom edge would otherwise read past the
  // last row (a real access violation, not a cosmetic one).
  left = (std::max)(0, left);
  top = (std::max)(0, top);
  right = (std::min)(right, static_cast<int>(frame.width));
  bottom = (std::min)(bottom, static_cast<int>(frame.height));
  const int w = right - left;
  const int h = bottom - top;
  if (w <= 0 || h <= 0) return false;
  std::vector<UINT32> crop(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      crop[static_cast<size_t>(y) * w + static_cast<size_t>(x)] = frame.At(left + x, top + y);
    }
  }
  ComPtr<IWICBitmap> bitmap;
  if (FAILED(gWic->CreateBitmapFromMemory(
          static_cast<UINT>(w), static_cast<UINT>(h), GUID_WICPixelFormat32bppPBGRA,
          static_cast<UINT>(w) * 4u, static_cast<UINT>(crop.size() * 4u),
          reinterpret_cast<BYTE*>(crop.data()), bitmap.GetAddressOf()))) {
    return false;
  }
  const std::wstring wide(path.begin(), path.end());
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapEncoder> encoder;
  ComPtr<IWICBitmapFrameEncode> outFrame;
  ComPtr<IPropertyBag2> bag;
  if (FAILED(gWic->CreateStream(stream.ReleaseAndGetAddressOf())) ||
      FAILED(stream->InitializeFromFilename(wide.c_str(), GENERIC_WRITE)) ||
      FAILED(gWic->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                 encoder.ReleaseAndGetAddressOf())) ||
      FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
      FAILED(encoder->CreateNewFrame(outFrame.ReleaseAndGetAddressOf(),
                                     bag.ReleaseAndGetAddressOf())) ||
      FAILED(outFrame->Initialize(bag.Get())) ||
      FAILED(outFrame->SetSize(static_cast<UINT>(w), static_cast<UINT>(h)))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
  return SUCCEEDED(outFrame->SetPixelFormat(&format)) &&
         SUCCEEDED(outFrame->WriteSource(bitmap.Get(), nullptr)) &&
         SUCCEEDED(outFrame->Commit()) && SUCCEEDED(encoder->Commit());
}

// ------------------------------------------------------------------ checks
// (1) the card carries no full-width 1 px rule.
void CheckCardHasNoRule(const Frame& frame, const Layout& layout, const Metrics& metrics) {
  const int x0 = static_cast<int>(layout.pill.left) + 2;
  const int x1 = static_cast<int>(layout.pill.right) - 2;
  const int width = x1 - x0;
  const int gapTop = static_cast<int>(layout.pill.top - metrics.statusGapAbove);
  const int pillTop = static_cast<int>(layout.pill.top);
  const int pillBottom = static_cast<int>(layout.pill.bottom);
  // The card's own squircle RIM is a rounded outline 1 px thick: its straight
  // middle segment is 86 % of the card's width, which is the same shape a rule
  // has. Rows within 3 px of the card's top/bottom edge are therefore the card's
  // outline and not a rule -- everything else, including the 12 px of air ABOVE
  // the card (where the pre-C105 hairline lived), is scanned.
  int rules = 0;
  int firstRuleY = -1;
  for (int y = gapTop; y < pillBottom; ++y) {
    if ((y >= pillTop - 1 && y <= pillTop + 3) ||
        (y >= pillBottom - 3 && y <= pillBottom + 1)) {
      continue;  // the card's own squircle rim (see the comment above)
    }
    if (!frame.In(x0, y) || !frame.In(x1, y)) continue;
    if (IsHorizontalRule(frame, y, x0, x1, width, nullptr)) {
      if (firstRuleY < 0) firstRuleY = y;
      ++rules;
    }
  }
  std::printf("C105 CARD RULE SCAN: rows=%d..%d width=%d rules=%d firstY=%d\n", gapTop,
              pillBottom, width, rules, firstRuleY);
  if (rules != 0) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "the info card carries %d full-width 1 px rule row(s) (first at y=%d)", rules,
                  firstRuleY);
    Fail(message);
  }
}

// (2) the two type sizes, read off the LIVE DirectWrite formats.
void CheckCardTypeSizes(const Harness& harness, const Layout& layout) {
  IDWriteTextFormat* labelFormat = harness.renderer.CardLabelFormat();
  IDWriteTextFormat* valueFormat = harness.renderer.CardValueFormat();
  if (labelFormat == nullptr || valueFormat == nullptr) {
    Fail("the info card's text formats were never created");
    return;
  }
  const float labelSize = labelFormat->GetFontSize();
  const float valueSize = valueFormat->GetFontSize();
  std::printf("C105 CARD TYPE: label=%.2f px value=%.2f px\n",
              static_cast<double>(labelSize), static_cast<double>(valueSize));
  if (!(std::fabs(labelSize - 10.0f) <= 1.0f)) {
    Fail("the card's label format is not 10 px (+/-1)");
  }
  if (!(std::fabs(valueSize - 12.0f) <= 1.0f)) {
    Fail("the card's value format is not 12 px (+/-1)");
  }
  // The layout half of the same contract: every label/value command carries the
  // token, and the value's box sits UNDER the label's (two lines, not one).
  int labels = 0;
  int values = 0;
  int stacked = 0;
  int separators = 0;
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::Separator) ++separators;
    if (command.kind == PrimKind::StatusLabel) {
      ++labels;
      if (!(std::fabs(command.fontSize - 10.0f) <= 1.0f)) {
        Fail("a StatusLabel command does not carry the 10 px token");
      }
    }
    if (command.kind == PrimKind::StatusText) {
      ++values;
      if (!(std::fabs(command.fontSize - 12.0f) <= 1.0f)) {
        Fail("a StatusText command does not carry the 12 px token");
      }
    }
  }
  for (const Command& command : layout.commands) {
    if (command.kind != PrimKind::StatusLabel) continue;
    for (const Command& value : layout.commands) {
      if (value.kind != PrimKind::StatusText) continue;
      if (value.box.top >= command.box.bottom - 0.01f &&
          std::fabs(value.box.left - command.box.left) < 0.01f) {
        ++stacked;
        break;
      }
    }
  }
  std::printf("C105 CARD LAYOUT: labels=%d values=%d stackedPairs=%d separators=%d\n", labels,
              values, stacked, separators);
  if (labels != 4 || values != 4) {
    Fail("the card does not emit four label/value pairs");
  }
  if (stacked != 4) {
    Fail("a card value is not on its own line under its label");
  }
  if (separators != 0) {
    Fail("the card still emits a Separator command (the hard underline)");
  }
  if (prism::win::sidebar::kCardUnderlineEnabled) {
    Fail("the card's underline switch is on in a build that must not draw it");
  }
}

// (3) adjacent status segments are at least 16 px apart.
void CheckStatusGaps(const prism::layout::Layout& frame,
                     const prism::layout::StatusInfo& info) {
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(frame, info);
  int dividers = 0;
  int tightest = 1 << 30;
  for (size_t i = 0; i < segments.size(); ++i) {
    if (segments[i].id == prism::layout::StatusId::Divider) ++dividers;
    if (i + 1 >= segments.size()) continue;
    const int gap = segments[i + 1].rect.left - segments[i].rect.right;
    tightest = (std::min)(tightest, gap);
  }
  std::printf("C105 STATUS GAPS: segments=%zu tightestGap=%d dividers=%d\n", segments.size(),
              tightest, dividers);
  if (segments.size() < 6) {
    Fail("the status band laid out fewer than six segments");
  }
  if (tightest < 16) {
    char message[160];
    std::snprintf(message, sizeof(message),
                  "two status segments are only %d px apart (want >= 16)", tightest);
    Fail(message);
  }
  if (dividers != 0) {
    Fail("the status band still emits a Divider segment (the 1 px vertical rule)");
  }
  if (prism::layout::kStatusDividerEnabled) {
    Fail("the band's divider switch is on in a build that must not draw it");
  }
}

// (4) the band paints no vertical rule.
//
// A rule is INK: a column that is one colour for the band's whole inner height,
// that colour is not the band's own fill, and the columns beside it are a
// different colour. The fill test matters -- a 1-2 px sliver of background
// BETWEEN two glyphs of "字典 64.0 MB" is also a full-height uniform column with
// ink on both sides, and it is not a rule (measured: it produced three false
// positives before this clause existed).
void CheckBandHasNoVerticalRule(const Frame& frame, const prism::layout::Layout& band,
                                const prism::layout::StatusInfo& info) {
  const int x0 = band.statusBar.left + 2;
  const int x1 = band.statusBar.right - 2;
  const int y0 = band.statusBar.top + 4;
  const int y1 = band.statusBar.bottom - 4;
  const int height = y1 - y0;
  // The empty middle of the band (the clusters leave >= 40 % blank): a point
  // that is provably the band's own fill, not a glyph.
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(band, info);
  int leftEnd = band.statusBar.left;
  int rightStart = band.statusBar.right;
  for (const prism::layout::StatusSegment& segment : segments) {
    if (segment.id == prism::layout::StatusId::Dictionary) leftEnd = segment.rect.right;
    if (segment.id == prism::layout::StatusId::ItemCount && segment.rect.left < rightStart) {
      rightStart = segment.rect.left;
    }
  }
  // 8 px right of the left cluster's last rect: inside the empty middle, and
  // clear of the midpoint where the pre-C105 divider used to sit (sampling the
  // midpoint would read the DIVIDER as the fill and turn the check inside out).
  const int fillX = leftEnd + 8 < rightStart - 8 ? leftEnd + 8 : rightStart - 8;
  const UINT32 bandFill = frame.At(fillX, band.statusBar.top + 10);
  int rules = 0;
  int firstRuleX = -1;
  for (int x = x0; x <= x1; ++x) {
    if (!frame.In(x, y0) || !frame.In(x, y1)) continue;
    Run run{};
    if (!IsVerticalRule(frame, x, y0, y1, height, &run)) continue;
    if (run.color == bandFill) continue;  // the background between glyphs
    if (firstRuleX < 0) firstRuleX = x;
    ++rules;
  }
  std::printf("C105 BAND RULE SCAN: columns=%d..%d height=%d fillX=%d fill=%08X rules=%d "
              "firstX=%d\n",
              x0, x1, height, fillX, bandFill, rules, firstRuleX);
  if (rules != 0) {
    char message[192];
    std::snprintf(message, sizeof(message),
                  "the status band paints %d vertical rule column(s) (first at x=%d)", rules,
                  firstRuleX);
    Fail(message);
  }
}

// (5) the value run is the PRIMARY role and the label run is the secondary one.
void CheckBandTwoTone(const Frame& frame, const GalleryRenderer& renderer,
                      const prism::layout::Layout& band,
                      const prism::layout::StatusInfo& info) {
  const std::vector<prism::layout::StatusSegment> segments =
      prism::layout::StatusSegments(band, info);
  const prism::layout::StatusSegment* ratio = nullptr;
  for (const prism::layout::StatusSegment& segment : segments) {
    if (segment.id == prism::layout::StatusId::Ratio) ratio = &segment;
  }
  std::printf("C105 BAND TONE: valueRuns=%d lastValueLeft=%.1f\n", renderer.dbgStatusValues,
              static_cast<double>(renderer.dbgLastStatusValueLeft));
  if (renderer.dbgStatusValues < 3) {
    Fail("fewer than three status values were painted in the primary role");
    return;
  }
  if (ratio == nullptr) {
    Fail("the status band has no ratio segment to measure");
    return;
  }
  const int valueLeft = static_cast<int>(renderer.dbgLastStatusValueLeft);
  if (valueLeft <= ratio->rect.left || valueLeft >= ratio->rect.right) {
    Fail("the ratio value was painted outside its own segment");
    return;
  }
  int labelMax = 0;
  int valueMax = 0;
  for (int y = ratio->rect.top; y < ratio->rect.bottom; ++y) {
    for (int x = ratio->rect.left; x < valueLeft - 2; ++x) {
      if (!frame.In(x, y)) continue;
      labelMax = (std::max)(labelMax, frame.Luminance(x, y));
    }
    for (int x = valueLeft; x < ratio->rect.right; ++x) {
      if (!frame.In(x, y)) continue;
      valueMax = (std::max)(valueMax, frame.Luminance(x, y));
    }
  }
  std::printf("C105 BAND TONE: labelMax=%d valueMax=%d delta=%d\n", labelMax, valueMax,
              valueMax - labelMax);
  if (valueMax - labelMax < 20) {
    Fail("the status value is not brighter than its label (the two-tone hierarchy is gone)");
  }
}

// (7) the ratio bar has a row of its own: it must not touch the value it
// describes, and it sits BELOW it (the design's choice -- a bar to the RIGHT of
// a right-aligned number would push the number out of the card's column).
//
// The integration build showed the pre-fix geometry: the bar was drawn inside
// the value line (bottom-5 .. bottom-2), so a 12 px "95.0%" and a 3 px accent
// bar shared one 16 px box and the bar ran through the glyphs. The fix reserves
// the row in Metrics; this check measures the rectangles the painter received.
void CheckCardBarRow(const Layout& layout, const Metrics& metrics) {
  const Command* bar = nullptr;
  const Command* ratioValue = nullptr;
  for (const Command& command : layout.commands) {
    if (command.kind == PrimKind::RatioBar) bar = &command;
    if (command.kind == PrimKind::StatusText && command.payload == 3) ratioValue = &command;
  }
  if (bar == nullptr || ratioValue == nullptr) {
    Fail("the card emitted no ratio bar / ratio value to compare");
    return;
  }
  const bool overlap = bar->box.Intersects(ratioValue->box);
  const bool below = bar->box.top >= ratioValue->box.bottom;
  std::printf("C105 CARD BAR: bar=(%.0f,%.0f)-(%.0f,%.0f) value=(%.0f,%.0f)-(%.0f,%.0f) "
              "overlap=%d below=%d\n",
              static_cast<double>(bar->box.left), static_cast<double>(bar->box.top),
              static_cast<double>(bar->box.right), static_cast<double>(bar->box.bottom),
              static_cast<double>(ratioValue->box.left),
              static_cast<double>(ratioValue->box.top),
              static_cast<double>(ratioValue->box.right),
              static_cast<double>(ratioValue->box.bottom), overlap ? 1 : 0,
              below ? 1 : 0);
  if (overlap) {
    Fail("the ratio bar intersects the ratio VALUE text (the bar runs through the number)");
  }
  if (!below) {
    Fail("the ratio bar is not below its value line");
  }
  if (!(bar->box.bottom <= layout.pill.bottom - metrics.statusPadding + 0.01f)) {
    Fail("the ratio bar escaped the card's bottom padding");
  }
  if (!(std::fabs((bar->box.bottom - bar->box.top) - metrics.cardBarHeight) < 0.01f)) {
    Fail("the ratio bar is not the height the layout reserved for it");
  }
  if (!prism::win::sidebar::kCardBarHasOwnRow) {
    Fail("the bar's own-row switch is off in a build that must keep the row");
  }
}

// (6) the i18n census, checked from the card that introduced the keys.
//
// test_i18n.cpp pins the census SIZE, and a size is a number somebody has to
// remember to bump (C88, C92 and this card each forgot it once -- the Linux lane
// is what caught all three). This check makes the other half visible locally: a
// key added twice, or a key the table was updated for but the census was not,
// fails HERE, in the suite that owns the words.
void CheckI18nCensus() {
  const std::vector<std::string>& keys = prism::i18n::RequiredKeys();
  const char* kAdded[] = {"sidebar.card.crypto", "sidebar.card.method",
                          "sidebar.card.dictionary", "sidebar.card.ratio"};
  for (const char* key : kAdded) {
    const int count = static_cast<int>(std::count(keys.begin(), keys.end(), std::string(key)));
    if (count != 1) {
      char message[192];
      std::snprintf(message, sizeof(message),
                    "i18n key %s appears %d times in RequiredKeys() (want exactly 1)", key,
                    count);
      Fail(message);
    }
  }
  int duplicates = 0;
  for (std::size_t i = 0; i < keys.size(); ++i) {
    for (std::size_t j = i + 1; j < keys.size(); ++j) {
      if (keys[i] == keys[j]) ++duplicates;
    }
  }
  std::printf("C105 I18N CENSUS: keys=%zu added=4 duplicates=%d\n", keys.size(), duplicates);
  if (duplicates != 0) {
    Fail("RequiredKeys() carries a duplicate key (the census would count it twice)");
  }
}

}  // namespace

int main(int argc, char** argv) {
  const char* outDir = argc > 1 ? argv[1] : nullptr;
  if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
    std::puts("SKIP: COM init failed");
    return 77;
  }
  ComPtr<IWICImagingFactory> wic;
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                              IID_PPV_ARGS(wic.ReleaseAndGetAddressOf())))) {
    std::puts("SKIP: WIC unavailable");
    return 77;
  }
  gWic = wic.Get();

#if defined(PRISM_C105_MUTANT_UNDERLINE)
  std::puts("C105 MUTANT ACTIVE: underline (the card's hairline is back)");
#elif defined(PRISM_C105_MUTANT_STATUS_DIVIDER)
  std::puts("C105 MUTANT ACTIVE: status_divider (the band's 1 px rule is back)");
#elif defined(PRISM_C105_MUTANT_STATUS_TIGHT_GAP)
  std::puts("C105 MUTANT ACTIVE: status_tight_gap (the 24 px air is 8 px)");
#endif

  SceneRig rig;
  BuildRig(&rig);

  Harness harness;
  if (!harness.Open(kWidth, kHeight)) {
    std::puts("FAIL: the renderer could not open a 1600x1000 WIC target");
    return 1;
  }
  Frame frame;
  if (!harness.Draw(rig.scene, &frame)) {
    std::puts("FAIL: the frame did not render");
    return 1;
  }

  // The rail's metrics are the window's: the card's geometry comes from the
  // SAME Layout the painter consumed, so the scan region is the painted card.
  std::printf("C105 FRAME: card=(%.0f,%.0f)-(%.0f,%.0f) band=(%d,%d)-(%d,%d)\n",
              static_cast<double>(rig.sidebar.pill.left),
              static_cast<double>(rig.sidebar.pill.top),
              static_cast<double>(rig.sidebar.pill.right),
              static_cast<double>(rig.sidebar.pill.bottom), rig.frame.statusBar.left,
              rig.frame.statusBar.top, rig.frame.statusBar.right, rig.frame.statusBar.bottom);

  CheckCardHasNoRule(frame, rig.sidebar, rig.metrics);
  CheckCardTypeSizes(harness, rig.sidebar);
  CheckStatusGaps(rig.frame, rig.scene.status);
  CheckBandHasNoVerticalRule(frame, rig.frame, rig.scene.status);
  CheckBandTwoTone(frame, harness.renderer, rig.frame, rig.scene.status);
  CheckCardBarRow(rig.sidebar, rig.metrics);
  CheckI18nCensus();

  // The design artifacts: the same pixels, cropped to the two surfaces the card
  // is about. Written only when a directory is passed (ctest passes none).
  if (outDir != nullptr) {
    const int cardLeft = static_cast<int>(rig.sidebar.panel.left) - 4;
    const int cardRight = static_cast<int>(rig.sidebar.panel.right) + 4;
    const int cardTop = static_cast<int>(rig.sidebar.contentBottom) - 60;
    const int cardBottom = static_cast<int>(rig.sidebar.pill.bottom) + 12;
    if (WritePng(frame, cardLeft, cardTop, cardRight, cardBottom,
                 std::string(outDir) + "\\c105-info-card.png")) {
      std::printf("SNAPSHOT %s\\c105-info-card.png\n", outDir);
    } else {
      Fail("the info-card snapshot could not be written");
    }
    if (WritePng(frame, rig.frame.statusBar.left, rig.frame.statusBar.top - 8,
                 rig.frame.statusBar.right, rig.frame.statusBar.bottom + 8,
                 std::string(outDir) + "\\c105-statusbar.png")) {
      std::printf("SNAPSHOT %s\\c105-statusbar.png\n", outDir);
    } else {
      Fail("the status-band snapshot could not be written");
    }
  }

  if (Failures() != 0) {
    std::printf("FAIL: C105 info-card/status-bar assertions: %d failure(s)\n", Failures());
    return 1;
  }
  std::puts("ALL C105 INFO-CARD CHECKS PASSED");
  return 0;
}
