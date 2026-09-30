// What the Setup carries: CoopIII.asi, the launcher and CoopIII.ini.
//
// payload.inc is written by xmake just before this file is compiled, from the
// artifacts the installer target depends on. It is what makes the release one
// exe rather than a folder - there is nothing to unzip and nothing to keep
// together.
#include "installer/core.h"

#include <cstring>

namespace {

struct PayloadFile {
	const char          *name;
	const unsigned char *data;
	size_t               size;
};

#include "payload.inc"

} // namespace

namespace coopiii::installer {

void RegisterPayload() {
	SetPayloadReader([](const std::string &name, std::vector<uint8_t> *out) {
		for (const PayloadFile &file : kPayload) {
			if (name != file.name)
				continue;
			out->assign(file.data, file.data + file.size);
			return true;
		}
		return false;
	});
}

} // namespace coopiii::installer
