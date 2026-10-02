// Broken and knocked-over street objects, remembered while somebody is near
// them (docs/objects.md 9).
//
// The engine keeps a broken lamp post broken only while it is a CObject, and
// it stops being one on a machine whose player goes more than 80 m away:
// CPopulation::ManagePopulation turns it back into a dummy and, on the way
// back in, builds a pristine object out of it. On one machine that is single
// player's own behaviour. With two it meant a player who drove off and came
// back saw the post standing while the one who stayed saw it lying in the
// road.
//
// So the session keeps what it was told - how broken, how hard it was hit,
// and where it came to rest - for as long as any player is near enough that
// some machine may still hold it broken. Once nobody is, every machine's copy
// has gone back to a dummy and will come back pristine everywhere, so the
// record means nothing any more and goes. A machine that rebuilds an object
// it has heard about asks (C_ObjectRebuilt), and is sent the break and the
// resting place again, to it alone.
//
// Fixed size and linear: a car ploughing a street is a dozen of these, and a
// rebuild is only asked about for an object the client was told of.
#pragma once

#include <coopiii/protocol.h>

#include <cstddef>
#include <cstdint>

namespace coopiii {

// Farther than the engine's 80 m on purpose. A player the server places at
// 85 m may be at 79 m on their own screen, still holding the object broken;
// keeping a record a little too long only means a returning player sees it
// broken a little longer than single player would have.
constexpr float  OBJECT_RECORD_RANGE    = 120.0f;
constexpr size_t OBJECT_RECORD_CAPACITY = 256;

struct ObjectRecord {
	bool           used     = false;
	uint8_t        reporter = INVALID_PLAYER;
	ObjectBreakBody breakBody{};   // state OR'd over every report
	bool           hasRest  = false;
	ObjectRestBody rest{};
	uint32_t       touchedMs = 0;   // last report, for evicting the oldest
};

inline bool SameObjectIdent(const ObjectIdent &a, const ObjectIdent &b) {
	if (a.modelIndex != b.modelIndex)
		return false;
	const float dx = a.pos.x - b.pos.x, dy = a.pos.y - b.pos.y, dz = a.pos.z - b.pos.z;
	// 0.25 m, as client/src/game/object.h's kObjectIdentTolerance.
	return dx * dx + dy * dy + dz * dz <= 0.25f * 0.25f;
}

// A shattered window, as the record every other broken object keeps: the
// ident, the amount, and OBJ_BREAK_GLASS for a state. Nothing else of the
// shatter is kept, because what the record is for is a machine that builds
// the window again or joins later, and that one is told it is gone, not shown
// it going (docs/objects.md 10).
inline ObjectBreakBody GlassRecordBody(const GlassBreakBody &glass) {
	ObjectBreakBody body{};
	body.ident            = glass.ident;
	body.ident.pad0       = 0;
	body.ident.pad1       = 0;
	body.amount           = glass.amount == glass.amount ? glass.amount : 0.0f;
	body.state            = OBJ_BREAK_GLASS;
	return body;
}

class ObjectRecords {
public:
	const ObjectRecord *Find(const ObjectIdent &ident) const {
		for (const ObjectRecord &r : m_rows)
			if (r.used && SameObjectIdent(r.breakBody.ident, ident))
				return &r;
		return nullptr;
	}

	void NoteBroken(uint8_t reporter, const ObjectBreakBody &body, uint32_t nowMs) {
		ObjectRecord *r = Row(body.ident, nowMs);
		if (!r->used) {
			*r           = ObjectRecord{};
			r->used      = true;
			r->breakBody = body;
		} else {
			r->breakBody.state = static_cast<uint8_t>(r->breakBody.state | body.state);
			if (body.amount > r->breakBody.amount)
				r->breakBody.amount = body.amount;
		}
		r->reporter  = reporter;
		r->touchedMs = nowMs;
	}

	// A resting place with no break before it is a post knocked loose but
	// not bent: the break half says only that it is uprooted.
	void NoteSettled(uint8_t reporter, const ObjectRestBody &body, uint32_t nowMs) {
		ObjectRecord *r = Row(body.ident, nowMs);
		if (!r->used) {
			*r                 = ObjectRecord{};
			r->used            = true;
			r->breakBody.ident = body.ident;
			r->reporter        = reporter;
		}
		r->breakBody.state = static_cast<uint8_t>(r->breakBody.state | OBJ_BREAK_UPROOTED);
		r->hasRest         = true;
		r->rest            = body;
		r->touchedMs       = nowMs;
	}

	// Drops every record no position in `players` is within OBJECT_RECORD_RANGE
	// of. `players` is every player whose position the session knows.
	size_t ExpireFar(const Vec3 *players, size_t count) {
		size_t dropped = 0;
		for (ObjectRecord &r : m_rows) {
			if (!r.used)
				continue;
			bool anybody = false;
			for (size_t i = 0; i < count && !anybody; ++i) {
				const float dx = players[i].x - r.breakBody.ident.pos.x;
				const float dy = players[i].y - r.breakBody.ident.pos.y;
				const float dz = players[i].z - r.breakBody.ident.pos.z;
				anybody = dx * dx + dy * dy + dz * dz <=
				          OBJECT_RECORD_RANGE * OBJECT_RECORD_RANGE;
			}
			if (!anybody) {
				r = ObjectRecord{};
				++dropped;
			}
		}
		return dropped;
	}

	size_t Count() const {
		size_t n = 0;
		for (const ObjectRecord &r : m_rows)
			n += r.used ? 1 : 0;
		return n;
	}

	const ObjectRecord *Rows() const { return m_rows; }
	void Clear() {
		for (ObjectRecord &r : m_rows)
			r = ObjectRecord{};
	}

private:
	// The row for this ident: its own, a free one, or the one reported
	// longest ago.
	ObjectRecord *Row(const ObjectIdent &ident, uint32_t nowMs) {
		ObjectRecord *free = nullptr, *oldest = nullptr;
		for (ObjectRecord &r : m_rows) {
			if (r.used && SameObjectIdent(r.breakBody.ident, ident))
				return &r;
			if (!r.used) {
				if (!free)
					free = &r;
			} else if (!oldest || nowMs - r.touchedMs > nowMs - oldest->touchedMs) {
				oldest = &r;
			}
		}
		if (free)
			return free;
		*oldest = ObjectRecord{};
		return oldest;
	}

	ObjectRecord m_rows[OBJECT_RECORD_CAPACITY];
};

} // namespace coopiii
