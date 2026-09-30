// A fire truck's water cannon, session side. No engine code in here: the
// engine half is game/emergency.cpp, reached through CannonBridge, so
// tools/clienttest drives all of this with a stub.
//
// docs/protocol.md 1.37 is the design. In short:
//
//   - the machine that aims a truck's cannon is the one whose engine runs
//     CAutomobile::FireTruckControl for real: the driver's, or for a truck
//     nobody drives the host of that traffic car. Every frame it hands
//     CWaterCannons::UpdateOne the jet's start and direction, and those go
//     out (C_WaterCannon) in the truck's own frame at the snapshot rate;
//   - every other machine's copy of the truck sprays the newest jet it has
//     heard of through the same UpdateOne, every frame, until nothing has
//     been heard for WATER_CANNON_HOLD_MS;
//   - what the water does is not on the wire. Each engine runs its own
//     CWaterCannon over its own copy of the jet and decides about what it
//     owns: its own fires, its own player and pedestrians. The engine half
//     keeps it off everybody else's.
#pragma once

#include "clock.h"
#include "log.h"

#include <coopiii/net.h>
#include <coopiii/protocol.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace coopiii {

// A car's orientation the way the engine keeps it: CMatrix's right, forward
// and up rows and its position.
struct CarFrame {
	Vec3 right = {1.0f, 0.0f, 0.0f};
	Vec3 fwd   = {0.0f, 1.0f, 0.0f};
	Vec3 up    = {0.0f, 0.0f, 1.0f};
	Vec3 pos   = {};
};

inline float Dot3(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// A direction in the car's frame, into the world: what FireTruckControl's
// Multiply3x3(GetMatrix(), dir) does.
inline Vec3 CarVectorToWorld(const CarFrame &f, const Vec3 &v) {
	return {f.right.x * v.x + f.fwd.x * v.y + f.up.x * v.z,
	        f.right.y * v.x + f.fwd.y * v.y + f.up.y * v.z,
	        f.right.z * v.x + f.fwd.z * v.y + f.up.z * v.z};
}

// A point in the car's frame, into the world: GetMatrix() * point.
inline Vec3 CarPointToWorld(const CarFrame &f, const Vec3 &p) {
	const Vec3 v = CarVectorToWorld(f, p);
	return {v.x + f.pos.x, v.y + f.pos.y, v.z + f.pos.z};
}

// And back. The rows are orthonormal, so the inverse is the transpose.
inline Vec3 WorldVectorToCar(const CarFrame &f, const Vec3 &v) {
	return {Dot3(f.right, v), Dot3(f.fwd, v), Dot3(f.up, v)};
}

inline Vec3 WorldPointToCar(const CarFrame &f, const Vec3 &p) {
	return WorldVectorToCar(f, {p.x - f.pos.x, p.y - f.pos.y, p.z - f.pos.z});
}

// How far from a truck's centre a jet may start, and how fast it may leave,
// before a packet is taken for garbage. FireTruckControl's own are (0, 1.5,
// 1.9) for the driver's and (0, 0, 2.2) for the NPC arm's, and a direction of
// length one plus at most 0.015 of jitter on z (0x005227C5, `and eax,0Fh`,
// times 1/1000).
constexpr float WATER_CANNON_MAX_START_M = 8.0f;
constexpr float WATER_CANNON_MAX_SPEED   = 2.0f;

inline bool JetIsSane(const Vec3 &pos, const Vec3 &dir) {
	const float p = Dot3(pos, pos);
	const float d = Dot3(dir, dir);
	if (!std::isfinite(p) || !std::isfinite(d))
		return false;
	return p <= WATER_CANNON_MAX_START_M * WATER_CANNON_MAX_START_M &&
	       d <= WATER_CANNON_MAX_SPEED * WATER_CANNON_MAX_SPEED && d > 0.0f;
}

// One frame of a jet the local engine sprayed from a truck it aims, already
// in the truck's frame.
struct LocalCannonJet {
	uint16_t netId = INVALID_NETID;
	Vec3     pos   = {};
	Vec3     dir   = {};
};

// The engine seam. Every entry is optional; with none set every jet stays the
// machine's own, which is how it always was.
struct CannonBridge {
	// The jets our own engine sprayed since the last call, one per truck.
	uint32_t (*DrainLocalJets)(LocalCannonJet *out, uint32_t max) = nullptr;
	// Spray one frame of somebody else's jet from our copy of the truck. False
	// when there is no copy here to spray it from, or the copy is ours.
	bool (*SprayCopy)(uint16_t netId, const Vec3 &pos, const Vec3 &dir) = nullptr;
};

// CWaterCannons has three slots (0x00522489, `cmp ax,3`), so no screen can
// show more jets than that however many are heard of. A few more are held so
// a fourth truck does not push out one that is still spraying.
constexpr size_t MAX_REMOTE_JETS = 6;
constexpr size_t MAX_LOCAL_JETS  = 3;

class CannonSync {
public:
	using SendFn = void (*)(void *ctx, const void *bytes, size_t len, Channel ch);

	void Bind(const CannonBridge *bridge, SendFn send, void *ctx) {
		m_bridge  = bridge;
		m_send    = send;
		m_sendCtx = ctx;
	}

	// Somebody else's jet. The newest one for its truck replaces the last.
	void OnJet(const S_WaterCannon &pkt, uint8_t localPlayerId, uint32_t nowMs) {
		if (pkt.playerId == localPlayerId || pkt.body.netId == INVALID_NETID)
			return;
		if (!JetIsSane(pkt.body.pos, pkt.body.dir)) {
			if (!m_saidInsane) {
				m_saidInsane = true;
				Log("cannon: dropped a jet from player %u for car %u that no fire truck "
				    "could spray (and will not say so again)", pkt.playerId, pkt.body.netId);
			}
			return;
		}
		Jet *slot = nullptr;
		for (Jet &j : m_in)
			if (j.active && j.netId == pkt.body.netId)
				slot = &j;
		if (!slot)
			for (Jet &j : m_in)
				if (!j.active) {
					slot = &j;
					break;
				}
		if (!slot) {
			// Full: the one heard of longest ago goes.
			slot = &m_in[0];
			for (Jet &j : m_in)
				if (static_cast<int32_t>(j.heardMs - slot->heardMs) < 0)
					slot = &j;
		}
		slot->active  = true;
		slot->netId   = pkt.body.netId;
		slot->pos     = pkt.body.pos;
		slot->dir     = pkt.body.dir;
		slot->heardMs = nowMs;
	}

	// Before the world updates, every frame: each jet still live sprays from
	// our copy of its truck, so CWaterCannons::Update carries it on in the
	// same frame the way it does the engine's own.
	void Tick(uint32_t nowMs) {
		for (Jet &j : m_in) {
			if (!j.active)
				continue;
			if (nowMs - j.heardMs > WATER_CANNON_HOLD_MS) {
				j.active = false;
				continue;
			}
			if (!m_bridge || !m_bridge->SprayCopy)
				continue;
			const bool sprayed = m_bridge->SprayCopy(j.netId, j.pos, j.dir);
			++(sprayed ? m_sprayed : m_noCopy);
			if (sprayed && !m_saidSprayed) {
				m_saidSprayed = true;
				Log("cannon: sprayed our first jet from somebody else's fire truck (car %u)",
				    j.netId);
			} else if (!sprayed && !m_saidNoCopy) {
				m_saidNoCopy = true;
				Log("cannon: heard a jet from fire truck %u and have no copy of it here to "
				    "spray it from (and will not say so again)", j.netId);
			}
		}
	}

	// After the world updates: this frame's own jets, out at the snapshot rate.
	void Send(uint32_t nowMs) {
		if (m_bridge && m_bridge->DrainLocalJets) {
			LocalCannonJet jets[MAX_LOCAL_JETS];
			const uint32_t n = m_bridge->DrainLocalJets(jets, MAX_LOCAL_JETS);
			for (uint32_t i = 0; i < n && i < MAX_LOCAL_JETS; ++i)
				Hold(jets[i]);
		}
		bool any = false;
		for (const Out &o : m_out)
			any = any || o.fresh;
		if (!any || !m_send || !m_rate.Ready(nowMs))
			return;
		for (Out &o : m_out) {
			if (!o.fresh)
				continue;
			C_WaterCannon out;
			InitHeader(out, nowMs);
			out.body.netId = o.jet.netId;
			out.body.pos   = o.jet.pos;
			out.body.dir   = o.jet.dir;
			m_send(m_sendCtx, &out, sizeof out, CH_SNAPSHOT);
			o.fresh = false;
			++m_sent;
			if (!m_saidSent) {
				m_saidSent = true;
				Log("cannon: our fire truck %u is spraying; its jet goes out so every "
				    "screen sprays it", o.jet.netId);
			}
		}
	}

	void Clear() {
		for (Jet &j : m_in)
			j = Jet{};
		for (Out &o : m_out)
			o = Out{};
	}

	// ---- for tools/clienttest ------------------------------------------------
	size_t LiveJets() const {
		size_t n = 0;
		for (const Jet &j : m_in)
			n += j.active ? 1u : 0u;
		return n;
	}
	uint32_t Sprayed() const { return m_sprayed; }
	uint32_t NoCopy() const { return m_noCopy; }
	uint32_t Sent() const { return m_sent; }

private:
	struct Jet {
		bool     active  = false;
		uint16_t netId   = INVALID_NETID;
		Vec3     pos     = {};
		Vec3     dir     = {};
		uint32_t heardMs = 0;
	};
	struct Out {
		bool           fresh = false;
		LocalCannonJet jet{};
	};

	void Hold(const LocalCannonJet &jet) {
		if (jet.netId == INVALID_NETID || !JetIsSane(jet.pos, jet.dir))
			return;
		Out *slot = nullptr;
		for (Out &o : m_out)
			if (o.jet.netId == jet.netId)
				slot = &o;
		if (!slot)
			for (Out &o : m_out)
				if (!o.fresh) {
					slot = &o;
					break;
				}
		if (!slot)
			return;
		slot->jet   = jet;
		slot->fresh = true;
	}

	const CannonBridge *m_bridge  = nullptr;
	SendFn              m_send    = nullptr;
	void               *m_sendCtx = nullptr;

	Jet         m_in[MAX_REMOTE_JETS];
	Out         m_out[MAX_LOCAL_JETS];
	RateLimiter m_rate{SNAPSHOT_HZ};

	uint32_t m_sprayed = 0;
	uint32_t m_noCopy  = 0;
	uint32_t m_sent    = 0;
	bool     m_saidInsane  = false;
	bool     m_saidSprayed = false;
	bool     m_saidNoCopy  = false;
	bool     m_saidSent    = false;
};

} // namespace coopiii
