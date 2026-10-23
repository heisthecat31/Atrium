// A small JSON reader for the Atrium app's state snapshot (one object per line on stdin).
#pragma once
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct Json
{
	enum Type { Null, Bool, Number, String, Array, Object } type = Null;
	bool b = false;
	double n = 0;
	std::string s;
	std::vector<Json> a;
	std::map<std::string, Json> o;

	const Json& operator[](const char* key) const
	{
		static const Json none;
		if (type != Object) return none;
		auto it = o.find(key);
		return it == o.end() ? none : it->second;
	}
	const Json& operator[](size_t i) const
	{
		static const Json none;
		return type == Array && i < a.size() ? a[i] : none;
	}
	size_t size() const { return type == Array ? a.size() : type == Object ? o.size() : 0; }
	std::string str(const char* fallback = "") const { return type == String ? s : fallback; }
	double num(double fallback = 0) const { return type == Number ? n : fallback; }
	bool truthy() const { return type == Bool ? b : type == Number ? n != 0 : type == String ? !s.empty() : type != Null; }

	static Json parse(const std::string& text)
	{
		size_t i = 0;
		Json j;
		parseValue(text, i, j);
		return j;
	}

private:
	static void ws(const std::string& t, size_t& i)
	{
		while (i < t.size() && (t[i] == ' ' || t[i] == '\t' || t[i] == '\n' || t[i] == '\r')) i++;
	}
	static void appendUtf8(std::string& out, unsigned cp)
	{
		if (cp < 0x80) out += (char)cp;
		else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
		else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
		else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
	}
	static bool parseString(const std::string& t, size_t& i, std::string& out)
	{
		if (i >= t.size() || t[i] != '"') return false;
		i++;
		while (i < t.size() && t[i] != '"') {
			char c = t[i++];
			if (c != '\\') { out += c; continue; }
			if (i >= t.size()) break;
			char e = t[i++];
			switch (e) {
			case 'n': out += '\n'; break;
			case 't': out += '\t'; break;
			case 'r': out += '\r'; break;
			case 'b': out += '\b'; break;
			case 'f': out += '\f'; break;
			case 'u': {
				unsigned cp = (unsigned)strtoul(t.substr(i, 4).c_str(), nullptr, 16);
				i += 4;
				if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
					unsigned lo = (unsigned)strtoul(t.substr(i + 2, 4).c_str(), nullptr, 16);
					i += 6;
					cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				}
				appendUtf8(out, cp);
				break;
			}
			default: out += e;
			}
		}
		i++;
		return true;
	}
	static void parseValue(const std::string& t, size_t& i, Json& j)
	{
		ws(t, i);
		if (i >= t.size()) return;
		char c = t[i];
		if (c == '{') {
			j.type = Object;
			i++;
			ws(t, i);
			while (i < t.size() && t[i] != '}') {
				std::string key;
				if (!parseString(t, i, key)) return;
				ws(t, i);
				if (i < t.size() && t[i] == ':') i++;
				parseValue(t, i, j.o[key]);
				ws(t, i);
				if (i < t.size() && t[i] == ',') i++;
				ws(t, i);
			}
			i++;
		} else if (c == '[') {
			j.type = Array;
			i++;
			ws(t, i);
			while (i < t.size() && t[i] != ']') {
				j.a.emplace_back();
				parseValue(t, i, j.a.back());
				ws(t, i);
				if (i < t.size() && t[i] == ',') i++;
				ws(t, i);
			}
			i++;
		} else if (c == '"') {
			j.type = String;
			parseString(t, i, j.s);
		} else if (c == 't' || c == 'f') {
			j.type = Bool;
			j.b = c == 't';
			i += j.b ? 4 : 5;
		} else if (c == 'n') {
			i += 4;
		} else {
			j.type = Number;
			char* end = nullptr;
			j.n = strtod(t.c_str() + i, &end);
			i = end ? (size_t)(end - t.c_str()) : t.size();
		}
	}
};

// JSON-escaped string, quoted.
inline std::string JsonQuote(const std::string& s)
{
	std::string out = "\"";
	for (unsigned char c : s) {
		if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
		else if (c == '\n') out += "\\n";
		else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); out += b; }
		else out += (char)c;
	}
	return out + "\"";
}
