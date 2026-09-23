// The component manifest, and the small JSON reader that parses it.
//
// A reader rather than a dependency: the manifest is one file with one shape,
// this project writes it, and a JSON library would be a lot of surface for
// twelve objects of strings and booleans. It is strict on purpose - a manifest
// that half-parsed would install half an Essential Pack and say it was done.
#include "installer/core.h"

#include <cctype>
#include <cstring>

namespace coopiii::installer {
namespace {

const unsigned char kManifestJson[] = {
#include "components.json.h"
};

struct Reader {
	const char *p;
	const char *end;
	std::string error;

	void Space() {
		while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ','))
			++p;
	}

	bool Expect(char c) {
		Space();
		if (p < end && *p == c) {
			++p;
			return true;
		}
		if (error.empty())
			error = std::string("expected '") + c + "'";
		return false;
	}

	bool Peek(char c) {
		Space();
		return p < end && *p == c;
	}

	// A JSON string, with the escapes this manifest can contain.
	bool String(std::string *out) {
		Space();
		if (p >= end || *p != '"') {
			if (error.empty())
				error = "expected a string";
			return false;
		}
		++p;
		out->clear();
		while (p < end && *p != '"') {
			if (*p == '\\' && p + 1 < end) {
				++p;
				switch (*p) {
				case 'n':  *out += '\n'; break;
				case 't':  *out += '\t'; break;
				case 'r':  *out += '\r'; break;
				case '"':  *out += '"';  break;
				case '\\': *out += '\\'; break;
				case '/':  *out += '/';  break;
				default:
					error = "unsupported escape in a string";
					return false;
				}
				++p;
			} else {
				*out += *p++;
			}
		}
		if (p >= end) {
			error = "a string never ends";
			return false;
		}
		++p;
		return true;
	}

	// Skips whatever value comes next, so a key this build does not know does
	// not stop the file from loading.
	bool SkipValue() {
		Space();
		if (p >= end) {
			error = "the file ends where a value should be";
			return false;
		}
		if (*p == '"') {
			std::string ignored;
			return String(&ignored);
		}
		if (*p == '{' || *p == '[') {
			const char open  = *p;
			const char close = open == '{' ? '}' : ']';
			int        depth = 0;
			while (p < end) {
				if (*p == '"') {
					std::string ignored;
					if (!String(&ignored))
						return false;
					continue;
				}
				if (*p == open)
					++depth;
				else if (*p == close && --depth == 0) {
					++p;
					return true;
				}
				++p;
			}
			error = "a block never closes";
			return false;
		}
		while (p < end && *p != ',' && *p != '}' && *p != ']' && !std::isspace(*p))
			++p;
		return true;
	}

	bool Bool(bool *out) {
		Space();
		if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) {
			*out = true;
			p += 4;
			return true;
		}
		if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) {
			*out = false;
			p += 5;
			return true;
		}
		error = "expected true or false";
		return false;
	}
};

bool ParseComponent(Reader &r, Component *out) {
	if (!r.Expect('{'))
		return false;

	while (!r.Peek('}')) {
		std::string key;
		if (!r.String(&key) || !r.Expect(':'))
			return false;

		// Each branch is a block, not a `&&`: a key that parsed must not then
		// fall through to SkipValue and eat the token after it.
		if (key == "id") { if (!r.String(&out->id)) return false; }
		else if (key == "name") { if (!r.String(&out->name)) return false; }
		else if (key == "description") { if (!r.String(&out->description)) return false; }
		else if (key == "version") { if (!r.String(&out->version)) return false; }
		else if (key == "source") { if (!r.String(&out->source)) return false; }
		else if (key == "url") { if (!r.String(&out->url)) return false; }
		else if (key == "sha256") { if (!r.String(&out->sha256)) return false; }
		else if (key == "homepage") { if (!r.String(&out->homepage)) return false; }
		else if (key == "destination") { if (!r.String(&out->destination)) return false; }
		else if (key == "required") { if (!r.Bool(&out->required)) return false; }
		else if (key == "files") {
			if (!r.Expect('['))
				return false;
			while (!r.Peek(']')) {
				std::string file;
				if (!r.String(&file))
					return false;
				out->files.push_back(file);
			}
			if (!r.Expect(']'))
				return false;
		}
		else if (!r.SkipValue()) return false;

		r.Space();
		if (r.p >= r.end) {
			r.error = "the file ends inside a component";
			return false;
		}
	}
	return r.Expect('}');
}

} // namespace

bool Manifest::Parse(const std::string &json, std::string *error) {
	components.clear();

	Reader r{json.data(), json.data() + json.size(), {}};
	if (!r.Expect('{')) {
		if (error)
			*error = r.error;
		return false;
	}

	bool sawComponents = false;
	while (!r.Peek('}')) {
		std::string key;
		if (!r.String(&key) || !r.Expect(':')) {
			if (error)
				*error = r.error;
			return false;
		}

		if (key == "components") {
			sawComponents = true;
			if (!r.Expect('[')) {
				if (error)
					*error = r.error;
				return false;
			}
			while (!r.Peek(']')) {
				Component c;
				if (!ParseComponent(r, &c)) {
					if (error)
						*error = r.error;
					return false;
				}
				if (c.id.empty() || c.name.empty()) {
					if (error)
						*error = "a component is missing its id or name";
					return false;
				}
				components.push_back(std::move(c));
			}
			if (!r.Expect(']')) {
				if (error)
					*error = r.error;
				return false;
			}
		} else if (!r.SkipValue()) {
			if (error)
				*error = r.error;
			return false;
		}

		r.Space();
		if (r.p >= r.end) {
			if (error)
				*error = "the file ends inside the manifest";
			return false;
		}
	}

	if (!sawComponents || components.empty()) {
		if (error)
			*error = "the manifest lists no components";
		return false;
	}
	return true;
}

const Component *Manifest::Find(const std::string &id) const {
	for (const Component &c : components)
		if (c.id == id)
			return &c;
	return nullptr;
}

const Manifest &BuiltInManifest() {
	static const Manifest manifest = [] {
		Manifest    m;
		std::string error;
		size_t length = sizeof(kManifestJson);
		while (length > 0 && kManifestJson[length - 1] == 0)
			--length;   // bin2c terminates the array; the JSON ends before it
		const std::string json(reinterpret_cast<const char *>(kManifestJson), length);
		// A Setup whose own manifest does not parse is a broken build, not a
		// runtime condition; it comes back empty and the window says so.
		m.Parse(json, &error);
		return m;
	}();
	return manifest;
}

} // namespace coopiii::installer
