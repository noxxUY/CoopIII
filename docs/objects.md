# Breakable street objects

Lamp posts, traffic lights, parking meters, phone-box-sized newspaper machines,
bins, post boxes, fire hydrants, benches, cones, crates, pallets, barriers.
Today every one of them breaks per machine: one player mows down a row of lamp
posts and on the other screen the street is untouched.

This is the first time CoopIII has touched `CObject`, so everything below was
walked out of the retail image. The addresses and the disassembly that proves
each one live in `client/src/game/addresses.h` under `---- breakable street
objects ----`; this file is the argument.

---

## 1. What a breakable object actually is

**A `CObject` in the object pool, placed there by the map.** The whole class is
pinned by one function, `CObjectData::SetObjectData` (`0x004BC270`), which
copies ten fields out of the `object.dat` table into a fresh `CObject` in one
straight run and so fixes five `CObject` offsets and two flag bytes at once.

Three members decide everything:

| member | offset | what it decides |
|---|---|---|
| `ObjectCreatedBy` | `+0x174` | whose object this is: 1 the map's, 2 the script's, 3 debris, 4 a cutscene's |
| `m_nCollisionDamageEffect` | `+0x17C` | **whether it can break at all**, and what breaking looks like |
| `m_fCollisionDamageMultiplier` | `+0x178` | how hard it has to be hit |

Both of the last two come out of `data/object.dat`, which is a text file
shipped with the game and therefore the same on every install. That matters
later: it is why two machines given the same damage number reach the same
decision without being told to.

### Breaking is a state, not a destroy-and-replace

`CObject::ObjectDamage` (`0x004BB240`, `__thiscall(float)`) is the one place an
object breaks. **Nothing in it frees, allocates, or touches a world list.**
Every one of its nine arms writes flags on the `CObject` that is already
standing there:

```
case 1  (change_model)         bRenderDamaged = true
case 2  (split_model)          nothing at all
case 3  (smash_completely)     bIsVisible=0, bUsesCollision=0, bIsStatic=1,
                               bExplosionProof=1, move/turn speed zeroed
case 4  (change_then_smash)    bRenderDamaged ? case 3 : bRenderDamaged = true
case 50/60/70/80               case 3, plus particles and a sound
```

So a break is a **latch**: it only goes one way, two players breaking the same
crate is not a conflict, and applying the same break twice is a no-op. That is
what makes this a far smaller feature than a ped or a car - there is no
creation, no deletion and no ownership handshake anywhere in it.

The threshold the whole thing turns on is one compare:

```
004BB319  fld st(0) / fmul [ecx+178h] / fcomp [005F7D88h]   amount * mult > 150.0f
```

and the two gates in front of it are `m_nCollisionDamageEffect != 0` and
`bUsesCollision`.

### Which objects, and how many

`CFileLoader::LoadObjectInstance`'s entire decision is `mi->GetObjectID() ==
-1`: an IPL instance whose model has an `object.dat` entry becomes a
`CDummyObject`, and everything else becomes a `CBuilding` or a `CTreadable`.
Parsing the 13 IPLs `gta3.dat` loads and joining them against `object.dat`:

- **8689** map instances in total
- **2336** of them become `CDummyObject`s
- **1851** of those have a non-zero damage effect - the breakable street furniture

By model, the top of the list is 392 `doublestreetlght1`, 333 `trafficlight1`,
184 `lamppost3`, 119 `lamppost1`, 107 `bar_barrier10`, 95 `cardboardbox4`,
70 `parkbench1`, 67 `lamppost2`, 64 `woodenbox`. 29 models in all.

### The 80 m horizon, which turns out to shape the whole design

`CPopulation::ManagePopulation` (`0x004F3B90`) walks 1/32 of the object pool
and 1/32 of the dummy pool every frame. Past **80 m** from the local player, a
`GAME_OBJECT` is handed to `ConvertToDummyObject`, which builds a
`CDummyObject` out of nothing but the model index, the `RwObject` and
`m_level`, and places it at `m_objectMatrix`. **Every flag `ObjectDamage`
wrote is thrown away**, and walking back in builds a brand new, pristine
`CObject`.

That is retail single-player behaviour, not a bug: street furniture stands back
up once you have left the block. Two things follow and the design leans on both:

1. **A broken object has a lifetime of one visit.** There is nothing for the
   server to remember and nothing to tell a joiner.
2. **The state only matters while two players are inside the same 80 m
   bubble** - which is exactly the co-op case, and is also why divergence is
   self-limiting: once everybody leaves, both machines are pristine again and
   agree for free.

---

## 2. What is *already* the same, measured before building anything

`roadmap.md` §6 - and three struck items - say measure first. Two measurements
took most of this job away.

### 2.1 An explosion already agrees, everywhere, for free

The object arm of `CWorld::TriggerExplosionSectorList` (`0x004B1340`):

```
004B15E4  fld [esp+0CCh] / fsub [esp+10h]   fRadius - fMagnitude
004B15EF  fmul [005F799Ch]                  * 2.0f
004B15F5  fdiv [esp+0CCh]                   / fRadius
004B15FE  fcomp [005F7994h]                 Min(.., 1.0f)
004B1616  fld [005F79A0h] / fmul            * 300.0f
004B1626  call 004BB240                     CObject::ObjectDamage
```

**The amount is a pure function of the two positions and the radius.** No RNG,
no impulse, no timestep, nothing carried over from a previous frame.
`ObjectDamage` then compares it against a multiplier out of `object.dat`.

CoopIII already replays every player's explosion at a world position every
machine agrees on (`protocol.md` §1.9.3). So **objects destroyed by an
explosion already break identically on every machine**, and a packet about one
would be a duplicate. This is the same finding `roadmap.md` §5.7 made about
pavement fires, reached the same way and for the same reason.

The seam therefore goes *quiet* inside an explosion rather than merely
tolerating the duplicates. `CWorld::TriggerExplosion` (`0x004B1140`) has
**exactly two callers in the whole image**, both inside `CExplosion`, so one
scoped guard covers every explosion there is. Without it a single rocket would
send one reliable packet per bin in its radius.

### 2.2 A bullet has never broken one — and cannot knock a lamp post over either

`CWeapon::FireInstantHit`'s `ENTITY_TYPE_OBJECT` arm adds eight spark
particles, clears `bIsStatic` when `m_fUprootLimit <= 0`, applies
`normal * -4.0f`, and stops. It never calls `ObjectDamage`. A whole-image scan
for rel32 targets equal to `0x004BB240` finds **five call sites and nothing in
`CWeapon` is among them**:

```
00497AA5   CPhysical, entity against a static object
0049D84B   CPhysical, two moving entities
004B1626   CWorld::TriggerExplosionSectorList, static arm
004B1A24   CWorld::TriggerExplosionSectorList, moving arm
004BB055   CObject::ProcessControl, with m_fDamageImpulse
```

Shooting street furniture in retail III nudges it; it does not break it. The
one exception is `CWeapon::BlowUpExplosiveThings`, which turns a barrel or a
petrol pump into a `CExplosion` - and §2.1 has already dealt with explosions.

**It does not even nudge a lamp post**, and that half was written down wrong
the first time. The arm above is shared by three functions - `DoBulletImpact`
(0x0055F950, arm at 0x00560481), `FireShotgun` (0x005616A2) and
`CBulletInfo::Update`, the sniper round (0x00558A64; first read as
`FireMelee`, which has no object arm, and `addresses.h` keeps the constant
under that name) - and every one of them is the same instructions:

```
mov al,[X+50h] / and al,7 / cmp al,4          ENTITY_TYPE_OBJECT
mov al,[X+122h] / shr al,2 / and al,1         bInfiniteMass -> skip
mov al,[X+51h]  / shr al,2 / and al,1         GetIsStatic()
fld [X+170h] / fcomp [const] ...              m_fUprootLimit <= 0.0f
[X+51h] &= 0FBh / call 004958F0               SetIsStatic(false), AddToMovingList
                                              then, only if !GetIsStatic():
                                              ApplyMoveForce(normal * -k)
```

The three constants it compares against are `0x00603060`, `0x00603060` and
`0x00602C88`, and all three read `00000000` - so the gate really is
`<= 0`, not a small threshold. And `data/object.dat`, which ships with the
game, gives column G:

| model | uproot limit | map instances |
|---|---|---|
| `doublestreetlght1`, `lamppost1/2/3` | **400.0** | 762 |
| `trafficlight1` | **500.0** | 333 |
| `bar_barrier10/12`, the `lhouse` barriers | **350.0** | 107+ |
| `parkingmeter`, `bin1`, `postbox1`, `fire_hydrant` | **100.0** | |
| `trafficcone` | **10.0** | 39 |
| `parkbench1` | **5.0** | 70 |
| `smashbar` | **1000.0** | |
| `woodenbox`, `cardboardbox`/`2`/`4`, `wastebin`, `dump1`, `palette`, `parktable1`, `papermachn01`, `fishstall03/04` | **0.0** | 204+ |

Every piece of street furniture worth the name is above zero, so the gate
fails - and because the object is still static, the `!GetIsStatic()` that
guards the move force fails too. **Shooting a lamp post in retail 1.0
produces eight sparks and a sound and moves nothing at all.** The only
breakable models a bullet can knock loose are the boxes and bins in the last
row, and those it moves on every machine, because §1.9.2 replays the shot
through the engine.

`tools/objecttest` carries that table, so the claim is a regression test
rather than a sentence.

### What is left

Two arms of `CPhysical` and `CObject::ProcessControl`, all three of which are
**a collision**. The brief guessed "a car driving into them and gunfire"; the
binary says gunfire was never in it. **The whole remaining feature is: somebody
drove into it.** That is what got built.

---

## 3. Naming one across machines

A pool index is a local number, and for objects it is *worse* than it is
anywhere else in this project: `ManagePopulation` is converting objects to
dummies and back on every frame, keyed off **the local player's** position,
and the streamer centres on one player (`roadmap.md` §2.1). Two machines are
churning different slots from the first second.

There are already three answers in this codebase and the brief was right that
one of them fits.

**The server-allocated netId (`population.md` §3) is overkill and would be
wrong.** A temp-id handshake exists because *the creator invents the entity*
and nobody else has one. Map objects are not created by anybody - the map
places 1851 of them identically on every machine before a single packet is
sent. There is nothing to name that both sides do not already have.

**The map generator index (a parked car's answer) does not exist here.** A
`CCarGenerator` is an entry in an array the engine keeps and indexes; a map
object is an IPL line that becomes a pool entry with no index stored on it
anywhere. The nearest equivalent would be the dummy pool slot, and that churns
for exactly the reason above.

**So it is the pickup's answer: position plus model.** And it is a *better* fit
here than it is for pickups, for one reason `pickups.md` §7 had to hedge about
and this does not: the coordinate is never computed at runtime.
`CFileLoader::LoadObjectInstance` `sscanf`s it out of a text file that ships
with the game, `CObject::CObject(CDummyObject*)` copies it into
`m_objectMatrix`, and `ConvertToDummyObject` reads it back out. A ped drop's z
comes out of `CWorld::FindGroundZFor3DCoord`; this is the same characters in
the same file on both machines.

### The key is `m_objectMatrix`, not the entity's position

`m_objectMatrix` is at `+0x128` and its position at `+0x158` - witnessed
directly by `ManagePopulation`, which measures *both* the live position
(`lea eax,[esi+34h]`) and the placement (`lea eax,[esi+158h]`) against 80 m.

This distinction is load-bearing. The moment something knocks an object loose,
the entity's own position starts moving and stops being a name for it -
differently on each machine, because that motion is physics. `m_objectMatrix`
does not move, and survives both conversions untouched.

---

## 4. The measurement that says 0.25 m is safe

`pickups.md` compared 312 script pickup coordinates pairwise. The equivalent
work here is all **1851** breakable map instances, compared pairwise inside
each model index:

```
GLOBAL minimum same-model pair distance: 0.5992 m   (papermachn01)
pairs closer than 0.50 m: 0
pairs closer than 0.25 m: 0
```

Per model, the only ones with anything inside 2 m at all:

| model | n | closest pair | pairs < 2 m |
|---|---|---|---|
| `papermachn01` | 20 | **0.5992 m** | 18 |
| `cardboardbox2` | 25 | 0.6776 m | 2 |
| `trafficcone` | 39 | 0.8646 m | 13 |
| `cardboardbox4` | 95 | 0.9330 m | 69 |
| `woodenbox` | 64 | 1.2124 m | 16 |

Everything else is at least 2.27 m apart, and the two commonest models by far
- 392 `doublestreetlght1` and 333 `trafficlight1` - are 11.97 m and 8.10 m
apart at their closest.

So the 118 near pairs are exactly what you would expect: stacked crates, rows
of cones, and newspaper boxes standing side by side on a pavement. **None of
them is at distance zero**, which is where `pickups.md`'s 29 near pairs all
were, and the closest of them clears a 0.25 m tolerance by 2.4x.

`tools/objecttest` carries `0.5992` as a constant and fails if the tolerance
ever grows into it, so this measurement is a regression test rather than a
sentence in a document.

The matcher also reports when two live objects matched, and the client logs it,
because a design that silently picked one of two would only ever be noticed as
"sometimes the wrong crate breaks".

---

## 5. Who reports

`roadmap.md` §5.8 settled this for a car nobody is driving: an ownerless world
entity is the **host's**, and exactly one machine reports.

**The literal answer does not transplant, and the binary is what says so.** An
object 80 m from the host is not a `CObject` on the host at all - it is a
dummy, or not converted yet - so the host has nothing to observe and nothing to
report. §5.8 works for a parked car because the *server* holds a row for that
car whatever the distance. There is no row here, and §1 says there must not be
one.

What does transplant is the shape, moved one entity across: **the machine that
owns whatever broke it reports it, and the host owns whatever nobody owns.**

```
ObjectCreatedBy != GAME_OBJECT       -> nobody reports  (script's, debris, cutscene)
the object is a pickup's             -> nobody reports  (docs/pickups.md owns it)
no session                           -> nobody reports  (single player is untouched)
inside CWorld::TriggerExplosion      -> nobody reports  (§2.1, already agreed)
cause is an entity we host           -> WE report
cause is a replica of somebody else's-> THEY report, we stay quiet
cause is nobody                      -> the HOST reports
```

"Cause" is `CPhysical::m_pDamageEntity` (`+0x110`), the neighbour of the
`m_fDamageImpulse` that `CObject::ProcessControl` passes straight into
`ObjectDamage`. It is still checked against the pool it claims to be in before
it is trusted at all - inside the entry array, on a slot boundary, and not a
free slot.

**It is not, however, a pointer "the engine never clears", and that was worth
finding out.** `CPhysical::ProcessControl` (`0x00495F10`) zeroes both halves of
the collision record at its top:

```
00495F78  mov dword [ebx+10Ch],0    m_fDamageImpulse = 0
00495F82  mov dword [ebx+110h],0    m_pDamageEntity  = nil
```

and `CObject::ProcessControl` calls `ObjectDamage(m_fDamageImpulse)`
immediately *before* that, at `0x004BB04F`/`0x004BB055`. So the pointer a
break detour reads is always this frame's. §9's second open question is
answered, in the safe direction, and the same fact is what makes the uproot
path below able to ask "did something collide with this, just now".

### 5.1 Who reports a bullet

A bullet writes no collision record at all, so `m_pDamageEntity` is nil and
the rule above reads `NOBODY` - which falls to the host. **That is the wrong
machine, and §5 above already contains the proof without applying it**:
`ManagePopulation` turns any map object more than 80 m from the local player
back into a dummy, so the host is the one participant in the session who is
not guaranteed to have the object at all. Handing an ownerless event to the
host hands it to whoever is most likely to be somewhere else.

So an uproot with no impulse behind it is attributed to the trigger pull it
happened inside:

```
inside combat.cpp's replay of somebody else's shot  -> THEY report
outside it                                          -> WE report
```

and that is a fact rather than a guess, for two reasons. The object arm of
§2.2 is the only thing left in the image that can clear `bIsStatic` on a
breakable map object without writing an impulse - a collision writes one in
the same frame, and a blast is already guarded - and that arm only ever runs
inside somebody's `CWeapon::Fire`. **The shooter knows the break was theirs
because the engine is still inside their own trigger pull when it happens.**
`combat.cpp` already holds exactly that flag for the duration of
`ReplayRemoteShot`'s call; all that was added is a way to ask it
(`ReplayingRemoteShot`).

The alternative - keep it with the host and make the host's break travel - is
worse for the 80 m reason above, and worse again because it needs the host to
be *watching*: a non-host who shoots a crate loose on a street the host has
never visited gets no report from anybody, which is exactly the symptom this
was reported as.

**Nothing is ever suppressed locally.** An observer whose own engine breaks the
object - because a replica of somebody else's car really did push through it
here - is allowed to break it. Suppressing would leave a car sitting inside an
intact lamp post. It simply does not *report*, because the owner's report is
coming and applying a break twice is a no-op. That asymmetry is what makes
"exactly one reporter" cheap here in a way it is not for health.

---

## 6. How the receiver applies it

**It replays the engine's own `ObjectDamage`, it does not write flags.**

Writing `bIsVisible = false` by hand would produce an object that vanished with
no particles, no sound, and four other flags left wrong. Every one of those is
the engine's business, and `roadmap.md` §6 already says to do what the engine
does.

So the packet carries two things besides the name:

- **`amount`** - the float the reporter's engine actually passed to
  `ObjectDamage`, not a made-up large number. The engine reads it twice: for
  `amount * m_fCollisionDamageMultiplier > 150.0f`, and for
  `fDirectionZ = 0.0002f * amount`, which is how fast the debris flies. The
  real number makes the crate burst the same way on both screens.
- **`state`** - two bits, because `ObjectDamage` has two outcomes. The receiver
  replays until its own object matches, **at most twice**, which is exactly
  what `DAMAGE_EFFECT_CHANGE_THEN_SMASH` needs and more than anything else
  needs.

The one place the receiver is allowed to adjust the number is upward, and only
upward: if `amount * multiplier` does not clear 150 here, it is raised until it
does. The two machines can disagree by a hair on an impulse that was only just
over the line, and an applier that silently did nothing would leave the object
standing here and gone there. The receiver is not deciding anything by doing
that - the reporter already decided, and this is the decision being carried
out.

A replay that finds no local object does nothing and counts it. That is the
ordinary case, not an error: their player is standing next to it and ours is
300 m away, so our copy is a dummy. When we walk over there the engine builds a
pristine one, which is what single player does too.

---

## 7. The wire

`PROTOCOL_VERSION` moves for this. The merged build is **18**, one number for
the ten branches that landed together; see the version history in
`sdk/include/coopiii/protocol.h`. The check this section used to record still
holds and is worth keeping: against the `wip-snapshot` capture of the
uncommitted work in the main tree, this uses nothing in `0xC0`-`0xCF`, declares
no `ObjectIdent` or `ObjectBreakBody`, and leaves `Vec3` and `PacketHeader`
byte-identical, so the sizes below hold on top of it. (The opcodes that tree
adds and this section does not are `0x74`/`0x75`, `C_/S_PED_BODY_PART`.)

Opcodes `0xC0`-`0xCF` are reserved for this; four are used.

| opcode | name | to | body |
|---|---|---|---|
| `0xC0` | `C_ObjectBroken` | server | `ObjectBreakBody` |
| `0xC1` | `S_ObjectBroken` | everyone but the reporter | `playerId` + `ObjectBreakBody` |
| `0xC2` | `C_ObjectSettled` | server | `ObjectRestBody` |
| `0xC3` | `S_ObjectSettled` | everyone but the reporter | `playerId` + `ObjectRestBody` |

```
struct ObjectIdent {      // 16 bytes
    Vec3    pos;          // m_objectMatrix.GetPosition() - the IPL coordinate
    int16_t modelIndex;
    uint8_t pad0, pad1;
};

struct ObjectBreakBody {  // 24 bytes
    ObjectIdent ident;
    float       amount;   // what the reporter's engine handed ObjectDamage
    uint8_t     state;    // ObjectBreakFlags
    uint8_t     pad[3];
};

struct ObjectRestBody {   // 64 bytes
    ObjectIdent ident;
    Vec3        right;    // CMatrix, entity +0x04
    Vec3        forward;  //          entity +0x14
    Vec3        up;       //          entity +0x24
    Vec3        pos;      //          entity +0x34, where it is lying
};
```

`ObjectBreakFlags` gained a third bit, `OBJ_BREAK_UPROOTED` (`1 << 2`), which
costs nothing - the byte had six spare - and means "`bIsStatic` is clear on my
copy". §8 is what it is for.

`ObjectRestBody` is the one packet in this feature that is not a latch, and it
is deliberately the full 3x3 rather than a heading: a lamp post does not lie
down about the z axis, it falls over, and the two vectors that say so are the
ones a heading throws away. 48 bytes once per uprooting is cheaper than
anything that would let the receiver work it out for itself.

**It is also the packet that proves why the key had to be `m_objectMatrix`.**
By the time a resting place exists, the object is metres from where the map
put it - and on two machines it is metres away in two different directions.
The placement has not moved on either.

Both reliable, on `CH_EVENT`. There is deliberately **no snapshot component, no
server-side table and no backfill**, and §1's 80 m horizon is the reason: the
engine itself throws the state away, so there is nothing a joiner could be told
that would still be true by the time they finished loading. The server's whole
job is to stamp the sender and pass it on, the same as a ped drop.

Exclusivity does not arise either, which is the interesting difference from a
pickup: two players can both correctly break the same crate, and the second
break is a no-op on every machine.

---

## 8. Uprooting — the half this used to leave out

**Uprooting is one bit and one list, and nothing else.** An object goes from
standing to lying down when `bIsStatic` (CEntity byte A `+0x51`, bit 2) is
cleared and the object is handed to `CPhysical::AddToMovingList`
(`0x004958F0`). From that instant `CWorld::Process` calls its `ProcessControl`
every frame and ordinary physics takes it. There is no second model, no flag
on `CObject`, and no call anywhere in the image that means "fall over".

It is therefore **orthogonal to breaking**, and `ObjectDamage` says so itself:
none of its nine arms clears `bIsStatic`, and the smash arm *sets* it
(`[ecx+51h] &= 0FBh / |= 4` at `0x004BB3A0`). A lamp post can be bent without
coming loose (impulse 200 - past 150, short of 400) and can come loose without
being bent (a bench, uproot limit 5, break threshold 150). Two decisions off
one number, with two different thresholds, which is why one of them travelling
never implied the other.

Three places decide it and all three read `m_fUprootLimit` (`+0x170`):

| | test | where |
|---|---|---|
| a collision | `impulse > m_fUprootLimit \|\| IsFence(model)` | `0x00497559`, `0x00497B6B`; uproot at `0x00497EB1` |
| a blast | `fPower > m_fUprootLimit` | `0x004B1473`; uproot at `0x004B154D` and `0x004B163A` |
| a bullet, a pellet, a bat | `m_fUprootLimit <= 0.0f` | §2.2 |

### 8.1 Two of the three already agree, and the third is the report

The same shape as §2.1 and §2.2, reached the same way.

- **A blast** uproots identically on every machine, because its power is the
  same pure function of two positions and a radius that its damage is. Free.
- **A bullet** uproots identically on every machine, because `protocol.md`
  §1.9.2 replays the shot through the engine's own `CWeapon::Fire` on the
  remote ped, out of the wire's muzzle and along the wire's direction, so
  every observer's own `DoBulletImpact` runs the same object arm against the
  same map object and applies the same `normal * -4.0f`. Also free - and for
  street furniture it is free in the strongest possible sense, because §2.2
  says a bullet cannot knock any of it over at all.
- **A collision** does not. Nobody else ran it. That is the case the report
  was about, and it is the only one that needed a packet.

### 8.2 What travels is the resting place, and only the resting place

Not the impulse. `roadmap.md` §2.4 is unambiguous that `ms_fTimeStep` is
frame-time-derived, so two machines handed the identical impulse put the same
post down in two different places - and a machine that never uprooted it has
nothing to integrate anyway. Not a stream either. **One packet per uprooting**,
sent when the engine's own sleep test says the object has finished:

```
004960D7  inc byte [ebx+0EDh]          m_nStaticFrames++
004960DD  cmp byte [ebx+0EDh],0Ah      more than 10 quiet frames
004960F1  [ebx+51h] &= 0FBh / |= 4     SetIsStatic(true)
004960FB  move/turn speed and both frictions zeroed
00496172  m_nStaticFrames = 0          (the else arm)
```

and `CWorld::Process` then unlinks it, in its own moving-entity loop:

```
004B1B99  call [edi+20h]               ProcessControl
004B1B9C  [ebp+51h] >> 2 & 1           GetIsStatic()
004B1BA8  call 00495940                RemoveFromMovingList
```

(again at `0x004B1BF3` for the postponed pass.) That second listing is why
**nothing here ever writes into the moving list to put an object back**.
Setting `bIsStatic` is enough; the engine does the unlink on its own next
pass. Writing into that list is what `client/src/game/movinglist.h` exists to
clean up after, and this feature deliberately does not.

The uproot is noticed by a detour on `AddToMovingList` itself, which is the
one door all six relevant call sites go through, and it is a read-only
bracket: it calls the original and then looks. The cause is read there rather
than later, because that is the only moment it is still a live fact - §5's
`m_pDamageEntity` is cleared on the next frame, and a bullet never writes one.

The receiver's apply is the engine's own three steps, in the engine's own
order: write the matrix, `CMatrix::UpdateRW` (`0x004B8EC0`),
`CEntity::UpdateRwFrame` (`0x00474330`), then `CPhysical::RemoveAndAdd`
(`0x00495540`). The last is not optional - an entity whose position is written
from outside stays filed in the sector it was added in, and
`CRenderer::ScanWorld` only walks the sectors around the camera, so a post
that fell into the next sector would simply stop being drawn.

Nine floats off a socket then go into the matrix the collision code reads, so
they are checked first (`SaneRotation`): every row finite and within a factor
of two of unit length. That rejects a NaN before it propagates - `pedanim.h`'s
rule, unchanged - and rejects the all-zero body a truncated or forged packet
carries, which would otherwise collapse the object's collision volume to a
point.

### 8.3 The uproot bit is what stops the post teleporting

`OBJ_BREAK_UPROOTED` rides the break packet and costs nothing: the state byte
had six spare bits. It is not the fix by itself - it is what lets an observer
drop its own post *at the moment the break arrives* instead of watching it
stand for a second and then snap flat. The receiver clears `bIsStatic` and
links the object exactly once, guarded on `bIsStatic` being set, because the
engine's invariant is that the moving list holds exactly the non-static
entities and a second node for one of them is the bug `movinglist.h` exists
for.

### 8.4 What is still different

**An explosion stays quiet about resting places too**, for the reason it stays
quiet about breaks: one rocket into a row of bins would be one reliable packet
per bin, and every machine already uprooted all of them from the same number.
They end up lying in slightly different places. That is the one difference
this deliberately leaves standing, and it is a far smaller one than
standing-versus-lying.

**A drive-by moves nothing, on anybody's screen, and breaks glass.** Drive-bys
travel now (`game/driveby.h`): each round goes out as a shot and every other
machine draws it without running the engine's fire path, whose every culprit
is `FindPlayerPed()`. That leaves §8.1's free bullet answer out of it, and what
it would have carried turns out to be one thing. `FireInstantHitFromCar` has an
arm for a ped, one for a car and one for everything else, and the last one is
`CGlass::WasGlassHitByBullet` and nothing more: no `ObjectDamage`, no uproot
(`addresses.h`, "the drive-by"). So a crate shot from a car stays put on the
shooter's screen too, and a window broke there alone. `DriveByImpact` now hands
the same entity and point to the same function for somebody else's round, so
the window breaks on every screen; its address is a lead, checked against both
of the engine's calls to it before it is used (`addresses-unverified.md`).

**The watch table is 24 deep and the newest entry is dropped past that.** A
car ploughing a row of lamp posts is nowhere near it and a rocket is excluded
by the paragraph above, but the number is in the heartbeat line so it stops
being a guess.

**Glass is not carried.** `CGlass` is its own system with its own arrays
(`WindowRespondsToCollision`, `WindowRespondsToExplosion`) and it never goes
through `ObjectDamage`, so nothing here reports it. Every machine breaks its own
windows from the same causes instead: a round on foot is replayed through
`CWeapon::Fire` and reaches `DoBulletImpact`'s call, a drive-by round reaches
the same call from `DriveByImpact` (above), and an explosion or a car is the
same event everywhere.

**Script objects are untouched.** `MISSION_OBJECT` exists on the machine
running the script, which is Area D's problem and `campaign.md`'s.

Opcodes `0xC4`-`0xCF` stay reserved.

---

## 9. What has not been in front of GTA III

Everything above is proved against the binary or is design. **None of it has
run in the game.** `tools/objecttest` is 86 checks and green, and covers the
wire layout, the tolerance, the break-state arithmetic, the replay count, the
reporting truth table, the uproot-cause truth table, the `object.dat` uproot
limits, the sanity test on a matrix off the wire, and the identity matcher
over a pool built by hand out of the same offsets and the same `0x19C` stride
the engine uses. `tools/clienttest` covers the routing either side of
`Client`, and `tools/sessiontest` covers the size-and-opcode gate the server's
dispatch goes through.

What that leaves untested, exactly:

1. **The three detours.** `CObject::ObjectDamage` is `__thiscall(float)`,
   hooked as `__fastcall` with a dummy `edx` - the standard trick here, but it
   has not been run. `CObject::ProcessControl` calls `ObjectDamage` for every
   object with a damage effect *every frame*, with `m_fDamageImpulse` usually
   zero, so the detour is on a hot path and the cost of the two byte reads
   either side of it is a guess until somebody watches a frame time.
   `CPhysical::AddToMovingList` is the third, and `CWorld::Add` routes every
   non-static entity through it, so the one byte read and compare that gets a
   ped or a car straight back out of it is the thing to watch there.
2. ~~**`m_pDamageEntity` being the right entity at the moment `ObjectDamage`
   runs.**~~ **Answered, §5.** `CPhysical::ProcessControl` zeroes both
   `m_fDamageImpulse` and `m_pDamageEntity` at its top, one call after
   `CObject::ProcessControl` reads them, so the pointer is never stale by more
   than the frame it was written in. What is still unmeasured is only whether
   the pool check ever fires at all.
3. **Whether a replica's collision produces a local break at all.** A remote
   car is simulated locally and corrected from the wire every frame
   (`protocol.md` §2.8), and whether that generates the same collision events
   as a real one is a measurement nobody has made. If it does not, the
   `REPLICA` arm never fires and costs nothing; if it does, it is doing exactly
   its job.
4. **Whether the receiver's replayed `ObjectDamage` looks right.** The amounts,
   the particles and the sound are the engine's, but nobody has watched a crate
   burst on a second screen.
5. **Whether a post dropped off the wire falls before its resting place
   arrives.** §8.3 clears `bIsStatic` on a standing copy the moment the break
   says the reporter's came loose, and what that looks like for the second or
   so before the rest packet lands has not been seen. A post standing perfectly
   upright with no angular velocity may barely move, in which case the bit is
   buying less than it looks like it should and the snap is doing all the work.
6. **Whether `CPhysical::RemoveAndAdd` is enough on its own** to put a
   written-in transform back in front of the renderer, or whether the object
   also needs its bounding rect recomputed. The call is the engine's own answer
   to "this entity moved" and it is called every frame by the physics for
   exactly that, so it should be - but it has never been called from outside
   the physics before.

Two things to watch in the log. `broken here` moving while `reported` and
`from the wire` stay at zero is the shape of "nothing is reaching anybody".
And on the uproot line, `came loose` moving while `rest sent` stays at zero
means objects are being knocked over here and never coming to rest - which
would be the watch table leaking, or the 80 m conversion taking them away
before the engine's sleep test gets to them.
