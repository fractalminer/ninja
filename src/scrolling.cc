// Author: dsicilia
#include "scrolling.h"

#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <regex>

using namespace std;

e_reformat_mode GetReformatMode() {
  static e_reformat_mode mode = [] {
    char const* mode = ::getenv("DSICILIA_NINJA_REFORMAT_MODE");
    if (mode == nullptr)
      return e_reformat_mode::none;
    if (::strcmp(mode, "pretty") == 0)
      return e_reformat_mode::pretty;
    return e_reformat_mode::none;
  }();
  return mode;
}

e_status_print_mode GetStatusPrintMode() {
  static e_status_print_mode mode = [] {
    char const* mode = ::getenv("DSICILIA_NINJA_STATUS_PRINT_MODE");
    if (mode == nullptr)
      return e_status_print_mode::singleline;
    if (::strcmp(mode, "multiline") == 0)
      return e_status_print_mode::multiline;
    if (::strcmp(mode, "scrolling") == 0)
      return e_status_print_mode::scrolling;
    return e_status_print_mode::singleline;
  }();
  return mode;
}

int TerminalColumns(int def) {
  winsize size;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_col)
    return size.ws_col;
  return def;
}

int TerminalRows(int def) {
  winsize size;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_row)
    return size.ws_row;
  return def;
}

namespace {

struct Replacer {
  string initial_;
  string input_;
  string final_;

  string result() const { return initial_ + input_ + final_; }

  Replacer(string input) : input_(std::move(input)) {
    if (auto start = input_.find("] "); start != string::npos) {
      start += 2;
      initial_ = input_.substr(0, start);
      input_ = input_.substr(start);
    }
  }

  template <typename Fn>
  void prefix_and_remainder(const string_view prefix, Fn&& on_value) {
    if (input_.find(prefix) != 0)
      return;
    input_ = on_value(input_.substr(prefix.size()));
  }

  template <typename Fn>
  void prefix_then_word(const string_view prefix, Fn&& on_value) {
    if (input_.find(prefix) != 0)
      return;
    input_ = input_.substr(prefix.size());
    const auto space = input_.find(" ");
    if (space == string::npos)
      return;
    input_ = input_.substr(0, space);
    input_ = on_value(input_);
  }

  void replace(const string_view from, const string_view to) {
    size_t start = {};
    while ((start = input_.find(from)) != string::npos)
      input_.replace(start, from.size(), to);
  }
};

}  // namespace

string CustomFormat(string const& input) {
  Replacer r(input);
  r.prefix_and_remainder("building rds definition ", [&](const string& _1) {
    return "\u001b[36mbuilding rds script\u001b[0m \u001b[34m"s + _1 +
           "\u001b[0m";
  });
  r.prefix_and_remainder("Building CXX object ", [&](const string& _1) {
    return "\u001b[32mbuilding c++ object \u001b[34m" + _1 + "\u001b[0m";
  });
  r.prefix_and_remainder("Building C object ", [&](const string& _1) {
    return "\u001b[32mbuilding c   object \u001b[34m" + _1 + "\u001b[0m";
  });
  r.prefix_and_remainder("Linking CXX static library ", [&](const string& _1) {
    return "\u001b[33;1mlinking: c++ static library \u001b[34;1m" + _1 +
           "\u001b[0m";
  });
  r.prefix_and_remainder("Linking CXX executable ", [&](const string& _1) {
    return "\u001b[33;1mlinking: c++ binary \u001b[34;1m" + _1 + "\u001b[0m";
  });
  r.prefix_and_remainder("Linking C static library ", [&](const string& _1) {
    return "\u001b[33;1mlinking: c   static library \u001b[34;1m" + _1 +
           "\u001b[0m";
  });
  r.prefix_and_remainder("Linking C executable ", [&](const string& _1) {
    return "\u001b[33;1mlinking: c   binary \u001b[34;1m" + _1 + "\u001b[0m";
  });
  r.prefix_then_word("Rendering midi/", [&](const string& _1) {
    return "\u001b[36mrendering midi file \u001b[0m\u001b[34m" + _1 +
           "\u001b[0m";
  });

  r.replace("CMakeFiles/", "");
  r.replace(".cpp.o", ".cpp");
  r.replace(".mid", "");

  string res = r.result();

  // Below are the cases where we need to use regexes. These
  // should be used sparingly because they are slow, and thus can
  // put strain on ninja when there are many active edges being
  // rendered. When they are used we should be sure to compile
  // them only once.

  // foo/xyz.dir/bar --> foo/bar
  {
    static const std::regex rx{ "[^/ ]+\\.dir/" };
    res = regex_replace(res, rx, "");
  }

  // Color the progress numbers e.g. [37/120].
  // NOTE: disabled; we don't really use this anymore.
#if 0
  {
    static const std::regex rx{ "\\[([ 0-9]+)/([ 0-9]+)\\]" };
    res = regex_replace(res, rx,
                        "[\u001b[37;1m$1\u001b[0m/\u001b[37m$2\u001b[0m]");
  }
#endif

  return res;
}
