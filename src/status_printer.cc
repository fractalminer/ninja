// Copyright 2016 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "status_printer.h"

#ifdef _WIN32
#include "win32port.h"
#else
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif
#include <cinttypes>
#endif

#include <stdarg.h>
#include <stdlib.h>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "build.h"
#include "debug_flags.h"
#include "exit_status.h"
#include "lexer.h"
#include "metrics.h"
#include "scrolling.h"
#include "util.h"

using namespace std;

namespace {
/// Env that resolves variables in a `--status` format string by asking
/// the StatusPrinter for their current value. `$description` is handled
/// here because its value is per-edge state passed in at print time.
struct StatusFormatEnv : public Env {
  const StatusPrinter* printer;
  const string& description;
  StatusFormatEnv(const StatusPrinter* p, const string& d)
      : printer(p), description(d) {}
  string LookupVariable(StringPiece var) override {
    if (var == "description")
      return description;
    return printer->FormatStatusVariable(var);
  }
};
}  // namespace

Status* Status::factory(const BuildConfig& config) {
  return new StatusPrinter(config);
}

StatusPrinter::StatusPrinter(const BuildConfig& config)
    : config_(config), started_edges_(0), finished_edges_(0), total_edges_(0),
      running_edges_(0), prev_running_edges_(0),
      start_time_millis_(GetTimeMillis()), progress_status_format_(NULL),
      current_rate_(config.parallelism) {
  // Don't do anything fancy in verbose mode.
  if (config_.verbosity != BuildConfig::NORMAL)
    printer_.set_smart_terminal(false);

  if (config.progress_status_format) {
    // --status uses Ninja-style variable expansion ($var / ${var}).
    // Append a newline because Lexer::ReadVarValue terminates on \n.
    string input = string(config.progress_status_format) + "\n";
    Lexer lexer;
    lexer.Start("--status", input);
    status_eval_.reset(new EvalString());
    string err;
    if (!lexer.ReadVarValue(status_eval_.get(), &err))
      Fatal("invalid --status: %s", err.c_str());
    progress_status_format_ = NULL;
  } else {
    progress_status_format_ = getenv("NINJA_STATUS");
    if (!progress_status_format_)
      progress_status_format_ = "[%f/%t] ";
  }
}

void StatusPrinter::EdgeAddedToPlan(const Edge* edge) {
  ++total_edges_;

  // Do we know how long did this edge take last time?
  if (edge->prev_elapsed_time_millis != -1) {
    ++eta_predictable_edges_total_;
    ++eta_predictable_edges_remaining_;
    eta_predictable_cpu_time_total_millis_ += edge->prev_elapsed_time_millis;
    eta_predictable_cpu_time_remaining_millis_ +=
        edge->prev_elapsed_time_millis;
  } else
    ++eta_unpredictable_edges_remaining_;
}

void StatusPrinter::EdgeRemovedFromPlan(const Edge* edge) {
  --total_edges_;

  // Do we know how long did this edge take last time?
  if (edge->prev_elapsed_time_millis != -1) {
    --eta_predictable_edges_total_;
    --eta_predictable_edges_remaining_;
    eta_predictable_cpu_time_total_millis_ -= edge->prev_elapsed_time_millis;
    eta_predictable_cpu_time_remaining_millis_ -=
        edge->prev_elapsed_time_millis;
  } else
    --eta_unpredictable_edges_remaining_;
}

void StatusPrinter::BuildEdgeStarted(const Builder& builder, const Edge* edge,
                                     int64_t start_time_millis) {
  ++started_edges_;
  ++running_edges_;
  time_millis_ = start_time_millis;

  if (edge->use_console() || printer_.is_smart_terminal() ||
      config_.verbosity == BuildConfig::VERBOSE)
    PrintStatus(builder, edge, start_time_millis);

  if (edge->use_console())
    printer_.SetConsoleLocked(true);
}

void StatusPrinter::RecalculateProgressPrediction() {
  time_predicted_percentage_ = 0.0;

  // Sometimes, the previous and actual times may be wildly different.
  // For example, the previous build may have been fully recovered from ccache,
  // so it was blazing fast, while the new build no longer gets hits from ccache
  // for whatever reason, so it actually compiles code, which takes much longer.
  // We should detect such cases, and avoid using "wrong" previous times.

  // Note that we will only use the previous times if there are edges with
  // previous time knowledge remaining.
  bool use_previous_times = eta_predictable_edges_remaining_ &&
                            eta_predictable_cpu_time_remaining_millis_;

  // Iff we have sufficient statistical information for the current run,
  // that is, if we have took at least 15 sec AND finished at least 5% of edges,
  // we can check whether our performance so far matches the previous one.
  if (use_previous_times && total_edges_ && finished_edges_ &&
      (time_millis_ >= 15 * 1e3) &&
      (((double)finished_edges_ / total_edges_) >= 0.05)) {
    // Over the edges we've just run, how long did they take on average?
    double actual_average_cpu_time_millis =
        (double)cpu_time_millis_ / finished_edges_;
    // What is the previous average, for the edges with such knowledge?
    double previous_average_cpu_time_millis =
        (double)eta_predictable_cpu_time_total_millis_ /
        eta_predictable_edges_total_;

    double ratio = std::max(previous_average_cpu_time_millis,
                            actual_average_cpu_time_millis) /
                   std::min(previous_average_cpu_time_millis,
                            actual_average_cpu_time_millis);

    // Let's say that the average times should differ by less than 10x
    use_previous_times = ratio < 10;
  }

  int edges_with_known_runtime = finished_edges_;
  if (use_previous_times)
    edges_with_known_runtime += eta_predictable_edges_remaining_;
  if (edges_with_known_runtime == 0)
    return;

  int edges_with_unknown_runtime = use_previous_times
                                       ? eta_unpredictable_edges_remaining_
                                       : (total_edges_ - finished_edges_);

  // Given the time elapsed on the edges we've just run,
  // and the runtime of the edges for which we know previous runtime,
  // what's the edge's average runtime?
  int64_t edges_known_runtime_total_millis = cpu_time_millis_;
  if (use_previous_times)
    edges_known_runtime_total_millis +=
        eta_predictable_cpu_time_remaining_millis_;

  double average_cpu_time_millis =
      (double)edges_known_runtime_total_millis / edges_with_known_runtime;

  // For the edges for which we do not have the previous runtime,
  // let's assume that their average runtime is the same as for the other edges,
  // and we therefore can predict their remaining runtime.
  double unpredictable_cpu_time_remaining_millis =
      average_cpu_time_millis * edges_with_unknown_runtime;

  // And therefore we can predict the remaining and total runtimes.
  double total_cpu_time_remaining_millis =
      unpredictable_cpu_time_remaining_millis;
  if (use_previous_times)
    total_cpu_time_remaining_millis +=
        eta_predictable_cpu_time_remaining_millis_;
  double total_cpu_time_millis =
      cpu_time_millis_ + total_cpu_time_remaining_millis;
  if (total_cpu_time_millis == 0.0)
    return;

  // After that we can tell how much work we've completed, in time units.
  time_predicted_percentage_ = cpu_time_millis_ / total_cpu_time_millis;
}

void StatusPrinter::BuildEdgeFinished(const Builder& builder, Edge* edge,
                                      int64_t start_time_millis,
                                      int64_t end_time_millis,
                                      ExitStatus exit_code,
                                      const string& output) {
  time_millis_ = end_time_millis;
  ++finished_edges_;

  int64_t elapsed = end_time_millis - start_time_millis;
  cpu_time_millis_ += elapsed;

  // Do we know how long did this edge take last time?
  if (edge->prev_elapsed_time_millis != -1) {
    --eta_predictable_edges_remaining_;
    eta_predictable_cpu_time_remaining_millis_ -=
        edge->prev_elapsed_time_millis;
  } else
    --eta_unpredictable_edges_remaining_;

  if (edge->use_console())
    printer_.SetConsoleLocked(false);

  if (config_.verbosity == BuildConfig::QUIET)
    return;

  // We don't want this in scrolling mode otherwise the number of
  // printed lines stutters each time an edge finishes, as op-
  // posed to the finished edge's line just getting smoothly re-
  // placed by the next edge, which is what we want.
  if (!edge->use_console() &&
      GetStatusPrintMode() != e_status_print_mode::scrolling)
    PrintStatus(builder, edge, end_time_millis);

  --running_edges_;

  // Print the command that is spewing before printing its output.
  if (exit_code != ExitSuccess) {
    string outputs;
    for (vector<Node*>::const_iterator o = edge->outputs_.begin();
         o != edge->outputs_.end(); ++o)
      outputs += (*o)->path() + " ";

#if 0
    string failed = "FAILED: [code=" + std::to_string(exit_code) + "] ";
    if (printer_.supports_color()) {
        printer_.PrintOnNewLine("\x1B[31m" + failed + "\x1B[0m" + outputs + "\n");
    } else {
        printer_.PrintOnNewLine(failed + outputs + "\n");
    }
    printer_.PrintOnNewLine(edge->EvaluateCommand() + "\n");
#endif
  }

  if (!output.empty()) {
#ifdef _WIN32
    // Fix extra CR being added on Windows, writing out CR CR LF (#773)
    fflush(stdout);  // Begin Windows extra CR fix
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    // ninja sets stdout and stderr of subprocesses to a pipe, to be able to
    // check if the output is empty. Some compilers, e.g. clang, check
    // isatty(stderr) to decide if they should print colored output.
    // To make it possible to use colored output with ninja, subprocesses should
    // be run with a flag that forces them to always print color escape codes.
    // To make sure these escape codes don't show up in a file if ninja's output
    // is piped to a file, ninja strips ansi escape codes again if it's not
    // writing to a |smart_terminal_|.
    // (Launching subprocesses in pseudo ttys doesn't work because there are
    // only a few hundred available on some systems, and ninja can launch
    // thousands of parallel compile commands.)
    if (printer_.supports_color() || output.find('\x1b') == std::string::npos) {
      if (GetStatusPrintMode() == e_status_print_mode::scrolling) {
        // Remove any status lines from the display otherwise the
        // subprocess output will get put over top of it and it
        // will look bad.
        ClearScrollingOutput();
        printer_.PrintExtend(output);
      } else {
        printer_.PrintOnNewLine(output);
      }
    } else {
      std::string final_output = StripAnsiEscapeCodes(output);
      printer_.PrintOnNewLine(final_output);
    }

#ifdef _WIN32
    fflush(stdout);
    _setmode(_fileno(stdout), _O_TEXT);  // End Windows extra CR fix
#endif
  }
}

void StatusPrinter::BuildStarted() {
  started_edges_ = 0;
  finished_edges_ = 0;
  running_edges_ = 0;
}

void StatusPrinter::BuildFinished() {
  printer_.SetConsoleLocked(false);
  if (GetStatusPrintMode() == e_status_print_mode::scrolling)
    ClearScrollingOutput();
  printer_.PrintExtend("");
}

void StatusPrinter::OnTick(const Builder& builder) {
  if (GetStatusPrintMode() == e_status_print_mode::scrolling)
    PrintStatusScrolling(builder);
}

string StatusPrinter::FormatProgressStatus(const char* progress_status_format,
                                           int64_t time_millis) const {
  string out;
  char buf[32];

  snprintf(buf, sizeof(buf), "%d", total_edges_);
  int total_edges_length = int(string(buf).size());

  for (const char* s = progress_status_format; *s != '\0'; ++s) {
    if (*s == '%') {
      ++s;
      switch (*s) {
      case '%':
        out.push_back('%');
        break;

        // Started edges.
      case 's': {
        snprintf(buf, sizeof(buf), "%d", started_edges_);
        int padding = total_edges_length - int(string(buf).size());
        for (int i = 0; i < padding; ++i)
          out += ' ';
        out += buf;
        break;
      }

        // Total edges.
      case 't':
        snprintf(buf, sizeof(buf), "%d", total_edges_);
        out += buf;
        break;

        // Running edges.
      case 'r': {
        snprintf(buf, sizeof(buf), "%d", running_edges_);
        out += buf;
        break;
      }

        // Unstarted edges.
      case 'u':
        snprintf(buf, sizeof(buf), "%d", total_edges_ - started_edges_);
        out += buf;
        break;

        // Finished edges.
      case 'f':
        snprintf(buf, sizeof(buf), "%d", finished_edges_);
        out += buf;
        break;

        // Overall finished edges per second.
      case 'o':
        SnprintfRate(finished_edges_ / (time_millis_ / 1e3), buf, "%.1f");
        out += buf;
        break;

        // Current rate, average over the last '-j' jobs.
      case 'c':
        current_rate_.UpdateRate(finished_edges_, time_millis_);
        SnprintfRate(current_rate_.rate(), buf, "%.1f");
        out += buf;
        break;

        // Percentage of edges completed
      case 'p': {
        int percent = 0;
        if (finished_edges_ != 0 && total_edges_ != 0)
          percent = (100 * finished_edges_) / total_edges_;
        snprintf(buf, sizeof(buf), "%3i%%", percent);
        out += buf;
        break;
      }

#define FORMAT_TIME_HMMSS(t)                                                \
  "%" PRId64 ":%02" PRId64 ":%02" PRId64 "", (t) / 3600, ((t) % 3600) / 60, \
      (t) % 60
#define FORMAT_TIME_MMSS(t) "%02" PRId64 ":%02" PRId64 "", (t) / 60, (t) % 60

        // Wall time
      case 'e':  // elapsed, seconds
      case 'w':  // elapsed, human-readable
      case 'E':  // ETA, seconds
      case 'W':  // ETA, human-readable
      {
        double elapsed_sec = time_millis_ / 1e3;
        double eta_sec = -1;  // To be printed as "?".
        if (time_predicted_percentage_ != 0.0) {
          // So, we know that we've spent time_millis_ wall clock,
          // and that is time_predicted_percentage_ percent.
          // How much time will we need to complete 100%?
          double total_wall_time = time_millis_ / time_predicted_percentage_;
          // Naturally, that gives us the time remaining.
          eta_sec = (total_wall_time - time_millis_) / 1e3;
        }

        const bool print_with_hours =
            elapsed_sec >= 60 * 60 || eta_sec >= 60 * 60;

        double sec = -1;
        switch (*s) {
        case 'e':  // elapsed, seconds
        case 'w':  // elapsed, human-readable
          sec = elapsed_sec;
          break;
        case 'E':  // ETA, seconds
        case 'W':  // ETA, human-readable
          sec = eta_sec;
          break;
        }

        if (sec < 0)
          snprintf(buf, sizeof(buf), "?");
        else {
          switch (*s) {
          case 'e':  // elapsed, seconds
          case 'E':  // ETA, seconds
            snprintf(buf, sizeof(buf), "%.3f", sec);
            break;
          case 'w':  // elapsed, human-readable
          case 'W':  // ETA, human-readable
            if (print_with_hours)
              snprintf(buf, sizeof(buf), FORMAT_TIME_HMMSS((int64_t)sec));
            else
              snprintf(buf, sizeof(buf), FORMAT_TIME_MMSS((int64_t)sec));
            break;
          }
        }
        out += buf;
        break;
      }

      // Percentage of time spent out of the predicted time total
      case 'P': {
        snprintf(buf, sizeof(buf), "%3i%%",
                 (int)(100. * time_predicted_percentage_));
        out += buf;
        break;
      }

      default:
        Fatal("unknown placeholder '%%%c' in $NINJA_STATUS", *s);
        return "";
      }
    } else {
      out.push_back(*s);
    }
  }

  return out;
}

string StatusPrinter::FormatStatusVariable(StringPiece name) const {
  char buf[32];

  if (name == "started") {
    snprintf(buf, sizeof(buf), "%d", started_edges_);
    return buf;
  }
  if (name == "total") {
    snprintf(buf, sizeof(buf), "%d", total_edges_);
    return buf;
  }
  if (name == "running") {
    snprintf(buf, sizeof(buf), "%d", running_edges_);
    return buf;
  }
  if (name == "remaining") {
    snprintf(buf, sizeof(buf), "%d", total_edges_ - started_edges_);
    return buf;
  }
  if (name == "finished") {
    snprintf(buf, sizeof(buf), "%d", finished_edges_);
    return buf;
  }
  if (name == "rate") {
    SnprintfRate(finished_edges_ / (time_millis_ / 1e3), buf, "%.1f");
    return buf;
  }
  if (name == "current_rate") {
    current_rate_.UpdateRate(finished_edges_, time_millis_);
    SnprintfRate(current_rate_.rate(), buf, "%.1f");
    return buf;
  }
  if (name == "progress") {
    int percent = 0;
    if (finished_edges_ != 0 && total_edges_ != 0)
      percent = (100 * finished_edges_) / total_edges_;
    snprintf(buf, sizeof(buf), "%3i%%", percent);
    return buf;
  }
  if (name == "predicted_progress") {
    snprintf(buf, sizeof(buf), "%3i%%",
             (int)(100. * time_predicted_percentage_));
    return buf;
  }

  if (name == "elapsed" || name == "elapsed_seconds" ||
      name == "eta" || name == "eta_seconds") {
    double elapsed_sec = time_millis_ / 1e3;
    double eta_sec = -1;
    if (time_predicted_percentage_ != 0.0) {
      double total_wall_time = time_millis_ / time_predicted_percentage_;
      eta_sec = (total_wall_time - time_millis_) / 1e3;
    }
    const bool print_with_hours =
        elapsed_sec >= 60 * 60 || eta_sec >= 60 * 60;
    const bool is_eta = (name == "eta" || name == "eta_seconds");
    double sec = is_eta ? eta_sec : elapsed_sec;
    if (sec < 0)
      return "?";
    if (name == "elapsed_seconds" || name == "eta_seconds") {
      snprintf(buf, sizeof(buf), "%.3f", sec);
    } else if (print_with_hours) {
      snprintf(buf, sizeof(buf), FORMAT_TIME_HMMSS((int64_t)sec));
    } else {
      snprintf(buf, sizeof(buf), FORMAT_TIME_MMSS((int64_t)sec));
    }
    return buf;
  }

  Fatal("unknown variable '%s' in --status format", name.AsString().c_str());
  return "";
}

// Note that in this function we use \n to move the cursor down
// instead of the "move cursor down" escape sequence (which is
// "\x1B[B") because the latter doesn't work when we are on the
// last line of the console.
void StatusPrinter::ClearScrollingOutput(int const lines) {
  printf("\x1B[?25l");  // hide cursor.
  for (size_t i = 0; i < lines; ++i)
    printer_.PrintExtend("\x1B[K\n");  // Clear to end of line then new line
  for (size_t i = 0; i < lines; ++i)
    printer_.PrintExtend("\x1B[A");  // cursor up.
  printf("\x1B[?25h");               // show cursor.
  fflush(stdout);
}

// Clear all scrolling output.
void StatusPrinter::ClearScrollingOutput() {
  int const lines = prev_running_edges_ + 1;  // +1 for progress bar.
  ClearScrollingOutput(lines);
}

// Note that in this function we use \n to move the cursor down
// instead of the "move cursor down" escape sequence (which is
// "\x1B[B") because the latter doesn't work when we are on the
// last line of the console.
void StatusPrinter::PrintStatusScrolling(const Builder& builder) {
  printer_.PrintExtend("\x1B[?25l");  // hide cursor.

  // For some reason the existing running_edges_ count is not al-
  // ways the same as this, so we'll just go with this one be-
  // cause it is closer to the source of truth.
  const int running_edges = builder.running_edges_map().size();

  // If we are running a single executable and it is running in
  // the "console" pool then that likely means we are running our
  // final target binary, e.g. a unit test binary or some other
  // executable after all other intermediate build steps have
  // completed. Since these final executables will generally
  // write to the console, we don't want to render the progress
  // bar or the command description in that scenario, so just
  // clear the entire thing and return to avoid stepping on the
  // output of the executable.
  //
  // The reason we check for the console status is to distinguish
  // the command from a pure build scenario where the last com-
  // mand might be a linker step, in which case we'd want to keep
  // the usual status output. The reason that our final executa-
  // bles have use_console=true is because CMake generates ninja
  // rules files where the custom commands have pool=console.
  bool const is_single_console_cmd =
      running_edges == 1 &&
      builder.running_edges_map().begin()->first->use_console();
  if (is_single_console_cmd) {
    ClearScrollingOutput();
    prev_running_edges_ = 1;
    return;
  }

  float percent = float(started_edges_) / total_edges_;
  percent = (percent > 1.0) ? 1.0 : percent;
  int screen_columns = TerminalColumns(/*def=*/80);
  int progress_columns = int(percent * screen_columns);
  printer_.PrintExtend("\u001b[38;5;244m");
  for (int i = 0; i < screen_columns; ++i) {
    if (i == 0)
      printer_.PrintExtend("[");
    else if (i < progress_columns && i < screen_columns - 1)
      printer_.PrintExtend("─");
    // printer_.PrintExtend("=");
    else if (i == progress_columns && i < screen_columns - 1)
      printer_.PrintExtend("▶");
    else if (i < screen_columns - 1)
      printer_.PrintExtend(" ");
    else if (i == screen_columns - 1)
      printer_.PrintExtend("]");
  }
  printer_.PrintExtend("\r[ ");
  printer_.PrintExtend("\033[0m");  // normal
  printer_.PrintExtend("\033[1m");  // bold
  printer_.PrintExtend(std::to_string(int(percent * 100.0)));
  printer_.PrintExtend("%\033[0m: \r");  // normal
  printer_.PrintExtend("\n");

  int now = (int)(GetTimeMillis() - start_time_millis_);

  int const emit_rows =
      std::min(std::max(TerminalRows(/*def=*/80) - 4, 4), int(running_edges));
  int const overflow = running_edges - emit_rows;
  bool const has_overflow = overflow > 0;

  int lines_emitted = 0;
  const auto& edges_map = builder.running_edges_map();
  std::vector<std::pair<const Edge*, int64_t>> sorted_edges(edges_map.begin(),
                                                            edges_map.end());
  std::sort(sorted_edges.begin(), sorted_edges.end(),
            [](const auto& l, const auto& r) { return l.second < r.second; });
  for (auto const& p : sorted_edges) {
    if (lines_emitted >= emit_rows)
      break;
    ++lines_emitted;
    Edge const* edge = p.first;
    int time_start = p.second;

    bool force_full_command = config_.verbosity == BuildConfig::VERBOSE;
    string to_print = edge->GetBinding("description");
    if (force_full_command)
      to_print = edge->GetBinding("command");
    if (!to_print.empty()) {
      // This will print the numerical status, e.g. [34/120] on each line.
      // to_print = FormatProgressStatus(progress_status_format_, kEdgeStarted)
      //   + to_print;
      const auto mode =
          force_full_command ? LinePrinter::FULL : LinePrinter::ELIDE;
      if (mode != LinePrinter::FULL) {
        int delta_secs = (now - time_start) / 1000;
        std::string running_time =
            std::string(" (") + std::to_string(delta_secs) + "s)";
        to_print += "\u001b[38;5;244m";
        to_print += running_time;
        to_print += "\033[0m";  // normal
      }
      // NOTE: it is importnat to put the entire output (in-
      // cluding all escape sequences) that we want to emit into
      // one string and emit it with Print that way it can have a
      // full view over its length if it needs to elide it due to
      // the terminal width (if it does elide it then it does it
      // in an ansi-code aware way).
      printer_.Print(to_print, mode);
    }
    printer_.PrintExtend("\x1B[K");  // Clear to end of line.
    printer_.PrintExtend("\n");
  }
  if (has_overflow) {
    ++lines_emitted;
    printer_.PrintExtend("\x1B[K");  // Clear to end of line.
    // Use Print here so that it can elide the length if needed.
    printer_.Print(std::string("\u001b[38;5;244m  (") +
                       std::to_string(running_edges) + " total tasks|" +
                       std::to_string(overflow) + " hidden tasks)\033[0m\n",
                   LinePrinter::ELIDE);
  }

  // Check if we need to clear out the additional lines from the
  // last status that had more lines.
  if (prev_running_edges_ > lines_emitted) {
    int const lines = prev_running_edges_ - lines_emitted;
    ClearScrollingOutput(lines);
  }

  // Move cursor back up to the top.
  for (size_t i = 0; i < lines_emitted; ++i)
    printer_.PrintExtend("\r\x1B[A");

  // One for the progress bar.
  printer_.PrintExtend("\r\x1B[A");

  prev_running_edges_ = lines_emitted;
  printer_.PrintExtend("\x1B[?25h");  // show cursor.
  fflush(stdout);
}

void StatusPrinter::PrintStatus(const Builder& builder, const Edge* edge,
                                int64_t time_millis) {
  if (GetStatusPrintMode() == e_status_print_mode::scrolling) {
    PrintStatusScrolling(builder);
    return;
  }
  if (explanations_) {
    explanations_->ExplainEdge(edge);
  }

  if (config_.verbosity == BuildConfig::QUIET
      || config_.verbosity == BuildConfig::NO_STATUS_UPDATE)
    return;

  RecalculateProgressPrediction();

  bool force_full_command = config_.verbosity == BuildConfig::VERBOSE;

  string description = edge->GetBinding("description");
  if (force_full_command)
    description = edge->GetBinding("command");
  if (description.empty())
    return;

  string to_print;
  if (status_eval_) {
    // For `--status`, the description is only shown if the user includes
    // `$description` in the format string. In verbose mode (or when the
    // description is empty) `$description` resolves to the command, matching
    // the NINJA_STATUS / default behaviour.
    StatusFormatEnv env(this, description);
    to_print = status_eval_->Evaluate(&env);
  } else {
    to_print = FormatProgressStatus(progress_status_format_, time_millis)
        + description;
  }

  printer_.Print(to_print,
                 force_full_command ? LinePrinter::FULL : LinePrinter::ELIDE);
}

void StatusPrinter::NewLine() {
  printer_.PrintOnNewLine("");
}

void StatusPrinter::Warning(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Warning(msg, ap);
  va_end(ap);
}

void StatusPrinter::Error(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Error(msg, ap);
  va_end(ap);
}

void StatusPrinter::Info(const char* msg, ...) {
  va_list ap;
  va_start(ap, msg);
  ::Info(msg, ap);
  va_end(ap);
}
