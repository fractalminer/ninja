// Copyright 2013 Google Inc. All Rights Reserved.
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

#include "line_printer.h"

#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <windows.h>
#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x4
#endif
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <sys/time.h>
#endif

#include <cstring>

#include <regex>

#include "util.h"

using namespace std;

LinePrinter::LinePrinter() : have_blank_line_(true), console_locked_(false) {
  const char* term = getenv("TERM");
#ifndef _WIN32
  smart_terminal_ = isatty(1) && term && string(term) != "dumb";
#else
  // Disable output buffer.  It'd be nice to use line buffering but
  // MSDN says: "For some systems, [_IOLBF] provides line
  // buffering. However, for Win32, the behavior is the same as _IOFBF
  // - Full Buffering."
  if (term && string(term) == "dumb") {
    smart_terminal_ = false;
  } else {
    setvbuf(stdout, NULL, _IONBF, 0);
    console_ = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    smart_terminal_ = GetConsoleScreenBufferInfo(console_, &csbi);
  }
#endif
  supports_color_ = smart_terminal_;
  if (!supports_color_) {
    const char* clicolor_force = getenv("CLICOLOR_FORCE");
    supports_color_ = clicolor_force && string(clicolor_force) != "0";
  }
#ifdef _WIN32
  // Try enabling ANSI escape sequence support on Windows 10 terminals.
  if (supports_color_) {
    DWORD mode;
    if (GetConsoleMode(console_, &mode)) {
      if (!SetConsoleMode(console_, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
        supports_color_ = false;
      }
    }
  }
#endif
}

string CustomFormat(string const& input) {
  auto res = input;
  res = regex_replace(res, std::regex{"building rds definition (.*)"}, "\u001b[36mbuilding rds script\u001b[0m \u001b[34m$1\u001b[0m");
  res = regex_replace(res, std::regex{"Building CXX(.*) ([^ ]+)"}, "\u001b[32mbuilding c++$1 \u001b[34m$2\u001b[0m");
  res = regex_replace(res, std::regex{"Linking CXX static library(.*) ([^ ]+)"}, "\u001b[33;1mlinking: c++ static$1 \u001b[34;1m$2\u001b[0m");
  res = regex_replace(res, std::regex{"Building C(.*) ([^ ]+)"}, "\u001b[32mbuilding c  $1 \u001b[34m$2\u001b[0m");
  res = regex_replace(res, std::regex{"Linking CXX executable(.*) ([^ ]+)"}, "\u001b[33;1mlinking: c++ binary$1 \u001b[34;1m$2\u001b[0m");
  res = regex_replace(res, std::regex{"Linking C static library(.*) ([^ ]+)"}, "\u001b[33;1mlinking: c   static$1 \u001b[34;1m$2\u001b[0m");
  res = regex_replace(res, std::regex{"Linking C(.*) ([^ ]+)"}, "\u001b[33;1mlinking: c  $1 \u001b[34;1m$2\u001b[0m");
  // foo/xyz.dir/bar --> foo/bar
  res = regex_replace(res, std::regex{"[^/ ]+\\.dir/"}, "");
  // foo/CMakeFiles/bar --> foo/bar
  res = regex_replace(res, std::regex{"CMakeFiles/"}, "");
  // foo.cpp.o --> foo.cpp
  res = regex_replace(res, std::regex{"\\.cpp\\.o"}, ".cpp");
  // Color the progress numbers e.g. [37/120].
  res = regex_replace(res, std::regex{"\\[([ 0-9]+)/([ 0-9]+)\\]"}, "[\u001b[37;1m$1\u001b[0m/\u001b[37m$2\u001b[0m]");
  return res;
}

int LinePrinter::TerminalColumns( int def ) {
  winsize size;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_col)
    return size.ws_col;
  return def;
}

int LinePrinter::TerminalRows( int def ) {
  winsize size;
  if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_row)
    return size.ws_row;
  return def;
}

void LinePrinter::Print(string to_print, LineType type) {
  if (GetReformatMode() == e_reformat_mode::pretty) {
    to_print = CustomFormat(to_print);
  }
  if (console_locked_) {
    line_buffer_ = to_print;
    line_type_ = type;
    return;
  }
  if (GetStatusPrintMode() == e_status_print_mode::multiline) {
    printf("%s\n", to_print.c_str());
    return;
  }

  if (smart_terminal_) {
    printf("\r");  // Print over previous line, if any.
    // On Windows, calling a C library function writing to stdout also handles
    // pausing the executable when the "Pause" key or Ctrl-S is pressed.
  }

  if (smart_terminal_ && type == ELIDE) {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(console_, &csbi);

    to_print = ElideMiddle(to_print, static_cast<size_t>(csbi.dwSize.X));
    // We don't want to have the cursor spamming back and forth, so instead of
    // printf use WriteConsoleOutput which updates the contents of the buffer,
    // but doesn't move the cursor position.
    COORD buf_size = { csbi.dwSize.X, 1 };
    COORD zero_zero = { 0, 0 };
    SMALL_RECT target = {
      csbi.dwCursorPosition.X, csbi.dwCursorPosition.Y,
      static_cast<SHORT>(csbi.dwCursorPosition.X + csbi.dwSize.X - 1),
      csbi.dwCursorPosition.Y
    };
    vector<CHAR_INFO> char_data(csbi.dwSize.X);
    for (size_t i = 0; i < static_cast<size_t>(csbi.dwSize.X); ++i) {
      char_data[i].Char.AsciiChar = i < to_print.size() ? to_print[i] : ' ';
      char_data[i].Attributes = csbi.wAttributes;
    }
    WriteConsoleOutput(console_, &char_data[0], buf_size, zero_zero, &target);
#else
    // Limit output to width of the terminal if provided so we don't cause
    // line-wrapping.
    winsize size;
    if ((ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0) && size.ws_col) {
      to_print = ElideMiddle(to_print, size.ws_col);
    }
    printf("%s", to_print.c_str());
    printf("\x1B[K");  // Clear to end of line.
    fflush(stdout);
#endif

    have_blank_line_ = false;
  } else {
    printf("%s\n", to_print.c_str());
  }
}

void LinePrinter::PrintOrBuffer(const char* data, size_t size) {
  if (console_locked_) {
    output_buffer_.append(data, size);
  } else {
    // Avoid printf and C strings, since the actual output might contain null
    // bytes like UTF-16 does (yuck).
    fwrite(data, 1, size, stdout);
  }
}

void LinePrinter::PrintWithoutNewLine(const string& to_print) {
  if (console_locked_ && !line_buffer_.empty()) {
    output_buffer_.append(line_buffer_);
    output_buffer_.append(1, '\n');
    line_buffer_.clear();
  }
  if (!to_print.empty()) {
    PrintOrBuffer(&to_print[0], to_print.size());
  }
  have_blank_line_ = (!to_print.empty() && to_print[0] == '\n') ||
                     (to_print.empty() && have_blank_line_);
}

void LinePrinter::PrintOnNewLine(const string& to_print) {
  if (console_locked_ && !line_buffer_.empty()) {
    output_buffer_.append(line_buffer_);
    output_buffer_.append(1, '\n');
    line_buffer_.clear();
  }
  if (!have_blank_line_) {
    PrintOrBuffer("\n", 1);
  }
  if (!to_print.empty()) {
    PrintOrBuffer(&to_print[0], to_print.size());
  }
  have_blank_line_ = to_print.empty() || *to_print.rbegin() == '\n';
}

e_reformat_mode LinePrinter::GetReformatMode() {
  static e_reformat_mode mode = []{
    char const* mode = getenv("DSICILIA_NINJA_REFORMAT_MODE");
    if( mode == nullptr )
      return e_reformat_mode::none;
    if( strcmp( mode, "pretty" ) == 0 )
      return e_reformat_mode::pretty;
    return e_reformat_mode::none;
  }();
  return mode;
}

e_status_print_mode LinePrinter::GetStatusPrintMode() {
  static e_status_print_mode mode = []{
    char const* mode = getenv("DSICILIA_NINJA_STATUS_PRINT_MODE");
    if( mode == nullptr )
      return e_status_print_mode::singleline;
    if( strcmp( mode, "multiline" ) == 0 )
      return e_status_print_mode::multiline;
    if( strcmp( mode, "scrolling" ) == 0 )
      return e_status_print_mode::scrolling;
    return e_status_print_mode::singleline;
  }();
  return mode;
}

void LinePrinter::SetConsoleLocked(bool locked) {
  if (locked == console_locked_)
    return;

  if (locked) {
    // Don't start a new line when we lock the console (which in
    // practice means that we start to run a custom command). In-
    // stead, just erase the current line to prepare for the cus-
    // tomer command.
    printf("\r\x1B[K\r");  // Clear to end of line.
    fflush(stdout);
  }

  console_locked_ = locked;

  if (!locked) {
    PrintWithoutNewLine(output_buffer_);
    if (!line_buffer_.empty()) {
      Print(line_buffer_, line_type_);
    }
    output_buffer_.clear();
    line_buffer_.clear();
  }
}
