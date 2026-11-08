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

#include "../PrismCore/I18n.hpp"        // C40: the four headings are table keys
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
#if defined(PRISM_C105_MUTANT_UNDERLINE)
inline constexpr bool kCardUnderlineEnabled = true;
#else
inline constexpr bool kCardUnderlineEnabled = false;
#endif
// C105: the ratio bar's own row exists only in the shipping build. The mutation
// arm draws the bar where it used to be -- INSIDE the value line, straight
// through the percentage -- which must turn the "the bar does not intersect its
// value" assertion red.
#if defined(PRISM_C105_MUTANT_BAR_OVER_VALUE)
inline constexpr bool kCardBarHasOwnRow = false;
#else
inline constexpr bool kCardBarHasOwnRow = true;
#endif

#if defined(PRISM_C104_MUTANT_SQUARE)
inline constexpr bool kSelectionPill = false;
#else
inline constexpr bool kSelectionPill = true;
#endif

enum class PrimKind : std::uint8_t {
  Panel,        // sidebar background
  SectionLabel, // "FAVORITES" / "RECENT" / "ARCHIVE TREE"
  RowHit,       // hit-test region for one row (not painted)
  Selection,    // keyboard focus highlight behind a row
  SelectionBar, // C79/C124: the 2 DIP accent rule at the left edge of that highlight
  Glyph,        // icon id at an explicit box
  Label,        // one text run at an explicit box
  Chevron,      // tree expander glyph
  Separator,    // hairline above the status pill
  EmptyState,   // "NO ARCHIVE OPEN"
  StatusPill,   // telemetry capsule background
  StatusIcon,
  // C105: the card's 10 px field label (the line ABOVE a value). StatusText is
  // the 12 px VALUE. Two primitives, not one with a flag: the paint path, the
  // type size and the colour role all differ, and a suite has to be able to
  // count them separately.
  StatusLabel,
  StatusText,
  RatioBar,     // compression-ratio bar inside the capsule
};

inline std::ostream& operator<<(std::ostream& os, PrimKind kind) {
  switch (kind) {
    case PrimKind::Panel: return os << "Panel";
    case PrimKind::SectionLabel: return os << "SectionLabel";
    case PrimKind::RowHit: return os << "RowHit";
    case PrimKind::Selection: return os << "Selection";
    case PrimKind::SelectionBar: return os << "SelectionBar";
    case PrimKind::Glyph: return os << "Glyph";
    case PrimKind::Label: return os << "Label";
    case PrimKind::Chevron: return os << "Chevron";
    case PrimKind::Separator: return os << "Separator";
    case PrimKind::EmptyState: return os << "EmptyState";
    case PrimKind::StatusPill: return os << "StatusPill";
    case PrimKind::StatusIcon: return os << "StatusIcon";
    case PrimKind::StatusLabel: return os << "StatusLabel";
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
  // C79: extra tracking for a text run, in DIPs at the run's own size. A group
  // heading is 11 px with .5 px tracking (Finder's sidebar); the renderer turns
  // this into DWrite's em-relative SetCharacterSpacing, so the number stays a
  // design token rather than a per-call-site magic constant.
  float letterSpacing = 0.0f;
  // C105: the type size of a text run, in DIPs. 0 means "whatever the renderer
  // uses for this primitive by default". The info card sets it explicitly
  // (10 px label / 12 px value) so the two sizes are a LAYOUT fact the geometry
  // suite can read, not a number buried in the painter.
  float fontSize = 0.0f;
  // C104: the corner radius of a filled primitive, in DIPs (0 = square). It
  // travels with the command because the VIEW owns the geometry -- a radius the
  // renderer had to guess would be a second design source, and the pill's
  // roundness could not be asserted without rendering.
  float cornerRadius = 0.0f;
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
  // C79: one indent step is 16 px (blueprint 3.2's 4 px scale), and the focus
  // rule is exactly 3 px wide.
  float indent = 16.0f;
  // C124 / "the seam": ONE 2 DIP accent line, in the tree and in the list alike.
  // C79 shipped 3 px here and the list had none, i.e. two widths for one
  // element -- the same "one product, several truths" the three blues were.
  float selectionBarWidth = 2.0f;
  // C104: the selection pill is inset 8 px at BOTH ends so the wash never
  float selectionInset = 8.0f;
  float selectionRadius = 6.0f;
  // C105: the card is FOUR label/value pairs stacked (plan 3.2). Each pair is
  // a 12 px label line above a 16 px value line (28 px), the pairs are 6 px
  // apart, and the card keeps 10 px of padding.
  //
  // The RATIO BAR HAS ITS OWN ROW. It used to be drawn INSIDE the value line
  // (bottom-5 .. bottom-2 of that box), which is where the integration build
  // showed it: a blue bar straight through the "95.0%" glyphs. A bar that shares
  // a row with text cannot be positioned correctly at every font/DPI, so the row
  // is reserved by the layout arithmetic that also places the text:
  //   4*28 + 3*6 + (cardBarGap + cardBarHeight) + 2*10 = 137 + 20 = 157.
  // The numbers are integral floats so the pixel assertions are exact.
  float statusHeight = 157.0f;
  float statusGapAbove = 12.0f;
  float statusPadding = 10.0f;
  float statusLineHeight = 22.0f;
  float cardLabelHeight = 12.0f;
  float cardValueHeight = 16.0f;
  float cardRowGap = 6.0f;
  float cardLabelFontSize = 10.0f;
  float cardValueFontSize = 12.0f;
  // C105: the ratio bar's own row: 3 px tall, 4 px below the value it belongs to.
  float cardBarHeight = 3.0f;
  float cardBarGap = 4.0f;

  float CardRowHeight() const { return cardLabelHeight + cardValueHeight; }
  float CardBarRowHeight() const { return cardBarGap + cardBarHeight; }
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

// ------------------------------------------------------------------ labels
// C40: the four section headings used to be literals in this header, so the
// sidebar stayed English in a Chinese build even though sidebar.section.* and
// sidebar.empty.noArchive had been in both tables since C19. They are now a
// value the caller can hand in (or that the installed translator fills).
struct Labels {
  // C40: the four section headings.
  std::string favorites = "FAVORITES";
  std::string recent = "RECENT";
  std::string tree = "ARCHIVE TREE";
  std::string noArchive = "NO ARCHIVE OPEN";
  // C74: the telemetry capsule's words. They were literals in Build(), so the
  // "info card" (NOT ENCRYPTED / UNKNOWN | DICT -- / RATIO 53.6%) stayed English
  // in a Chinese build even though every key below has been in both tables
  // since C19. The placeholders are the C19 ones ({percent} / {size}).
  std::string encrypted = "ENCRYPTED";
  std::string notEncrypted = "NOT ENCRYPTED";
  std::string unknownMethod = "UNKNOWN";
  std::string ratio = "RATIO {percent}";
  std::string dictionary = "DICT {size}";
  // C105: the four FIELD LABELS of the info card. The card is a label/value
  // stack now, so "DICT 64.0 MB" became a label ("DICTIONARY") above a value
  // ("64.0 MB"), and each label is a key of its own.
  std::string cardCrypto = "ENCRYPTION";
  std::string cardMethod = "METHOD";
  std::string cardDictionary = "DICTIONARY";
  std::string cardRatio = "RATIO";
};

// The translator the app installs ONCE at startup (Main.cpp, before the window
// exists). It is a slot rather than a Build() parameter because the renderer's
// call site belongs to another card (V1/V3) and must not be edited by this one;
// a null slot -- unit suites, the shell DLLs -- keeps the English literals.
inline const prism::i18n::Translator*& TranslatorSlot() {
  static const prism::i18n::Translator* translator = nullptr;
  return translator;
}

inline void SetTranslator(const prism::i18n::Translator* translator) {
  TranslatorSlot() = translator;
}

inline const prism::i18n::Translator* InstalledTranslator() { return TranslatorSlot(); }

inline Labels LabelsFrom(const prism::i18n::Translator* translator) {
  Labels labels;
  if (translator == nullptr) return labels;
  const auto pick = [translator](const char* key, const std::string& fallback) {
    return translator->Has(key) ? translator->Translate(key) : fallback;
  };
  labels.favorites = pick("sidebar.section.favorites", labels.favorites);
  labels.recent = pick("sidebar.section.recent", labels.recent);
  labels.tree = pick("sidebar.section.tree", labels.tree);
  labels.noArchive = pick("sidebar.empty.noArchive", labels.noArchive);
  labels.encrypted = pick("status.encrypted", labels.encrypted);
  labels.notEncrypted = pick("status.notEncrypted", labels.notEncrypted);
  labels.unknownMethod = pick("status.unknownMethod", labels.unknownMethod);
  labels.ratio = pick("status.ratio", labels.ratio);
  labels.dictionary = pick("status.dictionary", labels.dictionary);
  labels.cardCrypto = pick("sidebar.card.crypto", labels.cardCrypto);
  labels.cardMethod = pick("sidebar.card.method", labels.cardMethod);
  labels.cardDictionary = pick("sidebar.card.dictionary", labels.cardDictionary);
  labels.cardRatio = pick("sidebar.card.ratio", labels.cardRatio);
  return labels;
}

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

// C74: the cipher prefix is a token ("AES-256"), the word after it is the
// table's. A table-less build keeps the exact pre-C74 sentence.
inline std::string EncryptionLabel(prism::sidebar::Encryption encryption,
                                   const Labels& labels) {
  switch (encryption) {
    case prism::sidebar::Encryption::Aes256: return "AES-256 " + labels.encrypted;
    case prism::sidebar::Encryption::Unknown: return labels.encrypted;
    case prism::sidebar::Encryption::None: return labels.notEncrypted;
  }
  return labels.notEncrypted;
}

inline std::string RowLabel(std::string_view label, std::string_view path) {
  if (!label.empty()) return std::string(label);
  const std::string normalized = prism::NormalizeArchivePath(path);
  const std::string_view leaf = prism::FileNameOf(normalized);
  return leaf.empty() ? std::string("?") : std::string(leaf);
}

// ------------------------------------------------------- C104 folder icon
// C124: THE FOLDER IS GOLD (see ThemeTokens' FolderHighlight/Body/Shadow). It
// used to be the accent mixed toward the panel -- the fourth blue, and the same
// hue as the selection bar, so the icon and the state could not be told apart
// (UI-AUDIT 12.5). The three tones are tokens; what lives HERE is only the
// geometry and the alphas, both of which the pixel suite measures.
struct FolderIcon {
  float bodyAlpha = 0.92f;    // <= 0.95 by contract: never an opaque plate
  float tabAlpha = 0.78f;     // the back tab, under the same wash
  float shadowAlpha = 0.88f;  // the 1 DIP shadow row on the face's bottom edge
  float bodyRadius = 2.5f;
  float tabRadius = 1.5f;
};
inline constexpr FolderIcon kFolderIcon{};

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
// The app's entry point: the installed translator decides the four headings.
inline Layout Build(const prism::sidebar::Model& model, const Metrics& metrics);

inline Layout Build(const prism::sidebar::Model& model, const Metrics& metrics,
                    const Labels& labels) {
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
      // C79: 11 px, .5 px tracking, uppercase (the labels already are) and a
      // dimmer secondary role -- Finder's group heading, not another row.
      Command heading{PrimKind::SectionLabel, box, std::string(text), {},
                      ColorToken::SectionLabel, 0.75f, -1, section};
      heading.letterSpacing = 0.5f;
      push(std::move(heading));
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
        // C104: the highlight is a CAPSULE inset 8 px from both ends of the
        // row, with a 6 px corner (the square arm is the negative control). The
        // inset is applied here, in the view, because the view owns geometry:
        // the renderer must not shrink a box it was handed.
        const float inset = kSelectionPill ? metrics.selectionInset : 0.0f;
        const Box pill{row.left + inset, row.top, row.right - inset, row.bottom};
        Command selection{PrimKind::Selection, pill, {}, {},
                          ColorToken::SelectedBackground, 1.0f, index, section};
        selection.cornerRadius = kSelectionPill ? metrics.selectionRadius : 0.0f;
        push(std::move(selection));
        // C79/C124: the 2 DIP accent rule (THE SEAM) at the ROW's left edge. It is a separate
        // primitive (not a border on Selection) so the width is a number the
        // pixel suite can measure, and so the fill's alpha stays the token's
        // (14%) instead of being fused with an opaque edge.
        // C104: it stays on the ROW edge and does NOT follow the capsule's
        // inset. On a tree row the capsule's inset left edge (row.left + 8) is
        // exactly where the expander chevron's vertex lands, so a bar there is
        // notched by the chevron down to ~1 px of accent -- gallery_snapshot
        // measured exactly that ("the midnight selection rule is 1 px wide")
        // before this line moved back. The rule leads the capsule: the row owns
        // the rule, the capsule is inset from it.
        const float barTop = row.top + 2.0f;
        const float barBottom = row.bottom - 2.0f;
        push(Command{PrimKind::SelectionBar,
                     Box{row.left, barTop, row.left + metrics.selectionBarWidth, barBottom},
                     {}, {}, ColorToken::Accent, 1.0f, index, section});
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
    emitHeader(Section::Favorites, labels.favorites);
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
    emitHeader(Section::Recent, labels.recent);
    const auto& recent = model.Recent();
    for (std::size_t i = 0; i < recent.size(); ++i) {
      emitRow(Section::Recent, static_cast<int>(i), RowLabel({}, recent[i].path),
              "clock", 0, false, false, false);
    }
    y += metrics.sectionGap;
  }

  // --- ARCHIVE TREE --------------------------------------------------------
  emitHeader(Section::Tree, labels.tree);
  const auto& rows = model.Tree();
  if (rows.empty()) {
    const Box box{metrics.paddingX, y, metrics.width - metrics.paddingX,
                  y + metrics.rowHeight};
    if (visible(box)) {
      push(Command{PrimKind::EmptyState, box, labels.noArchive, "empty",
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

  // --- bottom telemetry card (anchored, never scrolled) --------------------
  // C105 (plan 3.2 "移除刺眼的下划线 / 标签与数值分层排版"): the pre-C105 capsule
  // was three one-line readouts ("AES-256 ENCRYPTED", "LZMA2 | DICT 64.0 MB",
  // "RATIO 42.8%") sitting under a full-width 1 px hairline -- the user's "hard
  // underline with the label and the value squeezed onto one line". The hairline
  // is gone in every shipping build (kCardUnderlineEnabled is false; the
  // mutation build restores it as the negative control) and every metric is a
  // 10 px TextSecondary label ABOVE a 12 px TextPrimary value.
  if (kCardUnderlineEnabled) {
    push(Command{PrimKind::Separator,
                 Box{metrics.paddingX, pillTop - metrics.statusGapAbove * 0.5f,
                     metrics.width - metrics.paddingX,
                     pillTop - metrics.statusGapAbove * 0.5f + 1.0f},
                 {}, {}, ColorToken::SecondaryText, 0.35f, -1, Section::None});
  }
  push(Command{PrimKind::StatusPill, layout.pill, {}, {}, ColorToken::Panel,
               status.hasArchive ? 1.0f : 0.6f, -1, Section::None});

  const float cardLeft = layout.pill.left + metrics.statusPadding;
  const float cardRight = layout.pill.right - metrics.statusPadding;
  const float textLeft = cardLeft + metrics.iconSize + metrics.iconGap;
  if (!status.hasArchive) {
    const Box lineTop{cardLeft, pillTop + metrics.statusPadding, cardRight,
                      pillTop + metrics.statusPadding + metrics.statusLineHeight};
    push(Command{PrimKind::StatusIcon, detail::GlyphBox(metrics, lineTop), {},
                 "empty", ColorToken::Muted, 0.7f, -1, Section::None});
    push(Command{PrimKind::StatusText,
                 Box{textLeft, lineTop.top, cardRight, lineTop.bottom},
                 labels.noArchive, {}, ColorToken::Muted, 0.7f, -1, Section::None});
    return layout;
  }

  // C105: the cipher is the VALUE under the "ENCRYPTION" label; when the engine
  // cannot name it the value is the honest word ("UNKNOWN" / "NOT ENCRYPTED"),
  // never a fabricated "AES-256".
  std::string cipher;
  switch (status.encryption) {
    case prism::sidebar::Encryption::Aes256: cipher = "AES-256"; break;
    case prism::sidebar::Encryption::None: cipher = labels.notEncrypted; break;
    case prism::sidebar::Encryption::Unknown: cipher = labels.unknownMethod; break;
  }
  const std::string method = status.method.empty() ? labels.unknownMethod : status.method;
  const std::string dictionary =
      status.dictionaryBytes == 0 ? std::string("--") : FormatBytes(status.dictionaryBytes);
  // C81/A3: the ratio VALUE is the CANONICAL view string -- the same one the
  // list cell and the status band render. FormatRatio stays for the bar's
  // fraction only, so the three surfaces cannot disagree about the number.
  const std::string ratioValue =
      status.ratioView.empty() ? FormatRatio(status.ratioPermille) : status.ratioView;

  struct CardRow {
    const char* icon;
    const std::string* label;
    const std::string* value;
  };
  const CardRow cardRows[4] = {
      {"lock", &labels.cardCrypto, &cipher},
      {"bolt", &labels.cardMethod, &method},
      {"drum", &labels.cardDictionary, &dictionary},
      {"chart", &labels.cardRatio, &ratioValue},
  };
  const float rowHeight = metrics.CardRowHeight();
  const float rowPitch = rowHeight + metrics.cardRowGap;
  float rowTop = pillTop + metrics.statusPadding;
  for (int i = 0; i < 4; ++i) {
    // The icon is centred on the PAIR (not on the value line): it belongs to
    // the metric, and centring it on one of the two lines makes the card read
    // as two unrelated columns.
    const float iconTop = rowTop + (rowHeight - metrics.iconSize) * 0.5f;
    push(Command{PrimKind::StatusIcon,
                 Box{cardLeft, iconTop, cardLeft + metrics.iconSize, iconTop + metrics.iconSize},
                 {}, std::string(cardRows[i].icon),
                 i == 0 && status.encrypted ? ColorToken::Locked : ColorToken::SecondaryText,
                 1.0f, i, Section::None});
    Command label{PrimKind::StatusLabel,
                  Box{textLeft, rowTop, cardRight, rowTop + metrics.cardLabelHeight},
                  *cardRows[i].label, {}, ColorToken::SecondaryText, 1.0f, i,
                  Section::None};
    label.fontSize = metrics.cardLabelFontSize;
    push(std::move(label));
    Command value{PrimKind::StatusText,
                  Box{textLeft, rowTop + metrics.cardLabelHeight, cardRight,
                      rowTop + rowHeight},
                  *cardRows[i].value, {}, ColorToken::PrimaryText, 1.0f, i,
                  Section::None};
    value.fontSize = metrics.cardValueFontSize;
    push(std::move(value));
    rowTop += rowPitch;
  }
  // The micro ratio bar closes the ratio pair. C105: it gets its OWN row, taken
  // from the same arithmetic that placed the label and the value above it --
  // cardBarGap below the value's bottom, cardBarHeight tall. The pre-fix bar was
  // drawn INSIDE the value line (bottom-5 .. bottom-2), which is a bar straight
  // through the "95.0%" glyphs: the integration build showed exactly that, and
  // the suite now asserts the two rectangles are disjoint.
  const float fraction =
      (std::min)(1.0f, static_cast<float>(status.ratioPermille) / 1000.0f);
  const Box ratioValueBox{textLeft, rowTop - rowPitch + metrics.cardLabelHeight, cardRight,
                          rowTop - rowPitch + rowHeight};
  Box barBox;
  if (kCardBarHasOwnRow) {
    const float barTop = ratioValueBox.bottom + metrics.cardBarGap;
    barBox = Box{ratioValueBox.left, barTop,
                 ratioValueBox.left + ratioValueBox.Width() * fraction,
                 barTop + metrics.cardBarHeight};
  } else {
    // NEGATIVE CONTROL (CONTRACTS D3): the pre-fix position, inside the value
    // line. The intersection assertion MUST go red here.
    barBox = Box{ratioValueBox.left, ratioValueBox.bottom - 5.0f,
                 ratioValueBox.left + ratioValueBox.Width() * fraction,
                 ratioValueBox.bottom - 2.0f};
  }
  push(Command{PrimKind::RatioBar, barBox, {}, {}, ColorToken::Accent, 0.9f, -1,
               Section::None});
  return layout;
}

inline Layout Build(const prism::sidebar::Model& model, const Metrics& metrics) {
  return Build(model, metrics, LabelsFrom(InstalledTranslator()));
}

}  // namespace prism::win::sidebar