# Ambient peds and traffic

`protocol.md` §3 lists NPCs and traffic as deliberately out of v1 and calls
fixing them the single biggest item after it. This is the design for that.

The problem, stated plainly: every machine runs `CPopulation` and `CCarCtrl`
for itself, seeded differently, so the crowd on one screen is not the crowd on
the other. It is not cosmetic. An NPC that exists for one player attacks him,
blocks him and gets run over by a car the other player cannot see.

---

## 1. The model

**Whoever's engine made it, owns it.** An entity the local engine puts into the
world becomes a session entity hosted by this machine: it keeps running its own
AI here, and everywhere else it is a replica that is written to and never
decides anything. That is the same ownership rule CoopIII already applies to
players and to a driver's car, extended to everything the city generates.

Three things follow, and the third is the one that makes it work at all.

### 1.1 One seam catches everything

`CWorld::Add` is the single door every entity walks through on its way into the
world, whoever created it and for whatever reason. Hooking it means the traffic
generator, the pedestrian generator, a script spawn and a car falling out of a
crane all register the same way, with no list of creation paths to keep up to
date.

CoopIII already knows this function well, and knows something sharp about it:
`CWorld::Add` and `CWorld::Remove` are **not symmetric** - both are gated on
`bIsStatic`, which is what put a freed node in the moving list and crashed the
game (`addresses.h`, the moving-list block). Anything hooked here has to
survive that asymmetry rather than assume a matching pair.

### 1.2 Identity has to be handed out, not chosen

The machine that creates a ped does not know what to call it. It cannot pick a
number, because the other machine is creating its own peds at the same moment
and would pick the same one.

So the creator announces the new entity under a **temporary id of its own**,
the server allocates the real netId and tells everybody including the creator,
and the creator swaps one for the other. Everything after that speaks netIds.
This is the two-phase spawn from `protocol.md` §1.6 with one more phase on the
front, and the same reason: the authoritative name comes from the server.

### 1.3 The engine has to be told the truth about how crowded it is

This is the part that decides whether the whole design works or produces double
the city.

If every machine keeps generating and everything generated is replicated, then
every machine ends up holding its own population **plus** everyone else's. Two
players make twice the traffic, three make three times, and the pools run out.

The fix is not to suppress generation on all but one machine - that gives the
other players a dead city around them, because the generator works off *the
local player's* position (`roadmap.md` §2.1) and would never make anything near
anyone else.

The fix is to **count the replicas before asking the engine to generate**.
GTA III decides whether to make another car from `CCarCtrl::NumRandomCars` and
its siblings, and another pedestrian from `CPopulation::ms_nNumCivMale`,
`ms_nNumCivFemale` and `ms_nTotalCivPeds`. Those counters only ever count what
this machine made. Before the generator runs, write into them what is actually
standing around - replicas included - and put the originals back afterwards.

The engine then stops generating when the street in front of you is full,
whoever filled it. Nobody's city is empty, nobody's city is doubled, and the
density is the one the game was tuned for.

### 1.3.1 Measured 2026-09-22: for pedestrians, the engine was already doing it

The counter rewriting above turned out to be unnecessary on the ped side, and
writing it would have been actively wrong.

`CPed::CPed` calls `CPopulation::UpdatePedCount`, so **a replica is counted the
moment it is constructed**, exactly like a ped this machine generated - and
`CPopulation::Update` folds the per-type counters into `ms_nTotalPeds` in the
same function, immediately before `AddToPopulation` compares against it. The
gate was seeing the shared crowd all along.

Measured with two clients in one session, five seconds apart, both reporting
what the engine counted next to what CoopIII was holding:

```
machine A   engine counts 12 peds   hosts 7   holds 4 replicas
machine B   engine counts 12 peds   hosts 4   holds 7 replicas
```

`ms_nTotalPeds` is `hosted + replicas + 1` on both, and the one is the local
player. The union of the two populations is eleven pedestrians, and **both
machines see eleven** - not twenty-two. Each engine generated less because it
was already counting the other machine's peds.

So step 3 is struck for pedestrians, and the sharing is self-limiting. Had it
been built on the assumption in the paragraph above, the replicas would have
been added to counts that already included them, and the street would have
been starved rather than doubled - the exact opposite mistake, and the harder
one to notice.

**This said nothing about traffic**, and step 4 measured it rather than
assuming - see §1.3.2 immediately below. The guess recorded here, that
`CCarCtrl`'s counters are not maintained by a vehicle constructor, turned out
to be **wrong**: they are, by `CVehicle::CVehicle`. What is true is the
sharper thing §1.3.2 records, which is that *which* counter they are
maintained in depends on a byte the replica chooses.

### 1.3.2 Measured 2026-09-22: for traffic, the same - but only because the replica is put in the right counter

Step 4 repeated the §1.3.1 measurement for cars, because the note above was
explicit that the ped result said nothing about traffic. The answer is the
same and the reasoning is **not**, which matters more than the answer.

`CVehicle::CVehicle` calls `CCarCtrl::UpdateCarCount`, exactly as `CPed::CPed`
calls `CPopulation::UpdatePedCount` - so a vehicle replica is counted the
moment it is constructed, like a pedestrian one. `UpdateCarCount` is at
`0x004202E0` and its disassembly, its two jump tables and its two callers (the
constructor and the destructor) are in `addresses.h`.

The difference is that `UpdateCarCount` **switches on `VehicleCreatedBy`**.
Which counter a car lands in is decided by the byte at `+0x1F4`, and the
generator's first gate reads `NumRandomCars` on its own:

```
NumRandomCars >= m_nTrafficMultiplier * CarDensityMultiplier * CarNumberMultiplier
```

So a replica created the way a *claimed* car is created - `MISSION_VEHICLE`,
which is what `CREATE_CAR` uses - would be counted in `NumMissionCars`, a
counter that gate cannot see. Every observer would go on generating as if the
street were empty and the city really would double. The replica is therefore
created as **`RANDOM_VEHICLE`**, landing in the same counter its original
landed in on the machine that made it, with `bIsLocked` carrying the whole
deletion gate by itself (`!bIsLocked && CanBeDeleted()` is how every reaping
site in the engine spells it, so the locked half is sufficient).

Measured with two clients, walked out onto the street, sampled every five
seconds for a hundred seconds each:

```
machine A   engine counts 3 random cars   hosts 3   holds 0 replicas
machine B   engine counts 3 random cars   hosts 0   holds 3 replicas
```

`NumRandomCars == hosted + replicas` in **38 of 38 samples across both
machines**, and both machines count the same number - the union of the two
machines' traffic, not the sum of it. The transition is visible in the log
from the session where A was alone first: A generated 4 cars of its own, B
joined, A's own cars were recycled and replaced by replicas of B's, and
`NumRandomCars` stayed at 4 the whole way through rather than climbing to 8.

So **step 3 is struck for traffic as well**, and the sharing is self-limiting
for the same reason it is for pedestrians. The counter rewriting has now been
measured away twice and built zero times.

One consequence worth stating because it looks like a bug and is not: the
split between the two machines is **not** even, and it drifts. Whoever fills
the street first gets to host it, and the other engine stops generating
because the gate it reads is already satisfied. In the runs above one machine
ended up hosting all the traffic and the other none. That is §1.3 working
exactly as written - "the engine stops generating when the street in front of
you is full, whoever filled it" - but it does interact with §2.3: the
generator works off *the local player's* position, so a machine that has
stopped generating is relying on somebody else's traffic being near it. Two
players on opposite islands would each still generate their own, because
neither one's cars are anywhere near the other's gate. Two players in the same
street share. Nothing in between has been measured.

**This is the idea CoopAndreas is built on**, and it is worth saying where it
came from. It is a different person's GPL-3.0 project and none of its code is
here - nor could it be, since it is San Andreas and not one address or
structure survives the trip. What survives is the shape of the answer.

Worth knowing, because the owner assumed otherwise: CoopAndreas has **no
ownership handoff**. Its `AssignHost` is commented out in full. An entity stays
with its creator until it is dropped. So this design should not promise a
handoff on the strength of somebody else's working example.

---

## 2. What this costs, honestly

### 2.1 The wire

A player carries about 60 bytes at 25 Hz. Liberty City can put 20-30 peds and a
dozen cars around you. Replicating all of them the way a player is replicated
is roughly 40 entities × 60 bytes × 25 Hz, call it 60 KB/s per observer, each
way. That is not a co-op mod over the internet, it is a LAN toy.

So the rate has to fall off with distance, and most of these entities have to
be dead-reckoned rather than streamed:

- a ped or car the player can see and is close to: the player rate
- middle distance: a few times a second, and interpolate
- far but still in the world: position and heading occasionally, or nothing

The numbers are not settled here. What is settled is that a flat rate is not on
the table, and that this has to be measured rather than guessed.

**What step 4 actually shipped**, as the first and crudest slice of this: one
batched packet, 8 cars, 10 Hz, **nearest the sender's own player first**. That
is 3.5 KB/s each way per observer for traffic against 13 KB/s for the same
dozen cars at the player rate. A car outside the nearest eight is held at the
last transform every observer heard rather than streamed.

Two honest limitations in that, both of which step 5 is what fixes:

- "Nearest" means nearest **the sender**, because the sender does not know
  where anybody else is standing when it picks its eight. Two players in one
  street get nearly the same eight; two players far apart do not, and the far
  one's view of this machine's traffic is a set of parked cars. Choosing per
  observer is the server's job and it is not done.
- The fall-off is a cliff rather than a curve: eight cars at full rate and
  everything else at nothing. §2.1 asks for three bands and this has two.

### 2.2 The pools

GTA III's ped and vehicle pools are fixed. §1.3 is what keeps them from
overflowing, which makes it load-bearing rather than an optimisation: get the
counter rewriting wrong and the failure is the pool filling up, not a car too
many.

### 2.3 The streamer

`roadmap.md` §2.1: the streamer centres on one player. A replica far from the
local player has no model and no collision there. §5.3 already settles what
happens to a *player* in that position - a blip and nothing else - and ambient
entities need the same answer, which is probably "do not create them at all
beyond the radius, and create them when they come into it".

### 2.4 The AI

`protocol.md` §6, open question 1, and it stops being optional here. A replica
must not run its own AI, or every machine's copy of a pedestrian wanders off in
its own direction and the entity means nothing. The same question already
applies to remote players; this design needs it answered, and it is the
prerequisite rather than a follow-up.

---

## 3. Order of work

1. **Answer the AI question** (§2.4). Everything else replicates entities that
   would immediately disagree with each other without it.
2. **The `CWorld::Add` seam and the temporary-id handshake**, for peds only.
   One kind of entity, one direction, no generation changes: does a pedestrian
   this machine created appear on the other screen and stay where it is put?
3. ~~**The counter rewriting** (§1.3).~~ **Struck entirely**, measured rather
   than assumed - §1.3.1 for pedestrians, §1.3.2 for traffic. The engine
   counts replicas already in both cases, so steps 2 and 4 are shippable on
   their own. It has now been measured away twice and built zero times.
4. ~~**Cars**~~ **Done, 2026-09-22.** The same machinery, with two additions
   the design did not anticipate in the same shape:

   - The autopilot is **not** replicated. The design offered "replicated,
     re-derived, or not needed" and the answer is the third: the autopilot is
     the state that decides where a car is *going*, and a replica decides
     nothing - its transform comes off the wire. What the replica does get is
     `CCarCtrl::JoinCarWithRoadSystem`, which re-derives the route nodes the
     engine wants populated from the position alone. Free, local, and it
     cannot disagree with anybody.
   - A transform stream, which pedestrians did not need. An ambient ped is
     created where the session says and left there; that looks like a person
     standing still. A car replica left where it was created is a locked,
     undeletable roadblock across a junction on every screen but its owner's.
     So hosted cars carry `C_CarStates` - see §2.1 below for the rate.
5. **Rate by distance** (§2.1), measured against a real session.

Step 2 is the point of no return: after it the city's pedestrians are shared
and every divergence is a bug rather than a documented difference.

---

## 4. Addresses this needs

None of these are verified yet. They are named here so the work starts from a
list rather than a search, and every one has to be proved against `gta3.exe`
before it is used - `roadmap.md` §6 has the rule and the reason.

| What | Why |
|---|---|
| `CWorld::Add` | the one door into the world |
| `CPopulation::ManagePopulation` | where pedestrians get made |
| `CPopulation::ms_nNumCivMale` / `ms_nNumCivFemale` / `ms_nTotalCivPeds` | the counters to rewrite |
| `CCarCtrl::GenerateOneRandomCar` | where traffic gets made |
| `CCarCtrl::NumRandomCars` / `NumLawEnforcerCars` / `NumMissionCars` | the counters to rewrite |
| `CPed::m_nCreatedBy` | tells an ambient ped from a mission one |

re3 has all of these by name. It has been wrong about a retail constant four
times on this project, so it says where to look and the binary says what is
true.

---

## 5. A pedestrian dying

Everything above shares a pedestrian's *existence*: he is created, he is
streamed, he is taken away. What none of it shares is the one thing that
happens to him in between and that everybody watches happen.

Until this, a death reached an observer only as whatever animation happened
to be dominant in the next `C_PedStates` row - and that is not a death. The
stream carries the twelve peds nearest **the sender** (`MAX_PED_STATES`), a
corpse drops out of that twelve as soon as the shooter walks on, and
`ApplyAmbientPedState` refuses to drive anything into a replica that is
already dead while never being the thing that makes one dead. So you shoot a
pedestrian, he drops on your screen, and on every other screen he keeps
walking - until his host's engine reaps him a minute later and he vanishes
mid-stride.

### 5.1 Only the host can witness it

The same rule as the limb, for a stronger reason. Every replica is
`bBulletProof`, `bFireProof`, `bMeleeProof` and `bCollisionProof`, so an
observer's engine never reaches the death at all; and an ambient ped carries
no health on the wire, deliberately (`protocol.h`, `AmbientPedState`), so
nothing downstream could infer one either.

The witness is the host's own `CPed::SetDie` detour, which `game/combat.cpp`
has held since M4 for the local player's death animation. One address carries
one detour, so the ambient half is a call out of that hook into
`population.cpp`, which is the only file that can answer "is this a
pedestrian I am entitled to talk about". The filter is `BodyPartHook`'s,
verbatim: hosted, named, netId allocated. A ped killed inside the round trip
of its own naming is missed, which is the same rare and harmless miss the
limbs already accept - his own engine is about to reap him and despawn him
everywhere.

The test is the transition, not the call: `SetDie` returns without doing
anything for a ped that is already dying, so "it was called" would announce a
death every time something shot a corpse.

### 5.2 The observer calls `SetDie`, and takes him out of the car first

The engine decides the death. `CPed::SetDie` picks the stored state, clears
the tasks, zeroes the health, blends the fall and leaves the body where
`CPed::ProcessControl` plays it out - the same argument `BlowUpRemoteVehicle`
makes for going through the engine's own `BlowUpCar` instead of building a
wreck by hand.

**The trap is the `PED_DRIVING` arm**, and it is four instructions:

```
004D3848  mov  eax,[ebx+224h] / cmp eax,2Ch    m_nPedState == PED_DRIVING
004D3855  call 004D48E0                        CPed::IsPlayer
004D385C  test al,al / jnz                     the player is spared
004D3860  mov ebp,[ecx] / call [ebp+40h]       vtable +0x40,
                                   FlagToDestroyWhenNextProcessed
```

Every replica is a `CCivilianPed` and a traffic driver replica is seated, so
killing one in place hands it to the engine to delete and the pool handle
stops resolving a frame later. Nothing else in the function reaches that
call - a ped that is merely `bInVehicle` takes the other arm, which only
cancels its vehicle animation - so `m_nPedState == PED_DRIVING` is the exact
test, and `bInVehicle` would unseat peds that were never in danger.

So a seated replica is unseated
first, and the standing seat request is cancelled with it - otherwise
`UpdateAmbientPedSeats` puts the corpse back behind the wheel on the very
next frame. `Client::OnDeath` has done exactly this for a seated *player*
since M4; this is the ambient copy of the same rule.

The animation rides the packet, the way `C_Death` carries one for a player
and for the same reason: a headshot, a drowning and a car knocking someone
over are three different animations and no observer could work out which. It
goes through `PlanDeathAnim` before it reaches the engine, because the id
becomes a subscript into `ASSOCGRP_STD` with nothing checking it.

### 5.3 Recorded, then carried out

`S_PedDeath` sets `dead` on the row; `Client::UpdateRemoteAmbientPeds` does
the killing, on the first frame there is a ped to kill. That is not caution,
it is the ordinary case: a live death overtakes its own ped's spawn while the
model is still streaming, and a joiner's backfilled corpse has not even been
asked for when its `S_PedDeath` lands. "If there is a ped, kill it" would
lose the death in both.

It is retried rather than dropped for the same reason. `dead` is a standing
fact about the pedestrian and outlives any particular replica of him - one
lost to a full pool and rebuilt has to die again.

### 5.4 Ordering against the limbs, and why it does not actually matter

Three things can be said about a hosted pedestrian on `CH_EVENT`, and they go
out in the host's own order: **limb, death, despawn**.

`CPed::InflictDamage` takes the limb off first and calls `CPed::SetDie` at
its tail, so a headshot really does happen in that order on the machine that
decided it, and `CH_EVENT` is reliable and ordered, so sending it in that
order is all it takes to replay it. The despawn goes last because it ends
both: a death announced after its ped has been taken away finds nothing to
kill, and the ped pool reuses a slot the instant it frees up.

**The two commute, and that is measured rather than reasoned.** Both
functions were disassembled end to end against the retail image
(`SetDie` 0x004D37D0..0x004D3945, `RemoveBodyPart` 0x004EAEE0..0x004EB055)
and the sets of fields they touch are disjoint:

```
SetDie           +0x18C +0x224 +0x2C0 +0x314 +0x1D8 +0x4C +0x157 +0x228 +0x4EC
RemoveBodyPart   +0x1A4 (m_pFrames[node]) +0x156 +0x4F2, the RwFrame
                 visibility walk, and the particle system
```

Neither reads what the other writes. So a limb applied *before* a deferred
death - which is what happens when the death is waiting on a model and the
limb is not - ends in the same state as the reverse. The two flag bytes are
+0x157 and +0x156: adjacent, and not the same byte.

Getting the order wrong would therefore have been invisible, which is exactly
why it is written down. The order is kept because it is the host's, not
because the engine needs it.

### 5.5 The late joiner

Unlike a limb, a death **is** kept on the server. A corpse lies in the street
for the minute or so `CPopulation` takes to reap it, and a joiner inside that
minute would otherwise be handed a pedestrian standing up in a pool of
everyone else's blood.

`Session::AmbientPed` grows `alive` and `deathAnimId`, written only by the
ped's owner and only once per life, and `BuildBackfill` emits an `S_PedDeath`
after the `S_PedSpawn` for every dead one. Spawned and then killed, rather
than left out the way a wrecked ambient car is: `S_PedDeath` can describe a
corpse where `S_CarSpawn` cannot describe a burnt shell, and a body everybody
else walks around is not something a joiner should walk through.

### 5.6 What this does not do

- **A ped killed before the session named him is missed** (§5.1).
- **Who killed him is not carried.** `C_Death` has a `killerNetId` because a
  session keeps score for players. Nothing keeps score for pedestrians, and
  the byte would buy nothing but a line in a log.
- **A replica that dies on its own is still not reported anywhere.** The
  local engine's `m_fHealth <= 1.0f` auto-`SetDie` or a stray explosion can
  still put one there, and `ApplyAmbientPedState` has refused to drive a walk
  into it since step 6. That is an observer disagreeing with its host about
  one pedestrian and it is not new; what is new is that the common cause of
  it - the host having killed him - no longer produces it.
