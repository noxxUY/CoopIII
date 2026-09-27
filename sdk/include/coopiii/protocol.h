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
// 18: ten changes at once, and one number for all of them. Each was built
//    against either 16 or 17 and none took a number of its own, so this
//    one entry covers them all. There is no
//    17.5 and no per-change number: a client and a server that disagree about
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
//
// 20: shooting somebody else's pedestrian. One opcode pair, C_PedDamage /
//    S_PedDamage (0x68/0x69), and no existing layout moves.
//
//    The gap was a missing direction, not a broken one. Version 17 carries a
//    limb and 18 carries a death, and both travel from the machine that
//    *hosts* a pedestrian out to the observers. Nothing travelled the other
//    way, so a player could empty a clip into a replica of somebody else's
//    ped and the machine that owns it never heard: every replica is bullet-,
//    fire-, melee- and explosion-proof on purpose, so the shooter's own
//    engine refused the hit and there was nothing left to refuse or forward.
//    No reaction, no blood, no death.
//
//    It is the player-damage exchange (§1.10) pointed at a pedestrian, and
//    the same split of authority: the shooter reports the hit it landed, the
//    machine that owns the ped feeds it into its own CPed::InflictDamage. So
//    the ped flinches, bleeds, staggers, loses a limb and dies exactly where
//    single player puts all of that - and the limb and the death then travel
//    back out on 17's and 18's own packets, to everybody including the
//    shooter. Nothing about a replica's health is ever decided by the machine
//    holding it.
//
//    Point to point, like S_Damage: only the ped's owner has anything to do
//    with it. Friendly fire does NOT gate it - a pedestrian is not a player,
//    and a session with friendly fire off still lets everyone shoot NPCs.
//
// 21: a passenger seat is announced when the entry starts, not when it
//    finishes. No opcode, no struct, no field - what moved is when
//    C_EnterVehicle goes out and what a passenger's copy of it promises.
//
//    Getting in became an animation and the announcement stayed at the end
//    of it, so the other machines were told about an entry that was already
//    over. Their own replica entry - the one thing that ever opens that car's
//    door on their screen, because a door is swung frame by frame by the
//    entering ped's own animation and nothing about an open door travels -
//    started a second late against a ped the pose stream had already carried
//    into the seat, or was skipped. The driver watched a passenger appear
//    beside them with the door shut.
//
//    So C_EnterVehicle with a passenger seat now means "I am getting into
//    this seat", sent as the walk begins. Two things follow from that and
//    both ride packets that already exist:
//
//    - the seat can be corrected. The slot has to be chosen before the walk
//      (the engine's entry animates to a *door*), and another ped can take
//      it in the second that walk lasts, so a second C_EnterVehicle for the
//      same car with the real seat follows when the two differ. The server's
//      NoteEnterVehicle already overwrites rather than accumulates.
//    - the entry can fail. The ordinary C_ExitVehicle the client sends when
//      a passenger's seat reads empty is the retraction, and no observer
//      needs a new rule for it: the seat request it was given simply stops
//      standing, which is the state its own abandon path already handles.
//
//    An 18/19 observer reads all three packets exactly as before. What it
//    would get wrong is only the thing it was already getting wrong - it
//    would sit the passenger down at the announcement instead of walking
//    them to the door - so this is a change of meaning rather than of
//    format, and it is written down here for the same reason 18's two
//    meaning-only entries were.
//
// 22: a car changes hands when somebody pulls the driver out of it. No
//    opcode and no struct: the handover is told with the S_ExitVehicle the
//    protocol has always had, addressed to the player who lost the seat.
//
//    A jack happens entirely inside the jacker's process - his engine plays
//    the animation, drags the replica out and puts his own player at the
//    wheel - and the victim's engine is never told. Both machines then read
//    their own CVehicle::m_pDriver, both answer "we drive it", and neither
//    is wrong from where it stands. Every ownership guard in the vehicle
//    seam asks that one question, so all of them gave a stale answer on one
//    of the two.
//
//    The server is the only thing that can break that tie, so it does: the
//    latest claim on seat 0 wins, the loser is recorded out of the car, and
//    the S_ExitVehicle naming him goes out BEFORE the S_EnterVehicle naming
//    the winner, on the same reliable ordered channel. No client ever holds
//    two owners for one car, not even for a single packet.
//
//    The version moves although no byte moved, because both halves changed
//    behaviour: a server that does not arbitrate leaves two owners, and a
//    client that does not stop re-claiming takes the car straight back and
//    the two machines trade it at the claim rate.
//
// 23: shooting a car somebody else is driving. One opcode pair, C_VehicleHit
//    / S_VehicleHit (0x6A/0x6B), and no existing layout moves.
//
//    Version 20 gave a pedestrian the direction the crowd never had. A car
//    still does not have it, and the hole is shaped differently enough to be
//    worth stating rather than assuming: a replica pedestrian is bullet-,
//    fire-, melee- and explosion-proof, so a shot at one was refused and
//    nothing happened. A replica car is proof against collisions and nothing
//    else, so a shot at one was *accepted* - by the machine with no right to
//    decide it. The health came off a copy nobody else could see, the shooter
//    watched a car smoke and burn on a number its owner never had, and the
//    owner drove on in a car that was never touched. Divergence, not silence,
//    and it is the worse of the two because both screens look correct.
//
//    Same split of authority as 20 and as §1.10: the shooter reports the hit
//    it landed, the machine that owns the car feeds it into its own
//    CVehicle::InflictDamage, and the car dents, smokes, catches fire and
//    blows up on its owner's schedule. Nothing new is needed to carry the
//    result back, and that is the point - the health rides the driver's 25 Hz
//    snapshot, the dents ride C_VehicleDamage, and the wreck rides
//    C_VehicleBlowUp at the transform the owner's own physics chose.
//
//    Three fields rather than five, because CVehicle::InflictDamage takes
//    three arguments where CPed::InflictDamage takes five. No health: writing
//    health destroys nothing and arms a five-second timer under somebody
//    else's car. No position and no shot vector: the shooter's engine already
//    resolved the ray and the conclusion is what travels.
//
//    Point to point, like S_PedDamage: only the car's driver has anything to
//    do with it. Friendly fire does NOT gate it - a car is not a player. Only
//    a car the session records a live driver for; an unowned one has no
//    machine entitled to decide its condition and roadmap.md §5.8 already
//    carries what diverges about it.
//
//    The observer also stops damaging its own copy, which is half the fix and
//    not a side effect. Without it the two halves both run: the owner takes
//    the reported hit and the observer takes its own, so one trigger pull
//    costs the session twice, on two machines, at two different healths.
//
// 24: a car nobody is driving, and a traffic car somebody got into. Three
//    opcodes in the 0x58 block - C_VehicleSettled, S_VehicleCustody and
//    S_CarPromoted - and no existing layout moves.
//
//    22 settled who owns a car with somebody in it and left the gap between
//    an exit and the next enter owned by nobody, which means every machine
//    pins the car at the last transform the session gave it. That is exactly
//    right for a car parked on the street and exactly wrong for one that was
//    still moving when its driver got out of it: the pin is applied after
//    physics, every frame, so a car reared up against a wall stays reared up
//    for the rest of the session. CVehicle::CanPedEnterCar (0x005522F0)
//    refuses a car whose up.z is *inside* +-0.1 - on its side, not upright -
//    and CPed::SeekCar (0x004D3F90) answers that refusal by walking the ped
//    back to the door with no timeout in it. A car that goes on its side
//    while nobody is recorded driving it is a car nobody can ever get into
//    again.
//
//    So a driverless car gets a custodian: one machine, named by the server
//    on the S_ExitVehicle's own reliable ordered channel and immediately
//    after it, that stops correcting the car and lets its own engine finish
//    what the car was doing. It streams the result on the C_VehicleState
//    that already exists, every observer follows it down on the interpolation
//    that already exists, and when the car comes to rest the custodian says
//    so and the session goes back to nobody simulating it - the pinned,
//    silent, perfectly still behaviour a parked car has always had, unchanged
//    line for line. Custody is the exception, rest is the rule.
//
//    Granted to the player who was driving rather than to the session host,
//    although roadmap §5.8 is right that an ownerless world entity is the
//    host's. The host is the right authority for a *fact* about a car nobody
//    owns; it is the wrong machine to run its physics, because GTA III
//    streams around one player and holds one island's collision, so a host on
//    the other side of the river would be simulating a car with no ground
//    under it and reporting the fall. The ex-driver was touching the car a
//    frame ago, which is the whole argument. Custody is therefore short:
//    VEHICLE_SETTLE_MS, on the custodian's own clock, and the car is handed
//    back whether or not it settled.
//
//    Session::MayReportVehicle is the whole of the server's half - it now
//    takes a snapshot from the driver, or from the custodian when there is no
//    driver, and from nobody else. A custodian is cleared by a new driver, by
//    the car's destruction, by the custodian disconnecting, and by the
//    custodian's own C_VehicleSettled. No client ever decides it is the
//    custodian, which is what stops two machines both being it, and it is the
//    same thing that stops two machines both being the driver.
//
//    The second half is traffic. An ambient car has an owner and no seats
//    (population.md §1.1), so a player taking the wheel of one was invisible
//    to the roster that holds it: the original host went on steering it and
//    every observer went on drawing the driver's ped in the road beside it.
//    A claim now names the netId the session already has, C_EnterVehicle
//    exactly as it is, and the server promotes the AmbientCar row into a
//    Vehicle row **under the same netId**. S_CarPromoted tells every machine
//    to move its bookkeeping across; no car is created or destroyed anywhere,
//    including on the machine whose own engine made it, which keeps its
//    CVehicle and merely stops being allowed to report it.
//
//    The version moves although nothing existing moved, for the reason 22's
//    did: both halves changed behaviour. A server that does not arbitrate
//    custody leaves a wedged car wedged, and a client that ignores
//    S_CarPromoted goes on hosting traffic the session has taken off it -
//    which is two machines reporting one car, the exact state 22 exists to
//    make impossible.
//
// 25: a rampage is one objective for the whole
//    session. Six opcodes out of the 0x88-0x8F block, C_RampageStart /
//    S_RampageOpen / C_RampageKill / S_RampageKill / C_RampageEnd /
//    S_RampageEnd, two bits of SessionFlags, and no existing layout moves.
//
//    roadmap.md §5.10 decided this and found it free: every machine runs
//    rampage.sc, the pickup work pushes a remote collection into every
//    machine's own aPickUpsCollected, and so every machine's own script calls
//    CDarkel::StartFrenzy with the same weapon, the same time limit, the same
//    target and the same four model ids, in the same frame. That half is real
//    and it survived the binary. What it does not do is *count*.
//
//    CDarkel::KillsNeeded (0x008F1AB8) is decremented in exactly two places,
//    CDarkel::RegisterKillByPlayer (0x00420F60) and
//    RegisterCarBlownUpByPlayer (0x00421070), and CPed::InflictDamage only
//    reaches the first of those when the damaging entity is FindPlayerPed()
//    or FindPlayerVehicle() - the test at 0x004EAD1A. In a session the ped is
//    hosted by one machine and shot by another, so on the host the damager is
//    a replica and the kill goes to RegisterKillNotByPlayer, which bumps a
//    statistic and nothing else; and the shooter's own engine returned before
//    that line, because its hit became a packet. **A co-op NPC kill counts
//    for nobody.** Four counters then run apart from the same start, each
//    machine ends its own rampage on its own arithmetic, and rampage.sc
//    hands out the reward on one machine while printing RAMPAGE FAILED on
//    another.
//
//    So what goes on the wire is kills, and only kills:
//
//    - C_RampageStart (0x88) is every machine saying "my script started a
//      frenzy, limit T, target K". The server keeps the first and ignores the
//      rest; they are the same numbers from the same script.
//    - S_RampageOpen (0x89) names the frenzy and gives back the target the
//      session is actually playing for, which is K under the default rule and
//      K scaled by the player count under `scaled`.
//    - C_RampageKill (0x8A) is one qualifying kill, as the three arguments
//      the engine's own register takes: the victim's model index, the weapon
//      and whether it was a headshot. Not a netId - an observer 200 m away
//      has no replica of that pedestrian and must still be able to judge it.
//    - S_RampageKill (0x8B) relays it to everybody but the reporter, whose
//      own engine has already counted it.
//    - C_RampageEnd (0x8C) is a machine reporting the ending its own CDarkel
//      reached, and S_RampageEnd (0x8D) is the session's verdict. First
//      report wins, exactly as the first pickup claim wins.
//
//    The engine is left running on every machine: its own HUD, its own
//    countdown, its own tick, its own weapon restore. The one thing held back
//    is what the *script* sees, through a detour on CDarkel::ReadStatus
//    (0x00420E50) - the only function the script's 01FA opcode reads the
//    status through, and the only caller it has in the whole image. Until the
//    session has a verdict the script is told ONGOING, so `rampage.sc` leaves
//    its wait loop everywhere on the same value.
//
//    An old client against a new server is the same session it was: it never
//    sends a kill, never hears one, and counts only its own player's - which
//    is exactly today's behaviour. The number moves anyway because a new
//    client waits on S_RampageEnd that an old server will never send, and
//    would sit in the script's wait loop for the rest of the session.
//
// Not a version: a replica that dies on its own. No opcode, no
//    struct, no byte, and the version does not move - because nothing about
//    this reaches the wire, and that is the finding rather than a shortcut.
//
//    docs/population.md §5.6 left one case open. An observer's copy of
//    somebody else's pedestrian could end up dead while the host's original
//    was alive and walking: ApplyAmbientPedState refuses to drive anything
//    into a corpse, so the replica stopped moving and the two machines
//    disagreed about that person for the rest of the session. 20 closed the
//    common cause - the host killing him now travels - and 23 closed the
//    shooter's half, but the local engine could still get there on its own.
//
//    The instinct is to report it, and it is wrong. Every other entry in this
//    list carries a fact from the machine entitled to know it to the machines
//    that are not. This is the opposite shape: the observer knows nothing.
//    The host's pedestrian is fine. What has happened is that an observer's
//    engine made a decision about an entity it does not own, and a packet
//    announcing it would ask the server to arbitrate between a machine that
//    is right and a machine that is wrong about the same pedestrian. There is
//    nothing to arbitrate. The opcode block reserved for this is given back
//    unused.
//
//    So the fix is local on both halves, and it is in two places because the
//    engine reaches a dead ped through two doors:
//
//    - CPed::SetDie (0x004D37D0) is refused for a replica unless CoopIII's
//      own KillAmbientReplica is the caller. That is one test on the object,
//      not on the damage cause, and the difference is load-bearing: the proof
//      flags SpawnAmbientReplica sets are dispatched inside a switch in
//      CPed::InflictDamage and six of its causes read no flag at all
//      (game/population.h, PedProofForDamageCause, transcribed from the jump
//      table at 0x005F9EB8). Flags were a backstop and were never a
//      mechanism - the same thing 23 found for a car.
//    - a replica that got there anyway is rebuilt. CAutomobile::BlowUpCar
//      kills a seated occupant through CPed::SetDead (0x004D3970), a
//      different address that no guard on SetDie can see, so
//      AmbientReplicaIsAlive now also treats "dead and the session never said
//      so" as gone, and re-arms the spawn exactly as it already does for a
//      replica the engine reaped. Testing the state rather than the route is
//      what makes that complete, including for routes nobody has found yet.
//
//    An older client talking to a newer one is unaffected in both directions,
//    which is the whole reason this takes no number: there is no packet to
//    misread. What changes is that one machine stops being wrong on its own.
//
// 26: getting into a car is announced when the entry starts, the way a
//    passenger seat already is, and it says which door. One opcode pair,
//    C_EnteringVehicle / S_EnteringVehicle (0x60/0x61), and no existing
//    layout moves.
//
//    Version 21 did this for the passenger seat and stopped there, and this
//    file said the driver's entry "has no equivalent and cannot have one"
//    because its claim is what introduces the car. That was true of the
//    claim and not of the entry. The two are separable and are now separate:
//    the claim still goes out at the end, still names the car, still moves
//    ownership, and §2.8.3 is untouched. What goes out at the start is a
//    statement of intent that decides nothing - the server relays it and
//    writes nothing down, and a receiver that acts on it must be able to
//    take it all back, because an entry can be abandoned and only the claim
//    ever confirms one.
//
//    The second byte is the one that was actually missing. A driver's entry
//    does NOT walk round the car: CPed::SeekCar sends it through
//    CPed::GetNearestDoor, so pressing the enter key on the passenger side
//    opens the near door, gets in through it and shuffles across inside.
//    Told only "seat 0", an observer opened the driver's door and
//    CPed::EnterCar's line-up dragged the replica round to it - the teleport
//    the player reported. Told the door, the observer's own engine plays the
//    near door, the get-in and the shuffle by itself. So the walk does not
//    travel and nothing about it needs to: what travels is which door and
//    when, four bytes, once per entry.
//
//    An observer that does not understand 0x61 behaves exactly as it does
//    today - late, and through the wrong door - because the claim it already
//    handles is unchanged. A server that does not relay it is the same.
//
// 27: a lamp post lies down on every screen. One
//    opcode pair, C_ObjectSettled / S_ObjectSettled (0xC2/0xC3), one new bit
//    in a byte that was already there, and no existing layout moves.
//
//    Version 18 made breaking shared and said in the same breath what it did
//    not do: "a lamp post shows its damaged model on both screens and may
//    still be standing on one of them". That is not a rounding error in the
//    break, it is a second mechanism that was never carried. **Breaking and
//    uprooting are different things in this engine and CObject::ObjectDamage
//    is what proves it** - none of its nine arms clears bIsStatic, and the
//    smash arm sets it. What makes an object fall over is bIsStatic being
//    *cleared* and the object being handed to CPhysical::AddToMovingList,
//    which happens in three places that all read m_fUprootLimit: a collision
//    (impulse > limit), a blast (power > limit), and a bullet, a pellet or a
//    bat (limit <= 0). A lamp post's limit is 400 and the break threshold is
//    150, so the same car that bends it at 200 also knocks it down at 500 and
//    the two decisions genuinely are separate.
//
//    Two of the three were already agreed and the third was not, which is the
//    same shape 18's own two measurements had. A blast uproots identically
//    everywhere because its power is a pure function of two positions and a
//    radius, exactly as its damage is. A bullet uproots identically
//    everywhere because 1.9.2 replays the shot through the engine's own
//    CWeapon::Fire on the remote ped, out of the wire's muzzle and along the
//    wire's direction, so every observer's own CWeapon::DoBulletImpact runs
//    the same object arm against the same map object. What nobody else ran
//    was somebody else's *collision* - and that is the case the report was
//    about.
//
//    So what travels is the resting place, and only the resting place. Not a
//    stream and not an impulse: roadmap.md 2.4 says the timestep is
//    frame-time-derived, so handing two machines the same impulse does not
//    put the post down in the same place, and a machine that never uprooted
//    it at all has nothing to integrate anyway. One packet per uprooting,
//    sent when the engine's own sleep test (CPhysical::ProcessControl,
//    m_nStaticFrames > 10, then SetIsStatic(true)) says the object has
//    stopped, carrying the 3x3 and the position. The receiver writes the
//    matrix, calls the engine's own CMatrix::UpdateRW, CEntity::UpdateRwFrame
//    and CPhysical::RemoveAndAdd - the last of which is what re-files it in
//    the sector grid, without which it would be lying in a sector nobody
//    renders - and sets bIsStatic, which is enough: CWorld::Process unlinks a
//    static entity from the moving list on its own next pass, so nothing here
//    ever writes into that list.
//
//    OBJ_BREAK_UPROOTED is the new bit, and it is free - the state byte had
//    six spare. It is not the fix by itself; it is what lets an observer drop
//    the post the moment the break arrives instead of watching it stand for a
//    second and then teleport flat. A receiver that has it clears bIsStatic
//    and links the object exactly once, guarded on bIsStatic being set,
//    because the engine's invariant is that the moving list holds exactly the
//    non-static entities and adding a second node for one of them is the bug
//    client/src/game/movinglist.h exists to clean up after.
//
//    An explosion stays quiet here too, for the reason it stays quiet about
//    the break: one rocket into a row of bins would be one reliable packet
//    per bin, and every machine already uprooted all of them from the same
//    number. They end up lying in slightly different places, which is the one
//    difference this deliberately leaves standing.
//
//    A 26 client reading this is harmless in both directions: the two opcodes
//    are in a block it has never used, and the new bit is one it masks off
//    when it decides how many times to replay ObjectDamage. What it would get
//    wrong is only what it already gets wrong - the post stays standing.
//
// 28: shooting somebody else's traffic. It is a wire change:
//
//    - C_CarHit / S_CarHit (0x6C/0x6D), VehicleHitBody again. A hit the local
//      player landed on a replica of a traffic car goes to the machine hosting
//      it, which applies it through its own CVehicle::InflictDamage.
//      docs/protocol.md §1.23.
//    - AmbientCarState's two pad bytes are now `health`. No layout moved, but
//      an older sender writes 0 there, which a newer receiver reads as
//      "unsaid" rather than as a number.
//
//    A replica used to take hits, fire and blasts off its own copy and, at
//    zero, run BlowUpCar on a car whose host never touched it - killing its
//    driver replica through CPed::SetDead on the way (0x0053BDCD, 0x0053BE2F).
//    Now it refuses both, holds the host's health every frame, and only
//    blows up when the host's C_UnownedBlowUp says so. It needs a number
//    because a client that refuses locally, connected to a server that
//    doesn't relay C_CarHit, gets traffic nobody can hurt.
//
// 29: vehicle rampages
//    count every player's cars. One opcode pair, C_RampageCar / S_RampageCar
//    (0x8E/0x8F, the last two of the rampage block), and no existing layout
//    moves.
//
//    25 shared pedestrian kills and left cars out. The car register,
//    CDarkel::RegisterCarBlownUpByPlayer (0x00421070), is not the same shape
//    as the kill register where it matters: CAutomobile::BlowUpCar calls it
//    at 0x0053BF04 with no culprit test anywhere in the function. So every
//    machine that holds a copy of a car counts its wreck when it replays it,
//    and a machine that doesn't hold one never hears of it. Copying 25's
//    report-and-relay onto that would count the car twice on every screen
//    that had it.
//
//    So the machine that decided the wreck reports it, and only that one:
//    the driver for his own car, the host for its traffic, and whoever's
//    engine got there for a parked car or a session car nobody drives. Every
//    replay CoopIII runs (BlowUpRemoteVehicle, BlowUpCarAsOwnerSaid,
//    WreckUnownedVehicle) goes through the engine's register with the
//    rampage branch kept out, and the relay is what counts it. The two kinds
//    more than one machine can decide carry their UnownedVehicleKey and are
//    counted once per key per frenzy, on the server and on every client.
//    The same replay rule now also keeps the occupants of a replayed wreck
//    off a pedestrian rampage, which 25 counted once per machine that had
//    the car.
//
//    It needs a number because the two halves change behaviour together. A
//    new client keeps replays off its own counter and waits for the relay; a
//    server that doesn't relay 0x8E leaves that client counting fewer cars
//    than it does today.
//
// 30: a car somebody is settling is theirs to damage. No opcode, no layout
//    moves.
//
//    24 made the custodian the one machine simulating a driverless car for up
//    to VEHICLE_SETTLE_MS, and 23 made "the machine simulating it decides its
//    condition" the rule for a driven car. The two never met: the detours on
//    every other client treated a car in custody as nobody's. A shot took
//    health off the observer's copy until the custodian's next snapshot put it
//    back, and a car that died on the observer's screen during the settle blew
//    up there and went out as UNOWNED_SESSION while the custodian's copy was
//    fine.
//
//    Now the custodian owns the car's condition for as long as it holds
//    custody, exactly as a driver does:
//
//    - Session::VehicleHitRecipient routes C_VehicleHit to the driver, or with
//      no driver to the custodian. A hit that arrives after the custody ended
//      has nobody to go to and is dropped, the same as a hit on a parked car.
//    - Session::NoteUnownedBlowUp takes UNOWNED_SESSION for a car in custody
//      from the custodian only.
//    - Every other client refuses damage and BlowUpCar on a car somebody else
//      is settling and forwards its own hits as C_VehicleHit. The custodian
//      applies them through CVehicle::InflictDamage until it sends
//      C_VehicleSettled, and its wreck still goes out as UNOWNED_SESSION.
//
//    It needs a number for the reason 24's did. A new client on an old server
//    refuses its hits locally and the server drops them, so a settling car
//    can't be shot at all; an old client on a new server is sent hits for its
//    custody car and drops them, and goes on damaging other people's.
//
// 31: the car horn. No opcode, no layout moves: bit 4 of
//    VehicleStateBody::flags, VEH_HORN, which was free.
//
//    A remote player's horn was never heard. Nothing sampled
//    m_nCarHornTimer (+0x22C), so when one player honked at another only the
//    one honking heard it. Now the driver's snapshot says whether the horn is
//    sounding, held one snapshot past the end so a single lost packet cannot
//    eat a tap, and every other machine holds its replica's timer at 42 after
//    CGame::Process for as long as a snapshot less than 250 ms old says so
//    and somebody is at the wheel.
//
//    42 and not the sender's 1 is the whole trap. A replica is never
//    STATUS_PLAYER (this said ABANDONED; that is only an empty one, and a
//    seated driver makes it PHYSICS - client/src/game/carstatus.h), and the
//    audio plays any other status's horn through the rhythm table at
//    0x00606AB8, column (44 - timer). Column 43 is off in all eight rhythms,
//    so the sender's value played nothing; column 2 is on in all eight. And
//    CAutomobile::ProcessControl takes the timer back every frame, the
//    ABANDONED arm zeroing it (0x00531BAC) and the horn block counting a
//    PHYSICS car's down (0x005341B5), so a write made before the frame never
//    reaches the audio as written.
//
//    No pedestrian flees from the replayed horn, on any machine. The flee the
//    horn causes is decided in the car's own ped scan and only for a car in
//    STATUS_PLAYER. The evasions that read it run from that same scan: after
//    the zero on an empty replica, and on a seated one after the horn block
//    took 42 to 41, which they see, behind their own early returns. The
//    machine hosting the pedestrians in front of a honking player does not
//    make them flee - which single player would. That is left
//    as a gap rather than faked, because the only way into that branch is a
//    replica in STATUS_PLAYER, which also reads the local pad.
//
//    The traffic AI's honk (PlayCarHorn, 45 counted down) is the same byte
//    and does not travel: AmbientCarState has no flags byte to put it in.
//
//    It needs a number because the two halves disagree about the bit, even
//    though neither misreads it. An older receiver ignores it and stays
//    silent, which is only today's behaviour. An older sender never sets it.
//    The one thing an older build gets wrong is this: its ApplyRemoteVehicle
//    compares the whole flags byte to decide whether to rewrite the engine,
//    lights and siren, so every honk from a newer sender makes an older
//    receiver rewrite all three - harmless as values, since they have not
//    changed, but it is a behaviour the bit changes on a build that has never
//    heard of it.
//
// 32: the police helicopter. Three opcode pairs out of 0xA4-0xAB -
//    C_HeliState / S_HeliState, C_HeliGone / S_HeliGone and C_HeliHit /
//    S_HeliHit - and no existing layout moves.
//
//    CHeli::UpdateHelis (0x005499F0) builds the police helicopter from the
//    local player's own CWanted - NumOfHelisRequired (0x004ADC00) says one at
//    three or four stars, two at five or six, none while the police are told
//    to ignore him - and CHeli::ProcessControl (0x00547CC0) steers it at
//    FindPlayerCoors and sets its fire rate from the same wanted level. So a
//    helicopter can only ever chase the player of the machine that made it,
//    the same wall docs/wanted.md 2.3 found for the cops, and nothing about
//    it reached anybody else: population.md's host test refuses it twice, as
//    PERMANENT_VEHICLE and as locked. The wanted player had a helicopter and
//    nobody else could see it. Worse, a replayed shot is a real bullet on the
//    owner's machine and CHeli::TestBulletCollision (0x0054AB30) never asks
//    who fired, so a second player's gunfire could bring the owner's
//    helicopter down there and pay the owner for it.
//
//    So the machine whose engine made it owns it, and only the two police
//    slots of CHeli::pHelis (0x0072CF50) - the script helicopter and
//    Catalina's stay with the campaign work. The owner streams it on
//    C_HeliState at HELI_STATE_HZ, unreliable, and says when it is finished
//    on C_HeliGone: flew away, or shot down with where it went off. Everybody
//    else builds a real CHeli and keeps it out of pHelis. Every reader of
//    that array is in Heli.cpp (a byte scan finds nothing else), so a replica
//    is invisible to UpdateHelis, both collision tests and
//    SpecialHeliPreRender, and CoopIII's detour on ProcessControl gives it
//    the owner's transform instead of the AI that would chase this machine's
//    player.
//
//    Hits go the way 20, 23 and 28 already send them. The shooter's engine
//    decides that a bullet or a rocket hit a replica and C_HeliHit goes to
//    the owner, whose engine decides what it costs with the rule
//    TestBulletCollision and TestRocketCollision apply to their own array.
//    The owner ignores the hits its own engine would have taken from a
//    replayed shot, or every bullet would count twice.
//
//    Who shot it down travels back on S_HeliGone as creditPlayerId, and it
//    is the shooter's own machine that registers CRIME_SHOOT_HELI and bumps
//    the three statistics UpdateHelis bumps. The owner's engine is kept from
//    doing either. The $250 is not paid to anybody for a helicopter another
//    player brought down: money does not travel between machines, and the
//    owner did not earn it.
//
//    A helicopter whose owner leaves, or whose stream stops, climbs away on
//    the observer's side and is removed; one whose owner loses his stars
//    flies away on the owner's own engine and the observers follow the
//    stream until C_HeliGone says it is gone.
//
//    It needs a number for the reason 28 did. A new owner refuses the hits a
//    replayed shot lands on its own helicopter and waits for C_HeliHit
//    instead, so against a server that doesn't relay 0xA8 nobody but the
//    owner could ever bring one down.
//
// Not a version: traffic horns, and the siren. No opcode and no layout
//    moves, and no mix of builds misreads anything: every sender and every
//    server builds these packets through InitHeader, which memsets the whole
//    struct, so a build from before this reads hornMask as zero and a build
//    after it behind an older server simply hears no traffic horns.
//
//    The wire half: one byte of C_CarStates' padding and one of
//    S_CarStates' are now `hornMask`, bit i for cars[i] (CarStateHornBit).
//    The host sets it when a hosted car's horn timer is running and its own
//    audio would play it (game/horn.h, TrafficHornOnWire), the server moves
//    the bit along with its row when it drops one, and every other machine
//    runs the engine's own 44-frame countdown on its replica for as long as
//    the newest row says so and is under HORN_FRESH_MS old. Before this a
//    traffic car honking at a blocked junction was heard only by the
//    machine hosting it. AmbientCarState did not grow: it has no spare byte
//    since 28, and the batch header had them.
//
//    Why it may not need a number: InitHeader zeroes the padding, so an
//    older sender and an older server both send a zero mask, and an older
//    receiver never reads it. Every mix of builds ends up where 32 is, with
//    traffic horns staying local, and none of them reads the byte as
//    anything else. The one mix that loses something new is a new client
//    behind an old server, which relays the rows and drops the mask.
//
//    The siren half is not a wire change at all. VEH_SIREN always reached a
//    replica and lit it, but cAudioManager::ProcessVehicleSirenOrAlarm
//    returns before queueing a siren for any status-4 car (0x0056C4C7), and
//    a replica is status 4 until its driver's ped is seated (then PHYSICS,
//    game/carstatus.h; this said every replica is 4). A detour now shows the
//    audio a status-4 replica with somebody else at the wheel as PHYSICS for
//    that one call (game/siren.h). Only the receiving client changes.
//
// Not a version: the police helicopter's gunfire. One opcode pair,
//    C_HeliShot / S_HeliShot, the 0xAA / 0xAB entry 32 kept for it, and no
//    existing layout moves. An older server and an older client both drop an
//    opcode they do not know, so a mix of builds only loses the gunfire.
//
//    Entry 32 shared the helicopter and not its gun. The owner's engine
//    fires at the owner's player and the owner's engine takes the health
//    off; observers saw the owner lose health and die under a helicopter
//    that never made a sound.
//
//    The gun is CHeli::ProcessControl's (0x00547CC0), so a replica - whose
//    ProcessControl CoopIII replaces - never fires, and nothing needs to be
//    kept from firing. Once the wanted level's interval has run out it fires
//    one round every 200 ms for as long as its searchlight holds the player,
//    each one FireOneInstantHitRound(&source, &target, 20) (0x00563B00, called
//    from 0x00549569) and the helicopter's own shot sound. addresses.h, "the
//    police helicopter's gun", has the whole function.
//
//    So the owner sends every round: which helicopter by owner and serial,
//    the two points its engine passed, and hdr.sendTimeMs. One packet per
//    round rather than one per burst, because the engine has no burst: a
//    burst lasts as long as the light holds the player, which nobody knows
//    until it ends, and every round scatters its target with two rand()
//    draws the observer can't repeat. Five a second per helicopter at most,
//    33 bytes each, a quarter of what its own state stream costs.
//
//    Unreliable, on the snapshot channel. A lost round is a tracer and a
//    report nobody sees and changes nothing that lasts, and a resent one
//    would arrive after its neighbours and be drawn out of step with them.
//
//    The observer draws it without dealing damage, and not by fencing the
//    engine's function: FireOneInstantHitRound calls CPed::InflictDamage and
//    CVehicle::InflictDamage itself, and anything it hit on the observer's
//    machine - a pedestrian, a parked car, the observer's own player standing
//    beside the owner - would be hurt a second time, by a machine that
//    decided nothing. It calls the cosmetic half instead, one by one: the
//    flash, the light, a line-of-sight query, the tracer, the impact sound or
//    smoke or splash for whatever that line finds, and the shot sound from the
//    replica. A walk of that whole call graph finds none of the engine's damage
//    functions. It is drawn when the replica's playback reaches the round's
//    send time, so the flash comes out of the replica rather than out of the
//    air in front of it.
//
//    A round for a helicopter the observer has no replica of - never built,
//    already gone, abandoned - is dropped without a word.
//
//    Mixed builds lose the gunfire and nothing else. A server from before
//    this drops 0xAA like any opcode it doesn't know, so nobody sees the
//    rounds; a client from before this is sent 0xAB and ignores it the same
//    way. No layout moves, so nothing is misread. The case for a number is
//    only that those sessions quietly have a silent helicopter again.
//
// Not a version: the flamethrower reaches pedestrians and cars
//    another machine owns. No opcode, no layout. What moved is the meaning
//    of one cause on three packets that already exist: C_PedDamage,
//    C_VehicleHit and C_CarHit with weapon 9 (WEAPONTYPE_FLAMETHROWER) mean
//    "our flame reached this, light it", and the amount is 0 and unread.
//    docs/protocol.md §1.24.
//
//    Cause 9 never meant damage on those packets. The shooter never sent it,
//    because IsForwardableDamage refuses it, and every receiver checks the
//    same list before calling the engine. So the value was free.
//
//    The shooter recognises its flame inside CShotInfo::Update, the only
//    place the flamethrower still acts in its own name: every
//    CFireManager::StartFire made there is the flame's, and its fleeFrom is
//    the shooter's ped. The owner lights its own entity with its own
//    StartFire, and its own CFire does the burning and the damage.
//
//    A mix of builds only loses the feature. An older owner drops cause 9 at
//    IsForwardableDamage, exactly where it dropped it before, and an older
//    server relays the three packets without reading the weapon (it never
//    bounded it). An older shooter never sends it. Nothing is misread, and
//    the owner's own replay of the flame still lights what it reaches, as it
//    did before.
//
//    The same change gives AmbientPedState's pad byte a name, `flags`, and
//    one bit, AMBIENT_PED_ON_FIRE: the host's pedestrian is burning, and
//    every observer lights a visual-only fire on its replica, the one a
//    burning player already gets. Without it the owner's fire was only ever
//    seen on the owner's screen. Every sender zeroed that byte, so an older
//    host says "not burning", an older observer never reads it, and the
//    server relays rows whole and never did either. Mixed builds just don't
//    see the flames.
//
// 33: cheats. A wire change:
//
//    - C_Cheat / S_Cheat (0xF0/0xF1), out of the 0xF0-0xF7 cheat block.
//    - S_Welcome's flags byte gives its last two bits to the server's
//      CheatRule (SESSION_CHEATS_MASK).
//
//    A cheat runs on the machine of the player who typed it, and ten of the
//    twenty-three change something that machine does not own in a session:
//    the host's sky (four weather cheats), the speed of the clock
//    (TIMEFLIESWHENYOU, BOOOOORING, MADWEATHER), and how every machine's
//    crowd behaves (ITSALLGOINGMAAAD, WEAPONSFORALL). Typed on a non-host,
//    a weather cheat lasted until the next S_WorldState put the host's sky
//    back; the clock ones had a non-host fighting the host's clock by jumps
//    every second. So those now go to whoever owns the thing: the sky to the
//    host, the rest to everybody, carrying the state they left behind rather
//    than "toggle" so a machine that was already the other way does not
//    invert. BANGBANGBANG stays local and was already safe - every wreck of a
//    car somebody else owns is refused by the BlowUpCar detour whoever calls
//    it - but it overflowed the two unowned-wreck queues, which is fixed with
//    no wire change. docs/cheats.md is the whole table.
//
//    It needs a number because the two halves change behaviour together. A
//    new client stops running a sky cheat locally and sends it to the host
//    instead; against an old server that is a cheat that does nothing at all.
//    An old client in a new session keeps today's behaviour, and reads the two
//    new flag bits as nothing.
//
// 34: releasing session cars. The server starts sending
//    S_VehicleDespawn, which it never did. No opcode or layout moves. A
//    session car nobody has been in or within VEHICLE_KEEP_RADIUS_M of for
//    VEHICLE_RELEASE_MS is released on every machine and its row reused, so
//    the 64-car cap counts cars alive rather than cars ever claimed.
//
//    It needs a number because an older client handles the packet it never
//    used to get badly: it destroys a copy even with the local player
//    climbing into it, and keeps streaming snapshots under a netId the
//    server has dropped. The client half that comes with this hands such a
//    car to its engine instead and claims it again (game/carlife.h).
//
//    The rest is client-only: copies no longer count against the engine's
//    traffic cap, and they stay out of CPools::SaveVehiclePool.
//
// Not a version: traffic sirens. No opcode and no layout moves. The second
//    spare byte of each car batch is now `sirenMask`, bit i for cars[i]
//    (CarStateSirenBit): the host's m_bSirenOrAlarm on that car. Before this a
//    police car, ambulance or fire truck in somebody else's traffic chased or
//    raced past with its light bar dark and no sound, on every screen but its
//    host's, because nothing a traffic replica is sent had room for the byte.
//
//    The receiver holds the bit from row to row, like the transform, and
//    writes the byte on the replica every frame after the physics. That is
//    the light bar. The sound is the replica's own status, which is what
//    keeps it the host's: a replica with its host's driver seated is PHYSICS
//    and the audio plays it, one its host's crew got out of is ABANDONED and
//    the audio holds it back (0x0056C4C7), lights still going - exactly the
//    police car parked beside a wanted player. The siren detour covers the
//    gap between a ped row naming a driver and his replica sitting down, as
//    it already did for session cars (game/siren.h).
//
//    InitHeader zeroes both bytes, so an older sender or an older server
//    says "siren off" for every car, which is what every build did before
//    this, and an older receiver never reads the byte. The one mix that loses
//    something new is a new client behind an old server, which relays the
//    rows and drops the mask.
//
// Not a version: drive-bys. No opcode and no layout moves; two fields that
//    already travel carry something new. client/src/game/driveby.h is the
//    design and addresses.h, "the drive-by", the engine side.
//
//    A drive-by is CWeapon::FireFromCar, never CWeapon::Fire, so none of the
//    on-foot path saw one. Observers got no round and a driver sitting still,
//    and the shooter's own machine threw away every hit on another player or
//    a pedestrian somebody else hosts: the ped arm names the car as culprit,
//    and "ours" only ever meant our ped.
//
//    - C_Shot with weapon 19 (UZI_DRIVEBY) is one round. origin and dir are
//      the trail the shooter's engine drew, and speed, which only a
//      projectile used, is that trail's length. An observer draws it -
//      flash, light, trail, impact sound, the report off the car - and never
//      calls the engine's fire path, which would aim, blame and hurt in the
//      observer's player's name.
//    - animId2, while the sender is in a car, is the drive-by overlay (77h
//      left, 78h right) whenever one is held. A seated observer's ped ignored
//      the pose stream and now reads this one field of it.
//    - The hits go out on C_Damage / C_PedDamage / C_VehicleHit / C_CarHit
//      with cause 19, which IsForwardableDamage always allowed.
//
//    Every mix of builds only loses the new part. An older observer refuses a
//    weapon-19 round at IsReplayableWeapon and ignores animId2 on a seated
//    ped, as before; an older shooter never sends either; the server relays
//    C_Shot without reading the weapon. Nothing is misread.
//
// Not a version: money, as a server setting (`money = off | own | shared`,
//    off by default). Four opcodes out of 0xE0-0xE5 and no existing layout
//    moves: C_MoneyChange / S_Money and C_MoneyAward / S_MoneyAward.
//
//    CPlayerInfo::AwardMoneyForExplosion (0x004A15F0) has two callers that
//    disagree about who earned it. The fire timer pays whoever's engine
//    watched the car burn out, so a parked car pays everybody who saw it;
//    the bomb timer pays the local culprit. And a police helicopter another
//    player shot down paid nobody (entry 32). Under `own` and `shared` only a
//    machine that decides the wreck pays, and it pays the culprit: its own
//    player, or the culprit's machine through C_MoneyAward, keyed when
//    several machines decide the same car so the server delivers it once. A
//    shooter credited with somebody else's helicopter pays himself the $250.
//    Under `shared` every change to anyone's cash also goes out as a delta
//    and the server's total comes back to everybody.
//
//    S_Welcome's flags are full, so the rule arrives in S_Money straight
//    after it, and only when it is not off. Why it may not need a number: an
//    old build drops all four opcodes, a new client behind an old server is
//    never told a rule and stays at off, and a session left at off sends
//    nothing new. The mix that loses something is an old client in a new
//    session with money on: awards forwarded to it are dropped and its cash
//    stays out of the pool. Nothing is misread.
//
// 35: a parked session car burns. No opcode and no layout moves;
//    C_VehicleHit means one more thing.
//
//    A session car nobody was driving or settling could be shot all day and
//    never catch fire. Every machine writes the session's last health back
//    onto its copy every frame (ApplyRemoteVehicle), so a hit lasted a frame,
//    health never stayed under 250 and the fire block never ran. And had it
//    stayed there, every machine's own fire timer would have run and every
//    machine would have blown it up on its own.
//
//    So a car nobody holds now gets an owner when it is shot. The shooter's
//    hit is refused locally and sent as C_VehicleHit, and where the server
//    used to drop that it makes the shooter the custodian, announces it on
//    S_VehicleCustody and sends the hit back behind it (Session::CustodyForHit).
//    The custodian's engine takes it, streams the health, and everybody else
//    refuses damage and holds the fire timer, as for any custody. A custodian
//    keeps a burning car until it goes up (client.h, CustodyMayEnd), so its
//    timer's BlowUpCar is the one wreck, sent as UNOWNED_SESSION and replayed
//    once everywhere else. A driver who bails out of a burning car keeps it the
//    same way. Blasts are unchanged: every machine replays them.
//
//    It needs a number for the reason 30 did. A new client behind an old
//    server refuses its hit on a parked session car and the server drops it,
//    so the car can't be shot at all - today the hit at least lands for a
//    frame, and one that kills outright still wrecks it. An old client in a
//    new session never forwards, and reads the custody it is sent the way it
//    reads any other.
//
// 36: melee. DamageBody and PedDamageBody
//    grow two bytes, `melee` and `hitLevel` (MELEE_*), so C_Damage/S_Damage
//    and C_PedDamage/S_PedDamage are two bytes longer. client/src/game/melee.h
//    is the design, addresses.h "fists and the bat" the engine side.
//
//    A punch or a bat hit used to be half decided on the wrong machine. The
//    attacker's engine forwarded the health, and then went on to play the
//    victim's side of the fight - StartFightDefend, the knockdown, the shove -
//    on its own copy of him, which the pose stream then fought. The victim's
//    engine only ever got the health: no defend, no knockdown, and a bat did
//    half what it does to a player in single player, because the attacker's
//    engine asked IsPlayer of a copy. Now the attacker's machine says which
//    fight path landed it and with what move, the owner plays that path's
//    reaction on the real ped, and copies never react to a melee hit.
//
//    It needs a number because the layout moved: an older build reads these
//    packets at the old size and drops them.
//
// Not a version: which hosted peds and cars go in each C_PedStates and
//    C_CarStates batch. No opcode and no layout moves; only the sender's
//    choice of rows changed (client/src/game/streampick.h).
//
//    A host took the twelve peds and eight cars nearest its *own* player,
//    with no memory from one tick to the next, so the rest of what it hosted
//    never got a row and stood frozen on every other screen - the one player
//    the ranking used is the only one who never reads the batch. Now every
//    row has a credit that grows each tick it is left out, by more the nearer
//    it is to another player, and the batch is filled from the top. A row
//    that has never gone out, or whose seat, fire, siren, horn or health just
//    changed, goes first, and so does a honking car or a burning ped that is
//    about to lapse on its receiver. The batches are the same size as before.
//
//    Every mix of builds reads the same packets. An old host keeps sending
//    its nearest rows and a new receiver takes them as it always did; a new
//    host's rows are ordinary rows to an old receiver and to the server.
//
// Not a version: pedestrians and cops fight players on every machine. Six
//    opcodes, 0x90-0x95, and no existing layout moves: C_NpcShot /
//    S_NpcShot, C_NpcDamage / S_NpcDamage, C_NpcVehicleHit / S_NpcVehicleHit,
//    and bits 1..4 of AmbientPedState::flags (AMBIENT_PED_WEAPON_*).
//
//    An NPC only ever hurt the player whose machine hosts it. Its round or
//    punch reaching another player's copy there was refused and forgotten,
//    so half of every hostile crowd was harmless to each player, and on every
//    other screen it stood empty-handed and silent while that player's health
//    went down. Now the host forwards the hit to the player it landed on, or
//    to the driver or custodian of the car it landed on, and the victim's
//    engine applies it, blamed on its replica of the NPC. The weapon rides the
//    ped row, so the replica holds the same gun, and each round from one of
//    the five guns that trace a ray goes to everybody within 150 m to be drawn
//    through their own CWeapon::Fire on the replica, which decides nothing.
//
//    Every mix of builds only loses the new part. An old build drops the six
//    opcodes and never reads the weapon bits; an old host never sends them; a
//    new client behind an old server has them dropped there. In each case an
//    NPC is harmless and unarmed on the other screens, as before. Nothing is
//    misread.
//
// Not a version: a sniper round is heard on every machine. No opcode and no
//    layout moves. C_Shot with weapon 7 used to carry the shooter's body
//    heading and went nowhere, because an observer cannot run
//    CWeapon::FireSniper. It now carries the line the shooter's camera was
//    looking down, and an observer plays the report off the shooter's ped and
//    the impact where that line meets something (combat.h, SniperProbe). The
//    victim was already hurt by it; now somebody heard it. An older observer
//    refuses weapon 7 at IsReplayableWeapon, as before.
//
// Not a version: the chat is on the HUD, with who came and went. No opcode
//    and no layout moves; C_Chat and S_Chat are as they were, and the one
//    new thing on the wire is PJF_ARRIVED, a spare bit of S_PlayerJoin::flags
//    the server sets on the live announcement and never on a backfill, so a
//    client says "bob joined" for bob and not for everybody it is told about
//    on its own way in. An older client never reads the bit; behind an older
//    server nobody's arrival is announced, and leaving still is.
//
// Not a version: a traffic car's dents. No opcode and no layout moves. The
//    host of a traffic car now sends its panels and doors on C_VehicleDamage,
//    the packet a driver's go on, and the server takes it for a netId that
//    names traffic from that car's host and nobody else (Session::NoteCarDamage),
//    keeps the word for a joiner and carries it into a promotion. A replica is
//    collision-proof, so until now it stayed undented everywhere but on its
//    host. An older server refuses the report at MayReportVehicle, as it always
//    did; an older client drops an S_VehicleDamage for a netId it has no
//    session car for. Either way the car is as it was: dented on its host.
//
// Not a version: a car whose driver or custodian disconnects is handed, as a
//    custody, to the nearest other player within 80 m instead of being pinned
//    wherever the last snapshot left it (Session::HandOverVehiclesOf). It is
//    the S_VehicleCustody every build already reads, sent after the leave.
//
// Not a version: the ping in the player list. One new opcode, S_PlayerPings
//    (0xB2), that the server broadcasts once a second with every slot's round
//    trip as ENet measures it. An older client drops the opcode and shows no
//    ping; an older server never sends it.
//
// Not a version: a player's own limb comes off on every screen. The same
//    C_PedBodyPart a pedestrian's host sends, naming the sender's own player
//    netId, which the server now takes from that player alone. An older server
//    refuses it and an older client finds no pedestrian by that netId and
//    drops it, so the limb stays on as before.
//
// Not a version: somebody else's rocket no longer crashes a Dodo on the
//    observer's machine. It used to, and the crime went on the observer's
//    player and the crash blasts went out as the observer's on top of the
//    shooter's. Now the shooter's machine decides the hit, as it decides every
//    projectile it fires, and its explosion ends the observer's copy. The
//    plane's fall is only on the shooter's screen. game/heli.h has the lead
//    and how it is checked.
//
// Not a version: the server's `hiddenPackages = shared | perplayer`, default
//    shared. Nothing on the wire changes and no client knows the rule: under
//    perplayer the server keeps a package's lock per player, grants each
//    player their own, sends nobody else an S_PickupTaken for it and hands a
//    joiner nobody else's. Every build of the client reads that the way it
//    reads a pickup nobody has taken yet.
//
// Not a version: a police helicopter's owner says when it kept the reward.
//    Bit 0 of the byte after HeliGoneBody::creditPlayerId, which was padding
//    and went out as zero: HELI_GONE_OWNER_KEPT, the owner's engine paid the
//    $250 and the statistics for a helicopter somebody else shot down and
//    could not take them back. The shooter then registers the crime and
//    nothing else. An older owner sends zero and the shooter pays itself as
//    before; an older shooter never reads the bit.
//
// Not a version: a shove settles a parked car. C_VehicleHit with weapon
//    VEHICLE_HIT_PUSH and no amount, from the machine whose car just pushed a
//    session car nobody holds; the server makes it the custodian, as for a
//    shot, and relays nothing. An older server relays it as a hit, which
//    every receiver drops as a weapon it cannot forward; an older client
//    never sends it, and the car stays a wall on its screen. The server
//    takes it only from somebody at the wheel of another car.
//
// Not a version: desync probes. Two new opcodes, C_DesyncProbe (0xB3) and
//    S_DesyncReport (0xB4), diagnostics only. An older server drops the probe
//    and never answers, so a newer client's list simply shows no distance;
//    an older client never probes.
//
// Not a version: a car that changes hands is played on its new driver's
//    clock. Nothing on the wire; the client's buffer for a car starts again
//    when the player whose snapshots it holds changes, where it used to drop
//    every snapshot older than the last one the previous driver sent - and a
//    machine whose game started later sends nothing but those.
//
// Not a version: a server password. One new opcode, C_Password (0xB5), sent
//    right behind the hello by a client that has a password, and one new
//    reject reason, REJECT_BAD_PASSWORD (3). A server without a password
//    drops the opcode or ignores it; one with a password turns away a client
//    that never sends it, older ones included, which is the point.
//
// Not a version: a joiner is told who is settling which car. The backfill
//    carries one S_VehicleCustody per custody in progress, after the seats;
//    every build reads it the way it reads a custody announced live.
//
// Not a version: a car's own blast is no longer relayed as C_Explosion. The
//    wreck's packet already makes every copy blow up through BlowUpCar, which
//    adds the explosion itself, so an older observer simply stops seeing it
//    twice. And the backfill's S_PickupTaken names nobody (INVALID_PLAYER)
//    rather than the collector, whose slot a joiner may now have.
//
// Not a version: a kick keeps the player out. The server hangs up with
//    LEAVE_KICKED as the ENet disconnect's data, as it always did, and a
//    client that was welcomed reads it and stops reconnecting until the
//    game restarts; an older client comes straight back as before. A
//    connection that never says hello is dropped after HELLO_WAIT_MS and is
//    sent nothing meanwhile, and the server keeps two connections past the
//    last slot so a ninth player hears REJECT_FULL.
//
// Not a version: Claude's outfit. One new opcode pair, C_PlayerLook /
//    S_PlayerLook (0xCA/0xCB), carrying the name model 0 is loaded under on
//    the sender's machine - "playerp" in the prison clothes, "player" after.
//    The script swaps the look by renaming model 0 in place (UNDRESS_CHAR,
//    CStreaming::RequestSpecialModel), so the model id on C_PlayerModel is 0
//    either way and never said which one it was. The server keeps it for the
//    backfill. An older server or client drops the opcode and every remote
//    Claude looks like whatever that machine's own model 0 is, as before.
//
// 37: the vote before a rampage. Four new
//    opcodes, C_RampageVote / S_RampageVote / C_RampageArrived /
//    S_RampageTeleport (0xC4-0xC7), and two spare bits of PickupIdent::flags,
//    PICKUP_F_RAMPAGE on a claim for a skull and PICKUP_F_VOTED on the grant
//    that ends a vote that passed. No layout moves. A mix misbehaves, which
//    is why it can't ride as an unnumbered entry: an older client never sets
//    PICKUP_F_RAMPAGE, so the server grants it the skull at 4 m and it starts
//    a rampage for the whole session with no vote, and it drops the vote
//    opcodes, so it counts toward the 75% and can never say yes - with two
//    players nothing would pass. Behind an older server a new client's claim
//    is simply granted, the skull goes at the touch and nobody is moved.
//
// 38: a leaver's crowd is adopted. One new
//    opcode, S_AmbientAdopt (0xD0 then, 0xCC since 39), no layout moves.
//    When a player leaves, each of his ambient peds and traffic cars that another player is near
//    enough to keep is given to the nearest one (server/core/adopt.h), whose
//    machine turns its replica into a ped or car of its own and streams it
//    under the same netId; the rest are despawned as before. A mix
//    misbehaves both ways round. An older client drops the opcode, so its
//    rows keep the leaver as owner and it throws away every row the new owner
//    streams (OnPedStates / OnCarStates check the owner): the adopted crowd
//    freezes on its screen, locked cars in the road included, until the new
//    owner's engine reaps each one. And an older client picked as the adopter
//    never takes anything over, never streams it and never lets it go, so the
//    same frozen crowd stands on every screen for as long as it is connected.
//    Behind an older server nothing is adopted and everything goes as before.
//
// Not a version: a carjack is played on every screen. One new opcode pair,
//    C_JackingVehicle / S_JackingVehicle (0xDA/0xDB), the entry intent's own
//    body, sent in place of C_EnteringVehicle when the sender's entry is a
//    jack and relayed for a traffic car as well as a session car. Every
//    receiver plays the engine's jack on its copy of the jacker and its own
//    engine drags out whoever is in the seat there. An older server drops the
//    opcode and an older client ignores it; either way the jack looks the way
//    it did before - the victim put beside the car and an ordinary get-in -
//    because the claim and the seat handover are unchanged.
//
// Not a version: the host kicks from inside the game. One new opcode, C_Kick
//    (0xB6), which a client sends when its player is the session's host and
//    types "/kick" and a number from the player list. The server takes it
//    from the host alone and throws the player out the way its window does.
//    An older server drops the opcode and nobody is kicked; an older client
//    never sends it.
//
// Not a version: the session's one mission (docs/missions.md 5, 9, 11, 12;
//    docs/protocol.md 1.29, 1.30). Twelve opcodes in 0x42..0x4D, free until
//    now: the claim at a start gate and its answer, who the mission waits
//    for, the start, the session's mission as everybody sees it, the end,
//    the death rule's order to fail it, a checkpoint's wait, what the
//    owner's mission shows, one instruction at a time, and the values behind
//    its HUD's widgets. Two more, 0x4E and 0x4F, for a participant saying
//    its models are in before a cutscene, 0x52 and 0x53 for the seats the
//    mission's passengers need, 0x54 and 0x55 for one of its objects
//    broken, 0x56 and 0x57 for one of its floating packages taken, 0xB7
//    and 0xB8 for a game in a mission of its own, or started over, 0x98
//    and 0x99 for a kill somebody else made, and 0x9A and 0x9B for what a
//    participant's engine answers to the owner's questions. Three
//    more in 0xD0..0xD2 for what a mission leaves behind in the campaign.
//    The server sends every joiner S_MissionState, and a client claims nothing before
//    it has one, so behind an older server every machine's missions stay
//    its own, as they always were. A client only reaches the script engine
//    with `missions = on` in its CoopIII.ini; an older client never sends
//    any of this and plays its missions alone.
//
// 39: the missions and the free roam work meet. Two values had been handed
//    out twice while they were apart, and they are moved here: S_AmbientAdopt
//    goes from 0xD0 to 0xCC, because C_CampaignDelta has 0xD0, and
//    PICKUP_F_STASH goes from bit 1 to bit 3, because PICKUP_F_RAMPAGE has
//    bit 1. A 38 build reads a campaign delta as an adoption and a mission's
//    stash as a rampage skull, so the number moves with them.
//
// Not a version: a start waits for a game in its own intro for
//    MISSION_BUSY_WAIT_MS at most. S_MissionWaiting's two pads become
//    busyMask, which of the missing are in a mission of their own, and
//    goesOnInS, how long until the start goes on without them. An older
//    server sends zeros there and waits for them as it always did; an older
//    client reads neither and says who is missing, as before.
//
// 40: the sky follows the session's mission
//    (sky.h). No layout moves and no opcode is added. While the session's
//    mission runs, the server takes C_WorldState from its owner instead of
//    the host and sends a sky cheat there, and every client works out the
//    same holder from S_MissionState; the owner takes it from its own
//    START_MISSION, before the server has answered. A mix misbehaves both
//    ways round. An older client that is the host never applies a world
//    packet, so during somebody else's mission its screen keeps its own
//    clock and weather while every other screen shows the mission's, and
//    its reports are dropped. A newer client behind an older server keeps
//    the sky its own mission set while the server goes on sending everybody
//    else the host's, so the two halves of the session see different skies
//    for the whole mission - the thing this change is for.
//
// 41: a pedestrian rides in a player's car.
//    No opcode and no layout moves. AmbientPedState::vehicleNetId, which only
//    ever named the sender's own traffic, may now name a session car: the
//    mission's 8-Ball in the Kuruma once a participant has its wheel, Misty
//    in the owner's car. The receiver seats its copy there, in the seat the
//    row names or another free one, and never over a player; with no seat
//    free it keeps the copy out of sight and out of the car's collision
//    instead of standing it in the car. A mix misbehaves one way round. An
//    older receiver finds no traffic of that number and stands the copy where
//    the seat is, inside the car: up through its roof on every screen, and on
//    the driver's own machine in the car's way every frame, so a participant
//    at the wheel of the mission's car can barely move it - the thing this
//    change is for. An older sender never names a session car, and a newer
//    receiver behind it shows what an older one would.
//
// 42: a failed mission and its retry. No
//    layout moves and no opcode is added; what the same packets mean does.
//    The owner sends C_MissionEnded when its mission's script ends
//    (TERMINATE_THIS_SCRIPT), not at the first MISSION_HAS_FINISHED, which a
//    failure runs twice; everything its cleanup does in between goes to
//    everybody as C_MissionEffect, and its campaign delta is read at the
//    end, after the cleanup has put its flags back. On S_MissionState going
//    idle the server lets go of every session car the mission made that
//    nobody is in (S_VehicleDespawn), and a claim's players must be alive
//    to be at the start. The owner sends each participant who gets out of
//    the car its mission asks IS_PLAYER_IN_CAR about a PRINT_NOW of its own
//    (MissionEffectBody::onlyTo), and never sends anybody its own "get back
//    in the vehicle". Two instructions join the replay list:
//    REMOVE_PARTICLE_EFFECTS_IN_AREA and DONT_REMOVE_OBJECT. A mix
//    misbehaves the old way: an older owner ends the session's mission at the
//    first MISSION_HAS_FINISHED, so a retry can start while its game is still
//    cleaning up and respawning, and its delta carries flags the cleanup
//    would have put back; an older participant runs neither new instruction
//    and lets go of the objects a mission keeps; an older server releases no
//    car and counts a dead player at the start.
//
// 43: the mission's player changes clothes on
//    every participant. No layout moves and no opcode is added; one
//    instruction joins the replay list. The owner sends UNDRESS_CHAR as a
//    C_MissionEffect when its mission runs it on the owner's own player -
//    Give Me Liberty's 'playerp' back to 'player' - with the char operand
//    blanked to 0 and the eight-byte label as it stood. A participant never
//    runs it as sent: it takes the label as the look its own player should
//    wear, and changes into it with its own engine's UNDRESS_CHAR and
//    DRESS_CHAR once that player is on foot and out of any door, death or
//    arrest (game/outfit.h). The look sync then carries the change to every
//    other screen, as it does any change of model 0. A mix misbehaves the
//    old way: an older owner never sends it, so only the owner changes; an
//    older participant finds no such instruction on its list and drops it,
//    and keeps the clothes it had.
//
// 44: the car radio (docs/radio.md,
//    docs/protocol.md 1.33). Two opcodes out of the free 0x9C..0x9F:
//    C_VehicleRadio, a player in a session car saying the station his
//    listener just put it on - the radio key, F9's user tracks, switching it
//    off - or, from the driver, the station a car the session has no station
//    for already had; and S_VehicleRadio, the session's station for that car,
//    to everybody, the sender included, and in the backfill. Every copy of
//    the car carries it in CVehicle::m_nRadioStation, so everybody in it
//    hears one station, and whoever gets in later hears it too. Where in a
//    station's one long stream a machine starts playing is not on the wire at
//    all: it is the session clock modulo the stream's length, so two cars on
//    the same station play the same moment. No layout moves. A mix misbehaves
//    one way round: an older server drops both opcodes, so each copy of a car
//    keeps its own station, as before, and only the position follows the
//    clock; an older client never sends one and ignores what it is sent, so
//    it keeps its own station and its own positions, and a newer passenger
//    riding with it hears the station the newer machines agreed on.
//
// 45: the mission's pedestrians and cars
//    catch up. Two opcodes out of the free 0xDE..0xDF: C_MissionCatchUp, a
//    participant who has just come into the session's running mission -
//    starting it with the others, late, back from a mission of its own game's
//    or from a dropped connection, or after its game started over - asking
//    for what the mission has made; and S_MissionCatchUp, the server's count
//    of what it sent back to that player alone just before it: an S_PedSpawn
//    and an S_CarSpawn, tempId 0 as in the backfill, for every AMBIENT_MISSION
//    pedestrian and car it has from somebody else, and an S_PedDeath for each
//    of those lying dead. No layout moves. The owner's side changes without a
//    new packet: every car its mission's CREATE_CAR makes is hosted as the
//    mission's, whether or not it went past the CWorld::Add detour inside the
//    instruction, and whatever the mission made that the engine takes out of
//    the world and puts back (a teleport, a pedestrian sitting down in a car)
//    keeps its netId, where it used to go from every other screen. A mix
//    misbehaves the old way: an older server drops the ask, so a late
//    participant has only what the backfill and the live spawns gave it; an
//    older client never asks and ignores the answer.
//
// 46: skipping a cutscene together. Two
//    opcodes, the last two free of 0x98..0x9F, no layout moves:
//    C_CutsceneState (0x9E), which cutscene the sender's game is in whenever
//    the engine's own skip test would let it be skipped, or none, and a press
//    of skip as a yes in its count; S_CutsceneVote (0x9F), the count, to
//    everybody in that cutscene, or the order to skip it now
//    (server/core/cutscenevote.h has the rules). With another player in the
//    same cutscene the skip input no longer skips on its own machine. A mix
//    falls back to the old rule rather than misbehaving: an older client
//    never reports a cutscene, so it counts for nobody and the newer players
//    beside it find themselves alone and skip as before; an older owner
//    skips alone and its CLEAR_CUTSCENE ends everybody's scene, as before;
//    behind an older server nobody hears a count and every machine skips on
//    its own, a participant of the session's mission not at all.
//
// 47: a pedestrian in a car nobody else
//    can see. AmbientPedState's flags byte gains AMBIENT_PED_IN_UNSEEN_CAR
//    (bit 5), set by the host of a pedestrian sitting in a car the session has
//    no name for; the observer keeps its replica hidden and out of the
//    collision instead of drawing a driver in mid-air where the seat is. No
//    opcode and no layout moves. A mix misbehaves the old way: an older sender
//    never sets the bit, so its unseen drivers still float on a newer screen,
//    and an older receiver ignores it. The same change makes a machine host
//    its own traffic again whatever happens to be in the pool slot beside the
//    model index (client/src/game/vehicle.cpp, SampleAmbientCarIdentity), with
//    nothing on the wire changing for it.
//
// 48: a unique stunt jump's shot reaches
//    the players riding in the car. Two opcodes out of the free 0xF8..0xF9,
//    no layout moves: C_StuntCamera (0xF8), the driver's USJ thread pointing
//    its fixed camera at the car, with the camera's position, and the end of
//    it; S_StuntCamera (0xF9), the same to every player the session has in a
//    passenger seat of that car, and to nobody else (docs/protocol.md
//    §1.35). Without a packet, three things change for a passenger: the
//    sub-mission key starts no odd job from a passenger seat, a car he rides
//    in is never in the air to his own stunt threads (so no unique jump and
//    no insane stunt bonus but the driver's), and the car he rides in is put
//    where the session says before this frame's camera looks at it. A mix
//    misbehaves the old way: an older server drops the shot and the riders
//    see the jump from behind; an older driver sends none.
//
// 49: a car bomb goes off on every screen,
//    and so does a mine. C_VehicleBomb and S_VehicleBomb grow from 9 to 13
//    bytes: `blame`, the player whose bomb it is (whoever set the fuse going
//    while it burns, whoever had it fitted otherwise), and `fuseMs`, what is
//    left of a lit fuse. Every copy of the car names that player's ped as its
//    rigger and lights its own fuse with it, so the ignition goes off
//    whoever gets in and blames the bomber, and a timer outlives a change of
//    driver. The server keeps each car's last word and hands it to a joiner.
//    A detonator press already travelled as a C_Shot with
//    WEAPONTYPE_DETONATOR; every machine now answers it by setting off the
//    remote bombs that player rigged on its own copies, where it used to
//    refuse it. A replayed wreck is blamed on whoever its copy's bomb names,
//    no longer on nobody. Two opcodes out of the free 0xEC..0xEF, the other
//    two still free: C_MineBlast (0xEC), where the sender's mine went off,
//    and S_MineBlast (0xED), relayed to everybody else, who take their own
//    mine there out of the world and set off the engine's explosion in its
//    place. DROP_MINE and DROP_NAUTICAL_MINE join the mission replay list.
//    A mix misbehaves: the bomb packets' length changed, so a newer machine
//    and an older one drop each other's bomb reports and each keeps its own
//    copy's bomb, as before bombs travelled; an older client refuses the
//    detonator shot and never sends nor applies a mine blast, so its mines
//    stay its own.
//
// 50: a medic's revive and a fire truck's
//    hose on every screen (docs/protocol.md 1.37). Four opcodes, 0xF2..0xF5,
//    out of the cheat block's free six, no layout moves. C_PedRevive, from
//    the machine whose medic stood a dead pedestrian up - one it hosts or
//    its replica of somebody else's - and S_PedRevive to everybody else, the
//    pedestrian's host included, whose engine stands him up with the medic's
//    own instructions; the session marks him alive again, so a joiner is not
//    handed a corpse. C_WaterCannon, unreliable at the snapshot rate while a
//    fire truck sprays, from the machine that aims it (its driver's, or the
//    host of a truck nobody drives), the jet's start and direction in the
//    truck's own frame; and S_WaterCannon to everybody else, whose copy of
//    the truck sprays the same jet through CWaterCannons::UpdateOne. What the
//    water does is then each engine's to decide about what it owns. A mix
//    misbehaves the old way: an older server drops all four, so a revive
//    stays on the medic's machine and a hose on the aimer's; an older client
//    never sends either and ignores both, so its medics treat only its own
//    pedestrians, a revive of one it hosts leaves him dead there, and its
//    copies of other people's trucks go on spraying by themselves.
//
// 51: a car's end (docs/protocol.md
//    1.38, client/src/game/wreck.h). No opcode and no layout
//    moves; what changes is what three packets may say. S_VehicleSpawn in the
//    backfill now carries a session wreck, VEH_WRECKED and zero health, for
//    WRECK_BACKFILL_MS after it went up, and the joiner builds the shell
//    without the blast. The machine that decided a wreck - its driver's
//    C_VehicleBlowUp, the first C_UnownedBlowUp for a car nobody drove - is
//    made its custodian on the S_VehicleCustody after the blast, and streams
//    it on C_VehicleState with VEH_WRECKED set until it lies still; the
//    server takes and relays those for a destroyed car from its custodian
//    alone, and drops a VEH_WRECKED snapshot for a car that is not yet one.
//    Two things ride packets whose meaning does not change: the thrower of a
//    blast that left a car nobody holds standing sends C_VehicleHit with the
//    blast's cause (18), which makes it the custodian as a shot does and is
//    applied by nobody; and only the session host runs the fire timer of a
//    car nobody holds, where every machine used to. A mix misbehaves one way
//    round: an older client handed a VEH_WRECKED spawn builds an intact car,
//    which is the bug the backfill used to avoid by leaving wrecks out. The
//    rest degrades to the old behaviour: an older server gives nobody a
//    wreck, so every machine holds it where it went up; an older client given
//    one ends the settle at once and drops a wreck's snapshots.
//
// 52: a car's own state (docs/protocol.md
//    1.39). Four opcodes, the block 0xE8..0xEB: C_VehicleAlarm /
//    S_VehicleAlarm, reliable, the milliseconds a session car's alarm has
//    left when the engine simulating it sets it off (0 when it stops early),
//    from the driver or the settler, kept by the server and backfilled with
//    what is left; C_VehicleAim / S_VehicleAim, on the snapshot channel, where
//    the driver's tank turret or fire truck cannon points (m_fCarGunLR/UD),
//    kept and backfilled too. Two bits of VehicleStateBody::flags that were
//    free: VEH_TAXI_LIGHT (5) and VEH_HANDBRAKE (6), sampled and applied with
//    the engine and siren, and replayed in S_VehicleSpawn like them. No
//    layout moves. And a sender-only change: a traffic car's horn bit now
//    goes out for a police car, ambulance or Enforcer with its siren on,
//    where the timer is the fast wail, which the replica - given the siren
//    bit in the same row - plays as its host does. A mix misbehaves only by
//    missing things: an older server drops the four opcodes and keeps the
//    flags byte as it comes, so nobody hears an alarm or sees a turret turn
//    but the taxi light and the handbrake still travel; an older client
//    never sends them, ignores them and the two bits, and a newer host's
//    stuck police car wails fast on it too, because every build since the
//    siren bit writes the siren beside the horn.
//
// 53: a pedestrian's door. Bits 6
//    and 7 of AmbientPedState::flags that every sender left zero, no layout
//    moves and no opcode. AMBIENT_PED_ENTERING: the host's pedestrian is
//    opening a door of the car `vehicleNetId` names, and `seat` packs the seat
//    he ends in with the door he goes in by (AmbientPedEntrySeatByte); the
//    observer's copy plays the same door, then sits where the entry leaves
//    him. AMBIENT_PED_EXITING: he is climbing out of the seat the row names,
//    and the copy climbs out with him. Misty getting into the car a player
//    drives is the case that showed it: every other screen only ever saw her
//    appear in her seat. A mix misbehaves one way round: an older observer
//    reads an entering row as a seat and warps the copy in while its host is
//    still at the door, a second early; an older host never sets either bit
//    and every copy warps as before.
//
// 54: a mission's bomb and a bomber's pay
//    (docs/protocol.md 1.36). Two opcodes, the rest of the mines' block, no
//    layout moves. C_MissionBomb (0xEE), from the owner's machine when its
//    mission's ARM_CAR_WITH_BOMB fits a bomb to a car the session names, and
//    S_MissionBomb (0xEF) to everybody else: the bomb is the owner's on every
//    copy, the one the receiver simulates included. A participant's replay
//    of the same instruction names the owner as the rigger rather than its
//    own player, so the first machine to hold the car no longer claims it.
//    The server keeps the flag with the car's bomb and backfills it; a later
//    C_VehicleBomb that changes the type or whose it is ends it. A detonator
//    press (C_Shot, WEAPONTYPE_DETONATOR) also sets off a remote bomb the
//    running mission fitted when it comes from anybody in that mission,
//    blamed on the owner, the rigger. No packet for the pay: the machine a
//    bomb goes off on now makes CVehicle::ProcessDelayedExplosion's award
//    when the bomber is another player's ped, under money own or shared, and
//    it goes to the bomber through the existing C_MoneyAward. A mix
//    misbehaves the old way: an older server drops both opcodes, an older
//    client ignores S_MissionBomb and claims the bomb for itself as before,
//    and an older machine a bomb goes off on pays nobody for it.
//
// 55: everybody into the car the mission
//    puts its player in, and only as many out of a car as a mission's
//    passengers need (docs/protocol.md 1.40). Two opcodes, the rest of the
//    money block: C_MissionBoard (0xE4), from the owner's machine once its
//    mission's WARP_PLAYER_INTO_CAR or WARP_CHAR_INTO_CAR has put its player
//    in a car the session names, with the passenger seat it gives each
//    participant, and S_MissionBoard (0xE5), relayed to everybody else.
//    MissionSeatCar's pad byte is now `leave`: the players who are to get
//    out of the car when MISSION_SEAT_LEAVE is set, no layout moves. A mix
//    misbehaves the old way: an older owner sends `leave` as 0, which a newer
//    participant reads as everybody out, an older participant ignores it and
//    gets out whether he was named or not, and an older server drops both
//    opcodes, so nobody follows the owner into the car.
//
// 56: a mission with three or four players
//    in it (docs/protocol.md 1.29, docs/missions.md 15). No opcode is added
//    and no layout moves. S_MissionWaiting's goesOnInS now counts a
//    checkpoint down too: a checkpoint waits MISSION_CHECKPOINT_WAIT_MS at
//    most, and then the owner's machine reports it over (missingMask 0) and
//    its mission goes on. An owner whose connection dropped sends
//    C_MissionStarted again, launch key 0, for the mission its script still
//    runs, and the server takes it up as the session's as it takes any
//    start it did not grant. The server drops a silent connection in 5 to
//    10 s (net.h, NET_PEER_TIMEOUT_*) instead of ENet's 5 to 30. A mix
//    misbehaves the old way: an older server never counts a checkpoint down
//    and an older client never shows it, an older owner's checkpoint waits
//    for ever, and an older owner back from a dropped connection keeps its
//    mission to itself.
//
// 57: a contact's marker and the odd jobs'
//    rewards (docs/protocol.md 1.30). No opcode and no layout moves; what a
//    C_MissionEffect and a C_CampaignDelta op may say changes. A blip operand
//    may now be `02 lo hi`, a main-script global each machine reads for its
//    own, where it was always a literal handle: REMOVE_BLIP and the other
//    blip instructions on a live blip the mission did not make go that way,
//    as MISSION_EFFECT_RUN, and in the delta's ops. ADD_SPRITE_BLIP_FOR_
//    CONTACT_POINT (02A7) joins the replay list with its output a global, in
//    the delta's ops too, and every machine takes off what that global holds
//    before making it. SET_PLAYER_NEVER_GETS_TIRED becomes a delta op and is
//    no longer put back when the mission ends. The odd jobs' stat counters
//    (0315, 0316, 03FD..0404) join the list. A mix misbehaves one way round:
//    an older participant drops a blip by global and a 02A7, and has none of
//    the stats, as before; an older machine applying a newer delta runs its
//    02A7 without taking the old marker off first, so a marker it already
//    had live is left twice on its radar.
//
// 58: what a mission does to the streets
//    and says out loud, on every participant's machine, and fewer packets
//    for it (docs/protocol.md 1.29). One new MissionEffectKind,
//    MISSION_EFFECT_SPHERE_NEW (8): ADD_SPHERE's result is the participant's
//    for `ownerBlip`, and REMOVE_SPHERE names the owner's handle, the way a
//    blip does. The replay list grows by CLEAR_AREA, both density
//    multipliers, SET_ZONE_CAR_INFO, SET_ZONE_PED_INFO and SET_GANG_WEAPONS
//    (the last three kept in the campaign delta as world instructions), the
//    mission audio (LOAD, PLAY, SET_POSITION, CLEAR), ADD_PAGER_MESSAGE,
//    PRINT_STRING_IN_STRING, ADD_BLIP_FOR_CHAR_OLD, ADD_SPHERE,
//    REMOVE_SPHERE, LOAD_SCENE, the credits' start and stop, and
//    RESTART_CRITICAL_MISSION as a TELEPORT. SET_ZONE_CAR_INFO's sixteen
//    numbers only fit the 64-byte code as 16-bit literals (type 5), which
//    the interpreter reads like any other. A corona the mission draws every
//    frame goes the way a blue marker does: a RUN of DRAW_CORONA with nine
//    literals and a tenth that says up or down, resent every 2 s while it
//    stays, never run as it stands by a newer participant. No layout moves.
//    And the owner sends less: an identical repeat of an instruction that
//    sets something is dropped, a changing one goes at most ten times a
//    second with its last value kept for when the gap is up, and a blip the
//    mission takes off and puts back in the same frame stays one blip, moved
//    at most twice a second. A mix misbehaves only by missing things: an
//    older client finds none of the new instructions on its list and runs
//    none of them, the corona and the sphere included; an older owner sends
//    none of it, and every repeat as before.
//
// 59: who is in the menu, and a mission's
//    gangs turning on everybody (docs/protocol.md 1.42). One opcode pair,
//    C_PlayerAway / S_PlayerAway (0xBE/0xBF): a byte, 1 while the sender's
//    pause menu is up, relayed to everybody else and given to a joiner after
//    the joins. SET_THREAT_FOR_PED_TYPE and CLEAR_THREAT_FOR_PED_TYPE (03F1,
//    03F2) join the replay list as world instructions, so the campaign delta
//    carries them too. No layout moves. A mix misbehaves only by missing
//    things: an older server drops C_PlayerAway and an older client never
//    shows it; an older participant runs neither threat instruction, so only
//    the owner is set on, as before.
//
// 60: the owner's scenes on everybody's
//    screen (docs/protocol.md 1.29). No layout moves. CAMERA_ON_PED (0159)
//    pointed at the owner's own player ped goes with WIRE_OWN_PLAYER
//    (0x10000, game/mission.h) in place of a netId, and each participant's
//    camera goes to its own player; it used to be dropped. A light and a
//    shadow the mission draws every frame, DRAW_LIGHT (0250) and DRAW_SHADOW
//    (016F), go the way the corona does: a RUN of the instruction with its
//    own six or ten operands as literals and one more that says up or down,
//    resent every 2 s while it stays. A mix misbehaves: an older participant
//    reads 0x10000 as netId 0, so its camera may go to whichever pedestrian
//    has that name, and it runs none of the lights or shadows; an older owner
//    sends neither.
//
// 61: the 1100..1154 instructions a
//    mission runs (docs/missions.md 15). No opcode is added and no layout
//    moves; what a C_MissionEffect may say grows. LOAD_COLLISION_WITH_SCREEN
//    (044C), MAKE_CRAIGS_CAR_A_BIT_STRONGER (044F, the car as its netId),
//    SET_JAMES_CAR_ON_PATH_TO_PLAYER (0450, to the car's holder alone) and
//    LOAD_END_OF_GAME_TUNE (0451) join the replay list, all as
//    MISSION_EFFECT_RUN. A participant runs 044C only while standing on that
//    island or between islands, and a TELEPORT to another island loads it
//    first on the participant's own. A mix misbehaves only by missing
//    things: an older client finds none of the four on its list, and an
//    older owner never sends them.
//
// 62: the pickups a mission leaves the
//    world (docs/protocol.md 1.30). No opcode and no layout moves; what a
//    C_MissionEffect and a C_CampaignDelta op may say changes, the way 57
//    changed it for blips. A CREATE_PICKUP or CREATE_PICKUP_WITH_AMMO of a
//    type that comes back (in a shop, on the street, on the street slow)
//    whose result the mission keeps in a global goes with that global as its
//    output, `02 lo hi`, as MISSION_EFFECT_RUN and in the delta's ops: every
//    machine makes its own into its own global, takes off what that global
//    held first, and never takes it away when the mission ends. It is no
//    stash any more. A REMOVE_PICKUP on a pickup the mission did not lay out
//    through the map goes by its global the same way. So Cipriani's
//    Chauffeur's Uzi, Phil's guns and Phil's armour stay, the out-of-stock
//    Uzi goes everywhere, and a late joiner or a loaded save gets both. A
//    mix misbehaves one way round: an older participant drops a REMOVE_PICKUP
//    by global, so its out-of-stock sign stays beside the gun, and an older
//    machine applying a newer delta makes the pickup without taking the old
//    one off first.
//
// 63: what a mission may not do to its
//    helpers, and the Import/Export lists as the session's (docs/protocol.md
//    1.29, docs/missions.md 5.6, 6.1, 12.1). Two opcodes, the last of the
//    cheat block: C_CarLists (0xF6), a machine's own collected bits when it
//    has one the session lacks and once on every connection, and S_CarLists
//    (0xF7), the session's union, to the sender and to everybody when it
//    grew. What a C_MissionEffect may say changes, no layout moves:
//    STORE_WANTED_LEVEL (01C0) joins the replay list with its output a
//    global, which each participant fills with its own level, and
//    ALTER_WANTED_LEVEL's (010D) level may be `02 lo hi`, a global each
//    machine reads for its own, where it was always a literal. An ADD_SCORE
//    that takes money is no longer sent, and a participant drops one that
//    comes. Nothing new travels for the checkpoints: a mission with a
//    countdown on the screen, a race, an odd job or an RC, 4x4 or Mayhem run
//    no longer reports anybody missing from one. A mix misbehaves the old
//    way: an older server drops both car-list opcodes and every machine's
//    lists stay its own; an older participant never stores its level and
//    gets back whatever its global held, and pays a charge an older owner
//    sends; an older owner's checkpoints still wait.
//
// 64: the rest of what a mission shows,
//    sets for a while and counts (docs/protocol.md 1.29, 1.30). No layout
//    moves. PLAYER_MADE_PROGRESS and REGISTER_MISSION_PASSED are no longer
//    RUN live: they, and the three islands' PASSED instructions with the
//    radio's "island open", go in the campaign delta as world instructions
//    alone, which every machine applies once. SET_GET_OUT_OF_JAIL_FREE is a
//    world instruction like SET_PLAYER_NEVER_GETS_TIRED. The replay list
//    grows by PRINT_BIG_Q, PRINT_WITH_NUMBER_BIG_Q,
//    PRINT_WITH_2_NUMBERS_SOON, SET_PHONE_MESSAGE, the launch's
//    MAKE_PLAYER_SAFE_FOR_CUTSCENE, SET_FREE_RESPRAYS,
//    SET_WANTED_MULTIPLIER, both restart overrides, SWITCH_WORLD_PROCESSING,
//    SET_ALL_CARS_CAN_BE_DAMAGED, SET_GENERATE_CARS_AROUND_CAMERA,
//    SET_NEAR_CLIP, SET_MUSIC_DOES_FADE, CLEAR_AREA_OF_CHARS, SHAKE_CAM,
//    ADD_MOVING_PARTICLE_EFFECT, CREATE_SINGLE_PARTICLE, the end of the
//    game's tune, and ADD_CONTINUOUS_SOUND and REMOVE_SOUND, whose handle
//    goes as the global that holds it, each machine's own. A mix misbehaves
//    by missing or doubling stats: an older owner still RUNs the two stats
//    and never puts them in its delta, so a newer participant counts them
//    live only; a newer owner's delta reaches an older participant that
//    counted nothing live, once. An older client runs none of the new list.
//
// 65: a passenger's gun (docs/protocol.md
//    1.42). No opcode and no layout moves. A C_Shot from a player in a
//    passenger seat with the pistol or the uzi is now a real round, fired
//    through CWeapon::Fire from the seat with the car out of its line, and an
//    observer replays it from its copy's hand where it used to refuse any
//    seated round but weapon 19. A seated player's drive-by overlay (animId2
//    DRIVEBY_L/R) now holds the gun on the wire when it is one of those two,
//    the uzi otherwise. The server drops a C_Damage between two players in the
//    same car, friendly fire or not. A mix misbehaves only by missing things:
//    an older observer draws no passenger round and poses the uzi, and an
//    older server relays a hit inside one car that a newer shooter never
//    sends.
//
// 66: a player standing on something that
//    moves (docs/protocol.md 1.7.1). C_PlayerStateRide / S_PlayerStateRide
//    (0xAE/0xAF) are the ordinary snapshot with a PlayerRideBody after it:
//    the session car, boat or traffic car under the player's feet by netId,
//    or the El or subway wagon they stand on or sit in by track and wagon
//    id, and where on it they are in its own frame. Sent instead of
//    C_PlayerState while that is true, so the ride can never arrive apart
//    from the snapshot it belongs to. The observer rebuilds the position off
//    its own copy of that vehicle, which is where its screen draws it; the
//    world position in the body is still right and is what everything else
//    reads. No existing layout moves. A mix: an older server drops the new
//    opcode, so a rider stops being seen until they step off; an older
//    observer never gets one.
//
// 67: gates, street objects that stand up
//    again, and pickup reservations (docs/protocol.md 1.43). C_GateState /
//    S_GateState (0xC8/0xC9): one bit per scripted gate, "my own GATES thread
//    wants it open", OR'd like the garages and replayed to a joiner.
//    C_ObjectRebuilt (0xCD): a machine that rebuilds a street object it was
//    told was broken asks, and the server, which now keeps breaks and resting
//    places while a player is within 120 m, answers with S_ObjectBroken and
//    S_ObjectSettled to it alone; a joiner is handed the breaks. No layout
//    moves. S_PickupDenied can now arrive for a pickup the client holds a
//    grant on: its reservation stood 15 s and was moved to somebody else, and
//    the old holder's collection inside 2 s still counts (the new holder is
//    sent S_PickupTaken). A client claims only the nearest pickup in reach
//    and backs off 1.5 s after a denial. A mix misbehaves only as before: an
//    older client neither sends nor reads a gate mask, never asks about a
//    rebuilt object, and ignores a denial for a grant, keeping it as it
//    always did.
//
// 68: a session car the engine takes away on purpose (docs/protocol.md
//    1.44). Two opcodes, the garages' 0xAC and 0xAD: C_VehicleRemoved
//    (0xAC), from the one machine that may let its engine crush, crane,
//    deliver or store a session car - its driver, else its custodian, else
//    whoever drove it last, else the session host - once its engine has done
//    it, with the reason; and S_VehicleRemoved (0xAD) to everybody else, just
//    ahead of the S_VehicleDespawn that drops the car. A receiver deletes its
//    CVehicle even when its own engine made the car. The lists the delivery
//    adds to go the way 63 sends them. Two layouts grow by two bytes:
//    EnterVehicleBody and S_VehicleSpawn carry `parkedSlot`, the car
//    generator a claimed parked car came out of plus one (0 for none), and
//    every receiver of the spawn lets go of its own car on that generator the
//    way the engine does when a player takes one. A mix cannot connect.
//
// 69: a traffic car its host's engine drops
// by distance while another player is near it is handed on rather than
// despawned (docs/protocol.md 1.45). One opcode, 0xDC: C_CarLetGo, the car
// and the host's peds sitting in it. The server gives them to the nearest
// player within 195 m who has not let go of the car in the last 15 s, with an
// S_AmbientAdopt to everybody but the sender, and backfill spawns under the
// new owner to the sender; nobody near, the despawns. S_AmbientAdopt's first
// pad byte is `why` now, AMBIENT_ADOPT_LEFT (0, what every older server sent)
// or AMBIENT_ADOPT_LET_GO. No layout moves. A mix cannot connect; were one to,
// an older server would drop the let-go and the car and its driver would
// stand frozen on the other screens until the sender left.
//
// 70: a Pay'n'Spray and a bribe clear the
//    stars they should (docs/wanted.md 4.9, docs/protocol.md 1.16). No layout
//    moves and no opcode is added; what two packets mean grows. S_Respray
//    now clears the receiver's wanted level when the session's rule reaches
//    it - always under `shared`, and under `perplayer` when the receiver is
//    in the car the respray names - and S_PickupTaken for a PICKUP_F_BRIBE
//    takes one star off on the same terms, the car being the one the
//    collector is seated in. A client whose stars come down, by its own
//    engine or by one of those two, counts everybody who was reporting more
//    for no more than what it now has until they report less or 3 s pass.
//    A mix misbehaves the old way: an older receiver keeps its stars through
//    somebody else's respray or bribe, and after 3 s puts them back on the
//    newer machine that paid for it.
//
// 71: every server setting the host can
//    change reaches the players already in the session, and the mission
//    rules the clients used to hardcode are the server's (docs/protocol.md
//    1.46). One opcode, 0x07 out of the handshake block: S_SessionRules,
//    the welcome's SessionFlags byte again plus `maxWanted`, the most stars
//    anybody may have. It follows every S_Welcome, and goes to everybody
//    when the host saves a change; a rampage rule change waits for a running
//    rampage and its vote to end. S_MissionState grows by eight bytes:
//    `checkpointWaitS`, `catchUpM`, `behindM` and `behindS`, the four
//    numbers MISSION_CHECKPOINT_WAIT_MS, MISSION_SUMMON_NEAR_M,
//    MISSION_BEHIND_FAR_M and MISSION_BEHIND_MS used to be, and its flags
//    gain MISSION_FLAG_TIMED_CHECKPOINTS; it is broadcast when the host
//    changes any mission rule. What two existing packets mean grows, with no
//    layout change: S_Money can now arrive in the middle of a session with
//    a new rule, `off` included, which a receiver already handled by
//    starting its wallet over; S_CutsceneVote's and S_RampageVote's `needed`
//    follow the server's cutsceneSkip and rampageVote rules rather than
//    always being 75%, and S_RampageVote's `msLeft` its rampageVoteTime.
//    The server no longer relays a mission's MISSION_EFFECT_PAY with
//    missionPayHelpers off, so only the owner is paid. A mix cannot
//    connect; were one to, each side would drop the other's S_MissionState
//    for its size and never learn whose mission runs, and an older client
//    would keep every rule it was welcomed with until it reconnected.
//
// 72: a mission's move of its player says
//    whether that player sat in a car (docs/protocol.md, "Moving everybody
//    with the mission"). No layout moves and no opcode is added; a
//    MISSION_EFFECT_TELEPORT's `ownerBlip`, unused before, is 0 when the
//    owner's player was on foot and 1 + the netId of its car otherwise. A
//    participant in a car moves it only when the owner was in one and it
//    simulates the car (onto the owner's spot when the owner rides in it);
//    after an owner's move on foot it stays in its car within 60 m and gets
//    out onto the ring further away. A mix misbehaves the old way: an older
//    owner sends -1 or 0, which a newer participant reads as on foot, and an
//    older participant drives its car onto the ring whatever the owner did.

constexpr uint16_t PROTOCOL_VERSION = 72;
constexpr uint16_t DEFAULT_PORT     = 2001;
constexpr uint8_t  MAX_PLAYERS      = 8;
constexpr uint8_t  SNAPSHOT_HZ      = 25;   // docs/protocol.md §1.2
constexpr size_t   NICK_LEN         = 24;
// CBaseModelInfo::m_name, 24 bytes at +0x04 (client/src/game/addresses.h).
constexpr size_t   PLAYER_LOOK_LEN  = 24;
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
	// The session's rules, after the welcome and whenever the host changes
	// one (S_SessionRules). Server to client only.
	OP_S_SESSION_RULES   = 0x07,

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
	// A car's bomb: vehicle state like its shape, from whoever simulates the
	// car (docs/mission-audit.md R6). 0x3E-0x3F stay free for the rest of the
	// vehicle block.
	OP_C_VEHICLE_BOMB    = 0x3C,
	OP_S_VEHICLE_BOMB    = 0x3D,

	OP_S_WORLD_STATE     = 0x40,
	OP_C_WORLD_STATE     = 0x41,

	// The session's one mission. 0x42..0x4F is the block; 0x4E..0x4F stay
	// free for the rest of what the mission sends while it runs.
	OP_C_MISSION_CLAIM      = 0x42,
	OP_S_MISSION_CLAIM      = 0x43,
	OP_S_MISSION_WAITING    = 0x44,
	OP_C_MISSION_STARTED    = 0x45,
	OP_S_MISSION_STATE      = 0x46,
	OP_C_MISSION_ENDED      = 0x47,
	OP_S_MISSION_FAIL       = 0x48,
	OP_C_MISSION_CHECKPOINT = 0x49,
	// What the owner's mission shows, for every participant's engine to show
	// too: one instruction at a time (docs/missions.md 5.4).
	OP_C_MISSION_EFFECT     = 0x4A,
	OP_S_MISSION_EFFECT     = 0x4B,
	// The value behind a timer or a counter the owner's mission has on the
	// HUD, which every participant's HUD reads out of the same global.
	OP_C_MISSION_WIDGET     = 0x4C,
	OP_S_MISSION_WIDGET     = 0x4D,
	OP_C_MISSION_READY      = 0x4E,
	OP_S_MISSION_READY      = 0x4F,
	OP_C_MISSION_SEATS      = 0x52,
	OP_S_MISSION_SEATS      = 0x53,
	OP_C_MISSION_OBJECT_BREAK = 0x54,
	OP_S_MISSION_OBJECT_BREAK = 0x55,
	OP_C_MISSION_PICKUP     = 0x56,
	OP_S_MISSION_PICKUP     = 0x57,
	// A game in a mission of its own, and one that started over in the
	// middle of the session's. Out of the free 0xB7..0xBF, since 0x58..0x5F
	// is the vehicle custody block's.
	OP_C_MISSION_BUSY       = 0xB7,
	OP_S_MISSION_HAND_OVER  = 0xB8,
	// A kill a participant's machine registered, for the owner's mission to
	// count. Out of the free 0x98..0x9F, past the NPC block's 0x96/0x97.
	OP_C_MISSION_KILL       = 0x98,
	OP_S_MISSION_KILL       = 0x99,
	// What a participant's engine answers to the owner's mission's
	// questions: its garages, and the planes it flies.
	OP_C_MISSION_ANSWERS    = 0x9A,
	OP_S_MISSION_ANSWERS    = 0x9B,
	// The station a session car's radio is on (docs/radio.md). 0x9E and
	// 0x9F are the cutscene vote's.
	OP_C_VEHICLE_RADIO      = 0x9C,
	OP_S_VEHICLE_RADIO      = 0x9D,
	// Skipping a cutscene together, the last two of that block. See
	// CutsceneKey.
	OP_C_CUTSCENE_STATE     = 0x9E,
	OP_S_CUTSCENE_VOTE      = 0x9F,

	OP_C_CHAT            = 0x50,
	OP_S_CHAT            = 0x51,

	// Who simulates a car nobody is driving, and what happens when a player
	// gets into traffic somebody else's engine made. 0x58..0x5F is the block;
	// three of the eight are used and the rest stay free for the same
	// subject - see §1.14.
	OP_C_VEHICLE_SETTLED = 0x58,
	OP_S_VEHICLE_CUSTODY = 0x59,
	OP_S_CAR_PROMOTED    = 0x5A,
	// "I am getting into that car." A statement of intent, and nothing else:
	// it claims no car, takes no seat and moves no ownership. The claim that
	// does all three is still C_EnterVehicle (0x30) at the end of the
	// animation, exactly where §2.8.3 needs it.
	//
	// It exists because an observer cannot animate an entry it is told about
	// only once the entry is over, and because the door is not derivable from
	// the seat: the engine walks a driver to the *nearest* door and shuffles
	// him across inside. See EnteringVehicleBody and docs/protocol.md §1.14.7.
	//
	// 0x60/0x61 out of the block below. The note there
	// called 0x60..0x6F a block for hits travelling towards an owner; this
	// pair travels the other way, and it is here because it is about getting
	// into a car and the vehicle block (0x30..0x3F) has no room left.
	OP_C_ENTERING_VEHICLE = 0x60,
	OP_S_ENTERING_VEHICLE = 0x61,
	// 0x62..0x67 stay free.

	// A hit one machine's player landed on a pedestrian another machine hosts.
	// The one thing about an ambient ped that travels *towards* its owner -
	// see PedDamageBody.
	//
	// 0x60..0x6F is the block for that direction, and it is a new block on
	// purpose. 0x70..0x7D is full (the ped and car handshakes plus their two
	// streams) and 0xD8..0xDF was reserved for "a hosted ped reaching a state
	// only its host can witness", which is the opposite of this: a hit is
	// witnessed by the shooter and nobody else. Two of the sixteen are used
	// and the rest stay free for the same direction - a limb or a wreck an
	// observer causes and cannot apply.
	OP_C_PED_DAMAGE      = 0x68,
	OP_S_PED_DAMAGE      = 0x69,
	// And the same direction pointed at a car somebody else is driving: a hit
	// the shooter's engine landed on its replica, sent to the one machine
	// whose CVehicle::InflictDamage is allowed to decide what it costs.
	//
	// Named "hit" and not "damage" because C_VehicleDamage (0x3A) already
	// exists and is a different thing entirely: that one is absolute cosmetic
	// state - which panels are bent, which doors are gone - sent BY the driver
	// and merged as a maximum. This is a delta, sent TO the driver, and it is
	// never merged. Two packets called damage on the same object, travelling
	// in opposite directions with opposite arbitration, is a name nobody could
	// keep straight at three in the morning.
	OP_C_VEHICLE_HIT     = 0x6A,
	OP_S_VEHICLE_HIT     = 0x6B,
	// The same hit landed on a replica of somebody else's traffic car. It goes
	// to the machine hosting the car rather than to a driver, because traffic
	// has no driver the session knows about. Its own pair rather than a second
	// meaning for 0x6A: the server answers "who owns this" from a different
	// table and by a different rule. See C_CarHit.
	OP_C_CAR_HIT         = 0x6C,
	OP_S_CAR_HIT         = 0x6D,
	// 0x6E/0x6F stay free.

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

	// 0x88-0x8F is the rampage block, and all eight are used. See the
	// version history's entry 25 and the structs near the bottom
	// of this file. Nothing here is sent unless a frenzy is running, so a
	// session that never touches a KILLFRENZY pickup pays nothing for it.
	OP_C_RAMPAGE_START    = 0x88,
	OP_S_RAMPAGE_OPEN     = 0x89,
	OP_C_RAMPAGE_KILL     = 0x8A,
	OP_S_RAMPAGE_KILL     = 0x8B,
	OP_C_RAMPAGE_END      = 0x8C,
	OP_S_RAMPAGE_END      = 0x8D,
	// A car wreck that counted toward a vehicle rampage. Its own pair and
	// not a flag on 0x8A, because a car is counted differently - see
	// RampageCarBody.
	OP_C_RAMPAGE_CAR      = 0x8E,
	OP_S_RAMPAGE_CAR      = 0x8F,

	// An NPC's gunfire and the hits it lands on players, from the machine
	// hosting it. 0x90..0x97 is the block; six of the eight are used. The
	// shot is only drawn where it arrives, and a hit goes to the one player it
	// landed on, or on whose car. See C_NpcShot, C_NpcDamage, C_NpcVehicleHit.
	OP_C_NPC_SHOT         = 0x90,
	OP_S_NPC_SHOT         = 0x91,
	OP_C_NPC_DAMAGE       = 0x92,
	OP_S_NPC_DAMAGE       = 0x93,
	OP_C_NPC_VEHICLE_HIT  = 0x94,
	OP_S_NPC_VEHICLE_HIT  = 0x95,

	// 0xA0..0xAF was the garage block: doors, garages and the Pay'n'Spray.
	// It uses 0xA0..0xA3. docs/protocol.md §1.16.
	OP_C_GARAGE_STATE     = 0xA0,
	OP_S_GARAGE_STATE     = 0xA1,
	OP_C_RESPRAY          = 0xA2,
	OP_S_RESPRAY          = 0xA3,

	// 0xA4..0xAB is the police helicopter's, held for it out of the garage
	// block. All eight are used: the last pair is its gunfire. See the
	// version history's entry 32, the unnumbered entry after it, and
	// HeliStateBody.
	OP_C_HELI_STATE       = 0xA4,
	OP_S_HELI_STATE       = 0xA5,
	OP_C_HELI_GONE        = 0xA6,
	OP_S_HELI_GONE        = 0xA7,
	OP_C_HELI_HIT         = 0xA8,
	OP_S_HELI_HIT         = 0xA9,
	OP_C_HELI_SHOT        = 0xAA,
	OP_S_HELI_SHOT        = 0xAB,
	// A session car an engine took away on purpose: crushed, craned,
	// delivered or stored (docs/protocol.md 1.44).
	OP_C_VEHICLE_REMOVED  = 0xAC,
	OP_S_VEHICLE_REMOVED  = 0xAD,

	// The snapshot of a player standing on or riding in something that moves,
	// with where on it they are. PlayerRideBody.
	OP_C_PLAYER_STATE_RIDE = 0xAE,
	OP_S_PLAYER_STATE_RIDE = 0xAF,

	// Ammunition for an inventory slot the player is NOT currently holding.
	// The held weapon's count rides the snapshot instead - see
	// PlayerStateBody::ammoTotal for the argument.
	OP_C_PLAYER_AMMO      = 0xB0,
	OP_S_PLAYER_AMMO      = 0xB1,

	// Everybody's round trip to the server, from the server, once a second:
	// the ping in the player list. 0xB2..0xBF were free; this takes one.
	OP_S_PLAYER_PINGS     = 0xB2,
	// Where this machine has somebody else's player or car, and at what
	// instant of its owner's clock, and the server's answer: how far that is
	// from where the owner said it was at that instant. Diagnostics only;
	// nothing is corrected by it.
	OP_C_DESYNC_PROBE     = 0xB3,
	OP_S_DESYNC_REPORT    = 0xB4,
	// The server's password, right behind the hello, when the player has one
	// to give. A server with a password holds the hello until it arrives.
	OP_C_PASSWORD         = 0xB5,
	// The host throwing somebody out from inside the game. Taken from the
	// session's host and nobody else.
	OP_C_KICK             = 0xB6,
	// 0xB7 and 0xB8 are the session's mission's (C_MissionBusy above).
	// The lobby: a launcher's own connection, made before anybody's game is
	// running, to wait with the others and be started together.
	OP_C_LOBBY_JOIN       = 0xB9,
	OP_S_LOBBY_ANSWER     = 0xBA,
	OP_S_LOBBY            = 0xBB,
	OP_C_LOBBY_START      = 0xBC,
	OP_S_LOBBY_START      = 0xBD,
	// Somebody's menu is up. See C_PlayerAway. The last two of the B block.
	OP_C_PLAYER_AWAY      = 0xBE,
	OP_S_PLAYER_AWAY      = 0xBF,

	// 0xC0..0xCF is the breakable-street-object block. docs/objects.md.
	// Four of the sixteen are used here, 0xC4-0xC7 went to the rampage vote
	// and 0xCA/0xCB to the player's look below, and the rest stay reserved. 0xC0/0xC1 are
	// how broken it is; 0xC2/0xC3 are where it came to rest, which is the
	// half that used to be missing and the reason the other twelve were
	// held back rather than handed out.
	OP_C_OBJECT_BROKEN    = 0xC0,
	OP_S_OBJECT_BROKEN    = 0xC1,
	OP_C_OBJECT_SETTLED   = 0xC2,
	OP_S_OBJECT_SETTLED   = 0xC3,

	// The vote before a rampage, out of the middle of that block. 0xC4-0xC9
	// are held for it; four are used. See RampageVoteBody.
	OP_C_RAMPAGE_VOTE     = 0xC4,
	OP_S_RAMPAGE_VOTE     = 0xC5,
	OP_C_RAMPAGE_ARRIVED  = 0xC6,
	OP_S_RAMPAGE_TELEPORT = 0xC7,
	// The seven gates main.scm's GATES threads open, the two the vote left.
	// See GateMaskBody.
	OP_C_GATE_STATE       = 0xC8,
	OP_S_GATE_STATE       = 0xC9,

	// Claude's outfit, out of the top of that block. See C_PlayerLook.
	OP_C_PLAYER_LOOK      = 0xCA,
	OP_S_PLAYER_LOOK      = 0xCB,

	// A player left and somebody else hosts his crowd now. See S_AmbientAdopt.
	// 0xCC, out of the free end of that block (0xD0 went to the campaign
	// first). Server to client only: a machine
	// that cannot take a pedestrian it was given says so with the
	// C_PedDespawn / C_CarDespawn it already has, as the new owner.
	OP_S_AMBIENT_ADOPT    = 0xCC,
	// A street object this machine has just built again, which it was told
	// was broken: send me what you have. See C_ObjectRebuilt.
	OP_C_OBJECT_REBUILT   = 0xCD,
	// What a mission leaves behind in the campaign, and a way back to it after
	// a dropped connection (docs/missions.md 5.5). 0xD0..0xD7 is the block.
	OP_C_CAMPAIGN_DELTA   = 0xD0,
	OP_S_CAMPAIGN_DELTA   = 0xD1,
	OP_C_CAMPAIGN_SINCE   = 0xD2,

	// An ambient pedestrian dying. 0xD8-0xDF was the block reserved for it;
	// the carjack took 0xDA/0xDB, the mission's catch-up 0xDE/0xDF and a
	// traffic car's host letting go of it 0xDC, and 0xDD stays free.
	//
	// Not folded into the 0x7x ambient block, which is full: 0x70..0x7D are
	// the ped and car handshakes and their two streams, and squeezing a
	// death in between them would have renumbered the lot.
	OP_C_PED_DEATH        = 0xD8,
	OP_S_PED_DEATH        = 0xD9,
	// A carjack, as it starts. See C_JackingVehicle.
	OP_C_JACKING_VEHICLE  = 0xDA,
	OP_S_JACKING_VEHICLE  = 0xDB,
	// A traffic car its host's engine is taking away by distance while
	// somebody else is near it (C_CarLetGo). Client to server only; what comes
	// back is an S_AmbientAdopt or the despawns.
	OP_C_CAR_LET_GO       = 0xDC,
	// A participant who has just come into the session's running mission, late
	// or back from its own, asks for every pedestrian and car the mission has
	// (C_MissionCatchUp).
	OP_C_MISSION_CATCH_UP = 0xDE,
	OP_S_MISSION_CATCH_UP = 0xDF,

	// Money, behind the server's MoneyRule. 0xE0-0xE5 is the block; four of
	// the six are used. Nothing here is sent in a session with money off.
	// See MoneyRule and the unnumbered history entry above PROTOCOL_VERSION.
	OP_C_MONEY_CHANGE     = 0xE0,
	OP_S_MONEY            = 0xE1,
	OP_C_MONEY_AWARD      = 0xE2,
	OP_S_MONEY_AWARD      = 0xE3,
	// The other two: everybody into the car the mission puts its player in
	// (C_MissionBoard).
	OP_C_MISSION_BOARD    = 0xE4,
	OP_S_MISSION_BOARD    = 0xE5,

	// A mine the mission dropped has gone off (C_MineBlast). 0xEC-0xEF is the
	// block, and the other two are the bomb a mission fits to a car
	// (C_MissionBomb).
	OP_C_MINE_BLAST       = 0xEC,
	OP_S_MINE_BLAST       = 0xED,
	OP_C_MISSION_BOMB     = 0xEE,
	OP_S_MISSION_BOMB     = 0xEF,
	// A session car's own state that a snapshot has no room for. 0xE8-0xEB is
	// the block: its alarm, reliable, and the aim of its gun - the tank's
	// turret, the fire truck's water cannon - on the snapshot channel.
	OP_C_VEHICLE_ALARM    = 0xE8,
	OP_S_VEHICLE_ALARM    = 0xE9,
	OP_C_VEHICLE_AIM      = 0xEA,
	OP_S_VEHICLE_AIM      = 0xEB,

	// A cheat somebody typed that changes something their machine does not
	// own. 0xF0-0xF7 is the cheat block; cheats use two of the eight. What
	// travels is which cheat and what it left behind, never the keystrokes -
	// docs/cheats.md.
	OP_C_CHEAT            = 0xF0,
	OP_S_CHEAT            = 0xF1,

	// Emergency services, out of the rest of that block: a medic's revive and
	// a fire truck's hose (docs/protocol.md 1.37).
	OP_C_PED_REVIVE       = 0xF2,
	OP_S_PED_REVIVE       = 0xF3,
	OP_C_WATER_CANNON     = 0xF4,
	OP_S_WATER_CANNON     = 0xF5,
	// The Import/Export garages' lists and the emergency crane's, as the
	// session's campaign (docs/missions.md 6.1): the last two of the block.
	OP_C_CAR_LISTS        = 0xF6,
	OP_S_CAR_LISTS        = 0xF7,
	// A unique stunt jump's slow-motion shot, from the driver to the players
	// riding with him (docs/protocol.md §1.35).
	OP_C_STUNT_CAMERA     = 0xF8,
	OP_S_STUNT_CAMERA     = 0xF9,
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
	// The server has a password and the hello brought the wrong one, or none
	// within PASSWORD_WAIT_MS. An older client reads it as a reason it does
	// not know, which it is.
	REJECT_BAD_PASSWORD    = 3,
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

	// Bits 4 and 5: the server's RampageRule. Here for the same reason the
	// wanted rule is - it governs something that lives inside each client's
	// own CDarkel and never passes through the server, so the server's whole
	// part is saying which rule is in force before the first frenzy starts.
	//
	// Unlike the wanted rule there *is* a server-side half, and it is the
	// reason the rule is not simply a client setting: under `scaled` the
	// server is the one that multiplies the kill target, because it is the
	// only thing that knows how many players there are and the only way all
	// of them can arrive at the same number.
	SESSION_RAMPAGE_MASK  = 3 << 4,
	SESSION_RAMPAGE_SHIFT = 4,

	// Bits 6 and 7, the last two: the server's CheatRule. Here for the reason
	// the wanted rule is - a personal cheat never leaves the machine it was
	// typed on, so the only place `off` can be enforced for one is that
	// machine. The server enforces the world half as well, by not relaying.
	SESSION_CHEATS_MASK  = 3 << 6,
	SESSION_CHEATS_SHIFT = 6,
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

// How a rampage behaves in a session. docs/roadmap.md §5.10 decided the
// first of these and it is the default; the second exists because §5.10 also
// named the price of that decision out loud - "the difficulty is not
// rebalanced, so a four-player rampage is trivial" - and said the fix needed
// the script intercepted, i.e. M5. It does not. CDarkel::StartFrenzy
// (0x004210E0) takes the kill target as its third argument and has exactly
// two callers, both of them the script's own opcodes, so scaling the target
// is a detour on one function and no script work at all.
enum RampageRule : uint8_t {
	// One rampage, one kill count, everybody's kills. The target is what
	// rampage.sc asked for. This is §5.10 exactly, and the default.
	RAMPAGE_RULE_SHARED = 0,

	// The same, with the kill target multiplied by the number of players in
	// the session when the frenzy opens, clamped to RAMPAGE_MAX_KILLS. Four
	// players killing 20 Diablos between them in two minutes is not a
	// rampage; four players killing 80 is.
	RAMPAGE_RULE_SCALED = 1,

	// Kills are not shared at all: every machine counts only its own
	// player's, which is what the build did before this feature existed. Here
	// because it is the one setting that can be reached for when something
	// about the sharing goes wrong mid-session, and because it costs one
	// branch to keep honest.
	RAMPAGE_RULE_OFF    = 2,
};

// The engine holds the target in a uint16 argument and an int32 global, and
// `dec dword [008F1AB8h]` will happily run past anything. The clamp is
// CoopIII's, not the engine's, and it is here so a nine-player session and a
// target of 20 cannot multiply into something nobody can finish.
constexpr uint16_t RAMPAGE_MAX_KILLS = 1000;

inline uint8_t RampageRuleFromFlags(uint8_t flags) {
	const uint8_t rule =
	    static_cast<uint8_t>((flags & SESSION_RAMPAGE_MASK) >> SESSION_RAMPAGE_SHIFT);
	// 3 is not a rule, the same stance WantedRuleFromFlags takes on one.
	return rule > RAMPAGE_RULE_OFF ? RAMPAGE_RULE_SHARED : rule;
}

inline uint8_t FlagsWithRampageRule(uint8_t flags, uint8_t rule) {
	if (rule > RAMPAGE_RULE_OFF)
		rule = RAMPAGE_RULE_SHARED;
	flags = static_cast<uint8_t>(flags & ~SESSION_RAMPAGE_MASK);
	return static_cast<uint8_t>(flags | (rule << SESSION_RAMPAGE_SHIFT));
}

// The scaled target, as arithmetic and with no engine and no session around
// it, so tools/sessiontest can put the awkward cases through the same code
// the server runs. `players` is how many are in the session; zero and one
// both mean "nobody to share with" and leave the script's own number alone.
inline uint16_t ScaledRampageTarget(uint16_t asked, uint8_t rule, uint8_t players) {
	if (rule != RAMPAGE_RULE_SCALED || players < 2 || asked == 0)
		return asked;
	const uint32_t scaled = static_cast<uint32_t>(asked) * players;
	return scaled > RAMPAGE_MAX_KILLS ? RAMPAGE_MAX_KILLS
	                                  : static_cast<uint16_t>(scaled);
}

// The three values CDarkel::Status holds, read off the writes rather than off
// re3: StartFrenzy writes 1 at 0x0042110C, Update writes 2 at 0x00420819 and
// 3 at 0x004206B6, and rampage.sc compares $FRENZY_STATUS against all three.
// On the wire only the two endings ever travel.
enum RampageOutcome : uint8_t {
	RAMPAGE_PASSED = 2,
	RAMPAGE_FAILED = 3,
};

inline bool IsRampageOutcome(uint8_t v) {
	return v == RAMPAGE_PASSED || v == RAMPAGE_FAILED;
}

// ---- cheats (docs/cheats.md) ------------------------------------------------
//
// Every cheat retail 1.0 has, numbered in the order CPad::AddToPCCheatString
// (0x00492450) tests them. Twenty-three: re3's KANGAROO and PEDDEBUG are not
// in this build, and the pad-button cheats are not either - on PC
// CPad::DoCheats(int16) (0x00492F20) is an empty stub. The strings and the
// handler each one calls are in client/src/game/addresses.h, which is the
// wrong direction for this file to include; the client static_asserts its
// table against these numbers instead.
enum CheatId : uint8_t {
	CHEAT_WEAPONS           = 0,    // GUNSGUNSGUNS
	CHEAT_MONEY             = 1,    // IFIWEREARICHMAN
	CHEAT_HEALTH            = 2,    // GESUNDHEIT
	CHEAT_WANTED_UP         = 3,    // MOREPOLICEPLEASE
	CHEAT_WANTED_DOWN       = 4,    // NOPOLICEPLEASE
	CHEAT_TANK              = 5,    // GIVEUSATANK
	CHEAT_BLOW_UP_CARS      = 6,    // BANGBANGBANG
	CHEAT_CHANGE_PLAYER     = 7,    // ILIKEDRESSINGUP
	CHEAT_MAYHEM            = 8,    // ITSALLGOINGMAAAD
	CHEAT_EVERYBODY_ATTACKS = 9,    // NOBODYLIKESME
	CHEAT_WEAPONS_FOR_ALL   = 10,   // WEAPONSFORALL
	CHEAT_FAST_TIME         = 11,   // TIMEFLIESWHENYOU
	CHEAT_SLOW_TIME         = 12,   // BOOOOORING
	CHEAT_ARMOUR            = 13,   // TURTOISE (the 1.0 spelling)
	CHEAT_SUNNY             = 14,   // SKINCANCERFORME
	CHEAT_CLOUDY            = 15,   // ILIKESCOTLAND
	CHEAT_RAINY             = 16,   // ILOVESCOTLAND
	CHEAT_FOGGY             = 17,   // PEASOUP
	CHEAT_FAST_WEATHER      = 18,   // MADWEATHER
	CHEAT_WHEELS_ONLY       = 19,   // ANICESETOFWHEELS
	CHEAT_FLYING_CARS       = 20,   // CHITTYCHITTYBB
	CHEAT_STRONG_GRIP       = 21,   // CORNERSLIKEMAD
	CHEAT_NASTY_LIMBS       = 22,   // NASTYLIMBSCHEAT
	CHEAT_COUNT             = 23,
};

// What a session does with a cheat. Server-configurable, docs/roadmap.md
// §5.14; the default is the one single player behaves like, per §5.5.
enum CheatRule : uint8_t {
	// Every cheat works. One that changes the typing player's own state runs
	// where it was typed; one that changes the world runs on the machine that
	// owns what it changes - the host for the sky, every machine for the
	// clock's speed and the crowd's temper.
	CHEAT_RULE_SHARED   = 0,
	// Only the cheats about the player who typed them. The world ones are
	// refused, with a log line saying so.
	CHEAT_RULE_PERSONAL = 1,
	// No cheats in a session at all.
	CHEAT_RULE_OFF      = 2,
};

inline uint8_t CheatRuleFromFlags(uint8_t flags) {
	const uint8_t rule =
	    static_cast<uint8_t>((flags & SESSION_CHEATS_MASK) >> SESSION_CHEATS_SHIFT);
	// 3 is not a rule, the same stance WantedRuleFromFlags takes on one.
	return rule > CHEAT_RULE_OFF ? CHEAT_RULE_SHARED : rule;
}

inline uint8_t FlagsWithCheatRule(uint8_t flags, uint8_t rule) {
	if (rule > CHEAT_RULE_OFF)
		rule = CHEAT_RULE_SHARED;
	flags = static_cast<uint8_t>(flags & ~SESSION_CHEATS_MASK);
	return static_cast<uint8_t>(flags | (rule << SESSION_CHEATS_SHIFT));
}

// Does this cheat change anything beyond the player who typed it, on some
// machine other than theirs? The ten that do are the ones CHEAT_RULE_PERSONAL
// refuses. The classification is argued cheat by cheat in docs/cheats.md §3;
// in one line each:
//
//   BANGBANGBANG     wrecks every parked car and every car the typist hosts,
//                    and the wrecks travel
//   ITSALLGOINGMAAAD rewrites the whole CPedType threat table
//   WEAPONSFORALL    arms every pedestrian the population generator makes
//   TIMEFLIES/BOOORING  the speed of the simulation, and so of the clock
//   the four skies   CWeather::ForceWeatherNow, and the sky is the host's
//   MADWEATHER       CClock::Update ticks a minute every frame
//
// NOBODYLIKESME is deliberately not on the list. It ORs PED_FLAG_PLAYER1 into
// the table, and PLAYER1 is the local player on every machine, so the only
// pedestrians it turns are the typist's own, against the typist.
inline bool CheatChangesTheWorld(uint8_t id) {
	switch (id) {
	case CHEAT_BLOW_UP_CARS:
	case CHEAT_MAYHEM:
	case CHEAT_WEAPONS_FOR_ALL:
	case CHEAT_FAST_TIME:
	case CHEAT_SLOW_TIME:
	case CHEAT_SUNNY:
	case CHEAT_CLOUDY:
	case CHEAT_RAINY:
	case CHEAT_FOGGY:
	case CHEAT_FAST_WEATHER:
		return true;
	default:
		return false;
	}
}

inline bool CheatAllowed(uint8_t rule, uint8_t id) {
	if (id >= CHEAT_COUNT)
		return false;
	switch (rule) {
	case CHEAT_RULE_OFF:      return false;
	case CHEAT_RULE_PERSONAL: return !CheatChangesTheWorld(id);
	default:                  return true;
	}
}

// ---- money ------------------------------------------------------------------
//
// What a session does with the players' cash. Server-configurable and off by
// default. It has no bit in S_Welcome, whose flags byte is full since 33: the
// rule comes in S_Money, which the server sends straight after the welcome
// unless the rule is off. A server too old to send it leaves a client at off.
enum MoneyRule : uint8_t {
	// Every machine's cash is its own and nothing about it travels. What
	// every build before this did, including its mistakes: the fire timer
	// pays whoever watched a car burn out, and a police helicopter another
	// player shot down pays nobody.
	MONEY_RULE_OFF    = 0,
	// Every machine's cash is still its own, but an award goes to the player
	// who earned it, paid once. The machine that decides a wreck forwards
	// AwardMoneyForExplosion to the culprit's machine instead of paying
	// whoever it is, and the shooter of somebody else's helicopter gets the
	// $250 its owner's engine takes back.
	MONEY_RULE_OWN    = 1,
	// One wallet for the session. Awards go the way they do under `own`, and
	// every change to any machine's cash travels as a delta; the server keeps
	// the total and every machine writes it back.
	MONEY_RULE_SHARED = 2,
};

inline uint8_t SaneMoneyRule(uint8_t rule) {
	return rule > MONEY_RULE_SHARED ? uint8_t(MONEY_RULE_OFF) : rule;
}

// The car an award is for, when more than one machine decides that car: a
// parked one or a driverless session car (UnownedVehicleKind). The server
// delivers one award per key inside MONEY_AWARD_KEY_MS and drops the rest.
constexpr uint8_t  MONEY_AWARD_UNKEYED = 0xFF;
constexpr uint32_t MONEY_AWARD_KEY_MS  = 10000;

// One car's worth. AwardMoneyForExplosion pays nMonetaryValue * 0.002f, and
// the dearest car in handling.cfg is nowhere near this.
constexpr int32_t MONEY_AWARD_MAX_UNIT = 100000;

inline bool IsSaneMoneyAward(int32_t unit) {
	return unit > 0 && unit <= MONEY_AWARD_MAX_UNIT;
}

// Which of the engine's two callers made the award.
enum MoneyAwardKind : uint8_t {
	MONEY_AWARD_FIRE = 0,   // CAutomobile::ProcessControl's fire timer
	MONEY_AWARD_BOMB = 1,   // CVehicle::ProcessDelayedExplosion
};

// S_Money::flags.
enum MoneyFlags : uint8_t {
	// The pool has a total. It doesn't until the first player in says what
	// he has, and it goes back to empty when the last one leaves.
	MONEY_POOL_SEEDED = 1 << 0,
};

// The pool after a delta, kept inside what the engine can hold. Busted and
// wasted both clamp the player's cash at zero (addresses.h, BUSTED_FINES),
// so the pool does too.
inline int32_t AddToMoneyPool(int32_t total, int32_t delta) {
	const int64_t sum = static_cast<int64_t>(total) + delta;
	if (sum < 0)
		return 0;
	if (sum > INT32_MAX)
		return INT32_MAX;
	return static_cast<int32_t>(sum);
}

// Where a cheat runs, once it is allowed.
enum CheatRoute : uint8_t {
	// Where it was typed, and nowhere else. Either it is about the typist, or
	// what it changes already reaches everyone by the path that thing always
	// takes - a wreck, a model, a star.
	CHEAT_ROUTE_LOCAL    = 0,
	// On the host alone. The host's sky is the session's (§2.7) and reaches
	// everybody on the next S_WorldState, so the host is the one machine that
	// has to act on it. While the session's mission runs, "the host" here is
	// its owner, whose sky is the session's then (sky.h).
	CHEAT_ROUTE_HOST     = 1,
	// On every machine, the typist's first. Each machine's own engine owns its
	// own CTimer, CClock and CPedType, so nobody could apply it for anybody
	// else.
	CHEAT_ROUTE_EVERYONE = 2,
};

inline uint8_t CheatRouteOf(uint8_t id) {
	switch (id) {
	case CHEAT_SUNNY:
	case CHEAT_CLOUDY:
	case CHEAT_RAINY:
	case CHEAT_FOGGY:
		return CHEAT_ROUTE_HOST;
	case CHEAT_MAYHEM:
	case CHEAT_WEAPONS_FOR_ALL:
	case CHEAT_FAST_TIME:
	case CHEAT_SLOW_TIME:
	case CHEAT_FAST_WEATHER:
		return CHEAT_ROUTE_EVERYONE;
	default:
		return CHEAT_ROUTE_LOCAL;
	}
}

// The state byte says what the cheat left behind on the typist's machine,
// not what it did, so a receiver ends up in the same place whatever it had
// before and a late joiner can be handed it too. A toggle carried as "toggle"
// would invert on any machine that was already the other way.
//
//   MAYHEM            1. The handler only ever writes 0xFFFFF into the
//                     table and nothing in the engine writes it back.
//   WEAPONSFORALL     the resulting CPopulation::ms_bGivePedsWeapons, 0 or 1
//   MADWEATHER        the resulting gbFastTime, 0 or 1
//   TIMEFLIES/BOOORING  the resulting CTimer::ms_fTimeScale as 2^(state - 2),
//                     so 0..4 is 0.25..4. Those are the only values the two
//                     handlers can reach from 1.0: one doubles below 4, the
//                     other halves above 0.25.
//   the four skies    0. The id already says which.
constexpr uint8_t CHEAT_TIME_SCALE_STATE_MIN    = 0;   // 0.25
constexpr uint8_t CHEAT_TIME_SCALE_STATE_NORMAL = 2;   // 1.0
constexpr uint8_t CHEAT_TIME_SCALE_STATE_MAX    = 4;   // 4.0

inline bool IsValidCheatState(uint8_t id, uint8_t state) {
	switch (id) {
	case CHEAT_MAYHEM:
		return state == 1;
	case CHEAT_WEAPONS_FOR_ALL:
	case CHEAT_FAST_WEATHER:
		return state <= 1;
	case CHEAT_FAST_TIME:
	case CHEAT_SLOW_TIME:
		return state <= CHEAT_TIME_SCALE_STATE_MAX;
	case CHEAT_SUNNY:
	case CHEAT_CLOUDY:
	case CHEAT_RAINY:
	case CHEAT_FOGGY:
		return state == 0;
	default:
		return false;   // a local cheat has no business on the wire
	}
}

// What the server does with a C_Cheat. Pure, so tools/sessiontest puts every
// rule and every cheat through the same decision server.h makes.
enum CheatRelay : uint8_t {
	CHEAT_RELAY_DROP   = 0,
	CHEAT_RELAY_HOST   = 1,   // to the host alone
	CHEAT_RELAY_OTHERS = 2,   // to everybody but the typist, who already ran it
};

inline uint8_t CheatRelayFor(uint8_t rule, uint8_t id, uint8_t state,
                             bool senderIsHost, bool haveHost) {
	if (!CheatAllowed(rule, id) || !IsValidCheatState(id, state))
		return CHEAT_RELAY_DROP;
	switch (CheatRouteOf(id)) {
	case CHEAT_ROUTE_HOST:
		// The host runs its own sky cheats and never sends them. One that
		// arrives from the host anyway is a client that disagrees about who
		// the host is, and there is nobody else to hand it to.
		return (haveHost && !senderIsHost) ? CHEAT_RELAY_HOST : CHEAT_RELAY_DROP;
	case CHEAT_ROUTE_EVERYONE:
		return CHEAT_RELAY_OTHERS;
	default:
		return CHEAT_RELAY_DROP;
	}
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

// The session's rules, to a joiner straight after S_Welcome and to everybody
// whenever the host changes one in the server's options. `flags` is the same
// SessionFlags byte the welcome carries, so a receiver applies the two the
// same way; the welcome's copy is what a player has for the moment between
// the two packets. A change the server makes while a rampage is running waits
// for it to end before it is sent, so nobody's CDarkel changes rule halfway
// through a frenzy.
//
// `maxWanted` is the most stars anybody may have, 1 to WANTED_LEVEL_CEILING
// (the server's maxWantedLevel). Every machine clamps its own player to it,
// the way `off` clamps to 0; anything out of that range is the ceiling.
struct S_SessionRules {
	static constexpr uint8_t OPCODE = OP_S_SESSION_RULES;
	PacketHeader hdr;
	uint8_t      flags;       // SessionFlags
	uint8_t      maxWanted;
	uint8_t      pad[2];
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
	// The live announcement of somebody connecting this moment, as opposed to
	// a backfill telling a joiner who was already here. It is what lets a
	// client say "X joined" without saying it for everybody it is told about
	// on its own way in. An older server never sets it, so behind one nobody's
	// arrival is announced.
	PJF_ARRIVED = 1 << 2,
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
// Stock GTA III never triggers this at all: Claude is model 0 in every
// outfit, and the outfit travels on C_PlayerLook below. But that's a fact
// about the stock script data, not the engine. Nothing
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

// What model 0 is loaded as on the sender's machine, which is the one thing
// C_PlayerModel can't say. The intro dresses Claude in 'PLAYERP' and
// 8-Ball's mission puts him back in 'PLAYER', and both go through
// UNDRESS_CHAR, which renames model 0 in place rather than moving the ped to
// another index (addresses.h, MI_PLAYER).
//
// Reliable, change-only and on join, like the model. Lower case, NUL
// padded, never empty on the wire (CleanPlayerLook).
struct C_PlayerLook {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_LOOK;
	PacketHeader hdr;
	char look[PLAYER_LOOK_LEN];
};

struct S_PlayerLook {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_LOOK;
	PacketHeader hdr;
	uint8_t playerId;
	char    look[PLAYER_LOOK_LEN];
};

// The sender has the pause menu up. With the pause policy the world keeps
// running under the menu, so his ped stands in the street with its controls
// taken away and nothing on anybody else's screen says why. A level, not a
// count: sent reliably on every change, stored by the server and given to a
// joiner, and dropped with the player. Not a snapshot bit, because
// PlayerFlags has none left and a menu is opened a few times an hour.
struct C_PlayerAway {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_AWAY;
	PacketHeader hdr;
	uint8_t away;   // 1 while the menu is up, 0 once it is closed
};

struct S_PlayerAway {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_AWAY;
	PacketHeader hdr;
	uint8_t playerId;
	uint8_t away;
};

// Puts a look into the one shape both ends accept: lower case, [a-z0-9_]
// only, terminated, zero after the terminator. False, and the buffer
// zeroed, for anything else or for an empty name. The engine compares these
// names byte for byte (RequestSpecialModel's inlined strcmp), which is why
// the case is settled here and not left to the receiver.
inline bool CleanPlayerLook(char (&look)[PLAYER_LOOK_LEN]) {
	size_t n = 0;
	while (n < PLAYER_LOOK_LEN && look[n] != '\0')
		++n;
	bool ok = n > 0 && n < PLAYER_LOOK_LEN;
	for (size_t i = 0; ok && i < n; ++i) {
		char ch = look[i];
		if (ch >= 'A' && ch <= 'Z')
			ch = static_cast<char>(ch - 'A' + 'a');
		ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
		look[i] = ch;
	}
	for (size_t i = ok ? n : 0; i < PLAYER_LOOK_LEN; ++i)
		look[i] = '\0';
	return ok;
}

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

// S_SessionRules::maxWanted as a receiver takes it: 1 to the ceiling, and
// anything else the ceiling, so a zero from a zeroed packet never reads as
// "no stars at all" - that is the `off` rule's job, not this cap's.
inline uint8_t SaneMaxWanted(uint8_t level) {
	return level == 0 || level > WANTED_LEVEL_CEILING ? WANTED_LEVEL_CEILING : level;
}

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

	// World-space aim direction, radians. While aiming, both are the two
	// arguments the sender's CPed::AimGun last passed to
	// CPedIK::PointGunInDirection, and the observer hands the pitch back to
	// that same function (client/src/game/pedaim.h). Pitch is the engine's
	// convention: positive is down. Before that it was m_fLookDirection and
	// m_torsoOrient.pitch, which an old client still sends - same units, so it
	// interoperates, and a pistol's pitch reads as level from it.
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

// What a player is standing on, when it moves (docs/protocol.md 1.7.1).
//
// A snapshot's position is a world position, and the observer draws it 100 ms
// and a round trip after it was true. For somebody on foot that is the whole
// point of the delay. For somebody on the roof of a moving bus it is a player
// left `speed * (100 ms + latency)` behind the bus they are standing on: the
// bus is drawn on its own timeline, the player on theirs, and nothing ties the
// two together. So while they ride, the owner also says what they ride and
// where on it, and the observer puts them there on its own copy.
//
// The owner reads it off the engine rather than working it out:
// CPed::m_pCurrentPhysSurface (+0x2FC) is the vehicle under a standing ped's
// feet, and a ped riding a train is bInVehicle with m_pMyVehicle on the wagon.
enum RideKind : uint8_t {
	RIDE_NONE    = 0,
	RIDE_VEHICLE = 1,   // `id` is the session's netId: a session car or boat, or traffic
	RIDE_TRAIN   = 2,   // `track` is CTrain::m_nTrackId, `id` CTrain::m_nWagonId
};

struct PlayerRideBody {
	uint8_t  kind;      // RideKind
	uint8_t  track;     // RIDE_TRAIN only: 0 the El, 1 the subway
	uint16_t id;
	Vec3     offset;    // position in the vehicle's own frame: right, forward, up
	float    heading;   // CPed::m_fRotationCur less the vehicle's heading
};

struct C_PlayerStateRide {
	static constexpr uint8_t OPCODE = OP_C_PLAYER_STATE_RIDE;
	PacketHeader    hdr;
	PlayerStateBody body;
	PlayerRideBody  ride;
};

struct S_PlayerStateRide {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_STATE_RIDE;
	PacketHeader    hdr;
	uint8_t         playerId;
	PlayerStateBody body;
	PlayerRideBody  ride;
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

	// The horn is sounding: CVehicle::m_nCarHornTimer is non-zero on the car
	// the sender is driving, held for one snapshot past the last one that saw
	// it (client/src/game/horn.h, HornOnWire). A state, not an event - it
	// means "honking now" and nothing about how long.
	//
	// A snapshot is the only place it means anything. The server keeps the
	// last snapshot's flags and replays them in S_VehicleSpawn, so a spawn can
	// carry this bit, and a receiver must not honk off it.
	VEH_HORN = 1 << 4,

	// The taxi light is on: bit 3 of CAutomobile's flags byte at +0x4D9, which
	// only the script turns on (SET_TAXI_LIGHTS, the taxi side job) and which
	// PreRender draws as the corona on the roof. A state like the siren, so it
	// rides the spawn as well and a joiner sees it.
	VEH_TAXI_LIGHT = 1 << 5,

	// The handbrake is on: CVehicle::bIsHandbrakeOn, bit 5 of +0x1F5, which
	// the driver's own ProcessControlInputs takes from his pad. It locks the
	// rear wheels, which is the skid, the smoke and the squeal of a handbrake
	// turn. Only a snapshot means anything by it.
	VEH_HANDBRAKE = 1 << 6,
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
	// Fists or the bat: MELEE_* below, 0 for anything else. What the owner
	// needs to play the struck ped's half of the fight code.
	uint8_t  melee;
	uint8_t  hitLevel;
};

// A punch, a kick or a bat, on DamageBody and PedDamageBody.
//
// The damage was never the whole of a melee hit. The fight code makes the
// victim defend, knocks him over and shoves him, all on the struck ped and
// none of it inside InflictDamage. So the attacker's machine used to do that
// to its copy of the victim, and the victim's own engine never did it at all.
// These two bytes are what the owner needs to do it on the real one
// (client/src/game/melee.h).
//
//   low two bits       which engine path landed it
//     MELEE_STRIKE       CPed::FightStrike: fists, knees, kicks, headbutts
//     MELEE_SWING        CWeapon::FireMelee: the bat
//   MELEE_ARMED        strike: the striker had a weapon in his hand
//   MELEE_GROUND_KICK  strike: the move was the kick at somebody on the floor
//   MELEE_HEAVY        swing: the weapon's second animation was playing
//
// hitLevel is the strike's move's own, out of the engine's fight move table.
// A swing's depends only on the victim, so it is the victim's to work out and
// travels as 0.
constexpr uint8_t MELEE_NONE        = 0;
constexpr uint8_t MELEE_STRIKE      = 1;
constexpr uint8_t MELEE_SWING       = 2;
constexpr uint8_t MELEE_KIND_MASK   = 0x03;
constexpr uint8_t MELEE_ARMED       = 0x04;
constexpr uint8_t MELEE_GROUND_KICK = 0x08;
constexpr uint8_t MELEE_HEAVY       = 0x10;
constexpr uint8_t MELEE_KNOWN_BITS  = 0x1F;
constexpr uint8_t MELEE_HIT_LEVELS  = 5;   // HITLEVEL_NULL .. HITLEVEL_HIGH

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

	// The car generator a parked car came out of, plus one; 0 for any other
	// car. Only a claim fills it in (netId INVALID_NETID). Every machine has
	// its own car standing on that generator, so every receiver of the spawn
	// has to let go of its own or the parked car stays on its screen beside
	// the one being driven away (docs/protocol.md 1.44). Plus one so that a
	// zeroed body means "not a parked car" and never generator 0.
	uint16_t parkedSlot;
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

// "I am getting into that car, through that door." Sent as the entry starts,
// carried by nothing else, and deliberately weaker than the claim above.
//
// It says only what the other machines have to know to animate the same entry
// at the same time, and it decides nothing: the server relays it and does not
// write it down, and a receiver that acts on it must be able to take it all
// back. An entry can be abandoned - shot, interrupted, given up on - and the
// only packet that ever confirms it is the C_EnterVehicle at the end.
//
// **`door` is not derivable from `seat`, which is the whole reason this
// packet carries two bytes instead of one.** A driver's entry seeks the
// nearest door, not the driver's door: `CPed::SeekCar` (`0x004D3F90`) sends
// anything with OBJECTIVE_ENTER_CAR_AS_DRIVER through `CPed::GetNearestDoor`
// (`0x004E1CF0`), which writes whichever of the four is closest into
// `m_vehDoor`. Walk up on the passenger side and press the enter key and the
// engine opens the near door, puts you in through it and shuffles you across
// to the wheel - `CPed::PedAnimDoorCloseCB` has that third arm, and it is
// reached whenever the door is not the front-left one and the objective is
// the driver's.
//
// An observer told only "seat 0" opens the driver's door instead, and
// `CPed::EnterCar`'s line-up drags the replica round to it. That is the
// teleport this pair exists to remove.
struct EnteringVehicleBody {
	uint16_t netId;   // a car the session already names; never INVALID_NETID
	uint8_t  seat;    // where the entry ENDS. 0 is the driver.
	// Which door it goes in THROUGH, named as the seat that door belongs to:
	// 0 front-left, 1 front-right, 2 rear-left, 3 rear-right. Equal to `seat`
	// for an ordinary entry. Named this way rather than as the engine's own
	// door constants because the seat-to-door mapping already exists on the
	// receiving side (game/ped.cpp, DoorForSeat) and is the one the engine's
	// own exit path is read backwards from, so the two cannot drift apart.
	uint8_t  door;
};

struct C_EnteringVehicle {
	static constexpr uint8_t OPCODE = OP_C_ENTERING_VEHICLE;
	PacketHeader hdr;
	EnteringVehicleBody body;
};

struct S_EnteringVehicle {
	static constexpr uint8_t OPCODE = OP_S_ENTERING_VEHICLE;
	PacketHeader hdr;
	uint8_t             playerId;
	EnteringVehicleBody body;
};

// "I have started pulling somebody out of that car, through that door." The
// same three bytes as the intent above, sent in its place when the sender's
// engine is in PED_CARJACK (or playing the quick jack), and just as weak: it
// decides nothing, the server writes nothing down, and the claim at the end is
// still what moves the seat.
//
// What it adds is that every other machine plays the jack instead of waiting
// for the claim. Each one runs CPed::SetCarJack_AllClear on its copy of the
// jacker, and its own engine then drags out whoever is in that seat THERE -
// the victim's own machine drags the victim's real ped, a traffic driver's
// host drags its real driver, and everybody else drags their replica. Nobody
// moves a ped that is not theirs to move.
//
// Unlike the intent it may name a traffic car (an AmbientCar netId) as well as
// a session car: a jack is how a player takes a traffic car, and the promotion
// only happens at the claim.
//
// `seat` is always 0. A jack is only ever made for the wheel
// (PedAnimPullPedOutCB quits anything without OBJECTIVE_ENTER_CAR_AS_DRIVER).
struct C_JackingVehicle {
	static constexpr uint8_t OPCODE = OP_C_JACKING_VEHICLE;
	PacketHeader hdr;
	EnteringVehicleBody body;
};

struct S_JackingVehicle {
	static constexpr uint8_t OPCODE = OP_S_JACKING_VEHICLE;
	PacketHeader hdr;
	uint8_t             playerId;
	EnteringVehicleBody body;
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
	// EnterVehicleBody::parkedSlot from the claim, for a claim and for the
	// backfill alike: a joiner has a car on that generator too.
	uint16_t parkedSlot;
};

struct S_VehicleDespawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DESPAWN;
	PacketHeader hdr;
	uint16_t netId;
};

// ---- a session car an engine took away on purpose (CH_EVENT) ---------------
//
// docs/protocol.md 1.44. The engine deletes cars by itself for reasons that
// are the player's doing and not an accident: the crusher, the military
// crane at the Portland docks, Craig's import/export garages, the police and
// bank-van garage, a mission garage, and a safehouse garage storing what is
// parked in it. Each of those runs on every machine, on that machine's own
// copy, and each pays that machine's player - so a car driven into the
// crusher used to pay everybody standing near it, and then came back, because
// a session car that leaves the pool without being a wreck is rebuilt.
//
// Now one machine may let its engine do it: the one that holds the car (the
// driver, else the custodian, else whoever drove it last, else the session
// host). Every other machine's crusher and crane cannot see the car and its
// garages leave it alone. The holder's engine does what it does, pays its
// own player through its own code, and this packet ends the car everywhere
// else. The server then sends S_VehicleDespawn like any release. What the
// delivery added to Craig's lists or the crane's goes as every change to
// them does (C_CarLists).
enum VehicleRemovedReason : uint8_t {
	VEHICLE_REMOVED_NONE     = 0,   // never sent
	VEHICLE_REMOVED_CRUSHED  = 1,   // GARAGE_CRUSHER
	VEHICLE_REMOVED_CRANE    = 2,   // the military crane (CCrane::Update)
	VEHICLE_REMOVED_EXPORTED = 3,   // Craig: GARAGE_COLLECTCARS_1..3
	VEHICLE_REMOVED_COLLECTED= 4,   // GARAGE_COLLECTSPECIFICCARS
	VEHICLE_REMOVED_MISSION  = 5,   // GARAGE_MISSION, a mission's delivery
	VEHICLE_REMOVED_STORED   = 6,   // a safehouse garage stored it
	VEHICLE_REMOVED_COUNT
};

struct VehicleRemovedBody {
	uint16_t netId;
	uint8_t  reason;   // VehicleRemovedReason
	uint8_t  pad;
};

struct C_VehicleRemoved {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_REMOVED;
	PacketHeader       hdr;
	VehicleRemovedBody body;
};

struct S_VehicleRemoved {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_REMOVED;
	PacketHeader       hdr;
	uint8_t            playerId;   // whose engine did it
	VehicleRemovedBody body;
};

// ---------------------------------------------------------------------------
// Who simulates a car nobody is driving (docs/protocol.md §1.20)
// ---------------------------------------------------------------------------
//
// Protocol 22 settled who owns a car with somebody in it: the player in seat
// 0, named by the server, and nobody else may report it. It said nothing
// about the gap between an exit and the next enter, and the answer it left
// there was "nobody" - every machine pins the car at the last transform the
// session gave it, for ever.
//
// Pinning is the right answer for a car standing on the street and the wrong
// one for a car that was still falling when its driver stepped out of it. The
// pin is applied after physics, every frame, so whatever pose the car was in
// at that instant is permanent - including reared up against a wall with its
// wheels off the ground, which CVehicle::CanPedEnterCar then refuses for the
// rest of the session while CPed::SeekCar walks the player at the door with
// no timeout to end it.
//
// So a driverless car gets a **custodian**: one machine, named by the server,
// that stops correcting the car and lets its own engine finish whatever the
// car was doing, streaming the result on the C_VehicleState the protocol
// already has. When the car comes to rest the custodian says so and the
// session goes back to having nobody simulate it - which is the pinned,
// zero-bandwidth, perfectly still behaviour a parked car has always had.
//
// Custody is granted at the exit and to the player who was driving, and the
// two reasons are the same reason: that machine has the car streamed in with
// the collision loaded around it, because it was driving it a frame ago. The
// session host is the wrong answer here even though §5.8 is right that an
// ownerless world entity is the host's - GTA III streams around one player
// (roadmap §2.1) and keeps one island's collision in memory (§2.2), so a host
// three streets away would be simulating a car with no ground under it and
// reporting the fall.
//
// Nothing a client decides for itself: a machine is the custodian when, and
// only when, it has been told so by this packet, and Session::MayReportVehicle
// drops a C_VehicleState from anyone else exactly as it does for a driver. Two
// machines can no more both be the custodian than both be the driver.
//
// While it holds custody the custodian also owns the car's condition, as a
// driver does: hits on it go to the custodian on S_VehicleHit, and only the
// custodian may report it wrecked (as UNOWNED_SESSION, since nobody drives it).
//
// It is also granted to whoever shoots a session car nobody holds, and sent
// just ahead of their own hit coming back (Session::CustodyForHit), and that
// includes the thrower of a blast that left the car standing, whose hit is the
// blast's own cause and is applied nowhere. And a custodian keeps a burning
// car until it goes up rather than until it stops: its fire timer is the only
// one running, so it is the one BlowUpCar - and if the timer has not gone off
// in CUSTODY_BURN_CAP_MS the custodian blows the car up itself.
//
// And a wreck has one: whoever decided it, told right after the blast
// (Session::DestroyVehicle). Its snapshots carry VEH_WRECKED and a transform,
// every other machine follows them, and the settle ends as any other does.
// A car nobody holds at all has its fire timer counted by the host alone
// (client/src/game/wreck.h).
struct S_VehicleCustody {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_CUSTODY;
	PacketHeader hdr;
	uint16_t netId;
	// Who simulates it now. INVALID_PLAYER means nobody does, which is an
	// instruction to pin it and not a gap in the record.
	uint8_t  playerId;
	uint8_t  pad;
};

// The custodian's own report that it is finished: the car has come to rest,
// or the settle ran past VEHICLE_SETTLE_MS and is being given up on.
//
// Reliable, because it is the end of an ownership and not a sample. A lost
// one would leave the session believing a car is being simulated that nobody
// is streaming - which degrades to exactly the behaviour this replaces
// (everybody holds the last transform), so it is safe rather than silent, but
// it is still the kind of fact that belongs on the ordered channel.
struct C_VehicleSettled {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_SETTLED;
	PacketHeader hdr;
	uint16_t netId;
};

// How long a custodian is allowed to keep a car before it gives up and hands
// it back to the pinned world.
//
// Bounded because a car that has not settled in two seconds is not settling:
// it is balanced on a kerb oscillating, or its custodian has walked out of the
// streamer's range and is simulating something that is no longer really
// there. Handing it back means everybody pins it at the last transform the
// custodian reported, which is the behaviour that existed before any of this.
//
// Held on the client rather than the server on purpose. The custodian is the
// machine with the frame clock and the car in front of it, and the failure
// mode of a custodian that never reports - an old build, a hung process - is
// that every observer holds the last transform, i.e. exactly the behaviour
// this replaces. A server timer would buy nothing that failure does not
// already give for free.
constexpr uint32_t VEHICLE_SETTLE_MS = 2000;

// How many consecutive quiet samples a car needs before its custodian calls
// it rest.
//
// The number is the engine's. CPhysical::ProcessControl (0x00495F10) counts
// quiet frames in m_nStaticFrames (+0xED), `inc / cmp 0Ah / jbe`, and on the
// frame that takes the counter past ten it sets bIsStatic, zeroes
// m_vecMoveSpeed and m_vecTurnSpeed outright and returns without applying
// either - and any frame that fails the quiet test puts the counter back to
// zero. So eleven is where GTA III itself stops believing a thing is moving,
// and that a run can be broken by one frame is the part worth copying: a car
// at the top of a bounce reads still for exactly one frame, and a
// single-sample test would hand it back in mid-air.
//
// **Counted on the snapshot tick and not per frame**, because that is where
// the custodian reads the car anyway. At 25 Hz that makes the window about
// 440 ms rather than the engine's 180, i.e. strictly longer, which is the
// conservative direction: a custodian that gives a car back too early leaves
// it pinned wherever it happened to be, which is the bug this exists to
// remove. Well inside VEHICLE_SETTLE_MS either way.
constexpr uint8_t VEHICLE_REST_FRAMES = 11;

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
	// replica's local wreck would be this machine's own opinion about
	// somebody else's car. Replicas don't get to have one any more: they
	// refuse damage and BlowUpCar outright and hold the health the host
	// streams (see C_CarHit), so this report is the only way one ends.
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
// the driver of that car, or with nobody driving it the player settling it
// (S_VehicleCustody), exactly as for C_VehicleState - so by the time this goes
// out the field is for the log.
struct S_VehicleDamage {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DAMAGE;
	PacketHeader      hdr;
	uint8_t           playerId;
	uint8_t           pad[3];
	VehicleDamageBody body;
};

// A car's bomb (docs/mission-audit.md R6): CAutomobile's bomb type, which a
// bomb shop fits on the machine whose player drove in, the mission fits with
// ARM_CAR_WITH_BOMB, and the car's own controls set ticking. The machine that
// simulates the car says when it changes, from the same seat C_VehicleDamage
// comes from: the driver, or with nobody driving it the player settling it.
// So IS_CAR_ARMED_WITH_BOMB reads the same on every copy, and the car goes up
// on that machine, whose blow-up already travels. On change only.
//
// The bomb is also whose it is and whether it is burning. `blame` is the
// player the car's copy names: the one who pressed the button while the fuse
// burns (m_pBlowUpEntity), and otherwise the one who had it fitted
// (m_pBombRigger). Every copy writes that player's ped in both, which is what
// lets the ignition blame the rigger wherever the car is driven, lets the
// rigger's detonator (a C_Shot with WEAPONTYPE_DETONATOR, relayed as it always
// was) find the car on the machine that simulates it, and credits the wreck
// to the bomber on every screen. `fuseMs` is what was left of a lit fuse when
// it was said, 0 when none is lit: every copy lights its own with it, so the
// car ticks everywhere and still goes up when whoever simulates it changes
// before the end.
constexpr uint8_t CARBOMB_NONE             = 0;
constexpr uint8_t CARBOMB_TIMED            = 1;
constexpr uint8_t CARBOMB_ONIGNITION       = 2;
constexpr uint8_t CARBOMB_REMOTE           = 3;
constexpr uint8_t CARBOMB_TIMEDACTIVE      = 4;
constexpr uint8_t CARBOMB_ONIGNITIONACTIVE = 5;
constexpr uint8_t CARBOMB_MAX = 5;   // 0 none .. 5 on the ignition and ticking
// The longest fuse the engine lights: the timed bomb's 7000 ms.
constexpr uint16_t CARBOMB_FUSE_MAX_MS = 7000;

struct C_VehicleBomb {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_BOMB;
	PacketHeader hdr;
	uint16_t     netId;
	uint8_t      bombType;   // CARBOMB_*, 0..CARBOMB_MAX
	uint8_t      blame;      // a player id, INVALID_PLAYER for nobody
	uint16_t     fuseMs;     // 0..CARBOMB_FUSE_MAX_MS
	uint16_t     pad;
};

// Relayed to everyone except the reporter, once the server has checked it is
// the car's driver or settler, as for C_VehicleDamage. The server keeps the
// last one for every car and hands it to a joiner after the car, from
// INVALID_PLAYER, with what is left of the fuse by its own clock.
struct S_VehicleBomb {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_BOMB;
	PacketHeader hdr;
	uint8_t      playerId;   // who said so
	uint8_t      bombType;
	uint16_t     netId;
	uint8_t      blame;
	uint8_t      pad;
	uint16_t     fuseMs;
};

// A bomb the mission's own script fitted, ARM_CAR_WITH_BOMB (docs/protocol.md
// 1.36). The handler writes FindPlayerPed() as the car's rigger, and it is
// replayed, so every participant's copy named its own player and the bomb
// became whoever first held the car. It is the mission owner's: the owner's
// machine says so when its script runs it, for a car the session names, and
// every participant's own replay names the owner rather than itself.
struct C_MissionBomb {
	static constexpr uint8_t OPCODE = OP_C_MISSION_BOMB;
	PacketHeader hdr;
	uint16_t     netId;
	uint8_t      bombType;   // CARBOMB_*, 0..CARBOMB_MAX
	uint8_t      pad;
};

// Relayed to everybody but the owner, once the server has checked the sender
// is the one whose mission may show things (MissionSlot::MayRelayEffect).
// The car's bomb is `bombType` and `playerId`'s, on every copy, the one its
// receiver simulates included: the script's word is not the holder's to keep
// or refuse. The server keeps it with the car's bomb and hands it to a joiner
// after the S_VehicleBomb it already sends.
struct S_MissionBomb {
	static constexpr uint8_t OPCODE = OP_S_MISSION_BOMB;
	PacketHeader hdr;
	uint8_t      playerId;   // the mission's owner, whose bomb it is
	uint8_t      bombType;
	uint16_t     netId;
};

// Whether a car's bomb is still the one the mission fitted after a word that
// says `type` and `blame` about it: the same bomb and the same player. Its
// timer set going, the detonator taking it off, a bomb shop fitting another
// or the owner leaving make it a bomb like any other.
inline bool KeepsMissionBomb(bool mission, uint8_t type, uint8_t blame, uint8_t newType,
                             uint8_t newBlame) {
	return mission && newType == type && newBlame == blame && newType != CARBOMB_NONE;
}

// Whether `presser`'s detonator sets off a car's bomb. Their own remote bomb,
// as CWorld::UseDetonator matches it; and a remote bomb the running mission
// fitted, for anybody in that mission: GIVE_WEAPON_TO_PLAYER is replayed, so
// every participant holds the mission's detonator, and the bomb still goes
// off blamed on its rigger, the owner, as UseDetonator blames the rigger and
// not the presser.
inline bool DetonatorSetsOff(uint8_t bombType, uint8_t blame, bool missionBomb, uint8_t presser,
                             bool missionRunning, uint8_t missionOwner, uint8_t participants) {
	if (bombType != CARBOMB_REMOTE || presser >= MAX_PLAYERS || blame >= MAX_PLAYERS)
		return false;
	if (blame == presser)
		return true;
	if (!missionBomb || !missionRunning || blame != missionOwner)
		return false;
	return presser == missionOwner || (participants & (1u << presser)) != 0;
}

// A mine went off (docs/protocol.md 1.36). DROP_MINE and DROP_NAUTICAL_MINE
// are replayed, so every participant's engine lays its own mine at the
// owner's spot, and each one arms and goes off by what its own engine sees:
// a car over it, or its ten seconds running out. The first machine whose mine
// goes off says where, and everybody else takes theirs out of the world and
// sets off the engine's own explosion there. A mine is nobody's, like a
// parked car, so there is nobody to wait for.
struct C_MineBlast {
	static constexpr uint8_t OPCODE = OP_C_MINE_BLAST;
	PacketHeader hdr;
	Vec3         pos;   // the mine object's position when it went off
};

// Relayed to everyone except the sender.
struct S_MineBlast {
	static constexpr uint8_t OPCODE = OP_S_MINE_BLAST;
	PacketHeader hdr;
	uint8_t      playerId;
	uint8_t      pad[3];
	Vec3         pos;
};

// A place a mine could be: numbers, and inside the map with room to spare.
// Liberty City's streets are within 2000 m of the origin either way.
constexpr float MINE_BLAST_MAX_XY = 4000.0f;
constexpr float MINE_BLAST_MIN_Z  = -200.0f;
constexpr float MINE_BLAST_MAX_Z  = 1000.0f;

inline bool MineBlastPlaceSane(const Vec3 &p) {
	return p.x == p.x && p.y == p.y && p.z == p.z && p.x > -MINE_BLAST_MAX_XY &&
	       p.x < MINE_BLAST_MAX_XY && p.y > -MINE_BLAST_MAX_XY && p.y < MINE_BLAST_MAX_XY &&
	       p.z > MINE_BLAST_MIN_Z && p.z < MINE_BLAST_MAX_Z;
}

// ---- the car radio (docs/radio.md, docs/protocol.md 1.33) -----------------
//
// CVehicle::m_nRadioStation (+0x229) is what cMusicManager plays in a car the
// local player sits in, and every machine rolls its own: the constructor
// picks GetRandomNumber() % 10, and a driver who is not the player picks from
// his model's radio category when he gets in (CPed::SetRadioStation). So two
// players in one car heard two stations. The session keeps one per car and
// every copy carries it.
//
// The values are the engine's eRadioStation: 0..8 the nine stations, 9 the
// user-track player, 10 the police scanner (which a police car plays whatever
// the byte says), 11 off.
constexpr uint8_t RADIO_STATION_USERTRACK = 9;
constexpr uint8_t RADIO_STATION_POLICE    = 10;
constexpr uint8_t RADIO_STATION_OFF       = 11;
// Not a station: the session has not been told one for this car.
constexpr uint8_t RADIO_STATION_UNKNOWN   = 0xFF;

constexpr bool RadioStationValid(uint8_t station) { return station <= RADIO_STATION_OFF; }

// From a player in that car, driver or passenger: his listener put it on
// `station`, or - from the driver only - the session had no station for it
// and this is the one his copy already had. On change only.
struct C_VehicleRadio {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_RADIO;
	PacketHeader hdr;
	uint16_t     netId;
	uint8_t      station;   // 0..RADIO_STATION_OFF
	uint8_t      pad;
};

// The session's station for a car. To everybody, the sender included, so
// that two players turning the dial in the same instant end on whichever the
// server took last on every machine; and to a sender the server refused, with
// the station it still holds, so that his copy goes back. `playerId` is who
// changed it, INVALID_PLAYER for the session's own record (the backfill, a
// refusal).
struct S_VehicleRadio {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_RADIO;
	PacketHeader hdr;
	uint8_t      playerId;
	uint8_t      station;
	uint16_t     netId;
};

// ---- a car's alarm (docs/protocol.md 1.39) --------------------------------
//
// CVehicle::m_nAlarmState (+0x1A0, int16) is 0 for no alarm, -1 for a car a
// generator parked with its alarm set (0x00542884), and otherwise the
// milliseconds it has left to sound. The driver sitting down in an armed car
// (0x004CF3EF) and an armed car touching another car (0x004971BE) both put
// 15000 there; CVehicle::ProcessCarAlarm (0x005525A0) takes the frame's time
// off it; the audio (0x0056C44F) and PreRender's flashing lights (0x00538150)
// read "not 0 and not -1" as sounding.
//
// Only a car its own machine's generator made can be armed, so a copy of it
// anywhere else never goes off by itself: every other copy is built by
// CoopIII, and the constructor writes 0. The machine that simulates the car
// says so once, when it starts, with how long it has left.
constexpr uint16_t VEHICLE_ALARM_MS = 15000;   // 0x3A98, both arming sites

// From the car's driver, or with nobody driving it the player settling it
// (C_VehicleDamage's entitlement): its alarm has `remainingMs` left, or with
// 0 has stopped. Once per alarm, not a stream.
struct C_VehicleAlarm {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_ALARM;
	PacketHeader hdr;
	uint16_t     netId;
	uint16_t     remainingMs;   // 0..VEHICLE_ALARM_MS
};

// To everybody else, and in the backfill with what is left of it then.
// `playerId` is INVALID_PLAYER for the session's own record.
struct S_VehicleAlarm {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_ALARM;
	PacketHeader hdr;
	uint8_t      playerId;
	uint8_t      pad;
	uint16_t     netId;
	uint16_t     remainingMs;
};

// ---- where a car's gun points (docs/protocol.md 1.39) ----------------------
//
// CAutomobile::m_fCarGunLR (+0x580) and m_fCarGunUD (+0x584), radians. The
// Rhino's turret turns with the first: CAutomobile::Render (0x00539EA0,
// vtable slot 13) rotates its turret frame by it, and TankControl
// (0x0053D530) aims the shell with it. The fire truck's water cannon uses
// both (FireTruckControl, 0x00522590). Each is changed only by the machine
// whose player is driving - both functions leave at once for any car that is
// not FindPlayerVehicle - so a copy's turret faced forward for good.
//
// Unreliable, on the snapshot channel, from the driver of one of those two,
// whenever it moved and once a second besides. The server keeps the last one
// for the backfill.
struct C_VehicleAim {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_AIM;
	PacketHeader hdr;
	uint16_t     netId;
	float        gunLR;
	float        gunUD;
};

struct S_VehicleAim {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_AIM;
	PacketHeader hdr;
	uint8_t      playerId;   // INVALID_PLAYER for the session's record
	uint8_t      pad;
	uint16_t     netId;
	float        gunLR;
	float        gunUD;
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
// script on the host. §2.7 is the argument in full. A mission's script runs
// on its owner's machine instead, so while one runs that player is the one
// followed (sky.h).
//
// Two weather types because CWeather doesn't have one. It blends from
// OldWeatherType to NewWeatherType across a game hour, so a single type
// describes the destination and not the sky. The blend position isn't sent:
// CWeather::Update recomputes it as CClock::GetMinutes()/60 every frame, so
// once the clock matches, the blend matches for free. ForcedWeatherType isn't
// either: a follower pins its own to what it is told, and GTA III has no
// extra colours (SET_EXTRA_COLOURS is Vice City's), so CTimeCycle::Update
// reads nothing past the hour, the minute, the two types and the blend.
struct WorldStateBody {
	uint8_t hour, minute;
	uint8_t weather;        // eWeatherType: 0 sunny, 1 cloudy, 2 rainy, 3 foggy
	uint8_t weatherOld;     // the one being blended out of
};

// Only the host sends this, once a second, and at once when its clock jumps or
// its weather turns. While the session's mission runs it is the mission's
// owner who sends it instead (sky.h). The server drops it from anyone else,
// the same way it drops a vehicle snapshot from a player who isn't driving
// that vehicle.
struct C_WorldState {
	static constexpr uint8_t OPCODE = OP_C_WORLD_STATE;
	PacketHeader   hdr;
	WorldStateBody body;
};

// hdr.sendTimeMs here, and on S_Welcome, is the server's own clock and not a
// relayed one. Clients estimate that clock from it and run the El, the
// subway, the background planes, the traffic lights and the Shoreside lift
// bridge on it (client/src/sessiontime.h), which is why none of their state
// is on the wire. Relaying the host's header instead would still parse and
// would put every machine's trains, planes, lights and bridge somewhere
// different again.
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
//
// A pedestrian or a car the session's mission made on its owner's machine
// (docs/missions.md 5.3) travels the same way, hosted by that machine, with
// AMBIENT_MISSION in `flags`: the mission's enemies, its targets and their
// cars exist on every participant's machine and are fought there.
constexpr uint8_t AMBIENT_MISSION = 0x01;

struct AmbientPedBody {
	uint16_t modelId;
	uint8_t  pedType;    // ePedType, so the replica lands in the same engine
	                     // counter as the original - see population.md §1.3
	uint8_t  flags;      // AMBIENT_MISSION; 0 from a build before it, and pos
	                     // stays 4-aligned
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
// carries twelve peds a batch (MAX_PED_STATES), a corpse's turn in it comes
// rarely, and ApplyAmbientPedState refuses to drive anything into a replica
// that is already dead without ever being the thing that makes one dead. So
// you shoot a pedestrian, he drops on your screen, and on every other screen
// he keeps walking.
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

// ---- shooting somebody else's pedestrian (CH_EVENT) ------------------------
//
// The direction the two packets above do not have, and the one the ambient
// population never had at all.
//
// A limb and a death travel from a ped's host out to the observers. Nothing
// travelled the other way, so an observer could empty a clip into a replica
// and the machine that owns the pedestrian never heard about it: every
// replica is bullet-, fire-, melee- and explosion-proof on purpose
// (game/population.cpp, SpawnAmbientReplica), so the shooter's own engine
// refused the hit and there was nothing left to apply or to forward. No
// flinch, no blood, no death, on either screen.
//
// **This is DamageBody's exchange, aimed at a pedestrian, and every field is
// an argument of the same function.** CPed::InflictDamage (0x004EA420,
// `ret 14h`, `this` in ecx) takes exactly five: `damagedBy`, `method`,
// `damage`, `pedPiece` and `direction`. Four of them are here unchanged and
// the fifth cannot travel - a pointer means nothing on another machine - so
// S_PedDamage carries the sender's player id instead and the owner resolves
// it back to the replica it holds of that player's own ped. Nothing else is
// carried, because the engine takes nothing else:
//
//   netId      which pedestrian, as the session named it. The shooter knows
//              it because it holds a replica under that name; the owner
//              resolves it back to a live CPed through its hosted roster.
//   weapon     `method`. Steers the proof-flag switch, the reaction, the
//              limb roll and CPed::m_lastWepDam (+0x51E). Bounded to the same
//              causes an attacker is allowed to decide about a player
//              (IsForwardableDamage): a ray or a melee reach, resolved from a
//              position, an instant and an aim only the shooter has.
//   amount     `damage`, raw. No multiplier, no armour, no clamp - all three
//              are the owner's, and only the owner has the ped's real health.
//   piece      `pedPiece`. It decides which limb comes off, and the limb then
//              travels back out on 17's own C_PedBodyPart from the machine
//              whose engine actually took it.
//   direction  0 front, 1 left, 2 back, 3 right. Two four-entry jump tables
//              at 0x005F9E3C and 0x005F9E5C index straight off it and pick
//              the knockdown animation (ebx = 17h..1Ch), so without it every
//              pedestrian in the city falls the same way.
//
// **No position and no shot vector.** The shooter's own engine already
// resolved the ray; what crossed the wire is its conclusion. A position would
// invite the owner to re-resolve it, which is the observer deciding damage
// with the arguments the other way round.
//
// **Friendly fire does not gate this.** It is a rule about players hurting
// each other (docs/roadmap.md §5.2); a pedestrian is not a player, and a
// session with friendly fire off still lets everybody shoot NPCs.
//
// **A pedestrian in a car cannot be killed by this, and that is retail 1.0
// rather than a limitation of the wire.** InflictDamage's in-vehicle arm
// (`cmp byte [ebp+314h],0 / jne` at 0x004EACF3) sends everything that is not
// WEAPONTYPE_DROWNING to 0x004EADD0, which is
// `mov dword [ebp+2C0h],3F800000h / xor al,al` - health clamped to exactly
// 1.0f, and "did not die". So the hit is sent and applied unchanged and the
// driver survives on one health, which is what single player does and what
// every machine then agrees on. Nothing here synthesises a death the engine
// declined to give.
struct PedDamageBody {
	uint16_t netId;
	// 9 (the flamethrower) is not a hit: the shooter's flame reached this
	// pedestrian and the owner should light him. amount is 0 (§1.24).
	uint8_t  weapon;
	float    amount;
	uint8_t  piece;
	uint8_t  direction;
	uint8_t  melee;      // as on DamageBody
	uint8_t  hitLevel;
};

struct C_PedDamage {
	static constexpr uint8_t OPCODE = OP_C_PED_DAMAGE;
	PacketHeader  hdr;
	PedDamageBody body;
};

// To the ped's owner alone, not broadcast, exactly like S_Damage. Nobody else
// has anything to do with it: what the other machines need to see - the
// flinch, the limb, the corpse - reaches them from the owner afterwards, on
// the owner's own ped stream and on 17's and 18's packets.
//
// Nothing is kept. Unlike a death, a hit is not a state a joiner has to be
// told about: the health it produced lives on the owner's machine and no
// packet has ever carried an ambient ped's health (AmbientPedState, and the
// omission is deliberate).
struct S_PedDamage {
	static constexpr uint8_t OPCODE = OP_S_PED_DAMAGE;
	PacketHeader  hdr;
	uint8_t       attackerId;
	PedDamageBody body;
};

// ---- an NPC's gunfire, and its hits on players (0x90..0x93) ----------------
//
// A pedestrian or a cop fights on the machine hosting it: its AI picks the
// target, its CWeapon::Fire traces the round, and when the round or a punch
// reaches another player's copy there, that machine's engine is the one that
// found the hit. It used to throw the hit away, so an NPC only ever hurt the
// player on its own host's machine. Now the host forwards it the way a
// player's own hit is forwarded (C_Damage) and the victim applies it to
// himself.
//
// The round goes to everybody else as well, so the NPC is seen and heard
// firing. It is drawn through the observer's own CWeapon::Fire on its replica,
// the way a remote player's round is, and it decides nothing there: the damage
// it would find is refused, because the host already decided it.
//
// Only the five guns that trace a ray (combat.h, IsInstantHitWeapon) are sent.
// Retail NPCs carry nothing else that fires.

// Unreliable: a lost round is a missing streak, and a firefight with a few
// cops in it is a stream rather than a handful of events.
struct C_NpcShot {
	static constexpr uint8_t OPCODE = OP_C_NPC_SHOT;
	PacketHeader hdr;
	uint16_t     pedNetId;
	ShotBody     body;
};

struct S_NpcShot {
	static constexpr uint8_t OPCODE = OP_S_NPC_SHOT;
	PacketHeader hdr;
	uint8_t      ownerPlayerId;
	uint16_t     pedNetId;
	ShotBody     body;
};

// One hit a hosted pedestrian landed on another player's copy, exactly as the
// host's CPed::InflictDamage was asked to make it. The body is C_Damage's: the
// victim's netId and the engine's own five arguments, plus the melee bytes
// for a punch or a bat. Friendly fire has no say, because nobody here is a
// player hurting another player.
struct C_NpcDamage {
	static constexpr uint8_t OPCODE = OP_C_NPC_DAMAGE;
	PacketHeader hdr;
	uint16_t     attackerPedNetId;
	DamageBody   body;
};

// To the victim alone, like S_Damage.
struct S_NpcDamage {
	static constexpr uint8_t OPCODE = OP_S_NPC_DAMAGE;
	PacketHeader hdr;
	uint8_t      ownerPlayerId;
	uint16_t     attackerPedNetId;
	DamageBody   body;
};

// ---- shooting somebody else's car (CH_EVENT) -------------------------------
//
// The pedestrian exchange above, aimed at a car another player is driving.
// Same split of authority, same direction, same block - and three fields
// instead of five, because the engine's function takes three instead of five.
//
// The gap it closes is NOT "nothing happens". A replica pedestrian is bullet-,
// fire-, melee- and explosion-proof on purpose, so a shot at one was refused
// outright. A replica *car* is proof against exactly one thing - CoopIII sets
// bCollisionProof on it and nothing else (game/vehicle.cpp,
// SetVehicleObserved) - so a shot at one was accepted, locally, and took
// health off a copy nobody else could see. The symptom is divergence rather
// than silence: the observer's copy smokes and burns on a health its owner
// never had, while the owner's copy is untouched and never dies. Nothing
// travelled either way, because SampleLocalVehicleDamage is driver-only and
// carries no health, and "a replica is never announced at all".
//
// **Every field is an argument of CVehicle::InflictDamage, and it has three.**
// Verified against retail 1.0 rather than taken from the tree: 0x00551950 is a
// clean function start (twelve bytes of 0x00 alignment padding in front of it,
// opening `push ebx / push esi / mov esi,ecx`), it is __thiscall, and it ends
// `ret 0Ch` - three dword arguments and no more. The callee reads them at
// [esp+20h], [esp+24h] and [esp+28h]; the explosion call site at 0x004B18BE
// writes the same three in the same order (`push eax / fstp dword [esp]` for
// the float, then `push 12h`, then the culprit). So:
//
//   netId    which car, as the session named it. The shooter knows it because
//            it holds a replica under that name; the owner resolves it back
//            through the roster to the CVehicle its own engine is driving.
//   weapon   the second argument. It steers a twenty-entry jump table at
//            0x006026CC that picks which proof flag - if any - gets to refuse
//            the hit, it is written verbatim into m_nLastWeaponDamage
//            (+0x228) at 0x00551AB1, and at 0x00551C1A a value of 18 is what
//            makes a destroyed car re-roll its bomb timer. Bounded to the
//            same causes an attacker may decide about a player or a
//            pedestrian (IsForwardableDamage): a ray or a melee reach,
//            resolved from a position, an instant and an aim only the shooter
//            has.
//   amount   the third argument, raw. No multiplier, no clamp against the
//            car's health, no 250 threshold - all three are the owner's, and
//            only the owner has the car's real health.
//
// **No health, and this is the field whose absence is load-bearing.** The
// engine's own damage path proves why in three instructions: InflictDamage
// writes `mov dword [esi+200h],0` at 0x00551C10 and then, sixty-nine bytes
// later, calls BlowUpCar through the vtable at 0x00551C5A. The zero and the
// destruction are two acts and only the second destroys anything, so a health
// copied off a socket produces a car with no health that is not wrecked - and
// one below 250 arms the engine's five-second fire timer, which is an observer
// deciding, five seconds later, that somebody else's car is finished. Health
// travels the way it always has: as a field on the driver's own 25 Hz
// snapshot, out of the machine that owns it.
//
// **No position and no shot vector.** The shooter's engine already resolved
// the ray; what crosses the wire is its conclusion, not its inputs. A position
// would invite the owner to re-resolve it against a car it holds at a
// different place - the observer deciding damage with the arguments the wrong
// way round - and it would put a second, competing source of truth next to the
// transform stream that already exists.
//
// **No panels and no doors.** Those already travel, as absolute state from the
// driver on C_VehicleDamage, and the owner's own InflictDamage is what will
// produce them in the first place. Sending a dent here would double-count
// against a record whose whole arbitration is that it is a monotone maximum.
//
// **Only a car another player is DRIVING**, or is settling for the session
// after getting out of it (S_VehicleCustody) - that machine's engine is the
// one simulating the car until it hands it back, so it is the owner in every
// sense this packet cares about. Not ambient traffic, not a parked car the
// map generated, not a session car somebody abandoned - and the reason
// is that those three have no machine entitled to decide their condition,
// which is what "unowned" means in this project. Each engine damages its own
// copy; a replayed blast damages every copy identically, because
// CWorld::TriggerExplosionSectorList's multiplier is a function of two
// positions every machine agrees on; and what does NOT converge - accumulated
// gunfire, a shove - is exactly what C_UnownedBlowUp was built to carry, host
// to observer, after the fact. Routing an unowned car's hits through here
// would invent a fourth ownership model for a problem roadmap.md §5.8 closed,
// and would require the server to keep a health for every car in Liberty City.
// Ambient traffic is the exception to that list: it has a host, and the host
// decides. It rides C_CarHit below with this same body, because its owner is
// found by a different rule.
//
// **And so, now, is the abandoned session car.** "Each engine damages its own
// copy" was never true of it: every machine writes the session's last health
// back onto it every frame, so a hit lasted one frame and a parked session car
// could not be shot into a fire. A hit on one comes through here, and the
// server makes the shooter its custodian and sends the hit back to them
// (S_VehicleCustody, Session::CustodyForHit). No health is kept on the server
// for it; the custodian's snapshot carries it, as it does after an exit.
//
// **Friendly fire has no say**, the same as the pedestrian pair. It is a rule
// about players hurting each other; a car is not a player, and a session with
// it off still lets everybody shoot cars.
struct VehicleHitBody {
	uint16_t netId;
	// 9 is an ignition rather than a hit, on C_CarHit too (§1.24), and
	// VEHICLE_HIT_PUSH on C_VehicleHit is a shove.
	uint8_t  weapon;
	float    amount;
};

// "Our car has just shoved this one, and nobody holds it": a session car
// parked where the session last had it is pinned there on every screen, so
// until somebody's engine is allowed to simulate it, pushing it is pushing a
// wall. The server makes the sender its custodian, as for a shot, and relays
// nothing. No engine weapon has this number and IsForwardableDamage refuses
// it, so a server from before it relays a shove nobody applies.
constexpr uint8_t VEHICLE_HIT_PUSH = 0xFE;

struct C_VehicleHit {
	static constexpr uint8_t OPCODE = OP_C_VEHICLE_HIT;
	PacketHeader   hdr;
	VehicleHitBody body;
};

// To the car's driver alone - or with no driver, its custodian, who may be the
// sender if the hit is what made them custodian - not
// broadcast, exactly like S_PedDamage and S_Damage. Nobody else has anything to do with it: what the other machines
// need to see - the smoke, the flames, the wreck - reaches them from the owner
// afterwards on paths that already exist. The health rides the driver's own
// 25 Hz VehicleStateBody, the dents ride C_VehicleDamage, and the explosion
// rides C_VehicleBlowUp with the transform the owner's physics chose.
//
// `attackerId` is the sender's player id, for the same reason S_PedDamage
// carries one: a CEntity* means nothing on another machine, so the owner
// resolves the id back to the replica it holds of that player's own ped and
// hands that to the engine as the culprit.
//
// Nothing is kept on the server. A hit is not a state a joiner has to be told
// about - the health it produced lives on the owner's machine and reaches the
// session on the snapshot that has always carried it.
struct S_VehicleHit {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_HIT;
	PacketHeader   hdr;
	uint8_t        attackerId;
	VehicleHitBody body;
};

// An NPC's round on a car another player is driving or settling, the last of
// the 0x90 block (C_NpcShot has the rest): the cop shooting at the car we
// drive, or at the one his own player is a passenger in. C_VehicleHit's body,
// to that car's driver or custodian (Session::VehicleHitRecipient), who blames
// the cop's replica rather than the player whose machine hosts him. Traffic
// and a car nobody holds are not sent (game/vehicle.h, DecideCarDamage).
struct C_NpcVehicleHit {
	static constexpr uint8_t OPCODE = OP_C_NPC_VEHICLE_HIT;
	PacketHeader   hdr;
	uint16_t       attackerPedNetId;
	VehicleHitBody body;
};

struct S_NpcVehicleHit {
	static constexpr uint8_t OPCODE = OP_S_NPC_VEHICLE_HIT;
	PacketHeader   hdr;
	uint8_t        ownerPlayerId;
	uint16_t       attackerPedNetId;
	VehicleHitBody body;
};

// ---- shooting somebody else's traffic (CH_EVENT) ---------------------------
//
// docs/protocol.md §1.23. A hit the local player landed on a replica of a
// traffic car another machine hosts, sent to that machine so its own
// CVehicle::InflictDamage decides what it costs. Same three fields as
// VehicleHitBody, bounded the same way on the receiving end.
//
// The owner is AmbientCar::ownerPlayerId, the machine whose CCarCtrl made the
// car. That machine is already the only one allowed to stream the car and the
// only one allowed to say it blew up (UNOWNED_AMBIENT), so the health that
// leads to the blow-up belongs there too. It's also the one machine with the
// car and the ground under it loaded: CCarCtrl drops traffic that gets far
// from its own player, so a hosted car only exists near its host.
//
// The replica refuses every hit and every BlowUpCar locally. Without this
// packet that would make traffic bulletproof to everyone but its host.
//
// Nothing carries the result back except what already did: the health rides
// AmbientCarState::health on the car stream, and the wreck rides
// C_UnownedBlowUp with the host's transform.
struct C_CarHit {
	static constexpr uint8_t OPCODE = OP_C_CAR_HIT;
	PacketHeader   hdr;
	VehicleHitBody body;
};

// To the car's host alone. `attackerId` is resolved back to the host's replica
// of the shooter's ped and passed as the culprit, the same as S_VehicleHit.
struct S_CarHit {
	static constexpr uint8_t OPCODE = OP_S_CAR_HIT;
	PacketHeader   hdr;
	uint8_t        attackerId;
	VehicleHitBody body;
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
//   - **health, armour, aim (about 16 bytes).** An ambient ped's damage,
//     death and removal all happen on its host. A replica can be shot from
//     another machine, but the hit goes to the host as C_PedDamage (20) and
//     the limb and the death come back as S_PedBodyPart (17) and
//     S_PedDeath (18), so health never needed to travel. The weapon did,
//     and it rides four bits of `flags` below: the replica holds the same
//     gun, and its rounds arrive on C_NpcShot with their own line, so the
//     aim is not needed either. (`flags` is what used to be padding.)
//
// 24 bytes, against PlayerStateBody's 71 and AmbientCarState's 44.
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
	//
	// A session car is named too: a car a player has claimed can have the
	// host's pedestrian in it, and the host is still the only machine that
	// knows which seat he is in (the history entry above PROTOCOL_VERSION).
	uint16_t vehicleNetId;  // INVALID_NETID when on foot
	uint8_t  seat;          // 0 = driver, 1.. = passenger
	// AMBIENT_PED_* bits. This byte was padding and every sender zeroed it,
	// so an older sender says "nothing" and an older receiver never looks.
	uint8_t  flags;
	Vec3     pos;
	float    heading;
};

// CPed::m_pFire != nil on the host's pedestrian. The observer lights its own
// copy on the replica, the way it does for a burning player (PF_ON_FIRE):
// no AI, nothing that can cost health, and it goes out a second after the
// host stops saying so. docs/protocol.md §1.24.
constexpr uint8_t AMBIENT_PED_ON_FIRE = 1 << 0;

// Bits 1..4: the eWeaponType in the host's pedestrian's hand, 0..12. An older
// sender leaves them 0, which is UNARMED and what every replica held before
// they existed, and an older receiver only ever reads bit 0. The observer puts
// the same weapon in its replica's hand, which is what C_NpcShot fires.
constexpr uint8_t AMBIENT_PED_WEAPON_SHIFT = 1;
constexpr uint8_t AMBIENT_PED_WEAPON_MASK  = 0x1E;

// Bit 5: the host's pedestrian sits in a car the session has no name for, so
// vehicleNetId cannot say which - a car his host does not host, or one it
// hosts and the server has not named yet. Where he is is that car's seat, so
// the observer keeps its replica out of sight and out of the collision rather
// than standing him in mid-air there, driving nothing. An older sender never
// sets it and an older receiver never reads it.
constexpr uint8_t AMBIENT_PED_IN_UNSEEN_CAR = 1 << 5;
static_assert((AMBIENT_PED_IN_UNSEEN_CAR & (AMBIENT_PED_ON_FIRE | AMBIENT_PED_WEAPON_MASK)) == 0,
              "the unseen-car bit is clear of the fire bit and the weapon bits");

inline uint8_t AmbientPedFlagsWithWeapon(uint8_t flags, uint8_t weapon) {
	if (weapon >= INVENTORY_SLOTS)
		weapon = 0;
	return static_cast<uint8_t>((flags & ~AMBIENT_PED_WEAPON_MASK) |
	                            (weapon << AMBIENT_PED_WEAPON_SHIFT));
}

// 0 for anything that is not an inventory weapon, which can only be a newer
// sender's bits this build does not know.
inline uint8_t AmbientPedWeapon(uint8_t flags) {
	const uint8_t w = static_cast<uint8_t>((flags & AMBIENT_PED_WEAPON_MASK) >>
	                                       AMBIENT_PED_WEAPON_SHIFT);
	return w < INVENTORY_SLOTS ? w : 0;
}

// Bit 6: the host's pedestrian is opening a door of `vehicleNetId` and is not
// in it yet (PED_ENTER_CAR). `seat` is then AmbientPedEntrySeatByte: the seat
// the entry ends in, low nibble, and the door he goes in by as a seat, high
// nibble - a driver can go in by the front passenger's door and shuffle
// across. The observer's copy opens the same door (Client::UpdateAmbientPedSeats).
constexpr uint8_t AMBIENT_PED_ENTERING = 1 << 6;
// Bit 7: he is climbing out of the seat `vehicleNetId` and `seat` name
// (PED_EXIT_CAR, still in it). The copy climbs out with him.
constexpr uint8_t AMBIENT_PED_EXITING = 1 << 7;
static_assert(((AMBIENT_PED_ENTERING | AMBIENT_PED_EXITING) &
               (AMBIENT_PED_ON_FIRE | AMBIENT_PED_WEAPON_MASK | AMBIENT_PED_IN_UNSEEN_CAR)) == 0,
              "the door bits are clear of the fire, weapon and unseen-car bits");

// How long an observer takes either bit at its word, and so how often the
// host restates it while it holds: a door takes about two seconds and a row
// can be lost.
constexpr uint32_t AMBIENT_DOOR_SAID_MS = 1500;

inline uint8_t AmbientPedEntrySeatByte(uint8_t seat, uint8_t door) {
	return static_cast<uint8_t>((seat & 0x0F) | ((door & 0x0F) << 4));
}
inline uint8_t AmbientPedEntrySeat(uint8_t seatByte) {
	return static_cast<uint8_t>(seatByte & 0x0F);
}
inline uint8_t AmbientPedEntryDoor(uint8_t seatByte) {
	return static_cast<uint8_t>(seatByte >> 4);
}

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
// A host with more peds than that gives them turns, the ones nearest another
// player most often (client/src/game/streampick.h), so the batch stays this
// size however many it hosts.
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
	uint8_t  flags;            // AMBIENT_MISSION, as for a pedestrian
	uint8_t  pad;              // keeps pos 4-aligned and the layout explicit
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

// ---------------------------------------------------------------------------
// A player got into somebody else's traffic (docs/protocol.md §1.21)
// ---------------------------------------------------------------------------
//
// An ambient car is owned by the machine whose CCarCtrl made it and that
// ownership never moves (population.md §1.1) - which was fine while nobody
// was in one, and stops being fine the moment a player takes the wheel. The
// ambient roster has an owner and no seats, so the driver is invisible to it:
// every observer goes on drawing that player's ped in the road beside a car
// its original host is still steering.
//
// So the car stops being traffic and becomes a session car - the same claim
// C_EnterVehicle has always carried, sent with the netId the session already
// has for it. The server recognises a netId that names an AmbientCar rather
// than a Vehicle and promotes the row in place.
//
// **The netId does not change and no car is created or destroyed anywhere.**
// Every machine keeps the CVehicle it already has and only the bookkeeping
// moves: the new driver and the observers convert a replica they built, the
// original host converts a car its own engine made and must never delete.
// That is strictly better than the promotion that already happens when a host
// gets into its own traffic (population.cpp, SweepHostedCars), which drops the
// replica on every other machine and rebuilds it.
//
// Sent to everyone, the claimer included, and immediately before the
// S_EnterVehicle that names the driver - so no machine ever sees a seat
// announced for a netId it still files under traffic.
struct S_CarPromoted {
	static constexpr uint8_t OPCODE = OP_S_CAR_PROMOTED;
	PacketHeader hdr;
	uint16_t netId;
	// Who took it. Carried so a machine that never had the ambient row can
	// still tell whether the car it is about to build is its own.
	uint8_t  driverPlayerId;
	// Who was hosting it as traffic until this moment. The one machine for
	// which this CVehicle is its own engine's work rather than a replica, and
	// therefore the one machine that must never destroy it afterwards.
	uint8_t  wasOwnerPlayerId;
	// Everything S_VehicleSpawn would have carried, so a machine with no row
	// for this car at all can build it from this packet alone rather than
	// waiting for a spawn that is never sent.
	AmbientCarBody body;
};


// ---------------------------------------------------------------------------
// A player left, and his crowd stays (CH_EVENT)
// ---------------------------------------------------------------------------
//
// Every ambient ped and traffic car is hosted by the machine whose engine made
// it, and until this a player leaving took all of them with him: one despawn
// each, and the whole street around him emptied at once on every other
// screen. Every other machine already has a replica of each of them - a real
// CCivilianPed or CAutomobile in its own world - so there is nothing to build,
// only somebody to put in charge.
//
// The server picks, per entity, the remaining player nearest to it (server/
// core/adopt.h has the rule and the distances), and names him here. That
// machine turns its replica back into an ordinary engine-driven ped or car
// and starts streaming it under the same netId; everybody else keeps the
// replica they have and starts taking rows from the new owner. Anything no
// remaining player is near enough to keep goes with an ordinary S_PedDespawn
// or S_CarDespawn, as before.
//
// A car and the peds sitting in it always go to the same machine and always in
// the same packet, which is what lets a receiver re-own them in one step: its
// seat pass ties a ped to a car only when both have the same owner.
//
// A machine given something it cannot take - no replica here yet, a corpse -
// lets go of it with C_PedDespawn / C_CarDespawn, which the server takes from
// the owner and it now is.
constexpr uint8_t AMBIENT_ADOPT_PED = 0;
constexpr uint8_t AMBIENT_ADOPT_CAR = 1;

struct AmbientAdoptRow {
	uint16_t netId;
	uint8_t  kind;               // AMBIENT_ADOPT_*
	uint8_t  newOwnerPlayerId;
};

// Enough for a car with a full load of passengers several times over. A leaver
// with more than this goes out in several packets, never splitting a car from
// its occupants.
constexpr uint8_t MAX_ADOPT_ROWS = 32;

// Why the rows changed hands. The old owner is still in the session for both:
// a leaver's batch goes out just ahead of his S_PlayerLeave.
constexpr uint8_t AMBIENT_ADOPT_LEFT   = 0;   // he is leaving
constexpr uint8_t AMBIENT_ADOPT_LET_GO = 1;   // his engine dropped it (C_CarLetGo)

struct S_AmbientAdopt {
	static constexpr uint8_t OPCODE = OP_S_AMBIENT_ADOPT;
	PacketHeader    hdr;
	uint8_t         wasOwnerPlayerId;
	uint8_t         count;
	uint8_t         why;             // AMBIENT_ADOPT_LEFT / _LET_GO
	uint8_t         pad;
	AmbientAdoptRow rows[MAX_ADOPT_ROWS];
};

// ---------------------------------------------------------------------------
// A traffic car its host's engine drops while somebody else can see it
// (CH_EVENT)
// ---------------------------------------------------------------------------
//
// Each machine's CCarCtrl::PossiblyRemoveVehicle measures a traffic car
// against that machine's own player and camera: 25 m behind him for one
// stopped in traffic, 50 m off his screen, 130 m on it. The machine that made
// a car hosts it, so a car can be past all of that for its host and right in
// front of another player, and the host's despawn took it off his screen.
//
// So a host whose engine drops a car by distance while another player is
// within AMBIENT_CAR_KEEP_RADIUS_M of it sends this instead of C_CarDespawn.
// The server hands the car, and the host's pedestrians named here with it
// (its driver and passengers, whom ~CVehicle takes with the car), to the
// player nearest it who has not let go of it lately, with an S_AmbientAdopt
// to everybody else (why AMBIENT_ADOPT_LET_GO) - the same handover a leaver's
// crowd gets. The sender is sent the car and the peds as backfill spawns
// under their new owner, since its engine has already deleted its own. With
// nobody to take it, it goes with S_CarDespawn / S_PedDespawn as before.
//
// The receiver's own engine then keeps it by its own player and camera, which
// is the only camera that can say whether the car is in view.
constexpr uint8_t MAX_LET_GO_PEDS = 9;   // a driver and eight passengers

// The engine's widest distance for keeping a traffic car, 130 m on screen
// times 1.5 for bExtendedRange (client addresses.h, CCarCtrl::
// PossiblyRemoveVehicle). Past it nobody's engine keeps the car anyway.
constexpr float AMBIENT_CAR_KEEP_RADIUS_M = 130.0f * 1.5f;

struct C_CarLetGo {
	static constexpr uint8_t OPCODE = OP_C_CAR_LET_GO;
	PacketHeader hdr;
	uint16_t     netId;
	uint8_t      pedCount;
	uint8_t      pad;
	uint16_t     peds[MAX_LET_GO_PEDS];
};

// Which pedestrians can be handed on at all. A replica is always built as a
// CCivilianPed, and for anything but the two civilian types it is built as
// CIVMALE (client/src/game/population.cpp, SpawnAmbientReplica): a cop's
// replica is not a CCopPed, a gang member's is counted as a civilian. Turned
// back into a hosted ped, those would be the wrong kind of pedestrian with the
// wrong AI, so only the two whose replica is exactly what the original was
// are adopted. Numbers are ePedType (client addresses.h, PEDTYPE_CIVMALE).
constexpr uint8_t AMBIENT_PEDTYPE_CIVMALE   = 4;
constexpr uint8_t AMBIENT_PEDTYPE_CIVFEMALE = 5;

inline bool AmbientPedTypeAdoptable(uint8_t pedType) {
	return pedType == AMBIENT_PEDTYPE_CIVMALE || pedType == AMBIENT_PEDTYPE_CIVFEMALE;
}

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
//
// `health` is CVehicle::m_fHealth in whole points, in the two bytes that used
// to be padding, so the row costs what it always did. A replica refuses all
// damage of its own (docs/protocol.md §1.23), so this is the only way its
// smoke and flames can match the host's. 0 means "not said" and the receiver
// keeps what it had. A live car never encodes to 0: the lowest value sent is
// 1, and a wreck travels as C_UnownedBlowUp, never as a health.
struct AmbientCarState {
	uint16_t netId;
	uint16_t health;
	Vec3     pos;
	Quat     rot;
	Vec3     velocity;
};

constexpr uint16_t AMBIENT_HEALTH_UNSAID = 0;

// Whole points, clamped to 1..65535. NaN is "unsaid" rather than a number.
inline uint16_t EncodeAmbientHealth(float health) {
	if (health != health)
		return AMBIENT_HEALTH_UNSAID;
	if (health < 1.0f)
		return 1;
	if (health >= 65535.0f)
		return 65535;
	return static_cast<uint16_t>(health + 0.5f);
}

// False for "unsaid", and `out` is left alone.
inline bool DecodeAmbientHealth(uint16_t wire, float &out) {
	if (wire == AMBIENT_HEALTH_UNSAID)
		return false;
	out = static_cast<float>(wire);
	return true;
}

// Eight, and the number is the design rather than a round figure.
//
// population.md §2.1: a dozen cars at the player rate is already most of the
// budget, and a flat rate is not on the table. Eight cars at 10 Hz is
// 8 × 44 × 10 = 3.5 KB/s each way per observer, against 13 KB/s for the same
// dozen cars at 25 Hz. A ninth car shares the eight rows by turns, the same
// way the peds share theirs.
constexpr uint8_t MAX_CAR_STATES = 8;

// A traffic car's horn, one bit per row, kept in the batch's own padding
// (C_CarStates::hornMask, S_CarStates::hornMask). The host sets bit i when the
// car in cars[i] has a running m_nCarHornTimer (client/src/game/horn.h,
// TrafficHornOnWire). It means "the timer is running now" and says nothing
// about how long - the receiver runs the engine's own 44-frame countdown for
// as long as the newest row says so. On a car the host's audio honks, that is
// the horn; on a police car, an ambulance or an Enforcer with its siren on
// (the siren bit beside it) it is the fast wail, on the replica as on the host.
//
// AmbientCarState itself has no room left: 28 turned its last two padding
// bytes into `health` and the other 40 are the transform. The batch header
// had three spare bytes going out and two coming back, and InitHeader zeroes
// them, so an older sender or an older server sends "no horns" and an older
// receiver never looks. The siren took the second one each way, which leaves
// one going out and none coming back.
inline constexpr uint8_t CarStateHornBit(uint8_t row) {
	return row < MAX_CAR_STATES ? static_cast<uint8_t>(1u << row) : 0;
}

inline constexpr bool CarStateHornSet(uint8_t mask, uint8_t row) {
	return (mask & CarStateHornBit(row)) != 0;
}

static_assert(MAX_CAR_STATES <= 8, "the horn mask is one byte, one bit per row");

// The siren, one bit per row the same way (C_CarStates::sirenMask,
// S_CarStates::sirenMask): bit i is cars[i]'s CVehicle::m_bSirenOrAlarm on its
// host. Unlike the horn it is a state and not a honk, so the receiver holds it
// from row to row, the way it holds the transform (game/siren.h,
// ReplicaTrafficSirenOn). It is the light bar; whether the siren is also heard
// is the replica's own status, as it is on the host.
inline constexpr uint8_t CarStateSirenBit(uint8_t row) { return CarStateHornBit(row); }

inline constexpr bool CarStateSirenSet(uint8_t mask, uint8_t row) {
	return CarStateHornSet(mask, row);
}

struct C_CarStates {
	static constexpr uint8_t OPCODE = OP_C_CAR_STATES;
	PacketHeader    hdr;
	uint8_t         count;   // how many of `cars` are real; the rest is padding
	uint8_t         hornMask;    // bit i: cars[i]'s horn is sounding
	uint8_t         sirenMask;   // bit i: cars[i]'s siren is on
	uint8_t         pad;
	AmbientCarState cars[MAX_CAR_STATES];
};

struct S_CarStates {
	static constexpr uint8_t OPCODE = OP_S_CAR_STATES;
	PacketHeader    hdr;
	uint8_t         ownerPlayerId;
	uint8_t         count;
	// The sender's masks, re-packed by the server when it drops a row so that
	// bit i still names cars[i].
	uint8_t         hornMask;
	uint8_t         sirenMask;
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
	// This model is MI_PICKUP_KILLFRENZY, the skull. Same reason as the bribe
	// bit: only the game knows the number. A claim with it set opens a vote
	// instead of being granted, when the session has a shared rampage and
	// somebody to share it with (server/core/rampagevote.h).
	PICKUP_F_RAMPAGE = 1 << 1,
	// Set by the server on the one grant that ends a vote that passed. The
	// starter's machine takes the pickup whether or not the player is still
	// standing on it: the vote was the say, and the others are already being
	// moved.
	PICKUP_F_VOTED   = 1 << 2,
	// A weapon, armour, health or cash pickup the session's mission laid out
	// (mission-audit.md R2, as decided): every player takes their
	// own. A claim is weighed against the claimant's own record, and a
	// collection is told to everybody so the mission's HAS_PICKUP_BEEN_COLLECTED
	// moves on, but nobody else's copy is taken away.
	PICKUP_F_STASH = 1 << 3,
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

// ---- a rampage (CH_EVENT) -------------------------------------------------
//
// docs/rampage.md is the investigation; roadmap.md §5.10 is the decision.
// What a reader of this file needs is four sentences.
//
// 1. **Nothing starts a rampage over the wire and nothing has to.** Every
//    machine runs rampage.sc, the pickup work pushes a remote collection into
//    every machine's own CPickups::aPickUpsCollected, and each machine's own
//    script then calls CDarkel::StartFrenzy with the same ten arguments in
//    the same frame. The weapon, the clock, the four target models, the HUD
//    and the start message are all already identical without a byte on the
//    wire.
//
// 2. **What does not survive is the counting.** KillsNeeded is decremented
//    only by CDarkel::RegisterKillByPlayer, and CPed::InflictDamage only
//    reaches it when the damaging entity is this machine's own player or car
//    (0x004EAD1A). So four machines count four different numbers from one
//    start, and a co-op kill - the ped hosted here, shot from there - counts
//    for none of them.
//
// 3. **So the kill is the fact that travels, and it travels as the engine's
//    own three arguments.** Not "player X killed pedestrian N": the victim's
//    *model index*, the weapon, and whether it was a headshot. A netId would
//    tie the credit to whether the receiver happens to hold a replica of that
//    pedestrian, and during a rampage in Portland it very often does not.
//    The model index is what CDarkel::RegisterKillByPlayer itself reads off
//    the victim (`movsx eax,word [ebp+5Ch]` at 0x00420FCA) and it is the only
//    thing about the victim the qualification test looks at.
//
// 4. **The ending is arbitrated, not synchronised.** Each machine's CDarkel
//    keeps running retail's own logic - its own countdown, its own tick
//    sound, its own weapon restore - and the first machine to leave ONGOING
//    reports what it reached. The server keeps the first report and tells
//    everybody. Between a machine's local ending and the session's verdict,
//    its *script* is still told ONGOING, so all of them leave the wait loop
//    on the same value.

// One machine's script has started a frenzy. Sent by every machine, not just
// the one that walked over the pickup, because every machine's script starts
// it independently and the server has no way to know which of them was first
// to the skull.
//
// `limitMs` is CDarkel::TimeLimit, straight off StartFrenzy's second
// argument, and it is signed because the engine's own no-limit rampage is a
// negative one (`cmp dword [00885BACh],0 / jl` at 0x00420696). `target` is
// the third argument, the kill count the script asked for.
struct RampageStartBody {
	int32_t  limitMs;
	uint16_t target;
};

struct C_RampageStart {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_START;
	PacketHeader     hdr;
	RampageStartBody body;
};

// The session's frenzy is open. To everybody, including the machine whose
// report opened it, because the target it gets back may not be the one it
// asked for.
//
// `frenzyId` is the session's own counter and it exists for one reason: a
// kill or an ending that names a frenzy that has already closed is dropped
// rather than applied to the next one. Rampage pickups are re-created by the
// script within a frame or two of a failure (docs/pickups.md §6), so "the
// next one" can be seconds away.
//
// `killsNeeded` is what the session still wants: the number the script asked
// for under `shared`, that number times the player count under `scaled`, and
// in both cases minus the kills already made. `elapsedMs` is how long the
// session's frenzy has been running on the server's own clock.
//
// Those two fields are also the whole of the late-join answer, and they are
// why this packet answers *every* start rather than only the first. A player
// who joins mid-rampage is backfilled with the KILLFRENZY collection like any
// other pickup, so his own rampage.sc starts a fresh 120-second frenzy with a
// fresh target - and without an answer he would sit in his script's wait loop
// for a verdict he is not part of. Answering him with the open frenzy puts him
// in it instead: two stores, into CDarkel::KillsNeeded and
// CDarkel::TimeOfFrenzyStart, and the engine's own HUD then draws the right
// number and the right clock.
struct RampageOpenBody {
	uint16_t frenzyId;
	uint16_t killsNeeded;
	uint32_t elapsedMs;
};

struct S_RampageOpen {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_OPEN;
	PacketHeader    hdr;
	RampageOpenBody body;
};

// One kill that counted on the machine that made it.
//
// Reported from inside a detour on CDarkel::RegisterKillByPlayer, so it is
// the engine's own judgement of "a kill that counts" rather than CoopIII's -
// including the five paths into that function that are not
// CPed::InflictDamage (a car, a fire, a blast).
//
// `model` is the victim's model index. `weapon` is the eWeaponType the engine
// was given; it is NOT compared against the frenzy's weapon here, because the
// engine's own comparison has five aliases in it (explosion counts for
// everything, uzi-driveby counts for uzi, and so on) and CoopIII would rather
// run that comparison than re-state it. `flags` carries the headshot bit,
// which is the third argument of the same function and matters to the three
// rampages rampage.sc starts with 0x0367.
enum RampageKillFlags : uint8_t {
	RK_F_HEADSHOT = 1 << 0,
};

struct RampageKillBody {
	uint16_t frenzyId;
	uint16_t model;
	uint8_t  weapon;
	uint8_t  flags;
};

struct C_RampageKill {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_KILL;
	PacketHeader    hdr;
	RampageKillBody body;
};

// To everybody but the reporter, whose own engine has already counted it.
// `byPlayer` is for the log and for nothing else - the credit is the
// session's, not any player's, which is the whole of §5.10.
struct S_RampageKill {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_KILL;
	PacketHeader    hdr;
	uint8_t         byPlayer;
	RampageKillBody body;
};

// A machine reporting the ending its own CDarkel reached: PASSED because its
// KillsNeeded hit zero, or FAILED because its clock ran out or its player
// died. A report for a frenzy that is not the open one is dropped.
struct RampageEndBody {
	uint16_t frenzyId;
	uint8_t  outcome;   // RampageOutcome
};

struct C_RampageEnd {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_END;
	PacketHeader   hdr;
	RampageEndBody body;
};

// The session's verdict, to everybody. The first report wins and the rest are
// dropped, which is the pickup rule (docs/pickups.md §3) applied to an
// outcome instead of an object: the client detects, the server arbitrates,
// the engine awards, in that order.
//
// Not kept for a joiner. A player who arrives mid-rampage has no frenzy of
// his own - his script never saw the pickup - so there is no wait loop on his
// machine for a verdict to end. His kills still count for everybody else,
// because he reports them and the qualification is done by the receivers.
struct S_RampageEnd {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_END;
	PacketHeader   hdr;
	RampageEndBody body;
};

// ---- the vote before a rampage ---------------------------------------------
//
// Under `shared` and `scaled` a rampage is everybody's, so it's everybody's to
// start. Touching a skull no longer starts it: the claim (C_PickupClaim with
// PICKUP_F_RAMPAGE) opens a vote on the server instead of being granted, and
// the skull stays where it is on every machine until the vote passes. Then the
// toucher is granted it (PICKUP_F_VOTED), takes it, and the ordinary pickup
// path starts the frenzy everywhere, and everybody else is brought over to
// the toucher. Solo, or with `rampages = off`, the claim is granted as before.
//
// The server holds the whole count (server/core/rampagevote.h): one vote at a
// time, 75% of the players in it rounded up, 15 seconds, the toucher's own yes
// counted from the start. A no ends it only once yes can't get there any more.

enum RampageVoteState : uint8_t {
	RAMPAGE_VOTE_OPEN      = 0,
	RAMPAGE_VOTE_PASSED    = 1,
	RAMPAGE_VOTE_FAILED    = 2,   // not enough yes, or out of time
	RAMPAGE_VOTE_CANCELLED = 3,   // the toucher died or left
};

// Where a vote stands. Broadcast when it opens, on every vote cast, and once
// when it ends. `msLeft` is from when the server sent it; a receiver counts it
// down on its own clock.
struct RampageVoteBody {
	uint8_t  voteId;
	uint8_t  starterId;
	uint8_t  state;     // RampageVoteState
	uint8_t  yes;
	uint8_t  voters;    // who the 75% is taken over
	uint8_t  needed;    // yes votes it takes
	uint16_t msLeft;
};

struct C_RampageVote {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_VOTE;
	PacketHeader hdr;
	uint8_t      voteId;
	uint8_t      yes;   // 1 yes, 0 no
};

struct S_RampageVote {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_VOTE;
	PacketHeader    hdr;
	RampageVoteBody body;
};

// To each player but the toucher, once a vote passes: stand next to them.
// `pos` is where the server last had the toucher. `slot` of `count` is this
// player's place in the ring around them, so two players never land on the
// same spot; the spot itself is worked out by the receiver, on its own ground.
struct RampageTeleportBody {
	uint8_t voteId;
	uint8_t starterId;
	uint8_t slot;
	uint8_t count;
	Vec3    pos;
};

struct S_RampageTeleport {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_TELEPORT;
	PacketHeader        hdr;
	RampageTeleportBody body;
};

// What a receiver did with its S_RampageTeleport. For the server's log only;
// nothing is decided on it.
enum RampageArrival : uint8_t {
	RAMPAGE_ARRIVED          = 0,
	RAMPAGE_ARRIVED_LOCKED   = 1,   // moved, onto an island its story hasn't opened
	RAMPAGE_SKIPPED_DEAD     = 2,
	RAMPAGE_SKIPPED_ARRESTED = 3,
	RAMPAGE_SKIPPED_CUTSCENE = 4,
	RAMPAGE_SKIPPED_MISSION  = 5,
	RAMPAGE_SKIPPED_NO_PED   = 6,
};

struct C_RampageArrived {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_ARRIVED;
	PacketHeader hdr;
	uint8_t      voteId;
	uint8_t      result;   // RampageArrival
};

// One car wreck that counted toward the rampage on the machine that decided
// it.
//
// Why a car isn't sent the way a kill is: CDarkel::RegisterCarBlownUpByPlayer
// (0x00421070) has no culprit test and neither does its call in
// CAutomobile::BlowUpCar (0x0053BF04), so every machine that holds a copy of
// the car registers the wreck when it replays it. Report-and-relay on top of
// that would count the same car twice on every screen that had it. So the
// machine that decided the wreck reports it, the replays are kept off the
// counter (game/darkel.cpp), and everybody counts it once off this packet.
//
// Two kinds of car can be decided by more than one machine at once, because
// every machine damages its own copy: a parked car out of a car generator and
// a session car nobody is driving. Those carry `key`, the same kind and id
// C_UnownedBlowUp would name them by, and the server and every client count a
// key once per frenzy. Everything else - our own car, traffic we host, a car
// no other machine has - has exactly one machine that can decide it and goes
// as RAMPAGE_CAR_UNKEYED.
//
// `model` is the only thing the receiving engine's test looks at
// (`movsx eax,word [ebx+5Ch]` at 0x00421088), and there's no weapon because
// the car register never reads one: a rocket rampage counts a car that burned
// out from gunfire just the same.
constexpr uint8_t RAMPAGE_CAR_UNKEYED = 0xFF;

inline bool RampageCarKeyed(const UnownedVehicleKey &key) {
	return key.kind == UNOWNED_PARKED || key.kind == UNOWNED_SESSION;
}

inline bool SameUnownedKey(const UnownedVehicleKey &a, const UnownedVehicleKey &b) {
	return a.kind == b.kind && a.id == b.id;
}

struct RampageCarBody {
	uint16_t          frenzyId;
	uint16_t          model;
	UnownedVehicleKey key;
};

struct C_RampageCar {
	static constexpr uint8_t OPCODE = OP_C_RAMPAGE_CAR;
	PacketHeader   hdr;
	RampageCarBody body;
};

// To everybody but the reporter, whose own engine already counted it.
// `byPlayer` is the machine that decided the wreck, which is not always the
// player who caused it; it's for the log.
struct S_RampageCar {
	static constexpr uint8_t OPCODE = OP_S_RAMPAGE_CAR;
	PacketHeader   hdr;
	uint8_t        byPlayer;
	RampageCarBody body;
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

// ---- the scripted gates (CH_EVENT) ------------------------------------------
//
// Not garages. main.scm's GATES thread starts seven threads (gates.sc), one
// per gate object init.sc creates: the two Staunton police HQ gates, the
// Colombians' two, Phil's, the fish factory's and the dog-food factory's.
// Each wakes once a second and slides its gate open for the *local* player in
// the right car or the right box and shut when he is in the zone but not the
// box - so a gate open on one screen was shut on the teammate's, who drove
// into it. They are main-script threads, so the mission replay never saw
// them (its SLIDE_OBJECT entry replays mission scripts only).
//
// The same union as the garages: one bit per gate, "my own GATES thread wants
// this gate open", and every machine holds a gate open while anybody's bit is
// set (client/src/game/gates.h). A gate is its index in GATE_COUNT's table,
// named there by the positions gates.sc slides it between. Sent reliably on
// change, relayed, and replayed to a joiner.
constexpr uint8_t GATE_COUNT = 7;

struct GateMaskBody {
	uint8_t open;     // bit i: gate i
	uint8_t pad[3];
};

struct C_GateState {
	static constexpr uint8_t OPCODE = OP_C_GATE_STATE;
	PacketHeader hdr;
	GateMaskBody body;
};

struct S_GateState {
	static constexpr uint8_t OPCODE = OP_S_GATE_STATE;
	PacketHeader hdr;
	uint8_t      playerId;
	uint8_t      pad[3];
	GateMaskBody body;
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
//   3. **The wanted level.** No field for it: every respray is a clear,
//      because the engine's respray arm calls CWanted::Reset whatever the
//      payer had (0x004226FB). The payer's own engine does that on the
//      payer's machine. A receiver clears its own stars only when the
//      session's wanted rule says the respray reaches it - always under
//      `shared`, under `perplayer` only when it is sitting in the car
//      `vehicleNetId` names - and it does so from Client::TickWanted through
//      CPlayerPed::SetWantedLevel(0), never by letting its own garage arm
//      run, which client/src/game/garage.cpp still refuses (docs/wanted.md
//      §4.9).
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

// Each slot's round trip to the server as ENet measures it, in milliseconds,
// PING_NONE for a slot nobody is in. Broadcast once a second; a client only
// ever draws it. What the server measures is the one number every machine can
// agree on for everybody, the machine it describes included.
constexpr uint16_t PING_NONE = 0xFFFF;
constexpr uint16_t PING_MAX  = 0xFFFE;

struct S_PlayerPings {
	static constexpr uint8_t OPCODE = OP_S_PLAYER_PINGS;
	PacketHeader hdr;
	uint16_t     rttMs[MAX_PLAYERS];
};

// ---------------------------------------------------------------------------
// The server's password
// ---------------------------------------------------------------------------
//
// A server somebody runs for their friends on the open internet needs a way to
// keep strangers out, and a hello carries no room for one. So a client with a
// password sends C_Password right after its hello, on the same reliable
// ordered channel, and a server that has one holds the hello until it comes:
// the right one and the hello goes on as it always did, the wrong one or none
// within PASSWORD_WAIT_MS and the welcome says REJECT_BAD_PASSWORD. A server
// without a password ignores it. It crosses the network as it is typed, so it
// is a gate against strangers and nothing more.

constexpr size_t   PASSWORD_LEN     = 32;
constexpr uint32_t PASSWORD_WAIT_MS = 3000;

struct C_Password {
	static constexpr uint8_t OPCODE = OP_C_PASSWORD;
	PacketHeader hdr;
	char         password[PASSWORD_LEN];   // not necessarily terminated
};

// ---------------------------------------------------------------------------
// The host's kick
// ---------------------------------------------------------------------------
//
// The server's window could always throw a player out, and a server run with
// --nogui never could. A mission waits for every player at its start
// (docs/missions.md 9), so somebody who never comes has to be thrown out from
// inside the game, by the host (Session::HostId). The client sends this when
// its player is the host; the server checks that again, because the client's
// word for it is only a claim, and kicks through the same path its window
// uses, so everybody reads "was kicked".

struct C_Kick {
	static constexpr uint8_t OPCODE = OP_C_KICK;
	PacketHeader hdr;
	uint8_t      playerId;   // who goes
};

// ---------------------------------------------------------------------------
// The lobby (docs/protocol.md 1.31)
// ---------------------------------------------------------------------------
//
// A new game has everybody starting at once, and nobody's game is running
// yet to connect with. So the launcher connects instead, before any game:
// it waits in the server's lobby, sees who else is waiting and who is
// already playing, and the lobby's host, the one who has waited longest,
// starts everybody's game at once. Every launcher in the lobby then starts
// its own gta3.exe and goes, and each game connects as it always does. A
// lobby member is no player: it holds no slot, has no ped and hears nothing
// of the session but this.
//
// Not a version. An older server drops C_LobbyJoin, and the launcher, hearing
// no answer, says so and leaves the player to start their game alone.

// Launchers waiting at once, besides the players in the game.
constexpr uint8_t  LOBBY_MAX              = MAX_PLAYERS;
// How long a start from the host keeps another one out, so one click is one
// start however often it arrives.
constexpr uint32_t LOBBY_START_COOLDOWN_MS = 5000;

// What the lobby's host starts everybody's game into.
enum LobbyStartMode : uint8_t {
	LOBBY_START_MENU     = 1,   // the menu: each player loads a save of their own, or picks
	LOBBY_START_NEW_GAME = 2,   // a new game, straight past the menu
};

// A launcher asking into the lobby. The password is the server's, if it has
// one (C_Password's), and travels as typed.
struct C_LobbyJoin {
	static constexpr uint8_t OPCODE = OP_C_LOBBY_JOIN;
	PacketHeader hdr;
	uint16_t     protocolVersion;
	char         nick[NICK_LEN];
	char         password[PASSWORD_LEN];
};

// The answer. Refused with the S_Welcome reasons: another version, a full
// lobby, or the wrong password, and the server hangs up behind it.
struct S_LobbyAnswer {
	static constexpr uint8_t OPCODE = OP_S_LOBBY_ANSWER;
	PacketHeader hdr;
	uint8_t      reject;            // RejectReason
	uint8_t      lobbyId;           // this launcher's place in the lobby
	uint16_t     protocolVersion;   // the server's
};

enum LobbyEntryFlags : uint8_t {
	LOBBY_ENTRY_HOST    = 1 << 0,   // may start everybody's game
	LOBBY_ENTRY_PLAYING = 1 << 1,   // in the game already, not in the lobby
};

struct LobbyEntry {
	uint8_t id;      // a lobby id, or a player id for somebody playing
	uint8_t flags;   // LobbyEntryFlags
	char    nick[NICK_LEN];
};

// Who is there: the lobby, longest waiting first, then everybody already in
// the game. To every lobby member whenever either changes.
struct S_Lobby {
	static constexpr uint8_t OPCODE = OP_S_LOBBY;
	PacketHeader hdr;
	uint8_t      count;         // entries used
	uint8_t      hostLobbyId;   // the lobby id of its host, INVALID_PLAYER with nobody waiting
	uint8_t      waiting;       // how many of the entries are the lobby's
	uint8_t      pad;
	LobbyEntry   entries[LOBBY_MAX + MAX_PLAYERS];
};

// The lobby's host starting everybody's game. Taken from the host alone.
struct C_LobbyStart {
	static constexpr uint8_t OPCODE = OP_C_LOBBY_START;
	PacketHeader hdr;
	uint8_t      mode;   // LobbyStartMode
	uint8_t      pad[3];
};

// To everybody in the lobby, the host included: start your game now.
struct S_LobbyStart {
	static constexpr uint8_t OPCODE = OP_S_LOBBY_START;
	PacketHeader hdr;
	uint8_t      mode;        // LobbyStartMode
	uint8_t      byLobbyId;   // the host who started it
	uint8_t      pad[2];
};

// ---------------------------------------------------------------------------
// The session's one mission
// ---------------------------------------------------------------------------
//
// docs/missions.md is the design and docs/protocol.md 1.29 the wire. Every
// machine runs its own main.scm, so every machine's triggers see its own
// player walk into a marker. The session adds the owner's rule: one mission
// at a time, with everybody in it. The machine that starts a mission owns
// and runs it; every other player is a participant.
//
// **Starting is a claim.** A start gate is the last check a trigger makes
// before its START_MISSION: CAN_PLAYER_START_MISSION for a contact or a
// payphone, the sub-mission button for an odd job. While its player keeps
// satisfying one, a machine sends C_MissionClaim every
// MISSION_CLAIM_REFRESH_MS with the start's area, and answers the gate false
// until S_MissionClaim says GRANTED. The server grants the slot when it is
// free and every other player is inside the area or within the session's
// margin of it (coopiii/mission.h, InMissionArea). Until then it answers
// WAITING with who is missing, and S_MissionWaiting tells everybody the
// same, so the players who are missing know they are waited for. A claim
// nobody refreshes lapses after MISSION_CLAIM_TTL_MS: its player walked away.
//
// **Then it runs.** When the owner's START_MISSION runs, C_MissionStarted
// names the mission and S_MissionState tells everybody, a joiner included.
// When it ends, C_MissionEnded says how and S_MissionState goes back to
// idle with that outcome. A participant who dies or is busted with the
// server's missionFailOnDeath on earns the owner an S_MissionFail, and the
// owner's script fails the mission the way it would for the owner's own
// death. The owner leaving fails it for everybody (missions.md 12.3).
//
// **Checkpoints.** A mission's location check whose area holds one of its
// coordinate blips waits for everybody too. The owner's machine has every
// player's position already and decides that itself; C_MissionCheckpoint
// only tells the others who is still missing, through S_MissionWaiting.

constexpr uint32_t MISSION_CLAIM_TTL_MS      = 2000;
constexpr uint32_t MISSION_CLAIM_REFRESH_MS  = 500;
// How far outside a start or a checkpoint still counts as there, unless the
// server says otherwise. 5 m, decided 2026-09-24 (missions.md 9).
constexpr uint16_t MISSION_MARGIN_CM_DEFAULT = 500;
// No mission, or one the claim does not know the number of yet.
constexpr uint16_t MISSION_NONE              = 0xFFFF;

// PlayerStateBody::pedState for a player the police have arrested
// (PED_ARRESTED, which only the respawn at the police station takes off
// again). The server reads it for the death rule, since a bust is the other
// half of it (missions.md 12.2). client.h's WIRE_PEDSTATE_ARRESTED is held to it.
constexpr uint8_t PEDSTATE_ON_WIRE_ARRESTED = 56;

enum MissionKind : uint8_t {
	MISSION_KIND_STORY  = 0,   // a contact or a payphone
	MISSION_KIND_ODDJOB = 1,   // paramedic, firefighter, vigilante, taxi
	MISSION_KIND_RC     = 2,   // the RC toy van, and Toyminator's
	MISSION_KIND_4X4    = 3,   // the three 4x4 runs and Multistorey Mayhem
};

// The engine's locates are boxes: a centre and a half-size on each axis. A
// 2D one ignores height. An odd job has no area at all, since its button
// works anywhere, so its area is the owner: `centre` is where they are.
enum MissionAreaShape : uint8_t {
	MISSION_AREA_BOX2D = 0,
	MISSION_AREA_BOX3D = 1,
	MISSION_AREA_OWNER = 2,
};

struct MissionArea {
	Vec3    centre;
	Vec3    half;     // unused for MISSION_AREA_OWNER
	uint8_t shape;    // MissionAreaShape
	uint8_t pad[3];
};

struct C_MissionClaim {
	static constexpr uint8_t OPCODE = OP_C_MISSION_CLAIM;
	PacketHeader hdr;
	// Names the gate, so a claim is refreshed rather than repeated: the
	// asking script's instruction pointer at the gate. Opaque to the server.
	uint32_t     launchKey;
	uint16_t     missionHint;   // the mission it launches, or MISSION_NONE
	uint8_t      kind;          // MissionKind
	uint8_t      pad;
	MissionArea  area;
};

enum MissionClaimVerdict : uint8_t {
	MISSION_CLAIM_GRANTED = 0,
	MISSION_CLAIM_WAITING = 1,   // everybody is not there yet: `missingMask`
	MISSION_CLAIM_BUSY    = 2,   // somebody else's claim or mission: `ownerId`
};

// To the claimant alone.
struct S_MissionClaim {
	static constexpr uint8_t OPCODE = OP_S_MISSION_CLAIM;
	PacketHeader hdr;
	uint32_t     launchKey;
	uint8_t      verdict;       // MissionClaimVerdict
	uint8_t      ownerId;       // whose it is, for BUSY; the claimant otherwise
	uint8_t      missingMask;   // bit i: player i is not there yet
	uint8_t      pad;
};

enum MissionWaitKind : uint8_t {
	MISSION_WAIT_NONE       = 0,   // nobody is waited for any more
	MISSION_WAIT_START      = 1,
	MISSION_WAIT_CHECKPOINT = 2,
};

// To everybody, whenever who is waited for changes.
struct S_MissionWaiting {
	static constexpr uint8_t OPCODE = OP_S_MISSION_WAITING;
	PacketHeader hdr;
	uint8_t      ownerId;       // who is waiting
	uint8_t      missingMask;   // for whom
	uint8_t      what;          // MissionWaitKind
	// Of missingMask, the ones whose game is in a mission of its own, a new
	// game's intro say (C_MissionBusy). A start only.
	uint8_t      busyMask;
	uint16_t     missionHint;   // MISSION_NONE when it is not known
	// Seconds from now until the wait goes on without whoever it is still
	// for; 0 when it does not. A start does only when the busyMask ones are
	// all it still waits for; a checkpoint always does, after
	// MISSION_CHECKPOINT_WAIT_MS from when it began to wait.
	uint16_t     goesOnInS;
	Vec3         where;         // the start or the checkpoint
};

// A start that waits only for players whose game is in a mission of its own
// goes on without them after this long. Their game may be in a two-minute
// intro nobody else can end for them, or stuck in one; they come into the
// running mission as a joiner does once it is over. Somebody who is simply
// somewhere else is still waited for, and is the host's to kick.
constexpr uint32_t MISSION_BUSY_WAIT_MS = 60000;

// A checkpoint waits this long for the participants who are not in it, and
// then the owner's mission goes on without them. Somebody across the map, or
// stuck behind a wall, would otherwise hold it for ever while the owner stands
// in it and the mission's own timer runs out. The owner's machine decides it
// (MissionSync::AskCheckpoint); the server counts the same time down for
// everybody's screen (S_MissionWaiting::goesOnInS).
constexpr uint32_t MISSION_CHECKPOINT_WAIT_MS = 60000;

// The server's own numbers for the rest of it, all in S_MissionState, with
// the defaults every build before them had hardcoded on the client:
//
//   checkpointWaitS  MISSION_CHECKPOINT_WAIT_MS in seconds; 0 and no
//                    checkpoint waits at all, and whoever falls behind is
//                    brought along instead, as in a race
//   catchUpM         a participant who comes into the mission late, or back
//                    from the hospital or the police station, and is further
//                    than this from the owner is brought beside them; 0 never
//   behindM, behindS in a mission whose checkpoints do not wait, a
//                    participant further than behindM from the owner for
//                    behindS seconds is brought beside them; 0 m never
//
// The ranges are the server's to keep (config.h); a receiver takes whatever
// arrives, capped at these maxima.
constexpr uint16_t MISSION_CHECKPOINT_WAIT_S_MAX     = 600;
constexpr uint16_t MISSION_CATCH_UP_M_DEFAULT        = 60;
constexpr uint16_t MISSION_BEHIND_M_DEFAULT          = 150;
constexpr uint16_t MISSION_BEHIND_S_DEFAULT          = 10;
constexpr uint16_t MISSION_DISTANCE_M_MAX            = 2000;
constexpr uint16_t MISSION_BEHIND_S_MAX              = 300;

struct C_MissionStarted {
	static constexpr uint8_t OPCODE = OP_C_MISSION_STARTED;
	PacketHeader hdr;
	uint32_t     launchKey;
	uint16_t     missionNumber;   // START_MISSION's operand
	uint16_t     pad;
};

enum MissionState : uint8_t {
	MISSION_STATE_IDLE    = 0,
	MISSION_STATE_RUNNING = 1,
};

enum MissionOutcome : uint8_t {
	MISSION_OUTCOME_NONE       = 0,
	MISSION_OUTCOME_PASSED     = 1,
	MISSION_OUTCOME_FAILED     = 2,
	MISSION_OUTCOME_OWNER_LEFT = 3,   // missions.md 12.3: it fails for everybody
};

// S_MissionState::flags
constexpr uint8_t MISSION_FLAG_FAIL_ON_DEATH = 0x01;   // the server's missionFailOnDeath
// The server's missionTimedCheckpoints: a checkpoint waits for everybody even
// while a countdown is on the owner's screen, in a race, an odd job or an RC,
// 4x4 or Mayhem run (CheckpointsWait). Off by default, which is protocol 63's
// rule; the countdown still runs while it waits.
constexpr uint8_t MISSION_FLAG_TIMED_CHECKPOINTS = 0x02;

// How the session's mission's enemies stand up to more than one player
// (docs/missions.md 10), the server's missionEnemies. The owner's machine
// applies it, as its mission gives each enemy a KILL_PLAYER objective.
enum MissionEnemies : uint8_t {
	MISSION_ENEMIES_ORIGINAL = 0,   // single player's, exactly
	MISSION_ENEMIES_TOUGHER  = 1,   // their health and armour grow with the players
	MISSION_ENEMIES_MORE     = 2,   // tougher, and copies beside them (§10.5)
};
// The server's missionScale: what each participant after the first adds to
// an enemy, in percent. 50 makes two players' enemies 1.5 times as tough.
constexpr uint16_t MISSION_SCALE_DEFAULT = 50;
constexpr uint16_t MISSION_SCALE_MAX     = 200;
// With `more`, each generic enemy gets one copy for each player past the
// first, at most this many (docs/missions.md 10.5).
constexpr uint8_t  MISSION_ENEMY_COPIES_MAX = 3;

// To everybody on every change, and to a joiner in the backfill. Idle with an
// outcome is the end of the mission `missionNumber`.
struct S_MissionState {
	static constexpr uint8_t OPCODE = OP_S_MISSION_STATE;
	PacketHeader hdr;
	uint8_t      state;          // MissionState
	uint8_t      ownerId;
	uint16_t     missionNumber;
	uint8_t      participants;   // bit i: player i is in it
	uint8_t      flags;          // MISSION_FLAG_*
	uint8_t      outcome;        // MissionOutcome, for the mission that just ended
	uint8_t      enemies;        // MissionEnemies
	uint16_t     marginCm;       // how far outside an area still counts as there
	uint16_t     scalePct;       // missionScale
	// Which run of the server this is, as its campaign log names itself
	// (C_CampaignSince). Another one than last time is a server that has
	// started over, and so has its log.
	uint32_t     campaignLog;
	// See MISSION_CHECKPOINT_WAIT_S_MAX above for what each of these is.
	uint16_t     checkpointWaitS;
	uint16_t     catchUpM;
	uint16_t     behindM;
	uint16_t     behindS;
};

struct C_MissionEnded {
	static constexpr uint8_t OPCODE = OP_C_MISSION_ENDED;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint8_t      outcome;        // MISSION_OUTCOME_PASSED or _FAILED
	uint8_t      pad;
};

enum MissionFailReason : uint8_t {
	MISSION_FAIL_DIED   = 0,
	MISSION_FAIL_BUSTED = 1,
};

// To the owner alone: fail it, the way the engine does for the owner's death.
struct S_MissionFail {
	static constexpr uint8_t OPCODE = OP_S_MISSION_FAIL;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint8_t      reason;         // MissionFailReason
	uint8_t      playerId;       // who died or was busted
};

// The owner, at a checkpoint the mission is waiting at for somebody. A
// missingMask of 0 is the wait being over.
struct C_MissionCheckpoint {
	static constexpr uint8_t OPCODE = OP_C_MISSION_CHECKPOINT;
	PacketHeader hdr;
	uint8_t      missingMask;
	uint8_t      pad[3];
	Vec3         where;
};

// **What the mission shows** travels as the instructions that show it. The
// owner's engine runs its mission; each instruction of it on a short list
// (game/replay.h: the words on the screen, the pay and the stats, the blips,
// the HUD's timers and counters, the cutscenes, the fades and the camera,
// the player's moves) goes out as C_MissionEffect with its operands as the
// owner's engine read them, and every participant's own interpreter runs it.
// A blip or a cutscene object is the owner's handle on the wire and each
// participant's own on its machine: a BLIP_NEW or an OBJECT_NEW says which of
// the owner's the new one stands for, and an instruction that uses one names
// the owner's. A TELEPORT is SET_PLAYER_COORDINATES, which every participant
// runs on its own player a little way from where the owner is put. The
// server relays one only from the running mission's owner, or from the
// player whose start it has just granted, whose trigger prints the mission's
// title.

constexpr size_t MISSION_EFFECT_CODE = 64;

enum MissionEffectKind : uint8_t {
	MISSION_EFFECT_RUN      = 0,
	MISSION_EFFECT_BLIP_NEW = 1,   // run it; its result is ours for `ownerBlip`
	MISSION_EFFECT_BLIP_USE = 2,   // `ownerBlip` at code[handleAt], ours in its place
	MISSION_EFFECT_PAY      = 3,   // ADD_SCORE: not under a shared wallet
	MISSION_EFFECT_OBJECT_NEW = 4, // run it; its result is ours for `ownerBlip`
	// SET_PLAYER_COORDINATES, beside the owner's spot. `ownerBlip` says where
	// the owner's player sat: 0 on foot, else 1 + the netId of its car (1 for
	// a car the session has no name for). A participant in a car moves it only
	// when the owner was in one and its own machine simulates that car.
	MISSION_EFFECT_TELEPORT   = 5,
	MISSION_EFFECT_PICKUP_NEW = 6, // run it; the pickup it made is ours for `ownerBlip`
	MISSION_EFFECT_FIRE_NEW   = 7, // run it; the script fire it lit is ours for `ownerBlip`
	MISSION_EFFECT_SPHERE_NEW = 8, // run it; the sphere it put up is ours for `ownerBlip`
};

struct MissionEffectBody {
	uint16_t missionNumber;   // MISSION_NONE for a title before the launch
	uint8_t  kind;            // MissionEffectKind
	uint8_t  length;          // bytes of `code` in use
	uint8_t  handleAt;        // BLIP_USE: where the handle's four bytes are
	// 0 for everybody; a player id plus one for that player alone, which is
	// how the owner hands a participant who has just come back what the
	// mission still has up: its blips, pickups, fires, objects and widgets.
	// Zero means everybody so that a body left zeroed goes to everybody.
	uint8_t  onlyTo;
	// Nonzero on the owner's LOAD_ALL_MODELS_NOW, numbered from 1 in each
	// mission: every participant says with C_MissionReady that it has run
	// it, and the owner's mission waits for its models until everybody has.
	uint16_t readySeq;
	int32_t  ownerBlip;
	// One instruction: the opcode, then every operand a literal, a text
	// label, a global by offset or the runner's local 0.
	uint8_t  code[MISSION_EFFECT_CODE];
};

struct C_MissionEffect {
	static constexpr uint8_t OPCODE = OP_C_MISSION_EFFECT;
	PacketHeader      hdr;
	MissionEffectBody body;
};

struct S_MissionEffect {
	static constexpr uint8_t OPCODE = OP_S_MISSION_EFFECT;
	PacketHeader      hdr;
	uint8_t           ownerId;
	uint8_t           pad[3];
	MissionEffectBody body;
};

// **A widget's value** (mission-audit.md R16). DISPLAY_ONSCREEN_TIMER and the
// counters register a global, and the HUD reads it out of the script space
// every frame (and counts a timer down itself). The instruction is replayed,
// so a participant's HUD shows the widget, reading the participant's copy of
// that global, which only this keeps up to date: the owner sends its value
// whenever it is not what the participant's copy would be by now, a counter
// on every change and a timer when it stops following its own countdown (a
// bonus, a pause), every two seconds at least, and ten times a second at
// most. The participant writes it into the same global, and only into one
// its own HUD is showing.
constexpr uint32_t MISSION_WIDGET_MIN_MS    = 100;
constexpr uint32_t MISSION_WIDGET_RESYNC_MS = 2000;
constexpr int32_t  MISSION_WIDGET_DRIFT_MS  = 250;

struct C_MissionWidget {
	static constexpr uint8_t OPCODE = OP_C_MISSION_WIDGET;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     offset;   // the global's, in the script space
	int32_t      value;
};

struct S_MissionWidget {
	static constexpr uint8_t OPCODE = OP_S_MISSION_WIDGET;
	PacketHeader hdr;
	uint8_t      ownerId;
	uint8_t      pad;
	uint16_t     missionNumber;
	uint16_t     offset;
	uint16_t     pad2;
	int32_t      value;
};

// **Everybody's models are in** (docs/missions.md 11.3). A cutscene is every
// machine's own playback, of models every machine loads for itself, and the
// owner's mission loads them with LOAD_ALL_MODELS_NOW and waits in a loop on
// HAS_MODEL_LOADED and HAS_SPECIAL_CHARACTER_LOADED before it starts. Its
// LOAD_ALL_MODELS_NOW goes out with a `readySeq`, a participant's replay of it
// loads everything the mission asked for before anything after it runs, and
// the participant then says so with C_MissionReady, which the server passes to
// the owner alone. Until every participant has, and for no longer than
// MISSION_READY_WAIT_MS, the owner's mission is held in its own wait, so
// nobody's cutscene starts behind everybody else's. A participant says it
// whether it could load anything or not: it is only ever "go ahead".
constexpr uint32_t MISSION_READY_WAIT_MS = 5000;

struct C_MissionReady {
	static constexpr uint8_t OPCODE = OP_C_MISSION_READY;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     readySeq;
};

struct S_MissionReady {
	static constexpr uint8_t OPCODE = OP_S_MISSION_READY;
	PacketHeader hdr;
	uint8_t      playerId;   // the participant who is ready
	uint8_t      pad;
	uint16_t     missionNumber;
	uint16_t     readySeq;
	uint16_t     pad2;
};

// **The seats a mission's passengers need** (mission-audit.md R4). In 25
// missions one of the mission's pedestrians gets into the owner's car:
// Misty, the bank manager, the patients and the fares with
// SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER, and the thugs, the girls and the
// others who follow the player, whose engine sends them to the player's car
// the same way. A participant riding in it can sit in the seat the mission
// needs, and the pedestrian then waits beside the car for ever. So while one
// of the owner's mission pedestrians is heading for a session car's seats,
// the owner's machine says so: nobody else sits down in it, and when the
// seats its engine has free are fewer than the pedestrians heading for them,
// only as many of the players riding in it as it takes get out, the ones the
// owner's machine names in `leave`: the players whose copies sit in its
// passenger seats there, from the last seat forward. The whole list goes each
// time it changes and every MISSION_SEATS_RESYNC_MS while it is not empty,
// and an empty one ends it. Taken from the running mission's owner alone and
// relayed to everybody else.
constexpr uint8_t  MISSION_SEAT_CARS        = 4;
constexpr uint32_t MISSION_SEATS_RESYNC_MS  = 2000;

enum MissionSeatFlags : uint8_t {
	MISSION_SEAT_KEPT  = 1u << 0,   // nobody else sits down in it
	MISSION_SEAT_LEAVE = 1u << 1,   // and the players in `leave` get out
};

struct MissionSeatCar {
	uint16_t netId;   // the session's car
	uint8_t  flags;   // MissionSeatFlags
	// Bit n for player n, who is to get out under MISSION_SEAT_LEAVE. 0 with
	// the flag set is everybody riding in it, which is what an owner of
	// protocol 54 or older sends.
	uint8_t  leave;
};

// **Everybody into the car the mission puts its player in.** Last Requests
// puts its player into the Reefer with WARP_PLAYER_INTO_CAR, Chaperone into
// Maria's Stretch, Cipriani's Chauffeur into Toni's car, and the mission then
// drives off with nothing left for anybody else to follow in, a boat least
// of all. So once a warp of the owner's mission has put the owner's player in
// a car the session names, the owner's machine hands the participants on
// foot the passenger seats its engine has free there, one each, nearest
// first, keeping back one for each of the mission's pedestrians heading for
// that car. Each participant given a seat is put straight into it; nobody
// else is moved. `seats[n]` is player n's wire seat (1 + passenger slot), 0
// for none. Under MISSION_BOARD_WATER the car is a boat: whoever has no seat
// in it can't follow it anywhere, so while the owner is in it the owner's
// checkpoints stop waiting for them (docs/missions.md 11.4). Taken from the
// running mission's owner alone and relayed to everybody else.
constexpr uint32_t MISSION_BOARD_WAIT_MS = 5000;

enum MissionBoardFlags : uint8_t {
	MISSION_BOARD_WATER = 1u << 0,
};

struct C_MissionBoard {
	static constexpr uint8_t OPCODE = OP_C_MISSION_BOARD;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     netId;               // the session's car
	uint8_t      seats[MAX_PLAYERS];  // wire seat per player, 0 for none
	uint8_t      flags;               // MissionBoardFlags
	uint8_t      pad;
};

struct S_MissionBoard {
	static constexpr uint8_t OPCODE = OP_S_MISSION_BOARD;
	PacketHeader hdr;
	uint8_t      ownerId;
	uint8_t      flags;
	uint16_t     missionNumber;
	uint16_t     netId;
	uint8_t      seats[MAX_PLAYERS];
};

struct C_MissionSeats {
	static constexpr uint8_t OPCODE = OP_C_MISSION_SEATS;
	PacketHeader   hdr;
	uint16_t       missionNumber;
	uint8_t        count;   // cars in use, at most MISSION_SEAT_CARS
	uint8_t        pad;
	MissionSeatCar cars[MISSION_SEAT_CARS];
};

struct S_MissionSeats {
	static constexpr uint8_t OPCODE = OP_S_MISSION_SEATS;
	PacketHeader   hdr;
	uint8_t        ownerId;
	uint8_t        count;
	uint16_t       missionNumber;
	MissionSeatCar cars[MISSION_SEAT_CARS];
};

// **One of the mission's objects broke** (mission-audit.md R3). The objects
// the mission makes are every machine's own, each in the same global, and the
// map's own identity (ObjectIdent, where the map put it) cannot name them. So
// a break that a machine's own player or car did to its copy, or that nobody
// did, on the owner's, goes out named by that global, with the state and the
// amount ObjectDamage was run with, and every other machine runs the engine's
// own damage on the object its global holds until it is as broken
// (OnObjectBrokenElsewhere's rule). That is how the owner's
// HAS_OBJECT_BEEN_DAMAGED sees a stall a participant drove through in
// Espresso-2-Go. A blast breaks every copy by itself and is not sent. Taken
// from anybody in the running mission and relayed to everybody else.
struct C_MissionObjectBreak {
	static constexpr uint8_t OPCODE = OP_C_MISSION_OBJECT_BREAK;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     global;   // the offset of the global that holds it, everywhere
	float        amount;   // what ObjectDamage was run with
	uint8_t      state;    // OBJ_BREAK_*, as ObjectBreakBody::state
	uint8_t      pad[3];
};

struct S_MissionObjectBreak {
	static constexpr uint8_t OPCODE = OP_S_MISSION_OBJECT_BREAK;
	PacketHeader hdr;
	uint8_t      playerId;   // whose machine it broke on
	uint8_t      state;
	uint16_t     missionNumber;
	uint16_t     global;
	uint16_t     pad;
	float        amount;
};

// **One of the mission's floating packages taken** (mission-audit.md R2). A
// Drop In The Ocean's packages fall from a plane and float where they land,
// so the pickup sync, which names a pickup by where it is, leaves them to each
// machine. Every machine makes its own (CREATE_FLOATING_PACKAGE is replayed),
// and a machine whose copy is taken says so, named by the owner's handle for
// it: the owner's machine then takes its own the way the engine does and
// pushes it into the ring HAS_PICKUP_BEEN_COLLECTED reads, so the mission
// counts it, and everybody else's copy goes. Taken from anybody in the
// running mission and relayed to everybody else.
struct C_MissionPickup {
	static constexpr uint8_t OPCODE = OP_C_MISSION_PICKUP;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     pad;
	int32_t      handle;   // the owner's, as the mission holds it
};

struct S_MissionPickup {
	static constexpr uint8_t OPCODE = OP_S_MISSION_PICKUP;
	PacketHeader hdr;
	uint8_t      playerId;   // whose machine it was taken on
	uint8_t      pad;
	uint16_t     missionNumber;
	int32_t      handle;
};

// **A game of its own** (docs/missions.md 11.6). A new game starts with the
// intro, a mission of that machine's own, and the player stands on the first
// mission's marker all through its opening scene, invisible. So a player
// whose game runs a mission that is not the session's says so, and the server
// counts them as nowhere: missing at a start, and out of the running mission
// until theirs is over, when they come into it like a joiner and are handed
// what it has up. Their machine runs nothing of the session's mission
// meanwhile. The owner going into one, a new game in the middle of theirs,
// ends the session's mission as their leaving would.
//
// `fresh` says this machine's game has started over, a load or a new game,
// so whatever of the running mission it had is gone. The server has the owner
// hand it over again (S_MissionHandOver), to that player alone.
struct C_MissionBusy {
	static constexpr uint8_t OPCODE = OP_C_MISSION_BUSY;
	PacketHeader hdr;
	uint8_t      busy;    // 1 while a mission of this machine's own runs
	uint8_t      fresh;   // 1 once, when this machine's game has started over
};

struct S_MissionHandOver {
	static constexpr uint8_t OPCODE = OP_S_MISSION_HAND_OVER;
	PacketHeader hdr;
	uint8_t      playerId;   // whose game started over
	uint8_t      pad;
	uint16_t     missionNumber;
};

// **Every pedestrian and car of the running mission, again** (docs/missions.md
// 11.5). What the mission shows comes to somebody who comes in late through
// S_MissionHandOver; what it made is the owner's hosted traffic, which reaches
// a joiner in the backfill and everybody else as it is made. A participant
// whose game was in a mission of its own when the owner's started, came back
// from a dropped connection, or started over asks for it all again: the
// server answers with an S_PedSpawn and an S_CarSpawn, tempId 0, for every
// AMBIENT_MISSION pedestrian and car it has from somebody else, an S_PedDeath
// for each of those lying dead, and then S_MissionCatchUp saying how many.
// The receiver skips what it has already.
struct C_MissionCatchUp {
	static constexpr uint8_t OPCODE = OP_C_MISSION_CATCH_UP;
	PacketHeader hdr;
	uint16_t     missionNumber;   // as the asker last heard it
};

struct S_MissionCatchUp {
	static constexpr uint8_t OPCODE = OP_S_MISSION_CATCH_UP;
	PacketHeader hdr;
	uint8_t      ownerId;         // the running mission's owner, INVALID_PLAYER for none
	uint8_t      pad;
	uint16_t     missionNumber;
	uint16_t     cars;            // AMBIENT_MISSION cars sent just before this
	uint16_t     peds;            // AMBIENT_MISSION pedestrians sent just before this
	uint16_t     sessionCars;     // of the mission's cars, the ones somebody has claimed since
};

// **A kill somebody else made** (docs/mission-audit.md R11). Uzi Rider and
// Bait count their kills with GET_NUM_OF_MODELS_KILLED_BY_PLAYER, which reads
// CDarkel::RegisteredKills on the owner's machine alone. A kill a
// participant's machine registers while the session's mission runs goes to
// the owner, whose counter moves by one for the victim's model: a
// pedestrian it hosts that its player killed, or that somebody else's hit
// killed there. The owner's own kills, and the ones the owner's machine
// registers for somebody else's hit on a pedestrian it hosts, count there as
// they always did.
struct C_MissionKill {
	static constexpr uint8_t OPCODE = OP_C_MISSION_KILL;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     model;   // the victim's model index
};

struct S_MissionKill {
	static constexpr uint8_t OPCODE = OP_S_MISSION_KILL;
	PacketHeader hdr;
	uint8_t      playerId;   // whose machine registered it
	uint8_t      pad;
	uint16_t     missionNumber;
	uint16_t     model;
	uint16_t     pad2;
};

// **What a participant's engine answers** (docs/mission-audit.md R5, R7).
// Some of what the owner's mission asks is decided on every machine, for that
// machine's player: a garage's state machine shuts on the car its own player
// brought and resprays its own player's car, and every machine flies its own
// copy of a mission Cessna for its own player's rocket to bring down. So each
// participant's machine asks its own engine the owner's questions and says
// when the answers change, to the owner alone, whose mission then hears "yes"
// from anybody's engine:
//
// - `hasCar` and `resprayed`, per garage: IS_CAR_IN_MISSION_GARAGE now, and a
//   HAS_RESPRAY_HAPPENED that said yes since the last. A garage is its index in
//   CGarages::aGarages, main.scm making the same ones in the same order
//   everywhere.
// - `shotDown`: a mission plane of the running mission that went down here
//   since the last, MISSION_SHOT_DOWN_*.
constexpr uint8_t MISSION_GARAGES = 32;   // bit i is garage i

enum MissionShotDown : uint16_t {
	MISSION_SHOT_DOWN_DRUG_PLANE    = 1 << 0,   // HAS_DRUG_PLANE_BEEN_SHOT_DOWN, S.A.M.
	MISSION_SHOT_DOWN_DROP_OFF      = 1 << 1,   // HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN
};

struct C_MissionAnswers {
	static constexpr uint8_t OPCODE = OP_C_MISSION_ANSWERS;
	PacketHeader hdr;
	uint16_t     missionNumber;
	uint16_t     shotDown;    // MissionShotDown, since the last
	uint32_t     hasCar;      // IS_CAR_IN_MISSION_GARAGE, true here now
	uint32_t     resprayed;   // HAS_RESPRAY_HAPPENED said true here since the last
};

struct S_MissionAnswers {
	static constexpr uint8_t OPCODE = OP_S_MISSION_ANSWERS;
	PacketHeader hdr;
	uint8_t      playerId;   // whose engine
	uint8_t      pad;
	uint16_t     missionNumber;
	uint16_t     shotDown;
	uint16_t     pad2;
	uint32_t     hasCar;
	uint32_t     resprayed;
};

// ---- skipping a cutscene together -------------------------------------------
//
// CCutsceneMgr::Update skips a cutscene on the machine whose pad asked for it
// and nowhere else, so one player's skip left everybody else's scene running
// and the players came out of it at different moments (docs/missions.md
// 11.3). Each machine now says which cutscene its game is in whenever the
// engine's own skip test would let it be skipped (C_CutsceneState, on
// change), and the server counts everybody in the same one
// (server/core/cutscenevote.h): the skip input casts a yes instead of
// skipping, and 75% of them, rounded up, skips it for all of them at once.
// Alone in a cutscene, a player skips it the way the game always has.
//
// Two players are in the same cutscene when the scope and the name are both
// the same. The session's mission's scene is one cutscene for its owner and
// every participant replaying it. A scene a game plays for its own script, a
// new game's intro say, is shared with anybody playing the same one for
// theirs, so the lobby's new games leave the intro together.
enum CutsceneScope : uint8_t {
	CUTSCENE_SCOPE_NONE   = 0,   // no cutscene the engine would skip
	CUTSCENE_SCOPE_OWN    = 1,   // this game's own script's
	CUTSCENE_SCOPE_SHARED = 2,   // the session's mission's, owner or participant
};

// CCutsceneMgr::ms_cutsceneName as LOAD_CUTSCENE's handler copies it: eight
// bytes, lower case on the wire, not necessarily terminated.
constexpr size_t CUTSCENE_NAME_LEN = 8;

struct CutsceneKey {
	uint8_t scope;                    // CutsceneScope
	char    name[CUTSCENE_NAME_LEN];
};

// Same scope and the same name, without regard to case.
inline bool SameCutscene(const CutsceneKey &a, const CutsceneKey &b) {
	if (a.scope != b.scope)
		return false;
	for (size_t i = 0; i < CUTSCENE_NAME_LEN; ++i) {
		char x = a.name[i], y = b.name[i];
		if (x >= 'A' && x <= 'Z')
			x = static_cast<char>(x - 'A' + 'a');
		if (y >= 'A' && y <= 'Z')
			y = static_cast<char>(y - 'A' + 'a');
		if (x != y)
			return false;
		if (x == '\0')
			return true;
	}
	return true;
}

// On every change of the cutscene the sender's game is in (into a skippable
// one, into another, or out of it: CUTSCENE_SCOPE_NONE with the name zeroed),
// and on a press of skip: `skip` 1 and the `voteId` of the count it is a yes
// in. There is no no: not pressing it is.
struct C_CutsceneState {
	static constexpr uint8_t OPCODE = OP_C_CUTSCENE_STATE;
	PacketHeader hdr;
	CutsceneKey  key;
	uint8_t      skip;     // 1: the skip input, as a yes
	uint8_t      voteId;   // with skip, the count it is cast in
};

// Where the count stands. `voters` below 2 tells the receiver it is alone in
// the cutscene and skips as the game does.
struct CutsceneVoteBody {
	uint8_t     voteId;
	uint8_t     yes;
	uint8_t     voters;
	uint8_t     needed;    // the yes it takes: 75% of voters, rounded up
	uint8_t     yesMask;   // bit n: player n said skip
	CutsceneKey key;
};

enum CutsceneVoteKind : uint8_t {
	CUTSCENE_VOTE_COUNT = 0,   // to everybody in it, whenever somebody comes, goes or says skip
	CUTSCENE_VOTE_SKIP  = 1,   // skip it now, to everybody in it at once
};

// CUTSCENE_VOTE_SKIP: the session's mission moves on with its owner's skip; on
// a participant it only takes the scene to its end, and the owner's
// CLEAR_CUTSCENE ends it there as it always did. It also goes alone to a
// participant who comes into the session's scene a few seconds after it was
// skipped, with `voters` 0.
struct S_CutsceneVote {
	static constexpr uint8_t OPCODE = OP_S_CUTSCENE_VOTE;
	PacketHeader     hdr;
	uint8_t          kind;   // CutsceneVoteKind
	CutsceneVoteBody body;
};

// ---------------------------------------------------------------------------
// What a mission leaves behind (docs/missions.md 5.5)
// ---------------------------------------------------------------------------
//
// Every machine runs the same main.scm, so a mission passed on the owner's
// machine is a set of main-script globals and threads there, and nowhere
// else: everybody else's triggers would offer it again. When the owner's
// mission ends its machine sends every main.scm global the mission wrote with
// a plain assignment (0004/0005 and the arithmetic on them, which is how all
// 99 completion and unlock flags are set) and that it left different from
// what it was before its first write, the main-script threads it started
// (the next mission's trigger, as a passed mission's START_NEW_SCRIPT), and
// each instruction it ran whose effect a save keeps: a parked-car generator
// switched, a building swapped, roads closed, a garage's type (game/replay.h,
// Kind::World). Those also went to everybody as they ran; a part carries one
// of them, and they come first. A global the mission filled with something
// an opcode handed back, a car or a blip, is a handle into that machine's
// pools and is not sent.
//
// The server numbers and keeps every delta, and every client keeps its own
// copy of the log. What a machine's campaign still lacks is worked out
// against its own globals: the newest delta whose every value is already
// there, and everything before it, is in (the owner's own, or a save made
// after it); everything after it is written and its threads are started. A
// load or a new game asks again, and a save from before the session gets the
// whole session's missions. A thread is not started a second time: one that
// names itself (NAME_THREAD, its first instruction) is looked for under that
// name, and one that does not, every mission trigger, carries a name of the
// form "@01A2B3" (its label) on every machine that started it for a mission.
//
// A client asks for the deltas after the last one it has with
// C_CampaignSince when it first hears the session's mission state, so a
// mission passed while it was away or before it came is not missed.
// S_MissionState::campaignLog says which log that is.
//
// Nothing is applied from a machine whose main.scm is not this one:
// `scriptHash` covers the script past its globals, which no save changes.

constexpr size_t CAMPAIGN_VALUES  = 32;
constexpr size_t CAMPAIGN_THREADS = 8;
// What one mission may leave, in parts, and what the server keeps in all. A
// mission of the campaign sends a few; the bounds are for one that doesn't
// stop.
constexpr uint32_t CAMPAIGN_PARTS_PER_MISSION = 128;
constexpr uint32_t CAMPAIGN_LOG_MAX           = 65536;

struct CampaignValue {
	uint16_t offset;   // into the script space, where the global lives
	uint16_t pad;
	int32_t  value;    // its four bytes, an int or a float alike
};

struct CampaignThread {
	int32_t label;     // START_NEW_SCRIPT's operand: where it starts in main.scm
	char    name[8];   // what it names itself (NAME_THREAD at `label`), or nothing
};

struct CampaignDeltaBody {
	uint32_t       seq;             // the server's number for it; 0 from a client
	uint16_t       missionNumber;
	uint8_t        valueCount;
	uint8_t        threadCount;
	uint8_t        last;            // the mission's last part; threads only ride it
	uint8_t        pad[3];
	uint32_t       scriptHash;      // FNV-1a of the owner's main.scm past its globals
	CampaignValue  values[CAMPAIGN_VALUES];
	CampaignThread threads[CAMPAIGN_THREADS];
	uint8_t        opLength;        // an instruction to run, as MissionEffectBody::code
	uint8_t        pad2[3];
	uint8_t        op[MISSION_EFFECT_CODE];
};

// From the running mission's owner, as its mission ends.
struct C_CampaignDelta {
	static constexpr uint8_t OPCODE = OP_C_CAMPAIGN_DELTA;
	PacketHeader      hdr;
	CampaignDeltaBody body;
};

// To everybody, the owner too, who only notes the number.
struct S_CampaignDelta {
	static constexpr uint8_t OPCODE = OP_S_CAMPAIGN_DELTA;
	PacketHeader      hdr;
	uint8_t           ownerId;
	uint8_t           pad[3];
	CampaignDeltaBody body;
};

// Every delta after `seq`, the last one this machine has, to this machine.
struct C_CampaignSince {
	static constexpr uint8_t OPCODE = OP_C_CAMPAIGN_SINCE;
	PacketHeader hdr;
	uint32_t     seq;
};

// The cars the Import/Export garages and the emergency crane have taken, as
// the session's campaign (docs/missions.md 6.1). Each list is the engine's
// own bits: CGarages::CarTypesCollected, one word for each of the three
// collecting garages, bit n for the n-th car on its board, and
// CCranes::CarsCollectedMilitaryCrane, seven bits. Nothing is ever taken off
// a list, so the session's is every machine's put together.
constexpr uint8_t CAR_LIST_GARAGES = 3;
constexpr uint8_t CAR_LIST_CRANE   = 3;   // the crane's, after the garages'
constexpr uint8_t CAR_LISTS        = 4;

// A machine's own lists, when they have a car the session's lacks, and once
// on every connection so it is told the session's.
struct C_CarLists {
	static constexpr uint8_t OPCODE = OP_C_CAR_LISTS;
	PacketHeader hdr;
	uint32_t     collected[CAR_LISTS];
};

// The session's lists: to the sender, and to everybody when they grew. Each
// machine puts the bits it lacks into its own engine, and its own main.scm
// ticks the board, pays nothing twice and opens the crane when its player
// comes by, as it does for a car of its own.
struct S_CarLists {
	static constexpr uint8_t OPCODE = OP_S_CAR_LISTS;
	PacketHeader hdr;
	uint32_t     collected[CAR_LISTS];
};

// Every bit of `add` that `into` lacks, put in; true when there were any.
inline bool MergeCarLists(uint32_t (&into)[CAR_LISTS], const uint32_t (&add)[CAR_LISTS]) {
	bool grew = false;
	for (uint8_t i = 0; i < CAR_LISTS; ++i) {
		if ((add[i] & ~into[i]) != 0)
			grew = true;
		into[i] |= add[i];
	}
	return grew;
}

// ---------------------------------------------------------------------------
// Desync probes
// ---------------------------------------------------------------------------
//
// docs/protocol.md 1.26. "He was somewhere else on my screen" is a claim about
// two machines, and the server is the one place that hears from both. Every
// DESYNC_PROBE_MS an observer says where its engine actually has each remote
// player on foot, each session car it is only watching and its replicas of
// other people's traffic and pedestrians, together with the
// instant of the owner's clock its playback was rendering (`atMs`, the
// owner's sendTimeMs timebase, which is the one its buffer runs on). The
// server keeps the last second or so of what each owner reported, looks up
// that instant and answers with the distance.
//
// So network delay is not counted as desync: an observer drawing a car 100 ms
// behind is compared with where the car was 100 ms ago. What is left is a copy
// the engine pushed off its pose, a replica frozen on a stale row, a stream
// that stalled, a car being corrected to the wrong place.

constexpr uint32_t DESYNC_PROBE_MS   = 2000;

// How far apart two consecutive samples are before a client's buffer jumps to
// the second instead of sliding (client/src/interp.h, SNAP_DISTANCE): a
// respawn, a teleport, a stall. Here so the server's comparison jumps where
// the copy did, instead of measuring the copy against a point halfway along a
// teleport that nobody was ever at.
constexpr float PED_SNAP_M = 5.0f;
constexpr float CAR_SNAP_M = 20.0f;

// And how far past the newest sample a player's or a session car's buffer
// runs on along its last velocity before it holds (interp.h,
// MAX_EXTRAPOLATE_MS), with that velocity converted from the engine's
// per-step move speed at this many steps a second (interp.h,
// ENGINE_STEPS_PER_SECOND).
constexpr uint32_t EXTRAPOLATE_MS          = 250;
constexpr float    ENGINE_STEPS_PER_SECOND = 50.0f;
constexpr uint8_t  DESYNC_PROBE_ROWS = 24;
// The answer for a row the server could not compare: an entity it has no
// history for, or an instant outside what it kept.
constexpr uint16_t DESYNC_UNKNOWN    = 0xFFFF;
constexpr uint16_t DESYNC_MAX_CM     = 0xFFFE;

struct DesyncProbeRow {
	uint16_t netId;   // a player, a session car, a replica of traffic or a pedestrian
	uint32_t atMs;    // the owner's clock
	Vec3     pos;     // where this machine's engine has it
};

struct C_DesyncProbe {
	static constexpr uint8_t OPCODE = OP_C_DESYNC_PROBE;
	PacketHeader   hdr;
	uint8_t        count;
	DesyncProbeRow rows[DESYNC_PROBE_ROWS];
};

struct DesyncReportRow {
	uint16_t netId;
	uint16_t offCm;   // DESYNC_UNKNOWN when there was nothing to compare with
};

// To the prober alone, in the order it asked.
struct S_DesyncReport {
	static constexpr uint8_t OPCODE = OP_S_DESYNC_REPORT;
	PacketHeader    hdr;
	uint8_t         count;
	DesyncReportRow rows[DESYNC_PROBE_ROWS];
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
//
// The third bit is not about ObjectDamage at all. `bIsStatic` (CEntity byte A
// bit 2) is the whole of "uprooted": the engine clears it and hands the
// object to CPhysical::AddToMovingList, and from there it is a falling
// physical rather than street furniture. It rides this byte because it costs
// nothing to put it here and because it is the only thing on the wire that
// tells an observer a resting place is on its way.
enum ObjectBreakFlags : uint8_t {
	OBJ_BREAK_RENDER_DAMAGED = 1 << 0,   // CEntity byte B bit 7
	OBJ_BREAK_SMASHED        = 1 << 1,   // !bIsVisible && !bUsesCollision
	OBJ_BREAK_UPROOTED       = 1 << 2,   // !bIsStatic - it came loose
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

// Where a knocked-over lamp post ended up.
//
// **A break is a latch and an uproot is a transform**, and that is the whole
// reason this is a second packet rather than three more fields on the first.
// A break only ever goes one way, applying it twice is a no-op and the
// receiver can replay the engine's own function to get there. Where an
// object comes to rest is a matrix, it is produced by local physics, and
// roadmap.md 2.4 is unambiguous that local physics does not reproduce: the
// timestep is frame-time-derived, so two machines handed the identical
// impulse put the same post down in two different places.
//
// So the *appearance* travels on ObjectBreakBody and the *place* travels
// here, once, when the object stops moving. Not a stream: CPhysical's own
// sleep test (m_nStaticFrames > 10, then SetIsStatic(true)) is the engine
// deciding it has finished, and CWorld::Process unlinks it from the moving
// list on the same pass. One object coming loose is one packet, whatever it
// was that knocked it over and however long it rolled.
//
// The full 3x3 rather than a heading, because a lamp post does not lie down
// about the z axis - it falls over, and the two vectors that say so are the
// ones a heading throws away. 48 bytes once per uprooting is cheaper than
// anything that would let an observer work it out for itself.
//
// `ident` is still m_objectMatrix's position - the IPL coordinate - and this
// is exactly the packet that proves why that had to be the key rather than
// the entity's own position. By the time this is sent, the object is metres
// from where the map put it, and on two machines it is metres away in two
// different directions. m_objectMatrix has not moved on either.
struct ObjectRestBody {
	ObjectIdent ident;
	Vec3        right;     // CMatrix, entity +0x04
	Vec3        forward;   //          entity +0x14
	Vec3        up;        //          entity +0x24
	Vec3        pos;       //          entity +0x34, the live position
};

// From the same machine that was entitled to report the break, chosen the
// same way: the owner of whatever knocked it loose, and the host for whatever
// nobody owns. docs/objects.md 5.
struct C_ObjectSettled {
	static constexpr uint8_t OPCODE = OP_C_OBJECT_SETTLED;
	PacketHeader   hdr;
	ObjectRestBody body;
};

// To everybody but the reporter. Reliable, and like the break there is no
// table, no snapshot component and no backfill - CPopulation::ManagePopulation
// converts the object back to a pristine dummy 80 m out and throws the whole
// transform away, so a resting place has the same one-visit lifetime the
// break does.
struct S_ObjectSettled {
	static constexpr uint8_t OPCODE = OP_S_OBJECT_SETTLED;
	PacketHeader   hdr;
	uint8_t        playerId;   // who reported it
	ObjectRestBody body;
};

// "My engine just built this object again, and I was told it was broken."
//
// The two packets above say there is no table and no backfill, and since this
// packet that is no longer true, for one reason: the 80 m horizon is *each
// machine's* horizon. A player who drives off and comes back has his own copy
// rebuilt pristine while a player who stayed still has it broken, so the
// session keeps the break and the resting place while any player is within
// OBJECT_RECORD_RANGE (server/core/objectrecords.h), and hands a joiner the
// breaks it holds.
//
// Sent only for an object this machine has heard about - a break or a
// resting place off the wire, one of its own reports, or a joiner's backfill -
// so driving through a city of intact lamp posts sends nothing. The answer is
// the S_ObjectBroken, then the S_ObjectSettled if there is one, to the asker
// alone; nothing when the session has forgotten it, which is right, because
// then nobody is near enough to hold it broken.
struct C_ObjectRebuilt {
	static constexpr uint8_t OPCODE = OP_C_OBJECT_REBUILT;
	PacketHeader hdr;
	ObjectIdent  ident;   // m_objectMatrix's position, as always
};

// ---- the police helicopter -------------------------------------------------
//
// See the version history's entry 32. A helicopter is named by
// its owner and a serial the owner hands out, one per helicopter its engine
// builds. Not by the engine's slot alone: UpdateHelis puts the next one in the
// same slot, and a late state for the old one must not bring it back. Not by
// a pool handle either, which means nothing on another machine.

// m_heliStatus (+0x2A8). The two CoopIII acts on are the binary's: SHOT_DOWN
// is 3 in both collision tests (`mov byte [eax+2A8h],3` at 0x0054AADF and
// 0x0054ADC0) and FLY_AWAY is 2 in UpdateHelis (`mov byte [ecx+2A8h],2` at
// 0x0054A3E2). The constructor writes 0 (0x00547280). The other two are re3's
// names and only ride the wire.
enum HeliStatus : uint8_t {
	HELI_STATUS_HOVER       = 0,
	HELI_STATUS_CHASE       = 1,
	HELI_STATUS_FLY_AWAY    = 2,
	HELI_STATUS_SHOT_DOWN   = 3,
	HELI_STATUS_HOVER2      = 4,
	HELI_STATUS_COUNT
};

enum HeliStateFlags : uint8_t {
	// bRenderScorched, set by UpdateHelis 7 s before the explosion, in the
	// same step that throws off the tail and the back rotor and sets off the
	// first blast. An observer that sees it go on plays that step.
	HELI_FLAG_TAIL_BLOWN = 1 << 0,
};

// What the owner's engine has, at HELI_STATE_HZ. 60 bytes.
struct HeliStateBody {
	uint16_t serial;
	uint8_t  slot;        // index into CHeli::pHelis, 0 or 1
	uint8_t  status;      // HeliStatus
	uint8_t  flags;       // HeliStateFlags
	uint8_t  pad[3];
	Vec3     pos;
	Quat     rot;
	// Metres per second. The engine's m_vecMoveSpeed is per timestep, and
	// CTimer::Update makes a timestep frameMs * 0.001 * 50 (0x004AD223,
	// 0x004AD232), so this is m_vecMoveSpeed * 50.
	Vec3     velocity;
	// Where the searchlight is on the ground and how bright it is, as the
	// owner's AI worked it out from the owner's player. The observer draws
	// what it is told rather than pointing the light at its own player.
	float    searchLightX;
	float    searchLightY;
	float    searchLightIntensity;
};

struct C_HeliState {
	static constexpr uint8_t OPCODE = OP_C_HELI_STATE;
	PacketHeader  hdr;
	HeliStateBody body;
};

// To everybody but the owner, on the snapshot channel.
struct S_HeliState {
	static constexpr uint8_t OPCODE = OP_S_HELI_STATE;
	PacketHeader  hdr;
	uint8_t       ownerPlayerId;
	HeliStateBody body;
};

enum HeliGoneReason : uint8_t {
	// Status FLY_AWAY and past 150 m up, the one other way UpdateHelis
	// deletes a helicopter (0x00549B81..0x00549C29).
	HELI_GONE_FLEW_AWAY = 0,
	// The explosion branch at 0x00549C4B.
	HELI_GONE_SHOT_DOWN = 1,
	// Gone from its slot by any other route: a load, a restart, the session
	// ending on the owner's side. Nothing to play, just take it away.
	HELI_GONE_VANISHED  = 2,
	HELI_GONE_COUNT
};

struct HeliGoneBody {
	uint16_t serial;
	uint8_t  slot;
	uint8_t  reason;          // HeliGoneReason
	// Whose hit brought it down, when that was somebody else's. The shooter's
	// own machine registers the crime and the statistics; INVALID_PLAYER when
	// the owner's own player or engine did it, or it wasn't shot down.
	uint8_t  creditPlayerId;
	uint8_t  flags;           // HELI_GONE_OWNER_KEPT; was padding, sent as 0
	uint8_t  pad[2];
	Vec3     pos;             // where it went off, or where it was last
};

// The owner's engine paid its $250 and statistics for a helicopter somebody
// else shot down and could not take them back (game/heli.h,
// WithholdHeliRewards). The shooter then registers only the crime: paying
// itself too would leave both of them holding the reward.
constexpr uint8_t HELI_GONE_OWNER_KEPT = 1 << 0;

// From the owner, reliable.
struct C_HeliGone {
	static constexpr uint8_t OPCODE = OP_C_HELI_GONE;
	PacketHeader hdr;
	HeliGoneBody body;
};

struct S_HeliGone {
	static constexpr uint8_t OPCODE = OP_S_HELI_GONE;
	PacketHeader hdr;
	uint8_t      ownerPlayerId;
	HeliGoneBody body;
};

enum HeliHitKind : uint8_t {
	HELI_HIT_BULLET = 0,   // TestBulletCollision's arm: damage accumulates
	HELI_HIT_ROCKET = 1,   // TestRocketCollision's arm: down at once
	HELI_HIT_KIND_COUNT
};

// A hit the shooter's engine landed on its replica. Carries what the two
// collision tests need and nothing about where: the shooter already resolved
// the ray, and the owner's rule for what it costs doesn't read a position.
struct HeliHitBody {
	uint8_t  ownerPlayerId;
	uint8_t  slot;
	uint16_t serial;
	uint8_t  kind;      // HeliHitKind
	uint8_t  pad;
	uint16_t damage;    // TestBulletCollision's last argument; 0 for a rocket
};

struct C_HeliHit {
	static constexpr uint8_t OPCODE = OP_C_HELI_HIT;
	PacketHeader hdr;
	HeliHitBody  body;
};

// To the owner alone. `attackerId` is who fired, the same as S_CarHit.
struct S_HeliHit {
	static constexpr uint8_t OPCODE = OP_S_HELI_HIT;
	PacketHeader hdr;
	uint8_t      attackerId;
	HeliHitBody  body;
};

// One round the owner's helicopter fired: the two points its engine handed
// FireOneInstantHitRound, as they were. Nothing about what it hit - the
// owner's engine already decided that, on the owner's own player, and an
// observer only draws it. hdr.sendTimeMs is when it went off, on the same
// clock as the helicopter's C_HeliState.
struct HeliShotBody {
	uint16_t serial;
	uint8_t  slot;
	uint8_t  pad;
	Vec3     source;   // three metres out from the helicopter, toward the player
	Vec3     target;   // the player, scattered, and three metres past
};

// From the owner, unreliable.
struct C_HeliShot {
	static constexpr uint8_t OPCODE = OP_C_HELI_SHOT;
	PacketHeader hdr;
	HeliShotBody body;
};

// To everybody but the owner, on the snapshot channel.
struct S_HeliShot {
	static constexpr uint8_t OPCODE = OP_S_HELI_SHOT;
	PacketHeader hdr;
	uint8_t      ownerPlayerId;
	HeliShotBody body;
};

// ---- cheats (docs/cheats.md) ------------------------------------------------

// Which cheat, and what it left behind on the typist's machine. See
// IsValidCheatState for what `state` means per cheat.
struct CheatBody {
	uint8_t cheat;   // CheatId
	uint8_t state;
};

// Sent by the typist for a cheat whose route is not CHEAT_ROUTE_LOCAL, after
// running it on its own engine (EVERYONE) or instead of running it (HOST).
struct C_Cheat {
	static constexpr uint8_t OPCODE = OP_C_CHEAT;
	PacketHeader hdr;
	CheatBody    body;
};

// To the host alone for a sky, to everybody but the typist otherwise, and
// replayed to a joiner for the EVERYONE kind (the last state of each).
// `playerId` is who typed it, or INVALID_PLAYER for a replay whose typist has
// since left.
struct S_Cheat {
	static constexpr uint8_t OPCODE = OP_S_CHEAT;
	PacketHeader hdr;
	uint8_t      playerId;
	CheatBody    body;
};

// ---- emergency services (docs/protocol.md 1.37) ------------------------------

// A dead ambient pedestrian a medic has just stood up. Sent by the machine
// whose medic did it, which is the only engine that ran the CPR: the medic is
// that machine's pedestrian, and its MedicAI decided the treatment was over.
// The pedestrian may be one it hosts or its replica of somebody else's; either
// way it is already on his feet there. Reliable, on CH_EVENT, after any
// C_PedDeath the same machine had for him.
struct PedReviveBody {
	uint16_t netId;
};

struct C_PedRevive {
	static constexpr uint8_t OPCODE = OP_C_PED_REVIVE;
	PacketHeader  hdr;
	PedReviveBody body;
};

// To everybody but the medic's machine, the pedestrian's host included: the
// host stands its real ped up, everybody else their replica. Only for a
// pedestrian the session holds dead and did not make for a mission; the
// session marks him alive, so his next death is announced like his first and
// a joiner is handed him on his feet. `playerId` is whose medic it was.
struct S_PedRevive {
	static constexpr uint8_t OPCODE = OP_S_PED_REVIVE;
	PacketHeader  hdr;
	uint8_t       playerId;
	PedReviveBody body;
};

// A fire truck's water cannon, one frame's jet: where it leaves the truck and
// which way, both in the truck's own frame (x right, y forward, z up, metres
// and metres per timestep), so an observer's copy sprays from where its own
// copy of the truck is. What CAutomobile::FireTruckControl handed
// CWaterCannons::UpdateOne, taken back through the truck's matrix.
struct WaterCannonBody {
	uint16_t netId;   // the truck: a session car or a traffic car
	Vec3     pos;
	Vec3     dir;
};

// From the machine that aims the truck - its driver's, or the host of a truck
// nobody drives - at the snapshot rate while it sprays, unreliable. A jet
// nobody has heard of for WATER_CANNON_HOLD_MS has stopped.
struct C_WaterCannon {
	static constexpr uint8_t OPCODE = OP_C_WATER_CANNON;
	PacketHeader    hdr;
	WaterCannonBody body;
};

// To everybody else, on the snapshot channel. Dropped for a sender who
// neither drives nor settles a session car nor hosts the traffic car.
struct S_WaterCannon {
	static constexpr uint8_t OPCODE = OP_S_WATER_CANNON;
	PacketHeader    hdr;
	uint8_t         playerId;
	WaterCannonBody body;
};

// How long an observer keeps spraying a jet after its last packet: three and
// a half packets at SNAPSHOT_HZ, and the same 150 ms CWaterCannon keeps each
// point of its own jet alive before it moves on (0x00521B86, `add eax,96h`).
constexpr uint32_t WATER_CANNON_HOLD_MS = 150;

// ---- a unique jump's shot (docs/protocol.md §1.35) ----------------------------

// What the driver's USJ thread did to his camera: SET_FIXED_CAMERA_POSITION
// then POINT_CAMERA_AT_CAR on the car he drives, or RESTORE_CAMERA_JUMPCUT.
// `mode` and `swap` are the CCam mode and switch it asked for, 15 (fixed) and
// 2 (a jump cut) in the stock script. `from` and `up` mean nothing when `on`
// is 0.
struct StuntCameraBody {
	uint16_t netId;   // the session car the shot looks at
	uint8_t  on;      // 1: the shot starts; 0: it is over
	uint8_t  mode;
	uint8_t  swap;
	uint8_t  reserved;
	Vec3     from;    // where the camera stands
	Vec3     up;      // SET_FIXED_CAMERA_POSITION's second vector
};

// From the car's driver, sent as his camera takes the shot and as it lets go.
struct C_StuntCamera {
	static constexpr uint8_t OPCODE = OP_C_STUNT_CAMERA;
	PacketHeader    hdr;
	StuntCameraBody body;
};

// To each player the session has in a passenger seat of that car. `playerId`
// is the driver.
struct S_StuntCamera {
	static constexpr uint8_t OPCODE = OP_S_STUNT_CAMERA;
	PacketHeader    hdr;
	uint8_t         playerId;
	StuntCameraBody body;
};

// ---- money (MoneyRule) --------------------------------------------------------

// A change to the sender's cash, under MONEY_RULE_SHARED only. `seq` counts
// from 1 per connection and comes back in S_Money::ackSeq, which is how the
// sender tells the changes the pool already holds from the ones still on
// their way. `have` is the cash after it: the first change a player sends
// into an empty pool seeds it with that and ignores the delta.
struct MoneyChangeBody {
	uint32_t seq;
	int32_t  delta;
	int32_t  have;
};

struct C_MoneyChange {
	static constexpr uint8_t OPCODE = OP_C_MONEY_CHANGE;
	PacketHeader    hdr;
	MoneyChangeBody body;
};

// The rule, straight after the welcome, and under `shared` the pool after
// every change, to every player. Per player rather than broadcast, because
// ackSeq is the receiver's own: the last of *their* changes this total holds.
struct S_Money {
	static constexpr uint8_t OPCODE = OP_S_MONEY;
	PacketHeader hdr;
	uint8_t      rule;           // MoneyRule
	uint8_t      flags;          // MoneyFlags
	uint8_t      fromPlayerId;   // whose change this was; INVALID_PLAYER for none
	uint8_t      pad;
	int32_t      total;          // the pool, if seeded
	uint32_t     ackSeq;
	int32_t      delta;          // the change, for the log
};

// An AwardMoneyForExplosion the machine that decided a wreck did not pay
// itself, for the player who earned it. `unit` is one car's worth; the
// recipient multiplies it by its own six-second chain, the way the engine
// would have if that player's machine had decided the car. `key` names a car
// several machines decide, or has kind MONEY_AWARD_UNKEYED.
struct MoneyAwardBody {
	uint8_t           toPlayerId;
	uint8_t           kind;    // MoneyAwardKind
	uint16_t          model;   // the wreck, for the log
	int32_t           unit;
	UnownedVehicleKey key;
};

struct C_MoneyAward {
	static constexpr uint8_t OPCODE = OP_C_MONEY_AWARD;
	PacketHeader   hdr;
	MoneyAwardBody body;
};

// To the recipient alone, who may be the sender: a car several machines
// decide goes through here even when the decider is the one being paid, so
// the key can be checked.
struct S_MoneyAward {
	static constexpr uint8_t OPCODE = OP_S_MONEY_AWARD;
	PacketHeader   hdr;
	uint8_t        fromPlayerId;
	MoneyAwardBody body;
};

#pragma pack(pop)

inline bool MoneyAwardKeyed(const UnownedVehicleKey &key) {
	return key.kind == UNOWNED_PARKED || key.kind == UNOWNED_SESSION;
}

// The helicopter's wire facts, here because the server has to check them
// without addresses.h.
//
// Two police slots: UpdateHelis only ever puts a police helicopter in
// pHelis[0] or pHelis[1] (0x00549A8C, 0x00549AA2), and its fly-away pass walks
// exactly those two (`cmp [ebp-0A4h],2` at 0x0054A3EF).
constexpr uint8_t  HELI_POLICE_SLOTS = 2;
constexpr uint8_t  HELI_STATE_HZ     = 10;
// Every caller of TestBulletCollision passes 4 (`push 4` at 0x0055D933,
// 0x0055DB03 and 0x00562334). Five times that is room for a weapon nobody
// has found yet and not for a packet that ends a helicopter in one hit.
constexpr uint16_t HELI_HIT_MAX_DAMAGE = 20;

constexpr bool IsPoliceHeliSlot(uint8_t slot) { return slot < HELI_POLICE_SLOTS; }
constexpr bool IsKnownHeliStatus(uint8_t s) { return s < HELI_STATUS_COUNT; }
constexpr bool IsKnownHeliGoneReason(uint8_t r) { return r < HELI_GONE_COUNT; }

// A hit the owner will look at. A rocket carries no damage; a bullet carries
// some, and never more than HELI_HIT_MAX_DAMAGE.
constexpr bool IsSaneHeliHit(const HeliHitBody &b) {
	return IsPoliceHeliSlot(b.slot) &&
	       (b.kind == HELI_HIT_ROCKET ||
	        (b.kind == HELI_HIT_BULLET && b.damage > 0 &&
	         b.damage <= HELI_HIT_MAX_DAMAGE));
}

// The helicopter's gun. The engine fires a round only when 200 ms have
// passed since the last one (`add eax,0C8h` at 0x0054938A), so no more than
// five a second per helicopter.
constexpr uint32_t HELI_SHOT_MIN_INTERVAL_MS = 200;
// Longer than any round the engine can fire. It shoots only while its
// searchlight - never more than 42 m out, or the intensity is under the 0.9
// it needs - holds the player to within 7 m, from a helicopter that hovers
// tens of metres up. Not an engine number: a bound that keeps a corrupt
// packet from asking an observer to trace a line across the map.
constexpr float    HELI_SHOT_MAX_LENGTH      = 250.0f;

constexpr bool HeliShotFinite(float v) { return v == v && v > -1.0e6f && v < 1.0e6f; }

constexpr bool IsSaneHeliShot(const HeliShotBody &b) {
	if (!IsPoliceHeliSlot(b.slot))
		return false;
	if (!HeliShotFinite(b.source.x) || !HeliShotFinite(b.source.y) ||
	    !HeliShotFinite(b.source.z) || !HeliShotFinite(b.target.x) ||
	    !HeliShotFinite(b.target.y) || !HeliShotFinite(b.target.z))
		return false;
	const float dx = b.target.x - b.source.x;
	const float dy = b.target.y - b.source.y;
	const float dz = b.target.z - b.source.z;
	return dx * dx + dy * dy + dz * dz <= HELI_SHOT_MAX_LENGTH * HELI_SHOT_MAX_LENGTH;
}

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
static_assert(sizeof(PlayerRideBody)    == 20, "ride layout");
static_assert(sizeof(C_PlayerStateRide) == 96, "riding snapshot layout");
static_assert(sizeof(S_PlayerStateRide) == 97, "riding snapshot layout");

// 1 weapon + 1 flags + 2 clip + 4 total.
static_assert(sizeof(AmmoSlotBody)    == 8,  "ammo slot layout");
static_assert(sizeof(C_PlayerAmmo)    == 13, "player ammo layout");
static_assert(sizeof(S_PlayerAmmo)    == 14, "player ammo layout");
static_assert(sizeof(S_PlayerPings)   == 5 + 2 * MAX_PLAYERS, "ping table layout");
static_assert(sizeof(C_Password)      == 5 + PASSWORD_LEN, "password layout");
static_assert(sizeof(C_Kick)          == 6, "kick layout");
static_assert(sizeof(C_LobbyJoin)     == 5 + 2 + NICK_LEN + PASSWORD_LEN, "lobby join layout");
static_assert(sizeof(S_LobbyAnswer)   == 5 + 4, "lobby answer layout");
static_assert(sizeof(LobbyEntry)      == 2 + NICK_LEN, "lobby entry layout");
static_assert(sizeof(S_Lobby)         == 5 + 4 + (LOBBY_MAX + MAX_PLAYERS) * (2 + NICK_LEN),
              "lobby roster layout");
static_assert(sizeof(C_LobbyStart)    == 5 + 4, "lobby start layout");
static_assert(sizeof(S_LobbyStart)    == 5 + 4, "lobby start layout");
static_assert(sizeof(MissionArea)         == 28, "mission area layout");
static_assert(sizeof(C_MissionClaim)      == 5 + 8 + 28, "mission claim layout");
static_assert(sizeof(S_MissionClaim)      == 5 + 8, "mission claim answer layout");
static_assert(sizeof(S_MissionWaiting)    == 5 + 8 + 12, "mission waiting layout");
static_assert(sizeof(C_MissionStarted)    == 5 + 8, "mission start layout");
static_assert(sizeof(S_MissionState)      == 5 + 24, "mission state layout");
static_assert(sizeof(C_MissionEnded)      == 5 + 4, "mission end layout");
static_assert(sizeof(S_MissionFail)       == 5 + 4, "mission fail layout");
static_assert(sizeof(C_MissionCheckpoint) == 5 + 4 + 12, "mission checkpoint layout");
static_assert(sizeof(MissionEffectBody)   == 12 + MISSION_EFFECT_CODE, "mission effect layout");
static_assert(sizeof(C_MissionEffect)     == 5 + sizeof(MissionEffectBody), "mission effect layout");
static_assert(sizeof(S_MissionEffect)     == 9 + sizeof(MissionEffectBody), "mission effect layout");
static_assert(sizeof(C_MissionWidget)     == 5 + 8, "mission widget layout");
static_assert(sizeof(S_MissionWidget)     == 5 + 12, "mission widget layout");
static_assert(sizeof(C_MissionBusy)        == 5 + 2, "mission busy layout");
static_assert(sizeof(S_MissionHandOver)    == 5 + 4, "mission hand-over layout");
static_assert(sizeof(C_MissionCatchUp)     == 5 + 2, "mission catch-up ask layout");
static_assert(sizeof(S_MissionCatchUp)     == 5 + 10, "mission catch-up answer layout");
static_assert(sizeof(C_MissionKill)        == 5 + 4, "mission kill layout");
static_assert(sizeof(S_MissionKill)        == 5 + 8, "mission kill layout");
static_assert(sizeof(C_MissionAnswers)     == 5 + 12, "mission answers layout");
static_assert(sizeof(S_MissionAnswers)     == 5 + 16, "mission answers layout");
static_assert(sizeof(CutsceneKey)          == 9, "cutscene key layout");
static_assert(sizeof(C_CutsceneState)      == 5 + 11, "cutscene state layout");
static_assert(sizeof(CutsceneVoteBody)     == 14, "cutscene vote layout");
static_assert(sizeof(S_CutsceneVote)       == 5 + 15, "cutscene vote layout");
static_assert(OP_C_CUTSCENE_STATE == 0x9E && OP_S_CUTSCENE_VOTE == 0x9F,
              "the cutscene vote stays inside 0x9E..0x9F");
static_assert(sizeof(C_MissionPickup)      == 5 + 8, "mission pickup layout");
static_assert(sizeof(S_MissionPickup)      == 5 + 8, "mission pickup layout");
static_assert(sizeof(C_MissionObjectBreak) == 5 + 12, "mission object break layout");
static_assert(sizeof(S_MissionObjectBreak) == 5 + 12, "mission object break layout");
static_assert(sizeof(MissionSeatCar)      == 4, "mission seat layout");
static_assert(sizeof(C_MissionSeats)      == 5 + 4 + 4 * MISSION_SEAT_CARS, "mission seats layout");
static_assert(sizeof(S_MissionSeats)      == 5 + 4 + 4 * MISSION_SEAT_CARS, "mission seats layout");
static_assert(offsetof(MissionSeatCar, leave) == 3, "leave is where the pad was");
static_assert(MAX_PLAYERS <= 8, "MissionSeatCar::leave has a bit per player");
static_assert(sizeof(C_MissionBoard)      == 5 + 4 + MAX_PLAYERS + 2, "mission board layout");
static_assert(sizeof(S_MissionBoard)      == 5 + 6 + MAX_PLAYERS, "mission board layout");
static_assert(OP_C_MISSION_BOARD == 0xE4 && OP_S_MISSION_BOARD == 0xE5,
              "the board takes the rest of the money block");
static_assert(sizeof(C_MissionReady)      == 5 + 4, "mission ready layout");
static_assert(sizeof(S_MissionReady)      == 5 + 8, "mission ready layout");
static_assert(offsetof(MissionEffectBody, readySeq) == 6, "readySeq is where the pad was");
static_assert(sizeof(CampaignValue)       == 8, "campaign value layout");
static_assert(sizeof(CampaignThread)      == 12, "campaign thread layout");
static_assert(sizeof(CampaignDeltaBody)   == 16 + 8 * CAMPAIGN_VALUES + 12 * CAMPAIGN_THREADS +
                                                 4 + MISSION_EFFECT_CODE,
              "campaign delta layout");
static_assert(sizeof(C_CampaignDelta)     == 5 + sizeof(CampaignDeltaBody), "campaign delta layout");
static_assert(sizeof(S_CampaignDelta)     == 9 + sizeof(CampaignDeltaBody), "campaign delta layout");
static_assert(sizeof(C_CampaignSince)     == 9, "campaign since layout");
static_assert(sizeof(C_CarLists) == 21 && sizeof(S_CarLists) == 21, "car lists layout");
static_assert(OP_C_CAR_LISTS == 0xF6 && OP_S_CAR_LISTS == 0xF7, "the car lists end the block");
static_assert(sizeof(DesyncProbeRow)  == 18, "desync probe row layout");
static_assert(sizeof(C_DesyncProbe)   == 6 + 18 * DESYNC_PROBE_ROWS, "desync probe layout");
static_assert(sizeof(DesyncReportRow) == 4, "desync report row layout");
static_assert(sizeof(S_DesyncReport)  == 6 + 4 * DESYNC_PROBE_ROWS, "desync report layout");
static_assert(sizeof(VehicleStateBody)== 72, "vehicle state layout");
static_assert(sizeof(C_VehicleState)  == 77, "vehicle snapshot layout");
static_assert(sizeof(S_VehicleState)  == 78, "vehicle snapshot layout");
static_assert(sizeof(C_Hello)         == 33, "hello layout");
static_assert(sizeof(S_Welcome)       == 17, "welcome layout");
static_assert(sizeof(S_SessionRules)  == 5 + 4, "session rules layout");
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

// Who simulates a car nobody is driving, and the traffic car that stopped
// being traffic. 5 + 2, 5 + 2 + 1 + 1, and 5 + 2 + 1 + 1 + 36.
static_assert(sizeof(C_VehicleSettled) == 7,  "settled layout");
static_assert(sizeof(S_VehicleCustody) == 9,  "custody layout");
static_assert(sizeof(S_CarPromoted)    == 45, "car promotion layout");
static_assert(sizeof(S_Chat)           == 134, "chat layout");

// 5 hdr + 2 netId + 2 animId = 9, and no padding anywhere in it: both
// members are 2-byte and the header ends on an odd byte, which is exactly
// the shape C_PedBodyPart already has and is the reason the bodies of both
// are laid out this way rather than starting with the byte fields.
static_assert(sizeof(PedDeathBody)    == 4,  "ped death layout");
static_assert(sizeof(C_PedDeath)      == 9,  "ped death layout");
static_assert(sizeof(S_PedDeath)      == 9,  "ped death layout");
static_assert(offsetof(PedDeathBody, animId) == 2, "netId first");

// The same eleven bytes DamageBody is, in the same order, because it is the
// same five arguments of the same engine function with a pedestrian's netId in
// place of a player's. 2 net + 1 weapon + 4 amount + 1 piece + 1 direction,
// then the two melee bytes.
// The offsetof is what says the float really is unaligned rather than padded
// into place - which is only true because of the #pragma pack above, and a
// pack that stopped covering it would show up here instead of as an owner
// reading somebody's hit out of the wrong four bytes.
static_assert(sizeof(PedDamageBody)   == 11, "ped damage layout");
static_assert(sizeof(C_PedDamage)     == 16, "ped damage layout");
static_assert(sizeof(S_PedDamage)     == 17, "ped damage layout");
static_assert(offsetof(PedDamageBody, amount) == 3, "the float is unaligned");
static_assert(offsetof(PedDamageBody, piece) == 7, "piece and direction follow it");
static_assert(offsetof(PedDamageBody, melee) == 9, "the melee bytes are last");

// Three fields because CVehicle::InflictDamage takes three arguments - it
// closes `ret 0Ch` where CPed::InflictDamage closes `ret 14h`. A car has no
// pedPiece and no hit direction: there is no limb to take off and no knockdown
// animation to choose, so there is nothing for a fourth or fifth field to be.
static_assert(sizeof(VehicleHitBody)  == 7,  "vehicle hit layout");
static_assert(sizeof(C_VehicleHit)    == 12, "vehicle hit layout");
static_assert(sizeof(S_VehicleHit)    == 13, "vehicle hit layout");
static_assert(offsetof(VehicleHitBody, amount) == 3, "the float is unaligned");
static_assert(sizeof(VehicleHitBody) < sizeof(PedDamageBody),
              "a car's hit is a ped's minus the piece and the direction, and "
              "that is not a saving - it is the two arguments the engine's "
              "vehicle function does not have");
static_assert(sizeof(C_CarHit) == sizeof(C_VehicleHit), "traffic hit layout");
static_assert(sizeof(S_CarHit) == sizeof(S_VehicleHit), "traffic hit layout");

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

// 5 hdr + 1 was + 1 count + 2 pad, then 4 a row.
static_assert(sizeof(AmbientAdoptRow) == 4, "adopt row layout");
static_assert(sizeof(S_AmbientAdopt)  == 9 + 4 * MAX_ADOPT_ROWS, "adopt batch layout");
static_assert(offsetof(S_AmbientAdopt, rows) == 9, "rows follow the four bytes");
static_assert(offsetof(S_AmbientAdopt, why) == 7, "why took the first pad byte");
// 5 hdr + 2 netId + 1 count + 1 pad, then 2 a ped.
static_assert(sizeof(C_CarLetGo) == 9 + 2 * MAX_LET_GO_PEDS, "let-go layout");
static_assert(offsetof(C_CarLetGo, peds) == 9, "peds follow the four bytes");
static_assert(OP_S_AMBIENT_ADOPT == 0xCC,
              "adoption stays inside its block");

// 2 netId + 2 health + 12 pos + 16 rot + 12 velocity = 44. The health took
// the old padding, so nothing moved.
static_assert(sizeof(AmbientCarState) == 44, "ambient car state layout");
static_assert(offsetof(AmbientCarState, health) == 2, "health is the old pad");
static_assert(offsetof(AmbientCarState, pos) == 4, "pos stays 4-aligned");
static_assert(sizeof(C_CarStates)     == 9 + 44 * MAX_CAR_STATES,
              "ambient car state batch layout");
static_assert(sizeof(S_CarStates)     == 9 + 44 * MAX_CAR_STATES,
              "ambient car state batch layout");
static_assert(offsetof(C_CarStates, hornMask) == 6 &&
                  offsetof(S_CarStates, hornMask) == 7 &&
                  offsetof(C_CarStates, cars) == 9 &&
                  offsetof(S_CarStates, cars) == 9,
              "the horn mask is the old padding; the rows did not move");
static_assert(offsetof(C_CarStates, sirenMask) == 7 &&
                  offsetof(S_CarStates, sirenMask) == 8,
              "so is the siren mask, and S_CarStates has no padding left");

static_assert(sizeof(WorldStateBody)  == 4,  "world state layout");
static_assert(sizeof(C_WorldState)    == 9,  "world state layout");
static_assert(sizeof(S_WorldState)    == 10, "world state layout");

// 2 netId + 1 seat + 1 jack + 2 model + 1 + 1 colour + 1 pad + 12 pos + 16 rot
// 2 netId + 1 seat + 1 jack + 2 modelId + 2 colours + 2 extras + 12 pos
// + 16 rot. The two extras took the place of one `pad` byte, so this grew by
// one rather than by two.
// + 2 parkedSlot, at the end so nothing before it moves.
static_assert(sizeof(EnterVehicleBody) == 40, "enter-vehicle layout");
static_assert(offsetof(EnterVehicleBody, parkedSlot) == 38, "enter-vehicle layout");
static_assert(sizeof(C_EnterVehicle)  == 45, "enter-vehicle layout");
static_assert(sizeof(S_EnterVehicle)  == 46, "enter-vehicle layout");

// 2 netId + 1 seat + 1 door, and nothing else. An intent that carried a car's
// identity would be a claim, which is the one thing it must not be.
static_assert(sizeof(EnteringVehicleBody) == 4,  "entering-vehicle layout");
static_assert(sizeof(C_EnteringVehicle)   == 9,  "entering-vehicle layout");
static_assert(sizeof(S_EnteringVehicle)   == 10, "entering-vehicle layout");
static_assert(sizeof(C_JackingVehicle)    == 9,  "jacking-vehicle layout");
static_assert(sizeof(S_JackingVehicle)    == 10, "jacking-vehicle layout");

// 5 hdr + 2 net + 2 model + 12 pos + 16 rot + 2 colour + 2 extras = 41
// identity, then 4 health + 1 flags = 5 condition.
// + 2 parkedSlot after the condition.
static_assert(sizeof(S_VehicleSpawn)  == 48, "vehicle spawn layout");
static_assert(offsetof(S_VehicleSpawn, parkedSlot) == 46, "vehicle spawn layout");
// 2 netId + 1 reason + 1 pad.
static_assert(sizeof(VehicleRemovedBody) == 4, "vehicle removed layout");
static_assert(sizeof(C_VehicleRemoved)   == 9, "vehicle removed layout");
static_assert(sizeof(S_VehicleRemoved)   == 10, "vehicle removed layout");
static_assert(OP_C_VEHICLE_REMOVED == 0xAC && OP_S_VEHICLE_REMOVED == 0xAD,
              "the removal takes the first free pair of the garage block");
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
static_assert(sizeof(C_VehicleBomb)     == 5 + 8, "vehicle bomb layout");
static_assert(sizeof(S_VehicleBomb)     == 5 + 8, "vehicle bomb layout");
static_assert(offsetof(C_VehicleBomb, fuseMs) == 9 && offsetof(S_VehicleBomb, fuseMs) == 11,
              "the fuse follows the blame");
static_assert(sizeof(C_MineBlast)       == 5 + 12, "mine blast layout");
static_assert(sizeof(S_MineBlast)       == 5 + 16, "mine blast relay layout");
static_assert(OP_C_MINE_BLAST >= 0xEC && OP_S_MINE_BLAST <= 0xEF, "mines stay inside their block");
static_assert(sizeof(C_MissionBomb)     == 5 + 4, "mission bomb layout");
static_assert(sizeof(S_MissionBomb)     == 5 + 4, "mission bomb relay layout");
static_assert(offsetof(S_MissionBomb, netId) == 7, "the owner and the type come first");
static_assert(OP_C_MISSION_BOMB == 0xEE && OP_S_MISSION_BOMB == 0xEF,
              "the mission's bomb takes the rest of the mines' block");
static_assert(sizeof(C_VehicleRadio)    == 5 + 4, "vehicle radio layout");
static_assert(sizeof(S_VehicleRadio)    == 5 + 4, "vehicle radio layout");
static_assert(offsetof(S_VehicleRadio, netId) == 7, "the radio relay's netId follows its two bytes");
static_assert(sizeof(C_VehicleAlarm)    == 5 + 4, "vehicle alarm layout");
static_assert(sizeof(S_VehicleAlarm)    == 5 + 6, "vehicle alarm layout");
static_assert(offsetof(S_VehicleAlarm, netId) == 7, "the alarm relay's netId follows its two bytes");
static_assert(sizeof(C_VehicleAim)      == 5 + 10, "vehicle aim layout");
static_assert(sizeof(S_VehicleAim)      == 5 + 12, "vehicle aim layout");
static_assert(offsetof(S_VehicleAim, gunLR) == 9, "the aim relay's angles follow its netId");
static_assert(OP_C_VEHICLE_ALARM >= 0xE8 && OP_S_VEHICLE_AIM <= 0xEB,
              "a car's own state stays inside its block");
static_assert(VEH_TAXI_LIGHT == 0x20 && VEH_HANDBRAKE == 0x40,
              "two bits the snapshot's flags byte had free");
static_assert(sizeof(C_PlayerModel)   == 7,  "player model layout");
static_assert(sizeof(S_PlayerModel)   == 8,  "player model layout");
static_assert(sizeof(C_PlayerLook)    == 29, "player look layout");
static_assert(sizeof(S_PlayerLook)    == 30, "player look layout");
static_assert(OP_S_PLAYER_LOOK <= 0xCF, "the look stays inside the C0 block");
static_assert(sizeof(C_PlayerAway)    == 6,  "player away layout");
static_assert(sizeof(S_PlayerAway)    == 7,  "player away layout");

// 1 weapon + 12 origin + 12 dir + 4 speed
static_assert(sizeof(ShotBody)        == 29, "shot layout");
static_assert(sizeof(C_Shot)          == 34, "shot layout");
static_assert(sizeof(S_Shot)          == 35, "shot layout");
static_assert(offsetof(ShotBody, speed) == 25, "speed follows dir");
static_assert(sizeof(ExplosionBody)   == 13, "explosion layout");
static_assert(sizeof(C_Explosion)     == 18, "explosion layout");
static_assert(sizeof(S_Explosion)     == 19, "explosion layout");

// 2 victim + 1 weapon + 4 amount + 1 piece + 1 direction + 1 melee + 1 level
static_assert(sizeof(DamageBody)      == 11, "damage layout");
static_assert(sizeof(C_Damage)        == 16, "damage layout");
static_assert(sizeof(S_Damage)        == 17, "damage layout");
static_assert(offsetof(DamageBody, piece) == 7, "piece and direction follow the amount");
static_assert(offsetof(DamageBody, melee) == 9, "the melee bytes are last");
static_assert(offsetof(DamageBody, melee) == offsetof(PedDamageBody, melee),
              "one melee layout on both");
static_assert((MELEE_KIND_MASK & (MELEE_ARMED | MELEE_GROUND_KICK | MELEE_HEAVY)) == 0,
              "the flags stay clear of the kind");
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

static_assert(sizeof(RampageStartBody)  == 6,  "rampage start layout");
static_assert(sizeof(C_RampageStart)    == 11, "rampage start layout");
static_assert(sizeof(RampageOpenBody)   == 8,  "rampage open layout");
static_assert(sizeof(S_RampageOpen)     == 13, "rampage open layout");
static_assert(sizeof(RampageKillBody)   == 6,  "rampage kill layout");
static_assert(sizeof(C_RampageKill)     == 11, "rampage kill layout");
static_assert(sizeof(S_RampageKill)     == 12, "rampage kill relay layout");
static_assert(sizeof(RampageEndBody)    == 3,  "rampage end layout");
static_assert(sizeof(C_RampageEnd)      == 8,  "rampage end layout");
static_assert(sizeof(S_RampageEnd)      == 8,  "rampage end layout");
// byPlayer sits before the body in the relay, the same shape S_PickupDrop and
// S_PedDamage use, so the body is byte-identical in both directions.
static_assert(offsetof(S_RampageKill, byPlayer) == sizeof(PacketHeader),
              "rampage kill relay layout");
static_assert(sizeof(RampageVoteBody)     == 8,  "rampage vote layout");
static_assert(sizeof(C_RampageVote)       == 7,  "rampage vote cast layout");
static_assert(sizeof(S_RampageVote)       == 13, "rampage vote layout");
static_assert(sizeof(RampageTeleportBody) == 16, "rampage teleport layout");
static_assert(sizeof(S_RampageTeleport)   == 21, "rampage teleport layout");
static_assert(sizeof(C_RampageArrived)    == 7,  "rampage arrival layout");
static_assert(OP_C_RAMPAGE_VOTE >= 0xC4 && OP_S_RAMPAGE_TELEPORT <= 0xC9,
              "the vote stays inside the block held for it");
static_assert((PICKUP_F_RAMPAGE | PICKUP_F_VOTED) != 0 &&
                  ((PICKUP_F_RAMPAGE | PICKUP_F_VOTED) & PICKUP_F_BRIBE) == 0,
              "the skull's bits stay clear of the bribe's");
static_assert(sizeof(RampageCarBody)    == 8,  "rampage car layout");
static_assert(sizeof(C_RampageCar)      == 13, "rampage car layout");
static_assert(sizeof(S_RampageCar)      == 14, "rampage car relay layout");
static_assert(offsetof(S_RampageCar, byPlayer) == sizeof(PacketHeader),
              "rampage car relay layout");

// 2 serial + 1 slot + 1 status + 1 flags + 3 pad + 12 pos + 16 rot + 12 vel
// + 12 searchlight = 60.
static_assert(sizeof(HeliStateBody) == 60, "heli state layout");
static_assert(offsetof(HeliStateBody, pos) == 8, "heli state layout");
static_assert(sizeof(C_HeliState)   == 65, "heli state layout");
static_assert(sizeof(S_HeliState)   == 66, "heli state relay layout");
static_assert(sizeof(HeliGoneBody)  == 20, "heli gone layout");
static_assert(sizeof(C_HeliGone)    == 25, "heli gone layout");
static_assert(sizeof(S_HeliGone)    == 26, "heli gone relay layout");
static_assert(sizeof(HeliHitBody)   == 8,  "heli hit layout");
static_assert(sizeof(C_HeliHit)     == 13, "heli hit layout");
static_assert(sizeof(S_HeliHit)     == 14, "heli hit relay layout");
static_assert(offsetof(S_HeliState, ownerPlayerId) == sizeof(PacketHeader) &&
                  offsetof(S_HeliGone, ownerPlayerId) == sizeof(PacketHeader) &&
                  offsetof(S_HeliHit, attackerId) == sizeof(PacketHeader),
              "the relay's extra byte comes first, so the body is the same "
              "bytes in both directions");
static_assert(OP_C_HELI_STATE >= 0xA4 && OP_S_HELI_HIT <= 0xAB,
              "the helicopter stays inside the block held for it");
// 2 serial + 1 slot + 1 pad + 12 source + 12 target = 28.
static_assert(sizeof(HeliShotBody)  == 28, "heli shot layout");
static_assert(offsetof(HeliShotBody, source) == 4, "heli shot layout");
static_assert(sizeof(C_HeliShot)    == 33, "heli shot layout");
static_assert(sizeof(S_HeliShot)    == 34, "heli shot relay layout");
static_assert(offsetof(S_HeliShot, ownerPlayerId) == sizeof(PacketHeader),
              "the relay's extra byte comes first, as for the other three");
static_assert(OP_C_HELI_SHOT == 0xAA && OP_S_HELI_SHOT == 0xAB,
              "the gun takes the pair entry 32 kept for it, and nothing else");

// 5 hdr + 4 mask, and 5 + 1 + 3 pad + 4 for the relay.
static_assert(sizeof(GarageMaskBody)    == 4,  "the whole city is one dword");
static_assert(sizeof(C_GarageState)     == 9,  "garage mask layout");
static_assert(sizeof(S_GarageState)     == 13, "garage mask relay layout");
static_assert(sizeof(C_GateState)       == 9,  "gate mask layout");
static_assert(sizeof(S_GateState)       == 13, "gate mask relay layout");
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
static_assert(sizeof(ObjectRestBody)   == 64, "object rest body layout");
static_assert(offsetof(ObjectRestBody, right) == 16,
              "the matrix follows the ident");
static_assert(offsetof(ObjectRestBody, pos) == 52,
              "the position is the last row, as CMatrix has it");
static_assert(sizeof(C_ObjectSettled)  == 69, "object settled layout");
static_assert(sizeof(S_ObjectSettled)  == 70, "object settled relay layout");
static_assert(sizeof(C_ObjectRebuilt)  == 21, "object rebuilt layout");

// 5 hdr + 1 cheat + 1 state, and one more for who typed it.
static_assert(sizeof(CheatBody) == 2, "cheat body layout");
static_assert(sizeof(C_Cheat)   == 7, "cheat layout");
static_assert(sizeof(S_Cheat)   == 8, "cheat relay layout");
static_assert(sizeof(StuntCameraBody) == 30, "stunt camera body layout");
static_assert(sizeof(C_StuntCamera)   == 35, "stunt camera layout");
static_assert(sizeof(S_StuntCamera)   == 36, "stunt camera relay layout");
static_assert(OP_C_STUNT_CAMERA == 0xF8 && OP_S_STUNT_CAMERA == 0xF9,
              "the stunt shot takes the two past the cheat block");
static_assert(offsetof(S_Cheat, body) == 6, "the typist comes first");
static_assert(sizeof(C_PedRevive)     == 7,  "ped revive layout");
static_assert(sizeof(S_PedRevive)     == 8,  "ped revive relay layout");
static_assert(offsetof(S_PedRevive, body) == 6, "the medic's machine comes first");
static_assert(sizeof(WaterCannonBody) == 26, "water cannon layout");
static_assert(sizeof(C_WaterCannon)   == 31, "water cannon layout");
static_assert(sizeof(S_WaterCannon)   == 32, "water cannon relay layout");
static_assert(offsetof(S_WaterCannon, body) == 6, "the aimer comes first");
static_assert(OP_C_CHEAT == 0xF0 && OP_S_WATER_CANNON <= 0xF7,
              "cheats and emergency services stay inside their block");
static_assert((SESSION_CHEATS_MASK & (SESSION_FRIENDLY_FIRE | SESSION_AMMO_SYNC |
                                      SESSION_WANTED_MASK | SESSION_RAMPAGE_MASK)) == 0,
              "the cheat rule has bits 6-7 to itself");

// 4 seq + 4 delta + 4 have; 1 rule + 1 flags + 1 from + 1 pad + 4 total +
// 4 ack + 4 delta; 1 to + 1 kind + 2 model + 4 unit + 4 key.
static_assert(sizeof(MoneyChangeBody) == 12, "money change layout");
static_assert(sizeof(C_MoneyChange)   == 17, "money change layout");
static_assert(sizeof(S_Money)         == 21, "money layout");
static_assert(offsetof(S_Money, total) == 9, "the total follows the four bytes");
static_assert(sizeof(MoneyAwardBody)  == 12, "money award layout");
static_assert(offsetof(MoneyAwardBody, key) == 8, "the key follows the unit");
static_assert(sizeof(C_MoneyAward)    == 17, "money award layout");
static_assert(sizeof(S_MoneyAward)    == 18, "money award relay layout");
static_assert(offsetof(S_MoneyAward, body) == 6, "the sender comes first");
static_assert(OP_C_MONEY_CHANGE >= 0xE0 && OP_S_MONEY_AWARD <= 0xE5,
              "money stays inside its block");
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
// PICKUP_COLLECTABLE1, the hidden package: a ONCE pickup that also counts
// towards CPlayerInfo::m_nCollectedPackages. Spelled here for the server's
// `hiddenPackages` rule, which is the one thing that tells packages apart.
constexpr uint8_t PICKUP_TYPE_PACKAGE = 5;

// The server's `hiddenPackages` rule (docs/roadmap.md 5.11). Never sent: under
// either rule a client does what it is told, and the difference is only in
// what the server tells it.
constexpr uint8_t PACKAGES_SHARED    = 0;   // one collects it, it is gone for all
constexpr uint8_t PACKAGES_PERPLAYER = 1;   // each player finds their own hundred

constexpr uint32_t PickupRespawnMs(uint8_t type, bool isBribeModel) {
	switch (type) {
	case 1:  return 5000;                              // PICKUP_IN_SHOP
	case 2:  return 30000;                             // PICKUP_ON_STREET
	case 15: return isBribeModel ? 300000u : 720000u;  // PICKUP_ON_STREET_SLOW
	default: return 0;
	}
}

// PICKUP_ONCE_TIMEOUT and PICKUP_MONEY: what a dead pedestrian drops, and
// gone from the ground on every machine within 20 s and 30 s (addresses.h,
// PICKUP_ONCE_TIMEOUT_MS and PICKUP_MONEY_MS). Collected, the session
// remembers one of those for a minute rather than for good: kept, every
// backfill grew by one for each drop anybody ever picked up.
constexpr uint8_t  PICKUP_TYPE_ONCE_TIMEOUT = 4;
constexpr uint8_t  PICKUP_TYPE_MONEY        = 7;
constexpr uint32_t PICKUP_DROP_FORGET_MS    = 60000;

constexpr uint32_t PickupForgetMs(uint8_t type) {
	return type == PICKUP_TYPE_ONCE_TIMEOUT || type == PICKUP_TYPE_MONEY ? PICKUP_DROP_FORGET_MS
	                                                                     : 0;
}

} // namespace coopiii
