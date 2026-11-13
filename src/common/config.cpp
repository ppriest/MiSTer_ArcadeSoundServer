// SPDX-License-Identifier: GPL-3.0-or-later
#include "config.h"

#include <fstream>

namespace arcsnd {

namespace {
std::string trim(const std::string &s)
{
	const size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
}  // namespace

std::string ini_get(const char *section, const char *key, const char *def)
{
	std::ifstream in(std::string(ARCSND_DIR) + "/arcsnd.ini");
	std::string line, sec;
	while (std::getline(in, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == ';' || line[0] == '#') continue;
		if (line[0] == '[') { sec = trim(line.substr(1, line.find(']') - 1)); continue; }
		const size_t eq = line.find('=');
		if (eq != std::string::npos && sec == section && trim(line.substr(0, eq)) == key)
			return trim(line.substr(eq + 1));
	}
	return def;
}

std::string ini_path(const std::string &p)
{
	return p.empty() || p[0] == '/' ? p : std::string(ARCSND_DIR) + "/" + p;
}

}  // namespace arcsnd
