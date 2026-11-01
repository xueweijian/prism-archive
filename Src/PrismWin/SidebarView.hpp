// PrismArchive -- PrismWin
// SidebarView: the sidebar as a LIST OF DRAW COMMANDS (blueprint 3.2).
//
// This header deliberately knows nothing about Direct2D, DirectWrite, HWND or
// DPI: it turns a prism::sidebar::Model plus a Metrics block into an ordered
// vector of primitive commands (rectangles, text runs, icon ids, a selection
// pill, a ratio bar). Card C6's renderer owns the pixels; this file owns the
// geometry, which is exactly what makes the layout assertable to the pixel in
// a unit test on any machine.
//
// Contract (also what the tests lock):
//   * commands are emitted in PAINT ORDER; RowHit is the only non-painting
//     primitive (it exists so the window can hit-test a row without measuring
//     text again);
//   * every label is preceded by its own Glyph/Label command with an explicit
//     box, so a text run and its background can never drift apart;
//   * rows are culled ONLY when they are entirely outside the scroll viewport,
//     and culling never changes contentHeight/maxScroll (the scrollbar must not
//     jump when rows leave the clip);
//   * the status pill is anchored to the bottom of the viewport, not to the
//     scrolled content;
//   * mutation switches exist for the CONTRACTS D3 negative controls: a
//     mutation build turns one rule off and the suite must go red.
#pragma once

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "../PrismCore/SidebarModel.hpp"

namespace prism::win::sidebar {

// ---------------------------------------------------------------- mutations
#if defined(PRISM_SIDEBAR_MUTANT_VIEW_NO_INDENT)
inline constexpr bool kIndentEnabled = false;
#else
inline constexpr bool kIndentEnabled = true;
#endif
#if defined(PRISM_SIDEBAR_MUTANT_VIEW_NO_CULL)
inline constexpr bool kCullEnabled = false;
#else
inline constexpr bool kCullEnabled = true;
#endif

enum class PrimKind : std::uint8_t {
  Panel,        // sidebar background
  SectionLabel, // "FAVORITES" / "RECENT" / "ARCHIVE TREE"
  RowHit,       // hit-test region for one row (not painted)
  Selection,    // keyboard focus highlight behind a row
  Glyph,        // icon id at an explicit box
  Label,        // one text run at an explicit box
  Chevron,      // tree expander glyph
  Separator,    // hairline above the status pill
  EmptyState,   // "NO ARCHIVE OPEN"
  StatusPill,   // telemetry capsule background
  StatusIcon,
  StatusText,
  RatioBar,     // compression-ratio bar inside the capsule
};

inline std::ostream& operator<<(std::ostream& os, PrimKind kind) {
  switch (kind) {
    case PrimKind::Panel: return os << "Panel";
    case PrimKind::SectionLabel: return os << "SectionLabel";
    case PrimKind::RowHit: return os << "RowHit";
    case PrimKind::Selection: return os << "Selection";
    case PrimKind::Glyph: return os << "Glyph";
    case PrimKind::Label: return os << "Label";
    case PrimKind::Chevron: return os << "Chevron";
    case PrimKind::Separator: return os << "Separator";
    case PrimKind::EmptyState: return os << "EmptyState";
    case PrimKind::StatusPill: return os << "StatusPill";
    case PrimKind::StatusIcon: return os << "StatusIcon";
    case PrimKind::StatusText: return os << "StatusText";
    case PrimKind::RatioBar: return os << "RatioBar";
  }
  return os << "?";
}

// Design tokens, resolved to real colours by the renderer (day/night matrices).
enum class ColorToken : std::uint32_t {
  Panel,
  SectionLabel,
  PrimaryText,
  SecondaryText,
  SelectedBackground,
  Accent,
  Muted,
  Locked,
};

inline std::ostream& operator<<(std::ostream& os, ColorToken token) {
  switch (token) {
    case ColorToken::Panel: return os << "Panel";
    case ColorToken::SectionLabel: return os << "SectionLabel";
    case ColorToken::PrimaryText: return os << "PrimaryText";
    case ColorToken::SecondaryText: return os << "SecondaryText";
    case ColorToken::SelectedBackground: return os << "SelectedBackground";
    case ColorToken::Accent: return os << "Accent";
    case ColorToken::Muted: return os << "Muted";
    case ColorToken::Locked: return os << "Locked";
  }
  return os << "?";
}

struct Box {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  float Width() const { return right - left; }
  float Height() const { return bottom - top; }
  bool Empty() const { return right <= left || bottom <= top; }
  bool Intersects(const Box& other) const {
    return left < other.right && other.left < right && top < other.bottom &&
           other.top < bottom;
  }
};

inline bool operator==(const Box& a, const Box& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}

struct Command {
  PrimKind kind = PrimKind::Panel;
  Box box;
  std::string text;  // Label / SectionLabel / EmptyState / StatusText
  std::string icon;  // Glyph / Chevron / StatusIcon: icon id, never a bitmap
  ColorToken color = ColorToken::PrimaryText;
  float opacity = 1.0f;
  int payload = -1;  // row index inside its section (RowHit / Selection / Glyph / Label)
  prism::sidebar::Section section = prism::sidebar::Section::None;
};

// Tokens from blueprint 3.3, translated to a 236 px column. Every number is an
// integral float so the pixel assertions in the tests are exact, not epsilon
// comparisons.
struct Metrics {
  float width = 236.0f;
  float paddingX = 16.0f;
  float paddingTop = 12.0f;
  float paddingBottom = 12.0f;
  float rowHeight = 28.0f;
  float sectionHeaderHeight = 18.0f;
  float sectionGap = 14.0f;
  float iconSize = 16.0f;
  float iconGap = 8.0f;
  float indent = 14.0f;
  float statusHeight = 86.0f;
  float statusGapAbove = 12.0f;
  float statusPadding = 10.0f;
  float statusLineHeight = 22.0f;
  float scrollY = 0.0f;
  float viewportTop = 0.0f;
  float viewportBottom = 1080.0f;
};

struct Layout {
  std::vector<Command> commands;
  Box panel;
  Box pill;
  float contentTop = 0.0f;
  float contentBottom = 0.0f;
  float contentHeight = 0.0f;
  float maxScroll = 0.0f;
};

// -------------------------------------------------------------- formatting
inline std::string FormatBytes(std::uint64_t bytes) {
  if (bytes < 1024) return std::to_string(bytes) + " B";
  static constexpr const char* kUnits[] = {"KB", "MB", "GB", "TB", "PB"};
  double value = static_cast<double>(bytes) / 1024.0;
  std::size_t unit = 0;
  while (value >= 1024.0 && unit + 1 < (sizeof(kUnits) / sizeof(kUnits[0]))) {
    value /= 1024.0;
    ++unit;
  }
  std::ostringstream os;
  os << std::fixed << std::setprecision(1) << value << ' ' << kUnits[unit];
  return os.str();
}

// 428 -> "42.8%" (integer math: no locale, no float formatting surprises).
inline std::string FormatRatio(std::uint32_t permille) {
  std::string out = std::to_string(permille / 10);
  out += '.';
  out += static_cast<char>('0' + (permille % 10));
  out += '%';
  return out;
}

inline std::string EncryptionLabel(prism::sidebar::Encryption encryption) {
  switch (encryption) {
    case prism::sidebar::Encryption::Aes256: return "AES-256 ENCRYPTED";
    case prism::sidebar::Encryption::Unknown: return "ENCRYPTED";
    case prism::sidebar::Encryption::None: return "NOT ENCRYPTED";
  }
  return "NOT ENCRYPTED";
}

inline std::string RowLabel(std::string_view label, std::string_view path) {
  if (!label.empty()) return std::string(label);
  const std::string normalized = prism::NormalizeArchivePath(path);
  const std::string_view leaf = prism::FileNameOf(normalized);
  return leaf.empty() ? std::string("?") : std::string(leaf);
}

namespace detail {

inline Box RowBox(const Metrics& m, float y, int depth) {
  const float depthOffset = kIndentEnabled ? m.indent * static_cast<float>(depth) : 0.0f;
  const float left = m.paddingX + depthOffset;
  return Box{left, y, m.width - m.paddingX, y + m.rowHeight};
}

inline Box GlyphBox(const Metrics& m, const Box& row) {
  const float top = row.top + (m.rowHeight - m.iconSize) * 0.5f;
  return Box{row.left, top, row.left + m.iconSize, top + m.iconSize};
}

inline Box TextBox(const Metrics& m, const Box& row, float textLeft) {
  return Box{textLeft, row.top, m.width - m.paddingX, row.top + m.rowHeight};
}

}  // namespace detail

// ------------------------------------------------------------------- build
inline Layout Build(const prism::sidebar::Model& model, const Metrics& metrics) {
  using prism::sidebar::FocusTarget;
  using prism::sidebar::Section;

  Layout layout;
  layout.panel = Box{0.0f, metrics.viewportTop, metrics.width, metrics.viewportBottom};
  layout.contentTop = metrics.viewportTop + metrics.paddingTop;
  const float pillBottom = metrics.viewportBottom - metrics.paddingBottom;
  const float pillTop = pillBottom - metrics.statusHeight;
  layout.pill = Box{metrics.paddingX, pillTop, metrics.width - metrics.paddingX, pillBottom};
  layout.contentBottom = pillTop - metrics.statusGapAbove;

  layout.commands.push_back(Command{PrimKind::Panel, layout.panel, {}, {},
                                    ColorToken::Panel, 1.0f, -1, Section::None});

  const auto visible = [&](const Box& box) {
    if (!kCullEnabled) return true;
    return box.bottom > metrics.viewportTop && box.top < layout.contentBottom;
  };
  auto push = [&layout](Command command) {
    layout.commands.push_back(std::move(command));
  };

  const prism::sidebar::Status status = model.ComputeStatus();
  const FocusTarget focus = model.Focus();

  float y = layout.contentTop - metrics.scrollY;

  auto emitHeader = [&](Section section, std::string_view text) {
    const Box box{metrics.paddingX, y, metrics.width - metrics.paddingX,
                  y + metrics.sectionHeaderHeight};
    if (visible(box)) {
      push(Command{PrimKind::SectionLabel, box, std::string(text), {},
                   ColorToken::SectionLabel, 0.9f, -1, section});
    }
    y += metrics.sectionHeaderHeight;
  };

  auto emitRow = [&](Section section, int index, std::string_view label,
                     std::string_view icon, int depth, bool chevron,
                     bool chevronOpen, bool reserveChevron) {
    const Box row = detail::RowBox(metrics, y, depth);
    const bool focused = focus.section == section && focus.index == index;
    if (visible(row)) {
      push(Command{PrimKind::RowHit, row, {}, {}, ColorToken::Panel, 0.0f, index,
                   section});
      if (focused) {
        push(Command{PrimKind::Selection, row, {}, {},
                     ColorToken::SelectedBackground, 1.0f, index, section});
      }
      if (chevron) {
        const Box box = detail::GlyphBox(metrics, row);
        push(Command{PrimKind::Chevron, box, {},
                     chevronOpen ? std::string("chevron-down")
                                 : std::string("chevron-right"),
                     ColorToken::SecondaryText, 1.0f, index, section});
      }
      // Tree rows always reserve the expander slot, even when the folder has no
      // children: otherwise a leaf's label would sit 16 px left of its
      // siblings and the column would visibly jitter with the data.
      const float iconLeft = row.left + (reserveChevron ? metrics.iconSize : 0.0f);
      const Box glyph{iconLeft, row.top + (metrics.rowHeight - metrics.iconSize) * 0.5f,
                      iconLeft + metrics.iconSize,
                      row.top + (metrics.rowHeight - metrics.iconSize) * 0.5f +
                          metrics.iconSize};
      push(Command{PrimKind::Glyph, glyph, {}, std::string(icon),
                   focused ? ColorToken::Accent : ColorToken::SecondaryText, 1.0f,
                   index, section});
      const Box text = detail::TextBox(metrics, row, glyph.right + metrics.iconGap);
      push(Command{PrimKind::Label, text, std::string(label), {},
                   ColorToken::PrimaryText, 1.0f, index, section});
    }
    y += metrics.rowHeight;
  };

  // --- FAVORITES -----------------------------------------------------------
  if (!model.Favorites().empty()) {
    emitHeader(Section::Favorites, "FAVORITES");
    const auto& favorites = model.Favorites();
    for (std::size_t i = 0; i < favorites.size(); ++i) {
      const prism::sidebar::Favorite& favorite = favorites[i];
      emitRow(Section::Favorites, static_cast<int>(i),
              RowLabel(favorite.label, favorite.path),
              favorite.icon.empty() ? std::string("star") : favorite.icon, 0, false,
              false, false);
    }
    y += metrics.sectionGap;
  }

  // --- RECENT --------------------------------------------------------------
  if (!model.Recent().empty()) {
    emitHeader(Section::Recent, "RECENT");
    const auto& recent = model.Recent();
    for (std::size_t i = 0; i < recent.size(); ++i) {
      emitRow(Section::Recent, static_cast<int>(i), RowLabel({}, recent[i].path),
              "clock", 0, false, false, false);
    }
    y += metrics.sectionGap;
  }

  // --- ARCHIVE TREE --------------------------------------------------------
  emitHeader(Section::Tree, "ARCHIVE TREE");
  const auto& rows = model.Tree();
  if (rows.empty()) {
    const Box box{metrics.paddingX, y, metrics.width - metrics.paddingX,
                  y + metrics.rowHeight};
    if (visible(box)) {
      push(Command{PrimKind::EmptyState, box, "NO ARCHIVE OPEN", "empty",
                   ColorToken::Muted, 0.7f, -1, Section::Tree});
    }
    y += metrics.rowHeight;
  } else {
    for (std::size_t i = 0; i < rows.size(); ++i) {
      const prism::sidebar::TreeRow& row = rows[i];
      emitRow(Section::Tree, static_cast<int>(i), row.name, "folder", row.depth,
              row.expandable, row.expanded, true);
    }
  }

  layout.contentHeight = y - (layout.contentTop - metrics.scrollY);
  const float scrollable = layout.contentBottom - layout.contentTop;
  layout.maxScroll = (std::max)(0.0f, layout.contentHeight - (std::max)(0.0f, scrollable));

  // --- bottom telemetry capsule (anchored, never scrolled) -----------------
  push(Command{PrimKind::Separator,
               Box{metrics.paddingX, pillTop - metrics.statusGapAbove * 0.5f,
                   metrics.width - metrics.paddingX,
                   pillTop - metrics.statusGapAbove * 0.5f + 1.0f},
               {}, {}, ColorToken::SecondaryText, 0.35f, -1, Section::None});
  push(Command{PrimKind::StatusPill, layout.pill, {}, {}, ColorToken::Panel,
               status.hasArchive ? 1.0f : 0.6f, -1, Section::None});

  const float textLeft = layout.pill.left + metrics.statusPadding + metrics.iconSize +
                         metrics.iconGap;
  if (!status.hasArchive) {
    const Box lineTop{layout.pill.left + metrics.statusPadding, pillTop + metrics.statusPadding,
                      layout.pill.right - metrics.statusPadding,
                      pillTop + metrics.statusPadding + metrics.statusLineHeight};
    push(Command{PrimKind::StatusIcon, detail::GlyphBox(metrics, lineTop), {},
                 "empty", ColorToken::Muted, 0.7f, -1, Section::None});
    push(Command{PrimKind::StatusText,
                 Box{textLeft, lineTop.top, layout.pill.right - metrics.statusPadding,
                     lineTop.bottom},
                 "NO ARCHIVE", {}, ColorToken::Muted, 0.7f, -1, Section::None});
    return layout;
  }

  const float lineTop = pillTop + metrics.statusPadding;
  const std::string method = status.method.empty() ? std::string("UNKNOWN") : status.method;
  const std::string dictionary =
      status.dictionaryBytes == 0 ? std::string("--") : FormatBytes(status.dictionaryBytes);
  const std::string lines[3] = {
      EncryptionLabel(status.encryption),
      method + " | DICT " + dictionary,
      "RATIO " + FormatRatio(status.ratioPermille),
  };
  const char* icons[3] = {"lock", "bolt", "chart"};
  for (int i = 0; i < 3; ++i) {
    const float top = lineTop + metrics.statusLineHeight * static_cast<float>(i);
    const Box line{layout.pill.left + metrics.statusPadding, top,
                   layout.pill.right - metrics.statusPadding,
                   top + metrics.statusLineHeight};
    push(Command{PrimKind::StatusIcon, detail::GlyphBox(metrics, line), {},
                 icons[i],
                 i == 0 && status.encrypted ? ColorToken::Locked : ColorToken::SecondaryText,
                 1.0f, i, Section::None});
    push(Command{PrimKind::StatusText,
                 Box{textLeft, top, layout.pill.right - metrics.statusPadding,
                     top + metrics.statusLineHeight},
                 lines[i], {}, ColorToken::PrimaryText, 1.0f, i, Section::None});
  }
  const float fraction =
      (std::min)(1.0f, static_cast<float>(status.ratioPermille) / 1000.0f);
  const Box lastLine{textLeft, lineTop + metrics.statusLineHeight * 2.0f,
                     layout.pill.right - metrics.statusPadding,
                     lineTop + metrics.statusLineHeight * 3.0f};
  push(Command{PrimKind::RatioBar,
               Box{lastLine.left, lastLine.bottom - 5.0f,
                   lastLine.left + lastLine.Width() * fraction, lastLine.bottom - 2.0f},
               {}, {}, ColorToken::Accent, 0.9f, -1, Section::None});
  return layout;
}

}  // namespace prism::win::sidebar