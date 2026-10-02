// The players' custom skins, on the server (docs/protocol.md 1.72).
//
// A skin comes in from its player in pieces (C_PlayerSkin). Each piece is
// checked as it lands (coopiii/skin.h, SkinAssembly), and only a skin that is
// all here and hashes right is kept, one per player, and sent on: to every
// other player, and to whoever joins later. Nothing of a half-sent skin ever
// leaves the server.
//
// What goes to each player is paced on its own (SkinBudget), so a joiner who
// is owed seven skins gets them over a few seconds rather than all at once in
// front of everything else on the channel. A skin that changes while it is
// still on its way to somebody starts again for him from its first piece; the
// receiver throws the old half away the same way.
//
// No sockets here: server.h feeds it and sends what it hands back, and
// sessiontest drives it alone.
#pragma once

#include <coopiii/net.h>
#include <coopiii/protocol.h>
#include <coopiii/skin.h>

#include <cstdint>
#include <memory>

namespace coopiii {

class SkinRelay {
public:
	// A piece from player `from`. True when it finished a skin, which is
	// now the one that player wears and is on its way to everybody else.
	bool Take(uint8_t from, const C_PlayerSkin &in) {
		if (from >= MAX_PLAYERS)
			return false;
		const SkinAssembly::Step step = m_in[from].Take(in.chunk);
		if (step == SkinAssembly::Step::Rejected) {
			++m_rejected;
			return false;
		}
		if (step != SkinAssembly::Step::Complete)
			return false;
		m_skin[from] = std::make_shared<const Skin>(m_in[from].Finished());
		for (uint8_t to = 0; to < MAX_PLAYERS; ++to)
			if (to != from && m_member[to])
				m_out[to][from] = Outgoing{m_skin[from], 0};
		return true;
	}

	// A player is in the session and may be sent skins: everybody's that is
	// held, from the start.
	void Joined(uint8_t id) {
		if (id >= MAX_PLAYERS)
			return;
		m_member[id] = true;
		m_budget[id].Reset();
		for (uint8_t from = 0; from < MAX_PLAYERS; ++from)
			m_out[id][from] = from != id && m_skin[from] ? Outgoing{m_skin[from], 0} : Outgoing{};
	}

	// Gone: their skin, what they were sending, and what was on its way to
	// them or from them.
	void Left(uint8_t id) {
		if (id >= MAX_PLAYERS)
			return;
		m_member[id] = false;
		m_skin[id].reset();
		m_in[id].Clear();
		for (uint8_t other = 0; other < MAX_PLAYERS; ++other) {
			m_out[id][other]  = Outgoing{};
			m_out[other][id]  = Outgoing{};
		}
	}

	// Every skin forgotten, for syncCustomSkins switched off. Who is in stays.
	void Forget() {
		for (uint8_t id = 0; id < MAX_PLAYERS; ++id) {
			m_skin[id].reset();
			m_in[id].Clear();
			for (Outgoing &o : m_out[id])
				o = Outgoing{};
		}
	}

	// The next piece for player `to`, when one is owed and its pace allows.
	// Takes turns between the skins it is owed.
	bool Next(uint8_t to, uint32_t nowMs, S_PlayerSkin &out) {
		if (to >= MAX_PLAYERS || !m_member[to])
			return false;
		for (uint8_t step = 0; step < MAX_PLAYERS; ++step) {
			const uint8_t from = static_cast<uint8_t>((m_turn[to] + step) % MAX_PLAYERS);
			Outgoing     &o    = m_out[to][from];
			if (!o.skin)
				continue;
			SkinChunk piece;
			uint32_t  next = o.offset;
			if (!CutSkinPiece(*o.skin, next, piece)) {
				o = Outgoing{};
				continue;
			}
			if (!m_budget[to].Spend(nowMs, SkinPieceCost(piece)))
				return false;
			o.offset = next;
			if (SkinPiecesDone(*o.skin, o.offset))
				o = Outgoing{};
			m_turn[to] = static_cast<uint8_t>((from + 1) % MAX_PLAYERS);
			InitHeader(out, nowMs);
			out.playerId = from;
			out.chunk    = piece;
			return true;
		}
		return false;
	}

	// What a player wears, as far as the server holds it, or null.
	const SkinInfo *Held(uint8_t id) const {
		return id < MAX_PLAYERS && m_skin[id] ? &m_skin[id]->info : nullptr;
	}
	// Whether anything is still owed to `to`.
	bool Owed(uint8_t to) const {
		if (to >= MAX_PLAYERS)
			return false;
		for (const Outgoing &o : m_out[to])
			if (o.skin)
				return true;
		return false;
	}
	uint32_t Rejected() const { return m_rejected; }

private:
	struct Outgoing {
		SkinRef  skin;
		uint32_t offset = 0;
	};

	SkinAssembly m_in[MAX_PLAYERS];
	SkinRef      m_skin[MAX_PLAYERS];
	Outgoing     m_out[MAX_PLAYERS][MAX_PLAYERS];   // [to][from]
	SkinBudget   m_budget[MAX_PLAYERS];
	uint8_t      m_turn[MAX_PLAYERS] = {};
	bool         m_member[MAX_PLAYERS] = {};
	uint32_t     m_rejected = 0;
};

} // namespace coopiii
