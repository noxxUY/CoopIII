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
  would for the owner. In an `if and` that asked a location of the owner
  earlier in the same block ("within 20 m of the destination *and* in
  $CAR_MEAT1"), only when the car itself is inside that location's area, 5 m
  of slack included (`InCarAtThePlace`, second pass below): the owner at the
  place, as the location said, and a participant bringing the car in beside
  him is one state that satisfies both halves. The "Hey! Get back in the
  vehicle!" the mission prints then only goes up when nobody is in the car.
- Location checks: already everybody's at a checkpoint, by where each
  participant is, whatever he drives (§5.6 of missions.md, 5 m of slack), so
  a helper in his own car at the place counts. Not changed.
- `IS_PLAYER_IN_ANY_CAR`, `IS_PLAYER_IN_MODEL`, `STORE_CAR_PLAYER_IS_IN`:
  not widened. Every "get a vehicle" wait is followed by `STORE_CAR_PLAYER_IS_IN`
  (Drive Misty For Me, 21_luigi3.sc:208-211), which reads the owner's car; a
  yes for a helper's car would hand the script no car at all.
- `IS_CHAR_IN_CAR` on a mission pedestrian: not widened. It is about her,
  and R4 above is what gets her in.

**Second pass, every "get back in" message (2026-09-30).** noxx asked for
every mission that tells its player to get back into the vehicle to take any
participant in it, the owner or a guest, at the wheel or riding. Read off the
retail main.scm, every one of them is behind the same condition:

| text | sites | missions (script) | the condition before it |
|---|---|---|---|
| `IN_VEH` "Hey! Get back in the vehicle!" | 50 | The Crook, The Thieves, The Wife, Her Lover (MEAT1-4); Give Me Liberty and Luigi's Girls (EIGHT, Misty's car too); Don't Spank Ma Bitch Up (LUIGI2); Mike Lips Last Lunch, Van Heist, Cipriani's Chauffeur, Dead Skunk in the Trunk (JOEY1, 3, 4, 5); Salvatore's Called a Meeting, Blow Fish (TONI3, 5); Cutting The Grass (FRANK2) and FRANK4; I Scream You Scream, Big 'n' Veiny (DIABLO2, 4); Rigged To Blow (HOOD3) | `NOT IS_PLAYER_IN_CAR` on the mission's car, alone or beside a flag |
| `FM1_1` "Get back into the Stretch!" | 7 | Chaperone (FRANK1) | `NOT IS_PLAYER_IN_CAR $MARIAS_STRETCH` beside a flag |
| `YD2_N` "Get your ass back in this car!" | 1 | Uzi Rider (YARD2) | `NOT IS_PLAYER_IN_CAR` on Courtney's car (offset 2754) |

`EBAL_2` and `LM1_6` are in american.gxt and in no script. `IN_VEH2` "You
need some wheels for this job!" is another question (any car: Bullion Run
and two odd jobs), behind `IS_PLAYER_IN_ANY_CAR` and followed by
`STORE_CAR_PLAYER_IS_IN`, whose car Bullion Run hands to a garage
(`set_garage $360 to_accept_car`): left as it is, for the reason above. No
"get back in" message is behind `IS_PLAYER_IN_MODEL`, `IS_PLAYER_IN_ANY_CAR`,
a `LOCATE_PLAYER_IN_CAR_*` or `IS_PLAYER_SITTING_IN_CAR`, and no mission
compares a stored player's car with one of its own (`003A` against an `00DA`
result appears nowhere), so `IS_PLAYER_IN_CAR` is the whole of it. The two
places a mission stores its player's car right after asking
`IS_PLAYER_IN_CAR` are Cutting The Grass (offset 2675, behind
`IS_PLAYER_IN_MODEL` taxi, storing over the taxi it already names) and
Bullion Run (offset 1168, behind `IS_PLAYER_IN_ANY_CAR`); neither needs a
participant's car.

What changed in this pass:

- The `if and` with a location first: seven blocks, The Crook's crusher
  hint, Give Me Liberty's three arrivals, Don't Spank Ma Bitch Up's two and
  Cutting The Grass's stop at the door. They are now answered for a
  participant in the car when the car stands in the area that location
  check asked about (`InCarAtThePlace`). The location itself is still the
  owner's (and everybody's at a checkpoint), so the block passes only with
  the owner there too.
- That location only counts inside its own block: an `if` (00D6) forgets
  it. No block in main.scm put an earlier block's location ahead of an
  `IS_PLAYER_IN_CAR` by the and/or counter, so this changes no answer
  today; it stops one block's locate from being read into the next.
- A participant is in the car when the session seats him in it *or* when
  his copy sits in it on the owner's machine (`ReplicaSeatedIn`): the
  session's word lags his copy by a round trip, and a car the owner still
  hosts as the mission's has no session netId to compare yet.
- Uzi Rider's `YD2_N` joins `IN_VEH` and `FM1_1` as a message that stays the
  owner's and is told to each participant who gets out (`IsGetBackInLabel`).

**One exception (2026-10-01, Kenji step by step in §3).** Deal Steal and
Shima end at the casino with a scene that turns the controls off (on every
machine), tells the owner's own ped (`GET_PLAYER_CHAR`) to leave the car he
came in, and waits `while is_player_in_car` that car. A participant riding or
driving with the owner, frozen in his seat, answered it yes for ever. The car
the owner's own ped was ordered out of is now the owner's alone to answer for,
until the script gives him his controls back (`standin.h`, `WalkedOut`).

### R4c. A session car the mission clears away goes from every screen (2026-09-30)

A playtest of Cipriani's Chauffeur softlocked at its start. The mission
opens with `0395: clear_area 1 at 1195.0 -870.25 range 15.0 10.0`
(27_joey4.sc:111), which in single player takes away the car its player
came in, so Toni's Mafia car can drive out of Joey's garage. The car the
players came in was a session car, and a copy of a session car is a locked
`MISSION_VEHICLE` on every machine but the one whose engine made it
(`game/carlife.h`): the clear passes over a locked car before it asks
`CanBeDeleted` (0x004B4FAF), so no machine took it, and the one that might
have had a player in it or on its roof. It stayed in front of the door.

Now (`game/mission.cpp`, `ClearArea` and `MissionDeletesCar`;
`game/mission.h`):

- **The owner's mission decides.** A `CLEAR_AREA` from it takes every session
  car inside its circle that is not one of the mission's own cars, holds
  none of the mission's people, is not a wreck and is not the car the
  owner's own player sits in (`ClearTakesCar`). A `DELETE_CAR` from it on a
  session car the owner is not in does the same for that car.
- **Everybody out first.** The owner's machine says so to every participant
  (`LeaveCarEffect`, protocol.md 1.63), and each machine puts its own player
  out of that car, by `WARP_PLAYER_FROM_CAR_TO_COORD`, or off its roof, by
  `SET_PLAYER_COORDINATES`, beside it on the side the buildings leave clear,
  on the ground. The owner's own player is moved off its roof the same way.
- **The mission waits for it.** The instruction is held, run again next
  frame, while another player still sits in one of those cars by the
  session's word or by his copy, `CLEAR_HOLD_MS` (3 s) at most. Then each car
  nobody sits in is taken away through `C_VehicleRemoved`
  (`VEHICLE_REMOVED_MISSION`), which deletes it on every machine, the one
  whose engine made it included. A car still occupied after the wait is left,
  as before, and this mission does not wait for that car again.
- **No engine clears a session car by itself.** For the length of the
  owner's `CLEAR_AREA`, and of each participant's replay of it, every session
  car is locked, so the engine's own clear can neither take a car with a
  player in it (its car loop removes whoever sits in a car it takes) nor one
  the session would rebuild.

`MARK_CAR_AS_NO_LONGER_NEEDED` deletes nothing: the car becomes the engine's
to reap, and the reapers skip a locked copy and a car a player is in, so a
car a participant occupies stays his. Not changed. Before this pass the four
missions whose `DELETE_CAR` names a car the player was made to use
(Chaperone's Stretch, Salvatore's Called a Meeting's limo, Cipriani's
Chauffeur's car on a retry, Kenji's cars in Grand Theft Auto) deleted the
owner's copy alone: a participant riding in it lost his seat on the owner's
screen and the session rebuilt the car there.

Not seen in a game yet: the hold, the move off a roof, and the removal
reaching the machine whose engine made the car.

### R4d. "The player at a place" is anybody there, the owner nearby (2026-10-01)

A playtest of Mike Lips Last Lunch: the guest drove Lips' car back to the
bistro, parked it and walked to the edge of the block, as the mission asks.
The last wait (JOEY1 at 3673) asks where *the player* stands, so it waited
for the owner, across the street, while the clock ran. noxx asked for one
rule for every mission: when the script asks the player to be somewhere,
stop somewhere, arrive in a car somewhere or get to an area or a zone, any
participant doing it counts, as long as the owner is nearby, in another car
or on foot.

**The rule** (`game/anyplace.h`, asked from `AnybodyAtPlace` in
`game/mission.cpp`, tested in `tools/clienttest/anyplace.cpp`). A location
condition of the owner's mission script about its player,
`IS_PLAYER_IN_AREA_2D/3D` (0056, 0057), `LOCATE_PLAYER_*` and
`LOCATE_STOPPED_PLAYER_*` (00E3-00E8, 00F5-00FA), `IS_PLAYER_*_IN_AREA_*` and
`IS_PLAYER_STOPPED_IN_AREA_*` (0197-01A0) and `IS_PLAYER_IN_ZONE` (0121), is
answered yes on the owner's machine when the engine said no and

- a participant is inside the same area the same way: on foot, in a car,
  stopped (his car's speed, or his own on foot, under 3.6 km/h) as the
  command asks (`standin.h`, `PlaceNeedsOf`, `PlayerAnswersPlace`). A zone
  is the engine's own box for that label (`CTheZones::ZoneArray`, addresses.h
  `zones`, checked against the exe);
- and the owner is within **60 m** of that area (the area grown by 60 m on
  every side, height left out), on foot or in any car. 60 m is the distance
  a summoned player is left to walk (`MISSION_MOVE_STAY_M`), a little more
  than a start's 5 m: across the street or round the corner the owner is
  with the group, a block away he is not and the step waits for him.

It is the general case of the path staD unified: `nearchar.h`'s places
anybody answers (Smack Down, Shima, Liberator, Bling-Bling Scramble: no owner
nearby needed, no checkpoint) and `standin.h`'s box anybody keeps
(Salvatore's garage) come first, and everything else falls to this rule. The
answer only goes from no to yes, through the flag the block would have had,
so `NOT`, `if and` and `if or` come out as with the owner there. Nothing new
goes on the wire: positions, seats and car speeds are what the owner's
machine already has.

**Checkpoints.** A place holding one of the mission's coordinate blips still
waits for everybody (C1), and a yes this rule gives goes through
`AskCheckpoint` like the owner's own: every *other* participant has to be
inside, 5 m of slack. What changed is the owner: he only has to be nearby,
which is what noxx asked. A timed or raced checkpoint waits for nobody, as
before, so there one participant with the owner nearby is the arrival. The
checkpoint wait has never applied under a `NOT` or in an `if or`
(`MayForceCondition`), and still does not.

**What stays the owner's own answer, and why:**

- *A scene.* While widescreen or a cutscene is up, while the script has
  taken the controls off the player (`SET_PLAYER_CONTROL`, the
  `PLAYERINFO` bit of `CPad::DisablePlayerControls`) and while the owner's
  ped is walked out of a car (R4b's exception, `standin.h` `WalkedOut`), a
  place is about where the script moves the owner's own ped.
- *The stored car.* Once the mission has run `STORE_CAR_PLAYER_IS_IN` and
  that car still stands, only a participant sitting in it answers, and an
  on-foot place stays the owner's: what the script does next at the place it
  does to that car (Misty walks to it at the hospital, the patients get out
  of the ambulance, Kanbu's bomb goes in the cop car, Ray's CIA chase it).
  This is how The Getaway's thugs and Kanbu's cop car were already left to
  the owner (R4b), applied to the places.
- *A car beside it in the block* (C1). An `if and` or `if or` that also asks
  `IS_PLAYER_IN_ANY_CAR`, `IS_PLAYER_IN_MODEL` or `IS_PLAYER_SITTING_IN_(ANY_)CAR`
  is the owner's one state, and one that asks `IS_PLAYER_IN_CAR` counts only
  when that car stands at the place (`InCarAtThePlace`'s 5 m), so a guest on
  foot at the laundry and the owner in Toni's car 50 m off is not "in Toni's
  car at the laundry", while a guest driving it there is. The conditions after
  the place have not run when it is asked, so the yes is settled at the
  block's `goto_if_false` (`PlanFor`, `SettleWrites`): written at once where
  only a car beside it could take it back (`if and`, or a `NOT` in an `if
  or`), and put back to the owner's no if one does; written at the
  `goto_if_false` otherwise. Each is a constant one condition decides on its
  own; clienttest runs every block of one to four conditions through a model
  of the engine's `UpdateCompareFlag` to show it comes out as the place's yes,
  or as the owner's no, and nothing else. The owner's horn, wanted level,
  phone and the script's own flags beside it still have to hold, and do not
  change who is at the place.
- *The sites below*, where the next steps act on the owner's ped or car at
  that place and nothing above sees it, or where a stealth rule already asks
  each participant (`anyplace.h`, `ExcludedWhy`).

**Every location condition in the retail main.scm** (the decompile, 471
conditions on `$PLAYER_CHAR` in 59 missions; none in the others). "Widened"
is this rule; "table" the earlier specific rules; "stored car" and "car
beside" the runtime exclusions above, counted where they hold when the
condition is reached (stored car: after the mission's first
`STORE_CAR_PLAYER_IS_IN`); a scene can still make any of them the owner's at
run time.

| # | Mission | Conditions | How each is answered |
|---|---|---|---|
| 7-10 | 4x4 runs, Multistorey Mayhem | 67 | stored car (the Patriot, the Stallion): a participant in the owner's car |
| 11 | Paramedic | 12 | stored car (the ambulance the patients ride in) |
| 14 | Taxi Driver | 28 zones | **owner's**: the fare's destination is picked by the owner's zone |
| 15 | The Crook | 1 | car beside (Marty's car at the crusher) |
| 16 | The Thieves | 1 | car beside (the respray block, standin.h's own rule first) |
| 19 | Give Me Liberty / Luigi's Girls | 6 | car beside 3 (8-Ball's help text); **owner's** 3: the hideout and Luigi's back door, whose scenes walk the owner's ped; the girls' leg is the stored car's |
| 20 | Don't Spank Ma Bitch Up | 5 | widened 2 (the look round the dealer's place, the garage message); car beside 3 |
| 21 | Drive Misty For Me | 4 | asked of whoever picks Misty up (R4e), not by the general rule |
| 22 | Pump-Action Pimp | 4 | widened (Ammu-Nation, the pimp's place) |
| 24 | Mike Lips Last Lunch | 4 | **widened**, the bistro ending included: the car parked, a guest at the edge and the owner within 60 m, Lips comes out |
| 25 | Farewell 'Chunky' Lee Chong | 3 | widened |
| 26 | Van Heist | 2 | widened |
| 27 | Cipriani's Chauffeur | 6 | widened 4 (the stop messages); car beside 2: the stops pass for Toni's car there with anybody in it |
| 28 | Dead Skunk In The Trunk | 1 | widened |
| 29 | The Getaway | 12 | asked of the robbers' driver (R4e), not by the general rule |
| 31 | The Pick-Up | 2 | widened 1; **owner's** 1, the walk into Toni's |
| 32 | Salvatore's Called A Meeting | 7 | widened 4; car beside 2 (the Stretch at Luigi's and Toni's); **owner's** 1, the start, which seats him in the Stretch |
| 33 | Triads And Tribulations | 5 | widened (zones and the fish factory) |
| 34 | Blow Fish | 2 | widened |
| 35 | Chaperone | 6 | widened 3; **owner's** 3, the end scene's 1 m steps to Salvatore's door |
| 36 | Cutting The Grass | 6 | widened 3 (the ramp spooks Curly for anybody, as R13); car beside 2; stored car 1 (the taxi) |
| 38 | Bomb Da Base: Act II | 2 | stored car |
| 39 | Last Requests | 4 | widened |
| 40 | Turismo | 3 | widened (the finish and the checkpoints) |
| 42 | Trial By Fire | 2 | widened |
| 44 | Sayonara Salvatore | 58 | widened 7; table 2 (the garage); the two checks after a spotting 24 (standin.h `SpottedStandIn`); stored car 25 (the car the convoy checks the player stopped in) |
| 47 | Payday For Ray | 5 | widened (lifting the phone is still the owner's) |
| 48 | Two-Faced Tanner | 1 | widened (a checkpoint: the others still come) |
| 49 | Kanbu Bust-Out | 2 | stored car (the cop car) |
| 51 | Deal Steal | 7 | **owner's**: the rendezvous is R13's per participant; the casino scene |
| 52 | Shima | 3 | table 1 (the Diablos); **owner's** 2, the store and casino scenes |
| 53 | Smack Down | 15 | table |
| 54 | Silence The Sneak | 1 | widened |
| 55 | Arms Shortage | 7 | widened 1; stored car 2; **owner's** 4, the warehouse and Phil, whose scenes run the owner's ped |
| 56 | Evidence Dash | 4 | the files (standin.h `IsEvidenceLocate`) 3; stored car 1 |
| 57 | Gone Fishing | 4 | widened 1; stored car 3 (the police boat) |
| 58 | Plaster Blaster | 5 | widened 2; **owner's** 3, the decoy is R13's per participant |
| 59 | Marked Man | 39 | widened 2; stored car 37 (the car the CIA chase) |
| 60 | Liberator | 7 | table 6; widened 1 |
| 61 | Waka-Gashira Wipeout! | 23 | **owner's**: the car park is the stealth rule's per participant |
| 62 | A Drop In The Ocean | 2 | **owner's**: the walk into Love's |
| 63 | Bling-Bling Scramble | 3 | table 1; **owner's** 2, the start, which stores and locks his car |
| 64 | Uzi Rider | 3 | widened (Hepburn Heights, a zone) |
| 65 | Gangcar Round-Up | 1 | stored car (the gang car, standin.h `RoundUpSubject`) |
| 67 | Grand Theft Aero | 15 | widened 13 (its goons' stores are no stored car, `StoreIsOnlyATarget`); **owner's** 2, the walk into Love's |
| 68 | Escort Service | 22 | widened 20; a box anybody keeps 2 (Love's stash garage) |
| 69 | Decoy | 2 | widened 1 (the approach); the end is the decoy van's driver's (`DecoyRider`) |
| 71 | Bait | 10 | table 4 (the cartel markers); widened 6 (the zones that make the Yakuza and the cartel, the killzone) |
| 72 | Espresso-2-Go! | 8 | widened (the stalls made by zone) |
| 73 | S.A.M. | 16 | the cargo 8 (standin.h `IsCargoLocate`); table 3 (the docks); widened 3 (the buoy, the stash); **owner's** 2, the island it loads at 160 m |
| 74 | Uzi Money | 2 | widened 1; car beside 1 |
| 77 | Bullion Run | 3 | stored car 2; car beside 1 |
| 78 | Rumble | 1 | table (the fighting ground) |
| 79 | The Exchange | 7 | table 2 (the helipad); widened 5 |

LOCATE_PLAYER_*_CHAR, _CAR and _OBJECT (near a pedestrian, a car or an
object) are not places: reaching a character stays C1's, with the specific
rules of `nearchar.h` and `standin.h` where a mission needed them.

**Not seen in a game yet:** Mike Lips' ending with a guest at the edge and
the owner across the street; a zone answered from the engine's array on a
participant (Uzi Rider's Hepburn Heights); a checkpoint passing with the
owner 40 m off and every guest inside; a block settled at its
`goto_if_false` (Cipriani's laundry with a guest on foot at the door and the
owner in Toni's car outside the 5 m: it must keep waiting).

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

**Corrected** (2026-10-01): the widening only took a single condition, and
retail main.scm asks `IS_CAR_IN_MISSION_GARAGE` (021C) at nine sites, three
of them Grand Theft Auto's, each in an `if and` beside the mission's own flag
for "the car stood in the lock-up undamaged". A participant's delivery deleted
the owner's copy with that block still saying no, and the `IS_CAR_DEAD` right
after it failed the mission with "The vehicle is wrecked!". It is widened
there now through the block's own arithmetic (`CompareFlagIfTrue`): whether
the car is in the garage is about the car, not about who brought it. No other
site is in a block.

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
| The crusher crane | Dead Skunk In The Trunk, The Crook | The machine holding the car works the crane and settles the car while the crane has it; every other machine's crane follows it, and the crusher's result reaches the owner's script (protocol.md 1.62). |

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

**Built** (2026-09-30): the crusher crane, and every crane with it. The
machine holding the car works the crane: its crane asks for the car's custody
as it goes for it and keeps it until the drop has settled, so the pin no
longer puts the car back under the hook. Every other machine's crane follows
that one's hook, state and car (`C_CraneState`), so the owner's
`IS_CAR_PICKED_UP_BY_CRANE` says yes whoever delivered the car, and a car
crushed on another machine names the owner's handle in `CGarages::CrushedCarId`
before it goes, for `IS_CAR_CRUSHED`. Not run in-game (§5).

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

**Corrected** (2026-10-01): not quite. Bomb Da Base: Act II asks
`IS_PLAYER_SHOOTING_IN_AREA` in an `if or` beside "a guard down", and a
participant's shot there was not heard. It is answered for anybody alone and
in an `if or` (a yes for anybody is the block's yes), never in an `if and`.
Cutting The Grass's "Curly got away!" is an `if and` of Curly off the owner's
screen and nobody within 160 m: the nearest player's answer now goes through
the block's flag there too (§3, Toni and Salvatore step by step).

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
A fourth the table missed, Waka-Gashira Wipeout!'s car park (on foot there,
or upstairs out of a Colombian car), is built the same way (2026-10-01, §3,
"Donald Love and King Courtney in Staunton").

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

#### Side jobs, step by step (2026-10-01)

Read off 03_rc1 to 14_taxi, triggers.sc, rewards.sc and usj.sc, and checked
against the retail main.scm where it matters. What every side job shares
first, then each one.

**The rules they all follow, as built before this pass.** Anybody starts one
(not only the host, missions.md 5.8), from the driver's seat only
(`game/sidejob.h`), and it is held at `START_MISSION` until every player is
within 5 m of the starter (the RC runs: of the van's spot). Whoever is
there is a participant: sees the text, blips, timer and counter (R16), and
is paid every `ADD_SCORE` in full under `money = own`, once under
`money = shared`, not at all under `missionPayHelpers = false` (missions.md
12.1). A participant's death fails the shift with `missionFailOnDeath` on,
as for any mission; Taxi Driver, which turns its own death check off, is
unwound to its cleanup the same way. The rewards that are engine state
(the Paramedic's sprint, `SET_PLAYER_NEVER_GETS_TIRED`; the Vigilante's
"get out of jail free"; the Borgnine taxis' car generator) are `World` on
the replay list: run by every participant live and by everybody from the
delta. **Decision: a side job's unlock is everybody's, its progress is each
save's own and never goes back.**

**Found and fixed in this pass:**

- *A guest's shift took the host's progress back.* The delta writes what the
  owner's save holds, and a side job's owner is any save. Three fares from a
  guest who had none put the host's `$TAXI_MISSION_DELIVERIES` from 90 back
  to 3; a guest's first bribe put the host's `$PLAY_PAGER_MESSAGE1` from 2
  back to 1, and the host's next Vigilante shift then paged the second bribe
  and paid its progress point again; a guest's slower Patriot Playground
  overwrote the host's best time. Now (`client/src/sideprogress.h`,
  `MissionSync::SettleCampaign`) a side job's counts, records, help flags and
  reward flags (32 globals, the table in the header) are written only when
  they move this game on: a count or flag only up, a best time only down.
  Whoever is furthest along leads and the others follow; everything else in
  the delta goes as before. Only under the retail main.scm, whose hash the
  table is kept for; `clienttest` finds every entry in it.
- *The same progress point paid twice.* A side job's `PLAYER_MADE_PROGRESS`
  and `REGISTER_MISSION_PASSED` travel in the delta and were counted on every
  machine whatever it already had: a host who had the flamethrower got the
  point again when a guest earned it, and an RC run's first pass, whose flag
  only its own mission reads (no latch, missions.md 5.8), counted twice in
  the missions-passed stat. Every progress point a side job pays is set
  beside a reward flag (`clienttest` checks the 20 in main.scm: 18 are, the
  Paramedic's last level is not), so a machine counts them only when one of
  those flags goes up there. The known issue "the RC runs' first pass" is
  closed by it.
- *Taxi Driver dead after a held start.* The trigger runs `0004
  $ON_TAXI_MISSION = 1` straight after its `START_MISSION`, and only the
  shift's cleanup sets it back to 0. A start held for a teammate more than
  50 m away and given up (the driver got out, or somebody else's mission
  started) went on past `START_MISSION` into that line, and the taxi trigger,
  which offers a shift only while the flag is 0, never offered one again
  until a load. A given-up odd job now skips that line with it
  (`GivenUpSequelLength`); the other three odd jobs' line is a help flag
  they had set already. All eight starts are checked against main.scm.
- *The safehouse racks were one rack between two players.* rewards.sc lays
  out its rack (bat, package weapons, flamethrower, bribes, health,
  adrenaline) on every machine whose player walks into the safehouse, from
  that save's flags, and the pickup sync made the two racks one: the first
  to take the flamethrower took it off the other's for twelve minutes. The 60
  rack spots are now each player's own, like the info pickups
  (`game/pickup.h`, `OnHideoutRack`; every spot is found in main.scm).

**Each job.**

| Job | Who gets paid | On both screens | Helpers | Unlock, and whose | Left |
|---|---|---|---|---|---|
| Taxi Driver | each participant every fare, speed bonus and in-a-row bonus | "FARES" counter, the timer, every print | no fares of their own: the fare boards `$TAXI_CAR1`, the owner's taxi. A guest can drive it with the owner riding (fare locates are the owner's, `IS_PLAYER_IN_CAR` is anybody's, R4b); a fare that finds the back seats full asks the last rider out (R4) | 100 fares: the Borgnine generator and its point, from the delta, for every save whose `$NEW_TAXI_CREATED_BEFORE` goes up | the owner out of the taxi with a guest still in it keeps the shift going, and no fare boards until he is back; a save pushed past 100 by somebody else's count without that delta never sees its own `== 100` |
| Paramedic | each participant each level's reward | level text, timer, "+N seconds", counts | patients board the owner's ambulance only; helpers escort. "Ambulance full" counts patients, not riders (R4) | 35 / 70 saved: health and adrenaline at the racks, from each save's `$TOTAL_SAVED_PEDS`; level 12: never tired, everybody | the last level's progress point has no flag beside it, so a save that already finished the Paramedic gets it again from somebody else's last level |
| Firefighter | each participant each fire | "FIRES" counter, timer, the "reported in" line | a helper's own fire truck puts the fire out (the jet reaches the owner's copy, `protocol.md` 1.37) and it counts | 20 fires on each island: the flamethrower at the racks, `$EARNED_FREE_FLAMETHROWER` | the burning car stays alight on a helper's screen until the owner's `REMOVE_ALL_SCRIPT_FIRES` |
| Vigilante | each participant each criminal and the bonus | "KILLS" counter, timer, the return-to-a-police-car countdown | a helper's kill of the criminal counts (R1, he is `ONLY_DAMAGED_BY_PLAYER`); he goes for the nearest player | 10 and 20 on each island: bribes at the racks; the bonus at 10 kills: out of jail free, everybody | the police radio's "suspect last seen" is the owner's ear alone |
| RC (Diablo, Mafia, Casino, Rumpo) | each participant the record money | timer, "KILLS" | a helper's wreck of a target counts (R11) | first pass: its point once per save; records only go up | the buggy is the owner's (R7); helpers are frozen with him for the 4 s intro |
| Patriot Playground, A Ride In The Park, Gripped!, Multistorey Mayhem | each participant the pass money | timer, "N of 15" | only the owner's car takes a checkpoint, and the checkpoints don't wait | first pass is a latch (missions.md 5.8); best times only go down | the owner's car is locked for the intro, a rider in it with it |
| Import/Export, the crane | the engine pays whoever delivers | the boards are the session's (missions.md 6.1) | anybody's car counts once | the reward cars, every machine's own import.sc | nothing new |
| Hidden packages | the finder's machine pays the $1000 | the count | shared or per player (pickups.md 6) | the racks' weapons, each save's own count, each player's own rack (above) | nothing new |
| Unique jumps, insane stunts | the driver's machine alone (`game/stunt.h`) | riders see the slow-motion shot (`game/ridecam.h`) | a rider gets neither the $5000 nor the jump in his save | per save | a rider has to do the jump again himself |
| Rampages | shared, scaled or off, voted (docs/rampage.md) | | | | nothing new |

**For a game to show:** a guest's Vigilante bribe with the host further on
(log: "a side job's delta left N of this game's own counts..."); a taxi start
held for a far teammate, given up, and offered again on the next key press;
two players at the Portland safehouse each taking the flamethrower.

### Marty Chonks (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 15 | The Crook | bank manager into your car, to the crusher | R4, R7 (crusher), R3 (factory door) | **reqs** owner drives and delivers the car to the crusher; the crane is built for anybody's car, not yet run (R7) |
| 16 | The Thieves | thieves to the dog food factory, respray, back | R4, R5 (respray), R3 | **reqs** owner drives |
| 17 | The Wife | Mrs Chonks, dump the car in the sea | R4, R3 | **reqs** |
| 18 | Her Lover | pick up the lover | R4 | **reqs** |

### Luigi

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 19 | Give Me Liberty & Luigi's Girls | escape with 8-Ball, reach the hideout, take Misty to Luigi's | C5, C4, R4 (8-Ball, Misty), R10 (bridge wreck objects, swaps), R12 (critical restart point), clock | **reqs** starts by itself at a New Game, arbitrated like any contact |
| 20 | Don't Spank Ma Bitch Up | bat pickup, beat the dealer, respray his car, stash it in Luigi's lock-up | R2, R5 (respray and lock-up) | **reqs** owner drives the car to the garages |
| 21 | Drive Misty For Me | horn outside the hospital, Misty to Joey's | R4, R14 | **built** whoever stops outside her flat picks her up; the checks follow that car (R4e) |
| 22 | Pump-Action Pimp | free Colt pickup, kill the pimp | R1 (pimp's car), R2 | **reqs** |
| 23 | The Fuzz Ball | round up four girls into your car in time | R4 (24 group sites) | **reqs** the girls follow the owner, so the owner does the driving |

#### Marty and Luigi, read again step by step (2026-10-01)

Read off the retail main.scm (the `.sc` files differ: Give Me Liberty's
hideout arrival there is `IS_PLAYER_IN_CAR`, in the retail script it is
`IS_PLAYER_SITTING_IN_CAR`). What the base bridge and R1-R16 already carry is
not repeated; this is what was still left, what was done about it
(`game/standin.h`, tested in `tools/clienttest/standin.cpp`) and what a game
has to show.

**Three rules added, each only ever widening an answer to yes:**

- *Escort* (Marty's four only). `LOCATE_PLAYER_*_CHAR` about a mission
  pedestrian is yes for a participant sitting in one of the mission's cars, or
  in the car the pedestrian rides in, inside the box. Two checks need it:
  the pick-up, `if or` (NOT player in a car within 8 m of him, NOT in Marty's
  car), which only the owner could pass, and "You have left the Bank Manager
  behind!" (and the thieves, the wife, Carlos) at 30 m, which **failed the
  mission** whenever a guest drove the passenger and the owner followed more
  than 30 m back. Not for Luigi's Girls and Drive Misty For Me: Misty is sent
  to the car the owner stored, so a guest beside her must not answer for him.
- *A respray in a block.* `HAS_RESPRAY_HAPPENED` is asked in exactly two
  places, The Thieves and Don't Spank Ma Bitch Up, both
  `if or (NOT respray, NOT in the car, NOT stopped in the Pay'n'Spray)`.
  R5's widening only reached a single condition, so **a guest's respray never
  counted**. It now answers there, and is taken only when the block went the
  way that yes pushed it (read at the block's `goto_if_false`): a frame where
  the owner's other conditions still say no keeps the respray for the next
  ask instead of losing it, as asking it would in single player.
- *The car at the place.* A location of the owner asked in the same block
  after `IS_PLAYER_IN_CAR` was answered for a participant in the car is yes
  when that car stands in the area (5 m of slack, stopped where the check
  asks for stopped). It is the mirror of `InCarAtThePlace`, for the order
  Marty's and Luigi's respray blocks use ("in the car, then in the shop").

**And one hint:** Give Me Liberty's hideout arrival waits for the owner
*sitting in* the Kuruma (`IS_PLAYER_SITTING_IN_CAR`, not widened: the scene
after it walks the owner's own ped out of the Kuruma and into the hideout, and
with the owner in another car the 10 s fallback's `SET_PLAYER_COORDINATES`
would put that car into the hideout). With a guest driving the Kuruma there,
the owner had nothing on his screen; he is now told "Hey! Get back in the
vehicle!" every 6 s while the Kuruma stands there with a participant in it.

| # | Mission | Left over, and what was done | Still for a game to show |
|---|---|---|---|
| 15 | The Crook | pick-up and left-behind owner-only: **fixed** (escort). Crusher, door, clear and pay are built. | a guest driving the bank manager to the factory with the owner 50 m behind; the guest delivering the Perennial to the crusher (R7) |
| 16 | The Thieves | as The Crook, both thieves: **fixed**. Guest's respray never counted, and the owner had to stop in the shop: **fixed** (held respray, car at the place). "Get out of the vehicle" at the end waits for every participant to get out, as it should. | the guest's respray with the owner elsewhere; the owner riding as passenger (both machines respray; the owner's own answer is enough) |
| 17 | The Wife | pick-up and left-behind: **fixed**. The Esperanto has one passenger seat: an owner riding with a guest driver takes Mrs Chonks' seat, and R4 asks only remote riders out, so the owner has to get out himself. `IS_CAR_IN_WATER` reads the owner's copy. | the guest dumping the car in the sea: that the owner's copy goes `bIsInWater` (its physics runs, so it should) |
| 18 | Her Lover | as The Wife (the Stallion has one passenger seat too): **fixed**. Carlos killing Marty is pedestrian against pedestrian, both the owner's. | Carlos boarding with the guest driving |
| 19 | Give Me Liberty / Luigi's Girls | hideout arrival needs the owner in the Kuruma, silently: **hint added**. Luigi's arrival already passes with the owner stopped there in his own car (widened `IS_PLAYER_IN_CAR` in an `if or`); the scene then walks or, after 5 s, moves the owner's ped. Misty's leg is the owner's by design (his stored car, he is her leader). | the owner arriving at Luigi's in his own car (the 5 s fallback, his car moved to the back door); the owner getting into the Kuruma at the hideout after the hint |
| 20 | Don't Spank Ma Bitch Up | the respray: **fixed** as The Thieves. The dealer turns on the player only when the owner comes within 10 m; anybody can kill him anyway. Lock-up delivery is R5 (`IS_CAR_IN_MISSION_GARAGE` widened); the owner's `DOES_GARAGE_CONTAIN_CAR` reads his copy's position, so the "get back in" only flashes once while the door shuts. | the guest's respray and lock-up delivery; whether the owner's own lock-up door lets the guest's car through on the owner's screen |
| 21 | Drive Misty For Me | nothing new: the owner's car, his horn, his stop at the hospital; Misty's seat is R4's corrected order. | the horn with the owner riding and a guest honking (R14) |
| 22 | Pump-Action Pimp | nothing new: the Diablo car is `ONLY_DAMAGED_BY_PLAYER` (R1), the Colt is everybody's (R2), the two get out below 999 health whoever hit the car, the kill orders go for the nearest player. Ammu-Nation's dialogue and the targets' help are the owner's alone (cosmetic). The sunk car's move to the road asks the owner's screen. | a guest's rams opening the car (R1, collisions as Van Heist) |
| 23 | The Fuzz Ball | nothing new: the redesign (`game/fuzzball.h`) answers each pick-up for the participant who stops by the girl; a girl who strays is picked up again the same way (00FD), or re-tied to the owner by the script's own 00E9. | four girls split between two cars before the clock runs out |

### Joey

Read step by step on 2026-10-01 against what is built. Each row says what
co-op still leaves to the owner; the notes under the table list every hazard
found, and whether it was fixed or left.

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 24 | Mike Lips Last Lunch | Lips' car to 8-Ball's bomb shop and back, lose the cops, arm it | R6, R12 (wanted), R4b | **ok** anybody drives the car, sprays it and arms it; the owner watches from the edge of the bistro |
| 25 | Farewell 'Chunky' Lee Chong | Colt pickup, kill Chunky | R1, R2, the player nearest Chunky | **ok** anybody can chase him; he is lost only once everybody is 160 m off |
| 26 | Van Heist | ram the security van, take it to the lock-up | R1 (rams, 1.57), R5, R10 | **ok** anybody delivers it; the lock-up is heard before the van goes |
| 27 | Cipriani's Chauffeur | drive Toni's mother; Ammu-Nation starts selling the Uzi | R4, R4c, R12 (warp into car), R3, R2 and R10 | **ok** the owner stops at each place, anybody in Toni's car |
| 28 | Dead Skunk In The Trunk | shake the Forellis, the car to the crusher | R1, R7 (crusher) | **ok** anybody delivers the car; the crane is not run in a game yet (R7) |
| 29 | The Getaway | three thugs into a four-seat car, horn, bank doors, lose the cops | R4 (every seat), R14, R3, R9, R12 | **built** everybody comes in a car of his own; the checks follow the car the robbers board (R4e) |

**Mike Lips Last Lunch (24).** Checked: the car is anybody's (R4b), the bomb
shop's fitting and the arming travel (R6), a helper's Pay'n'Spray repairs the
owner's copy (`C_Respray` runs `Fix`), the wanted level asked is the owner's.
Fixed: `SET_CAN_RESPRAY_CAR` (0294) reaches every copy, so a helper's
Pay'n'Spray no longer repaints Lips' car (protocol.md 1.65). Fixed: the last
wait (the `if or` at 3673) asked the *owner* to stand out of the 44 x 38 m box
round the bistro and inside the 64 x 50 m one, so a helper who parked the car
with the owner somewhere else waited on the owner while the clock ran; it is
now anybody at the edge with the owner within 60 m (R4d).
"Activate the car bomb then get out of there!" still reaches everybody. Live
test: a helper drives, sprays and arms; the owner walks to the edge; Lips
gets in and the car goes up on every screen (which machine blows it depends
on who holds the car once Lips is at the wheel).

**Farewell 'Chunky' Lee Chong (25).** Fixed: every locate against Chunky
(`not 00FB` 25 m and 20 m before the fight, five `not 00E9` 160 m, "He's
clean out of here!") is answered for the participant nearest him
(`nearchar.h`), so a helper on his heels starts the scene and keeps him in
reach while the owner is a block behind. Fixed: the Colt's sprite blip (03DD)
is on everybody's radar. Left: Ammu-Nation's lines and the gun tutorial play
for the owner walking in; the tutorial's help text reaches everybody. Live
test: a helper alone near Chunky starts the fight; the owner 200 m off does
not fail it while the helper chases.

**Van Heist (26).** Fixed: a helper's delivery to the lock-up. The helper's
garage destroys the van and sends `VEHICLE_REMOVED_MISSION`, the despawn
deletes the owner's copy, and the script asks `IS_CAR_DEAD` right after the
garage question, while the helper's `C_MissionAnswers` come only every eight
frames: the van read as wrecked first. The owner's machine now counts the
garage as holding the car the moment the removal names a car its
`GARAGE_MISSION` garage waits on (`MissionSync::CarDeliveredElsewhere`,
protocol.md 1.65). Left: the wanted level is cleared at the lock-up's door
only for the owner in a car there (`00F7`); everybody's is cleared at the
pass. Live test: the helper rams, drives the van in and walks out; the door
shuts and the mission passes, not "The vehicle is wrecked!".

**Cipriani's Chauffeur (27).** Checked, nothing new: the opening clear (R4c),
the warp into the car with the participants in its back seats (R12, 1.40),
Toni getting out and back in (R4 keeps his seat), the laundry and Mamma's
stops (`01A0`, the owner stopped there; `IS_PLAYER_IN_CAR`, anybody; the
owner's wanted level), the Uzi unlock (R2, R10). Live test: a helper at the
wheel with the owner riding; both stops pass.

**Dead Skunk In The Trunk (28).** Checked, nothing new: the Forellis' rams go
for the nearest player (RetargetRam), each Forelli's $5,000 is everybody's,
the crane and the crushed car are heard from the machine that holds it (R7,
`NoteCarCrushedElsewhere`).

**The Getaway (29).** The start at Joey's (`019B` stopped, the horn,
`IS_PLAYER_IN_ANY_CAR` then `STORE_CAR_PLAYER_IS_IN`) and everything after it
follows the robbers' driver, not the owner: R4e. The thugs' seats are kept
(R4); the bank doors are main-script objects moved on every machine (R3); the
alarm and the minimum wanted level reach everybody; the 17:00 limit is the
owner's clock, which is the session's.

**R4e. The robbers ride with whoever drives them** (`game/getaway.h`, the
Getaway block in `game/mission.cpp`; a playtest of 2026-10-01). Two players and
three robbers do not fit in one car, so the players arrive in different ones.
The script asks everything of its player (the owner's ped): the pick-up
(`019B` stopped in a car in Joey's box, `0122` the horn, `00DA` the car stored
and `01EA` its seats, `00DE` no bus), the robbers' `01DF`, `0320` and the 30 m
`00E9` that ties one back and puts his marker away, the stop at the bank
(`019E`/`01A0`), `00E0` every frame to store the car again, `0199` back at
Joey's, and `00DB` for each robber in the car stored. Left alone, a guest in
a car of his own was asked about as if he still had to be in the owner's, and
both players saw the markers above the robbers and the prompts about the car.

- **Who.** Before the robbers are in a car: the owner when he drives a car
  with three free seats at the pick-up, else the first participant who
  arrived there driving one. Once the robbers exist the choice stays while it
  still holds. With the robbers in a car: whoever drives it, the owner first;
  with nobody of the group in it, nobody (the owner in another car is not the
  player in the robbers' car). The owner as the driver is the script as it
  was, and so is the game alone.
- **A guest as the driver** answers for the player in `00E0`, `00DA` (the car
  stored is his, our copy of it), `00DE`, the horn (his car's, as the wire
  carries it), the 30 m locate against a robber, and every location check
  (`019B` and the others: where he is, in a car, stopped). `01DF` makes his
  copy the robbers' leader, so the engine's own follower logic walks them to
  his car; `0320` reads his copy as the player whose group they are in; and a
  follower kept out of his full car with a player riding in it is asked about
  the way the owner's is (R4).
- **Never required elsewhere.** The place checks are no checkpoint for
  everybody to reach: the other player, in a car of his own or on foot, holds
  nothing up, and a mission that ends with the robbers' car at Joey's passes
  with him anywhere.
- **What is shown.** The marker `0187` puts above a robber who fell behind,
  and the prompts about the car (`HORN`, `JM6_5`, `NODOORS`, `JM6_6`,
  `EBAL_5`, `HEY2`), are the driver's alone: kept off the wire when it is the
  owner, sent to the guest alone and put out on the owner's screen when it is
  him. The plot, the cops and "You need all 3" are everybody's. No wire
  change: `MissionEffectBody::onlyTo` as the get-back-in prompts use it.

**Drive Misty For Me (21)** follows the same rule with one passenger (`RIDES` in `getaway.h`): whoever stops outside Misty's flat with a free seat and sounds the horn picks her up, and the horn, `0443`, the 8 m locate that ties her back, "HEY4" with its marker and "IN_VEH2" are his.

Left: a rider in the guest's car beside him is asked out for the third robber
(R4) like the owner in his own; the two players in one car are not given the
choice of a second.

### Toni

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 30 | Taking Out The Laundry | destroy the laundry vans | R1 (vans), R2 (grenades and their marker) | **built** anybody wrecks a van, everybody takes grenades and sees their marker (below) |
| 31 | The Pick-Up | briefcase, ambush, cash back to Toni's | R2 (briefcase and its marker) | **built** anybody takes the briefcase; the walk into Toni's is the owner's (below) |
| 32 | Salvatore's Called A Meeting | Joey, Luigi and Toni in the limo, horn, repair, Salvatore's garage | R4 (every seat, the owner's own included), R14, R5, R3 | **built** anybody may drive the Stretch; a rider gets out for Toni, the owner too (below) |
| 33 | Triads And Tribulations | shotgun, Triad fish van, kill the Triads | R1, R12 (shotgun), R15 (zone gang info), the warlords' guards | **built** a guest near a warlord brings his guards out (below) |
| 34 | Blow Fish | the bomb truck into the fish factory | R6, R8 (explosions, fire), R10 (factory swap), R9 (the truck blown up), clock | **built** anybody may drive the truck; the mission blowing it up reaches whoever has it (below) |

### Salvatore

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 35 | Chaperone | drive Maria in the Stretch, the raid, back to Salvatore's | R4 (Maria), R12 (warp into the Stretch), R5, R9 | **note** Maria follows the owner after the raid; the waits at the club and the end are the owner's (below) |
| 36 | Cutting The Grass | Curly Bob in your taxi, then tail him without spooking him | R4 (Curly), R1, R13 (the Spookometer, "got away"), R16 (its bar) | **built** everybody keeps their distance; whoever is nearest Curly answers for the group (below) |
| 37 | Bomb Da Base: Act I | the briefing | nothing | **ok** |
| 38 | Bomb Da Base: Act II | $100,000, 8-Ball, snipe the guards, the ship blows | R12 (sniper rifle for everybody), R13 (shooting in the area, in an `if or`), R8, R10 (ship swap) | **built**. The money check is the owner's |
| 39 | Last Requests | the rigged car, the boat with Maria and Asuka; **the Staunton unlock** | R6, R12 (warp into the boat), R1 (Maria, Asuka), R10 (bridge, roads, tunnel, the barriers taken away) | **note** the owner drives the boat; Maria and Asuka stand on its deck (below) |

### Toni and Salvatore in Portland, step by step (2026-10-01)

Each script read through (30_toni1.sc to 34_toni5.sc, 35_frank1.sc to
39_frank4.sc) for what only the owner's machine had, what only asked the
owner, and where a guest doing the step could stall it. What was found and
built, and what a game still has to show:

**Taking Out The Laundry.** The vans are R1's, `IS_PLAYER_IN_CAR` on a van
(it lifts the van's "only the player" flag) is R4b's. Found: the grenades'
radar marker at 8-Ball's, `ADD_SPRITE_BLIP_FOR_PICKUP`, was the owner's
alone. *Built:* `ADD_BLIP_FOR_PICKUP` and `ADD_SPRITE_BLIP_FOR_PICKUP` are on
the replay list (replay.h), each machine's marker on its own copy of the
pickup, run only on a pickup still up there with its object, the sprite
checked against the radar's 21. The help text once the grenades are taken
is the owner's taking, shown to everybody. *Game:* the marker on a guest's
radar, gone from it once he has taken his.

**The Pick-Up.** The briefcase is one pickup, anybody's (R2); its marker
now reaches everybody, as above. The ambush's Triads go for the nearest
player (§5.3). *Left:* the end, "Get the cash back to Toni's", is a locate
of the owner on foot at Toni's door 7 m from its checkpoint, so not a
checkpoint, and the walk inside is the owner's own ped: a guest who took the
briefcase waits for the owner there. Nothing stalls; the story's player
walks in.

**Salvatore's Called A Meeting.** Joey, Luigi and Toni fill every passenger
seat of the Stretch. Found: with a guest at the wheel and the owner riding,
the owner's seat was nobody's to give up (only a player's *copy* counted as
a rider), so Toni's order was dropped for a full car and the mission waited
at the restaurant for ever. *Built:* on the owner's machine its own player
in a passenger seat is a rider like a participant (mission.h, RiderInSeat,
OwnerGivesUpSeat): a dropped order is noticed and given again, and when the
seats are short the owner is let out beside the car the way the seat key
lets a participant out. Luigi's and Toni's stops (`player stopped in the
area`, horn, wanted level) are the owner's, everybody inside the area as a
checkpoint; the wanted level asked is the owner's. The repair is the
limo's health, which its driver's stream carries; Salvatore's garage is R5;
the final `DELETE_CAR` is R4c. *Game:* guest driving, owner riding, Toni
gets in and the owner stands beside the car.

**Triads And Tribulations.** The shotgun is R12's, the warlords R1's, the
fish factory gate main.scm's (gates.h: open while anybody's own thread asks,
in a Triad van). Found: each warlord's two guards are made when *the owner*
comes within 80 m, the warlord only runs at 30 m, and the Mafia escort is
deleted past 120 m of the owner, so a guest who went to the fish factory
alone met a lone warlord who never moved. *Built:* in this mission, as in
Cutting The Grass, `LOCATE_PLAYER_ANY_MEANS_CHAR_2D/3D` is answered for the
player nearest the pedestrian, in any block (nearchar.h, one table with
Chunky, I Scream and Big'N'Veiny). *Left:* "You need a Triad fish van to enter" and
the warlords' audio go by where the owner is; a message, not a step.
*Game:* a guest alone at a warlord brings his guards out.

**Blow Fish.** The truck's timed bomb is R6's, its DAMAGE bar and the timer
R16's, the factory's explosions, fires, debris and the swap R8/R3/R10's.
Found: the mission blows the truck up itself (`EXPLODE_CAR`) when it is hurt
past 900 or the time runs out, and the owner's own `BlowUpCar` refuses a car
somebody else drives or settles, so with a guest at the wheel the truck never
went up. *Built:* `EXPLODE_CAR` goes to the machine simulating the car,
alone (replay.h, carauthority.h `WhereTheWreckGoes`); that machine's
blow-up reaches everybody as any wreck does. A car nobody holds goes up on
the owner's machine and is not sent. "The car bomb's not set!" only shows
with nobody in the truck (R4b). *Game:* the guest's truck going up when the
timer runs out, and the factory going up with the guest parking it.

**Chaperone.** The owner is put in the Stretch (R12), Maria's seat is R4's,
"Get back into the Stretch!" R4b's, the club's lights `missions.md` §15's.
Maria's own stops (`char stopped near point in car`) do not care who drives.
*Left, the owner's by design:* waiting by the club (the owner within 22 m
for 10 s; it is a checkpoint, so everybody comes), the Chico talk skip and
the ending's skip (the owner's pad), and Maria after the raid, who follows
the owner (SET_PLAYER_AS_LEADER) into whatever car he takes; the Stretch to
Salvatore's garage is anybody's (R5). Nothing stalls.

**Cutting The Grass.** Found: "Curly got away!" in the hunt is an `if and`
of Curly off the owner's screen and nobody within 160 m, and the
Spookometer's widening only ever looked at single conditions, so a guest on
Curly's heels did not stop the owner losing him. *Built:* the nearest
player's answer goes through the block's flag (CompareFlagIfTrue), in any
block. *Left:* the ramp where standing spooks Curly is the owner's area;
the taxi Curly takes is the one the owner is in (`STORE_CAR_PLAYER_IS_IN`).
*Game:* the guest tailing Curly at 100 m, the owner 300 m behind.

**Bomb Da Base: Act I.** Nothing; the contact it makes is a campaign marker.

**Bomb Da Base: Act II.** The rifle for everybody (R12), the barrels (R3),
8-Ball tied to the owner, the drop-off a checkpoint. Found: 8-Ball goes in
on "a guard down *or* the player shooting at the docks", an `if or`, and the
shooting check was only widened alone, so a guest's first miss was not
heard. *Built:* `IS_PLAYER_SHOOTING_IN_AREA` is anybody's alone or in an
`if or` (mission.h, QuietCheckMayWiden), never inside an `if and`.
*Left:* the $100,000 is the owner's (§12.1); the camera skip is the owner's
pad. *Game:* a guest's shot from the vantage point sends 8-Ball in.

**Last Requests.** The bomb car (ignition, R6) blows up whoever starts it,
the pier stop is the owner's, the boat warp R12's, the moves to Staunton
load the island first (§15). Found: the pass takes Portland's subway gate
and tunnel block away (`DELETE_OBJECT` on init.sc's globals), and a game that
caught up from the campaign log kept both. *Built:* a `DELETE_OBJECT` whose
global the mission never wrote goes in its delta as well (replay.h,
DeletesWorldObject), run on catching up only on a live object of that game's
own; Love's third mission's Staunton barriers go the same way. *Left:* the
Staunton barriers the pass makes and its Ammu-Nation and Pay'n'Spray icons
are live only, not in the delta (the host's places are drawn for guests,
§1.57); Maria and Asuka stand on the Reefer's deck, so a guest taking the
wheel from the owner drives them on a copy whose deck the owner's physics
only sees by the stream, and they may fall in. The owner drives the boat.
*Game:* the bridge, roads and tunnel open on a guest who joins after the
pass.

### El Burro (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 40 | Turismo | race three cars, finish first | R1 (the racers), R15 (roads) | **ok** the owner has to win; helpers ram the racers, whose blips everybody sees |
| 41 | I Scream, You Scream | briefcase (detonator), ice cream van, jingle, detonate | R2, R6, the player nearest the gang | **ok** the owner gets into a van; anybody parks it, plays the jingle and presses the detonator |
| 42 | Trial By Fire | flamethrower, 25 Triads in a frenzy | R2, R13 | **ok** replaying `START_KILL_FRENZY` hands it to `rampage.md`'s machinery |
| 43 | Big'N'Veiny | follow the van and pick up the magazines | R7 (power pills), the player nearest the thief | **note** the magazines are collected only with the owner in the van, driving or riding |

**Turismo (40).** Fixed: the three racers' blips (`ADD_BLIP_FOR_CAR_OLD`,
0161) were on the owner's radar alone. Checked: a race's checkpoints are the
owner's and wait for nobody (`CheckpointsWait`); the countdown freezes
everybody; RACE TIME and the place reach everybody.

**I Scream, You Scream (41).** Fixed: `GIVE_PLAYER_DETONATOR` (037F) was
never replayed, so the detonator anybody may press (R6) was in the owner's
hand alone; everybody gets it now, and the cleanup's `SET_PLAYER_AMMO` 12 to
0 takes it back. Fixed: the briefcase's marker (03DC). Fixed: the eight 8 m
locates that turn a gang member on "the player" are answered for the nearest
participant (`nearchar.h`). Checked: the jingle is the van's siren byte,
which every copy takes from its driver; the parking `if or` takes anybody in
the van. Left: "Find an icecream van" (`IS_PLAYER_IN_MODEL`, then
`STORE_CAR_PLAYER_IS_IN`) is the owner's: he has to be in a Mr Whoopee,
driving or riding. Live test: the owner rides, the helper parks and honks,
the helper presses the detonator.

**Trial By Fire (42).** Checked, nothing new: the flamethrower is everybody's
(the stash), the frenzy is replayed, the zone and the Triads' threat are
world instructions. Left: the six bonus Triads appear and turn on the player
for the owner's own position in Chinatown.

**Big'N'Veiny (43).** Fixed: the 30 m locate against the thief that stops the
countdown is answered for the nearest participant. Left (R7, as decided):
the magazines are the owner's engine's `CPacManPickup`s, collected by the
car the owner is in, so a helper driving the van alone collects none and the
26 s clock runs out. With the owner riding it works. Live test: the helper
drives the owner, and the timer grows with each magazine.

### Asuka

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 44 | Sayonara Salvatore | wait unseen, then kill Salvatore | R1 (Salvatore, the Mafia cars), R13 (13 spotted checks), R5 (his garage), R15 | **built** anybody may make the kill or give the game away; the garage shuts on nobody (below) |
| 45 | Under Surveillance | kill the surveillance team | R1 (their van) | **ok** anybody kills the feds and opens the van |
| 46 | Paparazzi Purge | chase the spy boat | R1 (the boat) | **built** anybody keeps the spy in reach (below) |
| 47 | Payday For Ray | payphone to payphone against the clock | nothing | **note** the phones are the owner's |
| 48 | Two-Faced Tanner | kill Tanner | R1, R12 (wanted), R15 (max wanted) | **built** the guests keep their cars for the chase (below) |

#### Asuka in Staunton, step by step (2026-10-01)

Each script read through off the retail decompile (ASUKA1 to ASUKA5) for what
only the owner's machine had, what only asked the owner, and where a guest
doing the step could stall or fail it. Already carried and not repeated: the
condo cutscenes and their teleport, the timers and the DAMAGE bars (R16), the
blips on the cars, peds, objects and coordinates, every `KILL_PLAYER`
(nearest participant), the `ONLY_DAMAGED_BY_PLAYER` targets (R1), the wanted
levels (R12), the garage and road switches (R5, R15), the pay and the next
contacts' threads.

**Sayonara Salvatore (44).** Checked: the spotting (13 `HAS_CHAR_SPOTTED_PLAYER`,
R13), Salvatore and the convoy (R1), `IS_PLAYER_IN_CAR` on the car a guard
took (R4b), the back door (R3), the street cleared for the convoy (R4c, a
guest's car in its way is taken as any car is). Found and *built*:

- *Salvatore's garage.* The door shuts once Salvatore is in and "the player"
  is not, opens again while he is, and the mission fails on a shut door. Asked
  of the owner, it came down on a guest inside: the mission failed and the
  guest was shut in for good (the cleanup leaves the door down). Both checks
  of that box are now yes for any participant inside (standin.h,
  `AreaAnybodyKeeps`), so it shuts only on nobody, as in single player.
- *Whom the guards spotted.* Each guard's spotting is followed by two checks
  that the player is out of the two places where being seen does not count
  (behind the club, across the street). Those were the owner's: a guest seen
  from a safe place gave the game away, and a guest seen in the open was let
  off whenever the owner stood in one. The two checks right after a spotting
  now ask about whoever was spotted, a participant seen in the open first
  (standin.h, `SpottedStandIn`). Salvatore's own spotting has no such checks
  and counts anybody he sees, as before.

*Left, the owner's as protagonist or cosmetic:* the wait for the owner to be
in Portland (`current_island`) and "You've missed Salvatore!" on the clock;
the "If you hang around Luigi's club" line (`in_zone`); the convoy's arrival
scene, played when the owner is out of the block; the cutscene skip (the
owner's pad); where Salvatore comes out (the owner at the foot of the steps
or not); a flipped Mafia car moved back onto the road when it is off the
owner's screen and he is 80 m off; the flankers sent after "You have been
spotted!" by where the owner is (they go for the nearest player anyway); the
passengers opening fire at a stopped car, which is the owner's.

**Under Surveillance (45).** Nothing found. The ten feds and the van are the
owner's mission peds and car; a guest's shots reach the owner's copies (R1),
the "spotted" turn is their own health and the van's (`< 999`, which a
guest's rams bring down as Van Heist's), and the kill orders go for the
nearest player. The timer is R16's. The pass takes Staunton's Ammu-Nation and
Pay'n'Spray icons off by their globals and puts Kenji's marker up, both
already everybody's.

**Paparazzi Purge (46).** Found and *built*: the spy boat runs once "the
player" is within 55 m (`LOCATE_PLAYER_ANY_MEANS_CAR_2D`, in an `if and`
beside the boat being unhurt), and "He's clean out of here!" fails the
mission at 160 m from the boat (twenty times), from the Stallion he swaps it
for (three) and from him on foot (two `00E9`). A guest on the boat's tail in
the Predator, the owner in a boat of his own further back, failed it. The
mission is in nearchar.h's table now, and its car locates are answered like
its pedestrian ones, for the participant nearest. Checked: the Predator is
R4b's (anybody in it takes its blip off), its guns are a drive-by's
(`FireInstantHitFromCar`), the spy boat is R1's. *Left:* the boat help texts
are the owner's; the spy on foot keeps away from the owner's ped
(`CHAR_AVOID_PLAYER`), not a guest's.

**Payday For Ray (47).** The phones stay the owner's (C1): each is a `if or`
of the owner on foot at the phone and lifting it, then the phone's own
answered state on his machine. A guest's copy of each phone rings with the
same message (1.29), so he can read where to go next, but his lifting it does
not move the mission. Found and *built*: the end switches the four phones off
(`TURN_PHONE_OFF`), which was not replayed, so a phone a guest never lifted
rang on after the mission with Ray's old directions (protocol.md 1.67).
Checked: the phone markers are objects (R3, `ADD_BLIP_FOR_OBJECT`), the last
stop at Ray's toilet block is a checkpoint, the camera at each call, the
ignore flags and the paused timer reach everybody. *Left:* while the owner
is on a phone everybody's camera looks at it for the call while the guests
keep their controls; a guest driving at that moment drives blind for a few
seconds.

**Two-Faced Tanner (48).** Checked: the start is a checkpoint (stopped in the
box), Tanner's car and Tanner are R1's, the four stars are R12's (`010E`,
again at each hit), the bar is R16's, the five-star cap is each island's own
restart thread. Found and *built*: the mission clears 20 m round Tanner's
car as its scene starts and again as the chase does, and the checkpoint
everybody had to stop in lies inside that circle; R4c's clear put every guest
out of his car and took it away a second before Tanner drove off. Here a car
a participant sits in is spared, as the owner's is (standin.h,
`ClearSparesRiders`). *Left:* "Tanner's on to you!" fails it if anybody
bumps his car before the scene, as in single player; Tanner on foot keeps
away from the owner's ped alone.

| # | Mission | Built | For a game to show |
|---|---|---|---|
| 44 | Sayonara Salvatore | the garage kept open by anybody inside; the place checks after a spotting asked about whoever was seen | a guest in the garage as the convoy arrives (the door stays up); a guest seen behind the club does not set the guards off; a guest seen in the street with the owner behind the club does |
| 45 | Under Surveillance | nothing needed | a guest alone on the roof kills the feds; a guest's rams open the van |
| 46 | Paparazzi Purge | the spy in reach of the nearest participant | a guest in the Predator chasing with the owner 300 m behind does not get "He's clean out of here!"; a guest coming within 55 m sets the boat running |
| 47 | Payday For Ray | the phones switched off on every machine | a guest who never lifted the Torrington phone walks past it after the pass: no ring |
| 48 | Two-Faced Tanner | the clears spare a guest's car | everybody parked in the checkpoint in their own cars; after the scene the guests are still in theirs and can chase |

### Kenji

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 49 | Kanbu Bust-Out | a cop car, the bomb shop, blow the cell wall, Kanbu to the dojo | R6, R4 (Kanbu), R8, R10 (wall), R12 (wanted) | **ok** the owner rides or drives the cop car; anybody at the wheel fits and sets the bomb (below) |
| 50 | Grand Theft Auto | three cars, mint, into Kenji's lock-up | R5 | **built** anybody delivers each car (below) |
| 51 | Deal Steal | Yardie car, the contact, horn, kill the Colombians, the briefcase | R4, R14, R1, R2, R13 (the rendezvous) | **built** the owner drives the Lobo, everybody arrives in a Yardie car (below) |
| 52 | Shima | the protection money, punish the gang | R2, R10 (store swap) | **built** anybody takes a briefcase and brings the gang out (below) |
| 53 | Smack Down | kill at least 8 dealers | R1 (every dealer) | **built** anybody finds, fights and kills a dealer (below) |

### Kenji, step by step (2026-10-01)

Each script read through on the retail main.scm (KENJI1 to KENJI5) for what
only the owner's machine had, what only asked the owner, and where a
participant doing the step stalled or failed it. What the base bridge and
R1-R16 carry is not repeated.

**Kanbu Bust-Out (49).** The steps are the owner's by design, and work with
him riding: "Steal a cop car" and every frame after it is `IS_PLAYER_IN_MODEL`
police then `STORE_CAR_PLAYER_IS_IN`, so the bomb car is whatever police car
*the owner* sits in, at the wheel or beside a participant who drives (R4b
leaves both unwidened, as decided). Checked: the Staunton bomb shop is
garage type 2, the timed bomb; whoever drives in has it fitted on his copy
and `C_VehicleBomb` writes it on the owner's (`bomb_status == 1`), and the
driver's fire button sets it ticking, which travels the same way (`== 4`).
The car's area check at the station is the car's. The blast: the car goes up
on the machine that simulates it, and the owner's copy is blown up through
its own `BlowUpCar`, which adds the engine's car explosion (type 3) there, so
`IS_EXPLOSION_IN_AREA` 3 sees it in the same frame the car reads wrecked, and
neither fail branch ("You brought the heat down on yourself!") is taken. The
wall swap, the debris thrown, the explosion and the cell sound are on the
replay list (R3, R8, R10). Kanbu follows the owner (`SET_PLAYER_AS_LEADER`),
so his seat is R4's and his re-tie at 8 m stays the owner's; the minimum
wanted level reaches everybody, and the wait for "no wanted level" is the
owner's (a Pay'n'Spray with the owner riding resprays on both machines).
The dojo is a checkpoint; the end scene walks Kanbu out of whatever car he
is in, by his own objective. *Left:* a guest alone in a cop car has the owner
told "You need a cop car to do the job!" until the owner gets in beside him.
*Game:* the guest drives, the owner rides, the guest fits and sets the bomb at
the wall, both get out; the wall goes and Kanbu comes out.

**Grand Theft Auto (50).** Each car's block opens with `if and`
(`IS_CAR_IN_MISSION_GARAGE`, the car stood in the lock-up undamaged), then
asks `IS_CAR_DEAD`. Found: the garage's widening only took a single
condition, so a participant's delivery, whose garage destroys the car and
deletes the owner's copy (Van Heist's `CarDeliveredElsewhere`), left the
block saying no and the next `IS_CAR_DEAD` failed the mission with "The
vehicle is wrecked!". *Built:* it is widened in that block through the
block's own arithmetic (R5, corrected). Checked: getting into a car to set
the lock-up's target is `IS_PLAYER_IN_CAR` (R4b), "stopped in the lock-up"
and "damaged" read the owner's copy, whose dents are its driver's (the
damage appliers set `bIsDamaged`), the repair at the Pay'n'Spray reaches the
owner's copy (`C_Respray`, `Fix`), the cars stay unresprayable on every copy
(0294), the 6-minute timer is R16's. *Left:* the lock-up has one target. Two
cars driven at once each set it every frame, the later in the script's order
(Cheetah, Stinger, Infernus) last, so the earlier one waits at the open door
until the other is in or its driver gets out, and after that its driver has
to get back in and out again for the target to come back to it. The owner's
own lock-up door stays shut on his screen while a guest drives in (R5's open
question). *Game:* the guest delivers the Stinger with the owner across town;
"Car delivered." and the next car, no "wrecked".

**Deal Steal (51).** The Yardie Lobo has one passenger seat and the contact
takes it, following the owner: the owner drives the Lobo. The contact's
pick-up, the rendezvous, the horn and the "player in a Yardie car" checks are
the owner's; the rendezvous is a checkpoint, and R13's check makes every
participant who comes within 10 m out of a Yardie car, or shooting, give the
deal away, so everybody comes in a Yardie car of his own. Checked: the
Colombians and their cars are R1's, the briefcase R2's with its marker on
everybody's radar (03DC), the trap's own 6 m checks go through the same
stealth rule (a participant shooting there springs the trap early, as the
owner would). Found: the end at the casino turns the controls off on every
machine, walks the owner's ped out of the car he came in, and waits `while
is_player_in_car` that car, which R4b answered yes for a participant in it,
frozen there: **a softlock** whenever anybody rode or drove with the owner to
the casino. *Built:* that car is the owner's alone to answer for until the
scene gives the controls back (R4b's exception, `standin.h`). *Game:* a guest
drives the owner to the casino (with the contact dead, or in a four-door);
the scene ends and the mission passes.

**Shima (52).** The three briefcases are R2's, each marker on everybody's
radar; the store and the casino are checkpoints; the store swap is R10's, the
store scene's door R10's, its cutscene everybody's. Found: the Diablo by the
second briefcase turns on the player only for the owner within 10 m, and the
five in Portland only for the owner within 25 m of them, so a guest who got
there first met men who never moved. *Built:* the first is answered for the
participant nearest him (`nearchar.h`), the second for anybody inside the
box (`nearchar.h`, `AnswersPlaceForAnybody`); `KILL_PLAYER` already goes for
the nearest player. Found: the casino scene is Deal Steal's, the same
softlock; *built* the same way. *Left:* the five Diablos and the third
briefcase are only made once the owner's island is Portland (`03C6`), since
the owner's machine makes them and has only its own island's collision: a
guest who crosses first waits for him. Belleville Park's walkers
(`SET_PED_DENSITY`, 0156, Shima's alone) are cleared on the owner's machine
only, cosmetic. *Game:* a guest walks up to the gunman in the park alone and
is shot at; a guest reaching the Diablos first has all five turn on him.

**Smack Down (53).** The dealers are R1's (`SET_CHAR_ONLY_DAMAGED_BY_PLAYER`),
their markers and the KILLS counter everybody's (0167/0168, 0162, R16), a
participant's kill counts as any death does. There is no wanted level of the
mission's own; the pass clears everybody's. Found: each dealer is made when
the player comes within 90 m of his marker, and that marker is a coordinate
blip, so the locate read as a checkpoint: no dealer was made until *every*
participant stood within 95 m, and two players hunting apart met none while
the 80 s clock took them off the streets. And a dealer is let go of, back to
a marker, once the owner is more than 90 m from him, mid-fight with a guest.
*Built:* the 90 m locate at a marker is answered for anybody inside it and is
no checkpoint, and the 90 m locate against a dealer is answered for the
participant nearest him (`nearchar.h`). *Left:* a new dealer's place is drawn
at least 110 m from the owner only. *Game:* the owner and a guest go
different ways; each meets and kills dealers, and KILLS counts both.

### Ray (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 54 | Silence The Sneak | grenades, torch the house, kill McAffrey | R12 (grenades), R8, R5 | **reqs** the ammo checks are the owner's grenades |
| 55 | Arms Shortage | Phil's weapons, defend the condo | R2 (Phil's weapons for everybody), R3 (gate), R1 (the attacking cars), R9 | **reqs** R1 is the whole point of a defend mission with helpers |
| 56 | Evidence Dash | chase the prosecution, collect the evidence, torch the car | R3 (files riding in the car), R1 | **reqs** |
| 57 | Gone Fishing | a police boat, kill Ray's partner at the lighthouse | R3, R8, R1, clock | **reqs** |
| 58 | Plaster Blaster | smash the bodycast with a car or a blast | R3 (object damage), R1 (the ambulance), R12 (wanted), R13 (the decoy's 25 m) | **reqs** |
| 59 | Marked Man | Ray to the airport, the stash | R4 (Ray), R1 (the CIA), R3 (doors), R2 (the stash for everybody), R5 | **reqs** everybody takes the whole stash |

#### Ray, step by step (2026-10-01)

Each of the six read off the retail main.scm (RAY1 to RAY6 in the
decompile; Ray's trigger is the toilet at 38.75, -725.375, and its gate is
`CAN_PLAYER_START_MISSION` like any contact's, so a guest in the marker
starts the host's trigger, `missions.md` §5.2). Every one opens the same way:
`CLEAR_AREA` round the toilet (R4c), `SET_PLAYER_COORDINATES` into it
(everybody beside the owner, §11.2), Ray's cutscene. What the base bridge and
R1-R16 carry is not repeated; this is what was left, what was done and what
a game has to show.

**Silence The Sneak (54).** The grenades are everybody's (`GIVE_WEAPON_TO_PLAYER`),
the house's explosion and four fires R8's, `$WITSEC_HOUSE_GARAGE` R5's
setup, the minimum wanted level R12's, the guards' `KILL_PLAYER` the nearest
participant's. The torching is `IS_PROJECTILE_IN_AREA` round the door, which
counts every `CProjectileInfo` on the owner's machine, a participant's
grenade the owner's engine animates among them. Found: "McAffrey escaped!"
is `not 00E9 160 m` in an `if and` with McAffrey off the owner's screen, then
4 s, so a guest on the getaway car with the owner a block behind lost him.
*Fixed:* the mission is in `nearchar.h`'s table, both its locates answered for
the participant nearest McAffrey. *Left:* the house is checked out by the
owner stopping in the car park (`0199`, whose box ends 2 m short of the blip,
so not a checkpoint); "You have used all the grenades!" and the marker to
Ammu-Nation go by the owner's own grenades and Molotovs, a message only;
`AVOID_PLAYER` is the owner. *Game:* a guest's grenade into the door torches
the house; the guest chases the Sentinel with the owner 200 m back and the
mission does not fail.

**Arms Shortage (55).** The warehouse is a checkpoint (blip and 4 m locate on
the same spot). Phil's weapons are the stash (R2), his lockers after the
fight and his armour the campaign's (`missions.md` §15), the gate's slide
main.scm's (R3), the three Colombian cars `ONLY_DAMAGED_BY_PLAYER` (R1) and
their gang members' kills Phil's or anybody's. `SET_CURRENT_PLAYER_WEAPON` after
each locker taken reaches everybody and only arms a weapon its player has
(`CPed::SetCurrentWeapon` tests `HasWeapon`). Nothing found that stalls.
*Left, the owner's by design:* the two scenes with Phil run the owner's own
ped (`GET_PLAYER_CHAR`, then `RUN_TO` 2 m from Phil) and his pad's skip; the
25 s to get ready ends early when the owner leaves the yard or gets in a car;
"Go and check on Phil!" waits for the owner within 2 m of him (a marker on a
pedestrian, not a checkpoint); the two flankers wait for a spot off the
owner's screen. *Game:* a guest's kills count toward the twelve; a guest
takes the M16 and the owner keeps the weapon he holds.

**Evidence Dash (56).** The prosecution's Bobcat is `ONLY_DAMAGED_BY_PLAYER`
(R1, rams included); any damage drops a file, a mission object placed on the
car and thrown up (R3), with its marker on each machine's own copy and the
corona at the owner's. The files are objects, not pickups: the pickup sync
and its blip replay have nothing of them. Found, two: a file is collected by
a locate of *the owner* at the owner's copy of it (3D 1.5 m, then 2D 1.5 m
after 10 s, 2D 30 m after two minutes), so a guest standing on one picked up
nothing; and the car's speed (20 to 50 by the player within 130, 90, 50,
20 m) and the "photos washed up" failure when it sinks with the player
within 50 m went by the owner alone, so a guest on its tail with the owner
far back had it crawl, and sinking it passed the mission instead of failing
it. *Fixed:* a participant within the file's box, 3 m of slack for his copy
landing off the owner's (`standin.h`, `IsEvidenceLocate`), collects it; the
collection deletes the file on every machine and counts in COLLECTED for
everybody. `LOCATE_PLAYER_ANY_MEANS_CAR_2D/3D` against the mission's car is
answered for the participant nearest it (`nearchar.h`, the car table). The
150 m locate round the car's start (where the next car after a decoy is
made) is left alone. *Left:* "Leave the evidence in a car then torch the
car" is the owner's: `IS_PLAYER_IN_ANY_CAR` then `STORE_CAR_PLAYER_IS_IN`
(R4b), then out of it, then that car wrecked by anybody; with the owner
riding, the car he rides in counts. The car's warp off a stall waits for it
to be off the owner's screen. *Game:* a guest rams the Bobcat and picks up
the file he sees land; the owner's COLLECTED moves.

**Gone Fishing (57).** The Ghost's damage bar is R16's, its health a boat's
collision damage that takes anybody's copy (CBoat has no only-the-player
test), the mines `DROP_NAUTICAL_MINE`'s replay, the partner's grenade at the
RC Bandit an owner-side projectile, the pass his death or his boat's wreck,
whoever's. Found: "Ray's partner has escaped!" is McAffrey's test (`not 00E9
160 m` and off the owner's screen), and the boat's and the car's speeds, when
he bails out, steals a car or drops a mine go by the owner's distance. *Fixed:*
in both tables of `nearchar.h`: every locate against the partner and against
his boat is answered for the participant nearest him. *Left:* "Go and steal a
police boat" waits for the owner in a Predator (`IS_PLAYER_IN_MODEL`, riding
counts, but a Predator has no passenger seat); the scene at the lighthouse
starts at the owner within 180 m or the Ghost hurt, so a guest alone starts
it by shooting it; the camera and the boat anchored in it are the owner's;
where the partner runs ashore goes by the owner's side of the pier. *Game:* a
guest chasing the Ghost with the owner 300 m behind; the partner killed by
the guest passes.

**Plaster Blaster (58).** The ambulance's `ONLY_DAMAGED_BY_PLAYER`, turned on
and off by the owner's screen, is lifted for a participant's hits anyway (R1);
its health under 900 bails the witness out; the SWAT's `KILL_PLAYER` go for
the nearest participant. Found, two: the decoy's 25 m was answered for a
participant only as a single condition, and two of its three are `if and`
with the script's `$FLAG_POLICE_TRIGGER == 0`, asked once the ambulance has
turned for the hospital, so a guest tailing it then was never seen. And the
bodycast: its break is a *health*, `CObject::nBodyCastHealth`, one int16 on
each machine that its damage effect waits to see under 200, so a guest's
smash reached the owner (R3's `C_MissionObjectBreak`) and the owner's
`ObjectDamage` took 75 off a fresh 1000 and broke nothing:
`HAS_OBJECT_BEEN_DAMAGED` never passed for a guest. *Fixed:* the decoy is
answered in its `if and` through the block's own flag
(`missioncombat.h`, `DecoyMayWidenUnder`); a reported break of a bodycast
first puts this machine's health under 200 (`object.cpp`,
`ApplyMissionObjectBreak`; `MI_BODYCAST` and the health in `addresses.h`,
checked against the exe), then replays the break, so whoever smashed it,
every copy is smashed. *Left:* the DAMAGE bar is the owner's health; a car
wheel's damage counts on the machine of the car's own driver (the engine
asks `STATUS_PLAYER`), and that machine's break is the one sent; "Bullets
won't get through" is the owner's weapon; the SWAT come out when the owner
reaches the hospital; "Witness has drowned!" is the owner's copy in the
water. A car bomb on a guest's car goes off on his machine (R6) and its
blast is played on the owner's, which breaks the owner's copy there. *Game:*
a guest rams the bodycast to pieces with the owner 100 m off; a guest behind
the ambulance on its way to the hospital gives the game away.

**Marked Man (59).** Ray follows the owner (`SET_PLAYER_AS_LEADER`, R4's
follower seat), his arrival is his own locate (`LOCATE_CHAR`, whoever drives),
the airport doors main.scm objects (R3), the CIA `ONLY_DAMAGED_BY_PLAYER`
(R1), the stash R2's, the flight timer R16's. The lock-up is a checkpoint at
50 m. *Left, the owner's by design:* "You have left Ray behind" and taking
him back (8 m) are the owner's, whom he follows; the CIA appear and go within
100 m of the owner and turn on him at 40 m (`STORE_CAR_PLAYER_IS_IN` names his
car), which is where Ray is; the doors reopen once the owner is out of the
doorway; Ray's key opens the lock-up when the owner stands at its door
(2 m, not the checkpoint's spot). *Game:* a guest drives the owner and Ray
to the airport; the pager and the Patriot.

**What a game still has to show for all six:** a guest at the toilet starting
the host's Ray mission (the 1.19 m marker; `OtherPlayerAtOurContact`), and
everybody moved into the toilet for the cutscene.

### Donald Love

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 60 | Liberator | a Colombian car through the gate, rescue the gentleman | R5 (the garages), R4 (the gentleman), the doors and the car for anybody | **built** anybody opens the garage doors and brings the Colombian car; the gentleman follows the owner (below) |
| 61 | Waka-Gashira Wipeout! | Colombian car, kill Kenji, dump the car | R12 (wanted), R9, R13 (the car park) | **built** anybody on foot in the car park, or upstairs out of a Colombian car, gives the hit away (below) |
| 62 | A Drop In The Ocean | follow the Cessna by boat, 6 floating packages | R7 (Cessna), R2 (floating packages), R10 (Shoreside opened) | **ok** anybody's boat collects a package for the mission (below) |
| 67 | Grand Theft Aero | the airport, the construction lift (a cutscene), the package | R13 (spotted), R12 (the teleport after the lift) | **built** anybody walks up to the van and leads the yard; the lift is a checkpoint (§3, Shoreside Vale) |
| 68 | Escort Service | protect the truck | R1 (the hitmen), R2, R5 | **built** anybody starts the truck; the stash shuts on nobody (§3, Shoreside Vale) |
| 69 | Decoy | six stars, lead the cops away | R12 (everybody gets the stars), R15 (max wanted) | **built** whoever drives the van is the decoy (§3, Shoreside Vale) |
| 70 | Love's Disappearance | cutscene | nothing | **ok** |

### King Courtney (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 63 | Bling-Bling Scramble | 15 checkpoints against three street racers | R15 (roads), the checkpoints for anybody | **built** anybody's car first at a checkpoint scores it for the group (below) |
| 64 | Uzi Rider | drive-bys with the Yardies in your car | R11, R12 (Uzi), R4, R4b, R1 | **ok** one guest rides along and his kills count (below) |
| 65 | Gangcar Round-Up | three gang cars to the lock-up | R5, a guest's gang car | **built** anybody brings a gang car in, the owner riding or not (below) |
| 66 | Kingdom Come | the abandoned car, then the SPANKed-up madmen and their vans | R4b, R8 (the human bombs) | **ok** the madmen run at the owner (below) |

### Donald Love and King Courtney in Staunton, step by step (2026-10-01)

Read off the retail decompile (LOVE1 to LOVE3, YARD1 to YARD4; the `.sc`
sources have no Yardie missions at all). What R1-R16 and the earlier passes
already carry is only named; what was still left is listed with what was
done about it (`game/standin.h`, `game/nearchar.h` and `game/missioncombat.h`, tested in
`tools/clienttest/standin.cpp` and `missioncombat.cpp`; mission.cpp asks
them). No wire change.

**Liberator (60).** Step by step: the cutscene and `SET_PLAYER_COORDINATES`
at Love's (R12); "go 'jack a Colombian gang car" waits on
`NOT IS_PLAYER_IN_MODEL #COLUMB`; the gentleman, two Colombian cars and twelve
guards are made; the compound gate is main.scm's (`game/gates.h`, open for
anybody's own Colombian car); the owner in the compound (0057) sends the
guards to their posts; standing on foot in front of one of five garages
(019C) opens it (`OPEN_GARAGE`, replayed) and its two guards come out with
`KILL_PLAYER` (the nearest participant); the gentleman is tied to the owner
(`SET_PLAYER_AS_LEADER`) within 3 m, and again within 8 m if he strays; he
is delivered when he himself stands in the sphere at Love's; the end scene,
the $40,000 and the wanted level are everybody's.
- *Fixed:* the five garage doors opened for the owner alone, so a guest who
  found the right one stood in front of a shut door until the owner came.
  019C in Liberator is now answered yes for a participant on foot in front
  of the door, and the compound's alarm (the one 0057 with the compound's
  corners) for a participant in the compound. Both are single conditions
  (`nearchar.h`, the table of places anybody answers, with Smack Down's and Shima's).
- *Fixed:* the Colombian car. Both `IS_PLAYER_IN_MODEL` are single (the wait
  at the start, and the "stop hanging around" check that takes the
  checkpoint down while the player is out of one): a participant in a
  Colombian car answers them (`AnybodyInModel`), so a guest who 'jacked it
  starts the rescue and the owner can follow him in.
- *Left, the owner's by design:* the gentleman follows the owner (his
  leader; R4 keeps his seat), so the pick-up at 3 m and 8 m is the owner's,
  and a guest who opened the garage waits beside him. The gate's "only opens
  for a Colombian car" message is the owner's.
- *Game:* a guest on foot opens the garage the gentleman is in while the
  owner is still on his way; a guest in a Colombian car starts the rescue
  with the owner on foot.

**Waka-Gashira Wipeout! (61).** Step by step: the cutscene; the owner in a
Colombian car in the Newport car park (`IS_PLAYER_IN_MODEL` and 0056 in `if
and`s, the owner's story) makes Kenji, his eight Yakuza and their cars; the
first Yakuza killed sets a minimum wanted level of 3 (R12); two Yakuza
`KILL_PLAYER` (the nearest); the ramp starts Kenji's scene (camera and
controls, replayed); all eight witnesses dead fails it; with Kenji dead, out
of Newport and out of the Colombian car passes it.
- *Fixed:* the stealth. On foot in the car park, or upstairs out of a
  Colombian car, "The Yakuza have identified you!!" fails the mission, and
  only the owner was ever asked, so a guest could walk up to Kenji. It is now
  a participant's too, as R13 decided for every stealth check, at the four
  places the script asks it (two `if and`s ending on the car park, the
  upstairs check with the model check after it, and the check once Kenji is
  dead), each block answered for one participant's state
  (`BlowsCarparkCover`). It is still only asked once the owner is in the car
  park, as in single player.
- *Left:* the arrival that makes Kenji is the owner's, in a Colombian car
  (he may ride in a guest's). So is dumping the car: out of Newport, not in
  a Colombian car.
- *Game:* the owner arrives in a Colombian car and a guest walks up the
  ramp: the mission fails. A guest upstairs in his own Colombian car does
  not fail it.

**A Drop In The Ocean (62).** Step by step: the Speeder by the pier,
`START_DRUG_DROP_OFF` and the 2-minute timer (R7, R16); the plane's marker is
taken down and put back every frame (one blip on every screen, missions.md
§15); six floating packages (not eighteen) dropped where the owner's plane
is (R2), each anybody's to collect, each raising the minimum wanted level
(R12); the police boat goes for the packages in turn; with all six, back to
Love's on Staunton: `current_island == 2` and the owner stopped by the door,
a checkpoint, so everybody comes; the walk in is the owner's ped; the pass
opens Shoreside (`COMMERCIAL_PASSED` in the delta, the subway gates and
tunnel blocks taken away through `DeletesWorldObject`, the roads replayed).
- *Found, nothing to fix:* "get a boat" takes the boat's marker down only
  once the owner is in one (`IS_PLAYER_IN_MODEL` in an `if or`), a marker
  only. Shooting the plane fails it from any machine (R7).
- *Game:* a guest's boat takes four of the six and the owner two; the
  COLLECTED counter counts all six; everybody waits at Love's door.

**Bling-Bling Scramble (63).** Step by step: the payphone's cutscene; the
owner stopped in a car at the start (019B; a race, so it waits for nobody);
his car locked and the camera on the racers (replayed); the countdown
freezes everybody; 15 checkpoints, each scored by whoever is first, then the
owner's points against each racer's; $1,000 a point.
- *Fixed:* the checkpoints. Only the owner's car scored one (00E5), so a
  guest ahead of the racers did nothing for the group while a racer took
  the point. A participant's car in the checkpoint now scores it for the
  group (`nearchar.h`'s table of places), and the race moves on to the next one. The
  racers' rubber band still reads the owner's car.
- *Left:* the start and "you jumped the start" are the owner's car.
- *Game:* a guest first at a checkpoint: "1 of 15!" on every screen and the
  next checkpoint up.

**Uzi Rider (64).** Step by step: the owner's ped is walked to the wheel of
the Perennial, the two Yardies take two passenger seats (R4 keeps them), the
Uzi is everybody's (R12); in Hepburn Heights ten Diablos, counted from
`rampage_kills` (R11, so a guest's drive-by from the fourth seat counts);
the car is the group's (R4b: "Get your ass back in this car!" asks anybody);
back to Yardie turf, the car stopped there; the pass makes the Diablos
hostile (`SET_THREAT_FOR_PED_TYPE`, in the delta).
- *Found, nothing to fix.* One guest rides along; a second has no seat and
  follows.
- *Game:* the guest in the back seat shoots and the KILLS counter moves.

**Gangcar Round-Up (65).** Step by step: `IS_PLAYER_IN_ANY_CAR`, then
`IS_PLAYER_IN_MODEL` for #DIABLOS, #MAFIA and #YAKUZA, then
`STORE_CAR_PLAYER_IS_IN` and "take it to the garage", the lock-up set to
accept that car (replayed by netId, R5); under 900 health it has to be
repaired; the car in the lock-up (`IS_CAR_IN_MISSION_GARAGE`, R5 and
`CarDeliveredElsewhere`) counts it.
- *Fixed:* a guest alone in a gang car did nothing, since every one of those
  checks asked the owner's car. With the owner in no gang car himself, the
  three are answered for the first participant who sits in one, and his car
  is the one stored and handed to the lock-up (`RoundUpSubject`, stored the
  way The Fuzz Ball stores a participant's car). The owner's own gang car
  comes first.
- *Left:* one car at a time, as in single player: a second guest's gang car
  waits until the first is in.
- *Game:* the owner on foot, a guest drives a Sentinel into the lock-up and
  walks out: "Mafia gangcar boosted!".

**Kingdom Come (66).** Step by step: the 90-second timer (R16); the abandoned
Esperanto, anybody in it (R4b); the letter and the demonstration (camera,
controls, the blast replayed, R8); four madmen leave their vans and run at
the player's coordinates, going up within 3 m of them or once hurt; every
van and madman gone passes it; the Yardies turn hostile (in the delta).
- *Left:* the madmen run at the owner (`GET_PLAYER_COORDINATES` names no
  pedestrian, so there is no nearest player to give); a guest shoots them
  before they reach him. Not a stall.
- *Game:* a guest shoots a madman and he goes up where he stands, on every
  screen.

### Asuka, Staunton and Shoreside

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 71 | Bait | lure the Colombians into the ambush | R1, R11, R15 | **built** anybody draws a cartel car to the trap (§3, Shoreside Vale) |
| 72 | Espresso-2-Go! | wreck 9 stalls on three islands | R3 (object damage), R8 | **ok** anybody's wreck counts; a stall is made by the owner's island (§3, Shoreside Vale) |
| 73 | S.A.M. | boat to the buoy, rocket the Cessna, the cargo | R2 (rocket), R3, R7 (Cessna) | **built** anybody's boat collects the cargo (§3, Shoreside Vale) |

### D-Ice (payphone)

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 74 | Uzi Money | 20 Nines with Uzi drive-bys, a frenzy | frenzy replay | **ok** `rampage.md` carries it |
| 75 | Toyminator | RC buggy against the armoured vans | R7 | **note** the buggy is the owner's; a helper's own weapons wreck a van too (R1) |
| 76 | Rigged To Blow | the rigged car to the defusal garage, no damage | R5 | **ok** anybody drives the Infernus there and back (§3, Shoreside Vale) |
| 77 | Bullion Run | 30 pieces of bullion in a car, drop-offs | R7 (power pills), R5 | **note** only the car the owner sits in collects, at the wheel or riding |
| 78 | Rumble | bat, the contact, the gang war | R1, R2, R13 | **built** a guest's bat kills count; anybody on the ground starts the fight (§3, Shoreside Vale) |

### The finale

| # | Mission | What it asks | Needs | Verdict |
|---|---|---|---|---|
| 79 | The Exchange | the weapons taken, the dam, the chopper, the credits | R12 (everybody disarmed), R7 (Catalina's helicopter), R2 (the rocket launcher, everybody's own), R15, clock, the credits replayed | **built** anybody brings the chopper down or the helipad guards out; Maria follows the owner (§3, Shoreside Vale) |

### Shoreside Vale, step by step (2026-10-01)

Read off the retail decompile (LOVE4 to LOVE7, ASUSB1 to ASUSB3, HOOD1 to
HOOD5, CAT1), every instruction in order, for what only the owner's machine
had, what only asked the owner, and where a participant doing the step
stalled or failed it. What R1-R16, R4b-R4d and the earlier passes already
carry is only named: the cutscenes and their teleports into Love's,
Asuka's and the construction site (R12), the timers, counters and DAMAGE
bars (R16), every blip, every `KILL_PLAYER` and `DESTROY_CAR` at the owner's
car (the nearest participant), the `ONLY_DAMAGED_BY_PLAYER` targets (R1), the
wanted levels (R12), `EXPLODE_CAR` (to the car's holder), the garages (R5),
the kills (R11), the zone and gang info (R15), the pay (the $500,000 The
Exchange takes is the owner's alone, the $1,000,000 everybody's) and the
death and arrest overrides (R12). What was found and built is in
`game/nearchar.h`, `game/standin.h` and `game/anyplace.h`, tested in
`tools/clienttest/shoreside.cpp`; mission.cpp asks them. No wire change.

**Grand Theft Aero (67).** Step by step: the cutscene at Love's; the owner's
island is Shoreside (`03C6`, the hangar is made by the owner's machine on
its own collision); the player within 200 m (00E3, widened) makes the vans,
the wingless Dodo and four goons; within 90 m the goons go to cover; the
hangar's three areas (0056) send a goon at the player; with all four dead,
"The package should be in the plane": `IS_PLAYER_IN_CAR` on the Dodo (R4b,
anybody); "Track down the Colombians": the player *on foot* within 6 m of
the Panlantic van (`not 01FD`), then a camera on the van and the checkpoint
at the construction site; the owner's island Staunton makes the yard, ten
goons on routes, the yard's areas (0056) pushing them on; the lift (`not
80F6`, 1 m on foot, a checkpoint); the tower cutscene, the broken lift kept
past the mission (01C7), the Yakuza; back to Love's (`80F9`, stopped, a
checkpoint) and the walk in.
- *Fixed:* the van. 01FD was not widened anywhere, so only the owner
  walking up to it moved the mission on. Grand Theft Aero is in nearchar.h's
  car table now, whose locates take their on-foot and in-car forms too
  (01FD, 01FE, 0200, 0201), answered for a participant on foot (or in a car)
  inside the box, alone or in an `if or` only (`CarLocateMayWiden`).
- *Fixed:* every goon who spots the owner in a car stores that car to
  destroy it (`00DA` then `01D9`, all 23 stores in the mission). R4d took the
  first as "the stored car", and from then on every place of the mission
  (the yard's areas, the lift) was answered only for a participant sitting
  in it, the on-foot lift for the owner alone. In this mission a store is not
  the stored car (`anyplace::StoreIsOnlyATarget`); Marked Man's, the CIA's,
  are left as decided.
- *Left:* the hangar and the yard are made when the owner's island is
  Shoreside and Staunton; the Dodo has no wings and stays put; the camera
  angle on the van comes from where the owner stands; the walk into Love's
  is the owner's (R4d).
- *Game:* a guest on foot at the van with the owner 50 m off: the camera
  goes to the van and the checkpoint goes up; the owner drives into the
  hangar, a goon is sent at his car, and a guest at the lift still counts.

**Escort Service (68).** Step by step: the Securicar with the Oriental
Gentleman and its driver, locked; the M16 at Love's (R2); the truck waits
for the player in a car within 15 m (`if or` of `not 01FE` and its spot off
the owner's screen; on foot within 15 m says "You'll need a car!", on foot
within 1 m and stopped also starts it); then its route through the tunnel to
Pike Creek; ten Colombians made and let go of by the player or the truck
within 220 m (00E3 in `if or`s with 01AD); four Colombian cars that block and
ram it, their men told to destroy it and to `KILL_PLAYER` when the player is
in their car (R4b); at the end Love's stash garage opens, shuts once the
truck stopped inside and the player is not in it (`not 0056`), opens again
while he is, and the shut door passes the mission.
- *Fixed:* the start. Escort Service is in the car table: a guest in a car
  by the truck answers the `not 01FE` of the `if or`, and one on foot gets
  "You'll need a car!" up. The `if and` of 1 m on foot and
  `IS_PLAYER_STOPPED` stays the owner's (one player's state).
- *Fixed:* the stash. The door came down on a guest who had followed the
  truck in, the owner more than 60 m off, and the cleanup leaves it shut. It
  is a box anybody keeps now, as Salvatore's garage is (`standin.h`,
  `LOVES_STASH_INSIDE`): it shuts only on nobody.
- *Left:* the truck is simulated on the owner's machine, and its leg through
  the tunnel waits for the owner's island (`03C6`, "Go ahead and scout the
  exit of the tunnel!"); the start still needs the truck's spot on the
  owner's screen.
- *Game:* a guest pulls up beside the truck with the owner on foot watching:
  "Lets go!"; at the end a guest walks into the stash behind the truck: the
  door stays up until he walks out.

**Decoy (69).** Step by step: the cutscene; the owner's island Shoreside,
the player within 200 m of the warehouse (00E3, widened) makes the
Securicar, eight police cars and SWAT vans and six officers;
`SET_MAX_WANTED_LEVEL` 6 (each machine's own S_RSTRT thread already set 6
when its player first came to Shoreside, so it is left the owner's); in the
van (R4b), six stars every frame for everybody (R12), the 3-minute timer and
DAMAGE bar (R16), the cars ram the van and the officers destroy it or
`KILL_PLAYER`; out of the van, 15 s to get back (`not IS_PLAYER_IN_CAR`,
anybody); at the end, the player more than 160 m from the warehouse passes,
within it fails ("You failed to lead the police far enough away!").
- *Fixed:* the end asked the owner where he was. A guest driving the van
  away with the owner left by the warehouse failed the mission the group had
  done, and R4d's rule would have failed it as well for a guest by the
  warehouse with the owner just past 160 m. While a participant sits in the van and
  the owner does not, the end is answered for the van (`standin.h`,
  `IsDecoyEnd`, `DecoyRider`; the van is the car the mission asks
  `IS_PLAYER_IN_CAR` about); otherwise it is the owner's own, and R4d leaves
  it alone (`anyplace.h`, a site with its radius, `Site::r`).
- *Left:* nothing that stalls. Six stars are on every participant whether
  or not he is near the van.
- *Game:* a guest drives the van 300 m away, the owner stays by the
  warehouse: the clock runs out and the mission passes; the other way round
  (the owner in the van far off, the guest by the warehouse) passes too.

**Love's Disappearance (70).** A cutscene at Love's (the clear, the teleport,
R4c, R12) and the pass. Nothing found.

**Bait (71).** Step by step: the cutscene at the construction site; four
checkpoints (the three cartel cars' markers and the killzone); the KILLS
counter; the Yakuza ambush is made with the owner in Pike Creek or Cochrane
Dam on Shoreside (zones, widened), each cartel car when the owner is in its
zone (widened) or within 70 m of D (00F5); a cartel car sets off when the
player comes within 30 m (A, D) or 40 m (B), rams whoever is nearest
(`SET_CAR_MISSION` 2, RetargetRam), slows when the owner is far, bails out
below 250 health or when the owner is on foot by it, and counts for Asuka
when it stands in the killzone; its men then `KILL_PLAYER` or go for a
Yakuza; three Yakuza killed by the player (`rampage_kills`, R11) turn them;
a sunk or flipped car is put back after the player out of the owner's sight
(`SET_JAMES_CAR_ON_PATH_TO_PLAYER`, to the car's holder).
- *Fixed:* the cartel markers. Each was a checkpoint (a coordinate blip at
  the locate's point), so no cartel car set off until every participant
  stood by its marker, and a locate R4d widened only with the owner within
  60 m. They are nearchar.h's places anybody answers now, as Smack Down's
  dealers: a guest alone at a marker draws its car, which rams the nearest
  player, and leads it into the trap; the counter is the car's.
- *Left:* the killzone check (`FLAG_WELLYBOB`) is written with corners as a
  radius and is never true, in single player as here; the bail-out on foot
  and the car's speed go by the owner's position; the Yakuza are made by the
  owner's zone.
- *Game:* a guest alone by cartel A: its car comes for him, he leads it into
  the killzone, "1 of 3".

**Espresso-2-Go! (72).** Step by step: the cutscene; nine stalls, each made
with its Cartel pusher by the owner's island (`03C6`) and zone (0121,
widened) or within 150 m (00E3, widened); each one wrecked
(`HAS_OBJECT_BEEN_DAMAGED`, R3's `C_MissionObjectBreak`) counts, sets it
alight (`START_SCRIPT_FIRE`, replayed) and sends its pusher running; the
first starts an 8-minute clock (R16).
- *Found, nothing to fix:* a guest's wreck of his copy reaches the owner's
  and counts (R3); the pushers' `KILL_PLAYER` go for the nearest.
- *Left:* a stall exists only once the owner has been on its island (the
  owner's machine places it on its own ground, z -100): a guest on another
  island waits for him there.
- *Game:* a guest rams a stall in Bedford Point with the owner across the
  street: "1 OF 9!" and the clock starts.

**S.A.M. (73).** Step by step: the cutscene; the timer; the Reefer and the
rocket launcher (R2, everybody's own); ten buoys and the platform buoy
(R3); the boat's and the buoy's markers swap on "the player in a Reefer, a
Predator or a Speeder" (three to an `if or`, and their NOTs in an `if
and`); within 160 m of the platform the owner's machine loads Shoreside
behind a screen (`044C`); within 15 m, "Wait for the plane"; under 91 s the
drug plane (R7, every machine's own, the shot-down answered for anybody);
shot down: a wrecked Dodo and eight packages thrown onto the water where
the owner's plane is; with the player in a boat, a package is taken within
4 m of where the owner's copy floats (00F5), otherwise by touching it
(`IS_PLAYER_TOUCHING_OBJECT`); with all eight, the stash at the
construction site (1 m on foot, a checkpoint), the ransom cutscene. The
Colombians on the docks turn on the player within 60 m and 80 m.
- *Fixed:* the cargo. A guest in a boat collected nothing: the boat checks
  asked the owner's model, and the locate the owner's position at the
  owner's copy. The boats are anybody's model now, through each block's own
  flag (`standin.h`, `AnybodyInModel`, block arithmetic), and a participant
  within 4 m of the owner's copy, 4 m of slack for his own copy's drift,
  takes the package (`IsCargoLocate`, `CollectSlack`): the deletion and the
  count reach everybody as Evidence Dash's files do.
- *Fixed:* the docks are places anybody answers (nearchar.h).
- *Left, the owner's:* the 160 m island load (anyplace.h site, by its
  radius: it moves the owner's collision); touching a package when nobody of
  the group is in a boat; the plane landing (`033B`) reads the owner's copy,
  which flies on after a guest brought his down: a shot-down in the last
  seconds before it touches the runway can still fail it.
- *Game:* the owner on the shore, a guest in the Reefer takes all eight
  packages; "8 OF 8" and the stash marker.

**Uzi Money (74).** The frenzy (`START_KILL_FRENZY`, replayed, `rampage.md`);
the start waits for the owner in any car or in Wichita Gardens (R4d's car
beside). Nothing found: a guest's drive-by kills count in every CDarkel.
*Game:* the owner rides, the guest drives, both shoot.

**Toyminator (75).** The Toyz van (`IS_PLAYER_SITTING_IN_CAR`, the owner's),
the RC buggy given to the owner (R7), the armoured vans `ONLY_DAMAGED_BY_PLAYER`
(R1: a helper's rockets wreck them too), the owner out of any car fails it. Nothing
found that stalls. *Left:* the buggy is the owner's alone.

**Rigged To Blow (76).** The Infernus, unresprayable on every copy (0294),
anybody in it (R4b, `IN_VEH` per participant); the defusal garage is a
keep-car mission garage set on every machine by netId (R5): a guest's own
garage shuts on the car he drives in, reopens, and answers
`IS_CAR_IN_MISSION_GARAGE`; the DETONATION bar is the owner's copy's health,
which its driver's stream carries; at 100 or with the timer out,
`EXPLODE_CAR` goes to whoever holds it; back to D-Ice stopped, undamaged
(`IS_CAR_DAMAGED`, the driver's dents; a Pay'n'Spray repairs every copy),
nobody in it. Nothing found. *Game:* a guest drives it both ways; the door
shuts on him and opens again.

**Bullion Run (77).** The car is the owner's (`IS_PLAYER_IN_ANY_CAR`,
`STORE_CAR_PLAYER_IS_IN` every frame), the bullion are the owner's power
pills (R7), collected by the car the owner sits in, at the wheel or riding;
the drop-off garage follows that car. *Left, as decided:* a guest alone in
his own car collects nothing; the car's extra weight is the owner's copy's.

**Rumble (78).** Step by step: the bat (R2); the contact, tied to the owner
within 8 m (`SET_PLAYER_AS_LEADER`, the owner's); his three lines; nine Nines
`ONLY_DAMAGED_BY_PLAYER`; the player on the fighting ground (0057) turns
them on him and lets the contact fight; each death must be by bat
(`HAS_CHAR_BEEN_DAMAGED_BY_WEAPON` 1), and any shooting in the district fails
it (`IS_PLAYER_SHOOTING_IN_AREA`, anybody, R13; a bat swing does not set
the shooting flag, `CWeapon::Fire`'s melee arm).
- *Checked:* a guest's bat hit lands on the owner's copy with its weapon
  (`ApplyRemotePedDamage` passes it to `InflictDamage`), so his kills count.
- *Fixed:* the fighting ground is a place anybody answers (nearchar.h): a
  guest there brings the Nines out with the owner still talking to the
  contact.
- *Left:* the contact follows the owner.
- *Game:* a guest kills three with the bat: they count; a guest's pistol
  shot fails it.

**The Exchange (79).** Step by step: the cutscene at the mansion, the
guards, everybody disarmed (R12), the Colt (R2); out of the mansion grounds
(0057, widened: the chopper waits while a guest is still inside with the
owner near), Catalina's chopper (R7, every machine's copy; brought down by
anybody); the dam and its zones (widened), the guards and flatbeds, the
rocket launcher (R2, everybody's own); the helipad box (0057) sends its
guards; Catalina runs, the chopper takes off; shot down or not, Maria is
tied to the owner on foot within 10 m and must be in his group to end it;
the END cutscene, the credits (every teleport, the clock, +$1,000,000 for
everybody), `SHORESIDE_COMPLETE`.
- *Fixed:* the helipad is a place anybody answers (nearchar.h).
- *Left, the owner's:* Maria follows the owner (his group); skipping the
  credits is his pad; the weather of the credits is his screen's.
- *Game:* a guest's rocket brings the chopper down; the owner walks to
  Maria and the END cutscene plays on every screen.

| # | Mission | Built | For a game to show |
|---|---|---|---|
| 67 | Grand Theft Aero | the van for anybody on foot; goons' stores are no stored car | a guest at the van; a guest at the lift after a goon went for the owner's car |
| 68 | Escort Service | the truck's start for anybody in a car; the stash kept open by anybody inside | a guest starts the truck; a guest in the stash keeps its door up |
| 69 | Decoy | the end asked of the van's driver | a guest drives the van away, the owner stays: passed |
| 71 | Bait | the cartel markers for anybody, no checkpoint | a guest alone draws cartel A into the trap |
| 73 | S.A.M. | the boats for anybody; the cargo for a participant on it; the docks | the owner ashore, a guest takes the eight packages |
| 78 | Rumble | the fighting ground for anybody | a guest's bat kills count |
| 79 | The Exchange | the helipad for anybody | a guest on the helipad brings its guards out |

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
- R7's crusher crane: that the holder's crane lifts the car with its custody,
  that the other screens follow the hook, and that the owner's
  IS_CAR_PICKED_UP_BY_CRANE and IS_CAR_CRUSHED say yes when a helper delivers.
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
