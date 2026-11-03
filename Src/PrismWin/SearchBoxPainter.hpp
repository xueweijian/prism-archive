// PrismArchive -- PrismWin
// SearchBoxPainter: the search field as a LIST OF DRAWING INSTRUCTIONS.
//
// This file draws nothing. Build() turns (model state, a DIP rectangle) into
// primitives -- fill, border, text runs, caret, clear button, count badge --
// each carrying a C15 theme TOKEN and the colour resolved from it. The renderer
// owns D2D; the layout math lives here so it can be asserted without a device
// or an HWND. That split is what makes the pixel contract testable on every
// lane (the card must not re-decide geometry in the window).
//
// Tokens (C15 is the only colour authority -- no literal RGBA in this file):
//   field fill      --surface-floating-island
//   field border    --border-specular-rim        (gradient: top -> bottom)
//   focus ring      --accent-liquid-primary      (gradient)
//   selection       --accent-liquid-primary      (gradient fill)
//   query text      --text-primary
//   composition     --text-primary
//   placeholder     --text-secondary
//   caret           --accent-liquid-primary (from)
//   clear button    --surface-canvas-base, glyph --text-secondary
//   badge fill      --accent-liquid-primary, text --text-primary
//
// Text measurement is INJECTED (Options::measure, in DIPs). The renderer will
// pass DirectWrite; headless tests get EstimateTextWidth(), which is
// deterministic and documented, so the same input always yields the same
// rectangles.
//
// Geometry is DIP at 96 DPI; Build() emits PHYSICAL pixels (bounds * dpi/96),
// the same convention DialogChrome uses for its hit tests.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "SearchBoxModel.hpp"
#include "ThemeTokens.hpp"

namespace prism::win::searchbox {

namespace core = prism::search;
namespace theme = prism::theme;

struct Box {
  float left = 0.0f;
  float top = 0.0f;
  float right = 0.0f;
  float bottom = 0.0f;

  float Width() const { return right - left; }
  float Height() const { return bottom - top; }
  bool Empty() const { return Width() <= 0.0f || Height() <= 0.0f; }

  bool Contains(float x, float y) const {
    return x >= left && x <= right && y >= top && y <= bottom;
  }
  bool operator==(const Box&) const = default;
};

inline std::ostream& operator<<(std::ostream& os, const Box& box) {
  return os << "[" << box.left << "," << box.top << " " << box.right << "x" << box.bottom
            << "]";
}

// One primitive. The renderer switches on kind; the test asserts on this
// vector instead of on pixels, so no device is needed to prove the geometry.
enum class Primitive : uint8_t {
  FieldBackground,  // rounded-rect fill
  FieldBorder,      // rounded-rect stroke, gradient
  FocusRing,        // rounded-rect stroke, accent gradient (focused only)
  TextSelection,    // selection rectangle behind the text (gradient fill)
  QueryText,        // committed query run
  CompositionText,  // IME preview run (underlined by the renderer)
  Placeholder,      // shown only when the box is empty
  Caret,            // the 1.5 DIP insertion bar
  ClearButton,      // round button, only with text
  ClearGlyph,       // one of the two X strokes (rotation +/-45)
  Badge,            // count/empty-state pill
  BadgeText,        // its label
};

inline std::ostream& operator<<(std::ostream& os, Primitive primitive) {
  switch (primitive) {
    case Primitive::FieldBackground: return os << "FieldBackground";
    case Primitive::FieldBorder: return os << "FieldBorder";
    case Primitive::FocusRing: return os << "FocusRing";
    case Primitive::TextSelection: return os << "TextSelection";
    case Primitive::QueryText: return os << "QueryText";
    case Primitive::CompositionText: return os << "CompositionText";
    case Primitive::Placeholder: return os << "Placeholder";
    case Primitive::Caret: return os << "Caret";
    case Primitive::ClearButton: return os << "ClearButton";
    case Primitive::ClearGlyph: return os << "ClearGlyph";
    case Primitive::Badge: return os << "Badge";
    case Primitive::BadgeText: return os << "BadgeText";
  }
  return os << "?";
}

// What a click at a physical pixel hits. The window only forwards the click;
// the geometry lives here with the paint instructions.
enum class Target : uint8_t { None, Field, ClearButton, Badge };

inline std::ostream& operator<<(std::ostream& os, Target target) {
  switch (target) {
    case Target::None: return os << "None";
    case Target::Field: return os << "Field";
    case Target::ClearButton: return os << "ClearButton";
    case Target::Badge: return os << "Badge";
  }
  return os << "?";
}

struct Instruction {
  Primitive primitive = Primitive::FieldBackground;
  Box rect;
  Box clip;  // text runs are clipped to the text area
  theme::TokenId token = theme::TokenId::Count;
  theme::Rgba color{};    // solid colour, or the gradient's first stop
  theme::Rgba colorTo{};  // gradient's second stop (== color when solid)
  bool gradient = false;
  float cornerRadius = 0.0f;
  float rotationDegrees = 0.0f;
  float fontSize = 0.0f;
  std::string text;  // text runs + badge label
};

struct Scene {
  float scale = 1.0f;
  Box field;
  Box textArea;
  Box caret;
  Box clearButton;
  Box badge;
  std::vector<Instruction> instructions;

  bool Empty() const { return instructions.empty(); }

  // Front-to-back: the clear button sits above the badge, both above the field.
  Target HitTest(float x, float y) const {
    if (!clearButton.Empty() && clearButton.Contains(x, y)) return Target::ClearButton;
    if (!badge.Empty() && badge.Contains(x, y)) return Target::Badge;
    if (field.Contains(x, y)) return Target::Field;
    return Target::None;
  }

  const Instruction* Find(Primitive primitive) const {
    for (const Instruction& instruction : instructions) {
      if (instruction.primitive == primitive) return &instruction;
    }
    return nullptr;
  }

  std::size_t Count(Primitive primitive) const {
    std::size_t count = 0;
    for (const Instruction& instruction : instructions) {
      if (instruction.primitive == primitive) ++count;
    }
    return count;
  }
};

// Deterministic headless text width: ASCII advances 0.55 em, every non-ASCII
// code point 1.0 em (CJK is full-width). The renderer replaces this with a
// DirectWrite measurement; tests use it so rectangles are reproducible.
inline float EstimateTextWidth(std::string_view text, float fontSize) {
  float width = 0.0f;
  for (std::size_t at = 0; at < text.size();) {
    const unsigned char lead = static_cast<unsigned char>(text[at]);
    const std::size_t length = prism::list::Utf8SequenceLength(text, at);
    width += lead < 0x80u ? fontSize * 0.55f : fontSize;
    at += length == 0 ? 1 : length;
  }
  return width;
}

struct Options {
  theme::Variant variant = theme::Variant::Midnight;
  int dpi = 96;                 // scene coordinates are emitted at this DPI
  float fontSize = 14.0f;       // DIP
  float cornerRadius = 8.0f;    // DIP
  float textPadding = 10.0f;    // DIP, left inset
  float textGap = 6.0f;         // DIP, between text and the right-hand controls
  float clearSize = 22.0f;      // DIP, square button
  float clearInset = 5.0f;      // DIP from the field's right edge
  float badgeHeight = 18.0f;    // DIP
  float badgePadding = 8.0f;    // DIP, horizontal
  float badgeGap = 4.0f;        // DIP between badge and clear button
  float caretWidth = 1.5f;      // DIP
  // DIP text width. Empty -> EstimateTextWidth(text, fontSize).
  std::function<float(std::string_view)> measure;
};

class SearchBoxPainter {
 public:
  explicit SearchBoxPainter(Options options = Options{}) : options_(std::move(options)) {}

  const Options& GetOptions() const { return options_; }

  Scene Build(const core::Model& model, const Box& bounds) const {
    Scene scene;
    const float scale =
        options_.dpi > 0 ? static_cast<float>(options_.dpi) / 96.0f : 1.0f;
    scene.scale = scale;

    // A closed search box paints nothing at all: no stale filter, no caret.
    if (!model.IsOpen()) return scene;

    const Box field = bounds;
    scene.field = Scale(field, scale);
    if (field.Empty()) return scene;

    const std::string display = model.DisplayText();
    const std::string status = model.StatusText();
    const bool hasClear = !display.empty();
    const bool hasBadge = !status.empty();

    const float centerY = (field.top + field.bottom) * 0.5f;

    // Right-hand controls, laid out from the field's right edge inwards.
    Box clear{};
    float controlsLeft = field.right - options_.clearInset;
    if (hasClear) {
      clear = Box{controlsLeft - options_.clearSize, centerY - options_.clearSize * 0.5f,
                  controlsLeft, centerY + options_.clearSize * 0.5f};
      controlsLeft = clear.left;
    }

    Box badge{};
    if (hasBadge) {
      const float width = Measure(status) + options_.badgePadding * 2.0f;
      badge = Box{controlsLeft - options_.badgeGap - width, centerY - options_.badgeHeight * 0.5f,
                  controlsLeft - options_.badgeGap, centerY + options_.badgeHeight * 0.5f};
      controlsLeft = badge.left;
    }

    const float textRight = (hasClear || hasBadge) ? controlsLeft - options_.textGap
                                                   : field.right - options_.textPadding;
    const float textLeft = field.left + options_.textPadding;
    const float fontHalf = options_.fontSize * 0.5f;
    // A field narrower than its padding is a degenerate layout, not an
    // inverted rectangle -- same rule as WindowLayout's Inset.
    Box textArea{textLeft, centerY - fontHalf, textRight, centerY + fontHalf};
    if (textArea.right < textArea.left) textArea.right = textArea.left;

    scene.textArea = Scale(textArea, scale);
    scene.clearButton = Scale(clear, scale);
    scene.badge = Scale(badge, scale);

    // --- background + border + focus ---------------------------------------
    Instruction background;
    background.primitive = Primitive::FieldBackground;
    background.rect = scene.field;
    background.cornerRadius = options_.cornerRadius * scale;
    ApplyToken(background, theme::TokenId::SurfaceFloatingIsland);
    scene.instructions.push_back(std::move(background));

    Instruction border;
    border.primitive = Primitive::FieldBorder;
    border.rect = scene.field;
    border.cornerRadius = options_.cornerRadius * scale;
    ApplyToken(border, theme::TokenId::BorderSpecularRim);
    scene.instructions.push_back(std::move(border));

    if (model.Focused()) {
      Instruction ring;
      ring.primitive = Primitive::FocusRing;
      ring.rect = Inset(scene.field, 1.0f * scale);
      ring.cornerRadius = (options_.cornerRadius - 1.0f) * scale;
      ApplyToken(ring, theme::TokenId::AccentLiquidPrimary);
      scene.instructions.push_back(std::move(ring));
    }

    // --- selection ----------------------------------------------------------
    if (!model.Composing() && model.HasSelection()) {
      const std::size_t start = model.SelectionStart();
      const std::size_t end = model.SelectionEnd();
      const float x0 = textArea.left + Measure(display.substr(0, start));
      const float x1 = x0 + Measure(display.substr(start, end - start));
      Instruction selection;
      selection.primitive = Primitive::TextSelection;
      selection.rect = Scale(Box{x0, textArea.top, x1, textArea.bottom}, scale);
      selection.cornerRadius = 2.0f * scale;
      ApplyToken(selection, theme::TokenId::AccentLiquidPrimary);
      scene.instructions.push_back(std::move(selection));
    }

    // --- text runs ----------------------------------------------------------
    if (display.empty()) {
      Instruction placeholder;
      placeholder.primitive = Primitive::Placeholder;
      placeholder.rect = scene.textArea;
      placeholder.clip = scene.textArea;
      placeholder.fontSize = options_.fontSize * scale;
      placeholder.text = std::string(core::kPlaceholderText);
      ApplyToken(placeholder, theme::TokenId::TextSecondary);
      scene.instructions.push_back(std::move(placeholder));
    } else if (model.Composing()) {
      const std::size_t start = model.SelectionStart();
      const std::size_t end = model.SelectionEnd();
      float pen = textArea.left;
      EmitText(model.Query().substr(0, start), Primitive::QueryText, textArea, pen, scene);
      EmitText(model.Composition(), Primitive::CompositionText, textArea, pen, scene);
      EmitText(model.Query().substr(end), Primitive::QueryText, textArea, pen, scene);
    } else {
      float pen = textArea.left;
      EmitText(model.Query(), Primitive::QueryText, textArea, pen, scene);
    }

    // --- caret --------------------------------------------------------------
    if (model.Focused()) {
      const std::string shown = model.DisplayText();
      const std::size_t at = model.DisplayCaret() > shown.size() ? shown.size()
                                                                 : model.DisplayCaret();
      float caretX = textArea.left + Measure(std::string_view(shown).substr(0, at));
      if (caretX < textArea.left) caretX = textArea.left;
      if (caretX > textArea.right) caretX = textArea.right;
      const float caretW = options_.caretWidth;
      scene.caret = Scale(Box{caretX, centerY - fontHalf, caretX + caretW, centerY + fontHalf},
                          scale);
      Instruction caret;
      caret.primitive = Primitive::Caret;
      caret.rect = scene.caret;
      ApplyToken(caret, theme::TokenId::AccentLiquidPrimary);
      scene.instructions.push_back(std::move(caret));
    }

    // --- clear button -------------------------------------------------------
    if (hasClear) {
      Instruction button;
      button.primitive = Primitive::ClearButton;
      button.rect = scene.clearButton;
      button.cornerRadius = scene.clearButton.Width() * 0.5f;
      ApplyToken(button, theme::TokenId::SurfaceCanvasBase);
      scene.instructions.push_back(std::move(button));

      // Two strokes of an X, each a thin rectangle rotated about its centre.
      // The renderer applies the rotation; the test pins the +/-45 degrees and
      // the fact that both strokes share one centre.
      const float strokeLength = options_.clearSize * 0.5f;
      const float strokeWidth = 1.5f;
      for (int i = 0; i < 2; ++i) {
        const float cx = (clear.left + clear.right) * 0.5f;
        const float cy = (clear.top + clear.bottom) * 0.5f;
        Instruction glyph;
        glyph.primitive = Primitive::ClearGlyph;
        glyph.rect = Scale(Box{cx - strokeLength * 0.5f, cy - strokeWidth * 0.5f,
                               cx + strokeLength * 0.5f, cy + strokeWidth * 0.5f},
                           scale);
        glyph.rotationDegrees = i == 0 ? 45.0f : -45.0f;
        glyph.cornerRadius = strokeWidth * 0.5f * scale;
        ApplyToken(glyph, theme::TokenId::TextSecondary);
        scene.instructions.push_back(std::move(glyph));
      }
    }

    // --- badge --------------------------------------------------------------
    if (hasBadge) {
      Instruction pill;
      pill.primitive = Primitive::Badge;
      pill.rect = scene.badge;
      pill.cornerRadius = scene.badge.Height() * 0.5f;
      ApplyToken(pill, theme::TokenId::AccentLiquidPrimary);
      scene.instructions.push_back(std::move(pill));

      Instruction label;
      label.primitive = Primitive::BadgeText;
      label.rect = Scale(Box{badge.left + options_.badgePadding, textArea.top,
                             badge.left + options_.badgePadding + Measure(status),
                             textArea.bottom},
                         scale);
      label.clip = scene.badge;
      label.fontSize = options_.fontSize * scale;
      label.text = status;
      ApplyToken(label, theme::TokenId::TextPrimary);
      scene.instructions.push_back(std::move(label));
    }

    return scene;
  }

 private:
  float Measure(std::string_view text) const {
    if (options_.measure) return options_.measure(text);
    return EstimateTextWidth(text, options_.fontSize);
  }

  static Box Scale(const Box& box, float scale) {
    return Box{box.left * scale, box.top * scale, box.right * scale, box.bottom * scale};
  }

  static Box Inset(const Box& box, float by) {
    return Box{box.left + by, box.top + by, box.right - by, box.bottom - by};
  }

  // A run is placed at the caller's pen (DIP) and advances it, so a composing
  // query's prefix / preview / suffix cannot drift apart at a fractional DPI.
  void EmitText(std::string_view text, Primitive primitive, const Box& textArea,
                float& pen, Scene& scene) const {
    if (text.empty()) return;  // no zero-width run: "nothing to draw" is absence
    const float width = Measure(text);
    Instruction run;
    run.primitive = primitive;
    run.rect = Scale(Box{pen, textArea.top, pen + width, textArea.bottom}, scene.scale);
    run.clip = Scale(textArea, scene.scale);
    run.fontSize = options_.fontSize * scene.scale;
    run.text = std::string(text);
    ApplyToken(run, theme::TokenId::TextPrimary);
    scene.instructions.push_back(std::move(run));
    pen += width;
  }

  void ApplyToken(Instruction& instruction, theme::TokenId token) const {
    const theme::TokenValue value = theme::Lookup(token, options_.variant);
    instruction.token = token;
    instruction.gradient = value.kind == theme::Kind::Gradient;
    if (instruction.gradient) {
      instruction.color = value.gradient.from;
      instruction.colorTo = value.gradient.to;
    } else {
      instruction.color = value.color;
      instruction.colorTo = value.color;
    }
  }

  Options options_;
};

}  // namespace prism::win::searchbox
