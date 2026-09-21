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
      across the roster. Built and unit-tested, not yet run in-game. See
      `client/src/game/nametag.h` for the design and the numbers.
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
- [ ] Damage application and death, through `InflictDamage` / `SetDie` /
      `SetDead` rather than by writing health.
- [ ] Respawn: hospital, weapon loss, the whole SP behaviour.
- [ ] Arrest: police station, the same.
- [ ] Melee.
- [ ] Decide friendly fire. A co-op default of off, with a server option.

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
| Vitals | `m_fHealth`, `m_fArmour` | ✅ sent |
| State | `m_nPedState`, `m_nMoveState` | ✅ sent; `m_nMoveState` is also applied, so the engine picks the walk/run animation itself |
| Animation | `AnimationId` + time, base **and** partial | ✅ sent and applied via `CAnimManager::BlendAnimation`. Needs an in-game run |
| Weapon | `m_weapons[]`, `m_currentWeapon` | ✅ sent and applied via `CPed::GiveWeapon` + `SetCurrentWeapon`, so the model is in the hand. Ammo is not on the wire (M3) |
| Aim | yaw/pitch | ⚠️ both sent; yaw applied via `CPed::SetAimFlag`. Pitch is not applied: `CPed::AimGun` hard-codes 0 for non-player peds (`protocol.md` §1.8.3), so it needs a detour, and that belongs with M3 |
| Shots | event | ✅ sent reliably and replayed through the real `CWeapon::Fire`, so impacts happen for real (`combat.cpp`) |
| Damage / death | event | ❌ |
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
| Occupants | `pDriver` `0x1A4`, `pPassengers[8]` `0x1A8`, `m_nNumMaxPassengers` `0x1CC` | ✅ the driver is sent and seated (`SeatRemotePed`); ❌ passengers are not |
| Spawn / despawn | `CREATE_CAR` path, `sizeof(CAutomobile)` `0x5A8` | ✅ run in the game, both deletion gates shut |
| Damage model | panels, doors, lights, wheels | ❌ not designed. `CDamageManager` is the first `CAutomobile` member, at `+0x288` |

### World

| What | Status |
|---|---|
| Clock | ✅ 1 Hz |
| Weather | ✅ 1 Hz |
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

### 5.7 Fire is world state, and it gets synced. Two phases.

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

**Phase two, not built: fire is synced as world state, whatever lit it.**

Not "flamethrower fire and molotov fire". Fire. Explosions, molotovs, the
flamethrower, rockets, a car burning out, a ped alight, a script fire. Where
it came from does not matter, which is what makes it tractable: the engine
already treats fire as one flat table rather than as a property of the weapon
that caused it.

That table is `gFireManager`, a single global with a fixed `m_aFires[NUM_FIRES]`,
each entry carrying `m_bIsOngoing`, `m_pEntity` and `m_vecPos`. The strongest
hint that it is snapshot-shaped is that the engine's own replay system already
treats it as a block: `CReplay` stores and restores the whole array plus
`m_nTotalFires` with two `memcpy`s (re3 Replay.cpp:1176-1177, 1353-1356).

Until it is built, burning is cosmetic on an observer and damaging only on the
machine that lit it, and the fire a molotov leaves behind is terrain: it burns
whoever walks into it, friendly fire or not.

Two things to settle first, neither of which is answerable from `re3` alone
and both of which shape the design:

- whether the fire array is a fixed size in the retail build, and what that
  size is;
- whether a `CFire` entry can point at an entity (`m_pEntity`, a burning ped
  or car) rather than just a position. A fire attached to a remote player's
  ped is a different sync problem from a fire sitting on the pavement, and it
  is the one that decides whether this is a snapshot or an event stream.

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
