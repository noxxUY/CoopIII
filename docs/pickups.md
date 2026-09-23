# Pickups

Everything the player walks over and picks up: weapons and ammo on the ground,
health, armour, the adrenaline pill, the bribe, the camera, the money a dead
ped drops, the 100 hidden packages, and the 20 rampages.

Investigated 2026-09-22 against the retail `gta3.exe` (v1.0, MD5
`85414BF9EB414D00AD81062360F0DB1F`). Every address quoted here is in
`client/src/game/addresses.h` under `---- pickups ----` with the disassembly
that proves it. Nothing in this document was taken from `reference/re3` as a
fact; re3 was used as a map to know which function to disassemble next, and
where it disagreed with the binary the binary won. `reference/coopandreas` was
not read for this work at all - it is San Andreas, where none of this survives
the trip, and it is GPL-3.0 while CoopIII is MIT.

---

## 1. The finding that shapes everything else

**Every machine already creates every script pickup, at the same coordinates,
independently, and nothing had to be built for that.** It holds, and it was
checked rather than assumed:

- Nothing in `client/` touches `CTheScripts::Process` or
  `CRunningScript::ProcessCommands`. Suppressing the main script on clients is
  M5 Tier 2 and it is not built, so every machine runs the whole of `main.scm`.
- The campaign script creates pickups **448 times**, through two opcodes:
  `0213 CREATE_PICKUP` (531) and `032B CREATE_WEAPON_PICKUP` (811). Counted
  over `reference/scm`: 116 in `pickups.sc`, 60 in `rampage.sc`, 30 in
  `rewards.sc`, the rest scattered through missions. Plus `02EC
  PUT_HIDDEN_PACKAGE_AT` a hundred times in `packages.sc`, and `02ED
  SET_TOTAL_HIDDEN_PACKAGES_TO 100`.
- Every one of those is a **literal coordinate in the script**. There is no
  randomness anywhere on the creation path: `CPickups::GenerateNewOne`
  (`0x004304B0`) never calls the RNG, and the script hands it constants.

So the spawning half of M4's pickup line is solved by accident, exactly as
suspected. Two machines running the same `main.scm` put a health pickup at
`1147.75 -597.0625 14.875` and both mean the same health pickup.

**What it does not cover**, and these are genuinely different problems:

- **Ped drops.** `CPed::CreateDeadPedMoney` and
  `CPed::CreateDeadPedWeaponPickups` both call `GenerateNewOne` with positions
  and amounts straight out of `CGeneral::GetRandomNumber()`. The RNG is not
  synced and never will be. Worse, ambient peds are not synced at all
  (`protocol.md` §3), so the ped whose death made the pickup usually exists on
  one machine only. A drop is a **spawn** problem - the owner has to tell
  everyone else to create one - and it is out of scope here.

  > **Two corrections, 2026-09-22, and §10 is the write-up.**
  > `CreateDeadPedWeaponPickups` **never calls the RNG at all** - its scatter
  > is `i * 1.75f` and its only line-of-sight check reads buildings. Only the
  > money half is random. And ambient peds *are* synced now
  > (`docs/population.md`), which turned out to settle the ownership question
  > by itself rather than complicate it: a replica is proof against every
  > damage cause and is `MISSION_CHAR` with an empty inventory, so a drop is
  > already made exactly once in the session, on the machine that hosts the
  > ped. The conclusion - that it is a spawn problem and needs a packet -
  > survives both corrections, for a different reason than the one given here.
- **Mission pickups**, once M5 lands and the script stops running on clients.
  At that point *all* of this inverts and the host has to replicate creation
  too. The design below is written so that it survives that: the identity is a
  position and a model, not a slot, so it does not care who created the pickup.

**And it is exclusivity that is missing, which is what the roadmap already
said.** Today, if two people played:

- A walks over a shotgun. A's engine gives A the shotgun and removes it *on A's
  machine*. B's copy is untouched, still spinning, and B can take the same
  shotgun a minute later.
- If A and B stand on it in the same frame, both engines award it and both get
  a shotgun.
- A hidden package would be worth 100 packages to a group of eight, because
  each machine counts its own.

---

## 2. How the engine stores them

```
CPickups::aPickUps           0x00878C98    CPickup[336], stride 0x1C
CPickups::aPickUpsCollected  0x0087C538    int32[20]
CPickups::CollectedPickUpIndex 0x0095CC8A  int16
```

```
struct CPickup {            // 0x1C
    uint8  m_eType;         // +0x00   ePickupType, 0 = PICKUP_NONE = free slot
    uint8  m_bRemoved;      // +0x01
    uint16 m_nQuantity;     // +0x02   ammo, or money
    CObject *m_pObject;     // +0x04   the thing you see; nil while removed
    uint32 m_nTimer;        // +0x08   absolute CTimer::m_snTimeInMilliseconds
    int16  m_eModelIndex;   // +0x0C
    uint16 m_nIndex;        // +0x0E   generation counter, 1..0xFFFD
    CVector m_vecPos;       // +0x10
};
```

### The bound, with four independent witnesses

The instruction was to read the size off `gta3.exe` and not off re3, because
four re3 constants have already been refuted on this project. Four separate
functions carry it and all four agree on **336 slots of 0x1C bytes**, with the
first **320** reserved for general use:

| Function | Address | What it says |
|---|---|---|
| `CPickups::GenerateNewOne` | `0x004304B0` | `mov ebp,0x24A4 / mov ebx,0x14F` then `sub ebp,0x1C / dec ebx` - a descending scan from slot 335 at byte offset 9380, i.e. `335 * 28`. Its other three loops are `cmp ebx,0x140` (320) stepping `0x1C`, and the final guard is `cmp ebx,0x150` (336). |
| `CPickups::Update` | `0x004303D0` | second loop starts at `esi = 0x87AF98`, `ebx = 0x140`. `0x87AF98 - 0x878C98 = 0x2300 = 8960 = 320 * 28`. So the general range really is `[0,320)` and the array base really is `0x878C98`. |
| `CPickups::RemoveAllFloatingPickups` | `0x004307FF` | `add esi,0x1C` / `cmp ebp,0x150 / jl`. |
| `CPickups::Save` | `0x00433E40` | writes `0x24C0` (= 9408 = 336 × 28) as the block size, then copies **every field in order with its width** - `byte +0`, `byte +1`, `word +2`, `dword +4`, `dword +8`, `word +0xC`, `word +0xE`. That is the whole struct layout again, from a function that has nothing to do with how it was first found. |

`aPickUpsCollected` gets three of its own: `AddToCollectedPickupsArray`
(`0x00433D60`) and `IsPickUpPickedUp` (`0x00430770`) both bound at `0x14` = 20
and index `0x0087C538` by `*4`, and `CollectedPickUpIndex` at `0x0095CC8A` is
compared against `0x14` and wrapped to 0 in the first.

re3 says 336, 320 and 20 too. That is a coincidence that was checked, not a
constant that was trusted - and it is worth saying which of the two it is,
because the last several bugs on this project were an re3 number that did not
match retail.

### The handle is a slot plus a generation, like the blips

`CPickups::GetNewUniquePickupIndex` (`0x00433DB0`):

```
    if (aPickUps[slot].m_nIndex >= 0xFFFE) m_nIndex = 1; else ++m_nIndex;
    return slot | (m_nIndex << 16);
```

and `GetActualPickupIndex` (`0x00433DF0`) refuses the handle when the top half
no longer matches the slot's `m_nIndex`. Same shape as the radar's blip
handles. It is the game's own stale-handle guard and it is *per process*.

**It is not a network identity, and this is the trap.** The slot number is
assigned by a first-free scan over `[0,320)`, and ped drops allocate out of
that same range. The moment one player kills a pedestrian in traffic, that
machine's free-slot cursor has moved and every later script pickup lands in a
different slot than it does on everyone else's machine. Slot agreement holds
only for the deterministic prefix created at script start, and only until the
first drop. Anything that puts a raw slot index on the wire is wrong within the
first minute of play.

---

## 3. What collection looks like at the seam

**`CPickup::Update(CPlayerPed *player, CVehicle *vehicle, int playerId)` at
`0x00430860` is the whole seam.** It detects the touch, decides whether the
pickup may be taken, applies the reward and removes the object - all in one
`__thiscall`, returning `true` when it collected something.

Its only two callers are both inside `CPickups::Update` (`0x004303D0`) -
`0x0043042F` and `0x0043047F`, confirmed by scanning the entire image for
`E8`/`E9` rel32 targets. So **a detour on `CPickups::Update` covers every path
by which a pickup can be collected in this build.** There is no second route.

`CPickups::Update` is called once a frame from `CGame::Process` at
`0x0048C98E`, between `gAccidentManager.Update()` and `CGarages::Update()`, and
it only scans a sixth of the general range per frame (`mul 0xAAAAAAAB / shr 2`
against `CTimer::m_FrameCounter`, `imul 0x35` = 53 slots). Retail has the same
"318 out of 320" arithmetic bug re3 documents. Slots 320..335 are scanned every
frame.

### Inside `CPickup::Update`

```
  +0x12  if (m_bRemoved)  -> the respawn branch          (0x004313D5)
  +0x20  if (!m_pObject)  -> return false                (0x00430880)
         if (type is 8..13) -> the mine branch, switch at 0x005EE1D8
         otherwise: the touch test                       (0x00430C58)
         then CanBePickedUp, inlined                     (0x00430E19)
         then CPad::GetPad(0)->StartShake(120,100)
         then switch (m_eType - 1), table at 0x005EE1A0   (0x00430ED3)
```

**The touch test is a place, and that is the whole argument in §4.** For a
`BRIBE` or a `CAMERA` in a vehicle it is
`CVehicle::IsSphereTouchingVehicle(pos, 2.0)`; on foot it is
`|dz| < 2.0 && dx*dx + dy*dy < 1.8`. Nothing about it is a ray, a trace or a
claim on a remote entity. It is a distance from the local player to a fixed
world position.

**`CanBePickedUp` refuses four cases**, read off `0x00430E19`: `BODYARMOUR`
when `m_fArmour` (`CPed+0x2C4`) > 99.5, `HEALTH` when `m_fHealth`
(`CPed+0x2C0`) > 99.5, `BRIBE` when the wanted level (`m_pWanted` at
`CPed+0x53C`, level at `+0x18`) is zero, and `KILLFRENZY` when
`CTheScripts::IsPlayerOnAMission() || CDarkel::FrenzyOnGoing() ||
!CGame::nastyGame`.

**The award switch is by type, and the paths really do differ.** The jump table
at `0x005EE1A0` is indexed by `m_eType - 1`:

| type | | handler | what it does |
|---|---|---|---|
| 1 | `IN_SHOP` | `0x00430EDA` | checks `CostOfWeapon` (`0x005ED924`), debits `CPlayerInfo::m_nMoney` (`+0xAC`), `GiveWeapon` with `AmmoForWeapon` (`0x005ED8D4`), timer + 5000 |
| 2 | `ON_STREET` | `0x00430FEB` | `GiveWeapon` with `m_nQuantity`, timer + 30000 |
| 3 | `ONCE` | `0x0043117A` | gives, then `Remove()` - gone for good |
| 4 | `ONCE_TIMEOUT` | `0x0043117A` | same, and it also expires by itself at `0x00431382` |
| 5 | `COLLECTABLE1` | `0x00431260` | `++m_nCollectedPackages` (`+0xB4`), `+= 1000` money, and at `m_nTotalPackages` (`+0xB8`) a message and `+= 1000000` |
| 6 | `IN_SHOP_OUT_OF_STOCK` | - | nothing |
| 7 | `MONEY` | `0x00431321` | `m_nMoney += m_nQuantity` |
| 15 | `ON_STREET_SLOW` | `0x00430FEB` | shares with `ON_STREET`; timer + 300000 for a `BRIBE`, + 720000 for anything else |

Everything that is not a weapon goes through
`CPickups::GivePlayerGoodiesWithPickUpMI` (`0x004339F0`) first, which is a
straight chain of model-index comparisons: adrenaline (sets
`m_bAdrenalineActive` at `CPed+0x57C` and a 20 s deadline at `+0x580`, and
swaps `+0x54C` into `+0x548`), body armour (`m_fArmour = 100.0f`), the info
icon, health (`m_fHealth = 100.0f`), the bonus icon, the bribe (one wanted star
off). Those model indices are runtime globals at `0x005F5B10` upwards - see
`addresses.h` for how each one is identified by what the code does with it
rather than by a name.

**So: same function for a weapon, for money and for a hidden package, and
different arms of one switch inside it.** There is no separate collection path
to find, and no path that does not end in "the object is removed and
`m_bRemoved` is set".

### The one thing a hook must not forget

When `CPickup::Update` returns true, `CPickups::Update` calls
`AddToCollectedPickupsArray(slot)`, which pushes `slot | (m_nIndex << 16)` into
a 20-deep ring at `0x0087C538`. That ring is the *only* thing
`HAS_PICKUP_BEEN_COLLECTED` (opcode 532, handler `0x00443393` →
`CPickups::IsPickUpPickedUp` `0x00430770`) reads, and `IsPickUpPickedUp`
**consumes** the entry as it matches it.

`rampage.sc` and `rewards.sc` poll that opcode. So a pickup collected by
somebody else has to be pushed into the observer's ring too, or the observer's
rampage never starts and their reward thread never fires. That is not a detail;
it is the mechanism that makes §6 work.

---

## 4. Who decides

CoopIII's rule everywhere else is that the machine entitled to decide is the
one that owns the thing. A pickup is owned by nobody, so §5.7's question has to
be asked again: is a pickup's authority a **ray** or a **place**?

**Detection is a place, and it is answered about yourself.** "Am I standing on
this pickup" is `|dz| < 2 && dx² + dy² < 1.8` against my own position this
frame, with nothing interpolated in it, exactly like "am I standing in this
fire". No observer is entitled to answer it about somebody else and none needs
to: every machine has its own copy of the pickup at the same coordinates.

**But the award is not a place, because the resource is exclusive.** Fire can
burn two people at once and the right answer for each is independent. A shotgun
cannot be picked up twice. So the fire argument gets you to the doorstep and
stops: two players standing on the same weapon both correctly answer "yes, I am
standing on it", and somebody still has to say which of them gets it. Only the
server can, because only the server sees both claims.

So the split is:

> **The client detects, the server arbitrates, the engine awards.**

and the ordering matters more than any of the three: the arbitration has to
come **before** the award, not after it, because a pickup reward is not
revocable in any honest way. Ammo already fired, health already spent in a
fight, a bribe that already cleared a wanted star, an adrenaline rush already
half over - there is no un-giving any of that. A design that awards
optimistically and rolls back on a denial is a design that will visibly cheat
somebody.

### Default-blocked, and the engine never sees a pickup it may not award

The lever is `m_pObject`. `CPickup::Update` reads it at `0x00430880` and
returns `false` immediately when it is nil, **after** the `m_bRemoved` respawn
branch at `0x00430872` and **before** the mine switch, the touch test and the
whole award switch. So:

> around the call to `CPickups::Update`, stash and nil `m_pObject` for every
> pickup CoopIII has not been granted; call the trampoline; put them back.

That blocks exactly the award and nothing else. In particular it does **not**
block the respawn branch, which is above it, so pickups still come back on
their own. It does not touch `m_eType`, so `GenerateNewOne`'s free-slot scan
still sees an occupied slot - which matters, because a mine detonating inside
`CPickups::Update` can kill a ped and reach `CreateDeadPedMoney` →
`GenerateNewOne` reentrantly, and a design that blanked `m_eType` instead would
hand out a live slot in that window. It does not touch the `CObject`, so the
pickup still renders and still spins: `CPickups::DoPickUpEffects` runs off the
object's `bIsPickup` flag from the render path, not from the pickup table.

Mine types (8..13) are left unblocked and entirely local. They are script-only,
retail III barely uses them, and their branch is about arming and exploding
rather than about giving anybody anything.

### The claim is made on approach, not on contact

If the claim were sent when the player touched the pickup, the grant would
arrive a round trip later and the player would stand on a pickup that does
nothing for 50 ms. Instead CoopIII claims when the local player comes within
**4 m** of a collectable pickup - comfortably outside the engine's own 1.34 m
radius, so at walking speed the grant is in hand half a second before contact.

That 4 m is CoopIII's own network heuristic, not a reimplementation of the
engine's test. It decides *when to ask*, never *whether a pickup was
collected*; getting it wrong costs a wasted packet or a late grant, never a
wrong award. The engine's own test, which has to be exactly right, is still the
engine's and is still run unmodified.

**There is no race left.** The engine cannot award a pickup CoopIII has not
unblocked, and CoopIII does not unblock without a grant. Two players sprinting
at the same armour from opposite sides produce two claims; the server grants
one and denies the other; the loser's engine never sees an object there at all.
The cost is a pickup that occasionally refuses to be collected for one round
trip. The benefit is that nothing ever has to be taken back.

### A grant is a reservation, and that distinction is not a detail

Claiming on approach means **claiming things you turn out not to want**. A
player at full health walks past a health pickup; somebody with no wanted level
walks past a bribe; anybody at all walks down a street. If a grant were treated
as a collection, walking around Liberty City would quietly delete every pickup
on your route from everyone else's world.

So the exchange has four steps, not two:

```
    client  ---- C_PickupClaim ------>  server      claim, at 4 m
    client  <--- S_PickupGrant -------  server      reserved for you, nobody else told
    (the engine's own CPickups::Update runs on an unblocked pickup)
    client  ---- C_PickupCollected -->  server      it took it
    others  <--- S_PickupTaken -------  server      remove your copy
```

and the fourth step has an alternative: when the player leaves the 4 m radius
without the engine having taken the pickup, the client sends
`C_PickupRelease`, the reservation ends, and the pickup is available to
everybody again having never been removed from anybody.

**The collection is detected, not decided.** CoopIII holds the reservation, the
engine runs its own touch test, its own `CanBePickedUp` and its own award
switch on an unblocked pickup, and the slot being empty afterwards is what gets
reported. So nothing here has to predict what `CPickup::Update` would have
done - it reads what it did. That matters most for the four refusals
`CanBePickedUp` makes (armour above 99.5, health above 99.5, a bribe with no
wanted level, a killfrenzy during a mission or a running frenzy): CoopIII never
reimplements any of them, it just notices that the engine declined and gives
the reservation back.

A reservation nobody gives back would be a pickup nobody can have, so the
server expires one after **15 seconds**. That only fires when the holder never
says anything again - a crashed client, or a connection lost between the grant
and the release - and a player leaving drops every reservation they hold while
keeping everything they actually collected.

### What the observers do

`S_PickupTaken` goes to everybody **except** the collector, whose own engine
has already removed their copy - that removal is what produced the message.
Every other machine replays the engine's own removal tail on its own copy - `CWorld::Remove(m_pObject)`, the deleting
destructor through vtable slot 0 with `flags = 1`, `m_pObject = nil`,
`m_bRemoved = 1`, and then the type's own ending: `m_nTimer` set for the
respawning types, `m_eType = PICKUP_NONE` for `ONCE`, `COLLECTABLE1` and
`MONEY`. That is copied instruction for instruction from the tails at
`0x00430F9B` (shop), `0x004312F6` (package) and `0x0043134F` (money), which are
all the same six writes in a different order, and it is transcribed in
`client/src/game/pickup.cpp` with the address of the tail it came from beside
it.

And then `AddToCollectedPickupsArray(slot)` on the observer too, for the reason
in §3.

Observers deliberately do **not** replay the reward. Health, armour, money and
weapons all belong to one player, and that player's own machine already applied
them through the engine's own switch. The single exception is the hidden
package counter, and §6 is why.

---

## 5. Respawn

Weapon and health pickups come back: `+30 s` for `ON_STREET`, `+720 s` for
`ON_STREET_SLOW` (`+300 s` for a bribe), `+5 s` for a shop. The engine writes
`m_nTimer = CTimer::m_snTimeInMilliseconds + k` - an **absolute** stamp on a
**local** clock. Two machines' `m_snTimeInMilliseconds` do not agree: they
start at different moments and pause independently (`CoopIII` already writes
`m_UserPause` every frame, `game/pause.h`). So the stamp cannot go on the wire
and neither can the deadline.

The duration can, because it is a constant per type. And the duration is all
that is needed:

> **The server owns availability; each machine's clock owns appearance.**

The server records, per pickup, when it granted it and how long that type stays
gone. A claim that arrives before that window expires is denied. Each client is
free to let its own engine bring the object back whenever its local rule says
so - the respawn branch also insists the local player is more than 10 m away
(`dist > 100.0f` at `0x004313D5`), which is a *local* condition and will fire at
genuinely different moments on different machines.

That divergence is now harmless, and that is the point of putting availability
on the server rather than in the timer. A copy that comes back early is simply
blocked until the server says the window is over. A copy that comes back late
means its owner cannot claim yet - and because a claim is only made when the
local object exists, CoopIII never holds a grant it cannot consume. If the
object does disappear between claim and grant, the client sends
`C_PickupRelease` and the reservation ends immediately; that path is
logged once per occurrence, not per frame.

The two machines therefore agree about *when a pickup may be taken* exactly,
and about *when it is visible again* to within one round trip plus one 10 m
check. A pickup that reappears 80 ms apart on two screens is not a bug anybody
can see.

---

## 6. Rampages and hidden packages

These are progress, not items, so they need a gameplay decision and not just a
mechanism. Both are recorded as settled decisions in `roadmap.md` §5.10 and
§5.11; the reasoning is here.

### Hidden packages - shared

**A package collected by one player counts for everybody, and is gone from
everybody's world.** Server option `HiddenPackages = shared | perplayer`,
default `shared`; `perplayer` is named so the option exists, and is not
implemented in M4.

Why:

- **The engine has one counter.** `m_nCollectedPackages` is `CPlayerInfo+0xB4`
  and there is exactly one `CPlayerInfo` (`roadmap.md` §2.3). Remote players are
  `CPed`s. Per-player packages would be CoopIII's own state, and then
  `rewards.sc` - which polls the count to unlock the weapons at the hideout -
  would be reading a number that means something different on every machine.
- **The array cannot hold it.** 100 packages per player against 320 general
  slots is three players, and the object pool pays for every one of them.
- **§5.5, fidelity wins.** The single-player experience of hidden packages is
  that the map empties as you clear it. A shared world where seven players walk
  past a package that only the eighth can see is the version that feels wrong.
- **It costs one line, because the script does the sharing.** Every machine
  runs `packages.sc` and `rewards.sc` already. If the observer increments its
  own `m_nCollectedPackages` and pushes the collection into
  `aPickUpsCollected`, every script-visible consequence - the "package 34 of
  100" message, the reward weapons appearing at the hideout, the million at 100
  - happens on every machine, identically, with no packet for any of it. This
  is the pickup version of the fire finding: replicate the cause into each
  engine and the engine produces the effects.

The price, stated plainly: eight players finish the packages in an eighth of
the time, and the 100% grind - one of the longest solo activities in III -
collapses. That is the right price to pay for a shared map. Groups who disagree
get the option.

### Rampages - shared, one at a time

**One rampage runs for the whole session; everyone's kills count toward it;
passing or failing it does so for everybody.**

Why:

- **The engine gives no choice.** A rampage is a `KILLFRENZY` pickup that starts
  `CDarkel`, and `CDarkel` is a singleton with one kill count, one timer and one
  HUD counter. `CanBePickedUp` already refuses a second frenzy pickup while one
  is running (`CDarkel::FrenzyOnGoing()`, `0x00420E60`). Per-player rampages
  would mean CoopIII owning the frenzy state, the target, the clock and the HUD
  for every player - that is not a pickup feature, it is a reimplementation of
  `CDarkel`, and it belongs nowhere near M4.
- **It is also the better game.** "Kill 20 Diablos in two minutes" is the single
  most obviously co-operative thing in GTA III. Splitting it into eight private
  rampages happening in the same street is worse in every way.
- **Same free mechanism as packages.** `rampage.sc` polls
  `HAS_PICKUP_BEEN_COLLECTED`. Push the collection into every machine's
  `aPickUpsCollected` and every machine's rampage thread starts the same frenzy
  in the same frame, off its own script, with its own HUD, its own timer and its
  own failure condition.

The price: the difficulty is not rebalanced, so a four-player rampage is
trivial. Accepted for M4. If it ever matters, the fix is a server-side
multiplier on the kill target - which needs the script intercepted, i.e. M5 -
and **not** a per-player split.

**One thing is explicitly not in M4:** remote players' kills do not yet count
toward `CDarkel`. The frenzy starts for everyone and each machine counts only
its own player's kills, so the target is reached on whichever machine did the
killing and the others' counters lag. Closing it is small - M3 already carries
death events (`protocol.md` §1.10) - but it is a `CDarkel` feature, not a
pickup one, and it is listed in `roadmap.md` M4 as its own line.

---

## 7. Identity on the wire

Not the slot - §2 explains why. A pickup is named by **what it is and where it
is**:

```
struct PickupIdent {   // 16 bytes
    float   x, y, z;
    int16_t modelIndex;
    uint8_t type;       // ePickupType, so the server knows the respawn window
    uint8_t pad;
};
```

Both ends resolve it by nearest-match within **0.25 m** of the same model, over
their own `aPickUps`. Tolerance rather than an exact key, deliberately: script
pickups really are bit-identical on every machine, but a ped drop's z comes out
of `CWorld::FindGroundZFor3DCoord` and the design should not depend on a float
comparison being exact across two processes for something it does not need to.

The server keys its own table by the same model plus the position quantised to
0.25 m, which is a decision one machine makes consistently with itself and so
has none of that problem.

### The script really does reuse a coordinate, and it was checked

312 script pickup coordinates were parsed out of `reference/scm` and compared
pairwise within each model. **29 pairs of the same model are within 2 m of each
other, and all 29 are at a distance of exactly zero** - the same literal
coordinate written twice. They fall into two groups and neither aliases,
because in both the two are never live at the same time:

- **The Portland Ammu-Nation counter.** `pickups.sc:184` and `:191` create the
  same `COLT45` at `1068.5 -400.75 15.1875`, one `PICKUP_IN_SHOP` and one
  `PICKUP_OUT_OF_STOCK`, in the two arms of an `if $MASTERDEBUG` - mutually
  exclusive by construction. (`help.sc:52` is the third writer of that
  coordinate and is the same pickup again.)
- **The rampages.** Every one of the 20 rampage pickups is written twice in
  `rampage.sc`, once at `:61`-`:71` for the initial placement and once in its
  own failure handler - and every failure handler runs `0215 destroy_pickup`
  **before** re-creating it.

The 100 hidden packages are all at least **30.9 m** apart, so they are not
close to the problem at all.

So a live-at-once collision does not occur in the retail script. A ped dropping
two weapons of the same type onto the same spot is still conceivable, so the
client logs the collision once and refuses to claim either rather than claiming
the wrong one.

### A key is reused over time, and the server has to let go of it

`destroy_pickup` / `create_pickup` at the same coordinate means the *same key*
can name a different pickup instance later. A rampage failed twice is back at
its original coordinate, with the server still holding a record that says a
`PICKUP_ONCE` there was taken and never comes back.

So a taken-record is a lock, not a tombstone. `C_PickupRelease` carries the
second meaning: **a client whose local engine has a live object at a key that
client had previously removed on an `S_PickupTaken` sends it once**, and the
server drops the record. The re-created pickup then goes through a fresh claim
like any other, so exclusivity holds across the reuse. The client sees it for
nothing - it is already walking its own pickup table every frame for the 4 m
proximity test.

**What it watches is a list of idents, not a flag on a slot**, and that is not
a stylistic choice. `GenerateNewOne` hands out the first free slot, so a
re-created pickup almost never lands in the slot it came out of - a per-slot
flag would have missed every case it exists for.
`client/src/game/pickup.cpp` keeps sixteen of them, and only for the types with
no respawn window, since the rest are let go by the server's own clock. The
usual cost of the check is one integer compare for the whole pass, because the
list is usually empty.

---

## 8. The wire

`PROTOCOL_VERSION` has to move for this. **Opcodes `0x80`-`0x8F` are reserved
for pickups**; this uses six of them and the layouts are frozen by
`static_assert` in `sdk/include/coopiii/protocol.h`, as everything else here is.

| opcode | name | to | body |
|---|---|---|---|
| `0x80` | `C_PickupClaim` | server | `PickupIdent` |
| `0x81` | `S_PickupTaken` | everyone **but** the collector | `playerId` + `PickupIdent` |
| `0x82` | `S_PickupDenied` | the loser alone | `PickupIdent` |
| `0x83` | `C_PickupRelease` | server | `PickupIdent` |
| `0x84` | `S_PickupGrant` | the claimant alone | `PickupIdent` |
| `0x85` | `C_PickupCollected` | server | `PickupIdent` |

`0x86`-`0x8F` stay reserved for the drop replication §1 defers. **Two of them
are now spent** - `C_PickupDrop` / `S_PickupDrop`, §10.4 - and `0x88`-`0x8F`
remain.

All six are reliable. There is no snapshot component and there must not be one:
a pickup is not continuous state, it is a small number of discrete events, and
an unreliable channel for "who got the shotgun" is the same mistake
`protocol.md` §2.8 already made once with `driverPlayerId`.

A late joiner is told about pickups that are currently taken, out of the
server's own table, as part of the existing backfill - otherwise they arrive to
a world full of packages eight people already collected.

---

## 9. What could not be confirmed without a running game

Everything above is proved against the binary or is design. None of it has been
in front of GTA III. Specifically:

1. **That nilling `m_pObject` around `CPickups::Update` is invisible.** The
   argument is that the only reader inside that call is the early-out at
   `0x00430880`, and that rendering goes through the `CObject` rather than the
   pickup table. It has not been watched.
2. **That the respawn branch really is unaffected**, i.e. that blocked pickups
   still come back. The branch is above the nil check, so it should be; watch a
   blocked health pickup over a full 720 s cycle.
3. **Whether 4 m is far enough.** Sprinting, in a car through a bribe, or
   falling onto a rooftop pickup from above are the three cases where contact
   could beat the grant. If it shows up, the fix is a larger radius or a
   velocity term, not a change to the arbitration.
4. **How much claim traffic 4 m actually produces.** A player running down a
   street of health pickups claims and releases each one in turn, and a group
   standing together in Ammu-Nation claims the same counter repeatedly. Every
   one is a reliable packet. If it turns out to matter, the lever is a
   per-slot cooldown after a release, not a smaller radius.
5. **The `AddToCollectedPickupsArray` replay actually starting a rampage on an
   observer.** This is the load-bearing claim of §6 and the only way to test it
   is two machines and a rampage pickup.
6. **That the observer's package increment keeps `rewards.sc` in step.** Same
   test, at 10 packages, which is the first reward tier.
7. **Reentrancy.** The mine → explosion → `CreateDeadPedMoney` →
   `GenerateNewOne` path inside a blocked `CPickups::Update` is reasoned about
   in §4 and has never been executed. Mines are script-only and rare, which is
   why they are left unblocked, but it is the sharpest edge in the design.

---

## 10. Ped drops - what a dead pedestrian leaves behind

Investigated and built 2026-09-22, immediately after the above. This is the
one case §1 deferred, and the deferral's reasoning has not survived contact
with the binary intact. Three of the four things §1 said about it were right
and one was wrong, and the wrong one is the interesting one.

### 10.1 What a drop actually is, in the retail image

`CPed::SetDead` (`0x004D3970`) ends:

```
004D39F2  call 004A1150          FindPlayerPed
004D39F7  cmp  ebx,eax
004D39F9  je   004D3A09          the local player drops nothing
004D39FB  call 00433660          CreateDeadPedWeaponPickups
004D3A04  call 00433490          CreateDeadPedMoney
```

A whole-image scan for `E8`/`E9` rel32 targets equal to either address finds
**exactly one reference each**, and both are those two call sites. So two
detours cover every ped drop in the game, and there is no second route.

**`CPed::CreateDeadPedMoney` (`0x00433490`) is the random one.** Four gates,
then five RNG draws:

| gate | meaning |
|---|---|
| `cmp byte [005F4DD4],0` | `CGame::nastyGame`, or nothing at all |
| `movsx edi,word [ebx+5Ch]`, then `==1`, `[2..5]`, `==6` | model index in `MI_COP..MI_FIREMAN` drops nothing |
| `cmp byte [ebx+160h],2` | `CharCreatedBy == MISSION_CHAR` drops nothing |
| `cmp byte [ebx+314h],0` | `bInVehicle` drops nothing |

then `money = GetRandomNumber() % 60` (the `88888889h`/`sar 5`/`imul 3Ch`
pair), `< 10` gives nothing, `== 43` becomes **700**, `pickupCount =
money/40 + 1`, `moneyPerPickup = money/pickupCount`, and per pickup two more
draws for `1.5 * Sin/Cos((rnd % 256) * PI/128)`, a `FindGroundZFor3DCoord`
(`0x004B3AE0`) `+ 0.5f`, and one more for `+ (rnd & 7)` on the amount, then
`GenerateNewOne(pos, MI_MONEY, PICKUP_MONEY, amount)`.

So a money drop is one or two pickups almost always, and **eighteen** on the
one-in-sixty `money == 43` arm. None of it is reproducible on a second
machine.

**`CPed::CreateDeadPedWeaponPickups` (`0x00433660`) never calls the RNG
once.** That is the thing §1 got wrong, and it was checked rather than
assumed: the whole function was disassembled and scanned for rel32 calls to
`CGeneral::GetRandomNumber` (`0x005A41D0`) and there are none. Its only calls
are the FPU range-reduction helper, `FindGroundZFor3DCoord`,
`GetIsLineOfSightClear`, `GenerateNewOne_WeaponType` and `ClearWeapons`.

- the scatter is `angleToPed = i * 1.75f` - an index, not a draw, and
  `1.75 * k` is never a multiple of 2*pi for integer `k`, so no two of the
  thirteen slots can land on the same spot;
- the retry is `GetIsLineOfSightClear(edge, pedPos, true, 0,0,0,0,0,0)` at
  `0x004338C6` - **buildings only**, every dynamic flag zero, so it reads
  nothing but static collision and answers the same everywhere;
- the amount is `Min(m_nAmmoTotal, AmmoForWeapon_OnStreet[weapon])`, a table
  at `0x005ED8FC`;
- the loop is `cmp bp,0Dh`, thirteen inventory slots, `+0x18` each;
- the type is `PICKUP_ONCE_TIMEOUT`, and the tail is `CPed::ClearWeapons`
  (`0x004CFB70`).

A weapon drop is therefore a pure function of the ped's position and its
inventory. Worth knowing, and **not** what the design rests on - see §10.3.

Both types are short-lived, which turns out to decide more than it looks.
`GenerateNewOne` stamps an absolute expiry the moment it makes one:
`+ 4E20h` = **20 s** for `PICKUP_ONCE_TIMEOUT` (`0x004305A5`) and `+ 7530h` =
**30 s** for `PICKUP_MONEY` (`0x004305BB`).

### 10.2 The ambient ped sync changed the answer, and not in the direction the question expected

The brief asked what happens today when a remote player kills a hosted ambient
ped, because the answer decides where the drop is decided. The answer is that
**it cannot happen**. `game/population.cpp`'s replica recipe sets
`bBulletProof`, `bFireProof`, `bCollisionProof`, `bMeleeProof` and
`bExplosionProof` on every ambient replica, for the same reason a remote
player's ped gets them: an observer does not decide damage. So a pedestrian
dies on the machine that hosts it or not at all, and nothing about that death
travels except the despawn that already exists - `AmbientPedState` carries no
health by design (`protocol.h`).

Follow that through the two creators and the whole problem dissolves:

| ped, on this machine | money | weapons |
|---|---|---|
| local player | skipped by `SetDead`'s `FindPlayerPed` test | same |
| ambient ped this machine hosts | **created** - `RANDOM_CHAR`, real inventory | **created** |
| ambient replica of somebody else's | refused: `CharCreatedBy == MISSION_CHAR` | nothing: the replica was never given a weapon |
| remote player's ped | refused: `MISSION_CHAR` | **created, and this is a bug** |

So for the case the brief was actually about - an ambient pedestrian -
**the drop is already made exactly once in the session, on the machine that
owns the ped, and there is nothing to suppress.** The ownership rule holds by
construction rather than by a lock, and it holds through two independent
gates: the engine's own `MISSION_CHAR` test, and an empty inventory. That is a
seam, not a race: the non-deciding machines do not lose a race, they never
reach the code.

What was missing is only that nobody else ever heard about it. A drop exists
on one screen and not the others, and the exclusivity seam cannot help,
because it resolves an ident against the local `aPickUps` and an observer has
nothing there to resolve.

**The fourth row is a live bug and it predates this work.** An observer's copy
of a player *is* given a weapon - `game/ped.cpp`, with CoopIII's own invented
1000 rounds - `CreateDeadPedWeaponPickups` has no `CharCreatedBy` gate at all,
and the dying player's own machine skips the whole thing because there the ped
is `FindPlayerPed()`. `KillRemotePed` calls `CPed::SetDie`, and
`CPed::ProcessControl` at `0x004CAF6C` takes a `PED_DIE` ped with
`!bIsPedDieAnimPlaying` straight to `SetDead` (the `call` at `0x004CAFB2`). So
every death has been putting a gun on the pavement of every machine except the
one that mattered, with an ammo count CoopIII made up.

### 10.3 Who decides, and why a drop is not derived from something already on the wire

The owner of the ped. For an ambient pedestrian that is its host
(`population.md` §1.1); for a player's ped it is that player, and the engine
already agrees - nobody drops their own player's weapons.

A drop could not be *derived* on the observers even for the deterministic
half, and it is worth being explicit about why, because the deterministic
finding in §10.1 looks like it should have made the packet unnecessary.
`AmbientPedState` is 24 bytes and carries `netId`, `animId`, `vehicleNetId`,
`seat`, `pos` and `heading`. It does not carry the ped's inventory, its ammo,
its health or the fact that it died - all of them left out on purpose, with
the reasoning in `protocol.h`. An observer therefore has neither the input to
the deterministic function nor the trigger to run it. Adding an inventory and
a health to every ped row, at twelve rows per packet and 10 Hz, to avoid one
reliable packet per death, is the wrong trade by two orders of magnitude.

So a drop rides a new packet.

### 10.4 The wire

`PROTOCOL_VERSION` **14**. Two of the ten opcodes `0x86`-`0x8F` the pickup
work reserved for exactly this:

| opcode | name | to | body |
|---|---|---|---|
| `0x86` | `C_PickupDrop` | server | `PickupDropBody` |
| `0x87` | `S_PickupDrop` | everyone **but** the owner | `playerId` + `PickupDropBody` |

```
struct PickupDropBody {   // 20 bytes
    PickupIdent ident;    // 16: pos, modelIndex, type, flags
    uint16_t    quantity; // CPickup::m_nQuantity - money, or rounds
    uint16_t    pad;
};
```

Reliable, `CH_EVENT`, **one packet per pickup**. Not batched, and that is a
decision: the worst case is 18 money pickups plus 13 weapons from one death
and it is rare, so a batch would save about 500 bytes on an event that happens
a few times a minute, at the cost of a count, a bound and a truncation rule on
a path where truncation means a pickup that exists on one machine and not the
others. Each drop is arbitrated independently by §4's exchange anyway, so
there is nothing to keep together.

`quantity` is the one field `PickupIdent` has no room for and the only one
that cannot be worked out anywhere else.

**Read back, not predicted.** The owner stamps every slot's `(m_eType,
m_nIndex)` before calling the engine's creator and diffs afterwards, and sends
what appeared. So nothing re-derives a scatter angle, a ground height, a money
roll or an ammo cap - all four were decided once, by the engine, and this is a
transcription of the result. It is the same "detect, do not decide" the
collection half already uses.

The generation counter is in the stamp and not just the type, and that is
load-bearing: `GenerateNewOne` does not only fill *free* slots. When `[0,320)`
is full it scans for a `PICKUP_MONEY` to overwrite (`0x0043051D`) and then for
a `PICKUP_ONCE_TIMEOUT` (`0x0043053D`) - which are exactly the two types a
drop makes. A type-only diff would miss every drop made under pressure, i.e.
precisely when drops are happening fastest.

**No server table and no backfill**, and that is measured rather than lazy.
§10.1's 20 s and 30 s expiries mean every copy of a drop dies on its own clock
within half a minute of being born, so the only thing a late joiner could be
told about is something that will be gone before they finish loading. The
server relays and keeps nothing. The moment somebody claims a drop it enters
`m_pickups` like any other key, because the ident is a position and a model
and has never cared who made the pickup.

### 10.5 Suppression

`DecideDrop(replica, haveSession, charCreatedBy)` in `game/pickup.h`, pure so
`clienttest` walks the truth table:

- **REFUSE** - a ped CoopIII built as somebody else's. Their machine decides,
  and for a player's ped that means nothing at all, which is what their own
  engine already does. The `CreateDeadPedWeaponPickups` arm still calls
  `CPed::ClearWeapons` by hand, because that is the tail of the function being
  refused and skipping it would leave a corpse holding an inventory the engine
  meant to empty.
- **ANNOUNCE** - `CharCreatedBy == RANDOM_CHAR` and there is a session. That
  byte is the whole test and it is the engine's own: `RANDOM_CHAR` is written
  by the population generator and by nothing else, since every ped CoopIII
  builds is deliberately `MISSION_CHAR` (it is what stops the engine reaping
  it) and so is every ped the script creates.
- **LOCAL** - everything else, unchanged. A script ped's drop stays exactly as
  it is today: every machine runs `main.scm`, every machine has its own copy
  of that ped, and announcing one machine's would hand the others a second
  pickup beside the one their own copy will leave later. That is M5's problem
  and this is not the place to half-solve it.

"Is this ped a replica" cannot be answered from the pool - a remote player's
ped, an ambient replica and a script ped are all `CCivilianPed` and all
`MISSION_CHAR`. Only the roster knows, so `pickup.cpp` converts the pointer
with `CPools::GetPedRef` and the client half compares two integers against
`RemotePlayer::poolHandle` and `RemoteAmbientPed::poolHandle`. The ref rather
than the pointer, because a pool slot is reused the moment it is freed.

### 10.6 What could not be confirmed without a running game

1. **That a hosted pedestrian's drop appears on the other screen at all.**
   Everything above is proved against the binary; none of it has been in front
   of GTA III. The log says one line the first time each direction works
   ("one of our pedestrians dropped something", "built our first drop off the
   wire") precisely so a session is not lost to a feature that looks identical
   to one that never installed.
2. **That a remote player's death no longer leaves a gun behind.** This is the
   bug fix and it is the easiest thing in here to watch: kill a remote player
   and look at the ground on the observer's screen.
3. **Two money pickups landing within the ident tolerance.** The scatter is
   1.5 m at a random angle and money drops come in pairs, so two of them can
   be within 0.25 m of each other. `FindSlot` already refuses an ambiguous
   ident and logs it, so the cost is a pickup neither machine can claim. The
   weapon half cannot do this - see the `1.75 * k` argument in §10.1.
4. **Pool pressure.** 320 general slots, 448 script pickups over a campaign,
   and now every machine holds every machine's drops. `GenerateNewOne` evicts
   a money pickup and then a timed-out one before failing, and both are drops,
   so the table degrades by dropping drops - which is the right order. The
   `dropsLost` counter and one log line say when it happens.
5. **Reentrancy, again.** §9.7's mine to explosion to `CreateDeadPedMoney` to
   `GenerateNewOne` path now runs a table stamp and a diff inside a blocked
   `CPickups::Update`. The stamp is a plain copy and the diff reads the same
   table the blocker has only nilled `m_pObject` in, so neither is sensitive
   to the block - but it is the sharpest edge here for the same reason it was
   the sharpest edge there.

---

## 11. The seam that blocked everything when nobody was listening

Found 2026-09-22, in a live game, while smoke-testing §10's detours. Not part
of the drop work; it is a bug in §4's exclusivity seam that the drop work
tripped over on its way past.

`HookedPickupsUpdate` passes straight through when `g_cb.Claim == nullptr`,
and `pickup.h` calls that "no session". **It is not.** The callbacks are wired
once in `dllmain` and the client reconnects for as long as the game runs, so
`Claim` is non-null from the moment CoopIII finishes booting — including every
second before the first successful connect, and forever for anybody who
installed the `.asi` without running a server.

So with the socket down, the seam did what it does: hid every pickup in the
world from the engine and claimed into nothing. A single player with CoopIII
installed and no server **could not pick anything up at all**, which is
exactly what `roadmap.md` §5.6 says must not happen.

The log said so plainly and nobody had read it. From a live single-machine
session, forty seconds after boot:

```
[   55564] pickup: a claim for model 1361 went unanswered for 180 frames and
           is being made again. The event channel is reliable, so this means
           the session is in trouble.
```

That line was added as a diagnostic for a broken server. It fires on an
ordinary offline game, once, and then the world quietly has no pickups in it.

**The fix is one more callback**, `PickupCallbacks::HaveSession`, wired to
`Client::IsConnected()`, and it gates the whole detour body rather than any
one decision. The gates are cleared once on the way out, not every frame, so
nothing is left half-claimed across the transition and nothing is rewritten
sixty times a second. §10's `DecideDrop` asks the same question through the
same function, and `Client::DroppedPickup` refuses to send when the socket is
down so the seam can log "shared" and "single player" as different things
rather than claiming the first and meaning the second.

Two things worth keeping from this:

- **"Wired" and "connected" are different questions and this file asked the
  first while meaning the second.** Every seam with an optional-callback
  passthrough has the same shape. Worth a look at the others.
- **The instrument was already fitted and pointed at the right place.** The
  claim-timeout line exists precisely because a pickup that silently stops
  working is worse than a retry, and it did its job on the first live run. It
  went unread because nothing about an offline single-player session looks
  like a moment to go and read the log. The lesson is not "add a log line";
  it is that a diagnostic written for one failure will describe a different
  one, and the first live run of anything is the moment to read all of it.
