// Author: dsicilia
#ifndef NINJA_SCROLLING_H_
#define NINJA_SCROLLING_H_

#include <string>

enum class e_reformat_mode { none, pretty };

enum class e_status_print_mode { singleline, multiline, scrolling };

e_status_print_mode GetStatusPrintMode();

e_reformat_mode GetReformatMode();

// Get number of columns/rows in terminal, or return specified
// default value if not available.
int TerminalColumns(int def);
int TerminalRows(int def);

std::string CustomFormat(std::string const& input);

#endif  // NINJA_SCROLLING_H_
