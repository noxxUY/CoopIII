# CoopIII - wire protocol & sync design (v1 draft)

Every engine claim below is cited as `file:line` in `reference/re3` (branch
`main` = GTA III). Verify there before trusting anything here, and re-verify
against the real retail 1.0 binary before writing offsets. Five anchors pinned
this build, plus the correction from 1.1; `client/src/game/addresses.h` carries
the proof for every address it lists.

---

## 1. What the engine forces on us

These are constraints the design has to live with, not preferences.

### 1.1 The game is single-threaded and frame-driven

`Idle()` (`src/core/main.cpp:1551`) runs, per frame:

```
CTimer::Update()                  main.cpp:1557
CGame::Process()                  main.cpp:1594  → Game.cpp:1002
  CPad::UpdatePads()              Game.cpp:1004
  CStreaming::Update()            Game.cpp:1021
  CClock::Update() / CWeather::Update()   Game.cpp:1031-1032
  ... world & entity processing
DMAudio.Service()
... render phase
```

There is no engine tick separate from the render frame, and no threading model
to hook into. Consequence: socket I/O runs on its own thread, and the game
thread only drains queues. Applying net state from the I/O thread would race
the world update.

### 1.2 There is no fixed timestep, and no usable engine clock

`ms_fTimeStep` is derived from the real frame time every frame:

```cpp
ms_fTimeStep = frameTime / 1000.0f * 50.0f;   // src/core/Timer.cpp:113
```

So the `50` in `GetTimeStepInSeconds() = ms_fTimeStep / 50.0f`
(`src/core/Timer.h:20`) is a normalization unit, not a tick rate. The game has
no fixed logical step to align to: at 60 FPS `ms_fTimeStep` is ~0.833, at 30
FPS ~1.667. 25 Hz is therefore chosen on latency/bandwidth grounds (§2.3), not
because it divides anything.

`CTimer::GetTimeInMilliseconds()` is also not usable as a packet timestamp:

- it stops advancing while the game is paused (`Timer.cpp:107-114`);
- it is scaled by `ms_fTimeScale`, which missions set through `SET_TIME_SCALE`
  (`Script2.cpp:302`) and which player death drops to `1/3` for the slow-motion
  effect (`PlayerPed.cpp:480`).

`GetTimeInMillisecondsPauseMode()` keeps advancing while paused but is still
rescaled.

Consequence: CoopIII carries its own monotonic wall clock for
`PacketHeader::sendTimeMs`, and drives the 25 Hz send cadence off that same
clock. A dying player would otherwise emit timestamps at a third of everyone
else's rate and desync their own interpolation. `CTimer` is still the right
clock for anything that has to match engine behaviour, just not for the
network.

### 1.3 PC GTA III has exactly one player slot

`NUMPLAYERS = 1, // 4 on PS2` (`src/core/config.h:7`), and
`CWorld::Players[NUMPLAYERS]` (`src/core/World.h:61`).

Consequence: remote players can never be `CPlayerInfo` entries. They are
ordinary `CPed`s out of the ped pool whose transform and animation we write
every frame. Anything in the codebase that reaches for
`CWorld::Players[CWorld::PlayerInFocus]` is local-player-only.

### 1.4 `STATUS_PLAYER_REMOTE` is a trap - do not use it

It reads like a multiplayer leftover. It isn't: it's the RC car mode.
`CRemote::GivePlayerRemoteControlledCar()` sets it (`src/control/Remote.cpp:22`),
and `CAutomobile::ProcessControl` handles it by blowing the car up when the
weapon key is pressed (`src/vehicles/Automobile.cpp:335`).

Consequence: remote-driven vehicles use `STATUS_PHYSICS` (or `STATUS_PLAYER`
for the local one). Touching `STATUS_PLAYER_REMOTE` detonates cars.

### 1.5 Pool handles are per-process

`CPools::GetPedRef(ped)` / `GetPed(handle)` and the vehicle equivalents
(`src/core/Pools.h:47-52`) are slot-based. Slot N on your machine is a different
entity than slot N on mine.

Consequence: the wire carries server-assigned network IDs only. Each client
keeps `netid ↔ local pool handle` maps in both directions. A raw pool handle
must never be serialized.

### 1.6 Models must be streamed before an entity can exist

`CStreaming::RequestModel(id, flags)` / `HasModelLoaded(id)` /
`LoadAllRequestedModels()` (`src/core/Streaming.h:124,120,168`).

Consequence: spawning a remote player or their car is two-phase. The spawn
packet requests the model; the entity gets created on a later frame, once
`HasModelLoaded()` is true. Entity creation can't be synchronous with packet
arrival.

### 1.7 What state actually defines an entity

Transform lives in `CPlaceable::m_matrix` (`src/core/Placeable.h:9`), a full
matrix, inherited by every `CEntity` (`src/entities/Entity.h:35`). Velocities
live in `CPhysical`: `m_vecMoveSpeed` (linear) and `m_vecTurnSpeed` (angular)
(`src/entities/Physical.h:24-25`).

Ped-specific (`src/peds/Ped.h`):

| Field | Line | Notes |
|---|---|---|
| `m_nPedState` (`PedState`) | 421 | enum at 222; `PED_IDLE`, `PED_JUMP`, `PED_DRIVING`, `PED_DEAD`, … |
| `m_nMoveState` (`eMoveState`) | 423 | enum at 290: `STILL`/`WALK`/`RUN`/`SPRINT` |
| `m_fHealth`, `m_fArmour` | 436-437 | |
| `m_fRotationCur`, `m_fRotationDest` | 444-445 | ped heading is a scalar yaw, not a full rotation |
| `m_pMyVehicle`, `bInVehicle` | 454-455 | |
| `m_weapons[]`, `m_currentWeapon` | 478, 480 | `CWeapon { m_eWeaponType, m_eWeaponState, m_nAmmoInClip, m_nAmmoTotal, m_nTimer }` (`src/weapons/Weapon.h:17-21`). 13 slots, indexed by `eWeaponType`; the index *is* the type |
| `m_pedIK.m_torsoOrient` | `src/peds/PedIK.h:37` | where the gun actually points: `{yaw, pitch}`, yaw relative to `m_fRotationCur`, clamped to ±50° / ±45° |
| `m_fLookDirection` | 484 | the aim *target* yaw, in world space. `CPed::AimGun` feeds it to `CPedIK::PointGunInDirection`, which subtracts the heading |

Vehicle-specific (`src/vehicles/Vehicle.h`): `m_fSteerAngle` (129),
`m_fGasPedal` (130), `m_fBrakePedal` (131), `m_nCurrentGear` (174), `m_fHealth`
(173: *1000 = full, 250 = on fire, 0 = explodes*), `bEngineOn` (139),
`m_currentColour1/2` (114-115), `m_bSirenOrAlarm` (190), `m_vehType` (194).

Consequence: peds need 1 float of orientation, vehicles need a full one. That
asymmetry shapes the packet layouts below.

### 1.8 Animations are a small stable integer namespace

`enum AnimationId` (`src/animation/AnimationId.h:3`): `ANIM_STD_WALK`,
`ANIM_STD_RUN`, `ANIM_STD_IDLE`, the `ANIM_STD_KO_*` / `ANIM_STD_HITBYGUN_*`
families, etc.

A ped's clump carries a list of `CAnimBlendAssociation`s, each with a
`blendAmount`, not a single current animation. Two of them matter for sync:

- the dominant non-partial association, the whole-body pose (walk, run, idle,
  jump, fall, KO, get-up, sitting in a car);
- the dominant partial association (`ASSOC_PARTIAL`), the overlay played on
  top, where firing, punching and throwing live.

Without the second one a remote player fires a gun and nothing moves. So the
wire carries two ids, each with its playback time.

Consequence: animation sync is two `uint16` + time, not anything structural.
It's cheap, and it's most of what makes a remote player look alive, so it's in
v1.

#### 1.8.1 The anim group is a trap: `animId` is an *index*

`CAnimManager::BlendAnimation(clump, groupId, animId, delta)` resolves the
animation as `ms_aAnimAssocGroups[groupId].assocList[animId]`, a raw index,
with no bounds check (verified: the retail `CAnimBlendAssocGroup::GetAnimation`
is literally `shl eax,6 / add eax,[ecx] / ret`).

The groups are not the same size. `ASSOCGRP_STD` ("man") holds the whole
`AnimationId` namespace; every other group (`ASSOCGRP_PLAYER`, `ASSOCGRP_GANG1`,
`ASSOCGRP_WOMAN`, …) holds only the four or five locomotion anims (`WALK`,
`RUN`, `RUNFAST`, `IDLE`, sometimes `STARTWALK`), because a group is a walking
*style*, not a repertoire.

The engine respects this: `CPed::SetMoveAnim` passes `m_animGroup` and never an
id above `ANIM_STD_IDLE`; everything else in `CPed` passes `ASSOCGRP_STD`.

Consequence, and it's a memory-safety rule rather than a style one: use the
ped's own `m_animGroup` only for ids ≤ `ANIM_STD_IDLE`, `ASSOCGRP_STD` for
everything else, and bounds-check against that group's `numAssociations` before
the call. An id off the end of a 4-entry group reads 0x40-byte strides past the
array and hands the result to the blender.

#### 1.8.2 Ids are portable between groups

Within every group, `assocList[i].animId == i` (`CAnimBlendAssocGroup::CreateAssociations`),
and the per-group name tables are parallel: index 1 is "that group's run".
So an `animId` sampled from a player using `ASSOCGRP_PLAYER` means the same
thing when replayed into a civilian using `ASSOCGRP_STD`. No remapping is
needed. Without that, one `uint16` would not be enough.

#### 1.8.3 Aim: set the engine's inputs, do not rotate bones

`CWorld::Process` updates every moving entity's animations first, and only
then runs their `ProcessControl`. Torso aiming happens in the second pass
(`CPed::ProcessControl` → `AimGun` → `CPedIK::PointGunInDirection` →
`RotateTorso`), on top of the pose the first pass just wrote.

CoopIII's inbound hook runs before both. Anything it writes into a bone matrix
is overwritten in the same frame.

Consequence: aim is applied by setting the engine's own inputs,
`CPed::bIsAimingGun` and `m_fLookDirection`, through `CPed::SetAimFlag`, and
letting `ProcessControl` do the rotation at the point in the frame where it
survives. Same reason the locomotion animation is driven by writing
`m_nMoveState` and letting `CPed::SetMoveAnim` choose.

Pitch is sampled and sent, but not yet applied. `CPed::AimGun` hard-codes
`PointGunInDirection(m_fLookDirection, 0.0f)` for anything that is not the
local `CPlayerPed`, and `MoveLimb` drags the torso back toward that zero at
7°/timestep. Writing the pitch every 25 Hz tick against a 60 Hz decay produces
a twitch, not an aim. Doing it properly means detouring `AimGun`; that belongs
with M3, where the pitch has to agree with the shot vector anyway.

### 1.9 Combat: a shot is an event, a trigger is a state, a projectile is an entity

Three different things get called "firing" and they need three different
treatments. This section is the design; `sdk/include/coopiii/protocol.h` has the
structs and `client/src/game/combat.h` the engine side.

#### 1.9.1 Why firing rides *both* the snapshot flag and the reliable event

`PF_FIRING` is `CPed::bIsShooting`. It's a state (the trigger is held), and a
25 Hz snapshot handles states well. It keeps the remote ped in a firing posture
between shots and costs nothing.

It cannot carry the shots themselves. An Uzi empties a clip between two
snapshot ticks, so a flag would collapse a burst into one sample; and the flag
carries no origin, so there is nothing to draw a muzzle flash at or throw a
molotov from. So each discharge is also a reliable `C_Shot` on channel 1. The
division is: the flag says they are shooting, the event says what left the
barrel.

#### 1.9.2 The observer replays the engine's own `CWeapon::Fire`

An observer does not reproduce a shot by hand. It calls `CWeapon::Fire` on the
remote ped's own `CWeapon`, the same function the shooter's engine called. That
gives the muzzle flash, the gunshot, the shell casing, the point light, the
bullet decal and the projectile for free, and gets them right for every weapon
without CoopIII knowing which is which. It's §1.8.3's rule applied to combat:
drive the engine, don't paint the result.

The price is that `CWeapon::Fire` also *decides what it hit*, which is not the
observer's to decide. Two things make that safe, and neither is a new idea:

- remote peds are already `bBulletProof` / `bFireProof` / `bCollisionProof` /
  `bMeleeProof` (Area B), and are now `bExplosionProof` as well;
- the local player is made `bBulletProof` for the duration of the call, the
  same flip `CPlayerInfo::MakePlayerSafe` does for the cheat.

Everything else a replayed shot can touch (traffic, ambient peds, glass) has no
owner anywhere in the session and is already expected to differ between
machines (§3).

Consequence, and it isn't a bug: no player can currently shoot another player.
The shooter's machine cannot decide it, because the victim's ped is bulletproof
there; the victim's machine will not, because the replay is defused. Closing
that is what `C_Damage` / `S_Damage` / `S_Death` are reserved for: the shooter's
machine intercepts its own hit on a remote ped and sends it, and the victim
applies it to itself. It's a separate piece of work with its own in-game
verification. Doing it by accident, by simply letting the replayed shot damage
the local player, would mean the question "did A hit B" is answered by B's
machine, using A's position interpolated 100 ms late. People would be shot
around corners.

Explosions are the exception, and not an inconsistency. §1.9.3's explosion is
replayed at a fixed world position that its owner chose, so "is B standing in
it" is a question about B, answered on B's machine, with no stale data
involved. Same rule, not a relaxation of it: nobody decides another player's
damage, and a grenade hurts where a bullet doesn't because a grenade's
authority is a *place* and a bullet's is a *ray* from somewhere only the shooter
knows. Remote peds are therefore `bExplosionProof`; an observer must not apply
the blast to *them*.

Four weapons are deliberately not replayed:

| Weapon | Why not |
|---|---|
| `SNIPERRIFLE` | `CWeapon::FireSniper` returns immediately unless *this* machine's camera is in a first-person mode, and then fires along `TheCamera.Cams[ActiveCam].Front`, the observer's own view. It is unusable for anyone but the local player. |
| `FLAMETHROWER` | `CShotInfo` keeps burning for as long as the shot lives and damages through `WEAPONTYPE_FLAMETHROWER` long after the replay call has returned, so the one-call bulletproof flip does not cover it. |
| `DETONATOR` | Fires nothing; it sets off bombs already planted. A planted bomb is not synced, so replaying it would detonate a different machine's props. |
| `UNARMED` / `BASEBALLBAT` | Melee is a damage event with no projectile and no flash. Everything visible about it is the animation, which the snapshot already carries. |

#### 1.9.3 A projectile is animated by the observer and ended by its owner

Molotovs, grenades and rockets are `CProjectileInfo` entries with a real
`CObject` flying on real physics, bouncing with an elasticity, and exploding on
a timer or on contact. Two machines running that from identical initial
conditions still diverge, because `ms_fTimeStep` is the real frame time (§1.2)
and the explosion deadline is each machine's own `CTimer`. A molotov that lands
two metres apart leaves the fire in the wrong street.

So the projectile is treated exactly like a remote car (`vehicle.h`): the
observer may animate it, but it may not decide where it ends up.

- `C_Shot` for a projectile weapon carries `dir` and `speed`, the unit
  direction and magnitude of the projectile's initial `m_vecMoveSpeed`, read
  off the object the thrower's own engine created. The observer lets
  `CWeapon::Fire` create its copy and then writes that position and velocity
  over it, so both arcs *start* identical.
- `C_Explosion` carries the type and the position of the explosion the owner's
  machine actually produced. The observer plays it there.
- The observer's copy is forbidden to explode at all. CoopIII detours
  `CProjectileInfo::RemoveProjectile` and, for a projectile it created for a
  remote player, blanks `m_eWeaponType` before calling the original. The
  original's three-way switch then falls through to the teardown and skips
  `CExplosion::AddExplosion` entirely. The engine frees the slot; nothing else
  changes.

`C_Explosion` is sampled by detouring `CExplosion::AddExplosion`, which every
explosion in the game funnels through, and forwarding the ones whose *culprit*
is the local player ped. One seam for grenades, molotovs and rockets rather
than three, and it catches the case a projectile tracker would miss entirely:
a throw against a wall, where `CWeapon::FireProjectile` finds the line of sight
blocked and goes straight to `CProjectileInfo::RemoveNotAdd` without ever
creating a projectile.

#### 1.9.4 Ammunition is not synced

`CPed::GiveWeapon` gives a remote ped a large fixed amount and the replay resets
that slot to `WEAPONSTATE_READY` before each shot. A remote player's clip is not
a thing anyone can see; what it *can* do is make `CWeapon::Fire` refuse
(`if (m_nAmmoInClip <= 0) return false`) and silently stop rendering their
shots. Syncing the real count would buy nothing and add a failure mode.

---

## 2. Design decisions

### 2.1 Topology: dedicated server, client-authoritative players

Each client is authoritative over (a) its own ped and (b) the vehicle it is
currently driving. The server owns global state (clock, weather) and assigns
ownership for everything else.

Why not server-authoritative: GTA III physics is frame-rate-coupled through
`ms_fTimeStep` (§1.2) and not deterministic across machines, so rolling back or
resimulating isn't realistic here. Why that's acceptable: this is co-op with
friends, not competitive play. There is no anti-cheat requirement, so the usual
reason to pay for server authority doesn't apply.

Explicit consequence to document for players: a malicious client can cheat.
It's a deliberate trade, not an oversight.

### 2.2 Transport: UDP via ENet

Needs unreliable-sequenced (snapshots) *and* reliable-ordered (events) on one
socket, with fragmentation and MTU handling. ENet does exactly this, is C, and
builds as a static x86 lib. The x86 part matters: the client DLL has to be
32-bit to load into `gta3.exe` (`README.md` has the build commands).

Channels:

| Ch | Mode | Carries |
|---|---|---|
| 0 | unreliable sequenced | `S_PLAYER_STATE`, `S_VEHICLE_STATE` snapshots |
| 1 | reliable ordered | spawn/despawn, enter/exit, damage, death, chat, world state |

Rejected: raw UDP + hand-rolled reliability (rebuilding ENet badly), TCP
(head-of-line blocking ruins 25 Hz snapshots).

### 2.3 Rates

| Stream | Rate | Rationale |
|---|---|---|
| Player snapshot | 25 Hz | Smooth at 100 ms interp delay (§2.6) without flooding; see §1.2 |
| Vehicle snapshot (driver's) | 25 Hz, same packet as its driver | avoids ped/vehicle desync within a frame |
| World state (clock, weather) | 1 Hz | `CClock`/`CWeather` update per frame but change slowly (`Game.cpp:1031-1032`) |
| Events | on occurrence, reliable | shots, damage, enter/exit, death |

Bandwidth check at 8 players: 7 remotes × 25 Hz × ~72 B ≈ 12.6 KB/s down per
client. Small enough that v1 doesn't quantize anything, just plain `float32`.
Bit-packing can come later.

### 2.4 Framing

1-byte opcode + fixed-layout payload. `#pragma pack(1)`, little-endian on both
ends (x86 → x86, no swapping). Shared structs live in
`sdk/include/coopiii/protocol.h` and are the single source of truth for both
`client/` and `server/`.

```c
struct PacketHeader {
    uint8  opcode;
    uint32 sendTimeMs;   // sender's monotonic wall clock (§1.2)
};
```

`sendTimeMs` is what interpolation buffers key off (§2.6).

### 2.5 Identity

- `uint16 netId`: server-assigned, unique per entity for the session lifetime.
- `uint8 playerId`: server-assigned slot, 0..MAX_PLAYERS-1.
- Client keeps `netId → pool handle` and `pool handle → netId`. Rebuilt on
  respawn/restream; never persisted, never sent (§1.5).

### 2.6 Interpolation

Remote entities buffer ~100 ms of snapshots and render interpolated between the
two straddling `sendTimeMs`. If the buffer underruns, extrapolate using the last
`m_vecMoveSpeed` for at most ~250 ms, then freeze.

Position is never hard-set except on spawn or when the error exceeds ~5 m
(teleport/desync recovery). Writing `SetPosition()` raw every frame fights the
collision system and produces the jitter these mods are known for.

Peds interpolate `m_fRotationCur` as a scalar angle (§1.7). Vehicles slerp the
orientation quaternion.

---

## 3. v1 sync scope

In:

- On-foot player: position, heading, move state, animation, health, armour
- Weapons: current weapon, aim direction, shot events
- Vehicles: the driver's vehicle (transform, steer/gas/brake, health, engine)
- Enter/exit/carjack, using the engine's own API so animations play correctly:
  `SetEnterCar` (`Ped.h:701`), `SetExitCar` (703), `SetCarJack` (711),
  `SetPedPositionInCar` (565)
- Damage/death: `InflictDamage` (612), `SetDie` (545), `SetDead` (546)
- Clock + weather
- Chat

Out (deliberately, for v1):

- NPC peds and traffic: every client simulates its own. They will visibly
  differ between machines. Fixing this needs an ownership/streaming model
  (§1.6) and is the single biggest item after v1.
- Missions/script state: out of v1, but not out of scope. The project's goal is
  the campaign played co-op; v1 is Tier 1 of that plan (clients suppress
  `CTheScripts::Process()`, so the campaign does not run and the city is free
  roam). Tiers 2 and 3 add the host-driven campaign and shared triggers. The
  design is in [campaign.md](campaign.md); read it before touching anything
  script-related. Nothing in v1 is throwaway: Tier 2 replicates mission
  entities through exactly the machinery listed above.
- Pickups, wanted level, stats.

Rationale: the smallest slice where two people can drive around Liberty City
together and it feels right. Everything cut is cut because it needs an ownership
model that doesn't exist yet.

---

## 4. Packet list (v1)

`C_` = client→server, `S_` = server→client.

| Opcode | Name | Ch | Payload |
|---|---|---|---|
| 0x01 | `C_HELLO` | 1 | protocol version, nickname, model id |
| 0x02 | `S_WELCOME` | 1 | your `playerId`, server tick rate, world state |
| 0x03 | `S_PLAYER_JOIN` | 1 | `playerId`, `netId`, nickname, model id, spawn transform |
| 0x04 | `S_PLAYER_LEAVE` | 1 | `playerId`, reason |
| 0x10 | `C_PLAYER_STATE` | 0 | pos `float[3]`, heading `float`, `m_vecMoveSpeed` `float[3]`, `m_nMoveState` `u8`, `m_nPedState` `u8`, anim `u16` + time `float` + speed `float`, partial anim `u16` + time `float`, health `float`, armour `float`, weapon `u8`, aim yaw/pitch `float[2]`, flags `u8` (64 bytes) |
| 0x11 | `S_PLAYER_STATE` | 0 | `playerId` + the above |
| 0x12 | `C_VEHICLE_STATE` | 0 | `netId`, pos `float[3]`, quat `float[4]`, move/turn speed `float[6]`, steer/gas/brake `float[3]`, gear `u8`, health `float`, flags `u8` |
| 0x13 | `S_VEHICLE_STATE` | 0 | same, relayed |
| 0x20 | `C_SHOT` / 0x21 `S_SHOT` | 1 | weapon `u8`, origin `float[3]`, direction `float[3]`, speed `float` (§1.9.1) |
| 0x22 | `C_DAMAGE` / 0x23 `S_DAMAGE` | 1 | victim `netId`, weapon `u8`, amount `float`, piece `u8`; reserved, see §1.9.2 |
| 0x24 | `S_DEATH` | 1 | `playerId`, killer `netId`, anim `u16`; reserved |
| 0x25 | `C_RESPAWN` / 0x26 `S_RESPAWN` | 1 | spawn transform |
| 0x27 | `C_EXPLOSION` / 0x28 `S_EXPLOSION` | 1 | type `u8`, pos `float[3]` (§1.9.3) |
| 0x30 | `C_ENTER_VEHICLE` / 0x31 `S_ENTER_VEHICLE` | 1 | `netId`, seat `u8`, jack `bool` |
| 0x32 | `C_EXIT_VEHICLE` / 0x33 `S_EXIT_VEHICLE` | 1 | `netId` |
| 0x34 | `S_VEHICLE_SPAWN` | 1 | `netId`, model id, transform, colours |
| 0x35 | `S_VEHICLE_DESPAWN` | 1 | `netId` |
| 0x40 | `S_WORLD_STATE` | 1 | game hour/minute, weather type, blend |
| 0x50 | `C_CHAT` / 0x51 `S_CHAT` | 1 | `playerId`, text |

---

## 5. Hook points

Two hooks, both in the frame flow of §1.1:

1. **Inbound**, early in `CGame::Process`, after `CPad::UpdatePads()`
   (`Game.cpp:1004`) and before world processing: drain the inbound queue,
   apply remote state, spawn/despawn anything whose model finished streaming
   (§1.6).
2. **Outbound**, end of frame: sample the local ped/vehicle, push a snapshot to
   the outbound queue (rate-limited to 25 Hz; the frame rate is not the send
   rate).

Both queues are SPSC between the game thread and the ENet thread. No engine call
happens off the game thread (§1.1).

Offsets for these hook sites are not in this document, on purpose: they have to
come from the retail 1.0 binary, with re3 used only as the map. They live in
`client/src/game/addresses.h`, each one with its proof next to it.

---

## 6. Open questions

1. **Ped AI suppression.** A remote `CPed` must not run its own AI. Options:
   park `m_nPedState` in a state past `PED_STATES_NO_AI` (`Ped.h:260`), or skip
   `ProcessControl` for owned-remote peds. Needs testing. It's the detail most
   likely to cause "remote players wander off on their own".
2. **Vehicle authority handoff** when a driver exits, or when two clients both
   think they own a car. Probably a server-assigned owner with a grace period.
3. **Streaming divergence.** Client A may have a model resident that client B
   doesn't. Does the server track per-client residency, or does each client
   force-load player models and eat the memory? (`CStreaming::RequestModel` with
   a "don't remove" flag is likely the pragmatic answer.)
4. Whether to sync `CControllerState` (`src/core/Pad.h:18-26`) instead of
   resulting state for vehicles. Input sync gives better-looking driving but
   needs determinism we probably don't have (§2.1). Revisit after v1 driving
   feels wrong.
