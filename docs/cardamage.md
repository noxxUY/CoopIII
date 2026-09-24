# A car's damage model

`roadmap.md` §4 has carried this line since the sync inventory was written:

| Damage model | panels, doors, lights, wheels | ❌ not designed. `CDamageManager` is the first `CAutomobile` member, at `+0x288` |

Health travels and damage does not, so the same car is dented, missing a door
and sitting on a flat tyre on one screen and showroom-new on the other. This is
the design for closing that, and most of it is about deciding how little has to
travel rather than how much.

Everything below was read out of `gta3.exe` (v1.0 retail, MD5
`85414BF9EB414D00AD81062360F0DB1F`). `re3` was used to know what to look for
and for nothing else — five of its constants have already been refuted against
this binary, so every offset, every threshold and every function here is quoted
from the disassembly with the instruction that proves it. This time all of
re3's claims about `CDamageManager` held; that is a result, not an assumption,
and §1.3 says how it was checked.

---

## 1. What `CDamageManager` actually is

### 1.1 Where it lives, proved twice

`CVehicle::InflictDamage` (`0x00551950`, already verified in `addresses.h`) is
the only place in the engine where a `CVehicle` method reaches into
`CAutomobile`'s own storage, and it does it in three instructions:

```
00551BD4  cmp  dword ptr [esi+284h],0          IsCar()   — m_vehicleType, CAR == 0
00551BE1  lea  ecx,[esi+288h]                  &this->Damage
00551BE7  push 0E1h                            225 = ENGINE_STATUS_ON_FIRE
00551BEC  call 00545940                        CDamageManager::SetEngineStatus
```

`CAutomobile::BlowUpCar` (`0x0053BC60`) says the same thing from the other end:

```
0053BCB1  lea  ecx,[ebx+288h]
0053BCB7  call 00545B70                        CDamageManager::FuckCarCompletely
```

`+0x288` is `SIZEOF_VEHICLE`, so `Damage` is `CAutomobile`'s first member, which
is what `addresses.h` has said since Area E. What was never established is what
is inside it.

### 1.2 The layout, from the accessors rather than from a header

Every field below is pinned by an instruction that touches it. The accessors are
four to twelve instructions each and there is no ambiguity in any of them.

```
005458B0  mov  eax,[esp+4]        SetPanelStatus(panel, status)
005458B6  lea  eax,[eax*4]          shift = panel*4
005458BD  movzx ecx,al
005458C0  mov  eax,0Fh              a four-bit field
005458CA  and  [edx+14h],eax        m_panelStatus at +0x14
005458D3  or   [edx+14h],eax

00545860  add  eax,eax            SetLightStatus(light, status)
0054586B  mov  eax,3                a two-bit field
00545875  and  [edx+10h],eax        m_lightStatus at +0x10

00545900  mov  al,[esp+8]         SetWheelStatus(wheel, status)
00545908  mov  [ecx+edx+5],al       m_wheelStatus at +0x05, one byte each

00545920  mov  al,[esp+8]         SetDoorStatus(door, status)
00545928  mov  [ecx+edx+9],al       m_doorStatus at +0x09, one byte each

00545940  cmp  eax,0FAh           SetEngineStatus(status)
0054594B  mov  byte ptr [ecx+4],0FAh   clamped to 250
00545951  mov  byte ptr [ecx+4],al     m_engineStatus at +0x04, one byte
```

So:

| Offset | Field | Width |
|---|---|---|
| `+0x00` | `m_fWheelDamageEffect` | `float` |
| `+0x04` | `m_engineStatus` | `uint8`, 0..250 |
| `+0x05` | `m_wheelStatus[4]` | `uint8` each, 0..2 |
| `+0x09` | `m_doorStatus[6]` | `uint8` each, 0..3 |
| `+0x0D` | (three bytes of padding) | |
| `+0x10` | `m_lightStatus` | `uint32`, 2 bits per light |
| `+0x14` | `m_panelStatus` | `uint32`, 4 bits per panel |
| `+0x18` | `field_18` | `uint8` |
| | **`sizeof`** | **`0x1C`** |

The size is proved twice and neither proof is a declaration.
`CDamageManager::ResetDamageStatus` (`0x00545850`) is `xor eax,eax` followed by
**seven** `stosd` — 28 bytes, `0x1C`. And the inlined constructor inside
`CAutomobile::CAutomobile` (`0x0052C6B0`) names the next member out loud:

```
0052C6E6  call  00545850                       ResetDamageStatus()
0052C6EF  mov   dword ptr [edx+288h],3F400000h m_fWheelDamageEffect = 0.75f
0052C6FD  mov   byte  ptr [eax+2A0h],1         field_18 = 1        (0x288 + 0x18)
0052C708  add   eax,2A4h                       the member AFTER it (0x288 + 0x1C)
0052C70D  push  6 / push 24h / push 0          6 elements, stride 0x24
0052C713  push  0052D150                       its constructor
0052C719  call  0059CD10                       vector ctor iterator
```

`0x2A4` is `0x288 + 0x1C`, and six objects of stride `0x24` starting there are
`CDoor Doors[6]`. There is no room for anything else.

### 1.3 Everything that writes it, exhaustively

Scanning `.text` for `E8`/`E9` rel32s to the five setters and to
`FuckCarCompletely` gives 46 sites, and they fall into five groups and no
others:

| Writer | Address | What it writes |
|---|---|---|
| `CDamageManager::ApplyDamage` | `0x00545A80` | the whole class, through the five `Progress*` helpers |
| `CAutomobile::VehicleDamage` | `0x0052F390` | calls `ApplyDamage` thirteen times and nothing else does |
| `CDamageManager::FuckCarCompletely` | `0x00545B70` | wheel 0, all six doors, then zeroes lights and panels and sets the engine to 250 |
| `CAutomobile::BurstTyre` | `0x0053C0E0` | one wheel — see §2.4, it is never reached |
| `CVehicle::InflictDamage` `CAutomobile::VehicleDamage`'s tail `CVehicle::ExtinguishCarFire` `CGarages` | `0x00551BEC` `0x0052FFB9` `0x00552B4A` `0x00479EC9`, `0x00490ECD`, `0x004A084E` | `m_engineStatus` alone |
| `CPed` (eight sites), `CAutomobile` (one) | `0x004DE724` … `0x004E4BF4`, `0x0052E87C` | `m_doorStatus`, to `SWINGING` and back to `OK` — a door someone opened, not damage. §3.2 |
| `CPools::LoadVehiclePool`, `CReplay::ProcessCarUpdate` | `0x004A1D27`, `0x00595040` onwards | restoring a car nobody simulated. §5 is built on these two |

The row that matters is the second one. **`CAutomobile::VehicleDamage` has one
caller in the whole image** — `0x00531FE3`, inside `CAutomobile::ProcessControl`
— and it is the only path to a dent, a smashed panel or a door on the pavement.
`ApplyDamage`'s thirteen call sites are all inside it, one per body part:

```
0052F7C1  COMPONENT_BUMPER_FRONT        0052F847  COMPONENT_DOOR_BONNET
0052F8AA  COMPONENT_BUMPER_REAR         0052F925  COMPONENT_DOOR_BOOT
0052F99B  COMPONENT_DOOR_FRONT_LEFT     0052FA11  COMPONENT_DOOR_FRONT_RIGHT
0052FA87  COMPONENT_DOOR_REAR_LEFT      0052FAFD  COMPONENT_DOOR_REAR_RIGHT
0052FB5C  COMPONENT_PANEL_FRONT_LEFT    0052FBBF  COMPONENT_PANEL_FRONT_RIGHT
0052FC22  COMPONENT_PANEL_REAR_LEFT     0052FC85  COMPONENT_PANEL_REAR_RIGHT
0052FCC3  COMPONENT_PANEL_WINDSCREEN
```

Thirteen components, no wheel among them. That single fact decides most of §2.

### 1.4 The arithmetic a dent comes out of

`VehicleDamage` reads no arguments of its own in the path that matters. Its one
caller passes `(0.0f, 0)`, and the first thing it does is fetch what the
collision solver left behind:

```
0052F390  fld   dword ptr [6004FCh]            0.2f, the multiplier for an explicit call
0052F3BD  jne   0052F3DA                       impulse != 0 ?
0052F3BF  fld   dword ptr [ebp+10Ch]           m_fDamageImpulse
0052F3CF  mov   di,word ptr [ebp+120h]         m_nDamagePieceType
0052F3C9  fld   dword ptr [6004F4h]            1.0f
```

Then four gates, in this order:

```
0052F3F8  [ebp+1F7h] bit 6                     bCanBeDamaged
0052F47C  fcomp [6005B0h]                      impulse > 25.0f
0052F653  [ebp+53h]  bit 4                     bOnlyDamagedByPlayer
0052F685  [ebp+53h]  bit 2                     bCollisionProof   -> return
0052F6A0  DotProduct(m_vecDamageNormal, up) > 0.6f against a building
```

and then the switch on `m_nDamagePieceType` that picks one of the thirteen
components above. `ApplyDamage` itself scales the impulse by a per-group
constant and compares against a fixed threshold. Both the table and the
thresholds were read out of the image rather than taken from re3:

```
006012B0  G_aComponentDamage[7] = 2.5, 1.25, 3.2, 1.4, 2.5, 2.8, 0.5
          (bumper, wheel, door, bonnet, boot, panel, default)
006012CC  150.0f    the damage threshold — below it ApplyDamage returns false
006012D0  220.0f    above it the bonnet and front-panel arms also age the engine
```

and the jump table at `0x006012D4` has exactly six entries, one per component
group, in re3's order. So the whole decision is

```
    ApplyDamage fires  <=>  m_fDamageImpulse * impulseMult * G_aComponentDamage[group] > 150
```

with `impulseMult` 4.0, or 0.5 for a handling flagged `bMoreResistantToDamage`.
Deterministic. Nothing in `VehicleDamage` calls `CGeneral::GetRandomNumber` —
checked by scanning its whole body, `0x0052F390`–`0x005300B5`, for a rel32 to
`0x005A41D0`, and there is not one. The only random step anywhere in
`CDamageManager` is `ProgressEngineDamage` (`0x005459B0`,
`status + 32 + (rand & 0x1F)`), and §2.3 shows that one does not matter.

---

## 2. What converges on its own

This is the question that has decided the shape of every similar piece of work
in this project, and twice the honest answer struck planned work rather than
justifying it — the ped AI in `protocol.md` §1.13 and the unowned car's health
in `game/vehicle.h`. It does it again here: three of the four things the sync
inventory lists come off the wire before anything is built.

### 2.1 Gunfire does not dent a car. At all.

A bullet that hits a vehicle reaches exactly one function:

```cpp
    ((CVehicle *)victim)->InflictDamage(shooter, m_eWeaponType, info->m_nDamage);
```

and `CVehicle::InflictDamage` subtracts a float from `m_fHealth`, sets
`m_nLastWeaponDamage`, and — if the health crosses 250 on the way down — calls
`SetEngineStatus(ENGINE_STATUS_ON_FIRE)`. It never touches a panel, a door, a
light or a wheel. There is no path from a weapon to `ApplyDamage`, because
`ApplyDamage`'s only caller is `VehicleDamage` and `VehicleDamage`'s only caller
is `ProcessControl` acting on a recorded *collision*.

So the per-machine shotgun spread, the per-machine bullet ray, the whole family
of divergence that M3 had to reason about for peds, produces **no visible
difference on a car whatsoever**. It produces a health difference, and health
has been on the wire since M2.

### 2.2 An explosion does not dent a car either

`CWorld::TriggerExplosion` damages everything in its radius through
`CVehicle::InflictDamage` with a multiplier that is a function of distance, and
that is the same function as above: health only. If the health reaches zero the
car goes through `BlowUpCar`, and `BlowUpCar`'s damage is
`FuckCarCompletely` — wheel 0 missing, all six doors missing, lights and panels
zeroed, engine 250, with no input of any kind:

```
00545B73  mov byte ptr [ebx+5],2               m_wheelStatus[0] = MISSING
00545B7A  mov byte ptr [ebx+9],3               all six doors = MISSING
  ... 0Ah 0Bh 0Ch 0Dh 0Eh ...
00545BAD  mov dword ptr [ebx+10h],0            m_lightStatus = 0
00545BB9  mov dword ptr [ebx+14h],0            m_panelStatus = 0
00545BC0  call 00545940 (250)                  m_engineStatus = 250
```

A blown-up car's damage model is therefore **identical on every machine by
construction**, and `protocol.md` §1.11 already makes sure every machine runs
`BlowUpCar` on it. Nothing about a wreck needs to travel that does not travel
today.

(The three `ProgressPanelDamage(16)` / `ProgressPanelDamage(17)` calls in the
middle of that function are the retail build's own bug — 16 and 17 are
`tComponent` values where a `ePanels` index belongs, and `SetPanelStatus`'s
`shl` masks the shift to five bits, so they land on panels 0 and 1. It is
deterministic nonsense and it is immediately overwritten by the two `mov
dword ptr …,0` above, so it changes nothing. Worth knowing only so nobody
"fixes" it into a divergence.)

### 2.3 The engine status is re-derived from health on every machine, every frame

`m_engineStatus` looks like it must travel: it decides the smoke, the steam and
whether the car is considered on fire. It does not, because the tail of
`VehicleDamage` — which `ProcessControl` runs on every car on every machine on
every frame — recomputes it from health:

```
0052FF7E  fld   dword ptr [ebp+200h]           m_fHealth
0052FF86  fcomp dword ptr [6005C0h]            250.0f
0052FF9E  call  00545960                       GetEngineStatus()
0052FFB9  call  00545940                       SetEngineStatus(225) if it was lower
```

Health is already on the wire at 25 Hz for the car's driver, so every observer
reaches 225 within a frame or two of the owner. Below 225 the value only picks
which particle system `AddDamagedVehicleParticles` runs (`< 100` returns
immediately), so the random `ProgressEngineDamage` steps change the density of
the smoke and nothing else. That is a difference nobody can see and nobody can
report, and putting a byte on the wire for it would be paying for smoke.

### 2.4 A tyre never bursts in retail GTA III v1.0

This is the one that surprised me, and it is the one I would most expect a
reviewer to disbelieve, so here is the whole chain.

`m_wheelStatus` has exactly two writers: `SetWheelStatus` (`0x00545900`), called
from `ProgressWheelDamage` (`0x00545A40`) and from `CAutomobile::BurstTyre`
(`0x0053C0E0`), plus `FuckCarCompletely`'s direct byte write.

- `ProgressWheelDamage` has **one** caller, `ApplyDamage`'s `COMPGROUP_WHEEL`
  arm at `0x00545AE0`. Nothing ever passes it a wheel: the thirteen
  `ApplyDamage` call sites in §1.3 are four panels, a windscreen, two bumpers
  and six doors, and `VehicleDamage`'s four `CAR_PIECE_WHEEL_*` cases are a bare
  `break`. So that arm is unreachable.
- `BurstTyre` has **no** `call` or `jmp` to it anywhere in `.text`, and its
  address appears exactly once in the whole file, at `0x00600C98` — which is
  `CAutomobile`'s vtable (`0x00600C1C`) slot 31. Scanning `.text` for every
  `call dword ptr [reg+7Ch]` returns three sites, `0x00582350`, `0x005993C2` and
  `0x0059A082`, and disassembling all three shows them to be COM interface calls
  in the movie and networking code, not vehicle dispatches.

So a wheel's status is only ever non-zero because `FuckCarCompletely` set wheel
0 to `MISSING`, and that happens inside a `BlowUpCar` that every machine already
runs. **Wheels are converged, and they are struck from the wire.** The
inventory row says "panels, doors, lights, wheels"; one of the four is a ghost.

If somebody later ports the Vice City behaviour, or a mod calls slot 31, this
paragraph is wrong and the wire needs a wheel nibble. Until then it is four bits
nobody would ever set.

### 2.5 Lights are a function of panels, and they are the one thing a number is enough for

`SetLightStatus` (`0x00545860`) has one caller in the image: `ApplyDamage`'s
panel arm.

```
00545B1B  movzx eax,byte ptr [esp+0Fh]         subComp
00545B22  push 1                               always 1, never any other value
00545B25  call 00545860                        SetLightStatus(subComp, 1)
00545B2A  ...                                  falls through into the bumper arm
00545B56  call 00545A00                        ProgressPanelDamage(subComp)
```

The light and the panel are set in the same breath, from the same `subComp`, on
every damage step. `ProgressPanelDamage` increments from 0 on the first hit and
`SetLightStatus` only ever writes 1, so for panels 0..4:

```
    GetLightStatus(i) == 1   <=>   GetPanelStatus(i) != 0
```

both ways, for the whole life of the car, including after `FuckCarCompletely`
zeroes both. A receiver that has the panel word has the light word, and it does
not need two bits on the wire for something it can compute.

And unlike everything else here, writing the number really is enough. The 46
reads of `GetLightStatus` in the image all sit in one contiguous block,
`0x0053848B`–`0x00539D56`, inside a large function that nothing calls directly
and that is reached only through the vehicle's vtable — the render-preparation
pass. Every read is fresh, every frame; no atomic is swapped and no flag is
cached. A broken headlight is the only part of a car's damage that is a number
rather than an event.

### 2.6 What is left is collisions, and collisions are exactly what nobody simulates twice

Strike gunfire, strike explosions, strike the wreck, strike wheels, strike
lights, strike the engine byte. What remains is the thirteen components of
§1.3, driven by `m_fDamageImpulse` and `m_nDamagePieceType`, which the local
collision solver writes and nothing else does.

Every machine runs `ProcessControl` on every car it holds, including the cars it
is only observing, so every machine computes its own dents from its own
collisions. Take the two cases in turn.

**Two cars both driven by players.** On A's machine, A's car is real and B's is
a replica whose transform is written back from an interpolated snapshot after
every frame of physics (`protocol.md` §1.11.5). At the moment of contact the
replica is where B was `interp` milliseconds ago — 100 ms by §2.6 of the
protocol — so at a 50 km/h closing speed the two cars are **1.39 m** out of
position relative to where B's own machine has them. A GTA III saloon is about
4.5 m long and 1.8 m wide, so a third of a car length of error sits right on the
boundary between `CAR_PIECE_BUMP_FRONT` and `CAR_PIECE_WING_LF`. Both machines
produce a dent; they do not reliably produce the *same* dent, and the impulse
either side of the `> 150` threshold is a coin toss rather than a small error.

**A car and something only one machine simulated.** A parked car, a traffic
replica, a session car somebody walked away from. Here the collision is not
merely inaccurately shared, it is not shared at all: one machine's real car hits
it, every other machine's *replica* hits it with a fabricated impulse derived
from a transform that is being overwritten sixty times a second. This is the
same class as §5.8's "a shove from a replica is not a collision anybody
simulated twice".

So: **a car's damage diverges through exactly one hole, and it is collisions.**
The wire has to carry panels and doors, and nothing else.

---

## 3. What goes on the wire

### 3.1 Panels: a four-bit ladder that only ever climbs

`m_panelStatus` is seven used nibbles — four wings, the windscreen and two
bumpers — and `ProgressPanelDamage` (`0x00545A00`) is the only thing that moves
one:

```
00545A0C  call 005458E0                        GetPanelStatus(panel)
00545A11  cmp  al,3
00545A13  jne  00545A20                        already MISSING? refuse
00545A20  ... SetPanelStatus(panel, status+1)
```

It increments and it refuses at 3. Nothing anywhere decrements a panel except
`ResetDamageStatus` and `CAutomobile::Fix` (§6.2). Panels are a monotone ladder
and go on the wire as they sit in memory: one `uint32`.

### 3.2 Doors: the byte is not the damage, and this is a trap

`m_doorStatus` looks like the same shape and is not, because two of its four
values are not damage at all. The eight `CPed` writers and the one `CAutomobile`
writer from §1.3 do this:

```
004DE71E  push 2 / push door / call 00545920   SetDoorStatus(door, SWINGING)

004E4BE4  call 00545930                        GetDoorStatus(2)
004E4BE9  cmp  eax,2                           is it SWINGING?
004E4BF4  push 0 / push 2 / call 00545920      SetDoorStatus(2, OK)
```

That is a ped opening a door and a ped closing it. `DOOR_STATUS_SWINGING` means
"somebody has this door open"; `DOOR_STATUS_OK` means they shut it. Neither is
damage, and the write to `SWINGING` is unconditional — a door that is `MISSING`
and gets "opened" has its byte set back to 2.

The byte can therefore go *down*, and when it does, **the model does not follow
it**. `CAutomobile::SetDoorDamage` (`0x00530200`) hides a missing door by
calling `SetComponentVisibility(frame, ATOMIC_FLAG_NONE)`, and nothing puts the
atomics back except `Fix`. So after a ped touches a missing door the status byte
says `SWINGING` and the car still has a hole where the door was.

`m_doorStatus` is not a description of the car. It is the last thing that
happened to the door, and only two of its values are the accumulated damage
the car is actually wearing. So what goes on the wire is not the byte. It is a
**damage level**, two bits, mapped once at the sampling end:

| raw `m_doorStatus` | meaning | level on the wire |
|---|---|---|
| 0 `DOOR_STATUS_OK` | shut | 0 |
| 2 `DOOR_STATUS_SWINGING` | open | 0 |
| 1 `DOOR_STATUS_SMASHED` | dented, damaged atomic shown | 1 |
| 3 `DOOR_STATUS_MISSING` | gone | 2 |

and the sender keeps the running maximum of that level per door rather than
sampling the byte, because the byte is transient and the damage is not. Six
doors × 2 bits = 12 bits.

Sending the raw byte instead would flicker every time a passenger got in, and
would occasionally tell an observer to un-lose a door — which it could not act
on anyway, since `SetDoorDamage` has no arm that restores anything.

A door's *angle* is deliberately not carried. `CReplay` carries two of them
(`int8 door_angles[2]`, front left and front right only) and even that is a
replay concession; the ordinary enter/exit path animates them locally, and
`protocol.md` §2.8.2 already covers who is sitting where.

### 3.3 The body

```c
// Everything about a car's condition that does not converge on its own.
struct VehicleDamageBody {
    uint16_t netId;
    uint32_t panels;   // CDamageManager::m_panelStatus, seven nibbles used
    uint16_t doors;    // six doors, two bits each, damage level per §3.2
};   // 8 bytes
```

Eight bytes, and the receiver derives `m_lightStatus` from `panels` (§2.5),
leaves `m_wheelStatus` alone (§2.4), and leaves `m_engineStatus` to its own
engine (§2.3).

This is very close to what GTA III itself decided it needed. `CReplay`'s
`tVehicleUpdatePacket` is 48 bytes of everything a car needs to be reproduced on
a machine that did not simulate it, and the damage model's share of it is
`uint32 panels` and `uint8 door_status` — the panel word verbatim, and a
one-bit-per-door "this one is gone". CoopIII carries two bits per door instead
of one because a session lasts longer than a replay and a merely smashed door
is a thing you look at for an hour.

### 3.4 Why it is a reliable change-only packet and not a snapshot field

Adding the six bytes to `VehicleStateBody` is the cheap-looking option and it is
the wrong one, for four reasons in increasing order of how much they matter.

**It is 8% of the whole vehicle budget for a constant.** §2.1's bandwidth check
is 7 remotes × 25 Hz × ~72 B ≈ 12.6 KB/s. Six more bytes in the 72 is
`+1.05 KB/s` at eight players, paid continuously, for a value that is unchanged
for minutes at a time. The reliable packet costs 13 bytes per dent, and a dent
is an event that happens a handful of times in a session.

**Applying it is not free the way reading a float is.** A changed door means
`SetDoorDamage` → `SetComponentVisibility` → `HideAllComps` plus
`RwFrameForAllObjects` over the component's atomics. That belongs on a change,
not on a stream, and putting it behind a change test inside the 25 Hz path just
means writing the change test anyway.

**A snapshot cannot carry it for the cars that need it most.** `C_VehicleState`
is sent by one machine and one only — the driver's — and `ApplyRemoteVehicle`
returns early for a wreck. So a damage field in the snapshot is structurally
dead for a parked car, for a traffic replica and for a session car somebody
walked away from, which is precisely `roadmap.md` §5.8's finding restated. The
same sentence appears there: *"Not a new field on the snapshot — a parked car
has no snapshot to put it on, which is the whole problem."*

**The backfill wants an event, not a sample.** §2.8 says a late joiner has to
end up in the same world. A joiner is handed `S_VehicleSpawn` and then whatever
the session has recorded; the session already replays `S_UnownedBlowUp` rows
into a joiner's backfill, and a damage row is the same shape and the same
lifetime.

So: `C_VEHICLE_DAMAGE` (**0x3A**) and `S_VEHICLE_DAMAGE` (**0x3B**),
`CH_EVENT`, reliable, sent when the sampled damage word changes and never
otherwise. Opcodes 0x3C–0x3F stay free for the rest of this block.

```
VehicleDamageBody   netId u16, panels u32, doors u16          8 bytes
C_VehicleDamage     hdr + body                                13
S_VehicleDamage     hdr + playerId u8 + pad[3] + body         17
```

`PROTOCOL_VERSION` moves for the two new opcodes. The merged build is **18**,
one number for the ten branches that landed together; see the version history
in `sdk/include/coopiii/protocol.h`.

---

## 4. Who may report a dent

`roadmap.md` §5.8 settled who may report a *wreck*, for four kinds of car, and
the answer for a dent is the same answer with one property of the data doing the
extra work. Nothing new is invented.

| Kind | Who may report a wreck (§5.8, built) | Who may report a dent |
|---|---|---|
| a car with a driver | the driver — `Session::MayReportVehicle` | the driver |
| `UNOWNED_AMBIENT`, a traffic car | the host that generated it, and nobody else | the host that generated it |
| `UNOWNED_SESSION`, a car somebody parked | anybody; `DestroyVehicle` is idempotent | anybody; the merge is idempotent |
| `UNOWNED_PARKED`, a car generator's car | anybody, first report wins | anybody; the merge is idempotent |

The two "anybody" rows are the ones that need the argument, because a wreck is a
boolean and a dent is not. For a wreck, `NoteUnownedBlowUp` can take the first
report and drop the rest, since every later report says the same thing. For a
dent, first-wins would freeze a car at whatever its first reported dent was.

What makes "anybody" safe anyway is that **the damage word is a monotone join,
not a delta.** §3.1 and §3.2 established that panels only climb and that the
door level is a high-water mark by construction. So the session's record is the
componentwise maximum of everything reported, the merge is commutative,
associative and idempotent, and the order packets arrive in cannot change the
answer. Two machines that each saw half of a shunt end up agreeing on the union
of the two halves — which is what would have happened if one machine had
simulated both.

That is why the packet carries absolute state and not "panel 2 got worse". A
delta over an unreliable ordering is a car that drifts; a monotone absolute
state over a reliable one is a car that converges.

The receiver applies `max` as well as the server, for the same reason and with
the same code: a report can never take damage off a car, here or anywhere.
`SetPanelDamage` and `SetDoorDamage` have no arm that restores anything, so a
lower value would be silently ignored by the engine in any case — clamping
makes that explicit instead of accidental.

**Phase one builds the first row only.** A car with a driver is the car every
player is looking at, its owner is already settled, it already has a session
row, and it is the one whose divergence is reported by a human rather than
inferred. The other three rows are the shape written down, and they need a
per-car damage word in three places that do not have one yet
(`Session::Vehicle` has it for free, `AmbientCar` needs one, and a parked car
needs a table keyed by car generator index, about 160 rows × 6 bytes). They are
named work in the sense §5.8 means it, not a gap nobody looked at.

**The traffic row is built since, 2026-09-23.** The host of a traffic car
samples its dents at the traffic stream's 10 Hz (`game/population.cpp`,
`DrainHostedCarDamage`, a cursor over the hosted table) and sends them on the
same `C_VehicleDamage`, change-only and absolute. netIds are one space, so the
server tells a traffic netId from a session one by looking:
`Session::NoteCarDamage` takes the report from the car's host and nobody else,
merges it into a word on `AmbientCar`, and refuses a repair marker, since a
traffic car is a session car by the time anybody drives it into a spray shop.
A joiner is handed the word after the car's spawn, and a promotion carries it
into the `Vehicle` row. A replica is collision-proof (§5.3), so before this it
stayed pristine on every screen but its host's; it now wears what its host
said, through the same appliers, with its parts flying if it is in front of
you and without if it is being built. No layout moved and nothing was
renumbered: an older server drops the report at `MayReportVehicle`, and an
older client ignores a damage packet for a netId it has no session car for.

The session-car row somebody walked away from was built before that, as the
custodian's report (`Client::SendCustodyVehicleDamage`). A car generator's car
is the one row left: nobody's engine simulates it, so each machine keeps its
own.

`game/vehicle.h` used to say of an unowned car that its "doors, panels and
dents" are deliberately not carried "because the cause already travels and the
divergence is a dent". Half of that was measured to be wrong, and the comment
was corrected when the traffic row landed: the cause travels for an
explosion and for gunfire, and it does not travel for a shunt, which is the only
thing that dents anything at all (§2.6). The remaining reason to leave parked
cars out is the bookkeeping, not the convergence.

---

## 5. Applying it: writing the bytes is not enough, and the engine says so twice

`protocol.md` §1.11.1's lesson was that writing zero into `m_fHealth` produces a
car with no health that is not destroyed, because destruction is a function.
The damage model is the same shape: the status bytes are a *record*, and the
thing you see is an `RpAtomic` that was hidden or swapped when the status
changed. Writing `m_panelStatus = 0x0030` changes the record and nothing on
screen — exactly like `m_aExtras` in `protocol.md` §1.12.

The engine has two places where it already solves this problem, and both are
worth copying rather than improvising around.

### 5.1 The per-component appliers

```
CAutomobile::SetPanelDamage (component, ePanels panel, bool noFlyingComponents)   0x005301A0
CAutomobile::SetBumperDamage(component, ePanels panel, bool noFlyingComponents)   0x00530120
CAutomobile::SetDoorDamage  (component, eDoors door,   bool noFlyingComponents)   0x00530200
CAutomobile::SetComponentVisibility(RwFrame*, uint32 flags)                       0x005300E0
CAutomobile::SpawnFlyingComponent(int32 component, uint32 group)                  0x00530300
CAutomobile::HideAllComps()                                                       0x005300C0
```

Each one re-reads the status out of `Damage` itself and acts on it — there is no
version that takes a value:

```
005301A0  mov  eax,[esp+8]                     panel
005301A8  lea  ecx,[ebx+288h]
005301B3  call 005458E0                        GetPanelStatus(panel)
005301B8  mov  edx,[ebx+ebp*4+37Ch]            m_aCarNodes[component]
005301BF  test edx,edx / jne                   no node -> do nothing at all
005301C8  cmp  al,1                            SMASHED1 -> show the damaged atomic
005301D3  cmp  al,3                            MISSING  -> spawn + hide both
```

So the sequence is always *write the status, then call the applier for that
component*, which is the ordering `CReplay::ProcessCarUpdate` uses six times in
a row from `0x00595086`:

```
0059508E  call 00545930                        GetDoorStatus(DOOR_BONNET)
00595095  cmp  eax,3 / je                      already gone, skip
005950A0  push 3 / push 0 / call 00545920      SetDoorStatus(BONNET, MISSING)
005950AF  push 1 / push 0 / push 11h
005950B1  call 00530200                        SetDoorDamage(CAR_BONNET, DOOR_BONNET, true)
```

That is the whole recipe, and it is the engine's own code for the exact problem
this document is about: *make a car on a machine that did not simulate the
damage look like the car on the machine that did*. `m_aCarNodes` is at `+0x37C`
and `CAR_BONNET` is 17, which the `0x11` above and `[ebx+3C0h]` in
`SetupDamageAfterLoad` agree on.

### 5.2 The all-at-once applier, and the reason not to use it

```
CAutomobile::SetupDamageAfterLoad()                                               0x0053C310
```

Twelve guarded calls — two bumpers, six doors, four wings — each one
`if(m_aCarNodes[c]) Set*Damage(c, panel)`. It is the one-call answer to "the
`CDamageManager` bytes are already right, make the model match", and it has
exactly one caller in the whole image: `0x004A1D27`, inside
`CPools::LoadVehiclePool` — the function `addresses.h` already cites as proof of
the `CVehicle` layout. A save game writes the raw damage bytes into a freshly
constructed `CAutomobile` and then calls this.

**It passes `noFlyingComponents = false`**, and that is the trap:

```
0053C31C  push 0                               noFlyingComponents
0053C31E  push 5                               VEHBUMPER_FRONT
0053C320  push 7                               CAR_BUMP_FRONT
0053C322  call 00530120                        SetBumperDamage
```

so every already-missing part is `SpawnFlyingComponent`ed — a real `CObject`,
allocated from the object pool, dropped at the car's feet. That is tolerable
once, at a load screen. It is not tolerable for a late joiner who is handed
eight damaged cars and gets a shower of doors and bumpers out of nowhere, and it
is not tolerable as pool pressure on a machine already holding every other
player's replicas.

So CoopIII uses the per-component route of §5.1, with the flag chosen by which
of two things is happening:

- **a change arriving while the car is in front of you** — apply with
  `noFlyingComponents = false`, so the bumper actually flies off. Each machine
  makes its own flying component, exactly as each machine makes its own broken
  lamp post; they are not session entities and they never were.
- **a spawn or a backfill** — apply with `noFlyingComponents = true`, so a car
  that has been missing its boot for ten minutes simply arrives without one.
  This is the case `CReplay::ProcessCarUpdate` passes `true` for, and for the
  same reason.

### 5.3 The observer has to stop denting the car itself

This is the half that is easy to forget and impossible to recover from, because
damage is monotone: a dent an observer invents is a dent that never goes away.

Today a remote car is deliberately still simulated locally — that is what turns
its wheels and works its suspension (`protocol.md` §1.11.5) — so
`ProcessControl` runs on it, so `VehicleDamage` runs on it, so the observer's
own collision solver is writing damage into a car it is only watching. Every
snapshot corrects the transform and nothing corrects the panels.

The switch is one bit, and it is the engine's own:

```
0052F685  mov al,byte ptr [ebp+53h]
0052F688  shr al,2
0052F68B  and al,1
0052F68D  je  0052F6A0
0052F68F  add esp,38h / pop … / ret 8          bCollisionProof -> return
```

`bCollisionProof` is byte C bit 2, which `addresses.h` already has as
`ENTITY_COLLISION_PROOF`, and the test above is unconditional: one comparison,
one return, before the first of the thirteen `ApplyDamage` calls. It is worth
saying why this is different from the ped case, because `protocol.md` §1.10.2 is
a whole section on the proof flags *not* being enough: `CPed::InflictDamage`
checks its flags inside a switch on the damage cause, and two arms of that
switch check nothing at all. `CAutomobile::VehicleDamage` has no switch in front
of this test. The flag really is the whole gate here.

Two consequences to write down rather than discover:

- Setting `bCollisionProof` also stops `CVehicle::InflictDamage` taking health
  for `WEAPONTYPE_RAMMEDBYCAR` (`0x005519FC`, the same bit). That is not a side
  effect to work around, it is the same rule: an observer must not decide health
  either.
- The flag has to follow the same test that decides whether to correct the
  transform — the driver's seat, not "is the local player inside"
  (`protocol.md` §1.11.5). A car the local player has got into must become
  dentable again in the same frame it stops being corrected, or you get a car
  you can drive into a wall forever without marking it.

One residual, stated because it is real and small rather than because it is
fixed. `VehicleDamage`'s upside-down health drain sits *above* the
`bCollisionProof` test:

```
0052F413  fld  dword ptr [esi+8]               GetUp().z
0052F416  fcomp dword ptr [6004F8h]            < 0.0f
0052F460  fld  dword ptr [6005A4h]             4.0f
          m_fHealth -= 4.0f * CTimer::GetTimeStep()
```

so an observer holding a remote car on its roof still takes health off its own
copy. For a driven car the wire overwrites it 25 times a second; for a wreck the
health is already zero. It is invisible today and it would stop being invisible
if a car with no driver ever started carrying health, so it belongs in the same
note as §4's phase two.

---

## 6. Two things that are not damage and will look like it

### 6.1 A remote player's door, which used to never open

When this was written a remote player was put into a seat with
`CPed::WarpPedIntoCar` rather than the enter animation, so none of the eight
`CPed` writers of `SetDoorStatus(door, SWINGING)` ever ran on an observer. The
door was shut, then the player was in the car.

That was closed by the enter/exit work, not here: the entry is now played
through `CPed::SetEnterCar` and `SetExitCar`, announced when it starts with the
door it goes in through (`protocol.md` §1.14.7), so the engine's own writers
swing the door on the observer as well, with the warp kept behind it as the
guarantee. The damage packet still carries no swing, and should not: it would
put a cosmetic, high-churn field on a packet designed to be sent a handful of
times a session.

### 6.2 A respray un-dents a car, and that has to travel too

`CAutomobile::Fix` (`0x0053C240`) opens with

```
0053C247  lea  ecx,[ebx+288h]
0053C24D  call 00545850                        Damage.ResetDamageStatus()
```

and then walks `m_aCarNodes[7..20]` putting every component's atomics back.
It is the only thing in the engine that lowers a damage status, and it is what a
Pay 'n' Spray and the script's `FIX_CAR` run.

It is an event, not a number — the same shape as `BlowUpCar`, and for the same
reason. Phase one left it out on the grounds that garages were wholly unsynced
(`roadmap.md` §4: "a garage that opens for one player is shut for everybody
else"), so a respray was already invisible to everybody else and skipping it
made nothing worse.

**That premise stopped being true, and this is the reconciliation.** The garage
work built the spray shop, so the two met at the version 18 merge: merged as
written, a car resprayed on one machine stayed dented on every other, because
the repair only ever cleared damage locally and the `max` merge has no way to
express a decrease.

Three separate things stayed broken, and only the first is the one people would
notice:

- the observer's `RemoteVehicle` row went on holding the dents, so a car that
  was despawned and rebuilt came back dented;
- the server's record went on holding them too, so a late joiner was handed
  them in the backfill;
- and the owner's own high-water mark — the `m_sentDamagePanels` /
  `m_sentDamageDoors` pair that decides whether a dent is worth reporting —
  still held the pre-respray word, so every later dent *below* it was read as
  "nothing new" and never left the machine again. That one is permanent and
  silent.

The fix reuses this packet rather than adding a second one. `VEH_DAMAGE_RESET`
is bit 31 of `panels`, which is free — seven panels take four bits each and
`CleanPanelWord` masks everything above bit 27 off — so the layout does not
move and no sender can produce the marker by accident. `Client::
SendLocalResprays` sends it immediately after the `C_Respray`, on the same
reliable ordered channel, and clears its own baseline; `Session::
NoteVehicleDamage` clears the record and relays the marker on (even when the
record was already clean, because it is also what tells every observer's row to
let go); `Client::OnVehicleDamage` empties the row and applies nothing.

Applying nothing is deliberate. `ApplyRemoteVehicleDamage` never lowers
anything — it writes a status and calls the engine's applier per component, and
there is no per-component undo — so the only thing that can clean the car on
screen is the `Fix` that `ApplyRemoteResprayImpl` already ran one packet
earlier. This half is the bookkeeping either side of it.

`TestARespraysClearTravelsAsADamageReport` in `sessiontest` and
`TestARespraysClearLetsTheRowGoOfTheDents` in `clienttest` are the checks; the
one worth reading is the last assertion in each, which is the silent case.

---

## 7. What was built, and what has and has not been run

Phase one is written and every suite is green; **none of it has been in front
of GTA III.** That is the same state M2, M3 and M4 were each in before their
first live run went badly, so the list at the end of this section is not a
formality.

1. `addresses.h` — the `CDamageManager` layout, the eleven accessors, the four
   appliers, `SetupDamageAfterLoad`, `Fix`, `VehicleDamage`, `BurstTyre`'s
   vtable slot, and the thresholds, each with the instruction that proves it;
   `static_assert`s pinning `sizeof` `0x1C`, `Damage` at `SIZEOF_VEHICLE`, and
   `Doors` at `Damage + 0x1C`.
2. `sdk/include/coopiii/protocol.h` — `VehicleDamageBody`, the two opcodes, and
   the wire arithmetic, which lives in the sdk rather than in the client
   because the server needs the same merge and must not grow an `addresses.h`.
   `client/src/game/cardamage.h` `static_assert`s the sdk's counts against the
   ones read out of `gta3.exe`, so the two agree with each other rather than
   each agreeing only with itself.
3. `client/src/game/cardamage.h` — the engine-facing half, deliberately pure so
   `clienttest` can reach all of it: the §3.2 raw-byte → damage-level mapping,
   the light derivation, and the panel/door → `m_aCarNodes` pairing.
4. `client/src/game/vehicle.cpp` — `SampleLocalVehicleDamage`,
   `ApplyRemoteVehicleDamage` (status **and** applier, per §5.1) and
   `SetVehicleObserved`, wired into the two correction paths that already
   answer "is this car ours".
5. `client/src/client.cpp` — change-only send, the merge on receipt, and the
   hold-until-spawned pass beside `UpdateRemoteSeats`.
6. `server/` — the entitlement gate (the driver, and nobody else), the merge,
   the relay, and the backfill row.

Green, and what they actually cover: `clienttest` 764 checks, `sessiontest`,
`basetest`, `interptest`, `patterntest`, `nettest` against a real `server.exe`.
Nine new `clienttest` cases and three new `sessiontest` cases. What they pin is
**decisions and bounds** — the door mapping, the join, the component pairing,
the hold-until-spawned reconciliation, the entitlement, the backfill — because
none of them can call an engine function.

The bounds in (3) and (2) are not decoration and this is the fourth time:

- `SetDoorStatus` is `mov [ecx+edx+9],al` and `SetWheelStatus` is
  `mov [ecx+edx+5],al`. **Neither checks its index.** A door index off a socket
  writes one byte anywhere the index reaches — `m_doorStatus[24]` lands in
  `m_panelStatus`, `m_doorStatus[0x100]` lands in a `CDoor`. Six doors is the
  bound and it is ours to enforce.
- `SetPanelStatus` and `SetLightStatus` compute a shift and `shl` masks it to
  five bits, so an out-of-range panel silently aliases a valid one rather than
  corrupting anything. Different failure, same missing check.

That is the same class as the four-entry animation group (`protocol.md` §1.8.1),
the twelve-slot node array (§1.8.1.1) and `CreateInstance`'s six-entry
`m_comps` (§1.12): **CoopIII drives an engine function past a bound the engine
does not check.**

And the things no headless test can reach, for whoever runs this in the game
first, in the order they are most likely to be what is wrong:

1. **That an observer's car stops inventing dents.** Drive a remote player's car
   into a wall on your own screen and watch it stay straight. If it dents,
   `bCollisionProof` is not being set, or it is being cleared by something.
2. **That a reported dent actually shows.** The status write is the easy half;
   the `Set*Damage` call is the half that has an `m_aCarNodes[component] == nil`
   early return for models that do not have that part, and a wrong component
   index there is a silent no-op, not a crash.
3. **That a part flying off on an observer's screen looks right rather than
   late.** §5.2's `noFlyingComponents = false` for live changes is the guess in
   this design with the least evidence behind it.
4. **That a late joiner gets dented cars dented and does not get a shower of
   doors.** That is `true` on the backfill path, and it is the same code with
   the opposite flag, so getting one right and the other wrong is exactly the
   failure to expect.
5. **That nothing double-counts.** A dent the local engine produced on a car we
   own is sampled and sent; a dent applied from the wire must not be sampled and
   sent back. `g_replaying` in `combat.cpp` is the shape to copy, and a feedback
   loop here would look like a car getting steadily worse rather than like a
   bug.
