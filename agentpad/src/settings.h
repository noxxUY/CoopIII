// AgentPad.ini - the two things about AgentPad a person might want to change.
//
// Kept tiny and kept separate from client/src/config.*. That one belongs to
// the co-op client, knows about the wire protocol, links the sdk. AgentPad
// links neither of those, and an input driver needing the network protocol
// headers just to read a boolean would be a bad sign.
//
// Both keys are off or conservative by default. Installing AgentPad should
// never change how the game behaves until somebody actually asks it to.
#pragma once

#include <cstdint>
#include <string>

namespace agentpad {

struct Settings {
	// Forces the game past its two startup movies, which cost about 70 seconds
	// on the target machine, paid on every automated test run.
	//
	// Off by default. Writes gGameState, the game's own state machine - safe
	// for exactly two transitions and no others (see intro.cpp for the full
	// argument).
	//
	//   AgentPad.ini:  SkipIntro=1
	//   environment:   AGENTPAD_SKIPINTRO=1
	bool skipIntro = false;

	// Whether to give this process its own "CdStream" semaphore name.
	//
	// GTA III creates a session-named semaphore called "CdStream" for its
	// streaming thread. Not a single-instance guard, but it does collide - the
	// second copy of the game opens the first copy's semaphore, and from then
	// on one process's ReleaseSemaphore wakes the other process's streaming
	// thread. re3 fixes this under FIX_BUGS by making the semaphore unnamed
	// (CdStream.cpp:73-77). Can't recompile the retail exe, so we rename it in
	// memory instead.
	//
	//   Auto (default) - rename only when the name's already taken, so the
	//                    first copy behaves exactly as always and only the
	//                    second one deviates.
	//   On             - always rename.
	//   Off            - never touch it.
	//
	//   AgentPad.ini:  MultiInstance=auto|on|off
	//   environment:   AGENTPAD_MULTIINSTANCE=auto|on|off
	enum class MultiInstance : uint32_t { Off = 0, Auto = 1, On = 2 };
	MultiInstance multiInstance = MultiInstance::Auto;

	// Parses INI text. Unknown keys get ignored instead of failing, so a
	// config from a newer build still loads. Pure - no file, no environment,
	// no Windows - so padtest can cover it directly.
	void ParseIni(const std::string &text);

	// Applies environment overrides. `skipIntro` and `multiInstance` are the
	// raw strings - a null or empty value leaves the field alone, an
	// unparseable one is just ignored. Split from the getenv call so it stays
	// testable.
	void ApplyOverrides(const char *skipIntroValue, const char *multiInstanceValue);

	// Reads `path` if it exists (a missing file isn't an error, the defaults
	// are the intended behavior), then applies AGENTPAD_* from the
	// environment, which wins. That order's deliberate: the ini is the
	// machine's setting, the environment is this run's.
	void Load(const std::string &path);

	static const char *MultiInstanceName(MultiInstance m);
};

} // namespace agentpad
