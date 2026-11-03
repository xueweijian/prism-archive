// PrismArchive -- PrismWin (card C62)
// SEARCH BOX PAINT INSTRUCTIONS, asserted headlessly (no window, no D2D device).
//
// Src/PrismWin/SearchBoxPainter.hpp emits primitives, not pixels. This suite
// pins the four things the later UI card must be able to trust:
//
//   1. WHICH primitives: background, border, text run, caret, clear button (and
//      its two X strokes), count badge -- exactly the six layers the card lists;
//   2. WHERE: the field is the caller's rectangle, the caret sits at the text
//      pen, the clear button is a square inside the right edge, the badge sits
//      to its left, and every text run is clipped to the text area;
//   3. WHAT COLOUR: every instruction carries a C15 TokenId and the colour
//      resolved from that token -- no literal RGBA, no magenta sentinel, and a
//      theme switch changes the field fill;
//   4. HIT TESTING: the same geometry decides which target a click reaches, so
//      the window never needs a second copy of the layout math.
//
// DPI: bounds are DIP; the scene is emitted in physical pixels (bounds * dpi/96).
// 96 and 144 are both asserted, so a hard-coded pixel constant cannot hide.
#include <windows.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "../mini_test.hpp"
#include "../../Src/PrismWin/SearchBoxPainter.hpp"

namespace {

namespace core = prism::search;
namespace sb = prism::win::searchbox;
namespace theme = prism::theme;

using prism::list::Row;
using prism::list::TypeTagOf;

Row MakeRow(std::string name, uint32_t index, bool isDir = false) {
  Row row;
  row.catalogIndex = index;
  row.name = std::move(name);
  row.typeTag = TypeTagOf(row.name);
  row.size = 100;
  row.packSize = 50;
  row.isDir = isDir;
  if (isDir) {
    row.size = 0;
    row.packSize = 0;
    row.typeTag.clear();
  }
  return row;
}

std::vector<Row> Fixture() {
  return {
      MakeRow("年度报告.txt", 0),
      MakeRow("report.docx", 1),
      MakeRow("照片", 2, true),
      MakeRow("notes.md", 3),
      MakeRow("报告草稿.txt", 4),
  };
}

bool SameColor(const theme::Rgba& a, const theme::Rgba& b) {
  return a.r == b.r && a.g == b.g && a.b == b.b && a.a == b.a;
}

bool InField(const sb::Box& outer, const sb::Box& inner) {
  return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
         inner.bottom <= outer.bottom;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Which primitives an active search emits.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.emits_the_layers_the_card_lists") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});

  PRISM_CHECK(!scene.Empty());
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::FieldBackground), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::FieldBorder), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::FocusRing), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::QueryText), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::Caret), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::ClearButton), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::ClearGlyph), std::size_t{2});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::Badge), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::BadgeText), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::Placeholder), std::size_t{0});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::TextSelection), std::size_t{0});

  const sb::Instruction* text = scene.Find(sb::Primitive::QueryText);
  if (!PRISM_CHECK(text != nullptr)) return;
  PRISM_CHECK_EQ(text->text, std::string("报告"));
  const sb::Instruction* badge = scene.Find(sb::Primitive::BadgeText);
  if (!PRISM_CHECK(badge != nullptr)) return;
  PRISM_CHECK_EQ(badge->text, std::string("过滤中 2 条"));

  // The X is two strokes with opposite rotations, both centred on the button.
  const float cx = (scene.clearButton.left + scene.clearButton.right) * 0.5f;
  const float cy = (scene.clearButton.top + scene.clearButton.bottom) * 0.5f;
  int strokes = 0;
  for (const sb::Instruction& instruction : scene.instructions) {
    if (instruction.primitive != sb::Primitive::ClearGlyph) continue;
    ++strokes;
    PRISM_CHECK(std::abs(instruction.rotationDegrees) == 45.0f);
    PRISM_CHECK_EQ((instruction.rect.left + instruction.rect.right) * 0.5f, cx);
    PRISM_CHECK_EQ((instruction.rect.top + instruction.rect.bottom) * 0.5f, cy);
  }
  PRISM_CHECK_EQ(strokes, 2);
}

// ---------------------------------------------------------------------------
// 2. Rectangles: inside the field, in paint order, at the text pen.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.geometry_stays_inside_the_field") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");

  sb::Options options;  // Midnight / 96 DPI / 14 DIP text
  sb::SearchBoxPainter painter(options);
  const sb::Box bounds{0.0f, 0.0f, 320.0f, 32.0f};
  const sb::Scene scene = painter.Build(model, bounds);

  // The field IS the caller's rectangle (96 DPI -> 1:1).
  PRISM_CHECK_EQ(scene.field, bounds);
  PRISM_CHECK_EQ(scene.field.Width(), 320.0f);
  PRISM_CHECK_EQ(scene.field.Height(), 32.0f);

  // Right-hand controls: a 22 DIP square 5 DIP from the right edge, then the
  // badge 4 DIP to its left.
  PRISM_CHECK_EQ(scene.clearButton.Width(), 22.0f);
  PRISM_CHECK_EQ(scene.clearButton.Height(), 22.0f);
  PRISM_CHECK_EQ(scene.clearButton.right, 315.0f);
  PRISM_CHECK_EQ(scene.badge.right, 289.0f);
  PRISM_CHECK_EQ(scene.badge.Height(), 18.0f);
  PRISM_CHECK(InField(scene.field, scene.clearButton));
  PRISM_CHECK(InField(scene.field, scene.badge));

  // The text area ends before the badge; every text run is clipped to it.
  const float badgeText = sb::EstimateTextWidth("过滤中 2 条", 14.0f);
  PRISM_CHECK_EQ(scene.badge.left, 289.0f - (badgeText + 16.0f));
  PRISM_CHECK_EQ(scene.textArea.right, scene.badge.left - 6.0f);
  PRISM_CHECK_EQ(scene.textArea.left, 10.0f);
  const sb::Instruction* run = scene.Find(sb::Primitive::QueryText);
  if (!PRISM_CHECK(run != nullptr)) return;
  PRISM_CHECK_EQ(run->rect.left, 10.0f);
  PRISM_CHECK_EQ(run->rect.Width(), sb::EstimateTextWidth("报告", 14.0f));
  PRISM_CHECK_EQ(run->clip, scene.textArea);
  PRISM_CHECK_EQ(run->fontSize, 14.0f);

  // Caret: 1.5 DIP wide, font-high, at the end of the committed run.
  PRISM_CHECK_EQ(scene.caret.left, 10.0f + sb::EstimateTextWidth("报告", 14.0f));
  PRISM_CHECK_EQ(scene.caret.Width(), 1.5f);
  PRISM_CHECK_EQ(scene.caret.Height(), 14.0f);
  PRISM_CHECK(InField(scene.textArea, scene.caret));
}

PRISM_TEST("painter.caret_tracks_the_caret_not_the_text_end") {
  core::Model model;
  model.Open();
  model.InsertText("abcdef");
  model.Press(core::Key::Home);
  model.Press(core::Key::Right);
  model.Press(core::Key::Right);

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  PRISM_CHECK_EQ(scene.caret.left, 10.0f + sb::EstimateTextWidth("ab", 14.0f));
}

// ---------------------------------------------------------------------------
// 3. Colours: C15 tokens only.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.every_colour_comes_from_a_c15_token") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  PRISM_CHECK(!scene.instructions.empty());

  for (const sb::Instruction& instruction : scene.instructions) {
    PRISM_CHECK(instruction.token != theme::TokenId::Count);
    const theme::TokenValue value = theme::Lookup(instruction.token, theme::Variant::Midnight);
    PRISM_CHECK(value.known);
    if (value.kind == theme::Kind::Gradient) {
      PRISM_CHECK(instruction.gradient);
      PRISM_CHECK(SameColor(instruction.color, value.gradient.from));
      PRISM_CHECK(SameColor(instruction.colorTo, value.gradient.to));
    } else {
      PRISM_CHECK(!instruction.gradient);
      PRISM_CHECK(SameColor(instruction.color, value.color));
    }
    PRISM_CHECK(!SameColor(instruction.color, theme::kUnknownFallbackColor));
  }

  const sb::Instruction* background = scene.Find(sb::Primitive::FieldBackground);
  if (!PRISM_CHECK(background != nullptr)) return;
  PRISM_CHECK_EQ(background->token, theme::TokenId::SurfaceFloatingIsland);
  PRISM_CHECK(SameColor(
      background->color,
      theme::Lookup(theme::TokenId::SurfaceFloatingIsland, theme::Variant::Midnight).color));

  const sb::Instruction* border = scene.Find(sb::Primitive::FieldBorder);
  if (!PRISM_CHECK(border != nullptr)) return;
  PRISM_CHECK_EQ(border->token, theme::TokenId::BorderSpecularRim);
  PRISM_CHECK(border->gradient);

  const sb::Instruction* caret = scene.Find(sb::Primitive::Caret);
  if (!PRISM_CHECK(caret != nullptr)) return;
  PRISM_CHECK_EQ(caret->token, theme::TokenId::AccentLiquidPrimary);

  // A theme switch really changes a value this scene uses.
  sb::Options daylight;
  daylight.variant = theme::Variant::Daylight;
  const sb::Scene light = sb::SearchBoxPainter(daylight)
                              .Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  const sb::Instruction* lightBackground = light.Find(sb::Primitive::FieldBackground);
  if (!PRISM_CHECK(lightBackground != nullptr)) return;
  PRISM_CHECK(!SameColor(lightBackground->color, background->color));
}

PRISM_TEST("painter.placeholder_and_empty_state_use_the_right_tokens") {
  core::Model empty;
  empty.Open();
  sb::SearchBoxPainter painter;
  const sb::Scene placeholders =
      painter.Build(empty, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  const sb::Instruction* placeholder = placeholders.Find(sb::Primitive::Placeholder);
  if (!PRISM_CHECK(placeholder != nullptr)) return;
  PRISM_CHECK_EQ(placeholder->text, std::string("搜索"));
  PRISM_CHECK_EQ(placeholder->token, theme::TokenId::TextSecondary);
  PRISM_CHECK_EQ(placeholders.Count(sb::Primitive::ClearButton), std::size_t{0});
  PRISM_CHECK_EQ(placeholders.Count(sb::Primitive::Badge), std::size_t{0});

  core::Model missing;
  missing.SetRows(Fixture());
  missing.Open();
  missing.InsertText("zzz");
  const sb::Scene none = painter.Build(missing, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  const sb::Instruction* badge = none.Find(sb::Primitive::BadgeText);
  if (!PRISM_CHECK(badge != nullptr)) return;
  PRISM_CHECK_EQ(badge->text, std::string("无结果"));
  PRISM_CHECK_EQ(badge->text.size(), std::size_t{9});
  PRISM_CHECK_EQ(badge->token, theme::TokenId::TextPrimary);
  PRISM_CHECK_EQ(none.badge.Width(), sb::EstimateTextWidth("无结果", 14.0f) + 16.0f);
}

// ---------------------------------------------------------------------------
// 4. DPI: the scene is emitted in physical pixels.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.scales_every_rectangle_with_the_dpi") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");

  sb::Options options;
  options.dpi = 144;
  sb::SearchBoxPainter painter(options);
  const sb::Box bounds{0.0f, 0.0f, 320.0f, 32.0f};  // DIP
  const sb::Scene scene = painter.Build(model, bounds);

  PRISM_CHECK_EQ(scene.scale, 1.5f);
  PRISM_CHECK_EQ(scene.field.Width(), 480.0f);
  PRISM_CHECK_EQ(scene.field.Height(), 48.0f);
  PRISM_CHECK_EQ(scene.clearButton.Width(), 33.0f);
  PRISM_CHECK_EQ(scene.badge.Height(), 27.0f);
  PRISM_CHECK_EQ(scene.caret.Width(), 2.25f);
  PRISM_CHECK_EQ(scene.caret.Height(), 21.0f);
  const sb::Instruction* background = scene.Find(sb::Primitive::FieldBackground);
  if (!PRISM_CHECK(background != nullptr)) return;
  PRISM_CHECK_EQ(background->cornerRadius, 12.0f);
}

// ---------------------------------------------------------------------------
// 5. Hit testing.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.hit_test_matches_the_painted_geometry") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("报告");

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});

  const float clearX = (scene.clearButton.left + scene.clearButton.right) * 0.5f;
  const float clearY = (scene.clearButton.top + scene.clearButton.bottom) * 0.5f;
  PRISM_CHECK_EQ(scene.HitTest(clearX, clearY), sb::Target::ClearButton);

  const float badgeX = (scene.badge.left + scene.badge.right) * 0.5f;
  const float badgeY = (scene.badge.top + scene.badge.bottom) * 0.5f;
  PRISM_CHECK_EQ(scene.HitTest(badgeX, badgeY), sb::Target::Badge);

  PRISM_CHECK_EQ(scene.HitTest(4.0f, 16.0f), sb::Target::Field);
  PRISM_CHECK_EQ(scene.HitTest(-1.0f, 16.0f), sb::Target::None);
  PRISM_CHECK_EQ(scene.HitTest(400.0f, 16.0f), sb::Target::None);
  PRISM_CHECK_EQ(scene.HitTest(20.0f, 40.0f), sb::Target::None);

  // Clearing the query removes both right-hand controls; the old clear point
  // must then fall through to the field instead of hitting a ghost button.
  model.ClearQuery();
  const sb::Scene cleared = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  PRISM_CHECK_EQ(cleared.Count(sb::Primitive::ClearButton), std::size_t{0});
  PRISM_CHECK_EQ(cleared.HitTest(clearX, clearY), sb::Target::Field);
  PRISM_CHECK_EQ(cleared.HitTest(badgeX, badgeY), sb::Target::Field);
}

// ---------------------------------------------------------------------------
// 6. Text runs: selection, composition, closed box.
// ---------------------------------------------------------------------------
PRISM_TEST("painter.selection_highlight_covers_the_selected_run") {
  core::Model model;
  model.Open();
  model.InsertText("abc");
  model.Press(core::Key::SelectAll);

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  const sb::Instruction* selection = scene.Find(sb::Primitive::TextSelection);
  if (!PRISM_CHECK(selection != nullptr)) return;
  PRISM_CHECK_EQ(selection->rect.left, 10.0f);
  // Compare ENDPOINTS, not the derived width: left + width re-rounds in float
  // and a test that asserts arithmetic noise is a brittle test, not a sharper one.
  PRISM_CHECK_EQ(selection->rect.right, 10.0f + sb::EstimateTextWidth("abc", 14.0f));
  PRISM_CHECK_EQ(selection->token, theme::TokenId::AccentLiquidPrimary);
  // The caret sits at the end of the selected run.
  PRISM_CHECK_EQ(scene.caret.left, 10.0f + sb::EstimateTextWidth("abc", 14.0f));
}

PRISM_TEST("painter.ime_composition_is_a_separate_run_and_the_query_is_committed") {
  core::Model model;
  model.SetRows(Fixture());
  model.Open();
  model.InsertText("ab");
  model.BeginComposition();
  model.UpdateComposition("报告");

  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});

  PRISM_CHECK_EQ(model.Query(), std::string("ab"));  // painting never commits
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::CompositionText), std::size_t{1});
  PRISM_CHECK_EQ(scene.Count(sb::Primitive::QueryText), std::size_t{1});
  const sb::Instruction* committed = scene.Find(sb::Primitive::QueryText);
  const sb::Instruction* preview = scene.Find(sb::Primitive::CompositionText);
  if (!PRISM_CHECK(committed != nullptr) || !PRISM_CHECK(preview != nullptr)) return;
  PRISM_CHECK_EQ(committed->text, std::string("ab"));
  PRISM_CHECK_EQ(preview->text, std::string("报告"));
  PRISM_CHECK_EQ(preview->rect.left, 10.0f + sb::EstimateTextWidth("ab", 14.0f));
  PRISM_CHECK_EQ(scene.caret.left,
                 10.0f + sb::EstimateTextWidth("ab", 14.0f) +
                     sb::EstimateTextWidth("报告", 14.0f));
  PRISM_CHECK_EQ(preview->token, theme::TokenId::TextPrimary);
}

PRISM_TEST("painter.a_closed_box_emits_nothing") {
  core::Model model;
  model.SetRows(Fixture());
  sb::SearchBoxPainter painter;
  const sb::Scene scene = painter.Build(model, sb::Box{0.0f, 0.0f, 320.0f, 32.0f});
  PRISM_CHECK(scene.Empty());
  PRISM_CHECK(scene.field.Empty());
  PRISM_CHECK_EQ(scene.HitTest(10.0f, 10.0f), sb::Target::None);
}

int main(int argc, char** argv) {
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return rc;
  std::cout << "ALL SEARCH-PAINTER CHECKS PASSED" << std::endl;
  return 0;
}
