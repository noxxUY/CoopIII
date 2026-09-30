# Mission audit: every mission against the bridge

Every mission in the game, checked on 2026-09-24 against the design in
[missions.md](missions.md), to find what wouldn't work.

**How it was done.** First, every one of the 80 missions was tagged with the
mechanics it uses. That's a count of the opcodes behind each kind of hazard
(seats, pickups, garages, bombs, damage flags...) over the full decompilation
(`missions.md` §1). Then the script of every mission that hit a hazard was
read: its objectives (the green `~g~` lines) and the instructions around each
hazard. §2 says which missions each hazard touches, and §3 goes through
them one by one. Every claim about a mission below comes from its script.
Every claim about the engine is tagged with where it comes from, and §5
lists what can only be settled in a game.

When this was written nothing in it was built. Most of it is now, and each
requirement says so under its own **Built** line. What has no such line is
still on the list of what the bridge has to do before that mission plays
properly.

---

## 1. The verdict

1. **The design holds for the whole campaign.** No mission needs a change to
   `main.scm`, and none is out of reach of the stock-script bridge. Every
   problem found has a seam, and most of those seams are ones CoopIII already
   uses.

2. **The base bridge in `missions.md` §5 isn't enough on its own.** The audit
   found sixteen requirements it doesn't cover (§2), each needed by specific
   missions. Five of them decide whether the helpers are any use at all:

   | Requirement | Missions |
   |---|---|
   | R1, damage from a participant counts as the player's | 28 |
   | R4, passenger seats | 25 |
   | R3, mission objects | 16 create them, 11 move them |
   | R2, mission pickups | 19 |
   | R5, garages | 14 |

3. **One correction to the design.** `missions.md` §5.6 said a player
   condition could be answered "for any participant", opcode by opcode. That's
   unsafe:
   - 42 `if and` blocks test the player twice, like MEAT1's "player within
     20 m of the destination *and* in `$CAR_MEAT1`";
   - most objectives chain across separate blocks ("in the car" then "at the
     destination").

   Answered per condition, helper A sitting in the mission car and helper B
   standing at the destination would pass a check that no single player
   passed. So a condition can't be answered for "whoever". The rule instead
   (C1) is that **everybody has to be there**:
   - at the start;
   - at every checkpoint;
   - quiet for every stealth check.

   "Everybody" is safe where "anybody" wasn't: it distributes over the `if
   and` blocks, so a compound check still needs one state that satisfies
   all of it. Everything else stays the owner's as protagonist, and the helpers
   fight, drive, escort and protect.

4. **The owner can ride as a passenger.** The vehicle conditions test "in the
   car", not "driving". `IS_PLAYER_IN_CAR` is `bInVehicle && m_pMyVehicle ==
   car` in re3's handler, and `FindPlayerVehicle()` returns a passenger's car
   too. So a helper can drive the owner around and the mission still advances.
   What breaks in that case is everything else that looks at the owner's car:
   - the bomb shops (R6);
   - the Pay'n'Spray and the mission garages (R5);
   - the seats the mission's passengers need (R4);
   - the script's commands to that car, which reach the owner's copy and not
     the machine simulating it (R9).

   **Until those exist, the owner drives the mission car.**

5. **Some missions leave the helpers little to do**, and that's how the
   missions are written, not a bug:
   - the RC missions (one remote car);
   - the payphone run in Payday For Ray;
   - the floating packages in A Drop In The Ocean;
   - the bullion in Bullion Run;
   - the checkpoint runs and the taxi fares.

   The helpers escort. Nothing in §3 is unplayable.

---

## 2. The requirements the audit adds

Each one lists its evidence, where the seam is, and whether CoopIII already
has the machinery.

### R1. Damage from a participant counts as the player's

`SET_CHAR_ONLY_DAMAGED_BY_PLAYER` is set on at 79 sites in 11 missions, and
`SET_CAR_ONLY_DAMAGED_BY_PLAYER` at 43 sites in 19 missions. That's 28
missions, and they're the targets:

- Chunky Lee Chong and his Triads;
- Salvatore, and the Mafia cars in Sayonara Salvatore;
- Tanner;
- the eight dealers of Smack Down;
- the Triad laundry vans;
- the security van in Van Heist;
- the Colombian cars attacking Asuka's condo;
- the CIA agents in Marked Man;
- the racers in Turismo;
- and more in §3.

Both flags refuse any culprit that isn't `FindPlayerPed()` or
`FindPlayerVehicle()`:

- for a car, `addresses.h` proves it at `0x00551972`, inside
  `CVehicle::InflictDamage`, and `0x0052F653`, inside
  `CAutomobile::VehicleDamage`, the collision path;
- for a ped, re3's `CPed::InflictDamage` has the same test, and that is still
  to be proved on retail.

CoopIII applies a helper's hit on the owner's machine with the helper's
replica as the culprit (`ApplyRemotePedDamage`, and `vehicle.cpp`'s hit
path). So today **a helper's bullets, rockets and rams do nothing to any of
these targets.** `vehicle.cpp` already calls this out, as "a residual and it is
left standing", because in free roam the flag means "only the player may hurt
this". In a session mission every participant is the player (§9 of
`missions.md`), so the residual is the bug.

The fix is scoped to the owner's mission entities. When a participant's hit,
collision or blast reaches one of them, the flag is cleared around the
engine's own call and put back. The culprit stays honest, and nothing outside
a session mission changes.

**Built** (2026-09-24, `population.h`, `MissionHitScope`) for a hit or a ram
that arrives as `C_PedDamage` or `C_CarHit`. A blast is still the engine's
own, on the owner's machine, with the participant's replica as its culprit;
since 2026-09-25 the same scope lifts the flag there too, for the blast
causes and the fire a blast leaves (`missioncombat.h`,
`ParticipantBlastCounts`; the car's and the ped's `InflictDamage` detours).

**Corrected** (2026-09-30): a participant's *ram* never arrived as `C_CarHit`.
A collision is not forwarded at all; the owner's engine prices it off its copy
of the participant's car in `CAutomobile::VehicleDamage`, and the flag refused
it there, as it did a participant's car running over a mission pedestrian. So
in Van Heist only the owner's rams opened the van. ProcessControl's call to
`VehicleDamage` is now taken, and for a mission car a participant's car hits
the flag is lifted and the rammer reads `STATUS_PLAYER` (the Securicar's seven
times asks it), for that call only (`ParticipantCollisionCounts`,
`protocol.md` §1.57).

### R2. Mission pickups exist on every machine

40 `CREATE_PICKUP` / `CREATE_WEAPON_PICKUP` / `CREATE_CASH_PICKUP` /
`CREATE_DROP_OFF_PACKAGE` sites in 19 missions, all on the owner alone. That
covers the bat, the briefcases, the weapons at Phil's, Ray's stash, the
flamethrower, and the rocket launchers in S.A.M. and The Exchange.

Replaying the creation (§5.4) is enough, because the pickup work already
names a pickup by its position and model (`protocol.md` §1.13.1). The claim,
the grant and the award then run as they do for every script pickup. A
helper's collection lands in the owner's `aPickUpsCollected`, so the owner's
`HAS_PICKUP_BEEN_COLLECTED` goes true.

**Decided: the stash is for everybody.** Every weapon, ammo,
armour, health and cash pickup a mission lays out is *per player*:

- everybody takes their own, and nobody's copy disappears when somebody else
  takes one;
- the first collection by anybody still counts for the objective, so "Pick
  up the bat!" moves on however many bats get taken;
- a per-player pickup the script removes stays on every machine, the owner's
  included, until its player has taken it or the mission ends.

This covers Ray's stash (four weapons and $20,000), Phil's weapons, the bat
and the rocket launchers. The server side is already built for hidden
packages. Under `hiddenPackages = perplayer`, `Session::PickupIsPerPlayer`
weighs each claim against that player's own record and tells nobody else
(`pickups.md` §6). The change is widening its test from "a package, under
that rule" to "also a weapon, armour, health or cash pickup a session
mission created".

The mission's *objects of the story* stay single: a briefcase, a package,
the cargo. Taking one takes it for everybody, as the pickup sync does today.

Two edges:

- **Floating packages aren't in the pickup sync.** A Drop In The Ocean's 18
  packages are types 12 and 13, which `IsMine` in `pickup.cpp` leaves alone
  along with the mines. They need to join the exclusivity, or only the owner
  can collect them.
- **Some mission pickups outlive the mission.** Cipriani's Chauffeur removes
  Ammu-Nation's Uzi placeholder and creates the Uzi for sale
  (`$AMMUNATION_UZI_PICKUP`). That's a permanent unlock, so it belongs to the
  campaign delta (R10).

**Built** (2026-09-24): the creation and removal are replayed with a pickup
handle map, and the stash rule is `PICKUP_F_STASH` on the pickup's identity,
set by every machine for a weapon (`CPickups::WeaponForModel`), armour,
health or cash pickup the mission made. The server weighs its claims per
player and tells everybody of a collection without taking anybody's copy.
The floating packages too, their own way: `CREATE_FLOATING_PACKAGE` is
replayed where the owner's plane is, every machine watches its own copies,
and one that goes without the mission taking it away was taken there. That
machine says which, by the owner's handle (`C_MissionPickup`, `protocol.md`
§1.29): the owner's machine takes its own the way the engine does and pushes
it into the ring `HAS_PICKUP_BEEN_COLLECTED` reads, so the mission counts it,
and everybody else's copy goes.

### R3. Mission objects exist, move, break and are touched on every machine

The missions create objects at 87 sites in 16 missions and move them at 61
sites in 11 missions (`SLIDE_OBJECT`, `ROTATE_OBJECT`,
`PLACE_OBJECT_RELATIVE_TO_CAR`, `SET_OBJECT_COORDINATES`). What they are:

- the bank doors in The Getaway;
- the airport doors in Marked Man;
- the evidence files riding in the prosecution's car in Evidence Dash;
- Plaster Blaster's bodycast (an object, `#BODYCAST`);
- the 15 espresso stalls;
- the buoys and the cargo in S.A.M.;
- the wreck of the Callahan bridge.

Four needs:

- **Creation and deletion** replayed, with the handle map (§5.4).
- **Movement.** Many of the moved objects are *main-script* doors (`$JOEY_DOOR1`,
  `$BANKJOB_DOOR`, `$DOGFOOD_DOOR`, `$PORTLAND_HIDEOUT_DOOR`) that every
  machine created at startup. So the replay has to name the *global variable*
  a handle came from, not its value, and each machine then moves its own
  door. That's a sharper rule than §5.4's handle map, and it's the right one
  for anything the main script made. The same rule fixes the safehouse
  pedestrian door that `protocol.md` §1.16.7 left open, whenever a mission
  swings it.
- **Damage forwarded to the owner.** `HAS_OBJECT_BEEN_DAMAGED` decides
  Espresso-2-Go (9 sites) and Plaster Blaster. A helper who wrecks a stall
  does it to a replica, so the hit has to reach the owner's copy, the way ped
  and car hits already do.
- **Touching stays the owner's.** `IS_PLAYER_TOUCHING_OBJECT` is S.A.M.'s
  cargo, 8 sites (C1 below).

**Built** (2026-09-24), the first two by the sharper rule: every object
operand in the missions is a global, so the replay sends the global and each
machine's own holds its own object; a creation goes into the same global
everywhere. A participant checks its global holds a live object first
(`CPools::GetObject`) and releases what the mission made when it ends.
The damage forwarding too: a break a machine's own player or car does to its
copy of one of the mission's objects goes to everybody else, named by its
global, and each runs the engine's own `ObjectDamage` on its own until it is
as broken, so the owner's `HAS_OBJECT_BEEN_DAMAGED` sees a stall a
participant drove through (`C_MissionObjectBreak`, `protocol.md` §1.29).

### R4. Passenger seats are kept for the mission

In 25 missions a mission ped gets into the owner's car:

- `SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER`: Misty, Maria, Curly Bob, the bank
  manager, the patients, the fares;
- `SET_PLAYER_AS_LEADER`: the three thugs in The Getaway, Luigi's girls,
  8-Ball, Kanbu, Ray, the Old Oriental Gentleman.

A participant sitting in the owner's passenger seat takes the seat the
mission needs. The ped then waits outside forever, and the script keeps
saying "They ain't sardines!" or "Go back and get her". Some missions need
every passenger seat:

- Salvatore's Called A Meeting puts Joey, Luigi and Toni in the limo;
- The Getaway's three thugs fill a four-door car.

So in those, the owner can't be a passenger either.

The rule: while a mission ped is heading for the owner's car, CoopIII keeps
participants out of its free seats. The seat key refuses them, with a line
on the HUD. Anybody already sitting there gets out, the way `SetExitCar`
already plays it.

**Built** (2026-09-24): the owner's machine looks at the pedestrians its
mission made a few times a second, and one whose objective is to get into a
session car as a passenger keeps that car's seats (`C_MissionSeats`,
`protocol.md` §1.29). That objective is `SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER`'s
and also the one the engine gives a follower when the player gets into a car,
so both lists above are covered. A participant's seat key refuses the car
while it is kept, and when the car has fewer free passenger seats on the
owner's machine than pedestrians heading for them, as many participants
riding in it as it takes get out, the ones the owner names, and no more.
The Paramedic's capacity check (`GET_NUMBER_OF_PASSENGERS`, the only one in
main.scm; the taxi doesn't ask) no longer counts a participant riding along
(`protocol.md` §1.40).

**Corrected** (2026-09-30, a playtest of Drive Misty For Me softlocked with
the guest in the owner's passenger seat). "Also the one the engine gives a
follower" was only half true, and the objective was never there to be seen
when it mattered:

- `CPed::ProcessObjective`'s passenger arm drops the order for a full car
  (`0x004D9B26`: not in a vehicle and `m_nNumPassengers >=
  m_nNumMaxPassengers`, then `RestorePreviousObjective`). The script gives
  it and the ped is processed in the same frame, so with a participant in the
  only seat Misty had already given up before the owner's machine looked.
- The follower arm doesn't set the objective at all for a full car: it tests
  for a free seat first.

So the owner's machine now remembers each `SET_CHAR_OBJ_ENTER_CAR_AS_PASSENGER`
and `SET_PLAYER_AS_LEADER` (`CLEAR_LEADER` forgets it) its mission gives. An
order the engine dropped with a player's copy filling the car, and a follower
on foot beside the owner's full car with a player riding in it, count as on
the way in: the seat is kept and the rider asked out as before. The dropped
order is given again, through the same instruction, as soon as a seat is free
on the owner's machine, and let go of after 60 s if nobody ever leaves.
Misty then gets in, and `IS_CHAR_IN_CAR` passes as in single player
(`game/mission.h`, `NextSeatOrderStep`, `FollowerKeptOut`). Taking the
participant out rather than pretending she got in is what the script's
checks allow: Drive Misty For Me asks next where Misty herself is
(at Joey's, 21_luigi3.sc:420), not only whether she boarded.

### R4b. "The player in the car" is anybody in it (2026-09-30)

A playtest of Mike Lips Last Lunch: the guest got into Lips' car and the
mission went on waiting at `while not is_player_in_car $PLAYER_CHAR car
$LIPSFORELLI_CAR`, which only the owner could end. `IS_PLAYER_IN_CAR` (00DC)
is now answered yes, on the owner's machine, for any participant the session
seats in that car, at the wheel or riding, in every mission. That is what lets
the owner and the helpers drive their own cars (the next point): the helper
can take the mission car while the owner follows in his.

What was widened, and what wasn't:

- `IS_PLAYER_IN_CAR`: widened, through the flag the and/or block would have
  had (`CompareFlagIfTrue`), so `if and`, `if or` and `NOT` come out as they
  would for the owner. Not in an `if and` that asked a location of the owner
  earlier in the same block ("within 20 m of the destination *and* in
  $CAR_MEAT1", 15_meat1.sc:350; toni3 and frank2 the same): that is one
  player's state, and stays the owner's
  (`MayAnswerInCarForAnybody`). The "Hey! Get back in the vehicle!" the
  mission prints then only goes up when nobody is in the car.
- Location checks: already everybody's at a checkpoint, by where each
  participant is, whatever he drives (§5.6 of missions.md, 5 m of slack), so
  a helper in his own car at the place counts. Not changed.
- `IS_PLAYER_IN_ANY_CAR`, `IS_PLAYER_IN_MODEL`, `STORE_CAR_PLAYER_IS_IN`:
  not widened. Every "get a vehicle" wait is followed by `STORE_CAR_PLAYER_IS_IN`
  (Drive Misty For Me, 21_luigi3.sc:208-211), which reads the owner's car; a
  yes for a helper's car would hand the script no car at all.
- `IS_CHAR_IN_CAR` on a mission pedestrian: not widened. It is about her,
  and R4 above is what gets her in.

### R5. Garage conditions come from the machine the car is on

There are 14 missions. The script asks the owner's own engine:

- `IS_CAR_IN_MISSION_GARAGE`;
- `DOES_GARAGE_CONTAIN_CAR`;
- `HAS_RESPRAY_HAPPENED`, which is a *garage* question: "garage
  `$PORTLAND_PAYNSPRAY_GARAGE` respray_done";
- `IS_GARAGE_CLOSED`.

But the garage state machines key off `FindPlayerVehicle()` on each machine,
and `protocol.md` §1.16.4 deliberately stops an observer running the arm that
ends a Pay'n'Spray visit. So if a helper delivers the car, the owner's garage
may never reach the state the script waits for.

It works as it stands if the owner drives, or rides (§1.4). What's needed:

- replay the garage setup (`SET_TARGET_CAR_FOR_MISSION_GARAGE`,
  `CHANGE_GARAGE_TYPE`, `ACTIVATE_GARAGE`, `OPEN_GARAGE`, `CLOSE_GARAGE`) to
  every machine;
- answer those four conditions from the machine that has the car.

**Built** (2026-09-24, `protocol.md` §1.29): the setup reaches everybody, the
target car by its netId, so every machine's own garage opens for the car when
its own player brings it and shuts on it when they walk out. Every
participant's machine asks its own garages the two questions 36 of the
sites ask, `IS_CAR_IN_MISSION_GARAGE` (32) and `HAS_RESPRAY_HAPPENED` (4),
and tells the owner when the answers change (`C_MissionAnswers`). On the
owner the two are widened: its own garage's no becomes yes when a
participant's garage holds the car, or had a respray nobody has asked about
yet, which the asking takes. `DOES_GARAGE_CONTAIN_CAR` and `IS_GARAGE_CLOSED`,
a site each, stay the owner's.

Garage *types* matter more than they look. `protocol.md` §1.16.3's union is
"away from where this type rests", so two machines that disagree on a garage's
type disagree on what the bit means. Van Heist turns the Securicar lock-up
into a collect garage for good (`CHANGE_GARAGE_TYPE_WITH_CAR_MODEL`), so
garage types belong to the campaign delta (R10).

### R6. A car's bomb travels with the car

Five missions:

- Mike Lips Last Lunch (8-Ball's bomb shop, then Lips' own ignition);
- Blow Fish;
- Kanbu Bust-Out (the timed bomb that blows the station wall);
- I Scream, You Scream (the remote);
- Last Requests.

`IS_CAR_ARMED_WITH_BOMB` reads the owner's copy. A bomb shop arms the copy on
whichever machine's player drove in. So a helper who drives Lips' car into
8-Ball's arms a car the owner's script can't see, and the mission stops at
"The car bomb's not set!".

The bomb (`m_bombType` and its rigger) has to travel as vehicle state, like
health, and it has to go off on the machine simulating the car. The explosion
already travels once it happens.

**Built** (2026-09-24, `protocol.md` §1.29): a session car's bomb type is
vehicle state now (`C_VehicleBomb`). Whoever simulates the car, its driver or
with nobody driving it the player settling it, says when it changes: a bomb
shop's fitting, or the timer set going at the wheel. Everybody else writes it
on their own copy, so the owner's `IS_CAR_ARMED_WITH_BOMB` reads what the
helper's bomb shop fitted. The car goes up on the machine that simulates it,
as it would have, and that blow-up already travels; a copy elsewhere whose
own timer runs out is held back like any blow-up that machine does not
decide. The mission's own `ARM_CAR_WITH_BOMB` and `SET_FREE_BOMB_SHOP` reach
everybody, so Blow Fish's truck carries its bomb whoever drives it, and
8-Ball's is free for the helper too.

The rigger travels too now (`protocol.md` §1.36): every copy
names the bomber's ped, a lit fuse burns on every copy, and a detonator press
sets off the bomber's remote bombs on whichever machine simulates the car. A
bomb the mission fits used to be the one case the session could not name the
bomber for: `ARM_CAR_WITH_BOMB` runs on every machine and each named its own
player, so the bomb became the player whose machine first held the car. It is
the owner's now (`protocol.md` §1.36, `C_MissionBomb`): the owner's machine
tells the session, and a participant's replay names the owner, not itself. In
I Scream, You Scream the van is the owner's on every screen, and anybody in
the mission may press the detonator the mission hands out; the blast is
still the owner's, as `UseDetonator` blames the rigger. Whoever the bomb is
blamed on is paid for the car it destroys, once, by the machine the car goes
up on, under money `own` or `shared`. Not yet run in-game.

### R7. The engine's own mission machines

| Machine | Missions | Need |
|---|---|---|
| Drug-run Cessna (`START_DRUG_RUN`), drop-off Cessna (`START_DRUG_DROP_OFF`) | S.A.M., A Drop In The Ocean | Created on every machine, and `planetime.h` already aligns them in the air. "Shot down" gets answered from whichever machine's rocket hit it. |
| The Catalina helicopter | The Exchange | Joins the helicopter sync, which already streams the police helicopter and forwards hits on it (`C_HeliHit`). "Shot down" gets answered by its owner. |
| The RC buggy | the four RC missions, Toyminator | Only the owner drives it. Everybody else sees it as an ordinary car, never `STATUS_PLAYER_REMOTE` (`protocol.md` §1.4). |
| Power-pill pickups (`START_PACMAN_SCRAMBLE`) | Bullion Run, Big'N'Veiny | A separate engine collector outside `CPickups`. Only the owner's car collects. They're drawn on the owner's screen only unless replayed, and a replay would collect on the wrong machine. Owner only. |
| The crusher crane | Dead Skunk In The Trunk, The Crook | Cranes aren't synced. If a helper drops the car in, two machines' cranes may each grab their own copy, and the copy is being pinned to the session transform. The owner delivers the car until this has been seen in a game (§5). |

**Built** (2026-09-24): the two Cessnas. `START_DRUG_RUN` and
`START_DRUG_DROP_OFF` are replayed to every participant, whose own copy
flies on the session's clock (`game/planes.cpp` shifts both start times). A
participant's machine watches its copy once the owner's start of it has run
there, and says when its `HAS_DRUG_PLANE_BEEN_SHOT_DOWN` or
`HAS_DROP_OFF_PLANE_BEEN_SHOT_DOWN` goes true (`C_MissionAnswers`); the owner's
two conditions are widened by it. So a helper's rocket brings S.A.M.'s plane
down, and a helper shooting the drop-off plane fails A Drop In The Ocean. The
owner's own copy flies on after a helper brought theirs down, beside the
wreck the script then makes, until it lands.

**Built** (2026-09-29): the Catalina helicopter, the Cessnas' way. It is
not one of the mission's entities: GenerateHeli builds it as a permanent
vehicle inside UpdateHelis, outside any instruction, so nothing hosts it and
there is no second copy. `START_CATALINA_HELI`, `CATALINA_HELI_TAKE_OFF`,
`CATALINA_HELI_FLY_AWAY` and `REMOVE_CATALINA_HELI` are replayed (a take-off
before a participant's copy is up waits for it: the handler writes through
the slot unchecked), the marker and the camera on `$ESCAPE_CHOPPER` name each
machine's own copy, and a participant's copy going down answers the owner's
`HAS_CATALINA_HELI_BEEN_SHOT_DOWN` through `C_MissionAnswers`. Somebody
else's replayed rounds and rockets pass each machine's own copy by.

**Built** (2026-09-29): the power pills, drawn only. The owner sends each slot
of its `CPacManPickups` table as it changes; a participant writes it into its
own table, whose `Render` draws it, and its `CPacManPickup::Update` call is
taken away from them, so only the owner's car collects.

**Checked, not seen:** the RC buggy. `GIVE_REMOTE_CONTROLLED_CAR_TO_PLAYER`
makes a `MISSION_VEHICLE` inside the instruction, which the `CWorld::Add`
detour hosts as one of the mission's cars (its driver is nobody, so the claim
path does not take it). A participant builds an ordinary replica, `ABANDONED`
and `PHYSICS` only with somebody at its wheel, never `STATUS_PLAYER_REMOTE`;
`BLOW_UP_RC_BUGGY` goes through the `BlowUpCar` detour like any hosted car.
What a game shows of it is still to be seen (§5).

Not built, and why:

- **The crusher crane.** As above: the owner delivers until a game has shown
  two machines' cranes with one car.

### R8. Script explosions and fires

- **Explosions:** `ADD_EXPLOSION` at 36 sites in 6 missions: Kanbu's wall,
  Blow Fish's factory, Bomb Da Base's ship, Kingdom Come's human bombs, Silence
  The Sneak, Gone Fishing.
- **Fires:** `START_SCRIPT_FIRE` and its car and char siblings at 19 sites in
  4 missions.

`HookedAddExplosion` relays only explosions whose culprit is the local
player, and a script explosion has no culprit. So today they happen on the
owner's screen alone, and hurt nobody else. Both get replayed at their
position. The roadmap's §5.7 already names script fires as Area D's.

**Built** (2026-09-24): `ADD_EXPLOSION`, `START_SCRIPT_FIRE` and the car and
char fires, and `REMOVE_SCRIPT_FIRE`, on the replay list, each machine's fire
its own.

### R9. The script's commands to a car go to the machine simulating it

`APPLY_BRAKES_TO_PLAYERS_CAR` (22 sites in 8 missions) and the car commands a
mission gives a car somebody else is driving (`LOCK_CAR_DOORS`,
`SET_CAR_COORDINATES`, `SET_CAR_HEALTH`) land on the owner's copy. That copy
is overwritten by its driver's stream the next frame. They get forwarded to
the driver's machine, the way hits already are.

**Built** (2026-09-24, `game/replay.h`, `Kind::Holder`): `SET_CAR_COORDINATES`,
`SET_CAR_HEADING` and `SET_CAR_HEALTH` on a session car somebody else drives,
or settles while nobody drives it, go to that player alone, as the targeted
effect a late joiner gets, and their engine runs them on their own copy. On a
session car nobody holds they go to everybody, whose copies are each their
own. A car the owner's machine drives or hosts, the cars the mission made
among them, needs nothing: its stream carries what the owner's engine did.
`LOCK_CAR_DOORS` and `CHANGE_CAR_COLOUR` go to everybody, since a door is
tried on the copy of whoever tries to get in and a colour is what everybody
sees. `APPLY_BRAKES_TO_PLAYERS_CAR` goes to everybody too: it sets the pad's
`bApplyBrakes`, not the car's, so each participant brakes the car they drive,
the helper driving the owner included, and it is let off when the mission
ends.

The same class, found on 2026-09-24 with a participant at the wheel of Give
Me Liberty's Kuruma: `CHANGE_CAR_LOCK` (the other door lock, handler
0x0043ED92, the same store as `LOCK_CAR_DOORS`), `SET_CAR_ONLY_DAMAGED_BY_PLAYER`
and `SET_CAR_STRONG` go to everybody as well, so whoever ends up simulating
the car has what the mission made it stand up to. `SET_CAR_PROOFS` does not:
its collision bit is the one an observer keeps on its copy for itself.

And the mission's pedestrians ride along. A host now names a session car in
its pedestrian rows, so 8-Ball sits in the Kuruma a participant drives on
every screen instead of standing where his seat is, inside the car and in
its way; a seat one of them holds counts as taken for every player's entry;
and nobody is moved out of a seat for a copy of him (`game/seatplan.h`).

### R10. Permanent world changes are campaign state

- `SWAP_NEAREST_BUILDING_MODEL`, 32 sites in 7 missions.
- `SET_VISIBILITY_OF_CLOSEST_OBJECT_OF_TYPE`, 37 sites in 12 missions.

Between them they make the world the story leaves behind:

- the Callahan bridge wrecked in the first mission (EIGHT's `BRIDGEFUKA`
  objects) and **repaired in Last Requests**, which is the Staunton unlock:
  six bridge swaps, `SWITCH_ROADS_ON` over the bridge and the tunnel, and the
  tunnel block created;
- the fish factory destroyed in Blow Fish;
- the Colombian ship in Bomb Da Base;
- the police cell wall in Kanbu Bust-Out;
- the damaged store in Shima.

Add the permanent pickups (R2), the garage types (R5) and the objects a
mission leaves standing. `missions.md` §5.5 already sends the world opcodes
live; the audit says which ones, and that they're not optional. Without them,
the island unlocks happen on one machine.

### R11. Participants' kills count toward `rampage_kills`

Uzi Rider and Bait count kills with `GET_NUM_OF_MODELS_KILLED_BY_PLAYER`,
which is `CDarkel::RegisteredKills`.

- **Peds only count during a frenzy.** A helper's kill of an owner-hosted
  ped is credited through `CreditRemotePedKill`, which returns unless a frenzy
  is on (`darkel.cpp`). During a session mission it should always credit.
- **Cars already count.** The RC missions count cars, and a car wreck bumps
  `RegisteredKills` even on a replay (`rampage.md` §9.2), so they need
  nothing.

**Built** (2026-09-24, `protocol.md` §1.29): while the session's mission runs,
`CreditRemotePedKill` puts somebody else's killing hit through the engine's own
register on every machine in the mission, as it does in a frenzy. A kill a
participant's machine registers, its player's own or one it credited that way,
goes to the owner as `C_MissionKill`, and the owner's `RegisteredKills` moves by
one for the model, with none of the stats around it. The occupants of a wreck a
machine replays are left out, since the machine that decided the wreck
registers them. So a Diablo counts for Uzi Rider whichever machine hosted it
and whoever shot it. Also built (2026-09-29): a pedestrian a participant's grenade
or rocket kills on another machine, whose blast names the thrower's replica
there, the same gap a rampage has: its host finds the blast among the explosions
going off and credits it through the same register, once, on the one machine
that hosts the pedestrian (`darkel.cpp`, `missioncombat.h` KilledByRemoteBlast).

### R12. What the story does to "the player" happens to every participant

This follows from the decisions: everybody paid, everybody fails
together. The opcodes that change the protagonist's state apply to every
participant:

- the weapons a mission hands over (the sniper rifle in Bomb Da Base, the
  shotgun, the Uzi for Uzi Rider, Silence The Sneak's grenades);
- `REMOVE_ALL_PLAYER_WEAPONS` at The Exchange;
- the wanted levels a mission sets. Decoy sets 6 stars; Kanbu, The Getaway and
  Two-Faced Tanner set minimums;
- health, visibility, the teleports (§11.2), `PUT_PLAYER_IN_CAR` (a
  participant goes near the car, not into seats a mission ped needs), control;
- the hospital and police overrides, and EIGHT's critical restart point, so
  everybody wakes up in the same place.

The owner's *conditions* stay the owner's (C1).

**Built** (2026-09-24) on the replay list, but for `PUT_PLAYER_IN_CAR` and
EIGHT's critical restart point, which are still the owner's alone.

**Corrected** (2026-09-25): "built" wasn't true of the hospital and police
overrides. `OVERRIDE_HOSPITAL_LEVEL` and `OVERRIDE_POLICE_STATION_LEVEL`
(Bait, Espresso-2-Go, S.A.M., set in the failure branch after a death or an
arrest) weren't on the replay list. They are now. At the end a participant
keeps the override only while its own player is on the way to that restart,
and otherwise gets `LEVEL_GENERIC` back. Also added: the free Pay'n'Spray
(Luigi's second mission) and the Paramedic's and Firefighter's crime
sensitivity, both given back at the end; the Vigilante's "get out of jail
free", kept in the campaign like the Paramedic's reward; and the launch's
`MAKE_PLAYER_SAFE_FOR_CUTSCENE`, so helpers are frozen and unhurt through
the title's fade. With no cutscene after it, the end clears the pad's
CUTSCENE bit and cutscene processing itself.

`PUT_PLAYER_IN_CAR` (0369) since: once the session names the car the owner
was put in, the participants on foot get its free passenger seats, one each,
nearest first, less the ones the mission's pedestrians are heading for; the
rest stay where they are and follow in a car of their own. A boat has one
seat, so in Last Requests two of three stay ashore, and while the owner is on
the water his checkpoints don't wait for anybody without a boat
(`protocol.md` §1.40).

### R13. Detection is answered for every participant

`HAS_CHAR_SPOTTED_PLAYER` (13 sites in Sayonara Salvatore, and Grand Theft
Aero) and `IS_PLAYER_SHOOTING_IN_AREA` (Bomb Da Base, Trial By Fire, Rumble)
ask "has the player given the game away". Answered for the owner alone, a
helper can stroll past the Mafia outside Luigi's, or open fire near the
Colombian ship, and give nothing away.

These are single conditions, never chained, so "any participant" is safe
here.

**Decided: everybody has to be quiet.** Three stealth checks
aren't `HAS_CHAR_SPOTTED_PLAYER` at all. They're ordinary distance checks,
so they need a small table of sites, answered for the participant nearest the
danger:

| Mission | The check | Answered as |
|---|---|---|
| Cutting The Grass | the **Spookometer**. `LOCATE_PLAYER_ANY_MEANS_CHAR_3D` against Curly Bob at 40, 30 and 20 m raises `$SPOOKED_COUNTER` faster the closer the player is, and faster still in a Mafia car; at 100 "Curly's spooked! The meeting's off!" | the nearest participant's distance, and whether *that* participant is in a Mafia car |
| Deal Steal | arriving within 10 m of the rendezvous not in a Yardie car, or aggressive, gives "You have been spotted the deal is off!" | every participant who arrives has to be in a Yardie car and calm |
| Plaster Blaster | anybody within 25 m of the decoy ambulance: "You've been spotted!!" | any participant |

A site is named by mission and instruction offset. That's valid for the stock
`main.scm`, which the session hashes anyway (`missions.md` §5.5). On a
modified script the table doesn't apply and those checks stay the owner's.

**Built** (2026-09-24): the two conditions for any participant, and the
Spookometer, named by its mission rather than by offsets (every locate against
a pedestrian in Cutting The Grass is against Curly). Deal Steal and Plaster
Blaster are built too (2026-09-29, missioncombat.h `stealth`): a
participant within 10 m of Deal Steal's rendezvous out of a Yardie car or
shooting, or within 25 m of Plaster Blaster's decoy, gives it away.

### R14. The horn, probably already

Four missions wait on `IS_PLAYER_PRESSING_HORN`:

- Drive Misty For Me;
- The Getaway;
- Salvatore's Called A Meeting;
- Deal Steal.

re3's handler reads the horn timer of the car the player is in, for any seated
player. CoopIII writes a driver's horn onto every copy of the car after
`CGame::Process` (`horn.h`), and the script reads it at the start of the next
frame. So an owner riding while a helper honks should pass. That's to be
seen in a game (§5).

### R15. The world opcodes run on every machine

The population is shared, but each machine generates its own part of it from
its own zone tables. These opcodes go to every machine:

- `SET_ZONE_CAR_INFO`, `SET_ZONE_PED_INFO` (gang info), `SET_GANG_WEAPONS`;
- `SET_MAX_WANTED_LEVEL`, and the SWAT, FBI and army switches;
- `SWITCH_ROADS_ON/OFF`;
- `SWITCH_WORLD_PROCESSING`;
- the ped and car density multipliers;
- `CLEAR_AREA`.

Without them the owner's machine fills the Towers with Diablo cars for the RC
mission while the helpers' machines, generating the traffic around them, keep
the ordinary mix. `missions.md` §13 had the last three; the audit adds the
rest.

**Built in part** (2026-09-25, `missions.md` §15, "the streets"): `CLEAR_AREA`,
both density multipliers, `SET_ZONE_CAR_INFO`, `SET_ZONE_PED_INFO` and
`SET_GANG_WEAPONS` go to every machine; the zone and gang ones are in the
campaign delta too. A participant puts its densities back when the mission
ends, and its zones and gangs when the owner's cleanup never reached it.
`SET_MAX_WANTED_LEVEL` and the SWAT, FBI and army switches are still the
owner's alone; the roads were already replayed as world instructions.
`SWITCH_WORLD_PROCESSING` (Bait, Espresso-2-Go, S.A.M.),
`SET_ALL_CARS_CAN_BE_DAMAGED` (six missions), Chaperone's
`SET_GENERATE_CARS_AROUND_CAMERA` and `CLEAR_AREA_OF_CHARS` (the Meat Business
missions, Luigi's) go to every machine as well (2026-09-25). The first three
are put back at the end if the mission left them changed.

### R16. The mission's HUD widgets are on everybody's screen

41 missions put something of their own on the HUD:

- **28 show an on-screen timer**, `DISPLAY_ONSCREEN_TIMER`, sometimes
  paused with `FREEZE_ONSCREEN_TIMER`: the 4x4 runs, Mike Lips' countdown,
  Payday For Ray, the time till Ray's flight...
- **17 show a number counter**: KILLS, FARES, FIRES, GIRLS, COLLECTED,
  BUGGIES LEFT, RACE TIME.
- **10 show a bar**, which is a counter of type 1:
  - DAMAGE on the security van, Blow Fish's truck, the spy boat, Tanner, the
    partner's boat, the ambulance and the bodycast, the escort truck and the
    decoy van;
  - DETONATION on Rigged To Blow's car;
  - and Cutting The Grass's **SPOOKOMETER**.

None of them is drawn from a value the opcode passes. `DISPLAY_ONSCREEN_TIMER`
and `DISPLAY_ONSCREEN_COUNTER_WITH_STRING` register a *global variable*, and
every frame the HUD reads that variable out of `ScriptSpace`. For a timer,
the HUD also counts the variable down itself (re3's `COnscreenTimer`, to be
confirmed on retail). So replaying the opcode (§5.4) puts the widget on a
helper's screen, but showing the helper's own copy of the variable, which
nothing on their machine updates. The spookometer would sit at zero, and the
DAMAGE bars would never move.

What's needed is a stream of the displayed variables:

- **What travels.** While a widget is up on the owner's screen, the owner
  sends its variable's value, on change and a few times a second at most.
- **What the helper does.** Each helper writes it into the same global
  before its HUD draws.
- **Timers.** A timer keeps counting down on its own between two values, so
  it's smooth and only drifts by a round trip. The owner's value still wins,
  which is what carries the checkpoint bonuses of the 4x4 runs and the pauses
  of The Fuzz Ball and Payday For Ray.
- **Widgets that aren't the script's.** The rampage counter is `CDarkel`'s
  own HUD and is already on every screen (`rampage.md`). So are Trial By
  Fire's and Uzi Money's frenzies, once their `START_KILL_FRENZY` is
  replayed.

**Built** (2026-09-24, `protocol.md` §1.29, `C_MissionWidget`): the owner's
machine sends the displayed global's value on change, or for a timer when it
leaves the countdown, at most ten times a second and at least every two
seconds, and each participant writes it into the global its own HUD shows.

### The design corrections

- **C1. Who has to satisfy a condition**, as decided on
  2026-09-24. It replaces `missions.md` §5.6.

  **The start: everybody.** Each of the 85 launches has a *start gate*, the
  last player condition its trigger asks before the `0417`:

  | Kind of mission | Its start gate |
  |---|---|
  | story contacts and payphones | `CAN_PLAYER_START_MISSION` |
  | odd jobs | the sub-mission button |
  | RC | the van's locate |
  | 4x4 and Mayhem | the vehicle and zone checks |

  A scan of the script at load finds them, the way §5.2 of `missions.md`
  tells a launch from a save point. The owner satisfies the gate as written.
  Every other connected player has to be inside the start's area, or no more
  than 5 m outside it (§6). The start's area is the last one its trigger
  checks: the marker for a contact or a payphone, the van's spot for RC,
  where the car waits for the 4x4 and Mayhem runs. An odd job's button works
  anywhere, so for an odd job the area is the owner. Until everybody is
  there, CoopIII answers the gate false and the HUD lists who's missing. The
  loop just asks again, as it always does.

  **Checkpoints: everybody.** A location or area condition of the mission
  counts as a checkpoint when its area holds one of the mission's active
  coordinate blips, the checkpoint on the radar:
  - 54 such checks are on a blip at literal coordinates;
  - 160 more have centres held in variables, which only resolve at run time.
    Those include every 4x4 and Mayhem checkpoint and the taxi's
    destinations.

  The owner satisfies the check as written. Everybody else has to be inside
  the area, however they got there, or no more than 5 m outside it, so that
  a friend parked beside the owner in their own car counts. "Not arrived"
  means somebody's missing, so the script simply keeps waiting.

  **Stealth: anybody.** Any participant can give the game away (R13).

  **Everything else: the owner**, as protagonist. That's getting into the
  mission car, reaching a character, touching the cargo, lifting a phone.
  Checks "near a mission character" aren't checkpoints: a blip on a car or a
  ped isn't a coordinate blip.
- **C2. The owner drives the mission car** until R4, R5, R6 and R9 exist
  (§1.4). All four are built now, none of them yet seen in a game (§5), so
  until one has shown them, the owner at the wheel stays the safe way.
- **C3. One mission at a time for the whole session, and everybody is in
  it.** This follows from C1's start rule. A mission only starts with
  everybody at its start, so there's never a second one, and nobody is ever
  left out of one. That includes the odd jobs and the RC, 4x4 and Mayhem
  runs: a taxi shift is the whole session's taxi shift, and every
  participant's `$ONMISSION` is set for it.
- **C4. A failed mission sends a delta too.** Failures change campaign state:
  EIGHT sets `$FLAG_REACHED_HIDEOUT`, which moves both where it restarts and
  which marker starts it again, and every mission bumps
  `INCREMENT_MISSION_ATTEMPTS`.
- **C5. How a New Game starts.** The intro (mission 0) is launched
  unconditionally by the main script at a new game, so every machine plays it
  locally, and it isn't a session mission. Everybody then spawns on
  EIGHT's marker (811.875, -939.9375), and `CAN_PLAYER_START_MISSION`'s claim
  picks one owner, exactly as for any contact.

---

## 3. Mission by mission

Codes: **ok** works with the base bridge; **reqs** works once the listed requirements
exist; **note** plays, but see the note. "Owner drives" means C2.

Two requirements apply across the board, so they're left out of the rows:

- C1's start and checkpoint rules apply to every mission;
- R16 applies to the 41 missions with a timer, a counter or a bar.

### Local and personal

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 0 | Intro | the new-game movie | nothing (C5) | **ok** local on every machine |
| 1-2 | Hospital / police info scenes | an info pickup, then a scene | nothing | **note** the shared-pickup catch in `missions.md` §6 |
| 3-6 | RC Toyz (Diablo, Mafia, Rumpo, Casino) | blow up as many cars as possible with the RC buggy in 2 minutes | R7 (buggy), R15 (zone car info) | **ok** owner only. A helper's wreck of a target counts, as any wreck does in vanilla |
| 7-9 | Patriot Playground, A Ride In The Park, Gripped! | checkpoints in a Patriot or a Landstalker | nothing | **note** only the owner's car collects. Helpers watch |
| 10 | Multistorey Mayhem | checkpoints in a Stallion | nothing | **note** as above |
| 11 | Paramedic | patients into the ambulance | R4 | **note** owner drives. Everybody's in it (C3) and escorts |
| 12 | Firefighter | put out burning cars | R8 (car fires) | **note** owner drives the fire truck |
| 13 | Vigilante | kill the criminals | R1 | **ok** helpers can shoot criminals once R1 exists |
| 14 | Taxi Driver | fares into the taxi | R4 | **note** owner drives. Helpers can't take fares |

### Marty Chonks (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 15 | The Crook | bank manager into your car, to the crusher | R4, R7 (crusher), R3 (factory door) | **reqs** owner drives and delivers the car to the crusher |
| 16 | The Thieves | thieves to the dog food factory, respray, back | R4, R5 (respray), R3 | **reqs** owner drives |
| 17 | The Wife | Mrs Chonks, dump the car in the sea | R4, R3 | **reqs** |
| 18 | Her Lover | pick up the lover | R4 | **reqs** |

### Luigi

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 19 | Give Me Liberty & Luigi's Girls | escape with 8-Ball, reach the hideout, take Misty to Luigi's | C5, C4, R4 (8-Ball, Misty), R10 (bridge wreck objects, swaps), R12 (critical restart point), clock | **reqs** starts by itself at a New Game, arbitrated like any contact |
| 20 | Don't Spank Ma Bitch Up | bat pickup, beat the dealer, respray his car, stash it in Luigi's lock-up | R2, R5 (respray and lock-up) | **reqs** owner drives the car to the garages |
| 21 | Drive Misty For Me | horn outside the hospital, Misty to Joey's | R4, R14 | **reqs** owner drives |
| 22 | Pump-Action Pimp | free Colt pickup, kill the pimp | R1 (pimp's car), R2 | **reqs** |
| 23 | The Fuzz Ball | round up four girls into your car in time | R4 (24 group sites) | **reqs** the girls follow the owner, so the owner does the driving |

### Joey

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 24 | Mike Lips Last Lunch | Lips' car to 8-Ball's bomb shop and back, lose the cops, arm it | R6, R12 (wanted) | **reqs** anybody can drive it in (R6) |
| 25 | Farewell 'Chunky' Lee Chong | Colt pickup, kill Chunky | R1 (Chunky and his Triads), R2 | **reqs** without R1, only the owner can hurt Chunky |
| 26 | Van Heist | ram the security van, take it to the lock-up | R1 (the van), R5, R10 (garage type) | **reqs** |
| 27 | Cipriani's Chauffeur | drive Toni's mother; Ammu-Nation starts selling the Uzi | R4, R12 (warp into car), R3 (doors), R2 and R10 (the Uzi unlock) | **reqs** |
| 28 | Dead Skunk In The Trunk | shake the Forellis, the car to the crusher | R1 (Forelli car), R7 (crusher) | **reqs** owner delivers the car |
| 29 | The Getaway | three thugs into a four-seat car, horn, bank doors, lose the cops | R4 (every seat), R14, R3 (bank doors), R9 (brakes), R12 (wanted) | **reqs** owner drives, nobody rides with him |

### Toni

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 30 | Taking Out The Laundry | destroy the laundry vans | R1 (vans), R2 (grenades) | **reqs** without R1 helpers can't destroy vans |
| 31 | The Pick-Up | briefcase, ambush, cash back to Toni's | R2 | **ok** once R2 exists |
| 32 | Salvatore's Called A Meeting | Joey, Luigi and Toni in the limo, horn, repair, Salvatore's garage | R4 (every seat), R14, R5, R3 | **reqs** owner drives, nobody rides |
| 33 | Triads And Tribulations | shotgun, Triad fish van, kill the Triads | R1, R12 (shotgun), R15 (zone gang info) | **reqs** |
| 34 | Blow Fish | the bomb truck into the fish factory | R6, R8 (explosions, fire), R10 (factory swap), clock | **reqs** |

### Salvatore

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 35 | Chaperone | drive Maria in the Stretch, the raid, back to Salvatore's | R4 (Maria), R12 (warp into the Stretch), R5, R9 | **reqs** owner drives |
| 36 | Cutting The Grass | Curly Bob in your taxi, then tail him without spooking him | R4 (Curly), R1, R13 (the Spookometer), R16 (its bar) | **reqs** everybody has to keep their distance |
| 37 | Bomb Da Base: Act I | the briefing | nothing | **ok** |
| 38 | Bomb Da Base: Act II | $100,000, 8-Ball, snipe the guards, the ship blows | R12 (sniper rifle for everybody), R13 (shooting in the area), R8, R10 (ship swap) | **reqs**. The money check is the owner's |
| 39 | Last Requests | the rigged car, the boat with Maria and Asuka; **the Staunton unlock** | R6, R12 (warp into the boat), R1 (Maria, Asuka), R10 (bridge, roads, tunnel) | **reqs** without R10 only the owner gets Staunton |

### El Burro (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 40 | Turismo | race three cars, finish first | R1 (the racers), R15 (roads) | **ok** the owner has to win; helpers can ram the racers once R1 exists |
| 41 | I Scream, You Scream | briefcase (detonator), ice cream van, jingle, detonate | R2 | **ok** owner drives the van. The script arms it, so the owner's copy has the bomb |
| 42 | Trial By Fire | flamethrower, 25 Triads in a frenzy | R2, R13 | **ok** replaying `START_KILL_FRENZY` hands it to `rampage.md`'s machinery |
| 43 | Big'N'Veiny | follow the van and pick up the magazines | R7 (power pills) | **note** only the owner's car collects |

### Asuka

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 44 | Sayonara Salvatore | wait unseen, then kill Salvatore | R1 (Salvatore, the Mafia cars), R13 (13 spotted checks), R5 (his garage), R15 | **reqs** without R1 a helper can't make the kill; without R13 a helper can't be spotted |
| 45 | Under Surveillance | kill the surveillance team | R1 (their van) | **reqs** |
| 46 | Paparazzi Purge | chase the spy boat | R1 (the boat) | **reqs** |
| 47 | Payday For Ray | payphone to payphone against the clock | nothing | **note** the phones are the owner's |
| 48 | Two-Faced Tanner | kill Tanner | R1, R12 (wanted), R15 (max wanted) | **reqs** |

### Kenji

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 49 | Kanbu Bust-Out | a cop car, the bomb shop, blow the cell wall, Kanbu to the dojo | R6, R4 (Kanbu), R8, R10 (wall), R12 (wanted) | **reqs** owner drives the cop car |
| 50 | Grand Theft Auto | three cars, mint, into Kenji's lock-up | R5 | **reqs** anybody delivers each car (R5) |
| 51 | Deal Steal | Yardie car, the contact, horn, kill the Colombians, the briefcase | R4, R14, R1, R2, R13 (the rendezvous) | **reqs** everybody arrives in a Yardie car |
| 52 | Shima | the protection money, punish the gang | R2, R10 (store swap) | **reqs** |
| 53 | Smack Down | kill at least 8 dealers | R1 (every dealer) | **reqs** without R1 only the owner can kill a dealer |

### Ray (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 54 | Silence The Sneak | grenades, torch the house, kill McAffrey | R12 (grenades), R8, R5 | **reqs** the ammo checks are the owner's grenades |
| 55 | Arms Shortage | Phil's weapons, defend the condo | R2 (Phil's weapons for everybody), R3 (gate), R1 (the attacking cars), R9 | **reqs** R1 is the whole point of a defend mission with helpers |
| 56 | Evidence Dash | chase the prosecution, collect the evidence, torch the car | R3 (files riding in the car), R1 | **reqs** |
| 57 | Gone Fishing | a police boat, kill Ray's partner at the lighthouse | R3, R8, R1, clock | **reqs** |
| 58 | Plaster Blaster | smash the bodycast with a car or a blast | R3 (object damage), R1 (the ambulance), R12 (wanted), R13 (the decoy's 25 m) | **reqs** |
| 59 | Marked Man | Ray to the airport, the stash | R4 (Ray), R1 (the CIA), R3 (doors), R2 (the stash for everybody), R5 | **reqs** everybody takes the whole stash |

### Donald Love

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 60 | Liberator | a Colombian car through the gate, rescue the gentleman | R5 (the gate and garages), R4 | **reqs** owner drives |
| 61 | Waka-Gashira Wipeout! | Colombian car, kill Kenji, dump the car | R12 (wanted), R9 | **ok** |
| 62 | A Drop In The Ocean | follow the Cessna by boat, 18 floating packages | R7 (Cessna), R2 (floating packages) | **reqs** anybody's boat collects a package for the mission (R2) |
| 67 | Grand Theft Aero | the airport, the construction lift (a cutscene), the package | R13 (spotted), R12 (the teleport after the lift) | **reqs** |
| 68 | Escort Service | protect the truck | R1 (the hitmen), R2, R5 | **reqs** |
| 69 | Decoy | six stars, lead the cops away | R12 (everybody gets the stars), R15 (max wanted) | **ok** once R12 exists |
| 70 | Love's Disappearance | cutscene | nothing | **ok** |

### King Courtney (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 63 | Bling-Bling Scramble | race | R15 (roads) | **ok** the owner has to win |
| 64 | Uzi Rider | drive-bys with the Yardies in your car | R11, R12 (Uzi), R4, R1 | **reqs** everybody's kills count (R11) |
| 65 | Gangcar Round-Up | three gang cars to the lock-up | R5 | **reqs** anybody delivers (R5) |
| 66 | Kingdom Come | stop the SPANKed-up vans | R8 (the human bombs) | **reqs** |

### Asuka, Staunton and Shoreside

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 71 | Bait | lure the Colombians into the ambush | R1, R11, R15 | **reqs** everybody's kills count (R11) |
| 72 | Espresso-2-Go! | wreck 15 stalls on three islands | R3 (object damage), R8 | **reqs** without R3 the helpers' wrecking doesn't count |
| 73 | S.A.M. | boat to the buoy, rocket the Cessna, the cargo | R2 (rocket), R3, R7 (Cessna) | **reqs** the cargo is the owner's to touch (C1) |

### D-Ice (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 74 | Uzi Money | 20 Nines with Uzi drive-bys, a frenzy | frenzy replay | **ok** `rampage.md` carries it |
| 75 | Toyminator | RC buggy against the armoured vans | R7 | **note** the vans only take the buggy's damage, and only the owner has one |
| 76 | Rigged To Blow | the rigged car to the defusal garage, no damage | R5 | **reqs** owner drives |
| 77 | Bullion Run | 30 pieces of bullion in a car, drop-offs | R7 (power pills), R5 | **note** only the owner's car collects |
| 78 | Rumble | bat, the contact, the gang war | R1, R2, R13 | **reqs** |

### The finale

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 79 | The Exchange | the weapons taken, the dam, the chopper, the credits | R12 (everybody disarmed), R7 (Catalina's helicopter), R2 (the rocket launcher, one), R15, clock, the credits replayed | **reqs** anybody who hits the chopper counts once R7 exists |

---

## 4. What to build first

By how many missions each requirement opens up, and by what it costs:

1. **R10 and R15 with the campaign delta.** Without them there's no shared
   campaign: the islands unlock on one machine.
2. **R1.** One seam, 28 missions, and it's what makes a helper worth bringing.
3. **R16.** 41 missions show a widget, and the variable stream is small.
4. **R2 and R3.** They reuse the pickup sync and §5.4's replay, and cover 19
   and 16 missions.
5. **C1's start gates and checkpoints, R12 and R13.** Short opcode lists
   and one scan of the script. The host's kick they need is built (§6).
6. **R4, R5, R6, R9.** These are what C2 costs, and they can wait. Until they
   exist, the owner drives.
7. **R7, R8, R11, R14.** A few missions each.

---

## 5. What only a game can settle

- R1's ped half: that retail `CPed::InflictDamage` refuses a non-player
  culprit for `bOnlyDamagedByPlayer` peds, as re3 does.
- C2's premise: that retail `IS_PLAYER_IN_CAR`, `LOCATE_PLAYER_IN_CAR_*` and
  `FindPlayerVehicle` accept a passenger, as re3 does.
- R14: that the horn written after `CGame::Process` is what the next frame's
  script reads.
- R7's crusher: what two machines' cranes do with one car.
- R7's Cessnas: whether a rocket replayed on the owner's machine meets the
  plane's own collision test.
- The garage state machines with a passenger owner (R5): which of the four
  conditions already come out right.
- R16: that the retail HUD reads a timer's and a counter's variable out of
  `ScriptSpace` every frame, as re3's `COnscreenTimer` does.

---

## 6. What was decided

Decided on 2026-09-24:

1. **Everybody at the start and at every checkpoint** (C1). That also
   settles the sub-missions: one mission at a time, everybody in it (C3).
2. **Ray's stash, Phil's weapons and the like: everybody takes their own, and
   nothing disappears for the others** (R2).
3. **Everybody follows Curly quietly.** The Spookometer, Deal Steal's
   rendezvous and Plaster Blaster's decoy are answered for the nearest
   participant (R13), and the Spookometer's bar is on everybody's screen
   (R16).
4. **Being there means inside the area, or no more than 5 m outside it**, at
   the start and at every checkpoint (C1). The 5 m is a server setting, and
   that's its default.
5. **Somebody who never comes is for the host to kick, from inside the
   game.** The rule stays as it is. A player somewhere else stops every
   mission from starting, which is what's wanted while they're on their way.
   When they aren't coming, the host throws them out from the player list:
   `/kick` and the list's number, in the chat (`protocol.md` §1.28).

Nothing the audit raised is left open.
