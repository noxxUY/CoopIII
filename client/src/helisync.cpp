#include "helisync.h"

#include "log.h"

#include <coopiii/net.h>

namespace coopiii {

// ---- inbound ----------------------------------------------------------------

void HeliSync::OnState(const S_HeliState &pkt, uint8_t localPlayerId,
                       uint32_t nowMs) {
	const uint8_t owner = pkt.ownerPlayerId;
	const HeliStateBody &in = pkt.body;
	if (owner >= MAX_PLAYERS || owner == localPlayerId)
		return;
	if (!IsPoliceHeliSlot(in.slot) || !IsKnownHeliStatus(in.status))
		return;
	if (m_gone[owner].Has(in.serial))
		return;   // overtook its own C_HeliGone

	RemoteHeli *row = FindRow(owner, in.serial);
	if (!row) {
		for (RemoteHeli &r : m_rows)
			if (!r.active) {
				row = &r;
				break;
			}
		if (!row) {
			if (!m_saidFull) {
				m_saidFull = true;
				Log("heli: already holding %zu helicopters; the rest are not "
				    "shown here (and this will not be said again)",
				    MAX_REMOTE_HELIS);
			}
			return;
		}
		*row        = RemoteHeli{};
		row->active = true;
		row->owner  = owner;
		row->slot   = in.slot;
		row->serial = in.serial;
		if (!m_saidStream) {
			m_saidStream = true;
			Log("heli: player %u has a police helicopter (serial %u, slot %u); "
			    "building a replica of it here",
			    owner, in.serial, in.slot);
		}
	}

	// A stream that went quiet and came back. Not for an owner who has left:
	// nothing of theirs is taken back once they are gone.
	if (row->orphaned && !row->ownerLeft) {
		row->orphaned = false;
		row->interp.Clear();
	}
	if (row->ownerLeft)
		return;

	row->interp.Push(pkt.hdr.sendTimeMs, in.pos, in.rot, in.velocity);
	if (!row->haveState) {
		row->last.pos  = in.pos;
		row->last.rot  = in.rot;
		row->haveState = true;
	}
	row->look.status               = in.status;
	row->look.flags                = in.flags;
	row->look.searchLightX         = in.searchLightX;
	row->look.searchLightY         = in.searchLightY;
	row->look.searchLightIntensity = in.searchLightIntensity;
	row->look.velocity             = in.velocity;
	row->lastHeardMs               = nowMs;
}

void HeliSync::OnGone(const S_HeliGone &pkt, uint8_t localPlayerId) {
	const uint8_t owner = pkt.ownerPlayerId;
	const HeliGoneBody &in = pkt.body;
	if (owner >= MAX_PLAYERS || owner == localPlayerId)
		return;
	if (!IsPoliceHeliSlot(in.slot) || !IsKnownHeliGoneReason(in.reason))
		return;

	m_gone[owner].Add(in.serial);

	if (RemoteHeli *row = FindRow(owner, in.serial)) {
		if (in.reason == HELI_GONE_SHOT_DOWN && m_bridge &&
		    m_bridge->ExplodeHeliReplica)
			m_bridge->ExplodeHeliReplica(*row, in.pos);
		Remove(*row);
	}

	// Ours to credit whether or not we ever had a replica: the owner says our
	// hit is the one that did it.
	if (in.reason == HELI_GONE_SHOT_DOWN && in.creditPlayerId == localPlayerId &&
	    localPlayerId != INVALID_PLAYER) {
		const bool paid = m_payShootDown && m_bridge && m_bridge->PayHeliShootDown;
		Log("heli: we shot down player %u's police helicopter; the crime and the "
		    "statistics are ours, the $250 is %s",
		    owner, paid ? "ours too" : "nobody's");
		if (m_bridge && m_bridge->CreditHeliShootDown)
			m_bridge->CreditHeliShootDown(in.slot, in.pos);
		if (paid)
			m_bridge->PayHeliShootDown();
	}
}

void HeliSync::OnHit(const S_HeliHit &pkt, uint8_t localPlayerId) {
	const HeliHitBody &in = pkt.body;
	if (in.ownerPlayerId != localPlayerId || !IsSaneHeliHit(in))
		return;
	if (pkt.attackerId >= MAX_PLAYERS || pkt.attackerId == localPlayerId)
		return;

	const OwnHeli &own = m_own[in.slot];
	if (!own.active || own.serial != in.serial)
		return;   // a hit on a helicopter we have already reported gone
	if (!m_bridge || !m_bridge->ApplyHeliHit)
		return;

	const bool took = m_bridge->ApplyHeliHit(in.slot, own.handle, pkt.attackerId, in);
	if (took && !m_saidHitTaken) {
		m_saidHitTaken = true;
		Log("heli: player %u hit our police helicopter (%s); our engine decides "
		    "what it cost",
		    pkt.attackerId, in.kind == HELI_HIT_ROCKET ? "rocket" : "bullet");
	}
}

void HeliSync::OnShot(const S_HeliShot &pkt, uint8_t localPlayerId, uint32_t nowMs) {
	const uint8_t owner = pkt.ownerPlayerId;
	const HeliShotBody &in = pkt.body;
	if (owner >= MAX_PLAYERS || owner == localPlayerId || !IsSaneHeliShot(in))
		return;

	// Only from a replica that is here and following its owner. One that was
	// never built, has gone, or is climbing away has nothing to fire from,
	// and a flash in the empty sky is worse than none.
	const RemoteHeli *row = FindRow(owner, in.serial);
	if (!row || row->slot != in.slot || !row->Spawned() || row->orphaned ||
	    !row->haveState) {
		if (!m_saidShotNoReplica) {
			m_saidShotNoReplica = true;
			Log("heli: player %u's helicopter %u fired and we have no replica of it "
			    "here; the round is dropped (and this will not be said again)",
			    owner, in.serial);
		}
		return;
	}

	PendingShot *slot = nullptr;
	for (PendingShot &s : m_shots)
		if (!s.used) {
			slot = &s;
			break;
		}
	if (!slot) {
		if (!m_saidShotsFull) {
			m_saidShotsFull = true;
			Log("heli: already holding %zu helicopter rounds; the rest are not "
			    "drawn (and this will not be said again)",
			    MAX_PENDING_HELI_SHOTS);
		}
		return;
	}
	slot->used   = true;
	slot->owner  = owner;
	slot->serial = in.serial;
	slot->dueMs  = nowMs + HeliShotHoldMs(pkt.hdr.sendTimeMs, row->interp.NewestTimeMs());
	slot->source = in.source;
	slot->target = in.target;
}

void HeliSync::OnOwnerLeft(uint8_t owner, uint32_t nowMs) {
	if (owner >= MAX_PLAYERS)
		return;
	for (RemoteHeli &r : m_rows)
		if (r.active && r.owner == owner) {
			r.ownerLeft = true;
			if (!r.orphaned)
				Orphan(r, nowMs);
		}
	// The slot will be somebody else's, with serials of their own.
	m_gone[owner] = GoneRing{};
}

// ---- per frame --------------------------------------------------------------

void HeliSync::Tick(uint32_t nowMs) {
	for (RemoteHeli &r : m_rows) {
		if (!r.active)
			continue;

		if (!r.orphaned && r.haveState &&
		    static_cast<int32_t>(nowMs - r.lastHeardMs) > static_cast<int32_t>(HELI_STALE_MS))
			Orphan(r, nowMs);

		if (r.orphaned) {
			const uint32_t elapsed = nowMs - r.orphanedAtMs;
			const VehicleTransform at = OrphanedHeliAt(r.last, elapsed);
			if (OrphanedHeliIsGone(at, elapsed)) {
				Remove(r);
				continue;
			}
			if (r.Spawned() && m_bridge && m_bridge->PoseHeliReplica) {
				HeliLook look = r.look;
				look.searchLightIntensity = 0.0f;
				look.velocity = {0.0f, 0.0f, HELI_ORPHAN_CLIMB_MPS};
				if (!m_bridge->PoseHeliReplica(r, at, look))
					r.poolHandle = -1;
			}
			continue;
		}

		if (!r.haveState || !m_bridge)
			continue;

		if (!r.Spawned()) {
			if (!m_bridge->RequestHeliModel || !m_bridge->RequestHeliModel())
				continue;
			if (!m_bridge->SpawnHeliReplica || !m_bridge->SpawnHeliReplica(r))
				continue;
			if (!m_saidReplica) {
				m_saidReplica = true;
				Log("heli: built the first replica, player %u's serial %u, ref %d",
				    r.owner, r.serial, r.poolHandle);
			}
		}

		VehicleTransform at;
		if (r.interp.SampleDelayed(nowMs, at))
			r.last = at;
		if (m_bridge->PoseHeliReplica && !m_bridge->PoseHeliReplica(r, r.last, r.look)) {
			r.poolHandle = -1;   // the engine took it; built again next frame
			continue;
		}

		if ((r.look.flags & HELI_FLAG_TAIL_BLOWN) && !r.tailBlownPlayed) {
			r.tailBlownPlayed = true;
			if (m_bridge->BlowTailOffHeliReplica)
				m_bridge->BlowTailOffHeliReplica(r);
		}
	}

	DrawDueShots(nowMs);
}

// After the rows, so a replica built this frame can fire and one removed this
// frame can't.
void HeliSync::DrawDueShots(uint32_t nowMs) {
	for (PendingShot &s : m_shots) {
		if (!s.used || static_cast<int32_t>(nowMs - s.dueMs) < 0)
			continue;
		const PendingShot shot = s;
		s = PendingShot{};

		RemoteHeli *row = FindRow(shot.owner, shot.serial);
		if (!row || !row->Spawned() || row->orphaned || !m_bridge ||
		    !m_bridge->DrawHeliShot)
			continue;
		if (m_bridge->DrawHeliShot(*row, shot.source, shot.target) && !m_saidShotDrawn) {
			m_saidShotDrawn = true;
			Log("heli: drew the first round player %u's helicopter %u fired; its "
			    "damage is the owner's and none is dealt here",
			    shot.owner, shot.serial);
		}
	}
}

void HeliSync::Send(uint8_t localPlayerId, uint32_t nowMs) {
	if (!m_bridge || localPlayerId == INVALID_PLAYER)
		return;

	// What left, first, so a slot the engine has already refilled is told as
	// the end of one helicopter and the start of the next rather than as a
	// helicopter that jumped.
	if (m_bridge->DrainOwnHeliGone) {
		OwnHeliGone gone[8];
		const uint8_t n = m_bridge->DrainOwnHeliGone(gone, 8);
		for (uint8_t i = 0; i < n; ++i) {
			const OwnHeliGone &g = gone[i];
			if (!IsPoliceHeliSlot(g.slot))
				continue;
			OwnHeli &own = m_own[g.slot];
			if (!own.active || own.handle != g.handle)
				continue;   // never streamed; nobody has a replica of it
			SendGone(own, g.slot, g.reason, g.creditPlayerId, g.pos, nowMs);
			own = OwnHeli{};
		}
	}

	OwnHeliSample samples[HELI_POLICE_SLOTS];
	uint8_t n = 0;
	if (m_bridge->SampleOwnHelis)
		n = m_bridge->SampleOwnHelis(samples, HELI_POLICE_SLOTS);

	bool seen[HELI_POLICE_SLOTS] = {};
	for (uint8_t i = 0; i < n; ++i) {
		OwnHeliSample &s = samples[i];
		const uint8_t slot = s.body.slot;
		if (!IsPoliceHeliSlot(slot) || seen[slot])
			continue;
		seen[slot] = true;

		OwnHeli &own = m_own[slot];
		if (own.active && own.handle != s.handle) {
			// Replaced without the detour telling us, which only happens when
			// it isn't installed. The old one is gone, and that is all we know.
			SendGone(own, slot, HELI_GONE_VANISHED, INVALID_PLAYER, own.lastPos, nowMs);
			own = OwnHeli{};
		}
		if (!own.active) {
			own.active = true;
			own.handle = s.handle;
			own.serial = NextSerial();
			Log("heli: our engine built a police helicopter in slot %u; it goes "
			    "out as serial %u",
			    slot, own.serial);
		}
		own.lastPos   = s.body.pos;
		s.body.serial = own.serial;
	}
	for (uint8_t slot = 0; slot < HELI_POLICE_SLOTS; ++slot)
		if (!seen[slot] && m_own[slot].active) {
			SendGone(m_own[slot], slot, HELI_GONE_VANISHED, INVALID_PLAYER,
			         m_own[slot].lastPos, nowMs);
			m_own[slot] = OwnHeli{};
		}

	if (n > 0 && m_stateRate.Ready(nowMs)) {
		for (uint8_t i = 0; i < n; ++i) {
			const uint8_t slot = samples[i].body.slot;
			if (!IsPoliceHeliSlot(slot) || !m_own[slot].active ||
			    m_own[slot].handle != samples[i].handle)
				continue;
			C_HeliState out;
			InitHeader(out, nowMs);
			out.body = samples[i].body;
			Out(out, CH_SNAPSHOT);
		}
	}

	// After the states, so a helicopter that fired on the frame it was first
	// sampled already has its serial.
	SendOwnShots(nowMs);

	// Our hits on theirs. Drained whether or not they go anywhere, so a hit on
	// a row that has gone can't pile up.
	if (m_bridge->DrainLocalHeliHits) {
		LocalHeliHit hits[16];
		const uint8_t h = m_bridge->DrainLocalHeliHits(hits, 16);
		for (uint8_t i = 0; i < h; ++i) {
			const RemoteHeli *row = FindRow(hits[i].owner, hits[i].serial);
			if (!row || !HeliHitIsWorthSending(*row, localPlayerId))
				continue;
			C_HeliHit out;
			InitHeader(out, nowMs);
			out.body               = HeliHitBody{};
			out.body.ownerPlayerId = row->owner;
			out.body.slot          = row->slot;
			out.body.serial        = row->serial;
			out.body.kind          = hits[i].kind;
			out.body.damage        = hits[i].kind == HELI_HIT_ROCKET ? 0 : hits[i].damage;
			if (!IsSaneHeliHit(out.body))
				continue;
			Out(out, CH_EVENT);
			if (!m_saidHitSent) {
				m_saidHitSent = true;
				Log("heli: our first hit on player %u's police helicopter is on its "
				    "way to them",
				    row->owner);
			}
		}
	}
}

void HeliSync::SendOwnShots(uint32_t nowMs) {
	if (!m_bridge->DrainOwnHeliShots)
		return;
	OwnHeliShot shots[16];
	const uint8_t n = m_bridge->DrainOwnHeliShots(shots, 16);
	for (uint8_t i = 0; i < n; ++i) {
		const OwnHeliShot &s = shots[i];
		if (!IsPoliceHeliSlot(s.slot))
			continue;
		const OwnHeli &own = m_own[s.slot];
		if (!own.active || own.handle != s.handle)
			continue;   // never streamed; nobody has a replica to fire it from
		C_HeliShot out;
		InitHeader(out, nowMs);
		out.body        = HeliShotBody{};
		out.body.serial = own.serial;
		out.body.slot   = s.slot;
		out.body.source = s.source;
		out.body.target = s.target;
		if (!IsSaneHeliShot(out.body))
			continue;
		Out(out, CH_SNAPSHOT);
		if (!m_saidShotSent) {
			m_saidShotSent = true;
			Log("heli: our helicopter %u opened fire; its rounds go out for the "
			    "others to see, and the damage stays ours",
			    own.serial);
		}
	}
}

void HeliSync::Clear() {
	for (RemoteHeli &r : m_rows)
		if (r.active)
			Remove(r);
	for (GoneRing &g : m_gone)
		g = GoneRing{};
	for (PendingShot &s : m_shots)
		s = PendingShot{};
	for (OwnHeli &o : m_own)
		o = OwnHeli{};
	if (m_bridge && m_bridge->ResetHeliSession)
		m_bridge->ResetHeliSession();
}

bool HeliSync::IsReplica(int32_t poolHandle) const {
	if (poolHandle < 0)
		return false;
	for (const RemoteHeli &r : m_rows)
		if (r.active && r.poolHandle == poolHandle)
			return true;
	return false;
}

const RemoteHeli *HeliSync::Find(uint8_t owner, uint16_t serial) const {
	for (const RemoteHeli &r : m_rows)
		if (r.active && r.owner == owner && r.serial == serial)
			return &r;
	return nullptr;
}

size_t HeliSync::ActiveCount() const {
	size_t n = 0;
	for (const RemoteHeli &r : m_rows)
		if (r.active)
			++n;
	return n;
}

size_t HeliSync::PendingShotCount() const {
	size_t n = 0;
	for (const PendingShot &s : m_shots)
		if (s.used)
			++n;
	return n;
}

// ---- private ----------------------------------------------------------------

RemoteHeli *HeliSync::FindRow(uint8_t owner, uint16_t serial) {
	return const_cast<RemoteHeli *>(
	    static_cast<const HeliSync *>(this)->Find(owner, serial));
}

void HeliSync::Remove(RemoteHeli &heli) {
	if (heli.Spawned() && m_bridge && m_bridge->DespawnHeliReplica)
		m_bridge->DespawnHeliReplica(heli);
	heli = RemoteHeli{};
}

void HeliSync::Orphan(RemoteHeli &heli, uint32_t nowMs) {
	heli.orphaned     = true;
	heli.orphanedAtMs = nowMs;
	if (!m_saidOrphan) {
		m_saidOrphan = true;
		Log("heli: player %u's helicopter %u %s; it climbs away here and is "
		    "removed",
		    heli.owner, heli.serial,
		    heli.ownerLeft ? "lost its owner" : "stopped being streamed");
	}
}

void HeliSync::SendGone(const OwnHeli &own, uint8_t slot, uint8_t reason,
                        uint8_t credit, const Vec3 &pos, uint32_t nowMs) {
	C_HeliGone out;
	InitHeader(out, nowMs);
	out.body                = HeliGoneBody{};
	out.body.serial         = own.serial;
	out.body.slot           = slot;
	out.body.reason         = reason;
	out.body.creditPlayerId = credit;
	out.body.pos            = pos;
	Out(out, CH_EVENT);
	Log("heli: our helicopter %u is gone (%s)", own.serial,
	    reason == HELI_GONE_SHOT_DOWN ? "shot down"
	    : reason == HELI_GONE_FLEW_AWAY ? "flew away" : "left its slot");
}

uint16_t HeliSync::NextSerial() {
	const uint16_t s = m_nextSerial++;
	if (m_nextSerial == 0)
		m_nextSerial = 1;
	return s;
}

} // namespace coopiii
