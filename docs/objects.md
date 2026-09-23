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

### 2.2 A bullet has never broken one

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
`ObjectDamage`. It is a raw pointer the engine never clears, so before it is
trusted at all it is checked against the pool it claims to be in: inside the
entry array, on a slot boundary, and not a free slot. A pointer that fails any
of the three is `NOBODY`, which falls to the host - the safe direction, because
the worst case is one extra packet rather than a wrong read.

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

Opcodes `0xC0`-`0xCF` are reserved for this; two are used.

| opcode | name | to | body |
|---|---|---|---|
| `0xC0` | `C_ObjectBroken` | server | `ObjectBreakBody` |
| `0xC1` | `S_ObjectBroken` | everyone but the reporter | `playerId` + `ObjectBreakBody` |

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
```

Both reliable, on `CH_EVENT`. There is deliberately **no snapshot component, no
server-side table and no backfill**, and §1's 80 m horizon is the reason: the
engine itself throws the state away, so there is nothing a joiner could be told
that would still be true by the time they finished loading. The server's whole
job is to stamp the sender and pass it on, the same as a ped drop.

Exclusivity does not arise either, which is the interesting difference from a
pickup: two players can both correctly break the same crate, and the second
break is a no-op on every machine.

---

## 8. What this deliberately does not do

**A knocked-over lamp post does not lie down on the other screen.** Uprooting
is a *different mechanism* from breaking: `CPhysical` clears `bIsStatic` and
calls `AddToMovingList` when the impulse beats `m_fUprootLimit`, and where the
post ends up after that is local physics, which `roadmap.md` §2.4 says is not
reproducible. It is a transform, not a latch, and carrying it honestly means
either a stream or a one-shot resting-place packet once the object goes back to
sleep.

That is named work, not an oversight, and it is worth being precise about what
is and is not fixed by what shipped. For the 29 breakable models:

- The ones that **vanish** - crates, pallets, cones, `smashbar`, everything
  with effect 3, 50, 60, 70 or 80 - are fully handled. Those are the
  satisfying ones and they are now the same on both screens.
- The ones with effect 1 (`change_model`) - lamp posts, traffic lights, meters,
  bins, hydrants, benches - now show the *damaged model* on both screens. A
  post that was also uprooted is bent-and-standing here and lying down there.
  Better than pristine-versus-gone, and not yet right.

Opcodes `0xC2`-`0xCF` are reserved for it.

**Glass is untouched.** `CGlass` is its own system with its own arrays
(`WindowRespondsToCollision`, `WindowRespondsToExplosion`) and it never goes
through `ObjectDamage`. Separate job.

**Script objects are untouched.** `MISSION_OBJECT` exists on the machine
running the script, which is Area D's problem and `campaign.md`'s.

---

## 9. What has not been in front of GTA III

Everything above is proved against the binary or is design. **None of it has
run in the game.** `tools/objecttest` is 55 checks and green, and covers the
wire layout, the tolerance, the break-state arithmetic, the replay count, the
reporting truth table, and the identity matcher over a pool built by hand out
of the same offsets and the same `0x19C` stride the engine uses.

What that leaves untested, exactly:

1. **The two detours.** `CObject::ObjectDamage` is `__thiscall(float)`, hooked
   as `__fastcall` with a dummy `edx` - the standard trick here, but it has not
   been run. `CObject::ProcessControl` calls `ObjectDamage` for every object
   with a damage effect *every frame*, with `m_fDamageImpulse` usually zero, so
   the detour is on a hot path and the cost of the two byte reads either side
   of it is a guess until somebody watches a frame time.
2. **`m_pDamageEntity` being the right entity at the moment `ObjectDamage`
   runs.** It is set by `CPhysical`'s own collision bookkeeping next to
   `m_fDamageImpulse`, and the `ProcessControl` path is a frame later than the
   collision that set it. The failure mode is a stale pointer, which the pool
   check turns into `NOBODY`, which the host reports - one extra packet, not a
   wrong break. Worth confirming rather than assuming.
3. **Whether a replica's collision produces a local break at all.** A remote
   car is simulated locally and corrected from the wire every frame
   (`protocol.md` §2.8), and whether that generates the same collision events
   as a real one is a measurement nobody has made. If it does not, the
   `REPLICA` arm never fires and costs nothing; if it does, it is doing exactly
   its job.
4. **Whether the receiver's replayed `ObjectDamage` looks right.** The amounts,
   the particles and the sound are the engine's, but nobody has watched a crate
   burst on a second screen.

The one thing to watch in the log is the heartbeat line: `broken here` moving
while `reported` and `from the wire` stay at zero is the shape of "nothing is
reaching anybody".
