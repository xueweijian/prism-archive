// PrismArchive -- PrismWin
// StartupTrace: the QPC segment recorder card C76 uses to break the
// process-to-first-frame path into named stages.
//
// WHY THIS FILE EXISTS (contract R1/R2): "the first frame is 164.5 ms" is a
// single number that says nothing about where the time goes, and every earlier
// attempt to explain it was an argument from reading code. This recorder makes
// the path produce EVIDENCE: one span per named stage, each with its start
// offset and its duration on ONE clock, printed as machine-readable
// C76_BREAKDOWN lines plus a human table.
//
// THE CLOCK. Every span is measured with QueryPerformanceCounter. Spans record
// (start offset since Begin(), duration), so the GAPS between stages -- the glue
// nobody instruments -- are computable exactly: gap_i = start_i - end_(i-1).
// The reporter prints them, and prints the total plus the unaccounted tail, so
// "we measured everything" is never an assumption.
//
// COST WHEN OFF. Disabled is the shipping state: Enabled() is one static bool
// read from the environment once, and every Span becomes a branch. A shipping
// build pays nothing measurable; the recorder is a measurement tool, not a
// product feature.
//
// HOW IT IS SWITCHED ON. PRISM_C76_TRACE=1 in the environment. The bench
// (Tests/win/bench_c76_startup.cpp) launches the REAL PrismArchive.exe with it
// set, so the digits describe the product path -- window class, D2D/DWrite,
// HLSL, DirectComposition, the first WM_PAINT -- not a harness copy of it.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace prism::win::startup {

inline double QpcNowMs() {
  LARGE_INTEGER counter{};
  LARGE_INTEGER frequency{};
  QueryPerformanceCounter(&counter);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(counter.QuadPart) * 1000.0 /
         static_cast<double>(frequency.QuadPart);
}

// Process creation -> now, from GetProcessTimes against GetSystemTimeAsFileTime.
// The FILETIME here is quantised to the ~15.6 ms system tick, so this datum is
// COARSE by construction and is reported as such; it is never used in an
// equality assertion (docs/PERF.md says the same about the C1 number).
inline double MsSinceProcessStart() {
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1.0;
  FILETIME now{};
  GetSystemTimeAsFileTime(&now);
  ULARGE_INTEGER a{}, b{};
  a.LowPart = created.dwLowDateTime;
  a.HighPart = created.dwHighDateTime;
  b.LowPart = now.dwLowDateTime;
  b.HighPart = now.dwHighDateTime;
  if (b.QuadPart <= a.QuadPart) return 0.0;
  return static_cast<double>(b.QuadPart - a.QuadPart) / 10000.0;  // 100 ns -> ms
}

// One measured stage. startMs is relative to Trace::Begin(); ms is the stage
// own duration. Nested spans are NOT created by the instrumented call sites: a
// leaf stage is measured and its parent is left to the gap accounting, so the
// sum of the parts can never exceed the whole.
struct Stage {
  std::string name;
  std::string site;   // the function the span brackets, for the table
  double startMs = 0.0;
  double ms = 0.0;
};

class Trace {
 public:
  static Trace& Instance() {
    static Trace trace;
    return trace;
  }

  // PRISM_C76_TRACE=1. Read once: a recorder that re-read the environment per
  // span would be a cost in the very path it measures.
  bool Enabled() const { return enabled_; }

  // PRISM_C76_TRACE_EXIT=1 additionally closes the window once the first frame
  // has been reported. That is what lets the bench time a REAL process end to
  // end without injecting a WM_CLOSE at a guessed moment: the process itself
  // says "the first frame is done" and then leaves.
  bool ExitAfterFirstFrame() const { return exitAfterFirstFrame_; }

  void Begin() {
    enabled_ = ReadFlag("PRISM_C76_TRACE");
    exitAfterFirstFrame_ = ReadFlag("PRISM_C76_TRACE_EXIT");
    if (!enabled_) return;
    began_ = true;
    t0_ = QpcNowMs();
    const double processToHere = MsSinceProcessStart();
    if (processToHere >= 0.0) processMs_ = processToHere;
    stages_.clear();
    counts_.clear();
    finished_ = false;
  }

  // For a harness: begin on a clock the caller already owns.
  void BeginAt(double originMs, bool exitAfterFirstFrame = false) {
    enabled_ = true;
    exitAfterFirstFrame_ = exitAfterFirstFrame;
    began_ = true;
    t0_ = originMs;
    processMs_ = -1.0;
    stages_.clear();
    counts_.clear();
    finished_ = false;
  }

  double NowMs() const { return QpcNowMs() - t0_; }
  double TotalMs() const { return NowMs(); }

  void Add(const char* name, const char* site, double startOffsetMs, double durationMs) {
    // After the report the table is closed: a measuring build must not grow the
    // stage list once per frame for the rest of the session.
    if (!enabled_ || !began_ || finished_) return;
    Stage stage;
    stage.name = name ? name : "?";
    stage.site = site ? site : "";
    stage.startMs = startOffsetMs;
    stage.ms = durationMs < 0.0 ? 0.0 : durationMs;
    stages_.push_back(std::move(stage));
  }

  // Event counters are the DETERMINISTIC half of the evidence (contract R2: no
  // stopwatch assertions): "how many HLSL compiles ran before the first frame"
  // is a fact a mutant cannot fake and a slow machine cannot change.
  void Count(const char* name) {
    if (!enabled_) return;
    for (auto& entry : counts_) {
      if (entry.first == name) {
        ++entry.second;
        return;
      }
    }
    counts_.emplace_back(name, 1);
  }
  int CountOf(const char* name) const {
    for (const auto& entry : counts_) {
      if (entry.first == name) return entry.second;
    }
    return 0;
  }
  const std::vector<std::pair<std::string, int>>& Counters() const { return counts_; }

  double ProcessMs() const { return processMs_; }
  const std::vector<Stage>& Stages() const { return stages_; }
  bool Finished() const { return finished_; }

  // Prints the table once. `verdict` names the run ("cold", "warm", "inproc") so
  // several launches in one log can be told apart by a parser.
  void Finish(const char* verdict) {
    if (!enabled_ || !began_ || finished_) return;
    finished_ = true;
    const double total = NowMs();
    std::vector<Stage> ordered = stages_;
    // Ordered by start offset, insertion-sorted: the reporter must not reorder
    // two stages that began on the same microsecond (it would fabricate a
    // sequence the code never ran).
    for (size_t i = 1; i < ordered.size(); ++i) {
      for (size_t j = i; j > 0 && ordered[j].startMs < ordered[j - 1].startMs; --j) {
        std::swap(ordered[j], ordered[j - 1]);
      }
    }
    std::fprintf(stderr, "\nC76 STARTUP BREAKDOWN (%s):\n", verdict ? verdict : "?");
    std::fprintf(stderr, "  %-28s %10s %10s %10s  %s\n", "stage", "start_ms", "ms", "gap_ms",
                 "measured around");
    double previousEnd = 0.0;
    double accounted = 0.0;
    for (const Stage& stage : ordered) {
      const bool child = !stage.name.empty() && stage.name[0] == '.';
      const double gap = child ? 0.0 : stage.startMs - previousEnd;
      if (child) {
        std::fprintf(stderr, "  %-28s %10.3f %10.3f %10s  %s\n", stage.name.c_str(),
                     stage.startMs, stage.ms, "(child)", stage.site.c_str());
      } else {
        std::fprintf(stderr, "  %-28s %10.3f %10.3f %10.3f  %s\n", stage.name.c_str(),
                     stage.startMs, stage.ms, gap, stage.site.c_str());
      }
      std::printf("C76_BREAKDOWN {\"run\":\"%s\",\"seg\":\"%s\",\"site\":\"%s\","
                  "\"start_ms\":%.3f,\"ms\":%.3f,\"gap_ms\":%.3f}\n",
                  verdict ? verdict : "?", stage.name.c_str(), stage.site.c_str(),
                  stage.startMs, stage.ms, gap);
      // A stage whose name starts with "." is a CHILD of the stage before it
      // (its time is already inside the parent's). Counting it again would
      // report more work than the process spent, so only top-level stages are
      // summed and only they carry a gap.
      if (!stage.name.empty() && stage.name[0] != '.') {
        previousEnd = stage.startMs + stage.ms;
        accounted += stage.ms;
      }
    }
    const double unaccounted = total - accounted;
    if (processMs_ >= 0.0) {
      std::fprintf(stderr, "  %-28s %10.3f %10.3f %10s  %s\n", "process_start_to_winmain",
                   -processMs_, processMs_, "-", "GetProcessTimes (coarse: 15.6 ms tick)");
      std::printf("C76_BREAKDOWN {\"run\":\"%s\",\"seg\":\"process_start_to_winmain\","
                  "\"site\":\"GetProcessTimes\",\"start_ms\":%.3f,\"ms\":%.3f,\"gap_ms\":0.000}\n",
                  verdict ? verdict : "?", -processMs_, processMs_);
    }
    std::fprintf(stderr, "  %-28s %10s %10.3f %10s  (%zu stages, %.3f ms unaccounted)\n",
                 "TOTAL", "-", total, "-", ordered.size(), unaccounted);
    std::printf("C76_FIRSTFRAME {\"run\":\"%s\",\"total_ms\":%.3f,\"segments\":%zu,"
                "\"accounted_ms\":%.3f,\"unaccounted_ms\":%.3f,\"process_start_to_winmain_ms\":%.3f",
                verdict ? verdict : "?", total, ordered.size(), accounted, unaccounted, processMs_);
    for (const auto& entry : counts_) {
      std::printf(",\"%s\":%d", entry.first.c_str(), entry.second);
    }
    std::printf("}\n");
    std::fflush(stdout);
  }

 private:
  static bool ReadFlag(const char* name) {
    char value[16]{};
    const DWORD length = GetEnvironmentVariableA(name, value, sizeof(value));
    if (length == 0 || length >= sizeof(value)) return false;
    return value[0] == 49 || value[0] == 121 || value[0] == 89 || value[0] == 116 ||
           value[0] == 84;
  }

  bool enabled_ = false;
  bool exitAfterFirstFrame_ = false;
  bool began_ = false;
  bool finished_ = false;
  double t0_ = 0.0;
  double processMs_ = -1.0;
  std::vector<Stage> stages_;
  std::vector<std::pair<std::string, int>> counts_;
};

// RAII span. Disabled -> two branches and nothing else.
class Span {
 public:
  Span(const char* name, const char* site) : name_(name), site_(site) {
    Trace& trace = Trace::Instance();
    active_ = trace.Enabled();
    if (active_) {
      start_ = trace.NowMs();
      t0_ = QpcNowMs();
    }
  }
  ~Span() {
    if (!active_) return;
    const double end = QpcNowMs();
    Trace::Instance().Add(name_, site_, start_, end - t0_);
  }
  Span(const Span&) = delete;
  Span& operator=(const Span&) = delete;

 private:
  const char* name_;
  const char* site_;
  bool active_ = false;
  double start_ = 0.0;
  double t0_ = 0.0;
};

}  // namespace prism::win::startup

// One line at a call site; the name is the stage, the literal next to it is the
// function it brackets (printed in the table last column).
#define PRISM_C76_SPAN2(name, site, line) ::prism::win::startup::Span prism_c76_span_##line(name, site)
#define PRISM_C76_SPAN1(name, site, line) PRISM_C76_SPAN2(name, site, line)
#define PRISM_C76_SPAN(name, site) PRISM_C76_SPAN1(name, site, __LINE__)
