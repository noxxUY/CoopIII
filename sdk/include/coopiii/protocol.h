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
//    opcode, S_AmbientAdopt (0xD0), no layout moves. When a player leaves,
//    each of his ambient peds and traffic cars that another player is near
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

constexpr uint16_t PROTOCOL_VERSION = 38;
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
	// 0x60/0x61 out of the block below, assigned at the merge. The note there
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
	// 0xAC..0xAF stay free for the garages.

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

	// Claude's outfit, out of the top of that block. See C_PlayerLook.
	OP_C_PLAYER_LOOK      = 0xCA,
	OP_S_PLAYER_LOOK      = 0xCB,

	// A player left and somebody else hosts his crowd now. See S_AmbientAdopt.
	// 0xD0-0xD5 is the block; one is used. Server to client only: a machine
	// that cannot take a pedestrian it was given says so with the
	// C_PedDespawn / C_CarDespawn it already has, as the new owner.
	OP_S_AMBIENT_ADOPT    = 0xD0,

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
	// A carjack, as it starts. See C_JackingVehicle.
	OP_C_JACKING_VEHICLE  = 0xDA,
	OP_S_JACKING_VEHICLE  = 0xDB,

	// Money, behind the server's MoneyRule. 0xE0-0xE5 is the block; four of
	// the six are used. Nothing here is sent in a session with money off.
	// See MoneyRule and the unnumbered history entry above PROTOCOL_VERSION.
	OP_C_MONEY_CHANGE     = 0xE0,
	OP_S_MONEY            = 0xE1,
	OP_C_MONEY_AWARD      = 0xE2,
	OP_S_MONEY_AWARD      = 0xE3,

	// A cheat somebody typed that changes something their machine does not
	// own. 0xF0-0xF7 is the cheat block; two of the eight are used. What
	// travels is which cheat and what it left behind, never the keystrokes -
	// docs/cheats.md.
	OP_C_CHEAT            = 0xF0,
	OP_S_CHEAT            = 0xF1,
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
	// has to act on it.
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
};

struct S_VehicleDespawn {
	static constexpr uint8_t OPCODE = OP_S_VEHICLE_DESPAWN;
	PacketHeader hdr;
	uint16_t netId;
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
// just ahead of their own hit coming back (Session::CustodyForHit). And a
// custodian keeps a burning car until it goes up rather than until it stops:
// its fire timer is the only one running, so it is the one BlowUpCar.
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

struct S_AmbientAdopt {
	static constexpr uint8_t OPCODE = OP_S_AMBIENT_ADOPT;
	PacketHeader    hdr;
	uint8_t         wasOwnerPlayerId;
	uint8_t         count;
	uint8_t         pad[2];
	AmbientAdoptRow rows[MAX_ADOPT_ROWS];
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
// car in cars[i] has a running m_nCarHornTimer that its own audio would play
// (client/src/game/horn.h, TrafficHornOnWire). It means "honking now" and says
// nothing about how long - the receiver runs the engine's own 44-frame
// countdown for as long as the newest row says so.
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

// 1 weapon + 1 flags + 2 clip + 4 total.
static_assert(sizeof(AmmoSlotBody)    == 8,  "ammo slot layout");
static_assert(sizeof(C_PlayerAmmo)    == 13, "player ammo layout");
static_assert(sizeof(S_PlayerAmmo)    == 14, "player ammo layout");
static_assert(sizeof(S_PlayerPings)   == 5 + 2 * MAX_PLAYERS, "ping table layout");
static_assert(sizeof(C_Password)      == 5 + PASSWORD_LEN, "password layout");
static_assert(sizeof(DesyncProbeRow)  == 18, "desync probe row layout");
static_assert(sizeof(C_DesyncProbe)   == 6 + 18 * DESYNC_PROBE_ROWS, "desync probe layout");
static_assert(sizeof(DesyncReportRow) == 4, "desync report row layout");
static_assert(sizeof(S_DesyncReport)  == 6 + 4 * DESYNC_PROBE_ROWS, "desync report layout");
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
static_assert(OP_S_AMBIENT_ADOPT >= 0xD0 && OP_S_AMBIENT_ADOPT <= 0xD5,
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
static_assert(sizeof(EnterVehicleBody) == 38, "enter-vehicle layout");
static_assert(sizeof(C_EnterVehicle)  == 43, "enter-vehicle layout");
static_assert(sizeof(S_EnterVehicle)  == 44, "enter-vehicle layout");

// 2 netId + 1 seat + 1 door, and nothing else. An intent that carried a car's
// identity would be a claim, which is the one thing it must not be.
static_assert(sizeof(EnteringVehicleBody) == 4,  "entering-vehicle layout");
static_assert(sizeof(C_EnteringVehicle)   == 9,  "entering-vehicle layout");
static_assert(sizeof(S_EnteringVehicle)   == 10, "entering-vehicle layout");
static_assert(sizeof(C_JackingVehicle)    == 9,  "jacking-vehicle layout");
static_assert(sizeof(S_JackingVehicle)    == 10, "jacking-vehicle layout");

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
static_assert(sizeof(C_PlayerLook)    == 29, "player look layout");
static_assert(sizeof(S_PlayerLook)    == 30, "player look layout");
static_assert(OP_S_PLAYER_LOOK <= 0xCF, "the look stays inside the C0 block");

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

// 5 hdr + 1 cheat + 1 state, and one more for who typed it.
static_assert(sizeof(CheatBody) == 2, "cheat body layout");
static_assert(sizeof(C_Cheat)   == 7, "cheat layout");
static_assert(sizeof(S_Cheat)   == 8, "cheat relay layout");
static_assert(offsetof(S_Cheat, body) == 6, "the typist comes first");
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
