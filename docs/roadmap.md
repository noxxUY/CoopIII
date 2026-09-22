# Roadmap

The goal: the GTA III campaign, played co-op, with the game otherwise 1:1
faithful. This document is the path from where we are to that, and the list of
things that have to be true along the way.

Read `protocol.md` for the wire contract, `campaign.md` for the mission design
and `compat.md` for the constraints the player's other mods impose. This file
is the *plan*; those are the *decisions*.

---

## 1. Where we actually are

Verified running inside retail GTA III v1.0 on the target machine:

| | |
|---|---|
| Image guard | Refuses to load against any other build. ✅ |
| Frame hook | `CGame::Process` detoured; verified by reading the `E9` in live memory. ✅ |
| Socket thread + queues | Connected, 2 ms RTT on loopback, never touches game memory. ✅ |
| Roster | Join/leave/backfill, self-echo rejection, two-phase spawn. ✅ |
| Local player sampling | Position, heading, velocity, health, armour, ped state and move state, read from real memory every 25 Hz tick. ✅ Animation (base + partial, with phase), held weapon and aim yaw/pitch were added for M1 and are sampled from verified offsets, but have not been through a live session yet. |
| Remote ped creation | Created via the engine's own `CREATE_CHAR` path. Survived 18 000 frames. ✅ |
| Remote ped **rendering** | ✅ Fixed. Remote players are now visible in-game. |

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
This needs a decision before milestone 2, because vehicles make players separate
much faster than legs do.

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
everyone is usually on the same island, which saves us. Free roam has no such
guarantee, though, and the bridge sequences deliberately move players between
islands.

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

### M1 - See each other *(current)*

- [x] Fix remote ped rendering. Remote players are now visible in-game.
- [x] Animation sync: `animId` + time, base and partial, plus the held weapon
      and aim yaw. Built and unit-tested, but it still needs an in-game run to
      confirm it looks right. Protocol 2. See `protocol.md` §1.8 for the wire
      format and `client/src/game/addresses.h` for the offsets it reads.
- [x] Nametags over remote players. Weapon icon, name, health, drawn with the
      game's own `CFont` and its own `hud.txd` sprites from a detour on
      `CHud::Draw`. Fade out by 50 m and fade out behind walls, the second
      through one `CWorld::GetIsLineOfSightClear` per frame shared round robin
      across the roster. Run in game 2026-09-22 and the size settled at
      nametagScale 1.35. See
      `client/src/game/nametag.h` for the design and the numbers.
- [x] Remote players on the minimap. Not drawn by CoopIII at all: each player
      gets an entry in the game's own `CRadar::ms_RadarTrace` through
      `CRadar::SetEntityBlip(BLIP_CHAR, …)`, and `CHud::Draw`'s existing call
      to `CRadar::DrawBlips` draws it. Green on foot and red in a car, which
      are `ADD_BLIP_FOR_CHAR`'s and `ADD_BLIP_FOR_CAR`'s own colours, at
      `ADD_BLIP_FOR_CHAR`'s own scale of 3. No detour, no protocol change.
      The table is 32 slots with no bounds check on the far side, so CoopIII
      counts the free ones itself and keeps a reserve for the campaign
      script. Not yet run in game. See `client/src/game/radar.h`.
- [x] Decide the streaming policy (§2.1). Settled, see §5.3.

Done when: two players can see each other walk around Portland and it looks
right.

### M2 - Vehicles

**Unblocked 2026-09-21.** M2 was gated on `CVehicle` offsets that had been
located and never verified; all 22 have now been proved against the retail
binary and moved into `client/src/game/addresses.h`, along with the engine's
own `CREATE_CAR` creation path. Start with the spawn, and mind the reference
registration and the double deletion it guards against: a vehicle has two
deletion gates (`!bIsLocked && CanBeDeleted()`), so the Area B trap is waiting
here in a slightly nastier form. `client/src/game/vehicle.cpp` and `vehicle.h`
are where that path lives.

**Started 2026-09-21.** `client/src/game/vehicle.*` and the client/server
roster are in. What works and what does not is in the checklist below.

**How a vehicle enters the session, decided 2026-09-21.** GTA III generates
its own traffic and parked cars, locally and differently on every machine, so
there is no shared vehicle world to refer to, and synchronising Liberty City's
whole car population is not on the table. So a car joins the session when a
player first gets into it. The client sends `C_EnterVehicle` with the car's
identity and `netId == INVALID_NETID`, the server allocates a netId and tells
everyone else to create a matching one, and the reply tells the claimer what
its own car is called. Cars nobody has touched stay local and unsynchronised:
two players see different traffic and neither can tell. Leaving a car does not
remove it. Somebody parked it, it is still there.

- [x] Vehicle spawn/despawn, driver-authoritative.
- [x] Transform + velocities at 25 Hz, full rotation as a quaternion
      (`client/src/quat.h`, round-tripped in `basetest` over orientations a
      yaw-only sync would lose).
- [x] Steering, throttle, brake, gear, engine state, siren, lights.
- [x] Interpolation, with a slerp for the rotation (`VehicleInterpBuffer`).
- [x] Simulate and correct: the transform is written after `CGame::Process`,
      every frame, so local physics cannot own a car somebody else is driving.
- [x] Seat the remote driver. `SeatRemotePed`/`UnseatRemotePed`
      (`client/src/game/ped.cpp`), driven live from `UpdateRemoteSeats`
      (`client/src/client.cpp`). A remote driver now sits in their car instead
      of standing where it is.
- [ ] Play it through the engine's own API (`SetEnterCar`, `SetExitCar`,
      `SetCarJack`, `SetPedPositionInCar`) so the animation plays instead of
      an instant warp. Currently seated via `SetObjective` +
      `WarpPedIntoCar`, deliberately, to get the entity relationship right
      first; the animated version is the remaining piece here.
- [ ] Passengers: several players in one car, with the driver owning physics.
- [ ] Ownership handoff when the driver changes. This is the fiddly one.
- [ ] Vehicle damage: panels, doors, lights, wheels, fire, explosion.
- [ ] Never use `STATUS_PLAYER_REMOTE`. It is RC-car mode and it detonates
      cars (`protocol.md` §1.4).
- [x] Run in the game. A car spawns, renders upright, survives in the pool
      and is corrected against local physics every frame. Not yet with a real
      second player: `ghost -car` is what has driven it so far.

Done when: one player drives, another rides, and it looks the same on both
screens.

### M3 - Combat

- [ ] Weapon sync: current weapon, ammo, aim yaw/pitch. (Current weapon and
      aim yaw are already sent, see the sync inventory; ammo is not.)
- [x] Shot events (reliable), replayed through the real `CWeapon::Fire` so
      impacts happen for real (`combat.cpp`, `C_Shot`, `CH_EVENT`). Muzzle
      flash not separately handled.
- [x] Damage application and death, through `InflictDamage` / `SetDie` /
      `SetDead` rather than by writing health. **Run end to end in game
      2026-09-21**: the shooter's `C_Damage`, the victim applying it, the
      death and the respawn all appear in the two logs for the same hit. See
      `protocol.md` §1.10.2 for the exemption that makes it work, which is
      the whole fix.
- [x] Respawn. The engine's own, on the machine that died; what travels is
      that it happened, and observers see the ped die and come back. Run in
      game 2026-09-21. The clock jumping 12 hours on death comes free, since
      that is `CGameLogic::PassTime(720)` and the host's clock is the
      session's.
- [ ] Arrest: police station, the same.
- [ ] Melee.
- [x] Decide friendly fire. Settled in §5.2: off by default, server option.

### M4 - The world

- [ ] Pickups: weapons, health, armour, hidden packages. Collection must be
      exclusive, two players cannot take the same one.
- [ ] Doors and garages, including safehouse doors and the save point.
- [ ] Destructible objects, explosions, fires.
- [ ] Wanted level. Design settled (§5.1: per-player, GTA Online style,
      server-configurable). Not implemented yet; still not on the wire.
- [ ] Ambient peds and traffic: an ownership model, or accept divergence.
      `protocol.md` §3 currently accepts divergence. Honest for v1, wrong for
      a finished mod.

### M5 - Campaign, Tier 2

The design is settled in `campaign.md`; this is implementation.

- [ ] Suppress `CTheScripts::Process()` on clients.
- [ ] Settle what *else* the main script drives that clients would lose:
      pickups, garages, save points, the intro (`campaign.md` §6). Do this
      first, it may change the shape of the rest.
- [ ] Replicate script effects: subtitles, blips, script spheres, mission
      state, cutscenes, fades.
- [ ] Mission entities as ordinary netid entities.
- [ ] Mission cleanup on pass/fail.
- [ ] Cutscene handling: freeze remote players too, or they wander through the
      scripted camera.
- [ ] Server options for the three deliberate SP divergences (`campaign.md`
      §2.3): death fails the mission, cutscene freezing, host-owned save.

Done when: a group can play a mission from start to finish together.

### M6 - Campaign, Tier 3

- [ ] Intercept the three trigger opcodes so *any* player can start a
      mission, not only the host. Measured over the real campaign: 123 call
      sites across `IS_PLAYER_IN_AREA_3D` (63), `IS_PLAYER_IN_AREA_2D` (57),
      `IS_PLAYER_IN_ZONE` (3). `campaign.md` §4.
- [ ] Decide: one detour on `ProcessCommands` switching on opcode, or patch
      the individual handlers.

### M7 - Everything that makes it usable

- [ ] Chat UI (the protocol already carries it).
- [ ] Player list, ping, connection state on screen.
- [ ] Join/leave messages in game.
- [ ] Reconnection that restores you where you were.
- [ ] A server that can be run by a person who is not us: config file, clear
      logs, a README that covers port forwarding.
- [ ] Desync diagnostics. When someone says "he was in a different place on
      my screen", there has to be something to look at.

---

## 4. Sync inventory

Everything that has to travel, and where it stands. Sources are re3 members
(`protocol.md` §1.7).

### Player

| What | Fields | Status |
|---|---|---|
| Transform | `m_matrix` position, `m_fRotationCur` | ✅ sent |
| Velocity | `m_vecMoveSpeed` | ✅ sent |
| Vitals | `m_fHealth`, `m_fArmour` | ✅ sent, and on the join packet as well as the snapshot, so a late joiner creates a ped on the health it really has (`protocol.md` §2.8.1) |
| State | `m_nPedState`, `m_nMoveState` | ✅ sent; `m_nMoveState` is also applied, so the engine picks the walk/run animation itself |
| Animation | `AnimationId` + time, base **and** partial | ✅ sent and applied via `CAnimManager::BlendAnimation`. Run in game 2026-09-22, every weapon the owner tried |
| Weapon | `m_weapons[]`, `m_currentWeapon` | ✅ sent and applied via `CPed::GiveWeapon` + `SetCurrentWeapon`, so the model is in the hand. Ammo is not on the wire (M3) |
| Aim | yaw/pitch | ⚠️ both sent; yaw applied via `CPed::SetAimFlag`. Pitch is not applied: `CPed::AimGun` hard-codes 0 for non-player peds (`protocol.md` §1.8.3), so it needs a detour, and that belongs with M3 |
| Shots | event | ✅ sent reliably and replayed through the real `CWeapon::Fire`, so impacts happen for real (`combat.cpp`) |
| Damage / death | event | ✅ sent by the shooter, applied by the victim through `CPed::InflictDamage`, with the one exemption that lets an authorised hit past the remote-attacker rule. Run end to end in game 2026-09-21. Death is also **held by the session** and replayed on the join packet, so somebody who joins while a player is lying in the road gets a corpse rather than a live player on zero health (`protocol.md` §2.8.1) |
| Enter/exit vehicle | event | ⚠️ the remote ped is now seated (`SeatRemotePed`), but through `WarpPedIntoCar` rather than `SetEnterCar`, so no animation plays yet |
| Wanted level | `CPlayerInfo::m_pWanted` | ❌ design settled (§5.1), not implemented |

### Vehicle

Offsets verified 2026-09-21 (Area E) and now in `client/src/game/addresses.h`
with the proof for each, so a "❌ nothing sends it" below is about the wire and
nothing else. The address is no longer the obstacle.

| What | Fields | Status |
|---|---|---|
| Transform | full rotation, not yaw | ✅ sent and applied, as a quaternion (`client/src/quat.h`) |
| Velocities | `m_vecMoveSpeed` `0x78`, `m_vecTurnSpeed` `0x84` | ✅ sent and applied |
| Controls | `m_fSteerAngle` `0x1E8`, `m_fGasPedal` `0x1EC`, `m_fBrakePedal` `0x1F0`, `m_nCurrentGear` `0x204` | ✅ sent and applied |
| Health / state | `m_fHealth` `0x200` (1000 = full), `bEngineOn` `0x1F5` bit 4, `m_bSirenOrAlarm` `0x22E` | ✅ sent and applied; flags on change only, so the siren does not restart every frame |
| Appearance | `m_currentColour1/2` `0x19C`/`0x19D` | ✅ carried by the spawn packet, so both machines get the same car rather than two random paint jobs |
| Occupants | `pDriver` `0x1A4`, `pPassengers[8]` `0x1A8`, `m_nNumMaxPassengers` `0x1CC` | ✅ every seat is sent, seated (`SeatRemotePed`) and, since `protocol.md` §2.8.2, remembered by the session so a late joiner is told about passengers and not just drivers |
| Destroyed | `VEH_WRECKED` on the wire | ⚠️ sent and remembered when it comes from the car's driver, and a wreck is then left out of the backfill. A car destroyed while **parked** has nobody to report it at all — §5.8 |
| Extra components | `m_aExtras[2]` `0x19E` | ❌ each machine picks its own at construction — §5.9 |
| Spawn / despawn | `CREATE_CAR` path, `sizeof(CAutomobile)` `0x5A8` | ✅ run in the game, both deletion gates shut |
| Damage model | panels, doors, lights, wheels | ❌ not designed. `CDamageManager` is the first `CAutomobile` member, at `+0x288` |

### World

| What | Status |
|---|---|
| Clock | ✅ follows the host's `CClock`, not a clock the server keeps on its own. The host reports at 1 Hz; everyone else is moved only once they are more than 3 game minutes out, so the HUD clock and the sun do not stutter. Run in game 2026-09-21, including a host handover mid-session and the 12-hour jump the engine makes on death |
| Weather | ✅ follows the host's `CWeather`. Both ends of the blend are sent, since a single type describes where the sky is going and not where it is. Needs an in-game run |
| Pickups | ❌ needs exclusive collection |
| Garages / doors | ❌ |
| Explosions, fires | ❌ |
| Destroyed objects | ❌ |
| Ambient peds, traffic | ❌ deliberately out of v1 |

### Campaign

| What | Status |
|---|---|
| Script execution | ❌ host-only, designed not built |
| Mission entities | ❌ |
| Subtitles, blips, spheres | ❌ new packets needed |
| Cutscenes | ❌ |
| Mission pass/fail | ❌ |
| Triggers (Tier 3) | ❌ 3 opcodes, 123 sites |
| Save / progress | ❌ host-owned |

---

## 5. Decisions - settled

These change how the game *feels*, so they were decided deliberately rather
than left to fall out of the code. Decided 2026-09-21. Treat them as settled;
reopen only with the owner.

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

Note the engine constraint: the wanted level lives in `CPlayerInfo`, and there
is exactly one of those (§2.3). Remote players are `CPed`s, so their stars are
CoopIII's own state, and the police AI has to be driven from it rather than
read out of the engine. That makes `perplayer` more work than `shared`. It is
still the right default.

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

**The blip half of this is already built** (M1, `client/src/game/radar.h`) and
it was built so this would not need rebuilding. Today a remote player's blip is
a `BLIP_CHAR` tracking their `CPed`; a player with no ped gets no blip, and
that is one predicate, `BlipWanted`. When distant players land it becomes a
`BLIP_COORD` created with `CRadar::SetCoordBlip` (recorded in `addresses.h`),
drawn by the same `DrawBlips` loop with the same square, the same colour table
and the same rim clamp — so the handover is invisible on screen, which is the
whole reason the game's own blip was used rather than a sprite of our own. The
slot accounting and the reserve are already blip-kind-agnostic. Two things are
new: a coord blip carries no entity handle, so ownership has to come from the
slot plus the generation rather than from `TraceIsOurs`; and no engine setter
moves a coord blip, so CoopIII writes `m_vec2DPos`/`m_vecPos` itself each frame
from the position already on the wire. `radar.h` says both at the bottom.

### 5.4 Mission failure on death - it fails, as in single player

If a player dies during a mission, the mission fails. Faithful to SP.

This reverses the provisional default in `campaign.md` §2.3, which assumed
co-op should keep going while someone is alive. `campaign.md` §2.3 must be
updated. The server option can still exist, but its default now matches SP.

### 5.5 Fidelity wins by default

Stated once so it stops being case-by-case drift: when single-player behaviour
and co-op convenience conflict, single-player behaviour wins. Server options
may relax it; the defaults do not.

§5.4 is that policy applied.

### 5.6 CoopIII only runs when launched through the launcher

New requirement, and a good one. Dropping `CoopIII.asi` into the game folder
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

Status: not yet implemented. It touches `client/src/dllmain.cpp` and
`launcher/src/main.cpp`.

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
0047964F  je  00479890            -> jumps to 004796C7, past the whole AI
```

So GTA III already has a way to set a ped alight with no burning-ped AI
attached, and it uses it for the one ped whose movement is not the engine's to
decide. A remote player is that ped on this machine. CoopIII takes the same
branch: `LightRemoteFire` in `client/src/game/ped.cpp` is StartFire's shared
tail transcribed in its own order, with the branch not taken — and nothing
else. The engine's AI is never started, so there is nothing to suppress,
nothing to unwind, and no state for the two to fight over.

The other half is that a fire, once lit, writes nothing to the ped at all.
`CFire::ProcessFire` reads the ped's matrix, asserts the two-way link and
calls `InflictDamage`. It never touches `m_nPedState`, `m_nMoveState` or the
clump. **The AI was the whole of the risk and it was all in one branch.**

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

Found while making the join path late-joiner safe (`protocol.md` §2.8) and
**not fixed**, because the half that matters is somebody else's file.

`C_VEHICLE_STATE` is sent by one machine and one only: the driver's. So a
synced car that is parked receives no updates at all, and its row in the
session freezes at whatever its last driver said. Everything the backfill now
carries about a car's condition — health, engine, siren, `VEH_WRECKED` — can
therefore only ever arrive **from inside it**.

Which leaves the commonest way a car is destroyed with no carrier: you blow up
a parked one. Nobody is in it, nobody reports it, the session keeps a healthy
row for it forever, and every joiner from then on is handed a pristine car
standing where a burnt-out shell is on every other screen. That is very
probably the exact path the owner's car took.

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

### 5.9 A car's extra components are picked per machine. Named work.

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
- **Test what can be tested without the game.** Six suites cover the protocol,
  the roster, interpolation, patterns and hooks. What is left needing a live
  game is then small enough to reason about.
