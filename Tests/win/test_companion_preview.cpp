// Windows integration test (card C30): the inline-preview companion flyout
// (blueprint ch. 4.1) -- the shell-to-flyout channel and the focus-free window.
//
// Everything below runs WITHOUT a user, without Explorer and without a 7z.dll:
// the IPC layer is two kernel objects, the placement is pure arithmetic and the
// window is created, driven and torn down by this process. The one thing that
// still needs a human is the LOOK of the flyout (see the PR's manual checklist).
//
// What this proves (one section per PRISM_TEST below):
//   1. Packet contract: magic/version/count/path guards, a name that is longer
//      than the field is truncated AND terminated, a foreign layout is refused.
//   2. Placement: right-of-click by default, FLIP at the right edge, clamp at
//      the left/vertical edges, negative-coordinate (left-hand monitor) work
//      areas, oversized windows clipped to the work area.
//   3. IPC round trip THREADS: writer thread -> wake event -> reader thread ->
//      ack, N times, with the reader seeing every packet intact.
//   4. IPC round trip PROCESSES: this binary re-launches ITSELF as the companion
//      (owner+reader); the parent is the shell (writer). The child validates the
//      payload and acks; the parent asserts Acknowledged + child exit 0. That is
//      the real two-process shape, not a simulation of it.
//   5. Degradation, no reader: with no companion running the shell path returns
//      NoReader in well under a frame and NEVER creates the section (so a later
//      companion cannot inherit a stale payload).
//   6. Degradation, wedged reader: a reader that acks 400 ms late cannot hold
//      the caller past its 20 ms budget -- Publish returns AckTimeout on time.
//   7. The real window: WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
//      WS_EX_NOREDIRECTIONBITMAP on the created HWND, WM_MOUSEACTIVATE ->
//      MA_NOACTIVATE, SetWindowPos(SWP_NOACTIVATE) placement inside the work
//      area with the window left inactive, ESC and WA_INACTIVE close it, and the
//      entry list drives hover + wheel scrolling.
//
// SKIP POLICY (D1): creating a window needs a window station; if CreateWindowEx
// fails the suite says so and exits 77, which the suite file registers as
// SKIP_RETURN_CODE -- a labelled skip, never a silent pass.
//
// MUTATION MODE (D3): the same TU is compiled three more times with one rule
// broken each (no flip / no NOACTIVATE / ignore the ack budget). Each mutant is
// registered with WILL_FAIL and NO PASS_REGULAR_EXPRESSION (D26). In mutation
// mode the binary prints which injection is live, so the log shows the injection
// actually happened, and it exits 0 -- deliberately -- when it cannot create a
// window, so a mutant that could not run reports RED instead of a false green.
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "../mini_test.hpp"
#include "CompanionPreviewBridge.hpp"
#include "CompanionWindow.hpp"

namespace {

using namespace prism::win::companion;

constexpr int kSkipReturnCode = 77;

#if defined(PRISM_C30_MUTANT_NO_FLIP) || defined(PRISM_C30_MUTANT_DROP_NOACTIVATE) || \
    defined(PRISM_C30_MUTANT_IGNORE_TIMEOUT)
constexpr bool kMutationMode = true;
#else
constexpr bool kMutationMode = false;
#endif

#if defined(PRISM_C30_MUTANT_NO_FLIP)
constexpr const char* kMutationTag = "NO_FLIP (placement overflow control removed)";
#elif defined(PRISM_C30_MUTANT_DROP_NOACTIVATE)
constexpr const char* kMutationTag = "DROP_NOACTIVATE (window created as an activating popup)";
#elif defined(PRISM_C30_MUTANT_IGNORE_TIMEOUT)
constexpr const char* kMutationTag = "IGNORE_TIMEOUT (ack budget replaced by INFINITE)";
#else
constexpr const char* kMutationTag = "none";
#endif

// Set when the window station refuses to give us a window (base run only: the
// mutation run must not silently skip -- see the file header).
bool g_windowUnavailable = false;

double NowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration<double, std::milli>(clock::now().time_since_epoch()).count();
}

std::string Narrow(const wchar_t* text) {
  if (!text) return {};
  const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (bytes <= 1) return {};
  std::string out(static_cast<size_t>(bytes), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), bytes, nullptr, nullptr);
  out.resize(static_cast<size_t>(bytes - 1));
  return out;
}

std::wstring SelfPath() {
  wchar_t buffer[MAX_PATH * 2]{};
  const DWORD written = ::GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
  if (written == 0 || written >= ARRAYSIZE(buffer)) return {};
  return std::wstring(buffer, written);
}

// The payload both sides of the process test agree on. Deliberately not "hello":
// a name that is clipped, a directory entry and a non-zero cursor, so a packet
// that arrives half-copied cannot pass by accident.
constexpr wchar_t kChildArchive[] = L"C:\\fixtures\\round-trip.7z";
constexpr int kChildCursorX = 640;
constexpr int kChildCursorY = 480;

PreviewPacket ChildPacket() {
  PreviewPacket packet{};
  ResetPacket(packet);
  SetArchivePath(packet, kChildArchive);
  AddEntry(packet, L"alpha.txt", 11, false);
  AddEntry(packet, L"beta", 0, true);
  packet.cursorX = kChildCursorX;
  packet.cursorY = kChildCursorY;
  return packet;
}

bool ChildValidates(const PreviewPacket& packet) {
  bool ok = ValidPacket(packet);
  ok = ok && ArchivePathOf(packet) == kChildArchive;
  ok = ok && packet.entryCount == 2;
  // Bounded accessors, not std::wstring(const wchar_t*): the section is
  // reachable by any process in the session (see the header's trust note).
  ok = ok && EntryName(packet.entries[0]) == L"alpha.txt";
  ok = ok && packet.entries[0].sizeBytes == 11;
  ok = ok && packet.entries[0].kind == static_cast<uint32_t>(PreviewEntryKind::File);
  ok = ok && EntryName(packet.entries[1]) == L"beta";
  ok = ok && packet.entries[1].kind == static_cast<uint32_t>(PreviewEntryKind::Directory);
  ok = ok && packet.cursorX == kChildCursorX && packet.cursorY == kChildCursorY;
  return ok;
}

// Runs as the companion process: create the channel (the reader OWNS it), wait
// for the shell's wake, validate the payload, acknowledge, exit.
int RunChildReader() {
  PreviewChannel channel;
  if (!channel.CreateAsOwner()) {
    std::cout << "C30 CHILD: CreateAsOwner failed err=" << ::GetLastError() << std::endl;
    return 20;
  }
  if (!channel.WaitForWake(10000)) {
    std::cout << "C30 CHILD: no wake within 10 s" << std::endl;
    return 21;
  }
  PreviewPacket packet{};
  if (!channel.Read(packet)) {
    std::cout << "C30 CHILD: packet refused (torn or foreign layout)" << std::endl;
    return 22;
  }
  channel.Acknowledge();
  std::cout << "C30 CHILD READ: entries=" << packet.entryCount
            << " cursor=" << packet.cursorX << "," << packet.cursorY
            << " first=" << Narrow(packet.entries[0].name) << std::endl;
  if (!ChildValidates(packet)) {
    std::cout << "C30 CHILD: payload mismatch" << std::endl;
    return 23;
  }
  std::cout << "C30 CHILD: payload verified, acked" << std::endl;
  return 0;
}

// ---------------------------------------------------------------------------
// 1. Packet contract
// ---------------------------------------------------------------------------
PRISM_TEST("packet contract: layout guards, entry capacity, name truncation") {
  PreviewPacket packet{};
  ResetPacket(packet);
  PRISM_CHECK(packet.magic == kPacketMagic);
  PRISM_CHECK(packet.version == kPacketVersion);
  PRISM_CHECK(!ValidPacket(packet));  // no path yet: nothing to render

  SetArchivePath(packet, kChildArchive);
  PRISM_CHECK(ValidPacket(packet));

  // Capacity: exactly kMaxPreviewEntries fit, the next one is refused instead of
  // silently overwriting the last slot.
  for (uint32_t i = 0; i < kMaxPreviewEntries; ++i) {
    PRISM_CHECK(AddEntry(packet, L"entry", i, (i % 2) == 0));
  }
  PRISM_CHECK(!AddEntry(packet, L"one-too-many", 0, false));
  PRISM_CHECK(packet.entryCount == kMaxPreviewEntries);
  // Alternating kinds round-trip: even slots are the directories we asked for.
  PRISM_CHECK(packet.entries[0].kind == static_cast<uint32_t>(PreviewEntryKind::Directory));
  PRISM_CHECK(packet.entries[1].kind == static_cast<uint32_t>(PreviewEntryKind::File));

  // Truncation is explicit and terminated: a 4x oversized name must not run past
  // the field (the reader memcpy's the whole struct).
  PreviewPacket long_name{};
  ResetPacket(long_name);
  SetArchivePath(long_name, kChildArchive);
  const std::wstring huge(kMaxEntryNameChars * 4, L'z');
  PRISM_CHECK(AddEntry(long_name, huge, 0, false));
  PRISM_CHECK(EntryName(long_name.entries[0]).size() == kMaxEntryNameChars - 1);

  // Hostile input: a squatter in the same session can publish a field WITHOUT a
  // terminator. The bounded reader must stop at the field width instead of
  // walking off the end of shared memory.
  PreviewPacket hostile{};
  ResetPacket(hostile);
  SetArchivePath(hostile, kChildArchive);
  for (uint32_t i = 0; i < kMaxEntryNameChars; ++i) hostile.entries[0].name[i] = L'Q';
  PRISM_CHECK(EntryName(hostile.entries[0]).size() == kMaxEntryNameChars);
  PRISM_CHECK(EntryName(hostile.entries[0]) == std::wstring(kMaxEntryNameChars, L'Q'));

  // A foreign layout (future version) is refused, never rendered as garbage.
  PRISM_CHECK(ValidPacket(long_name));
  long_name.version = kPacketVersion + 1;
  PRISM_CHECK(!ValidPacket(long_name));

  // The shell packet asks the companion to enumerate; it must not pretend it
  // already has entries.
  const PreviewPacket shell = MakeShellPacket(L"C:\\x.7z", POINT{7, 9});
  PRISM_CHECK(shell.entryCount == 0);
  PRISM_CHECK((shell.flags & kFlagNeedsCatalogScan) != 0);
  PRISM_CHECK(shell.cursorX == 7 && shell.cursorY == 9);
  PRISM_CHECK(ValidPacket(shell));
}

// ---------------------------------------------------------------------------
// 2. Placement (pure geometry, no window required)
// ---------------------------------------------------------------------------
PRISM_TEST("placement: right of the click, flips at the right edge, clamps inside") {
  const RECT work{0, 0, 1920, 1040};
  const SIZE size{360, 300};

  // Comfortable middle of the screen: the flyout sits to the RIGHT, gap-sized.
  const FlyoutRect right = ComputeFlyoutRect(POINT{400, 300}, size, work);
  PRISM_CHECK(right.left == 400 + kFlyoutGap);
  PRISM_CHECK(right.right == right.left + 360);
  PRISM_CHECK(right.top == 300);

  // Near the right edge the flyout FLIPS to the left of the click. This is the
  // control the NO_FLIP mutant removes: it fails both assertions below.
  const FlyoutRect flipped = ComputeFlyoutRect(POINT{1800, 300}, size, work);
  PRISM_CHECK(flipped.left < 1800);                 // on the menu's left side now
  PRISM_CHECK(flipped.right <= work.right);         // and still on screen
  PRISM_CHECK(flipped.right == 1800 - kFlyoutGap);  // mirrored across the click

  // A window wider than the work area is clipped first, then clamped to the
  // left edge instead of hanging off it.
  const RECT narrow{0, 0, 300, 800};
  const FlyoutRect clamped = ComputeFlyoutRect(POINT{250, 100}, SIZE{360, 300}, narrow);
  PRISM_CHECK(clamped.left == 0);
  PRISM_CHECK(clamped.right == 300);

  // Left-hand monitor: the work area has negative coordinates and must not be
  // treated as "overflow" just because the numbers are negative.
  const RECT left_monitor{-1920, 0, 0, 1080};
  const FlyoutRect secondary = ComputeFlyoutRect(POINT{-1000, 100}, size, left_monitor);
  PRISM_CHECK(secondary.left == -1000 + kFlyoutGap);
  PRISM_CHECK(secondary.right == -1000 + kFlyoutGap + 360);
  const FlyoutRect secondary_flip = ComputeFlyoutRect(POINT{-20, 100}, size, left_monitor);
  PRISM_CHECK(secondary_flip.right <= left_monitor.right);
  PRISM_CHECK(secondary_flip.left >= left_monitor.left);

  // Vertical: hang from the click, pull up at the bottom, clamp at the top.
  const FlyoutRect bottom = ComputeFlyoutRect(POINT{400, 1000}, size, work);
  PRISM_CHECK(bottom.bottom == work.bottom);
  PRISM_CHECK(bottom.top == work.bottom - 300);
  // A taskbar-parked work area (top edge below the click) clamps DOWN, never up
  // and off-screen.
  const RECT lowered{0, 120, 1920, 1040};
  const FlyoutRect top = ComputeFlyoutRect(POINT{400, 100}, size, lowered);
  PRISM_CHECK(top.top == lowered.top);
}

// ---------------------------------------------------------------------------
// 3. IPC round trip across threads
// ---------------------------------------------------------------------------
PRISM_TEST("IPC: writer thread -> wake event -> reader thread -> ack, 25 packets") {
  PreviewChannel owner;
  PRISM_CHECK(owner.CreateAsOwner());
  PRISM_CHECK(owner.IsOpen());

  PreviewChannel writer;
  PRISM_CHECK(writer.OpenAsWriter());
  PRISM_CHECK(writer.IsOpen());
  PRISM_CHECK(!writer.IsOwner());

  constexpr int kWrites = 25;
  std::atomic<int> received{0};
  std::atomic<int> corrupted{0};
  std::thread reader([&] {
    for (int i = 0; i < kWrites; ++i) {
      if (!owner.WaitForWake(5000)) break;
      PreviewPacket packet{};
      if (!owner.Read(packet)) {
        ++corrupted;
        continue;
      }
      if (packet.cursorX == i && std::wstring(packet.archivePath) == L"C:\\round-trip.7z") {
        ++received;
      }
      owner.Acknowledge();
    }
  });

  int acked = 0;
  for (int i = 0; i < kWrites; ++i) {
    PreviewPacket packet = MakeShellPacket(L"C:\\round-trip.7z", POINT{i, i * 2});
    ResetPacket(packet);  // MakeShellPacket already does this: keep the flags explicit
    SetArchivePath(packet, L"C:\\round-trip.7z");
    packet.cursorX = i;
    packet.cursorY = i * 2;
    AddEntry(packet, L"file-" + std::to_wstring(i), static_cast<uint64_t>(i), false);
    if (writer.Publish(packet, 5000) == PublishResult::Acknowledged) ++acked;
  }
  reader.join();
  writer.Close();
  owner.Close();

  PRISM_CHECK(acked == kWrites);
  PRISM_CHECK(received.load() == kWrites);
  PRISM_CHECK(corrupted.load() == 0);
}

// ---------------------------------------------------------------------------
// 4. IPC round trip across processes (this exe re-launched as the companion)
// ---------------------------------------------------------------------------
PRISM_TEST("IPC: two processes -- the child is the companion, the parent is the shell") {
  const std::wstring self = SelfPath();
  PRISM_CHECK(!self.empty());

  std::wstring command = L"\"" + self + L"\" --c30-child-read";
  std::vector<wchar_t> mutable_line(command.begin(), command.end());
  mutable_line.push_back(L'\0');
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const BOOL created =
      ::CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, TRUE, 0, nullptr,
                       nullptr, &startup, &process);
  PRISM_CHECK(created != FALSE);
  if (!created) return;

  // Wait (bounded) for the child to publish its channel. This is the only place
  // that polls: the shell path never does.
  bool present = false;
  for (int attempt = 0; attempt < 500 && !present; ++attempt) {
    PreviewChannel probe;
    if (probe.OpenAsWriter()) {
      probe.Close();
      present = true;
      break;
    }
    ::Sleep(10);
  }
  PRISM_CHECK(present);

  const PublishResult result = TryPublishPreview(ChildPacket(), /*ackTimeoutMs=*/10000);
  PRISM_CHECK(result == PublishResult::Acknowledged);

  const DWORD waited = ::WaitForSingleObject(process.hProcess, 10000);
  PRISM_CHECK(waited == WAIT_OBJECT_0);
  DWORD child_code = 0xFFFFFFFFu;
  if (waited == WAIT_OBJECT_0) PRISM_CHECK(::GetExitCodeProcess(process.hProcess, &child_code) != FALSE);
  ::CloseHandle(process.hThread);
  ::CloseHandle(process.hProcess);
  // 0 means the child decoded, validated and acknowledged the packet itself.
  PRISM_CHECK(child_code == 0);
}

// ---------------------------------------------------------------------------
// 5. Degradation: no companion at all
// ---------------------------------------------------------------------------
PRISM_TEST("degradation: no companion -> NoReader in microseconds, nothing created") {
  // Precondition, asserted instead of assumed: no channel exists right now.
  //
  // BOUNDED wait added for card C36: under `ctest -j` the child process from the
  // previous case exits a little after its assertions finish, so the named
  // objects outlived the handle we closed. This suite still runs one instance at
  // a time (a RESOURCE_LOCK cannot fix an in-test ordering), so the fix belongs
  // here: wait for the leftovers to go away, then assert.
  for (int attempt = 0; attempt < 300 && CompanionPreviewPresent(); ++attempt) ::Sleep(10);
  PRISM_CHECK(!CompanionPreviewPresent());
  {
    PreviewChannel probe;
    PRISM_CHECK(!probe.OpenAsWriter());
    PRISM_CHECK(!probe.IsOpen());
  }

  const double started = NowMs();
  const PublishResult result =
      TryPublishPreview(MakeShellPacket(L"C:\\nobody-listens.7z", POINT{12, 34}), 0);
  const double elapsed = NowMs() - started;

  PRISM_CHECK(result == PublishResult::NoReader);
  // A frame is 16.7 ms at 60 Hz; the shell must not even spend that. Generous
  // bound on purpose: this asserts "no blocking", not a micro-benchmark.
  PRISM_CHECK(elapsed < 50.0);
  // The failed publish must not have created the section for a future companion
  // to inherit: the shell only ever OPENS the channel.
  PRISM_CHECK(!CompanionPreviewPresent());
  std::cout << "  [degrade] no-reader publish took " << elapsed << " ms" << std::endl;
}

// ---------------------------------------------------------------------------
// 6. Degradation: a reader that never acks in time
// ---------------------------------------------------------------------------
PRISM_TEST("degradation: an unresponsive reader cannot hold the caller past its budget") {
  PreviewChannel owner;
  PRISM_CHECK(owner.CreateAsOwner());

  std::thread late_reader([&] {
    if (!owner.WaitForWake(5000)) return;
    // Reads, but deliberately acks 400 ms later: the caller's 20 ms budget must
    // expire first. (IGNORE_TIMEOUT mutant: the caller blocks the full 400 ms.)
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    owner.Acknowledge();
  });

  const double started = NowMs();
  const PublishResult result =
      TryPublishPreview(MakeShellPacket(L"C:\\slow-reader.7z", POINT{5, 6}), /*ackTimeoutMs=*/20);
  const double elapsed = NowMs() - started;

  late_reader.join();
  owner.Close();

  PRISM_CHECK(result == PublishResult::AckTimeout);  // mutant returns Acknowledged
  PRISM_CHECK(elapsed >= 15.0);                      // it DID honour the budget (not 0)
  PRISM_CHECK(elapsed < 250.0);                      // and never exceeded it (mutant: 400+)
  std::cout << "  [degrade] wedged-reader publish returned " << elapsed << " ms (budget 20)"
            << std::endl;
}

// ---------------------------------------------------------------------------
// 7. The real window: focus policy, placement, close paths, hover/scroll
// ---------------------------------------------------------------------------
PRISM_TEST("flyout window: no-activate flags, MA_NOACTIVATE, placement, ESC, hover, wheel") {
  CompanionWindow window;
  HINSTANCE instance = ::GetModuleHandleW(nullptr);
  const bool registered = window.RegisterClass(instance);
  if (!registered && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    g_windowUnavailable = true;
    std::cout << "  [skip] RegisterClassExW failed err=" << ::GetLastError() << std::endl;
    return;
  }
  if (!window.Create(instance, 360, 300)) {
    g_windowUnavailable = true;
    std::cout << "  [skip] CreateWindowExW failed err=" << ::GetLastError() << std::endl;
    return;
  }
  HWND hwnd = window.Hwnd();
  PRISM_CHECK(hwnd != nullptr);

  // (a) The created HWND really carries the focus policy -- read back from the
  // window, not from the constant, so a Create() that dropped a flag is caught.
  const LONG_PTR ex_style = ::GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  const DWORD policy = CompanionWindow::ExStylePolicy();
  std::cout << "  [style] ex=0x" << std::hex << ex_style << std::dec
            << " policy=0x" << std::hex << policy << std::dec << std::endl;
  PRISM_CHECK((policy & WS_EX_NOACTIVATE) != 0);  // mutant: the policy itself is wrong
  PRISM_CHECK((ex_style & WS_EX_NOACTIVATE) != 0);
  PRISM_CHECK((ex_style & WS_EX_TOOLWINDOW) != 0);
  PRISM_CHECK((ex_style & WS_EX_NOREDIRECTIONBITMAP) != 0);

  // (b) A click into the flyout answers MA_NOACTIVATE: Explorer keeps focus.
  const LRESULT activate = ::SendMessageW(hwnd, WM_MOUSEACTIVATE,
                                          reinterpret_cast<WPARAM>(hwnd),
                                          MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN));
  PRISM_CHECK(activate == MA_NOACTIVATE);  // mutant: MA_ACTIVATE

  // (c) Placement: show in the flipped position for a right-edge click and prove
  // the HWND actually moved there, stayed inside the work area, and that the
  // window did not become active (SWP_NOACTIVATE / SW_SHOWNOACTIVATE).
  const RECT work{0, 0, 1920, 1040};
  const HWND foreground_before = ::GetForegroundWindow();
  const POINT cursor{1800, 500};
  window.PlaceForCursor(cursor, work);
  RECT actual{};
  PRISM_CHECK(::GetWindowRect(hwnd, &actual) != FALSE);
  const FlyoutRect planned = window.Placement();
  PRISM_CHECK(actual.left == planned.left && actual.top == planned.top);
  PRISM_CHECK(actual.right - actual.left == 360);
  PRISM_CHECK(actual.right <= work.right);
  PRISM_CHECK(actual.left >= work.left);
  PRISM_CHECK(actual.left < cursor.x);  // flipped to the menu's left
  PRISM_CHECK(window.IsVisible());
  PRISM_CHECK(::GetActiveWindow() != hwnd);
  PRISM_CHECK(::GetForegroundWindow() == foreground_before);

  // (d) The payload drives the list model: three entries, then hover + wheel.
  PreviewPacket packet = MakeShellPacket(L"C:\\preview.7z", cursor);
  SetArchivePath(packet, L"C:\\preview.7z");
  AddEntry(packet, L"one.txt", 1, false);
  AddEntry(packet, L"two", 0, true);
  AddEntry(packet, L"three.bin", 3, false);
  PRISM_CHECK(window.ApplyPreview(packet));
  PRISM_CHECK(window.EntryCount() == 3);
  PRISM_CHECK(window.ArchivePath() == L"C:\\preview.7z");

  // A packet with the wrong magic is refused and the list is left untouched.
  PreviewPacket foreign = packet;
  foreign.magic ^= 0xFFFFFFFFu;
  PRISM_CHECK(!window.ApplyPreview(foreign));
  PRISM_CHECK(window.EntryCount() == 3);

  // Hover: row 0 starts right below the header (sent as a real mouse message).
  ::SendMessageW(hwnd, WM_MOUSEMOVE, 0,
                 MAKELPARAM(10, CompanionWindow::kHeaderHeight + 1));
  PRISM_CHECK(window.HoveredIndex() == 0);
  ::SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(10, 2));
  PRISM_CHECK(window.HoveredIndex() == -1);  // the header is not a row

  // Wheel: one notch scrolls one entry and hit-testing follows the offset.
  ::SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
  PRISM_CHECK(window.ScrollOffset() == 1);
  ::SendMessageW(hwnd, WM_MOUSEMOVE, 0,
                 MAKELPARAM(10, CompanionWindow::kHeaderHeight + 1));
  PRISM_CHECK(window.HoveredIndex() == 1);
  // The list cannot be scrolled past its last entry.
  for (int i = 0; i < 10; ++i) {
    ::SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)), 0);
  }
  PRISM_CHECK(window.ScrollOffset() == 2);

  // (e) ESC closes the peek.
  PRISM_CHECK(window.IsVisible());
  ::SendMessageW(hwnd, WM_KEYDOWN, VK_ESCAPE, 0);
  PRISM_CHECK(!window.IsVisible());

  // (f) Losing activation closes it too (WA_INACTIVE is the shell's "you are no
  // longer the menu's flyout" signal).
  window.PlaceForCursor(cursor, work);
  PRISM_CHECK(window.IsVisible());
  ::SendMessageW(hwnd, WM_ACTIVATE, MAKEWPARAM(WA_INACTIVE, 0), 0);
  PRISM_CHECK(!window.IsVisible());

  ::DestroyWindow(hwnd);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--c30-child-read") return RunChildReader();

  if (kMutationMode) {
    // D3 evidence in the log: the injection is COMPILED IN, not just requested.
    std::cout << "C30 MUTATION ACTIVE: " << kMutationTag << std::endl;
  }
  const int rc = prism::test::RunAll(argc, argv);
  if (rc != 0) return 1;
  if (g_windowUnavailable) {
    if (kMutationMode) {
      // A mutant that could not run must not look like a caught mutation: exit 0
      // so WILL_FAIL reports RED and the environment problem is visible.
      std::cout << "C30 MUTANT COULD NOT RUN (no window station) -- reporting RED on purpose"
                << std::endl;
      return 0;
    }
    std::cout << "C30 SKIP: no window station for the flyout window (labelled skip)" << std::endl;
    return kSkipReturnCode;
  }
  std::cout << "ALL COMPANION-PREVIEW CHECKS PASSED (" << prism::test::CheckCount() << " checks)"
            << std::endl;
  return 0;
}
