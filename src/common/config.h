// SPDX-License-Identifier: GPL-3.0-or-later
// arcsnd.ini, beside arcsnd: a [section] per system, key=value lines, ';' or '#' comments.
// Read at each call, so an edit applies at the next game.
#pragma once

#include <string>

namespace arcsnd {

constexpr const char *ARCSND_DIR = "/media/fat/games/arcsnd";

// the value, spaces trimmed, or def when the file, section or key is missing
std::string ini_get(const char *section, const char *key, const char *def = "");
// a path from the ini: relative ones are under ARCSND_DIR
std::string ini_path(const std::string &p);

}  // namespace arcsnd
