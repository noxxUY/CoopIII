// The named sections themselves: the per-instance one the mod publishes, plus
// the small directory that lets a driver find every instance at once.
//
// Split out from protocol.h so the decision logic stays testable without
// Windows. This is the only file in the mod that calls CreateFileMapping.
#pragma once

#include "protocol.h"

namespace agentpad {

// One game process's input section, named "CoopIII.AgentPad.v1.<pid>".
//
// Per-process naming matters because running two copies of GTA III is the
// whole point: with a fixed name the second game's CreateFileMapping just
// attaches to the first game's section, and then both mods read whatever one
// driver happens to write.
class Channel {
public:
	Channel() = default;
	~Channel();
	Channel(const Channel &)            = delete;
	Channel &operator=(const Channel &) = delete;

	// Creates the section for `pid` if it doesn't exist yet, opens it if it does.
	// The mod passes its own pid; nothing else should be creating one.
	//
	// Returns false and leaves Get() null on failure. Caller falls back to
	// pass-through - shared memory being missing should never mean the player
	// loses keyboard control.
	bool Create(uint32_t pid);

	// Opens an existing section without creating one. For the driver side and
	// for tests.
	bool Open(uint32_t pid);

	void Close();

	// Null until Create()/Open() succeeds. Stays valid until Close().
	Shared       *Get()       { return m_view; }
	const Shared *Get() const { return m_view; }

	bool IsOpen() const { return m_view != nullptr; }

	// Pid this channel is bound to, and the section name it resolved to. Both
	// go in the log - AgentPad.log is useless once there's more than one game
	// running and it doesn't say which section it published.
	uint32_t    Pid() const  { return m_pid; }
	const char *Name() const { return m_name; }

	// Human-readable reason the last Create/Open failed, for the log.
	const char *Error() const { return m_error; }

private:
	void       *m_mapping           = nullptr;
	Shared     *m_view              = nullptr;
	uint32_t    m_pid               = 0;
	char        m_name[SHM_NAME_MAX] = {0};
	const char *m_error             = "";
};

// True if some process is currently publishing a section for `pid`. This is
// how we tell a stale index entry from a live one - the section dies with its
// last handle, so a crashed game leaves a pid in the table whose section can
// no longer be opened.
bool InstanceIsLive(uint32_t pid);

// Directory of live instances, published under SHM_INDEX_NAME.
//
// Every mod instance registers its pid here and clears it on unload, so a
// driver can enumerate instances without knowing the game's exe name. Treat
// the table as advisory - it can hold stale pids after a crash, and a driver
// should confirm each one by opening its section. It exists to make
// enumeration possible. The sections themselves are the source of truth.
class InstanceIndex {
public:
	InstanceIndex() = default;
	~InstanceIndex();
	InstanceIndex(const InstanceIndex &)            = delete;
	InstanceIndex &operator=(const InstanceIndex &) = delete;

	// `create` is what the mod passes; a pure reader passes false and just
	// gets false back if nobody's published one yet.
	bool Open(bool create);
	void Close();

	bool IsOpen() const { return m_view != nullptr; }

	// Claims a slot for `pid`. Already registered counts as success. If the
	// table's full, tries to reclaim a slot whose section can't be opened
	// anymore first. If every slot is genuinely live, this fails - and that's
	// not fatal, since a driver can still address an unlisted instance by pid
	// directly.
	bool Add(uint32_t pid);

	// Clears `pid`'s slot. Fine to call even if it was never added.
	void Remove(uint32_t pid);

	// Copies the non-zero pids into `out`, returns how many. Doesn't filter
	// stale entries - that's on the caller, by trying to open each section.
	uint32_t List(uint32_t *out, uint32_t cap) const;

	const char *Error() const { return m_error; }

private:
	void       *m_mapping = nullptr;
	Index      *m_view    = nullptr;
	const char *m_error   = "";
};

} // namespace agentpad
