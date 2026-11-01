// PrismArchive -- PrismWin
// Quick Look overlay SHELL (blueprint ch. 9.1 row 1: Space opens the Liquid
// Glass preview card, Esc closes it). Structure only -- no HWND, no D2D.
//
// Two reasons this exists as a separate object instead of more members on the
// window:
//   1. the CONTENT is a decision (which kind of preview, is it too big to
//      decode, is the payload still streaming in?) and it belongs to
//      PrismCore's QuickLookPlanner -- this shell only turns that decision into
//      the lines a renderer can draw;
//   2. geometry and content must come from one source, or a click on the card
//      lands somewhere the pixels are not (the batch-A breadcrumb lesson).
//
// The window keeps the QuickLookSession (which item, open/closed) because that
// state is also what the gallery highlights; the shell is told what to show.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../PrismCore/ArchiveCatalog.hpp"
#include "../PrismCore/PathSafety.hpp"
#include "../PrismCore/Preview.hpp"

namespace prism::win::preview {

struct OverlayRect {
  int left = 0;
  int top = 0;
  int right = 0;
  int bottom = 0;

  int Width() const { return right - left; }
  int Height() const { return bottom - top; }
  bool Contains(int x, int y) const {
    return x >= left && x < right && y >= top && y < bottom;
  }
};

struct OverlayLine {
  std::string text;
  bool dim = false;  // secondary metadata (size, kind), painted dimmer
};

class QuickLookOverlay {
 public:
  static constexpr int kMinWidth = 420;
  static constexpr int kMaxWidth = 880;
  static constexpr int kMinHeight = 260;
  static constexpr int kMaxHeight = 620;
  static constexpr int kPadding = 20;

  // Content from the archive item. Streaming is the solid-block case: the
  // bytes are still being produced, so the card must SAY so instead of showing
  // an empty preview and looking broken.
  void SetContent(const ArchiveItem& item, bool streaming) {
    item_ = item;
    streaming_ = streaming;
    title_ = std::string(prism::FileNameOf(prism::NormalizeArchivePath(item.path)));
    if (title_.empty()) title_ = "(unnamed)";
    const prism::QuickLookPlanner::Decision decision =
        prism::QuickLookPlanner::Decide(item);
    kind_ = decision.kind;
    lines_.clear();
    if (decision.kind == prism::PreviewKind::Directory) {
      // A folder has no payload: say so rather than invent a preview.
      lines_.push_back({"Folder", true});
      return;
    }
    OverlayLine size;
    size.text = FormatSize(item.size);
    size.dim = true;
    lines_.push_back(size);
    OverlayLine kind;
    kind.text = KindLabel(decision.kind);
    kind.dim = true;
    lines_.push_back(kind);
    if (decision.exceedsLimit) {
      // Bomb hygiene: decodable, but too big to decode on a keystroke.
      OverlayLine big;
      big.text = "Large file: preview bounded";
      big.dim = false;
      lines_.push_back(big);
    }
    if (streaming_) {
      OverlayLine streamingLine;
      streamingLine.text = "Streaming";
      streamingLine.dim = false;
      lines_.push_back(streamingLine);
    }
  }

  void Open() { open_ = true; }
  void Close() { open_ = false; }
  bool IsOpen() const { return open_; }
  // ToggleAt's half: Space on the same item closes, on another item opens.
  bool Toggle() {
    open_ = !open_;
    return open_;
  }

  const std::string& Title() const { return title_; }
  const std::vector<OverlayLine>& Lines() const { return lines_; }
  prism::PreviewKind Kind() const { return kind_; }
  bool streaming() const { return streaming_; }

  // Centred card, 48% x 62% of the client, clamped. The preview content itself
  // is painted by the renderer inside the padded box.
  OverlayRect Frame(int clientW, int clientH) const {
    int width = (clientW * 48) / 100;
    if (width < kMinWidth) width = kMinWidth;
    if (width > kMaxWidth) width = kMaxWidth;
    if (clientW > 0 && width > clientW) width = clientW;
    int height = (clientH * 62) / 100;
    if (height < kMinHeight) height = kMinHeight;
    if (height > kMaxHeight) height = kMaxHeight;
    if (clientH > 0 && height > clientH) height = clientH;
    OverlayRect frame;
    frame.left = (clientW - width) / 2;
    frame.top = (clientH - height) / 2;
    frame.right = frame.left + width;
    frame.bottom = frame.top + height;
    return frame;
  }

  // The area the preview raster/decoded image may occupy: the card minus its
  // padding and the title strip. Handing this to the renderer keeps the image
  // and the card from disagreeing about insets.
  OverlayRect ContentRect(int clientW, int clientH) const {
    const OverlayRect frame = Frame(clientW, clientH);
    OverlayRect content;
    content.left = frame.left + kPadding;
    content.top = frame.top + kPadding + kTitleStrip;
    content.right = frame.right - kPadding;
    content.bottom = frame.bottom - kPadding;
    return content;
  }

  static const char* KindLabel(prism::PreviewKind kind) {
    switch (kind) {
      case prism::PreviewKind::Image: return "Image";
      case prism::PreviewKind::Text: return "Text";
      case prism::PreviewKind::Pdf: return "PDF";
      case prism::PreviewKind::Media: return "Media";
      case prism::PreviewKind::Directory: return "Folder";
      case prism::PreviewKind::None: return "No preview";
    }
    return "No preview";
  }

  static std::string FormatSize(uint64_t bytes) {
    if (bytes < 1024ull) return std::to_string(bytes) + " B";
    if (bytes < 1024ull * 1024ull) {
      return std::to_string(bytes / 1024ull) + " KiB";
    }
    if (bytes < 1024ull * 1024ull * 1024ull) {
      return std::to_string(bytes / (1024ull * 1024ull)) + " MiB";
    }
    return std::to_string(bytes / (1024ull * 1024ull * 1024ull)) + " GiB";
  }

 private:
  static constexpr int kTitleStrip = 34;

  ArchiveItem item_;
  std::string title_;
  std::vector<OverlayLine> lines_;
  prism::PreviewKind kind_ = prism::PreviewKind::None;
  bool streaming_ = false;
  bool open_ = false;
};

}  // namespace prism::win::preview
