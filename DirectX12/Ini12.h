// 3DMigoto DX12 - ini file loader (portable).
// Keeps line order and allows repeated keys / keyless lines like 3DMigoto does,
// merges repeated sections, and implements [Include] / namespacing so that
// mods dropped in an included folder (include_recursive = Mods) get their own
// namespace for section and variable names.
#pragma once
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace mig12 {

namespace fs = std::filesystem;

inline std::string lower(std::string s)
{
	for (auto &c : s)
		c = (char)tolower((unsigned char)c);
	return s;
}

inline std::string trim(const std::string &s)
{
	size_t b = 0, e = s.size();
	while (b < e && isspace((unsigned char)s[b]))
		b++;
	while (e > b && isspace((unsigned char)s[e - 1]))
		e--;
	return s.substr(b, e - b);
}

struct IniLine {
	std::string key;
	std::string val;
	std::string raw; // whole line (comments stripped) for if/elif/else/endif
	bool has_eq = false;
};

struct IniSection {
	std::string name;  // as written
	std::string lname; // lower case
	std::string ns;    // lower case namespace ("" for the root ini)
	std::string dir;   // directory of the file that defined it
	std::vector<IniLine> lines;
};

inline bool glob_match(const char *pat, const char *str)
{
	for (; *pat; pat++, str++) {
		if (*pat == '*') {
			while (pat[1] == '*')
				pat++;
			if (!pat[1])
				return true;
			for (const char *s = str;; s++) {
				if (glob_match(pat + 1, s))
					return true;
				if (!*s)
					return false;
			}
		}
		if (!*str)
			return false;
		if (*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*str))
			return false;
	}
	return !*str;
}

class IniFile {
public:
	std::vector<IniSection> sections;
	std::string root_dir;
	std::vector<std::string> messages;

	bool Load(const std::string &path)
	{
		sections.clear();
		root_dir = fs::u8path(path).parent_path().u8string();
		if (!ParseFile(path, "", 0)) {
			messages.push_back("Unable to open " + path);
			return false;
		}
		ProcessIncludes();
		return true;
	}

	// For tests: parse text directly
	void LoadText(const std::string &text, const std::string &ns = "", const std::string &dir = "")
	{
		root_dir = dir;
		ParseText(text, ns, dir);
	}

private:
	IniSection *FindOrAdd(const std::string &name, const std::string &ns, const std::string &dir)
	{
		std::string ln = lower(name);
		for (auto &s : sections)
			if (s.lname == ln && s.ns == ns)
				return &s;
		sections.emplace_back();
		IniSection &s = sections.back();
		s.name = name;
		s.lname = ln;
		s.ns = ns;
		s.dir = dir;
		return &s;
	}

	static std::string strip_comment(const std::string &line)
	{
		bool in_quote = false;
		for (size_t i = 0; i < line.size(); i++) {
			if (line[i] == '"')
				in_quote = !in_quote;
			else if (line[i] == ';' && !in_quote)
				return line.substr(0, i);
		}
		return line;
	}

	void ParseText(const std::string &text, const std::string &ns, const std::string &dir)
	{
		std::istringstream in(text);
		std::string raw;
		IniSection *cur = nullptr;
		bool first = true;
		while (std::getline(in, raw)) {
			if (first) {
				first = false;
				if (raw.size() >= 3 && (unsigned char)raw[0] == 0xEF && (unsigned char)raw[1] == 0xBB && (unsigned char)raw[2] == 0xBF)
					raw = raw.substr(3);
			}
			std::string line = trim(strip_comment(raw));
			if (line.empty())
				continue;
			if (line[0] == '[') {
				size_t e = line.find(']');
				if (e == std::string::npos)
					continue;
				cur = FindOrAdd(trim(line.substr(1, e - 1)), ns, dir);
				continue;
			}
			if (!cur)
				continue;
			IniLine l;
			l.raw = line;
			size_t eq = line.find('=');
			if (eq == std::string::npos) {
				l.key = line;
			} else {
				l.key = trim(line.substr(0, eq));
				l.val = trim(line.substr(eq + 1));
				l.has_eq = true;
			}
			cur->lines.push_back(l);
		}
	}

	bool ParseFile(const std::string &path, const std::string &ns, int depth)
	{
		if (depth > 8)
			return false;
		std::ifstream f(fs::u8path(path), std::ios::binary);
		if (!f)
			return false;
		std::stringstream ss;
		ss << f.rdbuf();
		ParseText(ss.str(), ns, fs::u8path(path).parent_path().u8string());
		return true;
	}

	std::string RelNs(const fs::path &p) const
	{
		std::error_code ec;
		fs::path rel = fs::relative(p, fs::u8path(root_dir), ec);
		if (ec)
			rel = p.filename();
		std::string s = lower(rel.generic_u8string());
		std::replace(s.begin(), s.end(), '/', '\\');
		return s;
	}

	void ProcessIncludes()
	{
		std::vector<std::string> includes, recursive, excludes;
		for (auto &s : sections) {
			if (s.lname != "include" || !s.ns.empty())
				continue;
			for (auto &l : s.lines) {
				std::string k = lower(l.key);
				if (k == "include")
					includes.push_back(l.val);
				else if (k == "include_recursive")
					recursive.push_back(l.val);
				else if (k == "exclude_recursive")
					excludes.push_back(l.val);
			}
		}
		if (excludes.empty())
			excludes.push_back("DISABLED*");

		for (auto &inc : includes) {
			fs::path p = fs::u8path(root_dir) / fs::u8path(inc);
			if (!ParseFile(p.u8string(), RelNs(p), 1))
				messages.push_back("Include not found: " + inc);
		}

		for (auto &rec : recursive) {
			fs::path base = fs::u8path(root_dir) / fs::u8path(rec);
			std::error_code ec;
			if (!fs::exists(base, ec)) {
				messages.push_back("include_recursive path not found: " + rec);
				continue;
			}
			if (fs::is_regular_file(base, ec)) {
				ParseFile(base.u8string(), RelNs(base), 1);
				continue;
			}
			std::vector<fs::path> files;
			fs::recursive_directory_iterator it(base, fs::directory_options::skip_permission_denied, ec), end;
			for (; !ec && it != end; it.increment(ec)) {
				const fs::path &p = it->path();
				bool excluded = false;
				// test every path component below the include root against the exclusions
				fs::path relp = fs::relative(p, base, ec);
				for (auto &part : relp) {
					for (auto &pat : excludes)
						if (glob_match(pat.c_str(), part.u8string().c_str()))
							excluded = true;
				}
				if (excluded) {
					if (it->is_directory(ec))
						it.disable_recursion_pending();
					continue;
				}
				if (!it->is_regular_file(ec))
					continue;
				if (lower(p.extension().u8string()) != ".ini")
					continue;
				if (lower(p.filename().u8string()) == "desktop.ini")
					continue;
				files.push_back(p);
			}
			std::sort(files.begin(), files.end());
			for (auto &p : files)
				ParseFile(p.u8string(), RelNs(p), 1);
		}
	}
};

} // namespace mig12
