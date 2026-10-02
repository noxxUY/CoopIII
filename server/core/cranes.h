// Who works each crane (protocol.h, C_CraneState; docs/protocol.md 1.62).
//
// A crane is worked by the machine whose engine took a car for it, and only
// one machine at a time: two that both took one in the same round trip would
// each be writing its hook onto the other's. So the first busy state the
// session hears for a crane makes its sender the worker, the worker's states
// are relayed and everybody else's are dropped, and the worker's own end
// frees it. A worker that goes quiet for CRANE_FOLLOW_TIMEOUT_MS, or leaves,
// gives it up, and the next busy state takes it.
//
// No socket here, so tools/sessiontest walks it; server.h does the sending.
#pragma once

#include "coopiii/protocol.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coopiii {

class CraneWorkers {
public:
	// At most this many cranes are being worked at once. The world has three
	// cranes with a hook and CCranes holds eight.
	static constexpr size_t MAX_WORKED = 8;

	// A state from `playerId`. True when it goes on to everybody else.
	bool Accept(uint8_t playerId, const CraneStateBody &body, uint32_t nowMs) {
		if (playerId == INVALID_PLAYER || !CraneStateSane(body))
			return false;
		Entry *e = Find(body.craneX, body.craneY);
		if (body.active == 0) {
			// Only the worker ends it; anybody else's end is about a crane
			// nobody was following them on.
			if (!e || e->worker != playerId)
				return false;
			m_entries.erase(m_entries.begin() + (e - m_entries.data()));
			return true;
		}
		if (!e) {
			if (m_entries.size() >= MAX_WORKED)
				return false;
			m_entries.push_back(Entry{body.craneX, body.craneY, playerId, nowMs});
			return true;
		}
		if (e->worker != playerId && nowMs - e->heardMs < CRANE_FOLLOW_TIMEOUT_MS)
			return false;
		e->worker  = playerId;
		e->heardMs = nowMs;
		return true;
	}

	// A player left. The cranes he was working, so everybody can be told they
	// are nobody's now rather than wait out the timeout.
	std::vector<CraneStateBody> Forget(uint8_t playerId) {
		std::vector<CraneStateBody> ended;
		for (size_t i = 0; i < m_entries.size();) {
			if (m_entries[i].worker != playerId) {
				++i;
				continue;
			}
			CraneStateBody b{};
			b.craneX = m_entries[i].x;
			b.craneY = m_entries[i].y;
			b.active = 0;
			ended.push_back(b);
			m_entries.erase(m_entries.begin() + static_cast<std::ptrdiff_t>(i));
		}
		return ended;
	}

	uint8_t WorkerAt(float x, float y) {
		const Entry *e = Find(x, y);
		return e ? e->worker : INVALID_PLAYER;
	}
	size_t Count() const { return m_entries.size(); }

private:
	struct Entry {
		float    x;
		float    y;
		uint8_t  worker;
		uint32_t heardMs;
	};
	std::vector<Entry> m_entries;

	Entry *Find(float x, float y) {
		for (Entry &e : m_entries)
			if (SameCrane(e.x, e.y, x, y))
				return &e;
		return nullptr;
	}
};

} // namespace coopiii
