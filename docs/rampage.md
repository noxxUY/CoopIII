# Rampages

What a `KILLFRENZY` pickup does in a session, and why it is almost free.

`roadmap.md` §5.10 is the decision and `pickups.md` §6 is the argument for it.
This file is the investigation: what `CDarkel` actually is in the retail
binary, which half of §5.10 was already working, which half was not, and what
was built to close it.

Every address here was read out of `dumpbin /disasm` of
`reference/bin/gta3.exe` and is quoted at the instruction that proves it.
`client/src/game/addresses.h` carries the same list with the fuller
transcriptions; nothing below rests on `re3`.

---

## 1. Two claims in the roadmap, and only one of them was true

`roadmap.md` said two different things about this:

- the M4 table said **"Rampage sharing ❌ ... nothing starts `CDarkel` on the
  other machines, so a `KILLFRENZY` pickup today starts a rampage for whoever
  reached it"**;
- the M4 checklist and the pickups table said the frenzy **already** starts for
  everybody, and that the only gap is remote players' kills.

The second is the true one, and it is true in the merged code.
`client/src/game/pickup.cpp`'s `TellTheScript` calls
`CPickups::AddToCollectedPickupsArray` for a pickup taken by anybody, so the
collection lands in every machine's own `CPickups::aPickUpsCollected`; every
machine is running `rampage.sc`, which polls `HAS_PICKUP_BEEN_COLLECTED`; and
so every machine's own script calls `CDarkel::StartFrenzy` with the same ten
arguments within a round trip. The weapon, the 120 seconds, the four target
models, `bNeedHeadShot`, the HUD and the "Murder ~1~ Diablos in 120 seconds!"
line are identical everywhere with no packet at all.

The M4 table line was stale. It has been corrected.

**So §5.10's decision survived contact with the binary, and its central claim -
"it costs nothing to build" - survived too, for the half it was about.**

---

## 2. What was actually missing: nobody's kills counted

A rampage is not its start, it is its counter, and the counter is
`CDarkel::KillsNeeded` at **`0x008F1AB8`**. It is decremented in exactly two
places in the image:

| | |
|---|---|
| `CDarkel::RegisterKillByPlayer` | `0x00420F60`, `dec dword [008F1AB8h]` at `0x00421000` |
| `CDarkel::RegisterCarBlownUpByPlayer` | `0x00421070`, the same `dec` at `0x004210B1` |

And `CPed::InflictDamage` only reaches the first of those when the damaging
entity is this machine's own player or this machine's own car:

```
004EAD15  call 004D37D0        CPed::SetDie - the ped is dead here
004EAD1A  call 004A1150        FindPlayerPed()
004EAD1F  cmp esi,eax          damagedBy == our player?
004EAD21  je  004EAD30
004EAD23  test esi,esi         a null damager is "not by player"
004EAD25  je  004EAD50
004EAD27  call 004A10C0        FindPlayerVehicle()
004EAD2C  cmp esi,eax
004EAD2E  jne 004EAD50
004EAD39  call 00420F60        CDarkel::RegisterKillByPlayer
004EAD55  call 00421060        CDarkel::RegisterKillNotByPlayer
```

In a session a pedestrian is hosted by one machine and shot from another:

- on the **host**, `damagedBy` is the replica of the shooter's ped
  (`combat.cpp`, `ApplyRemotePedDamage` passes it deliberately, so the blood
  and the threat entity point at the right player), which is neither
  `FindPlayerPed()` nor `FindPlayerVehicle()` - so the kill goes to
  `RegisterKillNotByPlayer`, which is two instructions and bumps
  `CStats::PeopleKilledByOthers`;
- on the **shooter's** machine `CPed::InflictDamage` returned long before this
  line, because `combat.cpp` turned the hit into a `C_PedDamage` instead.

**A co-op NPC kill counted for nobody.** Four machines then ran four counters
apart from one identical start, each one ended its own rampage on its own
arithmetic, and `rampage.sc` handed out the reward on one machine while
printing RAMPAGE FAILED on another.

---

## 3. What `CDarkel` is

One singleton, all statics, no instance. The anchor is
`CDarkel::ReadStatus` at `0x00420E50`, which is the whole function
`66 A1 B4 CC 95 00 / C3` - `mov ax,[0095CCB4] / ret` - and
`CDarkel::FrenzyOnGoing` at `0x00420E60`,
`cmp word [0095CCB4],1 / sete al / ret`. Everything else falls out of
`StartFrenzy` and `RegisterKillByPlayer` writing and reading them.

| | | |
|---|---|---|
| `Status` | `0x0095CCB4` | uint16. 0 none, 1 ongoing, 2 passed, 3 failed |
| `KillsNeeded` | `0x008F1AB8` | int32, counts **down** |
| `TimeLimit` | `0x00885BAC` | int32 ms, negative means no limit |
| `TimeOfFrenzyStart` | `0x009430D8` | int32, a `CTimer::m_snTimeInMilliseconds` |
| `PreviousTime` | `0x00885B00` | int32 s, drives the clock tick sound |
| `WeaponType` | `0x009430F0` | int32 eWeaponType |
| `ModelToKill` .. `4` | `0x008F2C78`, `0x00885B40`, `0x00885B3C`, `0x00885B34` | int32 |
| `bNeedHeadShot` | `0x0095CDCA` | bool |
| `bStandardSoundAndMessages` | `0x0095CDB6` | bool |
| `bProperKillFrenzy` | `0x0095CD98` | bool |
| `pStartMessage` | `0x008F2C08` | wchar* |
| `RegisteredKills` | `0x006EDBE0` | uint16[200], no bounds check anywhere |

| function | address | callers |
|---|---|---|
| `Update` | `0x00420660` | 1 - `0x0048C90E`, `CGame::Process` |
| `DrawMessages` | `0x00420920` | the HUD |
| `ReadStatus` | `0x00420E50` | **1** - `0x00442BE8`, script opcode `01FA` |
| `FrenzyOnGoing` | `0x00420E60` | 4 - twice in `CanBePickedUp`, `CHud::Draw`, one more |
| `ResetOnPlayerDeath` | `0x00420E70` | 3, in the player-death path |
| `RegisterKillByPlayer` | `0x00420F60` | **6** - `CPed::InflictDamage` and five others |
| `RegisterKillNotByPlayer` | `0x00421060` | 1 |
| `RegisterCarBlownUpByPlayer` | `0x00421070` | 2 |
| `StartFrenzy` | `0x004210E0` | **2**, both of them script opcodes |
| `ResetModelsKilledByPlayer` | `0x00421310` | the loop that proves the array is 200 long |

Two of those caller counts are the whole design.

**`StartFrenzy` has two callers and both are the script** (`0x00442BD2` behind
`01F9 init_rampage`, `0x0044B9FE` behind `0367 init_headshot_rampage`).
Nothing else in GTA III starts a frenzy, so a detour there is a complete
account of "a rampage began here" - and, incidentally, it refutes §5.10's
aside that a kill-target multiplier "needs the script intercepted, i.e. M5".
The target is the third argument of that function.

**`ReadStatus` has one caller and it is script opcode `01FA`.** The HUD does
not go through it (`CHud::Draw` reaches `FrenzyOnGoing` at `0x005062CE`),
`CanBePickedUp` does not (`0x00430E99`, `0x00431543`), and `Update` reads the
global itself. So `ReadStatus` is not "the status" - it is *the script's* view
of the status, and it is the only seam in the game where "what this machine's
engine decided" and "what the session decided" can be told apart without
CoopIII owning any part of a rampage.

---

## 4. The design

**Kills travel. Nothing else does.**

1. **The start is still free.** Every machine's own `rampage.sc` starts the
   same frenzy off its own `aPickUpsCollected`, exactly as §5.10 said.
2. **A kill is reported from a detour on `CDarkel::RegisterKillByPlayer`**, and
   the report condition is the engine's own counter: run the original, and if
   `KillsNeeded` moved, this kill counted. Not a copy of the engine's test - the
   engine's test. That covers the five weapon aliases at `0x00420F7C`, the four
   model ids, `bNeedHeadShot`, and the five callers of that function that are
   not `InflictDamage` (a car, a fire, a blast).
3. **What goes on the wire is the engine's own three arguments**: the victim's
   model index, the weapon, and the headshot bit. Not a netId - a player 200 m
   away has no replica of that pedestrian and must still be able to judge the
   kill, and the model index is the only thing about the victim the
   qualification test reads (`movsx eax,word [ebp+5Ch]` at `0x00420FCA`).
4. **Receivers judge it themselves**, against their own `CDarkel`, with
   `RampageKillCounts` in `client/src/game/darkel.h` - a line-by-line
   transcription of `0x00420F7C`..`0x00420FF9`.
5. **The hole in §2 is closed by putting the kill through the engine's own
   register.** When a machine applies a hit off the wire and its own engine
   kills the pedestrian, `combat.cpp` calls
   `CDarkel::RegisterKillByPlayer` itself - which goes through the detour in
   (2), which is what puts it on the wire. One path for every rampage kill in
   the session.
6. **The ending is arbitrated.** Each machine's `CDarkel` keeps running
   retail's own `Update`: its own countdown, its own tick, its own weapon
   restore, its own HUD. The first machine to leave `ONGOING` reports what it
   reached; the server keeps the first report and tells everybody. Until the
   verdict arrives, the detour on `ReadStatus` tells `rampage.sc` `ONGOING`, so
   every machine's script leaves its wait loop on the same value.
7. **A verdict that arrives while our own frenzy is still running is applied by
   moving the engine's own inputs**, not by writing `Status`: `KillsNeeded = 0`
   for a pass, `TimeLimit = 0` and `TimeOfFrenzyStart = now` for a failure. The
   engine's own `Update` then produces the ending on the next frame, with the
   weapon restore, the sound and `m_AllRandomPedsThisType` that writing
   `Status` would have skipped. Same rule as the fires and the pickups:
   replicate the cause, let the engine produce the effect.

### The four questions

**Is a rampage one shared objective or one per player?** One, shared, and it is
not configurable, because the engine settles it: `CDarkel` is a singleton with
one kill count, one clock and one HUD, and retail's own `CanBePickedUp` already
refuses a second `KILLFRENZY` pickup while one is running. Per-player rampages
are a reimplementation of `CDarkel`, which is what §5.10 said and what the
disassembly confirms.

What *is* configurable is the thing players can reasonably disagree about, and
§5.10 named it out loud: four players sharing one 20-kill target in two minutes
is trivial. `RampageMode = shared | scaled | off` in `CoopIII-Server.ini`, two
bits of `SessionFlags`, built the way `ammoSync` and the wanted rule are built.
`shared` is the default and is §5.10 exactly. `scaled` multiplies the kill
target by the number of players. `off` keeps every machine counting its own,
which is what this build did before.

**Who owns the timer?** Nobody needs to own the countdown, because it is the
HUD and the HUD is per machine; what has to be owned is the *verdict*, and the
server owns that. The first ending reported wins and the rest are dropped, so
clock drift between two machines can only decide which of them ended a rampage
that was going to end within a round trip anyway - it can never produce two
different endings, because no machine's script acts on its own ending until the
session has one. The server also holds a backstop deadline on its own
monotonic clock (`Session::ExpireRampage`, the time limit plus one second),
which is what answers the player who paused - `CTimer` stops in the menu - and
the player who started a rampage and then disconnected.

**Whose kills count?** Everybody's, and they ride a fact one machine already
has rather than a new decision. The one machine that knows a pedestrian died
*and* what killed it is the machine hosting that pedestrian, which is also the
machine whose `CDarkel` the engine would have credited in single player. So the
report is made from inside the engine's own kill register on that machine, and
the two directions that used to lose the kill - the host crediting a replica,
the shooter having returned early - both end at the same call.

**A player who joins mid-rampage?** He is backfilled with the `KILLFRENZY`
collection like any other pickup, so his own `rampage.sc` starts a fresh
120-second frenzy and says so. The server answers *every* start report, not
only the first, with the open frenzy's id, the kills still wanted and how long
it has been running; two stores - `KillsNeeded` and `TimeOfFrenzyStart` - put
him into the session's rampage with the right number and the right clock.
Without that answer his script would sit in its wait loop with `$ONMISSION`
set, which is the failure this arrangement exists to avoid.

**The player who started it disconnects?** Nothing happens, because the frenzy
is the session's and not his: the server holds the id, the count, the deadline
and the verdict. If he was the last one running it, the server's own deadline
ends it (as a failure, which is what the engine would have done).

---

## 5. On the wire

Six opcodes out of the `0x88`-`0x8F` block, and none of them is sent unless a
frenzy is running.

| | |
|---|---|
| `C_RampageStart` `0x88` | "my script started a frenzy, limit T, target K" |
| `S_RampageOpen` `0x89` | the frenzy's id, the kills still wanted, how long it has run |
| `C_RampageKill` `0x8A` | one kill: model, weapon, headshot |
| `S_RampageKill` `0x8B` | the same, to everybody but the reporter |
| `C_RampageEnd` `0x8C` | "my own `CDarkel` ended it, this way" |
| `S_RampageEnd` `0x8D` | the session's verdict, to everybody |

The last two, `C_RampageCar` `0x8E` and `S_RampageCar` `0x8F`, are the cars
in §9. The feature is `PROTOCOL_VERSION` 25; the history comment in
`protocol.h` says why the number had to move even though an old client against
a new server plays the same session it always did. The car pair has no number
yet.

---

## 6. The one derivation, and why it is safe

`headShot` is a stack local of `CPed::InflictDamage`, cleared at `0x004EA448`
and set in exactly one place, `0x004EA7F7`, inside the bullet arm's
`PEDPIECE_HEAD` case - the same case that calls `RemoveBodyPart(PED_HEAD)`.
A kill made *on this machine* carries it as the third argument of
`RegisterKillByPlayer`, so the detour reads it rather than deriving it.

The one place it has to be derived is §4 (5): a hit that arrived off the wire
carries `ePedPieceTypes`, not the engine's conclusion. The limb only comes off
when `dontRemoveLimb` is false, and for the pistol, the uzi and the shotgun
that is a `CGeneral::GetRandomNumber()` roll two machines would not agree on.
It does not matter here: all three headshot rampages in `rampage.sc` (07, 19
and 20) are started with `0367` and use SNIPERRIFLE, SNIPERRIFLE and M16, and
those two weapons take the arm where the roll is skipped entirely. For them,
and only for them, `headshot` is exactly `pedPiece == PEDPIECE_HEAD`.

---

## 7. What is not built

**Vehicle rampages** were left out of this round and are built now, but not
the way this section said they would be. See §9.

**The stats screen.** A kill credited off the wire moves `KillsNeeded` and
`RegisteredKills[model]` and nothing else: `CStats::PeopleKilledByPlayer` means
"by the player at this keyboard" and `CStats::PedsKilledOfThisType` indexes off
the victim's `m_nPedType`, which a machine with no ped cannot read. In the
other direction - a host putting a remote player's kill through the register -
those statistics *do* move, and `PeopleKilledByOthers` has already counted the
same death. Two numbers on a stats screen, against a rampage that works.

---

## 8. Testing it without the game

`tools/clienttest` covers the qualification test against every branch of
`0x00420F7C`..`0x00420FF9` - the five weapon aliases, the four model slots,
`FRENZY_ANY_PED`, `FRENZY_ANY_CAR` refusing a pedestrian, and the headshot
rule - plus the client's own handling of the three inbound packets and the
`SessionFlags` bits not colliding with the wanted rule or ammo sync.

`tools/sessiontest` covers what the server holds: one frenzy at a time, every
machine's own start being answered with it, a joiner being answered with the
kills that are actually left and the seconds already gone, a straggling kill
for a closed frenzy being dropped rather than counted against the next one,
the first ending winning when two machines disagree, the backstop deadline and
its grace, a rampage with no time limit never timing out, and the scaled
target's arithmetic including the clamp.

`tools/configtest` covers the new `rampages` key and that its default is
`shared`, which is the decision.

---

## 9. Cars

### 9.1 The car register is not the kill register

Read out of the file, since the listing loses sync on the padding at
`0x00421067`:

```
00421070  cmp word [0095CCB4],1         Status == ONGOING
00421078  push ebx / mov ebx,[esp+8]    the car, one argument
0042107D  jne 004210C0                  not ongoing: straight to the stats
0042107F  cmp [008F2C78],-2             FRENZY_ANY_CAR
00421088  movsx eax,word [ebx+5Ch]      else the model against the four slots
004210B1  dec [008F1AB8]                KillsNeeded--
004210BB  call 0057CC20                 PlayFrontEndSound(5Dh)
004210C4  inc word [eax*2+006EDBE0]     RegisteredKills[model], always
004210CC  inc [00941288]                CStats::CarsExploded, always
004210D3  ret
```

So far it's the kill register without the weapon test and the headshot. The
callers are what's different. A scan for `E8` rel32 to `0x00421070` finds two:
`0x0053BF04` inside `CAutomobile::BlowUpCar`, and `0x0054A04F` in
`CHeli::UpdateHelis`. The first is reached on every call to `BlowUpCar` that
gets past `bCanBeDamaged`, and there is no culprit test anywhere in that
function; the culprit is only pushed on to `CFireManager::StartFire` and
`CExplosion::AddExplosion`. `CBoat::BlowUpCar` never calls it, so boats don't
count toward a car rampage, here or in single player.

Two things follow, and §7 had the first one wrong:

- A car counts on a machine when *that machine's copy* blows up, whoever blew
  it up. That includes the copies CoopIII blows up because the car's owner
  said so (`BlowUpRemoteVehicle`, `BlowUpCarAsOwnerSaid`,
  `WreckUnownedVehicle`). Before this change a vehicle rampage counted every
  wreck on your screen, anybody's, and missed every wreck on a car your machine
  didn't have. It didn't count "only your own".
- Doing what the kill does - count here, relay to the others - would count
  every car twice on every machine that held a copy of it.

The same goes for the people inside. `BlowUpCar` registers the driver and each
passenger with `RegisterKillByPlayer` at `0x0053BDB6` and `0x0053BE17`, again
with no culprit test, so since version 25 the occupants of a replayed wreck were
counted and reported by every machine that had the car.

Rampage 02 is 13 cars, not 20: `$RAMPAGE_02_KILLS = 13` (`rampage.sc` line
76), rocket launcher, 120 seconds. The other car rampages are 06 (10 cars,
grenades), 09 (8, shotgun), 10 (15, M16) and 16 (15, rockets). None of them
cares which weapon did it, because the register never reads one.

### 9.2 What's built

A detour on `RegisterCarBlownUpByPlayer`, and every wreck is sorted before the
original runs (`TallyCarWreck` in `game/darkel.h`):

- **A replay** is run with the rampage branch kept out: `Status` reads NONE for
  the length of the call, so the engine takes the jump at `0x0042107D` and
  still bumps `RegisteredKills` and `CarsExploded`. The occupants' kills get
  the same treatment in the kill detour.
- **A wreck this machine's engine decided** is counted by the engine and, if
  the counter moved, sent as `C_RampageCar`. The driver decides his own car,
  the host decides its traffic, and whoever's engine gets there decides a
  parked car or a session car nobody is driving.
- **Those last two** can go up on several machines at once, each copy in its
  own copy of the same blast, so they carry the `UnownedVehicleKey` that
  `C_UnownedBlowUp` already names them by. The server relays the first report
  of a key and drops the rest (`Session::NoteRampageCar`), and each client
  counts a key once per frenzy whether its own engine or the wire got there
  first (`CountedCars`).

A car off the wire moves `KillsNeeded` and plays `5Dh`, and that's all. Not
`RegisteredKills` or `CarsExploded`: those move when a car blows up on this
machine, and a car this machine held already moved them when its copy was
replayed.

If the car detour doesn't install, that machine ignores cars off the wire and
keeps counting its own copies, replays included, which is what it did before.
`rampages = off` does the same for the whole session.

### 9.3 Who is credited

For the rampage, the session, and it doesn't matter who did it because the
engine doesn't look. The machine that reports a car is the one that decided the
wreck, which is often not the player who caused it: B shoots A's traffic, A's
engine blows it up and A reports it. The only question the register asks is
whether a car went up, so that's enough.

The report on protocol 28 said a traffic car the shooter destroys credits
nobody in `CDarkel`. It credited every machine that had the car: the host's
engine counted it and the shooter's replay counted it again. Now it's once on
every machine, including the ones that never had a copy.

### 9.4 Money doesn't travel

`CPlayerInfo::AwardMoneyForExplosion` (`0x004A15F0`) is the only money the
engine pays for a car: `nMonetaryValue * 0.002`, repeated for each car blown up
within six seconds of the last. It has two callers and they disagree about who
earned it.

- The fire timer in `CAutomobile::ProcessControl` (`0x0053479B`) pays with no
  test at all. Whoever's engine watches a car burn out gets the money, whoever
  lit it.
- `CVehicle::ProcessDelayedExplosion` (`0x00551D6C`) pays only when
  `m_pBlowUpEntity` is `FindPlayerPed()` and it isn't the player's own car.

In a session that works out like this, and this change leaves it alone:

| | who's paid |
|---|---|
| a car burns out after gunfire | every machine whose copy burned: the host for its traffic, the driver for his car, everybody who saw a parked car burn |
| a rocket or grenade kills somebody else's traffic | nobody: on the host the culprit is a replica, and the shooter's replica can't blow up by itself |
| a rocket kills a parked car | the shooter, on his own copy; the others' copies blame a replica |

Money should not cross machines here, at least not as part of this. The
reasons:

- There is no fact to deliver. For the rampage the engine asks whether a car
  went up. For money the common path asks nothing, and the other one asks "am I
  the culprit". Sending "player N destroyed this car" to N's machine means
  CoopIII picking which of those rules applies to whom, and then taking back
  the host's own fire-timer payment to match. That's an economy rule, not a
  relay.
- Nothing in CoopIII writes one player's money because of something another
  machine did. Pickups aren't replayed for their reward, garages keep theirs,
  and each player's money is his own progress. The first packet that moves
  money between machines is the same kind of decision as sharing inventory,
  which belongs in a server setting.
- If it's wanted it should be built as that setting, the way `-ammosync` and
  `rampages` are: a `SessionFlags` bit, a key in `CoopIII-Server.ini`, off by
  default, and a detour on `AwardMoneyForExplosion` plus its two callers to
  decide who's paid. Half of it would be worse than none.

### 9.5 What's still open

- **Before the self-claim branch merges**, the local player's own claimed car
  has no Observed row on his machine. If he bails out and it burns, his copy
  goes up unkeyed while everybody else's is keyed `UNOWNED_SESSION`, and if two
  copies go up on their own it counts twice. With that branch merged both sides
  name it the same way.
- **The first round trip of a frenzy.** A machine that hasn't had
  `S_RampageOpen` yet sends nothing, same as for kills, and keeps its replays
  off its own counter. A car in the first 100 ms is lost.
- **The police helicopter** goes through the same register from
  `CHeli::UpdateHelis` and is counted as an unkeyed car. If it ever becomes one
  shared object whose destruction is replayed, that replay has to run while
  `ReplayingVehicleBlast()` is true or it will count on every machine.
- **A machine whose own `CDarkel` already ended** (its player died) stops
  counting and reporting, while the others keep their replays off their
  counters. The verdict follows within a round trip, so it's a car or two at
  the very end.

`tools/clienttest` covers the car test against `0x0042107F`..`0x004210AA`,
the tally decision, `CountedCars`, two machines taking a car in every order the
wreck, the replay and the relay can arrive in, and the client's handling of
`S_RampageCar`. `tools/sessiontest` covers the server's per-key count, stale
frenzies and the key table filling up.
