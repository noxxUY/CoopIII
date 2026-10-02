// Custom skins, session side (docs/protocol.md 1.72). No engine code in here:
// game/skin.cpp is the engine half, reached through SkinBridge, so
// tools/clienttest drives all of this with a stub.
//
// The engine lays one skin over every ped built from the "player" model, the
// local player's, so every remote player used to wear ours. Now each one
// wears his own:
//
//   - ours goes out. The skin our game wears (CPlayerInfo::m_aSkinName) is
//     read from the skins folder, decoded here, and sent in pieces
//     (C_PlayerSkin), paced, again whenever Player Setup changes it. A skin
//     that cannot be read or is not a size the wire takes goes out as the
//     default one, so nobody keeps an old one of ours;
//   - theirs come in. Each player's pieces are put back together and checked
//     (coopiii/skin.h), and a whole skin is handed to the engine half for
//     that player's ped alone. It keeps it while they stay, so a rebuilt ped
//     wears it at once. Until a skin of theirs is in, and whenever the
//     session's rule is off, a remote player wears the game's default skin,
//     never ours.
#pragma once

#include <coopiii/protocol.h>
#include <coopiii/skin.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace coopiii {

// A whole skin as pixels: RGBA, top row first.
struct SkinImage {
	uint16_t             width  = 0;
	uint16_t             height = 0;
	std::vector<uint8_t> rgba;
};

// The engine seam. Every entry is optional; with none set nothing is sent and
// nothing is drawn differently.
struct SkinBridge {
	// The name of the skin the local player wears, as Player Setup set it.
	// False when the game has not picked one yet.
	bool (*ReadLocalSkinName)(char (&name)[SKIN_NAME_LEN]) = nullptr;
	// Its pixels, from the skins folder. False for the default skin, for a
	// file that is not there, and for one that cannot be read.
	bool (*LoadLocalSkin)(const char *name, SkinImage &out) = nullptr;
	// What a remote player wears from now on: these pixels, or with null the
	// game's default skin.
	void (*SetRemoteSkin)(uint8_t playerId, const SkinImage *image) = nullptr;
	// Every frame, after the roster has built its peds: each slot's ped, by
	// pool reference, or -1 for none, so each is drawn in its own skin.
	void (*PlaceRemoteSkins)(const int32_t (&peds)[MAX_PLAYERS]) = nullptr;
};

// How often the local skin's name is looked at. Player Setup is a menu, so a
// second is plenty.
constexpr uint32_t SKIN_POLL_MS = 1000;

// A BMP file to RGBA, top row first: uncompressed 4, 8, 24 or 32 bits, either
// way up, the formats the game's own skins come in. False for anything else,
// or for a picture whose size is not a skin's.
bool ReadSkinBmp(const uint8_t *file, size_t len, SkinImage &out);

class SkinSync {
public:
	using SendFn = void (*)(void *ctx, const void *bytes, size_t len, Channel ch);

	void Bind(const SkinBridge *bridge, SendFn send, void *sendCtx) {
		m_bridge  = bridge;
		m_send    = send;
		m_sendCtx = sendCtx;
	}

	// S_SessionRules' `skins`. Off puts every remote player in the default
	// skin and stops ours; back on, ours goes out again.
	void SetRule(uint8_t rule);
	uint8_t Rule() const { return m_rule; }

	// A piece of somebody's skin. `known` is whether the roster has them.
	void OnSkin(const S_PlayerSkin &pkt, uint8_t localPlayerId, bool known);

	// They left: back to the default skin, and nothing of theirs kept.
	void Forget(uint8_t playerId);

	// The session is gone: every remote skin forgotten, and ours to be sent
	// again to the next one.
	void Clear();

	// Ours, once a frame while in a session: a new skin noticed, and pieces
	// sent as the pace allows.
	void Tick(uint8_t localPlayerId, uint32_t nowMs);

	// Each slot's ped, to the engine half.
	void Place(const int32_t (&peds)[MAX_PLAYERS]) {
		if (m_bridge && m_bridge->PlaceRemoteSkins)
			m_bridge->PlaceRemoteSkins(peds);
	}

	// ---- for tests -----------------------------------------------------------
	// What a remote player is known to wear: a whole skin's info, or null for
	// the default skin.
	const SkinInfo *WornBy(uint8_t playerId) const {
		return playerId < MAX_PLAYERS && m_worn[playerId] ? &m_wornInfo[playerId] : nullptr;
	}
	uint32_t PiecesSent() const { return m_piecesSent; }
	bool     Sending() const { return m_sending; }
	const SkinInfo &Ours() const { return m_mine.info; }

private:
	void StartSending(const char *name);
	void Wear(uint8_t playerId, const Skin *skin);

	const SkinBridge *m_bridge  = nullptr;
	SendFn            m_send    = nullptr;
	void             *m_sendCtx = nullptr;

	uint8_t m_rule = SKIN_RULE_SYNC;

	// Ours: the skin being sent, how far it has gone, and the name it was
	// read under.
	Skin       m_mine;
	uint32_t   m_offset   = 0;
	bool       m_sending  = false;
	bool       m_sent     = false;   // the session has a skin of ours
	char       m_sentName[SKIN_NAME_LEN] = {};
	uint32_t   m_serial   = 0;
	uint32_t   m_polledMs = 0;
	bool       m_polled   = false;
	SkinBudget m_budget;
	uint32_t   m_piecesSent = 0;

	// Theirs.
	SkinAssembly m_in[MAX_PLAYERS];
	bool         m_worn[MAX_PLAYERS] = {};
	SkinInfo     m_wornInfo[MAX_PLAYERS] = {};
};

} // namespace coopiii
