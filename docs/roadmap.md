# Roadmap

The goal: the GTA III campaign, played co-op, with the game otherwise 1:1
faithful. This file says where CoopIII is, what is left, and the decisions
that shape the rest.

Read `protocol.md` for the wire contract, `missions.md` for how the story is
shared, `campaign.md` for the original campaign design, and `compat.md` for
the constraints the player's other mods impose. The feature write-ups are
`population.md`, `wanted.md`, `pickups.md`, `rampage.md`, `cardamage.md`,
`objects.md`, `radio.md` and `cheats.md`.

---

## 1. Where it stands

Free roam is close to done. Players, traffic, pedestrians, the police, cars
and everything that happens to them, pickups, garages, rampages, the clock and
the weather are the same on every screen. The story can be played together:
one player starts a mission, it runs on their game, and everybody else in the
session plays it with them.

It is still in active development. Nearly everything below is covered by the
test suites, but a lot of it has only had short sessions in a real game, and
playtesting keeps turning up bugs that get fixed as they are found. §7 is the
list of what is known to be open.

Protocol version **70**. `sdk/include/coopiii/protocol.h` has the history of
every version and what an older build does against a newer one.

Status words used below:

- **Done**: built, tested, and in the build.
- **In progress**: being built or being tested in real sessions.
- **Planned**: decided or designed, not started.

---

## 2. The structural problems

None of these are bugs. It is how a 2001 single-player engine works, and it
shapes every milestone below. Meeting them early is cheaper than running into
them at milestone 5.

### 2.1 The streamer centres on one player

```cpp
StreamZoneModels(FindPlayerCoors());   // re3 src/core/Streaming.cpp:346
```

The game loads models and collision around *the* player, singular. Two players
a few hundred metres apart are fine. Two players across the map are not: the
remote one is standing in geometry that, on your machine, isn't loaded.

Consequences to design for:
- A remote player outside your streaming radius has no model to render.
- They will fall through the world if collision is not loaded there.
- Naively requesting models for every remote player will thrash the streamer
  and blow the memory budget the game was tuned for.

Options, none free: keep players soft-tethered; stream a reduced set around
remote players; or accept that distant players are represented by a blip only.
Settled in §5.3: a distant player is a blip.

### 2.2 Only one island's collision is in memory

```cpp
CGame::currLevel;                                        // Game.h:15
CModelInfo::RemoveColModelsFromOtherLevels(currLevel);   // Game.cpp:651
CCollision::ms_collisionInMemory = currLevel;            // Game.cpp:652
```

`eLevelName` is `INDUSTRIAL` / `COMMERCIAL` / `SUBURBAN`, and it is a single
global. So two players on different islands is not a distance problem at all.
The other island does not exist in this process.

The campaign gates islands behind story progress, so in a co-op campaign
everyone is usually on the same island, which saves us. The missions keep
their players together (the start gate, the checkpoints, and moving everybody
with the mission's own teleports, loading the island first). Free roam has no
such guarantee, and it stays the known limit there.

### 2.3 There is exactly one player slot

`NUMPLAYERS = 1` (`config.h:7`). Remote players are ordinary `CPed`s, never
`CPlayerInfo` entries (`protocol.md` §1.3). Everything that reaches for
`CWorld::Players[0]` is local-player-only *by definition*: the wanted level,
the camera, the HUD, the script.

### 2.4 Physics is not reproducible across machines

`ms_fTimeStep` is frame-time-derived (`protocol.md` §1.2). There is no fixed
timestep, so the same inputs do not produce the same outputs on two machines.
Lockstep and rollback are both out. Hence the client-authoritative design with
a host-authoritative script (`campaign.md` §2).

---

## 3. Milestones

A bare §1.x, or §2.5 and above, is a section of `protocol.md`. §2.1 to §2.4
and §5 are this file's.

### M1 - See each other: done

- Remote players are real peds in the world, created through the engine's own
  `CREATE_CHAR` path.
- Animation: base and partial, with their phase, so the pose, the run and the
  weapon in hand match (`protocol.md` §1.8).
- Aim: yaw through `CPed::SetAimFlag`, pitch through the engine's own IK, so
  the arm or the torso bends the way it does for the local player.
- Clothes: Claude's outfit follows the story on every screen (§1.32).
- Nametags with weapon, name and health, drawn with the game's own font and
  `hud.txd`, faded by distance and behind walls (`client/src/game/nametag.h`).
- Remote players on the radar as the same rotating arrow the local player gets
  (`client/src/game/radar.h`), including players too far away to have a ped
  (§5.3).
- A player standing on something that moves (a car, a boat, the El, the
  subway) is drawn on it, not beside it (protocol version 66).

### M2 - Vehicles: done

- A car joins the session when a player gets into it. Cars nobody has touched
  stay each machine's own; traffic is shared through the population sync (M4).
- Transform and velocities at 25 Hz, full rotation as a quaternion, controls,
  interpolation, and correction after physics every frame.
- Getting in and out plays through the engine's own `CPed::SetEnterCar` and
  `SetExitCar`, with the door announced when the entry starts, and a warp
  behind it so the seat is always right (`protocol.md` §1.14).
- Passengers: `seatKey` (G) takes the first free passenger seat of the nearest
  car. When a mission puts its player in a car, the free seats go to the other
  players and whoever gets none follows in their own (§1.40).
- Ownership: the player in seat 0 owns the car; a car still moving with
  nobody driving gets a custodian until it stops (§5.8.1, §5.8.2). A carjack
  plays as one on every screen and hands the car over.
- Colours and extras are the same on every copy (§5.9).
- Damage: health, panels and doors, fire, and the wreck, for cars somebody
  drives and for cars nobody does (§5.8, `cardamage.md`, `protocol.md` §1.38).
- Horn, siren, lights, alarms, the tank turret and the fire truck cannon, the
  taxi light and the handbrake (§1.39).
- The car radio: everybody in a car hears the same station at the same moment
  (`radio.md`, §1.33).
- **Boats.** `SpawnRemoteVehicle` builds a `CBoat` for a boat model, chosen
  the way `CREATE_CAR` chooses (model info type and `+0x58`, not model ids).
  The car-only writers are guarded on `m_vehType`: the damage model is
  skipped both ways, and the fire timer is held on the boat's own member
  (`+0x2CC`, not `+0x530`). A replica is seated in a boat by warp.
  `addresses.h`, "boats", and `game/boat.h`.
- A parked car somebody takes is gone from its spot on every screen, and the
  crusher, the Portland crane, Craig's garages and the safehouse garages act
  only on the copy of the machine holding the car (§1.44).
- A crane is worked by the machine holding the car it takes, which settles
  the car while the crane has it; every other screen follows the hook and the
  car, and a car crushed on one machine answers `IS_CAR_CRUSHED` on the
  others (§1.62). Not run in-game.
- A traffic car its host's engine would drop next to another player is handed
  to that player instead of vanishing (§1.45).
- Never `STATUS_PLAYER_REMOTE`: it is RC-car mode and it detonates cars
  (`protocol.md` §1.4).

### M3 - Combat: done

- Weapon, aim and, behind the server's `ammoSync`, ammunition (§5.12).
- Shots replayed through the real `CWeapon::Fire`, along the shooter's own
  direction, so impacts, trails and decals happen for real (§1.9). Sniper
  rounds are heard and land on every screen.
- Damage and death through `CPed::InflictDamage`, `SetDie` and `SetDead`, the
  victim's machine deciding its own health. Respawn at the hospital, arrest at
  the police station.
- Melee, with the right reaction on the victim's screen.
- Friendly fire, off by default (§5.2).
- Fire: whatever lit it, it burns on every screen and burns everybody (§5.7).
- Explosions, car bombs (bought or fitted by a mission) and mines, blamed on
  the right player (§1.36).
- Drive-bys, and passengers aiming a pistol or uzi out of any window with the
  mouse camera (§1.42).
- The lock-on skips teammates (§1.41).

### M4 - The world: done

- Ambient pedestrians and traffic: whoever's engine made one hosts it, and
  everybody else sees the same crowd (`population.md`). They fight every
  player, not only their host's. A leaving player's crowd and traffic are
  adopted by the others rather than dropped.
- The wanted level, per player by default, with the police chasing whoever is
  wanted (§5.1, `wanted.md`). The police helicopter and its gunfire. A
  Pay'n'Spray or a bribe clears stars for the car or the shared session
  (protocol version 70).
- Pickups: one player gets each one, the server decides who (`pickups.md`).
  Money and weapons a dead ped drops. Hidden packages, shared by default
  (§5.11).
- Rampages, shared, with a vote before one starts, for pedestrians and cars
  (§5.10, `rampage.md`).
- All 32 garages and their doors, Pay'n'Spray repairs and repaints (§1.16),
  and the scripted gates `gates.sc` opens (§1.43).
- Street objects: broken and knocked over on every screen, and still broken
  for a player who comes back (§5.13, `objects.md`).
- The clock and the weather follow the host (§2.7). Trains, planes, traffic
  lights and the Shoreside lift bridge run on the session's clock.
- Medics revive and fire trucks spray on every screen (§1.37).
- Cheats run where what they change is owned (§5.14, `cheats.md`).
- Money: off, own wallets or one shared wallet, as a server setting.

### M5 - Campaign together: done, being played through

The design changed from `campaign.md`'s "only the host runs the script" to
the one in `missions.md`: every machine keeps running its own `main.scm`, a
mission runs on the machine of the player who started it, and what it does is
replayed on everybody else's through the engine's own interpreter.
`missions.md` §15 is the full list of what is built.

- One mission at a time for the whole session, for two players or more. It
  starts only when everybody is at the marker (`missionMargin`, 5 m), and
  every checkpoint waits for everybody, 60 s at most.
- What the mission shows reaches everybody: text, blips, spheres, markers,
  coronas, HUD timers and counters, dialogue, sounds, the mission's
  pedestrians and cars, its pickups, objects, explosions and fires.
- Cutscenes play for everybody, loaded together, and a skip is a vote of
  everybody in it (§1.34). The mission's teleports move every participant,
  loading the island first when needed.
- Everybody is paid. A death or an arrest fails the mission for everybody,
  as in single player (§5.4, `missionFailOnDeath`). A failed mission can be
  retried from its marker.
- Campaign progress is shared: when a mission ends, the globals and threads
  it changed go to everybody (§1.30), so the next missions unlock on every
  machine and a late joiner or a loaded save catches up.
- A player who joins or reconnects during a mission is brought into it.
- Mission enemies can be made tougher, or more of them, for more players
  (`missionEnemies`, `missions.md` §10).
- Odd jobs, the RC, 4x4 and Mayhem runs are session missions too; a passenger
  can't start the driver's side job.

Done when a group can play the story from start to end together. The work
now is playing it through in order and fixing what each mission shows up.

### M6 - Anybody starts a mission: done

Any player can walk into a marker and start the mission for the session. No
trigger opcodes had to be intercepted: every machine already runs its own
triggers, so starting only needed arbitrating at `CAN_PLAYER_START_MISSION`
(`missions.md` §3.2, §5.2). The hooks are on the script engine's range
handlers, which works with or without III.CLEO installed.

Since 2026-09-29 a contact's or a payphone's mission is the host's to start:
each machine's triggers are its own save's, which may be behind or ahead of
the host's, so a guest's own are refused and its own contacts are not drawn;
it sees the host's instead (`missions.md` §5.8, `protocol.md` §1.49). The odd
jobs and the RC, 4x4 and Mayhem runs are still anybody's.

### M7 - Everything that makes it usable: done

- Chat on the HUD (T), with join and leave lines and a line when the
  connection drops.
- The scoreboard (hold Tab, or pin it with F9): health, stars, what each
  player is doing, who is paused, and ping.
- Reconnection that puts you back where you were, in the same car.
- Desync diagnostics: the F9 list and both logs say when a copy of something
  is more than a few metres from where its owner has it (§1.26).
- The host kicks from the chat (`/kick 3`, `/kick bob`) (§1.28).
- The version in the bottom-left corner.
- The dedicated server: a window with the log, the players and an options
  dialog, or `--nogui` for a console. `CoopIII-Server.ini` with a comment on
  every setting, an optional password, and a start-up line that says which
  address to hand out and whether the port needs forwarding. On start it
  looks up the public address, asks the router to forward the port over UPnP
  (closed again on a clean stop) and reads Windows Firewall's rules, with a
  button that adds an allow rule through an elevated netsh. The UPnP and
  firewall parts have not met a real router or a blocking firewall yet.
- The launcher: checks the install, starts the game with the mod on (§5.6),
  and has a lobby where everybody waits and the host starts every game at
  once, into a new game or at the menu (§1.31).
- CoopIII Setup: one installer that carries the mod and the launcher and
  turns a Steam copy of the game into v1.0 if needed.

---

## 4. Sync inventory

What travels, in one place. Everything here is **Done** unless it says
otherwise; the linked section has the details. Sources are re3 members
(`protocol.md` §1.7) and every offset is proved in
`client/src/game/addresses.h`.

### Player

| What | How |
|---|---|
| Transform, velocity | snapshot at 25 Hz |
| Health, armour | snapshot, and on the join packet so a late joiner gets the real value (§2.8.1) |
| Ped state, move state | snapshot; the move state is applied so the engine picks the walk or run |
| Animation | base and partial with their time, through `CAnimManager::BlendAnimation` |
| Weapon, aim | `GiveWeapon` + `SetCurrentWeapon`; yaw through `SetAimFlag`, pitch through the IK |
| Ammo | behind `ammoSync`, off by default (§5.12) |
| Shots | reliable, replayed through `CWeapon::Fire` along the shooter's direction (§1.9.7) |
| Damage, death, respawn, arrest | the victim applies it through `InflictDamage`; death is held by the session for joiners |
| Enter/exit, carjack | through the engine's own entry and exit, the door announced at the start (§1.14) |
| Passenger seat | `seatKey`; the engine picks the seat (`game/seat.cpp`) |
| Wanted level | spare bits of the snapshot's flags, with the rule in `S_Welcome` (`wanted.md`) |
| Clothes | `C_PlayerLook` (§1.32) |
| Riding on something | `C_PlayerStateRide` (protocol version 66) |
| In the pause menu | `C_PlayerAway` (§1.41) |

### Vehicle

| What | How |
|---|---|
| Transform, velocities, controls | snapshot, quaternion rotation |
| Health, engine, siren, lights, horn, taxi light, handbrake | snapshot flags |
| Colours, extras | spawn packet (§1.12, §5.9) |
| Occupants | every seat, remembered by the session for joiners (§2.8.2) |
| Destruction | the driver, or for a car nobody drives the machine entitled to it (§5.8) |
| Damage model | panels and doors, change-only, merged as a maximum (`cardamage.md`) |
| Alarm, turret, fire truck cannon | own packets, kept for joiners (§1.39) |
| Radio station | one per car, on the session clock (§1.33) |
| Bomb | whose it is and what is left of the fuse (§1.36) |
| Boats | same packets, `CBoat` on the receiving side |

### World

| What | How |
|---|---|
| Clock, weather | the host's, or the mission owner's during a mission (§2.7) |
| Trains, planes, traffic lights, lift bridge | the session's clock, no packets |
| Ambient peds and traffic | hosted by whoever's engine made them (`population.md`) |
| Police, helicopter | the police are ambient peds; the helicopter has its own opcodes |
| Pickups, ped drops, hidden packages | exclusive, arbitrated by the server (`pickups.md`) |
| Rampages | shared, with a vote, for peds and cars (`rampage.md`) |
| Garages, doors, Pay'n'Spray | one bit per garage, the union held on every machine (§1.16) |
| Scripted gates | one bit per gate, the same union (§1.43) |
| Fires, explosions | §5.7; explosions replayed at the owner's position |
| Street objects | breaks and resting places (§5.13, `objects.md`) |
| Medics, fire truck hose | §1.37 |
| Mines | the first machine whose mine goes off says where |
| Cheats | routed to where what they change is owned (§5.14) |
| Money | server setting: off, own, shared |
| Safehouse pedestrian door | Synced: all three on the gate mask (`protocol.md` §1.47) |

### Campaign

| What | How |
|---|---|
| Starting a mission | one at a time, everybody at the marker (§1.29) |
| The mission's script | runs on the owner's machine only |
| What it shows | replayed through each machine's own interpreter (`game/replay.h`) |
| Its peds and cars | hosted by the owner as mission entities |
| Cutscenes | loaded together, skipped by vote (§1.34) |
| Pass, fail, retry | the script's own end; a death fails it for everybody |
| Pay | everybody paid; charges are the owner's alone |
| Progress | the campaign delta, logged by the server (§1.30) |
| Late joiners | handed what the mission has up |
| Saving | each player saves at their own safehouse, outside missions; **In progress**: not tested well enough to call safe (§7) |

---

## 5. Decisions - settled

These change how the game *feels*, so they were decided deliberately rather
than left to fall out of the code. Most were decided on 2026-09-21, the rest
as the work reached them. They stay settled unless there is a good reason to
reopen one.

### 5.1 Wanted level - per-player, GTA Online style, server-configurable

Each player carries their own stars. The police pursue whoever is wanted, not
the group. Propagation follows GTA Online's rules, and the rule that matters
most is that sharing a vehicle with a wanted player shares the heat.

Server option `WantedLevel`:

| value | meaning |
|---|---|
| `perplayer` | **default.** Own stars; shared inside a shared vehicle. |
| `shared` | The whole session shares the highest wanted level. |
| `off` | No wanted level at all. |

**Built 2026-09-22. [docs/wanted.md](wanted.md) is the investigation, the
design and the measurements; it supersedes the paragraph that used to be here
and the table above still stands.** The two things this section had wrong:

- **The name.** The wanted level does not live in `CPlayerInfo`. It hangs off
  `CPlayerPed` at `+0x53C`, which is the end of `CPed` — so the constraint in
  §2.3 is right for a sharper reason than it gave: a `CCivilianPed` is
  *exactly* `sizeof(CPed)` bytes, and every remote player and every replica in
  CoopIII is one, so there is physically nowhere for a second wanted level and
  no engine code that would read one.
- **The cost, which was backwards.** `perplayer` is close to free and `shared`
  is the work. Each machine already runs its own `CWanted`, generates its own
  police from it, and attributes only its own player's crimes to it — the
  engine's `CEventList::RegisterEvent` reports a crime only when the criminal
  is `FindPlayerPed()`, so a replayed shot from another player cannot move
  your stars. And the police need nothing at all: a cop ped is a `RANDOM_CHAR`
  and a police car a `RANDOM_VEHICLE`, so [population.md](population.md) has
  been replicating both since it shipped. What is actually hard is `shared`,
  where a level held by more than one machine needs a rule for coming back
  down that no single machine can decide alone (`wanted.md` §4.5).

The police logic is **not** driven from CoopIII's state, and must not be:
`CCopPed` reaches for `FindPlayerPed()` nineteen times and has no "chase this
ped" path at all. CoopIII writes one number into the engine's own `CWanted`
and the engine does the rest.

### 5.2 Friendly fire - server-configurable, off by default

`FriendlyFire = false`. Groups that want it can turn it on.

### 5.3 Distant players - blip only, not rendered

Beyond the streaming radius a player is a map blip and nothing else: no ped, no
model request, no collision. It is the option that does not fight §2.1: no
tether, no multi-focus streaming, nothing thrashing the streamer's memory
budget.

The consequence to get right is the transition. A player crossing into your
radius must spawn smoothly rather than popping in mid-stride, and one leaving
must despawn without leaving a corpse behind. The handoff is where the work is.

**The marker half of this is done** (`client/src/game/radar.h`), and it is no
longer the part that needs planning.

It was a `BLIP_CHAR` in the game's own `CRadar::ms_RadarTrace` at first, which
made this section a list of things to arrange: a coord blip for players with no
ped, ownership without an entity handle to recognise it by, writing
`m_vec2DPos` by hand every frame because no engine setter moves a coord blip.
None of that is needed now. A remote player is drawn directly, as the rotating
arrow the engine draws the local player with, out of a detour on
`CRadar::DrawBlips` — and an arrow is a position and a heading, both of which
arrive on the wire whether or not there is a ped to hang them on.

So the streaming transition costs one branch, and it is already written:
`ResolveArrow` reads the live `CPed` (or the car it is sitting in) when there
is one and the last snapshot when there is not. `CRadar::LimitRadarPoint` pins
anyone past the radar's range to the rim and `CRadar::CalculateBlipAlpha` dims
them as they go, both the engine's own, so a player walking out of the
streaming radius loses their ped and their nametag and keeps their arrow,
without the arrow changing in any way as they cross.

What is left of §5.3 is the ped half: when to stop spawning, how to despawn
without leaving a corpse, and how to bring somebody back in mid-stride.

### 5.4 Mission failure on death - it fails, as in single player

If a player dies during a mission, the mission fails. Faithful to SP.

This reverses the provisional default in `campaign.md` §2.3, which assumed
co-op should keep going while someone is alive. The server option exists
(`missionFailOnDeath`), and its default matches SP.

### 5.5 Fidelity wins by default

Stated once so it stops being case-by-case drift: when single-player behaviour
and co-op convenience conflict, single-player behaviour wins. Server options
may relax it; the defaults do not.

§5.4 is that policy applied.

### 5.6 CoopIII only runs when launched through the launcher

Dropping `CoopIII.asi` into the game folder
must not change single-player. The mod activates only when the game was
started by `coopiii-launcher`. Started any other way, the `.asi` loads, logs
that it is standing down, and installs nothing.

The player keeps one install for both: the game launched normally for single
player, launched through the launcher for co-op. No files to move, and no
chance of the mod interfering with a solo campaign run.

Implementation: the launcher sets a marker in the child process's environment,
and the boot thread checks for it before doing anything. Environment blocks are
inherited by `CreateProcess` children, so this needs no IPC and cannot be
triggered accidentally.

Status: built. The launcher puts `COOPIII_LAUNCHED` in the child's
environment (`launcher/include/launcher/core.h`, `ENV_LAUNCHED`) and the boot
thread in `client/src/dllmain.cpp` stands down without it, having hooked
nothing, and says so in `CoopIII.log`.

---

### 5.7 Fire is world state, and it gets synced. Three phases.

Decided 2026-09-21, after a live run where a remote player held a flamethrower
and nothing at all came out of it on the other screen.

**Phase one, done: the flame has to appear.** A remote player pulling the
trigger on a flamethrower produces visible fire on every observer, and no
observer decides who burns.

The constraint that had the flamethrower on the refused list is real and has
not been waved away. `CWeapon::FireAreaEffect` hands the shot to `CShotInfo`,
whose slot lives for the weapon's `m_fLifespan` and keeps setting things
alight every frame until it expires, long after the call that created it
returned. A guard wrapped around the call never covered that.

What changed is that the guard is no longer the call. `CPed::InflictDamage`
now refuses anything a remote player's ped tries to take off the local
player's health, which holds for the whole life of the `CShotInfo` and every
`CFire` it lights, because `CFire::ProcessFire` passes its `m_pSource`
straight into `InflictDamage` and that source is the remote ped. No timer, no
engine flag held across frames. `CShotInfo::Update` itself only lights fires
and skips `bFireProof` peds, which every remote player already is.

The exception, and it is the same one §1.9.2 already made: an explosion may
still hurt the local player, because a blast is replayed at a fixed world
position everyone agrees on, so "was I standing in it" is a question about us
that we are entitled to answer.

**Phase two, done: fire burns, whatever lit it, and nobody decides it for
anybody else.**

The two questions phase two was gated on are both answered, against the
retail binary. They are recorded in full in `client/src/game/addresses.h`
under `---- fire ----`; the short version and what each one settled:

**1. The array is fixed, and it is 40 entries of 48 bytes.** Three separate
functions carry the bound and all three agree — `CFireManager::Update`
(`cmp ebp,28h` at `0x00479336`), `GetNextFreeFire` (`cmp eax,28h` at
`0x00479304`) and `FindFurthestFire_NeverMindFireMen` (`cmp ebx,28h` at
`0x004794C4`) — and all three step by `30h`. `gFireManager` is at
`0x008F31D0`, `m_nTotalFires` at `+0`, `m_aFires` at `+4`, so the whole thing
ends at `0x008F3954`. re3's `NUM_FIRES` also says 40; that is a coincidence
that was checked rather than a constant that was trusted, and given that the
last three bugs in this project were all an re3 number that did not match
retail, it is worth saying which of the two this is.

**2. Yes, a `CFire` can point at an entity — and the interesting part is what
it does with it.** `CFire::ProcessFire` (`0x004798D0`) opens on
`mov eax,[ebx+10h]`, and when `m_pEntity` is set it **rewrites `m_vecPos` from
that entity's matrix every single frame** before doing anything else. So an
entity fire's position is not state at all, it is derived; its life is tied to
the entity; the engine keeps a two-way link (`CPed::m_pFire` at `+0x4B4`,
`CVehicle::m_pCarFire` at `+0x1E4`) and asserts it every frame, extinguishing
a fire whose entity no longer points back; and it cannot exist on a machine
where that entity does not.

That answers "snapshot or event stream" with **neither, and the split is by
kind rather than by mechanism**:

- **A fire on the pavement** (`m_pEntity == nil`) is flat, ownerless state at
  a fixed position. It turns out to need nothing at all, which is the other
  finding: `CFireManager::StartFire(pos, size, propagation)` (`0x00479500`)
  has **exactly one caller in the whole image**, at `0x0055957E` inside
  `CExplosion::AddExplosion`. In retail 1.0, an explosion is the only thing
  that puts an unowned fire on the ground, and CoopIII already replays every
  player's explosion at a fixed world position every machine agrees on
  (`protocol.md` §1.9.3). Pavement fires are therefore already the same on
  every machine, for free, and a fire packet would have doubled them.
- **A fire on an entity** is a property of an entity that is already synced,
  so it belongs on that entity's own stream, not in a fire table snapshot.
  That is phase three.

And `CReplay`'s two `memcpy`s are not the precedent they looked like. Replay
is one process, so `m_pEntity` and `m_pSource` still point at something on the
way back in. Across the wire they are per-process pool pointers (§1.5), and
half the array is them.

**So who decides that a fire burned somebody? The victim, always.**
`protocol.md` §1.10.6 is the writeup. Fire damage never goes on the wire in
either direction — not because it was hard, but because a fire's authority is
a *place* rather than a *ray*, and "am I standing in it" is a question about
me that I answer from my own position this frame with nothing stale in it.
Most fires have no owner to send it from anyway, and `CFire::ProcessFire` hits
once per frame, so forwarding it would be sixty packets a second per burning
player.

The one predicate that had the flamethrower's fire refused now admits exactly
one cause, `WEAPONTYPE_FLAMETHROWER`, and it is narrow because the binary
makes it narrow: of the 21 `call CPed::InflictDamage` sites in the image,
exactly two push `9` and both are inside `CFire::ProcessFire`. That cause
cannot mean "a remote player shot me"; it can only mean "a fire is burning me
and it remembers who lit it". Nothing interpolated goes into the decision.

Friendly fire is decided by the fire's own `m_pSource`: null is terrain and
burns anyone, a remote player's ped is their fire and is gated like their
bullets. The molotov puddle is still terrain — `StartFire(pos, ...)` nils the
source — so "it burns whoever walks into it, friendly fire or not" still
holds for exactly the fire that sentence was written about.

No wire change. `PROTOCOL_VERSION` is untouched.

**Phase three, done: a burning player is visible to everyone.**

`PF_ON_FIRE`, one spare bit in the flags byte the snapshot was already
sending. No packet, no layout change, `PROTOCOL_VERSION` still **8**.
`docs/protocol.md` §1.10.7 is the writeup.

**The argument this phase was deferred for turned out not to exist, and the
engine is what says so.** Phase two's objection was real as far as it went:
for a ped that is not the local player, `CFireManager::StartFire`'s entity arm
calls `SetFlee`, `SetMoveState(PEDMOVE_SPRINT)`, `SetMoveAnim()` and
`SetPedState(PED_ON_FIRE)`, straight into the stream `ApplyRemotePose`
overwrites every frame. What phase two did not check is that all four sit
inside one branch, and the engine skips it:

```
00479640  mov [ebp+4B4h], esi     ped->m_pFire = fire
00479646  call 004A1150           FindPlayerPed()
0047964B  cmp ebp, eax
0047964F  je  00479890            -> jumps to 004796C7, past the whole NPC arm
```

So GTA III already has a way to set a ped alight with no burning-ped logic
attached, and it uses it for the one ped whose movement is not the engine's to
decide. A remote player is that ped on this machine. CoopIII takes the same
branch: `LightRemoteFire` in `client/src/game/ped.cpp` is StartFire's shared
tail transcribed in its own order, with the branch not taken — and nothing
else. The engine's NPC logic is never started, so there is nothing to suppress,
nothing to unwind, and no state for the two to fight over.

The other half is that a fire, once lit, writes nothing to the ped at all.
`CFire::ProcessFire` reads the ped's matrix, asserts the two-way link and
calls `InflictDamage`. It never touches `m_nPedState`, `m_nMoveState` or the
clump. **The NPC logic was the whole of the risk and it was all in one branch.**

**Nobody decides anybody's health.** The damage stayed exactly where phase two
put it. An observer's fire on a remote ped is refused twice: `bFireProof` is
the first and only thing `InflictDamage`'s cause-9 arm tests (`0x004EA898`),
and the detour refuses anything aimed at another player's ped before it asks
why. What the fire *is* allowed to do is spread to the local player, which is
single-player behaviour and is §1.10.6's rule already — the local machine
answering a question about itself from a fire in its own street.

**Why an observer saw nothing before, precisely.** Not a missing feature, a
working guard. `CWorld::SetPedsOnFire` tests `bFireProof` before lighting any
ped (`0x004B3D9A`), and every remote player is `bFireProof`, so the replayed
rocket that lights the victim on the victim's machine is refused on every
other. Which is why the flame has to be replicated deliberately or not at all.

**Two rules the reconciliation carries that are not tidiness:**

- **A remote ped's fire is ours or it is wrong.** `CShotInfo::Update` does not
  check `bFireProof` (`0x0055C1A8` gates on `IsPedInControl` and a distance),
  so a replayed flamethrower can still light a remote ped locally — with its
  own `SetFlee` before the call. A fire CoopIII did not light gets
  extinguished, which is also how the engine's own state gets unwound:
  `CFire::Extinguish` calls `CPed::RestorePreviousState`, which pops what
  `SetFlee` stored. CoopIII restores nothing by hand.
- **An observer's fire may not sit on a ped in a car.** `ProcessFire`'s ped
  arm writes `75.0f` into a burning ped's vehicle's `m_fHealth`
  (`0x00479959`) — that is how catching fire wrecks the car you get into, and
  on an observer it would be this machine deciding the health of somebody
  else's car. A burning player who gets in a car stops burning here.

`CPed::IsPedInControl` (`0x004CE6C0`) is the gate for both starting and
keeping, which is what makes this a loop like `UpdateRemoteSeats` rather than
an event handler. `tools/ghost -burn` claims to be alight for four seconds out
of every eight, which is the only way to exercise it without two games and a
rocket launcher.

Script fires (`CFireManager::StartScriptFire`, `0x00479E60`) are the other
loose end and they belong to Area D: only the host runs the script, so a
script fire exists on one machine today. It is the one fire kind with an owner
and no explosion behind it.

### 5.8 A car nobody is driving has nobody to report it. Named work.

**Closed 2026-09-22**, and not quite in the shape this section proposed below:
the three kinds of car nobody drives - a map generator's, a traffic car, a
session car somebody parked - each report their wreck on
`C_UnownedBlowUp` / `S_UnownedBlowUp` (0x38/0x39) under a key the map or the
session already agreed on, and a joiner is handed the map cars that are
already wrecks (`protocol.md` §1.11, §1.20.4; the Vehicle table's "Destroyed"
row). A wrecked session car is still left out of the backfill, as §2.8.5 of
`protocol.md` says. The text
from here on is the section as it was written.

Found while making the join path late-joiner safe (`protocol.md` §2.8) and
**not fixed then**, because the half that matters lives in the vehicle code
(`client/src/game/vehicle.*`).

`C_VEHICLE_STATE` is sent by one machine and one only: the driver's. So a
synced car that is parked receives no updates at all, and its row in the
session freezes at whatever its last driver said. Everything the backfill now
carries about a car's condition — health, engine, siren, `VEH_WRECKED` — can
therefore only ever arrive **from inside it**.

Which leaves the commonest way a car is destroyed with no carrier: you blow up
a parked one. Nobody is in it, nobody reports it, the session keeps a healthy
row for it forever, and every joiner from then on is handed a pristine car
standing where a burnt-out shell is on every other screen. That is very
probably the exact path the car in the first report took.

It is deliberately open rather than half-built, for two reasons:

1. **The detection does not exist yet.** Whether the engine can be asked "is
   this car destroyed" — and what it actually does when `m_fHealth` hits zero,
   since writing the field does not destroy the car — is live work in
   `client/src/game/vehicle.*`. Building a wire path on top of a detector
   nobody has written would be a protocol bump that carries nothing.
2. **The authority question has an answer already and it should be reused.**
   An ownerless world entity is the host's, exactly as the clock and the sky
   became the host's in §2.7. The host reports the destruction of a synced car
   that the session records no driver for; `Session::DestroyVehicle` is
   already the single place that lands in, and `MayReportVehicle` is already
   the single gate that would have to make an exception for it. Opcodes 0x60
   to 0x6F are free.

The shape to build, when the detector lands: one reliable
`C_VEHICLE_DESTROYED`/`S_VEHICLE_DESTROYED` pair carrying a `netId`, accepted
from the driver or, for a car with no driver, from the host and nobody else.
Not a new field on the snapshot — a parked car has no snapshot to put it on,
which is the whole problem.

### 5.8.1 Who owns a car, and what changing hands means. Decided 2026-09-22.

§5.8 above says who may *report* a car. This says who may *simulate* one, and
it is the rule the two bugs from the 2026-09-22 session both broke.

**The rule.**

> A session car has exactly one owner at a time: the player in seat 0. The
> owner simulates it and streams `C_VehicleState`; every other machine
> corrects it and writes nothing that makes its own engine act on it.
> Ownership moves only through the reliable, ordered `C_EnterVehicle` /
> `C_ExitVehicle` pair. **Between an exit and the next enter the car has no
> owner, and a car with no owner is simulated by nobody** — every machine
> holds it at the last transform the session gave, with its controls and
> velocities at rest.

Three things follow, and none of them needed a wire change: the server already
implements its half (`Session::NoteEnterVehicle` moves the driver out of the
old car first, `MayReportVehicle` refuses a snapshot from anyone but the
driver, and a claim that names a netId the session already has re-claims it
rather than allocating a second one).

1. **Entering a session car as driver claims it.** Already true, through
   `Client::ObservedVehicleWeAreDriving`, and it matched on the engine's own
   pool reference so it cannot pick the wrong one of two identical parked
   cars.

2. **The claimer keeps a row for the car it claimed.** This was the hole. The
   server broadcasts `S_VehicleSpawn` to everyone *except* the claimer —
   correctly; it is a car from the claimer's own world and they already have
   it — so the claimer was the one machine in the session with no record of
   that car. Step out, walk round, get back in, and nothing recognises it:
   the claim path registers the same physical `CVehicle` under a **second
   netId**, and every observer then holds two cars for it, one following the
   driver and one frozen. In the session log that is vehicle 364 and vehicle
   475, same model 104, same extras 3/-1, no despawn between them.

   The row is marked `RemoteVehicle::ours`, which is the whole difference
   between it and an observed car: it is never spawned (the car is already
   there) and never despawned (a car this engine made is not CoopIII's to
   delete — running `DespawnRemoteVehicle` on it would destroy one of the
   player's own traffic cars, quite possibly with the player in it, because a
   socket closed).

3. **The previous owner stops simulating it, and so does everyone else.** The
   owner's own machine stops sending on exit; that was already true. What was
   not is the other end: every observer went on replaying the last driver's
   snapshot at the car — velocity, steering, throttle, engine — every frame
   for the rest of the session. `RemoteVehicle::last` is frozen at whatever
   snapshot happened to arrive last before its owner stepped out, and
   `C_VehicleState` is unreliable and unordered, so that is not even reliably
   the last one sent.

   **That is what makes a used car impossible to get back into.**
   `CVehicle::CanPedEnterCar` (`0x005522F0`, disassembled into
   `addresses.h`) refuses any car whose `m_vecMoveSpeed` or `m_vecTurnSpeed`
   has a magnitude-squared over `0.04`, and `CPed::SeekCar` (`0x004D3F90`)
   answers that refusal with `CPed::RestorePreviousState` while leaving
   `m_objective` at `ENTER_CAR_AS_DRIVER`. There is no timeout in it: the
   next frame walks the ped back to the same door, and the player walks at
   the car forever.

   So a driverless car gets `WorldBridge::RestRemoteVehicle` instead of the
   controls half of `ApplyRemoteVehicle`. Health, damage and `VEH_WRECKED`
   still flow — a joiner has to be shown the shot-up car somebody parked —
   because those belong to the session whoever is or is not driving. What
   takes a driver to mean anything is the throttle and the velocity.

   Rested every frame it is parked, not once on the transition: the transform
   is pinned after physics every frame, so a car held a hair off the ground
   never lands and the engine would keep adding gravity to
   `m_vecMoveSpeed.z`. One frame of gravity cannot reach `0.04`; twenty can.

**What this does not do.** It does not touch the ambient population's cars
(`population.md` §1.3.1 is explicit that traffic replication promises no
handoff). `CorrectAmbientCarReplica` still only *guards* — it stops correcting
a replica the local player is driving without claiming it, so the session goes
on telling everybody else where its owner thinks it is. Getting into somebody
else's *traffic* car is still not an ownership change, and it should be: that
is the next piece, and it is the same rule applied to a second roster.

### 5.8.2 Both of those are built. Done 2026-09-23.

`protocol.md` §1.20 and §1.21 are the design. What changed against §5.8.1 above
is one sentence of it, and it is the sentence that turned out to be the bug.

> *"a car with no owner is simulated by nobody — every machine holds it at the
> last transform the session gave"*

That is right for a car standing in the street and wrong for one that was still
moving when the session stopped having a driver for it, because the hold is
applied **after** physics, every frame. `CVehicle::CanPedEnterCar` refuses a car
whose `up.z` is *inside* ±0.1 — on its side, which is the pose a rolling car
ends up in — and `CPed::SeekCar` answers that with no timeout. So the car is
pinned on its side on every machine and nobody can ever get in again.

A driverless car now gets a **custodian**: one machine, named by the server on
the `S_ExitVehicle`'s own reliable ordered channel and immediately after it,
that stops correcting the car and lets its own engine finish. It streams
`C_VehicleState` for it, every observer follows, and when the car comes to rest
it says so and the session goes back to nobody simulating it — i.e. back to the
rule above, which is where the bandwidth and the stillness come from. **Custody
is the exception; rest is the rule, and a car with no custodian takes exactly
the path it took before.**

**Granted to the player who was driving, not to the host, and §5.8's answer is
still right for what §5.8 is about.** The host is the right authority for a
*fact* about an ownerless entity — it is one machine and it is always there.
It is the wrong machine to run a car's physics on, because §2.1 and §2.2 above
mean a host across the river has neither the car streamed in nor the collision
under it. Custody is therefore short (2 s) and goes to the machine that was
touching the car a frame ago.

That machine is also whoever drives into the car afterwards. A car nobody holds
used to be a wall on every screen, since every machine pinned it after every
frame of physics; now the machine whose car shoves it asks for it the way a
shot does and settles it for as long as the shove lasts (`VEHICLE_HIT_PUSH`,
protocol.md §1.21.5, 2026-09-24). A shove by somebody else's traffic or by a
player on foot still meets the pin. Not run in game.

Traffic is the same rule applied to the second roster, and the answer to "the
same claim or something narrower" is **the same claim**: an `AmbientCar` has an
owner and no seats, so a player at its wheel is invisible to it and every
observer draws them in the road. The claim is `C_EnterVehicle` with the netId
the session already has (netIds are one space, so it is unambiguous), the row
is promoted **under the same netId**, and no car is created or destroyed on any
machine — including the one whose own engine made it, which keeps its `CVehicle`
and merely stops being allowed to report it.

§5.8's own subject — a *parked* car nobody has ever claimed being blown up —
is still open and is still the host's. Nothing here changes that; what it adds
is that a car being settled has an owner, so its destruction travels on the
ordinary vehicle blast path for the length of the settle.

### 5.9 A car's extra components are picked per machine. Named work.

**Done**: `EnterVehicleBody` and `S_VehicleSpawn` carry `m_aExtras`, forced on
the receiving machine through `CVehicleModelInfo::ms_compsToUse` before the
constructor runs (`protocol.md` §1.12). One thing below turned out wrong: the
extras cannot be written after construction, because they are cloned into the
clump inside it. The rest is the section as it was written.

`CVehicle::SetModelIndex` (`0x00551170`) copies
`CVehicleModelInfo::ms_compsUsed` into `m_aExtras[2]` at construction, and the
model info picks those at random. So every machine that spawns a given car
chooses its own set: one player sees a Mule with a roof rack, another sees the
same Mule without one.

**This is not a late-joiner gap** and that is why it is here rather than in
the join work. Two players who connected together already disagree, because
each of their engines picked independently the moment the car was created. It
is the same class as the paint job, which *is* carried — the colours went onto
the spawn packet for exactly this reason.

Cheap when somebody wants it. `m_aExtras` is two bytes sitting immediately
after `m_currentColour2` in `CVehicle` (the `static_assert` in `addresses.h`
pins all four as consecutive), `S_VehicleSpawn` has room, and
`EnterVehicleBody` already reserves a spare `pad` byte in precisely that slot
next to the colours. Both halves are in `client/src/game/vehicle.*`: the
claimer samples them beside the colours, the receiver writes them after
construction and before the model is set up.

---
### 5.10 A rampage is shared - one at a time, for the whole session

Decided 2026-09-22. Collecting a `KILLFRENZY` pickup starts the frenzy for
everybody, everybody's kills count toward it, and passing or failing it does so
for the whole session.

**The engine gives no real choice.** A rampage is `CDarkel`, and `CDarkel` is a
singleton with one kill count, one timer and one HUD counter. Retail's own
`CanBePickedUp` already refuses a second killfrenzy pickup while one is running
(`CDarkel::FrenzyOnGoing`, `0x00420E60`). Per-player rampages would mean
CoopIII owning the frenzy state, the target, the clock and the HUD for every
player - that is not a pickup feature, it is a reimplementation of `CDarkel`,
and it belongs nowhere near M4.

**It is also the better game.** "Kill 20 Diablos in two minutes" is the most
obviously co-operative thing in GTA III. Eight private rampages happening in
the same street is worse in every way.

**And it costs nothing to build**, which is the part worth knowing: every
machine runs `rampage.sc`, and that script asks `HAS_PICKUP_BEEN_COLLECTED`.
Pushing a remote collection into each machine's own `aPickUpsCollected` ring
makes every machine's own script start the same frenzy in the same frame, with
its own HUD, its own timer and its own failure condition. No packet for any of
it.

**The trade-off, plainly:** the difficulty is not rebalanced, so a four-player
rampage is trivial. Accepted for M4. If it matters later the fix is a
server-side multiplier on the kill target - which needs the script intercepted,
i.e. M5 - and **not** a per-player split.

> **Corrected 2026-09-23.** The multiplier does *not* need the script
> intercepted. `CDarkel::StartFrenzy` is `0x004210E0`, it takes the kill target
> as its third argument, and it has exactly two callers in the whole image -
> both of them the script opcodes `01F9` and `0367`. So the multiplier is a
> detour on one function and no script work at all, and it was built in the
> rampage round as server option `rampages = shared | scaled | off`, default
> `shared`, which is this section unchanged. `rampage.md` §4.
>
> The rest of §5.10 survived contact with the binary as written. What it did
> not know was that a co-op NPC kill counted for *nobody* rather than for the
> killer: the credit test at `0x004EAD1A` accepts only `FindPlayerPed()` and
> `FindPlayerVehicle()`, and in a session neither of them is the shooter.

### 5.11 A hidden package collected by one player counts for everybody

Decided 2026-09-22. Server option `hiddenPackages = shared | perplayer`,
default **`shared`**. `perplayer` was named in M4 and built 2026-09-23 on the
server alone (`Session::PickupIsPerPlayer`; `pickups.md` §6): each player's
claim on a package is weighed against their own record, nobody else is told,
and each machine's own count is its own player's. Not run in game.

- **The engine has one counter.** `m_nCollectedPackages` is `CPlayerInfo+0xB4`
  and there is exactly one `CPlayerInfo` (§2.3). Remote players are `CPed`s.
  Per-player packages would be CoopIII's own state, and `rewards.sc` - which
  polls the count to unlock the weapons at the hideout - would then be reading
  a number that means something different on every machine.
- **The array cannot hold the alternative.** 100 packages per player against
  320 general pickup slots is three players, and the object pool pays for every
  one of them.
- **§5.5, fidelity wins.** The single-player experience of hidden packages is
  that the map empties as you clear it. A shared world where seven players walk
  past a package only the eighth can see is the version that feels wrong.
- **Same free mechanism as §5.10.** The observer increments its own counter and
  pushes the collection into its own `aPickUpsCollected`, and its own
  `rewards.sc` produces the reward weapons by itself. The "34 of 100" message
  and the million at 100 turned out to be the engine's, not the script's, and
  the observer now applies them too (`pickups.md` §13.1).
- **Every save gets the group's packages, whatever it started with.** A
  package somebody's save had before the session is worked out on the server
  from the packages still lying in each machine's world and sent to everybody
  else like one collected in the session, and kept in the progress file
  (2026-09-29, `pickups.md` §13.2, `protocol.md` §1.50). So after the first
  report each, both games hold the union, and each save written afterwards
  has it. Not run in game.

**The trade-off, plainly:** eight players finish the packages in an eighth of
the time, and the 100% grind - one of the longest solo activities in III -
collapses. That is the right price for a shared map, and groups who disagree
get the option.

### 5.12 Ammunition is reported honestly - server-configurable, off by default

Decided 2026-09-22. Server option `ammoSync = true | false`, default
**`false`**. `docs/protocol.md` §1.9.6 is the wire and the engine argument;
this is the decision.

`false` is what CoopIII has always done: a remote player's gun is handed a
fixed thousand rounds at spawn and nobody ever watches anybody else run dry.
`true` puts each player's real count for their own weapons on the wire, so a
firefight has the same numbers on every screen.

- **It is not a shared inventory, and that is the point.** Two players
  carrying different weapons is the normal case and stays that way. The option
  decides whether *your own* counts are honest on *other people's* screens,
  not what anybody is carrying.
- **Off by default on §5.5 grounds, inverted.** Honest ammunition is the
  higher-fidelity answer, but it is also the one that can be *wrong* - it puts
  a number on the wire that the observer's own engine is simultaneously
  editing, and §1.9.6 is a page about keeping those two from fighting. A
  default that cannot misbehave is worth more than a default that is more
  faithful when it works.
- **Same shape as §5.2.** One bit in `S_Welcome.flags`, enforced by the server
  refusing to relay and by the client refusing to apply.

### 5.13 A broken street object is a latch, and the measurement took most of the job away

Decided 2026-09-22. [docs/objects.md](objects.md) is the investigation; this is
what was settled and why it is smaller than it looked.

**Breaking is a state, not a destroy-and-replace.** `CObject::ObjectDamage`
(`0x004BB240`) frees nothing, allocates nothing and touches no world list -
every one of its nine arms writes flags on the object that is already standing
there. So the whole feature is a one-way latch: two players breaking the same
crate is not a conflict, applying a break twice is a no-op, and there is no
creation, deletion or ownership handshake in it anywhere. That is the reason
this is a much smaller thing than an ambient ped or a car, and it was not
obvious going in - the intuitive guess is that the engine swaps in a broken
object, and it does not.

**Two measurements struck most of the planned work, which is now four times.**

- **An explosion already agrees.** The object arm of
  `CWorld::TriggerExplosionSectorList` computes its damage as
  `300 * min((radius - distance) * 2 / radius, 1)` - two positions and a
  radius, no RNG, no impulse, no timestep - and CoopIII already replays every
  explosion at a position every machine agrees on. Objects blown up by a blast
  therefore break identically everywhere for free. This is §5.7's pavement-fire
  finding again, reached the same way. The seam goes deliberately quiet inside
  `CWorld::TriggerExplosion` (two callers in the whole image) rather than
  sending one reliable packet per bin per rocket.
- **A bullet has never broken one.** The object arm shared by
  `CWeapon::DoBulletImpact`, `FireShotgun` and `FireMelee` adds sparks, clears
  `bIsStatic` and applies a force, and stops. A whole-image scan for calls to
  `ObjectDamage` finds five sites and **none of them is in `CWeapon`**. The
  first guess was "a car driving into them and gunfire"; the binary says
  gunfire was never in it.
- **And a bullet cannot uproot one either.** That arm's uproot gate is
  `GetIsStatic() && m_fUprootLimit <= 0.0f` - the three constants it compares
  against all read `00000000`, so it is a sign test and not a threshold - and
  `data/object.dat` gives lamp posts 400, traffic lights 500, barriers 350,
  meters and bins 100, cones 10 and benches 5. Every one is above zero, and
  because the object is still static the move force behind `!GetIsStatic()` is
  skipped too. **Shooting a lamp post in retail 1.0 produces eight sparks and
  a sound and moves nothing.** The only breakable models a bullet knocks loose
  are the zero-limit ones - crates, wooden boxes, wastebins, pallets,
  newspaper machines - and those it knocks loose on every machine, because
  §1.9.2 replays the shot through the engine.

So the entire remaining feature is *somebody drove into it*, and that is what
was built.

**Named by where the map put it**: `m_objectMatrix`'s position plus the model
index, the pickup's answer rather than the parked car's or the ambient ped's.
A map object has no generator index to borrow and needs no server-allocated
name, because the map already placed 1851 identical copies before a packet was
sent. The equivalent of `pickups.md`'s 312-coordinate check was done: all 1851
breakable map instances compared pairwise inside each model, closest pair
0.5992 m, nothing under 0.50 m. `tools/objecttest` carries that number so the
tolerance cannot quietly grow into it.

**§5.8's answer had to move one entity across, and the binary is what says
so.** An ownerless world entity is the host's - except that
`CPopulation::ManagePopulation` turns any map object more than 80 m from the
local player back into a pristine dummy, so an object across town from the host
is not a `CObject` on the host at all and the host has nothing to observe.
§5.8 works for a parked car because the *server* holds a row for it whatever
the distance; here there is no row and there must not be one. What transplants
is the shape: exactly one reporter, chosen by ownership - **the machine that
owns whatever broke it**, and the host for whatever nobody owns.

That same 80 m rule is why there is no server table and no backfill. The engine
throws the state away when the last player leaves the block, so a broken object
has a lifetime of one visit and nothing a joiner could be told would still be
true by the time they finished loading. It is also why divergence is
self-limiting: once everybody walks away, both machines are pristine again.

**The open half, now closed.** Uprooting - a lamp post falling over - really is
a different mechanism from breaking, and `CObject::ObjectDamage` is what proves
it: none of its nine arms clears `bIsStatic` and the smash arm *sets* it. What
makes an object fall over is `bIsStatic` being cleared and the object being
handed to `CPhysical::AddToMovingList`, decided in three places that all read
`m_fUprootLimit` - a collision (`impulse > limit`), a blast (`power > limit`)
and a bullet, a pellet or a bat (`limit <= 0`). A lamp post's limit is 400 and
its break threshold is 150, so the same car bends it at 200 and knocks it down
at 500: two decisions off one number, which is why one travelling never implied
the other.

Two of the three already agreed. A blast's power is the same pure function of
two positions and a radius that its damage is. A bullet's uproot rides §1.9.2's
shot replay - every observer fires the remote ped's own `CWeapon` through the
engine, out of the wire's muzzle and along the wire's direction, so their own
`DoBulletImpact` runs the same object arm - and in any case `object.dat` gives
every lamp post an uproot limit of 400 against a gate of `<= 0`, so **a bullet
cannot knock one over on anybody's screen, including the shooter's**. What
nobody else ran was somebody else's *collision*.

So what travels is the resting place and nothing else: one packet per
uprooting (`C_/S_ObjectSettled`, `0xC2`/`0xC3`), sent when
`CPhysical::ProcessControl`'s own sleep test - ten quiet frames, then
`SetIsStatic(true)` - says the object has stopped. Not an impulse: §2.4 means
two machines handed the identical impulse put the post down in two different
places. The receiver writes the matrix through the engine's own
`CMatrix::UpdateRW`, `CEntity::UpdateRwFrame` and `CPhysical::RemoveAndAdd`,
and setting `bIsStatic` is all the moving list needs, because `CWorld::Process`
unlinks a static entity itself on its next pass. Opcodes `0xC4`-`0xCF` stay
reserved. `docs/objects.md` §8 is the whole argument.

### 5.14 Cheats run where what they change is owned - server-configurable, all on by default

Decided 2026-09-23. Server option `cheats = shared | personal | off`, default
**`shared`**. [docs/cheats.md](cheats.md) is the investigation, the table of
all 23 cheats in retail 1.0 and the argument for each; this is the decision.

- **`shared`**: every cheat works, the way it does in single player (§5.5).
  The thirteen about the player who typed them - weapons, money, health,
  armour, stars, the skin, the tank, the three handling toggles, NASTYLIMBS -
  run on their machine and nowhere else, because what they change already
  travels. The four weather cheats go to the host, whose sky is the session's
  (§2.7). TIMEFLIESWHENYOU, BOOOOORING, MADWEATHER, ITSALLGOINGMAAAD and
  WEAPONSFORALL run on every machine, because each machine's own clock and
  crowd is its own and nobody can apply them for anybody else. BANGBANGBANG
  runs where it was typed; the BlowUpCar detour already refuses every car
  somebody else owns, and the wrecks it is allowed travel.
- **`personal`**: only the thirteen. The ten that change the world are refused
  with a log line that names the setting.
- **`off`**: none while connected.
- **Why a switch at all.** A riot, armed pedestrians or a slowed clock that one
  player types lands on everybody, and that is exactly the kind of thing
  players can disagree about. Same shape as §5.1 and §5.10: two bits in
  `S_Welcome.flags`, enforced by the server refusing to relay and by the
  client refusing to run or apply.
- **What is not routed, on purpose.** BANGBANGBANG could have been sent to
  everybody so every car in the city goes up. It is not: that would blow up
  other players' cars with them inside, which is what friendly fire off
  (§5.2) exists to prevent.
- **CoopIII's own cheats** (decided 2026-09-30, [cheats.md](cheats.md) §7).
  Typed the same way, matched against the same buffer. The first is
  TPTO1..TPTO8: beside the player with that number on the Tab list, in your
  car if you drive one. Their own switch, `coopCheats = outsidemissions |
  always | off`, default **`outsidemissions`**, because a player who jumps
  across the map in the middle of a mission's script is one the script may
  not expect. It travels in `S_SessionRules` (protocol.md §1.61), and each
  client keeps it: the move is the player's own position, which the server
  has no business refusing.

## 6. Rules that keep paying off

Learned the hard way this far in; worth not relearning.

- **Never resolve a rel32 by hand.** Use `tools/calltarget`. Hand arithmetic
  produced two wrong addresses in one sitting; one crashed the game, and it
  crashed *in a completely unrelated place*, minutes later.
- **A guess and a proof must not look alike.** Verified addresses live in
  `addresses.h` with a note on how they were proved; everything else lives in
  `addresses-unverified.md`. Two symbols have already been refuted.
- **Do what the engine does.** The ped spawn path works because it is the
  game's own `CREATE_CHAR` sequence, found by walking the opcode dispatcher,
  not a hand-rolled pool allocation.
- **Fail loudly.** With this many mods patching the same binary, "CoopIII
  loaded and quietly did nothing" is the failure mode to design against.
- **Test what can be tested without the game.** The suites cover the
  protocol, the session, the server, interpolation, patterns, hooks, the
  launcher, the installer and the UI, and `clienttest` checks the addresses
  against a real `gta3.exe` when it is given one. What is left needing a live
  game is then small enough to reason about.

---

## 7. Open work and known issues

### In progress: playing it through

- **The story, mission by mission.** The mission sync passes its tests and
  has been played in short sessions (Give Me Liberty, failed and retried, is
  where the retry fixes came from). The rest of the campaign has not been
  played through in order yet. That is the current work: play each mission
  with two or more players and fix what it shows up (`missions.md` §14, §15).
- **Three or four players in a mission.** Checkpoints counting down, a third
  player joining mid-mission, a helper dying with `missionFailOnDeath = off`,
  the owner's connection dropping and coming back, and four cars moved by one
  teleport without touching. All built, all needing a real session.
- **Cheats in a game with SilentPatch.** SilentPatch III rewrites two rows of
  `CPad::AddToPCCheatString` (TORTOISE, BOOOOORING's length). The cheat
  detour used to refuse to install there, so none of the routing ran. It now
  accepts those two rewrites (and refuses any other) and compares by the
  table the running code has (`cheats.md` §8). Built and tested against both
  byte shapes, not yet run in a session with SilentPatch: the sky to the
  host, the clock and riot for everybody need trying there. TPTO itself is
  built and tested, not yet run in a game.
- **A player's car taken by a mission.** The owner's mission teleporting the
  car the owner rides in (Taking Out The Laundry, started from a guest in
  the marker while the owner sat in the guest's car) announced that car as a
  new mission car, and the guest saw his car twice. A session car, a copy of
  somebody's traffic, a remote player and a copy of somebody's pedestrian
  are never the mission's now, and a mission car standing on a session car
  of the same model is never built (`missions.md` §15, "What the mission
  takes that is already there"). Tested, not run in-game yet.
- **Missions a helper drives.** A helper driving Lips' car to 8-Ball's,
  delivering to a lock-up, taking a car through a Pay'n'Spray, or driving the
  owner in a car the mission moves or brakes. Until a game has shown these,
  the owner at the wheel is the safe way (`mission-audit.md` C2).
- **What the engine's mission machines and stealth checks do for helpers.**
  The Exchange's Catalina helicopter is replayed to every participant and a
  participant's copy going down answers the owner's check; Deal Steal's and
  Plaster Blaster's stealth checks are answered for every participant; the
  power pills are drawn on everybody's screen and collected by the owner's
  car only; the crusher crane is worked by the machine holding the car and
  followed by everybody else (`mission-audit.md` R7, R13). Built, not run
  in-game. The RC buggy is checked and not seen.
- **Free roam features that are built and tested but have seen little of a
  real session**: car damage between players, the car radio, bombs and mines,
  medics and fire trucks, passenger free aim, the scripted gates, the traffic
  hand-over near another player (§1.45). The same for three money and stats
  fixes: a shared rampage's reward is taken back by what the script paid, so
  other money in the same frame still reaches a shared wallet; the $250 for
  a police helicopter somebody else shot down always leaves its owner and
  reaches the shooter under `money = own`; and a teammate's rampage kill is
  on his stats screen once (`protocol.md` §1.52).

### Known issues

- **The cranes** (`protocol.md` §1.62). Built and not run in-game: the
  Portland crusher crane lifting a car parked by either player, the hook moving
  on every screen, and Dead Skunk In The Trunk and The Crook passing when a
  helper delivers the car. The crusher's own jaws close on the crushing
  machine only; elsewhere the car is lowered into the open crusher and goes
  when the removal arrives. A mission owner who never saw the car go up (his
  copy missing at that moment) hears it gone as dead and fails rather than
  softlocking.
- **Alt-tab.** Retail stops its loop only when the D3D device is lost (a
  fullscreen alt-tab); the windowed-mode plugin's autoPause opens the menu
  instead, which the pause policy already runs through. In a session the lost
  device now runs Idle's no-render frame in place of `WaitMessage`
  (`game/pause.h`, `protocol.md` §1.47), and the player shows `AWAY`. Not
  run in-game yet.
- **Saving in co-op** is not tested well enough to call safe. Session copies
  are kept out of `CPools::SaveVehiclePool`, the session's weather pin is
  kept out of `GenericSave`'s `ForcedWeatherType`, and under `money = shared`
  the save holds the player's own cash rather than the wallet, which leaving
  the session also puts back in his pocket (`protocol.md` §1.51, not run
  in-game yet). A co-op save still has to be shown to load clean in single
  player. One thing still goes into one by re3's reading, not yet checked
  against the binary: a scripted gate another player is holding open is a
  script object, which `CPools::SaveObjectPool` writes where it stands, so it
  loads open until the loading game's own GATES thread next shuts it. (A
  safehouse door held open is saved open too, and harmless: the save thread
  opens it again the moment the game loads at that safehouse.)
- **The instant replay** is taken for the game starting over when it ends,
  because the frame counter it puts back is the witness for that
  (`addresses-unverified.md`, REFUTED).
- **Ramming a car another machine simulates.** The hit now goes to its owner
  (`protocol.md` §1.71): what our car's collision did to the copy's speed and
  spin, and the impulse for the owner's own dents, unless the owner's engine
  had the same collision. Our copy is left to our engine for a moment after
  the hit and blended back onto the owner's stream, instead of being put back
  every frame. A session car nobody holds is still taken over by the shove
  (§1.21.5). Only a car a player drives sends a bump: traffic hitting a
  player's car is still each machine's own. Covered by the suites; not run
  in-game yet - the window, the blend and the 250 ms hold over a real
  connection are unmeasured - and neither is the run-over hold
  (`protocol.md` §1.59.1).
- **The crowd vanishing or appearing in front of a player** when the machine
  hosting it drives off, dies, goes far or leaves (`protocol.md` §1.73,
  `population.md` §8). The hand-overs of §1.45 and §1.58 still come first;
  what nobody can take now fades out on a screen it stands on and goes at
  once off it, every copy fades in when built, a fresh one made inside our
  view at close range waits until we look away, and every machine's
  generators keep out of every player's camera, which is on the wire four
  times a second. Covered by the suites and checked against the exe; not run
  in-game. Left over: a pedestrian the engine re-files (`CPed::Teleport`,
  `WarpPedIntoCar`) is still a despawn and a new one elsewhere, a cop or gang
  member let go of still is not taken over (it fades), and a claim or a
  script's clear still takes a copy at once.
- **Shattered glass** (`protocol.md` §1.74, `objects.md` §10). A shop window
  shattered on one machine, by a car, a round or a blast, now shatters on every
  other with the engine's own panes and sound, and a joiner or a player who
  comes back while somebody is still near sees it gone. A round somebody else
  fired leaves the one in four that shatters a cracked window to the shooter's
  roll. Covered by the suites and checked against the exe; not run in-game.
  Left over: a crack alone (the first round, a soft knock) is not sent, so a
  window cracked by a collision only one engine ran is whole on the other
  screen until it shatters. Car glass is the windscreen, already a synced
  panel.
- **A co-op kill on a pedestrian another machine hosts** reaches the
  shooter's stats screen only when it counts toward a running rampage
  (`protocol.md` §1.52). Any other such kill is the host's
  `PeopleKilledByOthers` and nobody's own (`rampage.md` §7).
- **A guest's save ahead of the host's** no longer takes the deltas of story
  missions it had passed (`missions.md` §5.8, the story's latches). Still
  applied as before, and able to put a guest's own state back: a failed
  mission's globals, and deltas the server kept from a build before the
  latches. The side jobs no longer do (2026-10-01, `mission-audit.md` §3,
  "Side jobs, step by step"): their counts and records only go forward, and
  a progress point counts only where its reward flag goes up, except the
  Paramedic's last level, which has no flag. Not seen in a game.
- **The safehouses' pedestrian doors**: all three are synced in the gate
  mask, Portland's on bit 7 and Staunton's and Shoreside's on bits 8 and 9
  (`protocol.md` §1.47). Not run in-game.
- **A hidden package's money under `money = shared`.** The wallet takes a
  package's $1000 (and the million at the hundredth) once, from the
  collector's own engine; every other machine puts it on its player's own
  cash alone, so each save has the package money beside the package
  (`protocol.md` §1.51). Covered by `clienttest`; not run in-game. The
  million reaches the wallet only from a collector whose own count hits 100,
  so it relies on the counts being aligned (`pickups.md` §13.2).
- **Hidden packages across saves** (`pickups.md` §13) are worked out from
  each machine's report and not seen in a game yet: two saves of different
  ages, both saved after the session and loaded in single player, is the
  test.
- **Players on different islands** in free roam: only one island's collision
  is in memory (§2.2). A player on another island is now an arrow and a
  nametag hint with no ped (`protocol.md` §1.47). Not run in-game.

### Planned

- **A CLEO/Sanny Builder SDK for new co-op content**: `COOP_*` opcodes
  registered through III.CLEO, for races, co-op rampages and new missions,
  without touching `main.scm` (`missions.md` §4.3).
- **Per-mission overrides** for the few stock missions that still don't play
  well once the rest is done, through the same toolchain.
- The alt-tab and safehouse door fixes above, once they have been looked at
  in a game.
