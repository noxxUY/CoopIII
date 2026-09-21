#include "channel.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <cstring>

namespace agentpad {

Channel::~Channel() {
	Close();
}

InstanceIndex::~InstanceIndex() {
	Close();
}

#ifdef _WIN32

// Every section below is created with default security - lands in the
// caller's session namespace, carries the caller's default DACL. Same
// interactive user can open it, nobody else can. A null DACL would be more
// "convenient" but it'd hand a write handle on the game's input to every
// account on the machine. Not worth it for a debugging tool.

bool Channel::Create(uint32_t pid) {
	Close();

	if (!FormatSectionName(pid, m_name, sizeof(m_name))) {
		m_error = "the section name does not fit, refusing to use a truncated one";
		return false;
	}
	m_pid = pid;

	const HANDLE mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
	                                          static_cast<DWORD>(sizeof(Shared)), m_name);
	if (!mapping) {
		m_error = "CreateFileMapping failed";
		return false;
	}
	const bool existed = GetLastError() == ERROR_ALREADY_EXISTS;

	void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!view) {
		CloseHandle(mapping);
		m_error = "MapViewOfFile failed";
		return false;
	}

	m_mapping = mapping;
	m_view    = static_cast<Shared *>(view);

	// Kernel zero-fills a fresh section, and that's already a valid "disabled,
	// no magic" state - Decide() reports BAD_HEADER and we pass through. Must
	// not stamp the driver's half of the struct if the section already
	// existed, that would wipe a driver that got there first. With per-pid
	// names, "already existed" basically only means "this same process made
	// it" - only reachable from a test.
	if (!existed)
		std::memset(m_view, 0, sizeof(Shared));

	m_error = "";
	return true;
}

bool Channel::Open(uint32_t pid) {
	Close();

	if (!FormatSectionName(pid, m_name, sizeof(m_name))) {
		m_error = "the section name does not fit";
		return false;
	}
	m_pid = pid;

	const HANDLE mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, m_name);
	if (!mapping) {
		m_error = "OpenFileMapping failed. Is the mod loaded in that process?";
		return false;
	}

	void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared));
	if (!view) {
		CloseHandle(mapping);
		m_error = "MapViewOfFile failed";
		return false;
	}

	m_mapping = mapping;
	m_view    = static_cast<Shared *>(view);
	m_error   = "";
	return true;
}

void Channel::Close() {
	if (m_view) {
		UnmapViewOfFile(m_view);
		m_view = nullptr;
	}
	if (m_mapping) {
		CloseHandle(static_cast<HANDLE>(m_mapping));
		m_mapping = nullptr;
	}
}

bool InstanceIsLive(uint32_t pid) {
	char name[SHM_NAME_MAX];
	if (!FormatSectionName(pid, name, sizeof(name)))
		return false;

	// FILE_MAP_READ, not ALL_ACCESS - this is just a liveness probe, no reason
	// it should be able to write to somebody else's input.
	const HANDLE h = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
	if (!h)
		return false;
	CloseHandle(h);
	return true;
}

bool InstanceIndex::Open(bool create) {
	Close();

	const HANDLE mapping =
	    create ? CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
	                                static_cast<DWORD>(sizeof(Index)), SHM_INDEX_NAME)
	           : OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, SHM_INDEX_NAME);
	if (!mapping) {
		m_error = create ? "CreateFileMapping failed for the instance index"
		                 : "no instance index, no game is publishing one";
		return false;
	}

	void *view = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Index));
	if (!view) {
		CloseHandle(mapping);
		m_error = "MapViewOfFile failed for the instance index";
		return false;
	}

	m_mapping = mapping;
	m_view    = static_cast<Index *>(view);

	// Not memset, even when we created it. Fresh section is already
	// zero-filled by the kernel, and zeroing it here would race a second game
	// that created it a microsecond earlier and already wrote its pid.
	// Stamping the header is fine to do unconditionally though, since every
	// writer stamps the same constants.
	if (create) {
		m_view->magic    = MAGIC;
		m_view->version  = VERSION;
		m_view->capacity = INDEX_SLOTS;
	}

	m_error = "";
	return true;
}

void InstanceIndex::Close() {
	if (m_view) {
		UnmapViewOfFile(m_view);
		m_view = nullptr;
	}
	if (m_mapping) {
		CloseHandle(static_cast<HANDLE>(m_mapping));
		m_mapping = nullptr;
	}
}

bool InstanceIndex::Add(uint32_t pid) {
	if (!m_view || pid == 0) {
		m_error = "the instance index is not open";
		return false;
	}

	auto *slots = reinterpret_cast<volatile LONG *>(m_view->pid);

	// Already listed - re-register after a hook got reinstalled, probably.
	for (uint32_t i = 0; i < INDEX_SLOTS; ++i)
		if (static_cast<uint32_t>(slots[i]) == pid)
			return true;

	// Look for a free slot. CAS instead of a plain store because two games can
	// start at the same moment, and this is the only synchronization the whole
	// index has.
	for (uint32_t i = 0; i < INDEX_SLOTS; ++i)
		if (InterlockedCompareExchange(&slots[i], static_cast<LONG>(pid), 0) == 0)
			return true;

	// Table's full. Try reclaiming a slot whose section is gone - that game
	// crashed, or the pid got recycled by something unrelated.
	for (uint32_t i = 0; i < INDEX_SLOTS; ++i) {
		const LONG stale = slots[i];
		if (stale == 0 || InstanceIsLive(static_cast<uint32_t>(stale)))
			continue;
		if (InterlockedCompareExchange(&slots[i], static_cast<LONG>(pid), stale) == stale)
			return true;
	}

	m_error = "every index slot is held by a live instance";
	return false;
}

void InstanceIndex::Remove(uint32_t pid) {
	if (!m_view || pid == 0)
		return;

	auto *slots = reinterpret_cast<volatile LONG *>(m_view->pid);
	for (uint32_t i = 0; i < INDEX_SLOTS; ++i)
		InterlockedCompareExchange(&slots[i], 0, static_cast<LONG>(pid));
}

uint32_t InstanceIndex::List(uint32_t *out, uint32_t cap) const {
	if (!m_view || !out)
		return 0;

	uint32_t n = 0;
	for (uint32_t i = 0; i < INDEX_SLOTS && n < cap; ++i) {
		const uint32_t pid = m_view->pid[i];
		if (pid != 0)
			out[n++] = pid;
	}
	return n;
}

#else

bool Channel::Create(uint32_t) { m_error = "not a Windows build"; return false; }
bool Channel::Open(uint32_t)   { m_error = "not a Windows build"; return false; }
void Channel::Close()          { m_view = nullptr; m_mapping = nullptr; }

bool InstanceIsLive(uint32_t) { return false; }

bool InstanceIndex::Open(bool) { m_error = "not a Windows build"; return false; }
void InstanceIndex::Close()    { m_view = nullptr; m_mapping = nullptr; }
bool InstanceIndex::Add(uint32_t)    { return false; }
void InstanceIndex::Remove(uint32_t) {}
uint32_t InstanceIndex::List(uint32_t *, uint32_t) const { return 0; }

#endif

} // namespace agentpad
