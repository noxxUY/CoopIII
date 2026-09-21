// Refuses to run against an executable these addresses weren't derived from.
//
// Every address in addresses.h belongs to one exact build. Install a detour
// at the wrong address and it doesn't fail cleanly - it corrupts an
// unrelated function and the game dies somewhere else entirely, minutes
// later, with a stack that points nowhere near CoopIII. So the check runs
// once, before the first hook, and a mismatch stops us loading instead of
// limping along.
//
// Not idle caution, either: the target install is a Steam copy downgraded
// to the retail 1.0 exe. Whichever exe ends up in place is something the
// player can change just by copying a file over.
#pragma once

#include <string>

namespace coopiii::game {

struct VerifyResult {
	bool        ok = false;
	std::string detail;   // human-readable, always populated - goes in the log
};

// Checks the running gta3.exe against addresses.h. Read-only, safe to call
// before anything else is initialised.
VerifyResult VerifyGameImage();

} // namespace coopiii::game
