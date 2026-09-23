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

#### 1.8.1.1 And a clump holds twelve animations, not as many as you like

The second unchecked bound in the same subsystem, found the same way, and the
one that actually crashed the game.

`RpAnimBlendClumpUpdateAnimations` builds an array of the nodes it is about to
blend, in a local, and neither fills it nor terminates it with a bound. The
retail frame has room for twelve; past that the array runs into the
function's own saved registers, its return address and its arguments.
`client/src/game/addresses.h` carries the stack map and the register dump that
proved it.

Two things make this worth stating in the protocol document rather than only
in the code. The first is that **re3 declares the array as sixteen**
(`src/animation/RpAnimBlend.h:8-12`) and the retail build has twelve, so the
decompilation will actively mislead anyone who sizes a cap from it. The second
is that `CWorld::Process` keeps its moving-list cursor in `edi` across this
call, and saved `edi` is inside the overflow region, so an overfull clump
corrupts a list walk that has nothing to do with animation.

Consequence: anything that adds an animation to a ped CoopIII owns counts
first. `MAX_REMOTE_ANIM_ASSOCS` in `client/src/game/pedanim.h` keeps two slots
back for the animations the engine blends on its own, and a per-frame sweep
drops the least visible ones if the count ever goes over.

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

**Update (§1.9.7).** The shot vector no longer waits for this. A replayed shot
is aimed by correcting the *target* the fire path is about to trace, not by
aiming the ped, so a remote player's bullets go up and down correctly while
their arms stay level. What is left for an `AimGun` detour is the arms
themselves, which is now a cosmetic mismatch on the ped rather than a wrong
bullet.

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

So the flame comes out and nobody here decides who burns *for anybody else*.
Since §5.7's phase two the fire that flame lights does burn the person
standing in it, and that is not a relaxation of this rule either: §1.10.6 is
the argument, and it is the explosion argument below with "place" in place of
"ray".

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

**The position in that first bullet is not optional, and for a while it was
not written.** The code corrected the velocity and left the position where the
engine put it, on the reasoning that `CProjectileInfo::AddProjectile` derives
it from the thrower's matrix and the pose stream keeps that within half a metre
of the truth. That is true of a grenade and a molotov and false of a rocket.

`AddProjectile` (`0x0055B030`) builds a `CMatrix` per weapon and assigns it to
the new `CProjectile` whole — rotation *and* translation — so whatever is in
its position field is where the projectile is born. Three of the four arms put
the `pos` argument there:

| arm | where it adds `pos` |
|---|---|
| grenade | `0x0055B11C`, three `fld` / `fadd` / `fstp` |
| molotov | `0x0055B25B`, the same three |
| rocket, thrown by a player | `0x0055B3BC`, copied in whole after the camera basis |
| rocket, thrown at a seek target | `0x0055B471`, `+= pos` |

The fourth does not. A rocket from a ped that is neither the player nor
chasing anybody is `matrix = ped->GetMatrix()` (`0x0055B4A6`) and then straight
on to the velocity; `pos` is never read. The rocket is created **at the ped's
own origin**, hip height inside their collision, and the fire source is thrown
away.

Every remote player is a `CCivilianPed` with no seek target, so that fourth arm
is the one every replayed rocket takes — while on its owner's machine the same
rocket takes the first and starts a metre out in front of them. Retail GTA III
has no NPC who fires a rocket launcher, so nothing else in the game has ever
run that arm, which is why it is wrong.

Left uncorrected, the missile is then removed within a frame or two and in
silence:

- `CProjectileInfo::Update` (`0x0055B7C0`) removes a rocket outright if
  `bHasCollided` is set (`0x0055B89E`, byte B bit 3), which is what a `CObject`
  born inside a ped's collision gets on its first physics step;
- and it sweeps a line from `m_vecPos` to the projectile's current position
  every frame and removes anything whose sweep is not clear. The flags it
  passes at `0x0055B8B5` are buildings, vehicles, **peds**, objects, and three
  zeroes, and the only thing `CWorld::pIgnoreEntity` holds for that sweep is
  the projectile itself — never the ped that threw it;
- and the `RemoveProjectile` detour above blanks the weapon type on the way
  past, so that removal makes no explosion and no sound.

The owner's `C_Explosion` still arrives a second later and still plays in the
right street. From the outside: the blast syncs, the rocket never flies.

So the observer writes the position as well as the velocity, updates
`CProjectileInfo::m_vecPos` to match (otherwise the first sweep runs from
inside the thrower back out through them), and re-files the object —
`CMatrix::UpdateRW`, `CEntity::UpdateRwFrame`, `CPhysical::RemoveAndAdd`, the
same three `ped.cpp`'s `PlaceRemotePed` needs for the same reasons. The
rotation is rebuilt from `dir` too, the way `AddProjectile`'s own player arm
builds one from the camera, so the missile points where it is going instead of
along the ped's flat heading.

Grenades and molotovs were never affected by any of this. They take the two
arms that add `pos`, and they are on a full `CProjectileInfo` life with
gravity, a bounce and a two-second fuse rather than a one-second sprint
through a line sweep.

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

#### 1.9.6 Ammunition travels, behind a server switch

**This section used to say the opposite.** It said a remote player's clip is
not a thing anyone can see, so syncing it "would buy nothing and add a failure
mode". The first half is wrong and the second half is the thing to design
around rather than a reason not to.

What was wrong: `CPed::GiveWeapon` handed every remote ped an invented 1000
rounds, so nobody ever watched anybody else run dry, nobody ever saw a reload,
and the HUD-level truth of a firefight - who is nearly out, who has to back
off - was a number CoopIII made up. It is visible in the only way that matters
in co-op, which is that you cannot tell when the person covering you is about
to stop covering you.

**Which of the two numbers.** `CWeapon` has both, and they do different jobs.
Everything here is read out of the retail 1.0 binary, not out of re3:

| | |
|---|---|
| `m_nAmmoInClip` (`+0x08`) | **Gates firing.** `CWeapon::Fire` opens `cmp dword [edi+8],0 / jg` at `0x0055C4A2` and returns false on an empty clip, before anything is drawn. It decrements it at `0x0055C7D1`. |
| `m_nAmmoTotal` (`+0x0C`) | **What the game reports.** `CHud::Draw` loads it at `0x00506052` and prints either `"%d"` or `"%d-%d"` depending on the weapon's `m_nAmountofAmmunition` (`CWeaponInfo +0x10`, tested `<= 1 \|\| >= 1000` at `0x00506063`/`0x0050606C`). `GET_AMMO_IN_CHAR_WEAPON` (opcode 1050, handler `0x00588F22`) answers with `m_nAmmoTotal` and nothing else. `CWeapon::Fire` decrements it too, at `0x0055C7E9`, for anyone whose total is under 25000 or who is the local player. |

So both go on the wire. Sending one and deriving the other is a guess:
`CWeapon::Reload` (`0x005639D0`) fills the clip out of the total, capped at
`m_nAmountofAmmunition`, on a `CTimer` deadline that means nothing on another
machine.

**Where each one rides.** The weapon in the player's hands is the one that
changes every time a trigger is pulled, so its two counts are six more bytes
on `PlayerStateBody` (65 to 71). At 25 Hz across eight players that is about
1 KB/s for the whole session, which §2.3's budget has room for, and a dropped
packet is corrected 40 ms later - the same argument `PF_ON_FIRE` makes.

The other twelve slots change when somebody walks over a pickup or a mission
grants a weapon, which is minutes apart. They go out on `C_PlayerAmmo` /
`S_PlayerAmmo` (0xB0/0xB1), one slot per packet, reliable, **on change only** -
the shape `C_PlayerModel` settled on. Each one carries an `AMMO_SLOT_OWNED`
bit, because "I do not have this weapon" and "I have it with nothing in it"
are different facts and the engine keeps them apart the same way: `HasWeapon`
is `m_weapons[w].m_eWeaponType == w`, the comparison `CPed::GiveWeapon` makes
at `0x004CF9C9`. Collapse the two and an observer calls `CPed::GiveWeapon`
thirteen times and hands every remote player the whole armoury, empty. Putting all thirteen in the snapshot
would be 91 bytes per player per tick, more than the entire rest of the
snapshot, to restate numbers nobody touched. Putting the *held* slot on the
reliable channel instead would be ten ordered packets a second out of an Uzi,
queued in front of the shots and the damage that actually need the ordering.

The held slot is deliberately never sent as a `C_PlayerAmmo`. Both ends mirror
the snapshot's count into their per-slot table, so the table is complete
whichever hand the number arrived in, and a weapon switch costs no packet.

**Who wins, and the trap.** An observer's copy of somebody else's ammunition
has three writers and only one of them is entitled to it:

1. the wire, which is the owner's own engine;
2. `CWeapon::Fire`, every time §1.9.2 replays a shot - it spends a round out
   of the observer's copy, from a magazine the observer does not own;
3. `CWeapon::Update`, which `CCivilianPed::ProcessControl` runs on the held
   slot every frame, reloading on its own `CTimer` schedule.

The wire wins. The owner is authoritative for their own ped everywhere else in
CoopIII (§2.1, §1.10) and there is no reason for ammunition to be the
exception. Concretely: the held slot is rewritten from the newest snapshot
every time a pose is applied, so nothing local can accumulate for more than
40 ms, and a replayed shot puts back exactly what it spent.

**And a replayed shot must never be refused.** This is the trap, and it is
worse than the bug ammo sync exists to fix because it is silent. `CWeapon::Fire`
returns false on an empty clip *and* while the slot is `WEAPONSTATE_RELOADING`
or `WEAPONSTATE_OUT_OF_AMMO` - and its own tail, at `0x0055C80D` onward, is
what puts the slot into those states. If the observer's copy were allowed to
disagree with the owner, remote players would stop shooting on your screen
while they were still shooting on theirs, and only in a long firefight. So the
slot is forced into a state `Fire` cannot refuse for the length of that one
call, and the owner's number is written back immediately afterwards.

`WEAPONSTATE_RELOADING` is not synced and should not be: it is a deadline in
`CTimer::GetTimeInMilliseconds`, a clock that pauses and gets rescaled (§1.2).
The observer derives the state instead - nothing behind the gun is
`WEAPONSTATE_OUT_OF_AMMO`, anything else is `WEAPONSTATE_READY`.

**The switch.** `SESSION_AMMO_SYNC`, bit 1 of `S_Welcome.flags`, next to
friendly fire and enforced the same way in both places: the server refuses to
relay a `C_PlayerAmmo` with the bit clear, and a client with the bit clear
ignores the snapshot's two ammo fields and goes on handing a remote ped the
fixed amount. Off is the default and off is *exactly* the old behaviour, not a
degraded version of it.

It is **not a shared inventory**, and that distinction is the whole request:
two players carrying different weapons is the normal case and nothing here
changes it. What the switch decides is whether each player's own counts are
honest on everybody else's screen.

#### 1.9.7 The direction goes on the wire, and the observer aims with it

Reported from a live two-client session: both machines draw a bullet trail and
they draw it in different places. The shooter's goes one way, the observer's is
displaced.

**It is not the resolution**, which was the first guess and is ruled out by
what a trail is. `CBulletTraces::AddTrace` (`0x00518E90`) stores two
world-space `CVector`s; `CBulletTrace::Update` walks the near end toward the
far one by 0.8 m a frame; `CBulletTraces::Render` builds the quad from those
two points and takes its *width* from
`CrossProduct(TheCamera.GetForward(), sup - inf)` normalised and divided by 20
— a fixed 5 cm half-width in metres. Nothing in the path reads a screen
dimension. A different resolution draws the same segment thinner, not
elsewhere.

**It is the direction, and until now the direction was not on the wire in any
usable sense.** `ShotBody` carried `dir`, and §1.9's own note said the receiver
did not aim with it. The receiver replayed through `CWeapon::Fire`, and for
every ped that is not the local player `CWeapon::FireInstantHit` takes its
fourth branch, which does two things (`client/src/game/addresses.h` carries
both instructions):

1. derives the shot's yaw from the **ped's own matrix forward**, flattened to
   2D — `Atan2(-fwd.x, fwd.y)` at `0x0055D9AC`, whose sin/cos back out to
   exactly `normalise2D(fwd)`. Not the aim: the body. And the body is an
   interpolated copy of a ped that was somewhere else 100 ms ago;
2. copies `target.z` from `source.z` as a **dword move** (`0x0055DA47` loads
   it, `0x0055DA6F` stores it). The shot is flat. A player firing up at a
   rooftop or down off one draws a horizontal tracer on every screen except
   their own.

Then `CWeapon::DoDoomAiming` puts a slope back on it — aimed at whatever ped
*the observer's* machine finds near the shooter. That is an observer deciding
where somebody else's bullet goes, which is the same rule §1.10 applies to
damage, broken one layer down.

So:

**Sampling.** `dir` is the unit direction of the streak the shooter's own
screen drew, read from `CBulletTraces::AddTrace` (`0x00518E90`) while the local
player's `CWeapon::Fire` is running. A shotgun draws one per pellet, and the
unit directions are averaged, so `dir` is the middle of the cone rather than
whichever pellet flew furthest.

It was, for one round, the direction of the line the engine *traced*, read from
`CWeapon::ProcessLineOfSight` (`0x00564C00`). That is still the fallback, and
for a weapon that traces without drawing — `CWeapon::FireM16_1stPerson` — it is
the only answer there is. But it is the wrong one whenever both exist, and
§1.9.7.1 is why.

**Applying.** The observer turns the engine's own proposal onto that line
inside `CWeapon::DoDoomAiming` (`0x00562EB0`), which is handed `target` as a
pointer and whose entire purpose in retail is to move a shot that has been
aimed and not yet traced. It is a *rotation*, not a replacement, so the
shotgun's five-pellet spread survives and moves with the cone. The original is
not called for a replayed shot: its auto-aim is this machine's opinion about
somebody else's bullet.

Because the target is corrected before `ProcessLineOfSight` runs, everything
downstream follows one ray — the trail, the impact decal, the sparks, the
glass. A remote ped's `m_pPointGunAt` is cleared for the length of the call and
put back afterwards, because a ped with a gun target takes the *first* branch
and never calls `DoDoomAiming` at all.

**No wire layout change, and `PROTOCOL_VERSION` stays 8.** `ShotBody` is the
same 29 bytes with the same fields in the same order. What changed is what gets
written into `dir` for an instant-hit weapon, and the old meaning is exactly
the behaviour this replaced — so a client built before the change still
interoperates, at the old quality.

**What is left over, honestly.**

- ~~*The 3rd-person mouse camera's offset.*~~ This entry used to say the ray
  and the trail "converge with distance" and are "sub-degree" apart. They do
  not converge: they are **parallel**, so the gap is the same at every
  distance, and it is the gap that made the observer's line the wrong line.
  §1.9.7.1 is the correction and the fix.
- *Interpolation.* The origin is on the wire, so a stale ped no longer moves
  the trail at all. What is still stale is the ped the trail comes *out of*:
  the muzzle flash particles are placed at the wire origin, but the hand
  holding the gun is wherever the interpolation has that ped. At 25 Hz a
  sprinting player covers about 25 cm between snapshots, and `InterpBuffer`'s
  100 ms delay is the dominant term — call it a quarter of a metre between the
  drawn hand and the drawn trail, for a player at a dead run. Standing still it
  is zero.
- *Aim pitch on the ped itself.* §1.8.3 still stands: `CPed::AimGun` hard-codes
  zero pitch for anything that is not the local `CPlayerPed`, so a remote
  player's arms stay level while their tracer now goes up or down correctly.
  That is a detour on `AimGun` and it lives with the ped work, not here — the
  shot's geometry no longer depends on it, because the origin comes off the
  wire and the direction is applied to the target rather than to the ped.
- *The flamethrower.* `FireAreaEffect` traces no line and never calls
  `DoDoomAiming`; its flame still comes off the remote ped's heading.
- *Retail's own bug, which is the shooter's half of the report.*
  `FireInstantHit`'s 3rd-person-mouse-camera branch traces through its own
  `src`/`trgt` locals and **never writes the `target` local**, and the shared
  tail passes `&target` to `DoBulletImpact` anyway, whose no-victim arm is
  `AddTrace(source, target)`. So a mouse-aimed shot that hits nothing draws its
  trail to whatever was left in that stack slot — most often a target from an
  earlier shot, which looks exactly like a trail veering off to one side.
  CoopIII repairs it in a `DoBulletImpact` detour, by an *exact* test rather
  than a tolerance: the other three branches hand `&target` to
  `ProcessLineOfSight` themselves, so if the pointer arriving at
  `DoBulletImpact` is not the `point2` the engine just traced, nothing wrote
  it. Strictly cosmetic — the ray had already been traced either way.

#### 1.9.7.1 The ray and the streak are two different lines

Reported again after §1.9.7 shipped, and the second report rules out the first
explanation: *"las balas si van a donde el remote apunta y la animacion de
apuntar se ve bien, pero el otro jugador ve la trayectoria de las balas yendo a
otro lugar."* The bullets land where they should. The **drawn line** does not.

That is not the aim-pitch limitation, because the bullets arrive. It is this:
**§1.9.7 put the direction of the ray on the wire and the position of the
muzzle beside it, and on the branch that matters those two belong to different
lines.**

`CWeapon::FireInstantHit`'s 3rd-person mouse camera branch — the branch a
mouse-aiming player is on for every single shot — does not trace from the
barrel. It asks `CCamera::Find3rdPersonCamTargetVector` for a `src` and a
`trgt`, and that function's mouse arm ends `source += Dot(pos - source, target)
* target`: the muzzle projected onto the camera's own axis. Then:

| what | which point | where in the retail binary |
|---|---|---|
| the ray that is traced | `src`, on the camera axis | `ProcessLineOfSight` at `0x0055D907`, point1 is `[esp+0E8h]` |
| the streak that is drawn | `source`, the muzzle | `DoBulletImpact` at `0x0055F73A`, source is `[esp+90h]`, copied from `fireSource` at `0x0055D316` and never written again |

The two lines are **parallel**, offset by however far the hand sits off the
camera axis — tens of centimetres, and the same at every distance, because
parallel lines do not converge. An observer handed the ray's direction and told
to fire from the muzzle draws neither of them: it draws a third line through
the muzzle parallel to the shooter's ray, which at a close impact is several
degrees off the streak the shooter saw.

**The fix is to sample the thing the report is about.** `dir` now comes from
`CBulletTraces::AddTrace` (`0x00518E90`) — the one function in the engine that
is handed the two points of the streak, and the only place all six trail
callers funnel through (two in `DoBulletImpact`, two in `FireShotgun`, two in
`FireInstantHitFromCar`). Its near end *is* the muzzle, which is what the wire
already carries as `origin`, so origin and direction finally describe the same
segment and the observer's replay reconstructs it by construction rather than
by luck.

The sampler runs after the `DoBulletImpact` repair above, so what it measures is
the corrected segment and not the uninitialised one.

`combat.h`'s `ChooseShotDirection` is the whole decision — trail, then ray,
then body — and `clienttest` pins it, including that a trail and a ray from the
same trigger pull really are different lines.

**The client says the number now.** Nobody had ever measured the gap; both
rounds argued about it from the source. The first shot of a session logs where
the ray started, where the streak started, how far apart they are and how many
degrees lie between the two directions. An observer logs the segment it drew
for the first remote shot against the direction that arrived. Two lines in two
logs, and the comparison stops needing two people looking at two screens.

**Still no wire change, `PROTOCOL_VERSION` stays 16.** `ShotBody` is untouched;
only what gets written into `dir` changed, and an older client interoperates at
the old quality.

**What a live run measured, and what it did not.** One game, one server, and
`ghost -shoot -pitch` — which rakes a remote player's shots 40° up and down,
because a flat shot is the one thing an observer's own engine would have
produced for itself and so proves nothing:

```
combat: hooked CBulletTraces::AddTrace at 0x00518E90
combat: pointed our first replayed shot along its owner's own line instead of
        along our copy of their ped. The two were 12.4 degrees apart - their
        line is (-0.97 -0.15 0.21), their ped here is facing (-0.99 -0.11 0.00)
combat: drew a remote player's first bullet trail from (882.34 -305.73 9.32)
        to (855.69 -309.80 15.13), along the (-0.97 -0.15 0.21) their own
        machine sent
```

The drawn segment is `(-26.65, -4.07, 5.81)`, which normalises to
`(-0.966, -0.148, 0.211)`, and its near end is the wire origin to the
centimetre. So the **applying** half is exact: given a `dir`, the observer
draws that `dir`, out of the wire's origin, pitch and all.

The **sampling** half — the change this section is about — was not run in the
game. It only happens when the local player pulls a trigger, the test save
carries no weapon, and synthetic keyboard input cannot reach GTA III's cheat
path (AgentPad writes `CPad` state; `CPad::AddToPCCheatString` is fed by
`RsKeyboardEventHandler`, which is a different door). It is covered by
`clienttest` and by the disassembly above, and the first shot of the next real
session will print its own measurement.

#### 1.9.7.2 It *was* partly the resolution, on the shooter's own screen

The first report guessed the screen resolution and §1.9.7 ruled it out. That
was right about the trail and wrong as a general answer, and the retail binary
is unambiguous about the part that is true.

`Find3rdPersonCamTargetVector` turns the crosshair's screen fraction into two
angles. The vertical one is `(0.5 - multY) * 1.8 * 0.5 * FOV`. The horizontal
one is the same **times an aspect ratio** (`fmul st,st(4)` at `0x0046B646`,
absent from the vertical arm). And that aspect ratio is not the shape of the
window: it is `1.777778` or `1.333333`, chosen by a preference byte at
`0x0095CD23` — the widescreen toggle — at `0x0046B611`.

The crosshair sprite, meanwhile, is drawn at a fixed fraction of the screen,
and in retail that fraction is `0.53` across, not `0.5`. Off centre, which is
the only reason the factor bites at all.

So with the widescreen toggle not matching what is actually being rendered,
every mouse-aimed shot leaves the barrel about **0.8° to one side of the
crosshair**, always the same side: `(0.53 - 0.5) · 1.8 · 0.5 · FOV · (16/9 −
4/3)`, which at a 70° FOV is 15 cm at ten metres. That is "el que dispara ve el
trail de las balas hacia un lado", and it is world space, so every screen draws
the same wrong line and no amount of netcode moves it.

**Measured on this install, not assumed.** The Widescreen Fix in
`modloader/_ESSENTIALS` is exactly the kind of mod that would already have
fixed this, so the client reads the selection site at startup and compares it
against retail's bytes. It said:

```
combat: the game still aims the retail way - a mouse-aimed shot's horizontal
        angle is scaled by 1.7778 or 1.3333 depending on the widescreen toggle
        (currently on), not by the shape of the window.
```

Nothing has patched it, the toggle is **on** (so the game aims as if the screen
were 16:9), and the window that run reported in its own title was **958×1000** —
an aspect of 0.96. `(0.53 − 0.5) · 1.8 · 0.5 · 70 · (1.778 − 0.958)` is about
**1.5°**, roughly 26 cm to one side at ten metres and 80 cm at thirty.

So noxx's first guess was right about something real, just not about the thing
he was looking at: his shots leave the barrel to one side of his own crosshair,
in single player, because the game aims for a screen shape he is not using.

Retail's, not ours. Worth knowing before anybody spends another round on it.

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
| `FLAMETHROWER` | no | never a shot. In retail 1.0 the only producer of this cause is `CFire::ProcessFire`, so it means "a fire is burning somebody", and the fire is already in the victim's world at a position their own engine computed. Deciding it here would decide it twice, and `ProcessFire` hits once per frame, so one trigger pull would be sixty packets a second. §1.10.6 |
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

One exemption, and it is the whole feature. The detour also refuses anything a
remote player's ped names itself the culprit of, which is what lets the
flamethrower be replayed (§1.9.2). `ApplyRemoteDamage` names the attacker's
ped as the culprit on purpose, so the engine's blood and its threat entity
point at the player who did it, and without an exemption that rule refuses the
hits this section exists to deliver. It cost a session: the shooter converted
the hit, the server relayed it, the victim called `InflictDamage`, and the
victim's own detour threw it away, silently, at every step.

So every step says itself once now. Between the shooter's line, the victim's
line and the two refusals, the log answers in one glance whether a hit was
decided, carried, or dropped, and where.

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

#### 1.10.6 Fire is terrain, and terrain is the victim's own business

Nothing about fire goes on the wire, in either direction, and that is the
decision rather than an omission. Fire damage is decided on the victim's
machine, by the victim's own engine, about the victim. (Between players, that
is. A flame reaching somebody else's pedestrian or car sends the ignition to
its owner, and the owner's own fire still decides the damage - §1.24.)

This is the opposite of §1.10.1's rule for a bullet and it is the same
principle, applied to a different question. A bullet's authority is a **ray**,
from a position, at an instant, along an aim that only the shooter has, so a
victim asked to work it out is doing it off a ped 100 ms in the past and people
get shot around corners. A fire's authority is a **place**. It sits still, it
burns for ten seconds, and "is the local player standing in it" is a question
about the local player, answered on the local player's machine, from the local
player's position this frame. That is §1.9.2's explosion argument word for
word, and fire is a better fit for it than the explosion is, because a fire
does not even have to be replayed to be in the right place.

Three reasons it could not have gone the other way:

- **Most fires have nobody to send them.** A car burning out, a script fire,
  the puddle a molotov leaves behind: `m_pSource` is null and there is no
  machine entitled to decide. A rule that only works when a fire has an owner
  is not a rule about fire, and §5.7 is explicit that fire means fire.
- **Fire damage is continuous.** `CFire::ProcessFire` hits for
  `1.2f * CTimer::GetTimeStep()` *every frame* for as long as the fire lives.
  One trigger pull on a flamethrower is sixty packets a second per burning
  player. `WEAPONTYPE_FLAMETHROWER` therefore stays off §1.10.1's forwardable
  list, where it already was, for a better reason than the one written there.
- **The health arithmetic is the victim's anyway** (§1.10). Armour,
  `m_bCanBeDamaged` and the 0.33 multiplier a player gets and nobody else does
  all live on the machine whose player it is.

**How it reaches the victim without reopening the hole.** §1.10.2's rule -
nothing a remote player's ped names itself the culprit of may take health off
the local player - stays exactly as written, with one cause added to the
exemption list it already had. The cause is `WEAPONTYPE_FLAMETHROWER`, and it
is narrow because the binary makes it narrow: of the 21 `call
CPed::InflictDamage` sites in retail 1.0, exactly two push `9`, and both are
inside `CFire::ProcessFire` (`0x0047998D` and `0x004799B0`). There is no other
producer. So that cause arriving at the seam cannot mean "a remote player shot
us"; it means "a `CFire` is burning us, and it remembers who lit it".

The remote ped is never read for the decision. It is carried as the culprit for
the same reason `ApplyRemoteDamage` carries one: so the engine's blood, its
threat entity and `CDarkel`'s kill register point at the player who lit the
fire instead of at nobody.

**Friendly fire, and why `m_pSource` is the right discriminator.** §1.10.3 says
friendly fire is the server's except for the blast it never sees. Fire is now
the second thing it never sees, and the fire's own source byte is what tells an
attack apart from terrain:

| `m_pSource` | Whose fire | Friendly fire |
|---|---|---|
| null | nobody's - an explosion, a burning car, a script fire | ignores it, burns anyone. Unchanged, and already what happened before this |
| a remote player's ped | theirs, lit deliberately | gated, exactly like their bullets and their blast |

That is not a retreat from "the fire a molotov leaves behind is terrain, it
burns whoever walks into it". A molotov's leftover fire is started by
`CFireManager::StartFire(pos, size, propagation)` from inside
`CExplosion::AddExplosion`, which nils `m_pSource`. It is still terrain and it
still burns everyone. What is gated is the flame somebody is pointing at you.

#### 1.10.7 A burning player burns for everyone, and it costs one bit

Fire damage never goes on the wire (§1.10.6). The *fact* of burning does, one
way, from the player who is alight to everyone watching: `PF_ON_FIRE`, a spare
bit in the flags byte `PlayerStateBody` was already sending. No new packet, no
layout change, no version bump. `PROTOCOL_VERSION` stays at **8**.

The sender reads `CPed::m_pFire != nil` and nothing else - not the fire's
position, not its strength, not who lit it. An entity fire has no position of
its own to send: `CFire::ProcessFire` rewrites `m_vecPos` from the entity's
matrix every frame, so on the receiver it is derived from a ped that is
already synced (§5.7). A ped being alight really is just a boolean with a
lifetime, which is exactly what a flags byte is for.

**Why this is a replication and not a second authority.** The observer starts
its own `CFire` on its own copy of that ped and decides nothing about that
player's health. Two independent things make that true rather than intended:

- `CPed::InflictDamage`'s arm for cause 9 is `bFireProof` and an immediate
  `return false` (`0x004EA898`), and every remote ped is `bFireProof`
  (§1.10.2). The observer's fire cannot take health off the ped it is drawn
  on even if every other guard were removed.
- The `InflictDamage` detour refuses anything aimed at another player's ped
  before it looks at why (§1.10.1).

**What the observer's fire *is* allowed to do** is spread. `CFire::ProcessFire`
sets the local player alight if they are close enough and not already burning,
which is single-player behaviour and is the thing a teammate standing next to a
burning player should see. The damage that follows is then §1.10.6's, decided
on the local player's machine from a fire in the local player's world, and
gated by friendly fire through the fire's own `m_pSource`.

**The source is the burning player's own ped, not whoever lit them.** Null
would make it terrain, and terrain ignores friendly fire (the table above), so
a teammate brushing past you in a session with friendly fire off would set you
alight and burn you to death. For a fire spreading off somebody's body, their
ped is both the honest answer and the one that routes the spread through the
rule that already exists.

**It is a reconciliation loop, not an event.** The flag rides the unreliable
snapshot, `CFireManager::StartFire` refuses a ped that is not
`IsPedInControl()`, and an observer's fire carries a one-second extinguish time
that the owner re-arms while they are still burning. So every frame asks the
same question - is this player alight, and is the fire on their ped ours - and
every race falls out of that instead of needing a handler. A dropped packet
costs 40 ms; a client that stops talking costs a second.

**One thing an observer's fire may not do: be on a ped in a car.**
`ProcessFire`'s ped arm writes `75.0f` into a burning ped's vehicle's
`m_fHealth` (`0x00479959`), which is how catching fire wrecks the car you get
into. On an observer that would be this machine deciding the health of
somebody else's car, so a remote player who gets into a car stops burning
here. Their own machine wrecks their own car and the result arrives on the
vehicle's stream like everything else about it.

#### 1.10.8 Busted rides the death's respawn and the ped state, and adds no packet

What an arrest is, from the retail image (`addresses.h`, "busted"): a cop's
`CCopPed::SetArrestPlayer` (`0x004C2B00`) writes `PED_ARRESTED` (56) into the
player's ped, clears `m_bCanBeDamaged`, and for a player in a car sets the car
to `STATUS_PLAYER_DISABLED` with the handbrake on. It blends no animation on
the player - the only animation in the whole arrest is `ANIM_STD_ARREST` on
the cop. `CGameLogic::Update` (`0x00421400`) sees 56 and calls
`CPlayerInfo::ArrestPlayer`, which starts the four second `WBSTATE_BUSTED`
clock. At 0x800 ms the screen fades to black; at 0x1000 ms the engine takes a
fine and the weapons, empties the car seat, and runs the same
`RestorePlayerStuffDuringResurrection` a death uses, pointed at the nearest
police station. Health is never touched.

So everything an observer needs was already on the wire except the end:

- **The arrest itself** is in the snapshot. `pedState` says 56, the ped stays
  where the cop caught it, and it stays on the pose stream (it is not a
  corpse, so nothing stops the stream). In a car the ped stays seated, which
  is also right.
- **Being dragged out of a car** - the usual start of an arrest in a car - is
  `PED_DRAG_FROM_CAR` (51) on the snapshot from the first frame, and the
  owner's `JACKEDCAR` animation and positions are on the pose stream. What
  stopped anyone seeing it was the seat: the replica stayed in it until the
  `S_ExitVehicle` at the end of the drag. `UpdateRemoteSeats` now stands the
  seat down on the snapshot and holds it down through the `PED_ARRESTED` that
  follows (`DragLatchFor`), the same way §1.14.4 starts a get-out on the
  snapshot. For an arrested player the engine does not fade the drag
  animation (`0x004CF022`), so they stay on the ground where they landed.
- **The police station** was not. Health goes nowhere, so the death machine
  never fired, and the respawn reached everyone else as a snapshot across the
  map from the last one - and for a player arrested in a car, as an
  `S_ExitVehicle` that put them on foot at the car for an interpolation delay
  before the jump. The owner now reads `pedState` beside health
  (`LocalLifeEventFor`) and sends leaving 56 as `C_Respawn`. The engine ends
  an arrest with the same `RestorePlayerStuffDuringResurrection` it ends a
  death with, so §1.10.5's rebuild fits it unchanged.
  The server already took a respawn from a player who never died, and
  `NotePlayerRespawned` already takes them out of their car.

Nothing is sent at the moment of the arrest, and a late joiner is not handed
one. Death is held by the session because a corpse cannot be rebuilt from
snapshots - `SetDie` has no undo and health zero does not reproduce it. An
arrested player is alive, and every snapshot describes them correctly. No new
opcode and no version change.

What is left belongs elsewhere: the fine is `CPlayerInfo::m_nMoney`, and
`ArrestPlayer` calls `CDarkel::ResetOnPlayerDeath` exactly like `KillPlayer`
does; the 12 hours `PassTime(720)` adds is the session clock's problem, the
same as a death's.

### 1.11 A destroyed car: health is a number, destruction is an event

Reported, in this order:

> "cuando se explota un auto se desaparece"
>
> "el auto desaparece pero despues cuando volves a entrar despues como un late
> joiner si aparece pero no se puede manejar ni nada, no aparece roto te podes
> subir y todo"
>
> "parece que si se rompe porque aunque lo veia vivo y el late joiner lo
> recibia no se podia mover"

Three symptoms, three separate causes, and only the first of them is about
destruction at all. §1.11.1, §1.11.4 and §1.11.5 take them one at a time.

#### 1.11.1 Writing `m_fHealth` never destroyed anything

`VehicleStateBody.health` has always been sampled off the driver's car and
written straight into the observer's copy at `+0x200`. When a car blew up, the
number that arrived was zero, and a car with zero health is not a destroyed
car.

The engine says so in three instructions. `CVehicle::InflictDamage` - the
engine's own fatal path, the one that runs on the driver's machine - does this:

```
0x00551C10  mov dword [esi+200h],0        m_fHealth = 0
   ... 69 bytes ...
0x00551C55  mov ecx,esi / push ebp
0x00551C5A  call dword [ebx+74h]          BlowUpCar(culprit)
```

The write and the destruction are two separate acts, and only the second one
does anything. `CAutomobile::BlowUpCar` (`0x0053BC60`) is where the status
becomes `STATUS_WRECKED`, where `bRenderScorched` is set, where
`m_nTimeOfDeath` is stamped, where `CDamageManager::FuckCarCompletely` runs,
where the wheel flies off, where the occupants are killed, and where
`CExplosion::AddExplosion` is called with `EXPLOSION_CAR`. An observer handed a
zero got none of it: an intact-looking car, with dead health, that the local
engine had no reason to blow up.

Nothing anywhere in the engine tests `m_fHealth` for zero. Exactly one place
reads a raw `m_fHealth` and can end up destroying a car, and it is not a
comparison against zero - see §1.11.3.

#### 1.11.2 Who decides: the driver's machine, and only it

A car's owner is its driver, which is already CoopIII's rule for its transform,
its velocity and its controls (§2.1). Destruction is the same decision and goes
the same way:

- The driver's machine detours `CAutomobile::BlowUpCar` and `CBoat::BlowUpCar`.
  When the car the local player is driving actually blows up - confirmed by
  reading `STATUS_WRECKED` back off it afterwards, because `BlowUpCar` returns
  without doing anything if `bCanBeDamaged` is clear - it sends
  `C_VehicleBlowUp` with the transform the car had at that moment.
- The server takes the packet **only** from the player it believes is driving
  that car. It has no GTA III running, so it cannot tell a real explosion from
  an invented one; refusing a player who is declaring somebody else's car
  finished is the one useful thing it can check, and it is the same test
  `OnVehicleState` already applies to a snapshot.
- Every observer puts the car where the owner says it ended up and then runs
  the engine's own `BlowUpCar` on it, through the object's vtable slot 29.

One call, because one call is what the engine does. The blast and the burnt
shell are decided in the same function, so replaying it gets both - in the same
street, on every screen - without CoopIII inventing either. This reuses
`CExplosion::AddExplosion`, which §1.9.3 already replays at an agreed position;
it does not add a second mechanism beside it.

Position first, then the blast, and the order matters: `BlowUpCar` reads
`GetPosition()` for the explosion, the camera shake and the fire it starts.
Correcting afterwards would put the wreck in the right place and leave the
explosion where the observer's physics had guessed.

**An observer is forbidden to decide.** The same detour refuses outright any
`BlowUpCar` on a car another player is driving. A wreck is a place as well as
an event and an observer has neither the moment nor the position its owner will
pick; their machine is already deciding, and their `C_VehicleBlowUp` is what
brings it here. This is §1.9.2's rule for damage and §2.1's rule for a
transform, applied to one more decision.

**A synced car with nobody driving it is nobody's**, and each machine keeps its
ordinary behaviour for it - the same way untouched traffic already does. It has
no owner, so there is nobody entitled to announce anything about it.

#### 1.11.3 The five-second fire timer, and why an observer must not run it

This is the one path from a health value copied off a socket to a car the local
engine destroys. Inside `CAutomobile::ProcessControl`:

```
0x00534510  fld [ebp+200h] / fcomp [0x006005C0]   m_fHealth < 250.0f ?
0x0053452A  mov cl,[ebp+50h] / shr cl,3 / cmp eax,5   ... and not WRECKED ?
    ...
0x00534768  fadd  [ebp+530h] / fstp [ebp+530h]   m_fFireBlowUpTimer += timestep
0x0053477A  fcomp [0x00600730]                   5000.0f
0x0053479B  call  0x004A15F0                     AwardMoneyForExplosion(player)
0x005347AB  call  dword [ebx+74h]                BlowUpCar(m_pSetOnFireEntity)
```

Note what is **not** in the entry condition: no flag, no damage event, no
culprit. Just a health below 250 and five seconds of frames. So an observer
that copies a health of 249 arms this timer and, five seconds later, decides
by itself that somebody else's car is finished - at a moment its owner did not
choose, at whatever position local physics had it in, and paying the local
player `AwardMoneyForExplosion` once per frame until it succeeds.

So CoopIII holds `m_fFireBlowUpTimer` at zero for any car another player is
driving. **Only the timer.** The flames and the smoke are drawn off `m_fHealth`
in the same block and are left alone, so a burning car still burns here exactly
as it does on its driver's screen. What is taken away is the decision, not the
picture.

The detour is the backstop for the two callers no timer can head off,
`CVehicle::InflictDamage` and `CVehicle::ProcessDelayedExplosion`.

#### 1.11.4 A wreck is never respawned, and that is why the car "disappeared"

The observer's car vanishing for good was never one bug.

`CCarCtrl::PossiblyRemoveVehicle` deletes any `STATUS_WRECKED` vehicle about 60
seconds after `m_nTimeOfDeath`, and the branch that does it is reached
*precisely because* a car is locked or not deletable - CoopIII's two
registration gates route a car into it rather than protecting it from it. That
is correct behaviour and it means a destroyed synced car leaves the pool by
itself.

`ResolveRemoteVehicle` then saw an empty pool slot and re-armed the spawn. Two
things followed:

- With `destroyed` unknown, it would build a **brand new, undamaged car** where
  a burnt one had just been cleared away. `RemoteVehicle::destroyed` stops
  that, and it is set from the wire event *and* from reading `STATUS_WRECKED`
  off the car, so a wreck this machine's own engine made is caught by the same
  test.
- With the model no longer requested, the respawn waited forever. Nothing else
  in the game holds a reference to a model only CoopIII's car was using, so the
  moment that car left the pool the streamer was free to throw the model out,
  and `UpdateRemoteVehicles` sat on an `IsModelReady` that would never come
  true again. Reconnecting brought the car back, because `OnVehicleSpawn` was
  the only place that ever asked. `RequestModel` is now called on every pass of
  the respawn loop.

A wreck is still **corrected every frame**, deliberately. Its owner has stopped
sending, so the interpolator holds it at the blast position - which is the one
place both machines agree on. Letting local physics own it instead is how two
wrecks end up in different streets.

And the snapshots still in flight when it blew up are dropped, on both ends.
They were sampled before the blast; applying one would relight a burnt-out car
and drag it away from its own explosion.

#### 1.11.5 "no se puede manejar" was a different bug entirely

A late joiner got a car it could not drive an inch. That is not about
destruction and it would have happened to any claimed car.

`CorrectRemoteVehicle` runs from `PostFrame`, **every frame**, after physics
and before the frame draws - which is the whole point of it (§2.1). It ran for
every car the session knew about, including one the local player had since got
into. Sixty times a second the session's transform was written over whatever
the player had just done with it.

So the correction now stops the moment the local player is in the driver's
seat. It tests the driver's seat and not "is the local player inside", because
a passenger is not an owner - the same test `SampleLocalVehicle` uses to decide
what this machine may send.

This is a **guard, not the ownership handoff**. The session still believes
somebody else's netId names that car, and the local claim makes a second netId
for it. What the guard buys is that the car drives while that is being sorted
out. `docs/roadmap.md` M2 has the handoff.

#### 1.11.6 Packets

`C_VehicleBlowUp` (0x36) and `S_VehicleBlowUp` (0x37), both `CH_EVENT`,
reliable.

```
VehicleBlowUpBody   netId u16, pos Vec3, rot Quat          30 bytes
C_VehicleBlowUp     hdr + body                             35
S_VehicleBlowUp     hdr + playerId u8 + body               36
```

The transform travels with it because `BlowUpCar` decides the wreck as well as
the blast: whatever the observer's own physics had the car doing, this is where
its owner says it ended up.

Everyone gets out before the car goes up, on the observer as well: `BlowUpCar`
hands every occupant to the engine to destroy on its own schedule, so a ped
taken out afterwards is being taken out of a car that has already let go of it.
That is the same ordering `UnseatPlayer` exists for.

A blast for a car the receiver has never heard of is dropped, not queued and
not created from - the packet carries no model, and a blast is not a reason to
invent a car.

### 1.12 A car's extras, next to its colours

> "Hay autos que a un player le aparecen con extras que el otro jugador no ve y
> viceversa"

GTA III picks a vehicle's extra components at spawn, on each machine
independently, so two machines roll two different cars. This is the same family
of bug as the paint job, which `EnterVehicleBody.colour1/2` already solved by
carrying the claimer's own `m_currentColour1/2`. The extras join them.

**It is not fixed the same way, and that is the only surprising part.** A
colour is read by the renderer every frame, so writing `m_currentColour1/2`
after construction changes the car. `CVehicle::m_aExtras` (`+0x19E`, `int8[2]`)
is a *record* of a decision already taken: the components are RwAtomics that
`CVehicleModelInfo::CreateInstance` cloned into the clump while the car was
being built. Writing those two bytes afterwards changes the record and nothing
on screen.

So the choice is forced **before** the constructor, through the engine's own
override, `CVehicleModelInfo::ms_compsToUse` (`0x005FF2EC`, `int8[2]`, `{-2,
-2}` in the file). It is one-shot: `ChooseComponent` (`0x00520AB0`) either sees
-2 and rolls, or takes the value and puts -2 back in the same nine
instructions. The engine uses it for exactly this job - `CStoredCar::RestoreCar`
sets it so a garage gives you back the car you put in.

`CAutomobile`'s constructor calls `CVehicle::SetModelIndex` →
`CEntity::SetModelIndex` → `CreateRwObject`, so `CreateInstance` runs *inside*
the constructor call, which is why `SpawnRemoteVehicle` is the only place this
can happen. The override is set immediately before and put back immediately
after - unconditionally, because `CreateInstance`'s `m_numComps == 0` arm
returns without consuming it, and a leaked override would be worn by the next
car this machine creates, traffic included.

**The wire value is clamped against the model.** `CreateInstance` subscripts
`m_comps[6]` with whatever it is handed and checks only for -1:

```
0x0051FCE0  cmp ebx,-1 / je skip
0x0051FCE5  mov eax,[ebp+ebx*4+1DCh]        m_comps[comp], no upper bound
```

Anything at or above `m_numComps` reads past a six-entry array and hands the
result to `RpAtomicClone`. Anything CoopIII cannot prove the model has becomes
-1, "fit nothing", which is the engine's own value for an empty slot and the
one bound `CreateInstance` does check. This is the same class of bug as the
four-entry animation group (§1.8.1) and the twelve-slot node array, and it is
the third time, so it is a test rather than a comment.

**Wire changes.** `EnterVehicleBody`'s spare `pad` byte became `extra1` and
`extra2` (37 → 38 bytes), and `S_VehicleSpawn` gained the same pair (39 → 41),
so a late joiner's backfilled car has them too. `Session::Vehicle` carries them
between the claim and the backfill.

### 1.13 A remote ped's AI: two fields, not a state threshold and not a skipped frame

This answers §6 open question 1, which had been open since this document was
written. It was measured against `gta3.exe` on 2026-09-22; `addresses.h`
carries the instructions, under `what CPed::ProcessControl actually does`.

**The answer is neither of the two options §6 named, and the reason is the
same for both: they are aimed at the wrong thing.** The AI that would make a
remote player wander off does not live in "`ProcessControl` running". It lives
in two fields, `m_nPedState` and `m_objective`, and CoopIII has been holding
both in the harmless position since the seating work without ever saying so.
So there is nothing to build, and there is something to write down, because
the next person to read `Ped.h:260` will reach for the same two wrong options.

#### 1.13.1 `PED_STATES_NO_AI` is not what its name says

`PED_STATES_NO_AI` is 34, confirmed from the retail image rather than from
re3's enum: `CPed::ProcessControl`'s state switch is

```
004CB11B  mov eax,[ebx+224h]      m_nPedState
004CB127  dec eax
004CB128  cmp eax,36h             1..55 are in the table
004CB12D  ja  004CB9F0            the default arm
004CB133  jmp [eax*4 + 005F8778h]
```

and reading that 55-entry table off the file confirms every value in re3's
`ePedState` against retail, `PED_STATES_NO_AI` included.

Three things follow, and each one on its own sinks the idea:

1. **State 34's own table entry is the default arm**, and so are twenty of the
   other fifty-four. The default arm is `fstp st(0) / jmp 004CB784`, and
   `004CB784` is `call [vtable+48h]`, i.e. `SetMoveAnim`. Taking it skips the
   per-state function and *nothing else*. Everything before the switch —
   the alpha fade, `BuildPedLists`, `ProcessBuoyancy`, the collision-damage
   block, `CPhysical::ProcessControl`, `UpdatePosition`, `PlayFootSteps`,
   `ProcessObjective`, `AimGun` — and everything after it still runs.
2. **The name describes a bound, not a switch.** re3 uses `PED_STATES_NO_AI`
   in exactly three places and all three are predicates:
   `IsPedInControl()` (`m_nPedState <= 34`), `CanPedReturnToState()`, and the
   `m_fHealth <= 1.0f` auto-`SetDie` in `ProcessControl`. It means "above here
   the ped is being driven by a sequence rather than by its own head", which
   is a fact *about* the states, not a gate that turns anything off.
3. **Crossing it breaks something that works.** `IsPedInControl` is
   `CFireManager::StartFire`'s gate for a ped (`0x004795C2`, §1.10.7), so a
   ped parked at 35 or above can never be set on fire — the burning remote
   player, confirmed in game, would stop working the day this shipped.
   Parking at 34 exactly keeps the fire and buys nothing, because 34 already
   means the default arm.

For completeness, two neighbouring thresholds that are *not* affected, because
guessing about them is how this idea would come back: `CPed::SetObjective`
gates on `DyingOrDead()` only, so seating would survive; and `IsPedShootable`
(`<= PED_STATES_NO_ST`, 40) is inlined at three AI target-selection sites and
is not in the damage path at all, so damage would survive. The fire is the one
that dies.

#### 1.13.2 Skipping `ProcessControl` costs more than the AI does

The other option, refusing `CCivilianPed::ProcessControl` for a ped CoopIII
owns, is implementable — every remote player is a `CCivilianPed`, and
`RemotePlayerForPed` already answers "is this one ours" — and it would work.
It would also take away the following, all of which a remote player currently
gets for free and would have to be reimplemented by hand:

| What goes with it | Why it matters |
|---|---|
| `CVisibilityPlugins::SetClumpAlpha` at `0x004C8981` | The **only** thing in the engine that raises a ped's clump alpha. A ped that never runs `ProcessControl` is never drawn. This is the same fact the render block in `addresses.h` reaches from the other end, and it is how the second invisible ped was found. |
| `CPhysical::ProcessControl` (`0x00495F10`) | Gravity and collision response. Snapshots arrive at 25 Hz and frames at 60; between them this is what keeps a remote player standing on the ground instead of hanging in the air at the last sample. |
| `Die()` in the `PED_DIE` arm, and the `SetDead` else arm | A corpse's death animation is played out here and the transition to `PED_DEAD` happens here. Skip it and a killed remote player stays mid-fall forever. M4, confirmed in game. |
| `AimGun` (`0x004C6AA0`) | §1.8.3's whole design: CoopIII sets `bIsAimingGun` and lets the engine rotate the torso. No `ProcessControl`, no aim. |
| `CalculateNewOrientation`, `UpdatePosition`, `PlayFootSteps`, `ServiceTalking` | Facing eased toward `m_fRotationDest`, the animation's own translation applied, footstep sounds and dust, voice. |

Only two of those could be replaced cheaply (the alpha write is two known
calls). The rest is the engine doing a remote player's presentation, and
CoopIII would be trading a small AI problem for a large reimplementation.

`CWeapon::Update`, which would otherwise be on that list, is not: `combat.cpp`
already forces a remote ped's weapon slot to `WEAPONSTATE_READY` with a full
clip before every replayed shot (§1.9.6), so the weapon state machine is
already not the engine's business.

Two things that *look* like they need `ProcessControl` and do not, both worth
knowing because they have been written down wrongly in this repo:

- **A seated ped is positioned by `CWorld::Process`, not by
  `CPed::ProcessControl`.** The fifth walk over `ms_listMovingEntityPtrs`
  calls `SetPedPositionInCar()` on anything with `bInVehicle` set, from
  outside `ProcessControl` entirely.
- **Animations are advanced by `CWorld::Process`'s first walk**
  (`RpAnimBlendClumpUpdateAnimations`), also outside it.

#### 1.13.3 What the AI actually is, and where the off switch already is

`CPed::ProcessControl` contains exactly two places where a ped decides
something for itself, and both are already disarmed on a remote player:

**`ProcessObjective` (`0x004D94E0`)** is skipped whole by its own guard:

```
004D9527  cmp dword [ebx+164h], 0     m_objective != OBJECTIVE_NONE
004D952E  je  004D9544                ...or the entire body is skipped
```

`UnseatRemotePed` writes `OBJECTIVE_NONE` into that dword, and the comment
beside it already explains why — a `CCivilianPed` left holding
`ENTER_CAR_AS_DRIVER` walks back to the car. That write is the off switch. It
costs one dword and it holds whatever the ped's state is.

**The state switch** is disarmed by `m_nPedState` being `PED_IDLE`, which is
where `CCivilianPed`'s constructor leaves it and where `UnseatRemotePed` puts
it back. `PED_IDLE`'s arm is `CPed::Idle` (`0x004D0690`), and `Idle` cannot
change the ped's state: its whole repertoire is fading one animation in or
out and writing `m_nMoveState`. Nothing a remote player's ped runs today can
take it to `PED_WANDER_PATH`, `PED_FLEE_*`, `PED_SEEK_*` or `PED_ATTACK`,
because nothing calls `SetWanderPath` or `SetObjective` on it and
`bRespondsToThreats` is cleared at spawn.

So the "remote players wander off on their own" this document feared has not
been possible since the seating work landed, and the feature that closed it
was written for a different reason. **That is the answer: keep both fields,
and say in one place that keeping them is load-bearing rather than tidy.**

#### 1.13.4 The one thing this did find: the move-state mechanism has never worked

`CPed::Idle`'s non-still arm is:

```
004D077E  cmp  dword [ebx+22Ch], 1     m_nMoveState == PEDMOVE_STILL
004D0789  je   004D07C1               (the still arm)
004D07A6  call 004D48E0                CPed::IsPlayer()
004D07AD  jne  004D0949                ...a player is left alone
004D07B5  push 1 / call 004C5A30       SetMoveState(PEDMOVE_STILL)
```

`Idle` runs from the switch, and `SetMoveAnim` runs *after* the switch. So on
a `CCivilianPed` — which every remote player is — the move state CoopIII
writes from `PreFrame` is overwritten with `PEDMOVE_STILL` before
`SetMoveAnim` ever reads it, and `SetMoveAnim`'s first line is
`if (m_nStoredMoveState == m_nMoveState) return`.

§1.8.3 and the M1 notes say the locomotion animation is driven "by writing
`m_nMoveState` and letting `CPed::SetMoveAnim` pick". **That has never once
happened.** Remote players walk because `ApplyAnimation` blends the wire's
`animId` directly through `CAnimManager::BlendAnimation`, and that is the only
thing that has ever made them walk. The `m_nMoveState` write is kept — other
things read it, and it is what a co-op mission script would see — but it is
not the mechanism and nothing may be built on the belief that it is.

Worth noticing for its own sake: that `if (!IsPlayer())` in `Idle` is the same
shape as `CFireManager::StartFire`'s, which §1.10.7 already copies. GTA III
has more than one of these, and they are the engine agreeing that some peds'
movement is not its to decide. Where one exists, take its branch rather than
inventing a mechanism.

#### 1.13.5 The residual, which is a short list and not an AI

What is left is the engine acting *on* a remote ped rather than deciding for
it. Each has its own answer and none of them wants a general suppressor:

| Thing | Where | State today |
|---|---|---|
| `SetInTheAir` blends `ANIM_STD_FALL_GLIDE` (id `0x98`) when `CheckIfInTheAir` finds no ground | `0x004D0CA0` | Cosmetic and self-correcting; one of the two engine sources of clump animations. `EnforceAnimLimit` keeps it survivable. |
| `Idle` blends `ANIM_STD_IDLE_BIGGUN` (id `0x0A`) on a 3000-8500 ms random timer while still | `0x004D0690` | The other engine source. Same seatbelt. |
| `m_fHealth <= 1.0f && m_nPedState <= 34` → `SetDie(ANIM_STD_KO_FRONT)` | inside `ProcessControl` | An observer deciding a death. Harmless today because the owner announces its own death (§1.10.4) and the observer's `SetDie` reaches the same place, but it is an observer deciding, and §1.10 says observers do not. |
| `ProcessBuoyancy` → drowning | inside `ProcessControl` | Known, §1.10.2. `WEAPONTYPE_DROWNING` has its own arm in `InflictDamage` with no proof flag; the `InflictDamage` detour is what actually stops it. |
| `SetWaitState(WAITSTATE_STUCK)` at `m_panicCounter == 50` | inside `ProcessControl` | Needs a colliding ped that is `IsPedInControl`. Adds an animation. Not seen. |

**The anim pile-up** reported in a live session (`9 animations ... dropped 1`,
then 10, then 8) is now accounted for by name rather than by "the engine adds
its own without asking": `ApplyAnimGroup` up to five, `BlendRemoteAnim` one,
`ApplyOverlay` one held deliberately, `SetMoveAnim`'s single idle blend at
spawn, plus `IDLE_BIGGUN` and `FALL_GLIDE`. `EnforceAnimLimit` stays; it is
not a seatbelt for something unknown any more, and the twelve-slot bound from
`RpAnimBlendClumpUpdateAnimations` is what makes it necessary either way.

#### 1.13.6 What ambient NPC sync can rely on

`docs/population.md` §2.4 is blocked on this question, so, plainly: **a
replicated pedestrian does not need a new mechanism, and there is no ini
switch to wait for.** It needs the same two fields a remote player already
holds, set at creation and kept:

- `m_nPedState = PED_IDLE` (1). Do not put a replicated ped on a wander path.
  `CPopulation::AddPed`'s ambient route calls `SetWanderPath`, and that call —
  not `ProcessControl` — is what makes a pedestrian walk somewhere of its own
  choosing. A replicated ped is created the `CREATE_CHAR` way, as a
  `MISSION_CHAR`, exactly as `SpawnRemote` does.
- `m_objective = OBJECTIVE_NONE` (0), with `m_prevObjective` and
  `m_carInObjective` cleared alongside it, the way `UnseatRemotePed` does.
- `bRespondsToThreats = false` and `bAllowMedicsToReviveMe = false` at spawn,
  already part of `SpawnRemote`'s registration.

With those, `ProcessControl` runs, the ped is drawn, falls to the ground and
plays whatever animation the wire names, and decides nothing. What it will
still do is the §1.13.5 list, and a replicated pedestrian gets those for the
same price a remote player does. `PROTOCOL_VERSION` does not move for any of
this.

---

### 1.13 Pickups: the client detects, the server arbitrates, the engine awards

Protocol **11**. `docs/pickups.md` is the whole investigation and design; this
is the wire.

**There is no spawn packet, and that is the finding rather than an omission.**
CoopIII does not suppress the main script on clients (M5 Tier 2, not built), so
every machine runs `main.scm` and creates all 448 script pickups itself, from
literal coordinates, through a `CPickups::GenerateNewOne` that never touches
the RNG. The worlds already agree. What was missing was never the pickup, it
was the exclusivity: two players standing on the same shotgun and both engines
awarding it.

**There is no snapshot either.** A pickup is not continuous state, it is a
small number of discrete facts - taken, denied, live again - and putting "who
got the shotgun" on the unreliable channel is the mistake §2.8 already made
once with `driverPlayerId`.

| opcode | name | to | body |
|---|---|---|---|
| `0x80` | `C_PickupClaim` | server | `PickupIdent` (16) |
| `0x81` | `S_PickupTaken` | everyone **but** the collector | `uint8 playerId` + `PickupIdent` |
| `0x82` | `S_PickupDenied` | the loser alone | `PickupIdent` |
| `0x83` | `C_PickupRelease` | server | `PickupIdent` |
| `0x84` | `S_PickupGrant` | the claimant alone | `PickupIdent` |
| `0x85` | `C_PickupCollected` | server | `PickupIdent` |

All six ride `CH_EVENT`. `0x86`-`0x8F` are reserved for the drop replication
M4 defers.

#### 1.13.1 The identity is a position and a model, not a slot

The engine's own handle is `slot | (generation << 16)` and it is **per
process**: `GenerateNewOne` hands out the first free slot in `[0,320)`, and ped
drops allocate out of the same range, so one player killing a pedestrian in
traffic moves that machine's cursor and every later script pickup lands
somewhere different from everyone else's. Slot agreement survives about a
minute of play.

Both ends resolve an ident by nearest-match within **0.25 m** of the same
model. Tolerance rather than an exact float compare: script pickups really are
bit-identical on both machines, but a ped drop's z comes out of
`CWorld::FindGroundZFor3DCoord` and nothing here should depend on that landing
on the same bit in two processes.

`flags` carries one bit, `PICKUP_F_BRIBE`. The pickup model indices are runtime
globals `CModelInfo` fills from the IDE, so the number is that install's and
nobody else's - and the server needs exactly one thing from it, because a
bribe's `PICKUP_ON_STREET_SLOW` window is 300 s where everything else's is
720 s.

#### 1.13.2 The claim goes out on approach, and that is what removes the race

The client claims at **4 m**; the engine collects at about 1.34 m. At walking
speed the answer is in hand half a second before contact.

That ordering is load-bearing. A pickup reward cannot honestly be revoked -
ammo already fired, health already spent in a fight, a bribe that has already
cleared a wanted star - so the arbitration has to happen **before** the award
and not after it. The client enforces it by stashing and nilling `m_pObject`
for every pickup it has not been granted, around the engine's own
`CPickups::Update`: `CPickup::Update` returns false immediately on a nil
object, below the respawn branch and above everything else, so the engine
physically cannot award a pickup CoopIII has not unblocked. There is no race to
lose.

#### 1.13.3 A grant is a reservation; only a collection removes anything

Claiming on approach means claiming things you turn out not to want - a health
pickup at full health, a bribe with no wanted level, or simply a street you
walked down. So a grant tells the claimant alone, and nothing is removed from
anybody's world until the claimant's own engine has actually taken it:

```
    client  ---- C_PickupClaim ------>  server      at 4 m
    client  <--- S_PickupGrant -------  server      reserved; nobody else told
    (the engine's own CPickups::Update runs on an unblocked pickup)
    client  ---- C_PickupCollected -->  server      it took it
    others  <--- S_PickupTaken -------  server      remove your copy
```

or, when the player leaves the radius without collecting, `C_PickupRelease`
ends the reservation and the pickup is available again everywhere, having never
been removed anywhere.

The collection is **detected, not decided**: CoopIII holds the reservation, the
engine's own touch test, `CanBePickedUp` and award switch run, and the slot
being empty afterwards is what gets reported. Nothing re-implements the four
refusals `CanBePickedUp` makes; a refusal simply shows up as a reservation
handed back.

`S_PickupTaken` reaches everyone except the collector, whose own engine has
already removed their copy. Each of them replays the engine's own removal tail
and pushes the collection into their own `CPickups::aPickUpsCollected` -
without which the observer's `rampage.sc` and `rewards.sc` never notice,
because that ring is the only thing `HAS_PICKUP_BEEN_COLLECTED` reads.

A reservation nobody gives back would be a pickup nobody can have, so the
server expires one after 15 s, and a player leaving drops every reservation
they hold while keeping everything they collected.

An observer deliberately does not replay the *reward*. Health, armour, money
and weapons belong to one player and that player's own engine has already
applied them. The single exception is the hidden package counter, which is
shared by decision (`roadmap.md` §5.11) and is one increment.

#### 1.13.4 Respawn: the server owns availability, each engine owns appearance

The engine writes `m_nTimer = CTimer::m_snTimeInMilliseconds + k`, an absolute
stamp on a **local** clock that starts when that machine's game started and
stops while it is paused. The constant travels; the deadline must not.

So the server records when it granted a pickup and how long that type stays
gone, and denies a claim inside the window. Each client's engine is free to
bring the object back whenever its own rule says so - the respawn branch also
insists the local player is more than 10 m away, which genuinely fires at
different moments on different machines. That divergence is harmless once
availability is the server's: a copy that comes back early is simply blocked
until the window closes, and a copy that comes back late means its owner cannot
claim yet. A client only claims when its local object exists, so it never holds
a grant it cannot consume; if one goes away between the claim and the answer it
sends `C_PickupRelease` and the reservation ends at once.

Note that the window only starts at the **collection**, not at the claim: a
reservation carries no respawn time because nothing has been picked up.

#### 1.13.5 A taken-record is a lock, not a tombstone

`C_PickupRelease` has a second meaning, and it is needed because the script
reuses coordinates. Of 312 script pickup positions, 29 same-model pairs sit
within 2 m of each other and **all 29 are at distance exactly zero** - the
Ammu-Nation counter's in-stock/out-of-stock pair in the two arms of one `if`,
and the 20 rampages, each re-created at its original spot after two failures.
None of them is ever live twice at once (every rampage handler runs `0215
destroy_pickup` first), so the ident does not alias - but a `PICKUP_ONCE`
record would otherwise block the re-created pickup forever.

So a client whose engine has a live object at a key that client previously
removed on an `S_PickupTaken` says so once, and the server drops the record.
It costs nothing to notice: the client is already walking its pickup table
every frame for the proximity test.

#### 1.13.6 A joiner is told what is already gone

The backfill carries one `S_PickupTaken` per *collected* record - reservations
are left out, because nothing has been picked up and there is nothing for the
joiner to remove. Without it the joiner
is the one player in the session who can still see - and walk into - every
hidden package the group has already collected.

---

### 1.14 Getting into a car takes a second, and the wire does not

A remote player used to appear in a seat. `CPed::WarpPedIntoCar` is one call
and it is over before the frame is: no door opens, nothing is animated, the
ped is simply somewhere else. Getting out was the same in reverse.

The engine's own way in is `CPed::SetEnterCar`, and it is not a call you make
and read the answer to. It puts the ped into `PED_ENTER_CAR` and hangs an
animation on it; `CWorld::Process`'s walk over `ms_listMovingEntityPtrs` -
the same walk that already positions a seated ped, §1.13.2 - calls
`EnterCar()` on it every frame after that, and a chain of animation-finish
callbacks ends by assigning the seat. It takes the better part of a second
and it can stop at any point in that second without saying so.

So the session says "seat 2 of car 41" and the engine says "walking towards a
door", and the two are both true. Four things follow, and they are the whole
design.

#### 1.14.1 A third state, in the loop that already had two

`RemotePlayer` had `seatVehicleNetId` (what the session asks for) and
`seatedVehicleNetId` (what the engine has), with `UpdateRemoteSeats` driving
one toward the other every frame because the ped and the car each take as
long as their model does to stream. That shape did not need changing, only
extending: `enteringVehicleNetId` is a third field in the same loop, and
every race the animation adds - the car despawning mid-entry, the player
dying mid-entry, the session naming a different car mid-entry, the engine
quietly giving up - falls out of the same per-frame comparison rather than
needing a handler of its own.

#### 1.14.2 The animation is an attempt; the seat is not

**The warp did not go away and must not.** It is what the entry falls back
to, and every path out of an unfinished entry ends there in the same frame:

| how it ends | what happens |
|---|---|
| the engine refuses to start it | warp, same frame - a refusal costs nothing |
| the engine stops without seating them | give the door back, then warp |
| the deadline passes (2.5 s) | give the door back, then warp |
| the car is despawned or blown up | give the door back, no seat |
| the player dies | give the door back, no seat |

A player who teleports into a seat is one bad frame. A player stuck half
inside a car is the rest of the session, and this project has been bitten by
that shape of bug before - the driving animation that stuck forever, fixed in
`ped.cpp` by `FadeOutAllPartials`.

One attempt per enter event, marked spent before it is made rather than after
it succeeds, so that a refusal and a timeout both count. Otherwise a ped that
cannot get in is asked to again, forever, at sixty frames a second.

The pre-check on how fast the car may be moving was a guess, and it was looser
than the engine's. It is now `sq(0.2f)`, which the engine uses twice: once in
`CVehicle::CanPedEnterCar` (`0x005522F0`) before the walk, and again inside
the door-opening callback at `0x004DE758` - and the second one does not refuse
the entry, it calls `QuitEnteringCar` and `SetFall(1000, ...)`. A car creeping
at 0.3 m/s passed the old `sq(0.5f)`, the ped walked up to it, and the engine
knocked him into the road a second later. A pre-check looser than the engine's
own buys nothing except a worse-looking failure.

#### 1.14.3 Giving up costs a call, and skipping it breaks the car

`CPed::QuitEnteringCar` is not tidiness. An entry dropped without it leaves
the door's bit set in the car's `m_nGettingInFlags` and `m_nNumGettingIn`
counting somebody who is not coming - and that flag is the *first* thing
`SetEnterCar` tests, so the door is then refused to everybody for the rest of
the car's life. It is the door-shaped version of the stuck animation above.

That is why abandoning is a bridge call rather than something the client
could forget, and why `UnseatPlayer` makes it first: every caller that takes
somebody out of a car now also takes them out of the doorway.

#### 1.14.4 Getting out is started by the snapshot, not by the event

`S_ExitVehicle` is reliable and it is too late to animate from. The owner
sends it when their own `bInVehicle` goes false, and that is the *last* thing
their get-out animation does - starting from it would put every observer a
whole animation behind a player who is already running down the street.

`PlayerStateBody::pedState` has been on the wire since protocol 2 and has
never been read by anything. It says `PED_EXIT_CAR` the moment the owner's
engine starts the exit, which is a second earlier and exactly on time. So the
snapshot decides *how* they leave and the event still decides *that* they
left. Nothing on the wire changed.

The entry now has an equivalent, and this paragraph used to say it could not
have one. What it actually said, correctly, is that the *claim* cannot move:
the claim names the car, introduces it to the session and hands over
ownership, and sending it at the start would give a car to a player who is
still standing in the road, which §2.8.3 makes load-bearing. That is true and
unchanged.

What was wrong was treating the claim and the entry as one thing. They are
separable, and §1.14.7 separates them: the claim stays at the end, and a
statement of intent that decides nothing goes out at the start.

#### 1.14.5 The pose stream asks the ped, not the session

`ApplyRemotePose` stopped writing a seated player's position when seating was
written, because the car became the authority on where they are. The same now
holds while they are climbing in, for the same reason rather than a similar
one: it is the same walk over the moving list, and `LineUpPedWithCar` is what
carries the ped from the pavement to the seat.

The test is the engine's own (`bInVehicle || EnteringCar()`, re3
`World.cpp:1971`) rather than the roster's opinion, which is what makes it
self-healing. Three things that would otherwise each need handling stop
needing it: a ped dropped out of a seat to make room for whoever the session
says is driving; a get-out animation that finishes before the exit event
arrives; and an entry the engine abandoned without anybody noticing. In all
three the ped is back on the pose stream on the next frame instead of
standing frozen until a packet says so.

#### 1.14.6 Carjacking: the seat is handled, the animation is not

`EnterVehicleBody` has carried a `jack` byte since protocol 3 and nothing has
ever written a one into it. The receiving side now says so explicitly rather
than ignoring it, and does not play `CPed::SetCarJack`. Two reasons, and the
second is the one that would still hold if the first were fixed.

**It would not run — for a remote ped, which is the only ped this is about.**
`SetCarJack` returns without doing anything when the car's `VehicleCreatedBy`
is `MISSION_VEHICLE` (the gate is at `0x004E032B`), and that is what every car
CoopIII creates is, because it is what stops the engine reaping it.

That sentence was first written without its qualifier, and the qualifier
matters. The bail sits **after** the `CPed::IsPlayer` call at `0x004E0310`, and
the `jne` at `0x004E0319` jumps the whole guard block for anything `IsPlayer()`
says yes to — so it does not touch the local player pressing F on a session
car. The ped that would play this animation is always a `CCivilianPed`, so the
animation really is unavailable on exactly the cars a session has; it is just
not unavailable to *everybody*. `addresses.h` carries the disassembly, read out
of the retail image rather than from `re3`.

**It would decide something that is not ours to decide.** `SetCarJack`'s
animation chain ends by dragging the ped in the seat out of it. That is this
machine deciding that some *other* player left a car, which is the
host-authoritative rule backwards.

What the jack actually needed is handled without the flag. Seating somebody
into an occupied seat used to overwrite `pDriver` and leave the previous
occupant with `bInVehicle` set, `m_pMyVehicle` pointing at a car that had
never heard of them, and `CWorld::Process` asking that car every frame which
seat they were in. Now whoever holds the seat is taken out of it first - not
a decision, a consequence of the session having already said who is sitting
there, and two peds cannot both be the driver. Their own machine sends their
exit a moment later and the two agree from then on. The visible difference
from real jacking is that the jacker plays the ordinary get-in animation
rather than hauling somebody through the door.

**Re-examined after the handover landed, and the decision stands** - but only
one of the two reasons still carries it, and it is the one that used to be
called the weaker.

Protocol 22 changed the second reason. "It would decide something that is not
ours to decide" was written when a jack had no handover at all: the jacker's
machine dragged the replica out, the victim's machine was never told, and both
ended up believing they drove the car. That is no longer the situation. The
server arbitrates the seat, the loser is sent `S_ExitVehicle` *before* the
winner's `S_EnterVehicle` on the same reliable ordered channel, and
`game::SurrenderVehicleSeat` puts the victim on the pavement beside the door.
An observer playing the drag-out would no longer be deciding anything: it
would be showing a decision the session had already made and told it about, in
the right order. On its own, that reason would now fall.

The first reason does not fall, and it is absolute. `CPed::SetCarJack`
(`0x004E0220`) bails on a `MISSION_VEHICLE`, and the gate is at `0x004E032B`.
The bail sits behind the `CPed::IsPlayer` call at `0x004E0310` with a `jne` at
`0x004E0319` jumping the whole guard block - so it does not touch the local
player - and the two objectives that *also* jump it, at `0x004E031B`, are
`m_objective == 8` and `== 7`, which are `KILL_CHAR_ANY_MEANS` and
`KILL_CHAR_ON_FOOT`. Neither is `ENTER_CAR_AS_DRIVER` (15) or
`ENTER_CAR_AS_PASSENGER` (14). Every ped CoopIII builds is a `CCivilianPed`,
every car it builds is a `MISSION_VEHICLE` because that is what stops the
engine reaping it, and a replica jacking one therefore falls straight through
to the `ret` at `0x004E033D`. The animation is not available on exactly the
cars a session has. Calling it would do nothing at all, silently, and leave
the seating loop waiting out its deadline before warping - which is strictly
worse than the ordinary get-in it plays today.

So: no change, and the reason is now mechanical rather than architectural. It
would become worth revisiting only if a session's cars stopped being
`MISSION_VEHICLE`, and roadmap.md §5.8 is where that would have to be argued
first, because that flag is holding up the whole vehicle lifecycle.

#### 1.14.7 An entry is announced at the start, and it says which door

Two packets now describe getting into a car, and separating them is the whole
of this section.

| | `C_EnterVehicle` (0x30) | `C_EnteringVehicle` (0x60) |
|---|---|---|
| sent | when the entry **ends** | when the entry **starts** |
| says | "I am in that car, in that seat" | "I have started getting into it" |
| carries | the car's identity, for a car the session may never have heard of | a netId, a seat and a door |
| decides | the seat, the driver, ownership | nothing |
| retracted by | `C_ExitVehicle` | nothing - it expires |

The first does not move, for the reason §1.14.4 gives and §2.8.3 depends on.
The second is what version 21 gave a passenger seat and what a driver's entry
did not have.

**Why it is needed at all.** A car's door is swung frame by frame by the
entering ped's own animation, on whichever machine is running that ped
(`CPed::EnterCar`, `0x004E0D30`). Nothing about an open door travels and
nothing needs to - but it does mean the only thing that ever opens that door
on an observer's screen is a replica entry of its own, and an observer told
about an entry after it has finished has nothing left to play.

**Why the door has to be on the wire, and the walk does not.** The walk is
already on the wire, as position, at 25 Hz, and `ApplyRemotePose` stops
writing that position the moment the replica's own entry starts (§1.14.5). So
the ped walks up on the pose stream and the door opens on this packet, and
neither machine re-derives anything.

The door is different, because **the seat does not name it**. A driver's entry
does not walk round the car to the driver's door - that was the assumption and
the retail image refutes it. `CPed::SeekCar` (`0x004D3F90`) sends anything
holding `OBJECTIVE_ENTER_CAR_AS_DRIVER` through `CPed::GetNearestDoor`
(`0x004E1CF0`), which compares the four door positions by squared distance and
writes the winner into `m_vehDoor`. Press the enter key standing on the
passenger side and the engine opens the *near* door, puts you in through it,
and `CPed::PedAnimDoorCloseCB` then blends the shuffle and slides you across
the front seats to the wheel.

That is the bug the player reported in those words: *"si me subo a un auto con
la F desde el lado del pasajero... luego de cambiar hacia el otro asiento como
que se lo ve tpearse a la puerta del conductor y abrirla"*. Told only "seat
0", the observer opened the driver's door, and `CPed::EnterCar`'s line-up
dragged the replica round the car to it.

Could the observer work the door out for itself, from the ped's position and
the car's matrix? It has both. It should not, and this is the one place in
this feature where re-deriving loses to sending a byte: the observer's copy of
that position is a hundred milliseconds old and interpolated, `GetNearestDoor`
is a comparison of four distances, and a ped walking past the corner of a car
crosses the boundary between two of them. Getting it wrong is not a small
error - it is the teleport again, in the cases where the two answers differ,
which are exactly the cases this exists for. One byte, sent once per entry,
cannot disagree with itself.

**Why it decides nothing.** An entry can be abandoned - shot halfway in, the
car drives off, the player changes his mind - and nothing on the wire
describes an entry that did not happen. There is no packet to send, because
the only packet about an entry is the claim at the end and there is no claim.
So the intent expires on its own (`ENTER_INTENT_TTL_MS`, four seconds) and
everything it caused goes with it, including a replica that got all the way
into the seat on an entry its owner never finished. Three rules follow, and
`UpdateRemoteSeats` is written so that they are the same three comparisons it
already made:

- an intent is a *provisional* want. It drives the animation and nothing else.
- **it never reaches the warp.** The warp is the answer to "the session says
  this player is in that car and the animation did not work". An intent says
  no such thing. Warping on one would put a player in a seat they never
  reached and leave them there until a packet that is not coming says
  otherwise.
- when the claim arrives it supersedes the intent, and when it does not the
  intent lapses and the player goes back on foot through the ordinary
  `UnseatPlayer`.

The server relays it and writes nothing down, for the same reason: a seat
recorded from an intent would be a seat no packet ever corrects, and §2.8.3
would be reading it.

**The gap, stated.** An intent can only name a car the session already has.
A car nobody has claimed exists on one machine and nowhere else - the claim at
the end of the entry is what introduces it - so there is nothing on any other
screen to open a door on. That is the right gap to have: the entry an observer
most needs to see is somebody getting into a car it can already see.

#### 1.14.8 An abandoned entry used to leave the door open forever

`CPed::QuitEnteringCar` makes no call on the car at all - there is no
`call [reg+5Ch]` anywhere in `0x004E0E00..0x004E0F96`. It hands back the
door's bit in `m_nGettingInFlags`, which is what §1.14.3 is about, and leaves
the door itself wherever the animation had swung it to. With nobody in the
seat nothing ever swings it back: only somebody else's get-in or get-out
touches that door again, so a car sat in the street with a door open and no
driver for the rest of the session.

`AbandonPedEnterCar` now shuts it, with the engine's own call:
`ProcessOpenDoor(m_vehDoor, ANIM_STD_CAR_CLOSE_DOOR_LHS, 1.0f)` through
vehicle vtable slot `0x5C`. The animation id is `0x5C` and it was read out of
the retail image rather than guessed - `client/src/game/addresses.h` carries
the whole chain, but the short version is two independent readings that agree:

- `CPed::PedAnimDoorCloseCB` pushes `5Ch` and `1.0f` at `0x004DF251` to close
  a door at the end of every real get-in, whichever door it is. The side comes
  from the component, not from the id.
- `CAutomobile::ProcessOpenDoor` (`0x0052E910`) routes exactly four ids -
  `0x5C`, `0x5D`, `0x6B`, `0x6C` - to the closing arm at `0x0052EB6D`, which
  past `0.63f` calls `OpenDoor(component, door, 0.0f)`. Four, which is the set
  re3 groups as the LHS/RHS and normal/low closing animations.

Done before the ped loses the entry, and only for a ped that actually had one:
`m_vehDoor` is not cleared when an entry ends, so a ped holding a stale door
would otherwise reach into a car it has nothing to do with and slam a door
somebody else was halfway through opening.

---

### 1.15 A car's damage model: three of the four fields converge on their own

The whole design is [docs/cardamage.md](cardamage.md), because it is long and
most of it is the argument for what does *not* travel. What belongs here is the
wire decision and the one sentence that produced it.

`roadmap.md` §4 listed the gap as "panels, doors, lights, wheels". Measured
against the binary, two of those four need no packet at all. **A tyre never
bursts in retail 1.0**: `m_wheelStatus` has three writers, `ProgressWheelDamage`
is only reachable from an `ApplyDamage` arm nothing ever passes a wheel to, and
`CAutomobile::BurstTyre` (`0x0053C0E0`) has no call site anywhere — its address
appears once in the whole file, in `CAutomobile`'s vtable slot 31, and the three
`call dword [reg+7Ch]` sites in `.text` are all COM calls in the movie and
networking code. What is left is `FuckCarCompletely`, inside a `BlowUpCar` §1.11
already replays everywhere. **And a broken light is exactly a damaged panel**:
`SetLightStatus` has one caller, is always passed the literal 1, and sits two
instructions from the `ProgressPanelDamage` call for the same panel.

`m_engineStatus` goes the same way for a different reason: the tail of
`CAutomobile::VehicleDamage` recomputes it from `m_fHealth` every frame on every
machine, and health has been on the wire since M2.

So the packet is panels and doors. Eight bytes, reliable, sent only when
something got worse, and **not a field on the 25 Hz snapshot** — for the same
reason §5.8 gives for the blow-up: a snapshot is sent by a driver, and the cars
this is most needed for do not have one.

Two properties of the data do the rest of the work. It is **absolute state**,
so a duplicate is a no-op and a drop is repaired by the next report; and it
**only ever climbs**, because `ProgressPanelDamage` and `ProgressDoorDamage`
both refuse at 3 and nothing but a respray lowers either. The merge is
therefore a componentwise maximum — commutative, associative, idempotent — and
the session lands in the same place whatever order reports arrive in and
whoever sent them. That is what lets §5.8's reporter rules carry over to a value
that is not a boolean, instead of inventing a fourth ownership model.

The one trap worth repeating here: **`m_doorStatus` is not a description of the
car.** Two of its four values are a door somebody opened, and `CPed` writes
`DOOR_STATUS_SWINGING` over `DOOR_STATUS_MISSING` unconditionally, so the byte
goes *down* while the car still has a hole in it. What travels is a damage
level, mapped once at the sampling end.

---

### 1.16 Garages, doors and the Pay'n'Spray: the transition travels, the door does not

Status: **built 2026-09-22, not yet run in the game.** Client seam
`client/src/game/garage.{h,cpp}`, four opcodes in the reserved `0xA0..0xAF`
block, and it moves `PROTOCOL_VERSION` to 18 along with the nine branches it
was merged with.

Before this, nothing about a garage travelled. A garage that opened for one
player was shut for everybody else, including the safehouse garages and the
spray shops.

#### 1.16.1 Nothing has to be spawned, which is the whole starting point

All 32 garages are created by `main.scm` from literal coordinates
(`init.sc`, opcode `0219 create_garage`), so every machine already agrees
about where each one is, what type it is and how tall its door is. This is
the same finding that shaped the pickup work (`docs/pickups.md`): the worlds
already agree, and only the *behaviour* is missing.

What is missing is that `CGarage::Update` (`0x004222D0`) asks
`FindPlayerPed()`, `FindPlayerVehicle()` and `FindPlayerCoors()` — the local
player and nobody else. A remote player standing in their safehouse is, to
this machine's state machine, nobody standing anywhere.

#### 1.16.2 Two kinds of transition, and only one of them is news

Every garage type's state machine has the same skeleton, and the split is
clean:

- **out of `GS_OPENING` or `GS_CLOSING`: derived.** Ramp `m_fDoorPos` by the
  door's fixed speed times `CTimer::ms_fTimeStep`, call `UpdateDoorsHeight()`,
  and on reaching the limit take the resting state and play a sound. Every
  machine can compute all of that from the state alone.
- **out of a resting state: decided**, from the local player's position, car,
  money and wanted level.

So the **state transition** goes on the wire and the **door position** never
does. Three reasons, in order of how much they cost to get wrong:

1. It is §1.11's argument again. A car's destruction travels as an event and
   not as `m_fHealth`, because the number is a consequence and the event is
   the cause. A door's height is a consequence that changes 60 times a second
   while its cause changes about six times in a whole visit.
2. `m_fDoorPos` is meaningless on its own. It is read against `m_fDoorHeight`
   and applied to one or two door `CEntity`s that `CGarage::RefreshDoorPointers`
   resolves to a **per-machine pool pointer**. Sending a height would be
   sending a machine a float derived from its own map data — the same mistake
   `docs/roadmap.md` §5.8 refuses for a parked car's transform.
3. Snapshots arrive at 25 Hz and doors move at 60. Driving the state lets each
   machine animate at its own frame rate and get the sound, the camera and the
   `UpdateDoorsHeight` push for free, out of the engine's own code.

#### 1.16.3 Authority: nobody owns a garage, so it is a union

A garage belongs to the map, not to a player. §5.8 settled the shape for a
parked car — "whoever saw it may say so, first report wins, and no transform
travels because the map put it there on every machine" — and **half of it
carries over exactly**: anybody may report, and no transform travels.

The other half does not, and the difference is worth stating because it is the
difference between an event and a level:

| | a car being destroyed (§5.8) | a door being open (here) |
|---|---|---|
| shape | a fact that happens once | a level that is true while somebody is there |
| reduction | first report wins | the union of everybody's |
| a second report | a duplicate, dropped | another player also holding it |
| going back | never | the normal case |

First-report-wins here would mean a door that never closes. Last-writer-wins
would mean the first player to walk away shuts the door on the second. The
union is the only reduction that is correct in both directions and for any
number of players, and it is one `OR` in `Client::RemoteGarageMask`.

Each machine reports **one bit per garage**: *my own state machine has this
garage away from where this type of garage rests*. A garage's resting position
is one of two things and the classification is the whole of it:

- **rests open** — `GARAGE_RESPRAY`, `GARAGE_BOMBSHOP1..3`, `GARAGE_CRUSHER`.
  These stand open and close over a car being worked on, so *shut* is the
  news.
- **rests shut** — everything else: the three hideouts (the safehouse
  garages, which `init.sc` calls `GARAGE_SAVEONE/TWO/THREE`), the mission
  lockups, the collection garages, the script-driven doors. *Open* is the
  news.

That gives "whoever opens it, everybody sees it open" for a safehouse and
"whoever is inside it, everybody sees it shut" for a spray shop, out of one
bit and one `OR`.

#### 1.16.4 An observer must not run the arm that ends a visit

This is the part that is not obvious, and it is why there is a detour on
`CGarage::Update` rather than a field write from `PreFrame`.

The Pay'n'Spray's entire effect — the repair, the repaint, the money and the
wanted level — lives in the `GS_FULLYCLOSED` arm of *this machine's*
`CGarage::Update`, and it acts on `FindPlayerVehicle()` and `FindPlayerPed()`.
Hold that garage shut on an observer and let the engine run, and two seconds
later the observer's own car is repainted and the observer's own stars are
cleared because a stranger across the city bought a paint job. The same is
true of the bomb shops (a free bomb, $1000 taken) and the crusher.

So while a remote report holds a *serviced* garage shut, the detour runs the
ramp and then stops. The two moving arms are pure animation and always run,
which is what gets the door and the door-closed sound right on every machine
for nothing. `GarageUpdateMayRun` in `client/src/game/garage.h` is that rule
and `tools/clienttest` pins it.

Letting go is not a no-op either. A serviced garage sitting at
`GS_FULLYCLOSED` has `m_nTimeToStartAction` already in the past — its
`GS_CLOSING` arm set it on the way down — so the first unsuppressed frame
after the hold comes off would fire the completion arm anyway. Releasing
therefore winds the door back up through the engine's own `OpenThisGarage`,
which is the observer's copy of "the visit ended".

#### 1.16.5 The respray colour has to travel, and not for the reason you would guess

`docs/roadmap.md` §5.9 says the model info picks a car's appearance "at
random". That is true of the *extra components* and it is **not** true of the
colours. The retail `CVehicleModelInfo::ChooseVehicleColour` (`0x00520FD0`)
contains no call to `CGeneral::GetRandomNumber` at all:

```
m_lastColorVariation = (m_lastColorVariation + 1) % m_numColours
col1 = m_colours1[m_lastColorVariation]
col2 = m_colours2[m_lastColorVariation]
if (numColours > 1 && FindPlayerVehicle() is this model && its colours match)
        advance once more
```

It is a **round robin over the model's own colour table**, with a tiebreak
against whatever the local player happens to be driving. Both halves are
machine-local state: the cursor counts every car of that model the process has
ever built, and `FindPlayerVehicle` is a different car on every machine.

The conclusion is the same as if it *had* been an RNG roll — the colour has to
travel — but the reason is stronger. An RNG could in principle be seeded to
agree. "How many Kurumas has this process made" cannot be, and neither can
"what is the local player sitting in right now".

So `C_Respray` carries the two colours **read back off the car the owner's
engine actually painted**, and no observer ever calls `ChooseVehicleColour`.
The repair travels with it: `m_fHealth = 1000`, `m_fFireBlowUpTimer = 0` and
`CAutomobile::Fix()` (`0x0053C240`), which is what the owner's arm did.

`vehicleNetId` is `INVALID_NETID` when the car is not a session car — a player
can drive an unclaimed traffic car into a spray shop. The packet still goes
out, because the doors and the sound are worth replaying on their own.

One more packet goes with it, and it belongs to §1.15 rather than here. `Fix()`
cleans the car on screen and touches none of the bookkeeping around it: the
server's damage record, the observer's row, and the owner's own high-water mark
all still hold the dents that have just been sprayed off. So the owner follows
the `C_Respray` with a `C_VehicleDamage` carrying `VEH_DAMAGE_RESET`, on the
same reliable ordered channel and in that order, which is the one report that
lowers a car. `docs/cardamage.md` §6.2 is why each of the three matters and why
the last one is the dangerous one.

#### 1.16.6 The wanted level does not travel, and there is a seam where it would

The third effect of a respray is `FindPlayerPed()->m_pWanted->Reset()`
(`CWanted::Reset`, `0x004AD790`). It is **not** on the wire and this packet has
no field for it.

- The wanted level is not on the wire at all yet. `docs/roadmap.md` §5.1 has
  it designed (per-player, GTA Online style, server-configurable) and
  unbuilt, and it is being worked on separately.
- Under §5.1's per-player design the correct behaviour is already what
  happens: the player who paid gets their own stars cleared, on their own
  machine, by the engine, with no help from CoopIII. Clearing anybody else's
  would be wrong.
- What an observer must not do is clear its *own* player's stars because
  somebody else bought a paint job, and it does not — that falls out of
  §1.16.4 for free.

The seam is one marked comment in `ApplyRemoteResprayImpl`
(`client/src/game/garage.cpp`, `SEAM (wanted level)`) plus the suppression in
§1.16.4. If §5.1 ever lands a *shared* wanted level, those are the two places
that change, and the change is a call into whatever seam that work exposes —
never a second `CWanted::Reset` of CoopIII's own.

#### 1.16.7 What this does not cover: the safehouse *door*

The safehouse **garage** is `GARAGE_HIDEOUT_*` and is covered. The safehouse
**pedestrian door** is not a garage at all. `init.sc` creates it as a script
object (`$PORTLAND_HIDEOUT_DOOR = init_object #PLAYERSDOOR`) and `save.sc`
swings it with `034D ROTATE_OBJECT`, gated on the local player standing in a
literal box. The save screen behind it is `03D8 show_save_screen` from the
same script.

None of that is reachable from `CGarages`. Every machine runs its own copy of
`main.scm`, so the door opens for whoever walks up and for nobody else, and
syncing it means either running the script host-only or intercepting
`ROTATE_OBJECT` — both of which are **M5** (`docs/campaign.md`). This section
does not touch it, and the save menu stays local, which is also the safe
answer: a save is a write to the player's own `GTA3sf*.b`.

#### 1.16.8 The packets

| Opcode | Name | Ch | Payload |
|---|---|---|---|
| 0xA0 | `C_GARAGE_STATE` | 1 | `deviating` `u32`, one bit per garage. Sent on change only |
| 0xA1 | `S_GARAGE_STATE` | 1 | `playerId` + the mask. Relayed to everyone except the sender, and replayed in the backfill for every player whose mask is non-zero |
| 0xA2 | `C_RESPRAY` | 1 | `vehicleNetId` `u16`, `garage` `u8`, `colour1` `u8`, `colour2` `u8` |
| 0xA3 | `S_RESPRAY` | 1 | `playerId` + the above |
| 0xA4-0xAF | - | - | Reserved for the rest of the garage block |

Thirty-two bits because `CGarages::Update`'s loop bound is a literal
`cmp ebx,20h`, not `CGarages::NumGarages`.

The server arbitrates nothing here and that is not laziness — there is no
owner for a report to be a lie about. Compare `OnVehicleState`, which *does*
check, because a car has a driver. What the server does is remember, so a
joiner can be told: the mask is a level sent on change, so without a backfill
a joiner would hear nothing until the door closed and would then be told a
door they never saw open had shut.
### 1.17 A broken lamp post is a latch, and only one machine says so

`docs/objects.md` is the investigation. Four things from it belong here,
because they are what the wire looks like the way it does.

**Breaking is a state.** `CObject::ObjectDamage` (`0x004BB240`) frees nothing,
allocates nothing and touches no world list - every one of its nine arms writes
flags on the object that is already standing there. So this is a latch, not a
lifecycle: two players breaking the same crate is not a conflict, applying the
same break twice is a no-op, and there is no exclusivity to arbitrate. That is
the whole difference from §1.13's pickups, which look superficially similar and
are not.

**Most of it was already agreed, and it was measured rather than assumed.** The
object arm of `CWorld::TriggerExplosionSectorList` computes its damage as
`300 * min((radius - distance) * 2 / radius, 1)` - two positions and a radius,
no RNG - and §1.9.3 already replays every explosion at an agreed position. So
objects blown up by a blast break identically everywhere for free, and the seam
stays silent inside `CWorld::TriggerExplosion` rather than sending duplicates.
And a bullet never breaks one at all: `CWeapon::FireInstantHit`'s object arm
applies a force and sparks, and there is no `ObjectDamage` call anywhere in
`CWeapon`. What is left is a collision, and only a collision.

**Named by where the map put it.** `m_objectMatrix`'s position plus the model
index - `sscanf`'d out of an IPL text file that is identical on every install,
so unlike a ped drop's z it is not computed at runtime at all. Not the pool
index: `CPopulation::ManagePopulation` converts every map object more than 80 m
from *the local player* into a dummy and back again, so the object pool churns
harder than any other pool in the game. The tolerance is 0.25 m and the closest
same-model pair among all 1851 breakable map instances is 0.5992 m.

**Reported by the machine that owns whatever broke it**, and by the host when
nothing owns it. `roadmap.md` §5.8's literal answer - an ownerless world entity
is the host's - does not transplant, because an object 80 m from the host is
not a `CObject` on the host at all and the host has nothing to observe. The
*shape* transplants: exactly one reporter, chosen by ownership. An observer
that breaks the object locally is not suppressed; it just says nothing.

There is no server table and no backfill, for the same reason there is no
persistence anywhere in this: the engine throws the state away at 80 m, so
nothing a joiner could be told would still be true by the time they finished
loading.

---

### 1.18 The wanted level: four spare bits, and the police are already paid for

[docs/wanted.md](wanted.md) is the design, the GTA Online research behind it
and the disassembly it rests on. What belongs here is only what crosses the
wire, which is less than anything else in this document.

**Nothing new was added.** `PlayerStateBody::flags` had four bits free after
`PF_ON_FIRE`, and they carry the whole outbound half:

| bits | name | meaning |
|---|---|---|
| 4-6 | `PF_WANTED_MASK` | this player's wanted level, 0..6 |
| 7 | `PF_WANTED_BORROWED` | the level is the session's, not this player's own |

It rides the unreliable snapshot for the same reason `PF_ON_FIRE` does
(§1.10.7): it is a state with a lifetime rather than an event, and a dropped
packet is corrected 40 ms later. Three bits hold 0..7 while the engine's
ceiling is 6, so the eighth value is clamped on the way in rather than
trusted.

The session's rule goes the other way, in two spare bits of `S_Welcome::flags`
beside `SESSION_FRIENDLY_FIRE`: `perplayer` (0, the default), `shared` (1),
`off` (2). Same place as friendly fire and for the same reason - it governs
something the client has to apply locally. Unlike friendly fire there is no
server-side half at all: a wanted level never passes through the server as
anything but a number in a snapshot, so there is nothing to refuse.

**`PF_WANTED_BORROWED` is a bit of wire spent on one deadlock**, and it is
worth saying why here rather than only in `wanted.md` §4.5. In `shared`, A
earns four stars and B is raised to four to match. A dies and clears. Without
the bit, B is still reporting four, so A is immediately raised back to four by
a level that only exists because A had it - and neither can get out until
everybody happens to die within the same 40 ms. With it, the session's floor
is the maximum over players who are *not* borrowing, and B's echo goes when
A's original does.

**No packet carries anything about the police.** A cop ped is created as a
`RANDOM_CHAR` and a police car as a `RANDOM_VEHICLE`, so both already travel
on the ambient ped and traffic streams (§1.13, [population.md](population.md))
with no change and no case made for them. The wanted player's own engine makes
them, `CCopPed` can only pursue `FindPlayerPed()` so they pursue that player,
and every other machine has been receiving them as replicas that decide
nothing since the day that code shipped.

**`PROTOCOL_VERSION` is 18 for this, along with the other nine branches it was
merged with.** No struct grew and no layout moved - the meaning of two existing
bytes changed, which is still a wire change, and version 18 is the one number
all of it landed under.

One thing did collide with another branch, and it was not an opcode. This work
put the session's wanted rule in `SessionFlags` bits 1-2 while the ammunition
work took bit 1 for `SESSION_AMMO_SYNC`, so a session with ammo sync on would
also have told every client its wanted rule was `shared`. The rule moved to
bits 2-3 at the merge; see `SessionFlags` in `protocol.h`. `PlayerFlags` bits
4-7 were free and stayed free, and **this feature allocates no opcode at all**.

### 1.19 Shooting somebody else's pedestrian: the direction the crowd never had

Reported twice, and the second time on a build where everything it looked like
was already working: *"el remote sigue sin poder hacerle daño a los NPC del
host"*. A player shoots a pedestrian the other machine hosts and nothing
happens — no reaction, no blood, no death, on either screen.

**It was a missing direction, not a broken one.** Version 17 carries a limb and
18 carries a death, and both travel *from* the machine that hosts a pedestrian
*out* to the observers. There was nothing at all going the other way. Every
replica is `bBulletProof`, `bFireProof`, `bMeleeProof`, `bCollisionProof` and
`bExplosionProof` on purpose (`game/population.cpp`, `SpawnAmbientReplica`), so
the shooter's own `CPed::InflictDamage` threw the hit away inside the engine and
there was nothing left either to apply or to forward. The log said so too, in
the most misleading way available: `combat: our first hit on a remote player`
was in the file, because the *player* direction has worked since M4, and there
was no line anywhere for a pedestrian, because there was no code. A direction
that does not exist reads exactly like a direction that is broken.

#### 1.19.1 It is §1.10 pointed at a pedestrian

The fix is not "let the shooter kill the replica and announce it". That is the
one thing §1.10 and [roadmap.md](roadmap.md) §5 forbid: an observer never
decides damage, and never decides where a remote entity ends up. So the split of
authority is the one the player exchange already uses, for the same two reasons:

- **The shooter reports the hit.** It fired a ray from a position, at an instant,
  along an aim nobody else has. A host asked to work out whether it was hit is
  doing it from a replica's transform 100 ms in the past, which is how you get
  shot around corners.
- **The owner applies it, through the engine's own `CPed::InflictDamage`.** It
  is the only machine with that pedestrian's real health, its `bUsesCollision`,
  its state and its seat. So the ped flinches, bleeds, staggers, loses a limb,
  drops what it was holding and dies exactly where single player puts all of
  that — and then 17's `C_PedBodyPart` and 18's `C_PedDeath` carry the visible
  half of that outcome back out to everybody, *including the shooter*.

Nothing new was needed on the way back. The existing `npcdeath` path already
turns the owner's `CPed::SetDie` into a packet every observer replays; this just
gives it something to witness.

#### 1.19.2 Exactly the five arguments, and no more

`CPed::InflictDamage` is `0x004EA420`, `__thiscall`, `ret 14h` — `this` in `ecx`
and five stack arguments, confirmed against the retail image rather than taken
from `re3`:

```
004EA420  fld  [005F9AB8h]              dieDelta = 4.0f
004EA42D  mov  esi, [esp+34h]           arg1  CEntity *damagedBy
004EA43F  mov  ebx, 0Dh                 dieAnim = ANIM_STD_KO_FRONT
004EA444  mov  edi, [esp+40h]           arg4  ePedPieceTypes pedPiece
004EA44D  mov  ebp, ecx                 this
004EAD80  cmp  dword [esp+38h], 14h     arg2  eWeaponType method
004EACC6  fsub dword [esp+3Ch]          arg3  float damage
004EA9B4  movzx eax, byte [esp+44h]     arg5  uint8 direction
004EADCB  ret  14h
```

`PedDamageBody` is four of those five plus a name for the ped, and it is the same
nine bytes `DamageBody` is:

| field | engine argument | why it has to travel |
|---|---|---|
| `netId` | — | which pedestrian. The shooter knows it because it holds a replica under that name; the owner resolves it back to a live `CPed` through its own hosted roster. |
| `weapon` | `method` | steers the proof-flag switch, the reaction, the limb roll and `m_lastWepDam` (`+0x51E`). Bounded to `IsForwardableDamage`: a ray or a melee reach, i.e. the causes only the shooter can have resolved. |
| `amount` | `damage` | raw. No multiplier, no armour, no clamp — all three are the owner's, and only the owner has the health. |
| `piece` | `pedPiece` | decides which limb comes off. The limb then travels back out on the owner's own `C_PedBodyPart`. |
| `direction` | `direction` | 0 front, 1 left, 2 back, 3 right. Two four-entry jump tables at `0x005F9E3C` and `0x005F9E5C` index straight off it and pick `ebx` = `17h`..`1Ch`, the knockdown animation. Without it every pedestrian in the city falls the same way. |

`damagedBy` cannot travel — a pointer means nothing on another machine — so
`S_PedDamage` carries `attackerId` and the owner resolves it to the replica it
already holds of that player's ped. A null culprit is a supported input and not
a degraded one: `test esi,esi / je 004EADE6` at `0x004EAAC4` skips only the
car-ramming speed block and the damage still lands.

**No position and no shot vector.** The shooter's engine already resolved the
ray; what crosses the wire is its conclusion. A position would invite the owner
to resolve it again, which is the observer-decides rule with the arguments the
other way round.

#### 1.19.3 What the wire now trusts a client to assert

This is new trust and it is worth naming rather than burying. Until now every
packet about an ambient pedestrian was a *statement about the sender's own
world*: I made this one, I lost this one, mine lost a limb, mine died. The
server could check all of them with one rule — is the sender its owner — and
refuse anything else.

`C_PedDamage` is the first packet where a client tells **another machine to
change something it owns**. What it is now trusted to assert is: *that its player
really did land a hit, on that pedestrian, with that weapon, for that much, on
that body part, from that side.* The owner checks that the name resolves to one
of its own named pedestrians and that the five values are in range, and then it
does what it is told. It cannot check the claim itself, because the claim is
about a ray that was traced on another machine.

Concretely, a hostile client can put any pedestrian in the session on the floor
at will, from anywhere on the map, without line of sight, at whatever rate it
likes. `MAX_REMOTE_DAMAGE` (1000.0f, the biggest deliberate single hit in the
game) and the piece and direction bounds exist so a corrupt or hostile float
cannot become a NaN in `m_fHealth` and from there in the ped's matrix — they are
memory safety, not anti-cheat. §2.1 already says plainly that a malicious client
can cheat and that this is a deliberate trade for a co-op mod; this widens the
surface of that trade from "their own player and their own city" to "anybody's
pedestrians", and nothing else. No player's health and no player's position is
reachable through it.

#### 1.19.4 Every failure mode, and the choice made for each

| case | choice |
|---|---|
| **The ped already died on the owner's machine** before the report landed | Let the engine refuse it. `CPed::InflictDamage` tests `DyingOrDead` at `0x004EA485` and returns false without touching anything, so the rule that refuses a hit on a corpse in single player is the rule that refuses this one. The server drops it earlier for free (`Session::PedDamageRecipient` checks `alive`), and the shooter does not send it at all if the replica is already a corpse on its own screen — so the ordinary in-flight burst costs nothing. |
| **Two observers report the same hit** | No dedup, deliberately. They are not the same hit: two players shooting one pedestrian is two real hits and single player applies both. What keeps it honest is that a hit is only reported by the machine whose *own player* landed it (`damagedBy == localPed`), so one trigger pull produces exactly one report from exactly one machine. The *death* is still deduped, on the owner: the `CPed::SetDie` transition test plus the per-netId check in `NoteHostedPedDeath` plus `Session::NotePedDeath`'s once-per-life rule mean two simultaneous fatal hits still produce one `C_PedDeath`. |
| **A report names a netId the owner no longer has** | Drop it. Two layers: the server refuses a netId it has no row for, and `ResolveHostedPed` returns null for one whose ped the engine reaped inside the round trip. Not retried, and that is the decision rather than an omission — a hit is only worth anything on the ped that was standing there, and there is nothing left to land it on. `CPopulation` reaps pedestrians constantly, so this is the ordinary race and not an error. The owner says so once in the log and then stays quiet. |
| **The ped is inside a vehicle** | Send it and apply it, unchanged, and do not pretend it can kill. `CPed::InflictDamage` tests `bInVehicle` at `0x004EACF3` and sends everything that is not `WEAPONTYPE_DROWNING` to **`0x004EADD0`**, which is `mov dword [ebp+2C0h],3F800000h / xor al,al` — health clamped to exactly 1.0f, "did not die". So a driver shot in his seat survives on one health on the machine that owns him, no `C_PedDeath` is produced, and every screen agrees, because that is what retail 1.0 does. `combat.h`'s `CanKillPedInVehicle` carries the transcription and `clienttest` walks it against `IsForwardableDamage` over all 256 causes: the intersection is empty, so **no cause this wire can carry is able to kill a ped in a seat.** (The address was recorded as `0x004EADCD` in `combat.h`'s death-cause block; that is three bytes early — `0x004EADCB` is the drowning arm's own `ret 14h` and `0x004EADCE` is an alignment `mov eax,eax`. The fact was right, the address was not, and it is corrected at the declaration.) |
| **A blast** | Never routed through this packet. An explosion is replayed on every machine at a position everybody agreed on, so the owner puts its own pedestrian in its own blast and its own engine kills it — and that death already travels. Forwarding it as damage as well would apply it twice, the same argument `IsForwardableDamage` makes for a player. |
| **Fire, drowning, a fall, being run over** | Refused for a pedestrian for exactly the reasons §1.10.1 refuses them for a player. (The flamethrower now reaches a pedestrian as an ignition rather than as damage, §1.24.) The one worth restating is drowning: a replica standing in water on an observer's machine used to drown *locally* — `WEAPONTYPE_DROWNING` has its own arm in that switch and checks no proof flag at all — and then stayed a corpse on that screen forever, because `ApplyAmbientPedState` refuses to drive anything into a dead replica and nothing off the wire resets `m_nPedState`. The new refusal closes that hole as a side effect, which is the same hole M4 found on the player side and the same reason the proof flags were never enough. |
| **Friendly fire** | Has no say, on either end. It is a rule about players hurting *each other*; a pedestrian is not a player, and a session with it off — which is the default, and the one the bug was reported on — still lets everybody shoot NPCs. Consulting it here would make the default session one where the whole city is bulletproof. |
| **Kill credit** | A known residual, stated rather than hidden. At `0x004EAD1A` the engine credits `CDarkel` only when `damagedBy` is `FindPlayerPed()` or `FindPlayerVehicle()`. On the owner's machine the culprit is a replica of somebody else's ped, so the kill registers as "not by player"; the shooter cannot register it either, because its own `InflictDamage` returned before reaching that arm. So a co-op kill on an NPC counts for nobody's stats. Not worth widening this change for. |

#### 1.19.5 The packets

Two opcodes, and a new block for them. `0x70`..`0x7D` is full (the ped and car
handshakes plus their two streams) and `0xD8`..`0xDF` was reserved for "a hosted
ped reaching a state only its host can witness", which is the *opposite* of this
— a hit is witnessed by the shooter and by nobody else. So `0x60`..`0x6F` is the
block for the direction that runs towards an owner; two of sixteen are used and
the rest stay free for the same direction, which is where a limb or a wreck an
observer causes and cannot apply would go.

| opcode | packet | channel | to |
|---|---|---|---|
| `0x68` | `C_PedDamage` — 14 bytes, `PedDamageBody` | `CH_EVENT` | server |
| `0x69` | `S_PedDamage` — 15 bytes, `attackerId` + body | `CH_EVENT` | **the ped's owner alone** |

Point to point like `S_Damage`, and for the same reason: nobody else has
anything to do with it. What the rest of the session needs to see — the flinch,
the limb, the corpse — reaches them from the owner afterwards, on its own ped
stream and on `C_PedBodyPart` and `C_PedDeath`.

Nothing is recorded on the server. Unlike a death, a hit is not a state a joiner
has to be handed: the health it produced lives on the owner's machine, and no
packet has ever carried an ambient pedestrian's health (`AmbientPedState`, and
that omission is deliberate).

Every ownership test in `Session::PedDamageRecipient` is the **inverse** of the
three beside it. A despawn, a limb and a death are refused to anybody but the
owner; this is refused *to* the owner, because a machine reporting a hit on its
own pedestrian is reporting one its own engine already applied, and relaying it
back would apply it twice.

`PROTOCOL_VERSION` is deliberately left at 19. Nothing existing moved and no
layout changed, so the number is the lead's to assign at the merge — the same
way version 18 collected ten branches that each left theirs blank.

#### 1.19.6 The same gap exists for vehicles, and it is worse-shaped

Checked while this was written, reported rather than fixed.

**Vehicles: the gap is real.** *(Closed for a car somebody is driving — §1.20.
Closed for ambient traffic too, with the host as the owner - §1.23.)* Nothing
samples damage on a replica. The only
reporter of a car's condition is its *driver* (`WorldBridge::
SampleLocalVehicleDamage` is explicitly "the car the local player is driving.
False on foot, as a passenger"), and `game/vehicle.cpp` says in as many words
that "a replica is never announced at all". So a player shooting a replica of
somebody else's traffic car takes health off the local copy and nothing
travels — and unlike a pedestrian, a car is not proof against anything, so the
symptom is not "nothing happens" but **divergence**: the replica can burn out
here and stay whole there. §1.15 already records that `AmbientCarState` carries
no condition, so the two copies have independent healths from the frame the
replica is created; roadmap.md §5.8's wreck report is the backstop and it only
runs host → observer. Fixing it properly is the same shape as this change:
`CVehicle::InflictDamage` is `0x00551950` and already verified, and a
`C_CarDamage` in the free half of the `0x6x` block would do it.

**Breakable objects: not this gap, and a smaller one.** `C_ObjectBroken` is sent
by whichever machine's engine broke the object, and `game/object.cpp` already
refuses to report a break a *replica* caused (`BreakCause::REPLICA`), deferring
to the owner — so for objects the observer → host direction is deliberately
absent and correctly so: a map object exists identically on every machine, so
each one breaks its own copy when its own player drives into it. What is not
covered is a break with no owner at all: `m_pDamageEntity` is a collision
record, so a lamp post *shot* rather than driven into reads as
`BreakCause::NOBODY`, which `MayReportBreak` gives to the host. A non-host player
who shoots a lamp post therefore knocks it over on their own screen only. That is
a different bug in a different file and it is left alone here.

### 1.20 Shooting a car somebody else is driving: the gap §1.19.6 reported

§1.19 gave a pedestrian the direction the crowd never had. This is the same
direction for a car, and the hole is shaped differently enough that repeating
the pedestrian argument with the nouns changed would get it wrong.

#### 1.20.1 The symptom is divergence, not silence

A replica pedestrian is bullet-, fire-, melee- and explosion-proof on purpose,
so a shot at one was refused by the shooter's own engine and **nothing
happened** — no flinch, no blood, no death, on either screen.

A replica car is not. CoopIII sets exactly one proof flag on one
(`bCollisionProof`, `game/vehicle.cpp`'s `SetVehicleObserved`) and **cannot set
the rest**: `bExplosionProof` would stop a replayed blast reaching a car it is
supposed to reach identically on every machine, which is the one thing about an
unowned car that already worked for free. So a shot at a replica car was
*accepted* — by the one machine with no right to decide it. The health came off
a copy nobody else could see, the shooter watched a car smoke and burn on a
number its owner never had, and the owner drove on in a car that was never
touched.

Both screens looked correct, which is what made it worse than the pedestrian
bug. §1.15 already records that no packet carries a car's condition in that
direction, and `SampleLocalVehicleDamage` is driver-only, so the two copies had
independent healths from the frame the replica was created.

#### 1.20.2 Flags were never going to be the mechanism, and the binary says so

`CVehicle::InflictDamage` switches on the damage cause before it consults any
flag, exactly as `CPed::InflictDamage` does (§1.10.2), and its switch leaks in
the same way. At `0x0055199F` it does `cmp eax,13h / ja 0x00551A10` and then
`jmp [eax*4 + 0x006026CC]`. Resolved out of the retail image, that twenty-entry
table is:

| cause | arm | flag |
|---|---|---|
| 0, 1 (`UNARMED`, `BASEBALLBAT`) | `0x005519AB` | `bMeleeProof` |
| 2–7, 13, 19 (the guns, `HELICANNON`, `UZI_DRIVEBY`) | `0x005519BE` | `bBulletProof` |
| 8, 10, 11, 18 (`ROCKETLAUNCHER`, `MOLOTOV`, `GRENADE`, `EXPLOSION`) | `0x005519E6` | `bExplosionProof` |
| 9 (`FLAMETHROWER`) | `0x005519D4` | `bFireProof` |
| 16 (`RAMMEDBYCAR`) | `0x005519FC` | `bCollisionProof` |
| **12, 14, 15, 17, and everything from 20 up** | `0x00551A10` | **none — no flag is read** |

So `DETONATOR`, `TOTALWEAPONS`, `ARMOUR`, `RUNOVERBYCAR`, `DROWNING`, `FALL`
and `UNIDENTIFIED` reach `m_fHealth` with nothing consulted. Even setting all
five bits would not have closed this. The rule is therefore stated positively,
in a detour, the same way M4 stated the player rule and §1.19 stated the
pedestrian one: **nothing on this machine may take health off a car this
machine does not drive**, and anything the local player did on purpose becomes
a packet instead. The one flag stays, because a detour that failed to install
has to fail closed.

#### 1.20.3 Exactly the three arguments, and no more

`CVehicle::InflictDamage` is `0x00551950`. That address was already in the
tree; it was re-verified from the file for this change rather than inherited,
because one address in this project was found three bytes off in the same week.

- **It is a function start.** `0x00551944`–`0x0055194F` is twelve bytes of
  `0x00` alignment fill after the previous function's `jmp` at `0x00551942`;
  the entry is `push ebx / push esi / mov esi,ecx / push ebp / sub esp,10h`.
- **It is `__thiscall`** — `mov esi,ecx` is what makes every `[esi+…]` in it a
  member of `this`.
- **It takes three arguments.** Every one of its seven exits is
  `add esp,10h / pop ebp / pop esi / pop ebx / ret 0Ch`, the last at
  `0x00551C83`. Twelve bytes is three dwords, and the callee reads them at
  `[esp+20h]` (culprit), `[esp+24h]` (weapon) and `[esp+28h]` (damage).
- **The call site agrees**, which is the independent witness:
  `0x004B18BE push eax / mov ecx,ebp / fstp dword [esp]` (the float) then
  `push 12h` (the weapon) then `push [esp+0DCh]` (the culprit).
  (`addresses.h`'s existing transcription of that site shows the `fld`/`fmul`
  and the two pushes but not the `push eax / fstp` pair between them, which is
  where the float actually lands. The conclusion was right; the listing was two
  instructions short, and it is corrected there.)
- **It returns nothing.** The last exit leaves `eax` holding a model index from
  `movsx eax,word [esi+5Ch]`. Anything reading a bool out of it is reading a
  leftover.

So the wire carries `netId`, `weapon` and `amount`, and that is the whole
packet — seven bytes. `PedDamageBody` is nine; the two it has that this does
not are `pedPiece` and `direction`, and a car has neither because there is no
limb to take off and no knockdown animation to choose.

**No health.** This is the field whose absence is load-bearing and §1.11.1 is
the whole argument: `InflictDamage` writes `mov dword [esi+200h],0` at
`0x00551C10` and then, sixty-nine bytes later, calls `BlowUpCar` through the
vtable at `0x00551C5A`. The zero and the destruction are two acts and only the
second destroys anything, so a health copied off a socket produces a car with
no health that is not wrecked — and one below 250 arms the five-second fire
timer (§1.11.3), which is an observer deciding, five seconds later, that
somebody else's car is finished. Health travels the way it always has: as a
field on the driver's own 25 Hz snapshot, out of the machine that owns it.

**No position and no shot vector.** The shooter's engine already resolved the
ray; what crosses the wire is its conclusion, not its inputs. A position would
invite the owner to re-resolve it against a car it holds somewhere else — the
observer deciding damage with the arguments the wrong way round — and would put
a second, competing source of truth next to the transform stream.

**No panels and no doors.** Those already travel, as absolute state from the
driver on `C_VehicleDamage` (§1.15), and the owner's own `InflictDamage` →
`CAutomobile::VehicleDamage` is what produces them in the first place. Sending a
dent here would double-count against a record whose whole arbitration is that
it is a monotone maximum.

**Nothing carries the result back, and that is the point.** The health rides the
driver's snapshot, the dents ride `C_VehicleDamage`, and the wreck rides
`C_VehicleBlowUp` at the transform the owner's own physics chose. The observer
writes that health into its copy and the fire timer is already held at zero
under it (`ApplyRemoteVehicle`), so the flames and the smoke appear on every
screen at the same moment and only one machine ever decides.

#### 1.20.4 Only a car somebody is driving, and why the other three are not this

A car in a CoopIII session is one of four things, and only one of them has a
machine entitled to decide its condition:

| kind | owner | what carries its condition |
|---|---|---|
| a session car with a **live driver** | that driver's machine | **this change**, plus the snapshot, `C_VehicleDamage` and `C_VehicleBlowUp` |
| a session car somebody **parked and walked away from** | nobody | `C_UnownedBlowUp` with `UNOWNED_SESSION` |
| a **map-parked** car out of a car generator | nobody | `C_UnownedBlowUp` with `UNOWNED_PARKED` |
| an **ambient traffic** car | its host (since §1.23, for its condition as well as its transform) | `C_CarHit`, the car stream's `health`, and `C_UnownedBlowUp` with `UNOWNED_AMBIENT` |

The bottom three are refused outright, by the detour (it only fires for a car
with a recorded remote driver) and again by `Session::VehicleHitRecipient`.

For the two ownerless kinds there is no recipient to send to — that is what
"unowned" means here. Each engine damages its own copy; a replayed blast damages
every copy *identically*, because `CWorld::TriggerExplosionSectorList`'s
multiplier is a function of the blast position and the car's position and
nothing else; and what does not converge — accumulated gunfire, a shove — is
exactly what roadmap.md §5.8's wreck report was built to carry, after the fact.
Routing their hits through here would invent a fourth ownership model for a
problem that is already closed, and would require the server to keep a health
for every car in Liberty City.

Ambient traffic is the interesting one, because it *does* have a host. It is
still left out, on three counts: its netIds are a different table (the same
reason `C_PedDamage` is a separate packet from `C_Damage` rather than a flag on
it), its session row carries no condition at all and deliberately so
(`AmbientCarState`, population.md §2.1's bandwidth argument), and its
divergence is already bounded by `UNOWNED_AMBIENT` — the host decides the wreck
and every observer replays it at the host's transform, so the intermediate
health disagreement is never visible. A driven car has no such bound, because
`C_VehicleBlowUp` is only accepted from its driver and its driver never heard
about the hits.

*(Superseded by §1.23. The third count didn't hold: the replica wasn't bounded
by `UNOWNED_AMBIENT`, because nothing stopped it blowing up on its own first,
and that was visible. Traffic now gets its own pair, `0x6C`/`0x6D`, with the
host as the owner.)*

#### 1.20.5 What the wire now trusts a client to assert

§1.19.3 named the trust `C_PedDamage` added: a client may tell another machine
to hurt a pedestrian it owns, and the owner cannot check the claim because the
claim is about a ray traced somewhere else. This goes one step further **in
kind**, not just in scope.

A pedestrian is a thing in the world. A car somebody is driving is a thing that
player is *using*, and the consequence of a hit is not a flinch — it is
potentially the destruction of that player's vehicle, at a moment and a position
the owner's engine picks, in the middle of whatever they were doing with it.
What a client is now trusted to assert is: *that its player really did land a
hit, on that car, with that weapon, for that much.*

The bounds are: the cause must be in `IsForwardableDamage` (a ray or a melee
reach the shooter's own engine resolved — the same set a player and a pedestrian
allow), the amount must be finite, positive and under `MAX_REMOTE_DAMAGE`, the
target must be a car the server records **another** player as driving, and the
car must not already be a wreck. Nothing checks that the shooter was anywhere
near the car, had line of sight, was holding that weapon, or fired at all. A
hostile client can drive any other player's car to zero health at the packet
rate, from anywhere on the map. `MAX_REMOTE_DAMAGE` exists so a corrupt float
cannot become a NaN in `m_fHealth` and from there in the car's matrix — memory
safety, not anti-cheat. §2.1 already says a malicious client can cheat and that
this is a deliberate trade for a co-op mod; this widens that trade from
"anybody's pedestrians" to "anybody's car", and nothing else. No player's health
and no player's position is reachable through it.

#### 1.20.6 Every failure mode, and the choice made for each

| case | choice |
|---|---|
| **The car is already a wreck on the owner's machine** | Refused three times over, and the innermost one is the engine's own: `CVehicle::InflictDamage` compares `m_fHealth` against the `0.0f` at `0x00602534` at `0x00551A10` and leaves for the exit before any arithmetic. The server drops it earlier (`VehicleHitRecipient` checks `destroyed`), the owner's client drops it earlier still (`RemoteVehicle::destroyed`), and the shooter does not send it at all if the car reads `STATUS_WRECKED` on its own screen. A burst that was in the air when the car went up is the ordinary case, not a rare one, so all three are worth having. |
| **Two observers report hits on the same car** | No dedup, deliberately, and this is the opposite decision from `C_VehicleDamage` on the same object. That packet is *absolute* state merged as a maximum, so a duplicate must change nothing; this is a *delta*, so two players shooting one car is two real hits and single player applies both. What keeps it honest is that a hit is only reported by the machine whose own player landed it (`culprit == PlayerPed()`), so one trigger pull produces exactly one report from exactly one machine. The transport is reliable-ordered ENet, which does not duplicate, so no sequence number is added — the same reasoning §1.19 gives. The *destruction* is still single: only the owner ever calls `BlowUpCar`, once, and `C_VehicleBlowUp` is one event. |
| **A report names a netId the owner no longer has** | Drop it, three layers deep: the server refuses a netId with no row, the owner's client refuses one with no active roster entry or that it is not driving, and `ResolveRemoteVehicle` returns null for a car the pool has reaped. Not retried, and that is the decision rather than an omission — a hit is only worth anything on the car that was standing there. Said once in the log and then quiet. |
| **A car that is unowned traffic, parked, or abandoned** | Not this packet. §1.20.4 is the argument; the short version is that those have no machine entitled to decide their condition, blasts already converge on them for free, and `C_UnownedBlowUp` carries what does not. Traffic has its own packet now, `C_CarHit` (§1.23). |
| **A blast** | Never routed through this packet, and — new for a car — **also refused locally**. Not forwarding it is the argument `IsForwardableDamage` already makes for a player and a pedestrian: an explosion is replayed on every machine at an agreed position, so the owner puts its own car in its own blast and its own engine decides, and forwarding would apply it twice. Refusing it locally is the half a pedestrian did not need: a replica ped is `bExplosionProof` and a replica car is not, and unlike a player the observer's copy cannot then blow up (the `BlowUpCar` detour refuses a car a remote player is driving), so without the refusal it would sit at zero health, on fire, forever. That is §1.11.1's bug exactly. |
| **Fire, ramming, running over, drowning, a fall** | Refused for the same reasons §1.10.1 refuses them for a player. Ramming is worth restating for a car: `WEAPONTYPE_RAMMEDBYCAR` is the one cause `bCollisionProof` already blocks completely (`0x005519FC`), and it must stay blocked, because a remote car's transform is corrected 25 times a second rather than simulated and a "collision" with one is not a collision anybody ran. |
| **A car with `bOnlyDamagedByPlayer`** | A known residual, stated rather than worked around. At `0x00551972` the engine demands a culprit that is `FindPlayerPed()` or `FindPlayerVehicle()` and returns otherwise. On the owner's machine the culprit is a replica of the *shooter's* ped, which is neither, so such a car takes nothing off the wire. The only way round it is to name the local player as the culprit, which is a lie about who fired, and the flag exists precisely to stop anybody but the player hurting that car. The owner's log says so, with `bOnlyDamagedByPlayer` named, when a hit lands and takes no health. |
| **The local player gets into the car between firing and sending** | Dropped at the send (`VehicleHitIsWorthSending`). Our own engine is the one deciding now, so sending would be asking the server to route a hit back to us. |
| **A jack, so two machines each think they drive it** | The server decides, exactly as version 22 says. Both ends ask their own question anyway — the client asks its roster before sending to the seam, and the seam asks `CVehicle::m_pDriver` before calling the engine — and those are two different questions on purpose: the engine's answer moves first and the session's catches up. |
| **A city NPC shoots a replica** | Not forwarded. Only what the local player did deliberately, the same test and reason as the pedestrian branch: a shot fired by traffic happened in one simulation and not in the others, and forwarding it would have this machine's NPCs shooting up another machine's cars. |
| **Kill credit and `AwardMoneyForExplosion`** | Untouched, and untouched on purpose. If this hit is the one that sets the car alight, `m_pSetOnFireEntity` is written at `0x00551BF3` with the shooter's replica, so the car that burns out blames the player who shot it. The money is the owner's engine's business and stays there. |

#### 1.20.7 The packets

| opcode | packet | channel | to |
|---|---|---|---|
| `0x6A` | `C_VehicleHit` — 12 bytes, `VehicleHitBody` | `CH_EVENT` | server |
| `0x6B` | `S_VehicleHit` — 13 bytes, `attackerId` + body | `CH_EVENT` | **the car's driver alone** |

Named "hit" and not "damage" because `C_VehicleDamage` (`0x3A`) already exists
and is a different thing entirely: absolute cosmetic state, sent **by** the
driver, merged as a maximum. This is a delta, sent **to** the driver, never
merged. Two packets called damage on the same object travelling in opposite
directions with opposite arbitration is a name nobody could keep straight.

`Session::VehicleHitRecipient` is the exact inverse of
`Session::MayReportVehicle`, and the pair is the invariant `sessiontest` pins:
for a given car and a given player, exactly one of "may I describe it" and
"should I be told about a hit on it" is true, and which one never depends on
anything but the driver.

Nothing is recorded on the server. A hit is not a state a joiner has to be
handed — the health it produced lives on the owner's machine and reaches the
session on the snapshot that has always carried it (§2.8.1).

`PROTOCOL_VERSION` is deliberately left at **22**. Nothing existing moved and no
layout changed, so the number is the lead's to assign at the merge — the same
way version 18 collected ten branches that each left theirs blank, and the same
way §1.19 left 19.
### 1.21 A car nobody is driving: one machine settles it, everybody else pins it

Protocol 22 settled who owns a car with somebody in it — the player in seat 0,
named by the server, and nobody else may report it. It left the gap between an
exit and the next enter owned by nobody, and §5.8.1's rule for that gap is
that every machine holds the car at the last transform the session gave it,
with its controls and velocities at rest.

**That is right for a car standing in the street and wrong for one that was
still moving when the session stopped having a driver for it.** The pin is
applied *after* physics, every frame, so whatever pose the car was in at that
instant is permanent. And `CVehicle::CanPedEnterCar` (`0x005522F0`) refuses a
car whose `up.z` is **inside** ±0.1 — a car on its side, not one upright —
while `CPed::SeekCar` (`0x004D3F90`) answers that refusal with
`RestorePreviousState` and no timeout. A car that goes onto its side while
nobody is recorded driving it is a car nobody can ever get into again, on every
machine, for the rest of the session.

That pose is not exotic. `CVehicle::CanPedExitCar` (`0x005523C0`) refuses to
let anyone step out of a car doing more than `0.005` — so a player cannot get
out of a rolling car by hand. They *leave* one by dying in it, by being jacked
out of it, or by disconnecting, and all three run through
`Session::NoteExitVehicle`.

#### 1.21.1 Custody

A driverless car gets a **custodian**: one machine, named by the server, that
stops correcting the car and lets its own engine finish what the car was
doing, streaming the result on the `C_VehicleState` the protocol already has.
Every observer follows it on the interpolation that already exists. When the
car comes to rest the custodian says so and the session goes back to nobody
simulating it — the pinned, zero-bandwidth, perfectly still behaviour a parked
car has always had, unchanged line for line.

**Custody is the exception; rest is the rule.** A car with no custodian takes
exactly the code path it took before any of this, which is what makes the
common case — a car parked on the street for ten minutes — provably
unregressed rather than merely re-tuned.

#### 1.21.2 Why the last driver and not the host

`roadmap.md` §5.8 says an ownerless world entity is the host's, and for a
*fact* about a car nobody owns that is right: the host is one machine and it is
always there. It is the wrong machine to run a car's **physics** on. GTA III
streams around one player (`roadmap.md` §2.1) and keeps one island's collision
in memory (§2.2), so a host on the other side of the river would be simulating
a car with no ground under it and reporting the fall — and every observer would
follow it down, which is a worse bug than the one being fixed.

The player who has just stepped out is standing next to the car with the
collision loaded around it and was simulating it a frame ago. That is the whole
argument, and it is also why custody is short: `VEHICLE_SETTLE_MS` (2 s) on the
custodian's own clock, after which the car is handed back whether or not it
settled.

#### 1.21.3 What the server arbitrates

Nothing a client decides for itself. A machine is the custodian when, and only
when, it has been told so by `S_VehicleCustody`. `Session::MayReportVehicle`
reads driver and custodian as a **precedence**, not a union:

```
the driver, if there is one; otherwise the custodian; otherwise nobody
```

so even a record that somehow held both still names exactly one reporter. Two
machines can no more both be the custodian than both be the driver, which is
the same discipline and for the same reason: both bugs protocol 22 fixed were a
client working out an ownership for itself and being right from where it stood.

Custody ends four ways, and only the first costs a packet:

| | |
|---|---|
| the custodian's `C_VehicleSettled` | refused from anybody else |
| a new driver | `NoteEnterVehicle` clears it; the `S_EnterVehicle` already says it |
| the car's destruction | `DestroyVehicle` clears it; a wreck has nothing left to settle |
| the custodian disconnecting | `RemovePeer` clears it; handed to nobody, not to the next player along |

`S_VehicleCustody` is always sent **after** the `S_ExitVehicle` that created the
vacancy, on the same reliable ordered channel — the mirror of 22's
loser-before-winner rule, and what guarantees no client holds a driver and a
custodian for one car at the same instant.

#### 1.21.4 When a car has stopped

Asked of the engine, and agreeing with it rather than second-guessing it.
`CPhysical::ProcessControl` (`0x00495F10`) counts quiet frames in
`m_nStaticFrames` (`+0xED`), and on the frame that takes the counter past ten
it sets `bIsStatic`, zeroes both velocity vectors outright and returns without
applying either. Any moving frame puts the counter back to zero.

So the custodian's test is `bIsStatic` **or** a run of `VEHICLE_REST_FRAMES`
(11) consecutive quiet samples, and quiet means `CanPedExitCar`'s own numbers —
`|moveSpeed|² ≤ 0.005` and every component of `turnSpeed` within `0.01`. Those
are the tightest gates in the engine, which is the point: a car handed back to
the pinned world has to be one a player can get into **and** out of, and the
exit gate is eight times stricter about speed than the entry gate.

A run rather than an instant, because one still sample is not rest — a car at
the top of a bounce reads still for exactly one frame, and handing it back
there pins it in mid-air, which is the same bug reached by being impatient.

**What this deliberately does not do** is right a car that has landed on its
side. Nothing in GTA III does, single player included, so the car stays there
and stays unenterable — which is fidelity (`roadmap.md` §5.5), not a residual.
The bug was the pin, never the pose.

### 1.22 Getting into somebody else's traffic is an ownership change

`roadmap.md` §5.8.1's last paragraph and `population.md` §1.3.1 both name this
and neither fixed it. An ambient car belongs to the machine whose `CCarCtrl`
made it and that ownership never moves. `CorrectAmbientCarReplica` only
*guards*: it stops correcting a replica the local player is driving, so the car
moves on that screen while the session goes on telling everybody else where its
original host thinks it is.

**It becomes a session car, which is the claim the session already has.** Not
something narrower, and the reason is one thing the ambient roster cannot
carry: **a seat**. An `AmbientCar` has an owner and no seats, so a player at
the wheel is invisible to it — every observer would go on drawing that player's
ped in the road beside the car, which is exactly the bug the seat work fixed
for session cars. Damage, destruction and the backfill's record of a car's
condition all ride the session car as well; the ambient row only ever carried a
transform.

It is also the answer the codebase already gives from the other side.
`population.cpp`'s `SweepHostedCars` already retires a hosted car and hands it
to the M2 claim path the moment the *local* player gets into one of its own.
This is that, applied to the machine holding the replica.

#### 1.22.1 No new claim packet, and nothing created or destroyed

The claim is `C_EnterVehicle` exactly as it is, sent with the netId the session
already has for the car. netIds are one space (`Session::AllocNetId` is one
counter for players, vehicles, ambient peds and ambient cars), so a number that
names an `AmbientCar` can never also name a `Vehicle` — the server tells which
kind of claim it is holding by looking, and there was nothing to add.

The server promotes the row **under the same netId**, and `S_CarPromoted` tells
every machine to move its bookkeeping across. That is what lets every machine
keep the `CVehicle` it already has:

| machine | what it has | what changes |
|---|---|---|
| the new driver | a replica CoopIII built | the row; the object is untouched |
| any observer | a replica CoopIII built | the row; the object is untouched |
| the original host | a car its **own engine** made | the row, plus it stops hosting it |

The last one is why the netId is kept. A promotion that despawned and
respawned would have that machine run the deleting destructor on one of the
player's own traffic cars — the thing `RemoteVehicle::ours` exists to prevent —
and would flicker the car on every other screen as well. The promoted row is
marked `ours` on exactly that machine, so the roster can never destroy it.

The AI driver its own `CCarCtrl` put at the wheel comes out through the seat
path that already exists: `S_EnterVehicle` records the driver,
`Client::UpdateRemoteSeats` carries it out, and `SeatPedInCar` opens by calling
`EvictSeatOccupant` for precisely this case.

**Seat 0 only.** A passenger in somebody else's traffic changes nothing about
who is steering it, and promoting a car because somebody got into the back
would take it off the machine still driving it.

### 1.23 Shooting somebody else's traffic

Found by the replica-death work (`population.md` §5.7.4) and confirmed against
the binary and the code. A replica of somebody else's traffic car had no
protection at all. `SpawnAmbientCarReplica` set no proof flag,
`CorrectAmbientCarReplica` set only `bCollisionProof`, and both detours in
`game/vehicle.cpp` only refused cars with a recorded remote driver. So bullets,
fire and blasts took health off the replica, and at zero the observer ran
`CAutomobile::BlowUpCar` on a car its host never touched. `BlowUpCar`
(`0x0053BC60`) reads no proof flag, only `bCanBeDamaged` at `0x0053BC69`, and it
kills a seated occupant through `CPed::SetDead` (`0x0053BDCD` driver,
`0x0053BE2F` passengers), which is where most replica pedestrian deaths came
from. The shell then followed the host's stream around town, since nothing in
the correction checks for a wreck.

#### 1.23.1 The host decides

The machine whose `CCarCtrl` made the car. It already streams the car, and
`UNOWNED_AMBIENT` already makes it the only machine that may say the car blew
up, so the health that leads there is the same decision.

The worry with a host is distance (§1.21.2 gives custody to the last driver
rather than the host for that reason). It doesn't apply here. §1.21.2 is about
running a car's physics on a machine that may not have it loaded. A traffic car
only exists while its host keeps it: `CCarCtrl::PossiblyRemoveVehicle` measures
an unlocked random car's distance from the host's own player (`0x0041869B`) and
deletes it past that (`CWorld::Remove` at `0x0041870E`), and the replicas go
with it on `C_CarDespawn`. So whenever there is a replica to shoot, the host has
the car and the ground under it. Applying a hit is arithmetic on that car, not
a simulation of it.

It also buys something no observer could do: on the host, `InflictDamage`'s
`RANDOM_VEHICLE` arm (`0x00551A27`) runs on a car with a real autopilot, so the
AI driver floors it or bails out the way it does in single player. A replica's
status is `STATUS_ABANDONED` and that arm never ran anywhere before.

#### 1.23.2 Refused together, and the hit still gets there

On an observer, both detours treat a replica like a car somebody else drives:
`CVehicle::InflictDamage` is refused and `BlowUpCar` is refused.
`game/vehicle.h` has the decision as `ClassifyCar` / `DecideCarDamage` /
`MayBlowUpCar`, and `clienttest` checks the two halves agree for every car and
every cause. Refusing only the blow-up would leave the car at zero health on
fire for good, which is §1.11.1 again.

A hit the local player lands with a ray or a melee reach
(`IsForwardableDamage`) goes to the host as `C_CarHit` (`0x6C`). The server
sends it on as `S_CarHit` (`0x6D`) to the car's owner and nobody else
(`Session::CarHitRecipient`), and the host feeds it into its own
`InflictDamage` with the shooter's ped as the culprit. The body is
`VehicleHitBody`, the same three fields as `C_VehicleHit`. It is a separate pair
because the server finds the owner in a different table by a different rule.

The replica can only end one way: the host's `C_UnownedBlowUp`, applied
through `BlowUpCarAsOwnerSaid`, which is the one call the detour lets through.

The detours aren't the whole of it. Three writers take health off a car without
calling `InflictDamage`: the upside-down drain at `0x0052F472` (above
`VehicleDamage`'s `bCollisionProof` test), `CFire::ProcessFire` writing 75.0f on
the car of a burning occupant at `0x00479959`, and the engine-status drain at
`0x005347E0`. So `CorrectAmbientCarReplica` writes the host's health back every
frame after physics, and holds the fire timer (`+0x530`) at zero, the same split
`ApplyRemoteVehicle` makes for a driven car.

The health comes from the car stream. `AmbientCarState`'s two pad bytes are now
`health`, in whole points; 0 means "not said" and the receiver keeps what it
had. So the smoke and the flames show up on every screen when the host's car
starts burning, and not only when it blows up.

#### 1.23.3 Explosions

A blast is refused on the replica and not forwarded. Every machine replays the
explosion at the agreed position (§1.9.3), so the host's own copy of the blast
reaches the host's car and decides it. If that kills the car,
`C_UnownedBlowUp` brings the wreck to everybody at the host's transform. Each
blast is counted once, on the machine that owns the car, and the observer's
copy never takes damage it could double.

Fire is refused and not forwarded for the same reason. A `CFire` on a car can
be lit by a replayed explosion as easily as by a flamethrower, and
`InflictDamage` can't tell which, so forwarding fire would count the host's own
fire twice. The flamethrower therefore does nothing to somebody else's traffic,
the same as it already did to a car somebody else drives. That is the one
weapon this leaves out. *(§1.24 takes it back in, from the ignition rather
than the damage.)*

#### 1.23.4 Parked cars

Not in scope, and unchanged. A car generator's car is the map's, it exists on
every machine and nobody hosts it, so there is no owner to send a hit to. Each
machine damages its own copy, a replayed blast reaches every copy the same way,
and `UNOWNED_PARKED` carries a wreck from whoever sees it first. Health between
the first shot and the wreck can differ between screens. The wreck can't.

#### 1.23.5 Also fixed on the way

A promoted traffic car (§1.22) never went into the table the protocol-23 detours
read, because `SpawnRemoteVehicle` never ran for it. On every observer,
including the old host, it was a car somebody drives that this machine still
damaged and blew up on its own, and whose hits never reached the driver.
`AdoptPromotedCar` now registers it.

The same gap had one more car in it: the one you claimed yourself. The claimer
keeps a roster row for its own car (`RemoteVehicle::ours`) but the table only
got rows from `SpawnRemoteVehicle` and `AdoptPromotedCar`, and neither runs for
it. So once you got out and somebody else drove it, your engine still dented
and wrecked your copy on its own, and your shots never reached them. A car you
parked and then blew up here also went unreported, because without a row the
wreck had no `UNOWNED_SESSION` name. The claim reply now registers it
(`AdoptClaimedVehicle`), and when the session drops an `ours` car the row goes
with it (`ReleaseOwnVehicle`) instead of outliving the session with the last
driver's id in it. No wire change.

#### 1.23.6 Packets

| opcode | packet | channel | to |
|---|---|---|---|
| `0x6C` | `C_CarHit` - 12 bytes, `VehicleHitBody` | `CH_EVENT` | server |
| `0x6D` | `S_CarHit` - 13 bytes, `attackerId` + body | `CH_EVENT` | **the car's host alone** |

`AmbientCarState::health` (`u16`, was `pad[2]`) on the existing car stream.
`PROTOCOL_VERSION` is left for the merge; protocol.h has the unnumbered entry.

### 1.24 A flame is an ignition, and the owner lights it

§1.19.4, §1.20 and §1.23.3 all left the flamethrower out for the same reason:
by the time its damage reaches `InflictDamage` it is just a fire, and a fire
lit by a replayed explosion looks the same. That's true, and it's the wrong
place to look. The flamethrower never calls `InflictDamage` at all.
`addresses.h` has the whole path; the short version:

```
CWeapon::Fire -> FireAreaEffect (0x00561E00) -> CShotInfo::AddShot (0x0055BD70)
every frame   -> CShotInfo::Update (0x0055BFF0), per slot:
  for each of the source ped's m_nearPeds: in control, close enough,
      not bFireProof (0x0055C1D9) -> StartFire(ped, source, 0.8f, 1)  0x0055C232
  every 4th frame: SetCarsOnFire(pos, 4.0f, source)                   0x0055C26C
      -> StartFire(car, source, 0.8f, 1)                              0x004B3F9C
then CFire::ProcessFire -> InflictDamage(m_pSource, 9, 1.2 * timestep)
```

So the flame's own act is the ignition, and inside `CShotInfo::Update` it is
still recognisable: every `StartFire` made while that function runs is the
flamethrower's, and `fleeFrom` is the slot's own source. Ours when both hold,
`combat.h` `IsOurFlame`. Our molotov names our ped too, but its fire comes out
of `CExplosion::Update`, outside the window. Another player's replayed flame
is inside the window, but its source is our copy of their ped.

**What travels.** The ignition, on the packet that already carries a hit on
that target: `C_PedDamage`, `C_VehicleHit` or `C_CarHit`, cause 9, amount 0.
Cause 9 never meant damage on those packets - `IsForwardableDamage` has always
refused it and every receiver checks - so it can mean "our flame reached
this". At most once a second per target.

**Who lights it.** The owner, with its own `StartFire` on its own entity and
the shooter's ped as `fleeFrom`, after the two tests the shooter couldn't make:
`bFireProof` on the real entity and, for a car, not a wreck. The owner's `CFire`
does all the burning, so the damage, the death and the wreck happen where the
entity lives and travel on the packets that already carry them.

**Pedestrians need one more step.** A replica is `bFireProof`, so on the
shooter's machine `CShotInfo::Update` skips it one test before `StartFire`.
After `Update` returns, the shooter walks the same `m_nearPeds` with the same
position and radius and the same tests, minus the proof flag
(`FlameReachesPed`). Cars need nothing extra: a replica car isn't fire proof,
so `SetCarsOnFire` reaches `StartFire` and the detour sees it there. The
engine still lights our copy of that car. Nothing tells this machine the
owner's car is burning, so that fire is the only one our player sees. It
can't do damage: its `InflictDamage` is refused on a car we don't own, and
cause 9 is still never forwarded as damage.

**No double count.** The owner already lit part of this before: every machine
replays our flamethrower, so the owner's own engine runs a `CShotInfo` off its
copy of our ped and lights what that reaches. Its copy is 100 ms old and aims
along its body, not our camera, so it misses things our screen hit, and the
packet covers those. When both land on one target nothing doubles, because
`StartFire` refuses an entity that is already burning (`0x004795A6` for a ped,
`0x004795DD` for a car) and fire damage is per `CFire`, not per ignition. A
fire from a replayed explosion never reaches the packet at all.

**Seeing it.** Entity fires never travel, so the owner's fire on its own
pedestrian was visible on the owner's screen only: the replica is
`bFireProof` and nothing on an observer can light it. The owner now says so in
the ped stream - `AmbientPedState::flags`, the byte that used to be padding,
bit `AMBIENT_PED_ON_FIRE` when the real ped's `m_pFire` is set - and every
observer puts the same visual-only fire on the replica that a burning player
gets (§1.10.7): `ped.cpp`'s transcription of `StartFire`'s tail with the AI
arm left out, `PlanRemoteFire` deciding it, and out a second after the owner
stops saying so. A car needs nothing like this: the shooter's own copy of the
car is lit by the flame directly (above), and a traffic car's health already
rides its stream, so the owner's smoke shows everywhere.

**Molotovs** are not this shape and need nothing. Their fire comes from the
explosion (`CExplosion::Update`'s molotov arm, table `0x00602E34` entry 1,
calls `SetPedsOnFire` and `SetCarsOnFire` with a 6.0 radius), and every machine
replays that explosion at the thrower's position (§1.9.3), so each owner
lights its own entities.

**Players are unchanged.** A flame on a remote player is still the victim's
own business (§1.10.6): their machine's replay of our flame lights them, and
friendly fire gates the damage.

**Mixed builds** lose the feature and misread nothing. An older owner drops
cause 9 at `IsForwardableDamage`, where it always did, and an older server
relays the packet without reading the cause.

### 1.25 A cheat runs where what it changes is owned

[docs/cheats.md](cheats.md) is the investigation and the table; this is the wire.

A cheat runs on the machine of the player who typed it, and ten of the 23 in
retail 1.0 change something that machine does not own in a session. Typed on a
non-host, `PEASOUP` lasted until the next `S_WorldState` put the host's sky
back. `TIMEFLIESWHENYOU` had a non-host's clock racing the host's and being
jumped back every second. `ITSALLGOINGMAAAD` turned the typist's own
pedestrians and nobody else's.

So the client detours `CPad::AddToPCCheatString`, the only way into a cheat on
PC, and in a session decides before anything runs:

- **the typist's own** (13) run where they were typed and never go on the wire
- **the four skies** go to the host as `C_Cheat` and are not run by a
  non-host at all; the host runs `CWeather::ForceWeatherNow` and sends
  `C_WorldState` at once rather than waiting out its 1 Hz limiter
- **the clock's speed and the crowd** (5) run on the typist's machine and then
  go to everybody, carrying the state they left behind so a receiver arrives
  there instead of flipping a toggle
- **BANGBANGBANG** runs where it was typed; its wrecks travel by §1.11 and §1.23

A receiver brings itself to the state by calling the engine's own handlers
(`PlanRoutedCheat`), never by writing the globals. The server relays by
`CheatRelayFor` and keeps the last state of each everybody-cheat for the
backfill; an emptied session forgets them.

#### 1.25.1 Packets

| opcode | packet | channel | to |
|---|---|---|---|
| `0xF0` | `C_Cheat` - 7 bytes, `CheatBody` (`cheat` `u8`, `state` `u8`) | `CH_EVENT` | server |
| `0xF1` | `S_Cheat` - 8 bytes, `playerId` + body | `CH_EVENT` | the host alone for a sky, everybody but the typist otherwise; replayed to a joiner from `INVALID_PLAYER` |

`S_Welcome.flags` bits 6-7 carry `CheatRule`. `PROTOCOL_VERSION` is left for the
merge; protocol.h has the unnumbered entry.

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

Bandwidth check at 8 players: 7 remotes × 25 Hz × ~78 B ≈ 13.7 KB/s down per
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

`m_vecMoveSpeed` is metres per engine step, and a step is 1/50 s
(`CPhysical::ApplyMoveSpeed` multiplies it by `CTimer::ms_fTimeStep`, which
`CTimer::Update` sets to frame milliseconds × 0.05). It goes on the wire in that
unit, since receivers also write it back into the engine, and is multiplied by
50 where it enters an interpolation buffer (`MoveSpeedToMps` in
`client/src/interp.h`, which has the addresses). The helicopter is the one
exception: `HeliStateBody::velocity` is m/s on the wire.

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

### 2.8 A late joiner has to end up in the same world, not a smaller one

Someone who connects in the middle of a running session gets a *backfill*: a
replay, on the reliable channel, of enough packets to put the session that was
already happening onto their screen. Until protocol 9 that replay described
every object by its **spawn identity** and left its **current condition** to
the live stream, and the failure that follows from it is structural rather
than accidental:

> **Any state whose only carrier is an event is invisible to a late joiner by
> construction.** An event announces a change. Somebody who missed the change
> has no way to learn it happened.

The owner found it through a car he had blown up, which came back to a late
joiner in showroom condition: intact enough to climb into, and not drivable.

#### 2.8.1 Condition rides the packet that already exists

No new opcodes. Every field below went onto a packet the backfill was already
sending, which also means the live announcement and the replayed one are the
same packet built by the same function — so "what a player looks like on the
wire" has one answer and the two can never drift apart.

`S_PlayerJoin` grew `health`, `armour`, `weapon`, `deathAnimId` and a `flags`
byte holding:

| Flag | Meaning |
|---|---|
| `PJF_POS_VALID` | `pos`/`heading` are somewhere the session actually saw this player, not the zeroes a fresh slot starts with. Needed because the origin in GTA III is open water — the first remote ped this project ever created was born there and drowned in eight frames. A receiver without this bit cannot tell "at the origin" from "never heard from". |
| `PJF_DEAD` | Dead and waiting to respawn, with `deathAnimId` from their own engine's `SetDie` so a backfilled corpse lies the way it fell. This is the flag the version is about: a death has exactly one carrier, `C_Death`, so before this a joiner arriving while somebody lay in the road got a live player, standing up, on zero health. |

`S_VehicleSpawn` grew `health` and `flags`, the same flag byte the vehicle
snapshot uses, plus one new bit:

| Flag | Meaning |
|---|---|
| `VEH_WRECKED` | Destroyed. Not "on low health" — finished. Health alone does not carry it: writing zero into `m_fHealth` produces a car that reads dead and behaves brand new, because destroying a car is something the engine *does* (`BlowUpCar` and the status change with it), not a number it stores. So the sender says it outright and no receiver has to infer it from a float. |

#### 2.8.2 A passenger's seat is state, not an announcement

`EnterVehicleBody` has carried `seat` since protocol 3 and the client has
carried it end to end since seating was written. The **session** only ever
wrote down drivers, so a passenger's seat existed nowhere but in the
`S_EnterVehicle` that announced it — and an event only reaches whoever was
connected at the time. Everyone already in the session watched the passenger
get in; the next player through the door was told about a car with an empty
passenger seat and a player jogging along beside it. Nothing on the wire
changed to fix this; the server writes the number down now.

#### 2.8.3 Only the recorded driver may say what shape a car is in

The gate on `C_VEHICLE_STATE` used to be "drop this if we think the sender is
in some *other* car", which let a snapshot through whenever the session
happened to think the sender was on foot. Waving a stray position through was
survivable. Waving the condition fields through is not, because the same
packet now carries `VEH_WRECKED`: the permissive branch was a way for any
player in the session to delete any car from every future backfill. The gate
is now "the sender is the driver this session has recorded for that car", and
nothing else.

#### 2.8.4 A car out of the backfill is a real car, and anyone can get into it

The other half of the owner's report, and the half that explains
"no se puede manejar ni nada".

A synced car only ever exists as a `CVehicle` **CoopIII created** on a machine
that was not in the session when the car was claimed — for everybody else it
is a car from their own world that they happened to get into. So this is a
late joiner's problem by construction, and with two clients started together
it never happens at all.

The joiner climbs into that car and the claim goes out as
`C_ENTER_VEHICLE` with `netId == INVALID_NETID`, which means *a car the
session has never seen*. The server dutifully allocates a second netId for a
car it already had. Now every other machine spawns a duplicate on top of the
original, and the joiner is simultaneously **driving** one netId and
**observing** the other — the same physical vehicle. `CorrectRemoteVehicle`
runs after `CGame::Process` on every frame and puts an observed car back where
the session last saw it, so the engine turned the wheels, worked the
suspension, played the engine note, and the car never went anywhere.

Two rules, and the second is the converse of the observer rule in §1.2:

- A client claims a car by the netId the session already has whenever the car
  it just got into is one of ours. Identity is matched on the engine's own
  `CPools::GetVehicleRef`, never on model and position — two identical parked
  cars side by side are an ordinary sight in Liberty City.
- **A driver decides where their own car ends up and nothing else may.** A car
  the local player is driving is not corrected and is not written to, even
  though the session still has a row for it. Its own reports go into that
  row's interpolation buffer anyway, so the moment the driver steps out the
  car is held where they parked it — which is where every other machine in the
  session has it.

#### 2.8.5 What is deliberately not carried, and why

- **Transient motion.** `moveSpeed`, `turnSpeed`, `steer`, `gas`, `brake`,
  `gear`, `moveState`, `pedState`, `aimYaw`/`aimPitch` and the animation
  block. For anything that is sending snapshots these are 40 ms from being
  right, and 40 ms of a car with the wrong gear is not worth a byte. For
  anything that is *not* sending snapshots — a player in the frontend, on a
  loading screen, in a cutscene — the zeroes describe a stationary idle ped,
  which is what they are.
- **Whether a player is on fire.** `CPed::bIsOnFire` is not synced at all, in
  either direction, so there is no join-time gap to close: a player who was
  here from the start sees no flame either. Fire becomes world state in
  roadmap §5.7 phase two and gets a join-time carrier then.
- **A car's extra components** (`CVehicle::m_aExtras`). Also not a late-joiner
  gap: `CVehicle::SetModelIndex` copies `CVehicleModelInfo::ms_compsUsed` into
  them at construction, so every machine picks its own set the moment it
  spawns the car, and two players who joined together already disagree.
  Roadmap §5.9.
- **An arrest.** Unlike a death there is nothing to rebuild: a busted player
  is alive, standing or sitting where the cop caught them, and their
  snapshots say so. The police station reaches the session as a respawn
  (§1.10.8), which is what moves a joiner's copy of them.
- **Chat history.** A joiner starts with an empty log. Nothing reconstructs a
  conversation from a session it was not in, and no game in this category
  tries.
- **A wreck.** A destroyed car is kept as a session row so its netId stays
  spoken for, and left out of the backfill entirely, because nothing on the
  client can yet *build* a wreck. Reversible in one branch: the packet
  already carries everything a receiver would need. See roadmap §5.8.

---

## 3. v1 sync scope

In:

- On-foot player: position, heading, move state, animation, health, armour
- Weapons: current weapon, aim direction, shot events
- Vehicles: the driver's vehicle (transform, steer/gas/brake, health, engine)
- Enter/exit, using the engine's own API so animations play correctly:
  `SetEnterCar` (`0x004E0920`), `SetExitCar` (`0x004E1010`),
  `QuitEnteringCar` (`0x004E0E00`), `SetPedPositionInCar`. Done, with the
  warp behind it as the guarantee; §1.14. Carjacking is the exception -
  `SetCarJack` (`0x004E0220`) is verified and deliberately not called
  (§1.14.6)
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
- Stats. (Pickups are in - §1.13; the wanted level is in as of 2026-09-22 -
  §1.14.)

Rationale: the smallest slice where two people can drive around Liberty City
together and it feels right. Everything cut is cut because it needs an ownership
model that doesn't exist yet.

---

## 4. Packet list (v1)

`C_` = client→server, `S_` = server→client.

| Opcode | Name | Ch | Payload |
|---|---|---|---|
| 0x01 | `C_HELLO` | 1 | protocol version, nickname, model id |
| 0x02 | `S_WELCOME` | 1 | your `playerId`, your `netId`, server tick rate, world state, `hostPlayerId` (§2.7), session flags - friendly fire (§1.10.3), ammo sync (§1.9.6), the wanted rule (bits 2-3), the rampage rule (bits 4-5) and the cheat rule (bits 6-7, §1.24) |
| 0x03 | `S_PLAYER_JOIN` | 1 | `playerId`, `netId`, nickname, model id, transform, then condition: health `float`, armour `float`, weapon `u8`, flags `u8` (`PJF_POS_VALID`, `PJF_DEAD`), death anim `u16` (§2.8.1) |
| 0x04 | `S_PLAYER_LEAVE` | 1 | `playerId`, reason |
| 0x10 | `C_PLAYER_STATE` | 0 | pos `float[3]`, heading `float`, `m_vecMoveSpeed` `float[3]`, `m_nMoveState` `u8`, `m_nPedState` `u8`, anim `u16` + time `float` + speed `float`, partial anim `u16` + time `float`, health `float`, armour `float`, weapon `u8`, the held weapon's ammo (clip `u16` + total `u32`, §1.9.6), aim yaw/pitch `float[2]`, flags `u8` (71 bytes) |
| 0x11 | `S_PLAYER_STATE` | 0 | `playerId` + the above |
| 0x12 | `C_VEHICLE_STATE` | 0 | `netId`, pos `float[3]`, quat `float[4]`, move/turn speed `float[6]`, steer/gas/brake `float[3]`, gear `u8`, health `float`, flags `u8` |
| 0x13 | `S_VEHICLE_STATE` | 0 | same, relayed |
| 0x20 | `C_SHOT` / 0x21 `S_SHOT` | 1 | weapon `u8`, origin `float[3]`, direction `float[3]`, speed `float` (§1.9.1) |
| 0x22 | `C_DAMAGE` / 0x23 `S_DAMAGE` | 1 | victim `netId`, weapon `u8`, amount `float`, piece `u8`, direction `u8` (§1.10.1). `S_DAMAGE` goes to the victim alone, not broadcast |
| 0x24 | `S_DEATH` | 1 | `playerId`, killer `netId`, anim `u16` |
| 0x25 | `C_RESPAWN` / 0x26 `S_RESPAWN` | 1 | spawn transform (§1.10.5) |
| 0x27 | `C_EXPLOSION` / 0x28 `S_EXPLOSION` | 1 | type `u8`, pos `float[3]` (§1.9.3) |
| 0x29 | `C_DEATH` | 1 | killer `netId`, anim `u16` (§1.10.4). Out of order because the block above was numbered before it was clear who announces a death |
| 0x30 | `C_ENTER_VEHICLE` / 0x31 `S_ENTER_VEHICLE` | 1 | `netId`, seat `u8`, jack `bool`, plus the car's identity when `netId` is `INVALID_NETID`: model id, colours, extras `i8[2]` (§1.12) and transform. A client that has got into one of the session's own cars claims it by that car's existing `netId` instead (§2.8.4). Every seat is recorded, not just the driver's (§2.8.2) |
| 0x32 | `C_EXIT_VEHICLE` / 0x33 `S_EXIT_VEHICLE` | 1 | `netId` |
| 0x34 | `S_VEHICLE_SPAWN` | 1 | `netId`, model id, transform, colours, extras `i8[2]` (§1.12), then condition: health `float`, flags `u8` including `VEH_WRECKED` (§2.8.1) |
| 0x35 | `S_VEHICLE_DESPAWN` | 1 | `netId`. Declared and never sent: nothing in the server removes a car from the session, and a wreck is a wreck rather than a removal (§1.11.4) |
| 0x36 | `C_VEHICLE_BLOWUP` / 0x37 `S_VEHICLE_BLOWUP` | 1 | `netId`, pos `float[3]`, quat `float[4]`; `S_` also carries `playerId` (§1.11) |
| 0x38 | `C_UNOWNED_BLOWUP` / 0x39 `S_UNOWNED_BLOWUP` | 1 | a car nobody is driving: key (`kind` `u8`, `id` `u16`) plus pos `float[3]` and quat `float[4]`; `S_` also carries `reporterPlayerId`. Three kinds - a car generator index, an ambient car's `netId`, a session `netId`. Only the ambient kind reads the transform; a parked car sits where the map put it on every machine and the sender zeroes those bytes |
| 0x3A | `C_VEHICLE_DAMAGE` / 0x3B `S_VEHICLE_DAMAGE` | 1 | what shape a car is in: `netId` `u16`, `panels` `u32` (`CDamageManager::m_panelStatus`, seven nibbles), `doors` `u16` (six doors, two bits each); `S_` also carries `playerId`. Change-only and absolute, merged as a componentwise maximum on both ends. Accepted from the car's recorded driver and nobody else, the same gate `C_VEHICLE_STATE` uses. [docs/cardamage.md](cardamage.md) |
| 0x3C-0x3F | - | - | Reserved for the rest of the vehicle block |
| 0x40 | `S_WORLD_STATE` | 1 | game hour/minute, both weather types, `hostPlayerId` (§2.7) |
| 0x41 | `C_WORLD_STATE` | 1 | the host's own hour/minute and weather pair; dropped from anyone else |
| 0x50 | `C_CHAT` / 0x51 `S_CHAT` | 1 | `playerId`, text |
| 0x68 | `C_PED_DAMAGE` / 0x69 `S_PED_DAMAGE` | 1 | a hit one machine's player landed on a pedestrian another machine hosts: `netId` `u16`, weapon `u8`, amount `float`, piece `u8`, direction `u8` - `CPed::InflictDamage`'s own five arguments less the culprit pointer; `S_` also carries `attackerId` and goes to **the ped's owner alone** (§1.19). The only ambient packet that travels *towards* an owner, so the ownership test is inverted: refused *to* the owner, accepted from anybody else. Friendly fire has no say - a pedestrian is not a player |
| 0x6A | `C_VEHICLE_HIT` / 0x6B `S_VEHICLE_HIT` | 1 | a hit one machine's player landed on a car another player is **driving**: `netId` `u16`, weapon `u8`, amount `float` — `CVehicle::InflictDamage`'s own three arguments less the culprit pointer; `S_` also carries `attackerId` and goes to **the driver alone** (§1.21). The same inverted ownership test as `C_PED_DAMAGE`, and the exact inverse of `Session::MayReportVehicle`: refused *to* the driver, accepted from anybody else, refused outright for a car with no driver. No health, no position, no shot vector. Friendly fire has no say — a car is not a player |
| 0x6C | `C_CAR_HIT` / 0x6D `S_CAR_HIT` | 1 | the same three fields as `C_VEHICLE_HIT`, for a replica of somebody else's **traffic**; `S_` also carries `attackerId` and goes to **the car's host alone** (§1.23). Refused *to* the host, accepted from anybody else, refused for a wreck or a netId that isn't traffic any more |
| 0x6E-0x6F | - | - | Reserved for the rest of that direction |
| 0x80 | `C_PICKUP_CLAIM` | 1 | `PickupIdent`: pos `float[3]`, model index `i16`, type `u8`, flags `u8` (§1.13). Sent on approach, at 4 m |
| 0x81 | `S_PICKUP_TAKEN` | 1 | `playerId` + `PickupIdent`. To everyone **except** the collector, whose own engine already removed their copy (§1.13.3) |
| 0x82 | `S_PICKUP_DENIED` | 1 | `PickupIdent`. To the loser alone; nothing to undo, because the engine never saw an object there |
| 0x83 | `C_PICKUP_RELEASE` | 1 | `PickupIdent`. A grant that could not be consumed, a reservation the player walked away from, or a key the script has re-created (§1.13.5) |
| 0x84 | `S_PICKUP_GRANT` | 1 | `PickupIdent`. To the claimant alone. A *reservation* - nothing is removed anywhere until a collection is reported (§1.13.3) |
| 0x85 | `C_PICKUP_COLLECTED` | 1 | `PickupIdent`. The holder's engine actually took it. Detected, not decided |
| 0x86 | `C_PICKUP_DROP` / 0x87 `S_PICKUP_DROP` | 1 | `PickupIdent` + quantity; `S_` also carries the owner's `playerId` (§1.13) |
| 0x88-0x8D | `C_RAMPAGE_START` .. `S_RAMPAGE_END` | 1 | the shared rampage: start, the session's frenzy, one kill, an ending ([rampage.md](rampage.md) §5) |
| 0x8E | `C_RAMPAGE_CAR` / 0x8F `S_RAMPAGE_CAR` | 1 | one car wreck that counted toward a vehicle rampage, from the machine that decided it: `frenzyId` `u16`, model `u16`, key (`kind` `u8`, pad, `id` `u16`) - `UNOWNED_PARKED` or `UNOWNED_SESSION` for a car several machines can decide, `0xFF` otherwise. `S_` also carries `byPlayer` and goes to everybody but the reporter. A keyed car counts once per frenzy on the server and on every client ([rampage.md](rampage.md) §9) |
| 0xA0 | `C_GARAGE_STATE` | 1 | `deviating` `u32`, one bit per garage: "my own state machine has this garage away from where this type of garage rests" (§1.16.3). Sent on change only |
| 0xA1 | `S_GARAGE_STATE` | 1 | `playerId` + the mask. To everyone except the sender, and replayed in the backfill for every player whose mask is non-zero |
| 0xA2 | `C_RESPRAY` / 0xA3 `S_RESPRAY` | 1 | `vehicleNetId` `u16`, `garage` `u8`, `colour1` `u8`, `colour2` `u8`; `S_` also carries `playerId`. The colours are read off the car the owner's engine painted, because `ChooseVehicleColour` is a per-machine round robin (§1.16.5). The wanted level is deliberately not here (§1.16.6) |
| 0xA4-0xAF | - | - | Reserved for the rest of the garage block |
| 0xB0 | `C_PLAYER_AMMO` / 0xB1 `S_PLAYER_AMMO` | 1 | one weapon slot the sender is **not** holding: weapon `u8`, flags `u8` (`AMMO_SLOT_OWNED`), clip `u16`, total `u32`. `S_` also carries `playerId`. On change only, and dropped by the server unless the session has `SESSION_AMMO_SYNC` (§1.9.6). The held weapon's ammo rides the snapshot instead |
| 0xC0 | `C_OBJECT_BROKEN` | 1 | `ObjectBreakBody`: `ObjectIdent` (pos `float[3]` = `m_objectMatrix`'s position, model index `i16`, 2 pad), `amount` `float`, `state` `u8`, 3 pad (§1.17) |
| 0xC1 | `S_OBJECT_BROKEN` | 1 | `playerId` + `ObjectBreakBody`. To everyone but the reporter |
| 0xC2-0xCF | - | - | Reserved for breakable objects - where a knocked-over lamp post came to rest is the named open half (`objects.md` §8) |
| 0xD8 | `C_PED_DEATH` / 0xD9 `S_PED_DEATH` | 1 | an ambient pedestrian his host's engine killed: `netId` `u16`, anim `u16`. Host-only, like the despawn and the limb, and the one of the three the server **keeps** - a joiner is handed the corpse (`population.md` §5) |
| 0xDA-0xDF | - | - | Reserved beside it, for whatever else only a ped's host can witness |
| 0xF0 | `C_CHEAT` / 0xF1 `S_CHEAT` | 1 | a cheat that changes something its typist's machine does not own: `cheat` `u8` (`CheatId`), `state` `u8` - what it left the typist's engine at, never "toggle". `S_` also carries the typist's `playerId`, or `INVALID_PLAYER` for a joiner's replay. A sky goes to **the host alone**; the clock-speed, riot and armed-crowd cheats to everybody but the typist; nothing else is ever sent. Dropped by the server unless the session's `CheatRule` allows it (§1.24, [cheats.md](cheats.md)) |
| 0xF2-0xF7 | - | - | Reserved for the rest of the cheat block |

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

1. ~~**Ped AI suppression.**~~ **Answered 2026-09-22 — see §1.13.** Neither of
   the two options this question named. `PED_STATES_NO_AI` is a bound used by
   `IsPedInControl`, not a gate on `ProcessControl`: state 34's own entry in
   the retail state switch is the default arm, twenty other states share it,
   and taking it skips the per-state function and nothing else — while parking
   *past* 34 stops a remote player being able to catch fire. And skipping
   `ProcessControl` costs the clump alpha fade (so the ped is never drawn),
   `CPhysical::ProcessControl`, the death animation, aiming and footsteps.
   The AI is two fields, `m_nPedState` and `m_objective`, and CoopIII has been
   holding both since the seating work. Nothing was built; §1.13.6 says what
   ambient NPC sync can rely on.
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
