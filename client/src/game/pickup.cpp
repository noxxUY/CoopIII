#include "pickup.h"

#include "addresses.h"
#include "hook/hook.h"
#include "log.h"

#include <cmath>
#include <cstring>

namespace coopiii::game {

namespace {

Detour g_update;
Detour g_dropMoney;
Detour g_dropWeapons;

PickupCallbacks g_cb{};
PickupStats     g_stats{};

// One entry per engine slot. Indexed by slot the way the engine is, but
// nothing here ever puts a slot number on the wire - PickupSlot::ident is
// what a reply is matched against. docs/pickups.md 2.
PickupSlot g_slots[NUM_PICKUPS];

// m_pObject, taken away for the duration of one CPickups::Update and put back
// immediately afterwards. A separate array rather than a field on PickupSlot
// so the restore loop is a straight walk over contiguous memory in the one
// place where a missed restore would leave the player's world empty.
uintptr_t g_stashed[NUM_PICKUPS];

uint32_t g_frame = 0;

// Whether the last CPickups::Update pass ran with a live session, so the
// gates are cleared once on the way out rather than every frame.
bool g_seamActive = false;

// Wired *and* connected. See PickupCallbacks::HaveSession for why the second
// half is not redundant and for the log line that gave it away.
bool HaveSession() {
	return g_cb.HaveSession == nullptr || g_cb.HaveSession();
}

// Idents CoopIII removed because somebody else collected them, of the types
// the engine never brings back. See kRemovedRingSize for why this is a list
// of idents and not a flag on the slot.
struct RemovedIdent {
	bool        used = false;
	PickupIdent ident{};
	uint32_t    liveSince = 0;   // frame it was first seen alive again, or 0
};

RemovedIdent g_removed[kRemovedRingSize];
size_t       g_removedNext  = 0;
uint32_t     g_removedCount = 0;   // how many entries are in use

// A log line at most once per cause. docs/compat.md's rule and roadmap.md's:
// fail loudly, once, not sixty times a second.
bool g_saidCollision   = false;
bool g_saidUnusable    = false;
bool g_saidRingFull    = false;
bool g_saidTimeout     = false;
bool g_saidDropMade    = false;
bool g_saidDropTaken   = false;
bool g_saidDropFull    = false;
bool g_saidSuppressed  = false;

// ---------------------------------------------------------------------------
// The engine's pickup table
// ---------------------------------------------------------------------------

uint8_t *PickupAt(size_t slot) {
	return reinterpret_cast<uint8_t *>(CPickups__aPickUps + slot * SIZEOF_PICKUP);
}

uint8_t  PickupType(size_t slot)  { return *(PickupAt(slot) + offs::PICKUP_TYPE); }
uint8_t  Removed(size_t slot)     { return *(PickupAt(slot) + offs::PICKUP_REMOVED); }
int16_t  ModelOf(size_t slot)     { return *reinterpret_cast<int16_t *>(PickupAt(slot) + offs::PICKUP_MODEL); }

uintptr_t &ObjectOf(size_t slot) {
	return *reinterpret_cast<uintptr_t *>(PickupAt(slot) + offs::PICKUP_OBJECT);
}

uint16_t GenerationOf(size_t slot) {
	return *reinterpret_cast<uint16_t *>(PickupAt(slot) + offs::PICKUP_INDEX);
}

uint16_t QuantityOf(size_t slot) {
	return *reinterpret_cast<uint16_t *>(PickupAt(slot) + offs::PICKUP_QUANTITY);
}

uint32_t &TimerOf(size_t slot) {
	return *reinterpret_cast<uint32_t *>(PickupAt(slot) + offs::PICKUP_TIMER);
}

const float *PosOf(size_t slot) {
	return reinterpret_cast<const float *>(PickupAt(slot) + offs::PICKUP_POS);
}

// Types 8..13. Left entirely alone - pickup.h says why.
bool IsMine(uint8_t type) {
	return type >= PICKUP_MINE_INACTIVE && type <= PICKUP_FLOATINGPACKAGE_FLOATING;
}

// A pickup that is there, is not a mine, and could be walked into right now.
bool IsLive(size_t slot) {
	const uint8_t type = PickupType(slot);
	return type != PICKUP_NONE && !IsMine(type) && Removed(slot) == 0 &&
	       ObjectOf(slot) != 0;
}

int16_t ModelIndexGlobal(uintptr_t address) {
	return Global<int16_t>(address);
}

PickupIdent IdentOf(size_t slot) {
	PickupIdent id{};
	const float *p = PosOf(slot);
	id.pos.x      = p[0];
	id.pos.y      = p[1];
	id.pos.z      = p[2];
	id.modelIndex = ModelOf(slot);
	id.type       = PickupType(slot);
	// The server needs one thing the model index cannot tell it: the pickup
	// model indices are runtime globals CModelInfo fills from the IDE, so the
	// number is this install's and nobody else's. A bribe's respawn window is
	// 300 s where everything else's is 720 s, and that is the whole of what
	// the bit is for.
	id.flags      = ModelOf(slot) == ModelIndexGlobal(MI_PICKUP_BRIBE)
	                    ? PICKUP_F_BRIBE
	                    : 0;
	return id;
}

// ---------------------------------------------------------------------------
// The local player's position
// ---------------------------------------------------------------------------
//
// FindPlayerCoors is the engine's own answer and it already handles the case
// this would otherwise get wrong - a player in a car is at the car, not at
// the seat the engine will put them in next frame. It writes into a caller
// supplied CVector and returns it.
using FindPlayerCoorsFn = float *(__cdecl *)(float *out);

using FindPlayerPedFn = void *(__cdecl *)();

bool LocalPlayerPos(float out[3]) {
	// FindPlayerCoors does not check the ped for null - it goes straight to
	// `cmp byte [eax+314h],0` on whatever Players[focus].m_pPed holds. In
	// menus, on a loading screen and before the player exists that is zero,
	// so the null check has to happen here and not there.
	if (Func<FindPlayerPedFn>(FindPlayerPed)() == nullptr)
		return false;
	float buf[3] = {0.0f, 0.0f, 0.0f};
	Func<FindPlayerCoorsFn>(FindPlayerCoors)(buf);
	out[0] = buf[0];
	out[1] = buf[1];
	out[2] = buf[2];
	return true;
}

float Dist2(const float *a, const float *b) {
	const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
	return dx * dx + dy * dy + dz * dz;
}

// ---------------------------------------------------------------------------
// Replaying the engine's own removal on somebody else's collection
// ---------------------------------------------------------------------------

// CPickups::GenerateNewOne(CVector pos, uint32 modelIndex, uint8 type,
//                          uint32 quantity) -> slot | (m_nIndex << 16), or -1.
//
// cdecl, and the CVector goes by value as three floats: the call site at
// 0x00433643 pushes quantity, type, model and then three dwords, and cleans
// up 0x18. The type byte is read out of the low byte of its dword (`cmp al,4`
// at 0x00430587), so a full dword with the value in it is what the engine
// expects.
using GenerateNewOneFn = int32_t(__cdecl *)(float x, float y, float z,
                                            uint32_t modelIndex, uint32_t type,
                                            uint32_t quantity);

using WorldRemoveFn   = void(__cdecl *)(void *entity);
using DeletingDtorFn  = void *(__fastcall *)(void *self, void * /*edx*/, int flags);
using AddCollectedFn  = void(__cdecl *)(int slot);

// The six writes every award arm of CPickup::Update ends in, in the engine's
// own order. Transcribed from the shop tail at 0x00430F9B, the package tail
// at 0x004312F6 and the money tail at 0x0043134F - which are the same six
// writes with different endings, and the ending is the only thing that
// differs by type.
//
// Deliberately NOT CPickups::RemovePickUp (0x004307A0). That one sets
// m_eType = PICKUP_NONE unconditionally and frees the slot, which is right
// for the script's REMOVE_PICKUP and wrong here: a PICKUP_ON_STREET that has
// its type cleared never comes back.
void ReplayEngineRemoval(size_t slot) {
	uintptr_t object = ObjectOf(slot);
	if (object != 0) {
		Func<WorldRemoveFn>(CWorld__Remove)(reinterpret_cast<void *>(object));
		// Vtable slot 0 with flags = 1: the deleting destructor, exactly as
		// `mov ebx,[ecx] / push 1 / call [ebx]` at 0x00430FAC does it. The
		// flag matters - Area B's crash was a ped destroyed through the wrong
		// class's slot 0, which handed a pool pointer to the CRT heap.
		void *const *vtable = *reinterpret_cast<void *const *const *>(object);
		reinterpret_cast<DeletingDtorFn>(vtable[0])(
		    reinterpret_cast<void *>(object), nullptr, 1);
	}

	const uint8_t type  = PickupType(slot);
	const bool    bribe = ModelOf(slot) == ModelIndexGlobal(MI_PICKUP_BRIBE);

	ObjectOf(slot)                                  = 0;
	*(PickupAt(slot) + offs::PICKUP_REMOVED)        = 1;

	const uint32_t window = PickupRespawnMs(type, bribe);
	if (window != 0) {
		// Our own clock, not the collector's. The duration is a constant per
		// type; the deadline is an absolute stamp on a local
		// CTimer::m_snTimeInMilliseconds and must never travel.
		// docs/pickups.md 5.
		TimerOf(slot) = Global<uint32_t>(CTimer__m_snTimeInMilliseconds) + window;
	} else {
		// ONCE / ONCE_TIMEOUT / COLLECTABLE1 / MONEY all end in the inline
		// Remove(), which frees the slot for good.
		*(PickupAt(slot) + offs::PICKUP_TYPE) = PICKUP_NONE;
	}
}

// The ring HAS_PICKUP_BEEN_COLLECTED reads, and the whole reason rampages and
// hidden packages can be shared for free: every machine is running rampage.sc
// and rewards.sc already, so pushing the collection in here makes this
// machine's own script produce every consequence by itself.
// docs/pickups.md 6.
void TellTheScript(size_t slot) {
	Func<AddCollectedFn>(CPickups__AddToCollectedPickupsArray)(static_cast<int>(slot));
}

// A hidden package collected by anybody counts for everybody - roadmap.md 5.11.
// The only reward an observer ever applies, and it is the engine's own two
// writes out of the COLLECTABLE1 arm at 0x0043126A / 0x00431270, on this
// machine's single CPlayerInfo.
void CountPackageForUs() {
	// CWorld::Players is an inline array of CPlayerInfo, not an array of
	// pointers - m_pPed happens to be its first member, which is why
	// FindPlayerPed can load straight out of it. So this is the address of
	// the CPlayerInfo, not something to dereference.
	const uint8_t focus = Global<uint8_t>(CWorld__PlayerInFocus);
	uint8_t *info = reinterpret_cast<uint8_t *>(
	    CWorld__Players + focus * offs::PLAYERINFO_STRIDE);
	*reinterpret_cast<int32_t *>(info + offs::PLAYERINFO_COLLECTED_PACKAGES) += 1;
	*reinterpret_cast<int32_t *>(info + offs::PLAYERINFO_MONEY) += 1000;
}

// ---------------------------------------------------------------------------
// Finding a slot by ident
// ---------------------------------------------------------------------------

// Nearest live pickup of the same model within the tolerance, or NUM_PICKUPS.
//
// Nearest rather than first, so a collision is at least deterministic; and
// counted, so two live pickups that both match get refused rather than
// arbitrarily resolved. docs/pickups.md 7.
size_t FindSlot(const PickupIdent &ident, bool requireLive) {
	size_t best      = NUM_PICKUPS;
	float  bestDist  = kIdentToleranceSq;
	int    matches   = 0;
	const float want[3] = {ident.pos.x, ident.pos.y, ident.pos.z};

	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		if (PickupType(i) == PICKUP_NONE || ModelOf(i) != ident.modelIndex)
			continue;
		if (requireLive && !IsLive(i))
			continue;
		const float d = Dist2(PosOf(i), want);
		if (d > kIdentToleranceSq)
			continue;
		++matches;
		if (d <= bestDist) {
			bestDist = d;
			best     = i;
		}
	}

	if (matches > 1) {
		++g_stats.identCollisions;
		if (!g_saidCollision) {
			g_saidCollision = true;
			Log("pickup: %d live pickups of model %d within %.2f m of "
			    "(%.2f %.2f %.2f). Refusing to guess which one the session "
			    "means; this one stays local.",
			    matches, ident.modelIndex, kIdentTolerance, want[0], want[1],
			    want[2]);
		}
		return NUM_PICKUPS;
	}
	return best;
}

// ---------------------------------------------------------------------------
// Noticing that the script has put one back
// ---------------------------------------------------------------------------
//
// The server's record for a pickup with no respawn window is a lock, not a
// tombstone, because the script reuses coordinates: every one of the 20
// rampages is destroyed and re-created at its original spot after two
// failures, and the Ammu-Nation counter swaps its in-stock and out-of-stock
// pickups. Somebody has to tell the server the key is live again, and the
// only machines that can are the ones that removed it.

void RememberRemoved(const PickupIdent &ident) {
	// Respawning types need nothing: the server's own clock lets them go.
	if (PickupRespawnMs(ident.type, (ident.flags & PICKUP_F_BRIBE) != 0) != 0)
		return;

	for (RemovedIdent &r : g_removed)
		if (r.used && SameIdent(r.ident, ident))
			return;   // already watching it

	RemovedIdent &slot = g_removed[g_removedNext];
	if (slot.used && !g_saidRingFull) {
		g_saidRingFull = true;
		Log("pickup: more than %zu permanently-removed pickups are being "
		    "watched for the script putting them back; the oldest is being "
		    "dropped and its key may stay locked on the server",
		    kRemovedRingSize);
	}
	if (!slot.used)
		++g_removedCount;
	slot.used      = true;
	slot.ident     = ident;
	slot.liveSince = 0;
	g_removedNext  = (g_removedNext + 1) % kRemovedRingSize;
}

void ForgetRemoved(RemovedIdent &r) {
	r = RemovedIdent{};
	if (g_removedCount > 0)
		--g_removedCount;
}

// ---------------------------------------------------------------------------
// Ped drops
// ---------------------------------------------------------------------------
//
// pickup.h's middle section is the argument. The code is three things: decide
// whether this machine is entitled to make the drop, let the engine make it,
// and read back what it made.

using DropFn = void(__fastcall *)(void *self, void * /*edx*/);

// Slot identity before the engine ran, so what it created can be read off the
// difference. The generation counter is in here and not just the type because
// GenerateNewOne does not only fill *free* slots: when [0,320) is full it
// scans for a PICKUP_MONEY to overwrite and then for a PICKUP_ONCE_TIMEOUT
// (0x0043051D and 0x0043053D), and both of those are exactly the types a
// drop makes. A type-only diff would miss every drop made under pressure,
// which is the case where the drops are happening fastest.
struct SlotStamp {
	uint8_t  type       = PICKUP_NONE;
	uint16_t generation = 0;
};

SlotStamp g_beforeDrop[NUM_PICKUPS];

void StampPickupTable() {
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		g_beforeDrop[i].type       = PickupType(i);
		g_beforeDrop[i].generation = GenerationOf(i);
	}
}

// Everything that is in the table now and was not the same pickup before.
void AnnounceWhatTheEngineMade(const char *what) {
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		const uint8_t type = PickupType(i);
		if (type == PICKUP_NONE)
			continue;
		if (type == g_beforeDrop[i].type &&
		    GenerationOf(i) == g_beforeDrop[i].generation)
			continue;

		// The slot has been recycled under us. Whatever gate the previous
		// occupant left behind names a pickup that no longer exists, and a
		// stale GRANTED would report a collection nobody made on the next
		// pass.
		g_slots[i] = PickupSlot{};

		PickupDropBody drop{};
		drop.ident    = IdentOf(i);
		drop.quantity = QuantityOf(i);

		const bool sent = g_cb.Dropped(drop);
		if (sent)
			++g_stats.dropsMade;
		else
			++g_stats.dropsUnsent;

		if (!g_saidDropMade) {
			g_saidDropMade = true;
			Log("pickup: one of our pedestrians dropped something - %s, model "
			    "%d, type %u, %u of it, at (%.1f %.1f %.1f). %s",
			    what, static_cast<int>(drop.ident.modelIndex),
			    static_cast<unsigned>(drop.ident.type),
			    static_cast<unsigned>(drop.quantity), drop.ident.pos.x,
			    drop.ident.pos.y, drop.ident.pos.z,
			    sent ? "The session was told, so everyone else gets one too"
			         : "There is no session to tell, so it is ours alone - "
			           "which is single player working exactly as it always "
			           "did");
		}
	}
}

// Is this ped one CoopIII built to stand in for somebody else's?
//
// Nothing in the pool can answer it. A remote player's ped and an ambient
// replica are both CCivilianPed and both MISSION_CHAR, which is exactly what
// a script ped is - and a script ped's drop is the local engine's own
// business and must keep working. Only the roster knows, so only the client
// half can say.
using GetPedRefFn = int32_t(__cdecl *)(void *ped);

bool TheirPed(void *ped) {
	if (g_cb.IsReplicatedPed == nullptr)
		return false;
	return g_cb.IsReplicatedPed(Func<GetPedRefFn>(CPools__GetPedRef)(ped));
}

// The three facts DecideDrop needs, fetched, and the answer counted.
//
// The suppression is the REFUSE arm and it is a seam rather than a race: it
// is a straight-line decision taken before the engine's creator is entered,
// on the game thread, inside the single call that leads to it. There is no
// window in which two machines could both be creating, because the machine
// that must not never reaches the code that would.
//
// The narrowness of ANNOUNCE is deliberate. A *script* ped's drop is left
// exactly as it is today: every machine runs main.scm, every machine has its
// own copy of that ped, and announcing one machine's would hand the others a
// second pickup beside the one their own copy will leave later. That is M5's
// problem and this is not the place to half-solve it.
DropDecision DecideFor(void *ped) {
	const uint8_t createdBy =
	    *(reinterpret_cast<const uint8_t *>(ped) + offs::PED_CHAR_CREATED_BY);
	const DropDecision d =
	    DecideDrop(TheirPed(ped), g_cb.Dropped != nullptr && HaveSession(),
	               createdBy);

	if (d == DropDecision::REFUSE) {
		++g_stats.dropsSuppressed;
		if (!g_saidSuppressed) {
			g_saidSuppressed = true;
			Log("pickup: refused a drop our own engine tried to make for a "
			    "ped that belongs to somebody else. Their machine decides "
			    "what their ped leaves behind, exactly as ours decides "
			    "ours - and for a player that means nothing at all, which "
			    "is what their own engine already does.");
		}
	}
	return d;
}

void __fastcall HookedCreateDeadPedMoney(void *self, void * /*edx*/) {
	const DropDecision d = DecideFor(self);

	// Nothing to stand in for on the refusal: this function's own fourth gate
	// is `cmp byte [ebx+160h],2 / je ret`, so every ped being refused here
	// would have been refused two instructions in. The arm exists because the
	// rule should be stated once and hold for both creators, not because the
	// engine would have got this one wrong.
	if (d == DropDecision::REFUSE)
		return;

	if (d == DropDecision::LOCAL) {
		g_dropMoney.Original<DropFn>()(self, nullptr);
		return;
	}

	StampPickupTable();
	g_dropMoney.Original<DropFn>()(self, nullptr);
	AnnounceWhatTheEngineMade("money");
}

void __fastcall HookedCreateDeadPedWeaponPickups(void *self, void * /*edx*/) {
	const DropDecision d = DecideFor(self);

	if (d == DropDecision::REFUSE) {
		// The tail of the function we are refusing still has to run.
		// CreateDeadPedWeaponPickups ends in CPed::ClearWeapons (the call at
		// 0x004339CB), and skipping it would leave a corpse holding an
		// inventory the engine meant to empty - a difference between this
		// machine's copy of a dead player and every other machine's, in the
		// one place this whole change exists to make them agree.
		Func<DropFn>(CPed__ClearWeapons)(self, nullptr);
		return;
	}

	if (d == DropDecision::LOCAL) {
		g_dropWeapons.Original<DropFn>()(self, nullptr);
		return;
	}

	StampPickupTable();
	g_dropWeapons.Original<DropFn>()(self, nullptr);
	AnnounceWhatTheEngineMade("a weapon");
}

// ---------------------------------------------------------------------------
// The detour
// ---------------------------------------------------------------------------

using UpdateFn = void(__cdecl *)();

void __cdecl HookedPickupsUpdate() {
	// No session, or the seam is not wired: do nothing at all, so single
	// player behaves exactly as it always did (roadmap.md 5.6's spirit).
	//
	// The connection test is the load-bearing half and it was missing. With
	// the callbacks wired and the socket down, every pickup in the world was
	// hidden from the engine and claimed into nothing - a player with
	// CoopIII.asi installed and no server running could not collect
	// anything. Measured in a live game, where it printed the claim-timeout
	// line once and then sat there.
	if (g_cb.Claim == nullptr || !HaveSession()) {
		// Clear the gates once per transition, not every frame, so nothing
		// is left half-claimed and nothing is pointlessly rewritten sixty
		// times a second. The engine's own table is untouched either way -
		// all g_slots holds is CoopIII's gate.
		if (g_seamActive) {
			g_seamActive = false;
			for (PickupSlot &s : g_slots)
				s = PickupSlot{};
		}
		g_update.Original<UpdateFn>()();
		return;
	}
	g_seamActive = true;

	++g_frame;

	float player[3];
	const bool havePlayer = LocalPlayerPos(player);

	uint32_t blocked = 0;

	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		g_stashed[i] = 0;

		const uint8_t type = PickupType(i);
		if (type == PICKUP_NONE || IsMine(type))
			continue;

		PickupSlot &slot = g_slots[i];

		// The script may have re-created something we removed on somebody
		// else's behalf, in whichever slot happened to be free. Checked by
		// ident, and only while there is anything to check - the usual cost
		// of this block is one integer compare for the whole pass.
		if (g_removedCount != 0 && IsLive(i)) {
			const PickupIdent here = IdentOf(i);
			for (RemovedIdent &r : g_removed) {
				if (!r.used || !SameIdent(r.ident, here))
					continue;
				if (r.liveSince == 0) {
					r.liveSince = g_frame;
					break;
				}
				if (g_frame - r.liveSince < kRecreateConfirmFrames)
					break;
				// The server is holding a record for a key that is live
				// again. docs/pickups.md 7.
				ForgetRemoved(r);
				slot.gate = PickupGate::BLOCKED;
				if (g_cb.Release) {
					++g_stats.releases;
					g_cb.Release(here);
				}
				break;
			}
		}

		if (slot.gate == PickupGate::GRANTED) {
			// Left unblocked, so the engine may take it this pass if the
			// player is actually on it and its own CanBePickedUp agrees.
			// What happened is read back after the trampoline returns.
			continue;
		}

		// A claim that was never answered. Reliable and ordered, so this only
		// fires on a real fault - but a pickup that silently stops working
		// forever is worse than a retry.
		if (slot.gate == PickupGate::CLAIMED &&
		    g_frame - slot.claimedFrame > kClaimTimeoutFrames) {
			slot.gate = PickupGate::BLOCKED;
			++g_stats.claimTimeouts;
			if (!g_saidTimeout) {
				g_saidTimeout = true;
				Log("pickup: a claim for model %d went unanswered for %u "
				    "frames and is being made again. The event channel is "
				    "reliable, so this means the session is in trouble.",
				    ModelOf(i), kClaimTimeoutFrames);
			}
		}

		if (slot.gate == PickupGate::BLOCKED && havePlayer && IsLive(i) &&
		    Dist2(PosOf(i), player) <= kClaimRadiusSq) {
			const PickupIdent ident = IdentOf(i);
			if (FindSlot(ident, true) == i) {   // refuses an ambiguous ident
				slot.ident        = ident;
				slot.gate         = PickupGate::CLAIMED;
				slot.claimedFrame = g_frame;
				++g_stats.claimsSent;
				g_cb.Claim(ident);
			}
		}

		// Everything that is not GRANTED is hidden from the engine.
		if (ObjectOf(i) != 0) {
			g_stashed[i] = ObjectOf(i);
			ObjectOf(i)  = 0;
			++blocked;
		}
	}

	g_stats.blockedPerPass = blocked;

	g_update.Original<UpdateFn>()();

	// Put every one of them back, unconditionally and without a single
	// early-out. A missed restore is a pickup the player's own world has
	// lost, and it would look like a rendering bug.
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		if (g_stashed[i] != 0) {
			ObjectOf(i)  = g_stashed[i];
			g_stashed[i] = 0;
		}
	}

	// What became of the reservations we were holding.
	//
	// This is where CoopIII finds out whether a pickup was collected, and it
	// finds out by *reading* rather than by deciding: the engine ran its own
	// touch test, its own CanBePickedUp and its own award switch on an
	// unblocked pickup, and an empty slot afterwards is what it did.
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		PickupSlot &slot = g_slots[i];
		if (slot.gate != PickupGate::GRANTED)
			continue;

		if (!IsLive(i)) {
			// Taken. (Or, far more rarely, removed by the script in the same
			// frame - in which case the session records a collection nobody
			// made, which costs one pickup and no correctness.)
			slot.gate = PickupGate::BLOCKED;
			++g_stats.collected;

			// Once, and then never again. Every other seam in this project
			// says so the first time it works - the first hit off the wire,
			// the first fire, the first arrow, the first replicated driver -
			// and this one only ever spoke when something went wrong. A
			// success that looks exactly like a feature that was never
			// installed is the failure mode this project keeps paying for.
			static bool saidCollected = false;
			if (!saidCollected) {
				saidCollected = true;
				Log("pickup: took our first pickup - slot %zu, model %d, at "
				    "(%.0f %.0f %.0f). The session was told, so nobody else "
				    "gets this one",
				    i, static_cast<int>(slot.ident.modelIndex),
				    slot.ident.pos.x, slot.ident.pos.y, slot.ident.pos.z);
			}

			if (g_cb.Collected)
				g_cb.Collected(slot.ident);
			continue;
		}

		// Still there. Either the player is near it and has not stepped on
		// it, or the engine refused it - full health, no wanted level, a
		// frenzy already running. Hold the reservation while they are in
		// range and give it back the moment they are not, because a
		// reservation nobody gives back is a pickup nobody else can have.
		if (!havePlayer || Dist2(PosOf(i), player) > kClaimRadiusSq) {
			slot.gate = PickupGate::BLOCKED;
			++g_stats.walkedAway;
			if (g_cb.Release) {
				++g_stats.releases;
				g_cb.Release(slot.ident);
			}
		}
	}
}

}  // namespace

// ---------------------------------------------------------------------------

bool SameIdent(const PickupIdent &a, const PickupIdent &b) {
	if (a.modelIndex != b.modelIndex)
		return false;
	const float dx = a.pos.x - b.pos.x;
	const float dy = a.pos.y - b.pos.y;
	const float dz = a.pos.z - b.pos.z;
	return dx * dx + dy * dy + dz * dz <= kIdentToleranceSq;
}

void SetPickupCallbacks(const PickupCallbacks &callbacks) { g_cb = callbacks; }

const PickupStats &GetPickupStats() { return g_stats; }

bool InstallPickupHook() {
	for (PickupSlot &s : g_slots)
		s = PickupSlot{};
	for (uintptr_t &o : g_stashed)
		o = 0;
	for (RemovedIdent &r : g_removed)
		r = RemovedIdent{};
	g_removedNext  = 0;
	g_removedCount = 0;
	g_stats = PickupStats{};

	if (g_update.Install("CPickups::Update",
	                     reinterpret_cast<void *>(CPickups__Update),
	                     reinterpret_cast<void *>(&HookedPickupsUpdate))) {
		Log("pickup: hooked CPickups::Update at 0x%08X, %zu slots of %zu bytes "
		    "at 0x%08X",
		    CPickups__Update, NUM_PICKUPS, SIZEOF_PICKUP, CPickups__aPickUps);
		return true;
	}

	Log("pickup: FAILED to hook CPickups::Update at 0x%08X - pickups will be "
	    "local to each machine and two players can take the same one",
	    CPickups__Update);
	for (const auto &f : HookFailures())
		Log("pickup:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemovePickupHook() {
	ReleaseAllPickups();
	g_cb = PickupCallbacks{};
	g_update.Remove();
}

bool PickupHookInstalled() { return g_update.IsInstalled(); }

bool InstallPedDropHooks() {
	for (SlotStamp &s : g_beforeDrop)
		s = SlotStamp{};

	const bool money = g_dropMoney.Install(
	    "CPed::CreateDeadPedMoney",
	    reinterpret_cast<void *>(CPed__CreateDeadPedMoney),
	    reinterpret_cast<void *>(&HookedCreateDeadPedMoney));
	const bool weapons = g_dropWeapons.Install(
	    "CPed::CreateDeadPedWeaponPickups",
	    reinterpret_cast<void *>(CPed__CreateDeadPedWeaponPickups),
	    reinterpret_cast<void *>(&HookedCreateDeadPedWeaponPickups));

	if (money && weapons) {
		Log("pickup: hooked the two ped-drop creators at 0x%08X and 0x%08X - "
		    "what our pedestrians leave behind is shared, and a remote "
		    "player's weapons stop appearing on our pavement and nobody "
		    "else's",
		    CPed__CreateDeadPedMoney, CPed__CreateDeadPedWeaponPickups);
		return true;
	}

	// Say which half failed, because the two failures look nothing alike.
	// Without the money hook a dead pedestrian's cash is one machine's only.
	// Without the weapon hook the *existing* bug stays: every observer puts a
	// gun on the ground where a remote player died and the player's own
	// machine does not.
	Log("pickup: FAILED to hook the ped-drop creators (money %s, weapons %s). "
	    "Ped drops stay local, and an observer will still drop a remote "
	    "player's weapons that nobody else has",
	    money ? "ok" : "FAILED", weapons ? "ok" : "FAILED");
	for (const auto &f : HookFailures())
		Log("pickup:   %s: %s", f.name.c_str(), f.reason.c_str());
	return false;
}

void RemovePedDropHooks() {
	g_dropMoney.Remove();
	g_dropWeapons.Remove();
}

bool PedDropHooksInstalled() {
	return g_dropMoney.IsInstalled() && g_dropWeapons.IsInstalled();
}

bool OnPickupGrantedToUs(const PickupIdent &ident) {
	const size_t slot = FindSlot(ident, true);
	if (slot == NUM_PICKUPS) {
		// The object went away between the claim and the answer - our own
		// engine respawned it late, or the script removed it. Never sit on a
		// grant we cannot consume: hand it straight back so the pickup is
		// available to whoever can.
		++g_stats.grantsUnusable;
		if (g_cb.Release) {
			++g_stats.releases;
			g_cb.Release(ident);
		}
		if (!g_saidUnusable) {
			g_saidUnusable = true;
			Log("pickup: granted model %d at (%.2f %.2f %.2f) and there is "
			    "nothing there to take. Released it back to the session.",
			    ident.modelIndex, ident.pos.x, ident.pos.y, ident.pos.z);
		}
		return false;
	}

	++g_stats.grants;
	g_slots[slot].gate  = PickupGate::GRANTED;
	g_slots[slot].ident = ident;
	return true;
}

void OnPickupTakenByOther(const PickupIdent &ident) {
	const size_t slot = FindSlot(ident, true);
	if (slot == NUM_PICKUPS) {
		// Already gone here, or not created yet. Either way there is nothing
		// to remove and nothing has gone wrong: our own engine will have set
		// its own respawn timer if it collected it, and a pickup that does
		// not exist here cannot be taken here.
		return;
	}

	ReplayEngineRemoval(slot);
	TellTheScript(slot);

	// A hidden package counts for everybody - roadmap.md 5.11. The type is on
	// the wire, so an observer needs no model-index lookup to know.
	if (ident.type == PICKUP_COLLECTABLE1)
		CountPackageForUs();

	g_slots[slot].gate = PickupGate::BLOCKED;
	RememberRemoved(ident);
	++g_stats.remoteRemovals;
}

void OnPickupDroppedElsewhere(const PickupDropBody &drop) {
	// Already here. Two ways to get one: a duplicate packet, or - far more
	// interestingly - the local script having put an identical pickup at the
	// same spot. Either way making a second is worse than making none, since
	// two live pickups within the ident tolerance are a pair FindSlot will
	// refuse to tell apart for the rest of their lives.
	if (FindSlot(drop.ident, true) != NUM_PICKUPS) {
		++g_stats.dropsDuplicate;
		return;
	}

	// The engine's own creator, with the numbers the owner's engine wrote
	// into its own table. Nothing here re-derives a scatter angle, a ground
	// height, a money roll or an ammo cap: all four were decided once, on the
	// machine that owns the pedestrian, and this is a transcription of the
	// result rather than a second attempt at it.
	//
	// The model-index form, not the weapon-type one: GenerateNewOne_WeaponType
	// is eleven instructions that map a weapon to a model and tail-call this,
	// and the model is what the owner read out of the slot, so the mapping
	// has already happened.
	const int32_t handle = Func<GenerateNewOneFn>(CPickups__GenerateNewOne)(
	    drop.ident.pos.x, drop.ident.pos.y, drop.ident.pos.z,
	    static_cast<uint32_t>(static_cast<uint16_t>(drop.ident.modelIndex)),
	    static_cast<uint32_t>(drop.ident.type),
	    static_cast<uint32_t>(drop.quantity));

	if (handle < 0) {
		// The general range is 320 slots and GenerateNewOne has already tried
		// to evict a money pickup and then a timed-out one before giving up,
		// so this means the table is genuinely full. The drop is lost here
		// and exists elsewhere, which is a divergence and worth one line.
		++g_stats.dropsLost;
		if (!g_saidDropFull) {
			g_saidDropFull = true;
			Log("pickup: the pickup table had no room for a drop somebody "
			    "else's pedestrian left (model %d). It exists on their "
			    "machine and not on ours.",
			    static_cast<int>(drop.ident.modelIndex));
		}
		return;
	}

	// GenerateNewOne returns `slot | (m_nIndex << 16)`. The slot may have been
	// recycled off something we had a gate on, so clear it - the same reason
	// the announcing side does.
	const size_t slot = static_cast<size_t>(handle) & 0xFFFFu;
	if (slot < NUM_PICKUPS)
		g_slots[slot] = PickupSlot{};

	++g_stats.dropsReceived;
	if (!g_saidDropTaken) {
		g_saidDropTaken = true;
		Log("pickup: built our first drop off the wire - model %d, type %u, "
		    "%u of it, in slot %zu at (%.1f %.1f %.1f). It is an ordinary "
		    "pickup from here and only one of us gets it",
		    static_cast<int>(drop.ident.modelIndex),
		    static_cast<unsigned>(drop.ident.type),
		    static_cast<unsigned>(drop.quantity), slot, drop.ident.pos.x,
		    drop.ident.pos.y, drop.ident.pos.z);
	}
}

void OnPickupDenied(const PickupIdent &ident) {
	++g_stats.denials;
	for (size_t i = 0; i < NUM_PICKUPS; ++i) {
		if (g_slots[i].gate == PickupGate::CLAIMED &&
		    SameIdent(g_slots[i].ident, ident)) {
			g_slots[i].gate = PickupGate::BLOCKED;
			return;
		}
	}
}

void ReleaseAllPickups() {
	// Nothing to undo in the engine: the only lasting change CoopIII makes to
	// the pickup table is the removal in OnPickupTakenByOther, which is what
	// the collecting machine's own engine did too and which the game will
	// respawn on its own. What this clears is the gate, so that with no
	// session every pickup is simply the local engine's business again.
	// The callbacks are deliberately left alone. ReleaseAllPickups is called
	// on every welcome as well as on disconnect, and clearing them there
	// would silently turn the seam off for the session that had just started.
	// Unwiring belongs to RemovePickupHook.
	for (PickupSlot &s : g_slots)
		s = PickupSlot{};
	for (uintptr_t &o : g_stashed)
		o = 0;
	for (RemovedIdent &r : g_removed)
		r = RemovedIdent{};
	g_removedNext  = 0;
	g_removedCount = 0;
	// So the next pass with a live session arms cleanly rather than thinking
	// it is still mid-session.
	g_seamActive   = false;
}

void AddPickupsToBridge(WorldBridge &bridge) {
	bridge.PickupGrantedToUs  = &OnPickupGrantedToUs;
	bridge.PickupTakenByOther = &OnPickupTakenByOther;
	bridge.PickupDenied       = &OnPickupDenied;
	bridge.PickupsReset       = &ReleaseAllPickups;
	bridge.PickupDropped      = &OnPickupDroppedElsewhere;
}

}  // namespace coopiii::game
