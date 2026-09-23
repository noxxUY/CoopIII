# The wanted level and the police

`roadmap.md` §5.1 settled the direction — per-player, GTA Online style,
server-configurable — and nothing was ever built. This is the investigation and
the design, and it corrects §5.1 in two places.

Everything below that says "measured" was read out of
`reference/bin/gta3.exe` (v1.0 retail, MD5 `85414BF9EB414D00AD81062360F0DB1F`,
image base `0x400000`, not relocated). re3 said where to look; the addresses
and the constants are the binary's.

---

## 1. What GTA Online actually does

Asked for, so researched rather than assumed. The sourcing is thinner than it
should be and the gaps are named, because a design that quietly invents a rule
and attributes it to Rockstar is worse than one that admits it chose.

**Per player.** The GTA Wiki's *Wanted Level in GTA Online* article describes
the level as a property of a player, and evasion in per-player terms. One
player can have four stars while the player beside them has none; that is why
the article has to document the cases where a level *does* spread. If it were
shared by default those notes would be redundant.
<https://gta.wiki/w/Wanted_Level_in_GTA_Online> (mirror of the Fandom article,
which would not serve; the wording matched the search engine's snippets of the
Fandom URL verbatim).

**A shared vehicle shares the heat, and this is the behaviour noxx named.**
The same article states that the wanted level of *the occupant with the highest
one* is applied to all occupants. It lists both directions as wanted-level
causing actions: letting another player into your vehicle while you are wanted,
and getting into a wanted player's vehicle. Same source.

Three things about that rule the sources do **not** settle, flagged here
because the design has to pick and should be honest that it is picking:

- **Driver or passenger.** The article says "occupants" and draws no
  distinction. Nothing found says the driver is special.
- **One star or the full count.** The two bullets sit under the article's
  *One Star* heading, while the sentence between them says the highest level
  wins. Those cannot both be right. The reading taken here is that the
  bullets are filed under *One Star* as the minimum case and the sentence is
  the general rule — but that is a reading, not a citation.
- **What happens when you get out.** Not addressed anywhere. §4.4 picks.

**Proximity alone does nothing.** No source documents standing near a wanted
player transferring stars. What *is* documented, and is easy to mistake for it,
is that attacking another player within sight of police gives you a star — your
own action being witnessed, not somebody else's heat. Argument from silence,
labelled as such.

**Losing them is individual.** Evade for a time that grows with the star count,
or die, or pay Lester. There is no Busted in GTA Online — the police shoot
rather than arrest, even at one star. Same source.

**Group clears exist, but only as scripted features**, not as a property of
being in a group: a VIP/CEO's *Bribe Authorities* ($15,000) clears the level
for every member of the organisation, the *Phantom Wedge* grants three stars to
every member of an organisation or MC, and *Nine Tenths of the Law* clears the
whole MC when one member drives clear. Free-roam membership of a crew, an
organisation or an MC does not on its own pool anybody's stars.

**Not obtained.** Reddit and GTAForums were both unreachable, so there is no
player-anecdote layer behind any of this. Rockstar's own pages say nothing
mechanical. Treat the two flagged ambiguities above as genuinely open, and if
somebody with the game in front of them contradicts this file, believe them
over it.

---

## 2. What the engine allows, measured

This is the half that decides the design, and it is much more constraining than
§5.1 knew.

### 2.1 There is one `CWanted` and it belongs to the local player

`CPlayerPed::m_pWanted` is at `+0x53C` on the ped — the first member past
`sizeof(CPed)`, which is `0x53C`. Three independent witnesses in the image,
all reached from script opcodes through the dispatcher at `0x00439500`
(opcodes 269..272 are in the 200..299 range, handler `0x0043D530`, base opcode
**214**, table `0x005EEC40`, 85 entries):

```
0043E0EC  mov ecx,[eax+53Ch]       IS_WANTED_LEVEL_GREATER (271)
0043E0F7  mov edx,[ecx+18h]        m_nWantedLevel
004F3194  mov ecx,[ecx+53Ch]       CPlayerPed::SetWantedLevel
004F31B4  mov ecx,[ecx+53Ch]       CPlayerPed::SetWantedLevelNoDrop
004F4A76  mov eax,[eax+53Ch]       CPopulation::AddToPopulation
```

`roadmap.md` §2.3 is right about the consequence and wrong about the name: the
task brief and §5.1 both call it `CPlayerInfo::m_pWanted`, and it is not. It
hangs off the *ped*, and `CWorld::Players[0].m_pPed` is the only ped in the
process that has one, because a `CCivilianPed` is exactly `0x53C` bytes and
there is no room for it. **Every remote player in CoopIII is a `CCivilianPed`.
There is nowhere to put a second wanted level and no engine code that would
read one.**

The class, from `CWanted::UpdateWantedLevel` (`0x004AD900`), which writes four
of its members in one pass:

| Field | Offset | Witness |
|---|---|---|
| `m_nChaos` | `+0x00` | `mov [ecx],eax` at `0x004AD90F` |
| `m_nLastUpdateTime` | `+0x04` | re3 declaration order, unconfirmed here |
| `m_nLastWantedLevelChange` | `+0x08` | `mov [ecx+8],eax` from `CTimer::m_snTimeInMilliseconds` on a level change |
| `m_CurrentCops` | `+0x10` | `mov cl,[edi+10h]` in `GenerateOneRandomCar` |
| `m_MaxCops` | `+0x11` | `mov byte [ecx+11h],0Ah` at six stars |
| `m_MaximumLawEnforcerVehicles` | `+0x12` | `mov byte [ecx+12h],3` at six stars |
| `m_RoadblockDensity` | `+0x14` | `mov word [ecx+14h],0Ch` — a word, not a byte |
| `m_nWantedLevel` | `+0x18` | `mov dword [ecx+18h],6` |

### 2.2 The engine will not let you lose stars by running away

This is the single biggest difference from GTA Online and it is not a
CoopIII limitation, it is GTA III. `CWanted::Update` decays `m_nChaos` by one
per second **only while `m_nWantedLevel <= 1`, and only when no police are
within 18 m**. At two stars and above nothing decays at all.

The brackets, read out of `UpdateWantedLevel`:

```
chaos >= 3200 (0xC80) -> 6 stars, 10 cops, 3 cars, roadblocks 12
chaos >= 1600 (0x640) -> 5 stars,  8 cops, 3 cars, roadblocks 10
chaos >=  800 (0x320) -> 4 stars,  6 cops, 2 cars, roadblocks  8
chaos >=  400 (0x190) -> 3 stars,  4 cops, 2 cars, roadblocks  4
chaos >=  200 (0x0C8) -> 2 stars,  3 cops, 2 cars, roadblocks  0
chaos >=   40 (0x028) -> 1 star,   1 cop,  1 car,  roadblocks  0
otherwise             -> 0 stars
```

So in Liberty City a three-star level ends when you die, get busted, take a
bribe pickup, or use a Pay'n'Spray, and in no other way. There are no search
cones and there is nothing to evade. Any design that leans on "the stars
eventually go away on their own" is leaning on a GTA V mechanic that does not
exist here.

### 2.3 Nothing can pursue anybody but the local player

`CCopPed` reaches for `FindPlayerPed()` nineteen times. `SetPursuit` registers
itself in `FindPlayerPed()->m_pWanted->m_pCops`; `ProcessControl` picks its
target as `FindPlayerVehicle() ? that : FindPlayerPed()`; the decision to give
up reads the same wanted level. There is no "chase this ped" path in the class
at all.

Both police *generators* read the same one wanted level:

```
CPopulation::AddToPopulation  0x004F4A00     cops on foot
  004F4A71  call FindPlayerPed / [eax+53Ch] / [eax+18h] / cmp 2 / jle
  004F4A90  movzx eax,[esi+11h]  m_MaxCops
            cmp [00885AFC],eax   CPopulation::ms_nNumCop
  004F4AA0  cmp byte [ecx+314h],0 / jne        and only while on foot
  004F4AAD  movzx eax,[esi+12h]  m_MaximumLawEnforcerVehicles

CCarCtrl::GenerateOneRandomCar 0x004165F0     police cars
  00416746  PlayerInFocus / Players / [+53Ch] / [+18h]
  00416762  cmp eax,1 / jle                    needs more than one star
  00416767  movzx eax,[edi+12h] / cmp [008F1B38],eax   NumLawEnforcerCars
  00416773  m_CurrentCops < m_MaxCops
  004167AD  call 0x004181F0                    pick the model, class 0x11
```

Put together: **a machine's police are a function of that machine's own wanted
level, and they chase that machine's own player.** There is no version of this
feature in which one process drives police against somebody else's ped.

### 2.4 Crimes are already attributed correctly, for free

This is the fact that makes the whole feature small, and it was the one worth
checking first because if it had gone the other way the feature would have been
enormous.

`CEventList::RegisterEvent` ends with a single gate before it reports anything
to the wanted system:

```
00475DD1  call 004A1150            FindPlayerPed()
00475DD6  cmp  [esp+28h],eax       the `criminal` argument
00475DDA  jne  00475DF0            not the player: report nothing
00475DE8  call 00476070            CEventList::ReportCrimeForEvent
```

`ReportCrimeForEvent` (`0x00476070`) is the only caller of
`CWanted::RegisterCrime` (`0x004AD9F0`) and
`CWanted::RegisterCrime_Immediately` (`0x004ADA10`) outside the script.

So when M3 replays a remote player's shot through `CWeapon::Fire` on this
machine, the `criminal` is the *remote* ped and this machine reports no crime.
The local player's stars do not move because somebody else shot a pedestrian in
front of you. Nobody had to write that; it falls out of the engine being
written for one player.

The same gate is why the converse holds: your own crimes raise only your own
stars, on your own machine, from your own engine's judgement of whether a cop
was close enough to see it (`CWanted::WorkOutPolicePresence`, `0x004ADD00`,
14 m at the crime and 18 m for the decay).

---

## 3. Where §5.1 is wrong

Two corrections, both worth making before anything is built on it.

**§5.1 has the cost backwards.** It says "that makes `perplayer` more work than
`shared`. It is still the right default." The opposite is true. `perplayer` is
what the engine already does: every machine runs its own `CWanted`, generates
its own police from it, attributes only its own player's crimes to it, and
(since `population.md`) hands those police to every other machine as ordinary
ambient replicas. The per-player mode is close to free. It is **`shared` that
is the work**, because a level that is held by more than one machine needs a
rule for coming back down that no single machine can decide alone, and §4.5 is
most of this document's difficulty.

**§5.1 says police AI "has to be driven from" CoopIII's own state.** It does
not, and it must not. §2.3 is the measurement: `CCopPed` cannot pursue anything
but `FindPlayerPed()`, so driving it from CoopIII state would mean
reimplementing the class. What actually happens is the reverse — CoopIII drives
one number into the engine's own `CWanted` and the engine does all of it.

The direction §5.1 settled — per-player by default, GTA Online's vehicle rule,
a server switch — is right, and §1 backs it. The table stands. Everything else
in that section is superseded by this file.

---

## 4. The design

### 4.1 Authority: a player's stars belong to their own machine

Non-negotiable, and it is the host-authoritative rule (`roadmap.md` §5)
specialised to a value with exactly one possible owner. Every input to a
player's wanted level happens inside that player's process: the crimes they
commit, whether a cop was close enough to see, the bribe pickup they took, the
Pay'n'Spray they drove through, the death, the bust. No other machine holds any
of it, and §2.4 means no other machine is even *offered* any of it.

So: **an observer never decides a remote player's stars.** It is told the
number, and the only thing it may do with it is decide what its *own* player's
stars should be. That is the same shape as health (M4) and as fire
(`roadmap.md` §5.7): the owner decides, the wire carries the result, the
observer reacts.

### 4.2 The police are already done

This is the finding, and it is the reason this feature is a few hundred lines
rather than a milestone.

A police pedestrian is created by `CPopulation::AddPed` as a `CCopPed`, and
`AddPed` never touches `CharCreatedBy`, so it stays at the `CPed` constructor's
default of `RANDOM_CHAR`. A police car is created by
`CCarCtrl::GenerateOneRandomCar` as a `RANDOM_VEHICLE`. Both therefore already
pass `IsAmbientPedWeShouldHost` and `IsAmbientCarWeShouldHost` in
`client/src/game/population.cpp`, unchanged, with no test added and no case
made for them. **The police have been replicated since `population.md` step 4
shipped.** Nobody noticed because no machine has ever had a reason to make one.

**"`AddPed` never touches `CharCreatedBy`" is an absence, and it was measured
rather than read off re3.** That claim is the load-bearing one in this section
and re3 has been wrong about a retail fact six times on this project, so it
does not get to be the source for it. An absence is checkable by looking for
the instruction that would contradict it: every `mov`/`alu byte [reg+0x160],
imm8` and `mov byte [reg+0x160], r8` with a `disp32` in the whole of `.text`,
seventy-seven of them. **Not one is a write inside `CPopulation`'s address
range or `CCarCtrl`'s.** The two hits that are in those modules are `cmp byte
[ecx+0x160], 1` at `0x004F347B` and the same compare at `0x0041968F` — each
asking "is this ambient?", which is the question this design is relying on
them getting the right answer to. The only write either module can reach is
the constructor's own, `mov byte [eax+0x160], 1` at `0x004C42EB` inside
`CPed::CPed`, and every other immediate write in the image is a script opcode
stamping `MISSION_CHAR`, one of the two handlers that give a mission ped back
to the city, or the replay system.

So a `CCopPed` leaves `AddPed` carrying exactly what an ambient civilian
leaves it carrying, and nothing between there and `CWorld::Add` tells the two
apart. The live two-machine measurement in `population.md` §1.3.1 — eleven
pedestrians shared rather than twenty-two — therefore covers the police too,
and it covers them without a second measurement and without a line of code.

So the whole of "how do the other players see the chase" is: the wanted
player's own engine spawns the police, the police chase that player because
that is the only thing they can do, and every other machine receives them as
ambient replicas that are written to and decide nothing. The observer sees four
police cars and a Kuruma being chased through Portland, and its own engine
contributed no part of that except the drawing.

Two consequences worth stating rather than discovering:

**A cop replica is a `CCivilianPed` wearing a cop's model.**
`SpawnAmbientReplica` constructs `CCivilianPed` and falls back to
`PEDTYPE_CIVMALE` for any ped type that is not CIVMALE or CIVFEMALE, which
`PEDTYPE_COP` (6) is not. That is the right outcome and not a compromise: a
real `CCopPed` replica would run `CCopPed::ProcessControl` on the observer's
machine, register itself in *that* machine's `m_pCops`, and start its own
pursuit of a player who has done nothing. The replica has to be the class that
decides nothing, and `CCivilianPed` is that class. What it costs is that the
replica lands in `ms_nNumCivMale` rather than `ms_nNumCop`, which matters in
exactly one place and only in shared mode — §6.1.

**Nothing about the police is on the wire.** No police packet, no cop netId
allocated by this feature, no pursuit state. Every byte of it is already paid
for by the traffic and pedestrian streams.

### 4.3 What travels

One number and one bit, in spare bits of a byte that is already being sent.
`PlayerStateBody::flags` has four bits free after `PF_ON_FIRE`:

- **bits 4-6, `PF_WANTED_MASK`**: this player's wanted level, 0..6. Three bits
  hold 0..7 and the engine's ceiling is 6 (`CWanted::MaximumWantedLevel`,
  `0x005F7714`), so 7 is spare and is treated as 6 on the way in rather than
  trusted.
- **bit 7, `PF_WANTED_BORROWED`**: this level is the session's, not mine. Only
  read in `shared` mode, and §4.5 is what it is for.

It rides the unreliable 25 Hz snapshot, for the same reason `PF_ON_FIRE` does:
it is a state with a lifetime, not an event, and a dropped packet is corrected
40 ms later by the next one. The worst a lost update can do is delay somebody
else's stars by a frame and a half.

The session's rule goes the other way, in `S_Welcome::flags` bits 2-3, beside
`SESSION_FRIENDLY_FIRE` and `SESSION_AMMO_SYNC`. Same place, same reason: it is
a rule the client has to enforce locally because the server cannot see the
thing being governed.

**No struct grew and no layout moved.** The meaning of two existing bytes
changed, which is still a wire change, so `PROTOCOL_VERSION` moves: the merged
build is **18**, one number for the ten branches that landed together.

The bits were checked against the other trees rather than assumed free, and one
of those checks turned out to matter. `PlayerFlags` bits 4-7 really were free
and still are. `SessionFlags` was not: this work wrote the rule as `3 << 1`
while the ammunition work took bit 1 for `SESSION_AMMO_SYNC`, so a session with
ammo sync on would also have announced its wanted rule as `shared`. The rule
moved to bits 2-3 at the merge — neither number had shipped, and moving the
two-bit field was cheaper than moving the flag. This feature still allocates
**no opcode at all**, which is the other thing worth saying out loud when
several people are cutting into the same enum.

### 4.4 `perplayer`: the vehicle rule, and a borrowed star becomes yours

Every machine, every send tick, computes a **floor** — the lowest wanted level
its own player is entitled to — and raises its own engine to it if it is short.
In `perplayer` the floor is the highest level reported by any other player in
the same car:

```
floor = max over remote players p where p.seatVehicleNetId == our car's netId
        of p.wantedLevel
```

"Our car" is `m_localVehicleNetId` when driving and `m_localSeatNetId` when
riding; on foot, or in a car the session has never heard of, there are no
co-occupants and the floor is zero. `seatVehicleNetId` is used rather than
`seatedVehicleNetId` on purpose: it is the session's standing instruction
(`AGENTS.md`, "written as a reconciliation"), so a car-mate whose ped has not
finished streaming in here is still in the car as far as this rule is
concerned, which is what stops the stars flickering while a model loads.

**No driver/passenger distinction**, because §1 found none and because the
engine gives us none to hang one on.

**A borrowed star becomes yours.** Once the floor has raised this machine's
level, that level is this player's own and behaves exactly like one they
earned: getting out of the car does not take it away, and the only things that
end it are the four things §2.2 lists. Two reasons, and the second is the
stronger one:

- §1 could not establish what GTA Online does on exit, so this is a choice
  rather than a copy, and it should be made on GTA III's terms.
- A level that evaporated when you opened the door would put you on the
  pavement in front of four police cars with no stars, and the engine would
  then have those cars give up and drive away mid-chase. The alternative is
  not "more faithful", it is "visibly broken".

A consequence that reads like a bug and is not: after A has earned four stars
and B has ridden with them, B keeps four stars after A dies. B is genuinely
wanted, B's own engine is genuinely generating police for B, and a third player
getting into B's car is correctly given four stars by the same rule. The heat
propagates through the session the way it propagates through a car, and it is
ended the way GTA III ends it — one player at a time, by dying, being busted,
a bribe, or a spray.

### 4.5 `shared`: the session's level, and the rule for coming down

`shared` is the mode noxx asked for as the always-on alternative, and it is
where the work is. The naive rule — everyone takes the session maximum — has a
deadlock in it that is worth writing down because it is not obvious and it
cannot be tested for in the game without losing a session to it:

> A earns four stars. B is raised to four. A dies and clears to zero. A now
> reports zero, but B is still reporting four, so A is immediately raised back
> to four by B. B is at four only because A was. Neither can ever get out, and
> the session is at four stars until everybody happens to die within the same
> 40 ms.

The fix is the `PF_WANTED_BORROWED` bit. In `shared` the floor is the maximum
over players who are **not** borrowing:

```
floor = max over remote players p where !p.borrowed of p.wantedLevel
```

and a machine that is borrowing follows the floor **down** as well as up, while
a machine holding a level it earned never follows it down at all.

Trace the deadlock again with the bit: A earns four and reports `(4, own)`,
because its engine got there without CoopIII writing anything. B's floor is
four, B writes four, B reports `(4, borrowed)`. A's floor is now the maximum
over non-borrowers, which is nobody, so zero — A keeps its own four. A dies:
A's engine drops to zero by itself, A reports `(0, own)`. B's floor is zero, B
is borrowing, B writes zero. Session clear.

And the case that must *not* drop: A and B each independently earn three. Both
report `(3, own)`, because neither one's level was ever written by CoopIII.
Each one's floor is three, which equals what they already hold, so nothing is
written and neither is marked borrowed. A dies; A's floor is B's three and A is
raised straight back to three, which is what "the whole session shares the
highest level" means and is the mode working rather than failing.

### 4.6 The whole decision, as arithmetic

All of the above is one pure function of four inputs and it lives in
`client/src/game/wanted.h` as `PlanWanted`, deliberately with no engine
dependency at all, so `tools/clienttest` can put the traces above through it
without a running game. Per send tick:

```
engine  = the engine's current m_nWantedLevel
applied = the level CoopIII last left it at
own     = the level this player reached without help

if (engine != applied)        own = engine       the engine moved by itself:
                                                 a crime, a death, a bust,
                                                 a bribe, a Pay'n'Spray
floor  = Floor(rule, roster)
target = rule == Off ? 0 : max(own, floor)
if (target != engine)         Write(target)
if (rule == PerPlayer)        own = target       a borrowed star becomes yours
applied = target
report(level = target, borrowed = target > own)
```

Two details in there are load-bearing:

**`if (target != engine)` is what makes the feature invisible to a player who
is alone with their stars.** `CWanted::SetWantedLevel` (`0x004ADA50`) resets
`m_nChaos` to the bottom of the bracket — 820 for four stars — and calls
`ClearQdCrimes`. Writing it every tick would throw away a player's accumulated
chaos forty times a second and stop them ever reaching the next star. So
CoopIII writes only when it disagrees with the engine, which in `perplayer`
with nobody in your car is never.

**`own` is only re-read from the engine when the engine has moved on its own.**
That is the whole of the earned-versus-granted bookkeeping, and it is one
comparison rather than a hook: if the level is exactly where CoopIII left it,
CoopIII learned nothing this tick and must not overwrite what it knows.

The write goes through `CPlayerPed::SetWantedLevel` (`0x004F3190`), the
function `ALTER_WANTED_LEVEL` (269) and `CLEAR_WANTED_LEVEL` (272) both call,
rather than through `CWanted::SetWantedLevel` directly — one fewer offset
dereferenced at the call site, and it is an entry point the game uses itself.
`SetWantedLevelNoDrop` is not used: the arithmetic above already never lowers
anybody in `perplayer`, so the raise-only variant would be a second way of
saying the same thing and a second thing to keep in step.

### 4.7 `off`

`target` is zero unconditionally, so the engine is clamped to zero whenever it
is not already there. Honest about what this is: it is a clamp, not a
police-free Liberty City. A crime still registers, the level still goes up for
up to one send tick, and it is taken away again. Whether cops that had already
started a pursuit break off cleanly is **untested** —
`CWanted::ResetPolicePursuit` is what the engine uses for that and this does
not call it. If `off` turns out to leave cops standing around, that call is
where to look.

### 4.8 What the server does, and what it cannot

The server sends the rule in `S_Welcome` and nothing else. It does not compute
a session maximum, because every client already receives every other client's
snapshot and can do that arithmetic itself; adding a server-side copy would be
a second answer to the same question, arriving later.

It also cannot *enforce* any of this, and that is worth stating plainly rather
than leaving as a gap. Friendly fire is enforceable because the damage passes
through the server and can be refused. A wanted level does not pass through
anything — it lives in a client's engine, and the only thing a client reports
is a number. A client that lied could raise other players' stars. The blast
radius of that lie is exactly the blast radius of getting into their car and
shooting a policeman, which any client can already do honestly, so there is
nothing here worth a trust mechanism.

---

## 5. Addresses this needs

Four, and the feature needs nothing else from the engine.

| Symbol | Address / offset | Proof |
|---|---|---|
| `FindPlayerPed` | `0x004A1150` | already in `addresses.h` |
| `CPlayerPed::m_pWanted` | `+0x53C` | §2.1, four witnesses |
| `CWanted::m_nWantedLevel` | `+0x18` | §2.1, read by both generators and by opcode 271, written by `UpdateWantedLevel` |
| `CPlayerPed::SetWantedLevel(int32)` | `0x004F3190` | jump-table entry 55 of `0x005EEC40` (`ALTER_WANTED_LEVEL`, 269) and entry 58 (`CLEAR_WANTED_LEVEL`, 272) passing a literal 0 |

Recorded but not used, because they were established on the way and the next
person should not have to dig them out again: `CWanted::SetWantedLevel`
`0x004ADA50`, `SetWantedLevelNoDrop` `0x004ADAC0`, `UpdateWantedLevel`
`0x004AD900`, `RegisterCrime` `0x004AD9F0`, `RegisterCrime_Immediately`
`0x004ADA10`, `SetMaximumWantedLevel` `0x004ADAE0`, `WorkOutPolicePresence`
`0x004ADD00`, `ClearQdCrimes` `0x004ADF20`, `CWanted::MaximumWantedLevel`
`0x005F7714`, `nMaximumWantedLevel` `0x005F7718`,
`CEventList::ReportCrimeForEvent` `0x00476070`,
`CRunningScript::ProcessCommands200To299` `0x0043D530` (base opcode **214**,
table `0x005EEC40`, 85 entries).

---

## 6. What this costs, honestly

### 6.1 Shared mode can crowd the streets, and the gate is named

In `perplayer` only the wanted player's engine generates police, so the city
holds one player's worth of them and everybody else holds replicas. The counts
are self-limiting for exactly the reason `population.md` §1.3.2 gives: a police
car replica is created as `RANDOM_VEHICLE`, `CCarCtrl::UpdateCarCount` reads
`bIsLawEnforcer` off the model and counts it in `NumLawEnforcerCars`
(`0x008F1B38`), and that is the counter `GenerateOneRandomCar` compares against
`m_MaximumLawEnforcerVehicles`. Observers stop making police cars because
somebody else's are already there.

In `shared` every machine is wanted, so every machine generates. The car half
is still self-limiting. **The foot half is not**, and the gate is exactly one
line of `AddToPopulation`:

```
004F4A90  movzx eax,[esi+11h]           m_MaxCops
          cmp dword [00885AFC],eax      CPopulation::ms_nNumCop
```

A cop replica is a `CCivilianPed` (§4.2), so `CPed::CPed` counts it in
`ms_nNumCivMale` and this gate never sees it. With N players at four stars each
machine will happily make its own six cops on foot on top of everybody else's.
That is up to N × 6 in the union rather than 6.

This is **not measured** and it is not fixed. It is named here with the gate it
comes from so that whoever hits it starts from a line of disassembly rather
than a search — which is what `population.md` §1.3 did before §1.3.1 measured
it away. The tempting fix, giving the replica `PEDTYPE_COP` so it lands in
`ms_nNumCop`, is not obviously safe: the ped type is read by `CEventList`, by
`CPed::InflictDamage` and by the audio, and a ped that says it is a cop while
carrying a `CCivilianPed` vtable is a new kind of lie. Measure it in a two-client
session at four stars before writing anything.

### 6.2 The observer's police are a step behind

A remote player's police arrive over the ambient streams, which run at 10 Hz
and cover the eight cars and pedestrians nearest *the sender* — not nearest the
observer (`population.md` §2.1). A four-star chase is four police cars and up
to six cops on foot, all within a few metres of the player being chased, so the
"nearest the sender" ordering happens to be exactly right here. But it is luck
rather than design, and a chase crossing paths with heavy traffic will push
somebody out of the eight.

### 6.3 Nothing is drawn for a remote player's stars

There is no indication anywhere on an observer's screen that another player is
wanted, beyond the police physically being there. No blip colour, no nametag
change. That is deliberate scope, not an oversight: the wire carries the
number, and `radar.cpp` and `nametag.cpp` can read it whenever somebody decides
what it should look like.

### 6.4 The two GTA Online rules this does not implement

Both from §1, both left out on purpose:

- **Attacking a player in sight of police gives you a star.** GTA III's own
  `CEventList` already does this for any ped it can see you attack, and a
  remote player's ped is an ordinary `CPed` to the local engine — so shooting
  one in front of a cop should already register `CRIME_SHOOT_PED` through the
  normal path. Untested, and it would be worth one line in a live session.
- **Group clears.** Bribe Authorities has no equivalent here and nothing in
  Liberty City to hang it on.

---

## 7. What an in-game run has to confirm

None of this is testable headless past `PlanWanted`, which is why it is
arithmetic in a header. In rough order of how likely each is to be the thing
that is wrong:

1. **That `perplayer` does nothing at all to a lone player.** Start a session,
   shoot a policeman, and check the stars climb and behave exactly as they do
   in single player. If CoopIII is writing the level when it should not be, the
   symptom is stars that stick at a bracket floor and never climb — §4.6's
   first load-bearing detail failing.
2. **That the other player sees the police.** Two clients, one commits a crime,
   and the other should see police cars and cops converge on them without their
   own stars moving. This is §4.2 and it is the whole feature; if it does not
   work, the ambient replication is the suspect and not this file.
3. **That getting into a wanted player's car gives you their stars**, at the
   full count, within a send tick. Both directions: their car and yours.
4. **That the stars stay when you get out** (§4.4).
5. **That `shared` comes back down.** Run the deadlock trace deliberately: A
   earns four, B is raised, A dies, and B must clear within a tick or two. This
   is the one case that cannot be reached by playing normally and has to be
   staged.
6. **That `off` is quiet.** Whether the HUD flickers a star on each crime, and
   whether cops already in pursuit stop (§4.7).
7. **That a replayed shot does not raise the observer's stars.** §2.4 says it
   cannot, from the disassembly. Stand next to a player who is shooting
   pedestrians in front of a policeman and watch your own HUD stay empty.
