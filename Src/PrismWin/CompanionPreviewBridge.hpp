// PrismArchive -- PrismWin
// IPC contract for the inline-preview companion window (blueprint ch. 4.1):
// the Explorer shell extension writes the preview payload, the non-activating
// CompanionWindow reads it and renders it next to the context menu.
//
// Shape (all kernel objects live in the caller's session namespace, hence the
// "Local\" prefix -- a named object without it would need cross-session access
// rights the shell extension does not have):
//   * "Local\PrismLiquidPreviewSection"  fixed-layout shared section (this file),
//   * "Local\PrismLiquidPreviewWakeEvent" auto-reset: writer -> reader,
//   * "Local\PrismLiquidPreviewAckEvent"  auto-reset: reader -> writer (bounded).
//
// Ownership rule that makes degradation free: the COMPANION OWNS the section and
// both events; the shell only ever OPENS an existing section. When the companion
// is not running there is no section, OpenFileMappingW fails in microseconds and
// the shell carries on -- explorer.exe never waits on us, never creates state on
// our behalf and never leaks a section that outlives the click.
//
// Coherency: PreviewPacket::sequence is ODD while a write is in flight and EVEN
// when the packet is complete. The writer bumps it with InterlockedIncrement
// before and after the copy; the reader takes it before and after its own copy
// and rejects the packet when the value moved or is odd. That is what keeps a
// repaint from ever rendering a half-written entry list.
//
// Timeout rule: Publish(ackTimeoutMs == 0) is FIRE-AND-FORGET (no wait at all --
// the shell's only mode). A non-zero timeout is a bounded request/ack used by
// the companion's own hand-off and by the tests; it always returns, whatever the
// reader does (contract D1: a wedged reader must degrade, not hang the shell).
#pragma once

#include <windows.h>

#include <cstdint>
#include <cwchar>
#include <cstring>
#include <string>

namespace prism::win::companion {

// ---------------------------------------------------------------------------
// Kernel object names + packet layout (versioned: a stale section from an older
// build is rejected by ValidPacket, never rendered).
// ---------------------------------------------------------------------------
inline constexpr wchar_t kSectionName[] = L"Local\\PrismLiquidPreviewSection";
inline constexpr wchar_t kWakeEventName[] = L"Local\\PrismLiquidPreviewWakeEvent";
inline constexpr wchar_t kAckEventName[] = L"Local\\PrismLiquidPreviewAckEvent";

inline constexpr uint32_t kPacketVersion = 1;
// 'P','R','C','3' little-endian: a section left over from another product, or
// from a future layout, must fail this check instead of painting garbage.
inline constexpr uint32_t kPacketMagic = 0x33435250u;

inline constexpr uint32_t kMaxPreviewEntries = 12;   // "first N entries" (ch. 4.1)
inline constexpr uint32_t kMaxEntryNameChars = 96;   // truncation is explicit, never silent
inline constexpr uint32_t kMaxArchivePathChars = 512;

// flags bits
inline constexpr uint32_t kFlagNeedsCatalogScan = 1u << 0;  // shell could not enumerate

enum class PreviewEntryKind : uint32_t { File = 0, Directory = 1 };

struct PreviewEntry {
  wchar_t name[kMaxEntryNameChars];
  uint64_t sizeBytes;
  uint32_t kind;  // PreviewEntryKind
  uint32_t reserved;
};

struct PreviewPacket {
  uint32_t magic;
  uint32_t version;
  uint32_t entryCount;
  uint32_t flags;
  int32_t cursorX;      // screen coordinates of the context-menu click
  int32_t cursorY;
  int32_t anchorRight;  // menu rect right edge (0 => derive the anchor from cursorX)
  int32_t anchorBottom;
  wchar_t archivePath[kMaxArchivePathChars];
  PreviewEntry entries[kMaxPreviewEntries];
  // Written LAST by the writer and checked FIRST by the reader. Volatile because
  // two processes reach it through the mapping, not through a lock.
  volatile LONG sequence;
};

static_assert(sizeof(PreviewPacket) < 64 * 1024,
              "the preview section must stay far below a single 64 KiB view");

// ---------------------------------------------------------------------------
// Packet helpers. Truncation is a documented, visible policy: names are clipped
// to kMaxEntryNameChars and the count to kMaxPreviewEntries -- the renderer never
// guesses a length from a missing terminator in shared memory.
// ---------------------------------------------------------------------------
inline void ResetPacket(PreviewPacket& packet) noexcept {
  std::memset(static_cast<void*>(&packet), 0, sizeof(packet));
  packet.magic = kPacketMagic;
  packet.version = kPacketVersion;
}

inline void SetArchivePath(PreviewPacket& packet, const std::wstring& path) noexcept {
  const size_t room = kMaxArchivePathChars - 1;
  const size_t copy = path.size() < room ? path.size() : room;
  if (copy != 0) std::wmemcpy(packet.archivePath, path.c_str(), copy);
  packet.archivePath[copy] = L'\0';
}

inline bool AddEntry(PreviewPacket& packet, const std::wstring& name, uint64_t sizeBytes,
                     bool isDirectory) noexcept {
  if (packet.entryCount >= kMaxPreviewEntries) return false;
  PreviewEntry& entry = packet.entries[packet.entryCount];
  std::memset(&entry, 0, sizeof(entry));
  const size_t room = kMaxEntryNameChars - 1;
  const size_t copy = name.size() < room ? name.size() : room;
  if (copy != 0) std::wmemcpy(entry.name, name.c_str(), copy);
  entry.name[copy] = L'\0';
  entry.sizeBytes = sizeBytes;
  entry.kind = static_cast<uint32_t>(isDirectory ? PreviewEntryKind::Directory
                                                 : PreviewEntryKind::File);
  ++packet.entryCount;
  return true;
}

// Bounded name read: a squatter in the same session can open the section by
// name and fill a name field with 96 non-NUL wide chars, so the renderer must
// never treat it as a C string (std::wstring(const wchar_t*) would read past the
// field). Every consumer goes through this helper instead.
inline std::wstring EntryName(const PreviewEntry& entry) noexcept {
  size_t length = 0;
  while (length < kMaxEntryNameChars && entry.name[length] != L'\0') ++length;
  return std::wstring(entry.name, length);
}

inline std::wstring ArchivePathOf(const PreviewPacket& packet) noexcept {
  size_t length = 0;
  while (length < kMaxArchivePathChars && packet.archivePath[length] != L'\0') ++length;
  return std::wstring(packet.archivePath, length);
}

// Trust boundary: "Local\" sections are reachable by any process in the user's
// session, so the packet is UNTRUSTED input. The layout is fixed-size POD, every
// length is bounded, and no handle or pointer travels through it -- a squatter
// can at worst make the flyout show a list it made up, never corrupt memory.
inline bool ValidPacket(const PreviewPacket& packet) noexcept {
  return packet.magic == kPacketMagic && packet.version == kPacketVersion &&
         packet.entryCount <= kMaxPreviewEntries && packet.archivePath[0] != L'\0';
}

// The shell's own packet: the path it was invoked on plus the click position.
// entries stay empty and kFlagNeedsCatalogScan tells the companion to enumerate
// the first N entries itself (the shell must not open archives on the UI thread).
inline PreviewPacket MakeShellPacket(const std::wstring& archivePath, POINT cursorScreen) noexcept {
  PreviewPacket packet{};
  ResetPacket(packet);
  SetArchivePath(packet, archivePath);
  packet.cursorX = cursorScreen.x;
  packet.cursorY = cursorScreen.y;
  packet.flags = kFlagNeedsCatalogScan;
  return packet;
}

// ---------------------------------------------------------------------------
// Flyout placement (pure, no Win32 call): the menu-anchored rect with the
// right-edge flip blueprint ch. 4.1 asks for.
//
//   1. prefer the RIGHT of the click, gap px away;
//   2. when that would leave the work area, FLIP to the left of the click;
//   3. never place the window outside the work area (oversized window is
//      clipped to the work area first, then the remaining edge is clamped).
// ---------------------------------------------------------------------------
struct FlyoutRect {
  int32_t left = 0;
  int32_t top = 0;
  int32_t right = 0;
  int32_t bottom = 0;
};

inline constexpr int32_t kFlyoutGap = 6;

inline FlyoutRect ComputeFlyoutRect(POINT cursorScreen, SIZE windowSize, RECT workArea,
                                    int32_t gap = kFlyoutGap) noexcept {
  FlyoutRect out{};
  int32_t width = windowSize.cx > 0 ? windowSize.cx : 0;
  int32_t height = windowSize.cy > 0 ? windowSize.cy : 0;
  const int32_t areaWidth = workArea.right - workArea.left;
  const int32_t areaHeight = workArea.bottom - workArea.top;
  if (width > areaWidth) width = areaWidth;
  if (height > areaHeight) height = areaHeight;

  int32_t left = cursorScreen.x + gap;
#ifndef PRISM_C30_MUTANT_NO_FLIP
  // Overflow on the right edge: mirror to the LEFT of the click. This is the
  // line the C30 negative control removes (D3) -- without it the flyout runs
  // off-screen on a taskbar-right / multi-monitor setup.
  if (left + width > workArea.right) left = cursorScreen.x - gap - width;
  // Flipped (or a huge window) can still cross the left edge: clamp, never hide.
  if (left < workArea.left) left = workArea.left;
#else
  // MUTATION (PRISM_C30_MUTANT_NO_FLIP): keep the right-of-click placement even
  // when it leaves the work area.
#endif
  out.left = left;
  out.right = left + width;

  // Vertically the flyout hangs from the click and pulls up at the bottom edge;
  // a click near the top clamps down. Same rule for both: stay inside.
  int32_t top = cursorScreen.y;
  if (top + height > workArea.bottom) top = workArea.bottom - height;
  if (top < workArea.top) top = workArea.top;
  out.top = top;
  out.bottom = top + height;
  return out;
}

// ---------------------------------------------------------------------------
// The channel: one object, two roles (owner = companion, writer = shell).
// Every method is noexcept and bounded -- this header is included by a shell
// extension DLL that runs on explorer.exe's UI thread.
// ---------------------------------------------------------------------------
enum class PublishResult {
  NotOpen,       // channel handle missing / mapping failed
  NoReader,      // no companion: no section to open (the fast degradation path)
  Delivered,     // written and signalled, no ack requested
  Acknowledged,  // reader acked inside the caller's budget
  AckTimeout,    // reader did not ack inside the budget -- still returned on time
};

class PreviewChannel {
 public:
  PreviewChannel() = default;
  ~PreviewChannel() { Close(); }
  PreviewChannel(const PreviewChannel&) = delete;
  PreviewChannel& operator=(const PreviewChannel&) = delete;

  // Companion side: create (or join) the section + wake/ack events.
  bool CreateAsOwner() noexcept {
    Close();
    section_ = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                    static_cast<DWORD>(sizeof(PreviewPacket)), kSectionName);
    if (!section_) return false;
    const bool fresh = ::GetLastError() != ERROR_ALREADY_EXISTS;
    void* view = ::MapViewOfFile(section_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                 sizeof(PreviewPacket));
    if (!view) {
      Close();
      return false;
    }
    view_ = static_cast<PreviewPacket*>(view);
    wake_ = ::CreateEventW(nullptr, FALSE, FALSE, kWakeEventName);
    ack_ = ::CreateEventW(nullptr, FALSE, FALSE, kAckEventName);
    if (!wake_ || !ack_) {
      Close();
      return false;
    }
    // A fresh section may still hold the previous instance's bytes (the kernel
    // zero-fills a NEW section, so this only guards a reused name). Resetting
    // the sequence is what makes "even and stable" mean "complete here".
    if (fresh) {
      ::InterlockedExchange(&view_->sequence, 0);
      ResetPacket(*view_);
    }
    owner_ = true;
    return true;
  }

  // Shell side: OPEN ONLY. Never creates the section (a shell that created it
  // would keep a section alive with no reader and turn "no companion" into a
  // silent half-channel). Fails immediately when the companion is absent.
  bool OpenAsWriter() noexcept {
    Close();
    HANDLE section = ::OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, kSectionName);
    if (!section) return false;
    void* view = ::MapViewOfFile(section, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0,
                                 sizeof(PreviewPacket));
    ::CloseHandle(section);
    if (!view) return false;
    HANDLE wake = ::OpenEventW(EVENT_MODIFY_STATE, FALSE, kWakeEventName);
    if (!wake) {
      // Half-present channel (section without the wake event): refuse instead of
      // writing into a section nobody will ever look at.
      ::UnmapViewOfFile(view);
      return false;
    }
    view_ = static_cast<PreviewPacket*>(view);
    wake_ = wake;
    // The ack event is optional: without it Publish() degrades to fire-and-forget.
    ack_ = ::OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, kAckEventName);
    owner_ = false;
    return true;
  }

  void Close() noexcept {
    if (view_) {
      ::UnmapViewOfFile(static_cast<void*>(view_));
      view_ = nullptr;
    }
    if (wake_) {
      ::CloseHandle(wake_);
      wake_ = nullptr;
    }
    if (ack_) {
      ::CloseHandle(ack_);
      ack_ = nullptr;
    }
    if (section_) {
      ::CloseHandle(section_);
      section_ = nullptr;
    }
    owner_ = false;
  }

  bool IsOpen() const noexcept { return view_ != nullptr && wake_ != nullptr; }
  bool IsOwner() const noexcept { return owner_; }
  uint64_t wakeSignals() const noexcept { return wakeSignals_; }

  // ---- writer ----------------------------------------------------------
  PublishResult Publish(const PreviewPacket& packet, uint32_t ackTimeoutMs = 0) noexcept {
    if (!view_ || !wake_) return PublishResult::NotOpen;
    volatile LONG* sequence = &view_->sequence;
    ::InterlockedIncrement(sequence);  // odd: a write is in flight

    view_->magic = packet.magic;
    view_->version = packet.version;
    view_->entryCount = packet.entryCount;
    view_->flags = packet.flags;
    view_->cursorX = packet.cursorX;
    view_->cursorY = packet.cursorY;
    view_->anchorRight = packet.anchorRight;
    view_->anchorBottom = packet.anchorBottom;
    std::memcpy(view_->archivePath, packet.archivePath, sizeof(packet.archivePath));
    std::memcpy(static_cast<void*>(view_->entries), packet.entries, sizeof(packet.entries));

    ::InterlockedIncrement(sequence);  // even: complete
    if (ack_ && ackTimeoutMs > 0) ::ResetEvent(ack_);  // drop a stale ack from the last publish
    if (!::SetEvent(wake_)) return PublishResult::NotOpen;
    if (ackTimeoutMs == 0 || !ack_) return PublishResult::Delivered;

    DWORD budget = static_cast<DWORD>(ackTimeoutMs);
#ifdef PRISM_C30_MUTANT_IGNORE_TIMEOUT
    // MUTATION: ignore the caller's budget (the shell-side hang this card must
    // never ship). A wedged reader now stalls explorer.exe indefinitely.
    budget = INFINITE;
#endif
    const DWORD waited = ::WaitForSingleObject(ack_, budget);
    return waited == WAIT_OBJECT_0 ? PublishResult::Acknowledged : PublishResult::AckTimeout;
  }

  // ---- reader ----------------------------------------------------------
  // Waits for the writer's signal. Auto-reset event + bounded wait: the
  // companion's message pump uses 0/50 ms windows, tests use the value they mean.
  bool WaitForWake(uint32_t timeoutMs) noexcept {
    if (!wake_) return false;
    if (::WaitForSingleObject(wake_, static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) return false;
    ++wakeSignals_;
    return true;
  }

  // Consistent snapshot. Returns false for a torn packet, a section from another
  // layout, or an empty one -- the renderer keeps what it had instead of
  // painting half a list.
  bool Read(PreviewPacket& out) const noexcept {
    if (!view_) return false;
    volatile LONG* sequence = &view_->sequence;
    const LONG before = ::InterlockedCompareExchange(sequence, 0, 0);
    if ((before & 1) != 0) return false;  // write in flight
    out.magic = view_->magic;
    out.version = view_->version;
    out.entryCount = view_->entryCount;
    out.flags = view_->flags;
    out.cursorX = view_->cursorX;
    out.cursorY = view_->cursorY;
    out.anchorRight = view_->anchorRight;
    out.anchorBottom = view_->anchorBottom;
    std::memcpy(out.archivePath, view_->archivePath, sizeof(out.archivePath));
    std::memcpy(static_cast<void*>(out.entries), view_->entries, sizeof(out.entries));
    const LONG after = ::InterlockedCompareExchange(sequence, 0, 0);
    if (before != after) return false;  // writer moved under us: retry on the next wake
    return ValidPacket(out);
  }

  void Acknowledge() noexcept {
    if (ack_) ::SetEvent(ack_);
  }

 private:
  HANDLE section_ = nullptr;
  HANDLE wake_ = nullptr;
  HANDLE ack_ = nullptr;
  PreviewPacket* view_ = nullptr;
  bool owner_ = false;
  uint64_t wakeSignals_ = 0;
};

// Probe used by the shell BEFORE it decides to launch the app instead: no
// companion => no section => this is a couple of NtOpenSection calls.
inline bool CompanionPreviewPresent() noexcept {
  HANDLE section = ::OpenFileMappingW(FILE_MAP_READ, FALSE, kSectionName);
  if (!section) return false;
  ::CloseHandle(section);
  return true;
}

// The shell's entire IPC surface: open, write, signal, close. Returns NoReader
// (never blocks, never creates a section) when the companion is not running.
inline PublishResult TryPublishPreview(const PreviewPacket& packet,
                                       uint32_t ackTimeoutMs = 0) noexcept {
  PreviewChannel channel;
  if (!channel.OpenAsWriter()) return PublishResult::NoReader;
  return channel.Publish(packet, ackTimeoutMs);
}

}  // namespace prism::win::companion
