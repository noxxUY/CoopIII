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

Consequence: a replayed shot never damages anybody. It is a muzzle flash, a
report, a decal and a projectile, and nothing else. What makes a shot hurt is
§1.10, where the shooter's machine intercepts its own hit and sends it. Doing
it the other way, by simply letting the replayed shot damage the local player,
would mean the question "did A hit B" is answered by B's machine, using A's
position interpolated 100 ms late. People would be shot around corners.

The flamethrower used to be on that list, and the reason it was there still
stands: `CWeapon::FireAreaEffect` hands the shot to `CShotInfo`, whose slot
lives for the weapon's `m_fLifespan` and keeps lighting fires every frame
until it expires, long after the replay call returned. What changed is that
the guard is no longer the call. §1.10.1's detour refuses anything a remote
player's ped tries to take off the local player's health, which holds for the
whole life of the `CShotInfo` and every `CFire` it starts, because
`CFire::ProcessFire` passes its `m_pSource` straight into `InflictDamage`
(`src/core/Fire.cpp:70-80`) and that source is the remote ped.

So the flame comes out and nobody here decides who burns. Syncing the fires
themselves is `docs/roadmap.md` §5.7, and until it exists burning is cosmetic
on an observer and damaging only on the machine that lit it.

Explosions are the exception, and not an inconsistency. §1.9.3's explosion is
replayed at a fixed world position that its owner chose, so "is B standing in
it" is a question about B, answered on B's machine, with no stale data
involved. Same rule, not a relaxation of it: nobody decides another player's
damage, and a grenade hurts where a bullet doesn't because a grenade's
authority is a *place* and a bullet's is a *ray* from somewhere only the shooter
knows. Remote peds are therefore `bExplosionProof`; an observer must not apply
the blast to *them*.

Three weapons are deliberately not replayed:

| Weapon | Why not |
|---|---|
| `SNIPERRIFLE` | `CWeapon::FireSniper` returns immediately unless *this* machine's camera is in a first-person mode, and then fires along `TheCamera.Cams[ActiveCam].Front`, the observer's own view. It is unusable for anyone but the local player. |
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

#### 1.9.4 A partial animation ends itself, and an observer has to notice

The overlay in `animId2` is not a state that persists until it is replaced. It
is an animation with an end.

Every weapon animation is declared `ASSOC_FADEOUTWHENDONE | ASSOC_PARTIAL` in
`CAnimManager`'s own `ms_aAnimAssocDefinitions` table
(`src/animation/AnimManager.cpp:72-78`), and `CAnimBlendAssociation::Init`
copies those flags onto every copy made from the group
(`src/animation/AnimBlendAssociation.cpp:85-98`). So when one cycle finishes,
`UpdateTime` sets `blendDelta = -4.0f` and `ASSOC_DELETEFADEDOUT`
(`AnimBlendAssociation.cpp:159-175`) and the association is deleted a quarter
of a second later.

On the shooter's own machine that is fine: `CPed::FireGun` blends it again for
the next round, and only when it is missing
(`src/peds/PedFight.cpp:563-590`). An observer has no `FireGun`. The `animId2`
on the wire stays the same for as long as the trigger is held, so a receiver
that only re-blends when the id *changes* plays exactly one cycle and then
nothing at all, for the rest of the burst.

`CPed::SetMoveAnim` ends overlays the same way from the other direction: on any
change of move state to walk, run or sprint it gives every partial that is
*not* `ASSOC_FADEOUTWHENDONE` a `blendDelta` of `-2.0f` plus
`ASSOC_DELETEFADEDOUT`, and calls `ClearAimFlag` and `ClearLookFlag`
(`src/peds/Ped.cpp:526-540`). Weapon animations are spared by their flag;
knockdowns and punches are not.

Consequence: the receiver's test is "is the overlay still playing", not "has
the id changed". An association with `ASSOC_DELETEFADEDOUT` and a negative
`blendDelta` has been condemned by one of those two routes and gets blended
again. `CAnimManager::BlendAnimation` revives an association it finds rather
than making a second one, by recomputing the delta as
`(1 - blendAmount) * delta` (`AnimManager.cpp:744-747`).

The aim flag recovers on its own, because `SetAimFlag` is driven every frame
the sender reports `PF_AIMING`.

That was necessary and it was not sufficient, and the reason is the next
section.

#### 1.9.4.1 A weapon has one animation, not three

This is the part that looked wrong on screen: a remote player firing showed
the gun being *drawn*, cut off, drawn again, over and over, and never the
firing part.

The draw, the ready pose, the shot and the recovery are all frames of a single
association, `ANIM_STD_WEAPON_HGUN_BODY` and its siblings. Which part you see
is decided by two things, and the wire used to carry neither:

- **whether it is running.** `CPed::PointGunAt` parks the association on
  `m_fAnimLoopStart` and clears `ASSOC_RUNNING` (`PedFight.cpp:194-210`).
  That frozen frame *is* the aim: gun up, ready, not moving. A new
  association, on the other hand, is created running, because
  `CAnimManager::AddAnimation` ends in `Start(0.0f)` for anything that is not
  a movement animation (`AnimManager.cpp:675-698`). So a receiver that copies
  only the id and the phase creates a running copy of a frozen pose. It plays
  from the ready frame to the end of the animation, `ASSOC_FADEOUTWHENDONE`
  deletes it, §1.9.4 revives it, and round it goes.
- **where it loops.** While the trigger is held, `CPed::FireGun` wraps the
  playhead back to `m_fAnimLoopStart` the moment it passes `m_fAnimLoopEnd`
  (`PedFight.cpp:712-722`). A firing weapon cycles the middle of its
  animation and never reaches the end at all.

Consequence, and the test for it is not a theory but a comparison: **a remote
player firing has to look the way your own player looks firing the same
weapon.** So `PF_ANIM2_RUNNING` carries `ASSOC_RUNNING` in a spare bit of the
flags byte that was already on the wire, and the receiver replicates the loop
out of the weapon's own `CWeaponInfo` rather than re-seeding the phase off a
snapshot that is only accurate 25 times a second.

A frozen overlay never advances, so it can never reach the end, never be
condemned and never restart. A running one loops where its owner's does.

#### 1.9.5 `PF_FIRING` is `bIsShooting`, and `bIsShooting` draws nothing

Worth writing down, because the name suggests otherwise. `CPed::bIsShooting`
is written in exactly one place in the engine, the tail of `CWeapon::Fire`
(`src/weapons/Weapon.cpp:260`), and read in four, all of them script opcodes
(`src/control/Script3.cpp:1701,1717,1872,1880`). It is not what holds a ped in
a firing posture and it is not what plays the firing animation.

The flag that does that is `bIsAttacking`, and an observer must not set it:
`CPed::FireGun` acts on it by actually discharging the weapon
(`PedFight.cpp:536`), which is the shooter's decision and already arrives as
`C_Shot`.

So `PF_FIRING` is mirrored onto the remote ped because a co-op mission script
asking "is that character shooting" should get the same answer everywhere, and
for no other reason. What makes a remote player look like they are shooting is
§1.9.4.

#### 1.9.6 Ammunition is not synced

`CPed::GiveWeapon` gives a remote ped a large fixed amount and the replay resets
that slot to `WEAPONSTATE_READY` before each shot. A remote player's clip is not
a thing anyone can see; what it *can* do is make `CWeapon::Fire` refuse
(`if (m_nAmmoInClip <= 0) return false`) and silently stop rendering their
shots. Syncing the real count would buy nothing and add a failure mode.

### 1.10 Damage, death and respawn: the attacker decides the hit, the victim decides the health

§1.9 is what a shot looks like. This is what it does.

The split is one sentence: **the attacker's machine decides that a hit
happened, the victim's machine decides what it costs.** Everything else here
follows from it.

Neither half is arbitrary. The attacker fired a ray from a position only they
have, at an instant only they have, along an aim only they have. Ask the victim
to work out whether they were hit and they are doing it from a ped interpolated
100 ms into the past, which is the textbook recipe for being shot around
corners. But the health is the victim's: nobody else has their armour, their
`m_bCanBeDamaged`, or the 0.33 multiplier `CPed::InflictDamage` applies to a
player and to nothing else, and two machines subtracting from one health pool
disagree within seconds.

#### 1.10.1 One seam, and it is `CPed::InflictDamage`

Everything in the engine that hurts a ped ends up in
`CPed::InflictDamage(damagedBy, method, damage, pedPiece, direction)`
(`src/peds/PedFight.cpp:2034`). Bullets, fists, fire, explosions, cars,
drowning, falling. CoopIII detours it, and that one detour does both jobs:

- If the victim is another player's ped, the call is **refused**. Nothing on
  this machine may change the health of a player this machine does not own.
- If it was refused and the attacker was the local player ped and the cause is
  one the attacker legitimately owns, the same arguments go out as `C_Damage`
  instead.

The victim's machine receives `S_Damage` and calls the same function on its own
player with the arguments off the wire. Armour, the player multiplier, the
`ANIM_STD_HIGHIMPACT_*` reaction, the limb that comes off and the death all
happen in the place single player puts them, because they happen in the
function single player uses.

Which causes get forwarded, and why the rest do not:

| Cause | Forwarded | Why |
|---|---|---|
| `UNARMED`, `BASEBALLBAT`, `COLT45`, `UZI`, `SHOTGUN`, `AK47`, `M16`, `SNIPERRIFLE`, `UZI_DRIVEBY` | yes | a ray or a melee reach the attacker's machine resolved |
| `ROCKETLAUNCHER`, `MOLOTOV`, `GRENADE`, `EXPLOSION` | no | already handled, and handled better, by §1.9.3. A blast is replayed at a fixed world position, so "was I in it" is a question about the victim, answered on the victim's machine with nothing stale in it. Forwarding it as well would apply it twice |
| `FLAMETHROWER` | no | `CShotInfo` keeps damaging for as long as the shot lives, so one trigger pull becomes a stream of packets, and the victim would burn with no flame on screen because the flamethrower is not replayed either |
| `RAMMEDBYCAR`, `RUNOVERBYCAR` | no | a remote ped is teleported 25 times a second, which is not a motion any collision test was written for, and `CPed::KillPedWithCar`'s hit is a flat 1000 |
| `DROWNING`, `FALL` | no | these happen to a player on their own machine, where they are already handled correctly |

`SNIPERRIFLE` is on the list even though §1.9.2 refuses to *replay* a sniper
shot. The two questions are not the same one. The replay is refused because
`CWeapon::FireSniper` fires along the observer's own camera; the damage was
resolved on the shooter's machine like any other bullet. A sniper that hurts
without a visible flash is a cosmetic gap, not a missing weapon.

#### 1.10.2 The proof flags stay, and why they were never enough

Remote peds are still `bBulletProof`, `bFireProof`, `bCollisionProof`,
`bMeleeProof` and `bExplosionProof` (§1.9.2, Area B). They are a backstop now
rather than the mechanism: a detour that failed to install has to fail closed,
and §1.9.2's explosion rule genuinely depends on `bExplosionProof`.

They could never have been the mechanism on their own. `InflictDamage` checks a
proof flag inside a `switch` on the damage cause, and two arms of that switch
check nothing at all:

- `WEAPONTYPE_DROWNING` has its own arm with no flag test
  (`PedFight.cpp:2355`), and the `!bUsesCollision` early return is explicitly
  waived for it. So a remote ped standing in water on an observer's machine
  drowns locally, stays a corpse for everyone watching that screen, and no
  amount of health off the wire brings it back, because nothing off the wire
  resets `m_nPedState`.
- the `default:` arm covers everything else, including `UZI_DRIVEBY`.

Stating the rule once, in the detour, closes both. That is the actual fix; the
flags are the belt.

#### 1.10.3 Friendly fire is the server's, except for the one kind it never sees

`docs/roadmap.md` §5.2: server-configurable, off by default. Off means the
server does not relay `C_Damage` between players at all, so no client is ever
asked to hurt itself on another's behalf. `server.exe [port] [-friendlyfire]`.

The exception is the explosion. A blast never passes through the server as
damage: it is replayed locally from `S_Explosion` and this machine's own engine
decides whether the local player is standing in it. So the client has to be the
one that declines, which is why `S_Welcome` carries `SESSION_FRIENDLY_FIRE` and
why the observer flips its own player `bExplosionProof` for the length of the
replay, the same flip §1.9.2 uses for bullets.

Known gap, stated rather than papered over: the *fire* a molotov leaves behind
is a `CFire` with no owner, and it burns whoever walks into it regardless. That
is treated as terrain, not as an attack. Friendly fire governs what one player
aims at another.

#### 1.10.4 A death is announced by the player who died

`S_Death` existed from version 5 with no `C_Death` beside it, which implied the
server worked out who died. It cannot: a player's health lives on their own
machine and nowhere else. Version 7 adds `C_DEATH` (0x29) and the server relays
it.

The dying machine notices in two independent ways, and they share one
"already announced" flag:

- a detour on `CPed::SetDie`, which carries the `AnimationId` the engine chose
  for *this* death. A headshot, a drowning and a car knocking you over are
  three different animations and an observer has no way to derive which;
- the sampled health crossing to zero, which works when that detour failed to
  install and sends `ANIM_NONE` instead.

The snapshot cannot carry it, and this is worth knowing before anyone tries:
a death animation is created with `blendAmount` 0 and does not become the
clump's dominant association for several frames, by which point the player has
been on the floor with nobody told.

`killerNetId` is recency, not proof. Whoever last damaged the sender, if it was
within five seconds. A player who shot you and then watched you drown does not
get the kill.

Observers call `CPed::SetDie` on that player's ped with the animation off the
wire. Two consequences to respect:

- the ped is **taken out of its seat first**. `SetDie`'s `PED_DRIVING` arm
  calls `FlagToDestroyWhenNextProcessed` on anything that is not the player
  ped, and every remote player is a `CCivilianPed`, so killing a seated one
  hands it to the engine to delete;
- the pose stream stops driving a corpse. Position still comes off the wire,
  because the owner's ped is still falling over and its snapshots say where it
  lands, but the animation, the weapon, the aim and the move state do not.
  Re-blending the walk the snapshot still names over a death animation is how
  a corpse stands back up.

#### 1.10.5 A respawn rebuilds the ped

On the owner's machine a respawn is a teleport and a health reset:
`CGameLogic::RestorePlayerStuffDuringResurrection`
(`src/control/GameLogic.cpp:282`) resurrects the same `CPed` rather than making
a new one.

On every other machine it is not, because what they have is a corpse.
`CPed::SetDie` zeroed the health, cleared the collision through `ClearAll` and
handed the clump a death animation, and there is no undo for any of that. So
`S_Respawn` destroys the ped and the ordinary two-phase spawn (§1.6) builds a
new one.

Destroying a ped is the one thing in CoopIII that has to agree with the engine
about every list the engine is holding it in, and the moving list is the one
that bites. `CWorld::Remove`, which `~CPed` calls first, only unlinks an entity
from `CWorld::ms_listMovingEntityPtrs` when `bIsStatic` is clear
(`src/core/World.cpp:91`), and a remote ped whose owner stood still for ten
frames is static. So the teardown does that unlink by hand first;
`client/src/game/addresses.h` carries the disassembly and the predicate, and
`tools/clienttest` pins the truth table.

The transform is on the wire because the two ends of a respawn are half a city
apart. A ped rebuilt from the snapshot stream alone would be born where its
owner died and then snap to the hospital once the interpolation buffer caught
up, so the buffer is cleared and `last` is seeded from the packet. The ped
reappears about a snapshot plus an interpolation delay later, which nobody
notices against a four second death fade.

v1 stops there. The corpse is not left lying around to be walked over, there is
no wasted message for other players, and no kill feed. `killerNetId` is carried
and logged by the server so a scoreboard has something to read when there is
one.

---

## 2. Design decisions

### 2.1 Topology: dedicated server, client-authoritative players

Each client is authoritative over (a) its own ped and (b) the vehicle it is
currently driving. One designated client, the host, is additionally
authoritative over the time of day and the weather (§2.7). The server relays
all of it and assigns ownership for everything else.

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
| World state (clock, weather) | 1 Hz, host → server → everyone | `CClock`/`CWeather` update per frame but change slowly (`Game.cpp:1031-1032`), and a game minute is a real second, so anything faster is repetition (§2.7) |
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

### 2.7 Time of day and weather belong to the host, not to the server

The session's clock is one player's `CClock`. That player is the host: the
first one to connect, and on their way out the lowest-numbered player still
in. Everyone else, and only everyone else, is corrected toward it.

**Why not keep the clock on the server.** The server has no GTA III running,
so a clock it keeps is a clock nobody is playing to. Three things follow
from that and none of them have a workaround:

1. The campaign moves the clock. `SET_TIME_OF_DAY` (opcode 192) calls
   `CClock::SetGameClock` outright, and `FORCE_WEATHER` / `FORCE_WEATHER_NOW`
   / `RELEASE_WEATHER` (437-439) do the same to the sky. `campaign.md` §2
   already puts the script on the host, so the host's engine is where those
   opcodes land. A server-authoritative clock would drag the time back off
   whatever the mission just set it to, once a second, for the whole mission.
2. `CClock` is driven by `CTimer::GetTimeInMilliseconds()`, which stops on a
   loading screen and is scaled by `ms_fTimeScale` (§1.2). A server counting
   real milliseconds is not modelling that, it is only approximating it, and
   the approximation drifts in the one direction nothing corrects.
3. It is what was asked for. The time everyone sees should be the time the
   person running the session sees.

The cost is a handover when the host quits, which is one byte on a packet
that was already being sent.

A host that never reports leaves the session on the server's stand-in clock,
which is the old behaviour rather than a broken one. Worth knowing when
testing: `tools/ghost` has no `CClock`, so if it connects before any real
client it becomes the host and nobody's time of day gets followed. Start the
game first.

**Corrections are a jump, and the tolerance is what stops them.** A world
packet is at least a round trip old and the host reports once a second, so a
client that matched exactly would be told to step its clock every second, and
GTA III hangs the HUD clock, the sun's position and the streetlights off the
minute. So a client only moves once it is more than three game minutes out
(`kClockToleranceMinutes` in `client/src/client.h`). Three game minutes is
three real seconds, which nothing on screen can show; everything that
genuinely matters - joining at a different hour, a loading screen, a mission
setting the time - is hours out and gets fixed in one step. Two synced games
then stay synced on their own, because `CClock` runs at the same rate on
every machine.

**Weather is a pair, not a type.** `CWeather` blends from `OldWeatherType` to
`NewWeatherType` across a game hour, so one type says where the sky is going
and not where it is. Both go on the wire. The position within the blend does
not: `CWeather::Update` recomputes it as `CClock::GetMinutes() / 60` at the
top of every frame, so once the clock agrees the blend agrees for free.
Receivers also pin `ForcedWeatherType`, without which the local rotation
picks its own next type at the hour boundary and shows the wrong sky for the
second before the next packet arrives.

---

## 3. v1 sync scope

In:

- On-foot player: position, heading, move state, animation, health, armour
- Weapons: current weapon, aim direction, shot events
- Vehicles: the driver's vehicle (transform, steer/gas/brake, health, engine)
- Enter/exit/carjack, using the engine's own API so animations play correctly:
  `SetEnterCar` (`Ped.h:701`), `SetExitCar` (703), `SetCarJack` (711),
  `SetPedPositionInCar` (565)
- Damage/death/respawn: `InflictDamage` (612), `SetDie` (545), `SetDead` (546).
  The design is §1.10
- Clock + weather, taken from the host's own game (§2.7)
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
| 0x02 | `S_WELCOME` | 1 | your `playerId`, your `netId`, server tick rate, world state, `hostPlayerId` (§2.7), session flags (§1.10.3) |
| 0x03 | `S_PLAYER_JOIN` | 1 | `playerId`, `netId`, nickname, model id, spawn transform |
| 0x04 | `S_PLAYER_LEAVE` | 1 | `playerId`, reason |
| 0x10 | `C_PLAYER_STATE` | 0 | pos `float[3]`, heading `float`, `m_vecMoveSpeed` `float[3]`, `m_nMoveState` `u8`, `m_nPedState` `u8`, anim `u16` + time `float` + speed `float`, partial anim `u16` + time `float`, health `float`, armour `float`, weapon `u8`, aim yaw/pitch `float[2]`, flags `u8` (64 bytes) |
| 0x11 | `S_PLAYER_STATE` | 0 | `playerId` + the above |
| 0x12 | `C_VEHICLE_STATE` | 0 | `netId`, pos `float[3]`, quat `float[4]`, move/turn speed `float[6]`, steer/gas/brake `float[3]`, gear `u8`, health `float`, flags `u8` |
| 0x13 | `S_VEHICLE_STATE` | 0 | same, relayed |
| 0x20 | `C_SHOT` / 0x21 `S_SHOT` | 1 | weapon `u8`, origin `float[3]`, direction `float[3]`, speed `float` (§1.9.1) |
| 0x22 | `C_DAMAGE` / 0x23 `S_DAMAGE` | 1 | victim `netId`, weapon `u8`, amount `float`, piece `u8`, direction `u8` (§1.10.1). `S_DAMAGE` goes to the victim alone, not broadcast |
| 0x24 | `S_DEATH` | 1 | `playerId`, killer `netId`, anim `u16` |
| 0x25 | `C_RESPAWN` / 0x26 `S_RESPAWN` | 1 | spawn transform (§1.10.5) |
| 0x27 | `C_EXPLOSION` / 0x28 `S_EXPLOSION` | 1 | type `u8`, pos `float[3]` (§1.9.3) |
| 0x29 | `C_DEATH` | 1 | killer `netId`, anim `u16` (§1.10.4). Out of order because the block above was numbered before it was clear who announces a death |
| 0x30 | `C_ENTER_VEHICLE` / 0x31 `S_ENTER_VEHICLE` | 1 | `netId`, seat `u8`, jack `bool` |
| 0x32 | `C_EXIT_VEHICLE` / 0x33 `S_EXIT_VEHICLE` | 1 | `netId` |
| 0x34 | `S_VEHICLE_SPAWN` | 1 | `netId`, model id, transform, colours |
| 0x35 | `S_VEHICLE_DESPAWN` | 1 | `netId` |
| 0x40 | `S_WORLD_STATE` | 1 | game hour/minute, both weather types, `hostPlayerId` (§2.7) |
| 0x41 | `C_WORLD_STATE` | 1 | the host's own hour/minute and weather pair; dropped from anyone else |
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
