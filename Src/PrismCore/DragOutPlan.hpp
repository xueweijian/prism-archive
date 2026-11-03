// PrismArchive -- PrismCore
// The POLICY half of dragging files OUT of an archive (card C67): when may a
// drag materialise anything, where, and what does the receiver see.
//
// The rule the card exists for, in one sentence: a drag that never lands must
// cost nothing. Explorer asks a data object WHAT it offers while the pointer
// merely hovers (DragEnter/DragOver -> QueryGetData/EnumFormatEtc), and asking
// is not a drop. Only GetData(CF_HDROP) -- the bytes a copying target actually
// takes -- may extract, and it may do so ONCE.
//
// The Win32 side (Src/PrismWin/DragOutToShell.hpp) owns the COM objects; this
// header owns the state machine and the temp-directory naming, so both are
// testable without a window station.
//
// NEGATIVE CONTROL (D3): PRISM_C67_MUTANT_EAGER_MATERIALIZE makes Offer()
// extract as well -- the shape that leaves a temp tree behind every time the
// user drags past an Explorer window without letting go.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace prism::dragout {

// Where a drag-out currently is. Only Ready may be handed to a drop target.
enum class Stage : std::uint8_t { Idle, Offered, Extracting, Ready, Failed };

inline std::ostream& operator<<(std::ostream& os, Stage stage) {
  switch (stage) {
    case Stage::Idle: return os << "Idle";
    case Stage::Offered: return os << "Offered";
    case Stage::Extracting: return os << "Extracting";
    case Stage::Ready: return os << "Ready";
    case Stage::Failed: return os << "Failed";
  }
  return os << "?";
}

// Compile-time shape of the negative control (D28: the banner in the test TU is
// written next to this guard, and the macro name is the only thing that counts).
#if defined(PRISM_C67_MUTANT_EAGER_MATERIALIZE)
inline constexpr bool kEagerMaterialize = true;
#else
inline constexpr bool kEagerMaterialize = false;
#endif

// "prism_dragout_p<pid>_<n>" -- unique per PROCESS and per DRAG. A leftover from
// a crashed run can never be picked up by the next one (the C36 lesson), and a
// second drag in the same process gets its own directory.
inline std::wstring TempDirLeaf(std::uint32_t processId, std::uint64_t counter) {
  return std::wstring(L"prism_dragout_p") + std::to_wstring(processId) + L"_" +
         std::to_wstring(counter);
}

// The HDROP list Explorer will receive: absolute, backslash-separated paths.
// A relative path here would make Explorer resolve it against ITS OWN current
// directory, i.e. copy a file nobody selected.
inline bool IsAbsoluteWindowsPath(const std::wstring& path) {
  if (path.size() < 3) return false;
  const wchar_t drive = path[0];
  const bool letter = (drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z');
  return letter && path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

struct PathListCheck {
  std::size_t count = 0;
  std::size_t relative = 0;  // entries that are not absolute
  std::size_t empty = 0;
  bool ok() const { return count > 0 && relative == 0 && empty == 0; }
};

inline PathListCheck CheckPathList(const std::vector<std::wstring>& paths) {
  PathListCheck check;
  check.count = paths.size();
  for (const std::wstring& path : paths) {
    if (path.empty()) {
      ++check.empty;
      continue;
    }
    if (!IsAbsoluteWindowsPath(path)) ++check.relative;
  }
  return check;
}

// The gate a data object consults. "extract" is the caller's real work (open the
// archive, write the selection into the temp directory, answer the paths).
class MaterializeGate {
 public:
  using ExtractFn = std::function<bool()>;

  MaterializeGate() = default;
  explicit MaterializeGate(ExtractFn extract) : extract_(std::move(extract)) {}

  void SetExtractor(ExtractFn extract) { extract_ = std::move(extract); }

  // What a hovering target asks. MUST NOT extract in a clean build.
  bool Offer() {
    ++offers_;
    if (stage_ != Stage::Ready) stage_ = Stage::Offered;
    if (kEagerMaterialize && !ready_) (void)Run();
    return ready_ || static_cast<bool>(extract_);
  }

  // What a copying target asks (GetData(CF_HDROP)). The one door to extraction.
  bool Demand() { return Run(); }

  bool ready() const { return ready_; }
  Stage stage() const { return stage_; }
  std::size_t extractions() const { return extractions_; }
  std::size_t offers() const { return offers_; }
  std::size_t demands() const { return demands_; }

 private:
  bool Run() {
    ++demands_;
    if (ready_) return true;
    // No extractor at all: the gate can never produce a path list, and saying so
    // is the point of the state machine -- a demand that fails must not leave the
    // stage at Offered (the caller would show a drag that can never land).
    if (!extract_) { stage_ = Stage::Failed; return false; }
    if (extracting_) return false;
    extracting_ = true;
    stage_ = Stage::Extracting;
    ++extractions_;
    ready_ = extract_();
    extracting_ = false;
    stage_ = ready_ ? Stage::Ready : Stage::Failed;
    return ready_;
  }

  ExtractFn extract_;
  bool ready_ = false;
  bool extracting_ = false;
  Stage stage_ = Stage::Idle;
  std::size_t extractions_ = 0;
  std::size_t offers_ = 0;
  std::size_t demands_ = 0;
};

}  // namespace prism::dragout
