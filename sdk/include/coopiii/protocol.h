// Wire contract shared by client/ and server/. Layouts are pinned down by the
// static_asserts at the bottom. Update docs/protocol.md first, then this file.
#pragma once

#include <cstddef>
#include <cstdint>

namespace coopiii {

// 2: PlayerStateBody grew the animation block (animId/animTime/animSpeed,
//    plus a partial-animation id and time) and a flags byte. See
//    docs/protocol.md §1.8.
// 3: EnterVehicleBody gained the identity fields a client sends when it's
//    first into a car (see that struct). Server flat-out rejects any other
//    version, so an old client fails to connect instead of misreading a
//    packet.
// 4: C_PlayerModel / S_PlayerModel added. A player's model can change after
//    joining, so the join packet isn't the last word on it anymore.
// 5: combat. ShotBody grew `speed` so a thrown projectile's initial velocity
//    makes it across the wire, plus C_Explosion / S_Explosion (only the
//    projectile's owner gets to say where it went off). docs/protocol.md
//    §1.9.
// 6: PlayerStateBody gained animGroup. GTA III has no sideways-walk
//    animation id; strafing is a different group entirely, with its own
//    walk and run, same for every armed stance. See that field below.
// 7: damage, death and respawn. DamageBody grew `direction`, S_Welcome grew
//    `flags` so a client knows whether the session has friendly fire on, and
//    C_Death was added because the shapes reserved at version 5 had the
//    server announcing a death it has no way of knowing about.
//    docs/protocol.md §1.10.
// 8: time of day and weather follow the host's game instead of the server's
//    synthetic clock. C_WorldState added, S_WorldState and S_Welcome carry
//    hostPlayerId and the second weather type. docs/protocol.md §2.7.
// 9: late joiners. The backfill used to rebuild everything from its *spawn
//    identity* and nothing from its *current condition*, so a player who
//    joined mid-session got a different world from everyone who had been
//    there. S_PlayerJoin grew health, armour, weapon, a flags byte and the
//    death animation; S_VehicleSpawn grew health and flags; VehicleFlags
//    grew VEH_WRECKED. No new opcodes. docs/protocol.md 2.8.
// 10: two vehicle changes, both about two machines ending up with the same
//    car. docs/protocol.md 1.11 and 1.12. Landed alongside 9 rather than
//    after it - both were built in parallel and both claimed 9, so the pair
//    was reconciled here into one number.
//
//    - A car's destruction travels. C_VehicleBlowUp / S_VehicleBlowUp added
//      (opcodes 0x36/0x37), because a car exploding is an event and
//      m_fHealth is only a number - writing zero into an observer's copy
//      never ran the engine's destruction path, so the blast and the wreck
//      happened on one screen and not the other.
//    - A car's extras travel, next to its colours and for the same reason
//      the colours are there. EnterVehicleBody's spare `pad` byte became
//      `extra1`/`extra2`, and S_VehicleSpawn grew the same pair. The engine
//      rolls them per machine at spawn, so without this each player sees
//      extras the other does not.
//
// 11: pickups become exclusive. Four opcodes in the reserved 0x80..0x8F
//    pickup block: C_PickupClaim, S_PickupTaken, S_PickupDenied and
//    C_PickupRelease. docs/pickups.md is the design and docs/protocol.md
//    1.13 is the wire.
//
//    Nothing existing moved. Every machine already creates every script
//    pickup for itself (docs/pickups.md 1), so there is no spawn packet and
//    no pickup snapshot - what was missing was only the arbitration, and
//    that is six reliable events: claim, grant or deny, then collected or
//    released. A grant is a reservation and only a collection removes the
//    pickup from anybody else's world, which is what keeps a player who
//    merely walks past one from deleting it for everybody.
//
// 12: the city's traffic is shared, the way its pedestrians already are.
//    docs/population.md 3 step 4. Six opcodes in the block step 2 reserved
//    for them plus two more it did not know it would need:
//
//    - C_CarSpawn / S_CarSpawn / C_CarDespawn / S_CarDespawn (0x76..0x79),
//      the same temporary-id handshake as a pedestrian's, because the
//      machine whose engine made the car cannot name it.
//    - C_CarStates / S_CarStates (0x7A/0x7B), which a pedestrian did not
//      need. An ambient ped is created where the session says and left
//      there; a traffic car is going somewhere, and a replica left where it
//      was born is a permanent roadblock in the middle of a junction on
//      every other screen. So hosted cars carry a transform stream.
//
//      It is batched and it is not the player rate. One packet carries up
//      to 8 cars, nearest to the sender's own player first, at 10 Hz -
//      population.md 2.1 says a flat player rate for ambient entities is a
//      LAN toy, and it says so hardest about cars. Nearest-first is the
//      first slice of 2.1's rate-by-distance: the cars somebody is about to
//      drive into are the ones that get the bandwidth.
// 13: a replicated pedestrian stops being a statue, and a traffic driver
//    stops standing in the road. docs/population.md 3 step 6. Two opcodes:
//
//    - C_PedStates / S_PedStates (0x7C/0x7D), the ped equivalent of the
//      car stream and deliberately cheaper than it. Step 2 shipped
//      pedestrians with no stream at all - created where the session said
//      and left there forever - because "does it appear and stay where it
//      is put" was the whole of that step. It appeared, it stayed, and a
//      city of statues is what that looks like.
//
//      Twelve peds per packet at 10 Hz, nearest the sender's own player
//      first, 24 bytes each: 2.9 KB/s each way per observer against the
//      traffic stream's 3.5. A ped carries less than a car because it is
//      slower and smaller, not because it matters less - see
//      AmbientPedState for what was left out and why.
//
//    - The same packet carries `vehicleNetId`, which is how a traffic
//      driver gets into his car. Both halves are hosted by the same
//      machine and both already have netIds, so the pairing is something
//      the host knows and nobody else can work out. It rides the stream
//      rather than a one-shot event for the reason the player seating work
//      found the hard way (docs/protocol.md 2.8.2): a standing fact that
//      is restated is immune to every race an event has to handle by hand.
// 14: what a dead pedestrian leaves on the pavement. docs/pickups.md 10.
//    One opcode pair, C_PickupDrop / S_PickupDrop (0x86/0x87), out of the
//    0x86-0x8F range the pickup work reserved for exactly this.
//
//    The pickup design needed no spawn packet because every machine runs
//    main.scm and creates all 448 script pickups itself, from literal
//    coordinates. A ped drop is the one pickup that is not in the script,
//    and it is made by one machine only: CPed::SetDead's two creators are
//    refused for a MISSION_CHAR (money) and find an empty inventory (the
//    weapons) on every replica CoopIII builds, so the drop already happens
//    exactly once in the session - on the machine that hosts the ped. What
//    was missing is that the other machines never heard about it.
//
//    So this is not an arbitration packet and it does not decide anything.
//    It is the host reading back what its own engine created and saying so,
//    one packet per pickup, on the reliable channel; every observer runs
//    CPickups::GenerateNewOne with those exact arguments and the pickup then
//    goes through the ordinary claim/grant/collect exchange like any other.
//    The quantity has to travel because PickupIdent does not carry one and
//    the money amount is rolled from CGeneral::GetRandomNumber on the host.
// 15: a car nobody is driving gets somebody to report it. docs/roadmap.md
//    5.8, docs/protocol.md 1.14. One opcode pair, C_UnownedBlowUp /
//    S_UnownedBlowUp (0x38/0x39), nine and thirteen bytes, and a car
//    generator index for a name because the map hands that out and the
//    server does not have to.
//
//    Smaller than it looks, on purpose. Most of an unowned car's
//    destruction already travelled and nobody had noticed: an explosion is
//    replayed at an agreed position and damages every car in its radius
//    with a multiplier that depends only on distance, so a parked car blown
//    up by a rocket is already a wreck everywhere. What does not converge
//    is damage that accumulates - gunfire spread is rolled per machine, a
//    collision with a replica is not a collision anybody simulated twice -
//    and the engine's five-second fire timer turns a difference in health
//    into a difference in whether the car ever explodes. This is the
//    backstop for those, not the mechanism for the common case.
//
//    Built in parallel with 14 and it claimed that number too; this is the
//    reconciliation, the same way 9 and 10 were separated when the
//    late-joiner and vehicle work collided. Nothing in 14 moved.
// 16: C_UnownedBlowUp / S_UnownedBlowUp grew a BlastTransform, so a traffic
//    car explodes in the same street on every screen. Nine and thirteen
//    bytes become thirty-seven and forty-one.
//
//    Version 15 shipped the opcode pair with a key and nothing else, on the
//    written argument that a replica "is already being corrected to the
//    host's stream every frame, which is more current than anything this
//    packet could have carried". Play disagreed, and the argument has one
//    hole: the host stops streaming a car the frame it becomes a wreck, so
//    the newest AmbientCarState an observer holds predates the explosion.
//    The replica also renders an interpolation buffer behind even that. A
//    car doing 60 km/h covers about two and a half metres in the time those
//    two account for, and two and a half metres is a wreck in the wrong
//    lane.
//
//    So the transform is read at detonation, on the machine whose engine
//    destroyed the car, and the observer places the replica there before
//    calling BlowUpCar - the order BlowUpRemoteVehicle has used since
//    version 5, for the same reason: BlowUpCar reads GetPosition() for the
//    blast, the camera shake and the fire it lights, so correcting the car
//    afterwards would leave all three in the wrong place and move only the
//    shell.
//
//    UNOWNED_PARKED still ignores it; see BlastTransform.
// 17: a pedestrian's limbs come off on every screen. One opcode pair,
//    C_PedBodyPart / S_PedBodyPart (0x74/0x75), the two step 2 left free in
//    the pedestrian block.
//
//    Dismemberment only ever happened on the machine hosting the ped. The
//    shot that does it is replayed there against the real pedestrian, while
//    every observer's replica is bullet- and explosion-proof on purpose - so
//    the shooter watched a man die with his head on while the host watched
//    it come off. Players are not part of this: CPed::InflictDamage never
//    takes a limb off anybody IsPlayer(), in single player either.
//
//    A statement of fact from the host, like the ped drop: its engine has
//    already called CPed::RemoveBodyPart, and observers call the same
//    function on their replica with the same two arguments.
//
// 18: ten branches at once, and one number for all of them. Each was built
//    against either 16 or 17 and each left its own number unassigned, so this
//    is the entry those notes said would be written at the merge. There is no
//    17.5 and no per-branch number: a client and a server that disagree about
//    any one of the changes below cannot safely agree about the rest, so they
//    stand or fall together on 18.
//
//    Four of the ten put something new on the wire:
//
//    - C_VehicleDamage / S_VehicleDamage (0x3A/0x3B). A car's panels and
//      doors travel as absolute state, sent only when something got worse,
//      merged as a componentwise maximum. docs/cardamage.md.
//    - C_GarageState / S_GarageState and C_Respray / S_Respray (0xA0..0xA3).
//      One bit per garage saying "my own state machine has this garage away
//      from where this type rests"; the union of everybody's bits is what
//      each machine holds its own doors to. The respray pair carries the two
//      colours the owner's engine picked, because ChooseVehicleColour is a
//      per-machine round robin.
//    - C_PlayerAmmo / S_PlayerAmmo (0xB0/0xB1), behind SESSION_AMMO_SYNC.
//      One packet per inventory slot the sender is not holding, on change.
//      The held weapon's counts ride the snapshot instead, which is why
//      PlayerStateBody grew ammoClip and ammoTotal and went from 65 bytes to
//      71 - the one struct in this version whose layout moved.
//    - C_ObjectBroken / S_ObjectBroken (0xC0/0xC1), and C_PedDeath /
//      S_PedDeath (0xD8/0xD9). A street object somebody drove into, and an
//      ambient pedestrian his host's engine killed. Both are statements of
//      fact from one machine, like the ped drop and the limb.
//
//    Two changed the meaning of bytes that were already there, which is a
//    wire change even though nothing grew:
//
//    - PlayerFlags bits 4-7 now carry a player's wanted level (three bits,
//      0..6) and whether that level is the session's rather than their own.
//      docs/wanted.md.
//    - SessionFlags gained SESSION_AMMO_SYNC at bit 1 and the session's
//      wanted rule at bits 2-3. Those two collided: the wanted work wrote its
//      rule as `3 << 1` and the ammunition work took bit 1, so merged as
//      written a session with ammo sync on would have told every client its
//      wanted rule was `shared`. We moved the rule up two bits; neither
//      number had shipped.
//
//    The remaining four - the door-opening animation a remote player now
//    plays getting into a car, handing a car over when it changes drivers,
//    rebuilding an ambient replica the engine took away, and drawing a bullet
//    trail where the shooter saw it - add no opcode and move no layout. They
//    are in this version because they are in this build, not because the wire
//    needed them.
//
// 19: a vehicle claim is always answered. No opcode, no struct, no field:
//    what moved is the meaning of one value the server could not send
//    before.
//
//    A client that gets into a car the session has never seen sends
//    C_EnterVehicle with netId INVALID_NETID, meaning "name this", and
//    Client::m_vehicleClaimPending stops it ever asking twice. The server
//    had two ways out of that arm that returned without writing anything
//    back - a netId it did not recognise, and a vehicle table already
//    full - so either one left a player at the wheel of a car the session
//    said belonged to nobody, for the rest of the session. The 2026-09-22
//    logs caught exactly that.
//
//    S_EnterVehicle addressed to the claimer with netId INVALID_NETID is
//    now the refusal. It cannot be confused with a grant, because a grant
//    always names a real car, and it goes to the claimer alone since
//    nobody else was told the car existed. The client clears the pending
//    flag, leaves the claim alone for CLAIM_RETRY_MS and then asks again.
//
//    The version moves because an 18 server answers nothing and an 19
//    client would wait on it, while an 18 client reading a 19 refusal
//    would take INVALID_NETID as the name of its car.

constexpr uint16_t PROTOCOL_VERSION = 19;
constexpr uint16_t DEFAULT_PORT     = 2001;
constexpr uint8_t  MAX_PLAYERS      = 8;
constexpr uint8_t  SNAPSHOT_HZ      = 25;   // docs/protocol.md §1.2
constexpr size_t   NICK_LEN         = 24;
// CPed::m_weapons is thirteen slots and the eWeaponType doubles as the index
// into it, so an inventory weapon is a number in 0..12. Read out of the
// retail binary rather than re3 - CPed::CPed array-constructs 13 elements of
// 0x18 bytes at +0x35C (client/src/game/addresses.h).
constexpr uint8_t  INVENTORY_SLOTS  = 13;
constexpr size_t   CHAT_LEN         = 128;
constexpr uint8_t  INVALID_PLAYER   = 0xFF;
constexpr uint16_t INVALID_NETID    = 0;

// "no animation in this slot". AnimationId is a dense enum starting at 0, so
// it needs an out-of-band value rather than a zero, since ANIM_STD_WALK is 0.
constexpr uint16_t ANIM_NONE = 0xFFFF;

enum Channel : uint8_t {
	CH_SNAPSHOT = 0,   // unreliable, sequenced
	CH_EVENT    = 1,   // reliable, ordered
	CH_COUNT
};

enum Opcode : uint8_t {
	OP_C_HELLO           = 0x01,
	OP_S_WELCOME         = 0x02,
	OP_S_PLAYER_JOIN     = 0x03,
	OP_S_PLAYER_LEAVE    = 0x04,
	OP_C_PLAYER_MODEL    = 0x05,
	OP_S_PLAYER_MODEL    = 0x06,

	OP_C_PLAYER_STATE    = 0x10,
	OP_S_PLAYER_STATE    = 0x11,
	OP_C_VEHICLE_STATE   = 0x12,
	OP_S_VEHICLE_STATE   = 0x13,

	OP_C_SHOT            = 0x20,
	OP_S_SHOT            = 0x21,
	OP_C_DAMAGE          = 0x22,
	OP_S_DAMAGE          = 0x23,
	OP_S_DEATH           = 0x24,
	OP_C_RESPAWN         = 0x25,
	OP_S_RESPAWN         = 0x26,
	OP_C_EXPLOSION       = 0x27,
	OP_S_EXPLOSION       = 0x28,
	// Out of order because the block above was numbered before anyone had
	// worked out who gets to announce a death. It isn't the server: only the
	// machine that owns a player knows what that player's health really is.
	// See C_Death.
	OP_C_DEATH           = 0x29,

	OP_C_ENTER_VEHICLE   = 0x30,
	OP_S_ENTER_VEHICLE   = 0x31,
	OP_C_EXIT_VEHICLE    = 0x32,
	OP_S_EXIT_VEHICLE    = 0x33,
	OP_S_VEHICLE_SPAWN   = 0x34,
	OP_S_VEHICLE_DESPAWN = 0x35,
	OP_C_VEHICLE_BLOWUP  = 0x36,
	OP_S_VEHICLE_BLOWUP  = 0x37,
	// A car nobody owns was destroyed. Not the pair above with a different
	// name: that one is sent by a driver, about a car the session has a row
	// for, and carries the transform its owner's physics chose. This one is
	// about a car the session has never heard of and never will - one the map
	// put in the same place on every machine - so it carries a name the map
	// already agreed on and no transform at all. docs/protocol.md §1.14,
	// docs/roadmap.md §5.8.
	OP_C_UNOWNED_BLOWUP  = 0x38,
	OP_S_UNOWNED_BLOWUP  = 0x39,
	// What shape a car is in: panels and doors. Reliable and change-only,
	// because a car is undamaged for minutes and then takes a dent.
	// docs/cardamage.md. Deliberately NOT a field on the 25 Hz snapshot -
	// §3.4 of that document has the four reasons, and the one that decides it
	// is the same one roadmap.md §5.8 gives for the blow-up above: a snapshot
	// is sent by a driver and a parked car has no driver.
	OP_C_VEHICLE_DAMAGE  = 0x3A,
	OP_S_VEHICLE_DAMAGE  = 0x3B,
	// 0x3C-0x3F stay free for the rest of the vehicle block.

	OP_S_WORLD_STATE     = 0x40,
	OP_C_WORLD_STATE     = 0x41,

	OP_C_CHAT            = 0x50,
	OP_S_CHAT            = 0x51,

	// Ambient population (docs/population.md §3 steps 2 and 4).
	OP_C_PED_SPAWN       = 0x70,
	OP_S_PED_SPAWN       = 0x71,
	OP_C_PED_DESPAWN     = 0x72,
	OP_S_PED_DESPAWN     = 0x73,
	// A limb coming off, from the ped's host. Version 17.
	OP_C_PED_BODY_PART   = 0x74,
	OP_S_PED_BODY_PART   = 0x75,
	// The ped transform stream, added a protocol version after the traffic
	// one and modelled on it. 0x74/0x75 were left alone: they sit inside the
	// block step 2 reserved for pedestrians and the stream belongs next to
	// the traffic stream it copies, not in the middle of the handshake.
	// Version 17 gave them to the body-part pair above.
	OP_C_PED_STATES      = 0x7C,
	OP_S_PED_STATES      = 0x7D,
	// Traffic. The spawn/despawn pair is the ped handshake again; the state
	// pair is the thing a ped did not need, because a ped stands where it is
	// put and a traffic car is on its way somewhere.
	OP_C_CAR_SPAWN       = 0x76,
	OP_S_CAR_SPAWN       = 0x77,
	OP_C_CAR_DESPAWN     = 0x78,
	OP_S_CAR_DESPAWN     = 0x79,
	OP_C_CAR_STATES      = 0x7A,
	OP_S_CAR_STATES      = 0x7B,
	// 0x80..0x8F is the pickup block. Six of the sixteen are used; the rest
	// are reserved so the drop replication docs/pickups.md 1 defers has
	// somewhere to go without another renumbering.
	OP_C_PICKUP_CLAIM     = 0x80,
	OP_S_PICKUP_TAKEN     = 0x81,
	OP_S_PICKUP_DENIED    = 0x82,
	OP_C_PICKUP_RELEASE   = 0x83,
	OP_S_PICKUP_GRANT     = 0x84,
	OP_C_PICKUP_COLLECTED = 0x85,

	// 0x86-0x8F were reserved by the pickup work for "the drop replication
	// section 1 defers". This is it, and it needs two of the ten.
	OP_C_PICKUP_DROP      = 0x86,
	OP_S_PICKUP_DROP      = 0x87,

	// 0xA0..0xAF is the garage block: doors, garages and the Pay'n'Spray.
	// Four of the sixteen are used. docs/protocol.md §1.16.
	OP_C_GARAGE_STATE     = 0xA0,
	OP_S_GARAGE_STATE     = 0xA1,
	OP_C_RESPRAY          = 0xA2,
	OP_S_RESPRAY          = 0xA3,

	// Ammunition for an inventory slot the player is NOT currently holding.
	// The held weapon's count rides the snapshot instead - see
	// PlayerStateBody::ammoTotal for the argument.
	OP_C_PLAYER_AMMO      = 0xB0,
	OP_S_PLAYER_AMMO      = 0xB1,

	// 0xC0..0xCF is the breakable-street-object block. docs/objects.md.
	// Two of the sixteen are used and the rest stay reserved, because the
	// one thing this deliberately does not carry - where a knocked-over
	// lamp post came to rest - would want its own opcode if it is ever
	// built, and renumbering a wire twice is how two agents collided here.
	OP_C_OBJECT_BROKEN    = 0xC0,
	OP_S_OBJECT_BROKEN    = 0xC1,

	// An ambient pedestrian dying. 0xD8-0xDF is the block reserved for it;
	// two of the eight are used and the other six stay free, because the
	// thing that would want them next is the same shape - a hosted ped
	// reaching a state only its host can witness.
	//
	// Not folded into the 0x7x ambient block, which is full: 0x70..0x7D are
	// the ped and car handshakes and their two streams, and squeezing a
	// death in between them would have renumbered the lot.
	OP_C_PED_DEATH        = 0xD8,
	OP_S_PED_DEATH        = 0xD9,
};

enum LeaveReason : uint8_t {
	LEAVE_QUIT    = 0,
	LEAVE_TIMEOUT = 1,
	LEAVE_KICKED  = 2,
};

enum RejectReason : uint8_t {
	REJECT_NONE            = 0,
	REJECT_BAD_VERSION     = 1,
	REJECT_FULL            = 2,
};

// Session-wide rules a client has to know about, handed over in S_Welcome.
enum SessionFlags : uint8_t {
	// docs/roadmap.md §5.2: server-configurable, off by default. The server
	// is the one that enforces it, by refusing to relay a C_Damage between
	// players at all. This bit exists because one kind of damage never
	// reaches the server: an explosion is replayed at a fixed world position
	// and every machine decides for itself whether its own player is
	// standing in it (§1.9.2). A client that knows friendly fire is off
	// makes its own player immune for the length of that replay.
	SESSION_FRIENDLY_FIRE = 1 << 0,

	// Ammunition is reported honestly instead of every remote player holding
	// a gun CoopIII invented a thousand rounds for. Server-configurable, off
	// by default, and it is NOT a shared inventory: two players carrying
	// different weapons is the normal case and stays that way. What this
	// turns on is each player's own count for their own guns being the
	// number everybody else's copy of them holds.
	//
	// Enforced in both places, the way friendly fire is. The server refuses
	// to relay C_PlayerAmmo with the bit clear, and a client with the bit
	// clear ignores the two ammo fields in a snapshot and goes on handing a
	// remote ped the fixed amount. Off is today's behaviour exactly.
	SESSION_AMMO_SYNC     = 1 << 1,

	// docs/roadmap.md §5.1 and docs/wanted.md §4.8: two bits carrying the
	// server's WantedLevelRule. Here rather than in a packet of its own for
	// the same reason friendly fire is - it is a session rule that a client
	// has to apply locally, because the thing being governed lives inside
	// that client's engine and never passes through the server at all.
	//
	// Unlike friendly fire there is no server-side half. A wanted level is
	// not relayed and cannot be refused; the server's whole part in this is
	// saying which rule is in force.
	//
	// Bits 2 and 3. The wanted work wrote these as `3 << 1` while the ammo
	// work took bit 1 for SESSION_AMMO_SYNC above, so on a naive merge a
	// session with ammo sync on would also have told every client its wanted
	// rule was `shared`. Moved here at the version 18 merge; nothing had
	// shipped on either number.
	SESSION_WANTED_MASK  = 3 << 2,
	SESSION_WANTED_SHIFT = 2,
};

// WantedLevelRule as a wire value. Kept as plain integers rather than as the
// server's enum because sdk/ is shared with the client, which has no business
// including server/core/config.h. The three values are the ones
// ParseWantedLevel accepts and Name prints.
enum WantedRule : uint8_t {
	WANTED_RULE_PERPLAYER = 0,   // §5.1 default: own stars, shared inside a car
	WANTED_RULE_SHARED    = 1,   // the whole session holds the highest level
	WANTED_RULE_OFF       = 2,   // no wanted level at all
};

inline uint8_t WantedRuleFromFlags(uint8_t flags) {
	const uint8_t rule = static_cast<uint8_t>((flags & SESSION_WANTED_MASK) >> SESSION_WANTED_SHIFT);
	// 3 is not a rule. A client from a newer build could send one; falling
	// back to the default is the same stance Parse takes on an unknown key.
	return rule > WANTED_RULE_OFF ? WANTED_RULE_PERPLAYER : rule;
}

inline uint8_t FlagsWithWantedRule(uint8_t flags, uint8_t rule) {
	if (rule > WANTED_RULE_OFF)
		rule = WANTED_RULE_PERPLAYER;
	flags = static_cast<uint8_t>(flags & ~SESSION_WANTED_MASK);
	return static_cast<uint8_t>(flags | (rule << SESSION_WANTED_SHIFT));
}

#pragma pack(push, 1)

struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

// sendTimeMs is the sender's own monotonic clock, ms since its CoopIII
// started. Clocks aren't shared between machines, so interpolation buffers
// are keyed per sender rather than compared across them.
//
// This is NOT CTimer::GetTimeInMilliseconds(). That one stops while the game
// is paused and gets multiplied by ms_fTimeScale, which both missions
// (SET_TIME_SCALE) and player death (1/3 slow motion) change. If we used it,
// a dying player's timestamps would drop to a third speed of everyone
// else's and their interpolation would desync. Need a clock nothing in the
// game can rescale.
struct PacketHeader {
	uint8_t  opcode;
	uint32_t sendTimeMs;
};

// ---- session -------------------------------------------------------------

struct C_Hello {
	static constexpr uint8_t OPCODE = OP_C_HELLO;
	PacketHeader hdr;
	uint16_t protocolVersion;
	char     nick[NICK_LEN];
	uint16_t modelId;
};

// On accept, followed by one S_PlayerJoin per player already in the session.
struct S_Welcome {
	static constexpr uint8_t OPCODE = OP_S_WELCOME;
	PacketHeader hdr;
	uint8_t  reject;        // RejectReason; if != REJECT_NONE the rest is unset
	uint8_t  playerId;
	uint16_t netId;
	uint8_t  maxPlayers;
	uint8_t  snapshotHz;
	uint8_t  hour, minute;
	uint8_t  weather;
	uint8_t  weatherOld;
	// Who the session is taking its time of day from, or INVALID_PLAYER
	// while nobody has been picked. Here as well as in S_WorldState so a
	// newcomer who turns out to be the host never applies the hour above:
	// the host's own game is what that hour is supposed to be tracking.
	uint8_t  hostPlayerId;
	uint8_t  flags;         // SessionFlags
};

// What a player is, and what condition they are currently in.
//
// Both halves matter, and the second one is version 9. A join packet used to
// carry identity only, which is correct for the player it announces *as they
// arrive* and wrong for the eight this same packet replays to a late joiner:
// those eight have been playing for twenty minutes. Identity says who they
// are, and the rest says whether they are on 12 health, holding an AK, or
// lying dead in the road waiting for an ambulance.
enum PlayerJoinFlags : uint8_t {
	// `pos`/`heading` are somewhere the session actually saw this player,
	// rather than the zeroes a Player starts life with.
	//
	// Without this bit there is no way to tell "at the origin" from "we have
	// never heard from them", and the origin in GTA III is open water: the
	// first remote ped CoopIII ever created was born there and drowned in
	// eight frames. So a receiver that cannot tell waits, and a receiver
	// that can spawn them where they are. Set on every backfilled player the
	// session has had one snapshot from; clear on the live announcement of
	// someone who has this instant connected.
	PJF_POS_VALID = 1 << 0,
	// Dead and waiting to respawn. Their own machine said so (C_Death) and
	// the session has been holding it ever since.
	//
	// This is the bit the whole version is about. Death arrives as an event,
	// an event only reaches whoever was connected at the time, and there is
	// no second carrier - so before version 9 a player who joined while
	// somebody was lying in the road got a live one, standing up, on zero
	// health, until the corpse got up by itself.
	PJF_DEAD = 1 << 1,
};

struct S_PlayerJoin {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_JOIN;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t netId;
	char     nick[NICK_LEN];
	uint16_t modelId;
	Vec3     pos;
	float    heading;

	// ---- condition, not identity (version 9) ------------------------------
	//
	// These duplicate fields that ride the 25 Hz snapshot, and that is the
	// point rather than an oversight: a snapshot is 40 ms away for a player
	// who is *sending* one. A player on a loading screen, in the frontend or
	// mid-cutscene has no ped to sample and sends nothing at all, so for them
	// the snapshot is never. This packet is the only thing the session can
	// promise a joiner.
	float    health;
	float    armour;
	uint8_t  weapon;        // eWeaponType
	uint8_t  flags;         // PlayerJoinFlags
	// The animation their own engine picked when they died, kept from their
	// C_Death so a backfilled corpse lies the same way on every screen.
	// ANIM_NONE when they are alive, or when the sender had none to give.
	uint16_t deathAnimId;
};

struct S_PlayerLeave {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_LEAVE;
	PacketHeader hdr;
	uint8_t playerId;
	uint8_t reason;         // LeaveReason
};

// Sent only when a player's model actually changes, not on every join.
// Reliable, change-only: a model swap happens maybe a handful of times per
// playthrough, so cramming it into the 25 Hz snapshot would burn two bytes
// forty times a second for nothing.
//
// Stock GTA III never triggers this at all: one protagonist model, no
// outfits (see MI_PLAYER in client/src/game/addresses.h for the evidence).
// But that's a fact about the stock script data, not the engine. Nothing
// stops a ped's model index from changing, some mod probably will change it,
// and a remote player wearing the wrong body is exactly the kind of bug
// that's obvious on screen and invisible in any log.
struct C_PlayerModel {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_MODEL;
	PacketHeader hdr;
	uint16_t modelId;
};

struct S_PlayerModel {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_MODEL;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t modelId;
};

// ---- snapshots (CH_SNAPSHOT) ---------------------------------------------

enum PlayerFlags : uint8_t {
	PF_AIMING = 1 << 0,   // CPed::bIsAimingGun, aimYaw is a real target
	// CPed::bIsShooting: the state of holding the trigger, not the act of
	// discharging. 25 Hz can't carry one event per bullet, so individual
	// shots go out on C_Shot (reliable channel) and this flag just keeps the
	// ped in a firing posture in between. docs/protocol.md §1.9.
	PF_FIRING = 1 << 1,

	// ASSOC_RUNNING on the animation in animId2, and it is the difference
	// between a player aiming and a player firing.
	//
	// A weapon has one animation covering the draw, the ready pose, the shot
	// and the recovery. CPed::PointGunAt parks it on the ready frame and
	// clears ASSOC_RUNNING; CPed::FireGun sets it running and loops it over
	// the firing part. Send the id and the phase without this bit and the
	// receiver has no way to tell the two apart, so it plays the whole thing
	// from the top, forever, and what you see is a player drawing their gun
	// over and over and never firing it.
	//
	// Costs nothing: a spare bit in a byte that was already on the wire.
	PF_ANIM2_RUNNING = 1 << 2,

	// CPed::m_pFire != nil - this player is on fire right now.
	//
	// A ped being alight is a boolean with a lifetime, so it belongs in a
	// flags byte and not in a packet of its own. It is another spare bit in
	// a byte that was already being sent, which is why fire on a body cost
	// no wire format change and no version bump (docs/roadmap.md §5.7,
	// docs/protocol.md §1.10.7).
	//
	// It travels one way only: from the player who is burning, to everyone
	// watching. The observer lights its own copy of the ped so the flames
	// are there, and decides nothing at all about that player's health -
	// their machine already did that, from a fire physically in their world.
	//
	// It rides the unreliable snapshot on purpose. A dropped packet is
	// corrected 40 ms later by the next one, and the fire an observer starts
	// carries its own extinguish time, so the worst a lost "no longer
	// burning" can do is burn for another second.
	PF_ON_FIRE = 1 << 3,

	// ---- the wanted level (docs/wanted.md §4.3) --------------------------
	//
	// Three bits, 0..6 stars. The engine's ceiling is 6
	// (CWanted::MaximumWantedLevel, addresses.h), so the eighth value is
	// spare and is clamped on the way in rather than trusted.
	//
	// It rides the unreliable snapshot for the same reason PF_ON_FIRE does:
	// it is a state with a lifetime, not an event, and a dropped packet is
	// corrected 40 ms later by the next one.
	//
	// What it means is "what is on this player's HUD right now", not "what
	// they earned" - see PF_WANTED_BORROWED for the difference and why it
	// costs a bit.
	PF_WANTED_MASK  = 7 << 4,
	PF_WANTED_SHIFT = 4,

	// This level is the session's, not mine.
	//
	// Only read in the `shared` rule, and it is what keeps that rule from
	// deadlocking. A earns four stars and B is raised to four to match; A
	// dies and clears; without this bit B is still reporting four, so A is
	// immediately raised back to four by a level that only exists because A
	// had it. Neither can ever get out. With it, the session's floor is the
	// maximum over players who are *not* borrowing, so when A's own four
	// goes, B's borrowed four goes with it.
	//
	// docs/wanted.md §4.5 traces it, and the case that must NOT drop: two
	// players who each independently earned three are both reporting an
	// un-borrowed three, so neither follows the other down.
	PF_WANTED_BORROWED = 1 << 7,
};

// The most stars GTA III will hold. CWanted::MaximumWantedLevel is 6 and
// CWanted::SetWantedLevel clamps its argument against it (`cmp ebp,[5F7714h] /
// jle`), so seven - which three bits can carry - is not a state the engine has
// and is clamped rather than trusted.
constexpr uint8_t WANTED_LEVEL_CEILING = 6;

// The wanted level a flags byte carries, clamped to what the engine can hold.
inline uint8_t WantedFromFlags(uint8_t flags) {
	const uint8_t level = static_cast<uint8_t>((flags & PF_WANTED_MASK) >> PF_WANTED_SHIFT);
	return level > WANTED_LEVEL_CEILING ? WANTED_LEVEL_CEILING : level;
}

// `flags` with the wanted bits replaced. Clamped, so a caller that hands over
// a level out of a corrupted snapshot cannot spill into PF_WANTED_BORROWED.
inline uint8_t FlagsWithWanted(uint8_t flags, uint8_t level, bool borrowed) {
	if (level > WANTED_LEVEL_CEILING)
		level = WANTED_LEVEL_CEILING;
	flags = static_cast<uint8_t>(flags & ~(PF_WANTED_MASK | PF_WANTED_BORROWED));
	flags = static_cast<uint8_t>(flags | (level << PF_WANTED_SHIFT));
	if (borrowed)
		flags = static_cast<uint8_t>(flags | PF_WANTED_BORROWED);
	return flags;
}

// Field sources are re3 CPed/CPhysical members, see docs/protocol.md §1.7.
//
// Two animation slots because a ped's clump holds a list of blended
// associations, not a single animation (§1.8). animId is the dominant
// whole-body one, animId2 is the dominant ASSOC_PARTIAL overlay (firing,
// punching, that sort of thing lives here). Either can be ANIM_NONE.
//
// animTime is CAnimBlendAssociation::currentTime in seconds, the phase to
// resume at on the receiver so a run cycle doesn't restart mid-stride.
// blendAmount is NOT sent. The receiver blends in with the engine's own
// delta rather than fighting it.
struct PlayerStateBody {
	Vec3     pos;
	float    heading;       // CPed::m_fRotationCur
	Vec3     moveSpeed;     // CPhysical::m_vecMoveSpeed
	uint8_t  moveState;     // eMoveState
	uint8_t  pedState;      // PedState

	// CPed::m_animGroup, the walking style. Makes a remote player actually
	// face the way they're going.
	//
	// GTA III has no sideways-walk animation id. Strafing is a whole other
	// group instead: ASSOCGRP_PLAYERLEFT, _PLAYERRIGHT and _PLAYERBACK each
	// hold their own ANIM_STD_WALK/ANIM_STD_RUN, and
	// CPlayerPed::ProcessAnimGroups swaps the group once the walk angle
	// crosses 50 degrees. Same field also carries the weapon (rocket
	// launcher = ASSOCGRP_PLAYERROCKET, pistol = ASSOCGRP_PLAYER1ARMED, etc).
	//
	// So animId alone means nothing without this. Skip it and a player
	// strafing left gets sent plain ANIM_STD_RUN, which the receiver plays
	// from the default group, which is a forward run, so they slide sideways while
	// sprinting at nothing. First bug anyone spotted on screen.
	uint8_t  animGroup;     // AssocGroupId, 0..NUM_ANIM_ASSOC_GROUPS-1

	uint16_t animId;        // AnimationId, or ANIM_NONE
	float    animTime;      // CAnimBlendAssociation::currentTime, seconds
	float    animSpeed;     // CAnimBlendAssociation::speed
	uint16_t animId2;       // partial-overlay AnimationId, or ANIM_NONE
	float    animTime2;
	float    health;
	float    armour;
	uint8_t  weapon;        // eWeaponType

	// The real count for the weapon above, and only for that one.
	//
	// Two fields because the engine has two and they do different jobs, both
	// read out of the retail binary rather than out of re3:
	//
	//   m_nAmmoInClip (CWeapon +0x08) is what gates firing. CWeapon::Fire
	//   opens `cmp dword [edi+8],0 / jg` at 0x0055C4A2 and returns false when
	//   it is empty, and decrements it at 0x0055C7D1.
	//
	//   m_nAmmoTotal (CWeapon +0x0C) is what the game shows and what the
	//   script reports. CHud::Draw reads it at 0x00506052 and prints either
	//   "total" or "total-clip" depending on the weapon's
	//   m_nAmountofAmmunition, and GET_AMMO_IN_CHAR_WEAPON (opcode 1050,
	//   handler 0x00588F22) answers with m_nAmmoTotal and nothing else.
	//
	// Send one and the other is a guess. Sending both is six bytes on a
	// 65-byte snapshot, which at 25 Hz and eight players is about 1 KB/s
	// across the session (§2.3) - the price of a firefight having the same
	// numbers on every screen.
	//
	// Sizes: the total is capped by CPed::GiveWeapon at 99999 (0x1869F, the
	// `cmp eax,0x1869F` at 0x004CF9E0), so it needs 32 bits. The clip is
	// capped by CWeapon::Reload at the weapon's m_nAmountofAmmunition, whose
	// largest value in stock weapon.dat is 1000 - 16 bits with a saturating
	// write, because weapon.dat is data the player can edit.
	//
	// Both are zero and meaningless unless the session has SESSION_AMMO_SYNC.
	uint16_t ammoClip;      // CWeapon::m_nAmmoInClip, saturated
	uint32_t ammoTotal;     // CWeapon::m_nAmmoTotal

	// World-space aim direction. yaw is CPed::m_fLookDirection while aiming;
	// pitch is CPedIK::m_torsoOrient.pitch. §1.8.3 on why only yaw is applied.
	float    aimYaw, aimPitch;
	uint8_t  flags;         // PlayerFlags
};

struct C_PlayerState {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_STATE;
	PacketHeader hdr;
	PlayerStateBody body;
};

struct S_PlayerState {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_STATE;
	PacketHeader hdr;
	uint8_t playerId;
	PlayerStateBody body;
};

enum VehicleFlags : uint8_t {
	VEH_ENGINE_ON = 1 << 0,   // CVehicle::bEngineOn
	VEH_SIREN     = 1 << 1,   // CVehicle::m_bSirenOrAlarm
	VEH_LIGHTS    = 1 << 2,

	// This car has been destroyed. Not "is on low health" - blown up, wrecked,
	// finished.
	//
	// Health alone does not carry it, and that is a measured fact rather than
	// caution: an observer that writes zero into m_fHealth gets a car that
	// reads as dead and still looks and behaves brand new, because in GTA III
	// destroying a car is something the engine *does* (CVehicle::BlowUpCar and
	// the status change that goes with it), not a number it stores. The owner
	// found this from the other end - a car he had blown up came back to a
	// late joiner intact enough to climb into and too dead to drive.
	//
	// So the sender says it outright, the session remembers it, and no
	// receiver has to infer it from a float. Whose job it is to act on it
	// belongs to the vehicle seam (client/src/game/vehicle.cpp); this is only
	// the wire agreeing that there is something to act on.
	VEH_WRECKED = 1 << 3,
};

struct VehicleStateBody {
	uint16_t netId;
	Vec3     pos;
	Quat     rot;
	Vec3     moveSpeed;     // CPhysical::m_vecMoveSpeed
	Vec3     turnSpeed;     // CPhysical::m_vecTurnSpeed
	float    steer, gas, brake;   // CVehicle::m_fSteerAngle/m_fGasPedal/m_fBrakePedal
	uint8_t  gear;          // CVehicle::m_nCurrentGear
	float    health;        // CVehicle::m_fHealth, 1000 = full
	uint8_t  flags;         // VehicleFlags
};

struct C_VehicleState {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_STATE;
	PacketHeader hdr;
	VehicleStateBody body;
};

struct S_VehicleState {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_STATE;
	PacketHeader hdr;
	uint8_t playerId;       // driver
	VehicleStateBody body;
};

// ---- combat (CH_EVENT) ---------------------------------------------------

// One discharge of one weapon, as the shooter's machine saw it.
//
// Per-event on the reliable channel, not a snapshot bit. A shot is discrete,
// a snapshot is a sample, and an Uzi can empty a clip between two 25 Hz ticks,
// and a dropped rocket is a missing explosion, not a slightly wrong one.
// PF_FIRING in the snapshot covers the state of holding the trigger; this
// covers actual shots fired.
//
// origin is whatever the engine used as the fire source: CWeapon::Fire's
// fireSource, the muzzle, not the ped's centre.
//
// dir and speed mean different things depending on weapon type, which is
// really the whole reason this struct exists:
//
//   instant hit (pistol, uzi, shotgun, AK, M16): dir is the unit direction of
//     the line the shooter's own engine traced, speed is 0. Not the ped's
//     forward vector, which is what this field used to carry and what the
//     receiver would have derived for itself anyway - it is the aim, whether
//     that came from the camera, a lock-on or the hand bone, sampled at
//     CWeapon::ProcessLineOfSight so no branch of the fire path has to be
//     re-implemented to read it. A shotgun traces five rays and dir is the
//     middle of that cone.
//
//     The receiver aims with it now, which it did not before. It turns the
//     engine's own proposal onto this line inside CWeapon::DoDoomAiming, so
//     the trail, the impact decal and the line-of-sight all follow the
//     shooter rather than an interpolated ped's heading, and a shot fired up
//     or down is no longer flat on every screen but the shooter's.
//     docs/protocol.md 1.9.7.
//
//     **The layout did not change and PROTOCOL_VERSION did not move.** The
//     field is the same three floats in the same place; only what is written
//     into it did. A client built before this change interoperates: it sends
//     the body forward, the receiver aims along the body forward, and that is
//     precisely the behaviour this replaced.
//
//   projectile (rocket, molotov, grenade): dir is the unit direction of the
//     projectile's initial CPhysical::m_vecMoveSpeed, speed its magnitude.
//     These get applied exactly, because a molotov arc that starts from a
//     locally re-derived velocity lands on a different street, so the velocity
//     CProjectileInfo::AddProjectile computes depends on the thrower's
//     heading and the throw charge, neither of which an observer knows
//     first-hand.
struct ShotBody {
	uint8_t weapon;       // eWeaponType
	Vec3    origin;
	Vec3    dir;
	float   speed;
};

struct C_Shot {
	static constexpr uint8_t OPCODE = OP_C_SHOT;
	PacketHeader hdr;
	ShotBody body;
};

struct S_Shot {
	static constexpr uint8_t OPCODE = OP_S_SHOT;
	PacketHeader hdr;
	uint8_t  playerId;
	ShotBody body;
};

// One hit, as the attacker's machine resolved it, before anything was
// applied to anyone.
//
// This is the shooter's own CPed::InflictDamage call, taken away from their
// engine and put on the wire instead. Every field is an argument of that
// function, unchanged: no multiplier, no armour, no reduction. The victim's
// machine feeds them back into its own InflictDamage, so armour, the player's
// own damage multiplier, the hit reaction and death all happen exactly where
// single player puts them.
//
// Why the attacker decides and the victim applies, rather than either one
// doing both:
//
//   The attacker fired a ray from a position only they know, at an instant
//   only they know. Let the victim work out whether they were hit and the
//   question becomes "was A's ped, interpolated 100 ms late, in front of me",
//   which is how you get shot around corners. So the hit is the attacker's.
//
//   The health is the victim's. Nobody else has their armour, their current
//   state, or whether some mission just made them invulnerable, and two
//   machines subtracting from the same health pool disagree within seconds.
//
// docs/protocol.md §1.10.
struct DamageBody {
	uint16_t victimNetId;
	uint8_t  weapon;        // eWeaponType, and only the ones §1.10.1 allows
	float    amount;        // CPed::InflictDamage's `damage`, raw
	uint8_t  piece;         // ePedPieceTypes, 0..6
	// Which side the hit came from, 0 front, 1 left, 2 back, 3 right. Picks
	// between the four ANIM_STD_HIGHIMPACT_* reactions. Costs a byte and is
	// the difference between being knocked the way you were shot and always
	// falling on your face.
	uint8_t  direction;
};

struct C_Damage {
	static constexpr uint8_t OPCODE = OP_C_DAMAGE;
	PacketHeader hdr;
	DamageBody body;
};

// Sent to the victim alone, not broadcast. Nobody else needs it: the health
// it produces rides the victim's own snapshots a moment later, and the hit
// reaction is an animation that rides them too.
struct S_Damage {
	static constexpr uint8_t OPCODE = OP_S_DAMAGE;
	PacketHeader hdr;
	uint8_t    attackerId;
	DamageBody body;
};

// "I died." Sent by the machine whose player it is, and by no one else.
//
// The version-5 draft had only S_Death, which put the decision on the server.
// That contradicts the one rule the whole design rests on: a player's health
// lives on their own machine, so their own machine is the only thing that can
// say when it ran out. The server relays and keeps score; it doesn't decide.
//
// animId is the animation the engine picked for this particular death, taken
// from the CPed::SetDie call it made. A headshot, a drowning and a car
// knocking you over are three different animations and the observer has no
// way to work out which. ANIM_NONE means the sender couldn't capture one and
// the observer should use its default.
struct C_Death {
	static constexpr uint8_t OPCODE = OP_C_DEATH;
	PacketHeader hdr;
	// Whoever damaged the sender last, if it was recent enough to be the
	// reason. INVALID_NETID for drowning, a fall, a car, or a kill nobody
	// has a claim on.
	uint16_t killerNetId;
	uint16_t animId;
};

struct S_Death {
	static constexpr uint8_t OPCODE = OP_S_DEATH;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t killerNetId;
	uint16_t animId;
};

// Where a player's explosion actually went off.
//
// A projectile flies for a couple seconds and GTA III's physics is
// frame-rate coupled (§1.2), so two machines starting the same molotov from
// the same place at the same velocity still don't land it in the same spot, and
// the gap grows with every bounce. Observers animate the projectile locally
// but don't get to decide where it ends: only the thrower's machine says
// where it exploded, everyone else just plays that.
//
// Same idea as correcting a locally-simulated remote car
// (client/src/game/vehicle.h). Also why this needs its own event instead of
// being derived from C_Shot.
struct ExplosionBody {
	uint8_t type;         // eExplosionType: 0 grenade, 1 molotov, 2 rocket
	Vec3    pos;
};

struct C_Explosion {
	static constexpr uint8_t OPCODE = OP_C_EXPLOSION;
	PacketHeader  hdr;
	ExplosionBody body;
};

struct S_Explosion {
	static constexpr uint8_t OPCODE = OP_S_EXPLOSION;
	PacketHeader  hdr;
	uint8_t       playerId;   // whose explosion it is
	ExplosionBody body;
};

// "I'm alive again, over here."
//
// GTA III resurrects the same CPed rather than making a new one
// (CGameLogic::RestorePlayerStuffDuringResurrection), so on the owner's
// machine a respawn is a teleport and a health reset and nothing else. On
// every other machine it isn't: what they have is a corpse, in the state
// CPed::SetDie left it, with its collision cleared and its health at zero.
// There's no un-die, so the corpse gets destroyed and a fresh ped built the
// same way the first one was.
//
// The transform is here because the two ends of that are half a city apart.
// A ped rebuilt from the snapshot stream alone would be born at the place its
// owner died and then snap to the hospital once the buffer caught up.
struct RespawnBody {
	Vec3  pos;
	float heading;
};

struct C_Respawn {
	static constexpr uint8_t OPCODE = OP_C_RESPAWN;
	PacketHeader hdr;
	RespawnBody body;
};

struct S_Respawn {
	static constexpr uint8_t OPCODE = OP_S_RESPAWN;
	PacketHeader hdr;
	uint8_t     playerId;
	RespawnBody body;
};

// ---- vehicles (CH_EVENT) -------------------------------------------------

// Getting into a car. First time anyone gets into a given car, this is also
// what tells the session it exists.
//
// GTA III spawns its own traffic and parked cars locally, differently on
// every machine, so there's no shared vehicle world to point at. Instead of
// trying to sync all of Liberty City's traffic, a car enters the session
// only when someone gets in it: client sends its identity with
// netId == INVALID_NETID, server hands out a netId and tells everyone else
// to spawn a matching one. Cars nobody's touched just stay local and
// unsynced. Cheap, and nobody notices two players seeing different traffic.
//
// Identity fields only matter when netId is INVALID_NETID. The normal case
// (getting into a car the session already knows) just needs netId and seat.
struct EnterVehicleBody {
	uint16_t netId;
	uint8_t  seat;          // 0 is the driver
	uint8_t  jack;          // pulling the current occupant out

	uint16_t modelId;
	uint8_t  colour1, colour2;

	// The extra components fitted to this car: CVehicle::m_aExtras, -1 for
	// "nothing in that slot". Here for the same reason the colours are: the
	// engine chooses them at spawn, per machine, from
	// CVehicleModelInfo::ChooseComponent - so two machines rolling
	// independently give two players cars with different bits bolted on.
	//
	// Unlike a colour these cannot be applied after the fact. They are
	// RwAtomics cloned into the clump while the car is being constructed, so
	// the receiving machine has to force them through the engine's own
	// CVehicleModelInfo::ms_compsToUse override BEFORE it calls the
	// constructor. client/src/game/addresses.h, "a vehicle's extra
	// components", is the mechanism and the two traps in it.
	//
	// Signed on purpose and clamped on arrival: the engine's subscript into
	// m_comps[6] checks only for -1, so a wire value it does not expect is a
	// read off the end of a model info.
	int8_t   extra1, extra2;

	Vec3     pos;
	Quat     rot;
};

struct C_EnterVehicle {
	static constexpr uint8_t OPCODE = OP_C_ENTER_VEHICLE;
	PacketHeader hdr;
	EnterVehicleBody body;
};

struct S_EnterVehicle {
	static constexpr uint8_t OPCODE = OP_S_ENTER_VEHICLE;
	PacketHeader hdr;
	uint8_t          playerId;
	EnterVehicleBody body;
};

struct C_ExitVehicle {
	static constexpr uint8_t OPCODE = OP_C_EXIT_VEHICLE;
	PacketHeader hdr;
	uint16_t netId;
};

struct S_ExitVehicle {
	static constexpr uint8_t OPCODE = OP_S_EXIT_VEHICLE;
	PacketHeader hdr;
	uint8_t  playerId;
	uint16_t netId;
};

// Create this car, in the condition the session last saw it.
//
// The condition half is version 9, and it is the packet the owner's bug was
// actually about. A spawn used to describe a car the way a showroom describes
// one - model and paint - and the receiver filled in the rest with "brand
// new". That is right for the car being claimed this second and wrong for
// every car in the backfill, which have been driven, shot at and in one case
// blown up. `health` and `flags` are the difference between rebuilding the
// car's *identity* and rebuilding the car.
struct S_VehicleSpawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_SPAWN;
	PacketHeader hdr;
	uint16_t netId;
	uint16_t modelId;
	Vec3     pos;
	Quat     rot;
	uint8_t  colour1, colour2;   // CVehicle::m_currentColour1/2
	int8_t   extra1, extra2;     // CVehicle::m_aExtras, -1 for none
	float    health;             // CVehicle::m_fHealth, 1000 = full
	uint8_t  flags;              // VehicleFlags, same bits the snapshot uses
};

struct S_VehicleDespawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DESPAWN;
	PacketHeader hdr;
	uint16_t netId;
};

// A car was destroyed. Sent by the machine driving it, replayed by everyone
// else through the engine's own CAutomobile::BlowUpCar. docs/protocol.md
// §1.11 is the design; the short version is that health is a number and
// destruction is an event, and only one of those was on the wire.
//
// The transform travels with it because BlowUpCar is where the wreck is
// decided as well as the blast: whatever the observer's own physics had the
// car doing, this is where its owner says it ended up. Both machines put the
// car here first and then blow it up, so both end up with the same wreck in
// the same street rather than one wreck and one empty road.
struct VehicleBlowUpBody {
	uint16_t netId;
	Vec3     pos;
	Quat     rot;
};

struct C_VehicleBlowUp {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_BLOWUP;
	PacketHeader      hdr;
	VehicleBlowUpBody body;
};

struct S_VehicleBlowUp {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_BLOWUP;
	PacketHeader      hdr;
	uint8_t           playerId;   // whose car it was, for the log
	VehicleBlowUpBody body;
};

// ---- a car nobody owns was destroyed (CH_EVENT) ----------------------------
//
// docs/roadmap.md §5.8, docs/protocol.md §1.14. C_VehicleBlowUp above is sent
// by the driver and only the driver, so a car with no driver has nobody to
// report it - and the commonest way a car is destroyed in GTA III is that
// somebody blows up a parked one.
//
// Three things are deliberately absent, and each absence is the design:
//
//   - No netId. A parked car was never introduced to the session and never
//     will be. Naming it through the server would mean announcing every
//     parked car in Liberty City so that one of them could later be named.
//
//   - No transform. A parked car is where the map put it, on every machine.
//     C_VehicleBlowUp carries one because a driven car is somewhere its
//     owner's physics decided and nobody else can know; this one would be
//     sending a machine's own copy of shared map data back to itself.
//
//   - No health, and no condition of any kind. Writing m_fHealth destroys
//     nothing (addresses.h, "a car's destruction") and a health below 250
//     arms the engine's five-second fire timer, which is an observer
//     deciding to destroy a car for itself five seconds later. Health is a
//     number; this is the event.
//
// It is NOT the only way an unowned car's destruction travels, and it is
// important that it is not. An explosion is already replayed on every
// machine at a position everyone agrees on, and CWorld::TriggerExplosion
// damages every car in the radius through CVehicle::InflictDamage with a
// multiplier that is a function of distance and nothing else (addresses.h,
// "an explosion damages every car in its radius"). So the common case
// converges by itself. This event is the backstop for the cases that do not:
// accumulated gunfire, collisions with a replica, the fire timer.
//
// Which means two machines can both report the same car in the same second,
// and that is fine rather than a race to close. The server takes the first
// report and drops the rest; a client that is already holding a wreck does
// nothing with an arriving one. docs/pickups.md's rule, in a place where the
// arbitration costs nothing because nobody is waiting on the answer.
enum UnownedVehicleKind : uint8_t {
	// The key is an index into CTheCarGenerators::CarGeneratorArray. Every
	// PARKED_VEHICLE comes out of a car generator, the generators are loaded
	// from the IPLs in file order, and the same files in the same order give
	// the same index on every machine. The map hands the name out, so the
	// server does not have to (contrast docs/population.md §1.2, where
	// nothing in the world could name a ped the traffic generator invented).
	UNOWNED_PARKED = 0,

	// The key is an ambient car's netId - the id docs/population.md §3 step 4
	// hands out - for the other half of §5.8: a RANDOM_VEHICLE whose host
	// destroys it. The wire shape was reserved here by the parked work and
	// filled by the ambient work without a protocol bump, which is what it
	// was reserved for.
	//
	// The one rule that is not the parked one: **only the machine hosting
	// the car may send it.** A parked car belongs to the map, so whoever
	// watched it burn may say so and the first report wins. A traffic car
	// belongs to the engine that generated it, everybody else holds a
	// replica whose transform is written from that machine's stream, and a
	// replica's local wreck is this machine's own opinion about somebody
	// else's car - the replicas have independent healths (AmbientCarState
	// carries no condition) so they genuinely do blow up at different times.
	// The server accepts this from the recorded owner and nobody else.
	UNOWNED_AMBIENT = 1,

	// The key is a session netId: a car a player claimed, drove, parked and
	// walked away from. It has a row in the session and a last-reported
	// transform everybody agrees on - what it does not have is a driver, and
	// C_VehicleBlowUp is only ever accepted from one. This is roadmap 5.8's
	// literal case, and the likeliest way the owner lost a car to it.
	//
	// The server accepts it only for a car it records no driver for. A car
	// with a driver stays theirs, and a report about it from anybody else is
	// still a lie about somebody else's property.
	UNOWNED_SESSION = 2,
};

struct UnownedVehicleKey {
	uint8_t  kind;   // UnownedVehicleKind
	uint8_t  pad;    // keeps `id` 2-aligned and the layout explicit
	uint16_t id;
};

// Where it blew up, and which way up it was.
//
// Read at the moment of detonation on the machine that owns the car, and used
// by exactly one of the three kinds - see UnownedVehicleKind above and the
// version 16 note at the top of this file.
//
// UNOWNED_AMBIENT needs it because the owner stops streaming a car the frame
// it becomes a wreck, so the last AmbientCarState an observer received is from
// *before* the explosion, and the replica additionally renders a buffer behind
// that. Both errors point the same way and they add up: the observer detonated
// its replica wherever the stale stream had left it.
//
// UNOWNED_PARKED ignores it and always has. A car the map generated sits at
// coordinates every machine read out of the same file, so writing a transform
// there would be a machine copying a wire float over its own map data. The
// sender fills these with zero for that kind rather than leaving them
// uninitialised, because a packet that puts unread bytes on a socket is a
// packet nobody can debug.
struct BlastTransform {
	Vec3 pos;
	Quat rot;
};

struct C_UnownedBlowUp {
	static constexpr uint8_t OPCODE = OP_C_UNOWNED_BLOWUP;
	PacketHeader      hdr;
	UnownedVehicleKey key;
	BlastTransform    where;
};

// Relayed to everyone except the reporter, and replayed in the backfill for
// anyone who joins within the wreck's lifetime. `reporterPlayerId` is for the
// log and nothing else: nobody owns this car, so there is nothing the
// reporter's identity entitles them to.
struct S_UnownedBlowUp {
	static constexpr uint8_t OPCODE = OP_S_UNOWNED_BLOWUP;
	PacketHeader      hdr;
	uint8_t           reporterPlayerId;
	uint8_t           pad[3];
	UnownedVehicleKey key;
	BlastTransform    where;
};

// ---- what shape a car is in (CH_EVENT) -----------------------------------
//
// docs/cardamage.md is the design; this is what a reader of the wire needs.
//
// Health travelled from the first version of the vehicle work and damage never
// did, so the same car is dented, missing a door and pristine depending on
// whose screen you look at. Almost all of that gap closes without a packet,
// and the three fields that are NOT here are the interesting part:
//
//   - **wheels.** m_wheelStatus has three writers in the whole image and two
//     of them are unreachable in retail 1.0: ProgressWheelDamage is only
//     called from ApplyDamage's COMPGROUP_WHEEL arm and nothing ever passes it
//     a wheel, and CAutomobile::BurstTyre has no call site at all - its
//     address appears once in the file, in a vtable slot nothing dispatches.
//     What is left is FuckCarCompletely, which runs inside a BlowUpCar that
//     protocol.md §1.11 already replays on every machine. Wheels converge on
//     their own.
//   - **lights.** SetLightStatus has one caller, is always passed the literal
//     1, and sits two instructions from the ProgressPanelDamage call for the
//     same panel - so a broken light is exactly a damaged panel, and the
//     receiver derives it.
//   - **the engine status.** CAutomobile::VehicleDamage's tail recomputes it
//     from m_fHealth on every frame on every machine, and health has been on
//     the wire since M2. Below 225 the value only picks a particle density.
//
// What is left is collisions, which is precisely the damage nobody simulates
// twice: gunfire and explosions reach CVehicle::InflictDamage and take health
// and nothing else, while a dent comes from m_fDamageImpulse, which the local
// collision solver writes about a car whose transform is being corrected off
// this very socket.
//
// Two properties make the arbitration easy, and both are measured rather than
// assumed:
//
// 1. **It is absolute state, not a delta.** Panels and doors are what the car
//    is wearing, not what just happened to it. A duplicate is a no-op and a
//    drop is repaired by the next report.
// 2. **It only ever climbs.** ProgressPanelDamage and ProgressDoorDamage both
//    refuse at 3 and nothing but CAutomobile::Fix lowers either, so the merge
//    is a componentwise maximum - commutative, associative, idempotent. Order
//    and sender cannot change where the session lands. That is what lets this
//    reuse roadmap.md §5.8's reporter rules unchanged instead of inventing a
//    fourth ownership model.
struct VehicleDamageBody {
	uint16_t netId;

	// CDamageManager::m_panelStatus verbatim: four bits per panel, seven
	// panels used (four wings, the windscreen, two bumpers). The engine's own
	// word, because ProgressPanelDamage is the only thing that moves one.
	uint32_t panels;

	// Six doors, two bits each, low door first, in eDoors order: bonnet,
	// boot, front left, front right, rear left, rear right.
	//
	// **Not m_doorStatus.** That byte has four values and two of them are a
	// door somebody opened rather than a door somebody broke - a ped getting
	// in writes DOOR_STATUS_SWINGING over DOOR_STATUS_MISSING unconditionally,
	// so the byte can go *down* while the car still has a hole in it, because
	// nothing in the engine puts a hidden atomic back except a respray. What
	// travels is a damage level: 0 nothing, 1 smashed, 2 gone.
	uint16_t doors;
};

struct C_VehicleDamage {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_DAMAGE;
	PacketHeader      hdr;
	VehicleDamageBody body;
};

// Relayed to everyone except the reporter, whose own engine produced it, and
// replayed to a late joiner out of the session's own record so a car that has
// been missing its boot for ten minutes arrives without one (§2.8).
//
// `playerId` is who said so. The server checks entitlement before relaying -
// the driver of that car, and nobody else, exactly as for C_VehicleState - so
// by the time this goes out the field is for the log.
struct S_VehicleDamage {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DAMAGE;
	PacketHeader      hdr;
	uint8_t           playerId;
	uint8_t           pad[3];
	VehicleDamageBody body;
};

// How many of each the wire speaks. These are the engine's own counts -
// CDamageManager has uint8 m_doorStatus[6], and ApplyDamage reaches seven
// panel indices (four wings, the windscreen, two bumpers) - but they are
// written here, in the sdk, because the server has no addresses.h and must not
// grow one. client/src/game/cardamage.h static_asserts them against the values
// read out of gta3.exe, so the two agree with each other rather than each
// agreeing only with itself.
constexpr unsigned VEH_DAMAGE_PANELS = 7;
constexpr unsigned VEH_DAMAGE_DOORS  = 6;

// A door level, two bits: 0 nothing, 1 smashed, 2 gone. Deliberately not the
// engine's own eDoorStatus - see VehicleDamageBody::doors.
constexpr uint8_t VEH_DOOR_LEVEL_MAX  = 2;
// A panel level is the engine's ePanelStatus verbatim, four bits, 0..3.
constexpr uint8_t VEH_PANEL_LEVEL_MAX = 3;

constexpr uint16_t VEH_DAMAGE_DOOR_MASK =
    static_cast<uint16_t>((1u << (VEH_DAMAGE_DOORS * 2)) - 1u);
constexpr uint32_t VEH_DAMAGE_PANEL_MASK =
    (VEH_DAMAGE_PANELS >= 8) ? 0xFFFFFFFFu
                             : ((1u << (VEH_DAMAGE_PANELS * 4)) - 1u);

inline uint8_t GetPanelLevel(uint32_t panels, unsigned panel) {
	if (panel >= VEH_DAMAGE_PANELS)
		return 0;
	return static_cast<uint8_t>((panels >> (panel * 4)) & 0xF);
}

inline void SetPanelLevel(uint32_t &panels, unsigned panel, uint8_t level) {
	if (panel >= VEH_DAMAGE_PANELS)
		return;
	if (level > VEH_PANEL_LEVEL_MAX)
		level = VEH_PANEL_LEVEL_MAX;
	const unsigned shift = panel * 4;
	panels = (panels & ~(0xFu << shift)) | (static_cast<uint32_t>(level) << shift);
}

inline uint8_t GetDoorLevel(uint16_t doors, unsigned door) {
	if (door >= VEH_DAMAGE_DOORS)
		return 0;
	return static_cast<uint8_t>((doors >> (door * 2)) & 0x3);
}

inline void SetDoorLevel(uint16_t &doors, unsigned door, uint8_t level) {
	if (door >= VEH_DAMAGE_DOORS)
		return;
	if (level > VEH_DOOR_LEVEL_MAX)
		level = VEH_DOOR_LEVEL_MAX;
	const unsigned shift = door * 2;
	doors = static_cast<uint16_t>((doors & ~(0x3u << shift)) |
	                              (static_cast<unsigned>(level) << shift));
}

// Everything a sender never sets, dropped on receipt.
//
// This is not tidiness and it is the fourth time this project has needed it.
// CDamageManager::SetDoorStatus is `mov byte [ecx+edx+9],al` with no compare
// in front of it, so a door index of 24 writes a byte into m_panelStatus and
// an index of 0x100 writes one into a CDoor. SetPanelStatus and
// SetLightStatus are gentler only by accident - they compute a shift and x86
// `shl` masks the count to five bits, so a panel of 8 quietly rewrites panel 0
// instead of corrupting anything. Same missing check, different failure.
//
// The wire format has no room to express a bad index, and these two are what
// make that true of the bytes that actually arrive rather than only of the
// bytes a well-behaved sender puts on.
inline uint16_t CleanDoorWord(uint16_t doors) {
	uint16_t out = static_cast<uint16_t>(doors & VEH_DAMAGE_DOOR_MASK);
	for (unsigned i = 0; i < VEH_DAMAGE_DOORS; ++i)
		if (GetDoorLevel(out, i) > VEH_DOOR_LEVEL_MAX)
			SetDoorLevel(out, i, VEH_DOOR_LEVEL_MAX);
	return out;
}

inline uint32_t CleanPanelWord(uint32_t panels) {
	uint32_t out = panels & VEH_DAMAGE_PANEL_MASK;
	for (unsigned i = 0; i < VEH_DAMAGE_PANELS; ++i)
		if (GetPanelLevel(out, i) > VEH_PANEL_LEVEL_MAX)
			SetPanelLevel(out, i, VEH_PANEL_LEVEL_MAX);
	return out;
}

// Componentwise maximum, and the whole of the arbitration.
//
// Because every ladder in CDamageManager climbs and none of them descends,
// max is what the engine would have produced if one machine had simulated
// every collision. It is commutative, associative and idempotent, so a
// duplicate changes nothing, a reordering changes nothing, and two machines
// that each saw half of a shunt agree on the union of the two halves.
inline void MergeDamage(uint32_t &panels, uint16_t &doors,
                        uint32_t addPanels, uint16_t addDoors) {
	addPanels = CleanPanelWord(addPanels);
	addDoors  = CleanDoorWord(addDoors);
	for (unsigned i = 0; i < VEH_DAMAGE_PANELS; ++i) {
		const uint8_t b = GetPanelLevel(addPanels, i);
		if (b > GetPanelLevel(panels, i))
			SetPanelLevel(panels, i, b);
	}
	for (unsigned i = 0; i < VEH_DAMAGE_DOORS; ++i) {
		const uint8_t b = GetDoorLevel(addDoors, i);
		if (b > GetDoorLevel(doors, i))
			SetDoorLevel(doors, i, b);
	}
}

// The one thing that lowers a car's damage, and the only exception to the
// monotone join above.
//
// A Pay'n'Spray calls CAutomobile::Fix, which puts every panel and every door
// back; nothing else in the engine does. The damage work left the spray shop
// out of scope on the stated grounds that garages were wholly unsynced, and
// the garage work then built it, so the two met here. Merged as a maximum a
// repair says nothing at all - the word it would send is zero and zero is the
// identity of a join - and three things go wrong at once: a resprayed car
// stays dented on every screen but its owner's, the server goes on handing the
// old dents to joiners, and the owner's own high-water mark blocks every
// later dent below it from ever being reported again.
//
// So a repair is the same packet with this bit set. It rides the top nibble of
// `panels`, which is free: seven panels take four bits each, so CleanPanelWord
// masks everything above bit 27 off. A sender that does not know about it
// cannot produce it by accident, a receiver that cleans the word first cannot
// mistake it for damage, and nothing about the layout moves.
//
// It is absolute, not a delta: a receiver that honours it sets the car's
// damage to nothing rather than subtracting anything. Duplicates are still
// harmless and order still does not matter among repairs; what does matter is
// that a repair and a dent are ordered with respect to each other, which is
// why both travel on the reliable, ordered event channel.
constexpr uint32_t VEH_DAMAGE_RESET = 1u << 31;

inline bool IsDamageReset(uint32_t panels) {
	return (panels & VEH_DAMAGE_RESET) != 0;
}

// True when the second word says anything the first does not. What decides
// whether a packet goes out at all, and on most ticks of most sessions the
// answer is no.
inline bool DamageGrew(uint32_t havePanels, uint16_t haveDoors,
                       uint32_t addPanels, uint16_t addDoors) {
	uint32_t p = havePanels;
	uint16_t d = haveDoors;
	MergeDamage(p, d, addPanels, addDoors);
	return p != havePanels || d != haveDoors;
}

// ---- world (CH_EVENT) ----------------------------------------------------

// Time of day and sky, as one player's game has them.
//
// The session follows a designated host player rather than a clock the
// server keeps on its own. The server has no GTA III running, so a clock it
// invented is nobody's; the host has one, the campaign's script can move it
// (SET_TIME_OF_DAY, FORCE_WEATHER), and docs/campaign.md already puts the
// script on the host. §2.7 is the argument in full.
//
// Two weather types because CWeather doesn't have one. It blends from
// OldWeatherType to NewWeatherType across a game hour, so a single type
// describes the destination and not the sky. The blend position isn't sent:
// CWeather::Update recomputes it as CClock::GetMinutes()/60 every frame, so
// once the clock matches, the blend matches for free.
struct WorldStateBody {
	uint8_t hour, minute;
	uint8_t weather;        // eWeatherType: 0 sunny, 1 cloudy, 2 rainy, 3 foggy
	uint8_t weatherOld;     // the one being blended out of
};

// Only the host sends this, once a second. The server drops it from anyone
// else, the same way it drops a vehicle snapshot from a player who isn't
// driving that vehicle.
struct C_WorldState {
	static constexpr uint8_t OPCODE = OP_C_WORLD_STATE;
	PacketHeader   hdr;
	WorldStateBody body;
};

struct S_WorldState {
	static constexpr uint8_t OPCODE = OP_S_WORLD_STATE;
	PacketHeader   hdr;
	WorldStateBody body;
	// The current host. Carried on every world packet rather than announced
	// once, because it's how a client finds out it has become the host after
	// the previous one quit, and because it's free here.
	uint8_t        hostPlayerId;
};

// ---- ambient population (CH_EVENT) ---------------------------------------
//
// A pedestrian the local engine made, on its way to being one entity the
// whole session shares. docs/population.md §1.1 and §1.2.
//
// Everything about these four packets follows from one fact: **the machine
// that creates a ped cannot name it.** Both machines' CPopulation are running
// at the same time and would pick the same number, so the creator announces
// it under a `tempId` of its own choosing, the server allocates the netId,
// and the creator swaps one for the other when the answer comes back. The
// same two-phase spawn as a vehicle claim, with one more phase on the front.
//
// `tempId` is 32-bit and never leaves the creator's machine except in these
// two packets. It is not a netId, it is not unique across the session, and
// nothing may key on it after S_PedSpawn has been handled.
struct AmbientPedBody {
	uint16_t modelId;
	uint8_t  pedType;    // ePedType, so the replica lands in the same engine
	                     // counter as the original - see population.md §1.3
	uint8_t  pad;        // keeps pos 4-aligned and the layout explicit
	Vec3     pos;
	float    heading;
};

struct C_PedSpawn {
	static constexpr uint8_t OPCODE = OP_C_PED_SPAWN;
	PacketHeader   hdr;
	uint32_t       tempId;
	AmbientPedBody body;
};

// The server's answer, sent to everybody including the creator.
//
// The creator recognises its own by `ownerPlayerId` *and* `tempId`; everyone
// else ignores `tempId` entirely and creates a replica. A backfilled ped -
// one that existed before the receiver joined - carries tempId 0, which no
// creator ever allocates, so a joiner cannot mistake somebody else's ped for
// one of its own.
struct S_PedSpawn {
	static constexpr uint8_t OPCODE = OP_S_PED_SPAWN;
	PacketHeader   hdr;
	uint8_t        ownerPlayerId;
	uint32_t       tempId;
	uint16_t       netId;
	AmbientPedBody body;
};

// The owner's engine took this ped away. Only the owner may say so - an
// observer that has lost its replica says nothing and tries again, because
// the entity still exists where it is hosted.
struct C_PedDespawn {
	static constexpr uint8_t OPCODE = OP_C_PED_DESPAWN;
	PacketHeader hdr;
	uint16_t     netId;
};

struct S_PedDespawn {
	static constexpr uint8_t OPCODE = OP_S_PED_DESPAWN;
	PacketHeader hdr;
	uint16_t     netId;
};

// ---- a limb coming off (CH_EVENT) ------------------------------------------
//
// The host's engine has just run CPed::RemoveBodyPart on one of its own
// pedestrians, and every observer runs it on the replica with the same
// arguments. Version 17.
//
// Only the host can say it. The shot or blast that takes a limb off is
// resolved on the machine that owns the ped - an observer's replica is proof
// against both, so its own engine never gets as far as the limb - and which
// limb, if any, is a CGeneral::GetRandomNumber roll nobody else could repeat.
//
// Reliable and ordered, on the same channel as the despawn: a limb announced
// after the ped it belongs to has been taken away would find nothing, and one
// sent unreliably is a head that stays on for whoever lost the packet.
//
// `node` is a PedNode (re3 PedModelInfo.h): 2 head, 3 and 4 the upper arms,
// 7 and 8 the upper legs - the only five InflictDamage ever passes.
// `direction` is the side the hit came from, 0 front, 1 left, 2 back, 3
// right, which decides which way the limb flies.
struct PedBodyPartBody {
	uint16_t netId;
	uint8_t  node;
	int8_t   direction;
};

// The five PedNodes above, and nothing else. Checked by the server before it
// relays and by the client before it calls the engine, because the node is an
// index into CPed::m_pFrames and a bad one is a read off the end of it.
constexpr bool IsRemovableBodyPart(uint8_t node) {
	return node == 2 || node == 3 || node == 4 || node == 7 || node == 8;
}

struct C_PedBodyPart {
	static constexpr uint8_t OPCODE = OP_C_PED_BODY_PART;
	PacketHeader    hdr;
	PedBodyPartBody body;
};

// To everybody but the host, whose engine already did it. No record is kept:
// a late joiner is handed the ped with all its limbs, which is a corpse that
// lies there for as long as the host's engine keeps it and is not worth a
// table on the server.
struct S_PedBodyPart {
	static constexpr uint8_t OPCODE = OP_S_PED_BODY_PART;
	PacketHeader    hdr;
	PedBodyPartBody body;
};

// ---- an ambient pedestrian dying (CH_EVENT) --------------------------------
//
// The one thing about a hosted ped that nothing else on the wire can say.
//
// A death reaches observers today only as whatever animation happens to be
// dominant in the next C_PedStates row, and that is not a death: the stream
// carries the twelve peds nearest the *sender* (MAX_PED_STATES), a corpse is
// rarely among them for long, and ApplyAmbientPedState refuses to drive
// anything into a replica that is already dead without ever being the thing
// that makes one dead. So you shoot a pedestrian, he drops on your screen,
// and on every other screen he keeps walking.
//
// Only the host may say it, the same rule as the despawn and the limb, and
// for a stronger reason than either: every replica is bullet-, fire-, melee-
// and collision-proof, so an observer's engine can never reach the death
// itself. The host's own CPed::SetDie detour is the only witness there is.
//
// `animId` is the animation the host's engine chose, taken straight off that
// SetDie call - exactly what C_Death carries for a player, and for the same
// reason. A headshot, a drowning and a car knocking someone over are three
// different animations and the observer has no way to work out which. It is
// what makes a corpse lie the way it fell rather than in the default front
// knockdown. ANIM_NONE means the host could not capture one.
//
// Reliable and ordered, on CH_EVENT beside the limb and the despawn, and the
// ordering between those three is the whole reason it is not a snapshot flag:
// a limb belongs to a ped that is still there, a death to a ped that is still
// there, and a despawn ends both.
struct PedDeathBody {
	uint16_t netId;
	uint16_t animId;
};

struct C_PedDeath {
	static constexpr uint8_t OPCODE = OP_C_PED_DEATH;
	PacketHeader hdr;
	PedDeathBody body;
};

// To everybody but the host, whose engine already did it - and, unlike the
// limb, this one *is* kept. The session records that the ped is dead and the
// animation it died in, so a joiner is handed a corpse rather than a
// pedestrian standing in a pool of somebody else's blood. See Session::
// NotePedDeath and BuildBackfill.
struct S_PedDeath {
	static constexpr uint8_t OPCODE = OP_S_PED_DEATH;
	PacketHeader hdr;
	PedDeathBody body;
};

// ---- the ambient ped stream (CH_SNAPSHOT) ---------------------------------
//
// One hosted pedestrian, as its owner's engine has it right now.
//
// **What is in here was chosen by subtraction from PlayerStateBody, and
// every omission has a reason.** The whole argument is bandwidth
// (population.md 2.1): there are two to three times as many pedestrians
// around a player as there are cars, so a ped row that costs what a car row
// costs is a stream that costs three times what traffic costs.
//
//   - `pos` and `heading`, obviously. A statue is a ped whose position never
//     arrives.
//
//   - `animId`, and it is not optional and not obvious. The move-state
//     mechanism **has never worked** (docs/protocol.md 1.13.4): CPed::Idle's
//     non-still arm ends in `if (!IsPlayer()) SetMoveState(PEDMOVE_STILL)`
//     and it runs before SetMoveAnim, so an m_nMoveState written from
//     outside is overwritten before anything reads it. Remote players walk
//     because ApplyAnimation blends the wire's animId directly, and that is
//     the only thing that has ever made a non-player ped walk. Two bytes
//     here is the difference between a pedestrian walking and a pedestrian
//     gliding along the pavement in his idle pose.
//
//   - `vehicleNetId` and `seat`, which are the traffic driver. See below.
//
// Left out, and what each one would have cost:
//
//   - **velocity (12 bytes, +50%).** A car carries it because at 25 m/s and
//     10 Hz the interpolation buffer runs dry between samples the moment one
//     is lost, and a car extrapolated wrongly is metres out. A pedestrian
//     walks at 1.5 m/s and runs at about 4: a whole missed sample is 40 cm,
//     and holding still for 100 ms is cheaper to look at than a mis-guessed
//     sprint. The buffer is fed a zero velocity and holds rather than
//     extrapolates, which for a ped is the better of the two.
//   - **animTime / animSpeed (8 bytes).** A locomotion animation takes its
//     rate from the ped's own movement and the phase of a stranger's walk
//     cycle is not something anybody can see is wrong. A remote *player*
//     carries both because a player is looked at.
//   - **moveState (1 byte).** It is read by nothing. See above.
//   - **health, armour, weapon, aim, flags (about 20 bytes).** An ambient
//     ped is not shot at across machines: its damage, death and removal all
//     happen on its host, and the removal is what travels, as the despawn
//     that already exists. Carrying health would let an observer watch a
//     pedestrian die, which is nicer and is not what the pool pressure
//     budget is for.
//
// 24 bytes, against PlayerStateBody's 65 and AmbientCarState's 44.
struct AmbientPedState {
	uint16_t netId;
	uint16_t animId;        // ANIM_NONE when the owner has nothing to say
	// The car this pedestrian is sitting in, and the single most visible
	// thing that was wrong with the shared city: the traffic work replicated
	// a seated ped as a ped standing where it was created, while the car it
	// should have been driving drove past without him.
	//
	// Both halves are hosted by the same machine and both already have
	// netIds, so the host is the only one who can state the pairing - an
	// observer sees two unrelated entities. It rides this stream rather than
	// a one-shot event because it is a standing fact, not an event: the
	// observer's ped and car replicas are created independently, either can
	// be lost and rebuilt, and a restated fact heals all of that by itself
	// where an event has to handle each race by hand. That is the lesson
	// docs/protocol.md 2.8.2 records from the player seating work.
	uint16_t vehicleNetId;  // INVALID_NETID when on foot
	uint8_t  seat;          // 0 = driver, 1.. = passenger
	uint8_t  pad;           // keeps pos 4-aligned and the layout explicit
	Vec3     pos;
	float    heading;
};

// Twelve, against the traffic stream's eight, and both numbers come out of
// the same sum rather than out of symmetry.
//
// population.md 1.3.1 measured a machine hosting 7 pedestrians while the
// session held 11; 1.3.2 measured 3 traffic cars. Pedestrians outnumber
// traffic roughly two to one and they are a third cheaper each, so twelve
// ped rows cost 12 x 24 x 10 = 2.9 KB/s where eight car rows cost 3.5. Both
// streams together are 6.4 KB/s each way per observer, against the 13 KB/s
// 2.1 says a *dozen cars alone* would cost at the player rate.
//
// The thirteenth pedestrian and beyond is held where it was last heard of,
// which is 2.1's far band, and it is only ever a ped nobody is standing
// next to.
constexpr uint8_t MAX_PED_STATES = 12;

struct C_PedStates {
	static constexpr uint8_t OPCODE = OP_C_PED_STATES;
	PacketHeader    hdr;
	uint8_t         count;   // how many of `peds` are real; the rest is padding
	uint8_t         pad[3];
	AmbientPedState peds[MAX_PED_STATES];
};

struct S_PedStates {
	static constexpr uint8_t OPCODE = OP_S_PED_STATES;
	PacketHeader    hdr;
	uint8_t         ownerPlayerId;
	uint8_t         count;
	uint8_t         pad[2];
	AmbientPedState peds[MAX_PED_STATES];
};

// ---- ambient traffic (CH_EVENT for the handshake, CH_SNAPSHOT for state) ---
//
// docs/population.md §3 step 4. The handshake is the pedestrian one with the
// names changed, and for the same reason: two machines' CCarCtrl are running
// at the same moment and would pick the same number, so the creator announces
// under a `tempId` of its own and the server hands back the netId.
//
// What a car carries that a ped does not is everything that decides what it
// looks like. A pedestrian is a model index; a car is a model, two colours
// and two extra components that the engine rolls *per machine* at
// construction (docs/protocol.md §1.12) - so without these on the wire, every
// observer builds a differently-painted car with different bits bolted to it
// and blames the mod.
//
// The rotation is a quaternion rather than a heading, for the reason
// EnterVehicleBody's is: a car pitches over kerbs and lands on its roof, and
// a yaw-only sync deletes all of that.
struct AmbientCarBody {
	uint16_t modelId;
	uint8_t  colour1, colour2;
	int8_t   extra1, extra2;   // CVehicle::m_aExtras, -1 for an empty slot
	uint8_t  pad[2];           // keeps pos 4-aligned and the layout explicit
	Vec3     pos;
	Quat     rot;
};

struct C_CarSpawn {
	static constexpr uint8_t OPCODE = OP_C_CAR_SPAWN;
	PacketHeader   hdr;
	uint32_t       tempId;
	AmbientCarBody body;
};

// The server's answer, to everybody including the creator. Read exactly like
// S_PedSpawn: the creator matches on `ownerPlayerId` *and* `tempId`, everyone
// else ignores `tempId`, and a backfilled car carries tempId 0.
struct S_CarSpawn {
	static constexpr uint8_t OPCODE = OP_S_CAR_SPAWN;
	PacketHeader   hdr;
	uint8_t        ownerPlayerId;
	uint32_t       tempId;
	uint16_t       netId;
	AmbientCarBody body;
};

struct C_CarDespawn {
	static constexpr uint8_t OPCODE = OP_C_CAR_DESPAWN;
	PacketHeader hdr;
	uint16_t     netId;
};

struct S_CarDespawn {
	static constexpr uint8_t OPCODE = OP_S_CAR_DESPAWN;
	PacketHeader hdr;
	uint16_t     netId;
};

// One hosted car's transform, as its owner's engine has it right now.
//
// `velocity` is CPhysical::m_vecMoveSpeed and it is not decoration: the
// observer's interpolation buffer extrapolates along it when the stream runs
// dry, and at 10 Hz with a car doing 25 m/s it runs dry between every pair of
// samples the moment a packet is lost. Without it a replica stutters to a
// halt and jumps, twice a second.
struct AmbientCarState {
	uint16_t netId;
	uint8_t  pad[2];
	Vec3     pos;
	Quat     rot;
	Vec3     velocity;
};

// Eight, and the number is the design rather than a round figure.
//
// population.md §2.1: a dozen cars at the player rate is already most of the
// budget, and a flat rate is not on the table. Eight nearest cars at 10 Hz is
// 8 × 44 × 10 = 3.5 KB/s each way per observer, against 13 KB/s for the same
// dozen cars at 25 Hz. The ninth car and beyond is left where it was last
// seen, which is §2.1's "far but still in the world: nothing" - and it is
// only ever a car nobody is near.
constexpr uint8_t MAX_CAR_STATES = 8;

struct C_CarStates {
	static constexpr uint8_t OPCODE = OP_C_CAR_STATES;
	PacketHeader    hdr;
	uint8_t         count;   // how many of `cars` are real; the rest is padding
	uint8_t         pad[3];
	AmbientCarState cars[MAX_CAR_STATES];
};

struct S_CarStates {
	static constexpr uint8_t OPCODE = OP_S_CAR_STATES;
	PacketHeader    hdr;
	uint8_t         ownerPlayerId;
	uint8_t         count;
	uint8_t         pad[2];
	AmbientCarState cars[MAX_CAR_STATES];
};

// ---- chat (CH_EVENT) -----------------------------------------------------

struct C_Chat {
	static constexpr uint8_t OPCODE = OP_C_CHAT;
	PacketHeader hdr;
	char text[CHAT_LEN];
};

struct S_Chat {
	static constexpr uint8_t OPCODE = OP_S_CHAT;
	PacketHeader hdr;
	uint8_t playerId;
	char    text[CHAT_LEN];
};

// ---- pickups (CH_EVENT) --------------------------------------------------
//
// docs/pickups.md is the whole design; the two things a reader of this header
// needs are why there is no spawn packet and why there is no snapshot.
//
// No spawn packet, because there is nothing to spawn. Every machine runs
// main.scm, which creates all 448 script pickups from literal coordinates
// through a CPickups::GenerateNewOne that never touches the RNG - so the
// worlds already agree, and a create packet would only double them. What was
// missing was never the pickup, it was the exclusivity: two players standing
// on the same shotgun and both engines awarding it.
//
// No snapshot, because a pickup is not continuous state. It is a small number
// of discrete facts - taken, denied, live again - and putting "who got the
// shotgun" on the unreliable channel is the mistake 2.8 already made once
// with driverPlayerId.

// A pickup, named by what it is and where it is.
//
// Deliberately not the slot index. The engine's own handle is
// `slot | (generation << 16)` and it is per process: CPickups::GenerateNewOne
// hands out the first free slot in [0,320), and ped drops allocate out of the
// same range, so one player killing a pedestrian in traffic moves that
// machine's cursor and every later script pickup lands somewhere different
// from everyone else's. Slot agreement survives about a minute of play.
//
// Position and model do not have that problem, and they keep working when M5
// takes the script away from clients and the host starts replicating creation
// instead - the identity does not care who made the pickup.
//
// Matched by nearest-within-0.25 m of the same model rather than by an exact
// float compare. Script pickups really are bit-identical on both machines;
// a ped drop's z comes out of CWorld::FindGroundZFor3DCoord and this design
// should not depend on that landing on the same bit in two processes.
struct PickupIdent {
	Vec3     pos;
	int16_t  modelIndex;
	uint8_t  type;       // ePickupType; the server needs it for the window
	uint8_t  flags;      // PickupIdentFlags
};

enum PickupIdentFlags : uint8_t {
	// This model is MI_PICKUP_BRIBE. One bit rather than the model index,
	// because the pickup model indices are *runtime* globals - CModelInfo
	// fills them from the IDE at load, so the number differs by install and
	// only the game process knows it. The server needs exactly one thing
	// from it: a bribe's PICKUP_ON_STREET_SLOW window is 300 s and everything
	// else's is 720 s.
	PICKUP_F_BRIBE = 1 << 0,
};

// "I am near this pickup and I want it."
//
// Sent on *approach*, not on contact - the client claims at 4 m and the
// engine collects at 1.34 m, so the answer is in hand well before the player
// arrives. That is what makes the arbitration come before the award instead
// of after it, which matters because a pickup reward cannot honestly be taken
// back: ammo already fired, health already spent, a bribe that has already
// cleared a star.
struct C_PickupClaim {
	static constexpr uint8_t OPCODE = OP_C_PICKUP_CLAIM;
	PacketHeader hdr;
	PickupIdent  ident;
};

// "It is yours - take it." To the claimant alone.
//
// A *reservation*, not a collection. It unblocks the claimant's own copy so
// the engine's award path can run on it, and it says nothing to anybody else,
// because at this point nobody has picked anything up. That distinction is
// the whole reason this opcode exists separately from S_PickupTaken: the
// claim goes out on approach, at 4 m, and a player who walks past a health
// pickup at full health never collects it. Treating a grant as a collection
// would have somebody strolling down a street quietly deleting every pickup
// on it from everyone else's world.
struct S_PickupGrant {
	static constexpr uint8_t OPCODE = OP_S_PICKUP_GRANT;
	PacketHeader hdr;
	PickupIdent  ident;
};

// "I actually took it." From the holder, once their own engine has.
//
// Detected rather than decided: the seam holds the reservation, the engine
// collects through its own award switch, and the slot going empty underneath
// is what this reports. So CoopIII never has to predict what
// CPickup::Update would have done - it reads what it did.
struct C_PickupCollected {
	static constexpr uint8_t OPCODE = OP_C_PICKUP_COLLECTED;
	PacketHeader hdr;
	PickupIdent  ident;
};

// "Player P has taken this one." To everybody except P.
//
// P's own engine has already removed their copy, which is what produced the
// message. Everybody else replays the engine's removal tail on their copy and
// pushes the collection into CPickups::aPickUpsCollected, without which the
// observer's rampage and reward scripts never notice it happened.
struct S_PickupTaken {
	static constexpr uint8_t OPCODE = OP_S_PICKUP_TAKEN;
	PacketHeader hdr;
	uint8_t      playerId;
	PickupIdent  ident;
};

// "Somebody else got there first, or it is not back yet."
//
// To the loser only. Their engine never saw an object there, so there is
// nothing to undo - this only ends the claim so it can be made again after
// the window.
struct S_PickupDenied {
	static constexpr uint8_t OPCODE = OP_S_PICKUP_DENIED;
	PacketHeader hdr;
	PickupIdent  ident;
};

// "That one is live again on my machine."
//
// Three jobs, all about letting go of a lock:
//
//   - a grant that could not be consumed, because the local object went away
//     between the claim and the answer. The server reopens the window at
//     once rather than holding a pickup nobody has.
//   - a reservation the holder walked away from. Claiming at 4 m means
//     claiming things you turn out not to want - a health pickup at full
//     health, an armour you decided against, anything you simply walked past.
//     The reservation ends when the player leaves the radius and the pickup
//     is available again immediately, on every machine, having never been
//     removed from any of them.
//   - a key that has been *reused*. The script destroys and re-creates
//     pickups at the same coordinate - every one of the 20 rampages is
//     re-created at its original spot after two failures - so a taken-record
//     for a never-respawning type is a lock, not a tombstone. A client whose
//     engine has a live object at a key that client previously removed on an
//     S_PickupTaken says so once, and the server drops the record.
struct C_PickupRelease {
	static constexpr uint8_t OPCODE = OP_C_PICKUP_RELEASE;
	PacketHeader hdr;
	PickupIdent  ident;
};

// ---- the one pickup nobody else's engine makes ----------------------------
//
// A pickup a dead pedestrian left behind, as the machine that owns that
// pedestrian actually created it. docs/pickups.md 10.
//
// **Read back, not predicted.** The host snapshots CPickups::aPickUps around
// CPed::CreateDeadPedMoney / CreateDeadPedWeaponPickups, lets the engine run,
// and sends what appeared. So nothing here re-derives the money roll, the
// scatter angle, the ground height or the ammo cap - it reports the numbers
// the engine wrote into its own table, and the observers hand those same
// numbers straight back to CPickups::GenerateNewOne.
//
// `quantity` is the one field PickupIdent has no room for and the one field
// that cannot be worked out anywhere else. For money it is the amount, rolled
// from CGeneral::GetRandomNumber on the host and meaningless to guess at; for
// a weapon it is Min(m_nAmmoTotal, AmmoForWeapon_OnStreet[weapon]), which
// depends on an inventory that never goes on the wire.
struct PickupDropBody {
	PickupIdent ident;
	uint16_t    quantity;   // CPickup::m_nQuantity - money, or rounds
	uint16_t    pad;        // keeps the body a round 20 and the layout explicit
};

// "My engine just made this one." One packet per pickup, reliable.
//
// Not batched, and that is a decision rather than an oversight. The worst
// case is one death producing 18 money pickups (the 1-in-60 `money == 43`
// arm pays 700) plus up to 13 weapons, and it is *rare*: a batched packet
// would save about 500 bytes on an event that happens a few times a minute,
// at the cost of a count, a bound and a truncation rule on a path where
// truncation means a pickup that exists on one machine and not the others.
// Each drop is arbitrated independently by the existing exchange anyway, so
// there is nothing to keep together.
struct C_PickupDrop {
	static constexpr uint8_t OPCODE = OP_C_PICKUP_DROP;
	PacketHeader   hdr;
	PickupDropBody body;
};

// To everybody except the machine that made it, whose engine already has it.
//
// There is deliberately no server-side table behind this and no backfill.
// GenerateNewOne stamps a drop with an absolute expiry the moment it makes
// one - `now + 20 s` for PICKUP_ONCE_TIMEOUT (a weapon) and `now + 30 s` for
// PICKUP_MONEY, read off 0x004305A5 and 0x004305BB - so every copy dies on
// its own clock within half a minute of being born, and the only thing a
// joiner could be told about is something that will be gone before they
// finish loading. What the server does have to do is treat the key like any
// other from then on, which it already does, because the ident is a position
// and a model and does not care who created the pickup.
struct S_PickupDrop {
	static constexpr uint8_t OPCODE = OP_S_PICKUP_DROP;
	PacketHeader   hdr;
	uint8_t        playerId;   // whose ped dropped it
	PickupDropBody body;
};

// ---- garages, doors and the Pay'n'Spray (CH_EVENT) -----------------------
//
// docs/protocol.md §1.16 is the design. What a reader of this file needs:
//
// GTA III has 32 garages and every one of them is created by main.scm from
// literal coordinates, so - exactly like the 448 script pickups
// (docs/pickups.md) - the worlds already agree about where they are and what
// type each one is. Nothing about a garage has to be *spawned*. What is
// missing is only that CGarage::Update asks FindPlayerPed() and
// FindPlayerVehicle(), i.e. the local player and nobody else, so a garage
// that opens for one player is shut for everybody else.
//
// **The transition travels, the door position does not.** A garage's state
// machine has two kinds of transition and they are not the same kind of
// thing:
//
//   - out of GS_OPENING or GS_CLOSING: *derived*. Ramp m_fDoorPos by the
//     door's fixed speed times CTimer::ms_fTimeStep, call UpdateDoorsHeight,
//     and on reaching the limit take the resting state and play a sound.
//     Every machine can compute this for itself from the state alone, at its
//     own frame rate, and gets the sound and the camera for free.
//   - out of a resting state: *decided*, from the local player's position,
//     car, money and wanted level.
//
// Only the decided ones are anybody's news. This is the same argument that
// put a car's destruction on the wire as an event rather than as m_fHealth
// (§1.11): the door's height is a consequence, and shipping a consequence at
// 25 Hz while the thing that causes it changes six times a visit is both
// more traffic and less information. It would also be *wrong*: m_fDoorPos is
// meaningless without m_fDoorHeight and the door CEntity, which are the
// map's, differ per garage, and are resolved to a pool pointer per machine.
// Same shape as roadmap.md §5.8's parked car - the map put it there on every
// machine, so no transform travels.
//
// **Who is authoritative: nobody, and everybody.** A garage belongs to the
// map, not to a player, so there is no owner to ask. The rule is a union:
// each machine reports whether its *own* state machine has a garage away
// from where that type of garage rests, and a garage is away from rest if
// anybody says so. Open wins for a garage that rests shut (a safehouse door
// somebody is walking into), shut wins for one that rests open (a
// Pay'n'Spray somebody is inside). It composes with any number of players,
// it needs no arbitration, and a lost report costs a door position rather
// than a fact - unlike §5.8's "first report wins", which is right for a
// one-shot event and wrong for a level.

// One bit per garage, bit i = aGarages[i]. 32 garages exactly: the loop in
// CGarages::Update is `cmp ebx,20h`, a literal, not CGarages::NumGarages.
//
// A mask and not a list because the whole city fits in four bytes and
// because the receiver wants the union, which is an OR. There is no
// per-garage sequence number and no ack: the mask is a level, it is sent
// reliably whenever it changes, and the last one received is the truth.
struct GarageMaskBody {
	uint32_t deviating;
};

// Sent when this machine's own mask changes, and never otherwise. Reliable,
// so there is no repetition to re-establish it.
struct C_GarageState {
	static constexpr uint8_t OPCODE = OP_C_GARAGE_STATE;
	PacketHeader   hdr;
	GarageMaskBody body;
};

// Relayed to everyone except the sender, and replayed to a joiner for every
// player whose mask is non-zero, so somebody who connects while another
// player is standing in their safehouse sees the door already up.
struct S_GarageState {
	static constexpr uint8_t OPCODE = OP_S_GARAGE_STATE;
	PacketHeader   hdr;
	uint8_t        playerId;
	uint8_t        pad[3];
	GarageMaskBody body;
};

// A Pay'n'Spray finished. The door half of this already travels as a mask
// bit; this is the part a door cannot carry.
//
// Three effects, three different answers:
//
//   1. **Repair.** m_fHealth = 1000, m_fFireBlowUpTimer = 0 and
//      CAutomobile::Fix(). Travels, because an observer's copy of that car
//      is still dented on its own screen and health is already understood to
//      be a number rather than an event (§1.11).
//   2. **Repaint.** Travels, and it *has* to. The colour is not rolled from
//      the RNG - the retail CVehicleModelInfo::ChooseVehicleColour is a
//      round robin over the model's own colour table
//      (`m_lastColorVariation = (last + 1) % m_numColours`) with a tiebreak
//      against whatever the local player is driving. Both halves are
//      machine-local: the cursor counts every car of that model this process
//      has ever built, and FindPlayerVehicle is a different car on every
//      machine. So two machines running it independently produce different
//      paint, which is roadmap.md §5.9's bug for the third time. The colours
//      are read back off the car *after* the owner's engine chose them and
//      written straight onto the observer's copy; no observer ever calls
//      ChooseVehicleColour.
//   3. **The wanted level.** Does NOT travel, and this packet has no field
//      for it. The wanted level is not on the wire at all yet
//      (roadmap.md §5.1, designed and unbuilt, and being worked on right
//      now by somebody else). The respray clears the wanted level of the
//      player who paid for it, on their own machine, through the engine's
//      own CWanted::Reset - which already happens and needs nothing from
//      CoopIII. What an observer must *not* do is clear its own player's
//      stars because somebody else visited a spray shop, and the seam in
//      client/src/game/garage.cpp is written so that it does not: it skips
//      the arm that would. If §5.1 ever lands a shared wanted level, that
//      skip is the one place that has to change.
//
// vehicleNetId is INVALID_NETID when the car being sprayed is not a session
// car - a player can drive an unclaimed traffic car into a Pay'n'Spray, and
// then there is nothing on the other machines to repaint. The packet still
// goes out, because the door and the sound are worth replaying on their own.
struct ResprayBody {
	uint16_t vehicleNetId;
	uint8_t  garage;      // index into aGarages, 0..31
	uint8_t  colour1;     // CVehicle::m_currentColour1, as the owner's engine chose
	uint8_t  colour2;     // CVehicle::m_currentColour2
	uint8_t  pad[3];
};

struct C_Respray {
	static constexpr uint8_t OPCODE = OP_C_RESPRAY;
	PacketHeader hdr;
	ResprayBody  body;
};

struct S_Respray {
	static constexpr uint8_t OPCODE = OP_S_RESPRAY;
	PacketHeader hdr;
	uint8_t      playerId;
	uint8_t      pad[3];
	ResprayBody  body;
};

// ---- ammunition for a weapon that is not in the player's hands -----------
//
// Why this is not in the snapshot. CPed::m_weapons is thirteen slots, and
// putting all of them on a 25 Hz stream would be 13 x 7 = 91 bytes per
// player per tick - more than the whole rest of the snapshot, to restate a
// number that changes when somebody walks over a pickup. The held weapon is
// the one that changes every time a trigger is pulled, so that one rides the
// snapshot where a dropped packet costs 40 ms and nothing else; the other
// twelve change on a pickup, a mission grant or a death, and go out once,
// reliably, when they change.
//
// Same shape C_PlayerModel settled on for the same reason: a fact about a
// player that is stable for minutes at a time, restated only on change.
//
// The held slot is deliberately never sent here. If it were, a player firing
// an Uzi would put ten reliable-ordered packets a second on channel 1, in
// front of the shots and the damage that actually need the ordering.
enum AmmoSlotFlags : uint8_t {
	// The sender actually has this weapon. Without this bit the packet says
	// "I do not have it", which is a different thing from having it with
	// nothing in it and has to travel separately.
	//
	// The engine's own test is `m_weapons[w].m_eWeaponType == w` - the
	// comparison CPed::GiveWeapon makes at 0x004CF9C9 to decide between
	// topping a weapon up and initialising it. An unowned slot still holds
	// leftover members, so "clip 0, total 0" cannot be made to mean this.
	//
	// It matters on the receiving end, not the sending one: an observer that
	// read an unowned slot as owned would call CPed::GiveWeapon for all
	// thirteen and hand every remote player the whole armoury with no
	// ammunition in any of it.
	AMMO_SLOT_OWNED = 1 << 0,
};

struct AmmoSlotBody {
	uint8_t  weapon;   // eWeaponType, the slot this is about
	uint8_t  flags;    // AmmoSlotFlags
	uint16_t clip;     // CWeapon::m_nAmmoInClip, saturated
	uint32_t total;    // CWeapon::m_nAmmoTotal
};

struct C_PlayerAmmo {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_AMMO;
	PacketHeader hdr;
	AmmoSlotBody slot;
};

struct S_PlayerAmmo {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_AMMO;
	PacketHeader hdr;
	uint8_t      playerId;
	AmmoSlotBody slot;
};

// ---------------------------------------------------------------------------
// Breakable street objects
// ---------------------------------------------------------------------------
//
// docs/objects.md. Lamp posts, traffic lights, parking meters, bins, cones,
// crates, barriers - the 1851 map-placed objects the engine lets you break.
//
// **Named by where the map put it and what it is.** Not the pool index: the
// object pool churns constantly, because CPopulation::ManagePopulation turns
// every GAME_OBJECT more than 80 m from *the local player* into a dummy and
// back again, and the streamer centres on one player (roadmap.md 2.1), so two
// machines are converting different objects on different frames from the very
// first second. Pool agreement here is worse than it is for pickups, not
// better.
//
// And unlike a pickup, the coordinate is not computed at runtime at all.
// CFileLoader::LoadObjectInstance sscanf's it out of an IPL and
// CObject::CObject(CDummyObject*) copies it into m_objectMatrix, which
// survives every conversion untouched - so the key is the same text in the
// same file on both machines, and the tolerance below is insurance rather
// than a requirement.
//
// `pos` is therefore **m_objectMatrix's position, not the entity's**. They
// are the same number until something knocks the object loose, and then the
// entity's position is exactly the thing that has stopped being a name.
//
// The tolerance is the same 0.25 m the pickups use, and it was measured the
// same way: all 1851 breakable instances in the 13 IPLs gta3.dat loads were
// compared pairwise within each model index. The closest same-model pair in
// the whole map is **0.5992 m** (two papermachn01 newspaper boxes side by
// side on a Portland pavement), no pair is under 0.50 m, and the 118 pairs
// closer than 2 m are all stacks of boxes, cones and newspaper boxes. So
// 0.25 m clears the nearest ambiguity in the city by 2.4x.
struct ObjectIdent {
	Vec3    pos;          // m_objectMatrix.GetPosition()
	int16_t modelIndex;
	uint8_t pad0;
	uint8_t pad1;
};

// How broken the sender's copy is, after its engine finished with it. Two
// bits, because CObject::ObjectDamage only has two outcomes: it sets
// bRenderDamaged (a damaged model, DAMAGE_EFFECT_CHANGE_MODEL), or it hides
// the object and takes its collision away (DAMAGE_EFFECT_SMASH_COMPLETELY and
// the four smash-with-particles effects), or - for
// DAMAGE_EFFECT_CHANGE_THEN_SMASH - the first on the first hit and the second
// on the second.
enum ObjectBreakFlags : uint8_t {
	OBJ_BREAK_RENDER_DAMAGED = 1 << 0,   // CEntity byte B bit 7
	OBJ_BREAK_SMASHED        = 1 << 1,   // !bIsVisible && !bUsesCollision
};

// "This object is broken, and this is what broke it."
//
// `amount` is the float the sender's engine actually passed to ObjectDamage,
// not a made-up large number, and it travels for one reason: the receiver
// replays the engine's own ObjectDamage with it, and the engine reads it
// twice - for the `amount * m_fCollisionDamageMultiplier > 150.0f` gate, and
// for `fDirectionZ = 0.0002f * amount`, which is how fast the debris flies.
// Handing it the real number means the cardboard box bursts the same way on
// both screens instead of exploding on one of them.
//
// `state` is what the sender's object ended up as, and the receiver replays
// until its own object matches - at most twice, which is what
// DAMAGE_EFFECT_CHANGE_THEN_SMASH needs and nothing needs more of. It is not
// a set of flags to write: writing bIsVisible by hand would skip the
// particles, the sound and the four other flags each case sets, and every one
// of those is the engine's business.
struct ObjectBreakBody {
	ObjectIdent ident;
	float       amount;
	uint8_t     state;    // ObjectBreakFlags
	uint8_t     pad[3];
};

// From the machine that owns whatever broke it. docs/objects.md 6: exactly
// one machine reports, and it is the one where the physics that did it
// actually ran - the owner of the car, or of the ped, or the host when there
// is no owner at all. An observer whose engine happens to break the same
// object locally stays quiet.
struct C_ObjectBroken {
	static constexpr uint8_t OPCODE = OP_C_OBJECT_BROKEN;
	PacketHeader    hdr;
	ObjectBreakBody body;
};

// To everybody but the reporter.
//
// Reliable, and there is deliberately no snapshot component, no server-side
// table and no backfill. A break is a latch with an 80 m horizon: the engine
// itself throws the state away when the last player leaves the block
// (CPopulation::ManagePopulation converts the object back to a pristine
// dummy), so there is nothing a late joiner could usefully be told about, and
// nothing for the server to hold. Applying the same break twice is a no-op,
// which is what makes the reliable channel enough on its own.
struct S_ObjectBroken {
	static constexpr uint8_t OPCODE = OP_S_OBJECT_BROKEN;
	PacketHeader    hdr;
	uint8_t         playerId;   // who reported it
	ObjectBreakBody body;
};

#pragma pack(pop)

// How many garages the engine has, and therefore how wide the mask is. Here
// as well as in addresses.h because the server has no addresses.h and still
// has to reject a report about garage 40.
constexpr uint32_t NUM_GARAGES = 32;
constexpr uint32_t GARAGE_MASK_ALL = 0xFFFFFFFFu;
static_assert(NUM_GARAGES == 32, "the mask is exactly one dword wide");

static_assert(sizeof(PacketHeader)    == 5,  "header layout");

// The two geometry primitives every body is built out of. Nothing had pinned
// them, which meant every size below rested on an assumption rather than on a
// check - and the ten-branch merge at version 18 is exactly the kind of event
// that would have moved one of them quietly.
static_assert(sizeof(Vec3)            == 12, "three floats, no padding");
static_assert(sizeof(Quat)            == 16, "four floats, no padding");

static_assert(sizeof(PlayerStateBody) == 71, "player state layout");
static_assert(sizeof(C_PlayerState)   == 76, "player snapshot layout");
static_assert(sizeof(S_PlayerState)   == 77, "player snapshot layout");
static_assert(offsetof(PlayerStateBody, animGroup) == 30, "animation block");
static_assert(offsetof(PlayerStateBody, animId)   == 31, "animation block");
static_assert(offsetof(PlayerStateBody, animId2)  == 41, "animation block");
static_assert(offsetof(PlayerStateBody, weapon)   == 55, "weapon then its ammo");
static_assert(offsetof(PlayerStateBody, ammoClip)  == 56, "ammo follows the weapon");
static_assert(offsetof(PlayerStateBody, ammoTotal) == 58, "ammo follows the weapon");
static_assert(offsetof(PlayerStateBody, aimYaw)   == 62, "aim block");
static_assert(offsetof(PlayerStateBody, flags)    == 70, "flags is last");

// 1 weapon + 1 flags + 2 clip + 4 total.
static_assert(sizeof(AmmoSlotBody)    == 8,  "ammo slot layout");
static_assert(sizeof(C_PlayerAmmo)    == 13, "player ammo layout");
static_assert(sizeof(S_PlayerAmmo)    == 14, "player ammo layout");
static_assert(sizeof(VehicleStateBody)== 72, "vehicle state layout");
static_assert(sizeof(C_VehicleState)  == 77, "vehicle snapshot layout");
static_assert(sizeof(S_VehicleState)  == 78, "vehicle snapshot layout");
static_assert(sizeof(C_Hello)         == 33, "hello layout");
static_assert(sizeof(S_Welcome)       == 17, "welcome layout");
static_assert(sizeof(C_PedBodyPart)   == 9,  "body part layout");
static_assert(sizeof(S_PedBodyPart)   == 9,  "body part layout");
static_assert(sizeof(PedBodyPartBody) == 4,  "body part layout");

// The small packets nothing had pinned. Each is header plus its own fields
// with no padding, which is only true because of the #pragma pack above - and
// a pack that stopped covering one of them would show up here rather than as
// a session that reads a netId out of the wrong two bytes.
static_assert(sizeof(S_PlayerLeave)    == 7,   "leave layout");
static_assert(sizeof(C_ExitVehicle)    == 7,   "exit layout");
static_assert(sizeof(S_ExitVehicle)    == 8,   "exit layout");
static_assert(sizeof(S_VehicleDespawn) == 7,   "vehicle despawn layout");
static_assert(sizeof(C_Chat)           == 133, "chat layout");
static_assert(sizeof(S_Chat)           == 134, "chat layout");

// 5 hdr + 2 netId + 2 animId = 9, and no padding anywhere in it: both
// members are 2-byte and the header ends on an odd byte, which is exactly
// the shape C_PedBodyPart already has and is the reason the bodies of both
// are laid out this way rather than starting with the byte fields.
static_assert(sizeof(PedDeathBody)    == 4,  "ped death layout");
static_assert(sizeof(C_PedDeath)      == 9,  "ped death layout");
static_assert(sizeof(S_PedDeath)      == 9,  "ped death layout");
static_assert(offsetof(PedDeathBody, animId) == 2, "netId first");

// 5 hdr + 1 id + 2 net + 24 nick + 2 model + 12 pos + 4 heading = 50 identity,
// then 4 health + 4 armour + 1 weapon + 1 flags + 2 deathAnim = 12 condition.
static_assert(sizeof(S_PlayerJoin)    == 62, "player join layout");
static_assert(offsetof(S_PlayerJoin, health) == 50, "condition follows identity");
static_assert(offsetof(S_PlayerJoin, flags)  == 59, "join flags");

// 2 model + 1 type + 1 pad + 12 pos + 4 heading = 20.
static_assert(sizeof(AmbientPedBody)  == 20, "ambient ped layout");
static_assert(offsetof(AmbientPedBody, pos) == 4, "pos stays 4-aligned");
static_assert(sizeof(C_PedSpawn)      == 29, "ambient ped spawn layout");
static_assert(sizeof(S_PedSpawn)      == 32, "ambient ped spawn layout");
static_assert(sizeof(C_PedDespawn)    == 7,  "ambient ped despawn layout");
static_assert(sizeof(S_PedDespawn)    == 7,  "ambient ped despawn layout");
static_assert(sizeof(AmbientPedState) == 24, "ambient ped state layout");
static_assert(offsetof(AmbientPedState, pos) == 8, "pos stays 4-aligned");
static_assert(sizeof(C_PedStates)     == 9 + 24 * MAX_PED_STATES,
              "ambient ped state batch layout");
static_assert(sizeof(S_PedStates)     == 9 + 24 * MAX_PED_STATES,
              "ambient ped state batch layout");

// 2 model + 2 colours + 2 extras + 2 pad + 12 pos + 16 rot = 36.
static_assert(sizeof(AmbientCarBody)  == 36, "ambient car layout");
static_assert(offsetof(AmbientCarBody, pos) == 8, "pos stays 4-aligned");
static_assert(sizeof(C_CarSpawn)      == 45, "ambient car spawn layout");
static_assert(sizeof(S_CarSpawn)      == 48, "ambient car spawn layout");
static_assert(sizeof(C_CarDespawn)    == 7,  "ambient car despawn layout");
static_assert(sizeof(S_CarDespawn)    == 7,  "ambient car despawn layout");

// 2 netId + 2 pad + 12 pos + 16 rot + 12 velocity = 44.
static_assert(sizeof(AmbientCarState) == 44, "ambient car state layout");
static_assert(offsetof(AmbientCarState, pos) == 4, "pos stays 4-aligned");
static_assert(sizeof(C_CarStates)     == 9 + 44 * MAX_CAR_STATES,
              "ambient car state batch layout");
static_assert(sizeof(S_CarStates)     == 9 + 44 * MAX_CAR_STATES,
              "ambient car state batch layout");

static_assert(sizeof(WorldStateBody)  == 4,  "world state layout");
static_assert(sizeof(C_WorldState)    == 9,  "world state layout");
static_assert(sizeof(S_WorldState)    == 10, "world state layout");

// 2 netId + 1 seat + 1 jack + 2 model + 1 + 1 colour + 1 pad + 12 pos + 16 rot
// 2 netId + 1 seat + 1 jack + 2 modelId + 2 colours + 2 extras + 12 pos
// + 16 rot. The two extras took the place of one `pad` byte, so this grew by
// one rather than by two.
static_assert(sizeof(EnterVehicleBody) == 38, "enter-vehicle layout");
static_assert(sizeof(C_EnterVehicle)  == 43, "enter-vehicle layout");
static_assert(sizeof(S_EnterVehicle)  == 44, "enter-vehicle layout");

// 5 hdr + 2 net + 2 model + 12 pos + 16 rot + 2 colour + 2 extras = 41
// identity, then 4 health + 1 flags = 5 condition.
static_assert(sizeof(S_VehicleSpawn)  == 46, "vehicle spawn layout");
static_assert(offsetof(S_VehicleSpawn, health) == 41, "condition follows identity");

// 2 netId + 12 pos + 16 rot
static_assert(sizeof(VehicleBlowUpBody) == 30, "vehicle blow-up layout");
static_assert(sizeof(C_VehicleBlowUp)   == 35, "vehicle blow-up layout");
static_assert(sizeof(S_VehicleBlowUp)   == 36, "vehicle blow-up layout");
static_assert(sizeof(UnownedVehicleKey) == 4,  "unowned car key layout");
static_assert(offsetof(UnownedVehicleKey, id) == 2, "id stays 2-aligned");
static_assert(sizeof(BlastTransform)    == 28, "blast transform layout");
static_assert(sizeof(C_UnownedBlowUp)   == 37, "unowned car blow-up layout");
static_assert(sizeof(S_UnownedBlowUp)   == 41, "unowned car blow-up layout");

// 2 netId + 4 panels + 2 doors
static_assert(sizeof(VehicleDamageBody) == 8,  "vehicle damage layout");
static_assert(offsetof(VehicleDamageBody, doors) == 6, "doors follow panels");
static_assert(sizeof(C_VehicleDamage)   == 13, "vehicle damage layout");
static_assert(sizeof(S_VehicleDamage)   == 17, "vehicle damage layout");
static_assert(sizeof(C_PlayerModel)   == 7,  "player model layout");
static_assert(sizeof(S_PlayerModel)   == 8,  "player model layout");

// 1 weapon + 12 origin + 12 dir + 4 speed
static_assert(sizeof(ShotBody)        == 29, "shot layout");
static_assert(sizeof(C_Shot)          == 34, "shot layout");
static_assert(sizeof(S_Shot)          == 35, "shot layout");
static_assert(offsetof(ShotBody, speed) == 25, "speed follows dir");
static_assert(sizeof(ExplosionBody)   == 13, "explosion layout");
static_assert(sizeof(C_Explosion)     == 18, "explosion layout");
static_assert(sizeof(S_Explosion)     == 19, "explosion layout");

// 2 victim + 1 weapon + 4 amount + 1 piece + 1 direction
static_assert(sizeof(DamageBody)      == 9,  "damage layout");
static_assert(sizeof(C_Damage)        == 14, "damage layout");
static_assert(sizeof(S_Damage)        == 15, "damage layout");
static_assert(offsetof(DamageBody, piece) == 7, "piece and direction are last");
static_assert(sizeof(C_Death)         == 9,  "death layout");
static_assert(sizeof(S_Death)         == 10, "death layout");
static_assert(sizeof(RespawnBody)     == 16, "respawn layout");
static_assert(sizeof(C_Respawn)       == 21, "respawn layout");
static_assert(sizeof(S_Respawn)       == 22, "respawn layout");

// 12 pos + 2 model + 1 type + 1 pad
static_assert(sizeof(PickupIdent)       == 16, "pickup ident layout");
static_assert(sizeof(C_PickupClaim)     == 21, "pickup claim layout");
static_assert(sizeof(S_PickupTaken)     == 22, "pickup taken layout");
static_assert(sizeof(S_PickupDenied)    == 21, "pickup denied layout");
static_assert(sizeof(C_PickupRelease)   == 21, "pickup release layout");
static_assert(sizeof(S_PickupGrant)     == 21, "pickup grant layout");
static_assert(sizeof(C_PickupCollected) == 21, "pickup collected layout");
static_assert(sizeof(PickupDropBody)    == 20, "pickup drop body layout");
static_assert(sizeof(C_PickupDrop)      == 25, "pickup drop layout");
static_assert(sizeof(S_PickupDrop)      == 26, "pickup drop relay layout");

// 5 hdr + 4 mask, and 5 + 1 + 3 pad + 4 for the relay.
static_assert(sizeof(GarageMaskBody)    == 4,  "the whole city is one dword");
static_assert(sizeof(C_GarageState)     == 9,  "garage mask layout");
static_assert(sizeof(S_GarageState)     == 13, "garage mask relay layout");
static_assert(offsetof(S_GarageState, body) == 9,
              "the mask stays 4-aligned behind the pad");
// 2 net + 1 garage + 2 colours + 3 pad = 8, so 13 and 17.
static_assert(sizeof(ResprayBody)       == 8,  "respray layout");
static_assert(sizeof(C_Respray)         == 13, "respray layout");
static_assert(sizeof(S_Respray)         == 17, "respray relay layout");
static_assert(offsetof(PickupDropBody, quantity) == 16,
              "the quantity follows the ident, which is 16 bytes");
static_assert(offsetof(PickupIdent, modelIndex) == 12, "model follows pos");

// 12 pos + 2 model + 2 pad = 16, same shape as a pickup ident and for the
// same reason: a position and a model index, and nothing that is a handle.
static_assert(sizeof(ObjectIdent)      == 16, "object ident layout");
static_assert(offsetof(ObjectIdent, modelIndex) == 12,
              "object model follows pos");
// 16 ident + 4 amount + 1 state + 3 pad = 24.
static_assert(sizeof(ObjectBreakBody)  == 24, "object break body layout");
static_assert(offsetof(ObjectBreakBody, amount) == 16,
              "the amount the engine was handed follows the ident");
static_assert(offsetof(ObjectBreakBody, state)  == 20, "state follows amount");
static_assert(sizeof(C_ObjectBroken)   == 29, "object broken layout");
static_assert(sizeof(S_ObjectBroken)   == 30, "object broken relay layout");
static_assert(offsetof(S_PickupTaken, ident)    == 6,  "playerId comes first");

// How long a pickup of this type stays gone, in milliseconds, or 0 for "never
// comes back". Shared between the client and the server on purpose: the
// server owns availability and the client's engine owns appearance, and they
// have to be working off the same number (docs/pickups.md 5).
//
// Read off the retail award switch's tails, not off re3:
//   IN_SHOP         0x00430FBE  add eax,1388h    =   5 000
//   ON_STREET       0x0043111C  add eax,7530h    =  30 000
//   ON_STREET_SLOW  0x0043113A  add eax,493E0h   = 300 000  (bribe model)
//                   0x00431146  add eax,0AFC80h  = 720 000  (anything else)
// and ONCE / ONCE_TIMEOUT / COLLECTABLE1 / MONEY all end in the engine's
// inline Remove(), which frees the slot for good.
//
// The engine writes these as `CTimer::m_snTimeInMilliseconds + k`, i.e. an
// absolute stamp on a *local* clock that starts when that machine's game
// started and stops while it is paused. The constant travels; the deadline
// must not.
//
// The type numbers are spelled here rather than included from the client's
// addresses.h, which is the wrong direction for a shared header - addresses.h
// static_asserts its own copy against this one.
constexpr uint32_t PickupRespawnMs(uint8_t type, bool isBribeModel) {
	switch (type) {
	case 1:  return 5000;                              // PICKUP_IN_SHOP
	case 2:  return 30000;                             // PICKUP_ON_STREET
	case 15: return isBribeModel ? 300000u : 720000u;  // PICKUP_ON_STREET_SLOW
	default: return 0;
	}
}

} // namespace coopiii
