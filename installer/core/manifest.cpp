// The component manifest, and the small JSON reader that parses it.
//
// A reader rather than a dependency: the manifest is one file this project
// writes. It is strict on purpose - a manifest that half-parsed would install
// half an Essential Pack and say it was done - except about keys it does not
// know, which it skips so an older Setup can read a newer file.
#include "installer/core.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>

namespace coopiii::installer {
namespace {

const unsigned char kManifestJson[] = {
#include "components.json.h"
};

struct Value {
	enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
	bool                                   boolean = false;
	double                                 number  = 0;
	std::string                            text;
	std::vector<Value>                     items;
	std::vector<std::pair<std::string, Value>> members;

	const Value *Get(const char *key) const {
		for (const auto &m : members)
			if (m.first == key)
				return &m.second;
		return nullptr;
	}
};

struct Reader {
	const char *p;
	const char *end;
	std::string error;
	int         depth = 0;

	void Space() {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r'))
			++p;
	}

	bool Fail(const std::string &why) {
		if (error.empty())
			error = why;
		return false;
	}

	bool Literal(const char *word) {
		const size_t n = std::strlen(word);
		if (static_cast<size_t>(end - p) < n || std::strncmp(p, word, n) != 0)
			return false;
		p += n;
		return true;
	}

	bool String(std::string *out) {
		if (p >= end || *p != '"')
			return Fail("expected a string");
		++p;
		out->clear();
		while (p < end && *p != '"') {
			if (static_cast<unsigned char>(*p) < 0x20)
				return Fail("a string has a raw control character in it");
			if (*p != '\\') {
				*out += *p++;
				continue;
			}
			if (++p >= end)
				break;
			switch (*p) {
			case 'n':  *out += '\n'; break;
			case 't':  *out += '\t'; break;
			case 'r':  *out += '\r'; break;
			case 'b':  *out += '\b'; break;
			case 'f':  *out += '\f'; break;
			case '"':  *out += '"';  break;
			case '\\': *out += '\\'; break;
			case '/':  *out += '/';  break;
			case 'u': {
				if (end - p < 5)
					return Fail("a \\u escape is cut short");
				char hex[5] = {p[1], p[2], p[3], p[4], 0};
				char *stop  = nullptr;
				const unsigned long cp = std::strtoul(hex, &stop, 16);
				if (stop != hex + 4)
					return Fail("a \\u escape is not hex");
				// UTF-8, BMP only; the manifest has no use for more.
				if (cp < 0x80) {
					*out += static_cast<char>(cp);
				} else if (cp < 0x800) {
					*out += static_cast<char>(0xC0 | (cp >> 6));
					*out += static_cast<char>(0x80 | (cp & 0x3F));
				} else {
					*out += static_cast<char>(0xE0 | (cp >> 12));
					*out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
					*out += static_cast<char>(0x80 | (cp & 0x3F));
				}
				p += 4;
				break;
			}
			default:
				return Fail("unsupported escape in a string");
			}
			++p;
		}
		if (p >= end)
			return Fail("a string never ends");
		++p;
		return true;
	}

	bool Parse(Value *v) {
		Space();
		if (p >= end)
			return Fail("the file ends where a value should be");
		if (++depth > 32)
			return Fail("the file nests too deeply");

		bool ok = true;
		if (*p == '{') {
			++p;
			v->type = Value::Type::Object;
			Space();
			if (p < end && *p == '}') {
				++p;
			} else {
				for (;;) {
					Space();
					std::string key;
					if (!String(&key))
						return false;
					Space();
					if (p >= end || *p != ':')
						return Fail("expected ':' after \"" + key + "\"");
					++p;
					Value child;
					if (!Parse(&child))
						return false;
					v->members.emplace_back(std::move(key), std::move(child));
					Space();
					if (p < end && *p == ',') {
						++p;
						continue;
					}
					if (p < end && *p == '}') {
						++p;
						break;
					}
					return Fail("expected ',' or '}' in an object");
				}
			}
		} else if (*p == '[') {
			++p;
			v->type = Value::Type::Array;
			Space();
			if (p < end && *p == ']') {
				++p;
			} else {
				for (;;) {
					Value child;
					if (!Parse(&child))
						return false;
					v->items.push_back(std::move(child));
					Space();
					if (p < end && *p == ',') {
						++p;
						continue;
					}
					if (p < end && *p == ']') {
						++p;
						break;
					}
					return Fail("expected ',' or ']' in an array");
				}
			}
		} else if (*p == '"') {
			v->type = Value::Type::String;
			ok      = String(&v->text);
		} else if (Literal("true")) {
			v->type    = Value::Type::Bool;
			v->boolean = true;
		} else if (Literal("false")) {
			v->type = Value::Type::Bool;
		} else if (Literal("null")) {
			v->type = Value::Type::Null;
		} else if (*p == '-' || std::isdigit(static_cast<unsigned char>(*p))) {
			char *stop = nullptr;
			v->type    = Value::Type::Number;
			v->number  = std::strtod(p, &stop);
			if (stop == p)
				return Fail("a number is malformed");
			p = stop;
		} else {
			return Fail("unexpected character in the file");
		}
		--depth;
		return ok;
	}
};

// Typed reads over an object, remembering the first thing that was wrong.
struct Fields {
	const Value &object;
	std::string  where;
	std::string *error;
	bool         ok = true;

	bool Bad(const std::string &why) {
		if (ok && error)
			*error = where + ": " + why;
		ok = false;
		return false;
	}

	void Str(const char *key, std::string *out, bool needed = false) {
		const Value *v = object.Get(key);
		if (!v || v->type == Value::Type::Null) {
			if (needed)
				Bad(std::string("\"") + key + "\" is missing");
			return;
		}
		if (v->type != Value::Type::String) {
			Bad(std::string("\"") + key + "\" should be a string");
			return;
		}
		*out = v->text;
	}

	void Bool(const char *key, bool *out) {
		const Value *v = object.Get(key);
		if (!v)
			return;
		if (v->type != Value::Type::Bool) {
			Bad(std::string("\"") + key + "\" should be true or false");
			return;
		}
		*out = v->boolean;
	}

	void Num(const char *key, uint64_t *out) {
		const Value *v = object.Get(key);
		if (!v)
			return;
		if (v->type != Value::Type::Number || v->number < 0) {
			Bad(std::string("\"") + key + "\" should be a number");
			return;
		}
		*out = static_cast<uint64_t>(v->number);
	}

	void Strings(const char *key, std::vector<std::string> *out) {
		const Value *v = object.Get(key);
		if (!v)
			return;
		if (v->type != Value::Type::Array) {
			Bad(std::string("\"") + key + "\" should be a list");
			return;
		}
		for (const Value &item : v->items) {
			if (item.type != Value::Type::String) {
				Bad(std::string("\"") + key + "\" should hold only strings");
				return;
			}
			out->push_back(item.text);
		}
	}

	const std::vector<Value> *Objects(const char *key) {
		const Value *v = object.Get(key);
		if (!v)
			return nullptr;
		if (v->type != Value::Type::Array) {
			Bad(std::string("\"") + key + "\" should be a list");
			return nullptr;
		}
		for (const Value &item : v->items)
			if (item.type != Value::Type::Object) {
				Bad(std::string("\"") + key + "\" should hold only objects");
				return nullptr;
			}
		return &v->items;
	}
};

std::string Upper(std::string s) {
	for (char &c : s)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return s;
}

std::string Lower(std::string s) {
	for (char &c : s)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return s;
}

bool IsHex(const std::string &s, size_t length) {
	if (s.size() != length)
		return false;
	for (char c : s)
		if (!std::isxdigit(static_cast<unsigned char>(c)))
			return false;
	return true;
}

} // namespace

std::vector<std::string> Component::Sources() const {
	std::vector<std::string> out;
	if (!url.empty())
		out.push_back(url);
	for (const std::string &m : mirrors)
		if (!m.empty())
			out.push_back(m);
	return out;
}

bool Manifest::Parse(const std::string &json, std::string *error) {
	builds.clear();
	patches.clear();
	downgrader = Downgrader{};
	components.clear();

	Reader r{json.data(), json.data() + json.size(), {}};
	Value  root;
	if (!r.Parse(&root)) {
		if (error)
			*error = r.error;
		return false;
	}
	r.Space();
	if (r.p != r.end) {
		if (error)
			*error = "there is more after the end of the manifest";
		return false;
	}
	if (root.type != Value::Type::Object) {
		if (error)
			*error = "the manifest is not an object";
		return false;
	}

	Fields top{root, "the manifest", error};

	if (const auto *list = top.Objects("builds")) {
		for (const Value &v : *list) {
			Build  b;
			Fields f{v, "a build", error};
			f.Str("md5", &b.md5, true);
			f.Str("name", &b.name, true);
			f.Num("size", &b.size);
			b.md5 = Upper(b.md5);
			if (f.ok && !IsHex(b.md5, 32))
				f.Bad("\"md5\" is not an MD5");
			if (!f.ok)
				return false;
			builds.push_back(std::move(b));
		}
	}

	if (const auto *list = top.Objects("patches")) {
		for (const Value &v : *list) {
			PatchSource p;
			Fields      f{v, "a patch", error};
			f.Str("from", &p.from, true);
			f.Str("name", &p.name);
			f.Str("file", &p.file, true);
			f.Str("url", &p.url);
			f.Strings("mirrors", &p.mirrors);
			f.Str("sha256", &p.sha256, true);
			p.from   = Upper(p.from);
			p.sha256 = Lower(p.sha256);
			if (f.ok && !IsHex(p.from, 32))
				f.Bad("\"from\" is not an MD5");
			if (f.ok && !IsHex(p.sha256, 64))
				f.Bad("\"sha256\" is not a SHA-256");
			if (!f.ok)
				return false;
			patches.push_back(std::move(p));
		}
	}

	if (const Value *d = root.Get("downgrader")) {
		if (d->type != Value::Type::Object) {
			if (error)
				*error = "\"downgrader\" should be an object";
			return false;
		}
		Fields f{*d, "the downgrader", error};
		f.Str("name", &downgrader.name, true);
		f.Str("url", &downgrader.url);
		f.Str("infoName", &downgrader.infoName);
		f.Str("infoUrl", &downgrader.infoUrl);
		// Pages only. The Setup sends a player to read how to downgrade their
		// own copy, never to fetch somebody's exe.
		for (const std::string *u : {&downgrader.url, &downgrader.infoUrl}) {
			if (!f.ok || u->empty())
				continue;
			std::string lower = Lower(*u);
			const size_t cut  = lower.find_first_of("?#");
			if (cut != std::string::npos)
				lower.resize(cut);
			if (lower.rfind("https://", 0) != 0)
				f.Bad("its links have to be https");
			for (const char *ext : {".exe", ".zip", ".7z", ".rar", ".msi", ".dll", ".asi"})
				if (f.ok && lower.size() > std::strlen(ext) &&
				    lower.compare(lower.size() - std::strlen(ext), std::string::npos, ext) == 0)
					f.Bad("its links have to be pages, not a file to download");
		}
		if (!f.ok)
			return false;
	}

	const auto *list = top.Objects("components");
	if (!top.ok)
		return false;
	if (!list || list->empty()) {
		if (error)
			*error = "the manifest lists no components";
		return false;
	}

	for (const Value &v : *list) {
		Component c;
		Fields    f{v, "a component", error};
		f.Str("id", &c.id, true);
		f.Str("name", &c.name, true);
		if (!c.id.empty())
			f.where = "component \"" + c.id + "\"";
		f.Str("description", &c.description);
		f.Str("version", &c.version);
		f.Str("license", &c.license);
		f.Str("homepage", &c.homepage);
		f.Bool("required", &c.required);
		f.Bool("selected", &c.selected);
		f.Str("source", &c.source, true);
		f.Str("url", &c.url);
		f.Strings("mirrors", &c.mirrors);
		f.Str("sha256", &c.sha256);
		f.Num("size", &c.size);
		f.Str("archive", &c.archive);
		f.Str("saveAs", &c.saveAs);
		f.Strings("files", &c.files);
		f.Str("destination", &c.destination);

		if (const auto *rules = f.Objects("extract")) {
			for (const Value &rv : *rules) {
				ExtractRule rule;
				Fields      rf{rv, f.where + ", an extract rule", error};
				rf.Str("from", &rule.from, true);
				rf.Str("to", &rule.to);
				if (!rf.ok)
					return false;
				c.extract.push_back(std::move(rule));
			}
		}
		if (const auto *defaults = f.Objects("defaults")) {
			for (const Value &dv : *defaults) {
				DefaultFile d;
				Fields      df{dv, f.where + ", a default file", error};
				df.Str("file", &d.file, true);
				df.Str("to", &d.to, true);
				if (!df.ok)
					return false;
				c.defaults.push_back(std::move(d));
			}
		}
		if (!f.ok)
			return false;

		c.sha256 = Lower(c.sha256);
		if (c.required)
			c.selected = true;
		if (c.source != "local" && c.source != "download")
			return f.Bad("\"source\" should be \"local\" or \"download\"");
		if (c.archive != "zip" && c.archive != "file")
			return f.Bad("\"archive\" should be \"zip\" or \"file\"");
		if (c.saveAs.find_first_of("\\/:") != std::string::npos || c.saveAs == "..")
			return f.Bad("\"saveAs\" has to be a plain file name");
		if (!c.sha256.empty() && !IsHex(c.sha256, 64))
			return f.Bad("\"sha256\" is not a SHA-256");
		if (c.source == "download" && c.archive == "zip" && c.extract.empty())
			return f.Bad("a zip needs extract rules");
		if (Find(c.id))
			return f.Bad("the id is used twice");
		components.push_back(std::move(c));
	}
	return true;
}

const Component *Manifest::Find(const std::string &id) const {
	for (const Component &c : components)
		if (c.id == id)
			return &c;
	return nullptr;
}

const Build *Manifest::FindBuild(const std::string &md5) const {
	const std::string key = Upper(md5);
	for (const Build &b : builds)
		if (b.md5 == key)
			return &b;
	return nullptr;
}

const PatchSource *Manifest::FindPatch(const std::string &fromMd5) const {
	const std::string key = Upper(fromMd5);
	for (const PatchSource &p : patches)
		if (p.from == key)
			return &p;
	return nullptr;
}

const Manifest &BuiltInManifest(std::string *error) {
	static std::string     why;
	static const Manifest manifest = [] {
		Manifest m;
		size_t   length = sizeof(kManifestJson);
		while (length > 0 && kManifestJson[length - 1] == 0)
			--length;   // bin2c terminates the array; the JSON ends before it
		const std::string json(reinterpret_cast<const char *>(kManifestJson), length);
		// A Setup whose own manifest does not parse is a broken build, not a
		// runtime condition; it comes back empty and the window says so.
		if (!m.Parse(json, &why))
			m = Manifest{};
		return m;
	}();
	if (error)
		*error = why;
	return manifest;
}

} // namespace coopiii::installer
