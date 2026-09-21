#include "intro.h"

namespace agentpad {

bool ShouldSkipIntroFrom(uint32_t state) {
	// GS_LOGO_MPEG and GS_INTRO_MPEG, nothing else. The case for these two and
	// against every other value is the long comment in intro.h. Keeping it as
	// one expression here means the comment can't drift from the code without
	// this line changing too.
	return state == 2u || state == 4u;
}

} // namespace agentpad
